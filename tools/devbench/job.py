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
    tools/devbench/job.py flash   --label fnk0104s --set build-fnk0104s --provision [--adopt] [--dry-run]
    tools/devbench/job.py list                   # pending + recent results
    tools/devbench/job.py wait <id> [--timeout 600]

--provision is `npx tiny-tech flash --body sticky-the-reterminal` on the bench:
the page reads the MAC, names the board tiny-XXXX, enrols it (the bench host's
tiny session, else a pairing code the owner approves) and writes its identity
+ Wi-Fi into NVS in the same flash. The owner types the Wi-Fi on the Flash tab;
it never rides in a job file. --dry-run identifies the board and shows the plan;
nothing is written or enrolled.

Results land in jobs/done/<id>.json (and the usual reports/dumps).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent
JOBS = Path(os.environ.get("DEVBENCH_JOBS", ROOT / "jobs"))  # same override as server.py
KINDS = ("probe", "boot", "dump", "flash")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=(*KINDS, "list", "wait"))
    ap.add_argument("id", nargs="?")
    ap.add_argument("--label", help="board label the job is for (page refuses on mismatch)")
    ap.add_argument("--set", dest="image_set", help="flash: image set name from /api/sets")
    ap.add_argument("--erase-all", action="store_true", help="flash: erase whole chip first")
    ap.add_argument("--provision", action="store_true", help="flash: enrol + write identity/Wi-Fi NVS in the same flash")
    ap.add_argument("--adopt", action="store_true", help="flash --provision: re-key a same-name row that is online")
    ap.add_argument("--dry-run", action="store_true", help="flash: identify and show the plan only")
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
    if a.kind != "flash" and (a.provision or a.adopt or a.dry_run):
        ap.error("--provision/--adopt/--dry-run are flash options")
    if a.adopt and not a.provision:
        ap.error("--adopt needs --provision")
    job = {"kind": a.kind, "label": a.label, "note": a.note, "queued_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
    if a.kind == "flash":
        job.update(set=a.image_set, erase_all=a.erase_all, provision=a.provision, adopt=a.adopt, dry_run=a.dry_run)
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
