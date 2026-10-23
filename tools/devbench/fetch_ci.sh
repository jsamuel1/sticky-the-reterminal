#!/usr/bin/env bash
# fetch_ci.sh — pull CI-built devbench image sets from GitHub Actions.
#
#   tools/devbench/fetch_ci.sh                 # latest green firmware-build on main, all boards
#   tools/devbench/fetch_ci.sh fnk0104s        # one board
#   RUN=1234567890 tools/devbench/fetch_ci.sh  # a specific run (e.g. a PR's)
#
# Each artifact devbench-<board> lands in tools/devbench/images/ci-<board>/,
# replacing the previous one, so the bench lists "ci-<board>" with the CI
# commit in its version line. Needs `gh` authenticated for the repository.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGES="${DEVBENCH_IMAGES:-$HERE/images}"
REPO="${GH_REPO:-$(git -C "$HERE" remote get-url origin | sed -E 's#(git@|https://)github.com[:/]##; s#\.git$##')}"
boards=("$@")
[ ${#boards[@]} -gt 0 ] || boards=(reterminal fnk0104s amoled18)

run="${RUN:-}"
if [ -z "$run" ]; then
  run="$(gh run list -R "$REPO" -w firmware-build -b main -s success -L 1 --json databaseId -q '.[0].databaseId')"
  [ -n "$run" ] || { echo "fetch_ci: no successful firmware-build run on main in $REPO" >&2; exit 1; }
fi
sha="$(gh run view "$run" -R "$REPO" --json headSha -q .headSha)"
echo "run $run @ ${sha:0:8} ($REPO)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for b in "${boards[@]}"; do
  gh run download "$run" -R "$REPO" -n "devbench-$b" -D "$tmp/$b"
  [ -f "$tmp/$b/manifest.json" ] || { echo "fetch_ci: devbench-$b has no manifest.json" >&2; exit 1; }
  dst="$IMAGES/ci-$b"
  rm -rf "$dst.new"
  mv "$tmp/$b" "$dst.new"
  rm -rf "$dst"
  mv "$dst.new" "$dst"
  echo "  ci-$b: $(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["version"])' "$dst/manifest.json")"
done
