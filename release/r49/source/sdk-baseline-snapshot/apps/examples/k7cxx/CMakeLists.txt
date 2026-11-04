if(CONFIG_EXAMPLES_K7CXX)
  nuttx_add_application(NAME k7cxx SRCS k7cxx_main.cxx STACKSIZE 16384
                       PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7CXX})
endif()
