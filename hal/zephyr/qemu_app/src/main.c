/* Copyright (c) 2026 Halo-Atlas Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cortex-M55 Zephyr QEMU verification gate:
 * 1. Loads and executes an unmodified Pebble Store .pbw binary (test-app.pbw)
 *    from Cortex-M55 ITCM (0x10070000) under ARMv8.1-M MPU + CONFIG_FP_HARDABI=y,
 *    verifying non-blank rendering, bundled .pfo fonts, live tick updates, and clean exit.
 * 2. Exercises ARMv8.1-M MPU XN/PXN-clearing (region splitting + full slot restoration),
 *    cooperative k_timer/k_sem sleep polling, and ISR-posted k_msgq quit delivery on DTCM.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "pebble_compat.h"
#include "pebble_hal_zephyr.h"
#include "pebble_embedded_pbw.h"

typedef struct {
	uint32_t frames;
	uint32_t first_non_black;
	uint32_t first_border_non_black;
	uint32_t second_non_black;
	uint32_t first_hash;
	uint32_t second_hash;
	uint32_t diff_rows;
	uint32_t first_row_hashes[PEBBLE_HAL_CANVAS_HEIGHT];
} qemu_frame_stats_t;

typedef struct {
	pebble_zephyr_ctx_t *zctx;
	struct k_timer quit_timer;
	bool timer_started;
	volatile bool posted;
} quit_timer_ctx_t;

/* 32-byte-aligned DTCM buffer (marked XN=1 by Zephyr's default SRAM_0 MPU region). */
static uint8_t g_dtcm_xn_probe[8192] __aligned(32);

static uint32_t fnv1a_bytes(const uint8_t *buf, size_t len)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0u; i < len; ++i) {
		h ^= (uint32_t)buf[i];
		h *= 16777619u;
	}
	return h;
}

static void on_qemu_frame(void *user_data, const uint8_t *rgb888, uint32_t width, uint32_t height)
{
	qemu_frame_stats_t *st = (qemu_frame_stats_t *)user_data;
	if (st == NULL || rgb888 == NULL || width != PEBBLE_HAL_CANVAS_WIDTH ||
	    height != PEBBLE_HAL_CANVAS_HEIGHT) {
		return;
	}

	uint32_t center_non_black = 0u;
	uint32_t border_non_black = 0u;
	uint32_t changed_rows = 0u;

	for (uint32_t y = 0u; y < height; ++y) {
		const uint8_t *row = rgb888 + (size_t)y * (size_t)width * 3u;
		uint32_t rh = fnv1a_bytes(row, (size_t)width * 3u);
		if (st->frames == 0u) {
			st->first_row_hashes[y] = rh;
		} else if (rh != st->first_row_hashes[y]) {
			changed_rows++;
		}
		for (uint32_t x = 0u; x < width; ++x) {
			bool lit = (row[x * 3u + 0u] | row[x * 3u + 1u] | row[x * 3u + 2u]) != 0u;
			if (!lit) {
				continue;
			}
			if (x >= 38u && x < 218u && y >= 38u && y < 218u) {
				center_non_black++;
			} else if (x < 30u && y < 30u) {
				border_non_black++;
			}
		}
	}

	uint32_t frame_hash = fnv1a_bytes(rgb888, (size_t)width * (size_t)height * 3u);
	if (st->frames == 0u) {
		st->first_non_black = center_non_black;
		st->first_border_non_black = border_non_black;
		st->first_hash = frame_hash;
	} else {
		st->second_non_black = center_non_black;
		st->second_hash = frame_hash;
		st->diff_rows = changed_rows;
	}
	st->frames++;
}

static void *dtcm_probe_alloc_exec(void *ctx, size_t size)
{
	ARG_UNUSED(ctx);
	if (size == 0u || size > sizeof(g_dtcm_xn_probe)) {
		return NULL;
	}
	return g_dtcm_xn_probe;
}

static void dtcm_probe_free_exec(void *ctx, void *ptr, size_t size)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(ptr);
	ARG_UNUSED(size);
}

static void quit_timer_expiry(struct k_timer *timer)
{
	quit_timer_ctx_t *q = (quit_timer_ctx_t *)k_timer_user_data_get(timer);
	if (q != NULL && q->zctx != NULL) {
		pebble_hal_event_t quit_ev = {
			.type = PEBBLE_HAL_EVENT_QUIT,
			.value = 0,
		};
		q->posted = pebble_zephyr_post_event(q->zctx, &quit_ev);
	}
}

static void on_dtcm_frame(void *user_data, const uint8_t *rgb888, uint32_t width, uint32_t height)
{
	ARG_UNUSED(rgb888);
	ARG_UNUSED(width);
	ARG_UNUSED(height);
	quit_timer_ctx_t *q = (quit_timer_ctx_t *)user_data;
	if (q != NULL && !q->timer_started) {
		q->timer_started = true;
		/* Arm ISR quit timer after frame 1 flushes so app_event_loop sleeps on k_timer first. */
		k_timer_start(&q->quit_timer, K_MSEC(50), K_NO_WAIT);
	}
}

int main(void)
{
	printk("PEBBLE_QEMU_GATE: START (platform=%s, bin=%u B, res=%u B)\n",
	       PEBBLE_EMBEDDED_PLATFORM_NAME,
	       (unsigned int)g_embedded_pbw_bin_len,
	       (unsigned int)g_embedded_pbw_res_len);

	static qemu_frame_stats_t stats;
	static pebble_zephyr_ctx_t zctx;

	pebble_zephyr_config_t cfg = {
		.resource_buf = (g_embedded_pbw_res_len > 0u) ? g_embedded_pbw_res : NULL,
		.resource_len = g_embedded_pbw_res_len,
		.initial_epoch_sec = 1767236400,
		.live_sleep_on_poll = false,
		.on_frame = on_qemu_frame,
		.on_frame_user_data = &stats,
	};

	pebble_hal_t hal;
	pebble_zephyr_hal_init(&hal, &zctx, &cfg);

	/* 1. Primary gate: execute unmodified test-app.pbw directly from Cortex-M55 ITCM. */
	pebble_run_opts_t opts = {
		.trace_syscalls = false,
		.max_virtual_size = 0u,
		.platform = PEBBLE_EMBEDDED_PLATFORM,
		.max_frames = 2u,
		.sim_start_epoch = 1767236400,
		.sim_tick_step_sec = 900,
	};
	pebble_run_result_t result;
	pebble_status_t status = pebble_runtime_run(
		&hal, g_embedded_pbw_bin, g_embedded_pbw_bin_len, &opts, &result);

	pebble_zephyr_mpu_report_t itcm_mpu = zctx.mpu_report;
	qemu_frame_stats_t itcm_stats = stats;

	/* 2. Secondary gate: DTCM XN-clearing split + live k_timer/k_sem sleep + ISR k_msgq quit + MPU restore. */
	quit_timer_ctx_t quit_ctx = {
		.zctx = &zctx,
		.timer_started = false,
		.posted = false,
	};
	k_timer_init(&quit_ctx.quit_timer, quit_timer_expiry, NULL);
	k_timer_user_data_set(&quit_ctx.quit_timer, &quit_ctx);

	zctx.cfg.on_frame = on_dtcm_frame;
	zctx.cfg.on_frame_user_data = &quit_ctx;
	zctx.cfg.live_sleep_on_poll = true;
	hal.alloc_exec = dtcm_probe_alloc_exec;
	hal.free_exec = dtcm_probe_free_exec;

	pebble_run_opts_t live_opts = {
		.trace_syscalls = false,
		.max_virtual_size = 0u,
		.platform = PEBBLE_EMBEDDED_PLATFORM,
		.max_frames = 0u, /* exits via ISR-posted PEBBLE_HAL_EVENT_QUIT from k_msgq */
		.sim_start_epoch = 1767236400,
		.sim_tick_step_sec = 0,
	};
	pebble_run_result_t dtcm_result;
	pebble_status_t dtcm_status = pebble_runtime_run(
		&hal, g_embedded_pbw_bin, g_embedded_pbw_bin_len, &live_opts, &dtcm_result);

	k_timer_stop(&quit_ctx.quit_timer);
	uint32_t poll_sleeps = zctx.poll_sleeps;
	uint32_t poll_timer_fires = zctx.poll_timer_fires;
	pebble_zephyr_hal_deinit(&zctx);
	pebble_zephyr_mpu_report_t dtcm_mpu = zctx.mpu_report;

	size_t unused_stack = 0u;
	(void)k_thread_stack_space_get(k_current_get(), &unused_stack);
	size_t used_stack = CONFIG_MAIN_STACK_SIZE - unused_stack;

	printk("PEBBLE_QEMU_GATE: itcm status=%s rc=%d syscalls=%u frames=%u app='%s'\n",
	       pebble_status_str(status),
	       (int)result.app_return_code,
	       (unsigned int)result.syscall_count,
	       (unsigned int)result.frames_flushed,
	       result.image.name);

	printk("PEBBLE_QEMU_GATE: itcm_mpu checked=%d dregion=%u free=%u->%u matched=%d was_xn=%d cleared_xn=%d exec=0x%08x size=%u\n",
	       itcm_mpu.mpu_checked ? 1 : 0,
	       (unsigned int)itcm_mpu.dregion,
	       (unsigned int)itcm_mpu.free_slots_before,
	       (unsigned int)itcm_mpu.free_slots_after,
	       (int)itcm_mpu.matched_region,
	       itcm_mpu.was_xn ? 1 : 0,
	       itcm_mpu.cleared_xn ? 1 : 0,
	       (unsigned int)itcm_mpu.exec_addr,
	       (unsigned int)itcm_mpu.exec_size);

	printk("PEBBLE_QEMU_GATE: dtcm_xn status=%s rc=%d posted=%d sleeps=%u timer_fires=%u frames=%u free=%u->%u->%u matched=%d was_xn=%d cleared_xn=%d restored=%d exec=0x%08x\n",
	       pebble_status_str(dtcm_status),
	       (int)dtcm_result.app_return_code,
	       quit_ctx.posted ? 1 : 0,
	       (unsigned int)poll_sleeps,
	       (unsigned int)poll_timer_fires,
	       (unsigned int)dtcm_result.frames_flushed,
	       (unsigned int)dtcm_mpu.free_slots_before,
	       (unsigned int)dtcm_mpu.free_slots_after,
	       (unsigned int)dtcm_mpu.free_slots_after_restore,
	       (int)dtcm_mpu.matched_region,
	       dtcm_mpu.was_xn ? 1 : 0,
	       dtcm_mpu.cleared_xn ? 1 : 0,
	       dtcm_mpu.restored ? 1 : 0,
	       (unsigned int)dtcm_mpu.exec_addr);

	printk("PEBBLE_QEMU_GATE: fb frame1_lit=%u border_lit=%u frame2_lit=%u changed_rows=%u hash1=0x%08x hash2=0x%08x stack_used=%u/%u\n",
	       (unsigned int)itcm_stats.first_non_black,
	       (unsigned int)itcm_stats.first_border_non_black,
	       (unsigned int)itcm_stats.second_non_black,
	       (unsigned int)itcm_stats.diff_rows,
	       (unsigned int)itcm_stats.first_hash,
	       (unsigned int)itcm_stats.second_hash,
	       (unsigned int)used_stack,
	       (unsigned int)CONFIG_MAIN_STACK_SIZE);

	bool in_itcm = (itcm_mpu.exec_addr >= 0x00000000u &&
			itcm_mpu.exec_addr < 0x00080000u) ||
		       (itcm_mpu.exec_addr >= 0x10000000u &&
			itcm_mpu.exec_addr < 0x10080000u);

	bool ok = (status == PEBBLE_OK) &&
		  (result.app_return_code == 0) &&
		  (result.frames_flushed == 2u) &&
		  (itcm_stats.first_non_black > 500u) &&
		  (itcm_stats.first_border_non_black == 0u) &&
		  (itcm_stats.first_hash != itcm_stats.second_hash) &&
		  (itcm_stats.diff_rows > 5u) &&
		  in_itcm &&
		  quit_ctx.posted &&
		  (poll_sleeps >= 1u) &&
		  (poll_timer_fires >= 1u) &&
		  (dtcm_status == PEBBLE_OK) &&
		  (dtcm_result.app_return_code == 0) &&
		  (dtcm_result.frames_flushed == 1u) &&
		  dtcm_mpu.was_xn &&
		  dtcm_mpu.cleared_xn &&
		  dtcm_mpu.restored &&
		  (dtcm_mpu.free_slots_after_restore == dtcm_mpu.free_slots_before);

	if (ok) {
		printk("PEBBLE_QEMU_GATE: PASS\n");
	} else {
		printk("PEBBLE_QEMU_GATE: FAIL\n");
	}

	k_sleep(K_FOREVER);
	return ok ? 0 : 1;
}
