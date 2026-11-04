/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/include/board.h
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

#ifndef __BOARDS_ARM64_RK3576_KICKPI_K7_INCLUDE_BOARD_H
#define __BOARDS_ARM64_RK3576_KICKPI_K7_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clocking *****************************************************************/

/* Nominal baud clock only.  Earlycon does not establish its rate.
 * The nsh configuration preserves the bootloader UART setup.
 */

#define RK3576_UART0_CLKIN  24000000

/* Debug serial on UART0, 1500000 8N1 (K7 debug UART default) */

#define RK3576_UART0_BAUD   1500000

#endif /* __BOARDS_ARM64_RK3576_KICKPI_K7_INCLUDE_BOARD_H */
