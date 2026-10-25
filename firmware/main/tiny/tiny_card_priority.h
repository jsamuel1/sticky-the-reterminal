// tiny_card_priority — the card grammar's "priority" field (T1). Pure helper,
// shared by the renderer, the shell's universe page and the host tests.
#pragma once
#include <string.h>
#include "cJSON.h"

// 1 = "alert", 0 = present but not "alert" (an explicit "normal"), -1 = absent.
// Absent matters only to a device-composed repaint (universe row tap), which
// keeps the alert of the same page on glass; a sender's card treats absent as
// normal.
static inline int tiny_card_priority(const cJSON *root) {
    const cJSON *p = root ? cJSON_GetObjectItem(root, "priority") : NULL;
    if (!p) return -1;
    return (cJSON_IsString(p) && strcmp(p->valuestring, "alert") == 0) ? 1 : 0;
}
