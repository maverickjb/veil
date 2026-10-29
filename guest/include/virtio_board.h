#pragma once

#include <stddef.h>
#include <stdint.h>

#include "virt_machine.h"

struct trapframe;

/*
 * Guest hardware layer: virtio-mmio (console + block) on a QEMU virt-shaped
 * machine. AAVMF also needs the supporting virt devices (PL011, GICv3,
 * fw_cfg, PL031, CFI vars, PSCI); those live next to virtio in guest/.
 *
 * Host SoC code is not here. EL2 traps call virtio_board_mmio(); virtio
 * devices reach the host only through uart_putc / s2_ipa_to_pa.
 */

void virtio_board_init(void);
int virtio_board_mmio(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
int virtio_board_psci(struct trapframe *tf);

int icc_emulate(struct trapframe *tf, uint32_t insn);
void guest_vbar_hook(void);
int guest_el1_sync_from_hvc(struct trapframe *tf);
void veil_timer_poll(void);
void veil_timer_poll_tf(struct trapframe *tf);
void veil_wfx_idle(struct trapframe *tf);
int veil_timer_pending(void);
uint32_t icc_ack_irq(void);
void icc_eoi(uint32_t id);
int psci_call(struct trapframe *tf);

void virtio_init(void);
int virtio_mmio_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
uint32_t virtio_pending_intid(void);
void virtio_ack_irq(uint32_t intid);
void virt_mmio_init(void);
int virt_mmio_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
int pl031_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
void pflash_init(void);
int pflash_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
void fw_cfg_init(void);
int fw_cfg_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
void dtb_init(void);
const uint8_t *dtb_blob(void);
uint32_t dtb_size(void);
void smbios_init(void);
const uint8_t *smbios_tables_blob(void);
uint32_t smbios_tables_size(void);
const uint8_t *smbios_anchor_blob(void);
uint32_t smbios_anchor_size(void);
