#include "veil.h"
#include "virtio_board.h"

/*
 * QEMU fw_cfg at VEIL_FW_CFG_IPA. AAVMF discovers the virt machine here:
 *   etc/fdt     — guest DTB
 *   etc/ramfb   — GOP framebuffer cfg; a DMA write calls fb_map_guest_ramfb
 *   etc/smbios/smbios-tables, smbios-anchor
 */

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

void fw_cfg_init(void)
{
    fw_cfg_build_dir();
}

int fw_cfg_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    uint64_t off;

    if (ipa < VEIL_FW_CFG_IPA || ipa >= VEIL_FW_CFG_IPA + VEIL_FW_CFG_SIZE) {
        return -1;
    }
    off = ipa - VEIL_FW_CFG_IPA;
    if (is_write) {
        fw_write(off, size, *data);
    } else {
        *data = fw_read(off, size);
    }
    return 0;
}
