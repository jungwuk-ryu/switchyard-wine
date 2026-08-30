#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
PROVIDER_LIBRARY="$ROOT_DIR/switchyard/lib/native_cpu_provider.sh"
DXMT_ACCEPTANCE_TEST="$ROOT_DIR/switchyard/tests/dxmt_d3d11_acceptance_test.sh"
DIGEST_HELPER="$ROOT_DIR/switchyard/runtime_content_digest.py"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-native-provider.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"
RUNTIME="$TEST_ROOT/runtime"
MANIFEST="$RUNTIME/switchyard-runtime.json"

cleanup() {
  local status=$?

  trap - EXIT HUP INT TERM
  case "$TEST_ROOT" in
    /private/tmp/switchyard-native-provider.??????)
      [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected native-provider fixture root $TEST_ROOT" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "native CPU-provider fixture failed: $1" >&2
  exit 1
}

sha256_file() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

file_id() {
  /usr/bin/stat -f '%d:%i' "$1"
}

expect_failure() {
  local description="$1"
  shift

  if "$@" >"$TEST_ROOT/rejected.out" 2>"$TEST_ROOT/rejected.err"; then
    fail "$description was accepted"
  fi
}

restore_manifest() {
  /bin/cp "$TEST_ROOT/manifest.good" "$MANIFEST"
}

refresh_component_digest() {
  local guest="$1"
  local field="$2"
  local path="$3"

  /usr/bin/python3 -I - "$MANIFEST" "$guest" "$field" "$(sha256_file "$path")" <<'PY'
import json
import os
import sys
import tempfile

manifest, guest, field, digest = sys.argv[1:]
with open(manifest, "r", encoding="utf-8") as stream:
    value = json.load(stream)
matches = [
    component
    for component in value["cpuProvider"]["components"]
    if component["guestArchitecture"] == guest
]
if len(matches) != 1:
    raise SystemExit("fixture provider component selection is ambiguous")
matches[0][field] = digest
descriptor, temporary = tempfile.mkstemp(prefix=".manifest.", dir=os.path.dirname(manifest))
try:
    os.fchmod(descriptor, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        descriptor = -1
        json.dump(value, stream, ensure_ascii=True, indent=2)
        stream.write("\n")
    os.replace(temporary, manifest)
    temporary = None
finally:
    if descriptor >= 0:
        os.close(descriptor)
    if temporary is not None:
        os.unlink(temporary)
PY
}

refresh_policy_digest() {
  local module="$1"
  local path="$2"

  /usr/bin/python3 -I - "$MANIFEST" "$module" "$(sha256_file "$path")" <<'PY'
import json
import os
import sys
import tempfile

manifest, module, digest = sys.argv[1:]
with open(manifest, "r", encoding="utf-8") as stream:
    value = json.load(stream)
matches = [item for item in value["wow64UnixlibPolicy"]["auditedModules"] if item["module"] == module]
if len(matches) != 1:
    raise SystemExit("fixture policy module selection is ambiguous")
matches[0]["sha256"] = digest
descriptor, temporary = tempfile.mkstemp(prefix=".manifest.", dir=os.path.dirname(manifest))
try:
    os.fchmod(descriptor, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        descriptor = -1
        json.dump(value, stream, ensure_ascii=True, indent=2)
        stream.write("\n")
    os.replace(temporary, manifest)
    temporary = None
finally:
    if descriptor >= 0:
        os.close(descriptor)
    if temporary is not None:
        os.unlink(temporary)
PY
}

[ "$#" -eq 0 ] || {
  echo "usage: $0" >&2
  exit 2
}
[ "$(/usr/bin/uname -s)" = Darwin ] || fail "fixtures require macOS"
[ "$(/usr/bin/uname -m)" = arm64 ] || fail "fixtures require a native arm64 shell"
for source_file in \
    "$PROFILE_LIBRARY" "$PROVIDER_LIBRARY" "$DXMT_ACCEPTANCE_TEST" "$DIGEST_HELPER"; do
  [ -f "$source_file" ] && [ ! -L "$source_file" ] ||
    fail "required policy source is missing or unsafe: $source_file"
done
# shellcheck disable=SC1090 # Fixed repository-relative policy libraries.
source "$PROFILE_LIBRARY"
# shellcheck disable=SC1090
source "$PROVIDER_LIBRARY"
switchyard_load_runtime_profile preview-native-arm64-fex

[ "$SWITCHYARD_NATIVE_XTAJIT64_ABI_VERSION" = 10 ] ||
  fail "x64 provider validator ABI version is not v10"
[ "$(/usr/bin/grep -Fc "$SWITCHYARD_NATIVE_XTAJIT64_ABI_IDENTITY" \
  "$ROOT_DIR/dlls/xtajit64/unixlib.h")" -eq 1 ] ||
  fail "x64 provider header and validator ABI identities differ"

[ "$(/usr/bin/grep -Fxc \
  "  \"\$manifest\" preview-native-arm64-fex \"\$RUNTIME\"" \
  "$DXMT_ACCEPTANCE_TEST")" -eq 2 ] ||
  fail "DXMT acceptance does not bind both profile checks to its runtime root"
REPO_SOURCE_PATCH="$ROOT_DIR/switchyard/patches/$(basename "$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH")"
[ -f "$REPO_SOURCE_PATCH" ] && [ ! -L "$REPO_SOURCE_PATCH" ] ||
  fail "pinned Unicorn source patch is missing or unsafe: $REPO_SOURCE_PATCH"
[ "$(sha256_file "$REPO_SOURCE_PATCH")" = "$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH_SHA256" ] ||
  fail "pinned Unicorn source patch digest is not the validator contract"
[ "$(sha256_file "$ROOT_DIR/dlls/crypt32/unixlib.c")" = \
  7c2dab33bc8166286af5d8d48388c68eb83a095176ed25025e0740fd828fab3d ] ||
  fail "crypt32 v2-reviewed source differs from its frozen identity"
[ "$(sha256_file "$ROOT_DIR/dlls/secur32/schannel_gnutls.c")" = \
  8806af8a5217a580c2c980ac58f0561287f88e9807b9457d007dcf42cccedde2 ] ||
  fail "secur32 v2-reviewed source differs from its frozen identity"

UNICORN_CACHE="${SWITCHYARD_UNICORN_FIXTURE_CACHE:-${HOME}/.switchyard/deps/cpu-provider/unicorn-${SWITCHYARD_UNICORN_VERSION}-${SWITCHYARD_UNICORN_SOURCE_REVISION:0:12}-build${SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION}-arm64-macos-${SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS}}"
[ -d "$UNICORN_CACHE" ] && [ ! -L "$UNICORN_CACHE" ] ||
  fail "pinned Unicorn fixture cache is missing: $UNICORN_CACHE"
/usr/bin/python3 -I "$DIGEST_HELPER" verify "$UNICORN_CACHE" ||
  fail "pinned Unicorn fixture cache failed content verification"

/bin/mkdir -m 700 "$RUNTIME"
/bin/mkdir -p \
  "$RUNTIME/lib/wine/aarch64-unix" \
  "$RUNTIME/lib/wine/aarch64-windows" \
  "$RUNTIME/lib/switchyard-unicorn/lib" \
  "$RUNTIME/lib/switchyard-unicorn/share/doc/switchyard-unicorn" \
  "$RUNTIME/lib/switchyard-unicorn/share/src/switchyard-unicorn"
/bin/chmod 0755 \
  "$RUNTIME/lib" "$RUNTIME/lib/wine" \
  "$RUNTIME/lib/wine/aarch64-unix" "$RUNTIME/lib/wine/aarch64-windows" \
  "$RUNTIME/lib/switchyard-unicorn" "$RUNTIME/lib/switchyard-unicorn/lib" \
  "$RUNTIME/lib/switchyard-unicorn/share" \
  "$RUNTIME/lib/switchyard-unicorn/share/doc" \
  "$RUNTIME/lib/switchyard-unicorn/share/doc/switchyard-unicorn" \
  "$RUNTIME/lib/switchyard-unicorn/share/src" \
  "$RUNTIME/lib/switchyard-unicorn/share/src/switchyard-unicorn"

UNICORN_PACKAGE="$RUNTIME/lib/switchyard-unicorn"
/usr/bin/install -m 0755 "$UNICORN_CACHE/lib/libunicorn.2.dylib" \
  "$UNICORN_PACKAGE/lib/libunicorn.2.dylib"
/bin/ln -s libunicorn.2.dylib "$UNICORN_PACKAGE/lib/libunicorn.dylib"
/bin/chmod -h 0755 "$UNICORN_PACKAGE/lib/libunicorn.dylib"
/usr/bin/install -m 0644 "$UNICORN_CACHE/switchyard-unicorn-runtime.json" \
  "$UNICORN_PACKAGE/switchyard-unicorn-runtime.json"
for notice in README.txt CORRESPONDING-SOURCE.txt COPYING COPYING.LGPL2 COPYING_GLIB \
    QEMU-COPYING QEMU-COPYING.LIB QEMU-LICENSE; do
  /usr/bin/install -m 0644 \
    "$UNICORN_CACHE/share/doc/switchyard-unicorn/$notice" \
    "$UNICORN_PACKAGE/share/doc/switchyard-unicorn/$notice"
done
UNICORN_ARCHIVE="unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz"
/usr/bin/install -m 0644 \
  "$UNICORN_CACHE/share/src/switchyard-unicorn/$UNICORN_ARCHIVE" \
  "$UNICORN_PACKAGE/share/src/switchyard-unicorn/$UNICORN_ARCHIVE"
/usr/bin/install -m 0644 "$REPO_SOURCE_PATCH" \
  "$RUNTIME/$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH"
/usr/bin/python3 -I "$DIGEST_HELPER" write "$UNICORN_PACKAGE" >"$TEST_ROOT/payload.digest"
FIXTURE_PAYLOAD_DIGEST="$(<"$TEST_ROOT/payload.digest")"
[ "$FIXTURE_PAYLOAD_DIGEST" = "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" ] ||
  fail "pruned fixture payload digest is $FIXTURE_PAYLOAD_DIGEST, expected $SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST"

/bin/cat >"$TEST_ROOT/ntdll.c" <<'EOF'
int ntdll_fixture(void) { return 7; }
EOF
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/ntdll.so \
  "$TEST_ROOT/ntdll.c" -o "$RUNTIME/lib/wine/aarch64-unix/ntdll.so"

/bin/cat >"$TEST_ROOT/provider.c" <<'EOF'
extern int ntdll_fixture(void);
extern unsigned int uc_version(unsigned int *, unsigned int *);
struct uc_struct;
typedef int (*switchyard_unicorn_extension_t)(struct uc_struct *);
extern int uc_emu_stop_at_instruction_boundary(struct uc_struct *);
extern int uc_enable_shared_memory_atomics(struct uc_struct *);
#ifdef XTAJIT64_PROVIDER
extern int uc_clear_instruction_boundary_stop(struct uc_struct *);
extern int uc_set_shared_memory_atomic_callback(struct uc_struct *);
#endif
__attribute__((used, visibility("default")))
switchyard_unicorn_extension_t const switchyard_unicorn_fixture_imports[] = {
    uc_emu_stop_at_instruction_boundary,
    uc_enable_shared_memory_atomics,
#ifdef XTAJIT64_PROVIDER
    uc_clear_instruction_boundary_stop,
    uc_set_shared_memory_atomic_callback,
#endif
};
__attribute__((used, visibility("default"))) const char
    switchyard_xtajit64_fixture_abi_identity[] =
        "switchyard-xtajit64-provider-abi-v10-flight-bind-process-init-96-begin-472-doorbell";
__attribute__((visibility("default"))) unsigned int provider_fixture(void)
{
    return (unsigned int)ntdll_fixture() + uc_version(0, 0);
}
EOF
for provider_name in xtajit xtajit64; do
  provider_define=
  [ "$provider_name" != xtajit64 ] || provider_define=-DXTAJIT64_PROVIDER
  /usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
    -mmacosx-version-min=26.5 \
    ${provider_define:+"$provider_define"} \
    -Wl,-install_name,"@rpath/$provider_name.so" \
    -Wl,-rpath,@loader_path/ \
    -Wl,-rpath,@loader_path/../../switchyard-unicorn/lib \
    "$TEST_ROOT/provider.c" \
    "$RUNTIME/lib/wine/aarch64-unix/ntdll.so" \
    "$UNICORN_PACKAGE/lib/libunicorn.2.dylib" \
    -o "$RUNTIME/lib/wine/aarch64-unix/$provider_name.so"
done

/usr/bin/python3 -I - "$RUNTIME" \
  "$SWITCHYARD_NATIVE_XTAJIT_PE_LIBRARY" \
  "$SWITCHYARD_NATIVE_XTAJIT64_PE_LIBRARY" \
  "$SWITCHYARD_NATIVE_XTAJIT64_ABI_IDENTITY" <<'PY'
import os
import struct
import sys

root, xtajit, xtajit64, x64_abi_identity = sys.argv[1:]


def write_pe(relative, machine, arm64ec=False, abi_identity=None):
    value = bytearray(0x1200 if arm64ec else 0x98)
    value[:2] = b"MZ"
    struct.pack_into("<I", value, 0x3C, 0x80)
    value[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", value, 0x84, machine)
    struct.pack_into("<H", value, 0x96, 0x2000)
    if arm64ec:
        struct.pack_into("<H", value, 0x86, 1)
        struct.pack_into("<H", value, 0x94, 0xF0)
        optional = 0x98
        struct.pack_into("<H", value, optional, 0x20B)
        struct.pack_into("<Q", value, optional + 24, 0x180000000)
        struct.pack_into("<I", value, optional + 108, 16)
        struct.pack_into("<II", value, optional + 112 + 10 * 8, 0x1000, 0xD0)
        section = 0x188
        value[section:section + 8] = b".rdata\0\0"
        struct.pack_into("<IIII", value, section + 8, 0x1000, 0x1000, 0x1000, 0x200)
        struct.pack_into("<I", value, 0x200, 0xD0)
        struct.pack_into("<Q", value, 0x200 + 0xC8, 0x180001100)
        struct.pack_into("<III", value, 0x300, 2, 0x1120, 1)
        struct.pack_into("<II", value, 0x320, 0x2001, 0x100)
    if abi_identity is not None:
        value.extend(abi_identity.encode("ascii") + b"\0")
    path = os.path.join(root, relative)
    with open(path, "xb") as stream:
        stream.write(value)
    # Wine installs PE modules as data (0644); only the native Unix provider
    # library must carry host execute bits.
    os.chmod(path, 0o644)


write_pe(xtajit, 0xAA64)
write_pe(xtajit64, 0x8664, True, x64_abi_identity)
PY

/bin/cat >"$TEST_ROOT/policy.c" <<'EOF'
#include <stdint.h>
typedef int32_t (*unixlib_entry_t)(void *);
struct dispatch_entry_v2 { uint32_t args_size; uint32_t flags; };
struct dispatch_source_v2
{
    uint32_t version, size, entry_count, entry_size;
    const unixlib_entry_t *funcs;
    const struct dispatch_entry_v2 *entries;
    uint32_t flags, reserved;
};
static int32_t fixture_call(void *args) { return args ? 0 : 0; }
__attribute__((visibility("default"))) const unixlib_entry_t
    __wine_unix_call_wow64_funcs[] = {fixture_call};
static const struct dispatch_entry_v2 entries[] = {{4, 1}};
__attribute__((visibility("default"))) const struct dispatch_source_v2
    __wine_unix_call_wow64_dispatch_v2 =
        {2, sizeof(struct dispatch_source_v2), 1, sizeof(struct dispatch_entry_v2),
         __wine_unix_call_wow64_funcs, entries, 0, 0};
EOF
for module in crypt32 dwrite secur32 winemac ws2_32; do
  /usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
    -mmacosx-version-min=26.5 -Wl,-install_name,"@rpath/$module.so" \
    "$TEST_ROOT/policy.c" -o "$RUNTIME/lib/wine/aarch64-unix/$module.so"
done
/bin/cp "$RUNTIME/lib/wine/aarch64-unix/crypt32.so" \
  "$RUNTIME/lib/wine/aarch64-unix/winemetal-wow64.so"

XTAJIT_UNIX_SHA="$(sha256_file "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY")"
XTAJIT_PE_SHA="$(sha256_file "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_PE_LIBRARY")"
XTAJIT64_UNIX_SHA="$(sha256_file "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT64_UNIX_LIBRARY")"
XTAJIT64_PE_SHA="$(sha256_file "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT64_PE_LIBRARY")"
CRYPT32_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-unix/crypt32.so")"
DWRITE_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-unix/dwrite.so")"
SECUR32_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-unix/secur32.so")"
WINEMAC_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-unix/winemac.so")"
WS2_32_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-unix/ws2_32.so")"

/usr/bin/python3 -I - "$MANIFEST" \
  "$SWITCHYARD_UNICORN_VERSION" \
  "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY" \
  "$SWITCHYARD_UNICORN_SOURCE_REVISION" \
  "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" \
  "$SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION" \
  "$SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST" \
  "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" \
  "$SWITCHYARD_UNICORN_LIBRARY_SHA256" \
  "$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH" \
  "$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH_SHA256" \
  "$XTAJIT_UNIX_SHA" "$XTAJIT_PE_SHA" \
  "$XTAJIT64_UNIX_SHA" "$XTAJIT64_PE_SHA" \
  "$CRYPT32_SHA" "$DWRITE_SHA" "$SECUR32_SHA" \
  "$WINEMAC_SHA" "$WS2_32_SHA" <<'PY'
import json
import sys

(
    output,
    version,
    repository,
    revision,
    archive_digest,
    build_contract,
    development_digest,
    payload_digest,
    library_digest,
    source_patch,
    source_patch_digest,
    xtajit_unix_digest,
    xtajit_pe_digest,
    xtajit64_unix_digest,
    xtajit64_pe_digest,
    crypt32_digest,
    dwrite_digest,
    secur32_digest,
    winemac_digest,
    ws2_32_digest,
) = sys.argv[1:]
value = {
    "manifestVersion": 2,
    "id": "switchyard-local-native-arm64-fex-provider-fixture",
    "runtimeFamily": "preview-native-arm64-fex",
    "buildProfile": "switchyard-native-arm64-fex",
    "host": {
        "platform": "macos",
        "architecture": "arm64",
        "wineUnixArchitecture": "aarch64",
        "buildTriplet": "aarch64-apple-darwin",
        "hostTriplet": "aarch64-apple-darwin",
        "architectureCommand": ["arch", "-arm64"],
        "requiresRosetta": False,
        "minimumMacOS": "26.5",
        "gstreamerRegistryArchitecture": "arm64",
    },
    "peArchitectures": ["aarch64", "arm64ec", "x86_64", "i386"],
    "cpuProvider": {
        "implementation": "unicorn",
        "version": version,
        "sourceRepository": repository,
        "sourceRevision": revision,
        "sourceArchive": (
            "lib/switchyard-unicorn/share/src/switchyard-unicorn/"
            f"unicorn-{revision}.tar.gz"
        ),
        "sourceArchiveSha256": archive_digest,
        "sourcePatch": {"path": source_patch, "sha256": source_patch_digest},
        "buildContractVersion": int(build_contract),
        "hostArchitecture": "arm64",
        "kuserSharedDataModel": "translated-shadow",
        "emulatedArchitectures": ["i386", "x86_64"],
        "developmentCacheDigest": development_digest,
        "runtimeRoot": "lib/switchyard-unicorn",
        "runtimePayloadDigest": payload_digest,
        "library": "lib/switchyard-unicorn/lib/libunicorn.2.dylib",
        "librarySha256": library_digest,
        "providerUnixLibraries": [
            "lib/wine/aarch64-unix/xtajit.so",
            "lib/wine/aarch64-unix/xtajit64.so",
        ],
        "components": [
            {
                "guestArchitecture": "i386",
                "unixLibrary": "lib/wine/aarch64-unix/xtajit.so",
                "unixLibrarySha256": xtajit_unix_digest,
                "peLibrary": "lib/wine/aarch64-windows/xtajit.dll",
                "peLibrarySha256": xtajit_pe_digest,
            },
            {
                "guestArchitecture": "x86_64",
                "unixLibrary": "lib/wine/aarch64-unix/xtajit64.so",
                "unixLibrarySha256": xtajit64_unix_digest,
                "peLibrary": "lib/wine/aarch64-windows/xtajit64.dll",
                "peLibrarySha256": xtajit64_pe_digest,
            },
        ],
        "runtimeRpath": "@loader_path/../../switchyard-unicorn/lib",
        "manifest": "lib/switchyard-unicorn/switchyard-unicorn-runtime.json",
    },
    "wow64UnixlibPolicy": {
        "contractVersion": 2,
        "handleEncoding": "generation-tagged-v1",
        "internalDispatch": {"module": "ntdll", "sourceVersion": 1},
        "externalSourceVersion": 2,
        "requiredEntryFlag": "REVIEWED",
        "auditedModules": [
            {
                "module": "crypt32",
                "unixLibrary": "lib/wine/aarch64-unix/crypt32.so",
                "sha256": crypt32_digest,
            },
            {
                "module": "dwrite",
                "unixLibrary": "lib/wine/aarch64-unix/dwrite.so",
                "sha256": dwrite_digest,
            },
            {
                "module": "secur32",
                "unixLibrary": "lib/wine/aarch64-unix/secur32.so",
                "sha256": secur32_digest,
            },
            {
                "module": "winemac",
                "unixLibrary": "lib/wine/aarch64-unix/winemac.so",
                "sha256": winemac_digest,
            },
            {
                "module": "ws2_32",
                "unixLibrary": "lib/wine/aarch64-unix/ws2_32.so",
                "sha256": ws2_32_digest,
            },
        ],
    },
    "dxmt": {
        "wow64Companion": {
            "path": "lib/wine/aarch64-unix/winemetal-wow64.so",
        },
    },
}
with open(output, "x", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, ensure_ascii=True, indent=2)
    stream.write("\n")
PY
/bin/chmod 0644 "$MANIFEST"
/bin/cp "$MANIFEST" "$TEST_ROOT/manifest.good"

PROFILE_BINDING_LOG="$TEST_ROOT/profile-binding.log"
SIGNED_BINDING_MANIFEST="$TEST_ROOT/signed-binding-runtime.json"
switchyard_validate_runtime_manifest_profile() {
  /usr/bin/printf '%s\t%s\n' "$#" "${3:-}" >>"$PROFILE_BINDING_LOG"
}
switchyard_native_cpu_provider_validate_runtime_profile "$MANIFEST" "$RUNTIME"
/bin/cp "$MANIFEST" "$SIGNED_BINDING_MANIFEST"
/usr/bin/plutil -insert runtimeSigning -json '{"mode":"engineering-adhoc"}' \
  "$SIGNED_BINDING_MANIFEST"
switchyard_native_cpu_provider_validate_runtime_profile \
  "$SIGNED_BINDING_MANIFEST" "$RUNTIME"
[ "$(/usr/bin/sed -n '1p' "$PROFILE_BINDING_LOG")" = $'2\t' ] ||
  fail "unsigned provider metadata validation unexpectedly bound a runtime root"
[ "$(/usr/bin/sed -n '2p' "$PROFILE_BINDING_LOG")" = $'3\t'"$RUNTIME" ] ||
  fail "signed provider validation did not bind its exact runtime root"
[ "$(/usr/bin/wc -l <"$PROFILE_BINDING_LOG" | /usr/bin/tr -d ' ')" -eq 2 ] ||
  fail "provider profile-binding fixture observed unexpected validation calls"
# Restore the production profile validator for the complete unsigned fixture;
# runtime_profile_test.sh separately owns signed process-entry validation.
# shellcheck disable=SC1090
source "$PROFILE_LIBRARY"

switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"

semantic_provider="$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT64_UNIX_LIBRARY"
/bin/cp "$semantic_provider" "$TEST_ROOT/xtajit64.semantic-good"
/bin/cat >"$TEST_ROOT/provider-without-switchyard-api.c" <<'EOF'
extern int ntdll_fixture(void);
extern unsigned int uc_version(unsigned int *, unsigned int *);
__attribute__((used, visibility("default"))) const char
    switchyard_xtajit64_fixture_abi_identity[] =
        "switchyard-xtajit64-provider-abi-v10-flight-bind-process-init-96-begin-472-doorbell";
__attribute__((visibility("default"))) unsigned int provider_fixture(void)
{
    return (unsigned int)ntdll_fixture() + uc_version(0, 0);
}
EOF
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/xtajit64.so \
  -Wl,-rpath,@loader_path/ \
  -Wl,-rpath,@loader_path/../../switchyard-unicorn/lib \
  "$TEST_ROOT/provider-without-switchyard-api.c" \
  "$RUNTIME/lib/wine/aarch64-unix/ntdll.so" \
  "$UNICORN_PACKAGE/lib/libunicorn.2.dylib" \
  -o "$semantic_provider"
refresh_component_digest x86_64 unixLibrarySha256 "$semantic_provider"
expect_failure "provider without required Switchyard Unicorn imports" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
/usr/bin/grep -F 'does not import the required Switchyard Unicorn API' \
  "$TEST_ROOT/rejected.err" >/dev/null ||
  fail "semantic provider rejection did not identify its missing Unicorn API"
/bin/mv "$TEST_ROOT/xtajit64.semantic-good" "$semantic_provider"
restore_manifest
switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"

for abi_relative in \
    "$SWITCHYARD_NATIVE_XTAJIT64_UNIX_LIBRARY" \
    "$SWITCHYARD_NATIVE_XTAJIT64_PE_LIBRARY"; do
  abi_live="$RUNTIME/$abi_relative"
  abi_saved="$TEST_ROOT/$(/usr/bin/basename "$abi_relative").abi-good"
  /bin/cp "$abi_live" "$abi_saved"
  /usr/bin/python3 -I - "$abi_live" "$SWITCHYARD_NATIVE_XTAJIT64_ABI_IDENTITY" <<'PY'
import os
import sys

path, identity = sys.argv[1:]
with open(path, "rb") as stream:
    value = stream.read()
old = identity.encode("ascii")
new = old.replace(b"-v10-", b"-v9-", 1)
if new == old or value.count(old) != 1:
    raise SystemExit("fixture x64 provider ABI marker is not unique")
with open(path, "wb") as stream:
    stream.write(value.replace(old, new, 1))
PY
  case "$abi_relative" in
    "$SWITCHYARD_NATIVE_XTAJIT64_UNIX_LIBRARY")
      refresh_component_digest x86_64 unixLibrarySha256 "$abi_live"
      ;;
    "$SWITCHYARD_NATIVE_XTAJIT64_PE_LIBRARY")
      refresh_component_digest x86_64 peLibrarySha256 "$abi_live"
      ;;
  esac
  expect_failure "ABI-incompatible x64 provider pair $abi_relative" \
    switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
  /bin/mv "$abi_saved" "$abi_live"
  restore_manifest
done
switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"

/bin/chmod 0644 "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY"
expect_failure "non-executable provider Unix library" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
/bin/chmod 0755 "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY"
switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"

/usr/bin/plutil -remove cpuProvider.kuserSharedDataModel "$MANIFEST"
expect_failure "missing CPU-provider KUSER model" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
restore_manifest
for invalid_kuser_model in direct unknown; do
  /usr/bin/plutil -replace cpuProvider.kuserSharedDataModel \
    -string "$invalid_kuser_model" "$MANIFEST"
  expect_failure "invalid CPU-provider KUSER model $invalid_kuser_model" \
    switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
  restore_manifest
done

ABA_TOOL_ROOT="$TEST_ROOT/aba-tools"
ABA_TOOL_LOG="$TEST_ROOT/aba-tools.log"
ABA_SWAP_MARKER="$TEST_ROOT/aba-lipo-swap"
ABA_DIGEST_MARKER="$TEST_ROOT/aba-digest-swap"
ABA_INVALID="$TEST_ROOT/aba-invalid.bin"
ABA_DIGEST_INVALID="$TEST_ROOT/aba-invalid-readme.txt"
/bin/mkdir -m 700 "$ABA_TOOL_ROOT"
: >"$ABA_TOOL_LOG"
/usr/bin/printf 'not a Mach-O image\n' >"$ABA_INVALID"
/bin/chmod 0755 "$ABA_INVALID"
/usr/bin/printf 'temporarily invalid payload\n' >"$ABA_DIGEST_INVALID"
/bin/cat >"$TEST_ROOT/aba-inspection-tool" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

tool="$(/usr/bin/basename "$0")"
real_tool="/usr/bin/$tool"
swapped=0
restore_live() {
  if [ "$swapped" -eq 1 ]; then
    /bin/mv "$SWITCHYARD_ABA_LIVE" "$SWITCHYARD_ABA_REPLACEMENT"
    /bin/mv "$SWITCHYARD_ABA_LIVE.saved" "$SWITCHYARD_ABA_LIVE"
    swapped=0
  fi
}
trap restore_live EXIT HUP INT TERM
if [ "$tool" = lipo ] && /bin/mkdir "$SWITCHYARD_ABA_SWAP_MARKER" 2>/dev/null; then
  /bin/mv "$SWITCHYARD_ABA_LIVE" "$SWITCHYARD_ABA_LIVE.saved"
  /bin/mv "$SWITCHYARD_ABA_REPLACEMENT" "$SWITCHYARD_ABA_LIVE"
  swapped=1
fi
for argument in "$@"; do
  case "$argument" in
    "$SWITCHYARD_ABA_RUNTIME"/*)
      echo "inspection tool received a live runtime path: $argument" >&2
      exit 91
      ;;
  esac
done
/usr/bin/printf '%s\t%s\n' "$tool" "$*" >>"$SWITCHYARD_ABA_TOOL_LOG"
set +e
"$real_tool" "$@"
status=$?
set -e
restore_live
exit "$status"
EOF
for tool in codesign lipo nm otool vtool; do
  /usr/bin/install -m 0755 "$TEST_ROOT/aba-inspection-tool" "$ABA_TOOL_ROOT/$tool"
done
/bin/cat >"$TEST_ROOT/aba-digest-helper.py" <<'PY'
#!/usr/bin/env python3
import os
import subprocess
import sys

action, target = sys.argv[1:]
runtime = os.environ["SWITCHYARD_ABA_RUNTIME"]
log = os.environ["SWITCHYARD_ABA_TOOL_LOG"]
with open(log, "a", encoding="utf-8") as stream:
    stream.write(f"digest-{action}\t{target}\n")
if target == os.environ["SWITCHYARD_ABA_PACKAGE"] or target.startswith(runtime + os.sep):
    raise SystemExit("digest helper received the live Unicorn package path")
marker = os.environ["SWITCHYARD_ABA_DIGEST_MARKER"]
swapped = False
try:
    os.mkdir(marker, 0o700)
    live = os.environ["SWITCHYARD_ABA_DIGEST_LIVE"]
    replacement = os.environ["SWITCHYARD_ABA_DIGEST_REPLACEMENT"]
    saved = live + ".saved"
    os.replace(live, saved)
    os.replace(replacement, live)
    swapped = True
except FileExistsError:
    pass
try:
    result = subprocess.run(
        ["/usr/bin/python3", "-I", os.environ["SWITCHYARD_ABA_REAL_DIGEST"], action, target],
        check=False,
    )
finally:
    if swapped:
        os.replace(live, replacement)
        os.replace(saved, live)
raise SystemExit(result.returncode)
PY
/bin/chmod 0755 "$TEST_ROOT/aba-digest-helper.py"

switchyard_native_cpu_provider_inspection_tool() {
  /usr/bin/printf '%s/%s\n' "$ABA_TOOL_ROOT" "$1"
}
switchyard_native_cpu_provider_content_digest_helper() {
  /usr/bin/printf '%s\n' "$TEST_ROOT/aba-digest-helper.py"
}
export SWITCHYARD_ABA_RUNTIME="$RUNTIME"
export SWITCHYARD_ABA_PACKAGE="$UNICORN_PACKAGE"
export SWITCHYARD_ABA_TOOL_LOG="$ABA_TOOL_LOG"
export SWITCHYARD_ABA_SWAP_MARKER="$ABA_SWAP_MARKER"
export SWITCHYARD_ABA_DIGEST_MARKER="$ABA_DIGEST_MARKER"
export SWITCHYARD_ABA_REAL_DIGEST="$DIGEST_HELPER"
export SWITCHYARD_ABA_DIGEST_LIVE="$UNICORN_PACKAGE/share/doc/switchyard-unicorn/README.txt"
export SWITCHYARD_ABA_DIGEST_REPLACEMENT="$ABA_DIGEST_INVALID"
export SWITCHYARD_ABA_LIVE="$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY"
export SWITCHYARD_ABA_REPLACEMENT="$ABA_INVALID"
provider_before="$(sha256_file "$SWITCHYARD_ABA_LIVE")"
payload_before="$(sha256_file "$SWITCHYARD_ABA_DIGEST_LIVE")"
provider_id_before="$(file_id "$SWITCHYARD_ABA_LIVE")"
payload_id_before="$(file_id "$SWITCHYARD_ABA_DIGEST_LIVE")"
switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
[ -d "$ABA_SWAP_MARKER" ] && [ -d "$ABA_DIGEST_MARKER" ] ||
  fail "provider ABA swap hooks did not execute"
[ "$(sha256_file "$SWITCHYARD_ABA_LIVE")" = "$provider_before" ] ||
  fail "provider Mach-O ABA fixture did not restore the exact live inode bytes"
[ "$(sha256_file "$SWITCHYARD_ABA_DIGEST_LIVE")" = "$payload_before" ] ||
  fail "Unicorn payload ABA fixture did not restore the exact live bytes"
[ "$(file_id "$SWITCHYARD_ABA_LIVE")" = "$provider_id_before" ] &&
  [ "$(file_id "$SWITCHYARD_ABA_DIGEST_LIVE")" = "$payload_id_before" ] ||
  fail "provider ABA fixture did not restore the original live inodes"

/bin/rmdir "$ABA_SWAP_MARKER"
export SWITCHYARD_ABA_LIVE="$RUNTIME/lib/wine/aarch64-unix/dwrite.so"
policy_before="$(sha256_file "$SWITCHYARD_ABA_LIVE")"
policy_id_before="$(file_id "$SWITCHYARD_ABA_LIVE")"
switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
[ -d "$ABA_SWAP_MARKER" ] || fail "policy ABA swap hook did not execute"
[ "$(sha256_file "$SWITCHYARD_ABA_LIVE")" = "$policy_before" ] ||
  fail "audited-module ABA fixture did not restore the exact live bytes"
[ "$(file_id "$SWITCHYARD_ABA_LIVE")" = "$policy_id_before" ] ||
  fail "audited-module ABA fixture did not restore the original live inode"
for tool in codesign lipo nm otool vtool digest-verify digest-digest; do
  /usr/bin/grep -q "^$tool"$'\t' "$ABA_TOOL_LOG" ||
    fail "private-snapshot ABA fixture did not exercise $tool"
done

# Restore the production-only dependency resolvers before negative fixtures.
# shellcheck disable=SC1090
source "$PROVIDER_LIBRARY"
unset SWITCHYARD_ABA_RUNTIME SWITCHYARD_ABA_PACKAGE SWITCHYARD_ABA_TOOL_LOG
unset SWITCHYARD_ABA_SWAP_MARKER SWITCHYARD_ABA_DIGEST_MARKER
unset SWITCHYARD_ABA_REAL_DIGEST SWITCHYARD_ABA_DIGEST_LIVE
unset SWITCHYARD_ABA_DIGEST_REPLACEMENT SWITCHYARD_ABA_LIVE SWITCHYARD_ABA_REPLACEMENT

/usr/bin/plutil -insert cpuProvider.sourcePatch.unexpected -string rejected "$MANIFEST"
expect_failure "unexpected CPU-provider source patch field" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
restore_manifest

/usr/bin/python3 -I - "$MANIFEST" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    value = json.load(stream)
del value["cpuProvider"]["sourcePatch"]
with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, ensure_ascii=True, indent=2)
    stream.write("\n")
PY
expect_failure "missing CPU-provider source patch identity" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
restore_manifest

/usr/bin/printf 'tampered qualification patch\n' >>"$RUNTIME/$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH"
expect_failure "tampered CPU-provider source patch" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
/usr/bin/install -m 0644 "$REPO_SOURCE_PATCH" \
  "$RUNTIME/$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH"

/bin/mv "$RUNTIME/$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH" "$TEST_ROOT/source-patch.good"
/bin/ln -s "$TEST_ROOT/source-patch.good" "$RUNTIME/$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH"
expect_failure "symbolic-link CPU-provider source patch" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
/bin/rm "$RUNTIME/$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH"
/bin/mv "$TEST_ROOT/source-patch.good" "$RUNTIME/$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH"

expect_failure "policy validation without a source root" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST"
/bin/cp "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY" "$TEST_ROOT/xtajit-unix.good"

/usr/bin/python3 -I - "$MANIFEST" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as stream:
    value = json.load(stream)
del value["wow64UnixlibPolicy"]
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(value, stream)
PY
expect_failure "missing manifest policy" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
restore_manifest

/usr/bin/plutil -insert wow64UnixlibPolicy.unexpected -string rejected "$MANIFEST"
expect_failure "unexpected policy field" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
restore_manifest

/usr/bin/python3 -I - "$MANIFEST" <<'PY'
import sys
path = sys.argv[1]
with open(path, "r", encoding="utf-8") as stream:
    value = stream.read()
needle = '    "contractVersion": 2,\n'
if value.count(needle) != 1:
    raise SystemExit("fixture policy contract field changed")
with open(path, "w", encoding="utf-8") as stream:
    stream.write(value.replace(needle, needle + needle, 1))
PY
expect_failure "duplicate manifest policy key" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
restore_manifest

/usr/bin/python3 -I - "$MANIFEST" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as stream:
    value = json.load(stream)
value["wow64UnixlibPolicy"]["auditedModules"].reverse()
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(value, stream)
PY
expect_failure "unsorted policy module list" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
restore_manifest

/usr/bin/plutil -replace wow64UnixlibPolicy.handleEncoding -string pointer-v1 "$MANIFEST"
expect_failure "non-generation-tagged policy" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
restore_manifest

/usr/bin/printf 'tamper\n' >>"$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY"
expect_failure "tampered provider Unix library" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
/bin/mv "$TEST_ROOT/xtajit-unix.good" \
  "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY"

/bin/mkdir -p "$RUNTIME/lib/wine/x86_64-unix"
/bin/cp "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY" \
  "$RUNTIME/lib/wine/x86_64-unix/xtajit.so"
expect_failure "extra Rosetta provider artifact" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
/bin/rm -f "$RUNTIME/lib/wine/x86_64-unix/xtajit.so"
/bin/rmdir "$RUNTIME/lib/wine/x86_64-unix"

/usr/bin/printf 'extra\n' >"$UNICORN_PACKAGE/share/doc/switchyard-unicorn/EXTRA"
expect_failure "extra Unicorn package artifact" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
/bin/rm -f "$UNICORN_PACKAGE/share/doc/switchyard-unicorn/EXTRA"

/bin/cp "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_PE_LIBRARY" "$TEST_ROOT/xtajit.good"
/usr/bin/python3 -I - "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_PE_LIBRARY" <<'PY'
import struct
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.seek(0x84)
    stream.write(struct.pack("<H", 0x014C))
PY
refresh_component_digest i386 peLibrarySha256 \
  "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_PE_LIBRARY"
expect_failure "wrong provider PE machine" \
  switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
/bin/mv "$TEST_ROOT/xtajit.good" "$RUNTIME/$SWITCHYARD_NATIVE_XTAJIT_PE_LIBRARY"
restore_manifest

/bin/mv "$RUNTIME/lib/wine/aarch64-unix/winemac.so" "$TEST_ROOT/winemac.good"
/bin/ln -s "$TEST_ROOT/winemac.good" "$RUNTIME/lib/wine/aarch64-unix/winemac.so"
expect_failure "symbolic-link audited module" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/rm -f "$RUNTIME/lib/wine/aarch64-unix/winemac.so"
/bin/mv "$TEST_ROOT/winemac.good" "$RUNTIME/lib/wine/aarch64-unix/winemac.so"

/bin/cp "$RUNTIME/lib/wine/aarch64-unix/dwrite.so" "$TEST_ROOT/dwrite-signature.good"
/usr/bin/printf 'invalid-signature-tail\n' >>"$RUNTIME/lib/wine/aarch64-unix/dwrite.so"
refresh_policy_digest dwrite "$RUNTIME/lib/wine/aarch64-unix/dwrite.so"
expect_failure "invalid audited-module code signature" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/mv "$TEST_ROOT/dwrite-signature.good" "$RUNTIME/lib/wine/aarch64-unix/dwrite.so"
restore_manifest

/bin/cp "$RUNTIME/lib/wine/aarch64-unix/winemac.so" \
  "$RUNTIME/lib/wine/aarch64-unix/unaudited-v2.so"
expect_failure "unlisted external v2 dispatch module" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/rm -f "$RUNTIME/lib/wine/aarch64-unix/unaudited-v2.so"

/bin/cat >"$TEST_ROOT/policy-v1.c" <<'EOF'
#include <stdint.h>
typedef int32_t (*unixlib_entry_t)(void *);
struct dispatch_source_v1
{
    uint32_t version, size, entry_count, reserved;
    const unixlib_entry_t *funcs;
};
static int32_t fixture_call(void *args) { return args ? 0 : 0; }
__attribute__((visibility("default"))) const unixlib_entry_t
    __wine_unix_call_wow64_funcs[] = {fixture_call};
__attribute__((visibility("default"))) const struct dispatch_source_v1
    __wine_unix_call_wow64_dispatch_v1 =
        {1, sizeof(struct dispatch_source_v1), 1, 0, __wine_unix_call_wow64_funcs};
EOF
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/unaudited-v1.so \
  "$TEST_ROOT/policy-v1.c" -o "$RUNTIME/lib/wine/aarch64-unix/unaudited-v1.so"
expect_failure "unlisted external v1 dispatch module" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/rm -f "$RUNTIME/lib/wine/aarch64-unix/unaudited-v1.so"

/bin/cp "$RUNTIME/lib/wine/aarch64-unix/dwrite.so" "$TEST_ROOT/dwrite.good"
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/dwrite.so \
  "$TEST_ROOT/policy-v1.c" -o "$RUNTIME/lib/wine/aarch64-unix/dwrite.so"
refresh_policy_digest dwrite "$RUNTIME/lib/wine/aarch64-unix/dwrite.so"
expect_failure "external v1 dispatch module" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/mv "$TEST_ROOT/dwrite.good" "$RUNTIME/lib/wine/aarch64-unix/dwrite.so"
restore_manifest

SOURCE_FIXTURE="$TEST_ROOT/source"
for relative in \
    include/wine/unixlib.h \
    dlls/ntdll/unix/loader.c \
    dlls/ntdll/unix/virtual.c \
    dlls/crypt32/unixlib.c \
    dlls/dwrite/freetype.c \
    dlls/secur32/schannel_gnutls.c \
    dlls/winemac.drv/macdrv_main.c \
    dlls/ws2_32/unixlib.c \
    dlls/winemetal-wow64/unixlib.c; do
  /bin/mkdir -p "$SOURCE_FIXTURE/$(dirname "$relative")"
  /bin/cp "$ROOT_DIR/$relative" "$SOURCE_FIXTURE/$relative"
done
switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$SOURCE_FIXTURE"
/usr/bin/python3 -I - "$SOURCE_FIXTURE/include/wine/unixlib.h" <<'PY'
import sys
path = sys.argv[1]
with open(path, "r", encoding="utf-8") as stream:
    value = stream.read()
old = "#define WINE_UNIXLIB_DISPATCH_GENERATION_MASK 0xfffffffffffffull"
if value.count(old) != 1:
    raise SystemExit("fixture generation mask source changed")
with open(path, "w", encoding="utf-8") as stream:
    stream.write(value.replace(old, "#define WINE_UNIXLIB_DISPATCH_GENERATION_MASK 0xffffffffffffffull"))
PY
expect_failure "inconsistent generation-tagged source constants" \
  switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$SOURCE_FIXTURE"

switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME"
switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR"

/usr/bin/grep -F 'winemetal-wow64.so' "$PROVIDER_LIBRARY" >/dev/null ||
  fail "DXMT companion is absent from the exact v2 export inventory"
if /usr/bin/grep -E 'abi-schema-v3|companion_v3' "$PROVIDER_LIBRARY" >/dev/null; then
  fail "retired companion schema leaked into the provider validator"
fi

echo "native CPU-provider and generation-tagged v2 policy fixtures passed"
