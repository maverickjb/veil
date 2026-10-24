#include "board.h"
#include "veil.h"

#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_FR_TXFF (1u << 5)
#define PL011_IBRD 0x24
#define PL011_FBRD 0x28
#define PL011_LCRH 0x2c
#define PL011_CR   0x30
#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE    (1u << 8)
#define PL011_CR_RXE    (1u << 9)
#define PL011_LCRH_FEN  (1u << 4)
#define PL011_LCRH_WLEN_8 (3u << 5)

static struct platform plat = {
    .name = BOARD_NAME,
    .cpu_note = BOARD_CPU,
    .uart_base = BOARD_UART_BASE,
    .ram_pa = BOARD_RAM_PA,
    .ram_size = BOARD_RAM_SIZE,
    .hyp_pa = BOARD_HYP_PA,
    .hyp_size = BOARD_HYP_SIZE,
    .guest_ram_pa = BOARD_GUEST_PA,
    .guest_ram_size = BOARD_RAM_SIZE - (BOARD_GUEST_PA - BOARD_RAM_PA),
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

void board_mask_host_irqs(void)
{
}

void board_ack_host_irq(void)
{
}

void uart_init(void)
{
    *uart_reg(PL011_CR) = 0;
    *uart_reg(PL011_IBRD) = 1;
    *uart_reg(PL011_FBRD) = 0;
    *uart_reg(PL011_LCRH) = PL011_LCRH_FEN | PL011_LCRH_WLEN_8;
    *uart_reg(PL011_CR) = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
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
