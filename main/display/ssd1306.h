#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SSD1306_I2C_ADDR        0x3C
#define SSD1306_WIDTH           128
#define SSD1306_HEIGHT          64
#define SSD1306_BUFFER_SIZE     (SSD1306_WIDTH * SSD1306_HEIGHT / 8)

/**
 * @brief Initialize SSD1306 OLED display
 * @return ESP_OK on success
 */
esp_err_t ssd1306_init(void);

/**
 * @brief Clear the display
 */
void ssd1306_clear(void);

/**
 * @brief Update display from buffer
 */
void ssd1306_update(void);

/**
 * @brief Set a pixel
 * @param x X coordinate (0-127)
 * @param y Y coordinate (0-63)
 * @param on true = on, false = off
 */
void ssd1306_set_pixel(int x, int y, bool on);

/**
 * @brief Draw a string at position
 * @param x X position (column)
 * @param y Y position (row, 0-7 for 8 rows)
 * @param text String to display
 */
void ssd1306_draw_string(int x, int y, const char *text);

/**
 * @brief Draw a string with automatic line wrapping
 * @param x X start position
 * @param y Y start position (row)
 * @param text String to display
 * @return Y position for next line
 */
int ssd1306_draw_string_wrap(int x, int y, const char *text);

/**
 * @brief Fill a rectangle
 * @param x X position
 * @param y Y position
 * @param w Width
 * @param h Height
 * @param on true = fill, false = clear
 */
void ssd1306_fill_rect(int x, int y, int w, int h, bool on);

/**
 * @brief Draw a horizontal line
 * @param x X start position
 * @param y Y position
 * @param len Length
 * @param on true = on, false = off
 */
void ssd1306_hline(int x, int y, int len, bool on);

/**
 * @brief Draw a vertical line
 * @param x X position
 * @param y Y start position
 * @param len Length
 * @param on true = on, false = off
 */
void ssd1306_vline(int x, int y, int len, bool on);

/**
 * @brief Display progress bar
 * @param x X position
 * @param y Y position
 * @param w Width
 * @param h Height
 * @param percent 0-100
 */
void ssd1306_progress_bar(int x, int y, int w, int h, int percent);

/**
 * @brief Check if display is connected
 * @return true if display responds
 */
bool ssd1306_is_connected(void);

/**
 * @brief Get display buffer pointer (for direct access)
 * @return Pointer to 128x64 buffer
 */
uint8_t *ssd1306_get_buffer(void);
