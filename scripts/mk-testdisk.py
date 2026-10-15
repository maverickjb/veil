#!/usr/bin/env python3
"""Build BOOTAA64.EFI (PE32+) and a GPT disk with a FAT16 ESP."""
from __future__ import annotations

import binascii
import os
import struct
import subprocess
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
SRC = ROOT / "efi" / "hello.S"
EFI_OUT = BUILD / "BOOTAA64.EFI"
DISK = BUILD / "testdisk.img"
ESP_IMG = BUILD / "esp.img"
ESP_SIZE = 4 * 1024 * 1024
ESP_PART_START = 40

# EFI System Partition GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B (mixed endian)
ESP_TYPE_GUID = bytes.fromhex("28732AC11FF8D211BA4B00A0C93EC93B")

CROSS = os.environ.get("CROSS", "aarch64-linux-gnu-")
SECTOR = 512
PART_START = 2048  # 1 MiB
DISK_SIZE = 64 * 1024 * 1024


def run(cmd: list[str]) -> None:
    subprocess.check_call(cmd)


def build_efi_payload() -> bytes:
    BUILD.mkdir(parents=True, exist_ok=True)
    obj = BUILD / "efi_hello.o"
    elf = BUILD / "efi_hello.elf"
    raw = BUILD / "efi_hello.bin"
    run([f"{CROSS}as", "-o", str(obj), str(SRC)])
    run(
        [
            f"{CROSS}ld",
            "-Ttext=0",
            "-e",
            "efi_main",
            "-o",
            str(elf),
            str(obj),
        ]
    )
    run([f"{CROSS}objcopy", "-O", "binary", str(elf), str(raw)])
    return raw.read_bytes()


def wrap_pe(code: bytes) -> bytes:
    file_align = 0x200
    sect_align = 0x1000
    hdr_size = 0x200
    code_raw = (len(code) + file_align - 1) & ~(file_align - 1)
    code_va = (len(code) + sect_align - 1) & ~(sect_align - 1)
    if code_va < sect_align:
        code_va = sect_align
    size_of_image = sect_align + code_va
    entry = sect_align  # RVA of .text

    dos = bytearray(0x40)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)

    coff = struct.pack(
        "<HHIIIHH",
        0xAA64,  # Machine
        1,  # NumberOfSections
        0,  # TimeDateStamp
        0,
        0,
        0xF0,  # SizeOfOptionalHeader PE32+
        0x0022,  # EXECUTABLE | stripped line nums
    )

    opt = bytearray(0xF0)
    struct.pack_into("<HBB", opt, 0, 0x20B, 2, 40)  # PE32+, linker 2.40
    struct.pack_into("<I", opt, 4, code_raw)  # SizeOfCode
    struct.pack_into("<I", opt, 16, entry)  # AddressOfEntryPoint
    struct.pack_into("<I", opt, 20, sect_align)  # BaseOfCode
    struct.pack_into("<Q", opt, 24, 0)  # ImageBase (relocated)
    struct.pack_into("<I", opt, 32, sect_align)
    struct.pack_into("<I", opt, 36, file_align)
    struct.pack_into("<I", opt, 56, size_of_image)
    struct.pack_into("<I", opt, 60, hdr_size)
    struct.pack_into("<HH", opt, 68, 10, 0)  # Subsystem EFI_APPLICATION
    struct.pack_into("<Q", opt, 72, 0x100000)  # StackReserve
    struct.pack_into("<Q", opt, 80, 0x1000)  # StackCommit
    struct.pack_into("<I", opt, 108, 16)  # NumberOfRvaAndSizes

    sect = struct.pack(
        "<8sIIIIIIHHI",
        b".text",
        code_va,
        sect_align,
        code_raw,
        hdr_size,
        0,
        0,
        0,
        0,
        0x60000020,  # CODE | EXECUTE | READ
    )

    pe = dos + b"PE\0\0" + coff + opt + sect
    pe = pe.ljust(hdr_size, b"\0")
    pe += code.ljust(code_raw, b"\0")
    return bytes(pe)


def fat16_image(
    payload: bytes,
    payload_name: str,
    part_bytes: int,
    hidden: int = PART_START,
    spc: int = 8,
) -> bytes:
    bps = SECTOR
    reserved = 1
    nfats = 2
    root_ent = 512
    media = 0xF8
    total = part_bytes // bps
    root_secs = (root_ent * 32 + bps - 1) // bps
    # FAT16 size: iterate once
    fat_secs = 1
    for _ in range(4):
        data_secs = total - reserved - nfats * fat_secs - root_secs
        clusters = data_secs // spc
        fat_secs = ((clusters + 2) * 2 + bps - 1) // bps
    data_secs = total - reserved - nfats * fat_secs - root_secs
    clusters = data_secs // spc
    if clusters < 0xFF5 or clusters > 0xFFF4:
        raise SystemExit(f"cluster count {clusters} not FAT16")

    img = bytearray(part_bytes)
    jmp = b"\xeb\x3c\x90"
    oem = b"VEILDISK"
    bpb = struct.pack(
        "<HBHBHHBHHHII",
        bps,
        spc,
        reserved,
        nfats,
        root_ent,
        0,
        media,
        fat_secs,
        63,
        255,
        hidden,
        total,
    )
    boot = bytearray(bps)
    boot[0:3] = jmp
    boot[3:11] = oem
    boot[11:36] = bpb
    boot[36] = 0x80
    boot[38] = 0x29
    boot[39:43] = b"VEIL"
    boot[43:54] = b"ESP        "
    boot[54:62] = b"FAT16   "
    boot[510] = 0x55
    boot[511] = 0xAA
    img[0:bps] = boot

    fat = bytearray(fat_secs * bps)
    struct.pack_into("<HH", fat, 0, 0xFFF8, 0xFFFF)

    def set_fat(cl: int, val: int) -> None:
        struct.pack_into("<H", fat, cl * 2, val)

    root = bytearray(root_secs * bps)
    data_off = (reserved + nfats * fat_secs + root_secs) * bps
    next_cl = 2

    def alloc_clusters(n: int) -> int:
        nonlocal next_cl
        first = next_cl
        for i in range(n):
            cl = first + i
            set_fat(cl, 0xFFFF if i == n - 1 else cl + 1)
        next_cl += n
        return first

    def dirent(name: bytes, ext: bytes, attr: int, cl: int, size: int) -> bytes:
        e = bytearray(32)
        e[0:8] = name.ljust(8)
        e[8:11] = ext.ljust(3)
        e[11] = attr
        struct.pack_into("<H", e, 26, cl)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def write_dir_table(entries: list[bytes], first_cl: int | None) -> None:
        raw = bytearray()
        for e in entries:
            raw += e
        if first_cl is None:
            root[: len(raw)] = raw
            return
        ncl = (len(raw) + spc * bps - 1) // (spc * bps)
        # already allocated
        off = data_off + (first_cl - 2) * spc * bps
        img[off : off + len(raw)] = raw

    # EFI\BOOT\BOOTAA64.EFI
    efi_cl = alloc_clusters(1)
    boot_cl = alloc_clusters(1)
    file_ncl = (len(payload) + spc * bps - 1) // (spc * bps)
    file_cl = alloc_clusters(file_ncl)
    off = data_off + (file_cl - 2) * spc * bps
    img[off : off + len(payload)] = payload

    write_dir_table(
        [
            dirent(b".", b"   ", 0x10, boot_cl, 0),
            dirent(b"..", b"   ", 0x10, efi_cl, 0),
            dirent(b"BOOTAA64", b"EFI", 0x20, file_cl, len(payload)),
        ],
        boot_cl,
    )
    write_dir_table(
        [
            dirent(b".", b"   ", 0x10, efi_cl, 0),
            dirent(b"..", b"   ", 0x10, 0, 0),
            dirent(b"BOOT", b"   ", 0x10, boot_cl, 0),
        ],
        efi_cl,
    )
    root[0:32] = dirent(b"EFI", b"   ", 0x10, efi_cl, 0)

    fat_off = reserved * bps
    img[fat_off : fat_off + len(fat)] = fat
    img[fat_off + fat_secs * bps : fat_off + 2 * fat_secs * bps] = fat
    root_off = (reserved + nfats * fat_secs) * bps
    img[root_off : root_off + len(root)] = root
    return bytes(img)


def _crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def gpt_header(
    current: int,
    backup: int,
    first_usable: int,
    last_usable: int,
    disk_guid: bytes,
    ents_lba: int,
    ents_crc: int,
) -> bytes:
    h = bytearray(SECTOR)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)
    struct.pack_into("<I", h, 12, 92)
    struct.pack_into("<QQQQ", h, 24, current, backup, first_usable, last_usable)
    h[56:72] = disk_guid
    struct.pack_into("<QII", h, 72, ents_lba, 128, 128)
    struct.pack_into("<I", h, 88, ents_crc)
    struct.pack_into("<I", h, 16, _crc32(h[:92]))
    return bytes(h)


def write_gpt_disk(path: Path, fat: bytes, disk_size: int, part_start: int) -> None:
    nlba = disk_size // SECTOR
    last = nlba - 1
    backup_header = last
    backup_ents = last - 32
    first_usable = 34
    last_usable = last - 33
    part_end = last_usable
    img = bytearray(disk_size)

    img[446:462] = struct.pack(
        "<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, nlba - 1
    )
    img[510] = 0x55
    img[511] = 0xAA

    disk_guid = uuid.UUID("5645494C-0000-4000-8000-000000000001").bytes_le
    part_guid = uuid.UUID("5645494C-0000-4000-8000-0000000000E1").bytes_le
    ents = bytearray(128 * 128)
    ents[0:16] = ESP_TYPE_GUID
    ents[16:32] = part_guid
    struct.pack_into("<QQ", ents, 32, part_start, part_end)
    name = "ESP".encode("utf-16le")
    ents[56 : 56 + len(name)] = name
    ents_crc = _crc32(ents)

    img[SECTOR : 2 * SECTOR] = gpt_header(
        1, backup_header, first_usable, last_usable, disk_guid, 2, ents_crc
    )
    img[2 * SECTOR : 2 * SECTOR + len(ents)] = ents
    off = part_start * SECTOR
    if off + len(fat) > disk_size:
        raise SystemExit("FAT larger than GPT partition")
    img[off : off + len(fat)] = fat
    img[backup_ents * SECTOR : backup_ents * SECTOR + len(ents)] = ents
    img[backup_header * SECTOR : backup_header * SECTOR + SECTOR] = gpt_header(
        backup_header, 1, first_usable, last_usable, disk_guid, backup_ents, ents_crc
    )
    path.write_bytes(img)


def gpt_and_part(fat: bytes) -> None:
    run(["qemu-img", "create", "-f", "raw", str(DISK), str(DISK_SIZE)])
    run(["sgdisk", "-Z", str(DISK)])
    run(
        [
            "sgdisk",
            "-n",
            f"1:{PART_START}:0",
            "-t",
            "1:ef00",
            "-c",
            "1:ESP",
            str(DISK),
        ]
    )
    data = bytearray(DISK.read_bytes())
    off = PART_START * SECTOR
    if off + len(fat) > len(data):
        raise SystemExit("partition smaller than FAT image")
    data[off : off + len(fat)] = fat
    DISK.write_bytes(data)


def main() -> int:
    code = build_efi_payload()
    efi = wrap_pe(code)
    EFI_OUT.write_bytes(efi)
    part_bytes = DISK_SIZE - PART_START * SECTOR - 33 * SECTOR  # room for backup GPT
    fat = fat16_image(efi, "BOOTAA64.EFI", part_bytes)
    gpt_and_part(fat)

    small_part = ESP_SIZE - ESP_PART_START * SECTOR - 33 * SECTOR
    small_fat = fat16_image(
        efi, "BOOTAA64.EFI", small_part, hidden=ESP_PART_START, spc=1
    )
    write_gpt_disk(ESP_IMG, small_fat, ESP_SIZE, ESP_PART_START)
    print(f"wrote {EFI_OUT} ({len(efi)} bytes)")
    print(f"wrote {DISK} ({DISK_SIZE} bytes, GPT + FAT16 ESP)")
    print(f"wrote {ESP_IMG} ({ESP_SIZE} bytes, embedded virtio-blk)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
