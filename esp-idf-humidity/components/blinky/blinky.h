#ifndef BLINKY_H
#define BLINKY_H

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <modules/blinky.pb-c.h>
#include <sdkconfig.h>
#include <stdbool.h>

// {speed, pattern, repeat, rgb/led} with speed 0 meaning constant pattern[0]?
typedef struct _blinky_animation_t {
  uint32_t ms_delay;      // Delay between transitions in the pattern
  uint32_t repeat_count;  // 0xFF for infinite repeat, 0 for run once
  uint32_t brgb;
  Blinky__BlinkyLedT target;
  Blinky__BlinkyPatternT pattern;
  bool off_at_end;
} blinky_animation_t;

esp_err_t blinky_init();
void blinky_play(blinky_animation_t *animation);

#endif
