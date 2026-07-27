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

work="$(/usr/bin/mktemp -d /tmp/switchyard-atiadl-scope.XXXXXX)"
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
  env -u SWITCHYARD_ADL_COMPAT "${gptk_env[@]}" "$@" WINEPREFIX="$prefix" WINEDEBUG=-all \
    WINEDLLOVERRIDES="atiadlxx=b" \
    "$RUNTIME/bin/switchyard-wine" "$work/$executable" "${probe_args[@]}"
}

x86_64-w64-mingw32-gcc -Wall -Wextra -Werror \
  -o "$work/atiadl_info_probe.exe" "$ROOT_DIR/switchyard/tests/atiadl_info_probe.c"
cp "$work/atiadl_info_probe.exe" "$work/Overwatch.exe"
cp "$work/atiadl_info_probe.exe" "$work/GTA5_Enhanced.exe"
cp "$work/atiadl_info_probe.exe" "$work/PlayGTAV.exe"

run_case "generic_process_is_unavailable" atiadl_info_probe.exe unavailable
run_case "overwatch_is_unavailable" Overwatch.exe unavailable
run_case "gta_enhanced_receives_compatibility_version" GTA5_Enhanced.exe ""
run_case "gta_launcher_receives_compatibility_version" PlayGTAV.exe ""
run_case "explicit_opt_in_receives_compatibility_version" atiadl_info_probe.exe "" \
  SWITCHYARD_ADL_COMPAT=1
run_case "explicit_opt_out_overrides_gta_allowlist" GTA5_Enhanced.exe unavailable \
  SWITCHYARD_ADL_COMPAT=0

echo "[test] atiadl process scope test passed"
