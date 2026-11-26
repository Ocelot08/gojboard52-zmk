/*
 * Copyright (c) 2023 Kelly Helmut Lord
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT microchip_at42qt2120

#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include "at42qt2120.h"

LOG_MODULE_REGISTER(AT42QT2120, CONFIG_SENSOR_LOG_LEVEL);

/* Chip bring-up (reset, calibrate, configure) runs on this dedicated queue at the
 * lowest application priority so it can never delay keys or Bluetooth. */
#define AT42QT2120_WQ_STACK_SIZE 1536
static K_THREAD_STACK_DEFINE(at42qt2120_wq_stack, AT42QT2120_WQ_STACK_SIZE);
static struct k_work_q at42qt2120_wq;
static bool at42qt2120_wq_started;

#define AT42QT2120_MAX_STARTUP_ATTEMPTS 5
#define AT42QT2120_STARTUP_RETRY_MS 2000
#define AT42QT2120_CALIBRATION_TIMEOUT_MS 1000

/* Lock-up detection (see at42qt2120_note_calibration / _recheck_work). */
#define AT42QT2120_CAL_WINDOW_MS 5000
#define AT42QT2120_CAL_HITS_LIMIT 6
#define AT42QT2120_RECHECK_DELAY_MS 200
#define AT42QT2120_STUCK_RETRIES_LIMIT 3
#define AT42QT2120_RECOVER_COOLDOWN_MS 30000

static int at42qt2120_reset(const struct device *dev)
{
	const struct at42qt2120_config *cfg = dev->config;
	int ret;

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_RESET, AT42QT2120_RESET_VALUE);

	k_msleep(300);

	return ret;
}

static void at42qt2120_note_calibration(struct at42qt2120_data *data);

static int at42qt2120_get_slider_position(const struct at42qt2120_config *cfg, struct at42qt2120_data *data) {
	int ret;

	uint8_t status;
	uint8_t new_pos;

	/* All status registers must be read to clear the CHANGE pin */
	ret = i2c_reg_read_byte_dt(&cfg->i2c, AT42QT2120_REG_DETECTION_STATUS, &status);
	if (ret) {
		LOG_ERR("Could not read detection status %d", ret);
		return ret;
	}

	for (int i = 0; i < 2; i++) {
		uint8_t key_status;

		ret = i2c_reg_read_byte_dt(&cfg->i2c, AT42QT2120_REG_KEY_STATUS_BASE + i, &key_status);
		if (ret) {
			LOG_ERR("Could not read key status %d", ret);
			return ret;
		}
	}

	ret = i2c_reg_read_byte_dt(&cfg->i2c, AT42QT2120_REG_SLIDER_POSITION, &new_pos);
	if (ret) {
		LOG_ERR("Could not read slider position %d", ret);
		return ret;
	}

	LOG_INF("slider read: status=0x%02x sdet=%d pos=%d", status, AT42QT2120_SDET_GET(status), new_pos);

	if (AT42QT2120_CALIBRATE_GET(status)) {
		at42qt2120_note_calibration(data);
	}

	if (!AT42QT2120_SDET_GET(status)) {
		/* No finger on the slider right now. Don't report a delta, and
		 * make sure the next touch-down isn't diffed against a stale
		 * position from before the finger lifted. */
		data->delta = 0;
		data->has_prev = false;
		return 0;
	}

	if (data->has_prev) {
		data->delta = (int16_t)new_pos - (int16_t)data->rotation;
	} else {
		/* First sample of a new touch - nothing to compare against yet. */
		data->delta = 0;
		data->has_prev = true;
	}

	data->rotation = new_pos;

	return 0;
}

#ifdef CONFIG_PM_DEVICE
static int at42qt2120_set_power_mode(const struct device *dev, uint8_t power_mode) {
	const struct at42qt2120_config *cfg = dev->config;
	int ret;

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_LOW_POWER, power_mode);
	return ret;
}

static int at42qt2120_device_pm_action(const struct device *dev,
				    enum pm_device_action action)
{
	int ret;
	const struct at42qt2120_config *cfg = dev->config;
	enum pm_device_state curr_state;

	(void)pm_device_state_get(dev, &curr_state);

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:

		ret = at42qt2120_set_power_mode(dev, cfg->acquisition_time);
		if (ret) {
			return ret;
		}

		break;
	case PM_DEVICE_ACTION_SUSPEND:
		ret = at42qt2120_set_power_mode(dev, AT42QT2120_LOW_POWER_VALUE);
		if (ret) {
			return ret;
		}

		break;
	default:
		return -ENOTSUP;
	}

	return ret;
}
#endif

static int at42qt2120_sample_fetch(const struct device *dev,
				enum sensor_channel chan)
{
	struct at42qt2120_data *data = dev->data;
	const struct at42qt2120_config *cfg = dev->config;
	int ret;
	if (!data->ready) {
		/* Still starting up (or restarting after a lock-up): nothing to read yet. */
		return -EAGAIN;
	}
#ifdef CONFIG_PM_DEVICE
	enum pm_device_state state;

	(void)pm_device_state_get(dev, &state);
	if (state != PM_DEVICE_STATE_ACTIVE) {
		LOG_ERR("Sample fetch failed, device is not in active mode");
		return -ENXIO;
	}
#endif
	switch (chan) {
		case SENSOR_CHAN_ALL:
		case SENSOR_CHAN_ROTATION:
			ret = at42qt2120_get_slider_position(cfg, data);
			break;
		default:
			ret = -ENOTSUP;
			break;
	}

	return ret;
}

static int at42qt2120_channel_get(const struct device *dev,
			       enum sensor_channel chan,
			       struct sensor_value *val) {
	struct at42qt2120_data *data = dev->data;
	switch (chan) {
	case SENSOR_CHAN_ROTATION:
		/* ZMK's sensor-rotate behavior expects an incremental change
		 * since the last event here, not an absolute angle. The whole
		 * slider (0-255) counts as one full 360 degree turn. Report the
		 * fractional part in val2 (millionths of a degree) too: ZMK
		 * accumulates it, so slow swipes with small per-sample deltas
		 * no longer lose up to a degree per sample to integer rounding.
		 * val1 and val2 carry the same sign as delta. */
		int32_t scaled = (int32_t)data->delta * 360;

		val->val1 = scaled / 255;
		val->val2 = ((scaled % 255) * 1000000) / 255;
		break;
	default:
		LOG_ERR("Channel type not supported.");
		return -ENOTSUP;
	}

	return 0;
}

static int at42qt2120_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
				   sensor_trigger_handler_t handler) {
	struct at42qt2120_data *data = dev->data;

	if (trig->type != SENSOR_TRIG_DATA_READY || trig->chan != SENSOR_CHAN_ROTATION) {
		return -ENOTSUP;
	}

	data->trigger = trig;
	data->handler = handler;

	return 0;
}

static const struct sensor_driver_api at42qt2120_api_funcs = {
	.sample_fetch = at42qt2120_sample_fetch,
	.channel_get = at42qt2120_channel_get,
	.trigger_set = at42qt2120_trigger_set,
};

static void at42qt2120_work_handler(struct k_work *work) {
	struct at42qt2120_data *data = CONTAINER_OF(work, struct at42qt2120_data, work);

	/* Do NOT call at42qt2120_sample_fetch() here. ZMK's own sensor trigger
	 * consumer (trigger_sensor_data_for_position() in app/src/sensors.c)
	 * calls sensor_sample_fetch() + sensor_channel_get() itself in response
	 * to the handler call below - that's the standard Zephyr sensor-trigger
	 * contract (the handler just signals "data ready"; the consumer does the
	 * actual fetch). Fetching here too caused a double-fetch: this fetch
	 * would compute the real delta and update data->rotation to match, then
	 * ZMK's own fetch moments later would see no change since (rotation
	 * already updated) and always compute a delta of 0, silently dropping
	 * every swipe. EC11 (the reference driver) does not fetch in its own
	 * work handler for exactly this reason.
	 *
	 * This is what actually gets the swipe into ZMK's keymap sensor
	 * pipeline - without calling the handler ZMK registered via
	 * trigger_set, nothing downstream (sensor-bindings) ever fires. */
	if (!data->ready) {
		return;
	}

	if (data->handler) {
		data->handler(data->dev, data->trigger);
	}

	/* The CHANGE interrupt is edge-triggered, so if the line ever stays asserted
	 * nothing would fire again. Check shortly after the last event that ZMK's
	 * fetch really cleared it (every new edge pushes this check back). */
	k_work_reschedule_for_queue(&at42qt2120_wq, &data->recheck_work,
				    K_MSEC(AT42QT2120_RECHECK_DELAY_MS));
}

static void at42qt2120_gpio_callback(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins) {
	struct at42qt2120_data *data = CONTAINER_OF(cb, struct at42qt2120_data, gpio_cb);

	LOG_INF("CHANGE interrupt fired");

	k_work_submit(&data->work);
}

/* One-time GPIO setup. No I2C and no interrupt yet: the chip isn't ready. */
static int at42qt2120_init_interrupt(const struct device *dev) {

	struct at42qt2120_data *data = dev->data;
	const struct at42qt2120_config *cfg = dev->config;
	int ret;

	data->dev = dev;

	ret = gpio_pin_configure_dt(&cfg->change_gpio, GPIO_INPUT);
	if (ret) {
		LOG_ERR("Failed to configure gpio %s pin %d", cfg->change_gpio.port->name, cfg->change_gpio.pin);
		return ret;
	}

	gpio_init_callback(&data->gpio_cb, at42qt2120_gpio_callback, BIT(cfg->change_gpio.pin));

	ret = gpio_add_callback(cfg->change_gpio.port, &data->gpio_cb);
	if (ret) {
		LOG_ERR("Failed to add gpio callback");
		return ret;
	}

	return 0;
}

/* Clear any pending CHANGE assertion, then start listening for new ones. */
static int at42qt2120_arm_interrupt(const struct device *dev) {
	const struct at42qt2120_config *cfg = dev->config;
	uint8_t res;
	int ret;

	/* Reading the detection status and both key status bytes releases CHANGE. */
	i2c_reg_read_byte_dt(&cfg->i2c, AT42QT2120_REG_DETECTION_STATUS, &res);
	for (int i = 0; i < 2; i++) {
		i2c_reg_read_byte_dt(&cfg->i2c, AT42QT2120_REG_KEY_STATUS_BASE + i, &res);
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->change_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret) {
		LOG_ERR("Failed to configure gpio interrupt");
	}

	return ret;
}

static int at42qt2120_calibrate(const struct device *dev) {

	const struct at42qt2120_config *cfg = dev->config;
	int ret = 1;

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_CALIBRATE, AT42QT2120_CALIBRATE_VALUE);
	if (ret) {
		LOG_ERR("Could not start calibration %d", ret);
		return ret;
	}

	uint8_t calibrate_status;
	int64_t deadline = k_uptime_get() + AT42QT2120_CALIBRATION_TIMEOUT_MS;

	while (true) {
		ret = i2c_reg_read_byte_dt(&cfg->i2c, AT42QT2120_REG_DETECTION_STATUS, &calibrate_status);
		if (ret) {
			LOG_ERR("Could not read calibration status %d", ret);
			return ret;
		}
		if (AT42QT2120_CALIBRATE_GET(calibrate_status) == 0) {
			break;
		}
		if (k_uptime_get() > deadline) {
			LOG_ERR("Calibration did not finish within %d ms", AT42QT2120_CALIBRATION_TIMEOUT_MS);
			return -ETIMEDOUT;
		}
		k_msleep(5);
	}

	k_msleep(300);

	return 0;
}

static int at42qt2120_init_config(const struct device *dev) {
	const struct at42qt2120_config *cfg = dev->config;
	int ret;

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_ACQUISITION_TIME, cfg->acquisition_time);
	if (ret) {
		LOG_ERR("Could not set acquisition time %d", ret);
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_TOWARD_TOUCH_DRIFT_COMPENSATION, cfg->toward_touch_drift);
	if (ret) {
		LOG_ERR("Could not set toward touch drift %d", ret);
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_AWAY_TOUCH_DRIFT_COMPENSATION, cfg->away_touch_drift);
	if (ret) {
		LOG_ERR("Could not set away touch drift %d", ret);
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_DETECTION_INTEGRATOR, cfg->detection_integrator);
	if (ret) {
		LOG_ERR("Could not set detection integrator %d", ret);
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_TOUCH_RECALIBRATION_DELAY, cfg->touch_recal_delay);
	if (ret) {
		LOG_ERR("Could not set touch recal delay %d", ret);
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_DRIFT_HOLD_TIME, cfg->drift_hold_time);
	if (ret) {
		LOG_ERR("Could not set drift hold time %d", ret);
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_CHARGE_TIME, cfg->charge_time);
	if (ret) {
		LOG_ERR("Could not set charge time %d", ret);
		return ret;
	}

	for (int i = 3; i < 12; i++) {
		ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_KEY_CONTROL_BASE + i, AT42QT2120_KEY_CONTROL_DISABLE);
		if (ret) {
			LOG_ERR("Could not enable key %d %d", i, ret);
			return ret;
		}
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_SLIDER_OPTIONS, AT42QT2120_SLIDER_ENABLE_MSK);
	if (ret) {
		LOG_ERR("Could not enable slider %d", ret);
		return ret;
	}

	return ret;
}

/* Chip bring-up: reset, calibrate, configure. Takes about 0.7 s, so it only ever
 * runs from the startup work item, never from the device init itself. */
static int at42qt2120_bring_up(const struct device *dev) {
	const struct at42qt2120_config *cfg = dev->config;
	int ret;

	/* Don't listen while the chip is being reset: it may pulse CHANGE. */
	gpio_pin_interrupt_configure_dt(&cfg->change_gpio, GPIO_INT_DISABLE);

	ret = at42qt2120_reset(dev);
	if (ret) {
		LOG_ERR("Could not reset device %d", ret);
		return ret;
	}

	ret = at42qt2120_calibrate(dev);
	if (ret) {
		LOG_ERR("Could not calibrate device %d", ret);
		return ret;
	}

	ret = at42qt2120_init_config(dev);
	if (ret) {
		LOG_ERR("Could not initialize device %d", ret);
		return ret;
	}

	return 0;
}

static void at42qt2120_startup_work(struct k_work *work) {
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct at42qt2120_data *data = CONTAINER_OF(dwork, struct at42qt2120_data, startup_work);
	const struct device *dev = data->dev;
	int ret = at42qt2120_bring_up(dev);

	if (ret == 0) {
		data->has_prev = false;
		data->delta = 0;
		data->cal_hits = 0;
		data->cal_window_start = 0;
		data->stuck_retries = 0;

		/* Mark ready before arming so the first interrupt is never ignored. */
		data->ready = true;
		ret = at42qt2120_arm_interrupt(dev);
		if (ret) {
			data->ready = false;
		}
	}

	if (ret) {
		data->startup_attempts++;
		LOG_ERR("AT42QT2120 start-up failed (%d), attempt %u of %d", ret,
			data->startup_attempts, AT42QT2120_MAX_STARTUP_ATTEMPTS);
		if (data->startup_attempts < AT42QT2120_MAX_STARTUP_ATTEMPTS) {
			k_work_reschedule_for_queue(&at42qt2120_wq, &data->startup_work,
						    K_MSEC(AT42QT2120_STARTUP_RETRY_MS));
		}
		return;
	}

	data->startup_attempts = 0;
	LOG_INF("AT42QT2120 ready");
}

/* Restart the chip when it has clearly locked up (touch bar silent until the half was
 * reset). Safe to call from any thread; at most once per cooldown period. */
static void at42qt2120_request_recover(const struct device *dev, const char *reason) {
	struct at42qt2120_data *data = dev->data;
	const struct at42qt2120_config *cfg = dev->config;
	int64_t now = k_uptime_get();

	if (!data->ready) {
		return; /* a restart is already under way */
	}
	if (data->last_recover_ms != 0 && now - data->last_recover_ms < AT42QT2120_RECOVER_COOLDOWN_MS) {
		return;
	}

	data->last_recover_ms = now;
	data->ready = false;
	gpio_pin_interrupt_configure_dt(&cfg->change_gpio, GPIO_INT_DISABLE);

	LOG_WRN("AT42QT2120 %s: restarting the chip", reason);
	k_work_reschedule_for_queue(&at42qt2120_wq, &data->startup_work, K_NO_WAIT);
}

/* A healthy chip sets its CALIBRATE flag once in a while. Seeing it over and over
 * in a short window means it is stuck recalibrating (observed with a noisy supply:
 * CHANGE firing every 100-300 ms with the slider pinned at 0). */
static void at42qt2120_note_calibration(struct at42qt2120_data *data) {
	int64_t now = k_uptime_get();

	if (now - data->cal_window_start > AT42QT2120_CAL_WINDOW_MS) {
		data->cal_window_start = now;
		data->cal_hits = 0;
	}

	if (++data->cal_hits >= AT42QT2120_CAL_HITS_LIMIT) {
		data->cal_hits = 0;
		at42qt2120_request_recover(data->dev, "stuck recalibrating");
	}
}

/* The CHANGE interrupt is edge-triggered. If the line is still asserted a while after
 * the last event, no new edge can ever arrive: re-read, and restart the chip if that
 * does not release it. */
static void at42qt2120_recheck_work(struct k_work *work) {
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct at42qt2120_data *data = CONTAINER_OF(dwork, struct at42qt2120_data, recheck_work);
	const struct device *dev = data->dev;
	const struct at42qt2120_config *cfg = dev->config;

	if (!data->ready) {
		return;
	}

	if (gpio_pin_get_dt(&cfg->change_gpio) > 0) {
		if (++data->stuck_retries > AT42QT2120_STUCK_RETRIES_LIMIT) {
			data->stuck_retries = 0;
			at42qt2120_request_recover(dev, "CHANGE line stuck asserted");
			return;
		}
		LOG_WRN("AT42QT2120 CHANGE still asserted, re-reading (%u)", data->stuck_retries);
		k_work_submit(&data->work);
		return;
	}

	data->stuck_retries = 0;
}

static int at42qt2120_init(const struct device *dev)
{
	const struct at42qt2120_config *cfg = dev->config;
	struct at42qt2120_data *data = dev->data;

	if (!device_is_ready(cfg->i2c.bus)) {
		LOG_ERR("I2C bus device not ready");
		return -ENODEV;
	}

	data->dev = dev;
	k_work_init(&data->work, at42qt2120_work_handler);
	k_work_init_delayable(&data->startup_work, at42qt2120_startup_work);
	k_work_init_delayable(&data->recheck_work, at42qt2120_recheck_work);

	if (!at42qt2120_wq_started) {
		k_work_queue_init(&at42qt2120_wq);
		k_work_queue_start(&at42qt2120_wq, at42qt2120_wq_stack,
				   K_THREAD_STACK_SIZEOF(at42qt2120_wq_stack),
				   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
		k_thread_name_set(&at42qt2120_wq.thread, "at42qt2120");
		at42qt2120_wq_started = true;
	}

	if (at42qt2120_init_interrupt(dev) < 0) {
		LOG_ERR("Failed to initialize interrupt!");
		return -EIO;
	}

	/* Deliberately not calling pm_device_runtime_enable() here: ZMK's own
	 * zmk_pm_suspend_devices()/zmk_pm_resume_devices() (app/src/pm.c) walk
	 * every device and suspend/resume it on deep sleep - but they skip any
	 * device with runtime PM enabled, on the assumption something else is
	 * managing its power via get/put calls. Nothing does for this sensor,
	 * so enabling runtime PM here would silently opt this device out of
	 * ZMK's sleep-triggered suspend and defeat the point of having
	 * at42qt2120_device_pm_action() (LP=0 on suspend) at all. */

	/* The slow part (reset + calibration, ~0.7 s) runs in the background so the
	 * rest of the keyboard - keys, Bluetooth - doesn't wait for the touch bar. */
	k_work_schedule_for_queue(&at42qt2120_wq, &data->startup_work, K_NO_WAIT);

	LOG_INF("AT42QT2120 start-up scheduled");

	return 0;
}

#define AT42QT2120_CHANGE_PROPS(n)                                      \
	.change_gpio = GPIO_DT_SPEC_INST_GET(n, change_gpios),              \

#define AT42QT2120_CHANGE(n)				                            \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(n, change_gpios),                  \
		   (AT42QT2120_CHANGE_PROPS(n)))

#define AT42QT2120_INIT(n)							                    \
	static const struct at42qt2120_config at42qt2120_config_##n = {	    \
		.i2c = I2C_DT_SPEC_INST_GET(n),				                    \
		.acquisition_time = DT_INST_PROP(n, acquisition_time),	        \
		.idle_acquisition_time = DT_INST_PROP(n, idle_acquisition_time), \
		.toward_touch_drift = DT_INST_PROP(n, toward_touch_drift),	    \
		.away_touch_drift = DT_INST_PROP(n, away_touch_drift),	        \
		.detection_integrator = DT_INST_PROP(n, detection_integrator),  \
		.touch_recal_delay = DT_INST_PROP(n, touch_recal_delay),	    \
		.drift_hold_time = DT_INST_PROP(n, drift_hold_time),	        \
		.charge_time = DT_INST_PROP(n, charge_time),		            \
		AT42QT2120_CHANGE(n)				                            \
	};								                                    \
																		\
	static struct at42qt2120_data at42qt2120_data_##n = {               \
		.rotation = 0,                                                  \
	};																	\
                                 									    \
	PM_DEVICE_DT_INST_DEFINE(n, at42qt2120_device_pm_action);		    \
									                                    \
	DEVICE_DT_INST_DEFINE(n,					                    \
			      at42qt2120_init,				                        \
			      PM_DEVICE_DT_INST_GET(n),			                    \
			      &at42qt2120_data_##n,			                        \
			      &at42qt2120_config_##n,			                    \
			      POST_KERNEL,				                            \
			      CONFIG_SENSOR_INIT_PRIORITY,		                    \
			      &at42qt2120_api_funcs);

DT_INST_FOREACH_STATUS_OKAY(AT42QT2120_INIT)

/* Idle-throttle: slow the scan rate (not a full power-down - the chip can't
 * sense the touch that would wake it back up once fully off, see
 * idle-acquisition-time in the binding) while ZMK reports no keyboard
 * activity, and restore full speed the moment anything happens. This is
 * independent of the PM_DEVICE suspend/resume above, which writes LP=0 and
 * is reserved for a real deep-sleep path if one gets built later. */
#define AT42QT2120_DEVICE_PTR(n) DEVICE_DT_INST_GET(n),

static const struct device *const at42qt2120_devices[] = {
	DT_INST_FOREACH_STATUS_OKAY(AT42QT2120_DEVICE_PTR)
};

static int at42qt2120_activity_listener(const zmk_event_t *eh) {
	bool is_idle = zmk_activity_get_state() == ZMK_ACTIVITY_IDLE;

	for (size_t i = 0; i < ARRAY_SIZE(at42qt2120_devices); i++) {
		const struct device *dev = at42qt2120_devices[i];
		const struct at42qt2120_config *cfg = dev->config;
		const struct at42qt2120_data *data = dev->data;
		uint8_t lp = is_idle ? cfg->idle_acquisition_time : cfg->acquisition_time;

		if (!data->ready) {
			continue; /* (re)starting: the chip is configured with the normal rate anyway */
		}

		int ret = i2c_reg_write_byte_dt(&cfg->i2c, AT42QT2120_REG_LOW_POWER, lp);
		if (ret) {
			LOG_WRN("Could not update AT42QT2120 scan rate for idle state (%d)", ret);
		}
	}

	return 0;
}

ZMK_LISTENER(at42qt2120_activity, at42qt2120_activity_listener);
ZMK_SUBSCRIPTION(at42qt2120_activity, zmk_activity_state_changed);

