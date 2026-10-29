#include "aarch64.h"
#include "veil.h"
#include "virtio_board.h"

/*
 * Guest GICv3 distributor + redistributor MMIO (QEMU virt map).
 * CPU interface (ICC_* / HVC patch) is not here — Pi 5 has no VGIC.
 */

#define GICD_CTLR  0x000
#define GICD_TYPER 0x004
#define GICD_IIDR  0x008
#define GICD_PIDR2 0xffe8

#define GICR_CTLR  0x000
#define GICR_TYPER 0x008
#define GICR_WAKER 0x014
#define GICR_PIDR2 0xffe8
#define GICR_SGI   0x10000

static uint32_t gicd_ctlr;
static uint32_t gicd_isenabler[8];
static uint32_t gicr_waker = 0x6; /* ProcessorSleep | ChildrenAsleep */
static uint32_t gicr_ctlr;
static uint32_t gicr_isenabler;

static uint64_t gicr_affinity(void)
{
    uint64_t m = read_mpidr_el1();

    return (m & 0xffull) |
           (((m >> 8) & 0xffull) << 8) |
           (((m >> 16) & 0xffull) << 16) |
           (((m >> 32) & 0xffull) << 24);
}

static uint64_t gicr_typer_val(void)
{
    /* Last redistributor; affinity must match this CPU's MPIDR. */
    return (gicr_affinity() << 32) | (1ull << 4);
}

static uint64_t gicd_read(uint64_t off)
{
    if (off == GICD_CTLR) {
        return gicd_ctlr & ~(1u << 31); /* RWP is RO and never busy */
    }
    if (off == GICD_TYPER) {
        /* 256 SPIs, IDBits=16, A3V, no LPIs. Close to QEMU virt GICv3. */
        return 0x007b0007u;
    }
    if (off == GICD_IIDR) {
        return 0x043b0010u;
    }
    if (off == GICD_PIDR2) {
        return 0x30u;
    }
    if (off >= 0x080 && off < 0x100) {
        return 0xffffffffu; /* IGROUPR: Group-1 NS */
    }
    if (off >= 0x100 && off < 0x180) {
        unsigned n = (unsigned)((off - 0x100) / 4);
        return (n < 8) ? gicd_isenabler[n] : 0;
    }
    return 0;
}

static void gicd_write(uint64_t off, uint64_t val)
{
    if (off == GICD_CTLR) {
        gicd_ctlr = (uint32_t)val & ~(1u << 31);
        return;
    }
    if (off >= 0x100 && off < 0x180) {
        unsigned n = (unsigned)((off - 0x100) / 4);
        if (n < 8) {
            gicd_isenabler[n] |= (uint32_t)val;
        }
        return;
    }
    if (off >= 0x180 && off < 0x200) {
        unsigned n = (unsigned)((off - 0x180) / 4);
        if (n < 8) {
            gicd_isenabler[n] &= ~(uint32_t)val;
        }
    }
}

static uint64_t gicr_read(uint64_t off, unsigned size)
{
    if (off == GICR_CTLR) {
        return gicr_ctlr & ~(1u << 3); /* RWP */
    }
    if (off == GICR_TYPER) {
        uint64_t typer = gicr_typer_val();
        return (size == 8) ? typer : (uint32_t)typer;
    }
    if (off == GICR_TYPER + 4) {
        return (uint32_t)(gicr_typer_val() >> 32);
    }
    if (off == GICR_WAKER) {
        return gicr_waker;
    }
    if (off == GICR_SGI + 0x80) {
        return 0xffffffffu; /* IGROUPR0: PPIs/SGIs Group-1 NS */
    }
    if (off == GICR_SGI + 0x100) {
        return gicr_isenabler;
    }
    if (off == GICR_SGI + 0x200) {
        uint64_t vctl = read_cntv_ctl_el0();
        uint64_t pctl = read_cntp_ctl_el0();
        uint32_t pend = 0;
        if ((vctl & 5u) == 5u && !(vctl & 2u)) {
            pend |= (1u << VEIL_CNTV_INTID);
        }
        if ((pctl & 5u) == 5u && !(pctl & 2u)) {
            pend |= (1u << VEIL_CNTP_INTID);
        }
        return pend;
    }
    if (off == GICR_PIDR2 || off == GICR_SGI + GICR_PIDR2) {
        return 0x30u;
    }
    return 0;
}

static void gicr_write(uint64_t off, uint64_t val)
{
    if (off == GICR_CTLR) {
        gicr_ctlr = (uint32_t)val & ~(1u << 3);
    }
    if (off == GICR_WAKER) {
        /* QEMU: ProcessorSleep=1 keeps ChildrenAsleep; clearing PS wakes. */
        if ((uint32_t)val & 0x2u) {
            gicr_waker = 0x6u;
        } else {
            gicr_waker = 0;
        }
    }
    if (off == GICR_SGI + 0x100) {
        gicr_isenabler |= (uint32_t)val;
    }
    if (off == GICR_SGI + 0x180) {
        gicr_isenabler &= ~(uint32_t)val;
    }
}

void gic_enable_intid(uint32_t intid)
{
    if (intid < 32u) {
        gicr_isenabler |= (1u << intid);
        return;
    }
    {
        unsigned n = intid / 32u;
        unsigned b = intid % 32u;
        if (n < 8u) {
            gicd_isenabler[n] |= (1u << b);
        }
    }
}

uint32_t gicr_isenabler_get(void)
{
    return gicr_isenabler;
}

int gic_dist_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    if (ipa >= VEIL_GICD_IPA && ipa < VEIL_GICD_IPA + VEIL_GICD_SIZE) {
        uint64_t off = ipa - VEIL_GICD_IPA;
        if (is_write) {
            gicd_write(off, *data);
        } else {
            *data = gicd_read(off);
        }
        return 0;
    }
    if (ipa >= VEIL_GICR_IPA && ipa < VEIL_GICR_IPA + VEIL_GICR_DTB_SIZE) {
        uint64_t off = (ipa - VEIL_GICR_IPA) % VEIL_GICR_SIZE;
        if (is_write) {
            gicr_write(off, *data);
        } else {
            *data = gicr_read(off, size);
        }
        return 0;
    }
    return -1;
}
