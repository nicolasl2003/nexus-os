/* nexus-widgets — les widgets du bureau (en haut à droite) :
 *   - horloge, date et message d'accueil ;
 *   - performances : processeur, mémoire, disque + courbe du processeur ;
 *   - "Mes tâches" : petite liste à cocher (~/.config/nexus/taches.txt).
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

static double S = 1;                 /* échelle (widget_scale / 100) */
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
    int w = (int)(W_WIDTH * S + 0.5), h = (int)(content_h * S + 0.5);
    zwlr_layer_surface_v1_set_margin(win->layer, nx_conf.widget_mt, nx_conf.widget_mr, 0, 0);
    if (w != cur_w || h != cur_h) {
        cur_w = w;
        cur_h = h;
        zwlr_layer_surface_v1_set_size(win->layer, w, h);
    }
    wl_surface_commit(win->surface);
    nx_redraw(win);
}

static int compute_content_h(void) { return 12 + 118 + 14 + 222 + 14 + 66 + ntasks * 36 + 44 + 12; }

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

enum { Z_NONE, Z_CHECK, Z_DELETE, Z_ADD };
struct zone { double x, y, w, h; int kind, index; };
static struct zone zones[32];
static int nzones;
static int hover_kind, hover_index = -1;

static void add_zone(double x, double y, double w, double h, int kind, int index)
{
    if (nzones < 32) zones[nzones++] = (struct zone){ x, y, w, h, kind, index };
}

/* ---------- Dessin ---------- */

static const char *jours[] = { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" };
static const char *mois[] = { "janvier", "février", "mars", "avril", "mai", "juin", "juillet",
                              "août", "septembre", "octobre", "novembre", "décembre" };

static void ring(cairo_t *cr, double cx, double cy, double r, double pct, nx_color c, const char *label)
{
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 7);
    cairo_stroke(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, -M_PI / 2, -M_PI / 2 + 2 * M_PI * fmin(pct, 100) / 100);
    nx_set(cr, c);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cr);
    char t[16];
    snprintf(t, sizeof t, "%.0f%%", pct);
    nx_text(cr, t, 17, true, nx_rgb(NX_GLASS_TEXT), cx - r, cy - 13, 2 * r, 26, NX_CENTER);
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
    text_c = nx_conf.widget_style == 1 ? nx_rgb(NX_TEXT) : nx_rgb(NX_GLASS_TEXT);
    subtle_c = nx_conf.widget_style == 1 ? nx_rgb(NX_SUBTLE) :
               nx_conf.widget_style == 2 ? nx_alpha(nx_rgb("#ffffff"), 0.75) : nx_rgb(NX_GLASS_SUBTLE);
    nx_color text = text_c, subtle = subtle_c, accent = nx_accent_color();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    /* --- Horloge --- */
    double y = 12, ch = 118;
    card(cr, y, ch);
    char hm[8], date[64];
    snprintf(hm, sizeof hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(date, sizeof date, "%s %d %s %d", jours[tm.tm_wday], tm.tm_mday, mois[tm.tm_mon], tm.tm_year + 1900);
    nx_text(cr, hm, 50, true, text, CARD_X + 20, y + 14, 0, 62, NX_LEFT);
    nx_text(cr, date, 13.5, false, subtle, CARD_X + 22, y + 78, 210, 24, NX_LEFT);
    cairo_rectangle(cr, CARD_X + 228, y + 20, 1, ch - 40);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_fill(cr);
    bool day = tm.tm_hour >= 7 && tm.tm_hour < 19;
    const char *hello = tm.tm_hour < 5 ? "Bonne nuit" : tm.tm_hour < 12 ? "Bonjour" :
                        tm.tm_hour < 18 ? "Bon après-midi" : "Bonsoir";
    draw_sun_or_moon(cr, CARD_X + 270, y + 16, 44, day);
    nx_text(cr, hello, 12.5, true, text, CARD_X + 230, y + 66, CARD_W - 236, 22, NX_CENTER);
    nx_text(cr, username, 12.5, false, subtle, CARD_X + 232, y + 88, CARD_W - 240, 20, NX_CENTER);

    /* --- Performances --- */
    y += ch + 14;
    ch = 222;
    card(cr, y, ch);
    nx_text(cr, "Performances système", 15, true, text, CARD_X + 20, y + 14, 0, 26, NX_LEFT);
    nx_icon(cr, "monitor", CARD_X + CARD_W - 44, y + 14, 24, text);
    double rx = CARD_X + CARD_W / 6.0, ry = y + 92;
    ring(cr, rx, ry, 34, cpu_pct, nx_rgb("#3d8bff"), "Processeur");
    ring(cr, rx + CARD_W / 3.0, ry, 34, ram_pct, nx_rgb("#a86bff"), "Mémoire");
    ring(cr, rx + 2 * CARD_W / 3.0, ry, 34, disk_pct, nx_rgb("#2fd38a"), "Disque");
    /* courbe de l'activité du processeur */
    double gx = CARD_X + 18, gy = y + 156, gw = CARD_W - 36, gh = 50;
    cairo_new_path(cr);
    for (int i = 0; i < 48; i++) {
        double px = gx + gw * i / 47.0, py = gy + gh - gh * fmin(history[i], 100) / 100 * 0.9 - 2;
        if (i == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
    }
    cairo_path_t *line = cairo_copy_path(cr);
    cairo_line_to(cr, gx + gw, gy + gh);
    cairo_line_to(cr, gx, gy + gh);
    cairo_close_path(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, gy, 0, gy + gh);
    cairo_pattern_add_color_stop_rgba(g, 0, 0.24, 0.55, 1, 0.35);
    cairo_pattern_add_color_stop_rgba(g, 1, 0.66, 0.42, 1, 0.02);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_new_path(cr);
    cairo_append_path(cr, line);
    cairo_path_destroy(line);
    cairo_set_source_rgba(cr, 0.45, 0.7, 1, 0.9);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);

    /* --- Mes tâches --- */
    y += ch + 14;
    ch = 66 + ntasks * 36 + 44;
    card(cr, y, ch);
    nx_text(cr, "Mes tâches", 15, true, text, CARD_X + 20, y + 14, 0, 26, NX_LEFT);
    double ty = y + 52;
    for (int i = 0; i < ntasks; i++, ty += 36) {
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
        cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
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
        nx_text(cr, "+", 20, false, hot ? text : subtle, CARD_X + 20, ty + 2, 20, 34, NX_CENTER);
        nx_text(cr, "Ajouter une tâche…", 14, false, hot ? text : subtle, CARD_X + 50, ty + 2, CARD_W - 70, 34, NX_LEFT);
        add_zone(CARD_X + 12, ty, CARD_W - 24, 38, Z_ADD, -1);
    } else {
        nx_text(cr, "Liste pleine : coche ou supprime une tâche", 12.5, false, subtle,
                CARD_X + 20, ty + 2, CARD_W - 40, 34, NX_LEFT);
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
    (void)w; (void)width; (void)height;
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_save(cr);
    cairo_scale(cr, S, S);
    draw_logical(cr);
    cairo_restore(cr);
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
    double lx = x / S, ly = y / S;
    if (drag) {
        if (ev == NX_MOTION) {
            double dx = screen_x(x) - drag_x0, dy = screen_y(y) - drag_y0;
            if (drag == DRAG_MOVE) {
                int mr = drag_mr0 - (int)dx, mt = drag_mt0 + (int)dy;
                nx_conf.widget_mr = mr < 0 ? 0 : mr;
                nx_conf.widget_mt = mt < 0 ? 0 : mt;
            } else {
                double nw = drag_w0 - dx;
                int sc = (int)(100 * nw / W_WIDTH + 0.5);
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
    bool on_card = lx >= CARD_X && lx <= CARD_X + CARD_W;
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
    sample();
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    /* Clavier "à la demande" : la saisie d'une tâche reçoit le clavier après un clic */
    S = nx_conf.widget_scale / 100.0;
    content_h = compute_content_h();
    cur_w = (int)(W_WIDTH * S + 0.5);
    cur_h = (int)(content_h * S + 0.5);
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          cur_w, cur_h, 0,
                          ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND, "nexus-widgets");
    if (!win) return 1;
    apply_geometry();   /* position mémorisée */
    nx.timeout = timeout;
    nx.tick = tick;
    nx.reload = reload;
    nx_run();
    return 0;
}
