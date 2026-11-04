if(CONFIG_EXAMPLES_GIMBAL)
  nuttx_add_application(NAME gimbal SRCS gimbal_main.c gimbal_link.c
                        STACKSIZE 4096 PRIORITY 100)
endif()
