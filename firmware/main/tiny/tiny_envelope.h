// tiny_envelope — describe a relay envelope the node cannot dispatch.
//
// A relay message whose payload has no string `prompt` is answered with
// "unparseable envelope (no prompt)" and otherwise vanishes: the reply says
// nothing about what arrived, so a client flooding the queue with such
// messages cannot be identified from the device. tiny_envelope_describe()
// renders one log-safe line for it: the payload's form and size, its `type`,
// its top-level keys, and a short preview with every value under a
// credential-looking key replaced by "<redacted>".
//
// IDF-free (cJSON only) so tests/host/envelope_host_test.sh compiles it.
#pragma once
#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max bytes of the redacted payload preview inside the description.
#define TINY_ENVELOPE_PREVIEW 120

// What the node does with one relay envelope.
//   INVOKE — run `prompt` as a verb and PATCH the reply. The payload's `type`
//            is "invoke" or absent (older senders omit it) and `prompt` is a
//            string.
//   PUSH   — a typed platform push (`type` is a string other than "invoke":
//            the platform's late-reply "notify", for one). Nobody waits for
//            a reply, so the node drops it with no PATCH and no cadence
//            snap. A push never runs, even if it carries a `prompt`.
//   REJECT — anything else (no payload, unparseable, an invoke without a
//            string prompt, a non-string type). Somebody may be waiting, so
//            the node answers "unparseable envelope (no prompt)".
// Relay delivery is delivered-once (PUT hands an envelope over exactly once
// whether or not it is PATCHed), so dropping a push leaves nothing queued.
typedef enum {
    TINY_ENVELOPE_INVOKE = 0,
    TINY_ENVELOPE_PUSH,
    TINY_ENVELOPE_REJECT,
} tiny_envelope_kind_t;

// Classify a message's "payload" item (JSON string, object, other, or NULL).
// For INVOKE, *prompt (when prompt is non-NULL) receives a malloc'd copy of
// the prompt that the caller frees; otherwise it is set to NULL. For PUSH,
// type_out (when non-NULL and cap > 0) receives the type, truncated to fit.
tiny_envelope_kind_t tiny_envelope_classify(const cJSON *payload, char **prompt,
                                            char *type_out, size_t type_cap);

// `payload` is the message's "payload" item (a JSON string, an object, or
// anything else, or NULL). Writes a NUL-terminated line into out (always
// terminated when cap > 0) and returns out.
char *tiny_envelope_describe(const cJSON *payload, char *out, size_t cap);

// True when a key names a credential. Long words match anywhere in the name
// (token, secret, password, passwd, bearer, cookie, credential,
// authorization, apikey); short ones only as a whole word, split on
// _ - . / space and camelCase (key, auth, session, sig, pin), so
// "api_key" and "sessionId" are secret but "monkey" and "author" are not.
// Case-insensitive.
int tiny_envelope_key_is_secret(const char *key);

#ifdef __cplusplus
}
#endif
