/* nexus-greeter — l'écran de connexion graphique de Nexus OS.
 *
 * Lancé par nexus-login (root) dans un compositeur labwc dédié. Il affiche le
 * fond d'écran, l'heure, la liste des comptes et un champ mot de passe ;
 * vérifie le mot de passe dans /etc/shadow ; en cas de succès, écrit le nom du
 * compte dans /run/nexus-login.user et ferme son compositeur : nexus-login
 * ouvre alors la session de l'utilisateur.
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <crypt.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define RESULT_FILE "/run/nexus-login.user"
#define LAST_USER   "/usr/local/var/lib/nexus/dernier-compte"

struct account { char name[64], label[64]; };
static struct account accounts[16];
static int naccounts, cur;
static char password[128];
static char message[160];
static bool msg_error, logging_in;
static time_t blocked_until;
static struct nx_window *win;

/* ---------- Comptes ---------- */

static void load_accounts(void)
{
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) && naccounts < 15) {
        if (pw->pw_uid < 1000 || pw->pw_uid >= 65534) continue;
        if (strstr(pw->pw_shell, "nologin") || strstr(pw->pw_shell, "false")) continue;
        snprintf(accounts[naccounts].name, 64, "%s", pw->pw_name);
        snprintf(accounts[naccounts].label, 64, "%s", pw->pw_name);
        naccounts++;
    }
    endpwent();
    snprintf(accounts[naccounts].name, 64, "root");
    snprintf(accounts[naccounts].label, 64, "Administrateur");
    naccounts++;
    /* Présélectionner le dernier compte utilisé */
    FILE *f = fopen(LAST_USER, "r");
    if (f) {
        char last[64] = "";
        if (fgets(last, sizeof last, f)) last[strcspn(last, "\n")] = 0;
        fclose(f);
        for (int i = 0; i < naccounts; i++) if (!strcmp(accounts[i].name, last)) cur = i;
    }
}

/* Mot de passe vide autorisé uniquement si le compte n'en a pas (mode live) */
static bool check_password(const char *user, const char *pass)
{
    struct spwd *sp = getspnam(user);
    if (!sp) return false;
    const char *hash = sp->sp_pwdp;
    if (!hash[0]) return pass[0] == 0;
    if (hash[0] == '!' || hash[0] == '*') return false;   /* compte verrouillé */
    const char *c = crypt(pass, hash);
    return c && !strcmp(c, hash);
}

static bool has_empty_password(const char *user)
{
    struct spwd *sp = getspnam(user);
    return sp && !sp->sp_pwdp[0];
}

static void submit(void)
{
    if (logging_in || time(NULL) < blocked_until) return;
    const char *user = accounts[cur].name;
    if (check_password(user, password)) {
        int fd = open(RESULT_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            if (write(fd, user, strlen(user)) < 0) { /* rien : nexus-login réaffichera l'écran */ }
            close(fd);
        }
        mkdir("/usr/local/var/lib/nexus", 0755);
        FILE *f = fopen(LAST_USER, "w");
        if (f) { fprintf(f, "%s\n", user); fclose(f); }
        logging_in = true;
        msg_error = false;
        snprintf(message, sizeof message, "Ouverture de la session…");
    } else {
        msg_error = true;
        snprintf(message, sizeof message, "Mot de passe incorrect. Réessaie.");
        blocked_until = time(NULL) + 1;   /* freine les essais au hasard */
    }
    memset(password, 0, sizeof password);
    nx_redraw(win);
}

/* ---------- Zones cliquables ---------- */

enum kind { K_NONE, K_FIELD, K_GO, K_USER, K_REBOOT, K_POWEROFF };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[32];
static int nrects;
static int hover_kind = -1, hover_index = -1;

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 32) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
}
static bool hot(enum kind k, int i) { return (int)k == hover_kind && i == hover_index; }

/* ---------- Dessin ---------- */

static const char *jours[] = { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" };
static const char *mois[] = { "janvier", "février", "mars", "avril", "mai", "juin", "juillet",
                              "août", "septembre", "octobre", "novembre", "décembre" };

static cairo_surface_t *bg;
static int bg_w, bg_h;

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    nx_color white = nx_rgb("#ffffff"), text = nx_rgb(NX_GLASS_TEXT), subtle = nx_rgb(NX_GLASS_SUBTLE);
    nx_color accent = nx_accent_color();

    /* Fond d'écran (calculé une fois), assombri pour la lisibilité */
    if (!bg || bg_w != width || bg_h != height) {
        if (bg) cairo_surface_destroy(bg);
        bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
        cairo_t *c = cairo_create(bg);
        nx_draw_wallpaper(c, nx_conf.wallpaper, width, height);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, height);
        cairo_pattern_add_color_stop_rgba(g, 0, 0.02, 0.03, 0.08, 0.35);
        cairo_pattern_add_color_stop_rgba(g, 1, 0.02, 0.03, 0.08, 0.65);
        cairo_set_source(c, g);
        cairo_paint(c);
        cairo_pattern_destroy(g);
        cairo_destroy(c);
        bg_w = width; bg_h = height;
    }
    cairo_set_source_surface(cr, bg, 0, 0);
    cairo_paint(cr);

    /* Logo en haut à gauche */
    nx_logo(cr, 32, 28, 40);
    nx_text(cr, "Nexus OS", 18, true, white, 84, 28, 0, 40, NX_LEFT);

    /* Heure et date */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8], date[64];
    snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(date, sizeof date, "%s %d %s", jours[tm.tm_wday], tm.tm_mday, mois[tm.tm_mon]);
    double top = height * 0.10;
    nx_text(cr, hm, 92, true, white, 0, top, width, 110, NX_CENTER);
    nx_text(cr, date, 20, false, nx_alpha(white, 0.85), 0, top + 108, width, 32, NX_CENTER);

    /* Carte de connexion */
    double cw = 400, ch = 290, cx = (width - cw) / 2, cy = height * 0.42;
    nx_glass(cr, cx, cy, cw, ch, 22);
    double av = 92;
    cairo_new_path(cr);
    cairo_arc(cr, width / 2.0, cy, av / 2 + 5, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.07, 0.09, 0.16, 1);
    cairo_fill(cr);
    nx_icon(cr, "user", width / 2.0 - av / 2, cy - av / 2, av, white);
    nx_text(cr, accounts[cur].label, 22, true, text, cx, cy + 58, cw, 32, NX_CENTER);
    bool empty_ok = has_empty_password(accounts[cur].name);
    /* Champ mot de passe */
    double fx = cx + 40, fy = cy + 112, fw = cw - 80, fh = 50;
    nx_rounded_rect(cr, fx, fy, fw, fh, 12);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_fill_preserve(cr);
    if (msg_error) cairo_set_source_rgba(cr, 1, 0.4, 0.4, 0.8);
    else nx_set(cr, nx_alpha(accent, 0.85));
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);
    add_rect(fx, fy, fw, fh, K_FIELD, 0);
    size_t n = 0;
    for (const char *p = password; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) n++;
    if (n) {
        for (size_t i = 0; i < n && i < 24; i++) {
            cairo_new_path(cr);
            cairo_arc(cr, fx + 22 + i * 15, fy + fh / 2, 4.5, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill(cr);
        }
    } else {
        nx_text(cr, empty_ok ? "Pas de mot de passe : appuie sur Entrée" : "Mot de passe", 14.5, false,
                subtle, fx + 18, fy, fw - 70, fh, NX_LEFT);
    }
    /* Bouton flèche */
    double gx = fx + fw - 44, gy = fy + 7;
    nx_rounded_rect(cr, gx, gy, 36, 36, 9);
    nx_set(cr, hot(K_GO, 0) ? nx_mix(accent, white, 0.2) : accent);
    cairo_fill(cr);
    cairo_move_to(cr, gx + 11, gy + 18); cairo_line_to(cr, gx + 25, gy + 18);
    cairo_move_to(cr, gx + 19, gy + 12); cairo_line_to(cr, gx + 25, gy + 18); cairo_line_to(cr, gx + 19, gy + 24);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 2.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
    add_rect(gx, gy, 36, 36, K_GO, 0);
    if (message[0])
        nx_text(cr, message, 13.5, false, msg_error ? nx_rgb("#ff8a8a") : text, cx, fy + fh + 14, cw, 24, NX_CENTER);
    else
        nx_text(cr, "Entrée pour te connecter", 12.5, false, subtle, cx, fy + fh + 14, cw, 24, NX_CENTER);

    /* Comptes, en bas à gauche */
    double uy = height - 32 - naccounts * 60;
    for (int i = 0; i < naccounts; i++) {
        double y = uy + i * 60, x = 32, uw = 250;
        nx_rounded_rect(cr, x, y, uw, 52, 14);
        if (i == cur) nx_set(cr, nx_alpha(accent, 0.45));
        else cairo_set_source_rgba(cr, 0.08, 0.10, 0.18, hot(K_USER, i) ? 0.85 : 0.6);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        nx_icon(cr, "user", x + 10, y + 9, 34, white);
        nx_text(cr, accounts[i].label, 14.5, i == cur, white, x + 56, y, uw - 66, 52, NX_LEFT);
        add_rect(x, y, uw, 52, K_USER, i);
    }

    /* Alimentation, en bas à droite */
    struct { enum kind k; const char *icon, *label; } pw[] = {
        { K_REBOOT, "reboot", "Redémarrer" }, { K_POWEROFF, "power", "Éteindre" },
    };
    for (int i = 0; i < 2; i++) {
        double bx = width - 32 - 56 - (1 - i) * 72, by = height - 32 - 56;
        cairo_new_path(cr);
        cairo_arc(cr, bx + 28, by + 28, 28, 0, 2 * M_PI);
        if (hot(pw[i].k, 0)) {
            if (pw[i].k == K_POWEROFF) cairo_set_source_rgba(cr, 0.85, 0.3, 0.3, 0.6);
            else nx_set(cr, nx_alpha(accent, 0.55));
        } else {
            cairo_set_source_rgba(cr, 0.08, 0.10, 0.18, 0.7);
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        nx_icon(cr, pw[i].icon, bx + 16, by + 16, 24, white);
        if (hot(pw[i].k, 0))
            nx_text(cr, pw[i].label, 12.5, false, white, bx - 30, by - 26, 116, 20, NX_CENTER);
        add_rect(bx, by, 56, 56, pw[i].k, 0);
    }
}

/* ---------- Souris et clavier ---------- */

static struct rect *hit(double x, double y)
{
    for (int i = nrects - 1; i >= 0; i--)
        if (x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h)
            return &rects[i];
    return NULL;
}

static void select_account(int i)
{
    if (i == cur) return;
    cur = i;
    memset(password, 0, sizeof password);
    message[0] = 0;
    msg_error = false;
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct rect *r = hit(x, y);
    int k = r ? (int)r->kind : -1, i = r ? r->index : -1;
    if (ev == NX_MOTION || ev == NX_LEAVE) {
        if (ev == NX_LEAVE) k = i = -1;
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
    } else if (ev == NX_RELEASE && value == BTN_LEFT && r && !logging_in) {
        switch (r->kind) {
        case K_GO: submit(); break;
        case K_USER: select_account(r->index); break;
        case K_REBOOT: nx_spawn("/sbin/reboot"); break;
        case K_POWEROFF: nx_spawn("/sbin/poweroff"); break;
        default: break;
        }
        nx_redraw(w);
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (logging_in) return;
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { submit(); return; }
    if (sym == XKB_KEY_Escape) { memset(password, 0, sizeof password); message[0] = 0; }
    else if (sym == XKB_KEY_Tab || sym == XKB_KEY_Down) select_account((cur + 1) % naccounts);
    else if (sym == XKB_KEY_Up) select_account((cur + naccounts - 1) % naccounts);
    else if (sym == XKB_KEY_BackSpace) {
        size_t n = strlen(password);
        while (n > 0 && ((unsigned char)password[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        password[n] = 0;
    } else if (utf8[0] && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
               strlen(password) + strlen(utf8) < sizeof password - 1) {
        strcat(password, utf8);
        if (msg_error) { message[0] = 0; msg_error = false; }
    } else return;
    nx_redraw(w);
}

/* Horloge à jour ; après une connexion réussie, on ferme le compositeur */
static int timeout(void) { return logging_in ? 400 : 1000; }
static void tick(void)
{
    if (logging_in) {
        const char *pid = getenv("LABWC_PID");
        if (pid && atoi(pid) > 1) kill(atoi(pid), SIGTERM);
        nx_quit();
        return;
    }
    nx_redraw(win);
}

int greeter_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (getuid() != 0) { fprintf(stderr, "nexus-greeter doit être lancé par nexus-login (root).\n"); return 1; }
    load_accounts();
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          0, 0, -1, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, "nexus-greeter");
    if (!win) return 1;
    nx.timeout = timeout;
    nx.tick = tick;
    nx_run();
    return 0;
}
