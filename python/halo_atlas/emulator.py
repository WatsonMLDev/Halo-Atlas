"""Non-invasive HaloEmulator integration exposing frame.pebble.run(...) to Lua."""

from __future__ import annotations

from pathlib import Path
import threading
from typing import Any, Callable

from halo_emulator import HaloEmulator

from halo_atlas.runner import FRAME_WIDTH, PebbleRunnerSession


def _lua_opt(opts: Any, key: str, default: Any = None) -> Any:
    if opts is None:
        return default
    try:
        val = opts[key]
        return default if val is None else val
    except (KeyError, TypeError, AttributeError):
        return getattr(opts, key, default)


def attach_pebble_runtime(
    emulator: HaloEmulator,
    *,
    runner_bin: str | Path | None = None,
) -> None:
    """Attach `frame.pebble.run` / `frame.pebble.stop` onto an initialized HaloEmulator."""
    if emulator._lua is None:
        raise RuntimeError("HaloEmulator Lua runtime not initialized")

    session_lock = threading.Lock()
    active_session: dict[str, PebbleRunnerSession | None] = {"current": None}

    def _push_frame(rgb888: bytes) -> None:
        emulator._display.bitmap(1, 1, FRAME_WIDTH, 0, 0, rgb888)
        emulator._display.show()

    def _pebble_stop() -> None:
        with session_lock:
            sess = active_session["current"]
        if sess is not None:
            sess.stop()

    def _pebble_run(pbw_path: str, opts: Any = None) -> int:
        platform = _lua_opt(opts, "platform", None)
        time_epoch = _lua_opt(opts, "time_epoch", None)
        tick_step = _lua_opt(opts, "tick_step", None)
        max_frames = _lua_opt(opts, "max_frames", None)
        trace = bool(_lua_opt(opts, "trace", False))

        session = PebbleRunnerSession(
            pbw_path,
            on_frame=_push_frame,
            platform=str(platform) if platform is not None else None,
            time_epoch=int(time_epoch) if time_epoch is not None else None,
            tick_step=int(tick_step) if tick_step is not None else None,
            max_frames=int(max_frames) if max_frames is not None else None,
            trace=trace,
            runner_bin=runner_bin,
            stop_event=emulator._stop_event,
        )
        with session_lock:
            active_session["current"] = session
        try:
            summary = session.run()
        finally:
            with session_lock:
                if active_session["current"] is session:
                    active_session["current"] = None

        if summary.returncode != 0:
            raise RuntimeError(
                f"pebble-runner exited with code {summary.returncode}:\n{summary.stderr.strip()}"
            )
        return summary.frames

    rt = emulator._lua
    pebble_tbl = rt.table()
    pebble_tbl.run = _pebble_run
    pebble_tbl.stop = _pebble_stop
    rt.globals().frame.pebble = pebble_tbl
    # Keep stop hook accessible for AtlasHaloEmulator.stop()
    setattr(emulator, "_atlas_pebble_stop", _pebble_stop)


class AtlasHaloEmulator(HaloEmulator):
    """HaloEmulator subclass that non-invasively registers frame.pebble.* on every VM build."""

    def __init__(
        self,
        sandbox_dir: str | Path | None = None,
        *,
        print_handler: Callable[[str], None] | None = print,
        runner_bin: str | Path | None = None,
    ) -> None:
        super().__init__(sandbox_dir=sandbox_dir, print_handler=print_handler)
        self._runner_bin = runner_bin

    def _build_runtime(self) -> None:
        super()._build_runtime()
        attach_pebble_runtime(self, runner_bin=self._runner_bin)

    def stop(self) -> None:
        stop_fn = getattr(self, "_atlas_pebble_stop", None)
        if callable(stop_fn):
            stop_fn()
        super().stop()


def create_emulator(
    sandbox_dir: str | Path | None = None,
    *,
    print_handler: Callable[[str], None] | None = print,
    runner_bin: str | Path | None = None,
) -> AtlasHaloEmulator:
    """Create a HaloEmulator instance with frame.pebble.run(...) attached."""
    return AtlasHaloEmulator(
        sandbox_dir=sandbox_dir,
        print_handler=print_handler,
        runner_bin=runner_bin,
    )
