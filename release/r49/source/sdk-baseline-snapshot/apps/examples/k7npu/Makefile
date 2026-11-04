include $(APPDIR)/Make.defs
PROGNAME = k7npu
PRIORITY = 100
STACKSIZE = 4096
MODULE = $(CONFIG_EXAMPLES_K7NPU)
MAINSRC = k7npu_main.c
ifeq ($(CONFIG_K7NPU_MMU_TEST),y)
CSRCS = k7npu_mmutest.c rk3576_npu_mmu.c rk3576_npu_window.c
endif
ifeq ($(CONFIG_K7NPU_RAW_TEST),y)
CSRCS += k7npu_rawtest.c k7npu_fixture_io.c
endif
include $(APPDIR)/Application.mk
