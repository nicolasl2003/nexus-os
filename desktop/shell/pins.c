/*
 * Applications épinglées à la barre des tâches, et petits programmes du clic
 * droit sur la barre :
 *
 *   nexus-pin add <app_id>              épingler (Nexus, ou logiciel avec un .desktop)
 *   nexus-pin remove <app_id>           détacher
 *   nexus-pin close <app_id> [titre]    fermer la fenêtre (ou la 1re de l'application)
 *   nexus-popup <x> <libellé> <commande> [<libellé> <commande>...]
 *                                       petit menu au-dessus de la barre, en x
 */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include "pins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>

/* ---------- Liste des épingles ---------- */

static const struct nx_pin default_pins[] = {
    { "nexus-files", "Fichiers", "nexus-files" },
    { "nexus-browser", "Navigateur", "nexus-browser" },
    { "nexus-settings", "Paramètres", "nexus-settings" },
    { "nexus-store", "Magasin d'applications", "nexus-store" },
};

static void pins_path(char *p, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(p, n, "%s/.config/nexus/epingles", home ? home : "/tmp");
}

/* Le programme de la commande existe-t-il (dans le PATH) ? */
static bool cmd_available(const char *cmd)
{
    char prog[256];
    size_t k = strcspn(cmd, " \t");
    if (k == 0 || k >= sizeof prog) return false;
    memcpy(prog, cmd, k);
    prog[k] = 0;
    if (!strcmp(prog, "nexus-browser")) return access("/usr/local/bin/netsurf-fb", X_OK) == 0;
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char dirs[2048], f[2400];
    snprintf(dirs, sizeof dirs, "%s", path);
    for (char *d = strtok(dirs, ":"); d; d = strtok(NULL, ":")) {
        snprintf(f, sizeof f, "%s/%s", d, prog);
        if (access(f, X_OK) == 0) return true;
    }
    return false;
}

/* Toutes les épingles du fichier (installées ou non) */
static int pins_read_all(struct nx_pin *out, int max)
{
    char p[512], line[1024];
    pins_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) {
        int n = 0;
        for (unsigned i = 0; i < sizeof default_pins / sizeof default_pins[0] && n < max; i++)
            out[n++] = default_pins[i];
        return n;
    }
    int n = 0;
    while (n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *a = line, *b = strchr(a, '|');
        if (!b) continue;
        *b++ = 0;
        char *c = strchr(b, '|');
        if (!c) continue;
        *c++ = 0;
        if (!a[0] || !c[0]) continue;
        snprintf(out[n].app_id, sizeof out[n].app_id, "%s", a);
        snprintf(out[n].label, sizeof out[n].label, "%s", b);
        snprintf(out[n].cmd, sizeof out[n].cmd, "%s", c);
        n++;
    }
    fclose(f);
    return n;
}

static int pins_save(const struct nx_pin *pins, int n)
{
    char p[512], tmp[520], dir[512];
    pins_path(p, sizeof p);
    snprintf(dir, sizeof dir, "%s", p);
    *strrchr(dir, '/') = 0;
    char parent[512];
    snprintf(parent, sizeof parent, "%s", dir);
    *strrchr(parent, '/') = 0;
    mkdir(parent, 0755);
    mkdir(dir, 0755);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (int i = 0; i < n; i++) fprintf(f, "%s|%s|%s\n", pins[i].app_id, pins[i].label, pins[i].cmd);
    fclose(f);
    return rename(tmp, p);
}

int nx_pins_load(struct nx_pin *out, int max)
{
    struct nx_pin all[NX_MAX_PINS];
    int n = pins_read_all(all, NX_MAX_PINS), k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (cmd_available(all[i].cmd)) out[k++] = all[i];
    return k;
}

bool nx_pin_info(const char *app_id, struct nx_pin *out)
{
    if (!app_id || !app_id[0] || strchr(app_id, '|')) return false;
    memset(out, 0, sizeof *out);
    snprintf(out->app_id, sizeof out->app_id, "%s", app_id);
    /* Applications de Nexus : la commande est leur nom */
    for (unsigned i = 0; i < sizeof default_pins / sizeof default_pins[0]; i++)
        if (!strcmp(app_id, default_pins[i].app_id)) { *out = default_pins[i]; return true; }
    if (!strcmp(app_id, "foot") || !strcmp(app_id, "footclient")) {
        snprintf(out->app_id, sizeof out->app_id, "foot");
        snprintf(out->label, sizeof out->label, "Terminal");
        snprintf(out->cmd, sizeof out->cmd, "foot");
        return true;
    }
    if (!strncmp(app_id, "nexus-", 6)) {
        snprintf(out->label, sizeof out->label, "%s", app_id + 6);
        snprintf(out->cmd, sizeof out->cmd, "%s", app_id);
        return cmd_available(out->cmd);
    }
    /* Logiciel externe : son fichier .desktop, sinon un programme du même nom */
    char name[128], exec[512];
    if (nx_desktop_entry(app_id, name, sizeof name, exec, sizeof exec, NULL, 0) && !strchr(exec, '|')) {
        snprintf(out->label, sizeof out->label, "%s", name);
        snprintf(out->cmd, sizeof out->cmd, "%s", exec);
        return true;
    }
    if (cmd_available(app_id) && !strpbrk(app_id, " ;&$`'\"\\")) {
        snprintf(out->label, sizeof out->label, "%s", app_id);
        snprintf(out->cmd, sizeof out->cmd, "%s", app_id);
        return true;
    }
    return false;
}

/* ---------- nexus-pin close : fermer une fenêtre par son app_id ---------- */

struct ctask { struct zwlr_foreign_toplevel_handle_v1 *h; char app_id[128], title[256]; struct ctask *next; };
static struct ctask *ctasks;

static void c_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; struct ctask *t = d; snprintf(t->title, sizeof t->title, "%s", s); }
static void c_app(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s)
{ (void)h; struct ctask *t = d; snprintf(t->app_id, sizeof t->app_id, "%s", s); }
static void c_out(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) { (void)d; (void)h; (void)o; }
static void c_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *a) { (void)d; (void)h; (void)a; }
static void c_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)d; (void)h; }
static void c_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)d; (void)h; }
static void c_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p)
{ (void)d; (void)h; (void)p; }
static const struct zwlr_foreign_toplevel_handle_v1_listener c_listener = {
    .title = c_title, .app_id = c_app, .output_enter = c_out, .output_leave = c_out,
    .state = c_state, .done = c_done, .closed = c_closed, .parent = c_parent,
};
static void cm_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)d; (void)m;
    struct ctask *t = calloc(1, sizeof *t);
    t->h = h;
    t->next = ctasks;
    ctasks = t;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &c_listener, t);
}
static void cm_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d; (void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener cm_listener = {
    .toplevel = cm_toplevel, .finished = cm_finished,
};

static int close_window(const char *app_id, const char *title)
{
    nx.toplevel_listener = &cm_listener;
    if (!nx_init() || !nx.toplevels) return 1;
    wl_display_roundtrip(nx.display);
    wl_display_roundtrip(nx.display);
    struct ctask *found = NULL;
    for (struct ctask *t = ctasks; t; t = t->next) {
        bool same_app = !strcmp(t->app_id, app_id) ||
                        (!strcmp(app_id, "foot") && !strcmp(t->app_id, "footclient"));
        if (!same_app) continue;
        if (title && *title && !strcmp(t->title, title)) { found = t; break; }
        if (!found) found = t;
    }
    if (found) zwlr_foreign_toplevel_handle_v1_close(found->h);
    wl_display_roundtrip(nx.display);
    return found ? 0 : 1;
}

int pin_main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage : nexus-pin add|remove|close <app_id> [titre]\n");
        return 2;
    }
    const char *act = argv[1], *id = argv[2];
    if (!strcmp(act, "close")) return close_window(id, argc > 3 ? argv[3] : NULL);

    struct nx_pin pins[NX_MAX_PINS];
    int n = pins_read_all(pins, NX_MAX_PINS);
    if (!strcmp(act, "add")) {
        struct nx_pin p;
        if (!nx_pin_info(id, &p)) {
            fprintf(stderr, "nexus-pin : « %s » ne peut pas être épinglé (commande inconnue).\n", id);
            return 1;
        }
        for (int i = 0; i < n; i++) if (!strcmp(pins[i].app_id, p.app_id)) return 0;
        if (n >= NX_MAX_PINS) { fprintf(stderr, "nexus-pin : trop d'applications épinglées.\n"); return 1; }
        pins[n++] = p;
    } else if (!strcmp(act, "remove")) {
        int k = 0;
        for (int i = 0; i < n; i++)
            if (strcmp(pins[i].app_id, id)) pins[k++] = pins[i];
        n = k;
    } else {
        fprintf(stderr, "Usage : nexus-pin add|remove|close <app_id> [titre]\n");
        return 2;
    }
    if (pins_save(pins, n) != 0) { perror("nexus-pin"); return 1; }
    nx_notify_reload();   /* la barre des tâches relit la liste */
    return 0;
}

/* ---------- nexus-popup : petit menu au-dessus de la barre ---------- */

#define POP_W 280
#define ITEM_H 40

static struct nx_window *pop;
static double pop_x;
static const char **pop_labels, **pop_cmds;
static int pop_n, pop_hover = -1;

static void pop_box(int width, int height, double *x, double *y, double *h)
{
    *h = pop_n * ITEM_H + 12;
    *x = pop_x - POP_W / 2.0;
    if (*x + POP_W > width - 8) *x = width - 8 - POP_W;
    if (*x < 8) *x = 8;
    *y = height - *h - 10;
}

static int pop_hit(double x, double y)
{
    double bx, by, bh;
    pop_box(pop->width, pop->height, &bx, &by, &bh);
    if (x < bx || x >= bx + POP_W || y < by || y >= by + bh) return -2;   /* dehors */
    int i = (int)((y - by - 6) / ITEM_H);
    return i >= 0 && i < pop_n ? i : -1;
}

static void pop_draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    double bx, by, bh;
    pop_box(width, height, &bx, &by, &bh);
    nx_glass(cr, bx, by, POP_W, bh, 14);
    nx_color text = nx_rgb(NX_GLASS_TEXT), accent = nx_accent_color();
    for (int i = 0; i < pop_n; i++) {
        double iy = by + 6 + i * ITEM_H;
        if (i == pop_hover) {
            nx_rounded_rect(cr, bx + 6, iy, POP_W - 12, ITEM_H, 9);
            nx_set(cr, accent);
            cairo_fill(cr);
        }
        nx_text(cr, pop_labels[i], 14, false, i == pop_hover ? nx_rgb("#ffffff") : text,
                bx + 20, iy, POP_W - 36, ITEM_H, NX_LEFT);
    }
}

static void pop_pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    int h = pop_hit(x, y);
    if (ev == NX_MOTION || ev == NX_LEAVE) {
        int nh = ev == NX_LEAVE ? -1 : h;
        if (nh != pop_hover) { pop_hover = nh; nx_redraw(w); }
    } else if (ev == NX_PRESS) {
        if (h == -2) nx_quit();
    } else if (ev == NX_RELEASE && (value == BTN_LEFT || value == BTN_RIGHT) && h >= 0) {
        nx_spawn(pop_cmds[h]);
        nx_quit();
    }
}

static void pop_key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    (void)utf8;
    if (sym == XKB_KEY_Escape) nx_quit();
    else if (sym == XKB_KEY_Down) { pop_hover = (pop_hover + 1) % pop_n; nx_redraw(w); }
    else if (sym == XKB_KEY_Up) { pop_hover = pop_hover <= 0 ? pop_n - 1 : pop_hover - 1; nx_redraw(w); }
    else if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && pop_hover >= 0) {
        nx_spawn(pop_cmds[pop_hover]);
        nx_quit();
    }
}

int popup_main(int argc, char **argv)
{
    if (argc < 4 || (argc - 2) % 2) {
        fprintf(stderr, "Usage : nexus-popup <x> <libellé> <commande> [<libellé> <commande>...]\n");
        return 2;
    }
    if (nx_toggle_instance("nexus-popup")) return 0;
    pop_x = atof(argv[1]);
    pop_n = (argc - 2) / 2;
    pop_labels = calloc(pop_n, sizeof *pop_labels);
    pop_cmds = calloc(pop_n, sizeof *pop_cmds);
    for (int i = 0; i < pop_n; i++) { pop_labels[i] = argv[2 + 2 * i]; pop_cmds[i] = argv[3 + 2 * i]; }
    if (!nx_init()) { nx_instance_done(); return 1; }
    struct nx_handlers h = { .draw = pop_draw, .pointer = pop_pointer, .key = pop_key };
    pop = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          0, 0, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, "nexus-popup");
    if (pop) nx_run();
    nx_instance_done();
    return 0;
}
