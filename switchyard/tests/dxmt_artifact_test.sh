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
module_sources = [
    ("lib/wine/aarch64-unix/winemetal.so", "1c03a178db45540507e3784ed97890ee4fd8baffa1413e00991b6588c95859d0", "mach-o-dylib", "arm64"),
    ("lib/wine/aarch64-windows/d3d10core.dll", "0ca52517ce266d63b85310a8aae940e92b0a05392d1d03698dbc4156ce28a959", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/d3d11.dll", "bb74a3835c731d7dfe19e9d928cf20a4eef6d37c88edddfcf112557408a01fc6", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/dxgi.dll", "9c374cc1896dca4129fd5c810c09e8dce9df6b04398ddb1207da6bce01e15e3c", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/nvapi64.dll", "f4e1cf79244d378c660b5d9b6c98923e29f2bd30e9073dadf62ac1879ffd9f02", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/nvngx.dll", "b8ddc2d81dcf4306b58398b486299f31067617e4f5e66cd64c8e5eacde2a0c0c", "pe-dll", "arm64ec"),
    ("lib/wine/aarch64-windows/winemetal.dll", "64007d8901b691bd91aac8218bddb12e2cce272fbdaab8a7bdc3f0ca6fe3eb99", "pe-dll", "arm64ec"),
    ("lib/wine/i386-windows/d3d10core.dll", "77a7c58a8ee649a2959017a91211f5003bf988010a090447b78fa00ca8a7544b", "pe-dll", "i386"),
    ("lib/wine/i386-windows/d3d11.dll", "3f42b073b2954d7b27fa00380d4e268b6f8f2216d701b2c57176c9f3c83b49fb", "pe-dll", "i386"),
    ("lib/wine/i386-windows/dxgi.dll", "c6ba805aafd21668d487252747fadba3ee4525a55c7bfdf6f65ec26e140a39ff", "pe-dll", "i386"),
    ("lib/wine/i386-windows/winemetal.dll", "99db6924a2726d534562f9168692c5c1b4d4651d40a55133a8887e7621c9bc2f", "pe-dll", "i386"),
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
with open(os.path.join(runtime, files_manifest), "w", encoding="ascii", newline="\n") as stream:
    for item in modules:
        stream.write(f"{item['sha256']}  {item['path']}\n")

corresponding_source = "lib/switchyard-dxmt/share/doc/switchyard-dxmt/CORRESPONDING-SOURCE.txt"
with open(os.path.join(runtime, corresponding_source), "w", encoding="utf-8", newline="\n") as stream:
    stream.write("""DXMT corresponding source and artifact provenance

Repository: https://github.com/3Shain/dxmt.git
Revision: 856d9f35789679ef00c1ba01a6353438df84b66f
Source URL: https://github.com/3Shain/dxmt/tree/856d9f35789679ef00c1ba01a6353438df84b66f
Artifact: dxmt-856d9f35789679ef00c1ba01a6353438df84b66f.tar.gz
Artifact SHA-256: 8840df7038d7cbffed3652712c86ec4d6d495612aa39306e9a184bd213514acf
Package workflow: .github/workflows/ci.yml
Package workflow SHA-256: fe5a3656b9f59e81e650e60077bcdd840a5205ff0d960f00f6cb4c8fbacbe851
Package build: gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+clang-release-arm64ec-windows-cross

The PE modules are byte-for-byte files from the pinned artifact. The host
Mach-O module is derived from that artifact and may differ only through the
runtime's validated code-signing step; its final digest is recorded in both
files.sha256 and switchyard-runtime.json. DXMT is licensed under
LGPL-2.1-or-later; LICENSE and COPYING.LIB are retained in this directory.
""")

document_paths = [
    files_manifest,
    "lib/switchyard-dxmt/share/doc/switchyard-dxmt/LICENSE",
    "lib/switchyard-dxmt/share/doc/switchyard-dxmt/COPYING.LIB",
    corresponding_source,
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
        "contractVersion": 1,
        "implementation": "dxmt",
        "graphicsApi": "d3d11",
        "hostBackend": "metal",
        "provenance": {
            "sourceRepository": "https://github.com/3Shain/dxmt.git",
            "sourceRevision": revision,
            "artifactName": f"dxmt-{revision}.tar.gz",
            "artifactSha256": "8840df7038d7cbffed3652712c86ec4d6d495612aa39306e9a184bd213514acf",
            "packageWorkflow": ".github/workflows/ci.yml",
            "packageWorkflowSha256": "fe5a3656b9f59e81e650e60077bcdd840a5205ff0d960f00f6cb4c8fbacbe851",
            "packageBuild": "gcc-release-x86_64-windows-cross+gcc-release-x86-windows-cross+clang-release-arm64ec-windows-cross",
        },
        "license": "LGPL-2.1-or-later",
        "modules": modules,
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

mutate_manifest 'value["dxmt"]["unexpected"] = True'
expect_failure "unknown DXMT key" "unexpected field set"

reset_runtime
/usr/bin/python3 - "$manifest" <<'PY'
import sys
path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    text = stream.read()
needle = '    "contractVersion": 1,\n'
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
/usr/bin/grep -F "runtime root changed during validation" \
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
