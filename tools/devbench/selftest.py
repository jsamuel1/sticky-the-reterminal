#!/usr/bin/env python3
"""Server-side checks for devbench: sets, part download, dump upload, jobs.

Runs against a live server (default http://127.0.0.1:8765). No board needed.
    python3 tools/devbench/selftest.py
"""
from __future__ import annotations

import hashlib
import http.client
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
HOST, PORT = os.environ.get("DEVBENCH_HOST", "127.0.0.1"), int(os.environ.get("DEVBENCH_PORT", "8765"))
fails = 0


def req(method, path, body=None, headers=None, chunked=False):
    c = http.client.HTTPConnection(HOST, PORT, timeout=30)
    h = dict(headers or {})
    if chunked:
        h["Transfer-Encoding"] = "chunked"
        c.putrequest(method, path)
        for k, v in h.items():
            c.putheader(k, v)
        c.endheaders()
        for i in range(0, len(body), 65536):
            piece = body[i:i + 65536]
            c.send(f"{len(piece):x}\r\n".encode() + piece + b"\r\n")
        c.send(b"0\r\n\r\n")
    else:
        c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    return r.status, dict(r.getheaders()), r.read()


def check(name, cond, detail=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + (f"  ({detail})" if detail and not cond else ""))
    fails += 0 if cond else 1


st, _, b = req("GET", "/api/sets")
sets = json.loads(b)
check("sets list", st == 200 and isinstance(sets, list), b[:200])
for s in sets:
    print("     set", s["name"], s.get("chip"), s.get("boards"), [(hex(p["offset"]), p["file"], p["size"]) for p in s["parts"]], s.get("error") or "")
real = next((s for s in sets if s["parts"] and not s.get("error")), None)
if real:
    p = min(real["parts"], key=lambda x: x["size"])
    st, h, data = req("GET", f"/api/sets/{real['name']}/{p['file']}")
    check("part download md5", st == 200 and hashlib.md5(data).hexdigest() == p["md5"] == h.get("X-Devbench-Md5"), f"{st} {h.get('X-Devbench-Md5')}")
    st, _, _ = req("GET", f"/api/sets/{real['name']}/..%2Fmanifest.json")
    check("part traversal refused", st == 404, st)
    st, _, _ = req("GET", f"/api/sets/{real['name']}/manifest.json")
    check("non-part file refused", st == 404, st)

blob = os.urandom(1_500_000)
name = "selftest-dump"
st, _, b = req("POST", f"/api/dump/{name}?offset=0", blob[:1 << 20], {"Content-Type": "application/octet-stream"}, chunked=True)
check("dump chunk 1 (chunked encoding)", st == 202 and json.loads(b)["size"] == 1 << 20, b)
st, _, _ = req("POST", f"/api/dump/{name}?offset=5", blob[1 << 20:])
check("dump out-of-order refused", st == 409, st)
st, _, b = req("POST", f"/api/dump/{name}?offset={1 << 20}", blob[1 << 20:])
check("dump chunk 2", st == 202 and json.loads(b)["size"] == len(blob), b)
st, _, b = req("POST", f"/api/dump/{name}/done", json.dumps({"size": len(blob), "md5": "0" * 32}).encode())
check("dump wrong md5 -> 422", st == 422 and not json.loads(b)["verified"], st)
st, _, b = req("POST", f"/api/dump/{name}/done", json.dumps({"size": len(blob), "md5": hashlib.md5(blob).hexdigest()}).encode())
check("dump verified", st == 200 and json.loads(b)["verified"], b[:200])
st, _, b = req("POST", f"/api/dump/{name}?offset=0", blob[:10])
check("dump restart at offset 0 truncates", st == 202 and json.loads(b)["size"] == 10, b)
for suf in (".bin", ".json"):
    (ROOT / "dumps" / f"{name}{suf}").unlink(missing_ok=True)

jid = subprocess.run([sys.executable, str(ROOT / "job.py"), "dump", "--label", "selftest", "--what", "0x8000:0x1000"], capture_output=True, text=True, check=True).stdout.strip()
st, _, b = req("GET", "/api/jobs")
check("job listed", st == 200 and any(j["id"] == jid for j in json.loads(b)), b[:200])
st, _, b = req("POST", f"/api/jobs/{jid}/result", json.dumps({"ok": True, "selftest": True}).encode())
check("job result recorded", st == 200 and json.loads(b)["result"]["ok"], b[:200])
w = subprocess.run([sys.executable, str(ROOT / "job.py"), "wait", jid, "--timeout", "3"], capture_output=True, text=True)
check("job.py wait sees result", w.returncode == 0, w.stderr)
st, _, b = req("GET", "/api/jobs")
check("job removed from queue", not any(j["id"] == jid for j in json.loads(b)))
(ROOT / "jobs" / "done" / f"{jid}.json").unlink(missing_ok=True)

for path in ("/server.py", "/job.py", "/dumps/x", "/jobs/x", "/images/README.md"):
    st, _, _ = req("GET", path)
    check(f"static {path} refused", st == 404, st)

print("PASS" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
