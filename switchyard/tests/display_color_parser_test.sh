#!/bin/bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
WORK_DIR="$(/usr/bin/mktemp -d /tmp/switchyard-display-color.XXXXXX)"
CC_BIN="${CC:-clang}"

cleanup()
{
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

common_flags=(
  -std=gnu23
  -DMACDRV_NATIVE_TEST
  -D__WINESRC__
  -I"$WORK_DIR"
  -I"$ROOT_DIR/include"
  -I"$ROOT_DIR/dlls/winemac.drv"
  -I"$ROOT_DIR/dlls/winemac.drv/tests"
  -Wall
  -Wextra
  -Werror
)
source_file="$ROOT_DIR/dlls/winemac.drv/tests/display_color.c"

: > "$WORK_DIR/config.h"

"$CC_BIN" "${common_flags[@]}" "$source_file" -o "$WORK_DIR/display-color-test" -lm
"$WORK_DIR/display-color-test"

if "$CC_BIN" "${common_flags[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$source_file" -o "$WORK_DIR/display-color-test-san" -lm >/dev/null 2>&1; then
  "$WORK_DIR/display-color-test-san"
else
  echo "display color parser sanitizer build unavailable; deterministic tests passed" >&2
fi
