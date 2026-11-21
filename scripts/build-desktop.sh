#!/bin/sh
# Compile les programmes du bureau Nexus (dossier desktop/) -> build/gui/nexus-apps
#   nexus-shell  : barre des tâches, menu Démarrer, fond d'écran, Paramètres
#   nexus-power  : arrêt/redémarrage pour les utilisateurs du bureau (setuid)
# À lancer dans l'image Alpine, après 'make gui-stack' : 'make desktop-apps'.
. "$(dirname "$0")/common.sh"

P="$BUILD_DIR/gui/sysroot/usr/local"
OUT="$BUILD_DIR/gui/nexus-apps"
SRC="$ROOT_DIR/desktop"
[ -x "$P/bin/wayland-scanner" ] || { echo "Pile graphique absente : lance 'make gui-stack'" >&2; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/gen"

export PKG_CONFIG_LIBDIR="$P/lib/pkgconfig:$P/share/pkgconfig"
pc() { pkgconf --define-prefix --static "$@"; }

log "Génération du code des protocoles Wayland"
for xml in "$SRC"/protocols/*.xml; do
    n=$(basename "$xml" .xml)
    "$P/bin/wayland-scanner" client-header "$xml" "$OUT/gen/$n-client-protocol.h"
    "$P/bin/wayland-scanner" private-code "$xml" "$OUT/gen/$n-protocol.c"
done

DEPS="cairo pangocairo wayland-client wayland-cursor xkbcommon alsa libjpeg"
log "Compilation de nexus-shell"
# shellcheck disable=SC2046
gcc -static -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -I"$OUT/gen" -I"$SRC/common" $(pc --cflags $DEPS) \
    -o "$OUT/bin/nexus-shell" \
    "$SRC"/common/*.c "$SRC"/shell/*.c "$OUT"/gen/*.c \
    $(pc --libs $DEPS) -lm
strip "$OUT/bin/nexus-shell"

log "Compilation de nexus-power"
gcc -static -O2 -Wall -Wextra -o "$OUT/bin/nexus-power" "$SRC/power/nexus-power.c"
strip "$OUT/bin/nexus-power"
log "Compilation de nexus-pkg (aide du Magasin d'applications)"
gcc -static -O2 -Wall -Wextra -o "$OUT/bin/nexus-pkg" "$SRC/store/nexus-pkg.c"
strip "$OUT/bin/nexus-pkg"
log "Compilation de apk-split (paquets Alpine)"
gcc -static -O2 -Wall -Wextra -I"$P/include" -o "$OUT/bin/apk-split" "$SRC/alpine/apk-split.c" -L"$P/lib" -lz
strip "$OUT/bin/apk-split"
log "Compilation de nexus-account (gestion des comptes)"
gcc -static -O2 -Wall -Wextra -o "$OUT/bin/nexus-account" "$SRC/store/nexus-account.c"
strip "$OUT/bin/nexus-account"
log "Compilation de nexus-login"
gcc -static -O2 -Wall -Wextra -o "$OUT/bin/nexus-login" "$SRC/login/nexus-login.c"
strip "$OUT/bin/nexus-login"

log "Programmes du bureau prêts : nexus-shell ($(du -h "$OUT/bin/nexus-shell" | cut -f1)), nexus-power"
