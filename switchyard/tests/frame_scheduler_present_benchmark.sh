#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
GPTK_PATH="${2:-}"
RUNS="${3:-5}"

[ -x "$RUNTIME/bin/switchyard-wine" ] || {
  echo "usage: $0 RUNTIME GPTK_PATH [RUNS]" >&2
  exit 2
}
[ -f "$GPTK_PATH/redist/lib/external/D3DMetal.framework/D3DMetal" ] || {
  echo "selected GPTK does not contain D3DMetal" >&2
  exit 1
}
case "$RUNS" in
  ''|*[!0-9]*|0) echo "RUNS must be a positive integer" >&2; exit 2 ;;
esac
command -v x86_64-w64-mingw32-gcc >/dev/null || {
  echo "x86_64-w64-mingw32-gcc is required" >&2
  exit 1
}

work="$(/usr/bin/mktemp -d /tmp/switchyard-frame-scheduler.XXXXXX)"
prefix="$work/prefix"
# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() {
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
  /bin/rm -rf "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Werror \
  -o "$work/frame-scheduler-present-benchmark.exe" \
  "$ROOT_DIR/switchyard/tests/frame_scheduler_present_benchmark.c" \
  -ld3d11 -ldxgi -ldxguid -luuid

echo "scheduler_benchmark_host runs=$RUNS runtime=$(basename "$RUNTIME")"
"$ROOT_DIR/switchyard/build_runtime.sh" --source-info | sed 's/^/scheduler_source_/'
if [ -f "$RUNTIME/switchyard-runtime.json" ]; then
  echo "scheduler_runtime_manifest_sha256=$(shasum -a 256 "$RUNTIME/switchyard-runtime.json" | awk '{print $1}')"
fi
echo "scheduler_prefix_policy=fresh-prefix-shared-across-runs config=defaults WINEDEBUG=-all"
/usr/bin/sw_vers | tr '\n' ' '
echo
pmset -g batt 2>/dev/null | tr '\n' ' '
echo
pmset -g therm 2>/dev/null | tr '\n' ' '
echo
system_profiler SPDisplaysDataType 2>/dev/null | sed -n \
  -e '/Chipset Model:/p' -e '/Resolution:/p' -e '/Main Display:/p' -e '/Refresh Rate:/p'

for ((run = 1; run <= RUNS; ++run)); do
  time_output="$work/time-$run.txt"
  echo "scheduler_benchmark_run run=$run"
  set +e
  env -u WINE_OPENGL_DRIVER \
    WINEDLLPATH="$GPTK_PATH/redist/lib/wine" \
    DYLD_FRAMEWORK_PATH="$GPTK_PATH/redist/lib/external" \
    DYLD_LIBRARY_PATH="$GPTK_PATH/redist/lib/external" \
    SWITCHYARD_GPTK_PATH="$GPTK_PATH" \
    WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
    /usr/bin/time -lp "$RUNTIME/bin/switchyard-wine" \
      "$work/frame-scheduler-present-benchmark.exe" 2>"$time_output"
  status=$?
  set -e
  cat "$time_output"
  real_seconds="$(awk '$1 == "real" { value=$2 } $2 == "real" { value=$1 } END { print value }' "$time_output")"
  voluntary="$(awk '$2 == "voluntary" && $3 == "context" { value=$1 } END { print value }' "$time_output")"
  involuntary="$(awk '$2 == "involuntary" && $3 == "context" { value=$1 } END { print value }' "$time_output")"
  if [ -n "$real_seconds" ] && [ -n "$voluntary" ] && [ -n "$involuntary" ]; then
    awk -v real="$real_seconds" -v voluntary="$voluntary" -v involuntary="$involuntary" \
      'BEGIN { printf "scheduler_wakeup_proxy real_s=%.3f voluntary_context_switches=%d involuntary_context_switches=%d context_switches_per_s=%.3f\n", real, voluntary, involuntary, (voluntary + involuntary) / real }'
  fi
  [ "$status" -eq 0 ] || exit "$status"
done
