/* Copyright (c) 2026 Halo-Atlas Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEBBLE_HAL_ZEPHYR_H
#define PEBBLE_HAL_ZEPHYR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "pebble_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PEBBLE_ZEPHYR_MAX_MPU_SAVED 4u

typedef void (*pebble_zephyr_frame_cb_t)(
	void *user_data,
	const uint8_t *rgb888,
	uint32_t width,
	uint32_t height);

typedef struct {
	int32_t region;
	uint32_t orig_rbar;
	uint32_t orig_rlar;
	int32_t split_slots[2];
	uint8_t split_count;
} pebble_zephyr_mpu_saved_t;

typedef struct {
	uint32_t dregion;
	uint32_t free_slots_before;
	uint32_t free_slots_after;
	uint32_t free_slots_after_restore;
	int32_t matched_region;
	uintptr_t exec_addr;
	size_t exec_size;
	bool was_xn;
	bool cleared_xn;
	bool restored;
	bool mpu_checked;
} pebble_zephyr_mpu_report_t;

typedef struct {
	/* Optional LittleFS path for unpacked app_resources.pbpack (e.g. "/lfs/pebble/app.pbpack"). */
	const char *resource_path;
	/* Optional in-memory resource pack (takes precedence over resource_path when non-NULL). */
	const uint8_t *resource_buf;
	size_t resource_len;
	/* Optional LittleFS path for per-app persistent KV storage (e.g. "/lfs/pebble/app.kv"). */
	const char *persist_path;
	/* Initial Unix UTC epoch seconds used when HAL wall-clock time is queried. */
	int64_t initial_epoch_sec;
	/* When true, idle event polling sleeps cooperatively via k_timer/k_sem. */
	bool live_sleep_on_poll;
	/* Optional observer invoked on every 256x256 RGB888 canvas flush. */
	pebble_zephyr_frame_cb_t on_frame;
	void *on_frame_user_data;
} pebble_zephyr_config_t;

typedef struct {
	pebble_zephyr_config_t cfg;
	struct k_msgq event_msgq;
	pebble_hal_event_t msgq_storage[CONFIG_PEBBLE_COMPAT_EVENT_QUEUE_LEN];
	struct k_timer poll_timer;
	struct k_sem poll_sem;
	int64_t base_uptime_ms;
	uint32_t poll_sleeps;
	uint32_t poll_timer_fires;
	bool skip_next_idle_sleep;
	bool initialized;
	pebble_zephyr_mpu_saved_t mpu_saved[PEBBLE_ZEPHYR_MAX_MPU_SAVED];
	uint32_t mpu_saved_count;
	pebble_zephyr_mpu_report_t mpu_report;
} pebble_zephyr_ctx_t;

/* Initialize a Zephyr pebble_hal_t backed by ITCM, CDC200 canvas, LittleFS, and k_timer/k_msgq. */
void pebble_zephyr_hal_init(
	pebble_hal_t *hal_out,
	pebble_zephyr_ctx_t *ctx,
	const pebble_zephyr_config_t *cfg);

/* Stop timers, restore any modified MPU slots, and clean up context after pebble_runtime_run. */
void pebble_zephyr_hal_deinit(pebble_zephyr_ctx_t *ctx);

/* Post a Halo button/IMU/quit event from any thread or ISR into the running Pebble app. */
bool pebble_zephyr_post_event(pebble_zephyr_ctx_t *ctx, const pebble_hal_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif /* PEBBLE_HAL_ZEPHYR_H */
