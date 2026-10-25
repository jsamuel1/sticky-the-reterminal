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
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"   // render_test_recursive_held
#include "canvas.h"
#include "lcd_palette.h"
#include "tiny/tiny_display.h"
#include "tiny/tiny_rotation.h"
#include "tiny/tiny_universe_card.h"
#include <sys/stat.h>

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

// Rendezvous seams (TINY_DISPLAY_SEAM in tiny_display.cpp): a test arms one
// hook for one named point; it runs once there.
static std::mutex g_seam_mutex;
static std::string g_seam_point;
static std::function<void()> g_seam_hook;
void render_shim_seam(const char *point) {
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(g_seam_mutex);
        if (!g_seam_hook || g_seam_point != point) return;
        hook.swap(g_seam_hook);
    }
    hook();
}
static void arm_seam(const char *point, std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(g_seam_mutex);
    g_seam_point = point;
    g_seam_hook = std::move(hook);
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

static int g_gray_refreshes = 0;
Canvas *sticky_display_canvas() { return g_canvas; }
esp_err_t sticky_display_init() { return 0; }
esp_err_t sticky_display_refresh() { ++g_gray_refreshes; return 0; }
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
// SD card: absent unless a case inserts it; then the stores write under the
// scratch STICKY_SD_MOUNT_POINT (render_shim.h, emptied by the harness).
static bool g_sd_present = false;
extern "C" esp_err_t sticky_sdcard_ensure(void) {
    if (!g_sd_present) return ESP_ERR_NOT_FOUND;
    mkdir(STICKY_SD_MOUNT_POINT, 0775);
    return ESP_OK;
}
extern "C" int tiny_askq_count(void) { return 0; }
static char g_agent[TINY_AGENT_SLUG_MAX + 1] = "";
extern "C" const char *tiny_agent_current(void) { return g_agent; }
// Roster as the universe push commits it (tiny_agent's own rules and its NVS
// atomicity are agent_host_test.sh's business; here: what the page lists,
// and what the renderer does when the one roster write is refused).
static std::vector<std::string> g_roster = {"tiny"};
extern "C" int tiny_agent_roster(char out[][TINY_AGENT_SLUG_MAX + 1], int max) {
    int n = 0;
    for (const auto &r : g_roster) {
        if (n >= max) break;
        strlcpy(out[n++], r.c_str(), TINY_AGENT_SLUG_MAX + 1);
    }
    return n;
}
extern "C" void tiny_agent_roster_snapshot(tiny_agent_roster_t *out) {
    memset(out, 0, sizeof *out);
    strlcpy(out->active, g_agent, sizeof out->active);
    out->n = tiny_agent_roster(out->slugs, TINY_AGENT_ROSTER_MAX);
}
// Preview = the stub's add rule on a copy (tiny_agent's own preview/add
// equivalence is agent_host_test.sh's business).
extern "C" esp_err_t tiny_agent_roster_preview_add(tiny_agent_roster_t *r,
                                                   const char *slug) {
    for (int i = 0; i < r->n; ++i) if (!strcmp(r->slugs[i], slug)) return ESP_OK;
    if (r->n < TINY_AGENT_ROSTER_MAX)
        strlcpy(r->slugs[r->n++], slug, TINY_AGENT_SLUG_MAX + 1);
    return ESP_OK;
}
// The one roster write a universe commit makes. g_roster_commit_fail makes
// the next one return that error (an NVS refusal) and change nothing; the
// base check is the real one's compare-and-set.
static int g_roster_commits = 0;
static esp_err_t g_roster_commit_fail = ESP_OK;
extern "C" esp_err_t tiny_agent_roster_commit(const tiny_agent_roster_t *base,
                                              const tiny_agent_roster_t *next) {
    ++g_roster_commits;
    if (g_roster_commit_fail != ESP_OK) {
        const esp_err_t e = g_roster_commit_fail;
        g_roster_commit_fail = ESP_OK;
        return e;
    }
    tiny_agent_roster_t live;
    tiny_agent_roster_snapshot(&live);
    bool same = live.n == base->n && !strcmp(live.active, base->active);
    for (int i = 0; same && i < live.n; ++i)
        same = !strcmp(live.slugs[i], base->slugs[i]);
    if (!same) return ESP_ERR_INVALID_STATE;
    g_roster.assign(next->slugs, next->slugs + next->n);
    return ESP_OK;
}

// cJSON allocation failure injection (cJSON_InitHooks): while armed, the
// g_cjson_fail_at-th cJSON allocation after arming returns NULL. cJSON is
// the device's own allocator path for every parse, build and print.
static std::atomic<long> g_cjson_allocs{0};
static std::atomic<long> g_cjson_fail_at{0};
static std::atomic<bool> g_cjson_failed{false};
static void *cjson_test_malloc(size_t n) {
    const long k = ++g_cjson_allocs;
    if (g_cjson_fail_at > 0 && k == g_cjson_fail_at) {
        g_cjson_failed = true;
        return nullptr;
    }
    return malloc(n);
}
static void cjson_test_free(void *p) { free(p); }
static void cjson_fail_arm(long at) {
    g_cjson_allocs = 0;
    g_cjson_failed = false;
    g_cjson_fail_at = at;
    cJSON_Hooks h{cjson_test_malloc, cjson_test_free};
    cJSON_InitHooks(&h);
}
static void cjson_fail_disarm() {
    g_cjson_fail_at = 0;
    cJSON_InitHooks(nullptr);
}
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

        if (!strcmp(sz.tag, "fnk0104s")) {
            // ---- manual rotation (v25): the board with no accelerometer ----
            // The settings picker is the shipped tiny_rotation_picker_json;
            // a selection goes through the shipped tiny_rotation_select with
            // the REAL tiny_display_set_rotation as its apply step, exactly
            // as tiny_shell's rotation_route wires it.
            std::vector<int> saved;
            static std::vector<int> *s_saved;
            s_saved = &saved;
            auto save = [](uint8_t q) -> int { s_saved->push_back(q); return 0; };
            auto apply = [](int deg) -> int { return tiny_display_set_rotation(deg); };
            auto render_picker = [&](int cur) {
                char card[1024];
                CHECK(tiny_rotation_picker_json(card, sizeof card, cur, true) > 0,
                      "manual rotation: picker fits");
                return tiny_display_render_card(card);
            };
            CHECK(tiny_rotation_manual_offered(false), "fnk has no IMU: picker offered");
            CHECK(tiny_display_rotation() == 0 && render_picker(0) == ESP_OK,
                  "manual rotation: picker renders at 0 deg");
            for (int deg : {90, 180, 270, 0}) {
                bool persisted = false;
                saved.clear();
                const esp_err_t r = (esp_err_t)tiny_rotation_select(
                    deg, false, apply, save, &persisted);
                CHECK(r == ESP_OK && persisted && saved == std::vector<int>{deg / 90},
                      "manual rotation %d: applied then saved", deg);
                // set_rotation replayed the picker in the new frame; the shell
                // then repaints it so ACTIVE names the new angle.
                CHECK(render_picker(deg) == ESP_OK, "manual rotation %d: repaint", deg);
                Canvas *cv = sticky_display_canvas();
                CHECK(tiny_display_rotation() == deg &&
                          static_cast<int>(cv->rotation()) * 90 == deg &&
                          !strcmp(tiny_display_current_card_id(), "rotation"),
                      "manual rotation %d: glass, touch frame and card agree", deg);
                const bool quarter = deg == 90 || deg == 270;
                CHECK(cv->width() == (quarter ? 480 : 320) &&
                          cv->height() == (quarter ? 320 : 480),
                      "manual rotation %d: logical layout %dx%d", deg,
                      cv->width(), cv->height());
                CHECK(edge_ink(4) == 0, "manual rotation %d: ink in the edge band", deg);
                // Every tap region sits on the panel, over drawn ink, and maps
                // back into the layout where the row/button was drawn.
                tiny_button_region_t regs[48];
                const int n = tiny_display_button_regions(regs, 48);
                int rows = 0, back_ok = 0;
                for (int i = 0; i < n; ++i) {
                    const auto &q = regs[i];
                    CHECK(q.x >= 0 && q.y >= 0 && q.x + q.w <= sz.w && q.y + q.h <= sz.h,
                          "manual rotation %d: region %s leaves the panel", deg, q.id);
                    int ink = 0;
                    for (int y = q.y; y < q.y + q.h; ++y)
                        for (int x = q.x; x < q.x + q.w; ++x) ink += px(x, y) != 3;
                    CHECK(ink > 0, "manual rotation %d: region %s over blank glass", deg, q.id);
                    int lx, ly;
                    cv->to_logical(q.x + q.w / 2, q.y + q.h / 2, lx, ly);
                    int d = -1;
                    if (tiny_rotation_parse_id(q.id, &d) == TINY_ROTATION_ID_SET) {
                        ++rows;
                        CHECK(ly < cv->height() - 60,
                              "manual rotation %d: row %s is in the menu, not the bar",
                              deg, q.id);
                    }
                    if (!strcmp(q.id, "rot:back") && ly > cv->height() - 70 &&
                        lx < cv->width() / 2)
                        ++back_ok;  // the button bar is at the LAYOUT's bottom
                }
                CHECK(rows >= 2, "manual rotation %d: %d orientation rows on glass", deg, rows);
                CHECK(back_ok == 1,
                      "manual rotation %d: Back sits bottom-left of the turned layout", deg);
                // Short landscape glass shows two rows; the menu scrolls like
                // the Wi-Fi list. Every orientation must be reachable.
                {
                    std::set<int> reach;
                    for (int step = 0; step < 6; ++step) {
                        tiny_button_region_t rr[48];
                        const int m = tiny_display_button_regions(rr, 48);
                        for (int i = 0; i < m; ++i) {
                            int d = -1;
                            if (tiny_rotation_parse_id(rr[i].id, &d) == TINY_ROTATION_ID_SET)
                                reach.insert(d);
                        }
                        if (tiny_display_scroll_page(+1) != ESP_OK) break;
                    }
                    CHECK(reach.size() == 4, "manual rotation %d: %zu of 4 orientations "
                          "reachable by scrolling", deg, reach.size());
                    tiny_display_scroll(-(1 << 28));
                }
                // Gesture frame turns with the picture: "up from the bottom
                // band" of the layout is home, whatever edge that is on glass.
                int px0, py0, px1, py1;
                cv->to_physical(cv->width() / 2, cv->height() - 6, px0, py0);
                cv->to_physical(cv->width() / 2, cv->height() / 3, px1, py1);
                const int h0 = g_shell_home_calls;
                CHECK(tiny_display_scroll_gesture(px0, py0, px1, py1) == ESP_OK &&
                          g_shell_home_calls == h0 + 1,
                      "manual rotation %d: bottom-band up goes home", deg);
                if (out_dir) {
                    uint8_t *idx = nullptr; size_t len = 0; bool acc = false;
                    if (tiny_display_snapshot_index8(&idx, &len, &acc) == 0) {
                        std::vector<uint8_t> rgb(len * 3);
                        for (size_t i = 0; i < len; ++i) {
                            const lcd_palette::Rgb c = lcd_palette::color(
                                lcd_palette::Theme::Light, idx[i] >> 2, idx[i] & 3);
                            rgb[3 * i] = c.r; rgb[3 * i + 1] = c.g; rgb[3 * i + 2] = c.b;
                        }
                        char path[512];
                        snprintf(path, sizeof path, "%s/fnk0104s-rotation-%d.png",
                                 out_dir, deg);
                        write_png(path, sz.w, sz.h, rgb.data());
                        free(idx);
                    }
                }
            }
            // A failed panel commit: the glass still shows the old angle, so
            // nothing is saved and the reported angle stays (retry works).
            {
                CHECK(tiny_rotation_select(90, false, apply, save, nullptr) == ESP_OK,
                      "manual rotation: start at 90");
                saved.clear();
                bool persisted = true;
                g_fail_next_full = true;
                CHECK(tiny_rotation_select(180, false, apply, save, &persisted) == ESP_FAIL,
                      "manual rotation: injected refresh failure returned");
                CHECK(saved.empty() && !persisted && tiny_display_rotation() == 90,
                      "manual rotation: failed commit neither saves nor reports 180");
                CHECK(tiny_rotation_select(180, false, apply, save, &persisted) == ESP_OK &&
                          persisted && saved == std::vector<int>{2} &&
                          tiny_display_rotation() == 180,
                      "manual rotation: same angle retries and commits");
            }
            // An IMU board is refused before anything moves.
            {
                saved.clear();
                const int before = tiny_display_rotation(), refreshes = g_full_refreshes;
                CHECK(tiny_rotation_select(270, true, apply, save, nullptr) == ESP_ERR_NOT_SUPPORTED &&
                          saved.empty() && tiny_display_rotation() == before &&
                          g_full_refreshes == refreshes,
                      "manual rotation: refused on an accelerometer board, glass untouched");
            }
            // Reboot: the saved angle is seeded before anything is drawn, with
            // no refresh of its own; the first card comes up in that frame.
            {
                CHECK(tiny_display_set_rotation(0) == ESP_OK, "manual rotation: reset");
                tiny_display_init();
                const int refreshes = g_full_refreshes;
                CHECK(tiny_display_preset_rotation(tiny_rotation_boot_deg(true, 3)) == ESP_OK &&
                          g_full_refreshes == refreshes && tiny_display_rotation() == 270 &&
                          static_cast<int>(sticky_display_canvas()->rotation()) == 3,
                      "boot preset: 270 seeded, nothing pushed");
                CHECK(tiny_display_splash("t", "t") == ESP_OK && edge_ink(4) == 0,
                      "boot preset: splash drawn in the saved frame");
                CHECK(render_picker(270) == ESP_OK && sticky_display_canvas()->width() == 480,
                      "boot preset: first card lays out landscape");
                CHECK(tiny_display_preset_rotation(tiny_rotation_boot_deg(true, 9)) == ESP_OK &&
                          tiny_display_rotation() == 0,
                      "boot preset: invalid saved value boots 0 deg");
                CHECK(tiny_display_preset_rotation(45) == ESP_ERR_INVALID_ARG &&
                          tiny_display_rotation() == 0,
                      "boot preset: a non-quarter angle is refused");
            }
            CHECK(tiny_display_set_rotation(0) == ESP_OK, "manual rotation: back to 0");

            // ---- status light vs image / gallery / universe -----------------
            // Those three leave the text pipeline before its title prefix, so
            // priority must still reach their commits, survive the device's own
            // repaints of the same page, clear on the next other commit, and a
            // FAILED commit must change nothing. 1-bit frames take the
            // monochrome refresh, where g_fail_next_full can refuse them.
            const size_t mono = static_cast<size_t>(sz.w) * sz.h / 8;
            auto serve = [&](uint8_t fill) {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_http_body.assign(mono, fill);
                g_hold_http = true;
                g_http_blocked = false;
                g_release_http = true;
            };
            auto unserve = [&] {
                std::lock_guard<std::mutex> lock(g_http_mutex);
                g_hold_http = false;
                g_release_http = false;
                g_http_body.clear();
            };
            int calls = g_led_alert_calls;
            auto expect = [&](const char *what, esp_err_t r, esp_err_t want_r,
                              int commits, bool lit) {
                CHECK(r == want_r, "led/%s: returned %d, want %d", what, r, want_r);
                CHECK(g_led_alert_calls == calls + commits,
                      "led/%s: %d alert-layer updates, want %d", what,
                      g_led_alert_calls - calls, commits);
                CHECK(g_led_alert == lit, "led/%s: alert %d, want %d", what,
                      g_led_alert, lit);
                calls = g_led_alert_calls;
            };
            const char *normal =
                R"J({"type":"text","card_id":"n1","title":"plain","body":"nothing urgent"})J";
            auto repaint_universe = [] {   // what tiny_shell's row tap renders
                char roster[TINY_AGENT_ROSTER_MAX][TINY_AGENT_SLUG_MAX + 1];
                const int n = tiny_agent_roster(roster, TINY_AGENT_ROSTER_MAX);
                char *cs = tiny_universe_card_json(g_agent, roster, n, nullptr);
                const esp_err_t r = tiny_display_render_universe_page(cs);
                free(cs);
                return r;
            };
            serve(0xF0);
            expect("image alert", tiny_display_render_card(
                R"J({"type":"image","card_id":"img","url":"https://x/a.raw","priority":"alert"})J"),
                ESP_OK, 1, true);
            // Panel-native replay keeps the pixels on glass: no commit at all.
            expect("image alert rerender", tiny_display_rerender(), ESP_OK, 0, true);
            g_fail_next_full = true;
            expect("image normal, panel fails", tiny_display_render_card(
                R"J({"type":"image","card_id":"img2","url":"https://x/b.raw"})J"),
                ESP_FAIL, 0, true);
            unserve();
            CHECK(tiny_display_render_card(
                      R"J({"type":"image","card_id":"img2","url":"https://x/b.raw"})J") != ESP_OK &&
                      g_led_alert_calls == calls && g_led_alert,
                  "led/image normal, fetch fails: light unchanged");
            serve(0xF0);
            expect("image normal", tiny_display_render_card(
                R"J({"type":"image","card_id":"img2","url":"https://x/b.raw"})J"),
                ESP_OK, 1, false);

            expect("gallery alert", tiny_display_render_card(
                R"J({"type":"gallery","urls":["https://x/1.raw","https://x/2.raw"],"priority":"alert"})J"),
                ESP_OK, 1, true);
            expect("gallery alert next page", tiny_display_gallery_nav(+1), ESP_OK, 1, true);
            g_fail_next_full = true;
            expect("gallery alert page, panel fails", tiny_display_gallery_nav(-1),
                   ESP_FAIL, 0, true);
            expect("normal card replaces gallery", tiny_display_render_card(normal),
                   ESP_OK, 1, false);
            expect("gallery reopened after replace", tiny_display_gallery_nav(0),
                   ESP_OK, 1, false);
            expect("gallery normal", tiny_display_render_card(
                R"J({"type":"gallery","urls":["https://x/3.raw","https://x/4.raw"]})J"),
                ESP_OK, 1, false);
            expect("gallery normal next page", tiny_display_gallery_nav(+1), ESP_OK, 1, false);

            expect("universe alert push", tiny_display_render_card(
                R"J({"type":"universe","priority":"alert","tinys":["bonk"]})J"),
                ESP_OK, 1, true);
            CHECK(!strcmp(tiny_display_current_card_id(), "universe") &&
                      g_roster.size() == 2 && g_roster[1] == "bonk",
                  "led/universe: page on glass (%s), pushed slug merged",
                  tiny_display_current_card_id());
            expect("universe alert row-tap repaint", repaint_universe(), ESP_OK, 1, true);
            expect("universe alert rerender", tiny_display_rerender(), ESP_OK, 1, true);
            g_fail_next_full = true;
            expect("universe repaint, panel fails", repaint_universe(), ESP_FAIL, 0, true);
            expect("normal universe push over alert page", tiny_display_render_card(
                R"J({"type":"universe"})J"), ESP_OK, 1, false);
            expect("repaint after normal push", repaint_universe(), ESP_OK, 1, false);
            expect("alert universe again", tiny_display_render_card(
                R"J({"type":"universe","priority":"alert"})J"), ESP_OK, 1, true);
            expect("normal card replaces universe", tiny_display_render_card(normal),
                   ESP_OK, 1, false);
            expect("universe repaint after replace", repaint_universe(), ESP_OK, 1, false);

            // ---- page ownership is typed, not a card_id ---------------------
            // A sender may legally name any card "gallery" or "universe". Such
            // a card is not the device's page: a later page flip / reopen or
            // roster repaint must not inherit its alert, it must not drive the
            // UP/DOWN page-flip rule, and the real pages still inherit from
            // themselves.
            expect("gallery store page, alert", tiny_display_render_card(
                R"J({"type":"gallery","urls":["https://x/5.raw","https://x/6.raw"],"priority":"alert"})J"),
                ESP_OK, 1, true);
            CHECK(tiny_display_glass_is_gallery(),
                  "typed page: the gallery store's page is the gallery");
            expect("text card NAMED gallery, alert", tiny_display_render_card(
                R"J({"type":"text","card_id":"gallery","title":"x","priority":"alert","body":"y"})J"),
                ESP_OK, 1, true);
            CHECK(!tiny_display_glass_is_gallery() &&
                      !strcmp(tiny_display_current_card_id(), "gallery"),
                  "typed page: a card named gallery is not the gallery page");
            expect("gallery reopened after a card named gallery",
                   tiny_display_gallery_nav(0), ESP_OK, 1, false);
            CHECK(tiny_display_glass_is_gallery(), "typed page: reopened gallery");
            expect("image card NAMED gallery, alert", tiny_display_render_card(
                R"J({"type":"image","card_id":"gallery","url":"https://x/c.raw","priority":"alert"})J"),
                ESP_OK, 1, true);
            CHECK(!tiny_display_glass_is_gallery(),
                  "typed page: an image named gallery is not the gallery page");
            expect("gallery flip after an image named gallery",
                   tiny_display_gallery_nav(+1), ESP_OK, 1, false);

            expect("text card NAMED universe, alert", tiny_display_render_card(
                R"J({"type":"text","card_id":"universe","title":"x","priority":"alert","body":"y"})J"),
                ESP_OK, 1, true);
            expect("universe repaint after a card named universe", repaint_universe(),
                   ESP_OK, 1, false);
            expect("composite NAMED universe, alert", tiny_display_render_card(
                R"J({"type":"composite","card_id":"universe","title":"x","priority":"alert","parts":[{"type":"text","body":"y"}]})J"),
                ESP_OK, 1, true);
            expect("universe repaint after a composite named universe",
                   repaint_universe(), ESP_OK, 1, false);
            // The universe page's own JSON, but SENT (no priority) over the
            // device's alert universe page: a sender's card, so normal.
            expect("alert universe page for the sender case", tiny_display_render_card(
                R"J({"type":"universe","priority":"alert"})J"), ESP_OK, 1, true);
            {
                char roster[TINY_AGENT_ROSTER_MAX][TINY_AGENT_SLUG_MAX + 1];
                const int n = tiny_agent_roster(roster, TINY_AGENT_ROSTER_MAX);
                char *cs = tiny_universe_card_json(g_agent, roster, n, nullptr);
                expect("sender re-sends the page JSON with no priority",
                       tiny_display_render_card(cs), ESP_OK, 1, false);
                free(cs);
            }
            // The real pages still inherit from themselves, including after a
            // replay of the cached universe page.
            expect("alert universe page again", tiny_display_render_card(
                R"J({"type":"universe","priority":"alert"})J"), ESP_OK, 1, true);
            expect("universe page rerender keeps alert", tiny_display_rerender(),
                   ESP_OK, 1, true);
            expect("universe repaint after rerender keeps alert", repaint_universe(),
                   ESP_OK, 1, true);
            expect("normal card ends it", tiny_display_render_card(normal),
                   ESP_OK, 1, false);

            // ---- universe push: every allocation failure fails closed --------
            // One parse, one build, one render. Fail each cJSON allocation the
            // push makes, in turn (parse, preview build, print, the nested
            // render's parse). A failure must change NOTHING — no roster
            // merge, no paint, no cache, no tap regions, no status light —
            // and the first k that succeeds must be the whole push: alert
            // light on, slug merged. Never a "successful" normal page.
            {
                const char *push =
                    R"J({"type":"universe","priority":"alert","tinys":["oom-slug"]})J";
                const uint32_t frame0 = fnv1a(g_buf.data(), g_buf.size());
                const size_t roster0 = g_roster.size();
                bool succeeded = false;
                int failures = 0;
                for (long k = 1; k < 400 && !succeeded; ++k) {
                    const int refreshes = g_full_refreshes;
                    const uint32_t gen0 = tiny_display_card_gen();
                    calls = g_led_alert_calls;
                    cjson_fail_arm(k);
                    const esp_err_t r = tiny_display_render_card(push);
                    const bool injected = g_cjson_failed.load();
                    cjson_fail_disarm();
                    if (!injected) {
                        CHECK(r == ESP_OK, "universe oom: no failure at k=%ld yet %d", k, r);
                        succeeded = true;
                    } else if (r == ESP_OK) {
                        // cJSON may survive a failed allocation it can retry
                        // around; then the push must still be all there.
                        succeeded = true;
                    }
                    if (succeeded) {
                        CHECK(g_led_alert && g_led_alert_calls == calls + 1 &&
                                  !strcmp(tiny_display_current_card_id(), "universe") &&
                                  g_roster.size() == roster0 + 1 &&
                                  g_roster.back() == "oom-slug",
                              "universe oom: k=%ld success is the whole alert push", k);
                        // Whole page too: the glass and the tap regions must
                        // equal a reference render of the complete composite
                        // (built with no injection) — a half-built page would
                        // drop a row, or point a row index at the wrong slug.
                        auto census = [] {
                            tiny_button_region_t regs[48];
                            const int nr = tiny_display_button_regions(regs, 48);
                            std::vector<std::string> v;
                            for (int i = 0; i < nr; ++i)
                                v.push_back(std::string(regs[i].id) + "=" + regs[i].label);
                            return v;
                        };
                        const uint32_t got_frame = fnv1a(g_buf.data(), g_buf.size());
                        const auto got = census();
                        char roster[TINY_AGENT_ROSTER_MAX][TINY_AGENT_SLUG_MAX + 1];
                        const int rn = tiny_agent_roster(roster, TINY_AGENT_ROSTER_MAX);
                        char *ref = tiny_universe_card_json(g_agent, roster, rn, "alert");
                        CHECK(ref && tiny_display_render_card(ref) == ESP_OK,
                              "universe oom: reference page renders");
                        free(ref);
                        CHECK(got_frame == fnv1a(g_buf.data(), g_buf.size()) &&
                                  got == census() && got.size() >= 2,
                              "universe oom: k=%ld page is the complete composite "
                              "(%zu regions)", k, got.size());
                        break;
                    }
                    ++failures;
                    CHECK(g_roster.size() == roster0,
                          "universe oom: k=%ld (%d) merged a slug", k, r);
                    CHECK(g_full_refreshes == refreshes &&
                              tiny_display_card_gen() == gen0 &&
                              fnv1a(g_buf.data(), g_buf.size()) == frame0,
                          "universe oom: k=%ld (%d) reached the glass", k, r);
                    CHECK(g_led_alert_calls == calls && !g_led_alert,
                          "universe oom: k=%ld (%d) touched the light", k, r);
                    CHECK(!strcmp(tiny_display_current_card_id(), "n1"),
                          "universe oom: k=%ld (%d) changed glass identity", k, r);
                }
                CHECK(succeeded && failures >= 4,
                      "universe oom: swept %d failing allocations then succeeded",
                      failures);
                calls = g_led_alert_calls;
                CHECK(tiny_display_rerender() == ESP_OK && g_led_alert &&
                          !strcmp(tiny_display_current_card_id(), "universe"),
                      "universe oom: replay is the committed alert push");
                calls = g_led_alert_calls;
                expect("normal after oom sweep", tiny_display_render_card(normal),
                       ESP_OK, 1, false);
            }
            // A push whose panel commit fails merges nothing either.
            {
                const size_t roster0 = g_roster.size();
                g_fail_next_full = true;
                expect("universe push, panel fails", tiny_display_render_card(
                    R"J({"type":"universe","priority":"alert","tinys":["panel-fail-slug"]})J"),
                    ESP_FAIL, 0, false);
                CHECK(g_roster.size() == roster0,
                      "universe push, panel fails: no slug merged");
            }
            // ---- universe push: the roster write is part of the commit ------
            // The merged roster is ONE write, made after the panel took the
            // page and before anything is published. A refused write (NVS, or
            // a roster that changed under the page) must fail the push and
            // leave everything as it was: roster, glass, identity, tap
            // regions, card_gen, replay cache and light — so no row a human
            // can tap is one a reboot would lose.
            {
                auto census = [] {
                    tiny_button_region_t regs[48];
                    const int nr = tiny_display_button_regions(regs, 48);
                    std::vector<std::string> v;
                    for (int i = 0; i < nr; ++i)
                        v.push_back(std::string(regs[i].id) + "=" + regs[i].label);
                    return v;
                };
                const char *push =
                    R"J({"type":"universe","priority":"alert","tinys":["p1","p2","p3"]})J";
                for (const esp_err_t why : {ESP_FAIL, ESP_ERR_INVALID_STATE}) {
                    const auto roster0 = g_roster;
                    const uint32_t frame0 = fnv1a(g_buf.data(), g_buf.size());
                    const auto regs0 = census();
                    const uint32_t gen0 = tiny_display_card_gen();
                    const std::string id0 = tiny_display_current_card_id();
                    const int refreshes = g_full_refreshes;
                    const int commits = g_roster_commits;
                    if (why == ESP_FAIL) {
                        g_roster_commit_fail = ESP_FAIL;   // NVS refuses
                    } else {
                        // Another task edits the roster after the page was
                        // built from it: the page's rows are stale.
                        arm_seam("universe-claimed", [] { g_roster.push_back("racer"); });
                    }
                    const auto roster_at_commit = why == ESP_FAIL ? roster0 : [&] {
                        auto r = roster0; r.push_back("racer"); return r; }();
                    calls = g_led_alert_calls;
                    const esp_err_t r = tiny_display_render_card(push);
                    CHECK(r == why, "universe persist %x: returned %x", why, r);
                    CHECK(g_roster_commits == commits + 1,
                          "universe persist %x: %d roster writes, want one batch",
                          why, g_roster_commits - commits);
                    CHECK(g_full_refreshes == refreshes + 1,
                          "universe persist %x: panel took the page first", why);
                    CHECK(g_roster == roster_at_commit,
                          "universe persist %x: roster changed (%zu rows)", why,
                          g_roster.size());
                    CHECK(fnv1a(g_buf.data(), g_buf.size()) == frame0 &&
                              census() == regs0 && tiny_display_card_gen() == gen0 &&
                              id0 == tiny_display_current_card_id(),
                          "universe persist %x: glass, regions or identity moved", why);
                    CHECK(g_led_alert_calls == calls && !g_led_alert,
                          "universe persist %x: touched the light", why);
                    CHECK(tiny_display_rerender() == ESP_OK &&
                              id0 == tiny_display_current_card_id() && !g_led_alert &&
                              g_roster == roster_at_commit,
                          "universe persist %x: replay is the previous card", why);
                    g_roster = roster0;
                }
                // Retry with NVS healthy: all three slugs in one write, the
                // page committed alert, and every row it shows is in the roster.
                const auto roster0 = g_roster;
                const int commits = g_roster_commits;
                calls = g_led_alert_calls;
                expect("universe multi-slug push", tiny_display_render_card(push),
                       ESP_OK, 1, true);
                CHECK(g_roster_commits == commits + 1 &&
                          g_roster.size() == roster0.size() + 3 &&
                          g_roster[g_roster.size() - 3] == "p1" &&
                          g_roster.back() == "p3",
                      "universe multi-slug push: one write, all slugs");
                bool rows_ok = true;
                for (const auto &reg : census())
                    if (!strncmp(reg.c_str(), "u:i:", 4)) {
                        const int i = atoi(reg.c_str() + 4);
                        rows_ok = rows_ok && i >= 0 && i < (int)g_roster.size() &&
                                  reg.find("=@" + g_roster[i]) != std::string::npos;
                    }
                CHECK(rows_ok, "universe multi-slug push: every row names its roster slug");
                // Nothing new to merge: no write at all.
                expect("universe push, slugs already in roster",
                       tiny_display_render_card(push), ESP_OK, 1, true);
                CHECK(g_roster_commits == commits + 1,
                      "universe push of known slugs writes nothing");
                expect("normal after multi-slug", tiny_display_render_card(normal),
                       ESP_OK, 1, false);
            }
            unserve();

            // ---- one request, one claim: universe push ----------------------
            // Serialized: a newer card that arrives while a universe push holds
            // render ownership waits, then commits after it — never inside it.
            {
                std::atomic<int> newer{ESP_FAIL};
                std::thread nthr;
                int attempts;
                {
                    std::lock_guard<std::mutex> lock(g_attempt_mutex);
                    attempts = g_render_attempts;
                }
                arm_seam("universe-claimed", [&] {
                    nthr = std::thread([&] {
                        newer = tiny_display_render_card(normal);
                    });
                    CHECK(wait_for_attempts(attempts + 1),
                          "universe claim: newer card waits for the push");
                });
                calls = g_led_alert_calls;
                const esp_err_t r = tiny_display_render_card(
                    R"J({"type":"universe","priority":"alert"})J");
                nthr.join();
                CHECK(r == ESP_OK && newer == ESP_OK,
                      "universe claim: push %d, newer %d both commit", r, newer.load());
                CHECK(g_led_alert_calls == calls + 2 && !g_led_alert &&
                          !strcmp(tiny_display_current_card_id(), "n1"),
                      "universe claim: newer normal card is last on glass and light");
                CHECK(tiny_display_rerender() == ESP_OK &&
                          !strcmp(tiny_display_current_card_id(), "n1") && !g_led_alert,
                      "universe claim: replay is the newer card");
            }
            // Overtaken: a universe FINALE claims its epoch and releases render
            // ownership (stream_end). A newer card commits in that gap; the
            // older universe must then be SUPERSEDED — it may not mint a fresh
            // claim for its nested roster composite and paint over the newer.
            for (const char *newer_card :
                 {R"J({"type":"text","card_id":"after-u","title":"n","body":"newest"})J",
                  R"J({"type":"universe","tinys":["newer-u"]})J"}) {
                CHECK(tiny_display_stream_begin("tiny", "ansU", nullptr) == ESP_OK,
                      "universe finale: begin");
                tiny_display_stream_append("words");
                std::atomic<int> newer{ESP_FAIL};
                arm_seam("finale-claimed", [&] {
                    std::thread t([&] { newer = tiny_display_render_card(newer_card); });
                    t.join();   // the newer request fully commits in the gap
                });
                const int refreshes = g_full_refreshes;
                const uint32_t gen0 = tiny_display_card_gen();
                calls = g_led_alert_calls;
                const esp_err_t fin = tiny_display_stream_end(
                    R"J({"type":"universe","priority":"alert","tinys":["older-u"]})J");
                const bool newer_is_text = strstr(newer_card, "after-u") != nullptr;
                CHECK(newer == ESP_OK && fin == TINY_DISPLAY_SUPERSEDED,
                      "universe finale: newer %d, older finale %d (want SUPERSEDED)",
                      newer.load(), fin);
                CHECK(g_full_refreshes == refreshes + 1 &&
                          tiny_display_card_gen() == gen0 + 1 &&
                          g_led_alert_calls == calls + 1 && !g_led_alert,
                      "universe finale: exactly one commit (the newer, normal)");
                CHECK(!strcmp(tiny_display_current_card_id(),
                              newer_is_text ? "after-u" : "universe") &&
                          !tiny_display_stream_active(),
                      "universe finale: newer card owns glass (%s)",
                      tiny_display_current_card_id());
                CHECK(tiny_display_rerender() == ESP_OK && !g_led_alert &&
                          !strcmp(tiny_display_current_card_id(),
                                  newer_is_text ? "after-u" : "universe"),
                      "universe finale: replay is the newer card, still normal");
                bool older_merged = false;
                for (const auto &r : g_roster) older_merged |= r == "older-u";
                CHECK(!older_merged,
                      "universe finale: superseded push merged no roster slug");
            }

            // ---- photo archive follows glass order --------------------------
            // Image A paints and pauses after its commit, before archiving.
            // Image B is sent then. B cannot paint (or archive) until A has
            // archived, so 00001 = A and 00002 = B; a B resend archives
            // nothing (adjacent dedupe names B), and an A resend is new again.
            // Proven by admission, not by time: at the seam A must still hold
            // render ownership (the recursive lock's per-thread level), and B
            // must have reached its own render-lock attempt — so B is queued
            // on the lock A holds, cannot have painted, and runs only after
            // A's archive. Moving the archive outside A's render transaction
            // fails the ownership check on every run, whatever the scheduler.
            {
                g_sd_present = true;
                auto photo = [](int n) {
                    char path[96];
                    snprintf(path, sizeof path, STICKY_SD_MOUNT_POINT "/photos/%05d.raw", n);
                    std::vector<uint8_t> out;
                    if (FILE *f = fopen(path, "rb")) {
                        int ch;
                        while ((ch = fgetc(f)) != EOF) out.push_back((uint8_t)ch);
                        fclose(f);
                    }
                    return out;
                };
                const std::vector<uint8_t> a_bytes(mono, 0xA1), b_bytes(mono, 0xB2);
                serve(0xA1);
                const char *img_a =
                    R"J({"type":"image","card_id":"pa","url":"https://x/pa.raw"})J";
                const char *img_b =
                    R"J({"type":"image","card_id":"pb","url":"https://x/pb.raw"})J";
                std::atomic<int> rb{ESP_FAIL};
                std::atomic<bool> b_done{false};
                std::thread bthr;
                bool a_owned = false, b_queued = false, b_pending = false;
                int b_refreshes = -1;
                arm_seam("image-painted", [&] {
                    a_owned = render_test_recursive_held > 0;
                    {
                        std::lock_guard<std::mutex> lock(g_http_mutex);
                        g_http_body.assign(mono, 0xB2);   // A already fetched
                    }
                    int attempts;
                    {
                        std::lock_guard<std::mutex> lock(g_attempt_mutex);
                        attempts = g_render_attempts;
                    }
                    const int refreshes = g_full_refreshes;
                    bthr = std::thread([&] {
                        rb = tiny_display_render_card(img_b);
                        b_done = true;
                    });
                    b_queued = wait_for_attempts(attempts + 1);
                    b_pending = !b_done.load();
                    b_refreshes = g_full_refreshes - refreshes;
                });
                const esp_err_t ra = tiny_display_render_card(img_a);
                bthr.join();
                CHECK(ra == ESP_OK && rb == ESP_OK, "archive order: A %d, B %d", ra,
                      rb.load());
                CHECK(a_owned,
                      "archive order: A archives inside its render transaction");
                CHECK(b_queued && b_pending && b_refreshes == 0,
                      "archive order: B queued on render ownership, unpainted, "
                      "while A archives (queued %d, pending %d, refreshes %d)",
                      b_queued, b_pending, b_refreshes);
                CHECK(photo(1) == a_bytes && photo(2) == b_bytes && photo(3).empty(),
                      "archive order: 00001=A (%zu B), 00002=B (%zu B), no 00003",
                      photo(1).size(), photo(2).size());
                CHECK(!strcmp(tiny_display_current_card_id(), "pb"),
                      "archive order: B is on glass");
                CHECK(tiny_display_render_card(img_b) == ESP_OK && photo(3).empty(),
                      "archive order: B resend is deduped against the last archived");
                serve(0xA1);
                CHECK(tiny_display_render_card(img_a) == ESP_OK && photo(3) == a_bytes,
                      "archive order: A again after B archives 00003");
                unserve();
                g_sd_present = false;
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
            CHECK(image_result == TINY_DISPLAY_SUPERSEDED,
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
            CHECK(gal_result == TINY_DISPLAY_SUPERSEDED,
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
            const int repairs = g_gray_refreshes;
            CHECK(tiny_display_render_card(
                      R"J({"type":"text","card_id":"failed-over-image","title":"x","body":"y","buttons":[{"label":"bad","accent":"pink"}]})J") == ESP_FAIL &&
                      !strcmp(tiny_display_current_card_id(), "stream") &&
                      tiny_display_rotation() == 90 &&
                      sticky_display_canvas()->rotation() == CanvasRotation::Landscape &&
                      frame_hash() == image_frame_hash &&
                      g_gray_refreshes == repairs + 1,
                  "panel native: failed drawn card restores image bytes/touch frame "
                  "and repaints old glass");
            // Gallery publishes native-space tap regions without running the
            // generic layout. Its visible ack therefore catches stale s_w/s_h:
            // correct landscape dimensions repaint this bottom-left interior
            // white; the failed portrait candidate would repaint 80 px above.
            CHECK(tiny_display_gallery_nav(0) == ESP_OK && px(31, 390) == 0,
                  "panel native: gallery probe starts outside button paint");
            const esp_err_t ack_err = tiny_display_flash_button(0);
            const int ack_px = px(31, 390);
            const int ack_accent = sticky_display_canvas()->accent_at(31, 390);
            CHECK(ack_err == ESP_OK && ack_px == 3 &&
                      ack_accent == lcd_palette::kNeutral,
                  "panel native: failed card restores live ack dimensions/style "
                  "(px=%d accent=%d)", ack_px, ack_accent);
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
            CHECK(before_blit == TINY_DISPLAY_SUPERSEDED &&
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

                const uint32_t keyboard_frame = frame_hash();
                g_fail_next_full = true;
                CHECK(tiny_display_blit_1bit(raw_blit.data(), raw_blit.size(), false) == ESP_FAIL &&
                          !strcmp(tiny_display_current_card_id(), "kb-blit") &&
                          frame_hash() == keyboard_frame,
                      "kb->blit: failed raw frame keeps keyboard bytes on glass");
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
            CHECK(fin == TINY_DISPLAY_SUPERSEDED,
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
