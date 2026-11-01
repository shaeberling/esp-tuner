/*
 * LVGL on the SPD2010 panel and its touch controller. See display.h.
 *
 * LVGL renders in partial mode into two internal-RAM DMA buffers, 40 rows
 * each: while one goes out over QSPI (40 MHz, 4 lines), LVGL draws the
 * next. The panel takes big-endian RGB565 and update windows that start and
 * end on 4-column boundaries.
 *
 * Everything that touches the panel's SPI bus runs on core 0: app_main sets
 * the bus up there, so its interrupt is there too, and the LVGL task is
 * pinned there (the Muse SDK found ESP-IDF's SPI bus lock can lose a waiter
 * when the interrupt finishing a transfer is on the other core).
 */
#include "display.h"

#include <stdlib.h>

#include "board.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "display";

#define RES BOARD_LCD_RES
#define DRAW_LINES 40
#define DRAW_BUF_BYTES (RES * DRAW_LINES * 2)
#define LVGL_CORE 0
#define LVGL_PRIO 4
#define TOUCH_CORE 1
#define TP_LIFT_MS 40      /* reads empty this long before the finger counts as lifted */
#define TAP_MAX_MS 700
#define TAP_MAX_MOVE 40

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static SemaphoreHandle_t s_lock;
static int s_taps;   /* atomic */
/* Render statistics since the last display_stats(). */
static uint32_t s_frames, s_busy_us, s_worst_us, s_flushed_px;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    s_flushed_px += lv_area_get_size(a);
    lv_draw_sw_rgb565_swap(px, lv_area_get_size(a));
    esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

/* The SPD2010 needs update windows on 4-column boundaries. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~3;
    a->x2 |= 3;
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_render_done(lv_event_t *e)
{
    s_frames++;
}

void display_stats(display_stats_t *out, int64_t *since)
{
    const int64_t now = esp_timer_get_time();
    const float secs = (now - *since) / 1e6f;
    out->fps = secs > 0 ? s_frames / secs : 0;
    out->busy_pct = secs > 0 ? s_busy_us / (secs * 1e4f) : 0;
    out->worst_ms = s_worst_us / 1000.0f;
    out->kpx_per_frame = s_frames ? s_flushed_px / 1000.0f / s_frames : 0;
    s_frames = s_busy_us = s_worst_us = s_flushed_px = 0;
    *since = now;
}

static void lvgl_task(void *arg)
{
    for (;;) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
        const int64_t t0 = esp_timer_get_time();
        uint32_t wait = lv_timer_handler();
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        xSemaphoreGiveRecursive(s_lock);
        s_busy_us += dt;
        s_worst_us = dt > s_worst_us ? dt : s_worst_us;
        if (wait > 20) {
            wait = 20;
        } else if (wait < 2) {
            wait = 2;
        }
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

bool display_lock(int timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

int display_take_taps(void)
{
    return __atomic_exchange_n(&s_taps, 0, __ATOMIC_RELAXED);
}

/* ---- touch ---------------------------------------------------------------- */

/* The touch driver writes the register address, then reads with an empty
 * command. IDF 6 rejects that as a zero-length write; -1 makes it a plain
 * read (found by the Muse SDK). */
static esp_err_t (*s_tp_rx)(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size);

static esp_err_t tp_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size)
{
    return s_tp_rx(io, -1, param, param_size);
}

/*
 * The SPD2010 has no interrupt line here, so it's polled, and now and then a
 * poll finds no report though the finger is still down: the last point
 * stands until reads have come back empty for TP_LIFT_MS. A down and up
 * that's short and doesn't move is a tap.
 */
static void touch_task(void *arg)
{
    esp_lcd_touch_handle_t tp = arg;
    int64_t last_us = 0, down_us = 0;
    int x0 = 0, y0 = 0, moved = 0;
    for (;;) {
        esp_lcd_touch_point_data_t p;
        uint8_t n = 0;
        if (esp_lcd_touch_read_data(tp) == ESP_OK) {
            esp_lcd_touch_get_data(tp, &p, &n, 1);
        }
        const int64_t now = esp_timer_get_time();
        if (n) {
            if (!last_us) {
                down_us = now;
                x0 = p.x;
                y0 = p.y;
                moved = 0;
            } else {
                const int d = abs(p.x - x0) + abs(p.y - y0);
                moved = d > moved ? d : moved;
            }
            last_us = now;
        } else if (last_us && now - last_us >= TP_LIFT_MS * 1000) {
            if ((last_us - down_us) / 1000 < TAP_MAX_MS && moved < TAP_MAX_MOVE) {
                __atomic_fetch_add(&s_taps, 1, __ATOMIC_RELAXED);
            }
            last_us = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(last_us ? 10 : 25));
    }
}

static bool touch_init(void)
{
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = BOARD_TP_SDA,
        .scl_io_num = BOARD_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        return false;
    }
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK) {
        return false;
    }
    s_tp_rx = io->rx_param;
    io->rx_param = tp_rx_param;
    const esp_lcd_touch_config_t cfg = {
        .x_max = RES,
        .y_max = RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    esp_lcd_touch_handle_t tp;
    if (esp_lcd_touch_new_i2c_spd2010(io, &cfg, &tp) != ESP_OK) {
        return false;
    }
    /* Seeed reads once to start the controller. */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(tp);
    return xTaskCreatePinnedToCore(touch_task, "touch", 3072, tp, 3, NULL, TOUCH_CORE) == pdPASS;
}

/* ---- panel and LVGL ------------------------------------------------------- */

bool display_init(void)
{
    const spi_bus_config_t bus = SPD2010_PANEL_BUS_QSPI_CONFIG(BOARD_LCD_PCLK, BOARD_LCD_D0, BOARD_LCD_D1,
                                                               BOARD_LCD_D2, BOARD_LCD_D3, DRAW_BUF_BYTES);
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(RES, RES);

    esp_lcd_panel_io_spi_config_t io_cfg = SPD2010_PANEL_IO_QSPI_CONFIG(BOARD_LCD_CS, on_color_done, s_disp);
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(BOARD_LCD_HOST, &io_cfg, &s_io));
    spd2010_vendor_config_t vendor = { .flags.use_qspi_interface = 1 };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    void *buf1 = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "no internal RAM for the draw buffers");
        return false;
    }
    lv_display_set_buffers(s_disp, buf1, buf2, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush);
    lv_display_add_event_cb(s_disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(s_disp, on_render_done, LV_EVENT_RENDER_READY, NULL);

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock || xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, LVGL_PRIO, NULL, LVGL_CORE) != pdPASS) {
        return false;
    }
    if (!touch_init()) {
        ESP_LOGW(TAG, "touch didn't start; the wheel still works");
    }
    ESP_LOGI(TAG, "panel %dx%d up, free internal RAM %u", RES, RES,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}
