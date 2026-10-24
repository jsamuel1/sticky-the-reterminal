// render host test — draws real cards through the SHIPPED tiny_display.cpp
// (with canvas.cpp, font.cpp, cJSON 1.7.18 and qrcodegen: the device's own
// code) on a host canvas of each board's panel size, then checks what a human
// would see:
//
//   * nothing touches the outer edge bands of the glass (ink clipped by the
//     panel edge = a cut word), except cards that are full-bleed by design;
//   * every published tap region lies inside the panel;
//   * the per-size golden (FNV-1a of the gray4 framebuffer) still matches, so a
//     layout change for a small panel cannot silently move a reTerminal pixel;
//   * PNGs of every frame, in the board's own theme (colour boards: light FNK,
//     dark AMOLED), land in $RENDER_OUT for the PR / card evidence.
//
// Usage: render_test <golden-file> [--update]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

#include "esp_http_client.h"
#include "canvas.h"
#include "lcd_palette.h"
#include "tiny/tiny_display.h"

// ---- the panel: a host Canvas the renderer draws into -----------------------
static Canvas *g_canvas = nullptr;
static std::vector<uint8_t> g_buf;
static std::vector<uint8_t> g_acc;   // accent plane (colour boards only)

static bool g_fail_next_full = false;
static int g_full_refreshes = 0;
static std::mutex g_panel_mutex;
static std::condition_variable g_panel_ready;
static bool g_block_next_full = false;
static bool g_panel_blocked = false;
static bool g_release_panel = false;
static bool g_probe_rotation = false;
static bool g_rotation_probed = false;
static int g_rotation_during_block = -1;
static std::mutex g_attempt_mutex;
static std::condition_variable g_attempt_ready;
static int g_render_attempts = 0;
static std::mutex g_http_mutex;
static std::condition_variable g_http_ready;
static bool g_hold_http = false;
static bool g_http_blocked = false;
static bool g_release_http = false;
static std::vector<uint8_t> g_http_body;
static size_t g_http_offset = 0;
static constexpr uintptr_t kHttpHandle = 1;

void render_test_render_mutex_attempt() {
    std::lock_guard<std::mutex> lock(g_attempt_mutex);
    ++g_render_attempts;
    g_attempt_ready.notify_all();
}

static bool wait_for_attempts(int count) {
    std::unique_lock<std::mutex> lock(g_attempt_mutex);
    return g_attempt_ready.wait_for(lock, std::chrono::seconds(2),
                                    [&] { return g_render_attempts >= count; });
}

esp_http_client_handle_t render_test_http_init(const esp_http_client_config_t *) {
    std::lock_guard<std::mutex> lock(g_http_mutex);
    g_http_offset = 0;
    return g_hold_http ? reinterpret_cast<void *>(kHttpHandle) : nullptr;
}

esp_err_t render_test_http_open(esp_http_client_handle_t handle, int) {
    if (handle != reinterpret_cast<void *>(kHttpHandle)) return ESP_FAIL;
    std::unique_lock<std::mutex> lock(g_http_mutex);
    g_http_blocked = true;
    g_http_ready.notify_all();
    g_http_ready.wait(lock, [] { return g_release_http; });
    g_http_blocked = false;
    return ESP_OK;
}

int64_t render_test_http_fetch_headers(esp_http_client_handle_t handle) {
    return handle == reinterpret_cast<void *>(kHttpHandle)
               ? static_cast<int64_t>(g_http_body.size())
               : -1;
}

int render_test_http_status(esp_http_client_handle_t handle) {
    return handle == reinterpret_cast<void *>(kHttpHandle) ? 200 : 0;
}

int render_test_http_read(esp_http_client_handle_t handle, char *out,
                          size_t length) {
    if (handle != reinterpret_cast<void *>(kHttpHandle)) return -1;
    std::lock_guard<std::mutex> lock(g_http_mutex);
    const size_t left = g_http_body.size() - g_http_offset;
    const size_t n = std::min(left, length);
    if (!n) return 0;
    memcpy(out, g_http_body.data() + g_http_offset, n);
    g_http_offset += n;
    return static_cast<int>(n);
}

esp_err_t render_test_http_cleanup(esp_http_client_handle_t) { return ESP_OK; }

Canvas *sticky_display_canvas() { return g_canvas; }
esp_err_t sticky_display_init() { return 0; }
esp_err_t sticky_display_refresh() { return 0; }
esp_err_t sticky_display_refresh_partial() { return 0; }
esp_err_t sticky_display_refresh_partial_baseline(const uint8_t *) { return 0; }
esp_err_t sticky_display_refresh_monochrome() {
    ++g_full_refreshes;
    if (g_fail_next_full) {
        g_fail_next_full = false;
        return ESP_FAIL;
    }
    std::unique_lock<std::mutex> lock(g_panel_mutex);
    if (g_block_next_full) {
        g_block_next_full = false;
        g_panel_blocked = true;
        g_panel_ready.notify_all();
        while (!g_release_panel) {
            g_panel_ready.wait(lock, [] {
                return g_release_panel || g_probe_rotation;
            });
            if (g_probe_rotation) {
                g_probe_rotation = false;
                lock.unlock();
                const int rotation = tiny_display_rotation();
                lock.lock();
                g_rotation_during_block = rotation;
                g_rotation_probed = true;
                g_panel_ready.notify_all();
            }
        }
        g_panel_blocked = false;
        g_release_panel = false;
    }
    return ESP_OK;
}
esp_err_t sticky_display_clear() { return 0; }
esp_err_t sticky_display_sleep() { return 0; }
esp_err_t sticky_display_set_brightness(uint8_t) { return ESP_ERR_NOT_SUPPORTED; }
int sticky_display_brightness() { return -1; }

// ---- everything else the renderer calls: "nothing here" ---------------------
#include "sticky_battery.h"
#include "sticky_buzzer.h"
#include "sticky_sdcard.h"
#include "tiny/tiny_askq.h"
#include "tiny/tiny_agent.h"
#include "tiny/tiny_config.h"
#include "tiny/tiny_lock.h"
#include "tiny/tiny_node.h"
#include "tiny/tiny_shell.h"
#include "tiny/tiny_time.h"
#include "tiny/tiny_wifi.h"

esp_err_t sticky_battery_read(BatteryReading &) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_buzzer_beep() { return 0; }
esp_err_t sticky_buzzer_beep_double() { return 0; }
extern "C" esp_err_t sticky_sdcard_ensure(void) { return ESP_ERR_NOT_FOUND; }
extern "C" int tiny_askq_count(void) { return 0; }
static char g_agent[TINY_AGENT_SLUG_MAX + 1] = "";
extern "C" const char *tiny_agent_current(void) { return g_agent; }
extern "C" esp_err_t tiny_config_gallery_save(const void *, size_t, uint8_t, uint8_t) { return 0; }
extern "C" esp_err_t tiny_config_gallery_save_index(uint8_t) { return 0; }
extern "C" esp_err_t tiny_config_gallery_load(void *, size_t, uint8_t *, uint8_t *) { return ESP_ERR_NOT_FOUND; }
extern "C" bool tiny_lock_is_locked(void) { return false; }
static int g_unread = 0;
extern "C" int tiny_node_unread(void) { return g_unread; }
extern "C" esp_err_t tiny_shell_home(void) { return 0; }
extern "C" esp_err_t tiny_shell_key(const char *) { return 0; }
extern "C" esp_err_t tiny_shell_nav_back(void) { return 0; }
extern "C" bool tiny_shell_at_root(void) { return true; }
extern "C" esp_err_t tiny_shell_cycle(int) { return 0; }
extern "C" esp_err_t tiny_shell_universe_push(const char *) { return 0; }
// A fixed clock so the glance cluster has its widest realistic content.
extern "C" bool tiny_time_is_synced(void) { return true; }
extern "C" void tiny_time_utc_string(char *out, size_t cap) {
    snprintf(out, cap, "2026-10-24 12:34:56Z");
}
extern "C" bool tiny_wifi_is_up(void) { return true; }

// Status light (tiny/tiny_led.h, CONFIG_TINY_STATUS_LED via render_shim's
// sdkconfig.h): record what the renderer tells the alert layer.
static int g_led_alert_calls = 0;
static bool g_led_alert = false;
extern "C" void tiny_led_alert(bool on) { ++g_led_alert_calls; g_led_alert = on; }

// ---- PNG (gray8, stored deflate via zlib) -----------------------------------
static void put32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back((uint8_t)(x >> (8 * i)));
}
static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> c;
    put32(c, (uint32_t)data.size());
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    const uint32_t crc = crc32(0, c.data() + 4, (uInt)(c.size() - 4));
    put32(c, crc);
    fwrite(c.data(), 1, c.size(), f);
}
static void write_png(const char *path, int w, int h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)w); put32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8-bit RGB
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
    }
    uLongf zl = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zl);
    compress2(z.data(), &zl, raw.data(), (uLong)raw.size(), 9);
    z.resize(zl);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
}

// ---- checks ----------------------------------------------------------------
static int g_fail = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) { ++g_fail; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Gray level of physical pixel (x,y) from the packed 2bpp buffer.
static int px(int x, int y) {
    const uint8_t b = g_buf[(size_t)y * g_canvas->stride() + x / 4];
    return (b >> (6 - 2 * (x % 4))) & 3;
}

// Physical-space ink inside a band `band` px wide along every edge.
static int edge_ink(int band) {
    const int W = g_canvas->physical_width(), H = g_canvas->physical_height();
    int n = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool edge = x < band || y < band || x >= W - band || y >= H - band;
            if (edge && px(x, y) != 3) ++n;
        }
    return n;
}

struct Fixture { const char *name; std::string json; bool full_bleed; };

static std::vector<Fixture> fixtures() {
    const std::string long_body =
        "The quick brown fox jumps over the lazy dog. Agent prose arrives "
        "with em-dashes \xE2\x80\x94 curly quotes \xE2\x80\x9Clike these\xE2\x80\x9D and status "
        "glyphs \xE2\x9C\x93 \xE2\x9A\xA0; Turkish \xC3\xA7\xC4\x9F\xC4\xB1\xC3\xB6\xC5\x9F\xC3\xBC renders as written. "
        "A Supercalifragilisticexpialidocious token tests the hard wrap.";
    return {
        {"text", R"J({"type":"text","card_id":"t1","title":"A title long enough to need cutting on a small panel","body":")J" + long_body + R"J(","footer":"footer line that is also quite long - tiny sticky 0.32.0"})J", false},
        {"text_buttons", R"J({"type":"text","card_id":"t2","title":"confirm","body":"Turn the heater on in the living room?","buttons":["Yes","No","Later"]})J", false},
        {"list", R"J({"type":"list","card_id":"l1","title":"today","items":["Stand-up 09:30 with the platform team","Review CR-123456789 before lunch","Buy milk","Call the dentist about the appointment next Thursday afternoon","Water plants"]})J", false},
        {"kv", R"J({"type":"kv","card_id":"status","title":"sticky","rows":[["firmware","0.32.0"],["battery","n/a"],["wifi","MyHomeNetwork-5G"],["signal","-61 dBm"],["uptime","3h 12m"],["heap","4123 KB"],["mac","ac:15:18:d7:eb:a6"],["device","3f2a9c1e..."]]})J", false},
        {"menu", R"J({"type":"menu","card_id":"m1","title":"wi-fi","items":[{"label":"MyHomeNetwork-5G","note":"-48 dBm","id":"w:0"},{"label":"Neighbours Very Long Network Name","note":"-71 dBm","id":"w:1"},{"label":"Rescan","id":"w:scan"}],"buttons":["Back","Home"]})J", false},
        {"chart_line", R"J({"type":"chart","card_id":"c1","title":"battery","data":[98,97,95,94,90,88,85,83,80,79,77,74],"labels":["09:00","21:00"],"unit":"%","y_min":0,"y_max":100})J", false},
        {"chart_bar", R"J({"type":"chart","card_id":"c2","title":"steps","style":"bar","data":[3200,5400,1200,8800,6400,7200,4100],"labels":["Mon","Sun"]})J", false},
        {"chat", R"J({"type":"chat","card_id":"ch1","title":"tiny","messages":[{"role":"user","body":"What's on my calendar?"},{"role":"assistant","body":"Stand-up at 09:30, a review at 11:00, and lunch with Sam at 12:30."},{"role":"user","body":"Yes."}]})J", false},
        {"qr", R"J({"type":"qr","card_id":"q1","title":"pair","text":"https://tiny.technology/pair?code=ABCD-1234-EFGH","caption":"or type ABCD-1234 at tiny.technology/pair"})J", false},
        {"composite_wifi", R"J({"type":"composite","card_id":"ob-wifi","title":"1/3 - wi-fi","parts":[{"type":"text","body":"Scan with your phone camera, then open http://192.168.4.1 and pick your Wi-Fi."},{"type":"qr","text":"WIFI:S:tiny-eba6;T:WPA;P:tinysetup;;","caption":"no camera? join  tiny-eba6  / pw  tinysetup"}],"buttons":[{"label":"Back","id":"ob:back"},{"label":"Type it here","id":"ob:type"}]})J", false},
        {"composite_settings", R"J({"type":"composite","card_id":"settings","title":"settings","parts":[{"type":"text","body":"Wi-Fi joins a network. Config = QR for phone setup."},{"type":"menu","items":[{"label":"sound: ON","note":"tap to mute beeps","id":"silent"},{"label":"beeps: NORMAL","note":"tap for soft","id":"beepvol"},{"label":"rotation: AUTO","note":"tap to lock at 0 deg","id":"rotlock"}]},{"type":"kv","rows":[["name","tiny-eba6"],["fw","0.32.0"],["channel","sticky-dev"],["wifi","MyHomeNetwork-5G (-61 dBm)"],["mac","ac:15:18:d7:eb:a6"]]}],"buttons":["Wi-Fi","Bluetooth","Config","Home"]})J", false},
        {"keyboard", R"J({"type":"keyboard","card_id":"kb","title":"wi-fi password","value":"correct horse battery staple"})J", false},
        {"keyboard_sym", R"J({"type":"keyboard","card_id":"kb2","title":"message","value":"","sym":true})J", false},
        {"home", R"J({"type":"agent_home","card_id":"home"})J", false},
        {"home_preview", R"J({"type":"agent_home","card_id":"home","preview":"Stand-up at 09:30, a review at 11:00, and lunch with Sam at 12:30 - want reminders?"})J", false},
        {"buttons5", R"J({"type":"buttons","card_id":"b5","title":"pick","buttons":["One","Two","Three","Four","Bluetooth"]})J", false},
        {"alert", R"J({"type":"text","card_id":"al","title":"door open","priority":"alert","body":"The back door has been open for 10 minutes."})J", false},
        {"unknown", R"J({"type":"hologram","card_id":"u1","title":"future","body":"An agent sent a type this firmware does not know."})J", false},
    };
}

struct Size { int w, h; int rot; const char *tag; bool color; bool dark; };

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <golden> [--update]\n", argv[0]); return 2; }
    const bool update = argc > 2 && !strcmp(argv[2], "--update");
    const char *out_dir = getenv("RENDER_OUT");

    // Golden file: "<tag>/<fixture> <hash>" per line.
    std::map<std::string, uint32_t> golden;
    if (FILE *g = fopen(argv[1], "r")) {
        char k[96]; unsigned v;
        while (fscanf(g, "%95s %x", k, &v) == 2) golden[k] = v;
        fclose(g);
    }
    std::map<std::string, uint32_t> seen;

    // Panel sizes in PHYSICAL pixels, plus the rotation the board uses.
    // reTerminal: 800x480 landscape, and 90 deg (the IMU portrait turn).
    // FNK0104S: the 320x480 portrait panel at rotation 0.
    // AMOLED-1.8: the 368x448 panel at rotation 0, and 90 deg (QMI8658 turn).
    const Size sizes[] = {
        {800, 480, 0, "reterminal-0", false, false},
        {800, 480, 90, "reterminal-90", false, false},
        {320, 480, 0, "fnk0104s", true, false},     // light theme
        {368, 448, 0, "amoled18", true, true},      // dark theme (AMOLED)
        {368, 448, 90, "amoled18-90", true, true},  // landscape 448x368
    };
    for (const Size &sz : sizes) {
        const size_t stride = (size_t)(sz.w + 3) / 4;
        g_buf.assign(stride * sz.h, 0xFF);
        Canvas canvas((uint16_t)sz.w, (uint16_t)sz.h, g_buf.data(), g_buf.size());
        if (sz.color) {   // what lcd_canvas does on the colour boards
            g_acc.assign(canvas.accent_stride() * sz.h, 0);
            canvas.set_accent_plane(g_acc.data(), g_acc.size());
        } else {
            g_acc.clear();
        }
        g_canvas = &canvas;
        tiny_display_init();
        tiny_display_set_rotation(sz.rot);
        for (const Fixture &fx : fixtures()) {
            const std::string key = std::string(sz.tag) + "/" + fx.name;
            const int led_calls_before = g_led_alert_calls;
            const esp_err_t r = tiny_display_render_card(fx.json.c_str());
            CHECK(r == 0, "%s: render returned %d", key.c_str(), r);
            // Status light: exactly one alert-layer update per committed card,
            // on only for the priority:"alert" fixture.
            const bool want_alert = !strcmp(fx.name, "alert");
            CHECK(g_led_alert_calls == led_calls_before + 1,
                  "%s: %d tiny_led_alert calls for one commit", key.c_str(),
                  g_led_alert_calls - led_calls_before);
            CHECK(g_led_alert == want_alert, "%s: led alert %d, want %d",
                  key.c_str(), g_led_alert, want_alert);
            // Ink in the outer 4 px of the glass is ink the edge cut off.
            const int ink = edge_ink(4);
            CHECK(fx.full_bleed || ink == 0, "%s: %d px of ink in the 4 px edge band",
                  key.c_str(), ink);
            tiny_button_region_t regs[48];
            const int n = tiny_display_button_regions(regs, 48);
            for (int i = 0; i < n; ++i) {
                const auto &q = regs[i];
                CHECK(q.x >= 0 && q.y >= 0 && q.w > 0 && q.h > 0 &&
                          q.x + q.w <= sz.w && q.y + q.h <= sz.h,
                      "%s: region %d \"%s\" (%d,%d %dx%d) leaves the %dx%d panel",
                      key.c_str(), i, q.id, q.x, q.y, q.w, q.h, sz.w, sz.h);
            }
            uint32_t hsh = fnv1a(g_buf.data(), g_buf.size());
            if (!g_acc.empty()) hsh ^= fnv1a(g_acc.data(), g_acc.size()) * 31u;
            seen[key] = hsh;
            if (out_dir) {
                // What the glass shows: palette index -> the board's theme.
                uint8_t *idx = nullptr; size_t len = 0; bool acc = false;
                if (tiny_display_snapshot_index8(&idx, &len, &acc) == 0) {
                    const auto theme = sz.dark ? lcd_palette::Theme::Dark
                                               : lcd_palette::Theme::Light;
                    std::vector<uint8_t> rgb(len * 3);
                    for (size_t i = 0; i < len; ++i) {
                        const uint8_t a = idx[i] >> 2, l = idx[i] & 3;
                        lcd_palette::Rgb c = sz.color
                            ? lcd_palette::color(theme, a, l)
                            : lcd_palette::gray((uint8_t)(l * 0x55));
                        rgb[3 * i] = c.r; rgb[3 * i + 1] = c.g; rgb[3 * i + 2] = c.b;
                    }
                    char path[512];
                    snprintf(path, sizeof path, "%s/%s-%s.png", out_dir, sz.tag, fx.name);
                    write_png(path, sz.w, sz.h, rgb.data());
                    free(idx);
                }
            }
        }

        if (!strcmp(sz.tag, "amoled18-90")) {
            // Wake-baseline body is persisted RTC text and can contain quotes,
            // backslashes and controls. It must round-trip through JSON rather
            // than breaking the hidden redraw and forcing a full wake flash.
            tiny_display_note_sleep_card("agent said \"wait\" \\ then\ncontinued");
            CHECK(tiny_display_arm_wake_baseline() == ESP_OK,
                  "wake baseline: escaped RTC body renders successfully");
            CHECK(tiny_display_render_card(
                      R"J({"type":"text","card_id":"wake-after","title":"awake","body":"ready"})J") == ESP_OK,
                  "wake baseline: next card consumes escaped baseline");
            CHECK(!strcmp(tiny_display_current_card_id(), "wake-after"),
                  "wake baseline: escaped body preserves next glass commit");

            // A failed panel commit must not publish the requested angle as
            // current. tiny_orient compares against this state before calling
            // again; retaining the failed target would suppress every
            // same-pose retry indefinitely.
            const int before = tiny_display_rotation();
            const int target = (before + 90) % 360;
            const int refreshes = g_full_refreshes;
            g_fail_next_full = true;
            CHECK(tiny_display_set_rotation(target) == ESP_FAIL,
                  "rotation: injected panel failure is returned");
            CHECK(tiny_display_rotation() == before,
                  "rotation: failed commit keeps prior glass angle");
            CHECK(g_full_refreshes == refreshes + 1,
                  "rotation: failed request reaches panel once");
            CHECK(tiny_display_set_rotation(target) == ESP_OK,
                  "rotation: same angle retries after failure");
            CHECK(tiny_display_rotation() == target,
                  "rotation: successful retry commits target angle");
            CHECK(g_full_refreshes == refreshes + 2,
                  "rotation: retry reaches panel again");

            // Hold one card at the panel boundary, then queue auto-rotation,
            // a newer card, and a state reader in that order. The complete
            // cache -> canvas -> panel transaction must stay serialized. The
            // old partial lock let rotation set s_rerendering before waiting;
            // the newer card then skipped its cache update, so a later
            // rerender resurrected the stale card (and the reader observed a
            // not-yet-committed angle).
            CHECK(tiny_display_set_rotation(0) == ESP_OK,
                  "serialization: start at rotation 0");
            {
                std::lock_guard<std::mutex> lock(g_panel_mutex);
                g_block_next_full = true;
            }
            std::atomic<int> held_result{ESP_FAIL};
            std::thread held([&] {
                held_result = tiny_display_render_card(
                    R"J({"type":"text","card_id":"race-held","title":"held","body":"first"})J");
            });
            {
                std::unique_lock<std::mutex> lock(g_panel_mutex);
                CHECK(g_panel_ready.wait_for(lock, std::chrono::seconds(2),
                                             [] { return g_panel_blocked; }),
                      "serialization: first render reached blocked panel");
            }
            int attempts;
            {
                std::lock_guard<std::mutex> lock(g_attempt_mutex);
                attempts = g_render_attempts;
            }
            std::atomic<int> rotate_result{ESP_FAIL};
            std::thread rotate([&] { rotate_result = tiny_display_set_rotation(90); });
            CHECK(wait_for_attempts(++attempts),
                  "serialization: rotation waits before state mutation");
            {
                std::unique_lock<std::mutex> lock(g_panel_mutex);
                g_probe_rotation = true;
                g_panel_ready.notify_all();
                CHECK(g_panel_ready.wait_for(lock, std::chrono::seconds(2),
                                             [] { return g_rotation_probed; }),
                      "serialization: blocked render samples logical angle");
                CHECK(g_rotation_during_block == 0,
                      "serialization: angle stays old until panel commit");
            }
            std::atomic<int> newer_result{ESP_FAIL};
            std::thread newer([&] {
                newer_result = tiny_display_render_card(
                    R"J({"type":"text","card_id":"race-new","title":"new","body":"second"})J");
            });
            CHECK(wait_for_attempts(++attempts),
                  "serialization: newer card waits before cache mutation");
            std::atomic<bool> read_done{false};
            std::atomic<int> observed{-1};
            std::thread reader([&] {
                observed = tiny_display_rotation();
                read_done = true;
            });
            CHECK(wait_for_attempts(++attempts),
                  "serialization: rotation read joins transaction lock");
            CHECK(!read_done.load(),
                  "serialization: reader cannot expose uncommitted rotation");
            {
                std::lock_guard<std::mutex> lock(g_panel_mutex);
                g_release_panel = true;
                g_panel_ready.notify_all();
            }
            held.join();
            rotate.join();
            newer.join();
            reader.join();
            CHECK(held_result == ESP_OK && rotate_result == ESP_OK &&
                      newer_result == ESP_OK,
                  "serialization: overlapped render/rotate/render all commit");
            CHECK(observed == 90 && tiny_display_rotation() == 90,
                  "serialization: reader sees committed 90-degree angle");
            CHECK(!strcmp(tiny_display_current_card_id(), "race-new"),
                  "serialization: newer card is on glass");
            CHECK(tiny_display_rerender() == ESP_OK &&
                      !strcmp(tiny_display_current_card_id(), "race-new"),
                  "serialization: rerender keeps newer cached card");

            // Network bytes are fetched with no render/panel lock held. A newer
            // card must commit while the image request is blocked, then the
            // stale image response must be discarded without replacing glass
            // identity or replay cache.
            {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_http_body.assign(static_cast<size_t>(sz.w) * sz.h / 4, 0x55);
                g_hold_http = true;
                g_http_blocked = false;
                g_release_http = false;
            }
            std::atomic<int> image_result{ESP_OK};
            std::thread image([&] {
                image_result = tiny_display_render_card(
                    R"J({"type":"image","card_id":"race-image","url":"https://example.invalid/frame.raw"})J");
            });
            {
                std::unique_lock<std::mutex> lock(g_http_mutex);
                CHECK(g_http_ready.wait_for(lock, std::chrono::seconds(2),
                                            [] { return g_http_blocked; }),
                      "serialization: image fetch reaches held HTTPS");
            }
            CHECK(tiny_display_render_card(
                      R"J({"type":"text","card_id":"race-overtake","title":"newest","body":"wins"})J") == ESP_OK,
                  "serialization: newer card overtakes held image fetch");
            CHECK(!strcmp(tiny_display_current_card_id(), "race-overtake"),
                  "serialization: overtaking card commits to glass");
            {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_release_http = true;
                g_http_ready.notify_all();
            }
            image.join();
            CHECK(image_result == ESP_ERR_INVALID_STATE,
                  "serialization: stale image response is refused");
            CHECK(!strcmp(tiny_display_current_card_id(), "race-overtake"),
                  "serialization: stale image cannot overwrite glass");
            CHECK(tiny_display_rerender() == ESP_OK &&
                      !strcmp(tiny_display_current_card_id(), "race-overtake"),
                  "serialization: stale image cannot replace replay cache");
            {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_hold_http = false;
                g_release_http = false;
                g_http_body.clear();
            }
        }
    }

    if (update) {
        FILE *g = fopen(argv[1], "w");
        for (auto &kv : seen) fprintf(g, "%s %08x\n", kv.first.c_str(), kv.second);
        fclose(g);
        printf("golden updated: %zu frames\n", seen.size());
    } else {
        for (auto &kv : seen) {
            auto it = golden.find(kv.first);
            CHECK(it != golden.end(), "%s: no golden (run with --update)", kv.first.c_str());
            if (it != golden.end())
                CHECK(it->second == kv.second, "%s: frame hash %08x != golden %08x",
                      kv.first.c_str(), kv.second, it->second);
        }
    }
    if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    printf("render: %zu frames, edge + region + golden checks green\n", seen.size());
    return 0;
}
