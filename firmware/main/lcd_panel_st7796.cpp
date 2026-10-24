// lcd_panel_st7796 — the FNK0104S 4.0" ST7796S (320x480 SPI) behind lcd_canvas.
//
// Settings are the vendor's own (Libraries/FNK0104S TFT_eSPI_Setups v1.2,
// FNK0104S_4.0_320x480_ST7796.h): ST7796, TFT_INVERSION_ON, rotation 0 =
// MADCTL MX|BGR, backlight GPIO45 active HIGH, RST not wired (software reset).
// Bus: SPI2 on its IO_MUX pins (CLK 12, MOSI 11, MISO 13, CS 10), so the clock
// is not limited by the GPIO matrix; CONFIG_TINY_LCD_SPI_MHZ picks it (40
// default, 20 if a panel shows noise). GPIO45/46 are strapping pins: nothing
// here runs before the app, so driving them is safe.
#include "lcd_canvas.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pin_config.h"
#include "sdkconfig.h"

#ifndef CONFIG_TINY_LCD_SPI_MHZ
#define CONFIG_TINY_LCD_SPI_MHZ 40
#endif

namespace {

constexpr char kTag[] = "st7796";
constexpr spi_host_device_t kHost = SPI2_HOST;
constexpr uint16_t kWidth = 320, kHeight = 480;
// LEDC timer/channel 1: 0 is the reTerminal buzzer's, keep them apart even
// though that driver is not built here.
constexpr ledc_mode_t kBlMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBlTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_1;
constexpr uint32_t kBlMaxDuty = (1u << 10) - 1;

esp_lcd_panel_io_handle_t s_io = nullptr;
esp_lcd_panel_handle_t s_panel = nullptr;
SemaphoreHandle_t s_done = nullptr;   // one give per finished colour transfer

bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *,
                             void *)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woke);
    return woke == pdTRUE;
}

esp_err_t backlight_init()
{
    ledc_timer_config_t t = {};
    t.speed_mode = kBlMode;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num = kBlTimer;
    t.freq_hz = 5000;
    t.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), kTag, "bl timer");
    ledc_channel_config_t c = {};
    c.gpio_num = PIN_LCD_BL;
    c.speed_mode = kBlMode;
    c.channel = kBlChannel;
    c.timer_sel = kBlTimer;
    c.duty = 0;   // dark until the first frame is on the glass
    return ledc_channel_config(&c);
}

esp_err_t init(size_t max_transfer_bytes)
{
    ESP_RETURN_ON_ERROR(backlight_init(), kTag, "backlight");

    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_SCK;
    bus.mosi_io_num = PIN_LCD_MOSI;
    bus.miso_io_num = PIN_LCD_MISO;
    // Every unused data line must be -1, or IDF claims GPIO0 (= BOOT here).
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.max_transfer_sz = static_cast<int>(max_transfer_bytes);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO), kTag, "spi bus");

    s_done = xSemaphoreCreateCounting(8, 0);
    if (s_done == nullptr) return ESP_ERR_NO_MEM;

    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = PIN_LCD_CS;
    io.dc_gpio_num = PIN_LCD_DC;
    io.spi_mode = 0;
    io.pclk_hz = CONFIG_TINY_LCD_SPI_MHZ * 1000 * 1000;
    io.trans_queue_depth = 8;
    io.on_color_trans_done = on_color_done;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            static_cast<esp_lcd_spi_bus_handle_t>(kHost), &io, &s_io),
                        kTag, "panel io");

    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = -1;   // RST not wired: software reset
    dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    dev.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(s_io, &dev, &s_panel), kTag, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), kTag, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), kTag, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), kTag, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), kTag, "madctl");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), kTag, "disp on");
    ESP_LOGI(kTag, "SPI2 IO_MUX %d MHz, %ux%u RGB565, BL GPIO%d",
             CONFIG_TINY_LCD_SPI_MHZ, kWidth, kHeight, PIN_LCD_BL);
    return ESP_OK;
}

esp_err_t draw(int y0, int y1, const uint16_t *px)
{
    return esp_lcd_panel_draw_bitmap(s_panel, 0, y0, kWidth, y1, px);
}

esp_err_t wait_done(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? ESP_OK
                                                                        : ESP_ERR_TIMEOUT;
}

esp_err_t sleep(bool on)
{
    if (on) ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), kTag, "disp off");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_sleep(s_panel, on), kTag, "slp");
    if (!on) ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), kTag, "disp on");
    return ESP_OK;
}

esp_err_t brightness(uint8_t percent)
{
    const uint32_t duty = kBlMaxDuty * (percent > 100 ? 100 : percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(kBlMode, kBlChannel, duty), kTag, "bl duty");
    return ledc_update_duty(kBlMode, kBlChannel);
}

constexpr LcdPanelOps kOps = {
    .name = "ST7796S",
    .width = kWidth,
    .height = kHeight,
    .row_align = 1,
    .init = init,
    .draw = draw,
    .wait_done = wait_done,
    .sleep = sleep,
    .brightness = brightness,
};

}  // namespace

const LcdPanelOps *lcd_panel_ops() { return &kOps; }
