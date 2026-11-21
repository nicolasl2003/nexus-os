/* nexus-ui — implémentation (voir nexus-ui.h) */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <dirent.h>
#include <setjmp.h>
#include <stdint.h>
#include <strings.h>
#include <jpeglib.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <wayland-cursor.h>

struct nx_state nx;
struct nx_config nx_conf;
double nx_scale = 1.0;
static volatile sig_atomic_t reload_flag, quit_flag;

/* ===================== Couleurs ===================== */

nx_color nx_rgb(const char *hex)
{
    unsigned r = 0, g = 0, b = 0;
    if (hex && hex[0] == '#')
        sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
    return (nx_color){ r / 255.0, g / 255.0, b / 255.0, 1.0 };
}
nx_color nx_alpha(nx_color c, double a) { c.a = a; return c; }
nx_color nx_mix(nx_color a, nx_color b, double t)
{
    return (nx_color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                       a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}
void nx_set(cairo_t *cr, nx_color c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

/* ===================== Configuration ===================== */

const struct nx_accent nx_accents[] = {
    { "#1e6fe0", "Bleu Nexus" }, { "#00a3c4", "Cyan" },   { "#7a4fd8", "Violet" },
    { "#2f9e5a", "Vert" },       { "#e0782a", "Orange" }, { "#d8457a", "Rose" },
    { "#5a6170", "Graphite" },   { "#d83b3b", "Rouge" },  { "#c9a227", "Or" },
    { "#0f9d8a", "Émeraude" },   { "#3b4cc0", "Indigo" }, { "#8a5a3c", "Cacao" },
};
const int nx_accent_count = sizeof(nx_accents) / sizeof(nx_accents[0]);

static void config_path(char *buf, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/nexus/desktop.conf", xdg);
    else snprintf(buf, n, "%s/.config/nexus/desktop.conf", getenv("HOME") ? getenv("HOME") : "/tmp");
}

static void config_read(struct nx_config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "wallpaper")) snprintf(c->wallpaper, sizeof c->wallpaper, "%s", v);
        else if (!strcmp(k, "accent") && v[0] == '#') snprintf(c->accent, sizeof c->accent, "%s", v);
        else if (!strcmp(k, "text_scale")) c->text_scale = atoi(v);
        else if (!strcmp(k, "widgets")) c->widgets = atoi(v) != 0;
        else if (!strcmp(k, "desktop_icons")) c->desktop_icons = atoi(v) != 0;
        else if (!strcmp(k, "panel_size")) c->panel_size = atoi(v);
        else if (!strcmp(k, "panel_height")) c->panel_height = atoi(v);
        else if (!strcmp(k, "menu_scale")) c->menu_scale = atoi(v);
        else if (!strcmp(k, "widget_scale")) c->widget_scale = atoi(v);
        else if (!strcmp(k, "widget_style")) c->widget_style = atoi(v);
        else if (!strcmp(k, "widget_mr")) c->widget_mr = atoi(v);
        else if (!strcmp(k, "widget_mt")) c->widget_mt = atoi(v);
        else if (!strcmp(k, "icon_size")) c->icon_size = atoi(v);
        else if (!strcmp(k, "icon_style")) c->icon_style = atoi(v);
        else if (!strcmp(k, "icon_shape")) c->icon_shape = atoi(v);
        else if (!strcmp(k, "glass_opacity")) c->glass_opacity = atoi(v);
        else if (!strcmp(k, "glass_tint")) c->glass_tint = atoi(v) != 0;
    }
    fclose(f);
}

void nx_config_load(struct nx_config *c)
{
    snprintf(c->wallpaper, sizeof c->wallpaper, "preset:montagne");
    c->widgets = true;
    c->desktop_icons = true;
    snprintf(c->accent, sizeof c->accent, "#1e6fe0");
    c->text_scale = 100;
    c->panel_size = 1;
    c->panel_height = 0;
    c->menu_scale = 100;
    c->widget_scale = 100;
    c->widget_style = 0;
    c->widget_mr = c->widget_mt = 0;
    c->icon_size = 1;
    c->icon_style = 0;
    c->icon_shape = 0;
    c->glass_opacity = 1;
    c->glass_tint = false;
    config_read(c, "/usr/local/etc/nexus/desktop.conf");   /* réglages système */
    char p[512];
    config_path(p, sizeof p);
    config_read(c, p);                                      /* réglages de l'utilisateur */
    if (c->text_scale < 80 || c->text_scale > 200) c->text_scale = 100;
    if (c->panel_size < 0 || c->panel_size > 2) c->panel_size = 1;
    if (c->panel_height == 0) c->panel_height = (int[]){ 42, 52, 64 }[c->panel_size];
    if (c->panel_height < 36) c->panel_height = 36;
    if (c->panel_height > 110) c->panel_height = 110;
    if (c->menu_scale < 80 || c->menu_scale > 170) c->menu_scale = 100;
    if (c->widget_scale < 70 || c->widget_scale > 170) c->widget_scale = 100;
    if (c->widget_style < 0 || c->widget_style > 3) c->widget_style = 0;
    if (c->widget_mr < 0 || c->widget_mr > 8000) c->widget_mr = 0;
    if (c->widget_mt < 0 || c->widget_mt > 8000) c->widget_mt = 0;
    if (c->icon_size < 0 || c->icon_size > 2) c->icon_size = 1;
    if (c->icon_style < 0 || c->icon_style > 1) c->icon_style = 0;
    if (c->icon_shape < 0 || c->icon_shape > 2) c->icon_shape = 0;
    if (c->glass_opacity < 0 || c->glass_opacity > 2) c->glass_opacity = 1;
    if (c == &nx_conf) nx_scale = c->text_scale / 100.0;
}

int nx_config_save(const struct nx_config *c)
{
    char p[512], dir[512];
    config_path(p, sizeof p);
    snprintf(dir, sizeof dir, "%s", p);
    *strrchr(dir, '/') = 0;
    char parent[512];
    snprintf(parent, sizeof parent, "%s", dir);
    *strrchr(parent, '/') = 0;
    mkdir(parent, 0755);
    mkdir(dir, 0755);
    char tmp[520];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# Réglages du bureau Nexus (modifiés par l'application Paramètres)\n");
    fprintf(f, "wallpaper=%s\naccent=%s\ntext_scale=%d\nwidgets=%d\ndesktop_icons=%d\n",
            c->wallpaper, c->accent, c->text_scale, c->widgets, c->desktop_icons);
    fprintf(f, "panel_size=%d\nicon_size=%d\nicon_style=%d\nicon_shape=%d\n",
            c->panel_size, c->icon_size, c->icon_style, c->icon_shape);
    fprintf(f, "glass_opacity=%d\nglass_tint=%d\n", c->glass_opacity, c->glass_tint);
    fprintf(f, "panel_height=%d\nmenu_scale=%d\n", c->panel_height, c->menu_scale);
    fprintf(f, "widget_scale=%d\nwidget_style=%d\nwidget_mr=%d\nwidget_mt=%d\n",
            c->widget_scale, c->widget_style, c->widget_mr, c->widget_mt);
    fclose(f);
    return rename(tmp, p);
}

nx_color nx_accent_color(void) { return nx_rgb(nx_conf.accent); }

int nx_panel_height(void) { return nx_conf.panel_height; }

double nx_icon_factor(void)
{
    static const double f[3] = { 0.82, 1.0, 1.2 };
    return f[nx_conf.icon_size];
}

void nx_glass_source(cairo_t *cr, double y0, double y1)
{
    static const double alpha[3][2] = { { 0.62, 0.70 }, { 0.86, 0.90 }, { 0.98, 1.0 } };
    nx_color top = { 0.11, 0.13, 0.22, 1 }, bottom = { 0.06, 0.07, 0.13, 1 };
    if (nx_conf.glass_tint) {
        nx_color a = nx_accent_color();
        top = nx_mix(top, a, 0.38);
        bottom = nx_mix(bottom, a, 0.22);
    }
    const double *al = alpha[nx_conf.glass_opacity];
    cairo_pattern_t *g = cairo_pattern_create_linear(0, y0, 0, y1);
    cairo_pattern_add_color_stop_rgba(g, 0, top.r, top.g, top.b, al[0]);
    cairo_pattern_add_color_stop_rgba(g, 1, bottom.r, bottom.g, bottom.b, al[1]);
    cairo_set_source(cr, g);
    cairo_pattern_destroy(g);
}

/* ===================== Dessin ===================== */

void nx_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void nx_glass(cairo_t *cr, double x, double y, double w, double h, double r)
{
    for (int i = 5; i > 0; i--) {                 /* ombre douce */
        nx_rounded_rect(cr, x - i, y - i + 3, w + 2 * i, h + 2 * i, r + i);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.035);
        cairo_fill(cr);
    }
    nx_rounded_rect(cr, x, y, w, h, r);
    nx_glass_source(cr, y, y + h);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

static PangoLayout *make_layout(cairo_t *cr, const char *txt, double size, bool bold)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, "DejaVu Sans");
    pango_font_description_set_absolute_size(fd, size * nx_scale * PANGO_SCALE);
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, txt, -1);
    return l;
}

double nx_text(cairo_t *cr, const char *txt, double size, bool bold, nx_color c,
               double x, double y, double w, double h, int align)
{
    PangoLayout *l = make_layout(cr, txt, size, bold);
    if (w > 0) {
        pango_layout_set_width(l, (int)(w * PANGO_SCALE));
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment(l, align == NX_CENTER ? PANGO_ALIGN_CENTER :
                                      align == NX_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    }
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(l, &ink, &logical);
    nx_set(cr, c);
    cairo_move_to(cr, x, h > 0 ? y + (h - logical.height) / 2.0 : y);
    pango_cairo_show_layout(cr, l);
    cairo_new_path(cr);   /* sinon la forme suivante serait reliée au texte par un trait */
    g_object_unref(l);
    return logical.width;
}

double nx_text_width(cairo_t *cr, const char *txt, double size, bool bold)
{
    PangoLayout *l = make_layout(cr, txt, size, bold);
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    g_object_unref(l);
    return w;
}

/* Logo Nexus : carré arrondi en dégradé cyan -> bleu, "N" blanc (comme au démarrage) */
void nx_logo(cairo_t *cr, double x, double y, double s)
{
    double k = s / 96.0;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, k, k);
    nx_rounded_rect(cr, 3, 3, 90, 90, 20);
    cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, 96);
    cairo_pattern_add_color_stop_rgb(p, 0, 0, 210 / 255.0, 235 / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, 40 / 255.0, 80 / 255.0, 225 / 255.0);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, 26, 24, 12, 48);
    cairo_rectangle(cr, 58, 24, 12, 48);
    cairo_move_to(cr, 26, 24); cairo_line_to(cr, 39, 24);
    cairo_line_to(cr, 70, 72); cairo_line_to(cr, 57, 72);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* Icônes vectorielles : dessinées dans un carré de taille s (unités 0..1) */
void nx_icon(cairo_t *cr, const char *name, double x, double y, double s, nx_color fg)
{
    nx_color accent = nx_accent_color();
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (!strcmp(name, "nexus")) {
        cairo_restore(cr);
        nx_logo(cr, x, y, s);
        return;
    } else if (!strcmp(name, "terminal")) {
        nx_rounded_rect(cr, 0.06, 0.12, 0.88, 0.76, 0.12);
        cairo_set_source_rgb(cr, 0.16, 0.18, 0.23); cairo_fill(cr);
        nx_rounded_rect(cr, 0.06, 0.12, 0.88, 0.16, 0.08);
        cairo_set_source_rgb(cr, 0.30, 0.33, 0.40); cairo_fill(cr);
        cairo_set_line_width(cr, 0.07);
        cairo_move_to(cr, 0.22, 0.42); cairo_line_to(cr, 0.36, 0.55); cairo_line_to(cr, 0.22, 0.68);
        cairo_set_source_rgb(cr, 0.25, 0.85, 0.55); cairo_stroke(cr);
        cairo_move_to(cr, 0.44, 0.68); cairo_line_to(cr, 0.68, 0.68);
        cairo_set_source_rgb(cr, 0.9, 0.92, 0.95); cairo_stroke(cr);
    } else if (!strcmp(name, "settings")) {
        nx_set(cr, nx_mix(accent, nx_rgb("#5a6170"), 0.35));
        for (int i = 0; i < 8; i++) {          /* dents de l'engrenage */
            cairo_save(cr);
            cairo_translate(cr, 0.5, 0.5);
            cairo_rotate(cr, i * M_PI / 4);
            cairo_rectangle(cr, -0.08, -0.46, 0.16, 0.18);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        cairo_arc(cr, 0.5, 0.5, 0.32, 0, 2 * M_PI); cairo_fill(cr);
        cairo_arc(cr, 0.5, 0.5, 0.13, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 1, 1, 1); cairo_fill(cr);
    } else if (!strcmp(name, "monitor")) {
        nx_rounded_rect(cr, 0.06, 0.14, 0.88, 0.72, 0.1);
        cairo_set_source_rgb(cr, 0.12, 0.14, 0.19); cairo_fill(cr);
        cairo_set_line_width(cr, 0.06);
        cairo_move_to(cr, 0.16, 0.62); cairo_line_to(cr, 0.32, 0.62); cairo_line_to(cr, 0.42, 0.34);
        cairo_line_to(cr, 0.55, 0.72); cairo_line_to(cr, 0.65, 0.48); cairo_line_to(cr, 0.84, 0.48);
        nx_set(cr, nx_rgb("#3fd0a0")); cairo_stroke(cr);
    } else if (!strcmp(name, "editor")) {
        cairo_move_to(cr, 0.18, 0.06); cairo_line_to(cr, 0.62, 0.06); cairo_line_to(cr, 0.82, 0.26);
        cairo_line_to(cr, 0.82, 0.94); cairo_line_to(cr, 0.18, 0.94); cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.97, 0.97, 0.98); cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 0.03); cairo_set_source_rgb(cr, 0.6, 0.63, 0.7); cairo_stroke(cr);
        cairo_set_line_width(cr, 0.05); nx_set(cr, accent);
        for (int i = 0; i < 4; i++) {
            cairo_move_to(cr, 0.3, 0.42 + i * 0.13);
            cairo_line_to(cr, i == 3 ? 0.55 : 0.7, 0.42 + i * 0.13);
        }
        cairo_stroke(cr);
    } else if (!strcmp(name, "info")) {
        cairo_arc(cr, 0.5, 0.5, 0.44, 0, 2 * M_PI); nx_set(cr, accent); cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_arc(cr, 0.5, 0.3, 0.06, 0, 2 * M_PI); cairo_fill(cr);
        cairo_set_line_width(cr, 0.11);
        cairo_move_to(cr, 0.5, 0.46); cairo_line_to(cr, 0.5, 0.72); cairo_stroke(cr);
    } else if (!strcmp(name, "power")) {
        nx_set(cr, fg); cairo_set_line_width(cr, 0.1);
        cairo_arc(cr, 0.5, 0.54, 0.32, -M_PI / 2 + 0.6, 3 * M_PI / 2 - 0.6); cairo_stroke(cr);
        cairo_move_to(cr, 0.5, 0.12); cairo_line_to(cr, 0.5, 0.5); cairo_stroke(cr);
    } else if (!strcmp(name, "reboot")) {
        nx_set(cr, fg); cairo_set_line_width(cr, 0.1);
        cairo_arc(cr, 0.5, 0.52, 0.32, -M_PI / 2 + 0.3, 2 * M_PI - 0.5); cairo_stroke(cr);
        cairo_move_to(cr, 0.56, 0.06); cairo_line_to(cr, 0.70, 0.2); cairo_line_to(cr, 0.52, 0.30);
        cairo_close_path(cr); cairo_fill_preserve(cr); cairo_stroke(cr);
    } else if (!strcmp(name, "logout")) {
        nx_set(cr, fg); cairo_set_line_width(cr, 0.09);
        cairo_move_to(cr, 0.46, 0.14); cairo_line_to(cr, 0.16, 0.14); cairo_line_to(cr, 0.16, 0.86);
        cairo_line_to(cr, 0.46, 0.86); cairo_stroke(cr);
        cairo_move_to(cr, 0.38, 0.5); cairo_line_to(cr, 0.86, 0.5); cairo_stroke(cr);
        cairo_move_to(cr, 0.7, 0.34); cairo_line_to(cr, 0.86, 0.5); cairo_line_to(cr, 0.7, 0.66); cairo_stroke(cr);
    } else if (!strcmp(name, "user")) {
        cairo_arc(cr, 0.5, 0.5, 0.48, 0, 2 * M_PI);
        nx_set(cr, nx_mix(accent, nx_rgb("#ffffff"), 0.2)); cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
        cairo_arc(cr, 0.5, 0.38, 0.16, 0, 2 * M_PI); cairo_fill(cr);
        cairo_arc(cr, 0.5, 0.86, 0.3, M_PI, 2 * M_PI); cairo_fill(cr);
    } else if (!strcmp(name, "image")) {
        nx_rounded_rect(cr, 0.08, 0.16, 0.84, 0.68, 0.08); nx_set(cr, nx_rgb("#2c7be5")); cairo_fill(cr);
        cairo_move_to(cr, 0.14, 0.78); cairo_line_to(cr, 0.4, 0.46); cairo_line_to(cr, 0.58, 0.66);
        cairo_line_to(cr, 0.7, 0.54); cairo_line_to(cr, 0.86, 0.78); cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.85, 0.95, 1); cairo_fill(cr);
        cairo_arc(cr, 0.7, 0.34, 0.07, 0, 2 * M_PI); cairo_set_source_rgb(cr, 1, 0.85, 0.3); cairo_fill(cr);
    } else if (!strcmp(name, "palette")) {
        const char *cols[] = { "#e0482a", "#f2b84b", "#2f9e5a", "#1e6fe0", "#7a4fd8" };
        for (int i = 0; i < 5; i++) {
            double a = -M_PI / 2 + i * 2 * M_PI / 5;
            cairo_arc(cr, 0.5 + 0.28 * cos(a), 0.5 + 0.28 * sin(a), 0.16, 0, 2 * M_PI);
            nx_set(cr, nx_rgb(cols[i])); cairo_fill(cr);
        }
    } else if (!strcmp(name, "panel")) {
        /* écran avec sa barre des tâches */
        nx_set(cr, fg); cairo_set_line_width(cr, 0.08);
        nx_rounded_rect(cr, 0.08, 0.14, 0.84, 0.72, 0.1); cairo_stroke(cr);
        cairo_rectangle(cr, 0.08, 0.66, 0.84, 0.2); cairo_fill(cr);
    } else if (!strcmp(name, "apps")) {
        nx_set(cr, fg);
        for (int i = 0; i < 4; i++) {
            nx_rounded_rect(cr, 0.12 + (i % 2) * 0.42, 0.12 + (i / 2) * 0.42, 0.34, 0.34, 0.08);
            cairo_fill(cr);
        }
    } else if (!strcmp(name, "search")) {
        nx_set(cr, fg); cairo_set_line_width(cr, 0.1);
        cairo_arc(cr, 0.42, 0.42, 0.27, 0, 2 * M_PI); cairo_stroke(cr);
        cairo_move_to(cr, 0.62, 0.62); cairo_line_to(cr, 0.88, 0.88); cairo_stroke(cr);
    } else if (!strcmp(name, "network")) {
        nx_set(cr, fg); cairo_set_line_width(cr, 0.09);
        for (int i = 0; i < 3; i++) {
            cairo_arc(cr, 0.5, 0.82, 0.2 + i * 0.2, -M_PI * 0.78, -M_PI * 0.22);
            cairo_stroke(cr);
        }
        cairo_arc(cr, 0.5, 0.82, 0.07, 0, 2 * M_PI); cairo_fill(cr);
    } else if (!strncmp(name, "volume", 6)) {
        /* "volume-0".."volume-3" (niveau) ou "volume-mute" */
        nx_set(cr, fg);
        cairo_move_to(cr, 0.10, 0.38); cairo_line_to(cr, 0.28, 0.38); cairo_line_to(cr, 0.50, 0.18);
        cairo_line_to(cr, 0.50, 0.82); cairo_line_to(cr, 0.28, 0.62); cairo_line_to(cr, 0.10, 0.62);
        cairo_close_path(cr); cairo_fill(cr);
        cairo_set_line_width(cr, 0.07);
        if (!strcmp(name, "volume-mute")) {
            cairo_move_to(cr, 0.64, 0.36); cairo_line_to(cr, 0.88, 0.64);
            cairo_move_to(cr, 0.88, 0.36); cairo_line_to(cr, 0.64, 0.64);
            cairo_stroke(cr);
        } else {
            int lvl = name[6] == '-' ? name[7] - '0' : 3;
            for (int i = 0; i < lvl && i < 3; i++) {
                cairo_new_path(cr);
                cairo_arc(cr, 0.5, 0.5, 0.16 + i * 0.13, -0.8, 0.8);
                cairo_stroke(cr);
            }
        }
    } else if (!strcmp(name, "ethernet")) {
        nx_set(cr, fg); cairo_set_line_width(cr, 0.07);
        nx_rounded_rect(cr, 0.36, 0.12, 0.28, 0.22, 0.04); cairo_fill(cr);
        nx_rounded_rect(cr, 0.08, 0.64, 0.26, 0.22, 0.04); cairo_fill(cr);
        nx_rounded_rect(cr, 0.66, 0.64, 0.26, 0.22, 0.04); cairo_fill(cr);
        cairo_move_to(cr, 0.5, 0.34); cairo_line_to(cr, 0.5, 0.5);
        cairo_move_to(cr, 0.21, 0.64); cairo_line_to(cr, 0.21, 0.5); cairo_line_to(cr, 0.79, 0.5);
        cairo_line_to(cr, 0.79, 0.64);
        cairo_stroke(cr);
    } else if (!strcmp(name, "lock")) {
        nx_set(cr, fg); cairo_set_line_width(cr, 0.1);
        cairo_new_path(cr); cairo_arc(cr, 0.5, 0.42, 0.2, M_PI, 2 * M_PI); cairo_stroke(cr);
        nx_rounded_rect(cr, 0.22, 0.42, 0.56, 0.46, 0.08); cairo_fill(cr);
    } else if (!strcmp(name, "folder")) {
        /* Dossier jaune, onglet plus foncé */
        cairo_move_to(cr, 0.06, 0.24); cairo_line_to(cr, 0.38, 0.24); cairo_line_to(cr, 0.46, 0.32);
        cairo_line_to(cr, 0.94, 0.32); cairo_line_to(cr, 0.94, 0.5); cairo_line_to(cr, 0.06, 0.5);
        cairo_close_path(cr);
        nx_set(cr, nx_rgb("#e0a92e")); cairo_fill(cr);
        nx_rounded_rect(cr, 0.06, 0.34, 0.88, 0.52, 0.06);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0.34, 0, 0.86);
        cairo_pattern_add_color_stop_rgb(g, 0, 0.98, 0.82, 0.38);
        cairo_pattern_add_color_stop_rgb(g, 1, 0.95, 0.72, 0.25);
        cairo_set_source(cr, g); cairo_fill(cr); cairo_pattern_destroy(g);
    } else if (!strcmp(name, "file") || !strcmp(name, "exec") || !strcmp(name, "archive")) {
        /* Page blanche coin plié (+ emblème selon le type) */
        cairo_move_to(cr, 0.2, 0.06); cairo_line_to(cr, 0.62, 0.06); cairo_line_to(cr, 0.8, 0.24);
        cairo_line_to(cr, 0.8, 0.94); cairo_line_to(cr, 0.2, 0.94); cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.98, 0.98, 0.99); cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 0.03); cairo_set_source_rgb(cr, 0.62, 0.65, 0.72); cairo_stroke(cr);
        cairo_move_to(cr, 0.62, 0.06); cairo_line_to(cr, 0.62, 0.24); cairo_line_to(cr, 0.8, 0.24);
        cairo_stroke(cr);
        if (!strcmp(name, "exec")) {
            nx_rounded_rect(cr, 0.3, 0.42, 0.4, 0.34, 0.06); nx_set(cr, nx_rgb("#2f9e5a")); cairo_fill(cr);
            cairo_move_to(cr, 0.44, 0.5); cairo_line_to(cr, 0.58, 0.59); cairo_line_to(cr, 0.44, 0.68);
            cairo_close_path(cr); cairo_set_source_rgb(cr, 1, 1, 1); cairo_fill(cr);
        } else if (!strcmp(name, "archive")) {
            nx_rounded_rect(cr, 0.3, 0.44, 0.4, 0.34, 0.04); nx_set(cr, nx_rgb("#b07a3c")); cairo_fill(cr);
            cairo_rectangle(cr, 0.46, 0.44, 0.08, 0.34); nx_set(cr, nx_rgb("#e6c58f")); cairo_fill(cr);
        }
    } else if (!strcmp(name, "home")) {
        nx_set(cr, fg);
        cairo_move_to(cr, 0.5, 0.1); cairo_line_to(cr, 0.92, 0.48); cairo_line_to(cr, 0.8, 0.48);
        cairo_line_to(cr, 0.8, 0.9); cairo_line_to(cr, 0.2, 0.9); cairo_line_to(cr, 0.2, 0.48);
        cairo_line_to(cr, 0.08, 0.48); cairo_close_path(cr); cairo_fill(cr);
        cairo_rectangle(cr, 0.42, 0.62, 0.16, 0.28); cairo_set_source_rgb(cr, 1, 1, 1); cairo_fill(cr);
    } else if (!strcmp(name, "downloads")) {
        nx_set(cr, accent); cairo_set_line_width(cr, 0.1);
        cairo_move_to(cr, 0.5, 0.08); cairo_line_to(cr, 0.5, 0.6); cairo_stroke(cr);
        cairo_move_to(cr, 0.28, 0.4); cairo_line_to(cr, 0.5, 0.62); cairo_line_to(cr, 0.72, 0.4); cairo_stroke(cr);
        cairo_move_to(cr, 0.12, 0.66); cairo_line_to(cr, 0.12, 0.88); cairo_line_to(cr, 0.88, 0.88);
        cairo_line_to(cr, 0.88, 0.66); cairo_stroke(cr);
    } else if (!strcmp(name, "disk")) {
        nx_rounded_rect(cr, 0.06, 0.26, 0.88, 0.48, 0.1); nx_set(cr, nx_rgb("#5a6170")); cairo_fill(cr);
        nx_rounded_rect(cr, 0.12, 0.32, 0.76, 0.2, 0.06); nx_set(cr, nx_rgb("#7d8596")); cairo_fill(cr);
        cairo_arc(cr, 0.78, 0.62, 0.05, 0, 2 * M_PI); nx_set(cr, nx_rgb("#3fd0a0")); cairo_fill(cr);
    } else if (!strcmp(name, "trash")) {
        cairo_set_line_width(cr, 0.07); nx_set(cr, nx_rgb("#6b7385"));
        cairo_move_to(cr, 0.14, 0.24); cairo_line_to(cr, 0.86, 0.24); cairo_stroke(cr);
        cairo_move_to(cr, 0.38, 0.24); cairo_line_to(cr, 0.4, 0.12); cairo_line_to(cr, 0.6, 0.12);
        cairo_line_to(cr, 0.62, 0.24); cairo_stroke(cr);
        cairo_move_to(cr, 0.22, 0.3); cairo_line_to(cr, 0.28, 0.9); cairo_line_to(cr, 0.72, 0.9);
        cairo_line_to(cr, 0.78, 0.3); cairo_close_path(cr);
        nx_set(cr, nx_rgb("#9aa2b2")); cairo_fill(cr);
        cairo_set_line_width(cr, 0.05); cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
        for (int i = 0; i < 3; i++) {
            cairo_move_to(cr, 0.38 + i * 0.12, 0.42); cairo_line_to(cr, 0.38 + i * 0.12, 0.78);
        }
        cairo_stroke(cr);
    } else if (!strcmp(name, "browser")) {
        /* globe : disque d'accent, méridiens et parallèles blancs */
        cairo_arc(cr, 0.5, 0.5, 0.42, 0, 2 * M_PI);
        nx_set(cr, accent); cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9); cairo_set_line_width(cr, 0.05);
        cairo_arc(cr, 0.5, 0.5, 0.42, 0, 2 * M_PI); cairo_stroke(cr);
        cairo_save(cr); cairo_translate(cr, 0.5, 0.5); cairo_scale(cr, 0.18, 0.42);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI); cairo_restore(cr); cairo_stroke(cr);
        cairo_move_to(cr, 0.5, 0.08); cairo_line_to(cr, 0.5, 0.92); cairo_stroke(cr);
        cairo_move_to(cr, 0.08, 0.5); cairo_line_to(cr, 0.92, 0.5); cairo_stroke(cr);
        cairo_move_to(cr, 0.16, 0.3); cairo_line_to(cr, 0.84, 0.3); cairo_stroke(cr);
        cairo_move_to(cr, 0.16, 0.7); cairo_line_to(cr, 0.84, 0.7); cairo_stroke(cr);
    } else if (!strcmp(name, "display")) {
        nx_rounded_rect(cr, 0.06, 0.12, 0.88, 0.58, 0.08); nx_set(cr, nx_rgb("#3a4050")); cairo_fill(cr);
        nx_rounded_rect(cr, 0.12, 0.18, 0.76, 0.46, 0.04); nx_set(cr, accent); cairo_fill(cr);
        cairo_rectangle(cr, 0.42, 0.7, 0.16, 0.12); nx_set(cr, nx_rgb("#3a4050")); cairo_fill(cr);
        nx_rounded_rect(cr, 0.28, 0.82, 0.44, 0.07, 0.03); cairo_fill(cr);
    } else if (!strcmp(name, "keyboard")) {
        nx_rounded_rect(cr, 0.04, 0.22, 0.92, 0.56, 0.08); nx_set(cr, nx_rgb("#3a4050")); cairo_fill(cr);
        nx_set(cr, nx_rgb("#ffffff"));
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 6; c++) {
                cairo_rectangle(cr, 0.12 + c * 0.13, 0.30 + r * 0.14, 0.09, 0.09);
                cairo_fill(cr);
            }
        nx_set(cr, accent);
        nx_rounded_rect(cr, 0.25, 0.60, 0.50, 0.09, 0.03); cairo_fill(cr);
    } else {
        /* Icône générique : initiale dans un carré de couleur d'accent */
        nx_rounded_rect(cr, 0.06, 0.06, 0.88, 0.88, 0.2);
        nx_set(cr, accent); cairo_fill(cr);
        cairo_restore(cr);
        char letter[8] = "?";
        if (name && *name) {
            int n = (unsigned char)name[0] < 0x80 ? 1 : 2;
            memcpy(letter, name, n); letter[n] = 0;
            if (n == 1 && letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
        }
        double saved = nx_scale; nx_scale = 1;
        nx_text(cr, letter, s * 0.5, true, nx_rgb("#ffffff"), x, y, s, s, NX_CENTER);
        nx_scale = saved;
        return;
    }
    cairo_restore(cr);
}

/* ===================== Logos des applications ===================== */

struct app_logo { const char *id, *color; };
static const struct app_logo app_logos[] = {
    { "files", "#f0a830" },   { "browser", "#1e88e5" }, { "terminal", "#323844" },
    { "settings", "#6b7385" }, { "monitor", "#2f9e5a" }, { "editor", "#e0782a" },
    { "about", "#7a4fd8" },   { "store", "#d8457a" },   { "viewer", "#00a3c4" },
    { "calculator", "#3b4cc0" },
};

const char *nx_app_icon_for(const char *name)
{
    static const struct { const char *from, *to; } map[] = {
        { "nexus-files", "files" }, { "system-file-manager", "files" }, { "folder", "files" },
        { "nexus-browser", "browser" }, { "netsurf", "browser" }, { "web-browser", "browser" },
        { "firefox", "browser" }, { "org.mozilla.firefox", "browser" }, { "firefox-esr", "browser" },
        { "foot", "terminal" }, { "footclient", "terminal" }, { "nexus-terminal", "terminal" },
        { "utilities-terminal", "terminal" },
        { "nexus-settings", "settings" }, { "preferences-system", "settings" },
        { "nexus-monitor", "monitor" }, { "htop", "monitor" }, { "utilities-system-monitor", "monitor" },
        { "nexus-editor", "editor" }, { "nano", "editor" }, { "accessories-text-editor", "editor" },
        { "nexus-about", "about" }, { "help-about", "about" }, { "info", "about" },
        { "nexus-store", "store" }, { "system-software-install", "store" },
        { "nexus-viewer", "viewer" }, { "image", "viewer" },
        { "nexus-calculator", "calculator" }, { "accessories-calculator", "calculator" },
    };
    if (!name) return "";
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(name, map[i].from)) return map[i].to;
    return name;
}

/* Dessin du symbole d'une application dans le carré unité, couleur c */
static void app_glyph(cairo_t *cr, const char *id, nx_color c)
{
    nx_set(cr, c);
    cairo_set_line_width(cr, 0.085);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (!strcmp(id, "files")) {
        cairo_move_to(cr, 0.06, 0.2); cairo_line_to(cr, 0.4, 0.2); cairo_line_to(cr, 0.5, 0.3);
        cairo_line_to(cr, 0.94, 0.3); cairo_line_to(cr, 0.94, 0.84); cairo_line_to(cr, 0.06, 0.84);
        cairo_close_path(cr); cairo_fill(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
        cairo_rectangle(cr, 0.06, 0.38, 0.88, 0.05); cairo_fill(cr);
    } else if (!strcmp(id, "browser")) {
        cairo_arc(cr, 0.5, 0.5, 0.4, 0, 2 * M_PI); cairo_stroke(cr);
        cairo_save(cr); cairo_translate(cr, 0.5, 0.5); cairo_scale(cr, 0.17, 0.4);
        cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI); cairo_restore(cr); cairo_stroke(cr);
        cairo_set_line_width(cr, 0.065);
        cairo_move_to(cr, 0.1, 0.5); cairo_line_to(cr, 0.9, 0.5);
        cairo_move_to(cr, 0.18, 0.3); cairo_line_to(cr, 0.82, 0.3);
        cairo_move_to(cr, 0.18, 0.7); cairo_line_to(cr, 0.82, 0.7); cairo_stroke(cr);
    } else if (!strcmp(id, "terminal")) {
        nx_rounded_rect(cr, 0.08, 0.14, 0.84, 0.72, 0.1); cairo_stroke(cr);
        cairo_move_to(cr, 0.24, 0.38); cairo_line_to(cr, 0.4, 0.52); cairo_line_to(cr, 0.24, 0.66);
        cairo_move_to(cr, 0.48, 0.66); cairo_line_to(cr, 0.74, 0.66); cairo_stroke(cr);
    } else if (!strcmp(id, "settings")) {
        for (int i = 0; i < 8; i++) {
            cairo_save(cr); cairo_translate(cr, 0.5, 0.5); cairo_rotate(cr, i * M_PI / 4);
            nx_rounded_rect(cr, -0.09, -0.46, 0.18, 0.2, 0.03); cairo_fill(cr);
            cairo_restore(cr);
        }
        cairo_new_path(cr);
        cairo_arc(cr, 0.5, 0.5, 0.31, 0, 2 * M_PI);
        cairo_new_sub_path(cr);
        cairo_arc_negative(cr, 0.5, 0.5, 0.13, 2 * M_PI, 0);
        cairo_fill(cr);
    } else if (!strcmp(id, "monitor")) {
        nx_rounded_rect(cr, 0.08, 0.14, 0.84, 0.72, 0.1); cairo_stroke(cr);
        cairo_set_line_width(cr, 0.075);
        cairo_move_to(cr, 0.18, 0.56); cairo_line_to(cr, 0.34, 0.56); cairo_line_to(cr, 0.44, 0.32);
        cairo_line_to(cr, 0.56, 0.72); cairo_line_to(cr, 0.66, 0.48); cairo_line_to(cr, 0.82, 0.48);
        cairo_stroke(cr);
    } else if (!strcmp(id, "editor")) {
        cairo_save(cr); cairo_translate(cr, 0.5, 0.5); cairo_rotate(cr, M_PI / 4);
        nx_rounded_rect(cr, -0.1, -0.44, 0.2, 0.62, 0.04); cairo_fill(cr);
        cairo_move_to(cr, -0.1, 0.24); cairo_line_to(cr, 0.1, 0.24); cairo_line_to(cr, 0, 0.42);
        cairo_close_path(cr); cairo_fill(cr);
        cairo_restore(cr);
        cairo_move_to(cr, 0.12, 0.9); cairo_line_to(cr, 0.88, 0.9); cairo_stroke(cr);
    } else if (!strcmp(id, "about")) {
        cairo_arc(cr, 0.5, 0.24, 0.09, 0, 2 * M_PI); cairo_fill(cr);
        cairo_set_line_width(cr, 0.16);
        cairo_move_to(cr, 0.5, 0.46); cairo_line_to(cr, 0.5, 0.82); cairo_stroke(cr);
    } else if (!strcmp(id, "store")) {
        cairo_set_line_width(cr, 0.07);
        cairo_new_path(cr); cairo_arc(cr, 0.5, 0.34, 0.16, M_PI, 2 * M_PI); cairo_stroke(cr);
        cairo_move_to(cr, 0.14, 0.34); cairo_line_to(cr, 0.86, 0.34); cairo_line_to(cr, 0.8, 0.9);
        cairo_line_to(cr, 0.2, 0.9); cairo_close_path(cr); cairo_fill(cr);
    } else if (!strcmp(id, "viewer")) {
        nx_rounded_rect(cr, 0.08, 0.16, 0.84, 0.68, 0.08); cairo_stroke(cr);
        cairo_move_to(cr, 0.16, 0.76); cairo_line_to(cr, 0.4, 0.46); cairo_line_to(cr, 0.56, 0.64);
        cairo_line_to(cr, 0.68, 0.52); cairo_line_to(cr, 0.84, 0.76); cairo_close_path(cr); cairo_fill(cr);
        cairo_arc(cr, 0.7, 0.34, 0.07, 0, 2 * M_PI); cairo_fill(cr);
    } else if (!strcmp(id, "calculator")) {
        nx_rounded_rect(cr, 0.16, 0.06, 0.68, 0.88, 0.1); cairo_stroke(cr);
        nx_rounded_rect(cr, 0.28, 0.18, 0.44, 0.16, 0.03); cairo_fill(cr);
        for (int i = 0; i < 6; i++) {
            cairo_arc(cr, 0.34 + (i % 3) * 0.16, 0.5 + (i / 3) * 0.2, 0.05, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
}

static const struct app_logo *find_logo(const char *id)
{
    for (unsigned i = 0; i < sizeof app_logos / sizeof app_logos[0]; i++)
        if (!strcmp(app_logos[i].id, id)) return &app_logos[i];
    return NULL;
}

static void tile_path(cairo_t *cr)
{
    switch (nx_conf.icon_shape) {
    case 2: cairo_arc(cr, 0.5, 0.5, 0.46, 0, 2 * M_PI); break;
    case 1: nx_rounded_rect(cr, 0.04, 0.04, 0.92, 0.92, 0.08); break;
    default: nx_rounded_rect(cr, 0.04, 0.04, 0.92, 0.92, 0.26); break;
    }
}

void nx_app_icon(cairo_t *cr, const char *app, double x, double y, double s, nx_color fg)
{
    const char *id = nx_app_icon_for(app);
    const struct app_logo *logo = find_logo(id);
    bool mono = nx_conf.icon_style == 1;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    if (!logo) {
        /* Application inconnue : son initiale, couleur tirée de son nom */
        unsigned h = 0;
        for (const char *p = id; *p; p++) h = h * 31 + (unsigned char)*p;
        nx_color c = nx_rgb(nx_accents[h % nx_accent_count].hex);
        if (mono) {
            tile_path(cr); nx_set(cr, fg); cairo_set_line_width(cr, 0.07); cairo_stroke(cr);
        } else {
            tile_path(cr); nx_set(cr, c); cairo_fill(cr);
        }
        cairo_restore(cr);
        char letter[8] = "?";
        if (id[0]) {
            int n = (unsigned char)id[0] < 0x80 ? 1 : 2;
            memcpy(letter, id, n); letter[n] = 0;
            if (n == 1 && letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
        }
        double saved = nx_scale; nx_scale = 1;
        nx_text(cr, letter, s * 0.48, true, mono ? fg : nx_rgb("#ffffff"), x, y, s, s, NX_CENTER);
        nx_scale = saved;
        return;
    }
    if (mono) {
        cairo_translate(cr, 0.06, 0.06);
        cairo_scale(cr, 0.88, 0.88);
        app_glyph(cr, id, fg);
    } else {
        nx_color c = nx_rgb(logo->color);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, 1);
        nx_color top = nx_mix(c, nx_rgb("#ffffff"), 0.22);
        cairo_pattern_add_color_stop_rgb(g, 0, top.r, top.g, top.b);
        cairo_pattern_add_color_stop_rgb(g, 1, c.r, c.g, c.b);
        tile_path(cr);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        cairo_translate(cr, 0.22, 0.22);
        cairo_scale(cr, 0.56, 0.56);
        app_glyph(cr, id, nx_rgb("#ffffff"));
    }
    cairo_restore(cr);
}

/* ===================== Fonds d'écran ===================== */

const struct nx_preset nx_presets[] = {
    { "montagne", "Nuit étoilée", "#070b1f", "#1b1c5c", "#3b2a7a" },
    { "nexus",  "Nexus",  "#081633", "#1e6fe0", "#00c8e6" },
    { "aurore", "Aurore", "#140a2e", "#6a35d0", "#00b8d8" },
    { "nuit",   "Nuit",   "#05070d", "#121a2e", "#2a3a66" },
    { "ocean",  "Océan",  "#021f2b", "#00708f", "#3fd0d0" },
    { "foret",  "Forêt",  "#061a10", "#1f6b45", "#9ad06a" },
    { "ambre",  "Ambre",  "#2a0f05", "#c0561e", "#f2b84b" },
};
const int nx_preset_count = sizeof(nx_presets) / sizeof(nx_presets[0]);

/* Dégradé et halos d'un fond prédéfini (formes très douces) */
static void draw_preset_background(cairo_t *cr, const struct nx_preset *p, double w, double h)
{
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, w, h);
    nx_color a = nx_rgb(p->c1), b = nx_rgb(p->c2), c = nx_rgb(p->c3);
    cairo_pattern_add_color_stop_rgb(g, 0.0, a.r, a.g, a.b);
    cairo_pattern_add_color_stop_rgb(g, 0.6, b.r, b.g, b.b);
    cairo_pattern_add_color_stop_rgb(g, 1.0, c.r, c.g, c.b);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);

    /* Halos lumineux discrets */
    double d = w > h ? w : h;
    struct { double x, y, r, a; } glows[] = {
        { 0.82, 0.22, 0.45, 0.16 }, { 0.15, 0.85, 0.35, 0.10 }, { 0.55, 0.55, 0.25, 0.06 },
    };
    for (unsigned i = 0; i < sizeof glows / sizeof glows[0]; i++) {
        cairo_pattern_t *r = cairo_pattern_create_radial(glows[i].x * w, glows[i].y * h, 0,
                                                         glows[i].x * w, glows[i].y * h, glows[i].r * d);
        cairo_pattern_add_color_stop_rgba(r, 0, 1, 1, 1, glows[i].a);
        cairo_pattern_add_color_stop_rgba(r, 1, 1, 1, 1, 0);
        cairo_set_source(cr, r);
        cairo_paint(cr);
        cairo_pattern_destroy(r);
    }
}

static void draw_preset(cairo_t *cr, const struct nx_preset *p, double w, double h)
{
    /* Optimisation : le dégradé et les halos n'ont aucun détail fin. Pour un
     * grand écran, on les calcule en 1/4 de résolution (16 fois moins de
     * pixels) puis on les agrandit avec lissage : visuellement identique,
     * bien plus rapide en rendu logiciel. */
    int f = (w > 400 && h > 300 && !getenv("NX_FULLRES")) ? 4 : 1;
    if (f > 1) {
        int sw = (int)ceil(w / f) + 1, sh = (int)ceil(h / f) + 1;
        cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
        cairo_t *sc = cairo_create(small);
        draw_preset_background(sc, p, w / f, h / f);
        cairo_destroy(sc);
        cairo_save(cr);
        cairo_scale(cr, f, f);
        cairo_set_source_surface(cr, small, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(small);
    } else {
        draw_preset_background(cr, p, w, h);
    }

    /* Grand "N" en filigrane (pleine résolution : bords nets) */
    double s = h * 0.5, x = w * 0.72 - s / 2, y = h * 0.48 - s / 2;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s / 96.0, s / 96.0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
    nx_rounded_rect(cr, 3, 3, 90, 90, 20);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
    cairo_rectangle(cr, 26, 24, 12, 48);
    cairo_rectangle(cr, 58, 24, 12, 48);
    cairo_move_to(cr, 26, 24); cairo_line_to(cr, 39, 24);
    cairo_line_to(cr, 70, 72); cairo_line_to(cr, 57, 72);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}


/* ---------- Fond "Nuit étoilée" : ciel, étoiles, N lumineux, montagnes, lac ---------- */

static unsigned rnd_state;
static double rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return ((rnd_state >> 8) & 0xffffff) / (double)0x1000000;
}

/* Ligne de crête par déplacement du point milieu (déterministe : même graine,
 * même paysage). n doit valoir 2^k + 1. Valeurs autour de 0..1. */
static void ridge(double *y, int n, double rough, unsigned seed)
{
    rnd_state = seed;
    y[0] = rnd() * 0.5;
    y[n - 1] = rnd() * 0.5;
    for (int step = n - 1; step > 1; step /= 2)
        for (int i = step / 2; i < n; i += step)
            y[i] = (y[i - step / 2] + y[i + step / 2]) / 2 + (rnd() - 0.5) * rough * pow((double)step / (n - 1), 0.62);
}

static void N_path(cairo_t *cr, double x, double y, double s)
{
    double k = s / 96.0;
    cairo_new_path(cr);
    cairo_rectangle(cr, x + 26 * k, y + 24 * k, 12 * k, 48 * k);
    cairo_rectangle(cr, x + 58 * k, y + 24 * k, 12 * k, 48 * k);
    cairo_move_to(cr, x + 26 * k, y + 24 * k); cairo_line_to(cr, x + 39 * k, y + 24 * k);
    cairo_line_to(cr, x + 70 * k, y + 72 * k); cairo_line_to(cr, x + 57 * k, y + 72 * k);
    cairo_close_path(cr);
}

/* Une couche de montagnes : crête + dégradé vertical (sommets éclairés) */
static void mountains(cairo_t *cr, double w, double hy, double amp, unsigned seed, double rough,
                      double peak_x, double peak_boost, nx_color top, nx_color bottom, bool mirror)
{
    enum { N = 129 };
    double y[N];
    ridge(y, N, rough, seed);
    cairo_new_path(cr);
    for (int i = 0; i < N; i++) {
        double fx = (double)i / (N - 1);
        double boost = peak_boost * fmax(0, 1 - fabs(fx - peak_x) / 0.16);   /* sommet principal (pyramide) */
        double v = fmax(0, y[i] + 0.35 + boost);
        double py = mirror ? hy + amp * v * 0.8 : hy - amp * v;
        if (i == 0) cairo_move_to(cr, 0, py); else cairo_line_to(cr, fx * w, py);
    }
    cairo_line_to(cr, w, hy);
    cairo_line_to(cr, 0, hy);
    cairo_close_path(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, mirror ? hy + amp : hy - amp, 0, hy);
    cairo_pattern_add_color_stop_rgba(g, 0, top.r, top.g, top.b, top.a);
    cairo_pattern_add_color_stop_rgba(g, 1, bottom.r, bottom.g, bottom.b, bottom.a);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

static void draw_night_scene(cairo_t *cr, double w, double h)
{
    double hy = h * 0.70;              /* ligne d'horizon / rive du lac */

    /* Ciel */
    cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, hy);
    cairo_pattern_add_color_stop_rgb(sky, 0.0, 0.02, 0.03, 0.09);
    cairo_pattern_add_color_stop_rgb(sky, 0.55, 0.08, 0.10, 0.32);
    cairo_pattern_add_color_stop_rgb(sky, 1.0, 0.23, 0.17, 0.48);
    cairo_rectangle(cr, 0, 0, w, hy);
    cairo_set_source(cr, sky);
    cairo_fill(cr);
    cairo_pattern_destroy(sky);

    /* Étoiles */
    rnd_state = 42;
    int nstars = (int)(w * h / 2600);
    for (int i = 0; i < nstars; i++) {
        double x = rnd() * w, y = pow(rnd(), 1.4) * hy * 0.9, r = 0.4 + rnd() * 1.1, a = 0.25 + rnd() * 0.7;
        if (rnd() < 0.04) {   /* quelques étoiles brillantes avec halo */
            cairo_pattern_t *gl = cairo_pattern_create_radial(x, y, 0, x, y, r * 6);
            cairo_pattern_add_color_stop_rgba(gl, 0, 0.8, 0.88, 1, 0.5);
            cairo_pattern_add_color_stop_rgba(gl, 1, 0.8, 0.88, 1, 0);
            cairo_set_source(cr, gl);
            cairo_arc(cr, x, y, r * 6, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_pattern_destroy(gl);
        }
        cairo_set_source_rgba(cr, 0.9, 0.93, 1, a);
        cairo_arc(cr, x, y, r * fmax(1, h / 900), 0, 2 * M_PI);
        cairo_fill(cr);
    }

    /* Halo et arc lumineux derrière le N */
    double nx_ = w * 0.645, ny_ = h * 0.34, s = h * 0.42;
    cairo_pattern_t *halo = cairo_pattern_create_radial(nx_, ny_, 0, nx_, ny_, h * 0.42);
    cairo_pattern_add_color_stop_rgba(halo, 0, 0.35, 0.75, 1, 0.22);
    cairo_pattern_add_color_stop_rgba(halo, 1, 0.35, 0.75, 1, 0);
    cairo_set_source(cr, halo);
    cairo_paint(cr);
    cairo_pattern_destroy(halo);
    double ar = h * 0.40;
    struct { double wd, a; } arc_layers[] = { { 22, 0.03 }, { 12, 0.06 }, { 5, 0.14 }, { 1.6, 0.75 } };
    for (unsigned i = 0; i < 4; i++) {
        cairo_new_path(cr);
        cairo_arc(cr, nx_ - ar * 0.08, ny_ + ar * 0.05, ar, -2.3, 0.45);
        cairo_set_line_width(cr, arc_layers[i].wd * h / 900);
        cairo_set_source_rgba(cr, 0.72, 0.84, 1, arc_layers[i].a);
        cairo_stroke(cr);
    }

    /* Le N lumineux : lueur floue (N dessiné en tout petit puis agrandi avec
     * lissage = flou), puis remplissage en dégradé */
    double lx = nx_ - s / 2, ly = ny_ - s / 2;
    {
        int f = 10, gw = (int)(s * 1.6 / f) + 2;
        cairo_surface_t *gs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gw);
        cairo_t *gc = cairo_create(gs);
        N_path(gc, s * 0.3 / f, s * 0.3 / f, s / f);
        cairo_set_source_rgba(gc, 0.35, 0.8, 1, 1);
        cairo_fill(gc);
        cairo_destroy(gc);
        for (int pass = 0; pass < 2; pass++) {
            cairo_save(cr);
            cairo_translate(cr, lx - s * 0.3, ly - s * 0.3);
            cairo_scale(cr, f, f);
            cairo_set_source_surface(cr, gs, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint_with_alpha(cr, pass ? 0.55 : 0.35);
            cairo_restore(cr);
        }
        cairo_surface_destroy(gs);
    }
    N_path(cr, lx, ly, s);
    cairo_pattern_t *ng = cairo_pattern_create_linear(0, ly + s * 0.25, 0, ly + s * 0.75);
    cairo_pattern_add_color_stop_rgba(ng, 0, 0.62, 0.95, 1, 0.97);
    cairo_pattern_add_color_stop_rgba(ng, 1, 0.22, 0.52, 1, 0.95);
    cairo_set_source(cr, ng);
    cairo_fill(cr);
    cairo_pattern_destroy(ng);

    /* Lueur violette à l'horizon, derrière les montagnes */
    cairo_pattern_t *hz = cairo_pattern_create_linear(0, hy - h * 0.28, 0, hy);
    cairo_pattern_add_color_stop_rgba(hz, 0, 0.45, 0.30, 0.85, 0);
    cairo_pattern_add_color_stop_rgba(hz, 1, 0.55, 0.38, 0.95, 0.35);
    cairo_rectangle(cr, 0, hy - h * 0.28, w, h * 0.28);
    cairo_set_source(cr, hz);
    cairo_fill(cr);
    cairo_pattern_destroy(hz);

    /* Montagnes : lointaines, principales (sommet éclairé), collines */
    mountains(cr, w, hy, h * 0.26, 7, 1.1, 0.18, 0.25,
              (nx_color){ 0.25, 0.27, 0.58, 1 }, (nx_color){ 0.13, 0.14, 0.36, 1 }, false);
    mountains(cr, w, hy, h * 0.42, 3, 1.0, 0.42, 0.75,
              (nx_color){ 0.62, 0.66, 0.92, 1 }, (nx_color){ 0.09, 0.10, 0.27, 1 }, false);
    mountains(cr, w, hy, h * 0.10, 11, 0.8, 0.8, 0.2,
              (nx_color){ 0.08, 0.09, 0.23, 1 }, (nx_color){ 0.04, 0.05, 0.13, 1 }, false);

    /* Sapins le long de la rive */
    rnd_state = 99;
    double tx = 0;
    while (tx < w) {
        double fx = tx / w;
        double density = (fx < 0.3 || fx > 0.62) ? 1.0 : 0.35;
        double th = (12 + rnd() * 30) * h / 900 * (0.6 + density * 0.6);
        if (rnd() < density) {
            cairo_move_to(cr, tx, hy - th);
            cairo_line_to(cr, tx + th * 0.22, hy + 1);
            cairo_line_to(cr, tx - th * 0.22, hy + 1);
            cairo_close_path(cr);
            cairo_set_source_rgb(cr, 0.02, 0.03, 0.08);
            cairo_fill(cr);
        }
        tx += (4 + rnd() * 8) * h / 900;
    }

    /* Lac : eau sombre, reflet des montagnes, reflets scintillants */
    cairo_pattern_t *lake = cairo_pattern_create_linear(0, hy, 0, h);
    cairo_pattern_add_color_stop_rgb(lake, 0, 0.13, 0.12, 0.34);
    cairo_pattern_add_color_stop_rgb(lake, 1, 0.03, 0.04, 0.11);
    cairo_rectangle(cr, 0, hy, w, h - hy);
    cairo_set_source(cr, lake);
    cairo_fill(cr);
    cairo_pattern_destroy(lake);
    cairo_push_group(cr);
    mountains(cr, w, hy, h * 0.30, 3, 1.0, 0.42, 0.75,
              (nx_color){ 0.40, 0.44, 0.80, 1 }, (nx_color){ 0.10, 0.10, 0.28, 1 }, true);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.20);
    rnd_state = 5;
    for (int i = 0; i < (int)(h / 9); i++) {
        double y = hy + 3 + pow(rnd(), 0.8) * (h - hy);
        double x = rnd() * w, len = (20 + rnd() * 160) * w / 1440;
        cairo_rectangle(cr, x, y, len, 1);
        cairo_set_source_rgba(cr, 0.75, 0.82, 1, 0.03 + rnd() * 0.08);
        cairo_fill(cr);
    }
    /* Reflet du N sur l'eau */
    cairo_pattern_t *refl = cairo_pattern_create_radial(nx_, hy + (hy - ny_) * 0.6, 0,
                                                        nx_, hy + (hy - ny_) * 0.6, s * 0.9);
    cairo_pattern_add_color_stop_rgba(refl, 0, 0.35, 0.75, 1, 0.12);
    cairo_pattern_add_color_stop_rgba(refl, 1, 0.35, 0.75, 1, 0);
    cairo_set_source(cr, refl);
    cairo_rectangle(cr, 0, hy, w, h - hy);
    cairo_fill(cr);
    cairo_pattern_destroy(refl);
}

/* Cache de la dernière image chargée (les miniatures redessinent souvent) */
static char img_path[512];
static cairo_surface_t *img;

static cairo_surface_t *load_png(const char *path)
{
    if (img && !strcmp(path, img_path)) return img;
    if (img) cairo_surface_destroy(img);
    img = nx_load_image(path, 0);   /* PNG ou JPEG */
    snprintf(img_path, sizeof img_path, "%s", path);
    return img;
}

void nx_draw_wallpaper(cairo_t *cr, const char *spec, double w, double h)
{
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_clip(cr);
    if (!strncmp(spec, "color:", 6)) {
        nx_set(cr, nx_rgb(spec + 6));
        cairo_paint(cr);
    } else if (spec[0] == '/' && load_png(spec)) {
        /* Image : mise à l'échelle pour couvrir tout l'écran, centrée */
        double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
        double k = fmax(w / iw, h / ih);
        cairo_translate(cr, (w - iw * k) / 2, (h - ih * k) / 2);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    } else {
        const struct nx_preset *p = &nx_presets[0];   /* défaut : Nuit étoilée */
        if (!strncmp(spec, "preset:", 7))
            for (int i = 0; i < nx_preset_count; i++)
                if (!strcmp(spec + 7, nx_presets[i].id)) p = &nx_presets[i];
        if (!strcmp(p->id, "montagne")) draw_night_scene(cr, w, h);
        else draw_preset(cr, p, w, h);
    }
    cairo_restore(cr);
}

/* Cache des fonds déjà dessinés (miniatures et aperçus de Paramètres, qui
 * sont redessinés à chaque mouvement de souris) : on les calcule une fois. */
#define WCACHE_N 24
static struct {
    char spec[512];
    int w, h;
    cairo_surface_t *s;
    unsigned long used;
} wcache[WCACHE_N];
static unsigned long wcache_clock;

void nx_draw_wallpaper_cached(cairo_t *cr, const char *spec, double w, double h)
{
    int iw = (int)ceil(w), ih = (int)ceil(h), slot = -1, lru = 0;
    for (int i = 0; i < WCACHE_N; i++) {
        if (wcache[i].s && wcache[i].w == iw && wcache[i].h == ih && !strcmp(wcache[i].spec, spec)) {
            slot = i;
            break;
        }
        if (wcache[i].used < wcache[lru].used) lru = i;
    }
    if (slot < 0) {
        slot = lru;
        if (wcache[slot].s) cairo_surface_destroy(wcache[slot].s);
        wcache[slot].s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
        cairo_t *c = cairo_create(wcache[slot].s);
        nx_draw_wallpaper(c, spec, iw, ih);
        cairo_destroy(c);
        snprintf(wcache[slot].spec, sizeof wcache[slot].spec, "%s", spec);
        wcache[slot].w = iw;
        wcache[slot].h = ih;
    }
    wcache[slot].used = ++wcache_clock;
    cairo_save(cr);
    cairo_set_source_surface(cr, wcache[slot].s, 0, 0);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    cairo_restore(cr);
}

/* ===================== Tampons de mémoire partagée ===================== */

static void buffer_release(void *data, struct wl_buffer *wl)
{
    (void)wl;
    struct nx_buffer *b = data;
    b->busy = false;
}
static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void buffer_destroy(struct nx_buffer *b)
{
    if (b->cs) cairo_surface_destroy(b->cs);
    if (b->wl) wl_buffer_destroy(b->wl);
    if (b->data) munmap(b->data, b->size);
    memset(b, 0, sizeof *b);
}

static bool buffer_create(struct nx_buffer *b, int w, int h)
{
    int stride = w * 4;
    size_t size = (size_t)stride * h;
    int fd = memfd_create("nexus-ui", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); return false; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return false; }
    struct wl_shm_pool *pool = wl_shm_create_pool(nx.shm, fd, size);
    b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->wl, &buffer_listener, b);
    b->data = data; b->size = size; b->width = w; b->height = h; b->busy = false;
    b->cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, w, h, stride);
    return true;
}

static struct nx_buffer *get_buffer(struct nx_window *w)
{
    for (int i = 0; i < 2; i++) {
        struct nx_buffer *b = &w->bufs[i];
        if (b->busy) continue;
        if (b->wl && b->width == w->width && b->height == w->height) return b;
        buffer_destroy(b);
        if (buffer_create(b, w->width, w->height)) return b;
        return NULL;
    }
    return NULL;   /* les deux tampons sont encore utilisés par le compositeur */
}

static void draw_now(struct nx_window *w)
{
    if (!w->configured || w->width <= 0 || w->height <= 0 || !w->h.draw) return;
    struct nx_buffer *b = get_buffer(w);
    if (!b) return;   /* on réessaiera au prochain tour */
    cairo_t *cr = cairo_create(b->cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    w->h.draw(w, cr, w->width, w->height);
    cairo_destroy(cr);
    cairo_surface_flush(b->cs);
    wl_surface_attach(w->surface, b->wl, 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(w->surface);
    b->busy = true;
    w->dirty = false;
}

void nx_redraw(struct nx_window *w) { if (w) w->dirty = true; }

/* ===================== Souris ===================== */

static struct nx_window *win_of(struct wl_surface *s)
{
    for (int i = 0; i < nx.nwindows; i++)
        if (nx.windows[i]->surface == s) return nx.windows[i];
    return NULL;
}

static void send_pointer(enum nx_pointer_event ev, int value)
{
    struct nx_window *w = nx.pointer_focus;
    if (w && w->h.pointer) w->h.pointer(w, ev, w->px, w->py, value);
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                    wl_fixed_t x, wl_fixed_t y)
{
    (void)d;
    nx.pointer_focus = win_of(s);
    if (nx.cursor_image) {
        wl_pointer_set_cursor(p, serial, nx.cursor_surface,
                              nx.cursor_image->hotspot_x, nx.cursor_image->hotspot_y);
    }
    if (nx.pointer_focus) {
        nx.pointer_focus->px = wl_fixed_to_double(x);
        nx.pointer_focus->py = wl_fixed_to_double(y);
        send_pointer(NX_MOTION, 0);
    }
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    (void)d; (void)p; (void)serial; (void)s;
    send_pointer(NX_LEAVE, 0);
    nx.pointer_focus = NULL;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)t;
    if (!nx.pointer_focus) return;
    nx.pointer_focus->px = wl_fixed_to_double(x);
    nx.pointer_focus->py = wl_fixed_to_double(y);
    send_pointer(NX_MOTION, 0);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
                     uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)serial; (void)t;
    send_pointer(state == WL_POINTER_BUTTON_STATE_PRESSED ? NX_PRESS : NX_RELEASE, (int)button);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
    (void)d; (void)p; (void)t;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        send_pointer(NX_SCROLL, wl_fixed_to_double(v) > 0 ? 1 : -1);
}
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button,
    .axis = p_axis, .frame = p_frame, .axis_source = p_axis_source,
    .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
};

/* ===================== Clavier ===================== */

bool nx_mod_ctrl(void)
{
    return nx.xkb_state &&
           xkb_state_mod_name_is_active(nx.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
}
bool nx_mod_shift(void)
{
    return nx.xkb_state &&
           xkb_state_mod_name_is_active(nx.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
}

void nx_shell_quote(char *out, size_t n, const char *s)
{
    size_t o = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[o++] = '\'';
    for (; *s && o + 5 < n; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = 0;
}

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t format, int fd, uint32_t size)
{
    (void)d; (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    if (nx.xkb_state) xkb_state_unref(nx.xkb_state);
    if (nx.keymap) xkb_keymap_unref(nx.keymap);
    nx.keymap = xkb_keymap_new_from_string(nx.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map, size);
    nx.xkb_state = nx.keymap ? xkb_state_new(nx.keymap) : NULL;
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
    (void)d; (void)k; (void)serial; (void)keys;
    nx.keyboard_focus = win_of(s);
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void)d; (void)k; (void)serial; (void)s;
    nx.keyboard_focus = NULL;
}
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)t;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !nx.xkb_state) return;
    struct nx_window *w = nx.keyboard_focus;
    if (!w || !w->h.key) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(nx.xkb_state, key + 8);
    char utf8[16] = "";
    xkb_state_key_get_utf8(nx.xkb_state, key + 8, utf8, sizeof utf8);
    w->h.key(w, sym, utf8);
}
static void k_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
                        uint32_t lat, uint32_t lock, uint32_t group)
{
    (void)d; (void)k; (void)serial;
    if (nx.xkb_state) xkb_state_update_mask(nx.xkb_state, dep, lat, lock, 0, 0, group);
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    (void)d; (void)k; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = k_keymap, .enter = k_enter, .leave = k_leave, .key = k_key,
    .modifiers = k_modifiers, .repeat_info = k_repeat,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !nx.pointer) {
        nx.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(nx.pointer, &pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !nx.keyboard) {
        nx.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(nx.keyboard, &keyboard_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

/* ===================== Registre et coquilles ===================== */

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
#define NX_MIN(a, b) ((a) < (b) ? (a) : (b))
    if (!strcmp(iface, wl_compositor_interface.name))
        nx.compositor = wl_registry_bind(r, name, &wl_compositor_interface, NX_MIN(ver, 4));
    else if (!strcmp(iface, wl_shm_interface.name))
        nx.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name) && !nx.seat) {
        nx.seat = wl_registry_bind(r, name, &wl_seat_interface, NX_MIN(ver, 5));
        wl_seat_add_listener(nx.seat, &seat_listener, NULL);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        nx.layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, NX_MIN(ver, 4));
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        nx.wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(nx.wm_base, &wm_listener, NULL);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
        nx.decorations = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name))
        nx.toplevels = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, NX_MIN(ver, 3));
#undef NX_MIN
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener registry_listener = { .global = reg_global, .global_remove = reg_remove };

static void on_signal(int sig)
{
    if (sig == SIGUSR1) reload_flag = 1;
    else quit_flag = 1;
}

bool nx_init(void)
{
    nx_config_load(&nx_conf);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);   /* pas de processus zombies pour les programmes lancés */

    nx.display = wl_display_connect(NULL);
    if (!nx.display) {
        fprintf(stderr, "Impossible de se connecter au bureau (Wayland). Le bureau est-il lancé ?\n");
        return false;
    }
    nx.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    nx.registry = wl_display_get_registry(nx.display);
    wl_registry_add_listener(nx.registry, &registry_listener, NULL);
    wl_display_roundtrip(nx.display);
    wl_display_roundtrip(nx.display);
    if (!nx.compositor || !nx.shm) {
        fprintf(stderr, "Le compositeur ne fournit pas les fonctions de base.\n");
        return false;
    }
    /* Curseur de souris (thème intégré à libwayland-cursor si aucun thème installé) */
    struct wl_cursor_theme *theme = wl_cursor_theme_load(NULL, 24, nx.shm);
    struct wl_cursor *cur = theme ? wl_cursor_theme_get_cursor(theme, "left_ptr") : NULL;
    if (cur && cur->image_count > 0) {
        nx.cursor_image = cur->images[0];
        nx.cursor_surface = wl_compositor_create_surface(nx.compositor);
        wl_surface_attach(nx.cursor_surface, wl_cursor_image_get_buffer(nx.cursor_image), 0, 0);
        wl_surface_commit(nx.cursor_surface);
    }
    nx.running = true;
    return true;
}

static void add_window(struct nx_window *w)
{
    if (nx.nwindows < 8) nx.windows[nx.nwindows++] = w;
}

static void layer_configure(void *d, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h)
{
    struct nx_window *win = d;
    zwlr_layer_surface_v1_ack_configure(l, serial);
    if (w) win->width = w;
    if (h) win->height = h;
    win->configured = true;
    win->dirty = true;
}
static void layer_closed(void *d, struct zwlr_layer_surface_v1 *l)
{
    (void)l;
    struct nx_window *win = d;
    if (win->h.closed) win->h.closed(win); else nx_quit();
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure, .closed = layer_closed,
};

struct nx_window *nx_layer_window(struct nx_handlers h, uint32_t layer, uint32_t anchor,
                                  int width, int height, int exclusive_zone,
                                  uint32_t keyboard, const char *ns)
{
    if (!nx.layer_shell) {
        fprintf(stderr, "Le compositeur ne gère pas wlr-layer-shell.\n");
        return NULL;
    }
    struct nx_window *w = calloc(1, sizeof *w);
    w->h = h;
    w->width = width; w->height = height;
    w->surface = wl_compositor_create_surface(nx.compositor);
    w->layer = zwlr_layer_shell_v1_get_layer_surface(nx.layer_shell, w->surface, NULL, layer, ns);
    zwlr_layer_surface_v1_set_size(w->layer, width, height);
    zwlr_layer_surface_v1_set_anchor(w->layer, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(w->layer, exclusive_zone);
    zwlr_layer_surface_v1_set_keyboard_interactivity(w->layer, keyboard);
    zwlr_layer_surface_v1_add_listener(w->layer, &layer_listener, w);
    wl_surface_commit(w->surface);
    add_window(w);
    return w;
}

static void xdg_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    struct nx_window *w = d;
    xdg_surface_ack_configure(s, serial);
    if (w->pending_w > 0) w->width = w->pending_w;
    if (w->pending_h > 0) w->height = w->pending_h;
    w->configured = true;
    w->dirty = true;
}
static const struct xdg_surface_listener xdg_listener = { .configure = xdg_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
    (void)t; (void)st;
    struct nx_window *win = d;
    win->pending_w = w;
    win->pending_h = h;
}
static void top_close(void *d, struct xdg_toplevel *t)
{
    (void)t;
    struct nx_window *win = d;
    if (win->h.closed) win->h.closed(win); else nx_quit();
}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_listener = {
    .configure = top_configure, .close = top_close,
    .configure_bounds = top_bounds, .wm_capabilities = top_caps,
};

struct nx_window *nx_toplevel_window(struct nx_handlers h, const char *title,
                                     const char *app_id, int width, int height)
{
    if (!nx.wm_base) {
        fprintf(stderr, "Le compositeur ne gère pas xdg-shell.\n");
        return NULL;
    }
    struct nx_window *w = calloc(1, sizeof *w);
    w->h = h;
    w->width = width; w->height = height;
    w->surface = wl_compositor_create_surface(nx.compositor);
    w->xdg = xdg_wm_base_get_xdg_surface(nx.wm_base, w->surface);
    xdg_surface_add_listener(w->xdg, &xdg_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg);
    xdg_toplevel_add_listener(w->toplevel, &top_listener, w);
    xdg_toplevel_set_title(w->toplevel, title);
    xdg_toplevel_set_app_id(w->toplevel, app_id);
    xdg_toplevel_set_min_size(w->toplevel, width * 3 / 4, height * 3 / 4);
    /* Barre de titre dessinée par le compositeur (thème Nexus de labwc) */
    if (nx.decorations) {
        struct zxdg_toplevel_decoration_v1 *d =
            zxdg_decoration_manager_v1_get_toplevel_decoration(nx.decorations, w->toplevel);
        zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(w->surface);
    add_window(w);
    return w;
}

/* ===================== Boucle principale ===================== */

void nx_quit(void) { nx.running = false; }

void nx_run(void)
{
    struct pollfd pfd = { .fd = wl_display_get_fd(nx.display), .events = POLLIN };
    while (nx.running && !quit_flag) {
        for (int i = 0; i < nx.nwindows; i++)
            if (nx.windows[i]->dirty) draw_now(nx.windows[i]);

        while (wl_display_prepare_read(nx.display) != 0)
            wl_display_dispatch_pending(nx.display);
        if (wl_display_flush(nx.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(nx.display);
            break;
        }
        int timeout = nx.timeout ? nx.timeout() : -1;
        /* Si un dessin attend un tampon libre, on revérifie bientôt */
        for (int i = 0; i < nx.nwindows; i++)
            if (nx.windows[i]->dirty && (timeout < 0 || timeout > 16)) timeout = 16;
        int r = poll(&pfd, 1, timeout);
        if (r > 0) {
            if (wl_display_read_events(nx.display) < 0) break;
        } else {
            wl_display_cancel_read(nx.display);
        }
        if (wl_display_dispatch_pending(nx.display) < 0) break;   /* compositeur parti */

        if (r == 0 && nx.tick) nx.tick();
        if (reload_flag) {
            reload_flag = 0;
            nx_config_load(&nx_conf);
            if (nx.reload) nx.reload();
            for (int i = 0; i < nx.nwindows; i++) nx.windows[i]->dirty = true;
        }
    }
}

/* ===================== Divers ===================== */

void nx_spawn(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        signal(SIGUSR1, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* Prévient directement (sans lancer de processus) les programmes du bureau
 * que la configuration a changé : SIGUSR1 à chacun d'eux. */
void nx_notify_reload(void)
{
    static const char *names[] = { "nexus-wallpaper", "nexus-panel", "nexus-menu", "nexus-widgets" };
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    char path[64], comm[32];
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
                if (!strcmp(comm, names[i])) kill(atoi(e->d_name), SIGUSR1);
        }
        fclose(f);
    }
    closedir(d);
}

/* nexus-shell bench : chronomètre le dessin d'un fond d'écran 1280x800,
 * méthode optimisée contre pleine résolution (NX_FULLRES). */
#include <time.h>
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static double bench_once(int n)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1280, 800);
    cairo_t *cr = cairo_create(s);
    double t0 = now_ms();
    for (int i = 0; i < n; i++) nx_draw_wallpaper(cr, "preset:nexus", 1280, 800);
    cairo_surface_flush(s);
    double dt = (now_ms() - t0) / n;
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return dt;
}
int nx_bench(void)
{
    int n = 5;
    setenv("NX_FULLRES", "1", 1);
    double full = bench_once(n);
    unsetenv("NX_FULLRES");
    double fast = bench_once(n);
    printf("Fond d'écran 1280x800 : pleine résolution %.0f ms, optimisé %.0f ms (x%.1f)\n",
           full, fast, full / fast);
    return 0;
}

/* ===================== Fichiers récents ===================== */

static void recent_file(char *buf, size_t n)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    snprintf(buf, n, "%s/.local/share/nexus/recents", home);
}

int nx_recent_load(struct nx_recent *out, int max)
{
    char p[600];
    recent_file(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0;
    char line[1100];
    while (n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        if (access(tab + 1, F_OK) != 0) continue;   /* fichier disparu */
        out[n].when = atol(line);
        snprintf(out[n].path, sizeof out[n].path, "%s", tab + 1);
        n++;
    }
    fclose(f);
    return n;
}

void nx_recent_add(const char *path)
{
    struct nx_recent old[30];
    int n = nx_recent_load(old, 30);
    char p[600], dir[600];
    recent_file(p, sizeof p);
    snprintf(dir, sizeof dir, "%s", p);
    *strrchr(dir, '/') = 0;
    char parent[600];
    snprintf(parent, sizeof parent, "%s", dir);
    *strrchr(parent, '/') = 0;
    mkdir(parent, 0755);
    mkdir(dir, 0755);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%ld\t%s\n", (long)time(NULL), path);
    for (int i = 0; i < n && i < 29; i++)
        if (strcmp(old[i].path, path)) fprintf(f, "%ld\t%s\n", old[i].when, old[i].path);
    fclose(f);
}

void nx_ago(char *out, size_t n, long when)
{
    long d = (long)time(NULL) - when;
    if (d < 60) snprintf(out, n, "À l'instant");
    else if (d < 3600) snprintf(out, n, "Il y a %ld min", d / 60);
    else if (d < 86400) snprintf(out, n, "Il y a %ld h", d / 3600);
    else if (d < 2 * 86400) snprintf(out, n, "Hier");
    else snprintf(out, n, "Il y a %ld jours", d / 86400);
}

/* nexus-shell render <fond> <fichier.png> [largeur hauteur] : aperçu d'un fond */
int nx_render(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "Usage : nexus-shell render <fond> <fichier.png> [l h]\n"); return 2; }
    int w = argc > 4 ? atoi(argv[3]) : 1440, h = argc > 4 ? atoi(argv[4]) : 900;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    nx_draw_wallpaper(cr, argv[1], w, h);
    cairo_destroy(cr);
    int rc = cairo_surface_write_to_png(s, argv[2]) == CAIRO_STATUS_SUCCESS ? 0 : 1;
    cairo_surface_destroy(s);
    return rc;
}

/* ===================== Instance unique ===================== */

static char instance_pidfile[512];

bool nx_toggle_instance(const char *name)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(instance_pidfile, sizeof instance_pidfile, "%s/%s.pid", rt ? rt : "/tmp", name);
    FILE *f = fopen(instance_pidfile, "r");
    if (f) {
        int pid = 0;
        if (fscanf(f, "%d", &pid) == 1 && pid > 1 && kill(pid, 0) == 0) {
            fclose(f);
            kill(pid, SIGTERM);
            instance_pidfile[0] = 0;
            return true;
        }
        fclose(f);
    }
    f = fopen(instance_pidfile, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    return false;
}

void nx_instance_done(void)
{
    if (instance_pidfile[0]) unlink(instance_pidfile);
}

/* ---- Chargement d'images (PNG par cairo, JPEG par libjpeg) ---- */

struct nx_jpeg_err { struct jpeg_error_mgr pub; jmp_buf jb; };
static void nx_jpeg_fail(j_common_ptr c) { longjmp(((struct nx_jpeg_err *)c->err)->jb, 1); }

/* min_dim > 0 : le JPEG est décodé directement en plus petit (1/2, 1/4, 1/8)
 * tant que ses deux côtés restent >= min_dim : miniatures bien plus rapides */
cairo_surface_t *nx_load_image(const char *path, int min_dim)
{
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".png") == 0) {
        cairo_surface_t *s = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) return s;
        cairo_surface_destroy(s);
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct jpeg_decompress_struct ci;
    struct nx_jpeg_err je;
    cairo_surface_t *volatile s = NULL;
    unsigned char *volatile row = NULL;
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = nx_jpeg_fail;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        fclose(f);
        free(row);
        if (s) cairo_surface_destroy(s);
        return NULL;
    }
    jpeg_create_decompress(&ci);
    jpeg_stdio_src(&ci, f);
    jpeg_read_header(&ci, TRUE);
    ci.out_color_space = JCS_RGB;
    if (min_dim > 0) {
        unsigned d = 1;
        while (d < 8 && ci.image_width / (d * 2) >= (unsigned)min_dim && ci.image_height / (d * 2) >= (unsigned)min_dim)
            d *= 2;
        ci.scale_num = 1;
        ci.scale_denom = d;
    }
    jpeg_start_decompress(&ci);
    int w = ci.output_width, h = ci.output_height;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || ci.output_components != 3) longjmp(je.jb, 1);
    s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    row = malloc((size_t)w * 3);
    unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW rp = row;
        int y = ci.output_scanline;
        jpeg_read_scanlines(&ci, &rp, 1);
        uint32_t *dst = (uint32_t *)(data + (size_t)y * stride);
        for (int x = 0; x < w; x++)
            dst[x] = 0xff000000u | (uint32_t)row[3 * x] << 16 | (uint32_t)row[3 * x + 1] << 8 | row[3 * x + 2];
    }
    cairo_surface_mark_dirty(s);
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    free(row);
    fclose(f);
    return s;
}
