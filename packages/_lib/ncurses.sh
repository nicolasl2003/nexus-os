# Construit ncurses une seule fois (statique, musl) dans $WORK/ncurses-sysroot.
# Partagé par nano (bibliothèque) et ncurses-terminfo (base de terminaux).
NCURSES_VERSION=6.4
NCURSES_SYSROOT="$WORK/ncurses-sysroot"

build_ncurses_sysroot() {
    [ -f "$NCURSES_SYSROOT/usr/local/lib/libncursesw.a" ] && return 0
    log "Compilation de ncurses $NCURSES_VERSION (dépendance de build)"
    tar -xf "$(fetch "https://ftp.gnu.org/gnu/ncurses/ncurses-$NCURSES_VERSION.tar.gz")" -C "$WORK"
    cd "$WORK/ncurses-$NCURSES_VERSION"
    CC="$MUSL_CC" ./configure --prefix=/usr/local \
        --without-shared --without-cxx --without-cxx-binding --without-ada \
        --without-manpages --without-tests --without-debug --without-gpm \
        --enable-widec \
        --with-terminfo-dirs=/usr/local/share/terminfo \
        --with-default-terminfo-dir=/usr/local/share/terminfo >/dev/null
    make -j"$JOBS" >/dev/null
    make install DESTDIR="$NCURSES_SYSROOT" >/dev/null
}
