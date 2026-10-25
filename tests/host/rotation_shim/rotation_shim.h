// rotation_shim — the IDF surface firmware/main/tiny_config.cpp needs on a
// host, force-included ahead of it (rotation_host_test.sh). NVS is the
// file-backed store in ../shim/nvs.h (a PROCESS RESTART is a reboot); the
// pieces below are what that shim does not carry: esp_check, the NVS error
// codes, the blob calls and nvs_flash_erase. A forced failure lets the test
// prove a failed save never claims "persisted".
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

// glibc < 2.38 has no strlcpy; the device's newlib does (same detection as
// render_shim.h, so a fortified glibc strlcpy is never redefined).
#if !defined(ROTATION_SHIM_HAVE_STRLCPY) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38))
#define ROTATION_SHIM_HAVE_STRLCPY 1
#endif
#ifndef ROTATION_SHIM_HAVE_STRLCPY
static inline size_t strlcpy(char *d, const char *s, size_t n) {
    const size_t l = strlen(s);
    if (n) { const size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
#endif

#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_NO_FREE_PAGES (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)

#define ESP_RETURN_ON_ERROR(x, tag, msg)        \
    do {                                        \
        const esp_err_t err_rc_ = (x);          \
        if (err_rc_ != ESP_OK) return err_rc_;  \
    } while (0)

static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }

namespace rotation_shim {
inline bool &fail_next_set() { static bool f = false; return f; }
}
// nvs.h's nvs_set_u8 cannot fail; route tiny_config.cpp's calls through a
// wrapper so the test can inject one failed write.
static inline esp_err_t rotation_shim_set_u8(nvs_handle_t h, const char *k,
                                             uint8_t v) {
    if (rotation_shim::fail_next_set()) {
        rotation_shim::fail_next_set() = false;
        return ESP_FAIL;
    }
    return nvs_set_u8(h, k, v);
}
#define nvs_set_u8 rotation_shim_set_u8

static inline esp_err_t nvs_set_blob(nvs_handle_t, const char *k,
                                     const void *v, size_t len) {
    nvs_shim::store()[nvs_shim::ns() + "." + k] =
        std::string(static_cast<const char *>(v), len);
    return ESP_OK;
}
static inline esp_err_t nvs_get_blob(nvs_handle_t, const char *k, void *out,
                                     size_t *len) {
    auto &m = nvs_shim::store();
    auto it = m.find(nvs_shim::ns() + "." + k);
    if (it == m.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (out) {
        if (it->second.size() > *len) return ESP_ERR_INVALID_SIZE;
        memcpy(out, it->second.data(), it->second.size());
    }
    *len = it->second.size();
    return ESP_OK;
}
