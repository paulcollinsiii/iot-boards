#include "sensormgr.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs.h>
#include <esp_vfs_fat.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <mqttmgr.h>
#include <stdatomic.h>
#include <string.h>

// TODO: Convert this to an actual kconfig value
#define CONFIG_SENSOR_COUNT 2

#define SENSORMGR_MEASURETASK_NAME "sensormgr"
#define SENSORMGR_QUEUETASK_NAME "sensormgr-q"
#define SENSORMGR_FILEWRITERTASK_NAME "sensormgr-fw"
#define SENSORMGR_TASK_STACKSIZE 4 * 1024
#define SENSORMGR_RINBUFFER_SIZE CONFIG_SENSORMGR_RINGBUF_SIZE * 1024
// Ringbuffer free space is by max item size that can be sent in
#define SENSORMGR_RINBUFFER_LOWWATER (SENSORMGR_RINBUFFER_SIZE / 2) - 32
// TODO: This should be configurable via the command channel
#define SENSORMGR_RINBUFFER_LOWWATER_ITEM_CNT 15
// 12% space remaining means drain the queue to the FS
#define SENSORMGR_RINBUFFER_HIGHWATER SENSORMGR_RINBUFFER_SIZE / 8
// 128K left on FS means stop writing for now
#define SENSORMGR_FS_HIGHWATER 128

typedef struct {
  bool initilized;
  uint8_t sensor_cnt;
  sensormgr_registration_t sensors[CONFIG_SENSOR_COUNT];
  TaskHandle_t measure_task_handle, queue_task_handle, filewriter_task_handle;
  RingbufHandle_t ring_buffer;
  wl_handle_t wl_handle;
  atomic_uint_fast32_t ring_buffer_item_count;
  atomic_bool has_files;  // Are there files that need to be drained?
} state_t;

typedef struct {
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

static const char *TAG = SENSORMGR_MEASURETASK_NAME;
static state_t state;

// Pre-declare my static functions
static esp_err_t sensormgr_get_first_datafile(FILE **fp, char *f_name,
                                              size_t f_name_size);
static esp_err_t sensormgr_read_iter(sensor_iterator_t *iter_state,
                                     bool read_files);
static uint32_t sensormgr_free_space();
static void sensormgr_file_writer_task(void *pvParam);
static void sensormgr_queuesend_task(void *pvParam);
static void sensormgr_sensorread_task(void *pvParam);

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

static uint32_t sensormgr_free_space() {
  FATFS *fs;
  uint32_t fre_kb, tot_kb;

  f_getfree("0:", &fre_kb, &fs);
  tot_kb = ((fs->n_fatent - 2) * fs->csize * fs->ssize) / 1024;
  fre_kb = (fre_kb * fs->csize * fs->ssize) / 1024;

  /* Print the free space (assuming 512 bytes/sector) */
  ESP_LOGI(TAG, "%10u KiB total drive space.\n\t%10u KiB available.", tot_kb,
           fre_kb);
  return fre_kb;
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

static void sensormgr_file_writer_task(void *pvParam) {
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
      xEventGroupClearBits(mqttmgr_events, SENSORMGR_POLLSENSORS_BIT);
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
    for (;;) {
      // Read JUST the ring buffers
      sensormgr_read_iter(&iter_state, false);
      if (iter_state.reading == NULL) {
        break;
      }
      // Dump the entry straight to the file
      fwrite(iter_state.reading, iter_state.reading_size, 1, f_out);
    }
    fclose(f_out);
    state.has_files = true;
    ESP_LOGI(TAG, "closing: %s", f_name);
    f_out = NULL;
    if (sensormgr_free_space() <
        SENSORMGR_FS_HIGHWATER) {  // File space is full, stop polling
      xEventGroupClearBits(mqttmgr_events, SENSORMGR_POLLSENSORS_BIT);
      // TODO: Does anything _set_ this bit if we have enough freespace?
    }
    xEventGroupSetBits(mqttmgr_events, SENSORMGR_DONEWRITING_BIT);
  }
}

// At low-water try to send data if connected, till empty.
static void sensormgr_queuesend_task(void *pvParam) {
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
  mqttmgr_msg_t msg;
  esp_err_t ret;

  ESP_LOGI(TAG, "Staring %s task", SENSORMGR_QUEUETASK_NAME);
  for (;;) {
    xEventGroupWaitBits(mqttmgr_events,
                        MQTTMGR_CLIENT_STARTED_BIT |
                            MQTTMGR_CLIENT_CONNECTED_BIT |
                            SENSORMGR_LOWWATER_BIT,
                        pdFALSE,  // Do NOT clear the bits before returning
                        pdTRUE,   // Wait for ALL bits to be set
                        portMAX_DELAY);
    sensormgr_free_space();
    ESP_LOGD(TAG, "marshalling...");
    root = cJSON_CreateObject();
    cJSON_AddItemToObject(
        root, "metadata",
        metadata = cJSON_CreateObject());  // TODO: how to get device-id and
                                           // friendly name here?
    cJSON_AddItemToObject(root, "data", sensor_array = cJSON_CreateArray());
    for (idx = 0; idx < 5; idx++) {
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
      msg = (mqttmgr_msg_t){
          .msg = json_text,
          .len = strlen(json_text),
          .topic = MQTTMGR_TOPIC_SENSOR,
      };
      // Retry sending to the mqtt queue forever
      do {
        ret = mqttmgr_queuemsg(&msg);
      } while (ret != ESP_OK);
    }
    cJSON_Delete(root);  // Cleanup after all that JSON
    root = sensor_array = sensor_data = metadata = NULL;
    json_text = NULL;
  }
}

// Poll only while able to buffer safely
static void sensormgr_sensorread_task(void *pvParam) {
  time_t timestamp;
  uint8_t idx;
  void *sensor_data_ptr;
  size_t sensor_data_len, free_buf_size;
  esp_err_t ret;
  sensor_reading_t *wrapped_reading;

  ESP_LOGI(TAG, "Starting %s task", SENSORMGR_MEASURETASK_NAME);
  for (;;) {
    xEventGroupWaitBits(mqttmgr_events, SENSORMGR_POLLSENSORS_BIT,
                        pdFALSE,  // Do NOT clear the bits before returning
                        pdTRUE,   // Wait for ALL bits to be set
                        portMAX_DELAY);
    time(&timestamp);
    ESP_LOGD(TAG, "Polling %x sensors", state.sensor_cnt);
    for (idx = 0; idx < state.sensor_cnt; idx++) {
      sensor_data_ptr = NULL;
      sensor_data_len = 0;
      ret = state.sensors[idx].measure(&sensor_data_ptr, &sensor_data_len);
      /*ESP_LOGI(TAG, "Storing %d in ringbuf (free: %d)", sensor_data_len,
       * xRingbufferGetCurFreeSize(state.ring_buffer));*/
      if (ret == ESP_OK) {
        if (pdTRUE != xRingbufferSendAcquire(
                          state.ring_buffer, (void **)&wrapped_reading,
                          sizeof(sensor_reading_t) + sensor_data_len, 0)) {
          ESP_LOGE(TAG, "Error storing measurement in ring buffer");
          // TODO: Okay so I can't store... now what?
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
    ESP_LOGV(TAG, "data writting to ringbuffer");
    free_buf_size = xRingbufferGetCurFreeSize(state.ring_buffer);
    if (free_buf_size < SENSORMGR_RINBUFFER_LOWWATER ||
        state.ring_buffer_item_count > SENSORMGR_RINBUFFER_LOWWATER_ITEM_CNT) {
      xEventGroupSetBits(mqttmgr_events, SENSORMGR_LOWWATER_BIT);
      ESP_LOGI(TAG,
               "low-water bit set: (low: %d, high: %d) %d < %d | %d (item cnt) "
               "> %d (low water mark)",
               SENSORMGR_RINBUFFER_LOWWATER, SENSORMGR_RINBUFFER_HIGHWATER,
               free_buf_size, SENSORMGR_RINBUFFER_LOWWATER,
               state.ring_buffer_item_count,
               SENSORMGR_RINBUFFER_LOWWATER_ITEM_CNT);
    }
    if (free_buf_size < SENSORMGR_RINBUFFER_HIGHWATER) {
      xEventGroupSetBits(mqttmgr_events, SENSORMGR_HIGHWATER_BIT);
      ESP_LOGI(TAG, "high-water bit set: %d < %d", free_buf_size,
               SENSORMGR_RINBUFFER_HIGHWATER);
    }
    vTaskDelay(CONFIG_SENSORMGR_SAMPLE_RATE / portTICK_RATE_MS);
  }
}

// Check filebuffers, vfat space remaining, set can buffer flags
esp_err_t sensormgr_init() {
  FILE *f_test;
  char f_name[24];

  state = (state_t){
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

  return ESP_OK;
}

esp_err_t sensormgr_start() {
  if (!state.initilized) {
    ESP_LOGE(TAG, "Start before init!");
    abort();
  }

  if (state.measure_task_handle == NULL &&
      pdPASS != xTaskCreate(sensormgr_sensorread_task,
                            SENSORMGR_MEASURETASK_NAME,
                            SENSORMGR_TASK_STACKSIZE, (void *)1,
                            tskIDLE_PRIORITY, &state.measure_task_handle)) {
    ESP_LOGE(TAG, "Failed creating task sensorread!");
    return ESP_FAIL;
  } else if (state.measure_task_handle != NULL &&
             eTaskGetState(state.measure_task_handle) == eSuspended) {
    vTaskResume(state.measure_task_handle);
  }

  if (state.queue_task_handle == NULL &&
      pdPASS != xTaskCreate(sensormgr_queuesend_task, SENSORMGR_QUEUETASK_NAME,
                            SENSORMGR_TASK_STACKSIZE, (void *)1,
                            tskIDLE_PRIORITY, &state.queue_task_handle)) {
    ESP_LOGE(TAG, "Failed creating task queuesend!");
    return ESP_FAIL;
  } else if (state.queue_task_handle != NULL &&
             eTaskGetState(state.queue_task_handle) == eSuspended) {
    vTaskResume(state.queue_task_handle);
  }

  if (state.filewriter_task_handle == NULL &&
      pdPASS != xTaskCreate(sensormgr_file_writer_task,
                            SENSORMGR_FILEWRITERTASK_NAME,
                            SENSORMGR_TASK_STACKSIZE, (void *)1,
                            tskIDLE_PRIORITY, &state.filewriter_task_handle)) {
    ESP_LOGE(TAG, "Failed creating task filewriter!");
    return ESP_FAIL;
  } else if (state.filewriter_task_handle != NULL &&
             eTaskGetState(state.filewriter_task_handle) == eSuspended) {
    vTaskResume(state.filewriter_task_handle);
  }

  ESP_LOGI(TAG, "started!");
  return ESP_OK;
}

esp_err_t sensormgr_stop() {
  vTaskSuspend(state.measure_task_handle);
  vTaskSuspend(state.queue_task_handle);
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
