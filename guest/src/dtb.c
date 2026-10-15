#include "veil.h"
#include "virtio_board.h"

/*
 * Guest DTB comes from guest/dts/virt.dts (dtc). This file only copies the
 * blob and patches /memory@40000000/reg to the host's guest RAM size.
 */

#define FDT_MAGIC     0xd00dfeedu
#define FDT_BEGIN     0x1u
#define FDT_END_NODE  0x2u
#define FDT_PROP      0x3u
#define FDT_NOP       0x4u
#define FDT_END       0x9u

extern char veil_dtb_start[];
extern char veil_dtb_end[];

static uint8_t blob[16384];
static uint32_t blob_len;

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t align4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

static void patch_memory_reg(uint64_t ram_size)
{
    uint32_t off_struct = rd_be32(blob + 8);
    uint32_t off_str = rd_be32(blob + 12);
    uint32_t sz_struct = rd_be32(blob + 36);
    uint32_t p = off_struct;
    uint32_t end = off_struct + sz_struct;
    int in_memory = 0;

    while (p + 4 <= end) {
        uint32_t tag = rd_be32(blob + p);
        p += 4;
        if (tag == FDT_BEGIN) {
            const char *name = (const char *)(blob + p);
            in_memory = (name[0] == 'm' && memcmp(name, "memory@40000000", 16) == 0);
            while (p < end && blob[p]) {
                p++;
            }
            p = align4(p + 1);
        } else if (tag == FDT_END_NODE) {
            in_memory = 0;
        } else if (tag == FDT_PROP) {
            uint32_t plen = rd_be32(blob + p);
            uint32_t nameoff = rd_be32(blob + p + 4);
            const char *pname = (const char *)(blob + off_str + nameoff);
            uint8_t *val = blob + p + 8;
            if (in_memory && pname[0] == 'r' && pname[1] == 'e' &&
                pname[2] == 'g' && pname[3] == 0 && plen >= 16) {
                wr_be32(val + 8, (uint32_t)(ram_size >> 32));
                wr_be32(val + 12, (uint32_t)ram_size);
                return;
            }
            p = align4(p + 8 + plen);
        } else if (tag == FDT_NOP) {
            continue;
        } else if (tag == FDT_END) {
            break;
        } else {
            break;
        }
    }
}

const uint8_t *dtb_blob(void)
{
    return blob;
}

uint32_t dtb_size(void)
{
    return blob_len;
}

void dtb_init(void)
{
    uint32_t n = (uint32_t)(veil_dtb_end - veil_dtb_start);
    const struct platform *p = platform_get();

    if (n < 40 || n > sizeof(blob)) {
        blob_len = 0;
        return;
    }
    memcpy(blob, veil_dtb_start, n);
    blob_len = n;
    if (rd_be32(blob) != FDT_MAGIC) {
        blob_len = 0;
        return;
    }
    patch_memory_reg(p->guest_ram_size);
}
