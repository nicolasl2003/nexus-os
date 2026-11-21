/* nexus-monitor — le moniteur système de Nexus OS.
 *
 * En haut : processeur (courbe de la dernière minute), mémoire, disque,
 * durée depuis le démarrage. En dessous : les programmes en cours (nom,
 * utilisateur, part du processeur, mémoire, numéro), triables en cliquant
 * sur les titres de colonnes. « Arrêter le programme » envoie une demande
 * d'arrêt (SIGTERM) ; s'il ne répond pas, « Forcer l'arrêt » (SIGKILL).
 * Un utilisateur ne peut arrêter que ses propres programmes (règle du noyau).
 * Les informations viennent de /proc, rafraîchies toutes les 1,5 s.
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define CARDS_H 150
#define BAR_H   52
#define HEAD_H  32
#define ROW_H   30
#define STATUS_H 28
#define MAXP    1024

static struct nx_window *win;

/* ======================= Mesures ======================= */

struct proc {
    int pid;
    uid_t uid;
    char name[64], user[32];
    char label[64];            /* nom lisible : « Calculatrice », « Terminal »… */
    unsigned long long ticks;  /* utime + stime */
    double cpu;                /* % de la machine */
    long rss_kb;
    bool seen;
};
static struct proc procs[MAXP], prev[MAXP];
static int nprocs, nprev;
static unsigned long long last_total;
static double cpu_hist[60];
static int ncpu_hist;
static double cpu_now;
static long mem_total, mem_avail;     /* Ko */
static double disk_used, disk_total;  /* octets */
static const char *disk_label = "/";
static double uptime_s;
static int ncpus = 1;

static bool read_file(const char *p, char *buf, size_t n)
{
    FILE *f = fopen(p, "r");
    if (!f) return false;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return true;
}

static const char *user_name(uid_t uid)
{
    static struct { uid_t uid; char name[32]; } cache[32];
    static int n;
    for (int i = 0; i < n; i++) if (cache[i].uid == uid) return cache[i].name;
    struct passwd *pw = getpwuid(uid);
    if (n < 32) {
        cache[n].uid = uid;
        if (pw) snprintf(cache[n].name, 32, "%s", pw->pw_name);
        else snprintf(cache[n].name, 32, "%u", (unsigned)uid);
        return cache[n++].name;
    }
    return pw ? pw->pw_name : "?";
}

/* Nom complet : /proc/<pid>/comm est coupé à 15 caractères ; on prend le
 * nom du programme dans sa ligne de commande (ou du script lancé par sh) */
static void full_name(int pid, char *name, size_t n)
{
    char p[64], buf[512];
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    FILE *f = fopen(p, "r");
    if (!f) return;
    size_t r = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (!r) return;
    buf[r] = 0;
    const char *args[2] = { buf, NULL };
    size_t l0 = strlen(buf);
    if (l0 + 1 < r) args[1] = buf + l0 + 1;
    for (int i = 0; i < 2 && args[i]; i++) {
        const char *b = strrchr(args[i], '/') ? strrchr(args[i], '/') + 1 : args[i];
        if (strlen(b) > strlen(name) && !strncmp(b, name, strlen(name))) { snprintf(name, n, "%s", b); return; }
    }
}

static const char *friendly(const char *name)
{
    static const struct { const char *n, *l; } m[] = {
        { "nexus-files", "Fichiers" }, { "nexus-editor", "Éditeur de texte" },
        { "nexus-calculator", "Calculatrice" }, { "nexus-monitor", "Moniteur système" },
        { "nexus-settings", "Paramètres" }, { "nexus-store", "Magasin d'applications" },
        { "nexus-panel", "Barre des tâches" }, { "nexus-menu", "Menu Démarrer" },
        { "nexus-widgets", "Widgets du bureau" }, { "nexus-wallpaper", "Fond d'écran" },
        { "nexus-notifyd", "Notifications" }, { "nexus-network", "Panneau Réseau" },
        { "nexus-volume", "Panneau Son" }, { "nexus-greeter", "Écran de connexion" },
        { "nexus-login", "Connexion" }, { "nexus-update-check", "Vérification des mises à jour" },
        { "nexus-getty", "Console texte" }, { "getty", "Console texte" },
        { "labwc", "Gestionnaire de fenêtres" }, { "foot", "Terminal" },
        { "netsurf-fb", "Navigateur web" }, { "firefox", "Firefox" }, { "seatd", "Accès à l'écran (seatd)" },
        { "wpa_supplicant", "Wi-Fi" }, { "udhcpc", "Réseau (DHCP)" }, { "syslogd", "Journal système" },
        { "init", "Démarrage du système (init)" }, { "sh", "Interpréteur de commandes" },
        { "ash", "Interpréteur de commandes" },
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if (!strcmp(name, m[i].n)) return m[i].l;
    return name;
}

static unsigned long long read_total_cpu(void)
{
    char buf[512];
    if (!read_file("/proc/stat", buf, sizeof buf)) return 0;
    unsigned long long v[10] = { 0 }, total = 0;
    sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    for (int i = 0; i < 8; i++) total += v[i];
    static unsigned long long last_idle, last_all;
    unsigned long long idle = v[3] + v[4];
    if (last_all && total > last_all) cpu_now = 100.0 * (1.0 - (double)(idle - last_idle) / (total - last_all));
    if (cpu_now < 0) cpu_now = 0;
    last_idle = idle;
    last_all = total;
    return total;
}

static void sample(void)
{
    /* processeur, mémoire, disque */
    unsigned long long total = read_total_cpu();
    if (ncpu_hist == 60) { memmove(cpu_hist, cpu_hist + 1, 59 * sizeof *cpu_hist); ncpu_hist--; }
    cpu_hist[ncpu_hist++] = cpu_now;
    char buf[4096];
    if (read_file("/proc/meminfo", buf, sizeof buf)) {
        char *p = strstr(buf, "MemTotal:");
        if (p) mem_total = atol(p + 9);
        p = strstr(buf, "MemAvailable:");
        if (p) mem_avail = atol(p + 13);
    }
    struct statvfs vf;
    disk_label = access("/data/etc", F_OK) == 0 ? "/data" : "/";
    if (statvfs(disk_label, &vf) == 0) {
        disk_total = (double)vf.f_blocks * vf.f_frsize;
        disk_used = disk_total - (double)vf.f_bfree * vf.f_frsize;
    }
    if (read_file("/proc/uptime", buf, sizeof buf)) uptime_s = atof(buf);

    /* processus */
    memcpy(prev, procs, nprocs * sizeof *procs);
    nprev = nprocs;
    nprocs = 0;
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nprocs < MAXP) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int pid = atoi(e->d_name);
        char p[64];
        snprintf(p, sizeof p, "/proc/%d/stat", pid);
        if (!read_file(p, buf, sizeof buf)) continue;
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (!lp || !rp) continue;
        struct proc *pr = &procs[nprocs];
        memset(pr, 0, sizeof *pr);
        pr->pid = pid;
        snprintf(pr->name, sizeof pr->name, "%.*s", (int)(rp - lp - 1), lp + 1);
        if (strlen(pr->name) >= 15) full_name(pid, pr->name, sizeof pr->name);
        snprintf(pr->label, sizeof pr->label, "%s", friendly(pr->name));
        /* champs après ")" : état(3) ppid(4) ... utime(14) stime(15) ... rss(24) */
        char st;
        int ppid;
        unsigned long long ut = 0, stt = 0;
        long rss = 0;
        sscanf(rp + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
               &st, &ppid, &ut, &stt, &rss);
        if (pid == 2 || ppid == 2) continue;             /* fils du noyau (kthreadd) */
        pr->ticks = ut + stt;
        pr->rss_kb = rss * (sysconf(_SC_PAGESIZE) / 1024);
        snprintf(p, sizeof p, "/proc/%d/status", pid);
        if (read_file(p, buf, sizeof buf)) {
            char *u = strstr(buf, "\nUid:");
            if (u) pr->uid = (uid_t)atoi(u + 5);
        }
        snprintf(pr->user, sizeof pr->user, "%s", user_name(pr->uid));
        /* part du processeur depuis la mesure précédente */
        for (int i = 0; i < nprev; i++)
            if (prev[i].pid == pid) {
                if (total > last_total && pr->ticks >= prev[i].ticks)
                    pr->cpu = 100.0 * (pr->ticks - prev[i].ticks) / (double)(total - last_total);
                break;
            }
        nprocs++;
    }
    closedir(d);
    last_total = total;
}

/* ======================= Tri et sélection ======================= */

enum { COL_NAME, COL_USER, COL_CPU, COL_MEM, COL_PID, NCOLS };
static const char *col_names[NCOLS] = { "Nom", "Utilisateur", "Processeur", "Mémoire", "N°" };
static int sort_col = COL_NAME;          /* stable : les lignes ne bougent pas */
static bool sort_desc = false;
static int order[MAXP];
static int selected_pid = -1;
static int scroll;
static char status_msg[200];
static time_t status_until;
static int stopping_pid = -1;        /* SIGTERM envoyé : proposer de forcer */
static time_t stopping_since;
static bool confirm_open;

static int cmp_order(const void *a, const void *b)
{
    const struct proc *x = &procs[*(const int *)a], *y = &procs[*(const int *)b];
    int r = 0;
    switch (sort_col) {
    case COL_NAME: r = strcasecmp(x->label, y->label); break;
    case COL_USER: r = strcasecmp(x->user, y->user); break;
    case COL_CPU: r = x->cpu < y->cpu ? -1 : x->cpu > y->cpu; if (!r) r = x->rss_kb < y->rss_kb ? -1 : x->rss_kb > y->rss_kb; break;
    case COL_MEM: r = x->rss_kb < y->rss_kb ? -1 : x->rss_kb > y->rss_kb; break;
    case COL_PID: r = x->pid - y->pid; break;
    }
    if (!r) r = x->pid - y->pid;
    return sort_desc ? -r : r;
}

static void sort_procs(void)
{
    for (int i = 0; i < nprocs; i++) order[i] = i;
    qsort(order, nprocs, sizeof *order, cmp_order);
}

static struct proc *find_pid(int pid)
{
    for (int i = 0; i < nprocs; i++) if (procs[i].pid == pid) return &procs[i];
    return NULL;
}

static void set_status(const char *m)
{
    snprintf(status_msg, sizeof status_msg, "%s", m);
    status_until = time(NULL) + 6;
}

static void stop_selected(int sig)
{
    struct proc *p = find_pid(selected_pid);
    if (!p) return;
    char m[200];
    if (kill(p->pid, sig) != 0) {
        if (errno == EPERM)
            snprintf(m, sizeof m, "« %s » appartient à %s : seul l'administrateur (root) peut l'arrêter.", p->label, p->user);
        else snprintf(m, sizeof m, "Impossible d'arrêter « %s » : %s", p->name, strerror(errno));
        set_status(m);
        return;
    }
    if (sig == SIGTERM) {
        stopping_pid = p->pid;
        stopping_since = time(NULL);
        snprintf(m, sizeof m, "Arrêt de « %s » demandé…", p->label);
    } else {
        stopping_pid = -1;
        snprintf(m, sizeof m, "« %s » a été arrêté de force.", p->label);
    }
    set_status(m);
}

/* ======================= Dessin ======================= */

enum kind { K_HEAD, K_ROW, K_STOP, K_FORCE, K_CONFIRM_OK, K_CONFIRM_CANCEL, K_BG };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[160];
static int nrects;
static int hover_kind = -1, hover_index = -1;

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 160) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
}
static bool hot(enum kind k, int i) { return (int)k == hover_kind && i == hover_index; }

static void human(char *out, size_t n, double bytes)
{
    const char *u[] = { "o", "Ko", "Mo", "Go", "To" };
    int i = 0;
    while (bytes >= 1024 && i < 4) { bytes /= 1024; i++; }
    if (i == 0) snprintf(out, n, "%.0f %s", bytes, u[i]);
    else snprintf(out, n, bytes < 10 ? "%.1f %s" : "%.0f %s", bytes, u[i]);
    for (char *c = out; *c; c++) if (*c == '.') *c = ',';
}

static void card(cairo_t *cr, double x, double y, double w, double h, const char *title, const char *value,
                 const char *sub, double frac, nx_color col, bool graph)
{
    nx_rounded_rect(cr, x, y, w, h, 12);
    nx_set(cr, nx_rgb("#f4f6fa"));
    cairo_fill(cr);
    nx_text(cr, title, 13, true, nx_rgb(NX_SUBTLE), x + 16, y + 12, w - 32, 20, NX_LEFT);
    nx_text(cr, value, 24, true, nx_rgb(NX_TEXT), x + 16, y + 34, w - 32, 34, NX_LEFT);
    nx_text(cr, sub, 12, false, nx_rgb(NX_SUBTLE), x + 16, y + 68, w - 32, 18, NX_LEFT);
    double gx = x + 16, gy = y + 94, gw = w - 32, gh = h - 108;
    if (graph) {
        nx_rounded_rect(cr, gx, gy, gw, gh, 6);
        nx_set(cr, nx_rgb("#ffffff"));
        cairo_fill(cr);
        if (ncpu_hist > 1) {
            cairo_move_to(cr, gx, gy + gh);
            for (int i = 0; i < ncpu_hist; i++)
                cairo_line_to(cr, gx + gw * (i + 60 - ncpu_hist) / 59.0, gy + gh - gh * cpu_hist[i] / 100.0);
            cairo_line_to(cr, gx + gw, gy + gh);
            cairo_close_path(cr);
            nx_set(cr, nx_alpha(col, 0.2));
            cairo_fill_preserve(cr);
            cairo_new_path(cr);
            for (int i = 0; i < ncpu_hist; i++) {
                double px = gx + gw * (i + 60 - ncpu_hist) / 59.0, py = gy + gh - gh * cpu_hist[i] / 100.0;
                if (i == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
            }
            nx_set(cr, col);
            cairo_set_line_width(cr, 1.8);
            cairo_stroke(cr);
        }
    } else {
        double bh = 10, by = gy + (gh - bh) / 2;
        nx_rounded_rect(cr, gx, by, gw, bh, 5);
        nx_set(cr, nx_rgb("#e1e5ec"));
        cairo_fill(cr);
        if (frac > 0) {
            nx_rounded_rect(cr, gx, by, fmax(bh, gw * fmin(frac, 1)), bh, 5);
            nx_set(cr, frac > 0.9 ? nx_rgb("#d64545") : col);
            cairo_fill(cr);
        }
    }
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    nx_color text = nx_rgb(NX_TEXT), subtle = nx_rgb(NX_SUBTLE), accent = nx_accent_color();
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    /* ---- Cartes ---- */
    double pad = 16, cw = (width - 4 * pad) / 3, ch = CARDS_H - pad;
    char v[64], s[128], a[32], b[32];
    snprintf(v, sizeof v, "%.0f %%", cpu_now);
    int up = (int)uptime_s;
    if (up >= 3600) snprintf(s, sizeof s, "%d cœur%s · allumé depuis %d h %02d", ncpus, ncpus > 1 ? "s" : "", up / 3600, (up % 3600) / 60);
    else snprintf(s, sizeof s, "%d cœur%s · allumé depuis %d min", ncpus, ncpus > 1 ? "s" : "", up / 60);
    card(cr, pad, pad, cw, ch, "Processeur", v, s, 0, accent, true);
    long used = mem_total - mem_avail;
    human(a, sizeof a, used * 1024.0);
    human(b, sizeof b, mem_total * 1024.0);
    snprintf(v, sizeof v, "%.0f %%", mem_total ? 100.0 * used / mem_total : 0);
    snprintf(s, sizeof s, "%s utilisés sur %s", a, b);
    card(cr, 2 * pad + cw, pad, cw, ch, "Mémoire", v, s, mem_total ? (double)used / mem_total : 0, nx_rgb("#8e5bd8"), false);
    human(a, sizeof a, disk_used);
    human(b, sizeof b, disk_total);
    snprintf(v, sizeof v, "%.0f %%", disk_total ? 100.0 * disk_used / disk_total : 0);
    snprintf(s, sizeof s, "%s sur %s (%s)", a, b, !strcmp(disk_label, "/data") ? "disque de données" : "système");
    card(cr, 3 * pad + 2 * cw, pad, cw, ch, "Disque", v, s, disk_total ? disk_used / disk_total : 0, nx_rgb("#2f9e5a"), false);

    /* ---- Barre : titre + boutons ---- */
    double by = CARDS_H;
    char t[64];
    snprintf(t, sizeof t, "Programmes en cours (%d)", nprocs);
    nx_text(cr, t, 16, true, text, pad, by, 300, BAR_H, NX_LEFT);
    struct proc *sel = find_pid(selected_pid);
    bool can = sel != NULL;
    const char *stop_l = "Arrêter le programme";
    double sw = nx_text_width(cr, stop_l, 13, true) + 32, sx = width - pad - sw;
    nx_rounded_rect(cr, sx, by + 9, sw, 34, 8);
    nx_set(cr, !can ? nx_rgb("#e9ecf1") : hot(K_STOP, 0) ? nx_rgb("#c0392b") : nx_rgb("#d64545"));
    cairo_fill(cr);
    nx_text(cr, stop_l, 13, true, can ? nx_rgb("#ffffff") : nx_rgb("#a0a7b5"), sx, by + 9, sw, 34, NX_CENTER);
    if (can) add_rect(sx, by + 9, sw, 34, K_STOP, 0);
    if (stopping_pid > 0 && find_pid(stopping_pid) && time(NULL) - stopping_since >= 2) {
        const char *fl = "Forcer l'arrêt";
        double fw = nx_text_width(cr, fl, 13, true) + 32, fx = sx - 10 - fw;
        nx_rounded_rect(cr, fx, by + 9, fw, 34, 8);
        nx_set(cr, hot(K_FORCE, 0) ? nx_rgb("#7a1f1f") : nx_rgb("#962d2d"));
        cairo_fill(cr);
        nx_text(cr, fl, 13, true, nx_rgb("#ffffff"), fx, by + 9, fw, 34, NX_CENTER);
        add_rect(fx, by + 9, fw, 34, K_FORCE, 0);
    }

    /* ---- Tableau ---- */
    double hy = CARDS_H + BAR_H;
    double colx[NCOLS + 1];
    double tw = width - 2 * pad;
    double frac[NCOLS] = { 0.38, 0.17, 0.16, 0.16, 0.13 };
    colx[0] = pad;
    for (int i = 0; i < NCOLS; i++) colx[i + 1] = colx[i] + tw * frac[i];
    cairo_rectangle(cr, pad, hy, tw, HEAD_H);
    nx_set(cr, nx_rgb("#f4f6fa"));
    cairo_fill(cr);
    for (int i = 0; i < NCOLS; i++) {
        char l[48];
        snprintf(l, sizeof l, "%s%s", col_names[i], sort_col == i ? (sort_desc ? " ▾" : " ▴") : "");
        bool right = i >= COL_CPU;
        nx_text(cr, l, 12.5, sort_col == i, hot(K_HEAD, i) ? accent : subtle, colx[i] + 10, hy, colx[i + 1] - colx[i] - 20,
                HEAD_H, right ? NX_RIGHT : NX_LEFT);
        add_rect(colx[i], hy, colx[i + 1] - colx[i], HEAD_H, K_HEAD, i);
    }
    double ry0 = hy + HEAD_H, rh = height - ry0 - STATUS_H;
    int rows = (int)(rh / ROW_H);
    if (scroll > nprocs - rows) scroll = nprocs - rows;
    if (scroll < 0) scroll = 0;
    cairo_save(cr);
    cairo_rectangle(cr, 0, ry0, width, rh);
    cairo_clip(cr);
    for (int r = 0; r <= rows && scroll + r < nprocs; r++) {
        struct proc *p = &procs[order[scroll + r]];
        double y = ry0 + r * ROW_H;
        bool is_sel = p->pid == selected_pid;
        if (is_sel || hot(K_ROW, p->pid)) {
            cairo_rectangle(cr, pad, y, tw, ROW_H);
            nx_set(cr, is_sel ? nx_alpha(accent, 0.16) : nx_rgb("#f6f8fb"));
            cairo_fill(cr);
        }
        nx_app_icon(cr, nx_app_icon_for(p->name), colx[0] + 8, y + 5, 20, text);
        double lw = nx_text(cr, p->label, 13, is_sel, text, colx[0] + 36, y, colx[1] - colx[0] - 44, ROW_H, NX_LEFT);
        if (strcmp(p->label, p->name) && colx[0] + 36 + lw + 60 < colx[1])
            nx_text(cr, p->name, 11.5, false, nx_rgb("#a0a7b5"), colx[0] + 44 + lw, y, colx[1] - colx[0] - 52 - lw, ROW_H, NX_LEFT);
        nx_text(cr, p->user, 13, false, subtle, colx[1] + 10, y, colx[2] - colx[1] - 20, ROW_H, NX_LEFT);
        char c[32], m[32], pid[16];
        snprintf(c, sizeof c, "%.1f %%", p->cpu);
        for (char *q = c; *q; q++) if (*q == '.') *q = ',';
        human(m, sizeof m, p->rss_kb * 1024.0);
        snprintf(pid, sizeof pid, "%d", p->pid);
        nx_text(cr, c, 13, p->cpu >= 10, p->cpu >= 50 ? nx_rgb("#d64545") : text, colx[2] + 10, y, colx[3] - colx[2] - 20, ROW_H, NX_RIGHT);
        nx_text(cr, p->rss_kb ? m : "—", 13, false, text, colx[3] + 10, y, colx[4] - colx[3] - 20, ROW_H, NX_RIGHT);
        nx_text(cr, pid, 13, false, subtle, colx[4] + 10, y, colx[5] - colx[4] - 20, ROW_H, NX_RIGHT);
        add_rect(pad, y, tw, ROW_H, K_ROW, p->pid);
    }
    cairo_restore(cr);
    if (nprocs > rows && rows > 0) {
        double bh = fmax(30, rh * rows / (double)nprocs), byy = ry0 + (rh - bh) * scroll / (double)(nprocs - rows);
        nx_rounded_rect(cr, width - 7, byy, 4, bh, 2);
        nx_set(cr, nx_alpha(subtle, 0.45));
        cairo_fill(cr);
    }

    /* ---- Barre d'état ---- */
    double sy = height - STATUS_H;
    cairo_rectangle(cr, 0, sy, width, STATUS_H);
    nx_set(cr, nx_rgb("#f7f8fb"));
    cairo_fill(cr);
    const char *msg = status_msg[0] && time(NULL) < status_until ? status_msg :
                      sel ? "Clique sur « Arrêter le programme » pour fermer le programme sélectionné." :
                            "Clique sur un programme pour le sélectionner ; clique sur un titre de colonne pour trier.";
    nx_text(cr, msg, 12, false, nx_rgb("#3a4152"), pad, sy, width - 2 * pad, STATUS_H, NX_LEFT);

    /* ---- Confirmation ---- */
    if (confirm_open && sel) {
        cairo_set_source_rgba(cr, 0.05, 0.07, 0.1, 0.35);
        cairo_paint(cr);
        add_rect(0, 0, width, height, K_BG, 0);
        double dw = 460, dh = 170, dx = (width - dw) / 2, dy = (height - dh) / 2;
        nx_rounded_rect(cr, dx, dy, dw, dh, 12);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill(cr);
        char q[160];
        snprintf(q, sizeof q, "Arrêter « %s » ?", sel->label);
        nx_text(cr, q, 17, true, text, dx + 24, dy + 18, dw - 48, 30, NX_LEFT);
        const char *warn = sel->pid == 1 ? "C'est le premier programme du système : l'arrêter éteindrait tout."
                         : !strcmp(sel->name, "labwc") ? "C'est le gestionnaire de fenêtres : ta session se fermera."
                         : "Le travail non enregistré dans ce programme sera perdu.";
        nx_text(cr, warn, 13, false, subtle, dx + 24, dy + 56, dw - 48, 22, NX_LEFT);
        double bby = dy + dh - 56;
        nx_rounded_rect(cr, dx + dw - 24 - 120, bby, 120, 36, 8);
        nx_set(cr, hot(K_CONFIRM_OK, 0) ? nx_rgb("#c0392b") : nx_rgb("#d64545"));
        cairo_fill(cr);
        nx_text(cr, "Arrêter", 13, true, nx_rgb("#ffffff"), dx + dw - 24 - 120, bby, 120, 36, NX_CENTER);
        add_rect(dx + dw - 24 - 120, bby, 120, 36, K_CONFIRM_OK, 0);
        nx_rounded_rect(cr, dx + dw - 24 - 120 - 10 - 110, bby, 110, 36, 8);
        nx_set(cr, hot(K_CONFIRM_CANCEL, 0) ? nx_rgb("#e3e7ef") : nx_rgb("#eef1f6"));
        cairo_fill(cr);
        nx_text(cr, "Annuler", 13, false, text, dx + dw - 24 - 120 - 10 - 110, bby, 110, 36, NX_CENTER);
        add_rect(dx + dw - 24 - 120 - 10 - 110, bby, 110, 36, K_CONFIRM_CANCEL, 0);
    }
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
    switch (ev) {
    case NX_MOTION: case NX_LEAVE:
        if (ev == NX_LEAVE) k = i = -1;
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
        break;
    case NX_SCROLL:
        if (!confirm_open) { scroll += value * 3; nx_redraw(w); }
        break;
    case NX_RELEASE:
        if (value != BTN_LEFT || !r) break;
        if (confirm_open) {
            if (r->kind == K_CONFIRM_OK) { confirm_open = false; stop_selected(SIGTERM); }
            else if (r->kind == K_CONFIRM_CANCEL) confirm_open = false;
            nx_redraw(w);
            break;
        }
        switch (r->kind) {
        case K_HEAD:
            if (sort_col == r->index) sort_desc = !sort_desc;
            else { sort_col = r->index; sort_desc = r->index == COL_CPU || r->index == COL_MEM; }
            sort_procs();
            break;
        case K_ROW: selected_pid = r->index; break;
        case K_STOP: confirm_open = true; break;
        case K_FORCE: {
            int save = selected_pid;
            selected_pid = stopping_pid;
            stop_selected(SIGKILL);
            selected_pid = save;
            break;
        }
        default: break;
        }
        nx_redraw(w);
        break;
    default:
        break;
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    (void)utf8;
    if (confirm_open) {
        if (sym == XKB_KEY_Escape) confirm_open = false;
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { confirm_open = false; stop_selected(SIGTERM); }
        else return;
        nx_redraw(w);
        return;
    }
    int cur = -1;
    for (int i = 0; i < nprocs; i++) if (procs[order[i]].pid == selected_pid) cur = i;
    switch (sym) {
    case XKB_KEY_Down: if (nprocs) selected_pid = procs[order[cur + 1 < nprocs ? cur + 1 : cur]].pid; break;
    case XKB_KEY_Up: if (nprocs && cur > 0) selected_pid = procs[order[cur - 1]].pid; break;
    case XKB_KEY_Delete: if (find_pid(selected_pid)) confirm_open = true; break;
    case XKB_KEY_Escape: selected_pid = -1; break;
    default: return;
    }
    /* garder la sélection visible */
    for (int i = 0; i < nprocs; i++)
        if (procs[order[i]].pid == selected_pid) {
            int rows = (int)((win->height - CARDS_H - BAR_H - HEAD_H - STATUS_H) / ROW_H);
            if (i < scroll) scroll = i;
            if (i >= scroll + rows) scroll = i - rows + 1;
        }
    nx_redraw(w);
}

static int mon_timeout(void) { return 1500; }
static void mon_tick(void)
{
    sample();
    sort_procs();
    if (stopping_pid > 0 && !find_pid(stopping_pid)) {
        set_status("Le programme s'est arrêté.");
        stopping_pid = -1;
    }
    nx_redraw(win);
}

int monitor_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    ncpus = n > 0 ? (int)n : 1;
    sample();
    usleep(250000);
    sample();
    sort_procs();
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_toplevel_window(h, "Moniteur système", "nexus-monitor", 860, 620);
    if (!win) return 1;
    nx.timeout = mon_timeout;
    nx.tick = mon_tick;
    nx_run();
    return 0;
}
