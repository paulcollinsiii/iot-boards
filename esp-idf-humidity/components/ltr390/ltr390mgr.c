#include <sdkconfig.h>
#if CONFIG_LTR390_ENABLED

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <string.h>
#include <time.h>

#include "ltr390.h"
#include "ltr390mgr.h"
#include "sensormgr.h"

static const char *TAG = "ltr390mgmt";

typedef struct {
  i2c_dev_t dev;
} state_t;

typedef struct {
  time_t timestamp;
  float measurement;
  Ltr390__ModeT mode;
} sensor_data_t;

static state_t state;
static sensor_data_t sensor_reading;

static esp_err_t ltr390mgr_measure(void **sensor_data_out, size_t *len) {
  esp_err_t res;
  sensor_data_t **sdo = (sensor_data_t **)sensor_data_out;
  memset(&sensor_reading, 0, sizeof(sensor_data_t));  // Blank the reading out

  ESP_LOGD(TAG, "measure...");
  time(&sensor_reading.timestamp);
  // TODO: Retry on failure?
  res = ltr390_measure(&state.dev, &sensor_reading.measurement,
                       &sensor_reading.mode);
  // Toggle the sensor mode between readings
  ltr390_set_mode(&state.dev, !sensor_reading.mode);
  if (res != ESP_OK) {
    if (res != ESP_ERR_INVALID_STATE) {  // AKA Sensor not in standby
      ESP_LOGW(TAG, "measure - failed: %s", esp_err_to_name(res));
    }
    *sdo = NULL;
    *len = 0;
    return res;  // Don't attempt to add a bad reading to ring buffer
  }

  ESP_LOGV(TAG, "measure - done");

  *sdo = &sensor_reading;
  *len = sizeof(sensor_reading);
  return ESP_OK;
}

static esp_err_t ltr390mgr_serialize_data(void *sensor_data,
                                          cJSON *data_array) {
  char iso8601[32];

  sensor_data_t *data = (sensor_data_t *)sensor_data;
  SENSORMGR_ISO8601(data->timestamp, iso8601);
  cJSON *sensor_json = cJSON_CreateObject();
  cJSON_AddStringToObject(sensor_json, "timestamp", iso8601);
  cJSON_AddNumberToObject(sensor_json, "value", data->measurement);
  cJSON_AddStringToObject(sensor_json, "unit",
                          data->mode == LTR390__MODE_T__ALS ? "lux" : "uvi");
  cJSON_AddStringToObject(sensor_json, "sensor", "ltr390");
  cJSON_AddItemToArray(data_array, sensor_json);
  return ESP_OK;
}

static void ltr390mgr_cmd_set_optionshandler_dealloc_cb(
    CommandResponse *resp_out) {
  ESP_LOGD(TAG, "ltr390mgmt_cmd_set_options_dealloc_cb - freeing");
  free(resp_out->ltr390_set_options_response);
}

static CommandResponse__RetCodeT ltr390mgr_cmd_set_optionshandler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_LTR390_SET_OPTIONS_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  Ltr390__SetOptionsRequest *cmd = msg->ltr390_set_options_request;

  ESP_LOGD(TAG,
           "ltr390mgr_cmd_set_optionshandler(enable:%s, mode:%s, "
           "resolution:%s, measurerate:%s, gain:%s)",
           cmd->enable ? "true" : "false", ltr390_mode_to_str(cmd->mode),
           ltr390_resolution_to_str(cmd->resolution),
           ltr390_measurerate_to_str(cmd->measurerate),
           ltr390_gain_to_str(cmd->gain));
  resp_out->resp_case = COMMAND_RESPONSE__RESP_LTR390_SET_OPTIONS_RESPONSE;
  *cb = ltr390mgr_cmd_set_optionshandler_dealloc_cb;
  Ltr390__SetOptionsResponse *cmd_resp = (Ltr390__SetOptionsResponse *)calloc(
      1, sizeof(Ltr390__SetOptionsResponse));
  ltr390__set_options_response__init(cmd_resp);
  resp_out->ltr390_set_options_response = cmd_resp;

  ltr390_set_mode(&state.dev, cmd->mode);
  ltr390_set_gain(&state.dev, cmd->gain);
  ltr390_set_resolution(&state.dev, cmd->measurerate, cmd->resolution);
  if (cmd->enable) {
    ltr390_enable(&state.dev);
  } else {
    ltr390_standby(&state.dev);
  }

  // Verify state directly from device
  cmd_resp->enable = cmd->enable;
  ltr390_get_gain(&state.dev, &cmd_resp->gain);
  ltr390_get_mode(&state.dev, &cmd_resp->mode);
  ltr390_get_resolution(&state.dev, &cmd->measurerate, &cmd->resolution);

  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

static void ltr390mgr_cmd_get_optionshandler_dealloc_cb(
    CommandResponse *resp_out) {
  ESP_LOGD(TAG, "ltr390mgmt_cmd_get_options_dealloc_cb - freeing");
  free(resp_out->ltr390_get_options_response);
}

static CommandResponse__RetCodeT ltr390mgr_cmd_get_optionshandler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_LTR390_GET_OPTIONS_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  resp_out->resp_case = COMMAND_RESPONSE__RESP_LTR390_GET_OPTIONS_RESPONSE;
  *cb = ltr390mgr_cmd_get_optionshandler_dealloc_cb;
  Ltr390__GetOptionsResponse *cmd_resp = (Ltr390__GetOptionsResponse *)calloc(
      1, sizeof(Ltr390__GetOptionsResponse));
  ltr390__get_options_response__init(cmd_resp);
  resp_out->ltr390_get_options_response = cmd_resp;
  ltr390_get_cached_state((bool *)&cmd_resp->enable, &cmd_resp->gain,
                          &cmd_resp->measurerate, &cmd_resp->mode,
                          &cmd_resp->resolution);
  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

esp_err_t ltr390mgr_init() {
  // TODO: These need to be part of KCONFIG for this module
  ESP_LOGI(TAG, "Init hardware");
  ltr390_init_desc(&state.dev, CONFIG_LTR390_I2C_PORTNUM,
                   (gpio_num_t)CONFIG_LTR390_GPIO_SDA,
                   (gpio_num_t)CONFIG_LTR390_GPIO_SCL);
  ltr390_init(&state.dev);

  // TODO: Default sensor state config needs to be stored somewhere...
  // Enable sensors (if needed)
  ltr390_enable(&state.dev);

  ESP_LOGI(TAG, "Register Handlers");
  sensormgr_register_sensor((sensormgr_registration_t){
      .measure = ltr390mgr_measure,
      .marshall = ltr390mgr_serialize_data,
  });

  mqttmgr_register_cmd_handler(ltr390mgr_cmd_set_optionshandler);
  mqttmgr_register_cmd_handler(ltr390mgr_cmd_get_optionshandler);

  return ESP_OK;
}

#endif
