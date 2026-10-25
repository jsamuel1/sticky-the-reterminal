// tiny_rotation — the pure rules behind MANUAL display rotation (grammar v25):
// which boards offer it, what the saved value boots as, which tap ids select
// an orientation, and the picker card the settings row opens. No IDF types:
// tests/host/rotation_host_test.sh compiles this exact header.
//
// A board WITH an accelerometer (board_caps.imu) follows gravity through
// tiny_orient and its settings row stays the auto/lock toggle; it never gets
// this picker. A board WITHOUT one (FNK0104S today) has nothing to follow, so
// the owner picks which edge is up and the choice is saved (NVS tiny/cfg
// "rotq", quarter turns). The orientation itself is applied through
// tiny_display_set_rotation(), the same commit/rollback path auto-rotate uses,
// so layout, tap regions and the gesture frame turn together.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// What an unset or unreadable saved value boots as: the panel's native frame.
#define TINY_ROTATION_DEFAULT_DEG 0

// The capability gate. Driven by board_caps.imu (the authoritative table),
// never by a board name or by an IMU read that happened to fail.
static inline bool tiny_rotation_manual_offered(bool board_has_imu) {
    return !board_has_imu;
}

// Every orientation the canvas can lay a card out in (Canvas rotates any
// panel in quarter turns; lcd_canvas and the e-ink push physical rows).
static inline int tiny_rotation_supported_count(void) { return 4; }
static inline int tiny_rotation_supported_deg(int i) {
    return (i >= 0 && i < 4) ? i * 90 : -1;
}
static inline bool tiny_rotation_valid_deg(int deg) {
    return deg == 0 || deg == 90 || deg == 180 || deg == 270;
}

// NVS stores quarter turns (0..3). `have` false = never written. Anything
// else (a future encoding, a torn write) boots the default, never trusted.
static inline uint8_t tiny_rotation_to_stored(int deg) {
    return (uint8_t)(deg / 90);
}
static inline int tiny_rotation_boot_deg(bool have, uint8_t stored) {
    if (!have || stored > 3) return TINY_ROTATION_DEFAULT_DEG;
    return stored * 90;
}

// Human name of an orientation. panel_portrait = the PHYSICAL panel is taller
// than wide (FNK0104S 320x480); a quarter turn swaps the aspect.
static inline const char *tiny_rotation_name(int deg, bool panel_portrait) {
    const bool tall = ((deg / 90) % 2 == 0) == panel_portrait;
    const bool flipped = deg == 180 || deg == 270;
    if (tall) return flipped ? "portrait, flipped" : "portrait";
    return flipped ? "landscape, flipped" : "landscape";
}

// Tap ids owned by the picker. Strict: only the exact supported degree values
// select an orientation; "rot:", "rot:45", "rot:90x", "rot:0090" are refused
// (atoi would read most of them as an angle).
typedef enum {
    TINY_ROTATION_ID_NONE = 0,  // not a rot: id at all
    TINY_ROTATION_ID_OPEN = 1,  // "rot:open" - show the picker
    TINY_ROTATION_ID_SET = 2,   // "rot:<deg>" - *deg holds a supported angle
    TINY_ROTATION_ID_BAD = 3,   // rot: prefix, anything else - refuse, no change
    TINY_ROTATION_ID_BACK = 4,  // "rot:back" - return to settings
} tiny_rotation_id_t;

static inline tiny_rotation_id_t tiny_rotation_parse_id(const char *id, int *deg) {
    if (!id || strncmp(id, "rot:", 4) != 0) return TINY_ROTATION_ID_NONE;
    const char *t = id + 4;
    if (!strcmp(t, "open")) return TINY_ROTATION_ID_OPEN;
    if (!strcmp(t, "back")) return TINY_ROTATION_ID_BACK;
    for (int i = 0; i < tiny_rotation_supported_count(); ++i) {
        char want[8];
        snprintf(want, sizeof want, "%d", tiny_rotation_supported_deg(i));
        if (!strcmp(t, want)) {
            if (deg) *deg = tiny_rotation_supported_deg(i);
            return TINY_ROTATION_ID_SET;
        }
    }
    return TINY_ROTATION_ID_BAD;
}

// The settings row label: the live angle. Short enough for the 320 px FNK
// portrait column; the orientation name rides in the row's note.
static inline void tiny_rotation_row_label(char *out, size_t cap, int deg) {
    snprintf(out, cap, "rotation: %d deg", deg);
}
static inline void tiny_rotation_row_note(char *out, size_t cap, int deg,
                                          bool panel_portrait) {
    snprintf(out, cap, "%s - tap to choose",
             tiny_rotation_name(deg, panel_portrait));
}

// The picker card (card_id "rotation", type menu): one row per supported
// orientation, the live one marked ACTIVE, then Back/Home. The choice is
// saved, so the next boot keeps it. Every string is a
// firmware constant, so snprintf cannot inject JSON. Returns the length
// written, or -1 when `cap` is too small (nothing usable in `out`).
static inline int tiny_rotation_picker_json(char *out, size_t cap,
                                            int current_deg,
                                            bool panel_portrait) {
    size_t n = 0;
    // A plain menu card, no caption: on a landscape turn of a 320x480 panel
    // only ~2 rows fit, and every pixel of caption would push one more
    // orientation below the scroll line (rows off-glass publish no regions).
    int w = snprintf(out, cap,
        "{\"type\":\"menu\",\"card_id\":\"rotation\",\"title\":\"rotation\","
        "\"items\":[");
    if (w < 0 || (size_t)w >= cap) return -1;
    n = (size_t)w;
    for (int i = 0; i < tiny_rotation_supported_count(); ++i) {
        const int d = tiny_rotation_supported_deg(i);
        w = snprintf(out + n, cap - n,
                     "%s{\"label\":\"%s\",\"note\":\"%d deg%s\","
                     "\"id\":\"rot:%d\"}",
                     i ? "," : "", tiny_rotation_name(d, panel_portrait), d,
                     d == current_deg ? " - ACTIVE" : "", d);
        if (w < 0 || (size_t)w >= cap - n) return -1;
        n += (size_t)w;
    }
    w = snprintf(out + n, cap - n,
                 "],\"buttons\":[{\"label\":\"Back\",\"id\":\"rot:back\"},"
                 "{\"label\":\"Home\",\"id\":\"home\"}]}");
    if (w < 0 || (size_t)w >= cap - n) return -1;
    return (int)(n + (size_t)w);
}

// What a picker tap does, with the side effects injected so the host test
// runs the shipped decision: refuse on a board with an accelerometer (gravity
// owns rotation there), apply through the display's commit path, and save
// ONLY what the panel took - a failed commit leaves the stored choice alone,
// so the next boot cannot come up in an angle the glass never showed.
// *persisted (optional) reports whether the save happened and succeeded.
typedef int (*tiny_rotation_apply_fn)(int deg);
typedef int (*tiny_rotation_save_fn)(uint8_t quarters);
static inline int tiny_rotation_select(int deg, bool board_has_imu,
                                       tiny_rotation_apply_fn apply,
                                       tiny_rotation_save_fn save,
                                       bool *persisted) {
    if (persisted) *persisted = false;
    if (!tiny_rotation_manual_offered(board_has_imu)) return 0x106;  // ESP_ERR_NOT_SUPPORTED
    if (!tiny_rotation_valid_deg(deg)) return 0x102;                 // ESP_ERR_INVALID_ARG
    const int err = apply(deg);
    if (err != 0) return err;
    const int serr = save(tiny_rotation_to_stored(deg));
    if (persisted) *persisted = serr == 0;
    return 0;
}
