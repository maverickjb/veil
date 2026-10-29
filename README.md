# Veil

EL2 hypervisor that hides a real ARM64 SoC and presents a **QEMU virt**
machine (PL011, GICv3, fw_cfg, virtio-mmio) to the guest. The guest is
QEMU's own ARM UEFI — **AAVMF** / `QEMU_EFI.fd` — not a custom BIOS.

Host and guest maps are different on purpose. Default `make` is
`PLATFORM=qemu-virt`. Real hardware is `PLATFORM=raspi5`.

```
board/raspi5      host SoC (BCM2712); fb.c mailbox, fb_term.c ANSI,
                  fb_ramfb.c guest GOP blit
board/qemu-virt   default host (QEMU virt + ramfb / virtio-gpu)
guest/            presented virt machine
  dts, virtio, mmio.c, pl011, pl031, pflash, fw_cfg, psci
  gic_dist.c, gic_cpu.c (ICC / VBAR HVC hook), irq.c (vIRQ)
bios/             QEMU_EFI.fd (pre-built) and GUEST=mini
hypervisor        EL2: boot, traps, stage-2
```

```
EL1  AAVMF (QEMU_EFI.fd)        virt map: flash 0x0, RAM 0x40000000
EL2  Veil                       virt MMIO ↔ host UART / HDMI / GIC-400
HW   Pi 5 (BCM2712) or QEMU     host map is not the virt map
```

On Pi hosts there is no VGIC. EL1 `ICC_*` UNDEF; Veil patches AAVMF
CpuDxe IAR/EOI to HVC and emulates GICv3. `PLATFORM=qemu-virt` passes
the host GICv3 through.

## Traps

Three timelines, not one `handle_trap` kitchen sink:

| Path | Entry | Work |
|------|--------|------|
| EL1 sync | `veil_handle_trap` | WFX → `guest_wfx`; HVC → `guest_hvc`; SYS64 → `guest_sys64` / `icc_emulate`; DABT → ramfb fault or `guest_mmio` |
| Host IRQ | `el1_irq` / `el2_irq` | ack host (CNTHP / uart10), then `veil_timer_poll_tf` |
| WFI idle | WFX trap | `veil_wfx_idle` (same tick as host IRQ: hook VBAR, PL011 pull, ramfb present, vIRQ) |

`guest_vbar_hook` runs on sync (cheap VBAR compare) and on the tick path.
`VBAR_EL1` is not trapped. Do not poll CNTV from arbitrary DABT.

vIRQ: `idle → pending(id) → in_service → eoi → idle`. `HCR.VI` is not
asserted if `PSTATE.I=1` or EL1 PC is in the VBAR IRQ slot. GOP vIRQ is
gated on `guest_gop_ready()` (first quiet ramfb store, often SetMode
black). UART ConIn is the host UART; HDMI is ramfb GOP, not UART glyphs.

## Build and run

Needs `gcc-aarch64-linux-gnu`, `qemu-system-arm`, and
`device-tree-compiler` (`dtc`) to build `guest/dts/virt.dts`.
Guest UEFI is the pre-built `bios/QEMU_EFI.fd`.

`build/veil.elf` is shared across platforms. After switching
`PLATFORM`, `rm -f build/veil.elf build/veil.bin` or the link may no-op.

Hot-path UART logs share ConIn. Default `VEIL_DEBUG=0`. Restore with
`make VEIL_DEBUG=1`.

```bash
make              # GUEST=aavmf PLATFORM=qemu-virt
make run          # GTK window = TianoCore / ramfb; Ctrl-A x to quit
make test
make testdisk     # GPT + FAT ESP with EFI/BOOT/BOOTAA64.EFI
make disk         # empty 8G raw image (build/disk.img); DISK_SIZE=16G to change
make ref          # stock QEMU virt + AAVMF + testdisk (no Veil)
make debug        # Veil + gdbstub :1234; then: gdb-multiarch -x scripts/gdb-veil.gdb

make GUEST=mini run                 # original tiny virtio BIOS
make PLATFORM=raspi5                # real Pi 5 image
SERIAL=b16438be make PLATFORM=raspi5 tftp   # stage build/tftp/<serial>/
```

Expected on `make run`:

```
veil: el2 hypervisor on qemu-virt (cortex-a76)
veil: host uart 0x0000000009000000 hidden
veil: s2 ram ipa 0x0000000040000000 -> pa 0x0000000040000000 (2032 MiB)
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

A blank hard disk is the QEMU recipe, not another ESP baked into Veil:

```bash
make disk                 # qemu-img create -f raw build/disk.img 8G
# or: qemu-img create -f qcow2 build/disk.qcow2 8G && DISK=build/disk.qcow2 make run
make run
```

On `PLATFORM=qemu-virt` that file is `-device virtio-blk-pci`. AAVMF sees it next to the small virtio-mmio testdisk. Leave it empty for an OS installer to partition; `make disk` will not overwrite an existing image. Pi hosts have no PCI passthrough, so this image is not visible there.

To boot an Ubuntu ISO (same QEMU recipe: virtio-scsi CD + virtio-blk target):

```bash
cp ubuntu-*-arm64.iso build/
make disk          # 8G target; too small for a full desktop — DISK_SIZE=32G for that
make run           # picks up build/*.iso, or: make run ISO=build/ubuntu-....iso
```

`make run` hides the testdisk ESP so AAVMF boots `UEFI QEMU QEMU CD-ROM`. GRUB’s “Try or Install Ubuntu Server” auto-starts after 30s. The installer TUI is on the **GTK window** (`console=tty0`); the terminal is still for Esc / serial.

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
| `0x09020000` | fw_cfg | `etc/fdt`, `etc/ramfb`, `etc/smbios/*` |
| `0x0a000000` | virtio-mmio console | id 3 |
| `0x0a000200` | virtio-mmio block | id 2, embedded testdisk ESP (skipped on raspi5) |
| `0x10000000` | PCI MMIO (32-bit) | qemu-virt only |
| `0x3eff0000` | PCI I/O | qemu-virt only |
| `0x40000000` | RAM | identity on qemu-virt; offset on raspi5 |
| `0x4010000000` | PCI ECAM | qemu-virt only |
| `0x8000000000` | PCI MMIO (64-bit) | qemu-virt only |

Also in the guest DTB: one CPU, PSCI (`hvc`), ARM generic timer, `apb-pclk`.
Guest timer is CNTV PPI 27; UART SPI is INTID 33. Host CNTHP is PPI 26 and
is not a guest interrupt.

## Host devices (`PLATFORM=qemu-virt`)

QEMU attaches these on the host; Veil identity-maps PCI so AAVMF programs them.

| QEMU device | Role |
|-------------|------|
| `ramfb` | scanout / TianoCore GOP |
| `virtio-gpu-pci` | graphics (same as `make ref`) |
| `virtio-keyboard-pci` | keyboard (GTK often drops Esc on WSL) |
| `virtio-blk-pci` | `build/disk.img` when present (`make disk`) |
| `virtio-scsi-pci` + `scsi-cd` | `build/*.iso` on interactive `make run` |
| `virtio-net-pci` | user NAT, only with an ISO |
| `qemu-xhci` + `usb-kbd` | extra keyboard path |
| host PL011 (`-serial stdio`) | UEFI serial ConIn/ConOut; **Esc here opens setup** |

## Host platforms

| `PLATFORM` | Board | How to run |
|------------|-------|------------|
| `qemu-virt` (default) | QEMU `virt` + virtualization | `make run` |
| `raspi5` | BCM2712, real hardware only | SD: copy `veil.bin` + `config.txt`, or TFTP below |

### Raspberry Pi 5

Mailbox HDMI is 1920×1080 RGB565. Guest GOP is fw_cfg `etc/ramfb`
(typically XR24 800×600). `fb_ramfb.c` waits until GOP stores go quiet,
skips an all-black frame (`painted=0`), then convert-on-copy to HDMI.
UART is ConIn/ConOut only — do not paint UART onto HDMI.

`QEMU_EFI.fd` is not a TFTP file. It is linked into `veil.bin`. EEPROM
`TFTP_PREFIX=1` needs the board serial:

```bash
SERIAL=b16438be make PLATFORM=raspi5 tftp   # build/tftp/<serial>/kernel_2712.img
```

On the Pi, set EEPROM `BOOT_ORDER` to include network (`0xf21` or `0xf2`)
and `TFTP_IP` to the server. Serve `build/tftp` with dnsmasq
(`scripts/dnsmasq-raspi5.conf.example`). Also copy `bcm2712-rpi-5-b.dtb`
from a Pi OS boot partition if the script cannot find one. UART is the
JST-SH header next to USB-C, 115200 8n1.

Guest PXE (AAVMF “PXE over IPv4”) is a different path: it needs a virt NIC,
which exists on `PLATFORM=qemu-virt` only.
