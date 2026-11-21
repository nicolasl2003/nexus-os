#!/bin/sh
# Démarre l'ISO dans QEMU comme sur un vrai PC (via GRUB).
# Usage : run-iso.sh [bios|uefi]   (défaut : bios)
. "$(dirname "$0")/common.sh"

iso="$OUT_DIR/nexus-os-${NEXUS_VERSION}-x86_64.iso"
firmware=""
if [ "${1:-bios}" = "uefi" ]; then
    firmware="-bios /usr/share/ovmf/OVMF.fd"
fi

start_repo_server
start_display_server

# shellcheck disable=SC2046,SC2086
exec qemu-system-x86_64 \
    $(qemu_perf_args) \
    ${QEMU_EXTRA:-} \
    $(qemu_audio_args) \
    $firmware \
    $(qemu_disk_args) \
    -cdrom "$iso" -boot d \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0,romfile= \
    $(qemu_display_args) \
    -no-reboot
