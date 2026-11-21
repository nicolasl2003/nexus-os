/*
 * nexus-ui — petite bibliothèque d'interface du bureau Nexus OS.
 *
 * Elle fournit tout ce dont ont besoin les programmes du bureau (barre des
 * tâches, menu, fond d'écran, paramètres) :
 *   - la connexion Wayland et des fenêtres "layer-shell" (éléments du bureau)
 *     ou "xdg" (fenêtres normales), dessinées avec cairo dans des tampons
 *     de mémoire partagée ;
 *   - la souris et le clavier (xkbcommon) ;
 *   - des aides de dessin : couleurs, rectangles arrondis, texte (pango),
 *     icônes vectorielles, logo et fonds d'écran Nexus ;
 *   - la configuration du bureau (~/.config/nexus/desktop.conf).
 */
#ifndef NEXUS_UI_H
#define NEXUS_UI_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

/* ---------- Couleurs ---------- */

typedef struct { double r, g, b, a; } nx_color;

nx_color nx_rgb(const char *hex);                 /* "#rrggbb" */
nx_color nx_alpha(nx_color c, double a);
nx_color nx_mix(nx_color a, nx_color b, double t); /* t=0 : a, t=1 : b */
void nx_set(cairo_t *cr, nx_color c);

/* Palette du bureau */
#define NX_DARK      "#1b1f2a"
#define NX_DARKER    "#141821"
#define NX_LIGHT     "#f4f6fa"
#define NX_TEXT      "#1b1f2a"
#define NX_SUBTLE    "#6b7385"

/* ---------- Configuration ---------- */

struct nx_config {
    char wallpaper[512]; /* "preset:nexus", "color:#rrggbb" ou chemin d'une image PNG */
    char accent[16];     /* couleur d'accent "#rrggbb" */
    int text_scale;      /* taille du texte en % : 100 (normal) ou 125 (grand) */
    bool widgets;        /* widgets du bureau (horloge, performances, tâches) */
    bool desktop_icons;  /* icônes "Ce PC", "Mes fichiers"... sur le bureau */
    int panel_size;      /* (ancien réglage) barre : 0 petite, 1 moyenne, 2 grande */
    int panel_height;    /* hauteur de la barre des tâches en px (36 à 110) */
    int menu_scale;      /* taille du menu Démarrer en % (80 à 170) */
    int widget_scale;    /* taille des widgets en % (70 à 170) */
    int widget_style;    /* widgets : 0 sombres, 1 clairs, 2 couleur d'accent, 3 transparents */
    int widget_mr, widget_mt;  /* position des widgets : marges droite et haut (px) */
    int icon_size;       /* icônes des applications : 0 petites, 1 moyennes, 2 grandes */
    int icon_style;      /* 0 en couleurs (pastille), 1 monochromes */
    int icon_shape;      /* forme de la pastille : 0 arrondie, 1 carrée, 2 ronde */
    int glass_opacity;   /* barre et menus : 0 transparents, 1 translucides, 2 opaques */
    bool glass_tint;     /* barre et menus teintés de la couleur d'accent */
};

struct nx_accent { const char *hex, *name; };
extern const struct nx_accent nx_accents[];
extern const int nx_accent_count;

void nx_config_load(struct nx_config *c);
int nx_config_save(const struct nx_config *c);
extern struct nx_config nx_conf;       /* configuration courante */
extern double nx_scale;                 /* nx_conf.text_scale / 100 */
nx_color nx_accent_color(void);
int nx_panel_height(void);              /* hauteur de la barre des tâches (px) */
double nx_icon_factor(void);            /* 0.82, 1 ou 1.2 selon icon_size */
/* Fond des surfaces sombres (barre, menu, panneaux) selon les réglages */
void nx_glass_source(cairo_t *cr, double y0, double y1);

/* Touches de modification enfoncées (Ctrl, Maj) */
bool nx_mod_ctrl(void);
bool nx_mod_shift(void);
/* Met une chaîne entre apostrophes pour le shell (noms de fichiers) */
void nx_shell_quote(char *out, size_t n, const char *s);

/* ---------- Dessin ---------- */

enum { NX_LEFT, NX_CENTER, NX_RIGHT };

void nx_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r);
/* Panneau "verre" sombre du thème Nexus (fond translucide, liseré, ombre) */
void nx_glass(cairo_t *cr, double x, double y, double w, double h, double r);
#define NX_GLASS_TEXT   "#eef1f7"
#define NX_GLASS_SUBTLE "#9aa3b8"
/* Texte centré verticalement dans (x,y,w,h), coupé avec "…" si trop long.
 * size en pixels (multipliée par nx_scale). Retourne la largeur dessinée. */
double nx_text(cairo_t *cr, const char *txt, double size, bool bold, nx_color c,
               double x, double y, double w, double h, int align);
double nx_text_width(cairo_t *cr, const char *txt, double size, bool bold);

void nx_logo(cairo_t *cr, double x, double y, double size);
/* Icône vectorielle : "terminal", "settings", "monitor", "editor", "info",
 * "power", "reboot", "logout", "user", "image", "palette", "display",
 * "folder", "file", "exec", "archive", "home", "downloads", "disk", "trash",
 * sinon une lettre dans un carré de couleur d'accent. */
void nx_icon(cairo_t *cr, const char *name, double x, double y, double size, nx_color fg);
/* Logo d'application ("files", "browser", "terminal", "settings", "monitor",
 * "editor", "about", "store", "viewer", "calculator"...) selon le style choisi
 * (pastille colorée ou monochrome) ; un autre nom donne son initiale.
 * fg = couleur en style monochrome. */
void nx_app_icon(cairo_t *cr, const char *app, double x, double y, double size, nx_color fg);
/* Identifiant de logo pour une application (.desktop, app_id, paquet) */
const char *nx_app_icon_for(const char *name);

/* Fonds d'écran : prédéfinis (dessinés), couleur unie ou image PNG */
struct nx_preset { const char *id, *name, *c1, *c2, *c3; };
extern const struct nx_preset nx_presets[];
extern const int nx_preset_count;
void nx_draw_wallpaper(cairo_t *cr, const char *spec, double w, double h);
/* Même chose, en gardant le résultat en mémoire (miniatures, aperçus) */
void nx_draw_wallpaper_cached(cairo_t *cr, const char *spec, double w, double h);

/* ---------- Fenêtres Wayland ---------- */

enum nx_pointer_event { NX_MOTION, NX_PRESS, NX_RELEASE, NX_LEAVE, NX_SCROLL };

struct nx_window;
struct nx_handlers {
    void (*draw)(struct nx_window *w, cairo_t *cr, int width, int height);
    void (*pointer)(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value);
    void (*key)(struct nx_window *w, xkb_keysym_t sym, const char *utf8);
    void (*closed)(struct nx_window *w);
};

struct nx_buffer {
    struct wl_buffer *wl;
    cairo_surface_t *cs;
    void *data;
    size_t size;
    int width, height;
    bool busy;
};

struct nx_window {
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    int width, height, pending_w, pending_h;
    bool configured, dirty;
    struct nx_buffer bufs[2];
    struct nx_handlers h;
    double px, py;          /* dernière position de la souris */
};

struct nx_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct xdg_wm_base *wm_base;
    struct zwlr_foreign_toplevel_manager_v1 *toplevels;
    struct zxdg_decoration_manager_v1 *decorations;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    struct wl_surface *cursor_surface;
    struct wl_cursor_image *cursor_image;
    struct nx_window *pointer_focus, *keyboard_focus;
    struct nx_window *windows[8];
    int nwindows;
    bool running;
    /* Rappels optionnels du programme */
    int  (*timeout)(void);  /* ms avant le prochain tick, -1 = jamais */
    void (*tick)(void);
    void (*reload)(void);   /* SIGUSR1 : la configuration a changé */
};
extern struct nx_state nx;

bool nx_init(void);
struct nx_window *nx_layer_window(struct nx_handlers h, uint32_t layer, uint32_t anchor,
                                  int width, int height, int exclusive_zone,
                                  uint32_t keyboard, const char *ns);
struct nx_window *nx_toplevel_window(struct nx_handlers h, const char *title,
                                     const char *app_id, int width, int height);
void nx_redraw(struct nx_window *w);
void nx_run(void);
void nx_quit(void);

/* Fichiers ouverts récemment (~/.local/share/nexus/recents) */
struct nx_recent { char path[1024]; long when; };
void nx_recent_add(const char *path);
int nx_recent_load(struct nx_recent *out, int max);   /* du plus récent au plus ancien */
/* "il y a 3 min", "il y a 2 h", "hier"... */
void nx_ago(char *out, size_t n, long when);

/* Fenêtres "panneau" à instance unique (menu, volume, réseau) : si une
 * instance tourne déjà, la ferme et renvoie true (= un 2e clic referme). */
bool nx_toggle_instance(const char *name);
void nx_instance_done(void);

/* Lance une commande en arrière-plan, détachée (sh -c) */
void nx_spawn(const char *cmd);
/* Signale aux programmes du bureau que la configuration a changé */
void nx_notify_reload(void);

int nx_bench(void);
int nx_render(int argc, char **argv);   /* nexus-shell render */   /* nexus-shell bench */

#endif
