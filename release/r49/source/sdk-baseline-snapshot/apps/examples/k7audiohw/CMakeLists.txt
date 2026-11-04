if(CONFIG_EXAMPLES_K7AUDIOHW)
  nuttx_add_application(NAME k7audiohw SRCS k7audiohw_main.c live.c i2c3_timing.c
    STACKSIZE 8192 PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7AUDIOHW})
endif()
