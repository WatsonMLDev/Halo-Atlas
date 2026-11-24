"""Tests for Seam 2: ARM Thumb-2 pebble-runner executing binaries under qemu-arm-static."""

import pathlib
import re
import struct
import subprocess
import tempfile
import unittest
import zipfile

from tests.pbw_builder import build_pebble_app_bin

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNNER_BIN = REPO_ROOT / "build" / "pebble-runner"
TEST_PBW = REPO_ROOT / "tests" / "fixtures" / "test-app.pbw"


def _load_chalk_fixture() -> bytes:
    with zipfile.ZipFile(TEST_PBW) as z:
        return z.read("chalk/pebble-app.bin")


class ArmRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        proc = subprocess.run(["make", "-C", str(REPO_ROOT), "runner"], capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"make runner failed:\n{proc.stdout}\n{proc.stderr}")

    def _run_bin(self, data: bytes, extra_args: list[str] | None = None) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp:
            app_bin = pathlib.Path(tmp) / "pebble-app.bin"
            app_bin.write_bytes(data)
            cmd = ["qemu-arm-static", str(RUNNER_BIN), *(extra_args or []), str(app_bin)]
            return subprocess.run(cmd, capture_output=True, text=True, timeout=10)

    def test_unmodified_test_app_pbw_enters_main_traces_syscalls_and_exits_cleanly(self) -> None:
        proc = self._run_bin(_load_chalk_fixture(), ["--trace"])
        self.assertEqual(
            proc.returncode,
            0,
            f"runner crashed or failed (rc={proc.returncode}):\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}",
        )

        output = proc.stdout + proc.stderr
        # Must log app metadata and traced PebbleOS syscalls by name and slot index
        self.assertIn("For Your Friends", output)
        traced = re.findall(r"\[syscall\s+#(\d+)\]\s+([A-Za-z0-9_<>]+)", output)
        self.assertGreaterEqual(len(traced), 5, f"expected traced syscalls, got:\n{output}")

        traced_names = {name for _, name in traced}
        self.assertIn("window_create", traced_names)
        self.assertIn("app_event_loop", traced_names)
        # Verify slot indices match libpebble.a ground truth
        self.assertIn(("271", "window_create"), traced)
        self.assertIn(("31", "app_event_loop"), traced)

    def test_rejects_bad_magic_with_diagnostic_error(self) -> None:
        bad = bytearray(_load_chalk_fixture())
        bad[0:6] = b"NOPE!!"
        proc = self._run_bin(bytes(bad))
        self.assertNotEqual(proc.returncode, 0)
        self.assertGreater(proc.returncode, 0, "must return error code, not crash with signal")
        self.assertIn("magic", proc.stderr.lower())

    def test_rejects_truncated_header_and_truncated_image(self) -> None:
        valid = _load_chalk_fixture()

        # Truncated header (< 0x82 bytes)
        proc_hdr = self._run_bin(valid[:64])
        self.assertGreater(proc_hdr.returncode, 0)
        self.assertIn("truncated", proc_hdr.stderr.lower())

        # Truncated body (smaller than load_size)
        proc_img = self._run_bin(valid[:1024])
        self.assertGreater(proc_img.returncode, 0)
        self.assertIn("truncated", proc_img.stderr.lower())

        # Truncated relocation table (load_size present, missing reloc words)
        load_size = struct.unpack_from("<H", valid, 0x0E)[0]
        proc_reloc = self._run_bin(valid[:load_size])
        self.assertGreater(proc_reloc.returncode, 0)
        self.assertIn("truncated", proc_reloc.stderr.lower())

    def test_rejects_unsupported_struct_version(self) -> None:
        bad = bytearray(_load_chalk_fixture())
        bad[8] = 0x7F  # unknown struct_version.major
        proc = self._run_bin(bytes(bad))
        self.assertGreater(proc.returncode, 0)
        self.assertIn("version", proc.stderr.lower())

    def test_rejects_oversized_and_invalid_virtual_size(self) -> None:
        # Exceeds default RAM budget (0xFFFF > 61440)
        oversized = bytearray(_load_chalk_fixture())
        struct.pack_into("<H", oversized, 0x80, 0xFFFF)
        proc_big = self._run_bin(bytes(oversized))
        self.assertGreater(proc_big.returncode, 0)
        self.assertIn("virtual_size", proc_big.stderr.lower())

        # Exceeds explicit --max-ram budget
        proc_cap = self._run_bin(_load_chalk_fixture(), ["--max-ram", "2048"])
        self.assertGreater(proc_cap.returncode, 0)
        self.assertIn("virtual_size", proc_cap.stderr.lower())

        # virtual_size smaller than load_size
        too_small = bytearray(_load_chalk_fixture())
        struct.pack_into("<H", too_small, 0x80, 256)
        proc_small = self._run_bin(bytes(too_small))
        self.assertGreater(proc_small.returncode, 0)
        self.assertIn("virtual_size", proc_small.stderr.lower())

    def test_rejects_out_of_bounds_entry_and_relocation_offset(self) -> None:
        bad_entry = bytearray(_load_chalk_fixture())
        struct.pack_into("<I", bad_entry, 0x10, 0x5000)
        proc_entry = self._run_bin(bytes(bad_entry))
        self.assertGreater(proc_entry.returncode, 0)
        self.assertIn("entry_point", proc_entry.stderr.lower())

        bad_reloc = bytearray(_load_chalk_fixture())
        load_size = struct.unpack_from("<H", bad_reloc, 0x0E)[0]
        struct.pack_into("<I", bad_reloc, load_size, load_size - 2)
        proc_reloc = self._run_bin(bytes(bad_reloc))
        self.assertGreater(proc_reloc.returncode, 0)
        self.assertIn("relocation", proc_reloc.stderr.lower())

    def test_zeroes_bss_and_applies_32bit_relocations_on_v16_binary(self) -> None:
        c_src = r"""
#include <stdint.h>
extern void app_event_loop(void);

static const char k_tag[] = "HALO";
static const char *volatile g_reloc_ptr = k_tag;
volatile uint8_t g_bss_buf[512];

int main(void) {
    app_event_loop();
    for (uint32_t i = 0; i < sizeof(g_bss_buf); ++i) {
        if (g_bss_buf[i] != 0u) {
            return 100 + (int)g_bss_buf[i];
        }
    }
    const char *p = g_reloc_ptr;
    if (p[0] == 'H' && p[1] == 'A' && p[2] == 'L' && p[3] == 'O' && p[4] == '\0') {
        return 42;
    }
    return 99;
}
"""
        app_bin = build_pebble_app_bin(c_src, struct_version_major=0x10, app_name="BssRelocTest")
        proc = self._run_bin(app_bin, ["--trace"])
        self.assertEqual(proc.returncode, 0, f"stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")
        m = re.search(r"load=(\d+),\s*virt=(\d+),\s*relocs=(\d+)", proc.stdout)
        self.assertIsNotNone(m, f"expected loader banner in stdout:\n{proc.stdout}")
        assert m is not None
        load_sz, virt_sz, relocs = int(m.group(1)), int(m.group(2)), int(m.group(3))
        self.assertGreaterEqual(virt_sz - load_sz, 512, "fixture must actually have >=512B .bss")
        self.assertGreaterEqual(relocs, 1, "fixture must actually exercise 32-bit relocations")
        self.assertIn("App exited cleanly (rc=42, syscalls=1)", proc.stdout)
        self.assertIn("[syscall #31] app_event_loop", proc.stdout)

    def test_default_stubs_log_unimplemented_syscalls_without_trace_flag(self) -> None:
        c_src = r"""
extern void vibes_short_pulse(void);
extern int health_service_peek_hrv_ppi_ms(void);

int main(void) {
    vibes_short_pulse();
    (void)health_service_peek_hrv_ppi_ms();
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, struct_version_major=0x10, app_name="StubLogTest")
        proc = self._run_bin(app_bin)
        self.assertEqual(proc.returncode, 0, f"stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")
        output = proc.stdout + proc.stderr
        self.assertIn("vibes_short_pulse (stub)", output)
        self.assertIn("[syscall #675] health_service_peek_hrv_ppi_ms (stub)", output)

    def test_legacy_struct_versions_8_and_9_load_relocate_zero_bss_and_execute(self) -> None:
        c_src = r"""
#include <stdint.h>
extern void app_event_loop(void);

static const char k_legacy[] = "OK";
static const char *volatile g_ptr = k_legacy;
volatile uint8_t g_legacy_bss[4096];

int main(void) {
    app_event_loop();
    for (uint32_t i = 0; i < sizeof(g_legacy_bss); ++i) {
        if (g_legacy_bss[i] != 0u) {
            return 200 + (int)g_legacy_bss[i];
        }
    }
    return (g_ptr[0] == 'O' && g_ptr[1] == 'K') ? 7 : 1;
}
"""
        for sv_major in (0x08, 0x09):
            with self.subTest(struct_version_major=hex(sv_major)):
                app_bin = build_pebble_app_bin(
                    c_src, struct_version_major=sv_major, app_name=f"LegacyV{sv_major}"
                )
                proc = self._run_bin(app_bin, ["--trace"])
                self.assertEqual(proc.returncode, 0, f"stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")
                self.assertIn(f"struct v{sv_major}.0", proc.stdout)
                self.assertIn("App exited cleanly (rc=7, syscalls=1)", proc.stdout)

    def test_soft_float_difftime_and_short_enum_tuplet_abi_interop(self) -> None:
        c_src = r"""
#include <stdint.h>

extern double difftime(int32_t end, int32_t beginning);

typedef enum {
    TUPLE_BYTE_ARRAY = 0,
    TUPLE_CSTRING = 1,
    TUPLE_UINT = 2,
    TUPLE_INT = 3,
} TupleType;

typedef struct Tuplet {
    TupleType type;
    uint32_t key;
    union {
        struct { const uint8_t *data; uint16_t length; } bytes;
        struct { const char *data; uint16_t length; } cstring;
        struct { uint32_t storage; uint16_t width; } integer;
    };
} Tuplet;

extern uint32_t dict_calc_buffer_size_from_tuplets(const Tuplet *tuplets, uint8_t count);

int main(void) {
    _Static_assert(sizeof(TupleType) == 1, "pbw must compile with -fshort-enums");

    union { double d; uint64_t u; } pos;
    pos.d = difftime(1000, 250);
    /* IEEE-754 double 750.0 == 0x4087700000000000ULL */
    if (pos.u != 0x4087700000000000ULL) {
        return 10;
    }

    union { double d; uint64_t u; } neg;
    neg.d = difftime(250, 1000);
    /* IEEE-754 double -750.0 == 0xC087700000000000ULL */
    if (neg.u != 0xC087700000000000ULL) {
        return 11;
    }

    static const char k_hello[] = "hello";
    Tuplet items[2] = {
        { .type = TUPLE_UINT, .key = 1, .integer = { .storage = 1234, .width = 4 } },
        { .type = TUPLE_CSTRING, .key = 2, .cstring = { .data = k_hello, .length = 6 } },
    };
    uint32_t sz = dict_calc_buffer_size_from_tuplets(items, 2);
    /* Expected: 1 (dict header) + (7 + 4) + (7 + 6) = 25 */
    if (sz != 25u) {
        return (int)(100u + sz);
    }
    return 42;
}
"""
        app_bin = build_pebble_app_bin(c_src, struct_version_major=0x10, app_name="AbiInteropTest")
        proc = self._run_bin(app_bin, ["--trace"])
        self.assertEqual(proc.returncode, 0, f"stderr:\n{proc.stderr}\nstdout:\n{proc.stdout}")
        self.assertIn("[syscall #531] difftime", proc.stdout)
        self.assertIn("dict_calc_buffer_size_from_tuplets", proc.stdout)
        self.assertIn("App exited cleanly (rc=42, syscalls=3)", proc.stdout)


if __name__ == "__main__":
    unittest.main()
