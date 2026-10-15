#pragma once

#include <stdint.h>

/*
 * Guest-visible IPA map. Not the host SoC.
 * AAVMF (QEMU_EFI.fd) is built for this map.
 */

#define VEIL_BOOT_MAGIC 0x5645494c424f4f54ull /* "VEILBOOT" */

#define VEIL_FLASH_IPA            0x0000000000000000ull
#define VEIL_FLASH_SIZE           0x0000000000200000ull /* 2 MiB QEMU_EFI.fd */
#define VEIL_FLASH_WINDOW         0x0000000008000000ull /* virt NOR window before GIC */
#define VEIL_VARS_IPA             0x0000000004000000ull
#define VEIL_VARS_SIZE            0x0000000000200000ull /* 2 MiB: variable + FTW */

#define VEIL_GICD_IPA             0x0000000008000000ull
#define VEIL_GICD_SIZE            0x0000000000010000ull
#define VEIL_GICR_IPA             0x00000000080a0000ull
#define VEIL_GICR_SIZE            0x0000000000020000ull
#define VEIL_GICR_DTB_SIZE        0x0000000000f60000ull /* QEMU virt redistributor span */

#define VEIL_PL011_IPA            0x0000000009000000ull
#define VEIL_PL011_SIZE           0x0000000000001000ull
#define VEIL_PL031_IPA            0x0000000009010000ull
#define VEIL_PL031_SIZE           0x0000000000001000ull

#define VEIL_FW_CFG_IPA           0x0000000009020000ull
#define VEIL_FW_CFG_SIZE          0x0000000000000018ull

#define VEIL_VIRTIO_CONSOLE_IPA   0x000000000a000000ull
#define VEIL_VIRTIO_BLK_IPA       0x000000000a000200ull
#define VEIL_VIRTIO_MMIO_SIZE     0x200ull
#define VEIL_PCI_ECAM_IPA         0x0000004010000000ull
#define VEIL_PCI_ECAM_SIZE        0x0000000010000000ull
#define VEIL_PCI_MMIO32_IPA       0x0000000010000000ull
#define VEIL_PCI_MMIO32_MAP       0x0000000030000000ull
#define VEIL_PCI64_IPA            0x0000008000000000ull
#define VEIL_PCI64_MAP            0x0000000040000000ull

#define VEIL_GUEST_RAM_IPA        0x0000000040000000ull

#define VEIL_VIRT_MMIO_LO         0x0000000008000000ull
#define VEIL_VIRT_MMIO_HI         0x0000000010000000ull

struct veil_boot_info {
    uint64_t magic;
    uint64_t virtio_base;
    uint64_t virtio_size;
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t ncpus;
};
