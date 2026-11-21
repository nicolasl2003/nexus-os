/* nexus-menu — le menu Démarrer de Nexus OS (thème "verre" sombre).
 *
 *  ┌──────────────┬───────────────────────────────────────────┐
 *  │  N           │ [ Rechercher une application, un fichier ] │
 *  │ ▸ Accueil    │ Applications épinglées          Tout voir →│
 *  │   Applications│  [ ] [ ] [ ] [ ]                          │
 *  │   Fichiers   │  [ ] [ ] [ ] [ ]                          │
 *  │   Paramètres │ Récents                             Plus → │
 *  │   Système    │  fichier.png   Il y a 2 min               │
 *  │   Alimentation│                                          │
 *  │ (o) nicolas  │                                           │
 *  │ ⏻ Éteindre   │                                           │
 *  └──────────────┴───────────────────────────────────────────┘
 *
 * Pages : Accueil, Applications (liste complète), Alimentation, et la
 * recherche (applications + fichiers du dossier personnel).
 * Clavier : taper = rechercher, flèches, Entrée, Échap.
 * Un deuxième lancement ferme le menu (bouton N ou touche Super).
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <dirent.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define MENU_W  800
#define MENU_H  610
#define NAV_W   224

/* ======================= Applications (.desktop) ======================= */

struct app { char id[64], name[128], comment[256], exec[512], icon[64]; };
static struct app apps[128];
static int napps;

static bool in_path(const char *prog)
{
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    char buf[512];
    while (path && *path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        snprintf(buf, sizeof buf, "%.*s/%s", (int)n, path, prog);
        if (access(buf, X_OK) == 0) return true;
        path = end ? end + 1 : NULL;
    }
    return false;
}

static void strip_field_codes(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && r[1]) { r += 2; continue; }
        *w++ = *r++;
    }
    *w = 0;
}

static void load_desktop_file(const char *dir, const char *file)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f || napps >= 128) { if (f) fclose(f); return; }
    struct app a = { 0 };
    snprintf(a.id, sizeof a.id, "%.*s", (int)(strlen(file) - 8), file);
    char line[1024], tryexec[256] = "", name_fr[128] = "", comment_fr[256] = "";
    bool in_entry = false, hidden = false, terminal = false, is_app = true;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { in_entry = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in_entry) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "Name")) snprintf(a.name, sizeof a.name, "%s", v);
        else if (!strcmp(k, "Name[fr]")) snprintf(name_fr, sizeof name_fr, "%s", v);
        else if (!strcmp(k, "Comment")) snprintf(a.comment, sizeof a.comment, "%s", v);
        else if (!strcmp(k, "Comment[fr]")) snprintf(comment_fr, sizeof comment_fr, "%s", v);
        else if (!strcmp(k, "Exec")) snprintf(a.exec, sizeof a.exec, "%s", v);
        else if (!strcmp(k, "Icon")) snprintf(a.icon, sizeof a.icon, "%s", v);
        else if (!strcmp(k, "TryExec")) snprintf(tryexec, sizeof tryexec, "%s", v);
        else if (!strcmp(k, "Terminal")) terminal = !strcmp(v, "true");
        else if (!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) hidden |= !strcmp(v, "true");
        else if (!strcmp(k, "Type")) is_app = !strcmp(v, "Application");
    }
    fclose(f);
    if (name_fr[0]) snprintf(a.name, sizeof a.name, "%s", name_fr);
    if (comment_fr[0]) snprintf(a.comment, sizeof a.comment, "%s", comment_fr);
    if (hidden || !is_app || !a.name[0] || !a.exec[0]) return;
    if (tryexec[0] && !in_path(tryexec)) return;
    strip_field_codes(a.exec);
    if (terminal) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "foot %s", a.exec);
        snprintf(a.exec, sizeof a.exec, "%s", tmp);
    }
    for (int i = 0; i < napps; i++)
        if (!strcmp(apps[i].id, a.id)) { apps[i] = a; return; }
    apps[napps++] = a;
}

static void load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 8 && !strcmp(e->d_name + n - 8, ".desktop")) load_desktop_file(dir, e->d_name);
    }
    closedir(d);
}

static int cmp_app(const void *a, const void *b)
{
    return strcasecmp(((const struct app *)a)->name, ((const struct app *)b)->name);
}

/* Logo d'une application : "@identifiant" (dessiné par nx_app_icon) */
static const char *app_icon(const struct app *a)
{
    static char buf[8][80];
    static int k;
    k = (k + 1) % 8;
    const char *id = nx_app_icon_for(a->icon);
    if (!strcmp(id, a->icon)) {
        id = nx_app_icon_for(a->id);
        if (!strcmp(id, a->id)) id = a->name;   /* inconnue : initiale du nom */
    }
    snprintf(buf[k], sizeof buf[k], "@%s", id);
    return buf[k];
}

/* Applications épinglées sur l'accueil (si installées) */
static const char *pinned_ids[] = { "nexus-files", "nexus-browser", "nexus-store", "nexus-terminal", "nexus-settings", "nexus-editor",
                                    "nexus-monitor", "nexus-about" };
static int pinned[8], npinned;

/* ======================= Fichiers (récents, recherche) ======================= */

static const char *ext_of(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && d != n ? d + 1 : "";
}

static const char *file_icon(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return "folder";
    const char *e = ext_of(path);
    if (!strcasecmp(e, "png") || !strcasecmp(e, "jpg")) return "image";
    static const char *txt[] = { "txt", "md", "conf", "sh", "c", "h", "py", "lua", "json", "log", "csv" };
    for (unsigned i = 0; i < sizeof txt / sizeof txt[0]; i++) if (!strcasecmp(e, txt[i])) return "editor";
    if (!strcasecmp(e, "tar") || !strcasecmp(e, "gz") || !strcasecmp(e, "zip")) return "archive";
    return "file";
}

static void open_path(const char *path)
{
    char q[2200], cmd[2400];
    nx_shell_quote(q, sizeof q, path);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(cmd, sizeof cmd, "nexus-files %s", q);
    else if (!strcasecmp(ext_of(path), "png")) snprintf(cmd, sizeof cmd, "nexus-files --view %s", q);
    else if (!strcmp(file_icon(path), "editor"))
        snprintf(cmd, sizeof cmd, "foot --app-id=nexus-editor --title='Éditeur de texte' nano %s", q);
    else {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", path);
        char *s = strrchr(dir, '/');
        if (s) *s = 0;
        nx_shell_quote(q, sizeof q, dir);
        snprintf(cmd, sizeof cmd, "nexus-files %s", q);
    }
    if (!(stat(path, &st) == 0 && S_ISDIR(st.st_mode))) nx_recent_add(path);
    nx_spawn(cmd);
}

static struct nx_recent recents[4];
static int nrecents;

/* Résultats de recherche : applications puis fichiers */
struct result { bool is_app; int app; char path[1024]; };
static struct result results[12];
static int nresults;
static char search[128];

static void search_files(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nresults < 12) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (nx_match(e->d_name, search)) {
            results[nresults].is_app = false;
            snprintf(results[nresults].path, sizeof results[nresults].path, "%s", p);
            nresults++;
        }
        struct stat st;
        if (depth > 0 && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) search_files(p, depth - 1);
    }
    closedir(d);
}

/* ======================= État ======================= */

enum page { P_HOME, P_APPS, P_POWER };
static enum page page = P_HOME;
static int selected, scroll;
static struct nx_window *menu;
static char username[64] = "Utilisateur";

static void refilter(void)
{
    nresults = 0;
    selected = 0;
    scroll = 0;
    if (!search[0]) return;
    for (int i = 0; i < napps && nresults < 12; i++)
        if (nx_match(apps[i].name, search) || nx_match(apps[i].comment, search)) {
            results[nresults].is_app = true;
            results[nresults].app = i;
            nresults++;
        }
    const char *home = getenv("HOME");
    if (home && nresults < 12) search_files(home, 2);
}

/* ======================= Zones cliquables ======================= */

enum kind { K_OUTSIDE, K_INSIDE, K_NAV, K_USER, K_SHUTDOWN, K_SEARCH, K_PIN, K_SEEALL, K_RECENT,
            K_MORE, K_APP, K_RESULT, K_BACK, K_POWER };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[96];
static int nrects;
static int hover_kind = -1, hover_index = -1, press_kind = -1, press_index = -1;

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 96) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
}
static bool hot(enum kind k, int i) { return (int)k == hover_kind && i == hover_index; }

/* Navigation de gauche */
struct nav { const char *label, *icon; int page; const char *cmd; };
static const struct nav navs[] = {
    { "Accueil", "home", P_HOME, NULL },
    { "Applications", "apps", P_APPS, NULL },
    { "Fichiers", "folder", -1, "nexus-files" },
    { "Paramètres", "settings", -1, "nexus-settings" },
    { "Système", "display", -1, "nexus-settings --about" },
    { "Alimentation", "power", P_POWER, NULL },
};
#define NNAV ((int)(sizeof navs / sizeof navs[0]))

struct power { const char *label, *icon, *cmd; };
static const struct power powers[] = {
    { "Éteindre", "power", "/usr/local/libexec/nexus-power poweroff" },
    { "Redémarrer", "reboot", "/usr/local/libexec/nexus-power reboot" },
    { "Se déconnecter", "logout", "labwc --exit" },
};

/* ======================= Dessin ======================= */

struct box { double x, y, w, h, cx, cw; };
static struct box box_of(int width, int height)
{
    (void)width;
    struct box b;
    b.w = MENU_W;
    b.h = MENU_H < height - 16 ? MENU_H : height - 16;
    b.x = 8;
    b.y = height - b.h - 8;
    b.cx = b.x + NAV_W + 22;
    b.cw = b.w - NAV_W - 44;
    return b;
}

static void section_header(cairo_t *cr, double x, double y, double w, const char *title, const char *link,
                           enum kind k)
{
    nx_text(cr, title, 16, true, nx_rgb(NX_GLASS_TEXT), x, y, 0, 26, NX_LEFT);
    if (!link) return;
    nx_color a = nx_mix(nx_accent_color(), nx_rgb("#ffffff"), hot(k, 0) ? 0.45 : 0.25);
    double lw = nx_text_width(cr, link, 13.5, false);
    nx_text(cr, link, 13.5, false, a, x + w - lw - 4, y, 0, 26, NX_LEFT);
    add_rect(x + w - lw - 10, y, lw + 12, 26, k, 0);
}

static void draw_list_item(cairo_t *cr, double x, double y, double w, double h, const char *icon,
                           const char *title, const char *sub, bool is_hot)
{
    if (is_hot) {
        nx_rounded_rect(cr, x, y, w, h, 9);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.09);
        cairo_fill(cr);
    }
    double is = h - 16;
    if (icon[0] == '@') nx_app_icon(cr, icon + 1, x + 10, y + 8, is, nx_rgb("#ffffff"));
    else nx_icon(cr, icon, x + 10, y + 8, is, nx_rgb("#ffffff"));
    if (sub && sub[0]) {
        nx_text(cr, title, 14, false, nx_rgb(NX_GLASS_TEXT), x + is + 22, y + 6, w - is - 30, h / 2 - 4, NX_LEFT);
        nx_text(cr, sub, 12, false, nx_rgb(NX_GLASS_SUBTLE), x + is + 22, y + h / 2, w - is - 30, h / 2 - 6, NX_LEFT);
    } else {
        nx_text(cr, title, 14, false, nx_rgb(NX_GLASS_TEXT), x + is + 22, y, w - is - 30, h, NX_LEFT);
    }
}

static void draw_logical(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    add_rect(0, 0, width, height, K_OUTSIDE, 0);
    struct box b = box_of(width, height);
    nx_color accent = nx_accent_color(), white = nx_rgb("#ffffff");
    nx_color text = nx_rgb(NX_GLASS_TEXT), subtle = nx_rgb(NX_GLASS_SUBTLE);

    nx_glass(cr, b.x, b.y, b.w, b.h, 16);
    add_rect(b.x, b.y, b.w, b.h, K_INSIDE, 0);

    /* ---- Colonne de navigation ---- */
    cairo_save(cr);
    nx_rounded_rect(cr, b.x, b.y, b.w, b.h, 16);
    cairo_clip(cr);
    cairo_rectangle(cr, b.x, b.y, NAV_W, b.h);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_rectangle(cr, b.x + NAV_W, b.y + 16, 1, b.h - 32);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_fill(cr);
    nx_logo(cr, b.x + 22, b.y + 22, 44);
    for (int i = 0; i < NNAV; i++) {
        double y = b.y + 92 + i * 50, x = b.x + 12, nw = NAV_W - 24;
        bool cur = navs[i].page == (int)page && !search[0];
        if (cur) {
            cairo_pattern_t *g = cairo_pattern_create_linear(x, 0, x + nw, 0);
            cairo_pattern_add_color_stop_rgba(g, 0, accent.r, accent.g, accent.b, 0.95);
            cairo_pattern_add_color_stop_rgba(g, 1, accent.r, accent.g, accent.b, 0.55);
            nx_rounded_rect(cr, x, y, nw, 44, 10);
            cairo_set_source(cr, g);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
        } else if (hot(K_NAV, i)) {
            nx_rounded_rect(cr, x, y, nw, 44, 10);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
            cairo_fill(cr);
        }
        nx_icon(cr, navs[i].icon, x + 14, y + 11, 22, white);
        nx_text(cr, navs[i].label, 14.5, cur, cur ? white : text, x + 48, y, nw - 56, 44, NX_LEFT);
        add_rect(x, y, nw, 44, K_NAV, i);
    }
    /* Compte et Éteindre en bas */
    double uy = b.y + b.h - 108;
    if (hot(K_USER, 0)) {
        nx_rounded_rect(cr, b.x + 12, uy, NAV_W - 24, 48, 10);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
        cairo_fill(cr);
    }
    nx_icon(cr, "user", b.x + 22, uy + 7, 34, white);
    nx_text(cr, username, 14, true, text, b.x + 66, uy, NAV_W - 86, 48, NX_LEFT);
    add_rect(b.x + 12, uy, NAV_W - 24, 48, K_USER, 0);
    double sy = b.y + b.h - 56;
    if (hot(K_SHUTDOWN, 0)) {
        nx_rounded_rect(cr, b.x + 12, sy, NAV_W - 24, 44, 10);
        cairo_set_source_rgba(cr, 1, 0.35, 0.35, 0.18);
        cairo_fill(cr);
    }
    nx_icon(cr, "power", b.x + 26, sy + 11, 22, white);
    nx_text(cr, "Éteindre", 14.5, false, text, b.x + 60, sy, NAV_W - 80, 44, NX_LEFT);
    add_rect(b.x + 12, sy, NAV_W - 24, 44, K_SHUTDOWN, 0);

    /* ---- Barre de recherche ---- */
    double sx = b.cx, sw = b.cw, sh = 48, syy = b.y + 22;
    nx_rounded_rect(cr, sx, syy, sw, sh, 12);
    cairo_set_source_rgba(cr, 1, 1, 1, search[0] ? 0.10 : 0.06);
    cairo_fill_preserve(cr);
    if (search[0]) nx_set(cr, nx_alpha(accent, 0.8)); else cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_set_line_width(cr, search[0] ? 1.6 : 1);
    cairo_stroke(cr);
    nx_icon(cr, "search", sx + 16, syy + 13, 22, nx_alpha(white, 0.85));
    if (search[0]) {
        double tw = nx_text(cr, search, 15, false, text, sx + 52, syy, sw - 70, sh, NX_LEFT);
        cairo_rectangle(cr, sx + 53 + tw, syy + 13, 1.5, sh - 26);
        nx_set(cr, accent);
        cairo_fill(cr);
    } else {
        nx_text(cr, "Rechercher une application, un fichier…", 15, false, subtle, sx + 52, syy, sw - 70, sh, NX_LEFT);
    }
    add_rect(sx, syy, sw, sh, K_SEARCH, 0);

    double y = syy + sh + 24;
    if (search[0]) {
        /* ---- Résultats de recherche ---- */
        section_header(cr, b.cx, y, b.cw, "Résultats", NULL, K_INSIDE);
        y += 36;
        if (nresults == 0)
            nx_text(cr, "Aucun résultat", 14, false, subtle, b.cx, y + 20, b.cw, 30, NX_CENTER);
        for (int i = 0; i < nresults && y + 56 < b.y + b.h - 10; i++, y += 56) {
            const char *icon, *title, *sub;
            char ago[64] = "";
            if (results[i].is_app) {
                struct app *a = &apps[results[i].app];
                icon = app_icon(a); title = a->name; sub = a->comment[0] ? a->comment : "Application";
            } else {
                icon = file_icon(results[i].path);
                title = strrchr(results[i].path, '/') + 1;
                const char *home = getenv("HOME");
                size_t hl = home ? strlen(home) : 0;
                snprintf(ago, sizeof ago, "%s%s", hl && !strncmp(results[i].path, home, hl) ? "~" : "",
                         hl && !strncmp(results[i].path, home, hl) ? results[i].path + hl : results[i].path);
                sub = ago;
            }
            draw_list_item(cr, b.cx - 6, y, b.cw + 12, 52, icon, title, sub, hot(K_RESULT, i) || i == selected);
            add_rect(b.cx - 6, y, b.cw + 12, 52, K_RESULT, i);
        }
    } else if (page == P_HOME) {
        /* ---- Applications épinglées ---- */
        section_header(cr, b.cx, y, b.cw, "Applications épinglées", "Tout voir  →", K_SEEALL);
        y += 38;
        double isz = 52 * nx_icon_factor();
        double cellw = b.cw / 4, cellh = isz + 52;
        for (int i = 0; i < npinned; i++) {
            double x = b.cx + (i % 4) * cellw, cy = y + (i / 4) * cellh;
            struct app *a = &apps[pinned[i]];
            if (hot(K_PIN, i)) {
                nx_rounded_rect(cr, x + 6, cy, cellw - 12, cellh - 6, 12);
                cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
                cairo_fill(cr);
            }
            nx_app_icon(cr, app_icon(a) + 1, x + (cellw - isz) / 2, cy + 10, isz, white);
            nx_text(cr, a->name, 13.5, false, text, x + 4, cy + isz + 16, cellw - 8, 24, NX_CENTER);
            add_rect(x + 6, cy, cellw - 12, cellh - 6, K_PIN, i);
        }
        y += ((npinned + 3) / 4) * cellh + 12;
        /* ---- Récents ---- */
        section_header(cr, b.cx, y, b.cw, "Récents", "Plus  →", K_MORE);
        y += 36;
        if (nrecents == 0)
            nx_text(cr, "Les fichiers que tu ouvres apparaîtront ici.", 13.5, false, subtle,
                    b.cx + 4, y + 4, b.cw, 30, NX_LEFT);
        for (int i = 0; i < nrecents && y + 56 < b.y + b.h - 10; i++, y += 56) {
            char ago[64];
            nx_ago(ago, sizeof ago, recents[i].when);
            draw_list_item(cr, b.cx - 6, y, b.cw + 12, 52, file_icon(recents[i].path),
                           strrchr(recents[i].path, '/') + 1, ago, hot(K_RECENT, i));
            add_rect(b.cx - 6, y, b.cw + 12, 52, K_RECENT, i);
        }
    } else if (page == P_APPS) {
        /* ---- Toutes les applications ---- */
        section_header(cr, b.cx, y, b.cw, "Toutes les applications", "←  Retour", K_BACK);
        y += 38;
        double ly = y, lh = b.y + b.h - 14 - ly;
        int visible = (int)(lh / 56);
        if (scroll > napps - visible) scroll = napps - visible;
        if (scroll < 0) scroll = 0;
        for (int k = 0; k < visible && scroll + k < napps; k++) {
            int i = scroll + k;
            draw_list_item(cr, b.cx - 6, ly + k * 56, b.cw + 12, 52, app_icon(&apps[i]), apps[i].name,
                           apps[i].comment, hot(K_APP, i) || i == selected);
            add_rect(b.cx - 6, ly + k * 56, b.cw + 12, 52, K_APP, i);
        }
    } else {
        /* ---- Alimentation ---- */
        section_header(cr, b.cx, y, b.cw, "Alimentation", "←  Retour", K_BACK);
        y += 50;
        double cw = (b.cw - 2 * 16) / 3, ch = 150;
        for (int i = 0; i < 3; i++) {
            double x = b.cx + i * (cw + 16);
            nx_rounded_rect(cr, x, y, cw, ch, 14);
            if (hot(K_POWER, i)) {
                if (i == 0) cairo_set_source_rgba(cr, 0.9, 0.3, 0.3, 0.30);
                else nx_set(cr, nx_alpha(accent, 0.35));
            } else {
                cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
            }
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
            nx_icon(cr, powers[i].icon, x + (cw - 46) / 2, y + 30, 46, white);
            nx_text(cr, powers[i].label, 14.5, true, text, x, y + 96, cw, 30, NX_CENTER);
            add_rect(x, y, cw, ch, K_POWER, i);
        }
        nx_text(cr, "« Se déconnecter » ferme ta session et revient à l'écran de connexion.",
                12.5, false, subtle, b.cx, y + ch + 18, b.cw, 22, NX_LEFT);
    }
}

/* ======================= Actions ======================= */

static void run_and_quit(const char *cmd) { nx_spawn(cmd); nx_quit(); }

static void activate_result(int i)
{
    if (i < 0 || i >= nresults) return;
    if (results[i].is_app) run_and_quit(apps[results[i].app].exec);
    else { open_path(results[i].path); nx_quit(); }
}

static void activate(enum kind k, int i)
{
    switch (k) {
    case K_OUTSIDE: nx_quit(); break;
    case K_NAV:
        if (navs[i].cmd) run_and_quit(navs[i].cmd);
        else { page = navs[i].page; search[0] = 0; scroll = 0; selected = 0; nx_redraw(menu); }
        break;
    case K_USER: run_and_quit("nexus-files"); break;
    case K_SHUTDOWN: run_and_quit(powers[0].cmd); break;
    case K_SEEALL: page = P_APPS; scroll = 0; nx_redraw(menu); break;
    case K_BACK: page = P_HOME; nx_redraw(menu); break;
    case K_MORE: run_and_quit("nexus-files"); break;
    case K_PIN: run_and_quit(apps[pinned[i]].exec); break;
    case K_APP: run_and_quit(apps[i].exec); break;
    case K_RECENT: open_path(recents[i].path); nx_quit(); break;
    case K_RESULT: activate_result(i); break;
    case K_POWER: run_and_quit(powers[i].cmd); break;
    default: break;
    }
}

static struct rect *hit(double x, double y)
{
    for (int i = nrects - 1; i >= 0; i--)
        if (x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h)
            return &rects[i];
    return NULL;
}

static void pointer_logical(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct rect *r = hit(x, y);
    int k = r ? (int)r->kind : -1, i = r ? r->index : -1;
    switch (ev) {
    case NX_MOTION:
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
        break;
    case NX_LEAVE: hover_kind = hover_index = -1; nx_redraw(w); break;
    case NX_PRESS: press_kind = k; press_index = i; break;
    case NX_RELEASE:
        if (value == BTN_LEFT && k == press_kind && i == press_index && r) activate(r->kind, r->index);
        break;
    case NX_SCROLL:
        if (page == P_APPS && !search[0]) { scroll += value; nx_redraw(w); }
        break;
    }
}

/* ---------- Taille du menu (réglage menu_scale, ou en tirant son bord) ----------
 * Le menu est dessiné dans un repère « logique » de 800 × 610 puis agrandi :
 * texte, icônes et marges grandissent ensemble. */

static double cur_scale = 1;
static int surf_w, surf_h;
static int resize_mode;          /* 0 non, 1 bord haut, 2 bord droit, 3 coin haut-droit */
static int grip_hover;

static double fit_scale(double s, int width, int height)
{
    if (s < 0.8) s = 0.8;
    if (s > 1.7) s = 1.7;
    double sx = (width - 16.0) / MENU_W, sy = (height - 16.0) / MENU_H;
    if (s > sx) s = sx;
    if (s > sy) s = sy;
    return s < 0.5 ? 0.5 : s;
}

/* Zone de redimensionnement sous le curseur (coordonnées réelles) */
static int grip_at(double x, double y)
{
    struct box b = box_of((int)(surf_w / cur_scale), (int)(surf_h / cur_scale));
    double bx = b.x * cur_scale, by = b.y * cur_scale, bw = b.w * cur_scale, m = 10;
    bool top = y >= by - m && y <= by + m && x >= bx && x <= bx + bw + m;
    bool right = x >= bx + bw - m && x <= bx + bw + m && y >= by - m;
    if (top && right) return 3;
    if (top) return 1;
    if (right) return 2;
    return 0;
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    surf_w = width;
    surf_h = height;
    cur_scale = fit_scale(nx_conf.menu_scale / 100.0, width, height);
    cairo_save(cr);
    cairo_scale(cr, cur_scale, cur_scale);
    draw_logical(w, cr, (int)(width / cur_scale), (int)(height / cur_scale));
    cairo_restore(cr);
    /* poignée du coin haut-droit (et liseré du bord survolé) */
    struct box b = box_of((int)(width / cur_scale), (int)(height / cur_scale));
    double rx = (b.x + b.w) * cur_scale, ty = b.y * cur_scale;
    nx_color a = nx_accent_color();
    cairo_set_line_width(cr, 2);
    for (int i = 0; i < 3; i++) {
        cairo_move_to(cr, rx - 8 - i * 5, ty + 3);
        cairo_line_to(cr, rx - 3, ty + 8 + i * 5);
    }
    if (grip_hover || resize_mode) nx_set(cr, a); else cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
    cairo_stroke(cr);
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    if (resize_mode) {
        if (ev == NX_MOTION) {
            /* menu ancré en bas à gauche (marge 8) : la taille suit le curseur */
            double sx = (x - 8) / MENU_W, sy = (surf_h - y - 8) / MENU_H, s = cur_scale;
            if (resize_mode == 1) s = sy;
            else if (resize_mode == 2) s = sx;
            else s = sx > sy ? sx : sy;
            nx_conf.menu_scale = (int)(fit_scale(s, surf_w, surf_h) * 100 + 0.5);
            nx_redraw(w);
        } else if (ev == NX_RELEASE) {
            resize_mode = 0;
            nx_config_save(&nx_conf);
            nx_redraw(w);
        }
        return;
    }
    int g = ev == NX_LEAVE ? 0 : grip_at(x, y);
    if ((g != 0) != (grip_hover != 0)) { grip_hover = g; nx_redraw(w); }
    if (g && ev == NX_PRESS && value == BTN_LEFT) { resize_mode = g; return; }
    if (g && (ev == NX_PRESS || ev == NX_RELEASE)) return;
    pointer_logical(w, ev, x / cur_scale, y / cur_scale, value);
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    switch (sym) {
    case XKB_KEY_Escape:
        if (search[0]) { search[0] = 0; refilter(); }
        else if (page != P_HOME) page = P_HOME;
        else { nx_quit(); return; }
        break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter:
        if (search[0]) activate_result(selected);
        else if (page == P_APPS && selected < napps) run_and_quit(apps[selected].exec);
        return;
    case XKB_KEY_Down:
        if (search[0] ? selected < nresults - 1 : (page == P_APPS && selected < napps - 1)) selected++;
        break;
    case XKB_KEY_Up:
        if (selected > 0) selected--;
        break;
    case XKB_KEY_BackSpace: {
        size_t n = strlen(search);
        while (n > 0 && ((unsigned char)search[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        search[n] = 0;
        refilter();
        break;
    }
    default:
        if (utf8[0] && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
            strlen(search) + strlen(utf8) < sizeof search - 1) {
            strcat(search, utf8);
            refilter();
        } else {
            return;
        }
    }
    nx_redraw(w);
}

/* ======================= Un seul menu à la fois ======================= */

static char pidfile[512];

static bool toggle_existing(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(pidfile, sizeof pidfile, "%s/nexus-menu.pid", rt ? rt : "/tmp");
    FILE *f = fopen(pidfile, "r");
    if (f) {
        int pid = 0;
        if (fscanf(f, "%d", &pid) == 1 && pid > 1 && kill(pid, 0) == 0) {
            fclose(f);
            kill(pid, SIGTERM);
            return true;
        }
        fclose(f);
    }
    f = fopen(pidfile, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    return false;
}

static void reload(void) { nx_redraw(menu); }

int menu_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (toggle_existing()) return 0;
    struct passwd *pw = getpwuid(getuid());
    if (pw) snprintf(username, sizeof username, "%s", getuid() == 0 ? "Administrateur" : pw->pw_name);
    load_dir("/usr/share/applications");          /* logiciels Alpine (npk alpine) */
    load_dir("/usr/local/share/applications");
    char p[512];
    snprintf(p, sizeof p, "%s/.local/share/applications", getenv("HOME") ? getenv("HOME") : "/");
    load_dir(p);
    qsort(apps, napps, sizeof apps[0], cmp_app);
    for (unsigned k = 0; k < sizeof pinned_ids / sizeof pinned_ids[0] && npinned < 8; k++)
        for (int i = 0; i < napps; i++)
            if (!strcmp(apps[i].id, pinned_ids[k])) pinned[npinned++] = i;
    nrecents = nx_recent_load(recents, 3);

    if (!nx_init()) { unlink(pidfile); return 1; }
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    menu = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                           ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                           0, 0, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, "nexus-menu");
    if (menu) {
        nx.reload = reload;
        nx_run();
    }
    unlink(pidfile);
    return 0;
}
