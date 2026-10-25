#include "aarch64.h"
#include "veil.h"

static void unexpected_trap(struct trapframe *tf, uint64_t esr, uint64_t far)
{
    uint32_t ec = (uint32_t)((esr >> ESR_EC_SHIFT) & ESR_EC_MASK);
    hyp_printf("veil: trap ec=0x%x esr=0x%lx far=%p elr=%p\n",
               ec, (unsigned long)esr, far, tf->elr);
    halt();
}

static int decode_ldrstr(uint32_t insn, unsigned *rt, unsigned *rt2, unsigned *size,
                         int *is_write, int *is_pair)
{
    uint32_t op = insn & 0xffc00000u;
    *rt2 = 0;
    *is_pair = 0;

    /* LDP/STP signed offset (32/64-bit integer). */
    if (op == 0xa9000000u || op == 0xa9400000u || op == 0xa9800000u || op == 0xa9c00000u ||
        op == 0x29000000u || op == 0x29400000u || op == 0x29800000u || op == 0x29c00000u) {
        *is_pair = 1;
        *is_write = (op == 0xa9000000u || op == 0xa9800000u ||
                     op == 0x29000000u || op == 0x29800000u);
        *size = (op == 0xa9000000u || op == 0xa9400000u ||
                 op == 0xa9800000u || op == 0xa9c00000u) ? 8 : 4;
        *rt = insn & 31u;
        *rt2 = (insn >> 10) & 31u;
        return 0;
    }

    if (op == 0xb9000000u || op == 0xb8000000u || op == 0xb8200000u) {
        *is_write = 1;
        *size = 4;
        *rt = insn & 31u;
        return 0;
    }
    if (op == 0xb9400000u || op == 0xb8400000u || op == 0xb8600000u) {
        *is_write = 0;
        *size = 4;
        *rt = insn & 31u;
        return 0;
    }
    if (op == 0xf9000000u || op == 0xf8000000u || op == 0xf8200000u) {
        *is_write = 1;
        *size = 8;
        *rt = insn & 31u;
        return 0;
    }
    if (op == 0xf9400000u || op == 0xf8400000u || op == 0xf8600000u) {
        *is_write = 0;
        *size = 8;
        *rt = insn & 31u;
        return 0;
    }
    if (op == 0x39000000u || op == 0x38000000u) {
        *is_write = 1;
        *size = 1;
        *rt = insn & 31u;
        return 0;
    }
    if (op == 0x39400000u || op == 0x38400000u) {
        *is_write = 0;
        *size = 1;
        *rt = insn & 31u;
        return 0;
    }
    if (op == 0x79000000u || op == 0x78000000u) {
        *is_write = 1;
        *size = 2;
        *rt = insn & 31u;
        return 0;
    }
    if (op == 0x79400000u || op == 0x78400000u) {
        *is_write = 0;
        *size = 2;
        *rt = insn & 31u;
        return 0;
    }
    return -1;
}

void dump_guest_undef(void)
{
    uint64_t elr = read_elr_el1();
    uint64_t esr = read_esr_el1();
    uint64_t far = read_far_el1();
    uint64_t cpacr = read_cpacr_el1();
    uint32_t insn = guest_fetch_insn(elr);
    hyp_printf("veil: el1 esr=0x%lx elr=%p far=%p cpacr=0x%lx insn=0x%x\n",
               (unsigned long)esr, elr, far, (unsigned long)cpacr, insn);
}

void veil_handle_trap(struct trapframe *tf)
{
    uint64_t esr = read_esr_el2();
    uint64_t far = read_far_el2();
    uint32_t ec = (uint32_t)((esr >> ESR_EC_SHIFT) & ESR_EC_MASK);
    uint32_t iss = (uint32_t)(esr & 0x1ffffffu);
    unsigned size;
    unsigned rt;
    unsigned rt2 = 0;
    int is_write;
    int is_pair = 0;
    uint64_t data = 0;
    uint64_t data2 = 0;

    if (!BOARD_GIC_PASSTHROUGH) {
        guest_vbar_hook();
        veil_timer_poll();
    }

    if (ec == ESR_EC_WFX) {
        static int wfi_log;
        unsigned long spins = 0;
        if (!wfi_log) {
            wfi_log = 1;
            hyp_printf("veil: guest wfi\n");
        }
        while (!veil_timer_pending() && spins < 1000000ul) {
            veil_timer_poll();
            spins++;
        }
        tf->elr += 4;
        return;
    }

    if (ec == ESR_EC_HVC64) {
        if ((esr & 0xffffu) == 0x5645u) {
            if (guest_el1_sync_from_hvc(tf) != 0) {
                unexpected_trap(tf, esr, far);
            }
            return;
        }
        virtio_board_psci(tf);
        return;
    }
    if (ec == ESR_EC_SMC64) {
        virtio_board_psci(tf);
        tf->elr += 4;
        return;
    }
    if (ec == ESR_EC_SYS64) {
        int is_read = iss & 1u;
        unsigned crm = (iss >> 1) & 15u;
        unsigned rt = (iss >> 5) & 31u;
        unsigned crn = (iss >> 10) & 15u;
        unsigned op1 = (iss >> 14) & 7u;
        unsigned op2 = (iss >> 17) & 7u;
        unsigned op0 = (iss >> 20) & 3u;
        uint32_t insn = 0xd5000000u | (is_read ? (1u << 21) : 0) |
                        (op0 << 19) | (op1 << 16) | (crn << 12) | (crm << 8) |
                        (op2 << 5) | rt;
        if (icc_emulate(tf, insn) != 0 && veil_emulate_sys64(tf, iss) != 0) {
            unexpected_trap(tf, esr, far);
        }
        tf->elr += 4;
        return;
    }

    if (ec != ESR_EC_DABT_LOW) {
        unexpected_trap(tf, esr, far);
    }

    if (iss & ESR_ISS_ISV) {
        size = 1u << ESR_ISS_SAS(iss);
        rt = ESR_ISS_SRT(iss);
        is_write = (iss & ESR_ISS_WNR) != 0;
    } else {
        /* A76 S2 translation faults leave ISV=0. ELR_EL2 is a guest VA. */
        uint32_t insn = guest_fetch_insn(tf->elr);
        if (decode_ldrstr(insn, &rt, &rt2, &size, &is_write, &is_pair) != 0) {
            hyp_printf("veil: unhandled insn 0x%x at %p far=%p isv=0\n",
                       insn, tf->elr, far);
            unexpected_trap(tf, esr, far);
        }
    }

    {
        static int mmio_log;
        if (!mmio_log) {
            mmio_log = 1;
            hyp_printf("veil: guest mmio %s ipa=%p sz=%u elr=%p isv=%u\n",
                       is_write ? "w" : "r", far, size, tf->elr,
                       (iss & ESR_ISS_ISV) ? 1u : 0u);
        }
    }

    if (is_write && rt != 31) {
        data = tf->x[rt];
    }
    if (is_pair && is_write && rt2 != 31) {
        data2 = tf->x[rt2];
    }

    if (virtio_board_mmio(far, is_write, size, &data) != 0) {
        hyp_printf("veil: guest touched hidden hardware at %p\n", far);
        unexpected_trap(tf, esr, far);
    }
    if (is_pair && virtio_board_mmio(far + size, is_write, size, &data2) != 0) {
        hyp_printf("veil: guest touched hidden hardware at %p\n", far + size);
        unexpected_trap(tf, esr, far + size);
    }

    if (!is_write && rt != 31) {
        uint64_t mask = (size >= 8) ? ~0ull : ((1ull << (size * 8)) - 1ull);
        tf->x[rt] = (tf->x[rt] & ~mask) | (data & mask);
    }
    if (is_pair && !is_write && rt2 != 31) {
        uint64_t mask = (size >= 8) ? ~0ull : ((1ull << (size * 8)) - 1ull);
        tf->x[rt2] = (tf->x[rt2] & ~mask) | (data2 & mask);
    }

    tf->elr += 4;
}

#if defined(GUEST_MINI)
static void load_mini_bios(void)
{
    const struct platform *p = platform_get();
    size_t n = (size_t)(veil_bios_end - veil_bios_start);
    uint8_t *dst = (uint8_t *)p->guest_ram_pa;
    memcpy(dst, veil_bios_start, n);

    struct veil_boot_info info = {
        .magic = VEIL_BOOT_MAGIC,
        .virtio_base = VEIL_VIRTIO_CONSOLE_IPA,
        .virtio_size = VEIL_VIRTIO_MMIO_SIZE,
        .ram_base = VEIL_GUEST_RAM_IPA,
        .ram_size = p->guest_ram_size,
        .ncpus = 1,
    };
    memcpy(dst, &info, sizeof(info));
}
#endif

int veil_emulate_sys64(struct trapframe *tf, uint32_t iss)
{
    int is_read = iss & 1u;
    unsigned crm = (iss >> 1) & 15u;
    unsigned rt = (iss >> 5) & 31u;
    unsigned crn = (iss >> 10) & 15u;
    unsigned op1 = (iss >> 14) & 7u;
    unsigned op2 = (iss >> 17) & 7u;
    unsigned op0 = (iss >> 20) & 3u;
    uint64_t val = 0;

    if (!is_read) {
        return 0;
    }

    if (op0 == 3 && op1 == 0 && crn == 0) {
        if (crm == 0 && op2 == 0) {
            val = read_midr_el1();
        } else if (crm == 0 && op2 == 5) {
            val = read_mpidr_el1();
        } else if (crm == 0 && op2 == 6) {
            val = read_revidr_el1();
        } else if (crm == 4 && op2 == 0) {
            val = read_id_aa64pfr0_el1();
            val &= ~(0xfull << 24);
            val |= (1ull << 24);
        } else if (crm == 4 && op2 == 1) {
            val = read_id_aa64pfr1_el1();
        } else if (crm == 5 && op2 == 0) {
            val = read_id_aa64dfr0_el1();
        } else if (crm == 6 && op2 == 0) {
            val = read_id_aa64isar0_el1();
        } else if (crm == 6 && op2 == 1) {
            val = read_id_aa64isar1_el1();
        } else if (crm == 7 && op2 == 0) {
            val = read_id_aa64mmfr0_el1();
        } else if (crm == 7 && op2 == 1) {
            val = read_id_aa64mmfr1_el1();
        }
    }

    if (rt != 31) {
        tf->x[rt] = val;
    }
    return 0;
}

void enter_guest(uint64_t entry, uint64_t arg0)
{
    write_sctlr_el1(SCTLR_EL1_RES1);
    write_tcr_el1(0);
    write_ttbr0_el1(0);
    write_mair_el1(0);
    write_cptr_el2(CPTR_EL2_RES1);
    write_cpacr_el1(CPACR_EL1_FPEN);
    write_cnthctl_el2(CNTHCTL_EL1PCTEN | CNTHCTL_EL1PCEN);
    write_cntvoff_el2(0);
    write_sp_el1(VEIL_GUEST_RAM_IPA + 0x200000ull);

    uint64_t hcr = HCR_EL2_RW | HCR_EL2_VM | HCR_EL2_SWIO | HCR_EL2_TSC | HCR_EL2_TID3;
    if (!BOARD_GIC_PASSTHROUGH) {
        hcr |= HCR_EL2_IMO | HCR_EL2_TWI;
    } else {
        write_icc_sre_el2(0xfu);
        isb();
    }
    write_hcr_el2(hcr);
    isb();

    write_spsr_el2(SPSR_EL1H | SPSR_DAIF);
    write_elr_el2(entry);
    isb();

    __asm__ volatile(
        "mov x0, %[arg]\n"
        "mov x1, xzr\n"
        "mov x2, xzr\n"
        "mov x3, xzr\n"
        "eret\n"
        :
        : [arg] "r"(arg0)
        : "x0", "x1", "x2", "x3", "memory");
}

void veil_main(void)
{
    const struct platform *p = platform_get();
    uint64_t el = currentel_el(read_sysreg_currentel());

    uart_init();
    board_mask_host_irqs();
    hyp_printf("veil: cntfrq %u Hz\n", (unsigned)read_cntfrq_el0());
    if (fb_init() == 0) {
        hyp_printf("veil: host framebuffer ready\n");
        fb_selftest();
    } else {
        hyp_printf("veil: no host framebuffer (serial only)\n");
    }
    hyp_printf("veil: el%u hypervisor on %s (%s)\n",
               (unsigned)el, p->name, p->cpu_note);

    if (el != 2) {
        hyp_printf("veil: need EL2 (dropped from EL3 or started there)\n");
        halt();
    }

    virtio_board_init();
    s2_init();

    hyp_printf("veil: host uart %p hidden\n", (uint64_t)p->uart_base);
    hyp_printf("veil: s2 ram ipa %p -> pa %p (%u MiB)\n",
               (uint64_t)VEIL_GUEST_RAM_IPA, (uint64_t)p->guest_ram_pa,
               (unsigned)(p->guest_ram_size >> 20));

#if defined(GUEST_MINI)
    load_mini_bios();
    hyp_printf("veil: mini virtio bios at ipa %p\n", (uint64_t)VEIL_GUEST_RAM_IPA);
    enter_guest(VEIL_GUEST_RAM_IPA + 0x1000ull, VEIL_GUEST_RAM_IPA);
#else
    uint64_t efi_pa = (uint64_t)(uintptr_t)veil_efi_start;
    uint64_t efi_sz = (uint64_t)(veil_efi_end - veil_efi_start);
    memcpy((void *)p->guest_ram_pa, dtb_blob(), dtb_size());
    hyp_printf("veil: AAVMF QEMU_EFI.fd %u KiB at ipa 0 pa %p\n",
               (unsigned)(efi_sz >> 10), efi_pa);
    /* Flash boot: DTB lives at RAM base; x0 is 0, matching QEMU virt firmware. */
    enter_guest(VEIL_FLASH_IPA, 0);
#endif
    halt();
}
