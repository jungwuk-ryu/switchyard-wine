#!/usr/bin/env python3
"""Create and verify a deterministic digest for a Switchyard runtime tree."""

import argparse
import hashlib
import hmac
import os
import secrets
import stat
import struct
import sys


MARKER_NAME = b".switchyard-content-sha256"
FORMAT_HEADER = b"switchyard-runtime-content-v1\0"

# Runtime trees are expected to contain thousands of entries and several GiB of
# payload.  These limits leave substantial growth room while bounding memory,
# recursion, metadata, and I/O controlled by an untrusted tree.
MAX_DEPTH = 256
MAX_ROOT_COMPONENTS = 256
MAX_ROOT_PATH_BYTES = 16 * 1024
MAX_ENTRIES = 200_000
MAX_DIRECTORY_ENTRIES = 100_000
MAX_RELATIVE_PATH_BYTES = 16 * 1024
MAX_SYMLINK_TARGET_BYTES = 16 * 1024
MAX_METADATA_BYTES = 64 * 1024 * 1024
MAX_FILE_BYTES = 32 * 1024 * 1024 * 1024
MAX_TOTAL_FILE_BYTES = 128 * 1024 * 1024 * 1024
READ_SIZE = 1024 * 1024


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
    return (info.st_dev, info.st_ino, info.st_mode)


def required_open_flag(name):
    value = getattr(os, name, None)
    if value is None:
        raise DigestError(f"runtime content digest requires {name}")
    return value


def directory_open_flags():
    return (
        os.O_RDONLY
        | required_open_flag("O_DIRECTORY")
        | required_open_flag("O_NOFOLLOW")
        | getattr(os, "O_CLOEXEC", 0)
    )


def file_open_flags():
    return (
        os.O_RDONLY
        | required_open_flag("O_NOFOLLOW")
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NONBLOCK", 0)
    )


def display_relative(path):
    return "." if not path else os.fsdecode(path)


class RuntimeTree:
    """A canonical runtime root pinned through every path component."""

    def __init__(self, root_name):
        root = os.fsencode(os.path.abspath(root_name))
        if len(root) > MAX_ROOT_PATH_BYTES:
            raise DigestError(f"runtime root path exceeds {MAX_ROOT_PATH_BYTES} bytes")
        canonical = os.path.realpath(root)
        if canonical != root:
            raise DigestError(f"runtime root path is not canonical: {root_name!r}")

        self.root_name = root_name
        self.root = root
        self.components = []
        names = [component for component in root.split(b"/") if component]
        if len(names) > MAX_ROOT_COMPONENTS:
            raise DigestError(f"runtime root contains more than {MAX_ROOT_COMPONENTS} components")
        try:
            descriptor = os.open(b"/", directory_open_flags())
            try:
                info = os.fstat(descriptor)
                if not stat.S_ISDIR(info.st_mode):
                    raise DigestError("filesystem root is not a directory")
            except BaseException:
                os.close(descriptor)
                raise
            self.components.append((b"", descriptor, info))

            for name in names:
                parent = descriptor
                try:
                    child = os.open(name, directory_open_flags(), dir_fd=parent)
                except OSError as error:
                    raise DigestError(
                        "cannot open canonical runtime path component "
                        f"{os.fsdecode(name)!r}: {error}"
                    ) from error
                try:
                    info = os.fstat(child)
                    if not stat.S_ISDIR(info.st_mode):
                        raise DigestError(
                            f"runtime path component is not a directory: {os.fsdecode(name)!r}"
                        )
                except BaseException:
                    os.close(child)
                    raise
                descriptor = child
                self.components.append((name, descriptor, info))
        except BaseException:
            self.close()
            raise

    @property
    def descriptor(self):
        return self.components[-1][1]

    def close(self):
        components = getattr(self, "components", [])
        while components:
            _name, descriptor, _info = components.pop()
            try:
                os.close(descriptor)
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, _error_type, _error, _traceback):
        self.close()

    def validate_path(self, allow_root_metadata_change=False):
        """Validate pinned components and the live canonical path.

        Full ancestor identities include directory ctime, which detects a
        component that was renamed away and restored.  Marker publication is
        allowed to change only the runtime root directory metadata.
        """

        current_infos = []
        for index, (_name, descriptor, expected) in enumerate(self.components):
            try:
                current = os.fstat(descriptor)
            except OSError as error:
                raise DigestError(f"cannot recheck runtime path descriptor: {error}") from error
            if allow_root_metadata_change and index == len(self.components) - 1:
                unchanged = object_identity(current) == object_identity(expected)
            else:
                unchanged = stable_identity(current) == stable_identity(expected)
            if not unchanged:
                raise DigestError("runtime root path changed during traversal")
            current_infos.append(current)

        reopened = []
        try:
            descriptor = os.open(b"/", directory_open_flags())
            reopened.append(descriptor)
            if object_identity(os.fstat(descriptor)) != object_identity(current_infos[0]):
                raise DigestError("filesystem root changed during traversal")
            for index, (name, _pinned, _expected) in enumerate(self.components[1:], 1):
                descriptor = os.open(name, directory_open_flags(), dir_fd=descriptor)
                reopened.append(descriptor)
                if object_identity(os.fstat(descriptor)) != object_identity(current_infos[index]):
                    raise DigestError("runtime root path was replaced during traversal")
        except OSError as error:
            raise DigestError(f"cannot re-open canonical runtime root: {error}") from error
        finally:
            while reopened:
                os.close(reopened.pop())

    def rebaseline_root_after_marker_update(self):
        self.validate_path(allow_root_metadata_change=True)
        name, descriptor, _expected = self.components[-1]
        current = os.fstat(descriptor)
        if not stat.S_ISDIR(current.st_mode):
            raise DigestError("runtime root was replaced during marker update")
        self.components[-1] = (name, descriptor, current)

    def content_digest(self):
        try:
            before = os.fstat(self.descriptor)
        except OSError as error:
            raise DigestError(f"cannot inspect runtime root {self.root_name!r}: {error}") from error
        if not stat.S_ISDIR(before.st_mode):
            raise DigestError(f"runtime root is not a real directory: {self.root_name!r}")

        budget = TraversalBudget()
        records = collect_entries(self.descriptor, budget)
        try:
            after = os.fstat(self.descriptor)
        except OSError as error:
            raise DigestError(f"cannot recheck runtime root {self.root_name!r}: {error}") from error
        if stable_identity(after) != stable_identity(before):
            raise DigestError(f"runtime root changed during traversal: {self.root_name!r}")
        self.validate_path()

        digest = hashlib.sha256()
        digest.update(FORMAT_HEADER)
        frame(digest, b"directory")
        frame(digest, b"")
        frame(digest, struct.pack(">I", stat.S_IMODE(before.st_mode)))
        frame(digest, b"")
        for path, kind, mode, payload in records:
            frame(digest, kind)
            frame(digest, path)
            frame(digest, mode)
            frame(digest, payload)
        self.validate_path()
        return digest.hexdigest()


class TraversalBudget:
    def __init__(self):
        self.entries = 0
        self.metadata_bytes = 0
        self.file_bytes = 0

    def add_entry(self, relative):
        if len(relative) > MAX_RELATIVE_PATH_BYTES:
            raise DigestError(
                f"runtime relative path exceeds {MAX_RELATIVE_PATH_BYTES} bytes: "
                f"{display_relative(relative)!r}"
            )
        self.entries += 1
        if self.entries > MAX_ENTRIES:
            raise DigestError(f"runtime contains more than {MAX_ENTRIES} entries")
        self.add_metadata(len(relative))

    def add_metadata(self, size):
        self.metadata_bytes += size
        if self.metadata_bytes > MAX_METADATA_BYTES:
            raise DigestError(f"runtime metadata exceeds {MAX_METADATA_BYTES} bytes")

    def add_file(self, size, relative):
        if size < 0 or size > MAX_FILE_BYTES:
            raise DigestError(
                f"runtime file exceeds {MAX_FILE_BYTES} bytes: {display_relative(relative)!r}"
            )
        self.file_bytes += size
        if self.file_bytes > MAX_TOTAL_FILE_BYTES:
            raise DigestError(f"runtime file payload exceeds {MAX_TOTAL_FILE_BYTES} bytes")


def open_child_directory(parent, name, expected, relative):
    try:
        descriptor = os.open(name, directory_open_flags(), dir_fd=parent)
    except OSError as error:
        raise DigestError(
            f"cannot open runtime directory {display_relative(relative)!r}: {error}"
        ) from error
    try:
        current = os.fstat(descriptor)
        if (
            not stat.S_ISDIR(current.st_mode)
            or stable_identity(current) != stable_identity(expected)
        ):
            raise DigestError(
                f"runtime directory changed during traversal: {display_relative(relative)!r}"
            )
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def file_digest(parent, name, relative, expected, budget):
    budget.add_file(expected.st_size, relative)
    try:
        descriptor = os.open(name, file_open_flags(), dir_fd=parent)
    except OSError as error:
        raise DigestError(
            f"cannot open runtime file {display_relative(relative)!r}: {error}"
        ) from error

    digest = hashlib.sha256()
    bytes_read = 0
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or stable_identity(before) != stable_identity(expected):
            raise DigestError(
                f"runtime file changed during traversal: {display_relative(relative)!r}"
            )
        while True:
            chunk = os.read(descriptor, READ_SIZE)
            if not chunk:
                break
            bytes_read += len(chunk)
            if bytes_read > expected.st_size or bytes_read > MAX_FILE_BYTES:
                raise DigestError(
                    f"runtime file grew while hashing: {display_relative(relative)!r}"
                )
            digest.update(chunk)
        after = os.fstat(descriptor)
        if bytes_read != expected.st_size or stable_identity(after) != stable_identity(before):
            raise DigestError(f"runtime file changed while hashing: {display_relative(relative)!r}")
    finally:
        os.close(descriptor)

    try:
        descriptor = os.open(name, file_open_flags(), dir_fd=parent)
    except OSError as error:
        raise DigestError(
            f"cannot re-open runtime file {display_relative(relative)!r}: {error}"
        ) from error
    try:
        if stable_identity(os.fstat(descriptor)) != stable_identity(expected):
            raise DigestError(
                f"runtime file path changed while hashing: {display_relative(relative)!r}"
            )
    finally:
        os.close(descriptor)
    return digest.digest()


def directory_names(descriptor, relative):
    names = []
    try:
        with os.scandir(descriptor) as entries:
            for entry in entries:
                name = os.fsencode(entry.name)
                if not name or name in (b".", b"..") or b"/" in name or b"\0" in name:
                    raise DigestError(
                        f"invalid runtime entry name in {display_relative(relative)!r}"
                    )
                if not relative and name == MARKER_NAME:
                    continue
                names.append(name)
                if len(names) > MAX_DIRECTORY_ENTRIES:
                    raise DigestError(
                        f"runtime directory contains more than {MAX_DIRECTORY_ENTRIES} entries: "
                        f"{display_relative(relative)!r}"
                    )
    except OSError as error:
        raise DigestError(
            f"cannot list runtime directory {display_relative(relative)!r}: {error}"
        ) from error
    return sorted(names)


def collect_entries(root_descriptor, budget):
    records = []

    def visit(descriptor, relative, depth):
        if depth > MAX_DEPTH:
            raise DigestError(
                f"runtime directory depth exceeds {MAX_DEPTH}: {display_relative(relative)!r}"
            )
        try:
            before = os.fstat(descriptor)
        except OSError as error:
            raise DigestError(
                f"cannot inspect runtime directory {display_relative(relative)!r}: {error}"
            ) from error
        if not stat.S_ISDIR(before.st_mode):
            raise DigestError(
                f"runtime directory was replaced during traversal: {display_relative(relative)!r}"
            )

        for name in directory_names(descriptor, relative):
            relative_path = name if not relative else relative + b"/" + name
            budget.add_entry(relative_path)
            try:
                info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            except OSError as error:
                raise DigestError(
                    f"cannot inspect runtime entry {display_relative(relative_path)!r}: {error}"
                ) from error
            mode = struct.pack(">I", stat.S_IMODE(info.st_mode))

            if stat.S_ISDIR(info.st_mode):
                records.append((relative_path, b"directory", mode, b""))
                child = open_child_directory(descriptor, name, info, relative_path)
                try:
                    visit(child, relative_path, depth + 1)
                    if stable_identity(os.fstat(child)) != stable_identity(info):
                        raise DigestError(
                            f"runtime directory changed during traversal: "
                            f"{display_relative(relative_path)!r}"
                        )
                finally:
                    os.close(child)
            elif stat.S_ISREG(info.st_mode):
                payload = file_digest(descriptor, name, relative_path, info, budget)
                records.append((relative_path, b"file", mode, payload))
            elif stat.S_ISLNK(info.st_mode):
                try:
                    target = os.readlink(name, dir_fd=descriptor)
                    target = os.fsencode(target)
                    after = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
                except OSError as error:
                    raise DigestError(
                        f"cannot read runtime link {display_relative(relative_path)!r}: {error}"
                    ) from error
                if stable_identity(after) != stable_identity(info):
                    raise DigestError(
                        f"runtime link changed while hashing: {display_relative(relative_path)!r}"
                    )
                if len(target) > MAX_SYMLINK_TARGET_BYTES:
                    raise DigestError(
                        f"runtime link target exceeds {MAX_SYMLINK_TARGET_BYTES} bytes: "
                        f"{display_relative(relative_path)!r}"
                    )
                budget.add_metadata(len(target))
                records.append((relative_path, b"link", mode, target))
            else:
                raise DigestError(
                    f"unsupported runtime entry type: {display_relative(relative_path)!r}"
                )

            try:
                after_path = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            except OSError as error:
                raise DigestError(
                    f"cannot recheck runtime entry {display_relative(relative_path)!r}: {error}"
                ) from error
            if stable_identity(after_path) != stable_identity(info):
                raise DigestError(
                    f"runtime entry changed during traversal: {display_relative(relative_path)!r}"
                )

        try:
            after = os.fstat(descriptor)
        except OSError as error:
            raise DigestError(
                f"cannot recheck runtime directory {display_relative(relative)!r}: {error}"
            ) from error
        if stable_identity(after) != stable_identity(before):
            raise DigestError(
                f"runtime directory changed during traversal: {display_relative(relative)!r}"
            )

    visit(root_descriptor, b"", 0)
    return sorted(records, key=lambda record: record[0])


def content_digest(root_name):
    with RuntimeTree(root_name) as tree:
        return tree.content_digest()


def read_marker(tree):
    try:
        expected = os.stat(MARKER_NAME, dir_fd=tree.descriptor, follow_symlinks=False)
    except OSError as error:
        raise DigestError(f"cannot inspect runtime content marker: {error}") from error
    if not stat.S_ISREG(expected.st_mode) or stat.S_IMODE(expected.st_mode) != 0o644:
        raise DigestError("runtime content marker is not a mode-0644 regular file")

    try:
        descriptor = os.open(MARKER_NAME, file_open_flags(), dir_fd=tree.descriptor)
    except OSError as error:
        raise DigestError(f"cannot open runtime content marker: {error}") from error
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode) or stable_identity(before) != stable_identity(expected):
            raise DigestError("runtime content marker changed while opening")
        chunks = []
        remaining = 66
        while remaining:
            chunk = os.read(descriptor, remaining)
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        value = b"".join(chunks)
        after = os.fstat(descriptor)
        if stable_identity(after) != stable_identity(before):
            raise DigestError("runtime content marker changed while reading")
    finally:
        os.close(descriptor)
    return value, expected


def inspect_marker_for_write(tree):
    try:
        info = os.stat(MARKER_NAME, dir_fd=tree.descriptor, follow_symlinks=False)
    except FileNotFoundError:
        tree.validate_path()
        return None, None
    except OSError as error:
        raise DigestError(f"cannot inspect existing runtime content marker: {error}") from error

    if stat.S_ISREG(info.st_mode) and stat.S_IMODE(info.st_mode) == 0o644:
        value, opened_info = read_marker(tree)
        if stable_identity(opened_info) != stable_identity(info):
            raise DigestError("existing runtime content marker changed while inspecting")
    else:
        value = None
    tree.validate_path()
    return value, info


def require_unchanged_marker_state(tree, expected):
    try:
        current = os.stat(MARKER_NAME, dir_fd=tree.descriptor, follow_symlinks=False)
    except FileNotFoundError:
        current = None
    except OSError as error:
        raise DigestError(f"cannot recheck existing runtime content marker: {error}") from error

    if expected is None:
        unchanged = current is None
    else:
        unchanged = current is not None and stable_identity(current) == stable_identity(expected)
    if not unchanged:
        raise DigestError("existing runtime content marker changed before replacement")


def create_temporary_marker(tree, value):
    descriptor = None
    name = None
    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | required_open_flag("O_NOFOLLOW")
        | getattr(os, "O_CLOEXEC", 0)
    )
    for _attempt in range(128):
        candidate = b".switchyard-content-sha256.tmp." + secrets.token_hex(16).encode("ascii")
        try:
            descriptor = os.open(candidate, flags, 0o600, dir_fd=tree.descriptor)
            name = candidate
            break
        except FileExistsError:
            continue
        except OSError as error:
            raise DigestError(f"cannot create temporary runtime content marker: {error}") from error
    if descriptor is None or name is None:
        raise DigestError("cannot allocate a unique temporary runtime content marker")

    try:
        os.fchmod(descriptor, 0o644)
        written = 0
        while written < len(value):
            count = os.write(descriptor, value[written:])
            if count <= 0:
                raise DigestError("short write to temporary runtime content marker")
            written += count
        os.fsync(descriptor)
        expected = os.fstat(descriptor)
        if not stat.S_ISREG(expected.st_mode) or stat.S_IMODE(expected.st_mode) != 0o644:
            raise DigestError("temporary runtime content marker has unsafe metadata")
    except BaseException:
        os.close(descriptor)
        try:
            os.unlink(name, dir_fd=tree.descriptor)
        except OSError:
            pass
        raise
    os.close(descriptor)
    return name, expected


def write_marker(root_name):
    with RuntimeTree(root_name) as tree:
        hexadecimal = tree.content_digest()
        value = hexadecimal.encode("ascii") + b"\n"
        existing_value, existing_info = inspect_marker_for_write(tree)
        if existing_value == value:
            if tree.content_digest() != hexadecimal:
                raise DigestError("runtime content changed while confirming its existing marker")
            final_value, final_info = read_marker(tree)
            if (
                final_value != existing_value
                or stable_identity(final_info) != stable_identity(existing_info)
            ):
                raise DigestError("existing runtime content marker changed during verification")
            tree.validate_path()
            return hexadecimal

        temporary = None
        try:
            temporary, expected = create_temporary_marker(tree, value)
            require_unchanged_marker_state(tree, existing_info)
            os.replace(
                temporary,
                MARKER_NAME,
                src_dir_fd=tree.descriptor,
                dst_dir_fd=tree.descriptor,
            )
            temporary = None
            os.fsync(tree.descriptor)
            marker_value, marker_info = read_marker(tree)
            if marker_value != value or object_identity(marker_info) != object_identity(expected):
                raise DigestError("published runtime content marker changed during replacement")
        except OSError as error:
            raise DigestError(f"cannot publish runtime content marker: {error}") from error
        finally:
            if temporary is not None:
                try:
                    os.unlink(temporary, dir_fd=tree.descriptor)
                except OSError:
                    pass

        tree.rebaseline_root_after_marker_update()
        if tree.content_digest() != hexadecimal:
            raise DigestError("runtime content changed while publishing its digest marker")
        final_value, final_info = read_marker(tree)
        if final_value != value or stable_identity(final_info) != stable_identity(marker_info):
            raise DigestError("runtime content marker changed during post-write verification")
        tree.validate_path()
        return hexadecimal


def verify_marker(root_name, expected_digest=None):
    with RuntimeTree(root_name) as tree:
        try:
            expected, marker_info = read_marker(tree)
        except DigestError:
            return False
        if len(expected) != 65 or expected[-1:] != b"\n":
            return False
        hexadecimal = expected[:-1]
        if any(byte not in b"0123456789abcdef" for byte in hexadecimal):
            return False
        actual = tree.content_digest().encode("ascii")
        try:
            final, final_info = read_marker(tree)
        except DigestError:
            return False
        if final != expected or stable_identity(final_info) != stable_identity(marker_info):
            return False
        tree.validate_path()
        matches_marker = hmac.compare_digest(actual, hexadecimal)
        if expected_digest is None:
            return matches_marker
        if (
            not isinstance(expected_digest, str)
            or len(expected_digest) != 64
            or any(character not in "0123456789abcdef" for character in expected_digest)
        ):
            return False
        return matches_marker and hmac.compare_digest(
            actual, expected_digest.encode("ascii")
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("digest", "write", "verify"))
    parser.add_argument("root")
    parser.add_argument("expected_digest", nargs="?")
    arguments = parser.parse_args()

    try:
        if arguments.action != "verify" and arguments.expected_digest is not None:
            parser.error("expected_digest is valid only with verify")
        if arguments.action == "digest":
            print(content_digest(arguments.root))
        elif arguments.action == "write":
            print(write_marker(arguments.root))
        elif not verify_marker(arguments.root, arguments.expected_digest):
            return 1
    except (DigestError, OSError) as error:
        print(f"runtime content digest failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
