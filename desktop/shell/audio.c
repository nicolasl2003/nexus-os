/* Son de Nexus OS : volume via la bibliothèque ALSA, et le panneau "Son"
 * (nexus-volume) ouvert depuis l'icône de haut-parleur de la barre des tâches.
 *   - curseur de volume (clic, glisser, molette)
 *   - bouton muet
 */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include "audio.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>

/* ---------- Accès au mélangeur ALSA ---------- */

static snd_mixer_t *mixer;
static snd_mixer_elem_t *elem;

static void silent(const char *f, int l, const char *fn, int e, const char *fmt, ...)
{ (void)f; (void)l; (void)fn; (void)e; (void)fmt; }

static bool mixer_ready(void)
{
    if (elem) { snd_mixer_handle_events(mixer); return true; }
    if (mixer) return false;
    snd_lib_error_set_handler(silent);
    if (snd_mixer_open(&mixer, 0) < 0) return false;
    if (snd_mixer_attach(mixer, "default") < 0 || snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
        snd_mixer_load(mixer) < 0) {
        snd_mixer_close(mixer);
        mixer = NULL;
        return false;
    }
    /* Réglage principal : Master, sinon PCM, sinon le premier qui a un volume */
    static const char *names[] = { "Master", "PCM", "Speaker", "Headphone" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0] && !elem; i++)
        for (snd_mixer_elem_t *e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e))
            if (!strcmp(snd_mixer_selem_get_name(e), names[i]) && snd_mixer_selem_has_playback_volume(e)) {
                elem = e;
                break;
            }
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(mixer); e && !elem; e = snd_mixer_elem_next(e))
        if (snd_mixer_selem_has_playback_volume(e)) elem = e;
    return elem != NULL;
}

bool nx_audio_available(void) { return mixer_ready(); }

bool nx_volume_get(int *pct, bool *muted)
{
    *pct = 0;
    *muted = false;
    if (!mixer_ready()) return false;
    long min = 0, max = 100, v = 0;
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &v);
    *pct = max > min ? (int)lround(100.0 * (v - min) / (max - min)) : 0;
    if (snd_mixer_selem_has_playback_switch(elem)) {
        int sw = 1;
        snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw);
        *muted = !sw;
    }
    return true;
}

void nx_volume_set(int pct)
{
    if (!mixer_ready()) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    long min = 0, max = 100;
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_set_playback_volume_all(elem, min + (max - min) * pct / 100);
    if (snd_mixer_selem_has_playback_switch(elem)) snd_mixer_selem_set_playback_switch_all(elem, pct > 0);
}

void nx_volume_set_mute(bool mute)
{
    if (mixer_ready() && snd_mixer_selem_has_playback_switch(elem))
        snd_mixer_selem_set_playback_switch_all(elem, !mute);
}

const char *nx_volume_icon(void)
{
    int v;
    bool m;
    if (!nx_volume_get(&v, &m)) return NULL;
    if (m || v == 0) return "volume-mute";
    return v < 34 ? "volume-1" : v < 67 ? "volume-2" : "volume-3";
}

/* ---------- Panneau "Son" ---------- */

static struct nx_window *win;
static bool dragging;
static int hover;   /* 0 rien, 1 bouton muet, 2 curseur */
static char card_name[96] = "Sortie audio";

#define BOX_W 360
#define BOX_H 150

static void box(int width, int height, double *x, double *y)
{
    *x = width - BOX_W - 10;
    *y = height - BOX_H - 10;
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    double bx, by;
    box(width, height, &bx, &by);
    nx_glass(cr, bx, by, BOX_W, BOX_H, 16);
    nx_color text = nx_rgb(NX_GLASS_TEXT), subtle = nx_rgb(NX_GLASS_SUBTLE), accent = nx_accent_color();
    nx_text(cr, "Son", 16, true, text, bx + 20, by + 16, 0, 24, NX_LEFT);
    nx_text(cr, card_name, 12, false, subtle, bx + 20, by + 40, BOX_W - 40, 20, NX_LEFT);
    int v;
    bool m;
    if (!nx_volume_get(&v, &m)) {
        nx_text(cr, "Aucune carte son détectée.", 13.5, false, subtle, bx + 20, by + 80, BOX_W - 40, 24, NX_LEFT);
        return;
    }
    /* bouton muet */
    double mx = bx + 18, my = by + 78;
    nx_rounded_rect(cr, mx, my, 44, 44, 12);
    nx_set(cr, nx_fg(hover == 1 ? 0.16 : 0.08));
    cairo_fill(cr);
    nx_icon(cr, nx_volume_icon(), mx + 10, my + 10, 24, nx_fg(1));
    /* curseur */
    double sx = bx + 80, sw = BOX_W - 150, sy = my + 22;
    nx_rounded_rect(cr, sx, sy - 3, sw, 6, 3);
    nx_set(cr, nx_fg(0.15));
    cairo_fill(cr);
    double fill = sw * (m ? 0 : v) / 100.0;
    nx_rounded_rect(cr, sx, sy - 3, fmax(fill, 6), 6, 3);
    nx_set(cr, m ? nx_rgb("#5a6170") : accent);
    cairo_fill(cr);
    cairo_new_path(cr);
    cairo_arc(cr, sx + fill, sy, hover == 2 || dragging ? 10 : 8, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    char t[16];
    snprintf(t, sizeof t, m ? "Muet" : "%d %%", v);
    nx_text(cr, t, 14, true, text, sx + sw + 10, my, 60, 44, NX_LEFT);
}

static int zone(double x, double y)
{
    double bx, by;
    box(win->width, win->height, &bx, &by);
    if (x < bx || y < by || x > bx + BOX_W || y > by + BOX_H) return -1;
    double my = by + 78;
    if (x >= bx + 18 && x < bx + 62 && y >= my && y < my + 44) return 1;
    if (x >= bx + 70 && x < bx + BOX_W - 60 && y >= my && y < my + 44) return 2;
    return 0;
}

static void set_from_x(double x)
{
    double bx, by;
    box(win->width, win->height, &bx, &by);
    double sx = bx + 80, sw = BOX_W - 150;
    nx_volume_set((int)lround(100 * (x - sx) / sw));
    nx_notify_reload();   /* la barre des tâches met son icône à jour */
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    int z = zone(x, y);
    switch (ev) {
    case NX_MOTION:
        if (dragging) set_from_x(x);
        if (z != hover || dragging) { hover = z; nx_redraw(w); }
        break;
    case NX_PRESS:
        if (value != BTN_LEFT) break;
        if (z == -1) { nx_quit(); break; }
        if (z == 2) { dragging = true; set_from_x(x); nx_redraw(w); }
        break;
    case NX_RELEASE:
        if (dragging) { dragging = false; nx_redraw(w); break; }
        if (z == 1 && value == BTN_LEFT) {
            int v;
            bool m;
            nx_volume_get(&v, &m);
            nx_volume_set_mute(!m);
            nx_notify_reload();
            nx_redraw(w);
        }
        break;
    case NX_SCROLL: {
        int v;
        bool m;
        nx_volume_get(&v, &m);
        nx_volume_set(v - value * 5);
        nx_notify_reload();
        nx_redraw(w);
        break;
    }
    case NX_LEAVE: hover = 0; nx_redraw(w); break;
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    (void)utf8;
    int v;
    bool m;
    nx_volume_get(&v, &m);
    if (sym == XKB_KEY_Escape) nx_quit();
    else if (sym == XKB_KEY_Right || sym == XKB_KEY_Up) nx_volume_set(v + 5);
    else if (sym == XKB_KEY_Left || sym == XKB_KEY_Down) nx_volume_set(v - 5);
    else return;
    nx_notify_reload();
    nx_redraw(w);
}

int volume_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (nx_toggle_instance("nexus-volume")) return 0;
    char *name = NULL;
    if (snd_card_get_name(0, &name) == 0 && name) { snprintf(card_name, sizeof card_name, "%s", name); free(name); }
    if (!nx_init()) { nx_instance_done(); return 1; }
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          0, 0, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, "nexus-volume");
    if (win) nx_run();
    nx_instance_done();
    return 0;
}
