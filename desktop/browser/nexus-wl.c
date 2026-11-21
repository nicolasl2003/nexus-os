/* nexus-wl.c — sortie d'affichage Wayland de libnsfb pour Nexus OS.
 *
 * Remplace l'ancienne sortie « wld » (basée sur wl_shell, retiré des
 * compositeurs modernes) par une fenêtre xdg-shell :
 *   - barre de titre dessinée par labwc (xdg-decoration) ;
 *   - image dans des tampons wl_shm (double tampon, recopie des seules
 *     zones modifiées) ;
 *   - souris (clics, molette) et clavier via xkbcommon : la disposition
 *     du compositeur (AZERTY) s'applique, les caractères accentués sont
 *     transmis tels quels (NSFB_KEY_UCS4) ;
 *   - redimensionnement de la fenêtre (NSFB_EVENT_RESIZE) ;
 *   - répétition des touches maintenues.
 * NetSurf dessine son propre pointeur (main, curseur texte...) : on masque
 * celui du compositeur au-dessus de la page.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"
#include "libnsfb_event.h"
#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "cursor.h"

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#define QUEUE_LEN 256
#define SCROLL_STEP 10.0

struct shmbuf {
    struct wl_buffer *wb;
    void *data;
    size_t size;
    int w, h;
    bool busy;
    bool stale_any;
    nsfb_bbox_t stale;   /* zone pas encore recopiée dans ce tampon */
};

static struct {
    nsfb_t *nsfb;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_mgr;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;

    struct wl_surface *surface;
    struct xdg_surface *xsurface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *deco;

    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    bool ctrl;

    struct shmbuf bufs[2];
    bool pending_any;
    nsfb_bbox_t pending;

    bool configured;
    int cfg_w, cfg_h;          /* taille demandée par le compositeur */
    int sent_w, sent_h;        /* dernière taille signalée à NetSurf */

    nsfb_event_t queue[QUEUE_LEN];
    int qhead, qtail;

    double scroll_acc;

    int32_t rep_rate, rep_delay;
    uint32_t rep_key;          /* touche répétée (0 = aucune) */
    int rep_code;
    uint64_t rep_next;
} W;

/* ---------- Utilitaires ---------- */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void push(nsfb_event_t ev)
{
    /* déplacements consécutifs : seul le dernier compte */
    if (ev.type == NSFB_EVENT_MOVE_ABSOLUTE && W.qhead != W.qtail) {
        int last = (W.qtail + QUEUE_LEN - 1) % QUEUE_LEN;
        if (W.queue[last].type == NSFB_EVENT_MOVE_ABSOLUTE) {
            W.queue[last] = ev;
            return;
        }
    }
    int next = (W.qtail + 1) % QUEUE_LEN;
    if (next == W.qhead) return;   /* file pleine : on oublie l'évènement */
    W.queue[W.qtail] = ev;
    W.qtail = next;
}

static bool pop(nsfb_event_t *ev)
{
    if (W.qhead == W.qtail) return false;
    *ev = W.queue[W.qhead];
    W.qhead = (W.qhead + 1) % QUEUE_LEN;
    return true;
}

static void push_key(enum nsfb_event_type_e type, int code)
{
    nsfb_event_t ev = { .type = type };
    ev.value.keycode = code;
    push(ev);
}

static void bbox_add(bool *any, nsfb_bbox_t *acc, const nsfb_bbox_t *box)
{
    if (!*any) { *acc = *box; *any = true; return; }
    if (box->x0 < acc->x0) acc->x0 = box->x0;
    if (box->y0 < acc->y0) acc->y0 = box->y0;
    if (box->x1 > acc->x1) acc->x1 = box->x1;
    if (box->y1 > acc->y1) acc->y1 = box->y1;
}

static void damage(const nsfb_bbox_t *box)
{
    nsfb_bbox_t b = *box;
    if (b.x0 < 0) b.x0 = 0;
    if (b.y0 < 0) b.y0 = 0;
    if (b.x1 > W.nsfb->width) b.x1 = W.nsfb->width;
    if (b.y1 > W.nsfb->height) b.y1 = W.nsfb->height;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    bbox_add(&W.pending_any, &W.pending, &b);
}

static void damage_all(void)
{
    nsfb_bbox_t all = { 0, 0, W.nsfb->width, W.nsfb->height };
    damage(&all);
}

/* ---------- Tampons partagés ---------- */

static void buffer_release(void *data, struct wl_buffer *wb)
{
    (void)wb;
    ((struct shmbuf *)data)->busy = false;
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void buffer_destroy(struct shmbuf *b)
{
    if (b->wb) wl_buffer_destroy(b->wb);
    if (b->data) munmap(b->data, b->size);
    memset(b, 0, sizeof *b);
}

static bool buffer_create(struct shmbuf *b, int w, int h)
{
    size_t stride = (size_t)w * 4, size = stride * h;
    int fd = memfd_create("nexus-wl", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, size) < 0) { close(fd); return false; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return false; }
    struct wl_shm_pool *pool = wl_shm_create_pool(W.shm, fd, size);
    b->wb = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->wb, &buffer_listener, b);
    b->data = data;
    b->size = size;
    b->w = w;
    b->h = h;
    b->busy = false;
    b->stale_any = true;
    b->stale = (nsfb_bbox_t){ 0, 0, w, h };
    return true;
}

/* Envoie au compositeur les zones modifiées depuis la dernière image */
static void flush(void)
{
    nsfb_t *nsfb = W.nsfb;
    if (!W.pending_any || !W.configured || !nsfb->ptr) return;
    int w = nsfb->width, h = nsfb->height;

    struct shmbuf *b = NULL;
    for (int i = 0; i < 2; i++) {
        struct shmbuf *c = &W.bufs[i];
        if (c->wb && (c->w != w || c->h != h)) buffer_destroy(c);   /* taille changée */
        if (!c->wb && !buffer_create(c, w, h)) continue;
        if (!c->busy && !b) b = c;
    }
    if (!b) return;   /* les deux tampons sont encore affichés : on réessaiera */

    nsfb_bbox_t copy = W.pending;
    if (b->stale_any) {
        bool any = true;
        bbox_add(&any, &copy, &b->stale);
    }
    if (copy.x1 > w) copy.x1 = w;
    if (copy.y1 > h) copy.y1 = h;
    size_t stride = (size_t)w * 4;
    for (int y = copy.y0; y < copy.y1; y++)
        memcpy((uint8_t *)b->data + y * stride + copy.x0 * 4,
               nsfb->ptr + y * nsfb->linelen + copy.x0 * 4,
               (size_t)(copy.x1 - copy.x0) * 4);
    b->stale_any = false;
    struct shmbuf *other = (b == &W.bufs[0]) ? &W.bufs[1] : &W.bufs[0];
    if (other->wb) bbox_add(&other->stale_any, &other->stale, &W.pending);

    wl_surface_attach(W.surface, b->wb, 0, 0);
    wl_surface_damage_buffer(W.surface, W.pending.x0, W.pending.y0,
                             W.pending.x1 - W.pending.x0, W.pending.y1 - W.pending.y0);
    wl_surface_commit(W.surface);
    b->busy = true;
    W.pending_any = false;
}

/* ---------- Fenêtre ---------- */

static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xsurface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xs, serial);
    if (!W.configured) {
        W.configured = true;
        damage_all();
    }
    if (W.cfg_w > 0 && W.cfg_h > 0 && (W.cfg_w != W.sent_w || W.cfg_h != W.sent_h)) {
        nsfb_event_t ev = { .type = NSFB_EVENT_RESIZE };
        ev.value.resize.w = W.cfg_w;
        ev.value.resize.h = W.cfg_h;
        W.sent_w = W.cfg_w;
        W.sent_h = W.cfg_h;
        push(ev);
    }
}

static const struct xdg_surface_listener xsurface_listener = { .configure = xsurface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                               struct wl_array *states)
{
    (void)data; (void)t; (void)states;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    W.cfg_w = w;
    W.cfg_h = h;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)data; (void)t;
    nsfb_event_t ev = { .type = NSFB_EVENT_CONTROL };
    ev.value.controlcode = NSFB_CONTROL_QUIT;
    push(ev);
}

static void toplevel_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d; (void)t; (void)w; (void)h; }

static void toplevel_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{ (void)d; (void)t; (void)c; }

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_caps,
};

/* ---------- Souris ---------- */

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                      wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)s;
    wl_pointer_set_cursor(p, serial, NULL, 0, 0);   /* NetSurf dessine le sien */
    nsfb_event_t ev = { .type = NSFB_EVENT_MOVE_ABSOLUTE };
    ev.value.vector.x = wl_fixed_to_int(x);
    ev.value.vector.y = wl_fixed_to_int(y);
    push(ev);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{ (void)d; (void)p; (void)serial; (void)s; }

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)time;
    nsfb_event_t ev = { .type = NSFB_EVENT_MOVE_ABSOLUTE };
    ev.value.vector.x = wl_fixed_to_int(x);
    ev.value.vector.y = wl_fixed_to_int(y);
    push(ev);
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
                       uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)serial; (void)time;
    int code;
    switch (button) {
    case BTN_LEFT: code = NSFB_KEY_MOUSE_1; break;
    case BTN_MIDDLE: code = NSFB_KEY_MOUSE_2; break;
    case BTN_RIGHT: code = NSFB_KEY_MOUSE_3; break;
    default: return;
    }
    push_key(state == WL_POINTER_BUTTON_STATE_PRESSED ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, code);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)d; (void)p; (void)time;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    W.scroll_acc += wl_fixed_to_double(value);
    while (W.scroll_acc >= SCROLL_STEP || W.scroll_acc <= -SCROLL_STEP) {
        int code = W.scroll_acc > 0 ? NSFB_KEY_MOUSE_5 : NSFB_KEY_MOUSE_4;
        W.scroll_acc += W.scroll_acc > 0 ? -SCROLL_STEP : SCROLL_STEP;
        push_key(NSFB_EVENT_KEY_DOWN, code);
        push_key(NSFB_EVENT_KEY_UP, code);
    }
}

static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; W.scroll_acc = 0; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

/* ---------- Clavier ---------- */

static int keysym_to_nsfb(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Shift_L: return NSFB_KEY_LSHIFT;
    case XKB_KEY_Shift_R: return NSFB_KEY_RSHIFT;
    case XKB_KEY_Control_L: return NSFB_KEY_LCTRL;
    case XKB_KEY_Control_R: return NSFB_KEY_RCTRL;
    case XKB_KEY_Alt_L: return NSFB_KEY_LALT;
    case XKB_KEY_Alt_R: case XKB_KEY_ISO_Level3_Shift: return NSFB_KEY_RALT;
    case XKB_KEY_Super_L: return NSFB_KEY_LSUPER;
    case XKB_KEY_Super_R: return NSFB_KEY_RSUPER;
    case XKB_KEY_Caps_Lock: return NSFB_KEY_CAPSLOCK;
    case XKB_KEY_Num_Lock: return NSFB_KEY_NUMLOCK;
    case XKB_KEY_BackSpace: return NSFB_KEY_BACKSPACE;
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return NSFB_KEY_TAB;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return NSFB_KEY_RETURN;
    case XKB_KEY_Escape: return NSFB_KEY_ESCAPE;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: return NSFB_KEY_DELETE;
    case XKB_KEY_Up: case XKB_KEY_KP_Up: return NSFB_KEY_UP;
    case XKB_KEY_Down: case XKB_KEY_KP_Down: return NSFB_KEY_DOWN;
    case XKB_KEY_Left: case XKB_KEY_KP_Left: return NSFB_KEY_LEFT;
    case XKB_KEY_Right: case XKB_KEY_KP_Right: return NSFB_KEY_RIGHT;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: return NSFB_KEY_HOME;
    case XKB_KEY_End: case XKB_KEY_KP_End: return NSFB_KEY_END;
    case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: return NSFB_KEY_PAGEUP;
    case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: return NSFB_KEY_PAGEDOWN;
    case XKB_KEY_Insert: case XKB_KEY_KP_Insert: return NSFB_KEY_INSERT;
    case XKB_KEY_Menu: return NSFB_KEY_MENU;
    }
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F15) return NSFB_KEY_F1 + (int)(sym - XKB_KEY_F1);

    uint32_t uc = xkb_keysym_to_utf32(sym);
    if (W.ctrl) {
        /* raccourcis (Ctrl+C, Ctrl+V, Ctrl+Plus...) : codes ASCII classiques */
        if (uc >= 'A' && uc <= 'Z') uc += 'a' - 'A';
        if (uc == '+') uc = '=';
        if (uc > 32 && uc < 127) return (int)uc;
    }
    if (uc >= 32 && uc != 127) return (int)(NSFB_KEY_UCS4_FLAG | uc);
    return NSFB_KEY_UNKNOWN;
}

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
    (void)d; (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    struct xkb_keymap *km = xkb_keymap_new_from_string(W.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;
    if (W.state) xkb_state_unref(W.state);
    if (W.keymap) xkb_keymap_unref(W.keymap);
    W.keymap = km;
    W.state = xkb_state_new(km);
}

static void release_modifiers(void)
{
    static const int mods[] = { NSFB_KEY_LSHIFT, NSFB_KEY_RSHIFT, NSFB_KEY_LCTRL, NSFB_KEY_RCTRL };
    for (unsigned i = 0; i < 4; i++) push_key(NSFB_EVENT_KEY_UP, mods[i]);
    W.rep_key = 0;
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                     struct wl_array *keys)
{ (void)d; (void)k; (void)serial; (void)s; (void)keys; }

static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void)d; (void)k; (void)serial; (void)s;
    release_modifiers();
}

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key,
                   uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)time;
    if (!W.state) return;
    xkb_keycode_t kc = key + 8;
    int code = keysym_to_nsfb(xkb_state_key_get_one_sym(W.state, kc));
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (code == NSFB_KEY_UNKNOWN) return;
        push_key(NSFB_EVENT_KEY_DOWN, code);
        bool modifier = code >= NSFB_KEY_NUMLOCK && code <= NSFB_KEY_COMPOSE;
        if (!modifier && W.rep_rate > 0 && xkb_keymap_key_repeats(W.keymap, kc)) {
            W.rep_key = key;
            W.rep_code = code;
            W.rep_next = now_ms() + W.rep_delay;
        }
    } else {
        if (key == W.rep_key) W.rep_key = 0;
        if (code != NSFB_KEY_UNKNOWN) push_key(NSFB_EVENT_KEY_UP, code);
    }
}

static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
                         uint32_t lat, uint32_t lock, uint32_t group)
{
    (void)d; (void)k; (void)serial;
    if (!W.state) return;
    xkb_state_update_mask(W.state, dep, lat, lock, 0, 0, group);
    W.ctrl = xkb_state_mod_name_is_active(W.state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    (void)d; (void)k;
    W.rep_rate = rate;
    W.rep_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat,
};

/* ---------- Siège et registre ---------- */

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !W.pointer) {
        W.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(W.pointer, &pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !W.keyboard) {
        W.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(W.keyboard, &keyboard_listener, NULL);
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

static uint32_t minv(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name))
        W.compositor = wl_registry_bind(r, name, &wl_compositor_interface, minv(ver, 4));
    else if (!strcmp(iface, wl_shm_interface.name))
        W.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        W.wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, minv(ver, 5));
        xdg_wm_base_add_listener(W.wm_base, &wm_listener, NULL);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
        W.deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name) && !W.seat) {
        W.seat = wl_registry_bind(r, name, &wl_seat_interface, minv(ver, 5));
        wl_seat_add_listener(W.seat, &seat_listener, NULL);
    }
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }

static const struct wl_registry_listener registry_listener = { .global = reg_global, .global_remove = reg_remove };

/* ---------- Routines de la surface libnsfb ---------- */

static int wl_defaults(nsfb_t *nsfb)
{
    nsfb->width = 1100;
    nsfb->height = 700;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int wl_initialise(nsfb_t *nsfb)
{
    memset(&W, 0, sizeof W);
    W.nsfb = nsfb;
    W.rep_rate = 25;
    W.rep_delay = 600;

    W.display = wl_display_connect(NULL);
    if (!W.display) {
        fprintf(stderr, "nexus-wl : impossible de se connecter au compositeur Wayland\n");
        return -1;
    }
    W.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    W.registry = wl_display_get_registry(W.display);
    wl_registry_add_listener(W.registry, &registry_listener, NULL);
    wl_display_roundtrip(W.display);
    if (!W.compositor || !W.shm || !W.wm_base || !W.xkb) {
        fprintf(stderr, "nexus-wl : le compositeur ne fournit pas xdg-shell/wl_shm\n");
        return -1;
    }

    W.surface = wl_compositor_create_surface(W.compositor);
    W.xsurface = xdg_wm_base_get_xdg_surface(W.wm_base, W.surface);
    xdg_surface_add_listener(W.xsurface, &xsurface_listener, NULL);
    W.toplevel = xdg_surface_get_toplevel(W.xsurface);
    xdg_toplevel_add_listener(W.toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(W.toplevel, "Navigateur");
    xdg_toplevel_set_app_id(W.toplevel, "nexus-browser");
    xdg_toplevel_set_min_size(W.toplevel, 480, 320);
    if (W.deco_mgr) {
        W.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(W.deco_mgr, W.toplevel);
        zxdg_toplevel_decoration_v1_set_mode(W.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    W.sent_w = nsfb->width;
    W.sent_h = nsfb->height;
    wl_surface_commit(W.surface);

    /* image de départ : fond blanc */
    size_t size = (size_t)nsfb->linelen * nsfb->height;
    if (nsfb->linelen == 0) {
        nsfb->linelen = nsfb->width * 4;
        size = (size_t)nsfb->linelen * nsfb->height;
    }
    uint8_t *p = realloc(nsfb->ptr, size);
    if (!p) return -1;
    nsfb->ptr = p;
    memset(nsfb->ptr, 0xff, size);

    while (!W.configured)
        if (wl_display_dispatch(W.display) < 0) return -1;
    damage_all();
    return 0;
}

static int wl_finalise(nsfb_t *nsfb)
{
    for (int i = 0; i < 2; i++) buffer_destroy(&W.bufs[i]);
    if (W.deco) zxdg_toplevel_decoration_v1_destroy(W.deco);
    if (W.toplevel) xdg_toplevel_destroy(W.toplevel);
    if (W.xsurface) xdg_surface_destroy(W.xsurface);
    if (W.surface) wl_surface_destroy(W.surface);
    if (W.display) wl_display_disconnect(W.display);
    if (W.state) xkb_state_unref(W.state);
    if (W.keymap) xkb_keymap_unref(W.keymap);
    if (W.xkb) xkb_context_unref(W.xkb);
    free(nsfb->ptr);
    nsfb->ptr = NULL;
    return 0;
}

static int wl_set_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    (void)format;   /* toujours XRGB8888, le format de wl_shm */
    int ow = nsfb->width, oh = nsfb->height;
    if (width > 0) nsfb->width = width;
    if (height > 0) nsfb->height = height;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    nsfb->linelen = nsfb->width * 4;
    if (nsfb->ptr) {
        uint8_t *p = realloc(nsfb->ptr, (size_t)nsfb->linelen * nsfb->height);
        if (!p) {
            nsfb->width = ow;
            nsfb->height = oh;
            nsfb->linelen = ow * 4;
            return -1;
        }
        nsfb->ptr = p;
        if (nsfb->cursor) nsfb->cursor->plotted = false;   /* sauvegarde du pointeur périmée */
        W.pending_any = false;
        damage_all();
    }
    return 0;
}

static int wl_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    if (cursor && cursor->plotted && nsfb_plot_bbox_intersect(box, &cursor->loc))
        nsfb_cursor_clear(nsfb, cursor);
    return 0;
}

static int wl_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    if (cursor && !cursor->plotted) {
        nsfb_cursor_plot(nsfb, cursor);
        damage(&cursor->savloc);
    }
    damage(box);
    return 0;
}

static int wl_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    if (cursor && cursor->plotted) {
        nsfb_bbox_t old = cursor->savloc;
        nsfb_cursor_clear(nsfb, cursor);
        nsfb_cursor_plot(nsfb, cursor);
        damage(&old);
        damage(&cursor->savloc);
    }
    return true;
}

static void repeat_tick(void)
{
    if (!W.rep_key || W.rep_rate <= 0) return;
    uint64_t t = now_ms();
    if (t < W.rep_next) return;
    push_key(NSFB_EVENT_KEY_DOWN, W.rep_code);
    W.rep_next = t + 1000 / W.rep_rate;
}

static bool wl_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    (void)nsfb;
    if (pop(event)) return true;
    uint64_t deadline = timeout < 0 ? 0 : now_ms() + timeout;
    int fd = wl_display_get_fd(W.display);

    for (;;) {
        flush();
        while (wl_display_prepare_read(W.display) != 0)
            wl_display_dispatch_pending(W.display);
        if (wl_display_flush(W.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(W.display);
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_QUIT;
            return true;
        }
        int wait = -1;
        uint64_t t = now_ms();
        if (timeout >= 0) wait = deadline > t ? (int)(deadline - t) : 0;
        if (W.rep_key) {
            int rw = W.rep_next > t ? (int)(W.rep_next - t) : 0;
            if (wait < 0 || rw < wait) wait = rw;
        }
        if (W.qhead != W.qtail) wait = 0;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, wait);
        if (r > 0 && (pfd.revents & (POLLERR | POLLHUP))) {
            wl_display_cancel_read(W.display);
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_QUIT;
            return true;
        }
        if (r > 0) {
            if (wl_display_read_events(W.display) < 0) {
                event->type = NSFB_EVENT_CONTROL;
                event->value.controlcode = NSFB_CONTROL_QUIT;
                return true;
            }
        } else {
            wl_display_cancel_read(W.display);
        }
        wl_display_dispatch_pending(W.display);
        repeat_tick();
        flush();
        wl_display_flush(W.display);
        if (pop(event)) return true;
        if (timeout == 0) return false;
        if (timeout > 0 && now_ms() >= deadline) return false;
    }
}

const nsfb_surface_rtns_t nexus_wl_rtns = {
    .defaults = wl_defaults,
    .initialise = wl_initialise,
    .finalise = wl_finalise,
    .input = wl_input,
    .claim = wl_claim,
    .update = wl_update,
    .cursor = wl_cursor,
    .geometry = wl_set_geometry,
};

NSFB_SURFACE_DEF(wl, NSFB_SURFACE_WL, &nexus_wl_rtns)
