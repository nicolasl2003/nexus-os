/* nexus-settings — l'application Paramètres du bureau Nexus.
 *
 * Sections : Fond d'écran, Couleurs, Barre des tâches, Icônes, Affichage, À propos.
 * Les choix sont enregistrés dans ~/.config/nexus/desktop.conf puis signalés
 * au fond d'écran, à la barre des tâches et au menu (SIGUSR1) : tout se met à
 * jour immédiatement. La couleur d'accent est aussi appliquée aux fenêtres
 * (~/.config/labwc/themerc-override + labwc --reconfigure).
 *
 * Options : --wallpaper, --colors, --panel, --icons, --display, --about
 */
#define _GNU_SOURCE
#include "nexus-secu.h"
#include "nexus-ui.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <grp.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define WIN_W    820
#define WIN_H    640
#define SIDE_W   214
#define PAD      30

enum { SEC_WALL, SEC_COLORS, SEC_THEME, SEC_PANEL, SEC_ICONS, SEC_WIDGETS, SEC_DISPLAY, SEC_LOCK,
       SEC_SECURITY, SEC_PROTECTION, SEC_KEYBOARD, SEC_ACCOUNTS, SEC_ABOUT, SEC_COUNT };
static const struct { const char *label, *icon; } sections[SEC_COUNT] = {
    { "Fond d'écran", "image" },     { "Couleurs", "palette" },
    { "Thème", "display" },          { "Barre et menu", "panel" },    { "Icônes", "apps" },
    { "Widgets", "monitor" },        { "Affichage", "display" },
    { "Verrouillage", "lock" },      { "Sécurité", "lock" },          { "Protection", "disk" },
    { "Clavier", "keyboard" },       { "Comptes", "user" },           { "À propos", "info" },
};
static int section = SEC_WALL;

/* ---------- Zones cliquables (recalculées à chaque dessin) ---------- */

enum kind { K_NAV, K_WALL, K_ACCENT, K_SCALE, K_DSCALE, K_TOGGLE, K_PANEL, K_ISIZE, K_ISTYLE, K_ISHAPE,
            K_OPACITY, K_TINT, K_MENU, K_WSIZE, K_WSTYLE, K_WRESET,
            K_ACC_ADD, K_ACC_PW, K_ACC_DEL, K_DLG_FIELD, K_DLG_OK, K_DLG_CANCEL, K_DLG_WIPE, K_KB, K_THEME, K_LOCKDELAY, K_LOCKNOW,
            K_FW, K_SEC_OK, K_SEC_CANCEL, K_SEC_BG, K_SEC_FIELD, K_BLOCAGE, K_CRYPT, K_JOURNAL };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[96];
static int nrects;
static int hover_kind = -1, hover_index = -1;

static double rect_dy;   /* décalage vertical en cours (cairo_translate d'une section) */
static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 96) rects[nrects++] = (struct rect){ x, y + rect_dy, w, h, k, i };
}
static bool is_hover(enum kind k, int i) { return (int)k == hover_kind && i == hover_index; }

/* ---------- Fonds d'écran proposés ---------- */

struct wall { char spec[512], name[64]; bool swatch; };
static struct wall walls[48];
static int nwalls;

static const struct { const char *hex, *name; } solid[] = {
    { "#1b1f2a", "Ardoise" }, { "#0b1f3f", "Marine" }, { "#1f4d3a", "Sapin" },
    { "#4a2560", "Prune" },   { "#5a3a1e", "Noisette" }, { "#000000", "Noir" },
};

static void scan_images(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nwalls < 18) {
        size_t n = strlen(e->d_name);
        const char *dot = strrchr(e->d_name, '.');
        if (n < 5 || !dot || (strcasecmp(dot, ".png") && strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg")))
            continue;
        struct wall *w = &walls[nwalls++];
        snprintf(w->spec, sizeof w->spec, "%s/%s", dir, e->d_name);
        snprintf(w->name, sizeof w->name, "%.*s", (int)(dot - e->d_name), e->d_name);
        w->swatch = false;
    }
    closedir(d);
}

static void load_walls(void)
{
    for (int i = 0; i < nx_preset_count; i++) {
        struct wall *w = &walls[nwalls++];
        snprintf(w->spec, sizeof w->spec, "preset:%s", nx_presets[i].id);
        snprintf(w->name, sizeof w->name, "%s", nx_presets[i].name);
        w->swatch = false;
    }
    char p[512];
    const char *home = getenv("HOME") ? getenv("HOME") : "/";
    scan_images("/usr/local/share/backgrounds");
    snprintf(p, sizeof p, "%s/Images", home); scan_images(p);
    snprintf(p, sizeof p, "%s/images", home); scan_images(p);
    for (unsigned i = 0; i < sizeof solid / sizeof solid[0]; i++) {
        struct wall *w = &walls[nwalls++];
        snprintf(w->spec, sizeof w->spec, "color:%s", solid[i].hex);
        snprintf(w->name, sizeof w->name, "%s", solid[i].name);
        w->swatch = true;
    }
}

/* ---------- Informations système (À propos) ---------- */

static char info_label[8][32], info_value[8][160];
static int ninfo;
static char pretty_name[128] = "Nexus OS";

static void add_info(const char *label, const char *value)
{
    if (ninfo >= 8 || !value || !value[0]) return;
    snprintf(info_label[ninfo], sizeof info_label[0], "%s", label);
    snprintf(info_value[ninfo], sizeof info_value[0], "%s", value);
    ninfo++;
}

static void read_key(const char *path, const char *key, char sep, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl)) continue;
        char *v = strchr(line, sep);
        if (!v) continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '"') v++;
        v[strcspn(v, "\"\n")] = 0;
        snprintf(out, n, "%s", v);
        break;
    }
    fclose(f);
}

static void load_info(void)
{
    char buf[160] = "";
    read_key("/etc/os-release", "PRETTY_NAME", '=', pretty_name, sizeof pretty_name);
    read_key("/etc/os-release", "VERSION=", '=', buf, sizeof buf);
    add_info("Version", buf);
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(buf, sizeof buf, "Linux %s (%s)", u.release, u.machine);
        add_info("Noyau", buf);
        add_info("Nom de la machine", u.nodename);
    }
    buf[0] = 0;
    read_key("/proc/cpuinfo", "model name", ':', buf, sizeof buf);
    add_info("Processeur", buf);
    char mem[64] = "";
    read_key("/proc/meminfo", "MemTotal", ':', mem, sizeof mem);
    if (mem[0]) {
        snprintf(buf, sizeof buf, "%.0f Mo", atof(mem) / 1024.0);
        add_info("Mémoire", buf);
    }
    int pkgs = 0;
    DIR *d = opendir("/usr/local/var/lib/npk/installed");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) if (e->d_name[0] != '.') pkgs++;
        closedir(d);
    }
    snprintf(buf, sizeof buf, "%d (gestionnaire npk)", pkgs);
    add_info("Paquets installés", buf);
    struct passwd *pw = getpwuid(getuid());
    if (pw) add_info("Compte", pw->pw_name);
}

/* ---------- Application des réglages ---------- */

static struct nx_window *win;

static void apply(void)
{
    nx_config_save(&nx_conf);
    nx_notify_reload();
    nx_redraw(win);
}

static void apply_window_theme(void)
{
    /* Couleur d'accent sur les barres de titre des fenêtres (labwc) */
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    char dir[512], path[600];
    snprintf(dir, sizeof dir, "%s/.config", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.config/labwc", home); mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/themerc-override", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Écrit par Paramètres : couleur d'accent du bureau Nexus\n");
    fprintf(f, "window.active.title.bg.color: %s\n", nx_conf.accent);
    fprintf(f, "window.active.border.color: %s\n", nx_conf.accent);
    fprintf(f, "menu.items.active.bg.color: %s\n", nx_conf.accent);
    fprintf(f, "menu.border.color: %s\n", nx_conf.accent);
    fprintf(f, "osd.border.color: %s\n", nx_conf.accent);
    fprintf(f, "osd.window-switcher.preview.border.color: %s,#ffffff,%s\n", nx_conf.accent, nx_conf.accent);
    bool light = nx_conf.theme == 1;
    fprintf(f, "# Thème %s\n", light ? "clair" : "sombre");
    fprintf(f, "window.inactive.title.bg.color: %s\n", light ? "#e4e8ef" : "#2a2f3b");
    fprintf(f, "window.inactive.border.color: %s\n", light ? "#d0d5de" : "#2a2f3b");
    fprintf(f, "window.inactive.label.text.color: %s\n", light ? "#5d6576" : "#a0a8b8");
    fprintf(f, "window.inactive.button.unpressed.image.color: %s\n", light ? "#5d6576" : "#a0a8b8");
    fprintf(f, "menu.items.bg.color: %s\n", light ? "#f7f8fb" : "#1b1f2a");
    fprintf(f, "menu.items.text.color: %s\n", light ? "#1b1f2a" : "#e6e9ef");
    fprintf(f, "menu.separator.color: %s\n", light ? "#d0d5de" : "#3a4050");
    fprintf(f, "osd.bg.color: %s\n", light ? "#f7f8fb" : "#1b1f2a");
    fprintf(f, "osd.label.text.color: %s\n", light ? "#1b1f2a" : "#e6e9ef");
    fclose(f);
    /* labwc relit sa configuration et son thème quand il reçoit SIGHUP */
    const char *pid = getenv("LABWC_PID");
    if (pid && atoi(pid) > 1) kill(atoi(pid), SIGHUP);
}

/* ---------- Dessin ---------- */

static void section_title(cairo_t *cr, double x, const char *title, const char *sub)
{
    nx_text(cr, title, 24, true, nx_rgb(NX_TEXT), x, 26, 0, 34, NX_LEFT);
    nx_text(cr, sub, 13.5, false, nx_rgb(NX_SUBTLE), x, 62, 0, 22, NX_LEFT);
}

static void draw_check(cairo_t *cr, double cx, double cy, double r, nx_color c)
{
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    nx_set(cr, c);
    cairo_fill(cr);
    cairo_move_to(cr, cx - r * 0.45, cy);
    cairo_line_to(cr, cx - r * 0.1, cy + r * 0.38);
    cairo_line_to(cr, cx + r * 0.5, cy - r * 0.35);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, r * 0.28);
    cairo_stroke(cr);
}

static void draw_wallpapers(cairo_t *cr, double x0, double cw)
{
    nx_color accent = nx_accent_color();
    section_title(cr, x0, "Fond d'écran", "Choisis l'image affichée derrière tes fenêtres.");
    double tw = 168, th = 105, gap = 18;
    int cols = (int)((cw + gap) / (tw + gap));
    if (cols < 1) cols = 1;
    double y = 104;
    int k = 0;
    for (int i = 0; i < nwalls; i++) {
        if (walls[i].swatch) continue;
        double x = x0 + (k % cols) * (tw + gap), ty = y + (k / cols) * (th + 34);
        cairo_save(cr);
        nx_rounded_rect(cr, x, ty, tw, th, 10);
        cairo_clip(cr);
        cairo_translate(cr, x, ty);
        nx_draw_wallpaper_cached(cr, walls[i].spec, tw, th);
        cairo_restore(cr);
        bool sel = !strcmp(nx_conf.wallpaper, walls[i].spec);
        if (sel || is_hover(K_WALL, i)) {
            nx_rounded_rect(cr, x - 3, ty - 3, tw + 6, th + 6, 12);
            nx_set(cr, sel ? accent : nx_alpha(accent, 0.45));
            cairo_set_line_width(cr, 3);
            cairo_stroke(cr);
        }
        if (sel) draw_check(cr, x + tw - 16, ty + 16, 11, accent);
        nx_text(cr, walls[i].name, 12.5, sel, nx_rgb(NX_TEXT), x, ty + th + 6, tw, 22, NX_CENTER);
        add_rect(x, ty, tw, th + 28, K_WALL, i);
        k++;
    }
    /* Couleurs unies */
    double sy = y + ((k + cols - 1) / cols) * (th + 34) + 8;
    nx_text(cr, "Couleur unie", 14, true, nx_rgb(NX_TEXT), x0, sy, 0, 24, NX_LEFT);
    sy += 34;
    int j = 0;
    for (int i = 0; i < nwalls; i++) {
        if (!walls[i].swatch) continue;
        double cx = x0 + 22 + j * 64, cy = sy + 22;
        bool sel = !strcmp(nx_conf.wallpaper, walls[i].spec);
        if (sel || is_hover(K_WALL, i)) {
            cairo_arc(cr, cx, cy, 25, 0, 2 * M_PI);
            nx_set(cr, sel ? accent : nx_alpha(accent, 0.45));
            cairo_set_line_width(cr, 3);
            cairo_stroke(cr);
        }
        cairo_arc(cr, cx, cy, 20, 0, 2 * M_PI);
        nx_set(cr, nx_rgb(walls[i].spec + 6));
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        add_rect(cx - 24, cy - 24, 48, 48, K_WALL, i);
        j++;
    }
}

/* Paragraphe découpé en lignes */
static void para_text(cairo_t *cr, const char *txt, double x, double y, double w)
{
    char line[400] = "", word[96];
    const char *p = txt;
    while (*p) {
        size_t n = strcspn(p, " ");
        snprintf(word, sizeof word, "%.*s", (int)n, p);
        p += n;
        while (*p == ' ') p++;
        char test[500];
        snprintf(test, sizeof test, "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && nx_text_width(cr, test, 12.5, false) > w) {
            nx_text(cr, line, 12.5, false, nx_rgb(NX_SUBTLE), x, y, w, 20, NX_LEFT);
            y += 20;
            snprintf(line, sizeof line, "%s", word);
        } else snprintf(line, sizeof line, "%s", test);
    }
    if (line[0]) nx_text(cr, line, 12.5, false, nx_rgb(NX_SUBTLE), x, y, w, 20, NX_LEFT);
}

/* Carte de choix (bordure d'accent si choisie, coche en haut à droite) */
static void choice_card(cairo_t *cr, double x, double y, double w, double h, enum kind k, int i,
                        bool sel, const char *label)
{
    nx_color accent = nx_accent_color();
    nx_rounded_rect(cr, x, y, w, h, 12);
    nx_set(cr, sel ? nx_alpha(accent, 0.08) : nx_rgb("#f6f7fa"));
    cairo_fill_preserve(cr);
    nx_set(cr, sel ? accent : is_hover(k, i) ? nx_alpha(accent, 0.5) : nx_rgb("#dde1e8"));
    cairo_set_line_width(cr, sel ? 3 : 1.5);
    cairo_stroke(cr);
    if (label) nx_text(cr, label, 13, sel, nx_rgb(NX_TEXT), x, y + h - 30, w, 24, NX_CENTER);
    if (sel) draw_check(cr, x + w - 16, y + 16, 10, accent);
    add_rect(x, y, w, h, k, i);
}

/* Interrupteur sur une ligne (titre + explication) */
static void toggle_row(cairo_t *cr, double x0, double y, double w, enum kind k, int i,
                       const char *title, const char *sub, bool on)
{
    nx_rounded_rect(cr, x0, y, w, 56, 10);
    nx_set(cr, is_hover(k, i) ? nx_rgb("#eef1f6") : nx_rgb("#f6f7fa"));
    cairo_fill(cr);
    nx_text(cr, title, 14, true, nx_rgb(NX_TEXT), x0 + 16, y + 6, w - 100, 24, NX_LEFT);
    nx_text(cr, sub, 12, false, nx_rgb(NX_SUBTLE), x0 + 16, y + 28, w - 100, 20, NX_LEFT);
    double tx = x0 + w - 64, ty = y + 16;
    nx_rounded_rect(cr, tx, ty, 46, 24, 12);
    nx_set(cr, on ? nx_accent_color() : nx_rgb("#c5cbd6"));
    cairo_fill(cr);
    cairo_new_path(cr);
    cairo_arc(cr, on ? tx + 34 : tx + 12, ty + 12, 9, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    add_rect(x0, y, w, 56, k, i);
}

/* Bouton texte (contour d'accent) ; retourne sa largeur */
static double button_at(cairo_t *cr, double x, double y, const char *label, enum kind k, int i)
{
    nx_color a = nx_accent_color();
    double w = nx_text_width(cr, label, 13.5, true) + 36;
    nx_rounded_rect(cr, x, y, w, 38, 19);
    nx_set(cr, is_hover(k, i) ? nx_alpha(a, 0.12) : nx_rgb("#ffffff"));
    cairo_fill_preserve(cr);
    nx_set(cr, a);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    nx_text(cr, label, 13.5, true, a, x, y, w, 38, NX_CENTER);
    add_rect(x, y, w, 38, k, i);
    return w;
}

static void button_row(cairo_t *cr, double x, double y, const char *label, enum kind k)
{
    button_at(cr, x, y, label, k, 0);
}

/* Petite barre des tâches d'aperçu (fond, logo Nexus, quelques applications) */
static void mini_panel(cairo_t *cr, double x, double y, double w, double h)
{
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    nx_glass_source(cr, y, y + h);
    cairo_paint(cr);
    double is = h * 0.56 * nx_icon_factor(), ix = x + h * 0.3;
    nx_logo(cr, ix, y + (h - is) / 2, is);
    static const char *apps[] = { "files", "browser", "store", "terminal", "settings" };
    for (int i = 0; i < 5; i++) {
        ix += h * 1.05;
        if (ix + is > x + w - 4) break;
        nx_app_icon(cr, apps[i], ix, y + (h - is) / 2, is, nx_rgb("#ffffff"));
    }
    cairo_restore(cr);
}

static void draw_colors(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Couleurs",
                  "Couleur d'accent, transparence et teinte de la barre et des menus.");
    /* Couleurs d'accent : deux rangées */
    int per_row = (nx_accent_count + 1) / 2;
    double step = cw / per_row;
    if (step > 84) step = 84;
    for (int i = 0; i < nx_accent_count; i++) {
        double cx = x0 + step / 2 + (i % per_row) * step, cy = 128 + (i / per_row) * 84;
        bool sel = !strcasecmp(nx_conf.accent, nx_accents[i].hex);
        nx_color c = nx_rgb(nx_accents[i].hex);
        if (sel || is_hover(K_ACCENT, i)) {
            cairo_arc(cr, cx, cy, 27, 0, 2 * M_PI);
            nx_set(cr, sel ? c : nx_alpha(c, 0.4));
            cairo_set_line_width(cr, 3);
            cairo_stroke(cr);
        }
        cairo_arc(cr, cx, cy, 21, 0, 2 * M_PI);
        nx_set(cr, c);
        cairo_fill(cr);
        if (sel) draw_check(cr, cx, cy, 10, nx_alpha(nx_rgb("#000000"), 0.25));
        nx_text(cr, nx_accents[i].name, 11.5, sel, nx_rgb(NX_TEXT), cx - step / 2, cy + 28, step, 20, NX_CENTER);
        add_rect(cx - 28, cy - 28, 56, 78, K_ACCENT, i);
    }

    /* Transparence de la barre et des menus */
    double y = 296;
    nx_text(cr, "Barre des tâches et menus", 14, true, nx_rgb(NX_TEXT), x0, y, 0, 24, NX_LEFT);
    static const char *op[] = { "Transparents", "Translucides", "Opaques" };
    double gap = 12, w3 = (cw - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        double x = x0 + i * (w3 + gap);
        choice_card(cr, x, y + 32, w3, 96, K_OPACITY, i, nx_conf.glass_opacity == i, op[i]);
        /* aperçu : fond d'écran + barre avec cette transparence */
        int saved = nx_conf.glass_opacity;
        nx_conf.glass_opacity = i;
        cairo_save(cr);
        nx_rounded_rect(cr, x + 12, y + 44, w3 - 24, 46, 6);
        cairo_clip(cr);
        cairo_translate(cr, x + 12, y + 44);
        nx_draw_wallpaper_cached(cr, nx_conf.wallpaper, w3 - 24, 46);
        cairo_restore(cr);
        mini_panel(cr, x + 12, y + 70, w3 - 24, 20);
        nx_conf.glass_opacity = saved;
    }
    toggle_row(cr, x0, y + 144, cw, K_TINT, 0, "Teinte de la couleur d'accent",
               "Colore légèrement la barre des tâches, le menu et les panneaux", nx_conf.glass_tint);

    /* Aperçu global */
    nx_text(cr, "Aperçu", 14, true, nx_rgb(NX_TEXT), x0, y + 214, 0, 24, NX_LEFT);
    double py = y + 244, ph = 70;
    cairo_save(cr);
    nx_rounded_rect(cr, x0, py, cw, ph, 10);
    cairo_clip(cr);
    cairo_translate(cr, x0, py);
    nx_draw_wallpaper_cached(cr, nx_conf.wallpaper, cw, ph);
    cairo_restore(cr);
    cairo_save(cr);
    nx_rounded_rect(cr, x0, py, cw, ph, 10);
    cairo_clip(cr);
    mini_panel(cr, x0, py + ph - 34, cw, 34);
    cairo_restore(cr);
}

static const int panel_heights[] = { 42, 52, 72, 96 };
static const int menu_scales[] = { 100, 125, 150 };

static void draw_panel_section(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Barre des tâches et menu", "Hauteur de la barre et taille du menu Démarrer.");
    static const char *names[] = { "Petite", "Moyenne", "Grande", "Très grande" };
    double gap = 12, w4 = (cw - 3 * gap) / 4, y = 98;
    for (int i = 0; i < 4; i++) {
        double x = x0 + i * (w4 + gap);
        choice_card(cr, x, y, w4, 150, K_PANEL, i, nx_conf.panel_height == panel_heights[i], names[i]);
        cairo_save(cr);
        nx_rounded_rect(cr, x + 10, y + 10, w4 - 20, 100, 8);
        cairo_clip(cr);
        cairo_translate(cr, x + 10, y + 10);
        nx_draw_wallpaper_cached(cr, nx_conf.wallpaper, w4 - 20, 100);
        cairo_restore(cr);
        double ph = panel_heights[i] * 0.42;   /* aperçu réduit */
        cairo_save(cr);
        nx_rounded_rect(cr, x + 10, y + 10, w4 - 20, 100, 8);
        cairo_clip(cr);
        mini_panel(cr, x + 10, y + 110 - ph, w4 - 20, ph);
        cairo_restore(cr);
    }
    char cur[96];
    snprintf(cur, sizeof cur, "Hauteur actuelle : %d px — astuce : tire le bord du haut de la barre à la souris.",
             nx_conf.panel_height);
    nx_text(cr, cur, 12.5, false, nx_rgb(NX_SUBTLE), x0, y + 156, cw, 22, NX_LEFT);

    /* Menu Démarrer */
    y += 198;
    nx_text(cr, "Taille du menu Démarrer", 14, true, nx_rgb(NX_TEXT), x0, y, 0, 24, NX_LEFT);
    static const char *mnames[] = { "Normal", "Grand", "Très grand" };
    double w3 = (cw - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        double x = x0 + i * (w3 + gap), by = y + 30;
        choice_card(cr, x, by, w3, 130, K_MENU, i, nx_conf.menu_scale == menu_scales[i], mnames[i]);
        /* menu miniature, proportionnel */
        double k = menu_scales[i] / 150.0, mw = (w3 - 40) * k, mh = 62 * k;
        nx_rounded_rect(cr, x + 20, by + 84 - mh, mw, mh, 6);
        nx_glass_source(cr, by + 84 - mh, by + 84);
        cairo_fill(cr);
        nx_rounded_rect(cr, x + 26, by + 90 - mh, mw * 0.25, mh - 12, 3);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
        cairo_fill(cr);
        for (int k2 = 0; k2 < 3; k2++)
            nx_app_icon(cr, (const char *[]){ "files", "browser", "store" }[k2],
                        x + 26 + mw * 0.3 + k2 * mw * 0.22, by + 90 - mh, mw * 0.16, nx_rgb("#ffffff"));
    }
    snprintf(cur, sizeof cur, "Taille actuelle : %d %% — astuce : tire le coin en haut à droite du menu.",
             nx_conf.menu_scale);
    nx_text(cr, cur, 12.5, false, nx_rgb(NX_SUBTLE), x0, y + 166, cw, 22, NX_LEFT);
    nx_text(cr, "Le texte de la barre grandit avec elle ; pour tout le texte : section « Affichage ».",
            12.5, false, nx_rgb(NX_SUBTLE), x0, y + 190, cw, 22, NX_LEFT);
}

static void draw_widgets_section(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Widgets", "Horloge, performances et tâches sur le bureau.");
    toggle_row(cr, x0, 96, cw, K_TOGGLE, 0, "Afficher les widgets",
               "En haut à droite du bureau (déplaçables à la souris)", nx_conf.widgets);

    nx_text(cr, "Taille", 14, true, nx_rgb(NX_TEXT), x0, 166, 0, 24, NX_LEFT);
    static const char *sizes[] = { "Petits", "Moyens", "Grands", "Très grands" };
    static const int scales[] = { 80, 100, 130, 160 };
    double gap = 12, w4 = (cw - 3 * gap) / 4;
    for (int i = 0; i < 4; i++) {
        double x = x0 + i * (w4 + gap);
        choice_card(cr, x, 194, w4, 90, K_WSIZE, i, nx_conf.widget_scale == scales[i], sizes[i]);
        double k = scales[i] / 160.0;
        nx_rounded_rect(cr, x + w4 / 2 - 34 * k, 206, 68 * k, 44 * k, 8 * k);
        nx_glass_source(cr, 206, 206 + 44 * k);
        cairo_fill(cr);
        nx_text(cr, "12:00", 13 * k, true, nx_rgb("#ffffff"), x + w4 / 2 - 34 * k, 206, 68 * k, 44 * k, NX_CENTER);
    }

    nx_text(cr, "Couleur", 14, true, nx_rgb(NX_TEXT), x0, 298, 0, 24, NX_LEFT);
    static const char *styles[] = { "Sombres", "Clairs", "Couleur d'accent", "Transparents" };
    for (int i = 0; i < 4; i++) {
        double x = x0 + i * (w4 + gap);
        choice_card(cr, x, 326, w4, 104, K_WSTYLE, i, nx_conf.widget_style == i, styles[i]);
        /* aperçu sur le fond d'écran */
        cairo_save(cr);
        nx_rounded_rect(cr, x + 10, 336, w4 - 20, 56, 8);
        cairo_clip(cr);
        cairo_translate(cr, x + 10, 336);
        nx_draw_wallpaper_cached(cr, nx_conf.wallpaper, w4 - 20, 56);
        cairo_restore(cr);
        double cx = x + 20, cy = 344, cww = w4 - 40, chh = 40;
        nx_rounded_rect(cr, cx, cy, cww, chh, 8);
        nx_color fg = nx_rgb("#ffffff");
        if (i == 0) { nx_glass_source(cr, cy, cy + chh); cairo_fill(cr); }
        else if (i == 1) { cairo_set_source_rgba(cr, 0.97, 0.98, 1, 0.92); cairo_fill(cr); fg = nx_rgb(NX_TEXT); }
        else if (i == 2) { nx_set(cr, nx_alpha(nx_accent_color(), 0.9)); cairo_fill(cr); }
        else { cairo_set_source_rgba(cr, 0, 0, 0, 0.18); cairo_fill(cr); }
        nx_text(cr, "12:00", 14, true, fg, cx, cy, cww, chh, NX_CENTER);
    }

    button_row(cr, x0, 450, "Remettre les widgets en haut à droite", K_WRESET);
    nx_text(cr, "Astuce : fais glisser une carte pour déplacer les widgets ; tire le coin en bas à",
            12.5, false, nx_rgb(NX_SUBTLE), x0, 500, cw, 20, NX_LEFT);
    nx_text(cr, "gauche (ou le bord gauche) pour les agrandir ou les réduire.",
            12.5, false, nx_rgb(NX_SUBTLE), x0, 520, cw, 20, NX_LEFT);
}

static void draw_icons_section(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Icônes", "Taille, style et forme des logos des applications.");
    nx_color dark = nx_rgb(NX_TEXT);
    int saved_size = nx_conf.icon_size, saved_style = nx_conf.icon_style, saved_shape = nx_conf.icon_shape;

    /* Taille */
    nx_text(cr, "Taille", 14, true, dark, x0, 96, 0, 24, NX_LEFT);
    static const char *sizes[] = { "Petites", "Moyennes", "Grandes" };
    double gap = 12, w3 = (cw - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        double x = x0 + i * (w3 + gap);
        choice_card(cr, x, 124, w3, 96, K_ISIZE, i, saved_size == i, sizes[i]);
        nx_conf.icon_size = i;
        double s = 34 * nx_icon_factor();
        nx_app_icon(cr, "files", x + w3 / 2 - s - 3, 124 + 52 - s / 2 - 8, s, dark);
        nx_app_icon(cr, "browser", x + w3 / 2 + 3, 124 + 52 - s / 2 - 8, s, dark);
    }
    nx_conf.icon_size = saved_size;

    /* Style */
    nx_text(cr, "Style", 14, true, dark, x0, 232, 0, 24, NX_LEFT);
    static const char *styles[] = { "En couleurs", "Monochromes" };
    double w2 = (cw - gap) / 2;
    for (int i = 0; i < 2; i++) {
        double x = x0 + i * (w2 + gap);
        choice_card(cr, x, 260, w2, 70, K_ISTYLE, i, saved_style == i, NULL);
        nx_conf.icon_style = i;
        static const char *demo[] = { "files", "browser", "terminal" };
        for (int k = 0; k < 3; k++) nx_app_icon(cr, demo[k], x + 14 + k * 36, 281, 28, dark);
        nx_text(cr, styles[i], 13, saved_style == i, dark, x + 128, 260, w2 - 156, 70, NX_LEFT);
    }
    nx_conf.icon_style = saved_style;

    /* Forme (pastille des icônes en couleurs) */
    nx_text(cr, "Forme", 14, true, dark, x0, 342, 0, 24, NX_LEFT);
    static const char *shapes[] = { "Arrondie", "Carrée", "Ronde" };
    for (int i = 0; i < 3; i++) {
        double x = x0 + i * (w3 + gap);
        choice_card(cr, x, 370, w3, 70, K_ISHAPE, i, saved_shape == i, NULL);
        nx_conf.icon_shape = i;
        nx_conf.icon_style = 0;
        nx_app_icon(cr, "settings", x + 16, 389, 32, dark);
        nx_text(cr, shapes[i], 13, saved_shape == i, dark, x + 60, 370, w3 - 70, 70, NX_LEFT);
    }
    nx_conf.icon_shape = saved_shape;
    nx_conf.icon_style = saved_style;
    if (saved_style == 1)
        nx_text(cr, "(la forme s'applique aux icônes en couleurs)", 12, false, nx_rgb(NX_SUBTLE),
                x0 + 60, 342, 0, 24, NX_LEFT);

    /* Aperçu : toutes les applications */
    nx_text(cr, "Aperçu", 14, true, dark, x0, 452, 0, 24, NX_LEFT);
    nx_rounded_rect(cr, x0, 480, cw, 76, 12);
    nx_glass_source(cr, 480, 556);
    cairo_fill(cr);
    static const char *all[] = { "files", "browser", "store", "terminal", "settings",
                                 "monitor", "editor", "viewer", "calculator", "about" };
    double s = 40 * nx_icon_factor(), step2 = (cw - 20) / 10;
    for (int k = 0; k < 10; k++)
        nx_app_icon(cr, all[k], x0 + 10 + k * step2 + (step2 - s) / 2, 518 - s / 2, s, nx_rgb("#ffffff"));
}

static void draw_display(cairo_t *cr, double x0, double cw)
{
    (void)cw;
    nx_color accent = nx_accent_color();
    section_title(cr, x0, "Affichage", "Taille de l'affichage, du texte et éléments du bureau.");

    /* Taille de l'affichage : agrandit tout l'écran (toutes les fenêtres) */
    nx_text(cr, "Taille de l'affichage", 14, true, nx_rgb(NX_TEXT), x0, 104, 0, 24, NX_LEFT);
    static const struct { int pct; const char *name; } ds[] = {
        { 0, "Auto" }, { 100, "100 %" }, { 125, "125 %" },
        { 150, "150 %" }, { 175, "175 %" }, { 200, "200 %" },
    };
    for (int i = 0; i < 6; i++) {
        double w = (cw - 5 * 8) / 6, x = x0 + i * (w + 8), y = 134, h = 40;
        bool sel = nx_conf.display_scale == ds[i].pct;
        nx_rounded_rect(cr, x, y, w, h, 10);
        nx_set(cr, sel ? accent : is_hover(K_DSCALE, i) ? nx_rgb("#eef1f6") : nx_rgb("#f6f7fa"));
        cairo_fill(cr);
        nx_text(cr, ds[i].name, 13, true, sel ? nx_rgb("#ffffff") : nx_rgb(NX_TEXT), x, y, w, h, NX_CENTER);
        add_rect(x, y, w, h, K_DSCALE, i);
    }
    nx_text(cr, "Agrandit tout l'écran. « Auto » choisit selon la taille de l'écran.",
            12.5, false, nx_rgb(NX_SUBTLE), x0, 180, 0, 22, NX_LEFT);
    cairo_save(cr);
    cairo_translate(cr, 0, 112);   /* la suite de la section descend d'autant */
    rect_dy = 112;

    nx_text(cr, "Taille du texte", 14, true, nx_rgb(NX_TEXT), x0, 104, 0, 24, NX_LEFT);
    static const struct { int scale; const char *name; double size; } opts[] = {
        { 100, "Normal", 22 }, { 125, "Grand", 26 }, { 150, "Très grand", 28 },
    };
    for (int i = 0; i < 3; i++) {
        double w = (cw - 24) / 3, x = x0 + i * (w + 12), y = 140, h = 128;
        bool sel = nx_conf.text_scale == opts[i].scale;
        nx_rounded_rect(cr, x, y, w, h, 12);
        nx_set(cr, sel ? nx_alpha(accent, 0.08) : nx_rgb("#f6f7fa"));
        cairo_fill_preserve(cr);
        nx_set(cr, sel ? accent : is_hover(K_SCALE, i) ? nx_alpha(accent, 0.5) : nx_rgb("#dde1e8"));
        cairo_set_line_width(cr, sel ? 3 : 1.5);
        cairo_stroke(cr);
        double saved = nx_scale;
        nx_scale = 1;   /* l'aperçu montre la taille réelle de chaque option */
        nx_text(cr, "Aa", opts[i].size * opts[i].scale / 100.0, true, nx_rgb(NX_TEXT), x, y + 14, w, 64, NX_CENTER);
        nx_text(cr, opts[i].name, 14, sel, nx_rgb(NX_TEXT), x, y + 82, w, 30, NX_CENTER);
        nx_scale = saved;
        if (sel) draw_check(cr, x + w - 18, y + 18, 11, accent);
        add_rect(x, y, w, h, K_SCALE, i);
    }
    nx_text(cr, "S'applique à la barre des tâches, au menu Démarrer et aux Paramètres.",
            12.5, false, nx_rgb(NX_SUBTLE), x0, 280, 0, 22, NX_LEFT);

    /* Interrupteurs : widgets et icônes du bureau */
    nx_text(cr, "Bureau", 14, true, nx_rgb(NX_TEXT), x0, 322, 0, 24, NX_LEFT);
    struct { const char *title, *sub; bool on; } sw[] = {
        { "Widgets du bureau", "Horloge, performances et tâches en haut à droite", nx_conf.widgets },
        { "Icônes du bureau", "« Ce PC », « Mes fichiers » et « Corbeille »", nx_conf.desktop_icons },
    };
    for (int i = 0; i < 2; i++) {
        double y = 354 + i * 64, w = cw;
        nx_rounded_rect(cr, x0, y, w, 56, 10);
        nx_set(cr, is_hover(K_TOGGLE, i) ? nx_rgb("#eef1f6") : nx_rgb("#f6f7fa"));
        cairo_fill(cr);
        nx_text(cr, sw[i].title, 14, true, nx_rgb(NX_TEXT), x0 + 16, y + 6, w - 100, 24, NX_LEFT);
        nx_text(cr, sw[i].sub, 12, false, nx_rgb(NX_SUBTLE), x0 + 16, y + 28, w - 100, 20, NX_LEFT);
        double tx = x0 + w - 64, ty = y + 16;   /* interrupteur */
        nx_rounded_rect(cr, tx, ty, 46, 24, 12);
        nx_set(cr, sw[i].on ? accent : nx_rgb("#c5cbd6"));
        cairo_fill(cr);
        cairo_new_path(cr);
        cairo_arc(cr, sw[i].on ? tx + 34 : tx + 12, ty + 12, 9, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill(cr);
        add_rect(x0, y, w, 56, K_TOGGLE, i);
    }
    rect_dy = 0;
    cairo_restore(cr);
}

/* ---------- Comptes utilisateurs ---------- */

#define ACCOUNT_HELPER "/usr/local/libexec/nexus-account"

struct account { char name[64], gecos[64]; unsigned uid; };
static struct account accounts[32];
static int naccounts;
static char me[64];

static void load_accounts(void)
{
    naccounts = 0;
    struct passwd *p = getpwuid(getuid());
    snprintf(me, sizeof me, "%s", p ? p->pw_name : "");
    setpwent();
    while ((p = getpwent()) && naccounts < 32) {
        if (p->pw_uid != 0 && (p->pw_uid < 1000 || p->pw_uid >= 65534)) continue;
        struct account *a = &accounts[naccounts++];
        snprintf(a->name, sizeof a->name, "%s", p->pw_name);
        snprintf(a->gecos, sizeof a->gecos, "%s", p->pw_gecos ? p->pw_gecos : "");
        a->uid = p->pw_uid;
    }
    endpwent();
}

/* Boîte de dialogue : 1 ajouter, 2 mot de passe, 3 supprimer */
static int dlg;
static int dlg_account;
static int dlg_focus;
static bool dlg_wipe;
static char dlg_msg[200];
static char fields[4][128];     /* 0 nom, 1 mot de passe, 2 confirmation, 3 mot de passe d'autorisation */

static bool need_auth(void) { return getuid() != 0; }

static void open_dialog(int mode, int account)
{
    dlg = mode;
    dlg_account = account;
    dlg_wipe = false;
    dlg_msg[0] = 0;
    for (int i = 0; i < 4; i++) explicit_bzero(fields[i], sizeof fields[i]);
    dlg_focus = mode == 1 ? 0 : mode == 2 ? 1 : 3;
    if (mode == 3 && !need_auth()) dlg_focus = -1;
}

static void close_dialog(void)
{
    dlg = 0;
    for (int i = 0; i < 4; i++) explicit_bzero(fields[i], sizeof fields[i]);
}

/* Lance nexus-account et attend sa réponse (quelques secondes au plus) */
static int run_account_helper(char *const args[], const char *input, char *out, size_t outn)
{
    int in[2], o[2];
    if (pipe(in) || pipe(o)) return -1;
    void (*old)(int) = signal(SIGCHLD, SIG_DFL);   /* pour récupérer le code de retour */
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(o[1], 1); dup2(o[1], 2);
        close(in[0]); close(in[1]); close(o[0]); close(o[1]);
        execv(ACCOUNT_HELPER, args);
        fprintf(stderr, "Outil de gestion des comptes absent (%s)\n", ACCOUNT_HELPER);
        _exit(127);
    }
    close(in[0]); close(o[1]);
    size_t l = strlen(input);
    if (write(in[1], input, l) != (ssize_t)l) { /* l'outil refusera */ }
    close(in[1]);
    size_t n = 0;
    ssize_t r;
    while (n + 1 < outn && (r = read(o[0], out + n, outn - 1 - n)) > 0) n += r;
    out[n] = 0;
    close(o[0]);
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    signal(SIGCHLD, old);
    out[strcspn(out, "\n")] = 0;
    return pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void dialog_submit(void)
{
    char input[512] = "", out[256] = "";
    char *args[5] = { "nexus-account", NULL, NULL, NULL, NULL };
    const char *name = dlg == 1 ? fields[0] : accounts[dlg_account].name;
    if (dlg == 1 && !fields[0][0]) { snprintf(dlg_msg, sizeof dlg_msg, "Choisis un nom de compte."); return; }
    if (dlg != 3) {
        if (!fields[1][0]) { snprintf(dlg_msg, sizeof dlg_msg, "Choisis un mot de passe."); return; }
        if (strcmp(fields[1], fields[2])) {
            snprintf(dlg_msg, sizeof dlg_msg, "Les deux mots de passe sont différents.");
            return;
        }
    }
    if (need_auth() && !fields[3][0]) {
        snprintf(dlg_msg, sizeof dlg_msg, "Saisis le mot de passe d'autorisation.");
        dlg_focus = 3;
        return;
    }
    if (need_auth()) snprintf(input, sizeof input, "%s\n", fields[3]);
    if (dlg != 3) {
        size_t l = strlen(input);
        snprintf(input + l, sizeof input - l, "%s\n", fields[1]);
    }
    args[1] = dlg == 1 ? "add" : dlg == 2 ? "passwd" : "del";
    args[2] = (char *)name;
    if (dlg == 3) args[3] = dlg_wipe ? "wipe" : "keep";
    int code = run_account_helper(args, input, out, sizeof out);
    explicit_bzero(input, sizeof input);
    if (code == 0) {
        close_dialog();
        load_accounts();
    } else {
        snprintf(dlg_msg, sizeof dlg_msg, "%s", code == 3 ? "Mot de passe d'autorisation incorrect." :
                 out[0] ? out : "L'opération a échoué.");
        explicit_bzero(fields[3], sizeof fields[3]);
        if (need_auth()) dlg_focus = 3;
    }
}

static void draw_accounts_section(cairo_t *cr, double x0, double cw)
{
    nx_color accent = nx_accent_color();
    section_title(cr, x0, "Comptes", "Personnes qui peuvent se connecter à cet ordinateur.");
    double y = 100;
    for (int i = 0; i < naccounts; i++, y += 66) {
        struct account *a = &accounts[i];
        bool root = a->uid == 0, self = !strcmp(a->name, me);
        nx_rounded_rect(cr, x0, y, cw, 58, 12);
        nx_set(cr, nx_rgb("#f6f7fa"));
        cairo_fill(cr);
        cairo_arc(cr, x0 + 30, y + 29, 18, 0, 2 * M_PI);
        nx_set(cr, root ? nx_rgb("#5a6170") : accent);
        cairo_fill(cr);
        char ini[8] = { (char)toupper((unsigned char)a->name[0]), 0 };
        nx_text(cr, ini, 15, true, nx_rgb("#ffffff"), x0 + 12, y + 11, 36, 36, NX_CENTER);
        nx_text(cr, root ? "Administrateur (root)" : a->name, 14.5, true, nx_rgb(NX_TEXT),
                x0 + 60, y + 8, cw - 330, 24, NX_LEFT);
        char sub[96];
        snprintf(sub, sizeof sub, "%s%s", root ? "Tous les droits sur le système" : "Compte ordinaire",
                 self ? " · connecté" : "");
        nx_text(cr, sub, 12, false, nx_rgb(NX_SUBTLE), x0 + 60, y + 30, cw - 330, 20, NX_LEFT);
        /* boutons */
        double bx = x0 + cw - 250;
        nx_rounded_rect(cr, bx, y + 12, 130, 34, 17);
        nx_set(cr, is_hover(K_ACC_PW, i) ? nx_alpha(accent, 0.12) : nx_rgb("#ffffff"));
        cairo_fill_preserve(cr);
        nx_set(cr, accent); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
        nx_text(cr, "Mot de passe", 13, true, accent, bx, y + 12, 130, 34, NX_CENTER);
        add_rect(bx, y + 12, 130, 34, K_ACC_PW, i);
        if (!root && !self) {
            nx_color red = nx_rgb("#d83b3b");
            nx_rounded_rect(cr, bx + 140, y + 12, 100, 34, 17);
            nx_set(cr, is_hover(K_ACC_DEL, i) ? nx_alpha(red, 0.12) : nx_rgb("#ffffff"));
            cairo_fill_preserve(cr);
            nx_set(cr, red); cairo_stroke(cr);
            nx_text(cr, "Supprimer", 13, true, red, bx + 140, y + 12, 100, 34, NX_CENTER);
            add_rect(bx + 140, y + 12, 100, 34, K_ACC_DEL, i);
        }
    }
    button_row(cr, x0, y + 8, "+  Ajouter un compte", K_ACC_ADD);
    nx_text(cr, need_auth() ? "Les modifications demandent le mot de passe administrateur (root)."
                            : "Tu es connecté en administrateur : aucun mot de passe ne sera demandé.",
            12.5, false, nx_rgb(NX_SUBTLE), x0, y + 58, cw, 22, NX_LEFT);
}

static void dlg_field(cairo_t *cr, double x, double y, double w, int idx, const char *label, bool masked)
{
    nx_text(cr, label, 12.5, true, nx_rgb(NX_TEXT), x, y, w, 20, NX_LEFT);
    bool f = dlg_focus == idx;
    nx_rounded_rect(cr, x, y + 22, w, 36, 8);
    nx_set(cr, nx_rgb("#f4f6fa"));
    cairo_fill_preserve(cr);
    nx_set(cr, f ? nx_accent_color() : nx_rgb("#d3d8e2"));
    cairo_set_line_width(cr, f ? 2 : 1.2);
    cairo_stroke(cr);
    char shown[3 * 128] = "";
    if (masked) {
        size_t n = 0;
        for (const char *p = fields[idx]; *p; p++) if ((*p & 0xC0) != 0x80) n++;
        for (size_t i = 0; i < n && i < 40; i++) strcat(shown, "●");
    } else {
        snprintf(shown, sizeof shown, "%s", fields[idx]);
    }
    double tw = nx_text(cr, shown, 13, false, nx_rgb(NX_TEXT), x + 12, y + 22, w - 24, 36, NX_LEFT);
    if (f) {
        cairo_rectangle(cr, x + 13 + tw, y + 31, 1.5, 18);
        nx_set(cr, nx_rgb(NX_TEXT));
        cairo_fill(cr);
    }
    add_rect(x, y + 22, w, 36, K_DLG_FIELD, idx);
}

static void draw_account_dialog(cairo_t *cr, int width, int height)
{
    nrects = 0;
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_set_source_rgba(cr, 0.05, 0.07, 0.12, 0.45);
    cairo_fill(cr);
    int nf = (dlg == 1 ? 3 : dlg == 2 ? 2 : 0) + (need_auth() ? 1 : 0);
    double w = 460, h = 132 + nf * 64 + (dlg == 3 ? 70 : 0) + (dlg_msg[0] ? 24 : 0);
    double x = (width - w) / 2, y = (height - h) / 2;
    nx_rounded_rect(cr, x, y, w, h, 16);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    char title[160];
    const char *who = dlg == 1 ? "" : accounts[dlg_account].uid == 0 ? "root" : accounts[dlg_account].name;
    if (dlg == 1) snprintf(title, sizeof title, "Ajouter un compte");
    else if (dlg == 2) snprintf(title, sizeof title, "Mot de passe de %s", who);
    else snprintf(title, sizeof title, "Supprimer le compte %s ?", who);
    nx_icon(cr, dlg == 3 ? "trash" : "user", x + 24, y + 22, 30, nx_accent_color());
    nx_text(cr, title, 17, true, nx_rgb(NX_TEXT), x + 66, y + 22, w - 90, 30, NX_LEFT);
    double fy = y + 70, fw = w - 48;
    if (dlg == 1) { dlg_field(cr, x + 24, fy, fw, 0, "Nom du compte (minuscules, sans espace)", false); fy += 64; }
    if (dlg != 3) {
        dlg_field(cr, x + 24, fy, fw, 1, "Nouveau mot de passe", true); fy += 64;
        dlg_field(cr, x + 24, fy, fw, 2, "Confirme le mot de passe", true); fy += 64;
    } else {
        nx_text(cr, "Le compte ne pourra plus se connecter.", 13, false, nx_rgb("#3a4152"),
                x + 24, fy, fw, 22, NX_LEFT);
        double cbx = x + 24, cby = fy + 32;
        nx_rounded_rect(cr, cbx, cby, 20, 20, 5);
        if (dlg_wipe) { nx_set(cr, nx_rgb("#d83b3b")); cairo_fill(cr); }
        else { nx_set(cr, nx_rgb("#9aa3b8")); cairo_set_line_width(cr, 1.5); cairo_stroke(cr); }
        nx_text(cr, "Supprimer aussi ses fichiers (dossier personnel)", 13, false, nx_rgb(NX_TEXT),
                cbx + 30, cby - 4, fw - 30, 28, NX_LEFT);
        add_rect(cbx, cby - 4, fw, 28, K_DLG_WIPE, 0);
        fy += 70;
    }
    if (need_auth()) {
        dlg_field(cr, x + 24, fy, fw, 3, dlg == 2 && !strcmp(who, me) ?
                  "Ton mot de passe actuel (ou celui de root)" : "Mot de passe administrateur (root)", true);
        fy += 64;
    }
    if (dlg_msg[0]) nx_text(cr, dlg_msg, 12.5, true, nx_rgb("#d83b3b"), x + 24, fy, fw, 22, NX_LEFT);
    double by = y + h - 54;
    nx_color ok = dlg == 3 ? nx_rgb("#d83b3b") : nx_accent_color();
    nx_rounded_rect(cr, x + w - 24 - 130, by, 130, 36, 18);
    nx_set(cr, is_hover(K_DLG_OK, 0) ? nx_mix(ok, nx_rgb("#000000"), 0.12) : ok);
    cairo_fill(cr);
    nx_text(cr, dlg == 1 ? "Créer" : dlg == 2 ? "Changer" : "Supprimer", 13.5, true, nx_rgb("#ffffff"),
            x + w - 154, by, 130, 36, NX_CENTER);
    add_rect(x + w - 154, by, 130, 36, K_DLG_OK, 0);
    nx_rounded_rect(cr, x + w - 24 - 250, by, 110, 36, 18);
    nx_set(cr, nx_rgb("#5a6170")); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
    nx_text(cr, "Annuler", 13.5, true, nx_rgb("#5a6170"), x + w - 274, by, 110, 36, NX_CENTER);
    add_rect(x + w - 274, by, 110, 36, K_DLG_CANCEL, 0);
}

/* Saisie dans la boîte de dialogue ; renvoie true si la touche est prise */
static bool dialog_key(xkb_keysym_t sym, const char *utf8)
{
    if (!dlg) return false;
    int order[4], n = 0;
    if (dlg == 1) order[n++] = 0;
    if (dlg != 3) { order[n++] = 1; order[n++] = 2; }
    if (need_auth()) order[n++] = 3;
    if (sym == XKB_KEY_Escape) close_dialog();
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) dialog_submit();
    else if (sym == XKB_KEY_Tab && n) {
        int k = 0;
        while (k < n && order[k] != dlg_focus) k++;
        dlg_focus = order[(k + 1) % n];
    } else if (dlg_focus >= 0) {
        char *b = fields[dlg_focus];
        size_t l = strlen(b);
        if (sym == XKB_KEY_BackSpace) {
            while (l > 0 && (b[l - 1] & 0xC0) == 0x80) b[--l] = 0;
            if (l > 0) b[--l] = 0;
        } else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f && l + strlen(utf8) < 120) {
            strcat(b, utf8);
        }
    }
    return true;
}

static void draw_about(cairo_t *cr, double x0, double cw)
{
    nx_logo(cr, x0, 34, 92);
    nx_text(cr, "Nexus OS", 30, true, nx_rgb(NX_TEXT), x0 + 116, 40, 0, 40, NX_LEFT);
    nx_text(cr, "Une distribution Linux construite à partir du noyau.", 13.5, false,
            nx_rgb(NX_SUBTLE), x0 + 116, 84, cw - 116, 22, NX_LEFT);
    double y = 158;
    for (int i = 0; i < ninfo; i++) {
        if (i % 2 == 0) {
            nx_rounded_rect(cr, x0 - 10, y, cw + 20, 38, 8);
            nx_set(cr, nx_rgb("#f5f7fa"));
            cairo_fill(cr);
        }
        nx_text(cr, info_label[i], 13.5, true, nx_rgb(NX_TEXT), x0 + 4, y, 190, 38, NX_LEFT);
        nx_text(cr, info_value[i], 13.5, false, nx_rgb("#3a4152"), x0 + 200, y, cw - 210, 38, NX_LEFT);
        y += 40;
    }
    (void)pretty_name;
}

/* ---------- Thème ---------- */

static void draw_theme_section(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Thème", "Barre des tâches, menu Démarrer, notifications et panneaux.");
    static const char *names[] = { "Sombre", "Clair" };
    double gap = 16, w2 = (cw - gap) / 2, y = 100, h = 230;
    for (int i = 0; i < 2; i++) {
        double x = x0 + i * (w2 + gap);
        choice_card(cr, x, y, w2, h, K_THEME, i, nx_conf.theme == i, names[i]);
        /* aperçu : fond d'écran, menu et barre dans ce thème */
        int saved = nx_conf.theme;
        nx_conf.theme = i;
        double px = x + 14, py = y + 14, pw = w2 - 28, ph = h - 62;
        cairo_save(cr);
        nx_rounded_rect(cr, px, py, pw, ph, 8);
        cairo_clip(cr);
        cairo_translate(cr, px, py);
        nx_draw_wallpaper_cached(cr, nx_conf.wallpaper, pw, ph);
        cairo_restore(cr);
        cairo_save(cr);
        nx_rounded_rect(cr, px, py, pw, ph, 8);
        cairo_clip(cr);
        nx_glass(cr, px + 10, py + 14, pw * 0.55, ph - 52, 8);
        nx_text(cr, "Menu", 11, true, nx_rgb(NX_GLASS_TEXT), px + 20, py + 22, 80, 18, NX_LEFT);
        for (int k = 0; k < 3; k++) {
            nx_rounded_rect(cr, px + 20, py + 48 + k * 22, pw * 0.55 - 20 - (k == 1 ? 30 : 0), 10, 5);
            nx_set(cr, nx_fg(0.25));
            cairo_fill(cr);
        }
        mini_panel(cr, px, py + ph - 28, pw, 28);
        cairo_restore(cr);
        nx_conf.theme = saved;
    }
    para_text(cr, "Les fenêtres des applications (Fichiers, Paramètres…) restent claires ; le thème change "
                  "la barre, le menu, les bulles de notification, les panneaux Son et Réseau, les menus "
                  "et Alt+Tab.", x0, y + h + 20, cw);
}

/* ---------- Verrouillage ---------- */

static const int lock_delays[] = { 0, 5, 10, 30 };

static void draw_lock_section(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Verrouillage", "Protéger l'écran quand tu t'absentes.");
    nx_text(cr, "Verrouiller automatiquement après", 14, true, nx_rgb(NX_TEXT), x0, 100, 0, 24, NX_LEFT);
    static const char *names[] = { "Jamais", "5 minutes", "10 minutes", "30 minutes" };
    double gap = 10, w4 = (cw - 3 * gap) / 4;
    for (int i = 0; i < 4; i++)
        choice_card(cr, x0 + i * (w4 + gap), 132, w4, 70, K_LOCKDELAY, i, nx_conf.lock_minutes == lock_delays[i], names[i]);
    nx_text(cr, "… sans toucher au clavier ni à la souris.", 12.5, false, nx_rgb(NX_SUBTLE), x0, 210, cw, 22, NX_LEFT);
    button_row(cr, x0, 252, "Verrouiller maintenant", K_LOCKNOW);
    para_text(cr, "Raccourci : Super+L (touche Windows + L), ou menu Démarrer → Alimentation → Verrouiller. "
                  "Pour déverrouiller, tape le mot de passe de ton compte.", x0, 310, cw);
}

/* ---------- Sécurité ---------- */

#define PKG_HELPER "/usr/local/libexec/nexus-pkg"
static bool fw_installed, fw_active, fw_ssh, fw_ping = true;

static void load_firewall(void)
{
    fw_installed = access("/usr/local/sbin/nexus-parefeu", X_OK) == 0;
    char st[32] = "";
    FILE *f = fopen("/run/nexus-parefeu.etat", "r");
    if (f) { if (fgets(st, sizeof st, f)) st[strcspn(st, "\n")] = 0; fclose(f); }
    fw_active = !strcmp(st, "actif");
    fw_ssh = false;
    fw_ping = true;
    if ((f = fopen("/usr/local/etc/nexus/parefeu.conf", "r"))) {
        char line[64];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "SSH=", 4)) fw_ssh = line[4] == '1';
            if (!strncmp(line, "PING=", 5)) fw_ping = line[5] == '1';
        }
        fclose(f);
    }
}

/* ---------- Protection : blocage, chiffrement, journal ---------- */

static struct secu_conf prot;            /* blocage après plusieurs erreurs */
enum { CR_LIVE, CR_NONE, CR_PENDING, CR_DONE };
static int crypt_state;
static bool crypt_tool;

static void load_protection(void)
{
    prot = secu_read_conf();
    crypt_tool = access("/usr/sbin/cryptsetup", X_OK) == 0;
    if (access("/dev/mapper/nexus-data", F_OK) == 0) crypt_state = CR_DONE;
    else if (access("/data/etc", F_OK) != 0) crypt_state = CR_LIVE;
    else if (access("/data/etc/.chiffrement-cle", F_OK) == 0) crypt_state = CR_PENDING;
    else crypt_state = CR_NONE;
}

/* nexus-pkg <args...>, avec un texte sur l'entrée standard ; sortie dans out */
static int run_pkg_helper(char *const args[], const char *input, char *out, size_t outn)
{
    int in[2], o[2];
    if (pipe(in) || pipe(o)) return -1;
    void (*old)(int) = signal(SIGCHLD, SIG_DFL);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(o[1], 1); dup2(o[1], 2);
        close(in[0]); close(in[1]); close(o[0]); close(o[1]);
        execv(PKG_HELPER, args);
        _exit(127);
    }
    close(in[0]); close(o[1]);
    if (input && write(in[1], input, strlen(input)) < 0) { /* l'outil refusera */ }
    close(in[1]);
    size_t n = 0;
    ssize_t r;
    while (n + 1 < outn && (r = read(o[0], out + n, outn - 1 - n)) > 0) n += r;
    out[n] = 0;
    char sink[512];
    while (read(o[0], sink, sizeof sink) > 0) {}   /* (sortie trop longue : le reste est ignoré) */
    close(o[0]);
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    signal(SIGCHLD, old);
    return pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Boîte de dialogue « mot de passe administrateur » (+ phrases secrètes) */
enum { SD_FW, SD_BLOCAGE, SD_CRYPT_ON, SD_CRYPT_OFF, SD_CRYPT_PHRASE, SD_JOURNAL };
static int sd_kind;
static char sec_action[16];              /* argument de l'action (pare-feu, blocage) */
static char sd_f[4][128];                /* champs saisis */
static int sd_nf, sd_first, sd_focus;    /* nombre de champs, premier affiché, champ actif */
static char sec_msg[200];
static bool sec_dlg;
static char crypt_info[200];             /* message sous la carte du chiffrement */

static char *journal_text;               /* journal affiché (NULL : fermé) */
static int journal_top;                  /* première ligne affichée */

static void sd_clear(void)
{
    for (int i = 0; i < 4; i++) explicit_bzero(sd_f[i], sizeof sd_f[i]);
}

static void sec_close(void) { sec_dlg = false; sd_clear(); }

static void sec_submit(void)
{
    for (int i = sd_first; i < sd_nf; i++)
        if (!sd_f[i][0]) { sd_focus = i; snprintf(sec_msg, sizeof sec_msg, "Remplis tous les champs."); return; }
    bool new_phrase = sd_kind == SD_CRYPT_ON || sd_kind == SD_CRYPT_PHRASE;
    if (new_phrase) {
        const char *p1 = sd_f[sd_nf - 2], *p2 = sd_f[sd_nf - 1];
        if (strlen(p1) < 8) { snprintf(sec_msg, sizeof sec_msg, "Phrase secrète trop courte (8 caractères minimum)."); return; }
        if (strcmp(p1, p2)) { snprintf(sec_msg, sizeof sec_msg, "Les deux phrases secrètes ne correspondent pas."); return; }
    }
    /* Entrée de l'outil : mot de passe root (sauf pour root), puis les phrases */
    char input[600] = "";
    for (int i = sd_first; i < sd_nf - (new_phrase ? 1 : 0); i++) {
        strncat(input, sd_f[i], sizeof input - strlen(input) - 2);
        strcat(input, "\n");
    }
    static char out[64000];
    char *args[4] = { "nexus-pkg", NULL, NULL, NULL };
    switch (sd_kind) {
    case SD_FW:           args[1] = "parefeu"; args[2] = sec_action; break;
    case SD_BLOCAGE:      args[1] = "blocage"; args[2] = sec_action; break;
    case SD_CRYPT_ON:     args[1] = "chiffrement"; args[2] = "on"; break;
    case SD_CRYPT_OFF:    args[1] = "chiffrement"; args[2] = "off"; break;
    case SD_CRYPT_PHRASE: args[1] = "chiffrement"; args[2] = "phrase"; break;
    case SD_JOURNAL:      args[1] = "journal"; break;
    }
    int code = run_pkg_helper(args, input, out, sizeof out);
    explicit_bzero(input, sizeof input);
    if (code == 0) {
        sec_close();
        load_firewall();
        load_protection();
        if (sd_kind == SD_CRYPT_ON)
            snprintf(crypt_info, sizeof crypt_info, "C'est noté : le disque sera chiffré au prochain démarrage.");
        else if (sd_kind == SD_CRYPT_OFF) snprintf(crypt_info, sizeof crypt_info, "Chiffrement annulé.");
        else if (sd_kind == SD_CRYPT_PHRASE) snprintf(crypt_info, sizeof crypt_info, "Phrase secrète changée.");
        else if (sd_kind == SD_JOURNAL) {
            free(journal_text);
            journal_text = strdup(out);
            int n = 0;
            for (const char *c = out; *c; c++) n += *c == '\n';
            journal_top = n;                     /* (ramené à la fin au dessin) */
        }
        return;
    }
    /* erreur : on efface les champs et on explique */
    sd_clear();
    sd_focus = sd_first;
    if (code == 3) snprintf(sec_msg, sizeof sec_msg, "Mot de passe administrateur incorrect.");
    else if (code == 127) snprintf(sec_msg, sizeof sec_msg, "Outil absent : mets à jour le système (npk upgrade).");
    else if (out[0]) { out[strcspn(out, "\n")] = 0; snprintf(sec_msg, sizeof sec_msg, "%s", out); }
    else snprintf(sec_msg, sizeof sec_msg, "L'opération a échoué.");
}

static void sec_open(int kind, const char *action)
{
    sd_kind = kind;
    snprintf(sec_action, sizeof sec_action, "%s", action ? action : "");
    sec_msg[0] = crypt_info[0] = 0;
    sd_clear();
    sd_nf = kind == SD_CRYPT_ON ? 3 : kind == SD_CRYPT_PHRASE ? 4 : 1;
    sd_first = getuid() == 0 ? 1 : 0;          /* root : pas de mot de passe à demander */
    sd_focus = sd_first;
    if (sd_first >= sd_nf) { sec_submit(); return; }
    sec_dlg = true;
}

/* Ancienne forme : réglage du pare-feu */
static void sec_request(const char *action) { sec_open(SD_FW, action); }

static void check_item(cairo_t *cr, double x, double y, double w, bool ok, const char *title, const char *sub)
{
    cairo_new_path(cr);
    cairo_arc(cr, x + 12, y + 18, 11, 0, 2 * M_PI);
    nx_set(cr, ok ? nx_rgb("#2fb36a") : nx_rgb("#e08a1e"));
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 2.2);
    if (ok) { cairo_move_to(cr, x + 7, y + 18); cairo_line_to(cr, x + 11, y + 22); cairo_line_to(cr, x + 17, y + 14); }
    else { cairo_move_to(cr, x + 12, y + 12); cairo_line_to(cr, x + 12, y + 19); cairo_move_to(cr, x + 12, y + 23); cairo_line_to(cr, x + 12, y + 24); }
    cairo_stroke(cr);
    nx_text(cr, title, 13.5, true, nx_rgb(NX_TEXT), x + 34, y + 2, w - 40, 20, NX_LEFT);
    nx_text(cr, sub, 12, false, nx_rgb(NX_SUBTLE), x + 34, y + 21, w - 40, 18, NX_LEFT);
}

static void draw_protection_section(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Protection", "Blocage des comptes, chiffrement du disque et journal de sécurité.");
    char sub[160];
    snprintf(sub, sizeof sub, "Après %d mots de passe ratés, le compte est bloqué %d minutes", prot.essais, prot.duree_min);
    toggle_row(cr, x0, 96, cw, K_BLOCAGE, 0, "Blocage après plusieurs erreurs", sub, prot.blocage);

    /* Chiffrement du disque */
    double y = 172;
    nx_text(cr, "Chiffrement du disque", 14, true, nx_rgb(NX_TEXT), x0, y, 0, 24, NX_LEFT);
    y += 30;
    static const char *titles[] = { "Pas de disque (mode live)", "Disque non chiffré",
                                    "Chiffrement prévu au prochain démarrage", "Disque chiffré" };
    static const char *subs[] = { "Rien n'est enregistré : il n'y a rien à chiffrer.",
                                  "Si quelqu'un prend le disque, il peut lire tes fichiers.",
                                  "Redémarre : le disque sera chiffré avant l'ouverture de session.",
                                  "La phrase secrète est demandée à chaque démarrage." };
    static const char *cols[] = { "#8a93a6", "#8a93a6", "#e08a1e", "#2fb36a" };
    nx_color c = nx_rgb(cols[crypt_state]);
    nx_rounded_rect(cr, x0, y, cw, 64, 12);
    nx_set(cr, nx_alpha(c, 0.12));
    cairo_fill(cr);
    nx_icon(cr, "lock", x0 + 16, y + 16, 32, nx_mix(c, nx_rgb("#000000"), 0.2));
    nx_text(cr, titles[crypt_state], 15.5, true, nx_rgb(NX_TEXT), x0 + 62, y + 10, cw - 80, 24, NX_LEFT);
    nx_text(cr, subs[crypt_state], 12.5, false, nx_rgb(NX_SUBTLE), x0 + 62, y + 34, cw - 80, 20, NX_LEFT);
    y += 78;
    if (!crypt_tool && crypt_state != CR_LIVE) {
        para_text(cr, "Outil de chiffrement absent : mets à jour Nexus OS (nouvelle image).", x0, y, cw);
        y += 30;
    } else if (crypt_state == CR_NONE) {
        button_at(cr, x0, y, "Chiffrer le disque…", K_CRYPT, 0);
        y += 50;
    } else if (crypt_state == CR_PENDING) {
        double w = button_at(cr, x0, y, "Redémarrer maintenant", K_CRYPT, 1);
        button_at(cr, x0 + w + 12, y, "Annuler le chiffrement", K_CRYPT, 2);
        y += 50;
    } else if (crypt_state == CR_DONE) {
        button_at(cr, x0, y, "Changer la phrase secrète…", K_CRYPT, 3);
        y += 50;
    }
    if (crypt_info[0]) {
        nx_text(cr, crypt_info, 12.5, true, nx_accent_color(), x0, y - 4, cw, 20, NX_LEFT);
        y += 22;
    }
    if (crypt_state != CR_LIVE) {
        para_text(cr, "Important : sans la phrase secrète, personne (même toi) ne peut relire le disque. "
                      "Note-la en lieu sûr. Le chiffrement ne peut pas être retiré ensuite.", x0, y, cw);
        y += 52;
    }

    /* Journal */
    y += 6;
    nx_text(cr, "Journal de sécurité", 14, true, nx_rgb(NX_TEXT), x0, y, 0, 24, NX_LEFT);
    y += 28;
    para_text(cr, "Connexions, mots de passe ratés, blocages, logiciels installés ou supprimés, "
                  "changements de réglages de sécurité.", x0, y, cw);
    y += 46;
    button_at(cr, x0, y, "Afficher le journal…", K_JOURNAL, 0);
}

static void draw_security_section(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Sécurité", "Pare-feu et protections de Nexus OS.");
    /* État du pare-feu */
    double y = 96;
    nx_rounded_rect(cr, x0, y, cw, 64, 12);
    nx_set(cr, fw_active ? nx_alpha(nx_rgb("#2fb36a"), 0.12) : nx_alpha(nx_rgb("#d64545"), 0.10));
    cairo_fill(cr);
    nx_icon(cr, "lock", x0 + 16, y + 16, 32, fw_active ? nx_rgb("#1f8a4f") : nx_rgb("#b83232"));
    nx_text(cr, !fw_installed ? "Pare-feu non installé" : fw_active ? "Pare-feu actif" : "Pare-feu désactivé",
            16, true, nx_rgb(NX_TEXT), x0 + 62, y + 10, cw - 80, 24, NX_LEFT);
    nx_text(cr, !fw_installed ? "Installe-le : su -c \"npk update && npk install parefeu\"" :
                fw_active ? "Les connexions entrantes que tu n'as pas demandées sont refusées." :
                            "Ton ordinateur accepte toutes les connexions entrantes.",
            12.5, false, nx_rgb(NX_SUBTLE), x0 + 62, y + 34, cw - 80, 20, NX_LEFT);
    if (fw_installed) {
        toggle_row(cr, x0, y + 78, cw, K_FW, 0, "Pare-feu",
                   "Bloque les connexions entrantes non demandées (recommandé)", fw_active);
        toggle_row(cr, x0, y + 142, cw, K_FW, 1, "Connexion à distance (SSH)",
                   "Autorise le port 22, seulement si tu utilises SSH", fw_ssh);
        toggle_row(cr, x0, y + 206, cw, K_FW, 2, "Répondre au ping",
                   "Permet de vérifier depuis le réseau que l'ordinateur est allumé", fw_ping);
    }
    /* Autres protections */
    y += fw_installed ? 286 : 86;
    nx_text(cr, "Protections", 14, true, nx_rgb(NX_TEXT), x0, y, 0, 24, NX_LEFT);
    y += 30;
    double w2 = (cw - 16) / 2;
    char lock[80], bl[80];
    if (nx_conf.lock_minutes > 0) snprintf(lock, sizeof lock, "Après %d min d'inactivité", nx_conf.lock_minutes);
    else snprintf(lock, sizeof lock, "Désactivé : voir Verrouillage");
    if (prot.blocage) snprintf(bl, sizeof bl, "Après %d erreurs, %d min", prot.essais, prot.duree_min);
    else snprintf(bl, sizeof bl, "Désactivé : voir Protection");
    check_item(cr, x0, y, w2, true, "Dossiers personnels privés", "Les autres comptes ne lisent pas tes fichiers");
    check_item(cr, x0 + w2 + 16, y, w2, true, "Logiciels signés", "Chaque paquet est vérifié avant installation");
    check_item(cr, x0, y + 50, w2, true, "Noyau protégé", "Yama, adresses aléatoires, sysctl durcis");
    check_item(cr, x0 + w2 + 16, y + 50, w2, nx_conf.lock_minutes > 0, "Verrouillage automatique", lock);
    check_item(cr, x0, y + 100, w2, prot.blocage, "Blocage des comptes", bl);
    check_item(cr, x0 + w2 + 16, y + 100, w2, crypt_state == CR_DONE, "Disque chiffré",
               crypt_state == CR_DONE ? "Phrase secrète au démarrage" :
               crypt_state == CR_PENDING ? "Au prochain démarrage" : "Désactivé : voir Protection");
}

static void draw_sec_dialog(cairo_t *cr, int width, int height)
{
    cairo_set_source_rgba(cr, 0.05, 0.07, 0.1, 0.35);
    cairo_paint(cr);
    add_rect(0, 0, width, height, K_SEC_BG, 0);   /* fond : capte les clics, ne fait rien */
    static const char *titles[] = { "Mot de passe administrateur", "Mot de passe administrateur",
                                    "Chiffrer le disque", "Annuler le chiffrement",
                                    "Changer la phrase secrète", "Journal de sécurité" };
    static const char *subs[] = {
        "Modifier le pare-feu demande le mot de passe root.",
        "Changer le blocage des comptes demande le mot de passe root.",
        "Choisis une phrase secrète (8 caractères minimum), demandée à chaque démarrage. "
        "Note-la : sans elle, tes fichiers sont perdus.",
        "Le disque ne sera pas chiffré au prochain démarrage.",
        "Le disque reste chiffré ; seule la phrase demandée au démarrage change.",
        "Lire le journal demande le mot de passe root." };
    static const char *labels[6][4] = {
        { "Mot de passe root" }, { "Mot de passe root" },
        { "Mot de passe root", "Phrase secrète du disque", "Confirme la phrase secrète" },
        { "Mot de passe root" },
        { "Mot de passe root", "Phrase secrète actuelle", "Nouvelle phrase secrète", "Confirme la nouvelle phrase" },
        { "Mot de passe root" } };
    int shown = sd_nf - sd_first;
    double dw = 460, dh = 190 + shown * 66, dx = (width - dw) / 2, dy = (height - dh) / 2;
    nx_rounded_rect(cr, dx, dy, dw, dh, 14);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    nx_text(cr, titles[sd_kind], 17, true, nx_rgb(NX_TEXT), dx + 24, dy + 18, dw - 48, 28, NX_LEFT);
    para_text(cr, subs[sd_kind], dx + 24, dy + 50, dw - 48);
    double fy = dy + 118;                      /* (explication : jusqu'à 3 lignes) */
    for (int i = sd_first; i < sd_nf; i++, fy += 66) {
        nx_text(cr, labels[sd_kind][i], 12.5, true, nx_rgb(NX_SUBTLE), dx + 24, fy, dw - 48, 18, NX_LEFT);
        nx_rounded_rect(cr, dx + 24, fy + 20, dw - 48, 40, 9);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        nx_set(cr, i == sd_focus ? nx_accent_color() : nx_rgb("#c5cbd6"));
        cairo_set_line_width(cr, i == sd_focus ? 2 : 1);
        cairo_stroke(cr);
        add_rect(dx + 24, fy + 20, dw - 48, 40, K_SEC_FIELD, i);
        int n = 0;
        for (const char *c = sd_f[i]; *c; c++) if (((unsigned char)*c & 0xC0) != 0x80) n++;
        for (int k = 0; k < n && k < 26; k++) {
            cairo_new_path(cr);
            cairo_arc(cr, dx + 42 + k * 14, fy + 40, 4, 0, 2 * M_PI);
            nx_set(cr, nx_rgb(NX_TEXT));
            cairo_fill(cr);
        }
    }
    if (sec_msg[0]) nx_text(cr, sec_msg, 12.5, true, nx_rgb("#c0392b"), dx + 24, fy + 2, dw - 48, 20, NX_LEFT);
    double by = dy + dh - 56;
    nx_rounded_rect(cr, dx + dw - 24 - 120, by, 120, 36, 8);
    nx_set(cr, is_hover(K_SEC_OK, 0) ? nx_mix(nx_accent_color(), nx_rgb("#ffffff"), 0.15) : nx_accent_color());
    cairo_fill(cr);
    nx_text(cr, "Valider", 13.5, true, nx_rgb("#ffffff"), dx + dw - 24 - 120, by, 120, 36, NX_CENTER);
    add_rect(dx + dw - 24 - 120, by, 120, 36, K_SEC_OK, 0);
    nx_rounded_rect(cr, dx + dw - 24 - 240, by, 110, 36, 8);
    nx_set(cr, is_hover(K_SEC_CANCEL, 0) ? nx_rgb("#e3e7ef") : nx_rgb("#eef1f6"));
    cairo_fill(cr);
    nx_text(cr, "Annuler", 13.5, false, nx_rgb(NX_TEXT), dx + dw - 24 - 240, by, 110, 36, NX_CENTER);
    add_rect(dx + dw - 24 - 240, by, 110, 36, K_SEC_CANCEL, 0);
}

/* Journal de sécurité : liste défilante (molette, flèches, Page haut/bas) */
#define JOURNAL_ROWS 22
static void draw_journal(cairo_t *cr, int width, int height)
{
    cairo_set_source_rgba(cr, 0.05, 0.07, 0.1, 0.35);
    cairo_paint(cr);
    add_rect(0, 0, width, height, K_SEC_BG, 0);
    double dx = 40, dy = 30, dw = width - 80, dh = height - 60;
    nx_rounded_rect(cr, dx, dy, dw, dh, 14);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    nx_text(cr, "Journal de sécurité", 17, true, nx_rgb(NX_TEXT), dx + 24, dy + 16, dw - 48, 28, NX_LEFT);
    /* lignes */
    const char *lines[4000];
    int n = 0;
    for (char *p = journal_text; p && *p && n < 4000;) {
        lines[n++] = p;
        char *e = strchr(p, '\n');
        if (!e) break;
        p = e + 1;
    }
    int max_top = n > JOURNAL_ROWS ? n - JOURNAL_ROWS : 0;
    if (journal_top > max_top) journal_top = max_top;
    if (journal_top < 0) journal_top = 0;
    char info[80];
    snprintf(info, sizeof info, "%d événement%s · du plus ancien au plus récent", n, n > 1 ? "s" : "");
    nx_text(cr, info, 12, false, nx_rgb(NX_SUBTLE), dx + 24, dy + 44, dw - 48, 18, NX_LEFT);
    double ly = dy + 72;
    for (int i = journal_top; i < n && i < journal_top + JOURNAL_ROWS; i++, ly += 21) {
        char buf[260];
        size_t l = strcspn(lines[i], "\n");
        snprintf(buf, sizeof buf, "%.*s", (int)(l < 259 ? l : 259), lines[i]);
        bool bad = strstr(buf, "ÉCHEC") || strstr(buf, "BLOCAGE");
        if (strlen(buf) > 19) {                  /* date en gris, événement ensuite */
            char date[20];
            snprintf(date, sizeof date, "%.19s", buf);
            nx_text(cr, date, 11.5, false, nx_rgb(NX_SUBTLE), dx + 24, ly, 140, 20, NX_LEFT);
            nx_text(cr, buf + 20, 12, bad, bad ? nx_rgb("#c0392b") : nx_rgb(NX_TEXT), dx + 166, ly, dw - 190, 20, NX_LEFT);
        } else nx_text(cr, buf, 12, false, nx_rgb(NX_TEXT), dx + 24, ly, dw - 48, 20, NX_LEFT);
    }
    double by = dy + dh - 52;
    nx_rounded_rect(cr, dx + dw - 24 - 120, by, 120, 36, 8);
    nx_set(cr, is_hover(K_SEC_CANCEL, 1) ? nx_mix(nx_accent_color(), nx_rgb("#ffffff"), 0.15) : nx_accent_color());
    cairo_fill(cr);
    nx_text(cr, "Fermer", 13.5, true, nx_rgb("#ffffff"), dx + dw - 24 - 120, by, 120, 36, NX_CENTER);
    add_rect(dx + dw - 24 - 120, by, 120, 36, K_SEC_CANCEL, 1);
}

/* ---------- Clavier ---------- */

/* Même liste que /usr/sbin/nexus-keymap (id, disposition XKB, variante, nom) */
static const struct { const char *id, *layout, *variant, *label; } kbs[] = {
    { "fr", "fr", "", "Français (AZERTY)" },     { "fr-bepo", "fr", "bepo", "Français (BÉPO)" },
    { "fr-mac", "fr", "mac", "Français (Mac)" },   { "be", "be", "", "Belge" },
    { "ch-fr", "ch", "fr", "Suisse romand" },      { "ch-de", "ch", "", "Suisse alémanique" },
    { "ca", "ca", "", "Canadien français" },       { "us", "us", "", "Anglais (US)" },
    { "gb", "gb", "", "Anglais (UK)" },            { "de", "de", "", "Allemand" },
    { "es", "es", "", "Espagnol" },                { "it", "it", "", "Italien" },
    { "pt", "pt", "", "Portugais" },
};
#define NKBS ((int)(sizeof kbs / sizeof kbs[0]))
static int kb_current = 0;
static char kb_test[96], kb_msg[160];

static void load_keyboard(void)
{
    char id[32] = "fr";
    FILE *f = fopen("/etc/keyboard", "r");
    if (f) { if (fgets(id, sizeof id, f)) id[strcspn(id, "\n")] = 0; fclose(f); }
    for (int i = 0; i < NKBS; i++)
        if (!strcmp(kbs[i].id, id)) kb_current = i;
}

static void set_keyboard(int i)
{
    /* 1. Système (consoles texte, écran de connexion) : via nexus-power,
     *    autorisé aux comptes du bureau (groupe video) */
    void (*old)(int) = signal(SIGCHLD, SIG_DFL);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execl("/usr/local/libexec/nexus-power", "nexus-power", "keymap", kbs[i].id, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    bool sys_ok = pid > 0 && waitpid(pid, &st, 0) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    signal(SIGCHLD, old);

    /* 2. Bureau : fichier d'environnement de labwc, relu sur SIGHUP */
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    char dir[512], path[600];
    snprintf(dir, sizeof dir, "%s/.config", home); mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.config/labwc", home); mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/environment", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "# Écrit par Paramètres → Clavier\n");
        fprintf(f, "XKB_DEFAULT_LAYOUT=%s\n", kbs[i].layout);
        fprintf(f, "XKB_DEFAULT_VARIANT=%s\n", kbs[i].variant);
        fclose(f);
    }
    const char *lp = getenv("LABWC_PID");
    if (lp && atoi(lp) > 1) kill(atoi(lp), SIGHUP);

    kb_current = i;
    kb_test[0] = 0;
    snprintf(kb_msg, sizeof kb_msg, sys_ok ? "Clavier « %s » appliqué au bureau et aux consoles texte."
                                          : "Clavier « %s » appliqué au bureau (consoles : non modifiées).",
             kbs[i].label);
}

static void draw_keyboard_section(cairo_t *cr, double x0, double cw)
{
    section_title(cr, x0, "Clavier", "Disposition des touches, pour le bureau et les consoles texte.");
    double gap = 10, w3 = (cw - 2 * gap) / 3, y = 96;
    for (int i = 0; i < NKBS; i++) {
        double x = x0 + (i % 3) * (w3 + gap), yy = y + (i / 3) * 52;
        choice_card(cr, x, yy, w3, 44, K_KB, i, i == kb_current, NULL);
        nx_text(cr, kbs[i].label, 13, i == kb_current, nx_rgb(NX_TEXT), x + 12, yy, w3 - 36, 44, NX_LEFT);
    }
    y += ((NKBS + 2) / 3) * 52 + 14;
    nx_text(cr, "Essaie ton clavier ici :", 13, true, nx_rgb(NX_TEXT), x0, y, cw, 22, NX_LEFT);
    nx_rounded_rect(cr, x0, y + 26, cw, 40, 10);
    nx_set(cr, nx_rgb("#f6f7fa"));
    cairo_fill_preserve(cr);
    nx_set(cr, nx_accent_color());
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    char shown[112];
    snprintf(shown, sizeof shown, "%s|", kb_test);
    nx_text(cr, kb_test[0] ? shown : "Tape quelques lettres : a z q w é è ç à ù…", 14, false,
            nx_rgb(kb_test[0] ? NX_TEXT : NX_SUBTLE), x0 + 14, y + 26, cw - 28, 40, NX_LEFT);
    if (kb_msg[0])
        nx_text(cr, kb_msg, 12.5, false, nx_rgb(NX_SUBTLE), x0, y + 76, cw, 22, NX_LEFT);
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    nx_color accent = nx_accent_color();

    /* Contenu */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    /* Barre latérale */
    cairo_rectangle(cr, 0, 0, SIDE_W, height);
    nx_set(cr, nx_rgb("#eef1f6"));
    cairo_fill(cr);
    nx_icon(cr, "settings", 22, 22, 34, nx_rgb(NX_TEXT));
    nx_text(cr, "Paramètres", 16, true, nx_rgb(NX_TEXT), 64, 22, SIDE_W - 70, 34, NX_LEFT);
    for (int i = 0; i < SEC_COUNT; i++) {
        double y = 74 + i * 41;
        bool sel = i == section;
        if (sel || is_hover(K_NAV, i)) {
            nx_rounded_rect(cr, 10, y, SIDE_W - 20, 40, 8);
            nx_set(cr, sel ? nx_rgb("#ffffff") : nx_rgb("#e3e7ef"));
            cairo_fill(cr);
        }
        if (sel) {
            nx_rounded_rect(cr, 10, y + 9, 4, 22, 2);
            nx_set(cr, accent);
            cairo_fill(cr);
        }
        nx_icon(cr, sections[i].icon, 26, y + 9, 22, nx_rgb(NX_TEXT));
        nx_text(cr, sections[i].label, 14, sel, nx_rgb(NX_TEXT), 60, y, SIDE_W - 70, 40, NX_LEFT);
        add_rect(10, y, SIDE_W - 20, 40, K_NAV, i);
    }

    double x0 = SIDE_W + PAD, cw = width - SIDE_W - 2 * PAD;
    switch (section) {
    case SEC_WALL: draw_wallpapers(cr, x0, cw); break;
    case SEC_COLORS: draw_colors(cr, x0, cw); break;
    case SEC_PANEL: draw_panel_section(cr, x0, cw); break;
    case SEC_ICONS: draw_icons_section(cr, x0, cw); break;
    case SEC_WIDGETS: draw_widgets_section(cr, x0, cw); break;
    case SEC_ACCOUNTS: draw_accounts_section(cr, x0, cw); break;
    case SEC_DISPLAY: draw_display(cr, x0, cw); break;
    case SEC_KEYBOARD: draw_keyboard_section(cr, x0, cw); break;
    case SEC_THEME: draw_theme_section(cr, x0, cw); break;
    case SEC_LOCK: draw_lock_section(cr, x0, cw); break;
    case SEC_SECURITY: draw_security_section(cr, x0, cw); break;
    case SEC_PROTECTION: draw_protection_section(cr, x0, cw); break;
    case SEC_ABOUT: draw_about(cr, x0, cw); break;
    }
    if (dlg) draw_account_dialog(cr, width, height);
    if (sec_dlg) draw_sec_dialog(cr, width, height);
    if (journal_text) draw_journal(cr, width, height);
}

/* ---------- Souris ---------- */

static struct rect *hit(double x, double y)
{
    for (int i = nrects - 1; i >= 0; i--)
        if (x >= rects[i].x && x < rects[i].x + rects[i].w &&
            y >= rects[i].y && y < rects[i].y + rects[i].h)
            return &rects[i];
    return NULL;
}

static void click(struct rect *r)
{
    switch (r->kind) {
    case K_NAV:
        section = r->index;
        if (section == SEC_ACCOUNTS) load_accounts();   /* comptes créés ailleurs entre-temps */
        if (section == SEC_SECURITY || section == SEC_PROTECTION) { load_firewall(); load_protection(); }
        break;
    case K_WALL:
        snprintf(nx_conf.wallpaper, sizeof nx_conf.wallpaper, "%s", walls[r->index].spec);
        apply();
        break;
    case K_ACCENT:
        snprintf(nx_conf.accent, sizeof nx_conf.accent, "%s", nx_accents[r->index].hex);
        apply();
        apply_window_theme();
        break;
    case K_PANEL: nx_conf.panel_height = panel_heights[r->index]; apply(); break;
    case K_MENU: nx_conf.menu_scale = menu_scales[r->index]; apply(); break;
    case K_WSIZE: nx_conf.widget_scale = (int[]){ 80, 100, 130, 160 }[r->index]; apply(); break;
    case K_WSTYLE: nx_conf.widget_style = r->index; apply(); break;
    case K_WRESET: nx_conf.widget_mr = nx_conf.widget_mt = 0; apply(); break;
    case K_ACC_ADD: open_dialog(1, 0); break;
    case K_ACC_PW: open_dialog(2, r->index); break;
    case K_ACC_DEL: open_dialog(3, r->index); break;
    case K_DLG_FIELD: dlg_focus = r->index; break;
    case K_DLG_WIPE: dlg_wipe = !dlg_wipe; break;
    case K_DLG_OK: dialog_submit(); break;
    case K_DLG_CANCEL: close_dialog(); break;
    case K_KB: set_keyboard(r->index); break;
    case K_THEME: nx_conf.theme = r->index; apply(); apply_window_theme(); break;
    case K_LOCKDELAY: nx_conf.lock_minutes = lock_delays[r->index]; apply(); break;
    case K_LOCKNOW: nx_spawn("nexus-lock"); break;
    case K_FW: {
        static const char *on[] = { "on", "ssh-on", "ping-on" }, *off[] = { "off", "ssh-off", "ping-off" };
        bool cur = r->index == 0 ? fw_active : r->index == 1 ? fw_ssh : fw_ping;
        sec_request(cur ? off[r->index] : on[r->index]);
        break;
    }
    case K_SEC_OK: sec_submit(); break;
    case K_SEC_CANCEL:
        if (r->index == 1) { free(journal_text); journal_text = NULL; }
        else sec_close();
        break;
    case K_SEC_BG: break;
    case K_SEC_FIELD: sd_focus = r->index; break;
    case K_BLOCAGE: sec_open(SD_BLOCAGE, prot.blocage ? "off" : "on"); break;
    case K_JOURNAL: sec_open(SD_JOURNAL, NULL); break;
    case K_CRYPT:
        if (r->index == 0) sec_open(SD_CRYPT_ON, NULL);
        else if (r->index == 2) sec_open(SD_CRYPT_OFF, NULL);
        else if (r->index == 3) sec_open(SD_CRYPT_PHRASE, NULL);
        else nx_spawn("/usr/local/libexec/nexus-power reboot");
        break;
    case K_ISIZE: nx_conf.icon_size = r->index; apply(); break;
    case K_ISTYLE: nx_conf.icon_style = r->index; apply(); break;
    case K_ISHAPE: nx_conf.icon_shape = r->index; nx_conf.icon_style = 0; apply(); break;
    case K_OPACITY: nx_conf.glass_opacity = r->index; apply(); break;
    case K_TINT: nx_conf.glass_tint = !nx_conf.glass_tint; apply(); break;
    case K_DSCALE:
        nx_conf.display_scale = (int[]){ 0, 100, 125, 150, 175, 200 }[r->index];
        apply();
        nx_spawn("nexus-display");   /* tout l'écran change de taille tout de suite */
        break;
    case K_SCALE:
        nx_conf.text_scale = (int[]){ 100, 125, 150 }[r->index];
        nx_scale = nx_conf.text_scale / 100.0;
        apply();
        break;
    case K_TOGGLE:
        if (r->index == 0) {
            nx_conf.widgets = !nx_conf.widgets;
            apply();                                    /* les widgets se ferment d'eux-mêmes... */
            if (nx_conf.widgets) nx_spawn("nexus-widgets");   /* ...ou on les relance */
        } else {
            nx_conf.desktop_icons = !nx_conf.desktop_icons;
            apply();
        }
        break;
    }
    nx_redraw(win);
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct rect *r = hit(x, y);
    switch (ev) {
    case NX_MOTION: {
        int k = r ? (int)r->kind : -1, i = r ? r->index : -1;
        if (k != hover_kind || i != hover_index) {
            hover_kind = k; hover_index = i;
            nx_redraw(w);
        }
        break;
    }
    case NX_LEAVE:
        hover_kind = hover_index = -1;
        nx_redraw(w);
        break;
    case NX_RELEASE:
        if (r && value == BTN_LEFT) click(r);
        break;
    case NX_SCROLL:
        if (journal_text) { journal_top += value * 3; nx_redraw(w); }
        break;
    default:
        break;
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (journal_text) {
        if (sym == XKB_KEY_Escape || sym == XKB_KEY_Return) { free(journal_text); journal_text = NULL; }
        else if (sym == XKB_KEY_Up) journal_top--;
        else if (sym == XKB_KEY_Down) journal_top++;
        else if (sym == XKB_KEY_Page_Up) journal_top -= JOURNAL_ROWS - 2;
        else if (sym == XKB_KEY_Page_Down) journal_top += JOURNAL_ROWS - 2;
        else if (sym == XKB_KEY_Home) journal_top = 0;
        else if (sym == XKB_KEY_End) journal_top = 1 << 20;
        nx_redraw(w);
        return;
    }
    if (sec_dlg) {
        char *f = sd_f[sd_focus];
        if (sym == XKB_KEY_Escape) sec_close();
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
            if (sd_focus < sd_nf - 1 && !sd_f[sd_focus + 1][0]) sd_focus++;   /* champ suivant */
            else sec_submit();
        } else if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
            sd_focus += nx_mod_shift() || sym == XKB_KEY_ISO_Left_Tab ? -1 : 1;
            if (sd_focus >= sd_nf) sd_focus = sd_first;
            if (sd_focus < sd_first) sd_focus = sd_nf - 1;
        } else if (sym == XKB_KEY_BackSpace) {
            size_t l = strlen(f);
            while (l > 0 && ((unsigned char)f[l - 1] & 0xC0) == 0x80) l--;
            if (l > 0) l--;
            f[l] = 0;
        } else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f && strlen(f) + strlen(utf8) < sizeof sd_f[0] - 1)
            strcat(f, utf8);
        nx_redraw(w);
        return;
    }
    if (dialog_key(sym, utf8)) { nx_redraw(w); return; }
    if (section == SEC_KEYBOARD && sym == XKB_KEY_BackSpace && kb_test[0]) {
        size_t l = strlen(kb_test);
        while (l > 0 && ((unsigned char)kb_test[l - 1] & 0xC0) == 0x80) l--;   /* caractère UTF-8 entier */
        kb_test[l > 0 ? l - 1 : 0] = 0;
        nx_redraw(w);
        return;
    }
    if (section == SEC_KEYBOARD && utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
        strlen(kb_test) + strlen(utf8) < sizeof kb_test - 1) {
        strcat(kb_test, utf8);
        nx_redraw(w);
        return;
    }
    if (sym == XKB_KEY_Escape) nx_quit();
    else if (sym == XKB_KEY_Down || sym == XKB_KEY_Tab) section = (section + 1) % SEC_COUNT;
    else if (sym == XKB_KEY_Up) section = (section + SEC_COUNT - 1) % SEC_COUNT;
    else return;
    nx_redraw(w);
}

int settings_main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wallpaper")) section = SEC_WALL;
        else if (!strcmp(argv[i], "--colors")) section = SEC_COLORS;
        else if (!strcmp(argv[i], "--display")) section = SEC_DISPLAY;
        else if (!strcmp(argv[i], "--panel")) section = SEC_PANEL;
        else if (!strcmp(argv[i], "--icons")) section = SEC_ICONS;
        else if (!strcmp(argv[i], "--widgets")) section = SEC_WIDGETS;
        else if (!strcmp(argv[i], "--accounts")) section = SEC_ACCOUNTS;
        else if (!strcmp(argv[i], "--keyboard")) section = SEC_KEYBOARD;
        else if (!strcmp(argv[i], "--theme")) section = SEC_THEME;
        else if (!strcmp(argv[i], "--lock")) section = SEC_LOCK;
        else if (!strcmp(argv[i], "--security")) section = SEC_SECURITY;
        else if (!strcmp(argv[i], "--protection")) section = SEC_PROTECTION;
        else if (!strcmp(argv[i], "--about")) section = SEC_ABOUT;
    }
    if (!nx_init()) return 1;
    load_walls();
    load_info();
    load_accounts();
    load_keyboard();
    load_firewall();
    load_protection();
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_toplevel_window(h, "Paramètres", "nexus-settings", WIN_W, WIN_H);
    if (!win) return 1;
    nx_run();
    return 0;
}
