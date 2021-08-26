#include <cJSON.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <i2cdev.h>
#include <mqtt_client.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mqtt.h"
#include "sensor.h"
#include "wifi_provision.h"

static const char *TAG = "app";

static time_t now;

void hardware_init() {
  /* Initialize NVS partition */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    /* NVS partition was truncated
     * and needs to be erased */
    ESP_ERROR_CHECK(nvs_flash_erase());

    /* Retry nvs_flash_init */
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  /* Initialize I2C and sensors */
  i2cdev_init();
  sensor_init();

  /* Set timezone from NVS */
  setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
  tzset();
}

void client_init() {
  struct tm timeinfo;
  const int retry_count = 10;
  int retry = 0;

  wifi_provision();

  //[> Network time setup <]
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0,
                     "opensense.kaffi.home");  // TODO: Make this configurable
  sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
  sntp_init();
  time(&now);
  localtime_r(&now, &timeinfo);
  // Is time set? If not, tm_year will be (1970 - 1900).
  if (timeinfo.tm_year < (2016 - 1900)) {
    ESP_LOGI(
        TAG,
        "Time is not set yet. Connecting to WiFi and getting time over NTP.");
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET &&
           ++retry < retry_count) {
      ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry,
               retry_count);
      vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    // update 'now' variable with current time
    time(&now);
  }
}

void app_main() {
  ESP_LOGI(TAG, "setup mqtt lib...");
  ESP_ERROR_CHECK(mqtt_init());

  ESP_LOGI(TAG, "mqtt lib initted.");

  hardware_init();
  client_init();

  mqtt_start();
  sensor_start();
}
