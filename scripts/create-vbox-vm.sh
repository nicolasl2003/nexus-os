#!/bin/sh
# Crée (ou recrée) la VM VirtualBox « Nexus OS » qui démarre sur l'ISO de dist/.
# À lancer sur la machine hôte (pas dans Docker) : make vbox
#   VM_NAME, VM_RAM (Mo), VM_CPUS, VM_FIRMWARE (bios|efi) sont modifiables.
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT_DIR/config/versions.env"

VM_NAME="${VM_NAME:-Nexus OS}"
VM_RAM="${VM_RAM:-2048}"
VM_CPUS="${VM_CPUS:-2}"
VM_FIRMWARE="${VM_FIRMWARE:-bios}"
ISO="$ROOT_DIR/dist/nexus-os-${NEXUS_VERSION}-x86_64.iso"
RAW_DISK="$ROOT_DIR/disk/nexus-data.img"
VDI="$ROOT_DIR/disk/nexus-data.vdi"

command -v VBoxManage >/dev/null || { echo "VirtualBox (VBoxManage) introuvable" >&2; exit 1; }
[ -f "$ISO" ] || { echo "ISO introuvable : $ISO (lance make iso dist)" >&2; exit 1; }

# Ancienne VM du même nom : on l'éteint et on la supprime (le disque VDI est gardé)
if VBoxManage showvminfo "$VM_NAME" >/dev/null 2>&1; then
    echo "Suppression de l'ancienne VM « $VM_NAME »"
    VBoxManage controlvm "$VM_NAME" poweroff >/dev/null 2>&1 || true
    sleep 1
    VBoxManage storageattach "$VM_NAME" --storagectl SATA --port 1 --medium none >/dev/null 2>&1 || true
    VBoxManage unregistervm "$VM_NAME" --delete
fi

# Disque de données persistant (ext4 étiqueté NEXUS_DATA), converti au format VDI
if [ ! -f "$VDI" ]; then
    [ -f "$RAW_DISK" ] || { echo "Disque introuvable : $RAW_DISK (lance make disk)" >&2; exit 1; }
    echo "Conversion du disque de données en VDI"
    VBoxManage convertfromraw "$RAW_DISK" "$VDI" --format VDI
fi

echo "Création de la VM « $VM_NAME » ($VM_RAM Mo, $VM_CPUS processeurs, $VM_FIRMWARE)"
VBoxManage createvm --name "$VM_NAME" --ostype Linux26_64 --register
VBoxManage modifyvm "$VM_NAME" \
    --memory "$VM_RAM" --cpus "$VM_CPUS" --firmware "$VM_FIRMWARE" \
    --graphicscontroller vmsvga --vram 32 \
    --nic1 nat --nictype1 82540EM \
    --audio-enabled on --audio-controller hda \
    --mouse usbtablet --usbohci on \
    --uart1 0x3F8 4 --uart-mode1 file "$ROOT_DIR/disk/vbox-serial.log" \
    --boot1 dvd --boot2 disk --boot3 none --boot4 none

VBoxManage storagectl "$VM_NAME" --name SATA --add sata --controller IntelAhci --portcount 2
VBoxManage storageattach "$VM_NAME" --storagectl SATA --port 0 --type dvddrive --medium "$ISO"
VBoxManage storageattach "$VM_NAME" --storagectl SATA --port 1 --type hdd --medium "$VDI"

echo "VM prête. Démarrer : VBoxManage startvm \"$VM_NAME\"  (ou depuis l'interface VirtualBox)"
echo "Journal de la console série : disk/vbox-serial.log"
