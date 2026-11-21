/* nexus-store — le Magasin d'applications de Nexus OS.
 *
 * Liste les paquets du dépôt npk (index signé, déjà vérifié par npk) avec
 * leur logo, leur description et un bouton : Installer, Supprimer ou Mettre
 * à jour. Les opérations passent par nexus-pkg (setuid root) qui demande le
 * mot de passe administrateur, puis lance npk ; sa sortie s'affiche en bas.
 *   onglets : Toutes / Installées / Mises à jour / Alpine ; recherche au clavier.
 * L'onglet Alpine cherche parmi les ~30 000 logiciels d'Alpine Linux
 * (paquet « alpine » : npk alpine ...).
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define WIN_W   920
#define WIN_H   660
#define HEAD_H  132
#define FOOT_H  46
#define ROW_H   78

#define NPK_DB    "/usr/local/var/lib/npk"
#define HELPER    "/usr/local/libexec/nexus-pkg"

/* ---------- Paquets ---------- */

struct pkg {
    char name[64], ver[48], desc[256], inst[48];
    bool installed, in_repo, system;
};
static struct pkg pkgs[256];
static int npkgs;
static long index_age = -1;        /* âge de l'index en jours (-1 : absent) */

/* Paquets indispensables au bureau : pas de bouton « Supprimer » */
static const char *system_pkgs[] = { "nexus-desktop", "labwc", "foot", "seatd", "xkeyboard-config",
                                     "fonts-dejavu", "alsa", "ca-certificates" };

/* Noms plus parlants pour quelques paquets */
static const struct { const char *pkg, *title; } titles[] = {
    { "netsurf", "Navigateur web" },         { "htop", "Moniteur système" },
    { "nano", "Éditeur de texte" },          { "foot", "Terminal" },
    { "tmux", "Multiplexeur de terminal" },  { "jq", "Outil JSON" },
    { "lua", "Langage Lua" },                { "dropbear", "Serveur SSH" },
    { "curl", "Téléchargements (curl)" },    { "wifi", "Wi-Fi" },
    { "linux-wifi", "Pilotes Wi-Fi" },       { "alsa", "Son" },
    { "nexusfetch", "Infos système (nexusfetch)" }, { "nexus-desktop", "Bureau Nexus" },
    { "labwc", "Gestionnaire de fenêtres" }, { "fonts-dejavu", "Polices DejaVu" },
};

static const char *title_of(const struct pkg *p)
{
    for (unsigned i = 0; i < sizeof titles / sizeof titles[0]; i++)
        if (!strcmp(titles[i].pkg, p->name)) return titles[i].title;
    return p->name;
}

static struct pkg *find_pkg(const char *name)
{
    for (int i = 0; i < npkgs; i++) if (!strcmp(pkgs[i].name, name)) return &pkgs[i];
    return NULL;
}

static int cmp_pkg(const void *a, const void *b)
{
    return strcasecmp(title_of(a), title_of(b));
}

static void load_pkgs(void)
{
    npkgs = 0;
    index_age = -1;
    FILE *f = fopen(NPK_DB "/INDEX", "r");
    if (f) {
        struct stat st;
        if (fstat(fileno(f), &st) == 0) index_age = (time(NULL) - st.st_mtime) / 86400;
        char line[1024];
        while (fgets(line, sizeof line, f) && npkgs < 256) {
            line[strcspn(line, "\n")] = 0;
            char *fld[6] = { 0 }, *s = line;
            for (int i = 0; i < 6 && s; i++) {
                fld[i] = s;
                s = strchr(s, '|');
                if (s) *s++ = 0;
            }
            if (!fld[5] || !fld[0][0]) continue;
            struct pkg *p = &pkgs[npkgs++];
            memset(p, 0, sizeof *p);
            snprintf(p->name, sizeof p->name, "%s", fld[0]);
            snprintf(p->ver, sizeof p->ver, "%s", fld[1]);
            snprintf(p->desc, sizeof p->desc, "%s", fld[5]);
            p->in_repo = true;
        }
        fclose(f);
    }
    DIR *d = opendir(NPK_DB "/installed");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char path[512], line[512], ver[48] = "?";
            snprintf(path, sizeof path, NPK_DB "/installed/%s/PKGINFO", e->d_name);
            FILE *pf = fopen(path, "r");
            if (!pf) continue;
            char desc[256] = "";
            while (fgets(line, sizeof line, pf)) {
                line[strcspn(line, "\n")] = 0;
                if (!strncmp(line, "VERSION=", 8)) snprintf(ver, sizeof ver, "%s", line + 8);
                else if (!strncmp(line, "DESC=", 5)) snprintf(desc, sizeof desc, "%s", line + 5);
            }
            fclose(pf);
            struct pkg *p = find_pkg(e->d_name);
            if (!p && npkgs < 256) {
                p = &pkgs[npkgs++];
                memset(p, 0, sizeof *p);
                snprintf(p->name, sizeof p->name, "%s", e->d_name);
                snprintf(p->ver, sizeof p->ver, "%s", ver);
                snprintf(p->desc, sizeof p->desc, "%s", desc[0] == '"' ? desc + 1 : desc);
                p->desc[strcspn(p->desc, "\"")] = 0;
            }
            if (!p) continue;
            p->installed = true;
            snprintf(p->inst, sizeof p->inst, "%s", ver);
        }
        closedir(d);
    }
    for (int i = 0; i < npkgs; i++)
        for (unsigned k = 0; k < sizeof system_pkgs / sizeof system_pkgs[0]; k++)
            if (!strcmp(pkgs[i].name, system_pkgs[k])) pkgs[i].system = true;
    qsort(pkgs, npkgs, sizeof pkgs[0], cmp_pkg);
}

/* ---------- Paquets Alpine ---------- */

#define ALP_DB "/usr/local/var/lib/npk/alpine"
struct apkg { char *name, *ver, *desc; };
static struct apkg *apkgs;
static int napkgs, capkgs;
static bool alpine_tool;          /* paquet « alpine » installé ? */
static int amatch[80], namatch;

static void load_alpine(void)
{
    for (int i = 0; i < napkgs; i++) { free(apkgs[i].name); free(apkgs[i].ver); free(apkgs[i].desc); }
    napkgs = 0;
    alpine_tool = access("/usr/local/bin/npk-alpine", X_OK) == 0;
    static const char *repos[] = { "main", "community" };
    for (int r = 0; r < 2; r++) {
        char path[128];
        snprintf(path, sizeof path, ALP_DB "/%s.idx", repos[r]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[2048], name[128] = "", ver[64] = "", desc[256] = "";
        for (;;) {
            bool eof = !fgets(line, sizeof line, f);
            if (eof || line[0] == '\n') {
                if (name[0]) {
                    if (napkgs == capkgs) {
                        capkgs = capkgs ? capkgs * 2 : 4096;
                        apkgs = realloc(apkgs, capkgs * sizeof *apkgs);
                    }
                    apkgs[napkgs++] = (struct apkg){ strdup(name), strdup(ver), strdup(desc) };
                }
                name[0] = ver[0] = desc[0] = 0;
                if (eof) break;
                continue;
            }
            line[strcspn(line, "\n")] = 0;
            if (!strncmp(line, "P:", 2)) snprintf(name, sizeof name, "%s", line + 2);
            else if (!strncmp(line, "V:", 2)) snprintf(ver, sizeof ver, "%s", line + 2);
            else if (!strncmp(line, "T:", 2)) snprintf(desc, sizeof desc, "%s", line + 2);
        }
        fclose(f);
    }
}

/* Version installée d'un paquet Alpine (ou "" s'il n'est pas installé) */
static void alpine_installed(const char *name, char *ver, size_t n)
{
    char path[256];
    ver[0] = 0;
    snprintf(path, sizeof path, ALP_DB "/installed/%s", name);
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(ver, n, f)) ver[strcspn(ver, "\n")] = 0;
    fclose(f);
}

static void search_alpine(const char *q)
{
    namatch = 0;
    if (strlen(q) < 2) return;
    /* d'abord les noms qui commencent par la recherche, puis les autres */
    for (int pass = 0; pass < 3 && namatch < 80; pass++)
        for (int i = 0; i < napkgs && namatch < 80; i++) {
            const char *nm = apkgs[i].name;
            bool hit = pass == 0 ? !strcasecmp(nm, q) :
                       pass == 1 ? strncasecmp(nm, q, strlen(q)) == 0 && strcasecmp(nm, q) :
                       (strcasestr(nm, q) && strncasecmp(nm, q, strlen(q))) ||
                       (!strcasestr(nm, q) && strcasestr(apkgs[i].desc, q));
            if (hit) amatch[namatch++] = i;
        }
}

static bool has_update(const struct pkg *p)
{
    return p->installed && p->in_repo && strcmp(p->inst, p->ver);
}

/* ---------- État de l'interface ---------- */

enum { TAB_ALL, TAB_INSTALLED, TAB_UPDATES, TAB_ALPINE, NTABS };
static int tab = TAB_ALL;
static char search[64];
static double scroll;
static int visible[256], nvisible;

enum kind { K_TAB, K_ACTION, K_REFRESH, K_UPGRADE_ALL, K_DLG_OK, K_DLG_CANCEL, K_AACTION, K_ASETUP };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[128];
static int nrects;
static int hover_kind = -1, hover_index = -1;

static struct nx_window *win;

/* Opération en cours */
static pid_t child;
static int child_out = -1;
static char out_buf[4096];
static size_t out_len;
static char status[256], status_detail[256];
static bool status_error;
static char op_label[128];
static char op_args[4][64];
static int op_nargs;

/* Mot de passe administrateur (gardé en mémoire tant que la fenêtre est ouverte) */
static char password[128];
static bool password_ok;
static bool dialog;          /* fenêtre « mot de passe » affichée */
static char dlg_input[128];
static char dlg_error[128];

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 128) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
}
static bool is_hover(enum kind k, int i) { return (int)k == hover_kind && i == hover_index; }

static bool matches(const struct pkg *p)
{
    if (tab == TAB_INSTALLED && !p->installed) return false;
    if (tab == TAB_UPDATES && !has_update(p)) return false;
    if (search[0] && !strcasestr(p->name, search) && !strcasestr(title_of(p), search) &&
        !strcasestr(p->desc, search))
        return false;
    return true;
}

static int count_updates(void)
{
    int n = 0;
    for (int i = 0; i < npkgs; i++) n += has_update(&pkgs[i]);
    return n;
}

/* ---------- Lancement des opérations ---------- */

static void start_op(void);

static void set_status(const char *msg, const char *detail, bool error)
{
    snprintf(status, sizeof status, "%s", msg);
    snprintf(status_detail, sizeof status_detail, "%s", detail ? detail : "");
    status_error = error;
}

/* Demande une opération ; le mot de passe est demandé si nécessaire */
static void request_op(const char *label, const char *a0, const char *a1)
{
    if (child) return;
    snprintf(op_label, sizeof op_label, "%s", label);
    op_nargs = 0;
    snprintf(op_args[op_nargs++], sizeof op_args[0], "%s", a0);
    if (a1) snprintf(op_args[op_nargs++], sizeof op_args[0], "%s", a1);
    if (getuid() == 0 || password_ok) { start_op(); return; }
    dialog = true;
    dlg_input[0] = dlg_error[0] = 0;
    nx_redraw(win);
}

static void start_op(void)
{
    int in[2], out[2];
    if (pipe(in) || pipe(out)) { set_status("Impossible de lancer l'opération.", strerror(errno), true); return; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        char *args[6] = { "nexus-pkg" };
        int n = 1;
        for (int i = 0; i < op_nargs; i++) args[n++] = op_args[i];
        args[n] = NULL;
        execv(HELPER, args);
        fprintf(stderr, "nexus-pkg introuvable (%s)\n", HELPER);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]); close(out[0]);
        set_status("Impossible de lancer l'opération.", strerror(errno), true);
        return;
    }
    /* mot de passe sur l'entrée standard du programme d'aide */
    if (getuid() != 0) {
        size_t l = strlen(password);
        if (write(in[1], password, l) != (ssize_t)l || write(in[1], "\n", 1) != 1) { /* l'aide refusera */ }
    }
    close(in[1]);
    child = pid;
    child_out = out[0];
    fcntl(child_out, F_SETFL, O_NONBLOCK);
    out_len = 0;
    set_status(op_label, "", false);
    nx_redraw(win);
}

/* Dernière ligne utile de la sortie de npk, sans les codes de couleur */
static void last_line(char *dst, size_t n)
{
    char clean[sizeof out_buf];
    size_t k = 0;
    for (size_t i = 0; i < out_len; i++) {
        if (out_buf[i] == '\033') {
            while (i < out_len && out_buf[i] != 'm') i++;
            continue;
        }
        clean[k++] = out_buf[i];
    }
    clean[k] = 0;
    while (k > 0 && (clean[k - 1] == '\n' || clean[k - 1] == ' ')) clean[--k] = 0;
    char *l = strrchr(clean, '\n');
    l = l ? l + 1 : clean;
    if (!strncmp(l, ":: ", 3)) l += 3;
    if (!strncmp(l, "npk : ", 6)) l += 6;
    snprintf(dst, n, "%s", l);
}

/* Bulle de notification (nexus-notifyd) : utile si le Magasin est caché */
static void notify(const char *title, const char *body)
{
    char qt[300], qb[600], cmd[1000];
    nx_shell_quote(qt, sizeof qt, title);
    nx_shell_quote(qb, sizeof qb, body);
    snprintf(cmd, sizeof cmd, "nexus-notify -i store -a nexus-store %s %s", qt, qb);
    nx_spawn(cmd);
}

static void finish_op(int st)
{
    close(child_out);
    child_out = -1;
    child = 0;
    char line[256];
    last_line(line, sizeof line);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (code == 3) {
        password_ok = false;
        password[0] = 0;
        dialog = true;
        dlg_input[0] = 0;
        snprintf(dlg_error, sizeof dlg_error, "Mot de passe incorrect, réessaie.");
        set_status("", "", false);
    } else if (code == 0) {
        password_ok = true;
        if (!strcmp(op_args[0], "alpine-install")) set_status("Installation terminée", "Lance le logiciel depuis un terminal ou le menu Démarrer.", false);
        else if (!strcmp(op_args[0], "alpine-remove")) set_status("Suppression terminée", line, false);
        else if (!strcmp(op_args[0], "alpine-update")) set_status("Liste Alpine téléchargée", line, false);
        else if (!strcmp(op_args[0], "install")) set_status("Installation terminée", "L'application est dans le menu Démarrer.", false);
        else if (!strcmp(op_args[0], "remove")) set_status("Suppression terminée", line, false);
        else if (!strcmp(op_args[0], "update")) set_status("Liste des applications à jour", line, false);
        else set_status("Mises à jour installées", line, false);
        nx_notify_reload();   /* la barre des tâches revoit ses applications épinglées */
        char body[200];
        if (!strcmp(op_args[0], "install") || !strcmp(op_args[0], "alpine-install")) {
            snprintf(body, sizeof body, "« %s » est installé.", op_nargs > 1 ? op_args[1] : "");
            notify("Installation terminée", body);
        } else if (!strcmp(op_args[0], "upgrade") || !strcmp(op_args[0], "alpine-upgrade")) {
            notify("Mises à jour installées", "Ton système est à jour.");
        }
    } else {
        set_status("L'opération a échoué", line, true);
        notify("L'opération a échoué", line);
    }
    load_pkgs();
    load_alpine();
    search_alpine(search);
    nx_redraw(win);
}

static int op_timeout(void) { return child ? 150 : -1; }

static void op_tick(void)
{
    if (!child) return;
    char buf[1024];
    ssize_t r;
    while ((r = read(child_out, buf, sizeof buf)) > 0) {
        if (out_len + r >= sizeof out_buf) {     /* on garde la fin */
            size_t drop = out_len + r - sizeof out_buf + 1;
            if (drop > out_len) drop = out_len;
            memmove(out_buf, out_buf + drop, out_len - drop);
            out_len -= drop;
        }
        memcpy(out_buf + out_len, buf, r);
        out_len += r;
    }
    int st;
    if (waitpid(child, &st, WNOHANG) == child) {
        while ((r = read(child_out, buf, sizeof buf)) > 0 && out_len + r < sizeof out_buf) {
            memcpy(out_buf + out_len, buf, r);
            out_len += r;
        }
        finish_op(st);
        return;
    }
    char line[256];
    last_line(line, sizeof line);
    snprintf(status_detail, sizeof status_detail, "%s", line);
    nx_redraw(win);
}

/* ---------- Dessin ---------- */

static void button(cairo_t *cr, double x, double y, double w, double h, const char *label,
                   nx_color bg, nx_color fg, bool outline, enum kind k, int i, bool enabled)
{
    bool hv = enabled && is_hover(k, i);
    nx_rounded_rect(cr, x, y, w, h, h / 2);
    if (outline) {
        nx_set(cr, hv ? nx_alpha(bg, 0.10) : nx_rgb("#ffffff"));
        cairo_fill_preserve(cr);
        nx_set(cr, enabled ? bg : nx_rgb("#c5cbd6"));
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        nx_text(cr, label, 13, true, enabled ? bg : nx_rgb("#9aa3b8"), x, y, w, h, NX_CENTER);
    } else {
        nx_set(cr, !enabled ? nx_rgb("#c5cbd6") : hv ? nx_mix(bg, nx_rgb("#000000"), 0.12) : bg);
        cairo_fill(cr);
        nx_text(cr, label, 13, true, fg, x, y, w, h, NX_CENTER);
    }
    if (enabled) add_rect(x, y, w, h, k, i);
}

static void draw_dialog(cairo_t *cr, int width, int height)
{
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_set_source_rgba(cr, 0.05, 0.07, 0.12, 0.45);
    cairo_fill(cr);
    double w = 440, h = 236, x = (width - w) / 2, y = (height - h) / 2 - 20;
    nx_rounded_rect(cr, x, y, w, h, 16);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    nx_icon(cr, "lock", x + 24, y + 24, 28, nx_accent_color());
    nx_text(cr, "Mot de passe administrateur", 17, true, nx_rgb(NX_TEXT), x + 64, y + 22, w - 80, 32, NX_LEFT);
    nx_text(cr, op_label, 12.5, false, nx_rgb(NX_SUBTLE), x + 64, y + 52, w - 80, 20, NX_LEFT);
    nx_text(cr, "Saisis le mot de passe de root pour autoriser l'opération.", 12.5, false,
            nx_rgb("#3a4152"), x + 24, y + 82, w - 48, 20, NX_LEFT);
    /* champ masqué */
    double fx = x + 24, fy = y + 110, fw = w - 48;
    nx_rounded_rect(cr, fx, fy, fw, 40, 8);
    nx_set(cr, nx_rgb("#f4f6fa"));
    cairo_fill_preserve(cr);
    nx_set(cr, nx_accent_color());
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    char dots[3 * 128] = "";
    size_t nch = 0;
    for (const char *p = dlg_input; *p; p++) if ((*p & 0xC0) != 0x80) nch++;
    for (size_t i = 0; i < nch && i < 60; i++) strcat(dots, "●");
    double tw = nx_text(cr, dots, 13, false, nx_rgb(NX_TEXT), fx + 14, fy, fw - 28, 40, NX_LEFT);
    cairo_rectangle(cr, fx + 15 + tw, fy + 10, 1.5, 20);
    nx_set(cr, nx_rgb(NX_TEXT));
    cairo_fill(cr);
    if (dlg_error[0])
        nx_text(cr, dlg_error, 12.5, true, nx_rgb("#d83b3b"), x + 24, y + 154, w - 48, 20, NX_LEFT);
    button(cr, x + w - 24 - 110, y + h - 54, 110, 36, "Valider", nx_accent_color(), nx_rgb("#ffffff"),
           false, K_DLG_OK, 0, true);
    button(cr, x + w - 24 - 230, y + h - 54, 110, 36, "Annuler", nx_rgb("#5a6170"), nx_rgb("#ffffff"),
           true, K_DLG_CANCEL, 0, true);
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    nx_color accent = nx_accent_color(), text = nx_rgb(NX_TEXT), subtle = nx_rgb(NX_SUBTLE);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    /* En-tête */
    cairo_rectangle(cr, 0, 0, width, HEAD_H);
    nx_set(cr, nx_rgb("#eef1f6"));
    cairo_fill(cr);
    nx_app_icon(cr, "store", 26, 22, 48, text);
    nx_text(cr, "Magasin d'applications", 21, true, text, 88, 20, 0, 30, NX_LEFT);
    char sub[128];
    int ninst = 0;
    for (int i = 0; i < npkgs; i++) ninst += pkgs[i].installed;
    if (index_age < 0)
        snprintf(sub, sizeof sub, "Liste des applications absente : clique sur « Actualiser »");
    else
        snprintf(sub, sizeof sub, "%d applications · %d installées", npkgs, ninst);
    nx_text(cr, sub, 13, false, subtle, 88, 50, 0, 22, NX_LEFT);

    /* Recherche */
    double sx = width - 340, sy = 26, sw = 220;
    nx_rounded_rect(cr, sx, sy, sw, 38, 19);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    nx_set(cr, search[0] ? accent : nx_rgb("#d3d8e2"));
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    nx_icon(cr, "search", sx + 12, sy + 10, 18, subtle);
    if (search[0]) {
        double tw = nx_text(cr, search, 13.5, false, text, sx + 38, sy, sw - 50, 38, NX_LEFT);
        cairo_rectangle(cr, sx + 39 + tw, sy + 10, 1.5, 18);
        nx_set(cr, text);
        cairo_fill(cr);
    } else {
        nx_text(cr, "Rechercher (tape au clavier)", 13, false, subtle, sx + 38, sy, sw - 50, 38, NX_LEFT);
    }
    button(cr, width - 110, sy, 86, 38, "Actualiser", accent, nx_rgb("#ffffff"), true, K_REFRESH, 0, !child);

    /* Onglets */
    char upd[48];
    int nupd = count_updates();
    snprintf(upd, sizeof upd, nupd ? "Mises à jour (%d)" : "Mises à jour", nupd);
    const char *tabs[NTABS] = { "Toutes", "Installées", upd, "Alpine Linux" };
    double tx = 26;
    for (int i = 0; i < NTABS; i++) {
        double tw = nx_text_width(cr, tabs[i], 14, i == tab) + 28;
        if (i == tab || is_hover(K_TAB, i)) {
            nx_rounded_rect(cr, tx, 88, tw, 34, 17);
            nx_set(cr, i == tab ? accent : nx_rgb("#dde2ea"));
            cairo_fill(cr);
        }
        nx_text(cr, tabs[i], 14, i == tab, i == tab ? nx_rgb("#ffffff") : text, tx, 88, tw, 34, NX_CENTER);
        add_rect(tx, 88, tw, 34, K_TAB, i);
        tx += tw + 8;
    }
    if (tab == TAB_UPDATES && nupd)
        button(cr, width - 210, 88, 186, 34, "Tout mettre à jour", accent, nx_rgb("#ffffff"), false,
               K_UPGRADE_ALL, 0, !child);

    /* Onglet Alpine */
    if (tab == TAB_ALPINE) {
        double top = HEAD_H + 8, bottom = height - FOOT_H;
        cairo_save(cr);
        cairo_rectangle(cr, 0, top, width, bottom - top);
        cairo_clip(cr);
        if (!alpine_tool || !napkgs) {
            nx_app_icon(cr, "store", width / 2.0 - 32, top + 40, 64, text);
            nx_text(cr, "Les logiciels d'Alpine Linux", 17, true, text, 0, top + 116, width, 28, NX_CENTER);
            nx_text(cr, "Environ 30 000 logiciels : Firefox, GIMP, VLC, LibreOffice, Python…", 13.5, false,
                    subtle, 0, top + 146, width, 22, NX_CENTER);
            nx_text(cr, "Ils sont vérifiés avec les signatures d'Alpine avant installation.", 13, false,
                    subtle, 0, top + 170, width, 22, NX_CENTER);
            button(cr, width / 2.0 - 140, top + 210, 280, 40,
                   !alpine_tool ? "Activer les logiciels Alpine" : "Télécharger la liste Alpine",
                   accent, nx_rgb("#ffffff"), false, K_ASETUP, 0, !child);
        } else if (strlen(search) < 2) {
            char m[96];
            snprintf(m, sizeof m, "%d logiciels Alpine disponibles.", napkgs);
            nx_text(cr, m, 15, true, text, 0, top + 60, width, 26, NX_CENTER);
            nx_text(cr, "Tape le nom d'un logiciel au clavier (ex. : firefox, gimp, vlc, python3).",
                    13.5, false, subtle, 0, top + 90, width, 22, NX_CENTER);
        } else if (!namatch) {
            nx_text(cr, "Aucun logiciel Alpine ne correspond à la recherche.", 14, false, subtle,
                    0, top + 60, width, 26, NX_CENTER);
        }
        double maxs = namatch * ROW_H - (bottom - top);
        if (scroll > maxs) scroll = maxs > 0 ? maxs : 0;
        for (int k = 0; alpine_tool && napkgs && k < namatch; k++) {
            double y = top + k * ROW_H - scroll;
            if (y + ROW_H < top || y > bottom) continue;
            struct apkg *a = &apkgs[amatch[k]];
            char inst[64];
            alpine_installed(a->name, inst, sizeof inst);
            if (k) { cairo_rectangle(cr, 26, y, width - 52, 1); nx_set(cr, nx_rgb("#eceff4")); cairo_fill(cr); }
            nx_app_icon(cr, nx_app_icon_for(a->name), 30, y + 15, 48, text);
            nx_text(cr, a->name, 15, true, text, 96, y + 12, width - 340, 24, NX_LEFT);
            nx_text(cr, a->desc, 12.5, false, nx_rgb("#3a4152"), 96, y + 36, width - 340, 20, NX_LEFT);
            char meta[160];
            snprintf(meta, sizeof meta, "Alpine · version %s%s", inst[0] ? inst : a->ver, inst[0] ? " · installé" : "");
            nx_text(cr, meta, 11.5, false, subtle, 96, y + 55, width - 340, 18, NX_LEFT);
            double bx = width - 26 - 140, by = y + 21;
            if (inst[0] && strcmp(inst, a->ver))
                button(cr, bx, by, 140, 36, "Mettre à jour", accent, nx_rgb("#ffffff"), false, K_AACTION, amatch[k], !child);
            else if (inst[0])
                button(cr, bx, by, 140, 36, "Supprimer", nx_rgb("#d83b3b"), nx_rgb("#ffffff"), true, K_AACTION,
                       amatch[k], !child);
            else
                button(cr, bx, by, 140, 36, "Installer", accent, nx_rgb("#ffffff"), false, K_AACTION, amatch[k], !child);
        }
        cairo_restore(cr);
        goto footer;
    }

    /* Liste */
    nvisible = 0;
    for (int i = 0; i < npkgs; i++) if (matches(&pkgs[i])) visible[nvisible++] = i;
    double top = HEAD_H + 8, bottom = height - FOOT_H;
    double maxscroll = nvisible * ROW_H - (bottom - top);
    if (maxscroll < 0) maxscroll = 0;
    if (scroll > maxscroll) scroll = maxscroll;
    if (scroll < 0) scroll = 0;
    cairo_save(cr);
    cairo_rectangle(cr, 0, top, width, bottom - top);
    cairo_clip(cr);
    if (!nvisible) {
        const char *msg = tab == TAB_UPDATES ? "Tout est à jour." :
                          tab == TAB_INSTALLED ? "Aucune application installée ne correspond." :
                          index_age < 0 ? "Clique sur « Actualiser » pour télécharger la liste des applications." :
                          "Aucune application ne correspond à la recherche.";
        nx_text(cr, msg, 14, false, subtle, 0, top + 40, width, 30, NX_CENTER);
    }
    for (int k = 0; k < nvisible; k++) {
        double y = top + k * ROW_H - scroll;
        if (y + ROW_H < top || y > bottom) continue;
        struct pkg *p = &pkgs[visible[k]];
        if (k) {
            cairo_rectangle(cr, 26, y, width - 52, 1);
            nx_set(cr, nx_rgb("#eceff4"));
            cairo_fill(cr);
        }
        const char *logo = nx_app_icon_for(p->name);
        nx_app_icon(cr, logo, 30, y + 15, 48, text);
        nx_text(cr, title_of(p), 15, true, text, 96, y + 12, width - 340, 24, NX_LEFT);
        nx_text(cr, p->desc, 12.5, false, nx_rgb("#3a4152"), 96, y + 36, width - 340, 20, NX_LEFT);
        char meta[160];
        if (has_update(p)) snprintf(meta, sizeof meta, "%s · version %s → %s", p->name, p->inst, p->ver);
        else snprintf(meta, sizeof meta, "%s · version %s%s", p->name, p->installed ? p->inst : p->ver,
                      p->installed ? " · installée" : "");
        nx_text(cr, meta, 11.5, false, subtle, 96, y + 55, width - 340, 18, NX_LEFT);
        double bx = width - 26 - 140, by = y + 21;
        if (has_update(p))
            button(cr, bx, by, 140, 36, "Mettre à jour", accent, nx_rgb("#ffffff"), false, K_ACTION, visible[k], !child);
        else if (!p->installed)
            button(cr, bx, by, 140, 36, "Installer", accent, nx_rgb("#ffffff"), false, K_ACTION, visible[k],
                   !child && p->in_repo);
        else if (p->system)
            nx_text(cr, "Système", 13, true, subtle, bx, by, 140, 36, NX_CENTER);
        else
            button(cr, bx, by, 140, 36, "Supprimer", nx_rgb("#d83b3b"), nx_rgb("#ffffff"), true, K_ACTION,
                   visible[k], !child);
    }
    cairo_restore(cr);

footer:
    /* Barre d'état */
    cairo_rectangle(cr, 0, height - FOOT_H, width, FOOT_H);
    nx_set(cr, nx_rgb("#f4f6fa"));
    cairo_fill(cr);
    cairo_rectangle(cr, 0, height - FOOT_H, width, 1);
    nx_set(cr, nx_rgb("#e3e7ef"));
    cairo_fill(cr);
    if (child) {
        /* petite animation : trois points */
        static int phase;
        phase = (phase + 1) % 3;
        for (int i = 0; i < 3; i++) {
            cairo_arc(cr, 34 + i * 12, height - FOOT_H / 2.0, 4, 0, 6.3);
            nx_set(cr, nx_alpha(accent, i == phase ? 1 : 0.3));
            cairo_fill(cr);
        }
    } else if (status[0]) {
        cairo_arc(cr, 40, height - FOOT_H / 2.0, 6, 0, 6.3);
        nx_set(cr, status_error ? nx_rgb("#d83b3b") : nx_rgb("#2f9e5a"));
        cairo_fill(cr);
    }
    if (status[0]) {
        double tw = nx_text(cr, status, 13, true, status_error ? nx_rgb("#d83b3b") : text,
                            70, height - FOOT_H, 0, FOOT_H, NX_LEFT);
        nx_text(cr, status_detail, 12.5, false, subtle, 82 + tw, height - FOOT_H,
                width - 110 - tw, FOOT_H, NX_LEFT);
    } else {
        nx_text(cr, "Les applications viennent du dépôt Nexus, signé et vérifié.", 12.5, false, subtle,
                26, height - FOOT_H, 0, FOOT_H, NX_LEFT);
    }

    if (dialog) {
        nrects = 0;      /* seule la boîte de dialogue reste cliquable */
        draw_dialog(cr, width, height);
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

static void dialog_submit(void)
{
    if (!dlg_input[0]) { snprintf(dlg_error, sizeof dlg_error, "Saisis le mot de passe."); return; }
    snprintf(password, sizeof password, "%s", dlg_input);
    explicit_bzero(dlg_input, sizeof dlg_input);
    dialog = false;
    start_op();
}

static void dialog_cancel(void)
{
    dialog = false;
    explicit_bzero(dlg_input, sizeof dlg_input);
    set_status("Opération annulée", "", false);
}

static void click(struct rect *r)
{
    char label[160];
    switch (r->kind) {
    case K_TAB: tab = r->index; scroll = 0; break;
    case K_REFRESH:
        if (tab == TAB_ALPINE && alpine_tool) request_op("Téléchargement de la liste Alpine…", "alpine-update", NULL);
        else request_op("Mise à jour de la liste des applications…", "update", NULL);
        break;
    case K_ASETUP:
        if (!alpine_tool) request_op("Installation de la prise en charge d'Alpine…", "install", "alpine");
        else request_op("Téléchargement de la liste Alpine (1 à 2 min)…", "alpine-update", NULL);
        break;
    case K_AACTION: {
        struct apkg *a = &apkgs[r->index];
        char inst[64];
        alpine_installed(a->name, inst, sizeof inst);
        if (inst[0] && !strcmp(inst, a->ver)) {
            snprintf(label, sizeof label, "Suppression de « %s »…", a->name);
            request_op(label, "alpine-remove", a->name);
        } else {
            snprintf(label, sizeof label, "Installation de « %s » et de ce qu'il lui faut…", a->name);
            request_op(label, "alpine-install", a->name);
        }
        break;
    }
    case K_UPGRADE_ALL: request_op("Installation des mises à jour…", "upgrade", NULL); break;
    case K_DLG_OK: dialog_submit(); break;
    case K_DLG_CANCEL: dialog_cancel(); break;
    case K_ACTION: {
        struct pkg *p = &pkgs[r->index];
        if (has_update(p)) {
            snprintf(label, sizeof label, "Mise à jour de « %s »…", title_of(p));
            request_op(label, "install", p->name);
        } else if (!p->installed) {
            snprintf(label, sizeof label, "Installation de « %s »…", title_of(p));
            request_op(label, "install", p->name);
        } else {
            snprintf(label, sizeof label, "Suppression de « %s »…", title_of(p));
            request_op(label, "remove", p->name);
        }
        break;
    }
    }
    nx_redraw(win);
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct rect *r = hit(x, y);
    switch (ev) {
    case NX_MOTION: {
        int k = r ? (int)r->kind : -1, i = r ? r->index : -1;
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
        break;
    }
    case NX_LEAVE: hover_kind = hover_index = -1; nx_redraw(w); break;
    case NX_RELEASE: if (r && value == BTN_LEFT) click(r); break;
    case NX_SCROLL: if (!dialog) { scroll += value * 48; nx_redraw(w); } break;
    default: break;
    }
}

static void text_edit(char *buf, size_t n, xkb_keysym_t sym, const char *utf8)
{
    size_t l = strlen(buf);
    if (sym == XKB_KEY_BackSpace) {
        while (l > 0 && (buf[l - 1] & 0xC0) == 0x80) buf[--l] = 0;
        if (l > 0) buf[--l] = 0;
    } else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f && l + strlen(utf8) < n) {
        strcat(buf, utf8);
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (dialog) {
        if (sym == XKB_KEY_Escape) dialog_cancel();
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) dialog_submit();
        else text_edit(dlg_input, sizeof dlg_input, sym, utf8);
    } else if (sym == XKB_KEY_Escape) {
        if (search[0]) { search[0] = 0; search_alpine(search); } else { nx_quit(); return; }
    } else if (sym == XKB_KEY_Down) scroll += ROW_H;
    else if (sym == XKB_KEY_Up) scroll -= ROW_H;
    else if (sym == XKB_KEY_Next) scroll += ROW_H * 5;
    else if (sym == XKB_KEY_Prior) scroll -= ROW_H * 5;
    else if (sym == XKB_KEY_Tab) { tab = (tab + 1) % NTABS; scroll = 0; }
    else { text_edit(search, sizeof search, sym, utf8); scroll = 0; search_alpine(search); }
    nx_redraw(w);
}

int store_main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    load_pkgs();
    load_alpine();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--alpine")) tab = TAB_ALPINE;
        if (!strcmp(argv[i], "--updates")) tab = TAB_UPDATES;
    }
    if (!nx_init()) return 1;
    /* nx_init ignore SIGCHLD (pas de zombies) : ici on veut le code de
     * retour de nexus-pkg (mot de passe refusé, échec...) */
    signal(SIGCHLD, SIG_DFL);
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_toplevel_window(h, "Magasin d'applications", "nexus-store", WIN_W, WIN_H);
    if (!win) return 1;
    nx.timeout = op_timeout;
    nx.tick = op_tick;
    nx_run();
    if (child) kill(child, SIGTERM);
    explicit_bzero(password, sizeof password);
    return 0;
}
