#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
GPTK_PATH="${2:-}"

[ -n "$RUNTIME" ] && [ -n "$GPTK_PATH" ] || {
  echo "usage: $0 RUNTIME GPTK_PATH" >&2
  exit 2
}
[ -x "$RUNTIME/bin/switchyard-wine" ] || {
  echo "runtime launcher is missing" >&2
  exit 1
}
[ -f "$GPTK_PATH/redist/lib/external/D3DMetal.framework/D3DMetal" ] || {
  echo "selected GPTK does not contain D3DMetal" >&2
  exit 1
}
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-amd-umd-policy.XXXXXX)"
prefix="$work/prefix"
gptk_env=(
  "WINEDLLPATH=$GPTK_PATH/redist/lib/wine"
  "DYLD_FRAMEWORK_PATH=$GPTK_PATH/redist/lib/external"
  "DYLD_LIBRARY_PATH=$GPTK_PATH/redist/lib/external"
  "SWITCHYARD_GPTK_PATH=$GPTK_PATH"
)

cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror \
  -o "$work/amd-umd-loader-probe.exe" \
  "$ROOT_DIR/switchyard/tests/amd_umd_loader_probe.c"
x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -shared \
  -o "$work/atidxx64.dll" \
  "$ROOT_DIR/switchyard/tests/amd_umd_loader_native.c"

run_case() {
  local label="$1"
  local expected="$2"
  shift 2

  echo "[test] $label"
  env "${gptk_env[@]}" "$@" WINEPREFIX="$prefix" WINEDEBUG=-all \
    SWITCHYARD_AMD_UMD_EXPECTED="$expected" \
    "$RUNTIME/bin/switchyard-wine" "$work/amd-umd-loader-probe.exe"
}

run_case "default_blocks_app_local_amd_umd" missing
run_case "explicit_native_override_is_preserved" native \
  WINEDLLOVERRIDES=atidxx64=n

echo "[test] AMD UMD provider policy passed"
