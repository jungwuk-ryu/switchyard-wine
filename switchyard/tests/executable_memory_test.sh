#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || {
  echo "runtime launcher is missing" >&2
  exit 1
}
command -v otool >/dev/null || {
  echo "otool is required" >&2
  exit 1
}
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

wine_loader="$RUNTIME/lib/wine/x86_64-unix/wine"
[ -f "$wine_loader" ] || {
  echo "Wine Unix loader is missing" >&2
  exit 1
}

segment_max_protection() {
  otool -l "$wine_loader" |
    awk -v segment="$1" '
      $1 == "segname" && $2 == segment { found = 1; next }
      found && $1 == "maxprot" { print $2; exit }
    '
}

for segment in WINE_RESERVE WINE_TOP_DOWN; do
  max_protection="$(segment_max_protection "$segment")"
  [ -n "$max_protection" ] || {
    echo "$segment is missing from the Wine Unix loader" >&2
    exit 1
  }
  if (( (max_protection & 4) == 0 )); then
    echo "$segment does not permit executable mappings" >&2
    exit 1
  fi
done

work="$(/usr/bin/mktemp -d /tmp/switchyard-executable-memory.XXXXXX)"
prefix="$work/prefix"
# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -Os -s \
  -o "$work/executable-memory.exe" \
  "$ROOT_DIR/switchyard/tests/executable_memory.c"

log="$work/executable-memory.log"
env WINEPREFIX="$prefix" WINEDLLOVERRIDES="winedbg.exe=d" WINEDEBUG=-all \
  "$RUNTIME/bin/switchyard-wine" "$work/executable-memory.exe" \
  >"$log" 2>&1 &
wine_pid=$!
(
  sleep 30
  if kill -0 "$wine_pid" 2>/dev/null; then
    echo "Executable memory test timed out" >>"$log"
    kill -TERM "$wine_pid" 2>/dev/null || true
  fi
) &
watchdog_pid=$!
status=0
wait "$wine_pid" || status=$?
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true

/bin/cat "$log"
if [ "$status" -ne 0 ]; then
  exit "$status"
fi
/usr/bin/grep -F "Executable memory test passed" "$log" >/dev/null
