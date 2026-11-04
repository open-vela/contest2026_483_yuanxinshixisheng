/* SPDX-License-Identifier: Apache-2.0 */
#ifndef K7_PHOTO_H
#define K7_PHOTO_H
#include "k7_track.h"
#include <stddef.h>
struct k7_photo_s;
struct k7_photo_s *k7_photo_create(void);
/* Voice mode retains exact JPEGs in bounded RAM, without USB-host save ACK. */
struct k7_photo_s *k7_photo_create_native(void);
#include "k7_photo_store.h"
int k7_photo_native_copy(uint32_t,unsigned,struct k7_photo_record *,void *,size_t);
int k7_photo_native_complete(uint32_t);
int k7_photo_native_discard(uint32_t);
int k7_photo_native_status(uint32_t *,unsigned *);
void k7_photo_destroy(struct k7_photo_s *);
void k7_photo_process(struct k7_photo_s *,const uint8_t *,size_t,unsigned int,
                      uint64_t,const struct k7_track_result_s *,unsigned int,bool);
void k7_photo_ack(unsigned int,unsigned int,unsigned int,bool);
void k7_photo_reset(void);
/* True only after target lock loss; repeated healthy commands stay idempotent. */
bool k7_photo_needs_restart(void);
#endif
