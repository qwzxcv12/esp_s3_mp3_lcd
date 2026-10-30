#include "led_effects.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "LED_FX";

static led_strip_handle_t s_led_strip = NULL;
static SemaphoreHandle_t s_led_mutex = NULL;
static uint32_t s_idle_step = 0;

esp_err_t led_effects_init(void) {
    ESP_LOGI(TAG, "Initializing WS2812B LED strip on GPIO %d (%d LEDs)", PIN_LED_STRIP, LED_STRIP_COUNT);

    s_led_mutex = xSemaphoreCreateMutex();

    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_LED_STRIP,
        .max_leds = LED_STRIP_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT device for LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    led_strip_clear(s_led_strip);
    led_effects_mode_switch(0); // Startup ripple
    ESP_LOGI(TAG, "WS2812B RMT driver initialized successfully!");
    return ESP_OK;
}

void led_effects_clear(void) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    led_strip_clear(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Convert HSV (0-360, 0-100, 0-100) to RGB
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    float hf = (float)h / 60.0f;
    float sf = (float)s / 100.0f;
    float vf = (float)v / 100.0f;
    int i = (int)hf;
    float f = hf - (float)i;
    float p = vf * (1.0f - sf);
    float q = vf * (1.0f - sf * f);
    float t = vf * (1.0f - sf * (1.0f - f));

    float rf = 0, gf = 0, bf = 0;
    switch (i % 6) {
        case 0: rf = vf; gf = t;  bf = p;  break;
        case 1: rf = q;  gf = vf; bf = p;  break;
        case 2: rf = p;  gf = vf; bf = t;  break;
        case 3: rf = p;  gf = q;  bf = vf; break;
        case 4: rf = t;  gf = p;  bf = vf; break;
        case 5: rf = vf; gf = p;  bf = q;  break;
    }
    *r = (uint8_t)(rf * 255.0f);
    *g = (uint8_t)(gf * 255.0f);
    *b = (uint8_t)(bf * 255.0f);
}

// Mode switch wave animation
void led_effects_mode_switch(uint8_t mode_index) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    uint16_t base_hue = (mode_index * 40) % 360;
    for (int step = 0; step < LED_STRIP_COUNT + 5; step++) {
        for (int i = 0; i < LED_STRIP_COUNT; i++) {
            if (i <= step && (step - i) < 6) {
                uint8_t r, g, b;
                hsv_to_rgb((base_hue + i * 8) % 360, 100, 80, &r, &g, &b);
                led_strip_set_pixel(s_led_strip, i, r, g, b);
            } else {
                led_strip_set_pixel(s_led_strip, i, 0, 0, 0);
            }
        }
        led_strip_refresh(s_led_strip);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    led_strip_clear(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Note hit visual: center around note_idx on the strip
void led_effects_note(uint8_t note_idx, uint8_t total_notes, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    int center = (note_idx * (LED_STRIP_COUNT - 1)) / (total_notes > 0 ? total_notes : 1);
    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        int dist = abs(i - center);
        if (dist == 0) {
            led_strip_set_pixel(s_led_strip, i, r, g, b);
        } else if (dist == 1) {
            led_strip_set_pixel(s_led_strip, i, r / 3, g / 3, b / 3);
        } else {
            led_strip_set_pixel(s_led_strip, i, 0, 0, 0);
        }
    }
    led_strip_refresh(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Drum beat strobe
void led_effects_beat(void) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        led_strip_set_pixel(s_led_strip, i, 180, 20, 20); // Bright Red strobe
    }
    led_strip_refresh(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Typewriter mechanical spark
void led_effects_spark(void) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    int pos = rand() % LED_STRIP_COUNT;
    led_strip_clear(s_led_strip);
    led_strip_set_pixel(s_led_strip, pos, 255, 220, 100); // Warm yellow-white spark
    if (pos > 0) led_strip_set_pixel(s_led_strip, pos - 1, 80, 60, 20);
    if (pos < LED_STRIP_COUNT - 1) led_strip_set_pixel(s_led_strip, pos + 1, 80, 60, 20);
    led_strip_refresh(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Full strip color burst
void led_effects_color_burst(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        led_strip_set_pixel(s_led_strip, i, r, g, b);
    }
    led_strip_refresh(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Matrix green trail for Morse / Robot
void led_effects_matrix(void) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    int pos = rand() % LED_STRIP_COUNT;
    led_strip_set_pixel(s_led_strip, pos, 0, 255, 40); // Bright Matrix Green
    led_strip_refresh(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Cockpit police lights: left red, right blue alternating
void led_effects_police(bool toggle) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    int half = LED_STRIP_COUNT / 2;
    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        if (toggle) {
            if (i < half) led_strip_set_pixel(s_led_strip, i, 255, 0, 0); // Red
            else led_strip_set_pixel(s_led_strip, i, 0, 0, 255);          // Blue
        } else {
            if (i < half) led_strip_set_pixel(s_led_strip, i, 0, 0, 255); // Blue
            else led_strip_set_pixel(s_led_strip, i, 255, 0, 0);          // Red
        }
    }
    led_strip_refresh(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Gentle breathing animation when idle
void led_effects_idle_step(void) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    s_idle_step++;
    float rad = (float)s_idle_step * 0.05f;
    float brightness = (sinf(rad) + 1.0f) * 0.5f * 35.0f + 5.0f; // 5% to 40%

    uint8_t r, g, b;
    hsv_to_rgb((s_idle_step / 2) % 360, 80, (uint8_t)brightness, &r, &g, &b);
    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        led_strip_set_pixel(s_led_strip, i, r, g, b);
    }
    led_strip_refresh(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

// Light up exactly N LEDs for counting
void led_effects_count(uint8_t count, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_led_strip || !s_led_mutex) return;
    xSemaphoreTake(s_led_mutex, portMAX_DELAY);

    led_strip_clear(s_led_strip);
    if (count > LED_STRIP_COUNT) count = LED_STRIP_COUNT;

    for (int i = 0; i < count; i++) {
        led_strip_set_pixel(s_led_strip, i, r, g, b);
    }
    led_strip_refresh(s_led_strip);
    xSemaphoreGive(s_led_mutex);
}

