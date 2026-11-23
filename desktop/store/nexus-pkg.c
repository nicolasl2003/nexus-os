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
 *   nexus-pkg parefeu on|off|ssh-on|ssh-off|ping-on|ping-off
 *                                        réglages du pare-feu (Paramètres → Sécurité)
 *   nexus-pkg blocage on|off             blocage après plusieurs mots de passe ratés
 *   nexus-pkg chiffrement on             chiffre le disque au prochain démarrage
 *                                        (ligne 2 de l'entrée : la phrase secrète)
 *   nexus-pkg chiffrement off            annule un chiffrement pas encore fait
 *   nexus-pkg chiffrement phrase         change la phrase secrète du disque
 *                                        (lignes 2 et 3 : ancienne, nouvelle)
 *   nexus-pkg journal                    affiche le journal de sécurité
 *
 * Sécurité :
 *   - un utilisateur autre que root doit fournir le mot de passe
 *     administrateur (root) sur l'entrée standard, comme pour « su » ;
 *     en cas d'erreur, attente de 2 s (contre les essais en série), et
 *     blocage temporaire après plusieurs erreurs si c'est activé
 *     (voir nexus-secu.h) ; tout est noté dans le journal de sécurité ;
 *   - seules ces actions sont possibles, et les noms de paquets sont
 *     vérifiés (lettres minuscules, chiffres, « - + . _ ») : impossible de
 *     glisser une option ou une commande ;
 *   - npk est lancé avec un environnement propre ; il ne fait confiance
 *     qu'aux paquets signés du dépôt.
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nexus-secu.h"

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

static char caller[64] = "?";

/* Vérifie le mot de passe root (sauf si on est déjà root), avec blocage
 * après plusieurs erreurs et journal. Retourne 0 ou le code de sortie. */
static int authorize(const char *what)
{
    if (getuid() == 0) return 0;
    char where[160];
    snprintf(where, sizeof where, "administration : %s, par %s", what, caller);
    int remaining;
    if (secu_locked("root", &remaining)) {
        char m[120];
        secu_locked_msg(m, sizeof m, remaining);
        fprintf(stderr, "%s\n", m);
        return 6;
    }
    if (!check_password()) {
        secu_fail("root", where);
        sleep(2);
        fprintf(stderr, "Mot de passe administrateur incorrect\n");
        return 3;
    }
    secu_ok("root", where);
    return 0;
}

static void become_root(void)
{
    if (setgid(0) != 0 || setuid(0) != 0) { perror("nexus-pkg"); exit(1); }
    clearenv();
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("LANG", "C.UTF-8", 1);
}

static void read_secret(char *buf, size_t n)
{
    if (!fgets(buf, n, stdin)) buf[0] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
}

/* ---- Blocage après plusieurs erreurs ---- */
static int do_blocage(bool on)
{
    struct secu_conf c = secu_read_conf();
    mkdir("/usr/local/etc", 0755);
    mkdir("/usr/local/etc/nexus", 0755);
    umask(022);
    FILE *f = fopen(SECU_CONF ".tmp", "w");
    if (!f) { perror("nexus-pkg : " SECU_CONF); return 1; }
    fprintf(f, "# Blocage après plusieurs mots de passe ratés (Paramètres → Protection)\n"
               "BLOCAGE=%d\nESSAIS=%d\nDUREE=%d\n", on ? 1 : 0, c.essais, c.duree_min);
    fclose(f);
    chmod(SECU_CONF ".tmp", 0644);
    if (rename(SECU_CONF ".tmp", SECU_CONF) != 0) { perror("nexus-pkg"); return 1; }
    if (!on) {                                   /* plus de blocage : on débloque tout le monde */
        char *a[] = { "rm", "-rf", SECU_STATE, NULL };
        pid_t p = fork();
        if (p == 0) { execv("/bin/rm", a); _exit(127); }
        if (p > 0) waitpid(p, NULL, 0);
    }
    secu_log("RÉGLAGE : blocage après %d erreurs %s (par %s)", c.essais, on ? "activé" : "désactivé", caller);
    return 0;
}

/* ---- Chiffrement du disque ---- */
#define CRYPT_KEY    "/data/etc/.chiffrement-cle"   /* phrase en attente du prochain démarrage */
#define CRYPT_MAPPER "/dev/mapper/nexus-data"
#define CRYPT_DEV    "/run/nexus-data.dev"          /* disque chiffré (écrit par rcS) */

static bool write_secret(const char *path, const char *secret)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    size_t l = strlen(secret);
    bool ok = write(fd, secret, l) == (ssize_t)l;
    if (fsync(fd) != 0) ok = false;
    close(fd);
    return ok;
}

static int run_cmd(char *const argv[])
{
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }   /* nos propres messages, en français */
        execv(argv[0], argv);
        _exit(127);
    }
    int st;
    if (p < 0 || waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static bool phrase_ok(const char *p)
{
    if (strlen(p) < 8) { fprintf(stderr, "La phrase secrète doit faire au moins 8 caractères.\n"); return false; }
    return true;
}

static int do_chiffrement(const char *what)
{
    bool encrypted = access(CRYPT_MAPPER, F_OK) == 0;
    if (!strcmp(what, "on")) {
        char phrase[256];
        read_secret(phrase, sizeof phrase);
        if (encrypted) { explicit_bzero(phrase, sizeof phrase); fprintf(stderr, "Le disque est déjà chiffré.\n"); return 4; }
        if (access("/data/etc", W_OK) != 0) {
            explicit_bzero(phrase, sizeof phrase);
            fprintf(stderr, "Pas de disque NEXUS_DATA : rien à chiffrer (mode live).\n");
            return 4;
        }
        if (!phrase_ok(phrase)) { explicit_bzero(phrase, sizeof phrase); return 2; }
        bool ok = write_secret(CRYPT_KEY, phrase);
        explicit_bzero(phrase, sizeof phrase);
        if (!ok) { fprintf(stderr, "Impossible d'enregistrer la demande.\n"); return 1; }
        secu_log("CHIFFREMENT du disque demandé : il sera fait au prochain démarrage (par %s)", caller);
        return 0;
    }
    if (!strcmp(what, "off")) {
        if (unlink(CRYPT_KEY) == 0) secu_log("CHIFFREMENT du disque annulé avant d'être fait (par %s)", caller);
        if (encrypted) { fprintf(stderr, "Le disque est déjà chiffré : ce n'est pas réversible.\n"); return 4; }
        return 0;
    }
    /* phrase : changer la phrase secrète */
    char oldp[256], newp[256];
    read_secret(oldp, sizeof oldp);
    read_secret(newp, sizeof newp);
    int rc = 0;
    char dev[128] = "";
    FILE *f = fopen(CRYPT_DEV, "r");
    if (f) { if (fgets(dev, sizeof dev, f)) dev[strcspn(dev, "\n")] = 0; fclose(f); }
    if (!encrypted || strncmp(dev, "/dev/", 5)) { fprintf(stderr, "Le disque n'est pas chiffré.\n"); rc = 4; }
    else if (!phrase_ok(newp)) rc = 2;
    else {
        mkdir("/run/nexus-cle", 0700);
        if (!write_secret("/run/nexus-cle/ancienne", oldp) || !write_secret("/run/nexus-cle/nouvelle", newp)) rc = 1;
        else {
            char *a[] = { "/usr/sbin/cryptsetup", "luksChangeKey", dev, "--batch-mode",
                          "--key-file", "/run/nexus-cle/ancienne", "--pbkdf-memory", "262144",
                          "/run/nexus-cle/nouvelle", NULL };
            int r = run_cmd(a);
            if (r == 0) secu_log("CHIFFREMENT : phrase secrète du disque changée (par %s)", caller);
            else if (r == 2) { secu_log("ÉCHEC : ancienne phrase secrète du disque incorrecte (par %s)", caller);
                               fprintf(stderr, "Ancienne phrase secrète incorrecte.\n"); rc = 7; }
            else { fprintf(stderr, "Échec de cryptsetup (code %d).\n", r); rc = 1; }
        }
        unlink("/run/nexus-cle/ancienne");
        unlink("/run/nexus-cle/nouvelle");
        rmdir("/run/nexus-cle");
    }
    explicit_bzero(oldp, sizeof oldp);
    explicit_bzero(newp, sizeof newp);
    return rc;
}

/* ---- Journal de sécurité : les 300 dernières lignes ---- */
static int do_journal(void)
{
    FILE *f = fopen(SECU_LOG, "r");
    if (!f) { printf("(journal vide)\n"); return 0; }
    static char lines[300][256];
    int n = 0;
    char buf[256];
    while (fgets(buf, sizeof buf, f)) { snprintf(lines[n % 300], 256, "%s", buf); n++; }
    fclose(f);
    for (int i = n > 300 ? n - 300 : 0; i < n; i++) fputs(lines[i % 300], stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage : nexus-pkg check|update|upgrade|install|remove [paquet...]\n");
        return 2;
    }
    const char *act = argv[1];
    struct passwd *me = getpwuid(getuid());
    if (me) snprintf(caller, sizeof caller, "%s", me->pw_name);
    if (!strcmp(act, "parefeu")) {
        static const char *ok[] = { "on", "off", "ssh-on", "ssh-off", "ping-on", "ping-off" };
        bool valid = false;
        for (unsigned i = 0; argc == 3 && i < sizeof ok / sizeof ok[0]; i++) valid |= !strcmp(argv[2], ok[i]);
        if (!valid) { fprintf(stderr, "nexus-pkg : réglage du pare-feu inconnu\n"); return 2; }
        int rc = authorize("pare-feu");
        if (rc) return rc;
        become_root();
        setenv("NEXUS_PAR", caller, 1);             /* pour le journal */
        char *a[] = { "nexus-parefeu", "set", argv[2], NULL };
        execv("/usr/local/sbin/nexus-parefeu", a);
        perror("nexus-pkg : nexus-parefeu");
        return 1;
    }
    if (!strcmp(act, "blocage")) {
        if (argc != 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) { fprintf(stderr, "nexus-pkg : blocage on|off\n"); return 2; }
        int rc = authorize("blocage des comptes");
        if (rc) return rc;
        become_root();
        return do_blocage(!strcmp(argv[2], "on"));
    }
    if (!strcmp(act, "chiffrement")) {
        if (argc != 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off") && strcmp(argv[2], "phrase"))) {
            fprintf(stderr, "nexus-pkg : chiffrement on|off|phrase\n");
            return 2;
        }
        int rc = authorize("chiffrement du disque");
        if (rc) return rc;
        become_root();
        return do_chiffrement(argv[2]);
    }
    if (!strcmp(act, "journal")) {
        if (argc != 2) return 2;
        int rc = authorize("lecture du journal");
        if (rc) return rc;
        become_root();
        return do_journal();
    }
    bool alpine = !strncmp(act, "alpine-", 7);   /* paquets Alpine (npk alpine ...) */
    const char *sub = alpine ? act + 7 : act;
    bool with_names = !strcmp(sub, "install") || !strcmp(sub, "remove");
    bool no_names = (!alpine && !strcmp(act, "check")) || !strcmp(sub, "update") || !strcmp(sub, "upgrade");
    if (!with_names && !no_names) { fprintf(stderr, "nexus-pkg : action inconnue\n"); return 2; }
    if (no_names && argc != 2) { fprintf(stderr, "nexus-pkg : trop d'arguments\n"); return 2; }
    if (with_names && (argc < 3 || argc - 2 > MAX_PKGS)) { fprintf(stderr, "nexus-pkg : paquets ?\n"); return 2; }
    for (int i = 2; i < argc; i++)
        if (!valid_name(argv[i])) { fprintf(stderr, "nexus-pkg : nom de paquet invalide\n"); return 2; }

    int rc = authorize(!strcmp(act, "check") ? "Magasin d'applications" : act);
    if (rc) return rc;
    if (!strcmp(act, "check")) return 0;

    become_root();
    setenv("NEXUS_PAR", caller, 1);                 /* npk le note dans le journal */

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
