/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Build as ARM64 or ARM64EC without a CRT to test native direct and indirect
 * callers of the combined kernel32 image, not the Wine inline API helper.
 */
#include <windows.h>

static DWORD teb_thread_id(void)
{
    const BYTE *teb;
    __asm__ volatile("mov %0, x18" : "=r" (teb));
    return *(const DWORD *)(teb + 0x48);
}

static void print_ticks(const char *label, SIZE_T length, ULONGLONG value)
{
    char number[32];
    DWORD written;
    SIZE_T index = sizeof(number);
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);

    number[--index] = '\n';
    do
    {
        number[--index] = '0' + value % 10;
        value /= 10;
    } while (value);
    if (!WriteFile(error, label, length, &written, NULL) || written != length ||
        !WriteFile(error, number + index, sizeof(number) - index, &written, NULL) ||
        written != sizeof(number) - index)
        ExitProcess(3);
}

void mainCRTStartup(void)
{
    static const char success[] = "THREAD_ID_NATIVE_PASS direct export teb last-error\n";
    HMODULE module = GetModuleHandleA("kernel32.dll");
    FARPROC address = GetProcAddress(module, "GetCurrentThreadId");
    DWORD (WINAPI *thread_id)(void);
    DWORD expected = teb_thread_id(), written;
    LARGE_INTEGER frequency, begin, direct_end, indirect_end;
    unsigned int repeat;

    _Static_assert(sizeof(address) == sizeof(thread_id), "Windows function pointer size");
    __builtin_memcpy(&thread_id, &address, sizeof(thread_id));
    if (!expected || !thread_id) ExitProcess(1);
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&begin)) ExitProcess(1);
    SetLastError(0x13572468);
    for (repeat = 0; repeat < 4096; ++repeat)
        if (GetCurrentThreadId() != expected || GetLastError() != 0x13572468)
            ExitProcess(2);
    if (!QueryPerformanceCounter(&direct_end)) ExitProcess(1);
    SetLastError(0x13572468);
    for (repeat = 0; repeat < 4096; ++repeat)
        if (thread_id() != expected || GetLastError() != 0x13572468) ExitProcess(2);
    if (!QueryPerformanceCounter(&indirect_end) || direct_end.QuadPart < begin.QuadPart ||
        indirect_end.QuadPart < direct_end.QuadPart) ExitProcess(1);
#define PRINT_TICKS(label, value) print_ticks(label, sizeof(label) - 1, value)
    PRINT_TICKS("THREAD_ID_NATIVE_V1 frequency=", frequency.QuadPart);
    PRINT_TICKS("THREAD_ID_NATIVE_V1 direct_ticks=", direct_end.QuadPart - begin.QuadPart);
    PRINT_TICKS("THREAD_ID_NATIVE_V1 indirect_ticks=", indirect_end.QuadPart - direct_end.QuadPart);
#undef PRINT_TICKS
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), success, sizeof(success) - 1, &written, NULL) ||
        written != sizeof(success) - 1)
        ExitProcess(3);
    ExitProcess(0);
}
