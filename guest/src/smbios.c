#include "veil.h"
#include "virtio_board.h"

/*
 * AAVMF FrontPage "System Memory" is the sum of SMBIOS Type 19 ranges,
 * not the FDT /memory node. QEMU ships these tables on fw_cfg; this file
 * builds the same two payloads (tables + 3.0 anchor).
 */

static uint8_t tables[512];
static uint32_t tables_len;
static uint8_t anchor[24];

static void smbios_put(uint8_t v)
{
    if (tables_len < sizeof(tables)) {
        tables[tables_len++] = v;
    }
}

static void smbios_put16(uint16_t v)
{
    smbios_put((uint8_t)v);
    smbios_put((uint8_t)(v >> 8));
}

static void smbios_put32(uint32_t v)
{
    smbios_put16((uint16_t)v);
    smbios_put16((uint16_t)(v >> 16));
}

static void smbios_put64(uint64_t v)
{
    smbios_put32((uint32_t)v);
    smbios_put32((uint32_t)(v >> 32));
}

static void smbios_str(const char *s)
{
    while (*s) {
        smbios_put((uint8_t)*s++);
    }
    smbios_put(0);
}

const uint8_t *smbios_tables_blob(void)
{
    return tables;
}

uint32_t smbios_tables_size(void)
{
    return tables_len;
}

const uint8_t *smbios_anchor_blob(void)
{
    return anchor;
}

uint32_t smbios_anchor_size(void)
{
    return sizeof(anchor);
}

void smbios_init(void)
{
    const struct platform *p = platform_get();
    uint64_t ram_ipa = VEIL_GUEST_RAM_IPA;
    uint64_t ram_bytes = p->guest_ram_size;
    uint32_t ram_kb = (uint32_t)(ram_bytes >> 10);
    uint16_t ram_mb = (uint16_t)(ram_bytes >> 20);
    uint32_t start_kb = (uint32_t)(ram_ipa >> 10);
    uint32_t end_kb = start_kb + ram_kb - 1u;
    unsigned i;
    uint8_t sum = 0;

    tables_len = 0;

    /* Type 0 BIOS Information */
    smbios_put(0);
    smbios_put(0x18);
    smbios_put16(0x0000);
    smbios_put(1);
    smbios_put(2);
    smbios_put16(0xe800);
    smbios_put(3);
    smbios_put(0);
    smbios_put64(0x08);
    smbios_put(0);
    smbios_put(0x14);
    smbios_put(0);
    smbios_put(1);
    smbios_put(0xff);
    smbios_put(0xff);
    smbios_str("Veil");
    smbios_str("0.1");
    smbios_str("10/15/2026");
    smbios_put(0);

    /* Type 1 System Information */
    smbios_put(1);
    smbios_put(0x1b);
    smbios_put16(0x0001);
    smbios_put(1);
    smbios_put(2);
    smbios_put(3);
    smbios_put(4);
    for (i = 0; i < 16; i++) {
        smbios_put(0);
    }
    smbios_put(6); /* power-up */
    smbios_put(5);
    smbios_put(6);
    smbios_str("Veil");
    smbios_str("virt");
    smbios_str("0.1");
    smbios_str("0");
    smbios_str("0");
    smbios_str("virt");
    smbios_put(0);

    /* Type 16 Physical Memory Array */
    smbios_put(16);
    smbios_put(0x17);
    smbios_put16(0x1000);
    smbios_put(0x01); /* other */
    smbios_put(0x03); /* system memory */
    smbios_put(0x06); /* multi-bit ECC */
    smbios_put32(ram_kb);
    smbios_put16(0xfffe);
    smbios_put16(1);
    smbios_put64(0);
    smbios_put(0);
    smbios_put(0);

    /* Type 17 Memory Device (SMBIOS 2.8, length 0x28) */
    smbios_put(17);
    smbios_put(0x28);
    smbios_put16(0x1001);
    smbios_put16(0x1000);
    smbios_put16(0xfffe);
    smbios_put16(0xffff);
    smbios_put16(0xffff);
    smbios_put16(ram_mb);
    smbios_put(0x09); /* DIMM */
    smbios_put(0);
    smbios_put(1);
    smbios_put(0);
    smbios_put(0x07); /* RAM */
    smbios_put16(0);
    smbios_put16(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put32(0); /* Extended Size */
    smbios_put16(0); /* Configured Memory Clock Speed */
    smbios_put16(0);
    smbios_put16(0);
    smbios_put16(0);
    smbios_str("DIMM 0");
    smbios_put(0);

    /* Type 19 Memory Array Mapped Address — FrontPage sums this */
    smbios_put(19);
    smbios_put(0x1f);
    smbios_put16(0x1002);
    smbios_put32(start_kb);
    smbios_put32(end_kb);
    smbios_put16(0x1000);
    smbios_put(1);
    smbios_put64(0);
    smbios_put64(0);
    smbios_put(0);
    smbios_put(0);

    /* Type 32 System Boot Information */
    smbios_put(32);
    smbios_put(0x0b);
    smbios_put16(0x0020);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);

    /* Type 127 End-of-table */
    smbios_put(127);
    smbios_put(0x04);
    smbios_put16(0x007f);
    smbios_put(0);
    smbios_put(0);

    memset(anchor, 0, sizeof(anchor));
    memcpy(anchor, "_SM3_", 5);
    anchor[6] = 0x18; /* entry point length */
    anchor[7] = 3;    /* major */
    anchor[8] = 0;    /* minor */
    anchor[10] = 1;   /* entry point revision */
    anchor[12] = (uint8_t)tables_len;
    anchor[13] = (uint8_t)(tables_len >> 8);
    anchor[14] = (uint8_t)(tables_len >> 16);
    anchor[15] = (uint8_t)(tables_len >> 24);
    for (i = 0; i < sizeof(anchor); i++) {
        sum += anchor[i];
    }
    anchor[5] = (uint8_t)(0u - sum);

    hyp_printf("veil: smbios tables %u bytes ram %u MB\n",
               tables_len, (unsigned)ram_mb);
}
