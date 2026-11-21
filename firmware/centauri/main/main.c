// Centauri: the Claude pet on the round ESP32-2424S012 board (ESP32-C3 + GC9A01 240x240 + CST816 touch).
// The desktop app sends the missions over Wi-Fi (POST http://centauri.local/estado) and the
// screen shows one mission at a time; swipe to change mission.
// First boot (or no known Wi-Fi): the board opens its own network "Centauri" with a page at
// http://192.168.4.1 where you type your Wi-Fi name and password.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "font.h"
#include "sprites.h"

static const char *TAG = "centauri";

// Pins of the ESP32-2424S012 (checked on Jorge's board, 2026-11-21)
#define PIN_SCLK 6
#define PIN_MOSI 7
#define PIN_DC   2
#define PIN_CS   10
#define PIN_BL   3
#define PIN_TP_SDA 4
#define PIN_TP_SCL 5
#define PIN_TP_RST 1
#define TP_ADDR 0x15

#define W 240
#define H 240
#define BAND 40
#define AP_SSID "Centauri"
#define HOSTNAME "centauri"

// 8-bit framebuffer: each pixel is an index into pal[], so the screen takes 57 KB instead of 115 KB
// (the rest of the RAM is needed by Wi-Fi). Indices 0-31 are the sprite palette.
static uint8_t fb[W * H];
static uint16_t pal[64];
static int npal = 32;
static uint16_t *band_buf;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t flush_done;

static uint8_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    for (int i = 32; i < npal; i++) if (pal[i] == c) return i;
    if (npal >= 64) return 32;
    pal[npal] = c;
    return npal++;
}

// ---------- Missions sent by the desktop app ----------
#define MAX_P 8
typedef struct {
    char name[48];
    char status[96];
    int state;
    char objeto[12];    // forge item, empty if none
    int piezas, hechas;
} project_t;

static project_t projects[MAX_P];
static int n_projects;
static int64_t last_post_us;        // 0 = never received anything
static SemaphoreHandle_t data_lock;
static volatile bool dirty = true;

// Rewards collected by tapping a Victory on the screen. The screen shows them asleep at once;
// the PC hears about them in the reply to its next update, and until its data catches up
// the screen keeps them asleep.
#define MAX_PEND 8
typedef struct { char name[48]; bool told; int64_t until; } pending_t;
static pending_t pend[MAX_PEND];
static int n_pend;
static int64_t last_change_us, last_touch_us, reward_flash_us;

static const char *STATE_KEYS[6] = { "sleeping", "thinking", "working", "waiting", "happy", "worried" };
static const char *STATE_LABEL[6] = { "Descansa", "Estudia", "Forja", "Te llama", "\xc2\xa1Victoria!", "Herido" };
static uint8_t state_color[6];

// ---------- Wi-Fi state ----------
enum { NET_STARTING, NET_CONNECTING, NET_ONLINE, NET_SETUP };
static volatile int net_mode = NET_STARTING;
static char net_ip[16] = "";
static char saved_ssid[33] = "";
static char saved_pass[65] = "";
static char scan_list[1024] = "";   // <option> list for the setup page

// ---------- Drawing ----------
static void px(int x, int y, uint8_t c) { if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = c; }
static void rect(int x, int y, int w, int h, uint8_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c);
}
static void ring(int r0, int r1, uint8_t c) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int dx = x - W / 2, dy = y - H / 2, d2 = dx * dx + dy * dy;
        if (d2 >= r0 * r0 && d2 < r1 * r1) fb[y * W + x] = c;
    }
}
static void disc(int cx, int cy, int r, uint8_t c) {
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) if (x * x + y * y <= r * r) px(cx + x, cy + y, c);
}

static const glyph_t *glyph(uint32_t cp) {
    for (int i = 0; i < FONT_COUNT; i++) if (FONT[i].cp == cp) return &FONT[i];
    // accented letters the font lacks: show the plain letter
    if (cp == 0xe0 || cp == 0xe2 || cp == 0xe4) cp = 'a';
    else if (cp == 0xe8 || cp == 0xea || cp == 0xeb) cp = 'e';
    else if (cp == 0xec || cp == 0xee || cp == 0xef) cp = 'i';
    else if (cp == 0xf2 || cp == 0xf4 || cp == 0xf6) cp = 'o';
    else if (cp == 0xf9 || cp == 0xfb) cp = 'u';
    else if (cp == 0xe7) cp = 'c';
    else if (cp == 0x2014 || cp == 0x2013) cp = '-';
    else if (cp == 0x201c || cp == 0x201d) cp = '"';
    else if (cp == 0x2018 || cp == 0x2019) cp = '\'';
    else cp = '?';
    for (int i = 0; i < FONT_COUNT; i++) if (FONT[i].cp == cp) return &FONT[i];
    return &FONT[0];
}
static uint32_t next_cp(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp = *p++;
    int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
    if (extra) cp &= (0x3f >> extra);
    while (extra-- && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 0x3f);
    *s = (const char *)p;
    return cp;
}
static int text_width(const char *s, int scale) {
    int w = 0;
    while (*s) w += glyph(next_cp(&s))->w * scale;
    return w;
}
// y is the baseline
static void text(int x, int y, const char *s, int scale, uint8_t c) {
    while (*s) {
        const glyph_t *g = glyph(next_cp(&s));
        for (int r = 0; r < FONT_H; r++)
            for (int b = 0; b < 8; b++)
                if (g->rows[r] & (1 << b)) rect(x + b * scale, y + (r - FONT_BASE) * scale, scale, scale, c);
        x += g->w * scale;
    }
}
static void text_c(int y, const char *s, int scale, uint8_t c) { text((W - text_width(s, scale)) / 2, y, s, scale, c); }

// Copies s into out, cut with "…" so it fits in maxw pixels.
static void fit(char *out, size_t outsz, const char *s, int maxw, int scale) {
    if (text_width(s, scale) <= maxw) { snprintf(out, outsz, "%s", s); return; }
    int ell = text_width("\xe2\x80\xa6", scale), w = 0;
    const char *p = s, *cut = s;
    while (*p) {
        const char *q = p;
        int gw = glyph(next_cp(&q))->w * scale;
        if (w + gw + ell > maxw) break;
        w += gw; p = q; cut = p;
    }
    size_t n = cut - s; if (n > outsz - 4) n = outsz - 4;
    memcpy(out, s, n); while (n && out[n - 1] == ' ') n--;
    memcpy(out + n, "\xe2\x80\xa6", 4);
}
// Centred text over up to two lines, breaking at spaces. Returns lines used.
static int text_wrap2(int y, const char *s, int maxw, int scale, uint8_t c) {
    if (text_width(s, scale) <= maxw) { text_c(y, s, scale, c); return 1; }
    char line[96]; int best = 0;
    for (int i = 0; s[i] && i < (int)sizeof(line) - 1; i++) {
        if (s[i] != ' ') continue;
        memcpy(line, s, i); line[i] = 0;
        if (text_width(line, scale) <= maxw) best = i; else break;
    }
    if (!best) { fit(line, sizeof line, s, maxw, scale); text_c(y, line, scale, c); return 1; }
    memcpy(line, s, best); line[best] = 0;
    text_c(y, line, scale, c);
    char rest[96]; fit(rest, sizeof rest, s + best + 1, maxw, scale);
    text_c(y + (FONT_H + 3) * scale, rest, scale, c);
    return 2;
}

static void sprite(int st, int x0, int y0, int scale) {
    const uint8_t *s = SPRITES[st];
    for (int y = 0; y < SPR_H; y++)
        for (int x = 0; x < SPR_W; x++) {
            uint8_t i = s[y * SPR_W + x];
            if (i) rect(x0 + x * scale, y0 + y * scale, scale, scale, i);
        }
}

static bool on_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(flush_done, &woke);
    return woke == pdTRUE;
}
// Screen rotation in 90-degree steps, done here in software so touch matches exactly.
// Physical pixel (px,py) shows the drawn pixel returned by rot_src().
static int rotation;    // 0..3, saved in NVS
static inline void rot_src(int px, int py, int *lx, int *ly) {
    switch (rotation) {
        case 1: *lx = py; *ly = W - 1 - px; break;
        case 2: *lx = W - 1 - px; *ly = H - 1 - py; break;
        case 3: *lx = H - 1 - py; *ly = px; break;
        default: *lx = px; *ly = py;
    }
}
static void flush(void) {
    for (int y = 0; y < H; y += BAND) {
        if (rotation == 0) {
            for (int i = 0; i < W * BAND; i++) { uint16_t v = pal[fb[y * W + i]]; band_buf[i] = (v >> 8) | (v << 8); }
        } else {
            for (int j = 0; j < BAND; j++) for (int x = 0; x < W; x++) {
                int lx, ly; rot_src(x, y + j, &lx, &ly);
                uint16_t v = pal[fb[ly * W + lx]]; band_buf[j * W + x] = (v >> 8) | (v << 8);
            }
        }
        esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + BAND, band_buf);
        xSemaphoreTake(flush_done, portMAX_DELAY);
    }
}

// ---------- Touch (CST816) ----------
static bool touch_read(int *x, int *y) {
    uint8_t reg = 0x02, d[5];
    if (i2c_master_write_read_device(I2C_NUM_0, TP_ADDR, &reg, 1, d, 5, pdMS_TO_TICKS(20)) != ESP_OK) return false;
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
}

// ---------- Backlight (PWM, so it can dim) ----------
static int bl_now = -1;
static void bl_set(int duty) {
    if (duty == bl_now) return;
    bl_now = duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
static void bl_init(void) {
    ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&tc);
    ledc_channel_config_t cc = { .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                                 .timer_sel = LEDC_TIMER_0, .duty = 0 };
    ledc_channel_config(&cc);
    bl_now = 0;
}

// ---------- Display ----------
static void lcd_init(void) {
    bl_init();
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
    vTaskDelay(pdMS_TO_TICKS(150));     // let the panel power up before talking to it
    esp_lcd_panel_reset(panel);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    // This panel scans right-to-left by default, so mirror X to show the picture the right way.
    // (The first tests looked right without it only because the factory program had changed a
    // scan-direction register that kept its value until the board lost power.)
    esp_lcd_panel_swap_xy(panel, false);
    esp_lcd_panel_mirror(panel, true, false);
    esp_lcd_panel_disp_on_off(panel, true);
}

// ---------- Saved rotation (NVS) ----------
static void rotation_load(void) {
    nvs_handle_t h; uint8_t r = 0;
    if (nvs_open("pantalla", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "giro", &r); nvs_close(h); }
    rotation = r & 3;
}
static void rotation_save(void) {
    nvs_handle_t h;
    if (nvs_open("pantalla", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "giro", rotation); nvs_commit(h); nvs_close(h); }
}

// ---------- Saved Wi-Fi (NVS) ----------
static void creds_load(void) {
    nvs_handle_t h; size_t n;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    n = sizeof saved_ssid; if (nvs_get_str(h, "ssid", saved_ssid, &n) != ESP_OK) saved_ssid[0] = 0;
    n = sizeof saved_pass; if (nvs_get_str(h, "pass", saved_pass, &n) != ESP_OK) saved_pass[0] = 0;
    nvs_close(h);
}
static void creds_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    if (ssid) nvs_set_str(h, "ssid", ssid); else nvs_erase_key(h, "ssid");
    if (pass) nvs_set_str(h, "pass", pass); else nvs_erase_key(h, "pass");
    nvs_commit(h); nvs_close(h);
}

// ---------- Web server ----------
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hx[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(hx, NULL, 16); s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}
static void html_escape(char *out, size_t sz, const char *s) {
    size_t n = 0;
    for (; *s && n + 7 < sz; s++) {
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (r) { strcpy(out + n, r); n += strlen(r); } else out[n++] = *s;
    }
    out[n] = 0;
}

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=es><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Centauri</title><style>body{font-family:system-ui,sans-serif;background:#0d0f17;color:#e9dcc0;max-width:420px;"
    "margin:0 auto;padding:24px 16px}h1{color:#ff9a5c}label{display:block;margin:16px 0 6px}input,select,button{width:100%;"
    "box-sizing:border-box;font-size:17px;padding:10px;border-radius:8px;border:1px solid #444;background:#1b1e2b;color:#fff}"
    "button{margin-top:22px;background:#ff9a5c;color:#1b1000;border:0;font-weight:600}p{color:#b8b0a0;line-height:1.45}"
    "</style><h1>Centauri</h1>";

static esp_err_t page_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    char buf[200], esc[80];
    if (net_mode == NET_ONLINE) {
        html_escape(esc, sizeof esc, saved_ssid);
        snprintf(buf, sizeof buf, "<p>Conectado a <b>%s</b> con la direcci\xc3\xb3n <b>%s</b>.</p>", esc, net_ip);
        httpd_resp_sendstr_chunk(req, buf);
        httpd_resp_sendstr_chunk(req, "<p>Si quieres cambiar de red, escr\xc3\xad" "bela aqu\xc3\xad:</p>");
    } else {
        httpd_resp_sendstr_chunk(req, "<p>Elige tu red Wi\xe2\x80\x91" "Fi y escribe su contrase\xc3\xb1" "a. "
                                      "La pantalla se reiniciar\xc3\xa1 y se conectar\xc3\xa1 sola.</p>");
    }
    httpd_resp_sendstr_chunk(req, "<form method=post action=/wifi><label>Red Wi\xe2\x80\x91" "Fi</label>"
                                  "<input name=ssid list=redes required autocomplete=off><datalist id=redes>");
    httpd_resp_sendstr_chunk(req, scan_list);
    httpd_resp_sendstr_chunk(req, "</datalist><label>Contrase\xc3\xb1" "a</label><input name=pass type=password>"
                                  "<button>Guardar y conectar</button></form>"
                                  "<p style='margin-top:28px;font-size:14px'>Solo vale para redes de 2,4 GHz.</p>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void restart_later(void *arg) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }

static esp_err_t page_wifi(httpd_req_t *req) {
    char body[256] = "";
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) { int r = httpd_req_recv(req, body + got, len - got); if (r <= 0) break; got += r; }
    body[got] = 0;
    char ssid[33] = "", pass[65] = "";
    char *tok, *save;
    for (tok = strtok_r(body, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '='); if (!eq) continue;
        *eq = 0; url_decode(eq + 1);
        if (!strcmp(tok, "ssid")) snprintf(ssid, sizeof ssid, "%s", eq + 1);
        else if (!strcmp(tok, "pass")) snprintf(pass, sizeof pass, "%s", eq + 1);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    if (!ssid[0]) {
        httpd_resp_sendstr_chunk(req, "<p>Falta el nombre de la red. <a href=/ style='color:#ff9a5c'>Volver</a></p>");
    } else {
        creds_save(ssid, pass);
        httpd_resp_sendstr_chunk(req, "<p>Guardado. La pantalla se reinicia y se conecta a tu Wi\xe2\x80\x91" "Fi.</p>"
                                      "<p>Ya puedes volver a tu red normal. En la pantalla ver\xc3\xa1s si se ha conectado.</p>");
        xTaskCreate(restart_later, "restart", 2048, NULL, 5, NULL);
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}
static esp_err_t estado_options(httpd_req_t *req) { cors(req); httpd_resp_send(req, NULL, 0); return ESP_OK; }

static esp_err_t estado_get(httpd_req_t *req) {
    cors(req);
    httpd_resp_set_type(req, "application/json");
    char buf[96];
    snprintf(buf, sizeof buf, "{\"centauri\":true,\"projects\":%d}", n_projects);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static int state_index(const char *s) {
    for (int i = 0; i < 6; i++) if (s && !strcmp(s, STATE_KEYS[i])) return i;
    return ST_SLEEPING;
}
static void copy_str(char *dst, size_t sz, const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    snprintf(dst, sz, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static esp_err_t estado_post(httpd_req_t *req) {
    cors(req);
    int len = req->content_len;
    if (len <= 0 || len > 16384) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size"); return ESP_FAIL; }
    char *body = malloc(len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem"); return ESP_FAIL; }
    int got = 0;
    while (got < len) { int r = httpd_req_recv(req, body + got, len - got); if (r <= 0) break; got += r; }
    body[got] = 0;
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "projects") : NULL;
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }

    static project_t tmp[MAX_P];
    int n = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, arr) {
        if (n >= MAX_P) break;
        project_t *t = &tmp[n];
        memset(t, 0, sizeof *t);
        copy_str(t->name, sizeof t->name, p, "name");
        if (!t->name[0]) continue;
        copy_str(t->status, sizeof t->status, p, "status");
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(p, "state");
        t->state = state_index(cJSON_IsString(st) ? st->valuestring : NULL);
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(p, "forja");
        const cJSON *pz = f ? cJSON_GetObjectItemCaseSensitive(f, "piezas") : NULL;
        if (cJSON_IsArray(pz) && cJSON_GetArraySize(pz) > 0) {
            copy_str(t->objeto, sizeof t->objeto, f, "objeto");
            if (!t->objeto[0]) strcpy(t->objeto, "espada");
            const cJSON *q;
            cJSON_ArrayForEach(q, pz) {
                t->piezas++;
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(q, "hecha"))) t->hechas++;
            }
        }
        n++;
    }
    cJSON_Delete(root);

    xSemaphoreTake(data_lock, portMAX_DELAY);
    int64_t now_us = esp_timer_get_time();
    for (int k = 0; k < n_pend; k++) {
        bool keep = now_us < pend[k].until;
        for (int i = 0; i < n && keep; i++) {
            if (strcmp(tmp[i].name, pend[k].name)) continue;
            if (tmp[i].state == ST_HAPPY) { tmp[i].state = ST_SLEEPING; strcpy(tmp[i].status, "Recompensa recogida"); }
            else keep = false;      // the PC already has it
        }
        if (!keep) { pend[k] = pend[--n_pend]; k--; }
    }
    char reply[64 + MAX_PEND * 100] = "{\"ok\":true,\"accepted\":[";
    bool first = true;
    for (int k = 0; k < n_pend; k++) {
        if (pend[k].told) continue;
        pend[k].told = true;
        cJSON *js = cJSON_CreateString(pend[k].name);
        char *q = cJSON_PrintUnformatted(js);
        if (q) { if (!first) strcat(reply, ","); strcat(reply, q); first = false; cJSON_free(q); }
        cJSON_Delete(js);
    }
    strcat(reply, "]}");
    bool changed = n != n_projects || memcmp(tmp, projects, n * sizeof(project_t)) != 0 || last_post_us == 0;
    memcpy(projects, tmp, n * sizeof(project_t));
    n_projects = n;
    last_post_us = esp_timer_get_time();
    xSemaphoreGive(data_lock);
    if (changed) { dirty = true; last_change_us = now_us; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, reply);
    return ESP_OK;
}

static esp_err_t page_redirect(httpd_req_t *req, httpd_err_code_t err) {
    // captive portal: any other address goes to the setup page
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void web_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "web server failed"); return; }
    httpd_uri_t u[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page_root },
        { .uri = "/wifi", .method = HTTP_POST, .handler = page_wifi },
        { .uri = "/estado", .method = HTTP_GET, .handler = estado_get },
        { .uri = "/estado", .method = HTTP_POST, .handler = estado_post },
        { .uri = "/estado", .method = HTTP_OPTIONS, .handler = estado_options },
    };
    for (int i = 0; i < 5; i++) httpd_register_uri_handler(srv, &u[i]);
    if (net_mode == NET_SETUP) httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, page_redirect);
}

// ---------- Wi-Fi ----------
static EventGroupHandle_t wifi_ev;
#define GOT_IP BIT0

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (net_mode == NET_ONLINE) { net_mode = NET_CONNECTING; dirty = true; }
        xEventGroupClearBits(wifi_ev, GOT_IP);
        if (net_mode != NET_SETUP) { vTaskDelay(pdMS_TO_TICKS(1000)); esp_wifi_connect(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(net_ip, sizeof net_ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "online at %s", net_ip);
        net_mode = NET_ONLINE; dirty = true;
        xEventGroupSetBits(wifi_ev, GOT_IP);
    }
}

static void scan_networks(void) {
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof *recs);
    if (!recs) return;
    esp_wifi_scan_get_ap_records(&n, recs);
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) if (!strcmp((char *)recs[j].ssid, (char *)recs[i].ssid)) dup = true;
        if (dup) continue;
        char esc[80], opt[100];
        html_escape(esc, sizeof esc, (char *)recs[i].ssid);
        int k = snprintf(opt, sizeof opt, "<option value=\"%s\">", esc);
        if (used + k >= sizeof scan_list) break;
        memcpy(scan_list + used, opt, k + 1); used += k;
    }
    free(recs);
}

// Tiny DNS server for setup mode: answers every name with 192.168.4.1, so phones see a
// "sign in to network" page that opens the setup form by themselves.
static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof a) < 0) { vTaskDelete(NULL); return; }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (buf[2] & 0x80)) continue;
        int q = 12;                                   // skip the question name
        while (q < n && buf[q]) q += buf[q] + 1;
        q += 5;                                       // end of name + type + class
        if (q > n) continue;
        bool is_a = buf[q - 4] == 0 && buf[q - 3] == 1;
        buf[2] = 0x84; buf[3] = 0x00;                 // response, authoritative, no error
        buf[6] = 0; buf[7] = is_a ? 1 : 0;            // one answer (only for A questions)
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = q;
        if (is_a) {
            const uint8_t ans[16] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + q, ans, sizeof ans); len += sizeof ans;
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

static int64_t setup_since;
static void start_setup_ap(void) {
    setup_since = esp_timer_get_time();
    ESP_LOGI(TAG, "setup mode: network %s", AP_SSID);
    net_mode = NET_SETUP; dirty = true;
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap = { .ap = { .ssid = AP_SSID, .ssid_len = strlen(AP_SSID), .channel = 1,
                                 .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    wifi_config_t empty = { 0 };
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    esp_wifi_start();
    scan_networks();
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
}

static void wifi_init(void) {
    wifi_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta, HOSTNAME);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);

    creds_load();
    if (saved_ssid[0]) {
        net_mode = NET_CONNECTING; dirty = true;
        wifi_config_t sc = { 0 };
        memcpy(sc.sta.ssid, saved_ssid, strnlen(saved_ssid, sizeof sc.sta.ssid));
        memcpy(sc.sta.password, saved_pass, strnlen(saved_pass, sizeof sc.sta.password));
        sc.sta.threshold.authmode = saved_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sc);
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (!(xEventGroupWaitBits(wifi_ev, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(40000)) & GOT_IP))
            start_setup_ap();
    } else {
        start_setup_ap();
    }
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_instance_name_set("Centauri");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    web_start();
}

static void wifi_init_task(void *arg) { wifi_init(); vTaskDelete(NULL); }

// ---------- Screen ----------
static int sel = 0;
static float dial_rot = 0;    // degrees the dial is turned (eases towards sel)
static int64_t hold_since = 0;

static uint8_t C_BG, C_TEXT, C_DIM, C_GOLD, C_TRACK;

static void draw_message(const char *title, const char *l1, const char *l2, const char *l3, uint8_t col) {
    ring(110, 119, col);
    text_c(52, "CENTAURI", 2, C_TEXT);
    sprite(ST_SLEEPING, (W - SPR_W * 2) / 2, 66, 2);
    text_c(162, title, 2, col);
    if (l1) text_c(182, l1, 1, C_TEXT);
    if (l2) text_c(196, l2, 1, C_GOLD);
    if (l3) text_c(210, l3, 1, C_DIM);
}

static void draw(void) {
    for (int i = 0; i < W * H; i++) fb[i] = C_BG;

    if (hold_since && esp_timer_get_time() - hold_since > 2000000) {
        int left = 6 - (int)((esp_timer_get_time() - hold_since) / 1000000);
        char b[48]; snprintf(b, sizeof b, "o sigue %d s para olvidar", left < 0 ? 0 : left);
        draw_message("Suelta para girar", b, "el Wi-Fi guardado", NULL, state_color[ST_THINKING]);
        return;
    }
    if (net_mode == NET_SETUP) {
        if (saved_ssid[0]) {
            // a network is saved but wasn't found: say so, so it doesn't look forgotten
            char b[48]; fit(b, sizeof b, saved_ssid, 170, 1);
            draw_message("No encuentro el Wi-Fi", b, "Lo reintento cada minuto", "o cambia la red en 192.168.4.1", state_color[ST_WAITING]);
        } else
            draw_message("Configura el Wi-Fi", "Con\xc3\xa9" "ctate a la red", AP_SSID, "y abre 192.168.4.1", state_color[ST_WAITING]);
        return;
    }
    if (net_mode == NET_STARTING || net_mode == NET_CONNECTING) {
        char b[48]; fit(b, sizeof b, saved_ssid, 170, 1);
        draw_message("Conectando\xe2\x80\xa6", b, NULL, NULL, state_color[ST_THINKING]);
        return;
    }

    xSemaphoreTake(data_lock, portMAX_DELAY);
    int n = n_projects;
    bool fresh = last_post_us && esp_timer_get_time() - last_post_us < 90LL * 1000000;
    if (!n || !last_post_us) {
        xSemaphoreGive(data_lock);
        draw_message("Esperando al PC", "Abre la mascota en tu PC", "centauri.local", net_ip, state_color[ST_SLEEPING]);
        return;
    }
    if (sel >= n) sel = n - 1;
    project_t p = projects[sel];
    int gem_state[MAX_P];
    for (int i = 0; i < n; i++) gem_state[i] = projects[i].state;
    xSemaphoreGive(data_lock);

    uint8_t col = state_color[p.state];
    ring(111, 118, col);

    // the missions sit around the ring like gems on a dial; the selected one turns to the top
    if (n > 1) {
        float step = 360.0f / n;
        for (int i = 0; i < n; i++) {
            float a = (i * step - dial_rot) * 0.01745329f;
            int gx = W / 2 + (int)lroundf(sinf(a) * 114), gy = H / 2 - (int)lroundf(cosf(a) * 114);
            bool on = i == sel;
            disc(gx, gy, on ? 9 : 7, C_BG);
            if (on) disc(gx, gy, 8, C_TEXT);
            disc(gx, gy, on ? 6 : 5, state_color[gem_state[i]]);
        }
    }

    // forge progress: a golden arc inside the ring, clockwise from the top
    if (p.piezas > 0) {
        float frac = (float)p.hechas / p.piezas;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int dx = x - W / 2, dy = y - H / 2, d2 = dx * dx + dy * dy;
            if (d2 < 101 * 101 || d2 >= 106 * 106) continue;
            float a = atan2f(dx, -dy); if (a < 0) a += 6.2831853f;
            fb[y * W + x] = a <= frac * 6.2831853f ? C_GOLD : C_TRACK;
        }
    }

    char b[100];
    int sc = text_width(p.name, 2) <= 140 ? 2 : 1;
    fit(b, sizeof b, p.name, 140, sc);
    text_c(sc == 2 ? 52 : 48, b, sc, C_TEXT);

    // a little life: the centaur breathes (slowly when asleep) and sleeping missions snore
    int64_t now_us = esp_timer_get_time();
    float ph = (now_us % 1600000) / 1600000.0f * 6.2831853f;
    if (p.state == ST_SLEEPING) ph = (now_us % 3200000) / 3200000.0f * 6.2831853f;
    int bob = (int)lroundf(sinf(ph) * (p.state == ST_SLEEPING ? 1.0f : 2.0f));
    if (p.state == ST_WORKING || p.state == ST_HAPPY) bob = (int)lroundf(fabsf(sinf(ph)) * -3.0f);
    sprite(p.state, (W - SPR_W * 3) / 2, 60 + bob, 3);
    if (p.state == ST_SLEEPING) {
        int k = (now_us / 600000) % 3;
        for (int i = 0; i <= k; i++) text(164 + i * 9, 84 - i * 10, "z", 1 + (i == 2), C_DIM);
    }
    if (reward_flash_us && now_us - reward_flash_us < 1800000) {
        // golden sparkles around the centaur
        static const int8_t SP[8][2] = { {-70,-10},{70,-18},{-58,40},{62,34},{-30,-36},{34,-40},{-76,18},{78,10} };
        int f = (int)((now_us - reward_flash_us) / 150000);
        for (int i = 0; i < 8; i++) {
            if ((i + f) % 3 == 0) continue;
            int cx = W / 2 + SP[i][0], cy = 118 + SP[i][1], r = 2 + (i + f) % 3;
            rect(cx - r, cy, 2 * r + 1, 1, C_GOLD); rect(cx, cy - r, 1, 2 * r + 1, C_GOLD);
        }
    }

    if (p.piezas > 0 && p.state != ST_HAPPY)
        snprintf(b, sizeof b, "%s %d/%d", STATE_LABEL[p.state], p.hechas, p.piezas);
    else
        snprintf(b, sizeof b, "%s", STATE_LABEL[p.state]);
    text_c(192, b, 2, col);
    if (p.state == ST_HAPPY) {
        if (fresh) { fit(b, sizeof b, p.status, 120, 1); text_c(207, b, 1, C_TEXT); }
        text_c(221, "Toca para recoger", 1, C_GOLD);
        return;
    }

    if (fresh) text_wrap2(207, p.status, 120, 1, C_TEXT);
    else text_c(207, "Sin noticias del PC", 1, C_DIM);
}

void app_main(void) {
    state_color[ST_SLEEPING] = rgb(0x8f, 0xa0, 0xbf);
    state_color[ST_THINKING] = rgb(0xb3, 0xa4, 0xff);
    state_color[ST_WORKING]  = rgb(0xff, 0x9a, 0x5c);
    state_color[ST_WAITING]  = rgb(0xf6, 0xc9, 0x45);
    state_color[ST_HAPPY]    = rgb(0x7d, 0xdc, 0x8f);
    state_color[ST_WORRIED]  = rgb(0xff, 0x64, 0x64);
    C_BG = rgb(13, 15, 23); C_TEXT = rgb(233, 220, 192); C_DIM = rgb(143, 138, 153); C_GOLD = rgb(0xf6, 0xc9, 0x45); C_TRACK = rgb(40, 38, 48);
    for (int i = 0; i < 32; i++) pal[i] = SPR_PALETTE[i];

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }

    data_lock = xSemaphoreCreateMutex();
    flush_done = xSemaphoreCreateBinary();
    band_buf = heap_caps_malloc(W * BAND * 2, MALLOC_CAP_DMA);
    lcd_init();
    touch_init();
    rotation_load();
    creds_load();
    if (saved_ssid[0]) net_mode = NET_CONNECTING;
    draw(); flush();
    bl_set(255);
    dirty = false;

    xTaskCreate((TaskFunction_t)wifi_init_task, "wifi", 6144, NULL, 5, NULL);

    bool down = false; int sx = 0, sy = 0, lx = 0, ly = 0;
    int64_t last_tick = 0, hold_tick = 0, anim_tick = 0;
    for (;;) {
        int x, y;
        bool now = touch_read(&x, &y);
        if (now) { int px = x, py = y; rot_src(px, py, &x, &y); }   // touch in drawn coordinates
        int64_t t = esp_timer_get_time();
        if (now) {
            if (!down) { sx = x; sy = y; hold_since = t; last_touch_us = t; if (bl_now < 255) { bl_set(255); dirty = true; } }
            lx = x; ly = y;
            if (abs(lx - sx) > 20 || abs(ly - sy) > 20) hold_since = 0;
            if (hold_since && t - hold_since > 6000000) {
                creds_save(NULL, NULL);
                for (int i = 0; i < W * H; i++) fb[i] = C_BG;
                draw_message("Wi-Fi olvidado", "Reiniciando\xe2\x80\xa6", NULL, NULL, state_color[ST_WORRIED]);
                flush();
                vTaskDelay(pdMS_TO_TICKS(1200));
                esp_restart();
            }
        } else if (down) {
            bool was_hold = hold_since && t - hold_since > 2000000;
            hold_since = 0;
            int dx = lx - sx, n = n_projects;
            if (was_hold) {
                // hold 2-6 s and let go: turn the picture a quarter turn (saved for next time)
                rotation = (rotation + 1) & 3;
                rotation_save();
                dirty = true;
            }
            else if (n > 1 && dx > 40) { sel = (sel + n - 1) % n; dirty = true; }    // swipe right: previous mission
            else if (n > 1 && dx < -40) { sel = (sel + 1) % n; dirty = true; }       // swipe left: next mission
            else if (abs(dx) < 25 && abs(ly - sy) < 25 && n > 0) {                   // tap
                xSemaphoreTake(data_lock, portMAX_DELAY);
                if (sel < n_projects && projects[sel].state == ST_HAPPY) {
                    // collect the reward: asleep here now, and tell the PC
                    project_t *pp = &projects[sel];
                    pp->state = ST_SLEEPING; strcpy(pp->status, "Recompensa recogida");
                    if (n_pend < MAX_PEND) {
                        pending_t *q = &pend[n_pend++];
                        snprintf(q->name, sizeof q->name, "%s", pp->name);
                        q->told = false; q->until = t + 120000000LL;
                    }
                    reward_flash_us = t;
                } else if (n > 1) sel = (sel + 1) % n;                                // tap elsewhere: next mission
                xSemaphoreGive(data_lock);
                dirty = true;
            }
        }
        down = now;
        if (hold_since && t - hold_since > 2000000 && t - hold_tick > 500000) { hold_tick = t; dirty = true; }
        // saved Wi-Fi was not there at start-up (router off?): try it again every minute
        // while nobody is using the setup page
        if (net_mode == NET_SETUP && saved_ssid[0] && t - setup_since > 60000000LL) {
            wifi_sta_list_t sl;
            if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK && sl.num == 0) esp_restart();
            setup_since = t;
        }
        if (t - last_tick > 5000000) { last_tick = t; dirty = true; }   // catches "no news from the PC"

        // animation frames while missions are on screen and the screen is bright
        bool showing = net_mode == NET_ONLINE && n_projects > 0 && last_post_us;
        if (showing && bl_now == 255 && t - anim_tick > 200000) { anim_tick = t; dirty = true; }

        // dim the screen when everything is asleep and nobody has touched it for a minute
        bool calm = net_mode == NET_ONLINE;
        xSemaphoreTake(data_lock, portMAX_DELAY);
        for (int i = 0; i < n_projects; i++) if (projects[i].state != ST_SLEEPING) calm = false;
        xSemaphoreGive(data_lock);
        int64_t quiet = t - (last_touch_us > last_change_us ? last_touch_us : last_change_us);
        bl_set(calm && quiet > 60000000LL ? 20 : 255);
        // turn the dial smoothly towards the selected mission (shortest way round)
        if (n_projects > 1) {
            float step = 360.0f / n_projects, target = sel * step;
            float d = fmodf(target - dial_rot + 540.0f, 360.0f) - 180.0f;
            if (fabsf(d) > 0.5f) { dial_rot += d * 0.35f; dirty = true; }
            else if (dial_rot != target) { dial_rot = target; dirty = true; }
        }
        if (dirty) { dirty = false; draw(); flush(); }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
