#!/usr/bin/env python3
"""mkset.py — turn an ESP-IDF build dir into a devbench image set.

    tools/devbench/mkset.py firmware/build-fnk0104s out/ci-fnk0104s --board fnk0104s
    tools/devbench/mkset.py firmware/build out/ci-reterminal --board reterminal --note "..."

Reads the build's flasher_args.json (offsets + flash settings, exactly what
`idf.py flash` would use), copies the parts flat into the set dir and writes
manifest.json in the format tools/devbench/images/README.md documents. CI runs
this per board, so the artifact it uploads is a ready-to-drop image set.
"""
import argparse
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def fw_version() -> str:
    h = (REPO / "firmware/main/tiny/tiny_version.h").read_text()
    m = re.search(r'TINY_FW_VERSION\s+"([^"]+)"', h)
    return m.group(1) if m else "?"


def commit() -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short=8", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "nogit"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=Path, help="IDF build dir containing flasher_args.json")
    ap.add_argument("out", type=Path, help="image set dir to create")
    ap.add_argument("--board", required=True, help="bench label(s) this set is for")
    ap.add_argument("--note", default="", help="extra text shown above the parts list")
    a = ap.parse_args()

    fa = json.loads((a.build / "flasher_args.json").read_text())
    chip = fa.get("extra_esptool_args", {}).get("chip", "")
    chip = chip.upper().replace("ESP32S", "ESP32-S").replace("ESP32C", "ESP32-C")

    a.out.mkdir(parents=True, exist_ok=True)
    parts = []
    for off, rel in sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        src = a.build / rel
        dst = a.out / Path(rel).name
        shutil.copyfile(src, dst)
        parts.append({"offset": off, "file": dst.name})

    sha = commit()
    version = f"{fw_version()} {a.board} ({sha})"
    note = f"Sticky {version}."
    if a.note:
        note += " " + a.note
    manifest = {
        "chip": chip,
        "boards": [a.board],
        "version": version,
    "commit": sha,
    "built_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime((a.build / "flasher_args.json").stat().st_mtime)),
        "note": note,
        "flash_settings": fa.get("flash_settings", {}),
        "parts": parts,
    }
    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{a.out}: {version}, {len(parts)} parts")


if __name__ == "__main__":
    main()
