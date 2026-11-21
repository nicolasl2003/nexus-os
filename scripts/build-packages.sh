#!/bin/sh
# Construit les paquets npk à partir de packages/*/recipe et génère le dépôt
# dans build/repo (INDEX + fichiers .npk).
#
# Usage : build-packages.sh [paquet...]   (sans argument : tous)
#         FORCE=1 build-packages.sh nano  (reconstruit même si déjà fait)
. "$(dirname "$0")/common.sh"

PKG_LIB="$ROOT_DIR/packages/_lib"
WORK="$BUILD_DIR/pkgbuild"
mkdir -p "$WORK" "$REPO_DIR"

setup_musl_toolchain
export PKG_LIB WORK MUSL_CC

for recipe in "$ROOT_DIR"/packages/*/recipe; do
    (
        NAME=""; VERSION=""; REL=""; DEPENDS=""; DESC=""
        RECIPE_DIR="$(dirname "$recipe")"
        . "$recipe"
        # Version du paquet = version amont + révision éventuelle (ex. 2025.88-r1)
        PKGVER="$VERSION${REL:+-r$REL}"

        if [ $# -gt 0 ]; then
            case " $* " in *" $NAME "*) ;; *) exit 0 ;; esac
        fi

        pkgfile="$REPO_DIR/$NAME-$PKGVER.npk"
        if [ -f "$pkgfile" ] && [ -z "${FORCE:-}" ]; then
            log "$NAME $PKGVER déjà construit"
            exit 0
        fi

        log "Construction de $NAME $PKGVER"
        PREFIX=/usr/local
        DESTDIR="$WORK/stage-$NAME"
        rm -rf "$DESTDIR"
        mkdir -p "$DESTDIR$PREFIX"
        build

        pack="$WORK/pack-$NAME"
        rm -rf "$pack"
        mkdir -p "$pack"
        cp -a "$DESTDIR$PREFIX" "$pack/files"
        {
            echo "NAME=$NAME"
            echo "VERSION=$PKGVER"
            echo "DEPENDS=$DEPENDS"
            echo "DESC=$DESC"
        } > "$pack/PKGINFO"
        rm -f "$REPO_DIR/$NAME"-[0-9]*.npk
        tar -czf "$pkgfile" --owner=0 --group=0 -C "$pack" PKGINFO files
        log "$NAME $PKGVER -> $(basename "$pkgfile") ($(du -h "$pkgfile" | cut -f1))"
    )
done

# Régénère l'index à partir des paquets présents
log "Génération de l'index du dépôt"
: > "$REPO_DIR/INDEX.new"
for recipe in "$ROOT_DIR"/packages/*/recipe; do
    (
        NAME=""; VERSION=""; REL=""; DEPENDS=""; DESC=""
        RECIPE_DIR="$(dirname "$recipe")"
        . "$recipe"
        # Version du paquet = version amont + révision éventuelle (ex. 2025.88-r1)
        PKGVER="$VERSION${REL:+-r$REL}"
        pkgfile="$REPO_DIR/$NAME-$PKGVER.npk"
        [ -f "$pkgfile" ] || exit 0
        sum=$(sha256sum "$pkgfile" | cut -d' ' -f1)
        echo "$NAME|$PKGVER|$DEPENDS|$sum|$(basename "$pkgfile")|$DESC" >> "$REPO_DIR/INDEX.new"
    )
done
mv "$REPO_DIR/INDEX.new" "$REPO_DIR/INDEX"

# Signature de l'index. L'INDEX contient le SHA-256 de chaque paquet : le
# signer suffit à garantir l'authenticité de tous les paquets du dépôt.
# Le commentaire de confiance (signé lui aussi) porte l'heure de signature :
# npk refuse un index plus ancien que celui qu'il connaît (anti-rollback).
[ -x "$MINISIGN" ] || { echo "minisign absent : lance 'make tools'" >&2; exit 1; }
[ -f "$NPK_KEY" ]  || { echo "clé de signature absente : lance 'make keys'" >&2; exit 1; }
"$MINISIGN" -S -s "$NPK_KEY" -m "$REPO_DIR/INDEX" -x "$REPO_DIR/INDEX.minisig" \
    -t "nexus-index timestamp:$(date +%s)" >/dev/null
log "Index signé avec keys/nexus-npk.key"
log "Dépôt prêt : $(wc -l < "$REPO_DIR/INDEX") paquet(s) dans build/repo"
