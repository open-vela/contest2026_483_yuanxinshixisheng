include $(APPDIR)/Make.defs
PROGNAME = k7display
PRIORITY = 100
STACKSIZE = 4096
MODULE = $(CONFIG_EXAMPLES_K7DISPLAY)
MAINSRC = k7display_main.c
CSRCS = f050008m01.c panel_sequence.c rk3576_panel_board.c rk3576_dsi2.c rk3576_dcphy.c rk3576_vop.c display_session.c rk3576_display_resources.c
CSRCS += rk3576_display_transition.c rk3576_display_framebuffer.c rk3576_display_native_io.c rk3576_display_native.c
CSRCS += rk3576_display_guard.c
include $(APPDIR)/Application.mk
