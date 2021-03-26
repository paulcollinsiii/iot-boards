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
#include <shtc3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mqtt.h"
#include "wifi_provision.h"

static const char *TAG = "app";

static i2c_dev_t humidity_sensor;
static time_t now;
static char strftime_buf[64];

void hardware_setup() {
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
  shtc3_init_desc(&humidity_sensor, I2C_NUM_1, GPIO_NUM_23, GPIO_NUM_22);
  shtc3_init(&humidity_sensor);

  /* Set timezone from NVS */
  setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
  tzset();
}

void client_setup() {
  struct tm timeinfo;
  const int retry_count = 10;
  int retry = 0;

  wifi_provision();
  ESP_ERROR_CHECK(mqtt_setup());

  //[> Network time setup <]
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "192.168.2.1");  // TODO: DNS this...
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
  hardware_setup();
  client_setup();

  float temp, humidity = 0.0;
  char *json;
  cJSON *root, *sensors, *sensorData;

  while (1) {
    time(&now);
    strftime(strftime_buf, sizeof(strftime_buf), "%FT%H:%M:%SZ", gmtime(&now));
    ESP_ERROR_CHECK(shtc3_measure(&humidity_sensor, &temp, &humidity));

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "bub");
    cJSON_AddStringToObject(root, "timestamp", strftime_buf);
    cJSON_AddItemToObject(root, "sensorData", sensors = cJSON_CreateArray());
    sensorData = cJSON_CreateObject();
    cJSON_AddItemToArray(sensors, sensorData);
    cJSON_AddStringToObject(sensorData, "name", "temp");
    cJSON_AddItemToObject(sensorData, "value", cJSON_CreateNumber(temp));
    cJSON_AddStringToObject(sensorData, "units", "C");

    sensorData = cJSON_CreateObject();
    cJSON_AddItemToArray(sensors, sensorData);
    cJSON_AddStringToObject(sensorData, "name", "humidity");
    cJSON_AddItemToObject(sensorData, "value", cJSON_CreateNumber(humidity));
    cJSON_AddStringToObject(sensorData, "units", "% Rh");

    json = cJSON_PrintUnformatted(root);

    ESP_LOGI(TAG, "JSON Output: %s", json);
    mqtt_publish_sensor_data(json);
    cJSON_Delete(root);

    vTaskDelay(2500 / portTICK_PERIOD_MS);
  }
}
