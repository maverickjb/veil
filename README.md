# Veil

EL2 hypervisor that hides a real ARM64 SoC and presents a **QEMU virt**
machine (PL011, GICv3, fw_cfg, virtio-mmio) to the guest. The guest is
QEMU's own ARM UEFI — **AAVMF** / `QEMU_EFI.fd` — not a custom BIOS.

Host and guest maps are different on purpose.

```
board/raspi3    host SoC (UART, mailbox FB, linker)
board/raspi5    host SoC (BCM2712, real hardware; no QEMU)
board/qemu-virt default host (QEMU virt + ramfb / virtio-gpu)
guest/          presented virt machine (dts, virtio-mmio, PL011, GIC, fw_cfg)
bios/           guest firmware: QEMU_EFI.fd (pre-built) and GUEST=mini
hypervisor      EL2 core (boot, traps, stage-2)
```

```
EL1  AAVMF (QEMU_EFI.fd)           ← virt map: flash 0x0, RAM 0x40000000
EL2  Veil                          ← translates virt MMIO ↔ BCM UART
HW   Raspberry Pi 3B (BCM2837)     ← RAM at 0x0, UART at 0x3f201000
```

## Stage 1

- Host: QEMU `-M raspi3b`
- Guest firmware: `bios/QEMU_EFI.fd` (pre-built AAVMF, same image QEMU
  uses with `-bios` on `virt`)
- Stage-2: `ipa 0x40000000 → pa 0x01000000`, flash at `ipa 0x0`,
  host UART unmapped
- Emulated for AAVMF: PL011 @ `0x09000000`, GICv3 stub, fw_cfg + DTB,
  virtio-console, PSCI

The tiny virtio BIOS is still there as `GUEST=mini`.

## Build and run

Needs `gcc-aarch64-linux-gnu`, `qemu-system-arm`, and
`device-tree-compiler` (`dtc`) to build `guest/dts/virt.dts`.
Guest UEFI is the pre-built `bios/QEMU_EFI.fd`.

```bash
make              # GUEST=aavmf PLATFORM=qemu-virt
make run          # GTK window = TianoCore / ramfb; Ctrl-A x to quit
make test
make testdisk     # GPT + FAT ESP with EFI/BOOT/BOOTAA64.EFI
make ref          # stock QEMU virt + AAVMF + testdisk (no Veil)
make debug        # Veil + gdbstub :1234; then: gdb-multiarch -x scripts/gdb-veil.gdb

make GUEST=mini run                 # original tiny virtio BIOS
make PLATFORM=raspi3 run            # Pi 3 host in QEMU (HDMI text console)
make PLATFORM=raspi5                # real Pi 5: copy build/veil.bin + board/raspi5/config.txt
```

Expected:

```
veil: el2 hypervisor on qemu-virt (cortex-a76)
veil: host uart 0x0000000009000000 hidden
veil: s2 ram ipa 0x0000000040000000 -> pa 0x0000000040000000 (496 MiB)
veil: AAVMF QEMU_EFI.fd 2048 KiB at ipa 0 pa 0x000000005f400000
UEFI firmware (version 2024.02-2ubuntu0.8 built at ...)
BdsDxe: loading Boot0001 "UEFI Misc Device"
Veil test disk
EFI BOOT\BOOTAA64.EFI
```

`make run` opens a **GTK window** (QEMU ramfb + virtio-gpu). Press **Esc
in the terminal** (not the GTK window) during the splash to open the Boot
Manager. If you wait, BdsDxe boots the embedded virtio-blk ESP and prints
**Veil test disk**.

## Guest devices

These are what AAVMF sees. Host SoC addresses stay unmapped.

| IPA | Device | Notes |
|-----|--------|--------|
| `0x00000000` | CFI NOR flash (2 MiB) | `QEMU_EFI.fd`, AAVMF reset vector |
| `0x04000000` | CFI varstore (2 MiB) | UEFI variables + fault-tolerant write |
| `0x08000000` | GICv3 distributor | emulated on Pi hosts; passed through on qemu-virt |
| `0x080a0000` | GICv3 redistributor | same |
| `0x08080000` | GICv3 ITS | qemu-virt only (MSI for PCI) |
| `0x09000000` | PL011 UART | guest console; RX/TX bridged to the host UART |
| `0x09010000` | PL031 RTC | emulated |
| `0x09020000` | fw_cfg | `etc/fdt`, `etc/ramfb` |
| `0x0a000000` | virtio-mmio console | id 3 |
| `0x0a000200` | virtio-mmio block | id 2, embedded testdisk ESP |
| `0x10000000` | PCI MMIO (32-bit) | qemu-virt only |
| `0x3eff0000` | PCI I/O | qemu-virt only |
| `0x40000000` | RAM | identity on qemu-virt; offset on raspi3/raspi5 |
| `0x4010000000` | PCI ECAM | qemu-virt only |
| `0x8000000000` | PCI MMIO (64-bit) | qemu-virt only |

Also in the guest DTB: one CPU, PSCI (`hvc`), ARM generic timer, `apb-pclk`.

## Host devices (`PLATFORM=qemu-virt`)

QEMU attaches these on the host; Veil identity-maps PCI so AAVMF programs them.

| QEMU device | Role |
|-------------|------|
| `ramfb` | scanout / TianoCore GOP |
| `virtio-gpu-pci` | graphics (same as `make ref`) |
| `virtio-keyboard-pci` | keyboard (GTK often drops Esc on WSL) |
| `qemu-xhci` + `usb-kbd` | extra keyboard path |
| host PL011 (`-serial stdio`) | UEFI serial ConIn/ConOut; **Esc here opens setup** |

## Host platforms

| `PLATFORM` | Board | How to run |
|------------|-------|------------|
| `qemu-virt` (default) | QEMU `virt` + virtualization | `make run` |
| `raspi3` | BCM2837, QEMU `raspi3b` | `make PLATFORM=raspi3 run` |
| `raspi5` | BCM2712, real hardware only | `make PLATFORM=raspi5` then copy `build/veil.bin` |
