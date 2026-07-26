/*
 * Small synchronization stress probe for Switchyard macOS MSync validation.
 *
 * This is intentionally standalone so it can be cross-compiled with MinGW and
 * run through a candidate runtime before replacing a live runtime.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#define RING_SIZE 96
#define ITERATIONS 40000

static HANDLE free_slots, filled_slots;
static volatile LONG failures;
static DWORD values[RING_SIZE];

static DWORD WINAPI producer_thread(void *arg)
{
    DWORD i;

    (void)arg;
    for (i = 0; i < ITERATIONS; ++i)
    {
        DWORD slot = i % RING_SIZE;
        if (WaitForSingleObject(free_slots, 10000) != WAIT_OBJECT_0)
        {
            InterlockedIncrement(&failures);
            return 1;
        }
        values[slot] = i + 1;
        MemoryBarrier();
        if (!ReleaseSemaphore(filled_slots, 1, NULL))
        {
            InterlockedIncrement(&failures);
            return 1;
        }
    }
    return 0;
}

static DWORD WINAPI consumer_thread(void *arg)
{
    DWORD i;

    (void)arg;
    for (i = 0; i < ITERATIONS; ++i)
    {
        DWORD slot = i % RING_SIZE;
        if (WaitForSingleObject(filled_slots, 10000) != WAIT_OBJECT_0)
        {
            InterlockedIncrement(&failures);
            return 1;
        }
        MemoryBarrier();
        if (values[slot] != i + 1)
        {
            fprintf(stderr, "slot %lu expected %lu got %lu\n",
                    (unsigned long)slot, (unsigned long)(i + 1),
                    (unsigned long)values[slot]);
            InterlockedIncrement(&failures);
            return 1;
        }
        if (!ReleaseSemaphore(free_slots, 1, NULL))
        {
            InterlockedIncrement(&failures);
            return 1;
        }
    }
    return 0;
}

static DWORD WINAPI abandon_mutex_thread(void *arg)
{
    HANDLE mutex = arg;
    if (WaitForSingleObject(mutex, 10000) != WAIT_OBJECT_0)
    {
        InterlockedIncrement(&failures);
        return 1;
    }
    return 0;
}

static int test_wait_all(void)
{
    HANDLE events[2];
    DWORD ret;

    events[0] = CreateEventW(NULL, TRUE, TRUE, NULL);
    events[1] = CreateEventW(NULL, FALSE, TRUE, NULL);
    if (!events[0] || !events[1]) return 1;

    ret = WaitForMultipleObjects(2, events, TRUE, 10000);
    CloseHandle(events[0]);
    CloseHandle(events[1]);
    return ret != WAIT_OBJECT_0;
}

static int test_signal_object_and_wait(void)
{
    HANDLE event_a = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE event_b = CreateEventW(NULL, FALSE, TRUE, NULL);
    DWORD ret;

    if (!event_a || !event_b) return 1;
    ret = SignalObjectAndWait(event_a, event_b, 10000, FALSE);
    CloseHandle(event_a);
    CloseHandle(event_b);
    return ret != WAIT_OBJECT_0;
}

static int test_abandoned_mutex(void)
{
    HANDLE thread, mutex;
    DWORD ret;

    mutex = CreateMutexW(NULL, FALSE, NULL);
    if (!mutex) return 1;

    thread = CreateThread(NULL, 0, abandon_mutex_thread, mutex, 0, NULL);
    if (!thread) return 1;
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);

    ret = WaitForSingleObject(mutex, 10000);
    CloseHandle(mutex);
    return ret != WAIT_ABANDONED;
}

int main(void)
{
    HANDLE producer, consumer;
    DWORD start = GetTickCount();
    DWORD ret;

    free_slots = CreateSemaphoreW(NULL, RING_SIZE, RING_SIZE, NULL);
    filled_slots = CreateSemaphoreW(NULL, 0, RING_SIZE, NULL);
    if (!free_slots || !filled_slots)
    {
        fprintf(stderr, "failed to create semaphores\n");
        return 1;
    }

    producer = CreateThread(NULL, 0, producer_thread, NULL, 0, NULL);
    consumer = CreateThread(NULL, 0, consumer_thread, NULL, 0, NULL);
    if (!producer || !consumer)
    {
        fprintf(stderr, "failed to create threads\n");
        return 1;
    }

    ret = WaitForSingleObject(producer, 30000);
    if (ret != WAIT_OBJECT_0)
    {
        fprintf(stderr, "producer wait failed: %lu\n", (unsigned long)ret);
        return 1;
    }
    ret = WaitForSingleObject(consumer, 30000);
    if (ret != WAIT_OBJECT_0)
    {
        fprintf(stderr, "consumer wait failed: %lu\n", (unsigned long)ret);
        return 1;
    }

    CloseHandle(producer);
    CloseHandle(consumer);
    CloseHandle(free_slots);
    CloseHandle(filled_slots);

    if (test_wait_all())
    {
        fprintf(stderr, "WaitAll failed\n");
        return 1;
    }
    if (test_signal_object_and_wait())
    {
        fprintf(stderr, "SignalObjectAndWait failed\n");
        return 1;
    }
    if (test_abandoned_mutex())
    {
        fprintf(stderr, "abandoned mutex failed\n");
        return 1;
    }
    if (failures)
    {
        fprintf(stderr, "failures: %ld\n", failures);
        return 1;
    }

    printf("msync stress ok: %u iterations, %lu ms\n",
           ITERATIONS, (unsigned long)(GetTickCount() - start));
    return 0;
}
