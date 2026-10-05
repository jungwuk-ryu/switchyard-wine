/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A writable guest page must not make its read-only executable neighbor
 * writable, even when the host maps them in one larger physical page. */
#include <windows.h>

static volatile LONG handler_calls;
static BYTE *expected_address;
extern void write_neighbor(void *address);
extern const char write_neighbor_fault[], write_neighbor_resume[];

__asm__(".text\n.p2align 4\n"
        ".globl write_neighbor\n.def write_neighbor; .scl 2; .type 32; .endef\n"
        "write_neighbor:\n"
        ".globl write_neighbor_fault\nwrite_neighbor_fault:\n"
        "movb $0xa5,(%rcx)\n"
        ".globl write_neighbor_resume\nwrite_neighbor_resume:\nret\n");

static LONG CALLBACK handle_write(EXCEPTION_POINTERS *pointers)
{
    EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;

    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        (ULONG_PTR)record->ExceptionAddress != (ULONG_PTR)write_neighbor_fault ||
        context->Rip != (ULONG_PTR)write_neighbor_fault || record->NumberParameters != 2 ||
        record->ExceptionInformation[0] != 1 ||
        record->ExceptionInformation[1] != (ULONG_PTR)expected_address)
        return EXCEPTION_CONTINUE_SEARCH;
    ++handler_calls;
    context->Rip = (ULONG_PTR)write_neighbor_resume;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void mainCRTStartup(void)
{
    static const char success[] = "PRIVATE_CODE_WRITE_GUARD_PASS rx-neighbor-write-denied\n";
    static const char failure[] = "PRIVATE_CODE_WRITE_GUARD_FAIL rx-neighbor-write-allowed\n";
    SYSTEM_INFO info;
    BYTE *memory;
    void *handler;
    DWORD old_protect, written;

    GetSystemInfo(&info);
    if (!info.dwPageSize || info.dwAllocationGranularity / info.dwPageSize < 2) ExitProcess(10);
    memory = VirtualAlloc(NULL, info.dwAllocationGranularity,
                          MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!memory) ExitProcess(11);
    expected_address = memory + info.dwPageSize;
    *expected_address = 0x5a;
    handler = AddVectoredExceptionHandler(1, handle_write);
    if (!handler) ExitProcess(12);
    if (!VirtualProtect(memory, info.dwAllocationGranularity, PAGE_EXECUTE_READ, &old_protect)) ExitProcess(13);

    /* Establish that the exact same write and handler fault before the change. */
    write_neighbor(expected_address);
    if (handler_calls != 1 || *expected_address != 0x5a) ExitProcess(14);
    if (!VirtualProtect(memory, 1, PAGE_READWRITE, &old_protect))
        ExitProcess(GetLastError() == ERROR_ACCESS_DENIED ? 16 : 17);
    write_neighbor(expected_address);
    if (!RemoveVectoredExceptionHandler(handler)) ExitProcess(18);
    if (handler_calls == 1 && *expected_address == 0xa5)
    {
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), failure, sizeof(failure) - 1, &written, NULL);
        ExitProcess(40);
    }
    if (handler_calls != 2 || *expected_address != 0x5a) ExitProcess(41);
    if (!VirtualFree(memory, 0, MEM_RELEASE)) ExitProcess(19);
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), success, sizeof(success) - 1, &written, NULL) ||
        written != sizeof(success) - 1) ExitProcess(20);
    ExitProcess(0);
}
