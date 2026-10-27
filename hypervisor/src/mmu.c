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

static uint64_t load_pa64(uint64_t pa)
{
    /* Pull guest-cacheable PTEs/insns to DRAM, then read without EL2 MMU. */
    __asm__ volatile("dc civac, %0" ::"r"(pa) : "memory");
    dsb_sy();
    return *(volatile uint64_t *)(uintptr_t)pa;
}

static uint32_t load_pa32(uint64_t pa)
{
    __asm__ volatile("dc civac, %0" ::"r"(pa) : "memory");
    dsb_sy();
    return *(volatile uint32_t *)(uintptr_t)pa;
}

int guest_va_to_pa(uint64_t va, uint64_t *pa)
{
    uint64_t sctlr = read_sctlr_el1();
    uint64_t ipa;
    uint64_t table_ipa;
    unsigned t0sz;
    unsigned ia_bits;
    int level;

    if (!(sctlr & 1ull)) {
        return s2_ipa_to_pa(va, pa, 0);
    }

    t0sz = (unsigned)(read_tcr_el1() & 63ull);
    if (t0sz < 16u) {
        t0sz = 16u;
    }
    if (t0sz > 39u) {
        t0sz = 39u;
    }
    ia_bits = 64u - t0sz;
    if ((read_tcr_el1() >> 14) & 3ull) {
        /* Guest is 4K only; AAVMF uses 4K. */
        return -1;
    }

    if (ia_bits > 39u) {
        level = 0;
    } else if (ia_bits > 30u) {
        level = 1;
    } else if (ia_bits > 21u) {
        level = 2;
    } else {
        level = 3;
    }

    table_ipa = read_ttbr0_el1() & 0x0000fffffffff000ull;

    for (; level <= 3; level++) {
        unsigned shift = 12u + 9u * (3u - (unsigned)level);
        unsigned idx = (unsigned)((va >> shift) & 0x1ffull);
        uint64_t desc_pa;
        uint64_t desc;
        uint64_t mask;

        if (s2_ipa_to_pa(table_ipa + (uint64_t)idx * 8ull, &desc_pa, 0) != 0) {
            return -1;
        }
        desc = load_pa64(desc_pa);
        if ((desc & 1ull) == 0) {
            return -1;
        }
        if (level < 3 && (desc & 2ull) == 0) {
            mask = (1ull << shift) - 1ull;
            ipa = (desc & 0x0000fffffffff000ull & ~mask) | (va & mask);
            return s2_ipa_to_pa(ipa, pa, 0);
        }
        if (level == 3) {
            ipa = (desc & 0x0000fffffffff000ull) | (va & 0xfffull);
            return s2_ipa_to_pa(ipa, pa, 0);
        }
        table_ipa = desc & 0x0000fffffffff000ull;
    }
    return -1;
}

uint32_t guest_fetch_insn(uint64_t va)
{
    uint64_t pa;
    if (guest_va_to_pa(va, &pa) != 0) {
        return 0;
    }
    return load_pa32(pa);
}

void guest_mem_sync(uint64_t pa, size_t n)
{
    uint64_t ctr;
    uint64_t line;
    uint64_t a;
    uint64_t end;

    if (!n) {
        return;
    }
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line = 4ull << ((ctr >> 16) & 0xfull);
    if (line < 16ull || line > 256ull) {
        line = 64ull;
    }
    a = pa & ~(line - 1ull);
    end = pa + n;
    for (; a < end; a += line) {
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    }
    dsb_sy();
}

#define RAMFB_L3_MAX 4
static uint64_t l3_fb[RAMFB_L3_MAX][512] __attribute__((aligned(4096)));
static uint64_t l3_fb_ipa[RAMFB_L3_MAX];
static unsigned l3_fb_n;
static uint64_t ramfb_ipa0, ramfb_ipa1;

static uint64_t s2_page(uint64_t pa, int ro)
{
    uint64_t ap = ro ? S2_AP_RO : S2_AP_RW;
    return (pa & ~0xfffull) | S2_AF | S2_SH_IS | ap | S2_MEM_NORMAL | 3ull;
}

static uint64_t *s2_l2_for_ipa(uint64_t ipa, unsigned *idx)
{
    const struct platform *p = platform_get();
    unsigned ram_first = (unsigned)((VEIL_GUEST_RAM_IPA >> 21) & 0x1ff);
    uint64_t off;
    unsigned block;

    if (ipa < VEIL_GUEST_RAM_IPA) {
        return 0;
    }
    off = ipa - VEIL_GUEST_RAM_IPA;
    if (off >= p->guest_ram_size) {
        return 0;
    }
    block = (unsigned)(off / L2_BLOCK);
    if (ram_first + block < 512u) {
        *idx = ram_first + block;
        return l2_ram;
    }
    *idx = block - (512u - ram_first);
    if (*idx >= 512u) {
        return 0;
    }
    return l2_ram2;
}

static void s2_tlbi(void)
{
    dsb_sy();
    tlbi_vmalls12e1is();
    dsb_ish();
    isb();
}

static void s2_pte_publish(volatile uint64_t *pte)
{
    guest_mem_sync((uint64_t)(uintptr_t)pte, sizeof(*pte));
}

static int s2_split_ram_block(uint64_t blk)
{
    unsigned idx, p, t;
    uint64_t *l2 = s2_l2_for_ipa(blk, &idx);
    uint64_t old, pa;

    if (!l2) {
        return -1;
    }
    for (t = 0; t < l3_fb_n; t++) {
        if (l3_fb_ipa[t] == blk) {
            return (int)t;
        }
    }
    old = l2[idx];
    if ((old & 3ull) == 3ull) {
        return -1;
    }
    if (l3_fb_n >= RAMFB_L3_MAX) {
        return -1;
    }
    t = l3_fb_n;
    pa = old & 0x0000ffffffe00000ull;
    for (p = 0; p < 512u; p++) {
        l3_fb[t][p] = s2_page(pa + (uint64_t)p * 4096ull, 0);
    }
    l3_fb_ipa[t] = blk;
    guest_mem_sync((uint64_t)(uintptr_t)&l3_fb[t][0], 4096);
    l2[idx] = ((uint64_t)&l3_fb[t][0]) | S2_TABLE;
    s2_pte_publish(&l2[idx]);
    l3_fb_n++;
    return (int)t;
}

int s2_ramfb_watch(uint64_t ipa, size_t n)
{
    uint64_t start, end, blk;
    unsigned t, p;

    if (!n) {
        return -1;
    }
    ramfb_ipa0 = ipa;
    ramfb_ipa1 = ipa + n;
    start = ipa & ~(L2_BLOCK - 1ull);
    end = (ipa + n + L2_BLOCK - 1ull) & ~(L2_BLOCK - 1ull);
    for (blk = start; blk < end; blk += L2_BLOCK) {
        if (s2_split_ram_block(blk) < 0) {
            return -1;
        }
    }
    for (t = 0; t < l3_fb_n; t++) {
        for (p = 0; p < 512u; p++) {
            uint64_t page_ipa = l3_fb_ipa[t] + (uint64_t)p * 4096ull;
            if (page_ipa >= ramfb_ipa0 && page_ipa < ramfb_ipa1) {
                l3_fb[t][p] &= ~S2_AP_RW;
                l3_fb[t][p] |= S2_AP_RO;
            } else {
                l3_fb[t][p] |= S2_AP_RW;
            }
            s2_pte_publish(&l3_fb[t][p]);
        }
    }
    s2_tlbi();
    return 0;
}

int s2_ramfb_fault(uint64_t ipa)
{
    unsigned t, p;

    if (!l3_fb_n || ipa < ramfb_ipa0 || ipa >= ramfb_ipa1) {
        return -1;
    }
    for (t = 0; t < l3_fb_n; t++) {
        if (ipa >= l3_fb_ipa[t] && ipa < l3_fb_ipa[t] + L2_BLOCK) {
            p = (unsigned)((ipa - l3_fb_ipa[t]) >> 12);
            l3_fb[t][p] |= S2_AP_RW;
            s2_pte_publish(&l3_fb[t][p]);
            dsb_sy();
            __asm__ volatile("tlbi ipas2e1is, %0" ::"r"(ipa >> 12) : "memory");
            dsb_ish();
            __asm__ volatile("tlbi vmalle1is" ::: "memory");
            dsb_ish();
            isb();
            return 0;
        }
    }
    return -1;
}

void s2_ramfb_ro(void)
{
    unsigned t, p;

    for (t = 0; t < l3_fb_n; t++) {
        for (p = 0; p < 512u; p++) {
            uint64_t page_ipa = l3_fb_ipa[t] + (uint64_t)p * 4096ull;
            if (page_ipa >= ramfb_ipa0 && page_ipa < ramfb_ipa1) {
                l3_fb[t][p] &= ~S2_AP_RW;
                l3_fb[t][p] |= S2_AP_RO;
                s2_pte_publish(&l3_fb[t][p]);
            }
        }
    }
    if (l3_fb_n) {
        s2_tlbi();
    }
}

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

    write_vtcr_el2(VTCR_T0SZ_40 | VTCR_SL0_L1 | VTCR_SH0_IS | VTCR_TG0_4K |
                   VTCR_PS_40);
    write_vttbr_el2((uint64_t)l1);
    isb();
    tlbi_vmalls12e1is();
    dsb_ish();
    isb();
}
