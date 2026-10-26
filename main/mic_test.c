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

// DC-block filter state for Left and Right channels
static float s_dc_x_l = 0.0f, s_dc_y_l = 0.0f;
static float s_dc_x_r = 0.0f, s_dc_y_r = 0.0f;

// First-order IIR DC-blocking High-Pass Filter (removes MEMS resting bias voltage)
static inline float dc_block_filter_left(float sample) {
    const float R = 0.995f;
    float y = sample - s_dc_x_l + R * s_dc_y_l;
    s_dc_x_l = sample;
    s_dc_y_l = y;
    return y;
}

static inline float dc_block_filter_right(float sample) {
    const float R = 0.995f;
    float y = sample - s_dc_x_r + R * s_dc_y_r;
    s_dc_x_r = sample;
    s_dc_y_r = y;
    return y;
}

static void mic_reader_task(void *pvParameters) {
    const size_t SAMPLES_COUNT = 256;
    int32_t raw_buffer[SAMPLES_COUNT * 2]; // Stereo interleaved
    float smoothed_rms = 0.0f;
    uint32_t step = 0;

    ESP_LOGI(TAG, "INMP441 real-time reader task running with DC-block filter...");

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_handle, raw_buffer, sizeof(raw_buffer), &bytes_read, pdMS_TO_TICKS(100));
        if (ret == ESP_OK && bytes_read > 0) {
            size_t samples_read = bytes_read / (sizeof(int32_t) * 2);
            double sum_sq_l = 0.0;
            double sum_sq_r = 0.0;

            for (size_t i = 0; i < samples_read; i++) {
                // INMP441 sends 24-bit MSB-aligned data in 32-bit slot
                int32_t raw_l = raw_buffer[i * 2] >> 8;
                int32_t raw_r = raw_buffer[i * 2 + 1] >> 8;

                // Remove DC bias from both channels
                float ac_l = dc_block_filter_left((float)raw_l);
                float ac_r = dc_block_filter_right((float)raw_r);

                sum_sq_l += (double)(ac_l * ac_l);
                sum_sq_r += (double)(ac_r * ac_r);
            }

            float rms_l = (samples_read > 0) ? sqrtf((float)(sum_sq_l / samples_read)) : 0.0f;
            float rms_r = (samples_read > 0) ? sqrtf((float)(sum_sq_r / samples_read)) : 0.0f;

            // Automatically select active channel (immune to L/R pin grounding differences)
            float active_rms = (rms_l > rms_r) ? rms_l : rms_r;

            // Apply noise gate to cut ambient room floor noise
            const float NOISE_GATE = 400.0f;
            if (active_rms < NOISE_GATE) {
                active_rms = 0.0f;
            } else {
                active_rms -= NOISE_GATE;
            }

            // Smooth the level: fast attack, smooth decay
            if (active_rms > smoothed_rms) {
                smoothed_rms = smoothed_rms * 0.3f + active_rms * 0.7f;
            } else {
                smoothed_rms = smoothed_rms * 0.88f + active_rms * 0.12f;
            }

            // Map smoothed RMS to 0 - 100% (35,000 RMS is loud speaking/clap level)
            float pct = (smoothed_rms / 35000.0f) * 100.0f;
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            s_current_level = (uint8_t)pct;

            // Dynamic equalizer bars responding to live sound
            step++;
            for (int b = 0; b < 16; b++) {
                float wave = 0.65f + 0.35f * sinf((float)b * 0.8f + (float)step * 0.25f);
                int bar_h = (int)(pct * wave * 0.95f);
                if (pct > 0 && bar_h < 3) bar_h = 3;
                if (bar_h > 95) bar_h = 95;
                s_bars[b] = (uint8_t)bar_h;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // ~50Hz refresh
    }
}

esp_err_t mic_test_init(void) {
    ESP_LOGI(TAG, "Initializing INMP441 on I2S_NUM_1: SCK=%d, WS=%d, SD=%d",
             PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD);

    // 1. Enable internal pull-down on SD pin to prevent floating noise during tri-state
    gpio_config_t sd_conf = {
        .pin_bit_mask = (1ULL << PIN_MIC_SD),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sd_conf);

    // 2. Channel configuration on independent I2S controller (I2S_NUM_1)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S_NUM_1 channel: %s", esp_err_to_name(ret));
        s_mic_ready = false;
        return ret;
    }

    // 3. Standard Philips mode for INMP441 (16kHz, 32-bit slot stereo)
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

    // 4. Enable channel
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S_NUM_1 channel: %s", esp_err_to_name(ret));
        s_mic_ready = false;
        return ret;
    }

    s_mic_ready = true;
    ESP_LOGI(TAG, "INMP441 I2S_NUM_1 driver initialized successfully!");

    // 5. Start background reading task
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
