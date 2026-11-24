"""Non-invasive HaloEmulator integration exposing frame.pebble.run(...) to Lua."""

from __future__ import annotations

from pathlib import Path
import threading
from typing import Any, Callable

from halo_emulator import HaloEmulator
from halo_emulator.event_queue import Event
from PIL import Image

from halo_atlas.runner import FRAME_WIDTH, PebbleRunnerSession, RunnerOptions


def _event_to_runner_bytes(ev: Event) -> bytes | None:
    if ev.type == "button_single":
        return b"S\n"
    if ev.type == "button_double":
        return b"B\n"
    if ev.type == "button_long":
        return b"L\n"
    if ev.type == "imu_tap":
        if ev.data == "double":
            return b"2\n"
        if ev.data == "triple":
            return b"3\n"
        return b"T\n"
    return None


def _lua_opt(opts: Any, key: str, default: Any = None) -> Any:
    if opts is None:
        return default
    try:
        val = opts[key]
        return default if val is None else val
    except (KeyError, TypeError, AttributeError):
        return getattr(opts, key, default)


def _options_from_lua(opts: Any, *, default_persist_dir: Path | None = None) -> RunnerOptions:
    platform = _lua_opt(opts, "platform", None)
    time_epoch = _lua_opt(opts, "time_epoch", None)
    tick_step = _lua_opt(opts, "tick_step", None)
    max_frames = _lua_opt(opts, "max_frames", None)
    trace = bool(_lua_opt(opts, "trace", False))
    persist_path = _lua_opt(opts, "persist_path", None)
    return RunnerOptions(
        platform=str(platform) if platform is not None else None,
        time_epoch=int(time_epoch) if time_epoch is not None else None,
        tick_step=int(tick_step) if tick_step is not None else None,
        max_frames=int(max_frames) if max_frames is not None else None,
        trace=trace,
        persist_path=Path(str(persist_path)) if persist_path is not None else None,
        persist_dir=default_persist_dir if persist_path is None else None,
    )


def _install_pebble_table(
    emulator: HaloEmulator,
    *,
    runner_bin: str | Path | None = None,
    on_frame_shown: Callable[[Image.Image], None] | None = None,
) -> Callable[[], None]:
    session_lock = threading.Lock()
    active_session: dict[str, PebbleRunnerSession | None] = {"current": None}

    def _push_frame(rgb888: bytes) -> None:
        emulator._display.bitmap(1, 1, FRAME_WIDTH, 0, 0, rgb888)
        emulator._display.show()
        if on_frame_shown is not None:
            on_frame_shown(emulator.get_framebuffer())

    def _pebble_stop() -> None:
        with session_lock:
            sess = active_session["current"]
        if sess is not None:
            sess.stop()

    def _pebble_run(pbw_path: str, opts: Any = None) -> int:
        session_stop = threading.Event()
        persist_dir = emulator._sandbox_dir / "pebble_persist"
        lua_on_frame = _lua_opt(opts, "on_frame", None)
        frame_counter = [0]

        def _on_frame_dispatch(rgb888: bytes) -> None:
            _push_frame(rgb888)
            frame_counter[0] += 1
            if callable(lua_on_frame):
                lua_on_frame(frame_counter[0])

        session = PebbleRunnerSession(
            pbw_path,
            on_frame=_on_frame_dispatch,
            options=_options_from_lua(opts, default_persist_dir=persist_dir),
            runner_bin=runner_bin,
            stop_event=session_stop,
        )
        with session_lock:
            active_session["current"] = session

        pump_done = threading.Event()
        deferred_events: list[Event] = []

        def _pump_inputs() -> None:
            while not pump_done.is_set():
                if emulator._stop_event.is_set():
                    session.stop()
                    break
                ev = emulator._event_queue.get_nowait()
                if ev is None:
                    pump_done.wait(0.005)
                    continue
                if ev.type == "stop":
                    session.stop()
                    deferred_events.append(ev)
                    break
                wire = _event_to_runner_bytes(ev)
                if wire is not None:
                    session.send_input(wire)
                else:
                    deferred_events.append(ev)

        pump_thread = threading.Thread(target=_pump_inputs, daemon=True, name="halo-pebble-input")
        pump_thread.start()
        try:
            summary = session.run()
        finally:
            pump_done.set()
            pump_thread.join(timeout=1.0)
            for ev in deferred_events:
                emulator._event_queue.put(ev)
            with session_lock:
                if active_session["current"] is session:
                    active_session["current"] = None

        if summary.returncode != 0:
            raise RuntimeError(
                f"pebble-runner exited with code {summary.returncode}:\n{summary.stderr.strip()}"
            )
        return summary.frames

    rt = emulator._lua
    assert rt is not None
    pebble_tbl = rt.table()
    pebble_tbl.run = _pebble_run
    pebble_tbl.stop = _pebble_stop
    rt.globals().frame.pebble = pebble_tbl
    return _pebble_stop


def attach_pebble_runtime(
    emulator: HaloEmulator,
    *,
    runner_bin: str | Path | None = None,
    on_frame_shown: Callable[[Image.Image], None] | None = None,
) -> HaloEmulator:
    """Non-invasively attach `frame.pebble` onto any HaloEmulator, surviving connect()/start()."""
    if getattr(emulator, "_atlas_attached", False):
        return emulator

    orig_build = emulator._build_runtime
    orig_stop = emulator.stop
    stop_holder: dict[str, Callable[[], None] | None] = {"stop": None}

    def _wrapped_build() -> None:
        orig_build()
        stop_holder["stop"] = _install_pebble_table(
            emulator, runner_bin=runner_bin, on_frame_shown=on_frame_shown
        )

    def _wrapped_stop() -> None:
        stop_fn = stop_holder["stop"]
        if stop_fn is not None:
            stop_fn()
        orig_stop()

    emulator._build_runtime = _wrapped_build  # type: ignore[method-assign]
    emulator.stop = _wrapped_stop  # type: ignore[method-assign]
    setattr(emulator, "_atlas_attached", True)

    if emulator._lua is not None:
        stop_holder["stop"] = _install_pebble_table(
            emulator, runner_bin=runner_bin, on_frame_shown=on_frame_shown
        )
    return emulator


class AtlasHaloEmulator(HaloEmulator):
    """HaloEmulator subclass with `frame.pebble.run(...)` permanently attached."""

    def __init__(
        self,
        sandbox_dir: str | Path | None = None,
        *,
        print_handler: Callable[[str], None] | None = print,
        runner_bin: str | Path | None = None,
        on_frame_shown: Callable[[Image.Image], None] | None = None,
    ) -> None:
        super().__init__(sandbox_dir=sandbox_dir, print_handler=print_handler)
        self._runner_bin = runner_bin
        self._on_frame_shown = on_frame_shown
        self._pebble_stop: Callable[[], None] | None = None

    def load_lua_source(self, filename: str, source: str) -> None:
        """Write a Lua script directly into the emulator's sandbox directory."""
        (self._sandbox_dir / filename).write_text(source, encoding="utf-8")

    def _build_runtime(self) -> None:
        super()._build_runtime()
        self._pebble_stop = _install_pebble_table(
            self, runner_bin=self._runner_bin, on_frame_shown=self._on_frame_shown
        )

    def stop_pebble(self) -> None:
        """Stop any active Pebble app session without stopping the HaloEmulator Lua VM."""
        if self._pebble_stop is not None:
            self._pebble_stop()

    def stop(self) -> None:
        self.stop_pebble()
        super().stop()
