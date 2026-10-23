"""mkdocs hook — ship the firmware binaries next to the install manifest.

docs/install/manifest.json (written by tools/release.sh) points at fw/<part>.bin,
and each docs/install/manifest-<board>.json at fw/<board>/<part>.bin. Those
bytes are never committed; at build time this hook copies them from
firmware/build (reTerminal) or firmware/build-<board> into site/install/fw/ and
refuses to build a site whose parts do not match the manifest's sha256sums
file — a manifest must never describe bytes other than the ones it ships with.

Also mirrors docs/install/install.sh to site/install.sh so the short URL
  curl -fsSL https://cagataycali.github.io/sticky-the-reterminal/install.sh | sh
works alongside the canonical /install/install.sh.

Set STICKY_SKIP_FW=1 to build the docs without a firmware/build dir (the
install page then 404s on parts — fine for text-only previews, never deploy).
"""
import hashlib
import json
import os
import shutil

from mkdocs.exceptions import PluginError

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INSTALL = os.path.join(ROOT, "docs", "install")


# manifest file -> (build dir, sums file). manifest.json is the reTerminal's and
# must ship; a board manifest ships when its manifest is committed.
BOARDS = {
    "manifest.json": ("build", "sha256sums.txt"),
    "manifest-reterminal.json": ("build", "sha256sums-reterminal.txt"),
    "manifest-fnk0104s.json": ("build-fnk0104s", "sha256sums-fnk0104s.txt"),
    "manifest-amoled18.json": ("build-amoled18", "sha256sums-amoled18.txt"),
}


def _flasher_parts(build):
    fa = json.load(open(os.path.join(build, "flasher_args.json")))
    return {os.path.basename(v): os.path.join(build, v) for v in fa["flash_files"].values()}


def _expected_sums(sums_name):
    sums = {}
    p = os.path.join(INSTALL, sums_name)
    if not os.path.exists(p):
        return sums
    for line in open(p):
        line = line.strip()
        if line:
            digest, name = line.split(None, 1)
            sums[name] = digest
    return sums


def on_post_build(config, **kwargs):
    site = config["site_dir"]
    src_sh = os.path.join(INSTALL, "install.sh")
    if os.path.exists(src_sh):
        shutil.copy2(src_sh, os.path.join(site, "install.sh"))

    if os.environ.get("STICKY_SKIP_FW") == "1":
        return
    for mname, (bdir, sums_name) in BOARDS.items():
        manifest_path = os.path.join(INSTALL, mname)
        if not os.path.exists(manifest_path):
            if mname == "manifest.json":
                raise PluginError("install_assets_hook: docs/install/manifest.json missing — run tools/release.sh")
            continue
        build = os.path.join(ROOT, "firmware", bdir)
        if not os.path.exists(os.path.join(build, "flasher_args.json")):
            raise PluginError(
                f"install_assets_hook: firmware/{bdir} is missing for {mname} — run tools/build_board.sh "
                "and tools/release.sh (or STICKY_SKIP_FW=1 for a text-only preview)"
            )
        manifest = json.load(open(manifest_path))
        expected = _expected_sums(sums_name)
        parts = _flasher_parts(build)
        for b in manifest["builds"]:
            for part in b["parts"]:
                rel = part["path"]  # fw/<name> or fw/<board>/<name>
                src = parts.get(os.path.basename(rel))
                if not src or not os.path.exists(src):
                    raise PluginError(f"install_assets_hook: {mname} part {rel} not in firmware/{bdir}")
                data = open(src, "rb").read()
                if expected.get(rel) and expected[rel] != hashlib.sha256(data).hexdigest():
                    raise PluginError(
                        f"install_assets_hook: {rel} in firmware/{bdir} does not match "
                        f"{sums_name} — re-run tools/release.sh before deploying"
                    )
                dst = os.path.join(site, "install", rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(src, dst)
    # the sums files live in docs/install and are copied by mkdocs itself.
