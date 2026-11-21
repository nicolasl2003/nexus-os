#!/bin/sh
# Génère la paire de clés qui signe le dépôt npk (une seule fois).
#   keys/nexus-npk.pub  clé publique : intégrée à Nexus OS, peut être partagée
#   keys/nexus-npk.key  clé SECRÈTE  : ne jamais la publier ni la committer
. "$(dirname "$0")/common.sh"

KEYS_DIR="$ROOT_DIR/keys"
if [ -f "$KEYS_DIR/nexus-npk.key" ]; then
    log "Clé de signature déjà présente : keys/nexus-npk.key (rien à faire)"
    exit 0
fi

mkdir -p "$KEYS_DIR"
log "Génération de la paire de clés de signature (Ed25519)"
# -W : clé secrète sans mot de passe, pour pouvoir signer automatiquement au build
"$MINISIGN" -G -W -p "$KEYS_DIR/nexus-npk.pub" -s "$KEYS_DIR/nexus-npk.key" >/dev/null
chmod 600 "$KEYS_DIR/nexus-npk.key"
log "Clé publique : keys/nexus-npk.pub ($(sed -n 2p "$KEYS_DIR/nexus-npk.pub"))"
log "Clé SECRÈTE  : keys/nexus-npk.key — garde-la précieusement et hors de git"
