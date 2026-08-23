#!/bin/bash
# Compile and execute the production ARM64EC fixed-low decoder as native C.

set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
compiler_line=${CC:-cc}
read -r -a compiler_words <<<"$compiler_line"
test_tmp=$(/usr/bin/mktemp -d /tmp/arm64ec-low-guest-decode.XXXXXX)
sanitizer_flags=()

if [[ ${SANITIZE:-0} == 1 ]]; then
    sanitizer_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

cleanup()
{
    case "$test_tmp" in
        /tmp/arm64ec-low-guest-decode.??????)
            if [[ -e "$test_tmp/arm64ec_low_guest_decode" ||
                  -L "$test_tmp/arm64ec_low_guest_decode" ]]; then
                /bin/rm -- "$test_tmp/arm64ec_low_guest_decode"
            fi
            /bin/rmdir -- "$test_tmp"
            ;;
        *)
            echo "refusing to remove unexpected test path: $test_tmp" >&2
            ;;
    esac
}
trap cleanup EXIT HUP INT TERM

"${compiler_words[@]}" -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
    -Wconversion -Wsign-conversion \
    ${sanitizer_flags[@]+"${sanitizer_flags[@]}"} \
    -I"$root_dir/dlls/ntdll/unix" \
    "$root_dir/dlls/ntdll/tests/arm64ec_low_guest_decode.c" \
    -o "$test_tmp/arm64ec_low_guest_decode"

"$test_tmp/arm64ec_low_guest_decode"
