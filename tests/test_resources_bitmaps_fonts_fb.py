"""Tests at the HaloEmulator seam for Ticket #3: .pbpack resources, bitmaps, fonts, and framebuffer capture."""

from __future__ import annotations

import io
import pathlib
import struct
import subprocess
import tempfile
import unittest
import zipfile
import zlib

import numpy as np
from PIL import Image

from halo_atlas.emulator import AtlasHaloEmulator
from tests.pbw_builder import build_pebble_app_bin

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TEST_PBW = REPO_ROOT / "tests" / "fixtures" / "test-app.pbw"


def _build_pbpack(entries: list[bytes]) -> bytes:
    """Build a standard Pebble app_resources.pbpack (12-byte header + 256*16 table + data)."""
    if len(entries) > 256:
        raise ValueError("app_resources.pbpack supports at most 256 entries")
    table = bytearray(256 * 16)
    data = bytearray()
    for idx, payload in enumerate(entries):
        res_id = idx + 1
        struct.pack_into("<IIII", table, idx * 16, res_id, len(data), len(payload), 0)
        data.extend(payload)
    header = struct.pack("<III", len(entries), 0, 0)
    return header + bytes(table) + bytes(data)


def _build_pbi_1bit(width: int, height: int, pixels: list[list[int]]) -> bytes:
    """Build a Pebble 1-bit PBI (LSB-first, 4-byte aligned rows)."""
    row_bytes = ((width + 31) // 32) * 4
    info_flags = (1 << 12) | (0 << 1)  # version 1, GBitmapFormat1Bit (0)
    hdr = struct.pack("<HHhhhh", row_bytes, info_flags, 0, 0, width, height)
    body = bytearray(row_bytes * height)
    for y in range(height):
        for x in range(width):
            if pixels[y][x]:
                body[y * row_bytes + (x >> 3)] |= 1 << (x & 7)
    return hdr + bytes(body)


def _build_pbi_2bit_palette(
    width: int,
    height: int,
    indices: list[list[int]],
    palette_argb8: list[int],
) -> bytes:
    """Build a Pebble 2-bit palettized PBI (MSB-first, byte-aligned rows, 4-byte palette)."""
    assert len(palette_argb8) == 4
    row_bytes = (width + 3) // 4
    info_flags = (1 << 12) | (3 << 1)  # version 1, GBitmapFormat2BitPalette (3)
    hdr = struct.pack("<HHhhhh", row_bytes, info_flags, 0, 0, width, height)
    body = bytearray(row_bytes * height)
    for y in range(height):
        for x in range(width):
            shift = 6 - 2 * (x & 3)
            body[y * row_bytes + (x >> 2)] |= (indices[y][x] & 0x3) << shift
    return hdr + bytes(body) + bytes(palette_argb8)


def _png_chunk(chunk_type: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(chunk_type + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + chunk_type + payload + struct.pack(">I", crc)


def _build_indexed_2bit_png_with_trns() -> bytes:
    """Build an 8x8 2-bit indexed PNG with PLTE + tRNS (idx 0 transparent, 1=Red, 2=Cyan, 3=White)."""
    w, h = 8, 8
    ihdr = struct.pack(">IIBBBBB", w, h, 2, 3, 0, 0, 0)
    plte = bytes([
        0x00, 0x00, 0x00,  # 0: transparent
        0xFF, 0x00, 0x00,  # 1: Red
        0x00, 0xFF, 0xFF,  # 2: Cyan
        0xFF, 0xFF, 0xFF,  # 3: White
    ])
    trns = bytes([0x00, 0xFF, 0xFF, 0xFF])
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # Filter None
        row = bytearray(2)
        for x in range(w):
            if y < 4:
                idx = 0 if x < 4 else 1  # TL transparent, TR red
            else:
                idx = 2 if x < 4 else 3  # BL cyan, BR white
            row[x >> 2] |= (idx & 3) << (6 - 2 * (x & 3))
        raw.extend(row)
    idat = zlib.compress(bytes(raw))
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"PLTE", plte)
        + _png_chunk(b"tRNS", trns)
        + _png_chunk(b"IDAT", idat)
        + _png_chunk(b"IEND", b"")
    )


def _build_rgba_png() -> bytes:
    """Build an 8x8 RGBA PNG via Pillow (exercises real PNG filters + alpha)."""
    img = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    for y in range(8):
        for x in range(8):
            if y < 4 and x < 4:
                img.putpixel((x, y), (0, 0, 0, 0))  # transparent
            elif y < 4:
                img.putpixel((x, y), (255, 0, 255, 255))  # opaque Magenta
            elif x < 4:
                img.putpixel((x, y), (0, 255, 0, 170))  # 66% Green (alpha=2)
            else:
                img.putpixel((x, y), (85, 170, 255, 255))  # opaque PictonBlue
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def _extract_test_app_icon_png() -> bytes:
    with zipfile.ZipFile(TEST_PBW, "r") as z:
        pb = z.read("chalk/app_resources.pbpack")
    _rid, off, length, _crc = struct.unpack_from("<IIII", pb, 12)
    return pb[4108 + off : 4108 + off + length]


def _build_pfo_v2_font(
    max_height: int,
    glyphs: dict[int, tuple[int, int, int, int, int, list[int]]],
    wildcard_cp: int = 0x25AF,
) -> bytes:
    """Build a real Pebble PFO v2 binary font.

    glyphs maps codepoint -> (w, h, offset_left, offset_top, horiz_advance, bits_row_major).
    """
    ht_size = 255
    cp_bytes = 2
    buckets: list[list[int]] = [[] for _ in range(ht_size)]
    for cp in sorted(glyphs.keys()):
        buckets[cp % ht_size].append(cp)

    hash_table = bytearray(ht_size * 4)
    offset_table = bytearray()
    glyph_table = bytearray()

    for b in range(ht_size):
        cps = buckets[b]
        if not cps:
            continue
        struct.pack_into("<BBH", hash_table, b * 4, b, len(cps), len(offset_table))
        for cp in cps:
            w, h, left, top, adv, bits = glyphs[cp]
            goff = len(glyph_table)
            offset_table.extend(struct.pack("<HI", cp, goff))
            glyph_table.extend(struct.pack("<BBbbb", w, h, left, top, adv))
            nbytes = (w * h + 7) // 8
            gdata = bytearray(nbytes)
            for i, bit in enumerate(bits):
                if bit:
                    gdata[i >> 3] |= 1 << (i & 7)
            glyph_table.extend(gdata)

    header = struct.pack("<BBHHBB", 2, max_height, len(glyphs), wildcard_cp, ht_size, cp_bytes)
    return header + bytes(hash_table) + bytes(offset_table) + bytes(glyph_table)


def _encode_rle4_bits(bits: list[int]) -> tuple[int, bytes]:
    """Encode a bit list into Pebble RLE4 4-bit units: [symbol:1][length-1:3], low nibble first."""
    units: list[int] = []
    i = 0
    while i < len(bits):
        sym = bits[i] & 1
        run = 1
        while i + run < len(bits) and (bits[i + run] & 1) == sym and run < 8:
            run += 1
        units.append((sym << 3) | (run - 1))
        i += run
    out = bytearray((len(units) + 1) // 2)
    for idx, u in enumerate(units):
        if (idx & 1) == 0:
            out[idx >> 1] |= u & 0x0F
        else:
            out[idx >> 1] |= (u & 0x0F) << 4
    return len(units), bytes(out)


def _build_pfo_v3_rle4_font(
    max_height: int,
    glyphs: dict[int, tuple[int, int, int, int, int, list[int]]],
    wildcard_cp: int = 0x25AF,
) -> bytes:
    """Build a Pebble PFO v3 binary font with FEATURE_OFFSET_16 (0x01) | FEATURE_RLE4 (0x02)."""
    ht_size = 255
    cp_bytes = 2
    hdr_size = 10
    features = 0x01 | 0x02  # 16-bit offsets + RLE4
    buckets: list[list[int]] = [[] for _ in range(ht_size)]
    for cp in sorted(glyphs.keys()):
        buckets[cp % ht_size].append(cp)

    hash_table = bytearray(ht_size * 4)
    offset_table = bytearray()
    glyph_table = bytearray()

    for b in range(ht_size):
        cps = buckets[b]
        if not cps:
            continue
        struct.pack_into("<BBH", hash_table, b * 4, b, len(cps), len(offset_table))
        for cp in cps:
            w, _h, left, top, adv, bits = glyphs[cp]
            goff = len(glyph_table)
            offset_table.extend(struct.pack("<HH", cp, goff))
            num_units, rle_bytes = _encode_rle4_bits(bits)
            glyph_table.extend(struct.pack("<BBbbb", w, num_units, left, top, adv))
            glyph_table.extend(rle_bytes)

    header = struct.pack(
        "<BBHHBBBB", 3, max_height, len(glyphs), wildcard_cp, ht_size, cp_bytes, hdr_size, features
    )
    return header + bytes(hash_table) + bytes(offset_table) + bytes(glyph_table)


def _pack_pbw_with_resources(
    path: pathlib.Path,
    platform: str,
    app_bin: bytes,
    pbpack: bytes | None = None,
) -> None:
    bin_entry = "pebble-app.bin" if platform == "aplite" else f"{platform}/pebble-app.bin"
    res_entry = "app_resources.pbpack" if platform == "aplite" else f"{platform}/app_resources.pbpack"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(bin_entry, app_bin)
        if pbpack is not None:
            z.writestr(res_entry, pbpack)


class ResourceBitmapFontFramebufferTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        proc = subprocess.run(["make", "-C", str(REPO_ROOT), "runner"], capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"make runner failed:\n{proc.stdout}\n{proc.stderr}")

    def test_pbpack_raw_resources_load_size_and_byte_range_in_emulator(self) -> None:
        """App reads resource_get_handle/size/load/load_byte_range from app_resources.pbpack."""
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
typedef void *ResHandle;
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
extern ResHandle resource_get_handle(uint32_t resource_id);
extern size_t resource_size(ResHandle h);
extern size_t resource_load(ResHandle h, uint8_t *buffer, size_t max_length);
extern size_t resource_load_byte_range(ResHandle h, uint32_t start_offset, uint8_t *buffer, size_t num_bytes);
extern void app_event_loop(void);

static uint8_t s_color_ok_size = 0xF0;   /* Red = fail, Green (0xCC) = pass */
static uint8_t s_color_ok_load = 0xF0;
static uint8_t s_color_ok_range = 0xF0;
static uint8_t s_color_ok_missing = 0xF0;

static void draw_cb(Layer *layer, GContext *ctx) {
    (void)layer;
    graphics_context_set_fill_color(ctx, (GColor8){s_color_ok_size});
    graphics_fill_rect(ctx, (GRect){{10, 10}, {30, 30}}, 0, 0);

    graphics_context_set_fill_color(ctx, (GColor8){s_color_ok_load});
    graphics_fill_rect(ctx, (GRect){{50, 10}, {30, 30}}, 0, 0);

    graphics_context_set_fill_color(ctx, (GColor8){s_color_ok_range});
    graphics_fill_rect(ctx, (GRect){{90, 10}, {30, 30}}, 0, 0);

    graphics_context_set_fill_color(ctx, (GColor8){s_color_ok_missing});
    graphics_fill_rect(ctx, (GRect){{130, 10}, {30, 30}}, 0, 0);
}

static void on_load(Window *w) {
    ResHandle h1 = resource_get_handle(1);
    ResHandle h2 = resource_get_handle(2);
    ResHandle h_bad = resource_get_handle(99);

    if (h1 != NULL && h2 != NULL && resource_size(h1) == 10 && resource_size(h2) == 6) {
        s_color_ok_size = 0xCC; /* Green */
    }

    uint8_t buf1[16];
    size_t n1 = resource_load(h1, buf1, sizeof(buf1));
    if (n1 == 10 && buf1[0] == 'H' && buf1[4] == '-' && buf1[9] == '!') {
        s_color_ok_load = 0xCC; /* Green */
    }

    uint8_t sub[8];
    size_t n2 = resource_load_byte_range(h1, 5, sub, 4);
    /* Also verify clamping past end of resource 2 (offset 4, ask 10 -> gets 2) */
    uint8_t tail[8];
    size_t n3 = resource_load_byte_range(h2, 4, tail, 10);
    if (n2 == 4 && sub[0] == 'A' && sub[1] == 'T' && sub[2] == 'L' && sub[3] == 'S' &&
        n3 == 2 && tail[0] == 0x55 && tail[1] == 0xAA) {
        s_color_ok_range = 0xCC; /* Green */
    }

    if (h_bad == NULL && resource_size(h_bad) == 0 && resource_load(h_bad, buf1, 4) == 0) {
        s_color_ok_missing = 0xCC; /* Green */
    }

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
        app_bin = build_pebble_app_bin(c_src, app_name="ResRawTest")
        pbpack = _build_pbpack([b"HALO-ATLS!", bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0xAA])])

        with tempfile.TemporaryDirectory() as tmp:
            pbw_path = pathlib.Path(tmp) / "res_raw.pbw"
            _pack_pbw_with_resources(pbw_path, "chalk", app_bin, pbpack)

            with AtlasHaloEmulator(print_handler=None) as emu:
                emu.connect()
                frames = emu.execute_lua(f"return frame.pebble.run('{pbw_path.as_posix()}', {{ max_frames = 1 }})")
                self.assertEqual(frames, 1)
                arr = np.array(emu.get_framebuffer().convert("RGB"))
                # Chalk 180x180 centered at (+38, +38)
                self.assertEqual(tuple(int(c) for c in arr[38 + 20, 38 + 20]), (0, 255, 0), "resource_size failed")
                self.assertEqual(tuple(int(c) for c in arr[38 + 20, 38 + 60]), (0, 255, 0), "resource_load failed")
                self.assertEqual(
                    tuple(int(c) for c in arr[38 + 20, 38 + 100]), (0, 255, 0), "resource_load_byte_range failed"
                )
                self.assertEqual(
                    tuple(int(c) for c in arr[38 + 20, 38 + 140]), (0, 255, 0), "missing resource handling failed"
                )

    def test_pbi_and_png_bitmaps_and_bitmap_layer_transparency_and_palettes(self) -> None:
        """App decodes PBI & PNG resources (with transparency & palettes) via GBitmap and BitmapLayer."""
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
typedef struct BitmapLayer BitmapLayer;
typedef struct GBitmap GBitmap;
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
extern Layer *layer_create(GRect);
extern void layer_destroy(Layer *);
extern void layer_add_child(Layer *, Layer *);
extern void layer_set_update_proc(Layer *, LayerUpdateProc);
extern GBitmap *gbitmap_create_with_resource(uint32_t resource_id);
extern GBitmap *gbitmap_create_as_sub_bitmap(const GBitmap *base_bitmap, GRect sub_rect);
extern void gbitmap_destroy(GBitmap *);
extern GColor8 *gbitmap_get_palette(const GBitmap *);
extern GRect gbitmap_get_bounds(const GBitmap *);
extern BitmapLayer *bitmap_layer_create(GRect);
extern void bitmap_layer_destroy(BitmapLayer *);
extern Layer *bitmap_layer_get_layer(const BitmapLayer *);
extern void bitmap_layer_set_bitmap(BitmapLayer *, const GBitmap *);
extern void bitmap_layer_set_alignment(BitmapLayer *, uint8_t);
extern void bitmap_layer_set_background_color(BitmapLayer *, GColor8);
extern void bitmap_layer_set_compositing_mode(BitmapLayer *, uint8_t);
extern void graphics_context_set_compositing_mode(GContext *, uint8_t);
extern void graphics_draw_bitmap_in_rect(GContext *, const GBitmap *, GRect);
extern void app_event_loop(void);

static GBitmap *s_pbi_pal;
static GBitmap *s_pbi_1bit;
static GBitmap *s_pbi_1bit_sub;
static GBitmap *s_png_idx;
static GBitmap *s_png_rgba;
static GBitmap *s_png_real;
static BitmapLayer *s_bl_pbi_pal;
static BitmapLayer *s_bl_png_idx;
static BitmapLayer *s_bl_png_rgba;
static BitmapLayer *s_bl_png_real;
static Layer *s_custom_layer;

static void custom_draw(Layer *l, GContext *ctx) {
    (void)l;
    /* Draw 1-bit PBI (8x4) at (10, 60) with GCompOpAssign (0) */
    graphics_context_set_compositing_mode(ctx, 0);
    graphics_draw_bitmap_in_rect(ctx, s_pbi_1bit, (GRect){{10, 60}, {8, 4}});
    /* Draw right-half sub-bitmap (4x4) at (30, 60) */
    graphics_draw_bitmap_in_rect(ctx, s_pbi_1bit_sub, (GRect){{30, 60}, {4, 4}});
}

static void on_load(Window *w) {
    Layer *root = window_get_root_layer(w);

    /* 1. 2-bit palettized PBI (8x8) via BitmapLayer at (10, 10) size (16, 16), GAlignCenter (0), GCompOpSet (5), Yellow bg (0xFC) */
    s_pbi_pal = gbitmap_create_with_resource(1);
    s_bl_pbi_pal = bitmap_layer_create((GRect){{10, 10}, {16, 16}});
    bitmap_layer_set_background_color(s_bl_pbi_pal, (GColor8){0xFC});
    bitmap_layer_set_alignment(s_bl_pbi_pal, 0); /* GAlignCenter -> bitmap at (14, 14)..(21, 21) */
    bitmap_layer_set_compositing_mode(s_bl_pbi_pal, 5); /* GCompOpSet */
    bitmap_layer_set_bitmap(s_bl_pbi_pal, s_pbi_pal);
    layer_add_child(root, bitmap_layer_get_layer(s_bl_pbi_pal));

    /* 2. 1-bit PBI + sub-bitmap on custom Layer */
    s_pbi_1bit = gbitmap_create_with_resource(2);
    s_pbi_1bit_sub = gbitmap_create_as_sub_bitmap(s_pbi_1bit, (GRect){{4, 0}, {4, 4}});
    s_custom_layer = layer_create((GRect){{0, 0}, {180, 180}});
    layer_set_update_proc(s_custom_layer, custom_draw);
    layer_add_child(root, s_custom_layer);

    /* 3. 2-bit indexed PNG with tRNS (8x8) at (40, 10), GAlignTopLeft (1), GCompOpSet (5), Blue bg (0xC3) */
    s_png_idx = gbitmap_create_with_resource(3);
    /* Mutate palette entry 3 (White -> Orange 0xF8 = R3 G2 B0 = 255,170,0) to verify live palette! */
    GColor8 *pal = gbitmap_get_palette(s_png_idx);
    if (pal != NULL) {
        pal[3].argb = 0xF8;
    }
    s_bl_png_idx = bitmap_layer_create((GRect){{40, 10}, {16, 16}});
    bitmap_layer_set_background_color(s_bl_png_idx, (GColor8){0xC3});
    bitmap_layer_set_alignment(s_bl_png_idx, 1); /* GAlignTopLeft -> bitmap at (40, 10)..(47, 17) */
    bitmap_layer_set_compositing_mode(s_bl_png_idx, 5);
    bitmap_layer_set_bitmap(s_bl_png_idx, s_png_idx);
    layer_add_child(root, bitmap_layer_get_layer(s_bl_png_idx));

    /* 4. 8-bit RGBA PNG (8x8) at (70, 10), GAlignTopLeft (1), GCompOpSet (5), Black bg (0xC0) */
    s_png_rgba = gbitmap_create_with_resource(4);
    s_bl_png_rgba = bitmap_layer_create((GRect){{70, 10}, {8, 8}});
    bitmap_layer_set_background_color(s_bl_png_rgba, (GColor8){0xC0});
    bitmap_layer_set_alignment(s_bl_png_rgba, 1);
    bitmap_layer_set_compositing_mode(s_bl_png_rgba, 5);
    bitmap_layer_set_bitmap(s_bl_png_rgba, s_png_rgba);
    layer_add_child(root, bitmap_layer_get_layer(s_bl_png_rgba));

    /* 5. Real Pebble Store PNG icon (28x28) from test-app.pbw at (100, 10) */
    s_png_real = gbitmap_create_with_resource(5);
    s_bl_png_real = bitmap_layer_create((GRect){{100, 10}, {28, 28}});
    bitmap_layer_set_alignment(s_bl_png_real, 1);
    bitmap_layer_set_compositing_mode(s_bl_png_real, 0);
    bitmap_layer_set_bitmap(s_bl_png_real, s_png_real);
    layer_add_child(root, bitmap_layer_get_layer(s_bl_png_real));
}

int main(void) {
    Window *w = window_create();
    window_set_background_color(w, (GColor8){0xC0});
    window_set_window_handlers(w, (WindowHandlers){.load = on_load});
    window_stack_push(w, false);
    app_event_loop();
    bitmap_layer_destroy(s_bl_pbi_pal);
    bitmap_layer_destroy(s_bl_png_idx);
    bitmap_layer_destroy(s_bl_png_rgba);
    bitmap_layer_destroy(s_bl_png_real);
    layer_destroy(s_custom_layer);
    gbitmap_destroy(s_pbi_1bit_sub);
    gbitmap_destroy(s_pbi_1bit);
    gbitmap_destroy(s_pbi_pal);
    gbitmap_destroy(s_png_idx);
    gbitmap_destroy(s_png_rgba);
    gbitmap_destroy(s_png_real);
    window_destroy(w);
    return 0;
}
"""
        # 1. 8x8 2-bit palettized PBI: TL=0(clear), TR=1(red), BL=2(green), BR=3(blue)
        pal_indices = [
            [(0 if x < 4 else 1) if y < 4 else (2 if x < 4 else 3) for x in range(8)]
            for y in range(8)
        ]
        pbi_pal = _build_pbi_2bit_palette(8, 8, pal_indices, [0x00, 0xF0, 0xCC, 0xC3])

        # 2. 8x4 1-bit PBI: left half black (0), right half white (1)
        pbi_1bit = _build_pbi_1bit(8, 4, [[0, 0, 0, 0, 1, 1, 1, 1] for _ in range(4)])

        # 3-5. PNG fixtures
        png_idx = _build_indexed_2bit_png_with_trns()
        png_rgba = _build_rgba_png()
        png_real = _extract_test_app_icon_png()

        app_bin = build_pebble_app_bin(c_src, app_name="BitmapTest")
        pbpack = _build_pbpack([pbi_pal, pbi_1bit, png_idx, png_rgba, png_real])

        with tempfile.TemporaryDirectory() as tmp:
            pbw_path = pathlib.Path(tmp) / "bitmaps.pbw"
            _pack_pbw_with_resources(pbw_path, "chalk", app_bin, pbpack)

            with AtlasHaloEmulator(print_handler=None) as emu:
                emu.connect()
                frames = emu.execute_lua(f"return frame.pebble.run('{pbw_path.as_posix()}', {{ max_frames = 1 }})")
                self.assertEqual(frames, 1)
                arr = np.array(emu.get_framebuffer().convert("RGB"))

                def px(x: int, y: int) -> tuple[int, int, int]:
                    r, g, b = arr[38 + y, 38 + x]
                    return (int(r), int(g), int(b))

                # 1. PBI 2-bit palette centered at (14, 14) inside (10, 10, 16, 16) Yellow bg
                self.assertEqual(px(11, 11), (255, 255, 0), "BitmapLayer Yellow border")
                self.assertEqual(px(15, 15), (255, 255, 0), "PBI transparent idx 0 must reveal Yellow bg")
                self.assertEqual(px(19, 15), (255, 0, 0), "PBI idx 1 must be Red")
                self.assertEqual(px(15, 19), (0, 255, 0), "PBI idx 2 must be Green")
                self.assertEqual(px(19, 19), (0, 0, 255), "PBI idx 3 must be Blue")

                # 2. 1-bit PBI at (10, 60) + sub-bitmap of right half at (30, 60)
                self.assertEqual(px(11, 61), (0, 0, 0), "1-bit PBI left half black")
                self.assertEqual(px(15, 61), (255, 255, 255), "1-bit PBI right half white")
                self.assertEqual(px(31, 61), (255, 255, 255), "1-bit sub-bitmap must render right half white")

                # 3. Indexed 2-bit PNG with tRNS at (40, 10) over Blue bg + mutated palette[3] -> Orange
                self.assertEqual(px(41, 11), (0, 0, 255), "PNG tRNS transparent idx 0 must reveal Blue bg")
                self.assertEqual(px(45, 11), (255, 0, 0), "PNG PLTE idx 1 must be Red")
                self.assertEqual(px(41, 15), (0, 255, 255), "PNG PLTE idx 2 must be Cyan")
                self.assertEqual(px(45, 15), (255, 170, 0), "PNG mutated palette[3] must be Orange")

                # 4. RGBA PNG at (70, 10) over Black bg
                self.assertEqual(px(71, 11), (0, 0, 0), "RGBA PNG transparent reveals Black bg")
                self.assertEqual(px(75, 11), (255, 0, 255), "RGBA PNG opaque Magenta")
                self.assertEqual(px(71, 15), (0, 170, 0), "RGBA PNG 66% Green blended over Black -> (0, 170, 0)")
                self.assertEqual(px(75, 15), (85, 170, 255), "RGBA PNG PictonBlue")

                # 5. Real 28x28 PNG icon from test-app.pbw at (100, 10) must match reference decoded PNG!
                ref_icon = np.array(Image.open(io.BytesIO(png_real)).convert("RGB"))
                rendered_icon = arr[38 + 10 : 38 + 38, 38 + 100 : 38 + 128]
                # Every pixel in test-app.pbw's icon uses exact Pebble 2-bit palette values (0x00, 0x55, 0xFF)
                self.assertTrue(
                    np.array_equal(rendered_icon, ref_icon),
                    "Real test-app.pbw PNG icon did not match Pillow reference pixel-for-pixel!",
                )

    def test_custom_pfo_v2_v3_fonts_and_system_font_fallback(self) -> None:
        """App renders authentic Pebble PFO v2 and v3 RLE4 custom font glyphs and system font fallback."""
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
typedef void *ResHandle;
typedef void *GFont;
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
extern ResHandle resource_get_handle(uint32_t);
extern GFont fonts_load_custom_font(ResHandle);
extern void fonts_unload_custom_font(GFont);
extern GFont fonts_get_system_font(const char *);
extern void graphics_context_set_text_color(GContext *, GColor8);
extern void graphics_context_set_fill_color(GContext *, GColor8);
extern void graphics_fill_rect(GContext *, GRect, uint16_t, uint8_t);
extern void graphics_draw_text(GContext *, const char *, GFont, GRect, uint8_t, uint8_t, void *);
extern GSize graphics_text_layout_get_content_size(const char *, GFont, GRect, uint8_t, uint8_t);
extern void app_event_loop(void);

static GFont s_font_v2;
static GFont s_font_v3;
static GFont s_font_sys;

static void draw_cb(Layer *l, GContext *ctx) {
    (void)l;
    /* Draw custom PFO v2 glyph 'X' (6x6, left=0, top=2, adv=8, max_h=10) at (10, 10) in Red */
    graphics_context_set_text_color(ctx, (GColor8){0xF0});
    graphics_draw_text(ctx, "X", s_font_v2, (GRect){{10, 10}, {40, 20}}, 0, 0, NULL);

    /* Draw custom PFO v3 RLE4 glyph '+' (5x5, left=0, top=1, adv=7, max_h=8) at (40, 10) in Green */
    graphics_context_set_text_color(ctx, (GColor8){0xCC});
    graphics_draw_text(ctx, "+", s_font_v3, (GRect){{40, 10}, {40, 20}}, 0, 0, NULL);

    /* Measure "XX" in s_font_v2: width must be 16 (2*8), height must be 10 (max_h) */
    GSize sz = graphics_text_layout_get_content_size("XX", s_font_v2, (GRect){{0, 0}, {100, 100}}, 0, 0);
    if (sz.w == 16 && sz.h == 10) {
        graphics_context_set_fill_color(ctx, (GColor8){0xC3}); /* Blue = pass */
        graphics_fill_rect(ctx, (GRect){{100, 10}, {20, 20}}, 0, 0);
    }

    /* Draw system font fallback at (10, 50) in White */
    graphics_context_set_text_color(ctx, (GColor8){0xFF});
    graphics_draw_text(ctx, "HI", s_font_sys, (GRect){{10, 50}, {100, 40}}, 0, 0, NULL);
}

static void on_load(Window *w) {
    s_font_v2 = fonts_load_custom_font(resource_get_handle(1));
    s_font_v3 = fonts_load_custom_font(resource_get_handle(2));
    s_font_sys = fonts_get_system_font("RESOURCE_ID_GOTHIC_24_BOLD");
    layer_set_update_proc(window_get_root_layer(w), draw_cb);
}

int main(void) {
    Window *w = window_create();
    window_set_background_color(w, (GColor8){0xC0});
    window_set_window_handlers(w, (WindowHandlers){.load = on_load});
    window_stack_push(w, false);
    app_event_loop();
    fonts_unload_custom_font(s_font_v2);
    fonts_unload_custom_font(s_font_v3);
    window_destroy(w);
    return 0;
}
"""
        # 6x6 'X' diagonal pattern
        x_bits = [1 if (r == c or r + c == 5) else 0 for r in range(6) for c in range(6)]
        pfo_v2 = _build_pfo_v2_font(10, {ord("X"): (6, 6, 0, 2, 8, x_bits)})

        # 5x5 '+' cross pattern (RLE4)
        plus_bits = [1 if (r == 2 or c == 2) else 0 for r in range(5) for c in range(5)]
        pfo_v3 = _build_pfo_v3_rle4_font(8, {ord("+"): (5, 5, 0, 1, 7, plus_bits)})

        app_bin = build_pebble_app_bin(c_src, app_name="CustomFontTest")
        pbpack = _build_pbpack([pfo_v2, pfo_v3])

        with tempfile.TemporaryDirectory() as tmp:
            pbw_path = pathlib.Path(tmp) / "fonts.pbw"
            _pack_pbw_with_resources(pbw_path, "chalk", app_bin, pbpack)

            with AtlasHaloEmulator(print_handler=None) as emu:
                emu.connect()
                frames = emu.execute_lua(f"return frame.pebble.run('{pbw_path.as_posix()}', {{ max_frames = 1 }})")
                self.assertEqual(frames, 1)
                arr = np.array(emu.get_framebuffer().convert("RGB"))

                def px(x: int, y: int) -> tuple[int, int, int]:
                    r, g, b = arr[38 + y, 38 + x]
                    return (int(r), int(g), int(b))

                # PFO v2 'X' rendered at (10 + 0, 10 + 2) = (10, 12)..(15, 17)
                for r in range(6):
                    for c in range(6):
                        expected = (255, 0, 0) if (r == c or r + c == 5) else (0, 0, 0)
                        self.assertEqual(px(10 + c, 12 + r), expected, f"PFO v2 'X' mismatch at ({c},{r})")

                # PFO v3 RLE4 '+' rendered at (40 + 0, 10 + 1) = (40, 11)..(44, 15)
                for r in range(5):
                    for c in range(5):
                        expected = (0, 255, 0) if (r == 2 or c == 2) else (0, 0, 0)
                        self.assertEqual(px(40 + c, 11 + r), expected, f"PFO v3 RLE4 '+' mismatch at ({c},{r})")

                # graphics_text_layout_get_content_size("XX") == (16, 10) -> Blue swatch at (100, 10)
                self.assertEqual(px(110, 20), (0, 0, 255), "content_size for custom PFO font mismatch")

                # System font fallback "HI" rendered in white at (10, 50)
                sys_region = arr[38 + 50 : 38 + 90, 38 + 10 : 38 + 110]
                white_px = int(np.count_nonzero(np.all(sys_region == [255, 255, 255], axis=2)))
                self.assertGreater(white_px, 40, f"expected system font fallback pixels, got {white_px}")

    def test_real_test_app_pbw_loads_and_renders_bundled_pfo_fonts(self) -> None:
        """Unmodified test-app.pbw loads its bundled .pfo fonts and renders visibly in HaloEmulator.get_framebuffer()."""
        from halo_atlas.runner import PebbleRunnerSession, RunnerOptions

        # 1. Verify zero stub warnings for resource/font syscalls in traced execution
        sess = PebbleRunnerSession(
            TEST_PBW,
            on_frame=lambda _rgb: None,
            options=RunnerOptions(time_epoch=1767236400, max_frames=1, trace=True),
        )
        summary = sess.run()
        self.assertEqual(summary.returncode, 0)
        self.assertEqual(summary.frames, 1)
        self.assertNotIn("resource_get_handle (stub)", summary.stderr)
        self.assertNotIn("fonts_load_custom_font (stub)", summary.stderr)
        self.assertNotIn("fonts_unload_custom_font (stub)", summary.stderr)

        # 2. Verify rendered pixels directly via HaloEmulator.get_framebuffer() with vs without bundled .pbpack
        with tempfile.TemporaryDirectory() as tmp:
            with zipfile.ZipFile(TEST_PBW, "r") as z:
                raw_bin = pathlib.Path(tmp) / "pebble-app.bin"
                raw_bin.write_bytes(z.read("chalk/pebble-app.bin"))

            with AtlasHaloEmulator(print_handler=None) as emu:
                emu.connect()
                pbw_lua = TEST_PBW.as_posix()
                frames = emu.execute_lua(
                    f"return frame.pebble.run('{pbw_lua}', {{ time_epoch = 1767236400, max_frames = 1 }})"
                )
                self.assertEqual(frames, 1)
                arr_with_res = np.array(emu.get_framebuffer().convert("RGB"))

                frames_no_res = emu.execute_lua(
                    f"return frame.pebble.run('{raw_bin.as_posix()}', "
                    "{ platform = 'chalk', time_epoch = 1767236400, max_frames = 1 })"
                )
                self.assertEqual(frames_no_res, 1)
                arr_no_res = np.array(emu.get_framebuffer().convert("RGB"))

        diff_px = int(np.count_nonzero(np.any(arr_with_res != arr_no_res, axis=2)))
        self.assertGreater(
            diff_px,
            400,
            f"expected bundled PFO custom fonts in test-app.pbw to render visibly differently from fallback, diff={diff_px}",
        )

    def test_direct_framebuffer_capture_read_write_and_release_flushes_to_emulator(self) -> None:
        """App captures GBitmap framebuffer (8-bit and 1-bit), reads/writes pixels directly, and releases."""
        c_src_8bit = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint8_t argb; } GColor8;
typedef struct { int16_t x; int16_t y; } GPoint;
typedef struct { int16_t w; int16_t h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct { uint8_t *data; int16_t min_x; int16_t max_x; } GBitmapDataRowInfo;
typedef struct Window Window;
typedef struct Layer Layer;
typedef struct GBitmap GBitmap;
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
extern GBitmap *graphics_capture_frame_buffer(GContext *);
extern bool graphics_release_frame_buffer(GContext *, GBitmap *);
extern bool graphics_frame_buffer_is_captured(GContext *);
extern uint8_t *gbitmap_get_data(const GBitmap *);
extern uint16_t gbitmap_get_bytes_per_row(const GBitmap *);
extern GRect gbitmap_get_bounds(const GBitmap *);
extern GBitmapDataRowInfo gbitmap_get_data_row_info(const GBitmap *, uint16_t);
extern void app_event_loop(void);

static void draw_cb(Layer *l, GContext *ctx) {
    (void)l;
    /* 1. Paint a Red (0xF0) seed rect at (10,10,10,10) before capturing. */
    graphics_context_set_fill_color(ctx, (GColor8){0xF0});
    graphics_fill_rect(ctx, (GRect){{10, 10}, {10, 10}}, 0, 0);

    bool ok = !graphics_frame_buffer_is_captured(ctx);
    GBitmap *fb = graphics_capture_frame_buffer(ctx);
    ok = ok && (fb != NULL) && graphics_frame_buffer_is_captured(ctx);
    /* Nested capture must fail and return NULL */
    ok = ok && (graphics_capture_frame_buffer(ctx) == NULL);

    /* Graphics calls while captured must NOT overwrite the Red seed rect */
    graphics_context_set_fill_color(ctx, (GColor8){0xC3});
    graphics_fill_rect(ctx, (GRect){{10, 10}, {10, 10}}, 0, 0);

    if (fb != NULL) {
        uint8_t *data = gbitmap_get_data(fb);
        uint16_t bpr = gbitmap_get_bytes_per_row(fb);
        GRect bounds = gbitmap_get_bounds(fb);
        ok = ok && (data != NULL) && (bpr == 180) && (bounds.size.w == 180) && (bounds.size.h == 180);
        /* Read back Red seed pixel (12, 12) */
        ok = ok && (data[12 * bpr + 12] == 0xF0);

        /* Direct write via gbitmap_get_data: Green (0xCC) if all checks passed, else Blue */
        uint8_t c_direct = ok ? 0xCC : 0xC3;
        for (int y = 40; y < 60; ++y) {
            for (int x = 40; x < 60; ++x) {
                data[y * bpr + x] = c_direct;
            }
        }

        /* Direct write via gbitmap_get_data_row_info: Cyan (0xCF) at (80..99, 40..59) */
        for (uint16_t y = 40; y < 60; ++y) {
            GBitmapDataRowInfo row = gbitmap_get_data_row_info(fb, y);
            if (row.data != NULL && row.min_x <= 80 && row.max_x >= 99) {
                for (int x = 80; x < 100; ++x) {
                    row.data[x] = 0xCF;
                }
            }
        }

        /* Releasing wrong pointer must fail and leave fb captured */
        bool bad_rel = graphics_release_frame_buffer(ctx, NULL);
        bool still_cap = graphics_frame_buffer_is_captured(ctx);
        bool good_rel = graphics_release_frame_buffer(ctx, fb);
        bool now_free = !graphics_frame_buffer_is_captured(ctx);
        if (!bad_rel && still_cap && good_rel && now_free) {
            /* Normal drawing must work again after release: Yellow (0xFC) at (120, 40, 20, 20) */
            graphics_context_set_fill_color(ctx, (GColor8){0xFC});
            graphics_fill_rect(ctx, (GRect){{120, 40}, {20, 20}}, 0, 0);
        }
    }
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
        c_src_1bit = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint8_t argb; } GColor8;
typedef struct { int16_t x; int16_t y; } GPoint;
typedef struct { int16_t w; int16_t h; } GSize;
typedef struct { GPoint origin; GSize size; } GRect;
typedef struct Window Window;
typedef struct Layer Layer;
typedef struct GBitmap GBitmap;
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
extern GBitmap *graphics_capture_frame_buffer_format(GContext *, uint8_t format);
extern bool graphics_release_frame_buffer(GContext *, GBitmap *);
extern uint8_t *gbitmap_get_data(const GBitmap *);
extern uint16_t gbitmap_get_bytes_per_row(const GBitmap *);
extern void app_event_loop(void);

static void draw_cb(Layer *l, GContext *ctx) {
    (void)l;
    GBitmap *fb = graphics_capture_frame_buffer_format(ctx, 0); /* GBitmapFormat1Bit */
    if (fb != NULL) {
        uint8_t *data = gbitmap_get_data(fb);
        uint16_t bpr = gbitmap_get_bytes_per_row(fb);
        /* Paint 16x16 white block at x=[16..31], y=[16..31] (bytes 2 and 3 of each row) */
        for (int y = 16; y < 32; ++y) {
            data[y * bpr + 2] = 0xFF;
            data[y * bpr + 3] = 0xFF;
        }
        graphics_release_frame_buffer(ctx, fb);
    }
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
        app_8bit = build_pebble_app_bin(c_src_8bit, app_name="FBCapture8Bit")
        app_1bit = build_pebble_app_bin(c_src_1bit, app_name="FBCapture1Bit")

        with tempfile.TemporaryDirectory() as tmp:
            pbw_8 = pathlib.Path(tmp) / "fb8.pbw"
            pbw_1 = pathlib.Path(tmp) / "fb1.pbw"
            _pack_pbw_with_resources(pbw_8, "chalk", app_8bit)
            _pack_pbw_with_resources(pbw_1, "aplite", app_1bit)

            with AtlasHaloEmulator(print_handler=None) as emu:
                emu.connect()

                # 1. Verify 8-bit direct framebuffer read/write & release on chalk (offset +38, +38)
                frames = emu.execute_lua(f"return frame.pebble.run('{pbw_8.as_posix()}', {{ max_frames = 1 }})")
                self.assertEqual(frames, 1)
                arr = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(
                    tuple(int(c) for c in arr[38 + 12, 38 + 12]),
                    (255, 0, 0),
                    "seed Red pixel must survive suppressed draw during capture",
                )
                self.assertEqual(
                    tuple(int(c) for c in arr[38 + 50, 38 + 50]),
                    (0, 255, 0),
                    "direct gbitmap_get_data write must flush Green",
                )
                self.assertEqual(
                    tuple(int(c) for c in arr[38 + 50, 38 + 90]),
                    (0, 255, 255),
                    "direct gbitmap_get_data_row_info write must flush Cyan",
                )
                self.assertEqual(
                    tuple(int(c) for c in arr[38 + 50, 38 + 130]),
                    (255, 255, 0),
                    "normal drawing must resume after graphics_release_frame_buffer",
                )

                # 2. Verify 1-bit framebuffer capture/release on aplite (144x168 -> offset +56, +44)
                frames1 = emu.execute_lua(f"return frame.pebble.run('{pbw_1.as_posix()}', {{ max_frames = 1 }})")
                self.assertEqual(frames1, 1)
                arr1 = np.array(emu.get_framebuffer().convert("RGB"))
                self.assertEqual(
                    tuple(int(c) for c in arr1[44 + 20, 56 + 20]),
                    (255, 255, 255),
                    "1-bit framebuffer direct write must flush white pixels",
                )
                self.assertEqual(
                    tuple(int(c) for c in arr1[44 + 5, 56 + 5]),
                    (0, 0, 0),
                    "unwritten 1-bit framebuffer pixel must remain black",
                )


if __name__ == "__main__":
    unittest.main()
