#!/usr/bin/env bash
set -euo pipefail

# Empty guest hard disk, same recipe as QEMU: qemu-img + virtio-blk-pci.
# Does not overwrite an existing image (an install would live there).

root="$(cd "$(dirname "$0")/.." && pwd)"
img="${DISK:-$root/build/disk.img}"
size="${DISK_SIZE:-8G}"
fmt="${DISK_FORMAT:-raw}"

mkdir -p "$(dirname "$img")"
if [[ -e "$img" ]]; then
    echo "exists: $img"
    qemu-img info "$img" || true
    exit 0
fi

qemu-img create -f "$fmt" "$img" "$size"
echo "created $img ($fmt $size) — attach with make run (virtio-blk-pci)"
