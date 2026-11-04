/* SPDX-License-Identifier: Apache-2.0 */
/* K7 UART6 -> existing STM32 USART3, fixed 115200 8N1, no flow control. */
#include <nuttx/config.h>
#include "gimbal_link.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef GIMBAL_PORT
#  define GIMBAL_PORT "/dev/ttyS1"
#endif
#define PERIOD_MS 20
#define X_LIMIT 250
#define Y_LIMIT 200


static int number(const char *s, int lo, int hi, int *value)
{
  char *end;
  long v;
  errno = 0;
  v = strtol(s, &end, 10);
  if (errno || s == end || *end || v < lo || v > hi)
    {
      fprintf(stderr, "Invalid value '%s': expected %d..%d\n", s, lo, hi);
      return -1;
    }
  *value = (int)v;
  return 0;
}

static int pause_ms(unsigned int ms)
{
  struct timespec req = {ms / 1000, (ms % 1000) * 1000000L};
  /* An interrupted command must stop sending further frames. */
  return nanosleep(&req, NULL);
}

static int hold_target(int fd, int x, int y, int ms)
{
  int rounds = (ms + PERIOD_MS - 1) / PERIOD_MS;
  printf("TX target x=%d y=%d, %d pairs at 50 Hz\n", x, y, rounds);
  fflush(stdout);
  for (int i = 0; i < rounds; i++)
    {
      if (gimbal_link_send(fd, x, y) < 0 || pause_ms(PERIOD_MS) < 0)
        {
          return -1;
        }
    }
  return 0;
}

static void usage(void)
{
  puts("gimbal info                 - show fixed UART/protocol settings\n"
       "gimbal jog DX DY            - one axis, delta -20..20, manual calibration\n"
       "gimbal adopt X Y            - restore recorded target in RAM, NO UART TX\n"
       "gimbal set X Y [MS]         - X=-250..250, Y=-200..200\n"
       "                              MS=20..10000 (default 300)\n"
       "gimbal zero                - send target (0,0) for 300 ms\n"
       "gimbal test [A]            - small X/Y sweep, A=1..50 (default 20)\n"
       "gimbal frame X Y           - preview exact bytes; no transmission\n"
       "Values are original MCU target units, NOT degrees.\n"
       "zero is a position target, NOT a motor-disable command.\n"
       "No MCU acknowledgement exists in this protocol.");
}

int main(int argc, char **argv)
{
  int x = 0, y = 0, ms = 300, amplitude = 20;
  int test = 0;
  int jog = 0;
  int fd;
  int ret = 0;
  if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help"))
    {
      usage();
      return argc < 2 ? 1 : 0;
    }
  if (!strcmp(argv[1], "info") && argc == 2)
    {
      puts("K7 UART6 M0: /dev/ttyS1, fixed 115200 8N1, no flow control\n"
           "Pin 5 TX -> MCU RX; Pin 7 RX <- MCU TX; Pin 6 GND\n"
           "Frame: 55 AA axis lo hi 00 FA; axis 00=X, FF=Y; signed LE16\n"
           "UART0 /dev/console remains 1500000 baud.\n"
           "MCU PROFILE v1.3-xcenter X_MID=1492 X_RANGE=-800..800 Y_MID=700\n"
           "info reports configuration, not physical motion or MCU receipt.");
      return 0;
    }
  if (!strcmp(argv[1], "adopt") && argc == 4)
    {
      if (number(argv[2], -800, 800, &x) < 0 ||
          number(argv[3], -120, 1030, &y) < 0) return 1;
      if (gimbal_link_adopt(x, y) < 0) { perror("adopt"); return 1; }
      printf("ADOPT OK x=%d y=%d NO UART TX; not measured position\n",x,y);
      return 0;
    }
  if (!strcmp(argv[1], "jog") && argc == 4)
    {
      if (number(argv[2], -20, 20, &x) < 0 ||
          number(argv[3], -20, 20, &y) < 0 || (x && y)) return 1;
      jog = 1;
      ms = 20;
    }
  else if ((!strcmp(argv[1], "set") && (argc == 4 || argc == 5)) ||
      (!strcmp(argv[1], "frame") && argc == 4))
    {
      if (number(argv[2], -X_LIMIT, X_LIMIT, &x) < 0 ||
          number(argv[3], -Y_LIMIT, Y_LIMIT, &y) < 0 ||
          (argc == 5 && number(argv[4], 20, 10000, &ms) < 0))
        {
          return 1;
        }
      if (!strcmp(argv[1], "frame"))
        {
          uint8_t data[14];
          gimbal_link_pack(data, x, y);
          for (int i = 0; i < 14; i++)
            {
              printf("%02X%c", data[i], i % 7 == 6 ? '\n' : ' ');
            }
          return 0;
        }
    }
  else if (!strcmp(argv[1], "zero") && argc == 2)
    {
      /* Keep the same absolute zero semantics as the existing MCU code. */
    }
  else if (!strcmp(argv[1], "test") && (argc == 2 || argc == 3))
    {
      if (argc == 3 && number(argv[2], 1, 50, &amplitude) < 0)
        {
          return 1;
        }
      test = 1;
    }
  else
    {
      usage();
      return 1;
    }

  /* Fixed board initialization supplies the baud/8N1 settings. The
   * non-console NuttX tty defaults to binary I/O, with OPOST disabled.
   * O_NONBLOCK bounds a stalled UART instead of hanging in write/close.
   */
  int last_x, last_y;
  fd = gimbal_link_open(&last_x, &last_y);
  if (fd < 0)
    {
      perror("gimbal UART ownership");
      return 1;
    }
  if (jog)
    {
      x += last_x;
      y += last_y;
      if (x < -800 || x > 800 || y < -120 || y > 1030)
        {
          puts("JOG LIMIT: keep last requested target");
          gimbal_link_close(fd);
          return 1;
        }
    }
  if (test)
    {
      const int points[][2] = {{0,0}, {1,0}, {-1,0}, {0,0},
                               {0,1}, {0,-1}, {0,0}};
      for (unsigned int i = 0; i < sizeof(points) / sizeof(points[0]); i++)
        {
          ret = hold_target(fd, points[i][0] * amplitude,
                            points[i][1] * amplitude, 500);
          if (ret < 0)
            {
              break;
            }
        }
    }
  else
    {
      ret = hold_target(fd, x, y, ms);
    }
  if (ret < 0)
    {
      perror("gimbal TX");
    }
  gimbal_link_close(fd);
  if (ret == 0)
    {
      puts("TX complete; MCU receipt/motion is not acknowledged.");
      if (jog) printf("JOG OK x=%d y=%d\n", x, y);
    }
  return ret < 0 ? 1 : 0;
}
