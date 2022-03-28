#include "mqttmgr.h"

#include <backoff_algorithm.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <mqtt_client.h>

#define MQTT_TASK_NAME "mqtt"
#define MQTT_TASK_STACKSIZE 4 * 1024
#define MQTT_HANDLERS_MAX 8

#define MQTT_CLIENTWATCHER_NAME "mqtt-watcher"
#define MQTT_CLIENTWATCHER_STACKSIZE 2 * 1024

#define MQTT_BASE_BACKOFF_SEC 60
#define MQTT_MAX_BACKOFF 15 * 60

static const char *TAG = "mqtt";  // Logging handle name
char topic_names[4][64];

static BackoffAlgorithmContext_t retryParams;

typedef struct _mqttmgr_state_t {
  TaskHandle_t task_client_watchdog;  // TODO: Make exposed function to notify
                                      // this handler
  TaskHandle_t task_msgqueue;
  RingbufHandle_t msg_queue;

  time_t disabled_at;
  uint8_t retry_count;
  uint8_t json_handler_cnt;
  uint8_t cmd_handler_cnt;
  esp_mqtt_client_handle_t client;
  cmdhandler **cmd_handlers;
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
  if (esp_mqtt_client_publish(state.client, topic_names[MQTTMGR_TOPIC_RESPONSE],
                              (char *)buf, len * sizeof(char),
                              1,  // QoS 1
                              0   // Do not retain cmd responses
                              ) == -1) {
    ESP_LOGE(TAG, "mqttmgr_cmd_dispatch - publishing failed!");
  }

  // TODO: This dealloc callback is a silly complexity that should be dropped
  // Command handlers should return the packed reponse buffer only, so they can
  // alloc build and free the msg completely in the call. That leave the mqtt
  // manager to just forward the msg response itself
  if (dealloc_cb != NULL) {
    ESP_LOGI(TAG, "mqttmgr_cmd_dispatch - dealloc_cb start");
    dealloc_cb(&resp);
    ESP_LOGI(TAG, "mqttmgr_cmd_dispatch - dealloc_cb finish");
  }
  free(resp.uuid);
  free(buf);
}

/**
 * @brief Attempt to reconnect to Wifi and MQTT server now
 *
 * Will reset the backoff algorithm as well.
 *
 * @return esp_err_t
 *    ESP_OK - Reconnect Task Notified
 *    ESP_ERR_WIFI_STATE - Already connected
 */
esp_err_t mqttmgr_reconnect_now() {
  EventBits_t current_state = xEventGroupGetBits(mqttmgr_events);
  if (!(current_state & MQTTMGR_CLIENT_NOTCONNECTED_BIT)) {
    return ESP_ERR_WIFI_STATE;
  }
  xTaskNotifyGive(state.task_client_watchdog);
  return ESP_OK;
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
      xEventGroupSetBits(mqttmgr_events, MQTTMGR_CLIENT_CONNECTED_BIT);
      xEventGroupClearBits(mqttmgr_events, MQTTMGR_CLIENT_NOTCONNECTED_BIT);
      BackoffAlgorithm_InitializeParams(&retryParams, MQTT_BASE_BACKOFF_SEC,
                                        MQTT_MAX_BACKOFF,
                                        BACKOFF_ALGORITHM_RETRY_FOREVER);
      if (esp_mqtt_client_subscribe(
              state.client, topic_names[MQTTMGR_TOPIC_REQUEST], 1) == -1) {
        ESP_LOGE(TAG, "Failed to subscribe to control channel!");
      } else {
        ESP_LOGI(TAG, "Subscribed to %s", topic_names[MQTTMGR_TOPIC_REQUEST]);
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "MQTT_EVENT_DISCONNECTED");
      xEventGroupClearBits(mqttmgr_events, MQTTMGR_CLIENT_CONNECTED_BIT);
      xEventGroupSetBits(mqttmgr_events, MQTTMGR_CLIENT_DISCONNECTED_BIT |
                                             MQTTMGR_CLIENT_NOTCONNECTED_BIT);
      break;
    case MQTT_EVENT_SUBSCRIBED:
      ESP_LOGD(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      ESP_LOGD(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_DATA:
      if (strcmp(event->topic, topic_names[MQTTMGR_TOPIC_REQUEST]) == 0) {
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
static void mqttmgr_client_watchdog(void *pvParam) {
  uint16_t nextRetryBackoff = 0;
  uint8_t disconn_event_count = 0;
  ESP_LOGI(TAG, "Starting mqtt-client-watchdog");
  for (;;) {
    xEventGroupWaitBits(
        mqttmgr_events,
        MQTTMGR_CLIENT_STARTED_BIT | MQTTMGR_CLIENT_DISCONNECTED_BIT,
        pdFALSE,  // Do NOT clear the bits before returning
        pdTRUE,   // Wait for ALL bits to be set
        portMAX_DELAY);
    xEventGroupClearBits(mqttmgr_events, MQTTMGR_CLIENT_DISCONNECTED_BIT);
    ESP_LOGD(TAG, "Counting a disconnected error event for backoff");
    disconn_event_count++;
    if (disconn_event_count == 2) {
      BackoffAlgorithm_GetNextBackoff(&retryParams, esp_random(),
                                      &nextRetryBackoff);
      ESP_LOGI(TAG, "Too many mqtt disconnections, backing off for %d seconds",
               nextRetryBackoff);
      esp_mqtt_client_stop(state.client);
      // TODO: Handle the Wifi radio elsewhere?
      esp_wifi_disconnect();
      esp_wifi_stop();
      xEventGroupClearBits(mqttmgr_events, MQTTMGR_CLIENT_STARTED_BIT |
                                               MQTTMGR_CLIENT_CONNECTED_BIT);

      if (pdTRUE ==
          xTaskNotifyWait(0x0, ULONG_MAX, NULL,
                          (nextRetryBackoff * 1000) / portTICK_PERIOD_MS)) {
        ESP_LOGI(TAG, "(notified) attempting to connect to mqtt again...");
        ESP_LOGI(TAG, "(notified) resetting backoff params...");
        BackoffAlgorithm_InitializeParams(&retryParams, MQTT_BASE_BACKOFF_SEC,
                                          MQTT_MAX_BACKOFF,
                                          BACKOFF_ALGORITHM_RETRY_FOREVER);
      } else {
        ESP_LOGI(TAG, "attempting to connect to mqtt again...");
      }

      esp_wifi_start();
      vTaskDelay(5000 / portTICK_PERIOD_MS);  // 5 seconds to connect WiFi
      esp_mqtt_client_start(state.client);
      xEventGroupSetBits(mqttmgr_events, MQTTMGR_CLIENT_STARTED_BIT);
      disconn_event_count = 0;
    }
  }
}

/**
 * @brief Task for sending sensor data
 */
static void mqttmgr_task_msgqueue(void *pvParam) {
  bool resetBackoff = false;
  mqttmgr_msg_t *msg_buffer;
  size_t msg_size;
  uint16_t nextRetryBackoff = 0;
  BackoffAlgorithmContext_t mqttRetryParams;

  BackoffAlgorithm_InitializeParams(&mqttRetryParams, MQTT_BASE_BACKOFF_SEC,
                                    MQTT_MAX_BACKOFF,
                                    BACKOFF_ALGORITHM_RETRY_FOREVER);

  ESP_LOGD(TAG, "mqtt task entering loop");
  for (;;) {
    msg_buffer = (mqttmgr_msg_t *)xRingbufferReceive(state.msg_queue, &msg_size,
                                                     portMAX_DELAY);
    if (msg_buffer == NULL) {
      ESP_LOGE(TAG, "Failed to receive msg from msg queue!");
      abort();
    }
    xEventGroupWaitBits(
        mqttmgr_events,
        MQTTMGR_CLIENT_STARTED_BIT | MQTTMGR_CLIENT_CONNECTED_BIT,
        pdFALSE,  // Do NOT clear the bits before returning
        pdTRUE,   // Wait for ALL bits to be set
        portMAX_DELAY);
    ESP_LOGD(TAG, "publishing message to topic: %s",
             topic_names[msg_buffer->topic]);
    // Retry publishing the msg on failures with expo backoff
    while (-1 == esp_mqtt_client_publish(
                     state.client, topic_names[msg_buffer->topic],
                     (char *)msg_buffer->msg, msg_buffer->len,
                     1,  // QoS 1
                     1   // Retain as the last msg from this device
                     )) {
      ESP_LOGE(TAG, "Failed to enqueue mqtt message!");
      BackoffAlgorithm_GetNextBackoff(&mqttRetryParams, esp_random(),
                                      &nextRetryBackoff);
      resetBackoff = true;
      vTaskDelay((nextRetryBackoff * 1000) / portTICK_PERIOD_MS);
    }
    if (resetBackoff) {
      BackoffAlgorithm_InitializeParams(&mqttRetryParams, MQTT_BASE_BACKOFF_SEC,
                                        MQTT_MAX_BACKOFF,
                                        BACKOFF_ALGORITHM_RETRY_FOREVER);
      resetBackoff = false;
    }
    vRingbufferReturnItem(state.msg_queue, msg_buffer);
  }
}

esp_err_t mqttmgr_init(char *device_id) {
  // Setup topic names
  sprintf(topic_names[MQTTMGR_TOPIC_REQUEST], "command/%s/req/", device_id);
  sprintf(topic_names[MQTTMGR_TOPIC_RESPONSE], "command/%s/resp/", device_id);
  sprintf(topic_names[MQTTMGR_TOPIC_SENSOR], "sensordata/%s/", device_id);
  sprintf(topic_names[MQTTMGR_TOPIC_LOG], "logs/%s/", device_id);

  // Configure MQTT client
  esp_mqtt_client_config_t mqtt_cfg = {
      .buffer_size = 4096,
      .uri = "mqtt://mqtt.iot.kaffi.home"  // TODO: Make this configurable,
                                           // store in NVS?
  };

  // Init state, cmd_handlers, and backoff state
  mqttmgr_events = xEventGroupCreate();
  state = (mqttmgr_state_t){
      .msg_queue = xRingbufferCreate(4096, RINGBUF_TYPE_NOSPLIT),
      .disabled_at = 0,
      .retry_count = 0,
      .client = esp_mqtt_client_init(&mqtt_cfg),
      .cmd_handlers =
          calloc(command_request__descriptor.n_fields, sizeof(cmdhandler *))};
  if (state.cmd_handlers == NULL) {
    ESP_LOGE(TAG, "Failed to allocate cmd_handlers array");
    return ESP_FAIL;
  }

  if (state.msg_queue == NULL) {
    ESP_LOGE(TAG, "Failed to allocate message queue");
    return ESP_FAIL;
  }

  xEventGroupClearBits(mqttmgr_events, 0xFF);  // Clear all event bits
  BackoffAlgorithm_InitializeParams(&retryParams, MQTT_BASE_BACKOFF_SEC,
                                    MQTT_MAX_BACKOFF,
                                    BACKOFF_ALGORITHM_RETRY_FOREVER);

  return ESP_OK;
}

esp_err_t mqttmgr_start() {
  BaseType_t result;
  result =
      xTaskCreate(mqttmgr_task_msgqueue, MQTT_TASK_NAME, MQTT_TASK_STACKSIZE,
                  (void *)1, tskIDLE_PRIORITY, &state.task_msgqueue);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Error starting mqtt task!");
    return ESP_FAIL;
  }

  result = xTaskCreate(mqttmgr_client_watchdog, MQTT_CLIENTWATCHER_NAME,
                       MQTT_CLIENTWATCHER_STACKSIZE, (void *)1,
                       tskIDLE_PRIORITY, &state.task_client_watchdog);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Error starting mqtt-client-watcher task!");
    return ESP_FAIL;
  }

  esp_mqtt_client_register_event(state.client,
                                 (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                 mqttmgr_event_handler, state.client);
  esp_mqtt_client_start(state.client);
  xEventGroupSetBits(mqttmgr_events, MQTTMGR_CLIENT_STARTED_BIT);

  ESP_LOGI(TAG, "mqtt task started");
  return ESP_OK;
}

esp_err_t mqttmgr_stop() {
  if (xEventGroupGetBits(mqttmgr_events) & MQTTMGR_CLIENT_STARTED_BIT) {
    xEventGroupClearBits(mqttmgr_events, MQTTMGR_CLIENT_STARTED_BIT |
                                             MQTTMGR_CLIENT_CONNECTED_BIT);
    /*xEventGroupSetBits(mqttmgr_events, MQTTMGR_CLIENT_DISCONNECTED_BIT |*/
    /*MQTTMGR_CLIENT_NOTCONNECTED_BIT);*/
    esp_mqtt_client_stop(state.client);
  }

  if (state.task_msgqueue != NULL) {
    vTaskSuspend(state.task_msgqueue);
    vTaskSuspend(state.task_client_watchdog);
  }

  return ESP_OK;
}

esp_err_t mqttmgr_queuemsg(mqttmgr_topicidx topic, size_t msg_len, void *msg,
                           TickType_t delay) {
  mqttmgr_msg_t *rb_msg;

  if (state.task_msgqueue == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (pdTRUE != xRingbufferSendAcquire(state.msg_queue, (void **)&rb_msg,
                                       sizeof(mqttmgr_msg_t) + msg_len,
                                       delay)) {
    if (delay == portMAX_DELAY) {
      ESP_LOGE(TAG, "Unable to queue message: Message too large");
      return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGW(TAG, "Unable to queue msg!");
    return ESP_ERR_NO_MEM;
  }
  *rb_msg = (mqttmgr_msg_t){
      .len = msg_len,
      .topic = topic,
  };
  memcpy(rb_msg->msg, msg, msg_len);
  xRingbufferSendComplete(state.msg_queue, rb_msg);
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
