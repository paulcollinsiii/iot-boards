/*
 * ESP-IDF LTR390 Sensor Managment Package
 */

#include <sdkconfig.h>

#if CONFIG_LTR390_ENABLED
#ifndef ESP_LTR390MGR_H
#define ESP_LTR390MGR_H

#include <cJSON.h>
#include <esp_err.h>
#include <mqttmgr.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Init the ltr390 manager
 *
 * Registers MQTT command handlers
 * Registers the SensorMgr measure & marshall handlers
 *
 * @param type_id
 */
esp_err_t ltr390mgr_init();

#ifdef __cplusplus
}
#endif
#endif
#endif
