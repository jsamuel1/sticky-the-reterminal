#pragma once

#include "esp_err.h"

#define ESP_RETURN_ON_ERROR(expr, tag, msg) \
    do {                                     \
        const esp_err_t _err = (expr);       \
        if (_err != ESP_OK) return _err;     \
    } while (0)
