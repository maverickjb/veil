#pragma once

#include "board.h"
#include "aarch64.h"
#include "veil.h"

/*
 * Host HDMI internals shared by mailbox setup (fb.c), ANSI/selftest
 * (fb_term.c), and guest ramfb blit (fb_ramfb.c). Not a public board API.
 */

#define GLYPH_W   8u
#define GLYPH_H   8u
#define SCALE_X   2u
#define SCALE_Y   4u
#define CELL_W    (GLYPH_W * SCALE_X)
#define CELL_H    (GLYPH_H * SCALE_Y)
#define TERM_COLS 80u
#define TERM_ROWS 25u

#define COL_BG 0x00101828u
#define COL_FG 0x00d8d8d0u
#define COL_HD 0x00f0c040u

extern volatile uint8_t *fb;
extern uint32_t fb_pitch;
extern uint32_t fb_pa;
extern unsigned fb_w, fb_h, fb_bpp;
extern unsigned term_ox, term_oy;
extern int fb_ready;

uint16_t fb_rgb565(uint32_t color);
void fb_pixel(unsigned x, unsigned y, uint32_t color);
void fb_fill_rect(unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                  uint32_t color);
void fb_fill(uint32_t color);
void fb_flush(void);
void fb_term_after_mode(void);
