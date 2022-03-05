/*
 * Really simple alarm clock
 */

#include "alarm.h"

#include <commands.pb-c.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "cron.h"
#include "mqttmgr.h"
#include "sdkconfig.h"

#define ENABLE_UNDEFINED 0
#define ENABLE_FALSE 1
#define ENABLE_TRUE 2

static const char *TAG = "alarm";

typedef struct {
  uint8_t enabled;
  bool oneshot;
  char *crontab;
  cron_job *job;
} alarm_priv_t;

typedef struct {
  alarm_priv_t alarms[CONFIG_ALARM_NUM_MAX];
} alarm_state_t;

static alarm_state_t state;

static void alarm_task(struct cron_job_struct *job) {
  ESP_LOGI(TAG, "alarm fired!");
  for (int i = 0; i < 5; i++) {
    gpio_set_level(GPIO_NUM_13, 1);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    gpio_set_level(GPIO_NUM_13, 0);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

static void alarm_add_cmdhandler_dealloc_cb(CommandResponse *resp_out) {
  ESP_LOGD(TAG, "alarm_add_cmdhandler_dealloc_cb - freeing");
  free(resp_out->alarm_add_response);
}

static CommandResponse__RetCodeT alarm_add_cmdhandler(CommandRequest *msg,
                                                      CommandResponse *resp_out,
                                                      dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_ALARM_ADD_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  resp_out->resp_case = COMMAND_RESPONSE__RESP_ALARM_ADD_RESPONSE;
  *cb = alarm_add_cmdhandler_dealloc_cb;
  Alarm__AddResponse *alr =
      (Alarm__AddResponse *)calloc(1, sizeof(Alarm__AddResponse));
  alarm__add_response__init(alr);
  resp_out->alarm_add_response = alr;

  Alarm__AddRequest *cmd = msg->alarm_add_request;
  ESP_LOGD(TAG, "alarm_add_cmdhandler(%s, %s, oneshot:%s)", msg->uuid,
           cmd->crontab, cmd->oneshot ? "true" : "false");

  alarm_t alarm = {.crontab = cmd->crontab, .oneshot = false};

  if (alarm_add(&alarm) != ESP_OK) {
    ESP_LOGW(TAG, "alarm_add_cmdhandler - ERR");
    return COMMAND_RESPONSE__RET_CODE_T__ERR;
  }
  ESP_LOGI(TAG, "alarm_add_cmdhandler(%s, %s, oneshot:%s) - OK", msg->uuid,
           cmd->crontab, cmd->oneshot ? "true" : "false");
  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

static void alarm_delete_cmdhandler_dealloc_cb(CommandResponse *resp_out) {
  ESP_LOGD(TAG, "alarm_delete_cmdhandler_dealloc_cb - freeing");
  free(resp_out->alarm_delete_response);
}

static CommandResponse__RetCodeT alarm_delete_cmdhandler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_ALARM_DELETE_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }
  resp_out->resp_case = COMMAND_RESPONSE__RESP_ALARM_DELETE_RESPONSE;
  *cb = alarm_delete_cmdhandler_dealloc_cb;
  Alarm__DeleteResponse *alr =
      (Alarm__DeleteResponse *)calloc(1, sizeof(Alarm__DeleteResponse));
  alarm__delete_response__init(alr);
  resp_out->alarm_delete_response = alr;

  ESP_LOGD(TAG, "alarm_delete_cmdhandler - search");
  Alarm__DeleteRequest *cmd = msg->alarm_delete_request;

  ESP_LOGD(TAG, "alarm_delete_cmdhandler(%s, %s) - start", msg->uuid,
           cmd->crontab);

  switch (alarm_delete(cmd->crontab)) {
    case ESP_OK:
      ESP_LOGI(TAG, "alarm_delete_cmdhandler(%s, %s) - OK", msg->uuid,
               cmd->crontab);
      return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
    case ESP_ERR_NOT_FOUND:
      ESP_LOGI(TAG, "alarm_delete_cmdhandler(%s, %s) - NOT FOUND", msg->uuid,
               cmd->crontab);
      return COMMAND_RESPONSE__RET_CODE_T__ERR;
    default:
      return COMMAND_RESPONSE__RET_CODE_T__ERR;
  }
  return COMMAND_RESPONSE__RET_CODE_T__ERR;
}

static void alarm_list_cmdhandler_dealloc_cb(CommandResponse *resp_out) {
  ESP_LOGD(TAG, "alarm_list_cmdhandler_dealloc_cb - freeing");
  Alarm__ListResponse *alr = resp_out->alarm_list_response;
  if (alr->n_alarms > 0) {
    free(alr->alarms);
  }
  free(alr);
}

// Return the index of the next alarm (start
// The very first call to this should set last_idx = -1
static int alarm_idx_iter(int last_idx) {
  last_idx++;
  for (; last_idx < CONFIG_ALARM_NUM_MAX; last_idx++) {
    if (state.alarms[last_idx].enabled != ENABLE_UNDEFINED) {
      return last_idx;
    }
  }
  return -2;
}

static CommandResponse__RetCodeT alarm_list_cmdhandler(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  uint8_t i, n_alarms = 0;
  int alarm_idx = -1;

  if (msg->cmd_case != COMMAND_REQUEST__CMD_ALARM_LIST_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  for (i = 0; i < CONFIG_ALARM_NUM_MAX; i++) {
    if (state.alarms[i].enabled == ENABLE_UNDEFINED) {
      continue;
    }
    n_alarms++;
  }

  resp_out->resp_case = COMMAND_RESPONSE__RESP_ALARM_LIST_RESPONSE;
  *cb = alarm_list_cmdhandler_dealloc_cb;
  Alarm__ListResponse *alr =
      (Alarm__ListResponse *)calloc(1, sizeof(Alarm__ListResponse));
  alarm__list_response__init(alr);
  resp_out->alarm_list_response = alr;

  alr->n_alarms = n_alarms;
  if (n_alarms != 0) {
    alr->alarms = calloc(n_alarms, sizeof(Alarm__ListResponse__Alarm *));
    for (i = 0; i < n_alarms; i++) {
      alarm_idx = alarm_idx_iter(alarm_idx);
      // We should be getting exactly the right number of idx
      if (alarm_idx == -2) {
        ESP_LOGW(TAG, "alarm_list_cmdhandler - Error finding all alarms");
        alr->n_alarms = i;  // So the cleanup callback can correctly free
        return COMMAND_RESPONSE__RET_CODE_T__ERR;
      }
      ESP_LOGD(TAG, "alarm_list_cmdhandler - Adding alarm in list #%d state#%d",
               i, alarm_idx);

      alr->alarms[i] = calloc(1, sizeof(Alarm__ListResponse__Alarm));
      alarm__list_response__alarm__init(alr->alarms[i]);
      alr->alarms[i]->crontab = state.alarms[alarm_idx].crontab;
      alr->alarms[i]->oneshot = state.alarms[alarm_idx].oneshot;
      alr->alarms[i]->enabled = state.alarms[alarm_idx].enabled == ENABLE_TRUE;
    }

    ESP_LOGD(TAG, "alarm_list_cmdhandler - Alarms added to list.");

    alarm_idx = alarm_idx_iter(alarm_idx);
    if (alarm_idx != -2) {
      ESP_LOGE(TAG,
               "alarm_list_cmdhandler - Error failed to add all alarms (%d)",
               alarm_idx);
      return COMMAND_RESPONSE__RET_CODE_T__ERR;
    }
  }

  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

esp_err_t alarm_add(alarm_t *alarm) {
  uint8_t i;
  enum cron_job_errors ret;

  // Dupe check
  for (i = 0; i < CONFIG_ALARM_NUM_MAX; i++) {
    if (state.alarms[i].enabled == ENABLE_UNDEFINED) {
      continue;
    }
    if (state.alarms[i].enabled == ENABLE_TRUE &&
        strcmp(state.alarms[i].crontab, alarm->crontab) == 0) {
      ESP_LOGI(TAG, "Discarding re-add of existing alarm");
      return ESP_ERR_INVALID_STATE;
    }
  }
  ESP_LOGD(TAG, "alarm_add(%s) - dupe check done", alarm->crontab);
  ret = cron_stop();  // Before any cron modification it must be stopped
  if (ret != Cron_ok && ret != Cron_is_stopped) {
    ESP_LOGE(TAG, "alarm_add - Unable to stop cron to edit jobs");
    return ESP_ERR_INVALID_STATE;
  }

  // Create the alarm in our state
  for (i = 0; i < CONFIG_ALARM_NUM_MAX; i++) {
    if (state.alarms[i].enabled == ENABLE_UNDEFINED) {
      ESP_LOGD(TAG, "alarm_add(%s) - creating...", alarm->crontab);
      state.alarms[i].enabled = ENABLE_TRUE;
      state.alarms[i].crontab =
          (char *)calloc(strlen(alarm->crontab) + 1, sizeof(char));
      strncpy(state.alarms[i].crontab, alarm->crontab, strlen(alarm->crontab));
      state.alarms[i].job =
          cron_job_create(state.alarms[i].crontab, alarm_task, (void *)0);
      if (state.alarms[i].job == NULL) {
        ESP_LOGE(TAG, "Err creating alarm (%s)", alarm->crontab);
      }
      ESP_LOGI(TAG, "alarm_add(%s) - created...", alarm->crontab);
      if (cron_start() != Cron_ok) {
        ESP_LOGE(TAG, "alarm_add - failed starting cron system");
        return ESP_ERR_INVALID_STATE;
      }
      return ESP_OK;
    }
  }

  ret = cron_start();
  if (ret != Cron_ok && ret != Cron_not_stopped) {
    ESP_LOGE(TAG, "alarm_add - failed starting cron system");
    return ESP_ERR_INVALID_STATE;
  }

  // All alarm slots were filled?
  ESP_LOGW(TAG, "No open alarm slots, discarding add request");
  return ESP_ERR_NO_MEM;
}

esp_err_t alarm_delete(char *crontab) {
  uint8_t i;
  enum cron_job_errors ret;

  ret = cron_stop();  // Before any cron modification it must be stopped
  if (ret != Cron_ok && ret != Cron_is_stopped) {
    ESP_LOGE(TAG, "alarm_add - Unable to stop cron to edit jobs");
    return ESP_ERR_INVALID_STATE;
  }

  for (i = 0; i < CONFIG_ALARM_NUM_MAX; i++) {
    if (state.alarms[i].enabled == ENABLE_TRUE &&
        strcmp(state.alarms[i].crontab, crontab) == 0) {
      // Clean up the alarm from the state
      ESP_LOGD(TAG, "Destroying alarm %d", i);
      if (Cron_ok != cron_job_destroy(state.alarms[i].job)) {
        ESP_LOGE(TAG, "Unable to remove cronjob!");
        return ESP_FAIL;
      }
      free(state.alarms[i].crontab);
      memset(state.alarms + i, 0, sizeof(alarm_priv_t));
      state.alarms[i].enabled = ENABLE_UNDEFINED;
      if (cron_start() != Cron_ok) {
        ESP_LOGE(TAG, "alarm_add - failed starting cron system");
        return ESP_ERR_INVALID_STATE;
      }
      return ESP_OK;
    }
  }

  if (cron_start() != Cron_ok) {
    ESP_LOGE(TAG, "alarm_add - failed starting cron system");
    return ESP_ERR_INVALID_STATE;
  }
  ESP_LOGI(TAG, "alarm_delete(%s) not found", crontab);
  return ESP_ERR_NOT_FOUND;
}

esp_err_t alarm_init() {
  // Config GPIO light
  gpio_config_t io_conf;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = GPIO_SEL_13;
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;
  gpio_config(&io_conf);

  // Init state
  // Thing to check, static state is null init? If so no need to memset here
  cron_job_init();

  // Register Command Handlers
  mqttmgr_register_cmd_handler(alarm_add_cmdhandler);
  mqttmgr_register_cmd_handler(alarm_delete_cmdhandler);
  mqttmgr_register_cmd_handler(alarm_list_cmdhandler);

  return ESP_OK;
}
