#ifndef VV_WSS_INCREMENTAL_H
#define VV_WSS_INCREMENTAL_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
/* Caller has completed VERIFIED TLS + RFC6455 Upgrade. Single owner only.
 * try I/O: positive progress, VV_WSI_BLOCK for no readiness, read 0 EOF,
 * other negative failure. Callbacks must never wait. No borrowed TX storage.
 * Pump bounded to budget I/O calls; tick often, including during inactivity. */
#define VV_WSI_BLOCK (-20001)
#define VV_WSI_TX_CAP 4096u
struct vv_wsi_ops {
 ssize_t (*read)(void *,void *,size_t);
 ssize_t (*write)(void *,const void *,size_t);
 int (*random)(void *,uint8_t *,size_t);
 void (*close)(void *);
 int (*message)(void *,unsigned,const void *,size_t);
 void *arg;
};
struct vv_wsi {
 struct vv_wsi_ops io; uint8_t *message;size_t capacity,used;
 uint8_t header[10],control[125];size_t have,need,frame_read;
 uint64_t frame_length,deadline,last_now;
 unsigned opcode,message_opcode;bool fin,parsed,active,close_after,tx_close;
 uint8_t tx[VV_WSI_TX_CAP+14],pending[125];size_t tx_length,tx_offset,pending_length;
 unsigned pending_opcode;
};
int vv_wsi_init(struct vv_wsi *,const struct vv_wsi_ops *,void *message_storage,size_t capacity,uint64_t now,uint64_t deadline);
int vv_wsi_queue(struct vv_wsi *,unsigned opcode,const void *,size_t);
int vv_wsi_pump(struct vv_wsi *,uint64_t now,bool cancelled,unsigned budget);
void vv_wsi_abort(struct vv_wsi *);
#endif
