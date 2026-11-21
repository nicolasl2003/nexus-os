#!/bin/sh
# Compile BusyBox en statique et l'installe dans build/rootfs
. "$(dirname "$0")/common.sh"

archive="$(fetch "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2")"
src="$SRC_DIR/busybox-${BUSYBOX_VERSION}"

if [ ! -d "$src" ]; then
    log "Extraction de BusyBox"
    tar -xf "$archive" -C "$SRC_DIR"
fi

cd "$src"
if [ ! -f .config ]; then
    log "Configuration de BusyBox (defconfig)"
    make defconfig >/dev/null
fi

# Réglages Nexus, appliqués à chaque build (idempotent)
cp .config .config.before
# Binaire statique : pas besoin de libc dans le système
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
# 'tc' ne compile plus avec les en-têtes noyau récents
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
# Mots de passe chiffrés en SHA-512 (le défaut DES est obsolète, 8 caractères max)
sed -i 's/^CONFIG_FEATURE_DEFAULT_PASSWD_ALGO=.*/CONFIG_FEATURE_DEFAULT_PASSWD_ALGO="sha512"/' .config
if ! cmp -s .config .config.before; then
    log "Configuration de BusyBox mise à jour"
    make oldconfig </dev/null >/dev/null
fi
rm -f .config.before

log "Compilation de BusyBox avec $JOBS jobs"
make -j"$JOBS"
rm -rf "$ROOTFS_DIR"
make CONFIG_PREFIX="$ROOTFS_DIR" install >/dev/null
log "BusyBox installé dans build/rootfs"
