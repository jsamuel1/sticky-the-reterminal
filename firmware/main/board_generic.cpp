// board_generic — board_init() for the colour boards (FNK0104S, AMOLED-1.8).
//
// Neither board has a power latch: USB or the battery switch powers them, so
// there is no GPIO45/46 dance here and a panic simply reboots. What phase 1
// does is the minimum a later display/touch/audio phase stands on:
//   * the shared I2C bus (one bus carries touch, codec and, on the AMOLED,
//     PMU/RTC/IMU/expander);
//   * AMOLED only: the TCA9554 expander releases panel power and the LCD and
//     touch resets BEFORE anything probes the panel or the touch controller;
//   * a bus probe that names what answered, so the boot log is the evidence
//     for the next phase (CST820 at 0x15 = V2 board);
//   * one line with the real flash and PSRAM size/mode.
#include "board.h"

#include <cinttypes>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"
#include "sdkconfig.h"
#include "tiny/board_caps.h"

namespace {

constexpr char kTag[] = "board";
constexpr int kProbeTimeoutMs = 50;

i2c_master_bus_handle_t s_bus = nullptr;

struct KnownDevice {
    uint8_t addr;
    const char *name;
};

#if defined(CONFIG_TINY_BOARD_FNK0104S)
constexpr KnownDevice kBusDevices[] = {
    {FT6336_I2C_ADDR, "FT6336U touch"},
    {ES8311_I2C_ADDR, "ES8311 codec"},
};
#elif defined(CONFIG_TINY_BOARD_AMOLED18)
constexpr KnownDevice kBusDevices[] = {
    {TCA9554_I2C_ADDR, "TCA9554 expander"},
    {CST820_I2C_ADDR, "CST820 touch (V2)"},
    {FT3168_I2C_ADDR, "FT3168 touch (original revision)"},
    {AXP2101_I2C_ADDR, "AXP2101 PMU"},
    {PCF85063_I2C_ADDR, "PCF85063A RTC"},
    {QMI8658_I2C_ADDR, "QMI8658 IMU"},
    {ES8311_I2C_ADDR, "ES8311 codec"},
};
#endif

bool probe(uint8_t addr)
{
    return i2c_master_probe(s_bus, addr, kProbeTimeoutMs) == ESP_OK;
}

void log_memory()
{
    uint32_t flash_id = 0;
    uint32_t flash_phys = 0;
    esp_flash_read_id(nullptr, &flash_id);
    esp_flash_get_physical_size(nullptr, &flash_phys);
#if defined(CONFIG_SPIRAM_MODE_OCT)
    const char *psram_mode = "octal";
#elif defined(CONFIG_SPIRAM_MODE_QUAD)
    const char *psram_mode = "quad";
#else
    const char *psram_mode = "none";
#endif
    const size_t psram = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    ESP_LOGI(kTag,
             "%s: flash id 0x%06" PRIx32 " %" PRIu32 " MB (image %d MB), "
             "psram %u MB %s %d MHz",
             tiny_board_caps()->name, flash_id, flash_phys >> 20,
             (int)tiny_board_caps()->flash_mb, (unsigned)(psram >> 20),
             psram ? psram_mode : "absent", CONFIG_SPIRAM_SPEED);
}

#if defined(CONFIG_TINY_BOARD_AMOLED18)
// TCA9554 registers.
constexpr uint8_t kTcaOutput = 0x01;
constexpr uint8_t kTcaConfig = 0x03;  // 1 = input

esp_err_t tca_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof buf, kProbeTimeoutMs);
}

// Panel power on, LCD + touch out of reset. Mirrors the vendor examples:
// bits 0..2 low for 20 ms, then high. The output register is written before
// the direction register so the pins never glitch high-low on the way out.
esp_err_t amoled_expander_up()
{
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = TCA9554_I2C_ADDR;
    dc.scl_speed_hz = 400000;
    i2c_master_dev_handle_t dev = nullptr;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dc, &dev), kTag,
                        "tca9554 add");
    constexpr uint8_t kDriven = (1U << TCA_BIT_LCD_RST) |
                                (1U << TCA_BIT_DSI_PWR_EN) |
                                (1U << TCA_BIT_TOUCH_RST);
    const char *step = "output low";
    esp_err_t err = tca_write(dev, kTcaOutput, static_cast<uint8_t>(0xFF & ~kDriven));
    if (err == ESP_OK) {
        step = "direction";
        err = tca_write(dev, kTcaConfig, static_cast<uint8_t>(0xFF & ~kDriven));
    }
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    // Release the lines whatever happened above: a write that failed halfway
    // must not leave the panel rail off and touch/LCD held in reset.
    const esp_err_t rel = tca_write(dev, kTcaOutput, 0xFF);
    if (err == ESP_OK && rel != ESP_OK) {
        step = "release";
        err = rel;
    }
    i2c_master_bus_rm_device(dev);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "tca9554: bring-up failed at %s: %s (release write: %s)",
                 step, esp_err_to_name(err), esp_err_to_name(rel));
        return err;
    }
    // CST820 needs time after reset release before it answers on the bus.
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(kTag, "tca9554: DSI_PWR_EN + LCD_RST + TOUCH_RST released");
    return ESP_OK;
}
#endif

#if defined(CONFIG_TINY_BOARD_FNK0104S)
// Release the FT6336 reset so the bus probe can see it. INT stays an input.
void fnk_touch_out_of_reset()
{
    gpio_config_t rst = {};
    rst.pin_bit_mask = 1ULL << PIN_TOUCH_RST;
    rst.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&rst) == ESP_OK) {
        gpio_set_level(static_cast<gpio_num_t>(PIN_TOUCH_RST), 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(static_cast<gpio_num_t>(PIN_TOUCH_RST), 1);
        vTaskDelay(pdMS_TO_TICKS(300));  // FT6336 datasheet: >=300 ms to ready
    }
}
#endif

void log_bus()
{
    int found = 0;
    for (const KnownDevice &d : kBusDevices) {
        const bool up = probe(d.addr);
        found += up ? 1 : 0;
        ESP_LOGI(kTag, "i2c 0x%02X %-32s %s", d.addr, d.name, up ? "present" : "absent");
    }
#if defined(CONFIG_TINY_BOARD_AMOLED18)
    const char *rev = probe(CST820_I2C_ADDR)   ? "V2 (CST820 at 0x15)"
                      : probe(FT3168_I2C_ADDR) ? "original (FT3168 at 0x38) - unsupported"
                                               : "unknown (no touch controller answered)";
    ESP_LOGI(kTag, "touch probe: board revision %s", rev);
#endif
    ESP_LOGI(kTag, "i2c bus SDA=%d SCL=%d: %d of %d known devices answered",
             PIN_I2C_SDA, PIN_I2C_SCL, found,
             (int)(sizeof kBusDevices / sizeof kBusDevices[0]));
}

}  // namespace

esp_err_t board_init()
{
    log_memory();

    esp_err_t result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = static_cast<gpio_num_t>(PIN_I2C_SDA);
    bus.scl_io_num = static_cast<gpio_num_t>(PIN_I2C_SCL);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = 1;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_bus), kTag, "i2c bus");

#if defined(CONFIG_TINY_BOARD_AMOLED18)
    // Logged, not fatal: a missing expander must not cost the relay.
    (void)amoled_expander_up();
#elif defined(CONFIG_TINY_BOARD_FNK0104S)
    fnk_touch_out_of_reset();
#endif
    log_bus();
    return ESP_OK;
}

i2c_master_bus_handle_t board_sensor_i2c_bus()
{
    return s_bus;
}
