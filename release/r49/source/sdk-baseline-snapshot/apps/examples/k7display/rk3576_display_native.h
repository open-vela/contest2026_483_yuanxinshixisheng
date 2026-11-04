/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef K7DISPLAY_NATIVE_H
#define K7DISPLAY_NATIVE_H
#include <stddef.h>
#include <stdint.h>
/* One-shot guarded display startup; explicit command or opt-in native boot. */
/* Read-only guard diagnostics; zero register writes even on failure. */
int k7display_native_probe(void);
int k7display_native_start(void);
int k7display_native_stop(void);
int k7display_native_status(void);
/* blank=1 hides only the backlight; blank=0 restores existing brightness.
 * Keeps scanout, framebuffer lease and all non-display services running.
 * This is not system sleep; requires an already running display session. */
int k7display_native_blank(int blank);
/* Camera-only sink. Requires a successfully started display session.
 * Copies RGB synchronously; never retains caller pixels or starts devices. */
int k7display_video_ready(void);
/* Rectangle in unmirrored source 640x480 pixels, half-open x/y + width/height.
 * NULL, empty or wholly off-image boxes are ignored; crossing boxes clip.
 * The overlay changes display pixels only, never source RGB or coordinates.
 */
struct k7display_box { int32_t x, y, width, height; };
int k7display_video_present_rgb888_box(const uint8_t *, unsigned width,
                                      unsigned height, size_t stride,
                                      size_t bytes, const struct k7display_box *);
int k7display_video_present_rgb888(const uint8_t *, unsigned width,
                                  unsigned height, size_t stride, size_t bytes);
#endif
