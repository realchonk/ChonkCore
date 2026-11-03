# Rebuilding the ESP32-S3 IDF libraries with power management

The stock Arduino-ESP32 2.0.17 libraries (shipped inside PlatformIO's
`framework-arduinoespressif32` package) are built with `CONFIG_PM_ENABLE`
disabled, so automatic light sleep (`esp_pm`), FreeRTOS tickless idle and BLE
modem sleep are compiled out. This fork runs a custom build of those
libraries with power management enabled, plus a small instrumentation patch.

## Why this matters

- `esp_pm_configure(light_sleep_enable=true)` is what lets the CPU suspend
  between radio events (~17 mA awake -> ~2.5 mA asleep at 80 MHz)
- BLE modem sleep only releases the controller's `APB_FREQ_MAX` PM lock when
  compiled in AND configured with a valid sleep clock -- without it, light
  sleep can never engage while Bluetooth is on
- The patch adds two globals (`g_pm_sleep_us`, `g_pm_sleep_count`) to
  `pm_impl.c` so the firmware can report the exact sleep duty cycle
  (the `power` CLI command)

## Recipe

Requirements: docker, ~10 GB disk, network for the initial pulls.

```bash
# one-time: sources + image
git clone --depth 1 --branch 2.0.17 https://github.com/espressif/arduino-esp32.git
docker pull espressif/idf:v4.4.7

# project skeleton (kept in this repo under tools/libbuild-template):
# - CMakeLists.txt with set(SDKCONFIG_DEFAULTS "/project/sdkconfig.defaults")
# - main/CMakeLists.txt REQUIRES: bt esp_wifi nvs_flash driver esp_lcd fatfs
#   spiffs protobuf-c mqtt json app_trace app_update esp_https_ota ... (see file)
# - main/main.c: void app_main(void) {}
# - sdkconfig.defaults = arduino-esp32/tools/sdk/esp32s3/sdkconfig with these
#   lines appended:
#       CONFIG_PM_ENABLE=y
#       CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
#       CONFIG_BT_CTRL_MODEM_SLEEP=y
#       CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y
#       CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y
# - pm_impl.c patched to accumulate sleep time (see
#   tools/libbuild-template/pm-sleep-instrumentation.patch)

docker run --rm --network none \
  -v $PWD:/project -v $PWD/../arduino-esp32:/arduino \
  -v $PWD/pm_impl_patched.c:/opt/esp/idf/components/esp_pm/pm_impl.c:ro \
  -w /project espressif/idf:v4.4.7 idf.py build

# swap the PM-affected libs into the PlatformIO package (back up first!)
# flat lib/ set:      bt driver esp_lcd esp_pm esp_wifi espcoredump
# qio_qspi/ set:      esp_hw_support esp_system freertos spi_flash
# plus, crucially:    build/config/sdkconfig.h merged into
#                     qio_qspi/include/sdkconfig.h (see below)
```

Verify every other lib is symbol-identical before leaving stock ones in place
(`nm` set comparison) -- only ~10 of ~120 libs change with PM enabled.

## Landmines (all of which we hit)

1. **`SDKCONFIG_DEFAULTS` only seeds a NEW sdkconfig.** Appending lines to
   the defaults file does nothing once `sdkconfig` exists -- edit the
   generated `sdkconfig` directly (or delete it).
2. **Kconfig silently drops options with unmet dependencies.**
   `BT_CTRL_MODEM_SLEEP` requires `!BT_CTRL_HCI_MODE_UART_H4` -- always
   re-grep the generated sdkconfig to confirm your options took.
3. **Header/macro split brain.** The libs are half the story:
   `BT_CONTROLLER_INIT_CONFIG_DEFAULT()` is a *header macro* expanded when
   the Arduino BLE library compiles, using the package's
   `tools/sdk/esp32s3/<memory_type>/include/sdkconfig.h`. Swapping `.a`
   files without merging that header leaves modem sleep off at runtime.
   Merge: stock header + your deltas. Do NOT copy the docker-generated
   header wholesale -- it lacks ~160 Arduino-specific defines (starting
   with `CONFIG_AUTOSTART_ARDUINO`, whose absence breaks the link with
   `undefined reference to app_main`).
4. **Main-XTAL BLE sleep clock needs `MAIN_XTAL_PU_DURING_LIGHT_SLEEP`**,
   otherwise the controller holds a `btLS` NO_LIGHT_SLEEP lock forever
   (no external 32 kHz crystal on most boards).
5. **The Arduino loopTask never yields.** Even perfect libs won't sleep
   unless the main loop blocks (we `vTaskDelay` when idle); and a USB data
   host injects 1 ms SOF interrupts that keep idle windows under the
   `FREERTOS_IDLE_TIME_BEFORE_SLEEP` (3 ms) threshold -- on battery only.
6. **PlatformIO does not track framework lib mtimes.** After swapping
   `.a`/header files, `touch` a source (or `pio run -t clean`) to force a
   relink, or you flash a stale binary.

## Restore stock behavior

```bash
# restore the backed-up libs + sdkconfig.h, remove -D ESP32_PM_LIGHT_SLEEP
# from variants/heltec_v4/platformio.ini, rebuild
```
