# Nexus OS

Distribution Linux minimale : noyau Linux + BusyBox, démarrée en initramfs dans QEMU.

## Prérequis

Docker (OrbStack). Tout le reste (compilateur, QEMU) est dans l'image `nexus-os-builder`.

## Commandes

| Commande     | Effet                                                    |
|--------------|----------------------------------------------------------|
| `make all`   | Compile noyau + BusyBox, crée l'initramfs et l'ISO, copie dans `dist/` |
| `make run`   | Démarre Nexus OS dans QEMU (quitter : `poweroff` ou Ctrl+A puis X) |
| `make run-native` | **Le plus rapide** : Nexus OS dans une fenêtre du Mac, avec accélération matérielle (HVF). Prérequis : `./scripts/build-qemu-mac.sh` (une fois) |
| `make run-gui` | Démarre Nexus OS avec un **écran graphique**, visible sur http://localhost:6080 |
| `NO_DISK=1 make run…` | Démarre sans le disque de données (mode live) |
| `make run-iso-gui` | Pareil, en démarrant depuis l'ISO (menu GRUB visible à l'écran) |
| `make iso dist` | Régénère l'ISO bootable `dist/nexus-os-<version>-x86_64.iso` |
| `make run-iso`  | Démarre l'ISO via GRUB, comme un PC BIOS              |
| `make run-uefi` | Démarre l'ISO via GRUB, comme un PC UEFI              |
| `make rootfs`| Reconstruit seulement l'initramfs (après modif de `rootfs/`) |
| `make tools` | Compile minisign (signature des paquets) |
| `make keys` | Génère la paire de clés qui signe le dépôt (une seule fois, dans `keys/`) |
| `make packages` | Construit les paquets npk et le dépôt (`make packages PKG=nano` pour un seul) |
| `make disk`  | Crée le disque persistant `disk/nexus-data.img` (fait automatiquement par `make run*`) |
| `make disk-reset` | **Efface** toutes les données persistantes et recrée un disque vierge |
| `make shell` | Shell dans l'environnement de build Linux                |
| `make clean` | Supprime `dist/` et le volume de build (le disque de données est conservé) |

## Arborescence

- `config/versions.env` — versions du noyau et de BusyBox
- `config/kernel.fragment` — options noyau ajoutées au `x86_64_defconfig`
- `rootfs/` — fichiers propres à Nexus OS copiés dans le système (init, motd, os-release…)
- `packages/` — recettes des paquets npk (une par dossier)
- `scripts/` — scripts de build exécutés dans Docker
- `dist/` — résultat : `bzImage` (noyau) et `initramfs.cpio.gz` (système)

Les sources compilées vivent dans le volume Docker `nexus-os-build`, car le disque
du Mac n'est pas sensible à la casse et casse l'arbre du noyau.

## Persistance

Au démarrage, Nexus OS cherche une partition ext4 étiquetée `NEXUS_DATA`.
Si elle existe, elle est montée sur `/data` et `/root`, `/home`, `/usr/local`
(paquets) et les comptes (`/etc/passwd`, `shadow`, `group`) y sont
redirigés : ce que tu y crées survit au redémarrage. Le reste du système
(`/etc`, `/bin`…) est recréé à neuf à chaque démarrage depuis l'initramfs.

- Le disque virtuel utilisé par QEMU est `disk/nexus-data.img` (1 Go, modifiable avec `DATA_DISK_SIZE=4G make disk-reset`).
- Pour changer le nom de la machine de façon permanente : `echo monpc > /data/hostname`.
- Sur un vrai PC (clé USB), n'importe quelle partition ext4 étiquetée `NEXUS_DATA` fonctionne.
- Éteins toujours avec `poweroff` (en root : `su -c poweroff`) pour que les écritures soient bien enregistrées.

## Gestionnaire de paquets : npk

`npk` est le gestionnaire de paquets de Nexus OS (script shell : `rootfs/usr/bin/npk`).
Pendant `make run`, le dépôt `build/repo` est servi automatiquement à QEMU
sur `http://10.0.2.2:8080`.

```sh
su                          # les commandes qui modifient le système demandent root
npk update                  # récupère la liste des paquets
npk search                  # liste ce qui est disponible
npk install nano            # installe nano + sa dépendance ncurses-terminfo
npk list                    # paquets installés
npk remove nano             # désinstalle (refusé si un autre paquet en dépend)
npk upgrade                 # met à jour ce qui a changé dans le dépôt
```

Les paquets s'installent dans `/usr/local`, qui est sur le disque persistant :
ils restent installés après redémarrage.

**Format** : un `.npk` est un `tar.gz` contenant `PKGINFO` (nom, version,
dépendances, description) et `files/` (arborescence copiée dans `/usr/local`).
Le fichier `INDEX` du dépôt liste une ligne par paquet :
`nom|version|dépendances|sha256|fichier|description`. npk vérifie le SHA-256
de chaque téléchargement et refuse les conflits de fichiers entre paquets.

**Paquets disponibles** : `nano`, `htop`, `tmux`, `jq`, `lua`, `dropbear` (SSH),
`curl` (HTTPS), `ca-certificates`, `nexusfetch`, `ncurses-terminfo`.

**Ajouter un paquet** : crée `packages/<nom>/recipe` avec `NAME`, `VERSION`,
`REL` (révision, optionnelle : à incrémenter si tu modifies la recette sans
changer de version), `DEPENDS`, `DESC` et une fonction `build()` qui installe dans
`$DESTDIR$PREFIX`. Compile avec `$MUSL_CC` (notre propre musl 1.2.5, déjà en `-static`) : Nexus OS n'a pas
de libc partagée, chaque binaire doit être autonome. Exemples : `packages/lua`
(simple), `packages/nano` (avec dépendance), `packages/nexusfetch` (script).
Puis `make packages PKG=<nom>`, et dans Nexus OS `npk update && npk install <nom>`.

## Utilisateurs et connexion

**Premier démarrage avec disque** : un assistant (`nexus-setup`) demande le
nom de la machine, le mot de passe root et crée ton compte utilisateur.
Ensuite, chaque démarrage affiche un écran `login:`.

**Mode live** (sans disque `NEXUS_DATA`) : pas d'assistant, connecte-toi en
`root` sans mot de passe. Rien n'est sauvegardé.

| Commande | Effet |
|---|---|
| `su` | devenir root (mot de passe root) |
| `su -c "commande"` | lancer une seule commande en root |
| `passwd` | changer son mot de passe |
| `adduser bob` | (root) créer un compte |
| `deluser bob` | (root) supprimer un compte |
| `nexus-setup` | (root) relancer l'assistant de configuration |

- Mots de passe chiffrés en SHA-512 dans `/etc/shadow` (lisible par root seul).
- Mot de passe root oublié : `make disk-reset` efface tout ; ou, pour garder
  tes fichiers, relance l'assistant au prochain démarrage avec
  `make shell` puis `debugfs -w -R "rm /etc/.configured" disk/nexus-data.img`.
- Fichiers concernés : `rootfs/usr/sbin/nexus-setup` (assistant),
  `rootfs/usr/sbin/nexus-getty` (écrans de connexion), `rootfs/etc/inittab`.

## Services

Un paquet peut fournir un service : un script `etc/init.d/SNNnom` (installé
dans `/usr/local/etc/init.d/`) qui accepte `start`, `stop` et `status`.
Les services démarrent automatiquement au boot et s'arrêtent proprement à
l'extinction. `npk upgrade` redémarre un service mis à jour.

```sh
service                      # liste les services et leur état
service dropbear restart     # (root) start | stop | restart | status
service dropbear disable     # (root) ne plus démarrer au boot (enable pour réactiver)
logread                      # journal système (/var/log/messages)
```

## SSH depuis le Mac

```sh
# Dans Nexus OS (en root) :
npk update && npk install dropbear && service dropbear start
```

`make run` redirige le port 2222 du Mac vers le port 22 de Nexus OS
(uniquement sur `127.0.0.1`). Depuis le Terminal du Mac :

```bash
ssh -p 2222 alice@localhost             # remplace alice par ton compte
scp -O -P 2222 fichier.txt alice@localhost:~/
```

- La connexion directe en root est interdite : connecte-toi avec ton compte puis `su`.
- La clé d'hôte est générée une fois et conservée sur le disque persistant.
  Après un `make disk-reset`, le Mac signalera un changement de clé :
  `ssh-keygen -R "[localhost]:2222"` règle ça.
- Pour te connecter sans mot de passe, copie ta clé publique du Mac dans
  `~/.ssh/authorized_keys` sur Nexus OS (droits 700 sur `~/.ssh`, 600 sur le fichier).

## HTTPS (curl + TLS)

```sh
npk install curl                       # installe aussi ca-certificates
curl -s https://api.github.com/repos/torvalds/linux | jq .stargazers_count
```

- `curl` est compilé en statique avec **mbedTLS** (bibliothèque TLS légère).
- `ca-certificates` fournit les 118 certificats racines de Mozilla
  (`/usr/local/etc/ssl/certs/ca-certificates.crt`) : curl refuse les sites au
  certificat expiré, auto-signé ou au mauvais nom de domaine.
- npk utilise curl dès qu'il est installé : un dépôt peut alors être en
  `https://` (dans `/usr/local/etc/npk/repo.conf`). Sans curl, npk n'accepte
  que les dépôts `http://` (le `wget` de BusyBox ne vérifie pas les certificats).
- TLS a besoin d'une horloge juste (vérification des dates des certificats) :
  vérifie `date` si une connexion HTTPS échoue sans raison apparente.
- Mettre à jour les certificats : change `VERSION` dans
  `packages/ca-certificates/recipe` (date d'un bundle publié sur curl.se/ca),
  puis `make packages PKG=ca-certificates` et `npk upgrade`.

## Signature des paquets

Chaque dépôt npk est **signé** : `make packages` signe l'`INDEX` avec
`keys/nexus-npk.key` (signature Ed25519 via **minisign**). Nexus OS
embarque `minisign` et la clé publique (`/etc/npk/keys/nexus-npk.pub`)
dans son système de base, et `npk update` vérifie tout :

| Attaque | Réaction de npk |
|---|---|
| Index modifié | `SIGNATURE INVALIDE`, mise à jour refusée |
| Index re-signé avec une autre clé | `SIGNATURE INVALIDE`, mise à jour refusée |
| Paquet modifié (index authentique) | `somme de contrôle invalide`, installation refusée |
| Ancien index signé resservi | refusé : `possible attaque par retour en arrière` |
| Signature supprimée | refusé : `le dépôt n'est pas signé` |

**Chaîne de confiance** : clé publique (dans l'image système) → signature de
l'INDEX → SHA-256 de chaque paquet listé dans l'INDEX. Le commentaire de
confiance signé contient l'heure de signature, et npk refuse tout index
plus ancien que le dernier accepté (anti-rollback).

⚠️ **`keys/nexus-npk.key` est la clé secrète** :
- ne la publie jamais et ne la committe pas (elle est dans `.gitignore`) ;
- **sauvegarde-la** (clé USB, gestionnaire de mots de passe) : si tu la perds,
  il faudra générer une nouvelle paire et reconstruire l'ISO, et les systèmes
  déjà installés refuseront les nouveaux paquets ;
- si elle fuite, n'importe qui peut signer des paquets acceptés par Nexus OS :
  supprime `keys/`, relance `make keys`, puis reconstruis tout (`make all`).

Pour faire confiance à un autre dépôt, root peut ajouter sa clé publique dans
`/usr/local/etc/npk/keys/`. Pour un dépôt de test non signé (déconseillé) :
`ALLOW_UNSIGNED=yes` dans `/usr/local/etc/npk/repo.conf`.

## Affichage graphique

```bash
make run-gui
```

Puis ouvre **http://localhost:6080** dans ton navigateur : c'est l'écran de
Nexus OS (logo au démarrage, puis écran de connexion). Le terminal où tu as
lancé la commande reste la console série (Ctrl+A puis X pour quitter QEMU).

- QEMU émule une carte graphique **virtio-gpu**, un clavier et une souris
  virtio ; l'écran est exporté en VNC (port 5900, pour un client VNC) et en
  VNC sur websocket (port 5700), affiché dans le navigateur par **noVNC**
  (port 6080). Tous ces ports n'écoutent que sur `127.0.0.1`.
- Le noyau inclut aussi `simpledrm` (framebuffer du BIOS/UEFI) et `bochs` :
  l'écran devrait s'afficher sur la plupart des PC et des VM.
- Le **logo de démarrage** est généré par `config/logo/make-logo.py` (modifie-le
  puis relance-le, puis `make kernel`). Le noyau impose une image PPM d'au
  plus 224 couleurs ; `nexus_logo_preview.png` permet de voir le résultat.
- Les options graphiques du noyau sont dans `config/kernel.fragment`.
- Le noyau démarre avec `quiet loglevel=5` : sans `loglevel=5`, les noyaux
  récents n'affichent plus le logo en mode `quiet`.
- `NO_DISK=1 make run-gui` démarre en mode live, sans brancher le disque.
- **Clavier** : AZERTY par défaut, sur le bureau (touches mortes : `^` puis
  `e` = `ê`) comme dans les consoles texte. Autre disposition : Paramètres →
  Clavier, ou `su -c "nexus-keymap set us"` (liste : `nexus-keymap list`).

## Bureau graphique (Wayland)

**Installation automatique** : au premier démarrage (et donc après un
`make disk-reset`), l'assistant `nexus-setup` propose en étape 4/4
d'installer le bureau (réponse par défaut : oui). Il télécharge
`nexus-desktop` depuis le dépôt (lancer la VM avec `make run-native` pour que
le dépôt soit servi), démarre `seatd`, puis le premier écran passe directement
à la connexion graphique.

À la main :

```sh
# Dans Nexus OS (en root) :
npk update && npk install nexus-desktop && service seatd start
```

Ensuite, **se connecter sur l'écran du PC** (`make run-gui`, puis
http://localhost:6080) ouvre automatiquement le bureau. Les autres consoles
(Ctrl+Alt+F2…) restent en mode texte ; `nexus-desktop` lance aussi le bureau
à la main. Un compte utilisateur doit faire partie du groupe `video`
(`nexus-setup` s'en charge ; sinon : `su -c "addgroup moncompte video"`).

| Paquet | Rôle |
|---|---|
| `nexus-desktop` | Le bureau Nexus : barre des tâches, menu Démarrer, fond d'écran, Paramètres, thème |
| `labwc` | Compositeur Wayland : affiche et gère les fenêtres |
| `foot` | Terminal graphique |
| `seatd` | Service qui donne au bureau l'accès à l'écran, au clavier, à la souris |
| `xkeyboard-config` | Dispositions de clavier et touches mortes |
| `fonts-dejavu` | Polices |

Raccourcis : **Super+Entrée** ou **Ctrl+Alt+T** (terminal), **Alt+F4** (fermer),
**Alt+Tab** (changer de fenêtre), **clic droit sur le bureau** (menu).

**Construction** : la pile graphique (31 composants : cairo, pango, glib,
Wayland, libinput, wlroots, labwc, foot…) est compilée depuis les sources, en
statique, dans une image **Alpine** (musl native avec compilateur C++, requis
par harfbuzz) : `make gui-stack` (reprend là où il s'est arrêté ; `make
gui-stack C=labwc` reconstruit un composant). Les recettes de paquets copient
ensuite les programmes depuis `build/gui/sysroot`. Le rendu est logiciel
(pixman) : pas besoin d'accélération 3D.

Configuration : `packages/nexus-desktop/files/` (session `bin/nexus-desktop`,
`etc/xdg/labwc/` pour les raccourcis/menu/démarrage, thème dans
`share/themes/Nexus/openbox-3/themerc`).

## Le bureau Nexus (nos programmes)

Écrits en C pour Nexus OS, dans `desktop/` :

| Programme | Rôle |
|---|---|
| `nexus-panel` | Barre des tâches en bas : bouton **N**, fenêtres ouvertes (clic = afficher/réduire, clic du milieu = fermer), horloge et date |
| `nexus-menu` | Menu Démarrer façon Windows 7 : applications et recherche à gauche, compte, raccourcis et Arrêter/Redémarrer/Se déconnecter à droite. S'ouvre avec le bouton N ou la touche **Super** ; au clavier : taper pour chercher, flèches, Entrée, Échap |
| `nexus-wallpaper` | Fond d'écran : 6 fonds Nexus dessinés (s'adaptent à toute résolution), couleur unie ou image PNG |
| `nexus-settings` | Paramètres : fond d'écran, couleur d'accent (barre, menu ET barres de titre), taille du texte, À propos |
| `nexus-power` | Arrêt/redémarrage depuis le menu pour les membres du groupe `video` (setuid root, n'accepte que ces deux actions) |

- `desktop/common/nexus-ui.[ch]` : notre bibliothèque d'interface (Wayland
  layer-shell/xdg-shell, dessin cairo, texte pango, icônes vectorielles,
  logo, fonds d'écran, configuration).
- Les quatre programmes sont **un seul binaire** (`nexus-shell`, 6 Mo) appelé
  sous quatre noms : en statique, cela évite de copier quatre fois cairo,
  pango et glib.
- Réglages : `~/.config/nexus/desktop.conf` (défauts système :
  `/usr/local/etc/nexus/desktop.conf`). Paramètres prévient les autres
  programmes par `SIGUSR1` et labwc par `SIGHUP` : tout change en direct.
- Applications du menu : fichiers `.desktop` de `/usr/local/share/applications`
  et `~/.local/share/applications` (ceux de Nexus : `desktop/data/applications/`).
  `TryExec=` masque une application non installée.
- Images personnelles : dépose des PNG dans `~/Images`, elles apparaissent
  dans Paramètres → Fond d'écran.

**Compiler** : `make desktop-apps` (image Alpine), puis `make packages PKG=nexus-desktop`.

**Compte créé avant cette version** : il doit rejoindre le groupe `video`
pour ouvrir le bureau et utiliser Arrêter/Redémarrer :
`su -c "addgroup moncompte video"`, puis se reconnecter.

## Performances

Dans Docker, QEMU n'a pas accès à l'accélération matérielle du Mac : il
**émule** le processeur en logiciel (TCG), 10 à 50 fois plus lentement qu'un
vrai PC. Ce qui a été optimisé :

| Optimisation | Gain mesuré |
|---|---|
| QEMU : 4 processeurs virtuels + émulation multi-cœur (MTTCG), 1 Go de mémoire | tâches parallèles 2x plus rapides (53 s → 26 s) |
| Fond d'écran : dégradés calculés en 1/4 de résolution puis agrandis | 10x plus rapide (3 s → 0,3 s dans la VM) |
| Paramètres : miniatures dessinées une seule fois (cache) | plus de recalcul à chaque mouvement de souris |
| Bureau : programmes prévenus par signal direct (sans lancer de processus) | changements de réglages immédiats |
| **`make run-native`** : QEMU sur le Mac avec HVF | démarrage 8 s → 2 s, calcul 10x+ plus rapide, fond d'écran 298 ms → 9 ms |

`nexus-shell bench` (dans Nexus OS) chronomètre le dessin du fond d'écran.
Sur un PC Linux, les scripts utilisent KVM automatiquement si `/dev/kvm` est
accessible.

⚠️ Ne lance jamais deux Nexus OS en même temps sur le même disque
(`make run-native` et `make run-gui` par exemple) : le disque de données
serait abîmé. Utilise `NO_DISK=1` pour un second lancement.

### QEMU pour le Mac (mode natif)

`brew install qemu` échoue sur ce Mac (macOS 14 : Homebrew compile tout, et un
test de la dépendance p11-kit dépasse son délai). `scripts/build-qemu-mac.sh`
compile donc une version **minimale** de QEMU (émulateur x86_64, accélération
HVF, fenêtre Cocoa, réseau slirp) dans `tools/qemu` (~30 min, une seule fois).
Dépendances Homebrew : `glib pixman libslirp meson ninja pkgconf`.
Le binaire est signé avec le droit `com.apple.security.hypervisor`, requis par HVF.

`make run-native` (ou un double-clic sur `scripts/nexus-fenetre.command`)
ouvre Nexus OS **en plein écran** sur le Mac, à la résolution exacte de
l'écran (ex. 1440×900 sur un écran Retina), lancé depuis l'application
Terminal : c'est le mode le plus fluide. **Cmd+F** sort du plein écran (ou y
revient), **Cmd+Q** ferme Nexus OS. `NATIVE_FULLSCREEN=0` pour une simple
fenêtre. (Lancée depuis certaines applications, comme le
terminal intégré de l'app Claude, la fenêtre QEMU ne reçoit pas le clavier :
d'où le passage par Terminal.)

`make run-native-web` affiche plutôt l'écran dans le navigateur
(http://localhost:6080) : moins fluide (VNC), mais utilisable partout.

QEMU 8 et plus déclarent la tablette virtuelle avec `BTN_TOUCH` : sans le
second correctif de libudev-zero (dans `scripts/build-gui.sh`), elle serait
prise pour un écran tactile et la souris ne bougerait pas (labwc ≥ 0.8.2-r3).

## Gestionnaire de fichiers (nexus-files)

Menu Démarrer → **Fichiers** (ou les raccourcis Dossier personnel, Documents,
Images de la colonne de droite).

- Barre latérale : Dossier personnel, Documents, Images, Téléchargements
  (créés automatiquement), Disque de données, Système, Corbeille.
- Double-clic : ouvrir un dossier, un texte (éditeur nano) ou une image PNG
  (visionneuse, avec « Définir comme fond d'écran » et ◀ ▶ pour les autres
  images du dossier).
- Clic droit : Ouvrir, Couper, Copier, Coller, Renommer, Mettre à la corbeille,
  Nouveau dossier, Afficher les fichiers cachés.
- **Corbeille** (`~/.local/share/Trash`) : supprimer est rattrapable
  (Restaurer) ; « Vider la corbeille » et « Supprimer définitivement »
  demandent confirmation.
- Clavier : Entrée, Retour arrière (dossier parent), Suppr, F2 (renommer),
  F5, Ctrl+C/X/V/A, Ctrl+H (fichiers cachés), Ctrl+N (nouveau dossier).

## Interface moderne (Nexus 0.4)

Thème sombre « verre », inspiré des bureaux actuels, bouton Démarrer à gauche :

- **Écran de connexion graphique** (`nexus-login` + `nexus-greeter`) : fond
  d'écran, grande horloge, carte de connexion, choix du compte, Redémarrer /
  Éteindre. Le dernier compte utilisé est présélectionné. Se déconnecter
  ramène à cet écran. Si l'affichage graphique n'est pas disponible, retour
  automatique à la connexion texte. (Le premier écran du PC lance
  `/usr/sbin/nexus-tty1`, qui choisit entre les deux.)
- **Fond « Nuit étoilée »** (par défaut) : ciel étoilé, N lumineux, montagnes,
  lac, entièrement dessiné par le programme (s'adapte à toute résolution).
- **Bureau** : icônes Ce PC, Mes fichiers, Corbeille (double-clic) et
  **widgets** : horloge et message d'accueil, performances (processeur,
  mémoire, disque + courbe), **Mes tâches** (cocher, ajouter, supprimer ;
  `~/.config/nexus/taches.txt`). Désactivables dans Paramètres → Affichage.
- **Barre des tâches** : N (menu), loupe (recherche), applications épinglées
  avec indicateur de fenêtre ouverte/active, autres fenêtres, réseau, heure et
  date.
- **Menu Démarrer** : navigation à gauche (Accueil, Applications, Fichiers,
  Paramètres, Système, Alimentation, compte, Éteindre) ; à droite recherche
  (applications **et fichiers**), applications épinglées, fichiers récents.

`nexus-shell render preset:montagne apercu.png 1440 900` dessine un fond
d'écran dans une image (pratique pour essayer des variantes).

**Sécurité** : l'écran de connexion tourne en root (il lit `/etc/shadow`),
dans un compositeur sans raccourcis ; un vrai système isolerait cette partie
dans un compte dédié (comme greetd). Après 1 mauvais mot de passe, 1 s
d'attente.

## Son et Wi-Fi

```sh
npk install wifi          # Wi-Fi : pilotes + firmwares (linux-wifi) + wpa_supplicant
                          # le son (alsa) est installé avec le bureau
```

**Son** : pilotes Intel HD Audio (+ codecs Realtek, HDMI, Conexant, IDT) et
cartes son USB, bibliothèque ALSA. Icône de haut-parleur dans la barre des
tâches → panneau **Son** (curseur, muet, molette). Le volume est mémorisé
(service `son`). Petit carillon à l'ouverture de session. Sur le Mac, `make
run-native` envoie le son vers les haut-parleurs (CoreAudio).

**Wi-Fi** : pilotes en modules (Intel AX200/AX201/AX210/9560/8265…, MediaTek
MT7921, Realtek RTL8822/8821, Atheros ath9k) avec leurs firmwares (paquet
`linux-wifi`), chargés automatiquement selon le matériel présent ou branché.
Icône réseau de la barre → panneau **Réseau** : état Ethernet, liste des
réseaux Wi-Fi (signal, cadenas), clic + mot de passe → connecté (réseaux
mémorisés dans `/usr/local/etc/wpa_supplicant.conf`, service `wifi`).

**Tester le Wi-Fi sans carte Wi-Fi** (ex. dans QEMU) : `su -c nexus-wifi-test`
crée deux radios simulées par le noyau (`mac80211_hwsim`) et un point d'accès
« Nexus-Test » (mot de passe `nexus2026`) auquel se connecter depuis le panneau.

Les comptes du bureau sont ajoutés automatiquement aux groupes `audio` (son)
et `netdev` (Wi-Fi) à la connexion.

## Navigateur web (NetSurf)

```sh
npk install netsurf       # puis : icône globe de la barre des tâches, ou menu → Navigateur
nexus-browser [adresse]   # depuis un terminal du bureau
```

[NetSurf](https://www.netsurf-browser.org) est un navigateur léger (HTML,
CSS, un peu de JavaScript via Duktape, HTTPS via OpenSSL). Il est compilé
dans la pile graphique (`make gui-stack`, étapes `openssl curl libjpeg
netsurf`) avec une **sortie d'affichage Wayland écrite pour Nexus**
(`desktop/browser/nexus-wl.c`) : vraie fenêtre labwc (barre de titre,
agrandir, redimensionner), molette, clavier AZERTY avec accents (y compris
dans la barre d'adresse, voir `desktop/browser/patch-netsurf.py`).

- Page d'accueil : DuckDuckGo Lite (sans JavaScript) ; réglages dans
  `~/.netsurf/Choices` (créé au premier lancement).
- Raccourcis : Ctrl+C / Ctrl+V / Ctrl+A, Ctrl+Plus / Ctrl+Moins (zoom),
  Ctrl+Z, flèches et Page préc./suiv. pour défiler.
- Limites : les sites « applications » très chargés en JavaScript moderne
  (Gmail, YouTube, réseaux sociaux…) ne fonctionnent pas ou mal ; les sites
  d'information, Wikipédia, la documentation, les forums marchent bien.
  Interface de NetSurf en anglais (pas de traduction française fournie).

**DNS de secours** : si le serveur DNS fourni par le réseau ne répond pas (cas
du DNS intégré de QEMU sur certains Mac), le script DHCP ajoute 1.1.1.1 et
9.9.9.9 dans `/etc/resolv.conf`.

## Personnalisation et Magasin d'applications (Nexus 0.7)

**Paramètres** (icône engrenage) — nouvelles sections :

- **Couleurs** : 12 couleurs d'accent ; barre des tâches et menus
  *transparents*, *translucides* ou *opaques* ; option « Teinte de la couleur
  d'accent » qui colore la barre, le menu, les widgets et les panneaux.
- **Barre des tâches** : hauteur *petite* (42 px, heure seule), *moyenne*
  (52 px) ou *grande* (64 px). La barre change de taille immédiatement.
- **Icônes** : taille (*petites / moyennes / grandes* : barre, menu, bureau),
  style (*en couleurs* : logo blanc sur pastille colorée, ou *monochromes*),
  forme de la pastille (*arrondie / carrée / ronde*).

Les logos des applications sont dessinés par `nx_app_icon` (desktop/common) :
fichiers, navigateur, magasin, terminal, paramètres, moniteur, éditeur,
visionneuse, calculatrice, à propos ; une application inconnue a son initiale.
Réglages enregistrés dans `~/.config/nexus/desktop.conf` (`panel_size`,
`icon_size`, `icon_style`, `icon_shape`, `glass_opacity`, `glass_tint`).

**Magasin d'applications** (`nexus-store`, icône sac rose) : liste des paquets
du dépôt avec description et version, onglets *Toutes / Installées / Mises à
jour*, recherche au clavier, boutons **Installer**, **Supprimer**, **Mettre à
jour**, **Tout mettre à jour** et **Actualiser** (= `npk update`). Les paquets
indispensables au bureau sont marqués « Système » (pas de suppression).

Sécurité : le magasin passe par `/usr/local/libexec/nexus-pkg` (setuid root,
`desktop/store/nexus-pkg.c`) qui exige le **mot de passe administrateur (root)**
— gardé en mémoire tant que la fenêtre est ouverte —, n'accepte que
`check / update / upgrade / install / remove` et des noms de paquets simples,
puis lance `npk` (qui ne fait confiance qu'aux paquets signés).

## Nexus 0.8 : fenêtres, barre, widgets, comptes, logiciels Alpine

**Fenêtres** (labwc patché : `desktop/labwc/patch-labwc.py`) — glisser une
fenêtre par sa barre de titre :
- dans un **coin** de l'écran → elle occupe ce **quart** d'écran ;
- en **haut au centre** → **plein écran** ;
- contre le bord **gauche** ou **droit** → **moitié** d'écran.
Un aperçu bleuté montre la place avant de lâcher. **Redimensionner** : tirer un
bord ou un coin de la fenêtre (zone de prise élargie à 14 px), ou Alt + clic
droit glissé.

**Barre des tâches** : tirer son **bord supérieur** à la souris (36 à 110 px) ;
l'heure, la date et les icônes grandissent avec elle. Hauteurs prédéfinies dans
Paramètres → *Barre et menu*.

**Menu Démarrer** : tirer son **coin en haut à droite** (ou son bord haut ou
droit) pour l'agrandir jusqu'à 170 % (texte et icônes compris) ; tailles
Normal / Grand / Très grand dans Paramètres → *Barre et menu*.

**Widgets** : faire **glisser une carte** pour les déplacer, tirer le **coin en
bas à gauche** (ou le bord gauche) pour les agrandir. Paramètres → *Widgets* :
taille (petits → très grands), couleur (sombres, clairs, couleur d'accent,
transparents), « Remettre en haut à droite ».

**Texte** : Paramètres → *Affichage* : Normal, Grand ou **Très grand** (150 %).

**Comptes** (Paramètres → *Comptes*) : ajouter un compte, changer un mot de
passe, supprimer un compte (avec ou sans ses fichiers). Passe par
`/usr/local/libexec/nexus-account` (setuid root, `desktop/store/nexus-account.c`) :
mot de passe root exigé (ou ton propre mot de passe pour changer le tien),
root et le compte connecté ne peuvent pas être supprimés.

**Logiciels Alpine Linux** (paquet `alpine`) — Alpine utilise la même
bibliothèque C (musl) que Nexus : ses ~30 000 logiciels fonctionnent tels quels.

```sh
su -c "npk install alpine"          # active la prise en charge (+ openssl)
su -c "npk alpine update"           # liste signée des paquets (main + community)
npk alpine search firefox
su -c "npk alpine install firefox"  # avec toutes ses dépendances
su -c "npk alpine remove firefox"
npk alpine list ; su -c "npk alpine upgrade"
```

Aussi dans le **Magasin d'applications**, onglet *Alpine Linux* (recherche au
clavier, Installer / Supprimer). Testé : Python 3, **Firefox** (qui ouvre
Google normalement).

Fonctionnement : chaque liste et chaque paquet est vérifié avec les clés
publiques d'Alpine (signature RSA via `openssl`, puis empreinte SHA-256 du
contenu) ; les fichiers vont dans `/usr/local/alpine` (disque persistant) puis
`nexus-alpine-link` (service `alpine`, lancé à chaque démarrage) crée les liens
dans `/usr/lib`, `/usr/bin`, `/lib`, `/etc`… sans jamais remplacer un fichier
de Nexus. Les scripts d'installation d'Alpine ne sont pas exécutés (liste dans
`/usr/local/var/lib/npk/alpine/scripts-skipped`). Prévoir de la place : Firefox
et ses dépendances ≈ 300 Mo.

Noyau : `CONFIG_USER_NS=y` (bac à sable de Firefox).

## Nexus 0.9 : clavier, notifications, fichiers, Wi-Fi, mises à jour

Version unique **0.9.0** partout (écran de connexion, `/etc/os-release`,
Paramètres → À propos, nom de l'ISO, paquet `nexus-desktop`) : un seul
réglage, `NEXUS_VERSION` dans `config/versions.env`.

**Clavier** — `nexus-keymap` (`rootfs/usr/sbin/nexus-keymap`) : 13
dispositions (AZERTY, BÉPO, Mac, belge, suisse, canadien, US, UK, allemand,
espagnol, italien, portugais). Choisie à la 1re étape de l'assistant de
premier démarrage, enregistrée dans `/etc/keyboard` (+ `/data/keyboard`),
appliquée aux consoles au démarrage (tables `rootfs/usr/share/keymaps/*.bmap.gz`
chargées par `loadkmap`), à l'écran de connexion et au bureau.
**Paramètres → Clavier** : clic sur une disposition = changement immédiat
(labwc relit `~/.config/labwc/environment` sur SIGHUP) et des consoles (via
`nexus-power keymap`, autorisé au groupe `video`), avec une zone pour essayer.

**Notifications** — bulles en bas à droite (`nexus-notifyd`, lancé à
l'ouverture de session ; `desktop/shell/notify.c`). Clic : action (ex. ouvrir
le Magasin), croix : fermer ; la bulle survolée reste affichée.
```sh
nexus-notify "Titre" "Texte"                       # depuis un script
nexus-notify -i store -a nexus-store "Titre" "Texte"   # icône + action au clic
```
Envoyées automatiquement : fin d'installation / mise à jour dans le Magasin,
Wi-Fi connecté / déconnecté, batterie faible (20 %), très faible (8 %),
chargée, mises à jour disponibles. Les notifications du système (écrites par
root dans `/run/nexus/notify/`) ne s'affichent qu'une fois par session.

**Mises à jour automatiques** — service `updates` (paquet `nexus-desktop`) :
30 s après le démarrage puis toutes les 6 h, `npk update` puis `npk outdated`
(nouvelle commande : paquets qui ont une mise à jour). S'il y en a :
bulle « 3 mises à jour disponibles », un clic ouvre le Magasin sur l'onglet
*Mises à jour* (`nexus-store --updates`). Rien n'est installé sans toi.
Désactiver : `su -c "service updates disable"`.

**Fichiers** — miniatures des images (PNG et JPEG), visionneuse et fonds
d'écran JPEG (`nx_load_image`, libjpeg), **glisser-déposer** : faire glisser
des éléments sur un dossier ou un emplacement de la barre latérale les
déplace (Ctrl : copie) ; sur la Corbeille, les y met.

**Wi-Fi** — le paquet `wifi` (pilotes, firmwares, wpa_supplicant, ≈ 9 Mo)
est maintenant installé avec le bureau : icône réseau de la barre → choisir
un réseau → mot de passe → connecté, sans rien installer à la main.

Testé dans une VM vierge : assistant (clavier, bureau), AZERTY en console et à
la connexion, passage US/AZERTY en direct, bulles (test, mise à jour → clic →
Magasin), miniatures, déplacement vers Documents et Corbeille, visionneuse
JPEG, connexion Wi-Fi simulée (`nexus-wifi-test`) avec notification.
