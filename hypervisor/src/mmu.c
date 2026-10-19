#include "aarch64.h"
#include "veil.h"

/*
 * Stage-2: guest sees a virt machine. Host SoC windows stay invalid.
 *
 *   IPA 0x00000000  flash (QEMU_EFI.fd)  →  PA &veil_efi_start
 *   IPA 0x40000000  RAM                  →  platform.guest_ram_pa
 *   everything else unmapped (trapped)
 */

#define VTCR_T0SZ_40   24u
#define VTCR_SL0_L1    (1u << 6)
#define VTCR_IRGN0_WB  (1u << 8)
#define VTCR_ORGN0_WB  (1u << 10)
#define VTCR_SH0_IS    (3u << 12)
#define VTCR_TG0_4K    (0u << 14)
#define VTCR_PS_40     (2u << 16)

#define S2_BLOCK_VALID (1ull << 0)
#define S2_TABLE       (3ull << 0)
#define S2_AF          (1ull << 10)
#define S2_SH_IS       (3ull << 8)
#define S2_AP_RO       (1ull << 6)
#define S2_AP_RW       (3ull << 6)
#define S2_MEM_NORMAL  (0xfull << 2)
#define S2_MEM_DEVICE  (0ull << 2)

#define L2_BLOCK       0x200000ull

static uint64_t l1[1024] __attribute__((aligned(8192)));
static uint64_t l2_low[512] __attribute__((aligned(4096)));
static uint64_t l2_ram[512] __attribute__((aligned(4096)));
static uint64_t l2_ram2[512] __attribute__((aligned(4096)));
#if BOARD_GIC_PASSTHROUGH
static uint64_t l2_ecam[512] __attribute__((aligned(4096)));
static uint64_t l2_pci64[512] __attribute__((aligned(4096)));
#endif

static uint64_t s2_block(uint64_t pa)
{
    return pa | S2_AF | S2_SH_IS | S2_AP_RW | S2_MEM_NORMAL | S2_BLOCK_VALID;
}

static uint64_t s2_block_ro(uint64_t pa)
{
    return pa | S2_AF | S2_SH_IS | S2_AP_RO | S2_MEM_NORMAL | S2_BLOCK_VALID;
}

#if BOARD_GIC_PASSTHROUGH
static uint64_t s2_dev_block(uint64_t pa)
{
    return pa | S2_AF | S2_AP_RW | S2_MEM_DEVICE | S2_BLOCK_VALID;
}

static void s2_map_dev(uint64_t *l2, uint64_t ipa, uint64_t size)
{
    unsigned first = (unsigned)((ipa >> 21) & 0x1ff);
    unsigned n = (unsigned)(size / L2_BLOCK);

    for (unsigned i = 0; i < n && (first + i) < 512; i++) {
        l2[first + i] = s2_dev_block(ipa + ((uint64_t)i * L2_BLOCK));
    }
}
#endif

int s2_ipa_to_pa(uint64_t ipa, uint64_t *pa, size_t *remain)
{
    const struct platform *p = platform_get();

    if (veil_efi_start && ipa < VEIL_FLASH_SIZE) {
        *pa = (uint64_t)(uintptr_t)veil_efi_start + ipa;
        if (remain) {
            *remain = (size_t)(VEIL_FLASH_SIZE - ipa);
        }
        return 0;
    }

    if (ipa < VEIL_GUEST_RAM_IPA) {
        return -1;
    }
    uint64_t off = ipa - VEIL_GUEST_RAM_IPA;
    if (off >= p->guest_ram_size) {
        return -1;
    }
    *pa = p->guest_ram_pa + off;
    if (remain) {
        *remain = (size_t)(p->guest_ram_size - off);
    }
    return 0;
}

void s2_init(void)
{
    const struct platform *p = platform_get();
    unsigned ram_l1 = (unsigned)(VEIL_GUEST_RAM_IPA >> 30);
    unsigned ram_first = (unsigned)((VEIL_GUEST_RAM_IPA >> 21) & 0x1ff);
    unsigned ram_blocks = (unsigned)(p->guest_ram_size / L2_BLOCK);

    memset(l1, 0, sizeof(l1));
    memset(l2_low, 0, sizeof(l2_low));
    memset(l2_ram, 0, sizeof(l2_ram));
    memset(l2_ram2, 0, sizeof(l2_ram2));

    if (veil_efi_start) {
        uint64_t flash_pa = (uint64_t)(uintptr_t)veil_efi_start;
        unsigned n = (unsigned)((VEIL_FLASH_SIZE + L2_BLOCK - 1) / L2_BLOCK);
        l1[0] = ((uint64_t)l2_low) | S2_TABLE;
        for (unsigned i = 0; i < n && i < 512; i++) {
            l2_low[i] = s2_block_ro(flash_pa + ((uint64_t)i * L2_BLOCK));
        }
    }

#if BOARD_GIC_PASSTHROUGH
    /* Host GIC + PCI (identity). UART/fw_cfg/virtio-mmio stay trapped. */
    l1[0] = ((uint64_t)l2_low) | S2_TABLE;
    s2_map_dev(l2_low, VEIL_GICD_IPA, 8 * L2_BLOCK);
    s2_map_dev(l2_low, VEIL_PCI_MMIO32_IPA, VEIL_PCI_MMIO32_MAP);
    l1[(unsigned)(VEIL_PCI_ECAM_IPA >> 30)] = ((uint64_t)l2_ecam) | S2_TABLE;
    s2_map_dev(l2_ecam, VEIL_PCI_ECAM_IPA, VEIL_PCI_ECAM_SIZE);
    l1[(unsigned)(VEIL_PCI64_IPA >> 30)] = ((uint64_t)l2_pci64) | S2_TABLE;
    s2_map_dev(l2_pci64, VEIL_PCI64_IPA, VEIL_PCI64_MAP);
#endif

    l1[ram_l1] = ((uint64_t)l2_ram) | S2_TABLE;
    unsigned i = 0;
    for (; i < ram_blocks && (ram_first + i) < 512; i++) {
        l2_ram[ram_first + i] = s2_block(p->guest_ram_pa + ((uint64_t)i * L2_BLOCK));
    }
    if (i < ram_blocks) {
        unsigned n2 = ram_blocks - i;
        l1[ram_l1 + 1] = ((uint64_t)l2_ram2) | S2_TABLE;
        for (unsigned j = 0; j < n2 && j < 512; j++) {
            l2_ram2[j] = s2_block(p->guest_ram_pa + ((uint64_t)(i + j) * L2_BLOCK));
        }
    }

    write_vtcr_el2(VTCR_T0SZ_40 | VTCR_SL0_L1 | VTCR_IRGN0_WB | VTCR_ORGN0_WB |
                   VTCR_SH0_IS | VTCR_TG0_4K | VTCR_PS_40);
    write_vttbr_el2((uint64_t)l1);
    isb();
    tlbi_vmalls12e1is();
    dsb_ish();
    isb();
}
