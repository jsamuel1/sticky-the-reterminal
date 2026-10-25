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
static std::string g_http_last_url;
static int g_http_fetches = 0;
// Wake baseline: the bytes the driver was handed as the "previous" plane.
static uint32_t g_baseline_hash = 0;
static int g_baseline_calls = 0;

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

esp_http_client_handle_t render_test_http_init(const esp_http_client_config_t *config) {
    std::lock_guard<std::mutex> lock(g_http_mutex);
    g_http_offset = 0;
    if (g_hold_http) {
        g_http_last_url = config && config->url ? config->url : "";
        ++g_http_fetches;
    }
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

// Allocation failure injection (render_shim/esp_heap_caps.h): every request
// of exactly g_fail_alloc_size bytes fails while armed.
static std::atomic<size_t> g_fail_alloc_size{0};
void *render_test_malloc(size_t n) {
    if (n && n == g_fail_alloc_size.load()) return nullptr;
    return malloc(n);
}

Canvas *sticky_display_canvas() { return g_canvas; }
esp_err_t sticky_display_init() { return 0; }
esp_err_t sticky_display_refresh() { return 0; }
esp_err_t sticky_display_refresh_partial() { return 0; }
static uint32_t fnv1a(const uint8_t *p, size_t n);
esp_err_t sticky_display_refresh_partial_baseline(const uint8_t *prev) {
    ++g_baseline_calls;
    g_baseline_hash = prev ? fnv1a(prev, g_buf.size()) : 0;
    return 0;
}
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
// Shell entry points the gesture router dispatches to: recorded so a test can
// prove WHICH routing policy a swipe took (keyboard vs ordinary card).
static int g_shell_home_calls = 0, g_shell_back_calls = 0, g_shell_cycle_calls = 0;
static std::string g_shell_last_key;
extern "C" esp_err_t tiny_shell_home(void) { ++g_shell_home_calls; return 0; }
extern "C" esp_err_t tiny_shell_key(const char *k) { g_shell_last_key = k ? k : ""; return 0; }
extern "C" esp_err_t tiny_shell_nav_back(void) { ++g_shell_back_calls; return 0; }
extern "C" bool tiny_shell_at_root(void) { return true; }
extern "C" esp_err_t tiny_shell_cycle(int) { ++g_shell_cycle_calls; return 0; }
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
            // backslashes and controls. The hidden redraw must carry THAT body
            // (not the consumed, cleared RTC slot) through JSON escaping: the
            // baseline handed to the driver must equal an explicitly escaped
            // goodbye card, and two different bodies must differ.
            const char *wake_body_a = "agent said \"wait\" \\ then\ncontinued";
            const char *wake_card_a =
                R"J({"type":"text","title":"zzz","card_id":"sleep","body":"agent said \"wait\" \\ then\ncontinued","footer":"deep sleep - the panel keeps this image at 0 mA"})J";
            auto wake_baseline_for = [&](const char *body) -> uint32_t {
                tiny_display_note_sleep_card(body);
                const int calls = g_baseline_calls;
                CHECK(tiny_display_arm_wake_baseline() == ESP_OK,
                      "wake baseline: RTC body renders successfully");
                CHECK(tiny_display_render_card(
                          R"J({"type":"text","card_id":"wake-after","title":"awake","body":"ready"})J") == ESP_OK,
                      "wake baseline: next card consumes baseline");
                CHECK(g_baseline_calls == calls + 1,
                      "wake baseline: wake commit uses the diff-partial path");
                CHECK(!strcmp(tiny_display_current_card_id(), "wake-after"),
                      "wake baseline: next glass commit is the wake card");
                return g_baseline_hash;
            };
            const uint32_t base_a = wake_baseline_for(wake_body_a);
            const uint32_t base_b = wake_baseline_for("a different goodbye");
            CHECK(base_a != base_b,
                  "wake baseline: body reaches the baseline (bodies differ)");
            CHECK(tiny_display_render_card(wake_card_a) == ESP_OK,
                  "wake baseline: explicit escaped goodbye card renders");
            CHECK(fnv1a(g_buf.data(), g_buf.size()) == base_a,
                  "wake baseline: equals explicitly escaped goodbye card");
            CHECK(tiny_display_arm_wake_baseline() == ESP_ERR_INVALID_STATE,
                  "wake baseline: RTC note is one-shot");

            // A failed panel commit must not publish the requested angle as
            // current. tiny_orient compares against this state before calling
            // again; retaining the failed target would suppress every
            // same-pose retry indefinitely.
            //
            // The card on glass is a keyboard so the failed request also
            // proves gesture POLICY survives: a bottom-band swipe delivered in
            // panel coordinates of the committed frame must still resolve to
            // keyboard cancel, which needs the restored canvas rotation AND
            // layout dimensions (the router maps panel -> logical through them).
            CHECK(tiny_display_render_card(
                      R"J({"type":"keyboard","card_id":"kb-rot","title":"pw","value":"abc"})J") == ESP_OK,
                  "rotation: keyboard commits before failed rotation");
            const int before = tiny_display_rotation();
            CHECK(before != 0, "rotation: failure case starts at nonzero angle (%d)",
                  before);
            const int target = (before + 90) % 360;
            const int refreshes = g_full_refreshes;
            const int glass_w = sticky_display_canvas()->width();
            const int glass_h = sticky_display_canvas()->height();
            g_fail_next_full = true;
            CHECK(tiny_display_set_rotation(target) == ESP_FAIL,
                  "rotation: injected panel failure is returned");
            CHECK(tiny_display_rotation() == before,
                  "rotation: failed commit keeps prior glass angle");
            CHECK(static_cast<int>(sticky_display_canvas()->rotation()) * 90 ==
                      before &&
                      sticky_display_canvas()->width() == glass_w &&
                      sticky_display_canvas()->height() == glass_h,
                  "rotation: failed commit restores canvas touch frame");
            CHECK(!strcmp(tiny_display_current_card_id(), "kb-rot"),
                  "rotation: failed commit keeps keyboard identity on glass");
            {
                Canvas *cv = sticky_display_canvas();
                int px0, py0, px1, py1;
                cv->to_physical(glass_w / 2, glass_h * 98 / 100, px0, py0);
                cv->to_physical(glass_w / 2, glass_h * 40 / 100, px1, py1);
                g_shell_last_key.clear();
                const int h0 = g_shell_home_calls;
                tiny_display_scroll_gesture(px0, py0, px1, py1);
                CHECK(g_shell_last_key == "k:cancel" && g_shell_home_calls == h0,
                      "rotation: failed commit keeps keyboard gesture policy "
                      "(key=%s home=%d)",
                      g_shell_last_key.c_str(), g_shell_home_calls - h0);
            }
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
            // Baseline is snapshotted only after `held` owns the render lock
            // and is parked at the panel, so earlier acquisitions (init,
            // prior cases, held's own) cannot offset it: each ++attempts
            // below counts exactly one new waiter.
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

            // ---- auto-rotation during an active stream ----------------------
            // Rotation must redraw the live answer (buffered words + caret) in
            // the new layout and keep the stream alive, not replay the cached
            // begin frame as an outsider card that ends the stream.
            auto frame_hash = [&]() {
                uint32_t h = fnv1a(g_buf.data(), g_buf.size());
                if (!g_acc.empty()) h ^= fnv1a(g_acc.data(), g_acc.size()) * 31u;
                return h;
            };
            auto rotated_stream_hash = [&](const char *words) -> uint32_t {
                CHECK(tiny_display_set_rotation(90) == ESP_OK,
                      "stream: start at rotation 90");
                CHECK(tiny_display_stream_begin("tiny", "ans", nullptr) == ESP_OK,
                      "stream: begin");
                if (words) tiny_display_stream_append(words);
                CHECK(tiny_display_set_rotation(0) == ESP_OK,
                      "stream: rotation during stream commits");
                CHECK(tiny_display_stream_active(),
                      "stream: rotation does not end the active stream");
                CHECK(!strcmp(tiny_display_current_card_id(), "ans"),
                      "stream: rotated frame is the stream card");
                const uint32_t h = frame_hash();
                CHECK(tiny_display_stream_end(nullptr) == ESP_OK,
                      "stream: owner can still end its stream after rotation");
                CHECK(!tiny_display_stream_active(), "stream: ended");
                return h;
            };
            const uint32_t with_words = rotated_stream_hash("rotated words arrive");
            const uint32_t without_words = rotated_stream_hash(nullptr);
            CHECK(with_words != without_words,
                  "stream: rotation redraws buffered words, not the begin frame");

            // Threaded: while auto-rotation's stream redraw is parked at the
            // panel, the ask engine's append must wait for render ownership
            // instead of writing the buffer the redraw is reading.
            CHECK(tiny_display_set_rotation(90) == ESP_OK,
                  "stream race: start at rotation 90");
            CHECK(tiny_display_stream_begin("tiny", "ans2", nullptr) == ESP_OK,
                  "stream race: begin");
            tiny_display_stream_append("first words");
            {
                std::lock_guard<std::mutex> lock(g_panel_mutex);
                g_block_next_full = true;
            }
            std::atomic<int> srot_result{ESP_FAIL};
            std::thread srot([&] { srot_result = tiny_display_set_rotation(0); });
            {
                std::unique_lock<std::mutex> lock(g_panel_mutex);
                CHECK(g_panel_ready.wait_for(lock, std::chrono::seconds(2),
                                             [] { return g_panel_blocked; }),
                      "stream race: rotation redraw reached blocked panel");
            }
            int sattempts;
            {
                std::lock_guard<std::mutex> lock(g_attempt_mutex);
                sattempts = g_render_attempts;
            }
            std::atomic<bool> append_done{false};
            std::thread sappend([&] {
                tiny_display_stream_append(" then more");
                append_done = true;
            });
            CHECK(wait_for_attempts(++sattempts),
                  "stream race: append waits for render ownership");
            CHECK(!append_done.load(),
                  "stream race: append cannot mutate buffer mid-redraw");
            {
                std::lock_guard<std::mutex> lock(g_panel_mutex);
                g_release_panel = true;
                g_panel_ready.notify_all();
            }
            srot.join();
            sappend.join();
            CHECK(srot_result == ESP_OK && tiny_display_stream_active(),
                  "stream race: rotation commits, stream survives");
            CHECK(tiny_display_stream_end(nullptr) == ESP_OK,
                  "stream race: stream ends normally");

            // ---- gallery: staged commit -------------------------------------
            // 1-bit frames take the monochrome refresh, which can fail/block.
            {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_http_body.assign(static_cast<size_t>(sz.w) * sz.h / 8, 0xAA);
                g_hold_http = true;
                g_release_http = true;   // serve immediately unless re-held
                g_http_fetches = 0;
            }
            auto fetches = [] {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                return g_http_fetches;
            };
            auto last_url = [] {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                return g_http_last_url;
            };
            const char *gal_a =
                R"J({"type":"gallery","urls":["https://h/a0.raw","https://h/a1.raw","https://h/a2.raw"],"index":0})J";
            CHECK(tiny_display_render_card(gal_a) == ESP_OK &&
                      tiny_display_gallery_count() == 3 &&
                      last_url() == "https://h/a0.raw",
                  "gallery: committed set A opens on a0");

            // New set whose panel refresh fails: set A stays the gallery.
            g_fail_next_full = true;
            CHECK(tiny_display_render_card(
                      R"J({"type":"gallery","urls":["https://h/b0.raw","https://h/b1.raw"],"index":0})J") == ESP_FAIL,
                  "gallery: failed new-set refresh is returned");
            CHECK(tiny_display_gallery_count() == 3,
                  "gallery: failed new set keeps committed count");
            CHECK(tiny_display_gallery_nav(+1) == ESP_OK &&
                      last_url() == "https://h/a1.raw",
                  "gallery: next after failed new set pages set A (a1)");

            // Page flip whose refresh fails: logical page stays on glass page.
            int f0 = fetches();
            g_fail_next_full = true;
            CHECK(tiny_display_gallery_nav(+1) == ESP_FAIL &&
                      last_url() == "https://h/a2.raw" && fetches() == f0 + 1,
                  "gallery: failed flip to a2 is returned");
            f0 = fetches();
            CHECK(tiny_display_gallery_nav(+1) == ESP_OK &&
                      last_url() == "https://h/a2.raw" && fetches() == f0 + 1,
                  "gallery: failed flip neither advanced page nor cached a2");

            // Held new-set fetch overtaken by a newer card: set A, page a2,
            // and its cached frame survive; the stale set is discarded.
            {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_release_http = false;
                g_http_blocked = false;
            }
            std::atomic<int> gal_result{ESP_OK};
            std::thread gal([&] {
                gal_result = tiny_display_render_card(
                    R"J({"type":"gallery","urls":["https://h/c0.raw","https://h/c1.raw"],"index":1})J");
            });
            {
                std::unique_lock<std::mutex> lock(g_http_mutex);
                CHECK(g_http_ready.wait_for(lock, std::chrono::seconds(2),
                                            [] { return g_http_blocked; }),
                      "gallery race: new-set fetch reaches held HTTPS");
            }
            CHECK(tiny_display_gallery_count() == 3,
                  "gallery race: held fetch has not replaced the set");
            CHECK(tiny_display_render_card(
                      R"J({"type":"text","card_id":"gal-overtake","title":"newest","body":"wins"})J") == ESP_OK,
                  "gallery race: newer card overtakes held gallery fetch");
            {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_release_http = true;
                g_http_ready.notify_all();
            }
            gal.join();
            CHECK(gal_result == ESP_ERR_INVALID_STATE,
                  "gallery race: stale gallery response is refused");
            CHECK(!strcmp(tiny_display_current_card_id(), "gal-overtake"),
                  "gallery race: stale gallery cannot overwrite glass");
            CHECK(tiny_display_gallery_count() == 3,
                  "gallery race: stale set never became the gallery");
            f0 = fetches();
            CHECK(tiny_display_gallery_nav(0) == ESP_OK && fetches() == f0 &&
                      !strcmp(tiny_display_current_card_id(), "gallery"),
                  "gallery race: reopen shows cached a2 with no fetch");
            CHECK(tiny_display_gallery_nav(-1) == ESP_OK && fetches() == f0,
                  "gallery race: prev pages set A (cached a1), not set C");

            // Panel-native gallery commits own no text scroll window: both a
            // gallery card and a page flip over a long text card must clear
            // every stale scroll claim once their frame is on glass.
            auto render_long_text = [](const char *cid) {
                std::string card = std::string(R"J({"type":"text","card_id":")J") +
                                   cid + R"J(","title":"long","body":")J";
                for (int i = 0; i < 80; ++i) card += "overflowing line\\n";
                card += R"J("})J";
                return tiny_display_render_card(card.c_str()) == ESP_OK &&
                       tiny_display_can_scroll();
            };
            auto scroll_cleared = [] {
                int off = -1, content = -1, view = -1;
                tiny_display_scroll_state(&off, &content, &view);
                return !tiny_display_can_scroll() && off == 0 && content == 0 &&
                       view == 0 &&
                       tiny_display_scroll(40) == ESP_ERR_INVALID_STATE;
            };
            CHECK(render_long_text("long-before-gallery") &&
                      tiny_display_scroll(40) == ESP_OK,
                  "gallery scroll: long text scrolls");
            CHECK(tiny_display_render_card(gal_a) == ESP_OK && scroll_cleared(),
                  "gallery scroll: gallery card clears stale scroll geometry");
            CHECK(render_long_text("long-before-flip"),
                  "gallery scroll: long text again");
            CHECK(tiny_display_gallery_nav(0) == ESP_OK && scroll_cleared(),
                  "gallery scroll: page flip clears stale scroll geometry");
            {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_hold_http = false;
                g_release_http = false;
                g_http_body.clear();
            }

            // ---- image identity ---------------------------------------------
            // A painted image publishes the card_id it was SENT with (the
            // stream player sends "stream"); "image" only when none is named.
            const size_t gray4 = static_cast<size_t>(sz.w) * sz.h / 4;
            const size_t mono = static_cast<size_t>(sz.w) * sz.h / 8;
            auto serve_http = [&](size_t bytes, bool release) {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_http_body.assign(bytes, 0x55);
                g_hold_http = true;
                g_http_blocked = false;
                g_release_http = release;
            };
            auto stop_http = [&] {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_hold_http = false;
                g_release_http = false;
                g_http_body.clear();
            };
            auto wait_http_blocked = [&] {
                std::unique_lock<std::mutex> lock(g_http_mutex);
                return g_http_ready.wait_for(lock, std::chrono::seconds(2),
                                             [] { return g_http_blocked; });
            };
            auto release_http = [&] {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_release_http = true;
                g_http_ready.notify_all();
            };
            // Panel-native commits own no card-layout scroll window. Start
            // with a genuinely overflowing text card, then prove an image
            // clears every stale scroll claim when it reaches the glass.
            std::string long_card =
                R"J({"type":"text","card_id":"long-before-image","title":"long","body":")J";
            for (int i = 0; i < 80; ++i) long_card += "overflowing line\\n";
            long_card += R"J("})J";
            CHECK(tiny_display_render_card(long_card.c_str()) == ESP_OK &&
                      tiny_display_can_scroll(),
                  "panel native: long text establishes scroll state");
            CHECK(tiny_display_set_rotation(90) == ESP_OK,
                  "panel native: policy angle is portrait before image");
            serve_http(gray4, true);
            CHECK(tiny_display_render_card(
                      R"J({"type":"image","card_id":"stream","url":"https://h/id.raw"})J") == ESP_OK &&
                      !strcmp(tiny_display_current_card_id(), "stream"),
                  "image identity: requested card_id is published on success");
            CHECK(tiny_display_rotation() == 90 &&
                      sticky_display_canvas()->rotation() == CanvasRotation::Landscape,
                  "panel native: image keeps policy angle but commits landscape frame");
            const uint32_t image_frame_hash = frame_hash();
            g_fail_alloc_size = g_buf.size();
            CHECK(tiny_display_render_card(
                      R"J({"type":"text","card_id":"snapshot-oom","title":"x","body":"y"})J") == ESP_ERR_NO_MEM &&
                      !strcmp(tiny_display_current_card_id(), "stream") &&
                      frame_hash() == image_frame_hash,
                  "panel native: rollback-snapshot OOM refuses before drawing");
            g_fail_alloc_size = 0;
            g_fail_next_full = true;
            CHECK(tiny_display_render_card(
                      R"J({"type":"text","card_id":"failed-over-image","title":"x","body":"y"})J") == ESP_FAIL &&
                      !strcmp(tiny_display_current_card_id(), "stream") &&
                      tiny_display_rotation() == 90 &&
                      sticky_display_canvas()->rotation() == CanvasRotation::Landscape &&
                      frame_hash() == image_frame_hash,
                  "panel native: failed drawn card restores image bytes and touch frame");
            // Gallery publishes native-space tap regions without running the
            // generic layout. Its visible ack therefore catches stale s_w/s_h:
            // correct landscape dimensions repaint this bottom-left interior
            // white; the failed portrait candidate would repaint 80 px above.
            CHECK(tiny_display_gallery_nav(0) == ESP_OK && px(31, 390) == 0,
                  "panel native: gallery probe starts outside button paint");
            const esp_err_t ack_err = tiny_display_flash_button(0);
            const int ack_px = px(31, 390);
            CHECK(ack_err == ESP_OK && ack_px == 3,
                  "panel native: failed card restores live ack dimensions (px=%d)",
                  ack_px);
            int native_offset = -1, native_content = -1, native_view = -1;
            tiny_display_scroll_state(&native_offset, &native_content, &native_view);
            CHECK(!tiny_display_can_scroll() && native_offset == 0 &&
                      native_content == 0 && native_view == 0,
                  "panel native: image clears stale scroll geometry");
            CHECK(tiny_display_render_card(
                      R"J({"type":"image","url":"https://h/id2.raw"})J") == ESP_OK &&
                      !strcmp(tiny_display_current_card_id(), "image"),
                  "image identity: unnamed image falls back to \"image\"");
            stop_http();

            // ---- replacement during a stream is a glass transaction ---------
            // A replacing card that never lands (malformed, failed refresh,
            // failed fetch) leaves the stream frame on glass, so it must
            // leave the stream alive with its words; one that lands ends it.
            CHECK(tiny_display_set_rotation(0) == ESP_OK,
                  "stream replace: start at rotation 0");
            CHECK(tiny_display_stream_begin("tiny", "ansR", nullptr) == ESP_OK,
                  "stream replace: begin");
            tiny_display_stream_append("kept words");
            CHECK(tiny_display_render_card("{\"type\":\"text\",") == ESP_ERR_INVALID_ARG,
                  "stream replace: malformed outsider refused");
            CHECK(tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "ansR"),
                  "stream replace: malformed outsider leaves stream live");
            g_fail_next_full = true;
            CHECK(tiny_display_render_card(
                      R"J({"type":"text","card_id":"rep-fail","title":"x","body":"y"})J") == ESP_FAIL,
                  "stream replace: failed outsider refresh returned");
            CHECK(tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "ansR"),
                  "stream replace: failed outsider commit leaves stream live");
            CHECK(tiny_display_render_card(
                      R"J({"type":"image","card_id":"rep-img","url":"https://h/none.raw"})J") != ESP_OK,
                  "stream replace: failed outsider fetch returned");
            CHECK(tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "ansR"),
                  "stream replace: failed outsider fetch leaves stream live");
            // The live stream still owns rotation: its words, not a frame.
            CHECK(tiny_display_set_rotation(90) == ESP_OK &&
                      tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "ansR"),
                  "stream replace: surviving stream redraws on rotation");
            CHECK(tiny_display_render_card(
                      R"J({"type":"text","card_id":"rep-ok","title":"x","body":"y"})J") == ESP_OK,
                  "stream replace: landing outsider commits");
            CHECK(!tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "rep-ok"),
                  "stream replace: landed outsider ends stream");

            // Owner ends the answer while an outsider is still fetching. The
            // end defers behind it; when the outsider then fails at the panel
            // the stream's closing frame is painted and the stream is over.
            CHECK(tiny_display_stream_begin("tiny", "ansD", nullptr) == ESP_OK,
                  "stream defer: begin");
            tiny_display_stream_append("answer");
            serve_http(mono, false);
            std::atomic<int> dimg{ESP_OK};
            std::thread dthr([&] {
                dimg = tiny_display_render_card(
                    R"J({"type":"image","card_id":"d-img","url":"https://h/d.raw"})J");
            });
            CHECK(wait_http_blocked(), "stream defer: outsider fetch held");
            CHECK(tiny_display_stream_end(nullptr) == ESP_OK,
                  "stream defer: owner end returns while outsider in flight");
            CHECK(tiny_display_stream_active(),
                  "stream defer: stream still owns glass during outsider fetch");
            g_fail_next_full = true;
            release_http();
            dthr.join();
            stop_http();
            CHECK(dimg == ESP_FAIL, "stream defer: outsider panel failure returned");
            CHECK(!tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "ansD"),
                  "stream defer: closing frame painted after outsider fails");

            // ---- cache allocation failure -----------------------------------
            // The new card lands but no replay copy could be made: replay is
            // CLEARED, so rotation cannot resurrect the previous card.
            const char *oom_a =
                R"J({"type":"text","card_id":"oom-a","title":"a","body":"first"})J";
            const char *oom_b =
                R"J({"type":"text","card_id":"oom-b","title":"b","body":"second card, cache-less"})J";
            CHECK(tiny_display_render_card(oom_a) == ESP_OK,
                  "cache oom: previous card cached");
            g_fail_alloc_size = strlen(oom_b) + 1;
            CHECK(tiny_display_render_card(oom_b) == ESP_OK &&
                      !strcmp(tiny_display_current_card_id(), "oom-b"),
                  "cache oom: new card commits without a replay copy");
            g_fail_alloc_size = 0;
            CHECK(tiny_display_rerender() == ESP_ERR_INVALID_STATE &&
                      !strcmp(tiny_display_current_card_id(), "oom-b"),
                  "cache oom: rerender refuses instead of resurrecting oom-a");
            CHECK(tiny_display_set_rotation(0) == ESP_OK &&
                      strcmp(tiny_display_current_card_id(), "oom-a") != 0,
                  "cache oom: rotation never replays the previous card");
            CHECK(!tiny_display_can_scroll(),
                  "cache oom: no replay means no scroll over stale spec");

            // ---- raw blit is a display transaction -------------------------
            // A raw frame is newer than an in-flight media fetch and must
            // invalidate that fetch's epoch. It also replaces a text stream
            // only after its own panel refresh succeeds.
            std::vector<uint8_t> raw_blit(mono, 0x00);
            serve_http(gray4, false);
            std::atomic<int> before_blit{ESP_OK};
            std::thread old_media([&] {
                before_blit = tiny_display_render_card(
                    R"J({"type":"image","card_id":"before-blit","url":"https://h/old.raw"})J");
            });
            CHECK(wait_http_blocked(), "blit epoch: older image fetch held");
            CHECK(tiny_display_blit_1bit(raw_blit.data(), raw_blit.size(), false) == ESP_OK &&
                      !strcmp(tiny_display_current_card_id(), "blit"),
                  "blit epoch: newer raw frame commits");
            release_http();
            old_media.join();
            stop_http();
            CHECK(before_blit == ESP_ERR_INVALID_STATE &&
                      !strcmp(tiny_display_current_card_id(), "blit"),
                  "blit epoch: older media response cannot overwrite raw frame");

            CHECK(tiny_display_stream_begin("tiny", "ansB", nullptr) == ESP_OK,
                  "blit stream: begin");
            tiny_display_stream_append("raw replacement waits for glass");
            g_fail_next_full = true;
            CHECK(tiny_display_blit_1bit(raw_blit.data(), raw_blit.size(), false) == ESP_FAIL &&
                      tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "ansB"),
                  "blit stream: failed raw refresh keeps text stream live");
            CHECK(tiny_display_blit_1bit(raw_blit.data(), raw_blit.size(), false) == ESP_OK &&
                      !tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "blit"),
                  "blit stream: committed raw frame retires text stream");
            {
                std::string card =
                    R"J({"type":"text","card_id":"long-before-blit","title":"long","body":")J";
                for (int i = 0; i < 80; ++i) card += "overflowing line\\n";
                card += R"J("})J";
                CHECK(tiny_display_render_card(card.c_str()) == ESP_OK &&
                          tiny_display_can_scroll(),
                      "blit scroll: long text scrolls");
                CHECK(tiny_display_blit_1bit(raw_blit.data(), raw_blit.size(), false) == ESP_OK,
                      "blit scroll: raw frame commits");
                int off = -1, content = -1, view = -1;
                tiny_display_scroll_state(&off, &content, &view);
                CHECK(!tiny_display_can_scroll() && off == 0 && content == 0 &&
                          view == 0 &&
                          tiny_display_scroll(40) == ESP_ERR_INVALID_STATE,
                      "blit scroll: raw frame clears stale scroll geometry");
            }

            // ---- raw blit commits card TYPE with identity -------------------
            // Gesture policy follows what is on glass. After a keyboard, a
            // committed raw frame must route swipes as an ordinary card
            // (home / back / page flip); a FAILED raw frame leaves the
            // keyboard on glass, so keyboard policy must survive it.
            {
                struct Routed { std::string key; int home, back, cycle; esp_err_t r; };
                // Swipe in the canvas's CURRENT logical frame, delivered in
                // panel coordinates exactly like the touch task does.
                auto swipe = [&](double fx0, double fy0, double fx1, double fy1) {
                    Canvas *cv = sticky_display_canvas();
                    const int W = cv->width(), H = cv->height();
                    int px0, py0, px1, py1;
                    cv->to_physical((int)(fx0 * W), (int)(fy0 * H), px0, py0);
                    cv->to_physical((int)(fx1 * W), (int)(fy1 * H), px1, py1);
                    g_shell_last_key.clear();
                    const int h0 = g_shell_home_calls, b0 = g_shell_back_calls,
                              c0 = g_shell_cycle_calls;
                    const esp_err_t r = tiny_display_scroll_gesture(px0, py0, px1, py1);
                    return Routed{g_shell_last_key, g_shell_home_calls - h0,
                                  g_shell_back_calls - b0,
                                  g_shell_cycle_calls - c0, r};
                };
                // bottom band up, left band right, mid-content leftward flip
                auto bottom_up = [&] { return swipe(0.5, 0.98, 0.5, 0.40); };
                auto left_back = [&] { return swipe(0.02, 0.5, 0.70, 0.5); };
                auto page_flip = [&] { return swipe(0.75, 0.5, 0.25, 0.5); };
                const char *kb_card =
                    R"J({"type":"keyboard","card_id":"kb-blit","title":"pw","value":"abc"})J";
                auto expect_keyboard_policy = [&](const char *tag) {
                    Routed u = bottom_up();
                    CHECK(u.key == "k:cancel" && u.home == 0,
                          "%s: bottom-up is keyboard cancel (key=%s home=%d)", tag,
                          u.key.c_str(), u.home);
                    Routed b = left_back();
                    CHECK(b.r == ESP_ERR_NOT_ALLOWED && b.back == 0,
                          "%s: left-band back refused over draft (r=%d back=%d)",
                          tag, b.r, b.back);
                    Routed p = page_flip();
                    CHECK(p.r == ESP_ERR_NOT_SUPPORTED && p.cycle == 0,
                          "%s: page flip silenced over draft (r=%d cycle=%d)",
                          tag, p.r, p.cycle);
                };
                CHECK(tiny_display_render_card(kb_card) == ESP_OK,
                      "kb->blit: keyboard commits");
                // Rotated glass: a failed raw frame (pinned landscape while
                // drawing) must hand the keyboard's touch frame back.
                CHECK(tiny_display_set_rotation(90) == ESP_OK &&
                          tiny_display_rotation() == 90 &&
                          !strcmp(tiny_display_current_card_id(), "kb-blit"),
                      "kb->blit: keyboard re-renders at 90 deg");
                expect_keyboard_policy("kb->blit baseline");

                g_fail_next_full = true;
                CHECK(tiny_display_blit_1bit(raw_blit.data(), raw_blit.size(), false) == ESP_FAIL &&
                          !strcmp(tiny_display_current_card_id(), "kb-blit"),
                      "kb->blit: failed raw frame keeps keyboard on glass");
                // Same logical frame as the keyboard: the failed blit must
                // hand the canvas rotation back (no test-side fix-up).
                CHECK(static_cast<int>(sticky_display_canvas()->rotation()) * 90 ==
                          tiny_display_rotation(),
                      "kb->blit: failed raw frame restores the touch frame");
                expect_keyboard_policy("kb->failed blit");

                CHECK(tiny_display_blit_1bit(raw_blit.data(), raw_blit.size(), false) == ESP_OK &&
                          !strcmp(tiny_display_current_card_id(), "blit"),
                      "kb->blit: raw frame commits");
                Routed u = bottom_up();
                CHECK(u.key.empty() && u.home == 1 && u.r == ESP_OK,
                      "kb->blit: bottom-up goes home, not keyboard cancel (key=%s home=%d)",
                      u.key.c_str(), u.home);
                Routed b = left_back();
                CHECK(b.r == ESP_OK && b.back == 1,
                      "kb->blit: left-band back navigates (r=%d back=%d)", b.r, b.back);
                Routed p = page_flip();
                CHECK(p.r == ESP_OK && p.cycle == 1,
                      "kb->blit: horizontal swipe flips the page (r=%d cycle=%d)",
                      p.r, p.cycle);
                CHECK(tiny_display_set_rotation(0) == ESP_OK,
                      "kb->blit: rotation back to 0 for later cases");
            }

            // ---- structured stream finale over HTTPS ------------------------
            // Successful image finale: requested identity, stream retired.
            CHECK(tiny_display_stream_begin("tiny", "ansF", nullptr) == ESP_OK,
                  "finale: begin");
            serve_http(gray4, true);
            CHECK(tiny_display_stream_end(
                      R"J({"type":"image","card_id":"fin-img","url":"https://h/f.raw"})J") == ESP_OK,
                  "finale: image finale paints");
            CHECK(!tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "fin-img"),
                  "finale: image finale owns glass with its card_id");
            stop_http();

            // Held finale fetch: stream_end holds NO render ownership across
            // HTTPS, so a newer card commits meanwhile; the stale finale is
            // then refused and cannot overwrite it.
            CHECK(tiny_display_stream_begin("tiny", "ansH", nullptr) == ESP_OK,
                  "finale race: begin");
            tiny_display_stream_append("words before the card");
            serve_http(gray4, false);
            std::atomic<int> fin{ESP_OK};
            std::thread fthr([&] {
                fin = tiny_display_stream_end(
                    R"J({"type":"image","card_id":"fin-stale","url":"https://h/s.raw"})J");
            });
            CHECK(wait_http_blocked(), "finale race: final image fetch held");
            std::atomic<int> newer_fin{ESP_FAIL};
            std::atomic<bool> newer_done{false};
            std::thread nthr([&] {
                newer_fin = tiny_display_render_card(
                    R"J({"type":"text","card_id":"after-fin","title":"n","body":"newest"})J");
                newer_done = true;
            });
            for (int i = 0; i < 200 && !newer_done.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(newer_done.load() && newer_fin == ESP_OK,
                  "finale race: newer card commits during held finale fetch");
            CHECK(!strcmp(tiny_display_current_card_id(), "after-fin") &&
                      !tiny_display_stream_active(),
                  "finale race: newer card owns glass, stream retired");
            release_http();
            fthr.join();
            nthr.join();
            stop_http();
            CHECK(fin == ESP_ERR_INVALID_STATE,
                  "finale race: stale finale refused");
            CHECK(!strcmp(tiny_display_current_card_id(), "after-fin") &&
                      tiny_display_rerender() == ESP_OK &&
                      !strcmp(tiny_display_current_card_id(), "after-fin"),
                  "finale race: stale finale overwrote neither glass nor replay");

            // Malformed finale still ends the answer (as its text frame).
            CHECK(tiny_display_stream_begin("tiny", "ansX", nullptr) == ESP_OK,
                  "finale malformed: begin");
            CHECK(tiny_display_stream_end("{\"type\":") == ESP_ERR_INVALID_ARG,
                  "finale malformed: refusal returned");
            CHECK(!tiny_display_stream_active() &&
                      !strcmp(tiny_display_current_card_id(), "ansX"),
                  "finale malformed: closing text frame painted, stream over");
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
