/* nexus-secu.h — blocage après plusieurs mots de passe ratés et journal de
 * sécurité de Nexus OS (code partagé par les programmes qui vérifient un mot
 * de passe en root : écran de connexion, nexus-account, nexus-pkg).
 *
 * Réglages : /usr/local/etc/nexus/securite.conf
 *     BLOCAGE=0|1     blocage activé (désactivé par défaut)
 *     ESSAIS=5        nombre d'erreurs avant blocage
 *     DUREE=5         durée du blocage en minutes
 * État : /usr/local/var/lib/nexus/echecs/<compte> (« erreurs fin_du_blocage »)
 * Journal : /usr/local/var/log/securite.log (root seulement, 0600)
 *
 * Fonctions « static » : chaque programme en a sa copie (pas de bibliothèque
 * partagée dans Nexus OS).
 */
#ifndef NEXUS_SECU_H
#define NEXUS_SECU_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SECU_CONF  "/usr/local/etc/nexus/securite.conf"
#define SECU_STATE "/usr/local/var/lib/nexus/echecs"
#define SECU_LOG   "/usr/local/var/log/securite.log"

struct secu_conf { bool blocage; int essais, duree_min; };

static __attribute__((unused)) struct secu_conf secu_read_conf(void)
{
    struct secu_conf c = { false, 5, 5 };
    FILE *f = fopen(SECU_CONF, "r");
    if (!f) return c;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "BLOCAGE=", 8)) c.blocage = atoi(line + 8) != 0;
        else if (!strncmp(line, "ESSAIS=", 7)) c.essais = atoi(line + 7);
        else if (!strncmp(line, "DUREE=", 6)) c.duree_min = atoi(line + 6);
    }
    fclose(f);
    if (c.essais < 2 || c.essais > 50) c.essais = 5;
    if (c.duree_min < 1 || c.duree_min > 1440) c.duree_min = 5;
    return c;
}

static __attribute__((unused)) void secu_mkdirs(void)
{
    mkdir("/usr/local/var", 0755);
    mkdir("/usr/local/var/lib", 0755);
    mkdir("/usr/local/var/lib/nexus", 0755);
    mkdir(SECU_STATE, 0700);
    mkdir("/usr/local/var/log", 0755);
}

/* Journal : une ligne horodatée (2026-11-23 10:15:02 message) */
static __attribute__((unused)) void secu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static __attribute__((unused)) void secu_log(const char *fmt, ...)
{
    secu_mkdirs();
    mode_t old = umask(077);
    FILE *f = fopen(SECU_LOG, "a");
    umask(old);
    if (!f) return;
    if (!getenv("TZ")) { setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset(); }   /* heure de Paris */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static __attribute__((unused)) void secu_path(char *out, size_t n, const char *user)
{
    snprintf(out, n, "%s/%s", SECU_STATE, user);
}

/* Compte bloqué ? (*remaining : secondes restantes) */
static __attribute__((unused)) bool secu_locked(const char *user, int *remaining)
{
    struct secu_conf c = secu_read_conf();
    if (remaining) *remaining = 0;
    if (!c.blocage) return false;
    char p[256];
    secu_path(p, sizeof p, user);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    long count = 0, until = 0;
    if (fscanf(f, "%ld %ld", &count, &until) != 2) until = 0;
    fclose(f);
    long now = (long)time(NULL);
    if (until > now) { if (remaining) *remaining = (int)(until - now); return true; }
    return false;
}

/* Mot de passe raté : compte l'erreur, bloque au-delà de ESSAIS (si activé).
 * Retourne true si le compte vient d'être bloqué. */
static __attribute__((unused)) bool secu_fail(const char *user, const char *where)
{
    struct secu_conf c = secu_read_conf();
    secu_mkdirs();
    char p[256];
    secu_path(p, sizeof p, user);
    long count = 0, until = 0;
    FILE *f = fopen(p, "r");
    if (f) { if (fscanf(f, "%ld %ld", &count, &until) != 2) count = until = 0; fclose(f); }
    long now = (long)time(NULL);
    if (until && until <= now) count = 0;        /* blocage terminé : on repart de zéro */
    count++;
    bool blocked = false;
    until = 0;
    if (c.blocage && count >= c.essais) {
        until = now + c.duree_min * 60L;
        blocked = true;
    }
    mode_t old = umask(077);
    if ((f = fopen(p, "w"))) { fprintf(f, "%ld %ld\n", count, until); fclose(f); }
    umask(old);
    secu_log("ÉCHEC mot de passe : compte %s (%s), erreur n°%ld", user, where, count);
    if (blocked) secu_log("BLOCAGE : compte %s bloqué %d min après %ld erreurs", user, c.duree_min, count);
    return blocked;
}

static __attribute__((unused)) void secu_ok(const char *user, const char *where)
{
    char p[256];
    secu_path(p, sizeof p, user);
    remove(p);
    secu_log("OK : %s (%s)", where, user);
}

/* Message « Trop d'essais : réessaie dans N min » */
static __attribute__((unused)) void secu_locked_msg(char *out, size_t n, int remaining)
{
    int min = (remaining + 59) / 60;
    snprintf(out, n, "Trop d'essais ratés : réessaie dans %d minute%s.", min, min > 1 ? "s" : "");
}

#endif
