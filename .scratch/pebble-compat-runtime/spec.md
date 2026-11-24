# Spec: Native PebbleOS Binary Compatibility Runtime for Brilliant Labs Halo

**Status:** ready-for-agent

## Problem Statement

Brilliant Labs Halo owners want to run existing Pebble smartwatch watchfaces and apps downloaded directly from the Pebble/Rebble App Store (`.pbw` bundles) natively on Halo's circular optical display — without needing access to an app's C source code, without recompiling apps, and without running a phone-side Pebble CPU emulator that continuously streams video frames over Bluetooth LE. Furthermore, while physical Halo glasses are not yet in hand, there is no existing way to build, execute, and visually verify firmware-grade native Pebble binary execution inside Brilliant Labs' official `HaloEmulator`, because the stock emulator only runs mocked x86 Lua scripts rather than compiled ARM firmware.

## Solution

Build **Halo-Atlas**, a Wine/Proton + DXVK-style native ARM Thumb-2 binary compatibility runtime for PebbleOS `.pbw` applications, designed as an OS-agnostic C core behind a minimal hardware abstraction layer (`pebble_hal`):

1. **Native Binary Execution Without Source Code:** Load raw, unmodified ARM Thumb-2 `pebble-app.bin` executables and `app_resources.pbpack` resource packs straight from `.pbw` archives, apply relocations in executable RAM, patch the PebbleOS syscall jump table pointer (`pbl_table_addr`), and execute the app's native Thumb-2 instructions directly.
2. **Stub-First Syscall Translation ("Wine/Proton Layer"):** Auto-generate a complete 663-slot PebbleOS syscall jump table with safe logging/no-op defaults so closed-source store apps never crash on unimplemented APIs, and implement the core PebbleOS subsystems (event loop, windows, layers, timers, resources, button clicks, IMU taps) incrementally.
3. **Display Translation & Scaling ("DXVK + Gamescope Layer"):** Render Pebble's `GContext`, `Layer`, `TextLayer`, `GBitmap`, `GPath`, and direct framebuffer (`graphics_capture_frame_buffer`) output into a compact Pebble-format `GColor8` (`ARGB2222`) shadow buffer and flush it to Halo's `256×256` RGB888 canvas — automatically centering `180×180` (`chalk`) and `144×168` (`basalt`/`aplite`) apps and center-cropping `260×260` (`gabbro`) apps by 2px per edge.
4. **Official `HaloEmulator` Integration + Firmware Portability:** Expose a `frame.pebble.run(...)` Lua API inside Brilliant Labs' official `HaloEmulator` backed by the exact same compiled 32-bit ARM Thumb-2 C runtime running under `qemu-arm-static` and Halo's real canvas renderer, so real Pebble Store `.pbw` apps can be watched, interacted with via Halo's button/temple-tap keys, and recorded on desktop today — and then compiled into `halo-firmware` behind a thin Zephyr HAL adapter as the final verification gate.

## User Stories

1. As a Halo owner, I want to run an unmodified `.pbw` watchface downloaded from the Pebble/Rebble App Store without needing its C source code, so that I can immediately use the existing Pebble app ecosystem.
2. As a Halo developer without physical glasses yet, I want to launch a real `.pbw` binary inside Brilliant Labs' official `HaloEmulator` window via `frame.pebble.run(...)`, so that I can visually verify real ARM binary execution and rendering on my desktop.
3. As a Halo developer, I want the `.pbw` loader to parse `PebbleProcessInfo` headers, allocate executable memory, zero-initialize `.bss`, apply all 32-bit relocations, and patch `pbl_table_addr`, so that any valid position-independent Pebble binary can enter `main()` cleanly.
4. As a Halo developer, I want invalid or corrupted `.pbw` binaries (bad magic, truncated load size, unsupported struct version, or oversized virtual size) to fail gracefully with a clear diagnostic error instead of crashing the host runtime, so that bad downloads cannot wedge the glasses or emulator.
5. As a Halo developer, I want all 663 PebbleOS jump-table slots populated automatically from the official SDK's `libpebble.a` symbol table with safe default logger stubs, so that unmodified store apps calling unimplemented APIs (such as vibration or health metrics) log the missing call name and keep running instead of jumping to null.
6. As a Halo developer, I want a syscall trace mode that prints every PebbleOS API call invoked by a running `.pbw` binary by name and table index, so that I can see the exact minimum set of APIs required to bring up any new store app.
7. As a Halo developer, I want hard-float host C code to interoperate safely with soft-float Cortex-M3 `.pbw` binaries and short-enum Pebble ABI structs, so that floating-point arguments and packed structs do not silently corrupt registers or memory.
8. As a Halo owner, I want circular `chalk` (`180×180`) watchfaces to render centered cleanly inside Halo's `256×256` circular optical display, so that existing Pebble Time Round apps look natural on Halo.
9. As a Halo owner, I want circular `gabbro` (`260×260`) apps to center-crop 2 pixels per edge into Halo's `256×256` display, so that modern round Pebble apps render at crisp 1:1 native pixel fidelity.
10. As a Halo owner, I want rectangular `basalt` and `aplite` (`144×168`) apps to render centered inside Halo's `256×256` circular aperture when a `.pbw` has no circular platform binary, so that classic Pebble Store apps still work cleanly.
11. As a Halo owner, I want Pebble's `GColor8` (`ARGB2222`, 64 colors + alpha) colors translated accurately into Halo's RGB888 framebuffer, so that color watchfaces and apps retain their intended palette.
12. As a Halo owner, I want watchfaces using `Window`, `Layer`, `TextLayer`, `BitmapLayer`, `GContext` drawing primitives (pixels, lines, rectangles, circles, arcs, paths, text), and `tick_timer_service` to update live on the display as time advances, so that real watchfaces tick accurately.
13. As a Halo owner, I want apps that read embedded assets (`app_resources.pbpack`) — including raw data, PBI/PNG bitmaps, and Pebble bitmap fonts — to load and render their bundled icons, graphics, and typography natively, so that apps display their authentic visuals.
14. As a Halo owner, I want apps that call `graphics_capture_frame_buffer()` and `graphics_release_frame_buffer()` to read and write pixels directly against a Pebble-format shadow framebuffer that flushes to Halo's canvas on release, so that low-level pixel-poking apps also work.
15. As a Halo owner, I want to interact with multi-screen Pebble apps inside `HaloEmulator` using Halo's physical control scheme (single-click = Select, double-click/long-press = Back, single temple tap = Down, double temple tap = Up), so that interactive apps are usable with Halo's real input hardware.
16. As a Halo owner, I want Pebble apps using `accel_tap_service` to receive Halo IMU tap events, so that shake-to-refresh or tap-triggered watchface features work.
17. As a Halo owner, I want Pebble apps using `app_timer_register` and `app_timer_cancel` to run smooth animations and scheduled callbacks, so that animated watchfaces and interactive apps behave naturally.
18. As a Halo owner, I want Pebble apps using `persist_*` key-value storage to save and reload small settings within the app's sandboxed storage, so that user preferences survive app restarts.
19. As a Halo owner, I want to start and cleanly stop a running Pebble `.pbw` app from Halo's Lua environment (`frame.pebble.run(...)` / exit on Back from the root window or interrupt), so that Pebble apps coexist cleanly with normal Halo Lua apps.
20. As a Halo developer, I want to record live `.gif` or `.mp4` captures of a `.pbw` app running inside `HaloEmulator`, so that I can verify and share visual progress before physical glasses arrive.
21. As a Halo firmware developer, I want the entire Pebble compatibility engine separated from host OS and Zephyr specifics by a small `pebble_hal` C interface, so that the exact same C codebase runs under `qemu-arm-static` on desktop and compiles directly into `halo-firmware`.
22. As a Halo firmware developer, I want to verify that the C compatibility module compiles and links cleanly against `halo-firmware` (Zephyr RTOS, ARM Cortex-M55, hard-float ABI, ITCM allocation) and executes a real `.pbw` binary on Zephyr's Cortex-M55 QEMU target, so that the runtime is proven firmware-ready when physical Halo glasses arrive.

## Implementation Decisions

### Core Architecture & Module Boundaries
- **OS-Agnostic Portable C Core (`pebble_compat`):** All Pebble binary loading, syscall dispatch, UI/layer tree management, graphics rendering, resource pack parsing, and event loop logic live in pure C with zero Linux or Zephyr headers. All platform interactions cross a single minimal **`pebble_hal`** C interface (~6 primitives: executable memory allocation/free, monotonic milliseconds and wall-clock time, `256×256` RGB888 canvas flush, non-blocking input event poll, resource file/buffer read, and logging).
- **Binary Loader Module:** Parses `PebbleProcessInfo` (struct version 16 and legacy <16 offsets), validates magic (`PBLAPP`) and size bounds, allocates `virtual_size` bytes from `pebble_hal`'s executable arena, copies `load_size` bytes, zeroes `.bss`, applies every 32-bit offset in the appended relocation table (`+= base_addr`), patches `pbl_table_addr` with the address of the runtime's 663-entry syscall table, issues memory/instruction barriers, and invokes `(base_addr + entry_point) | 1`.
- **Auto-Generated 663-Slot Syscall Jump Table:** A build-time generator script inspects the official Pebble SDK's `libpebble.a` to extract the exact `(byte_offset / 4) -> symbol_name` mapping for all 663 exported PebbleOS functions. Every slot defaults to a safe generated stub that logs the called symbol name/index and returns `0` / `NULL`, while implemented PebbleOS functions override their corresponding slots cleanly.
- **ARM ABI Compatibility Contract:**
  - The host runtime (both `arm-linux-gnueabihf` desktop runner and Halo's Cortex-M55 `CONFIG_FP_HARDABI=y` firmware) compiles with hard-float ABI, while `.pbw` binaries are Cortex-M3 soft-float (`-mfloat-abi=soft`, `-fshort-enums`).
  - Any jump-table function taking or returning `float`/`double` is explicitly annotated with `__attribute__((pcs("aapcs")))` so the compiler expects soft-float register conventions at the `.pbw` boundary.
  - Pebble ABI structs containing enums use explicit fixed-width integer fields or isolated `-fshort-enums` compilation to match the Pebble SDK's packed struct layout.
- **Graphics & Display Module ("DXVK + Gamescope Layer"):**
  - Maintains a compact Pebble-native `GColor8` (`ARGB2222`, 1 byte/pixel) shadow framebuffer sized to the loaded `.pbw` platform (`180×180` for `chalk`, `260×260` for `gabbro`, `144×168` for `basalt`/`aplite`).
  - Implements `Window`, `Layer` hierarchy + dirty marking, `TextLayer`, `BitmapLayer`, `GContext` drawing primitives, `GPath` stroke/fill, `GBitmap` blitting, Pebble bitmap font rendering (with fallback to Halo's built-in Dogica pixel font when a system font is requested), and `graphics_capture_frame_buffer` / `graphics_release_frame_buffer`.
  - On frame flush, converts `GColor8` pixels to RGB888 and blits them through Halo's verbatim `canvas` drawing module (preserving Halo's rotated physical framebuffer layout), centering `chalk`/`basalt`/`aplite` inside `256×256` and center-cropping `gabbro` by 2px per edge.
- **Resource Pack (`pbpack`) & `.pbw` Unpacking:**
  - Unpacking `.pbw` ZIP archives into raw `pebble-app.bin` + `app_resources.pbpack` (plus platform selection preferring `gabbro` → `chalk` → `basalt` → `aplite`) is handled on the host/emulator side (and future phone companion) so the C firmware runtime never requires an on-device ZIP parser.
  - Inside `pebble_compat`, a lightweight `pbpack` reader indexes the 256-entry resource table and serves `resource_get_handle`, `resource_size`, `resource_load`, and `resource_load_byte_range`, plus bitmap (`PBI`/PNG) and font decoders.
- **Event Loop, Timers & Input Mapping:**
  - Implements a cooperative `app_event_loop` driven by `pebble_hal` monotonic time and event polling: dispatches `tick_timer_service` (`SECOND_UNIT`, `MINUTE_UNIT`, etc.), `app_timer` one-shot timers, `click_config` button events, and `accel_tap_service` callbacks, then flushes dirty layers to `canvas`.
  - Maps Halo inputs consistently with `Halo-Iris`: Button Single-Click → `BUTTON_ID_SELECT`, Button Double-Click or Long-Press → `BUTTON_ID_BACK`, IMU Single-Tap → `BUTTON_ID_DOWN`, IMU Double-Tap → `BUTTON_ID_UP`.
- **Desktop ARM Runner & Official `HaloEmulator` Bridge:**
  - Compiles `pebble_compat` + Halo's verbatim `canvas` module + a thin POSIX ARM HAL adapter into a static 32-bit ARM Thumb-2 binary (`clang --target=arm-linux-gnueabihf -mthumb -static -fuse-ld=lld`) executed via `qemu-arm-static`.
  - A non-invasive Python integration layer in `Halo-Atlas` attaches `frame.pebble.run(path)` onto Brilliant Labs' official `HaloEmulator` Lua runtime (without modifying `brilliant_sdk`). It spawns the ARM runner subprocess, streams `256×256` RGB888 frames directly into `HaloEmulator`'s `DisplayBuffer` (`fmt=0`), and forwards `HaloEmulator` button clicks and IMU taps into the ARM runner over a lightweight stdin/stdout pipe.
- **Zephyr Firmware Adapter (Final Gate):**
  - A thin Zephyr HAL adapter maps `alloc_exec` onto Halo's existing 448 KB ITCM heap (`HALO_MEM_REGION_EXTERNAL` at `0x00010000`, clearing `MPU_RBAR_XN_Msk` if needed), `flush_canvas` onto Halo's `CDC200` framebuffer, `read_resource` onto LittleFS (`/lfs/pebble/`), and timers/events onto Zephyr `k_timer`/`k_msgq`.

## Testing Decisions

- **What Makes a Good Test:** Tests verify observable external behavior at high-level public seams — never internal C struct layouts or private helper functions. Every test feeds a real, unmodified `.pbw` binary (or deterministic fixture) into the public runtime interface and asserts on real rendered framebuffer pixels, clean lifecycle transitions, input responses, or syscall trace logs.
- **Primary Testing Seam — Official `HaloEmulator` + ARM Runner (`frame.pebble.run` / `get_framebuffer` / `inject_*`):**
  - Launches real `.pbw` binaries (starting with `test-app.pbw` *"For Your Friends"* and a real interactive multi-screen Rebble Store `.pbw`) through `HaloEmulator` backed by the compiled ARM Thumb-2 C runner under `qemu-arm-static`.
  - Verifies:
    1. Binary loading, relocation, and `main()` entry with syscall trace logging (including graceful handling of unimplemented syscalls and malformed binaries).
    2. Non-blank, expected watchface rendering in `HaloEmulator.get_framebuffer()` (`256×256` RGB888) and visual pixel changes as simulated clock ticks advance.
    3. Interactive screen/state changes in `HaloEmulator.get_framebuffer()` in response to `inject_button_single()`, `inject_button_double()`, and `inject_imu_tap("single" | "double")`.
- **Secondary Testing Seam — Zephyr Module Build & Cortex-M55 QEMU Gate:**
  - Compiles the exact same OS-agnostic `pebble_compat` C core against the Zephyr HAL adapter (`west build` for `-b halo` and/or Zephyr's `mps3/corstone300/an547` Cortex-M55 QEMU target) and verifies clean compilation, linking, and `.pbw` execution under Zephyr RTOS + ARMv8.1-M MPU.
- **Prior Art:** Follows the headless framebuffer-snapshot verification pattern established in `Halo-Iris` (capturing `HaloEmulator.get_framebuffer()` PNGs and asserting non-blank/changed regions across simulated button and tap events) and the offline host C harness pattern used in `halo-firmware` (compiling firmware C modules on the host for fast deterministic iteration before flashing).

## Out of Scope

- **Phone-Side PebbleKit JS (`pebble-js-app.js`) & BLE `AppMessage` Bridge:** Apps that require internet access or phone-side JavaScript (such as weather apps) will stub `app_message_*` safely in this spec; building the phone companion JS bridge is deferred to a follow-up spec.
- **On-Device `.pbw` ZIP Extraction:** `.pbw` archives are unpacked before passing `pebble-app.bin` and `app_resources.pbpack` to the C runtime.
- **Physical Halo Glasses Flashing & On-Device BLE Testing:** Flashing `zephyr.signed.bin` over BLE OTA and validating physical `CDC200` / `BMA580` hardware wait until physical glasses arrive.
- **Pebble Background Workers (`pebble-worker.bin`):** Only foreground watchfaces and watchapps (`pebble-app.bin`) are targeted.
- **Implementing All 663 PebbleOS APIs Up Front:** Only the core APIs needed for real watchfaces and interactive apps are implemented; remaining APIs remain safe logging stubs until driven by real apps.
- **Vendoring Full `PebbleOS` Kernel / FreeRTOS Internals:** Clean-room shims over Halo's canvas are preferred; selective standalone parsers (such as Pebble's `.pfo` font format) may be adapted only if needed.

## Further Notes

- Detailed primary-source hardware, memory map, ITCM (`0x00010000`), `libpebble.a` disassembly, and `HaloEmulator` integration findings are tracked in `FINDINGS.md` at the repo root and should be kept up to date as implementation uncovers new ABI or firmware details.
- When physical Halo glasses arrive, Halo's MCUboot one-shot test-boot / auto-revert OTA mechanism ensures firmware builds incorporating `pebble_compat` can be tested safely without bricking the device.
- Future enhancements after this spec include: BLE `AppMessage` + PebbleKit JS phone companion integration, Rebble App Store direct-download UI, Alif `Dave2D` hardware-accelerated blits, and optional SFXR audio cues mapped to `vibes_*` calls.
