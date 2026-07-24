#!/usr/bin/env python3

import argparse
import struct
from pathlib import Path


def read_u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def read_u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def rva_to_offset(data, section_table, section_count, rva):
    for index in range(section_count):
        section = section_table + index * 40
        virtual_size = read_u32(data, section + 8)
        virtual_address = read_u32(data, section + 12)
        raw_size = read_u32(data, section + 16)
        raw_offset = read_u32(data, section + 20)
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            offset = raw_offset + rva - virtual_address
            if offset >= len(data):
                break
            return offset
    raise ValueError(f"RVA {rva:#x} is not backed by a PE section")


def rename_module(path, expected_name, replacement_name):
    expected = expected_name.encode("ascii")
    replacement = replacement_name.encode("ascii")
    if len(expected) != len(replacement):
        raise ValueError("PE module names must have the same byte length")
    if not expected or b"\0" in expected or b"\0" in replacement:
        raise ValueError("PE module names must be non-empty ASCII strings")

    data = bytearray(path.read_bytes())
    if data[:2] != b"MZ":
        raise ValueError("file does not have an MZ header")

    pe_offset = read_u32(data, 0x3C)
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError("file does not have a PE header")

    coff_header = pe_offset + 4
    section_count = read_u16(data, coff_header + 2)
    optional_size = read_u16(data, coff_header + 16)
    optional_header = coff_header + 20
    optional_magic = read_u16(data, optional_header)
    if optional_magic == 0x10B:
        data_directories = optional_header + 96
    elif optional_magic == 0x20B:
        data_directories = optional_header + 112
    else:
        raise ValueError(f"unsupported PE optional-header magic {optional_magic:#x}")

    export_rva = read_u32(data, data_directories)
    if not export_rva:
        raise ValueError("PE image does not have an export directory")

    section_table = optional_header + optional_size
    export_offset = rva_to_offset(data, section_table, section_count, export_rva)
    name_rva = read_u32(data, export_offset + 12)
    name_offset = rva_to_offset(data, section_table, section_count, name_rva)
    terminator = data.find(b"\0", name_offset)
    if terminator < 0:
        raise ValueError("PE export module name is not terminated")

    current = bytes(data[name_offset:terminator])
    if current != expected:
        raise ValueError(
            f"PE export module name is {current!r}, expected {expected!r}"
        )

    with path.open("r+b") as output:
        output.seek(name_offset)
        output.write(replacement)


def main():
    parser = argparse.ArgumentParser(
        description="Rename an in-place, same-length PE export module name."
    )
    parser.add_argument("image", type=Path)
    parser.add_argument("expected_name")
    parser.add_argument("replacement_name")
    args = parser.parse_args()

    try:
        rename_module(args.image, args.expected_name, args.replacement_name)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
