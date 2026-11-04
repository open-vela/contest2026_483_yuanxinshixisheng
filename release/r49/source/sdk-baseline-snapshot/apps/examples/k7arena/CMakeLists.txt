if(CONFIG_EXAMPLES_K7ARENA)
  include_directories(../k7graph/vendor/ggml/include ../k7graph/vendor/ggml/src ../k7graph/vendor/ggml/src/ggml-cpu)
  nuttx_add_application(NAME k7arena SRCS k7arena_main.c k7_target_provider.c
    k7_arena_diagnostic.c k7_arena_provider_main.c k7_host_buft.cpp
    STACKSIZE 16384 PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7ARENA})
endif()
