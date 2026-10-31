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
    // Invalid UTF-8 never lands in the composer: bad bytes collapse like
    // other separators, while surrounding valid words stay editable.
    CHECK(merge("", "ok \xC0\xAF now", 64, &v) == "ok now");
    CHECK(v == TINY_DICTATE_MERGE_OK);
    CHECK(merge("", "\x80\x80", 64, &v) == "");
    CHECK(v == TINY_DICTATE_MERGE_EMPTY);
    CHECK(merge("", "\xED\xA0\x80 x", 64, &v) == "x"); // UTF-8 surrogate
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
    // Review ask (PR #24): first rune multi-byte, exactly one byte of room
    // — the draft comes back unmodified, never with a lone lead byte.
    got = merge("abcd", "\xC3\xA9t\xC3\xA9", 6, &v);  // room for " " only
    CHECK(got == "abcd");
    CHECK(v == TINY_DICTATE_MERGE_CLIPPED);
    got = merge("", "\xC3\xA9", 2, &v);                  // empty field, 1 byte
    CHECK(got == "");
    CHECK(v == TINY_DICTATE_MERGE_CLIPPED);
    got = merge("abc", "\xE2\x82\xAC", 6, &v);           // " " + 1 of 3 bytes
    CHECK(got == "abc");
    CHECK(v == TINY_DICTATE_MERGE_CLIPPED);
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
    // Long transcript: cut to the buffer on a rune boundary and say clipped.
    CHECK(tiny_dictate_classify(200, "{\"text\":\"aaaaaaaaaaaaaa\xC3\xA9\xC3\xA9\"}", t, sizeof t) ==
          TINY_DICTATE_CLIPPED);
    CHECK(strlen(t) < sizeof t && valid_utf8(t));
    CHECK(tiny_dictate_classify(200, "{\"text\":\"ok \\uD800 now\"}", t, sizeof t) ==
          TINY_DICTATE_NOTHING);  // cJSON rejects unpaired surrogate
    // Invalid input bytes become separators; runs still collapse in the merge.
    CHECK(tiny_dictate_classify(200, "{\"text\":\"ok\xC0\xAFnow\"}", t, sizeof t) ==
          TINY_DICTATE_OK);
    CHECK(!strcmp(t, "ok  now"));
    CHECK(valid_utf8(t));
    char malformed_tail[64];
    snprintf(malformed_tail, sizeof malformed_tail,
             "{\"text\":\"ok%.*s\xC0\"}", 30, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(tiny_dictate_classify(200, malformed_tail, t, sizeof t) ==
          TINY_DICTATE_CLIPPED);  // valid unread runes remain before bad tail
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
    // Every verdict has a stable name; every failure/truncation has a note.
    for (int c = TINY_DICTATE_OK; c <= TINY_DICTATE_CLIPPED; ++c) {
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
    // Cross-task routing reads only the atomic committed-composer hint, then
    // re-checks lock-owned keyboard state before capture admission.
    const std::string shell_src = [] {
        FILE *f = fopen("../../firmware/main/tiny_shell.cpp", "rb");
        CHECK(f != nullptr);
        if (!f) return std::string{};
        std::string s;
        char chunk[1024];
        size_t n = 0;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) s.append(chunk, n);
        fclose(f);
        return s;
    }();
    CHECK(shell_src.find("static std::atomic<bool> s_dictating_composer_open") !=
          std::string::npos);
    CHECK(shell_src.find("return s_kb_active && s_kb_for") == std::string::npos);
    CHECK(shell_src.find("if (!s_kb_active || !kb_dictates()") != std::string::npos);

    // One atomic reservation spans queue publication through worker hand-back.
    // Direct relay calls compete for the same slot, so either producer wins;
    // never both. A producer that cannot reserve publishes nothing.
    const auto read_source = [](const char *path) {
        FILE *f = fopen(path, "rb");
        CHECK(f != nullptr);
        if (!f) return std::string{};
        std::string s;
        char chunk[1024];
        size_t n = 0;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) s.append(chunk, n);
        fclose(f);
        return s;
    };
    const std::string button_src = read_source("../../firmware/main/tiny_button.cpp");
    const std::string node_src = read_source("../../firmware/main/tiny_node.cpp");
    const size_t reserve = button_src.find("AskReservation ask_slot;");
    const size_t publish = button_src.find("xQueueSend(s_ask_q, &req, 0)");
    CHECK(reserve != std::string::npos && publish != std::string::npos &&
          reserve < publish);
    CHECK(button_src.find("tiny_node_dictate_reserved(") != std::string::npos);
    CHECK(button_src.find("tiny_node_do_ask_reserved(") != std::string::npos);
    CHECK(button_src.find("tiny_node_ask_release();") != std::string::npos);
    CHECK(node_src.find("reserved ? tiny_ask_reservation::held()") !=
          std::string::npos);
    CHECK(node_src.find("return tiny_node_do_ask_impl(text, voice_secs, out, out_cap, false);") !=
          std::string::npos);
    CHECK(node_src.find("return tiny_node_do_ask_impl(text, voice_secs, out, out_cap, true);") !=
          std::string::npos);
    CHECK(node_src.find("s_ask_mux") == std::string::npos);

    if (g_fail) {
        fprintf(stderr, "dictate host test: %d FAILED\n", g_fail);
        return 1;
    }
    printf("dictate host test: ALL GREEN\n");
    return 0;
}
