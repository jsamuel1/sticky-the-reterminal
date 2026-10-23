// sticky_power_generic — sleep/wake for boards without a power latch.
//
// The only wake button is BOOT (GPIO0, active-low, RTC-capable on the S3).
// There is nothing to hold: no latch, no panel/touch/mic/SD rails driven in
// phase 1. Same contract as the reTerminal file: wait for release (so a held
// button cannot re-wake us instantly), arm EXT1, sleep.
#include "sticky_power.h"

#include <cstdio>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"
#include "sticky_display.h"
#include "sticky_sdcard.h"

namespace {

constexpr char kTag[] = "sticky_power";
constexpr gpio_num_t kWakePin = static_cast<gpio_num_t>(PIN_BTN_OK);

void wait_for_release()
{
    constexpr TickType_t kPoll = pdMS_TO_TICKS(20);
    constexpr TickType_t kTimeout = pdMS_TO_TICKS(10000);
    TickType_t waited = 0;
    while (gpio_get_level(kWakePin) == 0 && waited < kTimeout) {
        vTaskDelay(kPoll);
        waited += kPoll;
    }
    vTaskDelay(pdMS_TO_TICKS(60));
}

}  // namespace

void sticky_power_log_wakeup_reason()
{
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(kTag, "Woke from deep sleep by BOOT button (GPIO%d)", PIN_BTN_OK);
    }
}

[[noreturn]] void sticky_power_enter_deep_sleep()
{
    sticky_sdcard_prepare_sleep();
    // LCD boards: backlight off + panel SLPIN, so a sleeping board is dark
    // rather than a lit white page (headless boards answer ESP_OK).
    sticky_display_sleep();
    wait_for_release();

    gpio_set_direction(kWakePin, GPIO_MODE_INPUT);
    gpio_pullup_en(kWakePin);
    gpio_pulldown_dis(kWakePin);
    if (gpio_get_level(kWakePin) != 0) {
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_BTN_OK,
                                                        ESP_EXT1_WAKEUP_ANY_LOW));
        ESP_LOGI(kTag, "Wake on BOOT (GPIO%d low)", PIN_BTN_OK);
    } else {
        // Still held after 10 s: arming ANY_LOW now would wake instantly.
        ESP_LOGW(kTag, "BOOT still held - sleeping with timer wake only");
    }
    ESP_LOGI(kTag, "Entering deep sleep now");
    std::fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}
