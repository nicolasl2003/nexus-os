#!/bin/sh
# Démarre Nexus OS dans QEMU, console sur le terminal.
# Pour quitter : tape 'poweroff' ou Ctrl+A puis X.
. "$(dirname "$0")/common.sh"

start_repo_server
start_display_server

# shellcheck disable=SC2046,SC2086
exec qemu-system-x86_64 \
    $(qemu_perf_args) \
    ${QEMU_EXTRA:-} \
    $(qemu_audio_args) \
    $(qemu_disk_args) \
    -kernel "$OUT_DIR/bzImage" \
    -initrd "$OUT_DIR/initramfs.cpio.gz" \
    -append "$(kernel_console_args)" \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0,romfile= \
    $(qemu_display_args) \
    -no-reboot
