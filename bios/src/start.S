.section .text.boot, "ax"
.global _start
_start:
    /* Hypervisor overwrites the first page with veil_boot_info; skip it. */
    adr     x1, _start
    and     x1, x1, #~0xfff
    add     x1, x1, #0x1000
    br      x1

    .org    0x1000
.global _bios_entry
_bios_entry:
    mov     x19, x0
    ldr     x1, =__stack_top
    mov     sp, x1

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:
    cmp     x1, x2
    b.ge    2f
    str     xzr, [x1], #8
    b       1b
2:
    mov     x0, x19
    bl      bios_main
3:
    wfe
    b       3b
