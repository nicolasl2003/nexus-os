#!/bin/sh
# Télécharge noVNC : client VNC en HTML/JavaScript qui affiche l'écran de
# Nexus OS (QEMU) dans un onglet de navigateur, sans rien installer sur le Mac.
. "$(dirname "$0")/common.sh"

NOVNC_VERSION=1.5.0
if [ -f "$NOVNC_DIR/vnc.html" ]; then
    log "noVNC déjà présent"
    exit 0
fi
log "Installation de noVNC $NOVNC_VERSION"
mkdir -p "$(dirname "$NOVNC_DIR")"
archive="$(fetch "https://github.com/novnc/noVNC/archive/refs/tags/v$NOVNC_VERSION.tar.gz" "novnc-$NOVNC_VERSION.tar.gz")"
rm -rf "$NOVNC_DIR" "$(dirname "$NOVNC_DIR")/noVNC-$NOVNC_VERSION"
tar -xzf "$archive" -C "$(dirname "$NOVNC_DIR")"
mv "$(dirname "$NOVNC_DIR")/noVNC-$NOVNC_VERSION" "$NOVNC_DIR"

# Page d'accueil : se connecte directement à QEMU (websocket sur le port 5700)
cat > "$NOVNC_DIR/index.html" <<'HTML'
<!doctype html>
<meta charset="utf-8">
<title>Nexus OS</title>
<meta http-equiv="refresh" content="0; url=vnc.html?autoconnect=true&amp;resize=scale&amp;reconnect=true&amp;reconnect_delay=1000&amp;port=5700&amp;path=">
<p>Connexion à l'écran de Nexus OS…</p>
HTML
log "noVNC prêt"
