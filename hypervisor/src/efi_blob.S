    .section .firmware, "a"
    .global veil_efi_start
    .global veil_efi_end
    .align  21
veil_efi_start:
    .incbin "bios/QEMU_EFI.fd"
veil_efi_end:
