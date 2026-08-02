#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="$(/usr/bin/mktemp -d /tmp/switchyard-macos-edr.XXXXXX)"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.15}"

cleanup()
{
    /bin/rm -rf -- "$WORK_DIR"
}
trap cleanup EXIT HUP INT TERM

command -v xcrun >/dev/null 2>&1 || {
    echo "xcrun is required to build the native macOS EDR probe" >&2
    exit 1
}

xcrun --sdk macosx clang \
    -x objective-c \
    -std=gnu17 \
    -fno-objc-arc \
    -fblocks \
    -fobjc-exceptions \
    -mmacosx-version-min="$DEPLOYMENT_TARGET" \
    -Wall -Wextra -Werror \
    "$ROOT_DIR/switchyard/tests/macos_edr_probe.m" \
    -o "$WORK_DIR/macos-edr-probe" \
    -framework AppKit \
    -framework CoreGraphics \
    -framework Metal \
    -framework QuartzCore

if [ "$#" -eq 0 ]; then
    set -- --json
fi

"$WORK_DIR/macos-edr-probe" "$@"
