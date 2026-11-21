#!/bin/sh
# Crée une ISO bootable (BIOS + UEFI) avec GRUB -> build/out/nexus-os-<version>-x86_64.iso
. "$(dirname "$0")/common.sh"

for f in bzImage initramfs.cpio.gz; do
    [ -f "$OUT_DIR/$f" ] || { echo "Manque build/out/$f : lance d'abord make kernel busybox rootfs" >&2; exit 1; }
done

iso_root="$BUILD_DIR/iso"
iso="$OUT_DIR/nexus-os-${NEXUS_VERSION}-x86_64.iso"

log "Préparation de l'arborescence ISO"
rm -rf "$iso_root"
mkdir -p "$iso_root/boot/grub"
cp "$OUT_DIR/bzImage" "$OUT_DIR/initramfs.cpio.gz" "$iso_root/boot/"
sed "s/@VERSION@/$NEXUS_VERSION/g" "$ROOT_DIR/config/grub.cfg" > "$iso_root/boot/grub/grub.cfg"

log "Génération de l'ISO avec grub-mkrescue (BIOS + UEFI)"
grub-mkrescue -o "$iso" "$iso_root" -- -volid NEXUS_OS 2>/dev/null
log "ISO prête : $(basename "$iso") ($(du -h "$iso" | cut -f1))"
