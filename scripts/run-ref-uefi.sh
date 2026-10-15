#!/usr/bin/env bash
set -euo pipefail

# Stock QEMU virt + AAVMF. No Veil.
# virt has no VGA; without ramfb/virtio-gpu the GTK window stays black
# and BDS falls through to PXE on the serial console.

root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$root/build"

code="${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.fd}"
vars_src="${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}"
vars="$root/build/AAVMF_VARS.fd"
fw="${EFI_FD:-/usr/share/qemu-efi-aarch64/QEMU_EFI.fd}"

qemu=(
    qemu-system-aarch64
    -machine virt,gic-version=3
    -cpu cortex-a53
    -m 512M
    -serial mon:stdio
    -display gtk,gl=off
    -device ramfb
    -device virtio-gpu-pci
    -device virtio-keyboard-pci
    -nic none
)

disk="$root/build/testdisk.img"
if [[ -f "$disk" ]]; then
    qemu+=(
        -drive if=none,id=testdisk,format=raw,file="$disk"
        -device virtio-blk-pci,drive=testdisk,bootindex=1
    )
fi

if [[ -f "$code" && -f "$vars_src" ]]; then
    if [[ ! -f "$vars" ]]; then
        cp "$vars_src" "$vars"
    fi
    qemu+=(
        -drive if=pflash,format=raw,unit=0,readonly=on,file="$code"
        -drive if=pflash,format=raw,unit=1,file="$vars"
    )
else
    qemu+=(-bios "$fw")
fi

exec "${qemu[@]}"
