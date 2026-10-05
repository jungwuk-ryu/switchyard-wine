/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ARM64EC VirtualAlloc code is x64 unless EC_CODE is requested. Updating one
 * guest page must preserve executable neighbors without requiring host RWX.
 */
#include <windows.h>

static DWORD invoke(const void *address)
{
    DWORD (WINAPI *function)(void);

    _Static_assert(sizeof(function) == sizeof(address), "Windows code pointer size");
    __builtin_memcpy(&function, &address, sizeof(function));
    return function();
}

static void check_native_siblings(const SYSTEM_INFO *info)
{
    void *(WINAPI *allocate)(HANDLE, void *, SIZE_T, DWORD, DWORD, MEM_EXTENDED_PARAMETER *, ULONG);
    FARPROC address = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2");
    MEM_EXTENDED_PARAMETER parameter = {0};
    static const DWORD code[] = {0x52824680, 0xd65f03c0}; /* mov w0,#0x1234; ret */
    BYTE *memory;
    DWORD old_protect;
    SIZE_T i;

    _Static_assert(sizeof(allocate) == sizeof(address), "Windows API pointer size");
    __builtin_memcpy(&allocate, &address, sizeof(allocate));
    if (!allocate) ExitProcess(26);
    parameter.Type = MemExtendedParameterAttributeFlags;
    parameter.ULong64 = MEM_EXTENDED_PARAMETER_EC_CODE;
    memory = allocate(GetCurrentProcess(), NULL, info->dwAllocationGranularity,
                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
    if (!memory) ExitProcess(27);
    for (i = 0; i < sizeof(code); ++i)
        memory[i] = memory[info->dwPageSize + i] = ((const BYTE *)code)[i];
    if (!VirtualProtect(memory, info->dwAllocationGranularity, PAGE_EXECUTE_READ, &old_protect) ||
        !FlushInstructionCache(GetCurrentProcess(), memory, info->dwAllocationGranularity)) ExitProcess(28);
    if (invoke(memory) != 0x1234 || invoke(memory + info->dwPageSize) != 0x1234) ExitProcess(29);

    /* On a host with larger pages this cannot succeed without removing X
     * from native neighbors. Failure must leave both functions executable.
     * A host supporting separate Windows pages may legitimately succeed. */
    if (VirtualProtect(memory, sizeof(code), PAGE_READWRITE, &old_protect))
    {
        if (invoke(memory + info->dwPageSize) != 0x1234) ExitProcess(30);
        if (!VirtualProtect(memory, sizeof(code), old_protect, &old_protect)) ExitProcess(31);
    }
    else if (GetLastError() != ERROR_ACCESS_DENIED) ExitProcess(32);
    if (invoke(memory) != 0x1234 || invoke(memory + info->dwPageSize) != 0x1234) ExitProcess(33);
    if (!VirtualFree(memory, 0, MEM_RELEASE)) ExitProcess(34);
}

void mainCRTStartup(void)
{
    static const BYTE first[] = {0xb8, 0x44, 0x33, 0x22, 0x11, 0xc3};
    static const BYTE second[] = {0xb8, 0x88, 0x77, 0x66, 0x55, 0xc3};
    static const char failure[] = "PRIVATE_CODE_REPROTECT_FAIL subpage-rx-to-rw\n";
    static const char success[] = "PRIVATE_CODE_REPROTECT_PASS subpage neighbor reuse native-preserved\n";
    SYSTEM_INFO info;
    MEMORY_BASIC_INFORMATION before, after;
    BYTE *memory, *reused;
    SIZE_T i, size;
    DWORD old_protect, written;

    GetSystemInfo(&info);
    size = info.dwAllocationGranularity;
    if (info.dwPageSize < sizeof(first) || size / info.dwPageSize < 2) ExitProcess(10);
    memory = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!memory) ExitProcess(11);
    for (i = 0; i < sizeof(first); ++i)
        memory[i] = memory[info.dwPageSize + i] = first[i];
    if (!VirtualProtect(memory, size, PAGE_EXECUTE_READ, &old_protect) ||
        !FlushInstructionCache(GetCurrentProcess(), memory, size)) ExitProcess(12);
    if (invoke(memory) != 0x11223344 || invoke(memory + info.dwPageSize) != 0x11223344) ExitProcess(13);
    if (VirtualQuery(memory, &before, sizeof(before)) != sizeof(before)) ExitProcess(14);
    if (!VirtualProtect(memory, sizeof(first), PAGE_READWRITE, &old_protect))
    {
        DWORD error = GetLastError();

        if (VirtualQuery(memory, &after, sizeof(after)) != sizeof(after) ||
            before.Protect != after.Protect || invoke(memory) != 0x11223344 ||
            invoke(memory + info.dwPageSize) != 0x11223344) ExitProcess(15);
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), failure, sizeof(failure) - 1, &written, NULL);
        ExitProcess(error == ERROR_ACCESS_DENIED ? 16 : 17);
    }
    for (i = 0; i < sizeof(second); ++i) memory[i] = second[i];
    if (!VirtualProtect(memory, sizeof(second), old_protect, &old_protect) ||
        !FlushInstructionCache(GetCurrentProcess(), memory, sizeof(second))) ExitProcess(18);
    if (invoke(memory) != 0x55667788 || invoke(memory + info.dwPageSize) != 0x11223344) ExitProcess(19);
    if (!VirtualFree(memory, 0, MEM_RELEASE)) ExitProcess(20);
    reused = VirtualAlloc(memory, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (reused != memory) ExitProcess(21);
    for (i = 0; i < sizeof(first); ++i) reused[i] = first[i];
    if (!VirtualProtect(reused, size, PAGE_EXECUTE_READ, &old_protect) ||
        !FlushInstructionCache(GetCurrentProcess(), reused, sizeof(first))) ExitProcess(22);
    if (invoke(reused) != 0x11223344) ExitProcess(23);
    if (!VirtualFree(reused, 0, MEM_RELEASE)) ExitProcess(24);
    check_native_siblings(&info);
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), success, sizeof(success) - 1, &written, NULL) ||
        written != sizeof(success) - 1) ExitProcess(25);
    ExitProcess(0);
}
