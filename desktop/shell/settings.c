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
#include "nexus-ui.h"

#include <ctype.h>
#include <dirent.h>
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

enum { SEC_WALL, SEC_COLORS, SEC_PANEL, SEC_ICONS, SEC_WIDGETS, SEC_DISPLAY, SEC_ACCOUNTS, SEC_ABOUT,
       SEC_COUNT };
static const struct { const char *label, *icon; } sections[SEC_COUNT] = {
    { "Fond d'écran", "image" },     { "Couleurs", "palette" },
    { "Barre et menu", "panel" },    { "Icônes", "apps" },
    { "Widgets", "monitor" },        { "Affichage", "display" },
    { "Comptes", "user" },           { "À propos", "info" },
};
static int section = SEC_WALL;

/* ---------- Zones cliquables (recalculées à chaque dessin) ---------- */

enum kind { K_NAV, K_WALL, K_ACCENT, K_SCALE, K_TOGGLE, K_PANEL, K_ISIZE, K_ISTYLE, K_ISHAPE,
            K_OPACITY, K_TINT, K_MENU, K_WSIZE, K_WSTYLE, K_WRESET,
            K_ACC_ADD, K_ACC_PW, K_ACC_DEL, K_DLG_FIELD, K_DLG_OK, K_DLG_CANCEL, K_DLG_WIPE };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[96];
static int nrects;
static int hover_kind = -1, hover_index = -1;

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 96) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
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
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".png")) continue;
        struct wall *w = &walls[nwalls++];
        snprintf(w->spec, sizeof w->spec, "%s/%s", dir, e->d_name);
        snprintf(w->name, sizeof w->name, "%.*s", (int)(n - 4), e->d_name);
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

/* Bouton texte (contour d'accent) */
static void button_row(cairo_t *cr, double x, double y, const char *label, enum kind k)
{
    nx_color a = nx_accent_color();
    double w = nx_text_width(cr, label, 13.5, true) + 36;
    nx_rounded_rect(cr, x, y, w, 38, 19);
    nx_set(cr, is_hover(k, 0) ? nx_alpha(a, 0.12) : nx_rgb("#ffffff"));
    cairo_fill_preserve(cr);
    nx_set(cr, a);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    nx_text(cr, label, 13.5, true, a, x, y, w, 38, NX_CENTER);
    add_rect(x, y, w, 38, k, 0);
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
    section_title(cr, x0, "Affichage", "Taille du texte et éléments du bureau.");
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
        double y = 84 + i * 48;
        bool sel = i == section;
        if (sel || is_hover(K_NAV, i)) {
            nx_rounded_rect(cr, 10, y, SIDE_W - 20, 42, 8);
            nx_set(cr, sel ? nx_rgb("#ffffff") : nx_rgb("#e3e7ef"));
            cairo_fill(cr);
        }
        if (sel) {
            nx_rounded_rect(cr, 10, y + 10, 4, 22, 2);
            nx_set(cr, accent);
            cairo_fill(cr);
        }
        nx_icon(cr, sections[i].icon, 26, y + 10, 22, nx_rgb(NX_TEXT));
        nx_text(cr, sections[i].label, 14, sel, nx_rgb(NX_TEXT), 60, y, SIDE_W - 70, 42, NX_LEFT);
        add_rect(10, y, SIDE_W - 20, 42, K_NAV, i);
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
    case SEC_ABOUT: draw_about(cr, x0, cw); break;
    }
    if (dlg) draw_account_dialog(cr, width, height);
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
    case K_ISIZE: nx_conf.icon_size = r->index; apply(); break;
    case K_ISTYLE: nx_conf.icon_style = r->index; apply(); break;
    case K_ISHAPE: nx_conf.icon_shape = r->index; nx_conf.icon_style = 0; apply(); break;
    case K_OPACITY: nx_conf.glass_opacity = r->index; apply(); break;
    case K_TINT: nx_conf.glass_tint = !nx_conf.glass_tint; apply(); break;
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
    default:
        break;
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (dialog_key(sym, utf8)) { nx_redraw(w); return; }
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
        else if (!strcmp(argv[i], "--about")) section = SEC_ABOUT;
    }
    if (!nx_init()) return 1;
    load_walls();
    load_info();
    load_accounts();
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_toplevel_window(h, "Paramètres", "nexus-settings", WIN_W, WIN_H);
    if (!win) return 1;
    nx_run();
    return 0;
}
