if(CONFIG_EXAMPLES_K7LOAD)
  nuttx_add_application(NAME k7load SRCS k7load_main.c STACKSIZE 8192
                       PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7LOAD})
endif()
