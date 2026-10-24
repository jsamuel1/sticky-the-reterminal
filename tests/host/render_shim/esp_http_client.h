// render host shim: the network never answers (image/gallery = honest miss)
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
static inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *) { return nullptr; }
static inline esp_err_t esp_http_client_open(esp_http_client_handle_t, int) { return ESP_FAIL; }
static inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) { return -1; }
static inline int esp_http_client_get_status_code(esp_http_client_handle_t) { return 0; }
static inline int esp_http_client_read(esp_http_client_handle_t, char *, size_t) { return -1; }
static inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t) { return ESP_OK; }
