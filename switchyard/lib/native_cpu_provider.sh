#!/usr/bin/env bash

# Runtime qualification for the preview native-arm64 CPU provider and its
# high-shadow WoW64 Unixlib dispatch policy.  This library is intentionally
# read-only: build and release code owns manifest production and hash refresh.

# shellcheck disable=SC2034 # Public contract constants are consumed by callers and tests.
SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY="lib/wine/aarch64-unix/xtajit.so"
SWITCHYARD_NATIVE_XTAJIT_PE_LIBRARY="lib/wine/aarch64-windows/xtajit.dll"
SWITCHYARD_NATIVE_XTAJIT64_UNIX_LIBRARY="lib/wine/aarch64-unix/xtajit64.so"
SWITCHYARD_NATIVE_XTAJIT64_PE_LIBRARY="lib/wine/aarch64-windows/xtajit64.dll"
SWITCHYARD_NATIVE_XTAJIT64_ABI_VERSION="10"
SWITCHYARD_NATIVE_XTAJIT64_ABI_IDENTITY="switchyard-xtajit64-provider-abi-v10-flight-bind-process-init-96-begin-472-doorbell"
SWITCHYARD_NATIVE_UNICORN_ROOT="lib/switchyard-unicorn"
SWITCHYARD_NATIVE_UNICORN_LIBRARY="lib/switchyard-unicorn/lib/libunicorn.2.dylib"
SWITCHYARD_NATIVE_UNICORN_RPATH='@loader_path/../../switchyard-unicorn/lib'
SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH="lib/switchyard-unicorn/share/src/switchyard-unicorn/unicorn-2.1.4-threaded-emu-stop.patch"
SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH_SHA256="96a647d57f6f749c3c3864ead959c2e9306488151f5fed468e6ad334483e6cc5"
SWITCHYARD_WOW64_UNIXLIB_POLICY_CONTRACT_VERSION="2"
SWITCHYARD_WOW64_UNIXLIB_POLICY_HANDLE_ENCODING="generation-tagged-v1"
SWITCHYARD_WOW64_UNIXLIB_POLICY_EXTERNAL_SOURCE_VERSION="2"
SWITCHYARD_WOW64_UNIXLIB_POLICY_REQUIRED_ENTRY_FLAG="REVIEWED"

switchyard_native_cpu_provider_load_profile_contract() {
  local library_dir profile_library

  if declare -F switchyard_validate_runtime_manifest_profile >/dev/null; then
    return 0
  fi
  library_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" || return 1
  profile_library="$library_dir/runtime_profile.sh"
  [ -f "$profile_library" ] && [ ! -L "$profile_library" ] || {
    echo "Native CPU-provider runtime-profile validator is missing or unsafe." >&2
    return 1
  }
  # shellcheck disable=SC1090 # The path is fixed next to this policy library.
  source "$profile_library"
}

switchyard_native_cpu_provider_content_digest_helper() {
  local library_dir helper

  library_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" || return 1
  helper="$(cd "$library_dir/.." && pwd -P)/runtime_content_digest.py"
  [ -f "$helper" ] && [ ! -L "$helper" ] && [ -x "$helper" ] || {
    echo "Native CPU-provider content-digest helper is missing or unsafe." >&2
    return 1
  }
  /usr/bin/printf '%s\n' "$helper"
}

switchyard_native_cpu_provider_inspection_tool() {
  local tool path

  [ "$#" -eq 1 ] || {
    echo "usage: switchyard_native_cpu_provider_inspection_tool TOOL" >&2
    return 2
  }
  tool="$1"
  case "$tool" in
    codesign | lipo | nm | otool | vtool) path="/usr/bin/$tool" ;;
    *)
      echo "Unsupported native CPU-provider inspection tool: $tool" >&2
      return 2
      ;;
  esac
  [ -f "$path" ] && [ ! -L "$path" ] && [ -x "$path" ] || {
    echo "Native CPU-provider inspection tool is missing or unsafe: $path" >&2
    return 1
  }
  /usr/bin/printf '%s\n' "$path"
}

switchyard_native_cpu_provider_validate_executable_path() {
  local path description directory canonical

  [ "$#" -eq 2 ] || return 2
  path="$1"
  description="$2"
  [[ "$path" = /* ]] && [[ "$path" != *$'\n'* ]] && [[ "$path" != *$'\r'* ]] &&
    [ -f "$path" ] && [ ! -L "$path" ] && [ -x "$path" ] || {
    echo "$description is missing or unsafe: $path" >&2
    return 1
  }
  directory="$(cd "$(/usr/bin/dirname "$path")" && pwd -P)" || return 1
  canonical="$directory/$(/usr/bin/basename "$path")"
  [ "$canonical" = "$path" ] || {
    echo "$description path is not canonical: $path" >&2
    return 1
  }
}

switchyard_native_cpu_provider_validate_runtime_profile() {
  local manifest runtime_root

  [ "$#" -eq 2 ] || return 2
  manifest="$1"
  runtime_root="$2"
  if [ -n "$(switchyard_runtime_manifest_value runtimeSigning.mode "$manifest")" ]; then
    switchyard_validate_runtime_manifest_profile \
      "$manifest" preview-native-arm64-fex "$runtime_root"
  else
    switchyard_validate_runtime_manifest_profile \
      "$manifest" preview-native-arm64-fex
  fi
}

switchyard_validate_native_cpu_provider_files() {
  local manifest runtime_root digest_helper lipo_tool nm_tool otool_tool vtool_tool

  [ "$#" -eq 2 ] || {
    echo "usage: switchyard_validate_native_cpu_provider_files MANIFEST RUNTIME" >&2
    return 2
  }
  manifest="$1"
  runtime_root="$2"
  switchyard_native_cpu_provider_load_profile_contract || return 1
  switchyard_native_cpu_provider_validate_runtime_profile \
    "$manifest" "$runtime_root" || return 1
  digest_helper="$(switchyard_native_cpu_provider_content_digest_helper)" || return 1
  lipo_tool="$(switchyard_native_cpu_provider_inspection_tool lipo)" || return 1
  nm_tool="$(switchyard_native_cpu_provider_inspection_tool nm)" || return 1
  otool_tool="$(switchyard_native_cpu_provider_inspection_tool otool)" || return 1
  vtool_tool="$(switchyard_native_cpu_provider_inspection_tool vtool)" || return 1
  switchyard_native_cpu_provider_validate_executable_path \
    "$digest_helper" "Native CPU-provider content-digest helper" || return 1
  switchyard_native_cpu_provider_validate_executable_path \
    "$lipo_tool" "Native CPU-provider lipo tool" || return 1
  switchyard_native_cpu_provider_validate_executable_path \
    "$nm_tool" "Native CPU-provider nm tool" || return 1
  switchyard_native_cpu_provider_validate_executable_path \
    "$otool_tool" "Native CPU-provider otool tool" || return 1
  switchyard_native_cpu_provider_validate_executable_path \
    "$vtool_tool" "Native CPU-provider vtool tool" || return 1

  /usr/bin/python3 -I - "$manifest" "$runtime_root" "$digest_helper" \
    "$lipo_tool" "$nm_tool" "$otool_tool" "$vtool_tool" \
    "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS" \
    "$SWITCHYARD_NATIVE_XTAJIT_UNIX_LIBRARY" \
    "$SWITCHYARD_NATIVE_XTAJIT_PE_LIBRARY" \
    "$SWITCHYARD_NATIVE_XTAJIT64_UNIX_LIBRARY" \
    "$SWITCHYARD_NATIVE_XTAJIT64_PE_LIBRARY" \
    "$SWITCHYARD_NATIVE_UNICORN_ROOT" \
    "$SWITCHYARD_NATIVE_UNICORN_LIBRARY" \
    "$SWITCHYARD_NATIVE_UNICORN_RPATH" \
    "$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH" \
    "$SWITCHYARD_NATIVE_UNICORN_SOURCE_PATCH_SHA256" \
    "$SWITCHYARD_NATIVE_XTAJIT64_ABI_VERSION" \
    "$SWITCHYARD_NATIVE_XTAJIT64_ABI_IDENTITY" <<'PY'
import hashlib
import json
import mmap
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile

(
    manifest_name,
    runtime_name,
    digest_helper,
    lipo_tool,
    nm_tool,
    otool_tool,
    vtool_tool,
    minimum_macos,
    xtajit_unix,
    xtajit_pe,
    xtajit64_unix,
    xtajit64_pe,
    unicorn_root,
    unicorn_library,
    unicorn_rpath,
    unicorn_source_patch,
    unicorn_source_patch_sha256,
    xtajit64_abi_version,
    xtajit64_abi_identity,
) = sys.argv[1:]

MAX_MANIFEST = 1024 * 1024
MAX_BINARY = 512 * 1024 * 1024
MAX_TEXT = 4 * 1024 * 1024
MAX_ARCHIVE = 128 * 1024 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}")


def fail(message):
    raise SystemExit("native CPU-provider validation failed: " + message)


def file_identity(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def no_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            fail("duplicate JSON object key: " + key)
        value[key] = item
    return value


def reject_constant(value):
    fail("non-standard JSON constant: " + value)


def load_json(path, maximum, description):
    try:
        info = os.lstat(path)
    except OSError as error:
        fail(f"cannot inspect {description}: {error}")
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
        fail(description + " is not a regular file")
    if info.st_size <= 0 or info.st_size > maximum:
        fail(description + " is outside its size bound")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = -1
    try:
        descriptor = os.open(path, flags)
        before = os.fstat(descriptor)
        if file_identity(info) != file_identity(before):
            fail(description + " changed while opening")
        stream = os.fdopen(descriptor, "r", encoding="utf-8")
        descriptor = -1
        with stream:
            value = json.load(
                stream,
                object_pairs_hook=no_duplicates,
                parse_constant=reject_constant,
            )
            after = os.fstat(stream.fileno())
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot parse {description}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    try:
        path_after = os.lstat(path)
    except OSError as error:
        fail(f"cannot reinspect {description}: {error}")
    if (
        file_identity(before) != file_identity(after)
        or file_identity(after) != file_identity(path_after)
    ):
        fail(description + " changed while parsing")
    return value


def canonical_root(path):
    if (
        not os.path.isabs(path)
        or os.path.normpath(path) != path
        or path == os.path.sep
        or "\n" in path
        or "\r" in path
    ):
        fail("runtime root must be one bounded canonical absolute path")
    try:
        info = os.lstat(path)
    except OSError as error:
        fail(f"cannot inspect runtime root: {error}")
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        fail("runtime root is not a real directory")
    if os.path.realpath(path) != path:
        fail("runtime root contains a symbolic-link component")
    return path


root = canonical_root(runtime_name)
manifest = os.path.abspath(manifest_name)
if manifest != os.path.join(root, "switchyard-runtime.json"):
    fail("manifest is not the runtime root manifest")
value = load_json(manifest, MAX_MANIFEST, "runtime manifest")
if type(value) is not dict or type(value.get("cpuProvider")) is not dict:
    fail("runtime manifest has no CPU-provider object")
provider = value["cpuProvider"]
if provider.get("kuserSharedDataModel") != "translated-shadow":
    fail("CPU-provider KUSER_SHARED_DATA model is not translated-shadow")

snapshot_workspace = tempfile.TemporaryDirectory(
    prefix="switchyard-native-provider.", dir="/private/tmp"
)
snapshot_root = snapshot_workspace.name
snapshot_root_info = os.lstat(snapshot_root)
if (
    not stat.S_ISDIR(snapshot_root_info.st_mode)
    or stat.S_ISLNK(snapshot_root_info.st_mode)
    or snapshot_root_info.st_uid != os.geteuid()
    or stat.S_IMODE(snapshot_root_info.st_mode) != 0o700
):
    fail("private provider validation workspace is unsafe")
snapshot_sequence = 0


def safe_relative(relative):
    return (
        type(relative) is str
        and re.fullmatch(r"[A-Za-z0-9._+/-]+", relative) is not None
        and not relative.startswith("/")
        and "\\" not in relative
        and all(part not in ("", ".", "..") for part in relative.split("/"))
    )


def checked_path(relative, maximum, executable=False):
    if not safe_relative(relative):
        fail("unsafe runtime-relative path: " + repr(relative))
    current = root
    parts = relative.split("/")
    for index, part in enumerate(parts):
        current = os.path.join(current, part)
        try:
            info = os.lstat(current)
        except OSError as error:
            fail(f"missing provider path {relative}: {error}")
        if stat.S_ISLNK(info.st_mode):
            fail("provider path contains a symbolic link: " + relative)
        if index != len(parts) - 1 and not stat.S_ISDIR(info.st_mode):
            fail("provider path component is not a directory: " + relative)
    if not stat.S_ISREG(info.st_mode) or info.st_size <= 0 or info.st_size > maximum:
        fail("provider artifact has an invalid type or size: " + relative)
    if info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        fail("provider artifact is group/world writable: " + relative)
    if executable and not info.st_mode & 0o111:
        fail("provider binary is not executable: " + relative)
    return current, info


def write_all(descriptor, data):
    offset = 0
    while offset < len(data):
        written = os.write(descriptor, data[offset:])
        if written <= 0:
            fail("cannot make progress writing a private provider snapshot")
        offset += written


def digest_regular(relative, maximum=MAX_BINARY, executable=False):
    global snapshot_sequence

    path, path_info = checked_path(relative, maximum, executable)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    snapshot_sequence += 1
    snapshot_name = f"{snapshot_sequence:04d}-{os.path.basename(relative)}"
    snapshot_path = os.path.join(snapshot_root, snapshot_name)
    snapshot_descriptor = -1
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        fail(f"cannot open provider artifact {relative}: {error}")
    digest = hashlib.sha256()
    try:
        before = os.fstat(descriptor)
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_size <= 0
            or before.st_size > maximum
        ):
            fail("provider artifact changed type: " + relative)
        if file_identity(path_info) != file_identity(before):
            fail("provider artifact changed while opening: " + relative)
        snapshot_descriptor = os.open(
            snapshot_path,
            os.O_WRONLY
            | os.O_CREAT
            | os.O_EXCL
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_NOFOLLOW", 0),
            0o400,
        )
        for block in iter(lambda: os.read(descriptor, 1024 * 1024), b""):
            digest.update(block)
            write_all(snapshot_descriptor, block)
        after = os.fstat(descriptor)
        if file_identity(before) != file_identity(after):
            fail("provider artifact changed while hashing: " + relative)
        current = os.lstat(path)
        if file_identity(current) != file_identity(after):
            fail("provider artifact path changed while snapshotting: " + relative)
        snapshot_mode = stat.S_IMODE(before.st_mode) & ~0o222
        os.fchmod(snapshot_descriptor, snapshot_mode)
        os.fsync(snapshot_descriptor)
        snapshot = os.fstat(snapshot_descriptor)
        if (
            not stat.S_ISREG(snapshot.st_mode)
            or snapshot.st_uid != os.geteuid()
            or stat.S_IMODE(snapshot.st_mode) != snapshot_mode
            or snapshot.st_size != before.st_size
            or snapshot.st_nlink != 1
        ):
            fail("private provider snapshot is unsafe: " + relative)
    finally:
        if snapshot_descriptor >= 0:
            os.close(snapshot_descriptor)
        os.close(descriptor)
    return {
        "path": path,
        "digest": digest.hexdigest(),
        "identity": file_identity(after),
        "snapshot": snapshot_path,
        "snapshot_identity": file_identity(snapshot),
    }


def verify_private_snapshot(record, relative):
    path = record["snapshot"]
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = -1
    digest = hashlib.sha256()
    try:
        descriptor = os.open(path, flags)
        opened = os.fstat(descriptor)
        if file_identity(opened) != record["snapshot_identity"]:
            fail("private provider snapshot changed while opening: " + relative)
        for block in iter(lambda: os.read(descriptor, 1024 * 1024), b""):
            digest.update(block)
        after = os.fstat(descriptor)
    except OSError as error:
        fail(f"cannot verify private provider snapshot {relative}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    if file_identity(after) != record["snapshot_identity"] or digest.hexdigest() != record["digest"]:
        fail("private provider snapshot changed during inspection: " + relative)
    live_descriptor = -1
    live_digest = hashlib.sha256()
    try:
        current = os.lstat(record["path"])
        live_descriptor = os.open(record["path"], flags)
        live_opened = os.fstat(live_descriptor)
        if (
            file_identity(current)[:4] != record["identity"][:4]
            or file_identity(live_opened)[:4] != record["identity"][:4]
        ):
            fail("provider artifact path changed during validation: " + relative)
        for block in iter(lambda: os.read(live_descriptor, 1024 * 1024), b""):
            live_digest.update(block)
        live_after = os.fstat(live_descriptor)
        live_path_after = os.lstat(record["path"])
    except OSError as error:
        fail(f"cannot rehash provider artifact {relative}: {error}")
    finally:
        if live_descriptor >= 0:
            os.close(live_descriptor)
    if (
        file_identity(live_after) != file_identity(live_opened)
        or file_identity(live_path_after)[:4] != record["identity"][:4]
        or live_digest.hexdigest() != record["digest"]
    ):
        fail("provider artifact bytes changed during validation: " + relative)
    record["identity"] = file_identity(live_after)


expected_components = [
    ("i386", xtajit_unix, xtajit_pe),
    ("x86_64", xtajit64_unix, xtajit64_pe),
]
components = provider.get("components")
if type(components) is not list or len(components) != len(expected_components):
    fail("CPU-provider component set is not exact")

binary_records = {}
for component, (guest, unix_relative, pe_relative) in zip(components, expected_components):
    if (
        type(component) is not dict
        or component.get("guestArchitecture") != guest
        or component.get("unixLibrary") != unix_relative
        or component.get("peLibrary") != pe_relative
    ):
        fail("CPU-provider component order or path is not canonical")
    for path_key, digest_key, executable in (
        ("unixLibrary", "unixLibrarySha256", True),
        ("peLibrary", "peLibrarySha256", False),
    ):
        relative = component[path_key]
        expected_digest = component.get(digest_key)
        if type(expected_digest) is not str or SHA256.fullmatch(expected_digest) is None:
            fail("CPU-provider component digest is malformed")
        record = digest_regular(relative, executable=executable)
        if record["digest"] != expected_digest:
            fail("CPU-provider component digest mismatch: " + relative)
        binary_records[relative] = record

try:
    expected_x64_abi = (
        "switchyard-xtajit64-provider-abi-v"
        + str(int(xtajit64_abi_version))
        + "-flight-bind-process-init-96-begin-472-doorbell"
    )
    x64_abi_bytes = xtajit64_abi_identity.encode("ascii")
except (UnicodeError, ValueError) as error:
    fail(f"invalid configured x64 provider ABI identity: {error}")
if xtajit64_abi_identity != expected_x64_abi or not (32 <= len(x64_abi_bytes) <= 128):
    fail("configured x64 provider ABI identity is inconsistent")
for relative in (xtajit64_unix, xtajit64_pe):
    record = binary_records[relative]
    try:
        with open(record["snapshot"], "rb") as stream:
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
                found = image.find(x64_abi_bytes) >= 0
    except OSError as error:
        fail(f"cannot inspect x64 provider ABI identity in {relative}: {error}")
    if not found:
        fail("x64 provider ABI identity is absent from " + relative)

if provider.get("library") != unicorn_library:
    fail("CPU-provider Unicorn library path is not canonical")
unicorn_expected = provider.get("librarySha256")
if type(unicorn_expected) is not str or SHA256.fullmatch(unicorn_expected) is None:
    fail("CPU-provider Unicorn library digest is malformed")
unicorn_record = digest_regular(unicorn_library, executable=True)
if unicorn_record["digest"] != unicorn_expected:
    fail("CPU-provider Unicorn library digest mismatch")
binary_records[unicorn_library] = unicorn_record

source_archive = provider.get("sourceArchive")
source_archive_sha = provider.get("sourceArchiveSha256")
if not safe_relative(source_archive) or not source_archive.startswith(unicorn_root + "/"):
    fail("CPU-provider source archive path is not canonical")
if type(source_archive_sha) is not str or SHA256.fullmatch(source_archive_sha) is None:
    fail("CPU-provider source archive digest is malformed")
archive_record = digest_regular(source_archive, MAX_ARCHIVE)
if archive_record["digest"] != source_archive_sha:
    fail("CPU-provider source archive digest mismatch")
verify_private_snapshot(archive_record, "Unicorn pristine source archive")

source_patch = provider.get("sourcePatch")
if type(source_patch) is not dict or set(source_patch) != {"path", "sha256"}:
    fail("CPU-provider source patch identity is absent or malformed")
if source_patch["path"] != unicorn_source_patch:
    fail("CPU-provider source patch path is not canonical")
if source_patch["sha256"] != unicorn_source_patch_sha256:
    fail("CPU-provider source patch digest is not the pinned qualification patch")
patch_record = digest_regular(unicorn_source_patch, MAX_TEXT)
if patch_record["digest"] != unicorn_source_patch_sha256:
    fail("CPU-provider source patch bytes do not match the pinned qualification patch")
verify_private_snapshot(patch_record, "Unicorn qualification source patch")
unicorn_source_patch_relative = os.path.relpath(unicorn_source_patch, unicorn_root).replace(
    os.sep, "/"
)

metadata_relative = provider.get("manifest")
if metadata_relative != unicorn_root + "/switchyard-unicorn-runtime.json":
    fail("CPU-provider package manifest path is not canonical")
metadata_path, _ = checked_path(metadata_relative, MAX_MANIFEST)
metadata = load_json(metadata_path, MAX_MANIFEST, "Unicorn runtime manifest")
expected_metadata = {
    "version": provider.get("version"),
    "sourceRepository": provider.get("sourceRepository"),
    "sourceRevision": provider.get("sourceRevision"),
    "buildContractVersion": provider.get("buildContractVersion"),
    "enabledArchitectures": ["x86"],
    "hostArchitecture": "arm64",
    "minimumMacOS": minimum_macos,
    "library": "lib/libunicorn.2.dylib",
    "librarySha256": unicorn_expected,
    "sourceArchive": "share/src/switchyard-unicorn/" + os.path.basename(source_archive),
    "sourceArchiveSha256": source_archive_sha,
    "sourcePatch": {
        "path": unicorn_source_patch_relative,
        "sha256": unicorn_source_patch_sha256,
    },
    "license": (
        "GPL-2.0-only with separately licensed GLib/QEMU components; preserve all "
        "included notices and corresponding source"
    ),
}
if type(metadata) is not dict or metadata != expected_metadata:
    fail("Unicorn runtime manifest does not match the closed package identity")

package_root = os.path.join(root, unicorn_root)
expected_entries = {
    ".switchyard-content-sha256",
    "lib/libunicorn.2.dylib",
    "lib/libunicorn.dylib",
    "share/doc/switchyard-unicorn/README.txt",
    "share/doc/switchyard-unicorn/CORRESPONDING-SOURCE.txt",
    "share/doc/switchyard-unicorn/COPYING",
    "share/doc/switchyard-unicorn/COPYING.LGPL2",
    "share/doc/switchyard-unicorn/COPYING_GLIB",
    "share/doc/switchyard-unicorn/QEMU-COPYING",
    "share/doc/switchyard-unicorn/QEMU-COPYING.LIB",
    "share/doc/switchyard-unicorn/QEMU-LICENSE",
    "share/src/switchyard-unicorn/" + os.path.basename(source_archive),
    unicorn_source_patch_relative,
    "switchyard-unicorn-runtime.json",
}
seen_entries = set()
try:
    package_root_info = os.lstat(package_root)
except OSError as error:
    fail(f"cannot inspect Unicorn package root: {error}")
if not stat.S_ISDIR(package_root_info.st_mode) or stat.S_ISLNK(package_root_info.st_mode):
    fail("Unicorn package root is not a real directory")
package_snapshot = os.path.join(snapshot_root, "unicorn-package")
os.mkdir(package_snapshot, 0o700)
os.chmod(package_snapshot, stat.S_IMODE(package_root_info.st_mode))


def package_file_limit(relative):
    if relative == ".switchyard-content-sha256":
        return 65
    if relative == "lib/libunicorn.2.dylib":
        return MAX_BINARY
    if relative == unicorn_source_patch_relative:
        return MAX_TEXT
    if relative.startswith("share/src/switchyard-unicorn/"):
        return MAX_ARCHIVE
    if relative == "switchyard-unicorn-runtime.json":
        return MAX_MANIFEST
    return MAX_TEXT


def copy_package_file(source, source_info, destination, relative):
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    source_fd = destination_fd = -1
    maximum = package_file_limit(relative)
    if source_info.st_size <= 0 or source_info.st_size > maximum:
        fail("Unicorn package artifact exceeds its bound: " + relative)
    try:
        source_fd = os.open(source, flags)
        opened = os.fstat(source_fd)
        if file_identity(opened) != file_identity(source_info):
            fail("Unicorn package artifact changed while opening: " + relative)
        destination_fd = os.open(
            destination,
            os.O_WRONLY
            | os.O_CREAT
            | os.O_EXCL
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_NOFOLLOW", 0),
            0o600,
        )
        for block in iter(lambda: os.read(source_fd, 1024 * 1024), b""):
            write_all(destination_fd, block)
        after = os.fstat(source_fd)
        if file_identity(after) != file_identity(opened):
            fail("Unicorn package artifact changed while snapshotting: " + relative)
        current = os.lstat(source)
        if file_identity(current) != file_identity(opened):
            fail("Unicorn package artifact path changed while snapshotting: " + relative)
        os.fchmod(destination_fd, stat.S_IMODE(opened.st_mode))
        copied = os.fstat(destination_fd)
        if (
            not stat.S_ISREG(copied.st_mode)
            or copied.st_uid != os.geteuid()
            or copied.st_size != opened.st_size
            or stat.S_IMODE(copied.st_mode) != stat.S_IMODE(opened.st_mode)
        ):
            fail("private Unicorn package snapshot is unsafe: " + relative)
    except OSError as error:
        fail(f"cannot snapshot Unicorn package artifact {relative}: {error}")
    finally:
        if destination_fd >= 0:
            os.close(destination_fd)
        if source_fd >= 0:
            os.close(source_fd)


for directory, directories, files in os.walk(package_root, topdown=True, followlinks=False):
    directories.sort()
    files.sort()
    directory_info = os.lstat(directory)
    if not stat.S_ISDIR(directory_info.st_mode) or stat.S_ISLNK(directory_info.st_mode):
        fail("Unicorn package directory changed during traversal")
    directory_relative = os.path.relpath(directory, package_root)
    snapshot_directory = (
        package_snapshot
        if directory_relative == "."
        else os.path.join(package_snapshot, directory_relative)
    )
    for name in directories:
        path = os.path.join(directory, name)
        info = os.lstat(path)
        if stat.S_ISLNK(info.st_mode):
            fail("Unicorn package contains a linked directory")
        if not stat.S_ISDIR(info.st_mode):
            fail("Unicorn package contains an invalid directory entry")
        destination = os.path.join(snapshot_directory, name)
        os.mkdir(destination, 0o700)
        os.chmod(destination, stat.S_IMODE(info.st_mode))
    for name in files:
        path = os.path.join(directory, name)
        relative = os.path.relpath(path, package_root).replace(os.sep, "/")
        seen_entries.add(relative)
        info = os.lstat(path)
        destination = os.path.join(snapshot_directory, name)
        if stat.S_ISLNK(info.st_mode):
            target = os.readlink(path)
            if relative != "lib/libunicorn.dylib" or target != "libunicorn.2.dylib":
                fail("Unicorn package contains an unexpected symbolic link: " + relative)
            os.symlink(target, destination)
            os.lchmod(destination, stat.S_IMODE(info.st_mode))
            copied_link = os.lstat(destination)
            current_link = os.lstat(path)
            if (
                not stat.S_ISLNK(copied_link.st_mode)
                or stat.S_IMODE(copied_link.st_mode) != stat.S_IMODE(info.st_mode)
                or file_identity(current_link) != file_identity(info)
                or os.readlink(path) != target
            ):
                fail("Unicorn package link changed while snapshotting: " + relative)
        elif not stat.S_ISREG(info.st_mode):
            fail("Unicorn package contains an unsupported artifact: " + relative)
        else:
            copy_package_file(path, info, destination, relative)
    if file_identity(os.lstat(directory)) != file_identity(directory_info):
        fail("Unicorn package directory changed during snapshot traversal")
if seen_entries != expected_entries:
    missing = sorted(expected_entries - seen_entries)
    extra = sorted(seen_entries - expected_entries)
    fail(f"Unicorn package artifact set is not exact; missing={missing}, extra={extra}")

payload_digest = provider.get("runtimePayloadDigest")
if type(payload_digest) is not str or SHA256.fullmatch(payload_digest) is None:
    fail("CPU-provider runtime payload digest is malformed")
verify = subprocess.run(
    ["/usr/bin/python3", "-I", digest_helper, "verify", package_snapshot],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    check=False,
)
if verify.returncode:
    fail("Unicorn package content marker is invalid")
digest = subprocess.run(
    ["/usr/bin/python3", "-I", digest_helper, "digest", package_snapshot],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    check=False,
)
if digest.returncode or digest.stdout.strip() != payload_digest:
    fail("Unicorn package content digest does not match the runtime manifest")


def pe_rva_to_offset(data, pe_offset, rva, size):
    section_count = struct.unpack_from("<H", data, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    section_table = pe_offset + 24 + optional_size
    if section_count > 96 or section_table > len(data) - section_count * 40:
        return None
    for index in range(section_count):
        section = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, section + 8
        )
        extent = max(virtual_size, raw_size)
        delta = rva - virtual_address
        if (
            delta >= 0
            and size <= extent
            and delta <= extent - size
            and size <= raw_size
            and delta <= raw_size - size
        ):
            offset = raw_offset + delta
            if offset <= len(data) - size:
                return offset
    return None


def has_arm64ec_metadata(data, pe_offset):
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    optional = pe_offset + 24
    if optional_size < 200 or optional > len(data) - optional_size:
        return False
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        return False
    if struct.unpack_from("<I", data, optional + 108)[0] < 11:
        return False
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    load_config_rva, load_config_size = struct.unpack_from(
        "<II", data, optional + 112 + 10 * 8
    )
    if load_config_size < 0xD0:
        return False
    load_config = pe_rva_to_offset(data, pe_offset, load_config_rva, 0xD0)
    if load_config is None or struct.unpack_from("<I", data, load_config)[0] < 0xD0:
        return False
    metadata_address = struct.unpack_from("<Q", data, load_config + 0xC8)[0]
    if metadata_address < image_base:
        return False
    metadata = pe_rva_to_offset(data, pe_offset, metadata_address - image_base, 12)
    if metadata is None:
        return False
    version, code_map_rva, code_map_count = struct.unpack_from("<III", data, metadata)
    if version not in (1, 2) or not code_map_count or code_map_count > 1024 * 1024:
        return False
    code_map = pe_rva_to_offset(data, pe_offset, code_map_rva, code_map_count * 8)
    if code_map is None:
        return False
    return any(
        struct.unpack_from("<I", data, code_map + index * 8)[0] & 1
        and struct.unpack_from("<I", data, code_map + index * 8 + 4)[0]
        for index in range(code_map_count)
    )


def pe_identity(relative):
    record = binary_records[relative]
    path = record["snapshot"]
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        fail(f"cannot open provider PE image {relative}: {error}")
    with os.fdopen(descriptor, "rb") as stream:
        opened = os.fstat(stream.fileno())
        if file_identity(opened) != record["snapshot_identity"]:
            fail("private provider PE snapshot changed while opening: " + relative)
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            if len(data) < 0x40 or data[:2] != b"MZ":
                fail("provider PE image has no DOS header: " + relative)
            pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
            if (
                pe_offset < 0x40
                or pe_offset > 1024 * 1024
                or pe_offset > len(data) - 24
                or data[pe_offset : pe_offset + 4] != b"PE\0\0"
            ):
                fail("provider PE image has no bounded PE header: " + relative)
            characteristics = struct.unpack_from("<H", data, pe_offset + 22)[0]
            if not characteristics & 0x2000:
                fail("provider PE image is not marked as a DLL: " + relative)
            machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
            result = machine, machine in (0x8664, 0xAA64) and has_arm64ec_metadata(data, pe_offset)
    verify_private_snapshot(record, relative)
    return result


machine, hybrid = pe_identity(xtajit_pe)
if machine != 0xAA64 or hybrid:
    fail("i386 guest provider is not a plain ARM64 PE DLL")
machine, hybrid = pe_identity(xtajit64_pe)
if machine != 0xA641 and not (machine == 0x8664 and hybrid):
    fail("x86_64 guest provider is not an ARM64EC PE DLL")


def command_output(arguments, description):
    result = subprocess.run(
        arguments,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode:
        fail(description + " command failed")
    return result.stdout


def version_tuple(text):
    if re.fullmatch(r"[0-9]+(?:[.][0-9]+){0,2}", text or "") is None:
        fail("malformed Mach-O version: " + repr(text))
    pieces = [int(piece) for piece in text.split(".")]
    return tuple((pieces + [0, 0])[:3])


def validate_macho(relative, install_name, provider_required_imports):
    record = binary_records[relative]
    path = record["snapshot"]
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        fail(f"cannot open provider Mach-O {relative}: {error}")
    try:
        opened = os.fstat(descriptor)
        if file_identity(opened) != record["snapshot_identity"]:
            fail("private provider Mach-O snapshot changed while opening: " + relative)
        header = os.read(descriptor, 16)
        after_header = os.fstat(descriptor)
        if file_identity(opened) != file_identity(after_header):
            fail("provider Mach-O changed while reading: " + relative)
    finally:
        os.close(descriptor)
    if (
        len(header) != 16
        or struct.unpack_from("<I", header)[0] != 0xFEEDFACF
        or struct.unpack_from("<I", header, 4)[0] != 0x0100000C
        or struct.unpack_from("<I", header, 12)[0] != 6
    ):
        fail("provider Unix artifact is not a thin arm64 Mach-O dylib: " + relative)
    if command_output([lipo_tool, "-archs", path], "lipo").strip() != "arm64":
        fail("provider Unix artifact has a non-arm64 slice: " + relative)
    build = command_output(
        [vtool_tool, "-arch", "arm64", "-show-build", path], "vtool"
    )
    if len(re.findall(r"^\s*cmd LC_BUILD_VERSION\s*$", build, re.MULTILINE)) != 1:
        fail("provider Mach-O does not have one LC_BUILD_VERSION: " + relative)
    if re.search(r"^\s*cmd LC_VERSION_MIN_MACOSX\s*$", build, re.MULTILINE):
        fail("provider Mach-O retains legacy deployment metadata: " + relative)
    fields = {}
    for line in build.splitlines():
        pieces = line.split()
        if len(pieces) == 2 and pieces[0] in ("platform", "minos", "sdk"):
            if pieces[0] in fields:
                fail("provider Mach-O repeats build metadata: " + relative)
            fields[pieces[0]] = pieces[1]
    if (
        fields.get("platform") != "MACOS"
        or version_tuple(fields.get("minos")) != version_tuple(minimum_macos)
        or version_tuple(fields.get("sdk")) != version_tuple(minimum_macos)
    ):
        fail("provider Mach-O build target is not the exact native profile: " + relative)
    identifiers = command_output([otool_tool, "-D", path], "otool -D").splitlines()
    if not identifiers or identifiers[-1].strip() != install_name:
        fail("provider Mach-O install name is not canonical: " + relative)
    dependencies = [
        line.split()[0]
        for line in command_output([otool_tool, "-L", path], "otool -L").splitlines()[1:]
        if line.split()
    ]
    rpaths = []
    lines = command_output([otool_tool, "-l", path], "otool -l").splitlines()
    for index, line in enumerate(lines):
        if line.split() == ["cmd", "LC_RPATH"]:
            if index + 2 >= len(lines) or not lines[index + 2].strip().startswith("path "):
                fail("provider Mach-O has malformed LC_RPATH metadata: " + relative)
            rpaths.append(lines[index + 2].split()[1])
    if provider_required_imports is not None:
        allowed = {install_name, "@rpath/libunicorn.2.dylib", "@rpath/ntdll.so"}
        for dependency in dependencies:
            if dependency in allowed or dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            fail("provider Unix library has an unexpected dependency: " + dependency)
        if dependencies.count("@rpath/libunicorn.2.dylib") != 1:
            fail("provider Unix library does not bind exactly one Unicorn dylib")
        if dependencies.count("@rpath/ntdll.so") != 1:
            fail("provider Unix library does not bind exactly one native ntdll")
        if sorted(rpaths) != sorted(["@loader_path/", unicorn_rpath]):
            fail("provider Unix library does not have the exact runtime rpaths")
        undefined = {
            line.strip()
            for line in command_output([nm_tool, "-ju", path], "nm -ju").splitlines()
            if line.strip()
        }
        missing = sorted(provider_required_imports - undefined)
        if missing:
            fail(
                "provider Unix library does not import the required Switchyard "
                "Unicorn API: " + ", ".join(missing)
            )
    else:
        for dependency in dependencies:
            if dependency == install_name or dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            fail("Unicorn dylib has an unexpected dependency: " + dependency)
        if rpaths:
            fail("Unicorn dylib unexpectedly contains an LC_RPATH")
        exports = {
            line.strip()
            for line in command_output([nm_tool, "-gjU", path], "nm -gjU").splitlines()
            if line.strip()
        }
        required = {
            "_uc_emu_stop_at_instruction_boundary",
            "_uc_clear_instruction_boundary_stop",
            "_uc_enable_shared_memory_atomics",
            "_uc_set_shared_memory_atomic_callback",
        }
        missing = sorted(required - exports)
        if missing:
            fail(
                "Unicorn dylib does not export the required Switchyard API: "
                + ", ".join(missing)
            )
    verify_private_snapshot(record, relative)


common_provider_imports = {
    "_uc_emu_stop_at_instruction_boundary",
    "_uc_enable_shared_memory_atomics",
}
validate_macho(xtajit_unix, "@rpath/xtajit.so", common_provider_imports)
validate_macho(
    xtajit64_unix,
    "@rpath/xtajit64.so",
    common_provider_imports | {
        "_uc_clear_instruction_boundary_stop",
        "_uc_set_shared_memory_atomic_callback",
    },
)
validate_macho(unicorn_library, "@rpath/libunicorn.2.dylib", None)

allowed_provider_paths = {
    xtajit_unix.casefold(),
    xtajit_pe.casefold(),
    xtajit64_unix.casefold(),
    xtajit64_pe.casefold(),
    unicorn_library.casefold(),
    (unicorn_root + "/lib/libunicorn.dylib").casefold(),
}
reserved_names = {
    "xtajit.so",
    "xtajit64.so",
    "xtajit.dll",
    "xtajit64.dll",
    "libunicorn.2.dylib",
    "libunicorn.dylib",
}
for directory, directories, files in os.walk(root, followlinks=False):
    for name in directories:
        path = os.path.join(directory, name)
        if stat.S_ISLNK(os.lstat(path).st_mode) and name.casefold() in {
            "aarch64-unix",
            "x86_64-unix",
            "aarch64-windows",
        }:
            fail("provider ABI directory is a symbolic link")
    for name in files:
        if name.casefold() not in reserved_names:
            continue
        relative = os.path.relpath(os.path.join(directory, name), root).replace(os.sep, "/")
        if relative.casefold() not in allowed_provider_paths:
            fail("unexpected or Rosetta provider artifact: " + relative)
PY
}

switchyard_validate_wow64_unixlib_policy_manifest() {
  local runtime_root manifest source_root codesign_tool lipo_tool nm_tool

  [ "$#" -eq 3 ] || {
    echo "usage: switchyard_validate_wow64_unixlib_policy_manifest RUNTIME MANIFEST ROOT_DIR" >&2
    return 2
  }
  runtime_root="$1"
  manifest="$2"
  source_root="$3"
  switchyard_native_cpu_provider_load_profile_contract || return 1
  switchyard_native_cpu_provider_validate_runtime_profile \
    "$manifest" "$runtime_root" || return 1
  codesign_tool="$(switchyard_native_cpu_provider_inspection_tool codesign)" || return 1
  lipo_tool="$(switchyard_native_cpu_provider_inspection_tool lipo)" || return 1
  nm_tool="$(switchyard_native_cpu_provider_inspection_tool nm)" || return 1
  switchyard_native_cpu_provider_validate_executable_path \
    "$codesign_tool" "WoW64 policy codesign tool" || return 1
  switchyard_native_cpu_provider_validate_executable_path \
    "$lipo_tool" "WoW64 policy lipo tool" || return 1
  switchyard_native_cpu_provider_validate_executable_path \
    "$nm_tool" "WoW64 policy nm tool" || return 1

  /usr/bin/python3 -I - "$runtime_root" "$manifest" \
    "$codesign_tool" "$lipo_tool" "$nm_tool" \
    "$SWITCHYARD_WOW64_UNIXLIB_POLICY_CONTRACT_VERSION" \
    "$SWITCHYARD_WOW64_UNIXLIB_POLICY_HANDLE_ENCODING" \
    "$SWITCHYARD_WOW64_UNIXLIB_POLICY_EXTERNAL_SOURCE_VERSION" \
    "$SWITCHYARD_WOW64_UNIXLIB_POLICY_REQUIRED_ENTRY_FLAG" <<'PY' || return 1
import hashlib
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile

(
    root_name,
    manifest_name,
    codesign_tool,
    lipo_tool,
    nm_tool,
    contract,
    encoding,
    external_version,
    required_flag,
) = sys.argv[1:]
expected_modules = [
    ("crypt32", "lib/wine/aarch64-unix/crypt32.so"),
    ("dwrite", "lib/wine/aarch64-unix/dwrite.so"),
    ("secur32", "lib/wine/aarch64-unix/secur32.so"),
    ("winemac", "lib/wine/aarch64-unix/winemac.so"),
    ("ws2_32", "lib/wine/aarch64-unix/ws2_32.so"),
]
companion_relative = "lib/wine/aarch64-unix/winemetal-wow64.so"
MAX_UNIX_IMAGES = 4096
MAX_UNIX_SNAPSHOT_BYTES = 4 * 1024 * 1024 * 1024
sha256_pattern = re.compile(r"[0-9a-f]{64}")


def fail(message):
    raise SystemExit("WoW64 Unixlib policy validation failed: " + message)


def file_identity(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def no_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            fail("duplicate JSON object key: " + key)
        value[key] = item
    return value


def reject_constant(value):
    fail("non-standard JSON constant: " + value)


if (
    not os.path.isabs(root_name)
    or os.path.normpath(root_name) != root_name
    or root_name == os.path.sep
    or "\n" in root_name
    or "\r" in root_name
):
    fail("runtime root must be one bounded canonical absolute path")
root = root_name
try:
    root_info = os.lstat(root)
except OSError as error:
    fail(f"cannot inspect runtime root: {error}")
if not stat.S_ISDIR(root_info.st_mode) or stat.S_ISLNK(root_info.st_mode) or os.path.realpath(root) != root:
    fail("runtime root is not a real canonical directory")
manifest = os.path.abspath(manifest_name)
if manifest != os.path.join(root, "switchyard-runtime.json"):
    fail("manifest is not the runtime root manifest")
try:
    manifest_info = os.lstat(manifest)
    if (
        not stat.S_ISREG(manifest_info.st_mode)
        or stat.S_ISLNK(manifest_info.st_mode)
        or manifest_info.st_size <= 0
        or manifest_info.st_size > 1024 * 1024
    ):
        fail("runtime manifest is missing, unsafe, or too large")
    manifest_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    manifest_descriptor = os.open(manifest, manifest_flags)
    manifest_opened = os.fstat(manifest_descriptor)
    if file_identity(manifest_info) != file_identity(manifest_opened):
        fail("runtime manifest changed while opening")
    stream = os.fdopen(manifest_descriptor, "r", encoding="utf-8")
    manifest_descriptor = -1
    with stream:
        value = json.load(
            stream,
            object_pairs_hook=no_duplicates,
            parse_constant=reject_constant,
        )
        manifest_after = os.fstat(stream.fileno())
except (OSError, UnicodeError, json.JSONDecodeError) as error:
    fail(f"cannot parse runtime manifest: {error}")
finally:
    if "manifest_descriptor" in locals() and manifest_descriptor >= 0:
        os.close(manifest_descriptor)
try:
    manifest_path_after = os.lstat(manifest)
except OSError as error:
    fail(f"cannot reinspect runtime manifest: {error}")
if (
    file_identity(manifest_opened) != file_identity(manifest_after)
    or file_identity(manifest_after) != file_identity(manifest_path_after)
):
    fail("runtime manifest changed while parsing")

policy = value.get("wow64UnixlibPolicy") if type(value) is dict else None
dxmt = value.get("dxmt") if type(value) is dict else None
expected_keys = {
    "contractVersion",
    "handleEncoding",
    "internalDispatch",
    "externalSourceVersion",
    "requiredEntryFlag",
    "auditedModules",
}
if type(policy) is not dict or set(policy) != expected_keys:
    fail("manifest policy is absent or has an unexpected field set")
if type(policy["contractVersion"]) is not int or policy["contractVersion"] != int(contract):
    fail("manifest policy contractVersion is not current")
if policy["handleEncoding"] != encoding or type(policy["handleEncoding"]) is not str:
    fail("manifest policy handle encoding is not generation-tagged-v1")
if policy["internalDispatch"] != {"module": "ntdll", "sourceVersion": 1}:
    fail("manifest policy does not reserve source v1 for internal ntdll")
if (
    type(policy["externalSourceVersion"]) is not int
    or policy["externalSourceVersion"] != int(external_version)
):
    fail("manifest policy does not require source v2 externally")
if policy["requiredEntryFlag"] != required_flag or type(policy["requiredEntryFlag"]) is not str:
    fail("manifest policy does not require REVIEWED entry metadata")
companion = dxmt.get("wow64Companion") if type(dxmt) is dict else None
if type(companion) is not dict or companion.get("path") != companion_relative:
    fail("manifest does not bind the exact DXMT WoW64 companion")

snapshot_workspace = tempfile.TemporaryDirectory(
    prefix="switchyard-wow64-policy.", dir="/private/tmp"
)
snapshot_root = snapshot_workspace.name
snapshot_root_info = os.lstat(snapshot_root)
if (
    not stat.S_ISDIR(snapshot_root_info.st_mode)
    or stat.S_ISLNK(snapshot_root_info.st_mode)
    or snapshot_root_info.st_uid != os.geteuid()
    or stat.S_IMODE(snapshot_root_info.st_mode) != 0o700
):
    fail("private WoW64 policy validation workspace is unsafe")
snapshot_sequence = 0


def write_all(descriptor, data):
    offset = 0
    while offset < len(data):
        written = os.write(descriptor, data[offset:])
        if written <= 0:
            fail("cannot make progress writing a private audited-module snapshot")
        offset += written


def snapshot_runtime_file(path, path_info, description, maximum, executable=False):
    global snapshot_sequence

    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    source_fd = destination_fd = -1
    snapshot_sequence += 1
    snapshot_path = os.path.join(
        snapshot_root, f"{snapshot_sequence:04d}-{os.path.basename(path)}"
    )
    digest = hashlib.sha256()
    header = b""
    try:
        source_fd = os.open(path, flags)
        opened = os.fstat(source_fd)
        if (
            file_identity(opened) != file_identity(path_info)
            or not stat.S_ISREG(opened.st_mode)
            or opened.st_size <= 0
            or opened.st_size > maximum
            or opened.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            or executable and not opened.st_mode & 0o111
        ):
            fail(description + " changed while opening")
        destination_fd = os.open(
            snapshot_path,
            os.O_WRONLY
            | os.O_CREAT
            | os.O_EXCL
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_NOFOLLOW", 0),
            0o400,
        )
        for block in iter(lambda: os.read(source_fd, 1024 * 1024), b""):
            if len(header) < 16:
                header += block[: 16 - len(header)]
            digest.update(block)
            write_all(destination_fd, block)
        after = os.fstat(source_fd)
        if file_identity(after) != file_identity(opened):
            fail(description + " changed while snapshotting")
        current = os.lstat(path)
        if file_identity(current) != file_identity(opened):
            fail(description + " path changed while snapshotting")
        snapshot_mode = stat.S_IMODE(opened.st_mode) & ~0o222
        os.fchmod(destination_fd, snapshot_mode)
        os.fsync(destination_fd)
        snapshot = os.fstat(destination_fd)
        if (
            not stat.S_ISREG(snapshot.st_mode)
            or snapshot.st_uid != os.geteuid()
            or stat.S_IMODE(snapshot.st_mode) != snapshot_mode
            or snapshot.st_size != opened.st_size
            or snapshot.st_nlink != 1
        ):
            fail("private snapshot is unsafe for " + description)
    except OSError as error:
        fail(f"cannot create private snapshot for {description}: {error}")
    finally:
        if destination_fd >= 0:
            os.close(destination_fd)
        if source_fd >= 0:
            os.close(source_fd)
    return {
        "live_path": path,
        "live_identity": file_identity(after),
        "snapshot": snapshot_path,
        "snapshot_identity": file_identity(snapshot),
        "digest": digest.hexdigest(),
        "header": header,
    }


def verify_private_snapshot(record, description):
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = -1
    digest = hashlib.sha256()
    try:
        descriptor = os.open(record["snapshot"], flags)
        opened = os.fstat(descriptor)
        if file_identity(opened) != record["snapshot_identity"]:
            fail("private snapshot changed while opening for " + description)
        for block in iter(lambda: os.read(descriptor, 1024 * 1024), b""):
            digest.update(block)
        after = os.fstat(descriptor)
    except OSError as error:
        fail(f"cannot verify private snapshot for {description}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    if file_identity(after) != record["snapshot_identity"] or digest.hexdigest() != record["digest"]:
        fail("private snapshot changed during inspection for " + description)
    live_descriptor = -1
    live_digest = hashlib.sha256()
    try:
        current = os.lstat(record["live_path"])
        live_descriptor = os.open(record["live_path"], flags)
        live_opened = os.fstat(live_descriptor)
        if (
            file_identity(current)[:4] != record["live_identity"][:4]
            or file_identity(live_opened)[:4] != record["live_identity"][:4]
        ):
            fail(description + " path changed during validation")
        for block in iter(lambda: os.read(live_descriptor, 1024 * 1024), b""):
            live_digest.update(block)
        live_after = os.fstat(live_descriptor)
        live_path_after = os.lstat(record["live_path"])
    except OSError as error:
        fail(f"cannot rehash {description}: {error}")
    finally:
        if live_descriptor >= 0:
            os.close(live_descriptor)
    if (
        file_identity(live_after) != file_identity(live_opened)
        or file_identity(live_path_after)[:4] != record["live_identity"][:4]
        or live_digest.hexdigest() != record["digest"]
    ):
        fail(description + " bytes changed during validation")
    record["live_identity"] = file_identity(live_after)

modules = policy["auditedModules"]
if type(modules) is not list or len(modules) != len(expected_modules):
    fail("manifest audited-module list is not exact")
audited_records = {}
for item, (expected_module, expected_relative) in zip(modules, expected_modules):
    if type(item) is not dict or set(item) != {"module", "unixLibrary", "sha256"}:
        fail("manifest audited module has an unexpected field set")
    if item["module"] != expected_module or item["unixLibrary"] != expected_relative:
        fail("manifest audited modules are not the exact sorted allowlist")
    expected_digest = item["sha256"]
    if type(expected_digest) is not str or sha256_pattern.fullmatch(expected_digest) is None:
        fail("manifest audited-module digest is malformed")
    current = root
    for index, part in enumerate(expected_relative.split("/")):
        current = os.path.join(current, part)
        try:
            info = os.lstat(current)
        except OSError as error:
            fail(f"missing audited module {expected_relative}: {error}")
        if stat.S_ISLNK(info.st_mode):
            fail("audited-module path contains a symbolic link: " + expected_relative)
        if index != len(expected_relative.split("/")) - 1 and not stat.S_ISDIR(info.st_mode):
            fail("audited-module path component is not a directory: " + expected_relative)
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_size <= 0
        or info.st_size > 512 * 1024 * 1024
        or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        or not info.st_mode & 0o111
    ):
        fail("audited module has an unsafe type, mode, or size: " + expected_relative)
    record = snapshot_runtime_file(
        current,
        info,
        "audited module " + expected_relative,
        512 * 1024 * 1024,
        executable=True,
    )
    if record["digest"] != expected_digest:
        fail("audited-module digest mismatch: " + expected_relative)
    audited_records[current] = record
    header = record["header"]
    if (
        len(header) != 16
        or struct.unpack_from("<I", header)[0] != 0xFEEDFACF
        or struct.unpack_from("<I", header, 4)[0] != 0x0100000C
        or struct.unpack_from("<I", header, 12)[0] not in (6, 8)
    ):
        fail("audited module is not a thin arm64 Mach-O image: " + expected_relative)
    lipo = subprocess.run(
        [lipo_tool, "-archs", record["snapshot"]],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if lipo.returncode or lipo.stdout.strip() != "arm64":
        fail("audited module contains a non-arm64 slice: " + expected_relative)
    signature = subprocess.run(
        [codesign_tool, "--verify", "--strict", "--verbose=2", record["snapshot"]],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if signature.returncode:
        fail("audited module does not have a valid strict code signature: " + expected_relative)
    symbols = subprocess.run(
        [nm_tool, "-gjU", record["snapshot"]],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if symbols.returncode:
        fail("cannot inspect audited-module exports: " + expected_relative)
    exported = set(symbols.stdout.splitlines())
    if "___wine_unix_call_wow64_dispatch_v2" not in exported:
        fail("audited module does not export a v2 dispatch source: " + expected_relative)
    if "___wine_unix_call_wow64_funcs" not in exported:
        fail("audited module does not export its WoW64 function table: " + expected_relative)
    if "___wine_unix_call_wow64_dispatch_v1" in exported:
        fail("audited external module exports the internal v1 descriptor")
    verify_private_snapshot(record, "audited module " + expected_relative)
unix_root = os.path.join(root, "lib/wine/aarch64-unix")
try:
    unix_root_info = os.lstat(unix_root)
except OSError as error:
    fail(f"cannot inspect native Unix library root: {error}")
if not stat.S_ISDIR(unix_root_info.st_mode) or stat.S_ISLNK(unix_root_info.st_mode):
    fail("native Unix library root is not a real directory")
unix_images = []
unix_snapshot_to_live = {}
unix_image_records = {}
unix_candidate_count = 0
unix_snapshot_bytes = 0
for entry in os.scandir(unix_root):
    info = entry.stat(follow_symlinks=False)
    if not entry.name.endswith(".so"):
        continue
    unix_candidate_count += 1
    unix_snapshot_bytes += info.st_size
    if unix_candidate_count > MAX_UNIX_IMAGES or unix_snapshot_bytes > MAX_UNIX_SNAPSHOT_BYTES:
        fail("native Unix library inventory exceeds its count or byte bound")
    if stat.S_ISLNK(info.st_mode):
        fail("native Unix library is a symbolic link: " + entry.name)
    if not stat.S_ISREG(info.st_mode) or info.st_size <= 0 or info.st_size > 512 * 1024 * 1024:
        fail("native Unix library has an unsafe type or size: " + entry.name)
    if entry.path in audited_records:
        record = audited_records[entry.path]
        if file_identity(info) != record["live_identity"]:
            fail("audited native Unix library changed before inventory: " + entry.name)
    else:
        record = snapshot_runtime_file(
            entry.path,
            info,
            "native Unix library " + entry.name,
            512 * 1024 * 1024,
        )
    header = record["header"][:8]
    if len(header) >= 8 and struct.unpack_from("<I", header)[0] == 0xFEEDFACF:
        unix_images.append(record["snapshot"])
        unix_snapshot_to_live[record["snapshot"]] = entry.path
        unix_image_records[entry.path] = record
v1_images = set()
v2_images = set()
for offset in range(0, len(unix_images), 64):
    result = subprocess.run(
        [nm_tool, "-A", "-gjU", *unix_images[offset : offset + 64]],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode:
        fail("cannot enumerate native Unix library dispatch exports")
    for line in result.stdout.splitlines():
        v1_suffix = ": ___wine_unix_call_wow64_dispatch_v1"
        v2_suffix = ": ___wine_unix_call_wow64_dispatch_v2"
        if line.endswith(v1_suffix):
            snapshot = line[: -len(v1_suffix)]
            if snapshot not in unix_snapshot_to_live:
                fail("nm reported an unknown private Unix library snapshot")
            v1_images.add(unix_snapshot_to_live[snapshot])
        if line.endswith(v2_suffix):
            snapshot = line[: -len(v2_suffix)]
            if snapshot not in unix_snapshot_to_live:
                fail("nm reported an unknown private Unix library snapshot")
            v2_images.add(unix_snapshot_to_live[snapshot])
for path, record in unix_image_records.items():
    verify_private_snapshot(record, "native Unix library " + os.path.basename(path))
if v1_images:
    paths = sorted(os.path.relpath(path, root) for path in v1_images)
    fail(f"external Unix library exports the ntdll-only v1 source: {paths}")
expected_v2_images = {os.path.join(root, relative) for _, relative in expected_modules}
expected_v2_images.add(os.path.join(root, companion_relative))
if v2_images != expected_v2_images:
    missing = sorted(os.path.relpath(path, root) for path in expected_v2_images - v2_images)
    extra = sorted(os.path.relpath(path, root) for path in v2_images - expected_v2_images)
    fail(f"external v2 dispatch module set is not exact; missing={missing}, extra={extra}")

expected_paths = {relative.casefold() for _, relative in expected_modules} | {companion_relative.casefold()}
reserved = {os.path.basename(relative).casefold() for _, relative in expected_modules}
reserved.add(os.path.basename(companion_relative).casefold())
for directory, directories, files in os.walk(root, followlinks=False):
    for name in directories:
        path = os.path.join(directory, name)
        if stat.S_ISLNK(os.lstat(path).st_mode) and name.casefold() in {
            "aarch64-unix",
            "x86_64-unix",
        }:
            fail("Unix ABI directory is a symbolic link")
    for name in files:
        if name.casefold() not in reserved:
            continue
        relative = os.path.relpath(os.path.join(directory, name), root).replace(os.sep, "/")
        if relative.casefold() not in expected_paths:
            fail("unexpected or Rosetta audited-module copy: " + relative)
PY

  /usr/bin/python3 -I - "$source_root" <<'PY'
import hashlib
import os
import re
import stat
import sys

root_name = sys.argv[1]


def fail(message):
    raise SystemExit("WoW64 Unixlib source-policy validation failed: " + message)


def file_identity(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


if (
    not os.path.isabs(root_name)
    or os.path.normpath(root_name) != root_name
    or root_name == os.path.sep
    or "\n" in root_name
    or "\r" in root_name
):
    fail("source root must be one bounded canonical absolute path")
root = root_name
try:
    info = os.lstat(root)
except OSError as error:
    fail(f"cannot inspect source root: {error}")
if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode) or os.path.realpath(root) != root:
    fail("source root is not a real canonical directory")


def read_source(relative, maximum=16 * 1024 * 1024):
    current = root
    for index, part in enumerate(relative.split("/")):
        current = os.path.join(current, part)
        try:
            item = os.lstat(current)
        except OSError as error:
            fail(f"missing policy source {relative}: {error}")
        if stat.S_ISLNK(item.st_mode):
            fail("policy source path contains a symbolic link: " + relative)
        if index != len(relative.split("/")) - 1 and not stat.S_ISDIR(item.st_mode):
            fail("policy source path component is not a directory: " + relative)
    if not stat.S_ISREG(item.st_mode) or item.st_size <= 0 or item.st_size > maximum:
        fail("policy source has an unsafe type or size: " + relative)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = -1
    try:
        descriptor = os.open(current, flags)
        opened = os.fstat(descriptor)
        if file_identity(item) != file_identity(opened):
            fail("policy source changed while opening: " + relative)
        stream = os.fdopen(descriptor, "r", encoding="utf-8")
        descriptor = -1
        with stream:
            value = stream.read()
            after = os.fstat(stream.fileno())
    except (OSError, UnicodeError) as error:
        fail(f"cannot read policy source {relative}: {error}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    try:
        path_after = os.lstat(current)
    except OSError as error:
        fail(f"cannot reinspect policy source {relative}: {error}")
    if file_identity(opened) != file_identity(after) or file_identity(after) != file_identity(path_after):
        fail("policy source changed while reading: " + relative)
    return value


header = read_source("include/wine/unixlib.h")
loader = read_source("dlls/ntdll/unix/loader.c")
virtual = read_source("dlls/ntdll/unix/virtual.c", 32 * 1024 * 1024)


def macro_number(name):
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|[0-9]+)(?:u|ul|ull|U|UL|ULL)*\s*$",
        header,
        re.MULTILINE,
    )
    if not match:
        fail("missing numeric dispatch constant: " + name)
    return int(match.group(1), 0)


tag = macro_number("WINE_UNIXLIB_DISPATCH_HANDLE_TAG")
version = macro_number("WINE_UNIXLIB_DISPATCH_VERSION")
max_slots = macro_number("WINE_UNIXLIB_DISPATCH_MAX_SLOTS")
slot_bits = macro_number("WINE_UNIXLIB_DISPATCH_SLOT_BITS")
slot_mask = macro_number("WINE_UNIXLIB_DISPATCH_SLOT_MASK")
generation_mask = macro_number("WINE_UNIXLIB_DISPATCH_GENERATION_MASK")
v2_version = macro_number("WINE_UNIXLIB_DISPATCH_SOURCE_V2_VERSION")
reviewed = macro_number("WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED")
valid_flags = macro_number("WINE_UNIXLIB_DISPATCH_ENTRY_VALID_FLAGS")
if (
    tag != 1 << 63
    or version != 1
    or slot_bits != 11
    or slot_mask != (1 << slot_bits) - 1
    or max_slots != 1025
    or max_slots > slot_mask
    or generation_mask != (1 << (63 - slot_bits)) - 1
    or v2_version != 2
    or reviewed != 1
    or not valid_flags & reviewed
):
    fail("generation-tagged dispatch constants are semantically inconsistent")
if not re.search(
    r"#define\s+WINE_UNIXLIB_DISPATCH_GENERATION_SHIFT\s+WINE_UNIXLIB_DISPATCH_SLOT_BITS",
    header,
):
    fail("dispatch generation shift is not bound to the slot width")
if not re.search(
    r"return\s+WINE_UNIXLIB_DISPATCH_HANDLE_TAG\s*\|\s*"
    r"\(generation\s*<<\s*WINE_UNIXLIB_DISPATCH_GENERATION_SHIFT\)\s*\|\s*"
    r"\(\(UINT64\)slot\s*\+\s*1\)",
    header,
    re.DOTALL,
):
    fail("dispatch handle encoder is not generation tagged")
for required in (
    "payload = handle & ~WINE_UNIXLIB_DISPATCH_HANDLE_TAG",
    "encoded_slot = payload & WINE_UNIXLIB_DISPATCH_SLOT_MASK",
    "encoded_generation = payload >> WINE_UNIXLIB_DISPATCH_GENERATION_SHIFT",
    "*slot = encoded_slot - 1",
    "*generation = encoded_generation",
):
    if required not in header:
        fail("dispatch handle decoder lost required semantics: " + required)

if not re.search(
    r"static\s+const\s+struct\s+wine_unixlib_dispatch_source_v1\s+"
    r"ntdll_wow64_dispatch_source\s*=",
    loader,
):
    fail("ntdll internal dispatch is not source v1")
if not re.search(
    r"register_wow64_unixlib_dispatch\s*\(\s*&ntdll_wow64_dispatch_source\s*,\s*"
    r"unix_call_wow64_funcs",
    loader,
    re.DOTALL,
):
    fail("ntdll internal v1 dispatch is not registered through the private gate")
if loader.count("!(flags & WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED)") < 1:
    fail("v2 registration no longer requires REVIEWED metadata")
if loader.count("!(metadata->flags & WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED)") < 1:
    fail("v2 execution no longer rechecks REVIEWED metadata")
if "__wine_unix_call_wow64_dispatch_v2" not in virtual or \
        "register_wow64_unixlib_dispatch_v2" not in virtual:
    fail("external Unixlib loading does not require a v2 source")
if "__wine_unix_call_wow64_dispatch_v1" in virtual:
    fail("external Unixlib loading retains a v1 fallback")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def initializer(source, declaration_pattern):
    match = re.search(declaration_pattern, source)
    if not match:
        fail("audited module is missing an immutable dispatch array")
    opening = source.find("{", match.end())
    if opening < 0:
        fail("audited dispatch array has no initializer")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if not depth:
                return source[opening + 1 : index]
    fail("audited dispatch array initializer is unterminated")


def split_entries(body):
    entries = []
    start = 0
    parentheses = braces = 0
    for index, character in enumerate(body):
        if character == "(":
            parentheses += 1
        elif character == ")":
            parentheses -= 1
        elif character == "{":
            braces += 1
        elif character == "}":
            braces -= 1
        elif character == "," and not parentheses and not braces:
            item = body[start:index].strip()
            if item:
                entries.append(item)
            start = index + 1
        if parentheses < 0 or braces < 0:
            fail("audited dispatch initializer is structurally invalid")
    tail = body[start:].strip()
    if tail:
        entries.append(tail)
    if parentheses or braces:
        fail("audited dispatch initializer is unbalanced")
    return entries


source_files = {
    "crypt32": (
        "dlls/crypt32/unixlib.c",
        8,
        "7c2dab33bc8166286af5d8d48388c68eb83a095176ed25025e0740fd828fab3d",
    ),
    "dwrite": ("dlls/dwrite/freetype.c", None, None),
    "secur32": (
        "dlls/secur32/schannel_gnutls.c",
        22,
        "8806af8a5217a580c2c980ac58f0561287f88e9807b9457d007dcf42cccedde2",
    ),
    "winemac": ("dlls/winemac.drv/macdrv_main.c", None, None),
    "ws2_32": ("dlls/ws2_32/unixlib.c", None, None),
    "winemetal-wow64": ("dlls/winemetal-wow64/unixlib.c", 138, None),
}
for module, (relative, expected_count, expected_digest) in source_files.items():
    raw_source = read_source(relative, 32 * 1024 * 1024)
    if (
        expected_digest is not None
        and hashlib.sha256(raw_source.encode("utf-8")).hexdigest() != expected_digest
    ):
        fail(module + " source differs from its frozen v2-reviewed identity")
    source = strip_comments(raw_source)
    if source.count("WINE_UNIXLIB_DISPATCH_SOURCE_V2(") != 1:
        fail(module + " does not publish exactly one v2 dispatch source")
    if "WINE_UNIXLIB_DISPATCH_SOURCE_V1(" in source:
        fail(module + " publishes a forbidden external v1 dispatch source")
    function_body = initializer(
        source,
        r"const\s+unixlib_entry_t\s+__wine_unix_call_wow64_funcs\s*\[\s*\]\s*=",
    )
    metadata_match = re.search(
        r"static\s+const\s+struct\s+wine_unixlib_dispatch_entry_v2\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]\s*=",
        source,
    )
    if not metadata_match:
        fail(module + " has no immutable v2 metadata array")
    metadata_name = metadata_match.group(1)
    metadata_body = initializer(source, metadata_match.re.pattern)
    functions = split_entries(function_body)
    entries = split_entries(metadata_body)
    if not functions or len(functions) != len(entries):
        fail(module + " function and metadata arrays have different bounds")
    if expected_count is not None and len(functions) != expected_count:
        fail(module + " function and metadata arrays do not have the frozen bound")
    for entry in entries:
        normalized = re.sub(r"\s+", "", entry)
        if normalized.startswith("WINE_UNIXLIB_DISPATCH_ARGS_V2(") and normalized.endswith(")"):
            continue
        if normalized == "{0,WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED}":
            continue
        fail(module + " contains an entry without explicit REVIEWED v2 metadata")
    publication = re.search(
        r"WINE_UNIXLIB_DISPATCH_SOURCE_V2\s*\(\s*"
        r"__wine_unix_call_wow64_funcs\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
        source,
    )
    if not publication or publication.group(1) != metadata_name:
        fail(module + " does not bind its exact metadata array to the function table")
PY
}
