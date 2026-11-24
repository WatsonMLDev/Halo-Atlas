# 01: Native `.pbw` Binary Loader & 663-Slot Syscall Tracer

**What to build:** Load and execute an unmodified Pebble `.pbw` binary (`pebble-app.bin`) natively as 32-bit ARM Thumb-2 code under `qemu-arm-static` through an OS-agnostic C core separated by a minimal `pebble_hal` boundary. The runtime auto-generates all 663 PebbleOS jump-table slots from the SDK's `libpebble.a` with safe logging/no-op defaults, validates and relocates the `PebbleProcessInfo` image, patches `pbl_table_addr`, enters `main()`, and prints every PebbleOS syscall invoked by the app without crashing on unimplemented APIs.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [x] A build-time generator extracts all 663 PebbleOS syscall names and jump-table indices from the Pebble SDK's `libpebble.a` and emits default logger/no-op stubs for every slot.
- [x] The OS-agnostic C core defines the minimal `pebble_hal` interface (executable allocation, time, canvas flush, input poll, resource read, logging) with zero OS-specific headers in the core.
- [x] The binary loader parses `PebbleProcessInfo` (struct version 16 and legacy <16), copies `load_size` bytes into executable RAM, zeroes `.bss` up to `virtual_size`, applies all 32-bit relocations, patches `pbl_table_addr`, and enters `(base_addr + entry_point) | 1`.
- [x] Malformed or invalid binaries (bad magic, truncated image, oversized virtual size) return a clear diagnostic error instead of faulting.
- [x] Soft-float `.pbw` calls interoperate safely with the hard-float ARM host ABI (`pcs("aapcs")` on floating-point shims) and packed `-fshort-enums` Pebble structs.
- [x] Running unmodified `test-app.pbw` under `qemu-arm-static` enters `main()`, traces every PebbleOS syscall it invokes by name and index, and exits cleanly without crashing on unimplemented calls.
