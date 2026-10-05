/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual dispatcher/virtual_unwind source is supplied by the companion builder. */
#if 0
#pragma makedep standalone
#endif
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>

#define WARN(...) ((void)0)

extern const BYTE boundary_dispatch_start[], dispatch_prepared_exception[];

/* These targets are linked only to assemble the real dispatcher. Never run it. */
void *boundary_prepare(EXCEPTION_RECORD *rec, ARM64EC_NT_CONTEXT *ctx, ARM64_NT_CONTEXT *arm)
{
    (void)rec;
    (void)ctx;
    (void)arm;
    ExitProcess(90);
}

void boundary_dispatch(void)
{
    ExitProcess(91);
}

#include "exception_unwind_source.inc"

static struct
{
    ARM64EC_NT_CONTEXT prepared;
    ARM64_NT_CONTEXT original;
} saved;

C_ASSERT(sizeof(ARM64EC_NT_CONTEXT) == 0x4d0);
C_ASSERT(FIELD_OFFSET(__typeof__(saved), original) == 0x4d0);

static void check(ULONG_PTR pc, ULONG_PTR sp, BOOL from_call, BOOL prepared)
{
    ARM64EC_NT_CONTEXT ctx = {0};
    DISPATCHER_CONTEXT_ARM64EC dispatch = {0};
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 nonvolatile;

    ctx.ContextFlags = CONTEXT_FULL | (from_call ? CONTEXT_UNWOUND_TO_CALL : 0);
    ctx.Pc = pc;
    ctx.Sp = sp;
    dispatch.NonVolatileRegisters = (BYTE *)&nonvolatile;
    dispatch.ContextRecord = &ctx.AMD64_Context;
    if (!RtlIsEcCode(pc)) ExitProcess(10);
    if (boundary_virtual_unwind(UNW_FLAG_NHANDLER, &dispatch, &ctx)) ExitProcess(11);
    if (ctx.Pc != (prepared ? saved.prepared.Pc : saved.original.Pc) ||
        ctx.Sp != (prepared ? saved.prepared.Sp : saved.original.Sp) ||
        ctx.X19 != (prepared ? saved.prepared.X19 : saved.original.X19) ||
        ctx.X27 != (prepared ? saved.prepared.X27 : saved.original.X27) ||
        ctx.V[8].Low != (prepared ? saved.prepared.V[8].Low : saved.original.V[8].Low) ||
        ctx.V[8].High != (prepared ? saved.prepared.V[8].High : saved.original.V[8].High) ||
        (ctx.ContextFlags & CONTEXT_UNWOUND_TO_CALL)) ExitProcess(12);
}

void mainCRTStartup(void)
{
    static const char message[] = "EXCEPTION_PHASE_PASS prologue prepare-return prepared-fault prepared-return\n";
    ULONG_PTR begin = (ULONG_PTR)boundary_dispatch_start;
    ULONG_PTR ready = (ULONG_PTR)dispatch_prepared_exception;
    ULONG_PTR pc;
    DWORD written;

    saved.original.ContextFlags = CONTEXT_ARM64_FULL;
    saved.original.Pc = 0x11223344;
    saved.original.Sp = 0x22334450;
    saved.original.X19 = 0x19345678;
    saved.original.X27 = 0x27345678;
    saved.original.V[8].Low = 0x81234567;
    saved.original.V[8].High = 0x82345678;
    saved.prepared.ContextFlags = CONTEXT_FULL;
    saved.prepared.Pc = 0x55667788;
    saved.prepared.Sp = 0x66778890;
    saved.prepared.X19 = 0x19876543;
    saved.prepared.X27 = 0x27876543;
    saved.prepared.V[8].Low = 0x88765432;
    saved.prepared.V[8].High = 0x87654321;

    /* SUB plus three argument instructions plus BL. No stale-context branch. */
    if (ready - begin != 20) ExitProcess(13);
    check(begin, (ULONG_PTR)&saved.original, FALSE, FALSE);
    for (pc = begin + 4; pc < ready; pc += 4)
        check(pc, (ULONG_PTR)&saved.prepared, FALSE, FALSE);
    check(ready, (ULONG_PTR)&saved.prepared, TRUE, FALSE);
    check(ready, (ULONG_PTR)&saved.prepared, FALSE, TRUE);
    check(ready + 4, (ULONG_PTR)&saved.prepared, TRUE, TRUE);
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, NULL) ||
        written != sizeof(message) - 1) ExitProcess(14);
    ExitProcess(0);
}
