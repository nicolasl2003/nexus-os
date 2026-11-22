/* Notifications du bureau Nexus : petites bulles en bas à droite.
 *
 *   nexus-notify [-i icône] [-a commande] "Titre" ["Texte"]
 *       envoie une notification (depuis un script, un programme...) ;
 *       -a : commande lancée si on clique sur la bulle.
 *   nexus-notifyd
 *       affiche les bulles (lancé à l'ouverture de session, autostart).
 *
 * Fonctionnement : chaque notification est un petit fichier texte
 *   ligne 1 : titre, ligne 2 : texte, ligne 3 : icône, ligne 4 : commande
 * déposé dans $XDG_RUNTIME_DIR/nexus-notify/ (celles de l'utilisateur) ou
 * dans /run/nexus/notify/ (celles du système, écrites par root : mises à
 * jour disponibles...). nexus-notifyd regarde ces dossiers deux fois par
 * seconde. Il surveille aussi lui-même la batterie et le Wi-Fi.
 */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include "network.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define SYS_SPOOL "/run/nexus/notify"
#define BOX_W     380
#define CARD_H    86
#define GAP       10
#define MAX_SHOWN 4
#define SHOW_MS   7000

struct notif {
    char title[128], body[256], icon[32], action[256];
    double until;          /* fin d'affichage (horloge monotone, en s) */
};
static struct notif shown[MAX_SHOWN];
static int nshown;
static struct nx_window *win;
static int cur_h = 1;
static int hover = -1, hover_close = -1;
static char user_spool[256];

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* ======================= Envoi (nexus-notify) ======================= */

static void spool_dir(char *out, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) {
        static char tmp[64];
        snprintf(tmp, sizeof tmp, "/run/user/%d", (int)getuid());
        rt = tmp;
    }
    snprintf(out, n, "%s/nexus-notify", rt);
}

static void one_line(char *s)
{
    for (; *s; s++) if (*s == '\n' || *s == '\r') *s = ' ';
}

int notify_send_main(int argc, char **argv)
{
    const char *icon = "info", *action = "";
    int i = 1;
    for (; i < argc - 1 && argv[i][0] == '-'; i += 2) {
        if (!strcmp(argv[i], "-i")) icon = argv[i + 1];
        else if (!strcmp(argv[i], "-a")) action = argv[i + 1];
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "Usage : nexus-notify [-i icône] [-a commande] \"Titre\" [\"Texte\"]\n");
        return 2;
    }
    char t[128], b[256], ic[32], ac[256];
    snprintf(t, sizeof t, "%s", argv[i]);
    snprintf(b, sizeof b, "%s", i + 1 < argc ? argv[i + 1] : "");
    snprintf(ic, sizeof ic, "%s", icon);
    snprintf(ac, sizeof ac, "%s", action);
    one_line(t); one_line(b); one_line(ic); one_line(ac);

    char dir[256], tmp[400], path[400];
    spool_dir(dir, sizeof dir);
    mkdir(dir, 0700);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(tmp, sizeof tmp, "%s/.%ld%09ld-%d", dir, (long)ts.tv_sec, ts.tv_nsec, (int)getpid());
    snprintf(path, sizeof path, "%s/%ld%09ld-%d", dir, (long)ts.tv_sec, ts.tv_nsec, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) { perror("nexus-notify"); return 1; }
    fprintf(f, "%s\n%s\n%s\n%s\n", t, b, ic, ac);
    fclose(f);
    if (rename(tmp, path) != 0) { perror("nexus-notify"); unlink(tmp); return 1; }
    return 0;
}

/* ======================= Affichage (nexus-notifyd) ======================= */

static void apply_size(void)
{
    int h = nshown ? nshown * (CARD_H + GAP) : 1;
    if (h != cur_h) {
        cur_h = h;
        zwlr_layer_surface_v1_set_size(win->layer, BOX_W, h);
    }
    /* Largeur fixe et pas de wl_surface_set_input_region : avec labwc, la
     * surface ne recevrait plus la souris (vérifié). Sans bulle, elle ne
     * fait qu'1 pixel de haut et reste transparente. */
    wl_surface_commit(win->surface);
    nx_redraw(win);
}

static void push_for(const char *title, const char *body, const char *icon, const char *action, int ms);
static void push(const char *title, const char *body, const char *icon, const char *action)
{
    push_for(title, body, icon, action, SHOW_MS);
}

static void push_for(const char *title, const char *body, const char *icon, const char *action, int ms)
{
    if (nshown == MAX_SHOWN) {   /* la plus ancienne laisse sa place */
        memmove(shown, shown + 1, sizeof shown[0] * (MAX_SHOWN - 1));
        nshown--;
    }
    struct notif *n = &shown[nshown++];
    snprintf(n->title, sizeof n->title, "%s", title);
    snprintf(n->body, sizeof n->body, "%s", body);
    snprintf(n->icon, sizeof n->icon, "%s", icon && *icon ? icon : "info");
    snprintf(n->action, sizeof n->action, "%s", action ? action : "");
    n->until = now_s() + ms / 1000.0;
    if (getenv("NX_DEBUG")) fprintf(stderr, "notification : %s — %s\n", title, body);
    apply_size();
}

static void dismiss(int i)
{
    if (i < 0 || i >= nshown) return;
    memmove(shown + i, shown + i + 1, sizeof shown[0] * (nshown - i - 1));
    nshown--;
    hover = hover_close = -1;
    apply_size();
}

static bool read_notif(const char *path, char lines[4][256])
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    for (int i = 0; i < 4; i++) {
        lines[i][0] = 0;
        if (fgets(lines[i], 256, f)) lines[i][strcspn(lines[i], "\n")] = 0;
    }
    fclose(f);
    return lines[0][0] != 0;
}

/* Notifications de l'utilisateur : affichées puis effacées */
static void scan_user(void)
{
    DIR *d = opendir(user_spool);
    if (!d) return;
    char *names[32];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 32)
        if (e->d_name[0] != '.') names[n++] = strdup(e->d_name);
    closedir(d);
    /* noms = horodatage : ordre d'arrivée */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
    for (int i = 0; i < n; i++) {
        char p[600], l[4][256];
        snprintf(p, sizeof p, "%s/%s", user_spool, names[i]);
        if (read_notif(p, l)) push(l[0], l[1], l[2], l[3]);
        unlink(p);
        free(names[i]);
    }
}

/* Notifications du système : chacune n'est montrée qu'une fois par session
 * (liste des noms déjà vus dans le dossier de l'utilisateur) */
static bool seen(const char *name)
{
    char p[400], line[256];
    snprintf(p, sizeof p, "%s/.vues", user_spool);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        found = !strcmp(line, name);
    }
    fclose(f);
    return found;
}

static void mark_seen(const char *name)
{
    char p[400];
    snprintf(p, sizeof p, "%s/.vues", user_spool);
    FILE *f = fopen(p, "a");
    if (f) { fprintf(f, "%s\n", name); fclose(f); }
}

static void scan_system(void)
{
    DIR *d = opendir(SYS_SPOOL);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strstr(e->d_name, ".tmp") || seen(e->d_name)) continue;
        char p[600], l[4][256];
        snprintf(p, sizeof p, "%s/%s", SYS_SPOOL, e->d_name);
        struct stat st;
        /* fichier de root uniquement (personne d'autre ne peut en déposer) */
        if (stat(p, &st) != 0 || st.st_uid != 0 || !S_ISREG(st.st_mode)) continue;
        if (read_notif(p, l)) push_for(l[0], l[1], l[2], l[3], 15000);   /* plus longtemps */
        mark_seen(e->d_name);
    }
    closedir(d);
}

/* ---- Batterie : faible (20 %), très faible (8 %), chargée ---- */

static int bat_stage = -1;   /* -1 inconnu, 0 normal, 1 faible, 2 très faible, 3 pleine */

static void check_battery(void)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300], type[32] = "", status[32] = "", cap[16] = "";
        FILE *f;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        if ((f = fopen(p, "r"))) { if (fgets(type, sizeof type, f)) type[strcspn(type, "\n")] = 0; fclose(f); }
        if (strcmp(type, "Battery")) continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        if ((f = fopen(p, "r"))) { if (!fgets(cap, sizeof cap, f)) cap[0] = 0; fclose(f); }
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
        if ((f = fopen(p, "r"))) { if (fgets(status, sizeof status, f)) status[strcspn(status, "\n")] = 0; fclose(f); }
        if (!cap[0]) continue;
        int c = atoi(cap);
        bool discharging = !strcmp(status, "Discharging");
        int stage = !discharging ? (!strcmp(status, "Full") || c >= 100 ? 3 : 0) : c <= 8 ? 2 : c <= 20 ? 1 : 0;
        char msg[96];
        if (bat_stage >= 0 && stage != bat_stage) {
            if (stage == 1) { snprintf(msg, sizeof msg, "Il reste %d %%. Branche le chargeur.", c); push("Batterie faible", msg, "power", ""); }
            if (stage == 2) { snprintf(msg, sizeof msg, "Il reste %d %% : l'ordinateur va bientôt s'éteindre.", c); push("Batterie très faible", msg, "power", ""); }
            if (stage == 3) push("Batterie chargée", "Tu peux débrancher le chargeur.", "power", "");
        }
        bat_stage = stage;
        break;
    }
    closedir(d);
}

/* ---- Wi-Fi : connecté / déconnecté ---- */

static int wifi_was = -1;
static char wifi_ssid[64];

static void check_wifi(void)
{
    char ssid[64] = "";
    int now = nx_wifi_connected(ssid, sizeof ssid) ? 1 : 0;
    if (wifi_was >= 0) {
        char msg[128];
        if (now && (!wifi_was || strcmp(ssid, wifi_ssid))) {
            snprintf(msg, sizeof msg, "Connecté au réseau « %s ».", ssid);
            push("Wi-Fi connecté", msg, "network", "nexus-network");
        } else if (!now && wifi_was) {
            snprintf(msg, sizeof msg, "Connexion au réseau « %s » perdue.", wifi_ssid);
            push("Wi-Fi déconnecté", msg, "network", "nexus-network");
        }
    }
    wifi_was = now;
    snprintf(wifi_ssid, sizeof wifi_ssid, "%s", ssid);
}

/* ---- Verrouillage automatique après inactivité (ext-idle-notify) ---- */

static struct ext_idle_notification_v1 *idle_note;

static void on_idled(void *d, struct ext_idle_notification_v1 *n)
{
    (void)d; (void)n;
    nx_spawn("nexus-lock");
}
static void on_resumed(void *d, struct ext_idle_notification_v1 *n) { (void)d; (void)n; }
static const struct ext_idle_notification_v1_listener idle_listener = { .idled = on_idled, .resumed = on_resumed };

static void setup_idle(void)
{
    if (idle_note) { ext_idle_notification_v1_destroy(idle_note); idle_note = NULL; }
    if (!nx.idle_notifier || !nx.seat || nx_conf.lock_minutes <= 0) return;
    idle_note = ext_idle_notifier_v1_get_idle_notification(nx.idle_notifier,
                                                           (uint32_t)nx_conf.lock_minutes * 60000u, nx.seat);
    ext_idle_notification_v1_add_listener(idle_note, &idle_listener, NULL);
}

static void notifyd_reload(void) { setup_idle(); nx_redraw(win); }

/* ---- Boucle : dossiers 2 fois par seconde, Wi-Fi toutes les 3 s,
 *      batterie toutes les 30 s, disparition des bulles ---- */

static double next_wifi, next_bat;

static int notifyd_timeout(void) { return 500; }

static void notifyd_tick(void)
{
    double t = now_s();
    scan_user();
    scan_system();
    if (t >= next_wifi) { check_wifi(); next_wifi = t + 3; }
    if (t >= next_bat) { check_battery(); next_bat = t + 30; }
    /* la bulle survolée reste affichée */
    for (int i = nshown - 1; i >= 0; i--)
        if (i != hover && t >= shown[i].until) dismiss(i);
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w; (void)height;
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    nx_color accent = nx_accent_color();
    /* les plus récentes en bas (près de la barre des tâches) */
    for (int i = 0; i < nshown; i++) {
        double y = i * (CARD_H + GAP), x = 0, cw = width;
        nx_glass(cr, x, y, cw, CARD_H, 14);
        if (hover == i) {
            nx_rounded_rect(cr, x, y, cw, CARD_H, 14);
            nx_set(cr, nx_fg(0.06));
            cairo_fill(cr);
        }
        /* liseré d'accent à gauche */
        nx_rounded_rect(cr, x + 8, y + 16, 4, CARD_H - 32, 2);
        nx_set(cr, accent);
        cairo_fill(cr);
        static const char *apps[] = { "files", "browser", "terminal", "settings", "monitor", "editor",
                                      "about", "store", "viewer", "calculator", "screenshot" };
        bool app = false;
        for (unsigned k = 0; k < sizeof apps / sizeof apps[0]; k++) app |= !strcmp(shown[i].icon, apps[k]);
        if (app) {
            nx_app_icon(cr, shown[i].icon, x + 24, y + (CARD_H - 40) / 2, 40, nx_rgb(NX_GLASS_TEXT));
        } else {   /* icône simple dans une pastille d'accent */
            cairo_new_path(cr);
            cairo_arc(cr, x + 44, y + CARD_H / 2, 20, 0, 2 * M_PI);
            nx_set(cr, nx_alpha(accent, 0.85));
            cairo_fill(cr);
            nx_icon(cr, shown[i].icon, x + 32, y + CARD_H / 2 - 12, 24, nx_rgb("#ffffff"));
        }
        nx_text(cr, shown[i].title, 14.5, true, nx_rgb(NX_GLASS_TEXT), x + 78, y + 16, cw - 124, 24, NX_LEFT);
        nx_text(cr, shown[i].body, 12.5, false, nx_rgb(NX_GLASS_SUBTLE), x + 78, y + 42, cw - 96, 24, NX_LEFT);
        /* croix de fermeture */
        double bx = x + cw - 34, by = y + 12;
        if (hover_close == i) {
            cairo_new_path(cr);
            cairo_arc(cr, bx + 11, by + 11, 13, 0, 2 * M_PI);
            nx_set(cr, nx_fg(0.15));
            cairo_fill(cr);
        }
        cairo_move_to(cr, bx + 6, by + 6); cairo_line_to(cr, bx + 16, by + 16);
        cairo_move_to(cr, bx + 16, by + 6); cairo_line_to(cr, bx + 6, by + 16);
        nx_set(cr, nx_rgb(NX_GLASS_SUBTLE));
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke(cr);
    }
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    if (getenv("NX_DEBUG")) fprintf(stderr, "souris : ev %d (%.0f, %.0f) bouton %d, %d bulle(s)\n", ev, x, y, value, nshown);
    int i = (int)(y / (CARD_H + GAP));
    bool inside = i >= 0 && i < nshown && y - i * (CARD_H + GAP) < CARD_H;
    bool on_close = inside && x > w->width - 44 && y - i * (CARD_H + GAP) < 40;
    if (ev == NX_LEAVE) inside = on_close = false;
    int h = inside ? i : -1, hc = on_close ? i : -1;
    if (h != hover || hc != hover_close) {
        if (hover >= 0 && h != hover && hover < nshown) shown[hover].until = now_s() + 3;   /* encore 3 s */
        hover = h;
        hover_close = hc;
        nx_redraw(w);
    }
    if (ev == NX_RELEASE && value == BTN_LEFT && inside) {
        if (!on_close && shown[i].action[0]) nx_spawn(shown[i].action);
        dismiss(i);
    }
}

int notifyd_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    spool_dir(user_spool, sizeof user_spool);
    mkdir(user_spool, 0700);
    /* Une seule instance par session */
    char pidf[300];
    snprintf(pidf, sizeof pidf, "%s/.notifyd.pid", user_spool);
    FILE *pf = fopen(pidf, "r");
    if (pf) {
        int pid = 0;
        bool alive = fscanf(pf, "%d", &pid) == 1 && pid > 1 && pid != getpid() && kill(pid, 0) == 0;
        fclose(pf);
        if (alive) return 0;
    }
    if ((pf = fopen(pidf, "w"))) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer };
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          BOX_W, 1, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE, "nexus-notify");
    if (!win) return 1;
    zwlr_layer_surface_v1_set_margin(win->layer, 0, 14, 14, 0);
    apply_size();
    /* Premier passage : état actuel du Wi-Fi et de la batterie, sans bulle */
    check_wifi();
    check_battery();
    next_wifi = now_s() + 3;
    next_bat = now_s() + 30;
    nx.timeout = notifyd_timeout;
    nx.tick = notifyd_tick;
    nx.reload = notifyd_reload;
    setup_idle();
    nx_run();
    return 0;
}
