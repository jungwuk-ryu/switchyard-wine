from pathlib import Path

path = Path("dlls/xtajit64/unixlib.c")
source = path.read_text(encoding="utf-8")
old = '''static uc_err prepare_x64_syscall_engine( struct thread_engine *engine,
                                          uint64_t dispatcher, uint32_t count,
                                          uint64_t *next_rip )
{
    uint64_t rax, r10, rip;
    uc_err err;

    if ((err = uc_reg_read( engine->uc, UC_X86_REG_RAX, &rax )) != UC_ERR_OK)
        return err;
    if ((err = uc_reg_read( engine->uc, UC_X86_REG_RIP, &rip )) != UC_ERR_OK)
        return err;
    if (rax >= count)
    {
        rax = (uint64_t)(int64_t)STATUS_INVALID_SYSTEM_SERVICE;
        if ((err = uc_reg_write( engine->uc, UC_X86_REG_RAX, &rax )) != UC_ERR_OK)
            return err;
        *next_rip = rip;
        return UC_ERR_OK;
    }
    if ((err = uc_reg_read( engine->uc, UC_X86_REG_R10, &r10 )) != UC_ERR_OK)
        return err;

    /* Match ntdll's ARM64EC STATUS_EMULATION_SYSCALL conversion.  Unicorn
     * reports RIP after both SYSCALL and INT 2E once the stop hook returns. */
    if ((err = uc_reg_write( engine->uc, UC_X86_REG_RCX, &r10 )) != UC_ERR_OK ||
        (err = uc_reg_write( engine->uc, UC_X86_REG_R10, &rip )) != UC_ERR_OK ||
        (err = uc_reg_write( engine->uc, UC_X86_REG_RIP, &dispatcher )) != UC_ERR_OK)
        return err;
    *next_rip = dispatcher;
    return UC_ERR_OK;
}
'''
new = '''static const int syscall_read_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_REG_R10,
};

static const int syscall_dispatch_regs[] =
{
    UC_X86_REG_RCX, UC_X86_REG_R10, UC_X86_REG_RIP,
};

C_ASSERT( ARRAY_SIZE(syscall_read_regs) == 3 );
C_ASSERT( ARRAY_SIZE(syscall_dispatch_regs) == 3 );

static uc_err prepare_x64_syscall_engine( struct thread_engine *engine,
                                          uint64_t dispatcher, uint32_t count,
                                          uint64_t *next_rip )
{
    uint64_t rax, r10, rip;
    void *read_values[] = {&rax, &rip, &r10};
    void *dispatch_values[] = {&r10, &rip, &dispatcher};
    uc_err err;

    /* This path executes for every translated x64 syscall.  Enter Unicorn
     * once for the stopped ABI register set instead of six times around the
     * same CPU state.  The array order preserves the scalar operation order. */
    if ((err = uc_reg_read_batch( engine->uc, syscall_read_regs, read_values,
                                  ARRAY_SIZE(syscall_read_regs) )) != UC_ERR_OK)
        return err;
    if (rax >= count)
    {
        rax = (uint64_t)(int64_t)STATUS_INVALID_SYSTEM_SERVICE;
        if ((err = uc_reg_write_batch( engine->uc, syscall_read_regs, read_values,
                                       1 )) != UC_ERR_OK)
            return err;
        *next_rip = rip;
        return UC_ERR_OK;
    }

    /* Match ntdll's ARM64EC STATUS_EMULATION_SYSCALL conversion.  Unicorn
     * reports RIP after both SYSCALL and INT 2E once the stop hook returns. */
    if ((err = uc_reg_write_batch( engine->uc, syscall_dispatch_regs,
                                   dispatch_values,
                                   ARRAY_SIZE(syscall_dispatch_regs) )) != UC_ERR_OK)
        return err;
    *next_rip = dispatcher;
    return UC_ERR_OK;
}
'''
count = source.count(old)
if count != 1:
    raise SystemExit(f"expected one syscall helper, found {count}")
path.write_text(source.replace(old, new, 1), encoding="utf-8")
