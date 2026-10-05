#!/bin/bash
# Run portable metadata checks and actual converters on ARM64 hosts.
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
compiler_line=${CC:-cc}
read -r -a compiler_words <<<"$compiler_line"
test_tmp=$(/usr/bin/mktemp -d /tmp/arm64ec-guest-flags.XXXXXX)
sanitizer_flags=()
if [[ ${SANITIZE:-0} == 1 ]]; then
    sanitizer_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

cleanup()
{
    case "$test_tmp" in
        /tmp/arm64ec-guest-flags.??????)
            if [[ -e "$test_tmp/arm64ec_guest_flags" || -L "$test_tmp/arm64ec_guest_flags" ]]; then
                /bin/rm -- "$test_tmp/arm64ec_guest_flags"
            fi
            /bin/rmdir -- "$test_tmp"
            ;;
        *) echo "refusing to remove unexpected test path: $test_tmp" >&2 ;;
    esac
}
trap cleanup EXIT HUP INT TERM

"${compiler_words[@]}" -std=c17 -O2 -fshort-wchar -D__WINESRC__ \
    -Wall -Wextra -Werror -isystem "$root_dir/include" \
    ${sanitizer_flags[@]+"${sanitizer_flags[@]}"} \
    "$root_dir/dlls/ntdll/tests/arm64ec_guest_flags.c" \
    -o "$test_tmp/arm64ec_guest_flags"
"$test_tmp/arm64ec_guest_flags"
