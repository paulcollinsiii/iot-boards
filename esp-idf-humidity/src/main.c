#include <esp_event.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <i2cdev.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "alarm.h"
#include "ltr390mgr.h"
#include "mqttlog.h"
#include "mqttmgr.h"
#include "sensormgr.h"
#include "sht4xmgr.h"
#include "shtc3mgr.h"
#include "uuid.h"
#include "wifi_provision.h"

#define CFG_ID "pciot_cfg_id"

static const char *TAG = "app";

static time_t now;
static char PRIVATE_ID[37];

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

  nvs_handle_t my_handle;
  size_t private_id_size = sizeof(PRIVATE_ID);
  ESP_ERROR_CHECK(nvs_open("nvs", NVS_READWRITE, &my_handle));
  ret = nvs_get_str(my_handle, CFG_ID, PRIVATE_ID, &private_id_size);
  switch (ret) {
    case ESP_OK:
      ESP_LOGI(TAG, "Private ID read from NVS: %s", PRIVATE_ID);
      break;
    case ESP_ERR_NVS_NOT_FOUND:
      ESP_LOGI(TAG, "Private ID not set, creating...");
      UUIDGen(PRIVATE_ID);
      ESP_LOGI(TAG, "UUID Generated: %s", PRIVATE_ID);
      ESP_ERROR_CHECK(nvs_set_str(my_handle, CFG_ID, PRIVATE_ID));
      break;
    default:
      ESP_LOGE(TAG, "Errors (%s) opening NVS handle", esp_err_to_name(ret));
      break;
  }

  nvs_close(my_handle);

  i2cdev_init();

  /* Set timezone from NVS */
  setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
  tzset();

  /* Setup power mgmt */
#if CONFIG_IDF_TARGET_ESP32
  esp_pm_config_esp32_t pm_config = {
      .light_sleep_enable = true, .max_freq_mhz = 240, .min_freq_mhz = 80};
#elif CONFIG_IDF_TARGET_ESP32S2
  esp_pm_config_esp32s2_t pm_config = {
      .light_sleep_enable = true, .max_freq_mhz = 240, .min_freq_mhz = 80};
#endif
  esp_pm_configure(&pm_config);
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

  // Initialize component libraries (non-hardware)
  ESP_ERROR_CHECK(mqttmgr_init(PRIVATE_ID));
  ESP_ERROR_CHECK(mqttlog_init());
}

void sensor_init() {
  ESP_ERROR_CHECK(sensormgr_init());
  ESP_ERROR_CHECK(alarm_init());
#if CONFIG_LTR390_ENABLED
  ESP_ERROR_CHECK(ltr390mgr_init());
#endif
#if CONFIG_SHTC3_ENABLED
  ESP_ERROR_CHECK(shtc3mgr_init());
#endif
#if CONFIG_SHT4X_ENABLED
  ESP_ERROR_CHECK(sht4xmgr_init());
#endif
}

void app_main() {
  hardware_init();
  client_init();
  sensor_init();

  mqttmgr_start();
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  sensormgr_start();
  MQTTLOG_LOGI(TAG, "test message", "");
}
