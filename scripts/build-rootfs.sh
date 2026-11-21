#!/bin/sh
# Assemble le système de fichiers racine et l'emballe en initramfs
. "$(dirname "$0")/common.sh"

[ -x "$ROOTFS_DIR/bin/busybox" ] || { echo "Lance d'abord build-busybox.sh" >&2; exit 1; }

log "Assemblage du rootfs"
cd "$ROOTFS_DIR"
mkdir -p dev proc sys tmp run root home etc/init.d var/log usr/local
ln -sfn /run var/run   # emplacement classique des sockets (wpa_supplicant, etc.)
chmod 1777 tmp

# Fichiers propres à Nexus OS (init, motd, os-release...)
cp -a "$ROOT_DIR/rootfs/." "$ROOTFS_DIR/"
sed -i "s/@VERSION@/$NEXUS_VERSION/g" etc/os-release etc/motd

# Vérification des paquets : minisign + clé publique du dépôt (dans le
# système de base, pour qu'un attaquant ne puisse pas les remplacer via npk)
[ -x "$MINISIGN" ] || { echo "minisign absent : lance 'make tools'" >&2; exit 1; }
[ -f "$NPK_PUB" ]  || { echo "clé publique absente : lance 'make keys'" >&2; exit 1; }
install -m 755 "$MINISIGN" usr/bin/minisign
mkdir -p etc/npk/keys
install -m 644 "$NPK_PUB" etc/npk/keys/nexus-npk.pub

# Droits : shadow lisible par root seul ; busybox setuid pour que
# passwd et su fonctionnent pour les utilisateurs (BusyBox abandonne
# les privilèges pour toutes les autres commandes).
chmod 600 etc/shadow
chmod 4755 bin/busybox

# Le noyau exécute /init depuis l'initramfs : on pointe sur busybox init
ln -sf sbin/init init

log "Création de l'initramfs"
find . -print0 | cpio --null -o -H newc --owner root:root 2>/dev/null \
    | gzip -9 > "$OUT_DIR/initramfs.cpio.gz"
log "Initramfs prêt : build/out/initramfs.cpio.gz ($(du -h "$OUT_DIR/initramfs.cpio.gz" | cut -f1))"
