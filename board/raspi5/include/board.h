#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Host SoC: BCM2712 (Raspberry Pi 5). Guest never programs these addresses.
 * The guest-visible map lives in guest/.
 *
 * Debug UART is uart10 (JST-SH "UART" next to USB-C), not the RP1 GPIO
 * header UART. Addresses follow linux bcm2712.dtsi (SoC window + 0x10_00000000).
 */

#define BOARD_UART_BASE 0x000000107d001000ull /* PL011 uart10 */
#define BOARD_MBOX_BASE 0x000000107c013880ull /* VideoCore mailbox */
#define BOARD_RAM_PA    0x00000000ull
#define BOARD_RAM_SIZE  0x3f000000ull /* first 1 GiB minus a conservative hole */
#define BOARD_HYP_PA    0x00080000ull
#define BOARD_HYP_SIZE  0x00f80000ull /* 0x80000 .. 0x1000000 */
#define BOARD_GUEST_PA  0x01000000ull
#define BOARD_NAME      "raspi5"
#define BOARD_CPU       "bcm2712 cortex-a76"
#define BOARD_GIC_PASSTHROUGH 0

struct platform {
    const char *name;
    const char *cpu_note;
    uintptr_t uart_base;
    uintptr_t ram_pa;
    size_t ram_size;
    uintptr_t hyp_pa;
    size_t hyp_size;
    uintptr_t guest_ram_pa;
    size_t guest_ram_size;
};

const struct platform *platform_get(void);
void platform_clip_guest_ram(uint64_t pa, size_t n);
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int fb_init(void);
void fb_putc(char c);
int fb_map_guest_ramfb(const uint8_t *be_cfg, uint32_t len);
int board_skip_testdisk(void);
