# 05: Zephyr RTOS HAL Adapter & Cortex-M55 Firmware Build Gate

**What to build:** Prove that the exact same OS-agnostic `pebble_compat` C core verified in `HaloEmulator` is firmware-ready for Brilliant Labs Halo hardware. Implement the thin Zephyr `pebble_hal` adapter (mapping executable allocation onto Halo's 448 KB ITCM heap with MPU executability verification, `CDC200` canvas flushing, LittleFS `/lfs/pebble/` resource reads, and Zephyr timers/queues) and verify clean compilation, linking, and real `.pbw` execution on Zephyr's Cortex-M55 QEMU target (`mps3/corstone300/an547`) and `-b halo` firmware build.

**Blocked by:** 03: Bundled `.pbpack` Resources, Bitmaps, Fonts & Direct Framebuffer Access, 04: Interactive Pebble Store Apps via Halo Buttons, Temple Taps, Timers & Persistence

**Status:** ready-for-agent

- [ ] A thin Zephyr `pebble_hal` adapter connects `pebble_compat` to Halo's 448 KB ITCM heap (`HALO_MEM_REGION_EXTERNAL`), ensures the executable arena is not marked `XN` in the ARMv8.1-M MPU, flushes frames to `CDC200` via Halo's `canvas` module, reads unpacked resources from LittleFS, and drives timers/events via Zephyr `k_timer`/`k_msgq`.
- [ ] The `pebble_compat` C core compiles and links cleanly inside `halo-firmware` (Zephyr RTOS, `arm-zephyr-eabi-gcc`, `CONFIG_FP_HARDABI=y`) with zero unresolved symbols or ABI warnings.
- [ ] Booting the Zephyr module on Zephyr's Cortex-M55 QEMU board (`mps3/corstone300/an547`) loads and executes an unmodified `.pbw` binary from ITCM without MPU or cache faults and produces verified output.
- [ ] Any hardware-specific findings (MPU slot usage, ITCM behavior, stack sizing, or build notes) are recorded in `FINDINGS.md`.
