#!/usr/bin/env python3
"""Create a default-instance static TrueType font without FontTools.

The glyf and hmtx tables of a TrueType variable font contain the default
instance.  Removing its variation tables therefore produces a static font at
the default axis coordinates, which older Wine font paths can load safely.
"""

import argparse
import os
import struct
import sys
import tempfile
from pathlib import Path


CHECKSUM_MAGIC = 0xB1B0AFBA
MAX_FONT_SIZE = 32 * 1024 * 1024
MAX_TABLES = 4096
VARIABLE_TABLES = {b"avar", b"cvar", b"fvar", b"gvar", b"HVAR", b"MVAR", b"STAT", b"VVAR"}
REQUIRED_TABLES = {b"OS/2", b"cmap", b"glyf", b"head", b"hhea", b"hmtx", b"loca", b"maxp", b"name", b"post"}


def fail(message):
    raise ValueError(message)


def checksum(data):
    padding = (-len(data)) % 4
    if padding:
        data += b"\0" * padding
    return sum(struct.unpack(">{:d}I".format(len(data) // 4), data)) & 0xffffffff


def parse_font(data):
    if len(data) > MAX_FONT_SIZE:
        fail("font exceeds the staticizer size limit")
    if len(data) < 12:
        fail("font is shorter than its SFNT header")

    version, table_count, _, _, _ = struct.unpack_from(">IHHHH", data)
    if version not in (0x00010000, 0x74727565):
        fail("font is not a TrueType SFNT")
    if table_count > MAX_TABLES:
        fail("font has too many tables")
    directory_size = 12 + table_count * 16
    if directory_size > len(data):
        fail("font table directory is truncated")

    tables = {}
    for index in range(table_count):
        offset = 12 + index * 16
        tag, _, table_offset, length = struct.unpack_from(">4sIII", data, offset)
        if tag in tables:
            fail("font has duplicate table tags")
        if table_offset < directory_size or table_offset > len(data) or length > len(data) - table_offset:
            fail("font table extends past the end of the file")
        tables[tag] = bytearray(data[table_offset:table_offset + length])
    missing = REQUIRED_TABLES.difference(tables)
    if missing:
        fail("font is missing required TrueType tables: {}".format(", ".join(sorted(tag.decode("ascii") for tag in missing))))
    if b"fvar" not in tables or b"gvar" not in tables:
        fail("font is not a TrueType variable font with glyph variations")
    return tables


def build_font(tables):
    if b"head" not in tables or len(tables[b"head"]) < 12:
        fail("font is missing a complete head table")

    removed = sorted(tag for tag in tables if tag in VARIABLE_TABLES)
    if not removed:
        fail("font has no variation tables to remove")
    kept = [(tag, tables[tag]) for tag in sorted(tables) if tag not in VARIABLE_TABLES]
    tables[b"head"][8:12] = b"\0\0\0\0"

    table_count = len(kept)
    entry_selector = table_count.bit_length() - 1
    search_range = (1 << entry_selector) * 16
    range_shift = table_count * 16 - search_range
    directory_size = 12 + table_count * 16

    records = []
    offset = directory_size
    for tag, table in kept:
        offset = (offset + 3) & ~3
        records.append((tag, checksum(bytes(table)), offset, len(table)))
        offset += len(table)

    output = bytearray(offset)
    struct.pack_into(">IHHHH", output, 0, 0x00010000, table_count, search_range, entry_selector, range_shift)
    head_offset = None
    for index, ((tag, table), (_, table_checksum, table_offset, length)) in enumerate(zip(kept, records)):
        struct.pack_into(">4sIII", output, 12 + index * 16, tag, table_checksum, table_offset, length)
        output[table_offset:table_offset + length] = table
        if tag == b"head":
            head_offset = table_offset

    if head_offset is None:
        fail("font is missing a head table")
    adjustment = (CHECKSUM_MAGIC - checksum(bytes(output))) & 0xffffffff
    struct.pack_into(">I", output, head_offset + 8, adjustment)
    if checksum(bytes(output)) != CHECKSUM_MAGIC:
        fail("failed to calculate checkSumAdjustment")
    return bytes(output), b", ".join(removed).decode("ascii")


def write_font(destination, data):
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".static-font-", dir=destination.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
        os.chmod(temporary, 0o644)
        os.replace(temporary, destination)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    try:
        if args.source.stat().st_size > MAX_FONT_SIZE:
            fail("font exceeds the staticizer size limit")
        generated, removed = build_font(parse_font(args.source.read_bytes()))
        write_font(args.destination, generated)
    except (OSError, ValueError, struct.error) as error:
        print("make_static_font.py: {}".format(error), file=sys.stderr)
        return 1
    print("generated static font without {}".format(removed), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
