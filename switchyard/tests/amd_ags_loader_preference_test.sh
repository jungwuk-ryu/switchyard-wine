#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"

[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || { echo "runtime launcher missing" >&2; exit 1; }
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "x86_64-w64-mingw32-gcc missing" >&2; exit 1; }

WINEBOOT="$RUNTIME/bin/wineboot"
if [ ! -x "$WINEBOOT" ]; then
  WINEBOOT="$RUNTIME/bin/wineboot.exe"
fi
[ -x "$WINEBOOT" ] || { echo "wineboot binary missing" >&2; exit 1; }

work="$(/usr/bin/mktemp -d /tmp/switchyard-ags-loadorder.XXXXXX)"
prefix="$work/prefix"

cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

run_case() {
  local label=$1 expected=$2
  shift 2

  echo "[test] $label"
  if ! env "$@" WINEPREFIX="$prefix" WINEDEBUG=-all SWITCHYARD_AGS_EXPECTED="$expected" \
    "$RUNTIME/bin/switchyard-wine" "$work/amd_ags_loader_preference_probe.exe"; then
    echo "case '$label' failed" >&2
    return 1
  fi
}

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -o "$work/amd_ags_loader_preference_probe.exe" \
  "$ROOT_DIR/switchyard/tests/amd_ags_loader_preference_probe.c"
x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -shared -o "$work/amd_ags_loader_preference_native.dll" \
  "$ROOT_DIR/switchyard/tests/amd_ags_loader_preference_native.c"

WINEPREFIX="$prefix" "$WINEBOOT" -u >/dev/null

system_file="$prefix/drive_c/windows/system32/amd_ags_x64.dll"
if [ -e "$system_file" ]; then
  echo "unexpected amd_ags_x64.dll placeholder present after prefix init" >&2
  exit 1
fi

run_case "default_no_native_is_missing" missing

native_file="$work/amd_ags_x64.dll"
cp "$work/amd_ags_loader_preference_native.dll" "$native_file"

run_case "native_present_without_override_still_builtin" builtin
run_case "explicit_native_override" native WINEDLLOVERRIDES='amd_ags_x64=n'
run_case "explicit_builtin_override" builtin WINEDLLOVERRIDES='amd_ags_x64=b'

echo "[test] amd_ags loader preference test passed"
