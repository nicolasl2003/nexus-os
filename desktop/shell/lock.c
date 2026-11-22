/* nexus-lock — l'écran de verrouillage de Nexus OS.
 *
 * Super+L, « Verrouiller » dans le menu Démarrer, ou automatiquement après
 * quelques minutes d'inactivité (Paramètres → Affichage). Utilise le
 * protocole ext-session-lock du compositeur : tant que l'écran est verrouillé,
 * labwc n'affiche que nos surfaces et ne transmet le clavier qu'à nous, même
 * si ce programme plante (l'écran resterait alors noir et verrouillé).
 *
 * Le mot de passe est vérifié par /usr/local/libexec/nexus-account verify
 * (setuid : lui seul peut lire /etc/shadow), en arrière-plan.
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define HELPER "/usr/local/libexec/nexus-account"

static struct ext_session_lock_v1 *lock;
static bool locked, finished;
static struct nx_window *wins[8];
static int nwins;
static char user[64] = "?";
static char password[128];
static char message[160];
static bool msg_error;
static pid_t checker;
static bool unlocking;

/* ---------- Protocole ---------- */

static void on_locked(void *d, struct ext_session_lock_v1 *l) { (void)d; (void)l; locked = true; }
static void on_finished(void *d, struct ext_session_lock_v1 *l)
{
    (void)d; (void)l;
    /* refusé (un autre écran de verrouillage est déjà actif) */
    finished = true;
    nx_quit();
}
static const struct ext_session_lock_v1_listener lock_listener = { .locked = on_locked, .finished = on_finished };

/* ---------- Vérification du mot de passe ---------- */

static void submit(void)
{
    if (checker || unlocking) return;
    int fd[2];
    if (pipe(fd) != 0) return;
    checker = fork();
    if (checker == 0) {
        dup2(fd[0], 0);
        close(fd[0]); close(fd[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execl(HELPER, "nexus-account", "verify", user, (char *)NULL);
        _exit(127);
    }
    close(fd[0]);
    char line[160];
    snprintf(line, sizeof line, "%s\n", password);
    if (write(fd[1], line, strlen(line)) < 0) { /* l'aide répondra « faux » */ }
    explicit_bzero(line, sizeof line);
    close(fd[1]);
    explicit_bzero(password, sizeof password);
    msg_error = false;
    snprintf(message, sizeof message, "Vérification…");
    for (int i = 0; i < nwins; i++) nx_redraw(wins[i]);
}

static void check_done(void)
{
    if (!checker) return;
    int st;
    pid_t r = waitpid(checker, &st, WNOHANG);
    if (r == 0) return;
    checker = 0;
    if (r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        unlocking = true;
        snprintf(message, sizeof message, "Bon retour !");
        msg_error = false;
    } else {
        msg_error = true;
        snprintf(message, sizeof message, r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 127 ?
                 "Outil de vérification absent : redémarre l'ordinateur." : "Mot de passe incorrect. Réessaie.");
    }
    for (int i = 0; i < nwins; i++) nx_redraw(wins[i]);
}

/* ---------- Dessin ---------- */

static const char *jours[] = { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" };
static const char *mois[] = { "janvier", "février", "mars", "avril", "mai", "juin", "juillet",
                              "août", "septembre", "octobre", "novembre", "décembre" };

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nx_color white = nx_rgb("#ffffff"), accent = nx_accent_color();
    nx_color subtle = nx_rgb("#b9c1d3");
    static cairo_surface_t *bg;
    static int bw, bh;
    if (!bg || bw != width || bh != height) {
        if (bg) cairo_surface_destroy(bg);
        bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
        cairo_t *c = cairo_create(bg);
        nx_draw_wallpaper(c, nx_conf.wallpaper, width, height);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, height);
        cairo_pattern_add_color_stop_rgba(g, 0, 0.02, 0.03, 0.08, 0.45);
        cairo_pattern_add_color_stop_rgba(g, 1, 0.02, 0.03, 0.08, 0.75);
        cairo_set_source(c, g);
        cairo_paint(c);
        cairo_pattern_destroy(g);
        cairo_destroy(c);
        bw = width; bh = height;
    }
    cairo_set_source_surface(cr, bg, 0, 0);
    cairo_paint(cr);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[8], date[64];
    snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(date, sizeof date, "%s %d %s", jours[tm.tm_wday], tm.tm_mday, mois[tm.tm_mon]);
    double top = height * 0.14;
    nx_text(cr, hm, 104, true, white, 0, top, width, 124, NX_CENTER);
    nx_text(cr, date, 21, false, nx_alpha(white, 0.85), 0, top + 120, width, 34, NX_CENTER);

    /* Compte et mot de passe */
    double cy = height * 0.56;
    double av = 84;
    cairo_new_path(cr);
    cairo_arc(cr, width / 2.0, cy, av / 2 + 4, 0, 2 * M_PI);
    nx_set(cr, nx_alpha(accent, 0.9));
    cairo_fill(cr);
    nx_icon(cr, "user", width / 2.0 - av / 2 + 8, cy - av / 2 + 8, av - 16, white);
    nx_text(cr, user, 20, true, white, 0, cy + 52, width, 30, NX_CENTER);
    double fw = 360, fh = 48, fx = (width - fw) / 2, fy = cy + 96;
    nx_rounded_rect(cr, fx, fy, fw, fh, 24);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_fill_preserve(cr);
    if (msg_error) cairo_set_source_rgba(cr, 1, 0.45, 0.45, 0.9);
    else nx_set(cr, nx_alpha(accent, 0.9));
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);
    int n = 0;
    for (const char *p = password; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) n++;
    if (n) {
        double dx = width / 2.0 - (fmin(n, 20) - 1) * 7.5;
        for (int i = 0; i < n && i < 20; i++) {
            cairo_new_path(cr);
            cairo_arc(cr, dx + i * 15, fy + fh / 2, 4.5, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill(cr);
        }
    } else {
        nx_text(cr, checker ? "" : "Mot de passe", 15, false, subtle, fx, fy, fw, fh, NX_CENTER);
    }
    const char *m = message[0] ? message : "Écran verrouillé · tape ton mot de passe puis Entrée";
    nx_text(cr, m, 13.5, msg_error, msg_error ? nx_rgb("#ff9a9a") : subtle, 0, fy + fh + 14, width, 24, NX_CENTER);
}

/* ---------- Clavier ---------- */

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    (void)w;
    if (checker || unlocking) return;
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { submit(); return; }
    if (sym == XKB_KEY_Escape) explicit_bzero(password, sizeof password);
    else if (sym == XKB_KEY_BackSpace) {
        size_t l = strlen(password);
        while (l > 0 && ((unsigned char)password[l - 1] & 0xC0) == 0x80) l--;
        if (l > 0) l--;
        password[l] = 0;
    } else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
               strlen(password) + strlen(utf8) < sizeof password - 1) {
        strcat(password, utf8);
        if (msg_error) { message[0] = 0; msg_error = false; }
    } else return;
    for (int i = 0; i < nwins; i++) nx_redraw(wins[i]);
}

static int timeout(void) { return checker || unlocking ? 100 : 1000; }
static void tick(void)
{
    check_done();
    if (unlocking) {
        ext_session_lock_v1_unlock_and_destroy(lock);
        wl_display_roundtrip(nx.display);
        nx_quit();
        return;
    }
    for (int i = 0; i < nwins; i++) nx_redraw(wins[i]);   /* horloge */
}

int lock_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Un seul écran de verrouillage à la fois */
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char pidf[256];
    snprintf(pidf, sizeof pidf, "%s/nexus-lock.pid", rt ? rt : "/tmp");
    FILE *f = fopen(pidf, "r");
    if (f) {
        int pid = 0;
        bool alive = fscanf(f, "%d", &pid) == 1 && pid > 1 && kill(pid, 0) == 0;
        fclose(f);
        if (alive) return 0;
    }
    if ((f = fopen(pidf, "w"))) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }

    struct passwd *pw = getpwuid(getuid());
    if (pw) snprintf(user, sizeof user, "%s", pw->pw_name);
    if (!nx_init()) return 1;
    signal(SIGCHLD, SIG_DFL);            /* pour lire la réponse de nexus-account */
    if (!nx.lock_manager) {
        fprintf(stderr, "nexus-lock : le compositeur ne permet pas de verrouiller l'écran.\n");
        return 1;
    }
    lock = ext_session_lock_manager_v1_lock(nx.lock_manager);
    ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
    struct nx_handlers h = { .draw = draw, .key = key };
    for (int i = 0; i < nx.noutputs && nwins < 8; i++) wins[nwins++] = nx_lock_window(h, lock, nx.outputs[i]);
    nx.timeout = timeout;
    nx.tick = tick;
    nx_run();
    unlink(pidf);
    explicit_bzero(password, sizeof password);
    return finished ? 1 : 0;
}
