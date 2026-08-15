#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// Waveshare ESP32-S3-ePaper-1.54 (V2: 8MB flash, 8MB octal PSRAM).
//
// Every pin below is cross-checked against two independent sources: the
// xiaozhi-esp32 board file this port descends from, and a capture of the
// factory firmware's serial boot log taken off the board itself. They agree.
//
// Careful when copying anything from another Waveshare board: GPIO 8 is the
// e-paper BUSY line here, and it is the I2C SDA line on the Touch-LCD-3.5B.
// Mixing the two does not fail to compile — it fails silently on the bench.

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK  GPIO_NUM_14
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_38
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_15
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_16
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_45

// Unlike the Touch-LCD-3.5B, this board can mute the amplifier from a GPIO.
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_47
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_48
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

// BOOT is the talk button (see InitializeButtons). PWR is an input: a long
// press cuts the rails, and the boot sequence waits for it to read high.
#define BOOT_BUTTON_GPIO  GPIO_NUM_0
#define VBAT_PWR_GPIO     GPIO_NUM_18

/* e-Paper — 200x200, 1bpp, on SPI3 */
#define EPD_SPI_NUM   SPI3_HOST

#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_CS_PIN    GPIO_NUM_11
#define EPD_SCK_PIN   GPIO_NUM_12
#define EPD_MOSI_PIN  GPIO_NUM_13
#define EPD_RST_PIN   GPIO_NUM_9
#define EPD_BUSY_PIN  GPIO_NUM_8   // input: high = panel busy

#define DISPLAY_WIDTH   200
#define DISPLAY_HEIGHT  200

/* Switched power rails — nothing on this board comes up without them.
 * The e-paper and audio rails are active LOW; the battery rail is active
 * HIGH. See board_power_bsp.cc, which is where that asymmetry is enforced. */
#define EPD_PWR_PIN    GPIO_NUM_6
#define AUDIO_PWR_PIN  GPIO_NUM_42
#define VBAT_PWR_PIN   GPIO_NUM_17

// Heartbeat LED driven by BoardPowerBsp: a 200ms blink every five seconds.
#define POWER_LED_PIN  GPIO_NUM_3

/* Battery sense on ADC1 channel 3, which is GPIO 4.
 *
 * The schematic for this board is not public, so the divider ratio is not
 * read off a part number: it is recovered from the xiaozhi board file, which
 * doubles the measured voltage. Two equal resistors reproduce that same 2x
 * for adc_battery_estimation, which only ever uses the ratio. If the real
 * schematic ever turns up and disagrees, it wins — fix it here. */
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_3
#define BATTERY_UPPER_RESISTOR  100000.0f
#define BATTERY_LOWER_RESISTOR  100000.0f
// No charge-status pin exists on this board, so charging is inferred from the
// voltage trend. GPIO_NUM_NC, never 0 — 0 is the BOOT button.
#define BATTERY_CHARGING_PIN    GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_
