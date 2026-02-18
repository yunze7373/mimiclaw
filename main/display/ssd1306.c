#include "display/ssd1306.h"
#include "display/font5x7.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c.h"
#include "mimi_config.h"
#include "driver/gpio.h"

static const char *TAG = "ssd1306";

#define I2C_MASTER_NUM       I2C_NUM_0
#define I2C_TIMEOUT_MS       1000

/* SSD1306 Commands */
#define SSD1306_CMD_SET_MEM_ADDR_MODE    0x20
#define SSD1306_CMD_SET_COL_ADDR         0x21
#define SSD1306_CMD_SET_PAGE_ADDR         0x22
#define SSD1306_CMD_SET_DISPLAY_START    0x40
#define SSD1306_CMD_SET_CONTRAST         0x81
#define SSD1306_CMD_SET_CHARGEPUMP       0x8D
#define SSD1306_CMD_SET_SEG_REMAP         0xA0
#define SSD1306_CMD_SET_ENTIRE_ON         0xA4
#define SSD1306_CMD_SET_INVERSE           0xA6
#define SSD1306_CMD_SET_MULTIPLEX         0xA8
#define SSD1306_CMD_SET_DISPLAY_OFF      0xAE
#define SSD1306_CMD_SET_DISPLAY_ON       0xAF
#define SSD1306_CMD_SET_PAGE_START        0xB0
#define SSD1306_CMD_SET_COM_PINS         0xDA
#define SSD1306_CMD_SET_VCOM_DETECT      0xDB
#define SSD1306_CMD_SET_CLOCK_DIV        0xD5
#define SSD1306_CMD_SET_PRECHARGE        0xD9

static uint8_t s_buffer[SSD1306_BUFFER_SIZE];
static bool s_inited = false;

static esp_err_t ssd1306_write_cmd(uint8_t cmd)
{
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(handle, 0x00, true);  /* Control byte: command */
    i2c_master_write_byte(handle, cmd, true);
    i2c_master_stop(handle);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, handle, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(handle);
    return ret;
}

static esp_err_t ssd1306_write_data(const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(handle, 0x40, true);  /* Control byte: data */
    i2c_master_write(handle, data, len, true);
    i2c_master_stop(handle);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, handle, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(handle);
    return ret;
}

bool ssd1306_is_connected(void)
{
    /* Try to init I2C if not already done */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = MIMI_PIN_I2C0_SDA,
        .scl_io_num = MIMI_PIN_I2C0_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = MIMI_I2C0_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);

    /* Install driver if not already installed (ignore error if already installed) */
    i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);

    /* Try to read ID (simple check by sending display off command) */
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(handle, 0x00, true);  /* Command mode */
    i2c_master_write_byte(handle, 0xAE, true);  /* Display off */
    i2c_master_stop(handle);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, handle, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(handle);
    return ret == ESP_OK;
}

esp_err_t ssd1306_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SSD1306 OLED on I2C0 (SDA=%d, SCL=%d)",
             MIMI_PIN_I2C0_SDA, MIMI_PIN_I2C0_SCL);

    /* Initialize I2C */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = MIMI_PIN_I2C0_SDA,
        .scl_io_num = MIMI_PIN_I2C0_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = MIMI_I2C0_FREQ_HZ,
    };

    /* Configure I2C params (may fail if already configured, that's OK) */
    i2c_param_config(I2C_MASTER_NUM, &conf);

    /* Install driver if not already installed */
    esp_err_t ret = i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C driver install failed: %d", ret);
        return ret;
    }

    /* Reset display */
    ssd1306_write_cmd(0xAE);  /* Display off */
    ssd1306_write_cmd(0xD5);  /* Clock div */
    ssd1306_write_cmd(0x80);
    ssd1306_write_cmd(0xA8);  /* Multiplex */
    ssd1306_write_cmd(0x3F);  /* 64 rows */
    ssd1306_write_cmd(0xD3);  /* Display offset */
    ssd1306_write_cmd(0x00);
    ssd1306_write_cmd(0x40);  /* Start line */
    ssd1306_write_cmd(0x8D);  /* Charge pump */
    ssd1306_write_cmd(0x14);
    ssd1306_write_cmd(0x20);  /* Memory mode */
    ssd1306_write_cmd(0x00);  /* Horizontal addressing */
    ssd1306_write_cmd(0xA1);  /* Segment remap */
    ssd1306_write_cmd(0xC8);  /* COM output direction */
    ssd1306_write_cmd(0xDA);  /* COM pins */
    ssd1306_write_cmd(0x12);
    ssd1306_write_cmd(0x81);  /* Contrast */
    ssd1306_write_cmd(0xCF);
    ssd1306_write_cmd(0xD9);  /* Pre-charge */
    ssd1306_write_cmd(0xF1);
    ssd1306_write_cmd(0xDB);  /* VCOM detect */
    ssd1306_write_cmd(0x40);
    ssd1306_write_cmd(0xA4);  /* Entire display off */
    ssd1306_write_cmd(0xA6);  /* Normal display */
    ssd1306_write_cmd(0xAF);  /* Display on */

    memset(s_buffer, 0, sizeof(s_buffer));
    s_inited = true;

    ESP_LOGI(TAG, "SSD1306 initialized successfully");
    return ESP_OK;
}

void ssd1306_clear(void)
{
    memset(s_buffer, 0, sizeof(s_buffer));
}

void ssd1306_update(void)
{
    for (int page = 0; page < 8; page++) {
        ssd1306_write_cmd(0xB0 + page);  /* Page address */
        ssd1306_write_cmd(0x00);         /* Column low */
        ssd1306_write_cmd(0x10);         /* Column high */
        ssd1306_write_data(&s_buffer[page * SSD1306_WIDTH], SSD1306_WIDTH);
    }
}

void ssd1306_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }
    int index = (y / 8) * SSD1306_WIDTH + x;
    if (on) {
        s_buffer[index] |= (1 << (y % 8));
    } else {
        s_buffer[index] &= ~(1 << (y % 8));
    }
}

uint8_t *ssd1306_get_buffer(void)
{
    return s_buffer;
}

/* Simple 5x7 font (ASCII 32-126) */
extern const uint8_t font5x7_data[];

void ssd1306_draw_string(int x, int y, const char *text)
{
    if (!text) return;

    int col = x;
    while (*text) {
        unsigned char c = *text;
        if (c < 32 || c > 126) {
            c = 32;  /* Space for unknown */
        }
        c -= 32;

        /* Draw 5x7 character */
        for (int i = 0; i < 5; i++) {
            uint8_t col_data = font5x7[c][i];
            for (int j = 0; j < 7; j++) {
                if (col_data & (0x80 >> j)) {
                    ssd1306_set_pixel(col + i, y + j, true);
                }
            }
        }

        /* Space between characters */
        col += 6;
        if (col >= SSD1306_WIDTH) break;
        text++;
    }
}

int ssd1306_draw_string_wrap(int x, int y, const char *text)
{
    if (!text) return y;

    int col = x;
    int row = y;

    while (*text) {
        unsigned char c = *text;

        /* Handle newlines */
        if (c == '\n' || c == '\r') {
            row += 8;
            col = x;
            if (row >= SSD1306_HEIGHT) break;
            text++;
            continue;
        }

        if (c < 32 || c > 126) c = 32;
        c -= 32;

        /* Check if we need to wrap */
        if (col + 6 > SSD1306_WIDTH) {
            row += 8;
            col = x;
            if (row >= SSD1306_HEIGHT) break;
        }

        /* Draw character */
        for (int i = 0; i < 5; i++) {
            uint8_t col_data = font5x7[c][i];
            for (int j = 0; j < 7; j++) {
                if (col_data & (0x80 >> j)) {
                    ssd1306_set_pixel(col + i, row + j, true);
                }
            }
        }

        col += 6;
        text++;
    }

    return row + 8;
}

void ssd1306_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < h; j++) {
            ssd1306_set_pixel(x + i, y + j, on);
        }
    }
}

void ssd1306_hline(int x, int y, int len, bool on)
{
    for (int i = 0; i < len; i++) {
        ssd1306_set_pixel(x + i, y, on);
    }
}

void ssd1306_vline(int x, int y, int len, bool on)
{
    for (int i = 0; i < len; i++) {
        ssd1306_set_pixel(x, y + i, on);
    }
}

void ssd1306_progress_bar(int x, int y, int w, int h, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* Draw border */
    ssd1306_fill_rect(x, y, w, h, false);
    ssd1306_hline(x, y, w, true);
    ssd1306_hline(x, y + h - 1, w, true);
    ssd1306_vline(x, y, h, true);
    ssd1306_vline(x + w - 1, y, h, true);

    /* Draw fill */
    int fill_w = (w - 2) * percent / 100;
    if (fill_w > 0) {
        ssd1306_fill_rect(x + 1, y + 1, fill_w, h - 2, true);
    }
}
