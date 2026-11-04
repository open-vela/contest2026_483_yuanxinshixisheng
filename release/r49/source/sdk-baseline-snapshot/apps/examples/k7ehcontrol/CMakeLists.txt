if(CONFIG_EXAMPLES_K7EHCONTROL)
  nuttx_add_application(NAME k7ehcontrol SRCS k7ehcontrol_main.cxx STACKSIZE 16384
                       PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7EHCONTROL})
endif()
