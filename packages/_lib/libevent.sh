# Construit libevent une seule fois (statique, musl) dans $WORK/libevent-sysroot.
# Bibliothèque de boucle d'événements, utilisée par tmux.
LIBEVENT_VERSION=2.1.12-stable
LIBEVENT_SYSROOT="$WORK/libevent-sysroot"

build_libevent_sysroot() {
    [ -f "$LIBEVENT_SYSROOT/usr/local/lib/libevent_core.a" ] && return 0
    log "Compilation de libevent $LIBEVENT_VERSION (dépendance de build)"
    tar -xf "$(fetch "https://github.com/libevent/libevent/releases/download/release-$LIBEVENT_VERSION/libevent-$LIBEVENT_VERSION.tar.gz")" -C "$WORK"
    cd "$WORK/libevent-$LIBEVENT_VERSION"
    CC="$MUSL_CC" ./configure --prefix=/usr/local \
        --disable-shared --enable-static --disable-openssl \
        --disable-samples --disable-libevent-regress --disable-debug-mode >/dev/null
    make -j"$JOBS" >/dev/null
    make install DESTDIR="$LIBEVENT_SYSROOT" >/dev/null
}
