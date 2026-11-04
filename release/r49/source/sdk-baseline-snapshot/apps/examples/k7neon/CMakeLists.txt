if(CONFIG_EXAMPLES_K7NEON)
  nuttx_add_application(NAME k7neon SRCS k7neon_main.c neon_hold.S
                       STACKSIZE 8192 PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7NEON})
  set_source_files_properties(k7neon_main.c PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()
