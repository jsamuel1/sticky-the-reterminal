#!/bin/bash
# sd report host test — source guard over the SHIPPED firmware for the two
# integration rules the policy header cannot hold on its own:
#  1. Every sd object that `status` (tiny_node_status_json) and `sd status`
#     report comes from one sticky_sdcard_report() call, which reconciles
#     hot-plug under the driver lock. Reading present/mounted/info directly
#     there let a card pushed into the FNK0104S's detect-less slot after an
#     empty boot stay invisible to `status` until some other verb probed.
#  2. On the SDMMC driver (no detect pin) present == mounted: present() and
#     report() never claim a card that is not mounted.
# Every driver that ships sticky_sdcard.h must define sticky_sdcard_report.
set -euo pipefail
cd "$(dirname "$0")"
python3 - ../../firmware/main <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
fails = []
def fail(m): fails.append(m); print("FAIL", m)
def ok(m): print("PASS", m)

def body(text, signature):
    i = text.find(signature)
    if i < 0: return None
    j = text.index("{", i); depth = 0
    for k in range(j, len(text)):
        depth += {"{": 1, "}": -1}.get(text[k], 0)
        if depth == 0: return text[j:k + 1]
    return None

def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)

node = (root / "tiny_node.cpp").read_text()
direct = re.compile(r"\bsticky_sdcard_(present|mounted|info)\s*\(")

st = body(node, 'extern "C" esp_err_t tiny_node_status_json(')
if st is None: fail("tiny_node_status_json definition not found")
else:
    st = strip_comments(st)
    if "sticky_sdcard_report(" not in st: fail("status json does not call sticky_sdcard_report()")
    else: ok("status json reads sd through sticky_sdcard_report()")
    if direct.search(st): fail(f"status json reads sticky_sdcard_{direct.search(st).group(1)}() directly")
    else: ok("status json has no direct present/mounted/info read")

m = re.search(r'\}\s*else\s*\{\s*//\s*"", "status", "df"(.*?)reply\(envelope_id', node, re.S)
if not m: fail('`sd` status/df branch not found')
else:
    br = strip_comments(m.group(1))
    if "sticky_sdcard_report(" not in br: fail("`sd status` does not call sticky_sdcard_report()")
    else: ok("`sd status` reads sd through sticky_sdcard_report()")
    if direct.search(br): fail(f"`sd status` reads sticky_sdcard_{direct.search(br).group(1)}() directly")
    else: ok("`sd status` has no direct present/mounted/info read")

sdmmc = (root / "sticky_sdcard_sdmmc.cpp").read_text()
pr = body(sdmmc, 'extern "C" bool sticky_sdcard_present(void)')
if pr is None: fail("SDMMC sticky_sdcard_present not found")
else:
    expr = re.search(r"return\s+([^;]+);", strip_comments(pr)).group(1)
    if re.sub(r"\s+", "", expr) != "s_ready&&s_card!=nullptr":
        fail(f"SDMMC present() returns '{expr}', want s_ready && s_card != nullptr")
    else: ok("SDMMC present() == mounted")
rp = body(sdmmc, 'extern "C" void sticky_sdcard_report(')
if rp is None: fail("SDMMC sticky_sdcard_report not found")
else:
    rp = strip_comments(rp)
    if "reconcile_locked()" not in rp: fail("SDMMC report() does not reconcile")
    else: ok("SDMMC report() reconciles before reading")
    if not re.search(r"out->present\s*=\s*out->mounted\s*;", rp):
        fail("SDMMC report() present is not mounted")
    else: ok("SDMMC report() present == mounted")
    take, give = rp.find("xSemaphoreTake(s_lock"), rp.find("xSemaphoreGive(s_lock")
    reads = [rp.find(x) for x in ("reconcile_locked()", "out->mounted =", "esp_vfs_fat_info(")]
    if take < 0 or give < 0 or not all(take < r < give for r in reads):
        fail("SDMMC report() reads outside s_lock")
    else: ok("SDMMC report() reconciles and reads under s_lock")

if "tried_narrower = true" not in sdmmc:
    fail("SDMMC driver does not record a 1-bit fallback attempt")
elif ("mounted %d-bit instead" not in sdmmc or
      "fallback also " not in sdmmc or
      sdmmc.count("if (tried_narrower)") < 2):
    fail("SDMMC driver does not log both successful and failed 1-bit fallback")
else:
    ok("SDMMC driver logs every 1-bit fallback outcome")

for f in ("sticky_sdcard.cpp", "sticky_sdcard_sdmmc.cpp", "hal_absent.cpp"):
    if 'sticky_sdcard_report(sticky_sdcard_report_t *out)' not in (root / f).read_text():
        fail(f"{f} does not define sticky_sdcard_report")
    else: ok(f"{f} defines sticky_sdcard_report")

if fails: sys.exit(f"sd report host test: {len(fails)} FAILED")
print("sd report host test passed")
PY
