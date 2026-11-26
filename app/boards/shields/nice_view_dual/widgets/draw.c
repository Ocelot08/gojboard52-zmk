/*
 * nice!view dual-battery status screen: pure drawing, no ZMK event handling,
 * so it can also be built and rendered on a desktop for previews.
 *
 * Layout (160x68, drawn directly in panel coordinates, no canvas rotation).
 * Positions come from the Figma design "Panel framebuffer (annotated)",
 * rounded to whole pixels:
 *
 *   row 1 (y 5-19):  [left battery] [host icon]                  [right battery]
 *   row 2 (y 34-60): (1) (2) (3)                                     layer name
 *
 * Figma positions are at 4x and include half pixels (e.g. battery y 6.5); those are
 * rounded up here.
 */

#include <zephyr/kernel.h>

#include "util.h"

/* ---- row 1: batteries and host connection icon ---- */
#define LEFT_BATTERY_X 6
#define RIGHT_BATTERY_X 121
#define BATTERY_Y 7
/* Icon text area is right-aligned: ICON_RIGHT_X is where the glyph's right edge lands
 * (tuned against the Figma design, where the Wi-Fi glyph spans x 44-62). */
#define ICON_RIGHT_X 63
#define ICON_Y 3
#define ICON_AREA_W 68

/* ---- row 2: Bluetooth profile circles and layer name ---- */
#define PROFILE_FIRST_CX 19
#define PROFILE_PITCH 30
#define PROFILE_CY 47
#define PROFILE_RING_R 13
#define PROFILE_DISC_R 9
#define LAYER_NAME_X 96
#define LAYER_NAME_Y 39
/* Right-aligned box x 96-153 (the Figma box is 58 px wide; its right edge is the text edge). */
#define LAYER_NAME_W 57

static void draw_host_icon(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);

    char text[10] = {};

    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_NONE:
        strcat(text, LV_SYMBOL_CLOSE);
        break;
    case ZMK_TRANSPORT_USB:
        strcat(text, LV_SYMBOL_USB);
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            if (state->active_profile_connected) {
                strcat(text, LV_SYMBOL_WIFI);
            } else {
                strcat(text, LV_SYMBOL_CLOSE);
            }
        } else {
            strcat(text, LV_SYMBOL_SETTINGS);
        }
        break;
    }

    canvas_draw_text(canvas, ICON_RIGHT_X - ICON_AREA_W, ICON_Y, ICON_AREA_W, &label_dsc, text);
}

/* One circle per Bluetooth profile slot:
 *   solid ring  = a device on this slot is connected
 *   dashed ring = bonded to a device but not connected
 *   no ring     = empty slot
 *   filled disc = the active slot (number drawn in the background colour)
 */
static void draw_profiles(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_arc_dsc_t arc_dsc;
    init_arc_dsc(&arc_dsc, LVGL_FOREGROUND, 2);
    lv_draw_arc_dsc_t arc_dsc_filled;
    init_arc_dsc(&arc_dsc_filled, LVGL_FOREGROUND, PROFILE_DISC_R);
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t label_dsc_inverted;
    init_label_dsc(&label_dsc_inverted, LVGL_BACKGROUND, &lv_font_montserrat_18,
                   LV_TEXT_ALIGN_CENTER);

    for (int i = 0; i < PROFILE_COUNT; i++) {
        const int cx = PROFILE_FIRST_CX + i * PROFILE_PITCH;
        const int cy = PROFILE_CY;
        const bool selected = i == state->active_profile_index;

        if (state->profiles_connected[i]) {
            canvas_draw_arc(canvas, cx, cy, PROFILE_RING_R, 0, 360, &arc_dsc);
        } else if (state->profiles_bonded[i]) {
            const int segments = 8;
            const int gap = 20;
            for (int j = 0; j < segments; ++j) {
                canvas_draw_arc(canvas, cx, cy, PROFILE_RING_R, 360. / segments * j + gap / 2.0,
                                360. / segments * (j + 1) - gap / 2.0, &arc_dsc);
            }
        }

        if (selected) {
            canvas_draw_arc(canvas, cx, cy, PROFILE_DISC_R, 0, 359, &arc_dsc_filled);
        }

        char label[2];
        snprintf(label, sizeof(label), "%d", i + 1);
        canvas_draw_text(canvas, cx - 8, cy - 10, 16, selected ? &label_dsc_inverted : &label_dsc,
                         label);
    }
}

/* Shortens text so it fits max_w pixels, ending in ".." when it had to be cut. */
static const char *fit_text(const char *txt, const lv_font_t *font, int32_t max_w, char *buf,
                            size_t buf_size) {
    size_t n = strlen(txt);
    if (lv_text_get_width(txt, n, font, 0) <= max_w) {
        return txt;
    }
    if (n > buf_size - 3) {
        n = buf_size - 3;
    }
    while (n > 0) {
        memcpy(buf, txt, n);
        buf[n] = '\0';
        strcat(buf, "..");
        if (lv_text_get_width(buf, strlen(buf), font, 0) <= max_w) {
            return buf;
        }
        n--;
    }
    return "..";
}

static void draw_layer_name(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t name_dsc;
    init_label_dsc(&name_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    /* Never wrap; names too wide for the box are shortened by fit_text(). */
    name_dsc.flag = LV_TEXT_FLAG_EXPAND;

    char text[24] = {};
    char fitted[24] = {};

    /* Layer name, or "Ln" (e.g. "L4") when the keymap layer has no label. */
    if (state->layer_label == NULL || strlen(state->layer_label) == 0) {
        snprintf(text, sizeof(text), "L%d", state->layer_index);
    } else {
        strncpy(text, state->layer_label, sizeof(text) - 1);
    }

    canvas_draw_text(canvas, LAYER_NAME_X, LAYER_NAME_Y, LAYER_NAME_W, &name_dsc,
                     fit_text(text, &lv_font_montserrat_14, LAYER_NAME_W, fitted, sizeof(fitted)));
}

void draw_status(lv_obj_t *canvas, const struct status_state *state) {
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    /* Row 1 */
    draw_battery(canvas, LEFT_BATTERY_X, BATTERY_Y, state->battery, state->charging);
    draw_host_icon(canvas, state);
    draw_battery(canvas, RIGHT_BATTERY_X, BATTERY_Y, state->peripheral_battery, false);

    /* Row 2 */
    draw_profiles(canvas, state);
    draw_layer_name(canvas, state);

#if IS_ENABLED(CONFIG_NICE_VIEW_DUAL_ROTATE_180)
    flip_canvas_180(canvas);
#endif
}
