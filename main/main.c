#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "st7735.h"
#include "audio_test.h"

static const char *TAG = "MAIN_APP";

#define NUM_VIZ_BARS 14

void app_main(void) {
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "  ESP32-S3 ZERO HARDWARE DIAGNOSTIC TEST ");
    ESP_LOGI(TAG, "=========================================");

    // 1. Initialize NVS (non-volatile storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize LCD ST7735S
    ESP_LOGI(TAG, "Initializing ST7735S LCD...");
    bool lcd_ok = (st7735_init() == ESP_OK);

    if (lcd_ok) {
        // Clear screen to modern dark background
        st7735_fill_screen(COLOR_BG_DARK);

        // Render Header Bar (Y: 0 -> 18)
        st7735_fill_rect(0, 0, ST7735_WIDTH, 18, COLOR_HEADER);
        st7735_draw_fast_h_line(0, 18, ST7735_WIDTH, COLOR_INDIGO);
        st7735_draw_string(6, 5, "ESP32-S3 ZERO", COLOR_WHITE, COLOR_HEADER, 1);
        st7735_draw_string(132, 5, "v1.0", COLOR_CYAN, COLOR_HEADER, 1);

        // Hardware Status Card Frame (Y: 22 -> 76)
        st7735_fill_rect(4, 22, 152, 54, COLOR_CARD);
        st7735_draw_rect(4, 22, 152, 54, COLOR_CARD_BORDER);

        // LCD Status
        st7735_draw_string(8, 28, "[OK] LCD ST7735S", COLOR_GREEN_OK, COLOR_CARD, 1);
        st7735_draw_string(114, 28, "160x128", COLOR_SLATE, COLOR_CARD, 1);
    } else {
        ESP_LOGE(TAG, "LCD ST7735S initialization FAILED!");
    }

    // 3. Initialize Audio PCM5102A (I2S)
    ESP_LOGI(TAG, "Initializing PCM5102A I2S DAC...");
    bool audio_ok = (audio_test_init() == ESP_OK);

    if (lcd_ok) {
        if (audio_ok) {
            st7735_draw_string(8, 42, "[OK] I2S PCM5102A", COLOR_GREEN_OK, COLOR_CARD, 1);
            st7735_draw_string(114, 42, "44.1kHz", COLOR_SLATE, COLOR_CARD, 1);
            st7735_draw_string(8, 56, "[OK] HARDWARE READY", COLOR_CYAN, COLOR_CARD, 1);
        } else {
            st7735_draw_string(8, 42, "[ERR] PCM5102A FAIL", COLOR_RED, COLOR_CARD, 1);
            st7735_draw_string(8, 56, "Check I2S Connections", COLOR_AMBER, COLOR_CARD, 1);
        }
    }

    // 4. If both hardware devices are OK -> Play Chime & Start Running Text Animation
    if (lcd_ok && audio_ok) {
        ESP_LOGI(TAG, "Both hardware peripherals are OK! Playing startup chime...");
        audio_play_chime();

        // Banner text for ticker marquee
        const char *marquee_text = " *** ESP32-S3 ZERO + ST7735S + PCM5102A KET NOI OK - HE THONG SAN SANG PHAT NHAC *** ";
        int16_t scroll_x = ST7735_WIDTH;

        uint8_t viz_levels[NUM_VIZ_BARS];
        uint32_t chime_timer = 0;

        ESP_LOGI(TAG, "Entering main animation & marquee loop...");
        while (1) {
            // Draw Running Text Marquee at bottom (Y: 108 -> 126)
            st7735_draw_marquee(108, 18, marquee_text, scroll_x, COLOR_GREEN_OK, COLOR_CARD);
            scroll_x -= 2;

            // Draw animated Equalizer Waveform in middle area (Y: 80 -> 102)
            audio_get_visualizer_data(viz_levels, NUM_VIZ_BARS);
            st7735_draw_visualizer(8, 80, 144, 22, viz_levels, NUM_VIZ_BARS);

            // Replay pleasant melodic chime every 10 seconds to confirm audio output continuously
            chime_timer += 40;
            if (chime_timer >= 10000) {
                chime_timer = 0;
                audio_play_chime();
            }

            vTaskDelay(pdMS_TO_TICKS(40)); // ~25 FPS smooth ticker
        }
    } else {
        ESP_LOGW(TAG, "One or more hardware checks failed. Halting animation.");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
