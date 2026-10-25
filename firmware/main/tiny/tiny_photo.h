// tiny_photo — message photos: fetch from the media store, decode to luma.
// Device half in tiny_photo.cpp (ESP32-S3 ROM TJpgDec); pure math in
// tiny_photo_util.h. The render host test supplies its own tiny_photo_load.
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Fetch `url` (https://plugin.tiny.technology/media/<key> only) and decode it
// to 8-bit luma, contain-fitted into box_w x box_h (enlarged to fill the box
// only when `allow_up`). *out is PSRAM the caller releases with
// heap_caps_free. Blocks on HTTPS and the decode: never call it with the
// render or display lock held.
//   ESP_ERR_INVALID_ARG    url not on the media store
//   ESP_ERR_NOT_FOUND      HTTP 404 (the platform lost the file)
//   ESP_ERR_NOT_SUPPORTED  not a baseline JPEG (progressive, PNG, video...)
//   ESP_ERR_INVALID_SIZE   over the byte or pixel cap
//   ESP_ERR_TIMEOUT        the fetch ran past its wall-clock cap
esp_err_t tiny_photo_load(const char *url, int box_w, int box_h, bool allow_up,
                          uint8_t **out, int *out_w, int *out_h);
#ifdef __cplusplus
}
#endif
