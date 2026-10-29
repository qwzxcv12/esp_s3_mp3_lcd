#include "st7735.h"
#include "font5x7.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ST7735";

// ST7735 Command codes
#define ST7735_NOP     0x00
#define ST7735_SWRESET 0x01
#define ST7735_RDDID   0x04
#define ST7735_SLPIN   0x10
#define ST7735_SLPOUT  0x11
#define ST7735_PTLON   0x12
#define ST7735_NORON   0x13
#define ST7735_INVOFF  0x20
#define ST7735_INVON   0x21
#define ST7735_DISPOFF 0x28
#define ST7735_DISPON  0x29
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_MADCTL  0x36
#define ST7735_COLMOD  0x3A

#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR  0xB4
#define ST7735_PWCTR1  0xC0
#define ST7735_PWCTR2  0xC1
#define ST7735_PWCTR3  0xC2
#define ST7735_PWCTR4  0xC3
#define ST7735_PWCTR5  0xC4
#define ST7735_VMCTR1  0xC5
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1

// Standard 1.8" 160x128 landscape offset
#define ST7735_COL_OFFSET 0
#define ST7735_ROW_OFFSET 0

static spi_device_handle_t spi_handle = NULL;

static void lcd_write_cmd(uint8_t cmd) {
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(spi_handle, &t);
}

static void lcd_write_data(const uint8_t *data, size_t len) {
    if (len == 0) return;
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(spi_handle, &t);
}

static void lcd_write_byte(uint8_t data) {
    lcd_write_data(&data, 1);
}

static void st7735_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += ST7735_COL_OFFSET;
    x1 += ST7735_COL_OFFSET;
    y0 += ST7735_ROW_OFFSET;
    y1 += ST7735_ROW_OFFSET;

    uint8_t col_data[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t row_data[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };

    lcd_write_cmd(ST7735_CASET);
    lcd_write_data(col_data, 4);

    lcd_write_cmd(ST7735_RASET);
    lcd_write_data(row_data, 4);

    lcd_write_cmd(ST7735_RAMWR);
}

esp_err_t st7735_init(void) {
    ESP_LOGI(TAG, "Initializing ST7735 SPI bus and pins...");

    // 1. Configure control GPIOs (DC, RST, BLK)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_RST) | (1ULL << PIN_LCD_BLK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Turn off backlight during init
    gpio_set_level(PIN_LCD_BLK, 0);

    // 2. Configure SPI Bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LCD_SDA,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCL,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST7735_WIDTH * 2 * 16,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Add LCD device to SPI bus (27MHz clock speed)
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 27 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 7,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    // 4. Hardware Reset
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // 5. Send ST7735S initialization sequence
    lcd_write_cmd(ST7735_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_write_cmd(ST7735_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(200));

    // Frame rate control in normal mode
    lcd_write_cmd(ST7735_FRMCTR1);
    lcd_write_byte(0x01);
    lcd_write_byte(0x2C);
    lcd_write_byte(0x2D);

    // Frame rate control in idle mode
    lcd_write_cmd(ST7735_FRMCTR2);
    lcd_write_byte(0x01);
    lcd_write_byte(0x2C);
    lcd_write_byte(0x2D);

    // Frame rate control in partial mode
    lcd_write_cmd(ST7735_FRMCTR3);
    lcd_write_byte(0x01);
    lcd_write_byte(0x2C);
    lcd_write_byte(0x2D);
    lcd_write_byte(0x01);
    lcd_write_byte(0x2C);
    lcd_write_byte(0x2D);

    // Display inversion control
    lcd_write_cmd(ST7735_INVCTR);
    lcd_write_byte(0x07);

    // Power control 1
    lcd_write_cmd(ST7735_PWCTR1);
    lcd_write_byte(0xA2);
    lcd_write_byte(0x02);
    lcd_write_byte(0x84);

    // Power control 2
    lcd_write_cmd(ST7735_PWCTR2);
    lcd_write_byte(0xC5);

    // Power control 3 (normal mode)
    lcd_write_cmd(ST7735_PWCTR3);
    lcd_write_byte(0x0A);
    lcd_write_byte(0x00);

    // Power control 4 (idle mode)
    lcd_write_cmd(ST7735_PWCTR4);
    lcd_write_byte(0x8A);
    lcd_write_byte(0x2A);

    // Power control 5 (partial mode)
    lcd_write_cmd(ST7735_PWCTR5);
    lcd_write_byte(0x8A);
    lcd_write_byte(0xEE);

    // VCOM control 1
    lcd_write_cmd(ST7735_VMCTR1);
    lcd_write_byte(0x0E);

    // Display Inversion Off
    lcd_write_cmd(ST7735_INVOFF);

    // Memory access control (Landscape 160x128, RGB format)
    lcd_write_cmd(ST7735_MADCTL);
    lcd_write_byte(0xA0); // MY=1, MX=0, MV=1 (Landscape 160x128)

    // Interface pixel format: 16-bit / pixel (RGB565)
    lcd_write_cmd(ST7735_COLMOD);
    lcd_write_byte(0x05);

    // Positive gamma control
    lcd_write_cmd(ST7735_GMCTRP1);
    uint8_t gamma_p[16] = {0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
    lcd_write_data(gamma_p, 16);

    // Negative gamma control
    lcd_write_cmd(ST7735_GMCTRN1);
    uint8_t gamma_n[16] = {0x03,0x1d,0x07,0x06,0x2e,0x2c,0x29,0x2d,0x2e,0x2e,0x37,0x3f,0x00,0x00,0x02,0x10};
    lcd_write_data(gamma_n, 16);

    // Normal display on
    lcd_write_cmd(ST7735_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Main screen turn on
    lcd_write_cmd(ST7735_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Turn on backlight LED
    gpio_set_level(PIN_LCD_BLK, 1);

    ESP_LOGI(TAG, "ST7735 initialized successfully!");
    return ESP_OK;
}

void st7735_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    // Clip negative coordinates (avoids garbage windows from uint16 wrap-around)
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= ST7735_WIDTH || y >= ST7735_HEIGHT || w <= 0 || h <= 0) return;
    if (x + w > ST7735_WIDTH) w = ST7735_WIDTH - x;
    if (y + h > ST7735_HEIGHT) h = ST7735_HEIGHT - y;

    st7735_set_window(x, y, x + w - 1, y + h - 1);

    // Use a line buffer for high speed DMA transfer
    size_t batch_size = w;
    if (batch_size > 160) batch_size = 160;
    uint8_t line_buf[160 * 2];
    for (size_t i = 0; i < batch_size; i++) {
        line_buf[i * 2]     = (uint8_t)(color >> 8);
        line_buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }

    gpio_set_level(PIN_LCD_DC, 1);
    for (int line = 0; line < h; line++) {
        spi_transaction_t t = {
            .length = batch_size * 16,
            .tx_buffer = line_buf,
        };
        spi_device_polling_transmit(spi_handle, &t);
    }
}

void st7735_fill_screen(uint16_t color) {
    st7735_fill_rect(0, 0, ST7735_WIDTH, ST7735_HEIGHT, color);
}

void st7735_draw_fast_h_line(int16_t x, int16_t y, int16_t w, uint16_t color) {
    st7735_fill_rect(x, y, w, 1, color);
}

void st7735_draw_fast_v_line(int16_t x, int16_t y, int16_t h, uint16_t color) {
    st7735_fill_rect(x, y, 1, h, color);
}

void st7735_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    st7735_draw_fast_h_line(x, y, w, color);
    st7735_draw_fast_h_line(x, y + h - 1, w, color);
    st7735_draw_fast_v_line(x, y, h, color);
    st7735_draw_fast_v_line(x + w - 1, y, h, color);
}

void st7735_draw_char(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t size) {
    if (c < 32 || c > 126) c = ' ';
    if (size == 0) size = 1;
    if (size > 4) size = 4;
    uint8_t idx = c - 32;

    // Character cell = 6 columns (5 glyph + 1 spacing) x 7 rows, scaled by size
    const int16_t cell_w = 6 * size;
    const int16_t cell_h = 7 * size;

    // Clip the cell against the screen
    int16_t x0 = (x < 0) ? 0 : x;
    int16_t y0 = (y < 0) ? 0 : y;
    int16_t x1 = x + cell_w; if (x1 > ST7735_WIDTH)  x1 = ST7735_WIDTH;
    int16_t y1 = y + cell_h; if (y1 > ST7735_HEIGHT) y1 = ST7735_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;

    // Render the whole visible cell into one buffer and send it in a single
    // window write (1 window + 1 data transfer instead of ~210 tiny SPI
    // polling transactions per character -> massively lower CPU load).
    static uint8_t buf[(6 * 4) * (7 * 4) * 2];
    size_t n = 0;
    const uint8_t fg_hi = color >> 8, fg_lo = color & 0xFF;
    const uint8_t bg_hi = bg >> 8,    bg_lo = bg & 0xFF;
    for (int16_t py = y0; py < y1; py++) {
        int row = (py - y) / size;
        for (int16_t px = x0; px < x1; px++) {
            int col = (px - x) / size;
            bool on = (col < 5) && (font5x7[idx][col] & (1 << row));
            buf[n++] = on ? fg_hi : bg_hi;
            buf[n++] = on ? fg_lo : bg_lo;
        }
    }

    st7735_set_window(x0, y0, x1 - 1, y1 - 1);
    lcd_write_data(buf, n);
}

void st7735_draw_string(int16_t x, int16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size) {
    while (*str) {
        if (x + 6 * size > ST7735_WIDTH) break;
        st7735_draw_char(x, y, *str, color, bg, size);
        x += 6 * size;
        str++;
    }
}

void st7735_draw_marquee(int16_t y, int16_t h, const char *text, int16_t scroll_x, uint16_t fg, uint16_t bg) {
    // Fill the banner area
    st7735_fill_rect(0, y, ST7735_WIDTH, h, bg);

    size_t len = strlen(text);
    int16_t text_width = len * 6; // 6 pixels per char (5 font + 1 space)
    if (text_width == 0) return;

    int16_t normalized_x = scroll_x % text_width;
    if (normalized_x > 0) normalized_x -= text_width;

    // Draw text with wrapping so ticker is seamless
    int16_t curr_x = normalized_x;
    while (curr_x < ST7735_WIDTH) {
        for (size_t i = 0; i < len; i++) {
            int16_t char_x = curr_x + i * 6;
            if (char_x >= -6 && char_x < ST7735_WIDTH) {
                st7735_draw_char(char_x, y + (h - 7) / 2, text[i], fg, bg, 1);
            }
        }
        curr_x += text_width;
    }
}

void st7735_draw_visualizer(int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *levels, uint8_t num_bars) {
    if (num_bars == 0) return;
    int16_t bar_w = (w - (num_bars - 1) * 2) / num_bars;
    if (bar_w < 1) bar_w = 1;

    for (uint8_t i = 0; i < num_bars; i++) {
        int16_t bx = x + i * (bar_w + 2);
        int16_t bar_h = (levels[i] * h) / 100;
        if (bar_h > h) bar_h = h;

        // Gradient color based on height: Cyan -> Green -> Amber
        uint16_t bar_color;
        if (levels[i] < 40) bar_color = COLOR_CYAN;
        else if (levels[i] < 75) bar_color = COLOR_GREEN_OK;
        else bar_color = COLOR_AMBER;

        // Draw active bar
        if (bar_h > 0) {
            st7735_fill_rect(bx, y + (h - bar_h), bar_w, bar_h, bar_color);
        }
        // Clear top empty area
        if (bar_h < h) {
            st7735_fill_rect(bx, y, bar_w, h - bar_h, COLOR_BG_DARK);
        }
    }
}
