"""Interactive CLI for running Pebble .pbw apps inside Brilliant Labs' official HaloEmulator."""

from __future__ import annotations

import argparse
from pathlib import Path
import threading
import time

from halo_atlas.emulator import AtlasHaloEmulator
from halo_atlas.pbw import PLATFORM_PRIORITY
from halo_atlas.runner import RunnerOptions


def _build_lua_launcher(pbw_path: Path, options: RunnerOptions) -> str:
    escaped = str(pbw_path.resolve()).replace("\\", "\\\\").replace("'", "\\'")
    opts: list[str] = []
    if options.platform is not None:
        opts.append(f"platform = '{options.platform}'")
    if options.time_epoch is not None:
        opts.append(f"time_epoch = {int(options.time_epoch)}")
    if options.tick_step is not None:
        opts.append(f"tick_step = {int(options.tick_step)}")
    if options.max_frames is not None:
        opts.append(f"max_frames = {int(options.max_frames)}")
    if options.trace:
        opts.append("trace = true")
    opts_lua = "{ " + ", ".join(opts) + " }"
    return f"frame.pebble.run('{escaped}', {opts_lua})\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="halo-atlas",
        description="Run unmodified Pebble .pbw watchfaces/apps inside official HaloEmulator",
    )
    parser.add_argument("pbw", type=Path, help="Path to a .pbw archive or raw pebble-app.bin")
    parser.add_argument(
        "--platform",
        choices=PLATFORM_PRIORITY,
        default=None,
        help="Override Pebble platform selection (default: gabbro -> chalk -> basalt -> aplite)",
    )
    parser.add_argument("--headless", action="store_true", help="Run without opening the Pygame window")
    parser.add_argument(
        "--record",
        type=Path,
        default=None,
        help="Record display output to .gif or .mp4/.webm/.avi",
    )
    parser.add_argument("--fps", type=float, default=30.0, help="Recording frame rate (default: 30)")
    parser.add_argument("--time-epoch", type=int, default=None, help="Simulated starting UTC epoch seconds")
    parser.add_argument("--tick-step", type=int, default=None, help="Advance simulated clock by N seconds/frame")
    parser.add_argument("--max-frames", type=int, default=None, help="Stop after flushing N frames")
    parser.add_argument("--trace", action="store_true", help="Log every invoked PebbleOS syscall")
    args = parser.parse_args(argv)

    options = RunnerOptions(
        platform=args.platform,
        time_epoch=args.time_epoch,
        tick_step=args.tick_step,
        max_frames=args.max_frames,
        trace=args.trace,
    )
    lua_code = _build_lua_launcher(args.pbw, options)

    with AtlasHaloEmulator() as emu:
        emu.load_lua_source("main.lua", lua_code)
        if args.record is not None:
            emu.start_recording(fps=args.fps)
            print(f"[halo-atlas] Recording at {args.fps:.0f} fps -> {args.record}")

        emu.start("main.lua")

        if args.headless:
            try:
                emu.wait()
            except KeyboardInterrupt:
                emu.stop()
            if args.record is not None:
                # Ensure VideoRecorder samples at least one rendered frame on fast headless runs.
                time.sleep(max(0.05, 1.5 / max(args.fps, 1.0)))
        else:
            from halo_emulator.cli import _pygame_window

            stop_event = threading.Event()

            def _watch_lua() -> None:
                try:
                    emu.wait()
                finally:
                    stop_event.set()

            watcher = threading.Thread(target=_watch_lua, daemon=True)
            watcher.start()
            try:
                _pygame_window(emu, stop_event)
            except KeyboardInterrupt:
                pass
            finally:
                stop_event.set()
                emu.stop()

        if args.record is not None:
            emu.stop_recording(args.record)
            print(f"[halo-atlas] Saved recording to {args.record}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
