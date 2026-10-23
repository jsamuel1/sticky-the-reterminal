// audio_es8311 — the ES8311 + I²S std audio path shared by the colour boards.
//
// One module, two boards: every pin and the amp-enable polarity come from
// pin_config_<board>.h (PIN_I2S_* / PIN_AUDIO_EN / AUDIO_EN_ON_LEVEL), so the
// AMOLED-1.8 follows the FNK0104S with no code change.
//
// Shape:
//   * ESP32-S3 is I²S master on I2S0, full duplex, 16 kHz, 16-bit stereo
//     slots, MCLK = 256 fs (4.096 MHz). Both channels stay enabled from init:
//     the ES8311 is clocked by MCLK, and on the S3 the RX half is clocked by
//     the TX half, so stopping either costs a codec re-sync.
//   * espressif/esp_codec_dev drives the ES8311 registers over the board's
//     ONE shared I²C bus (board_sensor_i2c_bus()); it never touches the amp
//     pin — this module owns it.
//   * TX underruns play zeros (auto-clear), so an idle DAC sits at mid-rail
//     and the amp can switch without a step. The amp is enabled only while a
//     tone sequence plays, with silence before and after it.
//   * Tones play on a dedicated task: a chime never blocks a button or touch
//     callback.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tiny_pcm.h"

#define AUDIO_ES8311_RATE 16000

// Idempotent. Returns the result of the first attempt on every later call.
esp_err_t audio_es8311_init(void);
bool audio_es8311_ready(void);

// Queue a tone sequence (copied). amp is the peak sample value.
// ESP_ERR_INVALID_STATE when the codec is not up, ESP_ERR_TIMEOUT when the
// queue is full (the tone is dropped, never blocks).
esp_err_t audio_es8311_play(const TinyNote *notes, size_t n, int16_t amp);

// Block until queued tones have finished and the amp is off, or timeout.
bool audio_es8311_wait_idle(uint32_t timeout_ms);

// Read interleaved stereo s16 frames from the ADC. Returns frames read.
size_t audio_es8311_read(int16_t *stereo, size_t frames, uint32_t timeout_ms);

// Peak amplitude for the current volume tier (sticky_buzzer_set_soft).
// Defined in sticky_buzzer_es8311.cpp, next to the tier flag it reads.
int16_t sticky_buzzer_amp(void);
