/* Copyright (c) 2026 Halo-Atlas Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/cache.h>

#if defined(CONFIG_FILE_SYSTEM)
#include <zephyr/fs/fs.h>
#endif

#if defined(CONFIG_HALO_MEM_MANAGER)
#include <halo/mem_manager.h>
#endif

#if defined(CONFIG_CDC200) && defined(CONFIG_CANVAS)
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/cdc200.h>
#include <canvas.h>
#endif

#if defined(CONFIG_PEBBLE_COMPAT_VERIFY_MPU)
#include <cmsis_core.h>
#endif

#include "pebble_hal_zephyr.h"

LOG_MODULE_REGISTER(pebble_compat, CONFIG_PEBBLE_COMPAT_LOG_LEVEL);

#if !defined(CONFIG_HALO_MEM_MANAGER)
#if CONFIG_PEBBLE_COMPAT_ITCM_EXEC_ADDR == 0
static uint8_t g_fallback_exec_arena[CONFIG_PEBBLE_COMPAT_FALLBACK_EXEC_ARENA_SIZE] __aligned(32);
#endif
static bool g_fallback_exec_in_use;
K_HEAP_DEFINE(g_fallback_data_heap, CONFIG_PEBBLE_COMPAT_FALLBACK_DATA_HEAP_SIZE);
#endif

#if defined(CONFIG_PEBBLE_COMPAT_VERIFY_MPU)

#define PEBBLE_MPU_RBAR_BASE(a)  ((uint32_t)(a) & MPU_RBAR_BASE_Msk)
#define PEBBLE_MPU_RLAR_LIMIT(last) ((uint32_t)(last) & MPU_RLAR_LIMIT_Msk)
#define PEBBLE_MPU_ALIGN_DOWN(a) ((uint32_t)(a) & ~31u)
#define PEBBLE_MPU_ALIGN_LAST(a) ((uint32_t)(a) | 31u)

#ifdef MPU_RLAR_PXN_Msk
#define PEBBLE_MPU_RLAR_PXN_MSK MPU_RLAR_PXN_Msk
#else
#define PEBBLE_MPU_RLAR_PXN_MSK 0u
#endif

/* Caller must hold irq_lock(). */
static uint32_t count_free_mpu_slots_locked(uint32_t dregion, int32_t *out_slots, uint32_t max_slots)
{
	uint32_t free_count = 0u;
	uint32_t saved = 0u;

	/* Take highest free slots first (away from Zephyr's low static/dynamic regions). */
	for (int32_t i = (int32_t)dregion - 1; i >= 0; --i) {
		MPU->RNR = (uint32_t)i;
		if ((MPU->RLAR & MPU_RLAR_EN_Msk) == 0u) {
			if (out_slots != NULL && saved < max_slots) {
				out_slots[saved++] = i;
			}
			free_count++;
		}
	}
	return free_count;
}

/* Caller must hold irq_lock(). Finds lowest enabled region base strictly above `after_addr` and <= `max_addr`. */
static bool find_next_mpu_region_base_locked(
	uint32_t dregion, uint32_t after_addr, uint32_t max_addr, uint32_t *next_base_out)
{
	bool found = false;
	uint32_t best = 0xFFFFFFFFu;

	for (uint32_t i = 0u; i < dregion; ++i) {
		MPU->RNR = i;
		uint32_t rlar = MPU->RLAR;
		if ((rlar & MPU_RLAR_EN_Msk) == 0u) {
			continue;
		}
		uint32_t base = MPU->RBAR & MPU_RBAR_BASE_Msk;
		if (base > after_addr && base <= max_addr && (!found || base < best)) {
			best = base;
			found = true;
		}
	}
	if (found) {
		*next_base_out = best;
	}
	return found;
}

/* Caller must hold irq_lock(). */
static void restore_arena_mpu_locked(pebble_zephyr_ctx_t *zctx)
{
	if (zctx == NULL || zctx->mpu_saved_count == 0u) {
		return;
	}

	uint32_t dregion = (MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;
	uint32_t ctrl = MPU->CTRL;
	MPU->CTRL = 0u;
	__DSB();
	__ISB();

	for (uint32_t i = zctx->mpu_saved_count; i > 0u; --i) {
		const pebble_zephyr_mpu_saved_t *saved = &zctx->mpu_saved[i - 1u];
		for (uint8_t s = 0u; s < saved->split_count; ++s) {
			if (saved->split_slots[s] >= 0) {
				MPU->RNR = (uint32_t)saved->split_slots[s];
				MPU->RLAR = 0u;
				MPU->RBAR = 0u;
			}
		}
		if (saved->region >= 0) {
			MPU->RNR = (uint32_t)saved->region;
			MPU->RBAR = saved->orig_rbar;
			MPU->RLAR = saved->orig_rlar;
		}
	}

	MPU->CTRL = ctrl;
	__DSB();
	__ISB();

	zctx->mpu_saved_count = 0u;
	zctx->mpu_report.restored = true;
	zctx->mpu_report.free_slots_after_restore = count_free_mpu_slots_locked(dregion, NULL, 0u);
}

static void restore_arena_mpu(pebble_zephyr_ctx_t *zctx)
{
	if (zctx == NULL || zctx->mpu_saved_count == 0u) {
		return;
	}
	unsigned int key = irq_lock();
	restore_arena_mpu_locked(zctx);
	irq_unlock(key);
}

static void ensure_arena_executable(pebble_zephyr_ctx_t *zctx, void *ptr, size_t size)
{
	uintptr_t addr = (uintptr_t)ptr;
	uint32_t dregion = (MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;
	uint32_t arena_start = PEBBLE_MPU_ALIGN_DOWN(addr);
	uint32_t arena_last = PEBBLE_MPU_ALIGN_LAST(addr + size - 1u);

	unsigned int key = irq_lock();

	/* Defensively restore any prior arena split before probing. */
	if (zctx->mpu_saved_count > 0u) {
		restore_arena_mpu_locked(zctx);
	}

	uint32_t free_before = count_free_mpu_slots_locked(dregion, NULL, 0u);
	zctx->mpu_report.dregion = dregion;
	zctx->mpu_report.free_slots_before = free_before;
	zctx->mpu_report.free_slots_after = free_before;
	zctx->mpu_report.free_slots_after_restore = free_before;
	zctx->mpu_report.matched_region = -1;
	zctx->mpu_report.exec_addr = addr;
	zctx->mpu_report.exec_size = size;
	zctx->mpu_report.was_xn = false;
	zctx->mpu_report.cleared_xn = false;
	zctx->mpu_report.restored = false;
	zctx->mpu_report.mpu_checked = true;

	uint32_t probe = arena_start;
	bool any_cleared = false;
	bool any_failed = false;

	for (uint32_t iter = 0u; iter <= dregion; ++iter) {
		uint32_t tt = 0u;
		__asm__ volatile("tt %0, %1" : "=r"(tt) : "r"(probe));

		bool mrvalid = (tt & BIT(16)) != 0u;
		int32_t matched = mrvalid ? (int32_t)(tt & 0xFFu) : -1;

		if (matched < 0 || (uint32_t)matched >= dregion) {
			/* Background map (PRIVDEFENA): ITCM 0x00000000-0x1FFFFFFF is RWX in privileged mode. */
			uint32_t next_base = 0u;
			if (!find_next_mpu_region_base_locked(dregion, probe, arena_last, &next_base)) {
				break;
			}
			probe = next_base;
			continue;
		}

		if (zctx->mpu_report.matched_region < 0) {
			zctx->mpu_report.matched_region = matched;
		}

		MPU->RNR = (uint32_t)matched;
		uint32_t orig_rbar = MPU->RBAR;
		uint32_t orig_rlar = MPU->RLAR;
		uint32_t orig_start = orig_rbar & MPU_RBAR_BASE_Msk;
		uint32_t orig_last = (orig_rlar & MPU_RLAR_LIMIT_Msk) | 31u;

		bool is_xn = (orig_rbar & MPU_RBAR_XN_Msk) != 0u;
		bool is_pxn = (orig_rlar & PEBBLE_MPU_RLAR_PXN_MSK) != 0u;

		if (is_xn || is_pxn) {
			zctx->mpu_report.was_xn = true;

			uint32_t exec_start = (arena_start > orig_start) ? arena_start : orig_start;
			uint32_t exec_last = (arena_last < orig_last) ? arena_last : orig_last;
			bool has_before = (exec_start > orig_start);
			bool has_after = (exec_last < orig_last);
			uint32_t extra_needed = (has_before ? 1u : 0u) + (has_after ? 1u : 0u);

			int32_t free_slots[2] = {-1, -1};
			uint32_t free_now = count_free_mpu_slots_locked(dregion, free_slots, 2u);

			if (zctx->mpu_saved_count >= PEBBLE_ZEPHYR_MAX_MPU_SAVED) {
				any_failed = true;
				break;
			}

			pebble_zephyr_mpu_saved_t *saved = &zctx->mpu_saved[zctx->mpu_saved_count++];
			saved->region = matched;
			saved->orig_rbar = orig_rbar;
			saved->orig_rlar = orig_rlar;
			saved->split_slots[0] = -1;
			saved->split_slots[1] = -1;
			saved->split_count = 0u;

			uint32_t rbar_exec_attrs = orig_rbar & (MPU_RBAR_SH_Msk | MPU_RBAR_AP_Msk);
			uint32_t rbar_orig_attrs = orig_rbar & ~MPU_RBAR_BASE_Msk;
			uint32_t rlar_exec_attrs = orig_rlar & (MPU_RLAR_AttrIndx_Msk | MPU_RLAR_EN_Msk);
			uint32_t rlar_orig_attrs = orig_rlar & ~MPU_RLAR_LIMIT_Msk;

			uint32_t ctrl = MPU->CTRL;
			MPU->CTRL = 0u;
			__DSB();
			__ISB();

			if (free_now >= extra_needed && extra_needed > 0u) {
				uint32_t slot_cursor = 0u;

				/* Original slot becomes executable [exec_start, exec_last] with XN & PXN cleared. */
				MPU->RNR = (uint32_t)matched;
				MPU->RBAR = PEBBLE_MPU_RBAR_BASE(exec_start) | rbar_exec_attrs;
				MPU->RLAR = PEBBLE_MPU_RLAR_LIMIT(exec_last) | rlar_exec_attrs;

				if (has_before) {
					int32_t slot = free_slots[slot_cursor++];
					saved->split_slots[saved->split_count++] = slot;
					MPU->RNR = (uint32_t)slot;
					MPU->RBAR = PEBBLE_MPU_RBAR_BASE(orig_start) | rbar_orig_attrs;
					MPU->RLAR = PEBBLE_MPU_RLAR_LIMIT(exec_start - 1u) | rlar_orig_attrs;
				}
				if (has_after) {
					int32_t slot = free_slots[slot_cursor++];
					saved->split_slots[saved->split_count++] = slot;
					MPU->RNR = (uint32_t)slot;
					MPU->RBAR = PEBBLE_MPU_RBAR_BASE(exec_last + 1u) | rbar_orig_attrs;
					MPU->RLAR = PEBBLE_MPU_RLAR_LIMIT(orig_last) | rlar_orig_attrs;
				}
			} else {
				/* Exact fit or no spare slots: clear XN and PXN on the matched region directly. */
				MPU->RNR = (uint32_t)matched;
				MPU->RBAR = orig_rbar & ~MPU_RBAR_XN_Msk;
				MPU->RLAR = orig_rlar & ~PEBBLE_MPU_RLAR_PXN_MSK;
			}

			MPU->CTRL = ctrl;
			__DSB();
			__ISB();
			any_cleared = true;
		}

		if (orig_last >= arena_last || orig_last == 0xFFFFFFFFu) {
			break;
		}
		probe = orig_last + 1u;
	}

	zctx->mpu_report.cleared_xn = any_cleared && !any_failed;
	zctx->mpu_report.free_slots_after = count_free_mpu_slots_locked(dregion, NULL, 0u);
	irq_unlock(key);
}

#endif /* CONFIG_PEBBLE_COMPAT_VERIFY_MPU */

static void *zephyr_alloc_exec(void *ctx, size_t size)
{
	ARG_UNUSED(ctx);
	if (size == 0u) {
		return NULL;
	}

#if defined(CONFIG_HALO_MEM_MANAGER) && defined(CONFIG_HALO_MEM_USE_EXTERNAL_SRAM)
	/* Halo's 448 KB ITCM heap (0x00010000 - 0x0007FFFF) lives in HALO_MEM_REGION_EXTERNAL. */
	return halo_malloc(size, HALO_MEM_REGION_EXTERNAL);
#elif defined(CONFIG_HALO_MEM_MANAGER)
	return halo_malloc(size, HALO_MEM_REGION_AUTO);
#else
	if (g_fallback_exec_in_use || size > CONFIG_PEBBLE_COMPAT_FALLBACK_EXEC_ARENA_SIZE) {
		return NULL;
	}
	g_fallback_exec_in_use = true;
#if CONFIG_PEBBLE_COMPAT_ITCM_EXEC_ADDR != 0
	return (void *)(uintptr_t)CONFIG_PEBBLE_COMPAT_ITCM_EXEC_ADDR;
#else
	return g_fallback_exec_arena;
#endif
#endif
}

static void zephyr_free_exec(void *ctx, void *ptr, size_t size)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;
	ARG_UNUSED(size);

#if defined(CONFIG_PEBBLE_COMPAT_VERIFY_MPU)
	restore_arena_mpu(zctx);
#else
	ARG_UNUSED(zctx);
#endif

	if (ptr == NULL) {
		return;
	}

#if defined(CONFIG_HALO_MEM_MANAGER)
	halo_free(ptr);
#else
	g_fallback_exec_in_use = false;
#endif
}

static void *zephyr_alloc_data(void *ctx, size_t size)
{
	ARG_UNUSED(ctx);
	if (size == 0u) {
		return NULL;
	}

#if defined(CONFIG_HALO_MEM_MANAGER)
	return halo_malloc(size, HALO_MEM_REGION_AUTO);
#else
	return k_heap_aligned_alloc(&g_fallback_data_heap, 8u, size, K_NO_WAIT);
#endif
}

static void zephyr_free_data(void *ctx, void *ptr, size_t size)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(size);
	if (ptr == NULL) {
		return;
	}

#if defined(CONFIG_HALO_MEM_MANAGER)
	halo_free(ptr);
#else
	k_heap_free(&g_fallback_data_heap, ptr);
#endif
}

static void zephyr_sync_icache(void *ctx, void *ptr, size_t size)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;

	if (ptr == NULL || size == 0u) {
		return;
	}

#if defined(CONFIG_PEBBLE_COMPAT_VERIFY_MPU)
	if (zctx != NULL) {
		ensure_arena_executable(zctx, ptr, size);
	}
#else
	ARG_UNUSED(zctx);
#endif

#if defined(CONFIG_DCACHE)
	(void)sys_cache_data_flush_range(ptr, size);
#endif
#if defined(CONFIG_ICACHE)
	if (sys_cache_instr_invd_range(ptr, size) != 0) {
		(void)sys_cache_instr_invd_all();
	}
#endif
#if defined(__arm__)
	__asm__ volatile("dsb sy\n\tisb\n\t" ::: "memory");
#endif
}

static uint64_t zephyr_get_time_ms(void *ctx)
{
	ARG_UNUSED(ctx);
	return (uint64_t)k_uptime_get();
}

static bool zephyr_get_wall_time(void *ctx, int64_t *unix_sec_out, pebble_hal_wall_time_t *tm_out)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;
	int64_t elapsed_ms = 0;
	int64_t base_sec = 0;

	if (zctx != NULL) {
		elapsed_ms = k_uptime_get() - zctx->base_uptime_ms;
		base_sec = zctx->cfg.initial_epoch_sec;
	} else {
		elapsed_ms = k_uptime_get();
	}

	int64_t sec = base_sec + (elapsed_ms / 1000);
	if (unix_sec_out != NULL) {
		*unix_sec_out = sec;
	}
	if (tm_out != NULL) {
		pebble_epoch_to_wall_time(sec, tm_out);
	}
	return true;
}

static void zephyr_flush_canvas(void *ctx, const uint8_t *rgb888, uint32_t width, uint32_t height)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;

	if (rgb888 == NULL || width == 0u || height == 0u) {
		return;
	}

#if defined(CONFIG_CDC200) && defined(CONFIG_CANVAS)
	const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (device_is_ready(display_dev) && width == LOG_WIDTH && height == LOG_HEIGHT) {
		struct cdc200_fb_desc layer = {0};
		cdc200_get_framebuffer(display_dev, 0, &layer);
		if (layer.fb_addr != NULL) {
			Canvas canvas;
			canvas_init(&canvas, (uint8_t (*)[PHYS_WIDTH][3])layer.fb_addr);
			canvas_draw_bitmap(&canvas, 0, 0, (int)width, (int)height, rgb888);
#if defined(CONFIG_DCACHE)
			(void)sys_cache_data_flush_range(
				layer.fb_addr, (size_t)PHYS_WIDTH * (size_t)PHYS_HEIGHT * 3u);
#endif
		}
	}
#endif

	if (zctx != NULL && zctx->cfg.on_frame != NULL) {
		zctx->cfg.on_frame(zctx->cfg.on_frame_user_data, rgb888, width, height);
	}
}

static void poll_timer_expiry(struct k_timer *timer)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)k_timer_user_data_get(timer);
	if (zctx != NULL) {
		zctx->poll_timer_fires++;
		k_sem_give(&zctx->poll_sem);
	}
}

static bool zephyr_poll_event(void *ctx, pebble_hal_event_t *out_event)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;

	if (zctx == NULL || out_event == NULL) {
		return false;
	}

	/* Consume stale wakeup tokens before reading the queue so any concurrent ISR post
	 * after k_msgq_get() leaves a fresh token in poll_sem and never loses a wakeup. */
	while (k_sem_take(&zctx->poll_sem, K_NO_WAIT) == 0) {
	}

	if (k_msgq_get(&zctx->event_msgq, out_event, K_NO_WAIT) == 0) {
		zctx->skip_next_idle_sleep = true;
		return true;
	}

	if (zctx->cfg.live_sleep_on_poll) {
		if (zctx->skip_next_idle_sleep) {
			zctx->skip_next_idle_sleep = false;
			return false;
		}
		k_timer_start(&zctx->poll_timer, K_MSEC(5), K_NO_WAIT);
		zctx->poll_sleeps++;
		(void)k_sem_take(&zctx->poll_sem, K_MSEC(10));
		k_timer_stop(&zctx->poll_timer);
		if (k_msgq_get(&zctx->event_msgq, out_event, K_NO_WAIT) == 0) {
			zctx->skip_next_idle_sleep = true;
			return true;
		}
	}

	return false;
}

static size_t zephyr_read_resource(void *ctx, uint32_t offset, void *dst, size_t len)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;

	if (zctx == NULL || dst == NULL || len == 0u) {
		return 0u;
	}

	if (zctx->cfg.resource_buf != NULL) {
		if ((size_t)offset >= zctx->cfg.resource_len) {
			return 0u;
		}
		size_t avail = zctx->cfg.resource_len - (size_t)offset;
		size_t to_copy = (len < avail) ? len : avail;
		memcpy(dst, zctx->cfg.resource_buf + offset, to_copy);
		return to_copy;
	}

#if defined(CONFIG_FILE_SYSTEM)
	if (zctx->cfg.resource_path != NULL) {
		struct fs_file_t f;
		fs_file_t_init(&f);
		if (fs_open(&f, zctx->cfg.resource_path, FS_O_READ) < 0) {
			return 0u;
		}
		size_t got = 0u;
		if (fs_seek(&f, (off_t)offset, FS_SEEK_SET) == 0) {
			uint8_t *buf = (uint8_t *)dst;
			while (got < len) {
				ssize_t rc = fs_read(&f, buf + got, len - got);
				if (rc <= 0) {
					break;
				}
				got += (size_t)rc;
			}
		}
		(void)fs_close(&f);
		return got;
	}
#endif

	return 0u;
}

static size_t zephyr_read_persist(void *ctx, void *dst, size_t max_len)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;

	if (zctx == NULL || dst == NULL || max_len == 0u) {
		return 0u;
	}

#if defined(CONFIG_FILE_SYSTEM)
	if (zctx->cfg.persist_path != NULL) {
		struct fs_file_t f;
		fs_file_t_init(&f);
		if (fs_open(&f, zctx->cfg.persist_path, FS_O_READ) < 0) {
			return 0u;
		}
		uint8_t *buf = (uint8_t *)dst;
		size_t total = 0u;
		while (total < max_len) {
			ssize_t rc = fs_read(&f, buf + total, max_len - total);
			if (rc <= 0) {
				break;
			}
			total += (size_t)rc;
		}
		(void)fs_close(&f);
		return total;
	}
#endif

	return 0u;
}

#if defined(CONFIG_FILE_SYSTEM)
static bool build_tmp_path(char *dst, size_t dst_cap, const char *base, const char *suffix)
{
	size_t base_len = strlen(base);
	size_t suf_len = strlen(suffix);
	if (base_len + suf_len + 1u > dst_cap) {
		return false;
	}
	memcpy(dst, base, base_len);
	memcpy(dst + base_len, suffix, suf_len + 1u);
	return true;
}
#endif

static bool zephyr_write_persist(void *ctx, const void *src, size_t len)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;

	if (zctx == NULL || (src == NULL && len > 0u)) {
		return false;
	}

#if defined(CONFIG_FILE_SYSTEM)
	if (zctx->cfg.persist_path != NULL) {
		char tmp_path[128];
		if (!build_tmp_path(tmp_path, sizeof(tmp_path), zctx->cfg.persist_path, ".tmp")) {
			return false;
		}

		struct fs_file_t f;
		fs_file_t_init(&f);
		if (fs_open(&f, tmp_path, FS_O_CREATE | FS_O_WRITE) < 0) {
			return false;
		}
		bool ok = (fs_truncate(&f, 0) == 0);
		const uint8_t *buf = (const uint8_t *)src;
		size_t off = 0u;
		while (ok && off < len) {
			ssize_t rc = fs_write(&f, buf + off, len - off);
			if (rc <= 0) {
				ok = false;
				break;
			}
			off += (size_t)rc;
		}
		if (fs_close(&f) != 0) {
			ok = false;
		}
		if (ok) {
			ok = (fs_rename(tmp_path, zctx->cfg.persist_path) == 0);
		}
		if (!ok) {
			(void)fs_unlink(tmp_path);
		}
		return ok;
	}
#endif

	return true;
}

static bool zephyr_send_protocol_packet(void *ctx, const uint8_t *pkt, size_t len)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;
	if (zctx == NULL || zctx->cfg.on_protocol_tx == NULL || pkt == NULL || len == 0u) {
		return false;
	}
	return zctx->cfg.on_protocol_tx(zctx->cfg.on_protocol_tx_user_data, pkt, len);
}

#if defined(CONFIG_PEBBLE_COMPAT_PROTOCOL_RX_BUF_SIZE) && (CONFIG_PEBBLE_COMPAT_PROTOCOL_RX_BUF_SIZE > 0)
static void ring_write_bytes(pebble_zephyr_ctx_t *zctx, const uint8_t *src, size_t len)
{
	const size_t cap = sizeof(zctx->proto_rx_buf);
	for (size_t i = 0u; i < len; ++i) {
		zctx->proto_rx_buf[zctx->proto_rx_head] = src[i];
		zctx->proto_rx_head = (zctx->proto_rx_head + 1u) % cap;
	}
	zctx->proto_rx_used += len;
}

static void ring_read_bytes(pebble_zephyr_ctx_t *zctx, uint8_t *dst, size_t len)
{
	const size_t cap = sizeof(zctx->proto_rx_buf);
	for (size_t i = 0u; i < len; ++i) {
		uint8_t b = zctx->proto_rx_buf[zctx->proto_rx_tail];
		zctx->proto_rx_tail = (zctx->proto_rx_tail + 1u) % cap;
		if (dst != NULL) {
			dst[i] = b;
		}
	}
	zctx->proto_rx_used -= len;
}

static size_t zephyr_recv_protocol_packet(void *ctx, uint8_t *dst, size_t max_len)
{
	pebble_zephyr_ctx_t *zctx = (pebble_zephyr_ctx_t *)ctx;
	if (zctx == NULL) {
		return 0u;
	}

	unsigned int key = irq_lock();
	if (zctx->proto_rx_used < 2u) {
		irq_unlock(key);
		return 0u;
	}
	uint8_t hdr[2];
	ring_read_bytes(zctx, hdr, 2u);
	size_t pkt_len = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
	if (pkt_len > zctx->proto_rx_used) {
		zctx->proto_rx_head = 0u;
		zctx->proto_rx_tail = 0u;
		zctx->proto_rx_used = 0u;
		irq_unlock(key);
		return 0u;
	}
	if (dst != NULL && pkt_len <= max_len) {
		ring_read_bytes(zctx, dst, pkt_len);
		irq_unlock(key);
		return pkt_len;
	}
	/* Discard oversized packet cleanly. */
	ring_read_bytes(zctx, NULL, pkt_len);
	irq_unlock(key);
	return 0u;
}
#endif

static void zephyr_log(void *ctx, pebble_hal_log_level_t level, const char *msg)
{
	ARG_UNUSED(ctx);
	if (msg == NULL) {
		return;
	}

	switch (level) {
	case PEBBLE_HAL_LOG_DEBUG:
		LOG_DBG("%s", msg);
		break;
	case PEBBLE_HAL_LOG_INFO:
		LOG_INF("%s", msg);
		break;
	case PEBBLE_HAL_LOG_WARN:
		LOG_WRN("%s", msg);
		break;
	case PEBBLE_HAL_LOG_ERROR:
	default:
		LOG_ERR("%s", msg);
		break;
	}
}

void pebble_zephyr_hal_init(
	pebble_hal_t *hal_out,
	pebble_zephyr_ctx_t *ctx,
	const pebble_zephyr_config_t *cfg)
{
	memset(ctx, 0, sizeof(*ctx));
	if (cfg != NULL) {
		ctx->cfg = *cfg;
	}
	ctx->base_uptime_ms = k_uptime_get();

	k_msgq_init(
		&ctx->event_msgq,
		(char *)ctx->msgq_storage,
		sizeof(pebble_hal_event_t),
		CONFIG_PEBBLE_COMPAT_EVENT_QUEUE_LEN);
	k_sem_init(&ctx->poll_sem, 0u, 1u);
	k_timer_init(&ctx->poll_timer, poll_timer_expiry, NULL);
	k_timer_user_data_set(&ctx->poll_timer, ctx);
	ctx->initialized = true;

	memset(hal_out, 0, sizeof(*hal_out));
	hal_out->ctx = ctx;
	hal_out->alloc_exec = zephyr_alloc_exec;
	hal_out->free_exec = zephyr_free_exec;
	hal_out->alloc_data = zephyr_alloc_data;
	hal_out->free_data = zephyr_free_data;
	hal_out->sync_icache = zephyr_sync_icache;
	hal_out->get_time_ms = zephyr_get_time_ms;
	hal_out->get_wall_time = zephyr_get_wall_time;
	hal_out->flush_canvas = zephyr_flush_canvas;
	hal_out->poll_event = zephyr_poll_event;
	hal_out->read_resource = zephyr_read_resource;
	hal_out->read_persist = zephyr_read_persist;
	hal_out->write_persist = zephyr_write_persist;
	hal_out->send_protocol_packet =
		(ctx->cfg.on_protocol_tx != NULL) ? zephyr_send_protocol_packet : NULL;
#if defined(CONFIG_PEBBLE_COMPAT_PROTOCOL_RX_BUF_SIZE) && (CONFIG_PEBBLE_COMPAT_PROTOCOL_RX_BUF_SIZE > 0)
	hal_out->recv_protocol_packet = zephyr_recv_protocol_packet;
#else
	hal_out->recv_protocol_packet = NULL;
#endif
	hal_out->log = zephyr_log;
}

void pebble_zephyr_hal_deinit(pebble_zephyr_ctx_t *ctx)
{
	if (ctx == NULL || !ctx->initialized) {
		return;
	}
	k_timer_stop(&ctx->poll_timer);
	k_msgq_purge(&ctx->event_msgq);
#if defined(CONFIG_PEBBLE_COMPAT_VERIFY_MPU)
	restore_arena_mpu(ctx);
#endif
	ctx->initialized = false;
}

bool pebble_zephyr_post_event(pebble_zephyr_ctx_t *ctx, const pebble_hal_event_t *ev)
{
	if (ctx == NULL || !ctx->initialized || ev == NULL) {
		return false;
	}
	if (k_msgq_put(&ctx->event_msgq, ev, K_NO_WAIT) != 0) {
		return false;
	}
	k_sem_give(&ctx->poll_sem);
	return true;
}

bool pebble_zephyr_post_protocol_rx(pebble_zephyr_ctx_t *ctx, const uint8_t *pkt, size_t len)
{
#if defined(CONFIG_PEBBLE_COMPAT_PROTOCOL_RX_BUF_SIZE) && (CONFIG_PEBBLE_COMPAT_PROTOCOL_RX_BUF_SIZE > 0)
	if (ctx == NULL || !ctx->initialized || pkt == NULL || len == 0u || len > 0xffffu) {
		return false;
	}
	const size_t cap = sizeof(ctx->proto_rx_buf);
	const size_t needed = 2u + len;
	unsigned int key = irq_lock();
	if (needed > cap - ctx->proto_rx_used) {
		irq_unlock(key);
		return false;
	}
	uint8_t hdr[2] = {(uint8_t)(len & 0xffu), (uint8_t)((len >> 8) & 0xffu)};
	ring_write_bytes(ctx, hdr, 2u);
	ring_write_bytes(ctx, pkt, len);
	irq_unlock(key);

	pebble_hal_event_t ev = {.type = PEBBLE_HAL_EVENT_PROTOCOL_RX, .value = 0};
	(void)k_msgq_put(&ctx->event_msgq, &ev, K_NO_WAIT);
	k_sem_give(&ctx->poll_sem);
	return true;
#else
	ARG_UNUSED(ctx);
	ARG_UNUSED(pkt);
	ARG_UNUSED(len);
	return false;
#endif
}
