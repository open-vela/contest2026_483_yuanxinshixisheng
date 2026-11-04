/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef K7NPU_FIXTURE_IO_H
#define K7NPU_FIXTURE_IO_H
#include <stddef.h>
#define K7NPU_RAW_BYTES 0x80000
#define K7NPU_RAW_WORDS 143
#define K7NPU_RAW_OUTPUT 0x60000
#define K7NPU_RAW_VARIANTS 8
int k7npu_fixture_prepare(unsigned char *data, size_t size);
int k7npu_fixture_compare(const unsigned char *data);
int k7npu_fixture_variant(unsigned char *data, unsigned int variant);
int k7npu_fixture_compare_variant(const unsigned char *data, unsigned int variant);
#endif
