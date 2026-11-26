/*
 * Drawing helpers for the nice!view dual-battery status screen.
 * Helper bodies are adapted from ZMK's stock nice_view widgets (MIT).
 */

#pragma once

#include <lvgl.h>
#include <zmk/endpoints.h>

#define SCREEN_W 160
#define SCREEN_H 68

#define CANVAS_COLOR_FORMAT LV_COLOR_FORMAT_L8 // smallest type supported by sw_rotate
#define CANVAS_BUF_SIZE                                                                            \
    LV_CANVAS_BUF_SIZE(SCREEN_W, SCREEN_H, LV_COLOR_FORMAT_GET_BPP(CANVAS_COLOR_FORMAT),           \
                       LV_DRAW_BUF_STRIDE_ALIGN)

#define LVGL_BACKGROUND                                                                            \
    IS_ENABLED(CONFIG_NICE_VIEW_DUAL_INVERTED) ? lv_color_black() : lv_color_white()
#define LVGL_FOREGROUND                                                                            \
    IS_ENABLED(CONFIG_NICE_VIEW_DUAL_INVERTED) ? lv_color_white() : lv_color_black()

/* Number of Bluetooth profile circles shown (profiles 1..PROFILE_COUNT). */
#define PROFILE_COUNT 3

/* Everything the screen needs to draw itself. Filled in by status.c. */
struct status_state {
    uint8_t battery;            /* this (central) half, 0-100 */
    bool charging;              /* USB power present on this half */
    uint8_t peripheral_battery; /* other half, last reported level, 0-100 */
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected; /* active profile, for the host icon */
    bool active_profile_bonded;
    bool profiles_connected[PROFILE_COUNT];
    bool profiles_bonded[PROFILE_COUNT];
    uint8_t layer_index;
    const char *layer_label;
};

/* Draws the whole 160x68 screen into the canvas. */
void draw_status(lv_obj_t *canvas, const struct status_state *state);

void draw_battery(lv_obj_t *canvas, int x, int y, uint8_t level, bool charging);
void flip_canvas_180(lv_obj_t *canvas);

void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align);
void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color);
void init_arc_dsc(lv_draw_arc_dsc_t *arc_dsc, lv_color_t color, uint8_t width);

void canvas_draw_rect(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                      lv_draw_rect_dsc_t *draw_dsc);
void canvas_draw_arc(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t r,
                     int32_t start_angle, int32_t end_angle, lv_draw_arc_dsc_t *draw_dsc);
void canvas_draw_text(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t max_w,
                      lv_draw_label_dsc_t *draw_dsc, const char *txt);
void canvas_draw_img(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, const lv_image_dsc_t *src,
                     lv_draw_image_dsc_t *draw_dsc);
