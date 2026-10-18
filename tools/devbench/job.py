#!/usr/bin/env python3
"""Queue a devbench job for the owner to run from the bench page.

The page polls /api/jobs and shows each job as a banner with a Run button.
Nothing runs without the owner's click; writes also get a confirm dialog that
names the board, the image set and whether flash is erased.

    tools/devbench/job.py probe   --label fnk0104s
    tools/devbench/job.py boot    --label amoled18-v2 --secs 15
    tools/devbench/job.py dump    --label fnk0104s --what partition:app0
    tools/devbench/job.py dump    --label fnk0104s --what all
    tools/devbench/job.py dump    --label fnk0104s --what 0x8000:0x1000
    tools/devbench/job.py flash   --label fnk0104s --set build-firmware [--erase-all]
    tools/devbench/job.py list                   # pending + recent results
    tools/devbench/job.py wait <id> [--timeout 600]

Results land in jobs/done/<id>.json (and the usual reports/dumps).
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent
JOBS = ROOT / "jobs"
KINDS = ("probe", "boot", "dump", "flash")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=(*KINDS, "list", "wait"))
    ap.add_argument("id", nargs="?")
    ap.add_argument("--label", help="board label the job is for (page refuses on mismatch)")
    ap.add_argument("--set", dest="image_set", help="flash: image set name from /api/sets")
    ap.add_argument("--erase-all", action="store_true", help="flash: erase whole chip first")
    ap.add_argument("--what", default="all", help="dump: all | partition:<name> | <offset>:<length>")
    ap.add_argument("--secs", type=int, default=10, help="boot: capture seconds")
    ap.add_argument("--note", default="", help="shown to the owner on the banner")
    ap.add_argument("--timeout", type=int, default=900)
    a = ap.parse_args()
    JOBS.mkdir(parents=True, exist_ok=True)

    if a.kind == "list":
        for f in sorted(JOBS.glob("*.json")):
            print("pending", f.stem, f.read_text().replace("\n", " ")[:160])
        for f in sorted((JOBS / "done").glob("*.json"), key=lambda p: p.stat().st_mtime)[-10:]:
            j = json.loads(f.read_text())
            print("done   ", f.stem, j["kind"], j.get("label"), "->", json.dumps(j.get("result", {}))[:160])
        return 0
    if a.kind == "wait":
        done = JOBS / "done" / f"{a.id}.json"
        t0 = time.time()
        while time.time() - t0 < a.timeout:
            if done.is_file():
                print(done.read_text())
                return 0 if json.loads(done.read_text()).get("result", {}).get("ok") else 1
            time.sleep(2)
        print(f"timed out waiting for {a.id}", file=sys.stderr)
        return 2

    if not a.label:
        ap.error("--label is required")
    if a.kind == "flash" and not a.image_set:
        ap.error("flash needs --set")
    job = {"kind": a.kind, "label": a.label, "note": a.note, "queued_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
    if a.kind == "flash":
        job.update(set=a.image_set, erase_all=a.erase_all)
    if a.kind == "dump":
        job.update(what=a.what)
    if a.kind == "boot":
        job.update(secs=a.secs)
    jid = f"{time.strftime('%Y%m%d-%H%M%S')}-{a.kind}-{uuid.uuid4().hex[:6]}"
    tmp = JOBS / f".{jid}.tmp"
    tmp.write_text(json.dumps(job, indent=2))
    tmp.rename(JOBS / f"{jid}.json")  # atomic: the page never sees a half-written job
    print(jid)
    return 0


if __name__ == "__main__":
    sys.exit(main())
