# 04: Interactive Pebble Store Apps via Halo Buttons, Temple Taps, Timers & Persistence

**What to build:** Make real interactive multi-screen Pebble Store `.pbw` apps usable inside `HaloEmulator` with Halo's physical control scheme. `HaloEmulator` button clicks and IMU temple taps route into Pebble's `click_config` and `accel_tap_service` callbacks, `app_timer_register`/`app_timer_cancel` drive smooth in-app animations and scheduled callbacks, `persist_*` saves and reloads small app settings across restarts, and pressing Back from the root window cleanly exits the Pebble app back to Halo's Lua environment.

**Blocked by:** 02: First Live Watchface Rendering Inside Official `HaloEmulator`

**Status:** ready-for-agent

- [x] `HaloEmulator` inputs forward live to the running ARM runner using the Halo-to-Pebble mapping: Single-Click → `BUTTON_ID_SELECT`, Double-Click/Long-Press → `BUTTON_ID_BACK`, Single Temple Tap → `BUTTON_ID_DOWN`, Double Temple Tap → `BUTTON_ID_UP`.
- [x] `window_set_click_config_provider`, `window_single_click_subscribe`, `window_long_click_subscribe`, `window_multi_click_subscribe`, and `accel_tap_service_subscribe` dispatch callbacks accurately inside `app_event_loop`.
- [x] `app_timer_register`, `app_timer_reschedule`, and `app_timer_cancel` trigger scheduled callbacks without blocking input or display updates.
- [x] `persist_exists`, `persist_read_*`, `persist_write_*`, and `persist_delete` persist key-value settings inside the app's sandboxed storage across runs.
- [x] Pressing Back on the root window (or interrupting from Lua) unwinds `app_event_loop`, cleans up allocated resources, and returns control to Halo's Lua runtime.
- [x] Automated tests at the `HaloEmulator` seam run a real interactive multi-screen `.pbw` app and verify visible framebuffer state changes across injected button clicks, temple taps, timers, and clean exit.
