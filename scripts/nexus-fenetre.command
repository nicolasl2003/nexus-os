#!/bin/sh
# Double-clic dans le Finder (ou 'open scripts/nexus-fenetre.command') :
# lance Nexus OS en mode natif accéléré, dans une fenêtre macOS, depuis
# l'application Terminal (macOS y donne bien le clavier et la souris à QEMU).
cd "$(dirname "$0")/.." || exit 1
NATIVE_DISPLAY=window ./scripts/run-native.sh
