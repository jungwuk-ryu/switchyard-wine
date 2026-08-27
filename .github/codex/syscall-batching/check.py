#!/usr/bin/env python3

from pathlib import Path
import sys


def fail(message: str) -> None:
    raise SystemExit(f"xtajit64 syscall batching contract failed: {message}")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: check.py dlls/xtajit64/unixlib.c")
    source = Path(sys.argv[1]).read_text(encoding="utf-8")

    start_marker = "static uc_err prepare_x64_syscall_engine("
    end_marker = "\n}\n\nstatic uc_err create_pool_engine_locked("
    try:
        start = source.index(start_marker)
        end = source.index(end_marker, start) + 3
    except ValueError as error:
        fail(f"cannot locate syscall helper: {error}")
    helper = source[start:end]

    required = (
        "uc_reg_read_batch( engine->uc, syscall_read_regs, read_values,",
        "uc_reg_write_batch( engine->uc, syscall_read_regs, read_values,",
        "uc_reg_write_batch( engine->uc, syscall_dispatch_regs,",
        "ARRAY_SIZE(syscall_read_regs)",
        "ARRAY_SIZE(syscall_dispatch_regs)",
    )
    for token in required:
        if token not in helper:
            fail(f"missing token {token!r}")
    for forbidden in ("uc_reg_read(", "uc_reg_write("):
        if forbidden in helper:
            fail(f"scalar Unicorn register call remains: {forbidden}")

    read_array = "UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_REG_R10,"
    write_array = "UC_X86_REG_RCX, UC_X86_REG_R10, UC_X86_REG_RIP,"
    if source.count(read_array) != 1:
        fail("syscall read register order is absent or ambiguous")
    if source.count(write_array) != 1:
        fail("syscall dispatch register order is absent or ambiguous")
    if "void *read_values[] = {&rax, &rip, &r10};" not in helper:
        fail("syscall read values no longer match the register order")
    if "void *dispatch_values[] = {&r10, &rip, &dispatcher};" not in helper:
        fail("syscall dispatch values no longer match the register order")

    print("xtajit64 syscall register batching contracts passed")


if __name__ == "__main__":
    main()
