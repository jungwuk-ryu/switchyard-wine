#!/usr/bin/env python3
"""Produce a descriptor-bound inventory for a private release runtime tree."""

import hashlib
import importlib.util
import os
import stat
import struct
import sys

_helper_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), "fd_tree_copy.py")
_helper_spec = importlib.util.spec_from_file_location("switchyard_fd_tree_copy", _helper_path)
if _helper_spec is None or _helper_spec.loader is None:
    raise RuntimeError("cannot load the descriptor-safe tree helper")
_helper = importlib.util.module_from_spec(_helper_spec)
_helper_spec.loader.exec_module(_helper)

DIRECTORY_FLAGS = _helper.DIRECTORY_FLAGS
MAX_DEPTH = _helper.MAX_DEPTH
MAX_DIRECTORY_ENTRIES = _helper.MAX_DIRECTORY_ENTRIES
MAX_ENTRIES = _helper.MAX_ENTRIES
MAX_FILE_BYTES = _helper.MAX_FILE_BYTES
MAX_RELATIVE_PATH_BYTES = _helper.MAX_RELATIVE_PATH_BYTES
MAX_TOTAL_FILE_BYTES = _helper.MAX_TOTAL_FILE_BYTES
SOURCE_FILE_FLAGS = _helper.SOURCE_FILE_FLAGS
SYMLINK_FLAGS = _helper.SYMLINK_FLAGS
CopyError = _helper.CopyError
provenance = _helper.provenance
require_plain = _helper.require_plain
safe_name = _helper.safe_name
safe_symlink_target = _helper.safe_symlink_target
stable_identity = _helper.stable_identity


READ_SIZE = 1024 * 1024
MAX_FAT_SLICES = 64
MAX_LOAD_COMMANDS = 100_000


class InventoryError(CopyError):
    pass


def descriptor_number(value):
    if not value or not value.isascii() or not value.isdecimal():
        raise InventoryError("runtime fd is not a decimal descriptor")
    descriptor = int(value, 10)
    if descriptor <= 2:
        raise InventoryError("runtime fd must not replace standard input or output")
    return descriptor


def update_record(digest, *fields):
    for field in fields:
        if isinstance(field, str):
            field = field.encode("utf-8")
        digest.update(len(field).to_bytes(8, "big"))
        digest.update(field)


def read_exact(descriptor, count, offset):
    if count < 0:
        return None
    value = bytearray()
    while len(value) < count:
        block = os.pread(descriptor, count - len(value), offset + len(value))
        if not block:
            return None
        value.extend(block)
    return bytes(value)


def valid_thin_macho(descriptor, offset, size):
    magic = read_exact(descriptor, 4, offset)
    layouts = {
        b"\xfe\xed\xfa\xce": (">", 28, 4),
        b"\xce\xfa\xed\xfe": ("<", 28, 4),
        b"\xfe\xed\xfa\xcf": (">", 32, 8),
        b"\xcf\xfa\xed\xfe": ("<", 32, 8),
    }
    layout = layouts.get(magic)
    if layout is None:
        return False
    endian, header_size, command_alignment = layout
    header = read_exact(descriptor, header_size, offset)
    if header is None or size < header_size:
        return False
    values = struct.unpack(endian + ("8I" if header_size == 32 else "7I"), header)
    _magic, cpu_type, _cpu_subtype, file_type, command_count, command_bytes, _flags = values[:7]
    if (
        not cpu_type
        or file_type < 1
        or file_type > 12
        or command_count > MAX_LOAD_COMMANDS
        or command_count * 8 > command_bytes
        or command_bytes > size - header_size
    ):
        return False
    command_offset = offset + header_size
    command_end = command_offset + command_bytes
    for _unused in range(command_count):
        command = read_exact(descriptor, 8, command_offset)
        if command is None:
            return False
        _command_type, command_size = struct.unpack(endian + "2I", command)
        if (
            command_size < 8
            or command_size % command_alignment
            or command_size > command_end - command_offset
        ):
            return False
        command_offset += command_size
    return command_offset == command_end


def valid_macho(descriptor, size):
    magic = read_exact(descriptor, 4, 0)
    if magic in {
        b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    }:
        return valid_thin_macho(descriptor, 0, size)
    fat_layouts = {
        b"\xca\xfe\xba\xbe": (">", False),
        b"\xbe\xba\xfe\xca": ("<", False),
        b"\xca\xfe\xba\xbf": (">", True),
        b"\xbf\xba\xfe\xca": ("<", True),
    }
    layout = fat_layouts.get(magic)
    if layout is None:
        return False
    endian, is_64 = layout
    header = read_exact(descriptor, 8, 0)
    if header is None:
        return False
    _magic, slice_count = struct.unpack(endian + "2I", header)
    record_size = 32 if is_64 else 20
    if not slice_count or slice_count > MAX_FAT_SLICES or slice_count * record_size > size - 8:
        return False
    ranges = []
    architectures = set()
    for index in range(slice_count):
        record = read_exact(descriptor, record_size, 8 + index * record_size)
        if record is None:
            return False
        if is_64:
            cpu_type, cpu_subtype, slice_offset, slice_size, alignment, reserved = struct.unpack(
                endian + "2I2Q2I", record
            )
            if reserved:
                return False
        else:
            cpu_type, cpu_subtype, slice_offset, slice_size, alignment = struct.unpack(
                endian + "5I", record
            )
        if (
            not cpu_type
            or (cpu_type, cpu_subtype) in architectures
            or alignment > 31
            or slice_offset < 8 + slice_count * record_size
            or not slice_size
            or slice_offset > size
            or slice_size > size - slice_offset
            or slice_offset % (1 << alignment)
            or not valid_thin_macho(descriptor, slice_offset, slice_size)
        ):
            return False
        architectures.add((cpu_type, cpu_subtype))
        ranges.append((slice_offset, slice_offset + slice_size))
    ranges.sort()
    return all(left[1] <= right[0] for left, right in zip(ranges, ranges[1:]))


class Inventory:
    def __init__(self, root_fd, excluded):
        self.root_fd = os.dup(root_fd)
        self.excluded = excluded
        self.seen_excluded = set()
        self.directory_ids = set()
        self.entry_count = 0
        self.total_bytes = 0
        self.shape = hashlib.sha256()
        self.all_content = hashlib.sha256()
        self.non_macho = hashlib.sha256()
        self.protected = hashlib.sha256()
        self.machos = []
        self.root_device = None

    def close(self):
        if self.root_fd >= 0:
            os.close(self.root_fd)
            self.root_fd = -1

    def names(self, descriptor, relative):
        names = []
        with os.scandir(descriptor) as entries:
            for entry in entries:
                name = os.fsencode(entry.name)
                safe_name(name, relative)
                names.append(name)
                if len(names) > MAX_DIRECTORY_ENTRIES:
                    raise InventoryError("directory has too many entries")
        return sorted(names)

    def count(self, relative):
        self.entry_count += 1
        if self.entry_count > MAX_ENTRIES:
            raise InventoryError("runtime tree has too many entries")
        if len(relative) > MAX_RELATIVE_PATH_BYTES:
            raise InventoryError("runtime relative path is too long")

    def record_shape(self, kind, relative, mode):
        update_record(self.shape, kind, relative, oct(mode))

    def record_content(self, kind, relative, mode, payload, macho=False):
        fields = (kind, relative, oct(mode), payload)
        update_record(self.all_content, *fields)
        if not macho:
            update_record(self.non_macho, *fields)
        if relative in self.excluded:
            self.seen_excluded.add(relative)
        else:
            update_record(self.protected, *fields)

    def hash_file(self, descriptor, size):
        digest = hashlib.sha256()
        offset = 0
        while offset < size:
            block = os.pread(descriptor, min(READ_SIZE, size - offset), offset)
            if not block:
                raise InventoryError("runtime file ended while hashing")
            digest.update(block)
            offset += len(block)
        if os.pread(descriptor, 1, offset):
            raise InventoryError("runtime file grew while hashing")
        return digest.digest()

    def visit(self, descriptor, relative, expected, expected_xattr, depth):
        if depth > MAX_DEPTH:
            raise InventoryError("runtime directory depth exceeds its bound")
        current, current_xattr = require_plain(
            descriptor, "directory", "runtime directory " + repr(os.fsdecode(relative))
        )
        if (
            stable_identity(current) != stable_identity(expected)
            or current_xattr != expected_xattr
            or current.st_dev != self.root_device
            or current.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
        ):
            raise InventoryError("runtime directory changed or is writable")
        directory_id = current.st_dev, current.st_ino
        if directory_id in self.directory_ids:
            raise InventoryError("runtime directory alias or cycle detected")
        self.directory_ids.add(directory_id)
        self.count(relative)
        self.record_shape(b"D", relative, stat.S_IMODE(current.st_mode))
        self.record_content(b"D", relative, stat.S_IMODE(current.st_mode), b"")

        names = self.names(descriptor, relative)
        for name in names:
            child_relative = name if not relative else relative + b"/" + name
            child = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            if child.st_dev != self.root_device:
                raise InventoryError("runtime tree crosses a filesystem boundary")
            if stat.S_ISDIR(child.st_mode):
                child_fd = os.open(name, DIRECTORY_FLAGS, dir_fd=descriptor)
                try:
                    opened, child_xattr = require_plain(
                        child_fd, "directory", "runtime directory " + repr(os.fsdecode(child_relative))
                    )
                    if stable_identity(opened) != stable_identity(child):
                        raise InventoryError("runtime directory changed while opening")
                    self.visit(child_fd, child_relative, child, child_xattr, depth + 1)
                finally:
                    os.close(child_fd)
            elif stat.S_ISREG(child.st_mode):
                self.visit_file(descriptor, name, child_relative, child)
            elif stat.S_ISLNK(child.st_mode):
                self.visit_link(descriptor, name, child_relative, child, relative)
            else:
                raise InventoryError("runtime contains an unsupported entry type")
            path_after = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            if stable_identity(path_after) != stable_identity(child):
                raise InventoryError("runtime entry changed during traversal")
        if self.names(descriptor, relative) != names:
            raise InventoryError("runtime directory entries changed during traversal")
        after, after_xattr = require_plain(
            descriptor, "directory", "runtime directory " + repr(os.fsdecode(relative))
        )
        if stable_identity(after) != stable_identity(current) or after_xattr != current_xattr:
            raise InventoryError("runtime directory changed during inventory")

    def visit_file(self, parent, name, relative, expected):
        if expected.st_nlink != 1 or expected.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise InventoryError("runtime file is hard-linked or writable")
        if expected.st_size < 0 or expected.st_size > MAX_FILE_BYTES:
            raise InventoryError("runtime file exceeds its size bound")
        self.total_bytes += expected.st_size
        if self.total_bytes > MAX_TOTAL_FILE_BYTES:
            raise InventoryError("runtime payload exceeds its size bound")
        descriptor = os.open(name, SOURCE_FILE_FLAGS, dir_fd=parent)
        try:
            before, xattr = require_plain(
                descriptor, "file", "runtime file " + repr(os.fsdecode(relative))
            )
            if stable_identity(before) != stable_identity(expected):
                raise InventoryError("runtime file changed while opening")
            digest = self.hash_file(descriptor, before.st_size)
            macho = valid_macho(descriptor, before.st_size)
            if (
                stable_identity(os.fstat(descriptor)) != stable_identity(before)
                or provenance(descriptor, "runtime file") != xattr
            ):
                raise InventoryError("runtime file changed while hashing")
        finally:
            os.close(descriptor)
        self.count(relative)
        mode = stat.S_IMODE(expected.st_mode)
        self.record_shape(b"M" if macho else b"F", relative, mode)
        self.record_content(b"M" if macho else b"F", relative, mode, digest, macho)
        if macho:
            self.machos.append((relative, digest.hex(), expected.st_size, mode))

    def visit_link(self, parent, name, relative, expected, parent_relative):
        if expected.st_nlink != 1:
            raise InventoryError("runtime symlink is hard-linked")
        descriptor = os.open(name, SYMLINK_FLAGS, dir_fd=parent)
        try:
            before, xattr = require_plain(
                descriptor, "link", "runtime symlink " + repr(os.fsdecode(relative))
            )
            if stable_identity(before) != stable_identity(expected):
                raise InventoryError("runtime symlink changed while opening")
            target = os.fsencode(os.readlink(name, dir_fd=parent))
            safe_symlink_target(target, parent_relative)
            if (
                stable_identity(os.fstat(descriptor)) != stable_identity(before)
                or provenance(descriptor, "runtime symlink") != xattr
            ):
                raise InventoryError("runtime symlink changed while reading")
        finally:
            os.close(descriptor)
        self.count(relative)
        mode = stat.S_IMODE(expected.st_mode)
        self.record_shape(b"L", relative, mode)
        self.record_content(b"L", relative, mode, target)

    def run(self):
        root, root_xattr = require_plain(self.root_fd, "directory", "runtime root")
        if root.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise InventoryError("runtime root is group- or other-writable")
        self.root_device = root.st_dev
        self.visit(self.root_fd, b"", root, root_xattr, 0)
        if self.seen_excluded != self.excluded:
            missing = sorted(self.excluded - self.seen_excluded)
            raise InventoryError("excluded runtime path is absent: " + repr(missing[0]))
        print("VERSION\t1")
        print("SHAPE\t" + self.shape.hexdigest())
        if self.excluded:
            print("PROTECTED\t" + self.protected.hexdigest())
            for relative in sorted(self.excluded):
                print("EXCLUDED\t" + os.fsdecode(relative))
        else:
            print("ALL\t" + self.all_content.hexdigest())
            print("NONMACHO\t" + self.non_macho.hexdigest())
        for relative, digest, size, mode in self.machos:
            print("MACHO\t{}\t{}\t{:o}\t{}".format(
                digest, size, mode, os.fsdecode(relative)
            ))


def main(argv):
    if sys.platform != "darwin":
        raise InventoryError("release tree inventory requires Darwin")
    if not sys.flags.isolated:
        raise InventoryError("invoke release tree inventory in isolated mode")
    if len(argv) < 2:
        raise InventoryError("usage: release_tree_inventory.py FD [--exclude PATH ...]")
    descriptor = descriptor_number(argv[1])
    excluded = set()
    index = 2
    while index < len(argv):
        if argv[index] != "--exclude" or index + 1 >= len(argv):
            raise InventoryError("release tree inventory arguments are malformed")
        raw = os.fsencode(argv[index + 1])
        if (
            not raw
            or raw.startswith(b"/")
            or os.path.normpath(raw) != raw
            or raw in (b".", b"..")
            or raw.startswith(b"../")
        ):
            raise InventoryError("excluded runtime path is unsafe")
        for component in raw.split(b"/"):
            safe_name(component, b"")
        if raw in excluded:
            raise InventoryError("excluded runtime path is duplicated")
        excluded.add(raw)
        index += 2
    inventory = Inventory(descriptor, excluded)
    try:
        inventory.run()
    finally:
        inventory.close()


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (InventoryError, OSError, UnicodeError, ValueError) as error:
        print("release-tree-inventory: " + str(error), file=sys.stderr)
        raise SystemExit(1)
