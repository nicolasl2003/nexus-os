/* nexus-panel — la barre des tâches de Nexus OS : un dock flottant centré.
 *
 *            ╭──────────────────────────────────╮
 *  ──────────│ N  🔍 │ Fichiers Navigateur ... │ fenêtres │──────  ^ 📶 🔊 🔋 │ 22:03
 *            ╰──────────────────────────────────╯                         22/11/2026
 * - N : menu Démarrer (il s'ouvre au-dessus, une flèche pointe vers N) ;
 *   loupe : menu Démarrer en mode recherche
 * - applications épinglées : clic = lancer, ou afficher/réduire si déjà ouverte ;
 *   un halo lumineux sous l'icône = fenêtre active, un point = fenêtre ouverte
 * - les autres fenêtres ouvertes suivent, après un séparateur ; clic du milieu = fermer
 * - à droite : raccourcis (^), réseau, son, batterie, heure et date
 * - tirer le bord supérieur de la barre change sa hauteur
 */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include "pins.h"
#include "audio.h"
#include "network.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

/* Hauteur de la barre (réglable) ; le dock dépasse un peu au-dessus */
static int cur_h = 66;
#define LIFT    0                        /* (débordement au-dessus : non, il capterait les clics) */
#define PANEL_H cur_h
#define DOCK_H  (cur_h - 8.0)
#define CELL    (DOCK_H * 0.98)          /* largeur d'une case du dock */
#define ICON    (DOCK_H * 0.68 * nx_icon_factor())
#define GRIP    6
/* Texte proportionnel à la hauteur de la barre */
static double tk(void) { double k = cur_h / 60.0; return k < 0.8 ? 0.8 : k > 1.6 ? 1.6 : k; }

static bool resizing, grip_hover;
static double grip_y0;
static int grip_h0;

struct task {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char title[256], app_id[128];
    bool activated, minimized, ready;
    struct task *next;
};
static struct task *tasks;
static struct nx_window *panel;

/* Applications épinglées (liste du compte, modifiable par clic droit) */
static struct nx_pin pins[NX_MAX_PINS];
static int npins;

/* Boutons de la barre, recalculés à chaque dessin */
enum bkind { B_START, B_SEARCH, B_PIN, B_TASK, B_NET, B_VOL, B_CLOCK, B_MORE, B_BAT };
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

static const struct nx_pin *pin_of(const struct task *t)
{
    for (int i = 0; i < npins; i++)
        if (!strcmp(t->app_id, pins[i].app_id) || (!strcmp(pins[i].app_id, "foot") && !strcmp(t->app_id, "footclient")))
            return &pins[i];
    return NULL;
}

static const char *task_icon(const struct task *t)
{
    if (nx_external_icon(t->app_id)) return t->app_id;   /* vrai logo (Firefox...) */
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

static void click_pin(const struct nx_pin *p)
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

/* Fond d'un bouton du dock survolé / pressé */
static void button_bg(cairo_t *cr, double x, double w, double y, double h, int index)
{
    if (hover != index && pressed != index) return;
    nx_rounded_rect(cr, x + 3, y, w - 6, h, h * 0.28);
    nx_set(cr, nx_fg(pressed == index ? 0.18 : 0.11));
    cairo_fill(cr);
}

/* Sous l'icône : halo coloré (fenêtre active) ou petit point (ouverte) */
static void indicator(cairo_t *cr, double cx, double y, int count, bool active)
{
    if (!count) return;
    nx_color a = nx_accent_color();
    if (active) {
        for (int i = 6; i >= 1; i--) {                     /* lueur */
            nx_rounded_rect(cr, cx - 10 - i * 1.6, y - i * 0.9, 20 + i * 3.2, 4 + i * 1.8, 2 + i);
            nx_set(cr, nx_alpha(a, 0.07));
            cairo_fill(cr);
        }
        nx_rounded_rect(cr, cx - 10, y, 20, 4, 2);
        nx_set(cr, nx_mix(a, nx_rgb("#ffffff"), 0.35));
        cairo_fill(cr);
    } else {
        cairo_new_path(cr);
        cairo_arc(cr, cx, y + 2, 2.4, 0, 2 * M_PI);
        nx_set(cr, nx_fg(0.6));
        cairo_fill(cr);
    }
}

/* Batterie (portable) : pourcentage, -1 si pas de batterie */
static int battery(bool *charging)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    struct dirent *e;
    int pct = -1;
    while ((e = readdir(d)) && pct < 0) {
        if (strncmp(e->d_name, "BAT", 3)) continue;
        char p[300], buf[32] = "";
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(buf, sizeof buf, f)) pct = atoi(buf); fclose(f); }
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
        if ((f = fopen(p, "r"))) { if (fgets(buf, sizeof buf, f)) *charging = !strncmp(buf, "Charging", 8); fclose(f); }
    }
    closedir(d);
    return pct;
}

static void draw_battery(cairo_t *cr, double x, double y, double s, int pct, bool charging, nx_color c)
{
    double w = s * 1.15, h = s * 0.56, by = y + (s - h) / 2;
    nx_rounded_rect(cr, x, by, w, h, h * 0.22);
    nx_set(cr, c);
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);
    cairo_rectangle(cr, x + w + 1, by + h * 0.3, 2.5, h * 0.4);
    cairo_fill(cr);
    double fw = (w - 5) * (pct < 0 ? 0 : pct > 100 ? 1 : pct / 100.0);
    nx_rounded_rect(cr, x + 2.5, by + 2.5, fw, h - 5, 1.5);
    nx_set(cr, pct <= 15 && !charging ? nx_rgb("#ff6b6b") : charging ? nx_rgb("#4cd07d") : c);
    cairo_fill(cr);
}

/* Position du bouton N, pour la flèche du menu Démarrer ($XDG_RUNTIME_DIR/nexus-dock) */
static void publish_start_x(double x)
{
    static int last = -1;
    if ((int)x == last) return;
    last = (int)x;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char p[256];
    snprintf(p, sizeof p, "%s/nexus-dock", rt ? rt : "/tmp");
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%d\n", last); fclose(f); }
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nbtns = 0;
    nx_color white = nx_fg(1);
    double bar_y = LIFT;                  /* haut de la barre (sous le dock qui dépasse) */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* ---- Barre : verre sur toute la largeur, coins supérieurs arrondis ---- */
    nx_rounded_rect(cr, 0, bar_y, width, height - bar_y + 40, 26);
    nx_glass_source(cr, bar_y, height);
    cairo_fill_preserve(cr);
    nx_set(cr, nx_fg(0.10));
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    /* ---- Contenu du dock : N, loupe, épinglées, fenêtres ---- */
    int ntask_btn = 0;
    for (struct task *t = tasks; t; t = t->next) if (t->ready && !pin_of(t)) ntask_btn++;
    double cell = CELL, pad = DOCK_H * 0.18;
    int n = 2 + npins + ntask_btn;
    double sep = ntask_btn ? 14 : 0;
    double dw = pad * 2 + n * cell + 14 + sep;           /* 14 : séparateur après la loupe */
    double right_w = 300 * tk();
    double dx = (width - dw) / 2;
    if (dx + dw > width - right_w) dx = width - right_w - dw;
    if (dx < 8) dx = 8;
    double dy = 4, dh = DOCK_H;
    /* ombre + verre plus marqué + liseré clair */
    for (int i = 6; i > 0; i--) {
        nx_rounded_rect(cr, dx - i, dy - i + 4, dw + 2 * i, dh + 2 * i, dh / 2 + i);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.045);
        cairo_fill(cr);
    }
    nx_rounded_rect(cr, dx, dy, dw, dh, dh / 2.6);
    nx_glass_source(cr, dy, dy + dh);
    cairo_fill_preserve(cr);
    cairo_fill_preserve(cr);
    nx_set(cr, nx_fg(0.16));
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    double isz = ICON, iy = dy + (dh - isz) / 2 - 2, by = dy + 4, bh = dh - 8;
    double x = dx + pad;
    /* N */
    button_bg(cr, x, cell, by, bh, nbtns);
    nx_logo(cr, x + (cell - isz) / 2, iy, isz);
    publish_start_x(x + cell / 2);
    add_btn(B_START, 0, x, cell);
    x += cell;
    /* loupe */
    button_bg(cr, x, cell, by, bh, nbtns);
    nx_icon(cr, "search", x + (cell - isz * 0.62) / 2, iy + isz * 0.19, isz * 0.62, white);
    add_btn(B_SEARCH, 0, x, cell);
    x += cell + 7;
    cairo_rectangle(cr, x - 4, dy + dh * 0.28, 1, dh * 0.44);
    nx_set(cr, nx_fg(0.14));
    cairo_fill(cr);
    x += 7;
    /* épinglées */
    for (int i = 0; i < npins; i++) {
        int count = 0;
        bool active = false;
        for (struct task *t = tasks; t; t = t->next)
            if (t->ready && pin_of(t) == &pins[i]) { count++; active |= t->activated && !t->minimized; }
        button_bg(cr, x, cell, by, bh, nbtns);
        nx_app_icon(cr, pins[i].app_id, x + (cell - isz) / 2, iy, isz, white);
        indicator(cr, x + cell / 2, dy + dh - 6, count, active);
        add_btn(B_PIN, i, x, cell);
        x += cell;
    }
    /* autres fenêtres */
    if (ntask_btn) {
        cairo_rectangle(cr, x + 6, dy + dh * 0.28, 1, dh * 0.44);
        nx_set(cr, nx_fg(0.14));
        cairo_fill(cr);
        x += sep;
    }
    int ti = 0;
    for (struct task *t = tasks; t; t = t->next, ti++) {
        if (!t->ready || pin_of(t)) continue;
        button_bg(cr, x, cell, by, bh, nbtns);
        nx_app_icon(cr, task_icon(t), x + (cell - isz) / 2, iy, isz, white);
        indicator(cr, x + cell / 2, dy + dh - 6, 1, t->activated && !t->minimized);
        add_btn(B_TASK, ti, x, cell);
        x += cell;
    }

    /* ---- Zone de droite : ^, réseau, son, batterie | heure ---- */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8], date[16];
    snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(date, sizeof date, "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    double k = tk(), mid = bar_y + (height - bar_y) / 2.0, ty = bar_y + 4, th = height - bar_y - 8;
    double cw = 100 * k, cx = width - cw - 14;
    button_bg(cr, cx, cw, ty, th, nbtns);
    nx_text(cr, hm, 17 * k, true, white, cx, mid - 21 * k, cw, 22 * k, NX_CENTER);
    nx_text(cr, date, 12.5 * k, false, nx_alpha(white, 0.8), cx, mid + 1, cw, 19 * k, NX_CENTER);
    add_btn(B_CLOCK, 0, cx, cw);
    cairo_rectangle(cr, cx - 8, mid - th * 0.32, 1, th * 0.64);
    nx_set(cr, nx_fg(0.18));
    cairo_fill(cr);
    double sw = 38 * k, si = 22 * k, rx = cx - 16 - sw;
    /* batterie */
    bool charging = false;
    int pct = battery(&charging);
    if (pct >= 0) {
        button_bg(cr, rx, sw, ty, th, nbtns);
        draw_battery(cr, rx + (sw - si * 1.2) / 2, mid - si / 2, si, pct, charging, white);
        add_btn(B_BAT, 0, rx, sw);
        rx -= sw;
    }
    /* son */
    const char *vol = nx_volume_icon();
    if (vol) {
        button_bg(cr, rx, sw, ty, th, nbtns);
        nx_icon(cr, vol, rx + (sw - si) / 2, mid - si / 2, si, white);
        add_btn(B_VOL, 0, rx, sw);
        rx -= sw;
    }
    /* réseau */
    bool up;
    const char *net = nx_network_icon(&up);
    button_bg(cr, rx, sw, ty, th, nbtns);
    nx_icon(cr, net, rx + (sw - si) / 2, mid - si / 2, si, nx_alpha(white, up ? 0.95 : 0.35));
    if (!up) {
        cairo_set_source_rgb(cr, 1, 0.4, 0.4);
        cairo_set_line_width(cr, 2);
        double my = mid + 4, mx = rx + (sw + si) / 2 - 7 * k;
        cairo_move_to(cr, mx, my); cairo_line_to(cr, mx + 7 * k, my + 7 * k);
        cairo_move_to(cr, mx + 7 * k, my); cairo_line_to(cr, mx, my + 7 * k);
        cairo_stroke(cr);
    }
    add_btn(B_NET, 0, rx, sw);
    rx -= sw;
    /* ^ : raccourcis (Paramètres) */
    button_bg(cr, rx, sw, ty, th, nbtns);
    cairo_move_to(cr, rx + sw / 2 - 6 * k, mid + 3 * k);
    cairo_line_to(cr, rx + sw / 2, mid - 3 * k);
    cairo_line_to(cr, rx + sw / 2 + 6 * k, mid + 3 * k);
    nx_set(cr, white);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    add_btn(B_MORE, 0, rx, sw);

    /* Poignée de redimensionnement : liseré coloré sur le bord supérieur de la barre */
    if (grip_hover || resizing) {
        nx_rounded_rect(cr, 20, bar_y, width - 40, 3, 1.5);
        nx_set(cr, nx_accent_color());
        cairo_fill(cr);
    }
}

static void set_height(int h)
{
    if (h < 44) h = 44;
    if (h > 110) h = 110;
    if (h == cur_h) return;
    cur_h = h;
    zwlr_layer_surface_v1_set_size(panel->layer, 0, h + LIFT);
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

/* Clic droit sur une application : petit menu au-dessus de la barre */
static void context_menu(const struct btn *b)
{
    char cmd[4096], q[700], q2[700], arg[1600];
    const char *app = NULL, *title = "";
    bool pinned = false, open = false;
    if (b->kind == B_PIN) {
        const struct nx_pin *p = &pins[b->index];
        app = p->app_id;
        pinned = true;
        for (struct task *t = tasks; t; t = t->next)
            if (t->ready && pin_of(t) == p) { open = true; title = t->title; }
    } else {
        struct task *t = task_at(b->index);
        if (!t) return;
        app = t->app_id;
        title = t->title;
        open = true;
    }
    snprintf(cmd, sizeof cmd, "nexus-popup %d", (int)(b->x + b->w / 2));
#define ITEM(label, c) do { nx_shell_quote(q, sizeof q, label); nx_shell_quote(q2, sizeof q2, c); \
        size_t l = strlen(cmd); snprintf(cmd + l, sizeof cmd - l, " %s %s", q, q2); } while (0)
    if (pinned) {
        ITEM(open ? "Nouvelle fenêtre" : "Ouvrir", pins[b->index].cmd);
        nx_shell_quote(q, sizeof q, app);
        snprintf(arg, sizeof arg, "nexus-pin remove %s", q);
        ITEM("Détacher de la barre des tâches", arg);
    } else {
        struct nx_pin info;
        if (nx_pin_info(app, &info)) {
            nx_shell_quote(q, sizeof q, app);
            snprintf(arg, sizeof arg, "nexus-pin add %s", q);
            ITEM("Épingler à la barre des tâches", arg);
        }
    }
    if (open) {
        char qa[300], qt[600];
        nx_shell_quote(qa, sizeof qa, app);
        nx_shell_quote(qt, sizeof qt, title);
        snprintf(arg, sizeof arg, "nexus-pin close %s %s", qa, qt);
        ITEM("Fermer la fenêtre", arg);
    }
#undef ITEM
    nx_spawn(cmd);
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    int h = hit(x);
    /* Redimensionnement en tirant le bord supérieur */
    if (resizing) {
        if (ev == NX_MOTION) {
            /* y est relatif à la barre, dont le haut bouge : y - hauteur ne dépend que du curseur */
            set_height(grip_h0 - (int)((y - (cur_h + LIFT)) - grip_y0));
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
    bool g = ev != NX_LEAVE && y < GRIP && h < 0;
    if (g != grip_hover) { grip_hover = g; nx_redraw(w); }
    if (g && ev == NX_PRESS && value == BTN_LEFT) {
        resizing = true;
        grip_h0 = cur_h;
        grip_y0 = y - (cur_h + LIFT);
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
                case B_PIN: click_pin(&pins[b->index]); break;
                case B_TASK: { struct task *t = task_at(b->index); if (t) toggle_task(t); break; }
                case B_NET: nx_spawn("nexus-network"); break;
                case B_VOL: nx_spawn("nexus-volume"); break;
                case B_CLOCK: nx_spawn("nexus-settings --about"); break;
                case B_MORE: {
                    char cmd[64];
                    snprintf(cmd, sizeof cmd, "nexus-tray %d", (int)(b->x + b->w / 2));
                    nx_spawn(cmd);
                    break;
                }
                case B_BAT: nx_spawn("nexus-monitor"); break;
                }
            } else if (value == BTN_RIGHT && (b->kind == B_PIN || b->kind == B_TASK)) {
                context_menu(b);
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
    npins = nx_pins_load(pins, NX_MAX_PINS);
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
    nx.toplevel_listener = &manager_listener;   /* fenêtres déjà ouvertes comprises */
    if (!nx_init()) return 1;
    cur_h = nx_panel_height();
    struct nx_handlers h = { .draw = draw, .pointer = pointer };
    panel = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                            0, PANEL_H + LIFT, PANEL_H,
                            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE, "nexus-panel");
    if (!panel) return 1;
    nx.timeout = until_next_minute;
    nx.tick = tick;
    nx.reload = reload;
    nx_run();
    return 0;
}
