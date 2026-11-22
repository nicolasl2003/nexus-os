/* nexus-assistant — l'assistant de configuration graphique de Nexus OS.
 *
 * Lancé par nexus-login (root) au premier démarrage, dans un compositeur labwc
 * dédié (comme l'écran de connexion). Une fenêtre en plusieurs étapes :
 *
 *   Bienvenue → Clavier → Ordinateur → Administrateur → Ton compte →
 *   Apparence → Découvrir Nexus → Terminé
 *
 * Il explique le fonctionnement du système, puis applique les réglages :
 * disposition du clavier (nexus-keymap, immédiatement), nom de la machine,
 * mot de passe root, compte utilisateur (groupes du bureau), couleur et fond
 * d'écran. Enfin il crée /data/etc/.configured : nexus-login affiche alors
 * l'écran de connexion, avec le nouveau compte présélectionné.
 *
 * L'assistant en mode texte (nexus-setup) reste disponible si l'affichage
 * graphique ne fonctionne pas.
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define LAST_USER  "/usr/local/var/lib/nexus/dernier-compte"
#define CARD_W     880
#define CARD_H     580
#define SIDE_W     230

static struct nx_window *win;

/* ======================= Étapes ======================= */

enum { P_WELCOME, P_KEYBOARD, P_HOST, P_ROOT, P_USER, P_LOOK, P_TOUR, P_DONE, P_COUNT };
static const char *step_names[P_COUNT] = {
    "Bienvenue", "Clavier", "Ordinateur", "Administrateur", "Ton compte", "Apparence",
    "Découvrir Nexus", "Terminé",
};
static int page = P_WELCOME;
static char error[200];
static bool finished;          /* réglages appliqués, on quitte bientôt */
static double finish_at;

/* ======================= Champs de saisie ======================= */

enum { F_HOST, F_ROOT1, F_ROOT2, F_USER, F_USER1, F_USER2, F_COUNT };
struct field { char text[128]; bool secret; const char *placeholder; };
static struct field fields[F_COUNT] = {
    [F_HOST]  = { "nexus", false, "nom-de-l-ordinateur" },
    [F_ROOT1] = { "", true, "Mot de passe administrateur" },
    [F_ROOT2] = { "", true, "Confirme le mot de passe" },
    [F_USER]  = { "", false, "prenom (en minuscules)" },
    [F_USER1] = { "", true, "Ton mot de passe" },
    [F_USER2] = { "", true, "Confirme ton mot de passe" },
};
static int focus = -1;

/* Champs de chaque étape (dans l'ordre de Tab) */
static int page_fields(int p, int out[3])
{
    switch (p) {
    case P_HOST: out[0] = F_HOST; return 1;
    case P_ROOT: out[0] = F_ROOT1; out[1] = F_ROOT2; return 2;
    case P_USER: out[0] = F_USER; out[1] = F_USER1; out[2] = F_USER2; return 3;
    default: return 0;
    }
}

/* ======================= Clavier ======================= */

static const struct { const char *id, *layout, *variant, *label; } kbs[] = {
    { "fr", "fr", "", "Français (AZERTY)" },     { "fr-bepo", "fr", "bepo", "Français (BÉPO)" },
    { "fr-mac", "fr", "mac", "Français (Mac)" },   { "be", "be", "", "Belge" },
    { "ch-fr", "ch", "fr", "Suisse romand" },      { "ch-de", "ch", "", "Suisse alémanique" },
    { "ca", "ca", "", "Canadien français" },       { "us", "us", "", "Anglais (US)" },
    { "gb", "gb", "", "Anglais (UK)" },            { "de", "de", "", "Allemand" },
    { "es", "es", "", "Espagnol" },                { "it", "it", "", "Italien" },
    { "pt", "pt", "", "Portugais" },
};
#define NKBS ((int)(sizeof kbs / sizeof kbs[0]))
static int kb_cur;
static char kb_test[80];

static int run(char *const argv[], const char *input);

static void set_keyboard(int i)
{
    kb_cur = i;
    kb_test[0] = 0;
    char *a[] = { "/usr/sbin/nexus-keymap", "set", (char *)kbs[i].id, NULL };
    run(a, NULL);
    /* compositeur de l'assistant : fichier d'environnement relu sur SIGHUP */
    const char *dir = getenv("NEXUS_ASSISTANT_CONF");
    if (dir) {
        char p[300];
        snprintf(p, sizeof p, "%s/environment", dir);
        FILE *f = fopen(p, "w");
        if (f) {
            fprintf(f, "XKB_DEFAULT_LAYOUT=%s\nXKB_DEFAULT_VARIANT=%s\n", kbs[i].layout, kbs[i].variant);
            fclose(f);
        }
        const char *lp = getenv("LABWC_PID");
        if (lp && atoi(lp) > 1) kill(atoi(lp), SIGHUP);
    }
}

static void load_keyboard(void)
{
    char id[32] = "fr";
    FILE *f = fopen("/etc/keyboard", "r");
    if (f) { if (fgets(id, sizeof id, f)) id[strcspn(id, "\n")] = 0; fclose(f); }
    for (int i = 0; i < NKBS; i++) if (!strcmp(kbs[i].id, id)) kb_cur = i;
}

/* ======================= Apparence ======================= */

static int accent_cur, wall_cur;

/* ======================= Outils ======================= */

/* Lance une commande (en root) et attend ; input éventuel sur stdin */
static int run(char *const argv[], const char *input)
{
    int fd[2];
    if (input && pipe(fd)) return -1;
    void (*old)(int) = signal(SIGCHLD, SIG_DFL);
    pid_t pid = fork();
    if (pid == 0) {
        if (input) { dup2(fd[0], 0); close(fd[0]); close(fd[1]); }
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execv(argv[0], argv);
        _exit(127);
    }
    if (input) {
        close(fd[0]);
        size_t l = strlen(input);
        if (write(fd[1], input, l) != (ssize_t)l) { /* la commande échouera */ }
        close(fd[1]);
    }
    int st = 0;
    bool ok = pid > 0 && waitpid(pid, &st, 0) > 0;
    signal(SIGCHLD, old);
    return ok && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int set_password(const char *user, const char *pw)
{
    char line[300];
    snprintf(line, sizeof line, "%s:%s\n", user, pw);
    char *a[] = { "/bin/busybox", "chpasswd", NULL };
    int r = run(a, line);
    explicit_bzero(line, sizeof line);
    return r;
}

static void wipe_secrets(void)
{
    for (int i = 0; i < F_COUNT; i++) if (fields[i].secret) explicit_bzero(fields[i].text, sizeof fields[i].text);
}

/* ======================= Vérifications ======================= */

static bool valid_host(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 40 || s[0] == '-') return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-') return false;
    return true;
}

static bool valid_user(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 32 || !(islower((unsigned char)s[0]) || s[0] == '_')) return false;
    for (; *s; s++)
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '_' && *s != '-') return false;
    static const char *reserved[] = { "root", "bin", "daemon", "nobody", "video", "audio", "netdev" };
    for (unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
        if (!strcmp(s - n, reserved[i])) return false;
    return true;
}

static bool check_password_pair(int a, int b, const char *who)
{
    if (strlen(fields[a].text) < 4) {
        snprintf(error, sizeof error, "Le mot de passe %s doit faire au moins 4 caractères.", who);
        focus = a;
        return false;
    }
    if (strchr(fields[a].text, ':')) {
        snprintf(error, sizeof error, "Le caractère « : » n'est pas autorisé dans un mot de passe.");
        focus = a;
        return false;
    }
    if (strcmp(fields[a].text, fields[b].text)) {
        snprintf(error, sizeof error, "Les deux mots de passe %s ne sont pas identiques.", who);
        fields[b].text[0] = 0;
        focus = b;
        return false;
    }
    return true;
}

/* L'étape courante est-elle complète ? (message d'erreur sinon) */
static bool page_ok(void)
{
    error[0] = 0;
    switch (page) {
    case P_HOST:
        if (!valid_host(fields[F_HOST].text)) {
            snprintf(error, sizeof error, "Nom invalide : lettres, chiffres et tirets (-), sans espace.");
            focus = F_HOST;
            return false;
        }
        return true;
    case P_ROOT:
        return check_password_pair(F_ROOT1, F_ROOT2, "administrateur");
    case P_USER:
        if (!valid_user(fields[F_USER].text)) {
            snprintf(error, sizeof error, "Nom de compte : minuscules, chiffres, - et _, commence par une lettre.");
            focus = F_USER;
            return false;
        }
        return check_password_pair(F_USER1, F_USER2, "du compte");
    default:
        return true;
    }
}

/* ======================= Application des réglages ======================= */

static void save_look_for(const char *home, uid_t uid, gid_t gid)
{
    char old_home[512] = "";
    if (getenv("HOME")) snprintf(old_home, sizeof old_home, "%s", getenv("HOME"));
    char *old_xdg = getenv("XDG_CONFIG_HOME") ? strdup(getenv("XDG_CONFIG_HOME")) : NULL;
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", home, 1);
    nx_config_save(&nx_conf);
    if (old_home[0]) setenv("HOME", old_home, 1);
    if (old_xdg) { setenv("XDG_CONFIG_HOME", old_xdg, 1); free(old_xdg); }
    char p[600];
    snprintf(p, sizeof p, "%s/.config", home);
    if (chown(p, uid, gid) != 0) { /* sans gravité */ }
    snprintf(p, sizeof p, "%s/.config/nexus", home);
    if (chown(p, uid, gid) != 0) { /* sans gravité */ }
    snprintf(p, sizeof p, "%s/.config/nexus/desktop.conf", home);
    if (chown(p, uid, gid) != 0) { /* sans gravité */ }
}

static bool apply_all(void)
{
    const char *host = fields[F_HOST].text, *user = fields[F_USER].text;

    /* 1. Nom de la machine */
    FILE *f = fopen("/etc/hostname", "w");
    if (f) { fprintf(f, "%s\n", host); fclose(f); }
    if (access("/data", F_OK) == 0 && (f = fopen("/data/hostname", "w"))) { fprintf(f, "%s\n", host); fclose(f); }
    if (sethostname(host, strlen(host)) != 0) { /* /etc/hostname suffira au prochain démarrage */ }

    /* 2. Mot de passe administrateur */
    if (set_password("root", fields[F_ROOT1].text) != 0) {
        snprintf(error, sizeof error, "Impossible d'enregistrer le mot de passe administrateur.");
        return false;
    }

    /* 3. Compte utilisateur (s'il existe déjà : seulement le mot de passe) */
    char home[128];
    snprintf(home, sizeof home, "/home/%s", user);
    if (!getpwnam(user)) {
        char *a1[] = { "/bin/busybox", "adduser", "-D", "-s", "/bin/sh", "-h", home, (char *)user, NULL };
        if (run(a1, NULL) != 0) {
            snprintf(error, sizeof error, "Impossible de créer le compte « %s ».", user);
            return false;
        }
        char *a2[] = { "/bin/busybox", "cp", "-a", "/etc/skel/.", home, NULL };
        run(a2, NULL);
    }
    static char *groups[] = { "video", "audio", "netdev" };
    for (int i = 0; i < 3; i++) {
        char *a[] = { "/bin/busybox", "addgroup", (char *)user, groups[i], NULL };
        run(a, NULL);
    }
    if (set_password(user, fields[F_USER1].text) != 0) {
        snprintf(error, sizeof error, "Impossible d'enregistrer le mot de passe du compte.");
        return false;
    }

    /* 4. Apparence : pour le compte et pour l'écran de connexion (root) */
    struct passwd *pw = getpwnam(user);
    snprintf(nx_conf.accent, sizeof nx_conf.accent, "%s", nx_accents[accent_cur].hex);
    snprintf(nx_conf.wallpaper, sizeof nx_conf.wallpaper, "preset:%s", nx_presets[wall_cur].id);
    if (pw) {
        char own[80];
        snprintf(own, sizeof own, "%s:%s", user, user);
        char *a3[] = { "/bin/busybox", "chown", "-R", own, home, NULL };
        run(a3, NULL);
        chmod(home, 0700);                    /* dossier personnel privé */
        save_look_for(pw->pw_dir, pw->pw_uid, pw->pw_gid);
    }
    save_look_for("/root", 0, 0);

    /* 5. Compte présélectionné sur l'écran de connexion, assistant terminé */
    mkdir("/usr/local/var", 0755);
    mkdir("/usr/local/var/lib", 0755);
    mkdir("/usr/local/var/lib/nexus", 0755);
    if ((f = fopen(LAST_USER, "w"))) { fprintf(f, "%s\n", user); fclose(f); }
    if (access("/data/etc", F_OK) == 0 && (f = fopen("/data/etc/.configured", "w"))) fclose(f);
    wipe_secrets();
    return true;
}

/* ======================= Navigation ======================= */

static void go(int p)
{
    if (p < 0 || p >= P_COUNT) return;
    page = p;
    error[0] = 0;
    int fl[3];
    focus = page_fields(page, fl) ? fl[0] : -1;
    nx_redraw(win);
}

static void next(void)
{
    if (finished) return;
    if (!page_ok()) { nx_redraw(win); return; }
    if (page == P_TOUR) {           /* tout appliquer avant la dernière page */
        if (!apply_all()) { nx_redraw(win); return; }
    }
    if (page == P_DONE) {
        finished = true;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        finish_at = t.tv_sec + t.tv_nsec / 1e9 + 0.3;
        nx_redraw(win);
        return;
    }
    go(page + 1);
}

static void back(void)
{
    if (page > P_WELCOME && page < P_DONE) go(page - 1);
}

/* ======================= Zones cliquables ======================= */

enum kind { K_NEXT, K_BACK, K_FIELD, K_KB, K_ACCENT, K_WALL };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[64];
static int nrects;
static int hover_kind = -1, hover_index = -1;

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 64) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
}
static bool hot(enum kind k, int i) { return (int)k == hover_kind && i == hover_index; }

/* ======================= Dessin ======================= */

static nx_color C_TEXT, C_SUB, C_ACC;

/* Paragraphe : texte découpé en lignes qui tiennent dans w ; renvoie la hauteur */
static double para(cairo_t *cr, const char *txt, double size, nx_color c, double x, double y, double w)
{
    char line[512] = "", word[128];
    double lh = size * nx_scale * 1.45, yy = y;
    const char *p = txt;
    while (*p) {
        if (*p == '\n') {
            nx_text(cr, line, size, false, c, x, yy, w, lh, NX_LEFT);
            line[0] = 0; yy += lh; p++;
            continue;
        }
        size_t n = strcspn(p, " \n");
        snprintf(word, sizeof word, "%.*s", (int)n, p);
        p += n;
        if (*p == ' ') p++;
        char test[640];
        snprintf(test, sizeof test, "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && nx_text_width(cr, test, size, false) > w) {
            nx_text(cr, line, size, false, c, x, yy, w, lh, NX_LEFT);
            yy += lh;
            snprintf(line, sizeof line, "%s", word);
        } else {
            snprintf(line, sizeof line, "%s", test);
        }
    }
    if (line[0]) { nx_text(cr, line, size, false, c, x, yy, w, lh, NX_LEFT); yy += lh; }
    return yy - y;
}

static void button(cairo_t *cr, double x, double y, double w, double h, const char *label, bool primary,
                   enum kind k)
{
    nx_rounded_rect(cr, x, y, w, h, h / 2);
    if (primary) nx_set(cr, hot(k, 0) ? nx_mix(C_ACC, nx_rgb("#ffffff"), 0.18) : C_ACC);
    else cairo_set_source_rgba(cr, 1, 1, 1, hot(k, 0) ? 0.16 : 0.08);
    cairo_fill(cr);
    nx_text(cr, label, 14, true, nx_rgb("#ffffff"), x, y, w, h, NX_CENTER);
    add_rect(x, y, w, h, k, 0);
}

static void field(cairo_t *cr, int id, double x, double y, double w, const char *label)
{
    nx_text(cr, label, 13, true, C_TEXT, x, y, w, 20, NX_LEFT);
    double fy = y + 24, fh = 44;
    nx_rounded_rect(cr, x, fy, w, fh, 10);
    cairo_set_source_rgba(cr, 1, 1, 1, focus == id ? 0.12 : 0.07);
    cairo_fill_preserve(cr);
    if (focus == id) nx_set(cr, C_ACC); else cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
    cairo_set_line_width(cr, focus == id ? 2 : 1);
    cairo_stroke(cr);
    const char *t = fields[id].text;
    double tw = 0;
    if (!t[0]) {
        nx_text(cr, fields[id].placeholder, 14, false, nx_alpha(C_SUB, 0.8), x + 14, fy, w - 28, fh, NX_LEFT);
    } else if (fields[id].secret) {
        int n = 0;
        for (const char *p = t; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) n++;
        for (int i = 0; i < n && i < 30; i++) {
            cairo_new_path(cr);
            cairo_arc(cr, x + 20 + i * 14, fy + fh / 2, 4.2, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill(cr);
        }
        tw = (n < 30 ? n : 30) * 14 - 4;
    } else {
        tw = nx_text(cr, t, 15, false, nx_rgb("#ffffff"), x + 14, fy, w - 28, fh, NX_LEFT);
    }
    if (focus == id) {
        cairo_rectangle(cr, x + 15 + tw + (fields[id].secret && t[0] ? 6 : 0), fy + 12, 1.6, fh - 24);
        nx_set(cr, C_ACC);
        cairo_fill(cr);
    }
    add_rect(x, fy, w, fh, K_FIELD, id);
}

static void title(cairo_t *cr, double x, double y, double w, const char *t, const char *sub, double *out_y)
{
    nx_text(cr, t, 26, true, nx_rgb("#ffffff"), x, y, w, 40, NX_LEFT);
    double h = sub ? para(cr, sub, 14, C_SUB, x, y + 46, w) : 0;
    *out_y = y + 46 + h + 18;
}

/* ---- Contenu de chaque étape ---- */

static void draw_welcome(cairo_t *cr, double x, double y, double w)
{
    nx_logo(cr, x, y, 64);
    double yy;
    title(cr, x, y + 80, w, "Bienvenue dans Nexus OS",
          "Nexus OS est un système d'exploitation construit à partir de zéro : le noyau Linux, "
          "les outils de base (BusyBox) et un bureau écrit pour lui.", &yy);
    yy = yy - 4;
    yy += para(cr, "Cet assistant va t'aider à préparer ton ordinateur en quelques minutes :", 14, C_TEXT, x, yy, w) + 10;
    static const char *items[] = {
        "choisir la disposition de ton clavier ;",
        "donner un nom à cet ordinateur ;",
        "protéger l'administration par un mot de passe ;",
        "créer ton compte personnel ;",
        "choisir les couleurs du bureau ;",
        "et découvrir comment tout fonctionne.",
    };
    for (unsigned i = 0; i < 6; i++) {
        cairo_new_path(cr);
        cairo_arc(cr, x + 8, yy + 10, 4, 0, 2 * M_PI);
        nx_set(cr, C_ACC);
        cairo_fill(cr);
        nx_text(cr, items[i], 14, false, C_TEXT, x + 22, yy, w - 22, 20, NX_LEFT);
        yy += 28;
    }
    para(cr, "Tu pourras tout modifier plus tard dans Paramètres.", 13, C_SUB, x, yy + 10, w);
}

static void draw_keyboard(cairo_t *cr, double x, double y, double w)
{
    double yy;
    title(cr, x, y, w, "Disposition du clavier",
          "Choisis la disposition qui correspond aux touches de ton clavier. Elle s'applique tout de suite, "
          "au bureau comme aux consoles texte.", &yy);
    double gap = 10, cw = (w - 2 * gap) / 3;
    for (int i = 0; i < NKBS; i++) {
        double cx = x + (i % 3) * (cw + gap), cy = yy + (i / 3) * 46;
        bool sel = i == kb_cur;
        nx_rounded_rect(cr, cx, cy, cw, 38, 9);
        if (sel) nx_set(cr, nx_alpha(C_ACC, 0.35));
        else cairo_set_source_rgba(cr, 1, 1, 1, hot(K_KB, i) ? 0.14 : 0.06);
        cairo_fill_preserve(cr);
        if (sel) nx_set(cr, C_ACC); else cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
        cairo_set_line_width(cr, sel ? 2 : 1);
        cairo_stroke(cr);
        nx_text(cr, kbs[i].label, 13, sel, nx_rgb("#ffffff"), cx + 12, cy, cw - 20, 38, NX_LEFT);
        add_rect(cx, cy, cw, 38, K_KB, i);
    }
    yy += ((NKBS + 2) / 3) * 46 + 12;
    nx_text(cr, "Essaie ici :", 13, true, C_TEXT, x, yy, w, 20, NX_LEFT);
    nx_rounded_rect(cr, x, yy + 24, w, 42, 10);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_fill_preserve(cr);
    nx_set(cr, C_ACC);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    char shown[96];
    snprintf(shown, sizeof shown, "%s|", kb_test);
    nx_text(cr, kb_test[0] ? shown : "Tape quelques lettres : a z q w é è ç à…", 14, false,
            kb_test[0] ? nx_rgb("#ffffff") : nx_alpha(C_SUB, 0.8), x + 14, yy + 24, w - 28, 42, NX_LEFT);
}

static void draw_host(cairo_t *cr, double x, double y, double w)
{
    double yy;
    title(cr, x, y, w, "Le nom de cet ordinateur",
          "Il permet de reconnaître la machine : il s'affiche dans le terminal (alice@nexus) et sur le "
          "réseau. Choisis un nom court, sans espace ni accent.", &yy);
    field(cr, F_HOST, x, yy, w * 0.7, "Nom de l'ordinateur");
    para(cr, "Exemples : nexus, pc-salon, portable-de-lea", 13, C_SUB, x, yy + 82, w);
}

static void draw_root(cairo_t *cr, double x, double y, double w)
{
    double yy;
    title(cr, x, y, w, "Le mot de passe administrateur",
          "Le compte administrateur (« root ») peut tout modifier : installer des logiciels, changer les "
          "réglages du système, gérer les comptes. Son mot de passe te sera demandé pour ces actions "
          "(Magasin d'applications, commande su dans le terminal).", &yy);
    field(cr, F_ROOT1, x, yy, w * 0.7, "Mot de passe administrateur");
    field(cr, F_ROOT2, x, yy + 84, w * 0.7, "Confirmation");
    para(cr, "Choisis un mot de passe dont tu te souviendras : il n'y a pas d'autre moyen de le retrouver.",
         13, C_SUB, x, yy + 170, w);
}

static void draw_user(cairo_t *cr, double x, double y, double w)
{
    double yy;
    title(cr, x, y, w, "Ton compte",
          "C'est le compte que tu utiliseras tous les jours. Il a son propre dossier personnel "
          "(Documents, Images…) et ses propres réglages ; il ne peut pas abîmer le système par erreur.", &yy);
    field(cr, F_USER, x, yy, w * 0.7, "Nom du compte");
    field(cr, F_USER1, x, yy + 84, w * 0.7, "Mot de passe");
    field(cr, F_USER2, x, yy + 168, w * 0.7, "Confirmation");
}

static void draw_look(cairo_t *cr, double x, double y, double w)
{
    double yy;
    title(cr, x, y, w, "Apparence", "Choisis une couleur et un fond d'écran. Tu en trouveras d'autres dans Paramètres.", &yy);
    nx_text(cr, "Couleur d'accent", 13, true, C_TEXT, x, yy, w, 20, NX_LEFT);
    for (int i = 0; i < nx_accent_count && i < 12; i++) {
        double cx = x + 20 + i * 46, cy = yy + 46;
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, 16, 0, 2 * M_PI);
        nx_set(cr, nx_rgb(nx_accents[i].hex));
        cairo_fill(cr);
        if (i == accent_cur || hot(K_ACCENT, i)) {
            cairo_new_path(cr);
            cairo_arc(cr, cx, cy, 21, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 1, 1, 1, i == accent_cur ? 0.95 : 0.4);
            cairo_set_line_width(cr, 2.5);
            cairo_stroke(cr);
        }
        add_rect(cx - 21, cy - 21, 42, 42, K_ACCENT, i);
    }
    double wy = yy + 86;
    nx_text(cr, "Fond d'écran", 13, true, C_TEXT, x, wy, w, 20, NX_LEFT);
    double gap = 12, tw = (w - 3 * gap) / 4, th = tw * 0.6;
    for (int i = 0; i < nx_preset_count && i < 8; i++) {
        double tx = x + (i % 4) * (tw + gap), ty = wy + 28 + (i / 4) * (th + 30);
        char spec[64];
        snprintf(spec, sizeof spec, "preset:%s", nx_presets[i].id);
        cairo_save(cr);
        nx_rounded_rect(cr, tx, ty, tw, th, 8);
        cairo_clip(cr);
        cairo_translate(cr, tx, ty);
        nx_draw_wallpaper_cached(cr, spec, tw, th);
        cairo_restore(cr);
        nx_rounded_rect(cr, tx, ty, tw, th, 8);
        if (i == wall_cur) { nx_set(cr, C_ACC); cairo_set_line_width(cr, 3); }
        else { cairo_set_source_rgba(cr, 1, 1, 1, hot(K_WALL, i) ? 0.6 : 0.15); cairo_set_line_width(cr, 1.5); }
        cairo_stroke(cr);
        nx_text(cr, nx_presets[i].name, 12.5, i == wall_cur, C_TEXT, tx, ty + th + 4, tw, 20, NX_CENTER);
        add_rect(tx, ty, tw, th, K_WALL, i);
    }
}

static void draw_tour(cairo_t *cr, double x, double y, double w)
{
    double yy;
    title(cr, x, y, w, "Comment fonctionne Nexus", NULL, &yy);
    yy -= 10;
    static const struct { const char *icon, *t, *d; } cards[] = {
        { "files", "Barre des tâches et menu",
          "Le bouton N (ou la touche Windows) ouvre le menu Démarrer : applications, recherche, éteindre." },
        { "files", "Fichiers",
          "Tes documents et images. Glisse un fichier sur un dossier pour le déplacer ; la Corbeille rattrape les erreurs." },
        { "store", "Magasin d'applications",
          "Installe des logiciels en un clic (mot de passe administrateur). Onglet Alpine : des milliers de plus." },
        { "settings", "Paramètres",
          "Fond d'écran, couleurs, taille de la barre, widgets, clavier, comptes… tout se règle ici." },
        { "browser", "Fenêtres",
          "Glisse une fenêtre dans un coin ou contre un bord pour la ranger ; en haut, elle s'agrandit. Alt+F4 ferme." },
        { "terminal", "Terminal et npk",
          "Pour aller plus loin : npk install, npk search… Les notifications te préviennent des mises à jour." },
    };
    double gap = 12, cw = (w - gap) / 2, ch = 112;
    for (int i = 0; i < 6; i++) {
        double cx = x + (i % 2) * (cw + gap), cy = yy + (i / 2) * (ch + gap);
        nx_rounded_rect(cr, cx, cy, cw, ch, 12);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
        cairo_fill(cr);
        if (i == 0) nx_logo(cr, cx + 14, cy + 14, 34);
        else nx_app_icon(cr, cards[i].icon, cx + 14, cy + 14, 34, nx_rgb("#ffffff"));
        nx_text(cr, cards[i].t, 14, true, nx_rgb("#ffffff"), cx + 60, cy + 14, cw - 72, 34, NX_LEFT);
        para(cr, cards[i].d, 12.5, C_SUB, cx + 14, cy + 54, cw - 28);
    }
}

static void draw_done(cairo_t *cr, double x, double y, double w)
{
    double cx = x + 40, cy = y + 40;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, 34, 0, 2 * M_PI);
    nx_set(cr, C_ACC);
    cairo_fill(cr);
    cairo_move_to(cr, cx - 15, cy + 1); cairo_line_to(cr, cx - 4, cy + 12); cairo_line_to(cr, cx + 16, cy - 11);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    double yy;
    title(cr, x, y + 100, w, "Tout est prêt !",
          "Nexus OS est configuré. Clique sur « Commencer » : l'écran de connexion s'affiche, "
          "ton compte est déjà sélectionné, il ne reste qu'à taper ton mot de passe.", &yy);
    char line[200];
    struct { const char *k; char v[160]; } recap[4];
    snprintf(recap[0].v, sizeof recap[0].v, "%s", fields[F_HOST].text);
    recap[0].k = "Ordinateur";
    snprintf(recap[1].v, sizeof recap[1].v, "%s", fields[F_USER].text);
    recap[1].k = "Ton compte";
    snprintf(recap[2].v, sizeof recap[2].v, "%s", kbs[kb_cur].label);
    recap[2].k = "Clavier";
    snprintf(recap[3].v, sizeof recap[3].v, "%s, fond « %s »", nx_accents[accent_cur].name, nx_presets[wall_cur].name);
    recap[3].k = "Apparence";
    for (int i = 0; i < 4; i++) {
        nx_text(cr, recap[i].k, 13.5, false, C_SUB, x, yy + i * 30, 150, 24, NX_LEFT);
        snprintf(line, sizeof line, "%s", recap[i].v);
        nx_text(cr, line, 13.5, true, nx_rgb("#ffffff"), x + 150, yy + i * 30, w - 150, 24, NX_LEFT);
    }
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    C_TEXT = nx_rgb(NX_GLASS_TEXT);
    C_SUB = nx_rgb(NX_GLASS_SUBTLE);
    C_ACC = nx_accent_color();

    /* Fond : le fond d'écran choisi, assombri (calculé une fois par choix) */
    static cairo_surface_t *bg;
    static int bg_w, bg_h, bg_wall = -1;
    if (!bg || bg_w != width || bg_h != height || bg_wall != wall_cur) {
        if (bg) cairo_surface_destroy(bg);
        bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
        cairo_t *c = cairo_create(bg);
        char spec[64];
        snprintf(spec, sizeof spec, "preset:%s", nx_presets[wall_cur].id);
        nx_draw_wallpaper(c, spec, width, height);
        cairo_set_source_rgba(c, 0.02, 0.03, 0.08, 0.45);
        cairo_paint(c);
        cairo_destroy(c);
        bg_w = width; bg_h = height; bg_wall = wall_cur;
    }
    cairo_set_source_surface(cr, bg, 0, 0);
    cairo_paint(cr);

    double cw = fmin(CARD_W, width - 40), ch = fmin(CARD_H, height - 40);
    double x0 = (width - cw) / 2, y0 = (height - ch) / 2;
    nx_glass(cr, x0, y0, cw, ch, 22);

    /* Colonne des étapes */
    nx_rounded_rect(cr, x0, y0, SIDE_W, ch, 22);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
    cairo_fill(cr);
    nx_logo(cr, x0 + 24, y0 + 26, 30);
    nx_text(cr, "Configuration", 15, true, nx_rgb("#ffffff"), x0 + 64, y0 + 26, SIDE_W - 70, 30, NX_LEFT);
    for (int i = 0; i < P_COUNT; i++) {
        double sy = y0 + 84 + i * 46;
        bool cur = i == page, done = i < page;
        if (cur) {
            nx_rounded_rect(cr, x0 + 12, sy - 4, SIDE_W - 24, 38, 10);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
            cairo_fill(cr);
        }
        cairo_new_path(cr);
        cairo_arc(cr, x0 + 36, sy + 15, 11, 0, 2 * M_PI);
        if (done || cur) nx_set(cr, C_ACC); else cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
        cairo_fill(cr);
        if (done) {
            cairo_move_to(cr, x0 + 31, sy + 15); cairo_line_to(cr, x0 + 35, sy + 19); cairo_line_to(cr, x0 + 42, sy + 11);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
        } else {
            char n[4];
            snprintf(n, sizeof n, "%d", i + 1);
            nx_text(cr, n, 11, true, nx_rgb("#ffffff"), x0 + 25, sy + 4, 22, 22, NX_CENTER);
        }
        nx_text(cr, step_names[i], 13.5, cur, cur ? nx_rgb("#ffffff") : (done ? C_TEXT : C_SUB),
                x0 + 58, sy, SIDE_W - 70, 30, NX_LEFT);
    }

    /* Contenu */
    double cx = x0 + SIDE_W + 40, cy = y0 + 36, cwid = cw - SIDE_W - 80;
    switch (page) {
    case P_WELCOME: draw_welcome(cr, cx, cy, cwid); break;
    case P_KEYBOARD: draw_keyboard(cr, cx, cy, cwid); break;
    case P_HOST: draw_host(cr, cx, cy, cwid); break;
    case P_ROOT: draw_root(cr, cx, cy, cwid); break;
    case P_USER: draw_user(cr, cx, cy, cwid); break;
    case P_LOOK: draw_look(cr, cx, cy, cwid); break;
    case P_TOUR: draw_tour(cr, cx, cy, cwid); break;
    case P_DONE: draw_done(cr, cx, cy, cwid); break;
    }

    /* Bas : erreur, Retour, Suivant */
    double by = y0 + ch - 64;
    if (error[0]) nx_text(cr, error, 13, true, nx_rgb("#ff8a8a"), cx, by - 34, cwid, 24, NX_LEFT);
    const char *nl = page == P_WELCOME ? "Commencer" : page == P_TOUR ? "Appliquer et terminer" :
                     page == P_DONE ? "Commencer" : "Suivant";
    double nw = nx_text_width(cr, nl, 14, true) + 48;
    button(cr, cx + cwid - nw, by, nw, 42, nl, true, K_NEXT);
    if (page > P_WELCOME && page < P_DONE) button(cr, cx + cwid - nw - 12 - 110, by, 110, 42, "Retour", false, K_BACK);
    int fl[3];
    if (page_fields(page, fl) > 1)
        nx_text(cr, "Tab : champ suivant · Entrée : valider", 12, false, nx_alpha(C_SUB, 0.8), cx, by, 260, 42, NX_LEFT);
}

/* ======================= Souris et clavier ======================= */

static struct rect *hit(double x, double y)
{
    for (int i = nrects - 1; i >= 0; i--)
        if (x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h)
            return &rects[i];
    return NULL;
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct rect *r = hit(x, y);
    int k = r ? (int)r->kind : -1, i = r ? r->index : -1;
    if (ev == NX_MOTION || ev == NX_LEAVE) {
        if (ev == NX_LEAVE) k = i = -1;
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
        return;
    }
    if (ev != NX_RELEASE || value != BTN_LEFT || !r || finished) return;
    switch (r->kind) {
    case K_NEXT: next(); break;
    case K_BACK: back(); break;
    case K_FIELD: focus = r->index; break;
    case K_KB: set_keyboard(r->index); break;
    case K_ACCENT:
        accent_cur = r->index;
        snprintf(nx_conf.accent, sizeof nx_conf.accent, "%s", nx_accents[accent_cur].hex);
        break;
    case K_WALL: wall_cur = r->index; break;
    }
    nx_redraw(w);
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (finished) return;
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        int fl[3], n = page_fields(page, fl);
        /* Entrée dans un champ qui n'est pas le dernier : champ suivant */
        for (int i = 0; i + 1 < n; i++)
            if (focus == fl[i] && !fields[fl[i + 1]].text[0]) { focus = fl[i + 1]; nx_redraw(w); return; }
        next();
        return;
    }
    if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
        int fl[3], n = page_fields(page, fl);
        if (n) {
            int cur = 0;
            for (int i = 0; i < n; i++) if (fl[i] == focus) cur = i;
            focus = fl[(cur + (nx_mod_shift() ? n - 1 : 1)) % n];
            nx_redraw(w);
        }
        return;
    }
    if (sym == XKB_KEY_Escape) { back(); return; }
    char *buf = NULL;
    size_t cap = 0;
    if (focus >= 0) { buf = fields[focus].text; cap = sizeof fields[focus].text; }
    else if (page == P_KEYBOARD) { buf = kb_test; cap = sizeof kb_test; }
    if (!buf) return;
    if (sym == XKB_KEY_BackSpace) {
        size_t n = strlen(buf);
        while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        buf[n] = 0;
    } else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f && strlen(buf) + strlen(utf8) < cap - 1) {
        /* nom de compte : minuscules directement */
        if (focus == F_USER && strlen(utf8) == 1) {
            char c = (char)tolower((unsigned char)utf8[0]);
            strncat(buf, &c, 1);
        } else {
            strcat(buf, utf8);
        }
        error[0] = 0;
    } else {
        return;
    }
    nx_redraw(w);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int timeout(void) { return finished ? 100 : -1; }
static void tick(void)
{
    if (finished && now_s() >= finish_at) {
        const char *pid = getenv("LABWC_PID");
        if (pid && atoi(pid) > 1) kill(atoi(pid), SIGTERM);
        nx_quit();
    }
}

int assistant_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (getuid() != 0) {
        fprintf(stderr, "nexus-assistant doit être lancé par nexus-login (root), au premier démarrage.\n");
        return 1;
    }
    load_keyboard();
    /* Valeurs de départ : nom actuel, couleur et fond par défaut */
    char h[64] = "";
    if (gethostname(h, sizeof h) == 0 && h[0] && strcmp(h, "(none)")) snprintf(fields[F_HOST].text, sizeof fields[F_HOST].text, "%s", h);
    for (int i = 0; i < nx_accent_count; i++) if (!strcasecmp(nx_accents[i].hex, nx_conf.accent)) accent_cur = i;
    for (int i = 0; i < nx_preset_count; i++) {
        char spec[64];
        snprintf(spec, sizeof spec, "preset:%s", nx_presets[i].id);
        if (!strcmp(spec, nx_conf.wallpaper)) wall_cur = i;
    }
    if (!nx_init()) return 1;
    struct nx_handlers hd = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_layer_window(hd, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          0, 0, -1, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, "nexus-assistant");
    if (!win) return 1;
    nx.timeout = timeout;
    nx.tick = tick;
    nx_run();
    wipe_secrets();
    return 0;
}
