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

# --- Variante setuid minimale : su, passwd, ping -----------------------------
# Seuls ces trois outils ont besoin des droits root pour un utilisateur
# ordinaire. Ils ont leur propre petit BusyBox (setuid) ; le BusyBox principal
# (ls, wget, sh…) n'a plus aucun droit particulier.
SUID_SRC="$SRC_DIR/busybox-${BUSYBOX_VERSION}-suid"
if [ ! -d "$SUID_SRC" ]; then
    mkdir -p "$SUID_SRC"
    tar -xf "$archive" -C "$SUID_SRC" --strip-components=1
fi
cd "$SUID_SRC"
# set_opt NOM valeur : remplace la ligne existante (activée ou « is not set »)
set_opt() {
    sed -i -e "/^CONFIG_$1=/d" -e "/^# CONFIG_$1 is not set/d" .config
    echo "CONFIG_$1=$2" >> .config
}
SUID_OPTS="STATIC LONG_OPTS FEATURE_SUID FEATURE_SHADOWPASSWDS USE_BB_PWD_GRP USE_BB_SHADOW
           USE_BB_CRYPT USE_BB_CRYPT_SHA SU FEATURE_SU_CHECKS_SHELLS PASSWD
           FEATURE_PASSWD_WEAK_CHECK PING PING6 FEATURE_FANCY_PING FEATURE_IPV6"
if [ ! -f .config ] || ! grep -q '^CONFIG_STATIC=y' .config || [ "${FORCE:-0}" = 1 ]; then
    make allnoconfig >/dev/null
    # deux passes : certaines options n'apparaissent qu'une fois leur parent activé
    for pass in 1 2; do
        for o in $SUID_OPTS; do set_opt "$o" y; done
        set_opt FEATURE_DEFAULT_PASSWD_ALGO '"sha512"'
        yes "" | make oldconfig >/dev/null 2>&1
    done
    grep -q '^CONFIG_STATIC=y' .config || { echo "busybox-suid : configuration statique refusée" >&2; exit 1; }
fi
log "Compilation de BusyBox setuid (su, passwd, ping)"
make -j"$JOBS" >/dev/null
install -m 755 busybox "$ROOTFS_DIR/bin/busybox-suid"
cd "$ROOTFS_DIR"
for app in su passwd ping ping6; do
    for d in bin sbin usr/bin usr/sbin; do
        if [ -L "$d/$app" ]; then rm -f "$d/$app"; ln -s /bin/busybox-suid "$d/$app"; fi
    done
done
log "BusyBox setuid : $(ls -l "$ROOTFS_DIR/bin/busybox-suid" | awk '{print $5}') octets"
