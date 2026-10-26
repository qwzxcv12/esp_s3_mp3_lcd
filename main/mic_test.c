#include "mic_test.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "MIC_INMP441";
static i2s_chan_handle_t rx_handle = NULL;
static bool s_mic_ready = false;
static uint8_t s_current_level = 0;
static uint8_t s_bars[16] = {0};

static void mic_reader_task(void *pvParameters) {
    const size_t SAMPLES_COUNT = 256;
    int32_t raw_buffer[SAMPLES_COUNT * 2]; // Stereo (Left/Right)
    float smoothed_rms = 0.0f;

    ESP_LOGI(TAG, "INMP441 reader task running...");

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_handle, raw_buffer, sizeof(raw_buffer), &bytes_read, pdMS_TO_TICKS(100));
        if (ret == ESP_OK && bytes_read > 0) {
            size_t samples_read = bytes_read / (sizeof(int32_t) * 2);
            int64_t sum_squares = 0;
            int32_t peak = 0;

            for (size_t i = 0; i < samples_read; i++) {
                // INMP441 with L/R tied to GND sends data on Left channel (even index)
                int32_t val = raw_buffer[i * 2] >> 14; // Normalize from 24/32-bit
                int32_t abs_val = (val < 0) ? -val : val;
                if (abs_val > peak) peak = abs_val;
                sum_squares += (int64_t)val * val;
            }

            float rms = 0.0f;
            if (samples_read > 0) {
                rms = sqrtf((float)sum_squares / (float)samples_read);
            }

            // Smooth the level (quick attack, smooth decay)
            if (rms > smoothed_rms) {
                smoothed_rms = smoothed_rms * 0.4f + rms * 0.6f;
            } else {
                smoothed_rms = smoothed_rms * 0.85f + rms * 0.15f;
            }

            // Map RMS to 0 - 100%
            int level_pct = (int)(smoothed_rms * 2.2f);
            if (level_pct < 0) level_pct = 0;
            if (level_pct > 100) level_pct = 100;
            s_current_level = (uint8_t)level_pct;

            // Generate responsive visualizer bars based on live sound energy
            for (int b = 0; b < 16; b++) {
                float variation = 0.7f + 0.6f * sinf((float)b * 0.8f + (float)peak * 0.05f);
                int bar_val = (int)(smoothed_rms * 2.0f * variation);
                if (bar_val < 3) bar_val = 3;
                if (bar_val > 95) bar_val = 95;
                s_bars[b] = (uint8_t)bar_val;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // ~50Hz sampling rate
    }
}

esp_err_t mic_test_init(void) {
    ESP_LOGI(TAG, "Initializing INMP441 on I2S_NUM_1: SCK=%d, WS=%d, SD=%d",
             PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD);

    // 1. Channel configuration on independent I2S controller (I2S_NUM_1)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S_NUM_1 channel: %s", esp_err_to_name(ret));
        s_mic_ready = false;
        return ret;
    }

    // 2. Standard Philips mode for INMP441 (16kHz, 32-bit slot stereo, left channel active)
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_MIC_SCK,
            .ws   = (gpio_num_t)PIN_MIC_WS,
            .din  = (gpio_num_t)PIN_MIC_SD,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S_NUM_1 standard mode: %s", esp_err_to_name(ret));
        s_mic_ready = false;
        return ret;
    }

    // 3. Enable channel
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S_NUM_1 channel: %s", esp_err_to_name(ret));
        s_mic_ready = false;
        return ret;
    }

    s_mic_ready = true;
    ESP_LOGI(TAG, "INMP441 I2S_NUM_1 driver initialized successfully!");

    // 4. Start background reading task
    xTaskCreate(mic_reader_task, "mic_reader", 4096, NULL, 4, NULL);

    return ESP_OK;
}

bool mic_is_ready(void) {
    return s_mic_ready;
}

uint8_t mic_get_level(void) {
    return s_current_level;
}

void mic_get_visualizer_bars(uint8_t *bars, uint8_t count) {
    for (uint8_t i = 0; i < count && i < 16; i++) {
        bars[i] = s_bars[i];
    }
}
