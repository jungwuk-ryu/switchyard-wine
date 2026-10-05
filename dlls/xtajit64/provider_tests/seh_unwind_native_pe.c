/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native ARM64EC exception control: no emulator-private interfaces or CRT. */
#if 0
#pragma makedep standalone
#endif
#include <windows.h>

#define NATIVE_CODE 0xe0426413u
static volatile LONG scopes, catches;
static unsigned int expected_depth;
static BOOL hardware;
static void *page;

static void fail(DWORD code)
{
    ExitProcess(code);
}

__declspec(noinline) static ULONG fault_read(void)
{
    return *(volatile ULONG *)page;
}

__declspec(noinline) static void frames(unsigned int depth)
{
    volatile ULONGLONG canary[16];
    unsigned int i;

    for (i = 0; i < 16; ++i) canary[i] = 0x13579bdf2468ace0ull ^ ((ULONGLONG)depth << 32) ^ i;
    __try
    {
        if (depth) frames(depth - 1);
        else if (hardware) fault_read();
        else RaiseException(NATIVE_CODE, 0, 0, NULL);
        fail(11);
    }
    __finally
    {
        if (!AbnormalTermination() || depth != expected_depth++) fail(12);
        for (i = 0; i < 16; ++i)
            if (canary[i] != (0x13579bdf2468ace0ull ^ ((ULONGLONG)depth << 32) ^ i)) fail(13);
        ++scopes;
    }
}

static LONG filter(EXCEPTION_POINTERS *exception)
{
    EXCEPTION_RECORD *rec = exception->ExceptionRecord;

    if ((!hardware && rec->ExceptionCode != NATIVE_CODE) ||
        (hardware && (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
                      rec->NumberParameters != 2 || rec->ExceptionInformation[0] != 0 ||
                      rec->ExceptionInformation[1] != (ULONG_PTR)page))) fail(14);
    return EXCEPTION_EXECUTE_HANDLER;
}

void mainCRTStartup(void)
{
    static const char message[] = "SEH_NATIVE_PASS software access-violation scopes=72 catches=8\n";
    unsigned int mode, repeat;
    DWORD written;

    page = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!page) fail(10);
    for (mode = 0; mode < 2; ++mode)
    {
        hardware = mode;
        for (repeat = 0; repeat < 4; ++repeat)
        {
            expected_depth = 0;
            __try
            {
                frames(8);
                fail(15);
            }
            __except(filter(GetExceptionInformation()))
            {
                ++catches;
            }
            if (expected_depth != 9) fail(16);
        }
    }
    if (scopes != 72 || catches != 8 || !VirtualFree(page, 0, MEM_RELEASE)) fail(17);
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, NULL) ||
        written != sizeof(message) - 1) fail(18);
    ExitProcess(0);
}
