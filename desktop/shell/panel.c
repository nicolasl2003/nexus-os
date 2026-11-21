/* nexus-panel — la barre des tâches de Nexus OS (en bas, style "verre" sombre).
 *
 *  [N] [loupe] [Fichiers][Terminal][Paramètres]...  | fenêtres |      (réseau)  14:37
 *                                                                           27/10/2025
 * - N : menu Démarrer ; loupe : menu Démarrer en mode recherche
 * - applications épinglées : clic = lancer, ou afficher/réduire si déjà ouverte ;
 *   un trait sous l'icône signale une fenêtre ouverte (large et coloré = active)
 * - les autres fenêtres ouvertes suivent, en icônes ; clic du milieu = fermer
 * - à droite : état du réseau, heure et date (mises à jour chaque minute)
 */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include "audio.h"
#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

/* Dimensions selon la taille choisie dans Paramètres (petite/moyenne/grande) */
static int cur_h = 52;
#define PANEL_H cur_h
#define BTN_W   (cur_h - 6)
#define BTN_H   (cur_h - 12)
#define ICON    (cur_h * 0.54 * nx_icon_factor())
#define SMALL   (cur_h * 22.0 / 52)
#define GRIP    6          /* bord supérieur : tirer pour changer la hauteur */
/* Texte proportionnel à la hauteur de la barre */
static double tk(void) { double k = cur_h / 52.0; return k < 0.85 ? 0.85 : k > 1.7 ? 1.7 : k; }

static bool resizing, grip_hover;
static double grip_y0;   /* position du curseur au début (repère indépendant de la taille) */
static int grip_h0;

struct task {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char title[256], app_id[128];
    bool activated, minimized, ready;
    struct task *next;
};
static struct task *tasks;
static struct nx_window *panel;

/* Applications épinglées */
struct pin { const char *label, *icon, *app_id, *cmd, *needs; };
static const struct pin all_pins[] = {
    { "Fichiers", "files", "nexus-files", "nexus-files", NULL },
    { "Navigateur", "browser", "nexus-browser", "nexus-browser", "/usr/local/bin/netsurf-fb" },
    { "Terminal", "terminal", "foot", "foot", NULL },
    { "Paramètres", "settings", "nexus-settings", "nexus-settings", NULL },
    { "Moniteur système", "monitor", "nexus-monitor",
      "foot --app-id=nexus-monitor --title='Moniteur système' htop", "/usr/local/bin/htop" },
    { "Magasin d'applications", "store", "nexus-store", "nexus-store", NULL },
};
static const struct pin *pins[8];
static int npins;

/* Boutons de la barre, recalculés à chaque dessin */
enum bkind { B_START, B_SEARCH, B_PIN, B_TASK, B_NET, B_VOL, B_CLOCK };
struct btn { enum bkind kind; int index; double x, w; };
static struct btn btns[64];
static int nbtns;
static int hover = -1, pressed = -1;

/* ---------- Fenêtres ouvertes (wlr-foreign-toplevel-management) ---------- */

static void t_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; struct task *t = d; snprintf(t->title, sizeof t->title, "%s", s); }
static void t_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; struct task *t = d; snprintf(t->app_id, sizeof t->app_id, "%s", s); }
static void t_output(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{ (void)d; (void)h; (void)o; }
static void t_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st)
{
    (void)h;
    struct task *t = d;
    t->activated = t->minimized = false;
    uint32_t *s;
    wl_array_for_each(s, st) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) t->activated = true;
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) t->minimized = true;
    }
}
static void t_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{ (void)h; struct task *t = d; t->ready = true; nx_redraw(panel); }
static void t_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    struct task *t = d, **pp = &tasks;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp) *pp = t->next;
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    free(t);
    hover = pressed = -1;
    nx_redraw(panel);
}
static void t_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p)
{ (void)d; (void)h; (void)p; }
static const struct zwlr_foreign_toplevel_handle_v1_listener task_listener = {
    .title = t_title, .app_id = t_app_id, .output_enter = t_output, .output_leave = t_output,
    .state = t_state, .done = t_done, .closed = t_closed, .parent = t_parent,
};
static void m_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)d; (void)m;
    struct task *t = calloc(1, sizeof *t), **pp = &tasks;
    t->handle = h;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &task_listener, t);
}
static void m_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d; (void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = m_toplevel, .finished = m_finished,
};

static const struct pin *pin_of(const struct task *t)
{
    for (int i = 0; i < npins; i++)
        if (!strcmp(t->app_id, pins[i]->app_id) || (!strcmp(pins[i]->app_id, "foot") && !strcmp(t->app_id, "footclient")))
            return pins[i];
    return NULL;
}

static const char *task_icon(const struct task *t)
{
    const char *id = nx_app_icon_for(t->app_id);
    if (strcmp(id, t->app_id)) return id;          /* application connue */
    return t->title[0] ? t->title : t->app_id;     /* sinon : initiale du titre */
}

/* Clic sur une application : lancer, activer, ou réduire */
static void toggle_task(struct task *t)
{
    if (t->activated && !t->minimized) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
    } else {
        if (t->minimized) zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
        zwlr_foreign_toplevel_handle_v1_activate(t->handle, nx.seat);
    }
}

static void click_pin(const struct pin *p)
{
    struct task *first = NULL, *active = NULL;
    for (struct task *t = tasks; t; t = t->next) {
        if (!t->ready || pin_of(t) != p) continue;
        if (!first) first = t;
        if (t->activated && !t->minimized) active = t;
    }
    if (!first) { nx_spawn(p->cmd); return; }
    if (active) {
        /* plusieurs fenêtres : passer à la suivante, sinon réduire */
        struct task *next = NULL;
        for (struct task *t = active->next; t && !next; t = t->next)
            if (t->ready && pin_of(t) == p) next = t;
        if (next) toggle_task(next); else toggle_task(active);
    } else {
        toggle_task(first);
    }
}

/* ---------- Dessin ---------- */

static void add_btn(enum bkind k, int index, double x, double w)
{
    if (nbtns < 64) btns[nbtns++] = (struct btn){ k, index, x, w };
}

static void button_bg(cairo_t *cr, double x, double w, int index)
{
    if (hover != index && pressed != index) return;
    nx_rounded_rect(cr, x, (PANEL_H - BTN_H) / 2.0, w, BTN_H, 8);
    cairo_set_source_rgba(cr, 1, 1, 1, pressed == index ? 0.16 : 0.10);
    cairo_fill(cr);
}

static void indicator(cairo_t *cr, double cx, int count, bool active)
{
    if (!count) return;
    double w = active ? 16 : 6;
    nx_rounded_rect(cr, cx - w / 2, PANEL_H - 7, w, 3, 1.5);
    if (active) nx_set(cr, nx_mix(nx_accent_color(), nx_rgb("#ffffff"), 0.3));
    else cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
    cairo_fill(cr);
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nbtns = 0;
    nx_color white = nx_rgb("#ffffff");

    /* Fond : verre sombre */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    nx_glass_source(cr, 0, height);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_rectangle(cr, 0, 0, width, 1);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_fill(cr);

    double x = 8;
    /* Bouton Démarrer et recherche */
    button_bg(cr, x, BTN_W, nbtns);
    nx_logo(cr, x + (BTN_W - ICON) / 2.0, (PANEL_H - ICON) / 2.0, ICON);
    add_btn(B_START, 0, x, BTN_W);
    x += BTN_W + 2;
    button_bg(cr, x, BTN_W, nbtns);
    nx_icon(cr, "search", x + (BTN_W - SMALL) / 2.0, (PANEL_H - SMALL) / 2.0, SMALL, white);
    add_btn(B_SEARCH, 0, x, BTN_W);
    x += BTN_W + 10;
    cairo_rectangle(cr, x - 6, PANEL_H * 0.27, 1, PANEL_H * 0.46);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_fill(cr);

    /* Applications épinglées */
    for (int i = 0; i < npins; i++) {
        int count = 0;
        bool active = false;
        for (struct task *t = tasks; t; t = t->next)
            if (t->ready && pin_of(t) == pins[i]) { count++; active |= t->activated && !t->minimized; }
        button_bg(cr, x, BTN_W, nbtns);
        nx_app_icon(cr, pins[i]->icon, x + (BTN_W - ICON) / 2.0, (PANEL_H - ICON) / 2.0 - 2, ICON, white);
        indicator(cr, x + BTN_W / 2.0, count, active);
        add_btn(B_PIN, i, x, BTN_W);
        x += BTN_W + 2;
    }
    /* Autres fenêtres ouvertes */
    int ti = 0;
    for (struct task *t = tasks; t; t = t->next, ti++) {
        if (!t->ready || pin_of(t)) continue;
        if (x + BTN_W > width - 200) break;
        button_bg(cr, x, BTN_W, nbtns);
        nx_app_icon(cr, task_icon(t), x + (BTN_W - ICON) / 2.0, (PANEL_H - ICON) / 2.0 - 2, ICON, white);
        indicator(cr, x + BTN_W / 2.0, 1, t->activated && !t->minimized);
        add_btn(B_TASK, ti, x, BTN_W);
        x += BTN_W + 2;
    }

    /* Zone de droite : réseau, heure, date */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8], date[16];
    snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(date, sizeof date, "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    double k = tk(), cw = 96 * k, cx = width - cw - 8;
    button_bg(cr, cx, cw, nbtns);
    if (PANEL_H < 48) {           /* petite barre : l'heure seule */
        nx_text(cr, hm, 13.5 * k, true, white, cx, 0, cw, PANEL_H, NX_CENTER);
    } else {
        nx_text(cr, hm, 13.5 * k, true, white, cx, PANEL_H / 2.0 - 20 * k, cw, 21 * k, NX_CENTER);
        nx_text(cr, date, 11.5 * k, false, nx_alpha(white, 0.75), cx, PANEL_H / 2.0, cw, 19 * k, NX_CENTER);
    }
    add_btn(B_CLOCK, 0, cx, cw);
    /* Son (si une carte son est présente) */
    double sw = 40 * k, si = 22 * k, rx = cx - sw - 4;
    const char *vol = nx_volume_icon();
    if (vol) {
        button_bg(cr, rx, sw, nbtns);
        nx_icon(cr, vol, rx + (sw - si) / 2, (PANEL_H - si) / 2.0, si, white);
        add_btn(B_VOL, 0, rx, sw);
        rx -= sw + 2;
    }
    /* Réseau : Ethernet, Wi-Fi, ou déconnecté */
    bool up;
    const char *net = nx_network_icon(&up);
    button_bg(cr, rx, sw, nbtns);
    nx_icon(cr, net, rx + (sw - si) / 2, (PANEL_H - si) / 2.0, si, nx_alpha(white, up ? 0.95 : 0.35));
    if (!up) {
        cairo_set_source_rgb(cr, 1, 0.4, 0.4);
        cairo_set_line_width(cr, 2);
        double my = PANEL_H / 2.0 + 4;
        double mx = rx + (sw + si) / 2 - 7 * k;
        cairo_move_to(cr, mx, my); cairo_line_to(cr, mx + 7 * k, my + 7 * k);
        cairo_move_to(cr, mx + 7 * k, my); cairo_line_to(cr, mx, my + 7 * k);
        cairo_stroke(cr);
    }
    add_btn(B_NET, 0, rx, sw);

    /* Poignée de redimensionnement : liseré coloré sur le bord supérieur */
    if (grip_hover || resizing) {
        cairo_rectangle(cr, 0, 0, width, 3);
        nx_set(cr, nx_accent_color());
        cairo_fill(cr);
        nx_rounded_rect(cr, width / 2.0 - 30, 4, 60, 4, 2);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
        cairo_fill(cr);
    }
}

static void set_height(int h)
{
    if (h < 36) h = 36;
    if (h > 110) h = 110;
    if (h == cur_h) return;
    cur_h = h;
    zwlr_layer_surface_v1_set_size(panel->layer, 0, h);
    zwlr_layer_surface_v1_set_exclusive_zone(panel->layer, h);
    wl_surface_commit(panel->surface);
}

/* ---------- Souris ---------- */

static int hit(double x)
{
    for (int i = 0; i < nbtns; i++)
        if (x >= btns[i].x && x < btns[i].x + btns[i].w) return i;
    return -1;
}

static struct task *task_at(int index)
{
    int i = 0;
    for (struct task *t = tasks; t; t = t->next, i++) if (i == index) return t;
    return NULL;
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    int h = hit(x);
    /* Redimensionnement en tirant le bord supérieur */
    if (resizing) {
        if (ev == NX_MOTION) {
            /* y est relatif à la barre, dont le haut bouge : y - hauteur ne dépend que du curseur */
            set_height(grip_h0 - (int)((y - cur_h) - grip_y0));
            nx_redraw(w);
        } else if (ev == NX_RELEASE) {
            resizing = false;
            nx_conf.panel_height = cur_h;
            nx_config_save(&nx_conf);
            nx_notify_reload();
            nx_redraw(w);
        }
        return;
    }
    bool g = ev != NX_LEAVE && y < GRIP;
    if (g != grip_hover) { grip_hover = g; nx_redraw(w); }
    if (g && ev == NX_PRESS && value == BTN_LEFT) {
        resizing = true;
        grip_h0 = cur_h;
        grip_y0 = y - cur_h;
        return;
    }
    if (g) return;
    switch (ev) {
    case NX_MOTION: if (h != hover) { hover = h; nx_redraw(w); } break;
    case NX_LEAVE: hover = pressed = -1; nx_redraw(w); break;
    case NX_PRESS: pressed = h; nx_redraw(w); break;
    case NX_RELEASE:
        if (h >= 0 && h == pressed) {
            struct btn *b = &btns[h];
            if (value == BTN_LEFT) {
                switch (b->kind) {
                case B_START: nx_spawn("nexus-menu"); break;
                case B_SEARCH: nx_spawn("nexus-menu --search"); break;
                case B_PIN: click_pin(pins[b->index]); break;
                case B_TASK: { struct task *t = task_at(b->index); if (t) toggle_task(t); break; }
                case B_NET: nx_spawn("nexus-network"); break;
                case B_VOL: nx_spawn("nexus-volume"); break;
                case B_CLOCK: nx_spawn("nexus-settings --about"); break;
                }
            } else if (value == BTN_MIDDLE && b->kind == B_TASK) {
                struct task *t = task_at(b->index);
                if (t) zwlr_foreign_toplevel_handle_v1_close(t->handle);
            }
        }
        pressed = -1;
        nx_redraw(w);
        break;
    default: break;
    }
}

/* ---------- Horloge : réveil à chaque nouvelle minute ---------- */

static int until_next_minute(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int ms = (int)((60 - ts.tv_sec % 60) * 1000 - ts.tv_nsec / 1000000) + 50;
    return ms < 5000 ? ms : 5000;   /* état du réseau et du son rafraîchi toutes les 5 s */
}
static void tick(void) { nx_redraw(panel); }

/* Applications épinglées présentes (le navigateur peut être installé ensuite) */
static void load_pins(void)
{
    npins = 0;
    for (unsigned i = 0; i < sizeof all_pins / sizeof all_pins[0] && npins < 8; i++)
        if (!all_pins[i].needs || access(all_pins[i].needs, X_OK) == 0) pins[npins++] = &all_pins[i];
}

/* Réglages modifiés (SIGUSR1) : hauteur de barre, applications installées */
static void reload(void)
{
    load_pins();
    if (panel && !resizing) set_height(nx_panel_height());
    nx_redraw(panel);
}

int panel_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    load_pins();
    if (!nx_init()) return 1;
    cur_h = nx_panel_height();
    struct nx_handlers h = { .draw = draw, .pointer = pointer };
    panel = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                            0, PANEL_H, PANEL_H,
                            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE, "nexus-panel");
    if (!panel) return 1;
    if (nx.toplevels) zwlr_foreign_toplevel_manager_v1_add_listener(nx.toplevels, &manager_listener, NULL);
    nx.timeout = until_next_minute;
    nx.tick = tick;
    nx.reload = reload;
    nx_run();
    return 0;
}
