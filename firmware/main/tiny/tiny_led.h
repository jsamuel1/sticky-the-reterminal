// tiny_led — the FNK0104S WS2812 as an ambient state light.
//
// Built only with CONFIG_TINY_STATUS_LED (Kconfig.projbuild; FNK0104S only).
// On every other board these are inline no-ops, so callers never #if and the
// other images carry no RMT or led_strip code.
//
// Callers report what is happening; tiny_led_state.h decides the colour:
//   tiny_audio  -> LISTENING while the mic captures, IDLE after
//   tiny_node   -> THINKING while an upload or ask is in flight,
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

void tiny_led_init(void);  // RMT channel + strip; logs its internal-RAM cost
void tiny_led_set(tiny_led_activity_t activity);  // clears a showing error
void tiny_led_error(void);         // red for a few seconds, then off
void tiny_led_sync(void);          // re-resolve (lock state changed)
void tiny_led_prepare_sleep(void); // off, and stays off until reset

#else

static inline void tiny_led_init(void) {}
static inline void tiny_led_set(tiny_led_activity_t) {}
static inline void tiny_led_error(void) {}
static inline void tiny_led_sync(void) {}
static inline void tiny_led_prepare_sleep(void) {}

#endif

#ifdef __cplusplus
}
#endif
