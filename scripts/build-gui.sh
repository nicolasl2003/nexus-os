#!/bin/sh
# Construit la pile graphique de Nexus OS (Wayland) depuis les sources.
# À lancer dans l'image Alpine (Dockerfile.gui) : 'make gui-stack'.
#
#   build/gui/sysroot/usr/local   bibliothèques statiques + en-têtes + données
#   build/gui/build/<composant>   dossiers de compilation
#
# Chaque composant réussi laisse un fichier .done : relancer le script
# reprend là où il s'était arrêté. 'build-gui.sh <composant>' force la
# reconstruction d'un composant.
. "$(dirname "$0")/common.sh"

GUI_DIR="$BUILD_DIR/gui"
SYSROOT="$GUI_DIR/sysroot"
BLD="$GUI_DIR/build"
P="$SYSROOT/usr/local"
mkdir -p "$BLD" "$P"

# --- Environnement : tout pointe vers notre sysroot -------------------------
# pkg-config ne voit que nos bibliothèques (+ hwdata du système) ;
# --define-prefix recalcule les chemins depuis l'emplacement des .pc ;
# --static ajoute les dépendances privées (indispensable en statique).
cat > "$GUI_DIR/pkg-config" <<'EOF'
#!/bin/sh
exec pkgconf --define-prefix --static "$@"
EOF
chmod +x "$GUI_DIR/pkg-config"
export PKG_CONFIG="$GUI_DIR/pkg-config"
export PKG_CONFIG_LIBDIR="$P/lib/pkgconfig:$P/share/pkgconfig:/usr/share/pkgconfig"
export CFLAGS="-O2 -fPIC -I$P/include"
export CXXFLAGS="$CFLAGS"
export CPPFLAGS="-I$P/include"
export LDFLAGS="-L$P/lib"
export PATH="$P/bin:$PATH"     # wayland-scanner, glib-mkenums...

# --- Outils génériques -------------------------------------------------------
done_file() { echo "$BLD/$1.done"; }
is_done()   { [ -f "$(done_file "$1")" ] && [ "${FORCE_ONE:-}" != "$1" ]; }

# unpack <nom> <url> [nom-archive] -> se place dans les sources fraîches
unpack() {
    archive="$(fetch "$2" "${3:-}")"
    rm -rf "$BLD/$1-src"; mkdir -p "$BLD/$1-src"
    tar -xf "$archive" -C "$BLD/$1-src" --strip-components=1
    cd "$BLD/$1-src"
}

# meson_build <nom> [options...]   (dans le dossier des sources)
meson_build() {
    name="$1"; shift
    rm -rf "$BLD/$name-meson"   # configuration toujours fraîche
    meson setup "$BLD/$name-meson" . --prefix=/usr/local --libdir=lib \
        --buildtype=release --default-library=static --wrap-mode=nodownload \
        -Db_ndebug=true -Dwerror=false -Dprefer_static=true "$@" >"$BLD/$name.log" 2>&1 || { tail -40 "$BLD/$name.log"; exit 1; }
    ninja -C "$BLD/$name-meson" >>"$BLD/$name.log" 2>&1 || { grep -E 'error|Error' "$BLD/$name.log" | head -30; exit 1; }
    DESTDIR="$SYSROOT" ninja -C "$BLD/$name-meson" install >>"$BLD/$name.log" 2>&1
}

# autotools_build <nom> [options configure...]
autotools_build() {
    name="$1"; shift
    ./configure --prefix=/usr/local --disable-shared --enable-static "$@" \
        >"$BLD/$name.log" 2>&1 || { tail -30 "$BLD/$name.log"; exit 1; }
    make -j"$JOBS" >>"$BLD/$name.log" 2>&1 || { grep -E 'error' "$BLD/$name.log" | head -30; exit 1; }
    make install DESTDIR="$SYSROOT" >>"$BLD/$name.log" 2>&1
    rm -f "$P"/lib/*.la   # les .la contiennent des chemins faux pour la suite
}

# step <nom> <fonction> : exécute une étape si elle n'est pas déjà faite
step() {
    if is_done "$1"; then return 0; fi
    log "Pile graphique : $1"
    ( "$2" )
    touch "$(done_file "$1")"
}

# --- Les composants, dans l'ordre des dépendances -----------------------------

b_zlib() {
    unpack zlib https://zlib.net/fossils/zlib-1.3.1.tar.gz
    ./configure --prefix=/usr/local --static >"$BLD/zlib.log" 2>&1
    make -j"$JOBS" >>"$BLD/zlib.log" 2>&1 && make install DESTDIR="$SYSROOT" >>"$BLD/zlib.log" 2>&1
}
b_libpng() {
    unpack libpng https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz
    autotools_build libpng
}
b_expat() {
    unpack expat https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.xz
    autotools_build expat --without-docbook --without-examples --without-tests
}
b_freetype() {
    unpack freetype https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
    autotools_build freetype --without-harfbuzz --without-brotli --without-bzip2 --with-png=yes --with-zlib=yes
}
b_fontconfig() {
    unpack fontconfig https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.xz
    meson_build fontconfig -Ddoc=disabled -Dtests=disabled -Dtools=disabled \
        -Dcache-build=disabled -Dnls=disabled
}
b_pixman() {
    unpack pixman https://cairographics.org/releases/pixman-0.44.2.tar.gz
    meson_build pixman -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled
}
b_libffi() {
    unpack libffi https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz
    autotools_build libffi --disable-docs
}
b_pcre2() {
    unpack pcre2 https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.bz2
    autotools_build pcre2 --enable-pcre2-8 --disable-pcre2-16 --disable-pcre2-32
}
b_glib() {
    unpack glib https://download.gnome.org/sources/glib/2.82/glib-2.82.4.tar.xz
    meson_build glib -Dintrospection=disabled -Dtests=false -Dnls=disabled \
        -Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dman-pages=disabled \
        -Ddocumentation=false -Dsysprof=disabled -Dglib_debug=disabled
}
b_cairo() {
    unpack cairo https://cairographics.org/releases/cairo-1.18.2.tar.xz
    meson_build cairo -Dxlib=disabled -Dxcb=disabled -Dquartz=disabled -Dtee=disabled \
        -Ddwrite=disabled -Dtests=disabled -Dgtk_doc=false -Dspectre=disabled \
        -Dsymbol-lookup=disabled -Dpng=enabled -Dfreetype=enabled -Dfontconfig=enabled -Dglib=enabled
}
b_fribidi() {
    unpack fribidi https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz
    meson_build fribidi -Ddocs=false -Dtests=false -Dbin=false
}
b_harfbuzz() {
    unpack harfbuzz https://github.com/harfbuzz/harfbuzz/releases/download/10.1.0/harfbuzz-10.1.0.tar.xz
    meson_build harfbuzz -Dtests=disabled -Ddocs=disabled -Dutilities=disabled \
        -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled -Dcairo=disabled \
        -Dicu=disabled -Dintrospection=disabled
}
b_pango() {
    unpack pango https://download.gnome.org/sources/pango/1.54/pango-1.54.0.tar.xz
    meson_build pango -Dintrospection=disabled -Ddocumentation=false \
        -Dbuild-testsuite=false -Dbuild-examples=false -Dsysprof=disabled \
        -Dlibthai=disabled -Dxft=disabled -Dcairo=enabled -Dfreetype=enabled -Dfontconfig=enabled
}
b_libxml2() {
    unpack libxml2 https://download.gnome.org/sources/libxml2/2.13/libxml2-2.13.5.tar.xz
    autotools_build libxml2 --without-python --without-lzma --without-icu --without-zlib
}

# --- Wayland et entrées ---------------------------------------------------------
STATIC_EXE="-Dc_link_args=-static -Dcpp_link_args=-static"   # programmes finaux autonomes

b_wayland() {
    unpack wayland https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.23.1/downloads/wayland-1.23.1.tar.xz
    meson_build wayland -Ddocumentation=false -Dtests=false -Ddtd_validation=false
}
b_wayland_protocols() {
    unpack wayland-protocols https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/1.38/downloads/wayland-protocols-1.38.tar.xz
    meson_build wayland-protocols -Dtests=false
}
b_libxkbcommon() {
    unpack libxkbcommon https://xkbcommon.org/download/libxkbcommon-1.7.0.tar.xz
    # Disposition clavier par défaut : français (AZERTY)
    meson_build libxkbcommon -Denable-tools=false -Denable-x11=false -Denable-docs=false \
        -Denable-wayland=false -Denable-xkbregistry=false -Denable-bash-completion=false \
        -Dxkb-config-root=/usr/local/share/X11/xkb -Dx-locale-root=/usr/local/share/X11/locale \
        -Ddefault-layout=fr
}
b_xkeyboard_config() {
    unpack xkeyboard-config https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-2.43.tar.xz
    meson_build xkeyboard-config -Dxorg-rules-symlinks=false -Dnls=false
}
b_libdrm() {
    unpack libdrm https://dri.freedesktop.org/libdrm/libdrm-2.4.123.tar.xz
    meson_build libdrm -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
        -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled \
        -Dvc4=disabled -Detnaviv=disabled -Dcairo-tests=disabled -Dman-pages=disabled \
        -Dvalgrind=disabled -Dtests=false -Dudev=false -Dinstall-test-programs=false
}
b_mtdev() {
    unpack mtdev https://bitmath.org/code/mtdev/mtdev-1.1.7.tar.bz2
    autotools_build mtdev
}
b_libevdev() {
    unpack libevdev https://www.freedesktop.org/software/libevdev/libevdev-1.13.3.tar.xz
    meson_build libevdev -Dtests=disabled -Dtools=disabled -Ddocumentation=disabled
}
b_libudev_zero() {
    # Remplace libudev de systemd : Nexus OS n'a pas de démon udev
    unpack libudev-zero https://github.com/illiliti/libudev-zero/archive/refs/tags/1.0.3.tar.gz libudev-zero-1.0.3.tar.gz
    # Correctif : un périphérique avec axes absolus ET molette relative (la
    # tablette de QEMU, beaucoup de tablettes USB) n'était classé ni souris ni
    # tablette, donc ignoré par libinput. On teste les axes absolus même si
    # le périphérique a aussi des axes relatifs.
    sed -i 's/^    else if (test_bit(ev_bits, EV_ABS)) {/    if (test_bit(ev_bits, EV_ABS)) {/' udev_device.c
    grep -q '^    if (test_bit(ev_bits, EV_ABS)) {' udev_device.c || { echo "correctif libudev-zero non appliqué" >&2; exit 1; }
    # Correctif 2 (comme le vrai udev de systemd) : un périphérique absolu qui a
    # des boutons de souris est une souris, même s'il déclare aussi BTN_TOUCH
    # (tablette de QEMU >= 8, tablettes USB). Sinon il serait pris pour un
    # écran tactile et la souris ne bougerait pas.
    python3 - <<'PY'
import re
src = open("udev_device.c").read()
anchor = "            else if (test_bit(key_bits, BTN_TOUCH)) {"
fix = ("            else if (test_bit(key_bits, BTN_MOUSE) && !test_bit(key_bits, BTN_TOOL_FINGER)) {\n"
       "                udev_list_entry_add(&udev_device->properties, \"ID_INPUT_MOUSE\", \"1\", 0);\n"
       "            }\n")
assert anchor in src, "ancre introuvable"
open("udev_device.c", "w").write(src.replace(anchor, fix + anchor, 1))
PY
    make -j"$JOBS" PREFIX=/usr/local libudev.a libudev.pc >"$BLD/libudev-zero.log" 2>&1
    make PREFIX=/usr/local DESTDIR="$SYSROOT" install-static install-headers install-pkgconfig >>"$BLD/libudev-zero.log" 2>&1
}
b_libinput() {
    unpack libinput https://gitlab.freedesktop.org/libinput/libinput/-/archive/1.27.0/libinput-1.27.0.tar.gz
    # libinput impose une bibliothèque partagée : on la rend statique
    sed -i "s/lib_libinput = shared_library('input',/lib_libinput = library('input',/" meson.build
    rm -f "$P"/lib/libinput.so*
    meson_build libinput -Dlibwacom=false -Ddebug-gui=false -Dtests=false -Ddocumentation=false \
        -Dinstall-tests=false -Dudev-dir=/usr/local/lib/udev -Dzshcompletiondir=no
}
b_seatd() {
    # Gestion des "sièges" : donne au bureau l'accès à l'écran et au clavier.
    # Mode "builtin" désactivé : il embarque un server_init() qui entre en
    # collision avec celui de labwc en statique. Le démon seatd sera un service.
    unpack seatd https://git.sr.ht/~kennylevinsen/seatd/archive/0.9.1.tar.gz seatd-0.9.1.tar.gz
    meson_build seatd -Dlibseat-logind=disabled -Dlibseat-seatd=enabled -Dlibseat-builtin=disabled \
        -Dserver=enabled -Dexamples=disabled -Dman-pages=disabled -Ddefaultpath=/run/seatd.sock $STATIC_EXE
}
b_libdisplay_info() {
    unpack libdisplay-info https://gitlab.freedesktop.org/emersion/libdisplay-info/-/releases/0.2.0/downloads/libdisplay-info-0.2.0.tar.xz
    meson_build libdisplay-info
}
b_wlroots() {
    # Bibliothèque de compositeur Wayland. Rendu logiciel (pixman) uniquement :
    # pas besoin de Mesa ni d'accélération 3D.
    unpack wlroots https://gitlab.freedesktop.org/wlroots/wlroots/-/releases/0.18.2/downloads/wlroots-0.18.2.tar.gz
    meson_build wlroots -Drenderers=[] -Dallocators=[] -Dbackends=drm,libinput \
        -Dsession=enabled -Dxwayland=disabled -Dexamples=false -Dcolor-management=disabled \
        -Dlibliftoff=disabled -Dxcb-errors=disabled
}

# --- Le bureau : compositeur, terminal, polices ---------------------------------
b_labwc() {
    unpack labwc https://github.com/labwc/labwc/archive/refs/tags/0.8.2.tar.gz labwc-0.8.2.tar.gz
    # Nexus : aimantation dans les coins, bordure de redimensionnement plus large
    python3 "$ROOT_DIR/desktop/labwc/patch-labwc.py" .
    meson_build labwc -Dman-pages=disabled -Dxwayland=disabled -Dsvg=disabled -Dicon=disabled \
        -Dnls=disabled -Dtest=disabled $STATIC_EXE
}
b_tllist() {
    unpack tllist https://codeberg.org/dnkl/tllist/archive/1.1.0.tar.gz tllist-1.1.0.tar.gz
    meson_build tllist
}
b_fcft() {
    unpack fcft https://codeberg.org/dnkl/fcft/archive/3.1.9.tar.gz fcft-3.1.9.tar.gz
    meson_build fcft -Dsvg-backend=none -Dtest-text-shaping=false -Dexamples=false
}
b_foot() {
    unpack foot https://codeberg.org/dnkl/foot/archive/1.20.2.tar.gz foot-1.20.2.tar.gz
    meson_build foot -Ddocs=disabled -Dthemes=true -Dime=true -Dgrapheme-clustering=disabled \
        -Dtests=false -Dterminfo=disabled -Ddefault-terminfo=xterm-256color \
        -Dutmp-backend=none $STATIC_EXE
}
b_xcompose() {
    # Table "Compose" de X.org : touches mortes (^ + e = ê) et caractères
    # composés. On ne prend que ce fichier de données dans les sources de libX11.
    unpack libx11 https://www.x.org/releases/individual/lib/libX11-1.8.10.tar.xz
    mkdir -p "$P/share/X11/locale/en_US.UTF-8"
    # Comme le build de libX11 : préprocesseur C (retire les commentaires /* */),
    # puis XCOMM -> # (commentaires du format Compose)
    cpp -undef -traditional -P nls/en_US.UTF-8/Compose.pre | sed -e 's/^XCOMM/#/' \
        > "$P/share/X11/locale/en_US.UTF-8/Compose"
}
# --- Son et Wi-Fi (outils système, compilés ici car l'image Alpine est pratique) ---
b_alsa_lib() {
    unpack alsa-lib https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.13.tar.bz2
    autotools_build alsa-lib --disable-python --disable-alisp --disable-old-symbols \
        --disable-topology --with-configdir=/usr/local/share/alsa
}
b_alsa_utils() {
    # Seulement aplay (jouer un son) et amixer (volume en ligne de commande)
    unpack alsa-utils https://www.alsa-project.org/files/pub/utils/alsa-utils-1.2.13.tar.bz2
    ./configure --prefix=/usr/local --disable-alsamixer --disable-alsaconf --disable-nls \
        --disable-xmlto --disable-rst2man --disable-bat --disable-alsatest \
        >"$BLD/alsa-utils.log" 2>&1 || { tail -20 "$BLD/alsa-utils.log"; exit 1; }
    for d in aplay amixer; do
        make -C $d LDFLAGS="-L$P/lib -all-static" >>"$BLD/alsa-utils.log" 2>&1 || { tail -20 "$BLD/alsa-utils.log"; exit 1; }
        install -m 755 $d/$d "$P/bin/$d"
    done
}
b_libnl() {
    unpack libnl https://github.com/thom311/libnl/releases/download/libnl3_11_0/libnl-3.11.0.tar.gz
    autotools_build libnl --disable-cli --disable-debug
}
wifi_config() {
    cat > .config <<'CFG'
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_CTRL_IFACE=y
CONFIG_TLS=internal
CONFIG_CRYPTO=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_INTERNAL_LIBTOMMATH_FAST=y
CONFIG_IEEE80211N=y
CONFIG_IEEE80211AC=y
CONFIG_IEEE80211W=y
CONFIG_NO_CONFIG_BLOBS=y
CFG
}
b_wpa_supplicant() {
    # Client Wi-Fi (WPA/WPA2), cryptographie interne : pas besoin d'OpenSSL
    unpack wpa_supplicant https://w1.fi/releases/wpa_supplicant-2.11.tar.gz
    cd wpa_supplicant
    wifi_config
    echo "CONFIG_BACKEND=file" >> .config
    make -j"$JOBS" CC=gcc EXTRA_CFLAGS="-I$P/include/libnl3" LDFLAGS="-static -L$P/lib" \
        wpa_supplicant wpa_cli wpa_passphrase >"$BLD/wpa_supplicant.log" 2>&1 || { grep -E "error" "$BLD/wpa_supplicant.log" | head; exit 1; }
    install -m 755 wpa_supplicant wpa_cli wpa_passphrase "$P/bin/"
}
b_hostapd() {
    # Point d'accès Wi-Fi : sert aux tests (radios simulées mac80211_hwsim)
    unpack hostapd https://w1.fi/releases/hostapd-2.11.tar.gz
    cd hostapd
    wifi_config
    make -j"$JOBS" CC=gcc EXTRA_CFLAGS="-I$P/include/libnl3" LDFLAGS="-static -L$P/lib" \
        hostapd hostapd_cli >"$BLD/hostapd.log" 2>&1 || { grep -E "error" "$BLD/hostapd.log" | head; exit 1; }
    install -m 755 hostapd hostapd_cli "$P/bin/"
}
b_regdb() {
    # Base réglementaire Wi-Fi (canaux et puissances autorisés par pays)
    unpack regdb https://mirrors.edge.kernel.org/pub/software/network/wireless-regdb/wireless-regdb-2024.10.07.tar.xz
    mkdir -p "$P/lib/firmware"
    cp regulatory.db regulatory.db.p7s "$P/lib/firmware/"
}

b_fonts() {
    # Polices DejaVu (Sans pour l'interface, Sans Mono pour le terminal)
    unpack fonts https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2
    mkdir -p "$P/share/fonts/dejavu"
    cp ttf/DejaVuSans.ttf ttf/DejaVuSans-Bold.ttf ttf/DejaVuSansMono.ttf ttf/DejaVuSansMono-Bold.ttf \
       "$P/share/fonts/dejavu/"
}

# --- Navigateur web : NetSurf -------------------------------------------------

b_openssl() {
    # TLS pour HTTPS (NetSurf affiche aussi les certificats des sites)
    unpack openssl https://github.com/openssl/openssl/releases/download/openssl-3.4.1/openssl-3.4.1.tar.gz
    # -static : l'outil « openssl » est aussi autonome (vérification des paquets Alpine)
    ./Configure linux-x86_64 no-shared no-tests no-docs -static --prefix=/usr/local --libdir=lib \
        --openssldir=/usr/local/etc/ssl >"$BLD/openssl.log" 2>&1 || { tail -20 "$BLD/openssl.log"; exit 1; }
    make -j"$JOBS" build_sw >>"$BLD/openssl.log" 2>&1 || { grep -E "error" "$BLD/openssl.log" | head; exit 1; }
    make install_sw DESTDIR="$SYSROOT" >>"$BLD/openssl.log" 2>&1
}
b_alpine_keys() {
    # Clés publiques d'Alpine Linux (celles de l'image Alpine de construction) :
    # npk alpine vérifie avec elles la signature de chaque paquet Alpine
    mkdir -p "$P/share/npk/alpine-keys"
    cp /etc/apk/keys/*.rsa.pub "$P/share/npk/alpine-keys/"
}
b_curl() {
    # libcurl : téléchargements HTTP/HTTPS du navigateur
    unpack curl https://curl.se/download/curl-8.17.0.tar.xz
    autotools_build curl --with-openssl="$P" --with-zlib="$P" \
        --with-ca-bundle=/usr/local/etc/ssl/certs/ca-certificates.crt \
        --without-libpsl --without-brotli --without-zstd --without-libidn2 \
        --without-nghttp2 --without-libssh2 --disable-ldap --disable-ldaps \
        --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
        --disable-imap --disable-smtp --disable-gopher --disable-mqtt \
        --disable-manual --disable-docs
}
b_libjpeg() {
    unpack libjpeg https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.4/libjpeg-turbo-3.0.4.tar.gz
    cmake -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DWITH_TURBOJPEG=OFF -DWITH_SIMD=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON >"$BLD/libjpeg.log" 2>&1 || { tail -20 "$BLD/libjpeg.log"; exit 1; }
    ninja -C build >>"$BLD/libjpeg.log" 2>&1 || { grep -E "error" "$BLD/libjpeg.log" | head; exit 1; }
    DESTDIR="$SYSROOT" ninja -C build install >>"$BLD/libjpeg.log" 2>&1
}
b_netsurf() {
    # NetSurf (moteur HTML/CSS + JavaScript Duktape), frontend « framebuffer »
    # affiché dans une fenêtre Wayland grâce à notre sortie nexus-wl.c
    unpack netsurf https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz
    s=libnsfb/src/surface
    cp "$ROOT_DIR/desktop/browser/nexus-wl.c" "$s/"
    for n in xdg-shell xdg-decoration-unstable-v1; do
        "$P/bin/wayland-scanner" client-header "$ROOT_DIR/desktop/protocols/$n.xml" "$s/$n-client-protocol.h"
        "$P/bin/wayland-scanner" private-code "$ROOT_DIR/desktop/protocols/$n.xml" "$s/$n-protocol.c"
    done
    python3 "$ROOT_DIR/desktop/browser/patch-netsurf.py" .
    # iconv fait partie de musl : bibliothèque vide pour satisfaire « -liconv »
    [ -f "$P/lib/libiconv.a" ] || ar rcs "$P/lib/libiconv.a"
    fonts=/usr/local/share/fonts/dejavu:/usr/local/share/netsurf/fonts
    cfg="NETSURF_FB_FONTLIB=freetype NETSURF_FB_FONTPATH=$fonts
         NETSURF_FB_FONT_CURSIVE=DejaVuSans.ttf NETSURF_FB_FONT_FANTASY=DejaVuSans.ttf
         NETSURF_USE_WEBP=NO NETSURF_USE_JPEGXL=NO NETSURF_USE_ROSPRITE=NO
         NETSURF_USE_LIBICONV_PLUG=NO NETSURF_STRIP_BINARY=YES"
    # Le Makefile de NetSurf appelle « pkg-config » : notre enveloppe --static
    # passe devant ; LDFLAGS dans l'environnement (et non en argument) pour
    # que les Makefile puissent encore y ajouter leurs bibliothèques.
    export PATH="$GUI_DIR:$PATH" LDFLAGS="-static -L$P/lib"
    make -j"$JOBS" TARGET=framebuffer PREFIX=/usr/local NETSURF_CONFIG="$(echo $cfg)" \
        >"$BLD/netsurf.log" 2>&1 || { grep -E "error|Error|undefined" "$BLD/netsurf.log" | head -40; exit 1; }
    make install TARGET=framebuffer PREFIX=/usr/local DESTDIR="$SYSROOT" NETSURF_CONFIG="$(echo $cfg)" \
        >>"$BLD/netsurf.log" 2>&1 || { tail -20 "$BLD/netsurf.log"; exit 1; }
    # Polices supplémentaires (italique, serif) propres au navigateur
    mkdir -p "$P/share/netsurf/fonts"
    archive="$(fetch https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2)"
    for f in DejaVuSans-Oblique DejaVuSans-BoldOblique DejaVuSerif DejaVuSerif-Bold; do
        tar -xjf "$archive" -O "dejavu-fonts-ttf-2.37/ttf/$f.ttf" >"$P/share/netsurf/fonts/$f.ttf"
    done
}

b_libmnl() {
    # Pare-feu (nftables) : bibliothèque de communication netlink
    unpack libmnl https://www.netfilter.org/projects/libmnl/files/libmnl-1.0.5.tar.bz2
    autotools_build libmnl
}
b_libnftnl() {
    unpack libnftnl https://www.netfilter.org/projects/libnftnl/files/libnftnl-1.2.8.tar.xz
    autotools_build libnftnl
}
b_nftables() {
    # nft : l'outil du pare-feu, entièrement statique (mini-gmp intégré,
    # sans readline ni JSON)
    unpack nftables https://www.netfilter.org/projects/nftables/files/nftables-1.1.1.tar.xz
    ./configure --prefix=/usr/local --disable-shared --enable-static --with-mini-gmp \
        --without-cli --without-json --disable-man-doc --disable-python \
        >"$BLD/nftables.log" 2>&1 || { tail -30 "$BLD/nftables.log"; exit 1; }
    # libtool : « -all-static » = binaire sans aucune bibliothèque partagée
    make -j"$JOBS" LDFLAGS="-L$P/lib -all-static" >>"$BLD/nftables.log" 2>&1 ||
        { grep -E 'error|undefined' "$BLD/nftables.log" | head -30; exit 1; }
    make install DESTDIR="$SYSROOT" >>"$BLD/nftables.log" 2>&1
    rm -f "$P"/lib/*.la
}

b_cryptsetup() {
    # Chiffrement du disque (LUKS2) : cryptsetup entièrement statique, avec
    # les bibliothèques statiques d'Alpine (pas celles de notre sysroot)
    unpack cryptsetup https://www.kernel.org/pub/linux/utils/cryptsetup/v2.7/cryptsetup-2.7.5.tar.xz
    PKG_CONFIG_LIBDIR=/usr/lib/pkgconfig CFLAGS="-O2" CPPFLAGS="" LDFLAGS="" \
    ./configure --prefix=/usr/local --disable-shared --enable-static --enable-static-cryptsetup \
        --with-crypto_backend=openssl --enable-libargon2 --disable-asciidoc --disable-nls \
        --disable-udev --disable-selinux --disable-ssh-token --disable-external-tokens \
        --disable-veritysetup --disable-integritysetup --disable-blkid \
        >"$BLD/cryptsetup.log" 2>&1 || { tail -30 "$BLD/cryptsetup.log"; exit 1; }
    make -j"$JOBS" cryptsetup.static >>"$BLD/cryptsetup.log" 2>&1 ||
        { grep -E 'error|undefined' "$BLD/cryptsetup.log" | head -30; exit 1; }
    install -D -m 755 cryptsetup.static "$P/sbin/cryptsetup"
    strip "$P/sbin/cryptsetup"
}
b_e2fsprogs() {
    # resize2fs (réduire le système de fichiers avant de le chiffrer) et e2fsck
    unpack e2fsprogs https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.1/e2fsprogs-1.47.1.tar.xz
    # musl : lseek64 n'est qu'une macro (avec _LARGEFILE64_SOURCE), introuvable
    # par configure ; on le lui dit pour éviter le repli « _llseek » (32 bits)
    ac_cv_func_lseek64=yes CFLAGS="-O2 -D_LARGEFILE64_SOURCE" CPPFLAGS="" LDFLAGS="-static" \
        ./configure --disable-nls --disable-fuse2fs \
        --disable-elf-shlibs --disable-uuidd --disable-defrag --disable-debugfs --disable-imager \
        >"$BLD/e2fsprogs.log" 2>&1 || { tail -30 "$BLD/e2fsprogs.log"; exit 1; }
    make -j"$JOBS" LDFLAGS="-static" libs >>"$BLD/e2fsprogs.log" 2>&1 &&
    make -j"$JOBS" LDFLAGS="-static" -C resize resize2fs >>"$BLD/e2fsprogs.log" 2>&1 &&
    make -j"$JOBS" LDFLAGS="-static" -C e2fsck e2fsck >>"$BLD/e2fsprogs.log" 2>&1 ||
        { grep -E 'error|undefined' "$BLD/e2fsprogs.log" | head -30; exit 1; }
    install -D -m 755 resize/resize2fs "$P/sbin/resize2fs"
    install -D -m 755 e2fsck/e2fsck "$P/sbin/e2fsck"
    strip "$P/sbin/resize2fs" "$P/sbin/e2fsck"
}

STEPS="zlib libpng expat freetype fontconfig pixman libffi pcre2 glib cairo fribidi harfbuzz pango libxml2
       wayland wayland_protocols libxkbcommon xkeyboard_config libdrm mtdev libevdev libudev_zero
       libinput seatd libdisplay_info wlroots labwc tllist fcft foot xcompose fonts
       alsa_lib alsa_utils libnl wpa_supplicant hostapd regdb
       openssl curl libjpeg netsurf alpine_keys libmnl libnftnl nftables cryptsetup e2fsprogs"

# Reconstruction forcée d'un composant : build-gui.sh <composant>
if [ $# -gt 0 ]; then
    for c in "$@"; do rm -f "$(done_file "$c")"; done
fi
for s in $STEPS; do
    step "$s" "b_$s"
done
log "Pile graphique prête dans build/gui/sysroot"
