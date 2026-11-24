"""Tests for Seam 1: build-time PebbleOS syscall table generator CLI."""

import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
GEN_SCRIPT = REPO_ROOT / "tools" / "gen_syscall_table.py"
LIBPEBBLE = REPO_ROOT / "third_party" / "pebble_sdk" / "libpebble.a"


class SyscallGeneratorTests(unittest.TestCase):
    def test_generates_all_663_symbols_and_677_non_null_slots_from_libpebble(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = pathlib.Path(tmp)
            proc = subprocess.run(
                [sys.executable, str(GEN_SCRIPT), "--lib", str(LIBPEBBLE), "--out-dir", str(out_dir)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(proc.returncode, 0, f"generator failed: {proc.stderr}")

            hdr = (out_dir / "pebble_syscall_table_gen.h").read_text()
            src = (out_dir / "pebble_syscall_table_gen.c").read_text()

            self.assertIn("#define PEBBLE_SYSCALL_TABLE_SLOTS 677", hdr)
            self.assertIn("#define PEBBLE_SYSCALL_SYMBOL_COUNT 663", hdr)

            # Verify independent ground-truth slot mappings from libpebble.a
            expected_slots = {
                0: "accel_data_service_subscribe__deprecated",
                31: "app_event_loop",
                271: "window_create",
                531: "difftime",
                676: "health_service_set_hrv_sample_period",
            }
            for slot, name in expected_slots.items():
                self.assertRegex(
                    hdr,
                    rf"#define\s+PEBBLE_SYS_{re.escape(name.upper())}\s+{slot}\b",
                    f"missing slot macro for {name}",
                )
                self.assertIn(f'[{slot}] = "{name}"', src)

            # Verify every single slot 0..676 has a non-NULL default stub entry
            stub_entries = re.findall(r"\[(\d+)\]\s*=\s*pebble_stub_slot_\d+", src)
            self.assertEqual(len(stub_entries), 677)
            self.assertEqual(set(int(i) for i in stub_entries), set(range(677)))

            # Verify gap slot (e.g. 32) has a reserved fallback name rather than NULL
            self.assertIn('[32] = "<reserved_slot_32>"', src)

    def test_rejects_missing_library_path_with_clear_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run(
                [sys.executable, str(GEN_SCRIPT), "--lib", f"{tmp}/does-not-exist.a", "--out-dir", tmp],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(proc.returncode, 0)
            self.assertTrue(proc.stderr.strip(), "expected diagnostic error message on stderr")


if __name__ == "__main__":
    unittest.main()
