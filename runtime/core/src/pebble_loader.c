#include "pebble_compat.h"

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void copy_fixed_string(char *dst, size_t dst_cap, const uint8_t *src, size_t src_len) {
    size_t n = src_len < (dst_cap - 1u) ? src_len : (dst_cap - 1u);
    for (size_t i = 0; i < n; ++i) {
        dst[i] = (char)src[i];
        if (src[i] == 0u) {
            return;
        }
    }
    dst[n] = '\0';
}

const char *pebble_status_str(pebble_status_t status) {
    switch (status) {
        case PEBBLE_OK: return "ok";
        case PEBBLE_ERR_INVALID_ARG: return "invalid argument";
        case PEBBLE_ERR_TRUNCATED_HEADER: return "truncated PebbleProcessInfo header";
        case PEBBLE_ERR_BAD_MAGIC: return "invalid PBLAPP header magic";
        case PEBBLE_ERR_UNSUPPORTED_VERSION: return "unsupported PebbleProcessInfo struct version";
        case PEBBLE_ERR_TRUNCATED_IMAGE: return "truncated binary or relocation table";
        case PEBBLE_ERR_OVERSIZED_VIRTUAL_SIZE: return "virtual_size exceeds allowed RAM budget";
        case PEBBLE_ERR_INVALID_VIRTUAL_SIZE: return "virtual_size is smaller than load_size";
        case PEBBLE_ERR_INVALID_ENTRY_POINT: return "entry_point lies outside loaded image";
        case PEBBLE_ERR_INVALID_SYM_TABLE_ADDR: return "sym_table_addr invalid or out of bounds";
        case PEBBLE_ERR_INVALID_RELOC: return "relocation entry lies outside loaded image";
        case PEBBLE_ERR_ALLOC_FAILED: return "executable memory allocation failed";
        default: return "unknown error";
    }
}

pebble_status_t pebble_loader_parse(
    const uint8_t *bin,
    size_t bin_len,
    uint32_t max_virtual_size,
    pebble_image_info_t *info_out) {
    if (bin == NULL || info_out == NULL) {
        return PEBBLE_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(*info_out); ++i) {
        ((uint8_t *)info_out)[i] = 0u;
    }

    if (bin_len < 16u) {
        return PEBBLE_ERR_TRUNCATED_HEADER;
    }

    static const char kMagic[8] = {'P', 'B', 'L', 'A', 'P', 'P', '\0', '\0'};
    for (size_t i = 0; i < 8u; ++i) {
        if ((char)bin[i] != kMagic[i]) {
            return PEBBLE_ERR_BAD_MAGIC;
        }
    }

    uint8_t sv_major = bin[offsetof(pebble_process_info_v16_t, struct_version)];
    uint8_t sv_minor = bin[offsetof(pebble_process_info_v16_t, struct_version) + 1u];
    uint32_t limit = (max_virtual_size != 0u) ? max_virtual_size : PEBBLE_DEFAULT_MAX_VIRTUAL_SIZE;
    size_t header_size = 0u;
    size_t uuid_offset = 0u;
    uint32_t reloc_table_offset = 0u;
    uint32_t num_reloc_entries = 0u;
    uint32_t virtual_size = 0u;
    uint16_t load_size = read_le16(bin + offsetof(pebble_process_info_v16_t, load_size));

    if (sv_major == PEBBLE_STRUCT_VERSION_CURRENT_V16) {
        header_size = sizeof(pebble_process_info_v16_t);
        if (bin_len < header_size) {
            return PEBBLE_ERR_TRUNCATED_HEADER;
        }
        num_reloc_entries = read_le32(bin + offsetof(pebble_process_info_v16_t, num_reloc_entries));
        reloc_table_offset = (uint32_t)load_size;
        virtual_size = (uint32_t)read_le16(bin + offsetof(pebble_process_info_v16_t, virtual_size));
        uuid_offset = offsetof(pebble_process_info_v16_t, uuid);
    } else if (sv_major == PEBBLE_STRUCT_VERSION_LEGACY_V9) {
        header_size = sizeof(pebble_process_info_v9_t);
        if (bin_len < header_size) {
            return PEBBLE_ERR_TRUNCATED_HEADER;
        }
        num_reloc_entries = read_le32(bin + offsetof(pebble_process_info_v9_t, num_reloc_entries));
        reloc_table_offset = (uint32_t)load_size;
        /* Pre-v16 headers omit virtual_size; reserve the full app RAM budget for .bss. */
        virtual_size = ((uint32_t)load_size > limit) ? (uint32_t)load_size : limit;
        uuid_offset = offsetof(pebble_process_info_v9_t, uuid);
    } else if (sv_major == PEBBLE_STRUCT_VERSION_LEGACY_V8) {
        header_size = sizeof(pebble_process_info_v8_t);
        if (bin_len < header_size) {
            return PEBBLE_ERR_TRUNCATED_HEADER;
        }
        reloc_table_offset = read_le32(bin + offsetof(pebble_process_info_v8_t, reloc_list_start));
        num_reloc_entries = read_le32(bin + offsetof(pebble_process_info_v8_t, num_reloc_entries));
        virtual_size = ((uint32_t)load_size > limit) ? (uint32_t)load_size : limit;
        uuid_offset = offsetof(pebble_process_info_v8_t, uuid);
    } else {
        return PEBBLE_ERR_UNSUPPORTED_VERSION;
    }

    uint32_t entry_point = read_le32(bin + offsetof(pebble_process_info_v16_t, offset));
    uint32_t sym_table_addr = read_le32(bin + offsetof(pebble_process_info_v16_t, sym_table_addr));
    uint32_t flags = read_le32(bin + offsetof(pebble_process_info_v16_t, flags));

    if (load_size < header_size || (size_t)load_size > bin_len) {
        return PEBBLE_ERR_TRUNCATED_IMAGE;
    }

    if (virtual_size < (uint32_t)load_size) {
        return PEBBLE_ERR_INVALID_VIRTUAL_SIZE;
    }

    if (virtual_size > limit) {
        return PEBBLE_ERR_OVERSIZED_VIRTUAL_SIZE;
    }

    uint64_t reloc_bytes = (uint64_t)num_reloc_entries * 4u;
    if ((size_t)reloc_table_offset > bin_len ||
        reloc_bytes > (uint64_t)(bin_len - (size_t)reloc_table_offset)) {
        return PEBBLE_ERR_TRUNCATED_IMAGE;
    }

    if (entry_point < (uint32_t)header_size || entry_point >= (uint32_t)load_size) {
        return PEBBLE_ERR_INVALID_ENTRY_POINT;
    }

    if (sym_table_addr < (uint32_t)header_size ||
        (sym_table_addr & 3u) != 0u ||
        sym_table_addr > (uint32_t)load_size - 4u) {
        return PEBBLE_ERR_INVALID_SYM_TABLE_ADDR;
    }

    for (uint32_t i = 0; i < num_reloc_entries; ++i) {
        uint32_t reloc_off = read_le32(bin + reloc_table_offset + (size_t)i * 4u);
        if (reloc_off > (uint32_t)load_size - 4u) {
            return PEBBLE_ERR_INVALID_RELOC;
        }
    }

    info_out->struct_version_major = sv_major;
    info_out->struct_version_minor = sv_minor;
    info_out->sdk_version_major = bin[offsetof(pebble_process_info_v16_t, sdk_version)];
    info_out->sdk_version_minor = bin[offsetof(pebble_process_info_v16_t, sdk_version) + 1u];
    info_out->load_size = load_size;
    info_out->virtual_size = virtual_size;
    info_out->entry_point = entry_point;
    info_out->sym_table_addr = sym_table_addr;
    info_out->reloc_table_offset = reloc_table_offset;
    info_out->num_reloc_entries = num_reloc_entries;
    info_out->flags = flags;
    for (size_t i = 0u; i < sizeof(info_out->uuid.bytes); ++i) {
        info_out->uuid.bytes[i] = bin[uuid_offset + i];
    }
    copy_fixed_string(
        info_out->name,
        sizeof(info_out->name),
        bin + offsetof(pebble_process_info_v16_t, name),
        PEBBLE_PROCESS_NAME_BYTES);
    copy_fixed_string(
        info_out->company,
        sizeof(info_out->company),
        bin + offsetof(pebble_process_info_v16_t, company),
        PEBBLE_COMPANY_NAME_BYTES);
    return PEBBLE_OK;
}

pebble_status_t pebble_loader_exec(
    const pebble_hal_t *hal,
    const uint8_t *bin,
    const pebble_image_info_t *info,
    void **syscall_table,
    int32_t *app_return_out) {
    if (hal == NULL || hal->alloc_exec == NULL || bin == NULL || info == NULL ||
        syscall_table == NULL) {
        return PEBBLE_ERR_INVALID_ARG;
    }

    uint8_t *base = (uint8_t *)hal->alloc_exec(hal->ctx, (size_t)info->virtual_size);
    if (base == NULL) {
        return PEBBLE_ERR_ALLOC_FAILED;
    }

    /* Copy code + initialized data (load_size) and zero-fill .bss up to virtual_size. */
    for (uint32_t i = 0; i < (uint32_t)info->load_size; ++i) {
        base[i] = bin[i];
    }
    for (uint32_t i = (uint32_t)info->load_size; i < info->virtual_size; ++i) {
        base[i] = 0u;
    }

    /* Apply 32-bit position-independent relocations. */
    uint32_t base_addr = (uint32_t)(uintptr_t)base;
    for (uint32_t i = 0; i < info->num_reloc_entries; ++i) {
        uint32_t reloc_off = read_le32(bin + info->reloc_table_offset + (size_t)i * 4u);
        uint32_t patched = read_le32(base + reloc_off) + base_addr;
        write_le32(base + reloc_off, patched);
    }

    /* Patch pbl_table_addr with the address of the runtime syscall jump table. */
    write_le32(base + info->sym_table_addr, (uint32_t)(uintptr_t)syscall_table);

    if (hal->sync_icache != NULL) {
        hal->sync_icache(hal->ctx, base, (size_t)info->virtual_size);
    }
#if defined(__arm__)
    __asm__ volatile("dsb sy\n\tisb\n\t" ::: "memory");
#endif

    typedef int32_t (*pebble_entry_fn_t)(void);
    uintptr_t thumb_entry = ((uintptr_t)base + (uintptr_t)info->entry_point) | 1u;
    pebble_entry_fn_t entry = (pebble_entry_fn_t)thumb_entry;
    int32_t rc = entry();

    if (app_return_out != NULL) {
        *app_return_out = rc;
    }

    if (hal->free_exec != NULL) {
        hal->free_exec(hal->ctx, base, (size_t)info->virtual_size);
    }

    return PEBBLE_OK;
}
