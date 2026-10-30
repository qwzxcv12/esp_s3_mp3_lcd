#ifndef LED_EFFECTS_H
#define LED_EFFECTS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define PIN_LED_STRIP      4   // WS2812B Data In GPIO
#define LED_STRIP_COUNT   30   // Default number of addressable LEDs

esp_err_t led_effects_init(void);
void led_effects_clear(void);

// Visual FX for All Modes
void led_effects_mode_switch(uint8_t mode_index);
void led_effects_note(uint8_t note_idx, uint8_t total_notes, uint8_t r, uint8_t g, uint8_t b);
void led_effects_beat(void);
void led_effects_spark(void);
void led_effects_color_burst(uint8_t r, uint8_t g, uint8_t b);
void led_effects_matrix(void);
void led_effects_police(bool toggle);
void led_effects_idle_step(void);
void led_effects_count(uint8_t count, uint8_t r, uint8_t g, uint8_t b);

#endif // LED_EFFECTS_H
