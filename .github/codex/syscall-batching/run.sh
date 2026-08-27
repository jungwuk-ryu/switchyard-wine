#!/bin/bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 APPLE_SILICON_HOTPATH_WORKSPACE" >&2
    exit 2
fi

workspace=$1
case $workspace in
    /*) ;;
    *) echo "workspace must be an absolute path" >&2; exit 2 ;;
esac
if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
    echo "syscall register benchmark requires native Apple Silicon" >&2
    exit 2
fi
if translated=$(/usr/sbin/sysctl -in sysctl.proc_translated 2>/dev/null); then
    if [[ $translated == 1 ]]; then
        echo "syscall register benchmark must not run through Rosetta" >&2
        exit 2
    fi
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
source_file="$script_dir/syscall_register_hotpath.c"
[[ -f $source_file && ! -L $source_file ]] || {
    echo "missing syscall benchmark source: $source_file" >&2
    exit 2
}
[[ -d $workspace && ! -L $workspace ]] || {
    echo "benchmark workspace is missing or unsafe: $workspace" >&2
    exit 2
}

header=$(find "$workspace" -path '*/include/unicorn/unicorn.h' -type f -print -quit)
library=$(find "$workspace" -name 'libunicorn.dylib' -type f -print -quit)
if [[ -z $header || -z $library ]]; then
    echo "workspace does not contain the Unicorn build produced by run_apple_silicon_hotpaths.sh" >&2
    exit 2
fi
include_dir=${header%/unicorn/unicorn.h}
library_dir=${library%/libunicorn.dylib}
binary="$workspace/syscall_register_hotpath"

/usr/bin/clang \
    -O3 -DNDEBUG -arch arm64 \
    -Wall -Wextra -Werror -Wconversion -Wshadow \
    -I"$include_dir" \
    "$source_file" "$library" \
    -o "$binary"

/usr/bin/file "$binary"
/usr/bin/file "$binary" | /usr/bin/grep -Eq 'Mach-O 64-bit executable arm64$'
DYLD_LIBRARY_PATH="$library_dir${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    "$binary"
