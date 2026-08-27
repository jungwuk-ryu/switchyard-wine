#!/usr/bin/env python3
"""Apply the measured xtajit64 x64-syscall register batching change."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PATH = ROOT / "dlls/xtajit64/unixlib.c"


def replace_function(source: str, name: str, replacement: str) -> str:
    marker = f"static uc_err {name}("
    start = source.find(marker)
    if start < 0:
        raise SystemExit(f"cannot find {name}()")
    brace = source.find("{", start)
    if brace < 0:
        raise SystemExit(f"cannot find body of {name}()")

    depth = 0
    end = -1
    for index in range(brace, len(source)):
        character = source[index]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                end = index + 1
                break
    if end < 0:
        raise SystemExit(f"unterminated {name}()")
    return source[:start] + replacement.rstrip() + source[end:]


def main() -> None:
    source = PATH.read_text(encoding="utf-8")

    context_read_layout = """static const int context_read_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_RSP,
    UC_X86_REG_R8,  UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
    UC_X86_REG_RIP, UC_X86_REG_EFLAGS, UC_X86_REG_MXCSR,
    UC_X86_REG_XMM0,  UC_X86_REG_XMM1,  UC_X86_REG_XMM2,  UC_X86_REG_XMM3,
    UC_X86_REG_XMM4,  UC_X86_REG_XMM5,  UC_X86_REG_XMM6,  UC_X86_REG_XMM7,
    UC_X86_REG_XMM8,  UC_X86_REG_XMM9,  UC_X86_REG_XMM10, UC_X86_REG_XMM11,
    UC_X86_REG_XMM12, UC_X86_REG_XMM13, UC_X86_REG_XMM14, UC_X86_REG_XMM15,
};
"""
    if source.count(context_read_layout) != 1:
        raise SystemExit("context read register layout changed unexpectedly")

    syscall_layouts = """
static const int x64_syscall_read_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_REG_R10,
};

static const int x64_syscall_write_regs[] =
{
    UC_X86_REG_RCX, UC_X86_REG_R10, UC_X86_REG_RIP,
};

C_ASSERT( ARRAY_SIZE(x64_syscall_read_regs) == 3 );
C_ASSERT( ARRAY_SIZE(x64_syscall_write_regs) == 3 );
"""
    source = source.replace(
        context_read_layout,
        context_read_layout + syscall_layouts,
        1,
    )

    replacement = """static uc_err prepare_x64_syscall_engine( struct thread_engine *engine,
                                          uint64_t dispatcher, uint32_t count,
                                          uint64_t *next_rip )
{
    uint64_t rax, rip, r10;
    void *read_values[] = {&rax, &rip, &r10};
    void *write_values[] = {&r10, &rip, &dispatcher};
    uc_err err;

    /* This is the normal x64 SYSCALL continuation path.  Keep Unicorn's
     * register-dispatch setup outside the three-register import and export
     * groups rather than crossing the dylib boundary six times per syscall. */
    if ((err = uc_reg_read_batch( engine->uc, x64_syscall_read_regs,
                                  read_values,
                                  (int)ARRAY_SIZE(x64_syscall_read_regs) )) != UC_ERR_OK)
        return err;
    if (rax >= count)
    {
        rax = (uint64_t)(int64_t)STATUS_INVALID_SYSTEM_SERVICE;
        if ((err = uc_reg_write( engine->uc, UC_X86_REG_RAX, &rax )) != UC_ERR_OK)
            return err;
        *next_rip = rip;
        return UC_ERR_OK;
    }

    /* Match ntdll's ARM64EC STATUS_EMULATION_SYSCALL conversion.  Unicorn
     * reports RIP after both SYSCALL and INT 2E once the stop hook returns. */
    if ((err = uc_reg_write_batch( engine->uc, x64_syscall_write_regs,
                                   write_values,
                                   (int)ARRAY_SIZE(x64_syscall_write_regs) )) != UC_ERR_OK)
        return err;
    *next_rip = dispatcher;
    return UC_ERR_OK;
}
"""
    source = replace_function(source, "prepare_x64_syscall_engine", replacement)
    PATH.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    main()
