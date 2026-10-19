#include "aarch64.h"
#include "veil.h"
#include "virtio_board.h"
#include "virtio_regs.h"

/*
 * Virtio hardware layer: virtio-mmio 1.0 console (id 3) and block (id 2).
 * Guest sees these at VEIL_VIRTIO_* on the virt machine; the host SoC does not.
 */

#define VIRTQ_MAX 256
#define VIRTQ_MAX_QUEUES 4

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

struct virtq {
    uint32_t num;
    uint32_t ready;
    uint64_t desc;
    uint64_t avail;
    uint64_t used;
    uint16_t last_avail;
};

struct virtio_mmio_dev {
    uint64_t base;
    uint32_t device_id;
    uint64_t device_features;
    uint32_t features_sel;
    uint64_t driver_features;
    uint32_t driver_features_sel;
    uint32_t status;
    uint32_t queue_sel;
    uint32_t int_status;
    uint64_t capacity;
    uint8_t *disk;
    size_t disk_size;
    struct virtq q[VIRTQ_MAX_QUEUES];
};

static struct virtio_mmio_dev console;
static struct virtio_mmio_dev block;
static uint32_t irq_latch;

static void virtio_pend_spi(uint32_t spi)
{
#if BOARD_GIC_PASSTHROUGH
    uint32_t intid = 32u + spi;
    volatile uint32_t *pend =
        (volatile uint32_t *)(VEIL_GICD_IPA + 0x200ull + 4ull * (intid / 32u));
    *pend = 1u << (intid % 32u);
    dsb_sy();
#else
    (void)spi;
    irq_latch = 1;
    veil_timer_poll();
#endif
}

static void *ipa_ptr(uint64_t ipa, size_t need)
{
    uint64_t pa;
    size_t remain;
    if (s2_ipa_to_pa(ipa, &pa, &remain) || remain < need) {
        return 0;
    }
    return (void *)(uintptr_t)pa;
}

static void virtq_reset(struct virtq *q)
{
    memset(q, 0, sizeof(*q));
}

static void console_reset(void)
{
    uint64_t base = console.base;
    memset(&console, 0, sizeof(console));
    console.base = base;
    console.device_id = VIRTIO_ID_CONSOLE;
    console.device_features = (1ull << VIRTIO_F_VERSION_1);
}

static void block_reset(void)
{
    uint64_t base = block.base;
    uint8_t *disk = block.disk;
    size_t disk_size = block.disk_size;
    uint64_t cap = block.capacity;
    memset(&block, 0, sizeof(block));
    block.base = base;
    block.disk = disk;
    block.disk_size = disk_size;
    block.capacity = cap;
    block.device_id = VIRTIO_ID_BLOCK;
    block.device_features = (1ull << VIRTIO_F_VERSION_1) | (1ull << VIRTIO_BLK_F_FLUSH);
}

static void process_tx(struct virtq *q)
{
    if (!q->ready || q->num == 0 || q->num > VIRTQ_MAX) {
        return;
    }

    struct virtq_desc *desc = ipa_ptr(q->desc, sizeof(*desc) * q->num);
    struct virtq_avail *avail = ipa_ptr(q->avail, 4 + 2 * q->num);
    struct virtq_used *used = ipa_ptr(q->used, 4 + 8 * q->num);
    if (!desc || !avail || !used) {
        hyp_printf("veil: bad virtq ipa\n");
        return;
    }

    dsb_sy();
    while (q->last_avail != avail->idx) {
        uint16_t slot = q->last_avail % (uint16_t)q->num;
        uint16_t id = avail->ring[slot];
        if (id >= q->num) {
            break;
        }

        uint16_t desc_id = id;
        uint32_t written = 0;
        for (;;) {
            struct virtq_desc *d = &desc[desc_id];
            if (!(d->flags & VIRTQ_DESC_F_WRITE) && d->len) {
                uint8_t *buf = ipa_ptr(d->addr, d->len);
                if (buf) {
                    for (uint32_t i = 0; i < d->len; i++) {
                        uart_putc((char)buf[i]);
                    }
                    written += d->len;
                }
            }
            if (!(d->flags & VIRTQ_DESC_F_NEXT)) {
                break;
            }
            desc_id = d->next;
            if (desc_id >= q->num) {
                break;
            }
        }

        uint16_t uslot = used->idx % (uint16_t)q->num;
        used->ring[uslot].id = id;
        used->ring[uslot].len = written;
        dsb_sy();
        used->idx++;
        q->last_avail++;
        console.int_status |= 1u;
        virtio_pend_spi(16);
    }
}

static int copy_from_chain(struct virtq_desc *desc, uint16_t n, uint16_t id,
                           uint32_t skip, void *dst, uint32_t want)
{
    uint8_t *out = dst;
    uint32_t got = 0;
    uint16_t desc_id = id;
    uint32_t off = 0;
    for (;;) {
        struct virtq_desc *d = &desc[desc_id];
        if (d->len && !(d->flags & VIRTQ_DESC_F_WRITE)) {
            uint32_t start = off;
            uint32_t end = off + d->len;
            if (end > skip && got < want) {
                uint32_t from = (start < skip) ? (skip - start) : 0;
                uint32_t ncopy = d->len - from;
                if (ncopy > want - got) {
                    ncopy = want - got;
                }
                uint8_t *buf = ipa_ptr(d->addr, d->len);
                if (!buf) {
                    return -1;
                }
                memcpy(out + got, buf + from, ncopy);
                got += ncopy;
            }
            off = end;
        }
        if (!(d->flags & VIRTQ_DESC_F_NEXT) || desc_id == n) {
            break;
        }
        desc_id = d->next;
        if (desc_id >= n) {
            return -1;
        }
    }
    return (got == want) ? 0 : -1;
}

static int xfer_data(struct virtq_desc *desc, uint16_t n, uint16_t id, uint32_t skip,
                     uint8_t *disk, uint64_t disk_off, uint32_t len, int to_disk)
{
    uint16_t desc_id = id;
    uint32_t off = 0;
    uint32_t done = 0;
    for (;;) {
        struct virtq_desc *d = &desc[desc_id];
        int is_write = (d->flags & VIRTQ_DESC_F_WRITE) != 0;
        int take = to_disk ? !is_write : is_write;
        if (d->len && take) {
            uint32_t start = off;
            uint32_t end = off + d->len;
            if (end > skip && done < len) {
                uint32_t from = (start < skip) ? (skip - start) : 0;
                uint32_t ncopy = d->len - from;
                if (ncopy > len - done) {
                    ncopy = len - done;
                }
                uint8_t *buf = ipa_ptr(d->addr, d->len);
                if (!buf) {
                    return -1;
                }
                if (to_disk) {
                    memcpy(disk + disk_off + done, buf + from, ncopy);
                } else {
                    memcpy(buf + from, disk + disk_off + done, ncopy);
                }
                done += ncopy;
            }
            off = end;
        } else if (d->len) {
            off += d->len;
        }
        if (!(d->flags & VIRTQ_DESC_F_NEXT)) {
            break;
        }
        desc_id = d->next;
        if (desc_id >= n) {
            return -1;
        }
    }
    return (done == len) ? 0 : -1;
}

static int write_status(struct virtq_desc *desc, uint16_t n, uint16_t id, uint8_t st)
{
    uint16_t desc_id = id;
    uint16_t last = id;
    for (;;) {
        last = desc_id;
        if (!(desc[desc_id].flags & VIRTQ_DESC_F_NEXT)) {
            break;
        }
        desc_id = desc[desc_id].next;
        if (desc_id >= n) {
            return -1;
        }
    }
    if (!(desc[last].flags & VIRTQ_DESC_F_WRITE) || desc[last].len < 1) {
        return -1;
    }
    uint8_t *p = ipa_ptr(desc[last].addr, 1);
    if (!p) {
        return -1;
    }
    *p = st;
    return 0;
}

static void process_blk(struct virtq *q)
{
    if (!q->ready || q->num == 0 || q->num > VIRTQ_MAX || !block.disk) {
        return;
    }

    struct virtq_desc *desc = ipa_ptr(q->desc, sizeof(*desc) * q->num);
    struct virtq_avail *avail = ipa_ptr(q->avail, 4 + 2 * q->num);
    struct virtq_used *used = ipa_ptr(q->used, 4 + 8 * q->num);
    if (!desc || !avail || !used) {
        hyp_printf("veil: bad blk virtq ipa\n");
        return;
    }

    dsb_sy();
    while (q->last_avail != avail->idx) {
        uint16_t slot = q->last_avail % (uint16_t)q->num;
        uint16_t id = avail->ring[slot];
        uint8_t status = VIRTIO_BLK_S_IOERR;
        uint32_t written = 1;
        if (id < q->num) {
            struct {
                uint32_t type;
                uint32_t reserved;
                uint64_t sector;
            } hdr;
            if (copy_from_chain(desc, (uint16_t)q->num, id, 0, &hdr, 16) == 0) {
                if (hdr.type == VIRTIO_BLK_T_FLUSH) {
                    status = VIRTIO_BLK_S_OK;
                } else if (hdr.type == VIRTIO_BLK_T_IN || hdr.type == VIRTIO_BLK_T_OUT) {
                    uint64_t off = hdr.sector * 512ull;
                    uint32_t total = 0;
                    uint16_t desc_id = id;
                    for (;;) {
                        struct virtq_desc *d = &desc[desc_id];
                        total += d->len;
                        if (!(d->flags & VIRTQ_DESC_F_NEXT)) {
                            break;
                        }
                        desc_id = d->next;
                        if (desc_id >= q->num) {
                            total = 0;
                            break;
                        }
                    }
                    uint32_t nbytes = (total > 17u) ? (total - 17u) : 0;
                    if (nbytes && off + nbytes <= block.disk_size) {
                        int to_disk = (hdr.type == VIRTIO_BLK_T_OUT);
                        if (xfer_data(desc, (uint16_t)q->num, id, 16, block.disk, off,
                                      nbytes, to_disk) == 0) {
                            status = VIRTIO_BLK_S_OK;
                            written = (hdr.type == VIRTIO_BLK_T_IN) ? (nbytes + 1) : 1;
                        }
                    }
                } else {
                    status = VIRTIO_BLK_S_UNSUPP;
                }
            }
            write_status(desc, (uint16_t)q->num, id, status);
        }

        uint16_t uslot = used->idx % (uint16_t)q->num;
        used->ring[uslot].id = id;
        used->ring[uslot].len = written;
        dsb_sy();
        used->idx++;
        q->last_avail++;
        block.int_status |= 1u;
        virtio_pend_spi(17);
    }
}

static void queue_notify(struct virtio_mmio_dev *dev, uint32_t sel)
{
    if (dev->device_id == VIRTIO_ID_CONSOLE && sel == VIRTIO_CONSOLE_VQ_TX) {
        process_tx(&dev->q[sel]);
    }
    if (dev->device_id == VIRTIO_ID_BLOCK && sel == 0) {
        process_blk(&dev->q[0]);
    }
}

static struct virtq *selected_queue(struct virtio_mmio_dev *dev)
{
    if (dev->queue_sel >= VIRTQ_MAX_QUEUES) {
        return 0;
    }
    return &dev->q[dev->queue_sel];
}

static uint32_t mmio_read32(struct virtio_mmio_dev *dev, uint64_t off)
{
    struct virtq *q = selected_queue(dev);

    switch (off) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        return VIRTIO_MMIO_MAGIC;
    case VIRTIO_MMIO_VERSION:
        return VIRTIO_MMIO_VERSION_MODERN;
    case VIRTIO_MMIO_DEVICE_ID:
        return dev->device_id;
    case VIRTIO_MMIO_VENDOR_ID:
        return VIRTIO_VENDOR_VEIL;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        if (dev->features_sel == 0) {
            return (uint32_t)dev->device_features;
        }
        if (dev->features_sel == 1) {
            return (uint32_t)(dev->device_features >> 32);
        }
        return 0;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        return q ? VIRTQ_MAX : 0;
    case VIRTIO_MMIO_QUEUE_READY:
        return q ? q->ready : 0;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        return dev->int_status;
    case VIRTIO_MMIO_STATUS:
        return dev->status;
    case VIRTIO_MMIO_CONFIG_GENERATION:
        return 0;
    default:
        if (off >= VIRTIO_MMIO_CONFIG && dev->device_id == VIRTIO_ID_BLOCK) {
            uint64_t cap = dev->capacity;
            uint32_t cfg_off = (uint32_t)(off - VIRTIO_MMIO_CONFIG);
            if (cfg_off < 8) {
                return (uint32_t)(cap >> (cfg_off * 8));
            }
        }
        return 0;
    }
}

static void mmio_write32(struct virtio_mmio_dev *dev, uint64_t off, uint32_t val)
{
    struct virtq *q = selected_queue(dev);

    switch (off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        dev->features_sel = val;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (dev->driver_features_sel == 0) {
            dev->driver_features &= ~0xffffffffull;
            dev->driver_features |= val;
        } else if (dev->driver_features_sel == 1) {
            dev->driver_features &= 0xffffffffull;
            dev->driver_features |= ((uint64_t)val << 32);
        }
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        dev->driver_features_sel = val;
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        dev->queue_sel = val;
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        if (q && val && val <= VIRTQ_MAX) {
            q->num = val;
        }
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        if (q) {
            q->ready = val ? 1 : 0;
            if (!val) {
                q->last_avail = 0;
            }
        }
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        queue_notify(dev, val);
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        dev->int_status &= ~val;
        break;
    case VIRTIO_MMIO_STATUS:
        if (val == 0) {
            if (dev->device_id == VIRTIO_ID_CONSOLE) {
                console_reset();
            } else {
                block_reset();
            }
        } else {
            dev->status = val;
        }
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        if (q) {
            q->desc = (q->desc & ~0xffffffffull) | val;
        }
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        if (q) {
            q->desc = (q->desc & 0xffffffffull) | ((uint64_t)val << 32);
        }
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
        if (q) {
            q->avail = (q->avail & ~0xffffffffull) | val;
        }
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        if (q) {
            q->avail = (q->avail & 0xffffffffull) | ((uint64_t)val << 32);
        }
        break;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
        if (q) {
            q->used = (q->used & ~0xffffffffull) | val;
        }
        break;
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        if (q) {
            q->used = (q->used & 0xffffffffull) | ((uint64_t)val << 32);
        }
        break;
    default:
        break;
    }
}

uint32_t virtio_pending_intid(void)
{
    if (!irq_latch) {
        return 0;
    }
    if (block.int_status) {
        return 49; /* SPI 17 */
    }
    if (console.int_status) {
        return 48; /* SPI 16 */
    }
    irq_latch = 0;
    return 0;
}

void virtio_ack_irq(uint32_t intid)
{
    if (intid == 48 || intid == 49) {
        irq_latch = 0;
    }
}

void virtio_init(void)
{
    console.base = VEIL_VIRTIO_CONSOLE_IPA;
    console_reset();
    for (int i = 0; i < VIRTQ_MAX_QUEUES; i++) {
        virtq_reset(&console.q[i]);
    }

    block.base = VEIL_VIRTIO_BLK_IPA;
    if (!board_skip_testdisk() && veil_disk_start &&
        (uintptr_t)veil_disk_end > (uintptr_t)veil_disk_start) {
        block.disk = (uint8_t *)veil_disk_start;
        block.disk_size = (size_t)((uintptr_t)veil_disk_end - (uintptr_t)veil_disk_start);
        block.capacity = block.disk_size / 512ull;
        hyp_printf("veil: virtio-blk %u KiB at ipa %p\n",
                   (unsigned)(block.disk_size >> 10), (uint64_t)VEIL_VIRTIO_BLK_IPA);
    }
    block_reset();
    for (int i = 0; i < VIRTQ_MAX_QUEUES; i++) {
        virtq_reset(&block.q[i]);
    }
}

static struct virtio_mmio_dev *dev_for_ipa(uint64_t ipa, uint64_t *off)
{
    if (ipa >= VEIL_VIRTIO_CONSOLE_IPA &&
        ipa < VEIL_VIRTIO_CONSOLE_IPA + VEIL_VIRTIO_MMIO_SIZE) {
        *off = ipa - VEIL_VIRTIO_CONSOLE_IPA;
        return &console;
    }
    if (ipa >= VEIL_VIRTIO_BLK_IPA &&
        ipa < VEIL_VIRTIO_BLK_IPA + VEIL_VIRTIO_MMIO_SIZE) {
        *off = ipa - VEIL_VIRTIO_BLK_IPA;
        return &block;
    }
    return 0;
}

int virtio_mmio_access(uint64_t ipa, int is_write, unsigned size, uint64_t *data)
{
    uint64_t off;
    struct virtio_mmio_dev *dev = dev_for_ipa(ipa, &off);
    if (!dev) {
        return -1;
    }
    if (size != 4 && size != 1 && size != 2 && size != 8) {
        return -1;
    }

    unsigned shift = (unsigned)(off & 3ull) * 8u;
    uint64_t aligned = off & ~3ull;

    if (is_write) {
        if (size == 8) {
            mmio_write32(dev, aligned, (uint32_t)*data);
            mmio_write32(dev, aligned + 4, (uint32_t)(*data >> 32));
        } else if (size == 4) {
            mmio_write32(dev, aligned, (uint32_t)*data);
        } else {
            uint32_t cur = mmio_read32(dev, aligned);
            uint32_t mask = (size == 1) ? 0xffu : 0xffffu;
            cur &= ~(mask << shift);
            cur |= ((uint32_t)*data & mask) << shift;
            mmio_write32(dev, aligned, cur);
        }
    } else {
        uint64_t v = mmio_read32(dev, aligned);
        if (size == 8) {
            v |= ((uint64_t)mmio_read32(dev, aligned + 4) << 32);
        } else if (size != 4) {
            uint32_t mask = (size == 1) ? 0xffu : 0xffffu;
            v = (v >> shift) & mask;
        }
        *data = v;
    }
    return 0;
}
