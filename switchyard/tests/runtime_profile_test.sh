#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/switchyard/build_runtime.sh"
RELEASE_SCRIPT="$ROOT_DIR/switchyard/release_runtime.sh"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
TEST_ROOT="$(mktemp -d)"
failure_index=0

cleanup() {
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

fail() {
  echo "$1" >&2
  exit 1
}

expect_failure() {
  local label="$1"
  local expected_status="$2"
  local expected_message="$3"
  local status
  local stdout_file
  local stderr_file
  shift 3

  failure_index=$((failure_index + 1))
  stdout_file="$TEST_ROOT/failure-$failure_index.stdout"
  stderr_file="$TEST_ROOT/failure-$failure_index.stderr"
  set +e
  "$@" >"$stdout_file" 2>"$stderr_file"
  status=$?
  set -e
  [ "$status" -eq "$expected_status" ] || {
    cat "$stdout_file" >&2
    cat "$stderr_file" >&2
    fail "$label returned $status instead of $expected_status"
  }
  grep -F -- "$expected_message" "$stderr_file" >/dev/null || {
    cat "$stderr_file" >&2
    fail "$label did not report the expected error"
  }
}

write_stable_manifest() {
  local destination="$1"

  cat >"$destination" <<'EOF'
{
  "manifestVersion": 2,
  "id": "switchyard-local-wow64-x86_64-test",
  "runtimeFamily": "stable-x86_64-rosetta",
  "buildProfile": "switchyard-wow64-pe",
  "host": {
    "platform": "macos",
    "architecture": "x86_64",
    "wineUnixArchitecture": "x86_64",
    "buildTriplet": "x86_64-apple-darwin",
    "hostTriplet": "x86_64-apple-darwin",
    "architectureCommand": ["arch", "-x86_64"],
    "requiresRosetta": true,
    "minimumMacOS": "14.0",
    "gstreamerRegistryArchitecture": "x86_64"
  },
  "peArchitectures": ["i386", "x86_64"]
}
EOF
}

write_preview_manifest() {
  local destination="$1"

  cat >"$destination" <<'EOF'
{
  "manifestVersion": 2,
  "id": "switchyard-local-native-arm64-fex-test",
  "runtimeFamily": "preview-native-arm64-fex",
  "buildProfile": "switchyard-native-arm64-fex",
  "host": {
    "platform": "macos",
    "architecture": "arm64",
    "wineUnixArchitecture": "aarch64",
    "buildTriplet": "aarch64-apple-darwin",
    "hostTriplet": "aarch64-apple-darwin",
    "architectureCommand": ["arch", "-arm64"],
    "requiresRosetta": false,
    "minimumMacOS": "26.5",
    "gstreamerRegistryArchitecture": "arm64"
  },
  "peArchitectures": ["aarch64", "arm64ec", "x86_64", "i386"]
}
EOF
}

# shellcheck disable=SC1090 # The test resolves the worktree root at runtime.
source "$PROFILE_LIBRARY"

[ "$(grep -c 'signed_runtime/bin/wineserver' "$RELEASE_SCRIPT")" -eq 2 ] ||
  fail "release cleanup and smoke teardown do not use the installed wineserver path"
if grep -F 'signed_runtime/bin/switchyard-wineserver' "$RELEASE_SCRIPT" >/dev/null; then
  fail "release still references the nonexistent switchyard-wineserver path"
fi

switchyard_load_runtime_profile stable-x86_64-rosetta
[ "$SWITCHYARD_RUNTIME_PROFILE_ENABLED" = "1" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX" = "switchyard-local-wow64-x86_64-" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH" = "x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH" = "x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS_CSV" = "i386,x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET" = "x86_64-apple-darwin" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET" = "x86_64-apple-darwin" ]
[ "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[*]}" = "arch -x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA" = "true" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" = "14.0" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH" = "x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX" = "macos-x86_64" ]

switchyard_load_runtime_profile preview-native-arm64-fex
[ "$SWITCHYARD_RUNTIME_PROFILE_ENABLED" = "0" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX" = "switchyard-local-native-arm64-fex-" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH" = "arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH" = "aarch64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS_CSV" = "aarch64,arm64ec,x86_64,i386" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET" = "aarch64-apple-darwin" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET" = "aarch64-apple-darwin" ]
[ "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[*]}" = "arch -arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA" = "false" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" = "26.5" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH" = "arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX" = "macos-arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH" != "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH" ]

default_source_info="$(SWITCHYARD_DISABLE_GPTK_OVERLAY=1 "$BUILD_SCRIPT" --source-info)"
stable_source_info="$(SWITCHYARD_DISABLE_GPTK_OVERLAY=1 \
  "$BUILD_SCRIPT" --runtime-profile stable-x86_64-rosetta --source-info)"
[ "$default_source_info" = "$stable_source_info" ] ||
  fail "the default build profile does not match the explicit stable profile"
grep -Fx 'runtimeProfile=stable-x86_64-rosetta' <<<"$default_source_info" >/dev/null
grep -Fx 'hostMachOArchitecture=x86_64' <<<"$default_source_info" >/dev/null
grep -Fx 'wineUnixArchitecture=x86_64' <<<"$default_source_info" >/dev/null
grep -Fx 'peArchitectures=i386,x86_64' <<<"$default_source_info" >/dev/null
grep -Fx 'requiresRosetta=true' <<<"$default_source_info" >/dev/null
grep -Fx 'gstreamerRegistryArchitecture=x86_64' <<<"$default_source_info" >/dev/null

injection_sentinel="$TEST_ROOT/profile-was-executed"
expect_failure "unknown profile" 2 "Unknown runtime profile." \
  "$BUILD_SCRIPT" --runtime-profile \
  "stable-x86_64-rosetta; touch $injection_sentinel" --source-info
[ ! -e "$injection_sentinel" ] || fail "an unknown profile value was executed as shell input"
expect_failure "equals-form profile" 2 "usage:" \
  "$BUILD_SCRIPT" --runtime-profile=stable-x86_64-rosetta --source-info
expect_failure "missing profile value" 2 "--runtime-profile requires a profile name." \
  "$BUILD_SCRIPT" --runtime-profile --source-info
expect_failure "duplicate profile" 2 "--runtime-profile may be specified only once." \
  "$BUILD_SCRIPT" --runtime-profile stable-x86_64-rosetta \
  --runtime-profile stable-x86_64-rosetta --source-info
expect_failure "disabled preview" 2 \
  "Runtime profile preview-native-arm64-fex is recognized but not enabled." \
  env PATH=/usr/bin:/bin /bin/bash "$BUILD_SCRIPT" \
  --runtime-profile preview-native-arm64-fex --source-info

stable_manifest="$TEST_ROOT/stable-runtime.json"
write_stable_manifest "$stable_manifest"
switchyard_validate_runtime_manifest_profile \
  "$stable_manifest" stable-x86_64-rosetta

preview_manifest="$TEST_ROOT/preview-runtime.json"
write_preview_manifest "$preview_manifest"
switchyard_validate_runtime_manifest_profile \
  "$preview_manifest" preview-native-arm64-fex

invalid_manifest="$TEST_ROOT/invalid-runtime.json"
cp "$stable_manifest" "$invalid_manifest"
/usr/bin/plutil -replace manifestVersion -integer 3 "$invalid_manifest"
expect_failure "manifest version" 1 "manifestVersion must be 2" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" stable-x86_64-rosetta

cp "$stable_manifest" "$invalid_manifest"
/usr/bin/plutil -replace manifestVersion -string 2 "$invalid_manifest"
expect_failure "string manifest version" 1 \
  "manifest is not valid unambiguous JSON data" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" stable-x86_64-rosetta

cp "$stable_manifest" "$invalid_manifest"
/usr/bin/plutil -replace host.requiresRosetta -string true "$invalid_manifest"
expect_failure "string Rosetta requirement" 1 \
  "manifest is not valid unambiguous JSON data" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" stable-x86_64-rosetta

cat >"$invalid_manifest" <<'EOF'
{
  "runtimeFamily": "stable-x86_64-rosetta",
  "runtimeFamily": "preview-native-arm64-fex"
}
EOF
expect_failure "duplicate manifest key" 1 \
  "manifest is not valid unambiguous JSON data" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" stable-x86_64-rosetta

cp "$stable_manifest" "$invalid_manifest"
/usr/bin/plutil -replace host.architecture -string arm64 "$invalid_manifest"
expect_failure "host architecture" 1 "host.architecture does not match" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" stable-x86_64-rosetta

cp "$stable_manifest" "$invalid_manifest"
/usr/bin/plutil -replace host.architectureCommand \
  -json '["arch", "-arm64;touch"]' "$invalid_manifest"
expect_failure "architecture command" 1 "host.architectureCommand is not the exact profile allowlist" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" stable-x86_64-rosetta

cp "$stable_manifest" "$invalid_manifest"
/usr/bin/plutil -insert peArchitectures.2 -string arm64ec "$invalid_manifest"
expect_failure "PE architecture allowlist" 1 "peArchitectures has an unexpected length" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" stable-x86_64-rosetta

ln -s "$stable_manifest" "$TEST_ROOT/symlink-runtime.json"
expect_failure "symbolic-link manifest" 1 "manifest must be a regular file" \
  switchyard_validate_runtime_manifest_profile \
  "$TEST_ROOT/symlink-runtime.json" stable-x86_64-rosetta

release_runtime="$TEST_ROOT/release-runtime"
mkdir -p "$release_runtime"
cp "$stable_manifest" "$release_runtime/switchyard-runtime.json"
/usr/bin/plutil -replace host.wineUnixArchitecture -string aarch64 \
  "$release_runtime/switchyard-runtime.json"
release_output="$TEST_ROOT/release-output"
release_temporary="$TEST_ROOT/release-temporary"
mkdir -p "$release_temporary"
expect_failure "release manifest validation" 1 \
  "host.wineUnixArchitecture does not match" \
  env TMPDIR="$release_temporary" "$RELEASE_SCRIPT" --runtime "$release_runtime" \
  --runtime-content-sha256 "$(printf '%064d' 0)" \
  --output "$release_output" --identity -
[ ! -e "$release_output" ] || fail "release validation created output before rejecting the manifest"
[ -z "$(find "$release_temporary" -mindepth 1 -print -quit)" ] ||
  fail "release validation did not remove its private manifest snapshot"

cp "$stable_manifest" "$release_runtime/switchyard-runtime.json"
/usr/bin/plutil -replace id -string \
  $'switchyard-local-wow64-x86_64-test",\n  "injected": true,\n  "tail": "' \
  "$release_runtime/switchyard-runtime.json"
expect_failure "release runtime ID validation" 1 \
  "id contains characters outside the safe identifier allowlist" \
  "$RELEASE_SCRIPT" --runtime "$release_runtime" \
  --runtime-content-sha256 "$(printf '%064d' 0)" \
  --output "$release_output" --identity -
[ ! -e "$release_output" ] || fail "malformed runtime ID created release output"

cp "$preview_manifest" "$release_runtime/switchyard-runtime.json"
expect_failure "disabled preview release" 2 \
  "Runtime profile preview-native-arm64-fex is recognized but not enabled." \
  "$RELEASE_SCRIPT" --runtime "$release_runtime" \
  --runtime-content-sha256 "$(printf '%064d' 0)" \
  --output "$release_output" --identity -
[ ! -e "$release_output" ] || fail "disabled preview release created an output directory"

echo "runtime profile tests passed"
