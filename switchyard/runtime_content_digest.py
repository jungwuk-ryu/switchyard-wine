#!/usr/bin/env python3
"""Create and verify a deterministic digest for a Switchyard runtime tree."""

import argparse
import hashlib
import hmac
import os
import stat
import struct
import sys
import tempfile


MARKER_NAME = b".switchyard-content-sha256"
FORMAT_HEADER = b"switchyard-runtime-content-v1\0"


class DigestError(Exception):
    pass


def frame(digest, value):
    digest.update(struct.pack(">Q", len(value)))
    digest.update(value)


def stable_identity(info):
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def file_digest(path, expected):
    flags = os.O_RDONLY
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise DigestError(f"cannot open runtime file {os.fsdecode(path)!r}: {error}") from error

    digest = hashlib.sha256()
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or stable_identity(before) != stable_identity(expected):
            raise DigestError(f"runtime file changed during traversal: {os.fsdecode(path)!r}")
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
        after = os.fstat(descriptor)
        if stable_identity(after) != stable_identity(before):
            raise DigestError(f"runtime file changed while hashing: {os.fsdecode(path)!r}")
    finally:
        os.close(descriptor)
    return digest.digest()


def collect_entries(root):
    records = []

    def visit(directory, relative):
        try:
            before = os.lstat(directory)
            entries = list(os.scandir(directory))
        except OSError as error:
            raise DigestError(f"cannot traverse runtime directory {os.fsdecode(directory)!r}: {error}") from error
        if not stat.S_ISDIR(before.st_mode):
            raise DigestError(f"runtime directory was replaced during traversal: {os.fsdecode(directory)!r}")

        for entry in entries:
            name = entry.name
            if not relative and name == MARKER_NAME:
                continue
            path = os.path.join(directory, name)
            relative_path = name if not relative else os.path.join(relative, name)
            try:
                info = entry.stat(follow_symlinks=False)
            except OSError as error:
                raise DigestError(f"cannot inspect runtime entry {os.fsdecode(path)!r}: {error}") from error
            mode = struct.pack(">I", stat.S_IMODE(info.st_mode))

            if stat.S_ISDIR(info.st_mode):
                records.append((relative_path, b"directory", mode, b""))
                visit(path, relative_path)
            elif stat.S_ISREG(info.st_mode):
                records.append((relative_path, b"file", mode, file_digest(path, info)))
            elif stat.S_ISLNK(info.st_mode):
                try:
                    target = os.readlink(path)
                    after = os.lstat(path)
                except OSError as error:
                    raise DigestError(f"cannot read runtime link {os.fsdecode(path)!r}: {error}") from error
                if stable_identity(after) != stable_identity(info):
                    raise DigestError(f"runtime link changed while hashing: {os.fsdecode(path)!r}")
                records.append((relative_path, b"link", mode, target))
            else:
                raise DigestError(f"unsupported runtime entry type: {os.fsdecode(path)!r}")

        try:
            after = os.lstat(directory)
        except OSError as error:
            raise DigestError(f"cannot recheck runtime directory {os.fsdecode(directory)!r}: {error}") from error
        if stable_identity(after) != stable_identity(before):
            raise DigestError(f"runtime directory changed during traversal: {os.fsdecode(directory)!r}")

    visit(root, b"")
    return sorted(records, key=lambda record: record[0])


def content_digest(root_name):
    root = os.fsencode(os.path.abspath(root_name))
    try:
        info = os.lstat(root)
    except OSError as error:
        raise DigestError(f"cannot inspect runtime root {root_name!r}: {error}") from error
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise DigestError(f"runtime root is not a real directory: {root_name!r}")

    digest = hashlib.sha256()
    digest.update(FORMAT_HEADER)
    frame(digest, b"directory")
    frame(digest, b"")
    frame(digest, struct.pack(">I", stat.S_IMODE(info.st_mode)))
    frame(digest, b"")
    for path, kind, mode, payload in collect_entries(root):
        frame(digest, kind)
        frame(digest, path)
        frame(digest, mode)
        frame(digest, payload)
    try:
        after = os.lstat(root)
    except OSError as error:
        raise DigestError(f"cannot recheck runtime root {root_name!r}: {error}") from error
    if stable_identity(after) != stable_identity(info):
        raise DigestError(f"runtime root changed during traversal: {root_name!r}")
    return digest.hexdigest()


def marker_path(root_name):
    return os.path.join(os.path.abspath(root_name), os.fsdecode(MARKER_NAME))


def write_marker(root_name):
    hexadecimal = content_digest(root_name)
    value = hexadecimal.encode("ascii") + b"\n"
    root = os.path.abspath(root_name)
    marker = marker_path(root)
    descriptor = None
    temporary = None
    try:
        descriptor, temporary = tempfile.mkstemp(prefix=".switchyard-content-sha256.", dir=root)
        os.fchmod(descriptor, 0o644)
        with os.fdopen(descriptor, "wb", closefd=True) as stream:
            descriptor = None
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, marker)
        temporary = None
        directory = os.open(root, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary is not None:
            os.unlink(temporary)
    return hexadecimal


def verify_marker(root_name):
    marker = marker_path(root_name)
    descriptor = None
    try:
        info = os.lstat(marker)
        if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) != 0o644:
            return False
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(marker, flags)
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or stable_identity(before) != stable_identity(info):
            return False
        expected = os.read(descriptor, 66)
        after = os.fstat(descriptor)
        if stable_identity(after) != stable_identity(before):
            return False
    except OSError:
        return False
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if len(expected) != 65 or expected[-1:] != b"\n":
        return False
    hexadecimal = expected[:-1]
    if any(byte not in b"0123456789abcdef" for byte in hexadecimal):
        return False
    return hmac.compare_digest(content_digest(root_name).encode("ascii"), hexadecimal)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("digest", "write", "verify"))
    parser.add_argument("root")
    arguments = parser.parse_args()

    try:
        if arguments.action == "digest":
            print(content_digest(arguments.root))
        elif arguments.action == "write":
            print(write_marker(arguments.root))
        elif not verify_marker(arguments.root):
            return 1
    except (DigestError, OSError) as error:
        print(f"runtime content digest failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
