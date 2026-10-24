// tiny_sd_policy — the decisions of the SDMMC microSD driver that do not
// touch hardware, kept IDF-free so tests/host/sd_policy_host_test.sh can
// compile them on a laptop.
//
// The SDMMC slot (FNK0104S) has no card-detect switch, so the only way to
// learn whether a card is in the slot is to talk to it. That probe costs a
// host init, a CMD0/CMD8/ACMD41 handshake and a host deinit, on whichever
// task asked. These rules decide when to pay it and how to read the answer.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// What one mount attempt says about the slot.
typedef enum {
    TINY_SD_ABSENT = 0,  // nothing answered and the probe timed out
    TINY_SD_UNREADABLE,  // card answered; FAT or data transfer failed
    TINY_SD_MOUNTED,     // FAT live at /sdcard
    TINY_SD_ERROR,       // software/config/other probe failure
} tiny_sd_probe_t;

// The mount's return code alone cannot say how far a probe got: the IDF
// host reports a data-phase error that is not a timeout or CRC as ESP_FAIL
// (the same code f_mount's "no filesystem" returns), and ESP_ERR_TIMEOUT is
// both "no card answered CMD8/ACMD41" and "a data block never arrived". So
// the driver traces every command of a probe (it wraps the host's
// do_transaction) and the rules below read that trace, not the code.
typedef struct {
    bool answered;     // some command that expects a response got one
    bool data_failed;  // a command that moves a data block failed
} tiny_sd_trace_t;

// Record one finished transaction. result = the transaction's own return,
// or, when that is ESP_OK, the command's error. An empty slot answers
// nothing (CMD0 has no response; CMD8, CMD55 and CMD52 time out), so
// `answered` stays false. A data command (SCR, SSR, a sector read) is only
// issued to a card that already answered identification.
static inline void tiny_sd_trace_note(tiny_sd_trace_t *t, bool expects_response,
                                      bool carries_data, esp_err_t result) {
    if (result == ESP_OK) {
        if (expects_response) t->answered = true;
        return;
    }
    if (carries_data) t->data_failed = true;
}

// SDMMC errors a bad D1..D3 line can produce after identification.
static inline bool tiny_sd_bus_error(esp_err_t err) {
    return err == ESP_FAIL || err == ESP_ERR_TIMEOUT ||
           err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_RESPONSE;
}

static inline tiny_sd_probe_t tiny_sd_classify(esp_err_t mount_err,
                                               tiny_sd_trace_t t) {
    if (mount_err == ESP_OK) return TINY_SD_MOUNTED;
    if (!t.answered)
        return mount_err == ESP_ERR_TIMEOUT ? TINY_SD_ABSENT : TINY_SD_ERROR;
    if (mount_err == ESP_FAIL && !t.data_failed) return TINY_SD_UNREADABLE;
    return t.data_failed && tiny_sd_bus_error(mount_err)
               ? TINY_SD_UNREADABLE
               : TINY_SD_ERROR;
}

// Whether a failed attempt at a wide bus is worth retrying 1-bit: the card
// answered and a data transfer failed with a signal/protocol error that a
// broken D1..D3 can produce. IDF's SDMMC host reports those as ESP_FAIL,
// TIMEOUT, INVALID_CRC or INVALID_RESPONSE. Software/configuration failures
// (INVALID_ARG, INVALID_SIZE, INVALID_STATE) and heap exhaustion are not
// width problems and must not double the probe.
static inline bool tiny_sd_retry_narrower(esp_err_t mount_err,
                                          tiny_sd_trace_t t) {
    return tiny_sd_bus_error(mount_err) && t.answered && t.data_failed;
}

// A narrower retry cannot erase evidence that the wide attempt reached data.
// If 4-bit answered and failed data but 1-bit gets no response, the honest
// outcome is still an unreadable card, not an empty slot.
static inline tiny_sd_probe_t tiny_sd_fallback_outcome(
    tiny_sd_probe_t wide, tiny_sd_probe_t narrow) {
    return wide == TINY_SD_UNREADABLE && narrow == TINY_SD_ABSENT
               ? TINY_SD_UNREADABLE
               : narrow;
}

// A card that answered and moved every block it was asked for, then failed
// with ESP_FAIL: f_mount found no FAT (a factory exFAT card). Only then is
// "`sd format yes`" the right hint.
static inline bool tiny_sd_fat_suspect(esp_err_t mount_err, tiny_sd_trace_t t) {
    return mount_err == ESP_FAIL && t.answered && !t.data_failed;
}

// Bus widths to try, in order, given the board's wired width and the width
// that last worked. Wired 4-bit tries 4 then 1 (the card spec requires every
// SD card to work 1-bit, so a bad D1..D3 still mounts, and the driver logs
// it). Once 1-bit is the width that worked, keep it: retrying 4-bit on every
// probe would re-fail the same broken line. Returns the count written (1..2).
static inline int tiny_sd_widths(int wired, int last_good, int out[2]) {
    if (wired >= 4 && last_good != 1) {
        out[0] = 4;
        out[1] = 1;
        return 2;
    }
    out[0] = 1;
    return 1;
}

// Re-probe backoff for an EMPTY slot, called from the hot-plug path
// (sticky_sdcard_ensure) that gallery flips and photo saves hit. Without
// it, every flip on a card-less board would pay a full probe. failures = the
// number of consecutive absent probes so far (>= 1). 2 s, 4 s, then 8 s
// forever: a card the owner just pushed in is seen within 8 s of the next
// file operation.
static inline uint32_t tiny_sd_backoff_ms(uint32_t failures) {
    if (failures <= 1) return 2000;
    if (failures == 2) return 4000;
    return 8000;
}

// Whether a probe is due at now_ms. last_ms = when the last absent probe
// finished; failures = consecutive absent probes (0 = never probed, always
// due). Wrap-safe for a uint32_t millisecond clock.
static inline bool tiny_sd_probe_due(uint32_t now_ms, uint32_t last_ms,
                                     uint32_t failures) {
    if (failures == 0) return true;
    return (uint32_t)(now_ms - last_ms) >= tiny_sd_backoff_ms(failures);
}

#ifdef __cplusplus
}
#endif
