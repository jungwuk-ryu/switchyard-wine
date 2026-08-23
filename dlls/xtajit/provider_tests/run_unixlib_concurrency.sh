#!/bin/bash
# Build and run the native xtajit i386 observer/concurrency regression.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 WINE_BUILD_DIR" >&2
    exit 2
fi

build_dir=$(cd "$1" && pwd)
makefile="$build_dir/Makefile"
if [[ ! -f "$makefile" || ! -f "$build_dir/include/config.h" ]]; then
    echo "not a configured Wine build directory: $build_dir" >&2
    exit 2
fi

source_dir=$(sed -n 's/^srcdir = //p' "$makefile" | head -n 1)
cc_line=${CC:-$(sed -n 's/^CC = //p' "$makefile" | head -n 1)}
cflags_line=$(sed -n 's/^CFLAGS = //p' "$makefile" | head -n 1)
unicorn_cflags=$(sed -n 's/^UNICORN_CFLAGS = //p' "$makefile" | head -n 1)
unicorn_libs=$(sed -n 's/^UNICORN_LIBS = //p' "$makefile" | head -n 1)
if [[ -z "$source_dir" || -z "$cc_line" || -z "$unicorn_libs" ]]; then
    echo "configured build is missing source, compiler, or Unicorn library settings" >&2
    exit 2
fi
if [[ "$source_dir" != /* ]]; then
    source_dir=$(cd "$build_dir/$source_dir" && pwd)
else
    source_dir=$(cd "$source_dir" && pwd)
fi
if [[ -n ${XTAJIT_EXPECTED_SOURCE_DIR:-} ]]; then
    expected_source_dir=$(cd "$XTAJIT_EXPECTED_SOURCE_DIR" && pwd)
    if [[ "$source_dir" != "$expected_source_dir" ]]; then
        echo "configured build source $source_dir does not match $expected_source_dir" >&2
        exit 2
    fi
fi

read -r -a cc_words <<<"$cc_line"
read -r -a cflag_words <<<"$cflags_line"
read -r -a unicorn_cflag_words <<<"$unicorn_cflags"
read -r -a unicorn_lib_words <<<"$unicorn_libs"
unicorn_lib_dir=
for word in "${unicorn_lib_words[@]}"; do
    if [[ "$word" == -L* ]]; then unicorn_lib_dir=${word#-L}; fi
done

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/xtajit-concurrency.XXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT
test_binary="$tmp_dir/unixlib_concurrency"
context_binary="$tmp_dir/context_generation"
performance_binary="$tmp_dir/hook_performance"
sanitizer_flags=()
if [[ ${SANITIZE:-0} == 1 && ${THREAD_SANITIZE:-0} == 1 ]]; then
    echo "SANITIZE and THREAD_SANITIZE are mutually exclusive" >&2
    exit 2
elif [[ ${SANITIZE:-0} == 1 ]]; then
    sanitizer_flags=("-fsanitize=address,undefined" -fno-omit-frame-pointer)
elif [[ ${THREAD_SANITIZE:-0} == 1 ]]; then
    sanitizer_flags=("-fsanitize=thread" -fno-omit-frame-pointer)
fi
iterations=${ITERATIONS:-1}
if [[ ! $iterations =~ ^[1-9][0-9]*$ || $iterations -gt 1000 ]]; then
    echo "ITERATIONS must be an integer from 1 through 1000" >&2
    exit 2
fi

"${cc_words[@]}" \
    ${cflag_words[@]+"${cflag_words[@]}"} \
    -o "$context_binary" \
    "$source_dir/dlls/xtajit/provider_tests/context_generation.c" \
    -I"$source_dir/dlls/xtajit" \
    -Wall -Werror -Wdeclaration-after-statement -Wempty-body \
    -Wignored-qualifiers -Winit-self -Wpointer-arith -Wstrict-prototypes \
    -Wtype-limits -Wunused-but-set-parameter -Wvla -Wwrite-strings \
    ${sanitizer_flags[@]+"${sanitizer_flags[@]}"}

"${cc_words[@]}" \
    ${cflag_words[@]+"${cflag_words[@]}"} \
    -o "$test_binary" \
    "$source_dir/dlls/xtajit/provider_tests/unixlib_concurrency.c" \
    -I"$build_dir/dlls/xtajit" \
    -I"$source_dir/dlls/xtajit" \
    -I"$build_dir/include" \
    -I"$source_dir/include" \
    -D__WINESRC__ -D_CRTIMP= -DHAVE_UNICORN -DWINE_UNIX_LIB \
    -DXTAJIT_UNIXLIB_TEST \
    -Wall -Werror -Wdeclaration-after-statement -Wempty-body \
    -Wignored-qualifiers -Winit-self -Wpointer-arith -Wstrict-prototypes \
    -Wtype-limits -Wunused-but-set-parameter -Wvla -Wwrite-strings \
    -fno-strict-aliasing -fno-stack-protector \
    ${sanitizer_flags[@]+"${sanitizer_flags[@]}"} \
    ${unicorn_cflag_words[@]+"${unicorn_cflag_words[@]}"} \
    "$build_dir/dlls/ntdll/ntdll.so" \
    "${unicorn_lib_words[@]}"

"${cc_words[@]}" \
    ${cflag_words[@]+"${cflag_words[@]}"} \
    -o "$performance_binary" \
    "$source_dir/dlls/xtajit/provider_tests/hook_performance.c" \
    -Wall -Werror -Wdeclaration-after-statement -Wempty-body \
    -Wignored-qualifiers -Winit-self -Wpointer-arith -Wstrict-prototypes \
    -Wtype-limits -Wunused-but-set-parameter -Wvla -Wwrite-strings \
    ${unicorn_cflag_words[@]+"${unicorn_cflag_words[@]}"} \
    "${unicorn_lib_words[@]}"

for ((iteration = 1; iteration <= iterations; ++iteration)); do
    "$context_binary"
    DYLD_LIBRARY_PATH="$build_dir/dlls/ntdll${unicorn_lib_dir:+:$unicorn_lib_dir}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$test_binary"
done

if [[ ${XTAJIT_RUN_PERF:-0} == 1 ]]; then
    DYLD_LIBRARY_PATH="${unicorn_lib_dir}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$performance_binary"
fi
