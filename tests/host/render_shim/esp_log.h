// render host shim: quiet unless RENDER_LOG is set at build time
#pragma once
#include <stdio.h>
#ifdef RENDER_LOG
#define ESP_LOGI(t, f, ...) fprintf(stderr, "I %s: " f "\n", t, ##__VA_ARGS__)
#else
#define ESP_LOGI(t, f, ...) do {} while (0)
#endif
#define ESP_LOGW(t, f, ...) fprintf(stderr, "W %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGE(t, f, ...) fprintf(stderr, "E %s: " f "\n", t, ##__VA_ARGS__)
