/* nexus-power — permet aux utilisateurs du bureau d'éteindre ou de
 * redémarrer l'ordinateur (bouton "Arrêter" du menu Démarrer).
 *
 * Installé setuid root (/usr/local/libexec/nexus-power) : il n'accepte que
 * que ces actions et seulement pour root ou les membres du groupe 'video'
 * (les comptes autorisés à ouvrir le bureau) :
 *   nexus-power poweroff | reboot
 *   nexus-power keymap <id>     disposition du clavier (Paramètres → Clavier)
 */
#include <ctype.h>
#include <grp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool allowed(void)
{
    if (getuid() == 0) return true;
    struct group *g = getgrnam("video");
    if (!g) return false;
    if (getgid() == g->gr_gid) return true;
    gid_t groups[64];
    int n = getgroups(64, groups);
    for (int i = 0; i < n; i++)
        if (groups[i] == g->gr_gid) return true;
    return false;
}

int main(int argc, char **argv)
{
    const char *cmd = NULL, *km = NULL;
    if (argc == 2 && !strcmp(argv[1], "poweroff")) cmd = "/sbin/poweroff";
    if (argc == 2 && !strcmp(argv[1], "reboot")) cmd = "/sbin/reboot";
    if (argc == 3 && !strcmp(argv[1], "keymap")) {
        /* nom court vérifié (lettres minuscules et « - ») ; nexus-keymap
         * refuse en plus tout ce qui n'est pas dans sa liste */
        km = argv[2];
        size_t n = strlen(km);
        bool ok = n > 0 && n <= 16;
        for (const char *c = km; ok && *c; c++) ok = islower((unsigned char)*c) || *c == '-';
        if (ok) cmd = "/usr/sbin/nexus-keymap";
    }
    if (!cmd) {
        fprintf(stderr, "Usage : nexus-power poweroff|reboot|keymap <id>\n");
        return 2;
    }
    if (!allowed()) {
        fprintf(stderr, "nexus-power : réservé aux membres du groupe 'video'.\n");
        return 1;
    }
    if (setgid(0) != 0 || setuid(0) != 0) {
        perror("nexus-power");
        return 1;
    }
    char *const args[] = { (char *)cmd, km ? "set" : NULL, (char *)km, NULL };
    char *const env[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL };
    execve(cmd, args, env);
    perror(cmd);
    return 1;
}
