/* nexus-files — le gestionnaire de fichiers de Nexus OS.
 *
 *  ┌──────────────┬────────────────────────────────────────────┐
 *  │ ◀ ▶ ▲  Dossier personnel › Images      [Nouveau dossier] │
 *  ├──────────────┼────────────────────────────────────────────┤
 *  │ Dossier pers.│  [dossier] [dossier] [image] [texte] ...   │
 *  │ Documents    │                                            │
 *  │ Images       │                                            │
 *  │ ...          │                                            │
 *  │ Corbeille    │                                            │
 *  ├──────────────┴────────────────────────────────────────────┤
 *  │ 12 éléments                          Espace libre : 3,2 Go │
 *  └───────────────────────────────────────────────────────────┘
 *
 * Double-clic : ouvrir (dossier, texte dans l'éditeur, image PNG/JPEG dans la
 * visionneuse). Miniatures des images. Glisser-déposer des éléments sur un
 * dossier ou un emplacement de la barre latérale : déplacer (Ctrl : copier ;
 * sur la Corbeille : mettre à la corbeille). Clic droit : menu. Supprimer = mettre à la corbeille
 * (~/.local/share/Trash, format freedesktop simplifié), donc rattrapable.
 * Raccourcis : Ctrl+C/X/V/A, Suppr, F2, F5, Retour arrière, Ctrl+H.
 *
 * Options : nexus-files [dossier]   |   nexus-files --view image.png
 */
#define _GNU_SOURCE
#include "nexus-ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

#define WIN_W     940
#define WIN_H     600
#define TOOL_H    56
#define SIDE_W    210
#define STATUS_H  32
#define CELL_W    118
#define CELL_H    112

/* ======================= Données ======================= */

struct entry {
    char name[256];
    bool dir, exec;
    off_t size;
    cairo_surface_t *thumb;   /* miniature (images), créée à l'affichage */
    bool thumb_tried;
};

static struct entry *ents;
static bool *sel;
static int nents, ents_cap;
static char cwd[PATH_MAX];
static char home[PATH_MAX], trash_files[PATH_MAX], trash_info[PATH_MAX];
static bool show_hidden;
static int scroll_y, focus = -1;
static char status_msg[256];
static time_t status_until;

static char history[32][PATH_MAX];
static int hist_len, hist_pos;

/* Presse-papiers interne : chemins + couper/copier */
static char **clip;
static int nclip;
static bool clip_cut;

static struct nx_window *win;

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_msg, sizeof status_msg, fmt, ap);
    va_end(ap);
    status_until = time(NULL) + 5;
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
}

static bool in_trash(void) { return !strcmp(cwd, trash_files); }

static const char *ext_of(const char *name)
{
    const char *d = strrchr(name, '.');
    return d && d != name ? d + 1 : "";
}

static bool is_png(const char *name) { return !strcasecmp(ext_of(name), "png"); }
static bool is_jpeg(const char *name)
{
    return !strcasecmp(ext_of(name), "jpg") || !strcasecmp(ext_of(name), "jpeg");
}
static bool is_image(const char *name) { return is_png(name) || is_jpeg(name); }

/* Miniature : image réduite pour tenir dans size x size */
static cairo_surface_t *make_thumb(const char *path, int size)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size > 40 * 1024 * 1024) return NULL;   /* trop gros : icône */
    cairo_surface_t *img = nx_load_image(path, size);
    if (!img) return NULL;
    double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    double k = fmin(size / iw, size / ih);
    if (k > 1) k = 1;
    int tw = (int)(iw * k) > 0 ? (int)(iw * k) : 1, th = (int)(ih * k) > 0 ? (int)(ih * k) : 1;
    cairo_surface_t *t = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
    cairo_t *cr = cairo_create(t);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(img);
    return t;
}

static bool is_archive(const char *name)
{
    static const char *e[] = { "tar", "gz", "xz", "bz2", "zip", "npk", "tgz", "zst", "iso" };
    for (unsigned i = 0; i < sizeof e / sizeof e[0]; i++)
        if (!strcasecmp(ext_of(name), e[i])) return true;
    return false;
}

/* Un fichier "texte" : pas d'octet nul dans le premier kilo-octet */
static bool looks_text(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < 0) return false;
    return memchr(buf, 0, n) == NULL;
}

static const char *icon_for(const struct entry *e)
{
    if (e->dir) return "folder";
    if (is_image(e->name)) return "image";
    if (is_archive(e->name)) return "archive";
    if (e->exec) return "exec";
    static const char *txt[] = { "txt", "md", "conf", "cfg", "ini", "sh", "c", "h", "py", "lua",
                                 "json", "xml", "html", "css", "js", "log", "desktop", "csv" };
    for (unsigned i = 0; i < sizeof txt / sizeof txt[0]; i++)
        if (!strcasecmp(ext_of(e->name), txt[i])) return "editor";
    return "file";
}

static void human_size(char *out, size_t n, double b)
{
    const char *u[] = { "octets", "Ko", "Mo", "Go", "To" };
    int i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; i++; }
    if (i == 0) snprintf(out, n, "%.0f %s", b, u[i]);
    else snprintf(out, n, "%.1f %s", b, u[i]);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';   /* virgule décimale */
}

static int cmp_entry(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;     /* dossiers d'abord */
    return strcasecmp(x->name, y->name);
}

static bool load_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        set_status("Impossible d'ouvrir « %s » : %s", path, strerror(errno));
        return false;
    }
    for (int i = 0; i < nents; i++)
        if (ents[i].thumb) { cairo_surface_destroy(ents[i].thumb); ents[i].thumb = NULL; }
    nents = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (de->d_name[0] == '.' && !show_hidden) continue;
        if (nents == ents_cap) {
            ents_cap = ents_cap ? ents_cap * 2 : 128;
            ents = realloc(ents, ents_cap * sizeof *ents);
            sel = realloc(sel, ents_cap * sizeof *sel);
        }
        struct entry *e = &ents[nents];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char full[PATH_MAX];
        join(full, sizeof full, path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 && lstat(full, &st) != 0) memset(&st, 0, sizeof st);
        e->dir = S_ISDIR(st.st_mode);
        e->exec = !e->dir && (st.st_mode & 0111);
        e->size = st.st_size;
        e->thumb = NULL;
        e->thumb_tried = false;
        nents++;
    }
    closedir(d);
    qsort(ents, nents, sizeof *ents, cmp_entry);
    if (sel) memset(sel, 0, ents_cap * sizeof *sel);
    return true;
}

static void refresh(void)
{
    load_dir(cwd);
    if (focus >= nents) focus = nents - 1;
    nx_redraw(win);
}

static void navigate(const char *path, bool push)
{
    char real[PATH_MAX];
    if (!realpath(path, real)) {
        set_status("Dossier introuvable : %s", path);
        nx_redraw(win);
        return;
    }
    char old[PATH_MAX];
    snprintf(old, sizeof old, "%s", cwd);
    snprintf(cwd, sizeof cwd, "%s", real);
    if (!load_dir(cwd)) {
        snprintf(cwd, sizeof cwd, "%s", old);
        nx_redraw(win);
        return;
    }
    if (push) {
        if (hist_len == 0 || strcmp(history[hist_pos], cwd)) {
            hist_len = (hist_len ? hist_pos + 1 : 0);
            if (hist_len == 32) { memmove(history, history + 1, sizeof history[0] * 31); hist_len = 31; }
            snprintf(history[hist_len], PATH_MAX, "%s", cwd);
            hist_pos = hist_len++;
        }
    }
    scroll_y = 0;
    focus = -1;
    if (win && win->toplevel) {
        const char *base = strrchr(cwd, '/');
        char title[300];
        snprintf(title, sizeof title, "%s — Fichiers", !strcmp(cwd, "/") ? "Système" :
                 !strcmp(cwd, home) ? "Dossier personnel" : in_trash() ? "Corbeille" : base + 1);
        xdg_toplevel_set_title(win->toplevel, title);
    }
    nx_redraw(win);
}

/* ======================= Opérations sur les fichiers ======================= */

static int remove_path(const char *p)
{
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (!d) return -1;
        struct dirent *de;
        char c[PATH_MAX];
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            join(c, sizeof c, p, de->d_name);
            remove_path(c);
        }
        closedir(d);
        return rmdir(p);
    }
    return unlink(p);
}

static int copy_path(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) return -1;
        target[n] = 0;
        return symlink(target, dst);
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) != 0 && errno != EEXIST) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *de;
        char s[PATH_MAX], t[PATH_MAX];
        int rc = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            join(s, sizeof s, src, de->d_name);
            join(t, sizeof t, dst, de->d_name);
            if (copy_path(s, t) != 0) rc = -1;
        }
        closedir(d);
        return rc;
    }
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) { rc = -1; break; }
    if (n < 0) rc = -1;
    close(in);
    close(out);
    return rc;
}

static int move_path(const char *src, const char *dst)
{
    if (rename(src, dst) == 0) return 0;
    if (errno != EXDEV) return -1;
    if (copy_path(src, dst) != 0) return -1;     /* autre disque : copier puis effacer */
    return remove_path(src);
}

/* "nom.ext" -> "nom (2).ext" tant que le nom existe déjà dans dir */
static void unique_name(char *out, size_t n, const char *dir, const char *name)
{
    char path[PATH_MAX];
    join(path, sizeof path, dir, name);
    if (access(path, F_OK) != 0) { snprintf(out, n, "%s", name); return; }
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int i = 2; i < 1000; i++) {
        snprintf(out, n, "%.*s (%d)%s", stem, name, i, name + stem);
        join(path, sizeof path, dir, out);
        if (access(path, F_OK) != 0) return;
    }
}

static int trash_one(const char *path)
{
    mkdir(trash_files, 0700);   /* ~/.local/share/Trash existe (créé au démarrage) */
    mkdir(trash_info, 0700);
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    char name[256], dst[PATH_MAX], info[PATH_MAX];
    unique_name(name, sizeof name, trash_files, base);
    join(dst, sizeof dst, trash_files, name);
    if (move_path(path, dst) != 0) return -1;
    snprintf(info, sizeof info, "%s/%s.trashinfo", trash_info, name);
    FILE *f = fopen(info, "w");
    if (f) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char date[32];
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
        fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", path, date);
        fclose(f);
    }
    return 0;
}

static int restore_one(const char *name)
{
    char info[PATH_MAX], src[PATH_MAX], orig[PATH_MAX] = "";
    snprintf(info, sizeof info, "%s/%s.trashinfo", trash_info, name);
    FILE *f = fopen(info, "r");
    if (f) {
        char line[PATH_MAX + 16];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "Path=", 5)) {
                line[strcspn(line, "\n")] = 0;
                snprintf(orig, sizeof orig, "%s", line + 5);
            }
        fclose(f);
    }
    if (!orig[0]) join(orig, sizeof orig, home, name);   /* origine inconnue : dossier personnel */
    char dir[PATH_MAX], uname[256], dst[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", orig);
    char *slash = strrchr(dir, '/');
    const char *base = slash ? slash + 1 : orig;
    if (slash) *slash = 0;
    if (!dir[0]) snprintf(dir, sizeof dir, "/");
    unique_name(uname, sizeof uname, dir, base);
    join(dst, sizeof dst, dir, uname);
    join(src, sizeof src, trash_files, name);
    if (move_path(src, dst) != 0) return -1;
    unlink(info);
    return 0;
}

static int selected_count(void)
{
    int n = 0;
    for (int i = 0; i < nents; i++) n += sel[i];
    return n;
}
static int first_selected(void)
{
    for (int i = 0; i < nents; i++) if (sel[i]) return i;
    return -1;
}

static void sel_path(int i, char *out, size_t n) { join(out, n, cwd, ents[i].name); }

static void do_trash(void)
{
    int ok = 0, fail = 0;
    for (int i = 0; i < nents; i++) {
        if (!sel[i]) continue;
        char p[PATH_MAX];
        sel_path(i, p, sizeof p);
        if (trash_one(p) == 0) ok++; else fail++;
    }
    if (fail) set_status("%d élément(s) mis à la corbeille, %d échec(s) : %s", ok, fail, strerror(errno));
    else set_status("%d élément(s) mis à la corbeille", ok);
    refresh();
}

static void do_restore(void)
{
    int ok = 0;
    for (int i = 0; i < nents; i++)
        if (sel[i] && restore_one(ents[i].name) == 0) ok++;
    set_status("%d élément(s) restauré(s)", ok);
    refresh();
}

static void do_delete_forever(void)
{
    int ok = 0;
    for (int i = 0; i < nents; i++) {
        if (!sel[i]) continue;
        char p[PATH_MAX], info[PATH_MAX];
        sel_path(i, p, sizeof p);
        if (remove_path(p) == 0) ok++;
        snprintf(info, sizeof info, "%s/%s.trashinfo", trash_info, ents[i].name);
        unlink(info);
    }
    set_status("%d élément(s) supprimé(s) définitivement", ok);
    refresh();
}

static void do_empty_trash(void)
{
    remove_path(trash_files);
    remove_path(trash_info);
    mkdir(trash_files, 0700);
    mkdir(trash_info, 0700);
    set_status("Corbeille vidée");
    refresh();
}

static void clip_set(bool cut)
{
    for (int i = 0; i < nclip; i++) free(clip[i]);
    free(clip);
    nclip = 0;
    clip = calloc(selected_count() + 1, sizeof *clip);
    for (int i = 0; i < nents; i++) {
        if (!sel[i]) continue;
        char p[PATH_MAX];
        sel_path(i, p, sizeof p);
        clip[nclip++] = strdup(p);
    }
    clip_cut = cut;
    set_status("%d élément(s) %s : utilise Coller dans le dossier de destination",
               nclip, cut ? "coupé(s)" : "copié(s)");
    nx_redraw(win);
}

static void do_paste(void)
{
    int ok = 0, fail = 0;
    for (int i = 0; i < nclip; i++) {
        const char *base = strrchr(clip[i], '/') ? strrchr(clip[i], '/') + 1 : clip[i];
        char name[256], dst[PATH_MAX];
        size_t cl = strlen(clip[i]);
        if (!strncmp(cwd, clip[i], cl) && (cwd[cl] == '/' || cwd[cl] == 0)) {
            fail++;   /* copier un dossier dans lui-même */
            continue;
        }
        unique_name(name, sizeof name, cwd, base);
        join(dst, sizeof dst, cwd, name);
        int rc = clip_cut ? move_path(clip[i], dst) : copy_path(clip[i], dst);
        if (rc == 0) ok++; else fail++;
    }
    if (clip_cut && fail == 0) { for (int i = 0; i < nclip; i++) free(clip[i]); nclip = 0; }
    if (fail) set_status("%d élément(s) collé(s), %d échec(s)", ok, fail);
    else set_status("%d élément(s) collé(s)", ok);
    refresh();
}

static void open_entry(int i)
{
    if (i < 0 || i >= nents) return;
    char p[PATH_MAX], q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 128];
    sel_path(i, p, sizeof p);
    if (ents[i].dir) { navigate(p, true); return; }
    if (in_trash()) { set_status("Restaure d'abord l'élément pour l'ouvrir."); nx_redraw(win); return; }
    nx_shell_quote(q, sizeof q, p);
    if (is_image(ents[i].name)) {
        snprintf(cmd, sizeof cmd, "nexus-files --view %s", q);
    } else if (looks_text(p)) {
        snprintf(cmd, sizeof cmd, "nexus-editor %s", q);
    } else {
        set_status("Nexus OS ne sait pas encore ouvrir ce type de fichier.");
        nx_redraw(win);
        return;
    }
    nx_spawn(cmd);
    nx_recent_add(p);
    set_status("Ouverture de « %s »…", ents[i].name);
    nx_redraw(win);
}

/* ======================= Menus et boîtes de dialogue ======================= */

enum action {
    A_NONE, A_OPEN, A_CUT, A_COPY, A_PASTE, A_RENAME, A_TRASH, A_RESTORE, A_DELETE,
    A_NEWDIR, A_HIDDEN, A_REFRESH, A_EMPTY, A_SHORTCUT,
};
struct menu_item { const char *label; enum action act; };
static struct menu_item menu[10];
static int nmenu;
static bool menu_open;
static double menu_x, menu_y;

enum dlg { DLG_NONE, DLG_RENAME, DLG_NEWDIR, DLG_EMPTY, DLG_DELETE };
static enum dlg dialog;
static char input[256];
static int dialog_target = -1;

static void add_menu(const char *label, enum action a)
{
    if (nmenu < 10) menu[nmenu++] = (struct menu_item){ label, a };
}

static void open_menu(double x, double y, bool on_item)
{
    nmenu = 0;
    if (on_item) {
        if (in_trash()) {
            add_menu("Restaurer", A_RESTORE);
            add_menu("Supprimer définitivement", A_DELETE);
        } else {
            if (selected_count() == 1) add_menu("Ouvrir", A_OPEN);
            add_menu("Couper", A_CUT);
            add_menu("Copier", A_COPY);
            if (selected_count() == 1) add_menu("Renommer", A_RENAME);
            if (selected_count() == 1 && first_selected() >= 0 && ents[first_selected()].dir)
                add_menu("Ajouter aux raccourcis", A_SHORTCUT);
            add_menu("Mettre à la corbeille", A_TRASH);
        }
    } else if (in_trash()) {
        add_menu("Vider la corbeille", A_EMPTY);
        add_menu("Actualiser", A_REFRESH);
    } else {
        add_menu("Nouveau dossier", A_NEWDIR);
        if (nclip) add_menu("Coller", A_PASTE);
        add_menu(show_hidden ? "Masquer les fichiers cachés" : "Afficher les fichiers cachés", A_HIDDEN);
        add_menu("Actualiser", A_REFRESH);
    }
    menu_x = x;
    menu_y = y;
    menu_open = true;
    nx_redraw(win);
}

static void start_dialog(enum dlg d)
{
    dialog = d;
    input[0] = 0;
    if (d == DLG_RENAME) {
        dialog_target = first_selected();
        if (dialog_target < 0) { dialog = DLG_NONE; return; }
        snprintf(input, sizeof input, "%s", ents[dialog_target].name);
    } else if (d == DLG_NEWDIR) {
        snprintf(input, sizeof input, "Nouveau dossier");
    }
    nx_redraw(win);
}

static void confirm_dialog(void)
{
    enum dlg d = dialog;
    dialog = DLG_NONE;
    if (d == DLG_RENAME && dialog_target >= 0 && input[0] && !strchr(input, '/')) {
        char from[PATH_MAX], to[PATH_MAX];
        sel_path(dialog_target, from, sizeof from);
        join(to, sizeof to, cwd, input);
        if (strcmp(from, to) && access(to, F_OK) == 0) set_status("« %s » existe déjà.", input);
        else if (rename(from, to) != 0) set_status("Impossible de renommer : %s", strerror(errno));
        else set_status("Renommé en « %s »", input);
        refresh();
    } else if (d == DLG_NEWDIR && input[0] && !strchr(input, '/')) {
        char name[256], p[PATH_MAX];
        unique_name(name, sizeof name, cwd, input);
        join(p, sizeof p, cwd, name);
        if (mkdir(p, 0755) != 0) set_status("Impossible de créer le dossier : %s", strerror(errno));
        else set_status("Dossier « %s » créé", name);
        refresh();
    } else if (d == DLG_EMPTY) {
        do_empty_trash();
    } else if (d == DLG_DELETE) {
        do_delete_forever();
    } else if ((d == DLG_RENAME || d == DLG_NEWDIR) && strchr(input, '/')) {
        set_status("Un nom ne peut pas contenir « / ».");
    }
    nx_redraw(win);
}

static void run_action(enum action a)
{
    menu_open = false;
    switch (a) {
    case A_OPEN: open_entry(first_selected()); break;
    case A_CUT: clip_set(true); break;
    case A_COPY: clip_set(false); break;
    case A_PASTE: do_paste(); break;
    case A_RENAME: start_dialog(DLG_RENAME); break;
    case A_TRASH: do_trash(); break;
    case A_RESTORE: do_restore(); break;
    case A_DELETE: start_dialog(DLG_DELETE); break;
    case A_NEWDIR: start_dialog(DLG_NEWDIR); break;
    case A_HIDDEN: show_hidden = !show_hidden; refresh(); break;
    case A_REFRESH: refresh(); break;
    case A_EMPTY: start_dialog(DLG_EMPTY); break;
    case A_SHORTCUT: {
        /* widget « Raccourcis » du bureau : 4 dossiers au plus */
        int i = first_selected();
        if (i < 0) break;
        char p[PATH_MAX], f[PATH_MAX], line[PATH_MAX + 300];
        sel_path(i, p, sizeof p);
        snprintf(f, sizeof f, "%s/.config/nexus/raccourcis", home);
        int n = 0;
        FILE *fp = fopen(f, "r");
        char existing[4][PATH_MAX + 300];
        if (fp) {
            while (n < 4 && fgets(existing[n], sizeof existing[n], fp)) n++;
            fclose(fp);
        } else {   /* premier ajout : garder les raccourcis par défaut */
            snprintf(existing[0], sizeof existing[0], "Bureau|%s\n", home);
            snprintf(existing[1], sizeof existing[1], "Images|%s/Images\n", home);
            snprintf(existing[2], sizeof existing[2], "Télécharg.|%s/Téléchargements\n", home);
            n = 3;
        }
        if (n >= 4) { set_status("Il y a déjà 4 raccourcis : enlève-en un dans ~/.config/nexus/raccourcis"); break; }
        char label[64];
        snprintf(label, sizeof label, "%.12s", ents[i].name);
        snprintf(line, sizeof line, "%s|%s\n", label, p);
        fp = fopen(f, "w");
        if (!fp) { set_status("Impossible d'enregistrer le raccourci"); break; }
        for (int k = 0; k < n; k++) fputs(existing[k], fp);
        fputs(line, fp);
        fclose(fp);
        nx_notify_reload();
        set_status("« %s » ajouté aux raccourcis du bureau", ents[i].name);
        break;
    }
    default: break;
    }
    nx_redraw(win);
}

/* ======================= Zones cliquables ======================= */

enum kind { K_BACK, K_FWD, K_UP, K_CRUMB, K_TOOLBTN, K_PLACE, K_ITEM, K_CONTENT,
            K_MENU, K_MENUBG, K_DLG_OK, K_DLG_CANCEL, K_DLG };
struct rect { double x, y, w, h; enum kind kind; int index; };
static struct rect rects[512];
static int nrects;
static int hover_kind = -1, hover_index = -1;

static void add_rect(double x, double y, double w, double h, enum kind k, int i)
{
    if (nrects < 512) rects[nrects++] = (struct rect){ x, y, w, h, k, i };
}
static bool hovered(enum kind k, int i) { return (int)k == hover_kind && i == hover_index; }

static struct rect *hit(double x, double y)
{
    for (int i = nrects - 1; i >= 0; i--)
        if (x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h)
            return &rects[i];
    return NULL;
}

/* Emplacements de la barre latérale */
struct place { const char *label, *icon; char path[PATH_MAX]; };
static struct place places[8];
static int nplaces;

static void add_place(const char *label, const char *icon, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode) || nplaces >= 8) return;
    places[nplaces].label = label;
    places[nplaces].icon = icon;
    snprintf(places[nplaces].path, PATH_MAX, "%s", path);
    nplaces++;
}

/* Fil d'Ariane : segments du chemin courant */
static char crumb_path[16][PATH_MAX];
static char crumb_label[16][128];
static int ncrumbs;

static void build_crumbs(void)
{
    ncrumbs = 0;
    const char *rest;
    if (in_trash()) {
        snprintf(crumb_label[0], 128, "Corbeille");
        snprintf(crumb_path[0], PATH_MAX, "%s", trash_files);
        ncrumbs = 1;
        return;
    }
    size_t hl = strlen(home);
    if (!strncmp(cwd, home, hl) && (cwd[hl] == '/' || cwd[hl] == 0) && strcmp(home, "/")) {
        snprintf(crumb_label[0], 128, "Dossier personnel");
        snprintf(crumb_path[0], PATH_MAX, "%s", home);
        rest = cwd + hl;
    } else {
        snprintf(crumb_label[0], 128, "Système");
        snprintf(crumb_path[0], PATH_MAX, "/");
        rest = cwd;
    }
    ncrumbs = 1;
    char acc[PATH_MAX];
    snprintf(acc, sizeof acc, "%s", crumb_path[0]);
    while (*rest && ncrumbs < 16) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/');
        int len = end ? (int)(end - rest) : (int)strlen(rest);
        snprintf(crumb_label[ncrumbs], 128, "%.*s", len, rest);
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s%s%.*s", acc, strcmp(acc, "/") ? "/" : "", len, rest);
        snprintf(acc, sizeof acc, "%s", tmp);
        snprintf(crumb_path[ncrumbs], PATH_MAX, "%s", acc);
        ncrumbs++;
        rest += len;
    }
}

/* ======================= Glisser-déposer ======================= */

static int drag_from = -1;           /* élément sous la souris au clic */
static bool dragging;                /* glissement en cours */
static double press_x, press_y, drag_x, drag_y;

/* Destination sous la souris : dossier de la grille (non sélectionné) ou
 * emplacement de la barre latérale ; NULL si aucune */
static const char *drop_target(int kind, int index, char *buf, size_t n)
{
    if (kind == K_ITEM && index >= 0 && index < nents && ents[index].dir && !sel[index]) {
        join(buf, n, cwd, ents[index].name);
        return buf;
    }
    if (kind == K_PLACE && index >= 0 && index < nplaces && strcmp(places[index].path, cwd)) {
        snprintf(buf, n, "%s", places[index].path);
        return buf;
    }
    return NULL;
}

/* Déposer la sélection dans dest : déplacer (ou copier si Ctrl) ;
 * la Corbeille = mettre à la corbeille */
static void do_drop(const char *dest, bool copy)
{
    if (!strcmp(dest, trash_files)) { do_trash(); return; }
    int ok = 0, fail = 0;
    for (int i = 0; i < nents; i++) {
        if (!sel[i]) continue;
        char src[PATH_MAX], name[256], dst[PATH_MAX];
        sel_path(i, src, sizeof src);
        size_t sl = strlen(src);
        if (!strncmp(dest, src, sl) && (dest[sl] == '/' || dest[sl] == 0)) { fail++; continue; }   /* dans lui-même */
        unique_name(name, sizeof name, dest, ents[i].name);
        join(dst, sizeof dst, dest, name);
        int rc = copy ? copy_path(src, dst) : move_path(src, dst);
        if (rc == 0) ok++; else fail++;
    }
    const char *where = strrchr(dest, '/') && dest[1] ? strrchr(dest, '/') + 1 : dest;
    if (fail) set_status("%d élément(s) %s vers « %s », %d échec(s)", ok, copy ? "copié(s)" : "déplacé(s)", where, fail);
    else set_status("%d élément(s) %s vers « %s »", ok, copy ? "copié(s)" : "déplacé(s)", where);
    refresh();
}

/* ======================= Dessin ======================= */

static int grid_cols(int width)
{
    int c = (width - SIDE_W - 24) / CELL_W;
    return c < 1 ? 1 : c;
}

static void draw_arrow(cairo_t *cr, double x, double y, int dir, bool enabled)
{
    /* dir : 0 gauche, 1 droite, 2 haut */
    cairo_save(cr);
    cairo_translate(cr, x + 18, y + 18);
    cairo_rotate(cr, dir == 0 ? M_PI : dir == 1 ? 0 : -M_PI / 2);
    cairo_move_to(cr, -4, -7);
    cairo_line_to(cr, 4, 0);
    cairo_line_to(cr, -4, 7);
    cairo_set_line_width(cr, 2.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    nx_set(cr, enabled ? nx_rgb(NX_TEXT) : nx_rgb("#b8bfcc"));
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void draw_button(cairo_t *cr, double x, double y, double w, double h, const char *label,
                        bool primary, bool hot)
{
    nx_color accent = nx_accent_color();
    nx_rounded_rect(cr, x, y, w, h, 7);
    if (primary) nx_set(cr, hot ? nx_mix(accent, nx_rgb("#ffffff"), 0.15) : accent);
    else nx_set(cr, hot ? nx_rgb("#e3e7ef") : nx_rgb("#eef1f6"));
    cairo_fill(cr);
    nx_text(cr, label, 13, primary, primary ? nx_rgb("#ffffff") : nx_rgb(NX_TEXT), x, y, w, h, NX_CENTER);
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    nx_color accent = nx_accent_color(), text = nx_rgb(NX_TEXT), subtle = nx_rgb(NX_SUBTLE);
    double cx = SIDE_W, cy = TOOL_H, cw = width - SIDE_W, ch = height - TOOL_H - STATUS_H;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    /* ---- Contenu : grille d'icônes ---- */
    add_rect(cx, cy, cw, ch, K_CONTENT, 0);
    int cols = grid_cols(width);
    double gx = cx + 12 + (cw - 24 - cols * CELL_W) / 2.0;
    int rows = (nents + cols - 1) / cols;
    int max_scroll = rows * CELL_H + 24 - (int)ch;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;
    if (scroll_y < 0) scroll_y = 0;

    cairo_save(cr);
    cairo_rectangle(cr, cx, cy, cw, ch);
    cairo_clip(cr);
    if (nents == 0)
        nx_text(cr, in_trash() ? "La corbeille est vide" : "Ce dossier est vide", 15, false, subtle,
                cx, cy + 60, cw, 30, NX_CENTER);
    for (int i = 0; i < nents; i++) {
        double x = gx + (i % cols) * CELL_W, y = cy + 12 + (i / cols) * CELL_H - scroll_y;
        if (y + CELL_H < cy || y > cy + ch) continue;
        char tb[PATH_MAX];
        bool target = dragging && hovered(K_ITEM, i) && drop_target(K_ITEM, i, tb, sizeof tb);
        if (target) {
            nx_rounded_rect(cr, x + 4, y + 2, CELL_W - 8, CELL_H - 6, 8);
            nx_set(cr, nx_alpha(accent, 0.22));
            cairo_fill_preserve(cr);
            nx_set(cr, accent);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
        } else if (sel[i] || hovered(K_ITEM, i) || i == focus) {
            nx_rounded_rect(cr, x + 4, y + 2, CELL_W - 8, CELL_H - 6, 8);
            nx_set(cr, sel[i] ? nx_alpha(accent, 0.16) : nx_rgb("#f1f4f9"));
            cairo_fill_preserve(cr);
            nx_set(cr, sel[i] ? nx_alpha(accent, 0.55) : i == focus ? nx_alpha(accent, 0.3) : nx_rgb("#e3e7ef"));
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        if (!ents[i].dir && is_image(ents[i].name) && !ents[i].thumb_tried) {
            char p[PATH_MAX];
            sel_path(i, p, sizeof p);
            ents[i].thumb = make_thumb(p, 96);
            ents[i].thumb_tried = true;
        }
        if (ents[i].thumb) {
            /* miniature : tient dans 96 x 62, centrée, avec un liseré */
            double tw = cairo_image_surface_get_width(ents[i].thumb), th = cairo_image_surface_get_height(ents[i].thumb);
            double k = fmin(96 / tw, 62 / th);
            if (k > 1) k = 1;
            double dw = tw * k, dh = th * k, dx = x + (CELL_W - dw) / 2, dy = y + 8 + (62 - dh) / 2;
            cairo_save(cr);
            nx_rounded_rect(cr, dx, dy, dw, dh, 4);
            cairo_clip(cr);
            cairo_translate(cr, dx, dy);
            cairo_scale(cr, k, k);
            cairo_set_source_surface(cr, ents[i].thumb, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            nx_rounded_rect(cr, dx, dy, dw, dh, 4);
            nx_set(cr, nx_rgb("#d5dae3"));
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        } else {
            nx_icon(cr, icon_for(&ents[i]), x + (CELL_W - 58) / 2, y + 10, 58, text);
        }
        nx_text(cr, ents[i].name, 12.5, false, text, x + 6, y + 72, CELL_W - 12, 22, NX_CENTER);
        add_rect(x + 4, y + 2, CELL_W - 8, CELL_H - 6, K_ITEM, i);
    }
    cairo_restore(cr);
    if (max_scroll > 0) {
        double bh = ch * ch / (rows * CELL_H + 24), by = cy + (ch - bh) * scroll_y / max_scroll;
        nx_rounded_rect(cr, width - 7, by, 4, bh, 2);
        nx_set(cr, nx_alpha(subtle, 0.45));
        cairo_fill(cr);
    }

    /* ---- Barre latérale ---- */
    cairo_rectangle(cr, 0, TOOL_H, SIDE_W, height - TOOL_H - STATUS_H);
    nx_set(cr, nx_rgb("#f2f4f8"));
    cairo_fill(cr);
    nx_text(cr, "Emplacements", 11.5, true, subtle, 18, TOOL_H + 10, SIDE_W - 30, 24, NX_LEFT);
    for (int i = 0; i < nplaces; i++) {
        double y = TOOL_H + 38 + i * 40;
        bool cur = !strcmp(cwd, places[i].path);
        char tb[PATH_MAX];
        if (dragging && hovered(K_PLACE, i) && drop_target(K_PLACE, i, tb, sizeof tb)) {
            nx_rounded_rect(cr, 8, y, SIDE_W - 16, 36, 7);
            nx_set(cr, nx_alpha(accent, 0.25));
            cairo_fill_preserve(cr);
            nx_set(cr, accent);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
        } else if (cur || hovered(K_PLACE, i)) {
            nx_rounded_rect(cr, 8, y, SIDE_W - 16, 36, 7);
            nx_set(cr, cur ? nx_alpha(accent, 0.14) : nx_rgb("#e6eaf1"));
            cairo_fill(cr);
        }
        nx_icon(cr, places[i].icon, 20, y + 7, 22, text);
        nx_text(cr, places[i].label, 13.5, cur, text, 52, y, SIDE_W - 62, 36, NX_LEFT);
        add_rect(8, y, SIDE_W - 16, 36, K_PLACE, i);
    }

    /* ---- Barre d'outils ---- */
    cairo_rectangle(cr, 0, 0, width, TOOL_H);
    nx_set(cr, nx_rgb("#f7f8fb"));
    cairo_fill(cr);
    cairo_rectangle(cr, 0, TOOL_H - 1, width, 1);
    nx_set(cr, nx_rgb("#e1e5ec"));
    cairo_fill(cr);
    bool can_back = hist_pos > 0, can_fwd = hist_pos < hist_len - 1, can_up = strcmp(cwd, "/") != 0;
    struct { enum kind k; int dir; bool en; } arrows[] = {
        { K_BACK, 0, can_back }, { K_FWD, 1, can_fwd }, { K_UP, 2, can_up && !in_trash() },
    };
    for (int i = 0; i < 3; i++) {
        double x = 12 + i * 40, y = 10;
        if (hovered(arrows[i].k, 0) && arrows[i].en) {
            nx_rounded_rect(cr, x, y, 36, 36, 8);
            nx_set(cr, nx_rgb("#e6eaf1"));
            cairo_fill(cr);
        }
        draw_arrow(cr, x, y, arrows[i].dir, arrows[i].en);
        add_rect(x, y, 36, 36, arrows[i].k, 0);
    }
    /* Bouton à droite : Nouveau dossier, ou Vider la corbeille */
    const char *btn = in_trash() ? "Vider la corbeille" : "Nouveau dossier";
    double bw = nx_text_width(cr, btn, 13, true) + 30, bx = width - bw - 14;
    draw_button(cr, bx, 11, bw, 34, btn, true, hovered(K_TOOLBTN, 0));
    add_rect(bx, 11, bw, 34, K_TOOLBTN, 0);
    /* Fil d'Ariane */
    build_crumbs();
    double x = 140, maxx = bx - 12;
    int first = 0;
    double total = 0;
    for (int i = 0; i < ncrumbs; i++) total += nx_text_width(cr, crumb_label[i], 13.5, i == ncrumbs - 1) + 34;
    while (first < ncrumbs - 1 && total > maxx - x) {   /* trop long : on masque le début */
        total -= nx_text_width(cr, crumb_label[first], 13.5, false) + 34;
        first++;
    }
    for (int i = first; i < ncrumbs; i++) {
        bool last = i == ncrumbs - 1;
        double tw = nx_text_width(cr, crumb_label[i], 13.5, last) + 16;
        if (x + tw > maxx) tw = maxx - x;
        if (tw < 30) break;
        if (hovered(K_CRUMB, i) && !last) {
            nx_rounded_rect(cr, x, 12, tw, 32, 6);
            nx_set(cr, nx_rgb("#e6eaf1"));
            cairo_fill(cr);
        }
        nx_text(cr, crumb_label[i], 13.5, last, last ? text : nx_rgb("#3a4152"), x + 8, 12, tw - 12, 32, NX_LEFT);
        add_rect(x, 12, tw, 32, K_CRUMB, i);
        x += tw;
        if (!last) {
            nx_text(cr, "›", 15, false, subtle, x, 12, 18, 32, NX_CENTER);
            x += 18;
        }
    }

    /* ---- Barre d'état ---- */
    double sy = height - STATUS_H;
    cairo_rectangle(cr, 0, sy, width, STATUS_H);
    nx_set(cr, nx_rgb("#f7f8fb"));
    cairo_fill(cr);
    cairo_rectangle(cr, 0, sy, width, 1);
    nx_set(cr, nx_rgb("#e1e5ec"));
    cairo_fill(cr);
    char left[300], right[96] = "";
    int ns = selected_count();
    if (status_msg[0] && time(NULL) < status_until) {
        snprintf(left, sizeof left, "%s", status_msg);
    } else if (ns > 0) {
        double total_size = 0;
        for (int i = 0; i < nents; i++) if (sel[i] && !ents[i].dir) total_size += ents[i].size;
        char hs[32];
        human_size(hs, sizeof hs, total_size);
        snprintf(left, sizeof left, "%d élément%s sélectionné%s (%s)", ns, ns > 1 ? "s" : "", ns > 1 ? "s" : "", hs);
    } else {
        snprintf(left, sizeof left, "%d élément%s", nents, nents > 1 ? "s" : "");
    }
    struct statvfs vfs;
    if (statvfs(cwd, &vfs) == 0) {
        char hs[32];
        human_size(hs, sizeof hs, (double)vfs.f_bavail * vfs.f_frsize);
        snprintf(right, sizeof right, "Espace libre : %s", hs);
    }
    nx_text(cr, left, 12.5, false, nx_rgb("#3a4152"), 16, sy, width * 0.65, STATUS_H, NX_LEFT);
    nx_text(cr, right, 12.5, false, subtle, width * 0.6, sy, width * 0.4 - 16, STATUS_H, NX_RIGHT);

    /* ---- Éléments glissés (sous la souris) ---- */
    if (dragging && drag_from >= 0 && drag_from < nents) {
        int ns = selected_count();
        double gx0 = drag_x + 10, gy0 = drag_y + 8;
        nx_rounded_rect(cr, gx0, gy0, 64, 64, 10);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
        cairo_fill_preserve(cr);
        nx_set(cr, nx_alpha(accent, 0.6));
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        nx_icon(cr, icon_for(&ents[drag_from]), gx0 + 10, gy0 + 10, 44, text);
        if (ns > 1) {
            char nb[16];
            snprintf(nb, sizeof nb, "%d", ns);
            cairo_new_path(cr);
            cairo_arc(cr, gx0 + 60, gy0 + 4, 12, 0, 2 * M_PI);
            nx_set(cr, accent);
            cairo_fill(cr);
            nx_text(cr, nb, 11.5, true, nx_rgb("#ffffff"), gx0 + 48, gy0 - 8, 24, 24, NX_CENTER);
        }
        const char *hint = nx_mod_ctrl() ? "Copier" : "Déplacer";
        nx_text(cr, hint, 11.5, true, accent, gx0, gy0 + 66, 64, 18, NX_CENTER);
    }

    /* ---- Menu contextuel ---- */
    if (menu_open) {
        add_rect(0, 0, width, height, K_MENUBG, 0);
        double mw = 230, mh = nmenu * 34 + 10;
        double mx = menu_x + mw > width ? width - mw - 4 : menu_x;
        double my = menu_y + mh > height ? height - mh - 4 : menu_y;
        for (int i = 4; i > 0; i--) {
            nx_rounded_rect(cr, mx - i + 2, my - i + 3, mw + 2 * i, mh + 2 * i, 10 + i);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.04);
            cairo_fill(cr);
        }
        nx_rounded_rect(cr, mx, my, mw, mh, 9);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        nx_set(cr, nx_rgb("#dde1e8"));
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        for (int i = 0; i < nmenu; i++) {
            double iy = my + 5 + i * 34;
            if (hovered(K_MENU, i)) {
                nx_rounded_rect(cr, mx + 5, iy, mw - 10, 34, 6);
                nx_set(cr, accent);
                cairo_fill(cr);
            }
            bool danger = menu[i].act == A_DELETE || menu[i].act == A_EMPTY;
            nx_text(cr, menu[i].label, 13.5, false,
                    hovered(K_MENU, i) ? nx_rgb("#ffffff") : danger ? nx_rgb("#c0392b") : text,
                    mx + 16, iy, mw - 28, 34, NX_LEFT);
            add_rect(mx + 5, iy, mw - 10, 34, K_MENU, i);
        }
    }

    /* ---- Boîte de dialogue ---- */
    if (dialog != DLG_NONE) {
        cairo_set_source_rgba(cr, 0.05, 0.07, 0.1, 0.35);
        cairo_paint(cr);
        add_rect(0, 0, width, height, K_DLG, 0);
        double dw = 440, dh = (dialog == DLG_RENAME || dialog == DLG_NEWDIR) ? 196 : 168;
        double dx = (width - dw) / 2, dy = (height - dh) / 2;
        nx_rounded_rect(cr, dx, dy, dw, dh, 12);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill(cr);
        const char *title = dialog == DLG_RENAME ? "Renommer" : dialog == DLG_NEWDIR ? "Nouveau dossier" :
                            dialog == DLG_EMPTY ? "Vider la corbeille ?" : "Supprimer définitivement ?";
        const char *ok = dialog == DLG_RENAME ? "Renommer" : dialog == DLG_NEWDIR ? "Créer" :
                         dialog == DLG_EMPTY ? "Vider" : "Supprimer";
        nx_text(cr, title, 18, true, text, dx + 24, dy + 18, dw - 48, 30, NX_LEFT);
        if (dialog == DLG_RENAME || dialog == DLG_NEWDIR) {
            nx_text(cr, "Nom :", 13, false, subtle, dx + 24, dy + 56, 0, 22, NX_LEFT);
            nx_rounded_rect(cr, dx + 24, dy + 80, dw - 48, 40, 8);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill_preserve(cr);
            nx_set(cr, accent);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
            double tw = nx_text(cr, input, 14, false, text, dx + 36, dy + 80, dw - 72, 40, NX_LEFT);
            cairo_rectangle(cr, dx + 37 + (tw < dw - 72 ? tw : dw - 72), dy + 90, 1.5, 20);
            nx_set(cr, accent);
            cairo_fill(cr);
        } else {
            nx_text(cr, dialog == DLG_EMPTY ? "Tous les éléments de la corbeille seront effacés pour de bon."
                                            : "Les éléments sélectionnés seront effacés pour de bon.",
                    13.5, false, nx_rgb("#3a4152"), dx + 24, dy + 58, dw - 48, 24, NX_LEFT);
            nx_text(cr, "Cette action est irréversible.", 13.5, true, nx_rgb("#c0392b"),
                    dx + 24, dy + 82, dw - 48, 24, NX_LEFT);
        }
        double by = dy + dh - 54;
        draw_button(cr, dx + dw - 24 - 120, by, 120, 36, ok, true, hovered(K_DLG_OK, 0));
        add_rect(dx + dw - 24 - 120, by, 120, 36, K_DLG_OK, 0);
        draw_button(cr, dx + dw - 24 - 120 - 12 - 110, by, 110, 36, "Annuler", false, hovered(K_DLG_CANCEL, 0));
        add_rect(dx + dw - 24 - 120 - 12 - 110, by, 110, 36, K_DLG_CANCEL, 0);
    }
}

/* ======================= Souris ======================= */

static double last_click_time;
static int last_click_index = -1;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void select_only(int i)
{
    if (sel) memset(sel, 0, ents_cap * sizeof *sel);
    if (i >= 0 && i < nents) sel[i] = true;
    focus = i;
}

static int press_kind = -1, press_index = -1;

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct rect *r = hit(x, y);
    int k = r ? (int)r->kind : -1, i = r ? r->index : -1;
    switch (ev) {
    case NX_MOTION:
        if (drag_from >= 0 && !dragging && !in_trash() && hypot(x - press_x, y - press_y) > 8) {
            dragging = true;
            if (!sel[drag_from]) select_only(drag_from);
        }
        if (dragging) { drag_x = x; drag_y = y; hover_kind = k; hover_index = i; nx_redraw(w); break; }
        if (k != hover_kind || i != hover_index) { hover_kind = k; hover_index = i; nx_redraw(w); }
        break;
    case NX_LEAVE:
        hover_kind = hover_index = -1;
        nx_redraw(w);
        break;
    case NX_SCROLL:
        if (!menu_open && dialog == DLG_NONE) { scroll_y += value * CELL_H / 2; nx_redraw(w); }
        break;
    case NX_PRESS:
        press_kind = k;
        press_index = i;
        if (dialog != DLG_NONE || menu_open) break;   /* traités au relâchement */
        if (k == K_ITEM) {
            if (value == BTN_RIGHT) {
                if (!sel[i]) select_only(i);
                open_menu(x, y, true);
            } else if (value == BTN_LEFT) {
                double t = now_s();
                drag_from = i;
                press_x = x;
                press_y = y;
                if (getenv("NX_DEBUG"))
                    fprintf(stderr, "clic : élément %d, précédent %d, écart %.3f s\n",
                            i, last_click_index, t - last_click_time);
                if (nx_mod_ctrl()) { sel[i] = !sel[i]; focus = i; }
                else if (i == last_click_index && t - last_click_time < 0.45) {
                    drag_from = -1;
                    open_entry(i);
                    last_click_index = -1;
                    break;
                }
                else select_only(i);
                last_click_time = t;
                last_click_index = i;
            }
            nx_redraw(w);
        } else if (k == K_CONTENT) {
            select_only(-1);
            if (value == BTN_RIGHT) open_menu(x, y, false);
            nx_redraw(w);
        }
        break;
    case NX_RELEASE:
        if (value == BTN_LEFT && dragging) {
            char dest[PATH_MAX];
            if (drop_target(k, i, dest, sizeof dest)) do_drop(dest, nx_mod_ctrl());
            dragging = false;
            drag_from = -1;
            nx_redraw(w);
            break;
        }
        if (value == BTN_LEFT) drag_from = -1;
        if (value != BTN_LEFT || k != press_kind || i != press_index) break;
        if (dialog != DLG_NONE) {
            if (k == K_DLG_OK) confirm_dialog();
            else if (k == K_DLG_CANCEL) { dialog = DLG_NONE; nx_redraw(w); }
            break;
        }
        if (menu_open) {
            if (k == K_MENU) run_action(menu[i].act);
            else { menu_open = false; nx_redraw(w); }
            break;
        }
        switch (k) {
        case K_BACK: if (hist_pos > 0) { hist_pos--; navigate(history[hist_pos], false); } break;
        case K_FWD: if (hist_pos < hist_len - 1) { hist_pos++; navigate(history[hist_pos], false); } break;
        case K_UP: if (strcmp(cwd, "/") && !in_trash()) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/..", cwd); navigate(p, true); } break;
        case K_CRUMB: navigate(crumb_path[i], true); break;
        case K_PLACE: navigate(places[i].path, true); break;
        case K_TOOLBTN: start_dialog(in_trash() ? DLG_EMPTY : DLG_NEWDIR); break;
        default: break;
        }
        break;
    }
}

/* ======================= Clavier ======================= */

static void move_focus(int delta)
{
    if (nents == 0) return;
    int f = focus < 0 ? 0 : focus + delta;
    if (f < 0) f = 0;
    if (f >= nents) f = nents - 1;
    select_only(f);
    /* faire défiler pour garder l'élément visible */
    int cols = grid_cols(win->width);
    int row_y = (f / cols) * CELL_H, ch = win->height - TOOL_H - STATUS_H;
    if (row_y < scroll_y) scroll_y = row_y;
    if (row_y + CELL_H + 12 > scroll_y + ch) scroll_y = row_y + CELL_H + 12 - ch;
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (dialog != DLG_NONE) {
        bool editing = dialog == DLG_RENAME || dialog == DLG_NEWDIR;
        if (sym == XKB_KEY_Escape) dialog = DLG_NONE;
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) { confirm_dialog(); return; }
        else if (editing && sym == XKB_KEY_BackSpace) {
            size_t n = strlen(input);
            while (n > 0 && ((unsigned char)input[n - 1] & 0xC0) == 0x80) n--;
            if (n > 0) n--;
            input[n] = 0;
        } else if (editing && utf8[0] && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f &&
                   strlen(input) + strlen(utf8) < sizeof input - 1) {
            strcat(input, utf8);
        } else return;
        nx_redraw(w);
        return;
    }
    if (menu_open) {
        if (sym == XKB_KEY_Escape) { menu_open = false; nx_redraw(w); }
        return;
    }
    int cols = grid_cols(w->width);
    bool ctrl = nx_mod_ctrl();
    if (ctrl) {
        switch (sym) {
        case XKB_KEY_c: case XKB_KEY_C: if (selected_count() && !in_trash()) clip_set(false); return;
        case XKB_KEY_x: case XKB_KEY_X: if (selected_count() && !in_trash()) clip_set(true); return;
        case XKB_KEY_v: case XKB_KEY_V: if (nclip && !in_trash()) do_paste(); return;
        case XKB_KEY_a: case XKB_KEY_A: for (int i = 0; i < nents; i++) sel[i] = true; nx_redraw(w); return;
        case XKB_KEY_h: case XKB_KEY_H: show_hidden = !show_hidden; refresh(); return;
        case XKB_KEY_n: case XKB_KEY_N: if (!in_trash()) start_dialog(DLG_NEWDIR); return;
        default: return;
        }
    }
    switch (sym) {
    case XKB_KEY_Right: move_focus(1); break;
    case XKB_KEY_Left: move_focus(-1); break;
    case XKB_KEY_Down: move_focus(cols); break;
    case XKB_KEY_Up: move_focus(-cols); break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: open_entry(focus >= 0 ? focus : first_selected()); return;
    case XKB_KEY_BackSpace:
        if (strcmp(cwd, "/") && !in_trash()) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/..", cwd); navigate(p, true); }
        return;
    case XKB_KEY_Delete:
        if (selected_count()) { if (in_trash()) start_dialog(DLG_DELETE); else do_trash(); }
        return;
    case XKB_KEY_F2: if (selected_count() == 1 && !in_trash()) start_dialog(DLG_RENAME); return;
    case XKB_KEY_F5: refresh(); return;
    case XKB_KEY_Escape: select_only(-1); break;
    default: return;
    }
    nx_redraw(w);
}

static int status_timeout(void)
{
    /* rafraîchir la barre d'état quand un message expire */
    if (!status_msg[0]) return -1;
    time_t now = time(NULL);
    if (now >= status_until) { status_msg[0] = 0; nx_redraw(win); return -1; }
    return (int)(status_until - now) * 1000 + 50;
}
static void status_tick(void) { nx_redraw(win); }
static void reload(void) { nx_redraw(win); }

static void init_paths(void)
{
    const char *h = getenv("HOME");
    snprintf(home, sizeof home, "%s", h && *h ? h : "/");
    char base[PATH_MAX], tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/.local", home); mkdir(tmp, 0755);
    snprintf(tmp, sizeof tmp, "%s/.local/share", home); mkdir(tmp, 0755);
    snprintf(base, sizeof base, "%s/.local/share/Trash", home); mkdir(base, 0700);
    snprintf(trash_files, sizeof trash_files, "%s/files", base); mkdir(trash_files, 0700);
    snprintf(trash_info, sizeof trash_info, "%s/info", base); mkdir(trash_info, 0700);
    /* Dossiers habituels, créés une fois s'ils n'existent pas */
    static const char *std[] = { "Documents", "Images", "Téléchargements" };
    for (unsigned i = 0; i < 3; i++) {
        snprintf(tmp, sizeof tmp, "%s/%s", home, std[i]);
        mkdir(tmp, 0755);
    }
    char p[PATH_MAX];
    add_place("Dossier personnel", "home", home);
    snprintf(p, sizeof p, "%s/Documents", home); add_place("Documents", "editor", p);
    snprintf(p, sizeof p, "%s/Images", home); add_place("Images", "image", p);
    snprintf(p, sizeof p, "%s/Téléchargements", home); add_place("Téléchargements", "downloads", p);
    add_place("Disque de données", "disk", "/data");
    add_place("Système", "display", "/");
    add_place("Corbeille", "trash", trash_files);
}

/* ======================= Visionneuse d'images ======================= */

static char **imgs;
static int nimgs, cur_img;
static cairo_surface_t *img_surface;
static char view_dir[PATH_MAX];
static bool viewer_hover[3];
static char view_msg[128];

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void viewer_load(void)
{
    if (img_surface) { cairo_surface_destroy(img_surface); img_surface = NULL; }
    if (nimgs == 0) return;
    char p[PATH_MAX];
    join(p, sizeof p, view_dir, imgs[cur_img]);
    img_surface = nx_load_image(p, 0);
    if (win && win->toplevel) {
        char t[300];
        snprintf(t, sizeof t, "%s — Visionneuse", imgs[cur_img]);
        xdg_toplevel_set_title(win->toplevel, t);
    }
    view_msg[0] = 0;
}

static void viewer_draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    nrects = 0;
    nx_color white = nx_rgb("#ffffff"), accent = nx_accent_color();
    cairo_set_source_rgb(cr, 0.07, 0.08, 0.1);
    cairo_paint(cr);
    double bar = 60, aw = width, ah = height - bar;
    if (img_surface) {
        double iw = cairo_image_surface_get_width(img_surface), ih = cairo_image_surface_get_height(img_surface);
        double k = fmin((aw - 40) / iw, (ah - 40) / ih);
        if (k > 1) k = 1;   /* pas d'agrandissement des petites images */
        cairo_save(cr);
        cairo_translate(cr, (aw - iw * k) / 2, (ah - ih * k) / 2);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, img_surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        nx_text(cr, "Impossible d'afficher cette image (formats pris en charge : PNG et JPEG).",
                14, false, nx_alpha(white, 0.8), 0, ah / 2 - 15, width, 30, NX_CENTER);
    }
    cairo_rectangle(cr, 0, ah, width, bar);
    cairo_set_source_rgba(cr, 0.12, 0.13, 0.17, 1);
    cairo_fill(cr);
    char label[300];
    snprintf(label, sizeof label, "%s   (%d / %d)", nimgs ? imgs[cur_img] : "", nimgs ? cur_img + 1 : 0, nimgs);
    nx_text(cr, view_msg[0] ? view_msg : label, 13, false, nx_alpha(white, 0.85), 18, ah, width - 420, bar, NX_LEFT);
    /* Boutons : précédente, suivante, fond d'écran */
    double y = ah + 12, h = 36;
    double fx = width - 18 - 230;
    for (int i = 0; i < 2; i++) {
        double x = fx - 12 - (2 - i) * 44;
        nx_rounded_rect(cr, x, y, 38, h, 8);
        cairo_set_source_rgba(cr, 1, 1, 1, viewer_hover[i] ? 0.2 : 0.1);
        cairo_fill(cr);
        cairo_save(cr);
        cairo_translate(cr, x + 19, y + 18);
        if (i == 0) cairo_rotate(cr, M_PI);
        cairo_move_to(cr, -4, -7); cairo_line_to(cr, 4, 0); cairo_line_to(cr, -4, 7);
        cairo_set_line_width(cr, 2.4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_stroke(cr);
        cairo_restore(cr);
        add_rect(x, y, 38, h, K_TOOLBTN, i);
    }
    nx_rounded_rect(cr, fx, y, 230, h, 8);
    nx_set(cr, viewer_hover[2] ? nx_mix(accent, white, 0.15) : accent);
    cairo_fill(cr);
    nx_text(cr, "Définir comme fond d'écran", 13, true, white, fx, y, 230, h, NX_CENTER);
    add_rect(fx, y, 230, h, K_TOOLBTN, 2);
}

static void viewer_go(int d)
{
    if (nimgs < 2) return;
    cur_img = (cur_img + d + nimgs) % nimgs;
    viewer_load();
    nx_redraw(win);
}

static void viewer_set_wallpaper(void)
{
    if (!nimgs) return;
    join(nx_conf.wallpaper, sizeof nx_conf.wallpaper, view_dir, imgs[cur_img]);
    nx_config_save(&nx_conf);
    nx_notify_reload();
    snprintf(view_msg, sizeof view_msg, "Fond d'écran modifié.");
    nx_redraw(win);
}

static void viewer_pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    struct rect *r = hit(x, y);
    if (ev == NX_MOTION || ev == NX_LEAVE) {
        bool h[3] = { false, false, false };
        if (r && ev == NX_MOTION) h[r->index] = true;
        if (memcmp(h, viewer_hover, sizeof h)) { memcpy(viewer_hover, h, sizeof h); nx_redraw(w); }
    } else if (ev == NX_RELEASE && value == BTN_LEFT && r) {
        if (r->index == 0) viewer_go(-1);
        else if (r->index == 1) viewer_go(1);
        else viewer_set_wallpaper();
    } else if (ev == NX_SCROLL) {
        viewer_go(value);
    }
}

static void viewer_key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    (void)w; (void)utf8;
    if (sym == XKB_KEY_Escape) nx_quit();
    else if (sym == XKB_KEY_Left || sym == XKB_KEY_BackSpace) viewer_go(-1);
    else if (sym == XKB_KEY_Right || sym == XKB_KEY_space) viewer_go(1);
}

static int viewer_main(const char *path)
{
    char real[PATH_MAX];
    if (!realpath(path, real)) { fprintf(stderr, "Image introuvable : %s\n", path); return 1; }
    snprintf(view_dir, sizeof view_dir, "%s", real);
    char *slash = strrchr(view_dir, '/');
    const char *name = slash + 1;
    char wanted[256];
    snprintf(wanted, sizeof wanted, "%s", name);
    if (slash == view_dir) view_dir[1] = 0; else *slash = 0;
    /* Toutes les images (PNG, JPEG) du dossier, pour passer de l'une à l'autre */
    DIR *d = opendir(view_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)))
            if (de->d_name[0] != '.' && is_image(de->d_name)) {
                imgs = realloc(imgs, (nimgs + 1) * sizeof *imgs);
                imgs[nimgs++] = strdup(de->d_name);
            }
        closedir(d);
    }
    qsort(imgs, nimgs, sizeof *imgs, cmp_str);
    for (int i = 0; i < nimgs; i++) if (!strcmp(imgs[i], wanted)) cur_img = i;
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = viewer_draw, .pointer = viewer_pointer, .key = viewer_key };
    win = nx_toplevel_window(h, "Visionneuse", "nexus-viewer", 900, 640);
    if (!win) return 1;
    viewer_load();
    nx_run();
    return 0;
}

/* ======================= Point d'entrée ======================= */

int files_main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--view")) return viewer_main(argv[2]);
    init_paths();
    if (!nx_init()) return 1;
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_toplevel_window(h, "Fichiers", "nexus-files", WIN_W, WIN_H);
    if (!win) return 1;
    navigate(argc > 1 ? argv[1] : home, true);
    nx.timeout = status_timeout;
    nx.tick = status_tick;
    nx.reload = reload;
    nx_run();
    return 0;
}
