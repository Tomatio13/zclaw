#include "ui_display.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ZCLAW_M5STACK_DISPLAY_AVAILABLE CONFIG_IDF_TARGET_ESP32

static const char *TAG = "ui_display";

#if ZCLAW_M5STACK_DISPLAY_AVAILABLE
// M5Stack Core/Basic display pins (ILI9342C over SPI)
#define LCD_HOST SPI2_HOST
#define LCD_PIN_SCLK 18
#define LCD_PIN_MOSI 23
#define LCD_PIN_MISO 19
#define LCD_PIN_CS   14
#define LCD_PIN_DC   27
#define LCD_PIN_RST  33
#define LCD_PIN_BL   32

#define LCD_WIDTH 320
#define LCD_HEIGHT 240

static TaskHandle_t s_clock_task_handle = NULL;
static volatile bool s_thinking_active = false;
static bool s_display_ready = false;
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;

typedef struct {
    char c;
    uint8_t rows[7];
} glyph_t;

typedef struct {
    int y;
    int speed;
    int length;
    int gap;
} matrix_column_t;

typedef struct {
    char ch;
    float brightness;
    bool is_head;
} matrix_cell_t;

#define MATRIX_SCALE      2
#define MATRIX_CELL_W     (6 * MATRIX_SCALE)
#define MATRIX_CELL_H     (7 * MATRIX_SCALE)
#define MATRIX_COLS       (LCD_WIDTH / MATRIX_CELL_W)
#define MATRIX_ROWS       (LCD_HEIGHT / MATRIX_CELL_H)

static const char *MATRIX_CHARS = "0123456789";
static matrix_column_t s_matrix_columns[MATRIX_COLS];
static matrix_cell_t s_matrix_grid[MATRIX_ROWS][MATRIX_COLS];
static uint32_t s_matrix_tick = 0;

static const glyph_t FONT_5X7[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x06, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'h', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
    {'k', {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}},
    {'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'g', {0x00, 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x0E}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

static const uint8_t *glyph_rows(char c)
{
    size_t count = sizeof(FONT_5X7) / sizeof(FONT_5X7[0]);
    for (size_t i = 0; i < count; i++) {
        if (FONT_5X7[i].c == c) {
            return FONT_5X7[i].rows;
        }
    }
    return FONT_5X7[sizeof(FONT_5X7) / sizeof(FONT_5X7[0]) - 1].rows; // space
}

static uint32_t rand_u32(void)
{
    return esp_random();
}

static int rand_range(int min_inclusive, int max_inclusive)
{
    if (max_inclusive <= min_inclusive) {
        return min_inclusive;
    }
    uint32_t span = (uint32_t)(max_inclusive - min_inclusive + 1);
    return min_inclusive + (int)(rand_u32() % span);
}

static char random_matrix_char(void)
{
    size_t n = strlen(MATRIX_CHARS);
    return MATRIX_CHARS[rand_u32() % n];
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

static uint16_t scale_blue(float b)
{
    if (b <= 0.0f) {
        return rgb565(0x00, 0x00, 0x00);
    }
    if (b > 1.0f) {
        b = 1.0f;
    }
    uint8_t blue = (uint8_t)(0x44 + (0xFF - 0x44) * b);
    return rgb565(0x00, 0x66, blue);
}

static void lcd_reset_pulse(void)
{
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t lcd_send_cmd(uint8_t cmd, const void *data, size_t data_len)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(s_lcd_io, cmd, data, data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD command 0x%02X failed: %s", cmd, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t lcd_init_sequence(void)
{
    static const uint8_t pix_fmt_16bpp = 0x55;
    static const uint8_t madctl_landscape_bgr = 0x48; // MX + BGR (no MV/MY)

    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x01, NULL, 0), TAG, "SWRESET");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x11, NULL, 0), TAG, "SLPOUT");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x3A, &pix_fmt_16bpp, 1), TAG, "COLMOD");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x36, &madctl_landscape_bgr, 1), TAG, "MADCTL");
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x21, NULL, 0), TAG, "INVON");
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x13, NULL, 0), TAG, "NORON");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x29, NULL, 0), TAG, "DISPON");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t lcd_set_window(int xs, int xe, int ys, int ye)
{
    uint8_t data_col[4] = {
        (uint8_t)(xs >> 8), (uint8_t)(xs & 0xFF),
        (uint8_t)(xe >> 8), (uint8_t)(xe & 0xFF),
    };
    uint8_t data_row[4] = {
        (uint8_t)(ys >> 8), (uint8_t)(ys & 0xFF),
        (uint8_t)(ye >> 8), (uint8_t)(ye & 0xFF),
    };

    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x2A, data_col, sizeof(data_col)), TAG, "CASET");
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x2B, data_row, sizeof(data_row)), TAG, "RASET");
    return ESP_OK;
}

static void matrix_init(void)
{
    memset(s_matrix_grid, 0, sizeof(s_matrix_grid));
    for (int x = 0; x < MATRIX_COLS; x++) {
        s_matrix_columns[x].y = -rand_range(0, MATRIX_ROWS);
        s_matrix_columns[x].speed = (rand_u32() % 100 < 70) ? 1 : 2;
        s_matrix_columns[x].length = rand_range(2, 5);
        s_matrix_columns[x].gap = rand_range(0, 2);
    }
}

static void matrix_step(void)
{
    s_matrix_tick++;

    for (int y = 0; y < MATRIX_ROWS; y++) {
        for (int x = 0; x < MATRIX_COLS; x++) {
            matrix_cell_t *cell = &s_matrix_grid[y][x];
            cell->is_head = false;
            cell->brightness -= 0.08f;
            if (cell->brightness < 0.0f) {
                cell->brightness = 0.0f;
            }
            if (cell->brightness > 0.15f && cell->brightness < 0.7f && (rand_u32() % 100) < 4) {
                cell->ch = random_matrix_char();
            }
            if (cell->brightness <= 0.01f) {
                cell->ch = ' ';
            }
        }
    }

    for (int x = 0; x < MATRIX_COLS; x++) {
        matrix_column_t *col = &s_matrix_columns[x];
        if (col->gap > 0) {
            col->gap--;
            continue;
        }

        if ((s_matrix_tick % (uint32_t)col->speed) != 0) {
            continue;
        }

        col->y++;

        if (col->y >= 0 && col->y < MATRIX_ROWS) {
            matrix_cell_t *head = &s_matrix_grid[col->y][x];
            head->ch = random_matrix_char();
            head->brightness = 1.0f;
            head->is_head = true;
        }

        for (int t = 1; t <= col->length; t++) {
            int ty = col->y - t;
            if (ty < 0 || ty >= MATRIX_ROWS) {
                continue;
            }
            matrix_cell_t *trail = &s_matrix_grid[ty][x];
            if (trail->ch == ' ') {
                trail->ch = random_matrix_char();
            }
            float trail_b = 1.0f - ((float)t / (float)col->length);
            if (trail_b > trail->brightness) {
                trail->brightness = trail_b;
            }
        }

        if (col->y - col->length > MATRIX_ROWS) {
            col->y = -rand_range(0, 2);
            col->speed = (rand_u32() % 100 < 70) ? 1 : 2;
            col->length = rand_range(2, 5);
            col->gap = rand_range(0, 3);
        }
    }
}

typedef struct {
    const char *text;
    int x;
    int y;
    int scale;
    uint16_t color;
} text_render_t;

static void prepare_centered_text(text_render_t *spec, const char *text, int y, int scale, uint16_t color)
{
    spec->text = text;
    spec->y = y;
    spec->scale = scale;
    spec->color = color;
    size_t len = strlen(text);
    int char_w = 6 * scale;
    int text_w = 0;
    if (len > 0) {
        // 5x7 glyph + 1-column spacing per character; no trailing spacing for last char.
        text_w = (int)(len * (size_t)char_w) - scale;
    }
    spec->x = (LCD_WIDTH - text_w) / 2;
}

static void rasterize_text_row(const text_render_t *spec, int row, uint16_t *line_buf)
{
    if (!spec || !spec->text) {
        return;
    }

    int local_y = row - spec->y;
    int glyph_h = 7 * spec->scale;
    if (local_y < 0 || local_y >= glyph_h) {
        return;
    }

    int glyph_row = local_y / spec->scale;
    size_t len = strlen(spec->text);
    int char_w = 6 * spec->scale;
    for (size_t i = 0; i < len; i++) {
        const uint8_t *rows = glyph_rows(spec->text[i]);
        uint8_t bits = rows[glyph_row];
        int char_x = spec->x + (int)(i * (size_t)char_w);
        for (int col = 0; col < 5; col++) {
            if (((bits >> (4 - col)) & 0x01) == 0) {
                continue;
            }
            int pixel_x0 = char_x + (col * spec->scale);
            for (int sx = 0; sx < spec->scale; sx++) {
                int px = pixel_x0 + sx;
                if (px < 0 || px >= LCD_WIDTH) {
                    continue;
                }
                int mirrored_x = (LCD_WIDTH - 1) - px;
                line_buf[mirrored_x] = spec->color;
            }
        }
    }
}

static void rasterize_char_row(char ch, int origin_x, int origin_y, int scale, uint16_t color,
                               int row, uint16_t *line_buf)
{
    int local_y = row - origin_y;
    int glyph_h = 7 * scale;
    if (local_y < 0 || local_y >= glyph_h) {
        return;
    }

    int glyph_row = local_y / scale;
    const uint8_t *rows = glyph_rows(ch);
    uint8_t bits = rows[glyph_row];
    for (int col = 0; col < 5; col++) {
        if (((bits >> (4 - col)) & 0x01) == 0) {
            continue;
        }
        int pixel_x0 = origin_x + (col * scale);
        for (int sx = 0; sx < scale; sx++) {
            int px = pixel_x0 + sx;
            if (px < 0 || px >= LCD_WIDTH) {
                continue;
            }
            int mirrored_x = (LCD_WIDTH - 1) - px;
            line_buf[mirrored_x] = color;
        }
    }
}

static esp_err_t render_clock_and_status(bool thinking_active, const char *time_text)
{
    const uint16_t black = rgb565(0x00, 0x00, 0x00);
    const uint16_t clock_blue = rgb565(0x00, 0x66, 0xFF);
    static uint16_t line_buf[LCD_WIDTH];

    text_render_t time_spec = {0};
    prepare_centered_text(&time_spec, time_text, 84, 4, clock_blue);

    text_render_t thinking_spec = {0};
    if (thinking_active) {
        prepare_centered_text(&thinking_spec, "Thinking....", 142, 3, clock_blue);
    }

    matrix_step();

    for (int row = 0; row < LCD_HEIGHT; row++) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            line_buf[x] = black;
        }

        // Matrix layer
        for (int gy = 0; gy < MATRIX_ROWS; gy++) {
            int char_top = gy * MATRIX_CELL_H;
            if (row < char_top || row >= char_top + MATRIX_CELL_H) {
                continue;
            }
            for (int gx = 0; gx < MATRIX_COLS; gx++) {
                matrix_cell_t *cell = &s_matrix_grid[gy][gx];
                if (cell->brightness <= 0.08f || cell->ch == ' ') {
                    continue;
                }
                uint16_t c = cell->is_head ? rgb565(0x80, 0xC0, 0xFF) : scale_blue(cell->brightness);
                rasterize_char_row(cell->ch, gx * MATRIX_CELL_W, char_top, MATRIX_SCALE, c, row, line_buf);
            }
        }

        // Overlay layer
        rasterize_text_row(&time_spec, row, line_buf);
        if (thinking_active) {
            rasterize_text_row(&thinking_spec, row, line_buf);
        }

        ESP_RETURN_ON_ERROR(lcd_set_window(0, LCD_WIDTH - 1, row, row), TAG, "set row window");
        esp_err_t err = esp_lcd_panel_io_tx_color(s_lcd_io, 0x2C, line_buf, LCD_WIDTH * sizeof(uint16_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Row %d transfer failed: %s", row, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t lcd_clear_fullscreen(uint16_t color)
{
    static uint16_t line_buf[LCD_WIDTH];
    for (int x = 0; x < LCD_WIDTH; x++) {
        line_buf[x] = color;
    }

    for (int y = 0; y < LCD_HEIGHT; y++) {
        ESP_RETURN_ON_ERROR(lcd_set_window(0, LCD_WIDTH - 1, y, y), TAG, "set clear window");
        esp_err_t err = esp_lcd_panel_io_tx_color(s_lcd_io, 0x2C, line_buf, LCD_WIDTH * sizeof(uint16_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Clear row %d failed: %s", y, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static void ui_clock_task(void *arg)
{
    (void)arg;

    int consecutive_failures = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        char current_time[16] = "--:--:--";
        time_t now = 0;
        struct tm timeinfo = {0};
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (1970 - 1900)) {
            strftime(current_time, sizeof(current_time), "%H:%M:%S", &timeinfo);
        }

        bool thinking_now = s_thinking_active;
        if (s_display_ready) {
            if (render_clock_and_status(thinking_now, current_time) == ESP_OK) {
                consecutive_failures = 0;
            } else {
                consecutive_failures++;
                ESP_LOGW(TAG, "Clock render failed (%d)", consecutive_failures);
                if (consecutive_failures >= 3) {
                    ESP_LOGW(TAG, "Reinitializing LCD after repeated render failures");
                    lcd_reset_pulse();
                    if (lcd_init_sequence() == ESP_OK &&
                        lcd_clear_fullscreen(rgb565(0x00, 0x00, 0x00)) == ESP_OK) {
                        consecutive_failures = 0;
                    }
                }
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
    }
}
#endif

esp_err_t ui_display_init(void)
{
#if !ZCLAW_M5STACK_DISPLAY_AVAILABLE
    ESP_LOGI(TAG, "M5Stack display support unavailable for this target/build");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_display_ready) {
        return ESP_OK;
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * sizeof(uint16_t) + 8,
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 10 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_lcd_io),
                        TAG, "panel io create");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_RST) | (1ULL << LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config");
    lcd_reset_pulse();
    ESP_RETURN_ON_ERROR(lcd_init_sequence(), TAG, "lcd_init_sequence");
    gpio_set_level(LCD_PIN_BL, 1);
    ESP_RETURN_ON_ERROR(lcd_clear_fullscreen(rgb565(0x00, 0x00, 0x00)), TAG, "lcd_clear_fullscreen");
    matrix_init();

    s_display_ready = true;

    if (xTaskCreate(ui_clock_task, "ui_clock", 8192, NULL, 2, &s_clock_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI clock task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Display UI initialized");
    return ESP_OK;
#endif
}

void ui_display_set_thinking(bool active)
{
#if ZCLAW_M5STACK_DISPLAY_AVAILABLE
    s_thinking_active = active;
#else
    (void)active;
#endif
}
