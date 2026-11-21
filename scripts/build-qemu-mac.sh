#!/bin/sh
# Compile une version minimale de QEMU pour le Mac -> tools/qemu
# (utilisé par 'make run-native' si QEMU n'est pas installé autrement).
# Seulement : émulateur x86_64, accélération HVF, fenêtre Cocoa, réseau slirp.
# Dépendances Homebrew : glib pixman libslirp meson ninja pkgconf
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER=11.1.3
SRC="$ROOT/tools/src/qemu-$VER"
cd "$ROOT/tools/src"
[ -f "qemu-$VER.tar.xz" ] || curl -fL -o "qemu-$VER.tar.xz" "https://download.qemu.org/qemu-$VER.tar.xz"
[ -d "$SRC" ] || tar -xf "qemu-$VER.tar.xz"
cd "$SRC"
./configure --prefix="$ROOT/tools/qemu" --target-list=x86_64-softmmu \
    --enable-hvf --enable-cocoa --enable-slirp \
    --disable-gnutls --disable-nettle --disable-gcrypt --disable-curl --disable-libssh \
    --enable-vnc --disable-vnc-sasl --disable-vnc-jpeg --disable-sdl --disable-gtk --disable-spice --disable-usb-redir \
    --disable-docs --disable-guest-agent --disable-tools --disable-capstone \
    --disable-werror
make -j"$(sysctl -n hw.ncpu)"
make install
"$ROOT/tools/qemu/bin/qemu-system-x86_64" --version | head -1
