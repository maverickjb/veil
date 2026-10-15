#include "veil.h"
#include "virtio_board.h"

#define FDT_MAGIC     0xd00dfeedu
#define FDT_BEGIN     0x1u
#define FDT_END_NODE  0x2u
#define FDT_PROP      0x3u
#define FDT_END       0x9u

static uint8_t blob[16384];
static uint32_t blob_len;
static uint8_t structs[12288];
static uint8_t strings[2048];
static uint32_t slen;
static uint32_t tlen;

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void s_align(void)
{
    while (slen & 3u) {
        structs[slen++] = 0;
    }
}

static void s_u32(uint32_t v)
{
    put_be32(structs + slen, v);
    slen += 4;
}

static void s_bytes(const void *src, uint32_t n)
{
    memcpy(structs + slen, src, n);
    slen += n;
    s_align();
}

static uint32_t intern(const char *s)
{
    uint32_t i = 0;
    while (i < tlen) {
        const char *p = (const char *)strings + i;
        const char *q = s;
        while (*p && *q && *p == *q) {
            p++;
            q++;
        }
        if (*p == *q) {
            return i;
        }
        while (i < tlen && strings[i]) {
            i++;
        }
        i++;
    }
    uint32_t off = tlen;
    while (*s) {
        strings[tlen++] = (uint8_t)*s++;
    }
    strings[tlen++] = 0;
    return off;
}

static void begin(const char *name)
{
    s_u32(FDT_BEGIN);
    s_bytes(name, (uint32_t)strlen(name) + 1);
}

static void end_node(void)
{
    s_u32(FDT_END_NODE);
}

static void prop(const char *name, const void *val, uint32_t n)
{
    s_u32(FDT_PROP);
    s_u32(n);
    s_u32(intern(name));
    s_bytes(val, n);
}

static void prop_str(const char *name, const char *val)
{
    prop(name, val, (uint32_t)strlen(val) + 1);
}

static void prop_strlist(const char *name, const char *const *vals, unsigned n)
{
    uint8_t buf[160];
    uint32_t len = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t l = (uint32_t)strlen(vals[i]) + 1;
        if (len + l > sizeof(buf)) {
            return;
        }
        memcpy(buf + len, vals[i], l);
        len += l;
    }
    prop(name, buf, len);
}

static void prop_u32(const char *name, uint32_t v)
{
    uint8_t b[4];
    put_be32(b, v);
    prop(name, b, 4);
}

static void prop_cells(const char *name, const uint32_t *c, unsigned n)
{
    s_u32(FDT_PROP);
    s_u32(n * 4u);
    s_u32(intern(name));
    for (unsigned i = 0; i < n; i++) {
        s_u32(c[i]);
    }
}

const uint8_t *dtb_blob(void)
{
    return blob;
}

uint32_t dtb_size(void)
{
    return blob_len;
}

void dtb_init(void)
{
    const struct platform *p = platform_get();
    slen = 0;
    tlen = 0;
    memset(structs, 0, sizeof(structs));
    memset(strings, 0, sizeof(strings));
    memset(blob, 0, sizeof(blob));

    begin("");
    prop_str("compatible", "linux,dummy-virt");
    prop_u32("#address-cells", 2);
    prop_u32("#size-cells", 2);
    prop_u32("interrupt-parent", 1);

    begin("chosen");
    prop_str("stdout-path", "/pl011@9000000");
    end_node();

    begin("memory@40000000");
    prop_str("device_type", "memory");
    {
        uint32_t reg[] = {0, 0x40000000u, (uint32_t)(p->guest_ram_size >> 32),
                          (uint32_t)p->guest_ram_size};
        prop_cells("reg", reg, 4);
    }
    end_node();

    begin("cpus");
    prop_u32("#address-cells", 1);
    prop_u32("#size-cells", 0);
    begin("cpu@0");
    prop_str("device_type", "cpu");
    prop_str("compatible", "arm,cortex-a53");
    prop_u32("reg", 0);
    prop_str("enable-method", "psci");
    end_node();
    end_node();

    begin("psci");
    {
        const char *c[] = {"arm,psci-1.0", "arm,psci-0.2", "arm,psci"};
        prop_strlist("compatible", c, 3);
    }
    prop_str("method", "hvc");
    end_node();

    begin("timer");
    {
        const char *c[] = {"arm,armv8-timer", "arm,armv7-timer"};
        prop_strlist("compatible", c, 2);
    }
    {
        uint32_t irqs[] = {1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4};
        prop_cells("interrupts", irqs, 12);
    }
    prop("always-on", "", 0);
    end_node();

    begin("intc@8000000");
    prop_str("compatible", "arm,gic-v3");
    prop_u32("phandle", 1);
    prop_u32("#interrupt-cells", 3);
    prop("interrupt-controller", "", 0);
    prop_u32("#address-cells", 2);
    prop_u32("#size-cells", 2);
    prop("ranges", "", 0);
    prop_u32("#redistributor-regions", 1);
    {
        uint32_t reg[] = {0, 0x08000000u, 0, 0x10000u, 0, 0x080a0000u,
                          0, (uint32_t)VEIL_GICR_DTB_SIZE};
        prop_cells("reg", reg, 8);
    }
#if BOARD_GIC_PASSTHROUGH
    begin("its@8080000");
    prop_str("compatible", "arm,gic-v3-its");
    prop("msi-controller", "", 0);
    prop_u32("#msi-cells", 1);
    prop_u32("phandle", 3);
    {
        uint32_t reg[] = {0, 0x08080000u, 0, 0x20000u};
        prop_cells("reg", reg, 4);
    }
    end_node();
#endif
    end_node();

    begin("apb-pclk");
    prop_str("compatible", "fixed-clock");
    prop_u32("phandle", 2);
    prop_u32("#clock-cells", 0);
    prop_u32("clock-frequency", 24000000);
    end_node();

    begin("pl031@9010000");
    {
        const char *c[] = {"arm,pl031", "arm,primecell"};
        prop_strlist("compatible", c, 2);
    }
    {
        uint32_t reg[] = {0, 0x09010000u, 0, 0x1000u};
        prop_cells("reg", reg, 4);
    }
    {
        uint32_t irqs[] = {0, 2, 4};
        prop_cells("interrupts", irqs, 3);
    }
    {
        uint32_t clocks[] = {2};
        prop_cells("clocks", clocks, 1);
    }
    {
        const char *n[] = {"apb_pclk"};
        prop_strlist("clock-names", n, 1);
    }
    end_node();

    begin("pl011@9000000");
    {
        const char *c[] = {"arm,pl011", "arm,primecell"};
        prop_strlist("compatible", c, 2);
    }
    {
        uint32_t reg[] = {0, 0x09000000u, 0, 0x1000u};
        prop_cells("reg", reg, 4);
    }
    {
        uint32_t irqs[] = {0, 1, 4};
        prop_cells("interrupts", irqs, 3);
    }
    {
        uint32_t clocks[] = {2, 2};
        prop_cells("clocks", clocks, 2);
    }
    {
        const char *n[] = {"uartclk", "apb_pclk"};
        prop_strlist("clock-names", n, 2);
    }
    end_node();

    begin("flash@0");
    prop_str("compatible", "cfi-flash");
    {
        uint32_t reg[] = {0, 0x0, 0, 0x200000u, 0, 0x04000000u, 0, 0x200000u};
        prop_cells("reg", reg, 8);
    }
    prop_u32("bank-width", 4);
    end_node();

    begin("fw-cfg@9020000");
    prop_str("compatible", "qemu,fw-cfg-mmio");
    {
        uint32_t reg[] = {0, 0x09020000u, 0, 0x18u};
        prop_cells("reg", reg, 4);
    }
    prop("dma-coherent", "", 0);
    end_node();

    begin("virtio_mmio@a000000");
    prop_str("compatible", "virtio,mmio");
    {
        uint32_t reg[] = {0, 0x0a000000u, 0, 0x200u};
        prop_cells("reg", reg, 4);
    }
    {
        uint32_t irqs[] = {0, 16, 1};
        prop_cells("interrupts", irqs, 3);
    }
    prop("dma-coherent", "", 0);
    end_node();

    begin("virtio_mmio@a000200");
    prop_str("compatible", "virtio,mmio");
    {
        uint32_t reg[] = {0, 0x0a000200u, 0, 0x200u};
        prop_cells("reg", reg, 4);
    }
    {
        uint32_t irqs[] = {0, 17, 1};
        prop_cells("interrupts", irqs, 3);
    }
    prop("dma-coherent", "", 0);
    end_node();

#if BOARD_GIC_PASSTHROUGH
    begin("pcie@10000000");
    prop_str("compatible", "pci-host-ecam-generic");
    prop_str("device_type", "pci");
    prop_u32("#address-cells", 3);
    prop_u32("#size-cells", 2);
    prop_u32("#interrupt-cells", 1);
    prop_u32("linux,pci-domain", 0);
    {
        uint32_t bus[] = {0, 0xff};
        prop_cells("bus-range", bus, 2);
    }
    prop("dma-coherent", "", 0);
    {
        uint32_t msi[] = {0, 3, 0, 0x10000u};
        prop_cells("msi-map", msi, 4);
    }
    {
        uint32_t reg[] = {0x40, 0x10000000u, 0, 0x10000000u};
        prop_cells("reg", reg, 4);
    }
    {
        uint32_t ranges[] = {
            0x01000000u, 0, 0, 0, 0x3eff0000u, 0, 0x10000u,
            0x02000000u, 0, 0x10000000u, 0, 0x10000000u, 0, 0x2eff0000u,
            0x03000000u, 0x80, 0, 0x80, 0, 0x80, 0,
        };
        prop_cells("ranges", ranges, 21);
    }
    {
        uint32_t mask[] = {0x1800u, 0, 0, 7};
        prop_cells("interrupt-map-mask", mask, 4);
    }
    {
        uint32_t imap[160];
        unsigned k = 0;
        for (unsigned slot = 0; slot < 4; slot++) {
            for (unsigned pin = 1; pin <= 4; pin++) {
                unsigned spi = 3 + ((slot + pin - 1) % 4);
                imap[k++] = slot << 11;
                imap[k++] = 0;
                imap[k++] = 0;
                imap[k++] = pin;
                imap[k++] = 1;
                imap[k++] = 0;
                imap[k++] = 0;
                imap[k++] = 0;
                imap[k++] = spi;
                imap[k++] = 4;
            }
        }
        prop_cells("interrupt-map", imap, 160);
    }
    end_node();
#endif

    end_node();
    s_u32(FDT_END);

    uint32_t off_rsv = 40;
    uint32_t off_struct = 56; /* header 40 + 16-byte empty rsvmap */
    uint32_t off_str = off_struct + slen;
    blob_len = off_str + tlen;
    while (blob_len & 7u) {
        blob_len++;
    }

    put_be32(blob + 0, FDT_MAGIC);
    put_be32(blob + 4, blob_len);
    put_be32(blob + 8, off_struct);
    put_be32(blob + 12, off_str);
    put_be32(blob + 16, off_rsv);
    put_be32(blob + 20, 17);
    put_be32(blob + 24, 16);
    put_be32(blob + 28, 0);
    put_be32(blob + 32, tlen);
    put_be32(blob + 36, slen);
    /* rsvmap terminator */
    memset(blob + off_rsv, 0, 16);
    memcpy(blob + off_struct, structs, slen);
    memcpy(blob + off_str, strings, tlen);
}
