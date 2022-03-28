// Originally based on
// https://github.com/espressif/esp-idf/blob/2f9d47c708f39772b0e8f92d147b9e85aa3a0b19/examples/peripherals/touch_sensor/touch_sensor_v1/touch_pad_interrupt/main/tp_interrupt_main.c

#include <sdkconfig.h>
#if CONFIG_TOUCHBTN_ENABLED

#include <blinky.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <mqttmgr.h>
#include <sensormgr.h>
#include <stdatomic.h>
#include <stdio.h>

static const char *TAG = "touchbtn";

void touchbtn_deepsleep() {
  blinky_animation_t animation = {
      .brgb = 0xe20f0f00,
      .target = BLINKY__BLINKY_LED_T__RGB_0,
      .ms_delay = 300,
      .off_at_end = false,
      .repeat_count = UINT32_MAX,
      .pattern = BLINKY__BLINKY_PATTERN_T__BLINK,
  };
  blinky_play(&animation);
  ESP_LOGW(TAG, "Stopping the sensor polling");
  mqttmgr_stop();
  esp_wifi_stop();
  sensormgr_stop();
  ESP_LOGW(TAG, "GO TO SLEEP NAO!");
  animation = (blinky_animation_t){
      .brgb = 0xe200ff00,
      .target = BLINKY__BLINKY_LED_T__RGB_0,
      .ms_delay = 300,
      .off_at_end = false,
      .repeat_count = UINT32_MAX,
      .pattern = BLINKY__BLINKY_PATTERN_T__BLINK,
  };
  blinky_play(&animation);
  esp_deep_sleep_start();
}

void touchbtn_wifiwake() {
  blinky_animation_t animation;
  ESP_LOGW(TAG, "Wifi btn pressed");
  if (ESP_OK != mqttmgr_reconnect_now()) {
    ESP_LOGW(TAG, "Already connected to Wifi, ignoring button press");
    animation = (blinky_animation_t){
        .brgb = 0xe2000f0f,
        .target = BLINKY__BLINKY_LED_T__RGB_0,
        .ms_delay = 300,
        .off_at_end = true,
        .repeat_count = 3,
        .pattern = BLINKY__BLINKY_PATTERN_T__BLINK,
    };
    blinky_play(&animation);
    return;
  }
  animation = (blinky_animation_t){
      .brgb = 0xe2000f00,
      .target = BLINKY__BLINKY_LED_T__RGB_0,
      .ms_delay = 300,
      .off_at_end = true,
      .repeat_count = 3,
      .pattern = BLINKY__BLINKY_PATTERN_T__BLINK,
  };
  blinky_play(&animation);
}

#endif
