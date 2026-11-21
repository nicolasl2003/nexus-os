/* nexus-screenshot — capture d'écran (touche Impr écran).
 *
 *   nexus-screenshot [--delai N] [fichier.png]
 *
 * Demande l'image de l'écran au compositeur (protocole wlr-screencopy de
 * labwc), l'enregistre en PNG dans ~/Images (« Capture 2026-11-22 10-15-32.png »)
 * puis affiche une notification : un clic ouvre la capture dans la visionneuse.
 */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *mgr;
static struct wl_output *output;

static struct {
    struct wl_buffer *buf;
    void *data;
    size_t size;
    uint32_t format, width, height, stride;
    bool y_invert, done, failed;
} cap;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && !output)
        output = wl_registry_bind(r, name, &wl_output_interface, 1);
    else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, ver < 2 ? ver : 2);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

static void f_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h, uint32_t stride)
{
    (void)d;
    cap.format = format; cap.width = w; cap.height = h; cap.stride = stride;
    cap.size = (size_t)stride * h;
    int fd = memfd_create("nexus-capture", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, cap.size) < 0) { cap.failed = true; return; }
    cap.data = mmap(NULL, cap.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (cap.data == MAP_FAILED) { close(fd); cap.failed = true; return; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, cap.size);
    cap.buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    zwlr_screencopy_frame_v1_copy(f, cap.buf);
}
static void f_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
    (void)d; (void)f;
    cap.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}
static void f_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c)
{
    (void)d; (void)f; (void)a; (void)b; (void)c;
    cap.done = true;
}
static void f_failed(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)d; (void)f; cap.failed = true; }
static void f_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)d; (void)f; (void)x; (void)y; (void)w; (void)h; }
static void f_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h)
{ (void)d; (void)f; (void)fmt; (void)w; (void)h; }
static void f_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)d; (void)f; }
static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = f_buffer, .flags = f_flags, .ready = f_ready, .failed = f_failed,
    .damage = f_damage, .linux_dmabuf = f_dmabuf, .buffer_done = f_buffer_done,
};

static void notify(const char *title, const char *body, const char *action)
{
    char qt[300], qb[700], qa[1400], cmd[2600];
    nx_shell_quote(qt, sizeof qt, title);
    nx_shell_quote(qb, sizeof qb, body);
    if (action) {
        nx_shell_quote(qa, sizeof qa, action);
        snprintf(cmd, sizeof cmd, "nexus-notify -i screenshot -a %s %s %s", qa, qt, qb);
    } else {
        snprintf(cmd, sizeof cmd, "nexus-notify -i screenshot %s %s", qt, qb);
    }
    if (system(cmd) != 0) { /* pas de notification : la capture est quand même faite */ }
}

int screenshot_main(int argc, char **argv)
{
    int delay = 0;
    const char *out = NULL;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "--delai") || !strcmp(argv[i], "--delay")) && i + 1 < argc) delay = atoi(argv[++i]);
        else out = argv[i];
    }
    if (delay > 0 && delay < 60) sleep(delay);

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "nexus-screenshot : pas d'affichage graphique (lancer depuis le bureau).\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!shm || !output || !mgr) {
        fprintf(stderr, "nexus-screenshot : le compositeur ne permet pas les captures (wlr-screencopy).\n");
        return 1;
    }
    struct zwlr_screencopy_frame_v1 *frame = zwlr_screencopy_manager_v1_capture_output(mgr, 0, output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);
    while (!cap.done && !cap.failed && wl_display_dispatch(dpy) != -1) {}
    if (!cap.done) {
        fprintf(stderr, "nexus-screenshot : la capture a échoué.\n");
        notify("Capture d'écran impossible", "Le compositeur a refusé la capture.", NULL);
        return 1;
    }
    if (cap.format != WL_SHM_FORMAT_XRGB8888 && cap.format != WL_SHM_FORMAT_ARGB8888) {
        fprintf(stderr, "nexus-screenshot : format d'image inattendu (%u).\n", cap.format);
        return 1;
    }

    /* Copie dans une image cairo (même disposition mémoire : BGRA), en
     * retournant l'image si le compositeur l'a fournie à l'envers */
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, cap.width, cap.height);
    unsigned char *dst = cairo_image_surface_get_data(img);
    int ds = cairo_image_surface_get_stride(img);
    for (uint32_t y = 0; y < cap.height; y++) {
        uint32_t sy = cap.y_invert ? cap.height - 1 - y : y;
        memcpy(dst + (size_t)y * ds, (unsigned char *)cap.data + (size_t)sy * cap.stride, cap.width * 4);
    }
    cairo_surface_mark_dirty(img);

    char path[1024];
    if (out) {
        snprintf(path, sizeof path, "%s", out);
    } else {
        const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
        char dir[600];
        snprintf(dir, sizeof dir, "%s/Images", home);
        mkdir(dir, 0755);
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char stamp[64];
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &tm);
        snprintf(path, sizeof path, "%s/Capture %s.png", dir, stamp);
    }
    cairo_status_t st = cairo_surface_write_to_png(img, path);
    cairo_surface_destroy(img);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "nexus-screenshot : impossible d'écrire %s\n", path);
        notify("Capture d'écran impossible", "L'image n'a pas pu être enregistrée.", NULL);
        return 1;
    }
    printf("%s\n", path);
    if (!out) {
        nx_recent_add(path);
        const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        char body[400], action[1200], q[1100];
        snprintf(body, sizeof body, "Images/%s — clique pour l'ouvrir.", base);
        nx_shell_quote(q, sizeof q, path);
        snprintf(action, sizeof action, "nexus-files --view %s", q);
        notify("Capture d'écran enregistrée", body, action);
    }
    return 0;
}
