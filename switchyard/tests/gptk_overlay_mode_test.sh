#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/switchyard/build_runtime.sh"

source_info="$(SWITCHYARD_DISABLE_GPTK_OVERLAY=1 GPTK_PATH=/path/that/must/be/ignored \
  "$BUILD_SCRIPT" --source-info)"
if ! grep -Fx 'gptkOverlayDisabled=1' <<<"$source_info" >/dev/null; then
  echo "source info did not report the explicitly disabled GPTK overlay" >&2
  exit 1
fi

error_file="$(mktemp)"
trap 'rm -f "$error_file"' EXIT
if SWITCHYARD_DISABLE_GPTK_OVERLAY=invalid "$BUILD_SCRIPT" --source-info 2>"$error_file"; then
  echo "invalid GPTK overlay mode unexpectedly succeeded" >&2
  exit 1
fi
if ! grep -F 'SWITCHYARD_DISABLE_GPTK_OVERLAY must be 0 or 1.' "$error_file" >/dev/null; then
  echo "invalid GPTK overlay mode did not report the expected error" >&2
  exit 1
fi

echo "GPTK overlay mode tests passed"
