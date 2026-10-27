// dictate host test — the SHIPPED firmware/main/tiny/tiny_dictate_util.h
// against the device's own cJSON. No board.
#include "tiny/tiny_dictate_util.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

static std::string merge(const char *have, const char *spoken, size_t cap,
                         tiny_dictate_merge_t *verdict) {
    char buf[256];
    CHECK(cap <= sizeof buf);
    snprintf(buf, sizeof buf, "%s", have);
    *verdict = tiny_dictate_merge(buf, cap, spoken);
    CHECK(strlen(buf) < cap);  // never past the composer buffer
    return buf;
}

static bool valid_utf8(const std::string &s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
        if (!n || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k)
            if ((((unsigned char)s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

static void test_merge() {
    tiny_dictate_merge_t v;
    // Empty field: words land as spoken, trimmed.
    CHECK(merge("", "  on my way  ", 64, &v) == "on my way");
    CHECK(v == TINY_DICTATE_MERGE_OK);
    // Existing draft: one joining space, never two.
    CHECK(merge("ok", "see you at six", 64, &v) == "ok see you at six");
    CHECK(merge("ok ", "see you", 64, &v) == "ok see you");
    // Control bytes and newlines from the STT model are spaces, runs collapse.
    CHECK(merge("", "line one\n\nline\ttwo\r", 64, &v) == "line one line two");
    // Nothing usable: the draft is untouched.
    CHECK(merge("draft", " \n\t ", 64, &v) == "draft");
    CHECK(v == TINY_DICTATE_MERGE_EMPTY);
    CHECK(merge("draft", "", 64, &v) == "draft");
    CHECK(v == TINY_DICTATE_MERGE_EMPTY);
    // Clipping: the head that fits, verdict says so, no trailing join space.
    std::string got = merge("", "abcdefghij", 6, &v);
    CHECK(got == "abcde");
    CHECK(v == TINY_DICTATE_MERGE_CLIPPED);
    got = merge("abcd", "xyz", 6, &v);  // room for " " only
    CHECK(got == "abcd");
    CHECK(v == TINY_DICTATE_MERGE_CLIPPED);
    // A full buffer: nothing fits, draft untouched, still CLIPPED (not EMPTY).
    got = merge("abcde", "x", 6, &v);
    CHECK(got == "abcde");
    CHECK(v == TINY_DICTATE_MERGE_CLIPPED);
    // UTF-8: a cut never leaves half a rune.
    for (size_t cap = 2; cap < 24; ++cap) {
        got = merge("", "caf\xC3\xA9 \xC3\xA7ok g\xC3\xBCzel \xE2\x82\xAC", cap, &v);
        CHECK(valid_utf8(got));
    }
    CHECK(merge("", "caf\xC3\xA9", 5, &v) == "caf");  // é needs 2 B, 1 left
    CHECK(merge("", "caf\xC3\xA9", 6, &v) == "caf\xC3\xA9");
}

static void test_pop() {
    char s[32];
    strcpy(s, "caf\xC3\xA9");
    tiny_dictate_utf8_pop(s);
    CHECK(!strcmp(s, "caf"));
    strcpy(s, "\xE2\x82\xAC");
    tiny_dictate_utf8_pop(s);
    CHECK(!strcmp(s, ""));
    tiny_dictate_utf8_pop(s);  // empty: no-op, no underflow
    CHECK(!strcmp(s, ""));
    strcpy(s, "ab");
    tiny_dictate_utf8_pop(s);
    CHECK(!strcmp(s, "a"));
}

static void test_classify() {
    char t[16];
    CHECK(tiny_dictate_classify(200, R"({"ok":true,"text":"see you at six"})", t, sizeof t) ==
          TINY_DICTATE_OK);
    CHECK(!strcmp(t, "see you at six"));
    // Long transcript: cut to the buffer on a rune boundary.
    CHECK(tiny_dictate_classify(200, "{\"text\":\"aaaaaaaaaaaaaa\xC3\xA9\xC3\xA9\"}", t, sizeof t) ==
          TINY_DICTATE_OK);
    CHECK(strlen(t) < sizeof t && valid_utf8(t));
    CHECK(tiny_dictate_classify(200, R"({"ok":true,"text":"   "})", t, sizeof t) == TINY_DICTATE_NOTHING);
    CHECK(t[0] == 0);
    CHECK(tiny_dictate_classify(200, R"({"ok":true})", t, sizeof t) == TINY_DICTATE_NOTHING);
    CHECK(tiny_dictate_classify(422, R"({"ok":false,"error":"nothing transcribed"})", t, sizeof t) ==
          TINY_DICTATE_NOTHING);
    CHECK(tiny_dictate_classify(401, R"({"error":"unknown device"})", t, sizeof t) == TINY_DICTATE_AUTH);
    CHECK(tiny_dictate_classify(400, R"({"error":"audioUrl too long"})", t, sizeof t) == TINY_DICTATE_REFUSED);
    // 404 WITH the route's sentence = the route ran and refused this audio.
    CHECK(tiny_dictate_classify(404, R"({"ok":false,"error":"audio not found"})", t, sizeof t) ==
          TINY_DICTATE_REFUSED);
    // 404 WITHOUT it = an older app that does not serve the route.
    CHECK(tiny_dictate_classify(404, "<!DOCTYPE html><title>404</title>", t, sizeof t) ==
          TINY_DICTATE_ROUTE_MISSING);
    CHECK(tiny_dictate_classify(404, "", t, sizeof t) == TINY_DICTATE_ROUTE_MISSING);
    CHECK(tiny_dictate_classify(-1, NULL, t, sizeof t) == TINY_DICTATE_TRANSPORT);
    CHECK(tiny_dictate_classify(0, "", t, sizeof t) == TINY_DICTATE_TRANSPORT);
    CHECK(tiny_dictate_classify(424, R"({"error":"registry unreachable"})", t, sizeof t) == TINY_DICTATE_SERVER);
    CHECK(tiny_dictate_classify(500, "oops", t, sizeof t) == TINY_DICTATE_SERVER);
    // Every verdict has a stable name and every failure a note.
    for (int c = TINY_DICTATE_OK; c <= TINY_DICTATE_UNAVAILABLE; ++c) {
        const auto k = (tiny_dictate_class_t)c;
        CHECK(strcmp(tiny_dictate_class_name(k), "?") != 0);
        CHECK((k == TINY_DICTATE_OK) == (tiny_dictate_note(k)[0] == 0));
        CHECK(strlen(tiny_dictate_note(k)) <= TINY_DICTATE_TITLE_MAX);  // uncut on 320 px
    }
    CHECK(strlen(TINY_DICTATE_TITLE_LISTENING) <= TINY_DICTATE_TITLE_MAX);
    CHECK(strlen(TINY_DICTATE_TITLE_CHECK) <= TINY_DICTATE_TITLE_MAX);
    CHECK(strlen(TINY_DICTATE_TITLE_CLIPPED) <= TINY_DICTATE_TITLE_MAX);
}

static void test_feature() {
    CHECK(tiny_dictate_feature(R"({"ok":true,"features":["transcribe"]})", "transcribe") == 1);
    CHECK(tiny_dictate_feature(R"({"ok":true,"unread":3,"features":["x","transcribe"]})", "transcribe") == 1);
    CHECK(tiny_dictate_feature(R"({"ok":true,"features":[]})", "transcribe") == 0);
    CHECK(tiny_dictate_feature(R"({"ok":true,"features":[{"transcribe":1}]})", "transcribe") == 0);
    // Older backend / not asked: "unknown", so the device keeps what it knew.
    CHECK(tiny_dictate_feature(R"({"ok":true,"unread":2})", "transcribe") == -1);
    CHECK(tiny_dictate_feature(R"({"ok":true,"features":"transcribe"})", "transcribe") == -1);
    CHECK(tiny_dictate_feature("", "transcribe") == -1);
    CHECK(tiny_dictate_feature(NULL, "transcribe") == -1);
}

int main() {
    test_merge();
    test_pop();
    test_classify();
    test_feature();
    if (g_fail) {
        fprintf(stderr, "dictate host test: %d FAILED\n", g_fail);
        return 1;
    }
    printf("dictate host test: ALL GREEN\n");
    return 0;
}
