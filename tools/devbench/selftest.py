#!/usr/bin/env python3
"""Server-side checks for devbench: sets, part download, dump upload, jobs, the
body-free upload channel, tiny.technology provisioning (mock API) and the
browser-side provisioning module (name vector, NVS image).

    python3 tools/devbench/selftest.py            # own server on a free port, temp dirs, mock tiny API
    DEVBENCH_PORT=8765 python3 tools/devbench/selftest.py --live   # also against a running bench

No board needed. The NVS check runs esp_idf_nvs_partition_gen in
espressif/idf:v5.4 when docker is here (else it compares against the committed
golden images in tools/devbench/testdata/).
"""
from __future__ import annotations

import base64
import hashlib
import http.client
import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent
fails = 0
HOST = "127.0.0.1"
PORT = 0


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


X = {"X-Devbench": "1"}


def tiny(path, body):
    st, _, b = req("POST", f"/api/tiny/{path}", json.dumps(body).encode(), {**X, "Content-Type": "application/json"})
    return st, json.loads(b)


def via_get(path, body: bytes, query: dict | None = None, piece=4000):
    """What bench.js send() does after a tunnel 502: GET /api/x/<path>?b=… in pieces."""
    enc = base64.urlsafe_b64encode(body).rstrip(b"=").decode()
    n = max(1, -(-len(enc) // piece))
    u = "st" + os.urandom(4).hex()
    from urllib.parse import urlencode
    for i in range(n):
        q = urlencode({**(query or {}), "u": u, "i": i, "n": n, "b": enc[i * piece:(i + 1) * piece]})
        st, _, b = req("GET", f"/api/x/api/{path}?{q}", headers=X)
        if i < n - 1 and st != 202:
            return st, b
    return st, b


# ---- mock tiny.technology ------------------------------------------------------------
class Tiny(http.server.BaseHTTPRequestHandler):
    state: dict = {}

    def log_message(self, *a):
        pass

    def reply(self, code, body):
        raw = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path == "/api/devices":
            if self.headers.get("Authorization") != "Bearer sess":
                return self.reply(401, {"error": "unauthorized"})
            return self.reply(200, {"devices": Tiny.state.get("devices", [])})
        self.reply(404, {})

    def do_POST(self):
        st = Tiny.state
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        st.setdefault("calls", []).append((self.path, body))
        if self.path == "/api/devices":
            return self.reply(200, {"ok": True, "device_id": "new-1", "device_token": "tind_SESSIONTOKEN_0001"})
        if self.path == "/api/devices/adopt":
            return self.reply(200, {"ok": True, "device_id": body["deviceId"], "device_token": "tind_ADOPTTOKEN_0002"})
        if self.path == "/api/devices/pair/start":
            st["hash"] = body["secretHash"]
            return self.reply(200, {"code": "ABC123", "ttl_s": 60})
        if self.path == "/api/devices/pair/poll":
            if body.get("code") != "ABC123":
                return self.reply(404, {"ok": False, "error": "unknown code"})
            if hashlib.sha256(body["secret"].encode()).hexdigest() != st.get("hash"):
                return self.reply(403, {"error": "bad secret"})
            st["polls"] = st.get("polls", 0) + 1
            if st["polls"] < 2:
                return self.reply(200, {"status": "pending"})
            out = st.get("outcome", "approved")
            if out == "approved":
                return self.reply(200, {"status": "approved", "device_id": "pair-1", "device_token": "tind_PAIRTOKEN_0003", "name": "tiny-eba6"})
            if out == "vanish":
                return self.reply(404, {"ok": False, "error": "unknown code"})
            return self.reply(200, {"status": out})
        self.reply(404, {})


def free_port():
    s = socket.socket()
    s.bind((HOST, 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_server(tmp: Path, tiny_port: int) -> subprocess.Popen:
    global PORT
    PORT = free_port()
    env = {**os.environ, "DEVBENCH_PORT": str(PORT), "DEVBENCH_QUIET": "1",
           "DEVBENCH_REPORTS": str(tmp / "reports"), "DEVBENCH_DUMPS": str(tmp / "dumps"),
           "DEVBENCH_JOBS": str(tmp / "jobs"), "DEVBENCH_IMAGES": str(tmp / "images"),
           "DEVBENCH_IDF_BUILDS": str(tmp / "build"),
           "TINY_API": f"http://{HOST}:{tiny_port}", "TINY_HOME": str(tmp / "tinyhome")}
    env.pop("TINY_TOKEN", None)
    p = subprocess.Popen([sys.executable, str(ROOT / "server.py")], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        try:
            socket.create_connection((HOST, PORT), 0.2).close()
            return p
        except OSError:
            time.sleep(0.05)
    raise SystemExit("server did not start: " + p.stderr.read().decode()[-500:])


def fake_set(tmp: Path):
    s = tmp / "images" / "tiny-fnk0104s-test"
    s.mkdir(parents=True)
    # app image with an esp_app_desc_t: magic at +32, version +48, time +112, date +128
    app = bytearray(os.urandom(4096))
    app[0] = 0xE9
    app[32:36] = (0xABCD5432).to_bytes(4, "little")
    for off, txt in ((16, b"0.31.0"), (48, b"tiny_sticky"), (80, b"01:10:02"), (96, b"Oct 24 2026"), (112, b"v5.4")):
        app[32 + off:32 + off + 16] = txt.ljust(16, b"\0")
    for f, n in (("bootloader.bin", 64), ("partition-table.bin", 32), ("ota_data_initial.bin", 32)):
        (s / f).write_bytes(os.urandom(n))
    (s / "tiny_sticky.bin").write_bytes(bytes(app))
    (s / "manifest.json").write_text(json.dumps({"chip": "ESP32-S3", "boards": ["fnk0104s"], "version": "test",
                                                "flash_settings": {"flash_mode": "dio", "flash_freq": "80m", "flash_size": "16MB"},
                                                "parts": [{"offset": "0x0", "file": "bootloader.bin"}, {"offset": "0x8000", "file": "partition-table.bin"},
                                                          {"offset": "0xf000", "file": "ota_data_initial.bin"}, {"offset": "0x20000", "file": "tiny_sticky.bin"}]}))
    (s / "README.md").write_text("not a part")
    old = tmp / "images" / "tiny-fnk0104s-older"
    shutil.copytree(s, old)
    app[32 + 96:32 + 112] = b"Oct 22 2026".ljust(16, b"\0")
    (old / "tiny_sticky.bin").write_bytes(bytes(app))


def section_server(tmp: Path):
    st, _, b = req("GET", "/api/sets")
    sets = json.loads(b)
    check("sets list", st == 200 and isinstance(sets, list), b[:200])
    real = next((s for s in sets if s["parts"] and not s.get("error")), None)
    check("image set listed with boards", bool(real) and real.get("boards") == ["fnk0104s"], sets)
    names = [s["name"] for s in sets]
    check("sets carry built_at from the app descriptor (UTC), newest first",
          real and real["name"] == "tiny-fnk0104s-test" and real.get("built_at") == "2026-10-24T01:10:02Z" and real.get("app_version") == "0.31.0"
          and names.index("tiny-fnk0104s-test") < names.index("tiny-fnk0104s-older"), [(s["name"], s.get("built_at")) for s in sets])
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

    jobs = tmp / "jobs"
    env = {**os.environ, "DEVBENCH_JOBS": str(jobs)}

    def queue(*args):
        r = subprocess.run([sys.executable, str(ROOT / "job.py"), *args], capture_output=True, text=True, env=env)
        return r.returncode, r.stdout.strip(), r.stderr

    rc, jid, _ = queue("dump", "--label", "selftest", "--what", "0x8000:0x1000")
    st, _, b = req("GET", "/api/jobs")
    check("job listed", st == 200 and any(j["id"] == jid for j in json.loads(b)), b[:200])
    st, _, b = req("POST", f"/api/jobs/{jid}/result", json.dumps({"ok": True, "selftest": True}).encode())
    check("job result recorded", st == 200 and json.loads(b)["result"]["ok"], b[:200])
    w = subprocess.run([sys.executable, str(ROOT / "job.py"), "wait", jid, "--timeout", "3"], capture_output=True, text=True, env=env)
    check("job.py wait sees result", w.returncode == 0, w.stderr)
    st, _, b = req("GET", "/api/jobs")
    check("job removed from queue", not any(j["id"] == jid for j in json.loads(b)))

    rc, jid, _ = queue("flash", "--label", "fnk0104s", "--set", "tiny-fnk0104s-test", "--provision", "--adopt")
    j = json.loads((jobs / f"{jid}.json").read_text())
    check("job.py flash --provision --adopt queues provision+adopt", rc == 0 and j["provision"] and j["adopt"] and not j["dry_run"], j)
    check("job file carries no Wi-Fi", not any(k in j for k in ("ssid", "key", "wifi")), j)
    rc, _, err = queue("flash", "--label", "fnk0104s", "--set", "x", "--adopt")
    check("job.py --adopt without --provision refused", rc != 0, err)
    rc, _, err = queue("probe", "--label", "x", "--provision")
    check("job.py --provision on a non-flash job refused", rc != 0, err)
    # Decline through the GET channel (the P1 report: Decline did nothing behind the tunnel)
    st, b = via_get(f"jobs/{jid}/result", json.dumps({"ok": False, "declined": True}).encode())
    check("Decline via GET channel moves the job to done", st == 200 and json.loads(b)["result"]["declined"]
          and (jobs / "done" / f"{jid}.json").is_file(), b[:200])

    # body-free channel: same effect as the POST, multi-piece, refusals
    log_text = ("boot line\n" * 2000).encode()
    st, b = via_get("log", log_text, {"s": "selftest-x", "l": "fnk0104s"})
    check("GET channel: 20 KB log in pieces appended", st == 202 and (tmp / "reports" / "selftest-x.log").read_bytes() == log_text, (st, b[:120]))
    st, b = via_get("report", json.dumps({"label": "selftest", "kind": "x"}).encode())
    check("GET channel: report stored", st == 201, b[:200])
    st, _, _ = req("GET", "/api/x/api/log?u=a&i=0&n=1&b=eA")
    check("GET channel without X-Devbench refused", st == 403, st)
    st, b = via_get("dump/whatever?offset=0", b"x")
    check("GET channel refuses dumps", st in (400, 404), st)

    for path in ("/server.py", "/job.py", "/dumps/x", "/jobs/x", "/images/README.md", "/provision.py"):
        st, _, _ = req("GET", path)
        check(f"static {path} refused", st == 404, st)
    st, _, _ = req("GET", "/provision.js")
    check("provision.js served to the page", st == 200, st)


def section_tiny(tmp: Path):
    home = tmp / "tinyhome"
    home.mkdir(exist_ok=True)
    reset = lambda **kw: Tiny.state.clear() or Tiny.state.update(kw)  # noqa: E731
    st, _, b = req("GET", "/api/tiny/status", headers=X)
    s = json.loads(b)
    check("tiny/status: no session", st == 200 and s["session"] is False, s)
    st, _, _ = req("GET", "/api/tiny/status")
    check("tiny/status without X-Devbench refused", st == 403, st)
    st, _ = tiny("pair/start", {"name": "board"})
    check("pair/start refuses a name that is not tiny-xxxx", st == 400, st)

    # pairing: approved
    reset()
    st, s = tiny("pair/start", {"name": "tiny-eba6"})
    start = next(b for p, b in Tiny.state["calls"] if p == "/api/devices/pair/start")
    check("pair/start → code + url, secret stays server-side", st == 200 and s["code"] == "ABC123" and s["url"].endswith("/devices/pair?code=ABC123")
          and "secret" not in json.dumps(s), s)
    check("pair/start claim: tiny-tech shape (body cap first, esp32s3, daemon, sha256 hash only)",
          start["name"] == "tiny-eba6" and start["platform"] == "esp32s3" and start["kind"] == "daemon"
          and start["capabilities"][0] == "body:sticky-the-reterminal" and len(start["secretHash"]) == 64 and "secret" not in start, start)
    _, p1 = tiny("pair/poll", {"code": "ABC123"})
    _, p2 = tiny("pair/poll", {"code": "ABC123"})
    check("pair/poll: pending then approved with token", p1["status"] == "pending" and p2["status"] == "approved"
          and p2["device_token"] == "tind_PAIRTOKEN_0003" and p2["device_id"] == "pair-1", (p1, p2))
    _, p3 = tiny("pair/poll", {"code": "ABC123"})
    check("pair/poll after approval: vanished (secret forgotten)", p3.get("status") == "vanished", p3)
    for out in ("denied", "expired", "claimed"):
        reset(outcome=out, polls=1)
        tiny("pair/start", {"name": "tiny-eba6"})
        _, p = tiny("pair/poll", {"code": "ABC123"})
        check(f"pair/poll: {out}", p["status"] == out, p)
    reset(outcome="vanish", polls=1)
    tiny("pair/start", {"name": "tiny-eba6"})
    _, p = tiny("pair/poll", {"code": "ABC123"})
    check("pair/poll: 404 upstream → vanished", p["status"] == "vanished", p)

    # session route
    (home / "credentials.json").write_text(json.dumps({"token": "sess", "user": {"login": "owner"}, "expires": 4102444800}))
    st, _, b = req("GET", "/api/tiny/status", headers=X)
    check("tiny/status: session + login", json.loads(b)["session"] is True and json.loads(b)["login"] == "owner", b)
    reset()
    st, r = tiny("enroll", {"name": "tiny-eba6"})
    check("enroll: no row → POST /api/devices", st == 200 and r["device_id"] == "new-1" and Tiny.state["calls"][0][0] == "/api/devices", r)
    reset(devices=[{"id": "old-9", "name": "tiny-eba6", "online": False}])
    st, r = tiny("enroll", {"name": "tiny-eba6"})
    check("enroll: stale same-name row → re-keyed by POST /api/devices (no ghost)", st == 200 and r["adopted"] and [c[0] for c in Tiny.state["calls"]] == ["/api/devices"], r)
    reset(devices=[{"id": "live-7", "name": "tiny-eba6", "online": True}])
    st, r = tiny("enroll", {"name": "tiny-eba6"})
    check("enroll: ONLINE row refused without adopt, nothing sent", st == 409 and "ONLINE" in r["error"] and not Tiny.state.get("calls"), r)
    st, r = tiny("lookup", {"name": "tiny-eba6"})
    check("lookup: same-name rows (for the plan)", st == 200 and r["rows"][0]["online"] is True, r)
    reset(devices=[{"id": "live-7", "name": "tiny-eba6", "online": True}])
    st, r = tiny("enroll", {"name": "tiny-eba6", "adopt": True})
    check("enroll adopt → POST /api/devices/adopt", st == 200 and Tiny.state["calls"] == [("/api/devices/adopt", {"deviceId": "live-7"})], r)

    # the token never reaches disk through the bench
    leak = {"label": "fnk0104s", "kind": "flash", "device_token": "tind_SESSIONTOKEN_0001", "token": "tind_ADOPTTOKEN_0002",
            "note": "accidentally logged tind_PAIRTOKEN_0003 and tind_FOREIGNTOKEN_9999", "wifi_key": "hunter22"}
    req("POST", "/api/report", json.dumps(leak).encode(), {"Content-Type": "application/json"})
    via_get("log", b"console: cfg {\"token\":\"tind_PAIRTOKEN_0003\"}\n", {"s": "leak", "l": "x"})
    req("POST", "/api/log?s=leak2", b"tind_SESSIONTOKEN_0001 in a POST log\n")
    on_disk = b"".join(f.read_bytes() for f in (tmp / "reports").rglob("*") if f.is_file())
    on_disk += b"".join(f.read_bytes() for f in (tmp / "jobs").rglob("*") if f.is_file())
    check("no device token / Wi-Fi key in reports, logs or job files",
          not any(t in on_disk for t in (b"tind_SESSIONTOKEN_0001", b"tind_ADOPTTOKEN_0002", b"tind_PAIRTOKEN_0003", b"tind_FOREIGNTOKEN_9999", b"hunter22")))


def section_provision_js(tmp: Path):
    node = shutil.which("node")
    if not node:
        print("skip provision.js checks (no node)")
        return
    vec = tmp / "nvs"
    vec.mkdir()
    cfgs = {
        "short": {"device_id": "dev_0123456789abcdef", "token": "tind_TESTTOKEN_not_real_0123456789", "api": "https://tiny.technology",
                  "name": "tiny-eba6", "networks": [{"ssid": "bench-ap", "key": "password123"}]},
        "long": {"device_id": "d" * 47, "token": "t" * 79, "api": "https://" + "a" * 87, "name": "n" * 47,
                 "networks": [{"ssid": "s%d" % i + "x" * 29, "key": "k" * 64} for i in range(8)]},
    }
    for n, cfg in cfgs.items():
        (vec / f"{n}.json").write_text(json.dumps(cfg))
        (vec / f"{n}.csv").write_text("key,type,encoding,value\ntiny,namespace,,\n" + "cfg,data,string,\"" + json.dumps(cfg, separators=(",", ":")).replace('"', '""') + "\"\n")
    golden = ROOT / "testdata"
    ref = {}
    if shutil.which("docker") and os.environ.get("DEVBENCH_NO_DOCKER") != "1":
        script = vec / "gen.sh"
        script.write_text("#!/bin/bash\nset -e\nfor n in short long; do python -m esp_idf_nvs_partition_gen generate /v/$n.csv /v/$n.bin 0x6000 >/dev/null; done\n")
        script.chmod(0o755)
        r = subprocess.run(["docker", "run", "--rm", "-u", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp", "-v", f"{vec}:/v",
                            os.environ.get("TINY_IDF_IMAGE", "espressif/idf:v5.4"), "/v/gen.sh"], capture_output=True, text=True)
        if r.returncode == 0:
            ref = {n: (vec / f"{n}.bin").read_bytes() for n in cfgs}
            print("     NVS reference: esp_idf_nvs_partition_gen in espressif/idf:v5.4")
        else:
            print("     docker generator failed; using golden images:", r.stderr[-200:])
    if not ref:
        ref = {n: (golden / f"nvs-{n}.bin").read_bytes() for n in cfgs}
        print("     NVS reference: tools/devbench/testdata golden images")
    for n, b in ref.items():
        check(f"golden testdata/nvs-{n}.bin matches the generator", (golden / f"nvs-{n}.bin").read_bytes() == b)
    mjs = vec / "t.mjs"
    mjs.write_text(f"""
import {{ readFileSync, writeFileSync }} from "node:fs";
import * as P from {json.dumps(str(ROOT / "provision.js"))};
const out = {{}};
out.name = P.boardName("b8:1f:3f:c5:05:2c");
out.softap = P.softapMac("b8:1f:3f:c5:05:2c");
out.wrap = P.softapMac("b8:1f:3f:d7:80:ff");
out.names = ["b8:1f:3f:d7:23:60", "b8:1f:3f:d7:80:ff", "00:00:00:00:00:00"].map(P.boardName);
for (const n of ["short", "long"]) {{
  const img = P.tinyNvs(P.buildCfg(JSON.parse(readFileSync({json.dumps(str(vec))} + "/" + n + ".json"))));
  writeFileSync({json.dumps(str(vec))} + "/" + n + ".js.bin", img);
  out["back_" + n] = P.readNvsStrings(img)["tiny/cfg"];
}}
const bad = (f) => {{ try {{ f(); return null; }} catch (e) {{ return e.message; }} }};
out.too_long_token = bad(() => P.buildCfg({{ device_id: "d", token: "t".repeat(80), name: "tiny-eba6", networks: [{{ ssid: "a", key: "password1" }}] }}));
out.short_key = bad(() => P.buildCfg({{ device_id: "d", token: "t", name: "x", networks: [{{ ssid: "a", key: "short" }}] }}));
out.open_net = bad(() => P.buildCfg({{ device_id: "d", token: "t", name: "x", networks: [{{ ssid: "a", key: "" }}] }}));
out.caps = P.STICKY_CAPABILITIES;
out.claim = P.stickyClaim("tiny-eba6");
out.redacted = P.redact({{ device_token: "tind_x", nested: [{{ token: "abc", key: "wifipw" }}], ok: 1 }});
console.log(JSON.stringify(out));
""")
    r = subprocess.run([node, str(mjs)], capture_output=True, text=True)
    if r.returncode:
        check("provision.js runs under node", False, r.stderr[-400:])
        return
    o = json.loads(r.stdout)
    check("name vector: base b8:1f:3f:c5:05:2c → softAP …:2d → tiny-eba6 (not tiny-ed39)", o["name"] == "tiny-eba6" and o["softap"] == "b8:1f:3f:c5:05:2d", o)
    check("softAP last octet wraps, no carry", o["wrap"] == "b8:1f:3f:d7:80:00", o["wrap"])
    sys.path.insert(0, str(REPO / "tools"))
    import provision_lib as plib  # the CLI path's implementation (tools/provision_new.sh)
    check("provision.js names == provision_lib names", o["names"] == [plib.board_name(m)[1] for m in ("b8:1f:3f:d7:23:60", "b8:1f:3f:d7:80:ff", "00:00:00:00:00:00")], o["names"])
    fw_caps = plib.firmware_caps(str(REPO / "firmware"))
    check("provision.js capabilities == firmware tiny_node.cpp s_caps[]", o["caps"] == fw_caps, {"js": o["caps"], "firmware": fw_caps})
    check("stickyClaim: body cap first, esp32s3, daemon", o["claim"]["capabilities"][0] == "body:sticky-the-reterminal" and o["claim"]["platform"] == "esp32s3" and o["claim"]["kind"] == "daemon")
    for n in cfgs:
        mine = (vec / f"{n}.js.bin").read_bytes()
        first = next((i for i in range(len(ref[n])) if i >= len(mine) or mine[i] != ref[n][i]), None)
        check(f"NVS image ({n}, {len(json.dumps(cfgs[n]))} B cfg) byte-identical to esp_idf_nvs_partition_gen", first is None and len(mine) == len(ref[n]) == 0x6000,
              f"first difference at 0x{first:x}" if first is not None else len(mine))
        check(f"NVS image ({n}) reads back tiny/cfg", json.loads(o[f"back_{n}"]) == cfgs[n])
    check("buildCfg refuses a token the board would truncate", "at most 79" in (o["too_long_token"] or ""), o["too_long_token"])
    check("buildCfg refuses a WPA2 key under 8 chars", "8 characters" in (o["short_key"] or ""), o["short_key"])
    check("buildCfg accepts an open network", o["open_net"] is None, o["open_net"])
    check("redact() hides token/device_token/key at any depth", "tind_x" not in json.dumps(o["redacted"]) and "wifipw" not in json.dumps(o["redacted"]) and o["redacted"]["ok"] == 1, o["redacted"])
    pub = REPO / "docs" / "install" / "provision.js"
    if pub.exists():
        check("docs/install/provision.js is byte-identical to tools/devbench/provision.js", pub.read_bytes() == (ROOT / "provision.js").read_bytes())


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="devbench-selftest-"))
    tiny_srv = http.server.ThreadingHTTPServer((HOST, 0), Tiny)
    threading.Thread(target=tiny_srv.serve_forever, daemon=True).start()
    fake_set(tmp)
    srv = start_server(tmp, tiny_srv.server_port)
    try:
        section_server(tmp)
        section_tiny(tmp)
        section_provision_js(tmp)
    finally:
        srv.terminate()
        srv.wait(5)
        tiny_srv.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("PASS" if not fails else f"{fails} FAILED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
