#pragma once

#include <stdint.h>

#define currentel_el(x) (((x) >> 2) & 3u)

#define HCR_EL2_VM   (1ull << 0)
#define HCR_EL2_SWIO (1ull << 1)
#define HCR_EL2_PTW  (1ull << 2)
#define HCR_EL2_FMO  (1ull << 3)
#define HCR_EL2_IMO  (1ull << 4)
#define HCR_EL2_AMO  (1ull << 5)
#define HCR_EL2_VF   (1ull << 6)
#define HCR_EL2_VI   (1ull << 7)
#define HCR_EL2_TWI  (1ull << 13)
#define HCR_EL2_TWE  (1ull << 14)
#define HCR_EL2_TID3 (1ull << 18)
#define HCR_EL2_TSC  (1ull << 19)
#define HCR_EL2_RW   (1ull << 31)

#define SPSR_EL1H    0x5u
#define SPSR_DAIF    (0xfu << 6)

#define ESR_EC_SHIFT 26
#define ESR_EC_MASK  0x3fu
#define ESR_EC_WFX      0x01u
#define ESR_EC_DABT_LOW 0x24u
#define ESR_EC_IABT_LOW 0x20u
#define ESR_EC_SVC64    0x15u
#define ESR_EC_HVC64    0x16u
#define ESR_EC_SMC64    0x17u
#define ESR_EC_SYS64    0x18u

#define ESR_ISS_ISV  (1u << 24)
#define ESR_ISS_SAS_SHIFT 22
#define ESR_ISS_SRT_SHIFT 16
#define ESR_ISS_WNR  (1u << 6)
#define ESR_ISS_SAS(iss) (((iss) >> ESR_ISS_SAS_SHIFT) & 3u)
#define ESR_ISS_SRT(iss) (((iss) >> ESR_ISS_SRT_SHIFT) & 0x1fu)

#define SCTLR_EL1_RES1 0x30C50830ull

/* nVHE reset: RES1 bits [11],[9],[7:0]; TFP=0 so FP/SIMD is not trapped to EL2. */
#define CPTR_EL2_RES1 0x33FFull
#define CPACR_EL1_FPEN (3ull << 20)

#define CNTHCTL_EL1PCTEN (1u << 0)
#define CNTHCTL_EL1PCEN  (1u << 1)

static inline uint64_t read_sysreg_currentel(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, currentel" : "=r"(v));
    return v;
}

#define SYSREG_READ(name)                                      \
    static inline uint64_t read_##name(void)                   \
    {                                                          \
        uint64_t v;                                            \
        __asm__ volatile("mrs %0, " #name : "=r"(v));          \
        return v;                                              \
    }

#define SYSREG_WRITE(name)                                     \
    static inline void write_##name(uint64_t v)                \
    {                                                          \
        __asm__ volatile("msr " #name ", %0" ::"r"(v));        \
    }

SYSREG_READ(esr_el2)
SYSREG_READ(far_el2)
SYSREG_READ(elr_el2)
SYSREG_READ(spsr_el2)
SYSREG_READ(hcr_el2)
SYSREG_WRITE(hcr_el2)
SYSREG_WRITE(vbar_el2)
SYSREG_WRITE(vttbr_el2)
SYSREG_WRITE(vtcr_el2)
SYSREG_WRITE(sctlr_el1)
SYSREG_READ(sctlr_el1)
SYSREG_WRITE(sctlr_el2)
SYSREG_WRITE(tcr_el1)
SYSREG_READ(tcr_el1)
SYSREG_WRITE(ttbr0_el1)
SYSREG_READ(ttbr0_el1)
SYSREG_WRITE(mair_el1)
SYSREG_WRITE(cptr_el2)
SYSREG_WRITE(cpacr_el1)
SYSREG_READ(cpacr_el1)
SYSREG_READ(elr_el1)
SYSREG_READ(esr_el1)
SYSREG_READ(far_el1)
SYSREG_READ(vbar_el1)
SYSREG_READ(spsr_el1)
SYSREG_READ(id_aa64pfr0_el1)
SYSREG_READ(id_aa64pfr1_el1)
SYSREG_READ(id_aa64dfr0_el1)
SYSREG_READ(id_aa64isar0_el1)
SYSREG_READ(id_aa64isar1_el1)
SYSREG_READ(id_aa64mmfr0_el1)
SYSREG_READ(id_aa64mmfr1_el1)
SYSREG_READ(id_aa64mmfr2_el1)
SYSREG_READ(midr_el1)
SYSREG_READ(mpidr_el1)
SYSREG_READ(revidr_el1)
SYSREG_READ(cntpct_el0)
SYSREG_READ(cntfrq_el0)
SYSREG_READ(cntvct_el0)
SYSREG_READ(cntv_ctl_el0)
SYSREG_READ(cntv_cval_el0)
SYSREG_READ(cntp_ctl_el0)
SYSREG_READ(cntp_cval_el0)
SYSREG_WRITE(cnthctl_el2)
SYSREG_WRITE(cntvoff_el2)
SYSREG_WRITE(spsr_el2)
SYSREG_WRITE(elr_el2)
SYSREG_WRITE(sp_el1)

/* GICv3: keep EL2 access, force EL1 ICC_* to UNDEF so Veil can emulate them. */
static inline void write_icc_sre_el2(uint64_t v)
{
    __asm__ volatile("msr s3_4_c12_c9_5, %0" ::"r"(v));
}

static inline void write_icc_sre_el1(uint64_t v)
{
    __asm__ volatile("msr s3_0_c12_c12_5, %0" ::"r"(v));
}

static inline void isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static inline void dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void dsb_ish(void)
{
    __asm__ volatile("dsb ish" ::: "memory");
}

static inline void tlbi_vmalls12e1is(void)
{
    __asm__ volatile("tlbi vmalls12e1is" ::: "memory");
}

static inline void tlbi_alle2is(void)
{
    __asm__ volatile("tlbi alle2is" ::: "memory");
}

static inline void wfe(void)
{
    __asm__ volatile("wfe");
}

static inline void halt(void)
{
    for (;;) {
        __asm__ volatile("wfe");
    }
}
