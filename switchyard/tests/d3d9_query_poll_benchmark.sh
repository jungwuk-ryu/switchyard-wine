#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASELINE_RUNTIME="${1:-}"
CANDIDATE_RUNTIME="${2:-}"
OUTPUT_DIR="${3:-}"
RUNS="${4:-5}"
CC="${CC:-i686-w64-mingw32-gcc}"

usage() {
  echo "usage: $0 BASELINE_RUNTIME CANDIDATE_RUNTIME OUTPUT_DIR [RUNS]" >&2
}

[ -x "$BASELINE_RUNTIME/bin/switchyard-wine" ] && \
  [ -x "$CANDIDATE_RUNTIME/bin/switchyard-wine" ] && [ -n "$OUTPUT_DIR" ] || {
  usage
  exit 2
}
case "$RUNS" in
  ''|*[!0-9]*|0) echo "RUNS must be a positive integer" >&2; exit 2 ;;
esac
[ "$RUNS" -le 20 ] || { echo "RUNS must not exceed 20" >&2; exit 2; }
command -v "$CC" >/dev/null || { echo "$CC is required" >&2; exit 1; }
command -v rg >/dev/null || { echo "rg is required" >&2; exit 1; }

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd -P "$OUTPUT_DIR" && pwd -P)"
WORK="$(/usr/bin/mktemp -d /tmp/switchyard-d3d9-query-poll.XXXXXX)"
BASELINE_PREFIX="$WORK/baseline-prefix"
CANDIDATE_PREFIX="$WORK/candidate-prefix"
BENCHMARK_EXE="$OUTPUT_DIR/d3d9-query-poll-benchmark.exe"

cleanup() {
  WINEPREFIX="$BASELINE_PREFIX" "$BASELINE_RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  WINEPREFIX="$CANDIDATE_PREFIX" "$CANDIDATE_RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
  /bin/rm -rf "$WORK"
}
trap cleanup EXIT

"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
  -o "$BENCHMARK_EXE" "$ROOT_DIR/switchyard/tests/d3d9_query_poll_benchmark.c" \
  -ld3d9 -lgdi32 -luser32

prepare_prefix() {
  local runtime="$1"
  local prefix="$2"

  mkdir -p "$prefix"
  WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
    "$runtime/bin/wineboot" -u >/dev/null 2>&1
  WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
    "$runtime/bin/switchyard-wine" reg add 'HKCU\Software\Wine\Direct3D' \
      /v renderer /t REG_SZ /d gl /f >/dev/null
}

baseline_polls=()
candidate_polls=()
benchmark_generations=0

run_case() {
  local name="$1"
  local runtime="$2"
  local prefix="$3"
  local run="$4"
  local log line_count skip_count line generations total_polls min_polls max_polls
  local result_pattern='^d3d9_query_poll_benchmark status=ok generations=[0-9]+ clears=[0-9]+ total_polls=[0-9]+ min_polls=[0-9]+ max_polls=[0-9]+ wall_ms=[0-9]+([.][0-9]+)? cpu_ms=[0-9]+([.][0-9]+)?$'

  log="$OUTPUT_DIR/$name-$run.log"
  env -u WINE_OPENGL_DRIVER \
    WINEPREFIX="$prefix" WINEDEBUG='-all,err+winediag' WINE_D3D_CONFIG='csmt=0x1' \
    WINEDLLOVERRIDES="winedbg.exe=d" \
    /usr/bin/time -lp "$runtime/bin/switchyard-wine" "$BENCHMARK_EXE" >"$log" 2>&1
  rg -q 'Using the OpenGL renderer' "$log" || {
    echo "$name run $run did not confirm the OpenGL renderer" >&2
    sed -n '1,80p' "$log" >&2
    exit 1
  }
  rg -q 'Setting multithreaded command stream to 0x1' "$log" || {
    echo "$name run $run did not confirm the multithreaded command stream" >&2
    sed -n '1,80p' "$log" >&2
    exit 1
  }

  skip_count="$(tr -d '\r' <"$log" | grep -Ec '^d3d9_query_poll_benchmark status=skip ' || true)"
  if [ "$skip_count" -eq 1 ] && rg -q "$result_pattern" "$log"; then
    echo "$name run $run produced both benchmark skip and result lines" >&2
    exit 1
  elif [ "$skip_count" -eq 1 ]; then
    printf 'd3d9_query_poll_comparison status=inconclusive reason=event_query_unavailable case=%s run=%s\n' \
      "$name" "$run"
    exit 77
  elif [ "$skip_count" -gt 1 ]; then
    echo "$name run $run produced multiple benchmark skip lines" >&2
    exit 1
  fi

  line_count="$(tr -d '\r' <"$log" | grep -Ec "$result_pattern" || true)"
  if [ "$line_count" -ne 1 ]; then
    echo "$name run $run produced $line_count valid benchmark result lines" >&2
    sed -n '1,80p' "$log" >&2
    exit 1
  fi
  line="$(tr -d '\r' <"$log" | grep -E "$result_pattern")"
  generations="${line#* generations=}"
  generations="${generations%% *}"
  total_polls="${line#* total_polls=}"
  total_polls="${total_polls%% *}"
  min_polls="${line#* min_polls=}"
  min_polls="${min_polls%% *}"
  max_polls="${line#* max_polls=}"
  max_polls="${max_polls%% *}"

  if [ "$generations" -eq 0 ] || [ "$min_polls" -eq 0 ] \
      || [ "$min_polls" -gt "$max_polls" ] \
      || [ "$total_polls" -lt "$((generations * min_polls))" ] \
      || [ "$total_polls" -gt "$((generations * max_polls))" ]; then
    echo "$name run $run produced inconsistent poll counts" >&2
    exit 1
  fi
  if [ "$benchmark_generations" -eq 0 ]; then
    benchmark_generations="$generations"
  elif [ "$benchmark_generations" -ne "$generations" ]; then
    echo "$name run $run changed the benchmark generation count" >&2
    exit 1
  fi

  if [ "$name" = baseline ]; then
    baseline_polls+=("$total_polls")
  else
    candidate_polls+=("$total_polls")
  fi
  printf '%s run=%s %s\n' "$name" "$run" "$line"
}

median_poll_count() {
  local index=$(( $# / 2 + 1 ))

  printf '%s\n' "$@" | LC_ALL=C sort -n | sed -n "${index}p"
}

printf 'd3d9_query_poll_host baseline=%s candidate=%s runs=%s\n' \
  "$(basename "$BASELINE_RUNTIME")" "$(basename "$CANDIDATE_RUNTIME")" "$RUNS"
/usr/bin/sw_vers | tr '\n' ' '
echo
prepare_prefix "$BASELINE_RUNTIME" "$BASELINE_PREFIX"
prepare_prefix "$CANDIDATE_RUNTIME" "$CANDIDATE_PREFIX"
printf 'd3d9_query_poll_case name=baseline runtime=%s\n' "$(basename "$BASELINE_RUNTIME")"
printf 'd3d9_query_poll_case name=candidate runtime=%s\n' "$(basename "$CANDIDATE_RUNTIME")"
for ((run = 1; run <= RUNS; ++run)); do
  if [ $((run % 2)) -eq 1 ]; then
    run_case baseline "$BASELINE_RUNTIME" "$BASELINE_PREFIX" "$run"
    run_case candidate "$CANDIDATE_RUNTIME" "$CANDIDATE_PREFIX" "$run"
  else
    run_case candidate "$CANDIDATE_RUNTIME" "$CANDIDATE_PREFIX" "$run"
    run_case baseline "$BASELINE_RUNTIME" "$BASELINE_PREFIX" "$run"
  fi
done

baseline_median="$(median_poll_count "${baseline_polls[@]}")"
candidate_median="$(median_poll_count "${candidate_polls[@]}")"
improved_pairs=0
for ((run = 0; run < RUNS; ++run)); do
  if [ "$((candidate_polls[run] * 4))" -le "${baseline_polls[run]}" ]; then
    improved_pairs=$((improved_pairs + 1))
  fi
done
required_pairs=$((RUNS / 2 + 1))

if [ "$baseline_median" -le "$((200 * benchmark_generations))" ]; then
  printf 'd3d9_query_poll_comparison status=inconclusive reason=baseline_not_starved baseline_median=%s generations=%s\n' \
    "$baseline_median" "$benchmark_generations"
  exit 77
fi
if [ "$candidate_median" -gt "$((baseline_median / 10))" ] \
    || [ "$improved_pairs" -lt "$required_pairs" ]; then
  printf 'd3d9_query_poll_comparison status=failed baseline_median=%s candidate_median=%s improved_pairs=%s required_pairs=%s\n' \
    "$baseline_median" "$candidate_median" "$improved_pairs" "$required_pairs" >&2
  exit 1
fi
printf 'd3d9_query_poll_comparison status=ok baseline_median=%s candidate_median=%s improved_pairs=%s required_pairs=%s\n' \
  "$baseline_median" "$candidate_median" "$improved_pairs" "$required_pairs"
