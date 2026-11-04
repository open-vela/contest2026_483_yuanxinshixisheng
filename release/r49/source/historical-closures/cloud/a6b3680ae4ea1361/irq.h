/****************************************************************************
 * arch/arm64/include/rk3576/irq.h
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

#ifndef __ARCH_ARM64_INCLUDE_RK3576_IRQ_H
#define __ARCH_ARM64_INCLUDE_RK3576_IRQ_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_RK3576_SMP_DIAG) && !defined(__ASSEMBLY__)
#  include <arch/chip/rk3576_cpu_topology.h>
#  define MPID_TO_CORE(mpid) vv_k7_logical_cpu((mpid), CONFIG_SMP_NCPUS)
#endif

#define NR_IRQS                 512   /* 16 SGIs + 16 PPIs + 480 SPIs */

#define RK3576_IRQ_SGI0         (0)   /* SGI 0 */
#define RK3576_IRQ_SGI1         (1)   /* SGI 1 */
#define RK3576_IRQ_SGI2         (2)   /* SGI 2 */
#define RK3576_IRQ_SGI3         (3)   /* SGI 3 */
#define RK3576_IRQ_SGI4         (4)   /* SGI 4 */
#define RK3576_IRQ_SGI5         (5)   /* SGI 5 */
#define RK3576_IRQ_SGI6         (6)   /* SGI 6 */
#define RK3576_IRQ_SGI7         (7)   /* SGI 7 */
#define RK3576_IRQ_SGI8         (8)   /* SGI 8 */
#define RK3576_IRQ_SGI9         (9)   /* SGI 9 */
#define RK3576_IRQ_SGI10        (10)  /* SGI 10 */
#define RK3576_IRQ_SGI11        (11)  /* SGI 11 */
#define RK3576_IRQ_SGI12        (12)  /* SGI 12 */
#define RK3576_IRQ_SGI13        (13)  /* SGI 13 */
#define RK3576_IRQ_SGI14        (14)  /* SGI 14 */
#define RK3576_IRQ_SGI15        (15)  /* SGI 15 */

#define RK3576_IRQ_PPI0         (16)  /* PPI 0 */
#define RK3576_IRQ_PPI1         (17)  /* PPI 1 */
#define RK3576_IRQ_PPI2         (18)  /* PPI 2 */
#define RK3576_IRQ_PPI3         (19)  /* PPI 3 */
#define RK3576_IRQ_PPI4         (20)  /* PPI 4 */
#define RK3576_IRQ_PPI5         (21)  /* PPI 5 */
#define RK3576_IRQ_PPI6         (22)  /* PPI 6 */
#define RK3576_IRQ_PPI7         (23)  /* PPI 7 */

/* GIC SPI interrupts (NuttX IRQ number = GIC SPI + 32) */

#define RK3576_IRQ_UART0        (32 + 76)   /* GIC_SPI 76, serial@2ad40000 */
#define RK3576_IRQ_UART1        (32 + 77)   /* GIC_SPI 77, serial@27310000 */

#endif /* __ARCH_ARM64_INCLUDE_RK3576_IRQ_H */
