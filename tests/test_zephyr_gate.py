"""Tests for Ticket #5: Zephyr RTOS HAL module, Cortex-M55 hard-float compilation, BSS budget, and QEMU/Halo gates."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE_INCLUDE = REPO_ROOT / "core" / "include"
CORE_SRC = REPO_ROOT / "core" / "src"
ZEPHYR_HAL_DIR = REPO_ROOT / "hal" / "zephyr"
GEN_SYSCALL_SCRIPT = REPO_ROOT / "tools" / "gen_syscall_table.py"
GEN_EMBEDDED_SCRIPT = REPO_ROOT / "tools" / "gen_embedded_pbw.py"
LIBPEBBLE = REPO_ROOT / "third_party" / "pebble_sdk" / "libpebble.a"
TEST_PBW = REPO_ROOT / "tests" / "fixtures" / "test-app.pbw"


class ZephyrCortexM55GateTests(unittest.TestCase):
    def test_zephyr_module_descriptor_points_to_existing_build_files(self) -> None:
        """zephyr/module.yml resolves to real CMakeLists.txt, Kconfig, HAL sources, and QEMU app."""
        module_yml = (REPO_ROOT / "zephyr" / "module.yml").read_text(encoding="utf-8")
        kv: dict[str, str] = {}
        for line in module_yml.splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#") or ":" not in stripped:
                continue
            key, val = stripped.split(":", 1)
            if val.strip():
                kv[key.strip()] = val.strip()

        self.assertEqual(kv.get("name"), "pebble_compat")
        cmake_dir = REPO_ROOT / kv["cmake"]
        kconfig_path = REPO_ROOT / kv["kconfig"]
        self.assertTrue((cmake_dir / "CMakeLists.txt").is_file())
        self.assertTrue(kconfig_path.is_file())
        self.assertTrue((cmake_dir / "include" / "pebble_hal_zephyr.h").is_file())
        self.assertTrue((cmake_dir / "pebble_hal_zephyr.c").is_file())
        self.assertTrue((cmake_dir / "qemu_app" / "CMakeLists.txt").is_file())

        cmake_txt = (cmake_dir / "CMakeLists.txt").read_text(encoding="utf-8")
        for core_c in sorted(CORE_SRC.glob("*.c")):
            self.assertIn(
                core_c.name,
                cmake_txt,
                f"core source {core_c.name} missing from {cmake_dir / 'CMakeLists.txt'}",
            )

    def test_embedded_pbw_generator_produces_valid_c_header_for_test_app(self) -> None:
        """tools/gen_embedded_pbw.py embeds unmodified test-app.pbw binary + .pbpack for Zephyr QEMU."""
        with tempfile.TemporaryDirectory() as tmp:
            out_hdr = pathlib.Path(tmp) / "pebble_embedded_pbw.h"
            proc = subprocess.run(
                [
                    "python3",
                    str(GEN_EMBEDDED_SCRIPT),
                    "--pbw",
                    str(TEST_PBW),
                    "--out",
                    str(out_hdr),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(proc.returncode, 0, f"gen_embedded_pbw.py failed:\n{proc.stderr}")
            text = out_hdr.read_text(encoding="utf-8")
            self.assertIn("#define PEBBLE_EMBEDDED_PLATFORM PEBBLE_PLATFORM_CHALK", text)
            self.assertIn("g_embedded_pbw_bin[4300]", text)
            self.assertIn("g_embedded_pbw_res[26588]", text)

    def test_core_compiles_for_cortex_m55_hard_float_within_halo_static_ram_budget(self) -> None:
        """All pebble_compat core C sources compile for Cortex-M55 hard-float and keep static BSS < 22 KB."""
        res_dir = subprocess.check_output(["clang", "-print-resource-dir"], text=True).strip()
        freestanding_inc = pathlib.Path(res_dir) / "include"

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = pathlib.Path(tmp)
            gen_dir = tmp_path / "gen"
            subprocess.run(
                ["python3", str(GEN_SYSCALL_SCRIPT), "--lib", str(LIBPEBBLE), "--out-dir", str(gen_dir)],
                check=True,
                capture_output=True,
                text=True,
            )

            all_sources = sorted(CORE_SRC.glob("*.c")) + [gen_dir / "pebble_syscall_table_gen.c"]
            objs: list[pathlib.Path] = []
            for src in all_sources:
                obj = tmp_path / f"{src.stem}.o"
                proc = subprocess.run(
                    [
                        "clang",
                        "--target=arm-none-eabi",
                        "-mcpu=cortex-m55",
                        "-mthumb",
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
                        "-Os",
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
                    f"{src.name} failed Cortex-M55 hard-float build:\n{proc.stderr}",
                )
                objs.append(obj)

            # Halo firmware (-b halo) has ~23.7 KB remaining in static 856 KB RAM; enforce < 22 KB total BSS+data.
            size_out = subprocess.check_output(["llvm-size", *[str(o) for o in objs]], text=True)
            total_ram = 0
            for line in size_out.splitlines()[1:]:
                parts = line.split()
                if len(parts) >= 4:
                    total_ram += int(parts[1]) + int(parts[2])  # data + bss
            self.assertLess(
                total_ram,
                22 * 1024,
                f"core static data+bss ({total_ram} B) exceeds Halo firmware 22 KB static RAM budget",
            )

    @unittest.skipUnless(
        os.environ.get("RUN_ZEPHYR_DOCKER_GATE") == "1",
        "Set RUN_ZEPHYR_DOCKER_GATE=1 to run the full Docker Zephyr -b halo & Cortex-M55 QEMU gates",
    )
    def test_zephyr_qemu_and_halo_firmware_docker_gates(self) -> None:
        """Full Docker verification: Cortex-M55 QEMU (mps3_an547) execution + real halo-firmware (-b halo) link."""
        proc = subprocess.run(
            ["bash", str(REPO_ROOT / "tools" / "run_zephyr_gate.sh"), "all"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(proc.returncode, 0, f"run_zephyr_gate.sh failed:\n{proc.stdout}\n{proc.stderr}")
        self.assertIn("PEBBLE_QEMU_GATE: PASS", proc.stdout)
        self.assertIn("pebble_runtime_run", proc.stdout)
        self.assertIn("pebble_zephyr_hal_init", proc.stdout)


if __name__ == "__main__":
    unittest.main()
