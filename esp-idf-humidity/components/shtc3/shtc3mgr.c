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
  bool enabled;
} state_t;

typedef struct {
  time_t timestamp;
  float temp;
  float humidity;
} sensor_data_t;

static state_t state;
static sensor_data_t sensor_reading;

static void shtc3mgr_cmd_get_optionshandler_dealloc_cb(
    CommandResponse *resp_out) {
  ESP_LOGD(TAG, "shtc3mgr_cmd_get_options_dealloc_cb - freeing");
  free(resp_out->shtc3_get_options_response);
}

static CommandResponse__RetCodeT shtc3mgr_cmd_get_optionshandler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_SHT4X_GET_OPTIONS_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  ESP_LOGD(TAG, "shtc3mgr_cmd_get_optionshandler()");
  resp_out->resp_case = COMMAND_RESPONSE__RESP_SHT4X_GET_OPTIONS_RESPONSE;
  *cb = shtc3mgr_cmd_get_optionshandler_dealloc_cb;
  Shtc3__GetOptionsResponse *cmd_resp =
      (Shtc3__GetOptionsResponse *)calloc(1, sizeof(Shtc3__GetOptionsResponse));
  shtc3__get_options_response__init(cmd_resp);
  resp_out->shtc3_get_options_response = cmd_resp;

  cmd_resp->enable = state.enabled;

  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

static void shtc3mgr_cmd_set_optionshandler_dealloc_cb(
    CommandResponse *resp_out) {
  ESP_LOGD(TAG, "shtc3mgr_cmd_set_options_dealloc_cb - freeing");
  free(resp_out->shtc3_set_options_response);
}

static CommandResponse__RetCodeT shtc3mgr_cmd_set_optionshandler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_SHT4X_SET_OPTIONS_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  Shtc3__SetOptionsRequest *cmd = msg->shtc3_set_options_request;

  ESP_LOGD(TAG, "shtc3mgr_cmd_set_optionshandler(enable:%s)",
           cmd->enable ? "true" : "false");
  resp_out->resp_case = COMMAND_RESPONSE__RESP_SHT4X_SET_OPTIONS_RESPONSE;
  *cb = shtc3mgr_cmd_set_optionshandler_dealloc_cb;
  Shtc3__SetOptionsResponse *cmd_resp =
      (Shtc3__SetOptionsResponse *)calloc(1, sizeof(Shtc3__SetOptionsResponse));
  shtc3__set_options_response__init(cmd_resp);
  resp_out->shtc3_set_options_response = cmd_resp;

  state.enabled = cmd->enable;

  cmd_resp->enable = state.enabled;

  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

static esp_err_t shtc3mgr_measure(void **sensor_data_out, size_t *len) {
  esp_err_t res;
  sensor_data_t **sdo = (sensor_data_t **)sensor_data_out;
  memset(&sensor_reading, 0, sizeof(sensor_data_t));  // Blank the reading out

  if (!state.enabled) {
    // Skip doing the sensor read for this
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGD(TAG, "measure...");
  time(&sensor_reading.timestamp);
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
  ESP_LOGI(TAG, "Init hardware");

  state.enabled = true;

  ESP_ERROR_CHECK(shtc3_init_desc(&state.dev, CONFIG_SHTC3_I2C_PORTNUM,
                                  (gpio_num_t)CONFIG_SHTC3_GPIO_SDA,
                                  (gpio_num_t)CONFIG_SHTC3_GPIO_SCL));
  ESP_ERROR_CHECK(shtc3_init(&state.dev));

  ESP_LOGI(TAG, "Register Handlers");
  sensormgr_register_sensor((sensormgr_registration_t){
      .measure = shtc3mgr_measure,
      .marshall = shtc3mgr_serialize_data,
  });

  mqttmgr_register_cmd_handler(shtc3mgr_cmd_get_optionshandler);
  mqttmgr_register_cmd_handler(shtc3mgr_cmd_set_optionshandler);

  return ESP_OK;
}

#endif
