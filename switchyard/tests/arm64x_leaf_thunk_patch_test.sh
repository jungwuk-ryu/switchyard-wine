#!/usr/bin/env bash
set -euo pipefail
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
PATCHER="$ROOT_DIR/switchyard/patch_arm64x_leaf_thunks.py"
BUILD_RUNTIME="$ROOT_DIR/switchyard/build_runtime.sh"
TEST_ROOT="$(/usr/bin/mktemp -d /private/tmp/switchyard-arm64x-leaf-thunk.XXXXXX)"
PYTHON3="${PYTHON3:-/usr/bin/python3}"

cleanup() {
  local status=$?

  trap - EXIT HUP INT TERM
  case "$TEST_ROOT" in
    /private/tmp/switchyard-arm64x-leaf-thunk.??????)
      [ ! -L "$TEST_ROOT" ] && /bin/rm -rf -- "$TEST_ROOT"
      ;;
    *) echo "refusing to clean unexpected ARM64X thunk test root: $TEST_ROOT" >&2 ;;
  esac
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "ARM64X leaf thunk patch test failed: $1" >&2
  exit 1
}

run_patcher() {
  PYTHONDONTWRITEBYTECODE=1 "$PYTHON3" -I "$PATCHER" "$@"
}

expect_failure() {
  local label="$1"
  shift

  if "$@" >"$TEST_ROOT/failure.out" 2>"$TEST_ROOT/failure.err"; then
    fail "$label was accepted"
  fi
}

[ -f "$PATCHER" ] && [ ! -L "$PATCHER" ] ||
  fail "patcher is missing or unsafe"

make_fixture() {
  local destination="$1"
  local mode="${2:-old}"

  PYTHONDONTWRITEBYTECODE=1 "$PYTHON3" -I - "$destination" "$mode" <<'PY'
import os
import struct
import sys

destination, mode = sys.argv[1:]

exports = [
    ("_rotl", "#MSVCRT__rotl", bytes.fromhex("89 c8 89 d1 d3 c0 c3") + b"\xcc" * 9),
    ("_lrotl", "#MSVCRT__lrotl", bytes.fromhex("89 c8 89 d1 d3 c0 c3") + b"\xcc" * 9),
    ("_rotr", "#MSVCRT__rotr", bytes.fromhex("89 c8 89 d1 d3 c8 c3") + b"\xcc" * 9),
    ("_lrotr", "#MSVCRT__lrotr", bytes.fromhex("89 c8 89 d1 d3 c8 c3") + b"\xcc" * 9),
    ("_rotl64", "#MSVCRT__rotl64", bytes.fromhex("48 89 c8 89 d1 48 d3 c0 c3") + b"\xcc" * 7),
    ("_rotr64", "#MSVCRT__rotr64", bytes.fromhex("48 89 c8 89 d1 48 d3 c8 c3") + b"\xcc" * 7),
]

file_size = 0x3000
data = bytearray(b"\0" * file_size)

def put(offset, payload):
    data[offset:offset + len(payload)] = payload

def old_thunk(thunk_rva, target_rva):
    prefix = bytes.fromhex("48 8b c4 48 89 58 20 55 5d e9")
    displacement = target_rva - (thunk_rva + 14)
    return prefix + struct.pack("<i", displacement) + b"\xcc\xcc"

def value_fixup(target_rva, size_code, payload):
    record = ((target_rva & 0xFFF) | (1 << 12) | (size_code << 14))
    body = struct.pack("<H", record) + payload + b"\0\0"
    body += b"\0" * ((-len(body)) & 3)
    return struct.pack("<II", target_rva & ~0xFFF, 8 + len(body)) + body

def write_exports(raw, rva, functions):
    function_rva = rva + 0x40
    name_rva = rva + 0x80
    ordinal_rva = rva + 0xC0
    cursor = raw + 0xE0
    struct.pack_into("<IIHHIIIIIII", data, raw, 0, 0, 0, 0, 0, 1,
                     len(exports), len(exports), function_rva, name_rva,
                     ordinal_rva)
    for index, function in enumerate(functions):
        struct.pack_into("<I", data, raw + 0x40 + index * 4, function)
        struct.pack_into("<I", data, raw + 0x80 + index * 4,
                         rva + cursor - raw)
        struct.pack_into("<H", data, raw + 0xC0 + index * 2, index)
        payload = exports[index][0].encode("ascii") + b"\0"
        put(cursor, payload)
        cursor += len(payload)

e_lfanew = 0x80
put(0, b"MZ")
struct.pack_into("<I", data, 0x3C, e_lfanew)
put(e_lfanew, b"PE\0\0")
coff = e_lfanew + 4
optional = coff + 20
optional_size = 0xF0
section_table = optional + optional_size
text_va, text_raw, text_size = 0x1000, 0x400, 0x400
hex_va, hex_raw, hex_size = 0x2000, 0x800, 0x400
rdata_va, rdata_raw, rdata_size = 0x3000, 0xC00, 0x1000
reloc_va, reloc_raw, reloc_size = 0x4000, 0x1C00, 0x400
native_export_rva = rdata_va
hybrid_export_rva = rdata_va + 0x300
load_config_rva = rdata_va + 0x800
dynamic_table_offset = 0x100
symbol_table = 0x2000

struct.pack_into("<HHIIIHH", data, coff, 0xAA64, 4, 0, symbol_table,
                 len(exports), optional_size, 0x2022)
struct.pack_into("<H", data, optional, 0x20B)
struct.pack_into("<I", data, optional + 0x38, 0x5000)
struct.pack_into("<I", data, optional + 0x3C, 0x400)
struct.pack_into("<H", data, optional + 0x46, 0x0168)
struct.pack_into("<I", data, optional + 0x6C, 16)
struct.pack_into("<II", data, optional + 0x70, native_export_rva, 0x200)
struct.pack_into("<II", data, optional + 0xC0, load_config_rva, 0x140)

sections = [
    (b".text\0\0\0", text_size, text_va, text_size, text_raw),
    (b".hexpthk", hex_size, hex_va, hex_size, hex_raw),
    (b".rdata\0\0", rdata_size, rdata_va, rdata_size, rdata_raw),
    (b".reloc\0\0", reloc_size, reloc_va, reloc_size, reloc_raw),
]
for index, (name, virtual_size, virtual_address, raw_size, raw_pointer) in enumerate(sections):
    offset = section_table + index * 40
    put(offset, name)
    struct.pack_into("<IIIIIIHHI", data, offset + 8, virtual_size,
                     virtual_address, raw_size, raw_pointer, 0, 0, 0, 0, 0)

target_rvas = []
native_rvas = []
thunk_rvas = []
for index, (_export_name, _symbol_name, patch_bytes) in enumerate(exports):
    target_rva = text_va + 0x40 + index * 0x10
    native_rva = text_va + 0x200 + index * 0x10
    thunk_rva = hex_va + index * 0x10
    target_rvas.append(target_rva)
    native_rvas.append(native_rva)
    thunk_rvas.append(thunk_rva)
    put(text_raw + target_rva - text_va, b"\xc3" + b"\xcc" * 15)
    put(text_raw + native_rva - text_va, b"\xc0\x03\x5f\xd6" + b"\xcc" * 12)
    if mode == "patched":
        put(hex_raw + thunk_rva - hex_va, patch_bytes)
    else:
        put(hex_raw + thunk_rva - hex_va, old_thunk(thunk_rva, target_rva))

write_exports(rdata_raw, native_export_rva, native_rvas)
write_exports(rdata_raw + 0x300, hybrid_export_rva, thunk_rvas)

load_config = rdata_raw + 0x800
struct.pack_into("<I", data, load_config, 0x140)
struct.pack_into("<I", data, load_config + 0xE0, dynamic_table_offset)
struct.pack_into("<H", data, load_config + 0xE4, 4)

alternate_machine = 0xAA64 if mode == "wrong-alt-machine" else 0x8664
fixups = b"".join((
    value_fixup(coff, 1, struct.pack("<H", alternate_machine)),
    value_fixup(optional + 0x70, 2, struct.pack("<I", hybrid_export_rva)),
    value_fixup(optional + 0x74, 2, struct.pack("<I", 0x200)),
))
if mode == "bad-fixup-block":
    fixups = bytearray(fixups)
    struct.pack_into("<I", fixups, 4, 7)
    fixups = bytes(fixups)
dynamic_entry = struct.pack("<QI", 6, len(fixups)) + fixups
if mode == "duplicate-arm64x":
    dynamic_entry += struct.pack("<QI", 6, len(fixups)) + fixups
dynamic_table = struct.pack("<II", 1, len(dynamic_entry)) + dynamic_entry
put(reloc_raw + dynamic_table_offset, dynamic_table)

string_table = bytearray(struct.pack("<I", 4))
symbol_records = bytearray()
for index, (_export_name, symbol_name, _patch_bytes) in enumerate(exports):
    encoded = symbol_name.encode("ascii") + b"\0"
    name_offset = len(string_table)
    string_table += encoded
    symbol_records += struct.pack("<II", 0, name_offset)
    symbol_records += struct.pack("<IhHBB", target_rvas[index] - text_va, 1,
                                  0x20, 2, 0)
struct.pack_into("<I", string_table, 0, len(string_table))
put(symbol_table, symbol_records)
put(symbol_table + len(symbol_records), string_table)

if mode == "bad-thunk":
    data[hex_raw + 1] ^= 0x7F
elif mode == "missing-symbol":
    first_name_offset = struct.unpack_from("<I", data, symbol_table + 4)[0]
    string_start = symbol_table + len(symbol_records)
    data[string_start + first_name_offset] = ord("X")
elif mode == "bad-hybrid-export":
    struct.pack_into("<I", data, rdata_raw + 0x300 + 0x40, target_rvas[0])
elif mode == "bad-native-export":
    struct.pack_into("<I", data, rdata_raw + 0x40, thunk_rvas[0])
elif mode == "oversized-dynamic-table":
    struct.pack_into("<I", data, reloc_raw + dynamic_table_offset + 4,
                     reloc_size)

with open(destination, "wb") as stream:
    stream.write(data)
os.chmod(destination, 0o644)
PY
}

fixture="$TEST_ROOT/ucrtbase.dll"
make_fixture "$fixture" old
expect_failure "unpatched fixture verification" run_patcher verify "$fixture"
run_patcher patch "$fixture" >"$TEST_ROOT/patch.out"
run_patcher verify "$fixture"
run_patcher patch "$fixture" >"$TEST_ROOT/idempotent.out"

PYTHONDONTWRITEBYTECODE=1 "$PYTHON3" -I - "$fixture" <<'PY'
import struct
import sys

path = sys.argv[1]
data = open(path, "rb").read()
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
dll_characteristics = struct.unpack_from("<H", data, e_lfanew + 4 + 20 + 0x46)[0]
assert not (dll_characteristics & 0x0008), hex(dll_characteristics)
expected = [
    bytes.fromhex("89 c8 89 d1 d3 c0 c3") + b"\xcc" * 9,
    bytes.fromhex("89 c8 89 d1 d3 c0 c3") + b"\xcc" * 9,
    bytes.fromhex("89 c8 89 d1 d3 c8 c3") + b"\xcc" * 9,
    bytes.fromhex("89 c8 89 d1 d3 c8 c3") + b"\xcc" * 9,
    bytes.fromhex("48 89 c8 89 d1 48 d3 c0 c3") + b"\xcc" * 7,
    bytes.fromhex("48 89 c8 89 d1 48 d3 c8 c3") + b"\xcc" * 7,
]
for index, payload in enumerate(expected):
    offset = 0x800 + index * 0x10
    actual = data[offset:offset + 0x10]
    assert actual == payload, (index, actual.hex(), payload.hex())
PY

make_fixture "$TEST_ROOT/bad-name.dll" old
expect_failure "unexpected basename" run_patcher patch "$TEST_ROOT/bad-name.dll"
mkdir "$TEST_ROOT/symlink-case"
ln -s ../ucrtbase.dll "$TEST_ROOT/symlink-case/ucrtbase.dll"
expect_failure "symlink image" run_patcher patch "$TEST_ROOT/symlink-case/ucrtbase.dll"
make_fixture "$TEST_ROOT/ucrtbase.dll" bad-thunk
expect_failure "unexpected thunk bytes" run_patcher patch "$TEST_ROOT/ucrtbase.dll"
make_fixture "$TEST_ROOT/ucrtbase.dll" missing-symbol
expect_failure "missing COFF symbol" run_patcher patch "$TEST_ROOT/ucrtbase.dll"
make_fixture "$TEST_ROOT/ucrtbase.dll" bad-hybrid-export
expect_failure "hybrid export outside .hexpthk" run_patcher patch "$TEST_ROOT/ucrtbase.dll"
make_fixture "$TEST_ROOT/ucrtbase.dll" bad-native-export
expect_failure "native export outside .text" run_patcher patch "$TEST_ROOT/ucrtbase.dll"
make_fixture "$TEST_ROOT/ucrtbase.dll" wrong-alt-machine
expect_failure "non-AMD64 alternate image" run_patcher patch "$TEST_ROOT/ucrtbase.dll"
make_fixture "$TEST_ROOT/ucrtbase.dll" duplicate-arm64x
expect_failure "duplicate ARM64X relocation" run_patcher patch "$TEST_ROOT/ucrtbase.dll"
make_fixture "$TEST_ROOT/ucrtbase.dll" bad-fixup-block
expect_failure "malformed ARM64X fixup block" run_patcher patch "$TEST_ROOT/ucrtbase.dll"
make_fixture "$TEST_ROOT/ucrtbase.dll" oversized-dynamic-table
expect_failure "oversized dynamic relocation table" run_patcher patch "$TEST_ROOT/ucrtbase.dll"

grep -F "patch_arm64x_leaf_thunks.py" "$BUILD_RUNTIME" >/dev/null ||
  fail "build_runtime.sh does not wire the ARM64X leaf thunk patcher"

echo "ARM64X leaf thunk patch tests passed"
