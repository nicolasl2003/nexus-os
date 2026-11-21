# Environnement de build de Nexus OS
# Contient tout le nécessaire pour compiler le noyau, BusyBox,
# générer l'initramfs et lancer le système dans QEMU.
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential bc bison flex \
        libelf-dev libssl-dev libncurses-dev \
        wget ca-certificates xz-utils bzip2 cpio gzip \
        qemu-system-x86 ovmf \
        grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools \
        busybox kmod \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
