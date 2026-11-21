#!/bin/sh
# Lance Nexus OS directement sur le Mac (hors Docker), avec l'accélération
# matérielle de macOS (Hypervisor.framework, "HVF") : vitesse quasi native,
# au lieu de l'émulation logicielle de QEMU dans Docker.
#
# L'écran s'ouvre dans une fenêtre du Mac ; ce terminal reste la console série
# (Ctrl+A puis X pour quitter QEMU). Prérequis : QEMU sur le Mac (brew install
# qemu, ou ./scripts/build-qemu-mac.sh) et les fichiers de dist/ ('make dist').
#
# Options : NO_DISK=1 (mode live, sans disque), NATIVE_MEM=2G (mémoire),
#           NATIVE_FULLSCREEN=0 (fenêtre au lieu du plein écran),
#           NATIVE_DISPLAY=window (fenêtre macOS au lieu du navigateur)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# QEMU : celui compilé par scripts/build-qemu-mac.sh (tools/qemu), sinon celui du système
QEMU="$ROOT/tools/qemu/bin/qemu-system-x86_64"
[ -x "$QEMU" ] || QEMU="$(command -v qemu-system-x86_64 || true)"
if [ -z "$QEMU" ]; then
    echo "QEMU n'est pas installé sur le Mac : lance ./scripts/build-qemu-mac.sh (ou brew install qemu)" >&2
    exit 1
fi
for f in bzImage initramfs.cpio.gz; do
    [ -f "dist/$f" ] || { echo "dist/$f manquant : lance d'abord 'make dist'" >&2; exit 1; }
done

# Accélération : HVF si macOS la propose, sinon émulation multi-cœur
if [ "$(sysctl -n kern.hv_support 2>/dev/null || echo 0)" = 1 ]; then
    ACCEL="-accel hvf -cpu host"
    echo "[nexus] Accélération matérielle HVF activée"
else
    ACCEL="-accel tcg,thread=multi"
    echo "[nexus] Pas d'accélération matérielle : émulation logicielle"
fi

# Dépôt de paquets npk, servi au Mac pour QEMU (10.0.2.2:8080 dans Nexus OS)
REPO_PID=""
if [ -f dist/repo/INDEX ]; then
    python3 -m http.server 8080 --bind 127.0.0.1 --directory dist/repo >/dev/null 2>&1 &
    REPO_PID=$!
    echo "[nexus] Dépôt npk servi sur http://10.0.2.2:8080"
fi
cleanup() {
    [ -n "${REPO_PID:-}" ] && kill "$REPO_PID" 2>/dev/null
    [ -n "${NOVNC_PID:-}" ] && kill "$NOVNC_PID" 2>/dev/null
    true
}
trap cleanup EXIT INT TERM

DISK=""
if [ -f disk/nexus-data.img ] && [ "${NO_DISK:-0}" != 1 ]; then
    DISK="-drive file=disk/nexus-data.img,if=virtio,format=raw"
fi

# Affichage :
#  - browser (défaut) : écran dans le navigateur (noVNC, http://localhost:6080),
#    le mode le plus fiable pour la souris et le clavier ;
#  - window : fenêtre macOS de QEMU (peut ne pas recevoir le clavier quand QEMU
#    est lancé depuis certaines applications).
NOVNC_PID=""
GPU="virtio-vga"
if [ "${NATIVE_DISPLAY:-browser}" = window ]; then
    if [ "${NATIVE_FULLSCREEN:-1}" = 1 ]; then
        # Plein écran à la résolution exacte de l'écran du Mac (taille logique,
        # ex. 1440x900 sur un écran Retina) : image nette, sans déformation.
        RES=$(osascript -l JavaScript -e 'ObjC.import("AppKit"); var f=$.NSScreen.mainScreen.frame; Math.round(f.size.width)+"x"+Math.round(f.size.height)' 2>/dev/null || true)
        case "$RES" in
            [0-9]*x[0-9]*) GPU="virtio-vga,xres=${RES%x*},yres=${RES#*x}" ;;
            *) RES="" ;;
        esac
        DISPLAY_ARGS="-display cocoa,show-cursor=on,full-screen=on,zoom-to-fit=on"
        echo "[nexus] Nexus OS s'ouvre en plein écran ${RES:+($RES)}."
        echo "[nexus] Cmd+F : quitter/revenir au plein écran   Cmd+Q : fermer Nexus OS"
    else
        DISPLAY_ARGS="-display cocoa,show-cursor=on"
        echo "[nexus] L'écran de Nexus OS s'ouvre dans une fenêtre. Ctrl+A puis X ici pour quitter."
    fi
else
    [ -f dist/novnc/vnc.html ] || { echo "dist/novnc manquant : lance 'make novnc dist'" >&2; exit 1; }
    python3 -m http.server 6080 --bind 127.0.0.1 --directory dist/novnc >/dev/null 2>&1 &
    NOVNC_PID=$!
    DISPLAY_ARGS="-display none -vnc 127.0.0.1:0,websocket=127.0.0.1:5700"
    echo "[nexus] Écran de Nexus OS : http://localhost:6080 (ouverture dans le navigateur)"
    echo "[nexus] Ce terminal est la console série. Ctrl+A puis X pour quitter."
    ( sleep 2; open "http://localhost:6080" ) &
fi
# shellcheck disable=SC2086
"$QEMU" \
    $ACCEL -smp 4 -m "${NATIVE_MEM:-2G}" \
    $DISK \
    -kernel dist/bzImage \
    -initrd dist/initramfs.cpio.gz \
    -append "console=tty0 console=ttyS0 quiet loglevel=5 fbcon=logo-count:1" \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=net0,romfile= \
    -device "$GPU" -device virtio-keyboard-pci -device virtio-tablet-pci \
    -audiodev coreaudio,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
    $DISPLAY_ARGS \
    -serial mon:stdio \
    -name "Nexus OS" \
    -no-reboot
