#include "board.h"
#include "aarch64.h"
#include "veil.h"

#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_FR_TXFF (1u << 5)
#define PL011_CR   0x30
#define PL011_IMSC 0x38
#define PL011_ICR  0x44

/* BCM2712 GIC-400 in the 0x10_xxxx_xxxx SoC window (linux bcm2712.dtsi). */
#define HOST_GICD 0x000000107fff9000ull
#define HOST_GICC 0x000000107fffa000ull
#define GICD_CTLR      0x000
#define GICD_IIDR      0x008
#define GICD_IGROUPR   0x080
#define GICD_ISENABLER 0x100
#define GICD_ICENABLER 0x180
#define GICD_ISPENDR   0x200
#define GICD_IPRIORITY 0x400
#define GICC_CTLR      0x000
#define GICC_PMR       0x004
#define GICC_IAR       0x00c
#define GICC_EOIR      0x010
#define GICC_AIAR      0x020
#define GICC_AEOIR     0x024
#define PPI_CNTHP      26u

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
    /* Keep RXE|TXE|UARTEN. Esc/Shell ConIn is this same uart10. */
    *uart_reg(PL011_CR) = 0x301u;
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
    write_cnthp_ctl_el2(0);
    hyp_printf("veil: host gic-400 iidr=0x%x masked\n", iidr);
}

static void cnthp_rearm(void)
{
    uint64_t frq = read_cntfrq_el0();
    if (frq == 0) {
        frq = 54000000ull;
    }
    write_cnthp_ctl_el2(0);
    isb();
    write_cnthp_tval_el2(frq / 15ull);
    write_cnthp_ctl_el2(1ull);
    isb();
}

void board_start_ramfb_timer(void)
{
    volatile uint32_t *gicd = (volatile uint32_t *)HOST_GICD;
    volatile uint32_t *gicc = (volatile uint32_t *)HOST_GICC;
    unsigned i;

    for (i = 0; i < 32; i++) {
        gicd[(GICD_ICENABLER / 4) + i] = 0xffffffffu;
    }
    gicd[GICD_IGROUPR / 4] |= (1u << PPI_CNTHP);
    ((volatile uint8_t *)(HOST_GICD + GICD_IPRIORITY))[PPI_CNTHP] = 0x80u;
    gicd[GICD_ISENABLER / 4] = (1u << PPI_CNTHP);
    dsb_sy();
    gicc[GICC_PMR / 4] = 0xf0u;
    gicd[GICD_CTLR / 4] = 3;
    gicc[GICC_CTLR / 4] = 3;
    cnthp_rearm();
    {
        uint64_t t0 = read_cntpct_el0();
        uint64_t frq = read_cntfrq_el0();
        if (frq == 0) {
            frq = 54000000ull;
        }
        while (read_cntpct_el0() - t0 < frq / 20ull) {
        }
        hyp_printf("veil: ramfb timer ppi%u ctl=0x%lx ispend=0x%x\n",
                   PPI_CNTHP, (unsigned long)read_cnthp_ctl_el2(),
                   gicd[GICD_ISPENDR / 4]);
    }
}

void board_ack_host_irq(void)
{
    volatile uint32_t *gicc = (volatile uint32_t *)HOST_GICC;
    uint32_t iar = gicc[GICC_IAR / 4];
    uint32_t id = iar & 0x3ffu;
    int grp1 = 0;
    static unsigned irq_n;

    if (id >= 1020u) {
        iar = gicc[GICC_AIAR / 4];
        id = iar & 0x3ffu;
        grp1 = 1;
    }
    if (id < 1020u) {
        if (grp1) {
            gicc[GICC_AEOIR / 4] = iar;
        } else {
            gicc[GICC_EOIR / 4] = iar;
        }
    }
    cnthp_rearm();
    if (irq_n < 3u) {
        hyp_printf("veil: host irq %u id=%u grp1=%d ctl=0x%lx\n", irq_n, id,
                   grp1, (unsigned long)read_cnthp_ctl_el2());
    }
    irq_n++;
}

void uart_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    while (*uart_reg(PL011_FR) & PL011_FR_TXFF) {
    }
    *uart_reg(PL011_DR) = (uint32_t)(uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}
