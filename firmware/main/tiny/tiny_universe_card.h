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
//   repaint (row tap / u:open)            -> no priority field: tiny_display
//          keeps the alert only while the universe page on glass committed
//          with it (decided under the display lock, at commit)
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
static inline char *tiny_universe_card_json(
        const char *cur, const char (*roster)[TINY_AGENT_SLUG_MAX + 1], int n,
        const char *priority) {
    if (!cur) cur = "";
    cJSON *card = cJSON_CreateObject();
    if (!card) return NULL;
    cJSON_AddStringToObject(card, "type", "composite");
    cJSON_AddStringToObject(card, "card_id", "universe");
    if (priority) cJSON_AddStringToObject(card, "priority", priority);
    // The title strip carries WHO IS ANSWERING — page identity + the owner
    // rule ("never mistake a persona's words for your own tiny's") in one.
    char title[80];
    snprintf(title, sizeof title, "universe%s%s",
             cur[0] ? " - @" : "", cur[0] ? cur : "");
    cJSON_AddStringToObject(card, "title", title);
    cJSON *parts = cJSON_AddArrayToObject(card, "parts");
    cJSON *head = cJSON_CreateObject();
    cJSON_AddStringToObject(head, "type", "text");
    cJSON_AddStringToObject(head, "body",
        "Who answers your asks. Tap a row to switch - it sticks until you "
        "switch back.");
    cJSON_AddItemToArray(parts, head);

    cJSON *mn = cJSON_CreateObject();
    cJSON_AddStringToObject(mn, "type", "menu");
    cJSON *items = cJSON_AddArrayToObject(mn, "items");
    {   // row 0, always: the way home. Never needs typing (tiny_agent law:
        // the roster may drop entries, this row may not).
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "label", "tiny (yours)");
        cJSON_AddStringToObject(it, "note",
                                cur[0] ? "tap to switch back" : "ACTIVE");
        cJSON_AddStringToObject(it, "id", "u:clear");
        cJSON_AddItemToArray(items, it);
    }
    for (int i = 0; i < n; ++i) {
        cJSON *it = cJSON_CreateObject();
        char lab[TINY_AGENT_SLUG_MAX + 2], id[16];
        snprintf(lab, sizeof lab, "@%.*s", TINY_AGENT_SLUG_MAX, roster[i]);
        snprintf(id, sizeof id, "u:i:%d", i);
        const bool active = cur[0] && strcmp(cur, roster[i]) == 0;
        cJSON_AddStringToObject(it, "label", lab);
        cJSON_AddStringToObject(it, "note",
                                active ? "ACTIVE" : "public tiny");
        cJSON_AddStringToObject(it, "id", id);
        cJSON_AddItemToArray(items, it);
    }
    {   // plain home row — universe is reachable from settings AND relay,
        // so the way out must live on the card itself (D-UX: no dead ends).
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "label", "home");
        cJSON_AddStringToObject(it, "note", "leave this page");
        cJSON_AddStringToObject(it, "id", "home");
        cJSON_AddItemToArray(items, it);
    }
    cJSON_AddItemToArray(parts, mn);

    char *cs = cJSON_PrintUnformatted(card);
    cJSON_Delete(card);
    return cs;
}
