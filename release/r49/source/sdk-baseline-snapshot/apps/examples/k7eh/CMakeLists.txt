if(CONFIG_EXAMPLES_K7EH)
  nuttx_add_application(NAME k7eh SRCS k7eh_main.cxx STACKSIZE 16384
                       PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7EH})
endif()
