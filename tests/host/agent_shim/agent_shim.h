// agent_shim — force-included ahead of firmware/main/tiny_agent.cpp by
// agent_host_test.sh. NVS is the file-backed store in ../shim/nvs.h (a
// PROCESS RESTART is a reboot). Adds what that shim does not carry — the
// blob calls, ESP_ERR_NVS_NOT_FOUND, strlcpy — and FAULT INJECTION on the
// calls a save makes (open read-write, set_blob, commit, erase), so the test
// can prove a refused save changes nothing in RAM, on flash or after reboot.
//
// Like the device's NVS, a set is durable the moment it returns (the shim
// flushes the file on every set): a commit refused after a successful set
// leaves the NEW bytes behind unless the caller puts the old ones back.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "esp_err.h"
#include "nvs.h"

#if !defined(AGENT_SHIM_HAVE_STRLCPY) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38))
#define AGENT_SHIM_HAVE_STRLCPY 1
#endif
#ifndef AGENT_SHIM_HAVE_STRLCPY
static inline size_t strlcpy(char *d, const char *s, size_t n) {
    const size_t l = strlen(s);
    if (n) { const size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
#endif

#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0c)

namespace agent_shim {
enum Op { kOpenRw, kSetBlob, kCommit, kErase, kOps };
// fail[op] = how many upcoming calls of that op fail (ESP_FAIL).
inline int *fail() { static int f[kOps] = {0, 0, 0, 0}; return f; }
inline int *calls() { static int c[kOps] = {0, 0, 0, 0}; return c; }
inline bool take(Op op) {
    ++calls()[op];
    if (fail()[op] > 0) { --fail()[op]; return true; }
    return false;
}
inline void reset() { for (int i = 0; i < kOps; ++i) fail()[i] = 0; }
// Blobs are stored hex-encoded so binary bytes never touch the line format.
inline std::string hex(const void *v, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    const auto *p = static_cast<const uint8_t *>(v);
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
inline std::string unhex(const std::string &s) {
    std::string out;
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out += static_cast<char>((nib(s[i]) << 4) | nib(s[i + 1]));
    return out;
}
inline std::string key(const char *k) { return nvs_shim::ns() + "." + k; }
}  // namespace agent_shim

static inline esp_err_t agent_shim_open(const char *name, nvs_open_mode_t m,
                                        nvs_handle_t *h) {
    if (m == NVS_READWRITE && agent_shim::take(agent_shim::kOpenRw))
        return ESP_FAIL;
    return nvs_open(name, m, h);
}
#define nvs_open agent_shim_open

static inline esp_err_t agent_shim_commit(nvs_handle_t h) {
    if (agent_shim::take(agent_shim::kCommit)) return ESP_FAIL;
    return nvs_commit(h);
}
#define nvs_commit agent_shim_commit

static inline esp_err_t agent_shim_erase(nvs_handle_t, const char *k) {
    if (agent_shim::take(agent_shim::kErase)) return ESP_FAIL;
    auto &m = nvs_shim::store();
    if (!m.erase(agent_shim::key(k))) return ESP_ERR_NVS_NOT_FOUND;
    nvs_shim::flush();
    return ESP_OK;
}
#define nvs_erase_key agent_shim_erase

static inline esp_err_t nvs_set_blob(nvs_handle_t, const char *k,
                                     const void *v, size_t len) {
    if (agent_shim::take(agent_shim::kSetBlob)) return ESP_FAIL;
    nvs_shim::store()[agent_shim::key(k)] = "x" + agent_shim::hex(v, len);
    nvs_shim::flush();   // durable on return, like the device
    return ESP_OK;
}
static inline esp_err_t nvs_get_blob(nvs_handle_t, const char *k, void *out,
                                     size_t *len) {
    auto &m = nvs_shim::store();
    auto it = m.find(agent_shim::key(k));
    if (it == m.end()) return ESP_ERR_NVS_NOT_FOUND;
    const std::string b = agent_shim::unhex(it->second.substr(1));
    if (out) {
        if (b.size() > *len) return ESP_ERR_NVS_INVALID_LENGTH;
        memcpy(out, b.data(), b.size());
    }
    *len = b.size();
    return ESP_OK;
}
