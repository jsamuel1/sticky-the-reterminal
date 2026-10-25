// tiny_envelope — see tiny/tiny_envelope.h.
#include "tiny/tiny_envelope.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

bool contains_ci(const char *hay, const char *needle) {
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            ++i;
        if (i == n) return true;
    }
    return false;
}

// Replace every value under a credential-looking key, at any depth.
void redact(cJSON *node) {
    for (cJSON *c = node ? node->child : nullptr; c; c = c->next) {
        if (c->string && tiny_envelope_key_is_secret(c->string)) {
            cJSON *r = cJSON_CreateString("<redacted>");
            if (r) cJSON_ReplaceItemViaPointer(node, c, r);
            c = r ? r : c;
            continue;
        }
        if (cJSON_IsObject(c) || cJSON_IsArray(c)) redact(c);
    }
}

// Append printf-style text, never past cap.
void put(char *out, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void put(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n > 0) *len += ((size_t)n < cap - *len) ? (size_t)n : cap - *len - 1;
}

// Printable preview: control bytes become '.', the cut is marked.
void put_preview(char *out, size_t cap, size_t *len, const char *s) {
    const size_t n = strlen(s);
    const size_t take = n < TINY_ENVELOPE_PREVIEW ? n : TINY_ENVELOPE_PREVIEW;
    put(out, cap, len, " preview=\"");
    for (size_t i = 0; i < take && *len + 1 < cap; ++i) {
        const unsigned char ch = (unsigned char)s[i];
        out[(*len)++] = (ch < 0x20 || ch == 0x7f) ? '.' : (char)ch;
    }
    out[*len < cap ? *len : cap - 1] = 0;
    put(out, cap, len, "\"%s", n > take ? "…" : "");
}

void describe_object(const cJSON *obj, char *out, size_t cap, size_t *len) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(obj, "type");
    if (cJSON_IsString(t))
        put(out, cap, len, " type=\"%.24s\"", t->valuestring);
    else
        put(out, cap, len, " type=%s", t ? "non-string" : "absent");
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(obj, "prompt");
    put(out, cap, len, " prompt=%s",
        !pr ? "absent" : cJSON_IsString(pr) ? "string" : "non-string");
    put(out, cap, len, " keys=[");
    int k = 0;
    for (const cJSON *c = obj->child; c; c = c->next) {
        if (k == 8) { put(out, cap, len, ",…"); break; }
        put(out, cap, len, "%s%.20s", k++ ? "," : "", c->string ? c->string : "?");
    }
    put(out, cap, len, "]");
    cJSON *copy = cJSON_Duplicate(obj, 1);
    if (!copy) return;
    redact(copy);
    char *js = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    if (js) {
        put_preview(out, cap, len, js);
        free(js);
    }
}

}  // namespace

extern "C" int tiny_envelope_key_is_secret(const char *key) {
    // Long words are unambiguous: a substring match anywhere (accesstoken,
    // client_secret, x-cookie) is a credential.
    static const char *const kAnywhere[] = {"token", "secret", "password", "passwd",
                                            "bearer", "cookie", "credential",
                                            "authorization", "apikey"};
    // Short words collide with ordinary names (monkey, keyboard, author,
    // sessions): they count only as a whole word of the name, split on
    // _ - . / space and at a lower->upper camelCase step.
    static const char *const kWord[] = {"key", "auth", "session", "sig", "pin"};
    if (!key) return 0;
    for (const char *w : kAnywhere)
        if (contains_ci(key, w)) return 1;
    char word[32];
    size_t n = 0;
    for (const char *p = key;; ++p) {
        const unsigned char ch = (unsigned char)*p;
        const bool sep = !ch || ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == ' ';
        const bool camel = ch && n > 0 && isupper(ch) && islower((unsigned char)p[-1]);
        if (sep || camel) {
            word[n] = 0;
            for (const char *w : kWord)
                if (n && !strcmp(word, w)) return 1;
            n = 0;
            if (!ch) break;
            if (sep) continue;
        }
        if (n + 1 < sizeof word) word[n++] = (char)tolower(ch);
    }
    return 0;
}

extern "C" char *tiny_envelope_describe(const cJSON *payload, char *out, size_t cap) {
    if (!out || cap == 0) return out;
    out[0] = 0;
    size_t len = 0;
    if (!payload) {
        put(out, cap, &len, "payload=absent");
        return out;
    }
    if (cJSON_IsString(payload)) {
        const char *s = payload->valuestring ? payload->valuestring : "";
        put(out, cap, &len, "payload=string %u B", (unsigned)strlen(s));
        cJSON *inner = cJSON_Parse(s);
        if (cJSON_IsObject(inner)) {
            describe_object(inner, out, cap, &len);
        } else {
            // Not a JSON object: nothing to key-redact, so show only its shape.
            // A raw string can itself be a credential; its bytes stay off the log.
            put(out, cap, &len, " json=%s",
                !inner ? "invalid" : cJSON_IsArray(inner) ? "array" : "scalar");
        }
        cJSON_Delete(inner);
        return out;
    }
    if (cJSON_IsObject(payload)) {
        put(out, cap, &len, "payload=object");
        describe_object(payload, out, cap, &len);
        return out;
    }
    put(out, cap, &len, "payload=%s",
        cJSON_IsNull(payload) ? "null" : cJSON_IsArray(payload) ? "array"
        : cJSON_IsNumber(payload) ? "number" : cJSON_IsBool(payload) ? "bool" : "other");
    return out;
}
