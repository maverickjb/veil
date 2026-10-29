#include "aarch64.h"
#include "virt_machine.h"
#include "virtio_board.h"

/* ARM PL031 RTC at VEIL_PL031_IPA. Time is CNTPCT/CNTFRQ plus a guest offset. */

static uint32_t pl031_dr;
static uint32_t pl031_mr;
static uint32_t pl031_cr;
static uint32_t pl031_imsc;

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

int pl031_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    uint64_t off;

    (void)size;
    if (ipa < VEIL_PL031_IPA || ipa >= VEIL_PL031_IPA + VEIL_PL031_SIZE) {
        return -1;
    }
    off = ipa - VEIL_PL031_IPA;
    if (is_write) {
        pl031_write(off, *data);
    } else {
        *data = pl031_read(off);
    }
    return 0;
}
