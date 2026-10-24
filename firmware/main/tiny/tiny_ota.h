// tiny_ota — fetch, verify, stage. NEVER judge: tiny_node marks the image valid.
// PUT /api/firmware/manifest {deviceId,token,channel} -> {bundle:{url,sha256}}
// -> manifest -> sha256-checked download -> esp_ota to next slot -> reboot into
// trial (rollback-armed). Artifact hosts pinned: tiny.technology,
// plugin.tiny.technology (measured: media lives on a DIFFERENT host than api).
// Every stage checks the image's board stamp (tiny/tiny_ota_util.h) first.
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t tiny_ota_check_and_stage(const char *channel);  // returns after staging; reboot is caller's call
// Stage a specific artifact: url must be https://plugin.tiny.technology/media/<key>,
// sha256 64 hex. No version ordering (the pusher proves ancestry); board stamp,
// sha256 and trial-boot rollback still apply. ESP_ERR_INVALID_ARG = bad url/sha,
// ESP_ERR_INVALID_RESPONSE = board guard refused, ESP_ERR_INVALID_CRC = sha mismatch.
esp_err_t tiny_ota_stage_url(const char *url, const char *sha256);
#ifdef __cplusplus
}
#endif
