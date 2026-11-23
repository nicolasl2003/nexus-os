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
# keys/nexus-npk.pub (dans git) est la clé officielle, qui signe le dépôt en
# ligne : on ne l'écrase pas. Sur une machine sans la clé secrète officielle,
# on génère une clé locale (keys/local-npk.pub, hors de git), intégrée à côté
# de l'officielle : le dépôt local ET le dépôt en ligne restent acceptés.
PUB="$KEYS_DIR/nexus-npk.pub"
[ -f "$PUB" ] && PUB="$KEYS_DIR/local-npk.pub"
log "Génération de la paire de clés de signature (Ed25519)"
# -W : clé secrète sans mot de passe, pour pouvoir signer automatiquement au build
"$MINISIGN" -G -W -f -p "$PUB" -s "$KEYS_DIR/nexus-npk.key" >/dev/null
chmod 600 "$KEYS_DIR/nexus-npk.key"
log "Clé publique : keys/$(basename "$PUB") ($(sed -n 2p "$PUB"))"
log "Clé SECRÈTE  : keys/nexus-npk.key — garde-la précieusement et hors de git"
