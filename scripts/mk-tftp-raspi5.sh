#!/usr/bin/env bash
set -euo pipefail

# Stage a Raspberry Pi 5 TFTP root. veil.bin already contains QEMU_EFI.fd
# (linked in); do not put bios/QEMU_EFI.fd on the TFTP share.

root="$(cd "$(dirname "$0")/.." && pwd)"
tftp="${TFTP_ROOT:-$root/build/tftp}"
bin="${BIN:-$root/build/veil.bin}"
cfg="$root/board/raspi5/config.txt"
serial="${SERIAL:-}"

if [[ ! -f "$bin" ]]; then
    echo "missing $bin — run: make PLATFORM=raspi5" >&2
    exit 1
fi

dest="$tftp"
if [[ -n "$serial" ]]; then
    dest="$tftp/$serial"
fi
mkdir -p "$dest"

cp -f "$bin" "$dest/veil.bin"
cp -f "$cfg" "$dest/config.txt"
# EEPROM also looks for this name if config.txt is missing kernel=.
cp -f "$bin" "$dest/kernel_2712.img"

copied_dtb=0
for dtb in \
    "${RPI_DTB:-}" \
    /boot/firmware/bcm2712-rpi-5-b.dtb \
    /boot/bcm2712-rpi-5-b.dtb \
    /usr/lib/linux-image-*/broadcom/bcm2712-rpi-5-b.dtb
do
    if [[ -n "$dtb" && -f "$dtb" ]]; then
        cp -f "$dtb" "$dest/bcm2712-rpi-5-b.dtb"
        mkdir -p "$dest/overlays"
        if [[ -d "$(dirname "$dtb")/overlays" ]]; then
            cp -a "$(dirname "$dtb")/overlays/." "$dest/overlays/"
        fi
        copied_dtb=1
        echo "dtb: $dtb"
        break
    fi
done

echo "TFTP root: $dest"
echo "  veil.bin / kernel_2712.img  (AAVMF is inside this image)"
echo "  config.txt"
if [[ "$copied_dtb" -eq 0 ]]; then
    echo "  bcm2712-rpi-5-b.dtb  MISSING — copy from a Pi OS boot partition" >&2
fi

cat <<EOF

Pi 5 EEPROM (on the board, once):
  sudo rpi-eeprom-config --edit
    BOOT_ORDER=0xf21          # SD then NETWORK (0xf2 = network only)
    TFTP_IP=<this machine>
    TFTP_PREFIX=0             # files at TFTP root, not /<serial>/
  # If TFTP_PREFIX=1, restage with: SERIAL=<8-digit-serial> $0

Example dnsmasq is in scripts/dnsmasq-raspi5.conf.example
Debug UART: JST-SH next to USB-C, 115200 8n1.
EOF
