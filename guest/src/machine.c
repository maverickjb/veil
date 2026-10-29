#include "aarch64.h"
#include "veil.h"
#include "virtio_board.h"

/* Supporting virt-machine devices around the virtio hardware layer. */

#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_FR_TXFE (1u << 7)
#define PL011_FR_RXFE (1u << 4)
#define PL011_IBRD 0x24
#define PL011_FBRD 0x28
#define PL011_LCRH 0x2c
#define PL011_CR   0x30
#define PL011_IMSC 0x38
#define PL011_RIS  0x3c
#define PL011_MIS  0x40
#define PL011_ICR  0x44
#define PL011_RXIM (1u << 4)
#define PL011_RTIM (1u << 6)
#define PL011_RXIS (1u << 4)
#define PL011_RTIS (1u << 6)
#define GICD_CTLR  0x000
#define GICD_TYPER 0x004
#define GICD_IIDR  0x008
#define GICD_PIDR2 0xffe8

#define GICR_CTLR  0x000
#define GICR_IIDR  0x004
#define GICR_TYPER 0x008
#define GICR_WAKER 0x014
#define GICR_PIDR2 0xffe8
#define GICR_SGI   0x10000

#define FW_CFG_SIGNATURE 0x00
#define FW_CFG_ID        0x01
#define FW_CFG_FILE_DIR  0x19
#define FW_CFG_FILE_FIRST 0x20
#define FW_CFG_FILE_RAMFB 0x21
#define FW_CFG_FILE_SMBIOS_TABLES 0x22
#define FW_CFG_FILE_SMBIOS_ANCHOR 0x23
#define FW_CFG_DMA_ENABLE (1u << 1)

#define FW_CFG_DMA_ERROR  (1u << 0)
#define FW_CFG_DMA_READ   (1u << 1)
#define FW_CFG_DMA_SKIP   (1u << 2)
#define FW_CFG_DMA_SELECT (1u << 3)
#define FW_CFG_DMA_WRITE  (1u << 4)

static uint32_t pl011_cr = 0x301;
static uint32_t pl011_ibrd = 1;
static uint32_t pl011_fbrd;
static uint32_t pl011_lcrh = 0x70;
static uint32_t pl011_imsc;
static uint32_t pl011_ris;
static uint8_t pl011_rx[32];
static unsigned pl011_rx_r, pl011_rx_w, pl011_rx_n;
static int pl011_irq;

static uint32_t gicd_ctlr;
static uint32_t gicd_isenabler[8];
static uint32_t gicr_waker = 0x6; /* ProcessorSleep | ChildrenAsleep */
static uint32_t gicr_ctlr;
static uint32_t gicr_isenabler;

static uint16_t fw_sel;
static uint32_t fw_off;
static uint64_t fw_dma;
static uint8_t file_dir[320];
static uint32_t file_dir_len;
static uint8_t ramfb_cfg[28];

static uint32_t be32(uint32_t v)
{
    return __builtin_bswap32(v);
}

static uint64_t be64(uint64_t v)
{
    return __builtin_bswap64(v);
}

static uint16_t be16(uint16_t v)
{
    return __builtin_bswap16(v);
}

static void put_dir_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const uint8_t *fw_payload(uint16_t sel, uint32_t *len)
{
    if (sel == FW_CFG_SIGNATURE) {
        *len = 4;
        return (const uint8_t *)"QEMU";
    }
    if (sel == FW_CFG_ID) {
        static uint32_t id;
        id = be32(FW_CFG_DMA_ENABLE | 1u);
        *len = 4;
        return (const uint8_t *)&id;
    }
    if (sel == FW_CFG_FILE_DIR) {
        *len = file_dir_len;
        return file_dir;
    }
    if (sel == FW_CFG_FILE_FIRST) {
        *len = dtb_size();
        return dtb_blob();
    }
    if (sel == FW_CFG_FILE_RAMFB) {
        *len = sizeof(ramfb_cfg);
        return ramfb_cfg;
    }
    if (sel == FW_CFG_FILE_SMBIOS_TABLES) {
        *len = smbios_tables_size();
        return smbios_tables_blob();
    }
    if (sel == FW_CFG_FILE_SMBIOS_ANCHOR) {
        *len = smbios_anchor_size();
        return smbios_anchor_blob();
    }
    *len = 0;
    return 0;
}

static void fw_cfg_add_file(unsigned idx, uint16_t sel, uint32_t size, const char *name)
{
    uint8_t *e = file_dir + 4 + idx * 64;
    put_dir_be32(e, size);
    e[4] = (uint8_t)(sel >> 8);
    e[5] = (uint8_t)sel;
    memcpy(e + 8, name, strlen(name));
}

static void fw_cfg_build_dir(void)
{
    unsigned n = 0;

    memset(file_dir, 0, sizeof(file_dir));
    fw_cfg_add_file(n++, FW_CFG_FILE_FIRST, dtb_size(), "etc/fdt");
#if BOARD_GUEST_RAMFB
    fw_cfg_add_file(n++, FW_CFG_FILE_RAMFB, sizeof(ramfb_cfg), "etc/ramfb");
#endif
    fw_cfg_add_file(n++, FW_CFG_FILE_SMBIOS_TABLES, smbios_tables_size(),
                    "etc/smbios/smbios-tables");
    fw_cfg_add_file(n++, FW_CFG_FILE_SMBIOS_ANCHOR, smbios_anchor_size(),
                    "etc/smbios/smbios-anchor");
    put_dir_be32(file_dir, n);
    file_dir_len = 4 + 64 * n;
}

static void *guest_ptr(uint64_t ipa, size_t n)
{
    uint64_t pa;
    size_t remain;
    if (s2_ipa_to_pa(ipa, &pa, &remain) || remain < n) {
        return 0;
    }
    /* Guest DRAM is cacheable; EL2 must see/commit through PoC. */
    guest_mem_sync(pa, n);
    return (void *)(uintptr_t)pa;
}

static void fw_dma_run(void)
{
    struct fw_cfg_dma {
        uint32_t control;
        uint32_t length;
        uint64_t address;
    } acc;
    void *p = guest_ptr(fw_dma, sizeof(acc));
    if (!p) {
        hyp_printf("veil: fw_cfg dma desc ipa=%p unreachable\n", fw_dma);
        return;
    }
    memcpy(&acc, p, sizeof(acc));
    uint32_t ctrl = be32(acc.control);
    uint32_t len = be32(acc.length);
    uint64_t addr = be64(acc.address);
    static int dma_log;

    if (!dma_log) {
        dma_log = 1;
        hyp_debug("veil: fw_cfg dma ctrl=0x%x len=%u addr=%p\n", ctrl, len, addr);
    }

    if (ctrl & FW_CFG_DMA_SELECT) {
        fw_sel = (uint16_t)(ctrl >> 16);
        fw_off = 0;
    }

    uint32_t paylen = 0;
    const uint8_t *pay = fw_payload(fw_sel, &paylen);
    uint32_t avail = (fw_off < paylen) ? (paylen - fw_off) : 0;
    uint32_t n = (len < avail) ? len : avail;

    if (ctrl & FW_CFG_DMA_READ) {
        void *dst = guest_ptr(addr, n);
        if (dst && pay) {
            memcpy(dst, pay + fw_off, n);
            guest_mem_sync((uint64_t)(uintptr_t)dst, n);
        } else if (len && !dst) {
            ctrl |= FW_CFG_DMA_ERROR;
        }
        fw_off += n;
    } else if (ctrl & FW_CFG_DMA_WRITE) {
        void *src = guest_ptr(addr, len);
        if (!src) {
            ctrl |= FW_CFG_DMA_ERROR;
        } else if (fw_sel == FW_CFG_FILE_RAMFB) {
            hyp_debug("veil: ramfb dma write off=%u len=%u\n", fw_off, len);
            if (len >= sizeof(ramfb_cfg)) {
                memcpy(ramfb_cfg, src, sizeof(ramfb_cfg));
                fw_off = sizeof(ramfb_cfg);
                if (fb_map_guest_ramfb(ramfb_cfg, sizeof(ramfb_cfg)) != 0) {
                    hyp_printf("veil: guest ramfb map failed\n");
                }
            } else {
                uint32_t w = len;
                if (fw_off + w > sizeof(ramfb_cfg)) {
                    w = (fw_off < sizeof(ramfb_cfg))
                            ? (uint32_t)(sizeof(ramfb_cfg) - fw_off)
                            : 0;
                }
                if (w) {
                    memcpy(ramfb_cfg + fw_off, src, w);
                    fw_off += w;
                    if (fw_off >= sizeof(ramfb_cfg)) {
                        if (fb_map_guest_ramfb(ramfb_cfg, sizeof(ramfb_cfg)) != 0) {
                            hyp_printf("veil: guest ramfb map failed\n");
                        }
                    }
                }
            }
        } else {
            fw_off += len;
        }
    } else if (ctrl & FW_CFG_DMA_SKIP) {
        fw_off += n;
    }

    acc.control = be32(ctrl & FW_CFG_DMA_ERROR);
    memcpy(p, &acc, sizeof(acc));
    guest_mem_sync((uint64_t)(uintptr_t)p, sizeof(acc));
    fw_dma = 0;
}

static void pl011_rx_push(uint8_t b)
{
    if (pl011_rx_n >= sizeof(pl011_rx)) {
        return;
    }
    pl011_rx[pl011_rx_w] = b;
    pl011_rx_w = (pl011_rx_w + 1u) % (unsigned)sizeof(pl011_rx);
    pl011_rx_n++;
}

static void pl011_update_irq(void)
{
    if (pl011_rx_n) {
        pl011_ris |= PL011_RXIS | PL011_RTIS;
    } else {
        pl011_ris &= ~(PL011_RXIS | PL011_RTIS);
    }
    pl011_irq = (pl011_imsc & pl011_ris) != 0;
}

/* Host UART RX → guest PL011. GTK often drops Esc; the QEMU serial window does not. */
static void pl011_pull_host(void)
{
    volatile uint32_t *fr = (volatile uint32_t *)(BOARD_UART_BASE + PL011_FR);
    volatile uint32_t *dr = (volatile uint32_t *)(BOARD_UART_BASE + PL011_DR);
    unsigned n = 0;

    while (pl011_rx_n < sizeof(pl011_rx) && n++ < 32u) {
        if (*fr & PL011_FR_RXFE) {
            break;
        }
        pl011_rx_push((uint8_t)*dr);
    }
    pl011_update_irq();
    if (n) {
        fb_ramfb_present();
    }
}

static uint64_t pl011_read(uint64_t off)
{
    pl011_pull_host();
    switch (off) {
    case PL011_DR:
        if (!pl011_rx_n) {
            return 0;
        }
        {
            uint8_t b = pl011_rx[pl011_rx_r];
            pl011_rx_r = (pl011_rx_r + 1u) % (unsigned)sizeof(pl011_rx);
            pl011_rx_n--;
            pl011_update_irq();
            return b;
        }
    case PL011_FR:
        return PL011_FR_TXFE | (pl011_rx_n ? 0u : PL011_FR_RXFE);
    case PL011_IBRD:
        return pl011_ibrd;
    case PL011_FBRD:
        return pl011_fbrd;
    case PL011_LCRH:
        return pl011_lcrh;
    case PL011_CR:
        return pl011_cr;
    case PL011_IMSC:
        return pl011_imsc;
    case PL011_RIS:
        return pl011_ris;
    case PL011_MIS:
        return pl011_imsc & pl011_ris;
    case 0xfe0:
        return 0x11;
    case 0xfe4:
        return 0x10;
    case 0xfe8:
        return 0x34;
    case 0xfec:
        return 0x00;
    case 0xff0:
        return 0x0d;
    case 0xff4:
        return 0xf0;
    case 0xff8:
        return 0x05;
    case 0xffc:
        return 0xb1;
    default:
        return 0;
    }
}

static void pl011_write(uint64_t off, uint64_t val)
{
    switch (off) {
    case PL011_DR: {
        char c = (char)(val & 0xff);
        static const char *want = "Synchronous Exception";
        static int match;
        uart_putc(c);
        fb_ramfb_present();
        if (c == want[match]) {
            match++;
            if (want[match] == 0) {
                dump_guest_undef();
                match = 0;
            }
        } else {
            match = (c == want[0]);
        }
        break;
    }
    case PL011_IBRD:
        pl011_ibrd = (uint32_t)val;
        break;
    case PL011_FBRD:
        pl011_fbrd = (uint32_t)val;
        break;
    case PL011_LCRH:
        pl011_lcrh = (uint32_t)val;
        break;
    case PL011_CR:
        pl011_cr = (uint32_t)val;
        break;
    case PL011_IMSC:
        pl011_imsc = (uint32_t)val;
        pl011_update_irq();
        break;
    case PL011_ICR:
        pl011_ris &= ~(uint32_t)val;
        pl011_update_irq();
        break;
    default:
        break;
    }
}

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

static uint64_t fw_read(uint64_t off, unsigned size)
{
    if (off == 0) {
        uint32_t len = 0;
        const uint8_t *p = fw_payload(fw_sel, &len);
        uint64_t v = 0;
        unsigned i;
        for (i = 0; i < size; i++) {
            uint8_t b = (fw_off < len && p) ? p[fw_off++] : 0;
            v |= ((uint64_t)b) << (8 * i);
        }
        return v;
    }
    if (off == 8) {
        return be16(fw_sel);
    }
    if (off == 0x10 && size == 8) {
        return be64(fw_dma);
    }
    return 0;
}

static void fw_write(uint64_t off, unsigned size, uint64_t val)
{
    if (off == 0) {
        unsigned i;
        for (i = 0; i < size; i++) {
            uint8_t b = (uint8_t)(val >> (8 * i));
            if (fw_sel == FW_CFG_FILE_RAMFB && fw_off < sizeof(ramfb_cfg)) {
                ramfb_cfg[fw_off++] = b;
                if (fw_off >= sizeof(ramfb_cfg)) {
                    fb_map_guest_ramfb(ramfb_cfg, sizeof(ramfb_cfg));
                }
            } else {
                fw_off++;
            }
        }
        return;
    }
    if (off == 8) {
        fw_sel = be16((uint16_t)val);
        fw_off = 0;
        return;
    }
    if (off == 0x10 && size == 8) {
        fw_dma = be64(val);
        fw_dma_run();
        return;
    }
    if (off == 0x10 && size == 4) {
        fw_dma = (fw_dma & 0xffffffffull) | ((uint64_t)be32((uint32_t)val) << 32);
        return;
    }
    if (off == 0x14 && size == 4) {
        fw_dma = (fw_dma & ~0xffffffffull) | be32((uint32_t)val);
        fw_dma_run();
    }
}

void virt_mmio_init(void)
{
    fw_cfg_build_dir();
    pflash_init();
}

int virt_mmio_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    if (pflash_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (ipa >= VEIL_PL011_IPA && ipa < VEIL_PL011_IPA + VEIL_PL011_SIZE) {
        uint64_t off = ipa - VEIL_PL011_IPA;
        if (is_write) {
            pl011_write(off, *data);
        } else {
            *data = pl011_read(off);
        }
        return 0;
    }
    if (pl031_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
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
    if (ipa >= VEIL_FW_CFG_IPA && ipa < VEIL_FW_CFG_IPA + VEIL_FW_CFG_SIZE) {
        uint64_t off = ipa - VEIL_FW_CFG_IPA;
        if (is_write) {
            fw_write(off, size, *data);
        } else {
            *data = fw_read(off, size);
        }
        return 0;
    }
    if (virtio_mmio_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    /* PCI ECAM: empty slots are 0xFFFFFFFF, not vendor 0. */
    if (ipa >= VEIL_PCI_ECAM_IPA && ipa < VEIL_PCI_ECAM_IPA + VEIL_PCI_ECAM_SIZE) {
        if (!is_write) {
            *data = ~0ull;
        }
        return 0;
    }
    /* Other virt-window probes: RAZ/WI so AAVMF can keep going. */
    if ((ipa >= VEIL_VIRT_MMIO_LO && ipa < VEIL_VIRT_MMIO_HI) || ipa >= 0x400000000ull) {
        if (!is_write) {
            *data = 0;
        }
        return 0;
    }
    return -1;
}

static uint32_t icc_sre = 0x7; /* SRE | DFB | DIB */
static uint32_t icc_ctlr;
static uint32_t icc_pmr = 0xff;
static uint32_t icc_bpr0 = 7;
static uint32_t icc_bpr1 = 7;
static uint32_t icc_igrpen0;
static uint32_t icc_igrpen1;
static uint32_t irq_id;
static uint64_t hooked_vbar;
static uint64_t hooked_vec_pa;
static uint64_t hooked_irq_pa;
static uint32_t saved_vec_insn;
static uint32_t saved_irq_insn;
static uint32_t irq_vec_words[4];
#define ICC_PATCH_MAX 8
static uint64_t icc_patch_va[ICC_PATCH_MAX];
static uint32_t icc_patch_insn[ICC_PATCH_MAX];
static unsigned icc_patch_n;

/*
 * One guest vIRQ:
 *   source ready (CNTV/UART/…)  → irq_pending(id)  (may set HCR.VI)
 *   IAR via patched HVC         → irq_in_service(id)  (drops VI)
 *   EOI                         → irq_idle()          (drops VI)
 * Poll may latch the next id while the handler still runs; VI stays down
 * until PSTATE.I=0 and ELR is outside the VBAR IRQ slots. Holding VI with
 * I=1 freezes this A76 (EL1 stuck on one add after EOI).
 */
enum {
    IRQ_IDLE = 0,
    IRQ_PENDING,
    IRQ_IN_SERVICE,
};

static unsigned irq_st;

static uint32_t pending_intid(void)
{
    uint64_t vctl = read_cntv_ctl_el0();
    uint64_t pctl = read_cntp_ctl_el0();

    {
        int uart = pl011_rx_n != 0;
        int tmr = (vctl & 1u) && !(vctl & 2u) &&
                  ((vctl & 4u) || read_cntvct_el0() >= read_cntv_cval_el0());
        static int prefer_uart;

        /* Leftover RX must not starve CNTV: Shell WaitForKey polls the
         * timer if SerialDxe never registered VEIL_UART_INTID. */
        if (uart && tmr) {
            prefer_uart = !prefer_uart;
            return prefer_uart ? VEIL_UART_INTID : VEIL_CNTV_INTID;
        }
        if (uart) {
            return VEIL_UART_INTID;
        }
        if (tmr) {
            return VEIL_CNTV_INTID;
        }
    }
    if ((pctl & 1u) && !(pctl & 2u) &&
        ((pctl & 4u) || read_cntpct_el0() >= read_cntp_cval_el0())) {
        return VEIL_CNTP_INTID;
    }
    if (pl011_irq) {
        return VEIL_UART_INTID;
    }
    return virtio_pending_intid();
}

static int irq_pin_blocked(void)
{
    uint64_t elr = read_elr_el2();
    int in_irq_vec = hooked_vbar && elr >= hooked_vbar + VEIL_VBAR_IRQ_OFF &&
                     elr < hooked_vbar + VEIL_VBAR_IRQ_END;

    /* PSTATE.I or still in the VBAR IRQ/FIQ/SError slots: do not assert VI. */
    return in_irq_vec || (read_spsr_el2() & (1ull << 7)) != 0;
}

static void irq_apply_pin(void)
{
    uint64_t hcr = read_hcr_el2();

#if VEIL_DEBUG
    {
        static unsigned trace_n;
        uint64_t elr = read_elr_el2();
        uint64_t vbar = read_vbar_el1();
        if (fb_ramfb_allow_virq() && trace_n < 8u) {
            uint64_t spsr = read_spsr_el2();
            uint64_t pa280 = 0;
            uint32_t i280 = 0;

            if (guest_va_to_pa(vbar + VEIL_VBAR_IRQ_OFF, &pa280) == 0) {
                i280 = *(volatile uint32_t *)(uintptr_t)pa280;
            }
            hyp_printf(
                "veil: irqtr %u elr=%p I=%u insn=0x%x vbar=%p i280=0x%x igrp1=%u gicr=0x%x id=%u vi=%u cntv=0x%lx elr1=%p sp=%p\n",
                trace_n, elr, (spsr & (1u << 7)) ? 1u : 0u,
                guest_fetch_insn(elr), vbar, i280, icc_igrpen1,
                gicr_isenabler, irq_id, (hcr & HCR_EL2_VI) ? 1u : 0u,
                (unsigned long)read_cntv_ctl_el0(), read_elr_el1(),
                read_sp_el1());
            trace_n++;
        }
    }
#endif
    /* No vIRQ until the first quiet ramfb store: earlier, AAVMF still
     * uses DefaultExceptionHandler. Waiting for pixels deadlocks
     * (SetMode black, WFI, no timer, no logo). */
    if (irq_pin_blocked()) {
        write_hcr_el2(hcr & ~HCR_EL2_VI);
    } else if (irq_st == IRQ_PENDING && irq_id && fb_ramfb_allow_virq()) {
        static int vi_log;
        write_hcr_el2(hcr | HCR_EL2_VI);
        if (!vi_log) {
            vi_log = 1;
            hyp_debug("veil: inject vi id=%u cntv=0x%lx cntp=0x%lx\n",
                      irq_id, (unsigned long)read_cntv_ctl_el0(),
                      (unsigned long)read_cntp_ctl_el0());
        }
    } else {
        static int no_vi_log;
        write_hcr_el2(hcr & ~HCR_EL2_VI);
        if (fb_ramfb_allow_virq() && !irq_id && !no_vi_log) {
            no_vi_log = 1;
            hyp_debug("veil: no guest timer cntv=0x%lx cntp=0x%lx\n",
                      (unsigned long)read_cntv_ctl_el0(),
                      (unsigned long)read_cntp_ctl_el0());
        }
    }
}

static void irq_pending(uint32_t id)
{
    irq_id = id;
    irq_st = id ? IRQ_PENDING : IRQ_IDLE;
    irq_apply_pin();
}

static void irq_in_service(uint32_t id)
{
    irq_id = 0;
    irq_st = IRQ_IN_SERVICE;
    write_hcr_el2(read_hcr_el2() & ~HCR_EL2_VI);
    (void)id;
}

static void irq_idle(void)
{
    irq_st = IRQ_IDLE;
    /* Leave irq_id: poll may already have latched the next INTID. */
    write_hcr_el2(read_hcr_el2() & ~HCR_EL2_VI);
}

void veil_timer_poll(void)
{
    guest_vbar_hook();
    pl011_pull_host();
    fb_ramfb_present();
    {
        uint64_t vctl = read_cntv_ctl_el0();
        if (fb_ramfb_allow_virq() && (vctl & 3ull) != 0) {
            static int unmask_cntv;
            if (!unmask_cntv) {
                uint64_t frq = read_cntfrq_el0();
                if (frq == 0) {
                    frq = 54000000ull;
                }
                write_cntv_ctl_el0((vctl | 1ull) & ~2ull);
                write_cntv_tval_el0(frq / 200ull);
                isb();
                gicr_isenabler |= (1u << VEIL_CNTV_INTID);
                unmask_cntv = 1;
                hyp_debug("veil: cntv kick was=0x%lx now=0x%lx\n",
                          (unsigned long)vctl,
                          (unsigned long)read_cntv_ctl_el0());
            }
        }
    }
    irq_pending(pending_intid());
    isb();
}

void veil_wfx_idle(struct trapframe *tf)
{
    uint64_t frq = read_cntfrq_el0();
    uint64_t start = read_cntpct_el0();
    uint64_t slice;

    if (frq == 0) {
        frq = 54000000ull;
    }
    slice = frq / 20ull;
    pl011_pull_host();
    while (read_cntpct_el0() - start < slice) {
        veil_timer_poll();
        pl011_pull_host();
        if (pl011_rx_n) {
            break;
        }
    }
    irq_id = pending_intid();
    if (irq_id) {
        irq_st = IRQ_PENDING;
    } else if (irq_st != IRQ_IN_SERVICE) {
        irq_st = IRQ_IDLE;
    }
    if (pl011_rx_n && tf) {
        static int rx_log;
        gicd_isenabler[1] |= (1u << 1); /* SPI 1 = VEIL_UART_INTID */
        irq_id = VEIL_UART_INTID;
        irq_st = IRQ_PENDING;
        tf->spsr &= ~(1ull << 7);
        write_hcr_el2(read_hcr_el2() | HCR_EL2_VI);
        if (!rx_log) {
            rx_log = 1;
            hyp_printf("veil: uart rx 0x%x n=%u\n", pl011_rx[pl011_rx_r],
                       pl011_rx_n);
        }
    }
    fb_ramfb_present();
}

void veil_timer_poll_tf(struct trapframe *tf)
{
    uint32_t i0, i1, i2, i3;

    veil_timer_poll();
    if (!tf) {
        return;
    }
    /*
     * AAVMF PerformanceLib NanoSecondDelay, seen at 0x7da97460 in IRQ:
     *   add x0, x0, x2     // 0x8b020000  step toward deadline in x20
     *   add x1, x1, #1     // 0x91000421  iteration count
     *   cmp x20, x0        // 0xeb00029f
     *   b.hi <loop>        // 0x54….08   taken while x20 > x0
     * x2 is 0 here, so x0 never reaches x20 and EL1 spins with I=1.
     * Set x0=x20 so b.hi falls through; leave x1/x2 alone.
     */
    i0 = guest_fetch_insn(tf->elr);
    i1 = guest_fetch_insn(tf->elr + 4);
    i2 = guest_fetch_insn(tf->elr + 8);
    i3 = guest_fetch_insn(tf->elr + 12);
    if (i0 == 0x8b020000u && i1 == 0x91000421u && i2 == 0xeb00029fu &&
        (i3 & 0xff00001fu) == 0x54000008u && tf->x[2] == 0) {
        static int skip_log;
        if (!skip_log) {
            hyp_debug("veil: skip zero-step delay elr=%p x0=%p x20=%p\n",
                      tf->elr, tf->x[0], tf->x[20]);
            skip_log = 1;
        }
        tf->x[0] = tf->x[20];
    }
}

int veil_timer_pending(void)
{
    return irq_id != 0 || pl011_irq;
}

uint32_t icc_ack_irq(void)
{
    uint32_t id = irq_id;

    if (!id) {
        id = pending_intid();
    }
    if (id == VEIL_CNTV_INTID || id == VEIL_CNTP_INTID ||
        id == VEIL_UART_INTID) {
        irq_in_service(id);
        return id;
    }
    if (id == 48 || id == 49) {
        virtio_ack_irq(id);
        irq_id = 0;
        irq_st = IRQ_IN_SERVICE;
        return id;
    }
    return 1023;
}

void icc_eoi(uint32_t id)
{
    irq_idle();
    hyp_debug("veil: eoi id=%u cntv=0x%lx\n", id,
              (unsigned long)read_cntv_ctl_el0());
    (void)id;
}

int icc_emulate(struct trapframe *tf, uint32_t insn)
{
    unsigned op0 = (insn >> 19) & 3u;
    unsigned op1 = (insn >> 16) & 7u;
    unsigned crn = (insn >> 12) & 15u;
    unsigned crm = (insn >> 8) & 15u;
    unsigned op2 = (insn >> 5) & 7u;
    unsigned rt = insn & 31u;
    int is_mrs = (insn >> 21) & 1;
    uint64_t val = 0;
    int handled = 0;

    if (op0 != 3 || op1 != 0) {
        return -1;
    }

    if (crn == 4 && crm == 6 && op2 == 0) {
        /* ICC_PMR_EL1 */
        handled = 1;
        if (is_mrs) {
            val = icc_pmr;
        } else if (rt != 31) {
            icc_pmr = (uint32_t)tf->x[rt] & 0xffu;
        }
    } else if (crn == 12) {
        handled = 1;
        if (is_mrs) {
            if (crm == 12 && op2 == 5) {
                val = icc_sre | 0x7u;
            } else if (crm == 12 && op2 == 4) {
                val = icc_ctlr;
            } else if (crm == 12 && op2 == 3) {
                val = icc_bpr1;
            } else if (crm == 8 && op2 == 3) {
                val = icc_bpr0;
            } else if (crm == 12 && op2 == 6) {
                val = icc_igrpen0;
            } else if (crm == 12 && op2 == 7) {
                val = icc_igrpen1;
            } else if (crm == 12 && op2 == 0) {
                val = icc_ack_irq();
            } else if (crm == 8 && op2 == 0) {
                val = 1023; /* ICC_IAR0_EL1 */
            } else if (crm == 11 && op2 == 3) {
                val = 0; /* ICC_RPR_EL1 idle */
            } else if (crm == 12 && op2 == 2) {
                val = irq_id ? irq_id : 1023;
            } else {
                val = 0;
            }
        } else if (rt != 31) {
            uint32_t w = (uint32_t)tf->x[rt];
            if (crm == 12 && op2 == 5) {
                icc_sre = w | 0x7u;
            } else if (crm == 12 && op2 == 4) {
                icc_ctlr = w;
            } else if (crm == 12 && op2 == 3) {
                icc_bpr1 = w & 7u;
            } else if (crm == 8 && op2 == 3) {
                icc_bpr0 = w & 7u;
            } else if (crm == 12 && op2 == 6) {
                icc_igrpen0 = w & 1u;
            } else if (crm == 12 && op2 == 7) {
                icc_igrpen1 = w & 1u;
            } else if (crm == 12 && op2 == 1) {
                icc_eoi(w);
            }
            /* DIR/SGI: ignore */
        }
    }

    if (!handled) {
        return -1;
    }
    hyp_debug("veil: icc %s crm=%u op2=%u val=0x%x elr=%p\n",
              is_mrs ? "mrs" : "msr", crm, op2,
              is_mrs ? (unsigned)val : (rt != 31 ? (unsigned)tf->x[rt] : 0u),
              tf->elr);
    if (is_mrs && rt != 31) {
        tf->x[rt] = val;
    }
    return 0;
}

static void patch_hvc(uint64_t va, uint64_t *pa_out, uint32_t *saved)
{
    uint64_t pa = 0;
    uint32_t *p;

    if (guest_va_to_pa(va, &pa) != 0) {
        return;
    }
    p = (uint32_t *)(uintptr_t)pa;
    __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
    dsb_sy();
    if (*p == HVC_ICC_INSN) {
        *pa_out = pa;
        return;
    }
    *saved = *p;
    *p = HVC_ICC_INSN;
    dsb_sy();
    __asm__ volatile("dc cvau, %0" ::"r"(p) : "memory");
    *pa_out = pa;
}

static int insn_is_icc(uint32_t insn)
{
    unsigned op0 = (insn >> 19) & 3u;
    unsigned op1 = (insn >> 16) & 7u;
    unsigned crn = (insn >> 12) & 15u;
    unsigned crm = (insn >> 8) & 15u;
    unsigned op2 = (insn >> 5) & 7u;
    int mrs = (insn >> 21) & 1;

    if (op0 != 3 || op1 != 0 || crn != 12) {
        return 0;
    }
    if ((insn & 0xff800000u) != 0xd5000000u) {
        return 0;
    }
    /* IAR1 / EOIR1 / HPPIR1 / IAR0 / EOIR0 only — not AP0R0/SRE/BPR.
     * Any Rt. AAVMF 2024.02 CpuDxe uses x0: ICC_IAR1_X0_INSN / ICC_EOIR1_X0_INSN. */
    if (mrs && crm == 12 && op2 == 0) {
        return 1;
    }
    if (!mrs && crm == 12 && op2 == 1) {
        return 1;
    }
    if (mrs && crm == 12 && op2 == 2) {
        return 1;
    }
    if (mrs && crm == 8 && op2 == 0) {
        return 1;
    }
    if (!mrs && crm == 8 && op2 == 1) {
        return 1;
    }
    return 0;
}

static uint64_t b_target(uint64_t pc, uint32_t insn)
{
    int64_t imm = (int32_t)((insn & 0x03ffffffu) << 6) >> 6;

    return pc + (uint64_t)(imm * 4);
}

static void patch_icc_at(uint64_t va, uint32_t insn)
{
    uint64_t pa = 0;
    uint32_t *p;
    unsigned i;

    if (!insn_is_icc(insn) || icc_patch_n >= ICC_PATCH_MAX) {
        return;
    }
    for (i = 0; i < icc_patch_n; i++) {
        if (icc_patch_va[i] == va) {
            return;
        }
    }
    if (guest_va_to_pa(va, &pa) != 0) {
        return;
    }
    p = (uint32_t *)(uintptr_t)pa;
    icc_patch_va[icc_patch_n] = va;
    icc_patch_insn[icc_patch_n] = insn;
    *p = HVC_ICC_INSN;
    dsb_sy();
    __asm__ volatile("dc cvau, %0" ::"r"(p) : "memory");
    dsb_ish();
    __asm__ volatile("ic ivau, %0" ::"r"(p) : "memory");
    hyp_debug("veil: icc patch va=%p insn=0x%x\n", va, insn);
    icc_patch_n++;
}

static void scan_icc(uint64_t va0, unsigned n)
{
    unsigned i = 0;

    while (i < n && icc_patch_n < ICC_PATCH_MAX) {
        uint64_t va = va0 + (uint64_t)i * 4u;
        uint64_t pa = 0;
        unsigned j, chunk;

        if (guest_va_to_pa(va, &pa) != 0) {
            i++;
            continue;
        }
        chunk = (unsigned)((4096u - (unsigned)(pa & 0xfffu)) / 4u);
        if (chunk > n - i) {
            chunk = n - i;
        }
        guest_mem_sync(pa, (size_t)chunk * 4u);
        for (j = 0; j < chunk && icc_patch_n < ICC_PATCH_MAX; j++) {
            uint32_t insn = *(volatile uint32_t *)(uintptr_t)(pa + (uint64_t)j * 4u);
            patch_icc_at(va + (uint64_t)j * 4u, insn);
        }
        i += chunk;
    }
}

void guest_vbar_hook(void)
{
    uint64_t vbar;
    unsigned i;

    if (BOARD_GIC_PASSTHROUGH) {
        return;
    }
    vbar = read_vbar_el1();
    if (vbar == hooked_vbar && hooked_vec_pa) {
        return;
    }
    /* VBAR=0 is flash padding; patching it corrupts QEMU_EFI.fd at 0x200. */
    if (vbar < VEIL_GUEST_RAM_IPA) {
        return;
    }
    for (i = 0; i < 4u; i++) {
        irq_vec_words[i] = guest_fetch_insn(vbar + VEIL_VBAR_IRQ_OFF + (uint64_t)i * 4u);
    }
    patch_hvc(vbar + VEIL_VBAR_SYNC_OFF, &hooked_vec_pa, &saved_vec_insn);
    hooked_irq_pa = 0;
    saved_irq_insn = irq_vec_words[0];
    icc_patch_n = 0;
    scan_icc(vbar + VEIL_VBAR_IRQ_OFF, 32);
    for (i = 0; i < 32u; i++) {
        uint64_t va = vbar + VEIL_VBAR_IRQ_OFF + (uint64_t)i * 4u;
        uint32_t insn = guest_fetch_insn(va);
        uint32_t op = insn & 0xfc000000u;

        if (op == 0x14000000u || op == 0x94000000u) {
            uint64_t tgt = b_target(va, insn);
            unsigned k;
            hyp_debug("veil: irq vec %s %p -> %p 0x%x 0x%x 0x%x 0x%x\n",
                      op == 0x94000000u ? "bl" : "b", va, tgt,
                      guest_fetch_insn(tgt), guest_fetch_insn(tgt + 4),
                      guest_fetch_insn(tgt + 8), guest_fetch_insn(tgt + 12));
            scan_icc(tgt, 1024);
            for (k = 0; k < 64u; k++) {
                uint32_t ins2 = guest_fetch_insn(tgt + (uint64_t)k * 4u);
                uint32_t op2 = ins2 & 0xfc000000u;
                if (op2 == 0x14000000u || op2 == 0x94000000u) {
                    uint64_t t2 = b_target(tgt + (uint64_t)k * 4u, ins2);
                    scan_icc(t2, 512);
                }
            }
        }
    }
    /* AAVMF 2024.02 CpuDxe: IAR/EOI (ICC_IAR1_X0_INSN / ICC_EOIR1_X0_INSN)
     * at VBAR+AAVMF_CPUDXE_IAR_OFF. Scan from AAVMF_CPUDXE_IAR_SCAN. */
    {
        unsigned k;
        uint64_t base = vbar + AAVMF_CPUDXE_IAR_SCAN;
        hyp_debug("veil: iar probe @%p = 0x%x loop @%p 0x%x 0x%x 0x%x 0x%x\n",
                  vbar + AAVMF_CPUDXE_IAR_OFF,
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_OFF),
                  vbar + AAVMF_CPUDXE_IAR_NEAR,
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_NEAR),
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_NEAR + 4ull),
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_NEAR + 8ull),
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_NEAR + 12ull));
        for (k = 0; k < 4096u && icc_patch_n < ICC_PATCH_MAX; k++) {
            uint64_t va = base + (uint64_t)k * 4u;
            patch_icc_at(va, guest_fetch_insn(va));
        }
    }
    __asm__ volatile("ic ialluis" ::: "memory");
    dsb_ish();
    isb();
    if (hooked_vec_pa) {
        hooked_vbar = vbar;
        hyp_debug("veil: vbar hook %p irq0=0x%x 0x%x 0x%x 0x%x npatch=%u\n", vbar,
                  irq_vec_words[0], irq_vec_words[1], irq_vec_words[2],
                  irq_vec_words[3], icc_patch_n);
    }
}

int guest_el1_sync_from_hvc(struct trapframe *tf)
{
    uint64_t esr1 = read_esr_el1();
    uint64_t elr1 = read_elr_el1();
    uint32_t ec1 = (uint32_t)((esr1 >> ESR_EC_SHIFT) & ESR_EC_MASK);
    uint32_t insn = 0;
    uint64_t off;
    uint64_t hvc_pc = tf->elr >= 4ull ? tf->elr - 4ull : tf->elr;
    unsigned i;

    /* HVC preferred return is the next insn, so ELR_EL2 is patch VA + 4. */
    for (i = 0; i < icc_patch_n; i++) {
        if (tf->elr == icc_patch_va[i] || hvc_pc == icc_patch_va[i]) {
            if (icc_emulate(tf, icc_patch_insn[i]) == 0) {
                if (tf->elr == icc_patch_va[i]) {
                    tf->elr += 4;
                }
                return 0;
            }
        }
    }

    off = hooked_vbar ? tf->elr - hooked_vbar : ~0ull;
    if (off >= VEIL_VBAR_SYNC_OFF && off < VEIL_VBAR_IRQ_OFF) {
        if (ec1 == 0) {
            insn = guest_fetch_insn(elr1);
            if (insn && icc_emulate(tf, insn) == 0) {
                tf->elr = elr1 + 4;
                tf->spsr = read_spsr_el1();
                return 0;
            }
        }
        hyp_debug("veil: el1 sync via hook esr=0x%lx elr1=%p insn=0x%x hvc=%p\n",
                  (unsigned long)esr1, elr1, insn, tf->elr);
        return -1;
    }

    /* Our HVC imm from a VBAR we have not recorded yet (race on first ICC). */
    if (ec1 == 0) {
        insn = guest_fetch_insn(elr1);
        if (insn && icc_emulate(tf, insn) == 0) {
            tf->elr = elr1 + 4;
            tf->spsr = read_spsr_el1();
            return 0;
        }
    }
    return -1;
}

int psci_call(struct trapframe *tf)
{
    uint32_t fid = (uint32_t)tf->x[0];
    switch (fid) {
    case 0x84000000u: /* PSCI_VERSION */
    case 0xc4000000u:
        tf->x[0] = 0x10001u;
        return 0;
    case 0x84000001u: /* CPU_SUSPEND */
    case 0xc4000001u:
        tf->x[0] = 0;
        return 0;
    case 0x84000002u: /* CPU_OFF */
        tf->x[0] = (uint64_t)(int32_t)(-2); /* DENIED */
        return 0;
    case 0x84000003u: /* CPU_ON */
    case 0xc4000003u:
        tf->x[0] = (uint64_t)(int32_t)(-1);
        return 0;
    case 0x84000004u: /* AFFINITY_INFO */
    case 0xc4000004u:
        tf->x[0] = 0; /* ON */
        return 0;
    case 0x84000005u: /* MIGRATE */
        tf->x[0] = (uint64_t)(int32_t)(-1);
        return 0;
    case 0x84000006u: /* MIGRATE_INFO_TYPE: no Trusted OS */
        tf->x[0] = 2;
        return 0;
    case 0x84000008u: /* SYSTEM_OFF */
        hyp_printf("veil: PSCI SYSTEM_OFF\n");
        halt();
        return 0;
    case 0x84000009u: /* SYSTEM_RESET */
        hyp_printf("veil: PSCI SYSTEM_RESET\n");
        halt();
        return 0;
    case 0x8400000Au: /* PSCI_FEATURES */
        switch ((uint32_t)tf->x[1]) {
        case 0x84000000u:
        case 0x84000001u:
        case 0x84000006u:
        case 0x84000008u:
        case 0x84000009u:
        case 0x8400000Au:
            tf->x[0] = 0;
            break;
        default:
            tf->x[0] = (uint64_t)(int32_t)(-1);
            break;
        }
        return 0;
    default:
        tf->x[0] = (uint64_t)(int32_t)(-1); /* NOT_SUPPORTED */
        return 0;
    }
}
