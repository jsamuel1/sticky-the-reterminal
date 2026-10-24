#!/bin/bash
# status cap host test — every caller of tiny_node_status_json() must hand it
# a buffer declared with TINY_STATUS_JSON_CAP (tiny/tiny_node.h) and pass
# `sizeof <that buffer>`. Callers with their own literal sizes is how the
# bench OTA preflight broke: the status object reached 1026 B (v17) and then
# 1303 B (v18, measured on tiny-eba6); a 1024 B caller made the writer refuse,
# and tools/ota_push.py stopped at its mandatory pre-status.
set -euo pipefail
cd "$(dirname "$0")"
python3 - ../../firmware/main <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
hdr = (root / "tiny" / "tiny_node.h").read_text()
m = re.search(r"#define\s+TINY_STATUS_JSON_CAP\s+(\d+)", hdr)
if not m:
    sys.exit("FAIL TINY_STATUS_JSON_CAP not defined in tiny/tiny_node.h")
cap = int(m.group(1))
# Largest status object measured on glass (grammar v18, tiny-eba6): 1303 B.
# Require headroom for a long ssid / pair_url / died_doing on top of that.
MEASURED = 1303
FLOOR = 1536
if cap < FLOOR:
    sys.exit(f"FAIL TINY_STATUS_JSON_CAP={cap} < {FLOOR} (measured {MEASURED} B on glass)")
call = re.compile(r"tiny_node_status_json\(\s*(\w+)\s*,\s*([^)]*)\)")
calls = 0
for src in sorted(root.rglob("*.c*")):
    text = src.read_text(errors="replace")
    for c in call.finditer(text):
        buf, size = c.group(1), c.group(2).strip()
        if buf == "out":  # the definition itself: (char *out, size_t cap)
            continue
        calls += 1
        where = f"{src.relative_to(root)}:{text.count(chr(10), 0, c.start()) + 1}"
        if size not in (f"sizeof {buf}", f"sizeof({buf})"):
            sys.exit(f"FAIL {where}: size '{size}' is not sizeof {buf}")
        decls = re.findall(rf"char\s+{buf}\s*\[([^\]]*)\]", text[: c.start()])
        if not decls or decls[-1].strip() != "TINY_STATUS_JSON_CAP":
            got = decls[-1] if decls else "no char[] declaration"
            sys.exit(f"FAIL {where}: buffer {buf} is [{got}], want [TINY_STATUS_JSON_CAP]")
        print(f"PASS {where}: {buf}[TINY_STATUS_JSON_CAP]")
if calls < 2:
    sys.exit(f"FAIL expected the status verb and settings-page callers, found {calls}")
print(f"status cap host test passed (cap {cap}, {calls} callers)")
PY
