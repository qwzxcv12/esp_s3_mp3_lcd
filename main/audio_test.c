#include "audio_test.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "AUDIO_I2S";
static i2s_chan_handle_t tx_handle = NULL;
static bool s_audio_ready = false;

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

esp_err_t audio_test_init(void) {
    ESP_LOGI(TAG, "Initializing PCM5102A I2S on LCK=%d, DIN=%d, BCK=%d, SCK=%d...",
             PIN_I2S_LCK, PIN_I2S_DIN, PIN_I2S_BCK, PIN_I2S_SCK);

    // 1. Channel configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S channel: %s", esp_err_to_name(ret));
        s_audio_ready = false;
        return ret;
    }

    // 2. Standard mode configuration (44.1kHz, 16-bit Stereo)
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)PIN_I2S_SCK,
            .bclk = (gpio_num_t)PIN_I2S_BCK,
            .ws   = (gpio_num_t)PIN_I2S_LCK,
            .dout = (gpio_num_t)PIN_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S standard mode: %s", esp_err_to_name(ret));
        s_audio_ready = false;
        return ret;
    }

    // 3. Enable channel
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        s_audio_ready = false;
        return ret;
    }

    s_audio_ready = true;
    ESP_LOGI(TAG, "PCM5102A I2S driver initialized successfully!");
    return ESP_OK;
}

bool audio_is_ready(void) {
    return s_audio_ready;
}

// Generate and play a note with smooth attack and decay envelope
static void play_tone(float freq_hz, uint32_t duration_ms, float max_amplitude) {
    if (!s_audio_ready || tx_handle == NULL) return;

    size_t total_samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    const size_t CHUNK_SIZE = 256;
    int16_t buffer[CHUNK_SIZE * 2]; // Stereo: Left & Right

    size_t samples_generated = 0;
    while (samples_generated < total_samples) {
        size_t chunk = (total_samples - samples_generated > CHUNK_SIZE) ? CHUNK_SIZE : (total_samples - samples_generated);
        for (size_t i = 0; i < chunk; i++) {
            size_t sample_idx = samples_generated + i;
            float t = (float)sample_idx / AUDIO_SAMPLE_RATE;

            // Envelope: 20ms attack, exponential decay
            float envelope = 1.0f;
            float time_ms = (float)sample_idx * 1000.0f / AUDIO_SAMPLE_RATE;
            if (time_ms < 20.0f) {
                envelope = time_ms / 20.0f;
            } else {
                envelope = expf(-(time_ms - 20.0f) / (duration_ms * 0.45f));
            }

            float sample_val = sinf(2.0f * M_PI * freq_hz * t) * envelope * max_amplitude * 30000.0f;
            int16_t pcm = (int16_t)sample_val;

            buffer[i * 2]     = pcm; // Left channel
            buffer[i * 2 + 1] = pcm; // Right channel
        }

        size_t bytes_written = 0;
        i2s_channel_write(tx_handle, buffer, chunk * sizeof(int16_t) * 2, &bytes_written, pdMS_TO_TICKS(100));
        samples_generated += chunk;
    }
}

// Play startup melodic chime (C5 -> E5 -> G5 -> C6)
void audio_play_chime(void) {
    if (!s_audio_ready) return;
    ESP_LOGI(TAG, "Playing startup sound chime...");
    play_tone(523.25f, 130, 0.6f); // C5
    play_tone(659.25f, 130, 0.65f); // E5
    play_tone(783.99f, 130, 0.7f); // G5
    play_tone(1046.50f, 350, 0.85f); // C6
}

// Provide animated visualizer levels (simulated equalizer)
void audio_get_visualizer_data(uint8_t *levels, uint8_t count) {
    static uint32_t step = 0;
    step++;
    for (uint8_t i = 0; i < count; i++) {
        float wave = sinf((float)step * 0.25f + (float)i * 0.8f);
        float wave2 = cosf((float)step * 0.15f - (float)i * 0.5f);
        int val = (int)((wave + wave2 + 2.0f) * 23.0f);
        if (val < 5) val = 5;
        if (val > 95) val = 95;
        levels[i] = (uint8_t)val;
    }
}
