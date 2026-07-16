#!/usr/bin/env python3
"""Extract an OpenType collection face and give it a compatibility family name."""

import argparse
import os
import re
import struct
import tempfile


CHECKSUM_MAGIC = 0xB1B0AFBA
RENAMED_NAME_IDS = {1, 2, 3, 4, 6, 16, 17}


def read_u16(data, offset):
    return struct.unpack_from(">H", data, offset)[0]


def read_u32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def checksum(data):
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xFFFFFFFF


def encode_name(value, platform_id, original):
    if platform_id in (0, 3):
        return value.encode("utf-16-be")
    if platform_id == 1:
        return value.encode("mac_roman")
    try:
        return value.encode("ascii")
    except UnicodeEncodeError:
        return original


def replacement_name(name_id, family, postscript):
    if name_id in (1, 4, 16):
        return family
    if name_id in (2, 17):
        return "Regular"
    if name_id == 3:
        return f"Switchyard; {family} Regular"
    if name_id == 6:
        return postscript
    raise ValueError(f"unsupported renamed name id {name_id}")


def rename_name_table(table, family, postscript):
    if len(table) < 6:
        raise ValueError("truncated OpenType name table")
    table_format, count, string_offset = struct.unpack_from(">HHH", table, 0)
    if table_format != 0:
        raise ValueError(f"unsupported OpenType name table format {table_format}")
    records_end = 6 + 12 * count
    if records_end > len(table) or string_offset < records_end or string_offset > len(table):
        raise ValueError("invalid OpenType name table offsets")

    records = []
    strings = bytearray()
    string_offsets = {}
    for index in range(count):
        record = struct.unpack_from(">6H", table, 6 + 12 * index)
        platform_id, encoding_id, language_id, name_id, length, offset = record
        start = string_offset + offset
        end = start + length
        if end > len(table):
            raise ValueError("invalid OpenType name record")
        value = table[start:end]
        if name_id in RENAMED_NAME_IDS:
            value = encode_name(replacement_name(name_id, family, postscript), platform_id, value)
        if value not in string_offsets:
            string_offsets[value] = len(strings)
            strings.extend(value)
        records.append((platform_id, encoding_id, language_id, name_id,
                        len(value), string_offsets[value]))

    result = bytearray(struct.pack(">HHH", 0, count, records_end))
    for record in records:
        result.extend(struct.pack(">6H", *record))
    result.extend(strings)
    return bytes(result)


def face_offset(data, face_index):
    if data[:4] != b"ttcf":
        if face_index:
            raise ValueError("a standalone font only has face index 0")
        return 0
    if len(data) < 12:
        raise ValueError("truncated OpenType collection header")
    face_count = read_u32(data, 8)
    if face_index < 0 or face_index >= face_count:
        raise ValueError(f"face index {face_index} is outside collection with {face_count} faces")
    offsets_end = 12 + 4 * face_count
    if offsets_end > len(data):
        raise ValueError("truncated OpenType collection offsets")
    return read_u32(data, 12 + 4 * face_index)


def extract_alias(data, index, family, postscript):
    offset = face_offset(data, index)
    if offset + 12 > len(data):
        raise ValueError("truncated OpenType face header")
    sfnt_version = data[offset:offset + 4]
    if sfnt_version not in (b"OTTO", b"\x00\x01\x00\x00", b"true"):
        raise ValueError("unsupported OpenType face signature")
    table_count = read_u16(data, offset + 4)
    directory_end = offset + 12 + 16 * table_count
    if directory_end > len(data):
        raise ValueError("truncated OpenType table directory")

    tables = []
    for table_index in range(table_count):
        tag, _old_checksum, table_offset, length = struct.unpack_from(
            ">4sIII", data, offset + 12 + 16 * table_index)
        if table_offset + length > len(data):
            raise ValueError(f"truncated {tag!r} table")
        value = data[table_offset:table_offset + length]
        if tag == b"name":
            value = rename_name_table(value, family, postscript)
        tables.append((tag, value))
    if not any(tag == b"name" for tag, _value in tables):
        raise ValueError("font has no name table")
    if not any(tag == b"head" for tag, _value in tables):
        raise ValueError("font has no head table")

    result = bytearray(data[offset:offset + 12])
    result.extend(b"\0" * (16 * table_count))
    table_records = []
    head_offset = None
    for tag, value in tables:
        while len(result) % 4:
            result.append(0)
        new_offset = len(result)
        value = bytearray(value)
        if tag == b"head":
            if len(value) < 12:
                raise ValueError("truncated OpenType head table")
            struct.pack_into(">I", value, 8, 0)
            head_offset = new_offset
        result.extend(value)
        table_records.append((tag, checksum(bytes(value)), new_offset, len(value)))

    for table_index, record in enumerate(table_records):
        struct.pack_into(">4sIII", result, 12 + 16 * table_index, *record)
    adjustment = (CHECKSUM_MAGIC - checksum(bytes(result))) & 0xFFFFFFFF
    struct.pack_into(">I", result, head_offset + 8, adjustment)
    if checksum(bytes(result)) != CHECKSUM_MAGIC:
        raise ValueError("failed to calculate OpenType checksum adjustment")
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("--face-index", type=int, default=0)
    parser.add_argument("--family", required=True)
    parser.add_argument("--postscript", required=True)
    args = parser.parse_args()

    if not re.fullmatch(r"[!-~]+", args.postscript) or any(
            char in args.postscript for char in "[](){}<>/%"):
        parser.error("the PostScript name must use safe printable ASCII without spaces")

    with open(args.source, "rb") as source:
        output = extract_alias(source.read(), args.face_index, args.family, args.postscript)

    output_dir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(output_dir, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".font-alias-", dir=output_dir)
    try:
        with os.fdopen(fd, "wb") as target:
            target.write(output)
        os.chmod(temporary, 0o644)
        os.replace(temporary, args.output)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


if __name__ == "__main__":
    main()
