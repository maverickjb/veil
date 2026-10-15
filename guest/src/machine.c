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

#define CFI_SECTOR    0x40000u
#define CFI_WBUF_SIZE 4096u

static uint8_t vars_flash[VEIL_VARS_SIZE];
static uint8_t cfi_wbuf[CFI_WBUF_SIZE];
static uint32_t cfi_cmd;
static unsigned cfi_cycle;
static uint32_t cfi_status = 0x80;
static uint32_t cfi_wcount;
static uint32_t cfi_wbase = ~0u;

/* QEMU virt pflash: bank-width 4, device-width 2, 256 KiB sectors. */
static const uint8_t cfi_table[0x40] = {
    [0x10] = 'Q', [0x11] = 'R', [0x12] = 'Y',
    [0x13] = 0x01, [0x15] = 0x31,
    [0x1b] = 0x45, [0x1c] = 0x55,
    [0x1f] = 0x07, [0x20] = 0x07, [0x21] = 0x0a,
    [0x23] = 0x04, [0x24] = 0x04, [0x25] = 0x04,
    [0x27] = 20, /* 1 MiB per x16 device */
    [0x28] = 0x02,
    [0x2a] = 0x0b,
    [0x2c] = 0x01,
    [0x2d] = 7, /* 8 blocks */
    [0x2f] = 0x00, [0x30] = 0x02, /* 128 KiB / device → 256 KiB bank */
    [0x31] = 'P', [0x32] = 'R', [0x33] = 'I',
    [0x34] = '1', [0x35] = '0',
    [0x3f] = 0x01,
};

static uint32_t pl031_dr;
static uint32_t pl031_mr;
static uint32_t pl031_cr;
static uint32_t pl031_imsc;

static uint32_t pl011_cr = 0x301;
static uint32_t pl011_ibrd = 1;
static uint32_t pl011_fbrd;
static uint32_t pl011_lcrh = 0x70;
static uint32_t pl011_imsc;
static uint8_t pl011_rx[32];
static unsigned pl011_rx_r, pl011_rx_w, pl011_rx_n;

static uint32_t gicd_ctlr;
static uint32_t gicd_isenabler[8];
static uint32_t gicr_waker;
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
    memset(file_dir, 0, sizeof(file_dir));
    put_dir_be32(file_dir, 4);
    fw_cfg_add_file(0, FW_CFG_FILE_FIRST, dtb_size(), "etc/fdt");
    fw_cfg_add_file(1, FW_CFG_FILE_RAMFB, sizeof(ramfb_cfg), "etc/ramfb");
    fw_cfg_add_file(2, FW_CFG_FILE_SMBIOS_TABLES, smbios_tables_size(),
                    "etc/smbios/smbios-tables");
    fw_cfg_add_file(3, FW_CFG_FILE_SMBIOS_ANCHOR, smbios_anchor_size(),
                    "etc/smbios/smbios-anchor");
    file_dir_len = 4 + 64 * 4;
}

static void *guest_ptr(uint64_t ipa, size_t n)
{
    uint64_t pa;
    size_t remain;
    if (s2_ipa_to_pa(ipa, &pa, &remain) || remain < n) {
        return 0;
    }
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
        return;
    }
    memcpy(&acc, p, sizeof(acc));
    uint32_t ctrl = be32(acc.control);
    uint32_t len = be32(acc.length);
    uint64_t addr = be64(acc.address);

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
        } else if (len && !dst) {
            ctrl |= FW_CFG_DMA_ERROR;
        }
        fw_off += n;
    } else if (ctrl & FW_CFG_DMA_WRITE) {
        void *src = guest_ptr(addr, len);
        if (!src) {
            ctrl |= FW_CFG_DMA_ERROR;
        } else if (fw_sel == FW_CFG_FILE_RAMFB) {
            uint32_t w = len;
            hyp_printf("veil: ramfb dma write off=%u len=%u\n", fw_off, len);
            if (fw_off + w > sizeof(ramfb_cfg)) {
                w = (fw_off < sizeof(ramfb_cfg)) ? (uint32_t)(sizeof(ramfb_cfg) - fw_off) : 0;
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
        } else {
            fw_off += len;
        }
    } else if (ctrl & FW_CFG_DMA_SKIP) {
        fw_off += n;
    }

    acc.control = be32(ctrl & FW_CFG_DMA_ERROR);
    memcpy(p, &acc, sizeof(acc));
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

/* Host UART RX → guest PL011. GTK often drops Esc; the QEMU serial window does not. */
static void pl011_pull_host(void)
{
    volatile uint32_t *fr = (volatile uint32_t *)(BOARD_UART_BASE + PL011_FR);
    volatile uint32_t *dr = (volatile uint32_t *)(BOARD_UART_BASE + PL011_DR);
    unsigned n = 0;

    while (pl011_rx_n < sizeof(pl011_rx) && n++ < 16u) {
        if (*fr & PL011_FR_RXFE) {
            break;
        }
        pl011_rx_push((uint8_t)*dr);
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
    case PL011_MIS:
        return 0;
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
        break;
    default:
        break;
    }
}

static uint32_t pl031_now(void)
{
    uint64_t frq = read_cntfrq_el0();
    if (frq == 0) {
        frq = 19200000ull;
    }
    return (uint32_t)(read_cntpct_el0() / frq) + 1600000000u;
}

static uint64_t pl031_read(uint64_t off)
{
    switch (off) {
    case 0x00:
        return pl031_now() + pl031_dr;
    case 0x04:
        return pl031_mr;
    case 0x08:
        return 0;
    case 0x0c:
        return pl031_cr;
    case 0x10:
        return pl031_imsc;
    case 0x14:
    case 0x18:
        return 0;
    case 0xfe0:
        return 0x31;
    case 0xfe4:
        return 0x10;
    case 0xfe8:
        return 0x14;
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

static void pl031_write(uint64_t off, uint64_t val)
{
    switch (off) {
    case 0x08:
        pl031_dr = (uint32_t)val - pl031_now();
        break;
    case 0x04:
        pl031_mr = (uint32_t)val;
        break;
    case 0x0c:
        pl031_cr = (uint32_t)val;
        break;
    case 0x10:
        pl031_imsc = (uint32_t)val;
        break;
    default:
        break;
    }
}

static uint64_t gicd_read(uint64_t off)
{
    if (off == GICD_CTLR) {
        return gicd_ctlr;
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
    if (off >= 0x100 && off < 0x180) {
        unsigned n = (unsigned)((off - 0x100) / 4);
        return (n < 8) ? gicd_isenabler[n] : 0;
    }
    return 0;
}

static void gicd_write(uint64_t off, uint64_t val)
{
    if (off == GICD_CTLR) {
        gicd_ctlr = (uint32_t)val;
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
        return gicr_ctlr;
    }
    if (off == GICR_TYPER) {
        uint64_t typer = (1ull << 4); /* Last, affinity 0 */
        return (size == 8) ? typer : (uint32_t)typer;
    }
    if (off == GICR_TYPER + 4) {
        return 0;
    }
    if (off == GICR_WAKER) {
        return gicr_waker;
    }
    if (off == GICR_SGI + 0x100) {
        return gicr_isenabler;
    }
    if (off == GICR_PIDR2 || off == GICR_SGI + GICR_PIDR2) {
        return 0x30u;
    }
    return 0;
}

static void gicr_write(uint64_t off, uint64_t val)
{
    if (off == GICR_CTLR) {
        gicr_ctlr = (uint32_t)val;
    }
    if (off == GICR_WAKER) {
        /* Clearing ProcessorSleep also clears ChildrenAsleep. */
        gicr_waker = (uint32_t)val & ~0x6u;
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

static uint32_t cfi_lanes(uint8_t b)
{
    return (uint32_t)b | ((uint32_t)b << 16);
}

static void cfi_read_array(void)
{
    cfi_cycle = 0;
    cfi_cmd = 0;
    cfi_wbase = ~0u;
}

static int cfi_status_cmd(uint32_t cmd)
{
    return cmd == 0x10 || cmd == 0x20 || cmd == 0x28 || cmd == 0x40 ||
           cmd == 0x50 || cmd == 0x60 || cmd == 0x70 || cmd == 0xe8;
}

static void cfi_store(uint32_t dest, unsigned size, uint64_t val)
{
    if (dest + size > VEIL_VARS_SIZE) {
        return;
    }
    memcpy(vars_flash + dest, &val, size);
}

static void cfi_erase(uint64_t off)
{
    uint32_t base = (uint32_t)(off & ~(uint64_t)(CFI_SECTOR - 1));
    uint32_t n = CFI_SECTOR;
    if (base >= VEIL_VARS_SIZE) {
        return;
    }
    if (base + n > VEIL_VARS_SIZE) {
        n = (uint32_t)(VEIL_VARS_SIZE - base);
    }
    memset(vars_flash + base, 0xff, n);
}

static uint64_t vars_read(uint64_t off, unsigned size)
{
    uint64_t v = 0;
    unsigned idx = (unsigned)(off >> 2);

    if (cfi_cmd == 0x98) {
        return (idx < sizeof(cfi_table)) ? cfi_lanes(cfi_table[idx]) : 0;
    }
    if (cfi_cmd == 0x90) {
        if (idx == 0) {
            return cfi_lanes(0x89);
        }
        if (idx == 1) {
            return cfi_lanes(0x18);
        }
        return 0;
    }
    if (cfi_status_cmd(cfi_cmd)) {
        return cfi_lanes((uint8_t)cfi_status);
    }
    if (off + size <= VEIL_VARS_SIZE) {
        memcpy(&v, vars_flash + off, size);
    }
    return v;
}

static void vars_write(uint64_t off, unsigned size, uint64_t val)
{
    uint8_t cmd = (uint8_t)val;

    if (cfi_cycle == 0) {
        switch (cmd) {
        case 0x00:
        case 0xff:
        case 0xf0:
            cfi_read_array();
            return;
        case 0x10:
        case 0x40:
            cfi_cmd = cmd;
            cfi_cycle = 1;
            return;
        case 0x20:
        case 0x28:
            cfi_erase(off);
            cfi_status |= 0x80;
            cfi_cmd = cmd;
            cfi_cycle = 1;
            return;
        case 0x50:
            cfi_status = 0;
            cfi_read_array();
            return;
        case 0x70:
        case 0x90:
            cfi_cmd = cmd;
            cfi_cycle = 0;
            return;
        case 0x60:
        case 0x98:
        case 0xe8:
            if (cmd == 0xe8) {
                cfi_status |= 0x80;
            }
            cfi_cmd = cmd;
            cfi_cycle = 1;
            return;
        default:
            return;
        }
    }

    if (cfi_cycle == 1) {
        if (cfi_cmd == 0x10 || cfi_cmd == 0x40) {
            cfi_store((uint32_t)off, size, val);
            cfi_status |= 0x80;
            cfi_cycle = 0;
            return;
        }
        if (cfi_cmd == 0x20 || cfi_cmd == 0x28 || cfi_cmd == 0x60) {
            if (cmd == 0xff) {
                cfi_read_array();
            } else {
                cfi_cycle = 0;
                cfi_status |= 0x80;
            }
            return;
        }
        if (cfi_cmd == 0xe8) {
            cfi_wcount = (uint32_t)val & 0xffffu;
            cfi_cycle = 2;
            return;
        }
        if (cfi_cmd == 0x98 && cmd == 0xff) {
            cfi_read_array();
            return;
        }
        cfi_read_array();
        return;
    }

    if (cfi_cycle == 2 && cfi_cmd == 0xe8) {
        if (cfi_wbase == ~0u) {
            cfi_wbase = (uint32_t)off & ~(CFI_WBUF_SIZE - 1u);
            if (cfi_wbase + CFI_WBUF_SIZE <= VEIL_VARS_SIZE) {
                memcpy(cfi_wbuf, vars_flash + cfi_wbase, CFI_WBUF_SIZE);
            }
        }
        if (cfi_wbase != ~0u && off >= cfi_wbase &&
            off + size <= cfi_wbase + CFI_WBUF_SIZE) {
            memcpy(cfi_wbuf + (off - cfi_wbase), &val, size);
        }
        cfi_status |= 0x80;
        if (cfi_wcount == 0) {
            cfi_cycle = 3;
        } else {
            cfi_wcount--;
        }
        return;
    }

    if (cfi_cycle == 3 && cfi_cmd == 0xe8) {
        if (cmd == 0xd0 && cfi_wbase != ~0u &&
            cfi_wbase + CFI_WBUF_SIZE <= VEIL_VARS_SIZE) {
            memcpy(vars_flash + cfi_wbase, cfi_wbuf, CFI_WBUF_SIZE);
        }
        cfi_status |= 0x80;
        cfi_cycle = 0;
        cfi_wbase = ~0u;
        return;
    }

    cfi_read_array();
}

void virt_mmio_init(void)
{
    fw_cfg_build_dir();
    memset(vars_flash, 0xff, sizeof(vars_flash));
}

int virt_mmio_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    if (ipa >= VEIL_VARS_IPA && ipa < VEIL_VARS_IPA + VEIL_VARS_SIZE) {
        uint64_t off = ipa - VEIL_VARS_IPA;
        if (is_write) {
            vars_write(off, size, *data);
        } else {
            *data = vars_read(off, size);
        }
        return 0;
    }
    if (ipa < VEIL_FLASH_WINDOW) {
        if (!is_write) {
            *data = ~0ull;
        }
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
    if (ipa >= VEIL_PL031_IPA && ipa < VEIL_PL031_IPA + VEIL_PL031_SIZE) {
        uint64_t off = ipa - VEIL_PL031_IPA;
        if (is_write) {
            pl031_write(off, *data);
        } else {
            *data = pl031_read(off);
        }
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
    if (ipa >= VEIL_GICR_IPA && ipa < VEIL_GICR_IPA + VEIL_GICR_SIZE) {
        uint64_t off = ipa - VEIL_GICR_IPA;
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

#define HVC_ICC_IMM 0x5645u
#define HVC_ICC_INSN (0xD4000002u | (HVC_ICC_IMM << 5))

static uint32_t icc_sre = 0x7; /* SRE | DFB | DIB */
static uint32_t icc_ctlr;
static uint32_t icc_pmr = 0xff;
static uint32_t icc_bpr0 = 7;
static uint32_t icc_bpr1 = 7;
static uint32_t icc_igrpen0;
static uint32_t icc_igrpen1;
static uint32_t timer_intid;
static uint64_t hooked_vbar;
static uint64_t hooked_vec_pa;
static uint32_t saved_vec_insn;

void veil_timer_poll(void)
{
    uint32_t id = 0;
    uint64_t vctl = read_cntv_ctl_el0();
    uint64_t pctl = read_cntp_ctl_el0();
    if ((vctl & 1u) && !(vctl & 2u) && read_cntvct_el0() >= read_cntv_cval_el0()) {
        id = 27;
    } else if ((pctl & 1u) && !(pctl & 2u) && read_cntpct_el0() >= read_cntp_cval_el0()) {
        id = 30;
    }
    timer_intid = id;
    uint64_t hcr = read_hcr_el2();
    if (id || virtio_pending_intid()) {
        write_hcr_el2(hcr | HCR_EL2_VI);
    } else {
        write_hcr_el2(hcr & ~HCR_EL2_VI);
    }
    isb();
}

int veil_timer_pending(void)
{
    return timer_intid != 0;
}

uint32_t icc_ack_irq(void)
{
    uint32_t id = timer_intid;
    if (id) {
        timer_intid = 0;
        return id;
    }
    uint32_t vid = virtio_pending_intid();
    if (vid) {
        virtio_ack_irq(vid);
        return vid;
    }
    return 1023;
}

void icc_eoi(uint32_t id)
{
    (void)id;
    veil_timer_poll();
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
                val = 0xff; /* ICC_RPR_EL1 */
            } else if (crm == 12 && op2 == 2) {
                val = 1023; /* ICC_HPPIR1_EL1 */
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
    if (is_mrs && rt != 31) {
        tf->x[rt] = val;
    }
    return 0;
}

void guest_vbar_hook(void)
{
    uint64_t vbar;

    if (BOARD_GIC_PASSTHROUGH) {
        return;
    }
    vbar = read_vbar_el1();
    uint64_t pa = 0;
    uint32_t *p;

    if (vbar < VEIL_GUEST_RAM_IPA || vbar == hooked_vbar) {
        return;
    }
    if (s2_ipa_to_pa(vbar + 0x200, &pa, 0) != 0) {
        return;
    }
    p = (uint32_t *)(uintptr_t)pa;
    if (*p == HVC_ICC_INSN) {
        hooked_vbar = vbar;
        hooked_vec_pa = pa;
        return;
    }
    saved_vec_insn = *p;
    *p = HVC_ICC_INSN;
    dsb_sy();
    __asm__ volatile("ic ialluis" ::: "memory");
    dsb_ish();
    isb();
    hooked_vbar = vbar;
    hooked_vec_pa = pa;
}

int guest_el1_sync_from_hvc(struct trapframe *tf)
{
    uint64_t esr1 = read_esr_el1();
    uint64_t elr1 = read_elr_el1();
    uint32_t ec1 = (uint32_t)((esr1 >> ESR_EC_SHIFT) & ESR_EC_MASK);
    uint64_t pa = 0;
    uint32_t insn = 0;

    if (ec1 == 0 && s2_ipa_to_pa(elr1, &pa, 0) == 0) {
        insn = *(volatile uint32_t *)(uintptr_t)pa;
        if (icc_emulate(tf, insn) == 0) {
            tf->elr = elr1 + 4;
            tf->spsr = read_spsr_el1();
            return 0;
        }
        hyp_printf("veil: el1 undef insn=0x%x elr=%p esr=0x%lx\n",
                   insn, elr1, (unsigned long)esr1);
    }

    if (hooked_vec_pa && saved_vec_insn) {
        *(volatile uint32_t *)(uintptr_t)hooked_vec_pa = saved_vec_insn;
        dsb_sy();
        __asm__ volatile("ic ialluis" ::: "memory");
        dsb_ish();
        isb();
        tf->elr = hooked_vbar + 0x200;
        hooked_vbar = 0;
        hooked_vec_pa = 0;
        return 0;
    }

    hyp_printf("veil: el1 sync via hook esr=0x%lx elr=%p insn=0x%x\n",
               (unsigned long)esr1, elr1, insn);
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
    case 0x84000002u: /* CPU_OFF */
        tf->x[0] = 0;
        return 0;
    case 0x84000005u: /* SYSTEM_OFF */
    case 0x84000006u: /* SYSTEM_RESET */
        hyp_printf("veil: guest requested poweroff/reset\n");
        halt();
        return 0;
    default:
        tf->x[0] = (uint64_t)(int32_t)(-1); /* NOT_SUPPORTED */
        return 0;
    }
}
