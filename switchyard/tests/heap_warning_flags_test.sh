#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || { echo "runtime launcher is missing" >&2; exit 1; }
[ -x "$RUNTIME/bin/wineserver" ] || { echo "runtime wineserver is missing" >&2; exit 1; }
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-heap-warning-flags.XXXXXX)"
prefix="$work/prefix"
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -Os -s \
  -o "$work/heap-warning-flags.exe" \
  "$ROOT_DIR/switchyard/tests/heap_warning_flags.c"

WINEPREFIX="$prefix" WINEDEBUG=-all,warn+heap WINEDLLOVERRIDES="winedbg.exe=d" \
  "$RUNTIME/bin/switchyard-wine" "$work/heap-warning-flags.exe" normal
WINEPREFIX="$prefix" WINEDEBUG=-all,trace+heap WINEDLLOVERRIDES="winedbg.exe=d" \
  "$RUNTIME/bin/switchyard-wine" "$work/heap-warning-flags.exe" validated \
  >"$work/trace.log" 2>&1

echo "Heap warning flags test passed"
