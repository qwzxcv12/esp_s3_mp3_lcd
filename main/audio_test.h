#ifndef AUDIO_TEST_H
#define AUDIO_TEST_H

#include <stdbool.h>
#include "esp_err.h"

// User's audio pinout for PCM5102A on ESP32-S3-Zero
#define PIN_I2S_LCK    2  // Word Select / LRCK
#define PIN_I2S_DIN    3  // Serial Data / DOUT
#define PIN_I2S_BCK    4  // Bit Clock / BCLK
#define PIN_I2S_SCK    5  // Master Clock / MCLK

#define AUDIO_SAMPLE_RATE 44100

esp_err_t audio_test_init(void);
bool audio_is_ready(void);
void audio_play_chime(void);
void audio_get_visualizer_data(uint8_t *levels, uint8_t count);

#endif // AUDIO_TEST_H
