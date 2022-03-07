/*
 * ESP-IDF SHTC3 Sensor Managment Package
 */

#include <sdkconfig.h>
#if CONFIG_SHT4X_ENABLED

#ifndef ESP_SHTC3MGR_H
#define ESP_SHTC3MGR_H

#include <cJSON.h>
#include <esp_err.h>
#include <mqttmgr.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Init the ltr390 manager
 *
 * Registers MQTT command handlers and saves the type_id
 * Registers the SensorMgr measure & marshall handlers
 *
 * @param type_id
 */
esp_err_t sht4xmgr_init();

#ifdef __cplusplus
}
#endif
#endif
#endif
