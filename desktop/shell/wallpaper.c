/* nexus-wallpaper — le fond du bureau : fond d'écran + icônes du bureau
 * ("Ce PC", "Mes fichiers", "Corbeille", puis le contenu du dossier
 * ~/Bureau). Double-clic pour ouvrir ; clic droit : menu (Ouvrir, Copier,
 * Couper, Coller, Propriétés...). Le presse-papiers de fichiers est partagé
 * avec le gestionnaire de fichiers.
 * Relit la configuration quand il reçoit SIGUSR1 (envoyé par Paramètres). */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

struct dicon { const char *label, *icon, *cmd; };
static const struct dicon sys_icons[] = {
    { "Ce PC", "display", "nexus-files /" },
    { "Mes fichiers", "folder", "nexus-files" },
    { "Corbeille", "trash", "nexus-files \"$HOME/.local/share/Trash/files\"" },
};
#define NSYS ((int)(sizeof sys_icons / sizeof sys_icons[0]))

/* Contenu de ~/Bureau */
struct dfile { char name[256]; bool dir; };
#define MAXF 120
static struct dfile files[MAXF];
static int nfiles;
static char bureau[PATH_MAX];

/* Taille des icônes selon le réglage « Icônes » (petites, moyennes, grandes) */
#define ISZ    (58 * nx_icon_factor())
#define ICON_W (ISZ + 52)
#define ICON_H (ISZ + 50)

static struct nx_window *win;
static cairo_surface_t *cache;          /* fond d'écran déjà dessiné */
static char cache_spec[512];
static int cache_w, cache_h;
static int hover = -1, selected = -1, last_click = -1;
static double last_time;

/* Menu contextuel */
enum act { A_OPEN, A_COPY, A_CUT, A_PASTE, A_PROPS, A_NEWDIR, A_REFRESH, A_PERSO, A_DISPLAY };
struct mitem { const char *label; enum act act; };
static struct mitem menu[10];
static int nmenu, menu_hover = -1, menu_target = -1;
static bool menu_open;
static bool menu_pressed;   /* un clic a commencé DANS le menu (pas celui qui l'a ouvert) */
static double menu_x, menu_y;

/* Fenêtre « Propriétés » (dessinée sur le bureau) */
static bool props_open;
static int props_target;
static char props_lines[8][2][300];
static int nprops;

static int nicons(void) { return NSYS + nfiles; }
static void path_of(int i, char *out, size_t n) { snprintf(out, n, "%s/%s", bureau, files[i - NSYS].name); }

static const char *ext_of(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && d != n ? d + 1 : "";
}
static bool is_image(const char *n)
{
    const char *e = ext_of(n);
    return !strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg");
}
static const char *file_icon(const struct dfile *f)
{
    if (f->dir) return "folder";
    if (is_image(f->name)) return "image";
    static const char *txt[] = { "txt", "md", "conf", "sh", "c", "h", "py", "json", "html", "log", "csv" };
    for (unsigned i = 0; i < sizeof txt / sizeof txt[0]; i++)
        if (!strcasecmp(ext_of(f->name), txt[i])) return "editor";
    return "file";
}

static int cmp_file(const void *a, const void *b)
{
    const struct dfile *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* Relit ~/Bureau ; renvoie true si son contenu a changé */
static bool scan(void)
{
    struct dfile nw[MAXF];
    int n = 0;
    DIR *d = opendir(bureau);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && n < MAXF) {
            if (de->d_name[0] == '.') continue;
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", bureau, de->d_name);
            struct stat st;
            if (stat(p, &st) != 0) continue;
            snprintf(nw[n].name, sizeof nw[n].name, "%s", de->d_name);
            nw[n].dir = S_ISDIR(st.st_mode);
            n++;
        }
        closedir(d);
    }
    qsort(nw, n, sizeof nw[0], cmp_file);
    bool changed = n != nfiles || memcmp(nw, files, n * sizeof nw[0]);
    memcpy(files, nw, n * sizeof nw[0]);
    nfiles = n;
    if (changed && selected >= nicons()) selected = -1;
    return changed;
}

/* Icônes en colonnes, de haut en bas puis de gauche à droite */
static void icon_pos(int i, double *x, double *y)
{
    int h = win ? win->height : 800;
    int per_col = (int)((h - 32 - nx_panel_height()) / (ICON_H + 8));
    if (per_col < 1) per_col = 1;
    *x = 20 + (i / per_col) * (ICON_W + 8);
    *y = 20 + (i % per_col) * (ICON_H + 8);
}

static void label_of(int i, const char **label, const char **icon)
{
    if (i < NSYS) { *label = sys_icons[i].label; *icon = sys_icons[i].icon; }
    else { *label = files[i - NSYS].name; *icon = file_icon(&files[i - NSYS]); }
}

/* ---------- Propriétés ---------- */

static void human(char *out, size_t n, double b)
{
    const char *u[] = { "octets", "Ko", "Mo", "Go", "To" };
    int k = 0;
    while (b >= 1024 && k < 4) { b /= 1024; k++; }
    if (k == 0) snprintf(out, n, "%.0f %s", b, u[k]);
    else snprintf(out, n, "%.1f %s", b, u[k]);
}

/* Taille d'un dossier (récursif, limité pour rester rapide) */
static void du(const char *p, double *bytes, long *nf, long *nd, long *budget)
{
    DIR *d = opendir(p);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && *budget > 0) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char q[PATH_MAX];
        snprintf(q, sizeof q, "%s/%s", p, de->d_name);
        struct stat st;
        if (lstat(q, &st) != 0) continue;
        (*budget)--;
        if (S_ISDIR(st.st_mode)) { (*nd)++; du(q, bytes, nf, nd, budget); }
        else { (*nf)++; *bytes += st.st_size; }
    }
    closedir(d);
}

static void prop(const char *k, const char *v)
{
    if (nprops >= 8) return;
    snprintf(props_lines[nprops][0], sizeof props_lines[nprops][0], "%s", k);
    snprintf(props_lines[nprops][1], sizeof props_lines[nprops][1], "%s", v);
    nprops++;
}

static void read_os(char *out, size_t n)
{
    snprintf(out, n, "Nexus OS");
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "PRETTY_NAME=", 12)) {
            char *v = line + 12;
            v[strcspn(v, "\n")] = 0;
            if (*v == '"') { v++; v[strcspn(v, "\"")] = 0; }
            snprintf(out, n, "%s", v);
        }
    fclose(f);
}

static void fill_props(int i)
{
    nprops = 0;
    props_target = i;
    char v[300], a[64], b[64];
    const char *home = getenv("HOME") ? getenv("HOME") : "/";
    if (i == 0) {   /* Ce PC */
        read_os(v, sizeof v);
        prop("Système", v);
        struct statvfs s;
        const char *disk = access("/data", F_OK) == 0 ? "/data" : "/";
        if (statvfs(disk, &s) == 0) {
            double tot = (double)s.f_blocks * s.f_frsize, fr = (double)s.f_bavail * s.f_frsize;
            human(a, sizeof a, tot - fr); human(b, sizeof b, tot);
            snprintf(v, sizeof v, "%s utilisés sur %s", a, b);
            prop("Disque", v);
            human(a, sizeof a, fr);
            prop("Espace libre", a);
        }
        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
            long kb = 0;
            if (fscanf(f, "MemTotal: %ld kB", &kb) == 1) { human(a, sizeof a, kb * 1024.0); prop("Mémoire", a); }
            fclose(f);
        }
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        snprintf(v, sizeof v, "%ld", cpus);
        prop("Processeurs", v);
        return;
    }
    char p[PATH_MAX];
    if (i == 1) snprintf(p, sizeof p, "%s", home);
    else if (i == 2) snprintf(p, sizeof p, "%s/.local/share/Trash/files", home);
    else path_of(i, p, sizeof p);
    const char *label, *icon;
    label_of(i, &label, &icon);
    prop("Nom", label);
    struct stat st;
    if (stat(p, &st) != 0) { prop("Erreur", "élément introuvable"); return; }
    bool dir = S_ISDIR(st.st_mode);
    prop("Type", dir ? "Dossier" : is_image(label) ? "Image" : !strcmp(icon, "editor") ? "Texte" : "Fichier");
    char loc[PATH_MAX];
    snprintf(loc, sizeof loc, "%s", p);
    char *sl = strrchr(loc, '/');
    if (sl && sl != loc) *sl = 0;
    prop("Emplacement", i < NSYS ? p : loc);
    if (dir) {
        double bytes = 0;
        long nf = 0, nd = 0, budget = 50000;
        du(p, &bytes, &nf, &nd, &budget);
        human(a, sizeof a, bytes);
        snprintf(v, sizeof v, "%s%s", a, budget <= 0 ? " (au moins)" : "");
        prop("Taille", v);
        snprintf(v, sizeof v, "%ld fichier(s), %ld dossier(s)", nf, nd);
        prop("Contenu", v);
    } else {
        human(a, sizeof a, st.st_size);
        prop("Taille", a);
    }
    struct tm tm;
    localtime_r(&st.st_mtime, &tm);
    strftime(v, sizeof v, "%d/%m/%Y à %H:%M", &tm);
    prop("Modifié le", v);
}

#define PW 460
static void props_box(int width, int height, double *x, double *y, double *h)
{
    *h = 96 + nprops * 30 + 64;
    *x = (width - PW) / 2.0;
    *y = (height - *h) / 2.0;
}

/* ---------- Actions ---------- */

static void open_icon(int i)
{
    if (i < 0) return;
    if (i < NSYS) { nx_spawn(sys_icons[i].cmd); return; }
    char p[PATH_MAX], q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 40];
    path_of(i, p, sizeof p);
    nx_shell_quote(q, sizeof q, p);
    snprintf(cmd, sizeof cmd, "nexus-files --open %s", q);
    nx_spawn(cmd);
}

static void run(enum act a)
{
    menu_open = false;
    int i = menu_target;
    char p[PATH_MAX], q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 64];
    switch (a) {
    case A_OPEN: open_icon(i); break;
    case A_COPY: case A_CUT: {
        if (i < 0) break;
        if (i == 1) snprintf(p, sizeof p, "%s", getenv("HOME") ? getenv("HOME") : "/");
        else path_of(i, p, sizeof p);
        char *paths[1] = { p };
        nx_fileclip_set(a == A_CUT, paths, 1);
        break;
    }
    case A_PASTE:
        nx_shell_quote(q, sizeof q, bureau);
        snprintf(cmd, sizeof cmd, "nexus-files --paste %s", q);
        if (system(cmd) != 0) { /* message déjà affiché par Fichiers */ }
        scan();
        break;
    case A_NEWDIR: {
        for (int k = 1; k < 100; k++) {
            if (k == 1) snprintf(p, sizeof p, "%s/Nouveau dossier", bureau);
            else snprintf(p, sizeof p, "%s/Nouveau dossier (%d)", bureau, k);
            if (mkdir(p, 0755) == 0) break;
        }
        scan();
        break;
    }
    case A_REFRESH: scan(); break;
    case A_PROPS: fill_props(i); props_open = true; break;
    case A_PERSO: nx_spawn("nexus-settings --wallpaper"); break;
    case A_DISPLAY: nx_spawn("nexus-settings --display"); break;
    }
    nx_redraw(win);
}

static void add_menu(const char *label, enum act a)
{
    if (nmenu < 10) menu[nmenu++] = (struct mitem){ label, a };
}

static void open_menu(int i, double x, double y)
{
    nmenu = 0;
    menu_target = i;
    if (i >= 0) {
        add_menu("Ouvrir", A_OPEN);
        if (i == 1 || i >= NSYS) {
            add_menu("Copier", A_COPY);
            if (i >= NSYS) add_menu("Couper", A_CUT);
        }
        add_menu("Propriétés", A_PROPS);
    } else {
        char **paths;
        bool cut;
        int n = nx_fileclip_get(&cut, &paths);
        nx_fileclip_free(paths, n);
        if (n) add_menu("Coller", A_PASTE);
        add_menu("Nouveau dossier", A_NEWDIR);
        add_menu("Actualiser", A_REFRESH);
        add_menu("Personnaliser (fond d'écran)", A_PERSO);
        add_menu("Propriétés de l'affichage", A_DISPLAY);
    }
    menu_x = x;
    menu_y = y;
    menu_hover = -1;
    menu_open = true;
    menu_pressed = false;
    hover = -1;   /* plus de surbrillance au survol tant que le menu est ouvert */
}

#define MW 250
#define MI 36
static void menu_box(double *x, double *y, double *h)
{
    *h = nmenu * MI + 12;
    *x = menu_x + MW > win->width - 4 ? win->width - MW - 4 : menu_x;
    double bottom = win->height - nx_panel_height() - 12;   /* au-dessus de la barre des tâches */
    *y = menu_y + *h > bottom ? bottom - *h : menu_y;
}

/* ---------- Dessin ---------- */

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    /* Le paysage n'est recalculé que si la taille ou le fond changent */
    if (nx_image_stale(cache) || cache_w != width || cache_h != height || strcmp(cache_spec, nx_conf.wallpaper)) {
        if (cache) cairo_surface_destroy(cache);
        cache = nx_image_surface(CAIRO_FORMAT_RGB24, width, height);
        cairo_t *c = cairo_create(cache);
        nx_draw_wallpaper(c, nx_conf.wallpaper, width, height);
        cairo_destroy(c);
        cache_w = width; cache_h = height;
        snprintf(cache_spec, sizeof cache_spec, "%s", nx_conf.wallpaper);
    }
    cairo_set_source_surface(cr, cache, 0, 0);
    cairo_paint(cr);
    nx_color accent = nx_accent_color();
    if (nx_conf.desktop_icons) {
        for (int i = 0; i < nicons(); i++) {
            double x, y;
            icon_pos(i, &x, &y);
            if (x + ICON_W > width) break;
            if (i == selected || i == hover) {
                nx_rounded_rect(cr, x, y, ICON_W, ICON_H, 16);
                nx_set(cr, nx_alpha(nx_mix(accent, nx_rgb("#0b1230"), 0.55), i == selected ? 0.75 : 0.45));
                cairo_fill_preserve(cr);
                nx_set(cr, nx_alpha(accent, i == selected ? 0.9 : 0.45));
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
            }
            const char *label, *icon;
            label_of(i, &label, &icon);
            nx_icon(cr, icon, x + (ICON_W - ISZ) / 2.0, y + 8, ISZ, nx_rgb("#ffffff"));
            /* texte blanc avec ombre portée pour rester lisible sur tous les fonds */
            double ts = nx_scale;
            nx_scale = ts > 1.1 ? 1.1 : ts;   /* l'étiquette doit tenir sous l'icône */
            nx_text(cr, label, 14.5, true, nx_alpha(nx_rgb("#000000"), 0.6),
                    x + 1, y + ISZ + 15, ICON_W, 26, NX_CENTER);
            nx_text(cr, label, 14.5, true, nx_rgb("#ffffff"), x, y + ISZ + 14, ICON_W, 26, NX_CENTER);
            nx_scale = ts;
        }
    }

    if (menu_open) {
        double mx, my, mh;
        menu_box(&mx, &my, &mh);
        nx_glass(cr, mx, my, MW, mh, 12);
        nx_color text = nx_rgb(NX_GLASS_TEXT);
        for (int i = 0; i < nmenu; i++) {
            double iy = my + 6 + i * MI;
            if (i == menu_hover) {
                nx_rounded_rect(cr, mx + 6, iy, MW - 12, MI, 8);
                nx_set(cr, accent);
                cairo_fill(cr);
            }
            nx_text(cr, menu[i].label, 13.5, false, i == menu_hover ? nx_rgb("#ffffff") : text,
                    mx + 18, iy, MW - 30, MI, NX_LEFT);
        }
    }

    if (props_open) {
        double px, py, ph;
        props_box(width, height, &px, &py, &ph);
        for (int i = 6; i > 0; i--) {
            nx_rounded_rect(cr, px - i, py - i + 4, PW + 2 * i, ph + 2 * i, 16 + i);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.05);
            cairo_fill(cr);
        }
        nx_rounded_rect(cr, px, py, PW, ph, 16);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill(cr);
        const char *label, *icon;
        label_of(props_target, &label, &icon);
        nx_icon(cr, icon, px + 24, py + 22, 44, accent);
        nx_text(cr, "Propriétés", 12.5, false, nx_rgb(NX_SUBTLE), px + 84, py + 20, PW - 110, 20, NX_LEFT);
        nx_text(cr, label, 17, true, nx_rgb(NX_TEXT), px + 84, py + 40, PW - 110, 28, NX_LEFT);
        for (int i = 0; i < nprops; i++) {
            double y = py + 90 + i * 30;
            nx_text(cr, props_lines[i][0], 13, false, nx_rgb(NX_SUBTLE), px + 24, y, 130, 26, NX_LEFT);
            nx_text(cr, props_lines[i][1], 13, true, nx_rgb(NX_TEXT), px + 160, y, PW - 184, 26, NX_LEFT);
        }
        double bx = px + PW - 24 - 110, by = py + ph - 52;
        nx_rounded_rect(cr, bx, by, 110, 36, 9);
        nx_set(cr, accent);
        cairo_fill(cr);
        nx_text(cr, "Fermer", 13.5, true, nx_rgb("#ffffff"), bx, by, 110, 36, NX_CENTER);
    }
}

static int hit(double px, double py)
{
    if (!nx_conf.desktop_icons) return -1;
    for (int i = 0; i < nicons(); i++) {
        double x, y;
        icon_pos(i, &x, &y);
        if (px >= x && px < x + ICON_W && py >= y && py < y + ICON_H) return i;
    }
    return -1;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    /* Fenêtre Propriétés : seul « Fermer » (ou un clic dehors) agit */
    if (props_open) {
        if (ev == NX_PRESS) {
            double px, py, ph;
            props_box(w->width, w->height, &px, &py, &ph);
            double bx = px + PW - 24 - 110, by = py + ph - 52;
            bool on_btn = x >= bx && x < bx + 110 && y >= by && y < by + 36;
            bool outside = x < px || x >= px + PW || y < py || y >= py + ph;
            if (on_btn || outside) { props_open = false; nx_redraw(w); }
        }
        return;
    }
    if (menu_open) {
        double mx, my, mh;
        menu_box(&mx, &my, &mh);
        bool inside = x >= mx && x < mx + MW && y >= my + 6 && y < my + 6 + nmenu * MI;
        int mi = inside ? (int)((y - my - 6) / MI) : -1;
        if (ev == NX_MOTION && mi != menu_hover) { menu_hover = mi; nx_redraw(w); }
        else if (ev == NX_PRESS && inside) { menu_pressed = true; return; }
        else if (ev == NX_PRESS && !inside && !(x >= mx && x < mx + MW && y >= my && y < my + mh)) {
            menu_open = false;
            nx_redraw(w);
            if (value != BTN_RIGHT) return;   /* un clic droit ailleurs rouvre un menu */
        } else if (ev == NX_RELEASE && menu_pressed && mi >= 0 && mi < nmenu) {
            run(menu[mi].act);
            return;
        } else {
            return;
        }
    }
    int h = hit(x, y);
    if (ev == NX_MOTION && h != hover) { hover = h; nx_redraw(w); }
    else if (ev == NX_LEAVE && hover != -1) { hover = -1; nx_redraw(w); }
    else if (ev == NX_PRESS && value == BTN_LEFT) {
        double t = now_s();
        if (h >= 0 && h == last_click && t - last_time < 0.5) {
            open_icon(h);
            last_click = -1;
        } else {
            last_click = h;
            last_time = t;
        }
        if (selected != h) { selected = h; nx_redraw(w); }
    } else if (ev == NX_PRESS && value == BTN_RIGHT) {
        selected = h;
        open_menu(h, x, y);
        nx_redraw(w);
    }
}

/* ~/Bureau relu toutes les 3 secondes (fichiers ajoutés ailleurs) */
static int timeout(void) { return 3000; }
static void tick(void) { if (scan()) nx_redraw(win); }
static void reload(void) { scan(); }

int wallpaper_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *home = getenv("HOME");
    snprintf(bureau, sizeof bureau, "%s/Bureau", home && *home ? home : "/tmp");
    mkdir(bureau, 0755);
    scan();
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer };
    uint32_t all = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    /* exclusive_zone -1 : couvre tout l'écran, y compris sous la barre des tâches */
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, all, 0, 0, -1,
                          ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE, "nexus-wallpaper");
    if (!win) return 1;
    nx.timeout = timeout;
    nx.tick = tick;
    nx.reload = reload;
    nx_run();
    return 0;
}
