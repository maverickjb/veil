    .section .rodata
    .global veil_dtb_start
    .global veil_dtb_end
    .align  3
veil_dtb_start:
    .incbin "build/guest.dtb"
veil_dtb_end:
