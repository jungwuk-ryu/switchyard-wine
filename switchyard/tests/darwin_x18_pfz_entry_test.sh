#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && /bin/pwd -P)"
if [ "$(/usr/bin/uname -s)" != Darwin ] || [ "$(/usr/bin/uname -m)" != arm64 ]; then
    echo "Darwin x18 PFZ entry test skipped: native macOS ARM64 required"
    exit 0
fi
TEST_ROOT="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/darwin-x18-pfz-entry.XXXXXX")"
trap '/bin/rm -rf -- "$TEST_ROOT"' EXIT
cmake -S "$ROOT_DIR/switchyard/tests/darwin_x18_pfz_entry" -B "$TEST_ROOT/build" \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_MAKE_PROGRAM=/usr/bin/make \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
if [ -f "$TEST_ROOT/build/pfz-entry-capability.skip" ]; then
    echo "Darwin x18 PFZ entry test skipped: custom-x18 SDK declarations required"
    exit 0
fi
cmake --build "$TEST_ROOT/build" --parallel 2
"$TEST_ROOT/build/darwin-x18-pfz-entry"
