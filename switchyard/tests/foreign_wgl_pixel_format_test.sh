#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || { echo "runtime launcher is missing" >&2; exit 1; }
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-foreign-wgl.XXXXXX)"
prefix="$work/prefix"
time_output="$work/time.txt"
# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -municode -Wall -Wextra -Werror \
  -o "$work/foreign-wgl-pixel-format.exe" \
  "$ROOT_DIR/switchyard/tests/foreign_wgl_pixel_format.c" \
  -lopengl32 -lgdi32 -luser32
echo "foreign_wgl_test_runtime=$(basename "$RUNTIME")"
"$ROOT_DIR/switchyard/build_runtime.sh" --source-info | sed 's/^/foreign_wgl_source_/'
WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
  /usr/bin/time -lp "$RUNTIME/bin/switchyard-wine" \
  "$work/foreign-wgl-pixel-format.exe" 2>"$time_output" &
wine_pid=$!
(
  sleep 60
  if kill -0 "$wine_pid" 2>/dev/null; then
    echo "foreign WGL pixel format test timed out" >&2
    kill -TERM "$wine_pid" 2>/dev/null || true
  fi
) &
watchdog_pid=$!
status=0
wait "$wine_pid" || status=$?
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
cat "$time_output"
real_seconds="$(awk '$1 == "real" { value=$2 } $2 == "real" { value=$1 } END { print value }' "$time_output")"
voluntary="$(awk '$2 == "voluntary" && $3 == "context" { value=$1 } END { print value }' "$time_output")"
involuntary="$(awk '$2 == "involuntary" && $3 == "context" { value=$1 } END { print value }' "$time_output")"
if [ -n "$real_seconds" ] && [ -n "$voluntary" ] && [ -n "$involuntary" ]; then
  awk -v real="$real_seconds" -v voluntary="$voluntary" -v involuntary="$involuntary" \
    'BEGIN { printf "foreign_wgl_wakeup_proxy real_s=%.3f voluntary_context_switches=%d involuntary_context_switches=%d context_switches_per_s=%.3f\n", real, voluntary, involuntary, (voluntary + involuntary) / real }'
fi
exit "$status"
