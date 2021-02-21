#ifndef WIFI_PROVISION_H
#define WIFI_PROVISION_H

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data);

void wifi_provision();

#endif
