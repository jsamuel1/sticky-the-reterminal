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

Reports land in tools/devbench/reports/ (gitignored). Binds loopback only.
"""
from __future__ import annotations

import json
import os
import re
import time
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPORTS = Path(os.environ.get("DEVBENCH_REPORTS", ROOT / "reports"))
MAX_BODY = 4 * 1024 * 1024  # a boot log plus metadata is far below this
SAFE = re.compile(r"[^A-Za-z0-9._-]+")


def slug(s: str, n: int = 48) -> str:
    return SAFE.sub("-", s).strip("-")[:n] or "x"


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
        return self._json(404, {"error": "unknown endpoint"})


def main() -> None:
    host = os.environ.get("DEVBENCH_HOST", "127.0.0.1")
    port = int(os.environ.get("DEVBENCH_PORT", "8765"))
    srv = ThreadingHTTPServer((host, port), Handler)
    print(f"devbench on http://{host}:{port}/  reports -> {REPORTS}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
