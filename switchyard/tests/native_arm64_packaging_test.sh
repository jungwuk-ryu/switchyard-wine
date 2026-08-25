#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
PACKAGING_LIBRARY="$ROOT_DIR/switchyard/lib/native_arm64_packaging.sh"
PROVIDER_LIBRARY="$ROOT_DIR/switchyard/lib/native_cpu_provider.sh"
DXMT_LIBRARY="$ROOT_DIR/switchyard/lib/dxmt_artifact.sh"
SIGNING_LIBRARY="$ROOT_DIR/switchyard/lib/macho_signing.sh"
DIGEST_HELPER="$ROOT_DIR/switchyard/runtime_content_digest.py"
NATIVE_ENTITLEMENTS="$ROOT_DIR/switchyard/wine-runtime-native-arm64.entitlements"
BUILD_RUNTIME="$ROOT_DIR/switchyard/build_runtime.sh"
DXMT_ARCHIVE="${1:-}"
DXMT_SOURCE="${2:-}"

[ "$#" -eq 2 ] || {
  echo "usage: $0 PINNED_DXMT_ARCHIVE PINNED_DXMT_SOURCE" >&2
  exit 2
}
for input in "$DXMT_ARCHIVE" "$DXMT_SOURCE"; do
  case "$input" in
    /*) ;;
    *) echo "native packaging fixtures must use absolute paths" >&2; exit 2 ;;
  esac
done

TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-native-packaging.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"
RUNTIME="$TEST_ROOT/runtime"
MANIFEST="$RUNTIME/switchyard-runtime.json"
failure_index=0

cleanup() {
  local status=$?

  trap - EXIT HUP INT TERM
  case "$TEST_ROOT" in
    /private/tmp/switchyard-native-packaging.??????)
      [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected packaging test root: $TEST_ROOT" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "native ARM64 packaging fixture failed: $1" >&2
  exit 1
}

expect_failure() {
  local label="$1"
  local output
  shift

  failure_index=$((failure_index + 1))
  output="$TEST_ROOT/failure-$failure_index.log"
  if "$@" >"$output" 2>&1; then
    fail "$label was accepted"
  fi
}

sha256_file() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

# Exercise the exact build_runtime.sh producer semantics instead of maintaining
# a second test-only approximation of the short dependency-tree digest.
[ -f "$BUILD_RUNTIME" ] && [ ! -L "$BUILD_RUNTIME" ] ||
  fail "build-runtime digest producer is missing or unsafe"
BUILD_DIGEST_FUNCTIONS="$TEST_ROOT/build-runtime-content-digest.sh"
{
  /usr/bin/sed -n '/^sha256_file() {$/,/^}$/p' \
    "$BUILD_RUNTIME"
  /usr/bin/sed -n '/^short_sha256_stream() {$/,/^}$/p' \
    "$BUILD_RUNTIME"
  /usr/bin/sed -n '/^content_tree_digest() {$/,/^}$/p' \
    "$BUILD_RUNTIME"
} >"$BUILD_DIGEST_FUNCTIONS"
# shellcheck disable=SC1090 # Extracted from the fixed repository build producer.
source "$BUILD_DIGEST_FUNCTIONS"
declare -F content_tree_digest >/dev/null ||
  fail "could not load the build-runtime dependency digest producer"

write_fixture_content_marker() {
  local root="$1"

  content_tree_digest "$root" >"$root/.switchyard-content-sha256"
  /bin/chmod 0644 "$root/.switchyard-content-sha256"
}

for library in \
    "$PROFILE_LIBRARY" "$PACKAGING_LIBRARY" "$PROVIDER_LIBRARY" \
    "$DXMT_LIBRARY" "$SIGNING_LIBRARY" "$DIGEST_HELPER" \
    "$NATIVE_ENTITLEMENTS"; do
  [ -f "$library" ] && [ ! -L "$library" ] ||
    fail "required packaging policy is missing or unsafe: $library"
done
# shellcheck disable=SC1090 # Fixed repository-relative policy libraries.
source "$PROFILE_LIBRARY"
# shellcheck disable=SC1090
source "$PACKAGING_LIBRARY"
# shellcheck disable=SC1090
source "$PROVIDER_LIBRARY"
# shellcheck disable=SC1090
source "$DXMT_LIBRARY"
# shellcheck disable=SC1090
source "$SIGNING_LIBRARY"
switchyard_load_runtime_profile preview-native-arm64-fex
[ "$SWITCHYARD_RUNTIME_PROFILE_ENABLED" = 1 ] ||
  fail "preview profile is not enabled for native packaging acceptance"

[ -f "$DXMT_ARCHIVE" ] && [ ! -L "$DXMT_ARCHIVE" ] ||
  fail "pinned DXMT archive fixture is missing or unsafe"
[ -d "$DXMT_SOURCE" ] && [ ! -L "$DXMT_SOURCE" ] ||
  fail "pinned DXMT source fixture is missing or unsafe"

COMPANION_SOURCE_FIXTURE="$TEST_ROOT/companion-source"
for relative in \
    configure.ac configure \
    include/wine/unixlib.h \
    dlls/winemetal-wow64/Makefile.in \
    dlls/winemetal-wow64/adapter.c \
    dlls/winemetal-wow64/buffer.h \
    dlls/winemetal-wow64/buffer.m \
    dlls/winemetal-wow64/commands.c \
    dlls/winemetal-wow64/unixlib.c \
    dlls/winemetal-wow64/winemetal_private.h \
    dlls/winemetal-wow64/abi-schema-v6.txt; do
  /bin/mkdir -p "$COMPANION_SOURCE_FIXTURE/$(dirname "$relative")"
  /bin/cp "$ROOT_DIR/$relative" "$COMPANION_SOURCE_FIXTURE/$relative"
done
[ "$(switchyard_validate_dxmt_wow64_companion_source "$COMPANION_SOURCE_FIXTURE")" = \
  "$SWITCHYARD_DXMT_WOW64_ABI_SCHEMA_SHA256" ] ||
  fail "exact DXMT WoW64 companion source preflight did not return its schema identity"
/bin/cp "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/Makefile.in" \
  "$TEST_ROOT/companion-Makefile.good"
/usr/bin/sed -i '' '/CoreFoundation/d' \
  "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/Makefile.in"
expect_failure "incomplete companion framework closure" \
  switchyard_validate_dxmt_wow64_companion_source "$COMPANION_SOURCE_FIXTURE"
/bin/cp "$TEST_ROOT/companion-Makefile.good" \
  "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/Makefile.in"
/bin/cp "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/abi-schema-v6.txt" \
  "$TEST_ROOT/abi-schema-v6.good"
/usr/bin/printf 'tampered\n' >> \
  "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/abi-schema-v6.txt"
expect_failure "tampered companion ABI schema source" \
  switchyard_validate_dxmt_wow64_companion_source "$COMPANION_SOURCE_FIXTURE"
/bin/cp "$TEST_ROOT/abi-schema-v6.good" \
  "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/abi-schema-v6.txt"
/bin/mv "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/abi-schema-v6.txt" \
  "$TEST_ROOT/abi-schema-v6.target"
/bin/ln -s "$TEST_ROOT/abi-schema-v6.target" \
  "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/abi-schema-v6.txt"
expect_failure "symbolic-link companion ABI schema source" \
  switchyard_validate_dxmt_wow64_companion_source "$COMPANION_SOURCE_FIXTURE"
/bin/rm "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/abi-schema-v6.txt"
/bin/mv "$TEST_ROOT/abi-schema-v6.target" \
  "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/abi-schema-v6.txt"
/bin/mv "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/buffer.m" \
  "$TEST_ROOT/buffer.m.target"
/bin/ln -s "$TEST_ROOT/buffer.m.target" \
  "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/buffer.m"
expect_failure "symbolic-link companion implementation source" \
  switchyard_validate_dxmt_wow64_companion_source "$COMPANION_SOURCE_FIXTURE"
/bin/rm "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/buffer.m"
/bin/mv "$TEST_ROOT/buffer.m.target" \
  "$COMPANION_SOURCE_FIXTURE/dlls/winemetal-wow64/buffer.m"
/bin/cp "$COMPANION_SOURCE_FIXTURE/include/wine/unixlib.h" \
  "$TEST_ROOT/unixlib.h.good"
/usr/bin/sed -i '' 's/{ 0x00,/{ 0x01,/' \
  "$COMPANION_SOURCE_FIXTURE/include/wine/unixlib.h"
expect_failure "mismatched companion ABI digest header" \
  switchyard_validate_dxmt_wow64_companion_source "$COMPANION_SOURCE_FIXTURE"
/bin/cp "$TEST_ROOT/unixlib.h.good" \
  "$COMPANION_SOURCE_FIXTURE/include/wine/unixlib.h"
/bin/mv "$COMPANION_SOURCE_FIXTURE/include/wine/unixlib.h" \
  "$TEST_ROOT/unixlib.h.missing"
expect_failure "missing companion ABI digest header" \
  switchyard_validate_dxmt_wow64_companion_source "$COMPANION_SOURCE_FIXTURE"
/bin/mv "$TEST_ROOT/unixlib.h.missing" \
  "$COMPANION_SOURCE_FIXTURE/include/wine/unixlib.h"

UNICORN_CACHE="${SWITCHYARD_UNICORN_FIXTURE_CACHE:-${HOME}/.switchyard/deps/cpu-provider/unicorn-${SWITCHYARD_UNICORN_VERSION}-${SWITCHYARD_UNICORN_SOURCE_REVISION:0:12}-build${SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION}-arm64-macos-${SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS}}"
[ -d "$UNICORN_CACHE" ] && [ ! -L "$UNICORN_CACHE" ] ||
  fail "pinned Unicorn fixture cache is missing: $UNICORN_CACHE"
/usr/bin/python3 -I "$DIGEST_HELPER" verify "$UNICORN_CACHE" ||
  fail "pinned Unicorn fixture cache failed content verification"

/bin/mkdir -m 700 "$RUNTIME"
/bin/mkdir -p \
  "$RUNTIME/bin" \
  "$RUNTIME/lib/wine/aarch64-unix" \
  "$RUNTIME/lib/wine/aarch64-windows" \
  "$RUNTIME/lib/wine/i386-windows" \
  "$RUNTIME/lib/wine/x86_64-windows" \
  "$RUNTIME/lib/switchyard-unicorn/lib" \
  "$RUNTIME/lib/switchyard-unicorn/share/doc/switchyard-unicorn" \
  "$RUNTIME/lib/switchyard-unicorn/share/src/switchyard-unicorn"
/bin/chmod 0755 \
  "$RUNTIME/bin" "$RUNTIME/lib" "$RUNTIME/lib/wine" \
  "$RUNTIME/lib/wine/aarch64-unix" "$RUNTIME/lib/wine/aarch64-windows" \
  "$RUNTIME/lib/wine/i386-windows" "$RUNTIME/lib/wine/x86_64-windows" \
  "$RUNTIME/lib/switchyard-unicorn" "$RUNTIME/lib/switchyard-unicorn/lib" \
  "$RUNTIME/lib/switchyard-unicorn/share" \
  "$RUNTIME/lib/switchyard-unicorn/share/doc" \
  "$RUNTIME/lib/switchyard-unicorn/share/doc/switchyard-unicorn" \
  "$RUNTIME/lib/switchyard-unicorn/share/src" \
  "$RUNTIME/lib/switchyard-unicorn/share/src/switchyard-unicorn"

UNICORN_PACKAGE="$RUNTIME/lib/switchyard-unicorn"
UNICORN_MANIFEST="$UNICORN_PACKAGE/switchyard-unicorn-runtime.json"
/usr/bin/install -m 0755 "$UNICORN_CACHE/lib/libunicorn.2.dylib" \
  "$UNICORN_PACKAGE/lib/libunicorn.2.dylib"
/bin/ln -s libunicorn.2.dylib "$UNICORN_PACKAGE/lib/libunicorn.dylib"
/bin/chmod -h 0755 "$UNICORN_PACKAGE/lib/libunicorn.dylib"
/usr/bin/install -m 0644 "$UNICORN_CACHE/switchyard-unicorn-runtime.json" \
  "$UNICORN_PACKAGE/switchyard-unicorn-runtime.json"
for notice in README.txt CORRESPONDING-SOURCE.txt COPYING COPYING.LGPL2 COPYING_GLIB \
    QEMU-COPYING QEMU-COPYING.LIB QEMU-LICENSE; do
  /usr/bin/install -m 0644 "$UNICORN_CACHE/share/doc/switchyard-unicorn/$notice" \
    "$UNICORN_PACKAGE/share/doc/switchyard-unicorn/$notice"
done
UNICORN_SOURCE_ARCHIVE="unicorn-${SWITCHYARD_UNICORN_SOURCE_REVISION}.tar.gz"
/usr/bin/install -m 0644 \
  "$UNICORN_CACHE/share/src/switchyard-unicorn/$UNICORN_SOURCE_ARCHIVE" \
  "$UNICORN_PACKAGE/share/src/switchyard-unicorn/$UNICORN_SOURCE_ARCHIVE"
/usr/bin/install -m 0644 \
  "$UNICORN_CACHE/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" \
  "$UNICORN_PACKAGE/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"
fixture_payload_digest="$(/usr/bin/python3 -I "$DIGEST_HELPER" write "$UNICORN_PACKAGE")"
[ "$fixture_payload_digest" = "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" ] ||
  fail "fixture Unicorn payload digest $fixture_payload_digest does not match $SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST"

/bin/cat >"$TEST_ROOT/ntdll.c" <<'EOF'
int ntdll_fixture(void) { return 7; }
EOF
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/ntdll.so \
  "$TEST_ROOT/ntdll.c" -o "$RUNTIME/lib/wine/aarch64-unix/ntdll.so"

/bin/cat >"$TEST_ROOT/provider.c" <<'EOF'
extern int ntdll_fixture(void);
extern unsigned int uc_version(unsigned int *, unsigned int *);
__attribute__((used, visibility("default"))) const char
    switchyard_xtajit64_fixture_abi_identity[] =
        "switchyard-xtajit64-provider-abi-v8-flight-bind-process-init-80-begin-464";
__attribute__((visibility("default"))) unsigned int provider_fixture(void)
{
    return (unsigned int)ntdll_fixture() + uc_version(0, 0);
}
EOF
for provider in xtajit xtajit64; do
  /usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
    -mmacosx-version-min=26.5 -Wl,-install_name,"@rpath/$provider.so" \
    -Wl,-rpath,@loader_path/ \
    -Wl,-rpath,@loader_path/../../switchyard-unicorn/lib \
    "$TEST_ROOT/provider.c" "$RUNTIME/lib/wine/aarch64-unix/ntdll.so" \
    "$UNICORN_PACKAGE/lib/libunicorn.2.dylib" \
    -o "$RUNTIME/lib/wine/aarch64-unix/$provider.so"
done

/usr/bin/python3 -I - "$RUNTIME" \
  "$SWITCHYARD_NATIVE_XTAJIT64_ABI_IDENTITY" <<'PY'
import os
import struct
import sys

root, x64_abi_identity = sys.argv[1:]


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
    os.chmod(path, 0o755)


write_pe("lib/wine/aarch64-windows/xtajit.dll", 0xAA64)
write_pe("lib/wine/aarch64-windows/xtajit64.dll", 0x8664, True,
         x64_abi_identity)
write_pe("lib/wine/i386-windows/ntdll.dll", 0x014C)
write_pe("lib/wine/x86_64-windows/ntdll.dll", 0x8664)
PY

/bin/cat >"$TEST_ROOT/entry.c" <<'EOF'
int main(void) { return 0; }
EOF
for entry in \
    "$RUNTIME/lib/wine/aarch64-unix/wine" \
    "$RUNTIME/bin/wine.switchyard-real"; do
  /usr/bin/xcrun --sdk macosx clang -arch arm64 -O2 -Wall -Wextra -Werror \
    -mmacosx-version-min=26.5 "$TEST_ROOT/entry.c" -o "$entry"
done
ENTRY_SNAPSHOT_FD=""
create_validated_entitlements_snapshot preview-native-arm64-fex \
  "$NATIVE_ENTITLEMENTS" "$TEST_ROOT" ENTRY_SNAPSHOT_FD
for entry in \
    "$RUNTIME/lib/wine/aarch64-unix/wine" \
    "$RUNTIME/bin/wine.switchyard-real"; do
  sign_engineering_macho_atomically /usr/bin/codesign \
    preview-native-arm64-fex "$entry" "$ENTRY_SNAPSHOT_FD"
done
close_validated_entitlements_snapshot "$ENTRY_SNAPSHOT_FD"
ENTRY_SNAPSHOT_FD=""

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
#define FIXTURE_IMMUTABLE_EXPORT __attribute__((visibility("default"), section("__DATA_CONST,__const")))
static int32_t fixture_call(void *args) { return args ? 0 : 0; }
FIXTURE_IMMUTABLE_EXPORT const unixlib_entry_t
    __wine_unix_call_wow64_funcs[] = {fixture_call};
static const struct dispatch_entry_v2 entries[] = {{4, 1}};
FIXTURE_IMMUTABLE_EXPORT const struct dispatch_source_v2
    __wine_unix_call_wow64_dispatch_v2 =
        {2, sizeof(struct dispatch_source_v2), 1, sizeof(struct dispatch_entry_v2),
         __wine_unix_call_wow64_funcs, entries, 0, 0};
EOF
for module in crypt32 dwrite secur32 winemac ws2_32; do
  /usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
    -mmacosx-version-min=26.5 -Wl,-install_name,"@rpath/$module.so" \
    "$TEST_ROOT/policy.c" -o "$RUNTIME/lib/wine/aarch64-unix/$module.so"
done
/bin/cat >"$TEST_ROOT/companion.c" <<'EOF'
#include "policy.c"
extern int ntdll_fixture(void);
extern void *NSClassFromString(void *);
extern void *MTLCreateSystemDefaultDevice(void);
extern void *objc_getClass(const char *);
__attribute__((constructor)) static void companion_dependencies(void)
{
    volatile const void *dependencies[] = {
        (const void *)(uintptr_t)ntdll_fixture,
        (const void *)(uintptr_t)NSClassFromString,
        (const void *)(uintptr_t)MTLCreateSystemDefaultDevice,
        (const void *)(uintptr_t)objc_getClass,
    };
    (void)dependencies;
}
struct companion_descriptor_v6
{
    uint32_t version, size, entry_count, flags;
    unsigned char abi_sha256[32];
    const void *bind;
    const void *quiesce;
    const void *unbind;
};
FIXTURE_IMMUTABLE_EXPORT const struct companion_descriptor_v6
    __wine_unix_call_wow64_companion_v6 = {
        6, sizeof(struct companion_descriptor_v6), 138, 0,
        {0x00, 0x51, 0xbd, 0x8c, 0x0b, 0xc3, 0xe3, 0xce,
         0x26, 0x1e, 0x9d, 0x50, 0x07, 0x66, 0x53, 0x42,
         0xac, 0x2d, 0x28, 0xa6, 0x43, 0x57, 0x67, 0x44,
         0xd8, 0xec, 0x71, 0x89, 0x6a, 0xf8, 0x56, 0xf1},
        __wine_unix_call_wow64_funcs,
        __wine_unix_call_wow64_funcs,
        __wine_unix_call_wow64_funcs,
    };
EOF
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -nostdlib -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/winemetal-wow64.so \
  -Wl,-rpath,@loader_path/ -Wl,-rpath,"$TEST_ROOT/build-only-dependency" \
  -I"$TEST_ROOT" "$TEST_ROOT/companion.c" \
  "$RUNTIME/lib/wine/aarch64-unix/ntdll.so" \
  -framework Foundation -framework Metal -framework CoreFoundation -lSystem -lobjc \
  -o "$RUNTIME/lib/wine/aarch64-unix/winemetal-wow64.so"
COMPANION_RPATHS_BEFORE="$(/usr/bin/otool -l \
  "$RUNTIME/lib/wine/aarch64-unix/winemetal-wow64.so" | /usr/bin/awk \
  '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')"
/usr/bin/grep -Fx "$TEST_ROOT/build-only-dependency" \
  <<<"$COMPANION_RPATHS_BEFORE" >/dev/null ||
  fail "companion RPATH regression fixture did not contain its build-only path"
switchyard_normalize_native_arm64_dxmt_companion_rpaths "$RUNTIME"
[ "$(/usr/bin/otool -l "$RUNTIME/lib/wine/aarch64-unix/winemetal-wow64.so" | \
  /usr/bin/awk '/cmd LC_RPATH/{found=1; next} found && /path /{print $2; found=0}')" = \
  '@loader_path/' ] || fail "companion RPATH normalization did not produce the exact closure"
/usr/bin/codesign --force --sign - \
  "$RUNTIME/lib/wine/aarch64-unix/winemetal-wow64.so"

DEPENDENCY_FIELDS=(gstreamerRuntime vulkanRuntime fontRuntime tlsRuntime)
DEPENDENCY_ROOTS=(
  "$RUNTIME/lib/switchyard-gstreamer"
  "$RUNTIME/lib/switchyard-vulkan"
  "$RUNTIME/lib/switchyard-fonts"
  "$RUNTIME/lib/switchyard-tls"
)
DEPENDENCY_MACHOS=()
DEPENDENCY_DIGESTS=()
for index in "${!DEPENDENCY_ROOTS[@]}"; do
  dependency_root="${DEPENDENCY_ROOTS[$index]}"
  /bin/mkdir -p "$dependency_root/lib" "$dependency_root/share/fixture"
  /usr/bin/install -m 0755 "$RUNTIME/lib/wine/aarch64-unix/crypt32.so" \
    "$dependency_root/lib/fixture-$index.dylib"
  /usr/bin/printf 'dependency-%s\n' "${DEPENDENCY_FIELDS[$index]}" \
    >"$dependency_root/share/fixture/identity.txt"
  /bin/ln -s "fixture-$index.dylib" \
    "$dependency_root/lib/fixture-current.dylib"
  write_fixture_content_marker "$dependency_root"
  DEPENDENCY_MACHOS+=("$dependency_root/lib/fixture-$index.dylib")
  DEPENDENCY_DIGESTS+=("$(content_tree_digest "$dependency_root")")
done
# Cover mode preservation rather than assuming every producer marker is 0644.
/bin/chmod 0600 "$RUNTIME/lib/switchyard-fonts/.switchyard-content-sha256"

MESA_ROOT="$RUNTIME/lib/switchyard-mesa"
FONT_ASSETS_ROOT="$RUNTIME/share/wine/fonts"
/bin/mkdir -p "$MESA_ROOT/x86_64-windows" "$FONT_ASSETS_ROOT"
/usr/bin/printf 'mesa-identity\n' >"$MESA_ROOT/x86_64-windows/opengl32.dll"
/usr/bin/printf 'font-asset-identity\n' >"$FONT_ASSETS_ROOT/fixture.ttf"
write_fixture_content_marker "$MESA_ROOT"
MESA_DIGEST="$(content_tree_digest "$MESA_ROOT")"
FONT_ASSETS_DIGEST="$(content_tree_digest "$FONT_ASSETS_ROOT")"

XTAJIT_UNIX_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-unix/xtajit.so")"
XTAJIT_PE_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-windows/xtajit.dll")"
XTAJIT64_UNIX_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-unix/xtajit64.so")"
XTAJIT64_PE_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-windows/xtajit64.dll")"
WINE_UNIX_SHA="$(sha256_file "$RUNTIME/lib/wine/aarch64-unix/wine")"
WINE_REAL_SHA="$(sha256_file "$RUNTIME/bin/wine.switchyard-real")"
I386_NTDLL_SHA="$(sha256_file "$RUNTIME/lib/wine/i386-windows/ntdll.dll")"
X86_64_NTDLL_SHA="$(sha256_file "$RUNTIME/lib/wine/x86_64-windows/ntdll.dll")"

/usr/bin/python3 -I - "$MANIFEST" \
  "$SWITCHYARD_UNICORN_VERSION" "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY" \
  "$SWITCHYARD_UNICORN_SOURCE_REVISION" "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" \
  "$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" \
  "$SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION" \
  "$SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST" \
  "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" "$SWITCHYARD_UNICORN_LIBRARY_SHA256" \
  "$XTAJIT_UNIX_SHA" "$XTAJIT_PE_SHA" "$XTAJIT64_UNIX_SHA" "$XTAJIT64_PE_SHA" \
  "$WINE_UNIX_SHA" "$WINE_REAL_SHA" "$I386_NTDLL_SHA" "$X86_64_NTDLL_SHA" \
  "${DEPENDENCY_DIGESTS[@]}" "$MESA_DIGEST" "$FONT_ASSETS_DIGEST" <<'PY'
import json
import sys

(
    output,
    version,
    repository,
    revision,
    archive_digest,
    patch_basename,
    patch_digest,
    build_contract,
    development_digest,
    payload_digest,
    library_digest,
    xtajit_unix_digest,
    xtajit_pe_digest,
    xtajit64_unix_digest,
    xtajit64_pe_digest,
    wine_unix_digest,
    wine_real_digest,
    i386_ntdll_digest,
    x86_64_ntdll_digest,
    gstreamer_digest,
    vulkan_digest,
    font_digest,
    tls_digest,
    mesa_digest,
    font_assets_digest,
) = sys.argv[1:]
value = {
    "manifestVersion": 2,
    "id": "switchyard-local-native-arm64-fex-packaging-fixture",
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
    "integrity": {
        "wineUnixSha256": wine_unix_digest,
        "i386NtdllSha256": i386_ntdll_digest,
        "x86_64NtdllSha256": x86_64_ntdll_digest,
    },
    "runtimeSigning": {
        "mode": "engineering-adhoc",
        "processEntryMachOs": [
            {
                "path": "lib/wine/aarch64-unix/wine",
                "sha256": wine_unix_digest,
            },
            {
                "path": "bin/wine.switchyard-real",
                "sha256": wine_real_digest,
            },
        ],
    },
    "gstreamerRuntime": {
        "root": "lib/switchyard-gstreamer",
        "digest": gstreamer_digest,
    },
    "vulkanRuntime": {
        "root": "lib/switchyard-vulkan",
        "digest": vulkan_digest,
    },
    "fontRuntime": {
        "root": "lib/switchyard-fonts",
        "digest": font_digest,
    },
    "tlsRuntime": {
        "root": "lib/switchyard-tls",
        "digest": tls_digest,
    },
    "mesaOpenGL": {
        "root": "lib/switchyard-mesa",
        "digest": mesa_digest,
        "fixtureSentinel": "mesa-unchanged",
    },
    "fontAssets": {
        "root": "share/wine/fonts",
        "digest": font_assets_digest,
        "fixtureSentinel": "font-assets-unchanged",
    },
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
        "sourcePatch": {
            "path": (
                "lib/switchyard-unicorn/share/src/switchyard-unicorn/"
                + patch_basename
            ),
            "sha256": patch_digest,
        },
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
}
with open(output, "x", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, ensure_ascii=True, indent=2)
    stream.write("\n")
PY
/bin/chmod 0644 "$MANIFEST"

switchyard_stage_native_arm64_dxmt_artifact \
  "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$ROOT_DIR" "$RUNTIME"
switchyard_finalize_native_arm64_runtime_manifest "$RUNTIME" "$MANIFEST"
switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"

/usr/bin/python3 -I - "$MANIFEST" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
if value["graphicsBackend"] != "dxmt-metal":
    raise SystemExit("producer did not select the frozen native graphics identity")
if [item["module"] for item in value["wow64UnixlibPolicy"]["auditedModules"]] != [
    "crypt32", "dwrite", "secur32", "winemac", "ws2_32"
]:
    raise SystemExit("producer did not emit the exact sorted audited-module list")
if len(value["dxmt"]["modules"]) != 17 or len(value["dxmt"]["documents"]) != 5:
    raise SystemExit("producer did not emit the exact DXMT closure")
if value["dxmt"].get("sourceMaterials") != [{
    "path": "lib/switchyard-dxmt/share/src/switchyard-dxmt/0001-dxmt-Preserve-guest-accessible-CPU-buffer-ownership.patch",
    "sha256": "2e6f6436706f283be6b9ca1668391e0fa70fe83e290781d2a2c5b9f2496a4a26",
    "type": "patch",
}]:
    raise SystemExit("producer did not emit the exact DXMT source-material closure")
provenance = value["dxmt"].get("provenance")
if (provenance.get("sourceRevision") != "856d9f35789679ef00c1ba01a6353438df84b66f"
        or provenance.get("sourceBaseTree") != "22fa93d36867f175c0283b36cd3628a4df94876e"
        or provenance.get("sourceTree") != "2c91e88660daecc3d492de23d32f9e2fed0dd001"
        or provenance.get("artifactBuildIdentity") != "af8ab67d197a4bc6751483b8c16fa17df3b0a6b0"):
    raise SystemExit("producer conflated public source with the opaque artifact build label")
companion = value["dxmt"]["wow64Companion"]
if (companion["path"] != "lib/wine/aarch64-unix/winemetal-wow64.so"
        or companion["format"] != "mach-o-dylib"
        or companion["minimumMacOS"] != "26.5"
        or companion["sdk"] != "26.5"
        or companion["installName"] != "@rpath/winemetal-wow64.so"
        or companion["rpaths"] != ["@loader_path/"]
        or companion["codeSignature"] != "strict"):
    raise SystemExit("producer did not emit the exact signed companion identity")
PY

RUNTIME_ID_BEFORE="$(/usr/bin/plutil -extract id raw -o - "$MANIFEST")"
MESA_MANIFEST_BEFORE="$(
  /usr/bin/plutil -extract mesaOpenGL.digest raw -o - "$MANIFEST"
)"
FONT_ASSETS_MANIFEST_BEFORE="$(
  /usr/bin/plutil -extract fontAssets.digest raw -o - "$MANIFEST"
)"
MESA_TREE_BEFORE="$(content_tree_digest "$MESA_ROOT")"
FONT_ASSETS_TREE_BEFORE="$(content_tree_digest "$FONT_ASSETS_ROOT")"
MESA_MARKER_SHA_BEFORE="$(
  sha256_file "$MESA_ROOT/.switchyard-content-sha256"
)"
MESA_MARKER_MODE_BEFORE="$(
  /usr/bin/stat -f '%Lp' "$MESA_ROOT/.switchyard-content-sha256"
)"
DEPENDENCY_MARKER_MODES_BEFORE=()
for dependency_root in "${DEPENDENCY_ROOTS[@]}"; do
  DEPENDENCY_MARKER_MODES_BEFORE+=("$(
    /usr/bin/stat -f '%Lp' "$dependency_root/.switchyard-content-sha256"
  )")
done

# An identity refresh must not rewrite the pinned Unicorn package merely to
# normalize JSON formatting when its Mach-O library bytes did not change.
UNICORN_MANIFEST_SHA_BEFORE="$(sha256_file "$UNICORN_MANIFEST")"
UNICORN_MARKER_SHA_BEFORE="$(
  sha256_file "$UNICORN_PACKAGE/.switchyard-content-sha256"
)"
switchyard_refresh_native_arm64_signed_runtime_manifest "$RUNTIME" "$MANIFEST"
[ "$(sha256_file "$UNICORN_MANIFEST")" = "$UNICORN_MANIFEST_SHA_BEFORE" ] ||
  fail "no-op signed refresh rewrote the pinned Unicorn manifest"
[ "$(sha256_file "$UNICORN_PACKAGE/.switchyard-content-sha256")" = \
    "$UNICORN_MARKER_SHA_BEFORE" ] ||
  fail "no-op signed refresh changed the pinned Unicorn content marker"
[ "$(/usr/bin/plutil -extract cpuProvider.runtimePayloadDigest raw -o - \
    "$MANIFEST")" = "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" ] ||
  fail "no-op signed refresh changed the pinned Unicorn payload identity"
switchyard_validate_runtime_manifest_profile \
  "$MANIFEST" preview-native-arm64-fex "$RUNTIME"
switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"

SIGNING_MUTATION_TARGETS=(
  "$UNICORN_PACKAGE/lib/libunicorn.2.dylib"
  "$RUNTIME/lib/wine/aarch64-unix/xtajit.so"
  "$RUNTIME/lib/wine/aarch64-unix/xtajit64.so"
  "$RUNTIME/lib/wine/aarch64-unix/crypt32.so"
  "$RUNTIME/lib/wine/aarch64-unix/dwrite.so"
  "$RUNTIME/lib/wine/aarch64-unix/secur32.so"
  "$RUNTIME/lib/wine/aarch64-unix/winemac.so"
  "$RUNTIME/lib/wine/aarch64-unix/ws2_32.so"
  "$RUNTIME/lib/wine/aarch64-unix/winemetal.so"
  "${DEPENDENCY_MACHOS[@]}"
)
unsigned_hashes="$TEST_ROOT/unsigned-macho-hashes.txt"
: >"$unsigned_hashes"
for target in "${SIGNING_MUTATION_TARGETS[@]}"; do
  /usr/bin/printf '%s\t%s\n' "$target" "$(sha256_file "$target")" >>"$unsigned_hashes"
done

signing_index=0
for target in "${SIGNING_MUTATION_TARGETS[@]}"; do
  signing_index=$((signing_index + 1))
  /usr/bin/codesign --force --sign - \
    --identifier "com.switchyard.refresh-test.$signing_index" "$target"
done
while IFS=$'\t' read -r target unsigned_hash; do
  [ "$(sha256_file "$target")" != "$unsigned_hash" ] ||
    fail "signing mutation did not change $target"
done <"$unsigned_hashes"

expect_failure "stale post-signing manifest" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/usr/bin/grep -F 'dependency tree does not match its marker and manifest' \
  "$TEST_ROOT/failure-$failure_index.log" >/dev/null ||
  fail "post-signing validation did not reject a stale dependency tree"
switchyard_refresh_native_arm64_signed_runtime_manifest "$RUNTIME" "$MANIFEST"
switchyard_validate_runtime_manifest_profile \
  "$MANIFEST" preview-native-arm64-fex "$RUNTIME"
switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/usr/bin/python3 -I "$DIGEST_HELPER" verify "$UNICORN_PACKAGE" ||
  fail "refreshed Unicorn nested content marker is invalid"
[ ! -e "$RUNTIME/.switchyard-content-sha256" ] ||
  fail "signed-manifest refresh published the outer runtime marker out of order"
for index in "${!DEPENDENCY_ROOTS[@]}"; do
  dependency_root="${DEPENDENCY_ROOTS[$index]}"
  dependency_field="${DEPENDENCY_FIELDS[$index]}"
  actual_digest="$(content_tree_digest "$dependency_root")"
  marker_digest="$(/usr/bin/tr -d '\n' \
    <"$dependency_root/.switchyard-content-sha256")"
  manifest_digest="$(
    /usr/bin/plutil -extract "$dependency_field.digest" raw -o - "$MANIFEST"
  )"
  [ "$actual_digest" != "${DEPENDENCY_DIGESTS[$index]}" ] ||
    fail "signing did not change the $dependency_field content-tree identity"
  [ "$marker_digest" = "$actual_digest" ] &&
    [ "$manifest_digest" = "$actual_digest" ] ||
    fail "signed-manifest refresh did not repair $dependency_field"
  [ "$(/usr/bin/stat -f '%Lp' \
    "$dependency_root/.switchyard-content-sha256")" = \
    "${DEPENDENCY_MARKER_MODES_BEFORE[$index]}" ] ||
    fail "signed-manifest refresh changed the $dependency_field marker mode"
done
[ "$(/usr/bin/plutil -extract id raw -o - "$MANIFEST")" = \
  "$RUNTIME_ID_BEFORE" ] ||
  fail "signed-manifest refresh changed the build-time runtime identity"
[ "$(/usr/bin/plutil -extract mesaOpenGL.digest raw -o - "$MANIFEST")" = \
  "$MESA_MANIFEST_BEFORE" ] &&
  [ "$(/usr/bin/plutil -extract mesaOpenGL.fixtureSentinel raw -o - \
    "$MANIFEST")" = mesa-unchanged ] &&
  [ "$(content_tree_digest "$MESA_ROOT")" = "$MESA_TREE_BEFORE" ] &&
  [ "$(sha256_file "$MESA_ROOT/.switchyard-content-sha256")" = \
    "$MESA_MARKER_SHA_BEFORE" ] &&
  [ "$(/usr/bin/stat -f '%Lp' \
    "$MESA_ROOT/.switchyard-content-sha256")" = \
    "$MESA_MARKER_MODE_BEFORE" ] ||
  fail "signed-manifest refresh changed the Mesa identity or bytes"
[ "$(/usr/bin/plutil -extract fontAssets.digest raw -o - "$MANIFEST")" = \
  "$FONT_ASSETS_MANIFEST_BEFORE" ] &&
  [ "$(/usr/bin/plutil -extract fontAssets.fixtureSentinel raw -o - \
    "$MANIFEST")" = font-assets-unchanged ] &&
  [ "$(content_tree_digest "$FONT_ASSETS_ROOT")" = \
    "$FONT_ASSETS_TREE_BEFORE" ] ||
  fail "signed-manifest refresh changed the font-assets identity or bytes"
if /usr/bin/find "$RUNTIME" -maxdepth 1 -name '.switchyard-signed-manifest.*' \
    -print -quit | /usr/bin/grep . >/dev/null; then
  fail "signed-manifest refresh left a private staging or capability path"
fi
expect_failure "unchecked signed profile identity" \
  switchyard_validate_runtime_manifest_profile \
  "$MANIFEST" preview-native-arm64-fex

/usr/bin/python3 -I - "$RUNTIME" "$MANIFEST" <<'PY'
import hashlib
import json
import os
import sys

root, manifest = sys.argv[1:]
with open(manifest, encoding="utf-8") as stream:
    value = json.load(stream)


def digest(relative):
    value = hashlib.sha256()
    with open(os.path.join(root, relative), "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


signing = value["runtimeSigning"]
if signing["mode"] != "engineering-adhoc":
    raise SystemExit("refresh did not preserve the engineering signing mode")
for item in signing["processEntryMachOs"]:
    if item["sha256"] != digest(item["path"]):
        raise SystemExit("runtimeSigning does not match final entry bytes")

provider = value["cpuProvider"]
if provider["librarySha256"] != digest(provider["library"]):
    raise SystemExit("refreshed Unicorn library digest is stale")
for component in provider["components"]:
    if component["unixLibrarySha256"] != digest(component["unixLibrary"]):
        raise SystemExit("refreshed provider Unix digest is stale")
    if component["peLibrarySha256"] != digest(component["peLibrary"]):
        raise SystemExit("refreshed provider PE digest is stale")
with open(os.path.join(root, provider["manifest"]), encoding="utf-8") as stream:
    nested = json.load(stream)
if nested["librarySha256"] != provider["librarySha256"]:
    raise SystemExit("nested Unicorn library identity is stale")

for item in value["wow64UnixlibPolicy"]["auditedModules"]:
    if item["sha256"] != digest(item["unixLibrary"]):
        raise SystemExit("refreshed v2 audited-module digest is stale")
for item in value["dxmt"]["modules"]:
    if item["sha256"] != digest(item["path"]):
        raise SystemExit("refreshed DXMT module digest is stale")
files = "".join(
    f"{item['sha256']}  {item['path']}\n" for item in value["dxmt"]["modules"]
)
companion = value["dxmt"]["wow64Companion"]
if companion["sha256"] != digest(companion["path"]):
    raise SystemExit("refreshed DXMT companion digest is stale")
if companion["originalSha256"] != value["dxmt"]["modules"][0]["sha256"]:
    raise SystemExit("refreshed DXMT companion lost its original-library binding")
if companion["abiSchemaSha256"] != digest(companion["abiSchema"]):
    raise SystemExit("refreshed DXMT companion schema digest is stale")
files += f"{companion['sha256']}  {companion['path']}\n"
files += f"{companion['abiSchemaSha256']}  {companion['abiSchema']}\n"
for item in value["dxmt"]["sourceMaterials"]:
    if item["sha256"] != digest(item["path"]):
        raise SystemExit("refreshed DXMT source-material digest is stale")
    files += f"{item['sha256']}  {item['path']}\n"
files = files.encode("ascii")
files_path = value["dxmt"]["documents"][0]["path"]
with open(os.path.join(root, files_path), "rb") as stream:
    if stream.read() != files:
        raise SystemExit("refreshed DXMT files.sha256 is stale")
if value["dxmt"]["documents"][0]["sha256"] != hashlib.sha256(files).hexdigest():
    raise SystemExit("refreshed DXMT files.sha256 document identity is stale")
PY

NO_TLS_RUNTIME="$TEST_ROOT/no-tls-runtime"
NO_TLS_MANIFEST="$NO_TLS_RUNTIME/switchyard-runtime.json"
/usr/bin/ditto "$RUNTIME" "$NO_TLS_RUNTIME"
/bin/rm -rf -- "$NO_TLS_RUNTIME/lib/switchyard-tls"
/usr/bin/python3 -I - "$NO_TLS_MANIFEST" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
value["tlsRuntime"] = None
with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, ensure_ascii=True, indent=2)
    stream.write("\n")
PY
switchyard_validate_native_arm64_signed_dependency_trees \
  "$NO_TLS_RUNTIME" "$NO_TLS_MANIFEST"
switchyard_refresh_native_arm64_signed_runtime_manifest \
  "$NO_TLS_RUNTIME" "$NO_TLS_MANIFEST"
switchyard_validate_native_arm64_signed_dependency_trees \
  "$NO_TLS_RUNTIME" "$NO_TLS_MANIFEST"
[ ! -e "$NO_TLS_RUNTIME/lib/switchyard-tls" ] ||
  fail "optional TLS refresh synthesized a dependency tree"
if ! /usr/bin/python3 -I - "$NO_TLS_MANIFEST" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
if "tlsRuntime" not in value or value["tlsRuntime"] is not None:
    raise SystemExit(1)
PY
then
  fail "optional TLS refresh synthesized a manifest"
fi
[ ! -e "$NO_TLS_RUNTIME/.switchyard-content-sha256" ] ||
  fail "optional TLS refresh published an outer runtime marker"

/bin/mkdir -p "$NO_TLS_RUNTIME/lib/switchyard-tls"
expect_failure "TLS tree without manifest" \
  switchyard_validate_native_arm64_signed_dependency_trees \
  "$NO_TLS_RUNTIME" "$NO_TLS_MANIFEST"
/bin/rm -rf -- "$NO_TLS_RUNTIME/lib/switchyard-tls"

MISSING_TLS_RUNTIME="$TEST_ROOT/missing-tls-runtime"
MISSING_TLS_MANIFEST="$MISSING_TLS_RUNTIME/switchyard-runtime.json"
/usr/bin/ditto "$RUNTIME" "$MISSING_TLS_RUNTIME"
/bin/rm -rf -- "$MISSING_TLS_RUNTIME/lib/switchyard-tls"
expect_failure "TLS manifest without tree" \
  switchyard_validate_native_arm64_signed_dependency_trees \
  "$MISSING_TLS_RUNTIME" "$MISSING_TLS_MANIFEST"

MARKER_RUNTIME="$TEST_ROOT/marker-runtime"
MARKER_MANIFEST="$MARKER_RUNTIME/switchyard-runtime.json"
/usr/bin/ditto "$RUNTIME" "$MARKER_RUNTIME"
MARKER_PATH="$MARKER_RUNTIME/lib/switchyard-gstreamer/.switchyard-content-sha256"
/bin/rm -f -- "$MARKER_PATH"
expect_failure "signed validation with missing dependency marker" \
  switchyard_validate_native_arm64_signed_dependency_trees \
  "$MARKER_RUNTIME" "$MARKER_MANIFEST"
expect_failure "missing dependency marker" \
  switchyard_refresh_native_arm64_signed_runtime_manifest \
  "$MARKER_RUNTIME" "$MARKER_MANIFEST"
[ ! -e "$MARKER_RUNTIME/.switchyard-signed-manifest-refresh-in-progress" ] ||
  fail "pre-publication missing-marker failure tainted the runtime"
/usr/bin/install -m 0644 \
  "$RUNTIME/lib/switchyard-gstreamer/.switchyard-content-sha256" "$MARKER_PATH"
/bin/chmod 0666 "$MARKER_PATH"
expect_failure "signed validation with unsafe dependency marker mode" \
  switchyard_validate_native_arm64_signed_dependency_trees \
  "$MARKER_RUNTIME" "$MARKER_MANIFEST"
expect_failure "unsafe dependency marker mode" \
  switchyard_refresh_native_arm64_signed_runtime_manifest \
  "$MARKER_RUNTIME" "$MARKER_MANIFEST"
[ ! -e "$MARKER_RUNTIME/.switchyard-signed-manifest-refresh-in-progress" ] ||
  fail "pre-publication unsafe-marker failure tainted the runtime"
/bin/chmod 0644 "$MARKER_PATH"
/usr/bin/printf '000000000000\n' >"$MARKER_PATH"
expect_failure "signed validation with mismatched dependency marker" \
  switchyard_validate_native_arm64_signed_dependency_trees \
  "$MARKER_RUNTIME" "$MARKER_MANIFEST"
expect_failure "mismatched dependency marker and manifest" \
  switchyard_refresh_native_arm64_signed_runtime_manifest \
  "$MARKER_RUNTIME" "$MARKER_MANIFEST"

UNSAFE_PARENT_RUNTIME="$TEST_ROOT/unsafe-parent-runtime"
UNSAFE_PARENT_MANIFEST="$UNSAFE_PARENT_RUNTIME/switchyard-runtime.json"
/usr/bin/ditto "$RUNTIME" "$UNSAFE_PARENT_RUNTIME"
/bin/chmod 0777 "$UNSAFE_PARENT_RUNTIME/lib"
expect_failure "unsafe dependency parent directory" \
  switchyard_refresh_native_arm64_signed_runtime_manifest \
  "$UNSAFE_PARENT_RUNTIME" "$UNSAFE_PARENT_MANIFEST"
[ ! -e "$UNSAFE_PARENT_RUNTIME/.switchyard-signed-manifest-refresh-in-progress" ] ||
  fail "pre-publication unsafe-parent failure tainted the runtime"

TAINT_RUNTIME="$TEST_ROOT/taint-runtime"
TAINT_MANIFEST="$TAINT_RUNTIME/switchyard-runtime.json"
/usr/bin/ditto "$RUNTIME" "$TAINT_RUNTIME"
/usr/bin/codesign --force --sign - \
  --identifier com.switchyard.refresh-taint \
  "$TAINT_RUNTIME/lib/wine/aarch64-unix/winemetal.so"
taint_mutated_hash="$(sha256_file \
  "$TAINT_RUNTIME/lib/wine/aarch64-unix/winemetal.so")"
/usr/bin/python3 -I - "$TAINT_MANIFEST" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
value["dxmt"]["graphicsApi"] = "invalid-post-publication-fixture"
with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, ensure_ascii=True, indent=2)
    stream.write("\n")
PY
expect_failure "post-publication signed-manifest validator failure" \
  switchyard_refresh_native_arm64_signed_runtime_manifest \
  "$TAINT_RUNTIME" "$TAINT_MANIFEST"
TAINT_MARKER="$TAINT_RUNTIME/.switchyard-signed-manifest-refresh-in-progress"
[ -f "$TAINT_MARKER" ] && [ ! -L "$TAINT_MARKER" ] ||
  fail "failed signed-manifest refresh did not taint the private runtime"
[ "$(/usr/bin/stat -f '%Lp' "$TAINT_MARKER")" = 600 ] ||
  fail "failed signed-manifest refresh left an unsafe taint-marker mode"
taint_token="$(/usr/bin/tr -d '\n' <"$TAINT_MARKER")"
[[ "$taint_token" =~ ^[0-9a-f]{64}$ ]] ||
  fail "failed signed-manifest refresh left a malformed taint token"
taint_device="$(/usr/bin/stat -f '%d' "$TAINT_MARKER")"
taint_inode="$(/usr/bin/stat -f '%i' "$TAINT_MARKER")"
expect_failure "ambient taint-token public-validator bypass" \
  env \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_TOKEN="$taint_token" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_DEVICE="$taint_device" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_INODE="$taint_inode" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_TOKEN="$taint_token" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_FD=18 \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_DEVICE="$taint_device" \
    SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_INODE="$taint_inode" \
    /bin/bash -c \
      'exec 18<"$4/.switchyard-signed-manifest-refresh-in-progress"; source "$1"; switchyard_validate_runtime_manifest_profile "$2" "$3" "$4"' \
    _ "$PROFILE_LIBRARY" "$TAINT_MANIFEST" \
    preview-native-arm64-fex "$TAINT_RUNTIME"
/usr/bin/grep -F \
  'runtime is tainted by an incomplete signed-manifest refresh' \
  "$TEST_ROOT/failure-$failure_index.log" >/dev/null ||
  fail "ambient taint-token bypass did not reach the fail-closed taint gate"
expect_failure "forged unlinked-capability public-validator bypass" \
  /bin/bash -c '
    set -euo pipefail
    capability="$4/.switchyard-forged-refresh-capability"
    trap '\''rm -f -- "$capability"'\'' EXIT
    printf "%s\n" "$5" >"$capability"
    chmod 0400 "$capability"
    capability_device="$(stat -f "%d" "$capability")"
    capability_inode="$(stat -f "%i" "$capability")"
    exec 18<"$capability"
    rm -f -- "$capability"
    export SWITCHYARD_NATIVE_SIGNED_REFRESH_TOKEN="$5"
    export SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_DEVICE="$6"
    export SWITCHYARD_NATIVE_SIGNED_REFRESH_TAINT_INODE="$7"
    export SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_TOKEN="$5"
    export SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_FD=18
    export SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_DEVICE="$capability_device"
    export SWITCHYARD_NATIVE_SIGNED_REFRESH_CAPABILITY_INODE="$capability_inode"
    source "$1"
    switchyard_validate_runtime_manifest_profile "$2" "$3" "$4"
  ' _ "$PROFILE_LIBRARY" "$TAINT_MANIFEST" \
    preview-native-arm64-fex "$TAINT_RUNTIME" \
    "$taint_token" "$taint_device" "$taint_inode"
/usr/bin/grep -F \
  'runtime is tainted by an incomplete signed-manifest refresh' \
  "$TEST_ROOT/failure-$failure_index.log" >/dev/null ||
  fail "forged unlinked capability did not reach the fail-closed taint gate"
/usr/bin/python3 -I - \
  "$TAINT_MANIFEST" "$taint_mutated_hash" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
matches = [
    item["sha256"] for item in value["dxmt"]["modules"]
    if item["path"] == "lib/wine/aarch64-unix/winemetal.so"
]
if matches != [sys.argv[2]]:
    raise SystemExit("validator failure occurred before refreshed identities were published")
PY
expect_failure "retry of tainted signed-manifest refresh" \
  switchyard_refresh_native_arm64_signed_runtime_manifest \
  "$TAINT_RUNTIME" "$TAINT_MANIFEST"
expect_failure "publication validation of tainted runtime" \
  switchyard_validate_runtime_manifest_profile \
  "$TAINT_MANIFEST" preview-native-arm64-fex "$TAINT_RUNTIME"
if /usr/bin/find "$TAINT_RUNTIME" -maxdepth 1 -type d \
    -name '.switchyard-signed-manifest.*' -print -quit |
    /usr/bin/grep . >/dev/null; then
  fail "failed signed-manifest refresh left a private staging directory"
fi
if /usr/bin/find "$TAINT_RUNTIME" -maxdepth 1 \
    -name '.switchyard-signed-manifest.capability.*' -print -quit |
    /usr/bin/grep . >/dev/null; then
  fail "failed signed-manifest refresh left a path-reopenable capability"
fi

INTERRUPT_RUNTIME="$TEST_ROOT/interrupted-runtime"
INTERRUPT_MANIFEST="$INTERRUPT_RUNTIME/switchyard-runtime.json"
/usr/bin/ditto "$RUNTIME" "$INTERRUPT_RUNTIME"
/usr/bin/codesign --force --sign - \
  --identifier com.switchyard.refresh-interrupted \
  "$INTERRUPT_RUNTIME/lib/wine/aarch64-unix/winemetal.so"
(
  switchyard_refresh_native_arm64_signed_runtime_manifest \
    "$INTERRUPT_RUNTIME" "$INTERRUPT_MANIFEST"
) >"$TEST_ROOT/interrupted-refresh.log" 2>&1 &
interrupt_launcher_pid=$!
interrupt_refresh_pid=""
for _ in {1..500}; do
  interrupt_refresh_pid="$(/bin/ps -axo pid=,command= | /usr/bin/awk \
    -v runtime="$INTERRUPT_RUNTIME" -v manifest="$INTERRUPT_MANIFEST" \
    '$2 ~ /\/(Python|python3)$/ && $3 == "-I" && $4 == "-" && \
      $5 == runtime && $6 == manifest && !found { found = $1 } \
      END { if (found) print found }')"
  [ -n "$interrupt_refresh_pid" ] && break
  /bin/sleep 0.01
done
[ -n "$interrupt_refresh_pid" ] || {
  /bin/kill -TERM "$interrupt_launcher_pid" 2>/dev/null || true
  fail "could not identify the signed-manifest refresher process"
}
INTERRUPT_MARKER="$INTERRUPT_RUNTIME/.switchyard-signed-manifest-refresh-in-progress"
for _ in {1..1000}; do
  [ -f "$INTERRUPT_MARKER" ] && [ ! -L "$INTERRUPT_MARKER" ] && break
  /bin/kill -0 "$interrupt_refresh_pid" 2>/dev/null || break
  /bin/sleep 0.01
done
[ -f "$INTERRUPT_MARKER" ] && [ ! -L "$INTERRUPT_MARKER" ] || {
  /bin/kill -TERM "$interrupt_refresh_pid" 2>/dev/null || true
  /bin/cat "$TEST_ROOT/interrupted-refresh.log" >&2 || true
  fail "refresher completed without exposing its fail-closed transaction marker"
}
/bin/kill -TERM "$interrupt_refresh_pid"
set +e
wait "$interrupt_launcher_pid"
interrupt_status=$?
set -e
[ "$interrupt_status" -ne 0 ] ||
  fail "interrupted signed-manifest refresh reported success"
[ -f "$INTERRUPT_MARKER" ] && [ ! -L "$INTERRUPT_MARKER" ] ||
  fail "interrupted signed-manifest refresh did not remain tainted"
expect_failure "publication validation after interrupted refresh" \
  switchyard_validate_runtime_manifest_profile \
  "$INTERRUPT_MANIFEST" preview-native-arm64-fex "$INTERRUPT_RUNTIME"
expect_failure "retry after interrupted refresh" \
  switchyard_refresh_native_arm64_signed_runtime_manifest \
  "$INTERRUPT_RUNTIME" "$INTERRUPT_MANIFEST"
if /usr/bin/find "$INTERRUPT_RUNTIME" -maxdepth 1 \
    -name '.switchyard-signed-manifest.capability.*' -print -quit |
    /usr/bin/grep . >/dev/null; then
  fail "interrupted refresh left a path-reopenable capability"
fi

/bin/cp "$MANIFEST" "$TEST_ROOT/manifest.good"
/bin/cp "$RUNTIME/lib/wine/x86_64-windows/d3d11.dll" "$TEST_ROOT/d3d11.good"
/bin/cp "$UNICORN_PACKAGE/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" \
  "$TEST_ROOT/unicorn-source-patch.good"

/usr/bin/printf 'tampered\n' >> \
  "$UNICORN_PACKAGE/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"
expect_failure "tampered Unicorn source patch" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/cp "$TEST_ROOT/unicorn-source-patch.good" \
  "$UNICORN_PACKAGE/share/src/switchyard-unicorn/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"

/usr/bin/python3 -I - "$MANIFEST" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
del value["cpuProvider"]["sourcePatch"]
with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, ensure_ascii=True, indent=2)
    stream.write("\n")
PY
expect_failure "missing Unicorn source-patch identity" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/cp "$TEST_ROOT/manifest.good" "$MANIFEST"

/bin/mv "$RUNTIME/lib/wine/aarch64-windows/dxgi.dll" "$TEST_ROOT/dxgi.missing"
expect_failure "missing staged DXMT module" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/mv "$TEST_ROOT/dxgi.missing" "$RUNTIME/lib/wine/aarch64-windows/dxgi.dll"

/bin/cp "$RUNTIME/lib/wine/i386-windows/dxgi.dll" \
  "$RUNTIME/lib/wine/i386-windows/nvngx.dll"
expect_failure "extra DXMT module" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/rm -f "$RUNTIME/lib/wine/i386-windows/nvngx.dll"

/usr/bin/python3 -I - "$MANIFEST" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    value = json.load(stream)
value["wow64UnixlibPolicy"]["auditedModules"][0:2] = reversed(
    value["wow64UnixlibPolicy"]["auditedModules"][0:2]
)
with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, ensure_ascii=True, indent=2)
    stream.write("\n")
PY
expect_failure "unsorted audited module contract" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/cp "$TEST_ROOT/manifest.good" "$MANIFEST"

/usr/bin/printf 'tampered\n' >>"$RUNTIME/lib/wine/x86_64-windows/d3d11.dll"
expect_failure "tampered DXMT module" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/cp "$TEST_ROOT/d3d11.good" "$RUNTIME/lib/wine/x86_64-windows/d3d11.dll"

companion="$RUNTIME/lib/wine/aarch64-unix/winemetal-wow64.so"
/bin/cp "$companion" "$TEST_ROOT/winemetal-wow64.good"
/usr/bin/printf 'tampered\n' >>"$companion"
expect_failure "tampered DXMT WoW64 companion" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/cp "$TEST_ROOT/winemetal-wow64.good" "$companion"
/bin/rm "$companion"
expect_failure "missing DXMT WoW64 companion" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/cp "$TEST_ROOT/winemetal-wow64.good" "$companion"
/bin/mv "$companion" "$TEST_ROOT/winemetal-wow64.target"
/bin/ln -s "$TEST_ROOT/winemetal-wow64.target" "$companion"
expect_failure "symbolic-link DXMT WoW64 companion" \
  switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/bin/rm "$companion"
/bin/mv "$TEST_ROOT/winemetal-wow64.target" "$companion"

expect_failure "missing artifact input" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$TEST_ROOT/missing.tar.gz" "$DXMT_SOURCE" "$ROOT_DIR" "$RUNTIME"

patch_input_runtime="$TEST_ROOT/patch-input-runtime"
/bin/mkdir -p "$patch_input_runtime/lib/wine"
/bin/chmod 0700 "$patch_input_runtime"
expect_failure "missing DXMT corresponding-source patch input" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$COMPANION_SOURCE_FIXTURE" "$patch_input_runtime"
/bin/mkdir -p "$COMPANION_SOURCE_FIXTURE/switchyard/patches"
/bin/cp "$ROOT_DIR/switchyard/patches/$SWITCHYARD_DXMT_SOURCE_PATCH_BASENAME" \
  "$COMPANION_SOURCE_FIXTURE/switchyard/patches/$SWITCHYARD_DXMT_SOURCE_PATCH_BASENAME"
/usr/bin/printf 'tampered\n' >> \
  "$COMPANION_SOURCE_FIXTURE/switchyard/patches/$SWITCHYARD_DXMT_SOURCE_PATCH_BASENAME"
expect_failure "tampered DXMT corresponding-source patch input" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$COMPANION_SOURCE_FIXTURE" "$patch_input_runtime"
/bin/rm "$COMPANION_SOURCE_FIXTURE/switchyard/patches/$SWITCHYARD_DXMT_SOURCE_PATCH_BASENAME"
/bin/ln -s "$ROOT_DIR/switchyard/patches/$SWITCHYARD_DXMT_SOURCE_PATCH_BASENAME" \
  "$COMPANION_SOURCE_FIXTURE/switchyard/patches/$SWITCHYARD_DXMT_SOURCE_PATCH_BASENAME"
expect_failure "symbolic-link DXMT corresponding-source patch input" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$COMPANION_SOURCE_FIXTURE" "$patch_input_runtime"

tampered_archive="$TEST_ROOT/tampered.tar.gz"
/bin/cp "$DXMT_ARCHIVE" "$tampered_archive"
/usr/bin/printf 'extra' >>"$tampered_archive"
empty_runtime="$TEST_ROOT/tampered-runtime"
/bin/mkdir -p "$empty_runtime/lib/wine"
/bin/chmod 0700 "$empty_runtime"
expect_failure "tampered artifact archive" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$tampered_archive" "$DXMT_SOURCE" "$ROOT_DIR" "$empty_runtime"

outside="$TEST_ROOT/outside"
unsafe_runtime="$TEST_ROOT/unsafe-runtime"
/bin/mkdir -p "$outside" "$unsafe_runtime/lib/wine"
/bin/chmod 0700 "$outside" "$unsafe_runtime"
/bin/ln -s "$outside" "$unsafe_runtime/lib/wine/aarch64-windows"
expect_failure "symbolic-link architecture directory" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$ROOT_DIR" "$unsafe_runtime"
[ -z "$(/usr/bin/find "$outside" -mindepth 1 -print -quit)" ] ||
  fail "staging wrote outside the runtime through a symbolic link"

real_parent="$TEST_ROOT/real-parent"
/bin/mkdir -p "$real_parent/runtime/lib/wine"
/bin/chmod 0700 "$real_parent" "$real_parent/runtime"
/bin/ln -s "$real_parent" "$TEST_ROOT/linked-parent"
expect_failure "symbolic-link runtime parent" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$ROOT_DIR" "$TEST_ROOT/linked-parent/runtime"

/bin/cp "$TEST_ROOT/manifest.good" "$TEST_ROOT/already-native.json"
expect_failure "manifest native-field overwrite" \
  switchyard_finalize_native_arm64_runtime_manifest \
    "$RUNTIME" "$TEST_ROOT/already-native.json"

default_source_info="$(SWITCHYARD_DISABLE_GPTK_OVERLAY=1 \
  "$ROOT_DIR/switchyard/build_runtime.sh" --source-info)"
stable_source_info="$(SWITCHYARD_DISABLE_GPTK_OVERLAY=1 \
  "$ROOT_DIR/switchyard/build_runtime.sh" \
  --runtime-profile stable-x86_64-rosetta --source-info)"
[ "$default_source_info" = "$stable_source_info" ] ||
  fail "native packaging changed the stable profile selection or source identity"
if /usr/bin/grep -E '^(dxmt|graphicsBackend|wow64UnixlibPolicy)' \
    <<<"$stable_source_info" >/dev/null; then
  fail "stable source identity includes native packaging inputs"
fi

/usr/bin/grep -F 'tarfile.open(fileobj=archive_stream, mode="r:gz")' \
  "$PACKAGING_LIBRARY" >/dev/null ||
  fail "archive extraction does not remain pinned to the verified descriptor"
/usr/bin/grep -F 'os.replace(temporary, name, src_dir_fd=descriptor, dst_dir_fd=descriptor)' \
  "$PACKAGING_LIBRARY" >/dev/null ||
  fail "runtime staging is not fd-relative across its validation/write boundary"

echo "native ARM64 packaging producer tests passed"
