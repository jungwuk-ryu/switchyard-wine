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

work="$(/usr/bin/mktemp -d /tmp/switchyard-font-fallback.XXXXXX)"
prefix="$work/prefix"
user_key='HKCU\Software\Wine\Fonts\Replacements'

# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  find "$work" -depth -delete
}
trap cleanup EXIT

run_wine() {
  WINEPREFIX="$prefix" WINEDEBUG=-all "$RUNTIME/bin/switchyard-wine" "$@"
}

run_wineboot() {
  run_wine wineboot.exe -u
}

set_registry_value() {
  local key="$1" name="$2" value="$3"
  run_wine reg.exe add "$key" /v "$name" /t REG_SZ /d "$value" /f >/dev/null
}

expect_registry_value() {
  local key="$1" name="$2" expected="$3"
  if ! run_wine reg.exe query "$key" /v "$name" | /usr/bin/grep -F "$expected" >/dev/null; then
    echo "expected $name in $key to be $expected" >&2
    return 1
  fi
}

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -Os -s \
  -o "$work/font-fallback-probe.exe" \
  "$ROOT_DIR/switchyard/tests/font_fallback_probe.c" \
  -ldwrite -lgdi32 -lole32 -luuid

run_wineboot >/dev/null
run_wineboot >/dev/null
run_wine "$work/font-fallback-probe.exe"

set_registry_value "$user_key" 'Segoe UI Emoji' 'Noto Sans CJK KR'
set_registry_value "$user_key" 'Segoe UI Symbol' 'Noto Sans CJK KR'
run_wineboot >/dev/null
expect_registry_value "$user_key" 'Segoe UI Emoji' 'Noto Sans CJK KR'
expect_registry_value "$user_key" 'Segoe UI Symbol' 'Noto Sans CJK KR'

set_registry_value "$user_key" 'Segoe UI Emoji' 'User Emoji Font'
set_registry_value "$user_key" 'Segoe UI Symbol' 'User Symbol Font'
run_wineboot >/dev/null
expect_registry_value "$user_key" 'Segoe UI Emoji' 'User Emoji Font'
expect_registry_value "$user_key" 'Segoe UI Symbol' 'User Symbol Font'

echo "font fallback test passed"
