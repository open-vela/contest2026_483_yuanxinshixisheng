if(CONFIG_EXAMPLES_K7DISPLAY)
  nuttx_add_application(NAME k7display
    SRCS k7display_main.c f050008m01.c panel_sequence.c
         rk3576_panel_board.c rk3576_dsi2.c rk3576_dcphy.c rk3576_vop.c
         display_session.c rk3576_display_resources.c rk3576_display_transition.c
         rk3576_display_framebuffer.c rk3576_display_native_io.c
         rk3576_display_native.c rk3576_display_guard.c
    STACKSIZE 4096 PRIORITY 100)
endif()
