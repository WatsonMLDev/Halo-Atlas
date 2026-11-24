# Halo-Atlas — Native PebbleOS App Execution on Brilliant Labs Halo

> **Halo-Atlas** *(Path 2 — Native Bare-Metal / Firmware Compatibility Layer)* is the yin/yang counterpart to **[Halo-Iris](../Halo-Iris)** *(Path 1 — Host-Side QEMU Emulation & BLE Framebuffer Streaming)*.
>
> **Why "Atlas"?** In mythology, Atlas carries the world on his own shoulders (Halo executing Pebble apps **natively on-device** with zero phone-side rendering). In cartography, an atlas is a book of maps — and this project is about **mapping** Pebble's ARM binary ABI, jump table, graphics engine, and event loop directly onto Halo's ARM Cortex-M55 + Zephyr RTOS hardware.

---

## 1. The Core Goal & Constraints

**Objective:** Run **raw, unmodified Pebble `.pbw` binaries** (compiled ARM Thumb-2 `pebble-app.bin` + `app_resources.pbpack`) **natively on the Brilliant Labs Halo smart glasses**, without CPU emulation and without streaming pixels over Bluetooth LE.

* **Preferred Path:** Binary-level compatibility (loading `.pbw` / `pebble-app.bin` directly on Halo's Cortex-M55 and resolving PebbleOS syscall jump-table entries to a Zephyr/Halo compatibility layer).
* **Fallback / Secondary Path:** Source-level compatibility (compiling open-source Pebble C apps against a Halo `<pebble.h>` shim), or static binary translation / porting tools if dynamic jump-table loading hits hardware constraints.
* **Non-Goal:** Emulating a Pebble CPU on the phone and streaming bitmaps over BLE (that is already solved in [`../Halo-Iris`](../Halo-Iris)).

---

## 2. Why This Is Surprisingly Feasible: Hardware & Architecture Comparison

Both Pebble watches and Brilliant Labs Halo are **ARM Cortex-M microcontrollers** with **circular displays** — and Halo's silicon is a massive superset of Pebble's:

| Dimension | Pebble Watch (`chalk` / `gabbro` Round 2) | Brilliant Labs Halo | Compatibility Notes |
|-----------|-------------------------------------------|---------------------|---------------------|
| **CPU Architecture** | ARM Cortex-M3 / M4 (Thumb-2, little-endian) | **ARM Cortex-M55** (ARMv8.1-M, Thumb-2, little-endian) | **Native ISA compatibility!** Cortex-M3/M4 Thumb-2 instructions execute natively on Cortex-M55 without translation! |
| **RAM** | ~64 KB app heap (`pebble-app.bin` ≤ 64 KB) | **1.5 MB internal SRAM + 7.5 MB external SRAM** (`halo_malloc`) | Halo has **~140× more RAM** than a Pebble app needs. Loading a 64 KB `.pbw` binary + resources into SRAM is trivial. |
| **RTOS / Firmware** | PebbleOS (FreeRTOS-based; modern Core Devices firmware also uses Zephyr/MCUboot) | **Zephyr RTOS + MCUboot** (`../halo-firmware`, self-buildable & BLE OTA-flashable with auto-revert!) | Owners can safely build & OTA-flash custom Halo firmware (`../halo-firmware/README.md`). |
| **Display** | `180×180` (`chalk`) or `260×260` (`gabbro`), 64-color RGB222 circular | **`256×256` RGB888** MIPI-DSI (`CDC200` framebuffer + `modules/canvas/canvas.c`), circular lens aperture | `gabbro` (`260×260`) is a near-1:1 native pixel match (crop 2px per edge). `chalk` (`180×180`) centers cleanly or scales. |
| **Storage** | SPI NOR Flash (PFS) | LittleFS (`/lfs/`) | `.pbw` files or unpacked `pebble-app.bin` + `app_resources.pbpack` can be uploaded over BLE and stored in `/lfs/pebble/`. |
| **Inputs** | 4 buttons (`Back`, `Up`, `Select`, `Down`), Accelerometer/Tap, Magnetometer | 1 physical button (`single`/`double`/`long`), BMA580 IMU (`single`/`double`/`triple` temple tap + raw accel/mag), Microphone, Speaker | Button + IMU tap gestures map directly to Pebble's 4 buttons & `accel_tap_service`. |
| **Phone Companion** | Pebble Protocol over BLE (`AppMessage`, `PebbleKit JS`, `BlobDB`) | Halo BLE Lua Service (`7A230001-...`) + ANCS (iOS) | Can either bridge `AppMessage` over Halo's existing BLE data channel or expose a standard Pebble BLE GATT service in Zephyr. |

---

## 3. How Raw Pebble `.pbw` Binaries Actually Work (The Key Insight!)

A `.pbw` file is simply a standard ZIP archive containing:
* `<platform>/pebble-app.bin` — Position-Independent Executable (PIE) ARM Thumb-2 binary
* `<platform>/app_resources.pbpack` — Binary resource pack (fonts, PNGs/PBI bitmaps, PDC vector graphics, raw data)
* `pebble-js-app.js` — Optional phone-side PebbleKit JS script (for web/weather APIs)

### Why Raw `pebble-app.bin` Can Run Natively on Halo
Pebble apps **do not** link against PebbleOS statically, and they **do not** use hardware `SVC` supervisor calls. Instead, they use a **relocatable binary + function pointer jump table**:

1. **Header (`PebbleProcessInfo`, 130 bytes / `0x82`):**
   * Magic `"PBLAPP"`, SDK version, app name, UUID, flags.
   * `load_size` (offset `0x0e`, `uint16`): bytes of code + initialized data to load into RAM.
   * `offset` (offset `0x10`, `uint32`): relative entry point (`main()`).
   * `sym_table_addr` / `jump_table_addr` (offset `0x5c`, `uint32`): offset inside the loaded binary where the loader must write the **pointer to the OS syscall jump table** (`pbl_table_addr`)!
   * `num_reloc_entries` (offset `0x64`, `uint32`): count of 32-bit offsets appended right after `load_size`.
   * `virtual_size` (offset `0x80`, `uint16`): total RAM footprint (`.text` + `.rodata` + `.data` + `.bss` + `.got`).

2. **Loading & Relocating on Cortex-M55 (Only ~30 lines of C!):**
   * Allocate `virtual_size` bytes in executable SRAM (on Cortex-M55, ensure the memory region is not marked XN/MPU-non-executable and flush I-Cache / D-Cache via `SCB_CleanDCache` + `SCB_InvalidateICache`!).
   * Copy `load_size` bytes from `pebble-app.bin` into `base_addr`, zero-fill `.bss`.
   * For each 32-bit offset `reloc` in the relocation table: `*(uint32_t *)(base_addr + reloc) += (uint32_t)base_addr;`.
   * Write the address of **Halo-Atlas's syscall table** (`g_halo_pebble_syscall_table`) into `*(uint32_t *)(base_addr + jump_table_addr)`.
   * Call `((int (*)(void))(base_addr + entry_point | 1))()` (setting the Thumb bit `| 1`)!

3. **The Syscall Jump Table (`exported_symbols.json` / `pbl_table_addr`):**
   Every call a Pebble app makes to `<pebble.h>` (`window_create()`, `graphics_draw_text()`, `app_event_loop()`, `tick_timer_service_subscribe()`, etc.) is compiled as an indirect jump through an index in `pbl_table_addr[N]`.
   * The exact table indices are public and documented in **`coredevices/PebbleOS`** (and in the SDK's `libpebble.a` / `exported_symbols.json`!).
   * **Every single function in that table is just a C function we can implement on top of Halo's Zephyr firmware!**

---

## 4. Candidate Approaches to Discuss & Evaluate

When starting the design session for `Halo-Atlas`, here are the 4 main architectural paths to weigh against each other:

### Option 1: On-Device `.pbw` Binary Loader + PebbleOS Syscall Jump Table in `halo-firmware` *(Most Exciting / True Binary Compat)*
* **How it works:** Add a new Zephyr module (`modules/pebble_compat/` in `../halo-firmware`) with:
  1. A **PIE ELF/PBLAPP loader** that loads `pebble-app.bin` from `/lfs/` into executable SRAM, applies relocations, flushes I-cache, and patches `pbl_table_addr`.
  2. A **PebbleOS Jump Table** implementing the core `<pebble.h>` subsystems directly on Halo's hardware:
     * **Graphics (`GContext`, `GBitmap`, `GPath`, `Layer`, `TextLayer`, `BitmapLayer`):** Draws directly into Halo's `CDC200` RGB888 framebuffer via `modules/canvas/canvas.c` (converting Pebble's 6-bit `GColor8` `ARGB2222` to RGB888).
     * **Event Loop & Timers (`app_event_loop`, `tick_timer_service`, `app_timer_register`):** Backed by Zephyr `k_msgq` + `k_timer` + RTC (`frame.time`).
     * **Resources (`app_resources.pbpack`, `GFont`, `GBitmap`, `GDrawCommand`):** Read directly from `/lfs/` or external SRAM.
     * **Inputs (`window_set_click_config_provider`, `accel_tap_service`):** Fed from Halo's button & BMA580 IMU drivers.
     * **Optional Lua Binding:** Expose `halo.pebble.run("/lfs/app.pbw")` so you can launch/stop native Pebble apps right from Lua or over BLE!
* **Pros:** Runs **raw, unmodified `.pbw` binaries** at **60 FPS** directly on Halo's Cortex-M55 with zero phone rendering overhead and zero BLE display traffic.
* **Cons / Challenges:**
  * **Cortex-M55 Memory Protection / MPU & Cache:** Must verify executable SRAM region permissions (ITCM / SRAM0 / SRAM1) and I-cache invalidation on Alif Balletto B1.
  * **ABI Details:** Must match Pebble's ARM EABI (`-fshort-enums`, struct packing, `GColor8` layout, `GRect`/`GPoint`/`GSize` passed by value in registers).
  * **Jump Table Coverage:** PebbleOS exports ~400–500 symbols. We can stub unimplemented ones with logging (`LOG_WRN("unimplemented syscall #%d", idx)`) and implement the top ~60–80 symbols first (enough to run most watchfaces and simple apps!).
  * **PebbleKit JS (`AppMessage`):** Apps like `TouchyWeather` still need a phone companion to run `pebble-js-app.js` and relay `AppMessage` dictionaries over BLE.

### Option 2: Port Core `PebbleOS` Graphics/UI Subsystems Directly Into `halo-firmware`
* **How it works:** Instead of re-implementing `Layer`, `TextLayer`, `ScrollLayer`, `MenuLayer`, `GPath`, `PBI`/`PNG`/`PDC` decoders, and Pebble's font renderer from scratch, vendor/adapt the Apache-2.0 licensed C implementations directly from **`coredevices/PebbleOS`** (`src/fw/applib/`) and wire only the bottom framebuffer flush + input/timer HAL to Halo's Zephyr drivers.
* **Pros:** Pixel-perfect PebbleOS rendering (`ScrollLayer`, `MenuLayer`, `GFont`, anti-aliased `GPath`, `PDC` animations) without reinventing Pebble's UI toolkit.
* **Cons:** Extracting `src/fw/applib/` cleanly from PebbleOS's internal kernel headers takes some untangling.

### Option 3: Static Binary Translator / Porting Script (Host-Side `.pbw` → Native Halo Module or Lua)
* **How it works:** A Python/LLVM tool on desktop inspects a `.pbw`, extracts resources/bitmaps/fonts, and either:
  * Re-links / patches the binary for Halo's memory map, OR
  * Converts simple watchfaces / assets into a Halo firmware module or Lua bundle.
* **Pros:** Moves complex parsing (`pbpack`, PNG/font rasterization) off-device.
* **Cons:** General-purpose ARM-to-Lua/C decompilation is hard; binary patching still needs the on-device syscall table from Option 1.

### Option 4: Source-Level `<pebble.h>` Compatibility Library (Fallback)
* **How it works:** Compile open-source Pebble C apps directly with Zephyr's `arm-none-eabi-gcc` against a custom `<pebble.h>` header that maps to Halo's `canvas.c` and Zephyr primitives.
* **Pros:** Zero binary loader / MPU / jump-table ABI headaches; great stepping stone to test our `<pebble.h>` implementation before wiring up the dynamic `.pbw` loader!
* **Cons:** Requires C source code and reflashing firmware per app (unless paired with Option 1's dynamic loader).

---

## 5. Key Local Reference Repositories Already in This Workspace

You already have everything needed right next door in `/home/watsonc/sudo_desktop/Halo/`:

1. **`../halo-firmware/`** — Full Brilliant Labs Halo Zephyr RTOS firmware:
   * `modules/canvas/canvas.c` & `include/canvas.h` — Halo's RGB888 framebuffer drawing primitives (`256×256` logical, `PHYS_WIDTH` physical).
   * `modules/halo/src/lua_display.c` — How Halo initializes `CDC200`, MIPI-DSI, pan calibration, and power-save resume.
   * `modules/halo/src/lua_runtime.c` — How Halo allocates from Internal SRAM (1.5 MB) vs. External SRAM (7.5 MB) via `halo_malloc`.
   * `applications/halo/SETUP.md` & `FLASHING.md` — Building with `west` (or the CI Docker container) and safe BLE OTA flashing with MCUboot auto-revert.
2. **`../Halo-Iris/`** — Path 1 (working PoC):
   * `pebble-qemu-wasm/tools/sdkpack-gabbro.zip` & `sdkpack-chalk.zip` — Contain the official Pebble SDK headers (`pebble.h`, `pebble_process_info.h`, `pebble_app.ld`) and **`libpebble.a`** (which encodes the exact jump-table indices!).
   * `pebble-qemu-wasm/appbuild/elfpack.js` — Concise, readable reference implementation of `PebbleProcessInfo` header layout, relocation table format, and `pbpack` resource format.
   * `pebble-qemu-wasm/web/test-app.pbw` — A real circular `chalk` `.pbw` watchface (*"For Your Friends"*) ready to test our loader against.
   * `TouchyWeather/` — A complex real-world `gabbro`/`chalk` C app + PebbleKit JS.

---

## 6. Suggested Questions & Milestones for the Next Session

1. **Inspect the Jump Table & ABI First:**
   * Extract `libpebble.a` from `../Halo-Iris/pebble-qemu-wasm/tools/sdkpack-gabbro.zip` (or `chalk`) and dump the exact symbol-to-index jump table mapping.
   * Check what syscalls a minimal circular `.pbw` (like `test-app.pbw` or a simple "Hello World" watchface) actually calls.
2. **Verify Executable SRAM on Alif Balletto B1 (Cortex-M55):**
   * Check `../halo-firmware/boards/arm/halo/` DTS and memory map (`ITCM`, `DTCM`, `SRAM0`, `SRAM1`, MPU regions) to see where dynamically loaded Thumb-2 code can execute safely in Zephyr.
3. **Desktop / Zephyr `native_sim` Prototype First:**
   * Before flashing physical glasses, can we build the `<pebble.h>` shim + `.pbw` loader (or at least the graphics/UI layer) and test it locally (or on a Halo dev-kit / OTA test-boot)?
4. **Decide the UI Layer Strategy:**
   * Write a minimal clean-room `<pebble.h>` implementation (start with `Window`, `Layer`, `TextLayer`, `GContext`, `TickTimer`, `ClickConfig`) vs. vendoring `coredevices/PebbleOS/src/fw/applib/`.
5. **Decide the `AppMessage` / PebbleKit JS Strategy:**
   * For apps that need internet access (like `TouchyWeather`), how should `AppMessage` travel between Halo and the phone (over Halo's existing BLE Lua service, or by adding a Pebble GATT service)?
