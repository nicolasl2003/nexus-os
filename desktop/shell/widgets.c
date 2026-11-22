/* nexus-widgets — les widgets du bureau (en haut à droite) :
 *   - horloge et date ;
 *   - raccourcis vers des dossiers (~/.config/nexus/raccourcis, « Ajouter
 *     aux raccourcis » dans Fichiers) ;
 *   - performances : processeur, mémoire, disque ;
 *   - liste de tâches à cocher (~/.config/nexus/taches.txt) ;
 *   - mode simplifié et taille du texte.
 * Réglables dans Paramètres → Widgets : affichage, taille, couleur.
 * À la souris : glisser une carte = déplacer les widgets ; tirer le coin en
 * bas à gauche (ou le bord gauche) = les agrandir / réduire. */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define W_WIDTH   380
#define W_HEIGHT  720
#define CARD_X    12
#define CARD_W    (W_WIDTH - 2 * CARD_X)
#define MAX_TASKS 6

static struct nx_window *win;
static char username[64] = "";
static int ntasks;          /* (défini plus bas avec la liste des tâches) */

/* ---------- Taille, couleur, position ---------- */

static double S = 1;                 /* échelle choisie (widget_scale / 100) */
static double Sd = 1;                /* échelle de dessin : S, réduite si l'écran est trop petit */
static int content_h = 600;          /* hauteur logique du contenu */
static int cur_w, cur_h;             /* taille réelle de la surface */
static nx_color text_c, subtle_c;    /* couleurs du texte selon le style */

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };
static int drag, grip_hover;
static double drag_x0, drag_y0;
static int drag_mr0, drag_mt0, drag_w0;

static void card(cairo_t *cr, double y, double h)
{
    switch (nx_conf.widget_style) {
    case 1:   /* clairs */
        for (int i = 4; i > 0; i--) {
            nx_rounded_rect(cr, CARD_X - i, y - i + 3, CARD_W + 2 * i, h + 2 * i, 18 + i);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.03);
            cairo_fill(cr);
        }
        nx_rounded_rect(cr, CARD_X, y, CARD_W, h, 18);
        cairo_set_source_rgba(cr, 0.97, 0.98, 1, 0.9);
        cairo_fill(cr);
        break;
    case 2: { /* couleur d'accent */
        nx_color a = nx_accent_color(), d = nx_mix(a, nx_rgb("#000000"), 0.45);
        nx_rounded_rect(cr, CARD_X, y, CARD_W, h, 18);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + h);
        cairo_pattern_add_color_stop_rgba(g, 0, a.r, a.g, a.b, 0.88);
        cairo_pattern_add_color_stop_rgba(g, 1, d.r, d.g, d.b, 0.92);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        break;
    }
    case 3:   /* transparents : simple voile */
        nx_rounded_rect(cr, CARD_X, y, CARD_W, h, 18);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        break;
    default:
        nx_glass(cr, CARD_X, y, CARD_W, h, 18);
    }
}

/* Applique taille et position (après un changement de réglage ou de tâches) */
static void apply_geometry(void)
{
    if (!win) return;
    S = nx_conf.widget_scale / 100.0;
    /* surface sur toute la hauteur disponible (au-dessus de la barre) : on
     * connaît ainsi la place et on réduit l'échelle si besoin */
    int w = (int)(W_WIDTH * S * nx_scale + 0.5);   /* le texte agrandi agrandit les cartes */
    zwlr_layer_surface_v1_set_margin(win->layer, nx_conf.widget_mt, nx_conf.widget_mr, 6, 0);
    if (w != cur_w) {
        cur_w = w;
        zwlr_layer_surface_v1_set_size(win->layer, w, 0);
    }
    wl_surface_commit(win->surface);
    nx_redraw(win);
}

/* Hauteurs des cartes */
#define H_CLOCK 104
#define H_SHORT 138
#define H_PERF  162
#define H_SIMPLE 120
#define GAP 14
static int compute_content_h(void)
{
    return 12 + H_CLOCK + GAP + H_SHORT + GAP + (nx_conf.simple ? 0 : H_PERF + GAP) +
           58 + ntasks * 34 + 40 + GAP + H_SIMPLE + 12;
}

/* ---------- Raccourcis ---------- */

struct shortcut { char label[64], path[512]; const char *icon; };
static struct shortcut shorts[4];
static int nshorts;

static void shortcuts_load(void)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/";
    char p[512];
    snprintf(p, sizeof p, "%s/.config/nexus/raccourcis", home);
    nshorts = 0;
    FILE *f = fopen(p, "r");
    if (f) {
        char line[600];
        while (nshorts < 4 && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            char *bar = strchr(line, '|');
            if (!bar) continue;
            *bar = 0;
            snprintf(shorts[nshorts].label, sizeof shorts[0].label, "%s", line);
            snprintf(shorts[nshorts].path, sizeof shorts[0].path, "%s", bar + 1);
            shorts[nshorts].icon = "folder";
            nshorts++;
        }
        fclose(f);
    }
    if (!nshorts) {   /* par défaut : dossier personnel, Images, Téléchargements */
        static const struct { const char *l, *d, *i; } def[] = {
            { "Bureau", "", "home" }, { "Images", "/Images", "image" }, { "Télécharg.", "/Téléchargements", "downloads" },
        };
        for (int i = 0; i < 3; i++) {
            snprintf(shorts[i].label, sizeof shorts[0].label, "%s", def[i].l);
            snprintf(shorts[i].path, sizeof shorts[0].path, "%s%s", home, def[i].d);
            shorts[i].icon = def[i].i;
        }
        nshorts = 3;
    }
    /* icônes reconnues par le nom du dossier */
    for (int i = 0; i < nshorts; i++) {
        const char *b = strrchr(shorts[i].path, '/');
        b = b ? b + 1 : shorts[i].path;
        if (!strcmp(shorts[i].path, home)) shorts[i].icon = "home";
        else if (!strcmp(b, "Images")) shorts[i].icon = "image";
        else if (!strcmp(b, "Téléchargements")) shorts[i].icon = "downloads";
        else if (!strcmp(b, "Documents")) shorts[i].icon = "editor";
    }
}

/* ---------- Mesures système ---------- */

static double cpu_pct, ram_pct, disk_pct;
static double history[48];
static unsigned long long last_busy, last_total;

static void sample(void)
{
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long v[8] = { 0 };
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
            unsigned long long total = 0;
            for (int i = 0; i < 8; i++) total += v[i];
            unsigned long long busy = total - v[3] - v[4];
            if (last_total && total > last_total)
                cpu_pct = 100.0 * (busy - last_busy) / (total - last_total);
            last_busy = busy;
            last_total = total;
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        double total = 0, avail = 0;
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %lf", &total);
            sscanf(line, "MemAvailable: %lf", &avail);
        }
        fclose(f);
        if (total > 0) ram_pct = 100.0 * (total - avail) / total;
    }
    struct statvfs s;
    if (statvfs(access("/data", F_OK) == 0 ? "/data" : "/", &s) == 0 && s.f_blocks)
        disk_pct = 100.0 * (s.f_blocks - s.f_bavail) / s.f_blocks;
    memmove(history, history + 1, sizeof history - sizeof history[0]);
    history[47] = cpu_pct;
}

/* ---------- Tâches ---------- */

struct task { char text[120]; bool done; };
static struct task tasks[MAX_TASKS];
static int ntasks;
static bool adding;
static char input[120];

static void tasks_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/.config/nexus/taches.txt", getenv("HOME") ? getenv("HOME") : "/tmp");
}

static void tasks_load(void)
{
    char p[512];
    tasks_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    ntasks = 0;
    if (!f) {   /* premier lancement : quelques exemples pour montrer le principe */
        static const char *ex[] = { "Découvrir le bureau Nexus", "Choisir un fond d'écran" };
        for (int i = 0; i < 2; i++) { snprintf(tasks[i].text, sizeof tasks[i].text, "%s", ex[i]); tasks[i].done = false; }
        ntasks = 2;
        return;
    }
    char line[200];
    while (ntasks < MAX_TASKS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) < 3 || line[1] != '|') continue;
        tasks[ntasks].done = line[0] == '1';
        snprintf(tasks[ntasks].text, sizeof tasks[ntasks].text, "%s", line + 2);
        ntasks++;
    }
    fclose(f);
}

static void tasks_save(void)
{
    char p[512], dir[512];
    tasks_path(p, sizeof p);
    snprintf(dir, sizeof dir, "%s", p);
    *strrchr(dir, '/') = 0;
    char parent[512];
    snprintf(parent, sizeof parent, "%s", dir);
    *strrchr(parent, '/') = 0;
    mkdir(parent, 0755);
    mkdir(dir, 0755);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < ntasks; i++) fprintf(f, "%d|%s\n", tasks[i].done, tasks[i].text);
    fclose(f);
    content_h = compute_content_h();   /* la carte « Mes tâches » change de hauteur */
    apply_geometry();
}

/* ---------- Zones cliquables ---------- */

enum { Z_NONE, Z_CHECK, Z_DELETE, Z_ADD, Z_SHORT, Z_SHORTADD, Z_SIMPLE, Z_TEXT };
struct zone { double x, y, w, h; int kind, index; };
static struct zone zones[32];
static int nzones;
static int hover_kind, hover_index = -1;

static void add_zone(double x, double y, double w, double h, int kind, int index)
{
    if (nzones < 32) zones[nzones++] = (struct zone){ x, y, w, h, kind, index };
}

/* ---------- Dessin ---------- */

/* Couleur de premier plan selon le style des cartes : verre (suit le thème),
 * clair (foncé), accent ou transparent (blanc) */
static nx_color wfg(double a)
{
    if (nx_conf.widget_style == 1) return nx_alpha(nx_rgb(NX_TEXT), a);
    if (nx_conf.widget_style == 0) return nx_fg(a);
    return nx_alpha(nx_rgb("#ffffff"), a);
}

/* Symbole blanc d'un raccourci (maison, image, téléchargement, dossier, texte) */
static void short_glyph(cairo_t *cr, const char *icon, double x, double y, double s)
{
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, s, s);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 0.1);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (!strcmp(icon, "home")) {
        cairo_move_to(cr, 0.08, 0.5); cairo_line_to(cr, 0.5, 0.1); cairo_line_to(cr, 0.92, 0.5);
        cairo_line_to(cr, 0.8, 0.5); cairo_line_to(cr, 0.8, 0.9); cairo_line_to(cr, 0.58, 0.9);
        cairo_line_to(cr, 0.58, 0.64); cairo_line_to(cr, 0.42, 0.64); cairo_line_to(cr, 0.42, 0.9);
        cairo_line_to(cr, 0.2, 0.9); cairo_line_to(cr, 0.2, 0.5); cairo_close_path(cr);
        cairo_fill(cr);
    } else if (!strcmp(icon, "image")) {
        cairo_move_to(cr, 0.06, 0.86); cairo_line_to(cr, 0.36, 0.44); cairo_line_to(cr, 0.56, 0.68);
        cairo_line_to(cr, 0.7, 0.52); cairo_line_to(cr, 0.94, 0.86); cairo_close_path(cr);
        cairo_fill(cr);
        cairo_arc(cr, 0.74, 0.24, 0.1, 0, 2 * M_PI);
        cairo_fill(cr);
    } else if (!strcmp(icon, "downloads")) {
        cairo_move_to(cr, 0.5, 0.08); cairo_line_to(cr, 0.5, 0.62);
        cairo_move_to(cr, 0.28, 0.42); cairo_line_to(cr, 0.5, 0.64); cairo_line_to(cr, 0.72, 0.42);
        cairo_move_to(cr, 0.12, 0.72); cairo_line_to(cr, 0.12, 0.9); cairo_line_to(cr, 0.88, 0.9); cairo_line_to(cr, 0.88, 0.72);
        cairo_stroke(cr);
    } else if (!strcmp(icon, "editor")) {
        cairo_rectangle(cr, 0.2, 0.08, 0.6, 0.84);
        cairo_stroke(cr);
        for (int i = 0; i < 3; i++) { cairo_move_to(cr, 0.32, 0.32 + i * 0.18); cairo_line_to(cr, 0.68, 0.32 + i * 0.18); }
        cairo_stroke(cr);
    } else {   /* dossier */
        cairo_move_to(cr, 0.06, 0.2); cairo_line_to(cr, 0.4, 0.2); cairo_line_to(cr, 0.5, 0.32);
        cairo_line_to(cr, 0.94, 0.32); cairo_line_to(cr, 0.94, 0.86); cairo_line_to(cr, 0.06, 0.86);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static const char *jours[] = { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" };
static const char *mois[] = { "janvier", "février", "mars", "avril", "mai", "juin", "juillet",
                              "août", "septembre", "octobre", "novembre", "décembre" };

static void ring(cairo_t *cr, double cx, double cy, double r, double pct, nx_color c, const char *label)
{
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    nx_set(cr, wfg(0.10));
    cairo_set_line_width(cr, 7);
    cairo_stroke(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, -M_PI / 2, -M_PI / 2 + 2 * M_PI * fmin(pct, 100) / 100);
    nx_set(cr, c);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
    char t[16];
    snprintf(t, sizeof t, "%.0f%%", pct);
    nx_text(cr, t, 17, true, wfg(1), cx - r, cy - 13, 2 * r, 26, NX_CENTER);
    nx_text(cr, label, 12, false, c, cx - r - 10, cy + r + 6, 2 * r + 20, 18, NX_CENTER);
}

static void draw_sun_or_moon(cairo_t *cr, double x, double y, double s, bool day)
{
    if (day) {
        cairo_set_source_rgb(cr, 1, 0.78, 0.25);
        cairo_arc(cr, x + s / 2, y + s / 2, s * 0.24, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_line_width(cr, s * 0.07);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (int i = 0; i < 8; i++) {
            double a = i * M_PI / 4;
            cairo_move_to(cr, x + s / 2 + cos(a) * s * 0.34, y + s / 2 + sin(a) * s * 0.34);
            cairo_line_to(cr, x + s / 2 + cos(a) * s * 0.46, y + s / 2 + sin(a) * s * 0.46);
        }
        cairo_stroke(cr);
    } else {
        cairo_push_group(cr);
        cairo_set_source_rgb(cr, 0.95, 0.9, 0.7);
        cairo_arc(cr, x + s / 2, y + s / 2, s * 0.36, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_arc(cr, x + s * 0.68, y + s * 0.38, s * 0.30, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
    }
}

static void draw_logical(cairo_t *cr)
{
    nzones = 0;
    text_c = nx_conf.widget_style == 1 ? nx_rgb(NX_TEXT) :
             nx_conf.widget_style == 0 ? nx_rgb(NX_GLASS_TEXT) : nx_rgb("#eef1f7");
    subtle_c = nx_conf.widget_style == 1 ? nx_rgb(NX_SUBTLE) :
               nx_conf.widget_style == 2 ? nx_alpha(nx_rgb("#ffffff"), 0.75) :
               nx_conf.widget_style == 0 ? nx_rgb(NX_GLASS_SUBTLE) : nx_rgb("#b9c1d3");
    nx_color text = text_c, subtle = subtle_c, accent = nx_accent_color();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    /* --- Horloge --- */
    double y = 12, ch = H_CLOCK;
    card(cr, y, ch);
    char hm[8], date[64];
    snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(date, sizeof date, "%s %d %s %d", jours[tm.tm_wday], tm.tm_mday, mois[tm.tm_mon], tm.tm_year + 1900);
    nx_text(cr, hm, 52, true, text, CARD_X + 20, y + 8, 0, 62, NX_LEFT);
    nx_text(cr, date, 15, true, subtle, CARD_X + 22, y + 68, CARD_W - 40, 24, NX_LEFT);
    bool day = tm.tm_hour >= 7 && tm.tm_hour < 19;
    draw_sun_or_moon(cr, CARD_X + CARD_W - 74, y + 16, 50, day);

    /* --- Raccourcis --- */
    y += ch + GAP;
    ch = H_SHORT;
    card(cr, y, ch);
    nx_text(cr, "Raccourcis", 16, true, text, CARD_X + 20, y + 12, 0, 26, NX_LEFT);
    int nt = nshorts < 4 ? nshorts + 1 : 4;
    double tw4 = (CARD_W - 32) / 4.0;
    for (int i = 0; i < nt; i++) {
        bool add = i >= nshorts;
        double tx = CARD_X + 16 + i * tw4, ty = y + 48;
        bool hov = hover_kind == (add ? Z_SHORTADD : Z_SHORT) && (add || hover_index == i);
        nx_rounded_rect(cr, tx + (tw4 - 54) / 2, ty, 54, 54, 14);
        if (add) nx_set(cr, wfg(hov ? 0.20 : 0.12));
        else {
            cairo_pattern_t *g = cairo_pattern_create_linear(0, ty, 0, ty + 54);
            nx_color a1 = nx_mix(accent, nx_rgb("#ffffff"), hov ? 0.30 : 0.15);
            cairo_pattern_add_color_stop_rgb(g, 0, a1.r, a1.g, a1.b);
            cairo_pattern_add_color_stop_rgb(g, 1, accent.r, accent.g, accent.b);
            cairo_set_source(cr, g);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
            nx_rounded_rect(cr, tx + (tw4 - 54) / 2, ty, 54, 54, 14);
            cairo_new_path(cr);
        }
        cairo_fill(cr);
        if (add) {
            nx_set(cr, text);
            cairo_set_line_width(cr, 2.6);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            double cx = tx + tw4 / 2, cy = ty + 27;
            cairo_move_to(cr, cx - 11, cy); cairo_line_to(cr, cx + 11, cy);
            cairo_move_to(cr, cx, cy - 11); cairo_line_to(cr, cx, cy + 11);
            cairo_stroke(cr);
        } else {
            short_glyph(cr, shorts[i].icon, tx + (tw4 - 28) / 2, ty + 13, 28);
        }
        nx_text(cr, add ? "Ajouter" : shorts[i].label, 12.5, true, text, tx, ty + 58, tw4, 22, NX_CENTER);
        add_zone(tx, ty, tw4, 82, add ? Z_SHORTADD : Z_SHORT, add ? -1 : i);
    }

    /* --- Performances (masquées en mode simplifié) --- */
    if (!nx_conf.simple) {
        y += ch + GAP;
        ch = H_PERF;
        card(cr, y, ch);
        nx_text(cr, "Performances système", 16, true, text, CARD_X + 20, y + 12, 0, 26, NX_LEFT);
        double rx = CARD_X + CARD_W / 6.0, ry = y + 90;
        ring(cr, rx, ry, 36, cpu_pct, nx_rgb("#2bc4f0"), "CPU");
        ring(cr, rx + CARD_W / 3.0, ry, 36, ram_pct, nx_rgb("#a86bff"), "RAM");
        ring(cr, rx + 2 * CARD_W / 3.0, ry, 36, disk_pct, nx_rgb("#2fd38a"), "Disque");
    }

    /* --- Liste de tâches --- */
    y += ch + GAP;
    ch = 58 + ntasks * 34 + 40;
    card(cr, y, ch);
    nx_text(cr, "Liste de tâches", 16, true, text, CARD_X + 20, y + 12, 0, 26, NX_LEFT);
    double ty = y + 46;
    for (int i = 0; i < ntasks; i++, ty += 34) {
        double bx = CARD_X + 20, by = ty + 8;
        nx_rounded_rect(cr, bx, by, 20, 20, 5);
        if (tasks[i].done) {
            nx_set(cr, accent);
            cairo_fill(cr);
            cairo_move_to(cr, bx + 5, by + 10.5);
            cairo_line_to(cr, bx + 8.5, by + 14);
            cairo_line_to(cr, bx + 15, by + 6.5);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_set_line_width(cr, 2.2);
            cairo_stroke(cr);
        } else {
            nx_set(cr, nx_alpha(text, hover_kind == Z_CHECK && hover_index == i ? 0.7 : 0.4));
            cairo_set_line_width(cr, 1.6);
            cairo_stroke(cr);
        }
        add_zone(CARD_X + 12, ty, CARD_W - 60, 36, Z_CHECK, i);
        double tw = nx_text(cr, tasks[i].text, 14, false, tasks[i].done ? subtle : text,
                            bx + 32, ty, CARD_W - 110, 36, NX_LEFT);
        if (tasks[i].done) {   /* texte barré */
            cairo_rectangle(cr, bx + 32, ty + 18, fmin(tw, CARD_W - 110), 1.3);
            nx_set(cr, subtle);
            cairo_fill(cr);
        }
        if (hover_index == i) {   /* croix de suppression au survol */
            double dx = CARD_X + CARD_W - 40, dy = ty + 9;
            nx_set(cr, nx_alpha(text, hover_kind == Z_DELETE ? 0.9 : 0.45));
            cairo_set_line_width(cr, 1.8);
            cairo_move_to(cr, dx + 4, dy + 4); cairo_line_to(cr, dx + 14, dy + 14);
            cairo_move_to(cr, dx + 14, dy + 4); cairo_line_to(cr, dx + 4, dy + 14);
            cairo_stroke(cr);
        }
        add_zone(CARD_X + CARD_W - 48, ty, 36, 36, Z_DELETE, i);
    }
    if (adding) {
        nx_rounded_rect(cr, CARD_X + 14, ty + 4, CARD_W - 28, 32, 8);
        nx_set(cr, wfg(0.08));
        cairo_fill_preserve(cr);
        nx_set(cr, accent);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        double tw = nx_text(cr, input[0] ? input : "", 14, false, text, CARD_X + 26, ty + 4, CARD_W - 60, 32, NX_LEFT);
        if (!input[0]) nx_text(cr, "Écris ta tâche puis Entrée…", 13.5, false, subtle, CARD_X + 26, ty + 4, CARD_W - 60, 32, NX_LEFT);
        cairo_rectangle(cr, CARD_X + 27 + (input[0] ? tw : 0), ty + 11, 1.5, 18);
        nx_set(cr, accent);
        cairo_fill(cr);
    } else if (ntasks < MAX_TASKS) {
        bool hot = hover_kind == Z_ADD;
        nx_rounded_rect(cr, CARD_X + 20, ty + 8, 20, 20, 5);
        nx_set(cr, nx_alpha(text, hot ? 0.7 : 0.4));
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
        nx_text(cr, "Ajouter une tâche…", 14, false, hot ? text : subtle, CARD_X + 52, ty + 2, CARD_W - 70, 34, NX_LEFT);
        add_zone(CARD_X + 12, ty, CARD_W - 24, 38, Z_ADD, -1);
    } else {
        nx_text(cr, "Liste pleine : coche ou supprime une tâche", 12.5, false, subtle,
                CARD_X + 20, ty + 2, CARD_W - 40, 34, NX_LEFT);
    }

    /* --- Mode simplifié et taille du texte --- */
    y += ch + GAP;
    ch = H_SIMPLE;
    card(cr, y, ch);
    nx_icon(cr, "access", CARD_X + 18, y + 16, 26, text);
    nx_text(cr, "Mode simplifié", 15, true, text, CARD_X + 56, y + 12, 0, 34, NX_LEFT);
    const char *bl = nx_conf.simple ? "Désactiver" : "Activer";
    double bw = nx_text_width(cr, bl, 13.5, true) + 34, bx = CARD_X + CARD_W - 16 - bw;
    nx_rounded_rect(cr, bx, y + 13, bw, 32, 16);
    nx_set(cr, nx_conf.simple ? wfg(hover_kind == Z_SIMPLE ? 0.22 : 0.14)
                              : (hover_kind == Z_SIMPLE ? nx_mix(accent, nx_rgb("#ffffff"), 0.2) : accent));
    cairo_fill(cr);
    nx_text(cr, bl, 13.5, true, nx_conf.simple ? text : nx_rgb("#ffffff"), bx, y + 13, bw, 32, NX_CENTER);
    add_zone(bx, y + 13, bw, 32, Z_SIMPLE, -1);
    nx_text(cr, "Aa", 18, true, text, CARD_X + 16, y + 66, 34, 34, NX_CENTER);
    nx_text(cr, "Taille du texte", 15, true, text, CARD_X + 56, y + 66, 0, 34, NX_LEFT);
    static const int sizes[] = { 100, 125, 150 };
    double sgw = 40, sx0 = CARD_X + CARD_W - 16 - 3 * sgw;
    nx_rounded_rect(cr, sx0 - 3, y + 63, 3 * sgw + 6, 40, 12);
    nx_set(cr, wfg(0.08));
    cairo_fill(cr);
    for (int i = 0; i < 3; i++) {
        bool on = nx_conf.text_scale == sizes[i];
        double sx = sx0 + i * sgw;
        if (on || (hover_kind == Z_TEXT && hover_index == i)) {
            nx_rounded_rect(cr, sx + 2, y + 66, sgw - 4, 34, 10);
            nx_set(cr, on ? accent : wfg(0.12));
            cairo_fill(cr);
        }
        double saved = nx_scale;
        nx_scale = 1;
        nx_text(cr, "A", 13 + i * 3.5, true, on ? nx_rgb("#ffffff") : text, sx, y + 66, sgw, 34, NX_CENTER);
        nx_scale = saved;
        add_zone(sx, y + 63, sgw, 40, Z_TEXT, i);
    }

    /* Poignées : déplacer (points en haut) et agrandir (coin en bas à gauche) */
    if (grip_hover || drag) {
        for (int i = 0; i < 6; i++) {
            cairo_arc(cr, W_WIDTH / 2.0 - 10 + (i % 3) * 10, 18 + (i / 3) * 6, 1.8, 0, 2 * M_PI);
            nx_set(cr, nx_alpha(text, 0.7));
            cairo_fill(cr);
        }
        double gy = content_h - 14;
        cairo_set_line_width(cr, 2);
        for (int i = 0; i < 3; i++) {
            cairo_move_to(cr, CARD_X + 4, gy - 16 + i * 6);
            cairo_line_to(cr, CARD_X + 20 - i * 6, gy);
        }
        nx_set(cr, drag == DRAG_RESIZE || grip_hover == 2 ? accent : nx_alpha(text, 0.6));
        cairo_stroke(cr);
    }
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    cur_h = height;
    double ts = nx_scale;
    Sd = fmin(S * ts, fmin(width / (double)W_WIDTH, height / (double)content_h));
    if (Sd < 0.5) Sd = 0.5;
    nx_scale = 1;                 /* (taille du texte déjà comprise dans Sd) */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_save(cr);
    cairo_translate(cr, width - W_WIDTH * Sd, 0);      /* collés au bord droit */
    cairo_scale(cr, Sd, Sd);
    draw_logical(cr);
    cairo_restore(cr);
    nx_scale = ts;
}

/* ---------- Souris et clavier ---------- */

static struct zone *find(double x, double y)
{
    for (int i = nzones - 1; i >= 0; i--)
        if (x >= zones[i].x && x < zones[i].x + zones[i].w && y >= zones[i].y && y < zones[i].y + zones[i].h)
            return &zones[i];
    return NULL;
}

static void pointer_logical(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct zone *z = find(x, y);
    if (ev == NX_MOTION || ev == NX_LEAVE) {
        int k = z && ev == NX_MOTION ? z->kind : Z_NONE, i = z && ev == NX_MOTION ? z->index : -1;
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
    } else if (ev == NX_RELEASE && value == BTN_LEFT && z) {
        if (z->kind == Z_CHECK) {
            tasks[z->index].done = !tasks[z->index].done;
            tasks_save();
        } else if (z->kind == Z_DELETE) {
            memmove(&tasks[z->index], &tasks[z->index + 1], (ntasks - z->index - 1) * sizeof tasks[0]);
            ntasks--;
            tasks_save();
            hover_index = -1;
        } else if (z->kind == Z_ADD) {
            adding = true;
            input[0] = 0;
        } else if (z->kind == Z_SHORT) {
            char q[1100], cmd[1200];
            nx_shell_quote(q, sizeof q, shorts[z->index].path);
            snprintf(cmd, sizeof cmd, "nexus-files %s", q);
            nx_spawn(cmd);
        } else if (z->kind == Z_SHORTADD) {
            nx_spawn("nexus-notify -i files 'Ajouter un raccourci' "
                     "'Dans Fichiers, clic droit sur un dossier → « Ajouter aux raccourcis ».'; nexus-files");
        } else if (z->kind == Z_SIMPLE || z->kind == Z_TEXT) {
            if (z->kind == Z_SIMPLE) nx_conf.simple = !nx_conf.simple;
            else nx_conf.text_scale = (int[]){ 100, 125, 150 }[z->index];
            nx_config_save(&nx_conf);
            nx_notify_reload();       /* barre, menu… et nous-mêmes (reload) */
        }
        nx_redraw(w);
    }
}

/* Coordonnées « écran » indépendantes de la position et de la taille de la
 * surface (ancrée en haut à droite) : X = x - marge droite - largeur, Y = y + marge haut */
static double screen_x(double x) { return x - nx_conf.widget_mr - cur_w; }
static double screen_y(double y) { return y + nx_conf.widget_mt; }

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    double lx = (x - (cur_w - W_WIDTH * Sd)) / Sd, ly = y / Sd;
    if (drag) {
        if (ev == NX_MOTION) {
            double dx = screen_x(x) - drag_x0, dy = screen_y(y) - drag_y0;
            if (drag == DRAG_MOVE) {
                int mr = drag_mr0 - (int)dx, mt = drag_mt0 + (int)dy;
                nx_conf.widget_mr = mr < 0 ? 0 : mr;
                nx_conf.widget_mt = mt < 0 ? 0 : mt;
            } else {
                double nw = drag_w0 - dx;
                int sc = (int)(100 * nw / (W_WIDTH * nx_scale) + 0.5);
                nx_conf.widget_scale = sc < 70 ? 70 : sc > 170 ? 170 : sc;
            }
            apply_geometry();
        } else if (ev == NX_RELEASE) {
            drag = DRAG_NONE;
            nx_config_save(&nx_conf);
            nx_redraw(w);
        }
        return;
    }
    struct zone *z = find(lx, ly);
    bool on_card = lx >= CARD_X && lx <= CARD_X + CARD_W && ly >= 0 && ly <= content_h;
    int g = ev == NX_LEAVE ? 0 : (lx < CARD_X + 26 && ly > content_h - 40) || lx < CARD_X + 6 ? 2 : on_card ? 1 : 0;
    if (g != grip_hover) { grip_hover = g; nx_redraw(w); }
    if (ev == NX_PRESS && value == BTN_LEFT && g && (g == 2 || !z) && !adding) {
        drag = g == 2 ? DRAG_RESIZE : DRAG_MOVE;
        drag_x0 = screen_x(x);
        drag_y0 = screen_y(y);
        drag_mr0 = nx_conf.widget_mr;
        drag_mt0 = nx_conf.widget_mt;
        drag_w0 = cur_w;
        return;
    }
    if (g == 2 && (ev == NX_PRESS || ev == NX_RELEASE)) return;
    pointer_logical(w, ev, lx, ly, value);
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (!adding) return;
    if (sym == XKB_KEY_Escape) adding = false;
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (input[0] && ntasks < MAX_TASKS) {
            snprintf(tasks[ntasks].text, sizeof tasks[ntasks].text, "%s", input);
            tasks[ntasks].done = false;
            ntasks++;
            tasks_save();
        }
        adding = false;
    } else if (sym == XKB_KEY_BackSpace) {
        size_t n = strlen(input);
        while (n > 0 && ((unsigned char)input[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        input[n] = 0;
    } else if (utf8[0] && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
               strlen(input) + strlen(utf8) < sizeof input - 1) {
        strcat(input, utf8);
    } else return;
    nx_redraw(w);
}

static int timeout(void) { return 2000; }
static void tick(void) { sample(); nx_redraw(win); }
static void reload(void)
{
    if (!nx_conf.widgets) { nx_quit(); return; }
    shortcuts_load();
    content_h = compute_content_h();
    apply_geometry();
}

int widgets_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    nx_config_load(&nx_conf);
    if (!nx_conf.widgets) return 0;   /* désactivés dans Paramètres */
    struct passwd *pw = getpwuid(getuid());
    snprintf(username, sizeof username, "%s", getuid() == 0 ? "Administrateur" : pw ? pw->pw_name : "");
    tasks_load();
    shortcuts_load();
    sample();
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    /* Clavier "à la demande" : la saisie d'une tâche reçoit le clavier après un clic */
    S = nx_conf.widget_scale / 100.0;
    content_h = compute_content_h();
    cur_w = (int)(W_WIDTH * S + 0.5);
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          cur_w, 0, 0,
                          ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND, "nexus-widgets");
    if (!win) return 1;
    apply_geometry();   /* position mémorisée */
    nx.timeout = timeout;
    nx.tick = tick;
    nx.reload = reload;
    nx_run();
    return 0;
}
