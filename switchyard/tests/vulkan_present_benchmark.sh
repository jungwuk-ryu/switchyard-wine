#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CC="${CC:-x86_64-w64-mingw32-gcc}"
warmup=600
frames=12000
repetitions=5
timeout_seconds=600
smoke=0
output_dir=""
baseline_wine=""
candidate_wine=""
baseline_prefix=""
candidate_prefix=""
baseline_args=()
candidate_args=()
baseline_env=()
candidate_env=()

usage() {
  cat >&2 <<EOF
usage:
  $0 --baseline-wine COMMAND --baseline-prefix PREFIX \\
     --candidate-wine COMMAND --candidate-prefix PREFIX [options]
  $0 BASELINE_WINE BASELINE_PREFIX CANDIDATE_WINE CANDIDATE_PREFIX [options]

options:
  --baseline-arg ARG     append one argument to the baseline Wine command
  --candidate-arg ARG    append one argument to the candidate Wine command
  --baseline-env N=V     set one environment variable for baseline runs
  --candidate-env N=V    set one environment variable for candidate runs
  --warmup N             warmup presents per repetition (default: 600)
  --frames N|smoke       measured presents, or 60-frame/one-repeat smoke mode
  --repetitions N        repetitions per Wine build (default/minimum: 5)
  --timeout SECONDS      per-repetition watchdog (default: 600)
  --output-dir DIR       retain PE, JSON, and /usr/bin/time -l logs in DIR
EOF
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

if [ "$#" -ge 4 ] && [[ "$1" != --* ]]; then
  baseline_wine="$1"
  baseline_prefix="$2"
  candidate_wine="$3"
  candidate_prefix="$4"
  shift 4
fi

while [ "$#" -gt 0 ]; do
  case "$1" in
    --baseline-wine)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      baseline_wine="$2"
      shift 2
      ;;
    --candidate-wine)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      candidate_wine="$2"
      shift 2
      ;;
    --baseline-prefix)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      baseline_prefix="$2"
      shift 2
      ;;
    --candidate-prefix)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      candidate_prefix="$2"
      shift 2
      ;;
    --baseline-arg)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      baseline_args+=("$2")
      shift 2
      ;;
    --candidate-arg)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      candidate_args+=("$2")
      shift 2
      ;;
    --baseline-env)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      [[ "$2" =~ ^[A-Za-z_][A-Za-z0-9_]*= ]] || {
        echo "invalid --baseline-env assignment: $2" >&2
        exit 2
      }
      baseline_env+=("$2")
      shift 2
      ;;
    --candidate-env)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      [[ "$2" =~ ^[A-Za-z_][A-Za-z0-9_]*= ]] || {
        echo "invalid --candidate-env assignment: $2" >&2
        exit 2
      }
      candidate_env+=("$2")
      shift 2
      ;;
    --warmup)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      warmup="$2"
      shift 2
      ;;
    --frames)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      if [ "$2" = smoke ]; then
        smoke=1
        frames=60
        warmup=10
        repetitions=1
      else
        smoke=0
        frames="$2"
      fi
      shift 2
      ;;
    --repetitions)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      repetitions="$2"
      shift 2
      ;;
    --timeout)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      timeout_seconds="$2"
      shift 2
      ;;
    --output-dir)
      [ "$#" -ge 2 ] || { usage; exit 2; }
      output_dir="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

[ -n "$baseline_wine" ] && [ -n "$candidate_wine" ] && \
  [ -n "$baseline_prefix" ] && [ -n "$candidate_prefix" ] || {
  usage
  exit 2
}
if [[ "$baseline_wine" = */* ]]; then
  [ -x "$baseline_wine" ] || { echo "baseline Wine command is not executable: $baseline_wine" >&2; exit 2; }
  baseline_wine="$(cd -P "$(dirname "$baseline_wine")" && pwd -P)/$(basename "$baseline_wine")"
else
  baseline_wine="$(command -v "$baseline_wine")" || {
    echo "baseline Wine command was not found: $baseline_wine" >&2
    exit 2
  }
fi
if [[ "$candidate_wine" = */* ]]; then
  [ -x "$candidate_wine" ] || { echo "candidate Wine command is not executable: $candidate_wine" >&2; exit 2; }
  candidate_wine="$(cd -P "$(dirname "$candidate_wine")" && pwd -P)/$(basename "$candidate_wine")"
else
  candidate_wine="$(command -v "$candidate_wine")" || {
    echo "candidate Wine command was not found: $candidate_wine" >&2
    exit 2
  }
fi
mkdir -p "$baseline_prefix" "$candidate_prefix"
baseline_prefix="$(cd -P "$baseline_prefix" && pwd -P)"
candidate_prefix="$(cd -P "$candidate_prefix" && pwd -P)"
[ "$baseline_prefix" != "$candidate_prefix" ] || {
  echo "baseline and candidate prefixes must be distinct" >&2
  exit 2
}
is_positive_integer "$warmup" && [ "$warmup" -le 10000 ] || {
  echo "--warmup must be an integer from 1 through 10000" >&2
  exit 2
}
is_positive_integer "$frames" && [ "$frames" -le 100000 ] || {
  echo "--frames must be an integer from 1 through 100000, or smoke" >&2
  exit 2
}
is_positive_integer "$repetitions" && [ "$repetitions" -le 50 ] || {
  echo "--repetitions must be an integer from 1 through 50" >&2
  exit 2
}
if [ "$smoke" -eq 0 ] && [ "$repetitions" -lt 5 ]; then
  echo "at least 5 repetitions are required outside smoke mode" >&2
  exit 2
fi
is_positive_integer "$timeout_seconds" && [ "$timeout_seconds" -le 3600 ] || {
  echo "--timeout must be an integer from 1 through 3600" >&2
  exit 2
}
command -v "$CC" >/dev/null || { echo "$CC is required" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required for result validation" >&2; exit 1; }
[ -x /usr/bin/time ] || { echo "/usr/bin/time is required" >&2; exit 1; }

work="$(/usr/bin/mktemp -d /tmp/switchyard-vulkan-present.XXXXXX)"
retain=0
if [ -n "$output_dir" ]; then
  mkdir -p "$output_dir"
  output_dir="$(cd -P "$output_dir" && pwd -P)"
  retain=1
else
  output_dir="$work/results"
  mkdir -p "$output_dir"
fi

cleanup() {
  /bin/rm -rf "$work"
}
trap cleanup EXIT

benchmark_exe="$output_dir/vulkan-present-benchmark.exe"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
  -I"$ROOT_DIR/include" \
  -o "$benchmark_exe" "$ROOT_DIR/switchyard/tests/vulkan_present_benchmark.c" \
  -luser32 -lgdi32

baseline_results=()
candidate_results=()

run_one() {
  local label="$1"
  local repetition="$2"
  local wine_command="$3"
  local prefix="$4"
  shift 4
  local command_args=("$@")
  local run_env=()
  local json_file="$output_dir/${label}-${repetition}.json"
  local time_file="$output_dir/${label}-${repetition}.time"
  local status=0

  if [ "$label" = baseline ]; then
    run_env=( ${baseline_env[@]+"${baseline_env[@]}"} )
  else
    run_env=( ${candidate_env[@]+"${candidate_env[@]}"} )
  fi

  mkdir -p "$prefix"
  python3 - "$timeout_seconds" "$label" "$repetition" "$json_file" "$time_file" \
    "$prefix" "${WINEDEBUG:--all}" "${#run_env[@]}" \
    ${run_env[@]+"${run_env[@]}"} -- "$wine_command" \
    ${command_args[@]+"${command_args[@]}"} "$benchmark_exe" \
    --warmup "$warmup" --frames "$frames" <<'PY' || status=$?
import os
import signal
import subprocess
import sys

timeout = int(sys.argv[1])
label, repetition, json_path, time_path = sys.argv[2:6]
prefix, wine_debug, env_count = sys.argv[6], sys.argv[7], int(sys.argv[8])
assignments = sys.argv[9:9 + env_count]
separator = 9 + env_count
if separator >= len(sys.argv) or sys.argv[separator] != "--":
    raise SystemExit("internal error: benchmark command separator is missing")
command = ["/usr/bin/time", "-l", *sys.argv[separator + 1:]]
environment = os.environ.copy()
for assignment in assignments:
    name, separator, value = assignment.partition("=")
    if not separator:
        raise SystemExit(f"internal error: invalid environment assignment {assignment!r}")
    environment[name] = value
environment["WINEPREFIX"] = prefix
environment["WINEDEBUG"] = wine_debug

def terminate_group(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()

with open(json_path, "wb") as json_stream, open(time_path, "wb") as time_stream:
    process = subprocess.Popen(command, stdout=json_stream, stderr=time_stream,
                               env=environment, start_new_session=True)
    try:
        result = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f"{label} repetition {repetition} timed out after {timeout} seconds", file=sys.stderr)
        terminate_group(process)
        raise SystemExit(124)
    except KeyboardInterrupt:
        terminate_group(process)
        raise
raise SystemExit(result)
PY
  if [ "$status" -ne 0 ]; then
    echo "$label repetition $repetition failed with status $status" >&2
    sed -n '1,160p' "$time_file" >&2
    return "$status"
  fi
  python3 - "$json_file" "$frames" <<'PY'
import json
import sys

path, expected = sys.argv[1], int(sys.argv[2])
with open(path, encoding="utf-8") as stream:
    data = json.load(stream)
required = (
    "provider", "device", "format", "count",
    "present_cpu_ns_mean", "present_cpu_ns_p50",
    "present_cpu_ns_p95", "present_cpu_ns_p99",
    "full_frame_cpu_ns_mean", "full_frame_cpu_ns_p50",
    "full_frame_cpu_ns_p95", "full_frame_cpu_ns_p99",
    "gpu_clear_ns_available",
)
missing = [key for key in required if key not in data]
if missing:
    raise SystemExit(f"{path}: missing JSON keys: {', '.join(missing)}")
if data["count"] != expected:
    raise SystemExit(f"{path}: got {data['count']} samples, expected {expected}")
if data["format"] not in ("VK_FORMAT_B8G8R8A8_SRGB", "VK_FORMAT_R8G8B8A8_SRGB"):
    raise SystemExit(f"{path}: non-SDR-sRGB format {data['format']!r}")
PY
  printf '[benchmark] %s repetition %s: ' "$label" "$repetition" >&2
  python3 - "$json_file" <<'PY' >&2
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    d = json.load(stream)
print(f"present p50={d['present_cpu_ns_p50'] / 1000:.3f} us, "
      f"mean={d['present_cpu_ns_mean'] / 1000:.3f} us")
PY
}

for ((repetition = 1; repetition <= repetitions; ++repetition)); do
  if ((repetition % 2)); then
    run_one baseline "$repetition" "$baseline_wine" "$baseline_prefix" \
      ${baseline_args[@]+"${baseline_args[@]}"}
    run_one candidate "$repetition" "$candidate_wine" "$candidate_prefix" \
      ${candidate_args[@]+"${candidate_args[@]}"}
  else
    run_one candidate "$repetition" "$candidate_wine" "$candidate_prefix" \
      ${candidate_args[@]+"${candidate_args[@]}"}
    run_one baseline "$repetition" "$baseline_wine" "$baseline_prefix" \
      ${baseline_args[@]+"${baseline_args[@]}"}
  fi
  baseline_results+=("$output_dir/baseline-${repetition}.json")
  candidate_results+=("$output_dir/candidate-${repetition}.json")
done

summary_file="$output_dir/summary.json"
set +e
python3 - "$summary_file" "${baseline_results[@]}" -- "${candidate_results[@]}" <<'PY'
import json
import statistics
import sys

summary_path = sys.argv[1]
separator = sys.argv.index("--")
baseline_paths = sys.argv[2:separator]
candidate_paths = sys.argv[separator + 1:]

def load(paths):
    results = []
    for path in paths:
        with open(path, encoding="utf-8") as stream:
            results.append(json.load(stream))
    return results

baseline_results = load(baseline_paths)
candidate_results = load(candidate_paths)
baseline_ns = statistics.median(r["present_cpu_ns_p50"] for r in baseline_results)
candidate_ns = statistics.median(r["present_cpu_ns_p50"] for r in candidate_results)
delta_ns = candidate_ns - baseline_ns
delta_percent = delta_ns * 100.0 / baseline_ns if baseline_ns else float("inf")
passed = candidate_ns <= baseline_ns * 1.05 or abs(delta_ns) < 20_000
summary = {
    "baseline_repetitions": len(baseline_results),
    "candidate_repetitions": len(candidate_results),
    "comparison_metric": "median_of_present_cpu_ns_p50",
    "baseline_ns": baseline_ns,
    "candidate_ns": candidate_ns,
    "delta_ns": delta_ns,
    "delta_percent": delta_percent,
    "relative_limit_percent": 5.0,
    "absolute_limit_ns": 20_000,
    "pass": passed,
}
with open(summary_path, "w", encoding="utf-8") as stream:
    json.dump(summary, stream, sort_keys=True)
    stream.write("\n")
print(json.dumps(summary, sort_keys=True))
raise SystemExit(0 if passed else 1)
PY
comparison_status=$?
set -e

if [ "$retain" -eq 1 ]; then
  echo "[benchmark] retained results: $output_dir" >&2
fi
if [ "$comparison_status" -ne 0 ]; then
  echo "[benchmark] FAIL: candidate exceeds both the 5% and 20 us allowances" >&2
  exit 1
fi
echo "[benchmark] PASS" >&2
