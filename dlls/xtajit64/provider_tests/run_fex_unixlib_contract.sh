#!/bin/bash
# Build and run the native xtajit64/FEX Unix-provider contract regression.

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
fex_cflags=$(sed -n 's/^SWITCHYARD_FEX_CFLAGS = //p' "$makefile" | head -n 1)
fex_libs=$(sed -n 's/^SWITCHYARD_FEX_LIBS = //p' "$makefile" | head -n 1)
if [[ -z "$source_dir" || -z "$cc_line" || -z "$fex_cflags" ||
      -z "$fex_libs" ]]; then
    echo "configured build is missing source, compiler, or FEX settings" >&2
    exit 2
fi
if [[ "$source_dir" != /* ]]; then
    source_dir=$(cd "$build_dir/$source_dir" && pwd)
else
    source_dir=$(cd "$source_dir" && pwd)
fi
if [[ -n ${XTAJIT64_EXPECTED_SOURCE_DIR:-} ]]; then
    expected_source_dir=$(cd "$XTAJIT64_EXPECTED_SOURCE_DIR" && pwd)
    if [[ "$source_dir" != "$expected_source_dir" ]]; then
        echo "configured build source $source_dir does not match $expected_source_dir" >&2
        exit 2
    fi
fi
if [[ -n ${XTAJIT64_FEX_ROOT:-} ]]; then
    if [[ $XTAJIT64_FEX_ROOT != /* || ! -d $XTAJIT64_FEX_ROOT ||
          -L $XTAJIT64_FEX_ROOT ]]; then
        echo "XTAJIT64_FEX_ROOT must name an absolute, real directory" >&2
        exit 2
    fi
    fex_root=$(cd "$XTAJIT64_FEX_ROOT" && pwd -P)
    if [[ ! -f $fex_root/include/switchyard_fex.h ||
          ! -f $fex_root/lib/libswitchyard-fex.6.dylib ]]; then
        echo "XTAJIT64_FEX_ROOT is missing the development header or dylib" >&2
        exit 2
    fi
    fex_cflags="-I$fex_root/include"
    fex_libs="-L$fex_root/lib -lswitchyard-fex"
fi

read -r -a cc_words <<<"$cc_line"
read -r -a cflag_words <<<"$cflags_line"
read -r -a fex_cflag_words <<<"$fex_cflags"
read -r -a fex_lib_words <<<"$fex_libs"
native_provider="$build_dir/dlls/xtajit64/xtajit64.so"
pe_provider="$build_dir/dlls/xtajit64/aarch64-windows/xtajit64.dll"
if [[ ! -f $native_provider || ! -f $pe_provider ]]; then
    echo "xtajit64 native or PE provider has not been built" >&2
    exit 2
fi

native_imports=$(/usr/bin/nm -u "$native_provider") || exit 1
for symbol in switchyard_fex_abi_version \
              switchyard_fex_process_create \
              switchyard_fex_process_set_executable_range_query \
              switchyard_fex_process_destroy \
              switchyard_fex_process_invalidate_code \
              switchyard_fex_provider_abi_identity \
              switchyard_fex_result_string \
              switchyard_fex_thread_create_with_domain \
              switchyard_fex_thread_destroy \
              switchyard_fex_thread_prepare_execution \
              switchyard_fex_thread_execute_prepared \
              switchyard_fex_thread_prepare_dispatch \
              switchyard_fex_thread_complete_dispatch \
              switchyard_fex_thread_export_state \
              switchyard_fex_thread_import_state \
              switchyard_fex_thread_import_register_window \
              switchyard_fex_thread_export_register_window \
              switchyard_fex_thread_reconstruct_jit_fault \
              switchyard_fex_thread_query_jit_stack \
              switchyard_fex_thread_repair_callret_fault \
              switchyard_fex_thread_repair_unaligned_tso \
              switchyard_fex_upstream_revision; do
    if ! grep -Eq "(^|[[:space:]])_?${symbol}$" <<<"$native_imports"; then
        echo "production xtajit64 Unixlib is missing FEX symbol $symbol" >&2
        exit 1
    fi
done

if grep -Eq '(^|[[:space:]])_?uc_[[:alnum:]_]+$' <<<"$native_imports"; then
    echo "production xtajit64 Unixlib still imports Unicorn" >&2
    exit 1
fi
if [[ $(uname -s) == Darwin ]]; then
    native_libraries=$(/usr/bin/otool -L "$native_provider") || exit 1
    if ! grep -Eq '/libswitchyard-fex\.6\.dylib([[:space:]]|$)' \
            <<<"$native_libraries"; then
        echo "production xtajit64 Unixlib is not linked to FEX ABI 6" >&2
        exit 1
    fi
    if grep -Eq '/libunicorn\.[0-9]+\.dylib([[:space:]]|$)' \
            <<<"$native_libraries"; then
        echo "production xtajit64 Unixlib still links to Unicorn" >&2
        exit 1
    fi
fi
native_strings=$(/usr/bin/strings "$native_provider") || exit 1
pe_strings=$(/usr/bin/strings "$pe_provider") || exit 1
if ! grep -Fxq 'switchyard-fex-provider-abi-v6-darwin-low-shadow-external-stops-signal-repair-stack-query-exec-range-detached-shared-admission-register-window' \
        <<<"$native_strings"; then
    echo "production xtajit64 Unixlib is missing the exact FEX ABI identity" >&2
    exit 1
fi
provider_abi_identity='switchyard-xtajit64-fex-provider-abi-v19-flight-bind-process-init-104-begin-472-doorbell-reconstruct-1248-jit-signal-stack-query-exec-range-dispatch-512-direct-64-native-stack-protect'
if ! grep -Fxq "$provider_abi_identity" <<<"$native_strings"; then
    echo "production xtajit64 Unixlib is missing the exact provider ABI identity" >&2
    exit 1
fi
if ! grep -Fxq "$provider_abi_identity" \
        <<<"$pe_strings"; then
    echo "production xtajit64 PE image is missing the exact provider ABI identity" >&2
    exit 1
fi
echo "xtajit64 production native and PE provider linkage verified"

fex_lib_dir=
for word in "${fex_lib_words[@]}"; do
    if [[ "$word" == -L* ]]; then fex_lib_dir=${word#-L}; fi
done
if [[ -z $fex_lib_dir || ! -f $fex_lib_dir/libswitchyard-fex.6.dylib ]]; then
    echo "configured FEX library directory is invalid" >&2
    exit 2
fi

if [[ -z ${MACOSX_DEPLOYMENT_TARGET:-} && $(uname -s) == Darwin ]]; then
    deployment_target=$(/usr/bin/otool -l "$build_dir/dlls/ntdll/ntdll.so" |
        awk '$1 == "minos" { print $2; exit }')
    if [[ -n $deployment_target ]]; then export MACOSX_DEPLOYMENT_TARGET=$deployment_target; fi
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/xtajit64-fex-contract.XXXXXX")
tmp_dir=$(cd "$tmp_dir" && pwd -P)
source "$source_dir/switchyard/lib/macho_signing.sh"
test_entitlements_fd=
trap 'if [[ -n ${test_entitlements_fd:-} ]]; then close_validated_entitlements_snapshot "$test_entitlements_fd"; fi; /usr/bin/find "$tmp_dir" -depth -delete' EXIT
test_binary="$tmp_dir/fex_unixlib_contract"
sanitizer_flags=()
sanitize_mode=${SANITIZE:-0}
case $sanitize_mode in
0)
    ;;
undefined)
    sanitizer_flags=("-fsanitize=undefined" -fno-omit-frame-pointer)
    ;;
*)
    echo "SANITIZE must be 0 or undefined" >&2
    exit 2
    ;;
esac
iterations=${ITERATIONS:-1}
if [[ ! $iterations =~ ^[1-9][0-9]*$ || $iterations -gt 1000 ]]; then
    echo "ITERATIONS must be an integer from 1 through 1000" >&2
    exit 2
fi

"${cc_words[@]}" \
    ${cflag_words[@]+"${cflag_words[@]}"} \
    -o "$test_binary" \
    "$source_dir/dlls/xtajit64/provider_tests/fex_unixlib_contract.c" \
    -I"$build_dir/dlls/xtajit64" \
    -I"$source_dir/dlls/xtajit64" \
    -I"$build_dir/include" \
    -I"$source_dir/include" \
    -D__WINESRC__ -D_CRTIMP= -DHAVE_SWITCHYARD_FEX -DWINE_UNIX_LIB \
    -DXTAJIT64_FEX_UNIXLIB_TEST \
    -Wall -Werror -Wdeclaration-after-statement -Wempty-body \
    -Wignored-qualifiers -Winit-self -Wpointer-arith -Wstrict-prototypes \
    -Wtype-limits -Wunused-but-set-parameter -Wvla -Wwrite-strings \
    -fno-strict-aliasing -fno-stack-protector \
    ${sanitizer_flags[@]+"${sanitizer_flags[@]}"} \
    ${fex_cflag_words[@]+"${fex_cflag_words[@]}"} \
    "$build_dir/dlls/ntdll/ntdll.so" \
    "${fex_lib_words[@]}"

# A short successful SDK toggle does not establish preservation across kernel
# preemption. The standalone custom-mode caller needs the SAME process-entry
# entitlement as Wine; use the existing snapshot-owned signing policy.
/usr/bin/codesign -dvvv --entitlements :- "$test_binary"
create_validated_entitlements_snapshot preview-native-arm64-fex \
    "$source_dir/switchyard/wine-runtime-native-arm64.entitlements" \
    "$tmp_dir" test_entitlements_fd
sign_engineering_macho_atomically /usr/bin/codesign preview-native-arm64-fex \
    "$test_binary" "$test_entitlements_fd"
verify_macho_entitlements_snapshot /usr/bin/codesign preview-native-arm64-fex \
    "$test_binary" "$test_entitlements_fd"
/usr/bin/shasum -a 256 "$test_binary" "$native_provider" "$pe_provider" \
    "$build_dir/dlls/ntdll/ntdll.so" "$fex_lib_dir/libswitchyard-fex.6.dylib"

for ((iteration = 1; iteration <= iterations; ++iteration)); do
    DYLD_LIBRARY_PATH="$build_dir/dlls/ntdll:$fex_lib_dir${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$test_binary"
done

# The real continuation must reject this test-only partial-state mutant. A
# positive-only gate would miss accidental use of native defaults at SYSCALL.
mutant_status=0
DYLD_LIBRARY_PATH="$build_dir/dlls/ntdll:$fex_lib_dir${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    XTAJIT64_FEX_TEST_WINDOW_SYSCALL_MUTANT=1 "$test_binary" \
    >"$tmp_dir/window-syscall-mutant.log" 2>&1 || mutant_status=$?
[[ $mutant_status -eq 1 ]] &&
    grep -Fxq 'failure: syscall lost full x87/segment/YMM state' "$tmp_dir/window-syscall-mutant.log" || {
    echo "full-state syscall mutant did not fail at the required invariant" >&2
    exit 1
}
