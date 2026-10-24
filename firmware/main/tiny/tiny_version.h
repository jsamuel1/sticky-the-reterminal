#pragma once
#define TINY_FW_VERSION "0.31.0"

// The verb grammar is a 3-consumer contract (dashboard, iOS StickyPanel,
// relay agents) held together by string prefixes. Bump this on ANY change to
// a verb, its arguments, or a reply shape — consumers gate on it instead of
// parsing fw strings, which checks 8-9 proved can lie about content.
// v1 = the 14-verb grammar of 0.14.3 (render_ui status sensors say ask voice
//      screenshot miccheck page rotate tap scroll sleep ota).
// v2 = the DM surface, complete (0.14.7→0.14.11):
//      + messages | messages unread | messages thread <login>  (device-token
//        reads via /api/devices/messages; unread replies JSON, others render)
//      + tap id namespaces m:* (inbox/thread/compose flow) and k:* (keyboard)
//        — ids are load-bearing: id present ⇒ label is inert (defuse rule)
//      + status reply gains fw_commit (build-injected git sha)
//      + heartbeat sends wantUnread:"1", reply may carry unread:n
//        (absent ⇒ keep last, never clear)
//      + ota verb gains the downgrade-refusal reply sentence (pointer not
//        newer + no force ⇒ refused, nothing staged)
// v3 = the streaming ask (0.15.0) + the chat card (0.15.2).
//      Two branches each wrote their own "v3 =" block here and both landed; they
//      were complementary, not duplicates, so this is the merge of the two —
//      deleting either one would have silently dropped half of v3 from the
//      contract that three consumers read.
//      + ask/voice render live on the glass when the backend answers SSE
//        (body field stream:"1"; deltas partial-refresh at <=2Hz with a
//        caret; closing full refresh). Relay reply text gains the
//        "[streamed to glass]" suffix on the streamed path.
//      + fallback: a non-SSE backend answer runs the classic render-once
//        path — the flag is an opt-in the server may ignore.
//      + card type "chat": {"messages":[{"role":"user|assistant","body"}]}
//        — iOS bubble chrome on 4-gray (user = LightGray right, assistant =
//        outlined white left, radius 12), scrollable. A streamed TEXT ask
//        renders as chat: question = user bubble, answer types into the
//        assistant bubble. Voice asks keep the plain text card (no honest
//        transcript to echo).
//      + NOT a grammar change, recorded so nobody hunts for one: a failed ask
//        with st<=0 (transport) is retried ONCE and its card is worded as the
//        device's own fault ("couldn't reach tiny"), not the backend's. Reply
//        text on that path begins "ask NOT SENT". st>0 is unchanged.
// v4 = the gesture injection verb (D-UX0, 0.16.2):
//      + swipe <x0> <y0> <x1> <y1> — synthetic swipe in PANEL coords through
//        the finger's own release classifier; reply mirrors tap's shape
//        ({"swipe":[x0,y0,x1,y1],"rotation","result","card_id","summary"}).
//        Sub-slop travel (<=24 px both axes) is REFUSED (ESP_ERR_INVALID_SIZE)
//        instead of being downgraded to a tap. Exists so UX_SPEC §2's gesture
//        grammar can be verified over the relay before any gesture ships.
//      + heartbeat capabilities[] gains "swipe".
// Consumers: gate DM buttons/deep-links on grammar_version >= 2, not on fw.
// v9 = the config verb: `config {json}` merges
//      a networks[] roaming list into NVS (upsert by ssid; "password" accepted
//      as an alias of "key"; {"forget":true} removes). Identity/scalar fields
//      in the body are REFUSED, reply is counts-only (keys never echoed), the
//      dispatch entry log redacts the body. Pairs with the 30s Wi-Fi re-roam
//      task and the settings [Config] QR (drill page TINY_PAGE_CFGQR ->
//      https://sticky.cagatay.my/s/<device_id>). Heartbeat caps gain "config"
//      — the dashboard settings page un-409s itself on this heartbeat.
// v10 = gallery card + g: touch namespace + page gallery.
// v11 = THE UNIVERSE ON THE GLASS (0.27.0):
//      + `agent` verb family: agent | agent <slug> | agent clear | agent list
//        | agent add <slug> | agent rm <slug>. Switch persists (NVS) across
//        reboot; `agent list` replies {"active","roster","summary"} JSON.
//      + every ask/voice body carries tiny:"<slug>" when a non-default agent
//        is active (backend 61b404f0 runs the turn AS that public tiny);
//        empty selection omits the field — owner-default body byte-identical.
//      + status reply gains active_agent ("" = owner's own tiny).
//      + universe card: {"type":"universe","tinys":["slug",…]?} — roster page,
//        active row marked, tap to switch; u: touch namespace (u:open,
//        u:clear, u:i:<n>); settings gains an "agents" row; token "universe".
//      + home badge: when a non-default agent is active the home surface says
//        @slug (input bar reads "Message @slug..."), streamed ask headers
//        carry @slug — a persona's words must never read as the owner's tiny.
//      + backend refusal sentences (unknown/private 404, priced 402) render
//        as normal answer text, not raw JSON.
//      + heartbeat capabilities[] gains "agent".
// v12 = first-run onboarding (0.28.0):
//      + `page onboard [welcome|wifi|link|pair|ready]` — preview any first-run
//        card; bare `page onboard` paints the step the device is actually at.
//      + ob: touch namespace (ob:next ob:back ob:phone ob:type ob:start),
//        card_ids ob-welcome ob-wifi ob-link ob-pair ob-ready.
//      + heartbeat capabilities[] gains "sd" — dispatched since 0.26.0, never
//        advertised (the drift README/commands.md carried). 23 = 23 now.
//      + status reply gains onboard:"<step>" ("done" once the tour is dismissed).
// v13 = board variants (0.30.0):
//      + status reply gains board:"reterminal"|"fnk0104s"|"amoled18" and
//        absent:[...] — the parts this build does not drive (display, touch,
//        mic, buzzer, battery, rtc, imu, env, sd, ota). reTerminal: [].
//      + status `display` / summary come from the board table ("800x480
//        4-gray" on the reTerminal, unchanged).
//      + `ota` on a board no channel carries replies "OTA refused: no channel
//        carries images for this board" and stages nothing.
// v14 = on-device pairing code (0.31.0, onboarding Phase B):
//      + the unprovisioned board enrols itself: POST /api/devices/pair/start
//        {name, platform:"esp32s3", kind:"daemon", capabilities[] +
//        "body:sticky-the-reterminal", secretHash} -> {code, ttl_s}; polls
//        /pair/poll {code, secret} every 3 s; approved -> merge + reboot.
//      + status reply gains, while a code is live, pair_code, pair_url,
//        pair_expires_s, and pair:"asking|waiting|denied|error|approved"
//        while the pairing task runs. Portal GET /info gains pair_code +
//        pair_url. The secret is never exposed.
//      + ob-pair card shape: code (3+3) as the part title + QR of pair_url;
//        denied/error variants. ob: namespace gains ob:pair (new code now)
//        and ob:portal (the Phase A paste page, kept as the fallback);
//        `page onboard portal` previews it.
// v15 = board-stamped images + direct-artifact OTA (CI bench push):
//      + `ota <url> <sha256>` — stage that exact artifact. url must be
//        https://plugin.tiny.technology/media/<key>, sha256 64 hex. No
//        version ordering on this form (CI ships many builds of one version
//        string); the pusher (tools/ota_push.py) proves git ancestry first.
//        `ota [channel]` is unchanged, ver_cmp and all.
//      + every image carries a 32-byte board stamp in .rodata_custom_desc
//        (image offset 288); both ota forms refuse an image whose stamp is
//        not this board's, before the slot is touched. New reply sentences:
//        "OTA refused: board guard ...", "OTA refused: sha256 mismatch ...",
//        "OTA refused: url must be ...".
//      + fnk0104s and amoled18 accept OTA: status `absent` drops "ota".
// v16 = colour UI on colour boards (FNK0104S light theme, AMOLED dark):
//      + card/composite-part/button-object field accent:"red|orange|yellow|
//        green|teal|blue|purple|pink|neutral|mono" (default blue) tints
//        chrome only; the e-ink ignores it (its canvas bytes are unchanged).
//        priority:"alert" titles render red on colour glass.
//      + screenshot on a colour board: 8-bit indexed PNG (colour type 3) with
//        the theme's 64-entry PLTE; the e-ink keeps the 2-bit gray PNG. The
//        reply text is unchanged (still the hosted URL).
//      + status gains color:bool and theme:"light"|"dark".
//      + page palette: preview card of every accent.
//      + raw image/gallery/stream frames are unchanged: gray, drawn literal.
// v17 = which build is on the glass:
//      + status reply gains fw_date: committer date (UTC, "YYYY-MM-DD HH:MM
//        UTC") of fw_commit, build-injected beside it; "?" if git was absent.
//      + settings page fw row reads "<version> (<commit>)" plus a date row.
// v18 = layouts for small panels + per-board caps (P5):
//      + status gains caps:{eink, panel, touch, mic, buzzer, speaker,
//        battery_gauge, battery_voltage, rtc, imu, env, sd, page_buttons,
//        ota, color, dark_theme, flash_mb} — the board table itself;
//        `absent` is derived from it and stays for v13 clients.
//      + status gains battery_mv (null when unmeasurable). FNK0104S reads it
//        from the GPIO 9 divider; its battery_pct stays null (no gauge).
//      + card layout on a panel whose short side is < 480 px (FNK0104S
//        320x480, AMOLED-1.8 368x448) uses the compact profile: 16 px
//        margin, exact glyph-advance wrapping, kv value column after the
//        widest key, stacked menu notes, title drops to body scale before
//        it is cut, one label scale per button bar, footers cut with
//        "...". The card JSON is unchanged; the reTerminal frame is
//        byte-identical (tests/host/render_golden.txt).
//      + `rotate auto` on a board with no IMU stays manual (reply mode
//        "manual"); settings hides the follow-gravity row there.
// v19 = strict direct-artifact OTA argument classification:
//      + any `ota` invocation with two or more arguments uses the direct form;
//        exactly two then reach the URL/sha guards, while extra arguments get
//        the usage refusal. A one-argument value containing "://" also uses
//        the direct form. Bare one-argument channel names and bare `ota` keep
//        the channel behavior. This prevents malformed URLs and channel plus
//        extra-argument forms from silently discarding argument 2.
#define TINY_GRAMMAR_VERSION 19  // v19: strict direct-artifact OTA arguments
