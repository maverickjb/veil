#include "virtio_board.h"
#include "veil.h"

void virtio_board_init(void)
{
    virtio_init();
    dtb_init();
    smbios_init();
    virt_mmio_init();
}

int virtio_board_mmio(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    return virt_mmio_access(ipa, is_write, size, data);
}

int virtio_board_psci(struct trapframe *tf)
{
    return psci_call(tf);
}
