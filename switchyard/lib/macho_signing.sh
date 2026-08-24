#!/usr/bin/env bash

# Atomic Mach-O signing and immutable entitlement snapshots.  Build and release
# scripts own target selection; this library signs one already-selected file.

readonly SWITCHYARD_ENTITLEMENTS_SNAPSHOT_FD=19

remove_owned_entitlements_snapshot_path() {
  local runtime_profile="$1"
  local private_root="$2"
  local snapshot_name="$3"
  local snapshot_device="$4"
  local snapshot_inode="$5"

  /usr/bin/python3 -I - \
    "$runtime_profile" "$private_root" "$snapshot_name" \
    "$snapshot_device" "$snapshot_inode" <<'PY'
import os
import plistlib
import stat
import sys

DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
MAX_ENTITLEMENTS_SIZE = 65536


def expected_entitlements(profile):
    if profile == "stable-x86_64-rosetta":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
        }
    if profile == "preview-native-arm64-fex":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-jit": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
            "com.apple.security.custom-x18-abi-toggle": True,
        }
    raise ValueError("unsupported entitlement snapshot profile: " + profile)


def open_directory(path):
    if not os.path.isabs(path) or os.path.normpath(path) != path or path == os.path.sep:
        raise ValueError("entitlement snapshot root is not a bounded canonical absolute path")
    descriptor = os.open(os.path.sep, DIRECTORY_FLAGS)
    try:
        for component in path.split(os.path.sep):
            if not component:
                continue
            following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = following
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


root_fd = -1
descriptor = -1
try:
    profile, root_name, snapshot_name, device, inode = sys.argv[1:]
    if not snapshot_name.startswith(".switchyard-entitlements.") or os.sep in snapshot_name:
        raise ValueError("entitlement snapshot has an invalid private name")
    expected_id = int(device), int(inode)
    root_fd = open_directory(root_name)
    try:
        entry = os.stat(snapshot_name, dir_fd=root_fd, follow_symlinks=False)
    except FileNotFoundError:
        raise SystemExit(0)
    if (entry.st_dev, entry.st_ino) != expected_id:
        raise ValueError("entitlement snapshot path no longer names the owned inode")
    if (
        not stat.S_ISREG(entry.st_mode)
        or entry.st_uid != os.geteuid()
        or stat.S_IMODE(entry.st_mode) != 0o400
        or entry.st_nlink != 1
        or entry.st_size <= 0
        or entry.st_size > MAX_ENTITLEMENTS_SIZE
    ):
        raise ValueError("owned entitlement snapshot path is unsafe")
    descriptor = os.open(snapshot_name, FILE_FLAGS, dir_fd=root_fd)
    opened = os.fstat(descriptor)
    if (opened.st_dev, opened.st_ino) != expected_id:
        raise ValueError("entitlement snapshot changed while opening it for cleanup")
    data = os.read(descriptor, opened.st_size + 1)
    if len(data) != opened.st_size:
        raise ValueError("entitlement snapshot has an inconsistent cleanup size")
    expected = expected_entitlements(profile)
    parsed = plistlib.loads(data)
    if (
        type(parsed) is not dict
        or set(parsed) != set(expected)
        or any(type(parsed[key]) is not bool or parsed[key] is not expected[key] for key in expected)
    ):
        raise ValueError("entitlement snapshot cleanup content is not allowlisted")
    current = os.stat(snapshot_name, dir_fd=root_fd, follow_symlinks=False)
    if (current.st_dev, current.st_ino) != expected_id:
        raise ValueError("entitlement snapshot changed before cleanup")
    os.unlink(snapshot_name, dir_fd=root_fd)
    os.fsync(root_fd)
except (OSError, plistlib.InvalidFileException, ValueError) as error:
    print(f"cannot clean up owned entitlement snapshot: {error}", file=sys.stderr)
    raise SystemExit(1)
finally:
    if descriptor >= 0:
        os.close(descriptor)
    if root_fd >= 0:
        os.close(root_fd)
PY
}

create_validated_entitlements_snapshot() {
  local runtime_profile source_entitlements private_root output_variable
  local snapshot_result snapshot_name snapshot_device snapshot_inode snapshot_path

  [ "$#" -eq 4 ] || {
    echo "usage: create_validated_entitlements_snapshot PROFILE SOURCE PRIVATE_ROOT OUTPUT_VARIABLE" >&2
    return 2
  }
  runtime_profile="$1"
  source_entitlements="$2"
  private_root="$3"
  output_variable="$4"
  [[ "$output_variable" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || {
    echo "Validated entitlement snapshot output variable is invalid." >&2
    return 2
  }
  printf -v "$output_variable" '%s' '' || {
    echo "Validated entitlement snapshot output variable is not writable." >&2
    return 2
  }
  [ "$runtime_profile" = stable-x86_64-rosetta ] ||
    [ "$runtime_profile" = preview-native-arm64-fex ] || {
    echo "Unsupported entitlement snapshot profile: $runtime_profile" >&2
    return 2
  }

  if ! snapshot_result="$(/usr/bin/python3 -I - \
      "$runtime_profile" "$source_entitlements" "$private_root" <<'PY'
import os
import plistlib
import secrets
import stat
import sys

DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
MAX_ENTITLEMENTS_SIZE = 65536


class SnapshotError(Exception):
    pass


def expected_entitlements(profile):
    if profile == "stable-x86_64-rosetta":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
        }
    if profile == "preview-native-arm64-fex":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-jit": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
            "com.apple.security.custom-x18-abi-toggle": True,
        }
    raise SnapshotError("unsupported entitlement snapshot profile: " + profile)


def file_state(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        stat.S_IMODE(value.st_mode),
        value.st_uid,
        value.st_gid,
        value.st_nlink,
    )


def open_directory(path, description):
    if not os.path.isabs(path) or os.path.normpath(path) != path or path == os.path.sep:
        raise SnapshotError(description + " is not a bounded canonical absolute path")
    descriptor = os.open(os.path.sep, DIRECTORY_FLAGS)
    try:
        for component in path.split(os.path.sep):
            if not component:
                continue
            following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = following
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def read_source(path):
    if not os.path.isabs(path) or os.path.normpath(path) != path:
        raise SnapshotError("entitlement source is not a canonical absolute path")
    directory, filename = os.path.split(path)
    if not filename or filename in (".", ".."):
        raise SnapshotError("entitlement source has an invalid filename")
    directory_fd = open_directory(directory, "entitlement source directory")
    descriptor = -1
    try:
        descriptor = os.open(filename, FILE_FLAGS, dir_fd=directory_fd)
        original = os.fstat(descriptor)
        entry = os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
        if file_state(entry) != file_state(original):
            raise SnapshotError("entitlement source changed while opening it")
        if (
            not stat.S_ISREG(original.st_mode)
            or original.st_uid != os.geteuid()
            or original.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
            or original.st_size <= 0
            or original.st_size > MAX_ENTITLEMENTS_SIZE
        ):
            raise SnapshotError("entitlement source has an unsafe type, owner, mode, or size")
        chunks = []
        remaining = MAX_ENTITLEMENTS_SIZE + 1
        while remaining:
            chunk = os.read(descriptor, min(16384, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
        if not data or len(data) > MAX_ENTITLEMENTS_SIZE:
            raise SnapshotError("entitlement source has an invalid size")
        if file_state(os.fstat(descriptor)) != file_state(original):
            raise SnapshotError("entitlement source changed while reading it")
        current = os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
        if file_state(current) != file_state(original):
            raise SnapshotError("entitlement source changed after reading it")
        return data
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        os.close(directory_fd)


def write_snapshot(root_name, data):
    root_fd = open_directory(root_name, "entitlement snapshot root")
    snapshot_name = None
    snapshot_fd = -1
    try:
        root_state = os.fstat(root_fd)
        if (
            not stat.S_ISDIR(root_state.st_mode)
            or root_state.st_uid != os.geteuid()
            or root_state.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        ):
            raise SnapshotError("entitlement snapshot root has an unsafe owner or mode")
        for unused in range(128):
            snapshot_name = ".switchyard-entitlements." + secrets.token_hex(16)
            try:
                snapshot_fd = os.open(
                    snapshot_name,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                    0o600,
                    dir_fd=root_fd,
                )
                break
            except FileExistsError:
                continue
        else:
            raise SnapshotError("cannot allocate a private entitlement snapshot")
        offset = 0
        while offset < len(data):
            written = os.write(snapshot_fd, data[offset:])
            if written <= 0:
                raise SnapshotError("cannot make progress writing entitlement snapshot")
            offset += written
        os.fchmod(snapshot_fd, 0o400)
        os.fsync(snapshot_fd)
        snapshot = os.fstat(snapshot_fd)
        if (
            not stat.S_ISREG(snapshot.st_mode)
            or snapshot.st_uid != os.geteuid()
            or stat.S_IMODE(snapshot.st_mode) != 0o400
            or snapshot.st_nlink != 1
            or snapshot.st_size != len(data)
        ):
            raise SnapshotError("private entitlement snapshot is unsafe")
        os.close(snapshot_fd)
        snapshot_fd = -1
        os.fsync(root_fd)
        return snapshot_name, snapshot.st_dev, snapshot.st_ino
    except BaseException:
        if snapshot_fd >= 0:
            os.close(snapshot_fd)
        if snapshot_name is not None:
            try:
                os.unlink(snapshot_name, dir_fd=root_fd)
            except OSError:
                pass
        raise
    finally:
        os.close(root_fd)


try:
    profile, source_name, root_name = sys.argv[1:]
    source = read_source(source_name)
    expected = expected_entitlements(profile)
    parsed = plistlib.loads(source)
    if (
        type(parsed) is not dict
        or set(parsed) != set(expected)
        or any(type(parsed[key]) is not bool or parsed[key] is not expected[key] for key in expected)
    ):
        raise SnapshotError("source entitlements do not match the exact profile allowlist")
    canonical = plistlib.dumps(expected, fmt=plistlib.FMT_XML, sort_keys=True)
    print(*write_snapshot(root_name, canonical), sep="\t")
except (OSError, plistlib.InvalidFileException, SnapshotError) as error:
    print(f"cannot create validated entitlement snapshot: {error}", file=sys.stderr)
    raise SystemExit(1)
PY
  )"; then
    return 1
  fi

  IFS=$'\t' read -r snapshot_name snapshot_device snapshot_inode <<<"$snapshot_result"
  [[ "$snapshot_name" =~ ^[.]switchyard-entitlements[.][0-9a-f]{32}$ ]] &&
    [[ "$snapshot_device" =~ ^[0-9]+$ ]] &&
    [[ "$snapshot_inode" =~ ^[0-9]+$ ]] || {
    echo "Validated entitlement snapshot identity is invalid." >&2
    return 1
  }
  snapshot_path="$private_root/$snapshot_name"

  if [ -e "/dev/fd/$SWITCHYARD_ENTITLEMENTS_SNAPSHOT_FD" ]; then
    echo "Validated entitlement snapshot descriptor is already in use." >&2
    remove_owned_entitlements_snapshot_path \
      "$runtime_profile" "$private_root" "$snapshot_name" \
      "$snapshot_device" "$snapshot_inode" || return 1
    return 1
  fi
  if ! exec 19<"$snapshot_path"; then
    echo "Cannot open private entitlement snapshot." >&2
    remove_owned_entitlements_snapshot_path \
      "$runtime_profile" "$private_root" "$snapshot_name" \
      "$snapshot_device" "$snapshot_inode" || return 1
    return 1
  fi

  if ! /usr/bin/python3 -I - \
      "$runtime_profile" "$private_root" "$snapshot_name" \
      "$SWITCHYARD_ENTITLEMENTS_SNAPSHOT_FD" "$snapshot_device" "$snapshot_inode" <<'PY'
import os
import plistlib
import stat
import sys

DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
MAX_ENTITLEMENTS_SIZE = 65536


def expected_entitlements(profile):
    if profile == "stable-x86_64-rosetta":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
        }
    if profile == "preview-native-arm64-fex":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-jit": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
            "com.apple.security.custom-x18-abi-toggle": True,
        }
    raise ValueError("unsupported entitlement snapshot profile: " + profile)


def snapshot_state(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        stat.S_IMODE(value.st_mode),
        value.st_uid,
        value.st_gid,
        value.st_nlink,
    )


def open_directory(path):
    descriptor = os.open(os.path.sep, DIRECTORY_FLAGS)
    try:
        for component in path.split(os.path.sep):
            if not component:
                continue
            following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = following
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


root_fd = -1
try:
    profile, root_name, snapshot_name, descriptor_text, device, inode = sys.argv[1:]
    if not os.path.isabs(root_name) or os.path.normpath(root_name) != root_name:
        raise ValueError("entitlement snapshot root is not canonical")
    if not snapshot_name.startswith(".switchyard-entitlements.") or os.sep in snapshot_name:
        raise ValueError("entitlement snapshot has an invalid private name")
    descriptor = int(descriptor_text)
    expected_id = int(device), int(inode)
    root_fd = open_directory(root_name)
    opened = os.fstat(descriptor)
    entry = os.stat(snapshot_name, dir_fd=root_fd, follow_symlinks=False)
    if (opened.st_dev, opened.st_ino) != expected_id or (entry.st_dev, entry.st_ino) != expected_id:
        raise ValueError("entitlement snapshot path or descriptor changed")
    if snapshot_state(opened) != snapshot_state(entry):
        raise ValueError("entitlement snapshot changed before detaching it")
    if (
        not stat.S_ISREG(opened.st_mode)
        or opened.st_uid != os.geteuid()
        or stat.S_IMODE(opened.st_mode) != 0o400
        or opened.st_nlink != 1
        or opened.st_size <= 0
        or opened.st_size > MAX_ENTITLEMENTS_SIZE
    ):
        raise ValueError("entitlement snapshot descriptor is unsafe")
    data = os.read(descriptor, opened.st_size + 1)
    expected = expected_entitlements(profile)
    parsed = plistlib.loads(data)
    if (
        len(data) != opened.st_size
        or type(parsed) is not dict
        or set(parsed) != set(expected)
        or any(type(parsed[key]) is not bool or parsed[key] is not expected[key] for key in expected)
    ):
        raise ValueError("entitlement snapshot content is not allowlisted")
    if snapshot_state(os.fstat(descriptor)) != snapshot_state(opened):
        raise ValueError("entitlement snapshot changed while detaching it")
    os.unlink(snapshot_name, dir_fd=root_fd)
    detached = os.fstat(descriptor)
    detached_state = snapshot_state(detached)
    opened_state = snapshot_state(opened)
    if (
        detached_state[:4] != opened_state[:4]
        or detached_state[5:-1] != opened_state[5:-1]
        or detached.st_nlink != 0
    ):
        raise ValueError("entitlement snapshot changed while unlinking it")
    os.fsync(root_fd)
    os.lseek(descriptor, 0, os.SEEK_SET)
except (OSError, plistlib.InvalidFileException, ValueError) as error:
    print(f"cannot detach validated entitlement snapshot: {error}", file=sys.stderr)
    raise SystemExit(1)
finally:
    if root_fd >= 0:
        os.close(root_fd)
PY
  then
    exec 19<&-
    remove_owned_entitlements_snapshot_path \
      "$runtime_profile" "$private_root" "$snapshot_name" \
      "$snapshot_device" "$snapshot_inode" || return 1
    return 1
  fi

  if ! validate_entitlements_snapshot \
      "$runtime_profile" "$SWITCHYARD_ENTITLEMENTS_SNAPSHOT_FD"; then
    exec 19<&-
    return 1
  fi
  printf -v "$output_variable" '%s' "$SWITCHYARD_ENTITLEMENTS_SNAPSHOT_FD"
}

validate_entitlements_snapshot() {
  local runtime_profile snapshot_fd

  [ "$#" -eq 2 ] || {
    echo "usage: validate_entitlements_snapshot PROFILE SNAPSHOT_FD" >&2
    return 2
  }
  runtime_profile="$1"
  snapshot_fd="$2"
  [[ "$snapshot_fd" =~ ^[0-9]+$ ]] || {
    echo "Validated entitlement snapshot descriptor is invalid." >&2
    return 2
  }

  /usr/bin/python3 -I - "$runtime_profile" "$snapshot_fd" <<'PY'
import os
import plistlib
import stat
import sys

MAX_ENTITLEMENTS_SIZE = 65536


def expected_entitlements(profile):
    if profile == "stable-x86_64-rosetta":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
        }
    if profile == "preview-native-arm64-fex":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-jit": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
            "com.apple.security.custom-x18-abi-toggle": True,
        }
    raise ValueError("unsupported entitlement snapshot profile: " + profile)


def state(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        stat.S_IMODE(value.st_mode),
        value.st_uid,
        value.st_gid,
        value.st_nlink,
    )


try:
    profile, descriptor_text = sys.argv[1:]
    descriptor = int(descriptor_text)
    original = os.fstat(descriptor)
    if (
        not stat.S_ISREG(original.st_mode)
        or original.st_uid != os.geteuid()
        or original.st_mode & 0o222
        or original.st_nlink != 0
        or original.st_size <= 0
        or original.st_size > MAX_ENTITLEMENTS_SIZE
    ):
        raise ValueError("entitlement snapshot descriptor is unsafe")
    os.lseek(descriptor, 0, os.SEEK_SET)
    data = os.read(descriptor, original.st_size + 1)
    if len(data) != original.st_size:
        raise ValueError("entitlement snapshot has an inconsistent size")
    if state(os.fstat(descriptor)) != state(original):
        raise ValueError("entitlement snapshot changed while validating it")
    parsed = plistlib.loads(data)
    expected = expected_entitlements(profile)
    if (
        type(parsed) is not dict
        or set(parsed) != set(expected)
        or any(type(parsed[key]) is not bool or parsed[key] is not expected[key] for key in expected)
    ):
        raise ValueError("entitlement snapshot does not match the exact profile allowlist")
    os.lseek(descriptor, 0, os.SEEK_SET)
except (OSError, plistlib.InvalidFileException, ValueError) as error:
    print(f"cannot validate entitlement snapshot: {error}", file=sys.stderr)
    raise SystemExit(1)
PY
}

close_validated_entitlements_snapshot() {
  local snapshot_fd

  [ "$#" -eq 1 ] || {
    echo "usage: close_validated_entitlements_snapshot SNAPSHOT_FD" >&2
    return 2
  }
  snapshot_fd="$1"
  [[ "$snapshot_fd" =~ ^[0-9]+$ ]] || {
    echo "Validated entitlement snapshot descriptor is invalid." >&2
    return 2
  }
  [ "$snapshot_fd" = "$SWITCHYARD_ENTITLEMENTS_SNAPSHOT_FD" ] || {
    echo "Validated entitlement snapshot descriptor is not the reserved descriptor." >&2
    return 2
  }
  [ -e "/dev/fd/$snapshot_fd" ] || {
    echo "Validated entitlement snapshot descriptor is already closed." >&2
    return 1
  }
  exec 19<&-
}

verify_macho_entitlements_snapshot() {
  local codesign_tool runtime_profile item snapshot_fd

  [ "$#" -eq 4 ] || {
    echo "usage: verify_macho_entitlements_snapshot CODESIGN PROFILE ITEM SNAPSHOT_FD" >&2
    return 2
  }
  codesign_tool="$1"
  runtime_profile="$2"
  item="$3"
  snapshot_fd="$4"
  [ -x "$codesign_tool" ] && [ ! -L "$codesign_tool" ] || {
    echo "Mach-O signing tool is missing or unsafe: $codesign_tool" >&2
    return 1
  }
  [[ "$snapshot_fd" =~ ^[0-9]+$ ]] || {
    echo "Validated entitlement snapshot descriptor is invalid." >&2
    return 2
  }

  /usr/bin/python3 -I - \
    "$codesign_tool" "$runtime_profile" "$item" "$snapshot_fd" <<'PY'
import os
import plistlib
import stat
import subprocess
import sys

FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
MAX_ENTITLEMENTS_SIZE = 65536


def expected_entitlements(profile):
    if profile == "stable-x86_64-rosetta":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
        }
    if profile == "preview-native-arm64-fex":
        return {
            "com.apple.security.cs.allow-dyld-environment-variables": True,
            "com.apple.security.cs.allow-jit": True,
            "com.apple.security.cs.allow-unsigned-executable-memory": True,
            "com.apple.security.custom-x18-abi-toggle": True,
        }
    raise ValueError("unsupported entitlement snapshot profile: " + profile)


def state(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        stat.S_IMODE(value.st_mode),
        value.st_uid,
        value.st_gid,
        value.st_nlink,
    )


descriptor = -1
try:
    codesign, profile, item_name, snapshot_text = sys.argv[1:]
    if not os.path.isabs(item_name) or os.path.normpath(item_name) != item_name:
        raise ValueError("entitled Mach-O path is not canonical and absolute")
    entry = os.lstat(item_name)
    if not stat.S_ISREG(entry.st_mode):
        raise ValueError("entitled Mach-O target is not a regular file")
    descriptor = os.open(item_name, FILE_FLAGS)
    opened = os.fstat(descriptor)
    if state(entry) != state(opened):
        raise ValueError("entitled Mach-O target changed while opening it")
    snapshot_fd = int(snapshot_text)
    snapshot = os.fstat(snapshot_fd)
    if (
        not stat.S_ISREG(snapshot.st_mode)
        or snapshot.st_uid != os.geteuid()
        or snapshot.st_mode & 0o222
        or snapshot.st_nlink != 0
        or snapshot.st_size <= 0
        or snapshot.st_size > MAX_ENTITLEMENTS_SIZE
    ):
        raise ValueError("entitlement snapshot descriptor is unsafe")
    os.lseek(snapshot_fd, 0, os.SEEK_SET)
    snapshot_data = os.read(snapshot_fd, snapshot.st_size + 1)
    if len(snapshot_data) != snapshot.st_size or state(os.fstat(snapshot_fd)) != state(snapshot):
        raise ValueError("entitlement snapshot changed while reading it")
    expected = plistlib.loads(snapshot_data)
    allowlist = expected_entitlements(profile)
    if (
        type(expected) is not dict
        or set(expected) != set(allowlist)
        or any(
            type(expected[key]) is not bool or expected[key] is not allowlist[key]
            for key in allowlist
        )
    ):
        raise ValueError("entitlement snapshot does not match the exact profile allowlist")
    strict = subprocess.run(
        [codesign, "--verify", "--strict", "--verbose=2", item_name],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if strict.returncode:
        sys.stderr.buffer.write(strict.stderr)
        raise ValueError("Mach-O target failed strict signature verification")
    result = subprocess.run(
        [codesign, "-d", "--xml", "--entitlements", "-", item_name],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        sys.stderr.buffer.write(result.stderr)
        raise ValueError("cannot inspect embedded Mach-O entitlements")
    if len(result.stdout) > MAX_ENTITLEMENTS_SIZE:
        raise ValueError("embedded Mach-O entitlements exceed their size bound")
    embedded = plistlib.loads(result.stdout)
    if (
        type(embedded) is not dict
        or set(embedded) != set(expected)
        or any(type(embedded[key]) is not bool or embedded[key] is not expected[key] for key in expected)
    ):
        raise ValueError("embedded Mach-O entitlements do not match the validated snapshot")
    current = os.lstat(item_name)
    if state(current) != state(opened) or state(os.fstat(descriptor)) != state(opened):
        raise ValueError("entitled Mach-O target changed during verification")
    if state(os.fstat(snapshot_fd)) != state(snapshot):
        raise ValueError("entitlement snapshot changed during embedded verification")
    os.lseek(snapshot_fd, 0, os.SEEK_SET)
except (OSError, plistlib.InvalidFileException, ValueError) as error:
    print(f"Mach-O entitlement verification failed: {error}", file=sys.stderr)
    raise SystemExit(1)
finally:
    if descriptor >= 0:
        os.close(descriptor)
PY
}

switchyard_sign_macho_atomically_internal() {
  local codesign_tool directory_fd item

  [ "$#" -ge 4 ] || {
    echo "usage: switchyard_sign_macho_atomically_internal DIRECTORY_FD CODESIGN ITEM CODESIGN_ARGUMENT..." >&2
    return 2
  }
  directory_fd="$1"
  codesign_tool="$2"
  item="$3"
  shift 3
  [ -x "$codesign_tool" ] && [ ! -L "$codesign_tool" ] || {
    echo "Mach-O signing tool is missing or unsafe: $codesign_tool" >&2
    return 1
  }

  /usr/bin/env \
    -u BASH_ENV -u ENV -u CODESIGN_ALLOCATE -u DEVELOPER_DIR \
    -u DYLD_INSERT_LIBRARIES -u DYLD_LIBRARY_PATH \
    -u DYLD_FALLBACK_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH \
    -u DYLD_FALLBACK_FRAMEWORK_PATH -u PYTHONHOME -u PYTHONPATH \
    /usr/bin/python3 -I - \
    "$directory_fd" "$codesign_tool" "$item" "$@" <<'PY'
import ctypes
import hashlib
import os
import plistlib
import re
import secrets
import signal
import stat
import subprocess
import sys

COPYFILE_ALL = 0x0F
RENAME_SWAP = 0x00000002
DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
MAX_CAPTURE = 1024 * 1024
MAX_ENTITLEMENTS_SIZE = 65536


class PublicationError(Exception):
    pass


def file_id(value):
    return value.st_dev, value.st_ino


def file_state(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        stat.S_IMODE(value.st_mode),
        value.st_uid,
        value.st_gid,
        getattr(value, "st_flags", 0),
    )


def snapshot_state(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        stat.S_IMODE(value.st_mode),
        value.st_uid,
        value.st_gid,
        value.st_nlink,
    )


def preserved_metadata(value):
    return (
        stat.S_IMODE(value.st_mode),
        value.st_uid,
        value.st_gid,
        getattr(value, "st_flags", 0),
    )


def open_directory(path):
    if (
        not os.path.isabs(path)
        or os.path.normpath(path) != path
        or path == os.path.sep
        or "\n" in path
        or "\r" in path
    ):
        raise PublicationError("Mach-O parent is not a bounded canonical absolute path")
    descriptor = os.open(os.path.sep, DIRECTORY_FLAGS)
    try:
        for component in path.split(os.path.sep):
            if not component:
                continue
            following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = following
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def open_directory_at(root_fd, path):
    descriptor = os.dup(root_fd)
    try:
        for component in path.split(os.path.sep):
            if not component or component == ".":
                continue
            if component == "..":
                raise PublicationError("Mach-O parent escapes its pinned directory")
            following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = following
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def require_directory_path(path, expected_id, root_fd=None):
    try:
        descriptor = (
            open_directory(path)
            if root_fd is None
            else open_directory_at(root_fd, path)
        )
    except OSError as error:
        raise PublicationError(f"Mach-O signing parent changed or contains a symlink: {error}") from error
    try:
        if file_id(os.fstat(descriptor)) != expected_id:
            raise PublicationError("Mach-O signing parent path changed")
    finally:
        os.close(descriptor)


def regular_entry(directory_fd, filename, description):
    try:
        value = os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as error:
        raise PublicationError(f"cannot identify {description}: {error}") from error
    if not stat.S_ISREG(value.st_mode):
        raise PublicationError(description + " is not a regular file")
    return value


def any_entry(directory_fd, filename, description):
    try:
        return os.stat(filename, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as error:
        raise PublicationError(f"cannot identify {description}: {error}") from error


def create_staging_directory(parent_fd, cleanup_on_error=True):
    for unused in range(128):
        name = ".switchyard-codesign." + secrets.token_hex(8)
        try:
            os.mkdir(name, 0o700, dir_fd=parent_fd)
            break
        except FileExistsError:
            continue
    else:
        raise PublicationError("cannot allocate a private Mach-O signing staging directory")
    try:
        descriptor = os.open(name, DIRECTORY_FLAGS, dir_fd=parent_fd)
        os.fchmod(descriptor, 0o700)
        value = os.fstat(descriptor)
        if (
            not stat.S_ISDIR(value.st_mode)
            or value.st_uid != os.geteuid()
            or stat.S_IMODE(value.st_mode) != 0o700
        ):
            raise PublicationError("private Mach-O signing staging directory is unsafe")
        return name, descriptor
    except BaseException:
        if cleanup_on_error:
            try:
                os.rmdir(name, dir_fd=parent_fd)
            except OSError:
                pass
        raise


def copy_file(source_fd, staging_fd, filename, source, libc):
    destination_fd = os.open(
        filename,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
        0o600,
        dir_fd=staging_fd,
    )
    try:
        if libc.fcopyfile(source_fd, destination_fd, None, COPYFILE_ALL) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        copied = os.fstat(destination_fd)
        if preserved_metadata(copied) != preserved_metadata(source) or copied.st_size != source.st_size:
            raise PublicationError("private Mach-O copy did not preserve source metadata")
        return copied
    finally:
        os.close(destination_fd)


def run_in_directory(directory_fd, arguments, pass_fds=()):
    saved_fd = os.open(".", DIRECTORY_FLAGS)
    process = None
    try:
        os.fchdir(directory_fd)
        process = subprocess.Popen(
            arguments,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            pass_fds=pass_fds,
            env={
                "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                "LC_ALL": "C",
                "LANG": "C",
                "TMPDIR": "/private/tmp",
                **{
                    name: value for name, value in os.environ.items()
                    if name.startswith("SWITCHYARD_FAKE_")
                    or name.startswith("SWITCHYARD_TEST_")
                },
            },
        )
    finally:
        os.fchdir(saved_fd)
        os.close(saved_fd)
    try:
        stdout, stderr = process.communicate()
    except BaseException:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        raise
    if len(stdout) > MAX_CAPTURE or len(stderr) > MAX_CAPTURE:
        raise PublicationError("Mach-O signing tool output exceeds its bound")
    return process.returncode, stdout, stderr


def rename_swap(libc, source_fd, source_name, destination_fd, destination_name):
    if libc.renameatx_np(
        source_fd,
        os.fsencode(source_name),
        destination_fd,
        os.fsencode(destination_name),
        RENAME_SWAP,
    ) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


def rollback_swap(libc, staging_fd, parent_fd, filename, signed_id, target_id):
    rename_swap(libc, staging_fd, filename, parent_fd, filename)
    target = any_entry(parent_fd, filename, "restored Mach-O target")
    staging = regular_entry(staging_fd, filename, "restored signed staging copy")
    if file_id(target) != target_id or file_id(staging) != signed_id:
        raise PublicationError("atomic Mach-O rollback restored unexpected inodes")


def pin_entitlements(arguments):
    arguments = list(arguments)
    indexes = []
    for index, argument in enumerate(arguments):
        if argument.startswith("--entitlements="):
            raise PublicationError("Mach-O entitlements must use a separate descriptor argument")
        if argument == "--entitlements":
            if index + 1 >= len(arguments):
                raise PublicationError("Mach-O entitlements argument has no value")
            indexes.append(index + 1)
    if not indexes:
        return arguments, (), (), None
    if len(indexes) != 1:
        raise PublicationError("Mach-O signing accepts exactly one entitlement snapshot")
    index = indexes[0]
    value = arguments[index]
    if not value.startswith("/dev/fd/") or not value[8:].isdigit():
        raise PublicationError("Mach-O entitlements must use a validated snapshot descriptor")
    source_fd = int(value[8:])
    try:
        source = os.fstat(source_fd)
    except OSError as error:
        raise PublicationError(f"cannot inspect entitlement snapshot: {error}") from error
    if (
        not stat.S_ISREG(source.st_mode)
        or source.st_uid != os.geteuid()
        or source.st_mode & 0o222
        or source.st_nlink != 0
        or source.st_size <= 0
        or source.st_size > MAX_ENTITLEMENTS_SIZE
    ):
        raise PublicationError("Mach-O entitlement snapshot is unsafe")
    os.lseek(source_fd, 0, os.SEEK_SET)
    pinned_fd = os.dup(source_fd)
    arguments[index] = f"/dev/fd/{pinned_fd}"
    return arguments, (pinned_fd,), ((pinned_fd, snapshot_state(source)),), pinned_fd


def signing_mode(arguments):
    if arguments == ["--force", "--sign", "-"]:
        return "dependency"
    if arguments == [
        "--force",
        "--sign",
        "-",
        "--entitlements",
        arguments[-1] if arguments else "",
    ] and arguments[-1].startswith("/dev/fd/"):
        return "engineering"
    if arguments.count("--sign") != 1:
        raise PublicationError("release signing must identify exactly one signing identity")
    identity_index = arguments.index("--sign") + 1
    if (
        identity_index >= len(arguments)
        or not arguments[identity_index]
        or arguments[identity_index].startswith("-")
        or "\n" in arguments[identity_index]
        or "\r" in arguments[identity_index]
    ):
        raise PublicationError("ad-hoc Mach-O signing arguments are not the exact engineering policy")
    if "--force" not in arguments or "--timestamp" not in arguments:
        raise PublicationError("release signing is missing force or timestamp policy")
    if not any(
        arguments[index:index + 2] == ["--options", "runtime"]
        for index in range(len(arguments) - 1)
    ):
        raise PublicationError("release signing is missing Hardened Runtime policy")
    return "release"


def read_snapshot(descriptor, expected_state):
    current = os.fstat(descriptor)
    if snapshot_state(current) != expected_state:
        raise PublicationError("Mach-O entitlement snapshot changed")
    os.lseek(descriptor, 0, os.SEEK_SET)
    data = os.read(descriptor, current.st_size + 1)
    if len(data) != current.st_size or snapshot_state(os.fstat(descriptor)) != expected_state:
        raise PublicationError("Mach-O entitlement snapshot changed while reading")
    try:
        value = plistlib.loads(data)
    except plistlib.InvalidFileException as error:
        raise PublicationError("Mach-O entitlement snapshot is malformed") from error
    if type(value) is not dict or not value or any(type(item) is not bool for item in value.values()):
        raise PublicationError("Mach-O entitlement snapshot is not a boolean allowlist")
    os.lseek(descriptor, 0, os.SEEK_SET)
    return value


def sha256_file(descriptor, expected):
    digest = hashlib.sha256()
    offset = 0
    while offset < expected.st_size:
        block = os.pread(descriptor, min(1024 * 1024, expected.st_size - offset), offset)
        if not block:
            raise PublicationError("Mach-O source ended while hashing")
        digest.update(block)
        offset += len(block)
    if os.pread(descriptor, 1, offset) or file_state(os.fstat(descriptor)) != file_state(expected):
        raise PublicationError("Mach-O source changed while hashing")
    return digest.hexdigest()


def verify_staging_signature(
    codesign, staging_fd, target_argument, mode, snapshot_fd, snapshot_fd_states
):
    status, stdout, stderr = run_in_directory(
        staging_fd, [codesign, "--verify", "--strict", "--verbose=2", target_argument]
    )
    if status:
        if stdout:
            sys.stderr.buffer.write(stdout)
        if stderr:
            sys.stderr.buffer.write(stderr)
        raise PublicationError("private signed Mach-O failed strict verification")
    if snapshot_fd is not None:
        expected_state = dict(snapshot_fd_states)[snapshot_fd]
        expected = read_snapshot(snapshot_fd, expected_state)
        status, stdout, stderr = run_in_directory(
            staging_fd,
            [codesign, "-d", "--xml", "--entitlements", "-", target_argument],
        )
        if status:
            if stderr:
                sys.stderr.buffer.write(stderr)
            raise PublicationError("cannot inspect private signed Mach-O entitlements")
        if len(stdout) > MAX_ENTITLEMENTS_SIZE:
            raise PublicationError("embedded Mach-O entitlements exceed their size bound")
        try:
            embedded = plistlib.loads(stdout)
        except plistlib.InvalidFileException as error:
            raise PublicationError("embedded Mach-O entitlements are malformed") from error
        if (
            type(embedded) is not dict
            or set(embedded) != set(expected)
            or any(
                type(embedded[key]) is not bool or embedded[key] is not expected[key]
                for key in expected
            )
        ):
            raise PublicationError("embedded Mach-O entitlements do not match the pinned snapshot")
    if mode in ("dependency", "engineering"):
        status, stdout, stderr = run_in_directory(
            staging_fd, [codesign, "-d", "--verbose=4", target_argument]
        )
        if status:
            raise PublicationError("cannot inspect private ad-hoc signature details")
        details = (stdout + stderr).decode("utf-8", "strict")
        lines = details.splitlines()
        if lines.count("Signature=adhoc") != 1:
            raise PublicationError("private Mach-O is not ad-hoc signed")
        if len(re.findall(r"\bflags=0x2\(adhoc\)(?:\s|$)", details)) != 1:
            raise PublicationError("private Mach-O does not have exact CodeDirectory flags 0x2")
        if "Runtime Version=" in details or re.search(
            r"\bflags=.*(?:\(|,)runtime(?:,|\)|\s|$)", details
        ):
            raise PublicationError("private ad-hoc Mach-O unexpectedly enables Hardened Runtime")
    elif mode == "release":
        status, stdout, stderr = run_in_directory(
            staging_fd, [codesign, "-d", "--verbose=4", target_argument]
        )
        if status:
            raise PublicationError("cannot inspect private release signature details")
        details = (stdout + stderr).decode("utf-8", "strict")
        if "Signature=adhoc" in details:
            raise PublicationError("release Mach-O was signed ad-hoc")
        if "Runtime Version=" not in details:
            raise PublicationError("release Mach-O is missing Hardened Runtime")
        if not any(
            line.startswith("Authority=Developer ID Application:")
            for line in details.splitlines()
        ):
            raise PublicationError("release Mach-O is not signed by a Developer ID Application identity")


def interrupted(signum, unused_frame):
    raise PublicationError(f"Mach-O signing interrupted by signal {signum}")


def main():
    if len(sys.argv) < 5:
        raise PublicationError("atomic Mach-O signing arguments are incomplete")
    directory_fd_text = sys.argv[1]
    codesign = sys.argv[2]
    item = sys.argv[3]
    if not os.path.isabs(codesign) or os.path.normpath(codesign) != codesign:
        raise PublicationError("Mach-O signing tool path is not canonical and absolute")
    root_fd = None
    if directory_fd_text:
        if not directory_fd_text.isascii() or not directory_fd_text.isdecimal():
            raise PublicationError("Mach-O signing directory descriptor is malformed")
        root_fd = os.dup(int(directory_fd_text))
        root = os.fstat(root_fd)
        if (
            not stat.S_ISDIR(root.st_mode)
            or root.st_uid != os.geteuid()
            or stat.S_IMODE(root.st_mode) != 0o700
            or getattr(root, "st_flags", 0)
        ):
            raise PublicationError("Mach-O signing directory descriptor is unsafe")
        if (
            os.path.isabs(item)
            or os.path.normpath(item) != item
            or item in ("", ".", "..")
            or item.startswith("../")
            or "\n" in item
            or "\r" in item
        ):
            raise PublicationError("Mach-O target is not a bounded relative path")
    elif (
        not os.path.isabs(item)
        or os.path.normpath(item) != item
        or "\n" in item
        or "\r" in item
    ):
        raise PublicationError("Mach-O target path is not canonical and absolute")
    raw_arguments = sys.argv[4:]
    expected_source_sha256 = None
    if "--switchyard-expected-source-sha256" in raw_arguments:
        digest_index = raw_arguments.index("--switchyard-expected-source-sha256")
        if (
            raw_arguments.count("--switchyard-expected-source-sha256") != 1
            or digest_index + 1 >= len(raw_arguments)
            or len(raw_arguments[digest_index + 1]) != 64
            or any(
                character not in "0123456789abcdef"
                for character in raw_arguments[digest_index + 1]
            )
        ):
            raise PublicationError("expected Mach-O source digest is malformed")
        expected_source_sha256 = raw_arguments[digest_index + 1]
        raw_arguments = raw_arguments[:digest_index] + raw_arguments[digest_index + 2:]
    arguments, signing_fds, signing_fd_states, snapshot_fd = pin_entitlements(raw_arguments)
    mode = signing_mode(arguments)
    directory, filename = os.path.split(item)
    if not filename or filename in (".", ".."):
        raise PublicationError("Mach-O signing target has an invalid filename")

    libc = ctypes.CDLL(None, use_errno=True)
    libc.fcopyfile.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint]
    libc.fcopyfile.restype = ctypes.c_int
    libc.renameatx_np.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    libc.renameatx_np.restype = ctypes.c_int
    libc.acl_get_fd_np.argtypes = ctypes.c_int, ctypes.c_int
    libc.acl_get_fd_np.restype = ctypes.c_void_p
    libc.acl_free.argtypes = ctypes.c_void_p,
    libc.acl_free.restype = ctypes.c_int
    if root_fd is not None:
        ctypes.set_errno(0)
        root_acl = libc.acl_get_fd_np(root_fd, 0x00000100)
        if root_acl:
            libc.acl_free(root_acl)
            raise PublicationError("Mach-O signing directory has an extended ACL")
        if ctypes.get_errno() not in (0, 2):
            raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))

    parent_fd = (
        open_directory(directory)
        if root_fd is None
        else open_directory_at(root_fd, directory)
    )
    source_fd = -1
    signed_fd = -1
    staging_fd = -1
    staging_name = None
    staging_has_entry = False
    retain_staging = False
    operation_error = None
    cleanup_errors = []
    try:
        parent_id = file_id(os.fstat(parent_fd))
        source_fd = os.open(filename, FILE_FLAGS, dir_fd=parent_fd)
        source = os.fstat(source_fd)
        entry = regular_entry(parent_fd, filename, "Mach-O signing target")
        if file_state(entry) != file_state(source):
            raise PublicationError("Mach-O signing target changed while opening it")
        if source.st_uid != os.geteuid() or source.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise PublicationError("Mach-O signing target has an unsafe owner or mode")
        if expected_source_sha256 is not None and sha256_file(source_fd, source) != expected_source_sha256:
            raise PublicationError("Mach-O signing target differs from its inventory digest")

        staging_name, staging_fd = create_staging_directory(
            parent_fd if root_fd is None else root_fd,
            cleanup_on_error=root_fd is None,
        )
        copied = copy_file(source_fd, staging_fd, filename, source, libc)
        staging_has_entry = True
        if file_id(copied) == file_id(source):
            raise PublicationError("private Mach-O copy reused the published inode")
        if file_state(os.fstat(source_fd)) != file_state(source):
            raise PublicationError("Mach-O signing target changed while copying")
        if expected_source_sha256 is not None:
            copied_fd = os.open(filename, FILE_FLAGS, dir_fd=staging_fd)
            try:
                copied_state = os.fstat(copied_fd)
                if sha256_file(copied_fd, copied_state) != expected_source_sha256:
                    raise PublicationError("private Mach-O copy differs from its inventory digest")
            finally:
                os.close(copied_fd)

        target_argument = "./" + filename
        status, stdout, stderr = run_in_directory(
            staging_fd,
            [codesign, *arguments, target_argument],
            pass_fds=signing_fds,
        )
        if status:
            if stdout:
                sys.stderr.buffer.write(stdout)
            if stderr:
                sys.stderr.buffer.write(stderr)
            raise PublicationError("cannot sign private Mach-O staging copy")
        for descriptor, expected in signing_fd_states:
            if snapshot_state(os.fstat(descriptor)) != expected:
                raise PublicationError("Mach-O entitlement snapshot changed while signing")

        signed_entry = regular_entry(staging_fd, filename, "signed Mach-O staging copy")
        signed_fd = os.open(filename, FILE_FLAGS, dir_fd=staging_fd)
        signed = os.fstat(signed_fd)
        if file_state(signed_entry) != file_state(signed) or file_id(signed) == file_id(source):
            raise PublicationError("signed Mach-O staging copy has an unexpected identity")
        os.fsync(signed_fd)
        verify_staging_signature(
            codesign, staging_fd, target_argument, mode, snapshot_fd, signing_fd_states
        )
        verified = regular_entry(staging_fd, filename, "verified Mach-O staging copy")
        if file_state(verified) != file_state(signed):
            raise PublicationError("signed Mach-O staging copy changed during verification")
        if preserved_metadata(verified) != preserved_metadata(source):
            raise PublicationError("signed Mach-O staging copy changed source metadata")

        require_directory_path(directory, parent_id, root_fd)
        if file_state(os.fstat(source_fd)) != file_state(source):
            raise PublicationError("Mach-O signing target changed before publication")
        current = regular_entry(parent_fd, filename, "Mach-O target before publication")
        if file_state(current) != file_state(source):
            raise PublicationError("Mach-O signing target changed before publication")

        rename_swap(libc, staging_fd, filename, parent_fd, filename)
        staging_has_entry = False
        # From this point until publication is durably confirmed, the staging
        # entry can be the only remaining name for the original target.  Keep
        # the private directory on every ambiguous/error path; clear this only
        # after a verified rollback or a fully completed publication.
        retain_staging = True
        displaced = any_entry(staging_fd, filename, "displaced Mach-O target")
        published = any_entry(parent_fd, filename, "published Mach-O target")
        displaced_original = stat.S_ISREG(displaced.st_mode) and file_id(displaced) == file_id(source)
        published_signed = stat.S_ISREG(published.st_mode) and file_id(published) == file_id(signed)
        if not displaced_original and published_signed:
            if root_fd is not None:
                raise PublicationError(
                    "Mach-O signing target changed during fd-bound atomic publication"
                )
            rollback_swap(
                libc, staging_fd, parent_fd, filename, file_id(signed), file_id(displaced)
            )
            staging_has_entry = True
            retain_staging = False
            raise PublicationError("Mach-O signing target changed during atomic publication")
        if displaced_original and not published_signed:
            staging_has_entry = True
            raise PublicationError("published Mach-O target changed during atomic publication")
        if not displaced_original or not published_signed:
            retain_staging = True
            raise PublicationError("atomic Mach-O publication reached an ambiguous state")
        staging_has_entry = True

        try:
            require_directory_path(directory, parent_id, root_fd)
        except BaseException as path_error:
            if root_fd is not None:
                raise path_error
            staging_entry = any_entry(staging_fd, filename, "rollback staging entry")
            target_entry = any_entry(parent_fd, filename, "rollback target entry")
            if (
                stat.S_ISREG(staging_entry.st_mode)
                and file_id(staging_entry) == file_id(source)
                and stat.S_ISREG(target_entry.st_mode)
                and file_id(target_entry) == file_id(signed)
            ):
                rollback_swap(
                    libc, staging_fd, parent_fd, filename, file_id(signed), file_id(source)
                )
                staging_has_entry = True
                retain_staging = False
            else:
                retain_staging = True
            raise path_error

        final = regular_entry(parent_fd, filename, "atomically published Mach-O")
        if file_id(final) != file_id(signed) or file_id(final) == file_id(source):
            raise PublicationError("atomic Mach-O publication installed an unexpected inode")
        os.fsync(parent_fd)
        retain_staging = root_fd is not None
    except BaseException as error:
        operation_error = error
        if root_fd is not None and staging_name is not None:
            retain_staging = True
    finally:
        for descriptor in signing_fds:
            os.close(descriptor)
        if signed_fd >= 0:
            os.close(signed_fd)
        if staging_fd >= 0:
            if staging_has_entry and not retain_staging:
                try:
                    os.unlink(filename, dir_fd=staging_fd)
                except OSError as error:
                    cleanup_errors.append(f"cannot remove private Mach-O staging entry: {error}")
            os.close(staging_fd)
        if staging_name is not None and not retain_staging:
            try:
                staging_parent_fd = parent_fd if root_fd is None else root_fd
                os.rmdir(staging_name, dir_fd=staging_parent_fd)
                os.fsync(staging_parent_fd)
            except OSError as error:
                cleanup_errors.append(f"cannot remove private Mach-O staging directory: {error}")
        if source_fd >= 0:
            os.close(source_fd)
        os.close(parent_fd)
        if root_fd is not None:
            os.close(root_fd)

    if operation_error is not None:
        print(str(operation_error), file=sys.stderr)
    for cleanup_error in cleanup_errors:
        print(cleanup_error, file=sys.stderr)
    if operation_error is not None or cleanup_errors:
        raise SystemExit(1)


for handled_signal in (signal.SIGHUP, signal.SIGQUIT, signal.SIGTERM):
    signal.signal(handled_signal, interrupted)

try:
    main()
except PublicationError as error:
    print(str(error), file=sys.stderr)
    raise SystemExit(1)
except OSError as error:
    print(f"atomic Mach-O signing failed: {error}", file=sys.stderr)
    raise SystemExit(1)
PY
}

sign_macho_atomically() {
  [ "$#" -ge 3 ] || {
    echo "usage: sign_macho_atomically CODESIGN ITEM CODESIGN_ARGUMENT..." >&2
    return 2
  }
  switchyard_sign_macho_atomically_internal "" "$@"
}

sign_macho_at_fd_atomically() {
  local codesign_tool directory_fd item

  [ "$#" -ge 4 ] || {
    echo "usage: sign_macho_at_fd_atomically CODESIGN DIRECTORY_FD ITEM CODESIGN_ARGUMENT..." >&2
    return 2
  }
  codesign_tool="$1"
  directory_fd="$2"
  item="$3"
  shift 3
  case "$directory_fd" in ''|*[!0-9]*) return 2 ;; esac
  switchyard_sign_macho_atomically_internal \
    "$directory_fd" "$codesign_tool" "$item" "$@"
}

sign_engineering_macho_atomically() {
  local codesign_tool runtime_profile item snapshot_fd signing_details

  [ "$#" -eq 4 ] || {
    echo "usage: sign_engineering_macho_atomically CODESIGN PROFILE ITEM SNAPSHOT_FD" >&2
    return 2
  }
  codesign_tool="$1"
  runtime_profile="$2"
  item="$3"
  snapshot_fd="$4"
  [ "$runtime_profile" = preview-native-arm64-fex ] || {
    echo "Engineering Mach-O signing requires preview-native-arm64-fex." >&2
    return 2
  }
  validate_entitlements_snapshot "$runtime_profile" "$snapshot_fd" || return 1
  sign_macho_atomically "$codesign_tool" "$item" \
    --force --sign - --entitlements "/dev/fd/$snapshot_fd" || return 1
  verify_macho_entitlements_snapshot \
    "$codesign_tool" "$runtime_profile" "$item" "$snapshot_fd" || return 1
  signing_details="$("$codesign_tool" -d --verbose=4 "$item" 2>&1)" || {
    echo "Cannot inspect engineering Mach-O signature: $item" >&2
    return 1
  }
  [ "$(/usr/bin/printf '%s\n' "$signing_details" |
      /usr/bin/grep -Fxc 'Signature=adhoc')" -eq 1 ] || {
    echo "Engineering Mach-O is not ad-hoc signed: $item" >&2
    return 1
  }
  [ "$(/usr/bin/printf '%s\n' "$signing_details" |
      /usr/bin/grep -Ec 'flags=0x2\(adhoc\)([[:space:]]|$)')" -eq 1 ] || {
    echo "Engineering Mach-O does not have exact CodeDirectory flags 0x2: $item" >&2
    return 1
  }
  if /usr/bin/printf '%s\n' "$signing_details" |
      /usr/bin/grep -E '(^Runtime Version=|flags=.*[=(,]runtime([,)[:space:]]|$))' >/dev/null; then
    echo "Engineering Mach-O unexpectedly enables Hardened Runtime: $item" >&2
    return 1
  fi
}

sign_release_macho_atomically() {
  local codesign_tool item identity signing_details

  [ "$#" -ge 3 ] || {
    echo "usage: sign_release_macho_atomically CODESIGN ITEM IDENTITY [CODESIGN_ARGUMENT...]" >&2
    return 2
  }
  codesign_tool="$1"
  item="$2"
  identity="$3"
  shift 3
  [ -n "$identity" ] && [ "$identity" != - ] &&
    [[ "$identity" != -* ]] && [[ "$identity" != *$'\n'* ]] &&
    [[ "$identity" != *$'\r'* ]] || {
    echo "Release Mach-O signing requires a non-ad-hoc identity." >&2
    return 2
  }
  sign_macho_atomically "$codesign_tool" "$item" \
    --force --sign "$identity" --options runtime --timestamp "$@" || return 1
  signing_details="$("$codesign_tool" -d --verbose=4 "$item" 2>&1)" || {
    echo "Cannot inspect release Mach-O signature: $item" >&2
    return 1
  }
  if /usr/bin/printf '%s\n' "$signing_details" |
      /usr/bin/grep -Fx 'Signature=adhoc' >/dev/null; then
    echo "Release Mach-O was signed ad-hoc: $item" >&2
    return 1
  fi
  /usr/bin/printf '%s\n' "$signing_details" |
    /usr/bin/grep -F 'Runtime Version=' >/dev/null || {
    echo "Release Mach-O is missing Hardened Runtime: $item" >&2
    return 1
  }
  /usr/bin/printf '%s\n' "$signing_details" |
    /usr/bin/grep -F 'Authority=Developer ID Application:' >/dev/null || {
    echo "Release Mach-O is not signed by a Developer ID Application identity: $item" >&2
    return 1
  }
}

sign_release_macho_at_fd_atomically() {
  local codesign_tool directory_fd item identity

  [ "$#" -ge 4 ] || {
    echo "usage: sign_release_macho_at_fd_atomically CODESIGN DIRECTORY_FD ITEM IDENTITY [CODESIGN_ARGUMENT...]" >&2
    return 2
  }
  codesign_tool="$1"
  directory_fd="$2"
  item="$3"
  identity="$4"
  shift 4
  [ -n "$identity" ] && [ "$identity" != - ] &&
    [[ "$identity" != -* ]] && [[ "$identity" != *$'\n'* ]] &&
    [[ "$identity" != *$'\r'* ]] || {
    echo "Release Mach-O signing requires a non-ad-hoc identity." >&2
    return 2
  }
  sign_macho_at_fd_atomically "$codesign_tool" "$directory_fd" "$item" \
    --force --sign "$identity" --options runtime --timestamp "$@"
}
