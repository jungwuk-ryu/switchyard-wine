#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DRIVER="$ROOT_DIR/switchyard/tests/native_i386_ui_acceptance.sh"
FIXTURE_SOURCE="$ROOT_DIR/switchyard/tests/native_i386_ui_fake_runtime.c"
PROBE_SOURCE="$ROOT_DIR/switchyard/tests/native_no_rosetta_process_probe.c"
PROFILE_LIBRARY="$ROOT_DIR/switchyard/lib/runtime_profile.sh"
PROVIDER_LIBRARY="$ROOT_DIR/switchyard/lib/native_cpu_provider.sh"
DIGEST_HELPER="$ROOT_DIR/switchyard/runtime_content_digest.py"
TEST_ROOT="$(/usr/bin/mktemp -d /tmp/switchyard-native-i386-ui-fixture.XXXXXX)"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd -P)"
RUNTIME="$TEST_ROOT/runtime"
TEST_PE="$TEST_ROOT/user32_test.exe"
MANIFEST="$RUNTIME/switchyard-runtime.json"

cleanup()
{
    local status=$?

    trap - EXIT HUP INT TERM
    if [ -x "$RUNTIME/bin/wineserver" ]; then
        for fixture_prefix in "$TEST_ROOT"/evidence-*/prefix; do
            [ -d "$fixture_prefix" ] && [ ! -L "$fixture_prefix" ] || continue
            WINEPREFIX="$fixture_prefix" "$RUNTIME/bin/wineserver" -k \
                >/dev/null 2>&1 || true
            WINEPREFIX="$fixture_prefix" "$RUNTIME/bin/wineserver" -w \
                >/dev/null 2>&1 || true
        done
    fi
    case "$TEST_ROOT" in
        /tmp/switchyard-native-i386-ui-fixture.??????|\
        /private/tmp/switchyard-native-i386-ui-fixture.??????)
            [ -L "$TEST_ROOT" ] || /bin/rm -rf -- "$TEST_ROOT"
            ;;
        *) echo "refusing to clean unexpected UI acceptance-fixture root $TEST_ROOT" >&2 ;;
    esac
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
    echo "native i386 UI acceptance fixture failed: $1" >&2
    exit 1
}

require_line()
{
    /usr/bin/grep -Fx -- "$1" "$2" >/dev/null || fail "$3"
}

expect_usage_failure()
{
    local expected="$1"
    shift
    local status

    set +e
    "$@" >"$TEST_ROOT/usage.out" 2>"$TEST_ROOT/usage.err"
    status=$?
    set -e
    [ "$status" -eq 2 ] || fail "unsafe invocation returned $status instead of usage status 2"
    /usr/bin/grep -F -- "$expected" "$TEST_ROOT/usage.err" >/dev/null ||
        fail "unsafe invocation did not report its exact rejection"
}

sha256_file()
{
    /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

expect_driver_failure()
{
    local selector="$1"
    local expected="$2"
    shift 2
    local evidence="$TEST_ROOT/evidence-$selector"
    local status

    set +e
    "$DRIVER" "$RUNTIME" "$TEST_PE" win "$selector" \
        --evidence-dir "$evidence" "$@" \
        >"$TEST_ROOT/$selector.out" 2>"$TEST_ROOT/$selector.err"
    status=$?
    set -e
    [ "$status" -eq 1 ] ||
        fail "$selector returned $status instead of acceptance failure status 1"
    if ! /usr/bin/grep -F -- "$expected" "$TEST_ROOT/$selector.err" >/dev/null; then
        /bin/cat "$TEST_ROOT/$selector.err" >&2
        fail "$selector did not report its exact rejection: $expected"
    fi
    [ -f "$evidence/cleanup.evidence" ] ||
        fail "$selector did not retain exact-prefix cleanup evidence"
    require_line 'killStatus=passed' "$evidence/cleanup.evidence" \
        "$selector prefix kill did not pass"
    require_line 'waitStatus=passed' "$evidence/cleanup.evidence" \
        "$selector prefix wait did not pass"
}

[ "$#" -eq 0 ] || {
    echo "usage: $0" >&2
    exit 2
}
[ "$(/usr/bin/uname -s)" = Darwin ] || fail "fixtures require macOS"
[ "$(/usr/bin/uname -m)" = arm64 ] || fail "fixtures require a native arm64 shell"
for source_file in "$DRIVER" "$FIXTURE_SOURCE" "$PROBE_SOURCE" \
        "$PROFILE_LIBRARY" "$PROVIDER_LIBRARY" "$DIGEST_HELPER"; do
    [ -f "$source_file" ] && [ ! -L "$source_file" ] ||
        fail "required source is missing or unsafe: $source_file"
done
[ -x "$DRIVER" ] || fail "UI acceptance driver is not executable"
/bin/bash -n "$DRIVER"

# shellcheck disable=SC1090 # Fixed repository-relative policy libraries.
source "$PROFILE_LIBRARY"
# shellcheck disable=SC1090
source "$PROVIDER_LIBRARY"
switchyard_load_runtime_profile preview-native-arm64-fex

REPO_SOURCE_PATCH="$ROOT_DIR/switchyard/patches/$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME"
[ -f "$REPO_SOURCE_PATCH" ] && [ ! -L "$REPO_SOURCE_PATCH" ] ||
    fail "pinned Unicorn source patch is missing or unsafe"
[ "$(sha256_file "$REPO_SOURCE_PATCH")" = "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" ] ||
    fail "pinned Unicorn source patch digest changed"
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
    "$RUNTIME/lib/switchyard-unicorn/lib" \
    "$RUNTIME/lib/switchyard-unicorn/share/doc/switchyard-unicorn" \
    "$RUNTIME/lib/switchyard-unicorn/share/src/switchyard-unicorn"
/bin/chmod 0755 \
    "$RUNTIME/bin" "$RUNTIME/lib" "$RUNTIME/lib/wine" \
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
FIXTURE_PAYLOAD_DIGEST="$(/usr/bin/python3 -I "$DIGEST_HELPER" write "$UNICORN_PACKAGE")"
[ "$FIXTURE_PAYLOAD_DIGEST" = "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" ] ||
    fail "fixture Unicorn payload differs from the pinned profile"

/usr/bin/xcrun --sdk macosx clang -arch arm64 -std=c17 -O2 \
    -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
    "$FIXTURE_SOURCE" -o "$RUNTIME/lib/wine/aarch64-unix/wine"
/bin/cp "$RUNTIME/lib/wine/aarch64-unix/wine" "$RUNTIME/bin/wineserver"
/bin/cat >"$RUNTIME/bin/wine" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
bin_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
runtime_dir="$(cd "$bin_dir/.." && pwd -P)"
exec -a "$bin_dir/wine" "$runtime_dir/lib/wine/aarch64-unix/wine" "$@"
EOF
/bin/chmod 0755 "$RUNTIME/bin/wine"
/bin/ln -s wine "$RUNTIME/bin/switchyard-wine"

/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 \
    -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
    -Wl,-install_name,@rpath/ntdll.so \
    -x c /dev/stdin -o "$RUNTIME/lib/wine/aarch64-unix/ntdll.so" <<'EOF'
int ntdll_fixture(void) { return 7; }
EOF
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 \
    -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
    -Wl,-install_name,@rpath/xtajit.so \
    -Wl,-rpath,@loader_path/ \
    -Wl,-rpath,@loader_path/../../switchyard-unicorn/lib \
    -x c /dev/stdin -x none \
    "$RUNTIME/lib/wine/aarch64-unix/ntdll.so" \
    "$UNICORN_PACKAGE/lib/libunicorn.2.dylib" \
    -o "$RUNTIME/lib/wine/aarch64-unix/xtajit.so" <<'EOF'
extern int ntdll_fixture(void);
extern unsigned int uc_version(unsigned int *, unsigned int *);
struct uc_struct;
typedef int (*switchyard_unicorn_extension_t)(struct uc_struct *);
extern int uc_emu_stop_at_instruction_boundary(struct uc_struct *);
extern int uc_clear_instruction_boundary_stop(struct uc_struct *);
extern int uc_enable_shared_memory_atomics(struct uc_struct *);
__attribute__((used, visibility("default")))
switchyard_unicorn_extension_t const switchyard_unicorn_fixture_imports[] = {
    uc_emu_stop_at_instruction_boundary,
    uc_clear_instruction_boundary_stop,
    uc_enable_shared_memory_atomics,
};
__attribute__((visibility("default"))) unsigned int provider_fixture(void)
{
    return (unsigned int)ntdll_fixture() + uc_version(0, 0);
}
EOF
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 \
    -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
    -Wl,-install_name,@rpath/xtajit64.so \
    -Wl,-rpath,@loader_path/ \
    -Wl,-rpath,@loader_path/../../switchyard-unicorn/lib \
    -x c /dev/stdin -x none \
    "$RUNTIME/lib/wine/aarch64-unix/ntdll.so" \
    "$UNICORN_PACKAGE/lib/libunicorn.2.dylib" \
    -o "$RUNTIME/lib/wine/aarch64-unix/xtajit64.so" <<'EOF'
extern int ntdll_fixture(void);
extern unsigned int uc_version(unsigned int *, unsigned int *);
struct uc_struct;
typedef int (*switchyard_unicorn_extension_t)(struct uc_struct *);
extern int uc_emu_stop_at_instruction_boundary(struct uc_struct *);
extern int uc_clear_instruction_boundary_stop(struct uc_struct *);
extern int uc_enable_shared_memory_atomics(struct uc_struct *);
extern int uc_set_shared_memory_atomic_callback(struct uc_struct *);
__attribute__((used, visibility("default")))
switchyard_unicorn_extension_t const switchyard_unicorn_fixture_imports[] = {
    uc_emu_stop_at_instruction_boundary,
    uc_clear_instruction_boundary_stop,
    uc_enable_shared_memory_atomics,
    uc_set_shared_memory_atomic_callback,
};
__attribute__((used, visibility("default"))) const char
    switchyard_xtajit64_fixture_abi_identity[] =
        "switchyard-xtajit64-provider-abi-v10-flight-bind-process-init-96-begin-472-doorbell";
__attribute__((visibility("default"))) unsigned int provider_fixture(void)
{
    return (unsigned int)ntdll_fixture() + uc_version(0, 0);
}
EOF

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
        struct.pack_into("<Q", value, 0x2C8, 0x180001100)
        struct.pack_into("<III", value, 0x300, 2, 0x1120, 1)
        struct.pack_into("<II", value, 0x320, 0x2001, 0x100)
    if abi_identity is not None:
        value.extend(abi_identity.encode("ascii") + b"\0")
    path = os.path.join(root, relative)
    with open(path, "xb") as stream:
        stream.write(value)
    os.chmod(path, 0o755)


write_pe(xtajit, 0xAA64)
write_pe(xtajit64, 0x8664, True, x64_abi_identity)
PY

for module in crypt32 dwrite secur32 winemac ws2_32; do
    /usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 \
        -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
        -Wl,-install_name,"@rpath/$module.so" \
        -x c /dev/stdin -o "$RUNTIME/lib/wine/aarch64-unix/$module.so" <<'EOF'
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
done
/bin/cp "$RUNTIME/lib/wine/aarch64-unix/crypt32.so" \
    "$RUNTIME/lib/wine/aarch64-unix/winemetal-wow64.so"

/usr/bin/xcrun --sdk macosx clang -arch arm64 -std=c17 -O2 \
    -mmacosx-version-min=26.5 -Wall -Wextra -Werror \
    "$PROBE_SOURCE" -o "$TEST_ROOT/native-process-probe"
"$TEST_ROOT/native-process-probe" --regression-fixture-process-list-capacity
"$TEST_ROOT/native-process-probe" --regression-fixture-wrapper-bootstrap \
    >"$TEST_ROOT/wrapper.out" 2>"$TEST_ROOT/wrapper.err"

# Build the exact production-shaped provider and Wow64 closure so this fixture
# drives the public acceptance entry point without a validator bypass.
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
    "$SWITCHYARD_UNICORN_VERSION" "$SWITCHYARD_UNICORN_SOURCE_REPOSITORY" \
    "$SWITCHYARD_UNICORN_SOURCE_REVISION" "$SWITCHYARD_UNICORN_SOURCE_ARCHIVE_SHA256" \
    "$SWITCHYARD_UNICORN_SOURCE_PATCH_BASENAME" \
    "$SWITCHYARD_UNICORN_SOURCE_PATCH_SHA256" \
    "$SWITCHYARD_UNICORN_BUILD_CONTRACT_VERSION" \
    "$SWITCHYARD_RUNTIME_PROFILE_KUSER_SHARED_DATA_MODEL" \
    "$SWITCHYARD_UNICORN_DEVELOPMENT_CACHE_DIGEST" \
    "$SWITCHYARD_UNICORN_RUNTIME_PAYLOAD_DIGEST" \
    "$SWITCHYARD_UNICORN_LIBRARY_SHA256" \
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
    patch_basename,
    patch_digest,
    build_contract,
    kuser_model,
    development_digest,
    payload_digest,
    library_digest,
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
    "id": "switchyard-local-native-arm64-fex-ui-acceptance-fixture",
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
        "sourcePatch": {
            "path": (
                "lib/switchyard-unicorn/share/src/switchyard-unicorn/"
                + patch_basename
            ),
            "sha256": patch_digest,
        },
        "buildContractVersion": int(build_contract),
        "hostArchitecture": "arm64",
        "kuserSharedDataModel": kuser_model,
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
switchyard_validate_runtime_manifest_profile \
    "$MANIFEST" preview-native-arm64-fex ||
    fail "current native runtime-profile fixture was rejected"
switchyard_validate_native_cpu_provider_files "$MANIFEST" "$RUNTIME" ||
    fail "native CPU-provider fixture closure was rejected"
switchyard_validate_wow64_unixlib_policy_manifest "$RUNTIME" "$MANIFEST" "$ROOT_DIR" ||
    fail "native Wow64 Unixlib fixture closure was rejected"

/usr/bin/python3 -I - "$TEST_PE" <<'PY'
import struct
import sys

value = bytearray(0x100)
value[:2] = b"MZ"
struct.pack_into("<I", value, 0x3c, 0x80)
value[0x80:0x84] = b"PE\0\0"
struct.pack_into("<H", value, 0x84, 0x014c)
with open(sys.argv[1], "xb") as stream:
    stream.write(value)
PY
/bin/cp "$TEST_PE" "$TEST_ROOT/test-pe.good"

expect_usage_failure "native runtime path must be absolute" \
    "$DRIVER" relative-runtime "$TEST_PE" win test_success
expect_usage_failure "test selector must be one exact test_ identifier" \
    "$DRIVER" "$RUNTIME" "$TEST_PE" win 'test_success;touch_bad'

SUCCESS_EVIDENCE="$TEST_ROOT/evidence-test_success"
"$DRIVER" "$RUNTIME" "$TEST_PE" win test_success \
    --timeout-seconds 10 --max-log-bytes 1048576 \
    --evidence-dir "$SUCCESS_EVIDENCE" \
    >"$TEST_ROOT/test_success.out" 2>"$TEST_ROOT/test_success.err" &
DRIVER_PID=$!
SUCCESS_STARTED=0
for ((attempt = 0; attempt < 300; ++attempt)); do
    if [ -f "$SUCCESS_EVIDENCE/prefix/fixture-invocation.txt" ]; then
        SUCCESS_STARTED=1
        break
    fi
    process_state="$(/bin/ps -p "$DRIVER_PID" -o stat= 2>/dev/null || true)"
    if ! /bin/kill -0 "$DRIVER_PID" 2>/dev/null || [[ "$process_state" == *Z* ]]; then
        break
    fi
    /bin/sleep 0.02
done
[ "$SUCCESS_STARTED" -eq 1 ] || {
    set +e
    wait "$DRIVER_PID"
    status=$?
    set -e
    /bin/cat "$TEST_ROOT/test_success.err" >&2
    fail "success acceptance fixture did not start (status $status)"
}
/usr/bin/printf 'mutated-after-private-snapshot\n' >>"$TEST_PE"
set +e
wait "$DRIVER_PID"
SUCCESS_STATUS=$?
set -e
[ "$SUCCESS_STATUS" -eq 0 ] || {
    /bin/cat "$TEST_ROOT/test_success.err" >&2
    fail "success acceptance fixture returned $SUCCESS_STATUS"
}

INVOCATION="$SUCCESS_EVIDENCE/prefix/fixture-invocation.txt"
CLEANUP="$SUCCESS_EVIDENCE/prefix/fixture-cleanup.txt"
require_line 'argc=4' "$INVOCATION" "fixture launcher received the wrong argc"
require_line "argv1=$SUCCESS_EVIDENCE/i386-test.exe" "$INVOCATION" \
    "fixture launcher did not execute its private PE snapshot"
require_line 'argv2=win' "$INVOCATION" "fixture launcher lost the suite argument"
require_line 'argv3=test_success' "$INVOCATION" "fixture launcher lost the selector"
require_line "WINEPREFIX=$SUCCESS_EVIDENCE/prefix" "$INVOCATION" \
    "fixture launcher lost the exact prefix"
for name in WINEARCH WINEDLLPATH WINELOADER WINESERVER \
        DISABLE_GPTK_OVERLAY GPTK_PATH SWITCHYARD_DISABLE_GPTK_OVERLAY \
        SWITCHYARD_GPTK_PATH SWITCHYARD_GPTK_DLL_NT_PATH \
        DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH DYLD_FRAMEWORK_PATH \
        DYLD_FALLBACK_FRAMEWORK_PATH DYLD_INSERT_LIBRARIES; do
    require_line "$name=<unset>" "$INVOCATION" \
        "fixture inherited forbidden host environment: $name"
done
require_line 'result=passed' "$SUCCESS_EVIDENCE/result.evidence" \
    "driver did not retain a passed fixture result"
require_line 'loadedImageEvidence=true' "$SUCCESS_EVIDENCE/result.evidence" \
    "driver did not retain authoritative loaded-image evidence"
require_line "testPEInput=$TEST_PE" "$SUCCESS_EVIDENCE/invocation.evidence" \
    "driver did not retain the original PE input path"
require_line "testPESnapshot=$SUCCESS_EVIDENCE/i386-test.exe" \
    "$SUCCESS_EVIDENCE/invocation.evidence" \
    "driver did not retain its private PE snapshot path"
[ "$(/usr/bin/stat -f '%Lp' "$SUCCESS_EVIDENCE/i386-test.exe")" = 400 ] ||
    fail "private PE snapshot was not retained read-only"
[ "$(sha256_file "$SUCCESS_EVIDENCE/i386-test.exe")" = \
    "$(sha256_file "$TEST_ROOT/test-pe.good")" ] ||
    fail "private PE snapshot changed when its original input was mutated"
require_line "wineExecutable=$RUNTIME/lib/wine/aarch64-unix/wine" \
    "$SUCCESS_EVIDENCE/loaded-image.evidence" \
    "loaded-image evidence lost the exact native Wine executable"
require_line "winemacImage=$RUNTIME/lib/wine/aarch64-unix/winemac.so" \
    "$SUCCESS_EVIDENCE/loaded-image.evidence" \
    "loaded-image evidence lost the exact winemac image"
require_line "i386ProviderImage=$RUNTIME/lib/wine/aarch64-unix/xtajit.so" \
    "$SUCCESS_EVIDENCE/loaded-image.evidence" \
    "loaded-image evidence lost the exact i386 provider image"
/usr/bin/grep -E \
    '^Loaded-image proof passed for native ARM64 pid [0-9]+ across [1-9][0-9]* regions[.]$' \
    "$SUCCESS_EVIDENCE/loaded-image.evidence" >/dev/null ||
    fail "loaded-image evidence lacks a native vnode inspection result"
/usr/bin/grep -F 'initialized Unicorn fixture i386 provider registry' \
    "$SUCCESS_EVIDENCE/runtime.log" >/dev/null || fail "fixture log lacks provider diagnostics"
/usr/bin/grep -F 'build_module Loaded' "$SUCCESS_EVIDENCE/runtime.log" >/dev/null ||
    fail "fixture log lacks winemac load evidence"
/usr/bin/grep -E ' [1-9][0-9]* tests executed .* 0 failures.*, 0 skipped[.]$' \
    "$SUCCESS_EVIDENCE/runtime.log" >/dev/null || fail "fixture log lacks a zero-failure summary"
[ "$(/usr/bin/grep -c '^operation=-k$' "$CLEANUP")" -eq 1 ] ||
    fail "fixture cleanup did not issue one prefix-scoped -k"
[ "$(/usr/bin/grep -c '^operation=-w$' "$CLEANUP")" -eq 1 ] ||
    fail "fixture cleanup did not issue one prefix-scoped -w"
[ "$(/usr/bin/grep -Fxc "prefix=$SUCCESS_EVIDENCE/prefix" "$CLEANUP")" -eq 2 ] ||
    fail "fixture cleanup escaped its exact prefix"
/bin/cp "$TEST_ROOT/test-pe.good" "$TEST_PE"

expect_driver_failure test_missing_winemac \
    "exact native winemac and i386 CPU-provider images were not proven loaded" \
    --timeout-seconds 10
expect_driver_failure test_missing_provider \
    "exact native winemac and i386 CPU-provider images were not proven loaded" \
    --timeout-seconds 10
for forged_selector in test_missing_winemac test_missing_provider; do
    /usr/bin/grep -F 'initialized Unicorn fixture i386 provider registry' \
        "$TEST_ROOT/evidence-$forged_selector/runtime.log" >/dev/null ||
        fail "$forged_selector did not exercise a forged provider-positive log"
    /usr/bin/grep -F 'build_module Loaded' \
        "$TEST_ROOT/evidence-$forged_selector/runtime.log" >/dev/null ||
        fail "$forged_selector did not exercise a forged winemac-positive log"
done
expect_driver_failure test_provider_poison \
    "i386 CPU provider poison was observed" --timeout-seconds 10
expect_driver_failure test_failures \
    "Wine test failure output was observed" --timeout-seconds 10
expect_driver_failure test_wrong_summary \
    "exact selector did not produce one zero-failure, zero-skip Wine summary" \
    --timeout-seconds 10
expect_driver_failure test_zero_assertions \
    "exact selector did not produce one zero-failure, zero-skip Wine summary" \
    --timeout-seconds 10
expect_driver_failure test_timeout \
    "exact test invocation exceeded 1 seconds" --timeout-seconds 1
expect_driver_failure test_log_limit \
    "runtime log exceeded the configured byte bound" \
    --timeout-seconds 10 --max-log-bytes 4096

/usr/bin/grep -F \
    'switchyard_validate_runtime_manifest_profile "$MANIFEST" preview-native-arm64-fex' \
    "$DRIVER" >/dev/null || fail "driver is not bound to the current native profile"
if /usr/bin/grep -E 'abi-schema-v3|companion_v3' \
        "$DRIVER" >/dev/null; then
    fail "retired DXMT companion schema leaked into the current UI harness"
fi

echo "Native i386 UI acceptance fixture-only driver coverage passed; no production runtime was qualified."
