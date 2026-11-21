/* nexus-power — permet aux utilisateurs du bureau d'éteindre ou de
 * redémarrer l'ordinateur (bouton "Arrêter" du menu Démarrer).
 *
 * Installé setuid root (/usr/local/libexec/nexus-power) : il n'accepte que
 * deux actions et seulement pour root ou les membres du groupe 'video'
 * (les comptes autorisés à ouvrir le bureau). */
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
    const char *cmd = NULL;
    if (argc == 2 && !strcmp(argv[1], "poweroff")) cmd = "/sbin/poweroff";
    if (argc == 2 && !strcmp(argv[1], "reboot")) cmd = "/sbin/reboot";
    if (!cmd) {
        fprintf(stderr, "Usage : nexus-power poweroff|reboot\n");
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
    char *const args[] = { (char *)cmd, NULL };
    char *const env[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL };
    execve(cmd, args, env);
    perror(cmd);
    return 1;
}
