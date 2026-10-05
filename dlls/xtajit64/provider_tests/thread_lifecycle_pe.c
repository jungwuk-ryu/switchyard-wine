/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Identical native/x64 source: bounded thread lifecycle cost, not game timing. */
#if 0
#pragma makedep standalone
#endif
#include <windows.h>

#ifdef THREAD_LIFECYCLE_LONG
#define SERIAL_ROUNDS 512
#define BATCH_ROUNDS 64
#else
#define SERIAL_ROUNDS 32
#define BATCH_ROUNDS 4
#endif

static LONG completed;
static DWORD parent_id;

static DWORD WINAPI worker(void *argument)
{
    DWORD index = (DWORD)(ULONG_PTR)argument;
    if (!GetCurrentThreadId() || GetCurrentThreadId() == parent_id) return 90;
    InterlockedIncrement(&completed);
    return index + 1;
}

static void run_batch(unsigned int count)
{
    HANDLE threads[8];
    DWORD status;
    unsigned int i;

    for (i = 0; i < count; ++i)
    {
        threads[i] = CreateThread(NULL, 0, worker, (void *)(ULONG_PTR)i, 0, NULL);
        if (!threads[i]) ExitProcess(11);
    }
    if (WaitForMultipleObjects(count, threads, TRUE, 10000) != WAIT_OBJECT_0) ExitProcess(12);
    for (i = 0; i < count; ++i)
        if (!GetExitCodeThread(threads[i], &status) || status != i + 1 || !CloseHandle(threads[i]))
            ExitProcess(13);
}

static void ticks(const char *label, DWORD length, ULONGLONG value)
{
    char digits[32];
    DWORD written, offset = sizeof(digits);
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);

    digits[--offset] = '\n';
    do
    {
        digits[--offset] = '0' + value % 10;
        value /= 10;
    } while (value);
    if (!WriteFile(error, label, length, &written, NULL) || written != length ||
        !WriteFile(error, digits + offset, sizeof(digits) - offset, &written, NULL) ||
        written != sizeof(digits) - offset) ExitProcess(14);
}

void mainCRTStartup(void)
{
#ifdef THREAD_LIFECYCLE_LONG
    static const char text[] = "THREAD_LIFECYCLE_PASS serial=512 batches=64x8 joined=1024\n";
#else
    static const char text[] = "THREAD_LIFECYCLE_PASS serial=32 batches=4x8 joined=64\n";
#endif
    LARGE_INTEGER frequency, begin, serial_end, end;
    DWORD written;
    unsigned int i;

    parent_id = GetCurrentThreadId();
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&begin)) ExitProcess(10);
    for (i = 0; i < SERIAL_ROUNDS; ++i) run_batch(1);
    if (!QueryPerformanceCounter(&serial_end)) ExitProcess(10);
    for (i = 0; i < BATCH_ROUNDS; ++i) run_batch(8);
    if (!QueryPerformanceCounter(&end) || serial_end.QuadPart <= begin.QuadPart ||
        end.QuadPart <= serial_end.QuadPart || completed != SERIAL_ROUNDS + BATCH_ROUNDS * 8) ExitProcess(15);
#define TICKS(label, value) ticks(label, sizeof(label) - 1, value)
    TICKS("THREAD_LIFECYCLE_V1 frequency=", frequency.QuadPart);
    TICKS("THREAD_LIFECYCLE_V1 serial_ticks=", serial_end.QuadPart - begin.QuadPart);
    TICKS("THREAD_LIFECYCLE_V1 batch_ticks=", end.QuadPart - serial_end.QuadPart);
#undef TICKS
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, sizeof(text) - 1, &written, NULL) ||
        written != sizeof(text) - 1) ExitProcess(14);
    ExitProcess(0);
}
