if(CONFIG_EXAMPLES_K7SMP)
  nuttx_add_application(NAME k7smp SRCS k7smp_main.c STACKSIZE 8192
                       PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7SMP})
endif()
