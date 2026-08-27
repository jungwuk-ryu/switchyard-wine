#!/usr/bin/env python3
"""Static contracts for the xtajit64 Unicorn context-transfer hot path."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import NoReturn


def fail(message: str) -> NoReturn:
    raise SystemExit(f"xtajit64 context-transfer contract failed: {message}")


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

    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    writer = function_body(source, "write_context")
    reader = function_body(source, "read_context")

    if writer.count("uc_reg_write_batch") != 1 or "uc_reg_write(" in writer:
        fail("write_context() is not a single batch API boundary")
    if reader.count("uc_reg_read_batch") != 1 or "uc_reg_read(" in reader:
        fail("read_context() is not a single batch API boundary")
    if "context_write_regs" not in writer or "context_read_regs" not in reader:
        fail("context transfer does not use closed register layouts")
    if "XTAJIT64_CONTEXT_INTEGER_REG_COUNT = 18" not in source:
        fail("integer register count contract is missing")
    if "XTAJIT64_CONTEXT_XMM_REG_COUNT = 16" not in source:
        fail("XMM register count contract is missing")
    if source.count("C_ASSERT( ARRAY_SIZE(context_") != 2:
        fail("register layout compile-time assertions are missing")

    print("xtajit64 Unicorn context-transfer contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
