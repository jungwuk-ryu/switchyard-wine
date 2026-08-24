#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT_DIR/switchyard/lib/runtime_profile.sh"
# Native release orchestration deliberately consumes the producer-owned
# manifest and validators instead of duplicating their signed identity schema.
source "$ROOT_DIR/switchyard/lib/native_arm64_packaging.sh"
source "$ROOT_DIR/switchyard/lib/native_cpu_provider.sh"
source "$ROOT_DIR/switchyard/lib/dxmt_artifact.sh"
source "$ROOT_DIR/switchyard/lib/macho_signing.sh"
source "$ROOT_DIR/switchyard/lib/runtime_prefix.sh"
ENTITLEMENTS=""
EXPECTED_TEAM_ID="${SWITCHYARD_DEVELOPER_TEAM_ID:-M3CULMDKU3}"
RUNTIME=""
OUTPUT_DIR=""
IDENTITY="${SWITCHYARD_CODESIGN_IDENTITY:-}"
NOTARY_PROFILE="${SWITCHYARD_NOTARY_PROFILE:-}"
EXPECTED_RUNTIME_DIGEST="${SWITCHYARD_RUNTIME_CONTENT_SHA256:-}"
manifest_snapshot=""
mach_o_list=""
verification_log=""
prefix=""
signed_runtime=""
native_release_smoke_runtime=""
runtime_profile=""
entitlements_snapshot_fd=""
native_release_private_root=""
native_release_complete=0
native_archive_public_path=""
native_archive_public_identity=""
native_checksum_public_path=""
native_checksum_public_identity=""
native_manifest_public_path=""
native_manifest_public_identity=""
CODESIGN_TOOL="/usr/bin/codesign"
NATIVE_SMOKE_TOOL="$ROOT_DIR/switchyard/tests/native_no_rosetta_process_test.sh"
NATIVE_RELEASE_WINESERVER_CONTROL_ATTEMPTS=600
NATIVE_RELEASE_WINESERVER_TERM_ATTEMPTS=50
NATIVE_RELEASE_WINESERVER_KILL_ATTEMPTS=20

remove_owned_native_release_output() {
  local path="$1"
  local expected_identity="$2"

  [ -n "$path" ] && [ -n "$expected_identity" ] || return 0
  /usr/bin/python3 -I - "$path" "$expected_identity" <<'PY'
import os
import stat
import sys

path, expected = sys.argv[1:]
try:
    expected_device, expected_inode = (int(item) for item in expected.split(":"))
except (TypeError, ValueError):
    raise SystemExit(1)
try:
    current = os.lstat(path)
except FileNotFoundError:
    raise SystemExit(0)
if (
    not stat.S_ISREG(current.st_mode)
    or (current.st_dev, current.st_ino) != (expected_device, expected_inode)
):
    raise SystemExit(1)
os.unlink(path)
PY
}

remove_native_release_private_root() {
  [ -n "$native_release_private_root" ] || return 0
  case "$native_release_private_root" in
    "$OUTPUT_DIR"/.switchyard-native-release.??????)
      [ -d "$native_release_private_root" ] &&
        [ ! -L "$native_release_private_root" ] || return 1
      /bin/rm -rf -- "$native_release_private_root"
      native_release_private_root=""
      signed_runtime=""
      native_release_smoke_runtime=""
      ;;
    *)
      echo "refusing to clean unexpected native release staging root: $native_release_private_root" >&2
      return 1
      ;;
  esac
}

cleanup() {
  if [ "${runtime_profile:-}" = preview-native-arm64-fex ]; then
    if [ -n "$entitlements_snapshot_fd" ]; then
      close_validated_entitlements_snapshot "$entitlements_snapshot_fd" >/dev/null 2>&1 ||
        exec 19<&-
      entitlements_snapshot_fd=""
    fi
    if [ -n "$prefix" ]; then
      if switchyard_stop_native_release_wineserver \
           "${native_release_smoke_runtime:-$signed_runtime}" "$prefix" &&
         switchyard_remove_runtime_prefix_offline preview-native-arm64-fex "$prefix"; then
        prefix=""
      else
        echo "native release cleanup could not safely remove its prepared prefix" >&2
      fi
    fi
    if [ "$native_release_complete" -eq 0 ]; then
      remove_owned_native_release_output \
        "$native_archive_public_path" "$native_archive_public_identity" || true
      remove_owned_native_release_output \
        "$native_checksum_public_path" "$native_checksum_public_identity" || true
      remove_owned_native_release_output \
        "$native_manifest_public_path" "$native_manifest_public_identity" || true
    fi
    if [ -z "$prefix" ]; then
      remove_native_release_private_root || true
    fi
  elif [ -n "$prefix" ]; then
    if [ -n "$signed_runtime" ]; then
      WINEPREFIX="$prefix" "$signed_runtime/bin/wineserver" -k >/dev/null 2>&1 || true
    fi
    /bin/rm -rf "$prefix"
  fi
  [ -z "$manifest_snapshot" ] || /bin/rm -f "$manifest_snapshot"
  [ -z "$mach_o_list" ] || /bin/rm -f "$mach_o_list"
  [ -z "$verification_log" ] || /bin/rm -f "$verification_log"
}

switchyard_validate_native_release_output_root() {
  /usr/bin/python3 -I - "$1" <<'PY'
import os
import stat
import sys

path = sys.argv[1]
if (
    not os.path.isabs(path)
    or os.path.normpath(path) != path
    or path == "/"
    or os.path.realpath(path) != path
):
    raise SystemExit("native release output is not a bounded canonical absolute path")

descriptor = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
try:
    for component in path.split("/")[1:]:
        child = os.open(
            component,
            os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC,
            dir_fd=descriptor,
        )
        info = os.fstat(child)
        if (
            info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            and not info.st_mode & stat.S_ISVTX
        ):
            os.close(child)
            raise SystemExit("native release output has an unsafe writable ancestor")
        os.close(descriptor)
        descriptor = child
    info = os.fstat(descriptor)
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.geteuid()
        or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        raise SystemExit("native release output has an unsafe owner or mode")
finally:
    os.close(descriptor)
PY
}

switchyard_native_release_file_identity() {
  /usr/bin/python3 -I - "$1" <<'PY'
import os
import stat
import sys

info = os.lstat(sys.argv[1])
if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
    raise SystemExit(1)
print(f"{info.st_dev}:{info.st_ino}")
PY
}

switchyard_publish_native_release_file_atomically() {
  /usr/bin/python3 -I - "$1" "$2" <<'PY'
import ctypes
import errno
import os
import stat
import sys

source, destination = sys.argv[1:]
RENAME_EXCL = 0x00000004
flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC

if any(
    not os.path.isabs(path)
    or os.path.normpath(path) != path
    or path == "/"
    or os.path.realpath(os.path.dirname(path)) != os.path.dirname(path)
    for path in (source, destination)
):
    raise SystemExit("native release publication path is not canonical")

def open_directory(path):
    descriptor = os.open("/", flags)
    try:
        for component in path.split("/")[1:]:
            child = os.open(component, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise

source_parent, source_name = os.path.split(source)
destination_parent, destination_name = os.path.split(destination)
if not source_name or not destination_name:
    raise SystemExit("native release publication filename is invalid")
source_fd = open_directory(source_parent)
destination_fd = open_directory(destination_parent)
try:
    source_info = os.stat(source_name, dir_fd=source_fd, follow_symlinks=False)
    if (
        not stat.S_ISREG(source_info.st_mode)
        or source_info.st_uid != os.geteuid()
        or source_info.st_nlink != 1
    ):
        raise SystemExit("native release publication source is unsafe")
    try:
        os.stat(destination_name, dir_fd=destination_fd, follow_symlinks=False)
    except FileNotFoundError:
        pass
    else:
        raise SystemExit("native release publication destination already exists")

    libc = ctypes.CDLL(None, use_errno=True)
    renameatx_np = libc.renameatx_np
    renameatx_np.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    renameatx_np.restype = ctypes.c_int
    if renameatx_np(
        source_fd,
        os.fsencode(source_name),
        destination_fd,
        os.fsencode(destination_name),
        RENAME_EXCL,
    ) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), destination)
    os.fsync(destination_fd)
    published = os.stat(destination_name, dir_fd=destination_fd, follow_symlinks=False)
    if (published.st_dev, published.st_ino) != (source_info.st_dev, source_info.st_ino):
        raise SystemExit("native release publication changed file identity")
finally:
    os.close(destination_fd)
    os.close(source_fd)
PY
}

switchyard_publish_native_release_file() {
  switchyard_publish_native_release_file_atomically "$@"
}

switchyard_validate_native_release_runtime() {
  local runtime_root="$1"
  local runtime_manifest="$2"

  switchyard_validate_runtime_manifest_profile \
    "$runtime_manifest" preview-native-arm64-fex "$runtime_root" &&
    switchyard_validate_native_arm64_runtime_packaging \
      "$runtime_root" "$runtime_manifest" "$ROOT_DIR"
}

switchyard_verify_native_release_team() {
  local item="$1"
  local signing_details

  signing_details="$("$CODESIGN_TOOL" -d --verbose=4 "$item" 2>&1)" || {
    echo "cannot inspect native release signature: $item" >&2
    return 1
  }
  [ "$(/usr/bin/printf '%s\n' "$signing_details" |
      /usr/bin/grep -Fxc "TeamIdentifier=$EXPECTED_TEAM_ID")" -eq 1 ] || {
    echo "native release Mach-O has an unexpected Developer Team ID: $item" >&2
    return 1
  }
}

switchyard_sign_native_release_runtime() {
  local entry_count=0
  local item
  local mach_o_count
  local wine_entry="$signed_runtime/lib/wine/aarch64-unix/wine"
  local fallback_entry="$signed_runtime/bin/wine.switchyard-real"

  mach_o_list="$native_release_private_root/mach-o.list"
  verification_log="$native_release_private_root/codesign-verification.log"
  : >"$mach_o_list"
  : >"$verification_log"

  if ! /usr/bin/find "$signed_runtime" -type f -print0 |
      while IFS= read -r -d '' item; do
        if /usr/bin/file -b "$item" | /usr/bin/grep -q 'Mach-O'; then
          /usr/bin/printf '%s\0' "$item"
        fi
      done >"$mach_o_list"; then
    echo "cannot enumerate native release Mach-O files" >&2
    return 1
  fi
  mach_o_count="$(/usr/bin/python3 -I - "$mach_o_list" <<'PY'
import sys
print(open(sys.argv[1], "rb").read().count(b"\0"))
PY
)"
  [ "$mach_o_count" -gt 0 ] || {
    echo "native release runtime has no Mach-O files" >&2
    return 1
  }

  echo "signing $mach_o_count native ARM64 Mach-O files"
  while IFS= read -r -d '' item; do
    case "$item" in
      "$wine_entry"|"$fallback_entry")
        sign_release_macho_atomically \
          "$CODESIGN_TOOL" "$item" "$IDENTITY" \
          --entitlements "/dev/fd/$entitlements_snapshot_fd" || return 1
        verify_macho_entitlements_snapshot \
          "$CODESIGN_TOOL" preview-native-arm64-fex \
          "$item" "$entitlements_snapshot_fd" || return 1
        entry_count=$((entry_count + 1))
        ;;
      *)
        sign_release_macho_atomically \
          "$CODESIGN_TOOL" "$item" "$IDENTITY" || return 1
        ;;
    esac
    switchyard_verify_native_release_team "$item" || return 1
  done <"$mach_o_list"
  [ "$entry_count" -eq 2 ] || {
    echo "native release does not contain exactly two signed process entries" >&2
    return 1
  }
}

switchyard_run_native_release_smoke() {
  local runtime_root="$1"
  local prepared_prefix="$2"

  "$NATIVE_SMOKE_TOOL" --prefix "$prepared_prefix" \
    "$runtime_root" \
    "$runtime_root/bin/switchyard-wine" \
    "$runtime_root/bin/wineserver" \
    "$runtime_root/lib/wine/aarch64-windows/cmd.exe" \
    SWITCHYARD_NATIVE_RELEASE_OK
}

switchyard_native_release_control_has_exited() {
  local pid="$1"
  local state

  case "$pid" in
    ''|0|*[!0-9]*) return 1 ;;
  esac
  if state="$(/bin/ps -o state= -p "$pid" 2>/dev/null | \
      /usr/bin/awk 'NR == 1 { print $1 }')"; then
    [ -z "$state" ] || [ "${state#Z}" != "$state" ]
  else
    ! /bin/kill -0 "$pid" 2>/dev/null
  fi
}

switchyard_wait_native_release_control() {
  local pid="$1"
  local attempts="$2"
  local count

  case "$attempts" in
    ''|0|*[!0-9]*) return 1 ;;
  esac
  for ((count = 0; count < attempts; count++)); do
    switchyard_native_release_control_has_exited "$pid" && return 0
    /bin/sleep 0.1
  done
  return 1
}

switchyard_run_native_release_wineserver_control() {
  local wineserver="$1"
  local prepared_prefix="$2"
  local operation="$3"
  local control_pid
  local status
  local clean_environment=(
    /usr/bin/env
    -u WINEARCH -u WINEDEBUG -u WINEDLLPATH -u WINELOADER -u WINESERVER
    -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH
    -u DYLD_FRAMEWORK_PATH -u DYLD_FALLBACK_FRAMEWORK_PATH
    -u DYLD_INSERT_LIBRARIES
  )

  [ "$operation" = -k ] || [ "$operation" = -w ] || return 1
  "${clean_environment[@]}" WINEPREFIX="$prepared_prefix" \
    "$wineserver" "$operation" >/dev/null 2>&1 &
  control_pid=$!
  if ! switchyard_wait_native_release_control \
      "$control_pid" "$NATIVE_RELEASE_WINESERVER_CONTROL_ATTEMPTS"; then
    /bin/kill -TERM "$control_pid" 2>/dev/null || true
    if ! { switchyard_wait_native_release_control \
        "$control_pid" "$NATIVE_RELEASE_WINESERVER_TERM_ATTEMPTS"; } 2>/dev/null; then
      /bin/kill -KILL "$control_pid" 2>/dev/null || true
      { switchyard_wait_native_release_control \
        "$control_pid" "$NATIVE_RELEASE_WINESERVER_KILL_ATTEMPTS"; } \
        2>/dev/null || return 124
    fi
    wait "$control_pid" 2>/dev/null || true
    return 124
  fi
  if wait "$control_pid"; then
    status=0
  else
    status=$?
  fi
  return "$status"
}

switchyard_stop_native_release_wineserver() {
  local runtime_root="$1"
  local prepared_prefix="$2"
  local wineserver="$runtime_root/bin/wineserver"

  [ -x "$wineserver" ] && [ ! -L "$wineserver" ] || return 1
  # A prior strict harness pass may already have drained this exact prefix, in
  # which case -k reports failure.  Bound both control clients, then treat only
  # an independently successful exact-prefix -w as proof of quiescence.
  switchyard_run_native_release_wineserver_control \
    "$wineserver" "$prepared_prefix" -k || true
  switchyard_run_native_release_wineserver_control \
    "$wineserver" "$prepared_prefix" -w
}

switchyard_publish_native_outer_digest() {
  write_runtime_content_tree_digest "$1"
}

switchyard_create_native_release_archive() {
  # Keep ACLs, extended attributes, and resource forks out of the portable
  # content contract instead of creating a second __MACOSX metadata root.
  /usr/bin/ditto -c -k --norsrc --noextattr --noacl --keepParent "$1" "$2"
}

switchyard_pin_native_release_archive() {
  /usr/bin/python3 -I - "$1" <<'PY'
import hashlib
import os
import stat
import sys

MAX_ARCHIVE_BYTES = 64 * 1024 * 1024 * 1024
path = sys.argv[1]
flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC

if (
    not os.path.isabs(path)
    or os.path.normpath(path) != path
    or os.path.realpath(os.path.dirname(path)) != os.path.dirname(path)
):
    raise SystemExit("native release archive path is not canonical")
descriptor = os.open(path, flags)
try:
    before = os.fstat(descriptor)
    entry = os.stat(path, follow_symlinks=False)
    identity = lambda value: (
        value.st_dev, value.st_ino, value.st_mode, value.st_nlink,
        value.st_uid, value.st_gid, value.st_size, value.st_mtime_ns,
        value.st_ctime_ns,
    )
    if identity(entry) != identity(before):
        raise SystemExit("native release archive changed while opening")
    if (
        not stat.S_ISREG(before.st_mode)
        or before.st_uid != os.geteuid()
        or before.st_nlink != 1
        or before.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        or before.st_size <= 0
        or before.st_size > MAX_ARCHIVE_BYTES
    ):
        raise SystemExit("native release archive has unsafe metadata or size")
    digest = hashlib.sha256()
    remaining = before.st_size
    while remaining:
        block = os.read(descriptor, min(1024 * 1024, remaining))
        if not block:
            raise SystemExit("native release archive ended early")
        remaining -= len(block)
        digest.update(block)
    if os.read(descriptor, 1) or identity(os.fstat(descriptor)) != identity(before):
        raise SystemExit("native release archive changed while hashing")
    if identity(os.stat(path, follow_symlinks=False)) != identity(before):
        raise SystemExit("native release archive path changed while hashing")
    print(digest.hexdigest() + "\t" + str(before.st_size))
finally:
    os.close(descriptor)
PY
}

switchyard_extract_native_release_archive() {
  [ "$#" -eq 6 ] || {
    echo "usage: switchyard_extract_native_release_archive ARCHIVE STAGING CONTAINER ROOT SHA256 SIZE" >&2
    return 2
  }
  /usr/bin/python3 -I - "$@" <<'PY'
import hashlib
import os
import posixpath
import shutil
import stat
import sys
import unicodedata
import zipfile

(
    archive_name,
    staging_name,
    container_name,
    expected_root,
    expected_sha256,
    expected_archive_size_text,
) = sys.argv[1:]

MAX_ARCHIVE_BYTES = 64 * 1024 * 1024 * 1024
MAX_MEMBER_BYTES = 1024 * 1024 * 1024
MAX_EXPANDED_BYTES = 128 * 1024 * 1024 * 1024
MAX_ENTRIES = 200_000
MAX_PATH_BYTES = 16 * 1024
MAX_LINK_BYTES = 16 * 1024
DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
SUPPORTED_COMPRESSION = {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}


class ArchiveError(Exception):
    pass


def fail(message):
    raise ArchiveError("native release archive self-audit failed: " + message)


def stable(value):
    return (
        value.st_dev, value.st_ino, value.st_mode, value.st_nlink,
        value.st_uid, value.st_gid, value.st_size, value.st_mtime_ns,
        value.st_ctime_ns,
    )


def unsafe_writable_directory(value):
    return (
        value.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        and not value.st_mode & stat.S_ISVTX
    )


def safe_text_component(component, description):
    if (
        component in ("", ".", "..")
        or "/" in component
        or "\\" in component
        or unicodedata.normalize("NFC", component) != component
        or any(unicodedata.category(character) == "Cc" for character in component)
        or len(os.fsencode(component)) > MAX_PATH_BYTES
    ):
        fail(description + " has a non-canonical path component")


def validate_absolute_directory(path, description, exact_mode=None):
    if (
        not os.path.isabs(path)
        or os.path.normpath(path) != path
        or path == os.path.sep
        or os.path.realpath(path) != path
    ):
        fail(description + " is not a canonical absolute directory")
    descriptor = os.open(os.path.sep, DIRECTORY_FLAGS)
    try:
        for component in path.split(os.path.sep)[1:]:
            safe_text_component(component, description)
            child = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            child_info = os.fstat(child)
            if (
                not stat.S_ISDIR(child_info.st_mode)
                or unsafe_writable_directory(child_info)
            ):
                os.close(child)
                fail(description + " has an unsafe writable ancestor")
            os.close(descriptor)
            descriptor = child
        info = os.fstat(descriptor)
        if (
            not stat.S_ISDIR(info.st_mode)
            or info.st_uid != os.geteuid()
            or info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            or exact_mode is not None and stat.S_IMODE(info.st_mode) != exact_mode
        ):
            fail(description + " has unsafe owner or mode")
        return descriptor, info
    except BaseException:
        os.close(descriptor)
        raise


def canonical_member_name(info):
    original = getattr(info, "orig_filename", info.filename)
    if original != info.filename or "\0" in original:
        fail("ZIP member name contains an embedded NUL")
    name = info.filename
    if (
        not name
        or name.startswith("/")
        or "\\" in name
        or len(name.encode("utf-8")) > MAX_PATH_BYTES
    ):
        fail("ZIP member name is not a bounded relative POSIX path")
    is_directory_name = name.endswith("/")
    body = name[:-1] if is_directory_name else name
    components = body.split("/")
    for component in components:
        safe_text_component(component, "ZIP member")
    if posixpath.normpath(body) != body:
        fail("ZIP member name is not normalized")
    return body, components, is_directory_name


def safe_link(relative, target):
    try:
        text = os.fsdecode(target)
    except UnicodeError:
        fail("symbolic-link target is not valid filesystem text")
    if (
        not text
        or posixpath.isabs(text)
        or "\\" in text
        or "\0" in text
        or len(target) > MAX_LINK_BYTES
        or any(unicodedata.category(character) == "Cc" for character in text)
    ):
        fail("symbolic-link target is unsafe: " + relative)
    combined = posixpath.normpath(posixpath.join(posixpath.dirname(relative), text))
    if combined == ".." or combined.startswith("../") or combined.startswith("/"):
        fail("symbolic link escapes the runtime root: " + relative)


def snapshot_staging(root_fd, root_info):
    entries = {}
    aliases = {}
    expanded = 0

    def remember(relative, kind, mode, size, target=None):
        nonlocal expanded
        if len(os.fsencode(relative)) > MAX_PATH_BYTES:
            fail("staging path exceeds its length bound")
        if relative in entries:
            fail("staging tree contains a duplicate path")
        key = unicodedata.normalize("NFC", relative).casefold()
        previous = aliases.get(key)
        if previous is not None and previous != relative:
            fail("staging tree contains a casefold or Unicode path collision")
        aliases[key] = relative
        entries[relative] = (kind, mode, size, target)
        if mode & ~0o777 or (kind != "symlink" and mode & 0o022):
            fail("staging tree contains unsafe permission bits")
        if kind == "file":
            expanded += size
            if size < 0 or size > MAX_MEMBER_BYTES or expanded > MAX_EXPANDED_BYTES:
                fail("staging payload exceeds its resource bounds")
        if len(entries) > MAX_ENTRIES:
            fail("staging tree exceeds its entry-count bound")

    remember(expected_root, "directory", stat.S_IMODE(root_info.st_mode), 0)

    def visit(descriptor, prefix):
        before = os.fstat(descriptor)
        with os.scandir(descriptor) as scan:
            names = sorted(entry.name for entry in scan)
        for name in names:
            safe_text_component(name, "staging tree")
            relative = prefix + "/" + name
            info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            if info.st_uid != os.geteuid():
                fail("staging tree contains a foreign-owned path: " + relative)
            mode = stat.S_IMODE(info.st_mode)
            if stat.S_ISDIR(info.st_mode):
                remember(relative, "directory", mode, 0)
                child = os.open(name, DIRECTORY_FLAGS, dir_fd=descriptor)
                try:
                    if stable(os.fstat(child)) != stable(info):
                        fail("staging directory changed while opening: " + relative)
                    visit(child, relative)
                    if stable(os.fstat(child)) != stable(info):
                        fail("staging directory changed during traversal: " + relative)
                finally:
                    os.close(child)
            elif stat.S_ISREG(info.st_mode):
                if info.st_nlink != 1:
                    fail("staging tree contains a hard-linked file: " + relative)
                remember(relative, "file", mode, info.st_size)
            elif stat.S_ISLNK(info.st_mode):
                if info.st_nlink != 1:
                    fail("staging tree contains a hard-linked symbolic link: " + relative)
                target = os.fsencode(os.readlink(name, dir_fd=descriptor))
                safe_link(relative[len(expected_root) + 1:], target)
                remember(relative, "symlink", mode, len(target), target)
            else:
                fail("staging tree contains an unsupported entry: " + relative)
            if stable(os.stat(name, dir_fd=descriptor, follow_symlinks=False)) != stable(info):
                fail("staging path changed during traversal: " + relative)
        if stable(os.fstat(descriptor)) != stable(before):
            fail("staging directory changed during traversal: " + prefix)

    visit(root_fd, expected_root)
    return entries


def open_parent(root_fd, relative):
    parts = relative.split("/")
    descriptor = os.dup(root_fd)
    try:
        for component in parts[:-1]:
            child = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        return descriptor, parts[-1]
    except BaseException:
        os.close(descriptor)
        raise


created_root = False
try:
    if not expected_root or "/" in expected_root:
        fail("expected archive root is not one canonical component")
    safe_text_component(expected_root, "expected archive root")
    if os.path.basename(staging_name) != expected_root:
        fail("staging root basename does not match the archive root")
    if len(expected_sha256) != 64 or any(c not in "0123456789abcdef" for c in expected_sha256):
        fail("expected archive SHA-256 is malformed")
    try:
        expected_archive_size = int(expected_archive_size_text)
    except ValueError:
        fail("expected archive size is malformed")
    if expected_archive_size <= 0 or expected_archive_size > MAX_ARCHIVE_BYTES:
        fail("expected archive size is outside its bound")

    staging_fd, staging_info = validate_absolute_directory(staging_name, "staging runtime")
    container_fd, container_info = validate_absolute_directory(
        container_name, "archive extraction container", 0o700
    )
    try:
        with os.scandir(container_fd) as scan:
            if next(scan, None) is not None:
                fail("archive extraction container is not empty")
        expected = snapshot_staging(staging_fd, staging_info)
        archive_fd = os.open(archive_name, FILE_FLAGS)
        try:
            archive_info = os.fstat(archive_fd)
            archive_entry = os.stat(archive_name, follow_symlinks=False)
            if stable(archive_entry) != stable(archive_info):
                fail("archive changed while opening")
            if (
                not stat.S_ISREG(archive_info.st_mode)
                or archive_info.st_uid != os.geteuid()
                or archive_info.st_nlink != 1
                or archive_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or archive_info.st_size != expected_archive_size
            ):
                fail("archive metadata does not match its pinned identity")
            digest = hashlib.sha256()
            remaining = archive_info.st_size
            while remaining:
                block = os.read(archive_fd, min(1024 * 1024, remaining))
                if not block:
                    fail("archive ended while checking its pinned identity")
                remaining -= len(block)
                digest.update(block)
            if digest.hexdigest() != expected_sha256:
                fail("archive does not match its pinned SHA-256")
            os.lseek(archive_fd, 0, os.SEEK_SET)

            members = {}
            aliases = {}
            expanded = 0
            archive_stream = os.fdopen(os.dup(archive_fd), "rb")
            try:
                with zipfile.ZipFile(archive_stream, "r") as package:
                    infos = package.infolist()
                    if not infos or len(infos) > MAX_ENTRIES:
                        fail("ZIP member count is outside its bound")
                    for info in infos:
                        relative, components, directory_name = canonical_member_name(info)
                        if components[0] != expected_root:
                            fail("ZIP does not contain exactly the expected root")
                        if relative in members:
                            fail("ZIP contains a duplicate member")
                        alias = unicodedata.normalize("NFC", relative).casefold()
                        previous = aliases.get(alias)
                        if previous is not None and previous != relative:
                            fail("ZIP contains a casefold or Unicode path collision")
                        aliases[alias] = relative
                        if info.flag_bits & 0x1 or info.flag_bits & 0x40:
                            fail("ZIP contains an encrypted member")
                        if info.compress_type not in SUPPORTED_COMPRESSION:
                            fail("ZIP uses an unsupported compression method")
                        if info.file_size < 0 or info.file_size > MAX_MEMBER_BYTES:
                            fail("ZIP member size is outside its bound")
                        expanded += info.file_size
                        if expanded > MAX_EXPANDED_BYTES:
                            fail("ZIP expanded size exceeds its bound")
                        if info.create_system != 3:
                            fail("ZIP member lacks canonical Unix metadata")
                        raw_mode = info.external_attr >> 16
                        kind_bits = stat.S_IFMT(raw_mode)
                        if kind_bits == stat.S_IFDIR and directory_name:
                            kind = "directory"
                        elif kind_bits == stat.S_IFREG and not directory_name:
                            kind = "file"
                        elif kind_bits == stat.S_IFLNK and not directory_name:
                            kind = "symlink"
                        else:
                            fail("ZIP contains an unsupported or inconsistent member type")
                        actual = (kind, stat.S_IMODE(raw_mode), info.file_size)
                        wanted = expected.get(relative)
                        if wanted is None or actual != wanted[:3]:
                            fail("ZIP member closure differs from staging: " + relative)
                        link_data = None
                        if kind == "symlink":
                            if info.file_size > MAX_LINK_BYTES:
                                fail("ZIP symbolic-link target exceeds its bound")
                            link_data = package.read(info)
                            safe_link(relative[len(expected_root) + 1:], link_data)
                            if link_data != wanted[3]:
                                fail("ZIP symbolic-link target differs from staging")
                        members[relative] = (info, kind, link_data)
                    if set(members) != set(expected):
                        fail("ZIP member set is not the exact staging closure")

                    directory_paths = sorted(
                        (path for path, item in members.items() if item[1] == "directory"),
                        key=lambda path: (path.count("/"), path),
                    )
                    for relative in directory_paths:
                        if relative == expected_root:
                            os.mkdir(expected_root, 0o700, dir_fd=container_fd)
                            created_root = True
                        else:
                            parent, name = open_parent(container_fd, relative)
                            try:
                                os.mkdir(name, 0o700, dir_fd=parent)
                            finally:
                                os.close(parent)
                    root_fd = os.open(expected_root, DIRECTORY_FLAGS, dir_fd=container_fd)
                    try:
                        for relative in sorted(members):
                            info, kind, link_data = members[relative]
                            if kind == "directory":
                                continue
                            inside = relative[len(expected_root) + 1:]
                            parent, name = open_parent(root_fd, inside)
                            try:
                                if kind == "symlink":
                                    os.symlink(os.fsdecode(link_data), name, dir_fd=parent)
                                    created = os.stat(name, dir_fd=parent, follow_symlinks=False)
                                    if not stat.S_ISLNK(created.st_mode):
                                        fail("extracted symbolic link changed type")
                                else:
                                    output_fd = os.open(
                                        name,
                                        os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                                        os.O_NOFOLLOW | os.O_CLOEXEC,
                                        0o600,
                                        dir_fd=parent,
                                    )
                                    try:
                                        count = 0
                                        with package.open(info, "r") as source:
                                            while True:
                                                block = source.read(1024 * 1024)
                                                if not block:
                                                    break
                                                count += len(block)
                                                if count > info.file_size:
                                                    fail("ZIP member expanded beyond its declared size")
                                                offset = 0
                                                while offset < len(block):
                                                    written = os.write(output_fd, block[offset:])
                                                    if written <= 0:
                                                        fail("short write while extracting ZIP member")
                                                    offset += written
                                        if count != info.file_size:
                                            fail("ZIP member ended before its declared size")
                                        os.fchmod(output_fd, expected[relative][1])
                                        os.fsync(output_fd)
                                        created = os.fstat(output_fd)
                                        if (
                                            not stat.S_ISREG(created.st_mode)
                                            or created.st_nlink != 1
                                            or created.st_size != info.file_size
                                        ):
                                            fail("extracted file has unexpected metadata")
                                    finally:
                                        os.close(output_fd)
                            finally:
                                os.close(parent)

                        for relative in sorted(
                            directory_paths, key=lambda path: (path.count("/"), path), reverse=True
                        ):
                            if relative == expected_root:
                                os.fchmod(root_fd, expected[relative][1])
                            else:
                                inside = relative[len(expected_root) + 1:]
                                parent, name = open_parent(root_fd, inside)
                                try:
                                    child = os.open(name, DIRECTORY_FLAGS, dir_fd=parent)
                                    try:
                                        os.fchmod(child, expected[relative][1])
                                    finally:
                                        os.close(child)
                                finally:
                                    os.close(parent)
                    finally:
                        os.close(root_fd)
            except (OSError, UnicodeError, zipfile.BadZipFile, zipfile.LargeZipFile) as error:
                fail("ZIP parsing, CRC validation, or extraction failed: " + str(error))
            finally:
                archive_stream.close()

            if stable(os.fstat(archive_fd)) != stable(archive_info):
                fail("archive changed during extraction")
            if stable(os.stat(archive_name, follow_symlinks=False)) != stable(archive_info):
                fail("archive path changed during extraction")
        finally:
            os.close(archive_fd)

        with os.scandir(container_fd) as scan:
            names = [entry.name for entry in scan]
        if names != [expected_root]:
            fail("extraction did not produce exactly one expected root")
        extracted = os.stat(expected_root, dir_fd=container_fd, follow_symlinks=False)
        if not stat.S_ISDIR(extracted.st_mode):
            fail("extracted archive root is not a directory")
    finally:
        os.close(container_fd)
        os.close(staging_fd)
except (ArchiveError, OSError, UnicodeError, ValueError) as error:
    if created_root:
        candidate = os.path.join(container_name, expected_root)
        try:
            if os.path.islink(candidate):
                os.unlink(candidate)
            else:
                shutil.rmtree(candidate)
        except OSError as cleanup_error:
            print(
                "native release archive self-audit cleanup failed: "
                + str(cleanup_error),
                file=sys.stderr,
            )
    print(error, file=sys.stderr)
    raise SystemExit(1)
PY
}

switchyard_validate_extracted_native_release_runtime() {
  local runtime_root="$1"
  local runtime_manifest="$2"

  /usr/bin/python3 -I - "$runtime_root" "$runtime_manifest" <<'PY' || return 1
import json
import os
import stat
import sys

root, manifest = sys.argv[1:]
if (
    not os.path.isabs(root)
    or os.path.normpath(root) != root
    or os.path.realpath(root) != root
    or manifest != os.path.join(root, "switchyard-runtime.json")
):
    raise SystemExit("extracted portable runtime root is not canonical")
info = os.lstat(manifest)
if (
    not stat.S_ISREG(info.st_mode)
    or stat.S_ISLNK(info.st_mode)
    or info.st_size <= 0
    or info.st_size > 1024 * 1024
):
    raise SystemExit("extracted portable runtime manifest is unsafe")


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def reject_constant(item):
    raise ValueError("non-standard JSON constant: " + item)


with open(manifest, "r", encoding="utf-8") as stream:
    value = json.load(
        stream, object_pairs_hook=unique, parse_constant=reject_constant
    )
if type(value) is not dict or value.get("installPrefix") != ".":
    raise SystemExit("extracted runtime installPrefix is not the portable root")
if value.get("executable") != "bin/switchyard-wine":
    raise SystemExit("extracted runtime executable is not the portable launcher")
resolved_root = os.path.normpath(os.path.join(root, value["installPrefix"]))
resolved_executable = os.path.normpath(
    os.path.join(resolved_root, value["executable"])
)
if resolved_root != root or os.path.commonpath((root, resolved_executable)) != root:
    raise SystemExit("portable runtime fields escape the extracted root")
if resolved_executable != os.path.join(root, "bin", "switchyard-wine"):
    raise SystemExit("portable runtime fields do not bind the extracted launcher")
PY
  runtime_content_tree_is_verified "$runtime_root" || {
    echo "extracted native runtime outer content digest did not verify" >&2
    return 1
  }
  switchyard_validate_native_release_runtime "$runtime_root" "$runtime_manifest"
}

switchyard_verify_native_release_macho_tree() {
  local runtime_root="$1"
  local snapshot_fd="$2"

  case "$snapshot_fd" in ''|*[!0-9]*) return 2 ;; esac
  /usr/bin/python3 -I - \
    "$runtime_root" "$EXPECTED_TEAM_ID" "$snapshot_fd" "$CODESIGN_TOOL" <<'PY'
import os
import plistlib
import stat
import subprocess
import sys

root, expected_team, snapshot_text, codesign_tool = sys.argv[1:]
snapshot_fd = int(snapshot_text)
MAX_FILES = 200_000
MAX_OUTPUT = 1024 * 1024
ENTRY_PATHS = {
    "lib/wine/aarch64-unix/wine",
    "bin/wine.switchyard-real",
}


def fail(message):
    raise RuntimeError("extracted Mach-O validation failed: " + message)


def command(arguments):
    result = subprocess.run(
        arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
    )
    if len(result.stdout) + len(result.stderr) > MAX_OUTPUT:
        fail("inspection output exceeds its bound")
    return result


def state(value):
    return (
        value.st_dev, value.st_ino, value.st_mode, value.st_nlink,
        value.st_uid, value.st_gid, value.st_size, value.st_mtime_ns,
        value.st_ctime_ns,
    )


try:
    codesign_info = os.lstat(codesign_tool)
    if (
        not os.path.isabs(codesign_tool)
        or os.path.normpath(codesign_tool) != codesign_tool
        or os.path.realpath(codesign_tool) != codesign_tool
        or not stat.S_ISREG(codesign_info.st_mode)
        or not os.access(codesign_tool, os.X_OK)
    ):
        fail("codesign tool is unsafe")
    root_info = os.lstat(root)
    if (
        not os.path.isabs(root)
        or os.path.normpath(root) != root
        or os.path.realpath(root) != root
        or not stat.S_ISDIR(root_info.st_mode)
        or stat.S_ISLNK(root_info.st_mode)
        or root_info.st_uid != os.geteuid()
        or root_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        fail("runtime root is unsafe")
    snapshot_info = os.fstat(snapshot_fd)
    if (
        not stat.S_ISREG(snapshot_info.st_mode)
        or snapshot_info.st_size <= 0
        or snapshot_info.st_size > 65536
    ):
        fail("entitlement snapshot is unsafe")
    snapshot_data = b""
    while len(snapshot_data) < snapshot_info.st_size:
        block = os.pread(
            snapshot_fd,
            snapshot_info.st_size - len(snapshot_data),
            len(snapshot_data),
        )
        if not block:
            fail("entitlement snapshot ended early")
        snapshot_data += block
    if os.pread(snapshot_fd, 1, snapshot_info.st_size):
        fail("entitlement snapshot grew while reading")
    if state(os.fstat(snapshot_fd)) != state(snapshot_info):
        fail("entitlement snapshot changed while reading")
    expected_entitlements = plistlib.loads(snapshot_data)
    if type(expected_entitlements) is not dict or not expected_entitlements:
        fail("entitlement snapshot is malformed")

    candidates = []
    for directory, directories, files in os.walk(root, followlinks=False):
        directories.sort()
        files.sort()
        for name in files:
            path = os.path.join(directory, name)
            info = os.lstat(path)
            if not stat.S_ISREG(info.st_mode):
                continue
            candidates.append((path, os.path.relpath(path, root), info))
            if len(candidates) > MAX_FILES:
                fail("runtime file count exceeds its bound")

    macho_count = 0
    found_entries = set()
    for path, relative, before in candidates:
        identified = command(["/usr/bin/file", "-b", path])
        if identified.returncode:
            fail("cannot identify runtime file: " + relative)
        if b"Mach-O" not in identified.stdout:
            continue
        macho_count += 1
        strict = command(
            [codesign_tool, "--verify", "--strict", "--verbose=2", path]
        )
        if strict.returncode:
            fail("strict signature verification failed: " + relative)
        details_result = command(
            [codesign_tool, "-d", "--verbose=4", path]
        )
        if details_result.returncode:
            fail("cannot inspect signature: " + relative)
        details = (details_result.stdout + details_result.stderr).decode(
            "utf-8", "strict"
        )
        lines = details.splitlines()
        if (
            "Signature=adhoc" in lines
            or "Runtime Version=" not in details
            or not any(
                line.startswith("Authority=Developer ID Application:")
                for line in lines
            )
            or lines.count("TeamIdentifier=" + expected_team) != 1
        ):
            fail(
                "Developer ID, Hardened Runtime, or Team ID is not exact: "
                + relative
            )

        arches_result = command(["/usr/bin/lipo", path, "-archs"])
        if arches_result.returncode:
            fail("cannot inspect architecture: " + relative)
        arches = arches_result.stdout.decode("ascii", "strict").split()
        expected_arches = (
            {"x86_64", "arm64"}
            if relative.startswith("lib/switchyard-gstreamer/")
            else {"arm64"}
        )
        if set(arches) != expected_arches or len(arches) != len(expected_arches):
            fail("Mach-O architecture set is not exact: " + relative)

        entitlements = command(
            [
                codesign_tool, "-d", "--xml", "--entitlements", "-",
                path,
            ]
        )
        if relative in ENTRY_PATHS:
            if entitlements.returncode:
                fail("cannot inspect process-entry entitlements: " + relative)
            try:
                embedded = plistlib.loads(entitlements.stdout)
            except plistlib.InvalidFileException as error:
                fail(
                    "process-entry entitlements are malformed: "
                    + relative + ": " + str(error)
                )
            if embedded != expected_entitlements:
                fail("process-entry entitlements are not exact: " + relative)
            found_entries.add(relative)
        else:
            payload = entitlements.stdout.strip()
            if payload:
                try:
                    embedded = plistlib.loads(payload)
                except plistlib.InvalidFileException:
                    fail(
                        "non-entry Mach-O entitlement output is malformed: "
                        + relative
                    )
                if embedded != {}:
                    fail(
                        "non-entry Mach-O unexpectedly has entitlements: "
                        + relative
                    )
        after = os.lstat(path)
        if state(after) != state(before):
            fail("Mach-O changed during validation: " + relative)

    if macho_count == 0:
        fail("runtime contains no Mach-O files")
    if found_entries != ENTRY_PATHS:
        fail("runtime does not contain the exact process-entry Mach-O set")
    if state(os.lstat(root)) != state(root_info):
        fail("runtime root changed during Mach-O validation")
except (OSError, RuntimeError, UnicodeError, ValueError) as error:
    print(error, file=sys.stderr)
    raise SystemExit(1)
PY
}

switchyard_submit_native_release_notary() {
  local release_archive="$1"
  local keychain_profile="$2"
  local result_path="$3"

  /usr/bin/xcrun notarytool submit "$release_archive" \
    --keychain-profile "$keychain_profile" \
    --wait --output-format json >"$result_path"
}

switchyard_release_text_has_no_korean() {
  /usr/bin/python3 -I - "$@" <<'PY'
import sys

for name in sys.argv[1:]:
    with open(name, "r", encoding="utf-8") as stream:
        data = stream.read()
    if any(
        "\u1100" <= character <= "\u11ff"
        or "\u3130" <= character <= "\u318f"
        or "\uac00" <= character <= "\ud7a3"
        for character in data
    ):
        raise SystemExit("native release metadata contains Korean text")
PY
}

switchyard_release_preview_native() {
  local archive_private
  local archive_pin
  local archive_recheck
  local archive_sha256
  local archive_size
  local checksum_private
  local extracted_manifest
  local extraction_container
  local manifest_private
  local notary_id
  local notary_result
  local notary_status
  local portable_manifest
  local release_architecture_command=""
  local release_pe_architectures=""
  local smoke_status
  local signed_runtime_digest
  local prefix_candidate
  local pe_architecture
  local command_part

  [[ "$EXPECTED_TEAM_ID" =~ ^[A-Z0-9]{10}$ ]] || {
    echo "native release Developer Team ID is malformed" >&2
    return 2
  }
  mkdir -p "$OUTPUT_DIR"
  OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd -P)"
  switchyard_validate_native_release_output_root "$OUTPUT_DIR" || return 1

  archive_name="${release_root_name}.zip"
  archive="$OUTPUT_DIR/$archive_name"
  release_manifest="$OUTPUT_DIR/switchyard-runtime-release.json"
  checksum_file="$OUTPUT_DIR/${archive_name}.sha256"
  for destination in "$archive" "$release_manifest" "$checksum_file"; do
    [ ! -e "$destination" ] && [ ! -L "$destination" ] || {
      echo "release output already exists: $destination" >&2
      return 1
    }
  done

  native_release_private_root="$(
    /usr/bin/mktemp -d "$OUTPUT_DIR/.switchyard-native-release.XXXXXX"
  )"
  native_release_private_root="$(cd "$native_release_private_root" && pwd -P)"
  /bin/chmod 0700 "$native_release_private_root"
  signed_runtime="$native_release_private_root/$release_root_name"
  archive_private="$native_release_private_root/$archive_name"
  checksum_private="$native_release_private_root/${archive_name}.sha256"
  manifest_private="$native_release_private_root/switchyard-runtime-release.json"

  echo "cloning native ARM64 runtime into private release staging"
  /bin/cp -cR "$RUNTIME" "$signed_runtime"
  [ "$(cd "$signed_runtime" && pwd -P)" = "$signed_runtime" ] || {
    echo "native release staging root is not physically canonical" >&2
    return 1
  }
  runtime_content_tree_is_verified "$signed_runtime" || {
    echo "native release staging content does not match the build-time runtime digest" >&2
    return 1
  }
  [ "$(runtime_content_tree_digest "$signed_runtime")" = "$EXPECTED_RUNTIME_DIGEST" ] || {
    echo "native release staging does not match the separately recorded build digest" >&2
    return 1
  }

  portable_manifest="$signed_runtime/switchyard-runtime.json"
  [ -f "$portable_manifest" ] && [ ! -L "$portable_manifest" ] || {
    echo "native release staging manifest is missing or unsafe" >&2
    return 1
  }
  /usr/bin/plutil -replace installPrefix -string "." "$portable_manifest"
  /usr/bin/plutil -replace executable -string "bin/switchyard-wine" "$portable_manifest"

  create_validated_entitlements_snapshot \
    preview-native-arm64-fex "$ENTITLEMENTS" \
    "$native_release_private_root" entitlements_snapshot_fd || return 1
  switchyard_sign_native_release_runtime || return 1

  # This producer operation is consuming and single-shot.  Any failure leaves
  # the private clone unusable; cleanup discards the whole staging root.
  switchyard_refresh_native_arm64_signed_runtime_manifest \
    "$signed_runtime" "$portable_manifest" || return 1
  switchyard_validate_native_release_runtime \
    "$signed_runtime" "$portable_manifest" || return 1

  # Bind every later check to the exact validated, signed staging payload.
  # The outer marker is excluded from this digest, so pinning before its
  # replacement also closes coherent payload+marker mutation races.
  signed_runtime_digest="$(runtime_content_tree_digest "$signed_runtime")" || return 1
  [[ "$signed_runtime_digest" =~ ^[0-9a-f]{64}$ ]] || {
    echo "native release signed staging digest is malformed" >&2
    return 1
  }

  # The outer marker is the sole allowed runtime mutation after the signed
  # manifest refresh.  Everything below this point is read-only for the tree.
  switchyard_publish_native_outer_digest "$signed_runtime" || return 1
  runtime_content_tree_is_verified "$signed_runtime" || {
    echo "native release outer content digest did not verify" >&2
    return 1
  }
  [ "$(runtime_content_tree_digest "$signed_runtime")" = \
      "$signed_runtime_digest" ] || {
    echo "native release signed staging changed before archive creation" >&2
    return 1
  }

  echo "creating $archive_name"
  switchyard_create_native_release_archive \
    "$signed_runtime" "$archive_private" || return 1
  archive_pin="$(switchyard_pin_native_release_archive "$archive_private")" || return 1
  case "$archive_pin" in
    *$'\t'*)
      archive_sha256="${archive_pin%%$'\t'*}"
      archive_size="${archive_pin#*$'\t'}"
      ;;
    *)
      echo "native release archive pin is malformed" >&2
      return 1
      ;;
  esac

  extraction_container="$(
    /usr/bin/mktemp -d "$native_release_private_root/extracted.XXXXXX"
  )" || return 1
  extraction_container="$(cd "$extraction_container" && pwd -P)"
  case "$extraction_container" in
    "$native_release_private_root"/extracted.??????) ;;
    *)
      echo "native release extraction container is not inside private staging" >&2
      return 1
      ;;
  esac
  /bin/chmod 0700 "$extraction_container"
  switchyard_extract_native_release_archive \
    "$archive_private" "$signed_runtime" "$extraction_container" \
    "$release_root_name" "$archive_sha256" "$archive_size" || return 1
  native_release_smoke_runtime="$extraction_container/$release_root_name"
  extracted_manifest="$native_release_smoke_runtime/switchyard-runtime.json"
  switchyard_validate_extracted_native_release_runtime \
    "$native_release_smoke_runtime" "$extracted_manifest" || return 1
  [ "$(runtime_content_tree_digest "$native_release_smoke_runtime")" = \
      "$signed_runtime_digest" ] || {
    echo "extracted native runtime differs from signed staging" >&2
    return 1
  }
  switchyard_verify_native_release_macho_tree \
    "$native_release_smoke_runtime" "$entitlements_snapshot_fd" || return 1
  runtime_content_tree_is_verified "$native_release_smoke_runtime" || {
    echo "extracted native runtime changed during Mach-O validation" >&2
    return 1
  }
  [ "$(runtime_content_tree_digest "$native_release_smoke_runtime")" = \
      "$signed_runtime_digest" ] || {
    echo "extracted native runtime changed during Mach-O validation" >&2
    return 1
  }
  close_validated_entitlements_snapshot "$entitlements_snapshot_fd" || return 1
  entitlements_snapshot_fd=""

  prefix_candidate="$native_release_private_root/smoke-prefix"
  switchyard_prepare_runtime_prefix \
    preview-native-arm64-fex "$prefix_candidate" prefix || return 1
  if switchyard_run_native_release_smoke \
       "$native_release_smoke_runtime" "$prefix"; then
    smoke_status=0
  else
    smoke_status=$?
  fi
  # The canonical harness already stops and waits this exact wineserver on all
  # returns.  Repeating the exact stop/wait makes release cleanup independently
  # fail closed before the fd-relative prefix removal.
  switchyard_stop_native_release_wineserver \
    "$native_release_smoke_runtime" "$prefix" || return 1
  switchyard_remove_runtime_prefix_offline \
    preview-native-arm64-fex "$prefix" || return 1
  prefix=""
  [ "$smoke_status" -eq 0 ] || {
    echo "native release failed the strict no-Rosetta smoke test" >&2
    return "$smoke_status"
  }
  runtime_content_tree_is_verified "$native_release_smoke_runtime" || {
    echo "extracted native runtime changed during no-Rosetta smoke" >&2
    return 1
  }
  [ "$(runtime_content_tree_digest "$native_release_smoke_runtime")" = \
      "$signed_runtime_digest" ] || {
    echo "extracted native runtime changed during no-Rosetta smoke" >&2
    return 1
  }

  archive_recheck="$(switchyard_pin_native_release_archive "$archive_private")" || return 1
  [ "$archive_recheck" = "$archive_pin" ] || {
    echo "native release archive changed after extraction self-audit" >&2
    return 1
  }

  notary_status="not-submitted"
  notary_id=""
  if [ -n "$NOTARY_PROFILE" ]; then
    notary_result="$native_release_private_root/notary-result.json"
    switchyard_submit_native_release_notary \
      "$archive_private" "$NOTARY_PROFILE" "$notary_result" || return 1
    notary_status="$(manifest_value status "$notary_result")"
    notary_id="$(manifest_value id "$notary_result")"
    /bin/rm -f "$notary_result"
    [ "$notary_status" = "Accepted" ] || {
      echo "Apple notarization did not accept the native runtime archive: $notary_status" >&2
      return 1
    }
    [[ "$notary_id" =~ ^[A-Za-z0-9._-]+$ ]] || {
      echo "Apple notarization returned a malformed submission ID" >&2
      return 1
    }
  fi

  archive_recheck="$(switchyard_pin_native_release_archive "$archive_private")" || return 1
  [ "$archive_recheck" = "$archive_pin" ] || {
    echo "native release archive changed before publication" >&2
    return 1
  }
  /usr/bin/printf '%s  %s\n' \
    "$archive_sha256" "$archive_name" >"$checksum_private"

  for pe_architecture in "${SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS[@]}"; do
    [ -z "$release_pe_architectures" ] || release_pe_architectures+=", "
    release_pe_architectures+="\"$pe_architecture\""
  done
  for command_part in "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[@]}"; do
    [ -z "$release_architecture_command" ] || release_architecture_command+=", "
    release_architecture_command+="\"$command_part\""
  done

  cat >"$manifest_private" <<EOF
{
  "schemaVersion": 1,
  "runtimeManifestVersion": $SWITCHYARD_RUNTIME_MANIFEST_VERSION,
  "runtimeID": "$runtime_id",
  "runtimeProfile": "$SWITCHYARD_RUNTIME_PROFILE",
  "sourceRevision": "$source_revision",
  "archive": "$archive_name",
  "archiveSha256": "$archive_sha256",
  "archiveSize": $archive_size,
  "platform": "macos",
  "hostArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH",
  "wineUnixArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH",
  "buildTriplet": "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET",
  "hostTriplet": "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET",
  "architectureCommand": [$release_architecture_command],
  "requiresRosetta": $SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA,
  "minimumMacOS": "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS",
  "gstreamerRegistryArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH",
  "peArchitectures": [$release_pe_architectures],
  "developerTeamID": "$EXPECTED_TEAM_ID",
  "notarizationStatus": "$notary_status",
  "notarizationID": "$notary_id"
}
EOF
  switchyard_release_text_has_no_korean "$manifest_private" || return 1

  native_archive_public_path="$archive"
  native_archive_public_identity="$(
    switchyard_native_release_file_identity "$archive_private"
  )"
  switchyard_publish_native_release_file "$archive_private" "$archive" || return 1
  native_checksum_public_path="$checksum_file"
  native_checksum_public_identity="$(
    switchyard_native_release_file_identity "$checksum_private"
  )"
  switchyard_publish_native_release_file \
    "$checksum_private" "$checksum_file" || return 1
  native_manifest_public_path="$release_manifest"
  native_manifest_public_identity="$(
    switchyard_native_release_file_identity "$manifest_private"
  )"
  switchyard_publish_native_release_file \
    "$manifest_private" "$release_manifest" || return 1

  remove_native_release_private_root || return 1
  native_release_complete=1
  echo "runtime release archive: $archive"
  echo "runtime release manifest: $release_manifest"
  echo "runtime archive sha256: $archive_sha256"
  echo "runtime notarization: $notary_status${notary_id:+ ($notary_id)}"
}

usage() {
  cat >&2 <<EOF
usage: $0 --runtime PATH --runtime-content-sha256 SHA256 --output DIR --identity IDENTITY [--notary-profile PROFILE]
EOF
  exit 2
}

release_main() {
trap cleanup EXIT

while [ "$#" -gt 0 ]; do
  case "$1" in
    --runtime)
      [ "$#" -ge 2 ] || usage
      RUNTIME="$2"
      shift 2
      ;;
    --runtime-content-sha256)
      [ "$#" -ge 2 ] || usage
      EXPECTED_RUNTIME_DIGEST="$2"
      shift 2
      ;;
    --output)
      [ "$#" -ge 2 ] || usage
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --identity)
      [ "$#" -ge 2 ] || usage
      IDENTITY="$2"
      shift 2
      ;;
    --notary-profile)
      [ "$#" -ge 2 ] || usage
      NOTARY_PROFILE="$2"
      shift 2
      ;;
    *) usage ;;
  esac
done

[ -n "$RUNTIME" ] || usage
[ -n "$OUTPUT_DIR" ] || usage
[ -n "$IDENTITY" ] || usage
[[ "$EXPECTED_RUNTIME_DIGEST" =~ ^[0-9a-f]{64}$ ]] || {
  echo "runtime content SHA-256 must contain exactly 64 lowercase hexadecimal characters" >&2
  exit 2
}
[ -d "$RUNTIME" ] || { echo "runtime does not exist: $RUNTIME" >&2; exit 1; }
[ ! -L "$RUNTIME" ] || { echo "runtime path must not be a symbolic link" >&2; exit 1; }
[ -f "$RUNTIME/switchyard-runtime.json" ] || { echo "runtime manifest is missing" >&2; exit 1; }
[ ! -L "$RUNTIME/switchyard-runtime.json" ] || { echo "runtime manifest must not be a symbolic link" >&2; exit 1; }
manifest_value() {
  /usr/bin/plutil -extract "$1" raw -o - "$2" 2>/dev/null || true
}

sha256_file() {
  /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'
}

runtime_content_tree_is_verified() {
  local root="$1"
  /usr/bin/python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" verify "$root"
}

runtime_content_tree_digest() {
  local root="$1"
  /usr/bin/python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" digest "$root"
}

write_runtime_content_tree_digest() {
  local root="$1"
  /usr/bin/python3 "$ROOT_DIR/switchyard/runtime_content_digest.py" write "$root" >/dev/null
}

manifest="$RUNTIME/switchyard-runtime.json"
manifest_snapshot="$(/usr/bin/mktemp)"
/bin/cp "$manifest" "$manifest_snapshot"
manifest_snapshot_sha256="$(sha256_file "$manifest_snapshot")"
runtime_profile="$(manifest_value runtimeFamily "$manifest_snapshot")"
switchyard_runtime_profile_is_known "$runtime_profile" || {
  echo "runtime manifest has an unknown runtime profile" >&2
  exit 1
}
switchyard_load_runtime_profile "$runtime_profile" || exit $?
if [ "$runtime_profile" = preview-native-arm64-fex ]; then
  RUNTIME="$(cd "$RUNTIME" && pwd -P)"
  manifest="$RUNTIME/switchyard-runtime.json"
fi
ENTITLEMENTS="$(switchyard_runtime_profile_entitlements_path "$ROOT_DIR")" || exit $?
[ -f "$ENTITLEMENTS" ] && [ ! -L "$ENTITLEMENTS" ] || {
  echo "runtime signing entitlements are missing or invalid" >&2
  exit 1
}
switchyard_require_runtime_profile_enabled || exit $?
if [ "$runtime_profile" = preview-native-arm64-fex ]; then
  switchyard_validate_runtime_manifest_profile \
    "$manifest" "$runtime_profile" "$RUNTIME" || exit 1
else
  switchyard_validate_runtime_manifest_profile "$manifest_snapshot" "$runtime_profile" || exit 1
fi
runtime_id="$(manifest_value id "$manifest_snapshot")"
source_revision="$(manifest_value sourceRevision "$manifest_snapshot")"
source_dirty="$(manifest_value sourceDirty "$manifest_snapshot")"
gptk_path="$(manifest_value gptkPath "$manifest_snapshot")"
gptk_digest="$(manifest_value gptkRedistDigest "$manifest_snapshot")"
current_revision="$(git -C "$ROOT_DIR" rev-parse HEAD)"

[ -n "$runtime_id" ] || { echo "runtime manifest has no id" >&2; exit 1; }
[ "$source_revision" = "$current_revision" ] || {
  echo "runtime source $source_revision does not match current source $current_revision" >&2
  exit 1
}
[ "$source_dirty" = "false" ] || { echo "release runtime was built from a dirty source tree" >&2; exit 1; }
[ -z "$gptk_path" ] || { echo "release runtime records a user-provided GPTK path" >&2; exit 1; }
[ "$gptk_digest" = "no-gptk" ] || { echo "release runtime contains a GPTK overlay" >&2; exit 1; }
runtime_content_tree_is_verified "$RUNTIME" || {
  echo "release runtime content digest is missing or does not match the runtime tree" >&2
  exit 1
}
[ "$(runtime_content_tree_digest "$RUNTIME")" = "$EXPECTED_RUNTIME_DIGEST" ] || {
  echo "release runtime content does not match the separately recorded build digest" >&2
  exit 1
}
[ -f "$manifest" ] && [ ! -L "$manifest" ] || {
  echo "runtime manifest changed type during release validation" >&2
  exit 1
}
[ "$(sha256_file "$manifest")" = "$manifest_snapshot_sha256" ] || {
  echo "runtime manifest changed during release validation" >&2
  exit 1
}
if [ "$runtime_profile" = preview-native-arm64-fex ]; then
  switchyard_validate_native_arm64_runtime_packaging \
    "$RUNTIME" "$manifest" "$ROOT_DIR" || exit 1
fi

for required_notice in \
  share/doc/switchyard-wine/LICENSE \
  share/doc/switchyard-wine/COPYING.LIB \
  share/doc/switchyard-wine/AUTHORS \
  share/doc/switchyard-wine/CORRESPONDING-SOURCE.txt \
  lib/switchyard-mesa/share/doc/switchyard-mesa/README.txt \
  lib/switchyard-mesa/share/doc/switchyard-mesa/MESA-LICENSE.rst \
  lib/switchyard-mesa/share/doc/switchyard-mesa/LLVM-LICENSE.txt \
  lib/switchyard-mesa/share/doc/switchyard-mesa/DISTRIBUTOR-LICENSE.txt \
  lib/switchyard-gstreamer/share/doc/switchyard-gstreamer/README.txt \
  lib/switchyard-gstreamer/share/doc/switchyard-gstreamer/INSTALLER-LICENSE.txt \
  lib/switchyard-gstreamer/share/doc/switchyard-gstreamer/packages.tsv \
  lib/switchyard-gstreamer/share/licenses/gstreamer-1.0/LGPL-2.0-or-later.txt \
  lib/switchyard-gstreamer/share/licenses/ffmpeg/LGPL-2.1-or-later.txt \
  lib/switchyard-tls/share/doc/switchyard-tls/packages.tsv \
  lib/switchyard-tls/share/doc/switchyard-tls/sources.tsv; do
  { [ -f "$RUNTIME/$required_notice" ] &&
    { [ "$runtime_profile" != preview-native-arm64-fex ] ||
      [ ! -L "$RUNTIME/$required_notice" ]; }; } || {
    echo "release runtime is missing notice $required_notice" >&2
    exit 1
  }
done

if /usr/bin/find "$RUNTIME" \( -iname '*d3dmetal*' -o -iname '*metalirconverter*' -o -iname 'libd3dshared*' \) -print -quit |
   /usr/bin/grep -q .; then
  echo "release runtime contains a user-provided Apple graphics component" >&2
  exit 1
fi
if /usr/bin/grep -R -I -q -E '/Users/[^/]+/.+(Game.Porting.Toolkit|heroic)' \
     "$RUNTIME/switchyard-runtime.json" "$RUNTIME/lib/switchyard-gstreamer/share/doc" \
     "$RUNTIME/lib/switchyard-tls/share/doc" 2>/dev/null; then
  echo "release runtime contains a user-local toolkit provenance path" >&2
  exit 1
fi
if /usr/bin/grep -R -I -q -E '/Users/[^/]+' \
     "$RUNTIME/lib/switchyard-gstreamer" 2>/dev/null; then
  echo "release runtime contains a user-local GStreamer build path" >&2
  exit 1
fi

if [ "$runtime_profile" = preview-native-arm64-fex ]; then
  release_short_revision="${source_revision:0:12}"
  release_root_name="Switchyard-Wine-Runtime-${release_short_revision}-${SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX}"
  switchyard_release_preview_native
  return
fi

mkdir -p "$OUTPUT_DIR"
release_short_revision="${source_revision:0:12}"
release_root_name="Switchyard-Wine-Runtime-${release_short_revision}-${SWITCHYARD_RUNTIME_PROFILE_RELEASE_SUFFIX}"
signed_runtime="$OUTPUT_DIR/$release_root_name"
archive_name="${release_root_name}.zip"
archive="$OUTPUT_DIR/$archive_name"
release_manifest="$OUTPUT_DIR/switchyard-runtime-release.json"
checksum_file="$OUTPUT_DIR/${archive_name}.sha256"

for destination in "$signed_runtime" "$archive" "$release_manifest" "$checksum_file"; do
  [ ! -e "$destination" ] || {
    echo "release output already exists: $destination" >&2
    exit 1
  }
done

echo "cloning runtime into release staging"
/bin/cp -cR "$RUNTIME" "$signed_runtime"
runtime_content_tree_is_verified "$signed_runtime" || {
  echo "release staging content does not match the build-time runtime digest" >&2
  exit 1
}
[ "$(runtime_content_tree_digest "$signed_runtime")" = "$EXPECTED_RUNTIME_DIGEST" ] || {
  echo "release staging content does not match the separately recorded build digest" >&2
  exit 1
}
portable_manifest="$signed_runtime/switchyard-runtime.json"
/usr/bin/plutil -replace installPrefix -string "." "$portable_manifest"
/usr/bin/plutil -replace executable -string "bin/switchyard-wine" "$portable_manifest"
wine_unix_executable="$signed_runtime/lib/wine/${SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH}-unix/wine"

mach_o_list="$(/usr/bin/mktemp)"
verification_log="$(/usr/bin/mktemp)"

/usr/bin/find "$signed_runtime" -type f -print0 |
while IFS= read -r -d '' item; do
  if /usr/bin/file -b "$item" | /usr/bin/grep -q 'Mach-O'; then
    /usr/bin/printf '%s\0' "$item"
  fi
done > "$mach_o_list"

mach_o_count="$(/usr/bin/python3 - "$mach_o_list" <<'PY'
import sys
print(open(sys.argv[1], 'rb').read().count(bytes([0])))
PY
)"
[ "$mach_o_count" -gt 0 ] || { echo "release runtime has no Mach-O files" >&2; exit 1; }

echo "signing $mach_o_count Mach-O files"
while IFS= read -r -d '' item; do
  /usr/bin/codesign --force --sign "$IDENTITY" --options runtime --timestamp "$item"
done < "$mach_o_list"

for launcher in \
  "$wine_unix_executable" \
  "$signed_runtime/bin/wine.switchyard-real"; do
  [ -f "$launcher" ] || { echo "release runtime is missing launcher $launcher" >&2; exit 1; }
  /usr/bin/codesign --force --sign "$IDENTITY" --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" "$launcher"
done

wine_sha256="$(sha256_file "$wine_unix_executable")"
/usr/bin/plutil -replace integrity.wineUnixSha256 -string "$wine_sha256" "$portable_manifest"

while IFS= read -r -d '' item; do
  if ! /usr/bin/codesign --verify --strict --verbose=2 "$item" >"$verification_log" 2>&1; then
    /bin/cat "$verification_log" >&2
    exit 1
  fi
done < "$mach_o_list"

signing_details="$(/usr/bin/codesign -d --verbose=4 "$wine_unix_executable" 2>&1)"
/usr/bin/printf '%s\n' "$signing_details" | /usr/bin/grep -F "TeamIdentifier=$EXPECTED_TEAM_ID" >/dev/null || {
  echo "signed runtime has an unexpected Developer Team ID" >&2
  exit 1
}
/usr/bin/printf '%s\n' "$signing_details" | /usr/bin/grep -F 'Runtime Version=' >/dev/null || {
  echo "signed runtime is missing Hardened Runtime" >&2
  exit 1
}

prefix="$(/usr/bin/mktemp -d /tmp/switchyard-release-prefix.XXXXXX)"
smoke_output="$(WINEPREFIX="$prefix" WINEDEBUG=-all "$signed_runtime/bin/switchyard-wine" cmd /c ver)"
/usr/bin/printf '%s' "$smoke_output" | /usr/bin/grep -F 'Microsoft Windows 10.0.19045' >/dev/null || {
  echo "signed runtime failed the fresh-prefix smoke test" >&2
  exit 1
}
WINEPREFIX="$prefix" "$signed_runtime/bin/wineserver" -k >/dev/null 2>&1 || true
/bin/sleep 1
/bin/rm -rf "$prefix"
prefix=""

write_runtime_content_tree_digest "$signed_runtime"
echo "creating $archive_name"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$signed_runtime" "$archive"
archive_sha256="$(sha256_file "$archive")"
archive_size="$(/usr/bin/stat -f '%z' "$archive")"
/usr/bin/printf '%s  %s\n' "$archive_sha256" "$archive_name" > "$checksum_file"

notary_status="not-submitted"
notary_id=""
if [ -n "$NOTARY_PROFILE" ]; then
  notary_result="$(/usr/bin/mktemp)"
  /usr/bin/xcrun notarytool submit "$archive" --keychain-profile "$NOTARY_PROFILE" \
    --wait --output-format json > "$notary_result"
  notary_status="$(manifest_value status "$notary_result")"
  notary_id="$(manifest_value id "$notary_result")"
  /bin/rm -f "$notary_result"
  [ "$notary_status" = "Accepted" ] || {
    echo "Apple notarization did not accept the runtime archive: $notary_status" >&2
    exit 1
  }
fi

release_pe_architectures=""
for pe_architecture in "${SWITCHYARD_RUNTIME_PROFILE_PE_ARCHS[@]}"; do
  [ -z "$release_pe_architectures" ] || release_pe_architectures+=", "
  release_pe_architectures+="\"$pe_architecture\""
done
release_architecture_command=""
for command_part in "${SWITCHYARD_RUNTIME_PROFILE_ARCH_COMMAND[@]}"; do
  [ -z "$release_architecture_command" ] || release_architecture_command+=", "
  release_architecture_command+="\"$command_part\""
done

cat > "$release_manifest" <<EOF
{
  "schemaVersion": 1,
  "runtimeManifestVersion": $SWITCHYARD_RUNTIME_MANIFEST_VERSION,
  "runtimeID": "$runtime_id",
  "runtimeProfile": "$SWITCHYARD_RUNTIME_PROFILE",
  "sourceRevision": "$source_revision",
  "archive": "$archive_name",
  "archiveSha256": "$archive_sha256",
  "archiveSize": $archive_size,
  "platform": "macos",
  "hostArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_MACHO_ARCH",
  "wineUnixArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_WINE_UNIX_ARCH",
  "buildTriplet": "$SWITCHYARD_RUNTIME_PROFILE_BUILD_TRIPLET",
  "hostTriplet": "$SWITCHYARD_RUNTIME_PROFILE_HOST_TRIPLET",
  "architectureCommand": [$release_architecture_command],
  "requiresRosetta": $SWITCHYARD_RUNTIME_PROFILE_REQUIRES_ROSETTA,
  "minimumMacOS": "$SWITCHYARD_RUNTIME_PROFILE_MINIMUM_MACOS",
  "gstreamerRegistryArchitecture": "$SWITCHYARD_RUNTIME_PROFILE_GSTREAMER_REGISTRY_ARCH",
  "peArchitectures": [$release_pe_architectures],
  "developerTeamID": "$EXPECTED_TEAM_ID",
  "notarizationStatus": "$notary_status",
  "notarizationID": "$notary_id"
}
EOF

echo "runtime release archive: $archive"
echo "runtime release manifest: $release_manifest"
echo "runtime archive sha256: $archive_sha256"
echo "runtime notarization: $notary_status${notary_id:+ ($notary_id)}"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  release_main "$@"
fi
