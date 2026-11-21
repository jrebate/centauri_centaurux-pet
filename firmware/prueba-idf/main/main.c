// Centauri: first test for the round ESP32-2424S012 board (ESP32-C3 + GC9A01 240x240 + CST816 touch).
// Shows the centaur and lets you change its state by tapping or swiping, so we can check
// that the screen, the colours and the touch all work before building the real pet.
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font.h"
#include "sprites.h"

static const char *TAG = "centauri";

// Pins of the ESP32-2424S012 (community pinout; this test is how we confirm them)
#define PIN_SCLK 6
#define PIN_MOSI 7
#define PIN_DC   2
#define PIN_CS   10
#define PIN_BL   3
#define PIN_TP_SDA 4
#define PIN_TP_SCL 5
#define PIN_TP_RST 1
#define PIN_TP_INT 0
#define TP_ADDR 0x15

#define W 240
#define H 240
#define BAND 40

static uint16_t fb[W * H];
static uint16_t *band_buf;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t flush_done;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }

static const char *STATE_LABEL[6] = { "Descansa", "Estudia", "Forja", "Te llama", "\xc2\xa1Victoria!", "Herido" };
static uint16_t state_color[6];

// ---------- Drawing ----------
static void px(int x, int y, uint16_t c) { if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = c; }
static void rect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c);
}
static void ring(int r0, int r1, uint16_t c) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int dx = x - W / 2, dy = y - H / 2, d2 = dx * dx + dy * dy;
        if (d2 >= r0 * r0 && d2 < r1 * r1) fb[y * W + x] = c;
    }
}

static const glyph_t *glyph(uint32_t cp) {
    for (int i = 0; i < FONT_COUNT; i++) if (FONT[i].cp == cp) return &FONT[i];
    return &FONT['?' - 32];
}
static uint32_t next_cp(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp = *p++;
    if (cp >= 0xc0 && *p) { cp = ((cp & 0x1f) << 6) | (*p++ & 0x3f); }
    *s = (const char *)p;
    return cp;
}
static int text_width(const char *s, int scale) {
    int w = 0;
    while (*s) w += glyph(next_cp(&s))->w * scale;
    return w;
}
// y is the baseline
static void text(int x, int y, const char *s, int scale, uint16_t c) {
    while (*s) {
        const glyph_t *g = glyph(next_cp(&s));
        for (int r = 0; r < FONT_H; r++)
            for (int b = 0; b < 8; b++)
                if (g->rows[r] & (1 << b)) rect(x + b * scale, y + (r - FONT_BASE) * scale, scale, scale, c);
        x += g->w * scale;
    }
}
static void text_c(int y, const char *s, int scale, uint16_t c) { text((W - text_width(s, scale)) / 2, y, s, scale, c); }

static void sprite(int st, int x0, int y0, int scale) {
    const uint8_t *s = SPRITES[st];
    for (int y = 0; y < SPR_H; y++)
        for (int x = 0; x < SPR_W; x++) {
            uint8_t i = s[y * SPR_W + x];
            if (i) rect(x0 + x * scale, y0 + y * scale, scale, scale, SPR_PALETTE[i]);
        }
}

static bool on_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(flush_done, &woke);
    return woke == pdTRUE;
}
static void flush(void) {
    for (int y = 0; y < H; y += BAND) {
        for (int i = 0; i < W * BAND; i++) { uint16_t v = fb[y * W + i]; band_buf[i] = (v >> 8) | (v << 8); }
        esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + BAND, band_buf);
        xSemaphoreTake(flush_done, portMAX_DELAY);
    }
}

// ---------- Touch (CST816) ----------
static bool touch_ok;
static bool touch_read(int *x, int *y) {
    uint8_t reg = 0x02, d[5];
    if (i2c_master_write_read_device(I2C_NUM_0, TP_ADDR, &reg, 1, d, 5, pdMS_TO_TICKS(20)) != ESP_OK) return false;
    touch_ok = true;
    if ((d[0] & 0x0f) == 0) return false;
    *x = ((d[1] & 0x0f) << 8) | d[2];
    *y = ((d[3] & 0x0f) << 8) | d[4];
    return true;
}
static void touch_init(void) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_TP_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(PIN_TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(60));
    i2c_config_t c = { .mode = I2C_MODE_MASTER, .sda_io_num = PIN_TP_SDA, .scl_io_num = PIN_TP_SCL,
                       .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
                       .master.clk_speed = 400000 };
    i2c_param_config(I2C_NUM_0, &c);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    uint8_t reg = 0xA7, id = 0;
    touch_ok = i2c_master_write_read_device(I2C_NUM_0, TP_ADDR, &reg, 1, &id, 1, pdMS_TO_TICKS(50)) == ESP_OK;
    ESP_LOGI(TAG, "touch %s, chip id 0x%02x", touch_ok ? "found" : "not found", id);
}

// ---------- Display ----------
static void lcd_init(void) {
    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_BL, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 0);

    spi_bus_config_t bus = { .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = W * BAND * 2 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = { .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS, .pclk_hz = 40 * 1000 * 1000,
                                          .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 4,
                                          .on_color_trans_done = on_flush_done };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io));

    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &pc, &panel));
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_mirror(panel, true, false);
    esp_lcd_panel_disp_on_off(panel, true);
}

// ---------- Screen ----------
static int state = ST_WORKING;
static int dot_x = -1, dot_y = -1;

static void draw(void) {
    for (int i = 0; i < W * H; i++) fb[i] = rgb(13, 15, 23);
    ring(110, 119, state_color[state]);
    text_c(40, "CENTAURI", 2, rgb(233, 220, 192));
    sprite(state, (W - SPR_W * 3) / 2, 52, 3);
    text_c(188, STATE_LABEL[state], 2, state_color[state]);
    text_c(204, "Toca o desliza", 1, rgb(143, 138, 153));
    text_c(216, touch_ok ? "T\xc3\xa1" "ctil: OK" : "T\xc3\xa1" "ctil: no detectado", 1,
           touch_ok ? rgb(125, 220, 143) : rgb(255, 100, 100));
    if (dot_x >= 0) { rect(dot_x - 3, dot_y - 3, 7, 7, rgb(255, 255, 255)); }
    // colour check: these three squares should read red, green, blue
    rect(96, 222, 14, 6, rgb(255, 0, 0)); rect(113, 222, 14, 6, rgb(0, 255, 0)); rect(130, 222, 14, 6, rgb(0, 0, 255));
}

void app_main(void) {
    state_color[ST_SLEEPING] = rgb(0x8f, 0xa0, 0xbf);
    state_color[ST_THINKING] = rgb(0xb3, 0xa4, 0xff);
    state_color[ST_WORKING]  = rgb(0xff, 0x9a, 0x5c);
    state_color[ST_WAITING]  = rgb(0xf6, 0xc9, 0x45);
    state_color[ST_HAPPY]    = rgb(0x7d, 0xdc, 0x8f);
    state_color[ST_WORRIED]  = rgb(0xff, 0x64, 0x64);

    flush_done = xSemaphoreCreateBinary();
    band_buf = heap_caps_malloc(W * BAND * 2, MALLOC_CAP_DMA);
    lcd_init();
    touch_init();
    draw(); flush();
    gpio_set_level(PIN_BL, 1);

    bool down = false; int sx = 0, sy = 0, lx = 0, ly = 0;
    for (;;) {
        int x, y;
        bool now = touch_read(&x, &y);
        bool redraw = false;
        if (now) {
            if (!down) { sx = x; sy = y; }
            lx = x; ly = y; dot_x = x; dot_y = y; redraw = true;
        } else if (down) {
            int dx = lx - sx;
            if (dx > 40) state = (state + 5) % 6;          // swipe right: previous state
            else if (dx < -40) state = (state + 1) % 6;    // swipe left: next state
            else if (abs(ly - sy) < 25) state = (state + 1) % 6;  // tap: next state
            ESP_LOGI(TAG, "release at %d,%d (start %d,%d) -> state %d", lx, ly, sx, sy, state);
            dot_x = -1; redraw = true;
        }
        down = now;
        if (redraw) { draw(); flush(); }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
