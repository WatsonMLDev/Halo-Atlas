# 02: First Live Watchface Rendering Inside Official `HaloEmulator`

**What to build:** Launch an unmodified Pebble watchface (such as `test-app.pbw`, *"For Your Friends"*) from Halo's Lua API (`frame.pebble.run(...)`) inside Brilliant Labs' official circular `HaloEmulator` window. The compiled 32-bit ARM Thumb-2 C runner executes the watchface, translates its `Window`, `Layer`, `TextLayer`, `GContext`, `GPath`, and `tick_timer_service` calls into a Pebble-native `GColor8` (`ARGB2222`) shadow buffer, flushes it through Halo's real RGB888 canvas renderer (centering `chalk`/`basalt`/`aplite` and center-cropping `gabbro` by 2px per edge), and streams live frames into `HaloEmulator`'s `256×256` circular HUD window.

**Blocked by:** 01: Native `.pbw` Binary Loader & 663-Slot Syscall Tracer

**Status:** ready-for-agent

- [x] A non-invasive integration attaches `frame.pebble.run(...)` onto `HaloEmulator`'s Lua runtime (without modifying `brilliant_sdk`), unpacking `.pbw` archives on the host (preferring `gabbro` → `chalk` → `basalt` → `aplite`) and launching the compiled ARM runner under `qemu-arm-static`.
- [x] Core PebbleOS watchface APIs (`app_event_loop`, `window_*`, `layer_*`, `text_layer_*`, `graphics_*` primitives, `gpath_*`, `tick_timer_service_subscribe`, and `GColor8` → RGB888 conversion) are implemented in the portable C core and wired into the 663-slot jump table.
- [x] Frame flushes render through Halo's verbatim `canvas` module into `HaloEmulator`'s `DisplayBuffer`, centering `180×180` (`chalk`) and `144×168` (`basalt`/`aplite`) outputs and center-cropping `260×260` (`gabbro`) by 2px per edge inside `256×256`.
- [x] Headless automated tests at the `HaloEmulator` seam verify that launching `test-app.pbw` produces a non-blank watchface framebuffer and that advancing simulated time updates rendered pixels on screen.
- [x] Interactive mode displays the live ticking watchface inside the official circular `HaloEmulator` Pygame window and supports recording `.gif`/`.mp4` output.
