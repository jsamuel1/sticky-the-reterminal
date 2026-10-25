// rotation host test — the SHIPPED manual-rotation rules (tiny/tiny_rotation.h)
// and the SHIPPED NVS persistence (tiny_config.cpp against a file-backed NVS
// shim, so a process restart is a genuine reboot).
//
//   rotation_test rules       capability gate, boot default, strict tap ids,
//                             names, picker card, select() decision
//   rotation_test save <deg>  pick <deg> the way the settings row does
//   rotation_test boot <deg>  "reboot": what the saved value boots as
//   rotation_test raw <byte>  write a raw stored byte (a torn/future value)
//   rotation_test failsave    a failed NVS write never reports persisted
#include "tiny/tiny_rotation.h"
#include "tiny/tiny_config.h"

#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++fails;                                                      \
        }                                                                 \
    } while (0)

static int boot_deg(void) {
    uint8_t q = 0;
    const bool have = tiny_config_rotation_get(&q);
    return tiny_rotation_boot_deg(have, q);
}

// select() fakes: what the display took and what got saved.
static std::vector<int> g_applied;
static std::vector<int> g_saved;
static int g_apply_err = 0;
static int g_save_err = 0;
static int fake_apply(int deg) { g_applied.push_back(deg); return g_apply_err; }
static int fake_save(uint8_t q) { g_saved.push_back(q); return g_save_err; }
static void reset_fakes() { g_applied.clear(); g_saved.clear(); g_apply_err = g_save_err = 0; }
// The production pair: tiny_config.cpp is the real one.
static int real_save(uint8_t q) { return tiny_config_rotation_set(q); }

static void rules(void) {
    // ---- capability gate: board_caps.imu is the only input ----
    CHECK(tiny_rotation_manual_offered(false));   // FNK0104S: no IMU
    CHECK(!tiny_rotation_manual_offered(true));   // reTerminal, AMOLED-1.8

    // ---- supported set: the four canvas rotations, nothing else ----
    CHECK(tiny_rotation_supported_count() == 4);
    for (int i = 0; i < 4; ++i) CHECK(tiny_rotation_supported_deg(i) == i * 90);
    CHECK(tiny_rotation_supported_deg(-1) == -1);
    CHECK(tiny_rotation_supported_deg(4) == -1);
    for (int d : {0, 90, 180, 270}) CHECK(tiny_rotation_valid_deg(d));
    for (int d : {-90, 1, 45, 89, 91, 360, 450}) CHECK(!tiny_rotation_valid_deg(d));

    // ---- boot default: unset / out of range -> native frame ----
    CHECK(tiny_rotation_boot_deg(false, 0) == TINY_ROTATION_DEFAULT_DEG);
    CHECK(tiny_rotation_boot_deg(false, 2) == TINY_ROTATION_DEFAULT_DEG);
    CHECK(TINY_ROTATION_DEFAULT_DEG == 0);
    for (uint8_t q = 0; q <= 3; ++q) CHECK(tiny_rotation_boot_deg(true, q) == q * 90);
    for (uint8_t q : {4, 9, 90, 180, 255}) CHECK(tiny_rotation_boot_deg(true, q) == 0);
    for (int d : {0, 90, 180, 270})
        CHECK(tiny_rotation_boot_deg(true, tiny_rotation_to_stored(d)) == d);

    // ---- strict tap ids ----
    int deg = -7;
    CHECK(tiny_rotation_parse_id("rot:open", &deg) == TINY_ROTATION_ID_OPEN && deg == -7);
    CHECK(tiny_rotation_parse_id("rot:back", &deg) == TINY_ROTATION_ID_BACK && deg == -7);
    for (int d : {0, 90, 180, 270}) {
        char id[16];
        snprintf(id, sizeof id, "rot:%d", d);
        deg = -7;
        CHECK(tiny_rotation_parse_id(id, &deg) == TINY_ROTATION_ID_SET && deg == d);
    }
    for (const char *b : {"rot:", "rot:45", "rot:90x", "rot:0090", "rot:-90",
                          "rot:360", "rot: 90", "rot:auto", "rot:OPEN"}) {
        deg = -7;
        CHECK(tiny_rotation_parse_id(b, &deg) == TINY_ROTATION_ID_BAD && deg == -7);
    }
    CHECK(tiny_rotation_parse_id("rotlock", &deg) == TINY_ROTATION_ID_NONE);
    CHECK(tiny_rotation_parse_id("home", &deg) == TINY_ROTATION_ID_NONE);
    CHECK(tiny_rotation_parse_id(nullptr, &deg) == TINY_ROTATION_ID_NONE);

    // ---- names follow the PHYSICAL panel aspect ----
    // FNK0104S is a 320x480 portrait panel: 0 = portrait, 90 = landscape.
    CHECK(!strcmp(tiny_rotation_name(0, true), "portrait"));
    CHECK(!strcmp(tiny_rotation_name(90, true), "landscape"));
    CHECK(!strcmp(tiny_rotation_name(180, true), "portrait, flipped"));
    CHECK(!strcmp(tiny_rotation_name(270, true), "landscape, flipped"));
    // An 800x480 landscape panel reads the other way round.
    CHECK(!strcmp(tiny_rotation_name(0, false), "landscape"));
    CHECK(!strcmp(tiny_rotation_name(90, false), "portrait"));

    // ---- settings row ----
    char lab[36], note[40];
    tiny_rotation_row_label(lab, sizeof lab, 90);
    CHECK(!strcmp(lab, "rotation: 90 deg"));
    tiny_rotation_row_note(note, sizeof note, 90, true);
    CHECK(!strcmp(note, "landscape - tap to choose"));

    // ---- picker card: valid JSON, one row per orientation, live one marked ----
    for (int cur : {0, 90, 180, 270}) {
        char card[1024];
        const int n = tiny_rotation_picker_json(card, sizeof card, cur, true);
        CHECK(n > 0 && (size_t)n < sizeof card);
        cJSON *o = cJSON_Parse(card);
        CHECK(o != nullptr);
        if (!o) continue;
        CHECK(!strcmp(cJSON_GetObjectItem(o, "card_id")->valuestring, "rotation"));
        CHECK(!strcmp(cJSON_GetObjectItem(o, "type")->valuestring, "menu"));
        const cJSON *items = cJSON_GetObjectItem(o, "items");
        CHECK(cJSON_GetArraySize(items) == 4);
        int active = 0;
        for (int i = 0; i < cJSON_GetArraySize(items); ++i) {
            const cJSON *it = cJSON_GetArrayItem(items, i);
            const char *id = cJSON_GetObjectItem(it, "id")->valuestring;
            const char *nt = cJSON_GetObjectItem(it, "note")->valuestring;
            int d = -1;
            CHECK(tiny_rotation_parse_id(id, &d) == TINY_ROTATION_ID_SET && d == i * 90);
            if (strstr(nt, "ACTIVE")) { ++active; CHECK(d == cur); }
        }
        CHECK(active == 1);
        const cJSON *btns = cJSON_GetObjectItem(o, "buttons");
        CHECK(cJSON_GetArraySize(btns) == 2);
        CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(btns, 0), "id")->valuestring,
                      "rot:back"));
        CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(btns, 1), "id")->valuestring,
                      "home"));
        cJSON_Delete(o);
    }
    char tiny_buf[32];
    CHECK(tiny_rotation_picker_json(tiny_buf, sizeof tiny_buf, 0, true) == -1);

    // ---- select(): gate, apply-then-save, never save an uncommitted angle ----
    bool persisted = true;
    reset_fakes();
    CHECK(tiny_rotation_select(90, /*imu=*/true, fake_apply, fake_save, &persisted)
          == 0x106);  // ESP_ERR_NOT_SUPPORTED
    CHECK(g_applied.empty() && g_saved.empty() && !persisted);

    reset_fakes();
    CHECK(tiny_rotation_select(45, false, fake_apply, fake_save, &persisted) == 0x102);
    CHECK(g_applied.empty() && g_saved.empty() && !persisted);

    reset_fakes();
    CHECK(tiny_rotation_select(270, false, fake_apply, fake_save, &persisted) == 0);
    CHECK(g_applied == std::vector<int>{270});
    CHECK(g_saved == std::vector<int>{3});
    CHECK(persisted);

    reset_fakes();
    g_apply_err = -1;  // panel refresh failed: glass still shows the old angle
    CHECK(tiny_rotation_select(180, false, fake_apply, fake_save, &persisted) == -1);
    CHECK(g_applied == std::vector<int>{180});
    CHECK(g_saved.empty());  // the next boot must not come up at 180
    CHECK(!persisted);

    reset_fakes();
    g_save_err = -1;  // NVS full: rotation holds until reboot, and says so
    CHECK(tiny_rotation_select(90, false, fake_apply, fake_save, &persisted) == 0);
    CHECK(g_saved == std::vector<int>{1} && !persisted);

    // Missing step: refused before anything moves (no crash, no half-select).
    reset_fakes();
    persisted = true;
    CHECK(tiny_rotation_select(90, false, nullptr, fake_save, &persisted) == 0x102);
    CHECK(g_saved.empty() && !persisted);
    CHECK(tiny_rotation_select(90, false, fake_apply, nullptr, &persisted) == 0x102);
    CHECK(g_applied.empty() && !persisted);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage\n"); return 2; }
    const std::string mode = argv[1];
    if (mode == "rules") {
        rules();
    } else if (mode == "fresh") {
        uint8_t q = 99;
        CHECK(!tiny_config_rotation_get(&q) && q == 99);  // never written
        CHECK(boot_deg() == 0);
    } else if (mode == "save" && argc > 2) {
        const int deg = atoi(argv[2]);
        bool persisted = false;
        reset_fakes();
        CHECK(tiny_rotation_select(deg, false, fake_apply, real_save, &persisted) == 0);
        CHECK(persisted);
        CHECK(g_applied == std::vector<int>{deg});
        CHECK(boot_deg() == deg);
    } else if (mode == "boot" && argc > 2) {
        CHECK(boot_deg() == atoi(argv[2]));
    } else if (mode == "raw" && argc > 2) {
        // A value outside 0..3 cannot come from set(): it is refused there.
        const int v = atoi(argv[2]);
        if (v <= 3) {
            CHECK(tiny_config_rotation_set((uint8_t)v) == 0);
        } else {
            CHECK(tiny_config_rotation_set((uint8_t)v) == 0x102);
            // ...so plant it the way a future firmware's encoding would.
            nvs_handle_t h;
            nvs_open("tiny", NVS_READWRITE, &h);
            nvs_shim::store()[nvs_shim::ns() + ".rotq"] = std::to_string(v);
            nvs_commit(h);
            nvs_close(h);
        }
    } else if (mode == "failsave") {
        const int before = boot_deg();
        rotation_shim::fail_next_set() = true;
        bool persisted = true;
        reset_fakes();
        CHECK(tiny_rotation_select(before == 90 ? 180 : 90, false, fake_apply,
                                   real_save, &persisted) == 0);
        CHECK(!persisted);
        CHECK(boot_deg() == before);
    } else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 2;
    }
    if (fails) {
        std::fprintf(stderr, "%d rotation check(s) failed (%s)\n", fails, mode.c_str());
        return 1;
    }
    return 0;
}
