#!/bin/bash
# Run actual pure FP converters; SANITIZE=1 adds ASan/UBSan.
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
compiler_line=${CC:-cc}
read -r -a compiler_words <<<"$compiler_line"
test_tmp=$(/usr/bin/mktemp -d /tmp/arm64ec-fp-mapping.XXXXXX)
sanitizer_flags=()
if [[ ${SANITIZE:-0} == 1 ]]; then
    sanitizer_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

cleanup()
{
    case "$test_tmp" in
        /tmp/arm64ec-fp-mapping.??????)
            if [[ -e "$test_tmp/arm64ec_fp_mapping" || -L "$test_tmp/arm64ec_fp_mapping" ]]; then
                /bin/rm -- "$test_tmp/arm64ec_fp_mapping"
            fi
            /bin/rmdir -- "$test_tmp"
            ;;
        *) echo "refusing to remove unexpected test path: $test_tmp" >&2 ;;
    esac
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

"${compiler_words[@]}" -std=c17 -O2 -fshort-wchar -D__WINESRC__ \
    -Wall -Wextra -Werror -isystem "$root_dir/include" \
    ${sanitizer_flags[@]+"${sanitizer_flags[@]}"} \
    "$root_dir/dlls/ntdll/tests/arm64ec_fp_mapping.c" \
    -o "$test_tmp/arm64ec_fp_mapping"
"$test_tmp/arm64ec_fp_mapping"
