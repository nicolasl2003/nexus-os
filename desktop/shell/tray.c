/*
 * nexus-tray <x> — panneau de la flèche « ^ » de la barre des tâches.
 * Montre le logo des logiciels externes à Nexus OS qui sont ouverts
 * (Firefox, programmes Alpine...) : un clic affiche la fenêtre, un clic
 * droit propose de la fermer ou de l'épingler. En bas : Paramètres.
 */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include "pins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>

struct ttask {
    struct zwlr_foreign_toplevel_handle_v1 *h;
    char app_id[128], title[256];
    bool ready, closed;
    struct ttask *next;
};
static struct ttask *tasks;
static struct nx_window *win;
static double anchor_x;
static int hover = -1;

static void t_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; struct ttask *t = d; snprintf(t->title, sizeof t->title, "%s", s); }
static void t_app(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; struct ttask *t = d; snprintf(t->app_id, sizeof t->app_id, "%s", s); }
static void t_out(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d; (void)h; (void)o; }
static void t_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *a) { (void)d; (void)h; (void)a; }
static void t_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{ (void)h; struct ttask *t = d; t->ready = true; if (win) nx_redraw(win); }
static void t_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{ (void)h; struct ttask *t = d; t->closed = true; if (win) nx_redraw(win); }
static void t_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p)
{ (void)d; (void)h; (void)p; }
static const struct zwlr_foreign_toplevel_handle_v1_listener t_listener = {
    .title = t_title, .app_id = t_app, .output_enter = t_out, .output_leave = t_out,
    .state = t_state, .done = t_done, .closed = t_closed, .parent = t_parent,
};
static void m_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)d; (void)m;
    struct ttask *t = calloc(1, sizeof *t);
    t->h = h;
    /* ajout en fin de liste : ordre d'ouverture */
    struct ttask **pp = &tasks;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &t_listener, t);
}
static void m_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d; (void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener m_listener = {
    .toplevel = m_toplevel, .finished = m_finished,
};

/* Une seule icône par logiciel (même s'il a plusieurs fenêtres) */
#define MAXI 24
static struct ttask *items[MAXI];
static int nitems;

static void collect(void)
{
    nitems = 0;
    for (struct ttask *t = tasks; t && nitems < MAXI; t = t->next) {
        if (!t->ready || t->closed || nx_is_nexus_app(t->app_id)) continue;
        bool dup = false;
        for (int i = 0; i < nitems; i++) if (!strcmp(items[i]->app_id, t->app_id)) dup = true;
        if (!dup) items[nitems++] = t;
    }
}

#define COLS 4
#define CELL 84
#define BOX_W (COLS * CELL + 24)

static void box(int width, int height, double *x, double *y, double *h)
{
    int rows = nitems ? (nitems + COLS - 1) / COLS : 1;
    *h = 52 + rows * (CELL + 10) + 48;
    *x = anchor_x - BOX_W / 2.0;
    if (*x + BOX_W > width - 8) *x = width - 8 - BOX_W;
    if (*x < 8) *x = 8;
    *y = height - *h - 10;
}

/* Zone sous la souris : 0..nitems-1 icône, 100 Paramètres, -1 dans le cadre, -2 dehors */
static int zone(double px, double py)
{
    double bx, by, bh;
    box(win->width, win->height, &bx, &by, &bh);
    if (px < bx || px >= bx + BOX_W || py < by || py >= by + bh) return -2;
    for (int i = 0; i < nitems; i++) {
        double x = bx + 12 + (i % COLS) * CELL, y = by + 48 + (i / COLS) * (CELL + 10);
        if (px >= x && px < x + CELL && py >= y && py < y + CELL + 6) return i;
    }
    if (py >= by + bh - 46) return 100;
    return -1;
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    collect();
    double bx, by, bh;
    box(width, height, &bx, &by, &bh);
    nx_glass(cr, bx, by, BOX_W, bh, 16);
    nx_color text = nx_rgb(NX_GLASS_TEXT), subtle = nx_rgb(NX_GLASS_SUBTLE), accent = nx_accent_color();
    nx_text(cr, "Logiciels ouverts", 15, true, text, bx + 18, by + 14, 0, 24, NX_LEFT);
    if (!nitems)
        nx_text(cr, "Aucun logiciel externe à Nexus OS n'est ouvert.", 12.5, false, subtle,
                bx + 18, by + 52, BOX_W - 36, CELL, NX_LEFT);
    for (int i = 0; i < nitems; i++) {
        double x = bx + 12 + (i % COLS) * CELL, y = by + 48 + (i / COLS) * (CELL + 10);
        if (i == hover) {
            nx_rounded_rect(cr, x + 2, y, CELL - 4, CELL + 6, 12);
            nx_set(cr, nx_fg(0.12));
            cairo_fill(cr);
        }
        nx_app_icon(cr, items[i]->app_id, x + (CELL - 44) / 2, y + 8, 44, text);
        /* nom : celui du .desktop, sinon l'app_id */
        char name[128];
        if (!nx_desktop_entry(items[i]->app_id, name, sizeof name, NULL, 0, NULL, 0))
            snprintf(name, sizeof name, "%s", items[i]->app_id);
        nx_text(cr, name, 11.5, false, text, x + 2, y + 58, CELL - 4, 22, NX_CENTER);
    }
    double sy = by + bh - 46;
    cairo_rectangle(cr, bx + 14, sy, BOX_W - 28, 1);
    nx_set(cr, nx_fg(0.12));
    cairo_fill(cr);
    if (hover == 100) {
        nx_rounded_rect(cr, bx + 8, sy + 6, BOX_W - 16, 34, 9);
        nx_set(cr, nx_alpha(accent, 0.85));
        cairo_fill(cr);
    }
    nx_icon(cr, "settings", bx + 20, sy + 14, 18, hover == 100 ? nx_rgb("#ffffff") : text);
    nx_text(cr, "Paramètres", 13.5, false, hover == 100 ? nx_rgb("#ffffff") : text,
            bx + 48, sy + 6, BOX_W - 60, 34, NX_LEFT);
}

static void activate(struct ttask *t)
{
    zwlr_foreign_toplevel_handle_v1_unset_minimized(t->h);
    zwlr_foreign_toplevel_handle_v1_activate(t->h, nx.seat);
    wl_display_flush(nx.display);
}

static void right_menu(struct ttask *t)
{
    char cmd[2048], qa[300], qt[600], a1[700], a2[1000], q1[800], q2[1100];
    nx_shell_quote(qa, sizeof qa, t->app_id);
    nx_shell_quote(qt, sizeof qt, t->title);
    snprintf(a2, sizeof a2, "nexus-pin close %s %s", qa, qt);
    nx_shell_quote(q2, sizeof q2, a2);
    struct nx_pin info;
    if (nx_pin_info(t->app_id, &info)) {
        snprintf(a1, sizeof a1, "nexus-pin add %s", qa);
        nx_shell_quote(q1, sizeof q1, a1);
        snprintf(cmd, sizeof cmd, "nexus-popup %d 'Épingler à la barre des tâches' %s 'Fermer la fenêtre' %s",
                 (int)anchor_x, q1, q2);
    } else {
        snprintf(cmd, sizeof cmd, "nexus-popup %d 'Fermer la fenêtre' %s", (int)anchor_x, q2);
    }
    nx_spawn(cmd);
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    int z = zone(x, y);
    switch (ev) {
    case NX_MOTION: if (z != hover) { hover = z; nx_redraw(w); } break;
    case NX_LEAVE: hover = -1; nx_redraw(w); break;
    case NX_PRESS: if (z == -2) nx_quit(); break;
    case NX_RELEASE:
        if (z >= 0 && z < nitems) {
            if (value == BTN_LEFT) activate(items[z]);
            else if (value == BTN_RIGHT) right_menu(items[z]);
            nx_quit();
        } else if (z == 100 && value == BTN_LEFT) {
            nx_spawn("nexus-settings");
            nx_quit();
        }
        break;
    default: break;
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    (void)w; (void)utf8;
    if (sym == XKB_KEY_Escape) nx_quit();
}

int tray_main(int argc, char **argv)
{
    if (nx_toggle_instance("nexus-tray")) return 0;
    nx.toplevel_listener = &m_listener;
    if (!nx_init()) { nx_instance_done(); return 1; }
    anchor_x = argc > 1 ? atof(argv[1]) : 1e9;
    wl_display_roundtrip(nx.display);
    wl_display_roundtrip(nx.display);
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          0, 0, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, "nexus-tray");
    if (win) nx_run();
    nx_instance_done();
    return 0;
}
