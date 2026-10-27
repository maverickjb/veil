#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "virt_machine.h"
#include "virtio_board.h"

struct trapframe {
    uint64_t x[31];
    uint64_t sp_el1;
    uint64_t elr;
    uint64_t spsr;
};

/* lib.c */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);
int memcmp(const void *a, const void *b, size_t n);
void hyp_printf(const char *fmt, ...);

/* mmu.c */
void s2_init(void);
int s2_ipa_to_pa(uint64_t ipa, uint64_t *pa, size_t *remain);
int guest_va_to_pa(uint64_t va, uint64_t *pa);
uint32_t guest_fetch_insn(uint64_t va);
void guest_mem_sync(uint64_t pa, size_t n);
int s2_ramfb_watch(uint64_t ipa, size_t n);
int s2_ramfb_fault(uint64_t ipa);
void s2_ramfb_ro(void);

/* main / trap */
void veil_handle_trap(struct trapframe *tf);
void enter_guest(uint64_t entry, uint64_t arg0);
void dump_guest_undef(void);
int veil_emulate_sys64(struct trapframe *tf, uint32_t iss);

extern char veil_bios_start[] __attribute__((weak));
extern char veil_bios_end[] __attribute__((weak));
extern char veil_efi_start[] __attribute__((weak));
extern char veil_efi_end[] __attribute__((weak));
extern char veil_disk_start[] __attribute__((weak));
extern char veil_disk_end[] __attribute__((weak));
