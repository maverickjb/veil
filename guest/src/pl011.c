#include "veil.h"
#include "virtio_board.h"

/*
 * Guest PL011 at VEIL_PL011_IPA (0x09000000). RX is a ring filled from the
 * host UART (uart10 on Pi 5) by pl011_pull_host; TX is uart_putc on that
 * same line. EL2 debug prints share it with UEFI ConIn.
 */

#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_FR_TXFE (1u << 7)
#define PL011_FR_RXFE (1u << 4)
#define PL011_IBRD 0x24
#define PL011_FBRD 0x28
#define PL011_LCRH 0x2c
#define PL011_CR   0x30
#define PL011_IMSC 0x38
#define PL011_RIS  0x3c
#define PL011_MIS  0x40
#define PL011_ICR  0x44
#define PL011_RXIS (1u << 4)
#define PL011_RTIS (1u << 6)

static uint32_t pl011_cr = 0x301;
static uint32_t pl011_ibrd = 1;
static uint32_t pl011_fbrd;
static uint32_t pl011_lcrh = 0x70;
static uint32_t pl011_imsc;
static uint32_t pl011_ris;
static uint8_t pl011_rx[32];
static unsigned pl011_rx_r, pl011_rx_w, pl011_rx_n;
static int pl011_irq;

static void pl011_rx_push(uint8_t b)
{
    if (pl011_rx_n >= sizeof(pl011_rx)) {
        return;
    }
    pl011_rx[pl011_rx_w] = b;
    pl011_rx_w = (pl011_rx_w + 1u) % (unsigned)sizeof(pl011_rx);
    pl011_rx_n++;
}

static void pl011_update_irq(void)
{
    if (pl011_rx_n) {
        pl011_ris |= PL011_RXIS | PL011_RTIS;
    } else {
        pl011_ris &= ~(PL011_RXIS | PL011_RTIS);
    }
    pl011_irq = (pl011_imsc & pl011_ris) != 0;
}

void pl011_pull_host(void)
{
    volatile uint32_t *fr = (volatile uint32_t *)(BOARD_UART_BASE + PL011_FR);
    volatile uint32_t *dr = (volatile uint32_t *)(BOARD_UART_BASE + PL011_DR);
    unsigned n = 0;

    while (pl011_rx_n < sizeof(pl011_rx) && n++ < 32u) {
        if (*fr & PL011_FR_RXFE) {
            break;
        }
        pl011_rx_push((uint8_t)*dr);
    }
    pl011_update_irq();
    if (n) {
        fb_ramfb_present();
    }
}

static uint64_t pl011_read(uint64_t off)
{
    pl011_pull_host();
    switch (off) {
    case PL011_DR:
        if (!pl011_rx_n) {
            return 0;
        }
        {
            uint8_t b = pl011_rx[pl011_rx_r];
            pl011_rx_r = (pl011_rx_r + 1u) % (unsigned)sizeof(pl011_rx);
            pl011_rx_n--;
            pl011_update_irq();
            return b;
        }
    case PL011_FR:
        return PL011_FR_TXFE | (pl011_rx_n ? 0u : PL011_FR_RXFE);
    case PL011_IBRD:
        return pl011_ibrd;
    case PL011_FBRD:
        return pl011_fbrd;
    case PL011_LCRH:
        return pl011_lcrh;
    case PL011_CR:
        return pl011_cr;
    case PL011_IMSC:
        return pl011_imsc;
    case PL011_RIS:
        return pl011_ris;
    case PL011_MIS:
        return pl011_imsc & pl011_ris;
    case 0xfe0:
        return 0x11;
    case 0xfe4:
        return 0x10;
    case 0xfe8:
        return 0x34;
    case 0xfec:
        return 0x00;
    case 0xff0:
        return 0x0d;
    case 0xff4:
        return 0xf0;
    case 0xff8:
        return 0x05;
    case 0xffc:
        return 0xb1;
    default:
        return 0;
    }
}

static void pl011_write(uint64_t off, uint64_t val)
{
    switch (off) {
    case PL011_DR: {
        char c = (char)(val & 0xff);
        static const char *want = "Synchronous Exception";
        static int match;
        uart_putc(c);
        fb_ramfb_present();
        if (c == want[match]) {
            match++;
            if (want[match] == 0) {
                dump_guest_undef();
                match = 0;
            }
        } else {
            match = (c == want[0]);
        }
        break;
    }
    case PL011_IBRD:
        pl011_ibrd = (uint32_t)val;
        break;
    case PL011_FBRD:
        pl011_fbrd = (uint32_t)val;
        break;
    case PL011_LCRH:
        pl011_lcrh = (uint32_t)val;
        break;
    case PL011_CR:
        pl011_cr = (uint32_t)val;
        break;
    case PL011_IMSC:
        pl011_imsc = (uint32_t)val;
        pl011_update_irq();
        break;
    case PL011_ICR:
        pl011_ris &= ~(uint32_t)val;
        pl011_update_irq();
        break;
    default:
        break;
    }
}

void pl011_init(void)
{
}

int pl011_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    uint64_t off;

    (void)size;
    if (ipa < VEIL_PL011_IPA || ipa >= VEIL_PL011_IPA + VEIL_PL011_SIZE) {
        return -1;
    }
    off = ipa - VEIL_PL011_IPA;
    if (is_write) {
        pl011_write(off, *data);
    } else {
        *data = pl011_read(off);
    }
    return 0;
}

int pl011_has_rx(void)
{
    return pl011_rx_n != 0;
}

int pl011_irq_pending(void)
{
    return pl011_irq;
}

uint8_t pl011_rx_front(void)
{
    return pl011_rx_n ? pl011_rx[pl011_rx_r] : 0;
}

unsigned pl011_rx_count(void)
{
    return pl011_rx_n;
}
