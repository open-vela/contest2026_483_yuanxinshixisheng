/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_VIDEO_FRAME_H
#define K7_VIDEO_FRAME_H
#include <stddef.h>
#include <stdint.h>
#define K7_VIDEO_HEADER_BYTES 36
#define K7_VIDEO_MAX_BYTES (1024u * 1024u)
/* LE wire format: magic[8], sequence:u32, capture_us:u64, length:u32,
 * width:u16, height:u16, payload_crc32:u32, header_crc32:u32.
 * capture_us is board monotonic time, not the host clock.
 * Encode at frame admission; header+payload remain one immutable unit until
 * complete or disconnect. A sender must never splice in a newer frame. */
uint32_t k7_video_crc32(const uint8_t *data,size_t size);
int k7_video_header(uint8_t header[K7_VIDEO_HEADER_BYTES],uint32_t sequence,
                    uint64_t capture_us,uint16_t width,uint16_t height,
                    const uint8_t *jpeg,size_t size);
#endif
