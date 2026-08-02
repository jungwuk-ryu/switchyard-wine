#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$(/usr/bin/mktemp -d /tmp/switchyard-frame-scheduler-test.XXXXXX)"
# shellcheck disable=SC2329 # Invoked through the EXIT trap.
cleanup() { /bin/rm -rf "$work"; }
trap cleanup EXIT

common=(
  -std=gnu23 -O2 -g -Wall -Wextra -Werror -Wconversion
  -DMACDRV_FRAME_SCHEDULER_STANDALONE
  -I"$ROOT_DIR/dlls/winemac.drv"
  "$ROOT_DIR/dlls/winemac.drv/frame_scheduler.c"
  "$ROOT_DIR/switchyard/tests/macos_frame_scheduler_test.c"
  -framework CoreGraphics
)

clang "${common[@]}" -fsanitize=address,undefined \
  -fno-omit-frame-pointer -o "$work/frame-scheduler-test"
ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$work/frame-scheduler-test"

for _ in $(seq 1 20); do "$work/frame-scheduler-test" >/dev/null; done
echo "macOS frame scheduler sanitizer/stress soak passed (20 repetitions)."

if [ "${SWITCHYARD_RUN_TSAN:-0}" = 1 ]; then
  clang "${common[@]}" -fsanitize=thread -fno-omit-frame-pointer \
    -o "$work/frame-scheduler-tsan"
  TSAN_OPTIONS=halt_on_error=1 "$work/frame-scheduler-tsan"
fi
