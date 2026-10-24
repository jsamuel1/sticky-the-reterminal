#!/usr/bin/env python3
"""provision_lib.py — the parts of tools/provision_new.sh that are worth testing.

Stdlib only (the boot-log reader imports pyserial lazily, from esptool's own
interpreter), Python 3.6+ so a stock macOS python3 runs it. Subcommands:

  name <base-mac>                     softAP MAC + the tiny-XXXX the board broadcasts
  flash-id                            parse `esptool flash_id` output on stdin → "<mac> <size>"
  check-size <board> <detected>       refuse a board/flash-size mismatch
  plan <build-dir> <board> --out F    flash plan from the build's own flasher_args.json
  argv <plan.json> <nvs.bin>          NUL-separated write_flash arguments
  json-get <file> <dotted.key>        print one field (paths never interpolated into code)
  enroll --name N --receipt F         session (POST /api/devices, adopt) or pairing code
  nvs-csv <receipt> <out.csv>         the NVS CSV tiny_config.cpp reads (namespace tiny, key cfg)
  bootlog <port> <seconds> <log>      capture console after reset, print evidence lines

Secrets: the device token is written only to the 0600 receipt and the NVS CSV
in the caller's 0700 work dir. Nothing here prints it.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import secrets
import sys
import time
import urllib.error
import urllib.request

BOARDS = ("reterminal", "fnk0104s", "amoled18")
BODY = "sticky-the-reterminal"
PLATFORM = "esp32s3"   # = tiny_pair.cpp start_code()
KIND = "daemon"
CAP_LEN, CAP_MAX = 32, 32
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FW = os.path.join(os.path.dirname(HERE), "firmware")
DEFAULT_API = "https://tiny.technology"
UA = "sticky provision_new.sh"


class Refused(Exception):
    """A guard said no. Exit code 2, message on stderr."""


def say(msg):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


# ---------------------------------------------------------------- identity

def parse_mac(s):
    hexs = re.sub(r"[:\-]", "", s.strip().lower())
    if not re.fullmatch(r"[0-9a-f]{12}", hexs):
        raise Refused("not a MAC address: %r" % s)
    return bytes.fromhex(hexs)


def fmt_mac(b):
    return ":".join("%02x" % x for x in b)


def softap_mac(base):
    """ESP_MAC_WIFI_SOFTAP on the ESP32-S3 (IDF v5.4 esp_hw_support/mac_addr.c,
    CONFIG_ESP_MAC_ADDR_UNIVERSE_WIFI_AP): base MAC with mac[5] += 1 on a uint8,
    so the last octet wraps and nothing carries. esptool read_mac / flash_id
    report the BASE (= STA) MAC."""
    b = bytearray(base)
    b[5] = (b[5] + 1) & 0xFF
    return bytes(b)


def fnv_name(mac):
    """tiny_provision.cpp make_ssid(): FNV-1a32 over the 6 MAC bytes, low 16 bits."""
    h = 0x811C9DC5
    for x in mac:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return "tiny-%04x" % (h & 0xFFFF)


def board_name(base_mac_text):
    ap = softap_mac(parse_mac(base_mac_text))
    return fmt_mac(ap), fnv_name(ap)


def parse_flash_id(text):
    m = re.search(r"MAC:\s*((?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})", text)
    if not m:
        raise Refused("no MAC in esptool output (is it an ESP32-S3 in reach?)")
    s = re.search(r"Detected flash size:\s*(\S+)", text)
    return m.group(1).lower(), (s.group(1) if s else "unknown")


# ---------------------------------------------------------------- firmware facts
# Read from the firmware tree being flashed (its own branch), never copied here:
# a hand-kept copy drifts the day a verb or a board's flash size changes.

def firmware_caps(fw_dir):
    """The verb roster tiny_node.cpp s_caps[] advertises in every heartbeat."""
    path = os.path.join(fw_dir, "main", "tiny_node.cpp")
    try:
        src = open(path).read()
    except OSError:
        raise Refused("cannot read %s for the capability roster" % path)
    m = re.search(r"\bs_caps\s*\[\s*\]\s*=\s*\{([^}]*)\}", src)
    caps = re.findall(r'"([^"]+)"', m.group(1)) if m else []
    if not caps:
        raise Refused("no s_caps[] roster in %s" % path)
    return caps


def board_flash_size(fw_dir, board):
    """CONFIG_ESPTOOLPY_FLASHSIZE_* the board's sdkconfig defaults select (last one wins)."""
    size = None
    for f in ["sdkconfig.defaults"] + ([] if board == "reterminal" else ["sdkconfig.defaults." + board]):
        p = os.path.join(fw_dir, f)
        if not os.path.exists(p):
            raise Refused("no %s for board %s" % (p, board))
        for line in open(p):
            m = re.match(r"CONFIG_ESPTOOLPY_FLASHSIZE_(\d+MB)=y\s*$", line.strip())
            if m:
                size = m.group(1)
    if not size:
        raise Refused("no CONFIG_ESPTOOLPY_FLASHSIZE_* in %s sdkconfig defaults" % board)
    return size


def commit_of(fw_commit):
    """The sha inside a `git describe --always --abbrev=8 [--dirty]` stamp:
    'bb2f9323', 'bb2f9323-dirty', or 'v0.31.0-3-gbb2f9323[-dirty]' (= ota_push.py)."""
    s = str(fw_commit or "")
    if s.endswith("-dirty"):
        s = s[: -len("-dirty")]
    if "-g" in s:
        s = s.rsplit("-g", 1)[1]
    return s


def check_provenance(fw_commit, sha, allow_dirty=False):
    """Refuse an image that cannot name its exact tree: no stamp, a -dirty stamp,
    or a stamp that is not the source HEAD (a stale --no-build dir). allow_dirty
    downgrades all three to a warning."""
    c = commit_of(fw_commit)
    if not fw_commit or fw_commit == "nogit" or len(c) < 7:
        why = "build carries no fw_commit stamp (%s)" % (fw_commit or "missing tiny_commit.h")
    elif str(fw_commit).endswith("-dirty"):
        why = "build is %s (uncommitted firmware/ or vendor/ source)" % fw_commit
    elif not re.fullmatch(r"[0-9a-f]{40}", sha or "") or not sha.startswith(c):
        why = "build says fw_commit %s but the source HEAD is %s (stale --no-build dir?)" % (fw_commit, sha)
    else:
        return
    if not allow_dirty:
        raise Refused(why + " — commit and rebuild, or pass --allow-dirty")
    say("[provision] warning: " + why + " (--allow-dirty)")


def check_size(board, detected, fw_dir=DEFAULT_FW):
    want = board_flash_size(fw_dir, board)
    if detected in ("", "unknown", "Unknown"):
        say("[provision] warning: flash size not detected; expecting %s for %s" % (want, board))
        return
    if detected != want:
        hint = ("pass --board fnk0104s or --board amoled18 (env STICKY_BOARD)" if detected == "16MB"
                else "pass --board reterminal" if detected == "32MB" else "check the board")
        raise Refused("board reports %s flash but --board %s builds for %s — %s"
                      % (detected, board, want, hint))


# ---------------------------------------------------------------- flash plan

def _read_json(path):
    with open(path) as f:
        return json.load(f)


def _partition_rows(csv_path):
    rows = []
    with open(csv_path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cells = [c.strip() for c in line.split(",")]
            if len(cells) >= 5:
                rows.append(cells)
    return rows


def nvs_region(csv_path):
    for name, typ, sub, off, size in (r[:5] for r in _partition_rows(csv_path)):
        if name == "nvs" and typ == "data" and sub == "nvs":
            return int(off, 0), int(size, 0)
    raise Refused("no nvs data partition in %s" % csv_path)


def _defaults_partition_csv(firmware_dir, board):
    """Unbuilt (dry-run) fallback: the partition table the board's defaults pick."""
    name = "partitions.csv"
    for f in ["sdkconfig.defaults"] + ([] if board == "reterminal" else ["sdkconfig.defaults." + board]):
        p = os.path.join(firmware_dir, f)
        if os.path.exists(p):
            for line in open(p):
                m = re.match(r'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', line.strip())
                if m:
                    name = m.group(1)
    return os.path.join(firmware_dir, name)


def read_commit(build_dir):
    p = os.path.join(build_dir, "esp-idf", "main", "gen", "tiny_commit.h")
    if not os.path.exists(p):
        return None
    m = re.search(r'TINY_FW_COMMIT\s+"([^"]*)"', open(p).read())
    return m.group(1) if m else None


def make_plan(build_dir, board, allow_unbuilt=False):
    build_dir = os.path.abspath(build_dir)
    firmware_dir = os.path.dirname(build_dir)
    fa_path = os.path.join(build_dir, "flasher_args.json")
    want_size = board_flash_size(firmware_dir, board)
    plan = {"board": board, "build_dir": build_dir, "built": os.path.exists(fa_path),
            "expected_flash_size": want_size}
    if not plan["built"]:
        if not allow_unbuilt:
            raise Refused("no %s — build first (tools/build_board.sh %s)" % (fa_path, board))
        off, size = nvs_region(_defaults_partition_csv(firmware_dir, board))
        plan.update(nvs_offset=off, nvs_size=size, files=[], write_flash_args=[], commit=None)
        return plan
    fa = _read_json(fa_path)
    chip = fa.get("extra_esptool_args", {}).get("chip", "esp32s3")
    if chip != "esp32s3":
        raise Refused("%s is a %s build, not esp32s3" % (build_dir, chip))
    sdk = _read_json(os.path.join(build_dir, "config", "sdkconfig.json"))
    boards = sorted(k for k, v in sdk.items() if k.startswith("TINY_BOARD_") and v is True)
    built_board = boards[0][len("TINY_BOARD_"):].lower() if boards else "reterminal"
    if built_board != board:
        raise Refused("%s holds a %s build, not %s" % (build_dir, built_board, board))
    size = fa.get("flash_settings", {}).get("flash_size")
    if size != want_size:
        raise Refused("%s is built for %s flash; %s has %s" % (build_dir, size, board, want_size))
    part_csv = sdk.get("PARTITION_TABLE_FILENAME") or sdk.get("PARTITION_TABLE_CUSTOM_FILENAME")
    part_csv = part_csv if os.path.isabs(part_csv) else os.path.join(firmware_dir, part_csv)
    nvs_off, nvs_size = nvs_region(part_csv)
    files = []
    for off, rel in sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 0)):
        path = os.path.join(build_dir, rel)
        if not os.path.exists(path):
            raise Refused("flasher_args.json names %s but it is missing" % path)
        o, n = int(off, 0), os.path.getsize(path)
        if o < nvs_off + nvs_size and nvs_off < o + n:
            raise Refused("%s at %s overlaps NVS 0x%x+0x%x" % (rel, off, nvs_off, nvs_size))
        files.append({"offset": o, "path": path, "size": n})
    plan.update(nvs_offset=nvs_off, nvs_size=nvs_size, files=files,
                write_flash_args=list(fa.get("write_flash_args", [])),
                flash_settings=fa.get("flash_settings", {}), partition_csv=part_csv,
                commit=read_commit(build_dir))
    return plan


def plan_summary(plan):
    out = []
    if plan["built"]:
        out.append("flash     %s" % " ".join(plan["write_flash_args"]))
        for f in plan["files"]:
            out.append("  0x%06x  %-40s %8d B" % (f["offset"], os.path.relpath(f["path"], plan["build_dir"]), f["size"]))
    else:
        out.append("flash     %s, settings + parts from %s/flasher_args.json after the build"
                   % (plan["expected_flash_size"], os.path.basename(plan["build_dir"])))
    out.append("  0x%06x  %-40s %8d B  (nvs partition)" % (plan["nvs_offset"], "nvs.bin (tiny/cfg)", plan["nvs_size"]))
    return "\n".join(out)


def flash_argv(plan, nvs_bin):
    args = list(plan["write_flash_args"])
    for f in plan["files"]:
        args += ["0x%x" % f["offset"], f["path"]]
    return args + ["0x%x" % plan["nvs_offset"], nvs_bin]


# ---------------------------------------------------------------- credentials + API

def tiny_home():
    return os.environ.get("TINY_HOME") or os.path.join(os.path.expanduser("~"), ".tiny")


def load_creds():
    """TINY_TOKEN env (tiny-tech's CI override), else ~/.tiny/credentials.json.
    A file without `expires` (older logins) counts as valid; an expired one does not."""
    if os.environ.get("TINY_TOKEN"):
        return {"token": os.environ["TINY_TOKEN"], "login": "env"}
    try:
        c = _read_json(os.path.join(tiny_home(), "credentials.json"))
    except (OSError, ValueError):
        return None
    if not c.get("token"):
        return None
    exp = c.get("expires")
    if isinstance(exp, (int, float)) and exp <= time.time():
        say("[provision] ~/.tiny/credentials.json expired — falling back to a pairing code")
        return None
    c["login"] = (c.get("user") or {}).get("login") or c.get("login") or "?"
    return c


def api_base(creds):
    a = os.environ.get("TINY_API") or os.environ.get("TINY_API_URL") or (creds or {}).get("apiUrl") or DEFAULT_API
    return a.rstrip("/")


def capabilities(fw_dir=DEFAULT_FW):
    """body:<slug> (+ via:<host>) first, as tiny-tech does, then the flashed firmware's
    own s_caps[] — the same set its first heartbeat will advertise."""
    fw = firmware_caps(fw_dir)
    head = ["body:" + BODY]
    try:
        dev = _read_json(os.path.join(tiny_home(), "device.json"))
        if dev.get("deviceId"):
            head.append("via:" + str(dev["deviceId"])[:8])
    except (OSError, ValueError):
        pass
    out = []
    for c in head + fw:
        c = c.strip().lower()[:CAP_LEN]
        if c and c not in out:
            out.append(c)
    return out[:CAP_MAX]


def http(method, url, body=None, token=None, timeout=20):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("User-Agent", UA)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw.decode() or "null")
    except ValueError:
        return status, None


def _devices(payload):
    if isinstance(payload, dict):
        payload = payload.get("devices", [])
    return payload if isinstance(payload, list) else []


def fleet_rows(api, token, name):
    status, payload = http("GET", api + "/api/devices", token=token)
    if status != 200:
        raise Refused("GET /api/devices answered %s — npx tiny-tech login, or unset TINY_TOKEN" % status)
    same = [d for d in _devices(payload) if d.get("name") == name and d.get("kind") != "endpoint"]
    live = next((d for d in same if d.get("online") is True), None)
    return same, live


def _tell_owner(lines):
    """Pairing needs the human NOW. tiny-tech runs this script under spawnSync,
    which buffers stdout/stderr until exit, so also write to the controlling tty."""
    text = "\n".join(lines) + "\n"
    sys.stderr.write(text)
    sys.stderr.flush()
    if not sys.stderr.isatty():
        try:
            with open("/dev/tty", "w") as tty:
                tty.write(text)
        except OSError:
            pass


def pair(api, claim):
    secret = secrets.token_urlsafe(32)  # 32 random bytes, base64url (tiny-tech enroll.ts)
    body = dict(claim, secretHash=hashlib.sha256(secret.encode()).hexdigest())
    status, start = http("POST", api + "/api/devices/pair/start", body)
    if status >= 300 or not isinstance(start, dict) or not start.get("code"):
        raise Refused("pairing could not start: %s" % ((start or {}).get("error") if isinstance(start, dict) else status))
    code = str(start["code"])
    url = "%s/devices/pair?code=%s" % (api, code)
    lines = ["", "  Pair %s — approve it on %s/devices:" % (claim["name"], re.sub(r"^https?://", "", api)), "",
             "      code   %s %s" % (code[:3], code[3:]), "      link   %s" % url, ""]
    try:
        import io as _io
        import qrcode  # optional; prints a terminal QR when present
        q = qrcode.QRCode(border=1)
        q.add_data(url)
        buf = _io.StringIO()
        q.print_ascii(out=buf)
        lines[-1:-1] = ["      " + l for l in buf.getvalue().splitlines()]
    except Exception:
        pass
    _tell_owner(lines)
    poll_s = float(os.environ.get("TINY_PAIR_POLL_S", "3"))
    deadline = time.time() + float(start.get("ttl_s") or 600)
    last_note = time.time()
    while time.time() < deadline:
        status, data = http("POST", api + "/api/devices/pair/poll", {"code": code, "secret": secret})
        data = data if isinstance(data, dict) else {}
        if status == 404:
            raise Refused("pairing code vanished (registry reset?) — run again")
        st = data.get("status")
        if 200 <= status < 300 and st == "approved" and data.get("device_token"):
            _tell_owner(["  approved — enrolled as %s" % (data.get("name") or claim["name"])])
            return data
        if st == "denied":
            raise Refused("the owner denied this device")
        if st == "expired":
            raise Refused("pairing code expired — run again")
        if st == "claimed":
            raise Refused("this code was already claimed by another process")
        if time.time() - last_note >= 30:
            left = int(deadline - time.time())
            _tell_owner(["  waiting for approval… (%d:%02d left)" % (left // 60, left % 60)])
            last_note = time.time()
        time.sleep(poll_s)
    raise Refused("pairing timed out — run again")


def write_receipt(path, rec):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(rec, f, indent=2)
        f.write("\n")
    os.chmod(path, 0o600)


def enroll(name, receipt, adopt=False, dry_run=False, fw_dir=DEFAULT_FW):
    """Returns device_id (prints nothing else on stdout). Dry run: describe the route, send nothing
    but a read-only GET /api/devices when a session exists."""
    claim = {"name": name, "platform": PLATFORM, "kind": KIND, "capabilities": capabilities(fw_dir)}
    creds = load_creds()
    api = api_base(creds)
    if creds:
        same, live = fleet_rows(api, creds["token"], name)
        if dry_run:
            say("credential session (@%s) → POST %s/api/devices" % (creds["login"], api))
            say("fleet     " + ("%s ONLINE as %s… → %s" % (name, str(live.get("id"))[:8],
                                 "adopt (POST /api/devices/adopt)" if adopt else "REFUSE (pass --adopt)") if live
                               else "%s offline row → re-keyed by POST /api/devices" % name if same
                               else "no row named %s → new row" % name))
            say("row       kind=%s platform=%s capabilities=[%s]" % (KIND, PLATFORM, ", ".join(claim["capabilities"])))
            return None
        if live and not adopt:
            raise Refused('"%s" is ONLINE as %s… — this board (or its twin) is already heartbeating. '
                          "Pass --adopt to re-key that row." % (name, str(live.get("id"))[:8]))
        if live:
            status, r = http("POST", api + "/api/devices/adopt", {"deviceId": live["id"]}, token=creds["token"])
            via, adopted = "jwt", True
        else:
            # absent → insert; stale same-name row → the worker re-keys it (same id, new token)
            status, r = http("POST", api + "/api/devices", claim, token=creds["token"])
            via, adopted = "jwt", bool(isinstance(r, dict) and r.get("adopted"))
        if not isinstance(r, dict) or not r.get("device_token") or not r.get("device_id"):
            raise Refused("enrol failed (%s): %s" % (status, (r or {}).get("error") if isinstance(r, dict) else "no token"))
    else:
        if dry_run:
            say("credential no session (no ~/.tiny/credentials.json, no TINY_TOKEN) → pairing code via %s/api/devices/pair/*" % api)
            say("row       kind=%s platform=%s capabilities=[%s]" % (KIND, PLATFORM, ", ".join(claim["capabilities"])))
            return None
        r = pair(api, claim)
        via, adopted = "pair", False
    rec = {"device_id": str(r["device_id"]), "device_token": str(r["device_token"]),
           "name": str(r.get("name") or name), "mac_name": name, "api": api, "via": via,
           "adopted": adopted, "enrolled_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    w = wifi_from_env()
    if w:
        rec["wifi"] = w
    write_receipt(receipt, rec)
    return rec["device_id"]


# ---------------------------------------------------------------- NVS

def wifi_from_env():
    ssid = os.environ.get("WIFI_SSID", "")
    return {"ssid": ssid, "key": os.environ.get("WIFI_KEY", "")} if ssid else None


def cfg_blob(rec, wifi):
    """Exactly the keys tiny_config.cpp parse_into() reads."""
    return {"device_id": rec["device_id"], "token": rec["device_token"], "api": rec["api"],
            "name": rec["mac_name"], "networks": [wifi] if wifi else []}


def write_nvs_csv(rec, wifi, out):
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["key", "type", "encoding", "value"])
        w.writerow(["tiny", "namespace", "", ""])
        w.writerow(["cfg", "data", "string", json.dumps(cfg_blob(rec, wifi), separators=(",", ":"))])


# ---------------------------------------------------------------- boot log

EVIDENCE = re.compile(rb"tiny_config|provision|wifi|heartbeat|got ip|panic|abort|Guru|board", re.I)


def bootlog(port, seconds, log):
    import serial  # pyserial ships with esptool
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = s.rts = False  # do not hold EN/BOOT while listening
    t0 = time.time()
    while True:  # USB-Serial-JTAG re-enumerates after the reset
        try:
            s.open()
            break
        except (OSError, serial.SerialException):
            if time.time() - t0 > 10:
                say("[provision] could not reopen %s for the boot log" % port)
                return 0
            time.sleep(0.5)
    data = bytearray()
    end = time.time() + seconds
    try:
        while time.time() < end:
            try:
                data += s.read(4096)
            except (OSError, serial.SerialException):  # port bounced; reopen once more
                s.close()
                time.sleep(1)
                try:
                    s.open()
                except (OSError, serial.SerialException):
                    pass
    finally:
        s.close()
    with open(log, "wb") as f:
        f.write(bytes(data))
    hits = [l for l in bytes(data).splitlines() if EVIDENCE.search(l)]
    for l in hits[:20]:
        print(l.decode("utf-8", "replace"))
    return len(hits)


# ---------------------------------------------------------------- CLI

def main(argv=None):
    p = argparse.ArgumentParser(prog="provision_lib.py")
    sub = p.add_subparsers(dest="cmd")
    sub.required = True
    a = sub.add_parser("name"); a.add_argument("mac")
    sub.add_parser("flash-id")
    a = sub.add_parser("check-size"); a.add_argument("board", choices=BOARDS); a.add_argument("detected")
    a.add_argument("--fw", default=DEFAULT_FW)
    a = sub.add_parser("provenance"); a.add_argument("fw_commit"); a.add_argument("sha")
    a.add_argument("--allow-dirty", action="store_true")
    a = sub.add_parser("plan"); a.add_argument("build_dir"); a.add_argument("board", choices=BOARDS)
    a.add_argument("--out", required=True); a.add_argument("--allow-unbuilt", action="store_true")
    a = sub.add_parser("argv"); a.add_argument("plan"); a.add_argument("nvs_bin")
    a = sub.add_parser("json-get"); a.add_argument("file"); a.add_argument("key")
    a = sub.add_parser("enroll"); a.add_argument("--name", required=True); a.add_argument("--receipt", required=True)
    a.add_argument("--adopt", action="store_true"); a.add_argument("--dry-run", action="store_true")
    a.add_argument("--fw", default=DEFAULT_FW, help="firmware/ dir being flashed (caps roster)")
    a = sub.add_parser("nvs-csv"); a.add_argument("receipt"); a.add_argument("out")
    a = sub.add_parser("bootlog"); a.add_argument("port"); a.add_argument("seconds", type=float); a.add_argument("log")
    o = p.parse_args(argv)
    try:
        if o.cmd == "name":
            ap, name = board_name(o.mac)
            print(ap, name)
        elif o.cmd == "flash-id":
            print(*parse_flash_id(sys.stdin.read()))
        elif o.cmd == "check-size":
            check_size(o.board, o.detected, o.fw)
        elif o.cmd == "provenance":
            check_provenance(o.fw_commit, o.sha, o.allow_dirty)
        elif o.cmd == "plan":
            plan = make_plan(o.build_dir, o.board, o.allow_unbuilt)
            with open(o.out, "w") as f:
                json.dump(plan, f, indent=2)
            print(plan_summary(plan))
        elif o.cmd == "argv":
            sys.stdout.write("\0".join(flash_argv(_read_json(o.plan), o.nvs_bin)) + "\0")
        elif o.cmd == "json-get":
            v = _read_json(o.file)
            for k in o.key.split("."):
                v = v.get(k) if isinstance(v, dict) else None
            if v is None:
                return 1
            print(v if not isinstance(v, (dict, list)) else json.dumps(v))
        elif o.cmd == "enroll":
            dev = enroll(o.name, o.receipt, o.adopt, o.dry_run, o.fw)
            if dev:
                print(dev)
        elif o.cmd == "nvs-csv":
            write_nvs_csv(_read_json(o.receipt), wifi_from_env(), o.out)
        elif o.cmd == "bootlog":
            print(bootlog(o.port, o.seconds, o.log), file=sys.stderr)
    except Refused as e:
        say("REFUSED: %s" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
