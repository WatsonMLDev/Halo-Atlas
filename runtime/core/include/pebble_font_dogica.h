#ifndef PEBBLE_FONT_DOGICA_H
#define PEBBLE_FONT_DOGICA_H

#include <stdint.h>

typedef struct {
    uint32_t bitmap_offset;
    uint8_t width;
    uint8_t height;
    uint8_t x_advance;
    int8_t x_offset;
    int8_t y_offset;
} pebble_dogica_glyph_t;

#define PEBBLE_DOGICA_FIRST 0x20u
#define PEBBLE_DOGICA_LAST 0x7Eu
#define PEBBLE_DOGICA_Y_ADVANCE 10u

extern const uint8_t pebble_dogica_bitmaps[];
extern const pebble_dogica_glyph_t pebble_dogica_glyphs[95];

#endif /* PEBBLE_FONT_DOGICA_H */
