#include <sdkconfig.h>

#if CONFIG_TOUCHBTN_ENABLED
#ifndef TOUCHBTN_H
#define TOUCHBTN_H

#if CONFIG_IDF_TARGET_ESP32
#include <touchbtn_esp32.h>
#elif CONFIG_IDF_TARGET_ESP32S2
#include <touchbtn_esp32s2.h>
#endif

void touchbtn_init();
void touchbtn_deepsleep();
void touchbtn_wifiwake();

#endif
#endif
