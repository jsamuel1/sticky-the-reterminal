// tiny_agent — active-universe-agent state (see tiny/tiny_agent.h).
//
// Storage shape (namespace "tinyagent"): ONE blob, "state" (StoredState v1:
// active slug + roster), so every mutation persists all-or-nothing — an NVS
// blob write replaces the previous entry only once the new one is complete,
// where the old one-key-per-field layout could be cut between keys. Firmware
// before the blob wrote "slug", "rcount" and "r0".."r7"; those are read once,
// when no "state" blob exists, and never written again (an OTA rollback to
// such firmware reads the roster as it was before the upgrade).
//
// RAM mirror + mutex, lazy load. Every mutation builds the NEXT state, writes
// it, and adopts it into RAM only once NVS has accepted it: a refused write
// leaves RAM, flash and the caller's answer agreeing on the OLD state, so
// the glass can never show a roster a reboot would lose.
//
// A failed NVS write is OUTCOME-UNKNOWN, not "nothing happened": IDF writes
// the new blob version before erasing the old one, so set_blob can fail
// (ESP_ERR_NVS_REMOVE_FAILED, or any error from the old-version erase) after
// the new bytes are already the newest on flash. So:
//   set_blob OK or REMOVE_FAILED, commit OK -> the new state is what a
//       reboot loads (IDF finishes the old-version removal at init): adopt.
//   anything else after open -> put the old state back (rewrite it, or erase
//       the key when there was none) and say the write failed.
//   and if THAT restore fails too -> read back what flash holds and adopt
//       it: the old state (refused, settled), the new one (ESP_OK — it IS
//       what a reboot loads), else TINY_AGENT_UNSETTLED: flash holds the
//       old or the new whole state (one blob: never a mix), RAM stays old,
//       and s_unsettled makes the next mutation write even when it changes
//       nothing, so the first healthy write settles flash back onto RAM.
//
// Validation lives HERE, once: the backend clamps slugs at 64 chars and
// slugifies server-side, but a slug with spaces/quotes would also corrupt
// the JSON body and the roster keys — refuse locally with a real error
// instead of shipping garbage and rendering the backend's 404.

#include <string.h>
#include "esp_attr.h"  // EXT_RAM_BSS_ATTR: statics off the internal rail
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "tiny/tiny_agent.h"

static const char *TAG = "tiny_agent";
static const char *NS = "tinyagent";
static const char *KEY = "state";

struct State {
    char slug[TINY_AGENT_SLUG_MAX + 1];
    char roster[TINY_AGENT_ROSTER_MAX][TINY_AGENT_SLUG_MAX + 1];
    int n;
};

// On-flash layout. Fixed size, zero-padded; `ver` gates the decode.
struct StoredState {
    uint8_t ver;
    uint8_t n;
    char slug[TINY_AGENT_SLUG_MAX + 1];
    char roster[TINY_AGENT_ROSTER_MAX][TINY_AGENT_SLUG_MAX + 1];
};
static constexpr uint8_t kStateVer = 1;

static SemaphoreHandle_t s_mux;
static bool s_loaded = false;
// Live state, the candidate a mutation builds, and the encode buffer: all
// written only with s_mux held.
EXT_RAM_BSS_ATTR static State s_state;
EXT_RAM_BSS_ATTR static State s_next;
EXT_RAM_BSS_ATTR static State s_read;   // a read-back's decode
EXT_RAM_BSS_ATTR static StoredState s_blob;
// Whether flash holds a "state" blob (else a failed first write must leave
// none behind, so the legacy keys / seed stay what a reboot loads).
static bool s_blob_present = false;
// A write AND its restore failed: flash may differ from RAM (old or new whole
// state). Cleared by the next write that lands, or restore that succeeds.
static bool s_unsettled = false;

static void lock(void) {
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mux, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mux); }

// Trim + lowercase + validate into out (cap TINY_AGENT_SLUG_MAX+1).
// Empty result is VALID (means: clear back to owner default).
static esp_err_t normalize(const char *in, char *out) {
    out[0] = 0;
    if (!in) return ESP_OK;
    while (*in == ' ' || *in == '\t') ++in;
    size_t len = strlen(in);
    while (len && (in[len - 1] == ' ' || in[len - 1] == '\t' ||
                   in[len - 1] == '\r' || in[len - 1] == '\n'))
        --len;
    if (len > TINY_AGENT_SLUG_MAX) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < len; ++i) {
        const char c = in[i];
        // The backend slugifies, but JSON-hostile bytes stop here.
        if (c == '"' || c == '\\' || c == ' ' || (unsigned char)c < 0x21)
            return ESP_ERR_INVALID_ARG;
        out[i] = (char)tolower((unsigned char)c);
    }
    out[len] = 0;
    return ESP_OK;
}

// A stored/pushed string is acceptable only if it is already exactly what
// normalize() would produce (bounded, so a non-terminated field is refused).
static bool canonical(const char *s, size_t cap, bool allow_empty) {
    if (!memchr(s, 0, cap)) return false;
    char norm[TINY_AGENT_SLUG_MAX + 1];
    if (normalize(s, norm) != ESP_OK) return false;
    if (!allow_empty && !norm[0]) return false;
    return strcmp(norm, s) == 0;
}

// THE non-empty rule, in one place: the universe card is never an empty
// page ("tiny" is the platform's own public tiny — known-good, free, always
// resolvable). Every state that exists — seeded, decoded, loaded from the
// legacy keys, or built by a mutation before it is written — passes through
// here, so what a mutation writes and adopts is exactly what a reboot loads.
static void reseed_if_empty(State *st) {
    if (st->n != 0) return;
    memset(st->roster[0], 0, sizeof st->roster[0]);
    strlcpy(st->roster[0], "tiny", sizeof st->roster[0]);
    st->n = 1;
}

static void seed(State *st) {
    memset(st, 0, sizeof *st);
    reseed_if_empty(st);
}

static bool same_state(const State &a, const State &b) {
    if (a.n != b.n || strcmp(a.slug, b.slug) != 0) return false;
    for (int i = 0; i < a.n; ++i)
        if (strcmp(a.roster[i], b.roster[i]) != 0) return false;
    return true;
}

static void encode(const State &st, StoredState *out) {
    memset(out, 0, sizeof *out);
    out->ver = kStateVer;
    out->n = (uint8_t)st.n;
    strlcpy(out->slug, st.slug, sizeof out->slug);
    for (int i = 0; i < st.n; ++i)
        strlcpy(out->roster[i], st.roster[i], sizeof out->roster[i]);
}

static bool decode(const StoredState &in, State *out) {
    if (in.ver != kStateVer || in.n > TINY_AGENT_ROSTER_MAX) return false;
    if (!canonical(in.slug, sizeof in.slug, true)) return false;
    for (int i = 0; i < in.n; ++i)
        if (!canonical(in.roster[i], sizeof in.roster[i], false)) return false;
    memset(out, 0, sizeof *out);
    strlcpy(out->slug, in.slug, sizeof out->slug);
    for (int i = 0; i < in.n; ++i)
        strlcpy(out->roster[i], in.roster[i], sizeof out->roster[i]);
    out->n = in.n;
    // This build never writes n=0 (commit_next_locked reseeds first); a
    // blob from elsewhere with n=0 loads as the seed, the same rule.
    reseed_if_empty(out);
    return true;
}

static void load_legacy(nvs_handle_t h, State *st) {
    size_t len = sizeof st->slug;
    if (nvs_get_str(h, "slug", st->slug, &len) != ESP_OK ||
        !canonical(st->slug, sizeof st->slug, true))
        st->slug[0] = 0;
    uint8_t rc = 0;
    nvs_get_u8(h, "rcount", &rc);
    if (rc > TINY_AGENT_ROSTER_MAX) rc = TINY_AGENT_ROSTER_MAX;
    st->n = 0;
    for (int i = 0; i < rc; ++i) {
        char k[8];
        snprintf(k, sizeof k, "r%u", (unsigned)i & 7u);  // bounded: ROSTER_MAX=8
        len = sizeof st->roster[st->n];
        if (nvs_get_str(h, k, st->roster[st->n], &len) == ESP_OK &&
            canonical(st->roster[st->n], sizeof st->roster[st->n], false))
            ++st->n;
    }
    reseed_if_empty(st);
}

static void load_locked(void) {
    if (s_loaded) return;
    s_loaded = true;
    seed(&s_state);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;  // first boot: seed
    size_t len = sizeof s_blob;
    const esp_err_t r = nvs_get_blob(h, KEY, &s_blob, &len);
    // Present = flash holds the key at all, even one this build cannot read:
    // a refused first save must then restore rather than erase it.
    if (r == ESP_OK || r == ESP_ERR_NVS_INVALID_LENGTH) s_blob_present = true;
    if (r == ESP_OK && len == sizeof s_blob && decode(s_blob, &s_state)) {
        // the blob is the whole truth
    } else {
        if (r == ESP_OK)
            ESP_LOGW(TAG, "stored state unreadable (len %u) - legacy keys",
                     (unsigned)len);
        seed(&s_state);
        load_legacy(h, &s_state);
    }
    nvs_close(h);
    if (s_state.slug[0])
        ESP_LOGI(TAG, "restored: talking to @%s (survives reboot by design)",
                 s_state.slug);
}

// set_blob outcomes after which the written bytes are the newest version on
// flash. REMOVE_FAILED: "the value was written however, and update will be
// finished after re-initialization of nvs" (IDF nvs.h) — written, not refused.
static bool blob_landed(esp_err_t r) {
    return r == ESP_OK || r == ESP_ERR_NVS_REMOVE_FAILED;
}

// Put flash back to RAM's state (s_state) after a write whose outcome is
// unknown. Rewriting identical bytes is free (IDF compares before writing);
// with no blob before, the key is erased so the legacy keys / seed load.
static esp_err_t restore_locked(nvs_handle_t h) {
    esp_err_t rb;
    if (s_blob_present) {
        encode(s_state, &s_blob);
        rb = nvs_set_blob(h, KEY, &s_blob, sizeof s_blob);
        if (blob_landed(rb)) rb = ESP_OK;
    } else {
        rb = nvs_erase_key(h, KEY);
        if (rb == ESP_ERR_NVS_NOT_FOUND) rb = ESP_OK;
    }
    if (rb == ESP_OK) rb = nvs_commit(h);
    return rb;
}


// After a failed write AND a failed restore: which whole state flash holds
// now, as IDF's own reader answers — i.e. what a reboot loads. New = `next` (checked first: when `next`
// equals RAM, both answers are the same truth). Old = what RAM says: the
// blob equals it, or there was no blob before and there still is none (so
// the legacy keys / seed load). Unknown = unreadable, or neither.
enum class Onflash { Old, New, Unknown };
static Onflash read_back_locked(nvs_handle_t h, const State &next,
                                bool *blob_now) {
    *blob_now = false;
    size_t len = sizeof s_blob;
    const esp_err_t g = nvs_get_blob(h, KEY, &s_blob, &len);
    if (g == ESP_ERR_NVS_NOT_FOUND)
        return s_blob_present ? Onflash::Unknown : Onflash::Old;
    State &st = s_read;
    if (g != ESP_OK || len != sizeof s_blob || !decode(s_blob, &st))
        return Onflash::Unknown;
    *blob_now = true;
    // `next` and s_state are both canonical (never an empty roster), and
    // decode applies the same rule, so plain equality is "what a reboot
    // loads" — no empty-equals-seed special case to drift from RAM.
    if (same_state(s_read, next)) return Onflash::New;
    if (same_state(s_read, s_state)) return Onflash::Old;
    return Onflash::Unknown;
}

// Write `next` as the one "state" blob. ESP_OK: `next` is on flash (the
// caller adopts it). Any other result: flash is back on s_state — or, when
// neither the restore nor a read-back could establish that,
// TINY_AGENT_UNSETTLED with s_unsettled set.
static esp_err_t persist_locked(const State &next) {
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;   // no handle: nothing was written
    encode(next, &s_blob);
    r = nvs_set_blob(h, KEY, &s_blob, sizeof s_blob);
    if (r == ESP_ERR_NVS_REMOVE_FAILED)
        ESP_LOGW(TAG, "old state not removed (%s) - new state written; NVS "
                 "finishes the update at init", esp_err_to_name(r));
    if (blob_landed(r)) r = nvs_commit(h);
    if (r == ESP_OK) {
        s_blob_present = true;
        s_unsettled = false;
        nvs_close(h);
        return ESP_OK;
    }
    // Outcome unknown (a failed set_blob may still have landed the new
    // bytes; a failed commit may or may not have made them durable).
    const esp_err_t rb = restore_locked(h);
    if (rb == ESP_OK) {
        nvs_close(h);
        s_unsettled = false;   // flash == RAM again
        return r;
    }
    // The restore failed too: whatever flash holds now is what a reboot
    // loads. Read it back; a state we can name is adopted as the truth.
    bool blob_now = false;
    const Onflash f = read_back_locked(h, next, &blob_now);
    nvs_close(h);
    if (f == Onflash::Old) {
        s_unsettled = false;
        s_blob_present = blob_now;
        ESP_LOGW(TAG, "save failed (%s), restore failed (%s); flash still "
                 "holds the old state", esp_err_to_name(r), esp_err_to_name(rb));
        return r;
    }
    if (f == Onflash::New) {
        s_blob_present = true;
        s_unsettled = false;
        ESP_LOGW(TAG, "save reported %s but flash holds the new state - "
                 "adopted (a reboot loads it)", esp_err_to_name(r));
        return ESP_OK;
    }
    s_unsettled = true;
    ESP_LOGE(TAG, "save failed (%s) and restore failed (%s) - flash holds "
             "the old or the new state until the next write",
             esp_err_to_name(r), esp_err_to_name(rb));
    return TINY_AGENT_UNSETTLED;
}

// Commit s_next: canonicalise, persist, then adopt. Canonicalising FIRST is
// what makes ESP_OK mean one state everywhere: removing the last roster
// entry writes, adopts and reboots as the seed {"tiny"} — never an empty
// roster in RAM that a reboot would silently reseed. Unchanged state (after
// canonicalising) writes nothing — unless flash is unsettled, when this
// write is what settles it.
static esp_err_t commit_next_locked(void) {
    reseed_if_empty(&s_next);
    if (!s_unsettled && same_state(s_next, s_state)) return ESP_OK;
    const esp_err_t r = persist_locked(s_next);
    if (r == ESP_OK) s_state = s_next;
    else ESP_LOGW(TAG, "not saved (%s) - state unchanged", tiny_agent_err_name(r));
    return r;
}

extern "C" const char *tiny_agent_err_name(esp_err_t err) {
    return err == TINY_AGENT_UNSETTLED ? "AGENT_UNSETTLED" : esp_err_to_name(err);
}

extern "C" const char *tiny_agent_current(void) {
    lock();
    load_locked();
    unlock();
    return s_state.slug;  // stable static storage; readers get a C string
}

static int roster_find(const char (*roster)[TINY_AGENT_SLUG_MAX + 1], int n,
                       const char *slug) {
    for (int i = 0; i < n; ++i)
        if (strcmp(roster[i], slug) == 0) return i;
    return -1;
}

// THE add rule, on any roster copy: a mutation's candidate and a caller's
// preview (tiny_agent_roster_preview_add) cannot drift apart.
static void roster_add_to(char (*roster)[TINY_AGENT_SLUG_MAX + 1], int *n,
                          const char *slug) {
    if (!slug[0] || roster_find(roster, *n, slug) >= 0) return;
    if (*n >= TINY_AGENT_ROSTER_MAX) {
        // Full: drop the OLDEST non-"tiny" entry (index 0 is usually the
        // seed; keep it reachable — switching home must never need typing).
        int drop = strcmp(roster[0], "tiny") == 0 && *n > 1 ? 1 : 0;
        memmove(&roster[drop], &roster[drop + 1],
                sizeof roster[0] * (*n - drop - 1));
        --*n;
    }
    strlcpy(roster[*n], slug, sizeof roster[0]);
    ++*n;
}

extern "C" esp_err_t tiny_agent_set(const char *slug) {
    char norm[TINY_AGENT_SLUG_MAX + 1];
    esp_err_t r = normalize(slug, norm);
    if (r != ESP_OK) return r;
    // "tiny <owner's own>" and "clear" both mean default; the backend also
    // treats the owner's own slug as default but "" keeps the ask body
    // byte-identical, which is the stronger guarantee.
    if (strcmp(norm, "clear") == 0) norm[0] = 0;
    lock();
    load_locked();
    s_next = s_state;
    strlcpy(s_next.slug, norm, sizeof s_next.slug);
    if (norm[0]) roster_add_to(s_next.roster, &s_next.n, norm);
    r = commit_next_locked();
    unlock();
    if (r == ESP_OK)
        ESP_LOGI(TAG, "active agent -> %s", norm[0] ? norm : "(own tiny)");
    return r;
}

extern "C" int tiny_agent_roster(char out[][TINY_AGENT_SLUG_MAX + 1], int max) {
    lock();
    load_locked();
    const int n = s_state.n < max ? s_state.n : max;
    for (int i = 0; i < n; ++i)
        strlcpy(out[i], s_state.roster[i], TINY_AGENT_SLUG_MAX + 1);
    unlock();
    return n;
}

extern "C" esp_err_t tiny_agent_roster_add(const char *slug) {
    char norm[TINY_AGENT_SLUG_MAX + 1];
    esp_err_t r = normalize(slug, norm);
    if (r != ESP_OK) return r;
    if (!norm[0]) return ESP_ERR_INVALID_ARG;
    lock();
    load_locked();
    s_next = s_state;
    roster_add_to(s_next.roster, &s_next.n, norm);
    r = commit_next_locked();
    unlock();
    return r;
}

extern "C" void tiny_agent_roster_snapshot(tiny_agent_roster_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    lock();
    load_locked();
    strlcpy(out->active, s_state.slug, sizeof out->active);
    for (int i = 0; i < s_state.n; ++i)
        strlcpy(out->slugs[i], s_state.roster[i], sizeof out->slugs[i]);
    out->n = s_state.n;
    unlock();
}

extern "C" esp_err_t tiny_agent_roster_preview_add(tiny_agent_roster_t *r,
                                                   const char *slug) {
    if (!r) return ESP_ERR_INVALID_ARG;
    char norm[TINY_AGENT_SLUG_MAX + 1];
    esp_err_t e = normalize(slug, norm);
    if (e != ESP_OK) return e;
    if (!norm[0]) return ESP_ERR_INVALID_ARG;
    roster_add_to(r->slugs, &r->n, norm);   // the copy only: no lock, no NVS
    return ESP_OK;
}

extern "C" bool tiny_agent_roster_has(const tiny_agent_roster_t *r,
                                      const char *slug) {
    if (!r) return false;
    char norm[TINY_AGENT_SLUG_MAX + 1];
    if (normalize(slug, norm) != ESP_OK || !norm[0]) return false;
    return roster_find(r->slugs, r->n, norm) >= 0;
}

static bool roster_matches_locked(const tiny_agent_roster_t *r) {
    if (r->n != s_state.n || strcmp(r->active, s_state.slug) != 0) return false;
    for (int i = 0; i < r->n; ++i)
        if (strcmp(r->slugs[i], s_state.roster[i]) != 0) return false;
    return true;
}

extern "C" esp_err_t tiny_agent_roster_commit(const tiny_agent_roster_t *base,
                                              const tiny_agent_roster_t *next) {
    if (!base || !next) return ESP_ERR_INVALID_ARG;
    if (next->n < 0 || next->n > TINY_AGENT_ROSTER_MAX) return ESP_ERR_INVALID_ARG;
    if (strcmp(next->active, base->active) != 0) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < next->n; ++i)
        if (!canonical(next->slugs[i], sizeof next->slugs[i], false))
            return ESP_ERR_INVALID_ARG;
    lock();
    load_locked();
    // The caller built (and showed) `next` from `base`: if anything changed
    // the live state since, its rows no longer describe the roster.
    if (!roster_matches_locked(base)) {
        unlock();
        ESP_LOGW(TAG, "roster changed since the page was built - refused");
        return ESP_ERR_INVALID_STATE;
    }
    s_next = s_state;
    for (int i = 0; i < next->n; ++i)
        strlcpy(s_next.roster[i], next->slugs[i], sizeof s_next.roster[i]);
    s_next.n = next->n;
    const esp_err_t r = commit_next_locked();
    unlock();
    return r;
}

extern "C" esp_err_t tiny_agent_roster_remove(const char *slug) {
    char norm[TINY_AGENT_SLUG_MAX + 1];
    esp_err_t r = normalize(slug, norm);
    if (r != ESP_OK) return r;
    if (!norm[0]) return ESP_ERR_INVALID_ARG;
    lock();
    load_locked();
    const int i = roster_find(s_state.roster, s_state.n, norm);
    if (i < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    s_next = s_state;
    memmove(&s_next.roster[i], &s_next.roster[i + 1],
            sizeof s_next.roster[0] * (s_next.n - i - 1));
    --s_next.n;
    memset(s_next.roster[s_next.n], 0, sizeof s_next.roster[0]);
    // Removing the ACTIVE agent clears back to default — a roster entry
    // that no longer exists must not keep answering the asks.
    if (strcmp(s_next.slug, norm) == 0) s_next.slug[0] = 0;
    r = commit_next_locked();
    unlock();
    return r;
}
