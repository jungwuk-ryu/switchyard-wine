#!/bin/bash
# Build pinned Unicorn and benchmark xtajit64 hot paths on native Apple Silicon.

set -euo pipefail

if [[ $# -gt 1 ]]; then
    echo "usage: $0 [WORK_DIR]" >&2
    exit 2
fi
if [[ $(/usr/bin/uname -s) != Darwin || $(/usr/bin/uname -m) != arm64 ]]; then
    echo "this benchmark requires a native Apple Silicon runner" >&2
    exit 2
fi
if translated=$(/usr/sbin/sysctl -in sysctl.proc_translated 2>/dev/null); then
    [[ $translated != 1 ]] || {
        echo "this benchmark must not run through Rosetta" >&2
        exit 2
    }
fi

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && /bin/pwd -P)
work_dir=${1:-}
cleanup=0
if [[ -z $work_dir ]]; then
    work_dir=$(mktemp -d "${TMPDIR:-/tmp}/xtajit64-hotpaths.XXXXXX")
    cleanup=1
else
    mkdir -p "$work_dir"
    work_dir=$(cd "$work_dir" && /bin/pwd -P)
fi
if [[ $cleanup -eq 1 ]]; then trap 'rm -rf -- "$work_dir"' EXIT; fi

unicorn_source="$work_dir/unicorn"
unicorn_build="$work_dir/unicorn-build"
benchmark="$work_dir/apple_silicon_hotpaths"
revision=8028ec436f2d9376525352dd38ed9ed6b9f6be10

/usr/bin/git init -q "$unicorn_source"
/usr/bin/git -C "$unicorn_source" remote add origin \
    https://github.com/unicorn-engine/unicorn.git
/usr/bin/git -C "$unicorn_source" fetch -q --depth=1 origin "$revision"
/usr/bin/git -C "$unicorn_source" checkout -q --detach FETCH_HEAD

cmake_bin=$(command -v cmake)
[[ -x $cmake_bin ]] || { echo "cmake is required" >&2; exit 2; }

ARCHFLAGS="-arch arm64" "$cmake_bin" -S "$unicorn_source" -B "$unicorn_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DUNICORN_ARCH=x86 \
    -DUNICORN_BUILD_TESTS=OFF \
    -DUNICORN_BUILD_SAMPLES=OFF \
    -DCMAKE_OSX_ARCHITECTURES=arm64
"$cmake_bin" --build "$unicorn_build" --parallel 3

library=$(find "$unicorn_build" -maxdepth 3 \
    \( -name 'libunicorn.dylib' -o -name 'libunicorn.2.dylib' \) \
    -type f -print -quit)
[[ -n $library ]] || {
    echo "pinned Unicorn dylib was not produced" >&2
    exit 1
}
library_dir=$(cd "$(dirname "$library")" && /bin/pwd -P)

/usr/bin/clang -O3 -DNDEBUG -Wall -Wextra -Werror \
    -Wno-cast-qual \
    -I"$unicorn_source/include" \
    "$source_dir/dlls/xtajit64/provider_tests/apple_silicon_hotpaths.c" \
    -L"$library_dir" -Wl,-rpath,"$library_dir" -lunicorn \
    -o "$benchmark"

/usr/bin/file "$benchmark"
/usr/bin/arch -arm64 "$benchmark"
