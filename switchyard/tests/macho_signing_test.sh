#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
SIGNING_LIBRARY="$ROOT_DIR/switchyard/lib/macho_signing.sh"
ENTITLEMENTS="$ROOT_DIR/switchyard/wine-runtime-native-arm64.entitlements"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-macho-signing.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"

cleanup() {
  local status=$?

  trap - EXIT HUP INT TERM
  if [ -e /dev/fd/19 ]; then
    exec 19<&-
  fi
  case "$TEST_ROOT" in
    /private/tmp/switchyard-macho-signing.??????)
      [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected Mach-O signing fixture root $TEST_ROOT" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "Mach-O signing fixture failed: $1" >&2
  exit 1
}

expect_failure() {
  local description="$1"
  shift

  if "$@" >"$TEST_ROOT/rejected.out" 2>"$TEST_ROOT/rejected.err"; then
    fail "$description was accepted"
  fi
}

sha256_file() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

assert_no_staging() {
  local root="$1"

  if /usr/bin/find "$root" -name '.switchyard-codesign.*' -print -quit |
      /usr/bin/grep -q .; then
    fail "private signing staging directory leaked under $root"
  fi
}

assert_exact_native_entitlements() {
  /usr/bin/python3 -I - "$1" <<'PY'
import plistlib
import sys

expected = {
    "com.apple.security.cs.allow-dyld-environment-variables": True,
    "com.apple.security.cs.allow-jit": True,
    "com.apple.security.cs.allow-unsigned-executable-memory": True,
    "com.apple.security.custom-x18-abi-toggle": True,
}
with open(sys.argv[1], "rb") as stream:
    value = plistlib.load(stream)
if type(value) is not dict or value != expected:
    raise SystemExit("entitlement fixture is not the exact native allowlist")
PY
}

[ "$#" -eq 0 ] || {
  echo "usage: $0" >&2
  exit 2
}
[ "$(/usr/bin/uname -s)" = Darwin ] || fail "fixtures require macOS"
[ "$(/usr/bin/uname -m)" = arm64 ] || fail "fixtures require a native arm64 shell"
for input in "$SIGNING_LIBRARY" "$ENTITLEMENTS"; do
  [ -f "$input" ] && [ ! -L "$input" ] || fail "required input is missing or unsafe: $input"
done
# shellcheck disable=SC1090 # Fixed repository-relative signing policy.
source "$SIGNING_LIBRARY"
if /usr/bin/grep -E 'lib/wine/aarch64-unix/wine|bin/wine[.]switchyard-real' \
    "$SIGNING_LIBRARY" >/dev/null; then
  fail "single-item signing helper hardcodes build-runtime launcher integration"
fi

/bin/mkdir -m 700 "$TEST_ROOT/snapshots"
missing_entitlements="$TEST_ROOT/missing.entitlements"
extra_entitlements="$TEST_ROOT/extra.entitlements"
wrong_entitlements="$TEST_ROOT/wrong.entitlements"
integer_entitlements="$TEST_ROOT/integer.entitlements"
/usr/bin/python3 -I - "$ENTITLEMENTS" \
  "$missing_entitlements" "$extra_entitlements" "$wrong_entitlements" \
  "$integer_entitlements" <<'PY'
import plistlib
import sys

source, missing_name, extra_name, wrong_name, integer_name = sys.argv[1:]
with open(source, "rb") as stream:
    expected = plistlib.load(stream)
variants = []
missing = dict(expected)
del missing["com.apple.security.cs.allow-jit"]
variants.append((missing_name, missing))
extra = dict(expected)
extra["unexpected.entitlement"] = True
variants.append((extra_name, extra))
wrong = dict(expected)
wrong["com.apple.security.cs.allow-jit"] = False
variants.append((wrong_name, wrong))
integer = dict(expected)
integer["com.apple.security.cs.allow-jit"] = 1
variants.append((integer_name, integer))
for output, value in variants:
    with open(output, "xb") as stream:
        plistlib.dump(value, stream, fmt=plistlib.FMT_XML, sort_keys=True)
PY

for entitlement_case in \
    "$missing_entitlements" "$extra_entitlements" \
    "$wrong_entitlements" "$integer_entitlements"; do
  rejected_fd=""
  expect_failure "non-exact source entitlement set" \
    create_validated_entitlements_snapshot preview-native-arm64-fex \
      "$entitlement_case" "$TEST_ROOT/snapshots" rejected_fd
  [ -z "$rejected_fd" ] || fail "rejected entitlement source returned a descriptor"
done
/bin/ln -s "$ENTITLEMENTS" "$TEST_ROOT/linked.entitlements"
rejected_fd=""
expect_failure "symbolic-link entitlement source" \
  create_validated_entitlements_snapshot preview-native-arm64-fex \
    "$TEST_ROOT/linked.entitlements" "$TEST_ROOT/snapshots" rejected_fd
[ -z "$rejected_fd" ] || fail "symbolic-link source returned a descriptor"
if /usr/bin/find "$TEST_ROOT/snapshots" -name '.switchyard-entitlements.*' -print -quit |
    /usr/bin/grep -q .; then
  fail "rejected entitlement source leaked a snapshot"
fi

busy_fd=""
exec 19</dev/null
expect_failure "already occupied snapshot descriptor" \
  create_validated_entitlements_snapshot preview-native-arm64-fex \
    "$ENTITLEMENTS" "$TEST_ROOT/snapshots" busy_fd
exec 19<&-
[ -z "$busy_fd" ] || fail "occupied snapshot descriptor was returned"
if /usr/bin/find "$TEST_ROOT/snapshots" -name '.switchyard-entitlements.*' -print -quit |
    /usr/bin/grep -q .; then
  fail "occupied snapshot descriptor leaked a private snapshot"
fi

fake_codesign="$TEST_ROOT/fake-codesign"
/bin/cat >"$fake_codesign" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

operation=sign
case "${1:-}" in
  --verify) operation=verify ;;
  -d)
    operation=details
    for argument in "$@"; do
      [ "$argument" != "--entitlements" ] || operation=entitlements
    done
    ;;
esac
item="${!#}"

case "$operation" in
  sign)
    [ "${SWITCHYARD_FAKE_FAIL_SIGN:-0}" -eq 0 ] || exit 81
    identity=""
    entitlements=""
    previous=""
    for argument in "$@"; do
      if [ "$previous" = "--sign" ]; then identity="$argument"; fi
      if [ "$previous" = "--entitlements" ]; then entitlements="$argument"; fi
      previous="$argument"
    done
    if [ "$identity" = - ]; then
      [ -n "$entitlements" ] && [ -r "$entitlements" ] || exit 82
      /bin/cp "$entitlements" "$SWITCHYARD_FAKE_CAPTURED_ENTITLEMENTS"
      /usr/bin/printf '\nSWITCHYARD_ENGINEERING_SIGNATURE\n' >>"$item"
    else
      /usr/bin/printf '\nSWITCHYARD_RELEASE_SIGNATURE\n' >>"$item"
    fi
    if [ -n "${SWITCHYARD_FAKE_MUTATE_SOURCE:-}" ]; then
      /bin/cp "$SWITCHYARD_FAKE_MUTATION_CONTENT" "$SWITCHYARD_FAKE_MUTATE_SOURCE"
    fi
    if [ -n "${SWITCHYARD_FAKE_REPLACE_TARGET:-}" ]; then
      /usr/bin/printf 'CONCURRENT-TARGET\n' >"$SWITCHYARD_FAKE_REPLACE_TARGET.replacement"
      /bin/chmod 0755 "$SWITCHYARD_FAKE_REPLACE_TARGET.replacement"
      /bin/mv "$SWITCHYARD_FAKE_REPLACE_TARGET.replacement" "$SWITCHYARD_FAKE_REPLACE_TARGET"
    fi
    if [ -n "${SWITCHYARD_FAKE_SWAP_PARENT:-}" ]; then
      /bin/mv "$SWITCHYARD_FAKE_SWAP_PARENT" "$SWITCHYARD_FAKE_SWAP_PARENT_MOVED"
      /bin/ln -s "$SWITCHYARD_FAKE_SWAP_PARENT_REPLACEMENT" "$SWITCHYARD_FAKE_SWAP_PARENT"
    fi
    /usr/bin/printf 'sign\t%s\n' "$*" >>"$SWITCHYARD_FAKE_LOG"
    ;;
  verify)
    /usr/bin/grep -a -E 'SWITCHYARD_(ENGINEERING|RELEASE)_SIGNATURE' "$item" >/dev/null || exit 83
    ;;
  details)
    if /usr/bin/grep -a -F 'SWITCHYARD_RELEASE_SIGNATURE' "$item" >/dev/null; then
      /usr/bin/printf '%s\n' \
        'CodeDirectory v=20500 size=1 flags=0x10000(runtime) hashes=1+0 location=embedded' \
        'Signature size=9000' \
        'Authority=Developer ID Application: Fixture (TEAMID)' \
        'Runtime Version=26.0.0'
    elif [ "${SWITCHYARD_FAKE_HARDENED:-0}" -eq 1 ]; then
      /usr/bin/printf '%s\n' \
        'CodeDirectory v=20500 size=1 flags=0x10002(adhoc,runtime) hashes=1+0 location=embedded' \
        'Signature=adhoc' \
        'Runtime Version=26.0.0'
    else
      /usr/bin/printf '%s\n' \
        'CodeDirectory v=20400 size=1 flags=0x2(adhoc) hashes=1+0 location=embedded' \
        'Signature=adhoc'
    fi
    ;;
  entitlements)
    /bin/cat "${SWITCHYARD_FAKE_EMBEDDED_ENTITLEMENTS:-$SWITCHYARD_FAKE_CAPTURED_ENTITLEMENTS}"
    ;;
  *) exit 84 ;;
esac
EOF
/bin/chmod 0755 "$fake_codesign"

make_target() {
  local path="$1"

  /bin/mkdir -p "$(dirname "$path")"
  /usr/bin/printf 'UNSIGNED:%s\n' "$(basename "$path")" >"$path"
  /bin/chmod 0755 "$path"
}

runtime="$TEST_ROOT/runtime"
active="$runtime/lib/wine/aarch64-unix/wine"
fallback="$runtime/bin/wine.switchyard-real"
make_target "$active"
make_target "$fallback"
mutable_entitlements="$TEST_ROOT/mutable.entitlements"
/bin/cp "$ENTITLEMENTS" "$mutable_entitlements"
captured_entitlements="$TEST_ROOT/captured.entitlements"
codesign_log="$TEST_ROOT/codesign.log"
: >"$captured_entitlements"
: >"$codesign_log"
export SWITCHYARD_FAKE_CAPTURED_ENTITLEMENTS="$captured_entitlements"
export SWITCHYARD_FAKE_LOG="$codesign_log"
export SWITCHYARD_FAKE_MUTATE_SOURCE="$mutable_entitlements"
export SWITCHYARD_FAKE_MUTATION_CONTENT="$wrong_entitlements"

snapshot_fd=""
create_validated_entitlements_snapshot preview-native-arm64-fex \
  "$mutable_entitlements" "$TEST_ROOT/snapshots" snapshot_fd ||
  fail "could not create the exact native entitlement snapshot"
[ "$snapshot_fd" = 19 ] && [ -e /dev/fd/19 ] || fail "snapshot descriptor was not pinned"
if /usr/bin/find "$TEST_ROOT/snapshots" -name '.switchyard-entitlements.*' -print -quit |
    /usr/bin/grep -q .; then
  fail "validated snapshot retained a mutable filesystem name"
fi

active_before="$(sha256_file "$active")"
fallback_before="$(sha256_file "$fallback")"
sign_engineering_macho_atomically \
  "$fake_codesign" preview-native-arm64-fex "$active" "$snapshot_fd" ||
  fail "exact engineering active launcher signing failed"
sign_engineering_macho_atomically \
  "$fake_codesign" preview-native-arm64-fex "$fallback" "$snapshot_fd" ||
  fail "exact engineering fallback launcher signing failed"
[ "$(sha256_file "$active")" != "$active_before" ] || fail "active launcher was not replaced"
[ "$(sha256_file "$fallback")" != "$fallback_before" ] || fail "fallback launcher was not replaced"
/usr/bin/cmp -s "$mutable_entitlements" "$wrong_entitlements" ||
  fail "source mutation hook did not run"
assert_exact_native_entitlements "$captured_entitlements"
[ "$(/usr/bin/grep -c '^sign' "$codesign_log")" -eq 2 ] ||
  fail "engineering helper did not sign exactly two requested targets"
assert_no_staging "$runtime"
close_validated_entitlements_snapshot "$snapshot_fd" || fail "could not close snapshot"
unset SWITCHYARD_FAKE_MUTATE_SOURCE SWITCHYARD_FAKE_MUTATION_CONTENT

failure_snapshots="$TEST_ROOT/failure-snapshots"
/bin/mkdir -m 700 "$failure_snapshots"
failure_fd=""
create_validated_entitlements_snapshot preview-native-arm64-fex \
  "$ENTITLEMENTS" "$failure_snapshots" failure_fd || fail "could not create failure snapshot"

for embedded_case in \
    "$missing_entitlements" "$extra_entitlements" \
    "$wrong_entitlements" "$integer_entitlements"; do
  embedded_target="$TEST_ROOT/embedded-$(basename "$embedded_case").bin"
  make_target "$embedded_target"
  embedded_before="$(sha256_file "$embedded_target")"
  SWITCHYARD_FAKE_EMBEDDED_ENTITLEMENTS="$embedded_case"
  export SWITCHYARD_FAKE_EMBEDDED_ENTITLEMENTS
  expect_failure "non-exact embedded entitlement set" \
    sign_engineering_macho_atomically \
      "$fake_codesign" preview-native-arm64-fex "$embedded_target" "$failure_fd"
  [ "$(sha256_file "$embedded_target")" = "$embedded_before" ] ||
    fail "embedded-entitlement rejection changed the published target"
  assert_no_staging "$TEST_ROOT"
done
unset SWITCHYARD_FAKE_EMBEDDED_ENTITLEMENTS

hardened_target="$TEST_ROOT/hardened.bin"
make_target "$hardened_target"
hardened_before="$(sha256_file "$hardened_target")"
SWITCHYARD_FAKE_HARDENED=1
export SWITCHYARD_FAKE_HARDENED
expect_failure "Hardened engineering signature" \
  sign_engineering_macho_atomically \
    "$fake_codesign" preview-native-arm64-fex "$hardened_target" "$failure_fd"
[ "$(sha256_file "$hardened_target")" = "$hardened_before" ] ||
  fail "Hardened rejection changed the published target"
assert_no_staging "$TEST_ROOT"
unset SWITCHYARD_FAKE_HARDENED

failed_target="$TEST_ROOT/codesign-failure.bin"
make_target "$failed_target"
failed_before="$(sha256_file "$failed_target")"
SWITCHYARD_FAKE_FAIL_SIGN=1
export SWITCHYARD_FAKE_FAIL_SIGN
expect_failure "codesign failure" \
  sign_engineering_macho_atomically \
    "$fake_codesign" preview-native-arm64-fex "$failed_target" "$failure_fd"
[ "$(sha256_file "$failed_target")" = "$failed_before" ] ||
  fail "codesign failure changed the published target"
assert_no_staging "$TEST_ROOT"
unset SWITCHYARD_FAKE_FAIL_SIGN

safe_target="$TEST_ROOT/safe-target.bin"
linked_target="$TEST_ROOT/linked-target.bin"
make_target "$safe_target"
/bin/ln -s "$safe_target" "$linked_target"
expect_failure "symbolic-link signing target" \
  sign_engineering_macho_atomically \
    "$fake_codesign" preview-native-arm64-fex "$linked_target" "$failure_fd"

swap_target="$TEST_ROOT/target-swap.bin"
make_target "$swap_target"
SWITCHYARD_FAKE_REPLACE_TARGET="$swap_target"
export SWITCHYARD_FAKE_REPLACE_TARGET
expect_failure "concurrent signing target replacement" \
  sign_engineering_macho_atomically \
    "$fake_codesign" preview-native-arm64-fex "$swap_target" "$failure_fd"
/usr/bin/grep -Fx 'CONCURRENT-TARGET' "$swap_target" >/dev/null ||
  fail "target-swap rejection overwrote the concurrent replacement"
assert_no_staging "$TEST_ROOT"
unset SWITCHYARD_FAKE_REPLACE_TARGET

swap_parent="$TEST_ROOT/swap-parent"
swap_parent_moved="$TEST_ROOT/swap-parent.moved"
swap_parent_replacement="$TEST_ROOT/swap-parent.replacement"
/bin/mkdir -m 700 "$swap_parent" "$swap_parent_replacement"
parent_target="$swap_parent/launcher"
make_target "$parent_target"
SWITCHYARD_FAKE_SWAP_PARENT="$swap_parent"
SWITCHYARD_FAKE_SWAP_PARENT_MOVED="$swap_parent_moved"
SWITCHYARD_FAKE_SWAP_PARENT_REPLACEMENT="$swap_parent_replacement"
export SWITCHYARD_FAKE_SWAP_PARENT SWITCHYARD_FAKE_SWAP_PARENT_MOVED
export SWITCHYARD_FAKE_SWAP_PARENT_REPLACEMENT
expect_failure "concurrent signing parent replacement" \
  sign_engineering_macho_atomically \
    "$fake_codesign" preview-native-arm64-fex "$parent_target" "$failure_fd"
[ -L "$swap_parent" ] || fail "parent-swap fixture did not replace the public parent"
/usr/bin/grep -Fx 'UNSIGNED:launcher' "$swap_parent_moved/launcher" >/dev/null ||
  fail "parent-swap rejection changed the original launcher"
assert_no_staging "$swap_parent_moved"
unset SWITCHYARD_FAKE_SWAP_PARENT SWITCHYARD_FAKE_SWAP_PARENT_MOVED
unset SWITCHYARD_FAKE_SWAP_PARENT_REPLACEMENT

release_target="$TEST_ROOT/release.bin"
make_target "$release_target"
sign_release_macho_atomically \
  "$fake_codesign" "$release_target" 'Developer ID Application: Fixture (TEAMID)' ||
  fail "separate Developer-ID/Hardened release mode failed"
/usr/bin/grep -a -F 'SWITCHYARD_RELEASE_SIGNATURE' "$release_target" >/dev/null ||
  fail "release mode did not publish its signed target"
assert_no_staging "$TEST_ROOT"
close_validated_entitlements_snapshot "$failure_fd" || fail "could not close failure snapshot"

real_runtime="$TEST_ROOT/real-runtime"
real_active="$real_runtime/lib/wine/aarch64-unix/wine"
real_fallback="$real_runtime/bin/wine.switchyard-real"
/bin/mkdir -p "$(dirname "$real_active")" "$(dirname "$real_fallback")"
/usr/bin/xcrun --sdk macosx clang -arch arm64 -O2 -Wall -Wextra -Werror \
  -x c -o "$real_active" - <<'EOF'
int main(void)
{
    return 0;
}
EOF
/bin/cp -p "$real_active" "$real_fallback"
real_snapshots="$TEST_ROOT/real-snapshots"
/bin/mkdir -m 700 "$real_snapshots"
real_fd=""
create_validated_entitlements_snapshot preview-native-arm64-fex \
  "$ENTITLEMENTS" "$real_snapshots" real_fd || fail "could not create real snapshot"
for real_target in "$real_active" "$real_fallback"; do
  sign_engineering_macho_atomically \
    /usr/bin/codesign preview-native-arm64-fex "$real_target" "$real_fd" ||
    fail "real ad-hoc engineering signing failed: $real_target"
  /usr/bin/codesign --verify --strict --verbose=2 "$real_target" >/dev/null 2>&1 ||
    fail "real signed Mach-O failed strict verification"
  real_details="$(/usr/bin/codesign -d --verbose=4 "$real_target" 2>&1)"
  /usr/bin/printf '%s\n' "$real_details" |
    /usr/bin/grep -F 'flags=0x2(adhoc)' >/dev/null ||
    fail "real engineering Mach-O does not have flags 0x2"
  if /usr/bin/printf '%s\n' "$real_details" |
      /usr/bin/grep -F 'Runtime Version=' >/dev/null; then
    fail "real engineering Mach-O enabled Hardened Runtime"
  fi
  "$real_target" || fail "real signed Mach-O did not execute"
done
close_validated_entitlements_snapshot "$real_fd" || fail "could not close real snapshot"
assert_no_staging "$real_runtime"

echo "Mach-O atomic signing and entitlement snapshot fixtures passed"
