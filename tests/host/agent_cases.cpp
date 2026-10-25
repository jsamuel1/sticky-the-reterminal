// agent roster host test — the SHIPPED firmware/main/tiny_agent.cpp against
// the file-backed NVS shim (../shim/nvs.h) with save-path fault injection
// (agent_shim/agent_shim.h). Several PROCESS RUNS share one store file: each
// restart is a reboot, because statics die with the process like they die
// with the chip.
//
//   fresh   preview == add (normalisation, refusal, dedupe, eviction) and the
//           preview writes nothing; then every save-path NVS failure, for
//           every mutation (set / add / remove / batch commit), leaves RAM
//           and flash at the old state; batch commit is one write and a
//           compare-and-set against the snapshot it was built from
//   reboot  what the fresh run's last answer said persisted, did — and no
//           refused write came back
//   legacy  a store written by pre-blob firmware ("slug","rcount","rN") loads
//           as before; a refused first save leaves it loadable
//   corrupt an unreadable "state" blob falls back to the legacy keys, and a
//           refused save changes nothing a reboot loads
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

using Roster = std::vector<std::string>;
static Roster live() {
    tiny_agent_roster_t r;
    tiny_agent_roster_snapshot(&r);
    return Roster(r.slugs, r.slugs + r.n);
}
static Roster of(const tiny_agent_roster_t &r) { return Roster(r.slugs, r.slugs + r.n); }
static std::string nvs_dump() {
    std::string s;
    for (auto &kv : nvs_shim::store()) s += kv.first + "=" + kv.second + ";";
    return s;
}
static std::string state() { return std::string(tiny_agent_current()) + "|" + nvs_dump(); }

// Every way a save can be refused. A failed commit after a successful set is
// the case that needs the old bytes put back.
struct Fault { agent_shim::Op op; const char *name; };
static const Fault kFaults[] = {
    {agent_shim::kOpenRw, "open"},
    {agent_shim::kSetBlob, "set_blob"},
    {agent_shim::kCommit, "commit"},
};

// Run `mutate` under each fault: it must fail, and RAM (slug + roster) and
// flash must be exactly as before. Then once more healthy, returning its
// verdict.
template <class F>
static void refused_all(const char *what, F mutate) {
    for (const Fault &f : kFaults) {
        const Roster r0 = live();
        const std::string s0 = state();
        agent_shim::reset();
        agent_shim::fail()[f.op] = 1;
        const esp_err_t e = mutate();
        agent_shim::reset();
        CHECK(e != ESP_OK, "%s / %s refused: returned %d", what, f.name, e);
        CHECK(live() == r0 && state() == s0,
              "%s / %s refused: RAM and NVS unchanged", what, f.name);
    }
}
template <class F>
static esp_err_t refused_then_ok(const char *what, F mutate) {
    refused_all(what, mutate);
    return mutate();
}

static int fresh() {
    CHECK(live() == Roster{"tiny"}, "fresh roster is the seed");
    // Preview on a snapshot, then the real add; both must agree, and the
    // preview alone must not touch RAM state or NVS.
    const char *inputs[] = {"bonk", "  Clod \n", "bonk", "a\"b", "", "x y",
                            "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8"};
    for (const char *in : inputs) {
        tiny_agent_roster_t p;
        tiny_agent_roster_snapshot(&p);
        const Roster before = live();
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
    CHECK(tiny_agent_roster_preview_add(nullptr, "z") == ESP_ERR_INVALID_ARG,
          "preview with no roster refuses");

    // ---- every mutation is all-or-nothing under NVS failure -------------
    CHECK(refused_then_ok("rm s2", [] { return tiny_agent_roster_remove("s2"); }) == ESP_OK &&
              live().size() == TINY_AGENT_ROSTER_MAX - 1,
          "rm s2 healthy");
    CHECK(refused_then_ok("add new1", [] { return tiny_agent_roster_add("new1"); }) == ESP_OK &&
              live().back() == "new1",
          "add new1 healthy");
    // set: the active slug AND its roster entry move together, or neither.
    CHECK(refused_then_ok("set fresh-one", [] { return tiny_agent_set("fresh-one"); }) == ESP_OK &&
              !strcmp(tiny_agent_current(), "fresh-one") && live().back() == "fresh-one",
          "set fresh-one healthy (slug + roster)");
    // remove of the ACTIVE agent clears it: both, or neither.
    CHECK(refused_then_ok("rm active", [] { return tiny_agent_roster_remove("fresh-one"); }) == ESP_OK &&
              !tiny_agent_current()[0],
          "rm active healthy clears the active slug");
    CHECK(refused_then_ok("set s5", [] { return tiny_agent_set("s5"); }) == ESP_OK &&
              !strcmp(tiny_agent_current(), "s5"),
          "set s5 healthy");
    CHECK(refused_then_ok("clear", [] { return tiny_agent_set("clear"); }) == ESP_OK &&
              !tiny_agent_current()[0],
          "clear healthy");

    // ---- batch commit: the universe push's one write ---------------------
    {
        // A multi-slug merge that EVICTS (roster is full): refused as a whole
        // under every fault, then one write for all three.
        tiny_agent_roster_t base, next;
        tiny_agent_roster_snapshot(&base);
        next = base;
        for (const char *s : {"m1", "m2", "m3"}) tiny_agent_roster_preview_add(&next, s);
        CHECK(of(next) != of(base), "batch: preview differs from base");
        refused_all("batch m1..m3", [&] { return tiny_agent_roster_commit(&base, &next); });
        const int sets0 = agent_shim::calls()[agent_shim::kSetBlob];
        const int commits0 = agent_shim::calls()[agent_shim::kCommit];
        const esp_err_t e = tiny_agent_roster_commit(&base, &next);
        CHECK(e == ESP_OK && live() == of(next), "batch m1..m3: all three, healthy");
        CHECK(agent_shim::calls()[agent_shim::kSetBlob] - sets0 == 1 &&
                  agent_shim::calls()[agent_shim::kCommit] - commits0 == 1,
              "batch: three slugs, one blob write, one commit");
        // The SAME base again: the roster moved on, so this is stale.
        const std::string s0 = state();
        CHECK(tiny_agent_roster_commit(&base, &next) == ESP_ERR_INVALID_STATE &&
                  state() == s0,
              "batch: stale base refused, nothing written");
        // Another task switched agent after the snapshot: stale too.
        tiny_agent_roster_t b2, n2;
        tiny_agent_roster_snapshot(&b2);
        n2 = b2;
        tiny_agent_roster_preview_add(&n2, "m4");
        CHECK(tiny_agent_set("m1") == ESP_OK, "switch after snapshot");
        const std::string s1 = state();
        CHECK(tiny_agent_roster_commit(&b2, &n2) == ESP_ERR_INVALID_STATE &&
                  state() == s1,
              "batch: base with another active slug refused");
        // A commit may not change the active slug, nor carry junk.
        tiny_agent_roster_snapshot(&b2);
        n2 = b2;
        strlcpy(n2.active, "", sizeof n2.active);
        CHECK(tiny_agent_roster_commit(&b2, &n2) == ESP_ERR_INVALID_ARG && state() == s1,
              "batch: active slug change refused");
        n2 = b2;
        strlcpy(n2.slugs[0], "Bad Slug", sizeof n2.slugs[0]);
        CHECK(tiny_agent_roster_commit(&b2, &n2) == ESP_ERR_INVALID_ARG && state() == s1,
              "batch: non-canonical slug refused");
        // Unchanged roster: success, no write.
        const int sets1 = agent_shim::calls()[agent_shim::kSetBlob];
        CHECK(tiny_agent_roster_commit(&b2, &b2) == ESP_OK &&
                  agent_shim::calls()[agent_shim::kSetBlob] == sets1,
              "batch: no-op commit writes nothing");
    }
    // Leave a known state for the reboot run, then a REFUSED change it must
    // not see.
    CHECK(tiny_agent_set("keep-me") == ESP_OK, "final healthy switch");
    agent_shim::fail()[agent_shim::kCommit] = 1;
    CHECK(tiny_agent_roster_add("lost-on-reboot") != ESP_OK, "final refused add");
    agent_shim::reset();
    const Roster r = live();
    FILE *f = fopen(getenv("AGENT_EXPECT_FILE"), "w");
    if (f) {
        fprintf(f, "%s\n", tiny_agent_current());
        for (const auto &s : r) fprintf(f, "%s\n", s.c_str());
        fclose(f);
    }
    return 0;
}

static int reboot() {
    FILE *f = fopen(getenv("AGENT_EXPECT_FILE"), "r");
    Roster want;
    std::string active;
    char line[128];
    if (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        active = line;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            want.push_back(line);
        }
    }
    if (f) fclose(f);
    CHECK(!want.empty(), "reboot: expectation recorded");
    CHECK(active == tiny_agent_current(), "reboot: active slug \"%s\" kept",
          active.c_str());
    CHECK(live() == want, "reboot: roster kept (%zu rows)", want.size());
    bool lost = false;
    for (const auto &s : live()) lost = lost || s == "lost-on-reboot";
    CHECK(!lost, "reboot: a refused add did not come back");
    return 0;
}

// Store written by firmware before the blob: one key per field.
static int legacy() {
    CHECK(!strcmp(tiny_agent_current(), "bonk"), "legacy: active slug loads");
    CHECK(live() == (Roster{"tiny", "bonk", "clod"}), "legacy: roster loads");
    const std::string s0 = state();
    // First save refused after its blob landed: the blob must not stay
    // behind (it would shadow the legacy keys on the next boot).
    agent_shim::fail()[agent_shim::kCommit] = 1;
    CHECK(tiny_agent_roster_add("nope") != ESP_OK && state() == s0,
          "legacy: refused first save leaves the old store");
    agent_shim::reset();
    CHECK(tiny_agent_roster_add("next") == ESP_OK, "legacy: first save");
    return 0;
}
static int legacy_reboot() {
    CHECK(!strcmp(tiny_agent_current(), "bonk") &&
              live() == (Roster{"tiny", "bonk", "clod", "next"}),
          "legacy reboot: migrated state (no refused slug)");
    return 0;
}

// A "state" blob this build cannot decode (newer layout, torn by a tool):
// the legacy keys win, and a refused save must not change what a reboot
// loads.
static int corrupt() {
    CHECK(live() == (Roster{"tiny", "bonk"}), "corrupt blob: legacy keys load");
    const Roster r0 = live();
    agent_shim::fail()[agent_shim::kCommit] = 1;
    CHECK(tiny_agent_roster_add("zz") != ESP_OK && live() == r0,
          "corrupt blob: refused save leaves RAM");
    agent_shim::reset();
    return 0;
}
static int corrupt_reboot() {
    CHECK(live() == (Roster{"tiny", "bonk"}),
          "corrupt reboot: refused slug did not come back");
    return 0;
}

int main(int argc, char **argv) {
    const std::string phase = argc > 1 ? argv[1] : "fresh";
    if (phase == "fresh") fresh();
    else if (phase == "reboot") reboot();
    else if (phase == "legacy") legacy();
    else if (phase == "legacy-reboot") legacy_reboot();
    else if (phase == "corrupt") corrupt();
    else if (phase == "corrupt-reboot") corrupt_reboot();
    else { fprintf(stderr, "unknown phase %s\n", phase.c_str()); return 2; }
    if (g_fail) { fprintf(stderr, "%s: %d check(s) failed\n", phase.c_str(), g_fail); return 1; }
    return 0;
}
