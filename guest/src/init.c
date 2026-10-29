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

int guest_mmio(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    return virtio_board_mmio(ipa, is_write, size, data);
}

int virtio_board_psci(struct trapframe *tf)
{
    return psci_call(tf);
}

int guest_sys64(struct trapframe *tf, uint32_t iss)
{
    int is_read = iss & 1u;
    unsigned crm = (iss >> 1) & 15u;
    unsigned rt = (iss >> 5) & 31u;
    unsigned crn = (iss >> 10) & 15u;
    unsigned op1 = (iss >> 14) & 7u;
    unsigned op2 = (iss >> 17) & 7u;
    unsigned op0 = (iss >> 20) & 3u;
    uint32_t insn = 0xd5000000u | (is_read ? (1u << 21) : 0) |
                    (op0 << 19) | (op1 << 16) | (crn << 12) | (crm << 8) |
                    (op2 << 5) | rt;

    /* VBAR_EL1 is not trapped (no HCR.TVBAR). Hook on HVC / tick instead. */
    if (icc_emulate(tf, insn) == 0) {
        return 0;
    }
    return veil_emulate_sys64(tf, iss);
}
