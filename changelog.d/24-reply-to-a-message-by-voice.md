### grammar v28: reply to a message by voice

The reply keyboard (and the ask keyboard on home) has a mic key beside OK when the server can transcribe. Tap it, or press the AI button on the reTerminal or hold BOOT on the colour boards while a keyboard is open, and speak: your words land in the field, unsent, for you to fix and send with OK. If nothing was heard or the network dropped, the keyboard title says so and your draft is untouched. The Wi-Fi password keyboard never listens, and a board with no mic, or a server that has not advertised dictation, shows no key at all.

The device asks the server what it supports on every heartbeat (`wantFeatures`), uploads the clip, and calls `POST /api/devices/transcribe`; no agent turn runs. Card grammar: keyboard `mic` / `listening`, tap id `k:mic`. `status` gains `dictation`, `dictation_server` and `dictation_last`. The composer holds 159 bytes (was 63). Needs tiny-technology#1 on the server. Firmware 0.34.0.
