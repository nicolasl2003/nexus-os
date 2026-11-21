#!/bin/sh
# Crée le disque de données persistant de Nexus OS (ext4, étiquette NEXUS_DATA).
# Usage : make-disk.sh [--force]   (--force efface et recrée le disque)
. "$(dirname "$0")/common.sh"

if [ -f "$DATA_DISK" ] && [ "${1:-}" != "--force" ]; then
    log "Disque déjà présent : disk/$(basename "$DATA_DISK") (rien à faire)"
    exit 0
fi

mkdir -p "$(dirname "$DATA_DISK")"
rm -f "$DATA_DISK"
log "Création du disque de données ($DATA_DISK_SIZE)"
truncate -s "$DATA_DISK_SIZE" "$DATA_DISK"
mkfs.ext4 -q -F -L NEXUS_DATA "$DATA_DISK"
log "Disque prêt : disk/$(basename "$DATA_DISK")"
