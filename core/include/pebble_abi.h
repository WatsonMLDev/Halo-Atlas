#ifndef PEBBLE_ABI_H
#define PEBBLE_ABI_H

#include <stddef.h>
#include <stdint.h>

/*
 * ARM ABI compatibility contract:
 *   - Host runtime (desktop ARM runner and Halo Cortex-M55 Zephyr firmware) compiles
 *     with hard-float ABI (-mfloat-abi=hard, passing floats in s0-s15 / d0-d7).
 *   - Unmodified Pebble .pbw binaries are Cortex-M3 soft-float (-mfloat-abi=soft,
 *     passing floats in r0-r3) with -fshort-enums.
 *
 * Every syscall shim taking or returning float/double MUST carry PEBBLE_SOFTFP_ABI.
 * Every Pebble ABI struct containing enums MUST use explicit fixed-width integer fields
 * (uint8_t / int8_t) to match Pebble SDK's -fshort-enums packed layout.
 */
#if defined(__arm__) && (defined(__GNUC__) || defined(__clang__))
#define PEBBLE_SOFTFP_ABI __attribute__((pcs("aapcs")))
#else
#define PEBBLE_SOFTFP_ABI
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PEBBLE_PACKED __attribute__((packed))
#else
#define PEBBLE_PACKED
#endif

#define PEBBLE_PROCESS_INFO_MAGIC_LEN 8u
#define PEBBLE_PROCESS_NAME_BYTES 32u
#define PEBBLE_COMPANY_NAME_BYTES 32u

#define PEBBLE_STRUCT_VERSION_LEGACY_V8 0x08u
#define PEBBLE_STRUCT_VERSION_LEGACY_V9 0x09u
#define PEBBLE_STRUCT_VERSION_CURRENT_V16 0x10u

typedef struct PEBBLE_PACKED {
    uint8_t major;
    uint8_t minor;
} pebble_version_t;

typedef struct PEBBLE_PACKED {
    uint8_t bytes[16];
} pebble_uuid_t;

/* Matches PebbleProcessInfo (struct version 0x10 / 16, 0x82 = 130 bytes). */
typedef struct PEBBLE_PACKED {
    char header[PEBBLE_PROCESS_INFO_MAGIC_LEN];
    pebble_version_t struct_version;
    pebble_version_t sdk_version;
    pebble_version_t process_version;
    uint16_t load_size;
    uint32_t offset;
    uint32_t crc;
    char name[PEBBLE_PROCESS_NAME_BYTES];
    char company[PEBBLE_COMPANY_NAME_BYTES];
    uint32_t icon_resource_id;
    uint32_t sym_table_addr;
    uint32_t flags;
    uint32_t num_reloc_entries;
    pebble_uuid_t uuid;
    uint32_t resource_crc;
    uint32_t resource_timestamp;
    uint16_t virtual_size;
} pebble_process_info_v16_t;

/* Matches PebbleProcessInfo struct version 0x09 (no reloc_list_start, no virtual_size). */
typedef struct PEBBLE_PACKED {
    char header[PEBBLE_PROCESS_INFO_MAGIC_LEN];
    pebble_version_t struct_version;
    pebble_version_t sdk_version;
    pebble_version_t process_version;
    uint16_t load_size;
    uint32_t offset;
    uint32_t crc;
    char name[PEBBLE_PROCESS_NAME_BYTES];
    char company[PEBBLE_COMPANY_NAME_BYTES];
    uint32_t icon_resource_id;
    uint32_t sym_table_addr;
    uint32_t flags;
    uint32_t num_reloc_entries;
    pebble_uuid_t uuid;
    uint32_t resource_crc;
    uint32_t resource_timestamp;
} pebble_process_info_v9_t;

/* Matches LegacyPebbleAppInfo (struct version 0x08: has reloc_list_start). */
typedef struct PEBBLE_PACKED {
    char header[PEBBLE_PROCESS_INFO_MAGIC_LEN];
    pebble_version_t struct_version;
    pebble_version_t sdk_version;
    pebble_version_t process_version;
    uint16_t load_size;
    uint32_t offset;
    uint32_t crc;
    char name[PEBBLE_PROCESS_NAME_BYTES];
    char company[PEBBLE_COMPANY_NAME_BYTES];
    uint32_t icon_resource_id;
    uint32_t sym_table_addr;
    uint32_t flags;
    uint32_t reloc_list_start;
    uint32_t num_reloc_entries;
    pebble_uuid_t uuid;
} pebble_process_info_v8_t;

/*
 * Packed Pebble SDK Dictionary / Tuplet ABI structs (-fshort-enums: TupleType is 1 byte).
 * Matches <pebble.h> Tuple and Tuplet wire/memory layout in Cortex-M3 .pbw binaries.
 */
enum {
    PEBBLE_TUPLE_BYTE_ARRAY = 0,
    PEBBLE_TUPLE_CSTRING = 1,
    PEBBLE_TUPLE_UINT = 2,
    PEBBLE_TUPLE_INT = 3,
};

typedef struct PEBBLE_PACKED {
    uint32_t key;
    uint8_t type; /* TupleType (-fshort-enums => 1 byte) */
    uint16_t length;
    union PEBBLE_PACKED {
        uint8_t data[1];
        char cstring[1];
        uint8_t uint8;
        uint16_t uint16;
        uint32_t uint32;
        int8_t int8;
        int16_t int16;
        int32_t int32;
    } value;
} pebble_tuple_t;

typedef struct {
    uint8_t type; /* TupleType (-fshort-enums => 1 byte, 3-byte pad before key) */
    uint32_t key;
    union {
        struct {
            const uint8_t *data;
            uint16_t length;
        } bytes;
        struct {
            const char *data;
            uint16_t length;
        } cstring;
        struct {
            uint32_t storage;
            uint16_t width;
        } integer;
    };
} pebble_tuplet_t;

/* Matches <pebble.h> CompassHeadingData under -fshort-enums (CompassStatus = 1 byte). */
typedef struct {
    int32_t magnetic_heading;
    int32_t true_heading;
    uint8_t compass_status; /* CompassStatus (-fshort-enums => 1 byte) */
    uint8_t is_declination_valid;
} pebble_compass_heading_data_t;

_Static_assert(sizeof(pebble_process_info_v16_t) == 0x82u, "v16 header must be 0x82 bytes");
_Static_assert(offsetof(pebble_process_info_v16_t, load_size) == 0x0eu, "load_size offset");
_Static_assert(offsetof(pebble_process_info_v16_t, offset) == 0x10u, "offset field");
_Static_assert(offsetof(pebble_process_info_v16_t, sym_table_addr) == 0x5cu, "sym_table_addr offset");
_Static_assert(offsetof(pebble_process_info_v16_t, num_reloc_entries) == 0x64u, "num_reloc_entries offset");
_Static_assert(offsetof(pebble_process_info_v16_t, virtual_size) == 0x80u, "virtual_size offset");
_Static_assert(offsetof(pebble_process_info_v8_t, reloc_list_start) == 0x64u, "v8 reloc_list_start offset");
_Static_assert(offsetof(pebble_process_info_v8_t, num_reloc_entries) == 0x68u, "v8 num_reloc_entries offset");
_Static_assert(offsetof(pebble_tuple_t, type) == 4u, "Tuple.type offset");
_Static_assert(offsetof(pebble_tuple_t, length) == 5u, "Tuple.length offset");
_Static_assert(offsetof(pebble_tuple_t, value) == 7u, "Tuple.value offset");
_Static_assert(offsetof(pebble_tuplet_t, key) == 4u, "Tuplet.key offset");
_Static_assert(offsetof(pebble_compass_heading_data_t, is_declination_valid) == 9u,
               "CompassHeadingData short-enum offset");
_Static_assert(sizeof(pebble_compass_heading_data_t) == 12u,
               "CompassHeadingData short-enum size");

#endif /* PEBBLE_ABI_H */
