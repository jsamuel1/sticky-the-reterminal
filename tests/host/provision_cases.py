#!/usr/bin/env python3
"""provision_cases.py — tools/provision_lib.py against fake build dirs and a mock tiny API."""
import hashlib
import http.server
import json
import os
import shutil
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import provision_lib as pl  # noqa: E402

FAILS = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILS.append(what)


def refused(fn, *a, **k):
    try:
        fn(*a, **k)
    except pl.Refused as e:
        return str(e)
    return None


# ---- identity --------------------------------------------------------------
check(pl.board_name("b8:1f:3f:c5:05:2c") == ("b8:1f:3f:c5:05:2d", "tiny-eba6"),
      "FNK vector: base …:2c → softAP …:2d → tiny-eba6")
check(pl.fnv_name(pl.parse_mac("b8:1f:3f:c5:05:2c")) == "tiny-ed39", "base-MAC hash (the old bug) = tiny-ed39")
check(pl.softap_mac(bytes.fromhex("b81f3fd780ff")) == bytes.fromhex("b81f3fd78000"), "softAP wraps last octet, no carry")
FLASH_ID = "Chip is ESP32-S3 (QFN56) (revision v0.2)\nMAC: b8:1f:3f:c5:05:2c\nDetected flash size: 16MB\n"
check(pl.parse_flash_id(FLASH_ID) == ("b8:1f:3f:c5:05:2c", "16MB"), "flash_id parse")
check(refused(pl.check_size, "reterminal", "16MB") is not None, "reterminal build on a 16MB board refuses")
check(refused(pl.check_size, "fnk0104s", "16MB") is None, "fnk0104s on 16MB passes")
check(refused(pl.check_size, "amoled18", "32MB") is not None, "amoled18 build on a 32MB board refuses")

# ---- flash plan --------------------------------------------------------------
tmp = tempfile.mkdtemp()


def fake_build(board, size, mode, app_off=0x20000, app_len=1000):
    fw = os.path.join(tmp, board, "firmware")
    os.makedirs(fw, exist_ok=True)
    for f in ("partitions.csv", "partitions_16mb.csv", "sdkconfig.defaults",
              "sdkconfig.defaults.fnk0104s", "sdkconfig.defaults.amoled18"):
        shutil.copy(os.path.join(ROOT, "firmware", f), fw)
    bd = os.path.join(fw, "build" if board == "reterminal" else "build-" + board)
    for d in ("bootloader", "partition_table", "config", "esp-idf/main/gen"):
        os.makedirs(os.path.join(bd, d), exist_ok=True)
    files = {"0x0": "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
             "0xf000": "ota_data_initial.bin", hex(app_off): "tiny_sticky.bin"}
    for off, rel in files.items():
        open(os.path.join(bd, rel), "wb").write(b"\xff" * (app_len if rel == "tiny_sticky.bin" else 16))
    json.dump({"write_flash_args": ["--flash_mode", mode, "--flash_size", size, "--flash_freq", "80m"],
               "flash_settings": {"flash_mode": mode, "flash_size": size, "flash_freq": "80m"},
               "flash_files": files, "extra_esptool_args": {"chip": "esp32s3"}},
              open(os.path.join(bd, "flasher_args.json"), "w"))
    sdk = {"TINY_BOARD_RETERMINAL": board == "reterminal", "TINY_BOARD_FNK0104S": board == "fnk0104s",
           "TINY_BOARD_AMOLED18": board == "amoled18",
           "PARTITION_TABLE_FILENAME": "partitions.csv" if board == "reterminal" else "partitions_16mb.csv"}
    json.dump(sdk, open(os.path.join(bd, "config", "sdkconfig.json"), "w"))
    open(os.path.join(bd, "esp-idf/main/gen/tiny_commit.h"), "w").write('#define TINY_FW_COMMIT "3ffffd7f"\n')
    return bd


fnk = fake_build("fnk0104s", "16MB", "dio")
p = pl.make_plan(fnk, "fnk0104s")
argv = pl.flash_argv(p, "/w/nvs.bin")
check(argv[:6] == ["--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"],
      "fnk0104s: write_flash args from flasher_args.json (16MB DIO)")
check(argv[6:] == ["0x0", fnk + "/bootloader/bootloader.bin", "0x8000", fnk + "/partition_table/partition-table.bin",
                   "0xf000", fnk + "/ota_data_initial.bin", "0x20000", fnk + "/tiny_sticky.bin", "0x9000", "/w/nvs.bin"],
      "fnk0104s: parts in offset order, NVS last at 0x9000")
check(p["nvs_size"] == 0x6000 and p["commit"] == "3ffffd7f", "NVS 0x6000 from partitions_16mb.csv; fw_commit read")
amo = fake_build("amoled18", "16MB", "qio")
check(pl.flash_argv(pl.make_plan(amo, "amoled18"), "n")[1] == "qio", "amoled18: QIO from its own flasher_args")
ret = fake_build("reterminal", "32MB", "dio")
check(pl.make_plan(ret, "reterminal")["write_flash_args"][3] == "32MB", "reterminal: 32MB")
check(refused(pl.make_plan, fnk, "amoled18") is not None, "fnk build dir flashed as amoled18 refuses")
check(refused(pl.make_plan, os.path.join(tmp, "nope", "build"), "fnk0104s") is not None, "unbuilt dir refuses")
unbuilt = pl.make_plan(os.path.join(ROOT, "firmware", "build-nonexistent-x"), "fnk0104s", allow_unbuilt=True)
check(unbuilt["nvs_offset"] == 0x9000 and unbuilt["nvs_size"] == 0x6000, "dry-run plan reads NVS from defaults")
big = fake_build("bigapp", "16MB", "dio", app_off=0x8000, app_len=0x2000)
os.rename(os.path.dirname(big) + "/build-bigapp", os.path.dirname(big) + "/build-fnk0104s")
sdkp = os.path.dirname(big) + "/build-fnk0104s/config/sdkconfig.json"
s = json.load(open(sdkp)); s.update(TINY_BOARD_FNK0104S=True, PARTITION_TABLE_FILENAME="partitions_16mb.csv")
json.dump(s, open(sdkp, "w"))
check("overlaps NVS" in (refused(pl.make_plan, os.path.dirname(big) + "/build-fnk0104s", "fnk0104s") or ""),
      "a part overlapping NVS refuses")

# ---- NVS blob ----------------------------------------------------------------
rec = {"device_id": "d-1", "device_token": "SECRET-T", "api": "https://x", "mac_name": "tiny-eba6"}
blob = pl.cfg_blob(rec, {"ssid": "ap", "key": "k"})
check(blob == {"device_id": "d-1", "token": "SECRET-T", "api": "https://x", "name": "tiny-eba6",
               "networks": [{"ssid": "ap", "key": "k"}]}, "cfg blob = tiny_config.cpp keys")
check(pl.cfg_blob(rec, None)["networks"] == [], "--no-wifi: empty networks (board onboards Wi-Fi itself)")
csvp = os.path.join(tmp, "nvs.csv")
pl.write_nvs_csv(rec, None, csvp)
lines = open(csvp).read().splitlines()
check(lines[:2] == ["key,type,encoding,value", "tiny,namespace,,"] and lines[2].startswith("cfg,data,string,"),
      "NVS CSV: namespace tiny, key cfg, string")
check(oct(os.stat(csvp).st_mode & 0o777) == "0o600", "NVS CSV is 0600")


# ---- mock tiny API -----------------------------------------------------------
class Api(http.server.BaseHTTPRequestHandler):
    state = {}

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
        st = Api.state
        if self.path == "/api/devices":
            if self.headers.get("Authorization") != "Bearer sess":
                return self.reply(401, {"error": "unauthorized"})
            return self.reply(200, {"devices": st.get("devices", [])})
        self.reply(404, {})

    def do_POST(self):
        st = Api.state
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        st.setdefault("calls", []).append((self.path, body))
        if self.path == "/api/devices":
            return self.reply(200, {"ok": True, "device_id": "new-1", "device_token": "TOK-NEW"})
        if self.path == "/api/devices/adopt":
            return self.reply(200, {"ok": True, "device_id": body["deviceId"], "device_token": "TOK-ADOPT"})
        if self.path == "/api/devices/pair/start":
            st["hash"] = body["secretHash"]
            return self.reply(200, {"code": "ABC123", "ttl_s": 30})
        if self.path == "/api/devices/pair/poll":
            if hashlib.sha256(body["secret"].encode()).hexdigest() != st["hash"]:
                return self.reply(403, {"error": "bad secret"})
            st["polls"] = st.get("polls", 0) + 1
            if st["polls"] < 2:
                return self.reply(200, {"status": "pending"})
            outcome = st.get("pair_outcome", "approved")
            if outcome == "approved":
                return self.reply(200, {"status": "approved", "device_id": "pair-1",
                                        "device_token": "TOK-PAIR", "name": "tiny-eba6"})
            return self.reply(200, {"status": outcome})
        self.reply(404, {})


srv = http.server.HTTPServer(("127.0.0.1", 0), Api)
threading.Thread(target=srv.serve_forever, daemon=True).start()
home = os.path.join(tmp, "home")
os.makedirs(home)
os.environ.update(TINY_API="http://127.0.0.1:%d" % srv.server_port, TINY_HOME=home, TINY_PAIR_POLL_S="0.01")
os.environ.pop("TINY_TOKEN", None)
os.environ.pop("WIFI_SSID", None)
receipt = os.path.join(tmp, "receipt.json")


def reset(**kw):
    Api.state = dict(kw)
    if os.path.exists(receipt):
        os.unlink(receipt)


# pairing (no session)
reset()
check(pl.enroll("tiny-eba6", receipt) == "pair-1", "no session → pairing code → approved")
r = json.load(open(receipt))
check(r["device_token"] == "TOK-PAIR" and r["via"] == "pair" and oct(os.stat(receipt).st_mode & 0o777) == "0o600",
      "receipt holds the token, 0600")
start = [b for pth, b in Api.state["calls"] if pth == "/api/devices/pair/start"][0]
check(start["name"] == "tiny-eba6" and start["kind"] == "daemon" and start["platform"] == "esp32s3"
      and start["capabilities"][0] == "body:sticky-the-reterminal" and len(start["secretHash"]) == 64
      and "secret" not in start, "pair/start claim: name/kind/platform/body cap, only the secret's hash")
for outcome in ("denied", "expired", "claimed"):
    reset(pair_outcome=outcome)
    check(outcome in (refused(pl.enroll, "tiny-eba6", receipt) or "") and not os.path.exists(receipt),
          "pairing %s refuses, no receipt" % outcome)

# session
json.dump({"token": "sess", "user": {"login": "owner"}, "expires": 4102444800},
          open(os.path.join(home, "credentials.json"), "w"))
reset()
check(pl.enroll("tiny-eba6", receipt) == "new-1", "session, no row → POST /api/devices")
reset(devices=[{"id": "old-9", "name": "tiny-eba6", "online": False}])
check(pl.enroll("tiny-eba6", receipt) == "new-1" and Api.state["calls"][0][0] == "/api/devices",
      "session, offline same-name row → re-key via POST /api/devices")
reset(devices=[{"id": "live-7", "name": "tiny-eba6", "online": True}])
check("ONLINE" in (refused(pl.enroll, "tiny-eba6", receipt) or "") and not Api.state.get("calls"),
      "session, ONLINE same-name row refuses without --adopt, sends nothing")
reset(devices=[{"id": "live-7", "name": "tiny-eba6", "online": True}])
check(pl.enroll("tiny-eba6", receipt, adopt=True) == "live-7"
      and Api.state["calls"] == [("/api/devices/adopt", {"deviceId": "live-7"})], "--adopt → POST /api/devices/adopt")
reset()
check(pl.enroll("tiny-eba6", receipt, dry_run=True) is None and not Api.state.get("calls")
      and not os.path.exists(receipt), "dry run sends no POST, writes no receipt")
json.dump({"token": "sess", "expires": 1}, open(os.path.join(home, "credentials.json"), "w"))
reset()
check(pl.enroll("tiny-eba6", receipt) == "pair-1", "expired session → pairing")
srv.shutdown()
shutil.rmtree(tmp)

if FAILS:
    print("provision cases: %d FAILED" % len(FAILS))
    sys.exit(1)
print("provision cases: all passed")
