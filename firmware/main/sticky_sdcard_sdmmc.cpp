// MicroSD over the SDMMC peripheral (boards that wire the slot to SDMMC
// pins: FNK0104S 4-bit). Same contract as sticky_sdcard.h; this file
// replaces sticky_sdcard.cpp (the reTerminal's SPI-shared driver) on those
// boards, so every caller (status, `sd`, gallery cache, photo archive) runs
// unchanged.
//
// What differs from the reTerminal, from the FNK0104S 4.0" schematic:
//  - No card-detect switch (slot pin 9 is NC) and no card power switch.
//    A probe is a mount attempt, and "present" means mounted: nothing else
//    is known for sure. A card that answers but has no readable FAT
//    (factory exFAT) is remembered as such, so `sd` can still say
//    "format it".
//  - CLK/CMD/D0..D3 each have a 10K pull-up to 3V3 on the board, so the
//    internal pull-ups stay off (IDF calls them insufficient anyway).
//  - The ESP32-S3 routes SDMMC through the GPIO matrix; the pins come from
//    pin_config_<board>.h. 20 MHz default speed: through the matrix there
//    is no margin to spend on 40 MHz, and 4-bit at 20 MHz moves a 38 KB
//    frame in a few ms.
//
// Hot plug without a detect pin: ensure() checks a mounted card with CMD13
// (SEND_STATUS, one command); a yanked card stops answering and is
// unmounted. A slot that did not mount (empty, or an unreadable card) is
// re-probed from ensure() and report() at most every 2/4/8 s
// (tiny_sd_policy.h), so gallery flips and `status` on such a board do not
// each pay a probe. Probe-time IDF errors are silenced: an empty slot is the
// normal case here, and this file logs one line of its own instead.
//
// How far a probe got is read from a trace of its commands (the host's
// do_transaction is wrapped for the probe only), because the mount's return
// code is ambiguous: a data-phase error surfaces as ESP_FAIL, the same code
// as "no FAT", and a TIMEOUT is either an empty slot or a lost data block.
#include "sticky_sdcard.h"

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pin_config.h"
#include "sdmmc_cmd.h"
#include "tiny/tiny_sd_policy.h"

#if defined(PIN_SD_D3)
constexpr int kWiredWidth = 4;
#else
constexpr int kWiredWidth = 1;
#endif

namespace {

constexpr char kTag[] = "sticky_sdcard";

bool s_ready = false;
sdmmc_card_t *s_card = nullptr;      // non-null == mounted
SemaphoreHandle_t s_lock = nullptr;  // guards mount/unmount/probe state
sticky_sdcard_state_t s_state = STICKY_SD_STATE_NO_CARD;  // last probe
int s_last_good_width = 0; // bus width that last mounted (0 = none yet)
tiny_sd_trace_t s_trace = {};  // current probe attempt; written under s_lock
uint32_t s_miss_count = 0;  // consecutive probes that did not mount
uint32_t s_miss_at_ms = 0;  // when the last such probe finished

uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

size_t internal_free() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// The IDF tags an empty-slot probe prints errors under. Silenced for the
// probe only, restored afterwards.
constexpr const char *kQuietTags[] = {
    "sdmmc_init", "sdmmc_common", "sdmmc_sd", "sdmmc_cmd",
    "sdmmc_periph", "sdmmc_req", "vfs_fat_sdmmc",
};
constexpr int kQuietN = sizeof kQuietTags / sizeof kQuietTags[0];

struct QuietIdf {
    esp_log_level_t saved[kQuietN];
    QuietIdf() {
        for (int i = 0; i < kQuietN; i++) {
            saved[i] = esp_log_level_get(kQuietTags[i]);
            esp_log_level_set(kQuietTags[i], ESP_LOG_NONE);
        }
    }
    ~QuietIdf() {
        for (int i = 0; i < kQuietN; i++)
            esp_log_level_set(kQuietTags[i], saved[i]);
    }
};

// The probe's view of every command: did the card answer, did a data block
// fail. Installed in the host config for mount attempts only; a mounted
// card gets the plain host function back (mount_width).
esp_err_t traced_transaction(int slot, sdmmc_command_t *cmd) {
    const esp_err_t ret = sdmmc_host_do_transaction(slot, cmd);
    tiny_sd_trace_note(&s_trace, (cmd->flags & SCF_RSP_PRESENT) != 0,
                       cmd->data != nullptr, ret != ESP_OK ? ret : cmd->error);
    return ret;
}

// One mount attempt at one width. esp_vfs_fat_sdmmc_mount() inits the host,
// identifies the card and mounts FAT; on any failure it deinits the host
// and frees the card, so a failed attempt leaves nothing allocated.
// Caller holds s_lock (s_trace).
esp_err_t mount_width(int width, bool format, sdmmc_card_t **out) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.do_transaction = &traced_transaction;
    s_trace = {};

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = static_cast<gpio_num_t>(PIN_SD_CLK);
    slot.cmd = static_cast<gpio_num_t>(PIN_SD_CMD);
    slot.d0 = static_cast<gpio_num_t>(PIN_SD_D0);
#if defined(PIN_SD_D3)
    slot.d1 = static_cast<gpio_num_t>(PIN_SD_D1);
    slot.d2 = static_cast<gpio_num_t>(PIN_SD_D2);
    slot.d3 = static_cast<gpio_num_t>(PIN_SD_D3);
#endif
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = (uint8_t)width;
    slot.flags = 0;  // board has external 10K pull-ups on every line

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = format;  // only from the format verb
    mcfg.max_files = 4;
    mcfg.allocation_unit_size = 16 * 1024;

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(STICKY_SD_MOUNT_POINT,
                                                  &host, &slot, &mcfg, out);
    // The card keeps its own copy of the host config: file I/O after the
    // mount must not run through the probe's trace.
    if (err == ESP_OK && *out) (*out)->host.do_transaction = &sdmmc_host_do_transaction;
    return err;
}

// Caller holds s_lock. Probe the slot: widest wired width first, then 1-bit.
// Returns the last attempt's error; s_card/s_state reflect the outcome.
esp_err_t probe_locked(bool format) {
    if (s_card) return ESP_OK;
    int widths[2];
    const int nw = tiny_sd_widths(kWiredWidth, s_last_good_width, widths);
    const size_t before = internal_free();
    const uint32_t t0 = now_ms();
    esp_err_t first_err = ESP_OK, err = ESP_OK;
    bool tried_narrower = false;
    tiny_sd_probe_t outcome = TINY_SD_ABSENT;
    tiny_sd_probe_t first_outcome = TINY_SD_ABSENT;
    sdmmc_card_t *card = nullptr;
    int used = 0;
    {
        QuietIdf quiet;
        for (int i = 0; i < nw; i++) {
            card = nullptr;
            err = mount_width(widths[i], format, &card);
            outcome = tiny_sd_classify(err, s_trace);
            if (i == 0) {
                first_err = err;
                first_outcome = outcome;
            }
            used = widths[i];
            // Only a card that answered and then lost a data transfer earns
            // the narrower retry (a broken D1..D3). An unreadable FAT will
            // not read better narrower; an empty slot times out at any width.
            if (!tiny_sd_retry_narrower(err, s_trace)) break;
            tried_narrower = true;
        }
    }
    if (tried_narrower)
        outcome = tiny_sd_fallback_outcome(first_outcome, outcome);

    if (outcome == TINY_SD_MOUNTED) {
        s_state = STICKY_SD_STATE_MOUNTED;
        s_card = card;
        s_last_good_width = used;
        s_miss_count = 0;
        if (tried_narrower)
            ESP_LOGW(kTag, "%d-bit mount failed (%s); mounted %d-bit instead",
                     kWiredWidth, esp_err_to_name(first_err), used);
        ESP_LOGI(kTag,
                 "mounted %s: %s %.1f GB, %d-bit %d kHz (internal %d B, "
                 "%u B free)",
                 STICKY_SD_MOUNT_POINT, card->cid.name,
                 (double)card->csd.capacity * card->csd.sector_size / 1e9,
                 used, card->real_freq_khz,
                 (int)internal_free() - (int)before,
                 (unsigned)internal_free());
        return ESP_OK;
    }
    if (tried_narrower)
        ESP_LOGW(kTag, "%d-bit mount failed (%s); %d-bit fallback also "
                       "failed (%s)",
                 kWiredWidth, esp_err_to_name(first_err), used,
                 esp_err_to_name(err));
    if (outcome == TINY_SD_UNREADABLE) {
        const bool no_fat = tiny_sd_fat_suspect(err, s_trace);
        const sticky_sdcard_state_t st =
            no_fat ? STICKY_SD_STATE_NO_FAT : STICKY_SD_STATE_UNREADABLE;
        if (s_miss_count == 0 || s_state != st) {
            if (no_fat)
                ESP_LOGW(kTag, "card answered but FAT did not mount (%s): "
                               "exFAT? `sd format yes` makes it FAT32",
                         esp_err_to_name(err));
            else if (tried_narrower)
                ESP_LOGW(kTag, "card answered at %d-bit but the %d-bit "
                               "fallback did not mount (%s): reseat the card",
                         kWiredWidth, used, esp_err_to_name(err));
            else
                ESP_LOGW(kTag, "card answered but a %d-bit transfer failed "
                               "(%s): reseat the card",
                         used, esp_err_to_name(err));
        }
        s_state = st;
        s_miss_count++;
        s_miss_at_ms = now_ms();
        return err;
    }
    if (outcome == TINY_SD_ERROR) {
        if (s_miss_count == 0 || s_state != STICKY_SD_STATE_PROBE_ERROR)
            ESP_LOGE(kTag, "sd probe failed before mount (%s)",
                     esp_err_to_name(err));
        s_state = STICKY_SD_STATE_PROBE_ERROR;
        s_miss_count++;
        s_miss_at_ms = now_ms();
        return err;
    }
    // Empty slot: one quiet line the first time, silence while it stays so.
    s_state = STICKY_SD_STATE_NO_CARD;
    if (s_miss_count == 0)
        ESP_LOGI(kTag, "no card in slot (%s, probe %u ms)", esp_err_to_name(err),
                 (unsigned)(now_ms() - t0));
    s_miss_count++;
    s_miss_at_ms = now_ms();
    return ESP_ERR_NOT_FOUND;
}

// Caller holds s_lock.
esp_err_t unmount_locked() {
    if (!s_card) return ESP_OK;
    const esp_err_t err =
        esp_vfs_fat_sdcard_unmount(STICKY_SD_MOUNT_POINT, s_card);
    s_card = nullptr;
    ESP_LOGI(kTag, "unmounted (%s)", esp_err_to_name(err));
    return err;
}

}  // namespace

extern "C" esp_err_t sticky_sdcard_init(void) {
    if (s_ready) return ESP_OK;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    // First probe. An empty slot is a clean skip, not an error. s_ready is
    // published only after it, so no other task can race the boot probe.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    probe_locked(false);
    xSemaphoreGive(s_lock);
    s_ready = true;
    return ESP_OK;
}

extern "C" bool sticky_sdcard_present(void) {
    // No detect switch: the only thing known for sure is a mounted card.
    return s_ready && s_card != nullptr;
}

extern "C" bool sticky_sdcard_mounted(void) {
    return s_card != nullptr;
}

namespace {
// Caller holds s_lock. The hot-plug reconcile shared by ensure() and
// report(): CMD13 on a mounted card, a backed-off re-probe otherwise.
esp_err_t reconcile_locked() {
    if (s_card) {
        // Still there? CMD13 is one command; a yanked card cannot answer.
        const esp_err_t err = sdmmc_get_status(s_card);
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(kTag, "card stopped answering (%s): unmounting",
                 esp_err_to_name(err));
        unmount_locked();
        s_state = STICKY_SD_STATE_NO_CARD;
        s_miss_count = 1;
        s_miss_at_ms = now_ms();
        return ESP_ERR_NOT_FOUND;
    }
    if (tiny_sd_probe_due(now_ms(), s_miss_at_ms, s_miss_count)) {
        // Empty slot or unreadable card, on backoff: a gallery flip must not
        // pay a full identify each time. NOT_FOUND when the slot is empty.
        return probe_locked(false);
    }
    return s_state == STICKY_SD_STATE_NO_CARD ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}
}  // namespace

extern "C" esp_err_t sticky_sdcard_ensure(void) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = reconcile_locked();
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" void sticky_sdcard_report(sticky_sdcard_report_t *out) {
    if (!out) return;
    *out = {};
    out->state = STICKY_SD_STATE_NO_CARD;
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reconcile_locked();
    out->mounted = s_card != nullptr;
    out->present = out->mounted;  // no detect pin: present == mounted
    out->state = out->mounted ? STICKY_SD_STATE_MOUNTED : s_state;
    if (out->mounted &&
        esp_vfs_fat_info(STICKY_SD_MOUNT_POINT, &out->total_bytes,
                         &out->free_bytes) != ESP_OK) {
        out->total_bytes = out->free_bytes = 0;
    }
    xSemaphoreGive(s_lock);
}

extern "C" esp_err_t sticky_sdcard_mount(void) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = probe_locked(false);  // explicit: no backoff
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" esp_err_t sticky_sdcard_unmount(void) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = unmount_locked();
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" esp_err_t sticky_sdcard_info(uint64_t *total_bytes,
                                        uint64_t *free_bytes) {
    if (!s_card) return ESP_ERR_INVALID_STATE;
    uint64_t total = 0, free_b = 0;
    const esp_err_t err =
        esp_vfs_fat_info(STICKY_SD_MOUNT_POINT, &total, &free_b);
    if (err != ESP_OK) return err;
    if (total_bytes) *total_bytes = total;
    if (free_bytes) *free_bytes = free_b;
    return ESP_OK;
}

extern "C" esp_err_t sticky_sdcard_format(void) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unmount_locked();
    ESP_LOGW(kTag, "FORMAT: wiping card to FAT32 (this takes a while)");
    // Mount with format_if_mount_failed (the one sanctioned use), then
    // format explicitly: the mount may have found a valid FAT and the
    // verb's contract is "wipe".
    esp_err_t err = probe_locked(true);
    if (err == ESP_OK) {
        err = esp_vfs_fat_sdcard_format(STICKY_SD_MOUNT_POINT, s_card);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "explicit format failed: %s", esp_err_to_name(err));
            unmount_locked();
        } else {
            ESP_LOGI(kTag, "formatted FAT32 and mounted");
        }
    } else {
        ESP_LOGE(kTag, "format/mount failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

extern "C" void sticky_sdcard_prepare_sleep(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unmount_locked();  // no card power switch on this board: unmount only
    xSemaphoreGive(s_lock);
}
