#include "veil.h"
#include "virtio_board.h"

/*
 * SMBIOS payloads for AAVMF (tables + 3.0 entry-point on fw_cfg).
 *
 *   Type 0   BIOS Information
 *   Type 1   System Information
 *   Type 4   Processor Information
 *   Type 16  Physical Memory Array
 *   Type 17  Memory Device
 *   Type 19  Memory Array Mapped Address  (FrontPage "System Memory")
 *   Type 32  System Boot Information
 *   Type 127 End-of-Table
 */

#define SMBIOS_TABLE_CAPACITY 512

#define SMBIOS_TYPE_BIOS_INFO     0
#define SMBIOS_TYPE_SYSTEM_INFO   1
#define SMBIOS_TYPE_PROCESSOR     4
#define SMBIOS_TYPE_MEMORY_ARRAY  16
#define SMBIOS_TYPE_MEMORY_DEVICE 17
#define SMBIOS_TYPE_MEMORY_MAP    19
#define SMBIOS_TYPE_BOOT_INFO     32
#define SMBIOS_TYPE_END           127

#define SMBIOS_BIOS_INFO_LENGTH      0x18
#define SMBIOS_SYSTEM_INFO_LENGTH    0x1B
#define SMBIOS_PROCESSOR_LENGTH      0x30
#define SMBIOS_MEMORY_ARRAY_LENGTH   0x17
#define SMBIOS_MEMORY_DEVICE_LENGTH  0x28
#define SMBIOS_MEMORY_MAP_LENGTH     0x1F
#define SMBIOS_BOOT_INFO_LENGTH      0x0B
#define SMBIOS_END_LENGTH            0x04

#define SMBIOS_HANDLE_BIOS_INFO     0x0000
#define SMBIOS_HANDLE_SYSTEM_INFO   0x0001
#define SMBIOS_HANDLE_PROCESSOR     0x0004
#define SMBIOS_HANDLE_MEMORY_ARRAY  0x1000
#define SMBIOS_HANDLE_MEMORY_DEVICE 0x1001
#define SMBIOS_HANDLE_MEMORY_MAP    0x1002
#define SMBIOS_HANDLE_BOOT_INFO     0x0020
#define SMBIOS_HANDLE_END           0x007F

#define SMBIOS_INDEX_VENDOR       1
#define SMBIOS_INDEX_VERSION      2
#define SMBIOS_INDEX_RELEASE_DATE 3

#define SMBIOS_BIOS_START_SEGMENT   0xE800
#define SMBIOS_BIOS_ROM_SIZE        0
#define SMBIOS_BIOS_CHARACTERISTICS 0x08
#define SMBIOS_BIOS_EXT_FEATURES_1  0x00
#define SMBIOS_BIOS_EXT_FEATURES_2  0x14

#define SMBIOS_SYSTEM_WAKEUP_POWER_UP 6

#define SMBIOS_CPU_SOCKET_POPULATED 0x41
#define SMBIOS_CPU_UPGRADE_NONE     0x06
#define SMBIOS_CPU_FAMILY_ARMV8     0x011B

#define SMBIOS_MEMORY_USE_SYSTEM    0x03
#define SMBIOS_MEMORY_ERROR_ECC     0x06
#define SMBIOS_MEMORY_DEVICE_DIMM   0x09
#define SMBIOS_MEMORY_TYPE_RAM      0x07

#define SMBIOS_MEMORY_SIZE_UNKNOWN   0xFFFF
#define SMBIOS_MEMORY_ARRAY_EXTENDED 0xFFFE

#define SMBIOS3_ENTRY_POINT_LENGTH   0x18
#define SMBIOS3_MAJOR_VERSION        3
#define SMBIOS3_MINOR_VERSION        0
#define SMBIOS3_ENTRY_POINT_REVISION 1

static uint8_t tables[SMBIOS_TABLE_CAPACITY];
static uint32_t tables_len;
static uint8_t anchor[24];

static void smbios_put(uint8_t value)
{
    if (tables_len < sizeof(tables)) {
        tables[tables_len++] = value;
    }
}

static void smbios_put16(uint16_t value)
{
    smbios_put((uint8_t)value);
    smbios_put((uint8_t)(value >> 8));
}

static void smbios_put32(uint32_t value)
{
    smbios_put16((uint16_t)value);
    smbios_put16((uint16_t)(value >> 16));
}

static void smbios_put64(uint64_t value)
{
    smbios_put32((uint32_t)value);
    smbios_put32((uint32_t)(value >> 32));
}

static void smbios_put_string(const char *value)
{
    while (*value != '\0') {
        smbios_put((uint8_t)*value++);
    }
    smbios_put(0);
}

static void smbios_put_header(uint8_t type, uint8_t length, uint16_t handle)
{
    smbios_put(type);
    smbios_put(length);
    smbios_put16(handle);
}

/* Extra NUL after the last string of a structure. */
static void smbios_end_string_set(void)
{
    smbios_put(0);
}

/* A structure with no strings is still followed by two NULs. */
static void smbios_no_strings(void)
{
    smbios_put(0);
    smbios_put(0);
}

static void smbios_build_type0(const char *vendor, const char *version,
                               const char *release_date)
{
    smbios_put_header(SMBIOS_TYPE_BIOS_INFO, SMBIOS_BIOS_INFO_LENGTH,
                      SMBIOS_HANDLE_BIOS_INFO);
    smbios_put(SMBIOS_INDEX_VENDOR);
    smbios_put(SMBIOS_INDEX_VERSION);
    smbios_put16(SMBIOS_BIOS_START_SEGMENT);
    smbios_put(SMBIOS_INDEX_RELEASE_DATE);
    smbios_put(SMBIOS_BIOS_ROM_SIZE);
    smbios_put64(SMBIOS_BIOS_CHARACTERISTICS);
    smbios_put(SMBIOS_BIOS_EXT_FEATURES_1);
    smbios_put(SMBIOS_BIOS_EXT_FEATURES_2);
    smbios_put(0); /* BIOS major */
    smbios_put(1); /* BIOS minor */
    smbios_put(0xFF);
    smbios_put(0xFF);
    smbios_put_string(vendor);
    smbios_put_string(version);
    smbios_put_string(release_date);
    smbios_end_string_set();
}

static void smbios_build_type1(const char *manufacturer, const char *product_name,
                               const char *version, const char *serial_number,
                               const char *sku_number, const char *family)
{
    unsigned i;

    smbios_put_header(SMBIOS_TYPE_SYSTEM_INFO, SMBIOS_SYSTEM_INFO_LENGTH,
                      SMBIOS_HANDLE_SYSTEM_INFO);
    smbios_put(1);
    smbios_put(2);
    smbios_put(3);
    smbios_put(4);
    for (i = 0; i < 16; i++) {
        smbios_put(0);
    }
    smbios_put(SMBIOS_SYSTEM_WAKEUP_POWER_UP);
    smbios_put(5);
    smbios_put(6);
    smbios_put_string(manufacturer);
    smbios_put_string(product_name);
    smbios_put_string(version);
    smbios_put_string(serial_number);
    smbios_put_string(sku_number);
    smbios_put_string(family);
    smbios_end_string_set();
}

static void smbios_build_type4(const char *socket_designation,
                               const char *manufacturer,
                               const char *processor_version)
{
    unsigned i;

    smbios_put_header(SMBIOS_TYPE_PROCESSOR, SMBIOS_PROCESSOR_LENGTH,
                      SMBIOS_HANDLE_PROCESSOR);
    smbios_put(1);
    smbios_put(0x03); /* Central Processor */
    smbios_put(0xFE); /* Family 2 */
    smbios_put(2);
    for (i = 0; i < 8; i++) {
        smbios_put(0);
    }
    smbios_put(3);
    smbios_put(0);
    smbios_put16(0);
    smbios_put16(2000);
    smbios_put16(2000);
    smbios_put(SMBIOS_CPU_SOCKET_POPULATED);
    smbios_put(SMBIOS_CPU_UPGRADE_NONE);
    smbios_put16(SMBIOS_MEMORY_SIZE_UNKNOWN);
    smbios_put16(SMBIOS_MEMORY_SIZE_UNKNOWN);
    smbios_put16(SMBIOS_MEMORY_SIZE_UNKNOWN);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(1);
    smbios_put(1);
    smbios_put(1);
    smbios_put16(0);
    smbios_put16(SMBIOS_CPU_FAMILY_ARMV8);
    smbios_put16(1);
    smbios_put16(1);
    smbios_put16(1);
    smbios_put_string(socket_designation);
    smbios_put_string(manufacturer);
    smbios_put_string(processor_version);
    smbios_end_string_set();
}

static void smbios_build_type16(uint64_t ram_bytes)
{
    uint32_t ram_kb = (uint32_t)(ram_bytes >> 10);

    smbios_put_header(SMBIOS_TYPE_MEMORY_ARRAY, SMBIOS_MEMORY_ARRAY_LENGTH,
                      SMBIOS_HANDLE_MEMORY_ARRAY);
    smbios_put(0x01); /* Other */
    smbios_put(SMBIOS_MEMORY_USE_SYSTEM);
    smbios_put(SMBIOS_MEMORY_ERROR_ECC);
    smbios_put32(ram_kb);
    smbios_put16(SMBIOS_MEMORY_ARRAY_EXTENDED);
    smbios_put16(1);
    smbios_put64(0);
    smbios_no_strings();
}

static void smbios_build_type17(uint64_t ram_bytes)
{
    uint16_t ram_mb = (uint16_t)(ram_bytes >> 20);

    smbios_put_header(SMBIOS_TYPE_MEMORY_DEVICE, SMBIOS_MEMORY_DEVICE_LENGTH,
                      SMBIOS_HANDLE_MEMORY_DEVICE);
    smbios_put16(SMBIOS_HANDLE_MEMORY_ARRAY);
    smbios_put16(SMBIOS_MEMORY_ARRAY_EXTENDED);
    smbios_put16(SMBIOS_MEMORY_SIZE_UNKNOWN);
    smbios_put16(SMBIOS_MEMORY_SIZE_UNKNOWN);
    smbios_put16(ram_mb);
    smbios_put(SMBIOS_MEMORY_DEVICE_DIMM);
    smbios_put(0);
    smbios_put(1);
    smbios_put(0);
    smbios_put(SMBIOS_MEMORY_TYPE_RAM);
    smbios_put16(0);
    smbios_put16(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put32(0);
    smbios_put16(0);
    smbios_put16(0);
    smbios_put16(0);
    smbios_put16(0);
    smbios_put_string("DIMM 0");
    smbios_end_string_set();
}

static void smbios_build_type19(uint64_t ram_ipa, uint64_t ram_bytes)
{
    uint32_t start_kb = (uint32_t)(ram_ipa >> 10);
    uint32_t ram_kb = (uint32_t)(ram_bytes >> 10);
    uint32_t end_kb = start_kb + ram_kb - 1u;

    smbios_put_header(SMBIOS_TYPE_MEMORY_MAP, SMBIOS_MEMORY_MAP_LENGTH,
                      SMBIOS_HANDLE_MEMORY_MAP);
    /* Inclusive KiB range in the legacy 32-bit fields. */
    smbios_put32(start_kb);
    smbios_put32(end_kb);
    smbios_put16(SMBIOS_HANDLE_MEMORY_ARRAY);
    smbios_put(1);
    smbios_put64(0);
    smbios_put64(0);
    smbios_no_strings();
}

static void smbios_build_type32(void)
{
    smbios_put_header(SMBIOS_TYPE_BOOT_INFO, SMBIOS_BOOT_INFO_LENGTH,
                      SMBIOS_HANDLE_BOOT_INFO);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_put(0);
    smbios_no_strings();
}

static void smbios_build_type127(void)
{
    smbios_put_header(SMBIOS_TYPE_END, SMBIOS_END_LENGTH, SMBIOS_HANDLE_END);
    smbios_no_strings();
}

static void smbios_build_entry_point(void)
{
    unsigned i;
    uint8_t checksum = 0;

    memset(anchor, 0, sizeof(anchor));
    memcpy(anchor, "_SM3_", 5);
    anchor[6] = SMBIOS3_ENTRY_POINT_LENGTH;
    anchor[7] = SMBIOS3_MAJOR_VERSION;
    anchor[8] = SMBIOS3_MINOR_VERSION;
    anchor[10] = SMBIOS3_ENTRY_POINT_REVISION;
    anchor[12] = (uint8_t)tables_len;
    anchor[13] = (uint8_t)(tables_len >> 8);
    anchor[14] = (uint8_t)(tables_len >> 16);
    anchor[15] = (uint8_t)(tables_len >> 24);
    for (i = 0; i < sizeof(anchor); i++) {
        checksum = (uint8_t)(checksum + anchor[i]);
    }
    anchor[5] = (uint8_t)(0u - checksum);
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
    const struct platform *platform = platform_get();
    uint64_t guest_ram_bytes = platform->guest_ram_size;

    tables_len = 0;
    smbios_build_type0("Veil", "0.1", "10/15/2026");
    smbios_build_type1("Veil", "virt", "0.1", "0", "0", "virt");
    smbios_build_type4("CPU0", "Veil", "ARMv8 Processor");
    smbios_build_type16(guest_ram_bytes);
    smbios_build_type17(guest_ram_bytes);
    smbios_build_type19(VEIL_GUEST_RAM_IPA, guest_ram_bytes);
    smbios_build_type32();
    smbios_build_type127();
    smbios_build_entry_point();

    hyp_printf("veil: smbios tables %u bytes ram %u MB\n",
               tables_len, (unsigned)(guest_ram_bytes >> 20));
}
