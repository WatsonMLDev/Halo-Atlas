"""Tests for Seam 3: OS-agnostic C core compiles freestanding with zero OS headers."""

import pathlib
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE_INCLUDE = REPO_ROOT / "core" / "include"
CORE_SRC = REPO_ROOT / "core" / "src"
GEN_SCRIPT = REPO_ROOT / "tools" / "gen_syscall_table.py"
LIBPEBBLE = REPO_ROOT / "third_party" / "pebble_sdk" / "libpebble.a"


class CorePortabilityTests(unittest.TestCase):
    def test_core_compiles_freestanding_arm_thumb2_with_no_os_headers(self) -> None:
        self.assertTrue(CORE_SRC.is_dir(), "core/src directory does not exist yet")
        c_files = sorted(CORE_SRC.glob("*.c"))
        self.assertTrue(c_files, "expected C source files under core/src/")

        # Locate clang's compiler-provided freestanding headers (stdint.h, stddef.h, stdbool.h)
        res_dir = subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip()
        freestanding_inc = pathlib.Path(res_dir) / "include"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = pathlib.Path(tmp)
            gen_dir = tmp_path / "gen"
            subprocess.run(
                ["python3", str(GEN_SCRIPT), "--lib", str(LIBPEBBLE), "--out-dir", str(gen_dir)],
                check=True,
                capture_output=True,
                text=True,
            )

            all_sources = c_files + [gen_dir / "pebble_syscall_table_gen.c"]
            for src in all_sources:
                obj = tmp_path / f"{src.stem}.o"
                proc = subprocess.run(
                    [
                        "clang",
                        "--target=arm-linux-gnueabihf",
                        "-march=armv7ve",
                        "-mthumb",
                        "-mfpu=vfpv3-d16",
                        "-mfloat-abi=hard",
                        "-ffreestanding",
                        "-nostdinc",
                        "-isystem",
                        str(freestanding_inc),
                        "-I",
                        str(CORE_INCLUDE),
                        "-I",
                        str(gen_dir),
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-c",
                        str(src),
                        "-o",
                        str(obj),
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    proc.returncode,
                    0,
                    f"{src.name} failed freestanding zero-OS-header build:\n{proc.stderr}",
                )


if __name__ == "__main__":
    unittest.main()
