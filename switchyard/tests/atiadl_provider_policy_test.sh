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

work="$(/usr/bin/mktemp -d /tmp/switchyard-atiadl-provider.XXXXXX)"
prefix="$work/prefix"
gptk_env=(
  "WINEDLLPATH=$GPTK_PATH/redist/lib/wine"
  "DYLD_FRAMEWORK_PATH=$GPTK_PATH/redist/lib/external"
  "DYLD_LIBRARY_PATH=$GPTK_PATH/redist/lib/external"
  "SWITCHYARD_GPTK_PATH=$GPTK_PATH"
)

# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

run_case() {
  local label="$1"
  local executable="$2"
  local expected="$3"
  local probe_args=(atiadlxx.dll)
  shift 3

  if [ "$expected" = unavailable ]; then
    probe_args+=(unavailable)
  fi

  echo "[test] $label"
  env "${gptk_env[@]}" "$@" WINEPREFIX="$prefix" WINEDEBUG=-all \
    WINEDLLOVERRIDES="atiadlxx=b" \
    "$RUNTIME/bin/switchyard-wine" "$work/$executable" "${probe_args[@]}"
}

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror \
  -o "$work/atiadl_info_probe.exe" "$ROOT_DIR/switchyard/tests/atiadl_info_probe.c"
x86_64-w64-mingw32-gcc -Wall -Wextra -Werror \
  -o "$work/atiadl-native-provider-probe.exe" \
  "$ROOT_DIR/switchyard/tests/atiadl_native_provider_probe.c"
x86_64-w64-mingw32-gcc -Wall -Wextra -Werror -shared \
  -o "$work/atiadlxx.dll" \
  "$ROOT_DIR/switchyard/tests/atiadl_native_provider.c"
cp "$work/atiadl_info_probe.exe" "$work/Overwatch.exe"
cp "$work/atiadl_info_probe.exe" "$work/GTA5_Enhanced.exe"
cp "$work/atiadl_info_probe.exe" "$work/PlayGTAV.exe"

echo "[test] native_provider_remains_preferred"
env "${gptk_env[@]}" WINEPREFIX="$prefix" WINEDEBUG=-all \
  "$RUNTIME/bin/switchyard-wine" "$work/atiadl-native-provider-probe.exe"

run_case "generic_process_is_unavailable" atiadl_info_probe.exe unavailable
run_case "overwatch_name_does_not_change_provider_policy" Overwatch.exe unavailable
run_case "gta_game_name_does_not_change_provider_policy" GTA5_Enhanced.exe unavailable
run_case "gta_launcher_name_does_not_change_provider_policy" PlayGTAV.exe unavailable

echo "[test] atiadl provider policy passed"
