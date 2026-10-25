// envelope host test — the SHIPPED firmware/main/tiny_envelope.cpp against
// the device's vendored cJSON: every payload form gets a description, and no
// value under a credential-looking key ever reaches the log line.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "tiny/tiny_envelope.h"

static void expect(bool ok, const char *what, const char *got) {
    if (!ok) {
        fprintf(stderr, "FAIL %s\n  got: %s\n", what, got);
        std::exit(1);
    }
    printf("ok   %s\n", what);
}

static void describe_json(const char *msg_json, char *out, size_t cap) {
    cJSON *m = cJSON_Parse(msg_json);
    assert(m);
    tiny_envelope_describe(cJSON_GetObjectItem(m, "payload"), out, cap);
    cJSON_Delete(m);
}

int main() {
    char d[320];

    describe_json(R"({"id":"a"})", d, sizeof d);
    expect(!strcmp(d, "payload=absent"), "missing payload", d);

    describe_json(R"({"id":"a","payload":null})", d, sizeof d);
    expect(!strcmp(d, "payload=null"), "null payload", d);

    // The likely flood shape: a typed envelope that is not an invoke.
    describe_json(R"({"id":"a","payload":"{\"type\":\"notify\",\"title\":\"hi\"}"})", d, sizeof d);
    expect(strstr(d, "payload=string ") && strstr(d, "type=\"notify\"") &&
               strstr(d, "prompt=absent") && strstr(d, "keys=[type,title]") &&
               strstr(d, "preview=\"{\"type\":\"notify\""),
           "string payload: type, keys, preview", d);

    describe_json(R"({"id":"a","payload":{"type":"invoke","prompt":7}})", d, sizeof d);
    expect(strstr(d, "payload=object") && strstr(d, "prompt=non-string"),
           "object payload, non-string prompt", d);

    // Credentials: values under token/secret/auth-ish keys never print, at any depth.
    describe_json(
        R"({"payload":"{\"type\":\"x\",\"token\":\"SEKRIT1\",\"inner\":{\"apiKey\":\"SEKRIT2\",\"ok\":1},\"Authorization\":\"Bearer SEKRIT3\",\"list\":[{\"session_id\":\"SEKRIT4\"}]}"})",
        d, sizeof d);
    expect(!strstr(d, "SEKRIT") && strstr(d, "<redacted>") && strstr(d, "\"ok\":1"),
           "credential values redacted at every depth", d);

    // A non-object string may itself be a credential: shape only, no bytes.
    describe_json(R"({"payload":"eyJhbGciOiJIUzI1NiJ9.SEKRIT5.sig"})", d, sizeof d);
    expect(!strstr(d, "SEKRIT") && strstr(d, "json=invalid"), "raw string payload: no bytes", d);
    describe_json(R"({"payload":"[1,2]"})", d, sizeof d);
    expect(strstr(d, "json=array") != nullptr, "array-in-string payload", d);

    // Long previews are cut and marked; control bytes are neutralised.
    char big[600];
    snprintf(big, sizeof big, R"({"payload":{"type":"t","blob":"%0400d\n"}})", 0);
    describe_json(big, d, sizeof d);
    expect(strstr(d, "…") != nullptr && strlen(d) < sizeof d, "long preview cut", d);

    // Small buffers stay terminated; never overrun.
    char tiny[24];
    memset(tiny, 'X', sizeof tiny);
    describe_json(R"({"payload":{"type":"notify","title":"a long title here"}})", tiny, sizeof tiny);
    expect(memchr(tiny, 0, sizeof tiny) != nullptr, "small buffer terminated", tiny);

    expect(tiny_envelope_key_is_secret("deviceToken") && tiny_envelope_key_is_secret("PASSWORD") &&
               !tiny_envelope_key_is_secret("type") && !tiny_envelope_key_is_secret("prompt"),
           "secret-key classifier", "");
    // Short words count only as whole words of the name.
    const char *secret[] = {"key", "api_key", "apiKey", "x-api-key", "KEY", "privateKey",
                            "auth", "authToken", "Authorization", "session", "session_id",
                            "sessionId", "sig", "pin", "client_secret", "accesstoken"};
    for (const char *k : secret) expect(tiny_envelope_key_is_secret(k), k, "want secret");
    const char *plain[] = {"monkey", "donkey", "turkey_count", "locker", "keyboard",
                           "author", "authored_by", "sessions", "signal", "design",
                           "spinner", "title", "id", ""};
    for (const char *k : plain) expect(!tiny_envelope_key_is_secret(k), k, "want plain");
    expect(!tiny_envelope_key_is_secret(nullptr), "null key", "");
    // ...and benign fields keep their values in the preview.
    describe_json(R"({"payload":{"type":"t","monkey":"banana","apiKey":"SEKRIT6"}})", d, sizeof d);
    expect(strstr(d, "banana") && !strstr(d, "SEKRIT"), "benign kept, apiKey redacted", d);
    puts("envelope host test: ALL GREEN");
    return 0;
}
