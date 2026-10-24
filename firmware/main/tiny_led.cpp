// tiny_led — WS2812 status light on the FNK0104S (GPIO42). See tiny/tiny_led.h.
//
// One pixel through espressif/led_strip's RMT backend. The cost is held to
// the minimum the driver allows: one TX channel, 48 symbols of RMT memory (the
// S3's per-channel block, so no second block is chained), no DMA, the
// component's 4-deep transaction queue, and a 3-byte pixel buffer. init logs
// the measured internal-RAM delta next to the absolute figure. Static cost on
// the fnk0104s image (idf.py size, IDF 5.4): DIRAM +2952 B, almost all of it
// the RMT TX ISR and encoders, which IDF 5.4 marks IRAM_ATTR unconditionally.
//
// Writes come from several tasks (button worker, node task, esp_timer task
// for the error expiry), so every state change, the error timer's re-arm and
// the resolve + refresh run under one mutex. s_mux is published only after
// the strip and timer exist, so a non-null s_mux means a complete light;
// every runtime entry point returns early on null.
// A refresh is ~30 us on the wire plus the 280 us reset latch.
#include "sdkconfig.h"

#if CONFIG_TINY_STATUS_LED

#include "tiny/tiny_led.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "pin_config.h"
#include "tiny/tiny_lock.h"

static const char *TAG = "tiny_led";

namespace {

constexpr int64_t kErrorShowUs = 5 * 1000 * 1000;  // red lingers 5 s
constexpr uint8_t kBrightness = CONFIG_TINY_STATUS_LED_BRIGHTNESS;

led_strip_handle_t s_strip = nullptr;
SemaphoreHandle_t s_mux = nullptr;
esp_timer_handle_t s_err_timer = nullptr;
tiny_led_inputs_t s_in = {};
tiny_led_rgb_t s_shown = {0, 0, 0};
bool s_written = false;  // nothing sent yet: the first apply always writes

// Caller holds s_mux.
void apply_locked()
{
    if (!s_strip) return;
    s_in.locked = tiny_lock_is_locked();
    const tiny_led_rgb_t c =
        tiny_led_resolve(&s_in, esp_timer_get_time(), kBrightness);
    if (s_written && c.r == s_shown.r && c.g == s_shown.g && c.b == s_shown.b)
        return;
    esp_err_t err = (c.r | c.g | c.b)
                        ? led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b)
                        : ESP_OK;
    if (err == ESP_OK)
        err = (c.r | c.g | c.b) ? led_strip_refresh(s_strip)
                                : led_strip_clear(s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write %u,%u,%u: %s", c.r, c.g, c.b, esp_err_to_name(err));
        return;
    }
    s_shown = c;
    s_written = true;
}

void apply()
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    apply_locked();
    xSemaphoreGive(s_mux);
}

void on_error_expired(void *) { apply(); }

// Caller holds s_mux. Keeps the expiry timer in step with error_until_us:
// stopped when no error shows, (re)armed for the full window when one
// starts. Holding s_mux across stop+start means two error reports cannot
// interleave their stop/start pairs. esp_timer_stop does not wait for a
// running callback, so a callback blocked on s_mux cannot deadlock this;
// it simply re-resolves after we release, which is idempotent.
void rearm_error_timer_locked(bool showing)
{
    esp_err_t err = esp_timer_stop(s_err_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)  // INVALID_STATE: not armed
        ESP_LOGW(TAG, "error timer stop: %s", esp_err_to_name(err));
    if (!showing) return;
    err = esp_timer_start_once(s_err_timer, kErrorShowUs + 1000);
    if (err != ESP_OK)
        // Red would then only clear on the next activity change; say so.
        ESP_LOGW(TAG, "error timer start: %s - red holds until next activity",
                 esp_err_to_name(err));
}

}  // namespace

// Called once, from app_main's init_inputs (a single task, before the
// button worker, node task or lock timer can reach any tiny_led_* call).
// The unlocked re-entry guard relies on that; every other entry point
// tolerates a light that never finished init (s_mux stays null).
extern "C" void tiny_led_init(void)
{
    if (s_mux) return;
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Build everything locally and publish s_mux last: a half-initialised
    // light (mutex without timer or strip) must stay invisible to callers.
    SemaphoreHandle_t mux = xSemaphoreCreateMutex();
    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t ta = {};
    ta.callback = on_error_expired;
    ta.name = "led_err";
    if (!mux || esp_timer_create(&ta, &timer) != ESP_OK) {
        ESP_LOGE(TAG, "init: no memory for mutex/timer - light stays dark");
        if (mux) vSemaphoreDelete(mux);
        return;
    }

    led_strip_config_t sc = {};
    sc.strip_gpio_num = PIN_WS2812;
    sc.max_leds = 1;
    sc.led_model = LED_MODEL_WS2812;
    sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rc = {};
    rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = 10 * 1000 * 1000;  // 0.1 us ticks: WS2812 T0H/T1H fit
    rc.mem_block_symbols = 48;            // one S3 block (SOC_RMT_MEM_WORDS_PER_CHANNEL)
    rc.flags.with_dma = 0;
    led_strip_handle_t strip = nullptr;
    esp_err_t err = led_strip_new_rmt_device(&sc, &rc, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: led_strip_new_rmt_device: %s - light stays dark",
                 esp_err_to_name(err));
        esp_timer_delete(timer);
        vSemaphoreDelete(mux);
        return;
    }
    s_err_timer = timer;
    s_strip = strip;
    s_mux = mux;  // published last: from here callers may take it
    xSemaphoreTake(s_mux, portMAX_DELAY);
    // A soft reset (OTA, panic) leaves the pixel latched at whatever it last
    // showed; the first apply writes black unconditionally.
    apply_locked();
    xSemaphoreGive(s_mux);

    const size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "status light on GPIO%d, brightness %u/255: internal -%d B "
                  "(internal_free %u -> %u)",
             PIN_WS2812, kBrightness, (int)before - (int)after, (unsigned)before,
             (unsigned)after);
}

extern "C" void tiny_led_capture_begin(void)
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const bool had_error = s_in.error_until_us != 0;
    tiny_led_on_capture_begin(&s_in);  // a new capture supersedes the failure
    if (had_error) rearm_error_timer_locked(false);
    apply_locked();
    xSemaphoreGive(s_mux);
}

extern "C" void tiny_led_capture_end(bool ok)
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    // One resolve for "capture over" and "pending ask now thinking": blue
    // goes straight to amber, never through a latched black frame.
    tiny_led_on_capture_end(&s_in, ok);
    apply_locked();
    xSemaphoreGive(s_mux);
}

extern "C" void tiny_led_ask(tiny_led_ask_t state)
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (tiny_led_on_ask(&s_in, state)) rearm_error_timer_locked(false);
    apply_locked();
    xSemaphoreGive(s_mux);
}

extern "C" void tiny_led_error(void)
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    tiny_led_on_error(&s_in, esp_timer_get_time() + kErrorShowUs);
    rearm_error_timer_locked(true);  // same critical section as the deadline
    apply_locked();
    xSemaphoreGive(s_mux);
}

// Attention layer. Steady inputs with no timer: the light follows them until
// the cause changes (another card commits / the heartbeat reports 0).
extern "C" void tiny_led_alert(bool on)
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_in.alert = on;
    apply_locked();  // unchanged colour writes nothing (s_shown check)
    xSemaphoreGive(s_mux);
}

extern "C" void tiny_led_unread(bool on)
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_in.unread = on;
    apply_locked();
    xSemaphoreGive(s_mux);
}

extern "C" void tiny_led_sync(void) { apply(); }

extern "C" void tiny_led_prepare_sleep(void)
{
    if (!s_mux) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_in.asleep = true;  // never cleared: deep sleep ends in a reset
    rearm_error_timer_locked(false);  // no late expiry after the channel goes
    apply_locked();
    // A WS2812 holds the last frame it latched for as long as it has power,
    // so black is written before the rails idle (whether the FNK0104S cuts
    // the LED rail in deep sleep is not documented; this does not rely on it).
    // Releasing the channel leaves GPIO42 a pulled-down input; a WS2812 only
    // changes on a timed 800 kHz frame, which a quiet pad does not produce.
    if (s_strip) {
        led_strip_del(s_strip);
        s_strip = nullptr;
    }
    xSemaphoreGive(s_mux);
    gpio_set_direction(static_cast<gpio_num_t>(PIN_WS2812), GPIO_MODE_INPUT);
    gpio_pulldown_en(static_cast<gpio_num_t>(PIN_WS2812));
}

#endif  // CONFIG_TINY_STATUS_LED
