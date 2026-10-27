// tiny_button — physical controls, the no-relay path to talk to tiny.
// AI/OK button (GPIO4, active-low) single click → voice ask: chime, record
// 6s, upload, /api/devices/ask, answer renders on the panel. The iot_button
// callback runs on the button component's timer task and MUST NOT block for
// the ~30s a full ask takes, so the click only feeds a depth-1 queue that a
// dedicated worker task drains — a second click during an ask is dropped,
// not queued (double-press ≠ two questions).
// UP/DOWN (GPIO5/6) single click → device event upstream (page verbs later).
#pragma once
#include "esp_err.h"
#include <stdint.h>

// The widest composer value (bytes, NUL included) any producer can hand the
// ask worker. 160 since v28: dictation fills a field far faster than e-ink
// typing ever could, and the keyboard buffer grew to match.
#define TINY_ASK_TEXT_CAP 160
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t tiny_button_init(void);
// Start a voice ask (chime, 6s record, upload, /api/devices/ask, render).
// Non-blocking: feeds the same depth-1 queue the physical AI button uses, so a
// screen tap on "Ask tiny" and the hardware button can never run two asks at
// once. Returns ESP_ERR_INVALID_STATE when an ask is already in flight.
esp_err_t tiny_button_request_voice_ask(void);
// A TYPED question through the same single-flight door (keyboard "ok").
// The text is copied into the queue item, so the caller's buffer is free the
// moment this returns. ESP_ERR_INVALID_STATE = an ask is already in flight;
// ESP_ERR_INVALID_SIZE = over TINY_ASK_TEXT_CAP - 1 (refused, never clipped).
esp_err_t tiny_button_request_text_ask(const char *text);
// v28: dictate into the composer opened as generation `gen` (tiny_shell). The
// worker records, transcribes, and hands the words to
// tiny_shell_dictation_result. Same single-flight queue as the asks:
// INVALID_STATE while one is in flight, NOT_ALLOWED while pocket-locked.
esp_err_t tiny_button_request_dictation(uint32_t gen);
#ifdef __cplusplus
}
#endif
