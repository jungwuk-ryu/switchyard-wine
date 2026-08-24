#!/usr/bin/env python3
"""Copy a runtime tree between two already-open directory descriptors.

The caller must invoke the system interpreter with an empty environment and
isolated mode, for example:

    /usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        LC_ALL=C LANG=C TMPDIR=/private/tmp \
        /usr/bin/python3 -I fd_tree_copy.py SOURCE_FD DESTINATION_FD

SOURCE_FD and DESTINATION_FD select the roots for the lifetime of the copy.
This program never resolves a public pathname for either root.  The destination
must be a plain, owned, empty mode-0700 directory.  Failure intentionally leaves
new entries in place: deleting them by name could delete an attacker's
replacement after a concurrent rename.
"""

import ctypes
import errno
import hashlib
import os
import posixpath
import stat
import sys
import unicodedata


MAX_DEPTH = 256
MAX_ENTRIES = 200_000
MAX_DIRECTORY_ENTRIES = 100_000
MAX_RELATIVE_PATH_BYTES = 16 * 1024
MAX_SYMLINK_TARGET_BYTES = 16 * 1024
MAX_METADATA_BYTES = 64 * 1024 * 1024
MAX_FILE_BYTES = 32 * 1024 * 1024 * 1024
MAX_TOTAL_FILE_BYTES = 128 * 1024 * 1024 * 1024
MAX_PROVENANCE_BYTES = 1024
READ_SIZE = 1024 * 1024

ACL_TYPE_EXTENDED = 0x00000100
O_SYMLINK = 0x00200000  # Darwin sys/fcntl.h; opens the link, not its target.
ALLOWED_XATTR = b"com.apple.provenance"


class CopyError(Exception):
    pass


libc = ctypes.CDLL(None, use_errno=True)
libc.acl_get_fd_np.argtypes = ctypes.c_int, ctypes.c_int
libc.acl_get_fd_np.restype = ctypes.c_void_p
libc.acl_free.argtypes = ctypes.c_void_p,
libc.acl_free.restype = ctypes.c_int
libc.flistxattr.argtypes = ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int
libc.flistxattr.restype = ctypes.c_ssize_t
libc.fgetxattr.argtypes = (
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.c_uint32,
    ctypes.c_int,
)
libc.fgetxattr.restype = ctypes.c_ssize_t
libc.fsetxattr.argtypes = (
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.c_uint32,
    ctypes.c_int,
)
libc.fsetxattr.restype = ctypes.c_int
libc.fremovexattr.argtypes = ctypes.c_int, ctypes.c_char_p, ctypes.c_int
libc.fremovexattr.restype = ctypes.c_int


def required_flag(name):
    value = getattr(os, name, None)
    if value is None:
        raise CopyError("fd tree copy requires " + name)
    return value


DIRECTORY_FLAGS = (
    os.O_RDONLY
    | required_flag("O_DIRECTORY")
    | required_flag("O_NOFOLLOW")
    | getattr(os, "O_CLOEXEC", 0)
)
SOURCE_FILE_FLAGS = (
    os.O_RDONLY
    | required_flag("O_NOFOLLOW")
    | getattr(os, "O_CLOEXEC", 0)
    | getattr(os, "O_NONBLOCK", 0)
)
DESTINATION_FILE_FLAGS = (
    os.O_WRONLY
    | os.O_CREAT
    | os.O_EXCL
    | required_flag("O_NOFOLLOW")
    | getattr(os, "O_CLOEXEC", 0)
)
SYMLINK_FLAGS = os.O_RDONLY | O_SYMLINK | getattr(os, "O_CLOEXEC", 0)


def display(path):
    return "." if not path else os.fsdecode(path)


def descriptor_number(value, description):
    if not value or not value.isascii() or not value.isdecimal():
        raise CopyError(description + " is not a decimal descriptor")
    descriptor = int(value, 10)
    if descriptor <= 2:
        raise CopyError(description + " must not replace standard input or output")
    return descriptor


def stable_identity(info):
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_uid,
        info.st_gid,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
        getattr(info, "st_flags", 0),
        getattr(info, "st_gen", 0),
    )


def object_identity(info):
    return info.st_dev, info.st_ino, info.st_mode, info.st_uid, info.st_gid


def has_extended_acl(descriptor):
    ctypes.set_errno(0)
    acl = libc.acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED)
    if acl:
        libc.acl_free(acl)
        return True
    error = ctypes.get_errno()
    if error not in (0, errno.ENOENT):
        raise OSError(error, os.strerror(error))
    return False


def provenance(descriptor, description):
    retry_errors = {errno.ERANGE, getattr(errno, "ENOATTR", 93)}
    for _attempt in range(4):
        ctypes.set_errno(0)
        size = libc.flistxattr(descriptor, None, 0, 0)
        if size < 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        if size == 0:
            return None
        if size > 1024 * 1024:
            raise CopyError(description + " has excessive extended attributes")
        names_buffer = ctypes.create_string_buffer(size)
        actual = libc.flistxattr(descriptor, names_buffer, size, 0)
        if actual < 0:
            error = ctypes.get_errno()
            if error in retry_errors:
                continue
            raise OSError(error, os.strerror(error))
        names = set(names_buffer.raw[:actual].rstrip(b"\0").split(b"\0"))
        if names - {ALLOWED_XATTR}:
            raise CopyError(description + " has unsupported extended attributes")
        if ALLOWED_XATTR not in names:
            return None

        value_size = libc.fgetxattr(descriptor, ALLOWED_XATTR, None, 0, 0, 0)
        if value_size < 0:
            error = ctypes.get_errno()
            if error in retry_errors:
                continue
            raise OSError(error, os.strerror(error))
        if value_size > MAX_PROVENANCE_BYTES:
            raise CopyError(description + " has excessive provenance metadata")
        value_buffer = ctypes.create_string_buffer(value_size)
        value_pointer = value_buffer if value_size else None
        if libc.fgetxattr(
            descriptor, ALLOWED_XATTR, value_pointer, value_size, 0, 0
        ) != value_size:
            error = ctypes.get_errno()
            if error in retry_errors:
                continue
            raise OSError(error, os.strerror(error))

        # A second name query binds absence of every unrecognised attribute.
        final_size = libc.flistxattr(descriptor, None, 0, 0)
        if final_size != size:
            continue
        final_names = ctypes.create_string_buffer(final_size)
        if libc.flistxattr(descriptor, final_names, final_size, 0) != final_size:
            if ctypes.get_errno() in retry_errors:
                continue
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        if final_names.raw[:final_size] != names_buffer.raw[:actual]:
            continue
        return value_buffer.raw[:value_size]
    raise CopyError(description + " extended attributes changed repeatedly")


def require_plain(descriptor, expected_kind, description):
    info = os.fstat(descriptor)
    if expected_kind == "directory":
        correct_type = stat.S_ISDIR(info.st_mode)
    elif expected_kind == "file":
        correct_type = stat.S_ISREG(info.st_mode)
    elif expected_kind == "link":
        correct_type = stat.S_ISLNK(info.st_mode)
    else:
        raise AssertionError(expected_kind)
    if not correct_type:
        raise CopyError(description + " has an unexpected type")
    if info.st_uid != os.geteuid():
        raise CopyError(description + " is not owned by the effective user")
    if getattr(info, "st_flags", 0):
        raise CopyError(description + " has unsupported BSD flags")
    if has_extended_acl(descriptor):
        raise CopyError(description + " has an extended ACL")
    xattr = provenance(descriptor, description)
    return info, xattr


def set_provenance(descriptor, value, description):
    existing = provenance(descriptor, description)
    if value is None and existing is not None:
        if libc.fremovexattr(descriptor, ALLOWED_XATTR, 0) != 0:
            error = ctypes.get_errno()
            if error != getattr(errno, "ENOATTR", 93):
                raise OSError(error, os.strerror(error))
    elif value is not None and existing != value:
        data = ctypes.create_string_buffer(value) if value else None
        if libc.fsetxattr(descriptor, ALLOWED_XATTR, data, len(value), 0, 0) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
    if provenance(descriptor, description) != value:
        raise CopyError(description + " provenance metadata was not preserved")


def safe_name(name, relative):
    if (
        not name
        or name in (b".", b"..")
        or b"/" in name
        or b"\0" in name
        or any(byte < 0x20 or byte == 0x7F for byte in name)
    ):
        raise CopyError("unsafe entry name below " + repr(display(relative)))
    canonical_component(name, "entry name below " + repr(display(relative)))


def canonical_component(value, description):
    try:
        text = value.decode("utf-8", "strict")
    except UnicodeDecodeError as error:
        raise CopyError(description + " is not valid UTF-8") from error
    if unicodedata.normalize("NFC", text) != text:
        raise CopyError(description + " is not NFC-normalized")


def safe_symlink_target(target, parent_relative):
    if (
        not target
        or target.startswith(b"/")
        or b"\0" in target
        or len(target) > MAX_SYMLINK_TARGET_BYTES
        or any(byte < 0x20 or byte == 0x7F for byte in target)
    ):
        raise CopyError("unsafe symlink target below " + repr(display(parent_relative)))
    components = target.split(b"/")
    if any(not component for component in components):
        raise CopyError("symlink target contains an empty path component")
    for component in components:
        if component not in (b".", b".."):
            canonical_component(component, "symlink target component")
    resolved = posixpath.normpath(posixpath.join(parent_relative, target))
    if resolved == b".." or resolved.startswith(b"../") or resolved.startswith(b"/"):
        raise CopyError("symlink target escapes the source root")


class Budget:
    def __init__(self):
        self.entries = 0
        self.metadata_bytes = 0
        self.file_bytes = 0

    def entry(self, relative):
        if len(relative) > MAX_RELATIVE_PATH_BYTES:
            raise CopyError("relative path is too long: " + repr(display(relative)))
        self.entries += 1
        if self.entries > MAX_ENTRIES:
            raise CopyError("source tree has too many entries")
        self.metadata(len(relative))

    def metadata(self, count):
        self.metadata_bytes += count
        if self.metadata_bytes > MAX_METADATA_BYTES:
            raise CopyError("source tree metadata exceeds its resource bound")

    def file(self, count, relative):
        if count < 0 or count > MAX_FILE_BYTES:
            raise CopyError("source file is too large: " + repr(display(relative)))
        self.file_bytes += count
        if self.file_bytes > MAX_TOTAL_FILE_BYTES:
            raise CopyError("source file payload exceeds its resource bound")


class Copier:
    def __init__(self, source_fd, destination_fd):
        self.source_fd = os.dup(source_fd)
        self.destination_fd = os.dup(destination_fd)
        self.budget = Budget()
        self.source_directories = set()
        self.destination_records = {}
        self.source_root = None
        self.source_root_xattr = None
        self.destination_root_object = None
        self.destination_device = None

    def close(self):
        for attribute in ("destination_fd", "source_fd"):
            descriptor = getattr(self, attribute, -1)
            if descriptor >= 0:
                os.close(descriptor)
                setattr(self, attribute, -1)

    def names(self, descriptor, relative):
        result = []
        with os.scandir(descriptor) as entries:
            for entry in entries:
                name = os.fsencode(entry.name)
                safe_name(name, relative)
                result.append(name)
                if len(result) > MAX_DIRECTORY_ENTRIES:
                    raise CopyError(
                        "directory has too many entries: " + repr(display(relative))
                    )
        return sorted(result)

    def source_entry(self, parent_fd, name, relative):
        try:
            return os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        except OSError as error:
            raise CopyError(
                "cannot inspect source entry " + repr(display(relative)) + ": " + str(error)
            ) from error

    def open_source_directory(self, parent_fd, name, expected, relative):
        descriptor = os.open(name, DIRECTORY_FLAGS, dir_fd=parent_fd)
        try:
            current, xattr = require_plain(
                descriptor, "directory", "source directory " + repr(display(relative))
            )
            if stable_identity(current) != stable_identity(expected):
                raise CopyError("source directory changed while opening it")
            if current.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                raise CopyError("source directory is group- or other-writable")
            return descriptor, xattr
        except BaseException:
            os.close(descriptor)
            raise

    def create_destination_directory(self, parent_fd, name, relative):
        os.mkdir(name, 0o700, dir_fd=parent_fd)
        expected = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        descriptor = os.open(name, DIRECTORY_FLAGS, dir_fd=parent_fd)
        try:
            current, xattr = require_plain(
                descriptor, "directory", "destination directory " + repr(display(relative))
            )
            if (
                stable_identity(current) != stable_identity(expected)
                or current.st_dev != self.destination_device
                or stat.S_IMODE(current.st_mode) != 0o700
            ):
                raise CopyError("destination directory changed while creating it")
            return descriptor
        except BaseException:
            os.close(descriptor)
            raise

    def record_destination(self, relative, kind, descriptor, payload, source_xattr):
        current, current_xattr = require_plain(
            descriptor, kind, "destination " + kind + " " + repr(display(relative))
        )
        if (
            current.st_dev != self.destination_device
            or current_xattr != source_xattr
            or (kind != "directory" and current.st_nlink != 1)
        ):
            raise CopyError("destination entry has unsafe final metadata")
        self.destination_records[relative] = (
            kind,
            stable_identity(current),
            payload,
            source_xattr,
        )

    def copy_file(self, source_parent, destination_parent, name, relative, expected):
        if expected.st_nlink != 1:
            raise CopyError("source file is hard-linked: " + repr(display(relative)))
        self.budget.file(expected.st_size, relative)
        source = os.open(name, SOURCE_FILE_FLAGS, dir_fd=source_parent)
        destination = -1
        try:
            source_before, source_xattr = require_plain(
                source, "file", "source file " + repr(display(relative))
            )
            if stable_identity(source_before) != stable_identity(expected):
                raise CopyError("source file changed while opening it")
            if source_before.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
                raise CopyError("source file is group- or other-writable")
            self.budget.metadata(len(source_xattr) if source_xattr is not None else 0)

            destination = os.open(
                name, DESTINATION_FILE_FLAGS, 0o600, dir_fd=destination_parent
            )
            initial, initial_xattr = require_plain(
                destination, "file", "destination file " + repr(display(relative))
            )
            if (
                initial.st_dev != self.destination_device
                or initial.st_nlink != 1
                or stat.S_IMODE(initial.st_mode) != 0o600
                or initial.st_size != 0
            ):
                raise CopyError("new destination file has unsafe metadata")

            digest = hashlib.sha256()
            offset = 0
            while offset < expected.st_size:
                block = os.pread(source, min(READ_SIZE, expected.st_size - offset), offset)
                if not block:
                    raise CopyError("source file ended while copying it")
                digest.update(block)
                written = 0
                while written < len(block):
                    count = os.write(destination, block[written:])
                    if count <= 0:
                        raise CopyError("short write while copying destination file")
                    written += count
                offset += len(block)
            if os.pread(source, 1, offset):
                raise CopyError("source file grew while copying it")
            if (
                stable_identity(os.fstat(source)) != stable_identity(source_before)
                or provenance(source, "source file " + repr(display(relative)))
                != source_xattr
            ):
                raise CopyError("source file changed while copying it")
            path_after = self.source_entry(source_parent, name, relative)
            if stable_identity(path_after) != stable_identity(expected):
                raise CopyError("source file path changed while copying it")

            set_provenance(
                destination,
                source_xattr,
                "destination file " + repr(display(relative)),
            )
            os.fchmod(destination, stat.S_IMODE(expected.st_mode))
            os.fsync(destination)
            self.record_destination(
                relative, "file", destination, digest.digest(), source_xattr
            )
            destination_path = os.stat(
                name, dir_fd=destination_parent, follow_symlinks=False
            )
            if stable_identity(destination_path) != self.destination_records[relative][1]:
                raise CopyError("destination file path changed after copying it")
        finally:
            if destination >= 0:
                os.close(destination)
            os.close(source)

    def copy_link(self, source_parent, destination_parent, name, relative, expected, parent_relative):
        if expected.st_nlink != 1:
            raise CopyError("source symlink is hard-linked: " + repr(display(relative)))
        source = os.open(name, SYMLINK_FLAGS, dir_fd=source_parent)
        destination = -1
        try:
            source_before, source_xattr = require_plain(
                source, "link", "source symlink " + repr(display(relative))
            )
            if stable_identity(source_before) != stable_identity(expected):
                raise CopyError("source symlink changed while opening it")
            target = os.fsencode(os.readlink(name, dir_fd=source_parent))
            safe_symlink_target(target, parent_relative)
            self.budget.metadata(len(target) + (len(source_xattr) if source_xattr else 0))

            os.symlink(target, name, dir_fd=destination_parent)
            destination_expected = os.stat(
                name, dir_fd=destination_parent, follow_symlinks=False
            )
            destination = os.open(name, SYMLINK_FLAGS, dir_fd=destination_parent)
            destination_before, destination_xattr = require_plain(
                destination, "link", "destination symlink " + repr(display(relative))
            )
            if (
                stable_identity(destination_before) != stable_identity(destination_expected)
                or destination_before.st_dev != self.destination_device
                or destination_before.st_nlink != 1
                or stat.S_IMODE(destination_before.st_mode) != stat.S_IMODE(expected.st_mode)
            ):
                raise CopyError("new destination symlink has unsafe metadata")
            set_provenance(
                destination,
                source_xattr,
                "destination symlink " + repr(display(relative)),
            )

            if (
                os.fsencode(os.readlink(name, dir_fd=source_parent)) != target
                or stable_identity(os.fstat(source)) != stable_identity(source_before)
                or provenance(source, "source symlink " + repr(display(relative)))
                != source_xattr
                or stable_identity(self.source_entry(source_parent, name, relative))
                != stable_identity(expected)
            ):
                raise CopyError("source symlink changed while copying it")
            self.record_destination(
                relative, "link", destination, target, source_xattr
            )
            if os.fsencode(os.readlink(name, dir_fd=destination_parent)) != target:
                raise CopyError("destination symlink target changed after copying it")
        finally:
            if destination >= 0:
                os.close(destination)
            os.close(source)

    def copy_directory_contents(
        self, source, destination, relative, depth, expected, expected_xattr
    ):
        if depth > MAX_DEPTH:
            raise CopyError("source directory depth exceeds its resource bound")
        key = expected.st_dev, expected.st_ino
        if key in self.source_directories:
            raise CopyError("source directory cycle or alias detected")
        self.source_directories.add(key)
        self.budget.metadata(len(expected_xattr) if expected_xattr is not None else 0)

        names = self.names(source, relative)
        for name in names:
            child_relative = name if not relative else relative + b"/" + name
            self.budget.entry(child_relative)
            child = self.source_entry(source, name, child_relative)
            if child.st_dev != self.source_root.st_dev:
                raise CopyError("source tree crosses a filesystem boundary")
            if stat.S_ISDIR(child.st_mode):
                source_child, source_xattr = self.open_source_directory(
                    source, name, child, child_relative
                )
                destination_child = -1
                try:
                    destination_child = self.create_destination_directory(
                        destination, name, child_relative
                    )
                    self.copy_directory_contents(
                        source_child,
                        destination_child,
                        child_relative,
                        depth + 1,
                        child,
                        source_xattr,
                    )
                finally:
                    if destination_child >= 0:
                        os.close(destination_child)
                    os.close(source_child)
            elif stat.S_ISREG(child.st_mode):
                self.copy_file(source, destination, name, child_relative, child)
            elif stat.S_ISLNK(child.st_mode):
                self.copy_link(
                    source, destination, name, child_relative, child, relative
                )
            else:
                raise CopyError(
                    "unsupported source entry type: " + repr(display(child_relative))
                )

            if stable_identity(self.source_entry(source, name, child_relative)) != stable_identity(child):
                raise CopyError("source entry changed during traversal")

        if self.names(source, relative) != names:
            raise CopyError("source directory entries changed during traversal")
        source_after, source_xattr_after = require_plain(
            source, "directory", "source directory " + repr(display(relative))
        )
        if (
            stable_identity(source_after) != stable_identity(expected)
            or source_xattr_after != expected_xattr
        ):
            raise CopyError("source directory changed during traversal")

        if self.names(destination, relative) != names:
            raise CopyError("destination directory has unexpected entries")
        set_provenance(
            destination,
            expected_xattr,
            "destination directory " + repr(display(relative)),
        )
        os.fchmod(destination, stat.S_IMODE(expected.st_mode))
        os.fsync(destination)
        self.record_destination(
            relative, "directory", destination, b"", expected_xattr
        )

    def verify_destination_file(self, descriptor, expected_digest, expected_size, relative):
        digest = hashlib.sha256()
        offset = 0
        while offset < expected_size:
            block = os.pread(descriptor, min(READ_SIZE, expected_size - offset), offset)
            if not block:
                raise CopyError("destination file ended during final validation")
            digest.update(block)
            offset += len(block)
        if os.pread(descriptor, 1, offset) or digest.digest() != expected_digest:
            raise CopyError(
                "destination file content changed: " + repr(display(relative))
            )

    def verify_destination(self):
        visited = set()

        def visit(descriptor, relative, depth):
            if depth > MAX_DEPTH:
                raise CopyError("destination directory depth changed")
            names = self.names(descriptor, relative)
            expected_names = sorted(
                path[len(relative) + (1 if relative else 0):].split(b"/", 1)[0]
                for path in self.destination_records
                if path != relative
                and (not relative or path.startswith(relative + b"/"))
                and b"/" not in path[len(relative) + (1 if relative else 0):]
            )
            if names != expected_names:
                raise CopyError("destination directory changed before final validation")

            for name in names:
                child_relative = name if not relative else relative + b"/" + name
                kind, expected_identity, payload, expected_xattr = self.destination_records[
                    child_relative
                ]
                flags = DIRECTORY_FLAGS if kind == "directory" else (
                    SOURCE_FILE_FLAGS if kind == "file" else SYMLINK_FLAGS
                )
                child = os.open(name, flags, dir_fd=descriptor)
                try:
                    before, current_xattr = require_plain(
                        child,
                        kind,
                        "destination " + kind + " " + repr(display(child_relative)),
                    )
                    if (
                        stable_identity(before) != expected_identity
                        or current_xattr != expected_xattr
                    ):
                        raise CopyError("destination entry changed before final validation")
                    path_info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
                    if stable_identity(path_info) != expected_identity:
                        raise CopyError("destination path changed before final validation")
                    if kind == "directory":
                        visit(child, child_relative, depth + 1)
                    elif kind == "file":
                        self.verify_destination_file(
                            child, payload, before.st_size, child_relative
                        )
                    elif os.fsencode(os.readlink(name, dir_fd=descriptor)) != payload:
                        raise CopyError("destination symlink target changed")
                    after, final_xattr = require_plain(
                        child,
                        kind,
                        "destination " + kind + " " + repr(display(child_relative)),
                    )
                    if (
                        stable_identity(after) != expected_identity
                        or final_xattr != expected_xattr
                    ):
                        raise CopyError("destination entry changed during final validation")
                    visited.add(child_relative)
                finally:
                    os.close(child)

        visit(self.destination_fd, b"", 0)
        if visited != set(self.destination_records) - {b""}:
            raise CopyError("destination validation did not cover every copied entry")
        root_record = self.destination_records[b""]
        current, current_xattr = require_plain(
            self.destination_fd, "directory", "destination root"
        )
        if stable_identity(current) != root_record[1] or current_xattr != root_record[3]:
            raise CopyError("destination root changed during final validation")

    def run(self):
        source, source_xattr = require_plain(self.source_fd, "directory", "source root")
        destination, destination_xattr = require_plain(
            self.destination_fd, "directory", "destination root"
        )
        if object_identity(source) == object_identity(destination):
            raise CopyError("source and destination descriptors select the same directory")
        if source.st_uid != os.geteuid() or destination.st_uid != os.geteuid():
            raise CopyError("tree roots are not owned by the effective user")
        if (
            stat.S_IMODE(destination.st_mode) != 0o700
            or self.names(self.destination_fd, b"")
        ):
            raise CopyError("destination root is not a plain empty mode-0700 directory")
        if source.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise CopyError("source root is group- or other-writable")

        self.source_root = source
        self.source_root_xattr = source_xattr
        self.destination_root_object = object_identity(destination)
        self.destination_device = destination.st_dev
        self.copy_directory_contents(
            self.source_fd,
            self.destination_fd,
            b"",
            0,
            source,
            source_xattr,
        )
        self.verify_destination()
        source_after, source_xattr_after = require_plain(
            self.source_fd, "directory", "source root"
        )
        if (
            stable_identity(source_after) != stable_identity(source)
            or source_xattr_after != source_xattr
        ):
            raise CopyError("source root changed before copy completion")
        os.fsync(self.destination_fd)


def main(argv):
    if sys.platform != "darwin":
        raise CopyError("fd tree copy requires Darwin")
    if not sys.flags.isolated:
        raise CopyError("invoke fd tree copy with the system Python in isolated mode")
    if len(argv) != 3:
        raise CopyError("usage: fd_tree_copy.py SOURCE_FD DESTINATION_FD")
    source_fd = descriptor_number(argv[1], "source fd")
    destination_fd = descriptor_number(argv[2], "destination fd")
    if source_fd == destination_fd:
        raise CopyError("source and destination fd roles overlap")

    # Clearing here does not replace the caller's pre-launch env -i boundary;
    # it prevents later stdlib operations from consulting launcher-added state.
    os.environ.clear()
    copier = Copier(source_fd, destination_fd)
    try:
        copier.run()
    finally:
        copier.close()


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (CopyError, OSError) as error:
        print("fd-tree-copy: " + str(error), file=sys.stderr)
        raise SystemExit(1)
