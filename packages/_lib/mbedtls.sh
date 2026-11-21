# Construit mbedTLS une seule fois (statique, musl) dans $WORK/mbedtls-sysroot.
# Bibliothèque TLS légère (chiffrement HTTPS), utilisée par curl.
MBEDTLS_VERSION=3.6.4
MBEDTLS_SYSROOT="$WORK/mbedtls-sysroot"

build_mbedtls_sysroot() {
    [ -f "$MBEDTLS_SYSROOT/usr/local/lib/libmbedtls.a" ] && return 0
    log "Compilation de mbedTLS $MBEDTLS_VERSION (dépendance de build)"
    rm -rf "$WORK/mbedtls-$MBEDTLS_VERSION"
    tar -xf "$(fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2")" -C "$WORK"
    cd "$WORK/mbedtls-$MBEDTLS_VERSION"
    # Ces fichiers générés sont fournis dans l'archive, mais leur date les fait
    # paraître périmés et make voudrait les regénérer avec Python : on les
    # marque comme à jour.
    touch library/psa_crypto_driver_wrappers.h library/psa_crypto_driver_wrappers_no_static.c
    # Seulement les bibliothèques (pas les programmes d'exemple ni les tests)
    make -j"$JOBS" CC="$MUSL_CC" lib >/dev/null
    # Installation manuelle ('make install' voudrait aussi construire les tests)
    mkdir -p "$MBEDTLS_SYSROOT/usr/local/lib" "$MBEDTLS_SYSROOT/usr/local/include"
    cp -r include/mbedtls include/psa "$MBEDTLS_SYSROOT/usr/local/include/"
    cp library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a \
       "$MBEDTLS_SYSROOT/usr/local/lib/"
}
