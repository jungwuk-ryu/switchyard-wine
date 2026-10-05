/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Internal ARM64EC provider allocation boundary; not an application workaround. */
#if 0
#pragma makedep standalone
#endif
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>

struct state_prefix
{
    UINT64 magic, allocation_size, control_stack_top;
};

static void message(const char *text, DWORD length)
{
    DWORD written;
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL) || written != length)
        ExitProcess(90);
}

static LONG filter(EXCEPTION_POINTERS *exception, const void *target)
{
    EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (record->ExceptionCode != (DWORD)STATUS_ACCESS_VIOLATION || record->NumberParameters != 2 ||
        record->ExceptionInformation[0] != 1 || record->ExceptionInformation[1] != (ULONG_PTR)target)
        ExitProcess(91);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI check_stack(void *unused)
{
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    struct state_prefix before, *state;
    MEMORY_BASIC_INFORMATION memory;
    SYSTEM_BASIC_INFORMATION system;
    BYTE *guard;
    volatile BYTE *target;
    SIZE_T native_page_size;
    unsigned int i, caught = 0;

    (void)unused;
    if (!cpu || !(state = cpu->EmulatorData[0])) return 10;
    if (NtQuerySystemInformation(SystemBasicInformation, &system, sizeof(system), NULL) ||
        system.PageSize < 4096 || system.PageSize > 65536 ||
        (system.PageSize & (system.PageSize - 1))) return 11;
    if (!VirtualQuery(state, &memory, sizeof(memory)) || memory.AllocationBase != state ||
        memory.State != MEM_COMMIT || memory.Protect != PAGE_READWRITE) return 12;
    native_page_size = memory.RegionSize;
    if (native_page_size < system.PageSize || native_page_size > 65536 ||
        (native_page_size & (native_page_size - 1))) return 12;
    before = *state;
    if (before.magic != 0x363454494a415458ull || before.allocation_size != 0x40000 ||
        (ULONG_PTR)state > ~(ULONG_PTR)0 - before.allocation_size ||
        before.control_stack_top > (ULONG_PTR)state + before.allocation_size) return 13;
    /* The provider state must fit its first native page. The second protects it
     * from the descending control stack, independently of diagnostic enablement. */
    guard = (BYTE *)state + native_page_size;
    if (!VirtualQuery(guard, &memory, sizeof(memory)) || memory.BaseAddress != guard ||
        memory.AllocationBase != state || memory.RegionSize != native_page_size ||
        memory.State != MEM_COMMIT || memory.Protect != PAGE_NOACCESS)
    {
        static const char text[] = "CONTROL_STACK_FAIL missing native-page barrier\n";
        message(text, sizeof(text) - 1);
        return 14;
    }
    if (!VirtualQuery(guard + native_page_size, &memory, sizeof(memory)) ||
        memory.AllocationBase != state || memory.State != MEM_COMMIT ||
        memory.Protect != PAGE_READWRITE) return 15;
    for (i = 0; i < 2; ++i)
    {
        target = guard + (i ? native_page_size - 1 : 0);
        __try
        {
            *target = 0x5a;
            return 16;
        }
        __except(filter(GetExceptionInformation(), (const void *)target))
        {
            ++caught;
        }
    }
    if (caught != 2 || state->magic != before.magic || state->allocation_size != before.allocation_size ||
        state->control_stack_top != before.control_stack_top) return 17;
    return 0;
}

void mainCRTStartup(void)
{
    static const char text[] = "CONTROL_STACK_PASS threads=9 barriers=18 metadata-preserved\n";
    HANDLE thread;
    DWORD status;
    unsigned int i;

    if ((status = check_stack(NULL))) ExitProcess(status);
    for (i = 0; i < 8; ++i)
    {
        if (!(thread = CreateThread(NULL, 0, check_stack, NULL, 0, NULL))) ExitProcess(20);
        if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(thread, &status) || !CloseHandle(thread)) ExitProcess(21);
        if (status) ExitProcess(status);
    }
    message(text, sizeof(text) - 1);
    ExitProcess(0);
}
