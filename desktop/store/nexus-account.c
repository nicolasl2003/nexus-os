/* nexus-account — gestion des comptes pour Paramètres → Comptes (setuid root).
 *
 *   nexus-account add <nom>              crée le compte (groupes du bureau inclus)
 *   nexus-account passwd <nom>           change le mot de passe
 *   nexus-account del <nom> keep|wipe    supprime le compte (wipe : et ses fichiers)
 *   nexus-account verify <nom>           vérifie le mot de passe de l'appelant
 *                                        (écran verrouillé) : <nom> = soi-même
 *
 * Entrée standard :
 *   ligne 1 : mot de passe d'autorisation (absent si l'appelant est root) ;
 *             c'est le mot de passe root, ou pour « passwd <soi-même> » le
 *             mot de passe actuel du compte ;
 *   ligne 2 : nouveau mot de passe (add, passwd).
 *
 * Garde-fous : noms vérifiés, root et le compte connecté ne peuvent pas être
 * supprimés, seuls les comptes ordinaires (uid >= 1000) sont supprimables,
 * attente de 2 s après un mauvais mot de passe.
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <ctype.h>
#include <pwd.h>
#include <shadow.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nexus-secu.h"   /* blocage après plusieurs erreurs + journal */

static bool valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 32 || !(islower((unsigned char)s[0]) || s[0] == '_')) return false;
    for (; *s; s++)
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '_' && *s != '-') return false;
    return true;
}

static void read_line(char *buf, size_t n)
{
    if (!fgets(buf, n, stdin)) buf[0] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
}

static bool hash_ok(const char *user, const char *pw)
{
    struct spwd *sp = getspnam(user);
    if (!sp || !sp->sp_pwdp) return false;
    const char *h = sp->sp_pwdp;
    if (!h[0]) return !strcmp(user, "root");          /* root sans mot de passe (mode live) */
    if (h[0] == '!' || h[0] == '*') return false;
    const char *c = crypt(pw, h);
    return c && !strcmp(c, h);
}

/* Lance un applet BusyBox (en root), avec éventuellement un texte sur stdin */
static int run(char *const argv[], const char *input)
{
    int fd[2];
    if (input && pipe(fd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        if (input) { dup2(fd[0], 0); close(fd[0]); close(fd[1]); }
        execv("/bin/busybox", argv);
        _exit(127);
    }
    if (input) {
        close(fd[0]);
        size_t l = strlen(input);
        if (write(fd[1], input, l) != (ssize_t)l) { /* la commande échouera */ }
        close(fd[1]);
    }
    int st;
    if (pid < 0 || waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int set_password(const char *name, const char *pw)
{
    char line[256];
    snprintf(line, sizeof line, "%s:%s\n", name, pw);
    char *argv[] = { "chpasswd", NULL };
    int r = run(argv, line);
    explicit_bzero(line, sizeof line);
    return r;
}

static int fail(const char *msg, int code)
{
    fprintf(stderr, "%s\n", msg);
    return code;
}

int main(int argc, char **argv)
{
    if (argc < 3) return fail("Usage : nexus-account add|passwd|del <nom> [keep|wipe]", 2);
    const char *act = argv[1], *name = argv[2];
    bool add = !strcmp(act, "add"), pw = !strcmp(act, "passwd"), del = !strcmp(act, "del");
    if (!strcmp(act, "verify")) {
        /* Écran verrouillé : seulement son propre mot de passe, réponse oui/non */
        struct passwd *me = getpwuid(getuid());
        if (!me || strcmp(me->pw_name, name)) return fail("On ne peut vérifier que son propre mot de passe.", 2);
        char pwd[256] = "";
        read_line(pwd, sizeof pwd);
        int remaining;
        if (secu_locked(name, &remaining)) {
            explicit_bzero(pwd, sizeof pwd);
            char m[120];
            secu_locked_msg(m, sizeof m, remaining);
            return fail(m, 6);
        }
        bool ok = hash_ok(name, pwd);
        explicit_bzero(pwd, sizeof pwd);
        if (!ok) { secu_fail(name, "écran verrouillé"); sleep(2); return 3; }
        secu_ok(name, "déverrouillage de l'écran");
        return 0;
    }
    if (!add && !pw && !del) return fail("Action inconnue", 2);
    if (!valid_name(name)) return fail("Nom invalide : minuscules, chiffres, - et _ (commence par une lettre).", 2);
    bool wipe = del && argc > 3 && !strcmp(argv[3], "wipe");

    struct passwd *caller = getpwuid(getuid());
    char caller_name[64] = "";
    if (caller) snprintf(caller_name, sizeof caller_name, "%s", caller->pw_name);

    char auth[256] = "", newpw[256] = "";
    if (getuid() != 0) read_line(auth, sizeof auth);
    if (add || pw) read_line(newpw, sizeof newpw);

    if (getuid() != 0) {
        /* compte dont le mot de passe est demandé : root, ou soi-même (passwd) */
        bool self = pw && !strcmp(name, caller_name);
        int remaining;
        if (secu_locked("root", &remaining) && (!self || secu_locked(name, &remaining))) {
            explicit_bzero(auth, sizeof auth);
            char m[120];
            secu_locked_msg(m, sizeof m, remaining);
            return fail(m, 6);
        }
        bool root_ok = !secu_locked("root", NULL) && hash_ok("root", auth);
        bool ok = root_ok || (self && !secu_locked(name, NULL) && hash_ok(name, auth));
        explicit_bzero(auth, sizeof auth);
        if (!ok) {
            secu_fail(self ? name : "root", "Paramètres → Comptes");
            sleep(2);
            return fail("Mot de passe incorrect", 3);
        }
        secu_ok(root_ok ? "root" : name, "Paramètres → Comptes");
    }
    if (setgid(0) != 0 || setuid(0) != 0) return fail("Droits administrateur indisponibles", 1);
    clearenv();
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);

    struct passwd *target = getpwnam(name);
    if ((add || pw) && (!newpw[0] || strchr(newpw, ':') || strlen(newpw) > 120))
        return fail("Mot de passe vide ou invalide (le caractère « : » est interdit).", 2);

    if (add) {
        if (target) return fail("Ce compte existe déjà.", 4);
        char home[64];
        snprintf(home, sizeof home, "/home/%s", name);
        char *a1[] = { "adduser", "-D", "-s", "/bin/sh", "-h", home, (char *)name, NULL };
        if (run(a1, NULL) != 0) return fail("Échec de la création du compte.", 1);
        char skel[] = "/etc/skel/.", own[80];
        char *a2[] = { "cp", "-a", skel, home, NULL };
        run(a2, NULL);
        snprintf(own, sizeof own, "%s:%s", name, name);
        char *a3[] = { "chown", "-R", own, home, NULL };
        run(a3, NULL);
        chmod(home, 0700);                    /* dossier personnel privé */
        static char *groups[] = { "video", "audio", "netdev" };
        for (int i = 0; i < 3; i++) {
            char *a4[] = { "addgroup", (char *)name, groups[i], NULL };
            run(a4, NULL);
        }
        int r = set_password(name, newpw);
        explicit_bzero(newpw, sizeof newpw);
        if (r != 0) return fail("Compte créé, mais le mot de passe n'a pas pu être enregistré.", 1);
        secu_log("COMPTE créé : %s (par %s)", name, caller_name);
        printf("Compte %s créé.\n", name);
        return 0;
    }
    if (!target) return fail("Ce compte n'existe pas.", 4);
    if (pw) {
        int r = set_password(name, newpw);
        explicit_bzero(newpw, sizeof newpw);
        if (r != 0) return fail("Échec du changement de mot de passe.", 1);
        secu_log("COMPTE : mot de passe de %s changé (par %s)", name, caller_name);
        printf("Mot de passe de %s changé.\n", name);
        return 0;
    }
    /* suppression */
    if (target->pw_uid < 1000) return fail("Ce compte système ne peut pas être supprimé.", 5);
    if (!strcmp(name, caller_name)) return fail("Impossible de supprimer le compte connecté.", 5);
    char home[256];
    snprintf(home, sizeof home, "%s", target->pw_dir ? target->pw_dir : "");
    char *a5[] = { "deluser", (char *)name, NULL };
    if (run(a5, NULL) != 0) return fail("Échec de la suppression du compte.", 1);
    if (wipe && !strncmp(home, "/home/", 6) && strlen(home) > 6 && !strstr(home, "..")) {
        char *a6[] = { "rm", "-rf", home, NULL };
        run(a6, NULL);
    }
    secu_log("COMPTE supprimé : %s%s (par %s)", name, wipe ? " et ses fichiers" : "", caller_name);
    printf("Compte %s supprimé.\n", name);
    return 0;
}
