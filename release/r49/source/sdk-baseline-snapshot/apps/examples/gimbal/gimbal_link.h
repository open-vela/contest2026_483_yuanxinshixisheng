/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GIMBAL_LINK_H
#define GIMBAL_LINK_H
#include <stdint.h>
int gimbal_link_open(int *last_x, int *last_y);
int gimbal_link_send(int fd, int x, int y);
void gimbal_link_close(int fd);
int gimbal_link_pack(uint8_t data[14], int x, int y);
/* Restore a known last-requested target after a K7-only reboot, no UART I/O. */
int gimbal_link_adopt(int x, int y);
#endif
