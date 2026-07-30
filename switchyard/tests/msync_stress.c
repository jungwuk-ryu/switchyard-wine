/*
 * Small synchronization stress probe for Switchyard macOS MSync validation.
 *
 * This is intentionally standalone so it can be cross-compiled with MinGW and
 * run through a candidate runtime before replacing a live runtime.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define RING_SIZE 96
#define ITERATIONS 40000
#define ALERTABLE_WAIT_ITERATIONS 10000
#define ALLOCATOR_CHURN_HANDLES 12000
#define ALLOCATOR_CHURN_ROUNDS 4

static HANDLE free_slots, filled_slots;
static volatile LONG failures;
static volatile LONG apc_count;
static DWORD values[RING_SIZE];

struct alertable_wait_context
{
    HANDLE waiting_thread;
    HANDLE semaphores[2];
};

static VOID CALLBACK count_apc(ULONG_PTR arg)
{
    (void)arg;
    InterlockedIncrement(&apc_count);
}

static DWORD WINAPI alertable_signal_thread(void *arg)
{
    struct alertable_wait_context *context = arg;
    unsigned int i;

    for (i = 0; i < ALERTABLE_WAIT_ITERATIONS; ++i)
    {
        if (!ReleaseSemaphore(context->semaphores[i & 1], 1, NULL))
        {
            InterlockedIncrement(&failures);
            return 1;
        }
        if (!(i & 15) && !QueueUserAPC(count_apc, context->waiting_thread, 0))
        {
            InterlockedIncrement(&failures);
            return 1;
        }
        if (!(i & 127)) SwitchToThread();
    }

    return 0;
}

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

static int test_alertable_wait_churn(void)
{
    struct alertable_wait_context context = {0};
    HANDLE thread = NULL;
    DWORD consumed = 0, ret;
    int result = 1;

    context.semaphores[0] = CreateSemaphoreW(NULL, 0, ALERTABLE_WAIT_ITERATIONS, NULL);
    context.semaphores[1] = CreateSemaphoreW(NULL, 0, ALERTABLE_WAIT_ITERATIONS, NULL);
    if (!context.semaphores[0] || !context.semaphores[1])
    {
        fprintf(stderr, "failed to create alertable wait semaphores\n");
        goto done;
    }
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &context.waiting_thread, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        fprintf(stderr, "failed to duplicate the waiting thread handle\n");
        goto done;
    }

    apc_count = 0;
    failures = 0;
    thread = CreateThread(NULL, 0, alertable_signal_thread, &context, 0, NULL);
    if (!thread)
    {
        fprintf(stderr, "failed to create the alertable signal thread\n");
        goto done;
    }

    while (consumed < ALERTABLE_WAIT_ITERATIONS)
    {
        ret = WaitForMultipleObjectsEx(2, context.semaphores, FALSE, 10000, TRUE);
        if (ret == WAIT_OBJECT_0 || ret == WAIT_OBJECT_0 + 1)
            ++consumed;
        else if (ret != WAIT_IO_COMPLETION)
        {
            fprintf(stderr, "alertable multiple wait failed: %lu after %lu signals\n",
                    (unsigned long)ret, (unsigned long)consumed);
            InterlockedIncrement(&failures);
            break;
        }
    }

    do
        ret = WaitForSingleObjectEx(thread, 10000, TRUE);
    while (ret == WAIT_IO_COMPLETION);
    while (SleepEx(0, TRUE) == WAIT_IO_COMPLETION)
        ;

    if (ret != WAIT_OBJECT_0 || failures || !apc_count)
    {
        fprintf(stderr, "alertable churn failed: wait %lu, failures %ld, APCs %ld\n",
                (unsigned long)ret, failures, apc_count);
        goto done;
    }

    printf("msync alertable wait churn ok: %u signals, %ld APCs\n",
           ALERTABLE_WAIT_ITERATIONS, apc_count);
    result = 0;

done:
    if (thread) CloseHandle(thread);
    if (context.waiting_thread) CloseHandle(context.waiting_thread);
    if (context.semaphores[0]) CloseHandle(context.semaphores[0]);
    if (context.semaphores[1]) CloseHandle(context.semaphores[1]);
    return result;
}

static int test_allocator_churn(void)
{
    HANDLE *events;
    DWORD elapsed, round, start;
    unsigned int i;

    events = HeapAlloc(GetProcessHeap(), 0, ALLOCATOR_CHURN_HANDLES * sizeof(*events));
    if (!events)
    {
        fprintf(stderr, "failed to allocate the event handle array\n");
        return 1;
    }

    for (round = 0; round < ALLOCATOR_CHURN_ROUNDS; ++round)
    {
        start = GetTickCount();
        for (i = 0; i < ALLOCATOR_CHURN_HANDLES; ++i)
        {
            events[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
            if (!events[i])
            {
                fprintf(stderr, "round %lu failed to create event %u\n",
                        (unsigned long)round, i);
                while (i) CloseHandle(events[--i]);
                HeapFree(GetProcessHeap(), 0, events);
                return 1;
            }
        }
        elapsed = GetTickCount() - start;
        printf("msync allocator churn round %lu: %u handles, %lu ms\n",
               (unsigned long)round, ALLOCATOR_CHURN_HANDLES,
               (unsigned long)elapsed);

        /*
         * Closing in reverse order makes the lowest shared-memory index the
         * last reclaimed one. A single stale low-index hint then repeatedly
         * rescans all previously reused live entries on the following round.
         */
        for (i = ALLOCATOR_CHURN_HANDLES; i; --i)
            CloseHandle(events[i - 1]);

        /* Destroy notifications use the ordered Mach message queue. */
        Sleep(100);
    }

    HeapFree(GetProcessHeap(), 0, events);
    return 0;
}

int main(int argc, char **argv)
{
    HANDLE producer, consumer;
    DWORD start = GetTickCount();
    DWORD ret;

    if (argc == 2 && !strcmp(argv[1], "--alertable-race"))
        return test_alertable_wait_churn();
    if (argc == 2 && !strcmp(argv[1], "--allocator-churn"))
        return test_allocator_churn();
    if (argc != 1)
    {
        fprintf(stderr, "usage: %s [--allocator-churn|--alertable-race]\n", argv[0]);
        return 2;
    }

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
