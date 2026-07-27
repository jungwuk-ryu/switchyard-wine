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
[ -x "$RUNTIME/libexec/switchyard-host-gpu-info" ] || {
  echo "runtime GPU identity helper is missing" >&2
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

work="$(/usr/bin/mktemp -d /tmp/switchyard-graphics-identity.XXXXXX)"
gptk_env=(
  "WINEDLLPATH=$GPTK_PATH/redist/lib/wine"
  "DYLD_FRAMEWORK_PATH=$GPTK_PATH/redist/lib/external"
  "DYLD_LIBRARY_PATH=$GPTK_PATH/redist/lib/external"
  "SWITCHYARD_GPTK_PATH=$GPTK_PATH"
)

cleanup() {
  for prefix in "$work/provider-default-prefix" "$work/explicit-prefix"; do
    WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
    WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  done
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror \
  -o "$work/d3dmetal-adapter-identity.exe" \
  "$ROOT_DIR/switchyard/tests/graphics_adapter_identity.c" -ldxgi -luuid

run_probe() {
  local prefix="$1"
  shift
  local status=0

  env "${gptk_env[@]}" "$@" WINEPREFIX="$prefix" WINEDEBUG=-all \
    "$RUNTIME/bin/switchyard-wine" "$work/d3dmetal-adapter-identity.exe" &
  wine_pid=$!
  (
    sleep 120
    if kill -0 "$wine_pid" 2>/dev/null; then
      echo "graphics adapter identity test timed out" >&2
      kill -TERM "$wine_pid" 2>/dev/null || true
    fi
  ) &
  watchdog_pid=$!
  wait "$wine_pid" || status=$?
  kill "$watchdog_pid" 2>/dev/null || true
  wait "$watchdog_pid" 2>/dev/null || true
  [ "$status" -eq 0 ] || return "$status"
}

run_probe "$work/provider-default-prefix"
run_probe "$work/explicit-prefix" \
  D3DM_VENDOR_ID=0x106b \
  D3DM_DEVICE_ID=0x03f2 \
  D3DM_DEVICE_SUBSYS=0 \
  D3DM_DEVICE_REVISION=0 \
  "D3DM_DEVICE_DESCRIPTION=Explicit Test GPU"

echo "[test] graphics adapter identity passed"
