if(CONFIG_EXAMPLES_K7EMMC)
  nuttx_add_application(NAME k7emmc SRCS k7emmc_main.c sdhci_read.c
    sdhci_command.c sdhci_control.c emmc_init.c gpt_readonly.c STACKSIZE 8192 PRIORITY 100)
endif()
