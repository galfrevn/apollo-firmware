#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "adc_battery_monitor.h"
#include "application.h"
#include "board_power_bsp.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "wifi_board.h"

#define TAG "waveshare_epaper_1_54"

// Waveshare ESP32-S3-ePaper-1.54 — a pocket body, on a battery, with no
// touch and no screen worth animating.
//
// The display is deliberately absent from this port. Board::GetDisplay()
// falls back to NoDisplay, and that is the whole point of the first pass:
// audio, the protocol, the rails and the battery are all reachable without
// a single line of e-paper driver, and they are what the experiment is
// asking about. A 200x200 1bpp panel that repaints in a few hundred
// milliseconds is a different problem from an animated round LCD, and it
// gets solved on its own once this half is known to work.
class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button                  boot_button_;
    Button                  pwr_button_;
    BoardPowerBsp*          power_;

    // Nothing on this board answers before its rail is up, and the boot
    // sequence additionally waits on the power button reading high. This is
    // the ePaper's equivalent of the TCA9554 display reset on the 3.5B: skip
    // it and you get a healthy-looking firmware talking to dead silicon.
    void InitializePower() {
        power_ = new BoardPowerBsp(EPD_PWR_PIN, AUDIO_PWR_PIN, VBAT_PWR_PIN);
        power_->VbatPowerOn();
        power_->PowerAudioOn();
        power_->PowerEpdOn();

        while (!gpio_get_level(VBAT_PWR_GPIO)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port          = (i2c_port_t) 0;
        i2c_bus_cfg.sda_io_num        = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num        = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority     = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeButtons() {
#ifdef CONFIG_APOLLO_PROTOCOL
        // Push-to-talk, the same gesture the 1.85C uses: hold_start and
        // hold_end are what the protocol is built around, and holding the
        // button is the only way to bound an utterance with the wake word
        // model disabled — which is how this board is configured, because an
        // always-on AFE is the single largest current draw on a device that
        // is supposed to live in a pocket.
        boot_button_.OnPressDown([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                return;
            }
            app.StartListening();
        });

        boot_button_.OnPressUp([this]() { Application::GetInstance().StopListening(); });

        // Not a long press: holding the button *is* how you talk, so a long
        // press fires constantly during normal use.
        boot_button_.OnMultipleClick([this]() { EnterWifiConfigMode(); }, 3);
#else
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
#endif

        // Cutting the rails is the actual off switch. Audio first so the
        // amplifier does not pop on the way down.
        pwr_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "Power button held, shutting down");
            vTaskDelay(pdMS_TO_TICKS(200));
            power_->PowerAudioOff();
            power_->PowerEpdOff();
            power_->VbatPowerOff();
        });
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), pwr_button_(VBAT_PWR_GPIO) {
        InitializePower();
        InitializeI2c();
        InitializeButtons();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        // The board file this port came from reported charging = false
        // unconditionally, which on a battery-powered device is worse than
        // reporting nothing. There is no charge-status GPIO here either, so
        // the estimation library infers it from the voltage trend instead.
        static AdcBatteryMonitor battery_monitor(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL,
                                                 BATTERY_UPPER_RESISTOR, BATTERY_LOWER_RESISTOR,
                                                 BATTERY_CHARGING_PIN);
        level       = battery_monitor.GetBatteryLevel();
        charging    = battery_monitor.IsCharging();
        discharging = battery_monitor.IsDischarging();
        return true;
    }
};

DECLARE_BOARD(CustomBoard);
