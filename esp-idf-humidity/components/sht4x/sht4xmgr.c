#include <sdkconfig.h>
#if CONFIG_SHT4X_ENABLED

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <string.h>
#include <time.h>

#include "sensormgr.h"
#include "sht4x.h"
#include "sht4xmgr.h"

static const char *TAG = "sht4xmgr";

typedef struct {
  i2c_dev_t dev;
  Sht4x__ModeT mode;
} state_t;

typedef struct {
  time_t timestamp;
  float temp;
  float humidity;
} sensor_data_t;

static state_t state;
static sensor_data_t sensor_reading;

static esp_err_t sht4xmgr_measure(void **sensor_data_out, size_t *len) {
  esp_err_t res;
  sensor_data_t **sdo = (sensor_data_t **)sensor_data_out;
  memset(&sensor_reading, 0, sizeof(sensor_data_t));  // Blank the reading out

  ESP_LOGD(TAG, "measure...");
  time(&sensor_reading.timestamp);
  res = sht4x_measure(&state.dev, state.mode, &sensor_reading.temp,
                      &sensor_reading.humidity);

  switch (state.mode) {
    case SHT4X__MODE_T__HIGH_HEATER_1S:
    case SHT4X__MODE_T__HIGH_HEATER_100MS:
    case SHT4X__MODE_T__MED_HEATER_1S:
    case SHT4X__MODE_T__MED_HEATER_100MS:
    case SHT4X__MODE_T__LOW_HEATER_1S:
    case SHT4X__MODE_T__LOW_HEATER_100MS:
      ESP_LOGD(TAG,
               "previous mode had heating, setting to high repeatability now");
      state.mode = SHT4X__MODE_T__NO_HEATER_HIGH;
      break;
    default:
      break;
  }

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

static esp_err_t sht4xmgr_serialize_data(void *sensor_data, cJSON *data_array) {
  char iso8601[32];

  sensor_data_t *data = (sensor_data_t *)sensor_data;
  SENSORMGR_ISO8601(data->timestamp, iso8601);

  cJSON *sensor_json = cJSON_CreateObject();
  cJSON_AddStringToObject(sensor_json, "timestamp", iso8601);
  cJSON_AddNumberToObject(sensor_json, "value", data->temp);
  cJSON_AddStringToObject(sensor_json, "unit", "C");
  cJSON_AddStringToObject(sensor_json, "sensor", "sht4x");
  cJSON_AddItemToArray(data_array, sensor_json);

  sensor_json = cJSON_CreateObject();
  cJSON_AddStringToObject(sensor_json, "timestamp", iso8601);
  cJSON_AddNumberToObject(sensor_json, "value", data->humidity);
  cJSON_AddStringToObject(sensor_json, "unit", "%rH");
  cJSON_AddStringToObject(sensor_json, "sensor", "sht4x");
  cJSON_AddItemToArray(data_array, sensor_json);

  return ESP_OK;
}

esp_err_t sht4xmgr_init() {
  ESP_LOGI(TAG, "Init hardware");

  state.mode = SHT4X__MODE_T__NO_HEATER_HIGH;
  ESP_ERROR_CHECK(sht4x_init_desc(&state.dev, CONFIG_SHT4X_I2C_PORTNUM,
                                  (gpio_num_t)CONFIG_SHT4X_GPIO_SDA,
                                  (gpio_num_t)CONFIG_SHT4X_GPIO_SCL));
  ESP_ERROR_CHECK(sht4x_init(&state.dev));

  ESP_LOGI(TAG, "Register Handlers");
  sensormgr_register_sensor((sensormgr_registration_t){
      .measure = sht4xmgr_measure,
      .marshall = sht4xmgr_serialize_data,
  });

  return ESP_OK;
}

#endif
