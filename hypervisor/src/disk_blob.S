    .section .disk, "aw"
    .global veil_disk_start
    .global veil_disk_end
    .align  3
veil_disk_start:
    .incbin "build/esp.img"
veil_disk_end:
