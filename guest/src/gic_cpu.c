#include "aarch64.h"
#include "veil.h"
#include "virtio_board.h"

/*
 * Guest GICv3 CPU interface. Pi 5 has no VGIC, so ICC_* UNDEF at EL1
 * and AAVMF IAR/EOI are patched to HVC (HVC_ICC_INSN).
 */

static uint32_t icc_sre = 0x7; /* SRE | DFB | DIB */
static uint32_t icc_ctlr;
static uint32_t icc_pmr = 0xff;
static uint32_t icc_bpr0 = 7;
static uint32_t icc_bpr1 = 7;
static uint32_t icc_igrpen0;
static uint32_t icc_igrpen1;
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

uint32_t icc_igrpen1_get(void)
{
    return icc_igrpen1;
}

int guest_pc_in_irq_slot(uint64_t elr)
{
    return hooked_vbar && elr >= hooked_vbar + VEIL_VBAR_IRQ_OFF &&
           elr < hooked_vbar + VEIL_VBAR_IRQ_END;
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
                uint32_t id = irq_latched_id();
                val = id ? id : 1023;
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

int guest_hvc(struct trapframe *tf, uint64_t esr)
{
    guest_vbar_hook();
#if VEIL_DEBUG
    {
        static unsigned hvc_log;
        if (hvc_log < 16u) {
            hyp_printf("veil: hvc esr=0x%lx elr=%p x0=%p\n",
                       (unsigned long)esr, tf->elr, tf->x[0]);
            hvc_log++;
        }
    }
#endif
    if (guest_el1_sync_from_hvc(tf) == 0) {
        return 0;
    }
    if ((esr & 0xffffu) == HVC_ICC_IMM) {
#if VEIL_DEBUG
        static unsigned miss;
        if (miss < 4u) {
            hyp_printf("veil: unmatched icc hvc elr=%p skip\n", tf->elr);
            miss++;
        }
#endif
        /* ELR is already the insn after HVC. */
        return 0;
    }
    virtio_board_psci(tf);
    tf->elr += 4;
    return 0;
}
