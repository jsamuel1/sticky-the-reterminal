// lcd_panel_co5300 — the Waveshare ESP32-S3-Touch-AMOLED-1.8 V2's CO5300
// (368x448 AMOLED, QSPI) behind lcd_canvas.
//
// Driver: espressif/esp_lcd_co5300 2.2.0 (pinned in idf_component.yml; the
// first 2.x with a disp_sleep op). The init table, the x-gap rule and the
// QSPI bus/IO shape are vendor evidence from the Waveshare BSP
// waveshare/esp32_s3_touch_amoled_1_8 v2.0.3, esp32_s3_touch_amoled_1_8.c
// (lcd_init_cmds[], bsp_display_new(), bsp_touch_new()); the BSP itself is
// NOT a dependency (it pulls LVGL). No schematic is published: verify on glass.
//
// Power and reset belong to board_generic.cpp: amoled_expander_up() has
// already pulsed TCA9554 bit0 (LCD_RST) and raised bit1 (DSI_PWR_EN) before
// sticky_display_init() runs. This file never touches the expander, so
// reset_gpio_num is -1 and the driver issues a software reset.
//
// Alignment: the CO5300 takes window starts and sizes in units of 2 pixels
// (the BSP's LVGL rounder forces even x1/y1 and odd x2/y2). Every draw here is
// full width (368, plus the even x-gap) and lcd_canvas aligns rows to
// kOps.row_align = 2, so each window is even-aligned on both axes.
#include "lcd_canvas.h"

#include "board.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pin_config.h"
#include "sdkconfig.h"

#ifndef CONFIG_TINY_LCD_SPI_MHZ
#define CONFIG_TINY_LCD_SPI_MHZ 40
#endif

namespace {

constexpr char kTag[] = "co5300";
constexpr spi_host_device_t kHost = SPI2_HOST;   // BSP_LCD_SPI_NUM
constexpr uint16_t kWidth = 368, kHeight = 448;  // BSP_LCD_H_RES / V_RES
// BSP_LCD_CST816S_X_GAP: the BSP shifts the window 16 columns right when the
// CST816S-protocol touch controller (the V2's CST820) answers at 0x15, and
// leaves it at 0 for the FT5x06 variant. Mirrored exactly.
constexpr int kXGapCst816 = 0x10;
static_assert(kWidth % 2 == 0 && kHeight % 2 == 0 && kXGapCst816 % 2 == 0,
              "CO5300 windows must stay 2-pixel aligned");

// QSPI framing (esp_lcd_co5300_spi.c tx_param): opcode 0x02 = write command,
// DCS command in bits 15..8 of the 32-bit lcd_cmd.
constexpr int qspi_cmd(uint8_t dcs) { return (0x02 << 24) | (dcs << 8); }
constexpr uint8_t kDcsBrightness = 0x51;

// lcd_init_cmds[] from BSP v2.0.3, verbatim except ONE value: 0x51 (write
// display brightness) is 0x00 here, not 0xFF. The table ends in SLPOUT +
// DISPON, so with 0xFF the power-on garbage in panel RAM would be lit at full
// brightness until the first frame; lcd_canvas raises brightness only after it
// has painted a clean page. (Named arrays: C++ has no compound literals.)
constexpr uint8_t kFE[] = {0x00};
constexpr uint8_t kC4[] = {0x80};
constexpr uint8_t k3A[] = {0x55};                    // COLMOD RGB565
constexpr uint8_t k35[] = {0x00};                    // TE on
constexpr uint8_t k53[] = {0x20};                    // brightness control on
constexpr uint8_t k51[] = {0x00};                    // brightness: BSP 0xFF, dark here
constexpr uint8_t k63[] = {0xFF};                    // HBM brightness
constexpr uint8_t k2A[] = {0x00, 0x00, 0x01, 0x6F};  // CASET 0..367
constexpr uint8_t k2B[] = {0x00, 0x00, 0x01, 0xBF};  // RASET 0..447
const co5300_lcd_init_cmd_t kInitCmds[] = {
    {0xFE, kFE, 1, 0},
    {0xC4, kC4, 1, 0},
    {0x3A, k3A, 1, 0},
    {0x35, k35, 1, 0},
    {0x53, k53, 1, 0},
    {0x51, k51, 1, 0},
    {0x63, k63, 1, 0},
    {0x2A, k2A, 4, 0},
    {0x2B, k2B, 4, 0},
    {0x11, nullptr, 0, 100},   // SLPOUT
    {0x29, nullptr, 0, 0},     // DISPON
};

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

// bsp_touch_new(): CST816S answers at 0x15 -> x-gap 0x10, else 0.
int probe_x_gap()
{
    i2c_master_bus_handle_t bus = board_sensor_i2c_bus();
    const bool cst = bus != nullptr && i2c_master_probe(bus, CST820_I2C_ADDR, 50) == ESP_OK;
    return cst ? kXGapCst816 : 0;
}

esp_err_t init(size_t max_transfer_bytes)
{
    // CO5300_PANEL_BUS_QSPI_CONFIG, with every unused line spelled -1 (a
    // zeroed field is GPIO0 = BOOT).
    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_PCLK;
    bus.data0_io_num = PIN_LCD_D0;
    bus.data1_io_num = PIN_LCD_D1;
    bus.data2_io_num = PIN_LCD_D2;
    bus.data3_io_num = PIN_LCD_D3;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.max_transfer_sz = static_cast<int>(max_transfer_bytes);
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO), kTag, "qspi bus");

    s_done = xSemaphoreCreateCounting(8, 0);
    if (s_done == nullptr) return ESP_ERR_NO_MEM;

    // CO5300_PANEL_IO_QSPI_CONFIG: no D/C line, 32-bit opcode+command word.
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = PIN_LCD_CS;
    io.dc_gpio_num = -1;
    io.spi_mode = 0;
    io.pclk_hz = CONFIG_TINY_LCD_SPI_MHZ * 1000 * 1000;   // BSP: 40 MHz
    io.trans_queue_depth = 8;
    io.on_color_trans_done = on_color_done;
    io.lcd_cmd_bits = 32;
    io.lcd_param_bits = 8;
    io.flags.quad_mode = true;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            static_cast<esp_lcd_spi_bus_handle_t>(kHost), &io, &s_io),
                        kTag, "panel io");

    co5300_vendor_config_t vendor = {};
    vendor.init_cmds = kInitCmds;
    vendor.init_cmds_size = sizeof kInitCmds / sizeof kInitCmds[0];
    vendor.flags.use_qspi_interface = 1;
    esp_lcd_panel_dev_config_t dev = {};
    dev.reset_gpio_num = -1;   // reset is TCA9554 bit0, pulsed by board_generic
    dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    dev.bits_per_pixel = 16;
    dev.vendor_config = &vendor;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &dev, &s_panel), kTag, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), kTag, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), kTag, "init");
    const int x_gap = probe_x_gap();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, x_gap, 0), kTag, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), kTag, "disp on");
    // No INVON: RGB565 0x0000 is a pixel switched off, matching lcd_palette's
    // level 0 = black, so the gray ramp needs no inversion on this panel.
    ESP_LOGI(kTag, "QSPI SPI2 %d MHz, %ux%u RGB565, x-gap %d (%s), dark until first frame",
             CONFIG_TINY_LCD_SPI_MHZ, kWidth, kHeight, x_gap,
             x_gap ? "CST816S-class touch at 0x15" : "no touch at 0x15");
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

esp_err_t brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    const uint8_t level = static_cast<uint8_t>(percent * 255 / 100);
    return esp_lcd_panel_io_tx_param(s_io, qspi_cmd(kDcsBrightness), &level, 1);
}

// Sleep: dark + DISPOFF + SLPIN. Wake: SLPOUT, then DISPON. With no reset GPIO
// the 2.2.0 driver uses plain SLPIN/SLPOUT (not Deep Standby) and waits its
// own 120 ms settle after each; panel RAM survives, and lcd_canvas repaints
// before it restores brightness anyway. The panel rail (TCA bit1) stays up.
esp_err_t sleep(bool on)
{
    if (on) {
        ESP_RETURN_ON_ERROR(brightness(0), kTag, "dark");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), kTag, "disp off");
        return esp_lcd_panel_disp_sleep(s_panel, true);
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_sleep(s_panel, false), kTag, "slpout");
    return esp_lcd_panel_disp_on_off(s_panel, true);
}

constexpr LcdPanelOps kOps = {
    .name = "CO5300",
    .width = kWidth,
    .height = kHeight,
    .row_align = 2,
    .init = init,
    .draw = draw,
    .wait_done = wait_done,
    .sleep = sleep,
    .brightness = brightness,
};

}  // namespace

const LcdPanelOps *lcd_panel_ops() { return &kOps; }
