#ifndef MQTT_H
#define MQTT_H

void mqtt_publish_sensor_data(char *json);
esp_err_t mqtt_setup();
esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event);
void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                        int32_t event_id, void *event_data);

#endif
