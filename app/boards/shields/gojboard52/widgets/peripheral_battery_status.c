/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/central.h>

#include "peripheral_battery_status.h"

/* gojboard52 only ever has one peripheral (the other half), so the split
 * source index is always 0 - no need to track/display more than one. */
#define PERIPHERAL_SOURCE 0

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_battery_status_state {
    uint8_t level;
};

static void set_battery_symbol(lv_obj_t *label, struct peripheral_battery_status_state state) {
    char text[9] = {};
    uint8_t level = state.level;

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS_SHOW_PERCENTAGE)
    snprintf(text, sizeof(text), "%3u%%", level);
#else
    if (level > 95) {
        strcpy(text, LV_SYMBOL_BATTERY_FULL);
    } else if (level > 65) {
        strcpy(text, LV_SYMBOL_BATTERY_3);
    } else if (level > 35) {
        strcpy(text, LV_SYMBOL_BATTERY_2);
    } else if (level > 5) {
        strcpy(text, LV_SYMBOL_BATTERY_1);
    } else {
        strcpy(text, LV_SYMBOL_BATTERY_EMPTY);
    }
#endif
    lv_label_set_text(label, text);
}

static void peripheral_battery_status_update_cb(struct peripheral_battery_status_state state) {
    struct zmk_widget_peripheral_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_symbol(widget->obj, state); }
}

static struct peripheral_battery_status_state
peripheral_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev != NULL && ev->source == PERIPHERAL_SOURCE) {
        return (struct peripheral_battery_status_state){.level = ev->state_of_charge};
    }

    /* Not triggered by a real event yet (widget init) - ask directly instead
     * of showing nothing until the first update arrives. */
    uint8_t level = 0;
    zmk_split_central_get_peripheral_battery_level(PERIPHERAL_SOURCE, &level);
    return (struct peripheral_battery_status_state){.level = level};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_battery_status,
                            struct peripheral_battery_status_state,
                            peripheral_battery_status_update_cb,
                            peripheral_battery_status_get_state)

ZMK_SUBSCRIPTION(widget_peripheral_battery_status, zmk_peripheral_battery_state_changed);

int zmk_widget_peripheral_battery_status_init(struct zmk_widget_peripheral_battery_status *widget,
                                              lv_obj_t *parent) {
    widget->obj = lv_label_create(parent);

    sys_slist_append(&widgets, &widget->node);

    widget_peripheral_battery_status_init();
    return 0;
}

lv_obj_t *
zmk_widget_peripheral_battery_status_obj(struct zmk_widget_peripheral_battery_status *widget) {
    return widget->obj;
}
