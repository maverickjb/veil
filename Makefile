CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# Host SoC (real map) vs guest/ (presented virt machine, not a host).
PLATFORM ?= qemu-virt
ifeq ($(PLATFORM),raspi3)
BOARD := raspi3
else ifeq ($(PLATFORM),raspi5)
BOARD := raspi5
else
BOARD := qemu-virt
endif

# aavmf = QEMU's own ARM UEFI (QEMU_EFI.fd). mini = the tiny virtio BIOS.
GUEST ?= aavmf
EFI_FD ?= /usr/share/qemu-efi-aarch64/QEMU_EFI.fd
BUILD := build

ifeq ($(GUEST),mini)
CPPFLAGS += -DGUEST_MINI
GUEST_OBJ := $(BUILD)/bios_blob.o
else
CPPFLAGS += -DGUEST_AAVMF
GUEST_OBJ := $(BUILD)/efi_blob.o $(BUILD)/disk_blob.o
endif

CFLAGS := -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
          -mgeneral-regs-only -mstrict-align -nostdlib -nostartfiles \
          -std=c11 -Wall -Wextra -Werror -O2 -g \
          -I include -I hypervisor/include \
          -I board/$(BOARD)/include -I guest/include
LDFLAGS := -nostdlib -nostartfiles -no-pie -Wl,--build-id=none -static

HYP_CORE := hypervisor/src/boot.S \
            hypervisor/src/exception.S \
            hypervisor/src/main.c \
            hypervisor/src/lib.c \
            hypervisor/src/mmu.c

BOARD_SRCS := board/$(BOARD)/src/board.c \
              board/$(BOARD)/src/fb.c

VIRT_SRCS := guest/src/init.c \
             guest/src/virtio.c \
             guest/src/machine.c \
             guest/src/dtb.c

HYP_SRCS := $(HYP_CORE) $(BOARD_SRCS) $(VIRT_SRCS)
HYP_LD := board/$(BOARD)/linker.ld

BIOS_SRCS := bios/src/start.S bios/src/main.c

.PHONY: all clean run test debug ref testdisk

all: $(BUILD)/veil.elf $(BUILD)/veil.bin

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bios.elf: $(BIOS_SRCS) bios/linker.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -T bios/linker.ld -o $@ $(BIOS_SRCS)

$(BUILD)/bios.bin: $(BUILD)/bios.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/bios_blob.o: hypervisor/src/bios_blob.S $(BUILD)/bios.bin | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ hypervisor/src/bios_blob.S

$(BUILD)/qemu_efi.fd: $(EFI_FD) | $(BUILD)
	cp $< $@

$(BUILD)/efi_blob.o: hypervisor/src/efi_blob.S $(BUILD)/qemu_efi.fd | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ hypervisor/src/efi_blob.S

$(BUILD)/esp.img: efi/hello.S scripts/mk-testdisk.py | $(BUILD)
	python3 scripts/mk-testdisk.py

$(BUILD)/disk_blob.o: hypervisor/src/disk_blob.S $(BUILD)/esp.img | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ hypervisor/src/disk_blob.S

$(BUILD)/veil.elf: $(HYP_SRCS) $(GUEST_OBJ) $(HYP_LD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -T $(HYP_LD) \
		-o $@ $(HYP_SRCS) $(GUEST_OBJ)

$(BUILD)/veil.bin: $(BUILD)/veil.elf
	$(OBJCOPY) -O binary $< $@

run: $(BUILD)/veil.elf $(BUILD)/veil.bin
	PLATFORM=$(PLATFORM) GUEST=$(GUEST) ./scripts/run-qemu.sh

test: $(BUILD)/veil.elf $(BUILD)/veil.bin
	PLATFORM=$(PLATFORM) GUEST=$(GUEST) ./scripts/run-qemu.sh --test

# Veil + QEMU gdbstub (attach with gdb-multiarch -x scripts/gdb-veil.gdb)
debug: $(BUILD)/veil.elf $(BUILD)/veil.bin
	PLATFORM=$(PLATFORM) GUEST=$(GUEST) ./scripts/run-qemu.sh --debug

$(BUILD)/testdisk.img: $(BUILD)/esp.img

testdisk: $(BUILD)/testdisk.img

# Stock QEMU virt + AAVMF + testdisk (no Veil)
ref: $(BUILD)/testdisk.img
	./scripts/run-ref-uefi.sh

clean:
	rm -rf $(BUILD)
