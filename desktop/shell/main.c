/* nexus-shell — un seul programme pour tout le bureau Nexus.
 * Il se comporte selon le nom sous lequel il est appelé (liens symboliques) :
 *   nexus-panel, nexus-menu, nexus-wallpaper, nexus-settings, nexus-files
 * ou : nexus-shell <panel|menu|wallpaper|settings|files> [options]
 * (En statique, un seul binaire évite de copier 4 fois cairo, pango, glib...) */
#include <stdio.h>
#include <string.h>
#include "nexus-ui.h"

int panel_main(int argc, char **argv);
int menu_main(int argc, char **argv);
int wallpaper_main(int argc, char **argv);
int settings_main(int argc, char **argv);
int files_main(int argc, char **argv);
int widgets_main(int argc, char **argv);
int greeter_main(int argc, char **argv);
int volume_main(int argc, char **argv);
int network_main(int argc, char **argv);
int store_main(int argc, char **argv);

static const struct { const char *name; int (*fn)(int, char **); } progs[] = {
    { "panel", panel_main }, { "menu", menu_main },
    { "wallpaper", wallpaper_main }, { "settings", settings_main },
    { "files", files_main }, { "widgets", widgets_main },
    { "greeter", greeter_main }, { "volume", volume_main }, { "network", network_main },
    { "store", store_main },
};

int main(int argc, char **argv)
{
    const char *n = strrchr(argv[0], '/');
    n = n ? n + 1 : argv[0];
    if (!strncmp(n, "nexus-", 6)) n += 6;
    if (!strcmp(n, "shell") && argc > 1) { n = argv[1]; argv++; argc--; }
    if (!strcmp(n, "bench")) return nx_bench();
    if (!strcmp(n, "render")) return nx_render(argc, argv);
    for (unsigned i = 0; i < sizeof progs / sizeof progs[0]; i++)
        if (!strcmp(n, progs[i].name)) return progs[i].fn(argc, argv);
    fprintf(stderr, "Usage : nexus-shell <panel|menu|wallpaper|settings|files> [options]\n");
    return 2;
}
