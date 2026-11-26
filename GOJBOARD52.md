# gojboard52 firmware (ZMK fork)

Firmware for **gojboard52**, a split wireless keyboard on two nice!nano v2 controllers. This is a fork of
[zmkfirmware/zmk](https://github.com/zmkfirmware/zmk) (`main`, Zephyr 4.1) with the pieces below added. Nothing in
ZMK core is modified apart from two lines that register the touch-bar driver.

Hardware (PCBs, case, photos): https://github.com/Ocelot08/gojboard52-hardware

## What is added

| Path | What it is |
| --- | --- |
| `app/boards/shields/gojboard52/` | The shield: matrix, keymap, split config, deep sleep, OLED and nice!view options |
| `app/boards/shields/nice_view_dual/` | A custom nice!view screen: left and right battery, three BLE profile circles, layer name |
| `app/module/drivers/sensor/at42qt2120/` | Driver for the Microchip AT42QT2120 touch slider (right half), with deferred start-up and self-recovery |
| `app/module/dts/bindings/sensor/microchip,at42qt2120.yaml` | Devicetree binding for that driver |
| `app/module/drivers/sensor/{CMakeLists.txt,Kconfig}` | One line each to register the driver |
| `.github/workflows/build-gojib.yml` | CI that builds all firmware images |
| `scripts/draw_keymap.py` | Draws every keymap layer as a PNG (`python scripts/draw_keymap.py`, needs Pillow) |
| `scripts/watch_serial.ps1` | Serial console viewer for debug builds |

The keymap is `app/boards/shields/gojboard52/gojboard52.keymap` (4 layers). The touch bar sends volume up/down
(`triggers-per-rotation = <36>`, about 72% of the volume range per full swipe).

## Firmware images

| File | Flash to |
| --- | --- |
| `gojboard52_left_niceview.uf2` | Left half, with a nice!view display |
| `gojboard52_left_oled.uf2` | Left half, with an SSD1306 OLED |
| `gojboard52_right.uf2` | Right half (touch bar) |
| `settings_reset.uf2` | Either half, to clear pairing and settings |

Get them from the **Actions** tab: open the latest "Build Gojiboard52" run and download the `gojboard52-firmware`
artifact. Double-tap reset on the nice!nano and copy the file onto the drive that appears.

## Building locally

```
west init -l app
west update
west build -s app -d build/right -b "nice_nano@2.0.0/nrf52840/zmk" -- -DSHIELD="gojboard52_right"
west build -s app -d build/left_oled -b "nice_nano@2.0.0/nrf52840/zmk" -- -DSHIELD="gojboard52_left"
west build -s app -d build/left_niceview -b "nice_nano@2.0.0/nrf52840/zmk" -- \
  -DSHIELD="gojboard52_left nice_view_adapter nice_view_dual" \
  -DEXTRA_DTC_OVERLAY_FILE="boards/shields/gojboard52/nice_view_compat.overlay"
```

## Sleep behaviour

Idle after 30 s; deep sleep after 12 hours without a keypress (`CONFIG_ZMK_IDLE_SLEEP_TIMEOUT` in
`gojboard52_left.conf` and `gojboard52_right.conf`). A keypress on either half wakes that half.
Sleep and wake work on hardware; battery life has not been measured.

## Status

Working on hardware: keys, split link, nice!view screen, touch-bar volume (including the deferred start-up and
recovery changes), and deep sleep and wake. Not yet measured: battery life.

## Upstream

This fork tracks `zmkfirmware/zmk` `main`. ZMK's own README follows in `README.md`; ZMK is MIT licensed (see `LICENSE`).
