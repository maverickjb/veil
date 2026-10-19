#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
elf="$root/build/veil.elf"
bin="$root/build/veil.bin"
platform="${PLATFORM:-qemu-virt}"
guest="${GUEST:-aavmf}"

if [[ ! -f "$elf" ]]; then
    echo "build/veil.elf missing; run make" >&2
    exit 1
fi

# Interactive run opens the raspi HDMI window. Tests stay headless.
mode="${1:-}"
display_args=()
debug_args=()
if [[ "$mode" == "--test" ]]; then
    display_args=(-display none)
elif [[ "$mode" == "--debug" ]]; then
    display_args=(-display none)
    # gdbstub :1234, wait for attach; log guest faults to build/qemu.log
    mkdir -p "$root/build"
    debug_args=(-s -S -d guest_errors,unimp,int -D "$root/build/qemu.log")
else
    display_args=(-display gtk,gl=off,grab-on-hover=on)
fi

case "$platform" in
    raspi3)
        qemu=(
            qemu-system-aarch64
            -machine raspi3b
            "${display_args[@]}"
            "${debug_args[@]}"
            -serial mon:stdio
            -kernel "$bin"
        )
        ;;
    qemu-virt)
        qemu=(
            qemu-system-aarch64
            -machine virt,virtualization=on,gic-version=3
            -cpu cortex-a76
            -smp 1
            -m 2G
            "${display_args[@]}"
            "${debug_args[@]}"
            -device ramfb
            -device virtio-gpu-pci
            -device virtio-keyboard-pci
            -device qemu-xhci
            -device usb-kbd
            -serial stdio
            -kernel "$elf"
        )
        disk="${DISK:-$root/build/disk.img}"
        iso="${ISO:-}"
        if [[ -z "$iso" && -z "$mode" ]]; then
            shopt -s nullglob
            isos=("$root/build/"*.iso "$root/build/"*.ISO)
            shopt -u nullglob
            if [[ ${#isos[@]} -gt 0 ]]; then
                iso="${isos[0]}"
            fi
        fi
        if [[ -n "$iso" && -f "$iso" ]]; then
            if [[ ! -f "$disk" ]]; then
                DISK="$disk" "$root/scripts/mk-disk.sh"
            fi
            qemu+=(
                -fw_cfg name=opt/org.veil/skip-testdisk,string=1
                -netdev user,id=net0
                -device virtio-net-pci,netdev=net0
                -device virtio-scsi-pci,id=scsi0
                -drive "if=none,id=cd0,media=cdrom,readonly=on,format=raw,file=${iso}"
                -device scsi-cd,drive=cd0
            )
        else
            qemu+=(-nic none)
        fi
        if [[ -f "$disk" ]]; then
            fmt=raw
            case "$disk" in
                *.qcow2) fmt=qcow2 ;;
            esac
            qemu+=(
                -drive "if=none,id=hd0,format=${fmt},file=${disk}"
                -device virtio-blk-pci,drive=hd0
            )
        fi
        ;;
    raspi5)
        echo "QEMU has no raspi5 machine. Built for real hardware:" >&2
        echo "  $bin" >&2
        echo "SD: copy $bin + board/raspi5/config.txt to the boot partition" >&2
        echo "TFTP: make PLATFORM=raspi5 tftp   (QEMU_EFI.fd is inside veil.bin)" >&2
        echo "(debug UART is the JST-SH header next to USB-C, 115200 8n1)." >&2
        if [[ "$mode" == "--test" || "$mode" == "--debug" ]]; then
            exit 1
        fi
        exit 0
        ;;
    *)
        echo "unknown PLATFORM=$platform (raspi3|raspi5|qemu-virt)" >&2
        exit 1
        ;;
esac

if [[ "$mode" == "--debug" ]]; then
    echo "QEMU waiting on gdb :1234  (in another terminal:)" >&2
    echo "  gdb-multiarch -x scripts/gdb-veil.gdb" >&2
fi

if [[ -z "$mode" && "$platform" == "qemu-virt" ]]; then
    echo "UEFI setup: press Esc in THIS terminal during the TianoCore splash." >&2
    echo "(The GTK window often swallows Esc under WSL; serial ConIn does not.)" >&2
    if [[ -n "${iso:-}" && -f "${iso:-}" ]]; then
        echo "Install ISO: $iso" >&2
        echo "Target disk: ${disk:-build/disk.img}" >&2
        echo "Testdisk ESP is hidden; AAVMF should boot the CD-ROM." >&2
        echo "Ubuntu Server TUI is on the GTK window (console=tty0), not serial." >&2
    fi
fi

if [[ "$mode" == "--test" ]]; then
    out="$(mktemp)"
    trap 'rm -f "$out"' EXIT
    if ! timeout --foreground 35 "${qemu[@]}" </dev/null >"$out" 2>&1; then
        :
    fi
    if [[ "$guest" == "mini" ]]; then
        if grep -q "Veil virtio BIOS 0.1" "$out" &&
           grep -q "virtio-console online" "$out"; then
            echo "PASS: mini virtio BIOS on host $platform"
            exit 0
        fi
        echo "FAIL: expected mini BIOS banner on $platform" >&2
    else
        if grep -q "Veil test disk" "$out"; then
            echo "PASS: AAVMF booted testdisk on host $platform"
            exit 0
        fi
        if grep -q "BdsDxe" "$out"; then
            echo "PASS: AAVMF reached BDS on host $platform (no testdisk yet)"
            exit 0
        fi
        echo "FAIL: expected AAVMF BDS or testdisk on $platform" >&2
    fi
    cat "$out" >&2
    exit 1
fi

exec "${qemu[@]}"
