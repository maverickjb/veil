#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Comparison host: QEMU virt with virtualization=on.
 * UART/RAM sit closer to the guest virt map — only for identity-map tests.
 */

#define BOARD_UART_BASE 0x09000000ull
#define BOARD_RAM_PA    0x40000000ull
#define BOARD_RAM_SIZE  (2048ull * 1024ull * 1024ull)
/* Hyp at the top of RAM so guest IPA 0x40000000 is identity — PCI DMA works. */
#define BOARD_HYP_PA    0xBF000000ull
#define BOARD_HYP_SIZE  0x01000000ull
#define BOARD_GUEST_PA  0x40000000ull
#define BOARD_NAME      "qemu-virt"
#define BOARD_CPU       "cortex-a76"
/* Host GICv3 sits at the virt map; pass it through so AAVMF matches make ref. */
#define BOARD_GIC_PASSTHROUGH 1
#define BOARD_GUEST_RAMFB 1

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
void board_mask_host_irqs(void);
void board_ack_host_irq(void);
void uart_putc(char c);
void uart_puts(const char *s);
int fb_init(void);
void fb_putc(char c);
int fb_map_guest_ramfb(const uint8_t *be_cfg, uint32_t len);
int board_skip_testdisk(void);
