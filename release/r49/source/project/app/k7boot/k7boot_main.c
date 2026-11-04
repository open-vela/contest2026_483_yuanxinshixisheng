/* VelaVision opt-in startup. Console input belongs exclusively to NSH. */
#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern int nsh_main(int argc, char **argv);
extern int k7radio_main(int argc, char **argv);

static int g_started;
static int g_logging = 1;
static int g_stage; /* 0=pending, 1=starting, 2=ready, -1=failed */

static int radio_command(const char *command)
{
  char *argv[] = {"k7radio", (char *)command, NULL};
  return k7radio_main(2, argv);
}

static int startup_worker(int argc, char **argv)
{
  const char *steps[] = {"sdio-boot", "bt-host", "wifi-service-start"};
  unsigned int i;
  unsigned int ticks = 0;
  (void)argc;
  (void)argv;
  __atomic_store_n(&g_stage, 1, __ATOMIC_RELEASE);
  for (i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i)
    {
      int result;
      printf("VVBOOT start step=%s\n", steps[i]);
      result = radio_command(steps[i]);
      printf("VVBOOT done step=%s result=%d\n", steps[i], result);
      if (result != 0)
        {
          __atomic_store_n(&g_stage, -1, __ATOMIC_RELEASE);
          puts("VVBOOT failed; no automatic retry; NSH remains available");
          return result;
        }
    }
  __atomic_store_n(&g_stage, 2, __ATOMIC_RELEASE);
  puts("VVBOOT services ready; awaiting BLE provisioning; capture and motion idle");
#if defined(CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE)
  extern int k7boot_aliyun_production_worker(int, char **);
  int native = task_create("vvaliyunboot", 100, 65536,
                           k7boot_aliyun_production_worker, NULL);
  if (native < 0) perror("VVALIYUN task_create");
#elif defined(CONFIG_EXAMPLES_K7BOOT_NATIVE_CLOUD)
  extern int k7boot_native_worker(int, char **);
  int native = task_create("vvnativeboot", 100, 65536,
                           k7boot_native_worker, NULL);
  if (native < 0) perror("VVNATIVE task_create");
#endif
#ifdef CONFIG_EXAMPLES_K7BOOT_HEADLESS_TRIAL
  extern int k7boot_headless_worker(int, char **);
  int headless = task_create("vvheadless", 100, 65536,
                             k7boot_headless_worker, NULL);
  if (headless < 0) perror("VVHEADLESS task_create");
#endif
  for (;;)
    {
      if (__atomic_load_n(&g_logging, __ATOMIC_ACQUIRE))
        {
          printf("VVBOOT alive elapsed_s=%u services_ready=1\n", ticks * 10);
          /* Actual read-only radio status, not a synthetic online claim. */
          if (ticks % 6 == 0)
            {
              radio_command("wifi-status");
              radio_command("ble-status");
            }
          fflush(stdout);
        }
      sleep(10);
      ++ticks;
    }
  return 0;
}

int main(int argc, char **argv)
{
  int pid;
  if (argc == 2 && !strcmp(argv[1], "status"))
    {
      printf("VVBOOT stage=%d logging=%d\n",
             __atomic_load_n(&g_stage, __ATOMIC_ACQUIRE),
             __atomic_load_n(&g_logging, __ATOMIC_ACQUIRE));
#ifdef CONFIG_EXAMPLES_K7BOOT_ALIYUN_PRIVATE
      extern void k7boot_aliyun_status_print(void);
      k7boot_aliyun_status_print();
#endif
      return 0;
    }
  if (argc == 2 && (!strcmp(argv[1], "log-stop") ||
                    !strcmp(argv[1], "log-start")))
    {
      __atomic_store_n(&g_logging, !strcmp(argv[1], "log-start"),
                       __ATOMIC_RELEASE);
      return 0;
    }
  if (argc != 1)
    {
      puts("k7boot [status | log-stop | log-start]");
      return 1;
    }
  if (__atomic_exchange_n(&g_started, 1, __ATOMIC_ACQ_REL))
    {
      puts("VVBOOT already started; refusing another startup or console");
      return 1;
    }
  pid = task_create("vvstartup", 100, 16384, startup_worker, NULL);
  if (pid < 0)
    {
      __atomic_store_n(&g_stage, -1, __ATOMIC_RELEASE);
      perror("VVBOOT task_create");
    }
  return nsh_main(argc, argv);
}
