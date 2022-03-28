// Originally based on
// https://github.com/espressif/esp-idf/blob/2f9d47c708f39772b0e8f92d147b9e85aa3a0b19/examples/peripherals/touch_sensor/touch_sensor_v1/touch_pad_interrupt/main/tp_interrupt_main.c

#include <sdkconfig.h>
#if CONFIG_TOUCHBTN_ENABLED && CONFIG_IDF_TARGET_ESP32

#include <driver/touch_pad.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mqttmgr.h>
#include <sensormgr.h>
#include <soc/rtc_periph.h>
#include <soc/sens_periph.h>
#include <stdatomic.h>
#include <stdio.h>

#define TOUCHBTN_TASKNAME "touchbtn-task"
#define TOUCHBTN_THRESH_NO_USE 0
#define TOUCHBTN_WIFI_PAD_BITMASK 0x01 << CONFIG_TOUCHBTN_WIFI_PAD
#define TOUCHBTN_SLEEP_PAD_BITMASK 0x01 << CONFIG_TOUCHBTN_SLEEP_PAD
#define TOUCHPAD_FILTER_TOUCH_PERIOD 30

static const char *TAG = "touchbtn";

typedef struct _state_t {
  uint32_t s_pad_init_val[TOUCH_PAD_MAX];
  atomic_uint_fast32_t btn_laststate;
  TaskHandle_t task;
} state_t;

static state_t state;

/*
  Read values sensed at all available touch pads.
  Use 2 / 3 of read value as the threshold
  to trigger interrupt when the pad is touched.
  Note: this routine demonstrates a simple way
  to configure activation threshold for the touch pads.
  Do not touch any pads when this routine
  is running (on application start).
 */
static void touchbtn_set_threshold(touch_pad_t padnum) {
  uint16_t touch_value;
  touch_pad_read_filtered(padnum, &touch_value);
  state.s_pad_init_val[padnum] = touch_value;
  ESP_LOGI(TAG, "init: pad [%d] val is %d", padnum, touch_value);
  ESP_ERROR_CHECK(touch_pad_set_thresh(padnum, touch_value * 2 / 3));
}

static void touchbtn_isr_handler(void *arg) {
  uint32_t btn_bitmask = touch_pad_get_status();
  if (btn_bitmask ^ state.btn_laststate) {
    xTaskNotifyFromISR(state.task, btn_bitmask, eSetBits, NULL);
  }
}

static void touchbtn_task_handler(void *pvParameter) {
  uint32_t btn_bitmask;

  while (true) {
    xTaskNotifyWait(state.btn_laststate, ULONG_MAX, &btn_bitmask,
                    portMAX_DELAY);
    state.btn_laststate = btn_bitmask;
    if (btn_bitmask & TOUCHBTN_WIFI_PAD_BITMASK) {
      toubhtbtn_wifiwake();
    }
    if (btn_bitmask & TOUCHBTN_SLEEP_PAD_BITMASK) {
      touchbtn_deepsleep();
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    state.btn_laststate = 0x0;
  }
}

void touchbtn_init() {
  state = (state_t){
      .btn_laststate = 0x0,
  };

  // Initialize touch pad peripheral, it will start a timer to run a filter
  ESP_LOGI(TAG, "Setup touch buttons...");
  ESP_ERROR_CHECK(touch_pad_init());
  touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
  touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);

  // Init touch pad IO
  touch_pad_config(CONFIG_TOUCHBTN_WIFI_PAD, TOUCHBTN_THRESH_NO_USE);
  touch_pad_config(CONFIG_TOUCHBTN_SLEEP_PAD, TOUCHBTN_THRESH_NO_USE);

  touch_pad_filter_start(TOUCHPAD_FILTER_TOUCH_PERIOD);
  touchbtn_set_threshold(CONFIG_TOUCHBTN_WIFI_PAD);
  touchbtn_set_threshold(CONFIG_TOUCHBTN_SLEEP_PAD);

  // Register touch interrupt ISR
  touch_pad_isr_register(touchbtn_isr_handler, NULL);
  touch_pad_intr_enable();

  if (pdPASS != xTaskCreate(touchbtn_task_handler, TOUCHBTN_TASKNAME, 2048,
                            NULL, 5, &state.task)) {
    ESP_LOGE(TAG, "Failed to create task %s", TOUCHBTN_TASKNAME);
    abort();
  }
  ESP_LOGI(TAG, "Setup touch buttons COMPLETE");
}

#endif
