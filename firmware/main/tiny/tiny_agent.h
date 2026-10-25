// tiny_agent — THE UNIVERSE ON THE GLASS (grammar v11).
// Which tiny answers the asks. Empty slug = the owner's own tiny, and the
// ask body stays byte-identical to pre-universe firmware (the backend pins
// this with a regression test). A non-empty slug rides the ask as
// `tiny:"<slug>"` (backend contract, tinyai-id 61b404f0) and the turn runs
// as that PUBLIC tiny — its identity, no owner tools (backend enforces).
//
// State is NVS-backed ("tinyagent"): the active slug survives reboot, plus
// a small roster of slugs the owner can switch between from the universe
// card. Roster is editable over relay (`agent add/rm`) and seeded once
// with "tiny" so the card is never an empty page on first open.
#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TINY_AGENT_SLUG_MAX 64   // backend clamps at 64 — refuse locally too
#define TINY_AGENT_ROSTER_MAX 8

// Active slug, "" when the owner's own tiny (default). Never NULL.
const char *tiny_agent_current(void);

// Every mutation below is all-or-nothing: the whole state (active slug +
// roster) is one NVS blob, and RAM takes the new state only once NVS has
// committed it. A non-ESP_OK return means nothing changed — in RAM, on
// flash, or after a reboot.

// Switch. NULL/"" clears back to the owner's own tiny. Trims whitespace,
// lowercases, refuses >64 chars or embedded spaces/quotes. Persists to NVS
// and (on a real switch) adds the slug to the roster.
esp_err_t tiny_agent_set(const char *slug);

// Roster access: returns count, copies up to max slugs into out[i] (each
// cap TINY_AGENT_SLUG_MAX+1 bytes).
int tiny_agent_roster(char out[][TINY_AGENT_SLUG_MAX + 1], int max);
esp_err_t tiny_agent_roster_add(const char *slug);
esp_err_t tiny_agent_roster_remove(const char *slug);

// A roster COPY, for showing what a merge would produce before committing
// it (a universe push paints the merged page, then commits the slugs before
// the page is published; a refused commit rolls the page back). The
// snapshot is atomic: the active slug and roster it carries are one state.
// preview_add applies exactly the rules of roster_add — normalisation,
// refusal, dedupe, full-roster eviction — to the copy only: no lock held
// afterwards, nothing written to RAM state or NVS.
typedef struct {
    char active[TINY_AGENT_SLUG_MAX + 1];
    char slugs[TINY_AGENT_ROSTER_MAX][TINY_AGENT_SLUG_MAX + 1];
    int n;
} tiny_agent_roster_t;
void tiny_agent_roster_snapshot(tiny_agent_roster_t *out);
esp_err_t tiny_agent_roster_preview_add(tiny_agent_roster_t *r, const char *slug);

// Commit a previewed roster as ONE write. `base` is the snapshot `next` was
// built from; if the live state no longer equals it (another task switched,
// added or removed meanwhile), nothing is written and ESP_ERR_INVALID_STATE
// comes back, because the page built from `base` no longer describes the
// roster. Any NVS failure is returned and leaves RAM and flash at `base`.
// The active slug is not changed (next->active must equal base->active).
esp_err_t tiny_agent_roster_commit(const tiny_agent_roster_t *base,
                                   const tiny_agent_roster_t *next);

#ifdef __cplusplus
}
#endif
