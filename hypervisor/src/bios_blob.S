    .section .rodata
    .global veil_bios_start
    .global veil_bios_end
    .align  4
veil_bios_start:
    .incbin "build/bios.bin"
veil_bios_end:
