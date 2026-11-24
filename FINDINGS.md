# Halo-Atlas — Technical Feasibility & Research Findings

> Running log of verified primary-source findings from `../halo-firmware` and `../Halo-Iris` as we design **Halo-Atlas** (running unmodified Pebble Store `.pbw` binaries natively on Brilliant Labs Halo).

---

## 1. Halo Firmware Memory, ITCM & MPU Findings

Inspected against `../halo-firmware`:

1. **448 KB of Cortex-M55 ITCM Is Already Managed as a Heap:**
   * In `applications/halo/prj.conf` (`L72–78`), `modules/halo/src/mem_manager.c` (`L90–110`), and `modules/halo/src/audio_aec.c` (`L1581–1601`), the region named `HALO_MEM_REGION_EXTERNAL` (`CONFIG_HALO_MEM_EXTERNAL_SRAM_ADDR=0x00000000`) is physically **Cortex-M55 ITCM (Instruction Tightly Coupled Memory)**:
     * `0x00000000 – 0x0000FFFF` (`64 KB`): Reserved for `null_guard` (256 B) + `audio_aec.c` FDAF/suppressor state.
     * `0x00010000 – 0x0007FFFF` (**`458,752` bytes / `448 KB`**): Managed by `sys_heap` via `halo_malloc(size, HALO_MEM_REGION_EXTERNAL)`.
   * **Why ITCM is ideal for `.pbw` execution:**
     * `0x00000000` is ARM's architectural `Code` region and sits directly on the Cortex-M55 instruction bus (zero wait states).
     * On Cortex-M55, TCM accesses bypass L1 instruction/data caches, avoiding I-Cache/D-Cache coherency pitfalls when loading dynamic code (only `__DSB(); __ISB();` barriers are needed before branching).
2. **MPU Permissions Are Reconfigurable at Runtime:**
   * `modules/halo/Kconfig` (`L94–112`) and `modules/halo/src/mram_guard.c` (`L56–118`) confirm Halo runs in privileged thread mode (`!CONFIG_USERSPACE`, `!CONFIG_MPU_STACK_GUARD`) with unused hardware ARMv8-M MPU slots.
   * If the SoC's default MPU configuration marks unused ITCM `XN` (Execute-Never), clearing `MPU_RBAR_XN_Msk` on the `.pbw` arena takes ~10 lines of C modeled directly on `mram_guard.c`.
3. **Display Framebuffer & `canvas.c` Layout:**
   * Halo uses a `256×256` RGB888 framebuffer (`CDC200` layer 0, retrieved via `cdc200_get_framebuffer()` in `modules/halo/src/lua_display.c:367–371`).
   * **Physical memory orientation note:** `modules/canvas/canvas.c` (`L140–148`, `L551–561`) stores pixels rotated 90° with BGR byte ordering (`uint8_t *pixel = canvas->buffer[x][PHYS_WIDTH - y - 1]; pixel[2]=r; pixel[1]=g; pixel[0]=b;`). Routing our Pebble graphics shim through `canvas.c` (or matching its coordinate transform) handles panel orientation automatically.
   * `west.yml` (`L104–107`) also includes Alif's **Dave2D** hardware 2D GPU driver (`alif_dave2d-driver`) if hardware-accelerated blits or anti-aliased paths are ever needed.

---

## 2. Pebble `.pbw` Binary & Jump-Table ABI Findings

Inspected against `../Halo-Iris/pebble-qemu-wasm/tools/sdkpack-gabbro.zip` (`sdk/lib/libpebble.a`), `appbuild/appbuilder.js`, `appbuild/elfpack.js`, and `web/test-app.pbw`:

1. **Exact Jump-Table Size & Dispatcher Mechanism:**
   * `libpebble.a` contains **663 PebbleOS syscall shims** plus two helper symbols (`jump_to_pbl_function` and `pbl_table_addr`).
   * Every `.pbw` links a single **16-byte Thumb-2 trampoline** right after the `PebbleProcessInfo` header + GNU build-id:
     ```asm
     jump_to_pbl_function:
       adr r3, pbl_table_addr    ; reads pointer patched by loader at offset 0x5c
       ldr r0, [r3]              ; r0 = base address of Halo's syscall table
       add r0, r1                ; r0 = &syscall_table[byte_offset / 4]
       ldr r2, [r0]              ; r2 = target Halo C function pointer
       mov r12, r2
       pop {r0, r1, r2, r3}      ; restores caller's original C arguments!
       bx  r12                   ; tail-calls Halo C function directly (returns via lr)
     ```
   * Each PebbleOS API call inside the app binary is a 10-byte stub:
     ```asm
     push  {r0, r1, r2, r3}
     mov.w r1, #<byte_offset>
     b.w   jump_to_pbl_function
     ```
   * **Implication:** Zero instruction emulation or binary patching is needed per call. Unmodified `.pbw` machine code tail-calls directly into Halo C functions at native Cortex-M55 speed.
2. **Real-World `.pbw` Footprint (`test-app.pbw`):**
   * `chalk/pebble-app.bin` (*"For Your Friends"* circular watchface, SDK `5.72`):
     * `load_size`: **`4,292` bytes** (~4.2 KB)
     * `virtual_size`: **`4,676` bytes** (~4.6 KB total `.text` + `.data` + `.bss`)
     * `entry_point`: `0x9e0`
     * `pbl_table_addr`: `0xb8`
     * `num_reloc_entries`: **`2`** (two 32-bit words to relocate by `+base_addr`)
3. **Critical ABI Mismatch Caught Early (Hard-Float vs. Soft-Float & Short Enums):**
   * **Float ABI:** `halo-firmware` compiles with `CONFIG_FPU=y` and `CONFIG_FP_HARDABI=y` (`boards/arm/halo/halo_defconfig:12–13`, passing floats in `s0–s15`), whereas Pebble `.pbw` binaries are compiled for Cortex-M3 soft-float (`-mcpu=cortex-m3 -mthumb`, passing floats in `r0–r3`).
     * *Fix:* ~98% of `<pebble.h>` uses integer/fixed-point types (`int32_t`, `int16_t`, pointers) which use `r0–r3` identically in both ABIs. Any syscall taking or returning `float`/`double` must be annotated with `__attribute__((pcs("aapcs")))` in Halo's C shim so GCC expects soft-float register arguments.
   * **Enum Packing:** Pebble SDK compiles apps with `-fshort-enums` (`appbuilder.js:24`), packing small enums into `uint8_t`/`int8_t` (affecting structs like `Tuplet`).
     * *Fix:* Either compile the `pebble_compat` shim files with `-fshort-enums` (isolated from Zephyr headers) or define Pebble ABI struct enum fields using explicit `uint8_t` / `int8_t` widths.

---

## 3. Architectural Mental Model: "Proton + DXVK for PebbleOS"

To run closed-source `.pbw` apps directly from the Pebble/Rebble Store without recompiling C source:

* **CPU Layer (Native Execution, like x86 on Steam Deck):** Raw Thumb-2 `pebble-app.bin` runs directly on Cortex-M55 out of ITCM (`0x00010000`).
* **Wine / Proton Layer (`pbl_table_addr[663]`):**
  * Auto-generate all 663 jump-table slots from `libpebble.a` as safe logger/no-op stubs (`LOG_WRN("unimplemented syscall #%d: %s", idx, name)`) so apps never jump to `NULL` on unimplemented APIs (e.g., `vibes_*`, `health_*`).
  * Map core OS services (`app_event_loop`, `tick_timer_service`, `app_timer`, `click_config`, `accel_tap_service`, `resource_load`, `persist_*`) onto Zephyr primitives, LittleFS (`/lfs/`), and Halo's button + BMA580 IMU tap events.
* **DXVK + Gamescope Layer (`GContext` + Shadow Buffer → `canvas.c`):**
  * Maintain a compact Pebble-format shadow framebuffer (`GColor8` `ARGB2222`, `32 KB` for `180×180` `chalk` or `67.6 KB` for `260×260` `gabbro`).
  * Both high-level `<pebble.h>` draw calls (`Layer`, `TextLayer`, `GBitmap`, `GPath`) *and* raw pixel-poking apps (`graphics_capture_frame_buffer()`) write to this buffer, which flushes to Halo's `256×256` RGB888 `canvas.c` buffer at the end of each render pass (auto-centering `180×180` `chalk` / `144×168` `basalt`/`aplite`, or cropping 2px off `260×260` `gabbro`).
* **Companion App Layer:** Downloads `.pbw` from the Rebble Store, unpacks `pebble-app.bin` + `app_resources.pbpack` before BLE transfer (avoiding on-device ZIP parsing), and runs `pebble-js-app.js` on the phone when `AppMessage` web data is needed.

---

## 4. Hardware-Free Testing Strategy (No Physical Glasses Required)

### Why `HaloEmulator` Alone Isn't Enough — and Why Skipping Straight to Docker Is a Trap
* **Verified in `../Halo-Iris/brilliant_sdk/python/packages/halo_emulator/pyproject.toml`:** `halo-emulator` (`2.3.0`) is a Python + `lupa` (x86_64 Lua) + NumPy + Pygame mock of the `frame.*` Lua API. Out of the box, it does **not** run Zephyr or emulate an ARM CPU.
* **Verified in `../halo-firmware/Dockerfile.ci` & `.github/CI.md` (`L7–17`, `L51`):**
  * `ghcr.io/brilliantlabsar/halo-firmware-ci` builds `FROM ghcr.io/zephyrproject-rtos/ci:v0.26.13` (which includes Zephyr SDK + `qemu-system-arm`), **but** the image is **>10 GB**, relies on `PRIVATE_REPO_TOKEN` while any `west.yml` dependency is private, runs headlessly with ~15–30s `west build` cycles, and **still cannot boot `-b halo`** in QEMU because QEMU lacks Alif's Secure Enclave (`SE`), `CDC200` MIPI-DSI, and BLE ROM.
  * Therefore, using the Docker CI container for *daily bring-up* gives slow feedback without actually proving display/sensor glue on physical glasses, whereas using it as the **final compile + Zephyr QEMU gate** gives full value with zero drag.

### Recommended Architecture Seam: Portable ARM C Core Plugged Directly Into Official `HaloEmulator`

Inspecting `halo_emulator/src/halo_emulator/display.py` (`L409–423`), `lua_runtime.py` (`L54–191`), and `cli.py` (`L28–63`) revealed a clean way to run **real ARM Thumb-2 `.pbw` binaries inside Brilliant Labs' official `HaloEmulator` window**:

1. **OS-Agnostic C Core Behind a Tiny `pebble_hal.h` (~6 Functions):**
   * Keep `pebble_loader.c`, `pebble_syscalls.c`, `pebble_gfx.c`, `pebble_pbpack.c`, and Halo's verbatim `modules/canvas/canvas.c` 100% free of Linux or Zephyr headers — talking only to `pebble_hal.h` (`alloc_exec`, `get_time`, `flush_canvas`, `poll_event`, `read_resource`, `log`).
2. **Primary Dev & Demo Target — `qemu-arm-static` Bridged Into Official `HaloEmulator`:**
   * Compile the C core + `pebble_hal_posix_arm.c` with `clang --target=arm-linux-gnueabihf -mthumb -static -fuse-ld=lld` and execute it under **`qemu-arm-static`** (`<0.1s` build/startup, matching Halo's hard-float host vs. `.pbw` soft-float ABI).
   * Extend `HaloEmulator`'s Lua runtime (non-invasively from `Halo-Atlas`, matching the future firmware's `frame.pebble.run("/lfs/app.pbw")` API):
     * `qemu-arm-static` streams `canvas.c`'s `256×256` RGB888 output straight into `HaloEmulator._display.bitmap(1, 1, 256, 0, 0, rgb888_bytes)` (`fmt=0` direct RGB888).
     * `HaloEmulator`'s built-in circular HUD Pygame window (`SPACE` = single click, `D` = double click, `L` = long press, `T` = tap, `2` = double tap, plus `.gif`/`.mp4` recording) forwards Halo button & IMU tap events directly into the running `.pbw` app!
3. **Final Firmware Verification Gate — Zephyr / Docker CI (`pebble_hal_zephyr.c`):**
   * Once real Pebble Store `.pbw` watchfaces & apps run interactively in `HaloEmulator`, drop the exact same C core into `halo-firmware/modules/pebble_compat/` with a ~60-line `pebble_hal_zephyr.c` (`halo_malloc` ITCM + `cdc200` + `k_msgq`) and verify compilation/execution on Zephyr Cortex-M55 QEMU (`mps3/corstone300/an547`) and `-b halo`.
