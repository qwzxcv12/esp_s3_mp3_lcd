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

// Bandpass filter state (HPF 250Hz, LPF 3500Hz for human speech)
static float s_hpf_x_l = 0.0f, s_hpf_y_l = 0.0f;
static float s_lpf_y_l = 0.0f;
static float s_hpf_x_r = 0.0f, s_hpf_y_r = 0.0f;
static float s_lpf_y_r = 0.0f;

// 1. DC-blocking High-Pass Filter (removes MEMS resting bias voltage)
static inline float dc_block_filter(float sample, float *prev_x, float *prev_y) {
    const float R = 0.995f;
    float y = sample - *prev_x + R * (*prev_y);
    *prev_x = sample;
    *prev_y = y;
    return y;
}

// 2. High-pass filter at ~250Hz (cuts wind, fan hum, 50Hz mains hum, table rumble)
static inline float hpf_voice(float sample, float *prev_x, float *prev_y) {
    const float alpha_hp = 0.9106f; // fc ~ 250Hz at Fs=16kHz
    float y = alpha_hp * (*prev_y + sample - *prev_x);
    *prev_x = sample;
    *prev_y = y;
    return y;
}

// 3. Low-pass filter at ~3500Hz (cuts high-frequency hiss, switching noise)
static inline float lpf_voice(float sample, float *prev_y) {
    const float alpha_lp = 0.5788f; // fc ~ 3500Hz at Fs=16kHz
    float y = *prev_y + alpha_lp * (sample - *prev_y);
    *prev_y = y;
    return y;
}

// Complete Speech Bandpass Filter pipeline
static inline float apply_speech_filter(float raw, float *dc_x, float *dc_y, float *hp_x, float *hp_y, float *lp_y) {
    float no_dc = dc_block_filter(raw, dc_x, dc_y);
    float no_rumble = hpf_voice(no_dc, hp_x, hp_y);
    float clean_speech = lpf_voice(no_rumble, lp_y);
    return clean_speech;
}

static void mic_reader_task(void *pvParameters) {
    static int32_t raw_buffer[256 * 2]; // Static allocation to protect task stack
    float smoothed_rms = 0.0f;
    float adaptive_noise_floor = 300.0f; // Continuously tracks ambient room noise
    uint32_t step = 0;

    ESP_LOGI(TAG, "INMP441 DSP Noise Suppression & Bandpass Filter active (250Hz - 3500Hz)...");

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

                // Pass both channels through Speech Bandpass DSP filter
                float clean_l = apply_speech_filter((float)raw_l, &s_dc_x_l, &s_dc_y_l, &s_hpf_x_l, &s_hpf_y_l, &s_lpf_y_l);
                float clean_r = apply_speech_filter((float)raw_r, &s_dc_x_r, &s_dc_y_r, &s_hpf_x_r, &s_hpf_y_r, &s_lpf_y_r);

                sum_sq_l += (double)(clean_l * clean_l);
                sum_sq_r += (double)(clean_r * clean_r);
            }

            float rms_l = (samples_read > 0) ? sqrtf((float)(sum_sq_l / samples_read)) : 0.0f;
            float rms_r = (samples_read > 0) ? sqrtf((float)(sum_sq_r / samples_read)) : 0.0f;

            // Automatically select active microphone channel
            float active_rms = (rms_l > rms_r) ? rms_l : rms_r;

            // --- Adaptive Noise Floor Tracking ---
            // If sound is quiet, track the room noise floor quickly
            if (active_rms < adaptive_noise_floor) {
                adaptive_noise_floor = adaptive_noise_floor * 0.90f + active_rms * 0.10f;
            } else {
                // When speaking, adapt up very slowly so human speech does not inflate noise floor
                adaptive_noise_floor = adaptive_noise_floor * 0.998f + active_rms * 0.002f;
            }

            // Ensure baseline minimum floor
            if (adaptive_noise_floor < 150.0f) adaptive_noise_floor = 150.0f;

            // --- Soft Dynamic Expander (Spectral Noise Reduction) ---
            // Threshold = 1.35x ambient noise floor
            float noise_threshold = adaptive_noise_floor * 1.35f;
            float voice_signal = 0.0f;
            if (active_rms > noise_threshold) {
                // Significant human voice activity above background noise!
                voice_signal = active_rms - noise_threshold;
            } else {
                // Sub-threshold ambient noise: fully suppressed
                voice_signal = 0.0f;
            }

            // Smooth the level envelope: immediate attack, natural speech decay
            if (voice_signal > smoothed_rms) {
                smoothed_rms = smoothed_rms * 0.20f + voice_signal * 0.80f;
            } else {
                smoothed_rms = smoothed_rms * 0.85f + voice_signal * 0.15f;
            }

            // Scale to 0 - 100% (25,000 counts = standard speaking level)
            float pct = (smoothed_rms / 25000.0f) * 100.0f;
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            s_current_level = (uint8_t)pct;

            // Dynamic equalizer bars responding only to human voice
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

    // 5. Start background reading task with generous stack (core 0, audio owns core 1)
    xTaskCreatePinnedToCore(mic_reader_task, "mic_reader", 6144, NULL, 4, NULL, 0);

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
