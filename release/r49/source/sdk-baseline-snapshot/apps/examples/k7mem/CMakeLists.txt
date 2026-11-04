if(CONFIG_EXAMPLES_K7MEM)
  nuttx_add_application(NAME k7mem SRCS k7mem_main.c STACKSIZE 4096 PRIORITY 100)
endif()
