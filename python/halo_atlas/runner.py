"""Subprocess bridge to the compiled ARM Thumb-2 pebble-runner under qemu-arm-static."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
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
        self._lock = threading.Lock()

    def stop(self) -> None:
        self._stop_event.set()
        with self._lock:
            proc = self._proc
        if proc is not None and proc.poll() is None:
            try:
                if proc.stdin is not None and not proc.stdin.closed:
                    proc.stdin.write(b"Q\n")
                    proc.stdin.flush()
                    proc.stdin.close()
            except OSError:
                pass

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
            if self._input_path.suffix.lower() == ".pbw":
                unpacked = unpack_pbw(self._input_path, preferred_platform=opts.platform)
                platform = unpacked.platform
                bin_path = Path(tmp_dir) / "pebble-app.bin"
                bin_path.write_bytes(unpacked.app_bin)
            else:
                platform = opts.platform or "chalk"
                bin_path = self._input_path

            cmd = [
                qemu,
                str(self._runner_bin),
                "--stream-frames",
                "--platform",
                platform,
            ]
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
