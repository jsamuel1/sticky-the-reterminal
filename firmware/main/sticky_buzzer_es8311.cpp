// sticky_buzzer_es8311 — the sticky_buzzer_* API on the codec boards.
//
// The reTerminal has a piezo on LEDC; the FNK0104S and AMOLED-1.8 have an
// ES8311 DAC + amp. Same words, same timings (tiny_pcm's "beep"/"double"),
// so every caller of the buzzer API — touch acks, lock blips, shell toggles —
// sounds the same vocabulary on every board. Tones are queued to the
// audio_es8311 player task, so a beep never blocks a button or touch
// callback the way an LEDC busy-wait did.
#include "sticky_buzzer.h"

#include "audio_es8311.h"
#include "esp_log.h"
#include "tiny_pcm.h"

namespace {

// Normal = -12 dBFS sine, soft = 1/8 of that (the reTerminal soft tier is
// duty/8 too). Speaker loudness is set by ear on the bench, like the piezo.
constexpr int16_t kAmp = 8000;
constexpr int16_t kAmpSoft = 1000;

volatile bool s_silent = false;
volatile bool s_soft = false;

esp_err_t play_named(const char *name)
{
    if (s_silent) return ESP_OK;  // UI_SPEC §7: silence is a success
    TinyNote notes[8];
    const size_t n = tiny_pcm_chime_notes(name, notes, 8);
    if (!n) return ESP_ERR_INVALID_ARG;
    return audio_es8311_play(notes, n, s_soft ? kAmpSoft : kAmp);
}

}  // namespace

esp_err_t sticky_buzzer_init()
{
    // Boot ESP_ERROR_CHECKs this (on the reTerminal a dead buzzer is fatal).
    // A codec that does not answer must not cost the relay here: log it and
    // let the beeps report INVALID_STATE.
    const esp_err_t err = audio_es8311_init();
    if (err != ESP_OK)
        ESP_LOGW("sticky_buzzer", "codec tones unavailable: %s", esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t sticky_buzzer_beep() { return play_named("beep"); }
esp_err_t sticky_buzzer_beep_double() { return play_named("double"); }

void sticky_buzzer_set_silent(bool silent) { s_silent = silent; }
bool sticky_buzzer_silent(void) { return s_silent; }
void sticky_buzzer_set_soft(bool soft) { s_soft = soft; }
bool sticky_buzzer_soft(void) { return s_soft; }

int16_t sticky_buzzer_amp(void) { return s_soft ? kAmpSoft : kAmp; }
