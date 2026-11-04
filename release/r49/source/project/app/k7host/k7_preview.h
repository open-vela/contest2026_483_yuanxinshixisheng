/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_PREVIEW_H
#define K7_PREVIEW_H
#include <stddef.h>
#include <stdint.h>
int k7_preview_emit(const uint8_t *rgb, size_t size, unsigned int sequence);
struct k7_preview_queue_s;
int k7_preview_queue_start(struct k7_preview_queue_s **out);
void k7_preview_queue_submit(struct k7_preview_queue_s *, const uint8_t *, size_t, unsigned int);
int k7_preview_queue_stop(struct k7_preview_queue_s *);
#endif
