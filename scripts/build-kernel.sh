#!/bin/sh
# Compile le noyau Linux -> build/out/bzImage
. "$(dirname "$0")/common.sh"

major="${KERNEL_VERSION%%.*}"
archive="$(fetch "https://cdn.kernel.org/pub/linux/kernel/v${major}.x/linux-${KERNEL_VERSION}.tar.xz")"
src="$SRC_DIR/linux-${KERNEL_VERSION}"

if [ ! -d "$src" ]; then
    log "Extraction du noyau"
    tar -xf "$archive" -C "$SRC_DIR"
fi

cd "$src"
if [ ! -f .config ]; then
    log "Configuration de base (x86_64_defconfig)"
    make x86_64_defconfig >/dev/null
fi

# Fragment Nexus appliqué à chaque build (idempotent) : modifier
# config/kernel.fragment suffit pour changer la configuration du noyau.
cp .config .config.before
./scripts/kconfig/merge_config.sh -m .config "$ROOT_DIR/config/kernel.fragment" >/dev/null
make olddefconfig >/dev/null
if ! cmp -s .config .config.before; then
    log "Configuration du noyau mise à jour depuis config/kernel.fragment"
fi
rm -f .config.before
# Vérifie que chaque option demandée a bien été retenue
grep -E '^(# )?CONFIG_' "$ROOT_DIR/config/kernel.fragment" | while IFS= read -r line; do
    grep -qxF "$line" .config || log "ATTENTION : option non retenue : $line"
done

# Logo de démarrage de Nexus OS (à la place du manchot Tux)
if [ -f "$ROOT_DIR/config/logo/nexus_logo_clut224.ppm" ]; then
    cmp -s "$ROOT_DIR/config/logo/nexus_logo_clut224.ppm" drivers/video/logo/logo_linux_clut224.ppm ||
        cp "$ROOT_DIR/config/logo/nexus_logo_clut224.ppm" drivers/video/logo/logo_linux_clut224.ppm
fi

log "Compilation du noyau avec $JOBS jobs"
make -j"$JOBS" bzImage modules
cp arch/x86/boot/bzImage "$OUT_DIR/bzImage"

# Modules (pilotes Wi-Fi...) : installés dans build/out/modules, puis
# empaquetés par packages/linux-wifi. depmod génère modules.dep / modules.alias
# (utilisés par modprobe pour charger le bon pilote selon le matériel).
rm -rf "$OUT_DIR/modules"
make modules_install INSTALL_MOD_PATH="$OUT_DIR/modules" INSTALL_MOD_STRIP=1 >/dev/null
KREL=$(make -s kernelrelease)
depmod -b "$OUT_DIR/modules" "$KREL"
echo "$KREL" > "$OUT_DIR/kernel-release"
log "Noyau prêt : build/out/bzImage ($KREL) + $(find "$OUT_DIR/modules" -name '*.ko' | wc -l) modules"
