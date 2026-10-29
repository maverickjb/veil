#include "veil.h"
#include "virtio_board.h"

/*
 * QEMU virt pflash: CFI NOR for UEFI variables at VEIL_VARS_IPA, plus
 * RAZ/WI for the code NOR window below VEIL_FLASH_WINDOW.
 * Bank-width 4, device-width 2, 256 KiB sectors.
 */

#define CFI_SECTOR    0x40000u
#define CFI_WBUF_SIZE 4096u

static uint8_t vars_flash[VEIL_VARS_SIZE];
static uint8_t cfi_wbuf[CFI_WBUF_SIZE];
static uint32_t cfi_cmd;
static unsigned cfi_cycle;
static uint32_t cfi_status = 0x80;
static uint32_t cfi_wcount;
static uint32_t cfi_wbase = ~0u;

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

void pflash_init(void)
{
    memset(vars_flash, 0xff, sizeof(vars_flash));
}

int pflash_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
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
    return -1;
}
