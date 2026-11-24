# 03: Bundled `.pbpack` Resources, Bitmaps, Fonts & Direct Framebuffer Access

**What to build:** Enable unmodified `.pbw` watchfaces and apps that rely on embedded `app_resources.pbpack` assets or direct framebuffer manipulation to render their authentic visuals inside `HaloEmulator`. Apps can load raw resources, decode and draw bundled PBI/PNG bitmaps via `BitmapLayer` and `GBitmap`, render Pebble custom and system bitmap fonts, and read/write pixels directly via `graphics_capture_frame_buffer` and `graphics_release_frame_buffer`.

**Blocked by:** 02: First Live Watchface Rendering Inside Official `HaloEmulator`

**Status:** ready-for-agent

- [ ] The portable C core reads `app_resources.pbpack` via `pebble_hal`, supporting `resource_get_handle`, `resource_size`, `resource_load`, and `resource_load_byte_range`.
- [ ] `gbitmap_create_with_resource` and `bitmap_layer_*` decode and render embedded Pebble PBI and PNG bitmap resources (with transparency and palette support) onto the `GColor8` shadow buffer.
- [ ] `fonts_load_custom_font` and `fonts_get_system_font` render authentic Pebble bitmap font glyphs (with clean fallback to Halo's built-in pixel font for unbundled system fonts).
- [ ] Apps calling `graphics_capture_frame_buffer()` receive a valid Pebble-format `GBitmap` backed by the `GColor8` shadow buffer, and `graphics_release_frame_buffer()` flushes direct pixel writes to Halo's `canvas` and `HaloEmulator`.
- [ ] Automated tests at the `HaloEmulator` seam verify real `.pbw` apps with bundled resources, custom fonts/bitmaps, and direct framebuffer writes render accurately in `get_framebuffer()`.
