/* Réseau de Nexus OS : état (Ethernet, Wi-Fi) pour la barre des tâches, et le
 * panneau "Réseau" (nexus-network) pour choisir un réseau Wi-Fi.
 *
 * Le Wi-Fi passe par wpa_supplicant (service "wifi") : ce panneau lui parle
 * via son interface de contrôle (/run/wpa_supplicant/<carte>), comme wpa_cli.
 * Accès réservé au groupe 'netdev' (ajouté automatiquement à la connexion).
 */
#define _GNU_SOURCE
#include "nexus-ui.h"
#include "network.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

/* ======================= État des interfaces ======================= */

static bool read_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok;
}

static bool iface_up(const char *ifn)
{
    char p[128], s[32];
    snprintf(p, sizeof p, "/sys/class/net/%s/operstate", ifn);
    return read_line(p, s, sizeof s) && !strcmp(s, "up");
}

static bool iface_ip(const char *ifn, char *out, size_t n)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct ifreq ifr = { 0 };
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifn);
    bool ok = ioctl(fd, SIOCGIFADDR, &ifr) == 0;
    close(fd);
    if (ok) inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, out, n);
    return ok;
}

/* Interfaces Wi-Fi, sauf celles réservées aux tests (point d'accès simulé) */
static bool excluded(const char *ifn)
{
    char ex[256] = "";
    read_line("/run/nexus-wifi-exclude", ex, sizeof ex);
    return ex[0] && strstr(ex, ifn) != NULL;
}

static bool wifi_iface(char *out, size_t n)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) && !found) {
        if (e->d_name[0] == '.') continue;
        char p[300];
        snprintf(p, sizeof p, "/sys/class/net/%s/wireless", e->d_name);
        if (access(p, F_OK) == 0 && !excluded(e->d_name)) { snprintf(out, n, "%s", e->d_name); found = true; }
    }
    closedir(d);
    return found;
}

const char *nx_network_icon(bool *connected)
{
    char w[32];
    if (wifi_iface(w, sizeof w) && iface_up(w)) { *connected = true; return "network"; }
    if (iface_up("eth0")) { *connected = true; return "ethernet"; }
    *connected = false;
    return "network";
}

/* ======================= Client wpa_supplicant ======================= */

static int wpa_fd = -1;
static char wpa_local[108];
static char ifname[32];

static void wpa_close(void)
{
    if (wpa_fd >= 0) close(wpa_fd);
    wpa_fd = -1;
    if (wpa_local[0]) unlink(wpa_local);
}

static bool wpa_open(void)
{
    if (wpa_fd >= 0) return true;
    if (!wifi_iface(ifname, sizeof ifname)) return false;
    wpa_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (wpa_fd < 0) return false;
    struct sockaddr_un loc = { .sun_family = AF_UNIX }, dst = { .sun_family = AF_UNIX };
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(wpa_local, sizeof wpa_local, "%s/nexus-wpa-%d", rt ? rt : "/tmp", (int)getpid());
    unlink(wpa_local);
    snprintf(loc.sun_path, sizeof loc.sun_path, "%s", wpa_local);
    snprintf(dst.sun_path, sizeof dst.sun_path, "/run/wpa_supplicant/%s", ifname);
    if (bind(wpa_fd, (struct sockaddr *)&loc, sizeof loc) < 0 ||
        connect(wpa_fd, (struct sockaddr *)&dst, sizeof dst) < 0) {
        wpa_close();
        return false;
    }
    return true;
}

static bool wpa_cmd(const char *cmd, char *reply, size_t n)
{
    if (!wpa_open()) return false;
    if (send(wpa_fd, cmd, strlen(cmd), 0) < 0) { wpa_close(); return false; }
    struct pollfd p = { .fd = wpa_fd, .events = POLLIN };
    for (;;) {
        if (poll(&p, 1, 3000) <= 0) return false;
        ssize_t r = recv(wpa_fd, reply, n - 1, 0);
        if (r < 0) return false;
        reply[r] = 0;
        if (reply[0] != '<') return true;   /* ignorer les messages d'événements "<3>..." */
    }
}

/* ======================= Panneau "Réseau" ======================= */

struct net { char ssid[64]; int signal; bool secured; };
static struct net nets[24];
static int nnets;
static char cur_ssid[64], cur_state[32], cur_ip[32];
static int selected = -1, hover = -1;
static char password[80];
static char status[160];
static time_t connect_started;
static bool scanning;
static struct nx_window *win;

#define BOX_W 380
#define ROW_H 46

static void parse_status(void)
{
    char r[2048];
    cur_ssid[0] = cur_state[0] = cur_ip[0] = 0;
    if (!wpa_cmd("STATUS", r, sizeof r)) return;
    for (char *l = strtok(r, "\n"); l; l = strtok(NULL, "\n")) {
        if (!strncmp(l, "ssid=", 5)) snprintf(cur_ssid, sizeof cur_ssid, "%s", l + 5);
        else if (!strncmp(l, "wpa_state=", 10)) snprintf(cur_state, sizeof cur_state, "%s", l + 10);
    }
    if (ifname[0]) iface_ip(ifname, cur_ip, sizeof cur_ip);
}

/* Wi-Fi connecté ? (nom du réseau dans ssid) — pour les notifications */
bool nx_wifi_connected(char *ssid, size_t n)
{
    char w[32];
    if (!wifi_iface(w, sizeof w)) { wpa_close(); return false; }
    parse_status();
    if (strcmp(cur_state, "COMPLETED") || !cur_ssid[0]) return false;
    snprintf(ssid, n, "%s", cur_ssid);
    return true;
}

static int cmp_net(const void *a, const void *b) { return ((const struct net *)b)->signal - ((const struct net *)a)->signal; }

static void parse_scan(void)
{
    char r[8192];
    if (!wpa_cmd("SCAN_RESULTS", r, sizeof r)) return;
    nnets = 0;
    char *save;
    for (char *l = strtok_r(r, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(l, "bssid", 5)) continue;   /* en-tête */
        char *f[5] = { 0 };
        int i = 0;
        for (char *p = l; i < 5; i++) { f[i] = p; char *t = strchr(p, '\t'); if (!t) break; *t = 0; p = t + 1; }
        if (i < 4 || !f[4] || !f[4][0]) continue;
        int sig = atoi(f[2]);
        bool dup = false;
        for (int k = 0; k < nnets; k++)
            if (!strcmp(nets[k].ssid, f[4])) { dup = true; if (sig > nets[k].signal) nets[k].signal = sig; }
        if (dup || nnets >= 24) continue;
        snprintf(nets[nnets].ssid, sizeof nets[nnets].ssid, "%s", f[4]);
        nets[nnets].signal = sig;
        nets[nnets].secured = strstr(f[3], "WPA") || strstr(f[3], "WEP") || strstr(f[3], "RSN");
        nnets++;
    }
    qsort(nets, nnets, sizeof nets[0], cmp_net);
}

static void hex(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 3 < n; s++) o += snprintf(out + o, n - o, "%02x", (unsigned char)*s);
}

static void do_connect(int i)
{
    char r[256], cmd[400], id[16] = "", hx[140];
    /* réutiliser une configuration existante pour ce réseau, sinon en créer une */
    char list[4096];
    if (wpa_cmd("LIST_NETWORKS", list, sizeof list)) {
        for (char *l = strtok(list, "\n"); l; l = strtok(NULL, "\n")) {
            char *t = strchr(l, '\t');
            if (!t) continue;
            *t = 0;
            char *ssid = t + 1, *t2 = strchr(ssid, '\t');
            if (t2) *t2 = 0;
            if (!strcmp(ssid, nets[i].ssid)) snprintf(id, sizeof id, "%s", l);
        }
    }
    if (!id[0]) {
        if (!wpa_cmd("ADD_NETWORK", r, sizeof r)) { snprintf(status, sizeof status, "Le service Wi-Fi ne répond pas."); return; }
        snprintf(id, sizeof id, "%d", atoi(r));
    }
    hex(hx, sizeof hx, nets[i].ssid);
    snprintf(cmd, sizeof cmd, "SET_NETWORK %s ssid %s", id, hx);
    wpa_cmd(cmd, r, sizeof r);
    if (nets[i].secured) {
        snprintf(cmd, sizeof cmd, "SET_NETWORK %s psk \"%s\"", id, password);
        wpa_cmd(cmd, r, sizeof r);
        if (strncmp(r, "OK", 2)) { snprintf(status, sizeof status, "Mot de passe invalide (8 à 63 caractères)."); return; }
    } else {
        snprintf(cmd, sizeof cmd, "SET_NETWORK %s key_mgmt NONE", id);
        wpa_cmd(cmd, r, sizeof r);
    }
    snprintf(cmd, sizeof cmd, "SELECT_NETWORK %s", id);
    wpa_cmd(cmd, r, sizeof r);
    wpa_cmd("SAVE_CONFIG", r, sizeof r);
    memset(password, 0, sizeof password);
    snprintf(status, sizeof status, "Connexion à « %s »…", nets[i].ssid);
    connect_started = time(NULL);
    selected = -1;
}

/* ---------- Dessin ---------- */

static double box_h(void) { return 150 + (nnets ? nnets : 1) * ROW_H + (selected >= 0 ? 56 : 0) + 40; }

static void box(int width, int height, double *x, double *y, double *h)
{
    *h = fmin(box_h(), height - 20);
    *x = width - BOX_W - 10;
    *y = height - *h - 10;
}

static void bars(cairo_t *cr, double x, double y, int signal, bool white)
{
    int lvl = signal >= -55 ? 4 : signal >= -67 ? 3 : signal >= -78 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        double h = 5 + i * 4;
        nx_rounded_rect(cr, x + i * 6, y + 18 - h, 4, h, 1.5);
        nx_set(cr, nx_fg(i < lvl ? (white ? 0.95 : 0.85) : 0.2));
        cairo_fill(cr);
    }
}

enum { Z_OUT = -2, Z_NONE = -1, Z_CONNECT = 100, Z_FIELD = 101 };

static int zones_n;
static struct { double x, y, w, h; int id; } zones[40];
static void zone_add(double x, double y, double w, double h, int id)
{
    if (zones_n < 40) { zones[zones_n].x = x; zones[zones_n].y = y; zones[zones_n].w = w; zones[zones_n].h = h; zones[zones_n].id = id; zones_n++; }
}

static void draw(struct nx_window *w, cairo_t *cr, int width, int height)
{
    (void)w;
    zones_n = 0;
    double bx, by, bh;
    box(width, height, &bx, &by, &bh);
    nx_glass(cr, bx, by, BOX_W, bh, 16);
    nx_color text = nx_rgb(NX_GLASS_TEXT), subtle = nx_rgb(NX_GLASS_SUBTLE), accent = nx_accent_color();
    nx_color white = nx_fg(1);   /* premier plan (suit le thème) */
    nx_text(cr, "Réseau", 16, true, text, bx + 20, by + 16, 0, 24, NX_LEFT);

    /* Ethernet */
    double y = by + 52;
    char ip[32] = "";
    bool eth = iface_up("eth0");
    if (eth) iface_ip("eth0", ip, sizeof ip);
    nx_rounded_rect(cr, bx + 14, y, BOX_W - 28, 50, 10);
    nx_set(cr, nx_fg(0.06));
    cairo_fill(cr);
    nx_icon(cr, "ethernet", bx + 26, y + 13, 24, nx_alpha(white, eth ? 0.95 : 0.4));
    nx_text(cr, "Ethernet", 14, true, text, bx + 62, y + 5, 200, 22, NX_LEFT);
    char st[64];
    snprintf(st, sizeof st, eth ? "Connecté · %s" : "Câble débranché", ip);
    nx_text(cr, st, 12, false, eth ? nx_rgb("#5fd38a") : subtle, bx + 62, y + 26, BOX_W - 90, 20, NX_LEFT);

    /* Wi-Fi */
    y += 66;
    nx_text(cr, "Wi-Fi", 14, true, text, bx + 20, y, 0, 22, NX_LEFT);
    if (scanning) nx_text(cr, "Recherche…", 12, false, subtle, bx + 20, y, BOX_W - 40, 22, NX_RIGHT);
    y += 30;
    if (!wifi_iface(ifname, sizeof ifname)) {
        bool pkg = access("/usr/local/bin/wpa_supplicant", X_OK) == 0;
        nx_text(cr, pkg ? "Aucune carte Wi-Fi détectée." : "Wi-Fi non installé : npk install wifi",
                13, false, subtle, bx + 20, y, BOX_W - 40, 24, NX_LEFT);
    } else if (wpa_fd < 0 && !wpa_open()) {
        nx_text(cr, "Service Wi-Fi arrêté (service wifi start).", 13, false, subtle, bx + 20, y, BOX_W - 40, 24, NX_LEFT);
    } else if (nnets == 0) {
        nx_text(cr, scanning ? "Recherche des réseaux…" : "Aucun réseau trouvé.", 13, false, subtle, bx + 20, y, BOX_W - 40, 24, NX_LEFT);
    }
    for (int i = 0; i < nnets && y + ROW_H < by + bh - 40; i++) {
        bool cur = !strcmp(nets[i].ssid, cur_ssid) && !strcmp(cur_state, "COMPLETED");
        if (hover == i || selected == i) {
            nx_rounded_rect(cr, bx + 12, y, BOX_W - 24, ROW_H - 4 + (selected == i ? 56 : 0), 10);
            nx_set(cr, nx_fg(0.08));
            cairo_fill(cr);
        }
        bars(cr, bx + 24, y + 11, nets[i].signal, cur);
        nx_text(cr, nets[i].ssid, 14, cur, text, bx + 62, y, BOX_W - 160, ROW_H - 4, NX_LEFT);
        if (cur) nx_text(cr, "Connecté", 12, false, nx_rgb("#5fd38a"), bx + 20, y, BOX_W - 64, ROW_H - 4, NX_RIGHT);
        if (nets[i].secured) nx_icon(cr, "lock", bx + BOX_W - 40, y + 13, 16, nx_alpha(white, 0.6));
        zone_add(bx + 12, y, BOX_W - 24, ROW_H - 4, i);
        y += ROW_H;
        if (selected == i) {   /* champ mot de passe + bouton */
            double fx = bx + 24, fw = BOX_W - 160;
            nx_rounded_rect(cr, fx, y, fw, 40, 9);
            nx_set(cr, nx_fg(0.08));
            cairo_fill_preserve(cr);
            nx_set(cr, accent);
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
            size_t n = strlen(password);
            if (!nets[i].secured) nx_text(cr, "Réseau ouvert", 13, false, subtle, fx + 12, y, fw - 20, 40, NX_LEFT);
            else if (!n) nx_text(cr, "Mot de passe", 13, false, subtle, fx + 12, y, fw - 20, 40, NX_LEFT);
            for (size_t k = 0; k < n && k < 18; k++) {
                cairo_new_path(cr);
                cairo_arc(cr, fx + 16 + k * 12, y + 20, 3.5, 0, 2 * M_PI);
                cairo_set_source_rgb(cr, 1, 1, 1);
                cairo_fill(cr);
            }
            zone_add(fx, y, fw, 40, Z_FIELD);
            double cbx = fx + fw + 10, cbw = BOX_W - 24 - fw - 22;
            nx_rounded_rect(cr, cbx, y, cbw, 40, 9);
            nx_set(cr, hover == Z_CONNECT ? nx_mix(accent, white, 0.15) : accent);
            cairo_fill(cr);
            nx_text(cr, "Connecter", 13, true, white, cbx, y, cbw, 40, NX_CENTER);
            zone_add(cbx, y, cbw, 40, Z_CONNECT);
            y += 56;
        }
    }
    /* Ligne d'état */
    const char *msg = status;
    char buf[160];
    if (!strcmp(cur_state, "COMPLETED") && cur_ssid[0]) {
        snprintf(buf, sizeof buf, "Connecté à « %s »%s%s", cur_ssid, cur_ip[0] ? " · " : "", cur_ip);
        if (connect_started) { status[0] = 0; connect_started = 0; }
        msg = status[0] ? status : buf;
    }
    nx_text(cr, msg, 12, false, subtle, bx + 20, by + bh - 34, BOX_W - 40, 22, NX_LEFT);
}

/* ---------- Événements ---------- */

static int zone_at(double x, double y)
{
    double bx, by, bh;
    box(win->width, win->height, &bx, &by, &bh);
    if (x < bx || y < by || x > bx + BOX_W || y > by + bh) return Z_OUT;
    for (int i = zones_n - 1; i >= 0; i--)
        if (x >= zones[i].x && x < zones[i].x + zones[i].w && y >= zones[i].y && y < zones[i].y + zones[i].h)
            return zones[i].id;
    return Z_NONE;
}

static void pointer(struct nx_window *w, enum nx_pointer_event ev, double x, double y, int value)
{
    int z = zone_at(x, y);
    if (ev == NX_MOTION && z != hover) { hover = z; nx_redraw(w); }
    else if (ev == NX_LEAVE) { hover = Z_NONE; nx_redraw(w); }
    else if (ev == NX_RELEASE && value == BTN_LEFT) {
        if (z == Z_OUT) { nx_quit(); return; }
        if (z == Z_CONNECT && selected >= 0) do_connect(selected);
        else if (z >= 0 && z < nnets) {
            bool cur = !strcmp(nets[z].ssid, cur_ssid) && !strcmp(cur_state, "COMPLETED");
            if (cur) selected = -1;
            else if (!nets[z].secured) { selected = z; do_connect(z); }
            else { selected = z; memset(password, 0, sizeof password); status[0] = 0; }
        }
        nx_redraw(w);
    }
}

static void key(struct nx_window *w, xkb_keysym_t sym, const char *utf8)
{
    if (sym == XKB_KEY_Escape) { if (selected >= 0) selected = -1; else { nx_quit(); return; } }
    else if (selected < 0) return;
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) do_connect(selected);
    else if (sym == XKB_KEY_BackSpace) {
        size_t n = strlen(password);
        while (n > 0 && ((unsigned char)password[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        password[n] = 0;
    } else if (utf8[0] && (unsigned char)utf8[0] >= 0x20 && utf8[0] != '"' &&
               strlen(password) + strlen(utf8) < sizeof password - 1) {
        strcat(password, utf8);
    } else return;
    nx_redraw(w);
}

static int ticks;
static int timeout(void) { return 1000; }
static void tick(void)
{
    ticks++;
    if (wpa_fd >= 0 || wpa_open()) {
        if (scanning && ticks >= 3) { parse_scan(); scanning = false; }
        if (ticks % 10 == 0) { char r[64]; wpa_cmd("SCAN", r, sizeof r); scanning = true; ticks = 0; }
        parse_status();
        if (connect_started && time(NULL) - connect_started > 20 && strcmp(cur_state, "COMPLETED")) {
            snprintf(status, sizeof status, "Échec de la connexion : vérifie le mot de passe.");
            connect_started = 0;
        }
    }
    nx_redraw(win);
}

int network_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (nx_toggle_instance("nexus-network")) return 0;
    if (!nx_init()) { nx_instance_done(); return 1; }
    if (wpa_open()) {
        char r[64];
        parse_scan();                 /* résultats déjà connus */
        parse_status();
        if (wpa_cmd("SCAN", r, sizeof r)) scanning = true;
    }
    struct nx_handlers h = { .draw = draw, .pointer = pointer, .key = key };
    win = nx_layer_window(h, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
                          0, 0, 0, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE, "nexus-network");
    if (win) {
        nx.timeout = timeout;
        nx.tick = tick;
        nx_run();
    }
    wpa_close();
    nx_instance_done();
    return 0;
}
