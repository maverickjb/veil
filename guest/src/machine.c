#include "aarch64.h"
#include "veil.h"
#include "virtio_board.h"

/* Supporting virt-machine devices around the virtio hardware layer. */

#define GICD_CTLR  0x000
#define GICD_TYPER 0x004
#define GICD_IIDR  0x008
#define GICD_PIDR2 0xffe8

#define GICR_CTLR  0x000
#define GICR_IIDR  0x004
#define GICR_TYPER 0x008
#define GICR_WAKER 0x014
#define GICR_PIDR2 0xffe8
#define GICR_SGI   0x10000

static uint32_t gicd_ctlr;
static uint32_t gicd_isenabler[8];
static uint32_t gicr_waker = 0x6; /* ProcessorSleep | ChildrenAsleep */
static uint32_t gicr_ctlr;
static uint32_t gicr_isenabler;

static uint64_t gicr_affinity(void)
{
    uint64_t m = read_mpidr_el1();

    return (m & 0xffull) |
           (((m >> 8) & 0xffull) << 8) |
           (((m >> 16) & 0xffull) << 16) |
           (((m >> 32) & 0xffull) << 24);
}

static uint64_t gicr_typer_val(void)
{
    /* Last redistributor; affinity must match this CPU's MPIDR. */
    return (gicr_affinity() << 32) | (1ull << 4);
}

static uint64_t gicd_read(uint64_t off)
{
    if (off == GICD_CTLR) {
        return gicd_ctlr & ~(1u << 31); /* RWP is RO and never busy */
    }
    if (off == GICD_TYPER) {
        /* 256 SPIs, IDBits=16, A3V, no LPIs. Close to QEMU virt GICv3. */
        return 0x007b0007u;
    }
    if (off == GICD_IIDR) {
        return 0x043b0010u;
    }
    if (off == GICD_PIDR2) {
        return 0x30u;
    }
    if (off >= 0x080 && off < 0x100) {
        return 0xffffffffu; /* IGROUPR: Group-1 NS */
    }
    if (off >= 0x100 && off < 0x180) {
        unsigned n = (unsigned)((off - 0x100) / 4);
        return (n < 8) ? gicd_isenabler[n] : 0;
    }
    return 0;
}

static void gicd_write(uint64_t off, uint64_t val)
{
    if (off == GICD_CTLR) {
        gicd_ctlr = (uint32_t)val & ~(1u << 31);
        return;
    }
    if (off >= 0x100 && off < 0x180) {
        unsigned n = (unsigned)((off - 0x100) / 4);
        if (n < 8) {
            gicd_isenabler[n] |= (uint32_t)val;
        }
        return;
    }
    if (off >= 0x180 && off < 0x200) {
        unsigned n = (unsigned)((off - 0x180) / 4);
        if (n < 8) {
            gicd_isenabler[n] &= ~(uint32_t)val;
        }
    }
}

static uint64_t gicr_read(uint64_t off, unsigned size)
{
    if (off == GICR_CTLR) {
        return gicr_ctlr & ~(1u << 3); /* RWP */
    }
    if (off == GICR_TYPER) {
        uint64_t typer = gicr_typer_val();
        return (size == 8) ? typer : (uint32_t)typer;
    }
    if (off == GICR_TYPER + 4) {
        return (uint32_t)(gicr_typer_val() >> 32);
    }
    if (off == GICR_WAKER) {
        return gicr_waker;
    }
    if (off == GICR_SGI + 0x80) {
        return 0xffffffffu; /* IGROUPR0: PPIs/SGIs Group-1 NS */
    }
    if (off == GICR_SGI + 0x100) {
        return gicr_isenabler;
    }
    if (off == GICR_SGI + 0x200) {
        uint64_t vctl = read_cntv_ctl_el0();
        uint64_t pctl = read_cntp_ctl_el0();
        uint32_t pend = 0;
        if ((vctl & 5u) == 5u && !(vctl & 2u)) {
            pend |= (1u << VEIL_CNTV_INTID);
        }
        if ((pctl & 5u) == 5u && !(pctl & 2u)) {
            pend |= (1u << VEIL_CNTP_INTID);
        }
        return pend;
    }
    if (off == GICR_PIDR2 || off == GICR_SGI + GICR_PIDR2) {
        return 0x30u;
    }
    return 0;
}

static void gicr_write(uint64_t off, uint64_t val)
{
    if (off == GICR_CTLR) {
        gicr_ctlr = (uint32_t)val & ~(1u << 3);
    }
    if (off == GICR_WAKER) {
        /* QEMU: ProcessorSleep=1 keeps ChildrenAsleep; clearing PS wakes. */
        if ((uint32_t)val & 0x2u) {
            gicr_waker = 0x6u;
        } else {
            gicr_waker = 0;
        }
    }
    if (off == GICR_SGI + 0x100) {
        gicr_isenabler |= (uint32_t)val;
    }
    if (off == GICR_SGI + 0x180) {
        gicr_isenabler &= ~(uint32_t)val;
    }
}

void virt_mmio_init(void)
{
    fw_cfg_init();
    pflash_init();
    pl011_init();
}

int virt_mmio_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    if (pflash_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (pl011_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (pl031_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (ipa >= VEIL_GICD_IPA && ipa < VEIL_GICD_IPA + VEIL_GICD_SIZE) {
        uint64_t off = ipa - VEIL_GICD_IPA;
        if (is_write) {
            gicd_write(off, *data);
        } else {
            *data = gicd_read(off);
        }
        return 0;
    }
    if (ipa >= VEIL_GICR_IPA && ipa < VEIL_GICR_IPA + VEIL_GICR_DTB_SIZE) {
        uint64_t off = (ipa - VEIL_GICR_IPA) % VEIL_GICR_SIZE;
        if (is_write) {
            gicr_write(off, *data);
        } else {
            *data = gicr_read(off, size);
        }
        return 0;
    }
    if (fw_cfg_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (virtio_mmio_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    /* PCI ECAM: empty slots are 0xFFFFFFFF, not vendor 0. */
    if (ipa >= VEIL_PCI_ECAM_IPA && ipa < VEIL_PCI_ECAM_IPA + VEIL_PCI_ECAM_SIZE) {
        if (!is_write) {
            *data = ~0ull;
        }
        return 0;
    }
    /* Other virt-window probes: RAZ/WI so AAVMF can keep going. */
    if ((ipa >= VEIL_VIRT_MMIO_LO && ipa < VEIL_VIRT_MMIO_HI) || ipa >= 0x400000000ull) {
        if (!is_write) {
            *data = 0;
        }
        return 0;
    }
    return -1;
}

static uint32_t icc_sre = 0x7; /* SRE | DFB | DIB */
static uint32_t icc_ctlr;
static uint32_t icc_pmr = 0xff;
static uint32_t icc_bpr0 = 7;
static uint32_t icc_bpr1 = 7;
static uint32_t icc_igrpen0;
static uint32_t icc_igrpen1;
static uint32_t irq_id;
static uint64_t hooked_vbar;
static uint64_t hooked_vec_pa;
static uint64_t hooked_irq_pa;
static uint32_t saved_vec_insn;
static uint32_t saved_irq_insn;
static uint32_t irq_vec_words[4];
#define ICC_PATCH_MAX 8
static uint64_t icc_patch_va[ICC_PATCH_MAX];
static uint32_t icc_patch_insn[ICC_PATCH_MAX];
static unsigned icc_patch_n;

/*
 * One guest vIRQ:
 *   source ready (CNTV/UART/…)  → irq_pending(id)  (may set HCR.VI)
 *   IAR via patched HVC         → irq_in_service(id)  (drops VI)
 *   EOI                         → irq_idle()          (drops VI)
 * Poll may latch the next id while the handler still runs; VI stays down
 * until PSTATE.I=0 and ELR is outside the VBAR IRQ slots. Holding VI with
 * I=1 freezes this A76 (EL1 stuck on one add after EOI).
 */
enum {
    IRQ_IDLE = 0,
    IRQ_PENDING,
    IRQ_IN_SERVICE,
};

static unsigned irq_st;

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
    int in_irq_vec = hooked_vbar && elr >= hooked_vbar + VEIL_VBAR_IRQ_OFF &&
                     elr < hooked_vbar + VEIL_VBAR_IRQ_END;

    /* PSTATE.I or still in the VBAR IRQ/FIQ/SError slots: do not assert VI. */
    return in_irq_vec || (read_spsr_el2() & (1ull << 7)) != 0;
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
                guest_fetch_insn(elr), vbar, i280, icc_igrpen1,
                gicr_isenabler, irq_id, (hcr & HCR_EL2_VI) ? 1u : 0u,
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

void veil_timer_poll(void)
{
    guest_vbar_hook();
    pl011_pull_host();
    fb_ramfb_present();
    {
        uint64_t vctl = read_cntv_ctl_el0();
        if (fb_ramfb_allow_virq() && (vctl & 3ull) != 0) {
            static int unmask_cntv;
            if (!unmask_cntv) {
                uint64_t frq = read_cntfrq_el0();
                if (frq == 0) {
                    frq = 54000000ull;
                }
                write_cntv_ctl_el0((vctl | 1ull) & ~2ull);
                write_cntv_tval_el0(frq / 200ull);
                isb();
                gicr_isenabler |= (1u << VEIL_CNTV_INTID);
                unmask_cntv = 1;
                hyp_debug("veil: cntv kick was=0x%lx now=0x%lx\n",
                          (unsigned long)vctl,
                          (unsigned long)read_cntv_ctl_el0());
            }
        }
    }
    irq_pending(pending_intid());
    isb();
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
        gicd_isenabler[1] |= (1u << 1); /* SPI 1 = VEIL_UART_INTID */
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

int icc_emulate(struct trapframe *tf, uint32_t insn)
{
    unsigned op0 = (insn >> 19) & 3u;
    unsigned op1 = (insn >> 16) & 7u;
    unsigned crn = (insn >> 12) & 15u;
    unsigned crm = (insn >> 8) & 15u;
    unsigned op2 = (insn >> 5) & 7u;
    unsigned rt = insn & 31u;
    int is_mrs = (insn >> 21) & 1;
    uint64_t val = 0;
    int handled = 0;

    if (op0 != 3 || op1 != 0) {
        return -1;
    }

    if (crn == 4 && crm == 6 && op2 == 0) {
        /* ICC_PMR_EL1 */
        handled = 1;
        if (is_mrs) {
            val = icc_pmr;
        } else if (rt != 31) {
            icc_pmr = (uint32_t)tf->x[rt] & 0xffu;
        }
    } else if (crn == 12) {
        handled = 1;
        if (is_mrs) {
            if (crm == 12 && op2 == 5) {
                val = icc_sre | 0x7u;
            } else if (crm == 12 && op2 == 4) {
                val = icc_ctlr;
            } else if (crm == 12 && op2 == 3) {
                val = icc_bpr1;
            } else if (crm == 8 && op2 == 3) {
                val = icc_bpr0;
            } else if (crm == 12 && op2 == 6) {
                val = icc_igrpen0;
            } else if (crm == 12 && op2 == 7) {
                val = icc_igrpen1;
            } else if (crm == 12 && op2 == 0) {
                val = icc_ack_irq();
            } else if (crm == 8 && op2 == 0) {
                val = 1023; /* ICC_IAR0_EL1 */
            } else if (crm == 11 && op2 == 3) {
                val = 0; /* ICC_RPR_EL1 idle */
            } else if (crm == 12 && op2 == 2) {
                val = irq_id ? irq_id : 1023;
            } else {
                val = 0;
            }
        } else if (rt != 31) {
            uint32_t w = (uint32_t)tf->x[rt];
            if (crm == 12 && op2 == 5) {
                icc_sre = w | 0x7u;
            } else if (crm == 12 && op2 == 4) {
                icc_ctlr = w;
            } else if (crm == 12 && op2 == 3) {
                icc_bpr1 = w & 7u;
            } else if (crm == 8 && op2 == 3) {
                icc_bpr0 = w & 7u;
            } else if (crm == 12 && op2 == 6) {
                icc_igrpen0 = w & 1u;
            } else if (crm == 12 && op2 == 7) {
                icc_igrpen1 = w & 1u;
            } else if (crm == 12 && op2 == 1) {
                icc_eoi(w);
            }
            /* DIR/SGI: ignore */
        }
    }

    if (!handled) {
        return -1;
    }
    hyp_debug("veil: icc %s crm=%u op2=%u val=0x%x elr=%p\n",
              is_mrs ? "mrs" : "msr", crm, op2,
              is_mrs ? (unsigned)val : (rt != 31 ? (unsigned)tf->x[rt] : 0u),
              tf->elr);
    if (is_mrs && rt != 31) {
        tf->x[rt] = val;
    }
    return 0;
}

static void patch_hvc(uint64_t va, uint64_t *pa_out, uint32_t *saved)
{
    uint64_t pa = 0;
    uint32_t *p;

    if (guest_va_to_pa(va, &pa) != 0) {
        return;
    }
    p = (uint32_t *)(uintptr_t)pa;
    __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
    dsb_sy();
    if (*p == HVC_ICC_INSN) {
        *pa_out = pa;
        return;
    }
    *saved = *p;
    *p = HVC_ICC_INSN;
    dsb_sy();
    __asm__ volatile("dc cvau, %0" ::"r"(p) : "memory");
    *pa_out = pa;
}

static int insn_is_icc(uint32_t insn)
{
    unsigned op0 = (insn >> 19) & 3u;
    unsigned op1 = (insn >> 16) & 7u;
    unsigned crn = (insn >> 12) & 15u;
    unsigned crm = (insn >> 8) & 15u;
    unsigned op2 = (insn >> 5) & 7u;
    int mrs = (insn >> 21) & 1;

    if (op0 != 3 || op1 != 0 || crn != 12) {
        return 0;
    }
    if ((insn & 0xff800000u) != 0xd5000000u) {
        return 0;
    }
    /* IAR1 / EOIR1 / HPPIR1 / IAR0 / EOIR0 only — not AP0R0/SRE/BPR.
     * Any Rt. AAVMF 2024.02 CpuDxe uses x0: ICC_IAR1_X0_INSN / ICC_EOIR1_X0_INSN. */
    if (mrs && crm == 12 && op2 == 0) {
        return 1;
    }
    if (!mrs && crm == 12 && op2 == 1) {
        return 1;
    }
    if (mrs && crm == 12 && op2 == 2) {
        return 1;
    }
    if (mrs && crm == 8 && op2 == 0) {
        return 1;
    }
    if (!mrs && crm == 8 && op2 == 1) {
        return 1;
    }
    return 0;
}

static uint64_t b_target(uint64_t pc, uint32_t insn)
{
    int64_t imm = (int32_t)((insn & 0x03ffffffu) << 6) >> 6;

    return pc + (uint64_t)(imm * 4);
}

static void patch_icc_at(uint64_t va, uint32_t insn)
{
    uint64_t pa = 0;
    uint32_t *p;
    unsigned i;

    if (!insn_is_icc(insn) || icc_patch_n >= ICC_PATCH_MAX) {
        return;
    }
    for (i = 0; i < icc_patch_n; i++) {
        if (icc_patch_va[i] == va) {
            return;
        }
    }
    if (guest_va_to_pa(va, &pa) != 0) {
        return;
    }
    p = (uint32_t *)(uintptr_t)pa;
    icc_patch_va[icc_patch_n] = va;
    icc_patch_insn[icc_patch_n] = insn;
    *p = HVC_ICC_INSN;
    dsb_sy();
    __asm__ volatile("dc cvau, %0" ::"r"(p) : "memory");
    dsb_ish();
    __asm__ volatile("ic ivau, %0" ::"r"(p) : "memory");
    hyp_debug("veil: icc patch va=%p insn=0x%x\n", va, insn);
    icc_patch_n++;
}

static void scan_icc(uint64_t va0, unsigned n)
{
    unsigned i = 0;

    while (i < n && icc_patch_n < ICC_PATCH_MAX) {
        uint64_t va = va0 + (uint64_t)i * 4u;
        uint64_t pa = 0;
        unsigned j, chunk;

        if (guest_va_to_pa(va, &pa) != 0) {
            i++;
            continue;
        }
        chunk = (unsigned)((4096u - (unsigned)(pa & 0xfffu)) / 4u);
        if (chunk > n - i) {
            chunk = n - i;
        }
        guest_mem_sync(pa, (size_t)chunk * 4u);
        for (j = 0; j < chunk && icc_patch_n < ICC_PATCH_MAX; j++) {
            uint32_t insn = *(volatile uint32_t *)(uintptr_t)(pa + (uint64_t)j * 4u);
            patch_icc_at(va + (uint64_t)j * 4u, insn);
        }
        i += chunk;
    }
}

void guest_vbar_hook(void)
{
    uint64_t vbar;
    unsigned i;

    if (BOARD_GIC_PASSTHROUGH) {
        return;
    }
    vbar = read_vbar_el1();
    if (vbar == hooked_vbar && hooked_vec_pa) {
        return;
    }
    /* VBAR=0 is flash padding; patching it corrupts QEMU_EFI.fd at 0x200. */
    if (vbar < VEIL_GUEST_RAM_IPA) {
        return;
    }
    for (i = 0; i < 4u; i++) {
        irq_vec_words[i] = guest_fetch_insn(vbar + VEIL_VBAR_IRQ_OFF + (uint64_t)i * 4u);
    }
    patch_hvc(vbar + VEIL_VBAR_SYNC_OFF, &hooked_vec_pa, &saved_vec_insn);
    hooked_irq_pa = 0;
    saved_irq_insn = irq_vec_words[0];
    icc_patch_n = 0;
    scan_icc(vbar + VEIL_VBAR_IRQ_OFF, 32);
    for (i = 0; i < 32u; i++) {
        uint64_t va = vbar + VEIL_VBAR_IRQ_OFF + (uint64_t)i * 4u;
        uint32_t insn = guest_fetch_insn(va);
        uint32_t op = insn & 0xfc000000u;

        if (op == 0x14000000u || op == 0x94000000u) {
            uint64_t tgt = b_target(va, insn);
            unsigned k;
            hyp_debug("veil: irq vec %s %p -> %p 0x%x 0x%x 0x%x 0x%x\n",
                      op == 0x94000000u ? "bl" : "b", va, tgt,
                      guest_fetch_insn(tgt), guest_fetch_insn(tgt + 4),
                      guest_fetch_insn(tgt + 8), guest_fetch_insn(tgt + 12));
            scan_icc(tgt, 1024);
            for (k = 0; k < 64u; k++) {
                uint32_t ins2 = guest_fetch_insn(tgt + (uint64_t)k * 4u);
                uint32_t op2 = ins2 & 0xfc000000u;
                if (op2 == 0x14000000u || op2 == 0x94000000u) {
                    uint64_t t2 = b_target(tgt + (uint64_t)k * 4u, ins2);
                    scan_icc(t2, 512);
                }
            }
        }
    }
    /* AAVMF 2024.02 CpuDxe: IAR/EOI (ICC_IAR1_X0_INSN / ICC_EOIR1_X0_INSN)
     * at VBAR+AAVMF_CPUDXE_IAR_OFF. Scan from AAVMF_CPUDXE_IAR_SCAN. */
    {
        unsigned k;
        uint64_t base = vbar + AAVMF_CPUDXE_IAR_SCAN;
        hyp_debug("veil: iar probe @%p = 0x%x loop @%p 0x%x 0x%x 0x%x 0x%x\n",
                  vbar + AAVMF_CPUDXE_IAR_OFF,
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_OFF),
                  vbar + AAVMF_CPUDXE_IAR_NEAR,
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_NEAR),
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_NEAR + 4ull),
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_NEAR + 8ull),
                  guest_fetch_insn(vbar + AAVMF_CPUDXE_IAR_NEAR + 12ull));
        for (k = 0; k < 4096u && icc_patch_n < ICC_PATCH_MAX; k++) {
            uint64_t va = base + (uint64_t)k * 4u;
            patch_icc_at(va, guest_fetch_insn(va));
        }
    }
    __asm__ volatile("ic ialluis" ::: "memory");
    dsb_ish();
    isb();
    if (hooked_vec_pa) {
        hooked_vbar = vbar;
        hyp_debug("veil: vbar hook %p irq0=0x%x 0x%x 0x%x 0x%x npatch=%u\n", vbar,
                  irq_vec_words[0], irq_vec_words[1], irq_vec_words[2],
                  irq_vec_words[3], icc_patch_n);
    }
}

int guest_el1_sync_from_hvc(struct trapframe *tf)
{
    uint64_t esr1 = read_esr_el1();
    uint64_t elr1 = read_elr_el1();
    uint32_t ec1 = (uint32_t)((esr1 >> ESR_EC_SHIFT) & ESR_EC_MASK);
    uint32_t insn = 0;
    uint64_t off;
    uint64_t hvc_pc = tf->elr >= 4ull ? tf->elr - 4ull : tf->elr;
    unsigned i;

    /* HVC preferred return is the next insn, so ELR_EL2 is patch VA + 4. */
    for (i = 0; i < icc_patch_n; i++) {
        if (tf->elr == icc_patch_va[i] || hvc_pc == icc_patch_va[i]) {
            if (icc_emulate(tf, icc_patch_insn[i]) == 0) {
                if (tf->elr == icc_patch_va[i]) {
                    tf->elr += 4;
                }
                return 0;
            }
        }
    }

    off = hooked_vbar ? tf->elr - hooked_vbar : ~0ull;
    if (off >= VEIL_VBAR_SYNC_OFF && off < VEIL_VBAR_IRQ_OFF) {
        if (ec1 == 0) {
            insn = guest_fetch_insn(elr1);
            if (insn && icc_emulate(tf, insn) == 0) {
                tf->elr = elr1 + 4;
                tf->spsr = read_spsr_el1();
                return 0;
            }
        }
        hyp_debug("veil: el1 sync via hook esr=0x%lx elr1=%p insn=0x%x hvc=%p\n",
                  (unsigned long)esr1, elr1, insn, tf->elr);
        return -1;
    }

    /* Our HVC imm from a VBAR we have not recorded yet (race on first ICC). */
    if (ec1 == 0) {
        insn = guest_fetch_insn(elr1);
        if (insn && icc_emulate(tf, insn) == 0) {
            tf->elr = elr1 + 4;
            tf->spsr = read_spsr_el1();
            return 0;
        }
    }
    return -1;
}
