// tiny_photo — fetch a message photo from the media store and decode it to
// 8-bit luma (grammar v27).
//
// Message attachments are JPEGs on https://plugin.tiny.technology/media/<key>
// (the same origin the `ota <url>` form trusts; tiny_ota_util::media_url_ok
// is the one allowlist). Nothing else is fetched: an attachment URL is data a
// stranger chose, and the device does not follow it anywhere else.
//
// Decoding uses the TJpgDec that ships in the ESP32-S3 ROM (jd_prepare /
// jd_decomp, esp32s3.rom.ld) — no component, no flash cost. It handles
// baseline JPEG only; progressive, arithmetic-coded and non-JPEG bytes
// answer ESP_ERR_NOT_SUPPORTED, which the glass shows as "photo n/a".
// TJpgDec descales by 1/2..1/8 while decoding, so a 12 MP phone photo is
// decoded at 1/8 for a thumbnail and never exists at full size in RAM.
#include "tiny/tiny_photo.h"

#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_rom_caps.h"
#include "esp_timer.h"
#include "rom/tjpgd.h"

#if !ESP_ROM_HAS_JPEG_DECODE
#error "tiny_photo needs a ROM TJpgDec (ESP32-S3)"
#endif

#include "tiny/tiny_ota_util.h"
#include "tiny/tiny_photo_util.h"

static const char *TAG = "tiny_photo";

// A phone JPEG is 1-4 MB; the platform re-encodes most attachments far
// smaller (44 KB / 19 KB in the first real thread). 2 MB of PSRAM is the
// ceiling: past that the fetch is refused, not truncated.
static constexpr size_t kMaxJpegBytes = 2u * 1024u * 1024u;
// Decoded luma at the chosen 1/2^s scale. 1.5 MP is ample for an 800 px box.
static constexpr size_t kMaxDecodedPx = 1536u * 1024u;
// TJpgDec's pool: Huffman/quant tables + one MCU. 3.1 KB is the documented
// need (esp_jpeg JPEG_WORK_BUF_SIZE); 4 KB leaves margin for 2x2 subsampling.
static constexpr size_t kWorkBytes = 4096;
// One photo's whole fetch, wall clock (see fetch()).
static constexpr int64_t kFetchWallUs = 8 * 1000000;

static esp_err_t fetch(const char *url, uint8_t **out, size_t *out_len) {
    esp_http_client_config_t hc = {};
    hc.url = url;
    // 6 s, not the 15 s other fetches use: a thread opens on the touch
    // action task, whose wedge watchdog reboots at 45 s, and the messages
    // call plus several photos must all fit under it (tiny_display.cpp
    // budgets the photos of one card at kPhotoBudgetUs).
    hc.timeout_ms = 6000;
    hc.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return ESP_FAIL;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(c); return err; }
    const int64_t clen = esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "photo fetch: HTTP %d", status);
        esp_http_client_cleanup(c);
        return status == 404 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
    }
    if (clen > (int64_t)kMaxJpegBytes) {
        ESP_LOGW(TAG, "photo fetch: %lld B over the %u B cap", (long long)clen,
                 (unsigned)kMaxJpegBytes);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_SIZE;
    }
    // Known length: one exact buffer. Chunked: grow in 64 KB steps to the cap.
    size_t cap = clen > 0 ? (size_t)clen : 64u * 1024u;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_http_client_cleanup(c); return ESP_ERR_NO_MEM; }
    size_t got = 0;
    const int64_t t_start = esp_timer_get_time();
    for (;;) {
        // Wall-clock cap per photo: the read timeout is per read, so a server
        // that trickles bytes could otherwise hold the act task indefinitely.
        if (esp_timer_get_time() - t_start > kFetchWallUs) {
            ESP_LOGW(TAG, "photo fetch: over %d s, abandoned", (int)(kFetchWallUs / 1000000));
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (got == cap) {
            if (clen > 0) break;                       // exact: done
            if (cap >= kMaxJpegBytes) { err = ESP_ERR_INVALID_SIZE; break; }
            size_t ncap = cap + 64u * 1024u;
            if (ncap > kMaxJpegBytes) ncap = kMaxJpegBytes;
            uint8_t *nb = (uint8_t *)heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
            if (!nb) { err = ESP_ERR_NO_MEM; break; }
            buf = nb;
            cap = ncap;
        }
        const int r = esp_http_client_read(c, (char *)buf + got, cap - got);
        if (r < 0) { err = ESP_FAIL; break; }
        if (r == 0) break;
        got += (size_t)r;
    }
    esp_http_client_cleanup(c);
    if (err == ESP_OK && (got == 0 || (clen > 0 && got != (size_t)clen)))
        err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK) { heap_caps_free(buf); return err; }
    *out = buf;
    *out_len = got;
    return ESP_OK;
}

struct DecodeCtx {
    const uint8_t *src;
    size_t len;
    size_t pos;
    uint8_t *luma;   // ow * oh
    int ow, oh;
};

static UINT in_cb(JDEC *jd, BYTE *buf, UINT n) {
    DecodeCtx *d = static_cast<DecodeCtx *>(jd->device);
    const size_t left = d->len - d->pos;
    if (n > left) n = (UINT)left;
    if (buf) memcpy(buf, d->src + d->pos, n);   // NULL = skip n bytes
    d->pos += n;
    return n;
}

// TJpgDec hands over one decoded MCU block as RGB888 in output (scaled)
// coordinates. Rects can run past the floor-divided image edge by a partial
// MCU; those pixels are clipped, never written.
static UINT out_cb(JDEC *jd, void *bitmap, JRECT *rect) {
    DecodeCtx *d = static_cast<DecodeCtx *>(jd->device);
    const uint8_t *in = static_cast<const uint8_t *>(bitmap);
    for (int y = rect->top; y <= rect->bottom; ++y) {
        for (int x = rect->left; x <= rect->right; ++x, in += 3) {
            if (x < d->ow && y < d->oh)
                d->luma[(size_t)y * d->ow + x] = tiny_photo_util::luma(in[0], in[1], in[2]);
        }
    }
    return 1;
}

static esp_err_t decode(const uint8_t *jpg, size_t len, int box_w, int box_h,
                        bool allow_up, uint8_t **out, int *ow, int *oh) {
    void *work = heap_caps_malloc(kWorkBytes, MALLOC_CAP_SPIRAM);
    if (!work) work = malloc(kWorkBytes);
    if (!work) return ESP_ERR_NO_MEM;
    DecodeCtx d = {jpg, len, 0, nullptr, 0, 0};
    JDEC jd = {};
    JRESULT jr = jd_prepare(&jd, in_cb, work, kWorkBytes, &d);
    if (jr != JDR_OK) {
        free(work);   // IDF's free() releases heap_caps_malloc memory too
        ESP_LOGW(TAG, "jd_prepare: %d (%s)", (int)jr,
                 jr == JDR_FMT3 ? "progressive/unsupported JPEG" : "not a JPEG");
        return (jr == JDR_MEM1 || jr == JDR_MEM2) ? ESP_ERR_NO_MEM
                                                  : ESP_ERR_NOT_SUPPORTED;
    }
    const int sw = (int)jd.width, sh = (int)jd.height;
    int fw = 0, fh = 0;
    tiny_photo_util::fit(sw, sh, box_w, box_h, allow_up, &fw, &fh);
    const int s = tiny_photo_util::choose_scale(sw, sh, fw, fh);
    d.ow = sw >> s;
    d.oh = sh >> s;
    if (d.ow <= 0 || d.oh <= 0 || (size_t)d.ow * d.oh > kMaxDecodedPx) {
        free(work);
        ESP_LOGW(TAG, "photo %dx%d at 1/%d too large to decode", sw, sh, 1 << s);
        return ESP_ERR_INVALID_SIZE;
    }
    d.luma = (uint8_t *)heap_caps_malloc((size_t)d.ow * d.oh, MALLOC_CAP_SPIRAM);
    if (!d.luma) { free(work); return ESP_ERR_NO_MEM; }
    memset(d.luma, 0xFF, (size_t)d.ow * d.oh);
    jr = jd_decomp(&jd, out_cb, (BYTE)s);
    free(work);
    if (jr != JDR_OK) {
        heap_caps_free(d.luma);
        ESP_LOGW(TAG, "jd_decomp: %d", (int)jr);
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Area-resample the 1/2^s decode down to the fitted size.
    if (fw == d.ow && fh == d.oh) {
        *out = d.luma;
    } else {
        uint8_t *fitted = (uint8_t *)heap_caps_malloc((size_t)fw * fh, MALLOC_CAP_SPIRAM);
        if (!fitted) { heap_caps_free(d.luma); return ESP_ERR_NO_MEM; }
        tiny_photo_util::resample(d.luma, d.ow, d.oh, fitted, fw, fh);
        heap_caps_free(d.luma);
        *out = fitted;
    }
    *ow = fw;
    *oh = fh;
    ESP_LOGI(TAG, "photo %dx%d -> 1/%d -> %dx%d", sw, sh, 1 << s, fw, fh);
    return ESP_OK;
}

extern "C" esp_err_t tiny_photo_load(const char *url, int box_w, int box_h,
                                     bool allow_up, uint8_t **out, int *out_w,
                                     int *out_h) {
    if (!out || !out_w || !out_h || box_w <= 0 || box_h <= 0)
        return ESP_ERR_INVALID_ARG;
    *out = nullptr;
    if (!tiny_ota_util::media_url_ok(url)) {
        ESP_LOGW(TAG, "photo url refused (not the media store)");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *jpg = nullptr;
    size_t len = 0;
    esp_err_t err = fetch(url, &jpg, &len);
    if (err != ESP_OK) return err;
    err = decode(jpg, len, box_w, box_h, allow_up, out, out_w, out_h);
    heap_caps_free(jpg);
    return err;
}
