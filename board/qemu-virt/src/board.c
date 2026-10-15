#include "board.h"
#include "veil.h"

#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_FR_TXFF (1u << 5)

static struct platform plat = {
    .name = BOARD_NAME,
    .cpu_note = BOARD_CPU,
    .uart_base = BOARD_UART_BASE,
    .ram_pa = BOARD_RAM_PA,
    .ram_size = BOARD_RAM_SIZE,
    .hyp_pa = BOARD_HYP_PA,
    .hyp_size = BOARD_HYP_SIZE,
    .guest_ram_pa = BOARD_GUEST_PA,
    .guest_ram_size = BOARD_HYP_PA - BOARD_GUEST_PA,
};

static volatile uint32_t *uart_reg(unsigned off)
{
    return (volatile uint32_t *)(plat.uart_base + off);
}

const struct platform *platform_get(void)
{
    return &plat;
}

void platform_clip_guest_ram(uint64_t pa, size_t n)
{
    uint64_t start = plat.guest_ram_pa;
    uint64_t end = start + plat.guest_ram_size;
    uint64_t fb_end = pa + n;
    if (fb_end <= start || pa >= end) {
        return;
    }
    if (pa > start) {
        plat.guest_ram_size = (size_t)((pa - start) & ~0x1fffffull);
    }
}

void uart_init(void)
{
    (void)uart_reg(PL011_DR);
}

void uart_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    while (*uart_reg(PL011_FR) & PL011_FR_TXFF) {
    }
    *uart_reg(PL011_DR) = (uint32_t)(uint8_t)c;
    fb_putc(c);
}

void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}
