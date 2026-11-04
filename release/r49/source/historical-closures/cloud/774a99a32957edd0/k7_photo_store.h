/* Bounded RAM retention for one three-view set. NOT durable storage. */
#ifndef K7_PHOTO_STORE_H
#define K7_PHOTO_STORE_H
#include <stddef.h>
#include <stdint.h>
struct k7_photo_store;
struct k7_photo_record {
  uint32_t epoch, sequence;
  unsigned side; /* front=1, left=2, right=4 */
  size_t bytes;
};
struct k7_photo_store *k7_photo_store_create(size_t max_jpeg_bytes);
void k7_photo_store_destroy(struct k7_photo_store *);
/* Invalidates previous set; epoch 0 and reused/older epochs are rejected. */
int k7_photo_store_begin(struct k7_photo_store *, uint32_t epoch);
/* Exact immutable copy. Only front -> left -> right accepted. */
int k7_photo_store_put(struct k7_photo_store *,const struct k7_photo_record *,const void *);
/* Returns a caller-owned copy, so reset cannot invalidate an upload pointer. */
int k7_photo_store_copy(struct k7_photo_store *,uint32_t epoch,unsigned side,
                       struct k7_photo_record *,void *,size_t capacity);
/* Zero retained image bytes and records only when the owner epoch matches. */
int k7_photo_store_discard(struct k7_photo_store *,uint32_t epoch);
int k7_photo_store_complete(struct k7_photo_store *,uint32_t epoch);
int k7_photo_store_status(struct k7_photo_store *,uint32_t *epoch,unsigned *done);
#endif
