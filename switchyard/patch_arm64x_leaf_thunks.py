#!/usr/bin/env python3
"""Patch audited x64 leaf thunks in ARM64X ucrtbase.dll.

The default x64 thunks enter ARM64EC implementations, which is correct but
expensive for the small audited CRT rotate helpers.  This tool rewrites only
those known-equivalent leaf thunks, leaving the ARM64EC implementation and
module identity intact.
"""

import argparse
import fcntl
import os
import stat
import struct
import sys


IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
IMAGE_DLLCHARACTERISTICS_PREFER_X64_EMULATION = 0x0008
IMAGE_DYNAMIC_RELOCATION_ARM64X = 6
IMAGE_SYM_CLASS_EXTERNAL = 2
IMAGE_SYM_DTYPE_FUNCTION = 0x20
PE32_PLUS_MAGIC = 0x20B

MAX_IMAGE_BYTES = 128 * 1024 * 1024
MAX_VIRTUAL_IMAGE_BYTES = 256 * 1024 * 1024
MAX_SECTIONS = 96
MAX_SYMBOLS = 500_000
MAX_EXPORTS = 100_000
MAX_STRING_BYTES = 16 * 1024 * 1024
THUNK_SIZE = 16

OLD_THUNK_PREFIX = bytes.fromhex("48 8b c4 48 89 58 20 55 5d")
PATCH_ROTL32 = bytes.fromhex("89 c8 89 d1 d3 c0 c3") + b"\xcc" * 9
PATCH_ROTR32 = bytes.fromhex("89 c8 89 d1 d3 c8 c3") + b"\xcc" * 9
PATCH_ROTL64 = bytes.fromhex("48 89 c8 89 d1 48 d3 c0 c3") + b"\xcc" * 7
PATCH_ROTR64 = bytes.fromhex("48 89 c8 89 d1 48 d3 c8 c3") + b"\xcc" * 7

AUDITED_THUNKS = (
    ("_rotl", b"#MSVCRT__rotl", PATCH_ROTL32),
    ("_lrotl", b"#MSVCRT__lrotl", PATCH_ROTL32),
    ("_rotr", b"#MSVCRT__rotr", PATCH_ROTR32),
    ("_lrotr", b"#MSVCRT__lrotr", PATCH_ROTR32),
    ("_rotl64", b"#MSVCRT__rotl64", PATCH_ROTL64),
    ("_rotr64", b"#MSVCRT__rotr64", PATCH_ROTR64),
)


class PatchError(Exception):
    pass


class Section:
    def __init__(self, index, name, virtual_size, virtual_address,
                 raw_size, raw_pointer):
        self.index = index
        self.name = name
        self.virtual_size = virtual_size
        self.virtual_address = virtual_address
        self.raw_size = raw_size
        self.raw_pointer = raw_pointer

    def contains_rva(self, rva, size):
        span = max(self.virtual_size, self.raw_size)
        return (
            rva >= self.virtual_address and
            size <= span and
            rva - self.virtual_address <= span - size
        )

    def file_offset(self, rva, size):
        if not self.contains_rva(rva, size):
            raise PatchError(f"RVA {rva:#x} is outside section {self.display_name()}")
        delta = rva - self.virtual_address
        if size > self.raw_size or delta > self.raw_size - size:
            raise PatchError(
                f"RVA {rva:#x} is not backed by raw data in section {self.display_name()}"
            )
        return self.raw_pointer + delta

    def display_name(self):
        return self.name.decode("ascii", "replace")


class Symbol:
    def __init__(self, name, value, section_number, symbol_type,
                 storage_class):
        self.name = name
        self.value = value
        self.section_number = section_number
        self.symbol_type = symbol_type
        self.storage_class = storage_class


def read_u16(data, offset, label):
    require_range(data, offset, 2, label)
    return struct.unpack_from("<H", data, offset)[0]


def read_i16(data, offset, label):
    require_range(data, offset, 2, label)
    return struct.unpack_from("<h", data, offset)[0]


def read_u32(data, offset, label):
    require_range(data, offset, 4, label)
    return struct.unpack_from("<I", data, offset)[0]


def read_i32(data, offset, label):
    require_range(data, offset, 4, label)
    return struct.unpack_from("<i", data, offset)[0]


def require_range(data, offset, size, label):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise PatchError(f"{label} is outside the file bounds")


def checked_add(value, addition, limit, label):
    if addition < 0 or value > limit - addition:
        raise PatchError(f"{label} exceeds the file bounds")
    return value + addition


def read_regular_file(path, writable):
    basename = os.path.basename(path).lower()
    if basename != "ucrtbase.dll":
        raise PatchError(f"refusing to patch unsupported image name: {path!r}")
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise PatchError("O_NOFOLLOW is required to open the PE image safely")
    flags = (os.O_RDWR if writable else os.O_RDONLY) | nofollow
    flags |= getattr(os, "O_CLOEXEC", 0)

    descriptor = os.open(path, flags)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX if writable else fcntl.LOCK_SH)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            raise PatchError(f"image is not a regular file: {path!r}")
        if info.st_size <= 0 or info.st_size > MAX_IMAGE_BYTES:
            raise PatchError(
                f"image size {info.st_size} is outside the supported bound"
            )
        chunks = []
        remaining = info.st_size
        while remaining:
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                raise PatchError("image ended while reading")
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
        return descriptor, info, bytearray(data)
    except BaseException:
        os.close(descriptor)
        raise


class PEImage:
    def __init__(self, data):
        self.data = data
        self.sections = []
        self.sections_by_name = {}
        self.symbols = {}
        self.exports = {}
        self.hybrid_exports = {}
        self._parse_headers()
        self._parse_exports()
        self._parse_hybrid_exports()
        self._parse_symbols()

    def _parse_headers(self):
        data = self.data
        require_range(data, 0, 0x40, "DOS header")
        if data[:2] != b"MZ":
            raise PatchError("image is not a DOS/PE file")
        pe_offset = read_u32(data, 0x3C, "PE header offset")
        require_range(data, pe_offset, 4 + 20, "PE header")
        if data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise PatchError("image is missing the PE signature")

        coff = pe_offset + 4
        machine = read_u16(data, coff, "COFF machine")
        if machine != IMAGE_FILE_MACHINE_ARM64:
            raise PatchError(f"expected ARM64 image machine 0xaa64, found {machine:#x}")
        self.coff_offset = coff
        section_count = read_u16(data, coff + 2, "COFF section count")
        if not 1 <= section_count <= MAX_SECTIONS:
            raise PatchError(f"unsupported section count: {section_count}")
        self.symbol_table_offset = read_u32(data, coff + 8, "COFF symbol table")
        self.symbol_count = read_u32(data, coff + 12, "COFF symbol count")
        if self.symbol_count > MAX_SYMBOLS:
            raise PatchError(f"unsupported symbol count: {self.symbol_count}")
        optional_size = read_u16(data, coff + 16, "COFF optional header size")
        optional = coff + 20
        require_range(data, optional, optional_size, "optional header")
        if optional_size < 0x78:
            raise PatchError("optional header is too small for data directories")
        magic = read_u16(data, optional, "optional header magic")
        if magic != PE32_PLUS_MAGIC:
            raise PatchError(f"expected PE32+ optional header, found {magic:#x}")
        self.optional_offset = optional
        self.size_of_image = read_u32(data, optional + 0x38, "SizeOfImage")
        self.size_of_headers = read_u32(data, optional + 0x3C, "SizeOfHeaders")
        if not self.size_of_image or self.size_of_image > MAX_VIRTUAL_IMAGE_BYTES:
            raise PatchError(f"unsupported virtual image size: {self.size_of_image}")
        if not self.size_of_headers or self.size_of_headers > len(data):
            raise PatchError(f"unsupported PE header size: {self.size_of_headers}")
        number_of_directories = read_u32(data, optional + 0x6C,
                                         "data directory count")
        if number_of_directories <= 10:
            raise PatchError("image has no load-config data directory")
        self.dll_characteristics_offset = optional + 0x46
        require_range(data, self.dll_characteristics_offset, 2,
                      "DllCharacteristics")
        self.export_rva = read_u32(data, optional + 0x70, "export directory RVA")
        self.export_size = read_u32(data, optional + 0x74, "export directory size")
        self.load_config_rva = read_u32(data, optional + 0xC0,
                                        "load-config directory RVA")
        self.load_config_size = read_u32(data, optional + 0xC4,
                                         "load-config directory size")

        section_table = optional + optional_size
        require_range(data, section_table, section_count * 40, "section table")
        for index in range(section_count):
            offset = section_table + index * 40
            raw_name = bytes(data[offset:offset + 8]).split(b"\0", 1)[0]
            if not raw_name:
                raise PatchError(f"section {index + 1} has an empty name")
            virtual_size = read_u32(data, offset + 8, "section virtual size")
            virtual_address = read_u32(data, offset + 12, "section RVA")
            raw_size = read_u32(data, offset + 16, "section raw size")
            raw_pointer = read_u32(data, offset + 20, "section raw pointer")
            if raw_size:
                require_range(data, raw_pointer, raw_size,
                              f"raw data for section {raw_name!r}")
            section = Section(index + 1, raw_name, virtual_size,
                              virtual_address, raw_size, raw_pointer)
            self.sections.append(section)
            self.sections_by_name.setdefault(raw_name, []).append(section)

        virtual_ranges = sorted(
            (section.virtual_address,
             checked_add(section.virtual_address,
                         max(section.virtual_size, section.raw_size),
                         self.size_of_image, "section virtual range"),
             section)
            for section in self.sections
            if max(section.virtual_size, section.raw_size)
        )
        previous_end = self.size_of_headers
        for start, end, section in virtual_ranges:
            if start < previous_end:
                raise PatchError(
                    f"section {section.display_name()} overlaps PE headers or another section"
                )
            previous_end = end

        self.text = self._single_section(b".text")
        self.hexpthk = self._single_section(b".hexpthk")

    def _single_section(self, name):
        matches = self.sections_by_name.get(name, [])
        if len(matches) != 1:
            raise PatchError(
                f"expected exactly one {name.decode('ascii')} section, found {len(matches)}"
            )
        return matches[0]

    def rva_to_file_offset(self, rva, size, label):
        for section in self.sections:
            if section.contains_rva(rva, size):
                return section.file_offset(rva, size)
        raise PatchError(f"{label} RVA {rva:#x} is outside mapped sections")

    def read_c_string(self, rva, label, limit=512):
        offset = self.rva_to_file_offset(rva, 1, label)
        end = offset
        max_end = min(len(self.data), offset + limit)
        while end < max_end and self.data[end] != 0:
            end += 1
        if end == max_end:
            raise PatchError(f"{label} string is not NUL-terminated")
        return bytes(self.data[offset:end])

    def _parse_exports(self):
        if not self.export_rva or self.export_size < 40:
            raise PatchError("image has no valid native export directory")
        directory = self.rva_to_file_offset(self.export_rva, 40,
                                            "native export directory")
        number_of_functions = read_u32(self.data, directory + 20,
                                       "native export function count")
        number_of_names = read_u32(self.data, directory + 24,
                                   "native export name count")
        if number_of_functions > MAX_EXPORTS or number_of_names > MAX_EXPORTS:
            raise PatchError("native export directory exceeds supported bounds")
        functions = read_u32(self.data, directory + 28,
                             "native export function array RVA")
        names = read_u32(self.data, directory + 32,
                         "native export name array RVA")
        ordinals = read_u32(self.data, directory + 36,
                            "native export ordinal array RVA")
        self.rva_to_file_offset(functions, number_of_functions * 4,
                                "native export function array")
        self.rva_to_file_offset(names, number_of_names * 4,
                                "native export name array")
        self.rva_to_file_offset(ordinals, number_of_names * 2,
                                "native export ordinal array")

        for index in range(number_of_names):
            name_offset = self.rva_to_file_offset(names + index * 4, 4,
                                                  "native export name RVA")
            name = self.read_c_string(
                read_u32(self.data, name_offset, "native export name RVA"),
                "native export name",
            ).decode("ascii", "strict")
            ordinal_offset = self.rva_to_file_offset(ordinals + index * 2, 2,
                                                     "native export ordinal")
            ordinal = read_u16(self.data, ordinal_offset, "native export ordinal")
            if ordinal >= number_of_functions:
                raise PatchError(f"native export {name!r} has an invalid ordinal")
            function_offset = self.rva_to_file_offset(functions + ordinal * 4, 4,
                                                       "native export function RVA")
            function_rva = read_u32(self.data, function_offset,
                                    "native export function RVA")
            if name in self.exports:
                raise PatchError(f"duplicate native export name: {name}")
            self.exports[name] = function_rva

    @staticmethod
    def _view_require(view, offset, size, label):
        if offset < 0 or size < 0 or offset > len(view) or size > len(view) - offset:
            raise PatchError(f"{label} is outside the virtual image bounds")

    @classmethod
    def _view_read_c_string(cls, view, rva, label, limit=512):
        cls._view_require(view, rva, 1, label)
        end = rva
        max_end = min(len(view), rva + limit)
        while end < max_end and view[end] != 0:
            end += 1
        if end == max_end:
            raise PatchError(f"{label} string is not NUL-terminated")
        return bytes(view[rva:end])

    @classmethod
    def _exports_from_view(cls, view, export_rva, export_size):
        if not export_rva or export_size < 40:
            raise PatchError("hybrid image has no valid export directory")
        cls._view_require(view, export_rva, 40, "hybrid export directory")
        number_of_functions = read_u32(view, export_rva + 20,
                                       "hybrid export function count")
        number_of_names = read_u32(view, export_rva + 24,
                                   "hybrid export name count")
        if number_of_functions > MAX_EXPORTS or number_of_names > MAX_EXPORTS:
            raise PatchError("hybrid export directory exceeds supported bounds")
        functions = read_u32(view, export_rva + 28,
                             "hybrid export function array RVA")
        names = read_u32(view, export_rva + 32,
                         "hybrid export name array RVA")
        ordinals = read_u32(view, export_rva + 36,
                            "hybrid export ordinal array RVA")
        cls._view_require(view, functions, number_of_functions * 4,
                          "hybrid export function array")
        cls._view_require(view, names, number_of_names * 4,
                          "hybrid export name array")
        cls._view_require(view, ordinals, number_of_names * 2,
                          "hybrid export ordinal array")

        exports = {}
        for index in range(number_of_names):
            name_rva = read_u32(view, names + index * 4,
                                "hybrid export name RVA")
            name = cls._view_read_c_string(view, name_rva,
                                           "hybrid export name").decode(
                                               "ascii", "strict"
                                           )
            ordinal = read_u16(view, ordinals + index * 2,
                               "hybrid export ordinal")
            if ordinal >= number_of_functions:
                raise PatchError(f"hybrid export {name!r} has an invalid ordinal")
            function_rva = read_u32(view, functions + ordinal * 4,
                                    "hybrid export function RVA")
            if name in exports:
                raise PatchError(f"duplicate hybrid export name: {name}")
            exports[name] = function_rva
        return exports

    def _build_virtual_image(self):
        view = bytearray(self.size_of_image)
        view[:self.size_of_headers] = self.data[:self.size_of_headers]
        for section in self.sections:
            if not section.raw_size:
                continue
            end = checked_add(section.virtual_address, section.raw_size,
                              self.size_of_image, "section raw virtual range")
            view[section.virtual_address:end] = self.data[
                section.raw_pointer:section.raw_pointer + section.raw_size
            ]
        return view

    def _arm64x_fixup_bytes(self):
        if not self.load_config_rva or self.load_config_size < 4:
            raise PatchError("image has no load-config directory")
        config_offset = self.rva_to_file_offset(self.load_config_rva, 4,
                                                "load-config directory")
        declared_size = read_u32(self.data, config_offset, "load-config size")
        available = min(declared_size, self.load_config_size)
        if available < 0xE6:
            raise PatchError("load-config directory is too small for ARM64X relocations")
        self.rva_to_file_offset(self.load_config_rva, available,
                                "load-config directory")
        table_offset = read_u32(self.data, config_offset + 0xE0,
                                "dynamic relocation table offset")
        section_index = read_u16(self.data, config_offset + 0xE4,
                                 "dynamic relocation table section")
        if not section_index or section_index > len(self.sections):
            raise PatchError("dynamic relocation table has an invalid section")
        section = self.sections[section_index - 1]
        if table_offset > section.raw_size or section.raw_size - table_offset < 8:
            raise PatchError("dynamic relocation table is outside its section")
        table = section.raw_pointer + table_offset
        version = read_u32(self.data, table, "dynamic relocation table version")
        entries_size = read_u32(self.data, table + 4,
                                "dynamic relocation table size")
        if entries_size > section.raw_size - table_offset - 8:
            raise PatchError("dynamic relocation entries exceed their section")
        position = table + 8
        end = position + entries_size
        matches = []
        while position < end:
            if version == 1:
                if end - position < 12:
                    raise PatchError("truncated dynamic relocation v1 header")
                symbol = struct.unpack_from("<Q", self.data, position)[0]
                fixup_size = read_u32(self.data, position + 8,
                                      "dynamic relocation fixup size")
                header_size = 12
            elif version == 2:
                if end - position < 24:
                    raise PatchError("truncated dynamic relocation v2 header")
                header_size = read_u32(self.data, position,
                                       "dynamic relocation header size")
                fixup_size = read_u32(self.data, position + 4,
                                      "dynamic relocation fixup size")
                symbol = struct.unpack_from("<Q", self.data, position + 8)[0]
                if header_size < 24:
                    raise PatchError("dynamic relocation v2 header is too small")
            else:
                raise PatchError(f"unsupported dynamic relocation version: {version}")
            if header_size > end - position or fixup_size > end - position - header_size:
                raise PatchError("dynamic relocation record exceeds its table")
            if symbol == IMAGE_DYNAMIC_RELOCATION_ARM64X:
                matches.append(bytes(self.data[
                    position + header_size:position + header_size + fixup_size
                ]))
            position += header_size + fixup_size
        if position != end:
            raise PatchError("dynamic relocation table is not exactly consumed")
        if len(matches) != 1:
            raise PatchError(
                f"expected exactly one ARM64X dynamic relocation record, found {len(matches)}"
            )
        return matches[0]

    def _apply_arm64x_fixups(self, view, fixups):
        position = 0
        operations = 0
        while position < len(fixups):
            if len(fixups) - position < 8:
                if any(fixups[position:]):
                    raise PatchError("trailing ARM64X fixup bytes are not zero")
                break
            page_rva, block_size = struct.unpack_from("<II", fixups, position)
            if not block_size:
                if any(fixups[position:]):
                    raise PatchError("ARM64X zero block has nonzero trailing bytes")
                break
            if (block_size < 8 or block_size > len(fixups) - position or
                    (block_size - 8) % 2):
                raise PatchError("ARM64X fixup block has an invalid size")
            block_end = position + block_size
            record = position + 8
            while record < block_end:
                encoded = read_u16(fixups, record, "ARM64X fixup record")
                record += 2
                if not encoded:
                    if any(fixups[record:block_end]):
                        raise PatchError("ARM64X fixup block padding is not zero")
                    record = block_end
                    break
                offset = encoded & 0xFFF
                fixup_type = (encoded >> 12) & 3
                argument = encoded >> 14
                target = checked_add(page_rva, offset, len(view),
                                     "ARM64X fixup target")
                if fixup_type in (0, 1):
                    if not argument:
                        raise PatchError("ARM64X fixup has an invalid zero width")
                    width = 1 << argument
                    self._view_require(view, target, width, "ARM64X fixup target")
                    if fixup_type == 0:
                        view[target:target + width] = b"\0" * width
                    else:
                        if width > block_end - record:
                            raise PatchError("ARM64X value fixup exceeds its block")
                        view[target:target + width] = fixups[record:record + width]
                        record += width
                elif fixup_type == 2:
                    self._view_require(view, target, 4, "ARM64X delta target")
                    if block_end - record < 2:
                        raise PatchError("ARM64X delta fixup has no operand")
                    operand = read_u16(fixups, record, "ARM64X delta operand")
                    record += 2
                    delta = operand * (8 if argument & 2 else 4)
                    if argument & 1:
                        delta = -delta
                    current = read_u32(view, target, "ARM64X delta target")
                    struct.pack_into("<I", view, target, (current + delta) & 0xFFFFFFFF)
                else:
                    raise PatchError("ARM64X fixup has an unsupported type")
                operations += 1
            position = block_end
        if not operations:
            raise PatchError("ARM64X relocation record contains no fixups")

    def _parse_hybrid_exports(self):
        view = self._build_virtual_image()
        self._apply_arm64x_fixups(view, self._arm64x_fixup_bytes())
        machine = read_u16(view, self.coff_offset, "hybrid COFF machine")
        if machine != IMAGE_FILE_MACHINE_AMD64:
            raise PatchError(
                f"ARM64X alternate image is not AMD64 (machine {machine:#x})"
            )
        if read_u16(view, self.optional_offset,
                    "hybrid optional header magic") != PE32_PLUS_MAGIC:
            raise PatchError("ARM64X alternate image is not PE32+")
        export_rva = read_u32(view, self.optional_offset + 0x70,
                              "hybrid export directory RVA")
        export_size = read_u32(view, self.optional_offset + 0x74,
                               "hybrid export directory size")
        self.hybrid_exports = self._exports_from_view(view, export_rva, export_size)

    def _coff_symbol_name(self, entry_offset, string_table, string_size):
        raw_name = bytes(self.data[entry_offset:entry_offset + 8])
        if raw_name[:4] == b"\0\0\0\0":
            name_offset = struct.unpack_from("<I", raw_name, 4)[0]
            if name_offset < 4 or name_offset >= string_size:
                raise PatchError("COFF symbol string offset is invalid")
            start = string_table + name_offset
            end = start
            limit = string_table + string_size
            while end < limit and self.data[end] != 0:
                end += 1
            if end == limit:
                raise PatchError("COFF symbol string is not NUL-terminated")
            return bytes(self.data[start:end])
        return raw_name.split(b"\0", 1)[0]

    def _parse_symbols(self):
        if not self.symbol_table_offset or not self.symbol_count:
            raise PatchError("image has no COFF symbol table")
        table_size = self.symbol_count * 18
        table_end = checked_add(self.symbol_table_offset, table_size,
                                len(self.data), "COFF symbol table")
        require_range(self.data, self.symbol_table_offset, table_size,
                      "COFF symbol table")
        require_range(self.data, table_end, 4, "COFF string table size")
        string_size = read_u32(self.data, table_end, "COFF string table size")
        if string_size < 4 or string_size > MAX_STRING_BYTES:
            raise PatchError(f"unsupported COFF string table size: {string_size}")
        require_range(self.data, table_end, string_size, "COFF string table")

        index = 0
        while index < self.symbol_count:
            entry = self.symbol_table_offset + index * 18
            name = self._coff_symbol_name(entry, table_end, string_size)
            value = read_u32(self.data, entry + 8, "COFF symbol value")
            section_number = read_i16(self.data, entry + 12,
                                      "COFF symbol section")
            symbol_type = read_u16(self.data, entry + 14, "COFF symbol type")
            storage_class = self.data[entry + 16]
            auxiliary_count = self.data[entry + 17]
            if auxiliary_count > self.symbol_count - index - 1:
                raise PatchError("COFF auxiliary symbols exceed symbol table")
            symbol = Symbol(name, value, section_number, symbol_type,
                            storage_class)
            self.symbols.setdefault(name, []).append(symbol)
            index += 1 + auxiliary_count

    def target_symbol_rva(self, symbol_name):
        matches = self.symbols.get(symbol_name, [])
        if len(matches) != 1:
            raise PatchError(
                f"expected exactly one target symbol {symbol_name.decode('ascii')}, "
                f"found {len(matches)}"
            )
        symbol = matches[0]
        if symbol.storage_class != IMAGE_SYM_CLASS_EXTERNAL:
            raise PatchError(f"target symbol {symbol_name!r} is not external")
        if symbol.symbol_type != IMAGE_SYM_DTYPE_FUNCTION:
            raise PatchError(f"target symbol {symbol_name!r} is not a function")
        if symbol.section_number <= 0 or symbol.section_number > len(self.sections):
            raise PatchError(f"target symbol {symbol_name!r} has an invalid section")
        section = self.sections[symbol.section_number - 1]
        if section is not self.text:
            raise PatchError(f"target symbol {symbol_name!r} is not in .text")
        span = max(section.virtual_size, section.raw_size)
        if symbol.value >= span:
            raise PatchError(f"target symbol {symbol_name!r} is outside .text")
        return section.virtual_address + symbol.value

    def thunk_jump_destination(self, thunk_rva, thunk):
        if (
            len(thunk) == THUNK_SIZE and
            thunk[:len(OLD_THUNK_PREFIX)] == OLD_THUNK_PREFIX and
            thunk[9] == 0xE9 and
            thunk[14:] == b"\xcc\xcc"
        ):
            displacement = read_i32(thunk, 10, "x64 thunk displacement")
            return (thunk_rva + 14 + displacement) & 0xFFFFFFFF
        return None

    def export_thunk_offset(self, export_name):
        if export_name not in self.exports:
            raise PatchError(f"missing audited native export: {export_name}")
        native_rva = self.exports[export_name]
        if not self.text.contains_rva(native_rva, 4):
            raise PatchError(f"native export {export_name} is not in .text")
        if export_name not in self.hybrid_exports:
            raise PatchError(f"missing audited hybrid export: {export_name}")
        thunk_rva = self.hybrid_exports[export_name]
        if not self.hexpthk.contains_rva(thunk_rva, THUNK_SIZE):
            raise PatchError(
                f"hybrid export {export_name} does not point into .hexpthk"
            )
        if thunk_rva & (THUNK_SIZE - 1):
            raise PatchError(f"hybrid export {export_name} thunk is not 16-byte aligned")
        return thunk_rva, self.hexpthk.file_offset(thunk_rva, THUNK_SIZE)

    def planned_changes(self, verify_only):
        changes = []
        patched = []
        already = []
        thunk_rvas = set()
        dll_characteristics = read_u16(
            self.data, self.dll_characteristics_offset, "DllCharacteristics"
        )
        if dll_characteristics & IMAGE_DLLCHARACTERISTICS_PREFER_X64_EMULATION:
            if verify_only:
                raise PatchError("PREFER_X64_EMULATION bit is still set")
            new_characteristics = (
                dll_characteristics & ~IMAGE_DLLCHARACTERISTICS_PREFER_X64_EMULATION
            )
            changes.append((
                self.dll_characteristics_offset,
                struct.pack("<H", new_characteristics),
                "clear PREFER_X64_EMULATION",
            ))

        for export_name, symbol_name, patch_bytes in AUDITED_THUNKS:
            target_rva = self.target_symbol_rva(symbol_name)
            thunk_rva, thunk_offset = self.export_thunk_offset(export_name)
            if thunk_rva in thunk_rvas:
                raise PatchError(f"hybrid export {export_name} shares an audited thunk")
            thunk_rvas.add(thunk_rva)
            thunk = bytes(self.data[thunk_offset:thunk_offset + THUNK_SIZE])
            if thunk == patch_bytes:
                already.append(export_name)
                continue
            destination = self.thunk_jump_destination(thunk_rva, thunk)
            if destination != target_rva:
                raise PatchError(
                    f"hybrid export {export_name} has unexpected thunk bytes at "
                    f"RVA {thunk_rva:#x}: {thunk.hex(' ')}"
                )
            if verify_only:
                raise PatchError(f"export {export_name} is not patched")
            changes.append((thunk_offset, patch_bytes, f"patch {export_name}"))
            patched.append(export_name)

        return changes, patched, already


def write_changes(descriptor, original, changes):
    for offset, payload, _description in changes:
        expected = bytes(original[offset:offset + len(payload)])
        if os.pread(descriptor, len(payload), offset) != expected:
            raise PatchError("PE image changed concurrently before patching")
        written = os.pwrite(descriptor, payload, offset)
        if written != len(payload):
            raise PatchError("short write while patching PE image")
    os.fsync(descriptor)


def run(action, path):
    writable = action == "patch"
    descriptor, _info, data = read_regular_file(path, writable)
    try:
        image = PEImage(data)
        changes, patched, already = image.planned_changes(action == "verify")
        if action == "patch" and changes:
            write_changes(descriptor, data, changes)
        label = os.path.basename(path)
        if action == "verify":
            print(f"{label}: verified {len(already)} audited ARM64X x64 leaf thunks")
        elif changes:
            descriptions = ", ".join(description for _offset, _payload, description in changes)
            print(f"{label}: {descriptions}")
        else:
            print(f"{label}: already patched {len(already)} audited ARM64X x64 leaf thunks")
    finally:
        os.close(descriptor)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("patch", "verify"))
    parser.add_argument("path")
    arguments = parser.parse_args(argv)
    try:
        run(arguments.action, arguments.path)
    except (OSError, UnicodeDecodeError, PatchError) as error:
        print(f"patch_arm64x_leaf_thunks.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
