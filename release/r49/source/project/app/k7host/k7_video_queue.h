/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_VIDEO_QUEUE_H
#define K7_VIDEO_QUEUE_H
#include <stddef.h>
#include <stdint.h>
/* One camera producer, one transport consumer, one lifetime per boot.
 * Published JPEGs are private copies. An in-flight frame is immutable.
 * Time is parser publication, not exposure time. Blocks are padded to16KiB.
 */
void k7_video_queue_open(void);
void k7_video_queue_close(void);
void k7_video_publish(const uint8_t *,size_t,uint32_t,uint64_t);
int k7_video_queue_take(const uint8_t **wire,size_t *bytes);
void k7_video_queue_release(int slot);
#endif
