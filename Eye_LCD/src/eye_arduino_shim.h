// Minimal Arduino-compat layer so the original eye code compiles unchanged.
// Include this AFTER all system headers (it defines a `random` macro).
#pragma once
#include <stdint.h>
#include "esp_timer.h"
#include "esp_random.h"

#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t  *)(addr))
#define pgm_read_word(addr) (*(const uint16_t *)(addr))

static inline uint32_t micros(void) { return (uint32_t)esp_timer_get_time(); }
static inline uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Arduino semantics: random(max) -> [0,max), random(min,max) -> [min,max)
static inline long random1(long max)           { return max > 0 ? (long)(esp_random() % (uint32_t)max) : 0; }
static inline long random2(long min, long max) { return min + random1(max - min); }
#define PICK_RANDOM(_1, _2, NAME, ...) NAME
#define random(...) PICK_RANDOM(__VA_ARGS__, random2, random1, dummy)(__VA_ARGS__)

static inline long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// Empty on purpose: the blocking semaphore in lcd_push_rect() already lets
// other tasks (and the idle task / watchdog) run.
static inline void yield(void) {}
