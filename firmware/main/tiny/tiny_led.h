// tiny_led — the FNK0104S WS2812 as an ambient state light.
//
// Built only with CONFIG_TINY_STATUS_LED (Kconfig.projbuild; FNK0104S only).
// On every other board these are inline no-ops, so callers never #if and the
// other images carry no RMT or led_strip code.
//
// Callers report what is happening; tiny_led_state.h decides the colour:
//   tiny_audio  -> tiny_led_capture_begin/_end around every mic capture
//   tiny_node   -> tiny_led_ask(PENDING) before a voice ask's capture (the
//                  capture's OK end promotes it to THINKING atomically),
//                  tiny_led_ask(THINKING) for a text ask, IDLE on return,
//                  tiny_led_error() when the askfail card is painted
//   tiny_lock   -> tiny_led_sync() on every lock change (locked = dark)
//   sticky_power_generic -> tiny_led_prepare_sleep() before deep sleep
#pragma once

#include "sdkconfig.h"
#include "tiny_led_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_TINY_STATUS_LED

// Single caller: app_main's init_inputs, before any other task can reach the
// light. Not safe to race with itself; a failed init leaves every other entry
// point a no-op.
void tiny_led_init(void);  // RMT channel + strip; logs its internal-RAM cost
void tiny_led_capture_begin(void);       // blue; clears a showing error
void tiny_led_capture_end(bool ok);      // ok + pending ask -> amber, same frame
void tiny_led_ask(tiny_led_ask_t state); // non-IDLE clears a showing error
void tiny_led_error(void);         // red for a few seconds, then off
void tiny_led_sync(void);          // re-resolve (lock state changed)
void tiny_led_prepare_sleep(void); // off, and stays off until reset

#else

static inline void tiny_led_init(void) {}
static inline void tiny_led_capture_begin(void) {}
static inline void tiny_led_capture_end(bool) {}
static inline void tiny_led_ask(tiny_led_ask_t) {}
static inline void tiny_led_error(void) {}
static inline void tiny_led_sync(void) {}
static inline void tiny_led_prepare_sleep(void) {}

#endif

#ifdef __cplusplus
}
#endif
