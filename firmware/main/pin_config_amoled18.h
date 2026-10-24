#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 (CO5300 368x448 QSPI AMOLED, CST820
// touch). 16 MB QIO flash, 8 MB octal PSRAM. No board schematic is published:
// every pin and polarity here is vendor-code evidence (BSP
// waveshare/esp32_s3_touch_amoled_1_8 2.0.3 + the Arduino pin_config.h) and
// must be confirmed on hardware. Driven so far: BOOT button, the shared I2C
// bus, the TCA9554 expander, the CO5300 panel (lcd_panel_co5300.cpp), the
// ES8311 codec, the CST820 touch (tiny_touch.cpp), the AXP2101 (battery,
// USB, PWR key) and the PCF85063A RTC.

// Real drivers this board builds in place of hal_absent.cpp's stubs.
#define HAL_PMU_AXP2101     1   // sticky_pmu_axp2101.cpp
#define HAL_RTC_PCF85063    1   // sticky_rtc_pcf85063.cpp

// Buttons: BOOT is OK/AI. The PWR key is not a GPIO: it is an AXP2101
// interrupt whose IRQ line goes to TCA9554 P5 (vendor Arduino example 12
// reads expander pin 5; example 90's sdkconfig sets the PMU IRQ GPIO to -1).
// tiny_button polls the PMU's latched key status instead (sticky_pmu.h).
#define PIN_BTN_OK          0
#define PIN_BTN_UP          (-1)
#define PIN_BTN_DOWN        (-1)

// Shared I2C bus: TCA9554, CST820, AXP2101, PCF85063A, QMI8658, ES8311.
#define PIN_I2C_SCL         14
#define PIN_I2C_SDA         15
#define TCA9554_I2C_ADDR    0x20
#define CST820_I2C_ADDR     0x15   // V2 touch
#define FT3168_I2C_ADDR     0x38   // original revision touch (out of scope)
#define AXP2101_I2C_ADDR    0x34
#define PCF85063_I2C_ADDR   0x51
#define QMI8658_I2C_ADDR    0x6B
#define ES8311_I2C_ADDR     0x18

// TCA9554 output bits. The vendor examples drive 0..2 low for 20 ms, then high.
#define TCA_BIT_LCD_RST     0
#define TCA_BIT_DSI_PWR_EN  1
#define TCA_BIT_TOUCH_RST   2
#define TCA_BIT_PMU_IRQ     5   // input: AXP2101 IRQ (unused, polled instead)
#define TCA_BIT_SD_CS       7

// CO5300 QSPI panel. No reset/backlight GPIO (reset is TCA bit 0; brightness
// is DCS 0x51).
#define PIN_LCD_CS          12
#define PIN_LCD_PCLK        11
#define PIN_LCD_D0          4
#define PIN_LCD_D1          5
#define PIN_LCD_D2          6
#define PIN_LCD_D3          7

#define PIN_TOUCH_INT       21   // CST820 INT, active low; logged only (polled)

// Audio: ES8311. DIN = mic into the ESP, DOUT = ESP to the DAC.
#define PIN_I2S_MCLK        16
#define PIN_I2S_BCLK        9
#define PIN_I2S_WS          45
#define PIN_I2S_DOUT        8
#define PIN_I2S_DIN         10
// Amp enable, active-HIGH per the BSP (pa_reverted=false). Unverified.
#define PIN_AUDIO_EN        46
#define AUDIO_EN_ON_LEVEL   1

// MicroSD, SDMMC 1-bit.
#define PIN_SD_CLK          2
#define PIN_SD_CMD          1
#define PIN_SD_D0           3
