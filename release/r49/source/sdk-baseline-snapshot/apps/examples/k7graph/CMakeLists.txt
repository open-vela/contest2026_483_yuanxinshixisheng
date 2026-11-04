if(CONFIG_EXAMPLES_K7GRAPH)
  include_directories(vendor/ggml/include vendor/ggml/src vendor/ggml/src/ggml-cpu include)
  add_compile_definitions(GGML_USE_CPU GGML_POOL_POSIX=1 GGML_SCHED_MAX_COPIES=4 _GNU_SOURCE)
  # Keep vendor warnings visible without inheriting application-specific fatality.
  add_compile_options(-Wno-error=undef -Wno-error=unused-function)
  nuttx_add_application(NAME k7graph SRCS k7graph_main.c graph_probe.c graph_probe_main.c
    vendor/ggml/src/ggml.c
    vendor/ggml/src/ggml-alloc.c
    vendor/ggml/src/ggml-backend.cpp
    vendor/ggml/src/ggml-opt.cpp
    vendor/ggml/src/ggml-threading.cpp
    vendor/ggml/src/ggml-quants.c
    vendor/ggml/src/gguf.cpp
    candidate/ggml-cpu.c
    vendor/ggml/src/ggml-cpu/ggml-cpu.cpp
    vendor/ggml/src/ggml-cpu/ggml-cpu-aarch64.cpp
    vendor/ggml/src/ggml-cpu/ggml-cpu-hbm.cpp
    vendor/ggml/src/ggml-cpu/ggml-cpu-quants.c
    vendor/ggml/src/ggml-cpu/ggml-cpu-traits.cpp
    vendor/ggml/src/ggml-cpu/amx/amx.cpp
    vendor/ggml/src/ggml-cpu/amx/mmq.cpp
    vendor/ggml/src/ggml-cpu/binary-ops.cpp
    vendor/ggml/src/ggml-cpu/unary-ops.cpp
    candidate/ggml-backend-reg.cpp
    STACKSIZE 16384 PRIORITY 100 MODULE ${CONFIG_EXAMPLES_K7GRAPH})
endif()
