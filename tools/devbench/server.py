#!/usr/bin/env python3
"""devbench — the browser-side bench for USB-attached boards.

The boards are plugged into the OWNER's laptop; this server runs wherever the
agent runs (often a remote Linux box). The page it serves uses Web Serial in
the owner's Chrome/Edge to talk to the board, then POSTs what it learned back
here, where the agent can read it from disk. Nothing here touches a serial
port itself.

    python3 tools/devbench/server.py            # 127.0.0.1:8765
    DEVBENCH_PORT=9000 python3 tools/devbench/server.py

Endpoints (all relative, so the page works under a reverse-proxy prefix such
as the Kermes dashboard's /_browser-proxy/<id>/):

    GET  /                      the bench page
    GET  /vendor/*, /*.js       static assets from tools/devbench/
    POST /api/report            JSON report from the page -> reports/<ts>-<slug>.json
    GET  /api/reports           index of stored reports (newest first)
    GET  /api/reports/<name>    one stored report
    POST /api/log               append-only boot/console log chunk -> reports/<session>.log

    GET  /api/sets              flashable image sets (firmware/build via flasher_args.json,
                                plus tools/devbench/images/<set>/manifest.json), with size+md5
    GET  /api/sets/<set>/<file> one image part (raw bytes)
    POST /api/dump/<name>?offset=N   append a dump chunk at byte N -> dumps/<name>.bin
    POST /api/dump/<name>/done  {size, md5, ...meta}: server re-hashes the file and records it
    GET  /api/jobs              agent-queued requests (jobs/*.json) the owner can run from the page
    POST /api/jobs/<id>/result  page reports a job's outcome; the job file moves to jobs/done/

    GET  /api/x/<post-path>?b=<base64url body>   the same POST, body-free (X-Devbench: 1 required).
                                The Kermes browser pane reached over the AWS tunnel answers
                                every POST 502: the edge re-sends bodies chunked and the pane
                                proxy forwards Transfer-Encoding into undici fetch, which
                                refuses it ("invalid transfer-encoding header"). The page
                                falls back to this channel for small bodies (logs, reports,
                                job results, tiny calls). Not for dumps.

  Provisioning (the tiny-tech flash credential step; tiny.technology does not
  send CORS headers for its pairing API, so the browser cannot call it itself):
    GET  /api/tiny/status       {api, session, login}: is there an owner session on this host
    POST /api/tiny/lookup       {name} -> same-name rows (session only): id, online, kind
    POST /api/tiny/enroll       {name, adopt} -> session route: duplicate guard, adopt or
                                POST /api/devices -> {device_id, device_token, name, adopted}
    POST /api/tiny/pair/start   {name} -> {code, ttl_s, url}; the pairing secret is made
                                here and held in memory only
    POST /api/tiny/pair/poll    {code} -> pending | approved {device_id, device_token, name}
                                | denied | expired | claimed | vanished
  A device token passes through this process on its way to the page (which writes it
  into the NVS image) and is never stored, logged or echoed into reports.

The page never flashes on its own: every write needs the owner's click and a
confirm dialog, including agent-queued jobs. Queue one with tools/devbench/job.py.

Reports, dumps and jobs live under tools/devbench/{reports,dumps,jobs}/ (gitignored).
Binds loopback only.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import secrets
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent
REPORTS = Path(os.environ.get("DEVBENCH_REPORTS", ROOT / "reports"))
DUMPS = Path(os.environ.get("DEVBENCH_DUMPS", ROOT / "dumps"))
JOBS = Path(os.environ.get("DEVBENCH_JOBS", ROOT / "jobs"))
IMAGES = Path(os.environ.get("DEVBENCH_IMAGES", ROOT / "images"))
# IDF build dirs whose flasher_args.json becomes a set named after the dir.
_DEFAULT_BUILDS = os.pathsep.join(str(REPO / "firmware" / d) for d in ("build", "build-fnk0104s", "build-amoled18"))
IDF_BUILDS = [Path(p) for p in os.environ.get("DEVBENCH_IDF_BUILDS", _DEFAULT_BUILDS).split(os.pathsep) if p]
MAX_BODY = 4 * 1024 * 1024  # per request; dumps upload in 1 MiB chunks
MAX_DUMP = 64 * 1024 * 1024
SAFE = re.compile(r"[^A-Za-z0-9._-]+")
NAME = r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}"


def slug(s: str, n: int = 48) -> str:
    return SAFE.sub("-", s).strip("-")[:n] or "x"


def md5_file(p: Path) -> str:
    h = hashlib.md5()
    with open(p, "rb") as fh:
        for b in iter(lambda: fh.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


_md5_cache: dict[tuple, str] = {}


def part_info(path: Path) -> dict:
    st = path.stat()
    key = (str(path), st.st_size, st.st_mtime_ns)
    if key not in _md5_cache:
        _md5_cache[key] = md5_file(path)
    return {"size": st.st_size, "md5": _md5_cache[key], "mtime": int(st.st_mtime)}


def load_sets() -> dict[str, dict]:
    """name -> {name, chip, source, flash_settings, parts:[{offset, file, path(abs), size, md5}]}."""
    sets: dict[str, dict] = {}
    for b in IDF_BUILDS:
        fa = b / "flasher_args.json"
        if not fa.is_file():
            continue
        try:
            d = json.loads(fa.read_text())
            name = slug("build-" + (b.parent.name if b.name == "build" else b.name.removeprefix("build-")))
            parts = []
            for off, rel in sorted(d["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
                p = (b / rel).resolve()
                parts.append({"offset": int(off, 16), "file": Path(rel).name, "path": p, **part_info(p)})
            desc = b / "project_description.json"
            proj = json.loads(desc.read_text()) if desc.is_file() else {}
            sets[name] = {"name": name, "chip": (d.get("extra_esptool_args", {}).get("chip") or proj.get("target") or "").upper().replace("ESP32S", "ESP32-S").replace("ESP32C", "ESP32-C"),
                          "source": str(b), "flash_settings": d.get("flash_settings", {}),
                          "version": proj.get("project_version"), "boards": build_boards(b), "parts": parts}
        except Exception as e:  # a half-written build must not break the listing
            sets[slug("build-error-" + b.name)] = {"name": "error", "error": f"{fa}: {e}", "parts": []}
    if IMAGES.is_dir():
        for m in sorted(IMAGES.glob("*/manifest.json")):
            try:
                d = json.loads(m.read_text())
                parts = []
                for pt in d["parts"]:
                    p = (m.parent / pt["file"]).resolve()
                    if m.parent.resolve() not in p.parents:
                        raise ValueError(f"part escapes set dir: {pt['file']}")
                    off = pt["offset"] if isinstance(pt["offset"], int) else int(str(pt["offset"]), 0)
                    parts.append({"offset": off, "file": Path(pt["file"]).name, "path": p, **part_info(p)})
                name = slug(m.parent.name)
                sets[name] = {"name": name, "chip": d.get("chip", ""), "source": str(m.parent),
                              "flash_settings": d.get("flash_settings", {}), "version": d.get("version"),
                              "boards": d.get("boards", []), "note": d.get("note"), "parts": parts}
            except Exception as e:
                sets[slug("error-" + m.parent.name)] = {"name": "error", "error": f"{m}: {e}", "parts": []}
    return sets


def build_boards(b: Path) -> list[str]:
    """The TINY_BOARD a build dir was configured for (Kconfig.projbuild choice)."""
    try:
        cfg = json.loads((b / "config" / "sdkconfig.json").read_text())
    except Exception:
        return []
    return [k[len("TINY_BOARD_"):].lower() for k, v in cfg.items() if k.startswith("TINY_BOARD_") and v is True]


# ---- secrets hygiene -----------------------------------------------------------------
# Every device token this process hands to the page is remembered (in memory
# only) so that nothing the page sends back - logs, reports, job results - can
# carry it to disk, even if a future page bug logs it.
_issued: set[str] = set()
_issued_lock = threading.Lock()
TOKEN_RE = re.compile(rb"tind_[A-Za-z0-9_\-]{8,}")
SECRET_KEYS = re.compile(r"^(token|device_token|secret|secrethash|key|wifi_key|password|authorization)$", re.I)


def remember_token(tok: str | None) -> None:
    if tok:
        with _issued_lock:
            _issued.add(tok)


def scrub_bytes(raw: bytes) -> bytes:
    with _issued_lock:
        toks = list(_issued)
    for t in toks:
        raw = raw.replace(t.encode(), b"<redacted>")
    return TOKEN_RE.sub(b"<redacted>", raw)


def scrub(obj):
    """Drop secret-named fields anywhere in a JSON value, then scrub strings."""
    if isinstance(obj, dict):
        return {k: ("<redacted>" if SECRET_KEYS.match(str(k)) and obj[k] not in (None, "") else scrub(v)) for k, v in obj.items()}
    if isinstance(obj, list):
        return [scrub(v) for v in obj]
    if isinstance(obj, str):
        return scrub_bytes(obj.encode()).decode(errors="replace")
    return obj


# ---- tiny.technology ---------------------------------------------------------------
# Same session/claim rules as tools/provision_new.sh (provision_lib): TINY_TOKEN or
# ~/.tiny/credentials.json, TINY_API override, body:<slug> + via:<host> + firmware caps.
import sys  # noqa: E402
sys.path.insert(0, str(REPO / "tools"))
import provision_lib as plib  # noqa: E402


def tiny_session() -> dict | None:
    import contextlib, io  # noqa: E401
    with contextlib.redirect_stderr(io.StringIO()):  # load_creds narrates to stderr
        return plib.load_creds()


def tiny_api() -> str:
    return plib.api_base(tiny_session())


def tiny_call(method: str, path: str, body=None, auth: str | None = None, timeout: float = 20) -> tuple[int, dict]:
    try:
        st, d = plib.http(method, tiny_api() + path, body, token=auth, timeout=timeout)
    except Exception as e:  # DNS, TLS, timeout: the page shows it, nothing is retried blindly
        return 0, {"error": f"{type(e).__name__}: {e}"}
    if isinstance(d, list):
        d = {"devices": d}
    return st, d if isinstance(d, dict) else {}


NAME_RE = re.compile(r"tiny-[0-9a-f]{4}")


def sticky_claim(name: str) -> dict:
    return {"name": name, "platform": plib.PLATFORM, "kind": plib.KIND, "capabilities": plib.capabilities()}


def host_device_id() -> str | None:
    caps = plib.capabilities()
    return next((c[4:] for c in caps if c.startswith("via:")), None)


# code -> {secret, name, started, ttl}. In memory only: a restart loses pending
# pairings, which is the safe direction (the owner starts again).
_pairs: dict[str, dict] = {}
_pairs_lock = threading.Lock()


def tiny_route(p: str, req: dict) -> tuple[int, dict]:
    """The /api/tiny/* contract (module docstring). Returns (status, json)."""
    sess = tiny_session()
    if p == "/api/tiny/status":
        return 200, {"api": tiny_api(), "session": bool(sess), "login": (sess or {}).get("login"),
                     "via": host_device_id(), "capabilities": plib.capabilities()}
    name = str(req.get("name") or "")
    if p in ("/api/tiny/lookup", "/api/tiny/enroll", "/api/tiny/pair/start") and not NAME_RE.fullmatch(name):
        return 400, {"error": f"name must look like tiny-xxxx (the board's own softAP name), got {name!r}"}
    if p == "/api/tiny/lookup":
        if not sess:
            return 200, {"session": False, "rows": None}
        st, d = tiny_call("GET", "/api/devices", auth=sess["token"])
        if st != 200:
            return 502, {"error": f"GET /api/devices: {st} {d.get('error', '')}".strip()}
        rows = [{"id": r.get("id"), "online": r.get("online"), "kind": r.get("kind"), "last_seen": r.get("last_seen")}
                for r in (d.get("devices") if isinstance(d, dict) else d) or [] if r.get("name") == name and r.get("kind") != "endpoint"]
        return 200, {"session": True, "rows": rows}
    if p == "/api/tiny/enroll":
        if not sess:
            return 409, {"error": "no tiny.technology session on this host; use the pairing code"}
        st, d = tiny_call("GET", "/api/devices", auth=sess["token"])
        if st != 200:
            return 502, {"error": f"GET /api/devices: {st} {d.get('error', '')}".strip()}
        same = [r for r in (d.get("devices") if isinstance(d, dict) else d) or [] if r.get("name") == name and r.get("kind") != "endpoint"]
        live = next((r for r in same if r.get("online") is True), None)
        if live and not req.get("adopt"):
            return 409, {"error": f'"{name}" is ONLINE as {str(live.get("id"))[:8]}... - this board (or its twin) is heartbeating. Tick "adopt" to re-key that row.',
                         "refused": "online"}
        if live:
            st, r = tiny_call("POST", "/api/devices/adopt", {"deviceId": live["id"]}, auth=sess["token"])
            adopted = True
        else:
            # a stale same-name row is re-keyed by the worker (same id, new token); never a ghost row
            st, r = tiny_call("POST", "/api/devices", sticky_claim(name), auth=sess["token"])
            adopted = bool(r.get("adopted")) or bool(same)
        if not r.get("device_token") or not r.get("device_id"):
            return 502, {"error": f"{'adopt' if live else 'enroll'} failed: {st} {r.get('error') or 'no token'}"}
        remember_token(r["device_token"])
        return 200, {"device_id": r["device_id"], "device_token": r["device_token"], "name": name, "api": tiny_api(),
                     "adopted": adopted, "via": "session"}
    if p == "/api/tiny/pair/start":
        secret = secrets.token_urlsafe(32)  # 32 random bytes, base64url (tiny-tech enroll.ts)
        st, d = tiny_call("POST", "/api/devices/pair/start", {**sticky_claim(name), "secretHash": hashlib.sha256(secret.encode()).hexdigest()})
        code = str(d.get("code") or "")
        if st != 200 or not code:
            return 502, {"error": f"pairing could not start: {st} {d.get('error', '')}".strip()}
        ttl = int(d.get("ttl_s") or 600)
        with _pairs_lock:
            _pairs[code] = {"secret": secret, "name": name, "until": time.time() + ttl}
        return 200, {"code": code, "ttl_s": ttl, "url": f"{tiny_api()}/devices/pair?code={urllib.parse.quote(code)}", "claim": sticky_claim(name)}
    if p == "/api/tiny/pair/poll":
        code = str(req.get("code") or "")
        with _pairs_lock:
            pend = _pairs.get(code)
        if not pend:
            return 404, {"status": "vanished", "error": "no pairing in progress with that code on this bench (server restarted?)"}
        if time.time() > pend["until"]:
            with _pairs_lock:
                _pairs.pop(code, None)
            return 200, {"status": "expired"}
        st, d = tiny_call("POST", "/api/devices/pair/poll", {"code": code, "secret": pend["secret"]})
        if st == 404:
            with _pairs_lock:
                _pairs.pop(code, None)
            return 200, {"status": "vanished"}
        status = d.get("status")
        if st == 200 and status == "approved" and d.get("device_token"):
            with _pairs_lock:
                _pairs.pop(code, None)
            remember_token(d["device_token"])
            return 200, {"status": "approved", "device_id": d.get("device_id"), "device_token": d["device_token"],
                         "name": d.get("name") or pend["name"], "api": tiny_api(), "via": "pair"}
        if status in ("denied", "expired", "claimed"):
            with _pairs_lock:
                _pairs.pop(code, None)
            return 200, {"status": status}
        if st == 0:
            return 200, {"status": "pending", "note": d.get("error")}  # network blip: keep polling
        return 200, {"status": "pending"}
    return 404, {"error": "unknown tiny endpoint"}


# ---- body-free upload channel ------------------------------------------------------------
# GET /api/x/<post-path>?<inner query>&b=<b64url piece>&u=<upload id>&i=<n>&n=<total>
# Pieces are buffered per upload id (in memory, capped, 2 min TTL) and the whole
# body is dispatched as the POST it stands for once all n have arrived.
X_MAX = 1 << 20
_uploads: dict[str, dict] = {}
_uploads_lock = threading.Lock()


def x_piece(uid: str, i: int, n: int, piece: bytes) -> bytes | None:
    now = time.time()
    with _uploads_lock:
        for k in [k for k, v in _uploads.items() if now - v["t"] > 120]:
            del _uploads[k]
        u = _uploads.setdefault(uid, {"t": now, "n": n, "parts": {}})
        if u["n"] != n or not 0 <= i < n:
            raise ValueError("piece out of range")
        u["parts"][i] = piece
        u["t"] = now
        if sum(len(x) for x in u["parts"].values()) > X_MAX:
            del _uploads[uid]
            raise ValueError(f"upload over {X_MAX} bytes")
        if len(u["parts"]) < n:
            return None
        del _uploads[uid]
        return b"".join(u["parts"][k] for k in range(n))


def public(s: dict) -> dict:
    return {**s, "parts": [{k: v for k, v in p.items() if k != "path"} for p in s.get("parts", [])]}


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(ROOT), **kw)

    # Only the page and its assets are served; never reports/ or the server.
    def translate_path(self, path):
        p = path.split("?", 1)[0].split("#", 1)[0]
        if p in ("", "/"):
            p = "/index.html"
        allowed = p.startswith("/vendor/") or re.fullmatch(r"/[A-Za-z0-9_-]+\.(html|js|css)", p)
        if not allowed:
            return str(ROOT / "__denied__")
        return super().translate_path(p)

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()

    def log_message(self, fmt, *args):
        if os.environ.get("DEVBENCH_QUIET") != "1":
            # /api/x carries a body in its query string: log the path only.
            args = tuple(re.sub(r"(/api/x/[^?\s]*)\?\S*", r"\1?<body>", a) if isinstance(a, str) else a for a in args)
            super().log_message(fmt, *args)

    def _json(self, code: int, obj) -> None:
        body = json.dumps(obj, indent=2).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        # The Kermes dashboard proxy forwards bodies chunked (no Content-Length),
        # so accept both framings.
        if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
            out = bytearray()
            while True:
                size = int(self.rfile.readline().split(b";", 1)[0].strip() or b"0", 16)
                if size == 0:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass  # trailers
                    break
                out += self.rfile.read(size)
                self.rfile.readline()  # CRLF after chunk
                if len(out) > MAX_BODY:
                    raise ValueError(f"body over {MAX_BODY} bytes")
            if not out:
                raise ValueError("empty body")
            return bytes(out)
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > MAX_BODY:
            raise ValueError(f"body length {n} outside 1..{MAX_BODY}")
        return self.rfile.read(n)

    def do_GET(self):
        p = self.path.split("?", 1)[0]
        if p == "/api/sets":
            return self._json(200, [public(s) for s in load_sets().values()])
        m = re.fullmatch(rf"/api/sets/({NAME})/({NAME})", p)
        if m:
            s = load_sets().get(m.group(1))
            part = next((x for x in (s or {}).get("parts", []) if x["file"] == m.group(2)), None)
            if not part:
                return self._json(404, {"error": "no such set/part"})
            data = part["path"].read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Devbench-Md5", part["md5"])
            self.end_headers()
            self.wfile.write(data)
            return
        if p == "/api/jobs":
            JOBS.mkdir(parents=True, exist_ok=True)
            jobs = []
            for f in sorted(JOBS.glob("*.json")):
                try:
                    jobs.append({"id": f.stem, **json.loads(f.read_text())})
                except Exception as e:
                    jobs.append({"id": f.stem, "error": str(e)})
            return self._json(200, jobs)
        if p == "/api/dumps":
            DUMPS.mkdir(parents=True, exist_ok=True)
            return self._json(200, [json.loads(f.read_text()) for f in sorted(DUMPS.glob("*.json"), reverse=True)][:100])
        if p == "/api/reports":
            REPORTS.mkdir(parents=True, exist_ok=True)
            items = sorted(REPORTS.glob("*.json"), key=lambda f: f.stat().st_mtime, reverse=True)
            return self._json(200, [{"name": f.name, "bytes": f.stat().st_size} for f in items[:200]])
        m = re.fullmatch(r"/api/reports/([A-Za-z0-9._-]+\.(json|log))", p)
        if m:
            f = REPORTS / m.group(1)
            if not f.is_file():
                return self._json(404, {"error": "not found"})
            data = f.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "application/json" if f.suffix == ".json" else "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if p == "/api/tiny/status":
            if self.headers.get("X-Devbench") != "1":
                return self._json(403, {"error": "X-Devbench: 1 required"})
            return self._json(*tiny_route(p, {}))
        m = re.fullmatch(r"/api/x(/api/.+)", p)
        if m:
            return self._via_get(m.group(1))
        if p.startswith("/api/"):
            return self._json(404, {"error": "unknown endpoint"})
        return super().do_GET()

    def _via_get(self, inner: str):
        # A custom header forces a CORS preflight on any cross-site attempt, which
        # this server never answers, so a foreign page cannot drive writes by GET.
        if self.headers.get("X-Devbench") != "1":
            return self._json(403, {"error": "X-Devbench: 1 required"})
        q = urllib.parse.parse_qs(self.path.split("?", 1)[1] if "?" in self.path else "", keep_blank_values=True)
        one = lambda k, d="": (q.get(k) or [d])[0]  # noqa: E731
        try:
            piece = base64.urlsafe_b64decode(one("b") + "=" * (-len(one("b")) % 4))
            body = x_piece(slug(one("u", "x")), int(one("i", "0")), int(one("n", "1")), piece)
        except (ValueError, TypeError) as e:
            return self._json(400, {"error": f"bad piece: {e}"})
        if body is None:
            return self._json(202, {"piece": int(one("i", "0"))})
        if inner.startswith("/api/dump/"):
            return self._json(400, {"error": "dumps need a real POST"})
        rest = {k: v for k, v in q.items() if k not in ("b", "u", "i", "n")}
        return self._dispatch(inner, urllib.parse.urlencode(rest, doseq=True), body)

    def do_POST(self):
        p, _, query = self.path.partition("?")
        try:
            raw = self._body()
        except ValueError as e:
            self.log_message("rejected body: %s (headers: CL=%s TE=%s)", e,
                             self.headers.get("Content-Length"), self.headers.get("Transfer-Encoding"))
            return self._json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": str(e)})
        return self._dispatch(p, query, raw)

    def _dispatch(self, p: str, query: str, raw: bytes):
        q = {k: v[0] for k, v in urllib.parse.parse_qs(query, keep_blank_values=True).items()}
        REPORTS.mkdir(parents=True, exist_ok=True)
        if p.startswith("/api/tiny/"):
            # Forces a CORS preflight (never answered) on any cross-site attempt.
            if self.headers.get("X-Devbench") != "1":
                return self._json(403, {"error": "X-Devbench: 1 required"})
            try:
                req = json.loads(raw or b"{}")
                assert isinstance(req, dict)
            except Exception:
                return self._json(400, {"error": "body must be a JSON object"})
            return self._json(*tiny_route(p, req))
        if p == "/api/report":
            try:
                rep = scrub(json.loads(raw))
                assert isinstance(rep, dict)
            except Exception:
                return self._json(400, {"error": "body must be a JSON object"})
            rep["received_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
            label = slug(str(rep.get("label") or rep.get("chip", {}).get("mac") or "board"))
            name = f"{time.strftime('%Y%m%d-%H%M%S')}-{label}.json"
            (REPORTS / name).write_text(json.dumps(rep, indent=2))
            (REPORTS / f"latest-{label}.json").write_text(json.dumps(rep, indent=2))
            return self._json(201, {"stored": name})
        if p == "/api/log":
            raw = scrub_bytes(raw)
            session = slug(self.headers.get("X-Devbench-Session") or q.get("s") or "session")
            label = slug(self.headers.get("X-Devbench-Label") or q.get("l") or "board")
            with open(REPORTS / f"{session}.log", "ab") as fh:
                fh.write(raw)
            # One rolling file across sessions, each chunk stamped, so the agent can
            # `tail` a single path without knowing the session id.
            with open(REPORTS / "devbench.log", "ab") as fh:
                fh.write(f"\n--- {time.strftime('%Y-%m-%dT%H:%M:%S')} {session} {label} ---\n".encode())
                fh.write(raw)
            return self._json(202, {"appended": len(raw), "file": f"{session}.log"})
        m = re.fullmatch(rf"/api/dump/({NAME})", p)
        if m:
            off = int(q.get("offset", "-1"))
            DUMPS.mkdir(parents=True, exist_ok=True)
            f = DUMPS / f"{m.group(1)}.bin"
            cur = f.stat().st_size if f.exists() else 0
            if off == 0 and cur:
                f.unlink()
                cur = 0
            if off != cur or off + len(raw) > MAX_DUMP:
                return self._json(409, {"error": f"offset {off} != current size {cur} (or over {MAX_DUMP})", "size": cur})
            with open(f, "ab") as fh:
                fh.write(raw)
            return self._json(202, {"size": cur + len(raw)})
        m = re.fullmatch(rf"/api/dump/({NAME})/done", p)
        if m:
            f = DUMPS / f"{m.group(1)}.bin"
            try:
                meta = json.loads(raw)
            except Exception:
                return self._json(400, {"error": "body must be JSON"})
            if not f.is_file():
                return self._json(404, {"error": "no such dump"})
            got = {"size": f.stat().st_size, "md5": md5_file(f)}
            ok = got["size"] == meta.get("size") and got["md5"] == meta.get("md5")
            rec = {**meta, "file": f.name, "server": got, "verified": ok, "received_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
            (DUMPS / f"{m.group(1)}.json").write_text(json.dumps(rec, indent=2))
            return self._json(200 if ok else 422, rec)
        m = re.fullmatch(rf"/api/jobs/({NAME})/result", p)
        if m:
            f = JOBS / f"{m.group(1)}.json"
            if not f.is_file():
                return self._json(404, {"error": "no such job"})
            try:
                res = scrub(json.loads(raw))
                assert isinstance(res, dict)
            except Exception:
                return self._json(400, {"error": "body must be a JSON object"})
            (JOBS / "done").mkdir(exist_ok=True)
            job = json.loads(f.read_text())
            job["result"] = {**res, "at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
            (JOBS / "done" / f.name).write_text(json.dumps(job, indent=2))
            f.unlink()
            return self._json(200, job)
        return self._json(404, {"error": "unknown endpoint"})


def main() -> None:
    host = os.environ.get("DEVBENCH_HOST", "127.0.0.1")
    port = int(os.environ.get("DEVBENCH_PORT", "8765"))
    srv = ThreadingHTTPServer((host, port), Handler)
    print(f"devbench on http://{host}:{port}/  reports -> {REPORTS}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
