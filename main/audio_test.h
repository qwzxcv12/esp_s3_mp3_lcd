#ifndef AUDIO_TEST_H
#define AUDIO_TEST_H

#include <stdbool.h>
#include "esp_err.h"

// User's audio pinout for PCM5102A on ESP32-S3-Zero
#define PIN_I2S_LCK    1  // Word Select / LRCK
#define PIN_I2S_DIN    2  // Serial Data / DOUT
#define PIN_I2S_BCK    3  // Bit Clock / BCLK
#define PIN_I2S_SCK    4  // Master Clock / SCK (Pulled to 0V/GND)

#define AUDIO_SAMPLE_RATE 44100

esp_err_t audio_test_init(void);
bool audio_is_ready(void);
void audio_play_chime(void);
void audio_get_visualizer_data(uint8_t *levels, uint8_t count);

#endif // AUDIO_TEST_H
