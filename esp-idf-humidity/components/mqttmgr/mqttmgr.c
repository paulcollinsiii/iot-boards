#include "mqttmgr.h"

#include <backoff_algorithm.h>
#include <esp_log.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>

#define MQTT_TASK_NAME "mqtt"
#define MQTT_TASK_STACKSIZE 2 * 1024
#define MQTT_HANDLERS_MAX 8

#define MQTT_CLIENTWATCHER_NAME "mqtt-watcher"
#define MQTT_CLIENTWATCHER_STACKSIZE 2 * 1024

#define MQTT_CLIENT_STARTED_BIT (1 << 0)
#define MQTT_CLIENT_CONNECTED_BIT (1 << 1)
#define MQTT_CLIENT_DISCONNECTED_BIT (1 << 2)

#define MQTT_BASE_BACKOFF_SEC 60
#define MQTT_MAX_BACKOFF 15 * 60

#define CMD_REQ_IDX 0
#define CMD_RESP_IDX 1
#define LOG_IDX 2
#define SENSOR_IDX 3

static const char *TAG = "mqtt";  // Logging handle name
char topic_names[4][64];

static BackoffAlgorithmContext_t retryParams;

typedef struct {
  EventGroupHandle_t events;
  TaskHandle_t mqtt_client_watcher_handle;
  TaskHandle_t mqtt_task_handle;

  time_t disabled_at;
  uint8_t retry_count;
  uint8_t json_handler_cnt;
  uint8_t cmd_handler_cnt;
  esp_mqtt_client_handle_t client;
  cmdhandler **cmd_handlers;
  jsonhandler *json_handlers[MQTT_HANDLERS_MAX];
} mqttmgr_state_t;

static mqttmgr_state_t state;

static void mqttmgr_cmd_dispatch(esp_mqtt_event_handle_t event) {
  // Parse input command
  // Call all handlers, exit on first handler that claims to have handled
  // Pack up and send response cmd to CMD_RESP_IDX topic
  uint8_t *buf;
  size_t len;
  int i;
  CommandRequest *req;
  dealloc_cb_fn *dealloc_cb = NULL;

  ESP_LOGD(TAG, "mqttmgr_cmd_dispatch - parsing protobuf");
  req = command_request__unpack(NULL, event->data_len,
                                (const unsigned char *)(event->data));
  if (!req) {
    ESP_LOGE(TAG, "mqttmgr_cmd_dispatch - unable to parse protobuf");
    return;
  }

  CommandResponse resp = COMMAND_RESPONSE__INIT;
  resp.uuid = calloc(strlen(req->uuid) + 1, sizeof(char));
  strcpy(resp.uuid, req->uuid);

  ESP_LOGD(TAG, "mqttmgr_cmd_dispatch - start cmd dispatch");
  for (i = 0; i < state.cmd_handler_cnt; i++) {
    resp.ret_code = state.cmd_handlers[i](req, &resp, &dealloc_cb);
    switch (resp.ret_code) {
      case COMMAND_RESPONSE__RET_CODE_T__NOTMINE:
        continue;
      case COMMAND_RESPONSE__RET_CODE_T__HANDLED:
        goto loop_exit;
      case COMMAND_RESPONSE__RET_CODE_T__ERR:
        ESP_LOGE(TAG, "mqttmgr_cmd_dispatch - cmd dispatch err");
        goto loop_exit;
      default:
        ESP_LOGE(TAG, "mqttmgr_cmd_dispatch - UNDEFINED HANDLER ERR(%d)",
                 resp.ret_code);
        goto loop_exit;
    }
  }
loop_exit:
  command_request__free_unpacked(req, NULL);
  if (resp.ret_code == COMMAND_RESPONSE__RET_CODE_T__NOTMINE) {
    ESP_LOGE(TAG, "mqttmgr_cmd_dispatch - UNDEFINED HANDLER cmd dispatch err");
  }

  ESP_LOGI(TAG, "mqttmgr_cmd_dispatch - packing response");
  len = command_response__get_packed_size(&resp);
  ESP_LOGI(TAG, "mqttmgr_cmd_dispatch - packedlen response");
  buf = (uint8_t *)calloc(len, sizeof(char));
  command_response__pack(&resp, buf);

  ESP_LOGI(TAG, "mqttmgr_cmd_dispatch - publishing response");
  if (esp_mqtt_client_publish(state.client, topic_names[CMD_RESP_IDX],
                              (char *)buf, len * sizeof(char),
                              1,  // QoS 1
                              0   // Do not retain cmd responses
                              ) == -1) {
    ESP_LOGE(TAG, "mqttmgr_cmd_dispatch - publishing failed!");
  }

  if (dealloc_cb != NULL) {
    ESP_LOGI(TAG, "mqttmgr_cmd_dispatch - dealloc_cb start");
    dealloc_cb(&resp);
    ESP_LOGI(TAG, "mqttmgr_cmd_dispatch - dealloc_cb finish");
  }
  free(resp.uuid);
  free(buf);
}
/**
 * @brief Stop all MQTT related task handlers
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
static void mqttmgr_event_handler(void *handler_args, esp_event_base_t base,
                                  int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base,
           event_id);

  switch (event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
      break;
    case MQTT_EVENT_CONNECTED:
      // Reset backoff delay since we connected
      // Subscribe to the command channel
      ESP_LOGD(TAG, "MQTT_EVENT_CONNECTED");
      xEventGroupSetBits(state.events, MQTT_CLIENT_CONNECTED_BIT);
      BackoffAlgorithm_InitializeParams(&retryParams, MQTT_BASE_BACKOFF_SEC,
                                        MQTT_MAX_BACKOFF,
                                        BACKOFF_ALGORITHM_RETRY_FOREVER);
      if (esp_mqtt_client_subscribe(state.client, topic_names[CMD_REQ_IDX],
                                    1) == -1) {
        ESP_LOGE(TAG, "Failed to subscribe to control channel!");
      } else {
        ESP_LOGI(TAG, "Subscribed to %s", topic_names[CMD_REQ_IDX]);
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "MQTT_EVENT_DISCONNECTED");
      xEventGroupClearBits(state.events, MQTT_CLIENT_CONNECTED_BIT);
      xEventGroupSetBits(state.events, MQTT_CLIENT_DISCONNECTED_BIT);
      break;
    case MQTT_EVENT_SUBSCRIBED:
      ESP_LOGD(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      ESP_LOGD(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_DATA:
      if (strcmp(event->topic, topic_names[CMD_REQ_IDX]) == 0) {
        mqttmgr_cmd_dispatch(event);
      }
      break;
    default:
      break;
  }
}

/**
 * @brief Watchdog for ESP-MQTT-Client
 *
 * Used to implement a proper backoff so the client doesn't retry forever
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
static void mqttmgr_client_watcher(void *pvParam) {
  uint16_t nextRetryBackoff = 0;
  uint8_t disconn_event_count = 0;
  ESP_LOGI(TAG, "Starting mqtt-client-watchdog");
  for (;;) {
    xEventGroupWaitBits(state.events,
                        MQTT_CLIENT_STARTED_BIT | MQTT_CLIENT_DISCONNECTED_BIT,
                        pdFALSE,  // Do NOT clear the bits before returning
                        pdTRUE,   // Wait for ALL bits to be set
                        portMAX_DELAY);
    xEventGroupClearBits(state.events, MQTT_CLIENT_DISCONNECTED_BIT);
    ESP_LOGD(TAG, "Counting a disconnected error event for backoff");
    disconn_event_count++;
    if (disconn_event_count == 2) {
      BackoffAlgorithm_GetNextBackoff(&retryParams, esp_random(),
                                      &nextRetryBackoff);
      ESP_LOGI(TAG, "Too many mqtt disconnections, backing off for %d seconds",
               nextRetryBackoff);
      esp_mqtt_client_stop(state.client);
      xEventGroupClearBits(state.events,
                           MQTT_CLIENT_STARTED_BIT | MQTT_CLIENT_CONNECTED_BIT);

      vTaskDelay((nextRetryBackoff * 1000) / portTICK_PERIOD_MS);

      ESP_LOGI(TAG, "attempting to connect to mqtt again...");
      esp_mqtt_client_start(state.client);
      xEventGroupSetBits(state.events, MQTT_CLIENT_STARTED_BIT);
      disconn_event_count = 0;
    }
  }
}

/**
 * @brief Task for sending sensor data
 */
static void mqttmgr_sensor_topic_task(void *pvParam) {
  cJSON *root;
  char *json;
  uint8_t idx;
  int res;

  ESP_LOGD(TAG, "mqtt task entering loop");
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGD(TAG, "notified, waiting for mqtt server connection");
    xEventGroupWaitBits(state.events,
                        MQTT_CLIENT_STARTED_BIT | MQTT_CLIENT_CONNECTED_BIT,
                        pdFALSE,  // Do NOT clear the bits before returning
                        pdTRUE,   // Wait for ALL bits to be set
                        portMAX_DELAY);
    ESP_LOGD(TAG, "building JSON");

    root = cJSON_CreateObject();

    ESP_LOGD(TAG, "Handlers: %d", state.json_handler_cnt);
    // TODO: RingBuffer draining could happen here, handlers return a MORE_MSGS
    // flag after gathering max message buffer size amount?
    for (idx = 0; idx < state.json_handler_cnt; idx++) {
      state.json_handlers[idx](root);
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_LOGD(TAG, "sending sensor data");
    ESP_LOGD(TAG, "JSON MSG: %s", json);
    res = esp_mqtt_client_publish(state.client, topic_names[SENSOR_IDX], json,
                                  0,  // Calc the length from the msg directly
                                  1,  // QoS 1
                                  1   // Retain as the last msg from this device
    );
    if (res == -1) {
      ESP_LOGE(TAG, "Failed to enqueue mqtt message!");
    }
  }
}

esp_err_t mqttmgr_init(char *device_id) {
  // Setup topic names
  sprintf(topic_names[CMD_REQ_IDX], "%s/command/req/", device_id);
  sprintf(topic_names[CMD_RESP_IDX], "%s/command/resp/", device_id);
  sprintf(topic_names[SENSOR_IDX], "%s/sensordata/", device_id);
  sprintf(topic_names[LOG_IDX], "%s/logs/", device_id);

  // Configure MQTT client
  esp_mqtt_client_config_t mqtt_cfg = {
      .buffer_size = 4096,
      .uri = "mqtt://mqtt.kaffi.home"  // TODO: Make this configurable, store in
                                       // NVS?
  };

  // Init state, cmd_handlers, and backoff state
  state = (mqttmgr_state_t){
      .events = xEventGroupCreate(),
      .disabled_at = 0,
      .retry_count = 0,
      .client = esp_mqtt_client_init(&mqtt_cfg),
      .cmd_handlers =
          calloc(command_request__descriptor.n_fields, sizeof(cmdhandler *))};
  if (state.cmd_handlers == NULL) {
    ESP_LOGE(TAG, "Failed to allocate cmd_handlers array");
    return ESP_FAIL;
  }

  xEventGroupClearBits(state.events, 0xFF);  // Clear all event bits
  BackoffAlgorithm_InitializeParams(&retryParams, MQTT_BASE_BACKOFF_SEC,
                                    MQTT_MAX_BACKOFF,
                                    BACKOFF_ALGORITHM_RETRY_FOREVER);

  return ESP_OK;
}

esp_err_t mqttmgr_register_sensor_encoder(esp_err_t (*fn)(cJSON *)) {
  if (state.json_handler_cnt >= MQTT_HANDLERS_MAX) {
    ESP_LOGE(TAG, "Max number of json handlers registered: %d",
             MQTT_HANDLERS_MAX);
    return ESP_FAIL;
  }
  state.json_handlers[state.json_handler_cnt++] = fn;
  ESP_LOGD(TAG, "Registered event handler");
  return ESP_OK;
}

esp_err_t mqttmgr_start() {
  BaseType_t result;
  result = xTaskCreate(mqttmgr_sensor_topic_task, MQTT_TASK_NAME,
                       MQTT_TASK_STACKSIZE, (void *)1, tskIDLE_PRIORITY,
                       &state.mqtt_task_handle);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Error starting mqtt task!");
    return ESP_FAIL;
  }

  result = xTaskCreate(mqttmgr_client_watcher, MQTT_CLIENTWATCHER_NAME,
                       MQTT_CLIENTWATCHER_STACKSIZE, (void *)1,
                       tskIDLE_PRIORITY, &state.mqtt_client_watcher_handle);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Error starting mqtt-client-watcher task!");
    return ESP_FAIL;
  }

  esp_mqtt_client_register_event(state.client,
                                 (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                 mqttmgr_event_handler, state.client);
  esp_mqtt_client_start(state.client);
  xEventGroupSetBits(state.events, MQTT_CLIENT_STARTED_BIT);

  ESP_LOGI(TAG, "mqtt task started");
  return ESP_OK;
}

esp_err_t mqttmgr_stop() {
  if (xEventGroupGetBits(state.events) & MQTT_CLIENT_STARTED_BIT) {
    xEventGroupClearBits(state.events,
                         MQTT_CLIENT_STARTED_BIT | MQTT_CLIENT_CONNECTED_BIT);
    esp_mqtt_client_stop(state.client);
  }

  if (state.mqtt_task_handle != NULL) {
    vTaskSuspend(state.mqtt_task_handle);
  }

  return ESP_OK;
}

esp_err_t mqttmgr_notify() {
  if (state.mqtt_task_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  xTaskNotifyGive(state.mqtt_task_handle);
  return ESP_OK;
}

esp_err_t mqttmgr_register_cmd_handler(cmdhandler *handler) {
  if (state.cmd_handlers == NULL) {
    ESP_LOGE(TAG, "Handler registration before initialization");
    return ESP_ERR_INVALID_STATE;
  }
  if (state.cmd_handler_cnt >= command_request__descriptor.n_fields) {
    ESP_LOGE(TAG, "Attempt to register too many cmd_handlers");
    return ESP_FAIL;
  }

  state.cmd_handlers[state.cmd_handler_cnt++] = handler;
  return ESP_OK;
}
