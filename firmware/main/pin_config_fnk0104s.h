#pragma once

// Freenove FNK0104S — 4.0" ESP32-S3 display board (ESP32-S3R8: 8 MB octal
// PSRAM; 16 MB flash, JEDEC 0x18405e measured on the owner's unit).
// Sources: Freenove_ESP32_S3_Display 4.0inch schematic, the vendor TFT_eSPI
// setup FNK0104S_4.0_320x480_ST7796.h, and the vendor XiaoZhi flash script.
// Phase 1 drives only the BOOT button and the shared I2C bus; the rest is
// recorded here for the display/touch/audio phases.

// Buttons: BOOT is the only user button. click = home, hold = push-to-talk
// (tiny_button.cpp colour-board remap).
#define PIN_BTN_OK          0
#define PIN_BTN_UP          (-1)
#define PIN_BTN_DOWN        (-1)

// Shared I2C bus: FT6336U touch (0x38) + ES8311 codec (0x18).
#define PIN_I2C_SCL         15
#define PIN_I2C_SDA         16
#define FT6336_I2C_ADDR     0x38
#define ES8311_I2C_ADDR     0x18

// LCD ST7796S 320x480 SPI (SPI3, vendor runs it at 80 MHz). RST not wired.
// GPIO45/46 are strapping pins: only drive them after boot.
#define PIN_LCD_SCK         12
#define PIN_LCD_MOSI        11
#define PIN_LCD_MISO        13
#define PIN_LCD_CS          10
#define PIN_LCD_DC          46
#define PIN_LCD_BL          45

// Touch FT6336U (single contact).
#define PIN_TOUCH_RST       18
#define PIN_TOUCH_INT       17

// Audio: ES8311 + analog MEMS mic + SC8002B amp. Names are the ESP's side:
// DIN = mic data into the ESP (codec I2S_DO), DOUT = ESP to the DAC.
#define PIN_I2S_MCLK        4
#define PIN_I2S_BCLK        5
#define PIN_I2S_WS          7
#define PIN_I2S_DIN         6
#define PIN_I2S_DOUT        8
// AUDIO_EN drives the amp's SHUTDOWN (10K pull-up): LOW = amp on.
#define PIN_AUDIO_EN        1
#define AUDIO_EN_ON_LEVEL   0

// Misc.
#define PIN_WS2812          42
#define PIN_BAT_ADC         9   // behind a 2:1 divider

// MicroSD, SDMMC 4-bit.
#define PIN_SD_CLK          38
#define PIN_SD_CMD          40
#define PIN_SD_D0           39
#define PIN_SD_D1           41
#define PIN_SD_D2           48
#define PIN_SD_D3           47
