/* nexus-calculator — la calculatrice de Nexus OS.
 *
 * Saisie d'une expression complète (priorités respectées : 2 + 3 × 4 = 14),
 * parenthèses, pourcentage (50 % = 0,5 ; 200 + 10 % = 220), racine carrée,
 * carré, changement de signe. Le résultat s'affiche pendant la frappe ;
 * « = » le valide et l'ajoute à l'historique (cliquer sur une ligne de
 * l'historique la reprend). Virgule décimale à la française.
 * Clavier : chiffres, + - * / x, ( ), virgule ou point, %, r (racine),
 * Entrée ou = (résultat), Retour arrière, Échap (tout effacer).
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>

static struct nx_window *win;

static char expr[256];               /* expression (UTF-8 : × ÷ − √ ²) */
static char shown_result[64];        /* aperçu du résultat */
static bool just_done;               /* on vient d'appuyer sur = */
static char hist_e[8][256], hist_r[8][64];
static int nhist;

/* ======================= Calcul ======================= */

static const char *p;                 /* position de lecture */
static bool err;

static void skip(void) { while (*p == ' ') p++; }

/* Les symboles UTF-8 sont convertis en ASCII avant le calcul */
static void to_ascii(const char *in, char *out, size_t n)
{
    size_t o = 0;
    while (*in && o + 2 < n) {
        if (!strncmp(in, "×", 2)) { out[o++] = '*'; in += 2; }
        else if (!strncmp(in, "÷", 2)) { out[o++] = '/'; in += 2; }
        else if (!strncmp(in, "−", 3)) { out[o++] = '-'; in += 3; }
        else if (!strncmp(in, "√", 3)) { out[o++] = 'r'; in += 3; }
        else if (!strncmp(in, "²", 2)) { out[o++] = 'q'; in += 2; }
        else if (*in == ',') { out[o++] = '.'; in++; }
        else out[o++] = *in++;
    }
    out[o] = 0;
}

static double expr_sum(void);

static double primary(void)
{
    skip();
    double v;
    if (*p == '(') {
        p++;
        v = expr_sum();
        skip();
        if (*p == ')') p++;          /* parenthèse fermante oubliée : tolérée */
    } else if (*p == 'r') {
        p++;
        double a = primary();
        if (a < 0) err = true;
        v = sqrt(a);
    } else if (*p == '-') {
        p++;
        v = -primary();
    } else if (*p == '+') {
        p++;
        v = primary();
    } else if (isdigit((unsigned char)*p) || *p == '.') {
        char *end;
        v = strtod(p, &end);
        if (end == p) { err = true; return 0; }
        p = end;
    } else {
        err = true;
        return 0;
    }
    /* suffixes : carré, pourcentage */
    for (;;) {
        skip();
        if (*p == 'q') { p++; v = v * v; }
        else if (*p == '%') { p++; v = v / 100; }
        else break;
    }
    return v;
}

static double term(void)
{
    double v = primary();
    for (;;) {
        skip();
        if (*p == '*') { p++; v *= primary(); }
        else if (*p == '/') {
            p++;
            double d = primary();
            if (d == 0) err = true;
            v /= d;
        } else if (*p == '(' || *p == 'r') {
            v *= primary();          /* multiplication implicite : 2(3+4) */
        } else break;
    }
    return v;
}

static double expr_sum(void)
{
    double v = term();
    for (;;) {
        skip();
        if (*p == '+' || *p == '-') {
            char op = *p++;
            const char *save = p;
            double t = term();
            /* « 200 + 10 % » = 200 + 10 % de 200 (comme une calculatrice de poche) */
            const char *q = p - 1;
            while (q > save && *q == ' ') q--;
            if (*q == '%' && !strchr(save, '*') && !strchr(save, '/')) t = v * t;
            v = op == '+' ? v + t : v - t;
        } else break;
    }
    return v;
}

static bool evaluate(const char *e, double *out)
{
    static char a[256];
    to_ascii(e, a, sizeof a);
    p = a;
    err = false;
    skip();
    if (!*p) return false;
    double v = expr_sum();
    skip();
    if (*p || err || isnan(v) || isinf(v)) return false;
    *out = v;
    return true;
}

static void format(double v, char *out, size_t n)
{
    if (fabs(v) < 1e-12) v = 0;
    snprintf(out, n, "%.12g", v);
    /* virgule décimale, signe moins typographique */
    char tmp[64];
    size_t o = 0;
    for (const char *s = out; *s && o + 4 < sizeof tmp; s++) {
        if (*s == '.') tmp[o++] = ',';
        else if (*s == '-' && s == out) { memcpy(tmp + o, "−", 3); o += 3; }
        else tmp[o++] = *s;
    }
    tmp[o] = 0;
    snprintf(out, n, "%s", tmp);
}

static void update_preview(void)
{
    double v;
    if (evaluate(expr, &v)) format(v, shown_result, sizeof shown_result);
    else shown_result[0] = 0;
}

/* ======================= Saisie ======================= */

static void append(const char *s)
{
    bool is_op = !strcmp(s, " + ") || !strcmp(s, " − ") || !strcmp(s, " × ") || !strcmp(s, " ÷ ") ||
                 !strcmp(s, "²") || !strcmp(s, "%");
    if (just_done) {
        /* après « = » : un opérateur continue avec le résultat, un chiffre recommence */
        if (is_op) snprintf(expr, sizeof expr, "%s", hist_r[0]);
        else expr[0] = 0;
        just_done = false;
    }
    if (strlen(expr) + strlen(s) < sizeof expr - 1) strcat(expr, s);
    update_preview();
}

static void backspace(void)
{
    just_done = false;
    size_t n = strlen(expr);
    while (n > 0 && expr[n - 1] == ' ') n--;
    if (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)expr[n] & 0xC0) == 0x80) n--;
    }
    while (n > 0 && expr[n - 1] == ' ') n--;
    expr[n] = 0;
    update_preview();
}

static void clear_all(void)
{
    expr[0] = 0;
    shown_result[0] = 0;
    just_done = false;
}

static void negate(void)
{
    /* change le signe de tout le calcul en cours */
    char tmp[256];
    if (!expr[0]) return;
    if (!strncmp(expr, "−(", 4) && expr[strlen(expr) - 1] == ')') {
        snprintf(tmp, sizeof tmp, "%.*s", (int)strlen(expr) - 5, expr + 4);
    } else {
        snprintf(tmp, sizeof tmp, "−(%s)", expr);
    }
    snprintf(expr, sizeof expr, "%s", tmp);
    just_done = false;
    update_preview();
}

static void equals(void)
{
    double v;
    if (!evaluate(expr, &v)) {
        snprintf(shown_result, sizeof shown_result, expr[0] ? "Erreur" : "");
        return;
    }
    char r[64];
    format(v, r, sizeof r);
    memmove(hist_e + 1, hist_e, sizeof hist_e[0] * 7);
    memmove(hist_r + 1, hist_r, sizeof hist_r[0] * 7);
    snprintf(hist_e[0], sizeof hist_e[0], "%s", expr);
    snprintf(hist_r[0], sizeof hist_r[0], "%s", r);
    if (nhist < 8) nhist++;
    snprintf(expr, sizeof expr, "%s", r);
    shown_result[0] = 0;
    just_done = true;
}

/* ======================= Boutons ======================= */

struct key { const char *label, *ins; int kind; };   /* kind : 0 chiffre, 1 opérateur, 2 action, 3 égal */
static const struct key keys[6][4] = {
    { { "C", "C", 2 },  { "(", "(", 1 },  { ")", ")", 1 },  { "⌫", "B", 2 } },
    { { "√", "√", 1 },  { "x²", "²", 1 }, { "%", "%", 1 },  { "÷", " ÷ ", 1 } },
    { { "7", "7", 0 },  { "8", "8", 0 },  { "9", "9", 0 },  { "×", " × ", 1 } },
    { { "4", "4", 0 },  { "5", "5", 0 },  { "6", "6", 0 },  { "−", " − ", 1 } },
    { { "1", "1", 0 },  { "2", "2", 0 },  { "3", "3", 0 },  { "+", " + ", 1 } },
    { { "±", "N", 2 },  { "0", "0", 0 },  { ",", ",", 0 },  { "=", "=", 3 } },
};

static void press(const struct key *k)
{
    if (!strcmp(k->ins, "C")) clear_all();
    else if (!strcmp(k->ins, "B")) backspace();
    else if (!strcmp(k->ins, "N")) negate();
    else if (!strcmp(k->ins, "=")) equals();
    else append(k->ins);
}

/* ======================= Dessin ======================= */

enum kind { K_KEY, K_HIST };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[48];
static int nrects;
static int hover_kind = -1, hover_index = -1, pressed = -1;

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 48) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
}

#define HIST_W 220

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    nx_color accent = nx_accent_color(), white = nx_rgb("#ffffff");
    nx_color text = nx_rgb(NX_GLASS_TEXT), subtle = nx_rgb(NX_GLASS_SUBTLE);
    cairo_set_source_rgb(cr, 0.10, 0.12, 0.16);
    cairo_paint(cr);

    bool show_hist = width >= 560;
    double cw = show_hist ? width - HIST_W : width;

    /* Écran */
    double pad = 16, sh = 128;
    nx_rounded_rect(cr, pad, pad, cw - 2 * pad, sh, 14);
    cairo_set_source_rgb(cr, 0.06, 0.07, 0.10);
    cairo_fill(cr);
    const char *main_txt = expr[0] ? expr : "0";
    /* taille adaptée à la longueur */
    double fs = 38;
    while (fs > 16 && nx_text_width(cr, main_txt, fs, true) > cw - 2 * pad - 32) fs -= 2;
    if (just_done && nhist) {
        char top[300];
        snprintf(top, sizeof top, "%s =", hist_e[0]);
        nx_text(cr, top, 15, false, subtle, pad + 16, pad + 12, cw - 2 * pad - 32, 30, NX_RIGHT);
    } else if (shown_result[0] && strcmp(shown_result, main_txt)) {
        char top[80];
        snprintf(top, sizeof top, "= %s", shown_result);
        nx_text(cr, top, 17, false, !strcmp(shown_result, "Erreur") ? nx_rgb("#ff8a8a") : subtle,
                pad + 16, pad + 12, cw - 2 * pad - 32, 30, NX_RIGHT);
    }
    nx_text(cr, main_txt, fs, true, white, pad + 16, pad + 44, cw - 2 * pad - 32, 72, NX_RIGHT);

    /* Touches */
    double ky = pad + sh + 14, kh = height - ky - pad, gap = 8;
    double bw = (cw - 2 * pad - 3 * gap) / 4, bh = (kh - 5 * gap) / 6;
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 4; c++) {
            const struct key *k = &keys[r][c];
            int idx = r * 4 + c;
            double x = pad + c * (bw + gap), y = ky + r * (bh + gap);
            nx_rounded_rect(cr, x, y, bw, bh, 12);
            nx_color bg = k->kind == 3 ? accent : k->kind == 1 ? nx_rgb("#2c3240") :
                          k->kind == 2 ? nx_rgb("#3a3f4d") : nx_rgb("#232834");
            if (idx == pressed) bg = nx_mix(bg, white, 0.25);
            else if (hover_kind == K_KEY && hover_index == idx) bg = nx_mix(bg, white, 0.1);
            nx_set(cr, bg);
            cairo_fill(cr);
            nx_color fg = k->kind == 1 ? nx_mix(accent, white, 0.45) : white;
            if (!strcmp(k->ins, "C")) fg = nx_rgb("#ff8a8a");
            nx_text(cr, k->label, k->kind == 0 ? 22 : 20, k->kind != 0, fg, x, y, bw, bh, NX_CENTER);
            add_rect(x, y, bw, bh, K_KEY, idx);
        }

    /* Historique */
    if (show_hist) {
        double hx = cw, hy = pad;
        cairo_rectangle(cr, hx, 0, HIST_W, height);
        cairo_set_source_rgb(cr, 0.08, 0.09, 0.13);
        cairo_fill(cr);
        nx_text(cr, "Historique", 14, true, text, hx + 16, hy, HIST_W - 32, 30, NX_LEFT);
        if (!nhist) nx_text(cr, "Tes calculs s'afficheront ici.", 12.5, false, subtle, hx + 16, hy + 36, HIST_W - 32, 22, NX_LEFT);
        for (int i = 0; i < nhist; i++) {
            double y = hy + 40 + i * 62;
            if (hover_kind == K_HIST && hover_index == i) {
                nx_rounded_rect(cr, hx + 8, y - 4, HIST_W - 16, 58, 8);
                cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
                cairo_fill(cr);
            }
            nx_text(cr, hist_e[i], 12.5, false, subtle, hx + 16, y, HIST_W - 32, 22, NX_RIGHT);
            char r[80];
            snprintf(r, sizeof r, "= %s", hist_r[i]);
            nx_text(cr, r, 17, true, text, hx + 16, y + 22, HIST_W - 32, 28, NX_RIGHT);
            add_rect(hx + 8, y - 4, HIST_W - 16, 58, K_HIST, i);
        }
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
    if (ev == NX_MOTION || ev == NX_LEAVE) {
        if (ev == NX_LEAVE) k = i = -1;
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
        return;
    }
    if (value != BTN_LEFT) return;
    if (ev == NX_PRESS && r && r->kind == K_KEY) { pressed = r->index; nx_redraw(w); }
    if (ev == NX_RELEASE) {
        if (r && r->kind == K_KEY && r->index == pressed) press(&keys[r->index / 4][r->index % 4]);
        else if (r && r->kind == K_HIST) {
            snprintf(expr, sizeof expr, "%s", hist_r[r->index]);
            just_done = false;
            update_preview();
        }
        pressed = -1;
        nx_redraw(w);
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (nx_mod_ctrl()) {
        if (sym == XKB_KEY_c || sym == XKB_KEY_C) {
            char r[64];
            double v;
            if (evaluate(expr, &v)) {
                snprintf(r, sizeof r, "%.12g", v);
                for (char *c = r; *c; c++) if (*c == '.') *c = ',';
                nx_clipboard_set(r);
            }
        } else if (sym == XKB_KEY_v || sym == XKB_KEY_V) {
            char *t = nx_clipboard_get();
            if (t) {
                just_done = false;
                /* garder chiffres, opérateurs, parenthèses */
                for (char *c = t; *c && strlen(expr) < sizeof expr - 4; c++) {
                    char s[2] = { *c, 0 };
                    if (isdigit((unsigned char)*c) || strchr("()%,.", *c)) append(*c == '.' ? "," : s);
                    else if (*c == '+') append(" + ");
                    else if (*c == '-') append(" − ");
                    else if (*c == '*' || *c == 'x') append(" × ");
                    else if (*c == '/') append(" ÷ ");
                }
                free(t);
            }
        } else return;
        nx_redraw(w);
        return;
    }
    switch (sym) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: equals(); break;
    case XKB_KEY_BackSpace: backspace(); break;
    case XKB_KEY_Escape: case XKB_KEY_Delete: clear_all(); break;
    case XKB_KEY_KP_Add: append(" + "); break;
    case XKB_KEY_KP_Subtract: append(" − "); break;
    case XKB_KEY_KP_Multiply: append(" × "); break;
    case XKB_KEY_KP_Divide: append(" ÷ "); break;
    case XKB_KEY_KP_Decimal: case XKB_KEY_KP_Separator: append(","); break;
    default:
        if (!utf8 || !utf8[0]) return;
        if (utf8[1] == 0) {
            char c = utf8[0];
            if (isdigit((unsigned char)c)) { char s[2] = { c, 0 }; append(s); }
            else if (c == '+') append(" + ");
            else if (c == '-') append(" − ");
            else if (c == '*' || c == 'x' || c == 'X') append(" × ");
            else if (c == '/' || c == ':') append(" ÷ ");
            else if (c == ',' || c == '.') append(",");
            else if (c == '(' || c == ')') { char s[2] = { c, 0 }; append(s); }
            else if (c == '%') append("%");
            else if (c == '=') equals();
            else if (c == 'r' || c == 'R') append("√");
            else return;
        } else if (!strcmp(utf8, "²")) append("²");
        else if (!strcmp(utf8, "×")) append(" × ");
        else if (!strcmp(utf8, "÷")) append(" ÷ ");
        else return;
    }
    nx_redraw(w);
}

int calc_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_toplevel_window(h, "Calculatrice", "nexus-calculator", 600, 560);
    if (!win) return 1;
    nx_run();
    return 0;
}
