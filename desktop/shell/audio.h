/* Son de Nexus OS (ALSA) : utilisé par la barre des tâches et le panneau Son */
#ifndef NEXUS_AUDIO_H
#define NEXUS_AUDIO_H
#include <stdbool.h>

bool nx_audio_available(void);
bool nx_volume_get(int *pct, bool *muted);
void nx_volume_set(int pct);
void nx_volume_set_mute(bool mute);
const char *nx_volume_icon(void);   /* "volume-0..3", "volume-mute" ou NULL */

#endif
