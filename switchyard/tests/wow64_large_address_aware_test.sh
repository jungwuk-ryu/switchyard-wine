#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || { echo "runtime launcher is missing" >&2; exit 1; }
[ -x "$RUNTIME/bin/wineserver" ] || { echo "runtime wineserver is missing" >&2; exit 1; }
command -v i686-w64-mingw32-gcc >/dev/null || {
  echo "i686-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-wow64-laa.XXXXXX)"
prefix="$work/prefix"
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

i686-w64-mingw32-gcc -Wall -Wextra -Werror \
  -Wl,--disable-large-address-aware \
  -o "$work/wow64-large-address-aware.exe" \
  "$ROOT_DIR/switchyard/tests/wow64_large_address_aware.c"

(
  unset WINE_LARGE_ADDRESS_AWARE
  WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
    "$RUNTIME/bin/switchyard-wine" "$work/wow64-large-address-aware.exe" enabled
)
WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
  WINE_LARGE_ADDRESS_AWARE=0 \
  "$RUNTIME/bin/switchyard-wine" "$work/wow64-large-address-aware.exe" disabled

echo "WoW64 large-address-aware override test passed"
