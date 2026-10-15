set architecture aarch64
set pagination off
file build/veil.elf
target remote :1234
break unexpected_trap
break veil_handle_trap
continue
