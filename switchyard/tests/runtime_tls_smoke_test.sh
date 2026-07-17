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

work="$(/usr/bin/mktemp -d /tmp/switchyard-runtime-tls.XXXXXX)"
prefix="$work/prefix"
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/switchyard-wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/switchyard-wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -o "$work/winhttp-smoke.exe" \
  "$ROOT_DIR/switchyard/tests/winhttp_smoke.c" -lwinhttp
output="$(WINEPREFIX="$prefix" WINEDEBUG=-all \
  "$RUNTIME/bin/switchyard-wine" "$work/winhttp-smoke.exe")"
/usr/bin/printf '%s\n' "$output"
/usr/bin/printf '%s\n' "$output" | /usr/bin/grep -F 'HTTP 200' >/dev/null
