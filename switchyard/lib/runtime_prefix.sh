#!/usr/bin/env bash

# Profile-aware Wine prefix preparation and explicit offline migration.
# Stable callers retain Wine's legacy prefix behavior.  Preview callers get a
# closed marker contract and an isolated managed default.

# shellcheck disable=SC2034 # Public marker basename is consumed by callers/tests.
SWITCHYARD_RUNTIME_PREFIX_MARKER=".switchyard-prefix-profile-v1.json"

switchyard_runtime_prefix_python() {
  /usr/bin/python3 -I - "$@" <<'PY'
import ctypes
import errno
import fcntl
import hashlib
import os
import re
import secrets
import stat
import sys
import time

PROFILES = {
    "stable-x86_64-rosetta",
    "preview-native-arm64-fex",
}
MARKER = ".switchyard-prefix-profile-v1.json"
MAX_MARKER_BYTES = 4096
MAX_ENTRIES = 250_000
MAX_DEPTH = 128
MAX_RELATIVE_BYTES = 4096
MAX_ABSOLUTE_BYTES = 1023
DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC
RENAME_EXCL = 0x00000004
CLONE_NOFOLLOW = 0x0001
COPYFILE_ALL = 0x000F
BLOCKING_FILE_FLAGS = sum(
    getattr(stat, name, 0)
    for name in ("UF_APPEND", "UF_IMMUTABLE", "SF_APPEND", "SF_IMMUTABLE")
)


class PrefixError(Exception):
    pass


def fail(message):
    raise PrefixError(message)


def marker_bytes(profile):
    if profile not in PROFILES:
        fail("unknown runtime profile")
    return (
        '{"runtimeFamily":"' + profile + '","schemaVersion":1}\n'
    ).encode("ascii")


def validate_absolute(path, description):
    if (
        not path
        or not os.path.isabs(path)
        or os.path.normpath(path) != path
        or path == os.path.sep
        or "\0" in path
        or "\n" in path
        or "\r" in path
        or len(os.fsencode(path)) > MAX_ABSOLUTE_BYTES
    ):
        fail(description + " is not a bounded canonical absolute path")


def entry_state(value):
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        value.st_uid,
        value.st_gid,
        value.st_nlink,
    )


def inode(value):
    return value.st_dev, value.st_ino


def require_owned_directory(info, description, exact_private=False):
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid():
        fail(description + " is not an owned directory")
    mode = stat.S_IMODE(info.st_mode)
    if info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        fail(description + " is group/world writable")
    if getattr(info, "st_flags", 0) & BLOCKING_FILE_FLAGS:
        fail(description + " has append/immutable filesystem flags")
    if exact_private and mode != 0o700:
        fail(description + " must have mode 0700")


def require_safe_traversal_directory(info, description):
    if not stat.S_ISDIR(info.st_mode):
        fail(description + " is not a directory")
    writable_by_others = info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    if writable_by_others and (
        not info.st_mode & stat.S_ISVTX or info.st_uid not in (0, os.geteuid())
    ):
        fail(description + " is writable by another local principal without sticky protection")


def open_directory(path, create_missing=False, exact_private=False):
    validate_absolute(path, "directory path")
    descriptor = os.open(os.path.sep, DIRECTORY_FLAGS)
    created_any = False
    try:
        require_safe_traversal_directory(os.fstat(descriptor), "filesystem root")
        components = path.split(os.path.sep)[1:]
        for index, component in enumerate(components):
            if component in ("", ".", ".."):
                fail("directory path contains an unsafe component")
            try:
                following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
            except FileNotFoundError:
                if not create_missing:
                    raise
                parent = os.fstat(descriptor)
                require_owned_directory(parent, "parent of a created prefix directory")
                os.mkdir(component, 0o700, dir_fd=descriptor)
                os.fsync(descriptor)
                following = os.open(component, DIRECTORY_FLAGS, dir_fd=descriptor)
                created_any = True
            os.close(descriptor)
            descriptor = following
            require_safe_traversal_directory(
                os.fstat(descriptor), "prefix path ancestor"
            )
            if index == len(components) - 1:
                require_owned_directory(
                    os.fstat(descriptor),
                    "prefix directory",
                    exact_private=exact_private,
                )
        return descriptor, created_any
    except BaseException:
        os.close(descriptor)
        raise


def open_parent(path, create_missing=False):
    validate_absolute(path, "prefix path")
    parent_name, basename = os.path.split(path)
    if not basename or basename in (".", ".."):
        fail("prefix path has an unsafe final component")
    parent_fd, _created = open_directory(parent_name, create_missing=create_missing)
    require_owned_directory(os.fstat(parent_fd), "prefix parent")
    return parent_fd, basename


def require_directory_path(path, descriptor, description):
    payload = fcntl.fcntl(descriptor, fcntl.F_GETPATH, b"\0" * 1024)
    current_path = payload.split(b"\0", 1)[0]
    if current_path != os.fsencode(path):
        fail(description + " no longer has its requested physical path")
    reopened, _created = open_directory(path)
    try:
        if inode(os.fstat(reopened)) != inode(os.fstat(descriptor)):
            fail(description + " path no longer names its opened directory")
    finally:
        os.close(reopened)


def stat_at(directory_fd, name):
    try:
        return os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except FileNotFoundError:
        return None


def write_all(descriptor, data):
    view = memoryview(data)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            fail("short write")
        view = view[written:]


def read_bounded(descriptor, maximum, description):
    chunks = []
    total = 0
    os.lseek(descriptor, 0, os.SEEK_SET)
    while True:
        chunk = os.read(descriptor, min(65536, maximum + 1 - total))
        if not chunk:
            break
        chunks.append(chunk)
        total += len(chunk)
        if total > maximum:
            fail(description + " exceeds its size bound")
    os.lseek(descriptor, 0, os.SEEK_SET)
    return b"".join(chunks)


def validate_marker(directory_fd, expected_profile, required=True):
    expected = marker_bytes(expected_profile)
    entry = stat_at(directory_fd, MARKER)
    if entry is None:
        if required:
            fail("prefix profile marker is missing")
        return None
    if not stat.S_ISREG(entry.st_mode):
        fail("prefix profile marker is not an exact owned regular file")
    descriptor = os.open(MARKER, FILE_FLAGS, dir_fd=directory_fd)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_SH)
        opened = os.fstat(descriptor)
        if inode(opened) != inode(entry):
            fail("prefix profile marker changed while opening it")
        if (
            stat.S_IMODE(opened.st_mode) != 0o600
            or opened.st_uid != os.geteuid()
            or opened.st_nlink != 1
            or opened.st_size <= 0
            or opened.st_size > MAX_MARKER_BYTES
        ):
            fail("prefix profile marker is not an exact owned regular file")
        data = read_bounded(descriptor, MAX_MARKER_BYTES, "prefix profile marker")
        if data != expected:
            fail("prefix profile marker does not match the requested profile")
        if entry_state(os.fstat(descriptor)) != entry_state(opened):
            fail("prefix profile marker changed while reading it")
        current = os.stat(MARKER, dir_fd=directory_fd, follow_symlinks=False)
        if entry_state(current) != entry_state(opened):
            fail("prefix profile marker path changed during validation")
        return entry_state(opened), hashlib.sha256(data).hexdigest()
    finally:
        os.close(descriptor)


def create_marker(directory_fd, profile):
    data = marker_bytes(profile)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC
    temporary = ".switchyard-prefix-profile." + secrets.token_hex(16)
    descriptor = os.open(temporary, flags, 0o600, dir_fd=directory_fd)
    published = False
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        write_all(descriptor, data)
        os.fchmod(descriptor, 0o600)
        os.fsync(descriptor)
        opened = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened.st_mode)
            or stat.S_IMODE(opened.st_mode) != 0o600
            or opened.st_uid != os.geteuid()
            or opened.st_nlink != 1
            or opened.st_size != len(data)
        ):
            fail("created prefix profile marker has unsafe metadata")
        os.link(
            temporary,
            MARKER,
            src_dir_fd=directory_fd,
            dst_dir_fd=directory_fd,
            follow_symlinks=False,
        )
        published = True
        os.unlink(temporary, dir_fd=directory_fd)
        temporary = ""
        os.fsync(directory_fd)
    finally:
        os.close(descriptor)
        if temporary:
            try:
                os.unlink(temporary, dir_fd=directory_fd)
            except FileNotFoundError:
                pass
        if not published:
            os.fsync(directory_fd)
    validated = validate_marker(directory_fd, profile)
    return validated[0]


def unlink_owned_marker(directory_fd, expected_state):
    if expected_state is None:
        return
    current = stat_at(directory_fd, MARKER)
    if current is not None and entry_state(current) == expected_state:
        os.unlink(MARKER, dir_fd=directory_fd)
        os.fsync(directory_fd)


def list_prefix_entries(directory_fd):
    temporary_pattern = re.compile(r"[.]switchyard-prefix-profile[.][0-9a-f]{32}")
    for _attempt in range(1000):
        names = os.listdir(directory_fd)
        if MARKER in names:
            return names
        temporary = [name for name in names if temporary_pattern.fullmatch(name)]
        if temporary and len(temporary) == len(names):
            time.sleep(0.001)
            continue
        return names
    fail("concurrent prefix marker publication did not finish")


def prepare_prefix(profile, requested, managed_root):
    if profile != "preview-native-arm64-fex":
        fail("only the preview profile uses managed prefix preparation")
    validate_absolute(managed_root, "managed prefix root")
    if requested:
        target = requested
        create_parents = False
    else:
        target = os.path.join(managed_root, profile, "default")
        create_parents = True
    validate_absolute(target, "preview prefix")

    parent_fd = -1
    target_fd = -1
    created_directory = False
    created_marker_state = None
    target_name = ""
    target_opened = None
    try:
        parent_fd, target_name = open_parent(target, create_missing=create_parents)
        require_directory_path(os.path.dirname(target), parent_fd, "preview prefix parent")
        target_entry = stat_at(parent_fd, target_name)
        if target_entry is None:
            try:
                os.mkdir(target_name, 0o700, dir_fd=parent_fd)
                os.fsync(parent_fd)
                created_directory = True
            except FileExistsError:
                # A concurrent legitimate launcher may have reserved the same
                # profile default.  Reopen it through the pinned parent and
                # apply the same empty-or-exact-marker policy below.
                pass
            target_entry = os.stat(target_name, dir_fd=parent_fd, follow_symlinks=False)
        if not stat.S_ISDIR(target_entry.st_mode):
            fail("preview prefix is not a real directory")
        target_fd = os.open(target_name, DIRECTORY_FLAGS, dir_fd=parent_fd)
        target_opened = os.fstat(target_fd)
        if inode(target_opened) != inode(target_entry):
            fail("preview prefix changed while opening it")
        require_owned_directory(target_opened, "preview prefix", exact_private=True)
        parent_ready = entry_state(os.fstat(parent_fd))

        names = list_prefix_entries(target_fd)
        if MARKER in names:
            validate_marker(target_fd, profile)
        else:
            if names:
                fail("nonempty preview prefix has no profile marker; use explicit offline migration")
            try:
                created_marker_state = create_marker(target_fd, profile)
            except FileExistsError:
                # The only accepted concurrent winner is the exact marker on
                # this already-pinned directory inode.
                validate_marker(target_fd, profile)
        current_names = os.listdir(target_fd)
        if MARKER not in current_names:
            fail("preview prefix marker disappeared during preparation")
        validate_marker(target_fd, profile)
        current = os.stat(target_name, dir_fd=parent_fd, follow_symlinks=False)
        if inode(current) != inode(target_opened):
            fail("preview prefix path changed during preparation")
        require_directory_path(target, target_fd, "preview prefix")
        if entry_state(os.fstat(parent_fd)) != parent_ready:
            fail("preview prefix parent changed during preparation")
        require_directory_path(os.path.dirname(target), parent_fd, "preview prefix parent")
        print(target)
    except BaseException:
        if target_fd >= 0:
            try:
                unlink_owned_marker(target_fd, created_marker_state)
            except OSError:
                pass
        if created_directory and parent_fd >= 0 and target_fd >= 0 and target_opened is not None:
            try:
                current = stat_at(parent_fd, target_name)
                if current is not None and inode(current) == inode(target_opened):
                    os.rmdir(target_name, dir_fd=parent_fd)
                    os.fsync(parent_fd)
            except OSError:
                pass
        raise
    finally:
        if target_fd >= 0:
            os.close(target_fd)
        if parent_fd >= 0:
            os.close(parent_fd)


def validate_prepared_prefix(profile, target):
    if profile not in PROFILES:
        fail("prepared prefix validation requires a known profile")
    validate_absolute(target, "prepared prefix")
    parent_fd = -1
    target_fd = -1
    try:
        parent_fd, target_name = open_parent(target, create_missing=False)
        require_directory_path(
            os.path.dirname(target), parent_fd, "prepared prefix parent"
        )
        parent_opened = entry_state(os.fstat(parent_fd))
        target_entry = os.stat(target_name, dir_fd=parent_fd, follow_symlinks=False)
        if not stat.S_ISDIR(target_entry.st_mode):
            fail("prepared prefix is not a real directory")
        target_fd = os.open(target_name, DIRECTORY_FLAGS, dir_fd=parent_fd)
        target_opened = os.fstat(target_fd)
        if inode(target_opened) != inode(target_entry):
            fail("prepared prefix changed while opening it")
        require_owned_directory(target_opened, "prepared prefix", exact_private=True)
        validate_marker(target_fd, profile, required=True)
        current = os.stat(target_name, dir_fd=parent_fd, follow_symlinks=False)
        if inode(current) != inode(target_opened):
            fail("prepared prefix path changed during validation")
        require_directory_path(target, target_fd, "prepared prefix")
        if entry_state(os.fstat(parent_fd)) != parent_opened:
            fail("prepared prefix parent changed during validation")
        require_directory_path(
            os.path.dirname(target), parent_fd, "prepared prefix parent"
        )
        print(target)
    finally:
        if target_fd >= 0:
            os.close(target_fd)
        if parent_fd >= 0:
            os.close(parent_fd)


def relative_child(parent, name):
    value = name if not parent else parent + "/" + name
    if len(os.fsencode(value)) > MAX_RELATIVE_BYTES:
        fail("prefix entry path exceeds its size bound")
    return value


def validate_entry_name(name):
    if name in ("", ".", "..") or "/" in name or "\0" in name or "\n" in name or "\r" in name:
        fail("prefix contains an unsafe entry name")


def digest_fd(descriptor):
    digest = hashlib.sha256()
    os.lseek(descriptor, 0, os.SEEK_SET)
    while True:
        chunk = os.read(descriptor, 1024 * 1024)
        if not chunk:
            break
        digest.update(chunk)
    os.lseek(descriptor, 0, os.SEEK_SET)
    return digest.hexdigest()


libc = ctypes.CDLL(None, use_errno=True)
fclonefileat = getattr(libc, "fclonefileat", None)
if fclonefileat is not None:
    fclonefileat.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
    fclonefileat.restype = ctypes.c_int
fcopyfile = getattr(libc, "fcopyfile", None)
if fcopyfile is not None:
    fcopyfile.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    fcopyfile.restype = ctypes.c_int
renameatx_np = getattr(libc, "renameatx_np", None)
if renameatx_np is not None:
    renameatx_np.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_uint32,
    ]
    renameatx_np.restype = ctypes.c_int


def copy_regular(source_fd, destination_directory_fd, name, source_info):
    cloned = False
    if fclonefileat is not None:
        result = fclonefileat(
            source_fd,
            destination_directory_fd,
            os.fsencode(name),
            CLONE_NOFOLLOW,
        )
        if result == 0:
            cloned = True
        else:
            error = ctypes.get_errno()
            if error not in (errno.EXDEV, errno.ENOTSUP, errno.EINVAL, errno.ENOSYS):
                raise OSError(error, os.strerror(error), name)
    if not cloned:
        destination_fd = os.open(
            name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
            stat.S_IMODE(source_info.st_mode),
            dir_fd=destination_directory_fd,
        )
        try:
            os.lseek(source_fd, 0, os.SEEK_SET)
            if fcopyfile is not None:
                if fcopyfile(source_fd, destination_fd, None, COPYFILE_ALL) != 0:
                    error = ctypes.get_errno()
                    raise OSError(error, os.strerror(error), name)
            else:
                while True:
                    chunk = os.read(source_fd, 1024 * 1024)
                    if not chunk:
                        break
                    write_all(destination_fd, chunk)
        finally:
            os.close(destination_fd)

    destination_fd = os.open(name, FILE_FLAGS, dir_fd=destination_directory_fd)
    try:
        destination_info = os.fstat(destination_fd)
        if not stat.S_ISREG(destination_info.st_mode):
            fail("copied prefix file is not regular")
        os.fchmod(destination_fd, stat.S_IMODE(source_info.st_mode))
        try:
            os.utime(
                destination_fd,
                ns=(source_info.st_atime_ns, source_info.st_mtime_ns),
            )
        except (NotImplementedError, OSError):
            pass
        source_digest = digest_fd(source_fd)
        destination_digest = digest_fd(destination_fd)
        if source_digest != destination_digest:
            fail("copied prefix file bytes do not match their pinned source")
        os.fsync(destination_fd)
        return source_digest
    finally:
        os.close(destination_fd)


def copy_symlink(source_directory_fd, destination_directory_fd, name, source_info):
    target = os.readlink(name, dir_fd=source_directory_fd)
    current = os.stat(name, dir_fd=source_directory_fd, follow_symlinks=False)
    if entry_state(current) != entry_state(source_info):
        fail("prefix symbolic link changed while reading it")
    os.symlink(target, name, dir_fd=destination_directory_fd)
    try:
        os.chmod(
            name,
            stat.S_IMODE(source_info.st_mode),
            dir_fd=destination_directory_fd,
            follow_symlinks=False,
        )
    except (NotImplementedError, OSError, ValueError):
        pass
    copied = os.stat(name, dir_fd=destination_directory_fd, follow_symlinks=False)
    if not stat.S_ISLNK(copied.st_mode) or os.readlink(name, dir_fd=destination_directory_fd) != target:
        fail("copied prefix symbolic link changed")
    return target


def copy_tree(source_fd, destination_fd, records, relative="", depth=0, counter=None):
    if counter is None:
        counter = [0]
    if depth > MAX_DEPTH:
        fail("prefix tree exceeds its nesting bound")
    source_opened = os.fstat(source_fd)
    require_owned_directory(source_opened, "source prefix directory")
    names = sorted(os.listdir(source_fd), key=os.fsencode)
    records[relative] = {
        "kind": "directory",
        "state": entry_state(source_opened),
        "names": tuple(names),
        "mode": stat.S_IMODE(source_opened.st_mode),
    }
    for name in names:
        validate_entry_name(name)
        child_relative = relative_child(relative, name)
        counter[0] += 1
        if counter[0] > MAX_ENTRIES:
            fail("prefix tree exceeds its entry-count bound")
        source_entry = os.stat(name, dir_fd=source_fd, follow_symlinks=False)
        if source_entry.st_uid != os.geteuid():
            fail("prefix tree contains an entry not owned by the current user")
        if getattr(source_entry, "st_flags", 0) & BLOCKING_FILE_FLAGS:
            fail("prefix tree contains an append-only or immutable entry")
        if stat.S_ISREG(source_entry.st_mode):
            child_source_fd = os.open(name, FILE_FLAGS, dir_fd=source_fd)
            try:
                source_open_file = os.fstat(child_source_fd)
                if entry_state(source_open_file) != entry_state(source_entry):
                    fail("prefix file changed while opening it")
                if relative == "" and name == MARKER:
                    file_digest = digest_fd(child_source_fd)
                else:
                    file_digest = copy_regular(
                        child_source_fd,
                        destination_fd,
                        name,
                        source_open_file,
                    )
                if entry_state(os.fstat(child_source_fd)) != entry_state(source_open_file):
                    fail("prefix file changed while copying it")
                records[child_relative] = {
                    "kind": "source-marker" if relative == "" and name == MARKER else "file",
                    "state": entry_state(source_open_file),
                    "mode": stat.S_IMODE(source_open_file.st_mode),
                    "size": source_open_file.st_size,
                    "digest": file_digest,
                }
            finally:
                os.close(child_source_fd)
        elif stat.S_ISDIR(source_entry.st_mode):
            child_source_fd = os.open(name, DIRECTORY_FLAGS, dir_fd=source_fd)
            try:
                if entry_state(os.fstat(child_source_fd)) != entry_state(source_entry):
                    fail("prefix directory changed while opening it")
                os.mkdir(name, stat.S_IMODE(source_entry.st_mode), dir_fd=destination_fd)
                child_destination_fd = os.open(name, DIRECTORY_FLAGS, dir_fd=destination_fd)
                try:
                    copy_tree(
                        child_source_fd,
                        child_destination_fd,
                        records,
                        child_relative,
                        depth + 1,
                        counter,
                    )
                    os.fchmod(child_destination_fd, stat.S_IMODE(source_entry.st_mode))
                    try:
                        os.utime(
                            child_destination_fd,
                            ns=(source_entry.st_atime_ns, source_entry.st_mtime_ns),
                        )
                    except (NotImplementedError, OSError):
                        pass
                    os.fsync(child_destination_fd)
                finally:
                    os.close(child_destination_fd)
                if entry_state(os.fstat(child_source_fd)) != entry_state(source_entry):
                    fail("prefix directory changed while copying it")
            finally:
                os.close(child_source_fd)
        elif stat.S_ISLNK(source_entry.st_mode):
            target = copy_symlink(source_fd, destination_fd, name, source_entry)
            records[child_relative] = {
                "kind": "symlink",
                "state": entry_state(source_entry),
                "mode": stat.S_IMODE(source_entry.st_mode),
                "target": target,
            }
        else:
            fail("prefix tree contains a special file")
    if entry_state(os.fstat(source_fd)) != entry_state(source_opened):
        fail("source prefix directory changed while copying it")


def verify_source_tree(source_fd, records, relative="", depth=0):
    if depth > MAX_DEPTH:
        fail("prefix verification exceeds its nesting bound")
    record = records[relative]
    if entry_state(os.fstat(source_fd)) != record["state"]:
        fail("source prefix directory changed after copying")
    names = tuple(sorted(os.listdir(source_fd), key=os.fsencode))
    if names != record["names"]:
        fail("source prefix entries changed after copying")
    for name in names:
        child_relative = relative_child(relative, name)
        expected = records[child_relative]
        current = os.stat(name, dir_fd=source_fd, follow_symlinks=False)
        if entry_state(current) != expected["state"]:
            fail("source prefix entry changed after copying")
        if expected["kind"] == "directory":
            child = os.open(name, DIRECTORY_FLAGS, dir_fd=source_fd)
            try:
                verify_source_tree(child, records, child_relative, depth + 1)
            finally:
                os.close(child)
        elif expected["kind"] == "symlink":
            if os.readlink(name, dir_fd=source_fd) != expected["target"]:
                fail("source prefix symbolic link changed after copying")


def verify_destination_tree(destination_fd, records, target_profile, relative="", depth=0):
    if depth > MAX_DEPTH:
        fail("destination prefix verification exceeds its nesting bound")
    source_record = records[relative]
    expected_names = list(source_record["names"])
    if relative == "":
        expected_names = [name for name in expected_names if name != MARKER]
        expected_names.append(MARKER)
    expected_names = tuple(sorted(expected_names, key=os.fsencode))
    names = tuple(sorted(os.listdir(destination_fd), key=os.fsencode))
    if names != expected_names:
        fail("destination prefix closure differs from its source")
    if relative == "":
        require_owned_directory(
            os.fstat(destination_fd), "migrated prefix", exact_private=True
        )
        validate_marker(destination_fd, target_profile)
    for name in names:
        if relative == "" and name == MARKER:
            continue
        child_relative = relative_child(relative, name)
        expected = records[child_relative]
        current = os.stat(name, dir_fd=destination_fd, follow_symlinks=False)
        if current.st_uid != os.geteuid():
            fail("destination prefix entry is not owned by the current user")
        if expected["kind"] == "directory":
            if not stat.S_ISDIR(current.st_mode) or stat.S_IMODE(current.st_mode) != expected["mode"]:
                fail("destination prefix directory metadata differs from its source")
            child = os.open(name, DIRECTORY_FLAGS, dir_fd=destination_fd)
            try:
                verify_destination_tree(child, records, target_profile, child_relative, depth + 1)
            finally:
                os.close(child)
        elif expected["kind"] == "file":
            if (
                not stat.S_ISREG(current.st_mode)
                or stat.S_IMODE(current.st_mode) != expected["mode"]
                or current.st_size != expected["size"]
            ):
                fail("destination prefix file metadata differs from its source")
            child = os.open(name, FILE_FLAGS, dir_fd=destination_fd)
            try:
                if digest_fd(child) != expected["digest"]:
                    fail("destination prefix file bytes differ from their source")
            finally:
                os.close(child)
        elif expected["kind"] == "symlink":
            if not stat.S_ISLNK(current.st_mode) or os.readlink(name, dir_fd=destination_fd) != expected["target"]:
                fail("destination prefix symbolic link differs from its source")
        else:
            fail("destination prefix contains an unexpected source marker")


def snapshot_removal_tree(directory_fd, records, relative="", depth=0, counter=None):
    if counter is None:
        counter = [0]
    if depth > MAX_DEPTH:
        fail("prefix removal tree exceeds its nesting bound")
    os.fsync(directory_fd)
    opened = os.fstat(directory_fd)
    require_owned_directory(opened, "prefix removal tree directory")
    names = tuple(sorted(os.listdir(directory_fd), key=os.fsencode))
    records[relative] = {
        "kind": "directory",
        "state": entry_state(opened),
        "names": names,
    }
    for name in names:
        validate_entry_name(name)
        child_relative = relative_child(relative, name)
        counter[0] += 1
        if counter[0] > MAX_ENTRIES:
            fail("prefix removal tree exceeds its entry-count bound")
        child_info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if child_info.st_uid != os.geteuid():
            fail("prefix removal tree contains an entry not owned by the current user")
        if getattr(child_info, "st_flags", 0) & BLOCKING_FILE_FLAGS:
            fail("prefix removal tree contains an append-only or immutable entry")
        record = {
            "state": entry_state(child_info),
        }
        if stat.S_ISDIR(child_info.st_mode):
            child_fd = os.open(name, DIRECTORY_FLAGS, dir_fd=directory_fd)
            try:
                if entry_state(os.fstat(child_fd)) != entry_state(child_info):
                    fail("prefix removal directory changed while opening it")
                snapshot_removal_tree(
                    child_fd,
                    records,
                    child_relative,
                    depth + 1,
                    counter,
                )
            finally:
                os.close(child_fd)
            continue
        if stat.S_ISLNK(child_info.st_mode):
            record["kind"] = "symlink"
            record["target"] = os.readlink(name, dir_fd=directory_fd)
            if entry_state(
                os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            ) != entry_state(child_info):
                fail("prefix removal symbolic link changed while reading it")
        else:
            record["kind"] = "leaf"
        records[child_relative] = record
    if entry_state(os.fstat(directory_fd)) != entry_state(opened):
        fail("prefix removal directory changed while snapshotting it")


def verify_removal_tree(
    directory_fd, records, relative="", depth=0, allow_root_rename=False
):
    if depth > MAX_DEPTH:
        fail("prefix removal verification exceeds its nesting bound")
    expected = records[relative]
    current_directory_state = entry_state(os.fstat(directory_fd))
    expected_directory_state = expected["state"]
    if relative == "" and allow_root_rename:
        # Renaming a directory updates its own ctime without changing the tree.
        current_directory_state = (
            current_directory_state[:5] + current_directory_state[6:]
        )
        expected_directory_state = (
            expected_directory_state[:5] + expected_directory_state[6:]
        )
    if current_directory_state != expected_directory_state:
        fail("prefix removal directory changed after snapshotting")
    names = tuple(sorted(os.listdir(directory_fd), key=os.fsencode))
    if names != expected["names"]:
        fail("prefix removal entries changed after snapshotting")
    for name in names:
        child_relative = relative_child(relative, name)
        child_expected = records[child_relative]
        current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if entry_state(current) != child_expected["state"]:
            fail("prefix removal entry changed after snapshotting")
        if child_expected["kind"] == "directory":
            child_fd = os.open(name, DIRECTORY_FLAGS, dir_fd=directory_fd)
            try:
                if entry_state(os.fstat(child_fd)) != child_expected["state"]:
                    fail("prefix removal directory changed while reopening it")
                verify_removal_tree(child_fd, records, child_relative, depth + 1)
            finally:
                os.close(child_fd)
        elif child_expected["kind"] == "symlink":
            if os.readlink(name, dir_fd=directory_fd) != child_expected["target"]:
                fail("prefix removal symbolic link changed after snapshotting")


def clear_removal_tree(directory_fd, records, deletion_started, relative="", depth=0):
    if depth > MAX_DEPTH:
        fail("prefix removal cleanup exceeds its nesting bound")
    expected = records[relative]
    names = tuple(sorted(os.listdir(directory_fd), key=os.fsencode))
    if names != expected["names"]:
        fail("prefix removal tree changed before cleanup")
    ordered_names = [name for name in names if not (relative == "" and name == MARKER)]
    if relative == "" and MARKER in names:
        ordered_names.append(MARKER)
    for name in ordered_names:
        child_relative = relative_child(relative, name)
        child_expected = records[child_relative]
        current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if entry_state(current) != child_expected["state"]:
            fail("prefix removal entry changed before cleanup")
        if child_expected["kind"] == "directory":
            child_fd = os.open(name, DIRECTORY_FLAGS, dir_fd=directory_fd)
            try:
                if entry_state(os.fstat(child_fd)) != child_expected["state"]:
                    fail("prefix removal directory changed while opening for cleanup")
                clear_removal_tree(
                    child_fd, records, deletion_started, child_relative, depth + 1
                )
            finally:
                os.close(child_fd)
            latest = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if inode(latest) != inode(current):
                fail("prefix removal directory path changed during cleanup")
            deletion_started[0] = True
            os.rmdir(name, dir_fd=directory_fd)
        else:
            latest = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if entry_state(latest) != child_expected["state"]:
                fail("prefix removal entry path changed during cleanup")
            deletion_started[0] = True
            os.unlink(name, dir_fd=directory_fd)


def restore_removal_tombstone(parent_fd, target_name, tombstone_name, target_fd, target_info):
    target_current = stat_at(parent_fd, target_name)
    tombstone_current = stat_at(parent_fd, tombstone_name)
    if (
        target_current is not None
        or tombstone_current is None
        or inode(tombstone_current) != inode(target_info)
        or inode(os.fstat(target_fd)) != inode(target_info)
    ):
        return False
    result = renameatx_np(
        parent_fd,
        os.fsencode(tombstone_name),
        parent_fd,
        os.fsencode(target_name),
        RENAME_EXCL,
    )
    if result != 0:
        return False
    restored = os.stat(target_name, dir_fd=parent_fd, follow_symlinks=False)
    return inode(restored) == inode(target_info)


def remove_prefix(profile, prefix, home, managed_root):
    if profile not in PROFILES:
        fail("prefix removal requires a known profile")
    validate_absolute(prefix, "prefix removal target")
    validate_absolute(home, "home directory")
    validate_absolute(managed_root, "managed prefix root")
    if (
        prefix in (home, managed_root)
        or home.startswith(prefix + os.path.sep)
        or managed_root.startswith(prefix + os.path.sep)
    ):
        fail("prefix removal target is a protected directory")
    if renameatx_np is None:
        fail("platform does not provide exclusive atomic prefix removal")

    parent_fd = -1
    target_fd = -1
    target_name = ""
    tombstone_name = ""
    tombstone_path = ""
    target_info = None
    renamed = False
    deletion_started = [False]
    try:
        parent_fd, target_name = open_parent(prefix, create_missing=False)
        parent_name = os.path.dirname(prefix)
        require_directory_path(parent_name, parent_fd, "prefix removal parent")
        parent_info = os.fstat(parent_fd)

        target_entry = os.stat(target_name, dir_fd=parent_fd, follow_symlinks=False)
        if not stat.S_ISDIR(target_entry.st_mode):
            fail("prefix removal target is not a real directory")
        target_fd = os.open(target_name, DIRECTORY_FLAGS, dir_fd=parent_fd)
        target_info = os.fstat(target_fd)
        if entry_state(target_info) != entry_state(target_entry):
            fail("prefix removal target changed while opening it")
        require_owned_directory(target_info, "prefix removal target", exact_private=True)
        require_directory_path(prefix, target_fd, "prefix removal target")
        validate_marker(target_fd, profile, required=True)

        records = {}
        snapshot_removal_tree(target_fd, records)
        target_info = os.fstat(target_fd)
        if entry_state(target_info) != records[""]["state"]:
            fail("prefix removal target did not stabilize while snapshotting")
        verify_removal_tree(target_fd, records)
        validate_marker(target_fd, profile, required=True)
        if entry_state(os.fstat(target_fd)) != entry_state(target_info):
            fail("prefix removal target changed before tombstoning")
        if entry_state(os.fstat(parent_fd)) != entry_state(parent_info):
            fail("prefix removal parent changed before tombstoning")
        current_target = os.stat(target_name, dir_fd=parent_fd, follow_symlinks=False)
        if entry_state(current_target) != entry_state(target_info):
            fail("prefix removal target path changed before tombstoning")
        require_directory_path(prefix, target_fd, "prefix removal target")
        require_directory_path(parent_name, parent_fd, "prefix removal parent")

        for _attempt in range(128):
            candidate = ".switchyard-prefix-removal." + secrets.token_hex(16)
            if stat_at(parent_fd, candidate) is not None:
                continue
            result = renameatx_np(
                parent_fd,
                os.fsencode(target_name),
                parent_fd,
                os.fsencode(candidate),
                RENAME_EXCL,
            )
            if result == 0:
                tombstone_name = candidate
                tombstone_path = os.path.join(parent_name, candidate)
                renamed = True
                break
            error = ctypes.get_errno()
            if error == errno.EEXIST:
                continue
            raise OSError(error, os.strerror(error), prefix)
        if not renamed:
            fail("could not reserve a private prefix removal tombstone")
        os.fsync(parent_fd)

        # A second complete state pass creates a deterministic no-delete window
        # for detecting swaps immediately after the exclusive rename.
        verify_removal_tree(target_fd, records, allow_root_rename=True)
        tombstone_entry = os.stat(
            tombstone_name, dir_fd=parent_fd, follow_symlinks=False
        )
        if inode(tombstone_entry) != inode(target_info):
            fail("prefix removal tombstone path does not name the original prefix")
        if stat_at(parent_fd, target_name) is not None:
            fail("prefix removal target reappeared after tombstoning")
        require_directory_path(tombstone_path, target_fd, "prefix removal tombstone")
        require_directory_path(parent_name, parent_fd, "prefix removal parent")

        clear_removal_tree(target_fd, records, deletion_started)
        current_tombstone = os.stat(
            tombstone_name, dir_fd=parent_fd, follow_symlinks=False
        )
        if inode(current_tombstone) != inode(target_info):
            fail("prefix removal tombstone changed during cleanup")
        os.rmdir(tombstone_name, dir_fd=parent_fd)
        tombstone_name = ""
        tombstone_path = ""
        os.fsync(parent_fd)
    except BaseException:
        if renamed and tombstone_name:
            restored = False
            if not deletion_started[0]:
                try:
                    restored = restore_removal_tombstone(
                        parent_fd,
                        target_name,
                        tombstone_name,
                        target_fd,
                        target_info,
                    )
                    if restored:
                        os.fsync(parent_fd)
                except (OSError, PrefixError):
                    restored = False
            if not restored:
                try:
                    current_path = fcntl.fcntl(
                        target_fd, fcntl.F_GETPATH, b"\0" * 1024
                    ).split(b"\0", 1)[0]
                    if current_path.startswith(b"/"):
                        tombstone_path = os.fsdecode(current_path)
                except OSError:
                    pass
                print(
                    "runtime prefix removal preserved a recoverable tombstone: "
                    + tombstone_path,
                    file=sys.stderr,
                )
        raise
    finally:
        if target_fd >= 0:
            os.close(target_fd)
        if parent_fd >= 0:
            os.close(parent_fd)


def directory_is_descendant(candidate_fd, ancestor_info):
    descriptor = os.dup(candidate_fd)
    try:
        while True:
            current = os.fstat(descriptor)
            if inode(current) == inode(ancestor_info):
                return True
            parent = os.open("..", DIRECTORY_FLAGS, dir_fd=descriptor)
            parent_info = os.fstat(parent)
            if inode(parent_info) == inode(current):
                os.close(parent)
                return False
            os.close(descriptor)
            descriptor = parent
    finally:
        os.close(descriptor)


def clear_directory(directory_fd, depth=0):
    if depth > MAX_DEPTH:
        fail("staging cleanup exceeds its nesting bound")
    for name in os.listdir(directory_fd):
        validate_entry_name(name)
        current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if stat.S_ISDIR(current.st_mode):
            child = os.open(name, DIRECTORY_FLAGS, dir_fd=directory_fd)
            try:
                clear_directory(child, depth + 1)
            finally:
                os.close(child)
            latest = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if inode(latest) != inode(current):
                fail("staging directory changed during cleanup")
            os.rmdir(name, dir_fd=directory_fd)
        else:
            latest = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if inode(latest) != inode(current):
                fail("staging entry changed during cleanup")
            os.unlink(name, dir_fd=directory_fd)


def remove_stage(parent_fd, stage_name, stage_fd, stage_info):
    current = stat_at(parent_fd, stage_name)
    if current is None:
        return
    if inode(current) != inode(stage_info) or inode(os.fstat(stage_fd)) != inode(stage_info):
        fail("migration staging path changed before cleanup")
    clear_directory(stage_fd)
    current = os.stat(stage_name, dir_fd=parent_fd, follow_symlinks=False)
    if inode(current) != inode(stage_info):
        fail("migration staging path changed during cleanup")
    os.rmdir(stage_name, dir_fd=parent_fd)
    os.fsync(parent_fd)


def migrate_prefix(source_profile, source, target_profile, target):
    if source_profile not in PROFILES or target_profile not in PROFILES:
        fail("migration requires known source and target profiles")
    if source_profile == target_profile:
        fail("migration source and target profiles must differ")
    validate_absolute(source, "source prefix")
    validate_absolute(target, "target prefix")
    if target == source or target.startswith(source + os.path.sep):
        fail("migration target must not be the source or its descendant")

    source_fd = -1
    target_parent_fd = -1
    stage_fd = -1
    stage_name = ""
    stage_info = None
    published = False
    try:
        source_fd, _created = open_directory(source)
        source_info = os.fstat(source_fd)
        require_owned_directory(source_info, "source prefix")
        require_directory_path(source, source_fd, "source prefix")
        if source_profile == "stable-x86_64-rosetta":
            validate_marker(source_fd, source_profile, required=False)
        else:
            validate_marker(source_fd, source_profile, required=True)

        target_parent_fd, target_name = open_parent(target, create_missing=False)
        target_parent_name = os.path.dirname(target)
        require_directory_path(
            target_parent_name, target_parent_fd, "migration target parent"
        )
        if directory_is_descendant(target_parent_fd, source_info):
            fail("migration target parent is inside the source prefix")
        if stat_at(target_parent_fd, target_name) is not None:
            fail("migration target already exists")
        if renameatx_np is None:
            fail("platform does not provide exclusive atomic prefix publication")

        for _attempt in range(128):
            candidate = ".switchyard-prefix-migration." + secrets.token_hex(16)
            try:
                os.mkdir(candidate, 0o700, dir_fd=target_parent_fd)
                stage_name = candidate
                break
            except FileExistsError:
                continue
        if not stage_name:
            fail("could not reserve a private migration staging directory")
        stage_fd = os.open(stage_name, DIRECTORY_FLAGS, dir_fd=target_parent_fd)
        stage_info = os.fstat(stage_fd)
        require_owned_directory(stage_info, "migration staging directory", exact_private=True)
        require_directory_path(
            os.path.join(target_parent_name, stage_name),
            stage_fd,
            "migration staging directory",
        )
        target_parent_ready = entry_state(os.fstat(target_parent_fd))

        records = {}
        copy_tree(source_fd, stage_fd, records)
        create_marker(stage_fd, target_profile)
        verify_source_tree(source_fd, records)
        verify_destination_tree(stage_fd, records, target_profile)
        if entry_state(os.fstat(source_fd)) != entry_state(source_info):
            fail("source prefix changed before migration publication")
        require_directory_path(source, source_fd, "source prefix")
        require_directory_path(
            target_parent_name, target_parent_fd, "migration target parent"
        )
        if entry_state(os.fstat(target_parent_fd)) != target_parent_ready:
            fail("migration target parent changed before publication")
        require_directory_path(
            os.path.join(target_parent_name, stage_name),
            stage_fd,
            "migration staging directory",
        )
        current_stage = os.stat(stage_name, dir_fd=target_parent_fd, follow_symlinks=False)
        if inode(current_stage) != inode(stage_info):
            fail("migration staging path changed before publication")
        if stat_at(target_parent_fd, target_name) is not None:
            fail("migration target appeared before publication")

        result = renameatx_np(
            target_parent_fd,
            os.fsencode(stage_name),
            target_parent_fd,
            os.fsencode(target_name),
            RENAME_EXCL,
        )
        if result != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), target)
        published = True
        os.fsync(target_parent_fd)
        require_directory_path(
            target_parent_name, target_parent_fd, "migration target parent"
        )
        require_directory_path(target, stage_fd, "published migration target")
        final_entry = os.stat(target_name, dir_fd=target_parent_fd, follow_symlinks=False)
        if inode(final_entry) != inode(stage_info):
            fail("published migration target does not name the staged prefix")
        final_fd = os.open(target_name, DIRECTORY_FLAGS, dir_fd=target_parent_fd)
        try:
            if inode(os.fstat(final_fd)) != inode(stage_info):
                fail("published migration target changed while opening it")
            require_directory_path(target, final_fd, "published migration target")
            verify_destination_tree(final_fd, records, target_profile)
        finally:
            os.close(final_fd)
    except BaseException:
        if not published and stage_fd >= 0 and stage_info is not None:
            try:
                remove_stage(target_parent_fd, stage_name, stage_fd, stage_info)
            except (OSError, PrefixError) as cleanup_error:
                print(
                    "runtime prefix migration could not clean its private staging tree: "
                    + str(cleanup_error),
                    file=sys.stderr,
                )
        raise
    finally:
        if stage_fd >= 0:
            os.close(stage_fd)
        if target_parent_fd >= 0:
            os.close(target_parent_fd)
        if source_fd >= 0:
            os.close(source_fd)


try:
    if len(sys.argv) < 2:
        fail("invalid runtime prefix helper invocation")
    action = sys.argv[1]
    if action == "prepare" and len(sys.argv) == 5:
        prepare_prefix(*sys.argv[2:])
    elif action == "validate-prepared" and len(sys.argv) == 4:
        validate_prepared_prefix(*sys.argv[2:])
    elif action == "migrate" and len(sys.argv) == 6:
        migrate_prefix(*sys.argv[2:])
    elif action == "remove" and len(sys.argv) == 6:
        remove_prefix(*sys.argv[2:])
    else:
        fail("invalid runtime prefix helper invocation")
except (OSError, UnicodeError, PrefixError, ValueError) as error:
    print("runtime prefix policy failed: " + str(error), file=sys.stderr)
    raise SystemExit(1)
PY
}

switchyard_prepare_runtime_prefix() {
  local runtime_profile requested_prefix output_variable resolved_prefix

  [ "$#" -eq 3 ] || {
    echo "usage: switchyard_prepare_runtime_prefix PROFILE REQUESTED_PREFIX OUTPUT_VARIABLE" >&2
    return 2
  }
  runtime_profile="$1"
  requested_prefix="$2"
  output_variable="$3"
  [[ "$output_variable" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || {
    echo "Runtime prefix output variable is invalid." >&2
    return 2
  }
  printf -v "$output_variable" '%s' '' || return 2

  case "$runtime_profile" in
    stable-x86_64-rosetta)
      # Stable Wine keeps its historical behavior exactly: an explicit value is
      # passed through, and an unset value remains the conventional ~/.wine.
      if [ -n "$requested_prefix" ]; then
        resolved_prefix="$requested_prefix"
      else
        resolved_prefix="${HOME}/.wine"
      fi
      ;;
    preview-native-arm64-fex)
      resolved_prefix="$(switchyard_runtime_prefix_python prepare \
        "$runtime_profile" "$requested_prefix" \
        "${SWITCHYARD_MANAGED_PREFIX_ROOT:-${HOME}/.switchyard/prefixes}")" || return 1
      ;;
    *)
      echo "Unknown runtime prefix profile: $runtime_profile" >&2
      return 2
      ;;
  esac
  printf -v "$output_variable" '%s' "$resolved_prefix"
}

switchyard_validate_prepared_runtime_prefix() {
  local runtime_profile requested_prefix output_variable resolved_prefix

  [ "$#" -eq 3 ] || {
    echo "usage: switchyard_validate_prepared_runtime_prefix PROFILE PREFIX OUTPUT_VARIABLE" >&2
    return 2
  }
  runtime_profile="$1"
  requested_prefix="$2"
  output_variable="$3"
  [[ "$output_variable" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || {
    echo "Prepared prefix output variable is invalid." >&2
    return 2
  }
  printf -v "$output_variable" '%s' '' || return 2
  resolved_prefix="$(switchyard_runtime_prefix_python validate-prepared \
    "$runtime_profile" "$requested_prefix")" || return 1
  printf -v "$output_variable" '%s' "$resolved_prefix"
}

# The caller must first stop and wait for the source prefix's exact wineserver.
# This helper never terminates a live process or mutates the source; its pinned
# second pass rejects any filesystem mutation observed during the offline copy.
switchyard_migrate_runtime_prefix_offline() {
  [ "$#" -eq 4 ] || {
    echo "usage: switchyard_migrate_runtime_prefix_offline SOURCE_PROFILE SOURCE TARGET_PROFILE TARGET" >&2
    return 2
  }
  switchyard_runtime_prefix_python migrate "$1" "$2" "$3" "$4"
}

# The caller must first stop and wait for this exact prefix's wineserver.  Only
# a mode-0700, owner-controlled real directory carrying the exact same-profile
# marker is tombstoned and removed; legacy unmarked stable prefixes are refused.
switchyard_remove_runtime_prefix_offline() {
  [ "$#" -eq 2 ] || {
    echo "usage: switchyard_remove_runtime_prefix_offline PROFILE PREFIX" >&2
    return 2
  }
  switchyard_runtime_prefix_python remove "$1" "$2" \
    "$HOME" "${SWITCHYARD_MANAGED_PREFIX_ROOT:-${HOME}/.switchyard/prefixes}"
}
