"""Subprocess bridge to the compiled ARM Thumb-2 pebble-runner under qemu-arm-static."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import threading
from typing import Callable

from halo_atlas.pbw import unpack_pbw

FRAME_MAGIC = b"HALO"
FRAME_WIDTH = 256
FRAME_HEIGHT = 256
FRAME_RGB888_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 3


def default_runner_path() -> Path:
    repo_root = Path(__file__).resolve().parent.parent.parent
    return repo_root / "build" / "pebble-runner"


def _read_exact(stream, n: int) -> bytes | None:
    chunks: list[bytes] = []
    remaining = n
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


@dataclass(frozen=True)
class RunnerOptions:
    platform: str | None = None
    time_epoch: int | None = None
    tick_step: int | None = None
    max_frames: int | None = None
    trace: bool = False
    persist_path: Path | None = None
    persist_dir: Path | None = None


def _derive_persist_file(persist_dir: Path, app_uuid: str | None, input_path: Path) -> Path:
    if app_uuid:
        safe = re.sub(r"[^A-Za-z0-9_-]", "_", app_uuid)
    else:
        digest = hashlib.sha256(str(input_path.resolve()).encode("utf-8")).hexdigest()[:16]
        stem = re.sub(r"[^A-Za-z0-9_-]", "_", input_path.stem)[:32] or "app"
        safe = f"{stem}_{digest}"
    persist_dir.mkdir(parents=True, exist_ok=True)
    return persist_dir / f"{safe}.kv"


@dataclass(frozen=True)
class RunSummary:
    returncode: int
    frames: int
    platform: str
    stderr: str


class PebbleRunnerSession:
    """Manages a single qemu-arm-static pebble-runner process streaming 256x256 RGB888 frames."""

    def __init__(
        self,
        pbw_or_bin: str | Path,
        *,
        on_frame: Callable[[bytes], None],
        options: RunnerOptions | None = None,
        runner_bin: str | Path | None = None,
        stop_event: threading.Event | None = None,
    ) -> None:
        self._input_path = Path(pbw_or_bin)
        self._on_frame = on_frame
        self._options = options or RunnerOptions()
        self._runner_bin = Path(runner_bin) if runner_bin is not None else default_runner_path()
        self._stop_event = stop_event or threading.Event()
        self._proc: subprocess.Popen[bytes] | None = None
        self._pending_input: list[bytes] = []
        self._lock = threading.Lock()

    def send_input(self, data: bytes) -> None:
        """Forward a single-byte or line-delimited input command to the running ARM process."""
        if not data:
            return
        with self._lock:
            if self._stop_event.is_set():
                return
            proc = self._proc
            if proc is None:
                self._pending_input.append(data)
                return
            if proc.poll() is not None or proc.stdin is None or proc.stdin.closed:
                return
            try:
                proc.stdin.write(data)
                proc.stdin.flush()
            except OSError:
                pass

    @staticmethod
    def _send_quit_locked(proc: subprocess.Popen[bytes] | None) -> None:
        if proc is None or proc.poll() is not None:
            return
        if proc.stdin is not None and not proc.stdin.closed:
            try:
                proc.stdin.write(b"Q\n")
                proc.stdin.flush()
                proc.stdin.close()
            except OSError:
                pass

    def stop(self) -> None:
        self._stop_event.set()
        with self._lock:
            self._pending_input.clear()
            self._send_quit_locked(self._proc)

    def run(self) -> RunSummary:
        if not self._runner_bin.is_file():
            raise FileNotFoundError(
                f"pebble-runner binary not found at {self._runner_bin}; build it with `make runner`"
            )
        qemu = shutil.which("qemu-arm-static")
        if qemu is None:
            raise FileNotFoundError("qemu-arm-static not found on PATH")

        opts = self._options
        with tempfile.TemporaryDirectory(prefix="halo_atlas_pbw_") as tmp_dir:
            res_path: Path | None = None
            app_uuid: str | None = None
            if self._input_path.suffix.lower() == ".pbw":
                unpacked = unpack_pbw(self._input_path, preferred_platform=opts.platform)
                platform = unpacked.platform
                app_uuid = unpacked.uuid
                bin_path = Path(tmp_dir) / "pebble-app.bin"
                bin_path.write_bytes(unpacked.app_bin)
                if unpacked.resources_pbpack is not None:
                    res_path = Path(tmp_dir) / "app_resources.pbpack"
                    res_path.write_bytes(unpacked.resources_pbpack)
            else:
                platform = opts.platform or "chalk"
                bin_path = self._input_path

            persist_file: Path | None = None
            if opts.persist_path is not None:
                persist_file = Path(opts.persist_path)
                persist_file.parent.mkdir(parents=True, exist_ok=True)
            elif opts.persist_dir is not None:
                persist_file = _derive_persist_file(Path(opts.persist_dir), app_uuid, self._input_path)

            cmd = [
                qemu,
                str(self._runner_bin),
                "--stream-frames",
                "--platform",
                platform,
            ]
            if res_path is not None:
                cmd.extend(["--resources", str(res_path)])
            if persist_file is not None:
                cmd.extend(["--persist", str(persist_file)])
            if opts.trace:
                cmd.append("--trace")
            if opts.time_epoch is not None:
                cmd.extend(["--time-epoch", str(int(opts.time_epoch))])
            if opts.tick_step is not None:
                cmd.extend(["--tick-step", str(int(opts.tick_step))])
            if opts.max_frames is not None:
                cmd.extend(["--max-frames", str(int(opts.max_frames))])
            cmd.append(str(bin_path))

            stderr_chunks: list[bytes] = []
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            with self._lock:
                self._proc = proc
                if self._pending_input and proc.stdin is not None and not proc.stdin.closed:
                    try:
                        for payload in self._pending_input:
                            proc.stdin.write(payload)
                        proc.stdin.flush()
                    except OSError:
                        pass
                    self._pending_input.clear()
                if self._stop_event.is_set():
                    self._send_quit_locked(proc)

            def _drain_stderr() -> None:
                assert proc.stderr is not None
                while True:
                    chunk = proc.stderr.read(4096)
                    if not chunk:
                        break
                    stderr_chunks.append(chunk)

            stderr_thread = threading.Thread(target=_drain_stderr, daemon=True)
            stderr_thread.start()

            frames = 0
            assert proc.stdout is not None
            try:
                while not self._stop_event.is_set():
                    hdr = _read_exact(proc.stdout, 8)
                    if hdr is None:
                        break
                    if hdr[:4] != FRAME_MAGIC:
                        raise RuntimeError(f"desynchronized frame stream header: {hdr!r}")
                    (payload_len,) = struct.unpack_from("<I", hdr, 4)
                    if payload_len != FRAME_RGB888_BYTES:
                        raise RuntimeError(f"unexpected frame payload size: {payload_len}")
                    rgb888 = _read_exact(proc.stdout, payload_len)
                    if rgb888 is None:
                        break
                    self._on_frame(rgb888)
                    frames += 1
                    if self._stop_event.is_set():
                        self.stop()
                        break
            finally:
                if proc.poll() is None:
                    self.stop()
                    try:
                        proc.wait(timeout=2.0)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=2.0)
                stderr_thread.join(timeout=2.0)
                for pipe in (proc.stdin, proc.stdout, proc.stderr):
                    if pipe is not None and not pipe.closed:
                        try:
                            pipe.close()
                        except OSError:
                            pass
                with self._lock:
                    self._proc = None

            stderr_text = b"".join(stderr_chunks).decode("utf-8", errors="replace")
            return RunSummary(
                returncode=proc.returncode or 0,
                frames=frames,
                platform=platform,
                stderr=stderr_text,
            )
