/* Wi-Fi Provisioning Manager Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
 */

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

extern "C" {
void app_main();
}

static const char *TAG = "app";

static i2c_dev_t humidity_sensor;

void app_main() {
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

  i2cdev_init();
  shtc3_init_desc(&humidity_sensor, I2C_NUM_1, GPIO_NUM_23, GPIO_NUM_22);
  shtc3_init(&humidity_sensor);

  // wifi_provision();

  //[> Network time setup <]
  // sntp_setoperatingmode(SNTP_OPMODE_POLL);
  // sntp_setservername(0, "192.168.2.1");  // TODO: DNS this...
  // sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
  // sntp_init();

  //[> Setup MQTT <]
  // esp_mqtt_client_config_t mqtt_cfg = {
  //.uri = "mqtt://192.168.2.31"};  // TODO: DNS this...
  // esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
  // esp_mqtt_client_register_event(client,
  // (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, client);
  // esp_mqtt_client_start(client);

  //[> Start main application now <]
  // time_t now;
  // char strftime_buf[64];

  // time(&now);  // TODO: This takes a bit to sync, wait for actual sync before
  //// continuing

  float temp, humidity = 0.0;
  // char *json;
  // cJSON *root, *sensors, *sensorData;

  while (1) {
    // time(&now);
    // strftime(strftime_buf, sizeof(strftime_buf), "%FT%H:%M:%SZ",
    // gmtime(&now));
    ESP_ERROR_CHECK(shtc3_measure(&humidity_sensor, &temp, &humidity));
    ESP_LOGI(TAG, "%f %f", temp, humidity);

    // root = cJSON_CreateObject();
    // cJSON_AddStringToObject(root, "name", "bub");
    // cJSON_AddStringToObject(root, "timestamp", strftime_buf);
    // cJSON_AddItemToObject(root, "sensorData", sensors = cJSON_CreateArray());
    // sensorData = cJSON_CreateObject();
    // cJSON_AddItemToArray(sensors, sensorData);
    // cJSON_AddStringToObject(sensorData, "name", "temp");
    // cJSON_AddItemToObject(sensorData, "value", cJSON_CreateNumber(temp));
    // cJSON_AddStringToObject(sensorData, "units", "C");

    // sensorData = cJSON_CreateObject();
    // cJSON_AddItemToArray(sensors, sensorData);
    // cJSON_AddStringToObject(sensorData, "name", "humidity");
    // cJSON_AddItemToObject(sensorData, "value", cJSON_CreateNumber(humidity));
    // cJSON_AddStringToObject(sensorData, "units", "% Rh");

    // json = cJSON_PrintUnformatted(root);

    // ESP_LOGI(TAG, "JSON Output: %s", json);
    // esp_mqtt_client_publish(client, "/room/measurements", json, 0, 1, 0);
    // cJSON_Delete(root);

    vTaskDelay(2500 / portTICK_PERIOD_MS);
  }
}
