#ifndef ST7735_H
#define ST7735_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Screen configuration: 1.8" TFT 160x128 (Landscape)
#define ST7735_WIDTH   160
#define ST7735_HEIGHT  128

// User's specific pin connections
#define PIN_LCD_SCL    7
#define PIN_LCD_SDA    8
#define PIN_LCD_RST    9
#define PIN_LCD_DC     10
#define PIN_LCD_CS     11
#define PIN_LCD_BLK    12

// Color definitions (RGB565 format)
#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_BG_DARK     0x0844  // Modern dark navy #0F0F23
#define COLOR_HEADER      0x18F9  // Deep indigo #1E1B4B
#define COLOR_CARD        0x10E5  // Card background #16192E
#define COLOR_CARD_BORDER 0x296B  // Card border #272C4E
#define COLOR_GREEN_OK    0x262A  // Emerald Green #22C55E
#define COLOR_INDIGO      0x633E  // Accent Indigo #6366F1
#define COLOR_SLATE       0x9536  // Muted Slate #94A3B8
#define COLOR_RED         0xF9E7  // Destructive Red #EF4444
#define COLOR_CYAN        0x07FF  // Vibrant Cyan
#define COLOR_AMBER       0xFDC0  // Amber gold

// Convert standard 8-bit RGB to 16-bit RGB565
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Function declarations
esp_err_t st7735_init(void);
void st7735_fill_screen(uint16_t color);
void st7735_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void st7735_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void st7735_draw_fast_h_line(int16_t x, int16_t y, int16_t w, uint16_t color);
void st7735_draw_fast_v_line(int16_t x, int16_t y, int16_t h, uint16_t color);
void st7735_draw_char(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t size);
void st7735_draw_string(int16_t x, int16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size);
void st7735_draw_marquee(int16_t y, int16_t h, const char *text, int16_t scroll_x, uint16_t fg, uint16_t bg);
void st7735_draw_visualizer(int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *levels, uint8_t num_bars);

#endif // ST7735_H
