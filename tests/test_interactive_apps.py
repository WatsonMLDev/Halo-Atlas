"""Tests at the HaloEmulator seam for Ticket #4: interactive buttons, temple taps, timers, persistence, and clean exit."""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import threading
import unittest
import zipfile

import numpy as np
from PIL import Image

from halo_atlas.emulator import AtlasHaloEmulator
from tests.pbw_builder import build_pebble_app_bin

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


def _pack_interactive_pbw(
    path: pathlib.Path,
    platform: str,
    app_bin: bytes,
    *,
    uuid: str = "11111111-2222-3333-4444-555555555555",
    short_name: str = "InteractiveTest",
) -> None:
    bin_entry = "pebble-app.bin" if platform == "aplite" else f"{platform}/pebble-app.bin"
    appinfo = {
        "uuid": uuid,
        "shortName": short_name,
        "longName": short_name,
        "companyName": "HaloAtlas",
        "versionLabel": "1.0",
        "sdkVersion": "3",
        "targetPlatforms": [platform],
        "watchapp": {"watchface": False},
    }
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("appinfo.json", json.dumps(appinfo))
        z.writestr(bin_entry, app_bin)


class _FrameWaiter:
    """Thread-safe frame collector that avoids fixed sleeps in interactive emulator tests."""

    def __init__(self) -> None:
        self._cond = threading.Condition()
        self.frames: list[np.ndarray] = []

    def on_frame(self, img: Image.Image) -> None:
        arr = np.array(img.convert("RGB"))
        with self._cond:
            self.frames.append(arr)
            self._cond.notify_all()

    def wait_for_count(self, count: int, timeout: float = 5.0) -> np.ndarray:
        with self._cond:
            ok = self._cond.wait_for(lambda: len(self.frames) >= count, timeout=timeout)
            if not ok:
                raise AssertionError(f"timed out waiting for frame #{count} (got {len(self.frames)})")
            return self.frames[count - 1]

    def wait_for_predicate(self, predicate, timeout: float = 5.0) -> np.ndarray:
        with self._cond:
            ok = self._cond.wait_for(lambda: any(predicate(f) for f in self.frames), timeout=timeout)
            if not ok:
                raise AssertionError(
                    f"timed out waiting for frame predicate (saw {len(self.frames)} frames)"
                )
            for f in reversed(self.frames):
                if predicate(f):
                    return f
            raise AssertionError("unreachable")


def _chalk_px(arr: np.ndarray, x: int, y: int) -> tuple[int, int, int]:
    r, g, b = arr[38 + y, 38 + x]
    return (int(r), int(g), int(b))


class InteractivePebbleAppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        proc = subprocess.run(["make", "-C", str(REPO_ROOT), "runner"], capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"make runner failed:\n{proc.stdout}\n{proc.stderr}")

    def test_multiscreen_app_buttons_temple_taps_and_clean_exit_returns_to_lua(self) -> None:
        """Multi-screen .pbw navigates via temple taps + button clicks and cleanly returns to Lua on root Back."""
        c_src = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint8_t argb; } GColor8;
typedef struct { int16_t x; int16_t y; } GPoint;
typedef struct { int16_t w; int16_t h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct Window Window;
typedef struct Layer Layer;
typedef struct GContext GContext;
typedef void *ClickRecognizerRef;
typedef enum {
    BUTTON_ID_BACK = 0,
    BUTTON_ID_UP = 1,
    BUTTON_ID_SELECT = 2,
    BUTTON_ID_DOWN = 3,
} ButtonId;
typedef void (*WindowHandler)(Window *);
typedef void (*LayerUpdateProc)(Layer *, GContext *);
typedef void (*ClickHandler)(ClickRecognizerRef, void *);
typedef void (*ClickConfigProvider)(void *);
typedef struct { WindowHandler load, appear, disappear, unload; } WindowHandlers;

extern Window *window_create(void);
extern void window_destroy(Window *);
extern Layer *window_get_root_layer(const Window *);
extern void window_set_window_handlers(Window *, WindowHandlers);
extern void window_set_background_color(Window *, GColor8);
extern void window_set_click_config_provider(Window *, ClickConfigProvider);
extern void window_set_click_config_provider_with_context(Window *, ClickConfigProvider, void *);
extern void window_single_click_subscribe(ButtonId, ClickHandler);
extern ButtonId click_recognizer_get_button_id(ClickRecognizerRef);
extern void window_stack_push(Window *, bool);
extern void layer_set_update_proc(Layer *, LayerUpdateProc);
extern void layer_mark_dirty(Layer *);
extern void graphics_context_set_fill_color(GContext *, GColor8);
extern void graphics_fill_rect(GContext *, GRect, uint16_t, uint8_t);
extern void app_event_loop(void);

static Window *s_main_win;
static Window *s_detail_win;
static int s_selected_idx = 0;
static int s_detail_magic = 0x12345678;
static bool s_detail_toggled = false;
static bool s_main_unloaded = false;

static void detail_draw(Layer *l, GContext *ctx) {
    (void)l;
    /* Yellow (0xFC) initially, Magenta (0xF3) after Select with verified context & button_id. */
    graphics_context_set_fill_color(ctx, (GColor8){s_detail_toggled ? 0xF3 : 0xFC});
    graphics_fill_rect(ctx, (GRect){{20, 20}, {140, 140}}, 0, 0);
}

static void detail_select_click(ClickRecognizerRef rec, void *ctx) {
    if (ctx == &s_detail_magic && click_recognizer_get_button_id(rec) == BUTTON_ID_SELECT) {
        s_detail_toggled = !s_detail_toggled;
        layer_mark_dirty(window_get_root_layer(s_detail_win));
    }
}

static void detail_click_config(void *ctx) {
    (void)ctx;
    window_single_click_subscribe(BUTTON_ID_SELECT, detail_select_click);
}

static void detail_load(Window *w) {
    s_detail_toggled = false;
    layer_set_update_proc(window_get_root_layer(w), detail_draw);
}

static void main_draw(Layer *l, GContext *ctx) {
    (void)l;
    for (int i = 0; i < 3; ++i) {
        GColor8 col = (i == s_selected_idx) ? (GColor8){0xCC} : (GColor8){0xC3};
        graphics_context_set_fill_color(ctx, col);
        graphics_fill_rect(ctx, (GRect){{20, (int16_t)(20 + i * 45)}, {140, 30}}, 0, 0);
    }
}

static void main_down_click(ClickRecognizerRef rec, void *ctx) {
    (void)ctx;
    if (click_recognizer_get_button_id(rec) == BUTTON_ID_DOWN) {
        s_selected_idx = (s_selected_idx + 1) % 3;
        layer_mark_dirty(window_get_root_layer(s_main_win));
    }
}

static void main_up_click(ClickRecognizerRef rec, void *ctx) {
    (void)ctx;
    if (click_recognizer_get_button_id(rec) == BUTTON_ID_UP) {
        s_selected_idx = (s_selected_idx + 2) % 3;
        layer_mark_dirty(window_get_root_layer(s_main_win));
    }
}

static void main_select_click(ClickRecognizerRef rec, void *ctx) {
    (void)rec;
    (void)ctx;
    window_stack_push(s_detail_win, true);
}

static void main_click_config(void *ctx) {
    (void)ctx;
    window_single_click_subscribe(BUTTON_ID_DOWN, main_down_click);
    window_single_click_subscribe(BUTTON_ID_UP, main_up_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, main_select_click);
}

static void main_load(Window *w) {
    layer_set_update_proc(window_get_root_layer(w), main_draw);
}

static void main_unload(Window *w) {
    (void)w;
    s_main_unloaded = true;
}

int main(void) {
    s_main_win = window_create();
    window_set_background_color(s_main_win, (GColor8){0xC0});
    window_set_click_config_provider(s_main_win, main_click_config);
    window_set_window_handlers(s_main_win, (WindowHandlers){.load = main_load, .unload = main_unload});

    s_detail_win = window_create();
    window_set_background_color(s_detail_win, (GColor8){0xC0});
    window_set_click_config_provider_with_context(s_detail_win, detail_click_config, &s_detail_magic);
    window_set_window_handlers(s_detail_win, (WindowHandlers){.load = detail_load});

    window_stack_push(s_main_win, false);
    app_event_loop();

    window_destroy(s_detail_win);
    window_destroy(s_main_win);
    return s_main_unloaded ? 0 : 1;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="MultiScreenApp")

        with tempfile.TemporaryDirectory() as tmp:
            pbw_path = pathlib.Path(tmp) / "multiscreen.pbw"
            _pack_interactive_pbw(pbw_path, "chalk", app_bin, short_name="MultiScreenApp")

            waiter = _FrameWaiter()
            logs: list[str] = []
            with AtlasHaloEmulator(print_handler=logs.append, on_frame_shown=waiter.on_frame) as emu:
                emu.load_lua_source(
                    "main.lua",
                    f"local n = frame.pebble.run('{pbw_path.as_posix()}', {{}})\n"
                    "print('LUA_RESUMED:' .. tostring(n))\n",
                )
                emu.start("main.lua")

                # Frame 1: Root window, item 0 selected (Green), items 1 & 2 Blue
                f1 = waiter.wait_for_count(1)
                self.assertEqual(_chalk_px(f1, 40, 30), (0, 255, 0))
                self.assertEqual(_chalk_px(f1, 40, 75), (0, 0, 255))
                self.assertEqual(_chalk_px(f1, 40, 120), (0, 0, 255))

                # Single temple tap -> BUTTON_ID_DOWN -> item 1 selected
                emu.inject_imu_tap("single")
                f2 = waiter.wait_for_count(2)
                self.assertEqual(_chalk_px(f2, 40, 30), (0, 0, 255))
                self.assertEqual(_chalk_px(f2, 40, 75), (0, 255, 0))

                # Double temple tap -> BUTTON_ID_UP -> item 0 selected
                emu.inject_imu_tap("double")
                f3 = waiter.wait_for_count(3)
                self.assertEqual(_chalk_px(f3, 40, 30), (0, 255, 0))
                self.assertEqual(_chalk_px(f3, 40, 75), (0, 0, 255))

                # Single temple tap -> item 1 selected
                emu.inject_imu_tap("single")
                f4 = waiter.wait_for_count(4)
                self.assertEqual(_chalk_px(f4, 40, 75), (0, 255, 0))

                # Single button click -> BUTTON_ID_SELECT -> pushes Detail window (Yellow)
                emu.inject_button_single()
                f5 = waiter.wait_for_count(5)
                self.assertEqual(_chalk_px(f5, 90, 90), (255, 255, 0))

                # Single button click on Detail window -> verified context & button_id -> Magenta
                emu.inject_button_single()
                f6 = waiter.wait_for_count(6)
                self.assertEqual(_chalk_px(f6, 90, 90), (255, 0, 255))

                # Double button click -> BUTTON_ID_BACK -> pops Detail back to Root (item 1 still Green)
                emu.inject_button_double()
                f7 = waiter.wait_for_count(7)
                self.assertEqual(_chalk_px(f7, 40, 75), (0, 255, 0))
                self.assertEqual(_chalk_px(f7, 40, 120), (0, 0, 255))

                # Single temple tap after pop -> verifies Root ClickConfigProvider was re-bound -> item 2 Green
                emu.inject_imu_tap("single")
                f8 = waiter.wait_for_count(8)
                self.assertEqual(_chalk_px(f8, 40, 120), (0, 255, 0))

                # Long button press on Root (no Select long-click subscribed) -> BUTTON_ID_BACK -> clean exit!
                emu.inject_button_long()
                emu.wait(timeout=5.0)
                self.assertFalse(emu.is_running())
                self.assertIsNone(emu.get_error())
                self.assertTrue(
                    any(line.startswith("LUA_RESUMED:") for line in logs),
                    f"expected Lua script to resume after root Back exit, got logs={logs!r}",
                )

    def test_interrupting_pebble_app_via_stop_returns_control_to_lua(self) -> None:
        """Calling frame.pebble.stop() interrupts app_event_loop cleanly without killing HaloEmulator."""
        c_src = r"""
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint8_t argb; } GColor8;
typedef struct Window Window;
extern Window *window_create(void);
extern void window_destroy(Window *);
extern void window_set_background_color(Window *, GColor8);
extern void window_stack_push(Window *, bool);
extern void app_event_loop(void);

int main(void) {
    Window *w = window_create();
    window_set_background_color(w, (GColor8){0xC3});
    window_stack_push(w, false);
    app_event_loop();
    window_destroy(w);
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="StopInterruptApp")
        with tempfile.TemporaryDirectory() as tmp:
            pbw_path = pathlib.Path(tmp) / "stop_interrupt.pbw"
            _pack_interactive_pbw(pbw_path, "chalk", app_bin, short_name="StopInterruptApp")

            waiter = _FrameWaiter()
            logs: list[str] = []
            with AtlasHaloEmulator(print_handler=logs.append, on_frame_shown=waiter.on_frame) as emu:
                emu.load_lua_source(
                    "main.lua",
                    f"local n = frame.pebble.run('{pbw_path.as_posix()}', {{\n"
                    "  on_frame = function(count)\n"
                    "    if count >= 1 then frame.pebble.stop() end\n"
                    "  end\n"
                    "})\n"
                    "frame.sleep(0.02)\n"
                    "print('CLEAN_INTERRUPT:' .. tostring(n))\n",
                )
                emu.start("main.lua")
                emu.wait(timeout=5.0)
                self.assertIsNone(emu.get_error())
                self.assertTrue(
                    any(line.startswith("CLEAN_INTERRUPT:1") for line in logs),
                    f"expected Lua script to interrupt via frame.pebble.stop() and resume, got logs={logs!r}",
                )

    def test_long_click_multi_click_and_accel_tap_service_callbacks(self) -> None:
        """window_long_click_subscribe, window_multi_click_subscribe, and accel_tap_service dispatch accurately."""
        c_src = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint8_t argb; } GColor8;
typedef struct { int16_t x; int16_t y; } GPoint;
typedef struct { int16_t w; int16_t h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct Window Window;
typedef struct Layer Layer;
typedef struct GContext GContext;
typedef void *ClickRecognizerRef;
typedef enum {
    BUTTON_ID_BACK = 0,
    BUTTON_ID_UP = 1,
    BUTTON_ID_SELECT = 2,
    BUTTON_ID_DOWN = 3,
} ButtonId;
typedef enum {
    ACCEL_AXIS_X = 0,
    ACCEL_AXIS_Y = 1,
    ACCEL_AXIS_Z = 2,
} AccelAxisType;
typedef void (*WindowHandler)(Window *);
typedef void (*LayerUpdateProc)(Layer *, GContext *);
typedef void (*ClickHandler)(ClickRecognizerRef, void *);
typedef void (*ClickConfigProvider)(void *);
typedef void (*AccelTapHandler)(AccelAxisType, int32_t);
typedef struct { WindowHandler load, appear, disappear, unload; } WindowHandlers;

extern Window *window_create(void);
extern void window_destroy(Window *);
extern Layer *window_get_root_layer(const Window *);
extern void window_set_window_handlers(Window *, WindowHandlers);
extern void window_set_background_color(Window *, GColor8);
extern void window_set_click_config_provider(Window *, ClickConfigProvider);
extern void window_single_click_subscribe(ButtonId, ClickHandler);
extern void window_multi_click_subscribe(ButtonId, uint8_t, uint8_t, uint16_t, bool, ClickHandler);
extern void window_long_click_subscribe(ButtonId, uint16_t, ClickHandler, ClickHandler);
extern uint8_t click_number_of_clicks_counted(ClickRecognizerRef);
extern ButtonId click_recognizer_get_button_id(ClickRecognizerRef);
extern void accel_tap_service_subscribe(AccelTapHandler);
extern void accel_tap_service_unsubscribe(void);
extern void window_stack_push(Window *, bool);
extern void layer_set_update_proc(Layer *, LayerUpdateProc);
extern void layer_mark_dirty(Layer *);
extern void graphics_context_set_fill_color(GContext *, GColor8);
extern void graphics_fill_rect(GContext *, GRect, uint16_t, uint8_t);
extern void app_event_loop(void);

static Window *s_win;
static uint8_t s_swatch[4] = {0xC0, 0xC0, 0xC0, 0xC0};
static bool s_long_down_seen = false;
static int s_tap_count = 0;

static void draw_cb(Layer *l, GContext *ctx) {
    (void)l;
    for (int i = 0; i < 4; ++i) {
        graphics_context_set_fill_color(ctx, (GColor8){s_swatch[i]});
        graphics_fill_rect(ctx, (GRect){{(int16_t)(10 + i * 42), 60}, {34, 60}}, 0, 0);
    }
}

static void on_single(ClickRecognizerRef rec, void *ctx) {
    (void)ctx;
    if (click_recognizer_get_button_id(rec) == BUTTON_ID_SELECT &&
        click_number_of_clicks_counted(rec) == 1) {
        s_swatch[0] = 0xCC; /* Green */
    } else {
        s_swatch[0] = 0xF0; /* Red = error */
    }
    layer_mark_dirty(window_get_root_layer(s_win));
}

static void on_multi(ClickRecognizerRef rec, void *ctx) {
    (void)ctx;
    uint8_t n = click_number_of_clicks_counted(rec);
    if (click_recognizer_get_button_id(rec) == BUTTON_ID_SELECT && n == 2) {
        s_swatch[1] = 0xC3; /* Blue */
    } else {
        s_swatch[1] = 0xF0; /* Red = error */
    }
    layer_mark_dirty(window_get_root_layer(s_win));
}

static void on_long_down(ClickRecognizerRef rec, void *ctx) {
    (void)ctx;
    if (click_recognizer_get_button_id(rec) == BUTTON_ID_SELECT) {
        s_long_down_seen = true;
    }
}

static void on_long_up(ClickRecognizerRef rec, void *ctx) {
    (void)ctx;
    if (s_long_down_seen && click_recognizer_get_button_id(rec) == BUTTON_ID_SELECT) {
        s_swatch[2] = 0xFC; /* Yellow */
    } else {
        s_swatch[2] = 0xF0; /* Red */
    }
    layer_mark_dirty(window_get_root_layer(s_win));
}

static void on_accel_tap(AccelAxisType axis, int32_t direction) {
    if (axis > ACCEL_AXIS_Z || direction == 0) {
        s_swatch[3] = 0xF0; /* Red */
    } else {
        s_tap_count++;
        if (s_tap_count == 1) {
            s_swatch[3] = 0xCF; /* Cyan */
        } else if (s_tap_count == 2) {
            s_swatch[3] = 0xF3; /* Magenta */
            accel_tap_service_unsubscribe();
        } else {
            s_swatch[3] = 0xF0; /* Red: should not fire after unsubscribe */
        }
    }
    layer_mark_dirty(window_get_root_layer(s_win));
}

static void click_config(void *ctx) {
    (void)ctx;
    window_single_click_subscribe(BUTTON_ID_SELECT, on_single);
    window_multi_click_subscribe(BUTTON_ID_SELECT, 2, 3, 180, true, on_multi);
    window_long_click_subscribe(BUTTON_ID_SELECT, 500, on_long_down, on_long_up);
}

static void on_load(Window *w) {
    layer_set_update_proc(window_get_root_layer(w), draw_cb);
    accel_tap_service_subscribe(on_accel_tap);
}

int main(void) {
    s_win = window_create();
    window_set_background_color(s_win, (GColor8){0xC0});
    window_set_click_config_provider(s_win, click_config);
    window_set_window_handlers(s_win, (WindowHandlers){.load = on_load});
    window_stack_push(s_win, false);
    app_event_loop();
    accel_tap_service_unsubscribe();
    window_destroy(s_win);
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="LongMultiTapApp")
        with tempfile.TemporaryDirectory() as tmp:
            pbw_path = pathlib.Path(tmp) / "long_multi_tap.pbw"
            _pack_interactive_pbw(pbw_path, "chalk", app_bin, short_name="LongMultiTapApp")

            waiter = _FrameWaiter()
            with AtlasHaloEmulator(print_handler=None, on_frame_shown=waiter.on_frame) as emu:
                emu.load_lua_source("main.lua", f"frame.pebble.run('{pbw_path.as_posix()}', {{}})\n")
                emu.start("main.lua")

                # Frame 1: initial blank swatches
                f1 = waiter.wait_for_count(1)
                self.assertEqual(_chalk_px(f1, 20, 80), (0, 0, 0))

                # 1. Single click (waits out 180ms multi-click timeout -> fires single_click)
                emu.inject_button_single()
                f2 = waiter.wait_for_count(2)
                self.assertEqual(_chalk_px(f2, 20, 80), (0, 255, 0), "single_click failed")
                self.assertEqual(_chalk_px(f2, 62, 80), (0, 0, 0), "multi_click must not fire on 1 click")

                # 2. Rapid 2x single click within 180ms -> fires multi_click with count=2
                emu.inject_button_single()
                emu.inject_button_single()
                f3 = waiter.wait_for_count(3)
                self.assertEqual(_chalk_px(f3, 62, 80), (0, 0, 255), "multi_click (count=2) failed")

                # 3. Long press -> fires Select long_click down + up handlers
                emu.inject_button_long()
                f4 = waiter.wait_for_count(4)
                self.assertEqual(_chalk_px(f4, 104, 80), (255, 255, 0), "long_click down+up failed")

                # 4. IMU single tap -> accel_tap_service fires (#1 = Cyan)
                emu.inject_imu_tap("single")
                f5 = waiter.wait_for_count(5)
                self.assertEqual(_chalk_px(f5, 146, 80), (0, 255, 255), "accel_tap #1 failed")

                # 5. IMU double tap -> accel_tap_service fires (#2 = Magenta) and unsubscribes
                emu.inject_imu_tap("double")
                f6 = waiter.wait_for_count(6)
                self.assertEqual(_chalk_px(f6, 146, 80), (255, 0, 255), "accel_tap #2 failed")

                # 6. Triple tap after unsubscribe must NOT trigger callback; double-click Back exits cleanly
                emu.inject_imu_tap("triple")
                emu.inject_button_single()
                f7 = waiter.wait_for_count(7)
                self.assertEqual(_chalk_px(f7, 146, 80), (255, 0, 255), "accel_tap fired after unsubscribe")

                emu.inject_button_double()
                emu.wait(timeout=5.0)
                self.assertIsNone(emu.get_error())

    def test_app_timers_drive_animation_reschedule_cancel_without_blocking_input(self) -> None:
        """app_timer_register, reschedule, and cancel drive live animation without blocking input."""
        c_src = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint8_t argb; } GColor8;
typedef struct { int16_t x; int16_t y; } GPoint;
typedef struct { int16_t w; int16_t h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct Window Window;
typedef struct Layer Layer;
typedef struct GContext GContext;
typedef struct AppTimer AppTimer;
typedef void *ClickRecognizerRef;
typedef enum {
    BUTTON_ID_BACK = 0,
    BUTTON_ID_UP = 1,
    BUTTON_ID_SELECT = 2,
    BUTTON_ID_DOWN = 3,
} ButtonId;
typedef void (*WindowHandler)(Window *);
typedef void (*LayerUpdateProc)(Layer *, GContext *);
typedef void (*ClickHandler)(ClickRecognizerRef, void *);
typedef void (*ClickConfigProvider)(void *);
typedef void (*AppTimerCallback)(void *);
typedef struct { WindowHandler load, appear, disappear, unload; } WindowHandlers;

extern Window *window_create(void);
extern void window_destroy(Window *);
extern Layer *window_get_root_layer(const Window *);
extern void window_set_window_handlers(Window *, WindowHandlers);
extern void window_set_background_color(Window *, GColor8);
extern void window_set_click_config_provider(Window *, ClickConfigProvider);
extern void window_single_click_subscribe(ButtonId, ClickHandler);
extern void window_stack_push(Window *, bool);
extern void layer_set_update_proc(Layer *, LayerUpdateProc);
extern void layer_mark_dirty(Layer *);
extern void graphics_context_set_fill_color(GContext *, GColor8);
extern void graphics_fill_rect(GContext *, GRect, uint16_t, uint8_t);
extern AppTimer *app_timer_register(uint32_t, AppTimerCallback, void *);
extern bool app_timer_reschedule(AppTimer *, uint32_t);
extern void app_timer_cancel(AppTimer *);
extern void app_event_loop(void);

static Window *s_win;
static AppTimer *s_anim_timer;
static AppTimer *s_resched_timer;
static int s_anim_ctx = 0xAA;
static int s_resched_ctx = 0xBB;
static int s_anim_step = 0;
static bool s_cancel_ok = true;
static bool s_resched_ok = false;
static bool s_select_seen = false;

static void draw_cb(Layer *l, GContext *ctx) {
    (void)l;
    /* Row 1: animated Green square at x = 15 + s_anim_step * 35 */
    int16_t ax = (int16_t)(15 + s_anim_step * 35);
    graphics_context_set_fill_color(ctx, (GColor8){0xCC});
    graphics_fill_rect(ctx, (GRect){{ax, 20}, {24, 24}}, 0, 0);

    /* Row 2: Cancel status (Green=ok, Red=fired despite cancel), Resched status (Blue=ok), Input (Yellow=ok) */
    graphics_context_set_fill_color(ctx, (GColor8){s_cancel_ok ? 0xCC : 0xF0});
    graphics_fill_rect(ctx, (GRect){{15, 80}, {36, 36}}, 0, 0);

    graphics_context_set_fill_color(ctx, (GColor8){s_resched_ok ? 0xC3 : 0xC0});
    graphics_fill_rect(ctx, (GRect){{65, 80}, {36, 36}}, 0, 0);

    graphics_context_set_fill_color(ctx, (GColor8){s_select_seen ? 0xFC : 0xC0});
    graphics_fill_rect(ctx, (GRect){{115, 80}, {36, 36}}, 0, 0);
}

static void on_anim(void *data) {
    s_anim_timer = NULL;
    if (data != &s_anim_ctx) {
        return;
    }
    if (s_anim_step < 3) {
        s_anim_step++;
        layer_mark_dirty(window_get_root_layer(s_win));
        if (s_anim_step < 3) {
            s_anim_timer = app_timer_register(35, on_anim, &s_anim_ctx);
        }
    }
}

static void on_cancelled_bad(void *data) {
    (void)data;
    s_cancel_ok = false;
    layer_mark_dirty(window_get_root_layer(s_win));
}

static void on_resched(void *data) {
    if (data == &s_resched_ctx && !app_timer_reschedule(s_resched_timer, 100)) {
        s_resched_ok = true;
    }
    layer_mark_dirty(window_get_root_layer(s_win));
}

static void on_select(ClickRecognizerRef rec, void *ctx) {
    (void)rec;
    (void)ctx;
    s_select_seen = true;
    layer_mark_dirty(window_get_root_layer(s_win));
}

static void click_config(void *ctx) {
    (void)ctx;
    window_single_click_subscribe(BUTTON_ID_SELECT, on_select);
}

static void on_load(Window *w) {
    layer_set_update_proc(window_get_root_layer(w), draw_cb);
    s_anim_timer = app_timer_register(35, on_anim, &s_anim_ctx);

    AppTimer *to_cancel = app_timer_register(40, on_cancelled_bad, NULL);
    app_timer_cancel(to_cancel);

    s_resched_timer = app_timer_register(10000, on_resched, &s_resched_ctx);
    if (!app_timer_reschedule(s_resched_timer, 25)) {
        s_cancel_ok = false;
    }
}

static void on_unload(Window *w) {
    (void)w;
    if (s_anim_timer != NULL) {
        app_timer_cancel(s_anim_timer);
        s_anim_timer = NULL;
    }
}

int main(void) {
    s_win = window_create();
    window_set_background_color(s_win, (GColor8){0xC0});
    window_set_click_config_provider(s_win, click_config);
    window_set_window_handlers(s_win, (WindowHandlers){.load = on_load, .unload = on_unload});
    window_stack_push(s_win, false);
    app_event_loop();
    window_destroy(s_win);
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="AppTimerAnimApp")
        with tempfile.TemporaryDirectory() as tmp:
            pbw_path = pathlib.Path(tmp) / "timers.pbw"
            _pack_interactive_pbw(pbw_path, "chalk", app_bin, short_name="AppTimerAnimApp")

            waiter = _FrameWaiter()
            with AtlasHaloEmulator(print_handler=None, on_frame_shown=waiter.on_frame) as emu:
                emu.load_lua_source("main.lua", f"frame.pebble.run('{pbw_path.as_posix()}', {{}})\n")
                emu.start("main.lua")

                # Frame 1: animation starts at step 0 (x=15)
                f1 = waiter.wait_for_count(1)
                self.assertEqual(_chalk_px(f1, 25, 30), (0, 255, 0))
                self.assertEqual(_chalk_px(f1, 130, 30), (0, 0, 0))

                # Inject Select click while timers are actively ticking
                emu.inject_button_single()

                # Wait for final state: animation reached step 3 (x=120), resched=Blue, select=Yellow
                final = waiter.wait_for_predicate(
                    lambda f: (
                        _chalk_px(f, 130, 30) == (0, 255, 0)
                        and _chalk_px(f, 80, 95) == (0, 0, 255)
                        and _chalk_px(f, 130, 95) == (255, 255, 0)
                    ),
                    timeout=5.0,
                )
                # Cancelled timer must NEVER have fired (swatch remains Green)
                self.assertEqual(_chalk_px(final, 30, 95), (0, 255, 0), "cancelled timer fired!")
                # Animation must have progressed through intermediate frames
                self.assertGreaterEqual(len(waiter.frames), 4)

                emu.inject_button_double()
                emu.wait(timeout=5.0)
                self.assertIsNone(emu.get_error())

    def test_persist_storage_roundtrips_all_types_across_restarts_and_isolates_apps(self) -> None:
        """persist_exists/read_*/write_*/delete persist across restarts in per-app sandboxed storage."""
        c_src = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint8_t argb; } GColor8;
typedef struct { int16_t x; int16_t y; } GPoint;
typedef struct { int16_t w; int16_t h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct Window Window;
typedef struct Layer Layer;
typedef struct GContext GContext;
typedef void (*WindowHandler)(Window *);
typedef void (*LayerUpdateProc)(Layer *, GContext *);
typedef struct { WindowHandler load, appear, disappear, unload; } WindowHandlers;

extern Window *window_create(void);
extern void window_destroy(Window *);
extern Layer *window_get_root_layer(const Window *);
extern void window_set_window_handlers(Window *, WindowHandlers);
extern void window_set_background_color(Window *, GColor8);
extern void window_stack_push(Window *, bool);
extern void layer_set_update_proc(Layer *, LayerUpdateProc);
extern void graphics_context_set_fill_color(GContext *, GColor8);
extern void graphics_fill_rect(GContext *, GRect, uint16_t, uint8_t);
extern bool persist_exists(uint32_t key);
extern size_t persist_get_max_size(void);
extern int persist_get_size(uint32_t key);
extern bool persist_read_bool(uint32_t key);
extern int32_t persist_read_int(uint32_t key);
extern int persist_read_data(uint32_t key, void *buffer, size_t buffer_size);
extern int persist_read_string(uint32_t key, char *buffer, size_t buffer_size);
extern int32_t persist_write_bool(uint32_t key, bool value);
extern int32_t persist_write_int(uint32_t key, int32_t value);
extern int persist_write_data(uint32_t key, const void *data, size_t size);
extern int persist_write_string(uint32_t key, const char *cstring);
extern int32_t persist_delete(uint32_t key);
extern void app_event_loop(void);

static uint8_t s_result_color = 0xF0; /* Red = fail */

static void draw_cb(Layer *l, GContext *ctx) {
    (void)l;
    graphics_context_set_fill_color(ctx, (GColor8){s_result_color});
    graphics_fill_rect(ctx, (GRect){{20, 20}, {140, 140}}, 0, 0);
}

static void on_load(Window *w) {
    layer_set_update_proc(window_get_root_layer(w), draw_cb);

    if (!persist_exists(1)) {
        /* Fresh Run 1 */
        uint8_t sentinel_data[4] = {0x11, 0x22, 0x33, 0x44};
        char sentinel_str[8] = "KEEP";
        bool ok = true;
        ok = ok && (persist_read_int(1) == 0);
        ok = ok && (!persist_read_bool(2));
        ok = ok && (persist_get_size(1) == -9);
        ok = ok && (persist_get_max_size() == 4096u);
        ok = ok && (persist_read_data(3, sentinel_data, sizeof(sentinel_data)) == -9);
        ok = ok && (sentinel_data[0] == 0x11 && sentinel_data[3] == 0x44);
        ok = ok && (persist_read_string(4, sentinel_str, sizeof(sentinel_str)) == -9);
        ok = ok && (sentinel_str[0] == 'K' && sentinel_str[3] == 'P');

        const uint8_t blob[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        ok = ok && (persist_write_int(1, 42) == 4);
        ok = ok && (persist_write_bool(2, true) == 1);
        ok = ok && (persist_write_data(3, blob, sizeof(blob)) == 4);
        ok = ok && (persist_write_string(4, "HALO") == 5);
        ok = ok && (persist_write_int(5, 99) == 4);
        ok = ok && persist_exists(5);
        ok = ok && (persist_delete(5) == 1);
        ok = ok && !persist_exists(5);
        ok = ok && (persist_delete(5) == -9);

        if (ok) {
            s_result_color = 0xFC; /* Yellow = Run 1 OK */
        }
    } else if (persist_read_int(1) == 42) {
        /* Run 2: reload all persisted values across restart */
        uint8_t blob[4] = {0, 0, 0, 0};
        char str[16] = {0};
        bool ok = true;
        ok = ok && (persist_get_size(1) == 4);
        ok = ok && persist_exists(2) && persist_read_bool(2) && (persist_get_size(2) == 1);
        ok = ok && persist_exists(3) && (persist_read_data(3, blob, sizeof(blob)) == 4);
        ok = ok && (blob[0] == 0xDE && blob[1] == 0xAD && blob[2] == 0xBE && blob[3] == 0xEF);
        ok = ok && persist_exists(4) && (persist_read_string(4, str, sizeof(str)) == 5);
        ok = ok && (str[0] == 'H' && str[1] == 'A' && str[2] == 'L' && str[3] == 'O' && str[4] == '\0');
        ok = ok && !persist_exists(5);
        ok = ok && (persist_write_int(1, 100) == 4);

        if (ok) {
            s_result_color = 0xCC; /* Green = Run 2 OK */
        }
    } else if (persist_read_int(1) == 100) {
        /* Run 3: verify overwritten key persisted */
        s_result_color = 0xCF; /* Cyan = Run 3 OK */
    }
}

int main(void) {
    Window *w = window_create();
    window_set_background_color(w, (GColor8){0xC0});
    window_set_window_handlers(w, (WindowHandlers){.load = on_load});
    window_stack_push(w, false);
    app_event_loop();
    window_destroy(w);
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="PersistTestApp")
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)
            sandbox_dir = tmp_dir / "emu_sandbox"
            pbw_a = tmp_dir / "app_a.pbw"
            pbw_b = tmp_dir / "app_b.pbw"
            _pack_interactive_pbw(
                pbw_a, "chalk", app_bin, uuid="aaaaaaaa-1111-2222-3333-444444444444", short_name="PersistA"
            )
            _pack_interactive_pbw(
                pbw_b, "chalk", app_bin, uuid="bbbbbbbb-1111-2222-3333-444444444444", short_name="PersistB"
            )

            # Run 1 of App A -> Yellow (initial write + delete verified)
            with AtlasHaloEmulator(sandbox_dir=sandbox_dir, print_handler=None) as emu:
                emu.connect()
                emu.execute_lua(f"frame.pebble.run('{pbw_a.as_posix()}', {{ max_frames = 1 }})")
                arr1 = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(_chalk_px(arr1, 90, 90), (255, 255, 0), "Run 1 persist write/delete failed")

            # Run 2 of App A (fresh HaloEmulator & ARM runner process) -> Green (reloaded all types)
            with AtlasHaloEmulator(sandbox_dir=sandbox_dir, print_handler=None) as emu:
                emu.connect()
                emu.execute_lua(f"frame.pebble.run('{pbw_a.as_posix()}', {{ max_frames = 1 }})")
                arr2 = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(_chalk_px(arr2, 90, 90), (0, 255, 0), "Run 2 persist reload across restart failed")

            # Run 3 of App A -> Cyan (saw updated value 100)
            with AtlasHaloEmulator(sandbox_dir=sandbox_dir, print_handler=None) as emu:
                emu.connect()
                emu.execute_lua(f"frame.pebble.run('{pbw_a.as_posix()}', {{ max_frames = 1 }})")
                arr3 = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(_chalk_px(arr3, 90, 90), (0, 255, 255), "Run 3 persist overwrite failed")

            # Run 1 of App B in same sandbox -> Yellow (isolated from App A's keys!)
            with AtlasHaloEmulator(sandbox_dir=sandbox_dir, print_handler=None) as emu:
                emu.connect()
                emu.execute_lua(f"frame.pebble.run('{pbw_b.as_posix()}', {{ max_frames = 1 }})")
                arr_b = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(_chalk_px(arr_b, 90, 90), (255, 255, 0), "App B was not isolated from App A storage")

    def test_end_to_end_interactive_multiscreen_app_with_timers_taps_persist_and_exit(self) -> None:
        """End-to-end multi-screen .pbw combines buttons, temple taps, timers, custom Back, persist, and reload."""
        c_src = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint8_t argb; } GColor8;
typedef struct { int16_t x; int16_t y; } GPoint;
typedef struct { int16_t w; int16_t h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct Window Window;
typedef struct Layer Layer;
typedef struct GContext GContext;
typedef struct AppTimer AppTimer;
typedef void *ClickRecognizerRef;
typedef enum {
    BUTTON_ID_BACK = 0,
    BUTTON_ID_UP = 1,
    BUTTON_ID_SELECT = 2,
    BUTTON_ID_DOWN = 3,
} ButtonId;
typedef void (*WindowHandler)(Window *);
typedef void (*LayerUpdateProc)(Layer *, GContext *);
typedef void (*ClickHandler)(ClickRecognizerRef, void *);
typedef void (*ClickConfigProvider)(void *);
typedef void (*AppTimerCallback)(void *);
typedef struct { WindowHandler load, appear, disappear, unload; } WindowHandlers;

extern Window *window_create(void);
extern void window_destroy(Window *);
extern Layer *window_get_root_layer(const Window *);
extern void window_set_window_handlers(Window *, WindowHandlers);
extern void window_set_background_color(Window *, GColor8);
extern void window_set_click_config_provider(Window *, ClickConfigProvider);
extern void window_single_click_subscribe(ButtonId, ClickHandler);
extern void window_stack_push(Window *, bool);
extern Window *window_stack_pop(bool);
extern void layer_set_update_proc(Layer *, LayerUpdateProc);
extern void layer_mark_dirty(Layer *);
extern void graphics_context_set_fill_color(GContext *, GColor8);
extern void graphics_fill_rect(GContext *, GRect, uint16_t, uint8_t);
extern AppTimer *app_timer_register(uint32_t, AppTimerCallback, void *);
extern void app_timer_cancel(AppTimer *);
extern bool persist_exists(uint32_t);
extern int32_t persist_read_int(uint32_t);
extern int32_t persist_write_int(uint32_t, int32_t);
extern void app_event_loop(void);

#define KEY_THEME 10u
static const uint8_t kThemeColors[3] = {0xC3, 0xCC, 0xF3}; /* Blue, Green, Magenta */

static Window *s_home_win;
static Window *s_settings_win;
static AppTimer *s_pulse_timer;
static int32_t s_theme = 0;
static bool s_pulse_on = false;
static bool s_confirm_back = false;

static void home_draw(Layer *l, GContext *ctx) {
    (void)l;
    /* Top band shows persisted theme color */
    graphics_context_set_fill_color(ctx, (GColor8){kThemeColors[s_theme % 3]});
    graphics_fill_rect(ctx, (GRect){{20, 20}, {140, 50}}, 0, 0);
    /* Bottom band toggles Yellow via app_timer */
    graphics_context_set_fill_color(ctx, (GColor8){s_pulse_on ? 0xFC : 0xC0});
    graphics_fill_rect(ctx, (GRect){{20, 100}, {140, 40}}, 0, 0);
}

static void settings_draw(Layer *l, GContext *ctx) {
    (void)l;
    /* White frame distinguishes Settings screen; inner box previews s_theme */
    graphics_context_set_fill_color(ctx, (GColor8){0xFF});
    graphics_fill_rect(ctx, (GRect){{10, 10}, {160, 160}}, 0, 0);
    graphics_context_set_fill_color(ctx, (GColor8){kThemeColors[s_theme % 3]});
    graphics_fill_rect(ctx, (GRect){{40, 40}, {100, 100}}, 0, 0);
}

static void on_pulse(void *data) {
    (void)data;
    s_pulse_timer = NULL;
    s_pulse_on = true;
    layer_mark_dirty(window_get_root_layer(s_home_win));
}

static void settings_down(ClickRecognizerRef rec, void *ctx) {
    (void)rec;
    (void)ctx;
    s_theme = (s_theme + 1) % 3;
    layer_mark_dirty(window_get_root_layer(s_settings_win));
}

static void settings_back_override(ClickRecognizerRef rec, void *ctx) {
    (void)rec;
    (void)ctx;
    /* First Back saves theme and requires second Back to pop (tests custom Back override). */
    if (!s_confirm_back) {
        s_confirm_back = true;
        persist_write_int(KEY_THEME, s_theme);
        layer_mark_dirty(window_get_root_layer(s_settings_win));
    } else {
        s_confirm_back = false;
        window_stack_pop(true);
    }
}

static void settings_click_config(void *ctx) {
    (void)ctx;
    window_single_click_subscribe(BUTTON_ID_DOWN, settings_down);
    window_single_click_subscribe(BUTTON_ID_BACK, settings_back_override);
}

static void home_select(ClickRecognizerRef rec, void *ctx) {
    (void)rec;
    (void)ctx;
    window_stack_push(s_settings_win, true);
}

static void home_click_config(void *ctx) {
    (void)ctx;
    window_single_click_subscribe(BUTTON_ID_SELECT, home_select);
}

static void home_load(Window *w) {
    if (persist_exists(KEY_THEME)) {
        int32_t saved = persist_read_int(KEY_THEME);
        if (saved >= 0 && saved < 3) {
            s_theme = saved;
        }
    }
    layer_set_update_proc(window_get_root_layer(w), home_draw);
    s_pulse_timer = app_timer_register(30, on_pulse, NULL);
}

static void home_unload(Window *w) {
    (void)w;
    if (s_pulse_timer != NULL) {
        app_timer_cancel(s_pulse_timer);
        s_pulse_timer = NULL;
    }
}

static void settings_load(Window *w) {
    s_confirm_back = false;
    layer_set_update_proc(window_get_root_layer(w), settings_draw);
}

int main(void) {
    s_home_win = window_create();
    window_set_background_color(s_home_win, (GColor8){0xC0});
    window_set_click_config_provider(s_home_win, home_click_config);
    window_set_window_handlers(s_home_win, (WindowHandlers){.load = home_load, .unload = home_unload});

    s_settings_win = window_create();
    window_set_background_color(s_settings_win, (GColor8){0xC0});
    window_set_click_config_provider(s_settings_win, settings_click_config);
    window_set_window_handlers(s_settings_win, (WindowHandlers){.load = settings_load});

    window_stack_push(s_home_win, false);
    app_event_loop();

    window_destroy(s_settings_win);
    window_destroy(s_home_win);
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="ThemeSettingsApp")
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)
            sandbox_dir = tmp_dir / "emu_sandbox"
            pbw_path = tmp_dir / "theme_settings.pbw"
            _pack_interactive_pbw(
                pbw_path,
                "chalk",
                app_bin,
                uuid="cccccccc-1111-2222-3333-444444444444",
                short_name="ThemeSettingsApp",
            )

            # Session 1: boot with default Blue theme, observe timer pulse, change theme to Magenta, exit
            waiter1 = _FrameWaiter()
            logs1: list[str] = []
            with AtlasHaloEmulator(
                sandbox_dir=sandbox_dir, print_handler=logs1.append, on_frame_shown=waiter1.on_frame
            ) as emu:
                emu.load_lua_source(
                    "main.lua",
                    f"frame.pebble.run('{pbw_path.as_posix()}', {{}})\nprint('DONE_SESSION_1')\n",
                )
                emu.start("main.lua")

                # Wait for timer pulse (Yellow bottom bar) with default Blue theme on top
                f_pulsed = waiter1.wait_for_predicate(
                    lambda f: _chalk_px(f, 90, 120) == (255, 255, 0), timeout=5.0
                )
                self.assertEqual(_chalk_px(f_pulsed, 90, 40), (0, 0, 255))

                # Open Settings via Select -> White outer border
                emu.inject_button_single()
                f_set = waiter1.wait_for_predicate(
                    lambda f: _chalk_px(f, 15, 15) == (255, 255, 255), timeout=5.0
                )
                self.assertEqual(_chalk_px(f_set, 90, 90), (0, 0, 255))

                # Two Single Temple Taps (DOWN) -> Blue -> Green -> Magenta
                emu.inject_imu_tap("single")
                waiter1.wait_for_predicate(
                    lambda f: _chalk_px(f, 15, 15) == (255, 255, 255) and _chalk_px(f, 90, 90) == (0, 255, 0),
                    timeout=5.0,
                )
                emu.inject_imu_tap("single")
                waiter1.wait_for_predicate(
                    lambda f: _chalk_px(f, 15, 15) == (255, 255, 255) and _chalk_px(f, 90, 90) == (255, 0, 255),
                    timeout=5.0,
                )

                # First Back triggers custom Back override (saves Magenta, stays on Settings)
                count_before_save = len(waiter1.frames)
                emu.inject_button_double()
                f_still_set = waiter1.wait_for_count(count_before_save + 1)
                self.assertEqual(_chalk_px(f_still_set, 15, 15), (255, 255, 255), "custom Back override should stay")

                # Second Back pops back to Home -> border Black, top theme bar Magenta
                emu.inject_button_double()
                f_home = waiter1.wait_for_predicate(
                    lambda f: _chalk_px(f, 15, 15) == (0, 0, 0) and _chalk_px(f, 90, 40) == (255, 0, 255),
                    timeout=5.0,
                )
                self.assertEqual(_chalk_px(f_home, 90, 40), (255, 0, 255))

                # Back from root Home exits cleanly to Lua
                emu.inject_button_double()
                emu.wait(timeout=5.0)
                self.assertIsNone(emu.get_error())
                self.assertIn("DONE_SESSION_1", logs1)

            # Session 2: fresh HaloEmulator restarts app -> immediately renders persisted Magenta theme!
            with AtlasHaloEmulator(sandbox_dir=sandbox_dir, print_handler=None) as emu:
                emu.connect()
                emu.execute_lua(f"frame.pebble.run('{pbw_path.as_posix()}', {{ max_frames = 1 }})")
                arr_reloaded = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(
                    _chalk_px(arr_reloaded, 90, 40),
                    (255, 0, 255),
                    "expected persisted Magenta theme on fresh restart",
                )


if __name__ == "__main__":
    unittest.main()
