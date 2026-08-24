"""Execute a trusted tool with its working directory bound to an open fd.

Invoke this through the system interpreter in isolated mode:

    /usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        LC_ALL=C LANG=C TMPDIR=/private/tmp \
        /usr/bin/python3 -I fd_exec.py DIRECTORY_FD \
        [--pass-fd DESCRIPTOR ...] -- /absolute/tool [arguments ...]

Clearing the environment before starting ``/usr/bin/python3`` is required:
Apple's interpreter launcher itself consumes variables such as
``DEVELOPER_DIR``, before this module can sanitize the tool environment.

The caller owns validation of tool arguments.  Every pathname that is intended
to resolve below DIRECTORY_FD must be relative and must not contain ``..``.
"""

import ctypes
import errno
import fcntl
import os
import stat
import sys


class UsageError(Exception):
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

ACL_TYPE_EXTENDED = 0x00000100
MAX_PROVENANCE_BYTES = 1024


def descriptor_number(value, description):
    if not value or not value.isascii() or not value.isdecimal():
        raise UsageError(description + " is not a decimal descriptor")
    descriptor = int(value, 10)
    if descriptor <= 2:
        raise UsageError(description + " must not replace standard input or output")
    return descriptor


def arguments(argv):
    if len(argv) < 4:
        raise UsageError("arguments are incomplete")
    directory_fd = descriptor_number(argv[1], "directory fd")
    inherited = []
    index = 2
    while index < len(argv) and argv[index] == "--pass-fd":
        if index + 1 >= len(argv):
            raise UsageError("--pass-fd has no descriptor")
        inherited.append(descriptor_number(argv[index + 1], "inherited fd"))
        index += 2
    if index >= len(argv) or argv[index] != "--" or index + 1 >= len(argv):
        raise UsageError("tool arguments must follow --")
    if directory_fd in inherited or len(inherited) != len(set(inherited)):
        raise UsageError("descriptor roles overlap")
    return directory_fd, inherited, argv[index + 1:]


def trusted_tool(path):
    if (
        not os.path.isabs(path)
        or os.path.normpath(path) != path
        or os.path.realpath(path) != path
        or "\n" in path
        or "\r" in path
    ):
        raise UsageError("tool path is not canonical and absolute")
    value = os.lstat(path)
    if (
        not stat.S_ISREG(value.st_mode)
        or value.st_uid != 0
        or value.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        or not value.st_mode & stat.S_IXUSR
    ):
        raise UsageError("tool is not a trusted system executable")
    parent = os.path.dirname(path)
    while True:
        value = os.lstat(parent)
        if (
            not stat.S_ISDIR(value.st_mode)
            or value.st_uid != 0
            or value.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        ):
            raise UsageError("tool has an unsafe executable search path")
        if parent == os.path.sep:
            break
        parent = os.path.dirname(parent)


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


def validated_xattrs(descriptor):
    retry_errors = {errno.ERANGE, getattr(errno, "ENOATTR", 93)}
    for unused in range(4):
        size = libc.flistxattr(descriptor, None, 0, 0)
        if size < 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        if size == 0:
            return None
        if size > 1024 * 1024:
            raise UsageError("working directory has excessive extended attributes")
        names_buffer = ctypes.create_string_buffer(size)
        actual = libc.flistxattr(descriptor, names_buffer, size, 0)
        if actual < 0:
            error = ctypes.get_errno()
            if error in retry_errors:
                continue
            raise OSError(error, os.strerror(error))
        names = set(names_buffer.raw[:actual].rstrip(b"\0").split(b"\0"))
        if names - {b"com.apple.provenance"}:
            raise UsageError("working directory has unsafe extended attributes")
        if b"com.apple.provenance" not in names:
            return None
        value_size = libc.fgetxattr(
            descriptor, b"com.apple.provenance", None, 0, 0, 0
        )
        if value_size < 0:
            error = ctypes.get_errno()
            if error in retry_errors:
                continue
            raise OSError(error, os.strerror(error))
        if value_size > MAX_PROVENANCE_BYTES:
            raise UsageError("working directory provenance is excessive")
        value_buffer = ctypes.create_string_buffer(value_size)
        if libc.fgetxattr(
            descriptor,
            b"com.apple.provenance",
            value_buffer,
            value_size,
            0,
            0,
        ) != value_size:
            error = ctypes.get_errno()
            if error in retry_errors:
                continue
            raise OSError(error, os.strerror(error))
        return value_buffer.raw[:value_size]
    raise UsageError("working directory extended attributes changed repeatedly")


def private_directory_state(descriptor):
    value = os.fstat(descriptor)
    if (
        not stat.S_ISDIR(value.st_mode)
        or value.st_uid != os.geteuid()
        or stat.S_IMODE(value.st_mode) != 0o700
        or getattr(value, "st_flags", 0) != 0
        or has_extended_acl(descriptor)
    ):
        raise UsageError("working directory fd is not a plain owned mode-0700 directory")
    provenance = validated_xattrs(descriptor)
    return value.st_dev, value.st_ino, provenance


def close_unlisted(inherited):
    kept = {0, 1, 2, *inherited}
    # macOS has no close_range(2), and release shells commonly have a one
    # million descriptor soft limit.  Enumerating the actual descriptor table
    # avoids a syscall per absent descriptor while still closing every leak.
    for value in os.listdir("/dev/fd"):
        if not value.isdecimal():
            continue
        descriptor = int(value, 10)
        if descriptor in kept:
            continue
        try:
            os.close(descriptor)
        except OSError:
            # listdir's own now-closed descriptor can be present in the result.
            pass


def main(argv):
    directory_fd, inherited, command = arguments(argv)
    directory_state = private_directory_state(directory_fd)
    for descriptor in inherited:
        os.fstat(descriptor)
        flags = fcntl.fcntl(descriptor, fcntl.F_GETFD)
        fcntl.fcntl(descriptor, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)
    trusted_tool(command[0])
    os.fchdir(directory_fd)
    if private_directory_state(directory_fd) != directory_state:
        raise RuntimeError("working directory changed while binding it")
    environment = {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "LC_ALL": "C",
        "LANG": "C",
        "TMPDIR": "/private/tmp",
    }
    close_unlisted(inherited)
    os.execve(command[0], command, environment)


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, RuntimeError, UsageError) as error:
        print("fd-exec: " + str(error), file=sys.stderr)
        raise SystemExit(126)
