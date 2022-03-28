#include <blinky.h>
#include <commands.pb-c.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mqttmgr.h>
#include <stdbool.h>
#include <string.h>

typedef struct _state_t {
  spi_device_handle_t spi;
  TaskHandle_t task;
  QueueHandle_t blinky_queue;
} state_t;

static state_t state;
static const char *TAG = "blinky";

// Queue a big const from
// https://www.instructables.com/How-to-Make-Proper-Rainbow-and-Random-Colors-With-/
const uint8_t lights[360] = {
    0,   0,   0,   0,   0,   1,   1,   2,   2,   3,   4,   5,   6,   7,   8,
    9,   11,  12,  13,  15,  17,  18,  20,  22,  24,  26,  28,  30,  32,  35,
    37,  39,  42,  44,  47,  49,  52,  55,  58,  60,  63,  66,  69,  72,  75,
    78,  81,  85,  88,  91,  94,  97,  101, 104, 107, 111, 114, 117, 121, 124,
    127, 131, 134, 137, 141, 144, 147, 150, 154, 157, 160, 163, 167, 170, 173,
    176, 179, 182, 185, 188, 191, 194, 197, 200, 202, 205, 208, 210, 213, 215,
    217, 220, 222, 224, 226, 229, 231, 232, 234, 236, 238, 239, 241, 242, 244,
    245, 246, 248, 249, 250, 251, 251, 252, 253, 253, 254, 254, 255, 255, 255,
    255, 255, 255, 255, 254, 254, 253, 253, 252, 251, 251, 250, 249, 248, 246,
    245, 244, 242, 241, 239, 238, 236, 234, 232, 231, 229, 226, 224, 222, 220,
    217, 215, 213, 210, 208, 205, 202, 200, 197, 194, 191, 188, 185, 182, 179,
    176, 173, 170, 167, 163, 160, 157, 154, 150, 147, 144, 141, 137, 134, 131,
    127, 124, 121, 117, 114, 111, 107, 104, 101, 97,  94,  91,  88,  85,  81,
    78,  75,  72,  69,  66,  63,  60,  58,  55,  52,  49,  47,  44,  42,  39,
    37,  35,  32,  30,  28,  26,  24,  22,  20,  18,  17,  15,  13,  12,  11,
    9,   8,   7,   6,   5,   4,   3,   2,   2,   1,   1,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0};

// TODO:
// What I want out of this for the light is to have a way of queuing a pattern
// for it to display Run a rainbow once slow / med / fast Blink a pattern of
// colors passed in at a defined rate Blink the LED at a specific speed until
// notified Set the light to a specific color and leave it there till notified

// The above means that there's some kind of control language for this and that
// the task is notified of what it needs to do but can also be interrupted
// {speed, pattern, repeat, rgb/led} with speed 0 meaning constant pattern[0]?

static esp_err_t blinky_set_rgb_led(uint32_t brgb) {
  spi_transaction_t t;
  uint32_t df;

  // force correct leading global value
  brgb |= 0xe0000000;

  // dotstar is Blue Green Red ordering :headdesk:
  ESP_LOGV(TAG, "pre-swap:  0x%08x", brgb);
  df = ((0x000000FF & brgb) << 16) | ((0x00FF0000 & brgb) >> 16) |
       (0xFF00FF00 & brgb);
  brgb = df;
  ESP_LOGV(TAG, "post-swap: 0x%08x", brgb);

  memset(&t, 0, sizeof(t));
  t.length = 4 * 8;
  t.tx_buffer = &df;
  // TODO: This is LIKELY borked because there's no locking going on here....
  df = 0;
  ESP_ERROR_CHECK(spi_device_transmit(state.spi, &t));
  df = __bswap32(brgb);
  ESP_ERROR_CHECK(spi_device_transmit(state.spi, &t));
  df = 0xFFFFFFFF;
  ESP_ERROR_CHECK(spi_device_transmit(state.spi, &t));

  return ESP_OK;
}

static esp_err_t blinky_set_led(bool enabled) {
  gpio_set_level(GPIO_NUM_13, enabled ? 1 : 0);
  return ESP_OK;
}

static void blinky_set_led_request_handler_dealloc_cb(
    CommandResponse *resp_out) {
  ESP_LOGD(TAG, "blinky_set_led_request_handler_dealloc_cb - freeing");
  free(resp_out->blinky_set_led_response);
}

static CommandResponse__RetCodeT blinky_set_led_request_handler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_BLINKY_SET_LED_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  ESP_LOGD(TAG, "blinky_set_led_request_handler()");

  Blinky__SetLEDRequest *cmd = msg->blinky_set_led_request;

  resp_out->resp_case = COMMAND_RESPONSE__RESP_BLINKY_SET_LED_RESPONSE;
  *cb = blinky_set_led_request_handler_dealloc_cb;
  Blinky__SetLEDResponse *cmd_resp =
      (Blinky__SetLEDResponse *)calloc(1, sizeof(Blinky__SetLEDResponse));
  blinky__set_ledresponse__init(cmd_resp);
  resp_out->blinky_set_led_response = cmd_resp;

  blinky_animation_t animation = {
      .brgb = cmd->brgb,
      .ms_delay = cmd->ms_delay,
      .pattern = cmd->pattern,
      .repeat_count = cmd->repeat_count,
      .target = cmd->target,
      .off_at_end = cmd->off_at_end,
  };

  if (pdTRUE !=
      xQueueSend(state.blinky_queue, &animation, 500 / portTICK_PERIOD_MS)) {
    return COMMAND_RESPONSE__RET_CODE_T__ERR;
  }
  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

static void decr_repeat_count(blinky_animation_t *animation) {
  if (animation->repeat_count != UINT32_MAX && animation->repeat_count > 0)
    animation->repeat_count--;
}

static void rainbow(uint16_t *idx, blinky_animation_t *animation) {
  blinky_set_rgb_led(animation->brgb | (lights[(*idx + 120) % 360] << 16) |
                     (lights[*idx] << 8) | lights[(*idx + 240) % 360]);
  if (*idx > 360) {
    *idx = 0;
    decr_repeat_count(animation);
  }
}

static void blink_rgb(uint16_t *idx, blinky_animation_t *animation) {
  if (*idx & 0x01) {
    blinky_set_rgb_led(animation->brgb);
    decr_repeat_count(animation);
  } else {
    blinky_set_rgb_led(0xe0000000);
  }
}

static void blink_led(uint16_t *idx, blinky_animation_t *animation) {
  if (*idx & 0x01) {
    gpio_set_level(GPIO_NUM_13, 1);
    decr_repeat_count(animation);
  } else {
    gpio_set_level(GPIO_NUM_13, 0);
  }
}

static void blinky_rainbow_task(void *pvParameter) {
  uint16_t idx = 0;
  blinky_animation_t animation;
  void (*animation_ptr)(uint16_t *, blinky_animation_t *) = rainbow;

  while (true) {
    xQueueReceive(state.blinky_queue, &animation, portMAX_DELAY);
    // Start animation
    switch (animation.pattern) {
      case BLINKY__BLINKY_PATTERN_T__BLINK:
        animation_ptr = animation.target == BLINKY__BLINKY_LED_T__RGB_0
                            ? blink_rgb
                            : blink_led;
        break;
      case BLINKY__BLINKY_PATTERN_T__BREATH:
        animation_ptr = animation.target == BLINKY__BLINKY_LED_T__RGB_0
                            ? rainbow
                            : blink_led;
        break;
      case BLINKY__BLINKY_PATTERN_T__FADE_IN:
        animation_ptr = animation.target == BLINKY__BLINKY_LED_T__RGB_0
                            ? rainbow
                            : blink_led;
        break;
      case BLINKY__BLINKY_PATTERN_T__FADE_OUT:
        animation_ptr = animation.target == BLINKY__BLINKY_LED_T__RGB_0
                            ? rainbow
                            : blink_led;
        break;
      case BLINKY__BLINKY_PATTERN_T__RAINBOW:
        animation_ptr = animation.target == BLINKY__BLINKY_LED_T__RGB_0
                            ? rainbow
                            : blink_led;
        // BRGB ignored, except for brightness setting
        animation.brgb &= 0xFF000000;
        break;
      default:
        ESP_LOGE(TAG, "Unsupport animation requested");
        continue;
    }
    do {
      if (uxQueueMessagesWaiting(state.blinky_queue)) {
        idx = 0;
        break;
      }
      animation_ptr(&idx, &animation);
      idx++;
      vTaskDelay(animation.ms_delay / portTICK_PERIOD_MS);
    } while (animation.repeat_count != 0);
    if (animation.off_at_end) {
      if (animation.target == BLINKY__BLINKY_LED_T__LED_0) {
        blinky_set_led(false);
      }
      if (animation.target == BLINKY__BLINKY_LED_T__RGB_0) {
        blinky_set_rgb_led(0);
      }
    }
  }
}

esp_err_t blinky_init() {
  spi_bus_config_t led_data_bus_cfg = {.miso_io_num = -1,
                                       .mosi_io_num = GPIO_NUM_40,
                                       .sclk_io_num = GPIO_NUM_45,
                                       .quadwp_io_num = -1,  // not used
                                       .quadhd_io_num = -1,  // not used
                                       .max_transfer_sz = 1};
  spi_device_interface_config_t dotstar_cfg = {
      .clock_speed_hz = 20 * 1000 * 1000,
      .mode = 0,
      .spics_io_num = -1,
      .queue_size = 1,
  };
  // Also need to enable GPIO21 to send it power
  // Config GPIO light
  gpio_config_t io_conf = {
      io_conf.intr_type = GPIO_INTR_DISABLE,
      io_conf.mode = GPIO_MODE_OUTPUT,
      io_conf.pin_bit_mask = GPIO_SEL_13 | GPIO_SEL_21,
      io_conf.pull_down_en = 0,
      io_conf.pull_up_en = 0,
  };
  gpio_config(&io_conf);

  ESP_ERROR_CHECK(spi_bus_initialize(HSPI_HOST, &led_data_bus_cfg, 0));
  ESP_ERROR_CHECK(spi_bus_add_device(HSPI_HOST, &dotstar_cfg, &state.spi));
  gpio_set_level(GPIO_NUM_21, 1);
  gpio_set_level(GPIO_NUM_13, 1);

  ESP_ERROR_CHECK(mqttmgr_register_cmd_handler(blinky_set_led_request_handler));

  state.blinky_queue = xQueueCreate(2, sizeof(blinky_animation_t));

  if (pdPASS !=
      xTaskCreate(blinky_rainbow_task, TAG, 1536, NULL, 5, &state.task)) {
    ESP_LOGE(TAG, "Failed to create task %s", TAG);
    abort();
  }

  blinky_animation_t initial = {
      .ms_delay = 15,
      .repeat_count = 2,
      .brgb = 0xe1000000,
      .target = BLINKY__BLINKY_LED_T__RGB_0,
      .pattern = BLINKY__BLINKY_PATTERN_T__RAINBOW,
      .off_at_end = true,
  };
  xQueueSend(state.blinky_queue, &initial, portMAX_DELAY);
  gpio_set_level(GPIO_NUM_13, 0);
  return ESP_OK;
}

void blinky_play(blinky_animation_t *animation) {
  ESP_LOGI(TAG, "API Queue animation");
  xQueueSend(state.blinky_queue, animation, portMAX_DELAY);
}
