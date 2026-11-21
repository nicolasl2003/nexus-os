/* nexus-pkg — installe, supprime, met à jour les paquets pour le Magasin
 * d'applications (programme setuid root, lancé par nexus-store).
 *
 *   nexus-pkg check                      vérifie le mot de passe seulement
 *   nexus-pkg update                     npk update
 *   nexus-pkg upgrade                    npk upgrade
 *   nexus-pkg install <paquet>...        npk install ...
 *   nexus-pkg remove <paquet>...         npk remove ...
 *   nexus-pkg alpine-update|alpine-upgrade|alpine-install|alpine-remove ...
 *                                        mêmes actions sur les paquets Alpine
 *
 * Sécurité :
 *   - un utilisateur autre que root doit fournir le mot de passe
 *     administrateur (root) sur l'entrée standard, comme pour « su » ;
 *     en cas d'erreur, attente de 2 s (contre les essais en série) ;
 *   - seules ces actions sont possibles, et les noms de paquets sont
 *     vérifiés (lettres minuscules, chiffres, « - + . _ ») : impossible de
 *     glisser une option ou une commande ;
 *   - npk est lancé avec un environnement propre ; il ne fait confiance
 *     qu'aux paquets signés du dépôt.
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <ctype.h>
#include <shadow.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PKGS 32

static bool valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 64 || !(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0]))) return false;
    for (; *s; s++)
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && !strchr("-+._", *s)) return false;
    return true;
}

static bool check_password(void)
{
    char pw[256] = "";
    if (!fgets(pw, sizeof pw, stdin)) pw[0] = 0;
    pw[strcspn(pw, "\r\n")] = 0;
    struct spwd *sp = getspnam("root");
    bool ok = false;
    if (sp && sp->sp_pwdp) {
        const char *h = sp->sp_pwdp;
        if (!h[0]) ok = true;                          /* root sans mot de passe (mode live) */
        else if (h[0] != '!' && h[0] != '*') {
            const char *c = crypt(pw, h);
            ok = c && !strcmp(c, h);
        }
    }
    explicit_bzero(pw, sizeof pw);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage : nexus-pkg check|update|upgrade|install|remove [paquet...]\n");
        return 2;
    }
    const char *act = argv[1];
    bool alpine = !strncmp(act, "alpine-", 7);   /* paquets Alpine (npk alpine ...) */
    const char *sub = alpine ? act + 7 : act;
    bool with_names = !strcmp(sub, "install") || !strcmp(sub, "remove");
    bool no_names = (!alpine && !strcmp(act, "check")) || !strcmp(sub, "update") || !strcmp(sub, "upgrade");
    if (!with_names && !no_names) { fprintf(stderr, "nexus-pkg : action inconnue\n"); return 2; }
    if (no_names && argc != 2) { fprintf(stderr, "nexus-pkg : trop d'arguments\n"); return 2; }
    if (with_names && (argc < 3 || argc - 2 > MAX_PKGS)) { fprintf(stderr, "nexus-pkg : paquets ?\n"); return 2; }
    for (int i = 2; i < argc; i++)
        if (!valid_name(argv[i])) { fprintf(stderr, "nexus-pkg : nom de paquet invalide\n"); return 2; }

    if (getuid() != 0 && !check_password()) {
        sleep(2);
        fprintf(stderr, "Mot de passe administrateur incorrect\n");
        return 3;
    }
    if (!strcmp(act, "check")) return 0;

    if (setgid(0) != 0 || setuid(0) != 0) { perror("nexus-pkg"); return 1; }
    clearenv();
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("LANG", "C.UTF-8", 1);

    char *args[MAX_PKGS + 5];
    int n = 0;
    args[n++] = "npk";
    if (alpine) args[n++] = "alpine";
    args[n++] = (char *)sub;
    for (int i = 2; i < argc; i++) args[n++] = argv[i];
    args[n] = NULL;
    execv("/usr/bin/npk", args);
    perror("nexus-pkg : npk");
    return 1;
}
