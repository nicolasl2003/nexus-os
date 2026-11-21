/* nexus-editor — l'éditeur de texte graphique de Nexus OS.
 *
 *   nexus-editor [fichier]
 *
 * Barre d'outils (Nouveau, Ouvrir, Enregistrer, Rechercher, taille du texte),
 * numéros de ligne, police à chasse fixe, barre d'état (ligne, colonne).
 * Clavier : flèches (Maj : sélectionner, Ctrl : mot par mot), Origine/Fin,
 * Page préc./suiv., Ctrl+A/C/X/V, Ctrl+Z (annuler), Ctrl+Y (rétablir),
 * Ctrl+S (enregistrer), Ctrl+Maj+S (enregistrer sous), Ctrl+O, Ctrl+N,
 * Ctrl+F (rechercher, Entrée : suivant), Ctrl+molette ou Ctrl+= / Ctrl+- (zoom).
 * Souris : clic pour placer le curseur, glisser pour sélectionner, double-clic
 * pour un mot, molette pour défiler.
 * Fermer avec des modifications non enregistrées demande confirmation.
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>

#define TOOL_H   50
#define FIND_H   46
#define STATUS_H 28
#define MAX_FILE (8 * 1024 * 1024)

static struct nx_window *win;

/* ======================= Texte ======================= */

struct line { char *s; int len, cap; };
static struct line *L;
static int nl, capl;
static int cy, cx;               /* curseur : ligne, octet dans la ligne */
static int ay, ax;               /* ancre de la sélection */
static bool selecting;           /* sélection active (ancre ≠ curseur possible) */
static int top;                  /* première ligne affichée */
static double hscroll;           /* défilement horizontal (px) */
static char path[PATH_MAX];      /* fichier ouvert ("" : nouveau document) */
static bool modified;
static char status_msg[200];
static time_t status_until;

static void set_status(const char *m)
{
    snprintf(status_msg, sizeof status_msg, "%s", m);
    status_until = time(NULL) + 4;
}

static void line_reserve(struct line *l, int need)
{
    if (need + 1 <= l->cap) return;
    int c = l->cap ? l->cap : 32;
    while (c < need + 1) c *= 2;
    l->s = realloc(l->s, c);
    l->cap = c;
}

static void lines_reserve(int n)
{
    if (n <= capl) return;
    int c = capl ? capl : 256;
    while (c < n) c *= 2;
    L = realloc(L, c * sizeof *L);
    memset(L + capl, 0, (c - capl) * sizeof *L);
    capl = c;
}

static void insert_line(int at, const char *s, int len)
{
    lines_reserve(nl + 1);
    memmove(L + at + 1, L + at, (nl - at) * sizeof *L);
    L[at] = (struct line){ 0 };
    line_reserve(&L[at], len);
    memcpy(L[at].s, s, len);
    L[at].len = len;
    L[at].s[len] = 0;
    nl++;
}

static void remove_line(int at)
{
    free(L[at].s);
    memmove(L + at, L + at + 1, (nl - at - 1) * sizeof *L);
    nl--;
    L[nl] = (struct line){ 0 };
}

static void clear_text(void)
{
    for (int i = 0; i < nl; i++) free(L[i].s);
    nl = 0;
    insert_line(0, "", 0);
    cy = cx = ay = ax = 0;
    selecting = false;
    top = 0;
    hscroll = 0;
}

/* Texte entier (lignes jointes par \n) — à libérer */
static char *text_get(size_t *len_out)
{
    size_t n = 0;
    for (int i = 0; i < nl; i++) n += L[i].len + 1;
    char *t = malloc(n + 1), *p = t;
    for (int i = 0; i < nl; i++) {
        memcpy(p, L[i].s, L[i].len);
        p += L[i].len;
        if (i + 1 < nl) *p++ = '\n';
    }
    *p = 0;
    if (len_out) *len_out = p - t;
    return t;
}

static void text_set(const char *t)
{
    for (int i = 0; i < nl; i++) free(L[i].s);
    nl = 0;
    const char *p = t;
    for (;;) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len > 0 && p[len - 1] == '\r') len--;   /* fins de ligne Windows */
        insert_line(nl, p, len);
        if (!e) break;
        p = e + 1;
    }
    if (nl == 0) insert_line(0, "", 0);
}

/* ---- UTF-8 ---- */
static int next_char(const char *s, int len, int i)
{
    if (i >= len) return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}
static int prev_char(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}
static bool is_word(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

/* ======================= Annuler / rétablir ======================= */

struct snap { char *text; int cy, cx; };
static struct snap undo_stack[100], redo_stack[100];
static int nundo, nredo;
static int last_kind;            /* type de la dernière modification (regroupement) */
static double last_edit_time;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void push_snap(struct snap *st, int *n, struct snap s)
{
    if (*n == 100) { free(st[0].text); memmove(st, st + 1, 99 * sizeof *st); (*n)--; }
    st[(*n)++] = s;
}

/* À appeler avant une modification ; kind : 1 frappe, 2 effacement, 0 autre */
static void before_edit(int kind)
{
    double t = now_s();
    if (kind == 0 || kind != last_kind || t - last_edit_time > 1.2 || nundo == 0)
        push_snap(undo_stack, &nundo, (struct snap){ text_get(NULL), cy, cx });
    for (int i = 0; i < nredo; i++) free(redo_stack[i].text);
    nredo = 0;
    last_kind = kind;
    last_edit_time = t;
    modified = true;
}

static void restore(struct snap *from, int *nfrom, struct snap *to, int *nto)
{
    if (!*nfrom) return;
    push_snap(to, nto, (struct snap){ text_get(NULL), cy, cx });
    struct snap s = from[--(*nfrom)];
    text_set(s.text);
    free(s.text);
    cy = s.cy < nl ? s.cy : nl - 1;
    cx = s.cx <= L[cy].len ? s.cx : L[cy].len;
    selecting = false;
    last_kind = 0;
    modified = true;
}

/* ======================= Sélection et édition ======================= */

static bool has_sel(void) { return selecting && (ay != cy || ax != cx); }

static void sel_range(int *y0, int *x0, int *y1, int *x1)
{
    if (ay < cy || (ay == cy && ax < cx)) { *y0 = ay; *x0 = ax; *y1 = cy; *x1 = cx; }
    else { *y0 = cy; *x0 = cx; *y1 = ay; *x1 = ax; }
}

static char *sel_text(void)
{
    int y0, x0, y1, x1;
    sel_range(&y0, &x0, &y1, &x1);
    size_t n = 1;
    for (int y = y0; y <= y1; y++) n += L[y].len + 1;
    char *t = malloc(n), *p = t;
    for (int y = y0; y <= y1; y++) {
        int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : L[y].len;
        memcpy(p, L[y].s + a, b - a);
        p += b - a;
        if (y < y1) *p++ = '\n';
    }
    *p = 0;
    return t;
}

static void delete_sel(void)
{
    if (!has_sel()) { selecting = false; return; }
    int y0, x0, y1, x1;
    sel_range(&y0, &x0, &y1, &x1);
    struct line *a = &L[y0], *b = &L[y1];
    int tail = b->len - x1;
    line_reserve(a, x0 + tail);
    memmove(a->s + x0, b->s + x1, tail);
    a->len = x0 + tail;
    a->s[a->len] = 0;
    for (int y = y1; y > y0; y--) remove_line(y);
    cy = y0;
    cx = x0;
    selecting = false;
}

static void insert_text(const char *t)
{
    delete_sel();
    while (*t) {
        const char *e = strchr(t, '\n');
        int len = e ? (int)(e - t) : (int)strlen(t);
        struct line *l = &L[cy];
        line_reserve(l, l->len + len);
        memmove(l->s + cx + len, l->s + cx, l->len - cx);
        memcpy(l->s + cx, t, len);
        l->len += len;
        l->s[l->len] = 0;
        cx += len;
        if (!e) break;
        /* coupure de ligne */
        insert_line(cy + 1, L[cy].s + cx, L[cy].len - cx);
        L[cy].len = cx;
        L[cy].s[cx] = 0;
        cy++;
        cx = 0;
        t = e + 1;
    }
}

static char *clip;               /* presse-papiers (partagé entre fenêtres : non) */

static void copy_sel(void)
{
    if (!has_sel()) return;
    free(clip);
    clip = sel_text();
}

/* ======================= Fichiers ======================= */

static void update_title(void)
{
    if (!win || !win->toplevel) return;
    const char *base = path[0] ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : "Sans titre";
    char t[400];
    snprintf(t, sizeof t, "%s%s — Éditeur de texte", modified ? "• " : "", base);
    xdg_toplevel_set_title(win->toplevel, t);
}

static void reset_history(void)
{
    for (int i = 0; i < nundo; i++) free(undo_stack[i].text);
    for (int i = 0; i < nredo; i++) free(redo_stack[i].text);
    nundo = nredo = 0;
    last_kind = 0;
}

static bool load_file(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) {
        /* nouveau fichier : on l'ouvrira vide, il sera créé à l'enregistrement */
        if (errno == ENOENT) {
            clear_text();
            snprintf(path, sizeof path, "%s", p);
            modified = false;
            reset_history();
            set_status("Nouveau fichier : il sera créé à l'enregistrement.");
            return true;
        }
        char m[300];
        snprintf(m, sizeof m, "Impossible d'ouvrir : %s", strerror(errno));
        set_status(m);
        return false;
    }
    if (S_ISDIR(st.st_mode)) { set_status("C'est un dossier, pas un fichier."); return false; }
    if (st.st_size > MAX_FILE) { set_status("Fichier trop gros pour l'éditeur (8 Mo au plus)."); return false; }
    FILE *f = fopen(p, "rb");
    if (!f) { char m[300]; snprintf(m, sizeof m, "Impossible d'ouvrir : %s", strerror(errno)); set_status(m); return false; }
    char *buf = malloc(st.st_size + 1);
    size_t n = fread(buf, 1, st.st_size, f);
    fclose(f);
    buf[n] = 0;
    if (memchr(buf, 0, n)) {
        free(buf);
        set_status("Ce fichier n'est pas du texte (fichier binaire).");
        return false;
    }
    text_set(buf);
    free(buf);
    snprintf(path, sizeof path, "%s", p);
    cy = cx = 0;
    selecting = false;
    top = 0;
    hscroll = 0;
    modified = false;
    reset_history();
    nx_recent_add(path);
    update_title();
    return true;
}

static bool save_file(const char *p)
{
    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof tmp, "%s.nexus-tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        char m[300];
        if (errno == ENOENT) snprintf(m, sizeof m, "Ce dossier n'existe pas : choisis-en un autre.");
        else if (errno == EACCES) snprintf(m, sizeof m, "Tu n'as pas le droit d'écrire dans ce dossier.");
        else snprintf(m, sizeof m, "Enregistrement impossible : %s", strerror(errno));
        set_status(m);
        return false;
    }
    size_t len;
    char *t = text_get(&len);
    bool ok = fwrite(t, 1, len, f) == len && (len == 0 || fputc('\n', f) != EOF);
    free(t);
    ok = fclose(f) == 0 && ok;
    struct stat st;
    if (ok && stat(p, &st) == 0) chmod(tmp, st.st_mode & 07777);   /* garder les droits */
    if (!ok || rename(tmp, p) != 0) {
        unlink(tmp);
        set_status("Enregistrement impossible (disque plein ou droits insuffisants ?)");
        return false;
    }
    snprintf(path, sizeof path, "%s", p);
    modified = false;
    nx_recent_add(path);
    update_title();
    set_status("Enregistré.");
    return true;
}

/* ======================= Boîtes de dialogue ======================= */

enum dlg { D_NONE, D_OPEN, D_SAVEAS, D_CONFIRM };
enum after { A_NONE, A_NEW, A_OPEN, A_QUIT };
static enum dlg dialog;
static enum after pending;       /* action après « Enregistrer / Ne pas enregistrer » */
static char dlg_path[PATH_MAX];
static char dlg_msg[200];
static bool dlg_overwrite;       /* « existe déjà » : Entrée à nouveau pour remplacer */
static char dlg_names[200][256];
static bool dlg_isdir[200];
static int dlg_n, dlg_scroll;

static int cmp_names(const void *a, const void *b) { return strcasecmp(a, b); }

/* Liste du dossier contenant dlg_path */
static void dlg_list(void)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", dlg_path);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else snprintf(dir, sizeof dir, "./");
    dlg_n = 0;
    dlg_scroll = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char tmpn[200][256];
    bool tmpd[200];
    int n = 0;
    while ((e = readdir(d)) && n < 200) {
        if (e->d_name[0] == '.' && strcmp(e->d_name, "..")) continue;
        if (!strcmp(e->d_name, "..") && !strcmp(dir, "/")) continue;
        char full[PATH_MAX + 256];
        snprintf(full, sizeof full, "%s%s", dir, e->d_name);
        struct stat st;
        tmpd[n] = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        snprintf(tmpn[n], 256, "%s", e->d_name);
        n++;
    }
    closedir(d);
    /* dossiers d'abord, puis fichiers ; chacun par ordre alphabétique */
    for (int pass = 0; pass < 2; pass++) {
        int start = dlg_n;
        for (int i = 0; i < n; i++)
            if (tmpd[i] == (pass == 0)) { snprintf(dlg_names[dlg_n], 256, "%s", tmpn[i]); dlg_isdir[dlg_n] = pass == 0; dlg_n++; }
        qsort(dlg_names + start, dlg_n - start, 256, cmp_names);
    }
}

static void open_dialog(enum dlg d)
{
    dialog = d;
    dlg_msg[0] = 0;
    dlg_overwrite = false;
    if (d == D_OPEN || d == D_SAVEAS) {
        if (path[0]) snprintf(dlg_path, sizeof dlg_path, "%s", path);
        else {
            const char *h = getenv("HOME");
            char docs[PATH_MAX];
            snprintf(docs, sizeof docs, "%s/Documents", h ? h : "");
            mkdir(docs, 0755);                       /* compte tout neuf */
            snprintf(dlg_path, sizeof dlg_path, "%s/Documents/%s", h ? h : "", d == D_SAVEAS ? "sans-titre.txt" : "");
        }
        if (d == D_OPEN && path[0]) { char *s = strrchr(dlg_path, '/'); if (s) s[1] = 0; }
        dlg_list();
    }
}

static void do_new(void)
{
    clear_text();
    path[0] = 0;
    modified = false;
    reset_history();
    update_title();
}

/* Action protégée : si modifié, demander d'abord */
static void guarded(enum after a)
{
    if (modified) { pending = a; open_dialog(D_CONFIRM); return; }
    if (a == A_NEW) do_new();
    else if (a == A_OPEN) open_dialog(D_OPEN);
    else if (a == A_QUIT) nx_quit();
}

static void continue_pending(void)
{
    enum after a = pending;
    pending = A_NONE;
    dialog = D_NONE;
    if (a == A_NEW) do_new();
    else if (a == A_OPEN) open_dialog(D_OPEN);
    else if (a == A_QUIT) nx_quit();
}

static void save_or_ask(void)
{
    if (path[0]) { if (save_file(path) && pending) continue_pending(); }
    else open_dialog(D_SAVEAS);
}

static void dlg_validate(void)
{
    struct stat st;
    if (dialog == D_OPEN) {
        if (stat(dlg_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t l = strlen(dlg_path);
            if (l && dlg_path[l - 1] != '/' && l + 1 < sizeof dlg_path) strcat(dlg_path, "/");
            dlg_list();
            return;
        }
        if (stat(dlg_path, &st) != 0) { snprintf(dlg_msg, sizeof dlg_msg, "Ce fichier n'existe pas."); return; }
        if (load_file(dlg_path)) dialog = D_NONE;
        else snprintf(dlg_msg, sizeof dlg_msg, "%s", status_msg);
    } else if (dialog == D_SAVEAS) {
        size_t l = strlen(dlg_path);
        if (!l || dlg_path[l - 1] == '/') { snprintf(dlg_msg, sizeof dlg_msg, "Donne un nom au fichier."); return; }
        if (stat(dlg_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (l + 1 < sizeof dlg_path) strcat(dlg_path, "/");
            dlg_list();
            return;
        }
        if (stat(dlg_path, &st) == 0 && !dlg_overwrite && strcmp(dlg_path, path)) {
            snprintf(dlg_msg, sizeof dlg_msg, "Ce fichier existe déjà : Enregistrer à nouveau pour le remplacer.");
            dlg_overwrite = true;
            return;
        }
        if (save_file(dlg_path)) {
            dialog = D_NONE;
            if (pending) continue_pending();
        } else {
            snprintf(dlg_msg, sizeof dlg_msg, "%s", status_msg);
        }
    }
}

/* ======================= Recherche ======================= */

static bool find_open;
static char find_text[128];

static void find_next(void)
{
    if (!find_text[0]) return;
    size_t fl = strlen(find_text);
    for (int k = 0; k <= nl; k++) {
        int y = (cy + k) % nl;
        int from = k == 0 ? (has_sel() ? cx : cx) : 0;
        if (k == nl) from = 0;
        const char *base = L[y].s;
        for (int i = from; i + (int)fl <= L[y].len; i++)
            if (!strncasecmp(base + i, find_text, fl)) {
                ay = y; ax = i; cy = y; cx = i + (int)fl;
                selecting = true;
                return;
            }
    }
    set_status("Texte introuvable.");
}

/* ======================= Mise en page ======================= */

static double font_size = 13;
static double char_w = 8, line_h = 18;
static PangoFontDescription *mono;

static void setup_font(cairo_t *cr)
{
    if (!mono) mono = pango_font_description_from_string("DejaVu Sans Mono");
    pango_font_description_set_absolute_size(mono, font_size * nx_scale * PANGO_SCALE);
    PangoLayout *lay = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(lay, mono);
    pango_layout_set_text(lay, "MMMMMMMMMM", -1);
    int w, h;
    pango_layout_get_pixel_size(lay, &w, &h);
    g_object_unref(lay);
    char_w = w / 10.0;
    line_h = ceil(h * 1.25);
}

static PangoLayout *line_layout(cairo_t *cr, int y)
{
    PangoLayout *lay = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(lay, mono);
    PangoTabArray *tabs = pango_tab_array_new(1, TRUE);
    pango_tab_array_set_tab(tabs, 0, PANGO_TAB_LEFT, (int)(4 * char_w));
    pango_layout_set_tabs(lay, tabs);
    pango_tab_array_free(tabs);
    pango_layout_set_text(lay, L[y].s ? L[y].s : "", L[y].len);
    return lay;
}

static double index_x(cairo_t *cr, int y, int idx)
{
    PangoLayout *lay = line_layout(cr, y);
    PangoRectangle r;
    pango_layout_index_to_pos(lay, idx, &r);
    g_object_unref(lay);
    return r.x / (double)PANGO_SCALE;
}

static double gutter_w(void)
{
    int digits = 1;
    for (int n = nl; n >= 10; n /= 10) digits++;
    if (digits < 3) digits = 3;
    return digits * char_w + 24;
}

static int text_h(void) { return (win ? win->height : 600) - TOOL_H - STATUS_H - (find_open ? FIND_H : 0); }
static int visible_lines(void) { int v = (int)(text_h() / line_h); return v < 1 ? 1 : v; }

static cairo_t *measure_cr(void)
{
    static cairo_surface_t *s;
    static cairo_t *c;
    if (!c) { s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4); c = cairo_create(s); }
    return c;
}

/* Garder le curseur visible (vertical et horizontal) */
static void ensure_visible(void)
{
    int vis = visible_lines();
    if (cy < top) top = cy;
    if (cy >= top + vis) top = cy - vis + 1;
    if (top < 0) top = 0;
    double tw = (win ? win->width : 800) - gutter_w() - 20;
    double x = index_x(measure_cr(), cy, cx);
    if (x - hscroll > tw) hscroll = x - tw + 6 * char_w;
    if (x - hscroll < 0) hscroll = x - 6 * char_w < 0 ? 0 : x - 6 * char_w;
}

/* Position souris -> (ligne, octet) */
static void pos_at(double mx, double my, int *oy, int *ox)
{
    double ty = TOOL_H + (find_open ? FIND_H : 0);
    int y = top + (int)floor((my - ty) / line_h);
    if (y < 0) y = 0;
    if (y >= nl) y = nl - 1;
    double x = mx - gutter_w() + hscroll;
    PangoLayout *lay = line_layout(measure_cr(), y);
    int idx = 0, trailing = 0;
    pango_layout_xy_to_index(lay, (int)(x * PANGO_SCALE), (int)(line_h / 2 * PANGO_SCALE), &idx, &trailing);
    g_object_unref(lay);
    while (trailing-- > 0) idx = next_char(L[y].s, L[y].len, idx);
    if (x < 0) idx = 0;
    if (idx > L[y].len) idx = L[y].len;
    *oy = y;
    *ox = idx;
}

/* ======================= Dessin ======================= */

enum kind { K_TOOL, K_TEXT, K_FINDCLOSE, K_DLG_ITEM, K_DLG_OK, K_DLG_CANCEL, K_DLG_NO, K_DLG_FIELD, K_DLG_BG };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[256];
static int nrects;
static int hover_kind = -1, hover_index = -1;

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 256) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
}
static bool hot(enum kind k, int i) { return (int)k == hover_kind && i == hover_index; }

static const char *tools[] = { "Nouveau", "Ouvrir…", "Enregistrer", "Enregistrer sous…", "Rechercher" };
enum { T_NEW, T_OPEN, T_SAVE, T_SAVEAS, T_FIND, T_ZOOMOUT, T_ZOOMIN, T_COUNT };

static void draw_button(cairo_t *cr, double x, double y, double w, double h, const char *label, bool primary,
                        bool hov)
{
    nx_color a = nx_accent_color();
    nx_rounded_rect(cr, x, y, w, h, 7);
    if (primary) nx_set(cr, hov ? nx_mix(a, nx_rgb("#ffffff"), 0.15) : a);
    else nx_set(cr, hov ? nx_rgb("#e3e7ef") : nx_rgb("#eef1f6"));
    cairo_fill(cr);
    nx_text(cr, label, 13, primary, primary ? nx_rgb("#ffffff") : nx_rgb(NX_TEXT), x, y, w, h, NX_CENTER);
}

static void draw_dialog(cairo_t *cr, int width, int height)
{
    nx_color text = nx_rgb(NX_TEXT), subtle = nx_rgb(NX_SUBTLE), accent = nx_accent_color();
    cairo_set_source_rgba(cr, 0.05, 0.07, 0.1, 0.35);
    cairo_paint(cr);
    add_rect(0, 0, width, height, K_DLG_BG, 0);
    if (dialog == D_CONFIRM) {
        double dw = fmin(600, width - 40), dh = 170, dx = (width - dw) / 2, dy = (height - dh) / 2;
        nx_rounded_rect(cr, dx, dy, dw, dh, 12);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill(cr);
        const char *base = path[0] ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : "Sans titre";
        char t[400];
        snprintf(t, sizeof t, "Enregistrer les modifications de « %s » ?", base);
        nx_text(cr, t, 15, true, text, dx + 24, dy + 20, dw - 48, 28, NX_LEFT);
        nx_text(cr, "Sinon, elles seront perdues.", 13.5, false, subtle, dx + 24, dy + 56, dw - 48, 22, NX_LEFT);
        double by = dy + dh - 56;
        draw_button(cr, dx + dw - 24 - 130, by, 130, 36, "Enregistrer", true, hot(K_DLG_OK, 0));
        add_rect(dx + dw - 24 - 130, by, 130, 36, K_DLG_OK, 0);
        draw_button(cr, dx + dw - 24 - 130 - 10 - 160, by, 160, 36, "Ne pas enregistrer", false, hot(K_DLG_NO, 0));
        add_rect(dx + dw - 24 - 130 - 10 - 160, by, 160, 36, K_DLG_NO, 0);
        draw_button(cr, dx + 24, by, 100, 36, "Annuler", false, hot(K_DLG_CANCEL, 0));
        add_rect(dx + 24, by, 100, 36, K_DLG_CANCEL, 0);
        return;
    }
    double dw = fmin(620, width - 40), dh = fmin(470, height - 40), dx = (width - dw) / 2, dy = (height - dh) / 2;
    nx_rounded_rect(cr, dx, dy, dw, dh, 12);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    nx_text(cr, dialog == D_OPEN ? "Ouvrir un fichier" : "Enregistrer sous", 18, true, text, dx + 24, dy + 16, dw - 48, 30, NX_LEFT);
    /* champ chemin */
    double fy = dy + 56;
    nx_rounded_rect(cr, dx + 24, fy, dw - 48, 40, 8);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    nx_set(cr, accent);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    double tw = nx_text(cr, dlg_path, 13.5, false, text, dx + 36, fy, dw - 72, 40, NX_LEFT);
    cairo_rectangle(cr, dx + 37 + fmin(tw, dw - 72), fy + 10, 1.5, 20);
    nx_set(cr, accent);
    cairo_fill(cr);
    add_rect(dx + 24, fy, dw - 48, 40, K_DLG_FIELD, 0);
    /* liste du dossier */
    double ly = fy + 52, lh = dh - (ly - dy) - 100, rowh = 30;
    nx_rounded_rect(cr, dx + 24, ly, dw - 48, lh, 8);
    nx_set(cr, nx_rgb("#f4f6fa"));
    cairo_fill(cr);
    int rows = (int)(lh / rowh);
    if (dlg_scroll > dlg_n - rows) dlg_scroll = dlg_n - rows;
    if (dlg_scroll < 0) dlg_scroll = 0;
    cairo_save(cr);
    cairo_rectangle(cr, dx + 24, ly, dw - 48, lh);
    cairo_clip(cr);
    for (int i = dlg_scroll; i < dlg_n && i < dlg_scroll + rows; i++) {
        double ry = ly + (i - dlg_scroll) * rowh;
        if (hot(K_DLG_ITEM, i)) {
            nx_rounded_rect(cr, dx + 28, ry + 2, dw - 56, rowh - 4, 6);
            nx_set(cr, nx_rgb("#e3e7ef"));
            cairo_fill(cr);
        }
        nx_icon(cr, dlg_isdir[i] ? "folder" : "file", dx + 36, ry + 5, 20, text);
        nx_text(cr, !strcmp(dlg_names[i], "..") ? ".. (dossier parent)" : dlg_names[i], 13.5, false, text,
                dx + 64, ry, dw - 100, rowh, NX_LEFT);
        add_rect(dx + 28, ry + 2, dw - 56, rowh - 4, K_DLG_ITEM, i);
    }
    cairo_restore(cr);
    if (dlg_msg[0]) nx_text(cr, dlg_msg, 12.5, true, nx_rgb("#c0392b"), dx + 24, ly + lh + 8, dw - 48, 22, NX_LEFT);
    double by = dy + dh - 56;
    const char *ok = dialog == D_OPEN ? "Ouvrir" : "Enregistrer";
    draw_button(cr, dx + dw - 24 - 130, by, 130, 36, ok, true, hot(K_DLG_OK, 0));
    add_rect(dx + dw - 24 - 130, by, 130, 36, K_DLG_OK, 0);
    draw_button(cr, dx + dw - 24 - 130 - 10 - 110, by, 110, 36, "Annuler", false, hot(K_DLG_CANCEL, 0));
    add_rect(dx + dw - 24 - 130 - 10 - 110, by, 110, 36, K_DLG_CANCEL, 0);
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    setup_font(cr);
    nx_color text = nx_rgb(NX_TEXT), subtle = nx_rgb(NX_SUBTLE), accent = nx_accent_color();
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    /* ---- Barre d'outils ---- */
    cairo_rectangle(cr, 0, 0, width, TOOL_H);
    nx_set(cr, nx_rgb("#f7f8fb"));
    cairo_fill(cr);
    cairo_rectangle(cr, 0, TOOL_H - 1, width, 1);
    nx_set(cr, nx_rgb("#e1e5ec"));
    cairo_fill(cr);
    double x = 10;
    for (int i = 0; i < 5; i++) {
        double bw = nx_text_width(cr, tools[i], 13, false) + 26;
        if (hot(K_TOOL, i) || (i == T_FIND && find_open)) {
            nx_rounded_rect(cr, x, 8, bw, 34, 7);
            nx_set(cr, i == T_FIND && find_open ? nx_alpha(accent, 0.15) : nx_rgb("#e6eaf1"));
            cairo_fill(cr);
        }
        nx_text(cr, tools[i], 13, i == T_SAVE && modified, text, x, 8, bw, 34, NX_CENTER);
        add_rect(x, 8, bw, 34, K_TOOL, i);
        x += bw + 4;
    }
    /* zoom */
    char zs[16];
    snprintf(zs, sizeof zs, "%d %%", (int)(font_size / 13 * 100 + 0.5));
    double zx = width - 150;
    if (zx > x + 10) {
        struct { int t; const char *l; double xx; } z[] = { { T_ZOOMOUT, "−", zx }, { T_ZOOMIN, "+", zx + 100 } };
        for (int i = 0; i < 2; i++) {
            if (hot(K_TOOL, z[i].t)) { nx_rounded_rect(cr, z[i].xx, 8, 34, 34, 7); nx_set(cr, nx_rgb("#e6eaf1")); cairo_fill(cr); }
            nx_text(cr, z[i].l, 18, true, text, z[i].xx, 8, 34, 34, NX_CENTER);
            add_rect(z[i].xx, 8, 34, 34, K_TOOL, z[i].t);
        }
        nx_text(cr, zs, 12.5, false, subtle, zx + 34, 8, 66, 34, NX_CENTER);
    }

    /* ---- Barre de recherche ---- */
    double ty = TOOL_H;
    if (find_open) {
        cairo_rectangle(cr, 0, TOOL_H, width, FIND_H);
        nx_set(cr, nx_rgb("#fbfcfd"));
        cairo_fill(cr);
        nx_text(cr, "Rechercher :", 13, true, text, 14, TOOL_H, 100, FIND_H, NX_LEFT);
        nx_rounded_rect(cr, 110, TOOL_H + 7, 320, FIND_H - 14, 7);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        nx_set(cr, accent);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
        double tw = nx_text(cr, find_text, 13.5, false, text, 120, TOOL_H, 300, FIND_H, NX_LEFT);
        if (!dialog) { cairo_rectangle(cr, 121 + tw, TOOL_H + 14, 1.5, FIND_H - 28); nx_set(cr, accent); cairo_fill(cr); }
        nx_text(cr, "Entrée : suivant · Échap : fermer", 12, false, subtle, 444, TOOL_H, 260, FIND_H, NX_LEFT);
        if (hot(K_FINDCLOSE, 0)) { nx_rounded_rect(cr, width - 44, TOOL_H + 7, 32, 32, 6); nx_set(cr, nx_rgb("#e6eaf1")); cairo_fill(cr); }
        nx_text(cr, "×", 18, false, subtle, width - 44, TOOL_H + 7, 32, 32, NX_CENTER);
        add_rect(width - 44, TOOL_H + 7, 32, 32, K_FINDCLOSE, 0);
        cairo_rectangle(cr, 0, TOOL_H + FIND_H - 1, width, 1);
        nx_set(cr, nx_rgb("#e1e5ec"));
        cairo_fill(cr);
        ty += FIND_H;
    }

    /* ---- Texte ---- */
    double gw = gutter_w(), th = height - ty - STATUS_H;
    add_rect(0, ty, width, th, K_TEXT, 0);
    cairo_rectangle(cr, 0, ty, gw - 8, th);
    nx_set(cr, nx_rgb("#f4f6fa"));
    cairo_fill(cr);
    int vis = (int)(th / line_h) + 1;
    int sy0 = 0, sx0 = 0, sy1 = -1, sx1 = 0;
    if (has_sel()) sel_range(&sy0, &sx0, &sy1, &sx1);
    for (int i = 0; i < vis && top + i < nl; i++) {
        int y = top + i;
        double ly = ty + i * line_h;
        /* numéro de ligne */
        char num[16];
        snprintf(num, sizeof num, "%d", y + 1);
        nx_text(cr, num, font_size * 0.85, y == cy, y == cy ? text : nx_rgb("#a0a7b5"), 0, ly, gw - 16, line_h, NX_RIGHT);
        cairo_save(cr);
        cairo_rectangle(cr, gw, ty, width - gw, th);
        cairo_clip(cr);
        if (y == cy && !has_sel()) {
            cairo_rectangle(cr, gw - 8, ly, width, line_h);
            nx_set(cr, nx_alpha(accent, 0.06));
            cairo_fill(cr);
        }
        PangoLayout *lay = line_layout(cr, y);
        /* sélection */
        if (y >= sy0 && y <= sy1) {
            int a = y == sy0 ? sx0 : 0, b = y == sy1 ? sx1 : L[y].len;
            PangoRectangle ra, rb;
            pango_layout_index_to_pos(lay, a, &ra);
            pango_layout_index_to_pos(lay, b, &rb);
            double xa = gw + ra.x / (double)PANGO_SCALE - hscroll, xb = gw + rb.x / (double)PANGO_SCALE - hscroll;
            if (y < sy1) xb += char_w * 0.6;   /* fin de ligne sélectionnée */
            cairo_rectangle(cr, xa, ly, xb - xa, line_h);
            nx_set(cr, nx_alpha(accent, 0.25));
            cairo_fill(cr);
        }
        int lw, lhh;
        pango_layout_get_pixel_size(lay, &lw, &lhh);
        nx_set(cr, text);
        cairo_move_to(cr, gw - hscroll, ly + (line_h - lhh) / 2);
        pango_cairo_show_layout(cr, lay);
        /* curseur */
        if (y == cy && !dialog && !find_open) {
            PangoRectangle r;
            pango_layout_index_to_pos(lay, cx, &r);
            cairo_rectangle(cr, gw + r.x / (double)PANGO_SCALE - hscroll, ly + 2, 2, line_h - 4);
            nx_set(cr, accent);
            cairo_fill(cr);
        }
        g_object_unref(lay);
        cairo_restore(cr);
    }
    /* barre de défilement */
    int total = nl + visible_lines() - 1;
    if (nl > visible_lines()) {
        double bh = fmax(30, th * visible_lines() / total), by = ty + (th - bh) * top / (double)(total - visible_lines());
        nx_rounded_rect(cr, width - 7, by, 4, bh, 2);
        nx_set(cr, nx_alpha(subtle, 0.45));
        cairo_fill(cr);
    }

    /* ---- Barre d'état ---- */
    double sy = height - STATUS_H;
    cairo_rectangle(cr, 0, sy, width, STATUS_H);
    nx_set(cr, nx_rgb("#f7f8fb"));
    cairo_fill(cr);
    cairo_rectangle(cr, 0, sy, width, 1);
    nx_set(cr, nx_rgb("#e1e5ec"));
    cairo_fill(cr);
    int col = 1;
    for (int i = 0; i < cx; i = next_char(L[cy].s, L[cy].len, i)) col++;
    char left[300], right[200];
    if (status_msg[0] && time(NULL) < status_until) snprintf(left, sizeof left, "%s", status_msg);
    else snprintf(left, sizeof left, "%s", path[0] ? path : "Nouveau document (pas encore enregistré)");
    snprintf(right, sizeof right, "%sLigne %d, colonne %d · %d ligne%s · UTF-8", modified ? "Modifié · " : "",
             cy + 1, col, nl, nl > 1 ? "s" : "");
    nx_text(cr, left, 12, false, nx_rgb("#3a4152"), 14, sy, width * 0.55, STATUS_H, NX_LEFT);
    nx_text(cr, right, 12, false, subtle, width * 0.5, sy, width * 0.5 - 14, STATUS_H, NX_RIGHT);

    if (dialog) draw_dialog(cr, width, height);
}

/* ======================= Souris ======================= */

static bool dragging_sel;
static double last_click;
static int click_count;

static struct rect *hit(double x, double y)
{
    for (int i = nrects - 1; i >= 0; i--)
        if (x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h)
            return &rects[i];
    return NULL;
}

static void zoom(int d)
{
    font_size += d;
    if (font_size < 8) font_size = 8;
    if (font_size > 32) font_size = 32;
}

static void tool(int t)
{
    switch (t) {
    case T_NEW: guarded(A_NEW); break;
    case T_OPEN: guarded(A_OPEN); break;
    case T_SAVE: save_or_ask(); break;
    case T_SAVEAS: open_dialog(D_SAVEAS); break;
    case T_FIND: find_open = !find_open; break;
    case T_ZOOMOUT: zoom(-1); break;
    case T_ZOOMIN: zoom(1); break;
    }
    update_title();
}

static void select_word_at(int y, int x)
{
    const char *s = L[y].s;
    int a = x, b = x;
    while (a > 0 && is_word((unsigned char)s[a - 1])) a--;
    while (b < L[y].len && is_word((unsigned char)s[b])) b++;
    ay = y; ax = a; cy = y; cx = b;
    selecting = true;
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct rect *r = hit(x, y);
    int k = r ? (int)r->kind : -1, i = r ? r->index : -1;
    switch (ev) {
    case NX_MOTION:
        if (dragging_sel) {
            pos_at(x, y, &cy, &cx);
            ensure_visible();
            nx_redraw(w);
            break;
        }
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
        break;
    case NX_LEAVE:
        hover_kind = hover_index = -1;
        nx_redraw(w);
        break;
    case NX_SCROLL:
        if (dialog == D_OPEN || dialog == D_SAVEAS) { dlg_scroll += value * 3; nx_redraw(w); break; }
        if (dialog) break;
        if (nx_mod_ctrl()) { zoom(-value); nx_redraw(w); break; }
        top += value * 3;
        if (top > nl - 1) top = nl - 1;
        if (top < 0) top = 0;
        nx_redraw(w);
        break;
    case NX_PRESS:
        if (value != BTN_LEFT || dialog || k != K_TEXT) break;
        {
            double t = now_s();
            click_count = t - last_click < 0.4 ? click_count + 1 : 1;
            last_click = t;
            int py, px;
            pos_at(x, y, &py, &px);
            if (click_count == 2) { select_word_at(py, px); nx_redraw(w); break; }
            if (nx_mod_shift()) { if (!selecting) { ay = cy; ax = cx; selecting = true; } }
            else { ay = py; ax = px; selecting = true; }
            cy = py; cx = px;
            dragging_sel = true;
            last_kind = 0;
            nx_redraw(w);
        }
        break;
    case NX_RELEASE:
        if (value != BTN_LEFT) break;
        if (dragging_sel) { dragging_sel = false; if (!has_sel()) selecting = false; nx_redraw(w); break; }
        if (!r) break;
        switch (r->kind) {
        case K_TOOL: tool(r->index); break;
        case K_FINDCLOSE: find_open = false; break;
        case K_DLG_OK:
            if (dialog == D_CONFIRM) save_or_ask();
            else dlg_validate();
            break;
        case K_DLG_NO: modified = false; continue_pending(); break;
        case K_DLG_CANCEL: dialog = D_NONE; pending = A_NONE; break;
        case K_DLG_ITEM: {
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, "%s", dlg_path);
            char *s = strrchr(dir, '/');
            if (s) s[1] = 0; else dir[0] = 0;
            const char *n = dlg_names[r->index];
            if (!strcmp(n, "..")) {
                size_t l = strlen(dir);
                if (l > 1) { dir[l - 1] = 0; char *p = strrchr(dir, '/'); if (p) p[1] = 0; }
                snprintf(dlg_path, sizeof dlg_path, "%s", dir);
                dlg_list();
            } else if (dlg_isdir[r->index]) {
                char np[PATH_MAX];
                snprintf(np, sizeof np, "%.*s%s/", (int)(sizeof np - 260), dir, n);
                /* en « Enregistrer sous », garder le nom choisi */
                if (dialog == D_SAVEAS) {
                    const char *base = strrchr(dlg_path, '/') ? strrchr(dlg_path, '/') + 1 : dlg_path;
                    char nm[256];
                    snprintf(nm, sizeof nm, "%s", base);
                    snprintf(dlg_path, sizeof dlg_path, "%.*s%s", (int)(sizeof dlg_path - 260), np, nm);
                } else {
                    snprintf(dlg_path, sizeof dlg_path, "%s", np);
                }
                dlg_list();
            } else {
                char np[PATH_MAX];
                snprintf(np, sizeof np, "%.*s%s", (int)(sizeof np - 260), dir, n);
                bool same = !strcmp(np, dlg_path);
                snprintf(dlg_path, sizeof dlg_path, "%s", np);
                dlg_overwrite = false;
                dlg_msg[0] = 0;
                if (same || dialog == D_OPEN) dlg_validate();   /* 2e clic ou ouverture directe */
            }
            break;
        }
        default: break;
        }
        nx_redraw(w);
        break;
    }
}

/* ======================= Clavier ======================= */

static void move_to(int y, int x, bool shift)
{
    if (shift) { if (!selecting) { ay = cy; ax = cx; selecting = true; } }
    else selecting = false;
    cy = y;
    cx = x;
    last_kind = 0;
}

static int word_left(int y, int x)
{
    const char *s = L[y].s;
    while (x > 0 && !is_word((unsigned char)s[x - 1])) x--;
    while (x > 0 && is_word((unsigned char)s[x - 1])) x--;
    return x;
}
static int word_right(int y, int x)
{
    const char *s = L[y].s;
    while (x < L[y].len && !is_word((unsigned char)s[x])) x++;
    while (x < L[y].len && is_word((unsigned char)s[x])) x++;
    return x;
}

/* Garder (à peu près) la colonne en montant / descendant */
static int x_on_line(int from_y, int from_x, int to_y)
{
    cairo_t *c = measure_cr();
    double px = index_x(c, from_y, from_x);
    PangoLayout *lay = line_layout(c, to_y);
    int idx = 0, trailing = 0;
    pango_layout_xy_to_index(lay, (int)(px * PANGO_SCALE), 0, &idx, &trailing);
    g_object_unref(lay);
    while (trailing-- > 0) idx = next_char(L[to_y].s, L[to_y].len, idx);
    return idx > L[to_y].len ? L[to_y].len : idx;
}

static bool dialog_key(xkb_keysym_t sym, const char *utf8)
{
    if (!dialog) return false;
    if (dialog == D_CONFIRM) {
        if (sym == XKB_KEY_Escape) { dialog = D_NONE; pending = A_NONE; }
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) save_or_ask();
        return true;
    }
    if (sym == XKB_KEY_Escape) { dialog = D_NONE; pending = A_NONE; }
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) dlg_validate();
    else if (sym == XKB_KEY_BackSpace) {
        size_t n = strlen(dlg_path);
        n = prev_char(dlg_path, (int)n);
        dlg_path[n] = 0;
        dlg_overwrite = false;
        if (!n || dlg_path[n - 1] == '/') dlg_list();
    } else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
               strlen(dlg_path) + strlen(utf8) < sizeof dlg_path - 1) {
        strcat(dlg_path, utf8);
        dlg_overwrite = false;
        dlg_msg[0] = 0;
        if (utf8[0] == '/') dlg_list();
    }
    return true;
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    bool ctrl = nx_mod_ctrl(), shift = nx_mod_shift();
    if (dialog_key(sym, utf8)) { nx_redraw(w); return; }

    if (ctrl) {
        switch (sym) {
        case XKB_KEY_s: case XKB_KEY_S: if (shift) open_dialog(D_SAVEAS); else save_or_ask(); break;
        case XKB_KEY_o: case XKB_KEY_O: guarded(A_OPEN); break;
        case XKB_KEY_n: case XKB_KEY_N: guarded(A_NEW); break;
        case XKB_KEY_q: case XKB_KEY_Q: case XKB_KEY_w: case XKB_KEY_W: guarded(A_QUIT); break;
        case XKB_KEY_f: case XKB_KEY_F: find_open = true; break;
        case XKB_KEY_a: case XKB_KEY_A: ay = 0; ax = 0; cy = nl - 1; cx = L[cy].len; selecting = true; break;
        case XKB_KEY_c: case XKB_KEY_C: copy_sel(); break;
        case XKB_KEY_x: case XKB_KEY_X: if (has_sel()) { copy_sel(); before_edit(0); delete_sel(); } break;
        case XKB_KEY_v: case XKB_KEY_V: if (clip) { before_edit(0); insert_text(clip); } break;
        case XKB_KEY_z: case XKB_KEY_Z:
            if (shift) restore(redo_stack, &nredo, undo_stack, &nundo);
            else restore(undo_stack, &nundo, redo_stack, &nredo);
            break;
        case XKB_KEY_y: case XKB_KEY_Y: restore(redo_stack, &nredo, undo_stack, &nundo); break;
        case XKB_KEY_plus: case XKB_KEY_equal: case XKB_KEY_KP_Add: zoom(1); break;
        case XKB_KEY_minus: case XKB_KEY_KP_Subtract: zoom(-1); break;
        case XKB_KEY_0: case XKB_KEY_KP_0: font_size = 13; break;
        case XKB_KEY_Home: move_to(0, 0, shift); break;
        case XKB_KEY_End: move_to(nl - 1, L[nl - 1].len, shift); break;
        case XKB_KEY_Left:
            if (cx > 0) move_to(cy, word_left(cy, cx), shift);
            else if (cy > 0) move_to(cy - 1, L[cy - 1].len, shift);
            break;
        case XKB_KEY_Right:
            if (cx < L[cy].len) move_to(cy, word_right(cy, cx), shift);
            else if (cy + 1 < nl) move_to(cy + 1, 0, shift);
            break;
        default: return;
        }
        ensure_visible();
        update_title();
        nx_redraw(w);
        return;
    }

    if (find_open) {
        if (sym == XKB_KEY_Escape) find_open = false;
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) find_next();
        else if (sym == XKB_KEY_BackSpace) find_text[prev_char(find_text, (int)strlen(find_text))] = 0;
        else if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
                 strlen(find_text) + strlen(utf8) < sizeof find_text - 1) strcat(find_text, utf8);
        else return;
        ensure_visible();
        nx_redraw(w);
        return;
    }

    int vis = visible_lines();
    switch (sym) {
    case XKB_KEY_Left:
        if (has_sel() && !shift) { int y0, x0, y1, x1; sel_range(&y0, &x0, &y1, &x1); move_to(y0, x0, false); }
        else if (cx > 0) move_to(cy, prev_char(L[cy].s, cx), shift);
        else if (cy > 0) move_to(cy - 1, L[cy - 1].len, shift);
        break;
    case XKB_KEY_Right:
        if (has_sel() && !shift) { int y0, x0, y1, x1; sel_range(&y0, &x0, &y1, &x1); move_to(y1, x1, false); }
        else if (cx < L[cy].len) move_to(cy, next_char(L[cy].s, L[cy].len, cx), shift);
        else if (cy + 1 < nl) move_to(cy + 1, 0, shift);
        break;
    case XKB_KEY_Up: if (cy > 0) move_to(cy - 1, x_on_line(cy, cx, cy - 1), shift); else move_to(0, 0, shift); break;
    case XKB_KEY_Down:
        if (cy + 1 < nl) move_to(cy + 1, x_on_line(cy, cx, cy + 1), shift);
        else move_to(cy, L[cy].len, shift);
        break;
    case XKB_KEY_Page_Up: { int y = cy - vis < 0 ? 0 : cy - vis; move_to(y, x_on_line(cy, cx, y), shift); top -= vis; break; }
    case XKB_KEY_Page_Down: { int y = cy + vis >= nl ? nl - 1 : cy + vis; move_to(y, x_on_line(cy, cx, y), shift); top += vis; break; }
    case XKB_KEY_Home: {
        /* 1er appui : début du texte de la ligne (après l'indentation) */
        int ind = 0;
        while (ind < L[cy].len && (L[cy].s[ind] == ' ' || L[cy].s[ind] == '\t')) ind++;
        move_to(cy, cx == ind ? 0 : ind, shift);
        break;
    }
    case XKB_KEY_End: move_to(cy, L[cy].len, shift); break;
    case XKB_KEY_BackSpace:
        if (has_sel()) { before_edit(0); delete_sel(); }
        else if (cx > 0) {
            before_edit(2);
            int p = prev_char(L[cy].s, cx);
            memmove(L[cy].s + p, L[cy].s + cx, L[cy].len - cx + 1);
            L[cy].len -= cx - p;
            cx = p;
        } else if (cy > 0) {
            before_edit(0);
            int pl = L[cy - 1].len;
            line_reserve(&L[cy - 1], pl + L[cy].len);
            memcpy(L[cy - 1].s + pl, L[cy].s, L[cy].len + 1);
            L[cy - 1].len += L[cy].len;
            remove_line(cy);
            cy--;
            cx = pl;
        }
        selecting = false;
        break;
    case XKB_KEY_Delete:
        if (has_sel()) { before_edit(0); delete_sel(); }
        else if (cx < L[cy].len) {
            before_edit(2);
            int n = next_char(L[cy].s, L[cy].len, cx);
            memmove(L[cy].s + cx, L[cy].s + n, L[cy].len - n + 1);
            L[cy].len -= n - cx;
        } else if (cy + 1 < nl) {
            before_edit(0);
            line_reserve(&L[cy], L[cy].len + L[cy + 1].len);
            memcpy(L[cy].s + L[cy].len, L[cy + 1].s, L[cy + 1].len + 1);
            L[cy].len += L[cy + 1].len;
            remove_line(cy + 1);
        }
        selecting = false;
        break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: {
        before_edit(0);
        /* garder l'indentation de la ligne */
        char ins[130] = "\n";
        int ind = 0;
        while (ind < L[cy].len && ind < 120 && (L[cy].s[ind] == ' ' || L[cy].s[ind] == '\t')) ind++;
        if (ind > cx) ind = cx;
        memcpy(ins + 1, L[cy].s, ind);
        ins[1 + ind] = 0;
        insert_text(ins);
        break;
    }
    case XKB_KEY_Tab: before_edit(1); insert_text("    "); break;
    case XKB_KEY_Escape: selecting = false; break;
    default:
        if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
            before_edit(1);
            insert_text(utf8);
        } else {
            return;
        }
    }
    if (top < 0) top = 0;
    if (top > nl - 1) top = nl - 1;
    ensure_visible();
    update_title();
    nx_redraw(w);
}

static void closed(struct nx_window *w)
{
    (void)w;
    guarded(A_QUIT);
    nx_redraw(win);
}

static int status_timeout(void)
{
    if (!status_msg[0]) return -1;
    time_t now = time(NULL);
    if (now >= status_until) { status_msg[0] = 0; nx_redraw(win); return -1; }
    return (int)(status_until - now) * 1000 + 50;
}
static void status_tick(void) { nx_redraw(win); }

int editor_main(int argc, char **argv)
{
    clear_text();
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key, .closed = closed };
    win = nx_toplevel_window(h, "Éditeur de texte", "nexus-editor", 860, 600);
    if (!win) return 1;
    if (argc > 1) {
        char real[PATH_MAX];
        const char *p = argv[1];
        if (p[0] != '/' && realpath(".", real)) {
            char full[PATH_MAX * 2];
            snprintf(full, sizeof full, "%s/%s", real, p);
            load_file(full);
        } else {
            load_file(p);
        }
    }
    update_title();
    nx.timeout = status_timeout;
    nx.tick = status_tick;
    nx_run();
    return 0;
}
