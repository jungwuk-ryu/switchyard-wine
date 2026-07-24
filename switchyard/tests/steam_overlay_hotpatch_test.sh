#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || {
  echo "runtime launcher is missing" >&2
  exit 1
}
command -v i686-w64-mingw32-gcc >/dev/null || {
  echo "i686-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-steam-hotpatch.XXXXXX)"
prefix="$work/prefix"
# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

i686-w64-mingw32-gcc -Wall -Wextra -o "$work/steam-overlay-hotpatch.exe" \
  "$ROOT_DIR/switchyard/tests/steam_overlay_hotpatch.c"
WINEPREFIX="$prefix" WINEDEBUG=-all \
  "$RUNTIME/bin/switchyard-wine" "$work/steam-overlay-hotpatch.exe" &
wine_pid=$!
(
  sleep 60
  if kill -0 "$wine_pid" 2>/dev/null; then
    echo "Steam overlay hotpatch test timed out" >&2
    kill -TERM "$wine_pid" 2>/dev/null || true
  fi
) &
watchdog_pid=$!
status=0
wait "$wine_pid" || status=$?
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
exit "$status"
