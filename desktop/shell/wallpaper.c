/* nexus-wallpaper — le fond du bureau : fond d'écran + icônes du bureau
 * ("Ce PC", "Mes fichiers", "Corbeille"). Double-clic sur une icône pour
 * l'ouvrir dans le gestionnaire de fichiers.
 * Relit la configuration quand il reçoit SIGUSR1 (envoyé par Paramètres). */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/input-event-codes.h>

struct dicon { const char *label, *icon, *cmd; };
static const struct dicon icons[] = {
    { "Ce PC", "display", "nexus-files /" },
    { "Mes fichiers", "folder", "nexus-files" },
    { "Corbeille", "trash", "nexus-files \"$HOME/.local/share/Trash/files\"" },
};
#define NICONS ((int)(sizeof icons / sizeof icons[0]))
/* Taille des icônes selon le réglage « Icônes » (petites, moyennes, grandes) */
#define ISZ    (54 * nx_icon_factor())
#define ICON_W (ISZ + 50)
#define ICON_H (ISZ + 46)

static struct nx_window *win;
static cairo_surface_t *cache;          /* fond d'écran déjà dessiné */
static char cache_spec[512];
static int cache_w, cache_h;
static int hover = -1, selected = -1, last_click = -1;
static double last_time;

static void icon_pos(int i, double *x, double *y) { *x = 20; *y = 20 + i * (ICON_H + 8); }

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    /* Le paysage n'est recalculé que si la taille ou le fond changent */
    if (!cache || cache_w != width || cache_h != height || strcmp(cache_spec, nx_conf.wallpaper)) {
        if (cache) cairo_surface_destroy(cache);
        cache = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
        cairo_t *c = cairo_create(cache);
        nx_draw_wallpaper(c, nx_conf.wallpaper, width, height);
        cairo_destroy(c);
        cache_w = width; cache_h = height;
        snprintf(cache_spec, sizeof cache_spec, "%s", nx_conf.wallpaper);
    }
    cairo_set_source_surface(cr, cache, 0, 0);
    cairo_paint(cr);

    if (!nx_conf.desktop_icons) return;
    for (int i = 0; i < NICONS; i++) {
        double x, y;
        icon_pos(i, &x, &y);
        if (i == selected || i == hover) {
            nx_rounded_rect(cr, x, y, ICON_W, ICON_H, 10);
            cairo_set_source_rgba(cr, 1, 1, 1, i == selected ? 0.18 : 0.09);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, i == selected ? 0.30 : 0.12);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        nx_icon(cr, icons[i].icon, x + (ICON_W - ISZ) / 2.0, y + 8, ISZ, nx_rgb("#ffffff"));
        /* texte blanc avec ombre portée pour rester lisible sur tous les fonds */
        nx_text(cr, icons[i].label, 13, false, nx_alpha(nx_rgb("#000000"), 0.6),
                x + 1, y + ISZ + 13, ICON_W, 26, NX_CENTER);
        nx_text(cr, icons[i].label, 13, false, nx_rgb("#ffffff"), x, y + ISZ + 12, ICON_W, 26, NX_CENTER);
    }
}

static int hit(double px, double py)
{
    if (!nx_conf.desktop_icons) return -1;
    for (int i = 0; i < NICONS; i++) {
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
    int h = hit(x, y);
    if (ev == NX_MOTION && h != hover) { hover = h; nx_redraw(w); }
    else if (ev == NX_LEAVE && hover != -1) { hover = -1; nx_redraw(w); }
    else if (ev == NX_PRESS && value == BTN_LEFT) {
        double t = now_s();
        if (h >= 0 && h == last_click && t - last_time < 0.5) {
            nx_spawn(icons[h].cmd);
            last_click = -1;
        } else {
            last_click = h;
            last_time = t;
        }
        if (selected != h) { selected = h; nx_redraw(w); }
    }
}

int wallpaper_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer };
    uint32_t all = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    /* exclusive_zone -1 : couvre tout l'écran, y compris sous la barre des tâches */
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, all, 0, 0, -1,
                          ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE, "nexus-wallpaper");
    if (!win) return 1;
    nx_run();
    return 0;
}
