// tiny_pair — on-device pairing (onboarding Phase B). See tiny/tiny_pair.h for
// the contract; tiny/tiny_pair_util.h holds the pure half the host test runs.
//
// Rules this file keeps:
//   * the secret never leaves RAM except inside the poll body over TLS — no
//     log line, no status field, no card carries it (only the code is public);
//   * every server string that reaches JSON goes through cJSON or the
//     escaping merge builder; the code is gated to [A-Za-z0-9]{4,16} before
//     it reaches a URL, a QR, a log line or /info's snprintf;
//   * 15 s request timeout + the IDF cert bundle, as tiny_node;
//   * its own task (12 KB: mbedtls handshake + a card render), never the
//     shell/touch tasks; the glass is repainted only while it shows ob-pair,
//     so a human on the Wi-Fi keyboard page is never yanked away.
#include "tiny/tiny_pair.h"
#include "esp_attr.h"  // EXT_RAM_BSS_ATTR: statics off the internal rail
#include "tiny/tiny_pair_util.h"
#include "tiny/tiny_admit.h"
#include "tiny/tiny_config.h"
#include "tiny/tiny_display.h"
#include "tiny/tiny_node.h"
#include "tiny/tiny_onboard.h"
#include "tiny/tiny_provision.h"
#include "tiny/tiny_wifi.h"

#include <atomic>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace pu = tiny_pair_util;

static const char *TAG = "tiny_pair";
#define PAIR_TIMEOUT_MS 15000
#define PAIR_POLL_MS    3000
#define PAIR_TTL_DEFAULT_S 600

static std::atomic<int> s_state{TINY_PAIR_IDLE};
static std::atomic<bool> s_retry{false};
static std::atomic<int> s_last_http{0};
static TaskHandle_t s_task = NULL;
static int s_last_shown_http = 0;  // pair task only: error-card dedupe

// The public half, read by status / info / render from other tasks.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_code[pu::kCodeMax + 1] = "";
static int64_t s_expires_us = 0;

// The private half: this task only.
static char s_secret[pu::kSecretChars + 1];
static char s_api[96];

static void set_code(const char *code, int ttl_s) {
    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_code, code ? code : "", sizeof s_code);
    s_expires_us = code && *code ? esp_timer_get_time() + (int64_t)ttl_s * 1000000 : 0;
    taskEXIT_CRITICAL(&s_mux);
}

extern "C" bool tiny_pair_code(char *out, size_t cap) {
    if (!out || !cap) return false;
    taskENTER_CRITICAL(&s_mux);
    strlcpy(out, s_code, cap);
    taskEXIT_CRITICAL(&s_mux);
    return out[0] != 0;
}

extern "C" bool tiny_pair_url(char *out, size_t cap) {
    char code[pu::kCodeMax + 1];
    if (!out || !cap || !tiny_pair_code(code, sizeof code)) return false;
    return pu::pair_url(s_api, code, out, cap);
}

extern "C" int tiny_pair_expires_in_s(void) {
    taskENTER_CRITICAL(&s_mux);
    const int64_t e = s_expires_us;
    taskEXIT_CRITICAL(&s_mux);
    if (!e) return 0;
    const int64_t left = (e - esp_timer_get_time()) / 1000000;
    return left > 0 ? (int)left : 0;
}

extern "C" tiny_pair_state_t tiny_pair_state(void) { return (tiny_pair_state_t)s_state.load(); }
extern "C" int tiny_pair_last_http(void) { return s_last_http.load(); }

extern "C" const char *tiny_pair_state_name(tiny_pair_state_t s) {
    switch (s) {
        case TINY_PAIR_ASKING:   return "asking";
        case TINY_PAIR_WAITING:  return "waiting";
        case TINY_PAIR_DENIED:   return "denied";
        case TINY_PAIR_ERROR:    return "error";
        case TINY_PAIR_APPROVED: return "approved";
        default:                 return "idle";
    }
}

extern "C" void tiny_pair_retry(void) {
    s_retry.store(true);
    if (s_task) xTaskNotifyGive(s_task);
}

// Repaint only the pages this module feeds: the pair page, and the link page
// it replaces when Wi-Fi comes up after boot (the step is derived, so the
// render lands on "pair"). Anything else on the glass (the shell's Wi-Fi
// keyboard, a preview) belongs to someone else.
static void repaint(void) {
    // Work (tiny_admit.h): never paint over a claimed sleep's goodbye card.
    tiny_admit::Work work;
    if (!work) return;
    const char *id = tiny_display_current_card_id();
    if (!strcmp(id, "ob-pair") || !strcmp(id, "ob-link")) tiny_onboard_render();
}

// force: repaint even when the state did not move (a new code, a new HTTP
// number on the error card).
static void set_state(tiny_pair_state_t s, bool force = false) {
    if (s_state.exchange(s) != s || force) repaint();
}

// Sleep up to ms, waking early on tiny_pair_retry(). true = woken by retry.
static bool nap(uint32_t ms) {
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) > 0 || s_retry.load();
}

// ---------- HTTP ----------
typedef struct { char *buf; int len, cap; bool over; } sink_t;

static esp_err_t on_http(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    sink_t *s = (sink_t *)e->user_data;
    if (s->len + e->data_len < s->cap) {
        memcpy(s->buf + s->len, e->data, e->data_len);
        s->len += e->data_len;
        s->buf[s->len] = 0;
    } else {
        s->over = true;
    }
    return ESP_OK;
}

// POST json -> status (-1 = no response). An over-cap reply is reported as
// -1 too: a clipped JSON body is not an answer, and no pair reply is near 1 KB.
static int post(const char *path, const char *body, char *resp, int cap) {
    char url[160];
    snprintf(url, sizeof url, "%s%s", s_api, path);
    sink_t sink = { resp, 0, cap, false };
    resp[0] = 0;
    esp_http_client_config_t hc = {};
    hc.url = url;
    hc.method = HTTP_METHOD_POST;
    hc.timeout_ms = PAIR_TIMEOUT_MS;
    hc.crt_bundle_attach = esp_crt_bundle_attach;
    hc.event_handler = on_http;
    hc.user_data = &sink;
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "User-Agent", "sticky-the-reterminal pair");
    esp_http_client_set_post_field(c, body, strlen(body));
    const esp_err_t err = esp_http_client_perform(c);
    const int st = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (err != ESP_OK) ESP_LOGW(TAG, "%s: %s", path, esp_err_to_name(err));
    if (sink.over) {
        ESP_LOGE(TAG, "%s: reply over %d B — treated as no answer", path, cap);
        return -1;
    }
    return st;
}

// ---------- the two calls ----------

// New secret, POST /pair/start. true = a valid code is live.
static bool start_code(void) {
    uint8_t raw[pu::kSecretBytes];
    esp_fill_random(raw, sizeof raw);
    pu::b64url(raw, sizeof raw, s_secret, sizeof s_secret);
    memset(raw, 0, sizeof raw);
    char hash[65];
    pu::secret_hash(s_secret, hash);

    cJSON *b = cJSON_CreateObject();
    if (!b) return false;
    cJSON_AddStringToObject(b, "name", tiny_provision_ssid());
    cJSON_AddStringToObject(b, "platform", "esp32s3");
    cJSON_AddStringToObject(b, "kind", "daemon");
    cJSON *caps = cJSON_AddArrayToObject(b, "capabilities");
    int n = 0;
    const char *const *cv = tiny_node_caps(&n);
    for (int i = 0; i < n; ++i) cJSON_AddItemToArray(caps, cJSON_CreateString(cv[i]));
    cJSON_AddItemToArray(caps, cJSON_CreateString("body:sticky-the-reterminal"));
    cJSON_AddStringToObject(b, "secretHash", hash);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body) return false;

    EXT_RAM_BSS_ATTR static char resp[512];
    const int st = post("/api/devices/pair/start", body, resp, sizeof resp);
    free(body);
    s_last_http.store(st);
    if (st < 200 || st >= 300) {
        ESP_LOGW(TAG, "pair/start -> %d", st);
        return false;
    }
    cJSON *o = cJSON_Parse(resp);
    const cJSON *code = o ? cJSON_GetObjectItem(o, "code") : NULL;
    const cJSON *ttl = o ? cJSON_GetObjectItem(o, "ttl_s") : NULL;
    bool ok = cJSON_IsString(code) && pu::code_ok(code->valuestring);
    if (ok) {
        const int ttl_s = cJSON_IsNumber(ttl) && ttl->valueint > 0 && ttl->valueint <= 3600
                              ? ttl->valueint : PAIR_TTL_DEFAULT_S;
        set_code(code->valuestring, ttl_s);
        char split[pu::kCodeMax + 2], url[160];
        pu::code_split(code->valuestring, split, sizeof split);
        pu::pair_url(s_api, code->valuestring, url, sizeof url);
        // The one line a headless board needs. The code is public; the
        // secret is not, and it is not here.
        ESP_LOGI(TAG, "pair code %s — approve at %s (%d s)", split, url, ttl_s);
    } else {
        ESP_LOGW(TAG, "pair/start 2xx without a usable code");
    }
    cJSON_Delete(o);
    return ok;
}

// One POST /pair/poll. On approval: merge identity, paint, reboot (no return).
static pu::Poll poll_once(const char *code) {
    cJSON *b = cJSON_CreateObject();
    if (!b) return pu::Poll::kRetry;
    cJSON_AddStringToObject(b, "code", code);
    cJSON_AddStringToObject(b, "secret", s_secret);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body) return pu::Poll::kRetry;
    EXT_RAM_BSS_ATTR static char resp[768];
    const int st = post("/api/devices/pair/poll", body, resp, sizeof resp);
    memset(body, 0, strlen(body));  // it held the secret
    free(body);

    cJSON *o = st > 0 ? cJSON_Parse(resp) : NULL;
    const cJSON *sv = o ? cJSON_GetObjectItem(o, "status") : NULL;
    const cJSON *id = o ? cJSON_GetObjectItem(o, "device_id") : NULL;
    const cJSON *tok = o ? cJSON_GetObjectItem(o, "device_token") : NULL;
    const cJSON *nm = o ? cJSON_GetObjectItem(o, "name") : NULL;
    const bool has_tok = cJSON_IsString(tok) && tok->valuestring[0] &&
                         cJSON_IsString(id) && id->valuestring[0];
    const pu::Poll v = pu::classify(st, cJSON_IsString(sv) ? sv->valuestring : NULL, has_tok);
    if (v == pu::Poll::kApproved) {
        EXT_RAM_BSS_ATTR static char merge[320];
        const bool built = pu::merge_json(id->valuestring, tok->valuestring,
                                          cJSON_IsString(nm) ? nm->valuestring : "",
                                          merge, sizeof merge);
        const esp_err_t e = built ? tiny_config_merge_json(merge) : ESP_ERR_INVALID_SIZE;
        memset(merge, 0, sizeof merge);  // it held the token
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "approved: paired as %s (%s) — rebooting",
                     cJSON_IsString(nm) ? nm->valuestring : "?", id->valuestring);
            cJSON_Delete(o);
            s_state.store(TINY_PAIR_APPROVED);
            set_code(NULL, 0);
            tiny_display_render_card(
                "{\"type\":\"text\",\"title\":\"paired\",\"card_id\":\"provisioned\","
                "\"body\":\"Approved. Rebooting to say hello to your tiny...\"}");
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_restart();
        }
        // The token is shown once: a merge that failed lost it. Say so, and
        // let the loop get a fresh code (the server answers "claimed" here).
        ESP_LOGE(TAG, "approved but identity not saved: %s", esp_err_to_name(e));
        cJSON_Delete(o);
        return pu::Poll::kRestart;
    }
    if (v != pu::Poll::kPending)
        ESP_LOGI(TAG, "pair/poll -> %d %s", st, cJSON_IsString(sv) ? sv->valuestring : "-");
    cJSON_Delete(o);
    return v;
}

static bool provisioned(void) {
    EXT_RAM_BSS_ATTR static tiny_config_t cfg;  // ~1 KB: off this task's stack
    return tiny_config_load(&cfg) == ESP_OK && tiny_config_is_provisioned(&cfg);
}

static void pair_task(void *) {
    uint32_t err_backoff = 0;
    for (;;) {
        // Provisioned by someone else (the portal paste fallback reboots on
        // its own; this is the belt to that brace) -> the task is done.
        if (provisioned()) break;
        if (!tiny_wifi_is_up()) {
            set_code(NULL, 0);
            set_state(TINY_PAIR_ASKING);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        s_retry.store(false);
        // No repaint: renewing a code keeps the old card up until the new
        // one exists (one e-ink refresh per code, not two).
        s_state.store(TINY_PAIR_ASKING);
        if (!start_code()) {
            set_code(NULL, 0);
            err_backoff = pu::backoff_ms(err_backoff);
            const int prev_http = s_last_shown_http;
            s_last_shown_http = s_last_http.load();
            set_state(TINY_PAIR_ERROR, prev_http != s_last_shown_http);
            nap(err_backoff);
            continue;
        }
        err_backoff = 0;
        s_last_shown_http = 0;
        s_retry.store(false);  // a [New code] pressed while asking got this one
        set_state(TINY_PAIR_WAITING, true);  // a new code is a new card

        char code[pu::kCodeMax + 1];
        tiny_pair_code(code, sizeof code);
        uint32_t wait = PAIR_POLL_MS;
        bool denied = false;
        for (;;) {
            if (nap(wait)) break;                       // [New code]
            if (tiny_pair_expires_in_s() <= 0) {        // client deadline = ttl_s
                ESP_LOGI(TAG, "code expired unapproved — getting a new one");
                break;
            }
            if (!tiny_wifi_is_up()) { wait = PAIR_POLL_MS; continue; }
            const pu::Poll v = poll_once(code);
            if (v == pu::Poll::kPending) { wait = PAIR_POLL_MS; continue; }
            if (v == pu::Poll::kRetry) { wait = pu::backoff_ms(wait); continue; }
            denied = v == pu::Poll::kDenied;
            break;                                      // restart (or denied)
        }
        set_code(NULL, 0);
        memset(s_secret, 0, sizeof s_secret);
        if (denied) {
            set_state(TINY_PAIR_DENIED);
            // Wait for a human: a denied device must not re-ask on its own.
            while (!s_retry.load()) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
    set_code(NULL, 0);
    memset(s_secret, 0, sizeof s_secret);
    s_state.store(TINY_PAIR_IDLE);
    ESP_LOGI(TAG, "provisioned — pairing task ends");
    // s_claim stays set: a provisioned device never pairs again this boot.
    s_task = NULL;
    vTaskDelete(NULL);
}

extern "C" esp_err_t tiny_pair_start(void) {
    // Called from boot (tiny_main) and from every pair-page paint (act task):
    // one winner, everyone else sees it running.
    static std::atomic<bool> s_claim{false};
    bool expect = false;
    if (!s_claim.compare_exchange_strong(expect, true)) return ESP_OK;
    EXT_RAM_BSS_ATTR static tiny_config_t cfg;
    if (tiny_config_load(&cfg) != ESP_OK || tiny_config_is_provisioned(&cfg)) {
        s_claim.store(false);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_api, cfg.api[0] ? cfg.api : "https://tiny.technology", sizeof s_api);
    // strip a trailing '/' so "%s%s" with "/api/..." never doubles it
    for (size_t n = strlen(s_api); n > 0 && s_api[n - 1] == '/'; --n) s_api[n - 1] = 0;
    s_state.store(TINY_PAIR_ASKING);
    // 12 KB: mbedtls handshake + a full card render (repaint) on this stack;
    // the act task carries the same pair of jobs at the same figure.
    if (xTaskCreate(pair_task, "tiny_pair", 12288, NULL, 3, &s_task) != pdPASS) {
        s_state.store(TINY_PAIR_IDLE);
        s_claim.store(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
