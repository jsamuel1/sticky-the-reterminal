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
