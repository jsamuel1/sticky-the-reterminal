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
// accepted it. A failed NVS write is outcome-unknown (IDF writes the new
// version before erasing the old), so the result says what flash holds:
//   ESP_OK                the new state is on flash and in RAM. Includes
//                         ESP_ERR_NVS_REMOVE_FAILED from set_blob (IDF: new
//                         value written, old removal finished at init), and
//                         a failed write whose read-back found the new state.
//   TINY_AGENT_UNSETTLED  the write failed AND restoring the old state
//                         failed AND flash could not be read back as either:
//                         RAM is the old state, flash holds the old or the
//                         new whole state (never a mix). The next mutation
//                         rewrites flash even if it changes nothing, so the
//                         first healthy write settles it onto RAM.
//   any other error       nothing changed — in RAM, on flash, after a reboot.
#define TINY_AGENT_UNSETTLED ((esp_err_t)0x7D11)
// esp_err_to_name() that also names TINY_AGENT_UNSETTLED.
const char *tiny_agent_err_name(esp_err_t err);

// Switch. NULL/"" clears back to the owner's own tiny. Trims whitespace,
// lowercases, refuses >64 chars or embedded spaces/quotes. Persists to NVS
// and (on a real switch) adds the slug to the roster.
esp_err_t tiny_agent_set(const char *slug);

// Roster access: returns count, copies up to max slugs into out[i] (each
// cap TINY_AGENT_SLUG_MAX+1 bytes).
int tiny_agent_roster(char out[][TINY_AGENT_SLUG_MAX + 1], int max);
esp_err_t tiny_agent_roster_add(const char *slug);
// The roster is never empty: removing its last entry leaves the seed
// {"tiny"} — in RAM, on flash and after a reboot alike (one rule, applied
// before the write). Removing the sole seed itself is therefore ESP_OK with
// the roster unchanged (only a matching active slug is cleared).
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
// Whether `slug`, normalised as roster_add would, is in the copy `r`.
bool tiny_agent_roster_has(const tiny_agent_roster_t *r, const char *slug);

// Commit a previewed roster as ONE write. `base` is the snapshot `next` was
// built from; if the live state no longer equals it (another task switched,
// added or removed meanwhile), nothing is written and ESP_ERR_INVALID_STATE
// comes back, because the page built from `base` no longer describes the
// roster. NVS outcomes as above (ESP_OK / TINY_AGENT_UNSETTLED / nothing
// changed). The active slug is not changed (next->active must equal
// base->active). An empty `next` (n == 0) commits as the seed {"tiny"},
// the same non-empty rule as roster_remove.
esp_err_t tiny_agent_roster_commit(const tiny_agent_roster_t *base,
                                   const tiny_agent_roster_t *next);

#ifdef __cplusplus
}
#endif
