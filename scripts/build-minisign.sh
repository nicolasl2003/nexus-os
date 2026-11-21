#!/bin/sh
# Compile minisign (signatures Ed25519) en statique -> build/tools/bin/minisign
# Le même binaire sert à signer le dépôt (dans Docker) et à vérifier les
# signatures dans Nexus OS (il fait partie du système de base).
. "$(dirname "$0")/common.sh"

MINISIGN_BIN="$TOOLS_DIR/bin/minisign"
if [ -x "$MINISIGN_BIN" ]; then
    log "minisign $MINISIGN_VERSION déjà compilé"
    exit 0
fi

setup_musl_toolchain

# libsodium : la bibliothèque cryptographique utilisée par minisign
SODIUM="$TOOLS_DIR/libsodium-sysroot"
if [ ! -f "$SODIUM/lib/libsodium.a" ]; then
    log "Compilation de libsodium $LIBSODIUM_VERSION"
    rm -rf "$TOOLS_DIR/libsodium-$LIBSODIUM_VERSION"
    tar -xf "$(fetch "https://download.libsodium.org/libsodium/releases/libsodium-$LIBSODIUM_VERSION.tar.gz")" -C "$TOOLS_DIR"
    (
        cd "$TOOLS_DIR/libsodium-$LIBSODIUM_VERSION"
        CC="$MUSL_CC" ./configure --prefix="$SODIUM" --disable-shared --enable-static >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
    )
fi

log "Compilation de minisign $MINISIGN_VERSION"
rm -rf "$TOOLS_DIR/minisign-$MINISIGN_VERSION"
tar -xf "$(fetch "https://github.com/jedisct1/minisign/archive/refs/tags/$MINISIGN_VERSION.tar.gz" "minisign-$MINISIGN_VERSION.tar.gz")" -C "$TOOLS_DIR"
cd "$TOOLS_DIR/minisign-$MINISIGN_VERSION/src"
mkdir -p "$TOOLS_DIR/bin"
# shellcheck disable=SC2086
$MUSL_CC -O2 -I"$SODIUM/include" -o "$MINISIGN_BIN" \
    base64.c get_line.c helpers.c minisign.c -L"$SODIUM/lib" -lsodium
strip "$MINISIGN_BIN"
log "minisign prêt : build/tools/bin/minisign ($(du -h "$MINISIGN_BIN" | cut -f1))"
