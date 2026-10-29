#include "fb_priv.h"

/*
 * Host HDMI via the VideoCore mailbox. ANSI/selftest lives in fb_term.c.
 * Guest ramfb GOP blit lives in fb_ramfb.c.
 */

#define MBOX_READ   0x00
#define MBOX_STATUS 0x18
#define MBOX_WRITE  0x20
#define MBOX_FULL   0x80000000u
#define MBOX_EMPTY  0x40000000u
#define MBOX_PROP   8u
#define GPU_ALIAS   0xc0000000u

#define FB_BPP 32u

volatile uint8_t *fb;
uint32_t fb_pitch;
uint32_t fb_pa;
unsigned fb_w, fb_h, fb_bpp;
unsigned term_ox, term_oy;
int fb_ready;
static uint32_t mbox_buf[48] __attribute__((aligned(16)));

static const unsigned fb_mode_w[] = { 640, 800, 1024 };
static const unsigned fb_mode_h[] = { 480, 600, 768 };
#define FB_NMODES 3

static void fb_layout(void)
{
    unsigned tw = TERM_COLS * CELL_W;
    unsigned th = TERM_ROWS * CELL_H;

    term_ox = (fb_w > tw) ? (fb_w - tw) / 2u : 0;
    term_oy = (fb_h > th) ? (fb_h - th) / 2u : 0;
}

static volatile uint32_t *mbox_reg(unsigned off)
{
    return (volatile uint32_t *)(BOARD_MBOX_BASE + off);
}

static int mbox_call(void)
{
    uint32_t addr = ((uint32_t)(uintptr_t)mbox_buf & 0x3fffffffu) | GPU_ALIAS;
    unsigned spins = 0;

    dsb_sy();
    while (*mbox_reg(MBOX_STATUS) & MBOX_FULL) {
        if (++spins > 10000000u) {
            return 0;
        }
    }
    *mbox_reg(MBOX_WRITE) = addr | MBOX_PROP;

    for (;;) {
        spins = 0;
        while (*mbox_reg(MBOX_STATUS) & MBOX_EMPTY) {
            if (++spins > 10000000u) {
                return 0;
            }
        }
        uint32_t v = *mbox_reg(MBOX_READ);
        if ((v & 0xfu) == MBOX_PROP && (v & ~0xfu) == addr) {
            return mbox_buf[1] == 0x80000000u;
        }
    }
}

uint16_t fb_rgb565(uint32_t color)
{
    unsigned r = (color >> 16) & 0xffu;
    unsigned g = (color >> 8) & 0xffu;
    unsigned b = color & 0xffu;

    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void fb_pixel(unsigned x, unsigned y, uint32_t color)
{
    volatile uint8_t *p;

    if (!fb || x >= fb_w || y >= fb_h) {
        return;
    }
    p = fb + (size_t)y * fb_pitch;
    if (fb_bpp == 16) {
        ((volatile uint16_t *)p)[x] = fb_rgb565(color);
    } else if (fb_bpp == 24) {
        p += (size_t)x * 3u;
        p[0] = (uint8_t)color;
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)(color >> 16);
    } else {
        ((volatile uint32_t *)p)[x] = color;
    }
}

void fb_fill_rect(unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                         uint32_t color)
{
    unsigned y, x;

    if (!fb) {
        return;
    }
    if (x1 > fb_w) {
        x1 = fb_w;
    }
    if (y1 > fb_h) {
        y1 = fb_h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (fb_bpp == 16) {
        uint16_t v = fb_rgb565(color);
        for (y = y0; y < y1; y++) {
            volatile uint16_t *p =
                (volatile uint16_t *)(fb + (size_t)y * fb_pitch);
            for (x = x0; x < x1; x++) {
                p[x] = v;
            }
        }
    } else if (fb_bpp == 32) {
        for (y = y0; y < y1; y++) {
            volatile uint32_t *p =
                (volatile uint32_t *)(fb + (size_t)y * fb_pitch);
            for (x = x0; x < x1; x++) {
                p[x] = color;
            }
        }
    } else {
        for (y = y0; y < y1; y++) {
            for (x = x0; x < x1; x++) {
                fb_pixel(x, y, color);
            }
        }
    }
}

void fb_fill(uint32_t color)
{
    fb_fill_rect(0, 0, fb_w, fb_h, color);
}

void fb_flush(void)
{
    if (fb && fb_h && fb_pitch) {
        guest_mem_sync(fb_pa, (size_t)fb_pitch * fb_h);
    }
}

int fb_set_mode(unsigned width, unsigned height)
{
    uint32_t bus;
    unsigned got_w, got_h, depth, bpp, line_px;

    memset(mbox_buf, 0, sizeof(mbox_buf));
    mbox_buf[0] = 47 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x48003; /* set phys */
    mbox_buf[3] = 8;
    mbox_buf[4] = 8;
    mbox_buf[5] = width;
    mbox_buf[6] = height;
    mbox_buf[7] = 0x48004; /* set virt */
    mbox_buf[8] = 8;
    mbox_buf[9] = 8;
    mbox_buf[10] = width;
    mbox_buf[11] = height;
    mbox_buf[12] = 0x48005; /* set depth */
    mbox_buf[13] = 4;
    mbox_buf[14] = 4;
    mbox_buf[15] = FB_BPP;
    mbox_buf[16] = 0x48006; /* pixel order RGB */
    mbox_buf[17] = 4;
    mbox_buf[18] = 4;
    mbox_buf[19] = 1;
    mbox_buf[20] = 0x40001; /* allocate */
    mbox_buf[21] = 8;
    mbox_buf[22] = 8;
    mbox_buf[23] = 0x100000;
    mbox_buf[24] = 0;
    mbox_buf[25] = 0x40003; /* get phys (SET tags echo the request) */
    mbox_buf[26] = 8;
    mbox_buf[27] = 0;
    mbox_buf[28] = 0;
    mbox_buf[29] = 0;
    mbox_buf[30] = 0x40005; /* get depth */
    mbox_buf[31] = 4;
    mbox_buf[32] = 0;
    mbox_buf[33] = 0;
    mbox_buf[34] = 0x40008; /* get pitch */
    mbox_buf[35] = 4;
    mbox_buf[36] = 0;
    mbox_buf[37] = 0;
    mbox_buf[38] = 0;

    if (!mbox_call() || !mbox_buf[23]) {
        hyp_printf("veil: fb %ux%u mailbox failed\n", width, height);
        return -1;
    }

    bus = mbox_buf[23];
    fb_pa = bus & 0x3fffffffu;
    got_w = mbox_buf[28];
    got_h = mbox_buf[29];
    depth = mbox_buf[33];
    fb_pitch = mbox_buf[37];
    if (got_w == 0) {
        got_w = width;
    }
    if (got_h == 0) {
        got_h = height;
    }
    /* Pi 5 often ignores SET depth and echoes GET depth=32 while pitch is
     * 16bpp (3840 = 1920*2). Trust pitch over the depth tag. */
    if (fb_pitch != 0 && got_w != 0 && fb_pitch != got_w * 4u) {
        if (fb_pitch == got_w * 2u) {
            depth = 16;
        } else if (fb_pitch == got_w * 3u) {
            depth = 24;
        } else if (fb_pitch / 2u == 1920u || fb_pitch / 2u == 1280u ||
                   fb_pitch / 2u == 1024u) {
            depth = 16;
            got_w = fb_pitch / 2u;
        } else if (fb_pitch / 3u == 1920u || fb_pitch / 3u == 1280u) {
            depth = 24;
            got_w = fb_pitch / 3u;
        }
    }
    if (depth != 16 && depth != 24 && depth != 32) {
        depth = 32;
    }
    bpp = depth;
    if (fb_pitch == 0) {
        fb_pitch = got_w * (bpp / 8u);
    }
    line_px = fb_pitch / (bpp / 8u);
    if (line_px > got_w) {
        got_w = line_px;
    }
    if (got_w == 1920u && (got_h == 0 || got_h == height || got_h <= 600u)) {
        got_h = 1080;
    }
    if (got_w == 1280u && (got_h == 0 || got_h == height || got_h <= 480u)) {
        got_h = 720;
    }
    if (fb_pa < 0x400000ull) {
        hyp_printf("veil: fb pa %p overlaps hyp/firmware\n", (uint64_t)fb_pa);
        return -1;
    }

    fb = (volatile uint8_t *)(uintptr_t)fb_pa;
    fb_w = got_w;
    fb_h = got_h;
    fb_bpp = bpp;
    fb_layout();
    platform_clip_guest_ram(fb_pa, (size_t)fb_pitch * fb_h);
    fb_ready = 1;
    fb_fill(COL_BG);
    fb_term_after_mode();
    fb_flush();
    hyp_printf("veil: fb pa %p %ux%u pitch %u %ubpp term @%u,%u (asked %ux%u)\n",
               (uint64_t)fb_pa, fb_w, fb_h, fb_pitch, fb_bpp, term_ox, term_oy,
               width, height);
    return 0;
}

int board_skip_testdisk(void)
{
    return 1;
}

int fb_init(void)
{
    unsigned i;

    for (i = FB_NMODES; i-- > 0;) {
        if (fb_set_mode(fb_mode_w[i], fb_mode_h[i]) == 0) {
            return 0;
        }
    }
    return -1;
}
