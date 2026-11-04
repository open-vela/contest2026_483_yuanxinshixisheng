/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/src/kickpi_k7_appinit.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/types.h>
#include <nuttx/board.h>
#include <nuttx/fs/fs.h>
#include "kickpi_k7.h"

#if defined(CONFIG_CXX_EXCEPTION) && defined(CONFIG_BUILD_FLAT)
/* The flat image remains loaded for the entire boot.  Register exactly once
 * in lib_cxx_initialize(), after heap setup, before other constructors.
 * Do not move this into board_app_initialize(): that is too late.
 */
extern unsigned char __k7_eh_frame_start[];
extern void __register_frame(void *begin);

static void k7_unwind_initialize(void)
{
  static bool registered;

  if (!registered)
    {
      __register_frame(__k7_eh_frame_start);
      registered = true;
    }
}

/* A dedicated linker slot outranks all numeric init_array priorities.
 * This initializer is boot-serialized; the guard is not an SMP once API.
 */
static void (*const g_k7_unwind_init)(void)
  __attribute__((used, section(".k7_unwind_init"))) = k7_unwind_initialize;
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application specific initialization.  This function is never
 *   called directly from application code, but only indirectly via the
 *   (non-standard) boardctl() interface using the command BOARDIOC_INIT.
 *
 * Input Parameters:
 *   arg - The boardctl() argument is passed to the board_app_initialize()
 *         implementation without modification.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure to indicate the nature of the failure.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
#ifdef CONFIG_FS_PROCFS
  int ret;

  /* Provide the process and memory information used by NSH ps/free. */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0 && ret != -EBUSY)
    {
      return ret;
    }
#endif

  return OK;
}
