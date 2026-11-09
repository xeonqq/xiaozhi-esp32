#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#include <esp_log.h>

#include <algorithm>
#include <memory>
#include <string>

#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif
#include "display/oled_display.h"
#include "led/led.h"
#include "mcp_server.h"
#include "rc522_rfid_reader.h"
#include "spi_sd_card.h"

#if CONFIG_PHONIEBOX_ENABLED
#include "phoniebox/phoniebox_service.h"
#endif

#define TAG "PhonieboxBoard"

class PhonieboxEsp32Board : public WifiBoard {
public:
    PhonieboxEsp32Board()
        : boot_button_(BOOT_BUTTON_GPIO),
          next_button_(NEXT_BUTTON_GPIO),
          previous_button_(PREVIOUS_BUTTON_GPIO),
          play_pause_button_(PLAY_PAUSE_BUTTON_GPIO) {
        InitializeDisplay();
        InitializeSpiDevices();
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
#if defined(AUDIO_I2S_METHOD_SIMPLEX)
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                               AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                              AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Led* GetLed() override {
        static NoLed led;
        return &led;
    }

    RfidReader* GetRfidReader() override { return rfid_reader_.get(); }

    Storage* GetStorage() override { return storage_.get(); }

private:
    Button boot_button_;
    Button next_button_;
    Button previous_button_;
    Button play_pause_button_;

    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;

    std::unique_ptr<SpiSdCard> storage_;
    std::unique_ptr<Rc522RfidReader> rfid_reader_;

    void InitializeDisplay() {
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = DISPLAY_SDA_PIN;
        bus_config.scl_io_num = DISPLAY_SCL_PIN;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));

        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400 * 1000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;
#ifdef SH1106
        esp_lcd_panel_sh1106_config_t panel_vendor_config = {};
#else
        esp_lcd_panel_ssd1306_config_t panel_vendor_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
#endif
        panel_config.vendor_config = &panel_vendor_config;
#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "OLED initialization failed, continuing without a display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeSpiDevices() {
        spi_bus_config_t bus_config = {};
        bus_config.mosi_io_num = PHONIEBOX_SPI_MOSI_PIN;
        bus_config.miso_io_num = PHONIEBOX_SPI_MISO_PIN;
        bus_config.sclk_io_num = PHONIEBOX_SPI_SCK_PIN;
        bus_config.quadwp_io_num = -1;
        bus_config.quadhd_io_num = -1;
        bus_config.max_transfer_sz = 4096;

        esp_err_t err = spi_bus_initialize(PHONIEBOX_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to initialize shared SPI bus: %s", esp_err_to_name(err));
        }

        storage_ =
            std::make_unique<SpiSdCard>(PHONIEBOX_SPI_HOST, SD_CARD_CS_PIN, SD_CARD_MOUNT_POINT);
        if (!storage_->Initialize()) {
            ESP_LOGW(TAG, "Continuing without an SD card");
        }

        rfid_reader_ =
            std::make_unique<Rc522RfidReader>(PHONIEBOX_SPI_HOST, RFID_CS_PIN, RFID_RST_PIN);
        if (!rfid_reader_->Initialize()) {
            ESP_LOGW(TAG, "Continuing without an RFID reader");
        }
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_PHONIEBOX_ENABLED
        next_button_.OnClick([]() { PhonieboxService::GetInstance().Next(); });
        next_button_.OnLongPress([this]() {
            auto codec = GetAudioCodec();
            codec->SetOutputVolume(std::min(100, codec->output_volume() + 10));
            GetDisplay()->ShowNotification("Volume " + std::to_string(codec->output_volume()));
        });

        previous_button_.OnClick([]() { PhonieboxService::GetInstance().Previous(); });
        previous_button_.OnLongPress([this]() {
            auto codec = GetAudioCodec();
            codec->SetOutputVolume(std::max(0, codec->output_volume() - 10));
            GetDisplay()->ShowNotification("Volume " + std::to_string(codec->output_volume()));
        });

        play_pause_button_.OnClick([]() { PhonieboxService::GetInstance().TogglePause(); });
#else
        next_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            codec->SetOutputVolume(std::min(100, codec->output_volume() + 10));
        });
        previous_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            codec->SetOutputVolume(std::max(0, codec->output_volume() - 10));
        });
#endif
    }
};

DECLARE_BOARD(PhonieboxEsp32Board);
