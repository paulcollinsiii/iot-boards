#include "sensormgr.h"

#include <commands.pb-c.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs.h>
#include <esp_vfs_fat.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <mqttlog.h>
#include <mqttmgr.h>
#include <stdatomic.h>
#include <string.h>

// TODO: Convert this to an actual kconfig value
#define CONFIG_SENSOR_COUNT 4

#define SENSORMGR_TASKNAME_READ "sensormgr"
#define SENSORMGR_TASKNAME_QUEUE "sensormgr-q"
#define SENSORMGR_TASKNAME_FILEWRITER "sensormgr-fw"
#define SENSORMGR_TASK_STACKSIZE 3 * 1024
#define SENSORMGR_RINBUFFER_SIZE CONFIG_SENSORMGR_RINGBUF_SIZE * 1024
// Ringbuffer free space is by max item size that can be sent in
#define SENSORMGR_RINBUFFER_LOWWATER (SENSORMGR_RINBUFFER_SIZE / 2) - 32
// TODO: This should be configurable via the command channel
#define SENSORMGR_RINBUFFER_LOWWATER_ITEM_CNT 15
// 12% space remaining means drain the queue to the FS
#define SENSORMGR_RINBUFFER_HIGHWATER SENSORMGR_RINBUFFER_SIZE / 8
// 128K left on FS means stop writing for now
#define SENSORMGR_FS_HIGHWATER 128

typedef struct _state_t {
  bool initilized;
  uint8_t sensor_cnt;
  uint8_t LOWWATER_ITEM_CNT;
  sensormgr_registration_t sensors[CONFIG_SENSOR_COUNT];
  TaskHandle_t measure_task_handle, queue_task_handle, filewriter_task_handle;
  RingbufHandle_t ring_buffer;
  wl_handle_t wl_handle;
  atomic_uint_fast32_t ring_buffer_item_count;
  atomic_bool has_files;  // Are there files that need to be drained?
} state_t;

typedef struct _sensor_reading_t {
  uint8_t type_idx;
  size_t sensor_data_len;
  uint8_t sensor_data[];
} sensor_reading_t;

typedef enum _sensor_iterator_state_t {
  INIT = 0,
  HFNO,  // Has Files, None Open
  HFOO,  // Has Files, One Open
  NFRB,  // No Files, Ring Buffer
} sensor_iterator_state_t;

typedef struct sensor_iterator_t {
  sensor_iterator_state_t state;
  FILE *f_in;
  char f_name[24];
  sensor_reading_t *reading;
  size_t reading_size;
} sensor_iterator_t;

static const char *TAG = SENSORMGR_TASKNAME_READ;
static state_t state;

// Pre-declare my static functions
static esp_err_t sensormgr_get_first_datafile(FILE **fp, char *f_name,
                                              size_t f_name_size);
static esp_err_t sensormgr_read_iter(sensor_iterator_t *iter_state,
                                     bool read_files);
static uint32_t sensormgr_free_space();
static void sensormgr_task_file_writer(void *pvParam);
static void sensormgr_get_free_space(uint32_t *fre_kb, uint32_t *tot_kb);
static void sensormgr_log_free_space();
static void sensormgr_task_queuesend(void *pvParam);
static void sensormgr_task_sensorread(void *pvParam);

static esp_err_t sensormgr_get_stats(Sensormgr__GetStatsResponse *stats) {
  EventBits_t curr_events = xEventGroupGetBits(mqttmgr_events);
  stats->uptime_microsec = esp_timer_get_time();
  sensormgr_get_free_space(&stats->disk_free_kb, &stats->disk_total_kb);
  stats->ringbuffer_low_water = curr_events & SENSORMGR_LOWWATER_BIT;
  stats->ringbuffer_high_water = curr_events & SENSORMGR_HIGHWATER_BIT;
  stats->disk_high_water = stats->disk_free_kb < SENSORMGR_FS_HIGHWATER;

  return ESP_OK;
}

static void sensormgr_log_stats() {
  Sensormgr__GetStatsResponse local;
  const uint64_t q = 1000, s = 60;
  char uptime[64];

  sensormgr_get_stats(&local);

  snprintf(uptime, 64, "%02llu:%02llu:%02llu.%03llu",
           local.uptime_microsec / s / s / q / q,
           local.uptime_microsec / s / q / q % s,
           local.uptime_microsec / q / q % s, local.uptime_microsec / q % q);

  MQTTLOG_LOGI(
      TAG, "current stats",
      "disk_free_kb=%u disk_total_size=%u low_water=%b high_water=%b uptime=%s",
      local.disk_free_kb, local.disk_total_kb, local.ringbuffer_low_water,
      local.ringbuffer_high_water, uptime);
}

static void sensormgr_cmd_get_stats_dealloc_cb(CommandResponse *resp_out) {
  ESP_LOGD(TAG, "sensormgrmgmt_cmd_set_options_dealloc_cb - freeing");
  free(resp_out->sensormgr_get_stats_response);
}

static CommandResponse__RetCodeT sensormgr_cmd_get_stats(
    CommandRequest *msg, CommandResponse *resp_out, dealloc_cb_fn **cb) {
  if (msg->cmd_case != COMMAND_REQUEST__CMD_SENSORMGR_GET_STATS_REQUEST) {
    return COMMAND_RESPONSE__RET_CODE_T__NOTMINE;
  }

  resp_out->resp_case = COMMAND_RESPONSE__RESP_SENSORMGR_GET_STATS_RESPONSE;
  *cb = sensormgr_cmd_get_stats_dealloc_cb;
  Sensormgr__GetStatsResponse *cmd_resp = (Sensormgr__GetStatsResponse *)calloc(
      1, sizeof(Sensormgr__GetStatsResponse));
  sensormgr__get_stats_response__init(cmd_resp);
  resp_out->sensormgr_get_stats_response = cmd_resp;

  sensormgr_get_stats(cmd_resp);
  sensormgr_log_stats();  // TODO: Enh, duplicate work T-T

  return COMMAND_RESPONSE__RET_CODE_T__HANDLED;
}

static esp_err_t sensormgr_read_iter(sensor_iterator_t *iter_state,
                                     bool read_files) {
  size_t free_buf_size;
  esp_err_t ret;

  // cases
  //  Initial state unknown INIT
  //    detect config and set correct state
  //  has files, none open  HFNO
  //    detect file --> HFOO
  //    detect no more file --> NFRB
  //  has files, one open HFOO
  //    eof --> cleanup & HFNO
  //  ring buffer NFRB
  while (1) {
    switch (iter_state->state) {
      case INIT:
        if (read_files && state.has_files) {
          iter_state->state = HFNO;
        } else {
          iter_state->state = NFRB;
        }
        break;
      case HFNO:
        ret = sensormgr_get_first_datafile(&(iter_state->f_in),
                                           iter_state->f_name,
                                           sizeof(iter_state->f_name));
        if (ESP_ERR_NOT_FOUND == ret) {
          state.has_files = false;
          iter_state->state = NFRB;
          break;
        } else if (ESP_OK == ret) {
          iter_state->state = HFOO;
          ESP_LOGI(TAG, "iter - reading file: %s", iter_state->f_name);
          break;
        }
        ESP_LOGE(TAG, "Bad state transition HFNO --> UNKNOWN");
        abort();
        break;
      case HFOO:
        // Don't read while writing, unlikely but safety first!
        xEventGroupWaitBits(mqttmgr_events, SENSORMGR_DONEWRITING_BIT,
                            pdFALSE,  // Do NOT clear the bits before returning
                            pdTRUE,   // Wait for ALL bits to be set
                            portMAX_DELAY);
        if (iter_state->reading == NULL) {
          iter_state->reading = (sensor_reading_t *)calloc(512, 1);
        }
        if (fread(iter_state->reading, sizeof(sensor_reading_t), 1,
                  iter_state->f_in) < 1) {
          // Got less than one result, thus EOF
          ESP_LOGI(TAG, "iter - unlinking published file: %s",
                   iter_state->f_name);
          fclose(iter_state->f_in);
          remove(iter_state->f_name);  // Delete the file
          iter_state->f_in = NULL;
          free(iter_state->reading);
          iter_state->reading = NULL;
          iter_state->f_name[0] = '\0';
          iter_state->state = HFNO;
          sensormgr_log_free_space();
          break;
        }
        // Sanity check sensor reading to prevent overflow
        if ((sizeof(sensor_reading_t) + iter_state->reading->sensor_data_len) >
            512) {
          ESP_LOGE(
              TAG, "Sensor read is larger than buffer! %u > 512",
              sizeof(sensor_reading_t) + iter_state->reading->sensor_data_len);
          abort();
        }
        if (fread(iter_state->reading->sensor_data,
                  iter_state->reading->sensor_data_len, 1,
                  iter_state->f_in) < 1) {
          ESP_LOGE(TAG, "Failed to read a sensor reading");
          abort();
        }
        return ESP_OK;
        break;
      case NFRB:
        if (iter_state->reading != NULL) {
          vRingbufferReturnItem(state.ring_buffer,
                                (void *)(iter_state->reading));
          state.ring_buffer_item_count--;
        }
        iter_state->reading = (sensor_reading_t *)xRingbufferReceive(
            state.ring_buffer, &(iter_state->reading_size), 0);
        // Clear highwater only when reading from ring buffer if space is
        // correct Clear lowwater only when reading from ring buffer on empty
        free_buf_size = xRingbufferGetCurFreeSize(state.ring_buffer);
        if (free_buf_size >= SENSORMGR_RINBUFFER_HIGHWATER) {
          xEventGroupClearBits(mqttmgr_events, SENSORMGR_HIGHWATER_BIT);
          ESP_LOGV(TAG, "high-water bit clear: %d >= %d", free_buf_size,
                   SENSORMGR_RINBUFFER_HIGHWATER);
        }
        if (state.ring_buffer_item_count == 0) {
          xEventGroupClearBits(mqttmgr_events, SENSORMGR_LOWWATER_BIT);
          ESP_LOGI(TAG, "low-water bit clear: %d (item cnt) = 0",
                   state.ring_buffer_item_count);
        }
        if (iter_state->reading == NULL) {
          // Reset the state since the ring buffer has been drained
          iter_state->state = INIT;
          // Set Polling enabled since at least the RB has been drained
          // In normal cases this would also mean the file system has been
          // fully drained as well
          xEventGroupSetBits(mqttmgr_events, SENSORMGR_POLLSENSORS_BIT);
        }
        return ESP_OK;
        break;
      default:
        abort();
    }
  }
}

static void sensormgr_get_free_space(uint32_t *fre_kb, uint32_t *tot_kb) {
  FATFS *fs;
  f_getfree("0:", fre_kb, &fs);
  *tot_kb = ((fs->n_fatent - 2) * fs->csize * fs->ssize) / 1024;
  *fre_kb = (*fre_kb * fs->csize * fs->ssize) / 1024;
}

static uint32_t sensormgr_free_space() {
  uint32_t fre_kb, tot_kb;
  sensormgr_get_free_space(&fre_kb, &tot_kb);
  return fre_kb;
}

static void sensormgr_log_free_space() {
  uint32_t fre_kb, tot_kb;
  sensormgr_get_free_space(&fre_kb, &tot_kb);
  /* Print the free space (assuming 512 bytes/sector) */
  ESP_LOGI(TAG, "%5u / %5u KiB free / total drive space.", fre_kb, tot_kb);
}

/**
 * @brief Get the first file and open a file handle to it
 *
 * @param fp File pointer to open against or set NULL of no file
 */
static esp_err_t sensormgr_get_first_datafile(FILE **fp, char *f_name,
                                              size_t f_name_size) {
  FILINFO fno;
  FF_DIR dj;

  ESP_LOGI(TAG, "Searching for datafiles");
  f_opendir(&dj, "/");
  while (F_OK == f_readdir(&dj, &fno) && fno.fname[0]) {
    ESP_LOGI(TAG, "manual fileiter - %s", fno.fname);
    snprintf(f_name, f_name_size, "/log_data/%s", fno.fname);
    *fp = fopen(f_name, "rb");
    if (*fp == NULL) {
      ESP_LOGE(TAG, "Error opening datafile: %s", f_name);
      abort();
    }
    f_closedir(&dj);
    return ESP_OK;
  }
  ESP_LOGI(TAG, "manual filtiter - complete");
  *fp = NULL;

  return ESP_ERR_NOT_FOUND;
}

// At highwater and disconnected, buffer to file

static void sensormgr_task_file_writer(void *pvParam) {
  uint32_t bytes_free, bytes_written;
  time_t timestamp;
  struct tm timestamp_tm;
  char f_name[24];
  FILE *f_out = NULL;
  sensor_iterator_t iter_state = {
      .state = INIT,
      .f_in = NULL,
      .f_name[0] = '\0',
      .reading = NULL,
      .reading_size = 0,
  };

  // High watermark means drain ring buffer to the file till either empty or low
  // watermark

  ESP_LOGI(TAG, "File writing task starting...");
  sensormgr_log_free_space();
  for (;;) {
    xEventGroupWaitBits(
        mqttmgr_events,
        MQTTMGR_CLIENT_NOTCONNECTED_BIT | SENSORMGR_HIGHWATER_BIT,
        pdFALSE,  // Do NOT clear the bits before returning
        pdTRUE,   // Wait for ALL bits to be set
        portMAX_DELAY);
    ESP_LOGI(TAG, "File writing task wake...");
    // Check freespace, if we're too low then wait till something else has
    // drained the FS for us
    if (sensormgr_free_space() < SENSORMGR_FS_HIGHWATER) {
      ESP_LOGI(TAG, "File writing task paused, not enough free space...");
      ESP_LOGI(TAG, "File writing task pausing sensor polling...");
      // Clear polling, but make sure the system wants to drain the queue
      xEventGroupClearBits(mqttmgr_events, SENSORMGR_POLLSENSORS_BIT);
      xEventGroupSetBits(mqttmgr_events, SENSORMGR_LOWWATER_BIT);
      xEventGroupWaitBits(
          mqttmgr_events,
          SENSORMGR_POLLSENSORS_BIT,  // Will be re-enabled by the read iterator
          pdFALSE,                    // Do NOT clear the bits before returning
          pdTRUE,                     // Wait for ALL bits to be set
          portMAX_DELAY);
      // Restart the loop, since we expect the iter to have drained everything
      // here
      continue;
    }
    // About to go active writing to a file
    xEventGroupClearBits(mqttmgr_events, SENSORMGR_DONEWRITING_BIT);
    // open a timestamped file DDHHMMSS e.g. 01121500 - 1st of month, 12:15:00;
    // full month wrap-around is a _lot_ of data, beyond what could likely be
    // stored on one of these esp's
    time(&timestamp);
    gmtime_r(&timestamp, &timestamp_tm);
    strftime(f_name, 24, "/log_data/%d%H%M%S.BIN", &timestamp_tm);
    if (f_out != NULL) {
      ESP_LOGE(TAG, "Previously open file was not closed! Aborting");
      abort();
    }
    ESP_LOGI(TAG, "Writing current ringbuffer to: %s", f_name);
    f_out = fopen(f_name, "wb");
    if (f_out == NULL) {
      ESP_LOGE(TAG, "Failed to output file");
      abort();
    }
    bytes_free = (sensormgr_free_space() - SENSORMGR_FS_HIGHWATER) * 1024;
    bytes_written = 0;
    for (;;) {
      // Read JUST the ring buffers
      sensormgr_read_iter(&iter_state, false);
      if (iter_state.reading == NULL) {
        break;
      }
      // Dump the entry straight to the file
      fwrite(iter_state.reading, iter_state.reading_size, 1, f_out);
      bytes_written += iter_state.reading_size;
      // Allowed to go slightly over "free" due to reserved space
      if (bytes_written > bytes_free) {
        break;
      }
    }
    fclose(f_out);
    state.has_files = true;
    ESP_LOGI(TAG, "closing: %s", f_name);
    f_out = NULL;
    xEventGroupSetBits(mqttmgr_events, SENSORMGR_DONEWRITING_BIT);
  }
}

// At low-water try to send data if connected, till empty.
static void sensormgr_task_queuesend(void *pvParam) {
  uint8_t idx;
  sensor_iterator_t iter_state = {
      .state = INIT,
      .f_in = NULL,
      .f_name[0] = '\0',
      .reading = NULL,
      .reading_size = 0,
  };
  cJSON *root, *sensor_array, *sensor_data, *metadata;
  char *json_text;
  esp_err_t ret;

  ESP_LOGI(TAG, "Staring %s task", SENSORMGR_TASKNAME_QUEUE);
  for (;;) {
    xEventGroupWaitBits(mqttmgr_events,
                        MQTTMGR_CLIENT_STARTED_BIT |
                            MQTTMGR_CLIENT_CONNECTED_BIT |
                            SENSORMGR_LOWWATER_BIT,
                        pdFALSE,  // Do NOT clear the bits before returning
                        pdTRUE,   // Wait for ALL bits to be set
                        portMAX_DELAY);
    ESP_LOGD(TAG, "marshalling loop...");
    root = cJSON_CreateObject();
    cJSON_AddItemToObject(
        root, "metadata",
        metadata = cJSON_CreateObject());  // TODO: how to get device-id and
                                           // friendly name here?
    cJSON_AddItemToObject(root, "data", sensor_array = cJSON_CreateArray());
    for (idx = 0; idx < 10; idx++) {
      sensormgr_read_iter(&iter_state, true);
      if (iter_state.reading == NULL) {
        break;  // No data in ringbuffer, wait to be signled
      }
      state.sensors[iter_state.reading->type_idx].marshall(
          iter_state.reading->sensor_data, sensor_array);
    }
    if (idx != 0) {
      do {
        json_text = cJSON_PrintUnformatted(root);
        if (json_text == NULL) {
          ESP_LOGE(TAG,
                   "Marshalling Failed! Delaying in hope of more memory...");
          vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
      } while (json_text == NULL);
      ret = mqttmgr_queuemsg(MQTTMGR_TOPIC_SENSOR, strlen(json_text), json_text,
                             portMAX_DELAY);
      if (ret == ESP_ERR_INVALID_ARG) {
        abort();  // We configured messages badly if this happens
      }
    }
    cJSON_Delete(root);  // Cleanup after all that JSON
    free(json_text);
    root = sensor_array = sensor_data = metadata = NULL;
    json_text = NULL;
  }
}

// Poll only while able to buffer safely
static void sensormgr_task_sensorread(void *pvParam) {
  uint8_t idx, loop_cnt = 0;
  void *sensor_data_ptr;
  size_t sensor_data_len, free_buf_size;
  esp_err_t ret;
  sensor_reading_t *wrapped_reading;

  ESP_LOGI(TAG, "Starting %s task", SENSORMGR_TASKNAME_READ);
  for (;; loop_cnt++) {
    xEventGroupWaitBits(mqttmgr_events, SENSORMGR_POLLSENSORS_BIT,
                        pdFALSE,  // Do NOT clear the bits before returning
                        pdTRUE,   // Wait for ALL bits to be set
                        portMAX_DELAY);
    ESP_LOGD(TAG, "Polling %x sensors", state.sensor_cnt);
    for (idx = 0; idx < state.sensor_cnt; idx++) {
      sensor_data_ptr = NULL;
      sensor_data_len = 0;
      ret = state.sensors[idx].measure(&sensor_data_ptr, &sensor_data_len);
      ESP_LOGV(TAG, "Storing %d in ringbuf (free: %d)", sensor_data_len,
               xRingbufferGetCurFreeSize(state.ring_buffer));
      if (ret == ESP_OK) {
        while (pdTRUE != xRingbufferSendAcquire(
                             state.ring_buffer, (void **)&wrapped_reading,
                             sizeof(sensor_reading_t) + sensor_data_len, 0)) {
          // This likely means the FS is full
          // The ring buffer is full
          // AND MQTT is offline
          // We have to wait for that to come back, and for the buffers to drain
          // It can take a bit for the FS to drain as well so delay for 5
          // seconds here too
          ESP_LOGE(TAG, "Error storing measurement in ring buffer");
          xEventGroupWaitBits(
              mqttmgr_events,
              MQTTMGR_CLIENT_CONNECTED_BIT | SENSORMGR_POLLSENSORS_BIT,
              pdFALSE,  // Do NOT clear the bits before returning
              pdTRUE,   // Wait for ALL bits to be set
              portMAX_DELAY);
          vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
        *wrapped_reading = (sensor_reading_t){
            .type_idx = idx,
            .sensor_data_len = sensor_data_len,
        };
        memcpy(wrapped_reading->sensor_data, sensor_data_ptr, sensor_data_len);
        xRingbufferSendComplete(state.ring_buffer, wrapped_reading);
        state.ring_buffer_item_count++;
      }
    }
    ESP_LOGV(TAG, "data written to ringbuffer");
    free_buf_size = xRingbufferGetCurFreeSize(state.ring_buffer);
    if (free_buf_size < SENSORMGR_RINBUFFER_LOWWATER ||
        (state.LOWWATER_ITEM_CNT != 0 &&
         state.ring_buffer_item_count > state.LOWWATER_ITEM_CNT)) {
      xEventGroupSetBits(mqttmgr_events, SENSORMGR_LOWWATER_BIT);
      ESP_LOGI(TAG,
               "low-water bit set: (low: %d, high: %d) %d < %d | %d (item cnt) "
               "> %d (low water mark)",
               SENSORMGR_RINBUFFER_LOWWATER, SENSORMGR_RINBUFFER_HIGHWATER,
               free_buf_size, SENSORMGR_RINBUFFER_LOWWATER,
               state.ring_buffer_item_count, state.LOWWATER_ITEM_CNT);
    }
    if (free_buf_size < SENSORMGR_RINBUFFER_HIGHWATER) {
      xEventGroupSetBits(mqttmgr_events, SENSORMGR_HIGHWATER_BIT);
      ESP_LOGI(TAG, "high-water bit set: %d < %d", free_buf_size,
               SENSORMGR_RINBUFFER_HIGHWATER);
    }
    vTaskDelay(CONFIG_SENSORMGR_SAMPLE_RATE / portTICK_RATE_MS);

    if (loop_cnt > 250) {
      sensormgr_log_stats();
      loop_cnt = 0;
    }
  }
}

// Check filebuffers, vfat space remaining, set can buffer flags
esp_err_t sensormgr_init() {
  FILE *f_test;
  char f_name[24];

  state = (state_t){
      .LOWWATER_ITEM_CNT = SENSORMGR_RINBUFFER_LOWWATER_ITEM_CNT,
      .ring_buffer =
          xRingbufferCreate(SENSORMGR_RINBUFFER_SIZE, RINGBUF_TYPE_NOSPLIT),
      .ring_buffer_item_count = 0,
      .wl_handle = 0,
      .measure_task_handle = NULL,
      .queue_task_handle = NULL,
      .filewriter_task_handle = NULL,
      .initilized = true,
  };

  // While this starts the polling process, if there are files pending
  // it'll take till LOW-WATER for those to get drained
  xEventGroupSetBits(mqttmgr_events,
                     SENSORMGR_POLLSENSORS_BIT | SENSORMGR_DONEWRITING_BIT);

  if (state.ring_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to create ring buffer");
    return ESP_FAIL;
  }

  esp_vfs_fat_sdmmc_mount_config_t vfat_config = {
      .format_if_mount_failed = true,
      .max_files = 4,
      .allocation_unit_size = 0,
  };

  ESP_LOGI(TAG, "Attempting to mount log data partition");
  esp_vfs_fat_spiflash_mount("/log_data", "log_data", &vfat_config,
                             &state.wl_handle);
  if (ESP_OK == sensormgr_get_first_datafile(&f_test, f_name, sizeof(f_name))) {
    fclose(f_test);
    state.has_files = true;
    ESP_LOGI(TAG, "Previously saved sensordata detected!");
  }

  mqttmgr_register_cmd_handler(sensormgr_cmd_get_stats);

  return ESP_OK;
}

esp_err_t sensormgr_start() {
  if (!state.initilized) {
    ESP_LOGE(TAG, "Start before init!");
    abort();
  }

  // Reading sensor data is slightly higher priority than other tasks
  if (state.measure_task_handle == NULL &&
      pdPASS != xTaskCreate(sensormgr_task_sensorread, SENSORMGR_TASKNAME_READ,
                            SENSORMGR_TASK_STACKSIZE, (void *)1, 1,
                            &state.measure_task_handle)) {
    ESP_LOGE(TAG, "Failed creating task sensorread!");
    return ESP_FAIL;
  } else if (state.measure_task_handle != NULL &&
             eTaskGetState(state.measure_task_handle) == eSuspended) {
    vTaskResume(state.measure_task_handle);
  }

  if (state.queue_task_handle == NULL &&
      pdPASS != xTaskCreate(sensormgr_task_queuesend, SENSORMGR_TASKNAME_QUEUE,
                            SENSORMGR_TASK_STACKSIZE, (void *)1,
                            tskIDLE_PRIORITY, &state.queue_task_handle)) {
    ESP_LOGE(TAG, "Failed creating task queuesend!");
    return ESP_FAIL;
  } else if (state.queue_task_handle != NULL &&
             eTaskGetState(state.queue_task_handle) == eSuspended) {
    vTaskResume(state.queue_task_handle);
  }

  // Sensor writing tasks are also slightly higher priority as dumping data out
  // will prevent running out of memory.
  if (state.filewriter_task_handle == NULL &&
      pdPASS != xTaskCreate(sensormgr_task_file_writer,
                            SENSORMGR_TASKNAME_FILEWRITER,
                            SENSORMGR_TASK_STACKSIZE, (void *)1, 1,
                            &state.filewriter_task_handle)) {
    ESP_LOGE(TAG, "Failed creating task filewriter!");
    return ESP_FAIL;
  } else if (state.filewriter_task_handle != NULL &&
             eTaskGetState(state.filewriter_task_handle) == eSuspended) {
    vTaskResume(state.filewriter_task_handle);
  }

  sensormgr_log_stats();
  ESP_LOGI(TAG, "started!");
  return ESP_OK;
}

esp_err_t sensormgr_stop() {
  xEventGroupClearBits(mqttmgr_events, SENSORMGR_POLLSENSORS_BIT);

  vTaskSuspend(state.measure_task_handle);
  vTaskSuspend(state.queue_task_handle);

  xEventGroupSetBits(mqttmgr_events, SENSORMGR_HIGHWATER_BIT);
  ESP_LOGW(TAG, "Waiting for file writer task to finish up");
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  xEventGroupWaitBits(mqttmgr_events, SENSORMGR_DONEWRITING_BIT, pdFALSE,
                      pdTRUE, portMAX_DELAY);

  vTaskSuspend(state.filewriter_task_handle);
  return ESP_OK;
}

esp_err_t sensormgr_register_sensor(sensormgr_registration_t reg) {
  if (state.sensor_cnt >= CONFIG_SENSOR_COUNT) {
    ESP_LOGE(TAG, "Sensor register overflow");
    return ESP_ERR_NO_MEM;
  }

  state.sensors[state.sensor_cnt++] = reg;
  return ESP_OK;
}
