#pragma once

/*
 * Guest GIC CPU interface, VBAR slots, and the ICC hook.
 *
 * ARM architecture (same on any GICv3 virt machine):
 *   PPI/SPI INTIDs, VBAR_ELn vector offsets, ICC_IAR1/EOIR1 encodings.
 *
 * This QEMU_EFI.fd only (AAVMF 2024.02 CpuDxe):
 *   Where CpuDxe placed those ICC insns relative to VBAR.
 *   Re-measure if you swap firmware.
 */

/* virt.dts timer: <1 11 4> virtual, <1 14 4> EL1 physical. INTID = 16 + PPI. */
#define VEIL_CNTV_INTID 27u
#define VEIL_CNTP_INTID 30u
/* virt.dts pl011: <0 1 4>. INTID = 32 + SPI. */
#define VEIL_UART_INTID 33u

/* AArch64 VBAR: Current EL, SP_ELx. IRQ_END is the first Lower EL AArch64 slot. */
#define VEIL_VBAR_SYNC_OFF 0x200ull
#define VEIL_VBAR_IRQ_OFF  0x280ull
#define VEIL_VBAR_IRQ_END  0x400ull

/* Veil-chosen HVC imm (not an ARM-assigned value). Patches ICC_* to this. */
#define HVC_ICC_IMM  0x5645u
#define HVC_ICC_INSN (0xD4000002u | (HVC_ICC_IMM << 5))

/*
 * mrs x0, ICC_IAR1_EL1 / msr ICC_EOIR1_EL1, x0 — ARM encodings, Rt = x0.
 * AAVMF 2024.02 CpuDxe uses these two at VBAR + AAVMF_CPUDXE_IAR_OFF.
 */
#define ICC_IAR1_X0_INSN  0xd538cc00u
#define ICC_EOIR1_X0_INSN 0xd518cc20u

/* Scan window and IAR site inside this AAVMF CpuDxe image (from VBAR). */
#define AAVMF_CPUDXE_IAR_SCAN 0x5e000ull
#define AAVMF_CPUDXE_IAR_OFF  0x5f9fcu
#define AAVMF_CPUDXE_IAR_NEAR 0x5f960ull
