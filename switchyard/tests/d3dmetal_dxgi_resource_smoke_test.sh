#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
GPTK_PATH="${2:-}"
[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME [GPTK_PATH]" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || { echo "runtime launcher is missing" >&2; exit 1; }

gptk_env=()
if [ -n "$GPTK_PATH" ]; then
  [ -f "$GPTK_PATH/redist/lib/external/D3DMetal.framework/D3DMetal" ] || {
    echo "selected GPTK does not contain D3DMetal" >&2
    exit 1
  }
  gptk_env=(
    "WINEDLLPATH=$GPTK_PATH/redist/lib/wine"
    "DYLD_FRAMEWORK_PATH=$GPTK_PATH/redist/lib/external"
    "DYLD_LIBRARY_PATH=$GPTK_PATH/redist/lib/external"
    "SWITCHYARD_GPTK_PATH=$GPTK_PATH"
  )
else
  [ -f "$RUNTIME/lib/external/D3DMetal.framework/D3DMetal" ] || {
    echo "runtime does not contain the D3DMetal overlay" >&2
    exit 1
  }
fi
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-d3dmetal-resource.XXXXXX)"
prefix="$work/prefix"
# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -o "$work/d3dmetal-dxgi-resource-smoke.exe" \
  "$ROOT_DIR/switchyard/tests/d3dmetal_dxgi_resource_smoke.c" -ld3d11 -ldxgi -luuid
env "${gptk_env[@]}" WINEPREFIX="$prefix" WINEDEBUG=-all \
  "$RUNTIME/bin/switchyard-wine" "$work/d3dmetal-dxgi-resource-smoke.exe" &
wine_pid=$!
(
  sleep 120
  if kill -0 "$wine_pid" 2>/dev/null; then
    echo "D3DMetal DXGI resource smoke test timed out" >&2
    kill -TERM "$wine_pid" 2>/dev/null || true
  fi
) &
watchdog_pid=$!
status=0
wait "$wine_pid" || status=$?
kill "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
exit "$status"
