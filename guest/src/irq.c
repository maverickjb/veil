#include "aarch64.h"
#include "veil.h"
#include "virtio_board.h"

/*
 * Guest vIRQ pin (HCR.VI) and IAR/EOI coupling to PL011 / CNTV.
 * Distributor MMIO is gic_dist.c; ICC/HVC is gic_cpu.c.
 *
 *   pending(id) → may set VI (not if I=1 or PC in VBAR IRQ slot)
 *   IAR         → in_service, drop VI
 *   EOI         → idle, drop VI
 */

static uint32_t irq_id;

enum {
    IRQ_IDLE = 0,
    IRQ_PENDING,
    IRQ_IN_SERVICE,
};

static unsigned irq_st;

uint32_t irq_latched_id(void)
{
    return irq_id;
}

static uint32_t pending_intid(void)
{
    uint64_t vctl = read_cntv_ctl_el0();
    uint64_t pctl = read_cntp_ctl_el0();

    {
        int uart = pl011_has_rx();
        int tmr = (vctl & 1u) && !(vctl & 2u) &&
                  ((vctl & 4u) || read_cntvct_el0() >= read_cntv_cval_el0());
        static int prefer_uart;

        /* Leftover RX must not starve CNTV: Shell WaitForKey polls the
         * timer if SerialDxe never registered VEIL_UART_INTID. */
        if (uart && tmr) {
            prefer_uart = !prefer_uart;
            return prefer_uart ? VEIL_UART_INTID : VEIL_CNTV_INTID;
        }
        if (uart) {
            return VEIL_UART_INTID;
        }
        if (tmr) {
            return VEIL_CNTV_INTID;
        }
    }
    if ((pctl & 1u) && !(pctl & 2u) &&
        ((pctl & 4u) || read_cntpct_el0() >= read_cntp_cval_el0())) {
        return VEIL_CNTP_INTID;
    }
    if (pl011_irq_pending()) {
        return VEIL_UART_INTID;
    }
    return virtio_pending_intid();
}

static int irq_pin_blocked(void)
{
    uint64_t elr = read_elr_el2();

    /* PSTATE.I or still in the VBAR IRQ/FIQ/SError slots: do not assert VI. */
    return guest_pc_in_irq_slot(elr) || (read_spsr_el2() & (1ull << 7)) != 0;
}

static void irq_apply_pin(void)
{
    uint64_t hcr = read_hcr_el2();

#if VEIL_DEBUG
    {
        static unsigned trace_n;
        uint64_t elr = read_elr_el2();
        uint64_t vbar = read_vbar_el1();
        if (fb_ramfb_allow_virq() && trace_n < 8u) {
            uint64_t spsr = read_spsr_el2();
            uint64_t pa280 = 0;
            uint32_t i280 = 0;

            if (guest_va_to_pa(vbar + VEIL_VBAR_IRQ_OFF, &pa280) == 0) {
                i280 = *(volatile uint32_t *)(uintptr_t)pa280;
            }
            hyp_printf(
                "veil: irqtr %u elr=%p I=%u insn=0x%x vbar=%p i280=0x%x igrp1=%u gicr=0x%x id=%u vi=%u cntv=0x%lx elr1=%p sp=%p\n",
                trace_n, elr, (spsr & (1u << 7)) ? 1u : 0u,
                guest_fetch_insn(elr), vbar, i280, icc_igrpen1_get(),
                gicr_isenabler_get(), irq_id, (hcr & HCR_EL2_VI) ? 1u : 0u,
                (unsigned long)read_cntv_ctl_el0(), read_elr_el1(),
                read_sp_el1());
            trace_n++;
        }
    }
#endif
    /* No vIRQ until the first quiet ramfb store: earlier, AAVMF still
     * uses DefaultExceptionHandler. Waiting for pixels deadlocks
     * (SetMode black, WFI, no timer, no logo). */
    if (irq_pin_blocked()) {
        write_hcr_el2(hcr & ~HCR_EL2_VI);
    } else if (irq_st == IRQ_PENDING && irq_id && fb_ramfb_allow_virq()) {
        static int vi_log;
        write_hcr_el2(hcr | HCR_EL2_VI);
        if (!vi_log) {
            vi_log = 1;
            hyp_debug("veil: inject vi id=%u cntv=0x%lx cntp=0x%lx\n",
                      irq_id, (unsigned long)read_cntv_ctl_el0(),
                      (unsigned long)read_cntp_ctl_el0());
        }
    } else {
        static int no_vi_log;
        write_hcr_el2(hcr & ~HCR_EL2_VI);
        if (fb_ramfb_allow_virq() && !irq_id && !no_vi_log) {
            no_vi_log = 1;
            hyp_debug("veil: no guest timer cntv=0x%lx cntp=0x%lx\n",
                      (unsigned long)read_cntv_ctl_el0(),
                      (unsigned long)read_cntp_ctl_el0());
        }
    }
}

static void irq_pending(uint32_t id)
{
    irq_id = id;
    irq_st = id ? IRQ_PENDING : IRQ_IDLE;
    irq_apply_pin();
}

static void irq_in_service(uint32_t id)
{
    irq_id = 0;
    irq_st = IRQ_IN_SERVICE;
    write_hcr_el2(read_hcr_el2() & ~HCR_EL2_VI);
    (void)id;
}

static void irq_idle(void)
{
    irq_st = IRQ_IDLE;
    /* Leave irq_id: poll may already have latched the next INTID. */
    write_hcr_el2(read_hcr_el2() & ~HCR_EL2_VI);
}

void guest_cntv_kick(void)
{
    uint64_t vctl;
    static int unmask_cntv;

    if (unmask_cntv || !fb_ramfb_allow_virq()) {
        return;
    }
    vctl = read_cntv_ctl_el0();
    if ((vctl & 3ull) == 0) {
        return;
    }
    {
        uint64_t frq = read_cntfrq_el0();
        if (frq == 0) {
            frq = 54000000ull;
        }
        write_cntv_ctl_el0((vctl | 1ull) & ~2ull);
        write_cntv_tval_el0(frq / 200ull);
        isb();
        gic_enable_intid(VEIL_CNTV_INTID);
        unmask_cntv = 1;
        hyp_debug("veil: cntv kick was=0x%lx now=0x%lx\n",
                  (unsigned long)vctl,
                  (unsigned long)read_cntv_ctl_el0());
    }
}

void veil_timer_poll(void)
{
    /* VBAR_EL1 is not trapped; notice a new table on the tick path. */
    guest_vbar_hook();
    pl011_pull_host();
    fb_ramfb_present();
    guest_cntv_kick();
    irq_pending(pending_intid());
    isb();
}

void guest_wfx(struct trapframe *tf, uint32_t iss)
{
    static int wfi_log;

    if (!wfi_log) {
        wfi_log = 1;
        hyp_printf("veil: guest %s\n", (iss & 1u) ? "wfi" : "wfe");
    }
    veil_wfx_idle(tf);
    tf->elr += 4;
}

void veil_wfx_idle(struct trapframe *tf)
{
    uint64_t frq = read_cntfrq_el0();
    uint64_t start = read_cntpct_el0();
    uint64_t slice;

    if (frq == 0) {
        frq = 54000000ull;
    }
    slice = frq / 20ull;
    pl011_pull_host();
    while (read_cntpct_el0() - start < slice) {
        veil_timer_poll();
        pl011_pull_host();
        if (pl011_has_rx()) {
            break;
        }
    }
    irq_id = pending_intid();
    if (irq_id) {
        irq_st = IRQ_PENDING;
    } else if (irq_st != IRQ_IN_SERVICE) {
        irq_st = IRQ_IDLE;
    }
    if (pl011_has_rx() && tf) {
        static int rx_log;
        gic_enable_intid(VEIL_UART_INTID);
        irq_id = VEIL_UART_INTID;
        irq_st = IRQ_PENDING;
        tf->spsr &= ~(1ull << 7);
        write_hcr_el2(read_hcr_el2() | HCR_EL2_VI);
        if (!rx_log) {
            rx_log = 1;
            hyp_printf("veil: uart rx 0x%x n=%u\n", pl011_rx_front(),
                       pl011_rx_count());
        }
    }
    fb_ramfb_present();
}

void veil_timer_poll_tf(struct trapframe *tf)
{
    uint32_t i0, i1, i2, i3;

    veil_timer_poll();
    if (!tf) {
        return;
    }
    /*
     * AAVMF PerformanceLib NanoSecondDelay, seen at 0x7da97460 in IRQ:
     *   add x0, x0, x2     // 0x8b020000  step toward deadline in x20
     *   add x1, x1, #1     // 0x91000421  iteration count
     *   cmp x20, x0        // 0xeb00029f
     *   b.hi <loop>        // 0x54….08   taken while x20 > x0
     * x2 is 0 here, so x0 never reaches x20 and EL1 spins with I=1.
     * Set x0=x20 so b.hi falls through; leave x1/x2 alone.
     */
    i0 = guest_fetch_insn(tf->elr);
    i1 = guest_fetch_insn(tf->elr + 4);
    i2 = guest_fetch_insn(tf->elr + 8);
    i3 = guest_fetch_insn(tf->elr + 12);
    if (i0 == 0x8b020000u && i1 == 0x91000421u && i2 == 0xeb00029fu &&
        (i3 & 0xff00001fu) == 0x54000008u && tf->x[2] == 0) {
        static int skip_log;
        if (!skip_log) {
            hyp_debug("veil: skip zero-step delay elr=%p x0=%p x20=%p\n",
                      tf->elr, tf->x[0], tf->x[20]);
            skip_log = 1;
        }
        tf->x[0] = tf->x[20];
    }
}

int veil_timer_pending(void)
{
    return irq_id != 0 || pl011_irq_pending();
}

uint32_t icc_ack_irq(void)
{
    uint32_t id = irq_id;

    if (!id) {
        id = pending_intid();
    }
    if (id == VEIL_CNTV_INTID || id == VEIL_CNTP_INTID ||
        id == VEIL_UART_INTID) {
        irq_in_service(id);
        return id;
    }
    if (id == 48 || id == 49) {
        virtio_ack_irq(id);
        irq_id = 0;
        irq_st = IRQ_IN_SERVICE;
        return id;
    }
    return 1023;
}

void icc_eoi(uint32_t id)
{
    irq_idle();
    hyp_debug("veil: eoi id=%u cntv=0x%lx\n", id,
              (unsigned long)read_cntv_ctl_el0());
    (void)id;
}
