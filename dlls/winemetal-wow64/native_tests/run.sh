#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$test_dir/../../../" && pwd)
: "${WINE_BUILD_INCLUDE:?set WINE_BUILD_INCLUDE to the configured native ARM64 Wine include directory}"
schema_file="$source_dir/dlls/winemetal-wow64/abi-schema-v6.txt"
schema_sha=$(shasum -a 256 "$schema_file" | awk '{print $1}')

LC_ALL=C awk '/^[0-9][0-9][0-9] [FAD] / { if ($1 != sprintf("%03d", count)) exit 1; count++ }
             END { if (count != 138) exit 1 }' "$schema_file"
LC_ALL=C awk '/^[0-9][0-9][0-9] args-size=/ { if ($1 != sprintf("%03d", count)) exit 1; count++ }
             END { if (count != 138) exit 1 }' "$schema_file"

build_dir=$(mktemp -d "${TMPDIR:-/tmp}/winemetal-wow64-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

common_flags="-I$WINE_BUILD_INCLUDE -I$source_dir/include -I$source_dir/dlls/winemetal-wow64 -D__WINESRC__ -D_CRTIMP= -DWINE_UNIX_LIB -DWMT_NATIVE_TEST -Wall -Werror -Wdeclaration-after-statement -Wempty-body -Wignored-qualifiers -Winit-self -Wpointer-arith -Wstrict-prototypes -Wtype-limits -Wunused-but-set-parameter -Wvla -Wwrite-strings -fno-strict-aliasing -fno-stack-protector"

${CC:-clang} $common_flags \
    "$source_dir/dlls/winemetal-wow64/adapter.c" \
    "$source_dir/dlls/winemetal-wow64/commands.c" \
    "$source_dir/dlls/winemetal-wow64/unixlib.c" \
    "$test_dir/winemetal.c" \
    -o "$build_dir/winemetal-wow64-tests"

file "$build_dir/winemetal-wow64-tests" | grep -q 'Mach-O 64-bit executable arm64'
nm -m "$build_dir/winemetal-wow64-tests" | \
    grep '__wine_unix_call_wow64_funcs' | grep -q '(__DATA_CONST,__const)'
nm -m "$build_dir/winemetal-wow64-tests" | \
    grep '__wine_unix_call_wow64_companion_v6' | grep -q '(__DATA_CONST,__const)'
nm -m "$build_dir/winemetal-wow64-tests" | \
    grep '__wine_unix_call_wow64_dispatch_v2' | grep -q '(__DATA_CONST,__const)'
if nm "$build_dir/winemetal-wow64-tests" | \
    grep -E '__wine_unix_call_wow64_companion_v[123]|__wine_unix_call_native|__wine_unix_call_wow64_direct'; then
    exit 1
fi

"$build_dir/winemetal-wow64-tests" zero "$schema_sha"
"$build_dir/winemetal-wow64-tests" high "$schema_sha"

${CC:-clang} $common_flags -fblocks \
    "$source_dir/dlls/winemetal-wow64/buffer.m" \
    "$test_dir/buffer.m" \
    -framework Foundation -framework Metal \
    -o "$build_dir/winemetal-wow64-buffer-tests"

file "$build_dir/winemetal-wow64-buffer-tests" | grep -q 'Mach-O 64-bit executable arm64'
"$build_dir/winemetal-wow64-buffer-tests"
