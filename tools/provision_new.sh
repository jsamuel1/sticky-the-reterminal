#!/bin/bash
# provision_new.sh — enrol a NEW Sticky in the tiny fleet and flash it, in ONE run.
#
#   WIFI_SSID=MyAP WIFI_KEY=secret tools/provision_new.sh [options] <port>
#
#   --board reterminal|fnk0104s|amoled18   board variant (env STICKY_BOARD; default reterminal)
#   --repo <git-url|path>                  source to build (env STICKY_REPO; default this checkout)
#   --branch <branch|tag|sha>              ref to build (env STICKY_BRANCH; default: current HEAD,
#                                          or the remote's default branch with --repo)
#   --dry-run                              print the plan; read the MAC, write nothing, enrol nothing
#   --mac <base-mac>                       skip the port read (dry-run without a board)
#   --adopt                                re-key a same-name row that is ONLINE (else refused)
#   --allow-dirty                          flash uncommitted firmware/ or vendor/ source, or a
#                                          build whose fw_commit is not the source HEAD
#   --no-build                             flash the existing build dir as is
#   --no-wifi                              identity only; the board asks for Wi-Fi itself
#   --keep-src                             keep the temp clone of --repo/--branch
#
# `npx tiny-tech flash --body sticky-the-reterminal --from <checkout>` runs this
# with the port alone; everything else comes from env (WIFI_SSID/WIFI_KEY,
# STICKY_BOARD, STICKY_REPO, STICKY_BRANCH, TINY_API, TINY_TOKEN).
#
# What it does:
#   1. Reads the base MAC and flash size over the ROM loader (esptool flash_id).
#      The name is tiny-XXXX = FNV-1a over the softAP MAC (base + 1 on the
#      ESP32-S3), the same bytes tiny_provision.cpp make_ssid() hashes, so the
#      cloud row matches the AP the board broadcasts. A flash size that does not
#      match --board refuses.
#   2. Builds <repo>@<branch> for the board with tools/build_board.sh (Docker
#      espressif/idf:v5.4 via tools/idf.sh; TINY_IDF=host opts out). The ref is
#      cloned into a temp dir unless it is this checkout's HEAD. Uncommitted
#      firmware/ or vendor/ source refuses before the build, and the built
#      fw_commit must be exactly the source HEAD (not -dirty, not nogit, not a
#      stale --no-build dir) unless --allow-dirty. The board's flash size and
#      the enrolled capability roster are read from that source's firmware/.
#   3. Plans the write from the build's own flasher_args.json (parts, flash
#      mode/size/freq) plus NVS at the offset partitions*.csv gives it.
#   4. Enrols. With a session (~/.tiny/credentials.json or TINY_TOKEN): a
#      same-name ONLINE row refuses unless --adopt; an offline one is re-keyed
#      by POST /api/devices; otherwise a new row. Without a session: a pairing
#      code (POST /api/devices/pair/start, owner approves at
#      <api>/devices/pair?code=…, poll /pair/poll). The token is returned once.
#   5. Writes {device_id, token, api, name, networks} into an NVS image
#      (namespace "tiny", key "cfg" — the blob tiny_config.cpp reads) and
#      flashes everything in one write_flash (460800, then 115200: hub chains
#      corrupt >= 921600). Then 40 s of boot log to .logs/first-boot-<name>.log.
#
# Tools: esptool from IDF_PATH's python, an ESP-IDF python env, python3, or a
# pinned venv it creates in ~/.cache/sticky-provision (pip, needs network).
# NVS generation runs in espressif/idf:v5.4 (TINY_IDF=host: the host module).
# The USB port always stays on the host. Works on macOS and Linux.
#
# The token lands only in the NVS image (0700 temp dir, deleted) and the
# receipt .secrets.<name>.json (0600, gitignored). It is never printed.
# Pass the port explicitly: other CH343 adapters on a hub can be servo buses.
set -euo pipefail

usage() { sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; }
die() { echo "REFUSED: $*" >&2; exit 2; }
log() { echo "[provision] $*"; }

SELF="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$SELF/tools/provision_lib.py"
BOARD="${STICKY_BOARD:-reterminal}"
REPO="${STICKY_REPO:-}"
BRANCH="${STICKY_BRANCH:-}"
BOOT_S="${STICKY_BOOTLOG_S:-40}"
IMAGE="${TINY_IDF_IMAGE:-espressif/idf:v5.4}"
VENV="${XDG_CACHE_HOME:-$HOME/.cache}/sticky-provision/venv"
ESPTOOL_PIN="esptool==4.8.1"                     # = espressif/idf:v5.4
NVSGEN_PIN="esp-idf-nvs-partition-gen==0.1.6"    # = espressif/idf:v5.4
ADOPT_FLAG="" DIRTY_FLAG=""
DRY=0 ALLOW_DIRTY=0 NO_BUILD=0 NO_WIFI=0 KEEP=0
MAC="" PORT=""

need_arg() { if [ $# -lt 2 ] || [ -z "$2" ]; then die "$1 needs a value"; fi; }
while [ $# -gt 0 ]; do
  case "$1" in
    --board)       need_arg "$@"; BOARD="$2"; shift 2 ;;
    --board=*)     BOARD="${1#*=}"; shift ;;
    --repo)        need_arg "$@"; REPO="$2"; shift 2 ;;
    --repo=*)      REPO="${1#*=}"; shift ;;
    --branch)      need_arg "$@"; BRANCH="$2"; shift 2 ;;
    --branch=*)    BRANCH="${1#*=}"; shift ;;
    --mac)         need_arg "$@"; MAC="$2"; shift 2 ;;
    --mac=*)       MAC="${1#*=}"; shift ;;
    --dry-run)     DRY=1; shift ;;
    --adopt)       ADOPT_FLAG=--adopt; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; DIRTY_FLAG=--allow-dirty; shift ;;
    --no-build)    NO_BUILD=1; shift ;;
    --no-wifi)     NO_WIFI=1; shift ;;
    --keep-src)    KEEP=1; shift ;;
    -h|--help)     usage; exit 0 ;;
    --)            shift; [ $# -eq 0 ] || { PORT="$1"; shift; } ;;
    -*)            die "unknown option $1 (see --help)" ;;
    *)             [ -z "$PORT" ] || die "one port only (got $PORT and $1)"; PORT="$1"; shift ;;
  esac
done
case "$BOARD" in reterminal|fnk0104s|amoled18) ;; *) die "unknown board '$BOARD' (reterminal | fnk0104s | amoled18)" ;; esac
if [ -z "$PORT" ] && ! { [ "$DRY" = 1 ] && [ -n "$MAC" ]; }; then
  echo "usage: provision_new.sh [--board B] [--repo R] [--branch REF] [--dry-run] <port>   (--help)" >&2; exit 2
fi
[ -z "$MAC" ] || [ "$DRY" = 1 ] || die "--mac is for --dry-run only; a real run reads the board it flashes"
PY3="$(command -v python3 || true)"; [ -n "$PY3" ] || die "python3 not found"
lib() { "$PY3" "$LIB" "$@"; }

WORK="" SRC_TMP=""
cleanup() {
  [ -z "$WORK" ] || rm -rf "$WORK"
  if [ -n "$SRC_TMP" ]; then
    if [ "$KEEP" = 1 ]; then log "kept source clone $SRC_TMP"; else rm -rf "$SRC_TMP"; fi
  fi
}
trap cleanup EXIT
WORK="$(mktemp -d "${TMPDIR:-/tmp}/sticky-prov.XXXXXX")"; chmod 700 "$WORK"

# ---- tools: a python that imports esptool (+ pyserial, its dependency) ------
ESP_PY=""
find_esp_py() {
  local c
  for c in "${STICKY_ESPTOOL_PYTHON:-}" "${IDF_PYTHON_ENV_PATH:+$IDF_PYTHON_ENV_PATH/bin/python}" \
           "$HOME"/.espressif/python_env/idf5.4_py*_env/bin/python "$VENV/bin/python" python3 python; do
    [ -n "$c" ] || continue
    command -v "$c" >/dev/null 2>&1 || [ -x "$c" ] || continue
    if "$c" -c 'import esptool, serial' >/dev/null 2>&1; then ESP_PY="$c"; return 0; fi
  done
  return 1
}
if ! find_esp_py; then
  if [ -n "$MAC" ]; then
    ESP_PY=none   # dry run on a given MAC: no board, no esptool needed
  else
    log "no esptool found — creating $VENV ($ESPTOOL_PIN, $NVSGEN_PIN)"
    "$PY3" -m venv "$VENV" || die "python3 -m venv failed (install python3-venv)"
    "$VENV/bin/python" -m pip install -q --disable-pip-version-check "$ESPTOOL_PIN" "$NVSGEN_PIN" \
      || die "pip install $ESPTOOL_PIN failed (network?)"
    ESP_PY="$VENV/bin/python"
  fi
fi
esptool() { "$ESP_PY" -m esptool --chip esp32s3 -p "$PORT" "$@"; }

if [ -n "$PORT" ] && [ -z "$MAC" ]; then
  [ -e "$PORT" ] || die "no such port $PORT"
  if [ ! -r "$PORT" ] || [ ! -w "$PORT" ]; then
    hint="check permissions"; [ "$(uname -s)" = Linux ] && hint="add yourself to group $(stat -c %G "$PORT" 2>/dev/null || echo dialout), then log in again"
    die "cannot open $PORT read/write — $hint"
  fi
fi

# ---- Wi-Fi: env, else an earlier receipt's "wifi" block ---------------------
WIFI_SRC="$SELF/.secrets.device.json"
WIFI_FROM="env"
if [ "$NO_WIFI" = 1 ]; then
  unset WIFI_SSID WIFI_KEY; WIFI_FROM="none (--no-wifi: the board asks for it)"
elif [ -z "${WIFI_SSID:-}" ]; then
  if [ -f "$WIFI_SRC" ] && WIFI_SSID="$(lib json-get "$WIFI_SRC" wifi.ssid)"; then
    WIFI_KEY="$(lib json-get "$WIFI_SRC" wifi.key || true)"; WIFI_FROM="$WIFI_SRC"
  elif [ "$DRY" = 1 ]; then
    WIFI_FROM="MISSING — a real run refuses (set WIFI_SSID/WIFI_KEY or --no-wifi)"
  else
    die "set WIFI_SSID/WIFI_KEY, or pass --no-wifi (no $WIFI_SRC to copy from)"
  fi
fi
export WIFI_SSID WIFI_KEY

# ---- 1. identity from the silicon ------------------------------------------
SIZE=unknown
if [ -n "$MAC" ]; then
  BASE="$MAC"
else
  out="$(esptool -b 115200 --before default_reset --after hard_reset flash_id 2>&1)" \
    || { printf '%s\n' "$out" | tail -5 >&2; die "esptool could not talk to $PORT (is it an ESP32-S3? BOOT+RESET to force the loader)"; }
  ids="$(printf '%s\n' "$out" | lib flash-id)" || die "no MAC in esptool output on $PORT"
  read -r BASE SIZE <<<"$ids"
fi
ids="$(lib name "$BASE")" || die "bad MAC '$BASE'"
read -r AP NAME <<<"$ids"
log "${PORT:-<no port>} base=$BASE softap=$AP flash=$SIZE → name $NAME (board $BOARD)"

# ---- 2. source: this checkout's HEAD in place, else a temp clone ------------
toplevel() { git -C "$1" rev-parse --show-toplevel 2>/dev/null; }
IN_PLACE=0
SELF_TOP="$(toplevel "$SELF" || echo "$SELF")"
REPO_TOP=""
if [ -z "$REPO" ]; then REPO_TOP="$SELF_TOP"
elif [ -d "$REPO" ]; then REPO_TOP="$(toplevel "$REPO")" || die "--repo $REPO is not a git checkout"
fi
if [ "$REPO_TOP" = "$SELF_TOP" ]; then
  if [ -z "$BRANCH" ]; then IN_PLACE=1
  else
    want="$(git -C "$SELF" rev-parse -q --verify "$BRANCH^{commit}" 2>/dev/null \
            || git -C "$SELF" rev-parse -q --verify "origin/$BRANCH^{commit}" 2>/dev/null || true)"
    [ -n "$want" ] && [ "$want" = "$(git -C "$SELF" rev-parse HEAD)" ] && IN_PLACE=1
  fi
fi
if [ "$IN_PLACE" = 1 ]; then
  SRC="$SELF"; ORIGIN="this checkout"
else
  ORIGIN="${REPO:-$SELF_TOP}"
  SRC_TMP="$(mktemp -d "${TMPDIR:-/tmp}/sticky-src.XXXXXX")"; SRC="$SRC_TMP"
  log "cloning $ORIGIN${BRANCH:+ @ $BRANCH}"
  if [ -n "$REPO_TOP" ]; then
    # Local checkout: resolve the ref THERE (branches, origin/*, tags, any sha),
    # borrow its objects, then make them local — the build container mounts
    # only the clone, and gen_commit.cmake's git describe must see the commit.
    ref="$(git -C "$REPO_TOP" rev-parse -q --verify "${BRANCH:-HEAD}^{commit}" 2>/dev/null \
           || git -C "$REPO_TOP" rev-parse -q --verify "origin/$BRANCH^{commit}" 2>/dev/null || true)"
    [ -n "$ref" ] || die "no ref '$BRANCH' in $REPO_TOP"
    git clone -q --shared --no-checkout "$REPO_TOP" "$SRC" || die "git clone $REPO_TOP failed"
    git -C "$SRC" repack -a -d -q && rm -f "$SRC/.git/objects/info/alternates"
  else
    # Remote URL: shallow for a branch/tag; a sha needs the full history.
    if [ -n "$BRANCH" ] && git clone -q --depth 1 --no-checkout --branch "$BRANCH" "$REPO" "$SRC" 2>/dev/null; then :
    else
      rm -rf "$SRC"; mkdir -p "$SRC"
      git clone -q --no-checkout "$REPO" "$SRC" || die "git clone $REPO failed"
    fi
    ref="$(git -C "$SRC" rev-parse -q --verify "${BRANCH:-HEAD}^{commit}" 2>/dev/null \
           || git -C "$SRC" rev-parse -q --verify "origin/$BRANCH^{commit}" 2>/dev/null || true)"
    if [ -z "$ref" ]; then   # a sha or ref the clone did not advertise (PR heads)
      if git -C "$SRC" fetch -q origin "$BRANCH" 2>/dev/null; then ref="$(git -C "$SRC" rev-parse FETCH_HEAD)"
      else die "no ref '$BRANCH' in $REPO"; fi
    fi
  fi
  git -C "$SRC" checkout -q --detach "$ref"
fi
SHA="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo nogit)"
[ -f "$SRC/tools/idf.sh" ] || die "$ORIGIN${BRANCH:+@$BRANCH} predates tools/idf.sh — this script cannot build it"
if [ "$BOARD" = reterminal ]; then BUILD="$SRC/firmware/build"; else BUILD="$SRC/firmware/build-$BOARD"; fi
log "source $ORIGIN${BRANCH:+ @ $BRANCH} = $SHA"
# Board facts (flash size, verb roster) come from the source being flashed.
[ -n "$MAC" ] || lib check-size "$BOARD" "$SIZE" --fw "$SRC/firmware"
# What the image is built from: firmware/ + the vendored components (the scope
# gen_commit.cmake stamps). A dirty tree refuses before minutes of building.
DIRTY_SRC=""
if [ "$SHA" != nogit ]; then
  DIRTY_SRC="$(git -C "$SRC" status --porcelain -- firmware vendor/Sticky_dashboard_demo/components 2>/dev/null || true)"
fi
if [ -n "$DIRTY_SRC" ]; then
  printf '%s\n' "$DIRTY_SRC" | head -8 | sed 's/^/[provision]   /'
  if [ "$ALLOW_DIRTY" = 1 ]; then log "warning: uncommitted firmware/vendor source (--allow-dirty)"
  elif [ "$DRY" = 1 ]; then log "warning: uncommitted firmware/vendor source — a real run refuses without --allow-dirty"
  else die "uncommitted firmware/ or vendor/ source in $SRC — commit, or pass --allow-dirty"; fi
fi

# ---- 3. build, then plan from the build's own flasher_args.json -------------
if [ "$DRY" = 1 ]; then
  [ "$NO_BUILD" = 1 ] && note="existing build, not rebuilt" || note="would build with tools/build_board.sh $BOARD"
  lib plan "$BUILD" "$BOARD" --out "$WORK/plan.json" --allow-unbuilt
  echo "build     $BUILD ($note)"
  echo "wifi      ${WIFI_SSID:+ssid \"$WIFI_SSID\" from }$WIFI_FROM"
  echo "receipt   $SELF/.secrets.$NAME.json (0600)"
  lib enroll --name "$NAME" --receipt /dev/null --dry-run --fw "$SRC/firmware" ${ADOPT_FLAG:+"$ADOPT_FLAG"}
  echo "dry run — nothing built, enrolled or written${PORT:+ (the MAC read reset the board)}"
  exit 0
fi
mkdir -p "$SELF/.logs"
if [ "$NO_BUILD" = 0 ]; then
  BLOG="$SELF/.logs/provision-build-$BOARD.log"
  log "building $BOARD (log $BLOG)"
  if [ -x "$SRC/tools/build_board.sh" ]; then set -- "$SRC/tools/build_board.sh" "$BOARD"
  elif [ "$BOARD" = reterminal ]; then set -- "$SRC/tools/idf.sh" build
  else die "$ORIGIN${BRANCH:+@$BRANCH} has no tools/build_board.sh — it cannot build $BOARD"; fi
  "$@" >"$BLOG" 2>&1 </dev/null || { tail -15 "$BLOG" >&2; die "build failed (full log $BLOG)"; }
fi
lib plan "$BUILD" "$BOARD" --out "$WORK/plan.json"
COMMIT="$(lib json-get "$WORK/plan.json" commit || echo nogit)"
# The stamp must name this exact tree: refuse -dirty, nogit, or a sha that is not
# the source HEAD (a stale --no-build dir) unless --allow-dirty.
lib provenance "$COMMIT" "$SHA" --source "$SRC" ${DIRTY_FLAG:+"$DIRTY_FLAG"} \
  || die "refusing to flash an image that cannot name its exact tree"

# ---- 4. enrol (session or pairing code; token shown once) --------------------
RECEIPT="$SELF/.secrets.$NAME.json"
umask 077
DEVICE_ID="$(lib enroll --name "$NAME" --receipt "$RECEIPT" --fw "$SRC/firmware" ${ADOPT_FLAG:+"$ADOPT_FLAG"})" \
  || die "enrolment failed — nothing was flashed"
log "enrolled $NAME device_id=$DEVICE_ID (receipt $RECEIPT)"

# ---- 5. NVS image + one write -------------------------------------------------
NVS_SIZE="$(lib json-get "$WORK/plan.json" nvs_size)"
lib nvs-csv "$RECEIPT" "$WORK/nvs.csv"
if [ "${TINY_IDF:-docker}" != host ] && command -v docker >/dev/null 2>&1; then
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$WORK:$WORK" -w "$WORK" "$IMAGE" \
    python -m esp_idf_nvs_partition_gen generate nvs.csv nvs.bin "$NVS_SIZE" >/dev/null 2>"$WORK/nvs.err" \
    || { tail -5 "$WORK/nvs.err" >&2; die "NVS generation in $IMAGE failed"; }
else
  "$ESP_PY" -c 'import esp_idf_nvs_partition_gen' 2>/dev/null \
    || { [ "$ESP_PY" = "$VENV/bin/python" ] && "$ESP_PY" -m pip install -q "$NVSGEN_PIN"; } \
    || die "no docker and no esp_idf_nvs_partition_gen for $ESP_PY (pip install $NVSGEN_PIN)"
  "$ESP_PY" -m esp_idf_nvs_partition_gen generate "$WORK/nvs.csv" "$WORK/nvs.bin" "$NVS_SIZE" >/dev/null
fi
rm -f "$WORK/nvs.csv"
[ -s "$WORK/nvs.bin" ] || die "NVS image missing"
log "nvs image $(wc -c <"$WORK/nvs.bin" | tr -d ' ') bytes"

FLASH_ARGS=()
while IFS= read -r -d '' a; do FLASH_ARGS+=("$a"); done < <(lib argv "$WORK/plan.json" "$WORK/nvs.bin")
log "flashing $BOARD fw $COMMIT ($SHA) as $NAME"
flash() { esptool -b "$1" --before default_reset --after hard_reset write_flash "${FLASH_ARGS[@]}"; }
flash 460800 || flash 115200 \
  || die "write_flash failed. The token for $NAME is NOT on the board; run again (the offline row is re-keyed)"

# ---- 6. boot evidence ---------------------------------------------------------
BOOTLOG="$SELF/.logs/first-boot-$NAME.log"
log "boot log ${BOOT_S}s → $BOOTLOG"
"$ESP_PY" "$LIB" bootlog "$PORT" "$BOOT_S" "$BOOTLOG" || log "warning: boot log capture failed"
API="$(lib json-get "$RECEIPT" api)"
log "done: $NAME $DEVICE_ID fw $COMMIT ($SHA) — verify with GET $API/api/devices (online within ~60s)"
