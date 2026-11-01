#pragma once
/*
 * SenseCAP Watcher hardware: power rails and the wheel's push on a PCA9535
 * expander, the wheel's rotary encoder on PCNT, backlight, battery.
 * Pins and sequencing follow Seeed's BSP and the Muse SDK's Watcher board.
 */
#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* The board's pins, shared with display.c and audio.c. */
#define BOARD_LCD_RES 412
#define BOARD_LCD_HOST SPI3_HOST
#define BOARD_LCD_PCLK GPIO_NUM_7
#define BOARD_LCD_D0 GPIO_NUM_9
#define BOARD_LCD_D1 GPIO_NUM_1
#define BOARD_LCD_D2 GPIO_NUM_14
#define BOARD_LCD_D3 GPIO_NUM_13
#define BOARD_LCD_CS GPIO_NUM_45
#define BOARD_LCD_BL GPIO_NUM_8
#define BOARD_TP_SDA GPIO_NUM_39   /* touch has its own I2C bus */
#define BOARD_TP_SCL GPIO_NUM_38
#define BOARD_I2S_MCLK GPIO_NUM_10
#define BOARD_I2S_BCLK GPIO_NUM_11
#define BOARD_I2S_WS GPIO_NUM_12
#define BOARD_I2S_DIN GPIO_NUM_15
#define BOARD_I2S_DOUT GPIO_NUM_16

/* Brings up the expander and rails (system, LCD, battery divider), the
 * wheel and the battery ADC. Holds the LCD lines low until the LCD rail is
 * up. If woken from power-off by something other than the wheel, goes back
 * to sleep. */
esp_err_t board_init(void);

/* The bus the expander and the audio codecs share. */
i2c_master_bus_handle_t board_i2c(void);

/* Backlight, 0..100. */
void board_set_brightness(int pct);

/* The speaker amplifier's enable (expander rail). */
void board_speaker_amp(bool on);

/* Raw wheel push state (true while pressed); false if the expander can't be read. */
bool board_wheel_down(void);

/* Encoder counts since the last call (quadrature edges, signed). */
int board_wheel_counts(void);

/* Battery: millivolts and percent (-1 if unknown), USB power, charging. */
typedef struct {
    int mv;
    int pct;
    bool usb;
    bool charging;
} board_power_t;
esp_err_t board_read_power(board_power_t *out);

/* Rails off. On battery the board loses power once the wheel is released;
 * on USB it deep-sleeps until the wheel is pressed. Doesn't return. */
void board_power_off(void);
