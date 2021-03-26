#include "mqtt.h"

#include <esp_event.h>
#include <esp_log.h>
#include <mqtt_client.h>

static const char *TAG = "mqtt";

typedef struct {
  bool disable_send;
  bool client_started;
  bool client_connected;
  time_t disabled_at;
  uint8_t disable_count;
  esp_mqtt_client_handle_t client;
} mqtt_state_t;

static mqtt_state_t state;

esp_err_t mqtt_setup() {
  esp_mqtt_client_config_t mqtt_cfg = {
      .uri = "mqtt://192.168.2.31"};  // TODO: DNS this...

  state.disable_send = false;
  state.client_started = false;
  state.client_connected = false;
  state.disabled_at = 0;
  state.disable_count = 0;
  state.client = esp_mqtt_client_init(&mqtt_cfg);

  esp_mqtt_client_register_event(state.client,
                                 (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, state.client);
  esp_mqtt_client_start(state.client);
  state.client_started = true;

  return ESP_OK;
}

esp_err_t mqtt_event_error_handler(esp_mqtt_error_codes_t *err) {
  switch (err->error_type) {
    case MQTT_ERROR_TYPE_ESP_TLS:
      ESP_LOGI(TAG, "MQTT Server Connection Error");
      state.disable_send = true;
      break;
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
      ESP_LOGI(
          TAG,
          "MQTT Server Connection Refused, disabling mqtt sending (err: %d)",
          err->connect_return_code);
      state.disable_send = true;
      break;
    default:
      ESP_LOGE(TAG, "Unknown error type in err handler");
      break;
  }
  return ESP_OK;
}

esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
  // your_context_t *context = event->context;
  switch (event->event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
      break;
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
      state.client_connected = true;
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
      state.client_connected = false;
      break;
    case MQTT_EVENT_SUBSCRIBED:
      ESP_LOGD(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      ESP_LOGD(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_PUBLISHED:
      ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
      break;
    case MQTT_EVENT_DATA:
      ESP_LOGI(TAG, "MQTT_EVENT_DATA");
      printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
      printf("DATA=%.*s\r\n", event->data_len, event->data);
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
      return mqtt_event_error_handler(event->error_handle);
      break;
    default:
      ESP_LOGI(TAG, "Other event id:%d", event->event_id);
      break;
  }
  return ESP_OK;
}

void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                        int32_t event_id, void *event_data) {
  ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base,
           event_id);
  mqtt_event_handler_cb((esp_mqtt_event_handle_t)event_data);
}

void mqtt_publish_sensor_data(char *json) {
  // TODO: Make the disabled send a bit smarter...
  if (state.disable_send ||
      !state
           .client_connected) {  // Don't start sending messages till we connect
    if (state.client_started && state.disable_send) {
      esp_mqtt_client_stop(state.client);
      state.client_started = false;
    }
    return;
  }
  esp_mqtt_client_publish(state.client, "/room/measurements", json, 0, 1, 0);
}
