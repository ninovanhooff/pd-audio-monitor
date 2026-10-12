#pragma once

#include <stdint.h>

#define RING_SIZE 32768

// Buffer arithmetic at 44.1 kHz: 441 samples per 10 ms.
#define SAMPLES_PER_10MS 441
#define PREFILL_MIN_MS 50
#define PREFILL_STEP_MS 10
#define PREFILL_OPTIONS 26
#define PREFILL_DEFAULT_OPTION 10 // 150 ms
// Beyond prefill + this, the host's clock has run ahead, so playback drops back to the prefill.
#define OVERRUN_MARGIN_SAMPLES 11025 // 250 ms
// The replay keeps the buffer this far above the prefill so that it always primes.
#define REPLAY_MARGIN_SAMPLES 2205 // 50 ms

_Static_assert((PREFILL_MIN_MS / PREFILL_STEP_MS + PREFILL_OPTIONS - 1) * SAMPLES_PER_10MS + OVERRUN_MARGIN_SAMPLES < RING_SIZE,
               "the largest prefill's overrun threshold must fit in the ring");

#define METER_FLOOR_DB (-60.0f)
#define METER_FALL_DB_PER_SECOND 20.0f

static inline uint32_t prefill_samples(int option) {
    return (uint32_t)(PREFILL_MIN_MS / PREFILL_STEP_MS + option) * SAMPLES_PER_10MS;
}

static inline uint32_t max_fill_samples(uint32_t prefill) {
    return prefill + OVERRUN_MARGIN_SAMPLES;
}

static inline uint32_t replay_fill_samples(uint32_t prefill) {
    return prefill + REPLAY_MARGIN_SAMPLES;
}

static inline int samples_to_ms(uint32_t samples) {
    return (int)(samples * 10 / SAMPLES_PER_10MS);
}

// Rises instantly, falls METER_FALL_DB_PER_SECOND, never below the floor.
static inline float meter_next_db(float meterDb, float levelDb, unsigned int elapsedMs) {
    float next = levelDb > meterDb ? levelDb : meterDb - METER_FALL_DB_PER_SECOND * (float)elapsedMs / 1000.0f;
    return next < METER_FLOOR_DB ? METER_FLOOR_DB : next;
}
