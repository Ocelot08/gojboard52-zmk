# NOT CONFIG_NICE_VIEW_WIDGET_STATUS: when nice_view is also in the shield
# list, it ships its own custom_status_screen.c (canvas-drawn, not simple
# LVGL labels) - both would define zmk_display_status_screen() and fail to
# link. Defer to nice_view's own screen in that case; the peripheral battery
# widget doesn't show there yet (would need integrating into nice_view's own
# draw_top(), not a reuse of this one - see project memory).
if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM AND NOT CONFIG_NICE_VIEW_WIDGET_STATUS AND NOT CONFIG_NICE_VIEW_DUAL_STATUS)
  zephyr_library_sources(custom_status_screen.c)
  zephyr_library_sources(widgets/peripheral_battery_status.c)
endif()
