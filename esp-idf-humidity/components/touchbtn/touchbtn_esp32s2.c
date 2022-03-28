#include <sdkconfig.h>

#if CONFIG_TOUCHBTN_ENABLED && CONFIG_IDF_TARGET_ESP32S2

#include <blinky.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <touchbtn.h>

static const char *TAG = "touchbtn-esp32s2";

typedef struct _btn_click_t {
  uint64_t microsecond_ts;
  // True - Button is pressed / False - Button is NOT pressed
  bool state;
} btn_click_t;

typedef struct _state_t {
  QueueHandle_t queue;
  TaskHandle_t task;
} state_t;

static state_t state;

static void touchbtn_gpio_isr(void *arg) {
  btn_click_t btn_state = {
      .microsecond_ts = esp_timer_get_time(),
      .state = gpio_get_level(GPIO_NUM_0),
  };
  xQueueSendToBackFromISR(state.queue, &btn_state, NULL);
}

static void touchbtn_btn_task(void *pvParameter) {
  btn_click_t btn_click, btn_click2;
  ESP_LOGI(TAG, "button task started...");

  while (true) {
    xQueueReceive(state.queue, &btn_click, portMAX_DELAY);
    ESP_LOGI(TAG, "Got button click: %lld :: %d", btn_click.microsecond_ts,
             btn_click.state);
    if (pdTRUE ==
            xQueueReceive(state.queue, &btn_click2, 500 / portTICK_PERIOD_MS) &&
        ((btn_click2.microsecond_ts - btn_click.microsecond_ts) > 1000)) {
      ESP_LOGI(TAG, "Double Click!");
      touchbtn_deepsleep();
      continue;
    }
    ESP_LOGI(TAG, "Single Click!");
    touchbtn_wifiwake();
  }
}

void touchbtn_init() {
  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_POSEDGE,
      .mode = GPIO_MODE_INPUT,
      .pin_bit_mask = GPIO_SEL_0,
      .pull_down_en = 0,
      .pull_up_en = 1,
  };
  gpio_config(&io_conf);
  gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
  gpio_isr_handler_add(GPIO_NUM_0, &touchbtn_gpio_isr, 0);

  state = (state_t){
      .queue = xQueueCreate(2, sizeof(btn_click_t)),
  };
  if (state.queue == NULL) {
    ESP_LOGE(TAG, "Unable to create button queue!");
  }
  if (pdPASS !=
      xTaskCreate(touchbtn_btn_task, TAG, 2048, NULL, 5, &state.task)) {
    ESP_LOGE(TAG, "Failed to create task %s", TAG);
  }
  ESP_LOGW(TAG, "GPIO Button enabled...");
}

#endif
