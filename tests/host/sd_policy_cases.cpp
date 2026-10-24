// sd policy host test — the SHIPPED firmware/main/tiny/tiny_sd_policy.h:
// how a probe's command trace reads (absent / unreadable / mounted), when a
// 4-bit failure earns a 1-bit retry, when "no FAT" is the honest hint, which
// widths a probe tries, and the empty-slot re-probe backoff (wrap-safe).
//
// The traces below are the command sequences the pinned IDF v5.4 issues
// (sdmmc_card_init + f_mount), with the codes its host layer reports:
// sdmmc_transaction.c process_data_status() maps an unclassified data error
// to ESP_FAIL and a data timeout to ESP_ERR_TIMEOUT, so neither code alone
// says whether the card answered.
#include "tiny_sd_policy.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

struct Cmd { bool rsp; bool data; esp_err_t res; };

template <int N>
static tiny_sd_trace_t run(const Cmd (&cmds)[N])
{
    tiny_sd_trace_t t = {};
    for (int i = 0; i < N; i++) tiny_sd_trace_note(&t, cmds[i].rsp, cmds[i].data, cmds[i].res);
    return t;
}

// Empty slot: CMD52 reset (timeout), CMD0 (no response), CMD8 timeout,
// CMD5 timeout, ACMD41's CMD55 timing out until the error budget runs out.
static const Cmd kEmpty[] = {
    {true, false, ESP_ERR_TIMEOUT}, {false, false, ESP_OK},
    {true, false, ESP_ERR_TIMEOUT}, {true, false, ESP_ERR_TIMEOUT},
    {true, false, ESP_ERR_TIMEOUT}, {true, false, ESP_ERR_TIMEOUT},
};
// Card identifies (CMD8/ACMD41/CID/RCA/CSD/select answer), then a data read
// (SCR, or a sector) fails with an unclassified data error -> ESP_FAIL.
static const Cmd kDataFail[] = {
    {false, false, ESP_OK}, {true, false, ESP_OK}, {true, false, ESP_OK},
    {true, false, ESP_OK}, {true, true, ESP_FAIL},
};
// Same, but the data block never arrives -> data TIMEOUT.
static const Cmd kDataTimeout[] = {
    {false, false, ESP_OK}, {true, false, ESP_OK}, {true, false, ESP_OK},
    {true, true, ESP_ERR_TIMEOUT},
};
// Factory exFAT: identification and every sector read succeed; f_mount
// finds no FAT and the mount returns ESP_FAIL with no transaction failing.
static const Cmd kExfat[] = {
    {false, false, ESP_OK}, {true, false, ESP_OK}, {true, true, ESP_OK},
    {true, true, ESP_OK},
};
// Card answers CMD8, then a later response (no data) comes back with a CRC.
static const Cmd kCmdCrc[] = {
    {false, false, ESP_OK}, {true, false, ESP_OK},
    {true, false, ESP_ERR_INVALID_CRC},
};

static void trace_note()
{
    tiny_sd_trace_t t = {};
    tiny_sd_trace_note(&t, false, false, ESP_OK);  // CMD0: no response
    CHECK(!t.answered && !t.data_failed);
    tiny_sd_trace_note(&t, true, false, ESP_ERR_TIMEOUT);  // no reply
    CHECK(!t.answered && !t.data_failed);
    tiny_sd_trace_note(&t, true, false, ESP_OK);  // a reply
    CHECK(t.answered && !t.data_failed);
    tiny_sd_trace_note(&t, true, true, ESP_FAIL);  // data error
    CHECK(t.answered && t.data_failed);
    tiny_sd_trace_t u = {};
    tiny_sd_trace_note(&u, true, true, ESP_OK);  // a good data transfer
    CHECK(u.answered && !u.data_failed);
}

static void classify()
{
    const tiny_sd_trace_t none = {};
    CHECK(tiny_sd_classify(ESP_OK, run(kExfat)) == TINY_SD_MOUNTED);
    CHECK(tiny_sd_classify(ESP_ERR_TIMEOUT, run(kEmpty)) == TINY_SD_ABSENT);
    CHECK(tiny_sd_classify(ESP_ERR_NO_MEM, none) == TINY_SD_ERROR);
    // ESP_FAIL without a transaction is a probe fault, not missing media.
    CHECK(tiny_sd_classify(ESP_FAIL, none) == TINY_SD_ERROR);
    CHECK(tiny_sd_classify(ESP_FAIL, run(kExfat)) == TINY_SD_UNREADABLE);
    CHECK(tiny_sd_classify(ESP_FAIL, run(kDataFail)) == TINY_SD_UNREADABLE);
    // TIMEOUT is not "empty slot" by itself: here the card answered.
    CHECK(tiny_sd_classify(ESP_ERR_TIMEOUT, run(kDataTimeout)) == TINY_SD_UNREADABLE);
    CHECK(tiny_sd_classify(ESP_ERR_INVALID_CRC, run(kCmdCrc)) == TINY_SD_ERROR);
    CHECK(tiny_sd_classify(ESP_ERR_INVALID_ARG, run(kExfat)) == TINY_SD_ERROR);
    CHECK(tiny_sd_classify(ESP_ERR_INVALID_ARG, run(kDataFail)) == TINY_SD_ERROR);
    CHECK(tiny_sd_classify(ESP_ERR_INVALID_SIZE, run(kDataFail)) == TINY_SD_ERROR);
    CHECK(tiny_sd_classify(ESP_ERR_INVALID_STATE, run(kDataFail)) == TINY_SD_ERROR);
}

static void retry_narrower()
{
    const tiny_sd_trace_t none = {};
    CHECK(!tiny_sd_retry_narrower(ESP_OK, run(kExfat)));
    CHECK(!tiny_sd_retry_narrower(ESP_ERR_TIMEOUT, run(kEmpty)));  // one probe
    CHECK(!tiny_sd_retry_narrower(ESP_FAIL, run(kExfat)));  // FAT, not wires
    CHECK(!tiny_sd_retry_narrower(ESP_ERR_INVALID_CRC, run(kCmdCrc)));  // CMD line
    CHECK(!tiny_sd_retry_narrower(ESP_ERR_NO_MEM, none));  // heap, not wires
    CHECK(!tiny_sd_retry_narrower(ESP_ERR_NO_MEM, run(kDataFail)));
    CHECK(!tiny_sd_retry_narrower(ESP_ERR_INVALID_ARG, run(kDataFail)));
    CHECK(!tiny_sd_retry_narrower(ESP_ERR_INVALID_SIZE, run(kDataFail)));
    CHECK(!tiny_sd_retry_narrower(ESP_ERR_INVALID_STATE, run(kDataFail)));
    // A signal/protocol data failure after the card answered can be broken
    // D1..D3, including the two ambiguous codes IDF emits.
    CHECK(tiny_sd_retry_narrower(ESP_FAIL, run(kDataFail)));
    CHECK(tiny_sd_retry_narrower(ESP_ERR_TIMEOUT, run(kDataTimeout)));
    tiny_sd_trace_t crc = run(kExfat);
    tiny_sd_trace_note(&crc, true, true, ESP_ERR_INVALID_CRC);
    CHECK(tiny_sd_retry_narrower(ESP_ERR_INVALID_CRC, crc));
    CHECK(tiny_sd_retry_narrower(ESP_ERR_INVALID_RESPONSE, crc));

    CHECK(tiny_sd_fallback_outcome(TINY_SD_UNREADABLE, TINY_SD_ABSENT) ==
          TINY_SD_UNREADABLE);
    CHECK(tiny_sd_fallback_outcome(TINY_SD_UNREADABLE, TINY_SD_ERROR) ==
          TINY_SD_ERROR);
    CHECK(tiny_sd_fallback_outcome(TINY_SD_UNREADABLE, TINY_SD_MOUNTED) ==
          TINY_SD_MOUNTED);
}

static void fat_suspect()
{
    CHECK(tiny_sd_fat_suspect(ESP_FAIL, run(kExfat)));
    CHECK(!tiny_sd_fat_suspect(ESP_FAIL, run(kDataFail)));  // a transfer failed
    CHECK(!tiny_sd_fat_suspect(ESP_FAIL, tiny_sd_trace_t{}));  // nothing answered
    CHECK(!tiny_sd_fat_suspect(ESP_ERR_TIMEOUT, run(kExfat)));
    CHECK(!tiny_sd_fat_suspect(ESP_OK, run(kExfat)));
}

static void widths()
{
    int w[2] = {0, 0};
    CHECK(tiny_sd_widths(4, 0, w) == 2 && w[0] == 4 && w[1] == 1);  // first probe
    CHECK(tiny_sd_widths(4, 4, w) == 2 && w[0] == 4 && w[1] == 1);
    CHECK(tiny_sd_widths(4, 1, w) == 1 && w[0] == 1);  // 1-bit is what works
    CHECK(tiny_sd_widths(1, 0, w) == 1 && w[0] == 1);  // AMOLED-style 1-bit slot
    CHECK(tiny_sd_widths(1, 4, w) == 1 && w[0] == 1);
}

static void backoff()
{
    CHECK(tiny_sd_backoff_ms(1) == 2000);
    CHECK(tiny_sd_backoff_ms(2) == 4000);
    CHECK(tiny_sd_backoff_ms(3) == 8000);
    CHECK(tiny_sd_backoff_ms(1000) == 8000);  // capped: a new card seen in 8 s

    CHECK(tiny_sd_probe_due(5, 0, 0));  // never probed: due now
    CHECK(!tiny_sd_probe_due(1999, 0, 1));
    CHECK(tiny_sd_probe_due(2000, 0, 1));
    CHECK(!tiny_sd_probe_due(13999, 10000, 2));
    CHECK(tiny_sd_probe_due(14000, 10000, 2));
    CHECK(!tiny_sd_probe_due(17999, 10000, 9));
    CHECK(tiny_sd_probe_due(18000, 10000, 9));
    // uint32 ms clock wraps after ~49.7 days of uptime.
    CHECK(!tiny_sd_probe_due(1000u, 0xFFFFFC19u, 1));  // 1999 ms elapsed
    CHECK(tiny_sd_probe_due(1000u, 0xFFFFFC18u, 1));   // 2000 ms elapsed
    CHECK(!tiny_sd_probe_due(100u, 0xFFFFFF00u, 3));   // 356 ms elapsed
}

int main()
{
    trace_note();
    classify();
    retry_narrower();
    fat_suspect();
    widths();
    backoff();
    if (fails) { printf("sd policy: %d FAILED\n", fails); return 1; }
    printf("sd policy: all checks passed\n");
    return 0;
}
