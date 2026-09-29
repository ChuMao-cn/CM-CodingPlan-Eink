#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1681.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "font5x7.h"
#include "font_cn16x16.h"

#define WIFI_SSID CONFIG_ESP_WIFI_SSID
#define WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define DATA_URL CONFIG_DATA_URL

#define EPD_PIN_SCLK 13
#define EPD_PIN_MOSI 14
#define EPD_PIN_CS 15
#define EPD_PIN_DC 27
#define EPD_PIN_RST 26
#define EPD_PIN_BUSY 25

#define EPD_WIDTH 200
#define EPD_HEIGHT 200
#define EPD_BUFFER_SIZE (EPD_WIDTH * EPD_HEIGHT / 8)
#define REFRESH_S 600

static const char *TAG = "coding_plan_epd";
static EventGroupHandle_t wifi_events;
static SemaphoreHandle_t epaper_semaphore;
static esp_lcd_panel_handle_t panel_handle;
static uint8_t *frame_buffer;

static void set_pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    frame_buffer[y * (EPD_WIDTH / 8) + x / 8] |= 0x80 >> (x % 8);
}

static void draw_bitmap16(int x, int y, const uint16_t *image, int width, int height)
{
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            if (image[row] & (0x8000 >> col)) {
                set_pixel(x + col, y + row);
            }
        }
    }
}

static const font_cn16x16_glyph_t *find_cn_glyph(uint32_t codepoint)
{
    for (size_t i = 0; i < sizeof(font_cn16x16_glyphs) / sizeof(font_cn16x16_glyphs[0]); i++) {
        if (font_cn16x16_glyphs[i].codepoint == codepoint) {
            return &font_cn16x16_glyphs[i];
        }
    }
    return NULL;
}

static uint32_t utf8_decode(const char **text)
{
    const unsigned char *bytes = (const unsigned char *)(*text);
    uint32_t codepoint = 0xFFFD;
    if (bytes[0] < 0x80) {
        codepoint = bytes[0];
        *text += 1;
    } else if ((bytes[0] & 0xE0) == 0xC0) {
        codepoint = ((uint32_t)(bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F);
        *text += 2;
    } else if ((bytes[0] & 0xF0) == 0xE0) {
        codepoint = ((uint32_t)(bytes[0] & 0x0F) << 12) |
                    ((uint32_t)(bytes[1] & 0x3F) << 6) |
                    (bytes[2] & 0x3F);
        *text += 3;
    } else {
        *text += 1;
    }
    return codepoint;
}

static int text_width(const char *text, int scale)
{
    return strlen(text) * (5 + 1) * scale - scale;
}

static void draw_hline(int x, int y, int width)
{
    for (int col = 0; col < width; col++) set_pixel(x + col, y);
}

static void draw_vline(int x, int y, int height)
{
    for (int row = 0; row < height; row++) set_pixel(x, y + row);
}

static void draw_rect_outline(int x, int y, int width, int height)
{
    draw_hline(x, y, width);
    draw_hline(x, y + height - 1, width);
    draw_vline(x, y, height);
    draw_vline(x + width - 1, y, height);
}

static void draw_filled_rect(int x, int y, int width, int height)
{
    for (int row = 0; row < height; row++) draw_hline(x, y + row, width);
}

static void draw_char(int x, int y, char ch, int scale)
{
    const uint8_t *glyph = NULL;
    for (size_t i = 0; i < sizeof(font5x7_glyphs) / sizeof(font5x7_glyphs[0]); i++) {
        if (font5x7_glyphs[i].ch == ch) {
            glyph = font5x7_glyphs[i].glyph;
            break;
        }
    }
    if (!glyph) return;
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (glyph[col] & (1 << row)) {
                for (int sx = 0; sx < scale; sx++) {
                    for (int sy = 0; sy < scale; sy++) {
                        set_pixel(x + col * scale + sx, y + row * scale + sy);
                    }
                }
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale)
{
    int cursor = x;
    for (const char *p = text; *p; p++) {
        draw_char(cursor, y, *p, scale);
        cursor += (5 + 1) * scale;
    }
}

static void draw_text_centered(int y, const char *text, int scale)
{
    draw_text((EPD_WIDTH - text_width(text, scale)) / 2, y, text, scale);
}

static void draw_text_right(int x_right, int y, const char *text, int scale)
{
    draw_text(x_right - text_width(text, scale), y, text, scale);
}

static int mixed_text_width(const char *text, int ascii_scale)
{
    int width = 0;
    for (const char *p = text; *p;) {
        uint32_t codepoint = utf8_decode(&p);
        width += codepoint < 0x80 ? 6 * ascii_scale : 18;
    }
    return width > 0 ? width - 2 : 0;
}

static void draw_text_mixed(int x, int y, const char *text, int ascii_scale)
{
    int cursor = x;
    for (const char *p = text; *p;) {
        uint32_t codepoint = utf8_decode(&p);
        if (codepoint < 0x80) {
            draw_char(cursor, y + (16 - 7 * ascii_scale), (char)codepoint, ascii_scale);
            cursor += 6 * ascii_scale;
        } else {
            const font_cn16x16_glyph_t *glyph = find_cn_glyph(codepoint);
            if (glyph) draw_bitmap16(cursor, y, glyph->rows, 16, 16);
            cursor += 18;
        }
    }
}

static void draw_text_mixed_centered(int y, const char *text, int ascii_scale)
{
    draw_text_mixed((EPD_WIDTH - mixed_text_width(text, ascii_scale)) / 2, y, text, ascii_scale);
}

static bool wait_epaper_done(void)
{
    return xSemaphoreTake(epaper_semaphore, pdMS_TO_TICKS(15000)) == pdTRUE;
}

static bool epaper_refresh_done_cb(const esp_lcd_panel_handle_t handle, const void *edata, void *user_data)
{
    (void)handle;
    (void)edata;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(epaper_semaphore, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
        return true;
    }
    return false;
}

static void epaper_init(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = EPD_PIN_SCLK,
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUFFER_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EPD_PIN_DC,
        .cs_gpio_num = EPD_PIN_CS,
        .pclk_hz = 1000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_ssd1681_config_t ssd1681_config = {
        .busy_gpio_num = EPD_PIN_BUSY,
        .non_copy_mode = false,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EPD_PIN_RST,
        .flags.reset_active_high = false,
        .vendor_config = &ssd1681_config,
    };
    gpio_install_isr_service(0);
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1681(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    epaper_semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(epaper_semaphore);
    epaper_panel_callbacks_t callbacks = {
        .on_epaper_refresh_done = epaper_refresh_done_cb,
    };
    ESP_ERROR_CHECK(epaper_panel_register_event_callbacks(panel_handle, &callbacks, NULL));
}

static void epaper_show(void)
{
    static uint8_t *empty_buffer;
    if (!empty_buffer) {
        empty_buffer = heap_caps_malloc(EPD_BUFFER_SIZE, MALLOC_CAP_8BIT);
        memset(empty_buffer, 0, EPD_BUFFER_SIZE);
    }
    ESP_LOGI(TAG, "EPD refresh start");
    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_RED);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, EPD_WIDTH, EPD_HEIGHT, empty_buffer));
    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_BLACK);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, EPD_WIDTH, EPD_HEIGHT, frame_buffer));
    ESP_ERROR_CHECK(epaper_panel_refresh_screen(panel_handle));
    bool done = wait_epaper_done();
    ESP_LOGI(TAG, "EPD refresh %s", done ? "done" : "timeout");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, BIT0);
    }
}

static bool wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    for (int attempt = 1; attempt <= 3; attempt++) {
    ESP_ERROR_CHECK(esp_wifi_start());
        EventBits_t bits = xEventGroupWaitBits(wifi_events, BIT0, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(8 * 1000));
        if (bits & BIT0) {
            ESP_LOGI(TAG, "WiFi ready after attempt %d", attempt);
            return true;
        }
        ESP_LOGW(TAG, "WiFi attempt %d failed", attempt);
        ESP_ERROR_CHECK(esp_wifi_stop());
    }
    return false;
}

static bool is_sleep_time(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    return tm_info.tm_hour >= 19 || tm_info.tm_hour < 10;
}

static bool sync_time(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    for (int i = 0; i < 100; i++) {
        if (time(NULL) >= 1700000000) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    bool synced = time(NULL) >= 1700000000;
    ESP_LOGI(TAG, "NTP %s", synced ? "synced" : "sync timeout");
    return synced;
}

static void deep_sleep_until_work_start(void)
{
    time_t now = time(NULL);
    struct tm target;
    localtime_r(&now, &target);
    if (target.tm_hour >= 19) target.tm_mday++;
    target.tm_hour = 10;
    target.tm_min = 0;
    target.tm_sec = 0;

    time_t target_time = mktime(&target);
    uint64_t seconds = (uint64_t)difftime(target_time, now);
    ESP_LOGI(TAG, "Deep sleeping %llu seconds until 10:00", (unsigned long long)seconds);
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_deep_sleep_start();
}

static char *fetch_display_json(void)
{
    static char response[1024];
    memset(response, 0, sizeof(response));
    esp_http_client_config_t config = {
        .url = DATA_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    if (esp_http_client_open(client, 0) == ESP_OK &&
        esp_http_client_fetch_headers(client) >= 0) {
        int read_len = esp_http_client_read_response(client, response, sizeof(response) - 1);
        if (read_len <= 0 || esp_http_client_get_status_code(client) != 200) {
            memset(response, 0, sizeof(response));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return response[0] ? response : NULL;
}

static void copy_json_string(cJSON *root, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    const char *value = cJSON_IsString(item) ? item->valuestring : "--";
    strncpy(out, value, out_len - 1);
    out[out_len - 1] = '\0';
}

static bool draw_usage(void)
{
    char *json = fetch_display_json();
    char five[32] = "--";
    char week[32] = "--";
    char month[32] = "--";
    char reset[64] = "--";
    char updated[16] = "--";
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (root) {
        copy_json_string(root, "five_hour", five, sizeof(five));
        copy_json_string(root, "one_week", week, sizeof(week));
        copy_json_string(root, "one_month", month, sizeof(month));
        copy_json_string(root, "reset", reset, sizeof(reset));
        copy_json_string(root, "updated", updated, sizeof(updated));
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Usage updated: 5h=%s week=%s month=%s reset=%s updated=%s", five, week, month, reset, updated);
    } else {
        ESP_LOGE(TAG, "Fetch/parse failed");
        return false;
    }

    memset(frame_buffer, 0, EPD_BUFFER_SIZE);
    draw_text_centered(10, "CODING PLAN", 2);
    draw_text_centered(11, "CODING PLAN", 2);
    char updated_line[32];
    snprintf(updated_line, sizeof(updated_line), "%s 更新", updated);
    draw_text_mixed_centered(34, updated_line, 1);

    const char *labels[] = {"5小时", "一周", "一月"};
    const char *values[] = {five, week, month};
    int row_y = 58;
    for (int row = 0; row < 3; row++) {
        draw_text_mixed(12, row_y, labels[row], 2);
        draw_text_right(188, row_y + 2, values[row], 2);
        draw_rect_outline(12, row_y + 20, 176, 10);
        float percent = strtof(values[row], NULL);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        int filled = (int)(172 * percent / 100 + 0.5f);
        if (filled > 0) draw_filled_rect(14, row_y + 22, filled, 6);
        row_y += 42;
    }

    char reset_line[128];
    snprintf(reset_line, sizeof(reset_line), "%s后重置", reset);
    draw_text_mixed_centered(176, reset_line, 2);
    epaper_show();
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting");
    frame_buffer = heap_caps_malloc(EPD_BUFFER_SIZE, MALLOC_CAP_8BIT);
    memset(frame_buffer, 0, EPD_BUFFER_SIZE);
    bool wifi_ready = wifi_init();
    if (!wifi_ready) {
        ESP_LOGW(TAG, "WiFi not ready, keeping last display");
    }

    bool time_ready = sync_time();
    if (time_ready && is_sleep_time()) {
        ESP_LOGI(TAG, "Sleep window 19:00-10:00, sleeping until work start");
        deep_sleep_until_work_start();
    }

    epaper_init();
    while (true) {
        if (is_sleep_time()) {
            ESP_LOGI(TAG, "Work window ended, sleeping until 10:00");
            deep_sleep_until_work_start();
        }
        ESP_LOGI(TAG, "Fetching usage");
        draw_usage();
        ESP_LOGI(TAG, "Waiting %d seconds until next refresh", REFRESH_S);
        vTaskDelay(pdMS_TO_TICKS(REFRESH_S * 1000));
    }
}
