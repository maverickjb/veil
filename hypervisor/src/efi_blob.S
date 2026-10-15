    .section .firmware, "a"
    .global veil_efi_start
    .global veil_efi_end
    .align  21
veil_efi_start:
    .incbin "build/qemu_efi.fd"
veil_efi_end:
