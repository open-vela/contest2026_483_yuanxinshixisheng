if(CONFIG_EXAMPLES_K7AUDIO)
  nuttx_add_application(NAME k7audio SRCS k7audio_main.c audio_preflight.c
    STACKSIZE 4096 PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7AUDIO})
endif()
