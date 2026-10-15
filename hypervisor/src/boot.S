.section .text.boot, "ax"
.global _start
_start:
    /* Park every core except CPU0. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbnz    x0, .park

    mrs     x1, currentel
    lsr     x1, x1, #2
    cmp     x1, #3
    b.eq    .drop_el3
    cmp     x1, #2
    b.ne    .park
    b       .el2_ready

.drop_el3:
    /* NS | IRQ | FIQ | HCE | RW — a 64-bit non-secure EL2. */
    mov     x0, #0x531
    msr     scr_el3, x0
    msr     cptr_el3, xzr
    adr     x0, .el2_ready
    msr     elr_el3, x0
    mov     x0, #0x3c9          /* EL2h, DAIF masked */
    msr     spsr_el3, x0
    isb
    eret

.el2_ready:
    /* nVHE CPTR_EL2 RES1, TFP=0: do not trap FP/SIMD to EL2. */
    mov     x0, #0x33ff
    msr     cptr_el2, x0
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    mov     x0, #1
    msr     spsel, x0
    ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =el2_vectors
    msr     vbar_el2, x0
    isb

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:
    cmp     x0, x1
    b.ge    2f
    str     xzr, [x0], #8
    b       1b
2:
    bl      veil_main

.park:
    wfe
    b       .park
