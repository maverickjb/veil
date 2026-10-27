.macro ventry label
    .align  7
    b       \label
.endm

.macro save_regs
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]
    mrs     x0, sp_el1
    mrs     x1, elr_el2
    mrs     x2, spsr_el2
    stp     x0, x1, [sp, #248]
    str     x2,     [sp, #264]
.endm

.macro restore_regs
    ldr     x2,     [sp, #264]
    ldp     x0, x1, [sp, #248]
    msr     spsr_el2, x2
    msr     elr_el2,  x1
    msr     sp_el1,   x0
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp, sp, #272
.endm

    .align  11
    .global el2_vectors
el2_vectors:
    ventry  hang
    ventry  hang
    ventry  hang
    ventry  hang

    ventry  el2_sync_curr
    ventry  el2_irq
    ventry  hang
    ventry  hang

    ventry  el1_sync
    ventry  el1_irq
    ventry  hang
    ventry  hang

    ventry  hang
    ventry  hang
    ventry  hang
    ventry  hang

el1_irq:
    save_regs
    bl      board_ack_host_irq
    mov     x0, sp
    bl      veil_timer_poll_tf
    restore_regs
    eret

el2_irq:
    save_regs
    bl      board_ack_host_irq
    mov     x0, sp
    bl      veil_timer_poll_tf
    restore_regs
    eret

el1_sync:
    save_regs
    mov     x0, sp
    bl      veil_handle_trap
    restore_regs
    eret

el2_sync_curr:
    save_regs
    mov     x0, sp
    bl      veil_handle_trap
    restore_regs
    eret

hang:
    wfe
    b       hang
