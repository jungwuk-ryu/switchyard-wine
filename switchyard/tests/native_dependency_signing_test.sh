#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/switchyard/build_runtime.sh"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
MACHO_SIGNING_LIBRARY="$ROOT_DIR/switchyard/lib/macho_signing.sh"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-native-dependency-signing.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && /bin/pwd -P)"
failure_index=0

cleanup()
{
    case "$TEST_ROOT" in
        /private/tmp/switchyard-native-dependency-signing.??????)
            [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
            ;;
        *) echo "refusing to clean unexpected native signing test root $TEST_ROOT" >&2 ;;
    esac
}
trap cleanup EXIT

fail()
{
    echo "Native dependency signing test: $*" >&2
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

if [ "$(/usr/bin/uname -s)" != Darwin ] ||
   [ "$(/usr/bin/uname -m)" != arm64 ]; then
    echo "Native dependency signing test skipped outside native macOS ARM64"
    exit 0
fi

/bin/chmod 0700 "$TEST_ROOT"
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
export NATIVE_CPU_PROVIDER_ENABLED=1
HELPER_FILE="$TEST_ROOT/native-dependency-signing-helpers.sh"
/usr/bin/python3 -I - "$BUILD_SCRIPT" "$HELPER_FILE" <<'PY'
import pathlib
import re
import sys

source_path = pathlib.Path(sys.argv[1])
output_path = pathlib.Path(sys.argv[2])
source = source_path.read_text(encoding="utf-8")
names = (
    "adhoc_sign_host_macho_tree",
    "verify_host_macho_tree_signatures",
    "adhoc_sign_and_verify_host_macho_tree",
    "adhoc_sign_and_verify_host_macho_file",
)
declarations = list(
    re.finditer(
        r"^(?P<name>[A-Za-z_][A-Za-z0-9_]*)\(\)[ \t]*(?:\{\n|\n\{\n)",
        source,
        re.MULTILINE,
    )
)
fragments = []
for name in names:
    matches = [match for match in declarations if match.group("name") == name]
    if len(matches) != 1:
        raise SystemExit(f"expected one {name} definition, found {len(matches)}")
    match = matches[0]
    closing = re.search(r"^}\n", source[match.end():], re.MULTILINE)
    if closing is None:
        raise SystemExit(f"could not find the end of {name}")
    end = match.end() + closing.end()
    fragment = source[match.start():end].rstrip()
    if not fragment.endswith("}"):
        raise SystemExit(f"extracted {name} definition is incomplete")
    fragments.append(fragment)
output_path.write_text("\n\n".join(fragments) + "\n", encoding="utf-8")
PY

# shellcheck disable=SC1090 # Libraries are resolved from the worktree root.
source "$PROFILE_LIBRARY"
# The host-tree helper may use the shared atomic signer; source the production
# implementation so the test exercises either direct or atomic ad-hoc signing.
# shellcheck disable=SC1090
source "$MACHO_SIGNING_LIBRARY"
# build_runtime.sh has no source-only mode, so source its exact extracted helper
# definitions without executing downloads or dependency staging.
# shellcheck disable=SC1090
source "$HELPER_FILE"

native_homebrew=/opt/homebrew/bin/brew
[ -f "$native_homebrew" ] && [ ! -L "$native_homebrew" ] &&
    [ -x "$native_homebrew" ] || fail "physical Apple Silicon Homebrew is unavailable"
llvm_prefix="$("$native_homebrew" --prefix llvm)"
llvm_bin="$(cd "$llvm_prefix/bin" && /bin/pwd -P)"
switchyard_qualify_native_llvm_compilers "$llvm_bin" ||
    fail "native LLVM compiler qualification failed"
switchyard_qualify_native_macos_sdk 26.5 /usr/bin/xcrun ||
    fail "native macOS SDK qualification failed"

valid_root="$TEST_ROOT/valid"
/bin/mkdir -m 0700 "$valid_root"
fixture="$valid_root/libswitchyard-signature-fixture.dylib"
/usr/bin/printf '%s\n' 'int switchyard_signature_fixture(void) { return 7; }' |
    SDKROOT="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDKROOT" \
    MACOSX_DEPLOYMENT_TARGET=26.5 \
    "$SWITCHYARD_QUALIFIED_NATIVE_CLANG" \
        "$SWITCHYARD_NATIVE_CLANG_NO_DEFAULT_CONFIG_FLAG" \
        -arch arm64 "$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG" \
        -mmacosx-version-min=26.5 -dynamiclib \
        -Wl,-headerpad_max_install_names \
        -Wl,-install_name,@rpath/libswitchyard-signature-fixture.dylib \
        -x c - -o "$fixture"
/usr/bin/file -b "$fixture" | /usr/bin/grep -q 'Mach-O.*arm64' ||
    fail "qualified toolchain did not produce an ARM64 Mach-O dylib"
/usr/bin/codesign --force --sign - "$fixture" >/dev/null 2>&1 ||
    fail "could not create the initial ad-hoc signature"
/usr/bin/codesign --verify --strict --verbose=2 "$fixture" >/dev/null 2>&1 ||
    fail "initial ad-hoc signature did not pass strict verification"

/usr/bin/install_name_tool -id \
    '@rpath/libswitchyard-signature-fixture-relocated.dylib' "$fixture"
if /usr/bin/codesign --verify --strict --verbose=2 \
        "$fixture" >/dev/null 2>&1; then
    fail "install-name mutation unexpectedly preserved the code signature"
fi
expect_failure "invalid relocated signature" \
    verify_host_macho_tree_signatures "$valid_root" "relocated fixture"

adhoc_sign_and_verify_host_macho_tree "$valid_root" "relocated fixture" ||
    fail "host-tree helper did not repair the relocated signature"
/usr/bin/codesign --verify --strict --verbose=2 "$fixture" >/dev/null 2>&1 ||
    fail "repaired signature did not pass independent strict verification"
verify_host_macho_tree_signatures "$valid_root" "repaired fixture" ||
    fail "repaired tree did not pass the production signature validator"

/usr/bin/install_name_tool -id \
    '@rpath/libswitchyard-signature-fixture-file-relocated.dylib' "$fixture"
if /usr/bin/codesign --verify --strict --verbose=2 \
        "$fixture" >/dev/null 2>&1; then
    fail "second install-name mutation unexpectedly preserved the code signature"
fi
adhoc_sign_and_verify_host_macho_file "$fixture" "relocated fixture file" ||
    fail "host-file helper did not repair the relocated signature"
/usr/bin/codesign --verify --strict --verbose=2 "$fixture" >/dev/null 2>&1 ||
    fail "file-helper repaired signature did not pass independent strict verification"
verify_host_macho_tree_signatures "$valid_root" "file-helper repaired fixture" ||
    fail "file-helper repaired tree did not pass the production signature validator"

empty_root="$TEST_ROOT/no-macho"
/bin/mkdir -m 0700 "$empty_root"
/usr/bin/printf '%s\n' 'not a Mach-O file' >"$empty_root/README.txt"
expect_failure "signing a tree without Mach-O files" \
    adhoc_sign_host_macho_tree "$empty_root" "non-Mach-O fixture"
expect_failure "verifying a tree without Mach-O files" \
    verify_host_macho_tree_signatures "$empty_root" "non-Mach-O fixture"
expect_failure "signing and verifying a tree without Mach-O files" \
    adhoc_sign_and_verify_host_macho_tree "$empty_root" "non-Mach-O fixture"
expect_failure "signing a non-Mach-O file" \
    adhoc_sign_and_verify_host_macho_file \
    "$empty_root/README.txt" "non-Mach-O fixture file"

linked_root="$TEST_ROOT/linked-root"
/bin/ln -s "$valid_root" "$linked_root"
expect_failure "signing through a symlink root" \
    adhoc_sign_host_macho_tree "$linked_root" "linked fixture"
expect_failure "verifying through a symlink root" \
    verify_host_macho_tree_signatures "$linked_root" "linked fixture"
expect_failure "signing and verifying through a symlink root" \
    adhoc_sign_and_verify_host_macho_tree "$linked_root" "linked fixture"
linked_file="$TEST_ROOT/linked-file.dylib"
/bin/ln -s "$fixture" "$linked_file"
expect_failure "signing through a symlink file" \
    adhoc_sign_and_verify_host_macho_file "$linked_file" "linked fixture file"

echo "Native dependency relocation signature repair and root safety verified"
