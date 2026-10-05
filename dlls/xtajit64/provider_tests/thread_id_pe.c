/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static DWORD (WINAPI *thread_id)(void);
static HANDLE start_event;
static DWORD ids[8];

static DWORD teb_thread_id(void)
{
    DWORD value;
    __asm__ volatile("movl %%gs:0x48,%0" : "=r" (value));
    return value;
}

static DWORD WINAPI worker(void *argument)
{
    unsigned int index = (unsigned int)(uintptr_t)argument, repeat;
    DWORD expected = teb_thread_id();

    if (!expected || WaitForSingleObject(start_event, 10000) != WAIT_OBJECT_0) return 1;
    SetLastError(0x12340000 + index);
    for (repeat = 0; repeat < 4096; ++repeat)
        if (GetCurrentThreadId() != expected || thread_id() != expected ||
            GetLastError() != 0x12340000 + index)
            return 1;
    ids[index] = expected;
    return 0;
}

int main(int argc, char **argv)
{
    HANDLE threads[8] = {0};
    HMODULE module = GetModuleHandleA("kernel32.dll");
    FARPROC proc = GetProcAddress(module, "GetCurrentThreadId");
    unsigned int created = 0, index, other;
    DWORD exit_code;
    LARGE_INTEGER frequency, begin, ready, end;
    int passed = 1;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--require-leaf"))) return 2;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&begin)) return 1;
    _Static_assert(sizeof(proc) == sizeof(thread_id), "Windows function pointer size");
    /* FARPROC is an address carrier; call only through the API's real type. */
    memcpy(&thread_id, &proc, sizeof(thread_id));
    if (!thread_id || thread_id() != teb_thread_id()) return 1;
    if (argc == 2)
    {
        static const unsigned char expected[] = {0x65,0x8b,0x04,0x25,0x48,0,0,0,0xc3};
        unsigned char code[sizeof(expected)];
        uintptr_t address;
        SIZE_T read;

        _Static_assert(sizeof(address) == sizeof(proc), "Windows code address size");
        memcpy(&address, &proc, sizeof(address));
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)address, code, sizeof(code), &read) ||
            read != sizeof(code) || memcmp(code, expected, sizeof(code)))
        {
            fputs("GetCurrentThreadId is not the x64 TEB leaf export\n", stderr);
            return 3;
        }
    }
    start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!start_event) return 1;
    for (; created < 8; ++created)
    {
        threads[created] = CreateThread(NULL, 0, worker, (void *)(uintptr_t)created, 0, NULL);
        if (!threads[created]) { passed = 0; break; }
    }
    if (!QueryPerformanceCounter(&ready) || !SetEvent(start_event)) ExitProcess(4);
    /* Do not close a handle while a live worker could still be waiting on it. */
    if (created && WaitForMultipleObjects(created, threads, TRUE, 15000) != WAIT_OBJECT_0)
        ExitProcess(4);
    if (!QueryPerformanceCounter(&end) || ready.QuadPart < begin.QuadPart ||
        end.QuadPart < ready.QuadPart) ExitProcess(4);
    for (index = 0; index < created; ++index)
    {
        if (!GetExitCodeThread(threads[index], &exit_code) || exit_code)
            passed = 0;
        if (!CloseHandle(threads[index])) passed = 0;
    }
    if (!CloseHandle(start_event)) passed = 0;
    if (!passed) return 1;
    for (index = 0; index < 8; ++index)
    {
        if (!ids[index]) return 1;
        for (other = index + 1; other < 8; ++other)
            if (ids[index] == ids[other]) return 1;
    }
    fprintf(stderr, "THREAD_ID_PHASE_V1 frequency=%" PRId64 " setup_ticks=%" PRId64
            " threads_ticks=%" PRId64 "\n", (int64_t)frequency.QuadPart,
            (int64_t)(ready.QuadPart - begin.QuadPart), (int64_t)(end.QuadPart - ready.QuadPart));
    puts("THREAD_ID_PASS direct export teb last-error threads=8");
    return 0;
}
