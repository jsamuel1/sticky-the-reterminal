// agent roster host test — the SHIPPED firmware/main/tiny_agent.cpp against
// the file-backed NVS shim (../shim/nvs.h).
//
// A universe push paints the page from tiny_agent_roster_preview_add() and
// persists the slugs only once that page is on glass (tiny_display). That is
// honest only if the preview is EXACTLY what the real add would produce and
// changes nothing on its own. This checks both, including normalisation,
// refusal, dedupe and the full-roster eviction rule.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tiny/tiny_agent.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) { ++g_fail; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } \
        else printf("ok   " __VA_ARGS__), putchar('\n');                    \
    } while (0)

static std::vector<std::string> live() {
    tiny_agent_roster_t r;
    tiny_agent_roster_snapshot(&r);
    return std::vector<std::string>(r.slugs, r.slugs + r.n);
}
static std::vector<std::string> of(const tiny_agent_roster_t &r) {
    return std::vector<std::string>(r.slugs, r.slugs + r.n);
}
static size_t nvs_size() { return nvs_shim::store().size(); }
static std::string nvs_dump() {
    std::string s;
    for (auto &kv : nvs_shim::store()) s += kv.first + "=" + kv.second + ";";
    return s;
}

int main() {
    CHECK(live() == std::vector<std::string>{"tiny"}, "fresh roster is the seed");
    // Each input: preview on a snapshot, then the real add; both must agree,
    // and the preview alone must not touch RAM state or NVS.
    const char *inputs[] = {"bonk", "  Clod \n", "bonk", "a\"b", "", "x y",
                            "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8"};
    for (const char *in : inputs) {
        tiny_agent_roster_t p;
        tiny_agent_roster_snapshot(&p);
        const auto before = live();
        const std::string nvs_before = nvs_dump();
        const esp_err_t pe = tiny_agent_roster_preview_add(&p, in);
        CHECK(live() == before && nvs_dump() == nvs_before,
              "preview \"%s\": live roster and NVS untouched", in);
        const esp_err_t ae = tiny_agent_roster_add(in);
        CHECK(pe == ae, "preview \"%s\": same verdict as add (%d vs %d)", in, pe, ae);
        CHECK(of(p) == live(), "preview \"%s\": same roster as add (n=%d)", in, p.n);
    }
    CHECK(live().size() == TINY_AGENT_ROSTER_MAX && live()[0] == "tiny",
          "full roster keeps the seed at row 0");
    CHECK(tiny_agent_roster_preview_add(nullptr, "z") == ESP_ERR_INVALID_ARG &&
              nvs_size() > 0,
          "preview with no roster refuses");
    if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    return 0;
}
