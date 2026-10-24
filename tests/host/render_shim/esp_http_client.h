// render host shim: tests can hold one successful image fetch to prove a
// newer card overtakes it; every other request is an honest miss.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef void *esp_http_client_handle_t;
typedef struct {
    const char *url;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void *);
} esp_http_client_config_t;

esp_http_client_handle_t render_test_http_init(const esp_http_client_config_t *);
esp_err_t render_test_http_open(esp_http_client_handle_t, int);
int64_t render_test_http_fetch_headers(esp_http_client_handle_t);
int render_test_http_status(esp_http_client_handle_t);
int render_test_http_read(esp_http_client_handle_t, char *, size_t);
esp_err_t render_test_http_cleanup(esp_http_client_handle_t);

static inline esp_http_client_handle_t esp_http_client_init(
    const esp_http_client_config_t *config) {
    return render_test_http_init(config);
}
static inline esp_err_t esp_http_client_open(esp_http_client_handle_t handle,
                                             int length) {
    return render_test_http_open(handle, length);
}
static inline int64_t esp_http_client_fetch_headers(
    esp_http_client_handle_t handle) {
    return render_test_http_fetch_headers(handle);
}
static inline int esp_http_client_get_status_code(
    esp_http_client_handle_t handle) {
    return render_test_http_status(handle);
}
static inline int esp_http_client_read(esp_http_client_handle_t handle,
                                       char *out, size_t length) {
    return render_test_http_read(handle, out, length);
}
static inline esp_err_t esp_http_client_cleanup(
    esp_http_client_handle_t handle) {
    return render_test_http_cleanup(handle);
}
