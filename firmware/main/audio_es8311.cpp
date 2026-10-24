#include "audio_es8311.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "board.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "audio_es8311";

// Mic gain into the ES8311 PGA. 30 dB is the vendor XiaoZhi default for this
// codec + analog MEMS mic pairing; miccheck reports what it actually yields.
constexpr float kMicGainDb = 30.0f;
// The amp switches only around silence. Lead: the SC8002B leaves shutdown
// in well under this; tail: longer than the TX DMA ring (4 x 240 frames =
// 60 ms) so the last note is fully out before the amp turns off.
constexpr uint32_t kPaLeadMs = 30;
constexpr uint32_t kTailMs = 120;
constexpr size_t kMaxNotes = 32;
constexpr size_t kChunk = 240;  // frames per write = one DMA descriptor

struct PlayItem {
    TinyNote notes[kMaxNotes];
    uint8_t n;
    int16_t amp;
};

bool s_tried = false;
esp_err_t s_init_err = ESP_ERR_INVALID_STATE;
i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;
const audio_codec_if_t *s_codec = nullptr;
QueueHandle_t s_play_q = nullptr;
volatile int s_pending = 0;  // queued + playing items; 0 = amp off

void pa_set(bool on)
{
    const int level = on ? AUDIO_EN_ON_LEVEL : !AUDIO_EN_ON_LEVEL;
    gpio_set_level(static_cast<gpio_num_t>(PIN_AUDIO_EN), level);
}

esp_err_t pa_init()
{
    // Latch the OFF level before the pin becomes an output so it never
    // glitches ON (FNK: active-LOW behind a pull-up; AMOLED: active-HIGH).
    pa_set(false);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_AUDIO_EN;
    io.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&io), kTag, "pa gpio");
    pa_set(false);
    return ESP_OK;
}

void write_silence(uint32_t ms)
{
    static int16_t zeros[2 * kChunk];
    uint32_t frames = AUDIO_ES8311_RATE * ms / 1000;
    while (frames) {
        const size_t n = frames < kChunk ? frames : kChunk;
        size_t w = 0;
        if (i2s_channel_write(s_tx, zeros, n * 4, &w, pdMS_TO_TICKS(200)) != ESP_OK) return;
        frames -= n;
    }
}

void play_item(const PlayItem &it)
{
    static int16_t buf[2 * kChunk];
    for (size_t i = 0; i < it.n; ++i) {
        TinyTone t;
        tiny_tone_start(&t, AUDIO_ES8311_RATE, it.notes[i], it.amp);
        size_t n;
        while ((n = tiny_tone_render(&t, buf, kChunk)) > 0) {
            size_t w = 0;
            if (i2s_channel_write(s_tx, buf, n * 4, &w, pdMS_TO_TICKS(200)) != ESP_OK) {
                ESP_LOGW(kTag, "tx write stalled");
                return;
            }
        }
    }
}

void player_task(void *)
{
    PlayItem it;
    for (;;) {
        if (xQueueReceive(s_play_q, &it, portMAX_DELAY) != pdTRUE) continue;
        pa_set(true);
        vTaskDelay(pdMS_TO_TICKS(kPaLeadMs));
        // Back-to-back chimes share one amp-on window: no extra toggles.
        for (;;) {
            play_item(it);
            __atomic_sub_fetch(&s_pending, 1, __ATOMIC_SEQ_CST);
            if (xQueueReceive(s_play_q, &it, 0) != pdTRUE) break;
        }
        write_silence(kTailMs);
        // A chime queued during the tail rides the next amp window.
        pa_set(false);
    }
}

esp_err_t i2s_up()
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // DMA rings live in INTERNAL RAM (the colour boards' scarcest rail; the
    // TLS upload pre-flight needs 60 KB of it). 4 x 240 frames = 60 ms per
    // direction, 3.75 KB each: ample for 16 kHz tones and a polled capture.
    cc.dma_desc_num = 4;
    cc.dma_frame_num = 240;
    // An empty TX queue must play ZEROS, not loop the last note: the DAC
    // idles at mid-rail and the amp toggles against silence.
    cc.auto_clear_after_cb = true;
    cc.auto_clear_before_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_tx, &s_rx), kTag, "i2s channels");

    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_ES8311_RATE);
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = static_cast<gpio_num_t>(PIN_I2S_MCLK);
    sc.gpio_cfg.bclk = static_cast<gpio_num_t>(PIN_I2S_BCLK);
    sc.gpio_cfg.ws = static_cast<gpio_num_t>(PIN_I2S_WS);
    sc.gpio_cfg.dout = static_cast<gpio_num_t>(PIN_I2S_DOUT);
    sc.gpio_cfg.din = static_cast<gpio_num_t>(PIN_I2S_DIN);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &sc), kTag, "tx std");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &sc), kTag, "rx std");
    // MCLK must run before the codec is configured: the ES8311 is clocked
    // from it. Both halves stay enabled for the life of the firmware.
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), kTag, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), kTag, "rx enable");
    return ESP_OK;
}

esp_err_t codec_up()
{
    i2c_master_bus_handle_t bus = board_sensor_i2c_bus();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, kTag, "no shared i2c bus");

    audio_codec_i2c_cfg_t ic = {};
    ic.port = I2C_NUM_0;
    ic.addr = ES8311_I2C_ADDR << 1;  // the codec library takes the 8-bit form
    ic.bus_handle = bus;
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ic);
    ESP_RETURN_ON_FALSE(ctrl, ESP_FAIL, kTag, "codec i2c ctrl");

    es8311_codec_cfg_t ec = {};
    ec.ctrl_if = ctrl;
    ec.gpio_if = nullptr;
    ec.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    ec.pa_pin = -1;          // this module owns the amp pin
    ec.master_mode = false;  // the S3 drives BCLK/WS
    ec.use_mclk = true;
    ec.mclk_div = 256;
    ec.no_dac_ref = true;    // right ADC slot empty, not a DAC loopback
    s_codec = es8311_codec_new(&ec);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, kTag, "es8311 open (codec at 0x%02X?)",
                        ES8311_I2C_ADDR);

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 2;
    fs.sample_rate = AUDIO_ES8311_RATE;
    ESP_RETURN_ON_FALSE(s_codec->set_fs(s_codec, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL,
                        kTag, "es8311 set_fs");
    ESP_RETURN_ON_FALSE(s_codec->enable(s_codec, true) == ESP_CODEC_DEV_OK, ESP_FAIL,
                        kTag, "es8311 enable");
    s_codec->set_vol(s_codec, 0.0f);
    s_codec->set_mic_gain(s_codec, kMicGainDb);
    return ESP_OK;
}

}  // namespace

esp_err_t audio_es8311_init(void)
{
    if (s_tried) return s_init_err;
    s_tried = true;
    const size_t internal_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = pa_init();
    if (err == ESP_OK) err = i2s_up();
    if (err == ESP_OK) err = codec_up();
    if (err == ESP_OK) {
        // Queue storage and the player stack go to PSRAM: internal RAM is
        // what TLS needs, and nothing here runs during a flash write.
        s_play_q = xQueueCreateWithCaps(4, sizeof(PlayItem), MALLOC_CAP_SPIRAM);
        if (!s_play_q ||
            xTaskCreateWithCaps(player_task, "audio_play", 4096, nullptr, 5, nullptr,
                                MALLOC_CAP_SPIRAM) != pdPASS)
            err = ESP_ERR_NO_MEM;
    }
    s_init_err = err;
    const size_t internal_after =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(kTag, "ES8311 @0x%02X I2S0 MCLK=%d BCLK=%d WS=%d DIN=%d DOUT=%d, "
             "PA GPIO%d active-%s: %s (internal %u -> %u B)",
             ES8311_I2C_ADDR, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DIN,
             PIN_I2S_DOUT, PIN_AUDIO_EN, AUDIO_EN_ON_LEVEL ? "HIGH" : "LOW",
             esp_err_to_name(err), (unsigned)internal_before, (unsigned)internal_after);
    return err;
}

bool audio_es8311_ready(void) { return s_tried && s_init_err == ESP_OK; }

esp_err_t audio_es8311_play(const TinyNote *notes, size_t n, int16_t amp)
{
    if (!audio_es8311_ready()) return ESP_ERR_INVALID_STATE;
    if (!notes || !n) return ESP_ERR_INVALID_ARG;
    PlayItem local = {};  // the queue copies it
    local.n = static_cast<uint8_t>(n < kMaxNotes ? n : kMaxNotes);
    memcpy(local.notes, notes, local.n * sizeof(TinyNote));
    local.amp = amp;
    __atomic_add_fetch(&s_pending, 1, __ATOMIC_SEQ_CST);
    if (xQueueSend(s_play_q, &local, 0) != pdTRUE) {
        __atomic_sub_fetch(&s_pending, 1, __ATOMIC_SEQ_CST);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool audio_es8311_wait_idle(uint32_t timeout_ms)
{
    // s_pending reaches 0 when the last note is written; the tail and the
    // amp-off follow within kTailMs.
    TickType_t waited = 0;
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while (__atomic_load_n(&s_pending, __ATOMIC_SEQ_CST) > 0 && waited < limit) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += pdMS_TO_TICKS(10);
    }
    if (__atomic_load_n(&s_pending, __ATOMIC_SEQ_CST) > 0) return false;
    vTaskDelay(pdMS_TO_TICKS(kTailMs + 20));
    return true;
}

size_t audio_es8311_read(int16_t *stereo, size_t frames, uint32_t timeout_ms)
{
    if (!audio_es8311_ready() || !stereo || !frames) return 0;
    size_t got = 0;
    // A timeout still reports the bytes that did arrive.
    (void)i2s_channel_read(s_rx, stereo, frames * 4, &got, pdMS_TO_TICKS(timeout_ms));
    return got / 4;
}
