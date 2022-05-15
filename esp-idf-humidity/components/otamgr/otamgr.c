#include <sdkconfig.h>

#if CONFIG_OTAMGR_ENABLED
// TODO: The URL schema for firmware names needs to be sane
// selecting what sensors are on here and embedding that in the file name
// is needed, then version numbers are a thing too I suppose

#define OTAMGR_UPDATE_URL CONFIG_OTAMGR_FIRMWARE_URL_BASE "s2_firmware.bin"

#include <commands.pb-c.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqttlog.h>
#include <mqttmgr.h>
#include <otamgr.h>
#include <string.h>

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_crt_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_crt_end");

static const char *TAG = "otamgr";
static TaskHandle_t update_task_handle = NULL;

static esp_err_t validate_image_header(esp_app_desc_t *new_app_info) {
  if (new_app_info == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_app_desc_t running_app_info;
  if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
    ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
  }

  if (memcmp(new_app_info->version, running_app_info.version,
             sizeof(new_app_info->version)) == 0) {
    ESP_LOGW(TAG,
             "Current running version is the same as a new. We will not "
             "continue the update.");
    return ESP_FAIL;
  }

  return ESP_OK;
}

static void otamgr_update_task(void *pvParam) {
  esp_err_t err = ESP_OK;
  esp_http_client_config_t config = {
      .url = OTAMGR_UPDATE_URL,
      .cert_pem = (char *)server_cert_pem_start,
      .timeout_ms = 3000,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &config,
  };

  esp_https_ota_handle_t ota_handle = NULL;

  err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (err != ESP_OK) {
    MQTTLOG_LOGE(TAG, "OTA Update failed", "stage=begin");
    goto ota_end;
  }
  MQTTLOG_LOGI(TAG, "OTA Update checkpoint complete", "stage=begin");

  esp_app_desc_t app_desc;
  err = esp_https_ota_get_img_desc(ota_handle, &app_desc);
  if (err != ESP_OK) {
    MQTTLOG_LOGE(TAG, "OTA Update failed", "stage=esp_https_ota_read_img_desc");
    goto ota_end;
  }
  MQTTLOG_LOGI(TAG, "OTA Update checkpoint complete",
               "stage=esp_https_ota_read_img_desc");

  err = validate_image_header(&app_desc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "image header verification failed");
    MQTTLOG_LOGE(TAG, "OTA Update failed", "stage=validate_image_header");
    goto ota_end;
  }
  MQTTLOG_LOGI(TAG, "OTA Update checkpoint complete",
               "stage=validate_image_header");

  while (1) {
    err = esp_https_ota_perform(ota_handle);
    if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      break;
    }
    ESP_LOGI(TAG, "Image bytes read: %d",
             esp_https_ota_get_image_len_read(ota_handle));
  }

  MQTTLOG_LOGI(TAG, "OTA Update checkpoint complete", "stage=image_download");
  if (esp_https_ota_is_complete_data_received(ota_handle) != true) {
    // the OTA image was not completely received and user can customise the
    // response to this situation.
    ESP_LOGE(TAG, "Complete data was not received.");
    MQTTLOG_LOGE(TAG, "OTA Update failed",
                 "stage=esp_https_ota_is_complete_data_received");
    err = ESP_FAIL;
  } else {
    err = esp_https_ota_finish(ota_handle);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "ESP_HTTPS_OTA upgrade successful. Rebooting ...");
      // TODO: This needs to signal the sensormgr to safely shutdown
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      esp_restart();
    } else {
      if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        MQTTLOG_LOGE(TAG, "OTA Update failed", "stage=image_corrupted");
      }
      MQTTLOG_LOGE(TAG, "OTA Update failed",
                   "stage=image_validation_failed err=0x%u", err);
    }
  }

ota_end:
  esp_https_ota_abort(ota_handle);
  MQTTLOG_LOGE(TAG, "OTA Update failed", "stage=ota_end");
  update_task_handle = NULL;
  vTaskDelete(NULL);
}

static void otamgr_cmd_update_request_dealloc_cb(CommandResponse *resp_out) {
  ESP_LOGD(TAG, "otamgr_cmd_update_request_dealloc_cb - freeing");
  free(resp_out->otamgr_update_response);
}

static CommandResponse__RetCodeT otamgr_cmd_update_request(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_OTAMGR_UPDATE_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  resp_out->resp_case = COMMAND_RESPONSE__RESP_OTAMGR_UPDATE_RESPONSE;
  *cb = otamgr_cmd_update_request_dealloc_cb;
  Otamgr__UpdateResponse *cmd_resp =
      (Otamgr__UpdateResponse *)calloc(1, sizeof(Otamgr__UpdateResponse));
  otamgr__update_response__init(cmd_resp);
  resp_out->otamgr_update_response = cmd_resp;

  if (update_task_handle != NULL) {
    MQTTLOG_LOGW(TAG, "OTA Update request failed", "reason=already_running");
    return COMMAND_RESPONSE__RET_CODE_T__ERR;
  }
  if (pdPASS != xTaskCreate(otamgr_update_task, "otamgr", 4096, (void *)1, 1,
                            &update_task_handle)) {
    MQTTLOG_LOGW(TAG, "OTA Update request failed",
                 "reason=task_creation_failed");
    return COMMAND_RESPONSE__RET_CODE_T__ERR;
  }
  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

esp_err_t otamgr_init() {
  mqttmgr_register_cmd_handler(otamgr_cmd_update_request);
  return ESP_OK;
}

#endif
