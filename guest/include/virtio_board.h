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

/* EL1 sync dispatch. Host IRQ / WFX idle tick separately. */
void guest_wfx(struct trapframe *tf, uint32_t iss);
int guest_hvc(struct trapframe *tf, uint64_t esr);
int guest_sys64(struct trapframe *tf, uint32_t iss);
int guest_mmio(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
void guest_cntv_kick(void);

int icc_emulate(struct trapframe *tf, uint32_t insn);
void guest_vbar_hook(void);
int guest_el1_sync_from_hvc(struct trapframe *tf);
int guest_pc_in_irq_slot(uint64_t elr);
uint32_t icc_igrpen1_get(void);
uint32_t irq_latched_id(void);
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
void pl011_init(void);
int pl011_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
void pl011_pull_host(void);
int pl011_has_rx(void);
int pl011_irq_pending(void);
uint8_t pl011_rx_front(void);
unsigned pl011_rx_count(void);
int gic_dist_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data);
void gic_enable_intid(uint32_t intid);
uint32_t gicr_isenabler_get(void);
void dtb_init(void);
const uint8_t *dtb_blob(void);
uint32_t dtb_size(void);
void smbios_init(void);
const uint8_t *smbios_tables_blob(void);
uint32_t smbios_tables_size(void);
const uint8_t *smbios_anchor_blob(void);
uint32_t smbios_anchor_size(void);
