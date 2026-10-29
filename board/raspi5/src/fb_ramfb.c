#include "fb_priv.h"

/*
 * Guest ramfb (fw_cfg etc/ramfb) → host HDMI convert-on-copy.
 * Quiet-wait then skip-all-black; painted=1 only if a non-black pixel exists.
 */

#define RAMFB_XR24 0x34325258u
#define RAMFB_AR24 0x34325241u
#define RAMFB_RG16 0x36314752u

static int ramfb_on;
static int ramfb_have_pixels;
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
     * left the guest stuck in the VBAR IRQ slot for hundreds of ms. */
    if (guest_gop_ready() && ramfb_writes == scanned_wr) {
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
    if (!guest_gop_ready()) {
        hyp_debug("veil: ramfb virq wr=%u painted=%u\n", ramfb_writes,
                  painted);
        guest_gop_note_quiet();
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
    guest_gop_reset();
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
