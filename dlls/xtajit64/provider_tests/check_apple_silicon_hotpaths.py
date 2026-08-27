#!/usr/bin/env python3
"""Static contracts for measured xtajit64 Unicorn hot paths."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import NoReturn


def fail(message: str) -> NoReturn:
    raise SystemExit(f"xtajit64 Apple Silicon hot-path contract failed: {message}")


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
    syscall = function_body(source, "prepare_x64_syscall_engine")

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
        fail("context register layout compile-time assertions are missing")

    if syscall.count("uc_reg_read_batch") != 1 or "uc_reg_read(" in syscall:
        fail("prepare_x64_syscall_engine() does not batch its register import")
    if syscall.count("uc_reg_write_batch") != 1:
        fail("prepare_x64_syscall_engine() does not batch its valid-path export")
    if syscall.count("uc_reg_write(") != 1:
        fail("invalid-service fallback must retain exactly one scalar RAX write")
    if "x64_syscall_read_regs" not in syscall or "x64_syscall_write_regs" not in syscall:
        fail("x64 syscall continuation does not use closed register layouts")
    if source.count("C_ASSERT( ARRAY_SIZE(x64_syscall_") != 2:
        fail("x64 syscall register layout compile-time assertions are missing")

    normalized = re.sub(r"\s+", " ", source)
    if "UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_REG_R10" not in normalized:
        fail("x64 syscall import order changed")
    if "UC_X86_REG_RCX, UC_X86_REG_R10, UC_X86_REG_RIP" not in normalized:
        fail("x64 syscall export order changed")

    print("xtajit64 Apple Silicon hot-path contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
