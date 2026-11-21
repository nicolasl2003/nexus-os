# Nexus OS — commandes à lancer depuis le Mac
# Le build se fait dans Docker ; les sources compilées vivent dans un
# volume Docker (sensible à la casse, contrairement au disque du Mac).

IMAGE  := nexus-os-builder
VOLUME := nexus-os-build
DOCKER := docker run --rm -it -p 127.0.0.1:2222:2222 -v "$(CURDIR)":/work -v $(VOLUME):/work/build $(IMAGE)
DOCKER_GUI := docker run --rm -it -e GUI=1 -p 127.0.0.1:2222:2222 -p 127.0.0.1:6080:6080 -p 127.0.0.1:5700:5700 -p 127.0.0.1:5900:5900 -v "$(CURDIR)":/work -v $(VOLUME):/work/build $(IMAGE)
GUI_IMAGE := nexus-os-gui-builder
DOCKER_GUI_BUILD := docker run --rm -e FORCE -v "$(CURDIR)":/work -v $(VOLUME):/work/build $(GUI_IMAGE)
DOCKER_BATCH := docker run --rm -e DATA_DISK_SIZE -e FORCE -v "$(CURDIR)":/work -v $(VOLUME):/work/build $(IMAGE)

.PHONY: all image kernel busybox tools keys rootfs iso packages dist disk disk-reset run run-gui run-native run-native-web run-iso run-iso-gui run-uefi novnc gui-image gui-stack desktop-apps shell clean

all: kernel busybox tools keys packages rootfs iso dist

image:
	docker build -t $(IMAGE) .

kernel: image
	$(DOCKER_BATCH) ./scripts/build-kernel.sh

busybox: image
	$(DOCKER_BATCH) ./scripts/build-busybox.sh

# minisign (signature/vérification des paquets), compilé en statique
tools: image
	$(DOCKER_BATCH) ./scripts/build-minisign.sh

# Paire de clés qui signe le dépôt npk (générée une seule fois dans keys/)
keys: tools
	$(DOCKER_BATCH) ./scripts/make-keys.sh

rootfs: keys
	$(DOCKER_BATCH) ./scripts/build-rootfs.sh

# Construit les paquets npk (ex. : make packages PKG=nano)
packages: keys
	$(DOCKER_BATCH) ./scripts/build-packages.sh $(PKG)

iso: image
	$(DOCKER_BATCH) ./scripts/build-iso.sh

dist: image
	mkdir -p dist
	$(DOCKER_BATCH) sh -c 'cp build/out/bzImage build/out/initramfs.cpio.gz build/out/*.iso dist/ 2>/dev/null; rm -rf dist/repo dist/novnc; cp -r build/repo dist/repo 2>/dev/null; cp -r build/tools/novnc dist/novnc 2>/dev/null; true'

disk: image
	$(DOCKER_BATCH) ./scripts/make-disk.sh

# Efface toutes les données persistantes et recrée un disque vierge
disk-reset: image
	$(DOCKER_BATCH) ./scripts/make-disk.sh --force

run: disk
	$(DOCKER) ./scripts/run-qemu.sh

# Mode graphique : écran visible sur http://localhost:6080
run-gui: disk novnc
	$(DOCKER_GUI) ./scripts/run-qemu.sh

# Directement sur le Mac, avec accélération matérielle (HVF), dans une fenêtre
# macOS ouverte depuis l'application Terminal. Prérequis : scripts/build-qemu-mac.sh
run-native: novnc dist disk
	open scripts/nexus-fenetre.command

# Variante : écran dans le navigateur (http://localhost:6080), plus lent (VNC)
run-native-web: novnc dist disk
	NATIVE_DISPLAY=browser ./scripts/run-native.sh

run-iso-gui: disk novnc
	$(DOCKER_GUI) ./scripts/run-iso.sh bios

# Pile graphique (Wayland) compilée dans Alpine : make gui-stack [C=composant]
gui-image:
	docker build -f Dockerfile.gui -t $(GUI_IMAGE) .

gui-stack: gui-image
	$(DOCKER_GUI_BUILD) ./scripts/build-gui.sh $(C)

# Nos programmes du bureau (barre, menu, fond, paramètres) : dossier desktop/
desktop-apps: gui-image
	$(DOCKER_GUI_BUILD) ./scripts/build-desktop.sh

# Client VNC dans le navigateur (noVNC)
novnc: image
	$(DOCKER_BATCH) ./scripts/get-novnc.sh

run-iso: disk
	$(DOCKER) ./scripts/run-iso.sh bios

run-uefi: disk
	$(DOCKER) ./scripts/run-iso.sh uefi

shell: image
	$(DOCKER) bash

clean:
	rm -rf dist
	-docker volume rm $(VOLUME)
