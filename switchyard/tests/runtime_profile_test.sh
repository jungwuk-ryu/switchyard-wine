#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_SCRIPT="$ROOT_DIR/switchyard/build_runtime.sh"
RELEASE_SCRIPT="$ROOT_DIR/switchyard/release_runtime.sh"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
UNICORN_HELPER="$ROOT_DIR/switchyard/build_unicorn_runtime.sh"
UNICORN_PATCH="$ROOT_DIR/switchyard/patches/unicorn-2.1.4-threaded-emu-stop.patch"
ARM64_TLS_MANIFEST="$ROOT_DIR/switchyard/tls-deps-arm64.tsv"
STABLE_ENTITLEMENTS="$ROOT_DIR/switchyard/wine-runtime.entitlements"
NATIVE_ENTITLEMENTS="$ROOT_DIR/switchyard/wine-runtime-native-arm64.entitlements"
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
  "peArchitectures": ["aarch64", "arm64ec", "x86_64", "i386"],
  "cpuProvider": {
    "implementation": "unicorn",
    "version": "2.1.4",
    "sourceRepository": "https://github.com/unicorn-engine/unicorn.git",
    "sourceRevision": "8028ec436f2d9376525352dd38ed9ed6b9f6be10",
    "sourceArchive": "lib/switchyard-unicorn/share/src/switchyard-unicorn/unicorn-8028ec436f2d9376525352dd38ed9ed6b9f6be10.tar.gz",
    "sourceArchiveSha256": "d3859317cc562ad9d172a32a4e4c2e62613df494b1155a0bf58dd0581fc1675e",
    "sourcePatch": {
      "path": "lib/switchyard-unicorn/share/src/switchyard-unicorn/unicorn-2.1.4-threaded-emu-stop.patch",
      "sha256": "68f7df756eec731ec2143d63b8e454b7d1d538ccee8fa2205563fb26c3b995d6"
    },
    "buildContractVersion": 3,
    "hostArchitecture": "arm64",
    "kuserSharedDataModel": "translated-shadow",
    "emulatedArchitectures": ["i386", "x86_64"],
    "developmentCacheDigest": "7944bfa5710dfec5183ac1b8460c9dfce5f7c4b0af40675ce6afb893a3e38e87",
    "runtimeRoot": "lib/switchyard-unicorn",
    "runtimePayloadDigest": "a9a853d25af1274fde256ac3dff2b83ba563d8cbe70c5c7a754ca93b94ee0486",
    "library": "lib/switchyard-unicorn/lib/libunicorn.2.dylib",
    "librarySha256": "60b4c1e2cec6459c8d5bf7aafa11b43f79e029ec85b199e116dd29a4f0636b07",
    "providerUnixLibraries": [
      "lib/wine/aarch64-unix/xtajit.so",
      "lib/wine/aarch64-unix/xtajit64.so"
    ],
    "components": [
      {
        "guestArchitecture": "i386",
        "unixLibrary": "lib/wine/aarch64-unix/xtajit.so",
        "unixLibrarySha256": "3333333333333333333333333333333333333333333333333333333333333333",
        "peLibrary": "lib/wine/aarch64-windows/xtajit.dll",
        "peLibrarySha256": "4444444444444444444444444444444444444444444444444444444444444444"
      },
      {
        "guestArchitecture": "x86_64",
        "unixLibrary": "lib/wine/aarch64-unix/xtajit64.so",
        "unixLibrarySha256": "5555555555555555555555555555555555555555555555555555555555555555",
        "peLibrary": "lib/wine/aarch64-windows/xtajit64.dll",
        "peLibrarySha256": "6666666666666666666666666666666666666666666666666666666666666666"
      }
    ],
    "runtimeRpath": "@loader_path/../../switchyard-unicorn/lib",
    "manifest": "lib/switchyard-unicorn/switchyard-unicorn-runtime.json"
  }
}
EOF
}

# shellcheck disable=SC1090 # The test resolves the worktree root at runtime.
source "$PROFILE_LIBRARY"

if ! /usr/bin/python3 - "$STABLE_ENTITLEMENTS" "$NATIVE_ENTITLEMENTS" <<'PY'
import plistlib
import sys

expected = (
    {
        "com.apple.security.cs.allow-dyld-environment-variables": True,
        "com.apple.security.cs.allow-unsigned-executable-memory": True,
    },
    {
        "com.apple.security.cs.allow-dyld-environment-variables": True,
        "com.apple.security.cs.allow-jit": True,
        "com.apple.security.cs.allow-unsigned-executable-memory": True,
        "com.apple.security.custom-x18-abi-toggle": True,
    },
)
for path, wanted in zip(sys.argv[1:], expected):
    with open(path, "rb") as stream:
        actual = plistlib.load(stream)
    if actual != wanted:
        raise SystemExit(f"unexpected entitlements in {path}: {actual!r}")
PY
then
  fail "runtime signing entitlement plists do not contain the exact profile policy"
fi

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
[ "${SWITCHYARD_RUNTIME_PROFILE_INSTALLED_PE_ARCHS[*]}" = "i386 x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET" = "x86_64-apple-darwin" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET" = "x86_64-apple-darwin" ]
[ "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[*]}" = "arch -x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA" = "true" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" = "14.0" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH" = "x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_HOST_DEPENDENCY_ARCH" = "x86_64" ]
[ "${SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_MACHO_ARCHS[*]}" = "x86_64 arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_FONT_BOTTLE_TAG" = "sonoma" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_VULKAN_LOADER_BOTTLE_TAG" = "tahoe" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_VULKAN_HEADERS_BOTTLE_TAG" = "tahoe" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MOLTENVK_BOTTLE_TAG" = "sonoma" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_SUBDIR" = "osx-64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_MANIFEST_BASENAME" = "tls-deps.tsv" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_UNICORN" = "false" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_DXMT" = "false" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL" = "direct" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX" = "macos-x86_64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME" = "wine-runtime.entitlements" ]
[ "$(switchyard_runtime_profile_entitlements_path "$ROOT_DIR")" = "$STABLE_ENTITLEMENTS" ]

switchyard_load_runtime_profile preview-native-arm64-fex
[ "$SWITCHYARD_RUNTIME_PROFILE_ENABLED" = "1" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX" = "switchyard-local-native-arm64-fex-" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH" = "arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH" = "aarch64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS_CSV" = "aarch64,arm64ec,x86_64,i386" ]
[ "${SWITCHYARD_RUNTIME_PROFILE_INSTALLED_PE_ARCHS[*]}" = "aarch64 x86_64 i386" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET" = "aarch64-apple-darwin" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET" = "aarch64-apple-darwin" ]
[ "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[*]}" = "arch -arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA" = "false" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" = "26.5" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH" = "arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_HOST_DEPENDENCY_ARCH" = "arm64" ]
[ "${SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_MACHO_ARCHS[*]}" = "x86_64 arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_FONT_BOTTLE_TAG" = "arm64_sonoma" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_VULKAN_LOADER_BOTTLE_TAG" = "arm64_tahoe" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_VULKAN_HEADERS_BOTTLE_TAG" = "arm64_tahoe" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MOLTENVK_BOTTLE_TAG" = "arm64_tahoe" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_SUBDIR" = "osx-arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_TLS_PACKAGE_MANIFEST_BASENAME" = "tls-deps-arm64.tsv" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_UNICORN" = "true" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_REQUIRES_DXMT" = "true" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL" = "translated-shadow" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX" = "macos-arm64" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME" = "wine-runtime-native-arm64.entitlements" ]
[ "$(switchyard_runtime_profile_entitlements_path "$ROOT_DIR")" = "$NATIVE_ENTITLEMENTS" ]
[ "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH" != "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH" ]
[ "$SWITCHYARD_UNICORN_VERSION" = "2.1.4" ]
[ "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY" = "https://github.com/unicorn-engine/unicorn.git" ]
[ "$SWITCHYARD_UNICORN_SOURCE_REVISION" = "8028ec436f2d9376525352dd38ed9ed6b9f6be10" ]
[ "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" = "d3859317cc562ad9d172a32a4e4c2e62613df494b1155a0bf58dd0581fc1675e" ]
[ "$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" = "unicorn-2.1.4-threaded-emu-stop.patch" ]
[ "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" = "68f7df756eec731ec2143d63b8e454b7d1d538ccee8fa2205563fb26c3b995d6" ]
[ "$SWITCHYARD_UNICORN_LIBRARY_SHA256" = "60b4c1e2cec6459c8d5bf7aafa11b43f79e029ec85b199e116dd29a4f0636b07" ]
[ "$SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION" = "3" ]
[ "$SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST" = "7944bfa5710dfec5183ac1b8460c9dfce5f7c4b0af40675ce6afb893a3e38e87" ]
[ "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" = "a9a853d25af1274fde256ac3dff2b83ba563d8cbe70c5c7a754ca93b94ee0486" ]
[ "$SWITCHYARD_DXMT_SOURCE_REPOSITORY" = "https://github.com/3Shain/dxmt.git" ]
[ "$SWITCHYARD_DXMT_SOURCE_REVISION" = "856d9f35789679ef00c1ba01a6353438df84b66f" ]
[ "$SWITCHYARD_DXMT_SOURCE_BASE_TREE" = "22fa93d36867f175c0283b36cd3628a4df94876e" ]
[ "$SWITCHYARD_DXMT_SOURCE_TREE" = "a8c397f9b03dcb3592f6b0204ae6dbda5492990d" ]
[ "$SWITCHYARD_DXMT_SOURCE_PATCH_BASENAME" = "0001-fix-dxmt-use-owned-buffer-backing-for-i386.patch" ]
[ "$SWITCHYARD_DXMT_SOURCE_PATCH_SHA256" = "5491ef13f2adfd611c12df30f191ac0ffd0083bcb246c5ab81ef1d29a8baa852" ]
[ "$SWITCHYARD_DXMT_ARTIFACT_BUILD_IDENTITY" = "f02a37f5b7c8022941712a7cf9415ac9d1925442" ]
[ "$SWITCHYARD_NATIVE_RUNTIME_CLOSURE_CONTRACT_VERSION" = "3" ]
[ "$SWITCHYARD_DXMT_ARTIFACT_NAME" = "dxmt-f02a37f5b7c8022941712a7cf9415ac9d1925442.tar.gz" ]
[ "$SWITCHYARD_DXMT_ARTIFACT_SHA256" = "4bf4f0bd654a92c0feb6a8e5b960307be53d62ef45f9ed32fdcbf37c418b8a3c" ]
[ "$SWITCHYARD_DXMT_WINEMETAL_ORIGINAL_SHA256" = "1c03a178db45540507e3784ed97890ee4fd8baffa1413e00991b6588c95859d0" ]
[ "$SWITCHYARD_DXMT_WOW64_ABI_SCHEMA_SHA256" = "0051bd8c0bc3e3ce261e9d5007665342ac2d28a643576744d8ec71896af856f1" ]
[ "$SWITCHYARD_DXMT_PACKAGE_WORKFLOW" = ".github/workflows/ci.yml" ]
[ "$SWITCHYARD_DXMT_PACKAGE_WORKFLOW_SHA256" = "fe5a3656b9f59e81e650e60077bcdd840a5205ff0d960f00f6cb4c8fbacbe851" ]
[ "$SWITCHYARD_DXMT_PACKAGE_BUILD" = "gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+clang-release-arm64ec-windows-cross" ]

closure_inputs=(
  "0123456789ab-dirty-111111111111"
  "0123456789abcdef0123456789abcdef01234567"
  "true"
  "$(printf '%064x' 1)"
  "no-gptk"
  "$(printf '%064x' 2)"
  "$(printf '%064x' 3)"
  "$(printf '%064x' 4)"
  "$(printf '%064x' 5)"
  "$(printf '%064x' 6)"
  "$(printf '%064x' 7)"
  "$(printf '%064x' 8)"
  "@loader_path/../../switchyard-tls/lib/libgnutls.dylib"
  "$(printf '%064x' 9)"
  "$(printf '%064x' 10)"
  "$(printf '%064x' 11)"
  "$SWITCHYARD_DXMT_SOURCE_PATCH_SHA256"
  "$SWITCHYARD_DXMT_WINEMETAL_ORIGINAL_SHA256"
  "$(printf '%064x' 12)"
  "$(printf '%064x' 13)"
)
closure_digest="$(switchyard_native_runtime_closure_digest "${closure_inputs[@]}")"
[[ "$closure_digest" =~ ^[0-9a-f]{64}$ ]] ||
  fail "native runtime closure is not a full SHA-256"
[ "$closure_digest" = "6fcfa05d146b38f5c2ac641c02de447c708a3fb6a90402ba4aace75214b21e9a" ] ||
  fail "native runtime closure v3 labels, order, or domain changed"
[ "$(switchyard_native_runtime_closure_digest "${closure_inputs[@]}")" = \
  "$closure_digest" ] || fail "native runtime closure is not deterministic"

for closure_index in 0 1 2 3 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19; do
  mutated_closure_inputs=("${closure_inputs[@]}")
  case "$closure_index" in
    0) mutated_closure_inputs[closure_index]="different-source-identity" ;;
    1) mutated_closure_inputs[closure_index]="89abcdef0123456789abcdef0123456789abcdef" ;;
    2) mutated_closure_inputs[closure_index]="false" ;;
    12) mutated_closure_inputs[closure_index]="@loader_path/different/libgnutls.dylib" ;;
    *) mutated_closure_inputs[closure_index]="$(printf '%064x' "$((closure_index + 32))")" ;;
  esac
  mutated_closure_digest="$(
    switchyard_native_runtime_closure_digest "${mutated_closure_inputs[@]}"
  )"
  [ "$mutated_closure_digest" != "$closure_digest" ] ||
    fail "native runtime closure does not bind input $closure_index"
done

mutated_closure_inputs=("${closure_inputs[@]}")
mutated_closure_inputs[4]="caller-gptk"
expect_failure "native closure GPTK policy" 1 \
  "native runtime closure must not bind a GPTK overlay" \
  switchyard_native_runtime_closure_digest "${mutated_closure_inputs[@]}"
expect_failure "native closure arity" 2 \
  "Native runtime closure requires the exact 20-input contract." \
  switchyard_native_runtime_closure_digest "${closure_inputs[@]:0:19}"
mutated_closure_inputs=("${closure_inputs[@]}")
mutated_closure_inputs[13]="123456789abc"
expect_failure "truncated TLS dlopen closure input" 1 \
  "native runtime closure TLS dlopen digest is invalid" \
  switchyard_native_runtime_closure_digest "${mutated_closure_inputs[@]}"
mutated_closure_inputs=("${closure_inputs[@]}")
mutated_closure_inputs[17]="1C03A178DB45540507E3784ED97890EE4FD8BAFFA1413E00991B6588C95859D0"
expect_failure "uppercase original DXMT winemetal closure input" 1 \
  "native runtime closure input is not a full SHA-256: dxmtOriginalWinemetalSha256" \
  switchyard_native_runtime_closure_digest "${mutated_closure_inputs[@]}"
mutated_closure_inputs=("${closure_inputs[@]}")
mutated_closure_inputs[18]="123456789abc"
expect_failure "truncated DXMT companion ABI schema closure input" 1 \
  "native runtime closure input is not a full SHA-256: dxmtCompanionAbiSchemaSha256" \
  switchyard_native_runtime_closure_digest "${mutated_closure_inputs[@]}"
mutated_closure_inputs=("${closure_inputs[@]}")
closure_swap="${mutated_closure_inputs[6]}"
mutated_closure_inputs[6]="${mutated_closure_inputs[7]}"
mutated_closure_inputs[7]="$closure_swap"
[ "$(switchyard_native_runtime_closure_digest "${mutated_closure_inputs[@]}")" != \
  "$closure_digest" ] || fail "native runtime closure loses input order or labels"

closure_source_revision="${closure_inputs[1]}"
clean_runtime_id="$(switchyard_native_runtime_id_from_closure_digest \
  "$SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX" "$closure_source_revision" false \
  "$closure_digest")"
dirty_runtime_id="$(switchyard_native_runtime_id_from_closure_digest \
  "$SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX" "$closure_source_revision" true \
  "$closure_digest")"
[ "$clean_runtime_id" = \
  "switchyard-local-native-arm64-fex-0123456789ab-$closure_digest" ] ||
  fail "native clean runtime ID does not preserve its short source identity and full closure"
[ "$dirty_runtime_id" = \
  "switchyard-local-native-arm64-fex-0123456789ab-dirty-$closure_digest" ] ||
  fail "native dirty runtime ID does not preserve its source state and full closure"
[ "${#clean_runtime_id}" -eq 111 ] && [ "${#dirty_runtime_id}" -eq 117 ] ||
  fail "native runtime ID is not compact"

installhinf_budget_boundary="$(/usr/bin/python3 - <<'PY'
print("/" + "a" * 213)
PY
)"
[ "$(switchyard_utf16_code_unit_count "$installhinf_budget_boundary")" -eq 214 ] ||
  fail "InstallHinfSection boundary fixture has the wrong UTF-16 length"
switchyard_validate_native_runtime_prefix_bootstrap_budget \
  "$installhinf_budget_boundary" ||
  fail "native path policy rejected the confirmed 214-unit boundary"
installhinf_budget_overflow="$(/usr/bin/python3 - <<'PY'
print("/" + "a" * 214)
PY
)"
expect_failure "native InstallHinfSection path budget" 1 \
  "InstallHinfSection would publish a 260-UTF-16-code-unit wine.inf command tail; maximum is 259." \
  switchyard_validate_native_runtime_prefix_bootstrap_budget \
  "$installhinf_budget_overflow"
installhinf_astral_boundary="$(/usr/bin/python3 - <<'PY'
print("/" + "a" * 211 + "\U0001f600")
PY
)"
[ "$(switchyard_utf16_code_unit_count "$installhinf_astral_boundary")" -eq 214 ] ||
  fail "native path policy did not count an astral scalar as two UTF-16 units"
switchyard_validate_native_runtime_prefix_bootstrap_budget \
  "$installhinf_astral_boundary" ||
  fail "native path policy rejected the astral 214-unit boundary"
installhinf_astral_overflow="$(/usr/bin/python3 - <<'PY'
print("/" + "a" * 212 + "\U0001f600")
PY
)"
expect_failure "native astral InstallHinfSection path budget" 1 \
  "InstallHinfSection would publish a 260-UTF-16-code-unit wine.inf command tail; maximum is 259." \
  switchyard_validate_native_runtime_prefix_bootstrap_budget \
  "$installhinf_astral_overflow"

fake_llvm_root="$TEST_ROOT/fake-llvm"
fake_llvm_bin="$fake_llvm_root/bin"
fake_llvm_log="$TEST_ROOT/fake-llvm.log"
fake_llvm_mode="$TEST_ROOT/fake-llvm.mode"
mkdir -p "$fake_llvm_bin"
fake_llvm_bin="$(cd "$fake_llvm_bin" && /bin/pwd -P)"
cat >"$fake_llvm_bin/clang" <<'EOF'
#!/usr/bin/env bash
set -eu
program="${0##*/}"
{
  printf '%s' "$program"
  printf ' %s' "$@"
  printf '\n'
} >>"$SWITCHYARD_TEST_LLVM_LOG"
mode="$(cat "$SWITCHYARD_TEST_LLVM_MODE")"
if [ "$1" = "--no-default-config" ] && [ "$2" = "--version" ]; then
  if [ "$mode" = "bad-version" ]; then
    echo "Homebrew clang version 0.0.0"
  else
    echo "Homebrew clang version 22.1.8"
  fi
  exit 0
fi
target=""
for argument in "$@"; do
  case "$argument" in
    --target=*) target="${argument#--target=}" ;;
  esac
done
if [ "${2:-}" = "--print-target-triple" ] ||
   [ "${3:-}" = "--print-target-triple" ]; then
  case "$target" in
    "") echo "arm64-apple-darwin25.5.0" ;;
    arm64-apple-darwin) echo "arm64-apple-darwin" ;;
    aarch64-w64-mingw32)
      if [ "$mode" = "bad-target" ]; then
        echo "aarch64-unknown-linux-gnu"
      else
        echo "aarch64-w64-windows-gnu"
      fi
      ;;
    arm64ec-w64-mingw32) echo "arm64ec-w64-windows-gnu" ;;
    x86_64-w64-mingw32) echo "x86_64-w64-windows-gnu" ;;
    i686-w64-mingw32) echo "i686-w64-windows-gnu" ;;
    *) exit 91 ;;
  esac
  exit 0
fi
exit 92
EOF
cp "$fake_llvm_bin/clang" "$fake_llvm_bin/clang++"
chmod 0755 "$fake_llvm_bin/clang" "$fake_llvm_bin/clang++"
printf 'ok\n' >"$fake_llvm_mode"
export SWITCHYARD_TEST_LLVM_LOG="$fake_llvm_log"
export SWITCHYARD_TEST_LLVM_MODE="$fake_llvm_mode"
switchyard_qualify_native_llvm_compilers "$fake_llvm_bin"
[[ "$SWITCHYARD_QUALIFIED_NATIVE_COMPILER_IDENTITY" =~ ^[0-9a-f]{64}$ ]] ||
  fail "qualified compiler policy does not have a full identity"
[ "$SWITCHYARD_QUALIFIED_NATIVE_CLANG" = "$fake_llvm_bin/clang" ] &&
  [ "$SWITCHYARD_QUALIFIED_NATIVE_CLANGXX" = "$fake_llvm_bin/clang++" ] ||
  fail "qualified compiler policy did not retain physical executable paths"
cat >"$TEST_ROOT/fake-llvm.expected" <<'EOF'
clang --no-default-config --version
clang++ --no-default-config --version
clang --no-default-config --target=arm64-apple-darwin --print-target-triple
clang --no-default-config --target=aarch64-w64-mingw32 --print-target-triple
clang --no-default-config --target=arm64ec-w64-mingw32 --print-target-triple
clang --no-default-config --target=x86_64-w64-mingw32 --print-target-triple
clang --no-default-config --target=i686-w64-mingw32 --print-target-triple
clang++ --no-default-config --print-target-triple
EOF
cmp "$TEST_ROOT/fake-llvm.expected" "$fake_llvm_log" ||
  fail "qualified compiler capability probes ran in an unexpected order"

cp "$fake_llvm_bin/clang" "$TEST_ROOT/qualified-clang.backup"
printf '# mutation\n' >>"$fake_llvm_bin/clang"
expect_failure "qualified compiler mutation" 1 \
  "Qualified native LLVM compiler identity changed during the build." \
  switchyard_validate_qualified_native_llvm_compilers
cp "$TEST_ROOT/qualified-clang.backup" "$fake_llvm_bin/clang"
chmod 0755 "$fake_llvm_bin/clang"
switchyard_qualify_native_llvm_compilers "$fake_llvm_bin"
printf 'bad-version\n' >"$fake_llvm_mode"
expect_failure "qualified compiler version" 1 \
  "Native ARM64 runtime requires Homebrew clang version 22.1.8." \
  switchyard_qualify_native_llvm_compilers "$fake_llvm_bin"
printf 'bad-target\n' >"$fake_llvm_mode"
expect_failure "qualified compiler target" 1 \
  "Native Homebrew LLVM clang cannot target aarch64-w64-mingw32" \
  switchyard_qualify_native_llvm_compilers "$fake_llvm_bin"
printf 'ok\n' >"$fake_llvm_mode"
ln -s "$fake_llvm_bin" "$fake_llvm_root/linked-bin"
expect_failure "symbolic LLVM bin" 1 \
  "Native LLVM bin path is missing or is not physical" \
  switchyard_qualify_native_llvm_compilers "$fake_llvm_root/linked-bin"

qualified_makefile="$TEST_ROOT/qualified-Makefile"
cat >"$qualified_makefile" <<EOF
CXX = $fake_llvm_bin/clang++ --no-default-config -arch arm64
OBJC = $fake_llvm_bin/clang --no-default-config -arch arm64
CC = $fake_llvm_bin/clang --no-default-config -arch arm64 -std=gnu23
EOF
switchyard_native_configured_compiler_policy_is_exact \
  "$qualified_makefile" "$fake_llvm_bin/clang" "$fake_llvm_bin/clang++" \
  arm64 --no-default-config ||
  fail "configured compiler validator rejected configure-owned C23 mode"
cat >"$qualified_makefile" <<EOF
CXX = $fake_llvm_bin/clang++ --no-default-config -arch arm64
OBJC = $fake_llvm_bin/clang --no-default-config -arch arm64
CC = $fake_llvm_bin/clang --no-default-config -arch arm64
EOF
switchyard_native_configured_compiler_policy_is_exact \
  "$qualified_makefile" "$fake_llvm_bin/clang" "$fake_llvm_bin/clang++" \
  arm64 --no-default-config ||
  fail "configured compiler validator rejected an exact default C mode"
cat >"$qualified_makefile" <<EOF
CXX = $fake_llvm_bin/clang++ --no-default-config -arch arm64
OBJC = $fake_llvm_bin/clang --no-default-config -arch arm64
CC = $fake_llvm_bin/clang --no-default-config -arch arm64 -std=gnu23
EOF
cat >>"$qualified_makefile" <<EOF
CC += -fno-integrated-as
EOF
if switchyard_native_configured_compiler_policy_is_exact \
    "$qualified_makefile" "$fake_llvm_bin/clang" "$fake_llvm_bin/clang++" \
    arm64 --no-default-config; then
  fail "configured compiler validator accepted a duplicate compiler mutation"
fi
cat >"$qualified_makefile" <<EOF
CC = $fake_llvm_bin/clang -arch arm64 --no-default-config
CXX = $fake_llvm_bin/clang++ --no-default-config -arch arm64
OBJC = $fake_llvm_bin/clang --no-default-config -arch arm64
EOF
if switchyard_native_configured_compiler_policy_is_exact \
    "$qualified_makefile" "$fake_llvm_bin/clang" "$fake_llvm_bin/clang++" \
    arm64 --no-default-config; then
  fail "configured compiler validator accepted nondeterministic flag order"
fi
cat >"$qualified_makefile" <<EOF
CC = $fake_llvm_bin/clang --no-default-config -arch arm64 -std=gnu17
CXX = $fake_llvm_bin/clang++ --no-default-config -arch arm64
OBJC = $fake_llvm_bin/clang --no-default-config -arch arm64
EOF
if switchyard_native_configured_compiler_policy_is_exact \
    "$qualified_makefile" "$fake_llvm_bin/clang" "$fake_llvm_bin/clang++" \
    arm64 --no-default-config; then
  fail "configured compiler validator accepted an unqualified language mode"
fi
cat >"$qualified_makefile" <<EOF
CC = $fake_llvm_bin/clang --no-default-config -arch arm64 -std=gnu23 -O2
CXX = $fake_llvm_bin/clang++ --no-default-config -arch arm64
OBJC = $fake_llvm_bin/clang --no-default-config -arch arm64
EOF
if switchyard_native_configured_compiler_policy_is_exact \
    "$qualified_makefile" "$fake_llvm_bin/clang" "$fake_llvm_bin/clang++" \
    arm64 --no-default-config; then
  fail "configured compiler validator accepted an extra compiler suffix"
fi
unset SWITCHYARD_TEST_LLVM_LOG SWITCHYARD_TEST_LLVM_MODE

/bin/bash -n "$UNICORN_HELPER" || fail "Unicorn helper does not pass bash syntax validation"
grep -F 'BUILD_WORK_DIR="$(mktemp -d "$BUILD_DIR/.build.XXXXXX")"' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not isolate each CMake build in a fresh work directory"
grep -F -- '-B "$BUILD_WORK_DIR"' "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not configure the isolated build directory"
if grep -F 'cmake --build "$BUILD_DIR"' "$UNICORN_HELPER" >/dev/null; then
  fail "Unicorn helper can execute a persistent build-tree recipe"
fi
grep -F 'validate_source_archive "$source_archive"' "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not validate the deterministic source archive"
[ "$(/usr/bin/shasum -a 256 "$UNICORN_PATCH" | /usr/bin/awk '{print $1}')" = \
  "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" ] ||
  fail "Unicorn source patch differs from the qualified exact patch"
grep -F 'git -C "$SOURCE_DIR" archive --format=tar HEAD | /usr/bin/tar -xf - -C "$PATCHED_SOURCE_DIR"' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not export the pristine source into a private tree"
grep -F 'validate_source_patch "$PATCH_SNAPSHOT"' "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not validate its private exact-byte patch snapshot"
grep -F 'git -C "$PATCHED_SOURCE_DIR" apply --check --whitespace=error-all "$PATCH_SNAPSHOT"' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not preflight the exact source patch without whitespace repair"
grep -F 'git -C "$PATCHED_SOURCE_DIR" apply --whitespace=error-all "$PATCH_SNAPSHOT"' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not apply the qualified source patch"
grep -F 'git -C "$PATCHED_SOURCE_DIR" apply --reverse --check --whitespace=error-all "$PATCH_SNAPSHOT"' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not verify the private patched source state"
grep -F -- '-S "$PATCHED_SOURCE_DIR"' "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not build from the private patched source tree"
if grep -F -- '-S "$SOURCE_DIR"' "$UNICORN_HELPER" >/dev/null; then
  fail "Unicorn helper can still build directly from the pristine checkout"
fi
grep -F 'source "$ROOT_DIR/switchyard/lib/directory_safety.sh"' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not load the fd-relative directory publisher"
grep -F '"$SWAP_HELPER_DIR/switchyard-preview-directory-publish" \' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not use the fd-relative directory publisher"
grep -F 'ensure_preview_swap_helper || fail' "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not diagnose publisher preparation failure"
grep -F '"$STAGING" "$OUTPUT" exclusive || publication_status=$?' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper publication is not exclusive"
if ! /usr/bin/python3 -I - "$UNICORN_HELPER" <<'PY'
import sys

text = open(sys.argv[1], encoding="utf-8").read()
publication = text.index('"$STAGING" "$OUTPUT" exclusive || publication_status=$?')
failure_branch = text.index('if [ "$publication_status" -ne 0 ]; then', publication)
fail_stop = text.index('[ "$publication_status" -ne 3 ] || STAGING=""', failure_branch)
failure = text.index('fail "exclusive output publication failed: $OUTPUT"', fail_stop)
success_clear = text.index('STAGING=""', failure + 1)
if not publication < failure_branch < fail_stop < failure < success_clear:
    raise SystemExit("Unicorn publication status handling is out of order")
PY
then
  fail "Unicorn helper can path-clean an ambiguous fail-stop publication"
fi
if grep -F '/bin/mv "$STAGING" "$OUTPUT"' "$UNICORN_HELPER" >/dev/null; then
  fail "Unicorn helper retains a path-racy output publication"
fi
(
  publication_root="$TEST_ROOT/unicorn-publication"
  publication_target="$publication_root/output"
  publication_go="$publication_root/go"
  publication_ready_a="$publication_root/ready-a"
  publication_ready_b="$publication_root/ready-b"
  publication_stage_a="$publication_root/stage-a"
  publication_stage_b="$publication_root/stage-b"
  publication_status_a=0
  publication_status_b=0
  SWAP_HELPER_DIR=""
  unset SWITCHYARD_PREVIEW_PUBLISH_TEST_FAILURE

  cleanup_unicorn_publication_test() {
    if [ -n "$SWAP_HELPER_DIR" ] && [ -d "$SWAP_HELPER_DIR" ] &&
       [ ! -L "$SWAP_HELPER_DIR" ]; then
      /bin/rm -rf "$SWAP_HELPER_DIR"
    fi
  }
  trap cleanup_unicorn_publication_test EXIT

  # shellcheck source=../lib/directory_safety.sh
  source "$ROOT_DIR/switchyard/lib/directory_safety.sh"
  ensure_preview_swap_helper
  publication_helper="$SWAP_HELPER_DIR/switchyard-preview-directory-publish"
  mkdir -m 0700 "$publication_root" "$publication_stage_a" "$publication_stage_b"
  printf 'a\n' >"$publication_stage_a/winner"
  printf 'b\n' >"$publication_stage_b/winner"

  (
    : >"$publication_ready_a"
    while [ ! -e "$publication_go" ]; do /bin/sleep 0.01; done
    "$publication_helper" "$publication_stage_a" "$publication_target" exclusive
  ) >"$publication_root/publisher-a.stdout" \
    2>"$publication_root/publisher-a.stderr" &
  publication_pid_a=$!
  (
    : >"$publication_ready_b"
    while [ ! -e "$publication_go" ]; do /bin/sleep 0.01; done
    "$publication_helper" "$publication_stage_b" "$publication_target" exclusive
  ) >"$publication_root/publisher-b.stdout" \
    2>"$publication_root/publisher-b.stderr" &
  publication_pid_b=$!
  while [ ! -e "$publication_ready_a" ] || [ ! -e "$publication_ready_b" ]; do
    /bin/sleep 0.01
  done
  : >"$publication_go"
  wait "$publication_pid_a" || publication_status_a=$?
  wait "$publication_pid_b" || publication_status_b=$?

  if [ "$publication_status_a" -eq 0 ]; then
    [ "$publication_status_b" -ne 0 ] &&
      [ "$(/bin/cat "$publication_target/winner")" = a ] &&
      [ ! -e "$publication_stage_a" ] && [ -d "$publication_stage_b" ] ||
      fail "exclusive Unicorn output publication did not preserve the losing stage"
  elif [ "$publication_status_b" -eq 0 ]; then
    [ "$(/bin/cat "$publication_target/winner")" = b ] &&
      [ ! -e "$publication_stage_b" ] && [ -d "$publication_stage_a" ] ||
      fail "exclusive Unicorn output publication did not preserve the losing stage"
  else
    fail "both competing Unicorn output publications failed"
  fi
  [ "$(/usr/bin/find "$publication_target" -mindepth 1 -maxdepth 1 \
       -type d -print | /usr/bin/wc -l | /usr/bin/tr -d ' ')" -eq 0 ] ||
    fail "a losing Unicorn stage was nested inside the published output"

  symlink_stage="$publication_root/symlink-stage"
  symlink_target="$publication_root/symlink-output"
  symlink_backing="$publication_root/symlink-backing"
  mkdir -m 0700 "$symlink_stage" "$symlink_backing"
  printf 'stage\n' >"$symlink_stage/content"
  printf 'keep\n' >"$symlink_backing/content"
  ln -s "$symlink_backing" "$symlink_target"
  if "$publication_helper" "$symlink_stage" "$symlink_target" exclusive; then
    fail "Unicorn output publication followed a symbolic-link destination"
  fi
  [ -L "$symlink_target" ] && [ -d "$symlink_stage" ] &&
    [ "$(/bin/cat "$symlink_backing/content")" = keep ] ||
    fail "rejected symbolic-link publication changed an existing path"

  for publication_failure in verify fsync; do
    failure_stage="$publication_root/failure-$publication_failure-stage"
    failure_target="$publication_root/failure-$publication_failure-output"
    mkdir -m 0700 "$failure_stage"
    printf 'private\n' >"$failure_stage/content"
    if SWITCHYARD_PREVIEW_PUBLISH_TEST_FAILURE="$publication_failure" \
        "$publication_helper" "$failure_stage" "$failure_target" exclusive; then
      fail "injected Unicorn $publication_failure publication unexpectedly succeeded"
    fi
    [ -d "$failure_stage" ] && [ ! -e "$failure_target" ] &&
      [ ! -L "$failure_target" ] ||
      fail "injected Unicorn $publication_failure failure was not rolled back"
  done
)
if ! /usr/bin/python3 - "$ARM64_TLS_MANIFEST" <<'PY'
import sys

expected = [
    ("gmp", "6.3.0", "h7bae524_2", "gmp-6.3.0-h7bae524_2.conda", "76e222e072d61c840f64a44e0580c2503562b009090f55aa45053bf1ccb385dd"),
    ("gnutls", "3.8.13", "hfe86254_0", "gnutls-3.8.13-hfe86254_0.conda", "d36222db9e35788af78b6f41f66dc702d20b18375a9059c81ee074ea4c63d72a"),
    ("libffi", "3.5.2", "hcf2aa1b_0", "libffi-3.5.2-hcf2aa1b_0.conda", "6686a26466a527585e6a75cc2a242bf4a3d97d6d6c86424a441677917f28bec7"),
    ("libiconv", "1.18", "h23cfdf5_2", "libiconv-1.18-h23cfdf5_2.conda", "de0336e800b2af9a40bdd694b03870ac4a848161b35c8a2325704f123f185f03"),
    ("libidn2", "2.3.8", "ha90df94_1", "libidn2-2.3.8-ha90df94_1.conda", "6d9d2db2c8a645f63073553e9497ea266507c940de42800f2f0faddbf00149cd"),
    ("libintl", "0.25.1", "h493aca8_0", "libintl-0.25.1-h493aca8_0.conda", "99d2cebcd8f84961b86784451b010f5f0a795ed1c08f1e7c76fbb3c22abf021a"),
    ("libtasn1", "4.21.0", "h84a0fba_0", "libtasn1-4.21.0-h84a0fba_0.conda", "0593a03d869f3452ced058ca428ceeeff089f8556bb8609226c505362b72369b"),
    ("nettle", "3.10.1", "h2435c67_0", "nettle-3.10.1-h2435c67_0.conda", "83079cfea1512921327cdc8d7950ffaa6e6b7ca1d48bcacd5a5537852da088bb"),
    ("p11-kit", "0.26.4", "h4613bd8_0", "p11-kit-0.26.4-h4613bd8_0.conda", "927ba155350e9668e6661c196bd5076c699d787900b5751d52a753919f2ccbd0"),
]
rows = []
with open(sys.argv[1], encoding="utf-8") as stream:
    for raw in stream:
        line = raw.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        fields = tuple(line.split("\t"))
        if len(fields) != 5:
            raise SystemExit("invalid ARM64 TLS manifest row")
        rows.append(fields)
if rows != expected:
    raise SystemExit("ARM64 TLS manifest differs from the closed package policy")
PY
then
  fail "ARM64 TLS manifest does not match the exact closed policy"
fi

grep -F 'MACOSX_DEPLOYMENT_TARGET="$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"' \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "runtime build does not export the selected profile's macOS deployment target"
grep -F 'configure_deployment_flag="-mmacosx-version-min=$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"' \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "runtime build does not apply the selected profile's minimum macOS compiler/linker flag"

preview_entitlements_basename="$SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME"
SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME="wine-runtime.entitlements"
expect_failure "cross-profile entitlements path" 2 \
  "Runtime profile signing entitlements are not allowlisted." \
  switchyard_runtime_profile_entitlements_path "$ROOT_DIR"
SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME="../../untrusted.entitlements"
expect_failure "unallowlisted entitlements path" 2 \
  "Runtime profile signing entitlements are not allowlisted." \
  switchyard_runtime_profile_entitlements_path "$ROOT_DIR"
SWITCHYARD_RUNTIME_PROFILE_ENTITLEMENTS_BASENAME="$preview_entitlements_basename"

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
stable_trace="$TEST_ROOT/stable-source-info.trace"
SWITCHYARD_DISABLE_GPTK_OVERLAY=1 /bin/bash -x "$BUILD_SCRIPT" --source-info \
  >"$TEST_ROOT/stable-source-info.out" 2>"$stable_trace"
if /usr/bin/grep -E \
    '^\++ (source|\.) .*/(native_arm64_packaging|native_cpu_provider|dxmt_artifact|macho_signing)\.sh$|^\++ /bin/bash .*/native_arm64_host_probe\.sh|^\++ (switchyard_.*(native_arm64|dxmt)|create_validated_entitlements_snapshot|sign_engineering_macho_atomically)( |$)' \
    "$stable_trace" >/dev/null; then
  fail "stable source-info sourced or called native-only tooling"
fi
if grep -E '^(cpuProvider|unicorn)' <<<"$default_source_info" >/dev/null; then
  fail "stable source metadata unexpectedly includes native Unicorn identity"
fi
if grep -E '^kuserSharedDataModel=' <<<"$default_source_info" >/dev/null; then
  fail "stable source metadata unexpectedly includes the native KUSER model"
fi
grep -F -- '      --with-unicorn' "$BUILD_SCRIPT" >/dev/null ||
  fail "native profile does not pass --with-unicorn to Wine configure"
grep -F 'INSTALLED_PE_ARCHS=("${SWITCHYARD_RUNTIME_PROFILE_INSTALLED_PE_ARCHS[@]}")' \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "runtime build does not separate configure PE targets from installed PE layout"
[ "$(grep -F -c 'for pe_arch in "${INSTALLED_PE_ARCHS[@]}"; do' "$BUILD_SCRIPT")" -eq 2 ] ||
  fail "runtime install validation still iterates configure-only PE targets"
grep -F "UNICORN_RUNTIME_RPATH='@loader_path/../../switchyard-unicorn/lib'" \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "native providers do not use the runtime-relative Unicorn rpath"
grep -F 'stage_unicorn_runtime "$WINE_INSTALL_PREFIX" "$unicorn_runtime_prefix"' \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "native runtime assembly does not stage the validated Unicorn closure"
grep -F 'vulkanRuntime.moltenVK.bottle' "$BUILD_SCRIPT" >/dev/null ||
  fail "native runtime completeness does not validate the exact MoltenVK bottle"
grep -F 'tlsRuntime.packageSubdir' "$BUILD_SCRIPT" >/dev/null ||
  fail "native runtime completeness does not validate the TLS package subdir"
grep -F 'gstreamerRuntime.runtimePackageSha256' "$BUILD_SCRIPT" >/dev/null ||
  fail "native runtime completeness does not validate the GStreamer package digest"
grep -F "The staged files let Wine's GDI font backend run under Rosetta" \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "stable font dependency provenance text changed"
grep -F 'modern macOS signing and Rosetta compatibility' "$BUILD_SCRIPT" >/dev/null ||
  fail "stable TLS dependency provenance text changed"
if ! /usr/bin/python3 - "$BUILD_SCRIPT" <<'PY'
import sys

text = open(sys.argv[1], encoding="utf-8").read()
required = (
    '''if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  require_command file
  require_command lipo
  require_command otool
  require_command vtool
  require_command xar
fi''',
    'gstreamer_closure_digest="$(runtime_content_tree_digest "$gstreamer_deps_prefix")"',
    'vulkan_closure_digest="$(runtime_content_tree_digest "$vulkan_deps_prefix")"',
    'mesa_closure_digest="$(runtime_content_tree_digest "$mesa_windows_prefix")"',
    'capture_required_output FONT_RUNTIME_PREPARED_ROOT \\\n    prepare_font_runtime_for_install',
    'font_deps_digest="$(content_tree_digest "$FONT_RUNTIME_PREPARED_ROOT")"',
    'font_closure_digest="$(runtime_content_tree_digest "$FONT_RUNTIME_PREPARED_ROOT")"',
    'ditto "$FONT_RUNTIME_PREPARED_ROOT" "$runtime_font_root"',
    'content_tree_is_verified "$runtime_font_root"',
    'verify_host_macho_tree_signatures "$runtime_font_root" "staged font runtime"',
    'remove_prepared_font_runtime "$FONT_RUNTIME_PREPARED_ROOT"',
    'font_assets_closure_digest="$(runtime_content_tree_digest "$font_assets_prefix")"',
    'tls_closure_digest="$(runtime_content_tree_digest "$tls_deps_prefix")"',
    'switchyard_native_runtime_closure_digest \\',
    'switchyard_native_runtime_id_from_closure_digest \\',
    '"$SWITCHYARD_DXMT_ARTIFACT_SHA256" "$SWITCHYARD_DXMT_SOURCE_PATCH_SHA256" \\\n'
    '      "$SWITCHYARD_DXMT_WINEMETAL_ORIGINAL_SHA256" \\\n'
    '      "$DXMT_WOW64_COMPANION_ABI_SCHEMA_SHA256" "$NATIVE_COMPILER_POLICY_IDENTITY"',
    'native_homebrew="/opt/homebrew/bin/brew"',
    '"/opt/homebrew/Cellar/llvm/$SWITCHYARD_NATIVE_LLVM_VERSION/bin"',
    '"--with-mingw=$NATIVE_MINGW_CLANG"',
    'switchyard_qualify_native_macos_sdk \\\n    "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS"',
    'NATIVE_MACOS_SDK_FLAG="$SWITCHYARD_QUALIFIED_NATIVE_MACOS_SDK_FLAG"',
    'configure_cflags="-g -O2 $configure_deployment_flag $configure_sdk_flag"',
    'configure_ldflags_environment="${configure_deployment_flag} ${configure_sdk_flag} ${configure_ldflags}"',
    'reject_ambient_native_compiler_policy || exit $?',
)
for fragment in required:
    if fragment not in text:
        raise SystemExit("missing native-only policy fragment")
PY
then
  fail "native-only build tool and runtime-ID policy is not profile-gated"
fi
if ! /usr/bin/python3 - "$BUILD_SCRIPT" <<'PY'
import sys

text = open(sys.argv[1], encoding="utf-8").read()
stable_expression = (
    'runtime_id="${SWITCHYARD_RUNTIME_PROFILE_ID_PREFIX}${source_identity}'
    '-${gptk_redist_digest}-${wine_mono_digest:0:12}'
    '-${gstreamer_deps_digest}-${vulkan_deps_digest}-${mesa_windows_digest}'
    '-${font_deps_digest}-${font_assets_digest}-${tls_deps_digest}'
    '-${tls_dlopen_digest}"'
)
if text.count(stable_expression) != 1:
    raise SystemExit("stable runtime-ID expression is not literal-identical")
for stale in (
    '${runtime_id}-${SWITCHYARD_UNICORN_SOURCE_REVISION:0:12}',
    'full_content_tree_digest',
    '"runtimeClosure"',
):
    if stale in text:
        raise SystemExit("native compact-ID policy retained a stale fragment: " + stale)

closure = text.index('runtime_closure_digest="$(')
runtime_id = text.index('runtime_id="$(')
default_prefix = text.index('if [ -z "$USER_SET_WINE_INSTALL_PREFIX" ]', runtime_id)
final_budget = text.index(
    'switchyard_validate_native_runtime_prefix_bootstrap_budget \\\n    "$WINE_INSTALL_PREFIX"',
    default_prefix,
)
completion = text.index('runtime_is_complete_at()')
build_mkdir = text.index('mkdir -p "$WINE_BUILD_DIR"')
stage_budget = text.index(
    'switchyard_validate_native_runtime_prefix_bootstrap_budget \\\n    "$FINAL_WINE_INSTALL_PREFIX"',
    build_mkdir,
)
stage_root = text.index('INSTALL_STAGE_ROOT="$(mktemp -d', stage_budget)
final_completeness = text.index('if ! runtime_is_complete_at "$WINE_INSTALL_PREFIX"')
publish_budget = text.index(
    'switchyard_validate_native_runtime_prefix_bootstrap_budget \\\n    "$FINAL_WINE_INSTALL_PREFIX"',
    final_completeness,
)
publication = text.index('atomic_replace_directory ', publish_budget)
if not (
    closure < runtime_id < default_prefix < final_budget < completion
    and build_mkdir < stage_budget < stage_root
    and final_completeness < publish_budget < publication
):
    raise SystemExit("native runtime path or closure validation is out of order")

user_guard = text.index(
    'if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ] &&\n'
    '   [ -n "$USER_SET_WINE_INSTALL_PREFIX" ]; then'
)
dependency_preparation = text.index('gptk_redist_digest="no-gptk"')
if not user_guard < dependency_preparation:
    raise SystemExit("caller WINE_INSTALL_PREFIX is not rejected before dependency work")

manifest_compare = text.index('[ "$manifest_id" = "$runtime_id" ] || return 1')
native_packaging = text.index('switchyard_validate_native_arm64_runtime_packaging', manifest_compare)
if not manifest_compare < native_packaging:
    raise SystemExit("runtime reuse does not bind the recomputed exact native ID")

loader_reuse = text.index(
    '  if [ "$WINE_UNIX_ARCH" = "x86_64" ]; then'
)
loader_reuse_end = text.index(
    '  if ! grep -F "#define SONAME_LIBFREETYPE', loader_reuse
)
if not (
    text.index('HAVE_WINE_PRELOADER', loader_reuse, loader_reuse_end)
    < text.index('-no_huge', loader_reuse, loader_reuse_end)
):
    raise SystemExit("x86_64 loader-reservation reuse policy is incomplete")
PY
then
  fail "native compiler, compact-ID, or path ordering policy is incomplete"
fi
grep -F 'switchyard_stage_native_arm64_dxmt_artifact' "$BUILD_SCRIPT" >/dev/null ||
  fail "native runtime assembly does not stage the pinned DXMT closure"
grep -F 'switchyard_finalize_native_arm64_runtime_manifest' "$BUILD_SCRIPT" >/dev/null ||
  fail "native runtime assembly does not produce the frozen manifest extensions"
grep -F 'switchyard_validate_native_arm64_runtime_packaging' "$BUILD_SCRIPT" >/dev/null ||
  fail "native runtime assembly does not compose the frozen packaging validators"
grep -F 'switchyard_refresh_native_arm64_signed_runtime_manifest' "$BUILD_SCRIPT" >/dev/null ||
  fail "native runtime assembly does not refresh final signed identities"
grep -F -- '--kuser-model "$SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL"' \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "native --ensure publication does not use the exact profile KUSER model"
grep -F 'atomic_replace_directory "$WINE_INSTALL_PREFIX" "$FINAL_WINE_INSTALL_PREFIX" runtime \' \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "runtime publication does not use the profile-aware ownership API"
grep -F '  "$SWITCHYARD_RUNTIME_PROFILE"' "$BUILD_SCRIPT" >/dev/null ||
  fail "runtime publication does not pass the selected profile"
if ! /usr/bin/python3 - "$BUILD_SCRIPT" <<'PY'
import sys

text = open(sys.argv[1], encoding="utf-8").read()
native_source = '''if [ "$NATIVE_CPU_PROVIDER_ENABLED" -eq 1 ]; then
  for native_packaging_library in \\
      "$ROOT_DIR/switchyard/lib/native_arm64_packaging.sh" \\
      "$ROOT_DIR/switchyard/lib/native_cpu_provider.sh" \\
      "$ROOT_DIR/switchyard/lib/dxmt_artifact.sh" \\
      "$ROOT_DIR/switchyard/lib/macho_signing.sh"; do'''
if native_source not in text:
    raise SystemExit("native signing helper is not source-gated")
for call in (
    "switchyard_require_native_arm64_ensure_host",
    "switchyard_sign_preview_native_runtime_entries",
):
    definitions = text.count(call + "()")
    if definitions != 1:
        raise SystemExit(call + " definition is not exact")
if "switchyard_refresh_native_arm64_signed_runtime_manifest()" in text:
    raise SystemExit("build script redefines the producer-owned signed-manifest refresh")

sign_start = text.index("switchyard_sign_preview_native_runtime_entries() {")
sign_end = text.index("\n}\n", sign_start)
sign_body = text[sign_start:sign_end]
for fragment in (
    '"lib/wine/aarch64-unix/wine"',
    '"bin/wine.switchyard-real"',
    "create_validated_entitlements_snapshot",
    "sign_engineering_macho_atomically",
    "close_validated_entitlements_snapshot",
):
    if sign_body.count(fragment) != 1:
        raise SystemExit("native process-entry signing closure is not exact: " + fragment)
if not (
    sign_body.index("create_validated_entitlements_snapshot")
    < sign_body.index("sign_engineering_macho_atomically")
    < sign_body.index("close_validated_entitlements_snapshot")
):
    raise SystemExit("native entitlement snapshot lifecycle is out of order")

cleanup_start = text.index("cleanup_temporary_paths() {")
cleanup_end = text.index("\n}\n", cleanup_start)
cleanup_body = text[cleanup_start:cleanup_end]
if "close_validated_entitlements_snapshot" not in cleanup_body:
    raise SystemExit("native entitlement snapshot has no exit-trap cleanup")

early_publication = '''if [ "$MODE" = "--ensure" ] && runtime_is_complete; then
  switchyard_require_native_arm64_ensure_host
  wine_executable="$FINAL_WINE_INSTALL_PREFIX/bin/switchyard-wine"
  defaults write'''
if text.count(early_publication) != 1:
    raise SystemExit("existing native --ensure publication is not probe-gated")

final_publication = '''atomic_replace_directory "$WINE_INSTALL_PREFIX" "$FINAL_WINE_INSTALL_PREFIX" runtime \\
  "$SWITCHYARD_RUNTIME_PROFILE"'''
if text.count(final_publication) != 1:
    raise SystemExit("final runtime publication is not exact profile-aware form")
final_publication_pos = text.index(final_publication)
final_probe_pos = text.rfind(
    "switchyard_require_native_arm64_ensure_host", 0, final_publication_pos
)
if final_probe_pos < 0:
    raise SystemExit("final runtime publication is not native-host-probe-gated")

cursor = text.index('rm -rf "$WINE_INSTALL_PREFIX/bin/.switchyard-real"')
for label, fragment in (
    ("real process-entry staging", 'mv "$wine_binary_path" "$wine_real_path"'),
    ("process-entry signing", "switchyard_sign_preview_native_runtime_entries"),
    ("process-entry hashing", 'wine_unix_sha256="$(sha256_file'),
    ("base manifest publication", '} >"$WINE_INSTALL_PREFIX/switchyard-runtime.json"'),
    ("native manifest finalization", "switchyard_finalize_native_arm64_runtime_manifest"),
    ("signed identity refresh", "switchyard_refresh_native_arm64_signed_runtime_manifest"),
    ("runtime-bound profile validation", "if ! switchyard_validate_runtime_manifest_profile"),
    ("frozen native validators", "! switchyard_validate_native_arm64_runtime_packaging"),
    ("outer runtime digest", 'runtime_content_sha256="$(write_runtime_content_tree_digest'),
    ("final completeness validation", 'if ! runtime_is_complete_at "$WINE_INSTALL_PREFIX"'),
    ("final strict host probe and profile-aware publication", final_publication),
):
    following = text.find(fragment, cursor)
    if following < 0:
        raise SystemExit("missing or out-of-order native producer step: " + label)
    cursor = following + len(fragment)
PY
then
  fail "native producer/source isolation is not exact"
fi
grep -F 'git -C "$source" ls-files --others --ignored --exclude-standard' \
  "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not reject ignored material in the pinned source checkout"
grep -F '"$SOURCE_DIR"|"$SOURCE_DIR"/*)' "$UNICORN_HELPER" >/dev/null ||
  fail "Unicorn helper does not reject build/output paths inside the pinned source checkout"
grep -F '[ "$ntdll_dependency_count" -eq 1 ]' "$BUILD_SCRIPT" >/dev/null ||
  fail "native provider validation does not require exactly one ntdll dependency"
grep -F '[ "$loader_rpath_count" -eq 1 ]' "$BUILD_SCRIPT" >/dev/null ||
  fail "native provider validation does not require exactly one ntdll loader rpath"
grep -F '[ "$runtime_rpath_count" -eq 1 ]' "$BUILD_SCRIPT" >/dev/null ||
  fail "native provider validation does not require exactly one Unicorn runtime rpath"
grep -F '@loader_path|@loader_path/*|@executable_path|@executable_path/*)' \
  "$BUILD_SCRIPT" >/dev/null ||
  fail "native dependency validation accepts malformed loader/executable-path prefixes"
grep -F "*'/../'*|*'/./'*|*/..|*/.)" "$BUILD_SCRIPT" >/dev/null ||
  fail "native dependency validation does not reject traversing load commands"

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
preview_source_info="$(
  env SWITCHYARD_DISABLE_GPTK_OVERLAY=0 /bin/bash "$BUILD_SCRIPT" \
    --runtime-profile preview-native-arm64-fex --source-info
)"
preview_source_prefix="$(printf '%s\n' "$default_source_info" | sed -n '1,7p')"
expected_preview_source_info="$preview_source_prefix
runtimeProfile=preview-native-arm64-fex
hostMachOArchitecture=arm64
wineUnixArchitecture=aarch64
peArchitectures=aarch64,arm64ec,x86_64,i386
wineBuildTriplet=aarch64-apple-darwin
wineHostTriplet=aarch64-apple-darwin
requiresRosetta=false
minimumMacOS=26.5
gstreamerRegistryArchitecture=arm64
kuserSharedDataModel=translated-shadow"
[ "$preview_source_info" = "$expected_preview_source_info" ] || {
  diff -u <(printf '%s\n' "$expected_preview_source_info") \
    <(printf '%s\n' "$preview_source_info") >&2 || true
  fail "enabled preview source-info is not the exact native profile contract"
}

untrusted_compiler_path="$TEST_ROOT/untrusted-compiler-path"
hostile_brew_sentinel="$TEST_ROOT/hostile-brew-was-executed"
mkdir "$untrusted_compiler_path"
cat >"$untrusted_compiler_path/clang" <<'EOF'
#!/usr/bin/env bash
echo "caller PATH clang must not run" >&2
exit 97
EOF
cp "$untrusted_compiler_path/clang" "$untrusted_compiler_path/clang++"
cat >"$untrusted_compiler_path/brew" <<EOF
#!/usr/bin/env bash
touch "$hostile_brew_sentinel"
echo "caller PATH brew must not run" >&2
exit 96
EOF
chmod 0755 "$untrusted_compiler_path/clang" "$untrusted_compiler_path/clang++" \
  "$untrusted_compiler_path/brew"
hostile_preview_source_info="$(
  env PATH="$untrusted_compiler_path:$PATH" cc=/untrusted/cc \
    cxx=/untrusted/cxx cpp=/untrusted/cpp objc=/untrusted/objc \
    objcxx=/untrusted/objcxx SWITCHYARD_DISABLE_GPTK_OVERLAY=0 \
    /bin/bash "$BUILD_SCRIPT" \
      --runtime-profile preview-native-arm64-fex --source-info
)"
[ "$hostile_preview_source_info" = "$expected_preview_source_info" ] ||
  fail "native source-info relied on caller PATH or lowercase compiler variables"
[ ! -e "$hostile_brew_sentinel" ] ||
  fail "native source-info executed caller PATH brew"
expect_failure "ambient native CFLAGS" 1 \
  "Native Wine build rejects ambient CFLAGS; the profile owns its compiler policy." \
  env CFLAGS=-O0 /bin/bash "$BUILD_SCRIPT" \
    --runtime-profile preview-native-arm64-fex --source-info
expect_failure "ambient native SDKROOT" 1 \
  "Native Wine build rejects ambient SDKROOT; the profile owns its compiler policy." \
  env SDKROOT=/untrusted/sdk /bin/bash "$BUILD_SCRIPT" \
    --runtime-profile preview-native-arm64-fex --source-info
expect_failure "caller native runtime path budget" 1 \
  "InstallHinfSection would publish a 260-UTF-16-code-unit wine.inf command tail; maximum is 259." \
  env WINE_INSTALL_PREFIX="$installhinf_budget_overflow" \
    /bin/bash "$BUILD_SCRIPT" \
      --runtime-profile preview-native-arm64-fex --verify-media

stable_manifest="$TEST_ROOT/stable-runtime.json"
write_stable_manifest "$stable_manifest"
switchyard_validate_runtime_manifest_profile \
  "$stable_manifest" stable-x86_64-rosetta

preview_manifest="$TEST_ROOT/preview-runtime.json"
write_preview_manifest "$preview_manifest"
switchyard_validate_runtime_manifest_profile \
  "$preview_manifest" preview-native-arm64-fex
invalid_manifest="$TEST_ROOT/invalid-runtime.json"

unsigned_runtime="$TEST_ROOT/unsigned-native-runtime"
mkdir "$unsigned_runtime"
cp "$preview_manifest" "$unsigned_runtime/switchyard-runtime.json"
expect_failure "runtime-bound preview without signing identity" 1 \
  "runtimeSigning is required for runtime-bound native validation" \
  switchyard_validate_runtime_manifest_profile \
  "$unsigned_runtime/switchyard-runtime.json" preview-native-arm64-fex \
  "$unsigned_runtime"
ln -s missing-refresh-marker \
  "$unsigned_runtime/.switchyard-signed-manifest-refresh-in-progress"
expect_failure "dangling signed-refresh taint marker" 1 \
  "runtime is tainted by an incomplete signed-manifest refresh" \
  switchyard_validate_runtime_manifest_profile \
  "$unsigned_runtime/switchyard-runtime.json" preview-native-arm64-fex \
  "$unsigned_runtime"
rm "$unsigned_runtime/.switchyard-signed-manifest-refresh-in-progress"
printf '%064d\n' 0 > \
  "$unsigned_runtime/.switchyard-signed-manifest-refresh-in-progress"
chmod 0600 "$unsigned_runtime/.switchyard-signed-manifest-refresh-in-progress"
expect_failure "forged signed-refresh boolean bypass" 1 \
  "runtime is tainted by an incomplete signed-manifest refresh" \
  env SWITCHYARD_NATIVE_SIGNED_REFRESH_IN_PROGRESS=1 /bin/bash -c \
  'source "$1"; switchyard_validate_runtime_manifest_profile "$2" "$3" "$4"' \
  _ "$PROFILE_LIBRARY" "$unsigned_runtime/switchyard-runtime.json" \
  preview-native-arm64-fex "$unsigned_runtime"
rm "$unsigned_runtime/.switchyard-signed-manifest-refresh-in-progress"

stable_manifest_with_unrelated_provider="$TEST_ROOT/stable-runtime-unrelated-provider.json"
cp "$stable_manifest" "$stable_manifest_with_unrelated_provider"
/usr/bin/plutil -insert cpuProvider -string ignored-by-stable-profile \
  "$stable_manifest_with_unrelated_provider"
switchyard_validate_runtime_manifest_profile \
  "$stable_manifest_with_unrelated_provider" stable-x86_64-rosetta

cp "$preview_manifest" "$invalid_manifest"
/usr/bin/plutil -replace cpuProvider.implementation -string fex "$invalid_manifest"
expect_failure "native provider identity" 1 \
  "cpuProvider.implementation must identify Unicorn" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" preview-native-arm64-fex

cp "$preview_manifest" "$invalid_manifest"
/usr/bin/plutil -remove cpuProvider.kuserSharedDataModel "$invalid_manifest"
expect_failure "missing native KUSER_SHARED_DATA model" 1 \
  "manifest is not valid unambiguous JSON data" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" preview-native-arm64-fex

for invalid_kuser_model in direct unknown; do
  cp "$preview_manifest" "$invalid_manifest"
  /usr/bin/plutil -replace cpuProvider.kuserSharedDataModel \
    -string "$invalid_kuser_model" "$invalid_manifest"
  expect_failure "invalid native KUSER_SHARED_DATA model $invalid_kuser_model" 1 \
    "cpuProvider.kuserSharedDataModel does not match the native profile" \
    switchyard_validate_runtime_manifest_profile \
    "$invalid_manifest" preview-native-arm64-fex
done

cp "$preview_manifest" "$invalid_manifest"
/usr/bin/plutil -replace cpuProvider.runtimePayloadDigest \
  -string 0000000000000000000000000000000000000000000000000000000000000000 \
  "$invalid_manifest"
expect_failure "native provider payload digest" 1 \
  "cpuProvider.runtimePayloadDigest requires runtime-bound validation" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" preview-native-arm64-fex

cp "$preview_manifest" "$invalid_manifest"
/usr/bin/plutil -replace cpuProvider.components.0.unixLibrary \
  -string lib/wine/aarch64-unix/untrusted.so "$invalid_manifest"
expect_failure "native provider component layout" 1 \
  "cpuProvider.components is not the exact provider layout" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" preview-native-arm64-fex

cp "$preview_manifest" "$invalid_manifest"
/usr/bin/plutil -replace cpuProvider.components.0.unixLibrarySha256 \
  -string malformed "$invalid_manifest"
expect_failure "native provider component digest type" 1 \
  "manifest is not valid unambiguous JSON data" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" preview-native-arm64-fex

cp "$preview_manifest" "$invalid_manifest"
/usr/bin/plutil -insert cpuProvider.unexpected -string rejected "$invalid_manifest"
expect_failure "native provider unexpected field" 1 \
  "manifest is not valid unambiguous JSON data" \
  switchyard_validate_runtime_manifest_profile \
  "$invalid_manifest" preview-native-arm64-fex

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
expect_failure "unsigned native preview release" 1 \
  "runtimeSigning is required for runtime-bound native validation" \
  "$RELEASE_SCRIPT" --runtime "$release_runtime" \
  --runtime-content-sha256 "$(printf '%064d' 0)" \
  --output "$release_output" --identity -
[ ! -e "$release_output" ] || fail "unsigned native preview release created an output directory"

echo "runtime profile tests passed"
