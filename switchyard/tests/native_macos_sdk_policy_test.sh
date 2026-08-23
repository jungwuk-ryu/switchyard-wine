#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
TEST_ROOT="$(/usr/bin/mktemp -d /tmp/switchyard-native-sdk-policy.XXXXXX)"
failure_index=0

cleanup()
{
    case "${TEST_ROOT:-}" in
        /tmp/switchyard-native-sdk-policy.??????)
            [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
            ;;
        *) echo "refusing to remove unexpected SDK policy test root ${TEST_ROOT:-<empty>}" >&2 ;;
    esac
}
trap cleanup EXIT

fail()
{
    echo "Native macOS SDK policy test: $*" >&2
    exit 1
}

expect_failure()
{
    local label="$1"
    local status
    local stdout_file
    local stderr_file
    shift

    failure_index=$((failure_index + 1))
    stdout_file="$TEST_ROOT/failure-$failure_index.stdout"
    stderr_file="$TEST_ROOT/failure-$failure_index.stderr"
    set +e
    "$@" >"$stdout_file" 2>"$stderr_file"
    status=$?
    set -e
    [ "$status" -ne 0 ] || fail "$label was accepted"
    [ -s "$stderr_file" ] || fail "$label failed without a diagnostic"
}

expect_policy_rejection()
{
    local label="$1"
    shift

    if "$@"; then
        fail "$label was accepted"
    fi
}

write_policy_makefile()
{
    [ "$#" -eq 9 ] || return 2

    local destination="$1"
    local cc="$2"
    local cxx="$3"
    local objc="$4"
    local cflags="$5"
    local cxxflags="$6"
    local objcflags="$7"
    local ldflags="$8"
    local cppflags="$9"

    cat >"$destination" <<EOF
CC = $cc
CXX = $cxx
OBJC = $objc
CFLAGS = $cflags
CXXFLAGS = $cxxflags
OBJCFLAGS = $objcflags
LDFLAGS = $ldflags
CPPFLAGS = $cppflags
EOF
}

# shellcheck disable=SC1090 # The test resolves the worktree root at runtime.
source "$PROFILE_LIBRARY"

/bin/chmod 0700 "$TEST_ROOT"
fake_sdk="$TEST_ROOT/MacOSX26.5.sdk"
fake_sdk_link="$TEST_ROOT/current-macosx.sdk"
fake_xcrun="$TEST_ROOT/xcrun"
xcrun_path_file="$TEST_ROOT/xcrun.path"
xcrun_version_file="$TEST_ROOT/xcrun.version"
xcrun_build_file="$TEST_ROOT/xcrun.build"
xcrun_log="$TEST_ROOT/xcrun.log"

/bin/mkdir -m 0700 "$fake_sdk"
cat >"$fake_sdk/SDKSettings.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CanonicalName</key>
    <string>macosx26.5</string>
    <key>DefaultProperties</key>
    <dict>
        <key>MACOSX_DEPLOYMENT_TARGET</key>
        <string>26.5</string>
    </dict>
    <key>Version</key>
    <string>26.5</string>
</dict>
</plist>
EOF
/bin/chmod 0600 "$fake_sdk/SDKSettings.plist"
/bin/ln -s "$fake_sdk" "$fake_sdk_link"
/usr/bin/printf '%s\n' "$fake_sdk_link" >"$xcrun_path_file"
/usr/bin/printf '%s\n' '26.5' >"$xcrun_version_file"
/usr/bin/printf '%s\n' '25F70' >"$xcrun_build_file"

cat >"$fake_xcrun" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

{
    /usr/bin/printf '%s' "$1"
    shift
    /usr/bin/printf ' %s' "$@"
    /usr/bin/printf '\n'
} >>"$SWITCHYARD_TEST_XCRUN_LOG"

case "$*" in
    'macosx --show-sdk-path')
        /bin/cat "$SWITCHYARD_TEST_XCRUN_PATH_FILE"
        ;;
    'macosx --show-sdk-version')
        /bin/cat "$SWITCHYARD_TEST_XCRUN_VERSION_FILE"
        ;;
    'macosx --show-sdk-build-version')
        /bin/cat "$SWITCHYARD_TEST_XCRUN_BUILD_FILE"
        ;;
    *)
        echo "unexpected fake xcrun invocation: --sdk $*" >&2
        exit 91
        ;;
esac
EOF
/bin/chmod 0700 "$fake_xcrun"
export SWITCHYARD_TEST_XCRUN_PATH_FILE="$xcrun_path_file"
export SWITCHYARD_TEST_XCRUN_VERSION_FILE="$xcrun_version_file"
export SWITCHYARD_TEST_XCRUN_BUILD_FILE="$xcrun_build_file"
export SWITCHYARD_TEST_XCRUN_LOG="$xcrun_log"

physical_sdk="$(cd "$fake_sdk" && /bin/pwd -P)"
settings_sha256="$(/usr/bin/shasum -a 256 "$fake_sdk/SDKSettings.plist" |
    /usr/bin/awk '{print $1}')"
expected_identity="$(
    /usr/bin/printf '%s\0%s\0%s\0' '26.5' '25F70' "$settings_sha256" |
        /usr/bin/shasum -a 256 | /usr/bin/awk '{print $1}'
)"
switchyard_qualify_native_macos_sdk 26.5 "$fake_xcrun"
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT" = "$physical_sdk" ] ||
    fail "SDKROOT was not canonicalized through the reported symbolic link"
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_VERSION" = '26.5' ] ||
    fail "qualified SDK version is not exact"
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_BUILD_VERSION" = '25F70' ] ||
    fail "qualified SDK build version is not exact"
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_SETTINGS_SHA256" = "$settings_sha256" ] ||
    fail "qualified SDKSettings.plist identity is not exact"
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG" = "-isysroot$physical_sdk" ] ||
    fail "qualified SDK flag is not the exact single-token policy"
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_IDENTITY" = "$expected_identity" ] ||
    fail "qualified SDK identity does not bind the exact version, build, and settings"
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_XCRUN" = "$fake_xcrun" ] ||
    fail "qualified SDK policy did not retain its resolver"
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_EXPECTED_VERSION" = '26.5' ] ||
    fail "qualified SDK policy did not retain its expected version"

cat >"$TEST_ROOT/xcrun.expected" <<'EOF'
--sdk macosx --show-sdk-path
--sdk macosx --show-sdk-version
--sdk macosx --show-sdk-build-version
EOF
/usr/bin/cmp "$TEST_ROOT/xcrun.expected" "$xcrun_log" ||
    fail "SDK qualification used an unexpected xcrun probe contract"

qualified_identity="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_IDENTITY"
switchyard_validate_qualified_native_macos_sdk
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_IDENTITY" = "$qualified_identity" ] ||
    fail "unchanged SDK identity was not stable during revalidation"

expect_failure "wrong SDK version" \
    switchyard_qualify_native_macos_sdk 26.4 "$fake_xcrun"

/bin/mv "$fake_sdk/SDKSettings.plist" "$TEST_ROOT/SDKSettings.plist.saved"
expect_failure "missing SDKSettings.plist" \
    switchyard_qualify_native_macos_sdk 26.5 "$fake_xcrun"
/bin/mv "$TEST_ROOT/SDKSettings.plist.saved" "$fake_sdk/SDKSettings.plist"

whitespace_sdk="$TEST_ROOT/SDK with whitespace"
/bin/mkdir -m 0700 "$whitespace_sdk"
/bin/cp "$fake_sdk/SDKSettings.plist" "$whitespace_sdk/SDKSettings.plist"
/bin/chmod 0600 "$whitespace_sdk/SDKSettings.plist"
/usr/bin/printf '%s\n' "$whitespace_sdk" >"$xcrun_path_file"
expect_failure "SDK path containing whitespace" \
    switchyard_qualify_native_macos_sdk 26.5 "$fake_xcrun"
/usr/bin/printf '%s\n' "$fake_sdk_link" >"$xcrun_path_file"

switchyard_qualify_native_macos_sdk 26.5 "$fake_xcrun"
/usr/bin/printf '\n<!-- mutation after qualification -->\n' >> \
    "$fake_sdk/SDKSettings.plist"
expect_failure "SDKSettings.plist mutation after qualification" \
    switchyard_validate_qualified_native_macos_sdk

qualified_cc="$TEST_ROOT/llvm/bin/clang"
qualified_cxx="$TEST_ROOT/llvm/bin/clang++"
qualified_cc_assignment="$qualified_cc --no-default-config -arch arm64"
qualified_cxx_assignment="$qualified_cxx --no-default-config -arch arm64"
deployment_flag='-mmacosx-version-min=26.5'
sdk_flag="-isysroot$physical_sdk"
qualified_flags="$deployment_flag $sdk_flag"
valid_makefile="$TEST_ROOT/valid-Makefile"
write_policy_makefile "$valid_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" "$qualified_flags" "$qualified_flags" \
    "$qualified_flags" "$qualified_flags" ''
switchyard_native_configured_host_target_policy_is_exact \
    "$valid_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx" ||
    fail "exact configured host target policy was rejected"

missing_sdk_makefile="$TEST_ROOT/missing-sdk-Makefile"
write_policy_makefile "$missing_sdk_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" "$deployment_flag" "$qualified_flags" \
    "$qualified_flags" "$qualified_flags" ''
expect_policy_rejection "missing SDK flag" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$missing_sdk_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

duplicate_sdk_makefile="$TEST_ROOT/duplicate-sdk-Makefile"
write_policy_makefile "$duplicate_sdk_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" "$qualified_flags $sdk_flag" "$qualified_flags" \
    "$qualified_flags" "$qualified_flags" ''
expect_policy_rejection "duplicate SDK flag" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$duplicate_sdk_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

conflicting_sdk_makefile="$TEST_ROOT/conflicting-sdk-Makefile"
write_policy_makefile "$conflicting_sdk_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" \
    "$qualified_flags -isysroot/untrusted/MacOSX.sdk" "$qualified_flags" \
    "$qualified_flags" "$qualified_flags" ''
expect_policy_rejection "conflicting SDK flag" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$conflicting_sdk_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

missing_deployment_makefile="$TEST_ROOT/missing-deployment-Makefile"
write_policy_makefile "$missing_deployment_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" "$qualified_flags" "$qualified_flags" \
    "$qualified_flags" "$sdk_flag" ''
expect_policy_rejection "missing deployment flag" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$missing_deployment_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

duplicate_deployment_makefile="$TEST_ROOT/duplicate-deployment-Makefile"
write_policy_makefile "$duplicate_deployment_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" "$qualified_flags" "$qualified_flags" \
    "$qualified_flags $deployment_flag" "$qualified_flags" ''
expect_policy_rejection "duplicate deployment flag" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$duplicate_deployment_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

conflicting_deployment_makefile="$TEST_ROOT/conflicting-deployment-Makefile"
write_policy_makefile "$conflicting_deployment_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" "$qualified_flags" "$qualified_flags" \
    "$qualified_flags" \
    "$qualified_flags -mmacosx-version-min=14.0" ''
expect_policy_rejection "conflicting deployment flag" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$conflicting_deployment_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

conflicting_target_makefile="$TEST_ROOT/conflicting-target-Makefile"
write_policy_makefile "$conflicting_target_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" "$qualified_flags" \
    "$qualified_flags -target x86_64-apple-macos14.0" \
    "$qualified_flags" "$qualified_flags" ''
expect_policy_rejection "conflicting host target flag" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$conflicting_target_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

cppflags_makefile="$TEST_ROOT/cppflags-sdk-Makefile"
write_policy_makefile "$cppflags_makefile" \
    "$qualified_cc_assignment" "$qualified_cxx_assignment" \
    "$qualified_cc_assignment" "$qualified_flags" "$qualified_flags" \
    "$qualified_flags" "$qualified_flags" "$sdk_flag"
expect_policy_rejection "SDK flag in CPPFLAGS" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$cppflags_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

wrong_compiler_makefile="$TEST_ROOT/wrong-compiler-Makefile"
write_policy_makefile "$wrong_compiler_makefile" \
    "/untrusted/clang --no-default-config -arch arm64" \
    "$qualified_cxx_assignment" "$qualified_cc_assignment" \
    "$qualified_flags" "$qualified_flags" "$qualified_flags" \
    "$qualified_flags" ''
expect_policy_rejection "wrong configured compiler" \
    switchyard_native_configured_host_target_policy_is_exact \
    "$wrong_compiler_makefile" "$deployment_flag" "$sdk_flag" \
    "$qualified_cc" "$qualified_cxx"

if [ "$(/usr/bin/uname -s)" != Darwin ] ||
   [ "$(/usr/bin/uname -m)" != arm64 ]; then
    echo "Native macOS SDK integration skipped outside native macOS ARM64"
    echo "Native macOS SDK qualification policy verified"
    exit 0
fi

switchyard_qualify_native_macos_sdk 26.5 /usr/bin/xcrun
[ "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_VERSION" = '26.5' ] ||
    fail "the active Xcode SDK is not the required 26.5 SDK"
actual_clang=/opt/homebrew/opt/llvm/bin/clang
[ -x "$actual_clang" ] || fail "Homebrew LLVM clang is unavailable"
clang_version="$("$actual_clang" --no-default-config --version)" ||
    fail "Homebrew LLVM clang version probe failed"
[ "${clang_version%%$'\n'*}" = 'Homebrew clang version 22.1.8' ] ||
    fail "Homebrew LLVM clang is not version 22.1.8"

cat >"$TEST_ROOT/sdk-link.c" <<'EOF'
#include <stdio.h>

int main(void)
{
    return puts("qualified native macOS SDK link passed") < 0;
}
EOF
SDKROOT="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT" \
MACOSX_DEPLOYMENT_TARGET=26.5 \
    "$actual_clang" --no-default-config -arch arm64 \
    "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG" \
    -mmacosx-version-min=26.5 "$TEST_ROOT/sdk-link.c" \
    -o "$TEST_ROOT/sdk-link"
/usr/bin/file -b "$TEST_ROOT/sdk-link" | /usr/bin/grep -q 'Mach-O.*arm64' ||
    fail "qualified SDK compile did not produce an ARM64 Mach-O executable"
[ "$("$TEST_ROOT/sdk-link")" = 'qualified native macOS SDK link passed' ] ||
    fail "qualified SDK executable did not run successfully"

echo "Native macOS SDK qualification and Homebrew LLVM link verified"
