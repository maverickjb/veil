#include "board.h"
#include "veil.h"

#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_FR_TXFF (1u << 5)
#define PL011_IMSC 0x38
#define PL011_ICR  0x44

/* BCM2712 GIC-400 in the 0x10_xxxx_xxxx SoC window (linux bcm2712.dtsi). */
#define HOST_GICD 0x000000107fff9000ull
#define HOST_GICC 0x000000107fffa000ull
#define GICD_CTLR      0x000
#define GICD_IIDR      0x008
#define GICD_ICENABLER 0x180
#define GICC_CTLR      0x000
#define GICC_PMR       0x004
#define GICC_IAR       0x00c
#define GICC_EOIR      0x010

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

void uart_init(void)
{
    /*
     * Firmware already programmed uart10 (115200 8n1) for the debug header.
     * Do not rewrite IBRD/FBRD — those divisors are QEMU-raspi3 values.
     * Drop RX/TX IRQs so HCR.IMO does not livelock EL2 on uart10.
     */
    *uart_reg(PL011_IMSC) = 0;
    *uart_reg(PL011_ICR) = 0x7ffu;
}

void board_mask_host_irqs(void)
{
    volatile uint32_t *gicd = (volatile uint32_t *)HOST_GICD;
    volatile uint32_t *gicc = (volatile uint32_t *)HOST_GICC;
    uint32_t iidr;
    unsigned i;

    iidr = gicd[GICD_IIDR / 4];
    gicc[GICC_CTLR / 4] = 0;
    gicc[GICC_PMR / 4] = 0;
    gicd[GICD_CTLR / 4] = 0;
    for (i = 0; i < 32; i++) {
        gicd[(GICD_ICENABLER / 4) + i] = 0xffffffffu;
    }
    hyp_printf("veil: host gic-400 iidr=0x%x masked\n", iidr);
}

void board_ack_host_irq(void)
{
    volatile uint32_t *gicc = (volatile uint32_t *)HOST_GICC;
    uint32_t iar = gicc[GICC_IAR / 4];

    if ((iar & 0x3ffu) < 1020u) {
        gicc[GICC_EOIR / 4] = iar;
    }
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
