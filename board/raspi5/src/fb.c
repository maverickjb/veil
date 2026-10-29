#include "board.h"
#include "aarch64.h"
#include "veil.h"

/*
 * Host HDMI via the VideoCore mailbox framebuffer (same property tags
 * rpi5-uefi DisplayDxe uses). Pi 5 firmware only documents a basic/test
 * FB; 640x480 is the reliable mode, 800x600 and 1024x768 are requested
 * the same way and may be clamped.
 */

#define MBOX_READ   0x00
#define MBOX_STATUS 0x18
#define MBOX_WRITE  0x20
#define MBOX_FULL   0x80000000u
#define MBOX_EMPTY  0x40000000u
#define MBOX_PROP   8u
#define GPU_ALIAS   0xc0000000u

#define FB_BPP      32u
#define GLYPH_W     8u
#define GLYPH_H     8u
/* VGA-like 8x16 cells: 8x8 font, 2x wide and 4x tall. */
#define SCALE_X     2u
#define SCALE_Y     4u
#define CELL_W      (GLYPH_W * SCALE_X)
#define CELL_H      (GLYPH_H * SCALE_Y)
#define TERM_COLS   80u
#define TERM_ROWS   25u

#define COL_BG      0x00101828u
#define COL_FG      0x00d8d8d0u
#define COL_HD      0x00f0c040u

static const unsigned fb_mode_w[] = { 640, 800, 1024 };
static const unsigned fb_mode_h[] = { 480, 600, 768 };
#define FB_NMODES 3

#include "font8x8.inc"

static volatile uint8_t *fb;
static uint32_t fb_pitch;
static uint32_t fb_pa;
static unsigned fb_w, fb_h, fb_bpp;
static unsigned term_ox, term_oy;
static unsigned cur_c, cur_r;
static unsigned sav_c, sav_r;
static unsigned dirty_r0, dirty_r1;
static unsigned dirty_c0, dirty_c1;
static unsigned dirty_n;
static int fb_ready;
static uint32_t mbox_buf[48] __attribute__((aligned(16)));

enum {
    ANSI_NORM = 0,
    ANSI_ESC,
    ANSI_CSI,
    ANSI_IGN
};
static int ansi_st;
static unsigned ansi_arg[16];
static unsigned ansi_nargs;
static unsigned ansi_acc;
static int ansi_in_num;
static unsigned sgr_fg_i = 7;
static unsigned sgr_bg_i;
static int sgr_fg_bright;
static int sgr_bg_bright;
static int sgr_bold;
static int sgr_rev;

static unsigned fb_cols(void)
{
    return TERM_COLS;
}

static unsigned fb_rows(void)
{
    return TERM_ROWS;
}

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

static void fb_dirty_reset(void)
{
    dirty_r0 = 1;
    dirty_r1 = 0;
    dirty_c0 = 0;
    dirty_c1 = 0;
    dirty_n = 0;
}

static void fb_mark_dirty(unsigned col, unsigned row, unsigned cols, unsigned rows)
{
    unsigned r1 = row + rows;
    unsigned c1 = col + cols;

    if (row >= TERM_ROWS) {
        return;
    }
    if (r1 > TERM_ROWS) {
        r1 = TERM_ROWS;
    }
    if (c1 > TERM_COLS) {
        c1 = TERM_COLS;
    }
    if (dirty_r0 >= dirty_r1) {
        dirty_r0 = row;
        dirty_r1 = r1;
        dirty_c0 = col;
        dirty_c1 = c1;
    } else {
        if (row < dirty_r0) {
            dirty_r0 = row;
        }
        if (r1 > dirty_r1) {
            dirty_r1 = r1;
        }
        if (col < dirty_c0) {
            dirty_c0 = col;
        }
        if (c1 > dirty_c1) {
            dirty_c1 = c1;
        }
    }
    dirty_n++;
}

static void fb_flush_dirty(void)
{
    unsigned y, y0, y1, bytes;
    uint64_t xoff;

    if (!fb || dirty_r0 >= dirty_r1 || dirty_c0 >= dirty_c1) {
        fb_dirty_reset();
        return;
    }
    y0 = term_oy + dirty_r0 * CELL_H;
    y1 = term_oy + dirty_r1 * CELL_H;
    xoff = (uint64_t)(term_ox + dirty_c0 * CELL_W) * (fb_bpp / 8u);
    bytes = (dirty_c1 - dirty_c0) * CELL_W * (fb_bpp / 8u);
    if (y1 > fb_h) {
        y1 = fb_h;
    }
    for (y = y0; y < y1; y++) {
        uint64_t a = (fb_pa + (uint64_t)y * fb_pitch + xoff) & ~63ull;
        uint64_t end = fb_pa + (uint64_t)y * fb_pitch + xoff + bytes;
        for (; a < end; a += 64ull) {
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        }
    }
    dsb_sy();
    fb_dirty_reset();
}

static void fb_flush(void)
{
    fb_dirty_reset();
    if (fb && fb_h && fb_pitch) {
        guest_mem_sync(fb_pa, (size_t)fb_pitch * fb_h);
    }
}

static uint16_t fb_rgb565(uint32_t color)
{
    unsigned r = (color >> 16) & 0xffu;
    unsigned g = (color >> 8) & 0xffu;
    unsigned b = color & 0xffu;

    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void fb_pixel(unsigned x, unsigned y, uint32_t color)
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

static void fb_fill_rect(unsigned x0, unsigned y0, unsigned x1, unsigned y1,
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

static void fb_fill(uint32_t color)
{
    fb_fill_rect(0, 0, fb_w, fb_h, color);
}

static void fb_fill_term(uint32_t color)
{
    fb_fill_rect(term_ox, term_oy, term_ox + TERM_COLS * CELL_W,
                 term_oy + TERM_ROWS * CELL_H, color);
    fb_mark_dirty(0, 0, TERM_COLS, TERM_ROWS);
}

static uint32_t ansi_rgb(unsigned idx, int bright)
{
    static const uint32_t dim[8] = {
        0x00000000u, 0x00aa0000u, 0x0000aa00u, 0x00aa5500u,
        0x000000aau, 0x00aa00aau, 0x0000aaaau, 0x00aaaaaau
    };
    static const uint32_t lit[8] = {
        0x00555555u, 0x00ff5555u, 0x0055ff55u, 0x00ffff55u,
        0x005555ffu, 0x00ff55ffu, 0x0055ffffu, 0x00ffffffu
    };

    return (bright ? lit : dim)[idx & 7u];
}

static uint32_t sgr_fg(void)
{
    uint32_t fg = ansi_rgb(sgr_fg_i, sgr_bold || sgr_fg_bright);
    uint32_t bg = ansi_rgb(sgr_bg_i, sgr_bg_bright);

    return sgr_rev ? bg : fg;
}

static uint32_t sgr_bg(void)
{
    uint32_t fg = ansi_rgb(sgr_fg_i, sgr_bold || sgr_fg_bright);
    uint32_t bg = ansi_rgb(sgr_bg_i, sgr_bg_bright);

    return sgr_rev ? fg : bg;
}

static void fb_glyph(unsigned col, unsigned row, char ch, uint32_t fg, uint32_t bg)
{
    unsigned idx = ((unsigned char)ch < 32 || (unsigned char)ch > 127)
                       ? 0
                       : ((unsigned char)ch - 32);
    unsigned x0 = term_ox + col * CELL_W;
    unsigned y0 = term_oy + row * CELL_H;
    unsigned gy, gx, sy;

    if (col >= TERM_COLS || row >= TERM_ROWS) {
        return;
    }
    if (fb_bpp == 16 && x0 + CELL_W <= fb_w && y0 + CELL_H <= fb_h) {
        uint16_t fg16 = fb_rgb565(fg);
        uint16_t bg16 = fb_rgb565(bg);
        uint16_t pix[GLYPH_W];

        for (gy = 0; gy < GLYPH_H; gy++) {
            uint8_t bits = font8x8[idx][gy];
            for (gx = 0; gx < GLYPH_W; gx++) {
                pix[gx] = (bits & (1u << (7u - gx))) ? fg16 : bg16;
            }
            for (sy = 0; sy < SCALE_Y; sy++) {
                volatile uint16_t *p =
                    (volatile uint16_t *)(fb + (size_t)(y0 + gy * SCALE_Y + sy) *
                                                   fb_pitch) +
                    x0;
                for (gx = 0; gx < GLYPH_W; gx++) {
                    uint16_t v = pix[gx];
                    p[gx * SCALE_X] = v;
                    p[gx * SCALE_X + 1u] = v;
                }
            }
        }
    } else {
        unsigned sx;
        for (gy = 0; gy < GLYPH_H; gy++) {
            uint8_t bits = font8x8[idx][gy];
            for (gx = 0; gx < GLYPH_W; gx++) {
                uint32_t pix = (bits & (1u << (7u - gx))) ? fg : bg;
                for (sy = 0; sy < SCALE_Y; sy++) {
                    for (sx = 0; sx < SCALE_X; sx++) {
                        fb_pixel(x0 + gx * SCALE_X + sx, y0 + gy * SCALE_Y + sy,
                                 pix);
                    }
                }
            }
        }
    }
    fb_mark_dirty(col, row, 1, 1);
}

static void fb_puts_at(unsigned col, unsigned row, const char *s, uint32_t color)
{
    unsigned cols = fb_cols();

    while (*s && col + 1 < cols) {
        fb_glyph(col++, row, *s++, color, COL_BG);
    }
}

static void fb_header(void)
{
    char line[40];
    unsigned n = 0;
    unsigned w = fb_w;
    unsigned h = fb_h;
    unsigned x;
    const char *p = "Veil HDMI  ";

    while (*p && n < sizeof(line) - 1) {
        line[n++] = *p++;
    }
    if (w >= 1000 && n + 4 < sizeof(line)) {
        line[n++] = (char)('0' + w / 1000);
        w %= 1000;
    }
    if (n + 8 < sizeof(line)) {
        line[n++] = (char)('0' + (w / 100) % 10);
        line[n++] = (char)('0' + (w / 10) % 10);
        line[n++] = (char)('0' + w % 10);
        line[n++] = 'x';
        line[n++] = (char)('0' + (h / 100) % 10);
        line[n++] = (char)('0' + (h / 10) % 10);
        line[n++] = (char)('0' + h % 10);
    }
    line[n] = 0;
    fb_puts_at(1, 0, line, COL_HD);
    for (x = term_ox; x < term_ox + TERM_COLS * CELL_W && x < fb_w; x++) {
        fb_pixel(x, term_oy + CELL_H + 1, COL_HD);
    }
    cur_c = 0;
    cur_r = 2;
}

static void fb_scroll(void) __attribute__((unused));

static void fb_scroll(void)
{
    unsigned row_px = CELL_H;
    unsigned x0 = term_ox;
    unsigned x1 = term_ox + TERM_COLS * CELL_W;
    unsigned y0 = term_oy;
    unsigned y1 = term_oy + TERM_ROWS * CELL_H;
    unsigned y, x;

    if (x1 > fb_w) {
        x1 = fb_w;
    }
    if (y1 > fb_h) {
        y1 = fb_h;
    }
    for (y = y0; y + row_px < y1; y++) {
        for (x = x0; x < x1; x++) {
            volatile uint8_t *src = fb + (size_t)(y + row_px) * fb_pitch;
            uint32_t c;
            if (fb_bpp == 16) {
                uint16_t v = ((volatile uint16_t *)src)[x];
                c = ((uint32_t)(v >> 11) << 19) | ((uint32_t)((v >> 5) & 0x3fu) << 10) |
                    ((uint32_t)(v & 0x1fu) << 3);
            } else if (fb_bpp == 24) {
                src += (size_t)x * 3u;
                c = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16);
            } else {
                c = ((volatile uint32_t *)src)[x];
            }
            fb_pixel(x, y, c);
        }
    }
    for (y = y1 - row_px; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            fb_pixel(x, y, sgr_bg());
        }
    }
}

static void fb_delay_ms(unsigned ms)
{
    uint64_t frq = read_cntfrq_el0();
    uint64_t until;

    if (frq == 0) {
        frq = 54000000ull;
    }
    until = read_cntpct_el0() + (frq / 1000ull) * (uint64_t)ms;
    while (read_cntpct_el0() < until) {
    }
}

static void fb_draw_bars(void)
{
    static const uint32_t bars[] = {
        0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
        0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u,
    };
    unsigned i, x, y, bw;

    bw = fb_w / 8u;
    if (bw == 0) {
        return;
    }
    for (y = CELL_H * 3u; y < fb_h; y++) {
        for (i = 0; i < 8; i++) {
            unsigned x0 = i * bw;
            unsigned x1 = (i == 7) ? fb_w : x0 + bw;
            for (x = x0; x < x1; x++) {
                fb_pixel(x, y, bars[i]);
            }
        }
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
    fb_header();
    fb_flush();
    hyp_printf("veil: fb pa %p %ux%u pitch %u %ubpp term @%u,%u (asked %ux%u)\n",
               (uint64_t)fb_pa, fb_w, fb_h, fb_pitch, fb_bpp, term_ox, term_oy,
               width, height);
    return 0;
}

void fb_selftest(void)
{
    unsigned i;
    unsigned keep_w = fb_w;
    unsigned keep_h = fb_h;

    if (!fb_ready) {
        return;
    }

    for (i = 0; i < FB_NMODES; i++) {
        if (fb_set_mode(fb_mode_w[i], fb_mode_h[i]) != 0) {
            continue;
        }
        fb_puts_at(1, 1, "framebuffer selftest", COL_FG);
        fb_draw_bars();
        fb_flush();
        fb_delay_ms(800);
        keep_w = fb_w;
        keep_h = fb_h;
    }
    (void)fb_set_mode(keep_w, keep_h);
}

static void fb_clamp(void)
{
    unsigned cols = fb_cols();
    unsigned rows = fb_rows();

    if (cur_c >= cols) {
        cur_c = cols ? cols - 1 : 0;
    }
    if (cur_r >= rows) {
        cur_r = rows ? rows - 1 : 0;
    }
}

static void fb_erase_cols(unsigned row, unsigned c0, unsigned c1)
{
    if (row >= TERM_ROWS || c0 >= c1) {
        return;
    }
    if (c1 > TERM_COLS) {
        c1 = TERM_COLS;
    }
    fb_fill_rect(term_ox + c0 * CELL_W, term_oy + row * CELL_H,
                 term_ox + c1 * CELL_W, term_oy + (row + 1) * CELL_H, sgr_bg());
    fb_mark_dirty(c0, row, c1 - c0, 1);
}

static void fb_erase_lines(unsigned r0, unsigned r1)
{
    if (r0 >= r1) {
        return;
    }
    if (r1 > TERM_ROWS) {
        r1 = TERM_ROWS;
    }
    fb_fill_rect(term_ox, term_oy + r0 * CELL_H, term_ox + TERM_COLS * CELL_W,
                 term_oy + r1 * CELL_H, sgr_bg());
    fb_mark_dirty(0, r0, TERM_COLS, r1 - r0);
}

static void fb_sgr(void)
{
    unsigned i, n, code;

    n = ansi_nargs ? ansi_nargs : 1;
    for (i = 0; i < n; i++) {
        code = ansi_nargs ? ansi_arg[i] : 0;
        if (code == 0) {
            sgr_fg_i = 7;
            sgr_bg_i = 0;
            sgr_fg_bright = 0;
            sgr_bg_bright = 0;
            sgr_bold = 0;
            sgr_rev = 0;
        } else if (code == 1) {
            sgr_bold = 1;
        } else if (code == 7) {
            sgr_rev = 1;
        } else if (code == 22) {
            sgr_bold = 0;
        } else if (code == 27) {
            sgr_rev = 0;
        } else if (code == 39) {
            sgr_fg_i = 7;
            sgr_fg_bright = 0;
        } else if (code == 49) {
            sgr_bg_i = 0;
            sgr_bg_bright = 0;
        } else if (code >= 30 && code <= 37) {
            sgr_fg_i = code - 30;
            sgr_fg_bright = 0;
        } else if (code >= 40 && code <= 47) {
            sgr_bg_i = code - 40;
            sgr_bg_bright = 0;
        } else if (code >= 90 && code <= 97) {
            sgr_fg_i = code - 90;
            sgr_fg_bright = 1;
        } else if (code >= 100 && code <= 107) {
            sgr_bg_i = code - 100;
            sgr_bg_bright = 1;
        }
    }
}

static void fb_csi(char final)
{
    unsigned a0 = ansi_nargs ? ansi_arg[0] : 0;
    unsigned a1 = (ansi_nargs > 1) ? ansi_arg[1] : 0;
    unsigned n = a0 ? a0 : 1;

    switch (final) {
    case 'H':
    case 'f':
        fb_flush_dirty();
        cur_r = (a0 ? a0 : 1u) - 1u;
        cur_c = (a1 ? a1 : 1u) - 1u;
        fb_clamp();
        break;
    case 'A':
        fb_flush_dirty();
        cur_r = (cur_r > n) ? cur_r - n : 0;
        break;
    case 'B':
        fb_flush_dirty();
        cur_r += n;
        fb_clamp();
        break;
    case 'C':
        fb_flush_dirty();
        cur_c += n;
        fb_clamp();
        break;
    case 'D':
        fb_flush_dirty();
        cur_c = (cur_c > n) ? cur_c - n : 0;
        break;
    case 'G':
        fb_flush_dirty();
        cur_c = (a0 ? a0 : 1u) - 1u;
        fb_clamp();
        break;
    case 'J':
        if (a0 == 1) {
            fb_erase_lines(0, cur_r);
            fb_erase_cols(cur_r, 0, cur_c + 1);
        } else if (a0 == 2 || a0 == 3) {
            fb_fill_term(sgr_bg());
        } else {
            fb_erase_cols(cur_r, cur_c, TERM_COLS);
            fb_erase_lines(cur_r + 1, TERM_ROWS);
        }
        fb_flush_dirty();
        break;
    case 'K':
        if (a0 == 1) {
            fb_erase_cols(cur_r, 0, cur_c + 1);
        } else if (a0 == 2) {
            fb_erase_cols(cur_r, 0, TERM_COLS);
        } else {
            fb_erase_cols(cur_r, cur_c, TERM_COLS);
        }
        fb_flush_dirty();
        break;
    case 'm':
        fb_sgr();
        break;
    case 's':
        sav_c = cur_c;
        sav_r = cur_r;
        break;
    case 'u':
        cur_c = sav_c;
        cur_r = sav_r;
        fb_clamp();
        break;
    default:
        break;
    }
}

void fb_putc(char c)
{
    unsigned char u = (unsigned char)c;

    if (!fb_ready) {
        return;
    }

    if (ansi_st == ANSI_ESC) {
        if (u == '[') {
            ansi_st = ANSI_CSI;
            ansi_nargs = 0;
            ansi_acc = 0;
            ansi_in_num = 0;
            memset(ansi_arg, 0, sizeof(ansi_arg));
            return;
        }
        if (u == '7') {
            sav_c = cur_c;
            sav_r = cur_r;
        } else if (u == '8') {
            cur_c = sav_c;
            cur_r = sav_r;
            fb_clamp();
        }
        ansi_st = (u == ']') ? ANSI_IGN : ANSI_NORM;
        return;
    }
    if (ansi_st == ANSI_IGN) {
        if (u == 0x07 || u == '\\') {
            ansi_st = ANSI_NORM;
        }
        return;
    }
    if (ansi_st == ANSI_CSI) {
        if (u >= '0' && u <= '9') {
            ansi_in_num = 1;
            ansi_acc = ansi_acc * 10u + (unsigned)(u - '0');
            return;
        }
        if (u == ';' || u == ':') {
            if (ansi_nargs < 16) {
                ansi_arg[ansi_nargs++] = ansi_in_num ? ansi_acc : 0;
            }
            ansi_acc = 0;
            ansi_in_num = 0;
            return;
        }
        if (u == '?') {
            return;
        }
        if (ansi_in_num && ansi_nargs < 16) {
            ansi_arg[ansi_nargs++] = ansi_acc;
        }
        ansi_st = ANSI_NORM;
        if (u >= 0x40) {
            fb_csi((char)u);
        }
        return;
    }

    if (u == 0x1b) {
        ansi_st = ANSI_ESC;
        return;
    }
    if (u == '\r') {
        cur_c = 0;
        return;
    }
    if (u == '\n') {
        fb_flush_dirty();
        cur_c = 0;
        if (cur_r + 1 < fb_rows()) {
            cur_r++;
        }
        return;
    }
    if (u == '\t') {
        cur_c = (cur_c + 4) & ~3u;
        fb_clamp();
        return;
    }
    if (u == 0x08) {
        if (cur_c) {
            cur_c--;
        }
        return;
    }
    if (u < 32) {
        return;
    }
    if (cur_c >= fb_cols()) {
        return;
    }
    fb_glyph(cur_c, cur_r, (char)u, sgr_fg(), sgr_bg());
    cur_c++;
    if (dirty_n >= TERM_COLS) {
        fb_flush_dirty();
    }
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

#define RAMFB_XR24 0x34325258u
#define RAMFB_AR24 0x34325241u
#define RAMFB_RG16 0x36314752u

static int ramfb_on;
static int ramfb_have_pixels;
static int ramfb_irq_ok;
static int ramfb_saw_write;
static unsigned ramfb_writes;
static uint64_t ramfb_pa;
static uint32_t ramfb_w, ramfb_h, ramfb_stride, ramfb_fourcc;
static uint64_t ramfb_last_pct;
static uint64_t ramfb_last_write;

static uint32_t ramfb_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t ramfb_src32(const volatile uint8_t *line, unsigned x)
{
    if (ramfb_fourcc == RAMFB_RG16) {
        uint16_t v = ((const volatile uint16_t *)line)[x];
        unsigned r = (unsigned)(v >> 11) << 3;
        unsigned g = (unsigned)((v >> 5) & 0x3fu) << 2;
        unsigned b = (unsigned)(v & 0x1fu) << 3;
        return (r << 16) | (g << 8) | b;
    }
    return ((const volatile uint32_t *)line)[x];
}

void fb_ramfb_note_write(void)
{
    ramfb_saw_write = 1;
    ramfb_last_write = read_cntpct_el0();
    ramfb_writes++;
    if (ramfb_writes == 1u || ramfb_writes == 8u || ramfb_writes == 64u ||
        ramfb_writes == 256u || ramfb_writes == 480u ||
        (ramfb_writes % 512u) == 0u) {
        hyp_debug("veil: ramfb wr n=%u\n", ramfb_writes);
    }
}

void fb_ramfb_present(void)
{
    unsigned dw, dh, ox, oy, y, x;
    uint64_t now, frq;
    int painted = 0;
    static int blit_log;
    static unsigned scanned_wr;

    if (!ramfb_on || !fb || !ramfb_w || !ramfb_h) {
        return;
    }
    frq = read_cntfrq_el0();
    if (frq == 0) {
        frq = 54000000ull;
    }
    now = read_cntpct_el0();
    /* Wait until GOP has actually stored, then until writes go quiet so we
     * do not blit a half-drawn frame (花屏). */
    if (!ramfb_saw_write) {
        return;
    }
    if (now - ramfb_last_write < frq / 30ull) {
        return;
    }
    /* One scan per GOP store burst. Re-walking 1.8 MiB on every CNTHP
     * left the guest stuck at VBAR+0x280 for hundreds of ms. */
    if (ramfb_irq_ok && ramfb_writes == scanned_wr) {
        return;
    }
    ramfb_last_pct = now;

    dw = ramfb_w;
    dh = ramfb_h;
    if (dw > fb_w) {
        dw = fb_w;
    }
    if (dh > fb_h) {
        dh = fb_h;
    }
    ox = (fb_w - dw) / 2u;
    oy = (fb_h - dh) / 2u;
    guest_mem_sync(ramfb_pa, (size_t)ramfb_stride * ramfb_h);

    for (y = 0; y < dh && !painted; y += 2u) {
        const volatile uint8_t *src =
            (const volatile uint8_t *)(uintptr_t)ramfb_pa +
            (size_t)y * ramfb_stride;
        for (x = 0; x < dw; x += 2u) {
            if (ramfb_src32(src, x) & 0x00ffffffu) {
                painted = 1;
                break;
            }
        }
    }
#if VEIL_DEBUG
    {
        static int skip_log;
        const volatile uint8_t *base =
            (const volatile uint8_t *)(uintptr_t)ramfb_pa;
        uint32_t tl = ramfb_src32(base, 0);
        uint32_t mid = ramfb_src32(base + (size_t)(dh / 2u) * ramfb_stride,
                                   dw / 2u);
        uint32_t br = ramfb_src32(base + (size_t)(dh - 1u) * ramfb_stride,
                                  dw - 1u);
        unsigned quiet_ms =
            (unsigned)(((now - ramfb_last_write) * 1000ull) / frq);
        if (skip_log < 8) {
            hyp_printf(
                "veil: ramfb scan wr=%u quiet=%ums painted=%u tl=0x%x mid=0x%x br=0x%x\n",
                ramfb_writes, quiet_ms, painted, tl, mid, br);
            skip_log++;
        }
    }
#endif
    /* SetMode fills black then WFI. Withholding vIRQ until pixels exist
     * deadlocks BDS/logo: firmware never paints, HDMI keeps the splash. */
    scanned_wr = ramfb_writes;
    if (!ramfb_irq_ok) {
        ramfb_irq_ok = 1;
        hyp_debug("veil: ramfb virq wr=%u painted=%u\n", ramfb_writes,
                  painted);
    }
    if (!painted) {
        s2_ramfb_ro();
        return;
    }

    if (!ramfb_have_pixels) {
        fb_fill(0);
    }

    for (y = 0; y < dh; y++) {
        const volatile uint8_t *src =
            (const volatile uint8_t *)(uintptr_t)ramfb_pa +
            (size_t)y * ramfb_stride;
        if (fb_bpp == 16) {
            volatile uint16_t *dst =
                (volatile uint16_t *)(fb + (size_t)(oy + y) * fb_pitch);
            for (x = 0; x < dw; x++) {
                dst[ox + x] = fb_rgb565(ramfb_src32(src, x));
            }
        } else if (fb_bpp == 32) {
            volatile uint32_t *dst =
                (volatile uint32_t *)(fb + (size_t)(oy + y) * fb_pitch);
            for (x = 0; x < dw; x++) {
                dst[ox + x] = ramfb_src32(src, x) & 0x00ffffffu;
            }
        } else {
            for (x = 0; x < dw; x++) {
                fb_pixel(ox + x, oy + y, ramfb_src32(src, x));
            }
        }
    }
    guest_mem_sync(fb_pa + (uint64_t)oy * fb_pitch, (size_t)dh * fb_pitch);
    ramfb_have_pixels = 1;
    fb_ready = 0;
    s2_ramfb_ro();
    if (!blit_log) {
        blit_log = 1;
        hyp_printf("veil: ramfb blit %ux%u wr=%u\n", dw, dh, ramfb_writes);
    }
}

int fb_ramfb_shown(void)
{
    return ramfb_have_pixels;
}

int fb_ramfb_allow_virq(void)
{
    /* After the first quiet GOP store (often SetMode black). CpuDxe is
     * already installed by then; earlier VI hit DefaultExceptionHandler. */
    return ramfb_irq_ok;
}

int fb_map_guest_ramfb(const uint8_t *be_cfg, uint32_t len)
{
    uint64_t ipa, pa;
    uint32_t fourcc, w, h, stride;
    size_t remain, need, bpp;

    if (!be_cfg || len < 28) {
        return -1;
    }
    ipa = ((uint64_t)ramfb_be32(be_cfg) << 32) | ramfb_be32(be_cfg + 4);
    fourcc = ramfb_be32(be_cfg + 8);
    w = ramfb_be32(be_cfg + 16);
    h = ramfb_be32(be_cfg + 20);
    stride = ramfb_be32(be_cfg + 24);
    if (!ipa || !w || !h || !stride) {
        ramfb_on = 0;
        return -1;
    }
    if (fourcc != RAMFB_XR24 && fourcc != RAMFB_AR24 && fourcc != RAMFB_RG16) {
        hyp_printf("veil: ramfb fourcc 0x%x unsupported\n", fourcc);
        return -1;
    }
    bpp = (fourcc == RAMFB_RG16) ? 2u : 4u;
    if (stride < w * bpp) {
        return -1;
    }
    need = (size_t)stride * h;
    if (s2_ipa_to_pa(ipa, &pa, &remain) != 0 || remain < need) {
        hyp_printf("veil: ramfb ipa %p not in guest ram\n", ipa);
        return -1;
    }
    ramfb_pa = pa;
    ramfb_w = w;
    ramfb_h = h;
    ramfb_stride = stride;
    ramfb_fourcc = fourcc;
    ramfb_last_pct = 0;
    ramfb_last_write = 0;
    ramfb_have_pixels = 0;
    ramfb_irq_ok = 0;
    ramfb_saw_write = 0;
    ramfb_writes = 0;
    ramfb_on = 1;
    if (s2_ramfb_watch(ipa, need) != 0) {
        hyp_printf("veil: ramfb s2 watch failed ipa=%p n=%u\n", ipa,
                   (unsigned)need);
    } else {
        hyp_printf("veil: ramfb s2 watch %u KiB\n", (unsigned)(need >> 10));
    }
    board_start_ramfb_timer();
    hyp_printf("veil: guest ramfb ipa %p -> pa %p %ux%u fourcc=0x%x stride=%u\n",
               ipa, pa, w, h, fourcc, stride);
    return 0;
}
