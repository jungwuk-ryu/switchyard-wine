/*
 * Native xtajit64 identity, trap, hook, and concurrency tests
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep standalone
#endif

#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#undef WIN32_NO_STATUS

#define CACHE_RECORD_LIMIT 16

struct cache_remove_record
{
    uc_engine *engine;
    uint64_t start;
    uint64_t end;
};

static struct cache_remove_record cache_remove_records[CACHE_RECORD_LIMIT];
static uc_engine *cache_flush_engines[CACHE_RECORD_LIMIT];
static uint64_t cache_remove_start;
static uint64_t cache_remove_end;
static unsigned int cache_remove_calls;
static unsigned int cache_flush_calls;
static int cache_remove_fail_call = -1;

static uc_err record_cache_remove( uc_engine *uc, uint64_t start, uint64_t end )
{
    unsigned int call = cache_remove_calls++;

    cache_remove_start = start;
    cache_remove_end = end;
    if (call < CACHE_RECORD_LIMIT)
    {
        cache_remove_records[call].engine = uc;
        cache_remove_records[call].start = start;
        cache_remove_records[call].end = end;
    }
    if ((int)call == cache_remove_fail_call) return UC_ERR_RESOURCE;
    return uc_ctl_remove_cache( uc, start, end );
}

static uc_err record_cache_flush( uc_engine *uc )
{
    unsigned int call = cache_flush_calls++;

    if (call < CACHE_RECORD_LIMIT) cache_flush_engines[call] = uc;
    return uc_ctl_flush_tb( uc );
}

static void reset_cache_recorders(void)
{
    memset( cache_remove_records, 0, sizeof(cache_remove_records) );
    memset( cache_flush_engines, 0, sizeof(cache_flush_engines) );
    cache_remove_start = cache_remove_end = 0;
    cache_remove_calls = cache_flush_calls = 0;
    cache_remove_fail_call = -1;
}

#undef uc_ctl_remove_cache
#define uc_ctl_remove_cache record_cache_remove
#undef uc_ctl_flush_tb
#define uc_ctl_flush_tb record_cache_flush

static _Thread_local void *test_exception_jmp_buf;
void test_ntdll_set_exception_jmp_buf( jmp_buf jmp );
static void test_raise_mutation_exception(void);

#define ntdll_set_exception_jmp_buf test_ntdll_set_exception_jmp_buf
#define XTAJIT64_TEST_RAISE_EXCEPTION() test_raise_mutation_exception()

/* Include the implementation so the regression can inspect deterministic
 * test hooks without adding a production control opcode to the provider ABI. */
#include "../unixlib.c"

#undef XTAJIT64_TEST_RAISE_EXCEPTION
#undef ntdll_set_exception_jmp_buf
#undef uc_ctl_remove_cache
#undef uc_ctl_flush_tb

void test_ntdll_set_exception_jmp_buf( jmp_buf jmp )
{
    test_exception_jmp_buf = jmp;
}

static void test_raise_mutation_exception(void)
{
    jmp_buf *jmp = test_exception_jmp_buf;

    if (!jmp) abort();
    test_exception_jmp_buf = NULL;
    longjmp( *jmp, 1 );
}

#define TEST_PAGE             0x4000u
#define TEST_PAGE_COUNT       12u
#define TEST_PREFERRED_KUSER  (WINE_LOW_VA_SHADOW_BASE + 0x04000000ull)
#define TEST_PREFERRED_BASE   (WINE_LOW_VA_SHADOW_BASE + 0x06000000ull)
#define TEST_LOW_HOST_BASE    (WINE_LOW_VA_SHADOW_BASE + 0x08000000ull)
#define TEST_LOW_GUEST_BASE   0x08000000ull
#define TEST_FALLBACK_KUSER   0x0000001004000000ull
#define TEST_FALLBACK_BASE    0x0000001006000000ull
#define TEST_ASAN_KUSER       0x0000000404000000ull
#define TEST_ASAN_BASE        0x0000000406000000ull
#define TEST_SYSCALL_COUNT    2u
#define TEST_LOW_PAGE_COUNT   8u

static unsigned int failures;
static struct xtajit64_process_init_params process_params;
static unsigned char *test_pages;
static unsigned char *test_kuser;
static unsigned char *test_low_pages;
static uint64_t test_base;
static uint64_t test_ec_target;
static uint64_t test_syscall_dispatcher;
static uint64_t test_teb;

#define check(condition, ...) \
    do { if (!(condition)) { fprintf( stderr, "not ok: " __VA_ARGS__ ); ++failures; } } while (0)

struct code_buffer
{
    unsigned char *data;
    size_t offset;
};

struct simulation
{
    struct xtajit64_begin_params params;
    atomic_int ready;
    atomic_int done;
    NTSTATUS init_status;
    NTSTATUS status;
};

struct protect_worker
{
    struct xtajit64_memory_params params;
    atomic_int done;
    NTSTATUS status;
};

struct flush_worker
{
    struct xtajit64_memory_params params;
    atomic_int done;
    NTSTATUS status;
};

struct low_observer_worker
{
    struct wine_arm64ec_low_memory_range_v1 range;
    struct wine_arm64ec_low_memory_event_v1 event;
    atomic_int begun;
    atomic_int done;
    NTSTATUS status;
};

struct low_begin_worker
{
    struct wine_arm64ec_low_memory_event_v1 event;
    atomic_int done;
    NTSTATUS status;
    void *transaction;
};

struct engine_holder
{
    atomic_int ready;
    atomic_int release;
    atomic_int done;
    NTSTATUS status;
};

static void *alloc_pages_at( uint64_t address, size_t count )
{
    size_t size;
    void *ret;

    if (!count || count > SIZE_MAX / TEST_PAGE) return NULL;
    size = count * TEST_PAGE;
    ret = mmap( (void *)(uintptr_t)address, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0 );
    if (ret == MAP_FAILED) return NULL;
    if ((uintptr_t)ret == address) return ret;
    munmap( ret, size );
    return NULL;
}

static void *alloc_preferred_pages( uint64_t preferred, uint64_t fallback,
                                    size_t count )
{
    void *ret;

    if ((ret = alloc_pages_at( preferred, count ))) return ret;
    return alloc_pages_at( fallback, count );
}

static void emit_u8( struct code_buffer *code, unsigned int value )
{
    code->data[code->offset++] = value;
}

static void emit_u32( struct code_buffer *code, uint32_t value )
{
    memcpy( code->data + code->offset, &value, sizeof(value) );
    code->offset += sizeof(value);
}

static void emit_u64( struct code_buffer *code, uint64_t value )
{
    memcpy( code->data + code->offset, &value, sizeof(value) );
    code->offset += sizeof(value);
}

static void emit_movabs_rax( struct code_buffer *code, uint64_t value )
{
    emit_u8( code, 0x48 ); emit_u8( code, 0xb8 ); emit_u64( code, value );
}

static void emit_movabs_rcx( struct code_buffer *code, uint64_t value )
{
    emit_u8( code, 0x48 ); emit_u8( code, 0xb9 ); emit_u64( code, value );
}

static void emit_jump_rax( struct code_buffer *code )
{
    emit_u8( code, 0xff ); emit_u8( code, 0xe0 );
}

static void patch_rel8( struct code_buffer *code, size_t displacement, size_t target )
{
    intptr_t value = (intptr_t)target - (intptr_t)(displacement + 1);

    check( value >= INT8_MIN && value <= INT8_MAX,
           "relative branch is out of range\n" );
    code->data[displacement] = (unsigned char)(int8_t)value;
}

static NTSTATUS register_identity_page( void *page, unsigned int protect )
{
    struct xtajit64_memory_params params =
    {
        .guest = (uintptr_t)page,
        .host = (uintptr_t)page,
        .size = TEST_PAGE,
        .allocation_base = (uintptr_t)page,
        .protect = protect,
    };

    return memory_map( &params );
}

static NTSTATUS unregister_identity_page( void *page )
{
    struct xtajit64_memory_params params =
    {
        .guest = (uintptr_t)page,
        .size = TEST_PAGE,
    };

    return memory_unmap( &params );
}

static NTSTATUS observer_provider_status(void)
{
    NTSTATUS status;

    pthread_mutex_lock( &provider.mutex );
    status = provider.poison_status;
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static uint64_t observer_generation(void)
{
    uint64_t generation;

    pthread_mutex_lock( &provider.mutex );
    generation = provider.generation;
    pthread_mutex_unlock( &provider.mutex );
    return generation;
}

static BOOL canonical_range_matches( uint64_t guest, uint64_t host,
                                     unsigned int state, unsigned int perms,
                                     unsigned int domain, BOOL permanent )
{
    const struct mapped_range *range;
    BOOL found = FALSE;
    size_t i;

    pthread_mutex_lock( &provider.mutex );
    for (i = 0; i < provider.ranges.count; ++i)
    {
        range = &provider.ranges.data[i];
        if (guest < range->guest || guest >= range->guest + range->size) continue;
        found = range->host + guest - range->guest == host &&
                range->state == state && range->perms == perms &&
                range->domain == domain && range->permanent == permanent;
        break;
    }
    pthread_mutex_unlock( &provider.mutex );
    return found;
}

static void initialize_low_range( struct wine_arm64ec_low_memory_range_v1 *range,
                                  uint64_t guest, uint64_t size,
                                  uint64_t allocation_base, uint32_t state,
                                  uint32_t protect )
{
    memset( range, 0, sizeof(*range) );
    range->host_address = WINE_LOW_VA_SHADOW_BASE + guest;
    range->size = size;
    range->state = state;
    range->protect = protect;
    if (state != MEM_FREE)
        range->host_allocation_base = WINE_LOW_VA_SHADOW_BASE + allocation_base;
}

static void initialize_low_event( struct wine_arm64ec_low_memory_event_v1 *event,
                                  uint32_t operation, uint32_t flags,
                                  uint64_t guest, uint64_t size,
                                  uint64_t allocation_base,
                                  const struct wine_arm64ec_low_memory_range_v1 *ranges,
                                  size_t range_count, NTSTATUS mutation_status,
                                  NTSTATUS snapshot_status )
{
    memset( event, 0, sizeof(*event) );
    event->version = WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION;
    event->size = sizeof(*event);
    event->operation = operation;
    event->flags = flags;
    event->status = mutation_status;
    event->snapshot_status = snapshot_status;
    event->host_address = WINE_LOW_VA_SHADOW_BASE + guest;
    event->size_covered = size;
    if (allocation_base)
        event->host_allocation_base = WINE_LOW_VA_SHADOW_BASE + allocation_base;
    event->ranges = ranges;
    event->range_count = range_count;
}

static NTSTATUS publish_low_event(
    uint32_t operation, uint32_t flags, uint64_t guest, uint64_t size,
    uint64_t allocation_base,
    const struct wine_arm64ec_low_memory_range_v1 *ranges, size_t range_count,
    NTSTATUS mutation_status, NTSTATUS snapshot_status )
{
    struct wine_arm64ec_low_memory_event_v1 event;
    void *transaction = NULL;
    NTSTATUS status;

    initialize_low_event( &event, operation, flags, guest, size, allocation_base,
                          ranges, range_count, mutation_status, snapshot_status );
    status = arm64ec_low_memory_observer.begin( arm64ec_low_memory_observer.context,
                                                operation, event.host_address,
                                                event.size_covered,
                                                event.host_allocation_base,
                                                &transaction );
    if (!status)
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &event );
    if (!status) status = observer_provider_status();
    return status;
}

static NTSTATUS reset_test_provider(void)
{
    NTSTATUS status;

    if ((status = process_term( NULL ))) return status;
    process_params.enabled_capabilities = 0;
    return process_init( &process_params );
}

static uint64_t elapsed_milliseconds( const struct timespec *start,
                                      const struct timespec *now )
{
    time_t seconds = now->tv_sec - start->tv_sec;
    long nanoseconds = now->tv_nsec - start->tv_nsec;

    if (nanoseconds < 0)
    {
        --seconds;
        nanoseconds += 1000000000l;
    }
    return (uint64_t)seconds * 1000 + nanoseconds / 1000000;
}

static BOOL wait_atomic_int_at_least( atomic_int *value, int expected,
                                      unsigned int timeout_ms )
{
    struct timespec start, now;

    clock_gettime( CLOCK_MONOTONIC, &start );
    do
    {
        if (atomic_load_explicit( value, memory_order_acquire ) >= expected) return TRUE;
        sched_yield();
        clock_gettime( CLOCK_MONOTONIC, &now );
    } while (elapsed_milliseconds( &start, &now ) < timeout_ms);
    return FALSE;
}

static BOOL wait_mutation_stage( enum mutation_stage expected,
                                 unsigned int timeout_ms )
{
    struct timespec start, now;
    enum mutation_stage stage;

    clock_gettime( CLOCK_MONOTONIC, &start );
    do
    {
        pthread_mutex_lock( &provider.mutex );
        stage = provider.mutation_stage;
        pthread_mutex_unlock( &provider.mutex );
        if (stage == expected) return TRUE;
        sched_yield();
        clock_gettime( CLOCK_MONOTONIC, &now );
    } while (elapsed_milliseconds( &start, &now ) < timeout_ms);
    return FALSE;
}

static void initialize_begin_params( struct simulation *simulation,
                                     uint64_t code, uint64_t stack )
{
    memset( &simulation->params, 0, sizeof(simulation->params) );
    simulation->params.context.rip = code;
    simulation->params.context.rsp = stack + TEST_PAGE - 16;
    simulation->params.context.eflags = 0x202;
    simulation->params.context.mxcsr = 0x1f80;
    simulation->params.gs_base = test_teb;
    simulation->params.stack_limit = stack;
    simulation->params.stack_base = stack + TEST_PAGE;
}

static void *run_simulation( void *arg )
{
    struct simulation *simulation = arg;

    simulation->init_status = thread_init( NULL );
    atomic_store_explicit( &simulation->ready, 1, memory_order_release );
    if (!simulation->init_status)
        simulation->status = begin_simulation( &simulation->params );
    thread_term( NULL );
    atomic_store_explicit( &simulation->done, 1, memory_order_release );
    return NULL;
}

static BOOL join_simulation( pthread_t thread, struct simulation *simulation )
{
    BOOL done = wait_atomic_int_at_least( &simulation->done, 1, 5000 );

    if (!done)
    {
        struct xtajit64_poison_params params = { .status = STATUS_TIMEOUT };

        poison( &params );
    }
    pthread_join( thread, NULL );
    return done;
}

static void test_process_init_abi(void)
{
    struct xtajit64_process_init_params invalid = process_params;
    NTSTATUS status;

    check( process_init( NULL ) == STATUS_INVALID_PARAMETER,
           "NULL process init was accepted\n" );
    invalid.abi_version++;
    status = process_init( &invalid );
    check( status == STATUS_REVISION_MISMATCH,
           "wrong ABI version returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.abi_size--;
    status = process_init( &invalid );
    check( status == STATUS_REVISION_MISMATCH,
           "wrong ABI size returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.enabled_capabilities = 1;
    status = process_init( &invalid );
    check( status == STATUS_REVISION_MISMATCH,
           "pre-enabled capabilities returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.x64_syscall_count = (1u << 16) + 1;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER,
           "oversized syscall table returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.ec_bitmap++;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER,
           "misaligned EC bitmap returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.highest_user_address = XTAJIT64_X64_USER_ADDRESS_MAX + 1;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER,
           "oversized x64 address space returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.rtl_exit_user_thread = invalid.highest_user_address + 1;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER,
           "out-of-range exit thunk returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.x64_syscall_dispatcher = invalid.highest_user_address + 1;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER,
           "out-of-range syscall dispatcher returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.highest_user_address = XTAJIT64_GUEST_KUSER - 1;
    invalid.rtl_exit_user_thread = 1;
    invalid.x64_syscall_dispatcher = 1;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER,
           "out-of-range guest KUSER mapping returned %#x\n", (unsigned int)status );
    invalid = process_params;
    invalid.host_kuser = UINT64_MAX - TEST_PAGE + 1;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER,
           "overflowing host KUSER mapping returned %#x\n", (unsigned int)status );
}

static void *run_low_begin_only( void *arg )
{
    struct low_begin_worker *worker = arg;

    worker->status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, worker->event.operation,
        worker->event.host_address, worker->event.size_covered,
        worker->event.host_allocation_base, &worker->transaction );
    atomic_store_explicit( &worker->done, 1, memory_order_release );
    return NULL;
}

static void *run_engine_holder( void *arg )
{
    struct engine_holder *holder = arg;

    holder->status = thread_init( NULL );
    atomic_store_explicit( &holder->ready, 1, memory_order_release );
    while (!atomic_load_explicit( &holder->release, memory_order_acquire ))
        sched_yield();
    if (!holder->status) thread_term( NULL );
    atomic_store_explicit( &holder->done, 1, memory_order_release );
    return NULL;
}

static void test_low_observer_validation(void)
{
    struct wine_arm64ec_low_memory_range_v1 range;
    struct wine_arm64ec_low_memory_event_v1 event;
    struct low_begin_worker worker = {0};
    void *transaction, *duplicate, *stale;
    uint64_t generation;
    unsigned int starting_failures = failures;
    pthread_t non_owner;
    BOOL non_owner_created = FALSE, non_owner_done = FALSE;
    int ret;
    NTSTATUS status;

    check( arm64ec_low_memory_observer.version ==
               WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION &&
           arm64ec_low_memory_observer.size == sizeof(arm64ec_low_memory_observer) &&
           arm64ec_low_memory_observer.capabilities ==
               WINE_ARM64EC_LOW_MEMORY_OBSERVER_CAP_EXACT_POST_SNAPSHOT,
           "LOW observer descriptor does not advertise the exact v1 ABI\n" );

    transaction = (void *)(uintptr_t)1;
    status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, WINE_WOW64_MEMORY_UNMAP,
        TEST_LOW_HOST_BASE, 0, 0, &transaction );
    check( status == STATUS_INVALID_PARAMETER && !transaction,
           "zero-length LOW begin returned %#x/%p\n",
           (unsigned int)status, transaction );
    transaction = (void *)(uintptr_t)1;
    status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, WINE_WOW64_MEMORY_UNMAP,
        TEST_LOW_HOST_BASE + 1, TEST_PAGE, 0, &transaction );
    check( status == STATUS_INVALID_PARAMETER && !transaction,
           "unaligned LOW begin returned %#x/%p\n",
           (unsigned int)status, transaction );
    transaction = (void *)(uintptr_t)1;
    status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, 0, TEST_LOW_HOST_BASE,
        TEST_PAGE, 0, &transaction );
    check( status == STATUS_INVALID_PARAMETER && !transaction,
           "unknown LOW operation returned %#x/%p\n",
           (unsigned int)status, transaction );

    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE, 0,
                          MEM_FREE, PAGE_NOACCESS );
    initialize_low_event( &event, WINE_WOW64_MEMORY_UNMAP, 0,
                          TEST_LOW_GUEST_BASE, TEST_PAGE, 0, &range, 1,
                          STATUS_SUCCESS, STATUS_SUCCESS );
    transaction = NULL;
    status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, event.operation, event.host_address,
        event.size_covered, event.host_allocation_base, &transaction );
    check( !status && transaction, "LOW bad-size begin failed %#x\n",
           (unsigned int)status );
    event.size--;
    if (!status)
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &event );
    check( observer_provider_status() == STATUS_INVALID_PARAMETER,
           "short LOW event did not poison the provider %#x\n",
           (unsigned int)observer_provider_status() );
    check( !reset_test_provider(), "LOW reset after bad size failed\n" );

    initialize_low_event( &event, WINE_WOW64_MEMORY_UNMAP, 0x80000000u,
                          TEST_LOW_GUEST_BASE, TEST_PAGE, 0, &range, 1,
                          STATUS_SUCCESS, STATUS_SUCCESS );
    status = publish_low_event( event.operation, event.flags,
                                TEST_LOW_GUEST_BASE, TEST_PAGE, 0,
                                &range, 1, STATUS_SUCCESS, STATUS_SUCCESS );
    check( status == STATUS_INVALID_PARAMETER,
           "unknown LOW event flags returned %#x\n", (unsigned int)status );
    check( !reset_test_provider(), "LOW reset after bad event flags failed\n" );

    range.flags = 1;
    status = publish_low_event( WINE_WOW64_MEMORY_UNMAP, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE, 0,
                                &range, 1, STATUS_SUCCESS, STATUS_SUCCESS );
    check( status == STATUS_INVALID_PARAMETER,
           "unknown LOW range flags returned %#x\n", (unsigned int)status );
    check( !reset_test_provider(), "LOW reset after bad range flags failed\n" );
    range.flags = 0;

    initialize_low_range( &range, 0, WINE_LOW_VA_SHADOW_SIZE - TEST_PAGE,
                          0, MEM_FREE, PAGE_NOACCESS );
    status = publish_low_event( WINE_WOW64_MEMORY_RESYNC,
                                WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT,
                                0, WINE_LOW_VA_SHADOW_SIZE, 0, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( status == STATUS_INVALID_PARAMETER,
           "gapped LOW full snapshot returned %#x\n", (unsigned int)status );
    check( !reset_test_provider(), "LOW reset after gapped snapshot failed\n" );

    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE, 0,
                          MEM_FREE, PAGE_NOACCESS );
    initialize_low_event( &event, WINE_WOW64_MEMORY_UNMAP, 0,
                          TEST_LOW_GUEST_BASE, TEST_PAGE, 0, NULL, 0,
                          STATUS_SUCCESS, STATUS_NO_MEMORY );
    transaction = NULL;
    status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, event.operation, event.host_address,
        event.size_covered, event.host_allocation_base, &transaction );
    if (!status)
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &event );
    check( observer_provider_status() == STATUS_NO_MEMORY,
           "failed LOW snapshot did not poison once %#x\n",
           (unsigned int)observer_provider_status() );
    check( !reset_test_provider(), "LOW reset after snapshot failure failed\n" );

    initialize_low_event( &event, WINE_WOW64_MEMORY_UNMAP, 0,
                          TEST_LOW_GUEST_BASE, TEST_PAGE, 0, &range, 1,
                          STATUS_UNSUCCESSFUL, STATUS_SUCCESS );
    transaction = NULL;
    status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, event.operation, event.host_address,
        event.size_covered, event.host_allocation_base, &transaction );
    duplicate = (void *)(uintptr_t)1;
    if (!status)
        status = arm64ec_low_memory_observer.begin(
            arm64ec_low_memory_observer.context, event.operation,
            event.host_address, event.size_covered, event.host_allocation_base,
            &duplicate );
    check( status == STATUS_INVALID_DEVICE_STATE && !duplicate,
           "duplicate LOW mutation owner returned %#x/%p\n",
           (unsigned int)status, duplicate );
    worker.event = event;
    ret = pthread_create( &non_owner, NULL, run_low_begin_only, &worker );
    check( !ret, "non-owner duplicate LOW begin thread creation failed %d\n", ret );
    if (!ret)
    {
        non_owner_created = TRUE;
        non_owner_done = wait_atomic_int_at_least( &worker.done, 1, 2000 );
        check( non_owner_done,
               "non-owner duplicate LOW begin blocked behind the live transaction\n" );
    }
    if (non_owner_created && !non_owner_done)
    {
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &event );
        transaction = NULL;
    }
    if (non_owner_created) pthread_join( non_owner, NULL );
    check( !non_owner_created ||
           (worker.status == STATUS_INVALID_DEVICE_STATE && !worker.transaction),
           "non-owner duplicate LOW begin returned %#x/%p\n",
           (unsigned int)worker.status, worker.transaction );
    pthread_mutex_lock( &provider.mutex );
    check( !non_owner_done ||
           (provider.observer_transaction == transaction && provider.mutating),
           "non-owner duplicate LOW begin consumed the live transaction\n" );
    pthread_mutex_unlock( &provider.mutex );
    if (worker.transaction)
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              worker.transaction, &event );
    generation = observer_generation();
    if (transaction)
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &event );
    check( !observer_provider_status() && observer_generation() == generation + 1,
           "failed mutation post-snapshot was not completed once %#x\n",
           (unsigned int)observer_provider_status() );

    transaction = NULL;
    status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, event.operation, event.host_address,
        event.size_covered, event.host_allocation_base, &transaction );
    if (!status)
        arm64ec_low_memory_observer.complete(
            arm64ec_low_memory_observer.context, (void *)(uintptr_t)1, &event );
    pthread_mutex_lock( &provider.mutex );
    check( !status && provider.observer_transaction == transaction &&
           provider.mutating,
           "forged LOW completion consumed the active transaction\n" );
    pthread_mutex_unlock( &provider.mutex );
    if (!status)
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &event );
    pthread_mutex_lock( &provider.mutex );
    check( provider.poison_status == STATUS_INVALID_DEVICE_STATE &&
           !provider.observer_transaction && !provider.mutating,
           "valid LOW completion did not clean up after forged rejection\n" );
    pthread_mutex_unlock( &provider.mutex );
    check( !reset_test_provider(), "LOW reset after forged completion failed\n" );

    transaction = NULL;
    status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, event.operation, event.host_address,
        event.size_covered, event.host_allocation_base, &transaction );
    stale = transaction;
    if (!status)
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &event );
    check( !observer_provider_status(), "valid LOW completion poisoned provider\n" );
    arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                          stale, &event );
    check( observer_provider_status() == STATUS_INVALID_DEVICE_STATE,
           "stale LOW token was not rejected without dereference %#x\n",
           (unsigned int)observer_provider_status() );
    check( !reset_test_provider(), "LOW reset after stale token failed\n" );
    if (failures == starting_failures)
        printf( "XTAJIT64_LOW_OBSERVER_VALIDATION_PASS\n" );
}

static void test_low_observer_interval_replacement(void)
{
    struct wine_arm64ec_low_memory_range_v1 full[3], range;
    struct xtajit64_memory_translate_params translate;
    struct xtajit64_memory_resync_begin_params begin;
    struct xtajit64_memory_resync_params resync;
    struct xtajit64_memory_params legacy;
    uint64_t generation;
    unsigned int starting_failures = failures;
    NTSTATUS status;

    check( (uintptr_t)test_low_pages == TEST_LOW_HOST_BASE,
           "LOW observer host pages are unavailable at the authoritative shadow\n" );
    if ((uintptr_t)test_low_pages != TEST_LOW_HOST_BASE) return;

    initialize_low_range( &full[0], 0, TEST_LOW_GUEST_BASE, 0,
                          MEM_FREE, PAGE_NOACCESS );
    initialize_low_range( &full[1], TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_READWRITE );
    initialize_low_range( &full[2], TEST_LOW_GUEST_BASE + TEST_PAGE,
                          WINE_LOW_VA_SHADOW_SIZE - TEST_LOW_GUEST_BASE - TEST_PAGE,
                          0, MEM_FREE, PAGE_NOACCESS );
    status = publish_low_event( WINE_WOW64_MEMORY_RESYNC,
                                WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT,
                                0, WINE_LOW_VA_SHADOW_SIZE, 0, full, 3,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ | UC_PROT_WRITE,
               XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ) &&
           canonical_range_matches( XTAJIT64_GUEST_KUSER,
                                    (uintptr_t)test_kuser, MEM_COMMIT,
                                    UC_PROT_READ,
                                    XTAJIT64_MEMORY_ADDRESS_INVALID, TRUE ),
           "initial LOW full snapshot lost mapping or non-LOW KUSER %#x\n",
           (unsigned int)status );

    memset( &translate, 0, sizeof(translate) );
    translate.address = TEST_LOW_GUEST_BASE + 37;
    translate.size = 32;
    translate.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                      XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE;
    status = memory_translate( &translate );
    check( !status && translate.host == TEST_LOW_HOST_BASE + 37 &&
           translate.allocation_base == TEST_LOW_GUEST_BASE &&
           translate.domain == XTAJIT64_MEMORY_ADDRESS_AMD64_LOW,
           "LOW address codec returned %#x %#llx/%#llx/%u\n",
           (unsigned int)status, (unsigned long long)translate.host,
           (unsigned long long)translate.allocation_base, translate.domain );

    memset( &legacy, 0, sizeof(legacy) );
    legacy.guest = TEST_LOW_GUEST_BASE;
    legacy.size = TEST_PAGE;
    legacy.protect = PAGE_READONLY;
    check( memory_protect( &legacy ) == STATUS_ACCESS_DENIED &&
           memory_unmap( &legacy ) == STATUS_ACCESS_DENIED,
           "legacy mutation changed LOW-owned state\n" );

    memset( &legacy, 0, sizeof(legacy) );
    legacy.guest = TEST_LOW_HOST_BASE;
    legacy.host = TEST_LOW_HOST_BASE;
    legacy.size = TEST_PAGE;
    legacy.allocation_base = TEST_LOW_HOST_BASE;
    legacy.protect = PAGE_READONLY;
    check( memory_map( &legacy ) == STATUS_ACCESS_DENIED &&
           memory_protect( &legacy ) == STATUS_ACCESS_DENIED &&
           memory_unmap( &legacy ) == STATUS_ACCESS_DENIED,
           "host-address legacy mutation changed LOW-owned state\n" );
    legacy.size = 0;
    check( memory_unmap( &legacy ) == STATUS_ACCESS_DENIED,
           "host-allocation legacy unmap changed LOW-owned state\n" );
    puts( "XTAJIT64_LOW_LEGACY_HOST_SKIP_PASS" );

    status = memory_resync_begin( &begin );
    memset( &resync, 0, sizeof(resync) );
    resync.generation = begin.generation;
    if (!status) status = memory_resync( &resync );
    check( !status && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ | UC_PROT_WRITE,
               XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "identity resync replaced LOW ownership %#x\n", (unsigned int)status );

    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_READONLY );
    status = publish_low_event( WINE_WOW64_MEMORY_PROTECT, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ, XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "LOW protect interval replacement failed %#x\n", (unsigned int)status );

    generation = observer_generation();
    range.protect = PAGE_READWRITE;
    status = publish_low_event( WINE_WOW64_MEMORY_PROTECT, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &range, 1,
                                STATUS_ACCESS_DENIED, STATUS_SUCCESS );
    check( !status && observer_generation() == generation + 1 &&
           canonical_range_matches( TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE,
                                    MEM_COMMIT, UC_PROT_READ | UC_PROT_WRITE,
                                    XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "failed LOW mutation did not publish exact post-state %#x\n",
           (unsigned int)status );

    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_RESERVE, 0 );
    status = publish_low_event( WINE_WOW64_MEMORY_DECOMMIT, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_RESERVE,
               UC_PROT_NONE, XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "LOW decommit interval replacement failed %#x\n", (unsigned int)status );

    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_EXECUTE_READ );
    status = publish_low_event( WINE_WOW64_MEMORY_COMMIT, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ | UC_PROT_EXEC,
               XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "LOW commit interval replacement failed %#x\n", (unsigned int)status );

    initialize_low_range( &full[0], 0, TEST_LOW_GUEST_BASE + TEST_PAGE, 0,
                          MEM_FREE, PAGE_NOACCESS );
    initialize_low_range( &full[1], TEST_LOW_GUEST_BASE + TEST_PAGE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE + TEST_PAGE, MEM_RESERVE, 0 );
    initialize_low_range( &full[2], TEST_LOW_GUEST_BASE + 2 * TEST_PAGE,
                          WINE_LOW_VA_SHADOW_SIZE - TEST_LOW_GUEST_BASE - 2 * TEST_PAGE,
                          0, MEM_FREE, PAGE_NOACCESS );
    status = publish_low_event( WINE_WOW64_MEMORY_PROTECT,
                                WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT,
                                0, WINE_LOW_VA_SHADOW_SIZE, 0, full, 3,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && !canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ | UC_PROT_EXEC,
               XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ) &&
           canonical_range_matches( TEST_LOW_GUEST_BASE + TEST_PAGE,
                                    TEST_LOW_HOST_BASE + TEST_PAGE, MEM_RESERVE,
                                    UC_PROT_NONE,
                                    XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ) &&
           canonical_range_matches( XTAJIT64_GUEST_KUSER,
                                    (uintptr_t)test_kuser, MEM_COMMIT,
                                    UC_PROT_READ,
                                    XTAJIT64_MEMORY_ADDRESS_INVALID, TRUE ),
           "nested LOW full snapshot was not authoritative %#x\n",
           (unsigned int)status );

    initialize_low_range( &range, 0, WINE_LOW_VA_SHADOW_SIZE, 0,
                          MEM_FREE, PAGE_NOACCESS );
    status = publish_low_event( WINE_WOW64_MEMORY_RESYNC,
                                WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT,
                                0, WINE_LOW_VA_SHADOW_SIZE, 0, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && !canonical_range_matches(
               TEST_LOW_GUEST_BASE + TEST_PAGE, TEST_LOW_HOST_BASE + TEST_PAGE,
               MEM_RESERVE, UC_PROT_NONE,
               XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "LOW full free snapshot retained stale ownership %#x\n",
           (unsigned int)status );
    if (failures == starting_failures)
        printf( "XTAJIT64_LOW_OBSERVER_INTERVAL_PASS\n" );
}

static void test_low_observer_partial_engine_poison(void)
{
    struct wine_arm64ec_low_memory_range_v1 range;
    struct engine_holder holders[2] = {0};
    pthread_t threads[2];
    BOOL created[2] = {FALSE, FALSE};
    uint64_t generation;
    unsigned int starting_failures = failures;
    NTSTATUS status;
    int i, ret;

    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_READWRITE );
    status = publish_low_event( WINE_WOW64_MEMORY_ALLOCATE, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ | UC_PROT_WRITE,
               XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "LOW partial-engine setup failed %#x\n", (unsigned int)status );
    if (status) goto done;

    for (i = 0; i < 2; ++i)
    {
        ret = pthread_create( &threads[i], NULL, run_engine_holder, &holders[i] );
        check( !ret, "LOW partial-engine holder %d creation failed %d\n", i, ret );
        if (!ret) created[i] = TRUE;
    }
    for (i = 0; i < 2; ++i)
    {
        if (!created[i]) continue;
        check( wait_atomic_int_at_least( &holders[i].ready, 1, 5000 ),
               "LOW partial-engine holder %d did not initialize\n", i );
        check( !holders[i].status,
               "LOW partial-engine holder %d init failed %#x\n", i,
               (unsigned int)holders[i].status );
    }
    if (!created[0] || !created[1] || holders[0].status || holders[1].status)
        goto release;

    generation = observer_generation();
    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_READONLY );
    test_fail_engine_mutation = 1;
    status = publish_low_event( WINE_WOW64_MEMORY_PROTECT, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    test_fail_engine_mutation = -1;
    check( status == STATUS_UNSUCCESSFUL &&
           observer_provider_status() == STATUS_UNSUCCESSFUL,
           "partial LOW engine mutation did not poison provider %#x/%#x\n",
           (unsigned int)status, (unsigned int)observer_provider_status() );
    check( test_engine_mutation_count == 2,
           "partial LOW engine mutation touched %d engines\n",
           test_engine_mutation_count );
    check( observer_generation() == generation && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ | UC_PROT_WRITE,
               XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "partial LOW engine failure published a mixed registry\n" );
    pthread_mutex_lock( &provider.mutex );
    check( !provider.mutating && !provider.mutation_owner_valid &&
           provider.mutation_stage == MUTATION_STAGE_IDLE &&
           !provider.observer_transaction,
           "partial LOW engine failure retained mutation ownership\n" );
    pthread_mutex_unlock( &provider.mutex );
    check( thread_init( NULL ) == STATUS_UNSUCCESSFUL,
           "poisoned provider accepted a future engine\n" );
    thread_term( NULL );

release:
    test_fail_engine_mutation = -1;
    for (i = 0; i < 2; ++i)
        atomic_store_explicit( &holders[i].release, 1, memory_order_release );
    for (i = 0; i < 2; ++i)
    {
        if (!created[i]) continue;
        check( wait_atomic_int_at_least( &holders[i].done, 1, 5000 ),
               "LOW partial-engine holder %d did not terminate\n", i );
        pthread_join( threads[i], NULL );
    }
done:
    check( !reset_test_provider(), "LOW reset after partial-engine poison failed\n" );
    if (failures == starting_failures)
        printf( "XTAJIT64_LOW_OBSERVER_PARTIAL_POISON_PASS\n" );
}

static void test_low_flush_multi_engine_poison(void)
{
    struct wine_arm64ec_low_memory_range_v1 range;
    struct xtajit64_memory_params targeted =
    {
        .guest = TEST_LOW_GUEST_BASE + 0x40,
        .size = 0x80,
    };
    struct xtajit64_memory_params full = {0};
    struct xtajit64_memory_params empty =
    {
        .guest = TEST_LOW_GUEST_BASE,
    };
    struct xtajit64_memory_params unmapped =
    {
        .guest = TEST_LOW_GUEST_BASE + 2 * TEST_PAGE,
        .size = 0x80,
    };
    struct engine_holder holders[2] = {0};
    pthread_t threads[2];
    BOOL created[2] = {FALSE, FALSE};
    uint64_t generation;
    unsigned int starting_failures = failures;
    NTSTATUS status;
    int i, ret;

    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_READONLY );
    status = publish_low_event( WINE_WOW64_MEMORY_ALLOCATE, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ, XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "LOW cache-flush setup failed %#x\n", (unsigned int)status );
    if (status) goto done;

    for (i = 0; i < 2; ++i)
    {
        ret = pthread_create( &threads[i], NULL, run_engine_holder, &holders[i] );
        check( !ret, "LOW cache-flush holder %d creation failed %d\n", i, ret );
        if (!ret) created[i] = TRUE;
    }
    for (i = 0; i < 2; ++i)
    {
        if (!created[i]) continue;
        check( wait_atomic_int_at_least( &holders[i].ready, 1, 5000 ),
               "LOW cache-flush holder %d did not initialize\n", i );
        check( !holders[i].status,
               "LOW cache-flush holder %d init failed %#x\n", i,
               (unsigned int)holders[i].status );
    }
    if (!created[0] || !created[1] || holders[0].status || holders[1].status)
        goto release;

    reset_cache_recorders();
    status = flush_instruction_cache( &targeted );
    check( !status && cache_remove_calls == 2 && !cache_flush_calls,
           "LOW guest cache flush returned %#x remove/full %u/%u\n",
           (unsigned int)status, cache_remove_calls, cache_flush_calls );
    for (i = 0; i < 2 && i < cache_remove_calls; ++i)
        check( cache_remove_records[i].start == targeted.guest &&
               cache_remove_records[i].end == targeted.guest + targeted.size,
               "LOW guest cache interval %d was %#llx-%#llx\n", i,
               (unsigned long long)cache_remove_records[i].start,
               (unsigned long long)cache_remove_records[i].end );
    check( cache_remove_calls < 2 ||
           cache_remove_records[0].engine != cache_remove_records[1].engine,
           "LOW guest cache flush reused one engine\n" );

    reset_cache_recorders();
    targeted.guest = TEST_LOW_HOST_BASE + 0x40;
    status = flush_instruction_cache( &targeted );
    check( !status && cache_remove_calls == 2 && !cache_flush_calls,
           "LOW host cache flush returned %#x remove/full %u/%u\n",
           (unsigned int)status, cache_remove_calls, cache_flush_calls );
    for (i = 0; i < 2 && i < cache_remove_calls; ++i)
        check( cache_remove_records[i].start == TEST_LOW_GUEST_BASE + 0x40 &&
               cache_remove_records[i].end == TEST_LOW_GUEST_BASE + 0xc0,
               "LOW host cache interval %d was %#llx-%#llx\n", i,
               (unsigned long long)cache_remove_records[i].start,
               (unsigned long long)cache_remove_records[i].end );

    reset_cache_recorders();
    status = flush_instruction_cache( &full );
    check( !status && !cache_remove_calls && cache_flush_calls == 2 &&
           cache_flush_engines[0] != cache_flush_engines[1],
           "NULL/zero full flush returned %#x remove/full %u/%u\n",
           (unsigned int)status, cache_remove_calls, cache_flush_calls );

    reset_cache_recorders();
    full.size = 0x1234;
    status = flush_instruction_cache( &full );
    check( !status && !cache_remove_calls && cache_flush_calls == 2 &&
           cache_flush_engines[0] != cache_flush_engines[1],
           "NULL/nonzero full flush returned %#x remove/full %u/%u\n",
           (unsigned int)status, cache_remove_calls, cache_flush_calls );

    reset_cache_recorders();
    status = flush_instruction_cache( &empty );
    check( !status && !cache_remove_calls && !cache_flush_calls,
           "non-NULL/zero empty flush returned %#x remove/full %u/%u\n",
           (unsigned int)status, cache_remove_calls, cache_flush_calls );

    reset_cache_recorders();
    status = flush_instruction_cache( &unmapped );
    check( !status && !cache_remove_calls && !cache_flush_calls,
           "unmapped targeted flush returned %#x remove/full %u/%u\n",
           (unsigned int)status, cache_remove_calls, cache_flush_calls );

    generation = observer_generation();
    reset_cache_recorders();
    targeted.guest = TEST_LOW_GUEST_BASE + 0x40;
    cache_remove_fail_call = 1;
    status = flush_instruction_cache( &targeted );
    cache_remove_fail_call = -1;
    check( status == STATUS_UNSUCCESSFUL && cache_remove_calls == 2 &&
           observer_provider_status() == STATUS_UNSUCCESSFUL,
           "partial LOW cache flush did not poison provider %#x/%#x calls %u\n",
           (unsigned int)status, (unsigned int)observer_provider_status(),
           cache_remove_calls );
    check( observer_generation() == generation && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ, XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "partial LOW cache flush changed the canonical registry\n" );
    pthread_mutex_lock( &provider.mutex );
    check( !provider.mutating && !provider.mutation_owner_valid &&
           provider.mutation_stage == MUTATION_STAGE_IDLE &&
           !provider.observer_transaction,
           "partial LOW cache flush retained mutation ownership\n" );
    pthread_mutex_unlock( &provider.mutex );
    check( thread_init( NULL ) == STATUS_UNSUCCESSFUL,
           "cache-flush-poisoned provider accepted a future engine\n" );
    thread_term( NULL );

release:
    cache_remove_fail_call = -1;
    for (i = 0; i < 2; ++i)
        atomic_store_explicit( &holders[i].release, 1, memory_order_release );
    for (i = 0; i < 2; ++i)
    {
        if (!created[i]) continue;
        check( wait_atomic_int_at_least( &holders[i].done, 1, 5000 ),
               "LOW cache-flush holder %d did not terminate\n", i );
        pthread_join( threads[i], NULL );
    }
done:
    reset_cache_recorders();
    check( !reset_test_provider(), "LOW reset after cache-flush poison failed\n" );
    if (failures == starting_failures)
        printf( "XTAJIT64_LOW_FLUSH_MULTI_ENGINE_PASS\n" );
}

static void test_identity_codec(void)
{
    unsigned char *page = test_pages + 3 * TEST_PAGE;
    struct xtajit64_memory_translate_params translate;
    struct xtajit64_memory_params protect =
    {
        .guest = (uintptr_t)page,
        .size = TEST_PAGE,
        .protect = PAGE_READONLY,
    };
    NTSTATUS status;

    check( !register_identity_page( page, PAGE_READWRITE ),
           "identity codec map failed\n" );
    memset( &translate, 0, sizeof(translate) );
    translate.address = (uintptr_t)page + 37;
    translate.size = 64;
    translate.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                      XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ |
                      XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE;
    status = memory_translate( &translate );
    check( !status && translate.guest == (uintptr_t)page + 37 &&
           translate.host == translate.guest &&
           translate.allocation_base == (uintptr_t)page &&
           translate.domain == XTAJIT64_MEMORY_ADDRESS_IDENTITY,
           "identity guest codec returned %#x %#llx/%#llx/%u\n",
           (unsigned int)status, (unsigned long long)translate.guest,
           (unsigned long long)translate.host, translate.domain );

    memset( &translate, 0, sizeof(translate) );
    translate.address = (uintptr_t)page + 91;
    translate.size = 8;
    translate.flags = XTAJIT64_MEMORY_TRANSLATE_HOST_TO_GUEST |
                      XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE;
    status = memory_translate( &translate );
    check( !status && translate.guest == translate.host &&
           translate.host == (uintptr_t)page + 91,
           "identity host codec returned %#x %#llx/%#llx\n",
           (unsigned int)status, (unsigned long long)translate.guest,
           (unsigned long long)translate.host );

    check( !memory_protect( &protect ), "identity protect failed\n" );
    memset( &translate, 0, sizeof(translate) );
    translate.address = (uintptr_t)page;
    translate.size = 1;
    translate.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST |
                      XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE;
    status = memory_translate( &translate );
    check( status == STATUS_INVALID_ADDRESS,
           "read-only identity mapping remained writable %#x\n",
           (unsigned int)status );

    memset( &translate, 0, sizeof(translate) );
    translate.address = UINT64_MAX - 3;
    translate.size = 8;
    translate.flags = XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST;
    status = memory_translate( &translate );
    check( status == STATUS_INVALID_PARAMETER,
           "overflowing codec request returned %#x\n", (unsigned int)status );
    check( !unregister_identity_page( page ), "identity codec unmap failed\n" );
}

static void build_execute_only_code( unsigned char *host_page,
                                     uint64_t guest_page, uint64_t marker )
{
    struct code_buffer code = { host_page, 0 };

    memset( host_page, 0, TEST_PAGE );
    emit_movabs_rax( &code, guest_page + 0x100 );
    emit_u8( &code, 0x48 ); emit_u8( &code, 0x8b ); emit_u8( &code, 0x18 );
    emit_movabs_rax( &code, test_ec_target );
    emit_jump_rax( &code );
    memcpy( host_page + 0x100, &marker, sizeof(marker) );
}

static void test_execute_only_read_and_execute(void)
{
    const uint64_t identity_marker = 0x123456789abcdef0ull;
    const uint64_t low_first = 0xfedcba9876543210ull;
    const uint64_t low_second = 0x0f1e2d3c4b5a6978ull;
    const uint64_t low_third = 0x89abcdef01234567ull;
    unsigned char *identity = test_pages + 3 * TEST_PAGE;
    unsigned char *stack = test_pages + 4 * TEST_PAGE;
    struct xtajit64_memory_params flush = { .size = TEST_PAGE };
    struct wine_arm64ec_low_memory_range_v1 low_range;
    struct simulation simulation = {0};
    unsigned int starting_failures = failures;
    BOOL thread_initialized = FALSE;
    NTSTATUS status;

    build_execute_only_code( identity, (uintptr_t)identity, identity_marker );
    build_execute_only_code( test_low_pages, TEST_LOW_GUEST_BASE, low_first );
    check( !register_identity_page( identity, PAGE_EXECUTE ),
           "execute-only identity map failed\n" );
    check( !register_identity_page( stack, PAGE_READWRITE ),
           "execute-only stack map failed\n" );
    initialize_low_range( &low_range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_EXECUTE );
    status = publish_low_event( WINE_WOW64_MEMORY_ALLOCATE, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &low_range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status && canonical_range_matches(
               TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
               UC_PROT_READ | UC_PROT_EXEC,
               XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
           "execute-only LOW map returned %#x\n", (unsigned int)status );
    if (failures != starting_failures) goto done;
    status = thread_init( NULL );
    check( !status, "execute-only engine init failed %#x\n", (unsigned int)status );
    if (status) goto done;
    thread_initialized = TRUE;

    flush.guest = (uintptr_t)identity;
    check( !flush_instruction_cache( &flush ),
           "execute-only identity flush failed\n" );
    initialize_begin_params( &simulation, (uintptr_t)identity, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           simulation.params.context.rbx == identity_marker,
           "execute-only identity simulation returned %#x reason %u data %#llx\n",
           (unsigned int)status, simulation.params.stop_reason,
           (unsigned long long)simulation.params.context.rbx );

    initialize_begin_params( &simulation, TEST_LOW_GUEST_BASE, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           simulation.params.context.rbx == low_first,
           "execute-only LOW first simulation returned %#x reason %u data %#llx\n",
           (unsigned int)status, simulation.params.stop_reason,
           (unsigned long long)simulation.params.context.rbx );

    build_execute_only_code( test_low_pages, TEST_LOW_GUEST_BASE, low_second );
    reset_cache_recorders();
    flush.guest = TEST_LOW_GUEST_BASE;
    check( !flush_instruction_cache( &flush ),
           "execute-only LOW guest flush failed\n" );
    check( cache_remove_calls == 1 &&
           cache_remove_start == TEST_LOW_GUEST_BASE &&
           cache_remove_end == TEST_LOW_GUEST_BASE + TEST_PAGE,
           "execute-only LOW guest interval %#llx-%#llx calls %u\n",
           (unsigned long long)cache_remove_start,
           (unsigned long long)cache_remove_end, cache_remove_calls );
    initialize_begin_params( &simulation, TEST_LOW_GUEST_BASE, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           simulation.params.context.rbx == low_second,
           "execute-only LOW guest-flush simulation returned %#x reason %u data %#llx\n",
           (unsigned int)status, simulation.params.stop_reason,
           (unsigned long long)simulation.params.context.rbx );

    build_execute_only_code( test_low_pages, TEST_LOW_GUEST_BASE, low_third );
    reset_cache_recorders();
    flush.guest = TEST_LOW_HOST_BASE;
    check( !flush_instruction_cache( &flush ),
           "execute-only LOW host flush failed\n" );
    check( cache_remove_calls == 1 &&
           cache_remove_start == TEST_LOW_GUEST_BASE &&
           cache_remove_end == TEST_LOW_GUEST_BASE + TEST_PAGE,
           "execute-only LOW host interval %#llx-%#llx calls %u\n",
           (unsigned long long)cache_remove_start,
           (unsigned long long)cache_remove_end, cache_remove_calls );
    initialize_begin_params( &simulation, TEST_LOW_GUEST_BASE, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           simulation.params.context.rbx == low_third,
           "execute-only LOW host-flush simulation returned %#x reason %u data %#llx\n",
           (unsigned int)status, simulation.params.stop_reason,
           (unsigned long long)simulation.params.context.rbx );
done:
    if (thread_initialized) thread_term( NULL );
    initialize_low_range( &low_range, TEST_LOW_GUEST_BASE, TEST_PAGE, 0,
                          MEM_FREE, PAGE_NOACCESS );
    status = publish_low_event( WINE_WOW64_MEMORY_RELEASE, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE, 0, &low_range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status, "execute-only LOW cleanup returned %#x\n",
           (unsigned int)status );
    check( !unregister_identity_page( stack ),
           "execute-only stack cleanup failed\n" );
    check( !unregister_identity_page( identity ),
           "execute-only identity cleanup failed\n" );
    if (failures == starting_failures)
        printf( "XTAJIT64_EXECUTE_ONLY_PASS\n" );
}

static size_t build_syscall_trap_code( unsigned char *page, BOOL use_int2e,
                                       uint32_t syscall, uint64_t argument )
{
    struct code_buffer code = { page, 0 };
    size_t return_offset;

    emit_movabs_rcx( &code, argument );
    emit_u8( &code, 0x49 ); emit_u8( &code, 0x89 ); emit_u8( &code, 0xca );
    emit_u8( &code, 0xb8 ); emit_u32( &code, syscall );
    emit_u8( &code, use_int2e ? 0xcd : 0x0f );
    emit_u8( &code, use_int2e ? 0x2e : 0x05 );
    return_offset = code.offset;
    emit_u8( &code, 0x49 ); emit_u8( &code, 0x89 ); emit_u8( &code, 0xc0 );
    emit_u8( &code, 0x49 ); emit_u8( &code, 0x89 ); emit_u8( &code, 0xc9 );
    emit_u8( &code, 0x4d ); emit_u8( &code, 0x89 ); emit_u8( &code, 0xd4 );
    emit_movabs_rax( &code, test_ec_target );
    emit_jump_rax( &code );
    return return_offset;
}

static void build_syscall_dispatcher_code( unsigned char *page )
{
    struct code_buffer code = { page, 0 };

    emit_u8( &code, 0x48 ); emit_u8( &code, 0x89 ); emit_u8( &code, 0xc3 );
    emit_u8( &code, 0x49 ); emit_u8( &code, 0x89 ); emit_u8( &code, 0xc8 );
    emit_u8( &code, 0x4d ); emit_u8( &code, 0x89 ); emit_u8( &code, 0xd1 );
    emit_u8( &code, 0x49 ); emit_u8( &code, 0x89 ); emit_u8( &code, 0xe5 );
    emit_u8( &code, 0x4c ); emit_u8( &code, 0x8b ); emit_u8( &code, 0x34 );
    emit_u8( &code, 0x24 );
    emit_movabs_rax( &code, test_ec_target );
    emit_jump_rax( &code );
}

static void test_x64_syscall_traps(void)
{
    static const uint64_t arguments[] =
    {
        0x1122334455667788ull,
        0x8877665544332211ull,
    };
    const uint64_t stack_marker = 0xdecafbad12345678ull;
    unsigned char *code = test_pages + 3 * TEST_PAGE;
    unsigned char *stack = test_pages + 4 * TEST_PAGE;
    struct xtajit64_memory_params flush =
    {
        .guest = (uintptr_t)code,
        .size = TEST_PAGE,
    };
    struct simulation simulation = {0};
    struct code_buffer unsupported;
    unsigned int start_count, write_count, read_count;
    size_t return_offset;
    NTSTATUS status;

    memset( code, 0, TEST_PAGE );
    memset( stack, 0, TEST_PAGE );
    check( !register_identity_page( code, PAGE_EXECUTE_READ ),
           "syscall code map failed\n" );
    check( !register_identity_page( stack, PAGE_READWRITE ),
           "syscall stack map failed\n" );
    *(uint64_t *)(stack + TEST_PAGE - 16) = stack_marker;
    status = thread_init( NULL );
    check( !status, "syscall engine init failed %#x\n", (unsigned int)status );
    if (status) goto done;

    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    simulation.params.stack_limit = simulation.params.context.rsp + 1;
    status = begin_simulation( &simulation.params );
    check( status == STATUS_INVALID_PARAMETER,
           "out-of-bounds x64 stack returned %#x\n", (unsigned int)status );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    simulation.params.context.rip = XTAJIT64_X64_USER_ADDRESS_MAX + 1;
    status = begin_simulation( &simulation.params );
    check( status == STATUS_INVALID_PARAMETER,
           "non-canonical x64 RIP returned %#x\n", (unsigned int)status );

    return_offset = build_syscall_trap_code( code, TRUE, 0, arguments[0] );
    check( !flush_instruction_cache( &flush ), "INT 2E code flush failed\n" );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    start_count = atomic_load_explicit( &test_emu_start_count, memory_order_relaxed );
    write_count = atomic_load_explicit( &test_context_write_count, memory_order_relaxed );
    read_count = atomic_load_explicit( &test_context_read_count, memory_order_relaxed );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.stop_reason == XTAJIT64_STOP_EC_TRANSITION,
           "INT 2E returned %#x reason %u\n", (unsigned int)status,
           simulation.params.stop_reason );
    check( simulation.params.context.rbx == 0 &&
           simulation.params.context.r8 == arguments[0] &&
           simulation.params.context.r9 == (uintptr_t)code + return_offset &&
           simulation.params.context.r13 == (uintptr_t)stack + TEST_PAGE - 16 &&
           simulation.params.context.r14 == stack_marker,
           "INT 2E register bridge mismatch\n" );
    check( atomic_load_explicit( &test_emu_start_count, memory_order_relaxed ) ==
               start_count + 2 &&
           atomic_load_explicit( &test_context_write_count, memory_order_relaxed ) ==
               write_count + 1 &&
           atomic_load_explicit( &test_context_read_count, memory_order_relaxed ) ==
               read_count + 1,
           "INT 2E did not resume with one context transfer\n" );

    return_offset = build_syscall_trap_code( code, FALSE,
                                             TEST_SYSCALL_COUNT - 1,
                                             arguments[1] );
    check( !flush_instruction_cache( &flush ), "SYSCALL code flush failed\n" );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           simulation.params.context.rbx == TEST_SYSCALL_COUNT - 1 &&
           simulation.params.context.r8 == arguments[1] &&
           simulation.params.context.r9 == (uintptr_t)code + return_offset,
           "SYSCALL bridge returned %#x reason %u\n", (unsigned int)status,
           simulation.params.stop_reason );

    return_offset = build_syscall_trap_code( code, TRUE, TEST_SYSCALL_COUNT,
                                             arguments[0] );
    check( !flush_instruction_cache( &flush ), "unknown syscall flush failed\n" );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.context.r8 ==
               (UINT64)(INT64)STATUS_INVALID_SYSTEM_SERVICE &&
           simulation.params.context.r9 == arguments[0] &&
           simulation.params.context.r12 == arguments[0] && return_offset == 20,
           "unknown syscall did not resume with STATUS_INVALID_SYSTEM_SERVICE\n" );

    memset( code, 0x90, TEST_PAGE );
    unsupported.data = code;
    unsupported.offset = 0;
    emit_u8( &unsupported, 0xcd ); emit_u8( &unsupported, 0x80 );
    emit_movabs_rax( &unsupported, test_ec_target );
    emit_jump_rax( &unsupported );
    check( !flush_instruction_cache( &flush ), "unsupported interrupt flush failed\n" );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( status == STATUS_NOT_SUPPORTED &&
           simulation.params.stop_reason == XTAJIT64_STOP_INVALID_INSTRUCTION &&
           simulation.params.context.rip == (uintptr_t)code + 2 &&
           !provider.poison_status,
           "unsupported interrupt returned %#x reason %u poison %#x\n",
           (unsigned int)status, simulation.params.stop_reason,
           (unsigned int)provider.poison_status );

    thread_term( NULL );
done:
    unregister_identity_page( stack );
    unregister_identity_page( code );
}

static void build_peer_wait_code( unsigned char *page, uint64_t data,
                                  unsigned int self, unsigned int peer )
{
    struct code_buffer code = { page, 0 };
    size_t loop, branch;

    emit_movabs_rax( &code, data );
    emit_u8( &code, 0xc7 );
    emit_u8( &code, self ? 0x40 : 0x00 );
    if (self) emit_u8( &code, self * 4 );
    emit_u32( &code, 1 );
    loop = code.offset;
    emit_u8( &code, 0x83 ); emit_u8( &code, 0x78 );
    emit_u8( &code, peer * 4 ); emit_u8( &code, 1 );
    emit_u8( &code, 0x72 ); branch = code.offset; emit_u8( &code, 0 );
    patch_rel8( &code, branch, loop );
    emit_movabs_rax( &code, test_ec_target );
    emit_jump_rax( &code );
}

static void test_concurrent_engines(void)
{
    unsigned char *data = test_pages + 3 * TEST_PAGE;
    unsigned char *code0 = test_pages + 4 * TEST_PAGE;
    unsigned char *code1 = test_pages + 5 * TEST_PAGE;
    unsigned char *stack0 = test_pages + 6 * TEST_PAGE;
    unsigned char *stack1 = test_pages + 7 * TEST_PAGE;
    struct simulation simulations[2] = {0};
    pthread_t threads[2];
    unsigned int i;

    memset( data, 0, TEST_PAGE );
    memset( code0, 0, TEST_PAGE );
    memset( code1, 0, TEST_PAGE );
    build_peer_wait_code( code0, (uintptr_t)data, 0, 1 );
    build_peer_wait_code( code1, (uintptr_t)data, 1, 0 );
    check( !register_identity_page( data, PAGE_READWRITE ), "peer data map failed\n" );
    check( !register_identity_page( code0, PAGE_EXECUTE_READ ), "peer code0 map failed\n" );
    check( !register_identity_page( code1, PAGE_EXECUTE_READ ), "peer code1 map failed\n" );
    check( !register_identity_page( stack0, PAGE_READWRITE ), "peer stack0 map failed\n" );
    check( !register_identity_page( stack1, PAGE_READWRITE ), "peer stack1 map failed\n" );

    initialize_begin_params( &simulations[0], (uintptr_t)code0, (uintptr_t)stack0 );
    initialize_begin_params( &simulations[1], (uintptr_t)code1, (uintptr_t)stack1 );
    pthread_create( &threads[0], NULL, run_simulation, &simulations[0] );
    pthread_create( &threads[1], NULL, run_simulation, &simulations[1] );
    for (i = 0; i < 2; ++i)
        check( join_simulation( threads[i], &simulations[i] ),
               "peer engine %u timed out\n", i );
    for (i = 0; i < 2; ++i)
        check( !simulations[i].init_status && !simulations[i].status &&
               simulations[i].params.stop_reason == XTAJIT64_STOP_EC_TRANSITION,
               "peer engine %u returned %#x/%#x reason %u\n", i,
               (unsigned int)simulations[i].init_status,
               (unsigned int)simulations[i].status,
               simulations[i].params.stop_reason );

    unregister_identity_page( stack1 );
    unregister_identity_page( stack0 );
    unregister_identity_page( code1 );
    unregister_identity_page( code0 );
    unregister_identity_page( data );
}

static void build_marker_code( unsigned char *page, uint64_t marker )
{
    struct code_buffer code = { page, 0 };

    emit_u8( &code, 0x49 ); emit_u8( &code, 0xba ); emit_u64( &code, marker );
    emit_movabs_rax( &code, test_ec_target );
    emit_jump_rax( &code );
}

static void test_executable_cache_invalidation(void)
{
    const uint64_t first = 0x1020304050607080ull;
    const uint64_t second = 0x8070605040302010ull;
    unsigned char *code = test_pages + 3 * TEST_PAGE;
    unsigned char *stack = test_pages + 4 * TEST_PAGE;
    struct xtajit64_memory_params flush =
    {
        .guest = (uintptr_t)code,
        .size = 10,
    };
    struct simulation simulation = {0};
    NTSTATUS status;

    memset( code, 0, TEST_PAGE );
    build_marker_code( code, first );
    check( !register_identity_page( code, PAGE_EXECUTE_READ ), "cache code map failed\n" );
    check( !register_identity_page( stack, PAGE_READWRITE ), "cache stack map failed\n" );
    status = thread_init( NULL );
    check( !status, "cache engine init failed %#x\n", (unsigned int)status );
    if (status) goto done;
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.context.r10 == first,
           "first code generation returned %#x/%#llx\n", (unsigned int)status,
           (unsigned long long)simulation.params.context.r10 );

    build_marker_code( code, second );
    reset_cache_recorders();
    check( !flush_instruction_cache( &flush ), "targeted cache flush failed\n" );
    check( cache_remove_calls == 1 && cache_remove_start == (uintptr_t)code &&
           cache_remove_end == (uintptr_t)code + flush.size,
           "targeted cache interval %#llx-%#llx calls %u\n",
           (unsigned long long)cache_remove_start,
           (unsigned long long)cache_remove_end, cache_remove_calls );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.context.r10 == second,
           "cache flush retained stale code %#x/%#llx\n", (unsigned int)status,
           (unsigned long long)simulation.params.context.r10 );
    thread_term( NULL );
done:
    unregister_identity_page( stack );
    unregister_identity_page( code );
}

static void *run_protect( void *arg )
{
    struct protect_worker *worker = arg;

    worker->status = memory_protect( &worker->params );
    atomic_store_explicit( &worker->done, 1, memory_order_release );
    return NULL;
}

static void *run_flush( void *arg )
{
    struct flush_worker *worker = arg;

    worker->status = flush_instruction_cache( &worker->params );
    atomic_store_explicit( &worker->done, 1, memory_order_release );
    return NULL;
}

static void *run_low_observer( void *arg )
{
    struct low_observer_worker *worker = arg;
    void *transaction = NULL;

    worker->status = arm64ec_low_memory_observer.begin(
        arm64ec_low_memory_observer.context, worker->event.operation,
        worker->event.host_address, worker->event.size_covered,
        worker->event.host_allocation_base, &transaction );
    if (!worker->status)
    {
        atomic_store_explicit( &worker->begun, 1, memory_order_release );
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &worker->event );
        worker->status = observer_provider_status();
    }
    atomic_store_explicit( &worker->done, 1, memory_order_release );
    return NULL;
}

static void test_running_mutation_barrier(void)
{
    unsigned char *code = test_pages + 3 * TEST_PAGE;
    unsigned char *stack = test_pages + 4 * TEST_PAGE;
    struct code_buffer buffer = { code, 0 };
    struct simulation simulation = {0};
    struct protect_worker worker =
    {
        .params =
        {
            .guest = (uintptr_t)code,
            .size = TEST_PAGE,
            .protect = PAGE_EXECUTE_READWRITE,
        },
    };
    pthread_t runner, mutator;
    BOOL entered;

    memset( code, 0, TEST_PAGE );
    emit_movabs_rax( &buffer, test_ec_target );
    emit_jump_rax( &buffer );
    check( !register_identity_page( code, PAGE_EXECUTE_READ ), "barrier code map failed\n" );
    check( !register_identity_page( stack, PAGE_READWRITE ), "barrier stack map failed\n" );
    atomic_store( &test_hold_ec_hook, 1 );
    atomic_store( &test_ec_hook_entered, 0 );
    atomic_store( &test_release_ec_hook, 0 );
    atomic_store( &test_mutation_waiters, 0 );
    atomic_store( &test_pause_stop_owner_violation, 0 );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    pthread_create( &runner, NULL, run_simulation, &simulation );
    entered = wait_atomic_int_at_least( &test_ec_hook_entered, 1, 2000 );
    check( entered, "simulation did not enter held EC hook\n" );
    if (entered)
    {
        pthread_create( &mutator, NULL, run_protect, &worker );
        check( wait_mutation_stage( MUTATION_STAGE_WAIT, 2000 ),
               "mapping mutation did not wait for running engine\n" );
        check( !atomic_load_explicit( &worker.done, memory_order_acquire ),
               "mapping mutation published while engine hook was active\n" );
        atomic_store_explicit( &test_release_ec_hook, 1, memory_order_release );
        pthread_join( mutator, NULL );
        check( !worker.status, "mapping mutation returned %#x\n",
               (unsigned int)worker.status );
    }
    else atomic_store_explicit( &test_release_ec_hook, 1, memory_order_release );
    check( join_simulation( runner, &simulation ), "barrier simulation timed out\n" );
    check( !simulation.status &&
           simulation.params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           !atomic_load_explicit( &test_pause_stop_owner_violation,
                                  memory_order_acquire ),
           "barrier lost EC stop or stopped from foreign thread %#x/%u\n",
           (unsigned int)simulation.status, simulation.params.stop_reason );
    atomic_store( &test_hold_ec_hook, 0 );
    atomic_store( &test_release_ec_hook, 1 );
    unregister_identity_page( stack );
    unregister_identity_page( code );
}

static void test_running_low_observer_barrier(void)
{
    unsigned char *code = test_pages + 3 * TEST_PAGE;
    unsigned char *stack = test_pages + 4 * TEST_PAGE;
    struct wine_arm64ec_low_memory_range_v1 full[3], free_range;
    struct code_buffer buffer = { code, 0 };
    struct simulation simulation = {0};
    struct low_observer_worker worker = {0};
    unsigned int pause_count;
    unsigned int starting_failures = failures;
    pthread_t runner, observer;
    BOOL entered;
    NTSTATUS status;

    initialize_low_range( &full[0], 0, TEST_LOW_GUEST_BASE, 0,
                          MEM_FREE, PAGE_NOACCESS );
    initialize_low_range( &full[1], TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_READWRITE );
    initialize_low_range( &full[2], TEST_LOW_GUEST_BASE + TEST_PAGE,
                          WINE_LOW_VA_SHADOW_SIZE - TEST_LOW_GUEST_BASE - TEST_PAGE,
                          0, MEM_FREE, PAGE_NOACCESS );
    status = publish_low_event( WINE_WOW64_MEMORY_RESYNC,
                                WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT,
                                0, WINE_LOW_VA_SHADOW_SIZE, 0, full, 3,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status, "LOW barrier setup snapshot returned %#x\n",
           (unsigned int)status );
    if (status) return;

    memset( code, 0, TEST_PAGE );
    emit_movabs_rax( &buffer, test_ec_target );
    emit_jump_rax( &buffer );
    check( !register_identity_page( code, PAGE_EXECUTE_READ ),
           "LOW barrier code map failed\n" );
    check( !register_identity_page( stack, PAGE_READWRITE ),
           "LOW barrier stack map failed\n" );

    initialize_low_range( &worker.range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_READONLY );
    initialize_low_event( &worker.event, WINE_WOW64_MEMORY_PROTECT, 0,
                          TEST_LOW_GUEST_BASE, TEST_PAGE, TEST_LOW_GUEST_BASE,
                          &worker.range, 1, STATUS_SUCCESS, STATUS_SUCCESS );
    atomic_store( &test_hold_non_ec_hook, 1 );
    atomic_store( &test_non_ec_hook_entered, 0 );
    atomic_store( &test_release_non_ec_hook, 0 );
    atomic_store( &test_mutation_waiters, 0 );
    atomic_store( &test_pause_stop_owner_violation, 0 );
    pause_count = atomic_load_explicit( &test_pause_stop_count,
                                        memory_order_relaxed );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    pthread_create( &runner, NULL, run_simulation, &simulation );
    entered = wait_atomic_int_at_least( &test_non_ec_hook_entered, 1, 2000 );
    check( entered, "simulation did not enter held non-EC hook\n" );
    if (entered)
    {
        pthread_create( &observer, NULL, run_low_observer, &worker );
        check( wait_mutation_stage( MUTATION_STAGE_WAIT, 2000 ),
               "LOW observer did not wait for the running engine\n" );
        check( !atomic_load_explicit( &worker.begun, memory_order_acquire ) &&
               !atomic_load_explicit( &worker.done, memory_order_acquire ),
               "LOW observer returned before the running engine quiesced\n" );
        atomic_store_explicit( &test_release_non_ec_hook, 1,
                               memory_order_release );
        pthread_join( observer, NULL );
        check( !worker.status &&
               canonical_range_matches( TEST_LOW_GUEST_BASE,
                                        TEST_LOW_HOST_BASE, MEM_COMMIT,
                                        UC_PROT_READ,
                                        XTAJIT64_MEMORY_ADDRESS_AMD64_LOW,
                                        FALSE ),
               "LOW observer barrier publish returned %#x\n",
               (unsigned int)worker.status );
    }
    else atomic_store_explicit( &test_release_non_ec_hook, 1,
                                memory_order_release );
    check( join_simulation( runner, &simulation ),
           "LOW barrier simulation timed out\n" );
    check( !simulation.status &&
           simulation.params.stop_reason == XTAJIT64_STOP_EC_TRANSITION &&
           atomic_load_explicit( &test_pause_stop_count,
                                 memory_order_relaxed ) == pause_count + 1 &&
           !atomic_load_explicit( &test_pause_stop_owner_violation,
                                  memory_order_acquire ),
           "LOW barrier lost owner-thread pause or EC stop %#x/%u\n",
           (unsigned int)simulation.status, simulation.params.stop_reason );
    atomic_store( &test_hold_non_ec_hook, 0 );
    atomic_store( &test_release_non_ec_hook, 1 );
    unregister_identity_page( stack );
    unregister_identity_page( code );

    initialize_low_range( &free_range, 0, WINE_LOW_VA_SHADOW_SIZE, 0,
                          MEM_FREE, PAGE_NOACCESS );
    status = publish_low_event( WINE_WOW64_MEMORY_RESYNC,
                                WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT,
                                0, WINE_LOW_VA_SHADOW_SIZE, 0, &free_range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status, "LOW barrier cleanup snapshot returned %#x\n",
           (unsigned int)status );
    if (failures == starting_failures)
        printf( "XTAJIT64_LOW_OBSERVER_BARRIER_PASS\n" );
}

static void test_running_flush_preflight_failure(void)
{
    unsigned char *code = test_pages + 3 * TEST_PAGE;
    unsigned char *stack = test_pages + 4 * TEST_PAGE;
    struct wine_arm64ec_low_memory_range_v1 range;
    struct code_buffer buffer = { code, 0 };
    struct simulation simulation = {0};
    struct flush_worker worker =
    {
        .params =
        {
            .guest = TEST_LOW_GUEST_BASE + 0x40,
            .size = 0x80,
        },
    };
    pthread_t runner, flusher;
    BOOL runner_created = FALSE, flusher_created = FALSE;
    BOOL entered = FALSE, prompt = FALSE;
    uint64_t generation = 0;
    size_t range_count = 0;
    unsigned int pause_count = 0;
    unsigned int starting_failures = failures;
    NTSTATUS status;
    int ret;

    initialize_low_range( &range, TEST_LOW_GUEST_BASE, TEST_PAGE,
                          TEST_LOW_GUEST_BASE, MEM_COMMIT, PAGE_READONLY );
    status = publish_low_event( WINE_WOW64_MEMORY_ALLOCATE, 0,
                                TEST_LOW_GUEST_BASE, TEST_PAGE,
                                TEST_LOW_GUEST_BASE, &range, 1,
                                STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status, "flush preflight LOW setup returned %#x\n",
           (unsigned int)status );
    if (status) goto done;

    memset( code, 0, TEST_PAGE );
    emit_movabs_rax( &buffer, test_ec_target );
    emit_jump_rax( &buffer );
    check( !register_identity_page( code, PAGE_EXECUTE_READ ),
           "flush preflight code map failed\n" );
    check( !register_identity_page( stack, PAGE_READWRITE ),
           "flush preflight stack map failed\n" );
    if (failures != starting_failures) goto done;

    atomic_store( &test_hold_ec_hook, 1 );
    atomic_store( &test_ec_hook_entered, 0 );
    atomic_store( &test_release_ec_hook, 0 );
    initialize_begin_params( &simulation, (uintptr_t)code, (uintptr_t)stack );
    ret = pthread_create( &runner, NULL, run_simulation, &simulation );
    check( !ret, "flush preflight runner creation failed %d\n", ret );
    if (!ret)
    {
        runner_created = TRUE;
        entered = wait_atomic_int_at_least( &test_ec_hook_entered, 1, 2000 );
        check( entered, "flush preflight runner did not enter the held EC hook\n" );
    }
    if (!entered) goto release;

    generation = observer_generation();
    pthread_mutex_lock( &provider.mutex );
    range_count = provider.ranges.count;
    pthread_mutex_unlock( &provider.mutex );
    pause_count = atomic_load_explicit( &test_pause_stop_count,
                                        memory_order_relaxed );
    reset_cache_recorders();
    test_flush_interval_append_count = 0;
    test_fail_flush_interval_append = 0;
    ret = pthread_create( &flusher, NULL, run_flush, &worker );
    check( !ret, "flush preflight worker creation failed %d\n", ret );
    if (!ret)
    {
        flusher_created = TRUE;
        prompt = wait_atomic_int_at_least( &worker.done, 1, 1000 );
        check( prompt,
               "flush preflight failure waited on an engine that was never paused\n" );
    }

release:
    atomic_store_explicit( &test_release_ec_hook, 1, memory_order_release );
    if (flusher_created) pthread_join( flusher, NULL );
    test_fail_flush_interval_append = -1;
    if (runner_created)
        check( join_simulation( runner, &simulation ),
               "flush preflight simulation timed out\n" );
    atomic_store( &test_hold_ec_hook, 0 );
    atomic_store( &test_release_ec_hook, 1 );

    if (flusher_created)
    {
        check( prompt && worker.status == STATUS_NO_MEMORY &&
               test_flush_interval_append_count == 1,
               "flush preflight failure returned prompt/status/count %u/%#x/%d\n",
               prompt, (unsigned int)worker.status,
               test_flush_interval_append_count );
        check( !cache_remove_calls && !cache_flush_calls,
               "flush preflight failure touched remove/full cache %u/%u\n",
               cache_remove_calls, cache_flush_calls );
        check( !observer_provider_status() &&
               observer_generation() == generation && canonical_range_matches(
                   TEST_LOW_GUEST_BASE, TEST_LOW_HOST_BASE, MEM_COMMIT,
                   UC_PROT_READ, XTAJIT64_MEMORY_ADDRESS_AMD64_LOW, FALSE ),
               "flush preflight failure poisoned or changed the registry %#x\n",
               (unsigned int)observer_provider_status() );
        pthread_mutex_lock( &provider.mutex );
        check( provider.ranges.count == range_count && !provider.mutating &&
               !provider.mutation_owner_valid &&
               provider.mutation_stage == MUTATION_STAGE_IDLE &&
               !provider.observer_transaction,
               "flush preflight failure retained mutation ownership\n" );
        pthread_mutex_unlock( &provider.mutex );
        check( atomic_load_explicit( &test_pause_stop_count,
                                     memory_order_relaxed ) == pause_count,
               "flush preflight failure requested an engine pause\n" );

        status = thread_init( NULL );
        check( !status, "flush preflight retry engine init returned %#x\n",
               (unsigned int)status );
        if (!status)
        {
            reset_cache_recorders();
            status = flush_instruction_cache( &worker.params );
            check( !status && cache_remove_calls == 1 && !cache_flush_calls &&
                   cache_remove_start == worker.params.guest &&
                   cache_remove_end == worker.params.guest + worker.params.size,
                   "flush preflight retry returned %#x remove/full %#x-%#x %u/%u\n",
                   (unsigned int)status, (unsigned int)cache_remove_start,
                   (unsigned int)cache_remove_end, cache_remove_calls,
                   cache_flush_calls );
            thread_term( NULL );
        }
    }

done:
    test_fail_flush_interval_append = -1;
    reset_cache_recorders();
    check( !reset_test_provider(), "reset after flush preflight failure failed\n" );
    if (failures == starting_failures)
        printf( "XTAJIT64_FLUSH_PREFLIGHT_FAILURE_PASS\n" );
}

int main(void)
{
    uint64_t highest, page;
    uint64_t *bitmap;
    size_t bitmap_size;
    NTSTATUS status;

    test_pages = alloc_preferred_pages( TEST_PREFERRED_BASE, TEST_FALLBACK_BASE,
                                        TEST_PAGE_COUNT );
    test_kuser = alloc_preferred_pages( TEST_PREFERRED_KUSER, TEST_FALLBACK_KUSER, 1 );
    test_low_pages = alloc_pages_at( TEST_LOW_HOST_BASE, TEST_LOW_PAGE_COUNT );
    if (!test_pages) test_pages = alloc_pages_at( TEST_ASAN_BASE, TEST_PAGE_COUNT );
    if (!test_kuser) test_kuser = alloc_pages_at( TEST_ASAN_KUSER, 1 );
    check( test_pages && test_kuser && test_low_pages,
           "provider test address allocation failed pages %p KUSER %p LOW %p\n",
           test_pages, test_kuser, test_low_pages );
    if (!test_pages || !test_kuser || !test_low_pages) return 1;
    test_base = (uintptr_t)test_pages;
    test_ec_target = test_base;
    test_syscall_dispatcher = test_base + TEST_PAGE;
    test_teb = test_base + 2 * TEST_PAGE;
    highest = max( test_base + TEST_PAGE_COUNT * TEST_PAGE,
                   (uint64_t)(uintptr_t)test_kuser + TEST_PAGE ) - 1;
    highest = max( highest + 1,
                   (uint64_t)(uintptr_t)test_low_pages +
                   TEST_LOW_PAGE_COUNT * TEST_PAGE ) - 1;
    bitmap_size = ((highest / TEST_PAGE + 63) / 64) * sizeof(*bitmap);
    check( bitmap_size && bitmap_size <= 32 * 1024 * 1024,
           "EC bitmap size %lu is outside the test bound\n",
           (unsigned long)bitmap_size );
    bitmap = bitmap_size <= 32 * 1024 * 1024 ? calloc( 1, bitmap_size ) : NULL;
    if (!bitmap) return 1;
    page = test_ec_target / TEST_PAGE;
    bitmap[page / 64] |= 1ull << (page & 63);
    build_syscall_dispatcher_code( (void *)(uintptr_t)test_syscall_dispatcher );

    process_params.ec_bitmap = (uintptr_t)bitmap;
    process_params.highest_user_address = highest;
    process_params.guest_kuser = XTAJIT64_GUEST_KUSER;
    process_params.host_kuser = (uintptr_t)test_kuser;
    process_params.kuser_size = TEST_PAGE;
    process_params.rtl_exit_user_thread = test_ec_target;
    process_params.abi_version = XTAJIT64_PROCESS_ABI_VERSION;
    process_params.abi_size = sizeof(process_params);
    process_params.required_capabilities = XTAJIT64_CAPABILITIES;
    process_params.x64_syscall_dispatcher = test_syscall_dispatcher;
    process_params.x64_syscall_count = TEST_SYSCALL_COUNT;

    test_process_init_abi();
    status = process_init( &process_params );
    check( !status && process_params.enabled_capabilities == XTAJIT64_CAPABILITIES,
           "process init returned %#x capabilities %#x\n", (unsigned int)status,
           process_params.enabled_capabilities );
    if (status) return 1;
    check( provider.ranges.count == 1 && !provider.ranges.data[0].flags &&
           provider.ranges.data[0].permanent,
           "KUSER registry metadata was not initialized deterministically\n" );
    test_low_observer_validation();
    test_low_observer_interval_replacement();
    test_low_observer_partial_engine_poison();
    test_low_flush_multi_engine_poison();
    check( !register_identity_page( (void *)(uintptr_t)test_ec_target,
                                    PAGE_EXECUTE_READ ),
           "EC target map failed\n" );
    check( !register_identity_page( (void *)(uintptr_t)test_syscall_dispatcher,
                                    PAGE_EXECUTE_READ ),
           "syscall dispatcher map failed\n" );
    check( !register_identity_page( (void *)(uintptr_t)test_teb, PAGE_READWRITE ),
           "TEB map failed\n" );

    test_identity_codec();
    test_execute_only_read_and_execute();
    test_x64_syscall_traps();
    test_concurrent_engines();
    test_executable_cache_invalidation();
    test_running_mutation_barrier();
    test_running_low_observer_barrier();
    test_running_flush_preflight_failure();

    unregister_identity_page( (void *)(uintptr_t)test_teb );
    unregister_identity_page( (void *)(uintptr_t)test_syscall_dispatcher );
    unregister_identity_page( (void *)(uintptr_t)test_ec_target );
    check( !process_term( NULL ) && !process_term( NULL ),
           "idempotent process termination failed\n" );
    munmap( test_kuser, TEST_PAGE );
    munmap( test_pages, TEST_PAGE_COUNT * TEST_PAGE );
    munmap( test_low_pages, TEST_LOW_PAGE_COUNT * TEST_PAGE );
    free( bitmap );
    if (failures)
    {
        fprintf( stderr, "%u xtajit64 native provider checks failed\n", failures );
        return 1;
    }
    printf( "xtajit64 native provider checks passed\n" );
    return 0;
}
