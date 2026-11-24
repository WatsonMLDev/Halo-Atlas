#ifndef PEBBLE_COMPAT_H
#define PEBBLE_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pebble_abi.h"
#include "pebble_hal.h"

#define PEBBLE_DEFAULT_MAX_VIRTUAL_SIZE 61440u

typedef enum {
    PEBBLE_PLATFORM_CHALK = 0,  /* 180x180 round, centered in 256x256 */
    PEBBLE_PLATFORM_GABBRO = 1, /* 260x260 round, center-cropped 2px/edge into 256x256 */
    PEBBLE_PLATFORM_BASALT = 2, /* 144x168 rect, centered in 256x256 */
    PEBBLE_PLATFORM_APLITE = 3, /* 144x168 rect, centered in 256x256 */
} pebble_platform_t;

typedef enum {
    PEBBLE_OK = 0,
    PEBBLE_ERR_INVALID_ARG = 1,
    PEBBLE_ERR_TRUNCATED_HEADER = 2,
    PEBBLE_ERR_BAD_MAGIC = 3,
    PEBBLE_ERR_UNSUPPORTED_VERSION = 4,
    PEBBLE_ERR_TRUNCATED_IMAGE = 5,
    PEBBLE_ERR_OVERSIZED_VIRTUAL_SIZE = 6,
    PEBBLE_ERR_INVALID_VIRTUAL_SIZE = 7,
    PEBBLE_ERR_INVALID_ENTRY_POINT = 8,
    PEBBLE_ERR_INVALID_SYM_TABLE_ADDR = 9,
    PEBBLE_ERR_INVALID_RELOC = 10,
    PEBBLE_ERR_ALLOC_FAILED = 11,
} pebble_status_t;

typedef struct {
    uint8_t struct_version_major;
    uint8_t struct_version_minor;
    uint8_t sdk_version_major;
    uint8_t sdk_version_minor;
    uint16_t load_size;
    uint32_t virtual_size;
    uint32_t entry_point;
    uint32_t sym_table_addr;
    uint32_t reloc_table_offset;
    uint32_t num_reloc_entries;
    uint32_t flags;
    char name[PEBBLE_PROCESS_NAME_BYTES + 1u];
    char company[PEBBLE_COMPANY_NAME_BYTES + 1u];
} pebble_image_info_t;

typedef struct {
    bool trace_syscalls;
    uint32_t max_virtual_size; /* 0 uses PEBBLE_DEFAULT_MAX_VIRTUAL_SIZE */
    pebble_platform_t platform;
    uint32_t max_frames;       /* 0 = run until quit/pop; >0 = flush N frames then exit */
    int64_t sim_start_epoch;   /* 0 = live wall clock from HAL; >0 = deterministic UTC epoch */
    int32_t sim_tick_step_sec; /* >0 = advance simulated clock by N sec per frame */
} pebble_run_opts_t;

typedef struct {
    pebble_status_t status;
    int32_t app_return_code;
    uint32_t syscall_count;
    uint32_t frames_flushed;
    pebble_image_info_t image;
} pebble_run_result_t;

const char *pebble_status_str(pebble_status_t status);

/* Convert Unix epoch seconds (UTC) into wall-clock calendar fields. */
void pebble_epoch_to_wall_time(int64_t epoch_sec, pebble_hal_wall_time_t *out);

/* Parse and validate a raw pebble-app.bin header without allocating or executing. */
pebble_status_t pebble_loader_parse(
    const uint8_t *bin,
    size_t bin_len,
    uint32_t max_virtual_size,
    pebble_image_info_t *info_out);

/* Load, relocate, patch pbl_table_addr, and execute a raw pebble-app.bin image. */
pebble_status_t pebble_runtime_run(
    const pebble_hal_t *hal,
    const uint8_t *bin,
    size_t bin_len,
    const pebble_run_opts_t *opts,
    pebble_run_result_t *result_out);

#endif /* PEBBLE_COMPAT_H */
