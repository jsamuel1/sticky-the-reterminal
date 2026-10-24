#!/usr/bin/env python3
"""ota_push.py — push one CI-built app image to one bench board over the relay.

    TINY_TOKEN=<user JWT> TINY_DEVICE=<id|name> \\
        tools/ota_push.py --board fnk0104s --sha <ci sha> --image tiny_sticky.bin
    tools/ota_push.py --inspect firmware/build-fnk0104s/tiny_sticky.bin   # read the board stamp
    tools/ota_push.py --token-days                                        # days until TINY_TOKEN expires

Steps (each printed): resolve the device (GET /api/devices) → probe `status`
over the relay and require board == --board → prove the on-glass fw_commit is
an ancestor of --sha (docs/release.md rule 5; a downgrade or an unrelated
history is refused) → check the image's own board stamp → upload it to
/api/media → relay `ota <url> <sha256>` → poll `status` until fw_commit equals
--sha. Fails on refusal, rollback (the board comes back on its old commit) or
timeout.

--board-override skips the image-stamp and ancestry checks and sends the image
anyway. It exists for one purpose: proving the FIRMWARE's board guard refuses a
foreign image. The device must stay on its slot and reply "board guard".

Stdlib only. The token is read from env TINY_TOKEN (or ~/.tiny/credentials.json
with --creds) and is never printed. Cloudflare in front of tiny.technology 403s
the default Python-urllib User-Agent (error 1010), so every request names itself.
"""
import argparse
import base64
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

API = os.environ.get("TINY_API_URL", "https://tiny.technology").rstrip("/")
UA = "sticky-ota-push/1 (+https://github.com/cagataycali/sticky-the-reterminal)"
MEDIA_PREFIX = "https://plugin.tiny.technology/media/"
PROJECT = "tiny_sticky"

# --- image stamp: Python twin of firmware/main/tiny/tiny_ota_util.h ---------
DESC_OFF, DESC_LEN, STAMP_OFF, HEAD_LEN = 32, 256, 288, 320
DESC_MAGIC = 0xABCD5432
STAMP_MAGIC = b"TINYBRD1"


def parse_image(head: bytes):
    """Return (verdict, board, project). verdict matches tiny_ota_util::Verdict names."""
    if len(head) < HEAD_LEN:
        return "truncated", "", ""
    if head[0] != 0xE9 or struct.unpack_from("<I", head, DESC_OFF)[0] != DESC_MAGIC:
        return "not-esp", "", ""
    project = head[DESC_OFF + 48:DESC_OFF + 80].split(b"\0", 1)[0].decode("ascii", "replace")
    st = head[STAMP_OFF:STAMP_OFF + 32]
    if st[:8] != STAMP_MAGIC:
        return "unstamped", "", project
    field = st[8:24]
    raw = field.split(b"\0", 1)[0]
    if not raw or len(raw) >= 16 or any(field[len(raw):]):
        return "bad-stamp", "", project
    if any(not (0x61 <= c <= 0x7A or 0x30 <= c <= 0x39 or c in b"_-") for c in raw):
        return "bad-stamp", "", project
    return "ok", raw.decode(), project


# --- small helpers ----------------------------------------------------------
def step(msg):
    print(f"[ota_push] {msg}", flush=True)


def die(msg, code=1):
    print(f"::error::ota_push: {msg}", flush=True)
    sys.exit(code)


def load_token(use_creds):
    tok = os.environ.get("TINY_TOKEN", "").strip()
    if not tok and use_creds:
        p = os.path.expanduser("~/.tiny/credentials.json")
        try:
            tok = json.load(open(p)).get("token", "")
        except (OSError, ValueError):
            tok = ""
    return tok


def jwt_exp(tok):
    try:
        part = tok.split(".")[1]
        part += "=" * (-len(part) % 4)
        return int(json.loads(base64.urlsafe_b64decode(part)).get("exp"))
    except (IndexError, ValueError, TypeError):
        return None


class Api:
    def __init__(self, token):
        self._h = {"Content-Type": "application/json", "Authorization": f"Bearer {token}",
                   "User-Agent": UA}

    def call(self, method, path, body=None, timeout=60):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(API + path, data, self._h, method=method)
        for attempt in range(4):
            try:
                with urllib.request.urlopen(req, timeout=timeout) as r:
                    return r.status, json.loads(r.read() or b"{}")
            except urllib.error.HTTPError as e:
                txt = e.read()[:300].decode("utf-8", "replace")
                if e.code in (429, 502, 503, 504) and attempt < 3:
                    time.sleep(5 * (attempt + 1))
                    continue
                try:
                    return e.code, json.loads(txt)
                except ValueError:
                    return e.code, {"error": txt.strip()}
            except (urllib.error.URLError, TimeoutError) as e:
                if attempt < 3:
                    time.sleep(5 * (attempt + 1))
                    continue
                return 0, {"error": str(e)}
        return 0, {"error": "unreachable"}

    def resolve(self, dev):
        st, d = self.call("GET", "/api/devices")
        if st != 200 or not d.get("ok"):
            die(f"GET /api/devices -> {st} {d.get('error', '')}")
        rows = d.get("devices") or []
        hit = [r for r in rows if str(r.get("id")) == dev] or [r for r in rows if r.get("name") == dev]
        if len(hit) != 1:
            die(f"device {dev!r}: {len(hit)} matches among {len(rows)} devices")
        return hit[0]

    def invoke(self, dev_id, prompt, wait_s=180):
        """Send one verb, wait for the reply. Returns the verb's result string, or None on timeout."""
        st, r = self.call("POST", "/api/devices/relay",
                          {"toDevice": dev_id,
                           "payload": json.dumps({"type": "invoke", "prompt": prompt})})
        if st == 413:
            die(f"relay 413: envelope too large for {prompt.split(' ', 1)[0]!r}")
        if st != 200 or not r.get("ok") or not r.get("id"):
            die(f"relay send -> {st} {r.get('error', '')}")
        t0 = time.time()
        while time.time() - t0 < wait_s:
            time.sleep(3)
            st, p = self.call("GET", f"/api/devices/relay?inReplyTo={r['id']}")
            if st != 200:
                continue
            rep = p.get("reply")
            if rep:
                payload = rep.get("payload")
                try:
                    return json.loads(payload).get("result", payload)
                except (ValueError, AttributeError):
                    return payload
        return None

    def status(self, dev_id, wait_s=180):
        res = self.invoke(dev_id, "status", wait_s)
        if res is None:
            return None
        try:
            return json.loads(res)
        except (ValueError, TypeError):
            return {"_raw": res}

    def upload(self, data):
        st, r = self.call("POST", "/api/media",
                          {"data": base64.b64encode(data).decode(),
                           "contentType": "application/octet-stream"}, timeout=180)
        url = str(r.get("url", ""))
        if st != 200 or not url.startswith(MEDIA_PREFIX):
            die(f"POST /api/media -> {st} {r.get('error', '')} url={url!r}")
        if r.get("bytes") not in (None, len(data)):
            die(f"media stored {r.get('bytes')} B, sent {len(data)} B")
        return url


def is_ancestor(old, new):
    """True/False, or None when old is not a commit in this clone."""
    if subprocess.run(["git", "cat-file", "-e", f"{old}^{{commit}}"],
                      capture_output=True).returncode != 0:
        return None
    return subprocess.run(["git", "merge-base", "--is-ancestor", old, new],
                          capture_output=True).returncode == 0


def short(sha):
    return sha[:8]


def commit_of(fw_commit):
    """The sha inside a `git describe --always --abbrev=8 [--dirty]` stamp:
    'bb2f9323', 'bb2f9323-dirty', or 'v0.31.0-3-gbb2f9323[-dirty]'."""
    s = str(fw_commit or "")
    if s.endswith("-dirty"):
        s = s[: -len("-dirty")]
    if "-g" in s:
        s = s.rsplit("-g", 1)[1]
    return s


def ota_reply_kind(res):
    """Classify the device's reply to `ota <url> <sha256>` (tiny_node.cpp).

    'staged'      only the success line, which starts "OTA staged"
    'board-guard' the board-id refusal
    'refused'     everything else, including every "... - nothing staged"
                  refusal and "already up to date": a substring match on
                  "staged" would read those as success.
    """
    s = str(res or "").strip()
    if s.startswith("OTA staged"):
        return "staged"
    if s.startswith("OTA refused: board guard"):
        return "board-guard"
    return "refused"


def same_commit(fw_commit, sha):
    # describe --abbrev=8 grows past 8 when 8 is ambiguous: prefix-compare.
    c = commit_of(fw_commit)
    return len(c) >= 7 and sha.startswith(c)

# --- main -------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inspect", metavar="BIN", help="print the image's board stamp as JSON and exit")
    ap.add_argument("--token-days", action="store_true", help="print days until TINY_TOKEN expires")
    ap.add_argument("--board", help="board the target device must report (status.board)")
    ap.add_argument("--sha", help="commit the image was built from (CI sha)")
    ap.add_argument("--image", help="app image (tiny_sticky.bin)")
    ap.add_argument("--device", default=os.environ.get("TINY_DEVICE", ""), help="device id or name (env TINY_DEVICE)")
    ap.add_argument("--board-override", action="store_true",
                    help="send even if the image stamp is not --board (proves the firmware guard)")
    ap.add_argument("--creds", action="store_true", help="fall back to ~/.tiny/credentials.json for the token")
    ap.add_argument("--timeout", type=int, default=300, help="seconds to wait for the new fw_commit")
    a = ap.parse_args()

    if a.inspect:
        with open(a.inspect, "rb") as f:
            v, board, proj = parse_image(f.read(HEAD_LEN))
        print(json.dumps({"verdict": v, "board": board, "project": proj}))
        return 0 if v == "ok" else 3

    tok = load_token(a.creds)
    if a.token_days:
        exp = jwt_exp(tok) if tok else None
        if exp is None:
            die("TINY_TOKEN missing or not a JWT", 2)
        print(int((exp - time.time()) // 86400))
        return 0

    for k in ("board", "sha", "image", "device"):
        if not getattr(a, k):
            die(f"--{k} required", 2)
    if not tok:
        die("TINY_TOKEN not set", 2)
    sha = subprocess.run(["git", "rev-parse", a.sha], capture_output=True, text=True).stdout.strip() or a.sha
    data = open(a.image, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    verdict, img_board, proj = parse_image(data[:HEAD_LEN])
    step(f"image {a.image}: {len(data)} B sha256 {digest[:16]}… stamp={verdict}:{img_board or '-'} project={proj or '-'}")
    if verdict != "ok" or proj != PROJECT or img_board != a.board:
        if not a.board_override:
            die(f"image is not a {a.board} {PROJECT} image ({verdict}, board {img_board!r}, project {proj!r})")
        step(f"--board-override: sending a {img_board or verdict} image to a {a.board} board ON PURPOSE")

    api = Api(tok)
    dev = api.resolve(a.device)
    dev_id = str(dev["id"])
    step(f"device {dev.get('name')} ({dev_id[:8]}…) online={dev.get('online')}")
    if dev.get("online") is False:
        # A board with a TINY_BENCH_DEVICES entry is expected to be updated;
        # only a missing secret or entry may skip (the workflow's notice).
        die(f"{dev.get('name')} is offline (unplugged or asleep) - bench not updated")

    step("status (before)")
    s0 = api.status(dev_id)
    if not s0:
        die("status: no reply within 180 s (device asleep or offline?)")
    if s0.get("board") != a.board:
        die(f"device reports board {s0.get('board')!r}, job is for {a.board!r}")
    if "ota" in (s0.get("absent") or []):
        # Expected until the bootstrap USB flash; not a CI failure.
        print(f"::warning::ota_push: device fw {s0.get('fw')} ({s0.get('fw_commit')}) has OTA "
              "disabled - flash an OTA-capable image over USB once (AGENTS.md 10b bootstrap). Skipped.",
              flush=True)
        return 0
    before = str(s0.get("fw_commit", ""))
    step(f"on glass: fw {s0.get('fw')} fw_commit {before} grammar {s0.get('grammar_version')}")
    if same_commit(before, sha) and not before.endswith("-dirty"):
        step(f"already on {short(sha)} - nothing to do")
        return 0
    if not a.board_override:
        if before.endswith("-dirty") or before in ("", "nogit"):
            die(f"on-glass commit {before!r} is not a clean commit - flash it over USB once")
        anc = is_ancestor(commit_of(before), sha)
        if anc is False:
            die(f"refusing downgrade/sidegrade: {before} is not an ancestor of {short(sha)}")
        if anc is None:
            die(f"on-glass commit {before} is not in this clone's history - refusing to guess")
        step(f"ancestry ok: {before} is an ancestor of {short(sha)}")

    step("upload /api/media")
    url = api.upload(data)
    step(f"uploaded -> {url}")

    step(f"ota {url} {digest[:16]}…")
    res = api.invoke(dev_id, f"ota {url} {digest}", wait_s=240)
    if res is None:
        die("ota: no reply within 240 s")
    step(f"device: {res}")
    kind = ota_reply_kind(res)
    if kind != "staged":
        if a.board_override and kind == "board-guard":
            step("board guard refused the foreign image, as it must")
            s1 = api.status(dev_id) or {}
            if str(s1.get("fw_commit")) != before:
                die(f"device commit moved {before} -> {s1.get('fw_commit')} after a refusal")
            step(f"still on {before} - slot untouched")
            return 0
        die(f"ota refused: {res}")
    if a.board_override:
        die("FIRMWARE BOARD GUARD FAILED: a foreign image was staged")

    step(f"waiting up to {a.timeout} s for fw_commit {short(sha)}")
    t0 = time.time()
    time.sleep(20)  # reboot + Wi-Fi + first heartbeat
    while time.time() - t0 < a.timeout:
        s = api.status(dev_id, wait_s=90)
        if s:
            now = str(s.get("fw_commit", ""))
            step(f"status: fw {s.get('fw')} fw_commit {now} last_boot {s.get('last_boot')}")
            if same_commit(now, sha) and not now.endswith("-dirty"):
                step(f"UPDATED: {before} -> {now}")
                return 0
            if now == before and time.time() - t0 > 60:
                die(f"device came back on {before}: the trial image rolled back")
        time.sleep(10)
    die(f"timeout: fw_commit never became {short(sha)}")


if __name__ == "__main__":
    sys.exit(main())
