#include "aarch64.h"
#include "veil.h"
#include "virtio_board.h"

int psci_call(struct trapframe *tf)
{
    uint32_t fid = (uint32_t)tf->x[0];
    switch (fid) {
    case 0x84000000u: /* PSCI_VERSION */
    case 0xc4000000u:
        tf->x[0] = 0x10001u;
        return 0;
    case 0x84000001u: /* CPU_SUSPEND */
    case 0xc4000001u:
        tf->x[0] = 0;
        return 0;
    case 0x84000002u: /* CPU_OFF */
        tf->x[0] = (uint64_t)(int32_t)(-2); /* DENIED */
        return 0;
    case 0x84000003u: /* CPU_ON */
    case 0xc4000003u:
        tf->x[0] = (uint64_t)(int32_t)(-1);
        return 0;
    case 0x84000004u: /* AFFINITY_INFO */
    case 0xc4000004u:
        tf->x[0] = 0; /* ON */
        return 0;
    case 0x84000005u: /* MIGRATE */
        tf->x[0] = (uint64_t)(int32_t)(-1);
        return 0;
    case 0x84000006u: /* MIGRATE_INFO_TYPE: no Trusted OS */
        tf->x[0] = 2;
        return 0;
    case 0x84000008u: /* SYSTEM_OFF */
        hyp_printf("veil: PSCI SYSTEM_OFF\n");
        halt();
        return 0;
    case 0x84000009u: /* SYSTEM_RESET */
        hyp_printf("veil: PSCI SYSTEM_RESET\n");
        halt();
        return 0;
    case 0x8400000Au: /* PSCI_FEATURES */
        switch ((uint32_t)tf->x[1]) {
        case 0x84000000u:
        case 0x84000001u:
        case 0x84000006u:
        case 0x84000008u:
        case 0x84000009u:
        case 0x8400000Au:
            tf->x[0] = 0;
            break;
        default:
            tf->x[0] = (uint64_t)(int32_t)(-1);
            break;
        }
        return 0;
    default:
        tf->x[0] = (uint64_t)(int32_t)(-1); /* NOT_SUPPORTED */
        return 0;
    }
}
