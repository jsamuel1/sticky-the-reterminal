// pair host test — compiles the SHIPPED firmware/main/tiny/tiny_pair_util.h
// (the pure half of tiny_pair.cpp) into a host binary. Golden vectors come
// from Node's crypto + Buffer, the same calls tiny-tech enroll.js makes, so
// a pass means the board's secretHash is the one the server will check.
#include <cstdio>
#include <cstring>
#include <string>
#include "tiny/tiny_pair_util.h"

using namespace tiny_pair_util;

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } } while (0)

static std::string sha_hex(const std::string &s) {
    uint8_t d[32];
    char h[65];
    sha256(s.data(), s.size(), d);
    hex(d, 32, h);
    return h;
}

static std::string b64(const std::string &s) {
    char out[256];
    return b64url((const uint8_t *)s.data(), s.size(), out, sizeof out) >= 0 ? out : "<err>";
}

int main() {
    // --- sha256: FIPS 180-4 vectors, incl. the 56-byte two-block boundary ---
    CHECK(sha_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "S1 sha256('')");
    CHECK(sha_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "S2 sha256('abc')");
    CHECK(sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "S3 sha256 448-bit (two-block padding)");
    CHECK(sha_hex(std::string(1000, 'a')) ==
              "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "S4 sha256 1000 x 'a' (multi-block)");

    // --- base64url: Node Buffer.toString('base64url') ---
    CHECK(b64("f") == "Zg" && b64("fo") == "Zm8" && b64("foo") == "Zm9v" &&
              b64("foob") == "Zm9vYg" && b64("fooba") == "Zm9vYmE",
          "B1 RFC 4648 vectors, no padding");
    CHECK(b64("\xfb\xff\xbf") == "-_-_", "B2 url alphabet (- and _, never + /)");
    {
        uint8_t raw[32];
        for (int i = 0; i < 32; ++i) raw[i] = (uint8_t)i;
        char s[64];
        const int n = b64url(raw, 32, s, sizeof s);
        CHECK(n == 43 && kSecretChars == 43 && strlen(s) == 43, "B3 32 bytes -> 43 chars");
        CHECK(!strcmp(s, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"), "B4 Node vector 0..31");
        bool charset = true;
        for (const char *p = s; *p; ++p)
            if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", *p))
                charset = false;
        CHECK(charset && !strchr(s, '='), "B5 charset url-safe, no '='");
        char tiny[43];
        CHECK(b64url(raw, 32, tiny, sizeof tiny) == -1, "B6 short buffer refused (needs 44)");

        // --- secretHash = sha256hex(secret STRING) — enroll.js sha256hex(secret) ---
        char sh[65];
        secret_hash(s, sh);
        CHECK(!strcmp(sh, "ea866a757e4c38babfa8127cbe9a409d3e1f93a00ff1488ff735fcf917afffd0"),
              "H1 secretHash Node vector (hash of the base64url string)");
        CHECK(strlen(sh) == 64, "H2 secretHash 64 lowercase hex");
        // The tempting wrong answer: hashing the 32 raw bytes.
        uint8_t d[32];
        char wrong[65];
        sha256(raw, 32, d);
        hex(d, 32, wrong);
        CHECK(strcmp(sh, wrong) != 0, "H3 raw-bytes hash differs (would never match)");
    }

    // --- code + url ---
    CHECK(code_ok("ABC123") && code_ok("k7m2q9") && !code_ok("AB") &&
              !code_ok("ABC 123") && !code_ok("ABC\"12") && !code_ok("A/B?C=1") &&
              !code_ok(nullptr) && !code_ok("ABCDEFGHIJKLMNOPQ"),
          "C1 code charset/length gate");
    {
        char o[32];
        CHECK(code_split("ABC123", o, sizeof o) && !strcmp(o, "ABC 123"), "C2 3+3 split");
        char u[96];
        CHECK(pair_url("https://tiny.technology/", "ABC123", u, sizeof u) &&
                  !strcmp(u, "https://tiny.technology/devices/pair?code=ABC123"),
              "C3 owner URL, trailing slash dropped");
        char small[20];
        CHECK(!pair_url("https://tiny.technology", "ABC123", small, sizeof small),
              "C4 truncated URL refused");
    }

    // --- tiny_config_merge_json payload ---
    {
        char j[256];
        CHECK(merge_json("dev_1", "tind_abc", "kitchen", j, sizeof j) &&
                  !strcmp(j, "{\"device_id\":\"dev_1\",\"token\":\"tind_abc\",\"name\":\"kitchen\"}"),
              "J1 merge body id+token+name");
        CHECK(merge_json("dev_1", "tind_abc", "", j, sizeof j) &&
                  !strcmp(j, "{\"device_id\":\"dev_1\",\"token\":\"tind_abc\"}"),
              "J2 empty name omitted (merge keeps the old one)");
        CHECK(merge_json("d", "t", "a\"b\\c\n", j, sizeof j) &&
                  !strcmp(j, "{\"device_id\":\"d\",\"token\":\"t\",\"name\":\"a\\\"b\\\\c\\u000a\"}"),
              "J3 name escaped (a server string never closes the object)");
        CHECK(!merge_json("", "t", "n", j, sizeof j) && !merge_json("d", "", "n", j, sizeof j),
              "J4 empty id or token refused");
        char small[24];
        CHECK(!merge_json("dev_1", "tind_abc", "kitchen", small, sizeof small),
              "J5 no room -> refused, never truncated");
        CHECK(!strstr(j, "networks"), "J6 identity only, networks untouched");
    }

    // --- poll classification ---
    CHECK(classify(200, "approved", true) == Poll::kApproved, "P1 approved+token");
    CHECK(classify(200, "approved", false) == Poll::kRetry, "P2 approved w/o token != success");
    CHECK(classify(200, "pending", false) == Poll::kPending, "P3 pending");
    CHECK(classify(200, nullptr, false) == Poll::kPending, "P4 2xx unknown body = keep waiting");
    CHECK(classify(200, "denied", false) == Poll::kDenied && classify(403, "denied", false) == Poll::kDenied,
          "P5 denied (any status code)");
    CHECK(classify(410, "expired", false) == Poll::kRestart && classify(200, "claimed", false) == Poll::kRestart,
          "P6 expired/claimed restart");
    CHECK(classify(404, nullptr, false) == Poll::kRestart, "P7 404 restart");
    CHECK(classify(-1, nullptr, false) == Poll::kRetry && classify(503, nullptr, false) == Poll::kRetry,
          "P8 transport/5xx retry same code");
    CHECK(backoff_ms(3000) == 6000 && backoff_ms(12000) == 24000 && backoff_ms(24000) == 30000 &&
              backoff_ms(30000) == 30000 && backoff_ms(0) == 3000,
          "P9 backoff 3->6->12->24->30 s ceiling");

    printf(fails ? "pair: %d FAILED\n" : "pair: all passed\n", fails);
    return fails ? 1 : 0;
}
