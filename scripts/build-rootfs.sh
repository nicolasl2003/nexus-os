#!/bin/sh
# Assemble le système de fichiers racine et l'emballe en initramfs
. "$(dirname "$0")/common.sh"

[ -x "$ROOTFS_DIR/bin/busybox" ] || { echo "Lance d'abord build-busybox.sh" >&2; exit 1; }

log "Assemblage du rootfs"
cd "$ROOTFS_DIR"
mkdir -p dev proc sys tmp run root home etc/init.d var/log usr/local
ln -sfn /run var/run   # emplacement classique des sockets (wpa_supplicant, etc.)
chmod 1777 tmp

# Fichiers propres à Nexus OS (init, motd, os-release...)
cp -a "$ROOT_DIR/rootfs/." "$ROOTFS_DIR/"
# Dossiers habituels de chaque nouveau compte (copiés depuis /etc/skel)
mkdir -p etc/skel/Documents etc/skel/Images "etc/skel/Téléchargements"
sed -i "s/@VERSION@/$NEXUS_VERSION/g" etc/os-release etc/motd

# Vérification des paquets : minisign + clé publique du dépôt (dans le
# système de base, pour qu'un attaquant ne puisse pas les remplacer via npk)
[ -x "$MINISIGN" ] || { echo "minisign absent : lance 'make tools'" >&2; exit 1; }
[ -f "$NPK_PUB" ]  || { echo "clé publique absente : lance 'make keys'" >&2; exit 1; }
install -m 755 "$MINISIGN" usr/bin/minisign
mkdir -p etc/npk/keys
install -m 644 "$NPK_PUB" etc/npk/keys/nexus-npk.pub

# Bureau embarqué : les paquets du bureau (et leurs dépendances) sont copiés
# dans l'image, avec l'INDEX signé. Au premier démarrage avec disque, rcS les
# installe sans réseau : l'assistant de configuration graphique peut s'ouvrir
# tout de suite. (Toujours vérifiés par npk : signature + SHA-256.)
PREINSTALL="nexus-desktop netsurf parefeu"
rm -rf usr/share/nexus/preinstall
if [ -f "$REPO_DIR/INDEX" ] && [ -f "$REPO_DIR/INDEX.minisig" ]; then
    pre=usr/share/nexus/preinstall
    mkdir -p "$pre"
    cp "$REPO_DIR/INDEX" "$REPO_DIR/INDEX.minisig" "$pre/"
    todo="$PREINSTALL"; seen=""
    while [ -n "$todo" ]; do
        set -- $todo
        [ $# -gt 0 ] || break
        p=$1; shift; todo="$*"
        case " $seen " in *" $p "*) continue ;; esac
        seen="$seen $p"
        line=$(grep "^$p|" "$REPO_DIR/INDEX" | head -n 1)
        [ -n "$line" ] || { echo "Paquet embarqué absent du dépôt : $p (make packages)" >&2; exit 1; }
        cp "$REPO_DIR/$(echo "$line" | cut -d'|' -f5)" "$pre/"
        todo="$todo $(echo "$line" | cut -d'|' -f3)"
    done
    echo "$PREINSTALL" > "$pre/LIST"
    log "Bureau embarqué :$seen ($(du -sh "$pre" | cut -f1))"
else
    log "Dépôt absent : pas de bureau embarqué (make packages pour l'ajouter)"
fi

# Droits : shadow lisible par root seul ; seul le petit busybox-suid
# (su, passwd, ping) a les droits root, le BusyBox principal aucun.
chmod 600 etc/shadow
chmod 755 bin/busybox
[ -x bin/busybox-suid ] || { echo "busybox-suid absent : lance 'make busybox'" >&2; exit 1; }
chmod 4755 bin/busybox-suid

# Le noyau exécute /init depuis l'initramfs : on pointe sur busybox init
ln -sf sbin/init init

log "Création de l'initramfs"
find . -print0 | cpio --null -o -H newc --owner root:root 2>/dev/null \
    | gzip -9 > "$OUT_DIR/initramfs.cpio.gz"
log "Initramfs prêt : build/out/initramfs.cpio.gz ($(du -h "$OUT_DIR/initramfs.cpio.gz" | cut -f1))"
