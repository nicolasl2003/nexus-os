/* Applications épinglées à la barre des tâches (liste propre à chaque compte,
 * ~/.config/nexus/epingles : une ligne « app_id|nom|commande »). */
#ifndef NEXUS_PINS_H
#define NEXUS_PINS_H

#include <stdbool.h>

struct nx_pin { char app_id[128], label[128], cmd[512]; };

#define NX_MAX_PINS 16

/* Lit la liste (celle par défaut si le compte n'en a pas encore). Les
 * applications désinstallées sont gardées dans le fichier mais pas renvoyées. */
int nx_pins_load(struct nx_pin *out, int max);
/* Peut-on épingler cette application ? Remplit le nom et la commande. */
bool nx_pin_info(const char *app_id, struct nx_pin *out);

#endif
