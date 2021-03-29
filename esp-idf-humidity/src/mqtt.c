#include "mqtt.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/event_groups.h>

#define MQTT_TASK_NAME "mqtt"
#define MQTT_TASK_STACKSIZE 2 * 1024
#define MQTT_HANDLERS_MAX 8

#define MQTT_CLIENTWATCHER_NAME "mqtt-watcher"
#define MQTT_CLIENTWATCHER_STACKSIZE 2 * 1024

#define MQTT_CLIENT_STARTED_BIT (1 << 0)
#define MQTT_CLIENT_CONNECTED_BIT (1 << 1)
#define MQTT_CLIENT_DISCONNECTED_BIT (1 << 2)

static const char *TAG = "mqtt";

typedef struct {
  EventGroupHandle_t events;
  TaskHandle_t mqtt_client_watcher_handle;

  time_t disabled_at;
  uint8_t retry_count;
  esp_err_t (*handlers[MQTT_HANDLERS_MAX])(cJSON *);
  uint8_t handler_count;
  esp_mqtt_client_handle_t client;
} mqtt_state_t;

static mqtt_state_t state;

/**
 * @brief Stop all MQTT related task handlers
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base,
           event_id);

  switch (event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
      break;
    case MQTT_EVENT_CONNECTED:
      ESP_LOGD(TAG, "MQTT_EVENT_CONNECTED");
      xEventGroupSetBits(state.events, MQTT_CLIENT_CONNECTED_BIT);
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
      ESP_LOGI(TAG, "MQTT_EVENT_DATA");
      printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
      printf("DATA=%.*s\r\n", event->data_len, event->data);
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
static void mqtt_client_watcher(void *pvParam) {
  int disconn_event_count = 0;
  ESP_LOGI(TAG, "Starting mqtt-client-watchdog");
  for (;;) {
    xEventGroupWaitBits(state.events,
                        MQTT_CLIENT_STARTED_BIT | MQTT_CLIENT_DISCONNECTED_BIT,
                        pdFALSE,  // Do NOT clear the bits before returning
                        pdTRUE,   // Wait for ALL bits to be set
                        portMAX_DELAY);
    xEventGroupClearBits(state.events, MQTT_CLIENT_DISCONNECTED_BIT);
    ESP_LOGI(TAG, "Counting a disconnected error event for backoff");
    // TODO: Use the backoff library to do something more like expo backoff,
    // start with server retries of 1 min and go up to 15?
    disconn_event_count++;
    if (disconn_event_count > 3) {
      ESP_LOGI(TAG, "Too many mqtt disconnections, stopping mqtt");
      esp_mqtt_client_stop(state.client);
      xEventGroupClearBits(state.events,
                           MQTT_CLIENT_STARTED_BIT | MQTT_CLIENT_CONNECTED_BIT);
    }
  }
}

/**
 * @brief Stop all MQTT related task handlers
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
static void mqtt_task(void *pvParam) {
  cJSON *root;
  char *json;
  uint8_t idx;

  ESP_LOGD(TAG, "mqtt task entering loop");
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "notified, waiting for mqtt server connection");
    xEventGroupWaitBits(state.events,
                        MQTT_CLIENT_STARTED_BIT | MQTT_CLIENT_CONNECTED_BIT,
                        pdFALSE,  // Do NOT clear the bits before returning
                        pdTRUE,   // Wait for ALL bits to be set
                        portMAX_DELAY);
    ESP_LOGI(TAG, "building & sending JSON");

    root = cJSON_CreateObject();

    ESP_LOGI(TAG, "Handlers: %d", state.handler_count);
    for (idx = 0; idx < state.handler_count; idx++) {
      state.handlers[idx](root);
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "JSON QUEUED MSG Output: %s", json);
  }
}

esp_err_t mqtt_init() {
  memset(&state, 0, sizeof(state));
  mqtt_task_handle = NULL;

  esp_mqtt_client_config_t mqtt_cfg = {
      .uri = "mqtt://192.168.2.31"};  // TODO: DNS this...

  state.events = xEventGroupCreate();
  xEventGroupClearBits(state.events, 0xFF);  // Zero out everything
  state.disabled_at = 0;
  state.retry_count = 0;
  state.client = esp_mqtt_client_init(&mqtt_cfg);

  return ESP_OK;
}

esp_err_t mqtt_register(esp_err_t (*fn)(cJSON *)) {
  if (state.handler_count >= MQTT_HANDLERS_MAX) {
    ESP_LOGE(TAG, "Max number of handlers registered: %d", MQTT_HANDLERS_MAX);
    return ESP_FAIL;
  }
  // TODO: Worth checking for double registration?
  state.handlers[state.handler_count++] = fn;
  ESP_LOGI(TAG, "Registered event handler");
  return ESP_OK;
}

esp_err_t mqtt_start() {
  BaseType_t result;
  result = xTaskCreate(mqtt_task, MQTT_TASK_NAME, MQTT_TASK_STACKSIZE,
                       (void *)1, tskIDLE_PRIORITY, &mqtt_task_handle);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Error starting mqtt task!");
    return ESP_FAIL;
  }

  result = xTaskCreate(mqtt_client_watcher, MQTT_CLIENTWATCHER_NAME,
                       MQTT_CLIENTWATCHER_STACKSIZE, (void *)1,
                       tskIDLE_PRIORITY, &state.mqtt_client_watcher_handle);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Error starting mqtt-client-watcher task!");
    return ESP_FAIL;
  }

  esp_mqtt_client_register_event(state.client,
                                 (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, state.client);
  esp_mqtt_client_start(state.client);
  xEventGroupSetBits(state.events, MQTT_CLIENT_STARTED_BIT);

  ESP_LOGI(TAG, "mqtt task started");
  return ESP_OK;
}

esp_err_t mqtt_stop() {
  if (xEventGroupGetBits(state.events) & MQTT_CLIENT_STARTED_BIT) {
    xEventGroupClearBits(state.events,
                         MQTT_CLIENT_STARTED_BIT | MQTT_CLIENT_CONNECTED_BIT);
    esp_mqtt_client_stop(state.client);
  }

  if (mqtt_task_handle != NULL) {
    vTaskSuspend(mqtt_task_handle);
  }

  return ESP_OK;
}
