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

/usr/bin/python3 -I - "$RUNTIME" <<'PY'
import os
import struct
import sys

root = sys.argv[1]


def write_pe(relative, machine, arm64ec=False):
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
    path = os.path.join(root, relative)
    with open(path, "xb") as stream:
        stream.write(value)
    os.chmod(path, 0o755)


write_pe("lib/wine/aarch64-windows/xtajit.dll", 0xAA64)
write_pe("lib/wine/aarch64-windows/xtajit64.dll", 0x8664, True)
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
  "$WINE_UNIX_SHA" "$WINE_REAL_SHA" "$I386_NTDLL_SHA" "$X86_64_NTDLL_SHA" <<'PY'
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

switchyard_stage_native_arm64_dxmt_artifact "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$RUNTIME"
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
if len(value["dxmt"]["modules"]) != 17 or len(value["dxmt"]["documents"]) != 4:
    raise SystemExit("producer did not emit the exact DXMT closure")
PY

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
switchyard_refresh_native_arm64_signed_runtime_manifest "$RUNTIME" "$MANIFEST"
switchyard_validate_runtime_manifest_profile \
  "$MANIFEST" preview-native-arm64-fex "$RUNTIME"
switchyard_validate_native_arm64_runtime_packaging "$RUNTIME" "$MANIFEST" "$ROOT_DIR"
/usr/bin/python3 -I "$DIGEST_HELPER" verify "$UNICORN_PACKAGE" ||
  fail "refreshed Unicorn nested content marker is invalid"
[ ! -e "$RUNTIME/.switchyard-content-sha256" ] ||
  fail "signed-manifest refresh published the outer runtime marker out of order"
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
).encode("ascii")
files_path = value["dxmt"]["documents"][0]["path"]
with open(os.path.join(root, files_path), "rb") as stream:
    if stream.read() != files:
        raise SystemExit("refreshed DXMT files.sha256 is stale")
if value["dxmt"]["documents"][0]["sha256"] != hashlib.sha256(files).hexdigest():
    raise SystemExit("refreshed DXMT files.sha256 document identity is stale")
PY

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

expect_failure "missing artifact input" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$TEST_ROOT/missing.tar.gz" "$DXMT_SOURCE" "$RUNTIME"

tampered_archive="$TEST_ROOT/tampered.tar.gz"
/bin/cp "$DXMT_ARCHIVE" "$tampered_archive"
/usr/bin/printf 'extra' >>"$tampered_archive"
empty_runtime="$TEST_ROOT/tampered-runtime"
/bin/mkdir -p "$empty_runtime/lib/wine"
/bin/chmod 0700 "$empty_runtime"
expect_failure "tampered artifact archive" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$tampered_archive" "$DXMT_SOURCE" "$empty_runtime"

outside="$TEST_ROOT/outside"
unsafe_runtime="$TEST_ROOT/unsafe-runtime"
/bin/mkdir -p "$outside" "$unsafe_runtime/lib/wine"
/bin/chmod 0700 "$outside" "$unsafe_runtime"
/bin/ln -s "$outside" "$unsafe_runtime/lib/wine/aarch64-windows"
expect_failure "symbolic-link architecture directory" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$unsafe_runtime"
[ -z "$(/usr/bin/find "$outside" -mindepth 1 -print -quit)" ] ||
  fail "staging wrote outside the runtime through a symbolic link"

real_parent="$TEST_ROOT/real-parent"
/bin/mkdir -p "$real_parent/runtime/lib/wine"
/bin/chmod 0700 "$real_parent" "$real_parent/runtime"
/bin/ln -s "$real_parent" "$TEST_ROOT/linked-parent"
expect_failure "symbolic-link runtime parent" \
  switchyard_stage_native_arm64_dxmt_artifact \
    "$DXMT_ARCHIVE" "$DXMT_SOURCE" "$TEST_ROOT/linked-parent/runtime"

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
