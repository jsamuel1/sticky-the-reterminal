#!/usr/bin/env python3
"""tools/ota_push.py decision cases (no network, no device).

1. ota_reply_kind: only the firmware's success line counts as staged. Every
   reply string tiny_node.cpp sends for `ota <url> <sha256>` is listed; the
   refusals all contain "staged" ("... - nothing staged"), so a substring test
   would pass them.
2. A configured bench board that is offline fails the push (exit != 0): only
   a missing secret or map entry may skip, and that is the workflow's job.
3. --board-override still sends when the device already reports --sha: this
   mode proves the firmware board guard, not whether an update is needed.
"""
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ota_push", os.path.join(HERE, "../../tools/ota_push.py"))
op = importlib.util.module_from_spec(spec)
spec.loader.exec_module(op)

fails = 0


def check(name, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} {name}: {got!r}" + ("" if ok else f", want {want!r}"))


# Verbatim from firmware/main/tiny_node.cpp (the `ota` verb's replies).
REPLIES = {
    "OTA staged + sha verified \u2014 rebooting into trial (rollback-armed)": "staged",
    "OTA refused: usage ota <url> <sha256>": "refused",
    "OTA refused: url must be https://plugin.tiny.technology/media/<key> "
    "and sha256 64 hex - nothing staged": "refused",
    "OTA refused: board guard - image is not stamped for this board ("
    "see log) - nothing staged, slot untouched": "board-guard",
    "OTA refused: sha256 mismatch - nothing staged": "refused",
    "already up to date (0.31.0)": "refused",
    "OTA refused: OTA is disabled on this board - flash it over USB": "refused",
    "": "refused",
    None: "refused",
}
for text, want in REPLIES.items():
    check(f"reply_kind {str(text)[:48]!r}", op.ota_reply_kind(text), want)

# Every firmware reply must be in the table above, so a new refusal line that
# happens to mention "staged" cannot slip past this test unclassified.
src = open(os.path.join(HERE, "../../firmware/main/tiny_node.cpp"), encoding="utf-8").read()
for needle in ("nothing staged", "OTA staged"):
    n_src = src.count(needle)
    n_tab = sum(needle in str(t) for t in REPLIES)
    check(f"replies containing {needle!r} covered", n_tab >= n_src, True)


# --- offline configured board + same-commit override ------------------------
class OfflineApi:
    def __init__(self, token):
        pass

    def resolve(self, dev):
        return {"id": "4e6ed293-0000", "name": "tiny-eba6", "online": False}

    def status(self, *a, **k):
        raise AssertionError("must not relay to an offline board")


class OverrideApi:
    sent = False

    def __init__(self, token):
        pass

    def resolve(self, dev):
        return {"id": "4e6ed293-0000", "name": "tiny-eba6", "online": True}

    def status(self, *a, **k):
        return {"board": "fnk0104s", "fw_commit": self.sha, "grammar_version": 19,
                "absent": []}

    def upload(self, data):
        return "https://plugin.tiny.technology/media/foreign.bin"

    def invoke(self, dev_id, prompt, wait_s=180):
        type(self).sent = True
        return "OTA refused: board guard - image is not stamped for this board " \
               "(see log) - nothing staged, slot untouched"


def run_main(argv):
    # Capture the pusher's output: its "::error::" line is the expected
    # result here and must not become a GitHub annotation on a green run.
    import contextlib
    import io
    old = sys.argv
    sys.argv = ["ota_push.py"] + argv
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            return op.main()
    except SystemExit as e:
        return e.code
    finally:
        sys.argv = old
        print("  (pusher said: " + buf.getvalue().strip().replace("::error::", "error: ") + ")")


with tempfile.TemporaryDirectory() as td:
    # A stamped fnk0104s image from the C++ fixture writer.
    cxx = os.path.join(td, "ota_test")
    subprocess.run(["c++", "-std=c++17", "-I", os.path.join(HERE, "../../firmware/main"),
                    os.path.join(HERE, "ota_cases.cpp"), "-o", cxx], check=True)
    subprocess.run([cxx, td], check=True, stdout=subprocess.DEVNULL)
    os.environ["TINY_TOKEN"] = "x.e30.x"
    op.Api = OfflineApi
    sha = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                         cwd=HERE).stdout.strip()
    code = run_main(["--board", "fnk0104s", "--sha", sha, "--image", os.path.join(td, "own.bin"),
                     "--device", "4e6ed293-0000"])
    check("offline configured board exits non-zero", bool(code), True)

    # A same-commit short-circuit is correct for ordinary updates, but would
    # make the deliberate foreign-image probe a false pass. The override must
    # upload, invoke OTA, observe the board guard, then prove the commit stayed.
    OverrideApi.sha = sha
    OverrideApi.sent = False
    op.Api = OverrideApi
    code = run_main(["--board", "fnk0104s", "--sha", sha,
                     "--image", os.path.join(td, "foreign.bin"),
                     "--device", "4e6ed293-0000", "--board-override"])
    check("same-commit override exits success after guard", code, 0)
    check("same-commit override sends OTA probe", OverrideApi.sent, True)

if fails:
    print(f"ota_push cases: {fails} failed")
    sys.exit(1)
print("ota_push cases passed")
