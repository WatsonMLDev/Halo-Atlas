"""Tests for Seam 1: live watchface rendering inside official HaloEmulator via frame.pebble.run."""

import pathlib
import subprocess
import tempfile
import unittest
import zipfile

from halo_emulator import HaloEmulator
import numpy as np
from PIL import Image

from tests.pbw_builder import build_pebble_app_bin

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TEST_PBW = REPO_ROOT / "tests" / "fixtures" / "test-app.pbw"


def _pack_pbw(path: pathlib.Path, platform: str, app_bin: bytes) -> None:
    entry = "pebble-app.bin" if platform == "aplite" else f"{platform}/pebble-app.bin"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(entry, app_bin)


class HaloEmulatorWatchfaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        proc = subprocess.run(["make", "-C", str(REPO_ROOT), "runner"], capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"make runner failed:\n{proc.stdout}\n{proc.stderr}")

    def test_test_app_pbw_renders_non_blank_watchface_and_ticks_live_in_single_session(self) -> None:
        from halo_atlas.emulator import attach_pebble_runtime

        captured: list[np.ndarray] = []
        with HaloEmulator(print_handler=None) as emu:
            # Attach non-invasively BEFORE connect() to verify persistence across VM builds
            attach_pebble_runtime(emu, on_frame_shown=lambda img: captured.append(np.array(img.convert("RGB"))))
            emu.connect()
            pbw_lua_path = str(TEST_PBW).replace("\\", "/")

            # Single app_event_loop session advancing 15 minutes per frame across 3 frames
            frames = emu.execute_lua(
                f"return frame.pebble.run('{pbw_lua_path}', "
                "{ time_epoch = 1767236400, tick_step = 900, max_frames = 3 })"
            )
            self.assertEqual(frames, 3)
            self.assertEqual(len(captured), 3)

            arr1, arr3 = captured[0], captured[2]
            self.assertEqual(arr1.shape, (256, 256, 3))

            # Chalk (180x180) centered in 256x256 => x in [38, 218), y in [38, 218)
            center1 = arr1[38:218, 38:218]
            non_black = int(np.count_nonzero(center1.any(axis=2)))
            self.assertGreater(
                non_black,
                500,
                f"expected rich non-blank watchface pixels in centered 180x180 region, got {non_black}",
            )
            self.assertTrue(np.all(arr1[0:30, 0:30] == 0), "expected black letterbox outside chalk viewport")

            # Frame 3 (+30 minutes inside the same session) must visibly move the watch hands!
            diff_pixels = int(np.count_nonzero(np.any(arr1 != arr3, axis=2)))
            self.assertGreater(
                diff_pixels,
                50,
                f"expected watchface pixels to update across ticks in single session, diff={diff_pixels}",
            )

    def test_gcolor8_palette_centering_and_gabbro_2px_crop_through_verbatim_canvas(self) -> None:
        from halo_atlas.emulator import AtlasHaloEmulator

        c_src = r"""
#include <stdint.h>
#include <stdbool.h>

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
extern GRect layer_get_bounds(const Layer *);
extern void layer_set_update_proc(Layer *, LayerUpdateProc);
extern void graphics_context_set_fill_color(GContext *, GColor8);
extern void graphics_fill_rect(GContext *, GRect, uint16_t, uint8_t);
extern void app_event_loop(void);

static void draw_cb(Layer *layer, GContext *ctx) {
    GRect b = layer_get_bounds(layer);
    /* Outer 2px border in Blue (0xC3 => 0,0,255) — cropped off on gabbro (260x260)! */
    graphics_context_set_fill_color(ctx, (GColor8){0xC3});
    graphics_fill_rect(ctx, b, 0, 0);
    /* Inset 2px box in Red (0xF0 => 255,0,0) — maps to (0,0)..(255,255) on gabbro! */
    GRect inner = {{2, 2}, {(int16_t)(b.size.w - 4), (int16_t)(b.size.h - 4)}};
    graphics_context_set_fill_color(ctx, (GColor8){0xF0});
    graphics_fill_rect(ctx, inner, 0, 0);
    /* Center swatch in PictonBlue (0xDB = 0b11_01_10_11 => R=1*85=85, G=2*85=170, B=3*85=255) */
    GRect mid = {{(int16_t)(b.size.w / 2 - 10), (int16_t)(b.size.h / 2 - 10)}, {20, 20}};
    graphics_context_set_fill_color(ctx, (GColor8){0xDB});
    graphics_fill_rect(ctx, mid, 0, 0);
}

static void on_load(Window *w) {
    layer_set_update_proc(window_get_root_layer(w), draw_cb);
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
        app_bin = build_pebble_app_bin(c_src, app_name="FramingColorTest")

        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)
            gabbro_pbw = tmp_dir / "gabbro.pbw"
            basalt_pbw = tmp_dir / "basalt.pbw"
            _pack_pbw(gabbro_pbw, "gabbro", app_bin)
            _pack_pbw(basalt_pbw, "basalt", app_bin)

            with AtlasHaloEmulator(print_handler=None) as emu:
                emu.connect()

                emu.execute_lua(f"frame.pebble.run('{gabbro_pbw.as_posix()}', {{ max_frames = 1 }})")
                arr_g = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(tuple(int(c) for c in arr_g[0, 0]), (255, 0, 0))
                self.assertEqual(tuple(int(c) for c in arr_g[255, 255]), (255, 0, 0))
                self.assertEqual(tuple(int(c) for c in arr_g[128, 128]), (85, 170, 255))

                emu.execute_lua(f"frame.pebble.run('{basalt_pbw.as_posix()}', {{ max_frames = 1 }})")
                arr_b = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(tuple(int(c) for c in arr_b[44, 55]), (0, 0, 0))
                self.assertEqual(tuple(int(c) for c in arr_b[44, 56]), (0, 0, 255))
                self.assertEqual(tuple(int(c) for c in arr_b[46, 58]), (255, 0, 0))
                self.assertEqual(tuple(int(c) for c in arr_b[128, 128]), (85, 170, 255))

    def test_text_layer_and_gpath_watchface_renders_and_records_gif_and_mp4(self) -> None:
        from halo_atlas.cli import main as cli_main
        from halo_atlas.emulator import AtlasHaloEmulator

        c_src = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint8_t argb; } GColor8;
typedef struct { int16_t x; int16_t y; } GPoint;
typedef struct { int16_t w; int16_t h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct { uint32_t num_points; GPoint *points; } GPathInfo;
typedef struct GPath GPath;
typedef struct Window Window;
typedef struct Layer Layer;
typedef struct TextLayer TextLayer;
typedef struct GContext GContext;
typedef void *GFont;
typedef struct {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst, tm_gmtoff;
    char tm_zone[8];
} tm_t;
typedef void (*WindowHandler)(Window *);
typedef void (*LayerUpdateProc)(Layer *, GContext *);
typedef void (*TickHandler)(tm_t *, uint32_t);
typedef struct { WindowHandler load, appear, disappear, unload; } WindowHandlers;

extern Window *window_create(void);
extern void window_destroy(Window *);
extern Layer *window_get_root_layer(const Window *);
extern void window_set_window_handlers(Window *, WindowHandlers);
extern void window_set_background_color(Window *, GColor8);
extern void window_stack_push(Window *, bool);
extern Layer *layer_create(GRect);
extern void layer_destroy(Layer *);
extern void layer_add_child(Layer *, Layer *);
extern void layer_set_update_proc(Layer *, LayerUpdateProc);
extern TextLayer *text_layer_create(GRect);
extern void text_layer_destroy(TextLayer *);
extern Layer *text_layer_get_layer(TextLayer *);
extern void text_layer_set_text(TextLayer *, const char *);
extern void text_layer_set_background_color(TextLayer *, GColor8);
extern void text_layer_set_text_color(TextLayer *, GColor8);
extern void text_layer_set_font(TextLayer *, GFont);
extern void text_layer_set_text_alignment(TextLayer *, uint8_t);
extern GFont fonts_get_system_font(const char *);
extern GPath *gpath_create(const GPathInfo *);
extern void gpath_destroy(GPath *);
extern void gpath_move_to(GPath *, GPoint);
extern void gpath_draw_filled(GContext *, GPath *);
extern void graphics_context_set_fill_color(GContext *, GColor8);
extern void tick_timer_service_subscribe(uint32_t, TickHandler);
extern size_t strftime(char *, size_t, const char *, const tm_t *);
extern void app_event_loop(void);

static TextLayer *s_text;
static Layer *s_poly_layer;
static GPath *s_tri;
static char s_buf[16];
static GPoint s_tri_pts[3] = {{0, -20}, {20, 20}, {-20, 20}};
static GPathInfo s_tri_info = {3, s_tri_pts};

static void poly_draw(Layer *l, GContext *ctx) {
    (void)l;
    graphics_context_set_fill_color(ctx, (GColor8){0xCC}); /* Green (0,255,0) */
    gpath_draw_filled(ctx, s_tri);
}

static void on_tick(tm_t *t, uint32_t units) {
    (void)units;
    strftime(s_buf, sizeof(s_buf), "%H:%M", t);
    /* Subscribe happens BEFORE window_stack_push; s_text must already be created by on_load. */
    text_layer_set_text(s_text, s_buf);
}

static void on_load(Window *w) {
    Layer *root = window_get_root_layer(w);
    s_poly_layer = layer_create((GRect){{0, 0}, {180, 180}});
    layer_set_update_proc(s_poly_layer, poly_draw);
    layer_add_child(root, s_poly_layer);

    s_tri = gpath_create(&s_tri_info);
    gpath_move_to(s_tri, (GPoint){90, 45});

    s_text = text_layer_create((GRect){{10, 90}, {160, 40}});
    text_layer_set_background_color(s_text, (GColor8){0x00});
    text_layer_set_text_color(s_text, (GColor8){0xFF});
    text_layer_set_font(s_text, fonts_get_system_font("RESOURCE_ID_GOTHIC_24_BOLD"));
    text_layer_set_text_alignment(s_text, 1);
    layer_add_child(root, text_layer_get_layer(s_text));
}

int main(void) {
    /* Subscribe BEFORE pushing window — verifies initial tick defers until app_event_loop. */
    tick_timer_service_subscribe(1u << 1, on_tick);
    Window *w = window_create();
    window_set_background_color(w, (GColor8){0xC0});
    window_set_window_handlers(w, (WindowHandlers){.load = on_load});
    window_stack_push(w, false);
    app_event_loop();
    gpath_destroy(s_tri);
    text_layer_destroy(s_text);
    layer_destroy(s_poly_layer);
    window_destroy(w);
    return 0;
}
"""
        app_bin = build_pebble_app_bin(c_src, app_name="TextGPathClock")

        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = pathlib.Path(tmp)
            pbw_path = tmp_dir / "clock.pbw"
            _pack_pbw(pbw_path, "chalk", app_bin)

            with AtlasHaloEmulator(print_handler=None) as emu:
                emu.connect()
                frames = emu.execute_lua(
                    f"return frame.pebble.run('{pbw_path.as_posix()}', "
                    "{ time_epoch = 1767236400, tick_step = 60, max_frames = 2 })"
                )
                self.assertEqual(frames, 2)
                arr = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(tuple(int(c) for c in arr[83, 128]), (0, 255, 0))
                text_band = arr[128:168, 48:208]
                white_px = int(np.count_nonzero(np.all(text_band == [255, 255, 255], axis=2)))
                self.assertGreater(white_px, 40, f"expected rendered white text pixels, got {white_px}")

            # Verify headless CLI recording for both .gif and .mp4 on unmodified test-app.pbw
            gif_out = tmp_dir / "watchface.gif"
            rc_gif = cli_main(
                [
                    str(TEST_PBW),
                    "--headless",
                    "--time-epoch",
                    "1767236400",
                    "--tick-step",
                    "600",
                    "--max-frames",
                    "3",
                    "--record",
                    str(gif_out),
                    "--fps",
                    "10",
                ]
            )
            self.assertEqual(rc_gif, 0)
            self.assertTrue(gif_out.is_file() and gif_out.stat().st_size > 100)
            with Image.open(gif_out) as gif_img:
                self.assertEqual(gif_img.format, "GIF")
                self.assertEqual(gif_img.size, (256, 256))
                # Corner (0, 0) must show Halo's circular bezel mask (24, 24, 24), not raw pixels
                corner = gif_img.convert("RGB").getpixel((0, 0))
                self.assertEqual(corner, (24, 24, 24))

            mp4_out = tmp_dir / "watchface.mp4"
            rc_mp4 = cli_main(
                [
                    str(TEST_PBW),
                    "--headless",
                    "--time-epoch",
                    "1767236400",
                    "--tick-step",
                    "600",
                    "--max-frames",
                    "3",
                    "--record",
                    str(mp4_out),
                    "--fps",
                    "10",
                ]
            )
            self.assertEqual(rc_mp4, 0)
            self.assertTrue(mp4_out.is_file() and mp4_out.stat().st_size > 1000)


if __name__ == "__main__":
    unittest.main()
