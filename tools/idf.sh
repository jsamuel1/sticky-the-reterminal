#!/usr/bin/env bash
# idf.sh — run idf.py inside the pinned ESP-IDF image (the project default).
#
#   tools/idf.sh build                          # = cd firmware && idf.py build
#   tools/idf.sh -B build-fnk0104s ... build    # any idf.py arguments
#   TINY_IDF=host tools/idf.sh build            # opt out: use the exported host IDF
#
# Every build runs in espressif/idf:v5.4, the same image CI uses, so a laptop,
# an agent host and CI produce the same bytes. The repository is mounted at
# its OWN host path (not /project), so CMake caches and flasher_args.json
# carry paths that are also valid on the host. In a git worktree the shared
# .git directory is mounted too, so gen_commit.cmake stamps the real sha
# instead of "nogit".
#
# Only builds go through the container. Flashing and monitoring need the USB
# port, which a container on macOS cannot reach: tools/flash.sh, monitor.sh
# and first_flash.sh keep using the host's esptool.
#
# Env: TINY_IDF=host|docker (default docker), TINY_IDF_IMAGE (default
# espressif/idf:v5.4). Inside the image itself (CI, or this script's own
# container) idf.py runs directly.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${TINY_IDF_IMAGE:-espressif/idf:v5.4}"
MODE="${TINY_IDF:-docker}"

# Already inside an ESP-IDF container (CI job, nested call): no docker-in-docker.
if [ "$MODE" = host ] || [ "${IDF_PATH:-}" = /opt/esp/idf ]; then
  if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1090
    [ -n "${IDF_PATH:-}" ] && . "$IDF_PATH/export.sh" >/dev/null 2>&1 \
      || . ~/esp/esp-idf-v5.4/export.sh >/dev/null 2>&1 \
      || { echo "idf.sh: TINY_IDF=host but no ESP-IDF found (IDF_PATH or ~/esp/esp-idf-v5.4)" >&2; exit 1; }
  fi
  cd "$REPO/firmware"
  exec idf.py "$@"
fi

[ "$MODE" = docker ] || { echo "idf.sh: TINY_IDF must be docker or host, got '$MODE'" >&2; exit 2; }
command -v docker >/dev/null 2>&1 \
  || { echo "idf.sh: docker not found. Install it, or use TINY_IDF=host with ESP-IDF v5.4 exported." >&2; exit 1; }

mounts=(-v "$REPO:$REPO")
# Worktree: .git is a file pointing at the main repository's git dir.
common="$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
case "$common" in
  ""|"$REPO"/*) ;;
  *) mounts+=(-v "$common:$common:ro") ;;
esac

tty=()
[ -t 0 ] && [ -t 1 ] && tty=(-it)

exec docker run --rm "${tty[@]}" \
  -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
  "${mounts[@]}" -w "$REPO/firmware" \
  "$IMAGE" idf.py "$@"
