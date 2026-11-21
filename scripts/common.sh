#!/bin/sh
# Variables partagées par tous les scripts de build.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT_DIR/config/versions.env"

BUILD_DIR="$ROOT_DIR/build"
SRC_DIR="$BUILD_DIR/src"
OUT_DIR="$BUILD_DIR/out"
ROOTFS_DIR="$BUILD_DIR/rootfs"
JOBS="$(nproc 2>/dev/null || echo 2)"

# Disque de données persistant (monté sur /data dans Nexus OS)
DATA_DISK="$ROOT_DIR/disk/nexus-data.img"
DATA_DISK_SIZE="${DATA_DISK_SIZE:-1G}"

# Dépôt de paquets npk (servi en HTTP à QEMU sur 10.0.2.2:8080)
REPO_DIR="${REPO_DIR:-$BUILD_DIR/repo}"

start_repo_server() {
    if [ -f "$REPO_DIR/INDEX" ]; then
        busybox httpd -p 8080 -h "$REPO_DIR"
        log "Dépôt npk servi sur http://10.0.2.2:8080 ($(wc -l < "$REPO_DIR/INDEX") paquets)"
    else
        log "Pas de dépôt npk (lance 'make packages' pour en créer un)"
    fi
}

# Affichage : terminal texte (défaut) ou écran graphique (GUI=1).
# En mode graphique, l'écran est exporté en VNC (port 5900) et en VNC sur
# websocket (port 5700), affiché dans le navigateur par noVNC (port 6080).
GUI="${GUI:-0}"
NOVNC_DIR="$BUILD_DIR/tools/novnc"

qemu_display_args() {
    if [ "$GUI" = 1 ]; then
        # Carte graphique virtio + clavier et souris "tablette" (pointeur
        # absolu : la souris suit exactement le curseur du navigateur)
        echo "-device virtio-vga -device virtio-keyboard-pci -device virtio-tablet-pci"
        echo "-vnc :0,websocket=5700 -serial mon:stdio"
    else
        echo "-nographic"
    fi
}

# Consoles du noyau : en graphique, messages sur l'écran ET le port série
kernel_console_args() {
    if [ "$GUI" = 1 ]; then echo "console=tty0 console=ttyS0 quiet loglevel=5 fbcon=logo-count:1"; else echo "console=ttyS0 quiet"; fi
}

start_display_server() {
    [ "$GUI" = 1 ] || return 0
    [ -f "$NOVNC_DIR/vnc.html" ] || { echo "noVNC absent : lance 'make novnc'" >&2; exit 1; }
    busybox httpd -p 6080 -h "$NOVNC_DIR"
    log "Écran de Nexus OS : ouvre http://localhost:6080 dans ton navigateur"
    log "(ou un client VNC sur localhost:5900). Le terminal reste la console série."
}

# Arguments QEMU pour brancher le disque de données s'il existe
# Performances de la VM :
# - KVM si disponible (PC Linux) : vitesse quasi native ;
# - sinon émulation logicielle (TCG) multi-cœur : 4 processeurs virtuels
#   exécutés en parallèle sur les cœurs de la machine hôte (MTTCG).
qemu_perf_args() {
    if [ -w /dev/kvm ]; then
        echo "-accel kvm -cpu host -smp 4 -m 1G"
    else
        echo "-accel tcg,thread=multi -smp 4 -m 1G"
    fi
}

# Carte son Intel HD Audio. Dans Docker : son muet (pas de sortie audio) ;
# run-native.sh utilise la sortie audio du Mac (CoreAudio).
qemu_audio_args() {
    echo "-audiodev ${QEMU_AUDIO:-none},id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0"
}

# NO_DISK=1 : démarre en mode live sans brancher le disque (sans y toucher)
qemu_disk_args() {
    if [ -f "$DATA_DISK" ] && [ "${NO_DISK:-0}" != 1 ]; then
        echo "-drive file=$DATA_DISK,if=virtio,format=raw"
    fi
}

mkdir -p "$SRC_DIR" "$OUT_DIR"

log() { printf '\033[1;36m[nexus]\033[0m %s\n' "$*" >&2; }

# fetch <url> [nom] : télécharge une archive dans build/src si absente
fetch() {
    file="$SRC_DIR/${2:-$(basename "$1")}"
    if [ ! -f "$file" ]; then
        log "Téléchargement de $(basename "$file")"
        wget -q --show-progress -O "$file.part" "$1"
        mv "$file.part" "$file"
    fi
    echo "$file"
}

# Chaîne de compilation musl -> $MUSL_CC (binaires statiques autonomes).
# Utilisée pour les paquets npk et pour les outils du système de base.
TOOLS_DIR="$BUILD_DIR/tools"
MINISIGN="$TOOLS_DIR/bin/minisign"
NPK_KEY="$ROOT_DIR/keys/nexus-npk.key"
NPK_PUB="$ROOT_DIR/keys/nexus-npk.pub"
setup_musl_toolchain() {
    mkdir -p "$TOOLS_DIR"
    # Compilateur des paquets : musl en statique (binaires autonomes, sans libc
    # à installer). On compile notre propre musl : celle de Debian est amputée
    # de crypt(), indispensable à un serveur SSH.
    MUSL_PREFIX="$TOOLS_DIR/musl-$MUSL_VERSION-sysroot"
    if [ ! -x "$MUSL_PREFIX/bin/musl-gcc" ]; then
        log "Compilation de la libc musl $MUSL_VERSION (chaîne de compilation)"
        tar -xf "$(fetch "https://musl.libc.org/releases/musl-$MUSL_VERSION.tar.gz")" -C "$TOOLS_DIR"
        (
            cd "$TOOLS_DIR/musl-$MUSL_VERSION"
            ./configure --prefix="$MUSL_PREFIX" --disable-shared >/dev/null
            make -j"$JOBS" >/dev/null
            make install >/dev/null
        )
    fi

    # musl n'embarque pas les en-têtes du noyau (linux/, asm/) : on lui donne
    # ceux de Debian.
    KHEADERS="$TOOLS_DIR/kernel-headers"
    if [ ! -d "$KHEADERS" ]; then
        mkdir -p "$KHEADERS"
        ln -s /usr/include/linux "$KHEADERS/linux"
        ln -s /usr/include/asm-generic "$KHEADERS/asm-generic"
        ln -s /usr/include/x86_64-linux-gnu/asm "$KHEADERS/asm"
    fi
    MUSL_CC="$MUSL_PREFIX/bin/musl-gcc -static -idirafter $KHEADERS"
    export MUSL_CC
}
