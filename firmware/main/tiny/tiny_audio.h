// tiny_audio — mic capture (16kHz/16-bit WAV in PSRAM, cap 15s) + chimes.
// reTerminal: PDM mic + piezo buzzer. Colour boards: ES8311 ADC/DAC over
// I2S std (audio_es8311.cpp); chime names and RTTTL render as PCM tones.
// Buzzer = chimes/RTTTL (tier 1). PCM-through-PWM is a timeboxed EXPERIMENT
// (tier 2) — measure, don't promise (docs/ARCHITECTURE.md §Audio).
// The mic pin-mux fix after a deep-sleep wake lives inside the capture path.
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t tiny_audio_init(void);
esp_err_t tiny_audio_record_wav(int seconds, uint8_t **wav, size_t *len); // PSRAM, caller frees
esp_err_t tiny_audio_chime(const char *name_or_rtttl);                    // "ack","ask","error",...

// Hold-to-talk (colour boards; no-ops on the reTerminal). arm(true) makes the
// NEXT capture (within 3 s) run until tiny_audio_talk_release() or the 15 s
// cap, ignoring its `seconds`; arm(false) cancels an unconsumed arm.
void tiny_audio_talk_arm(bool armed);
void tiny_audio_talk_release(void);
#ifdef __cplusplus
}
#endif
