# Halo-Atlas

**Native PebbleOS binary compatibility runtime for Brilliant Labs Halo.**

Halo-Atlas loads and executes **raw, unmodified Pebble `.pbw` binaries** (compiled ARM Thumb-2 `pebble-app.bin` + `app_resources.pbpack`) natively on Halo's **ARM Cortex-M55** and **Zephyr RTOS** — with zero CPU emulation and zero phone-side rendering.

For desktop development and visual verification, the exact same OS-agnostic C runtime compiles as a standalone ARM Thumb-2 runner and integrates directly into Brilliant Labs' official **`HaloEmulator`** (including circular HUD rendering via Halo's verbatim `canvas.c`, interactive Halo button + temple-tap controls, and `.gif`/`.mp4` recording).

---

## How It Works

Pebble `.pbw` binaries are position-independent ARM Thumb-2 executables (`PBLAPP`) that call PebbleOS through an indirect function-pointer jump table (`pbl_table_addr`) rather than hardware `SVC` traps. Halo-Atlas implements:

1. **PBLAPP PIE Loader** (`runtime/core/src/pebble_loader.c`): Validates `PebbleProcessInfo` (struct v8.x–v16.x), copies code/data into executable RAM, zeroes `.bss`, applies 32-bit relocations, patches `pbl_table_addr`, flushes caches, and enters `main()` in Thumb mode.
2. **Generated 663-Symbol PebbleOS Jump Table** (`tools/gen_syscall_table.py`): Parses `third_party/pebble_sdk/libpebble.a` to generate the exact 677-slot syscall dispatch table with safe logging stubs for unimplemented entries.
3. **OS-Agnostic PebbleOS Core** (`runtime/core/`): Zero Linux, POSIX, or Zephyr headers — compiles cleanly with `-ffreestanding -nostdinc` for both ARMv7VE (`qemu-arm-static`) and Cortex-M55 (`-mcpu=cortex-m55 -mfloat-abi=hard`):
   - **Graphics & UI:** `Window`, `Layer`, `TextLayer`, `BitmapLayer`, `GContext`, `GPath`, `GColor8` (`ARGB2222` → Halo RGB888), direct framebuffer capture, `gabbro` (260×260 → 2px crop) & `chalk` (180×180 centered) mapping.
   - **Resources & Fonts:** `.pbpack` resource reader, PBI & PNG (zlib/DEFLATE + filtering + palette/transparency) decoder, authentic Pebble PFO v2/v3 RLE4 custom fonts + bundled fallback font.
   - **Events, Timers, & Persistence:** `app_event_loop`, `tick_timer_service`, `app_timer_*`, click config (`SINGLE`/`LONG`/`MULTI` + `BACK`), `accel_tap_service`, and per-app `persist_*` key-value storage.
4. **Thin Platform HAL (`pebble_hal_t`)** (`runtime/hal/`):
   - **`runtime/hal/posix_arm/`**: Linux ARMv7/QEMU user-mode runner wired to Halo's verbatim `canvas.c` (`CONFIG_CANVAS_ROTATE_90`) and bridged to `HaloEmulator` via `frame.pebble.run()`.
   - **`runtime/hal/zephyr/`**: Drop-in Zephyr RTOS module (`ZEPHYR_EXTRA_MODULES`) backed by Halo's 448 KB ITCM heap (`HALO_MEM_REGION_EXTERNAL`), ARMv8.1-M MPU `XN`/`PXN` region splitting & full slot restoration, `CDC200` + `canvas` flushing, LittleFS resources/atomic persistence, and `k_timer`/`k_msgq`/`k_sem` event delivery.

---

## Repository Layout

```text
Halo-Atlas/
├── GLOSSARY.md                 # Canonical domain vocabulary
├── Makefile                    # Builds ARM runner, runs tests & Zephyr gates
├── pyproject.toml              # Python package & CLI entry point (halo-atlas)
├── zephyr/
│   └── module.yml              # Zephyr ZEPHYR_EXTRA_MODULES descriptor
├── runtime/
│   ├── core/                   # OS-agnostic freestanding C runtime
│   │   ├── include/            # Public & internal headers (pebble_hal.h, pebble_compat.h, ...)
│   │   └── src/                # Loader, graphics, bitmaps, PFO fonts, timers, UI, persist
│   └── hal/
│       ├── posix_arm/          # Host ARMv7 / QEMU user-mode HAL & CLI runner
│       └── zephyr/             # Zephyr RTOS Cortex-M55 HAL + mps3_an547 QEMU gate app
├── python/
│   └── halo_atlas/             # PBW unpacker, ARM runner bridge, HaloEmulator integration, CLI
├── tools/
│   ├── gen_syscall_table.py    # Extracts PebbleOS jump-table indices from libpebble.a
│   ├── gen_embedded_pbw.py     # Embeds an unpacked .pbw into C header for QEMU gate
│   └── run_zephyr_gate.sh      # Runs Cortex-M55 QEMU + real halo-firmware (-b halo) gates
├── tests/                      # Automated test suite + .pbw test fixtures
├── third_party/
│   ├── halo_canvas/            # Verbatim Brilliant Labs Halo canvas.c/h
│   └── pebble_sdk/             # Pebble SDK libpebble.a & headers
└── docs/
    ├── FINDINGS.md             # Hardware, ABI, MPU, memory, & firmware deep-dive notes
    ├── adr/                    # Architecture Decision Records
    └── agents/                 # Repo workflow & issue-tracker conventions
```

---

## Quickstart (Desktop / `HaloEmulator`)

### Prerequisites
- `clang` + `lld` (ARM Thumb-2 cross-compilation)
- `qemu-arm-static` (runs the compiled ARMv7 runner natively on x86_64/arm64 hosts)
- Python 3.10+ & [`uv`](https://docs.astral.sh/uv/) (`uv sync` installs `halo-atlas` and `halo-emulator[video]`)

### Build & Run a Pebble `.pbw` App

```bash
# 1. Clone brilliant_sdk locally so uv can install the editable halo-emulator package
git clone --depth 1 https://github.com/brilliantlabsAR/brilliant_sdk.git

# 2. Install Python dependencies (halo-atlas + HaloEmulator)
uv sync

# 3. Build the freestanding ARM runner once (produces build/pebble-runner)
make runner

# 4. Run an unmodified .pbw interactively inside HaloEmulator's circular HUD window
uv run halo-atlas tests/fixtures/test-app.pbw

# Optional: run headless and record a circular-bezel GIF or MP4
uv run halo-atlas tests/fixtures/test-app.pbw \
    --headless --time-epoch 1767236400 --tick-step 900 --max-frames 8 \
    --record watchface.gif --fps 4

# Optional: trace every called / unimplemented PebbleOS syscall in any .pbw
uv run halo-atlas path/to/any-app.pbw --trace
```

### Halo-to-Pebble Input Mapping (in `HaloEmulator`)

| Halo Input (`HaloEmulator` Key) | Pebble Event |
|----------------------------------|--------------|
| Single button click (`SPACE`) | `BUTTON_ID_SELECT` |
| Double button click (`D`) | `BUTTON_ID_BACK` |
| Long button press (`L`) | `BUTTON_ID_SELECT` (long click) |
| Single temple tap (`T`) | `BUTTON_ID_DOWN` + `accel_tap_service` |
| Double temple tap (`2`) | `BUTTON_ID_UP` + `accel_tap_service` |

---

## Zephyr RTOS & Cortex-M55 Firmware Integration

`Halo-Atlas` is a drop-in Zephyr module (`zephyr/module.yml`) requiring **zero edits** to `halo-firmware`:

```bash
west build -b halo applications/halo -- \
    -DZEPHYR_EXTRA_MODULES=/path/to/Halo-Atlas \
    -DCONFIG_PEBBLE_COMPAT=y
```

### Verified Firmware & Cortex-M55 QEMU Gate Results

Run both Docker gates via `make zephyr-gate` (uses `ghcr.io/brilliantlabsar/halo-firmware-ci:latest`):

| Gate | Target | Verified Result |
|------|--------|-----------------|
| **Cortex-M55 QEMU** | `mps3_an547` (`CONFIG_FP_HARDABI=y`, `CONFIG_ARM_MPU=y`) | Loads & runs unmodified `test-app.pbw` from ITCM (`0x10070000`, 851 syscalls, 2 frames, 0 extra MPU slots) and DTCM (`0x30000120`, clears `XN`/`PXN` via 2 split slots, wakes via `k_timer`/`k_sem`, exits via ISR `k_msgq`, restores slots `14 -> 12 -> 14`). Peak stack: **3,176 / 8,192 B**. |
| **`halo-firmware`** | `-b halo` (`arm-zephyr-eabi-gcc 12.2.0`, `CONFIG_FP_HARDABI=y`) | Links cleanly with **0 warnings**: `+71.6 KB` Flash (`32.78%`), `+18.2 KB` static RAM (`99.42%`; runtime buffers allocate transiently via `halo_malloc`). |

---

## Running Tests

```bash
# Full local test suite (PBW unpacker, ARM runner, HaloEmulator watchfaces,
# interactive buttons/taps/timers/persist, freestanding & Cortex-M55 compilation)
make test

# Include the Docker Cortex-M55 QEMU + halo-firmware (-b halo) integration gate
RUN_ZEPHYR_DOCKER_GATE=1 make test
```

---

## Scope & Roadmap

- **Project Scope:** `Halo-Atlas` focuses strictly on the **PebbleOS C engine, Zephyr RTOS module, and local `HaloEmulator` developer harness** — not a standalone mobile app.
- **Universal Companion Seam ([ADR-0001](docs/adr/0001-pebble-protocol-hal-seam.md)):** Upcoming `AppMessage` and PebbleKit JS support ([`#1`](https://github.com/WatsonMLDev/Halo-Atlas/issues/1), [`#2`](https://github.com/WatsonMLDev/Halo-Atlas/issues/2)) speaks standard 4-byte-framed **Pebble Protocol** packets across `pebble_hal_t`. That single wire boundary lets `uv run halo-atlas` run `pebble-js-app.js` locally today while allowing downstream hardware integrations to either pair directly with an unmodified **Pebble Mobile App / Gadgetbridge** (via BLE Reversed PPoGATT) or tunnel companion traffic inside a custom **`brilliant_sdk` / Noa** app without changing `runtime/core/`.
- **Syscall Coverage:** **196 / 663** PebbleOS jump-table symbols implemented (**29.6%**) — enough to run unmodified watchfaces and custom-drawn interactive Watchapps with bundled `.pbpack` resources, PBI/PNG bitmaps, PFO fonts, buttons, temple taps, timers, and persistent storage on both `HaloEmulator` and Cortex-M55 Zephyr QEMU.
- **Roadmap:** Upcoming work and milestones are tracked in **[GitHub Issues](https://github.com/WatsonMLDev/Halo-Atlas/issues)**.

See **[GLOSSARY.md](GLOSSARY.md)** for domain terminology and **[docs/FINDINGS.md](docs/FINDINGS.md)** for hardware, ABI, ARMv8.1-M MPU, ITCM memory map, and Zephyr firmware measurements.
