#!/usr/bin/env python3
"""Static and arithmetic contracts for the xtajit64 Apple Silicon hot paths."""

from __future__ import annotations

import random
import re
import sys
from pathlib import Path


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"xtajit64 hot-path contract failed: {message}")


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"(?m)^static\s+(?:inline\s+)?[^\n(]+\b{name}\s*\([^;]*?\)\s*\{{",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not match:
        fail(f"cannot find {name}()")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    fail(f"unterminated {name}()")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} UNIXLIB_C", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    source = path.read_text(encoding="utf-8")

    classifier = function_body(source, "is_ec_code")
    if "address >> provider.ec_page_shift" not in classifier:
        fail("EC classifier does not use the precomputed shift")
    if "/" in re.sub(r"/\*.*?\*/|//[^\n]*", "", classifier, flags=re.DOTALL):
        fail("EC classifier still contains a division")
    if "page >> 6" not in classifier or "page & 63" not in classifier:
        fail("EC bitmap word/bit addressing is not shift-and-mask based")
    if "__builtin_ctzll( params->kuser_size )" not in source:
        fail("process initialization does not precompute the page shift")
    if "(params->kuser_size & (params->kuser_size - 1))" not in source:
        fail("power-of-two validation for the page size was removed")

    writer = function_body(source, "write_context")
    reader = function_body(source, "read_context")
    if writer.count("uc_reg_write_batch") != 1 or "uc_reg_write(" in writer:
        fail("write_context() is not a single batch API boundary")
    if reader.count("uc_reg_read_batch") != 1 or "uc_reg_read(" in reader:
        fail("read_context() is not a single batch API boundary")
    if "context_write_regs" not in writer or "context_read_regs" not in reader:
        fail("context transfer does not use the closed register layouts")

    rng = random.Random(0x5A17C0DE)
    bitmap = [rng.getrandbits(64) for _ in range(4096)]
    for page_size in (4096, 16384, 65536):
        shift = page_size.bit_length() - 1
        highest = page_size * len(bitmap) * 64 - 1
        probes = [0, page_size - 1, page_size, highest - 1, highest]
        probes.extend(rng.randrange(highest + 1) for _ in range(20000))
        for address in probes:
            page_div = address // page_size
            page_shift = address >> shift
            old = (bitmap[page_div // 64] >> (page_div & 63)) & 1
            new = (bitmap[page_shift >> 6] >> (page_shift & 63)) & 1
            if page_div != page_shift or old != new:
                fail(
                    f"classifier mismatch for page size {page_size} "
                    f"at address {address:#x}"
                )

    print("xtajit64 Apple Silicon hot-path contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
