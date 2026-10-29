#include "virtio_board.h"

/* Virt-machine MMIO router. Device implementations live in sibling .c files. */

void virt_mmio_init(void)
{
    fw_cfg_init();
    pflash_init();
    pl011_init();
}

int virt_mmio_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    if (pflash_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (pl011_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (pl031_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (gic_dist_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (fw_cfg_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    if (virtio_mmio_access(ipa, is_write, size, data) == 0) {
        return 0;
    }
    /* PCI ECAM: empty slots are 0xFFFFFFFF, not vendor 0. */
    if (ipa >= VEIL_PCI_ECAM_IPA && ipa < VEIL_PCI_ECAM_IPA + VEIL_PCI_ECAM_SIZE) {
        if (!is_write) {
            *data = ~0ull;
        }
        return 0;
    }
    /* Other virt-window probes: RAZ/WI so AAVMF can keep going. */
    if ((ipa >= VEIL_VIRT_MMIO_LO && ipa < VEIL_VIRT_MMIO_HI) || ipa >= 0x400000000ull) {
        if (!is_write) {
            *data = 0;
        }
        return 0;
    }
    return -1;
}
