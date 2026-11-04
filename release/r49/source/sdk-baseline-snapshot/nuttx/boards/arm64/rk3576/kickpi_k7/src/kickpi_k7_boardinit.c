/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/src/kickpi_k7_boardinit.c
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

#include <nuttx/config.h>
#include <nuttx/board.h>

#include "arm64_arch.h"
#include "kickpi_k7.h"

void rk3576_board_initialize(void)
{
  /* Take over UART0 interrupts without changing the bootloader's baud,
   * FIFO or line control settings.  DLAB must already be clear (8N1).
   * The generic 16550 driver enables RX/TX interrupts when NSH opens it.
   */

#ifdef CONFIG_RK3576_UART0
  putreg32(0, CONFIG_16550_UART0_BASE + 0x04);
#endif
}

#ifdef CONFIG_BOARDCTL_RESET
#include <nuttx/arch.h>
#include <errno.h>
int board_reset(int status)
{
  up_systemreset();
  return -EIO;
}
#endif
