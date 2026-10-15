#include <stddef.h>
#include <stdint.h>

#include "veil_abi.h"
#include "virtio_regs.h"

#define QSZ 8

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSZ];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[QSZ];
};

static volatile uint32_t *mmio;
static struct virtq_desc desc[QSZ] __attribute__((aligned(16)));
static struct virtq_avail avail __attribute__((aligned(2)));
static struct virtq_used used __attribute__((aligned(4)));
static uint16_t last_used;
static char txbuf[512];

static void dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t vr(unsigned off)
{
    return mmio[off / 4];
}

static void vw(unsigned off, uint32_t v)
{
    mmio[off / 4] = v;
}

static void virtio_fail(const char *why)
{
    (void)why;
    for (;;) {
        __asm__ volatile("wfe");
    }
}

static void setup_queue(uint32_t sel)
{
    vw(VIRTIO_MMIO_QUEUE_SEL, sel);
    if (vr(VIRTIO_MMIO_QUEUE_NUM_MAX) < QSZ) {
        virtio_fail("queue");
    }
    vw(VIRTIO_MMIO_QUEUE_NUM, QSZ);
    vw(VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)(uintptr_t)desc);
    vw(VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)((uint64_t)(uintptr_t)desc >> 32));
    vw(VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)(uintptr_t)&avail);
    vw(VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)((uint64_t)(uintptr_t)&avail >> 32));
    vw(VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)(uintptr_t)&used);
    vw(VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)((uint64_t)(uintptr_t)&used >> 32));
    vw(VIRTIO_MMIO_QUEUE_READY, 1);
}

static void virtio_console_init(uint64_t base)
{
    mmio = (volatile uint32_t *)(uintptr_t)base;

    if (vr(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) {
        virtio_fail("magic");
    }
    if (vr(VIRTIO_MMIO_VERSION) != VIRTIO_MMIO_VERSION_MODERN) {
        virtio_fail("version");
    }
    if (vr(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_ID_CONSOLE) {
        virtio_fail("id");
    }

    vw(VIRTIO_MMIO_STATUS, 0);
    vw(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    vw(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t hi = vr(VIRTIO_MMIO_DEVICE_FEATURES);
    vw(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    vw(VIRTIO_MMIO_DRIVER_FEATURES, hi & (1u << (VIRTIO_F_VERSION_1 - 32)));

    vw(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    (void)vr(VIRTIO_MMIO_DEVICE_FEATURES);
    vw(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    vw(VIRTIO_MMIO_DRIVER_FEATURES, 0);

    vw(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                               VIRTIO_STATUS_FEATURES_OK);
    if (!(vr(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        virtio_fail("features");
    }

    setup_queue(VIRTIO_CONSOLE_VQ_TX);

    vw(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                               VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
}

static void virtio_write(const char *s, size_t n)
{
    if (n == 0 || n > sizeof(txbuf)) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        txbuf[i] = s[i];
    }

    desc[0].addr = (uint64_t)(uintptr_t)txbuf;
    desc[0].len = (uint32_t)n;
    desc[0].flags = 0;
    desc[0].next = 0;

    uint16_t aidx = avail.idx;
    avail.ring[aidx % QSZ] = 0;
    dsb();
    avail.idx = (uint16_t)(aidx + 1);
    dsb();

    vw(VIRTIO_MMIO_QUEUE_SEL, VIRTIO_CONSOLE_VQ_TX);
    vw(VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_CONSOLE_VQ_TX);

    while (used.idx == last_used) {
        __asm__ volatile("yield");
    }
    last_used = used.idx;
}

static void bios_puts(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    virtio_write(s, n);
}

void bios_main(struct veil_boot_info *info)
{
    uint64_t base = VEIL_VIRTIO_CONSOLE_IPA;
    if (info && info->magic == VEIL_BOOT_MAGIC) {
        base = info->virtio_base;
    }

    virtio_console_init(base);

    bios_puts("Veil virtio BIOS 0.1\n");
    bios_puts("guest running at EL1; no host uart/gpio/soc registers visible\n");
    bios_puts("virtio-console online\n");

    for (;;) {
        __asm__ volatile("wfe");
    }
}
