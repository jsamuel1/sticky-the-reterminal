// tiny_universe_card — the universe page's card spec (grammar v11), built from
// the roster. Header-only and free of device state so the render host test
// builds exactly the composite the shell renders.
//
// Priority rides IN the composite, never beside it: renders run on several
// tasks (relay, touch, voice, timer, console), so a per-push flag stored
// outside the spec would let one push lend its priority to another.
//   push  (render_ui {"type":"universe",…}) -> the pushed spec's priority,
//          always written ("alert" or "normal"), so a normal push replaces an
//          alert page as normal and a retried/rerendered alert push stays alert
//   repaint (row tap / u:open)            -> no priority field, rendered via
//          tiny_display_render_universe_page: the TYPED universe page keeps
//          the alert only while the universe page on glass committed with it
//          (decided under the display lock, at commit). The same JSON sent
//          through tiny_display_render_card is a sender's card and never
//          inherits.
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "tiny/tiny_card_priority.h"
#include "tiny/tiny_agent.h"

// A universe PUSH always states its priority on the composite: absent on
// the pushed spec means the default, "normal".
static inline const char *tiny_universe_push_priority(const cJSON *spec) {
    return tiny_card_priority(spec) == 1 ? "alert" : "normal";
}

// The page. `priority`: "alert" / "normal" for a push, NULL for a repaint.
// Returns a malloc'd JSON string (cJSON_free/free) or NULL on no memory.
// All or nothing: cJSON's Add*/Create* helpers return NULL on allocation
// failure and leave the field silently absent, so every one is checked. A
// half-built page must never ship — an absent "priority" reads as a repaint
// and would inherit the alert of the page on glass, a lost row would point
// a row index at the wrong slug.
static inline char *tiny_universe_card_json(
        const char *cur, const char (*roster)[TINY_AGENT_SLUG_MAX + 1], int n,
        const char *priority) {
    if (!cur) cur = "";
    cJSON *card = cJSON_CreateObject();
    if (!card) return NULL;
    bool ok = cJSON_AddStringToObject(card, "type", "composite") &&
              cJSON_AddStringToObject(card, "card_id", "universe");
    if (ok && priority)
        ok = cJSON_AddStringToObject(card, "priority", priority) != NULL;
    // The title strip carries WHO IS ANSWERING — page identity + the owner
    // rule ("never mistake a persona's words for your own tiny's") in one.
    char title[80];   // "universe - @" + a slug (<= TINY_AGENT_SLUG_MAX)
    snprintf(title, sizeof title, "universe%s%.*s",
             cur[0] ? " - @" : "", TINY_AGENT_SLUG_MAX, cur[0] ? cur : "");
    if (ok) ok = cJSON_AddStringToObject(card, "title", title) != NULL;
    cJSON *parts = ok ? cJSON_AddArrayToObject(card, "parts") : NULL;
    cJSON *head = parts ? cJSON_CreateObject() : NULL;
    ok = head && cJSON_AddItemToArray(parts, head);
    if (!ok) cJSON_Delete(head);
    ok = ok && cJSON_AddStringToObject(head, "type", "text") &&
         cJSON_AddStringToObject(head, "body",
             "Who answers your asks. Tap a row to switch - it sticks until you "
             "switch back.");

    cJSON *mn = ok ? cJSON_CreateObject() : NULL;
    ok = mn && cJSON_AddItemToArray(parts, mn);
    if (!ok) cJSON_Delete(mn);
    ok = ok && cJSON_AddStringToObject(mn, "type", "menu");
    cJSON *items = ok ? cJSON_AddArrayToObject(mn, "items") : NULL;
    ok = items != NULL;
    auto row = [&](const char *label, const char *note, const char *id) {
        if (!ok) return;
        cJSON *it = cJSON_CreateObject();
        ok = it && cJSON_AddItemToArray(items, it);
        if (!ok) { cJSON_Delete(it); return; }
        ok = cJSON_AddStringToObject(it, "label", label) &&
             cJSON_AddStringToObject(it, "note", note) &&
             cJSON_AddStringToObject(it, "id", id);
    };
    // row 0, always: the way home. Never needs typing (tiny_agent law:
    // the roster may drop entries, this row may not).
    row("tiny (yours)", cur[0] ? "tap to switch back" : "ACTIVE", "u:clear");
    for (int i = 0; i < n; ++i) {
        char lab[TINY_AGENT_SLUG_MAX + 2], id[16];
        snprintf(lab, sizeof lab, "@%.*s", TINY_AGENT_SLUG_MAX, roster[i]);
        snprintf(id, sizeof id, "u:i:%d", i);
        const bool active = cur[0] && strcmp(cur, roster[i]) == 0;
        row(lab, active ? "ACTIVE" : "public tiny", id);
    }
    // plain home row — universe is reachable from settings AND relay,
    // so the way out must live on the card itself (D-UX: no dead ends).
    row("home", "leave this page", "home");

    char *cs = ok ? cJSON_PrintUnformatted(card) : NULL;
    cJSON_Delete(card);
    return cs;
}
