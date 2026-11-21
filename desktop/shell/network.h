/* Réseau de Nexus OS : icône d'état pour la barre des tâches */
#ifndef NEXUS_NETWORK_H
#define NEXUS_NETWORK_H
#include <stdbool.h>

/* "ethernet" ou "network" (Wi-Fi) ; *connected indique si une interface est active */
const char *nx_network_icon(bool *connected);

#endif
