/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_F050008M01_H
#define K7_F050008M01_H
#include <stddef.h>
extern const unsigned char k7panel_init[], k7panel_exit[];
extern const size_t k7panel_init_size, k7panel_exit_size;
#define K7PANEL_WIDTH 720
#define K7PANEL_HEIGHT 1280
#define K7PANEL_PIXEL_CLOCK 84000000
#define K7PANEL_HFP 32
#define K7PANEL_HSYNC 15
#define K7PANEL_HBP 32
#define K7PANEL_VFP 21
#define K7PANEL_VSYNC 15
#define K7PANEL_VBP 12
/* The caller owns verified power/reset/DSI setup and serializes all operations.
 * tx must return payload length on success, negative errno on failure.
 * No retries, and no writes at all when validation fails. */
struct k7panel_io {
  int (*tx)(void *arg, unsigned char type, const unsigned char *data, size_t size);
  int (*delay_ms)(void *arg, unsigned int ms);
  void *arg;
};
int k7panel_validate(const unsigned char *data, size_t size);
int k7panel_run(const unsigned char *data, size_t size, const struct k7panel_io *io);
#endif
