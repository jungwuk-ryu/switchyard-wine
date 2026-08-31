#!/usr/bin/env bash
# Build and run the translated x86-64 Apple Silicon CPU bottleneck benchmark.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNTIME="${1:-}"
RUNS="${2:-5}"

usage() {
    echo "usage: $0 RUNTIME [RUNS]" >&2
    exit 2
}

[[ -n "$RUNTIME" ]] || usage
[[ "$RUNTIME" = /* ]] || {
    echo "RUNTIME must be an absolute path" >&2
    exit 2
}
[[ -x "$RUNTIME/bin/switchyard-wine" && -x "$RUNTIME/bin/wineserver" ]] || {
    echo "RUNTIME does not contain switchyard-wine and wineserver: $RUNTIME" >&2
    exit 2
}
[[ "$RUNS" =~ ^[1-9][0-9]*$ && "$RUNS" -le 100 ]] || {
    echo "RUNS must be an integer from 1 through 100" >&2
    exit 2
}
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || {
    echo "this benchmark must run on Apple Silicon macOS" >&2
    exit 1
}
command -v x86_64-w64-mingw32-gcc >/dev/null || {
    echo "x86_64-w64-mingw32-gcc is required" >&2
    exit 1
}

target_ms="${APPLE_CPU_BENCH_TARGET_MS:-80}"
trials="${APPLE_CPU_BENCH_TRIALS:-9}"
case_filter="${APPLE_CPU_BENCH_CASE:-all}"
[[ "$target_ms" =~ ^[0-9]+$ ]] || {
    echo "APPLE_CPU_BENCH_TARGET_MS must be an integer" >&2
    exit 2
}
[[ "$trials" =~ ^[0-9]+$ ]] || {
    echo "APPLE_CPU_BENCH_TRIALS must be an integer" >&2
    exit 2
}
target_ms=$((10#$target_ms))
trials=$((10#$trials))
((target_ms >= 10 && target_ms <= 10000)) || {
    echo "APPLE_CPU_BENCH_TARGET_MS must be from 10 through 10000" >&2
    exit 2
}
((trials >= 3 && trials <= 101)) || {
    echo "APPLE_CPU_BENCH_TRIALS must be from 3 through 101" >&2
    exit 2
}
[[ "$case_filter" == all || "$case_filter" =~ ^[a-z0-9_]+$ ]] || {
    echo "APPLE_CPU_BENCH_CASE must be all or one lowercase benchmark name" >&2
    exit 2
}
runtime_archs="$(/usr/bin/lipo -archs "$RUNTIME/bin/switchyard-wine" 2>/dev/null || true)"
[[ " $runtime_archs " == *" arm64 "* ]] || {
    echo "RUNTIME switchyard-wine does not contain an arm64 slice" >&2
    exit 1
}

sanitize_value() {
    LC_ALL=C /usr/bin/tr -cs 'A-Za-z0-9._+-' '_' | /usr/bin/sed 's/^_*//; s/_*$//'
}

sysctl_value() {
    /usr/sbin/sysctl -n "$1" 2>/dev/null || true
}

work="$(/usr/bin/mktemp -d /private/tmp/switchyard-apple-cpu.XXXXXX)"
prefix="$work/prefix"
benchmark="$work/apple-silicon-cpu-bottleneck.exe"

cleanup() {
    WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -k >/dev/null 2>&1 || true
    WINEPREFIX="$prefix" "$RUNTIME/bin/wineserver" -w >/dev/null 2>&1 || true
    /bin/rm -rf -- "$work"
}
trap cleanup EXIT

x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
    -Wconversion -Wshadow -fno-tree-vectorize -fno-tree-slp-vectorize \
    -static-libgcc -o "$benchmark" \
    "$ROOT_DIR/switchyard/tests/apple_silicon_cpu_bottleneck_benchmark.c" -lm

hw_model="$(printf '%s' "$(sysctl_value hw.model)" | sanitize_value)"
chip="$(printf '%s' "$(sysctl_value machdep.cpu.brand_string)" | sanitize_value)"
macos_product="$(printf '%s' "$(/usr/bin/sw_vers -productVersion)" | sanitize_value)"
macos_build="$(printf '%s' "$(/usr/bin/sw_vers -buildVersion)" | sanitize_value)"
logical_cpu="$(printf '%s' "$(sysctl_value hw.logicalcpu)" | sanitize_value)"
physical_cpu="$(printf '%s' "$(sysctl_value hw.physicalcpu)" | sanitize_value)"
[[ -n "$hw_model" && -n "$chip" && -n "$macos_product" && -n "$macos_build" ]] || {
    echo "cannot determine the Apple Silicon host identity" >&2
    exit 1
}
host_signature="$(
    printf '%s\n' "$hw_model|$chip|$macos_product|$macos_build|$logical_cpu|$physical_cpu" |
        /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}'
)"
runtime_name="$(printf '%s' "$(basename "$RUNTIME")" | sanitize_value)"
runtime_manifest_sha256=missing
if [[ -f "$RUNTIME/switchyard-runtime.json" && ! -L "$RUNTIME/switchyard-runtime.json" ]]; then
    runtime_manifest_sha256="$(/usr/bin/shasum -a 256 \
        "$RUNTIME/switchyard-runtime.json" | /usr/bin/awk '{print $1}')"
fi

printf 'apple_cpu_host schema=1 signature=%s hw_model=%s chip=%s macos=%s build=%s arch=arm64 logical_cpu=%s physical_cpu=%s\n' \
    "$host_signature" "$hw_model" "$chip" "$macos_product" "$macos_build" \
    "$logical_cpu" "$physical_cpu"
printf 'apple_cpu_runtime schema=1 name=%s manifest_sha256=%s runs=%s target_ms=%s trials=%s case=%s\n' \
    "$runtime_name" "$runtime_manifest_sha256" "$RUNS" "$target_ms" "$trials" "$case_filter"

power_state="$(/usr/bin/pmset -g batt 2>/dev/null | /usr/bin/tr '\n' ' ' | sanitize_value)"
thermal_state="$(/usr/bin/pmset -g therm 2>/dev/null | /usr/bin/tr '\n' ' ' | sanitize_value)"
printf 'apple_cpu_environment schema=1 power=%s thermal=%s\n' \
    "${power_state:-unknown}" "${thermal_state:-unknown}"

for ((run = 1; run <= RUNS; ++run)); do
    output="$work/output-$run.txt"
    timing="$work/time-$run.txt"

    printf 'apple_cpu_run_begin schema=1 run=%d\n' "$run"
    set +e
    {
        /usr/bin/time -lp env \
            WINEPREFIX="$prefix" \
            WINEDEBUG=-all \
            WINEDLLOVERRIDES="winedbg.exe=d" \
            APPLE_CPU_BENCH_RUN="$run" \
            APPLE_CPU_BENCH_TARGET_MS="$target_ms" \
            APPLE_CPU_BENCH_TRIALS="$trials" \
            APPLE_CPU_BENCH_CASE="$case_filter" \
            "$RUNTIME/bin/switchyard-wine" "$benchmark" >"$output"
    } 2>"$timing"
    status=$?
    set -e

    cat "$output"
    cat "$timing" >&2
    [[ "$status" -eq 0 ]] || exit "$status"
    /usr/bin/grep -Fq "apple_cpu_complete schema=1 run=$run " "$output" || {
        echo "benchmark run $run did not publish a completion record" >&2
        exit 1
    }

    real_seconds="$(/usr/bin/awk '$1 == "real" { value=$2 } $2 == "real" { value=$1 } END { print value }' "$timing")"
    voluntary="$(/usr/bin/awk '$2 == "voluntary" && $3 == "context" { value=$1 } END { print value }' "$timing")"
    involuntary="$(/usr/bin/awk '$2 == "involuntary" && $3 == "context" { value=$1 } END { print value }' "$timing")"
    maximum_resident="$(/usr/bin/awk '$2 == "maximum" && $3 == "resident" { value=$1 } END { print value }' "$timing")"
    if [[ -n "$real_seconds" && -n "$voluntary" && -n "$involuntary" ]]; then
        /usr/bin/awk -v run="$run" -v real="$real_seconds" \
            -v voluntary="$voluntary" -v involuntary="$involuntary" \
            -v resident="${maximum_resident:-0}" \
            'BEGIN {
                printf "apple_cpu_process schema=1 run=%d real_s=%.6f voluntary_context_switches=%d involuntary_context_switches=%d context_switches_per_s=%.3f maximum_resident_bytes=%d\n",
                    run, real, voluntary, involuntary,
                    real > 0 ? (voluntary + involuntary) / real : 0, resident
            }'
    fi
    printf 'apple_cpu_run_end schema=1 run=%d status=ok\n' "$run"
done
