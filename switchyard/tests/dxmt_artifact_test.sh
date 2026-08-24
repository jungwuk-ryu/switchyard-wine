#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
POLICY_LIBRARY="$ROOT_DIR/switchyard/lib/dxmt_artifact.sh"
STAGED_FIXTURE="${1:-}"
ARCHIVE_FIXTURE="${2:-}"

[ "$#" -eq 2 ] || {
  echo "usage: $0 STAGED_DXMT_RUNTIME EXTRACTED_DXMT_ARTIFACT" >&2
  exit 2
}
for fixture in "$STAGED_FIXTURE" "$ARCHIVE_FIXTURE"; do
  case "$fixture" in
    /*) ;;
    *) echo "DXMT test fixture paths must be absolute." >&2; exit 2 ;;
  esac
  [ -d "$fixture" ] && [ ! -L "$fixture" ] || {
    echo "DXMT test fixture is missing or unsafe: $fixture" >&2
    exit 2
  }
  [ "$fixture" != / ] || {
    echo "DXMT test fixture path is unbounded." >&2
    exit 2
  }
done
STAGED_FIXTURE="$(cd "$STAGED_FIXTURE" && pwd -P)"
ARCHIVE_FIXTURE="$(cd "$ARCHIVE_FIXTURE" && pwd -P)"
[ -f "$STAGED_FIXTURE/lib/wine/aarch64-unix/winemetal.so" ] && \
  [ ! -L "$STAGED_FIXTURE/lib/wine/aarch64-unix/winemetal.so" ] || {
  echo "DXMT staged fixture has no regular arm64 Unix module." >&2
  exit 2
}
[ -f "$ARCHIVE_FIXTURE/x86_64-unix/winemetal.so" ] && \
  [ ! -L "$ARCHIVE_FIXTURE/x86_64-unix/winemetal.so" ] || {
  echo "DXMT extracted artifact fixture has no regular x86_64 Unix module." >&2
  exit 2
}
[ "$(/usr/bin/shasum -a 256 "$ARCHIVE_FIXTURE/x86_64-unix/winemetal.so" | \
  /usr/bin/awk '{print $1}')" = \
  "9a73df5fc25730a2b19286c6d34f365fade266cb833d0ca69a905d4696ed0b05" ] || {
  echo "DXMT extracted x86_64 Unix fixture does not match the pinned artifact." >&2
  exit 2
}
[ -f "$POLICY_LIBRARY" ] && [ ! -L "$POLICY_LIBRARY" ] || {
  echo "DXMT policy library is missing or unsafe." >&2
  exit 1
}
# shellcheck disable=SC1090 # Fixed repository-relative policy library.
source "$POLICY_LIBRARY"
declare -F switchyard_validate_dxmt_runtime_manifest >/dev/null || {
  echo "DXMT policy library does not expose the frozen validator seam." >&2
  exit 1
}

test_root="$(/usr/bin/mktemp -d /private/tmp/switchyard-dxmt-policy.XXXXXX)"
test_root="$(cd "$test_root" && pwd -P)"
runtime="$test_root/runtime"
manifest="$runtime/switchyard-runtime.json"
failure_log="$test_root/failure.log"
fixture_companion="$test_root/winemetal-wow64.so"
fixture_companion_extra="$test_root/winemetal-wow64-extra.so"

/bin/cat >"$test_root/winemetal-wow64.c" <<'EOF'
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
struct companion_descriptor_v6
{
    uint32_t version, size, entry_count, flags;
    unsigned char abi_sha256[32];
    const void *bind;
    const void *quiesce;
    const void *unbind;
};
__attribute__((visibility("default"))) const struct companion_descriptor_v6
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
#ifdef EXTRA_COMPANION_EXPORT
__attribute__((visibility("default"))) const uint32_t
    __wine_unix_call_wow64_unexpected_v2 = 2;
#endif
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
EOF
/bin/cat >"$test_root/ntdll.c" <<'EOF'
int ntdll_fixture(void) { return 7; }
EOF
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/ntdll.so \
  "$test_root/ntdll.c" -o "$test_root/ntdll.so"
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -nostdlib -O2 -Wall -Wextra -Werror \
  -mmacosx-version-min=26.5 -Wl,-install_name,@rpath/winemetal-wow64.so \
  -Wl,-rpath,@loader_path/ "$test_root/winemetal-wow64.c" "$test_root/ntdll.so" \
  -framework Foundation -framework Metal -framework CoreFoundation \
  -lSystem -lobjc -o "$fixture_companion"
/usr/bin/xcrun --sdk macosx clang -arch arm64 -dynamiclib -nostdlib -O2 -Wall -Wextra -Werror \
  -DEXTRA_COMPANION_EXPORT=1 -mmacosx-version-min=26.5 \
  -Wl,-install_name,@rpath/winemetal-wow64.so -Wl,-rpath,@loader_path/ \
  "$test_root/winemetal-wow64.c" "$test_root/ntdll.so" \
  -framework Foundation -framework Metal -framework CoreFoundation \
  -lSystem -lobjc -o "$fixture_companion_extra"

cleanup() {
  local status=$?

  trap - EXIT HUP INT TERM
  case "$test_root" in
    /private/tmp/switchyard-dxmt-policy.??????)
      [ ! -L "$test_root" ] && /bin/rm -rf -- "$test_root"
      ;;
    *) echo "refusing to clean unexpected DXMT test root: $test_root" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

write_manifest() {
  /usr/bin/python3 - "$runtime" <<'PY'
import hashlib
import json
import os
import sys

runtime = sys.argv[1]
revision = "856d9f35789679ef00c1ba01a6353438df84b66f"
artifact_build_identity = "af8ab67d197a4bc6751483b8c16fa17df3b0a6b0"
module_sources = [
    ("lib/wine/aarch64-unix/winemetal.so", "1c03a178db45540507e3784ed97890ee4fd8baffa1413e00991b6588c95859d0", "mach-o-dylib", "arm64"),
    ("lib/wine/aarch64-windows/d3d10core.dll", "38d9576da3adea9431e55007908286679ec3ecd32babff8038d32c25e967d540", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/d3d11.dll", "a55336ef712820d3fd33862374b5ad09122da4e8a26bf5d151caf5d5859ed653", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/dxgi.dll", "a0f29706e04a547bf789ddf399e80e9d15c85b6a1a0d0ddf8f7d6f636cf01cc1", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/nvapi64.dll", "f4e1cf79244d378c660b5d9b6c98923e29f2bd30e9073dadf62ac1879ffd9f02", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/nvngx.dll", "b8ddc2d81dcf4306b58398b486299f31067617e4f5e66cd64c8e5eacde2a0c0c", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/winemetal.dll", "5b46f00c1217e16bba8ae42f7084343750f2516faf8884c7d66cb3ba88128ac2", "pe-dll", "arm64ec"),
    ("lib/wine/i386-windows/d3d10core.dll", "15d74460b922bc46b1e563413320d61b460b8e2b3218b33a5e38070be56f59d5", "pe-dll", "i386"),
    ("lib/wine/i386-windows/d3d11.dll", "55ee41c57f5771ea2d8e277b9644a2ba9a0896b5a96b5edf24a3f55f12d6867e", "pe-dll", "i386"),
    ("lib/wine/i386-windows/dxgi.dll", "1819cf644d78d3a0cdb1aed5356e028a3603da7657aa0e503621302321081655", "pe-dll", "i386"),
    ("lib/wine/i386-windows/winemetal.dll", "79c261baa6eaec1ec9213debe241249b1d582a17f2baaf3359aee8d103d76869", "pe-dll", "i386"),
    ("lib/wine/x86_64-windows/d3d10core.dll", "4910ce0b1960a627c61114b019869057be8e1bf2edddd2ecb348c434bb98e5e0", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/d3d11.dll", "26b88098961e936b3bfe0ad984d3ad2a4568f10b04a4e6f7fa54711a9c17b583", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/dxgi.dll", "19ffb16b5dd22c944b284d9ea6d7b301e2ad96ef68f65ebdb642db49c55a9491", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/nvapi64.dll", "6e1bb14e6fb6c6f64d30e67aa351550d85d7d32d43ae429831f9ca49550ed323", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/nvngx.dll", "97e48d69a527e82b4269f50b1e1d5041e594e5a1dac5b51fce43008d372733d6", "pe-dll", "x86_64"),
    ("lib/wine/x86_64-windows/winemetal.dll", "34c66a7e56d1c0315f160775be009cf92efc56ec9396c2d61b6f03c307abefed", "pe-dll", "x86_64"),
]
load_commands = [
    {"command": "LC_LOAD_DYLIB", "path": "@rpath/winemac.so"},
    {"command": "LC_LOAD_DYLIB", "path": "@rpath/ntdll.so"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/Metal.framework/Versions/A/Metal"},
    {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/MetalFX.framework/Versions/A/MetalFX"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/ColorSync.framework/Versions/A/ColorSync"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libsqlite3.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libSystem.B.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libz.1.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libncurses.5.4.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libxml2.2.dylib"},
    {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libc++.1.dylib"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit"},
    {"command": "LC_LOAD_WEAK_DYLIB", "path": "/usr/lib/libobjc.A.dylib"},
]

def digest(relative):
    value = hashlib.sha256()
    with open(os.path.join(runtime, relative), "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()

modules = []
for path, source_digest, file_format, architecture in module_sources:
    item = {
        "path": path,
        "sha256": digest(path),
        "sourceSha256": source_digest,
        "format": file_format,
        "architecture": architecture,
    }
    if file_format == "mach-o-dylib":
        item.update({
            "platform": "macos",
            "minimumMacOS": "15.0",
            "sdk": "15.1",
            "installName": "@rpath/winemetal.so",
            "rpaths": ["@loader_path/", "@loader_path/../../"],
            "loadCommands": load_commands,
        })
    modules.append(item)

files_manifest = "lib/switchyard-dxmt/share/doc/switchyard-dxmt/files.sha256"
companion_path = "lib/wine/aarch64-unix/winemetal-wow64.so"
schema_path = "lib/switchyard-dxmt/share/doc/switchyard-dxmt/abi-schema-v6.txt"
source_patch = "0001-dxmt-Preserve-guest-accessible-CPU-buffer-ownership.patch"
source_patch_path = "lib/switchyard-dxmt/share/src/switchyard-dxmt/" + source_patch
companion = {
    "path": companion_path,
    "sha256": digest(companion_path),
    "format": "mach-o-dylib",
    "architecture": "arm64",
    "minimumMacOS": "26.5",
    "sdk": "26.5",
    "installName": "@rpath/winemetal-wow64.so",
    "rpaths": ["@loader_path/"],
    "loadCommands": [
        {"command": "LC_LOAD_DYLIB", "path": "@rpath/ntdll.so"},
        {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation"},
        {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/Metal.framework/Versions/A/Metal"},
        {"command": "LC_LOAD_DYLIB", "path": "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"},
        {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libSystem.B.dylib"},
        {"command": "LC_LOAD_DYLIB", "path": "/usr/lib/libobjc.A.dylib"},
    ],
    "originalUnixLibrary": module_sources[0][0],
    "originalSha256": modules[0]["sha256"],
    "abiSchema": schema_path,
    "abiSchemaSha256": digest(schema_path),
    "entryCount": 138,
    "dispatchSourceVersion": 2,
    "bindingVersion": 4,
    "codeSignature": "strict",
}
with open(os.path.join(runtime, files_manifest), "w", encoding="ascii", newline="\n") as stream:
    for item in modules:
        stream.write(f"{item['sha256']}  {item['path']}\n")
    stream.write(f"{companion['sha256']}  {companion_path}\n")
    stream.write(f"{companion['abiSchemaSha256']}  {schema_path}\n")
    stream.write(f"{digest(source_patch_path)}  {source_patch_path}\n")

corresponding_source = "lib/switchyard-dxmt/share/doc/switchyard-dxmt/CORRESPONDING-SOURCE.txt"
with open(os.path.join(runtime, corresponding_source), "w", encoding="utf-8", newline="\n") as stream:
    stream.write("""DXMT corresponding source and artifact provenance

Upstream repository: https://github.com/3Shain/dxmt.git
Upstream revision: 856d9f35789679ef00c1ba01a6353438df84b66f
Upstream tree: 22fa93d36867f175c0283b36cd3628a4df94876e
Upstream source URL: https://github.com/3Shain/dxmt/tree/856d9f35789679ef00c1ba01a6353438df84b66f
Artifact build label: af8ab67d197a4bc6751483b8c16fa17df3b0a6b0
Patched source tree: 2c91e88660daecc3d492de23d32f9e2fed0dd001
Artifact build label semantics: opaque local artifact identifier; no Git object is required
Patch: lib/switchyard-dxmt/share/src/switchyard-dxmt/0001-dxmt-Preserve-guest-accessible-CPU-buffer-ownership.patch
Patch SHA-256: 2e6f6436706f283be6b9ca1668391e0fa70fe83e290781d2a2c5b9f2496a4a26
Reconstruction contract: upstream revision plus the pinned patch yields the patched source tree
DirectX headers submodule: 9df86f2341616ef1888ae59919feaa6d4fad693d
NVAPI submodule: d08488fcc82eef313b0464db37d2955709691e94
Base artifact: dxmt-856d9f35789679ef00c1ba01a6353438df84b66f.tar.gz
Base artifact SHA-256: 8840df7038d7cbffed3652712c86ec4d6d495612aa39306e9a184bd213514acf
Artifact: dxmt-af8ab67d197a4bc6751483b8c16fa17df3b0a6b0.tar.gz
Artifact SHA-256: 3d1b73a42b25ff6b90c1c054ec501abfffe1f8433cbf3c97183b69e00d57f778
Package workflow path: .github/workflows/ci.yml
Package workflow SHA-256: fe5a3656b9f59e81e650e60077bcdd840a5205ff0d960f00f6cb4c8fbacbe851
Package build: gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+clang-release-arm64ec-windows-cross

To reconstruct the corresponding source, check out the public upstream
revision, apply the pinned patch, initialize the submodules at the commits
recorded above, and verify that the resulting superproject Git tree is the
patched source tree. The artifact build label does not identify a required Git
object, and no unpublished commit metadata or history is needed or promised.

The i386 and ARM64EC PE modules were rebuilt from the patched source tree. The
retained Unix, x86_64 PE, ARM64EC NVAPI, and ARM64EC NVNGX modules are
byte-for-byte identical to the base artifact. The host Mach-O module may differ
only through the runtime's validated code-signing
step; its final digest is recorded in both files.sha256 and
switchyard-runtime.json. DXMT is licensed under LGPL-2.1-or-later; LICENSE and
COPYING.LIB are retained in this directory.
""")

document_paths = [
    files_manifest,
    "lib/switchyard-dxmt/share/doc/switchyard-dxmt/LICENSE",
    "lib/switchyard-dxmt/share/doc/switchyard-dxmt/COPYING.LIB",
    corresponding_source,
    schema_path,
]
documents = [{"path": path, "sha256": digest(path)} for path in document_paths]
value = {
    "manifestVersion": 2,
    "id": "switchyard-local-native-arm64-fex-dxmt-test",
    "runtimeFamily": "preview-native-arm64-fex",
    "buildProfile": "switchyard-native-arm64-fex",
    "graphicsBackend": "dxmt-metal",
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
    "dxmt": {
        "contractVersion": 2,
        "implementation": "dxmt",
        "graphicsApi": "d3d11",
        "hostBackend": "metal",
        "provenance": {
            "sourceRepository": "https://github.com/3Shain/dxmt.git",
            "sourceBaseTree": "22fa93d36867f175c0283b36cd3628a4df94876e",
            "sourceRevision": revision,
            "sourceTree": "2c91e88660daecc3d492de23d32f9e2fed0dd001",
            "sourcePatch": source_patch_path,
            "sourcePatchSha256": "2e6f6436706f283be6b9ca1668391e0fa70fe83e290781d2a2c5b9f2496a4a26",
            "artifactBuildIdentity": artifact_build_identity,
            "artifactName": f"dxmt-{artifact_build_identity}.tar.gz",
            "artifactSha256": "3d1b73a42b25ff6b90c1c054ec501abfffe1f8433cbf3c97183b69e00d57f778",
            "packageWorkflow": ".github/workflows/ci.yml",
            "packageWorkflowSha256": "fe5a3656b9f59e81e650e60077bcdd840a5205ff0d960f00f6cb4c8fbacbe851",
            "packageBuild": "gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+clang-release-arm64ec-windows-cross",
        },
        "license": "LGPL-2.1-or-later",
        "sourceMaterials": [{
            "path": source_patch_path,
            "sha256": "2e6f6436706f283be6b9ca1668391e0fa70fe83e290781d2a2c5b9f2496a4a26",
            "type": "patch",
        }],
        "modules": modules,
        "wow64Companion": companion,
        "documents": documents,
    },
}
with open(os.path.join(runtime, "switchyard-runtime.json"), "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
}

reset_runtime() {
  case "$runtime" in
    "$test_root/runtime") ;;
    *) echo "refusing to reset unexpected DXMT test runtime: $runtime" >&2; exit 1 ;;
  esac
  /bin/rm -rf -- "$runtime"
  /bin/mkdir -p "$runtime"
  /bin/cp -R "$STAGED_FIXTURE/." "$runtime/"
  /usr/bin/install -m 0755 "$fixture_companion" \
    "$runtime/lib/wine/aarch64-unix/winemetal-wow64.so"
  /usr/bin/install -m 0644 "$ROOT_DIR/dlls/winemetal-wow64/abi-schema-v6.txt" \
    "$runtime/lib/switchyard-dxmt/share/doc/switchyard-dxmt/abi-schema-v6.txt"
  /bin/mkdir -p "$runtime/lib/switchyard-dxmt/share/src/switchyard-dxmt"
  /usr/bin/install -m 0644 "$ROOT_DIR/switchyard/patches/0001-dxmt-Preserve-guest-accessible-CPU-buffer-ownership.patch" \
    "$runtime/lib/switchyard-dxmt/share/src/switchyard-dxmt/0001-dxmt-Preserve-guest-accessible-CPU-buffer-ownership.patch"
  write_manifest
}

validate_runtime() {
  switchyard_validate_dxmt_runtime_manifest "$runtime" "$manifest"
}

expect_failure() {
  local label="$1"
  local expected="$2"

  if validate_runtime >"$failure_log" 2>&1; then
    echo "DXMT negative fixture unexpectedly passed: $label" >&2
    exit 1
  fi
  /usr/bin/grep -F "$expected" "$failure_log" >/dev/null || {
    echo "DXMT negative fixture failed at the wrong gate: $label" >&2
    /bin/cat "$failure_log" >&2
    exit 1
  }
}

wait_for_aba_barrier() {
  local shell_pid="$1"
  local attempt candidate state children

  attempt=0
  while [ "$attempt" -lt 500 ]; do
    attempt=$((attempt + 1))
    children="$(/usr/bin/pgrep -P "$shell_pid" 2>/dev/null || true)"
    for candidate in $shell_pid $children; do
      state="$(/bin/ps -o state= -p "$candidate" 2>/dev/null | /usr/bin/tr -d '[:space:]')"
      case "$state" in
        T*) /usr/bin/printf '%s\n' "$candidate"; return 0 ;;
      esac
    done
    if ! /bin/kill -0 "$shell_pid" 2>/dev/null; then
      break
    fi
    /bin/sleep 0.01
  done
  echo "DXMT ABA regression did not reach its deterministic validator barrier." >&2
  return 1
}

mutate_manifest() {
  local expression="$1"

  /usr/bin/python3 - "$manifest" "$expression" <<'PY'
import json
import sys

path, expression = sys.argv[1:]
with open(path, encoding="utf-8") as stream:
    value = json.load(stream)
exec(expression, {"value": value})
with open(path, "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
}

reset_runtime
validate_runtime

reset_runtime
/usr/bin/install -m 0755 "$fixture_companion_extra" \
  "$runtime/lib/wine/aarch64-unix/winemetal-wow64.so"
write_manifest
expect_failure "extra DXMT WoW64 companion export" "companion export set is not exact"

/bin/rm "$runtime/lib/wine/aarch64-unix/winemetal-wow64.so"
expect_failure "missing DXMT WoW64 companion" "failed safely"

reset_runtime
/usr/bin/printf 'tampered\n' >>"$runtime/lib/wine/aarch64-unix/winemetal-wow64.so"
expect_failure "tampered DXMT WoW64 companion" "companion digest mismatch"

reset_runtime
/bin/mv "$runtime/lib/wine/aarch64-unix/winemetal-wow64.so" \
  "$test_root/winemetal-wow64.target"
/bin/ln -s "$test_root/winemetal-wow64.target" \
  "$runtime/lib/wine/aarch64-unix/winemetal-wow64.so"
expect_failure "symbolic-link DXMT WoW64 companion" "failed safely"

reset_runtime
/usr/bin/printf 'tampered\n' >> \
  "$runtime/lib/switchyard-dxmt/share/doc/switchyard-dxmt/abi-schema-v6.txt"
expect_failure "tampered DXMT WoW64 schema" "document digest mismatch"

source_patch_path="$runtime/lib/switchyard-dxmt/share/src/switchyard-dxmt/0001-dxmt-Preserve-guest-accessible-CPU-buffer-ownership.patch"

reset_runtime
/bin/rm "$source_patch_path"
expect_failure "missing DXMT source patch" "failed safely"

reset_runtime
/usr/bin/printf 'tampered\n' >>"$source_patch_path"
expect_failure "tampered DXMT source patch" "source material differs from the pinned source"

reset_runtime
/bin/mv "$source_patch_path" "$test_root/dxmt-source-patch.good"
/bin/ln -s "$test_root/dxmt-source-patch.good" "$source_patch_path"
expect_failure "symbolic-link DXMT source patch" "failed safely"

reset_runtime
/bin/chmod 0664 "$source_patch_path"
expect_failure "group-writable DXMT source patch" "group/world writable"

reset_runtime
/usr/bin/printf 'unexpected\n' > \
  "$runtime/lib/switchyard-dxmt/share/src/switchyard-dxmt/unexpected.patch"
expect_failure "extra DXMT source material" "unexpected entry set"

reset_runtime
mutate_manifest 'value["dxmt"]["provenance"]["sourceTree"] = "0000000000000000000000000000000000000000"'
expect_failure "wrong reconstructed DXMT source tree" "provenance field is invalid: sourceTree"

reset_runtime
mutate_manifest 'value["dxmt"].pop("sourceMaterials")'
expect_failure "missing DXMT source material closure" "unexpected field set"

reset_runtime
mutate_manifest 'value["dxmt"]["unexpected"] = True'
expect_failure "unknown DXMT key" "unexpected field set"

reset_runtime
/usr/bin/python3 - "$manifest" <<'PY'
import sys
path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    text = stream.read()
needle = '    "contractVersion": 2,\n'
if text.count(needle) != 1:
    raise SystemExit("cannot build duplicate-key fixture")
with open(path, "w", encoding="utf-8", newline="\n") as stream:
    stream.write(text.replace(needle, needle + needle, 1))
PY
expect_failure "duplicate manifest key" "duplicate JSON object key"

reset_runtime
mutate_manifest 'value["graphicsBackend"] = "gptk-d3dmetal"'
expect_failure "wrong graphics backend" "graphicsBackend dxmt-metal"

reset_runtime
mutate_manifest 'value["dxmt"]["modules"][0]["minimumMacOS"] = "14.0"'
expect_failure "stale Mach-O minOS" "Mach-O metadata is invalid: minimumMacOS"

reset_runtime
mutate_manifest 'value["dxmt"]["modules"][0]["rpaths"].append("@executable_path")'
expect_failure "extra Mach-O rpath" "rpaths are not the exact allowlist"

reset_runtime
mutate_manifest 'value["dxmt"]["modules"][0]["loadCommands"].pop()'
expect_failure "missing Mach-O load command" "load commands are not the exact allowlist"

reset_runtime
/usr/bin/python3 - "$runtime/lib/wine/aarch64-unix/winemetal.so" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
command_count = struct.unpack_from("<I", data, 16)[0]
command_offset = 32
for _index in range(command_count):
    command, command_size = struct.unpack_from("<II", data, command_offset)
    if command == 0x32:
        struct.pack_into("<I", data, command_offset + 12, 14 << 16)
        break
    command_offset += command_size
else:
    raise SystemExit("fixture has no LC_BUILD_VERSION")
with open(path, "wb") as stream:
    stream.write(data)
PY
write_manifest
expect_failure "actual Mach-O minOS mismatch" "unexpected platform, minOS, or SDK"

reset_runtime
/usr/bin/python3 - "$runtime/lib/wine/aarch64-unix/winemetal.so" \
  '@loader_path/../../' '@loader_path/../bad' <<'PY'
import sys

path, old, new = sys.argv[1:]
with open(path, "rb") as stream:
    data = stream.read()
if len(old) != len(new) or data.count(old.encode("ascii")) != 1:
    raise SystemExit("cannot build LC_RPATH mutation fixture")
with open(path, "wb") as stream:
    stream.write(data.replace(old.encode("ascii"), new.encode("ascii"), 1))
PY
write_manifest
expect_failure "actual Mach-O rpath mismatch" "unexpected LC_RPATH closure"

reset_runtime
/usr/bin/python3 - "$runtime/lib/wine/aarch64-unix/winemetal.so" \
  '@rpath/ntdll.so' '@rpath/evil_.so' <<'PY'
import sys

path, old, new = sys.argv[1:]
with open(path, "rb") as stream:
    data = stream.read()
if len(old) != len(new) or data.count(old.encode("ascii")) != 1:
    raise SystemExit("cannot build dylib-load mutation fixture")
with open(path, "wb") as stream:
    stream.write(data.replace(old.encode("ascii"), new.encode("ascii"), 1))
PY
write_manifest
expect_failure "actual Mach-O load mismatch" "dylib/framework load-command closure"

reset_runtime
/usr/bin/printf 'tamper' >>"$runtime/lib/wine/x86_64-windows/d3d11.dll"
expect_failure "module digest mismatch" "module digest mismatch"

reset_runtime
/bin/mv "$runtime/lib/wine/aarch64-unix/winemetal.so" \
  "$runtime/lib/wine/aarch64-unix/winemetal.so.real"
/bin/ln -s winemetal.so.real "$runtime/lib/wine/aarch64-unix/winemetal.so"
expect_failure "module symlink" "failed safely"

reset_runtime
/bin/mkdir -p "$runtime/lib/wine/x86_64-unix"
/bin/cp "$ARCHIVE_FIXTURE/x86_64-unix/winemetal.so" \
  "$runtime/lib/wine/x86_64-unix/winemetal.so"
expect_failure "forbidden x86_64 Unix module" "module closure mismatch"

reset_runtime
/bin/cp "$ARCHIVE_FIXTURE/x86_64-unix/winemetal.so" \
  "$runtime/lib/wine/aarch64-unix/winemetal.so"
write_manifest
expect_failure "wrong host Mach-O architecture" "thin arm64 MH_DYLIB"

reset_runtime
/usr/bin/python3 - "$runtime/lib/wine/aarch64-unix/winemetal.so" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
command_count = struct.unpack_from("<I", data, 16)[0]
command_offset = 32
signature_offset = None
for _index in range(command_count):
    command, command_size = struct.unpack_from("<II", data, command_offset)
    if command == 0x1D:
        signature_offset = struct.unpack_from("<I", data, command_offset + 8)[0]
        break
    command_offset += command_size
if signature_offset is None:
    raise SystemExit("fixture has no LC_CODE_SIGNATURE")
data[signature_offset + 16] ^= 1
with open(path, "wb") as stream:
    stream.write(data)
PY
write_manifest
expect_failure "invalid host Mach-O signature" "valid embedded code signature"

reset_runtime
/bin/cp "$runtime/lib/wine/aarch64-unix/winemetal.so" "$test_root/winemetal-aba-signed.good"
/usr/bin/python3 - "$runtime/lib/wine/aarch64-unix/winemetal.so" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    data = bytearray(stream.read())
command_count = struct.unpack_from("<I", data, 16)[0]
command_offset = 32
for _index in range(command_count):
    command, command_size = struct.unpack_from("<II", data, command_offset)
    if command == 0x1D:
        signature_offset = struct.unpack_from("<I", data, command_offset + 8)[0]
        data[signature_offset + 16] ^= 1
        break
    command_offset += command_size
else:
    raise SystemExit("fixture has no LC_CODE_SIGNATURE")
with open(path, "wb") as stream:
    stream.write(data)
PY
write_manifest
aba_log="$test_root/aba-validation.log"
# shellcheck disable=SC2016 # Positional arguments are expanded by the child shell.
env SWITCHYARD_DXMT_TEST_ABA_BARRIERS=1 /bin/bash -c '
  source "$1"
  switchyard_validate_dxmt_runtime_manifest "$2" "$3"
' _ "$POLICY_LIBRARY" "$runtime" "$manifest" >"$aba_log" 2>&1 &
aba_shell_pid=$!
aba_python_pid="$(wait_for_aba_barrier "$aba_shell_pid")"
/bin/mv "$runtime/lib/wine/aarch64-unix/winemetal.so" "$test_root/winemetal-invalid.held"
/bin/cp "$test_root/winemetal-aba-signed.good" \
  "$runtime/lib/wine/aarch64-unix/winemetal.so"
/bin/kill -CONT "$aba_python_pid"
aba_python_pid="$(wait_for_aba_barrier "$aba_shell_pid")"
/bin/rm -f "$runtime/lib/wine/aarch64-unix/winemetal.so"
/bin/mv "$test_root/winemetal-invalid.held" \
  "$runtime/lib/wine/aarch64-unix/winemetal.so"
/bin/kill -CONT "$aba_python_pid"
set +e
wait "$aba_shell_pid"
aba_status=$?
set -e
[ "$aba_status" -ne 0 ] || {
  echo "DXMT validation accepted an invalid snapshot through a live-path ABA swap." >&2
  exit 1
}
/usr/bin/grep -F "valid embedded code signature" "$aba_log" >/dev/null || {
  /bin/cat "$aba_log" >&2
  echo "DXMT ABA regression failed at the wrong validation gate." >&2
  exit 1
}

reset_runtime
root_replacement="$test_root/runtime-root-replacement"
root_original="$test_root/runtime-root-original.held"
/bin/cp -R "$runtime" "$root_replacement"
/bin/rm "$root_replacement/lib/wine/aarch64-unix/winemetal.so"
/bin/ln "$runtime/lib/wine/aarch64-unix/winemetal.so" \
  "$root_replacement/lib/wine/aarch64-unix/winemetal.so"
/usr/bin/printf 'tamper' >> \
  "$root_replacement/lib/wine/x86_64-windows/d3d11.dll"
root_aba_log="$test_root/root-aba-validation.log"
# shellcheck disable=SC2016 # Positional arguments are expanded by the child shell.
env SWITCHYARD_DXMT_TEST_ABA_BARRIERS=1 /bin/bash -c '
  source "$1"
  switchyard_validate_dxmt_runtime_manifest "$2" "$3"
' _ "$POLICY_LIBRARY" "$runtime" "$manifest" >"$root_aba_log" 2>&1 &
root_aba_shell_pid=$!
root_aba_python_pid="$(wait_for_aba_barrier "$root_aba_shell_pid")"
/bin/mv "$runtime" "$root_original"
/bin/mv "$root_replacement" "$runtime"
/bin/kill -CONT "$root_aba_python_pid"
root_aba_python_pid="$(wait_for_aba_barrier "$root_aba_shell_pid")"
/bin/kill -CONT "$root_aba_python_pid"
set +e
wait "$root_aba_shell_pid"
root_aba_status=$?
set -e
[ "$root_aba_status" -ne 0 ] || {
  echo "DXMT validation accepted a replaced live runtime root." >&2
  exit 1
}
/usr/bin/grep -E \
  'runtime root changed during validation|DXMT WoW64 companion changed while it was inspected' \
  "$root_aba_log" >/dev/null || {
  /bin/cat "$root_aba_log" >&2
  echo "DXMT root-replacement regression failed at the wrong validation gate." >&2
  exit 1
}
/bin/rm -rf -- "$root_original"

reset_runtime
/usr/bin/touch "$runtime/lib/switchyard-dxmt/share/doc/switchyard-dxmt/EXTRA"
expect_failure "unknown DXMT document" "unexpected entry set"

reset_runtime
/bin/chmod 0666 "$runtime/lib/switchyard-dxmt/share/doc/switchyard-dxmt/LICENSE"
expect_failure "writable DXMT document" "group/world writable"

reset_runtime
/bin/cp "$runtime/lib/wine/x86_64-windows/nvngx.dll" \
  "$runtime/lib/wine/i386-windows/nvngx.dll"
expect_failure "unknown DXMT module" "module closure mismatch"

reset_runtime
/usr/bin/printf '\nchanged\n' >> \
  "$runtime/lib/switchyard-dxmt/share/doc/switchyard-dxmt/CORRESPONDING-SOURCE.txt"
/usr/bin/python3 - "$manifest" \
  "$runtime/lib/switchyard-dxmt/share/doc/switchyard-dxmt/CORRESPONDING-SOURCE.txt" <<'PY'
import hashlib
import json
import sys

manifest, document = sys.argv[1:]
with open(document, "rb") as stream:
    digest = hashlib.sha256(stream.read()).hexdigest()
with open(manifest, encoding="utf-8") as stream:
    value = json.load(stream)
value["dxmt"]["documents"][3]["sha256"] = digest
with open(manifest, "w", encoding="utf-8", newline="\n") as stream:
    json.dump(value, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
expect_failure "changed provenance document" "differs from the pinned source"

reset_runtime
/bin/mv "$manifest" "$manifest.real"
/bin/ln -s switchyard-runtime.json.real "$manifest"
expect_failure "manifest symlink" "failed safely"

reset_runtime
/bin/mkdir "$test_root/real-parent"
/bin/mv "$runtime" "$test_root/real-parent/runtime"
/bin/ln -s real-parent "$test_root/linked-parent"
runtime="$test_root/linked-parent/runtime"
manifest="$runtime/switchyard-runtime.json"
expect_failure "runtime parent symlink" "symbolic-link component"

echo "DXMT artifact policy tests passed"
