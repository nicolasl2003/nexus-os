/* nexus-login — gestionnaire de connexion graphique de Nexus OS (root, tty1).
 *
 * Boucle :
 *   1. premier démarrage avec disque : assistant texte nexus-setup ;
 *   2. écran de connexion : labwc (config dédiée) + nexus-greeter ;
 *   3. le greeter écrit le compte authentifié dans /run/nexus-login.user ;
 *   4. ouverture de la session : nexus-desktop avec les droits du compte ;
 *   5. à la déconnexion, retour à l'écran de connexion.
 * Si l'affichage graphique est indisponible (ou échoue plusieurs fois de
 * suite), on revient à la connexion texte classique (nexus-getty).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RESULT_FILE "/run/nexus-login.user"

/* Disposition du clavier de l'écran de connexion : celle du système
 * (nexus-keymap xkb -> XKB_LAYOUT=..., XKB_VARIANT=...) */
static void set_keyboard(void)
{
    setenv("XKB_DEFAULT_LAYOUT", "fr", 1);
    unsetenv("XKB_DEFAULT_VARIANT");
    FILE *f = popen("/usr/sbin/nexus-keymap xkb 2>/dev/null", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "XKB_LAYOUT=", 11) && line[11]) setenv("XKB_DEFAULT_LAYOUT", line + 11, 1);
        if (!strncmp(line, "XKB_VARIANT=", 12) && line[12]) setenv("XKB_DEFAULT_VARIANT", line + 12, 1);
    }
    pclose(f);
}

static void text_login(void)
{
    execl("/usr/sbin/nexus-getty", "nexus-getty", "tty1", "38400", "linux", (char *)NULL);
    _exit(1);
}

static bool needs_setup(void)
{
    return access("/data/etc", F_OK) == 0 && access("/data/etc/.configured", F_OK) != 0;
}

static bool graphics_ready(void)
{
    if (access("/usr/local/bin/labwc", X_OK) || access("/usr/local/bin/nexus-greeter", X_OK) ||
        access("/dev/dri/card0", F_OK))
        return false;
    for (int i = 0; i < 50; i++) {          /* attendre seatd (jusqu'à 10 s) */
        if (access("/run/seatd.sock", F_OK) == 0) return true;
        usleep(200000);
    }
    return false;
}

static void common_env(void)
{
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:/usr/local/sbin", 1);
    setenv("LANG", "C.UTF-8", 1);
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 0);
    set_keyboard();
    setenv("XCOMPOSEFILE", "/usr/local/share/X11/locale/en_US.UTF-8/Compose", 1);
    setenv("XDG_DATA_DIRS", "/usr/local/share", 1);
    setenv("LIBSEAT_BACKEND", "seatd", 1);
    setenv("WLR_RENDERER", "pixman", 1);
}

/* Écran de connexion ; renvoie le compte authentifié ou "" */
static void run_greeter(char *user, size_t n)
{
    user[0] = 0;
    unlink(RESULT_FILE);
    pid_t pid = fork();
    if (pid == 0) {
        clearenv();
        common_env();
        setenv("HOME", "/root", 1);
        mkdir("/run/user", 01777);
        mkdir("/run/user/0", 0700);
        setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
        execl("/usr/local/bin/labwc", "labwc", "-C", "/usr/local/etc/nexus/greeter",
              "-s", "nexus-greeter", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return;
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    int fd = open(RESULT_FILE, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t r = read(fd, user, n - 1);
        user[r > 0 ? r : 0] = 0;
        close(fd);
        unlink(RESULT_FILE);
    }
}

/* Assistant de configuration graphique (premier démarrage), dans un labwc
 * dédié dont le dossier de configuration est dans /run : l'assistant peut y
 * écrire la disposition du clavier choisie (relue par labwc sur SIGHUP). */
#define ASSISTANT_CONF "/run/nexus/assistant-labwc"
static void run_assistant(void)
{
    mkdir("/run/nexus", 0755);
    mkdir(ASSISTANT_CONF, 0700);
    if (system("cp /usr/local/etc/nexus/greeter/rc.xml " ASSISTANT_CONF "/rc.xml 2>/dev/null") != 0) { /* défauts de labwc */ }
    FILE *f = fopen(ASSISTANT_CONF "/environment", "w");
    if (f) fclose(f);
    pid_t pid = fork();
    if (pid == 0) {
        clearenv();
        common_env();
        setenv("HOME", "/root", 1);
        mkdir("/run/user", 01777);
        mkdir("/run/user/0", 0700);
        setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
        setenv("NEXUS_ASSISTANT_CONF", ASSISTANT_CONF, 1);
        execl("/usr/local/bin/labwc", "labwc", "-C", ASSISTANT_CONF, "-s", "nexus-assistant", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

/* Ouvre la session du compte et attend sa fermeture */
static void run_session(const char *name)
{
    struct passwd *pw = getpwnam(name);
    if (!pw) return;
    /* Groupes du bureau : video (écran), audio (son), netdev (Wi-Fi) */
    static const char *groups[] = { "video", "audio", "netdev" };
    for (unsigned k = 0; pw->pw_uid != 0 && k < 3; k++) {
        struct group *g = getgrnam(groups[k]);
        bool member = false;
        for (char **m = g ? g->gr_mem : NULL; m && *m; m++) if (!strcmp(*m, name)) member = true;
        if (g && !member) {
            char cmd[160];
            snprintf(cmd, sizeof cmd, "addgroup '%s' %s >/dev/null 2>&1", name, groups[k]);
            if (system(cmd) != 0) { /* nexus-desktop expliquera le problème */ }
        }
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0)
            _exit(1);
        clearenv();
        common_env();
        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
        setenv("LOGNAME", pw->pw_name, 1);
        setenv("SHELL", pw->pw_shell, 1);
        setenv("NEXUS_SESSION", "1", 1);
        if (chdir(pw->pw_dir) != 0 && chdir("/") != 0) _exit(1);
        umask(027);   /* fichiers créés : pas lisibles par les autres comptes */
        execl("/usr/local/bin/nexus-desktop", "nexus-desktop", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

int main(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    int quick_failures = 0;
    for (;;) {
        if (needs_setup()) {
            /* même verrou que sur la console série : un seul assistant à la fois */
            int lock = open("/run/nexus-setup.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0) {
                /* 1. assistant graphique (3 essais), 2. sinon assistant texte */
                for (int i = 0; i < 3 && needs_setup() && graphics_ready() &&
                                access("/usr/local/bin/nexus-assistant", X_OK) == 0; i++)
                    run_assistant();
                if (needs_setup() && system("/usr/sbin/nexus-setup --firstboot") != 0) { /* reboucle */ }
                flock(lock, LOCK_UN);
            } else {
                while (needs_setup()) sleep(2);   /* assistant ouvert ailleurs (console série) */
            }
            if (lock >= 0) close(lock);
            continue;
        }
        if (!graphics_ready()) text_login();
        time_t start = time(NULL);
        char user[64];
        run_greeter(user, sizeof user);
        if (!user[0]) {
            /* l'écran de connexion s'est fermé sans connexion : plantage ? */
            if (time(NULL) - start < 10) quick_failures++;
            else quick_failures = 0;
            if (quick_failures >= 3) text_login();
            sleep(1);
            continue;
        }
        quick_failures = 0;
        run_session(user);
    }
}
