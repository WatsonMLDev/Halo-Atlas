"""Host-side Pebble .pbw archive unpacker and platform selector."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import zipfile

PLATFORM_PRIORITY: tuple[str, ...] = ("gabbro", "chalk", "basalt", "aplite")

PLATFORM_DIMENSIONS: dict[str, tuple[int, int]] = {
    "gabbro": (260, 260),
    "chalk": (180, 180),
    "basalt": (144, 168),
    "aplite": (144, 168),
}


class PbwError(RuntimeError):
    """Raised when a .pbw archive is invalid or lacks a usable platform binary."""


@dataclass(frozen=True)
class UnpackedPbw:
    platform: str
    width: int
    height: int
    app_bin: bytes
    resources_pbpack: bytes | None
    uuid: str | None = None


def _platform_entry_paths(platform: str) -> tuple[list[str], list[str]]:
    if platform == "aplite":
        return (
            ["aplite/pebble-app.bin", "pebble-app.bin"],
            ["aplite/app_resources.pbpack", "app_resources.pbpack"],
        )
    return (
        [f"{platform}/pebble-app.bin"],
        [f"{platform}/app_resources.pbpack"],
    )


def unpack_pbw(pbw_path: str | Path, preferred_platform: str | None = None) -> UnpackedPbw:
    """Unpack a .pbw ZIP archive, selecting gabbro -> chalk -> basalt -> aplite."""
    path = Path(pbw_path)
    if not path.is_file():
        raise PbwError(f".pbw file not found: {path}")

    if preferred_platform is not None and preferred_platform not in PLATFORM_DIMENSIONS:
        raise PbwError(f"unsupported Pebble platform: {preferred_platform!r}")

    try:
        zf = zipfile.ZipFile(path, "r")
    except zipfile.BadZipFile as exc:
        raise PbwError(f"invalid .pbw archive (not a valid ZIP): {path}") from exc

    with zf:
        names = set(zf.namelist())
        app_uuid: str | None = None
        if "appinfo.json" in names:
            try:
                info = json.loads(zf.read("appinfo.json").decode("utf-8"))
                raw_uuid = info.get("uuid")
                if isinstance(raw_uuid, str) and raw_uuid.strip():
                    app_uuid = raw_uuid.strip()
            except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
                pass
        order: tuple[str, ...] = (
            (preferred_platform,) if preferred_platform is not None else PLATFORM_PRIORITY
        )
        for platform in order:
            bin_candidates, res_candidates = _platform_entry_paths(platform)
            bin_entry = next((c for c in bin_candidates if c in names), None)
            if bin_entry is None:
                continue
            res_entry = next((c for c in res_candidates if c in names), None)
            width, height = PLATFORM_DIMENSIONS[platform]
            return UnpackedPbw(
                platform=platform,
                width=width,
                height=height,
                app_bin=zf.read(bin_entry),
                resources_pbpack=zf.read(res_entry) if res_entry is not None else None,
                uuid=app_uuid,
            )

    if preferred_platform is not None:
        raise PbwError(f"platform {preferred_platform!r} binary not found in {path}")
    raise PbwError(f"no supported pebble-app.bin found in {path}")
