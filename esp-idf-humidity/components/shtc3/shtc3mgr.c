#include <sdkconfig.h>
#if CONFIG_SHTC3_ENABLED

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <string.h>
#include <time.h>

#include "sensormgr.h"
#include "shtc3.h"
#include "shtc3mgr.h"

static const char *TAG = "shtc3mgr";

typedef struct {
  i2c_dev_t dev;
} state_t;

typedef struct {
  time_t timestamp;
  float temp;
  float humidity;
} sensor_data_t;

static state_t state;
static sensor_data_t sensor_reading;

static esp_err_t shtc3mgr_measure(void **sensor_data_out, size_t *len) {
  esp_err_t res;
  sensor_data_t **sdo = (sensor_data_t **)sensor_data_out;
  memset(&sensor_reading, 0, sizeof(sensor_data_t));  // Blank the reading out

  ESP_LOGD(TAG, "measure...");
  time(&sensor_reading.timestamp);
  // TODO: Read data from sensor
  res =
      shtc3_measure(&state.dev, &sensor_reading.temp, &sensor_reading.humidity);

  if (res != ESP_OK) {
    ESP_LOGW(TAG, "measure - failed: %s", esp_err_to_name(res));
    *sdo = NULL;
    *len = 0;
    return res;  // Don't attempt to add a bad reading to ring buffer
  }

  ESP_LOGV(TAG, "measure - done");

  *sdo = &sensor_reading;
  *len = sizeof(sensor_reading);
  return ESP_OK;
}

static esp_err_t shtc3mgr_serialize_data(void *sensor_data, cJSON *data_array) {
  char iso8601[32];

  sensor_data_t *data = (sensor_data_t *)sensor_data;
  SENSORMGR_ISO8601(data->timestamp, iso8601);

  cJSON *sensor_json = cJSON_CreateObject();
  cJSON_AddStringToObject(sensor_json, "timestamp", iso8601);
  cJSON_AddNumberToObject(sensor_json, "value", data->temp);
  cJSON_AddStringToObject(sensor_json, "unit", "C");
  cJSON_AddStringToObject(sensor_json, "sensor", "shtc3");
  cJSON_AddItemToArray(data_array, sensor_json);

  sensor_json = cJSON_CreateObject();
  cJSON_AddStringToObject(sensor_json, "timestamp", iso8601);
  cJSON_AddNumberToObject(sensor_json, "value", data->humidity);
  cJSON_AddStringToObject(sensor_json, "unit", "%rH");
  cJSON_AddStringToObject(sensor_json, "sensor", "shtc3");
  cJSON_AddItemToArray(data_array, sensor_json);

  return ESP_OK;
}

esp_err_t shtc3mgr_init() {
  // TODO: These need to be part of KCONFIG for this module
  ESP_LOGI(TAG, "Init hardware");

  ESP_ERROR_CHECK(shtc3_init_desc(&state.dev, CONFIG_SHTC3_I2C_PORTNUM,
                                  (gpio_num_t)CONFIG_SHTC3_GPIO_SDA,
                                  (gpio_num_t)CONFIG_SHTC3_GPIO_SCL));
  ESP_ERROR_CHECK(shtc3_init(&state.dev));

  ESP_LOGI(TAG, "Register Handlers");
  sensormgr_register_sensor((sensormgr_registration_t){
      .measure = shtc3mgr_measure,
      .marshall = shtc3mgr_serialize_data,
  });

  return ESP_OK;
}

#endif
