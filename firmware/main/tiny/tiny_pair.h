// tiny_pair — on-device pairing (onboarding Phase B). The board enrols itself
// with a pairing code through tiny.technology's headless pairing API, the
// same contract tiny-tech `enroll` uses (dist/enroll.js pairDevice):
//
//   start  POST /api/devices/pair/start {name, platform, kind, capabilities,
//          secretHash}                      -> {code, ttl_s}
//   owner  approves at /devices/pair?code=<code> (glass QR / console / /info)
//   poll   POST /api/devices/pair/poll {code, secret} every 3 s
//          approved -> {device_id, device_token, name}: merge, reboot
//          denied   -> stop; the glass offers [New code]
//          expired / claimed / 404 -> a new code, automatically
//
// The secret is 32 random bytes, base64url, held in RAM only and never
// logged or exposed; only its sha256 leaves the board before approval. The
// code is public by design (it is printed on the glass).
//
// One FreeRTOS task, started while the device is unprovisioned with a saved
// network, ends itself once the device is provisioned (by us -> reboot, or by
// the portal /setup paste fallback -> reboot). Pure helpers live in
// tiny_pair_util.h so the host test compiles the same code.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TINY_PAIR_IDLE = 0,  // task not running (provisioned, or never started)
    TINY_PAIR_ASKING,    // waiting for Wi-Fi or for /pair/start to answer
    TINY_PAIR_WAITING,   // a code is live; polling for the owner's approval
    TINY_PAIR_DENIED,    // the owner said no; waits for tiny_pair_retry()
    TINY_PAIR_ERROR,     // /pair/start failed; retrying with backoff
    TINY_PAIR_APPROVED,  // identity saved; rebooting
} tiny_pair_state_t;

esp_err_t         tiny_pair_start(void);   // idempotent
tiny_pair_state_t tiny_pair_state(void);
const char       *tiny_pair_state_name(tiny_pair_state_t s);
// Copy the live code ("" when none). Returns true when a code is live.
bool              tiny_pair_code(char *out, size_t cap);
// The owner's approval link for the live code ({api}/devices/pair?code=...).
bool              tiny_pair_url(char *out, size_t cap);
int               tiny_pair_expires_in_s(void);  // 0 when no code is live
int               tiny_pair_last_http(void);     // last start failure: HTTP status, -1 transport
void              tiny_pair_retry(void);         // after DENIED/ERROR: a new code now

#ifdef __cplusplus
}
#endif
