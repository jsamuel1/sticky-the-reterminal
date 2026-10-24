// ota host test — compiles the SHIPPED firmware/main/tiny/tiny_ota_util.h
// (board-stamp parser, media URL + sha256 guards) into a host binary.
// Fixtures are synthetic images laid out exactly as IDF links them
// (image header, segment header, esp_app_desc_t, then the stamp). When a
// real build exists, ota_host_test.sh also feeds its .bin through
// tools/ota_push.py --inspect so the Python twin is checked on real bytes.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "tiny/tiny_ota_util.h"

using namespace tiny_ota_util;

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } } while (0)

static std::vector<uint8_t> image(const char *board, const char *project = "tiny_sticky",
                                  bool stamped = true) {
    std::vector<uint8_t> b(4096, 0x5a);
    b[0] = 0xE9;
    memset(&b[kDescOff], 0, kDescLen);
    const uint32_t m = kDescMagic;
    for (int i = 0; i < 4; ++i) b[kDescOff + i] = (uint8_t)(m >> (8 * i));
    memcpy(&b[kDescOff + 16], "0.32.0", 6);
    strncpy((char *)&b[kProjOff], project, 32);
    if (stamped) {
        tiny_board_stamp_t s = {};
        memcpy(s.magic, TINY_BOARD_STAMP_MAGIC, 8);
        strncpy(s.board, board, sizeof s.board);
        memcpy(&b[kStampOff], &s, sizeof s);
    }
    return b;
}

static Verdict judge(const std::vector<uint8_t> &b, const char *want, size_t len = 0,
                     char *out = nullptr) {
    char tmp[20];
    return parse_image(b.data(), len ? len : b.size(), "tiny_sticky", want, out ? out : tmp,
                       sizeof tmp);
}

int main(int argc, char **argv) {
    // --- layout is the one IDF's sections.ld.in produces ---
    CHECK(kDescOff == 32 && kStampOff == 288 && kHeadLen == 320, "L1 stamp at image offset 288");

    // --- own board accepted, id reported ---
    char got[20];
    CHECK(judge(image("fnk0104s"), "fnk0104s", 0, got) == IMG_OK && !strcmp(got, "fnk0104s"),
          "B1 fnk0104s image on fnk0104s");
    CHECK(judge(image("reterminal"), "reterminal") == IMG_OK, "B2 reterminal on reterminal");
    CHECK(judge(image("amoled18"), "amoled18") == IMG_OK, "B3 amoled18 on amoled18");

    // --- foreign board refused, and the refusal names the image's board ---
    CHECK(judge(image("amoled18"), "fnk0104s", 0, got) == IMG_WRONG_BOARD &&
              !strcmp(got, "amoled18"),
          "F1 amoled18 image on fnk0104s refused");
    CHECK(judge(image("reterminal"), "fnk0104s") == IMG_WRONG_BOARD, "F2 reterminal on fnk refused");
    CHECK(judge(image("fnk0104"), "fnk0104s") == IMG_WRONG_BOARD, "F3 prefix id refused");
    CHECK(judge(image("fnk0104s"), "fnk0104") == IMG_WRONG_BOARD, "F4 longer id refused");

    // --- unstamped (every pre-0.32 build) refused ---
    CHECK(judge(image("", "tiny_sticky", false), "fnk0104s") == IMG_UNSTAMPED,
          "U1 unstamped image refused");

    // --- truncated: anything shorter than the head ---
    const auto ok = image("fnk0104s");
    CHECK(judge(ok, "fnk0104s", kHeadLen - 1) == IMG_TRUNCATED, "T1 319 bytes refused");
    CHECK(judge(ok, "fnk0104s", 24) == IMG_TRUNCATED, "T2 header only refused");
    CHECK(judge(ok, "fnk0104s", kHeadLen) == IMG_OK, "T3 exactly the head is enough");

    // --- not an app image ---
    auto bad = ok; bad[0] = 0x00;
    CHECK(judge(bad, "fnk0104s") == IMG_NOT_ESP, "N1 no 0xE9 refused");
    bad = ok; bad[kDescOff] ^= 0xff;
    CHECK(judge(bad, "fnk0104s") == IMG_NOT_ESP, "N2 no app desc magic refused");
    CHECK(judge(image("fnk0104s", "other_app"), "fnk0104s") == IMG_WRONG_PROJECT,
          "N3 other project refused");

    // --- malformed stamp ---
    bad = ok; bad[kStampOff + 8] = 0;  // empty board
    CHECK(judge(bad, "fnk0104s") == IMG_BAD_STAMP, "M1 empty board refused");
    bad = ok; memset(&bad[kStampOff + 8], 'a', 16);  // no NUL
    CHECK(judge(bad, "fnk0104s") == IMG_BAD_STAMP, "M2 unterminated board refused");
    bad = ok; bad[kStampOff + 8 + 3] = 'X';  // uppercase
    CHECK(judge(bad, "fnk0104s") == IMG_BAD_STAMP, "M3 bad char refused");
    bad = ok; bad[kStampOff + 8 + 12] = 'z';  // junk after NUL
    CHECK(judge(bad, "fnk0104s") == IMG_BAD_STAMP, "M4 junk after NUL refused");

    // --- media URL guard ---
    CHECK(media_url_ok("https://plugin.tiny.technology/media/8363431e-182f-4373-99c1-24c73991488b.bin"),
          "U2 media url accepted");
    CHECK(!media_url_ok("http://plugin.tiny.technology/media/a.bin"), "U3 http refused");
    CHECK(!media_url_ok("https://tiny.technology/media/a.bin"), "U4 api host refused");
    CHECK(!media_url_ok("https://plugin.tiny.technology.evil.com/media/a.bin"), "U5 suffix host refused");
    CHECK(!media_url_ok("https://plugin.tiny.technology/mediax/a.bin"), "U6 other path refused");
    CHECK(!media_url_ok("https://plugin.tiny.technology/media/"), "U7 empty key refused");
    CHECK(!media_url_ok("https://plugin.tiny.technology/media/a/b.bin"), "U8 nested path refused");
    CHECK(!media_url_ok("https://plugin.tiny.technology/media/..bin"), "U9 dot-dot refused");
    CHECK(!media_url_ok("https://plugin.tiny.technology/media/a.bin?x=1"), "U10 query refused");
    const std::string longkey = std::string(kMediaPrefix) + std::string(200, 'a');
    CHECK(!media_url_ok(longkey.c_str()), "U11 overlong key refused");
    CHECK(!media_url_ok(nullptr), "U12 null refused");

    // --- verb dispatch: a scheme or second arg selects the URL form ---
    CHECK(ota_args_are_direct("https://plugin.tiny.technology/media/a.bin", 2),
          "D1 https + sha is direct form");
    CHECK(ota_args_are_direct("http://plugin.tiny.technology/media/a.bin", 2),
          "D2 http is direct form");
    CHECK(ota_args_are_direct("ftp://x/a.bin", 2), "D3 ftp is direct form");
    CHECK(!ota_args_are_direct("sticky-dev", 1), "D4 one arg is channel form");
    CHECK(!ota_args_are_direct("", 0), "D5 empty is channel form (default)");
    CHECK(!media_url_ok("http://plugin.tiny.technology/media/a.bin") &&
              ota_args_are_direct("http://plugin.tiny.technology/media/a.bin", 2),
          "D6 http url reaches the guard and is refused");
    CHECK(ota_args_are_direct("plugin.tiny.technology/media/a.bin", 2),
          "D7 malformed URL + sha reaches the guard and is refused");
    CHECK(ota_args_are_direct("sticky-dev", 2),
          "D8 channel + extra arg reaches direct guard instead of ignoring arg");

    // --- sha256 guard ---
    const std::string h64(64, 'a');
    CHECK(sha256_hex_ok(h64.c_str()), "H1 64 hex accepted");
    CHECK(sha256_hex_ok("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"),
          "H2 upper hex accepted");
    CHECK(!sha256_hex_ok(std::string(63, 'a').c_str()), "H3 63 refused");
    CHECK(!sha256_hex_ok(std::string(65, 'a').c_str()), "H4 65 refused");
    CHECK(!sha256_hex_ok((std::string(63, 'a') + "g").c_str()), "H5 non-hex refused");

    printf(fails ? "%d FAILED\n" : "all ota cases passed\n", fails);
    // Fixtures for the Python twin (tools/ota_push.py --inspect): the shell
    // driver checks it reaches the same verdict on the same bytes.
    if (argc > 1) {
        struct { const char *name; std::vector<uint8_t> b; size_t len; } fx[] = {
            {"own", image("fnk0104s"), 0},
            {"foreign", image("amoled18"), 0},
            {"unstamped", image("", "tiny_sticky", false), 0},
            {"truncated", image("fnk0104s"), kHeadLen - 1},
        };
        for (auto &f : fx) {
            const std::string p = std::string(argv[1]) + "/" + f.name + ".bin";
            FILE *o = fopen(p.c_str(), "wb");
            if (!o) return 2;
            fwrite(f.b.data(), 1, f.len ? f.len : f.b.size(), o);
            fclose(o);
        }
    }
    return fails ? 1 : 0;
}
