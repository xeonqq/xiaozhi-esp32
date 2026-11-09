#ifndef _PHONIEBOX_ESP32_CONFIG_H_
#define _PHONIEBOX_ESP32_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

// ---------------------------------------------------------------------------
// Audio: MAX98357A I2S amplifier (output) + INMP441 I2S microphone (input).
// MAX98357A needs no control interface, so the generic I2S codec is enough.
// ---------------------------------------------------------------------------
#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS GPIO_NUM_32
#define AUDIO_I2S_MIC_GPIO_SCK GPIO_NUM_14
#define AUDIO_I2S_MIC_GPIO_DIN GPIO_NUM_34

#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_25
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_27
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_26

// ---------------------------------------------------------------------------
// Shared SPI bus: SD card and MFRC522/RC522. These are the default VSPI pins
// of a classic ESP32 DevKit. Give each device its own chip select.
// ---------------------------------------------------------------------------
#define PHONIEBOX_SPI_HOST SPI2_HOST
#define PHONIEBOX_SPI_SCK_PIN GPIO_NUM_18
#define PHONIEBOX_SPI_MOSI_PIN GPIO_NUM_23
#define PHONIEBOX_SPI_MISO_PIN GPIO_NUM_19

#define SD_CARD_CS_PIN GPIO_NUM_5
#define SD_CARD_MOUNT_POINT "/sdcard"

#define RFID_CS_PIN GPIO_NUM_21
#define RFID_RST_PIN GPIO_NUM_17

// ---------------------------------------------------------------------------
// Buttons. BOOT keeps the normal XiaoZhi behaviour (chat / Wi-Fi config).
// The other three control the local music player.
// ---------------------------------------------------------------------------
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define NEXT_BUTTON_GPIO GPIO_NUM_16
#define PREVIOUS_BUTTON_GPIO GPIO_NUM_2
#define PLAY_PAUSE_BUTTON_GPIO GPIO_NUM_33

// ---------------------------------------------------------------------------
// Optional 128x64 OLED status display on I2C. The SSD1306 and SH1106 panels
// use the same wiring; the build variant selects the controller driver.
// ---------------------------------------------------------------------------
#define DISPLAY_SDA_PIN GPIO_NUM_4
#define DISPLAY_SCL_PIN GPIO_NUM_15
#define DISPLAY_WIDTH 128
#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT 32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT 64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT 64
#define SH1106
#else
#error "OLED display type is not selected"
#endif
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

#endif  // _PHONIEBOX_ESP32_CONFIG_H_
