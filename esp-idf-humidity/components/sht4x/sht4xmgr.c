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
  bool enabled;
  Sht4x__ModeT mode;
} state_t;

typedef struct {
  time_t timestamp;
  float temp;
  float humidity;
} sensor_data_t;

static state_t state;
static sensor_data_t sensor_reading;

static void sht4xmgr_cmd_get_optionshandler_dealloc_cb(
    CommandResponse *resp_out) {
  ESP_LOGD(TAG, "sht4xmgr_cmd_get_options_dealloc_cb - freeing");
  free(resp_out->sht4x_get_options_response);
}

static CommandResponse__RetCodeT sht4xmgr_cmd_get_optionshandler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_SHT4X_GET_OPTIONS_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  ESP_LOGD(TAG, "sht4xmgr_cmd_get_optionshandler()");
  resp_out->resp_case = COMMAND_RESPONSE__RESP_SHT4X_GET_OPTIONS_RESPONSE;
  *cb = sht4xmgr_cmd_get_optionshandler_dealloc_cb;
  Sht4x__GetOptionsResponse *cmd_resp =
      (Sht4x__GetOptionsResponse *)calloc(1, sizeof(Sht4x__GetOptionsResponse));
  sht4x__get_options_response__init(cmd_resp);
  resp_out->sht4x_get_options_response = cmd_resp;

  cmd_resp->enable = state.enabled;
  cmd_resp->mode = state.mode;

  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

static void sht4xmgr_cmd_set_optionshandler_dealloc_cb(
    CommandResponse *resp_out) {
  ESP_LOGD(TAG, "sht4xmgr_cmd_set_options_dealloc_cb - freeing");
  free(resp_out->sht4x_set_options_response);
}

static CommandResponse__RetCodeT sht4xmgr_cmd_set_optionshandler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_SHT4X_SET_OPTIONS_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  Sht4x__SetOptionsRequest *cmd = msg->sht4x_set_options_request;

  ESP_LOGD(TAG, "sht4xmgr_cmd_set_optionshandler(enable:%s, mode:%s, ",
           cmd->enable ? "true" : "false", sht4x_mode_to_str(cmd->mode));
  resp_out->resp_case = COMMAND_RESPONSE__RESP_SHT4X_SET_OPTIONS_RESPONSE;
  *cb = sht4xmgr_cmd_set_optionshandler_dealloc_cb;
  Sht4x__SetOptionsResponse *cmd_resp =
      (Sht4x__SetOptionsResponse *)calloc(1, sizeof(Sht4x__SetOptionsResponse));
  sht4x__set_options_response__init(cmd_resp);
  resp_out->sht4x_set_options_response = cmd_resp;

  state.enabled = cmd->enable;
  state.mode = cmd->mode;

  cmd_resp->enable = state.enabled;
  cmd_resp->mode = state.mode;

  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

static esp_err_t sht4xmgr_measure(void **sensor_data_out, size_t *len) {
  esp_err_t res;
  sensor_data_t **sdo = (sensor_data_t **)sensor_data_out;
  memset(&sensor_reading, 0, sizeof(sensor_data_t));  // Blank the reading out

  if (!state.enabled) {
    // Skip doing the sensor read for this
    return ESP_ERR_INVALID_STATE;
  }

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
  state.enabled = true;
  ESP_ERROR_CHECK(sht4x_init_desc(&state.dev, CONFIG_SHT4X_I2C_PORTNUM,
                                  (gpio_num_t)CONFIG_SHT4X_GPIO_SDA,
                                  (gpio_num_t)CONFIG_SHT4X_GPIO_SCL));
  ESP_ERROR_CHECK(sht4x_init(&state.dev));

  ESP_LOGI(TAG, "Register Handlers");
  sensormgr_register_sensor((sensormgr_registration_t){
      .measure = sht4xmgr_measure,
      .marshall = sht4xmgr_serialize_data,
  });

  mqttmgr_register_cmd_handler(sht4xmgr_cmd_get_optionshandler);
  mqttmgr_register_cmd_handler(sht4xmgr_cmd_set_optionshandler);

  return ESP_OK;
}

#endif
