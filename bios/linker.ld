OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

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
        . += 0x10000;
        __stack_top = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
