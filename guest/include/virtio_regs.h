#pragma once

#define VIRTIO_MMIO_MAGIC_VALUE           0x000
#define VIRTIO_MMIO_VERSION               0x004
#define VIRTIO_MMIO_DEVICE_ID             0x008
#define VIRTIO_MMIO_VENDOR_ID             0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES       0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL   0x014
#define VIRTIO_MMIO_DRIVER_FEATURES       0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL   0x024
#define VIRTIO_MMIO_QUEUE_SEL             0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX         0x034
#define VIRTIO_MMIO_QUEUE_NUM             0x038
#define VIRTIO_MMIO_QUEUE_READY           0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY          0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS      0x060
#define VIRTIO_MMIO_INTERRUPT_ACK         0x064
#define VIRTIO_MMIO_STATUS                0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW        0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH       0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW       0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH      0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW        0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH       0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION     0x0fc
#define VIRTIO_MMIO_CONFIG               0x100

#define VIRTIO_MMIO_MAGIC                 0x74726976u /* "virt" */
#define VIRTIO_MMIO_VERSION_MODERN        2u
#define VIRTIO_VENDOR_VEIL                0x5645494cu /* "VEIL" */

#define VIRTIO_ID_CONSOLE                 3u
#define VIRTIO_ID_BLOCK                   2u

#define VIRTIO_F_VERSION_1                32u
#define VIRTIO_BLK_F_FLUSH                0u

#define VIRTIO_BLK_T_IN                   0u
#define VIRTIO_BLK_T_OUT                  1u
#define VIRTIO_BLK_T_FLUSH                4u
#define VIRTIO_BLK_S_OK                   0u
#define VIRTIO_BLK_S_IOERR                1u
#define VIRTIO_BLK_S_UNSUPP               2u

#define VIRTIO_STATUS_ACKNOWLEDGE         1u
#define VIRTIO_STATUS_DRIVER              2u
#define VIRTIO_STATUS_DRIVER_OK           4u
#define VIRTIO_STATUS_FEATURES_OK         8u
#define VIRTIO_STATUS_FAILED              128u

#define VIRTQ_DESC_F_NEXT                 1u
#define VIRTQ_DESC_F_WRITE                2u

#define VIRTIO_CONSOLE_VQ_RX              0u
#define VIRTIO_CONSOLE_VQ_TX              1u
