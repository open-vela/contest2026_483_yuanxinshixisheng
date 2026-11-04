if(CONFIG_EXAMPLES_K7USB)
  nuttx_add_application(NAME k7usb SRCS k7usb_main.c
                        STACKSIZE 4096 PRIORITY 100)
endif()
