/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Compile with Clang MS extensions: actual x64 SEH scopes, not setjmp models. */
#if 0
#pragma makedep standalone
#endif
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define SOFTWARE_CODE 0xe0426411u
#define NESTED_CODE 0xe0426412u
#define ARGUMENT_VALUE UINT64_C(0x13579bdf2468ace0)
#define DEPTH 8
#define REPEATS 4

static void *fault_page;
static volatile LONG finally_count, filter_count, caught_count, nested_count, outer_veh_count;
static unsigned int expected_depth;
static int test_kind;

static void fail(const char *where)
{
    fprintf(stderr, "SEH_UNWIND_FAIL %s kind=%d finally=%ld filter=%ld caught=%ld nested=%ld error=%lu\n",
            where, test_kind, finally_count, filter_count, caught_count, nested_count, GetLastError());
    ExitProcess(1);
}

extern DWORD seh_read(const void *address);
extern const char seh_read_fault[];
__asm__(".text\n.p2align 4\n.globl seh_read\n.def seh_read; .scl 2; .type 32; .endef\n"
        "seh_read:\n.globl seh_read_fault\nseh_read_fault:\nmov (%rcx),%eax\nret\n");

static LONG CALLBACK vectored(EXCEPTION_POINTERS *exception)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    const ULONG_PTR argument = ARGUMENT_VALUE;

    if (record->ExceptionCode == NESTED_CODE)
    {
        if (record->NumberParameters != 1 || record->ExceptionInformation[0] != ARGUMENT_VALUE)
            fail("nested software arguments");
        ++nested_count;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (test_kind == 2 && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        exception->ContextRecord->Rip == (uintptr_t)seh_read_fault)
    {
        ++outer_veh_count;
        /* Reenter native exception APIs while the outer x64 VEH is active. */
        RaiseException(NESTED_CODE, 0, 1, &argument);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int filter(EXCEPTION_POINTERS *exception)
{
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;

    ++filter_count;
    if (!test_kind)
    {
        if (record->ExceptionCode != SOFTWARE_CODE || record->NumberParameters != 1 ||
            record->ExceptionInformation[0] != ARGUMENT_VALUE)
            fail("software record");
    }
    else if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters != 2 ||
             record->ExceptionInformation[0] != 0 || record->ExceptionInformation[1] != (uintptr_t)fault_page ||
             (uintptr_t)record->ExceptionAddress != (uintptr_t)seh_read_fault ||
             exception->ContextRecord->Rip != (uintptr_t)seh_read_fault)
        fail("hardware record/context");
    return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) static void unwind_frames(unsigned int depth)
{
    volatile uint64_t canary[16];
    const ULONG_PTR argument = ARGUMENT_VALUE;
    unsigned int i;

    for (i = 0; i < 16; ++i) canary[i] = ARGUMENT_VALUE ^ ((uint64_t)depth << 32) ^ i;
    __try
    {
        if (depth) unwind_frames(depth - 1);
        else if (!test_kind) RaiseException(SOFTWARE_CODE, 0, 1, &argument);
        else seh_read(fault_page);
        fail("fault unexpectedly returned");
    }
    __finally
    {
        if (!AbnormalTermination() || depth != expected_depth++) fail("unwind scope order");
        for (i = 0; i < 16; ++i)
            if (canary[i] != (ARGUMENT_VALUE ^ ((uint64_t)depth << 32) ^ i)) fail("unwound stack canary");
        ++finally_count;
    }
}

int main(void)
{
    void *handler;
    unsigned int repeat;
    LONG before;

    fault_page = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!fault_page) fail("allocation");
    handler = AddVectoredExceptionHandler(1, vectored);
    if (!handler) fail("VEH registration");
    for (test_kind = 0; test_kind < 3; ++test_kind)
    {
        for (repeat = 0; repeat < REPEATS; ++repeat)
        {
            before = finally_count;
            expected_depth = 0;
            __try
            {
                unwind_frames(DEPTH);
                fail("no outer exception");
            }
            __except(filter(GetExceptionInformation()))
            {
                ++caught_count;
            }
            if (finally_count - before != DEPTH + 1 || expected_depth != DEPTH + 1)
                fail("missing scope cleanup");
        }
    }
    if (filter_count != 3 * REPEATS || caught_count != 3 * REPEATS ||
        nested_count != REPEATS || outer_veh_count != REPEATS) fail("handler counts");
    if (!RemoveVectoredExceptionHandler(handler) || !VirtualFree(fault_page, 0, MEM_RELEASE)) fail("cleanup");
    puts("SEH_UNWIND_PASS software access-violation nested-veh scopes=108 catches=12");
    return 0;
}
