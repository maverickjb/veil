/*
 * Minimal AArch64 UEFI app. x0 = ImageHandle, x1 = SystemTable.
 * Prints to ConOut and waits so Boot Manager does not return immediately.
 */
        .text
        .global efi_main
        .align  2
efi_main:
        stp     x29, x30, [sp, #-32]!
        mov     x29, sp
        mov     x19, x1                 /* SystemTable */

        ldr     x20, [x19, #64]         /* ConOut */
        cbz     x20, .hang

        mov     x0, x20
        ldr     x1, [x20, #48]          /* ClearScreen */
        blr     x1

        mov     x0, x20
        adr     x1, .msg
        ldr     x2, [x20, #8]           /* OutputString */
        blr     x2

.hang:
        wfe
        b       .hang

        .align  1
.msg:
        /* CHAR16 */
        .short  'V','e','i','l',' ','t','e','s','t',' ','d','i','s','k','\r','\n'
        .short  'E','F','I',' ','B','O','O','T','\\','B','O','O','T','A','A','6','4','.','E','F','I','\r','\n'
        .short  0
