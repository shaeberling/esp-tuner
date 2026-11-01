/*
 * SenseCAP Watcher board support. See board.h.
 *
 * Expander (PCA9535 at 0x21 on I2C0, SDA 47 / SCL 48): port 0 inputs (the
 * wheel's push on bit 3, USB power on bit 2, charge state on bit 1), port 1
 * outputs switching the rails. The wheel's push powers the board on from
 * battery; the firmware then holds the system rail. Wheel encoder on GPIO
 * 41/42, battery through a 62k/20k divider on GPIO3 (ADC1 channel 2).
 */
#include "board.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define I2C_SDA GPIO_NUM_47
#define I2C_SCL GPIO_NUM_48
#define KNOB_A GPIO_NUM_41
#define KNOB_B GPIO_NUM_42
#define BATT_ADC ADC_CHANNEL_2

#define EXP_ADDR 0x21
#define EXP_INT GPIO_NUM_2          /* low when an input changes */
#define EXP_REG_INPUT 0x00
#define EXP_REG_OUTPUT 0x02
#define EXP_REG_CONFIG 0x06         /* 1 = input */
#define EXP_INPUTS 0x20FF
#define EXP_STDBY BIT(1)            /* low once charged */
#define EXP_VBUS BIT(2)             /* low on USB power */
#define EXP_WHEEL BIT(3)            /* low while pressed */
#define EXP_PWR_LCD BIT(9)          /* LCD and touch */
#define EXP_PWR_SYSTEM BIT(10)      /* holds the board on from battery */
#define EXP_PWR_AI BIT(11)          /* Himax camera: stays off */
#define EXP_PWR_CODEC_PA BIT(12)    /* speaker amp; the codecs have an always-on rail */
#define EXP_PWR_BAT_ADC BIT(15)

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exp;
static uint16_t s_exp_out;
static SemaphoreHandle_t s_exp_lock;
static pcnt_unit_handle_t s_knob;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

static esp_err_t exp_write(uint8_t reg, uint16_t v)
{
    const uint8_t buf[] = { reg, v & 0xff, v >> 8 };
    return i2c_master_transmit(s_exp, buf, sizeof(buf), 50);
}

static esp_err_t exp_read(uint16_t *v)
{
    const uint8_t reg = EXP_REG_INPUT;
    uint8_t buf[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_exp, &reg, 1, buf, sizeof(buf), 50), TAG, "expander read");
    *v = buf[0] | buf[1] << 8;
    return ESP_OK;
}

static esp_err_t exp_set(uint16_t mask, bool on)
{
    xSemaphoreTake(s_exp_lock, portMAX_DELAY);
    s_exp_out = on ? s_exp_out | mask : s_exp_out & ~mask;
    esp_err_t err = exp_write(EXP_REG_OUTPUT, s_exp_out);
    xSemaphoreGive(s_exp_lock);
    return err;
}

/* Deep sleep until an expander input changes; the wheel's push is one. */
static void sleep_until_wheel(void)
{
    uint16_t in;
    exp_read(&in);  /* clears the interrupt */
    rtc_gpio_pullup_en(EXP_INT);
    rtc_gpio_pulldown_dis(EXP_INT);
    esp_sleep_enable_ext0_wakeup(EXP_INT, 0);
    esp_deep_sleep_start();
}

static esp_err_t knob_init(void)
{
    const pcnt_unit_config_t unit_cfg = { .low_limit = -1000, .high_limit = 1000 };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &s_knob), TAG, "pcnt");
    const pcnt_glitch_filter_config_t filter = { .max_glitch_ns = 1000 };
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_knob, &filter), TAG, "pcnt filter");
    /* Full quadrature: both edges of both lines. */
    pcnt_channel_handle_t a, b;
    const pcnt_chan_config_t a_cfg = { .edge_gpio_num = KNOB_A, .level_gpio_num = KNOB_B };
    const pcnt_chan_config_t b_cfg = { .edge_gpio_num = KNOB_B, .level_gpio_num = KNOB_A };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_knob, &a_cfg, &a), TAG, "pcnt a");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_knob, &b_cfg, &b), TAG, "pcnt b");
    pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    gpio_pullup_en(KNOB_A);
    gpio_pullup_en(KNOB_B);
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_knob), TAG, "pcnt enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_knob), TAG, "pcnt clear");
    return pcnt_unit_start(s_knob);
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t ch = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    return ledc_channel_config(&ch);
}

esp_err_t board_init(void)
{
    /* Hold the LCD and touch lines low until the LCD rail is up, as Seeed's BSP does. */
    const gpio_config_t lcd_pins = {
        .pin_bit_mask = BIT64(BOARD_TP_SDA) | BIT64(BOARD_TP_SCL) | BIT64(BOARD_LCD_PCLK) | BIT64(BOARD_LCD_D0) |
                        BIT64(BOARD_LCD_D1) | BIT64(BOARD_LCD_D2) | BIT64(BOARD_LCD_D3) | BIT64(BOARD_LCD_CS) |
                        BIT64(BOARD_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&lcd_pins), TAG, "lcd pins");
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if (lcd_pins.pin_bit_mask & BIT64(pin)) {
            gpio_set_level(pin, 0);
        }
    }

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    const i2c_device_config_t exp_cfg = { .device_address = EXP_ADDR, .scl_speed_hz = 400000 };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &exp_cfg, &s_exp), TAG, "expander");
    s_exp_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_exp_lock, ESP_ERR_NO_MEM, TAG, "expander lock");

    /* Every rail off but the system latch, then the rails we use. The latch
     * stays on: after a software reset on battery, dropping it even briefly
     * would cut the power (the expander keeps its outputs across our reset).
     * The LCD rail does go off, so the panel starts from scratch. */
    s_exp_out = EXP_PWR_SYSTEM;
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_OUTPUT, s_exp_out), TAG, "expander outputs");
    ESP_RETURN_ON_ERROR(exp_write(EXP_REG_CONFIG, EXP_INPUTS), TAG, "expander config");
    uint16_t in;
    ESP_RETURN_ON_ERROR(exp_read(&in), TAG, "expander inputs");
    if ((esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT0)) && (in & EXP_WHEEL)) {
        /* Powered off on USB, and something other than the wheel woke us (the charger). */
        exp_write(EXP_REG_OUTPUT, 0);
        sleep_until_wheel();
    }
    ESP_RETURN_ON_ERROR(exp_set(EXP_PWR_SYSTEM, true), TAG, "system rail");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(exp_set(EXP_PWR_LCD | EXP_PWR_BAT_ADC, true), TAG, "rails");
    ESP_LOGI(TAG, "expander inputs 0x%04x", in);

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(knob_init(), TAG, "knob");
    const gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(EXP_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "expander int");

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATT_ADC,
        .atten = ADC_ATTEN_DB_2_5,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration: battery level unknown");
        s_cali = NULL;
    }
    /* The SPD2010 runs firmware of its own and needs a while after its rail
     * comes up before it takes its setup (the Muse SDK measured ~150 ms). */
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c(void)
{
    return s_i2c;
}

void board_set_brightness(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void board_speaker_amp(bool on)
{
    exp_set(EXP_PWR_CODEC_PA, on);
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool board_wheel_down(void)
{
    uint16_t in;
    return exp_read(&in) == ESP_OK && !(in & EXP_WHEEL);
}

int board_wheel_counts(void)
{
    int n = 0;
    if (pcnt_unit_get_count(s_knob, &n) == ESP_OK && n) {
        pcnt_unit_clear_count(s_knob);
    }
    return n;
}

esp_err_t board_read_power(board_power_t *out)
{
    uint16_t in;
    ESP_RETURN_ON_ERROR(exp_read(&in), TAG, "expander read");
    out->usb = !(in & EXP_VBUS);
    out->charging = out->usb && (in & EXP_STDBY);
    out->mv = 0;
    out->pct = -1;
    if (!s_cali) {
        return ESP_OK;
    }
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &mv), TAG, "adc cali");
        sum += mv;
    }
    const int v = sum / 8 * 82 / 20;
    out->mv = v;
    /* Seeed's charge curve. */
    const int pct = (-v * v + 9016 * v - 19189000) / 10000;
    out->pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return ESP_OK;
}

void board_power_off(void)
{
    board_set_brightness(0);
    exp_set(EXP_PWR_LCD | EXP_PWR_CODEC_PA | EXP_PWR_BAT_ADC | EXP_PWR_AI | EXP_PWR_SYSTEM, false);
    /* On battery, power goes once the wheel is up. On USB we're still here. */
    while (board_wheel_down()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    sleep_until_wheel();
}
