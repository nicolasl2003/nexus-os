# Accès à la pile graphique compilée par 'make gui-stack' (image Alpine).
GUI_P="$BUILD_DIR/gui/sysroot/usr/local"

gui_need() {
    [ -e "$GUI_P/$1" ] || { echo "Manque $1 : lance d'abord 'make gui-stack'" >&2; exit 1; }
}
# gui_copy <chemin relatif à /usr/local>... : copie depuis la pile graphique
gui_copy() {
    for f in "$@"; do
        gui_need "$f"
        mkdir -p "$DESTDIR$PREFIX/$(dirname "$f")"
        cp -a "$GUI_P/$f" "$DESTDIR$PREFIX/$f"
    done
}
# gui_bin <programme>... : copie un programme et le réduit (strip)
gui_bin() {
    for b in "$@"; do
        gui_copy "bin/$b"
        strip "$DESTDIR$PREFIX/bin/$b"
    done
}
