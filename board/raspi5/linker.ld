OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    /* Same load address the Pi firmware uses for kernel_2712.img. */
    . = 0x00080000;

    .text : ALIGN(8) {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(8) {
        *(.data .data.*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;

        . = ALIGN(16);
        __stack_bottom = .;
        . += 0x10000;
        __stack_top = .;
    }

    /* 2 MiB-aligned so stage-2 can map QEMU_EFI.fd as one block. */
    . = ALIGN(0x200000);
    .firmware : ALIGN(0x200000) {
        *(.firmware)
    }

    .disk : ALIGN(8) {
        *(.disk)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
