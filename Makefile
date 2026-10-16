CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
DTC     ?= $(firstword $(wildcard $(CURDIR)/tools/dtc) $(wildcard /usr/bin/dtc) dtc)

# Host SoC (real map) vs guest/ (presented virt machine, not a host).
PLATFORM ?= qemu-virt
ifeq ($(PLATFORM),raspi3)
BOARD := raspi3
else ifeq ($(PLATFORM),raspi5)
BOARD := raspi5
else
BOARD := qemu-virt
endif

# aavmf = pre-built ARM UEFI in bios/QEMU_EFI.fd. mini = tiny virtio BIOS.
GUEST ?= aavmf
EFI_FD ?= bios/QEMU_EFI.fd
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
             guest/src/dtb.c \
             guest/src/smbios.c

HYP_SRCS := $(HYP_CORE) $(BOARD_SRCS) $(VIRT_SRCS)
HYP_LD := board/$(BOARD)/linker.ld
DTB_OBJ := $(BUILD)/dtb_blob.o
ifeq ($(BOARD),qemu-virt)
DTS_CPPFLAGS := -DVEIL_PCI=1
else
DTS_CPPFLAGS :=
endif

BIOS_SRCS := bios/src/start.S bios/src/main.c

.PHONY: all clean run test debug ref testdisk disk

all: $(BUILD)/veil.elf $(BUILD)/veil.bin

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bios.elf: $(BIOS_SRCS) bios/linker.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -T bios/linker.ld -o $@ $(BIOS_SRCS)

$(BUILD)/bios.bin: $(BUILD)/bios.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/bios_blob.o: hypervisor/src/bios_blob.S $(BUILD)/bios.bin | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ hypervisor/src/bios_blob.S

$(BUILD)/efi_blob.o: hypervisor/src/efi_blob.S $(EFI_FD) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ hypervisor/src/efi_blob.S

$(BUILD)/esp.img: efi/hello.S scripts/mk-testdisk.py | $(BUILD)
	python3 scripts/mk-testdisk.py

$(BUILD)/disk_blob.o: hypervisor/src/disk_blob.S $(BUILD)/esp.img | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ hypervisor/src/disk_blob.S

$(BUILD)/guest.dtb: guest/dts/virt.dts | $(BUILD)
	cpp -nostdinc -undef -P -x assembler-with-cpp $(DTS_CPPFLAGS) $< | \
		$(DTC) -I dts -O dtb -o $@

$(BUILD)/dtb_blob.o: guest/src/dtb_blob.S $(BUILD)/guest.dtb | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ guest/src/dtb_blob.S

$(BUILD)/veil.elf: $(HYP_SRCS) $(GUEST_OBJ) $(DTB_OBJ) $(HYP_LD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -T $(HYP_LD) \
		-o $@ $(HYP_SRCS) $(GUEST_OBJ) $(DTB_OBJ)

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

# Empty sparse hard disk for AAVMF / an OS install (qemu-img + virtio-blk-pci).
DISK ?= $(BUILD)/disk.img
DISK_SIZE ?= 8G
disk: | $(BUILD)
	DISK=$(DISK) DISK_SIZE=$(DISK_SIZE) ./scripts/mk-disk.sh

# Stock QEMU virt + AAVMF + testdisk (no Veil)
ref: $(BUILD)/testdisk.img
	./scripts/run-ref-uefi.sh

clean:
	rm -rf $(BUILD)
