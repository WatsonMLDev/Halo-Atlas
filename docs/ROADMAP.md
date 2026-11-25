# Halo-Atlas — Current Status & Roadmap

This document captures the current state of **Halo-Atlas** after completing the core runtime, desktop `HaloEmulator` integration, and Zephyr RTOS / Cortex-M55 build & QEMU verification gates, along with concrete next steps for running full internet-connected Pebble Store apps (such as **TouchyWeather**) on desktop and on physical Brilliant Labs Halo glasses.

---

## 1. Current State (What Works Today)

All five foundational milestones are complete and verified:

1. **Native ARM Thumb-2 PBLAPP Loader & 663-Symbol Syscall Table:**
   - Loads unmodified `.pbw` archives (`gabbro` → `chalk` → `basalt` → `aplite` fallback) and raw `pebble-app.bin` images (struct v8.x–v16.x), applies 32-bit relocations, zeroes `.bss`, patches `pbl_table_addr`, flushes caches, and executes natively in Thumb-2 mode.
   - Generates all 663 PebbleOS jump-table symbols (677 slots) directly from `third_party/pebble_sdk/libpebble.a`, with safe logging stubs for unimplemented entries.
2. **Live Watchface & Custom UI Rendering (`HaloEmulator` + Verbatim Halo `canvas.c`):**
   - Renders `Window`, `Layer`, `TextLayer`, `BitmapLayer`, `GContext`, `GPath`, `GColor8` (`ARGB2222` → RGB888), and direct framebuffer capture (`graphics_capture_frame_buffer*`) through Brilliant Labs' verbatim `canvas.c` (`CONFIG_CANVAS_ROTATE_90`) into Halo's `256×256` circular HUD.
   - Supports `.pbpack` raw resources, PBI & PNG bitmaps (zlib/DEFLATE, palettes, transparency), and authentic Pebble PFO v2/v3 RLE4 custom fonts plus a bundled fallback font.
3. **Interactive Apps, Timers, & Persistent Storage:**
   - Drives `app_event_loop`, `tick_timer_service`, `app_timer_register`/`reschedule`/`cancel`, button clicks (`SINGLE`, `LONG`, `MULTI`, custom `BACK`), `accel_tap_service`, and per-app sandboxed `persist_*` key-value storage.
   - Maps Halo's 1 physical button + BMA580 temple taps (`SPACE`, `D`, `L`, `T`, `2` in `HaloEmulator`) to Pebble's 4 buttons and tap events.
   - Provides the `uv run halo-atlas <app.pbw>` CLI with interactive HUD window and circular-bezel `.gif`/`.mp4` recording.
4. **Drop-In Zephyr RTOS & Cortex-M55 Firmware Module:**
   - Compiles and links cleanly inside Brilliant Labs' `halo-firmware` (`-b halo`, `arm-zephyr-eabi-gcc 12.2.0`, `CONFIG_FP_HARDABI=y`) via `ZEPHYR_EXTRA_MODULES` with **zero warnings** and **zero edits** to `halo-firmware` (`+71.6 KB` Flash, `+18.2 KB` static RAM; runtime buffers allocate transiently via `halo_malloc`).
   - Verified on Zephyr's Cortex-M55 QEMU target (`mps3_an547`, `CONFIG_ARM_MPU=y`): executes unmodified `test-app.pbw` from ITCM (`0x10070000`, 0 extra MPU slots) and DTCM (`0x30000120`, clears `RBAR.XN` + `RLAR.PXN` via 2 split slots, wakes via `k_timer`/`k_sem`, exits via ISR-posted `k_msgq`, restores all MPU slots `14 → 12 → 14`).

### Syscall Table Coverage

From the generated 663-symbol PebbleOS jump table (`tools/gen_syscall_table.py`):

- **Implemented:** **196 / 663** (**29.6%**) — loader, stdlib/heap, `.pbpack` resources, PBI/PNG bitmaps, PFO fonts, `Window`/`Layer`/`TextLayer`/`BitmapLayer`, `GContext`/`GPath`, buttons/clicks, `accel_tap_service`, `tick_timer_service`, `app_timer_*`, and `persist_*`.
- **Stubbed:** **467 / 663** (**70.4%**) — **36** legacy/deprecated SDK 2.x aliases and **431** current SDK 3.x/4.x functions:

| Stubbed Count | Subsystem | Typical Store App Relevance |
|--------------:|-----------|-----------------------------|
| **100** | Composite UI layers (`MenuLayer`, `SimpleMenuLayer`, `ScrollLayer`, `ActionBarLayer`, `StatusBarLayer`, `ActionMenu`, `RotBitmapLayer`, `NumberWindow`, `ContentIndicator`) | High for multi-screen utility & weather apps |
| **94** | Watch sensors & platform services (`health_*`, `smartstrap_*`, `dictation_*`, `compass_*`, `data_logging_*`, `battery_state_service`, `connection_service`, `vibes_*`, `wakeup_*`, `app_worker_*`, `app_glance_*`) | Low–Medium (`battery`/`connection`/`vibes` are quick wins; `smartstrap`/`dictation`/`health` rarely needed for watchfaces/weather) |
| **80** | Misc / clock / stdlib / locale helpers (`clock_is_24h_style`, `i18n_get_system_locale`, extra `snprintf`/time helpers) | Medium (fast to fill in on demand) |
| **43** | PDC vector graphics (`GDrawCommandImage`, `GDrawCommandSequence`, `GDrawCommandFrame`) | High for weather icons & vector watchfaces |
| **42** | Animations (`Animation`, `PropertyAnimation`) | High for smooth UI transitions & animated icons |
| **38** | `AppMessage` / `Dictionary` / `AppSync` | **Critical** for any internet-connected app (weather, transit, calendar, settings) |
| **26** | Remaining graphics / geometry / bitmap / font helpers | Medium (fill in as apps exercise them) |
| **8** | Remaining `Window` / `Layer` / click helpers | Low–Medium |

> **Tip — Profile Any `.pbw` in Seconds:**
> Run `uv run halo-atlas <app.pbw> --trace` to execute any unmodified `.pbw` and log every `[UNIMPLEMENTED]` syscall it touches. That immediately produces the exact checklist of functions needed for that specific app.

---

## 2. Roadmap & Next Steps

### Phase A — Internet-Connected Pebble Apps (e.g., TouchyWeather)

Pebble binaries cannot make network calls directly; weather and API-driven apps bundle a companion **`pebble-js-app.js`** inside the `.pbw` that runs on the host/phone and sends data over `AppMessage`.

1. **`Dictionary` & `AppMessage` Core (38 syscalls in `runtime/core/`):**
   - Implement Pebble `Dictionary` read/write/iteration (`dict_write_*`, `dict_read_first`, `dict_read_next`, `dict_find`, `dict_serialize_tuplets`, `app_sync_*`).
   - Implement `app_message_open`, `app_message_register_inbox_received`/`outbox_sent`/`dropped`, and `app_message_outbox_begin`/`send`.
   - Extend `pebble_hal_t` with `send_app_message()` and `PEBBLE_HAL_EVENT_APP_MESSAGE` (keeping inbox/outbox buffers dynamically allocated via `pebble_rt_alloc_data()` to preserve Halo's static RAM budget).
2. **PebbleKit JS Companion Host (Desktop first, then Phone BLE):**
   - Extract `pebble-js-app.js` in `python/halo_atlas/pbw.py` and execute it in a lightweight JS runtime (e.g., Node.js or QuickJS) alongside `PebbleRunnerSession`.
   - Provide the PebbleKit JS browser-like shim: `Pebble.addEventListener("ready" | "appmessage" | "showConfiguration")`, `Pebble.sendAppMessage()`, `XMLHttpRequest`/`fetch`, `localStorage`, and `navigator.geolocation` (configurable host lat/lon or IP geolocation).
   - Support app settings / Clay configuration pages (local browser tab callback or pre-seeded `localStorage` JSON config).

### Phase B — Rich Store UI, Vector Icons, & Animations

1. **PDC Vector Graphics (`GDrawCommand*`, 43 syscalls):**
   - Parse Pebble Draw Command (`.pdc`) binary frames/paths/strokes and rasterize them onto `GContext` — unlocking animated weather icons and vector watchfaces.
2. **Animation Engine (`Animation` / `PropertyAnimation`, 42 syscalls):**
   - Implement the cooperative animation scheduler driven by `app_timer` / event-loop timestamps so screen transitions and weather animations run smoothly without blocking input.
3. **Composite UI Layers & Quick-Win Services:**
   - Implement `ScrollLayer`, `MenuLayer` / `SimpleMenuLayer`, `StatusBarLayer`, `ActionBarLayer`, and `RotBitmapLayer` on top of existing `Layer`/`GContext` primitives.
   - Wire lightweight system services: `battery_state_service`, `connection_service`, `vibes_*` (no-op or optional Halo audio cue), `clock_is_24h_style`, and `i18n_get_system_locale`.

### Phase C — On-Device Halo Glasses Integration (`halo-firmware`)

`Halo-Atlas` already compiles and links cleanly into `halo-firmware` (`-b halo`) with zero edits to `halo-firmware`. Running `.pbw` apps directly on physical Halo hardware requires a small integration patch inside `halo-firmware`:

1. **Lua Launcher Binding:** Expose `frame.pebble.run("/lfs/pebble/<app>")` / `frame.pebble.stop()` in `halo-firmware`'s Lua REPL thread (`CONFIG_HALO_LUA_REPL_TASK_STACK_SIZE=65536`, >20× the 3.1 KB measured peak stack).
2. **Hardware Input Wiring:** Route Halo's physical button callbacks and BMA580 IMU tap interrupts into `pebble_zephyr_post_event()`.
3. **BLE App Upload & `AppMessage` Tunnel:** Upload unpacked `pebble-app.bin` + `app_resources.pbpack` into `/lfs/pebble/` over Halo's BLE service and tunnel `AppMessage` packets to a phone companion running `pebble-js-app.js`.
4. **Static RAM Discipline:** `halo-firmware` has ~5 KB of free static RAM (`99.42%` used); keep all new runtime buffers transiently allocated through `pebble_rt_alloc_data()` (`halo_malloc`) and freed on app exit.

### Phase D — Developer & Contributor Ergonomics

1. **One-Command Desktop Launch:** Auto-invoke `make runner` from `uv run halo-atlas` if `build/pebble-runner` is missing so fresh clones work in a single command.
2. **Public CI Workflow:** Add a GitHub Actions workflow running `make test` on every PR and `make zephyr-gate` (Docker Cortex-M55 QEMU + `halo-firmware` link check) on `main`.
