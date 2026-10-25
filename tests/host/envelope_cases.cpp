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

    // ---- classify: which envelopes run, which are dropped, which are answered ----
    struct Case {
        const char *msg;
        tiny_envelope_kind_t want;
        const char *prompt;  // INVOKE: expected prompt
        const char *type;    // PUSH: expected type
        const char *what;
    };
    const Case cases[] = {
        // The 2026-10-25 flood, verbatim shape: a late-reply notify push.
        {R"({"payload":"{\"type\":\"notify\",\"title\":\"\ud83d\udcbb ip-10-0-3-99 finished\",\"body\":\"x\",\"tag\":\"t\",\"url\":\"/u\"}"})",
         TINY_ENVELOPE_PUSH, nullptr, "notify", "notify push (string payload) dropped"},
        {R"({"payload":{"type":"notify","title":"hi"}})", TINY_ENVELOPE_PUSH, nullptr,
         "notify", "notify push (object payload) dropped"},
        // A push never runs, even when it carries a prompt.
        {R"({"payload":"{\"type\":\"notify\",\"prompt\":\"sleep\"}"})", TINY_ENVELOPE_PUSH,
         nullptr, "notify", "push with a prompt is not run"},
        {R"({"payload":"{\"type\":\"device_task_result\"}"})", TINY_ENVELOPE_PUSH, nullptr,
         "device_task_r", "other push type, type truncated to buffer"},
        // Invokes: the sender's real shape, and older senders without a type.
        {R"({"payload":"{\"type\":\"invoke\",\"prompt\":\"status\"}"})", TINY_ENVELOPE_INVOKE,
         "status", nullptr, "invoke string payload"},
        {R"({"payload":{"prompt":"say hi"}})", TINY_ENVELOPE_INVOKE, "say hi", nullptr,
         "untyped object payload with prompt"},
        // Somebody may be waiting: these are answered, never dropped.
        {R"({"payload":"{\"type\":\"invoke\"}"})", TINY_ENVELOPE_REJECT, nullptr, nullptr,
         "invoke without prompt answered"},
        {R"({"payload":{"type":"invoke","prompt":7}})", TINY_ENVELOPE_REJECT, nullptr, nullptr,
         "non-string prompt answered"},
        {R"({"payload":{"type":5,"prompt":"status"}})", TINY_ENVELOPE_REJECT, nullptr, nullptr,
         "non-string type answered, not run"},
        {R"({"payload":"not json"})", TINY_ENVELOPE_REJECT, nullptr, nullptr,
         "unparseable string answered"},
        {R"({"payload":"[1]"})", TINY_ENVELOPE_REJECT, nullptr, nullptr, "array answered"},
        {R"({"payload":null})", TINY_ENVELOPE_REJECT, nullptr, nullptr, "null answered"},
        {R"({"id":"a"})", TINY_ENVELOPE_REJECT, nullptr, nullptr, "missing payload answered"},
    };
    for (const Case &c : cases) {
        cJSON *m = cJSON_Parse(c.msg);
        assert(m);
        char *prompt = reinterpret_cast<char *>(1);
        char type[14];
        memset(type, 'X', sizeof type);
        const tiny_envelope_kind_t k =
            tiny_envelope_classify(cJSON_GetObjectItem(m, "payload"), &prompt, type, sizeof type);
        bool ok = k == c.want;
        if (c.want == TINY_ENVELOPE_INVOKE) ok = ok && prompt && !strcmp(prompt, c.prompt);
        else ok = ok && prompt == nullptr;
        if (c.want == TINY_ENVELOPE_PUSH) ok = ok && !strcmp(type, c.type);
        else ok = ok && type[0] == 0;
        expect(ok, c.what, k == TINY_ENVELOPE_PUSH ? type : prompt ? prompt : "(none)");
        if (k == TINY_ENVELOPE_INVOKE) free(prompt);
        cJSON_Delete(m);
    }
    // NULL out-params are allowed.
    {
        cJSON *m = cJSON_Parse(R"({"payload":{"type":"notify"}})");
        expect(tiny_envelope_classify(cJSON_GetObjectItem(m, "payload"), nullptr, nullptr, 0) ==
                   TINY_ENVELOPE_PUSH,
               "classify with NULL out-params", "");
        cJSON_Delete(m);
    }
    puts("envelope host test: ALL GREEN");
    return 0;
}
