/*
 * nexus-display : règle la taille de l'affichage (échelle de l'écran).
 *
 *   nexus-display            réglage de l'utilisateur (Paramètres → Affichage)
 *   nexus-display auto       selon la taille de l'écran
 *   nexus-display 150        150 % (de 100 à 200)
 *
 * Passe par le protocole wlr-output-management de labwc : tout grandit d'un
 * coup (barre de titre, Firefox, terminal, applications Nexus), et chaque
 * application dessine directement à la bonne taille (texte net).
 */
#include "nexus-ui.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mode { struct zwlr_output_mode_v1 *m; int w, h; };
struct head {
    struct zwlr_output_head_v1 *h;
    bool enabled;
    struct zwlr_output_mode_v1 *cur;
    double scale;
};

static struct zwlr_output_manager_v1 *manager;
static struct head heads[8];
static int nheads;
static struct mode modes[128];
static int nmodes;
static uint32_t serial;
static bool got_done;
static int result;   /* 0 en attente, 1 appliqué, -1 refusé */

/* Échelle automatique : environ 1333 x 800 points logiques (la taille pour
 * laquelle le bureau est dessiné), arrondie au quart, entre 1 et 2. */
double nx_display_auto_scale(int w, int h)
{
    double s = fmin(w / 1333.0, h / 800.0);
    s = round(s * 4) / 4;
    if (s < 1) s = 1;
    if (s > 2) s = 2;
    return s;
}

/* ---- modes ---- */
static void mode_size(void *d, struct zwlr_output_mode_v1 *m, int32_t w, int32_t h)
{
    (void)d;
    for (int i = 0; i < nmodes; i++)
        if (modes[i].m == m) { modes[i].w = w; modes[i].h = h; }
}
static void mode_refresh(void *d, struct zwlr_output_mode_v1 *m, int32_t r) { (void)d; (void)m; (void)r; }
static void mode_preferred(void *d, struct zwlr_output_mode_v1 *m) { (void)d; (void)m; }
static void mode_finished(void *d, struct zwlr_output_mode_v1 *m) { (void)d; (void)m; }
static const struct zwlr_output_mode_v1_listener mode_listener = {
    .size = mode_size, .refresh = mode_refresh,
    .preferred = mode_preferred, .finished = mode_finished,
};

/* ---- écrans ---- */
static struct head *head_of(struct zwlr_output_head_v1 *h)
{
    for (int i = 0; i < nheads; i++) if (heads[i].h == h) return &heads[i];
    return NULL;
}
static void head_name(void *d, struct zwlr_output_head_v1 *h, const char *s) { (void)d; (void)h; (void)s; }
static void head_desc(void *d, struct zwlr_output_head_v1 *h, const char *s) { (void)d; (void)h; (void)s; }
static void head_phys(void *d, struct zwlr_output_head_v1 *h, int32_t w, int32_t hh) { (void)d; (void)h; (void)w; (void)hh; }
static void head_mode(void *d, struct zwlr_output_head_v1 *h, struct zwlr_output_mode_v1 *m)
{
    (void)d; (void)h;
    if (nmodes < (int)(sizeof modes / sizeof modes[0])) {
        modes[nmodes++] = (struct mode){ m, 0, 0 };
        zwlr_output_mode_v1_add_listener(m, &mode_listener, NULL);
    }
}
static void head_enabled(void *d, struct zwlr_output_head_v1 *h, int32_t e)
{
    (void)d;
    struct head *x = head_of(h);
    if (x) x->enabled = e;
}
static void head_current(void *d, struct zwlr_output_head_v1 *h, struct zwlr_output_mode_v1 *m)
{
    (void)d;
    struct head *x = head_of(h);
    if (x) x->cur = m;
}
static void head_pos(void *d, struct zwlr_output_head_v1 *h, int32_t x, int32_t y) { (void)d; (void)h; (void)x; (void)y; }
static void head_transform(void *d, struct zwlr_output_head_v1 *h, int32_t t) { (void)d; (void)h; (void)t; }
static void head_scale(void *d, struct zwlr_output_head_v1 *h, wl_fixed_t s)
{
    (void)d;
    struct head *x = head_of(h);
    if (x) x->scale = wl_fixed_to_double(s);
}
static void head_finished(void *d, struct zwlr_output_head_v1 *h) { (void)d; (void)h; }
static const struct zwlr_output_head_v1_listener head_listener = {
    .name = head_name, .description = head_desc, .physical_size = head_phys,
    .mode = head_mode, .enabled = head_enabled, .current_mode = head_current,
    .position = head_pos, .transform = head_transform, .scale = head_scale,
    .finished = head_finished,
};

/* ---- gestionnaire ---- */
static void man_head(void *d, struct zwlr_output_manager_v1 *m, struct zwlr_output_head_v1 *h)
{
    (void)d; (void)m;
    if (nheads < 8) {
        heads[nheads++] = (struct head){ .h = h, .scale = 1 };
        zwlr_output_head_v1_add_listener(h, &head_listener, NULL);
    }
}
static void man_done(void *d, struct zwlr_output_manager_v1 *m, uint32_t s)
{
    (void)d; (void)m;
    serial = s;
    got_done = true;
}
static void man_finished(void *d, struct zwlr_output_manager_v1 *m) { (void)d; (void)m; }
static const struct zwlr_output_manager_v1_listener man_listener = {
    .head = man_head, .done = man_done, .finished = man_finished,
};

static void cfg_ok(void *d, struct zwlr_output_configuration_v1 *c) { (void)d; (void)c; result = 1; }
static void cfg_fail(void *d, struct zwlr_output_configuration_v1 *c) { (void)d; (void)c; result = -1; }
static const struct zwlr_output_configuration_v1_listener cfg_listener = {
    .succeeded = cfg_ok, .failed = cfg_fail, .cancelled = cfg_fail,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d; (void)ver;
    if (!strcmp(iface, zwlr_output_manager_v1_interface.name))
        manager = wl_registry_bind(r, name, &zwlr_output_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

int display_main(int argc, char **argv)
{
    int pct;   /* 0 : automatique */
    if (argc > 1 && !strcmp(argv[1], "auto")) pct = 0;
    else if (argc > 1) {
        pct = atoi(argv[1]);
        if (pct < 100 || pct > 200) {
            fprintf(stderr, "Usage : nexus-display [auto|100..200]\n");
            return 2;
        }
    } else {
        nx_config_load(&nx_conf);
        pct = nx_conf.display_scale;
    }

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "nexus-display : pas de session graphique.\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!manager) { fprintf(stderr, "nexus-display : le compositeur ne permet pas de régler l'écran.\n"); return 1; }
    zwlr_output_manager_v1_add_listener(manager, &man_listener, NULL);
    while (!got_done && wl_display_dispatch(dpy) != -1) { }

    bool change = false;
    double want[8];
    for (int i = 0; i < nheads; i++) {
        int w = 0, h = 0;
        for (int k = 0; k < nmodes; k++)
            if (modes[k].m == heads[i].cur) { w = modes[k].w; h = modes[k].h; }
        want[i] = pct ? pct / 100.0 : (w > 0 ? nx_display_auto_scale(w, h) : 1);
        if (heads[i].enabled && fabs(want[i] - heads[i].scale) > 0.001) change = true;
        if (heads[i].enabled)
            printf("Écran %d : %dx%d, échelle %.2f -> %.2f\n", i + 1, w, h, heads[i].scale, want[i]);
    }
    if (!change) return 0;

    struct zwlr_output_configuration_v1 *cfg = zwlr_output_manager_v1_create_configuration(manager, serial);
    zwlr_output_configuration_v1_add_listener(cfg, &cfg_listener, NULL);
    for (int i = 0; i < nheads; i++) {
        if (!heads[i].enabled) { zwlr_output_configuration_v1_disable_head(cfg, heads[i].h); continue; }
        struct zwlr_output_configuration_head_v1 *ch = zwlr_output_configuration_v1_enable_head(cfg, heads[i].h);
        zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(want[i]));
    }
    zwlr_output_configuration_v1_apply(cfg);
    while (!result && wl_display_dispatch(dpy) != -1) { }
    wl_display_disconnect(dpy);
    if (result < 0) { fprintf(stderr, "nexus-display : réglage refusé par le compositeur.\n"); return 1; }
    return 0;
}
