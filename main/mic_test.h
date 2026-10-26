#ifndef MIC_TEST_H
#define MIC_TEST_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// User's pinout for INMP441 MEMS microphone on ESP32-S3-Zero
#define PIN_MIC_SCK    5   // Serial Clock (BCLK)
#define PIN_MIC_WS     6   // Word Select (LRCK)
#define PIN_MIC_SD     13  // Serial Data (DIN)

#define MIC_SAMPLE_RATE 16000

esp_err_t mic_test_init(void);
bool mic_is_ready(void);
uint8_t mic_get_level(void);
void mic_get_visualizer_bars(uint8_t *bars, uint8_t count);

#endif // MIC_TEST_H
