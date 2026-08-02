#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
GPTK_PATH="${2:-}"

[ -n "$RUNTIME" ] || { echo "usage: $0 RUNTIME [GPTK_PATH]" >&2; exit 2; }
[ -x "$RUNTIME/bin/switchyard-wine" ] || { echo "runtime launcher is missing" >&2; exit 1; }
[ -x "$RUNTIME/bin/wineserver" ] || { echo "runtime wineserver is missing" >&2; exit 1; }
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

gptk_env=()

if [ -n "$GPTK_PATH" ]; then
  [ -f "$GPTK_PATH/redist/lib/wine/x86_64-windows/d3d12.dll" ] || {
    echo "selected GPTK does not contain d3d12.dll" >&2
    exit 1
  }
  [ -f "$GPTK_PATH/redist/lib/wine/x86_64-unix/d3d12.so" ] || {
    echo "selected GPTK does not contain d3d12.so" >&2
    exit 1
  }

  gptk_env=(
    "WINEDLLPATH=$GPTK_PATH/redist/lib/wine"
    "DYLD_LIBRARY_PATH=$GPTK_PATH/redist/lib/external"
    "DYLD_FRAMEWORK_PATH=$GPTK_PATH/redist/lib/external:$GPTK_PATH/redist/lib/frameworks"
    "SWITCHYARD_GPTK_PATH=$GPTK_PATH"
  )
else
  [ -f "$RUNTIME/lib/wine/x86_64-windows/d3d12.dll" ] || {
    echo "runtime does not contain the built-in d3d12.dll" >&2
    exit 1
  }
fi

work="$(/usr/bin/mktemp -d /tmp/switchyard-d3d12-shader-cache.XXXXXX)"
prefix="$work/prefix"

# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -Os -s \
  -o "$work/d3d12-shader-cache-probe.exe" \
  "$ROOT_DIR/switchyard/tests/d3d12_shader_cache_probe.c" \
  -ld3d12 -ldxgi -luuid -lpsapi -ladvapi32

run_probe() {
  local log="$work/d3d12-shader-cache-probe.log"
  local -a probe_env=(env -u WINE_OPENGL_DRIVER)

  if [ "${#gptk_env[@]}" -ne 0 ]; then
    probe_env+=("${gptk_env[@]}")
  fi

  "${probe_env[@]}" \
    WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
    "$RUNTIME/bin/switchyard-wine" "$work/d3d12-shader-cache-probe.exe" \
    >"$log" 2>&1 &
  local probe_pid=$!

  (
    sleep 300
    if kill -0 "$probe_pid" 2>/dev/null; then
      echo "d3d12 shader cache probe timed out" >&2
      kill -TERM "$probe_pid" 2>/dev/null || true
    fi
  ) &
  local watchdog_pid=$!

  local status=0
  wait "$probe_pid" || status=$?
  kill "$watchdog_pid" 2>/dev/null || true
  wait "$watchdog_pid" 2>/dev/null || true

  /bin/cat "$log"
  if [ "$status" -eq 77 ] && /usr/bin/grep -q "\[skip\]" "$log"; then
    return 77
  fi
  [ "$status" -eq 0 ] || return "$status"

  /usr/bin/grep -F "[summary] d3d12_shader_cache_probe ret=0" "$log" >/dev/null
}

status=0
run_probe || status=$?
if [ "$status" -eq 77 ]; then
  echo "D3D12 shader cache probe skipped: required device/runtime capability unavailable"
  exit 77
fi
[ "$status" -eq 0 ] || exit "$status"

echo "D3D12 shader cache probe passed"
