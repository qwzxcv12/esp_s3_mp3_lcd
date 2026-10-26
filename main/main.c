#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "st7735.h"
#include "audio_test.h"
#include "mic_test.h"

static const char *TAG = "MAIN_APP";
#define NUM_VIZ_BARS 14

static void audio_player_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio task started: playing periodic test chime...");
    while (1) {
        audio_play_chime();
        vTaskDelay(pdMS_TO_TICKS(3000)); // Play chime every 3 seconds
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, " ESP32-S3 LAB: LCD + DAC + MIC + PSRAM   ");
    ESP_LOGI(TAG, "=========================================");

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Query PSRAM and Heap statistics
    size_t psram_size = esp_psram_get_size();
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_free = esp_get_free_heap_size();
    ESP_LOGI(TAG, ">>> PSRAM Total: %u bytes (%u KB)", (unsigned int)psram_size, (unsigned int)(psram_size / 1024));
    ESP_LOGI(TAG, ">>> PSRAM Free : %u bytes (%u KB)", (unsigned int)psram_free, (unsigned int)(psram_free / 1024));
    ESP_LOGI(TAG, ">>> Total Free Heap: %u bytes (%u KB)", (unsigned int)total_free, (unsigned int)(total_free / 1024));

    // 2. Initialize LCD ST7735S (SPI: SCL=7, SDA=8, RST=9, DC=10, CS=11, BLK=12)
    ESP_LOGI(TAG, "Initializing ST7735S LCD...");
    bool lcd_ok = (st7735_init() == ESP_OK);

    if (lcd_ok) {
        // Clear screen to modern dark navy background
        st7735_fill_screen(COLOR_BG_DARK);

        // Header Bar (Y: 0 -> 17)
        st7735_fill_rect(0, 0, ST7735_WIDTH, 17, COLOR_HEADER);
        st7735_draw_fast_h_line(0, 17, ST7735_WIDTH, COLOR_INDIGO);
        st7735_draw_string(6, 5, "ESP32-S3 AUDIO LAB", COLOR_WHITE, COLOR_HEADER, 1);
        st7735_draw_string(132, 5, "v2.1", COLOR_CYAN, COLOR_HEADER, 1);

        // Hardware Status Card Frame (Y: 19 -> 65)
        st7735_fill_rect(4, 19, 152, 46, COLOR_CARD);
        st7735_draw_rect(4, 19, 152, 46, COLOR_CARD_BORDER);
    } else {
        ESP_LOGE(TAG, "LCD ST7735S initialization FAILED!");
    }

    // 3. Initialize DAC PCM5102A on I2S_NUM_0 (LCK=1, DIN=2, BCK=3, SCK=4)
    ESP_LOGI(TAG, "Initializing PCM5102A I2S DAC on I2S_NUM_0...");
    bool audio_ok = (audio_test_init() == ESP_OK);

    if (lcd_ok) {
        // Line 1: LCD and DAC status
        st7735_draw_string(8, 23, "[OK] LCD 160x128", COLOR_GREEN_OK, COLOR_CARD, 1);
        st7735_draw_string(106, 23, "DAC I2S0", audio_ok ? COLOR_CYAN : COLOR_RED, COLOR_CARD, 1);
    }

    // 4. Initialize Mic INMP441 on I2S_NUM_1 (SCK=5, WS=6, SD=13)
    ESP_LOGI(TAG, "Initializing INMP441 I2S Mic on I2S_NUM_1...");
    bool mic_ok = (mic_test_init() == ESP_OK);

    if (lcd_ok) {
        // Line 2: Mic status
        st7735_draw_string(8, 36, "[OK] MIC INMP441", mic_ok ? COLOR_GREEN_OK : COLOR_RED, COLOR_CARD, 1);
        st7735_draw_string(114, 36, "DSP NS", COLOR_CYAN, COLOR_CARD, 1);

        // Line 3: PSRAM status
        if (psram_size > 0) {
            char psram_str[32];
            snprintf(psram_str, sizeof(psram_str), "[OK] PSRAM %uMB", (unsigned int)(psram_size / (1024 * 1024)));
            st7735_draw_string(8, 49, psram_str, COLOR_GREEN_OK, COLOR_CARD, 1);
            char heap_str[16];
            snprintf(heap_str, sizeof(heap_str), "RAM %uM", (unsigned int)(total_free / (1024 * 1024)));
            st7735_draw_string(108, 49, heap_str, COLOR_AMBER, COLOR_CARD, 1);
        } else {
            st7735_draw_string(8, 49, "[--] PSRAM NOT DETECTED", COLOR_SLATE, COLOR_CARD, 1);
        }
    }

    // 5. Start audio playback and real-time mic visualizer
    if (lcd_ok) {
        if (audio_ok) {
            // Play periodic chime in background
            xTaskCreate(audio_player_task, "audio_player", 4096, NULL, 5, NULL);
        }

        // Ticker marquee banner
        const char *marquee_text = " *** ESP32-S3: 2MB PSRAM KICH HOAT THANH CONG - SAN SANG STREAMING LMS 24/7 *** ";
        int16_t scroll_x = ST7735_WIDTH;
        uint8_t viz_levels[NUM_VIZ_BARS];
        char level_str[16];

        ESP_LOGI(TAG, "Entering real-time Mic VU meter and display loop...");
        while (1) {
            // A. Draw VU Meter bar for live Mic volume (Y: 68 -> 78)
            uint8_t mic_lvl = mic_get_level();
            st7735_draw_string(6, 69, "MIC:", COLOR_WHITE, COLOR_BG_DARK, 1);

            // VU Bar frame (X: 32, Y: 68, W: 90, H: 9)
            int16_t bar_max_w = 88;
            int16_t bar_active_w = (mic_lvl * bar_max_w) / 100;
            if (bar_active_w > bar_max_w) bar_active_w = bar_max_w;

            uint16_t bar_color = COLOR_GREEN_OK;
            if (mic_lvl > 50) bar_color = COLOR_AMBER;
            if (mic_lvl > 80) bar_color = COLOR_RED;

            if (bar_active_w > 0) {
                st7735_fill_rect(32, 69, bar_active_w, 7, bar_color);
            }
            if (bar_active_w < bar_max_w) {
                st7735_fill_rect(32 + bar_active_w, 69, bar_max_w - bar_active_w, 7, COLOR_CARD);
            }
            st7735_draw_rect(31, 68, bar_max_w + 2, 9, COLOR_CARD_BORDER);

            // Percentage text (X: 126, Y: 69)
            snprintf(level_str, sizeof(level_str), "%3d%%", mic_lvl);
            st7735_draw_string(126, 69, level_str, (mic_lvl > 50) ? bar_color : COLOR_SLATE, COLOR_BG_DARK, 1);

            // B. Draw Real-time Sound Spectrum Bars from Microphone (Y: 82 -> 103)
            mic_get_visualizer_bars(viz_levels, NUM_VIZ_BARS);
            st7735_draw_visualizer(6, 82, 148, 21, viz_levels, NUM_VIZ_BARS);

            // C. Draw Marquee Ticker at bottom (Y: 108 -> 126)
            st7735_draw_marquee(108, 18, marquee_text, scroll_x, COLOR_GREEN_OK, COLOR_CARD);
            scroll_x -= 2;

            vTaskDelay(pdMS_TO_TICKS(40)); // 25 FPS responsive loop
        }
    }
}
