"""Tests for Seam 2: host .pbw archive unpacker and platform selection."""

import io
import pathlib
import tempfile
import unittest
import zipfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TEST_PBW = REPO_ROOT / "tests" / "fixtures" / "test-app.pbw"


def _make_pbw(path: pathlib.Path, entries: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w") as z:
        for name, data in entries.items():
            z.writestr(name, data)


class PbwUnpackerTests(unittest.TestCase):
    def test_prefers_chalk_over_basalt_and_aplite_in_test_app_pbw(self) -> None:
        from halo_atlas.pbw import unpack_pbw

        unpacked = unpack_pbw(TEST_PBW)
        self.assertEqual(unpacked.platform, "chalk")
        self.assertEqual((unpacked.width, unpacked.height), (180, 180))
        self.assertTrue(unpacked.app_bin.startswith(b"PBLAPP\x00\x00"))
        self.assertIsNotNone(unpacked.resources_pbpack)
        assert unpacked.resources_pbpack is not None
        self.assertGreater(len(unpacked.resources_pbpack), 0)

    def test_prefers_gabbro_first_and_falls_back_through_chalk_basalt_aplite(self) -> None:
        from halo_atlas.pbw import unpack_pbw

        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)

            # gabbro beats chalk/basalt/aplite
            p_gabbro = tmp_dir / "gabbro.pbw"
            _make_pbw(
                p_gabbro,
                {
                    "gabbro/pebble-app.bin": b"PBLAPP\x00\x00gabbro",
                    "chalk/pebble-app.bin": b"PBLAPP\x00\x00chalk",
                    "basalt/pebble-app.bin": b"PBLAPP\x00\x00basalt",
                    "pebble-app.bin": b"PBLAPP\x00\x00aplite",
                },
            )
            u = unpack_pbw(p_gabbro)
            self.assertEqual(u.platform, "gabbro")
            self.assertEqual((u.width, u.height), (260, 260))
            self.assertEqual(u.app_bin, b"PBLAPP\x00\x00gabbro")

            # basalt beats aplite when gabbro & chalk are absent
            p_basalt = tmp_dir / "basalt.pbw"
            _make_pbw(
                p_basalt,
                {
                    "basalt/pebble-app.bin": b"PBLAPP\x00\x00basalt",
                    "pebble-app.bin": b"PBLAPP\x00\x00aplite",
                },
            )
            u2 = unpack_pbw(p_basalt)
            self.assertEqual(u2.platform, "basalt")
            self.assertEqual((u2.width, u2.height), (144, 168))

            # root pebble-app.bin maps to aplite (144x168)
            p_aplite = tmp_dir / "aplite.pbw"
            _make_pbw(p_aplite, {"pebble-app.bin": b"PBLAPP\x00\x00aplite"})
            u3 = unpack_pbw(p_aplite)
            self.assertEqual(u3.platform, "aplite")
            self.assertEqual((u3.width, u3.height), (144, 168))

    def test_explicit_platform_override_and_invalid_archive_errors(self) -> None:
        from halo_atlas.pbw import PbwError, unpack_pbw

        u_basalt = unpack_pbw(TEST_PBW, preferred_platform="basalt")
        self.assertEqual(u_basalt.platform, "basalt")
        self.assertEqual((u_basalt.width, u_basalt.height), (144, 168))

        with tempfile.TemporaryDirectory() as tmp:
            bad = pathlib.Path(tmp) / "not-a-zip.pbw"
            bad.write_bytes(b"not a zip archive")
            with self.assertRaises(PbwError):
                unpack_pbw(bad)

            empty = pathlib.Path(tmp) / "empty.pbw"
            _make_pbw(empty, {"appinfo.json": b"{}"})
            with self.assertRaises(PbwError):
                unpack_pbw(empty)


if __name__ == "__main__":
    unittest.main()
