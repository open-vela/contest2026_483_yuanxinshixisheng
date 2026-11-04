/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated test data from pinned charsiu; see k7npu_fixture.h and reference/.
 * No model runtime and no MMIO. Host and native builds share validation.
 */
#include "k7npu_fixture_io.h"
#include "k7npu_fixture.h"
#include <errno.h>
#include <string.h>

int k7npu_fixture_prepare(unsigned char *data, size_t size)
{
  unsigned int i, j, pointers = 0;
  if (!data || size != K7NPU_RAW_BYTES || K7_NPU_FIXTURE_WORDS != K7NPU_RAW_WORDS ||
      K7_NPU_COEF_BYTES > 0x41000) return -EINVAL;
  for (i = 0; i < K7_NPU_FIXTURE_WORDS; i++)
    {
      uint64_t word = g_fixture_commands[i];
      unsigned int target = word >> 48;
      unsigned int reg = word & 0xffff;
      uint32_t value = (uint32_t)(word >> 16);
      if ((reg & 3) || !((target == 0x201 && reg >= 0x1000 && reg < 0x1200) ||
          (target == 0x801 && reg >= 0x3000 && reg < 0x3100) ||
          (target == 0x1001 && reg >= 0x4000 && reg < 0x4200) ||
          (target == 0x2001 && reg >= 0x5000 && reg < 0x5100))) return -EINVAL;
      switch (reg)
        {
          case 0x1088: if (value != 0x10002000) return -EINVAL; pointers |= 1; break;
          case 0x1110: if (value != 0x10006000) return -EINVAL; pointers |= 2; break;
          case 0x4018: if (value != 0x10060000) return -EINVAL; pointers |= 4; break;
          case 0x5020: if (value != 0x10010000) return -EINVAL; pointers |= 8; break;
          case 0x5024: if (value < 0x10010000 || value >= 0x10051000) return -EINVAL; pointers |= 16; break;
          default: break;
        }
    }
  if (pointers != 31) return -EINVAL;
  for (i = 0; i < sizeof(g_fixture_coef)/sizeof(g_fixture_coef[0]); i++)
    if ((g_fixture_coef[i].offset & 3) || g_fixture_coef[i].offset > K7_NPU_COEF_BYTES - 4)
      return -EINVAL;

  /* Independently check the packed INT8 tile against the host CPU answers.
   * Shape crosses both n=32 and k=32 packing boundaries.
   */
  for (i = 0; i < 64; i++)
    {
      int32_t sum = 0;
      for (j = 0; j < 64; j++)
        {
          unsigned int index = (i/32)*2048 + (j/32)*1024 + (i%32)*32 + j%32;
          sum += (int8_t)g_fixture_input[j] * (int8_t)g_fixture_weights[index];
        }
      if (sum != g_fixture_expected[i]) return -EINVAL;
    }
  memset(data, 0, size);
  memcpy(data, g_fixture_commands, sizeof(g_fixture_commands));
  memcpy(data + 0x2000, g_fixture_input, sizeof(g_fixture_input));
  memcpy(data + 0x6000, g_fixture_weights, sizeof(g_fixture_weights));
  for (i = 0; i < sizeof(g_fixture_coef)/sizeof(g_fixture_coef[0]); i++)
    memcpy(data + 0x10000 + g_fixture_coef[i].offset, &g_fixture_coef[i].value, 4);
  memset(data + K7NPU_RAW_OUTPUT, 0xa5, 8192);
  return 0;
}

static int8_t variant_input(unsigned int channel, unsigned int variant)
{
  int value = (int8_t)g_fixture_input[(channel + 7 * variant) % 64];
  return variant & 1 ? -value : value;
}

int k7npu_fixture_variant(unsigned char *data, unsigned int variant)
{
  unsigned int j;
  if (!data || variant >= K7NPU_RAW_VARIANTS) return -EINVAL;
  /* Called only after the full immutable fixture/address validation. Change
   * input data, never instructions or DMA pointers. No reference is written
   * into the device output; poison it again to reject old completions.
   */
  for (j = 0; j < 64; j++) data[0x2000 + j] = (unsigned char)variant_input(j, variant);
  memset(data + K7NPU_RAW_OUTPUT, 0xa5, 8192);
  return 0;
}

int k7npu_fixture_compare_variant(const unsigned char *data, unsigned int variant)
{
  unsigned int i, j;
  int bad = 0;
  if (!data || variant >= K7NPU_RAW_VARIANTS) return -EINVAL;
  for (i = 0; i < 64; i++)
    {
      int32_t value, expected = 0;
      for (j = 0; j < 64; j++)
        {
          unsigned int index = (i/32)*2048 + (j/32)*1024 + (i%32)*32 + j%32;
          expected += variant_input(j, variant) * (int8_t)g_fixture_weights[index];
        }
      memcpy(&value, data + K7NPU_RAW_OUTPUT + i * 4, 4);
      if (value != expected) bad++;
    }
  return bad;
}

int k7npu_fixture_compare(const unsigned char *data)
{
  return k7npu_fixture_compare_variant(data, 0);
}
