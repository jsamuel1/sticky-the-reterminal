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

The page never flashes on its own: every write needs the owner's click and a
confirm dialog, including agent-queued jobs. Queue one with tools/devbench/job.py.

Reports, dumps and jobs live under tools/devbench/{reports,dumps,jobs}/ (gitignored).
Binds loopback only.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import time
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
IDF_BUILDS = [Path(p) for p in os.environ.get("DEVBENCH_IDF_BUILDS", str(REPO / "firmware" / "build")).split(os.pathsep) if p]
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
            name = slug("build-" + (b.parent.name if b.name == "build" else b.name))
            parts = []
            for off, rel in sorted(d["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
                p = (b / rel).resolve()
                parts.append({"offset": int(off, 16), "file": Path(rel).name, "path": p, **part_info(p)})
            desc = b / "project_description.json"
            proj = json.loads(desc.read_text()) if desc.is_file() else {}
            sets[name] = {"name": name, "chip": (d.get("extra_esptool_args", {}).get("chip") or proj.get("target") or "").upper().replace("ESP32S", "ESP32-S").replace("ESP32C", "ESP32-C"),
                          "source": str(b), "flash_settings": d.get("flash_settings", {}),
                          "version": proj.get("project_version"), "parts": parts}
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
        if p.startswith("/api/"):
            return self._json(404, {"error": "unknown endpoint"})
        return super().do_GET()

    def do_POST(self):
        p = self.path.split("?", 1)[0]
        try:
            raw = self._body()
        except ValueError as e:
            self.log_message("rejected body: %s (headers: CL=%s TE=%s)", e,
                             self.headers.get("Content-Length"), self.headers.get("Transfer-Encoding"))
            return self._json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": str(e)})
        REPORTS.mkdir(parents=True, exist_ok=True)
        if p == "/api/report":
            try:
                rep = json.loads(raw)
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
            q = dict(x.split("=", 1) for x in (self.path.split("?", 1)[1].split("&") if "?" in self.path else []) if "=" in x)
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
            q = dict(x.split("=", 1) for x in (self.path.split("?", 1)[1].split("&") if "?" in self.path else []) if "=" in x)
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
                res = json.loads(raw)
            except Exception:
                return self._json(400, {"error": "body must be JSON"})
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
