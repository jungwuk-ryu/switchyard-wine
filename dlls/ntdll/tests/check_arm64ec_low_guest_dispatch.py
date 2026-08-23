#!/usr/bin/env python3
"""Verify bounded ARM64EC EC-code lookup and low x64 guest dispatch."""

from pathlib import Path
import re
import sys


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function {signature}")
    start = source.find("{", start + len(signature))
    if start < 0:
        raise AssertionError(f"missing body for {signature}")
    depth = 0
    for offset in range(start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if not depth:
                return source[start : offset + 1]
    raise AssertionError(f"unterminated function {signature}")


def instruction(body: str, text: str) -> int:
    encoded = f'"{text}\\n\\t"'
    offset = body.find(encoded)
    if offset < 0:
        raise AssertionError(f"missing checker instruction {text}")
    if body.find(encoded, offset + 1) >= 0:
        raise AssertionError(f"duplicate checker instruction {text}")
    return offset


def source_instruction(body: str, text: str) -> int:
    offset = body.find(text)
    if offset < 0:
        raise AssertionError(f"missing checker source instruction {text}")
    if body.find(text, offset + 1) >= 0:
        raise AssertionError(f"duplicate checker source instruction {text}")
    return offset


def instruction_offsets(body: str, text: str):
    encoded = f'"{text}\\n\\t"'
    offsets = []
    start = 0
    while (offset := body.find(encoded, start)) >= 0:
        offsets.append(offset)
        start = offset + len(encoded)
    return offsets


def verify_bitmap_lookup_contract(
    signal_source: str, unix_private_source: str, low_va_source: str
) -> int:
    bits_match = re.search(
        r"(?m)^#define\s+WINE_ARM64EC_CODE_POINTER_BITS\s+([0-9]+)\s*$",
        low_va_source,
    )
    if not bits_match or int(bits_match.group(1)) != 47:
        raise AssertionError("ARM64EC code-pointer domain is not the required 47 bits")
    bits = int(bits_match.group(1))
    if "#define WINE_ARM64EC_CODE_POINTER_LIMIT (1ull << WINE_ARM64EC_CODE_POINTER_BITS)" not in low_va_source:
        raise AssertionError("ARM64EC code-pointer limit is not derived from its bit count")
    for assertion in (
        "WINE_ARM64EC_CODE_POINTER_LIMIT != 0x0000800000000000ull",
        "WINE_ARM64EC_CODE_POINTER_LIMIT <= WINE_LOW_VA_SHADOW_BASE + WINE_LOW_VA_SHADOW_SIZE",
    ):
        if assertion not in low_va_source:
            raise AssertionError(f"missing ARM64EC code-pointer compile assertion: {assertion}")

    range_body = function_body(
        low_va_source, "static inline int wine_arm64ec_code_pointer_in_range"
    )
    if "ptr < WINE_ARM64EC_CODE_POINTER_LIMIT" not in range_body:
        raise AssertionError("ARM64EC pointer range helper lost its exclusive limit")

    for source, signature, name in (
        (signal_source, "BOOLEAN WINAPI RtlIsEcCode", "PE RtlIsEcCode"),
        (unix_private_source, "static inline BOOL is_ec_code", "Unix is_ec_code"),
    ):
        body = function_body(source, signature)
        guard = body.find(
            "if (!map || !wine_arm64ec_code_pointer_in_range( ptr )) return FALSE;"
        )
        probe = body.find("map[")
        if guard < 0 or probe < 0 or guard >= probe:
            raise AssertionError(f"{name} is not bounded before its first bitmap dereference")

    class PoisonBitmap:
        def __getitem__(self, index: int) -> int:
            raise AssertionError(f"out-of-domain bitmap access at word {index}")

    class SparseBitmap:
        def __init__(self, words) -> None:
            self.words = words

        def __getitem__(self, index: int) -> int:
            return self.words.get(index, 0)

    limit = 1 << bits

    def is_ec_code(bitmap, ptr: int, page_size: int = 0x1000) -> bool:
        if bitmap is None or ptr >= limit or not page_size:
            return False
        page = ptr // page_size
        return bool((bitmap[page // 64] >> (page & 63)) & 1)

    bitmap = SparseBitmap({0: 1 << 1})
    if not is_ec_code(bitmap, 0x1000) or is_ec_code(bitmap, 0x2000):
        raise AssertionError("valid in-range EC bitmap positive/negative cases regressed")
    if is_ec_code(None, 0x1000) or is_ec_code(bitmap, 0x1000, 0):
        raise AssertionError("null bitmap/page-size lookup is not fail-closed")
    poison = PoisonBitmap()
    for ptr in (limit, (1 << 64) - 1, 0xFFFF800000000000, 0x6F646E69775C3A43):
        if is_ec_code(poison, ptr):
            raise AssertionError(f"out-of-domain pointer {ptr:#x} was classified as EC code")
    shadow = 0x0000010000000000
    if shadow >= limit or is_ec_code(SparseBitmap({}), shadow):
        raise AssertionError("unmarked native low-VA shadow pointer must remain non-EC")
    if is_ec_code(SparseBitmap({}), limit - 1):
        raise AssertionError("unmarked maximum in-range pointer was classified as EC code")

    return bits


def verify_contract(signal_source: str, low_va_source: str, unix_private_source: str) -> None:
    size_match = re.search(
        r"(?m)^#define\s+WINE_LOW_VA_SHADOW_SIZE\s+(0x[0-9a-fA-F]+)(?:ull)?\s*$",
        low_va_source,
    )
    if not size_match or int(size_match.group(1), 16) != 1 << 32:
        raise AssertionError("ARM64EC low-target gate no longer matches the low-VA shadow size")

    bits = verify_bitmap_lookup_contract(signal_source, unix_private_source, low_va_source)

    body = function_body(
        signal_source, "static void __attribute__((naked)) arm64x_check_call(void)"
    )
    if (
        "#define ARM64EC_STRINGIFY_(value) #value" not in signal_source
        or "#define ARM64EC_STRINGIFY(value) ARM64EC_STRINGIFY_(value)" not in signal_source
    ):
        raise AssertionError("ARM64EC assembly bit count lacks a two-step stringifier")
    classify_user = source_instruction(
        body,
        '"lsr x16, x11, #" ARM64EC_STRINGIFY(WINE_ARM64EC_CODE_POINTER_BITS) "\\n\\t"',
    )
    exit_outside = instruction(body, "cbnz x16, .Lexit")
    load_peb = instruction(body, "ldr x16, [x18, #0x60]")
    load_bitmap = instruction(body, "ldr x16, [x16, #0x368]")
    null_exits = instruction_offsets(body, "cbz x16, .Lexit")
    if len(null_exits) != 2:
        raise AssertionError("checker must contain bitmap-null and low-target exits")
    exit_null_bitmap, exit_low = null_exits
    probe_bitmap = instruction(body, "ldr x16, [x16, x17, lsl #3]")
    if not classify_user < exit_outside < load_peb < load_bitmap < exit_null_bitmap < probe_bitmap:
        raise AssertionError("non-user targets and null bitmaps must exit before the EC bitmap lookup")

    native = instruction(body, "tbnz x16, #0, .Ldone")
    classify = instruction(body, "lsr x16, x11, #32")
    first_probe = body.find("[x11]", native)
    if first_probe < 0:
        raise AssertionError("checker no longer contains a target-byte probe")
    if not native < classify < exit_low < first_probe:
        raise AssertionError(
            "low guest dispatch must follow the native bitmap decision and precede target probing"
        )

    exit_label = instruction(body, ".Lexit:")
    restore_target = instruction(body, "mov x9, x11")
    select_thunk = instruction(body, "mov x11, x10")
    done_label = instruction(body, ".Ldone:")
    if not exit_label < restore_target < select_thunk < done_label:
        raise AssertionError("x64 exit path does not preserve the guest target and exit thunk")

    def route(target: int, ec_code: bool) -> str:
        if target >= 1 << bits:
            return "x64"
        if ec_code:
            return "native"
        if target < 1 << 32:
            return "x64"
        return "probe"

    if route(0x00401000, False) != "x64" or route(0xFFFFFFFF, False) != "x64":
        raise AssertionError("translated low targets do not route to x64")
    if route(0x00401000, True) != "native":
        raise AssertionError("bitmap-positive low ARM64EC target lost native priority")
    if route(0x100000000, False) != "probe":
        raise AssertionError("high x64 thunk probing was broadened by the low-target gate")
    if route(1 << bits, False) != "x64" or route((1 << 64) - 1, False) != "x64":
        raise AssertionError("out-of-bitmap targets do not route to x64")


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} SIGNAL_ARM64EC_C LOW_VA_H", file=sys.stderr)
        return 2
    signal_source = Path(sys.argv[1]).read_text(encoding="utf-8")
    low_va_source = Path(sys.argv[2]).read_text(encoding="utf-8")
    unix_private_source = (
        Path(sys.argv[1]).resolve().parent / "unix" / "unix_private.h"
    ).read_text(encoding="utf-8")
    verify_contract(signal_source, low_va_source, unix_private_source)

    null_exit = '"cbz x16, .Lexit\\n\\t"'
    first_null_exit = signal_source.find(null_exit)
    second_null_exit = signal_source.find(null_exit, first_null_exit + len(null_exit))
    if first_null_exit < 0 or second_null_exit < 0:
        raise AssertionError("could not locate bitmap-null and low-dispatch exits")
    mutated = signal_source[:second_null_exit] + '"nop\\n\\t"' + signal_source[second_null_exit + len(null_exit):]
    if mutated == signal_source:
        raise AssertionError("could not construct missing low-dispatch mutation")
    try:
        verify_contract(mutated, low_va_source, unix_private_source)
    except AssertionError:
        pass
    else:
        raise AssertionError("regression test accepted native probing of low guest code")

    mutated = signal_source[:first_null_exit] + '"nop\\n\\t"' + signal_source[first_null_exit + len(null_exit):]
    if mutated == signal_source:
        raise AssertionError("could not construct missing bitmap-null mutation")
    try:
        verify_contract(mutated, low_va_source, unix_private_source)
    except AssertionError:
        pass
    else:
        raise AssertionError("regression test accepted a null EC bitmap dereference")

    mutated = signal_source.replace(
        '"cbnz x16, .Lexit\\n\\t"', '"nop\\n\\t"', 1
    )
    if mutated == signal_source:
        raise AssertionError("could not construct missing bitmap-boundary mutation")
    try:
        verify_contract(mutated, low_va_source, unix_private_source)
    except AssertionError:
        pass
    else:
        raise AssertionError("regression test accepted an unbounded EC bitmap lookup")

    mutated = low_va_source.replace(
        "return ptr < WINE_ARM64EC_CODE_POINTER_LIMIT;", "return 1;", 1
    )
    if mutated == low_va_source:
        raise AssertionError("could not construct missing C bitmap-boundary mutation")
    try:
        verify_contract(signal_source, mutated, unix_private_source)
    except AssertionError:
        pass
    else:
        raise AssertionError("regression test accepted an unbounded C EC bitmap lookup")

    mutated = unix_private_source.replace(
        "if (!map || !wine_arm64ec_code_pointer_in_range( ptr )) return FALSE;",
        "if (!map) return FALSE;",
        1,
    )
    if mutated == unix_private_source:
        raise AssertionError("could not construct Unix bitmap-reader mutation")
    try:
        verify_contract(signal_source, low_va_source, mutated)
    except AssertionError:
        pass
    else:
        raise AssertionError("regression test accepted an unbounded Unix EC bitmap reader")

    print("ARM64EC bounded EC-code lookup and low guest dispatch regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
