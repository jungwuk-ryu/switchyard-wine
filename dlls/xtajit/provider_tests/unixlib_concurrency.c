/*
 * Native xtajit i386 observer, mapping, and concurrency tests
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

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unicorn/unicorn.h>

static uint64_t test_cache_remove_start;
static uint64_t test_cache_remove_end;
static unsigned int test_cache_remove_calls;

static uc_err record_cache_remove( uc_engine *uc, uint64_t start, uint64_t end )
{
    test_cache_remove_start = start;
    test_cache_remove_end = end;
    ++test_cache_remove_calls;
    return uc_ctl_remove_cache( uc, start, end );
}

#undef uc_ctl_remove_cache
#define uc_ctl_remove_cache record_cache_remove

/* Keep the observer transaction and engine registry private in production.
 * This focused native regression includes the implementation so it can prove
 * fail-closed partial publication without adding a test-control Unix ABI. */
#include "../unixlib.c"

#undef uc_ctl_remove_cache

#define TEST_PAGE       XTAJIT_GUEST_PAGE_SIZE
#define TEST_HIGHEST    0x7ffeffffu
#define TEST_DATA       0x00100000u
#define TEST_CODE0      0x00200000u
#define TEST_CODE1      0x00300000u
#define TEST_STACK0     0x00400000u
#define TEST_STACK1     0x00500000u
#define TEST_TEB0       0x00600000u
#define TEST_TEB1       0x00700000u
#define TEST_MIXED      0x00800000u
#define TEST_MUTATION   0x00900000u
#define TEST_SELECTED   0x00a00000u
#define TEST_FAULT_CODE 0x00b00000u
#define TEST_FAULT_STACK 0x00c00000u
#define TEST_FAULT_TEB  0x00d00000u
#define TEST_GUARD      0x00e00000u
#define TEST_STACK_GROW 0x00f00000u
#define TEST_WRITEWATCH 0x01000000u
#define TEST_PROTECTED  0x01100000u
#define TEST_SEC_RESERVE 0x01200000u
#define TEST_CONCURRENT_FAULT0 0x01300000u
#define TEST_CONCURRENT_FAULT1 0x01400000u
#define TEST_PAUSE_CODE  0x01500000u
#define TEST_PAUSE_STACK 0x01600000u
#define TEST_PAUSE_TEB   0x01700000u
#define TEST_STRESS_CODE 0x01800000u
#define TEST_STRESS_STACK 0x01b00000u
#define TEST_STRESS_TEB  0x01c00000u
#define TEST_REP_BUFFER  0x02000000u
#define TEST_REP_SIZE    (16 * 1024 * 1024u)
#define TEST_STOP_FLAG   (TEST_DATA + 0x200)
#define TEST_ENTERED_FLAG (TEST_DATA + 0x204)

static unsigned int failures;
static struct xtajit_process_init_params process_params;
static void *kuser_page;
static uint64_t test_shadow_base;

#define check(condition, ...) \
    do { if (!(condition)) { fprintf( stderr, "not ok: " __VA_ARGS__ ); ++failures; } } while (0)

struct simulation
{
    struct xtajit_begin_params params;
    atomic_int ready;
    atomic_int *start;
    atomic_int done;
    NTSTATUS init_status;
    NTSTATUS status;
};

struct holder
{
    atomic_int ready;
    atomic_int *release;
    NTSTATUS status;
};

struct mutation
{
    struct wine_wow64_memory_range_v1 range;
    uint32_t operation;
    NTSTATUS status;
    atomic_int done;
};

struct fault_worker
{
    uint32_t guest;
    atomic_int ready;
    atomic_int *start;
    atomic_int done;
    NTSTATUS init_status;
    NTSTATUS status;
    uint32_t action;
};

static void initialize_fault( struct xtajit_fault_params *params,
                              uint32_t guest, uint32_t unicorn_type );

static uint64_t shadow_address( uint64_t guest )
{
    return test_shadow_base + guest;
}

static void *map_shadow_pages( uint64_t guest, size_t size )
{
    void *address = (void *)(uintptr_t)shadow_address( guest );
    int flags = MAP_PRIVATE | MAP_ANON | MAP_FIXED;
    void *ret;

    ret = mmap( address, size, PROT_READ | PROT_WRITE, flags, -1, 0 );
    return ret == MAP_FAILED || ret != address ? NULL : ret;
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

static uint64_t elapsed_microseconds( const struct timespec *start,
                                      const struct timespec *now )
{
    time_t seconds = now->tv_sec - start->tv_sec;
    long nanoseconds = now->tv_nsec - start->tv_nsec;

    if (nanoseconds < 0)
    {
        --seconds;
        nanoseconds += 1000000000l;
    }
    return (uint64_t)seconds * 1000000 + nanoseconds / 1000;
}

static BOOL wait_int( atomic_int *value, int expected, unsigned int timeout_ms )
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

static BOOL wait_guest_u32( volatile uint32_t *value, uint32_t expected,
                            unsigned int timeout_ms )
{
    struct timespec start, now;

    clock_gettime( CLOCK_MONOTONIC, &start );
    do
    {
        if (*value == expected) return TRUE;
        sched_yield();
        clock_gettime( CLOCK_MONOTONIC, &now );
    } while (elapsed_milliseconds( &start, &now ) < timeout_ms);
    return FALSE;
}

static void initialize_range( struct wine_wow64_memory_range_v1 *range,
                              uint64_t guest, uint64_t size, uint64_t allocation_base,
                              uint32_t state, uint32_t protect, BOOL translated )
{
    memset( range, 0, sizeof(*range) );
    range->address = shadow_address( guest );
    range->size = size;
    range->allocation_base = allocation_base ? shadow_address( allocation_base ) : 0;
    range->state = state;
    range->protect = protect;
    if (translated) range->flags = WINE_WOW64_MEMORY_RANGE_TRANSLATED;
}

static NTSTATUS publish_ranges( uint32_t operation,
                                struct wine_wow64_memory_range_v1 *ranges,
                                size_t count, NTSTATUS mutation_status,
                                NTSTATUS snapshot_status )
{
    struct wine_wow64_memory_event_v1 event = {0};
    void *transaction = NULL;
    uint64_t size = 0;
    NTSTATUS status;
    size_t i;

    if (!count) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < count; ++i) size += ranges[i].size;
    event.version = WINE_WOW64_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = operation;
    event.status = mutation_status;
    event.snapshot_status = snapshot_status;
    event.address = ranges[0].address;
    event.size_covered = size;
    event.allocation_base = ranges[0].allocation_base;
    event.ranges = ranges;
    event.range_count = count;
    status = memory_observer.begin( memory_observer.context, operation, event.address,
                                    event.size_covered, event.allocation_base, &transaction );
    if (!status) memory_observer.complete( memory_observer.context, transaction, &event );
    pthread_mutex_lock( &provider.mutex );
    if (!status && provider.poison_status) status = provider.poison_status;
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static NTSTATUS publish_page( uint32_t operation, uint64_t guest,
                              uint64_t allocation_base, uint32_t state,
                              uint32_t protect, BOOL translated )
{
    struct wine_wow64_memory_range_v1 range;

    initialize_range( &range, guest, TEST_PAGE, allocation_base,
                      state, protect, translated );
    return publish_ranges( operation, &range, 1, STATUS_SUCCESS, STATUS_SUCCESS );
}

static NTSTATUS publish_fault_snapshot( uint64_t hint_guest,
                                        struct wine_wow64_memory_range_v1 *ranges,
                                        size_t count )
{
    struct wine_wow64_memory_event_v1 event = {0};
    void *transaction = NULL;
    uint64_t size = 0;
    NTSTATUS status;
    size_t i;

    if (!count) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < count; ++i) size += ranges[i].size;
    event.version = WINE_WOW64_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_PROTECT;
    event.address = ranges[0].address;
    event.size_covered = size;
    event.allocation_base = ranges[0].allocation_base;
    event.ranges = ranges;
    event.range_count = count;
    status = memory_observer.begin( memory_observer.context, event.operation,
                                    shadow_address( hint_guest & ~(uint64_t)(TEST_PAGE - 1) ),
                                    TEST_PAGE, 0, &transaction );
    if (!status) memory_observer.complete( memory_observer.context, transaction, &event );
    pthread_mutex_lock( &provider.mutex );
    if (!status && provider.poison_status) status = provider.poison_status;
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

enum fault_test_case
{
    FAULT_TEST_GUARD,
    FAULT_TEST_STACK_GROW,
    FAULT_TEST_WRITEWATCH,
    FAULT_TEST_MIXED_DISARM,
    FAULT_TEST_EXEC_WRITE,
    FAULT_TEST_PROTECTION,
    FAULT_TEST_IN_PAGE,
    FAULT_TEST_SEC_RESERVE,
    FAULT_TEST_BRIDGE_FAILURE,
};

static enum fault_test_case fault_test_case;
static unsigned int fault_resolver_calls;
static atomic_int concurrent_resolver_entries;
static atomic_int concurrent_transaction_held;
static atomic_int release_concurrent_transaction;

static int32_t resolve_test_fault( uint64_t host, uint32_t access,
                                   struct wine_wow64_memory_fault_result_v1 *result )
{
    struct wine_wow64_memory_range_v1 ranges[2];
    uint64_t guest;
    NTSTATUS status;

    ++fault_resolver_calls;
    if (!result || result->version != WINE_WOW64_MEMORY_FAULT_VERSION ||
        result->size < sizeof(*result) || host < test_shadow_base ||
        host - test_shadow_base >= WINE_LOW_VA_SHADOW_SIZE)
        return STATUS_INVALID_PARAMETER;
    guest = host - test_shadow_base;
    if (fault_test_case == FAULT_TEST_BRIDGE_FAILURE) return STATUS_NO_MEMORY;

    memset( result, 0, sizeof(*result) );
    result->version = WINE_WOW64_MEMORY_FAULT_VERSION;
    result->size = sizeof(*result);
    result->action = WINE_WOW64_MEMORY_FAULT_RETRY;

    switch (fault_test_case)
    {
    case FAULT_TEST_GUARD:
        initialize_range( &ranges[0], TEST_GUARD, TEST_PAGE, TEST_GUARD,
                          MEM_COMMIT, PAGE_READWRITE, TRUE );
        status = publish_fault_snapshot( guest, ranges, 1 );
        result->action = WINE_WOW64_MEMORY_FAULT_RAISE;
        result->status = STATUS_GUARD_PAGE_VIOLATION;
        result->parameter_count = 2;
        break;
    case FAULT_TEST_STACK_GROW:
        initialize_range( &ranges[0], TEST_STACK_GROW, TEST_PAGE,
                          TEST_STACK_GROW, MEM_COMMIT,
                          PAGE_READWRITE | PAGE_GUARD, TRUE );
        initialize_range( &ranges[1], TEST_STACK_GROW + TEST_PAGE, TEST_PAGE,
                          TEST_STACK_GROW, MEM_COMMIT, PAGE_READWRITE, TRUE );
        status = publish_fault_snapshot( guest, ranges, 2 );
        break;
    case FAULT_TEST_WRITEWATCH:
        initialize_range( &ranges[0], TEST_WRITEWATCH, TEST_PAGE,
                          TEST_WRITEWATCH, MEM_COMMIT, PAGE_READWRITE, TRUE );
        status = publish_fault_snapshot( guest, ranges, 1 );
        break;
    case FAULT_TEST_MIXED_DISARM:
        if ((guest & ~(uint64_t)(TEST_PAGE - 1)) != TEST_MIXED + 2 * TEST_PAGE)
            return STATUS_INVALID_ADDRESS;
        initialize_range( &ranges[0], TEST_MIXED + 2 * TEST_PAGE, TEST_PAGE,
                          TEST_MIXED, MEM_COMMIT, PAGE_READWRITE, TRUE );
        status = publish_fault_snapshot( guest, ranges, 1 );
        break;
    case FAULT_TEST_EXEC_WRITE:
        if ((guest & ~(uint64_t)(TEST_PAGE - 1)) != TEST_MIXED + TEST_PAGE)
            return STATUS_INVALID_ADDRESS;
        status = STATUS_SUCCESS;
        result->action = WINE_WOW64_MEMORY_FAULT_RAISE;
        result->status = STATUS_IN_PAGE_ERROR;
        result->parameter_count = 3;
        result->information[2] =
            (uint64_t)(int64_t)STATUS_EXECUTABLE_MEMORY_WRITE;
        break;
    case FAULT_TEST_PROTECTION:
        initialize_range( &ranges[0], TEST_PROTECTED, TEST_PAGE,
                          TEST_PROTECTED, MEM_COMMIT, PAGE_READONLY, TRUE );
        status = publish_fault_snapshot( guest, ranges, 1 );
        result->action = WINE_WOW64_MEMORY_FAULT_RAISE;
        result->status = STATUS_ACCESS_VIOLATION;
        result->parameter_count = 2;
        break;
    case FAULT_TEST_IN_PAGE:
        initialize_range( &ranges[0], TEST_PROTECTED, TEST_PAGE,
                          TEST_PROTECTED, MEM_COMMIT, PAGE_READONLY, TRUE );
        status = publish_fault_snapshot( guest, ranges, 1 );
        result->action = WINE_WOW64_MEMORY_FAULT_RAISE;
        result->status = STATUS_IN_PAGE_ERROR;
        result->parameter_count = 3;
        result->information[2] =
            (uint64_t)(int64_t)STATUS_EXECUTABLE_MEMORY_WRITE;
        break;
    case FAULT_TEST_SEC_RESERVE:
        initialize_range( &ranges[0], TEST_SEC_RESERVE, TEST_PAGE,
                          TEST_SEC_RESERVE, MEM_COMMIT, PAGE_READWRITE, TRUE );
        status = publish_fault_snapshot( guest, ranges, 1 );
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    if (status) return status;
    if (result->action == WINE_WOW64_MEMORY_FAULT_RAISE)
    {
        result->information[0] = access;
        result->information[1] = host;
    }
    return STATUS_SUCCESS;
}

static int32_t resolve_concurrent_test_fault(
    uint64_t host, uint32_t access,
    struct wine_wow64_memory_fault_result_v1 *result )
{
    struct wine_wow64_memory_range_v1 range;
    struct wine_wow64_memory_event_v1 event = {0};
    void *transaction = NULL;
    uint64_t guest, page;
    int order;
    NTSTATUS status;

    if (!result || result->version != WINE_WOW64_MEMORY_FAULT_VERSION ||
        result->size < sizeof(*result) || access != WINE_WOW64_MEMORY_FAULT_WRITE ||
        host < test_shadow_base || host - test_shadow_base >= WINE_LOW_VA_SHADOW_SIZE)
        return STATUS_INVALID_PARAMETER;
    guest = host - test_shadow_base;
    page = guest & ~(uint64_t)(TEST_PAGE - 1);
    if (page != TEST_CONCURRENT_FAULT0 && page != TEST_CONCURRENT_FAULT1)
        return STATUS_INVALID_ADDRESS;
    order = atomic_fetch_add_explicit( &concurrent_resolver_entries, 1,
                                       memory_order_acq_rel );

    initialize_range( &range, page, TEST_PAGE, page,
                      MEM_COMMIT, PAGE_READWRITE, TRUE );
    event.version = WINE_WOW64_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_PROTECT;
    event.address = range.address;
    event.size_covered = range.size;
    event.allocation_base = range.allocation_base;
    event.ranges = &range;
    event.range_count = 1;
    status = memory_observer.begin( memory_observer.context, event.operation,
                                    range.address, range.size, 0, &transaction );
    if (status) return status;
    if (!order)
    {
        atomic_store_explicit( &concurrent_transaction_held, 1, memory_order_release );
        while (!atomic_load_explicit( &release_concurrent_transaction,
                                      memory_order_acquire ))
            sched_yield();
    }
    memory_observer.complete( memory_observer.context, transaction, &event );
    pthread_mutex_lock( &provider.mutex );
    status = provider.poison_status;
    pthread_mutex_unlock( &provider.mutex );
    if (status) return status;

    memset( result, 0, sizeof(*result) );
    result->version = WINE_WOW64_MEMORY_FAULT_VERSION;
    result->size = sizeof(*result);
    result->action = WINE_WOW64_MEMORY_FAULT_RETRY;
    return STATUS_SUCCESS;
}

static void *run_fault_worker( void *arg )
{
    struct fault_worker *worker = arg;
    struct xtajit_fault_params fault;

    worker->init_status = thread_init( NULL );
    atomic_store_explicit( &worker->ready, 1, memory_order_release );
    while (worker->start &&
           !atomic_load_explicit( worker->start, memory_order_acquire ))
        sched_yield();
    if (!worker->init_status)
    {
        memset( &fault, 0, sizeof(fault) );
        fault.guest = worker->guest + 8;
        fault.unicorn_type = UC_MEM_WRITE_UNMAPPED;
        fault.result.version = WINE_WOW64_MEMORY_FAULT_VERSION;
        fault.result.size = sizeof(fault.result);
        worker->status = resolve_memory_fault( &fault );
        worker->action = fault.result.action;
        thread_term( NULL );
    }
    atomic_store_explicit( &worker->done, 1, memory_order_release );
    return NULL;
}

static const struct mapped_range *find_registry_range( uint64_t guest )
{
    size_t i;

    for (i = 0; i < provider.ranges.count; ++i)
    {
        const struct mapped_range *range = &provider.ranges.data[i];

        if (guest >= range->guest && guest < range->guest + range->size) return range;
    }
    return NULL;
}

static BOOL engine_page_has_perms( struct xtajit_engine *engine,
                                   uint64_t guest, unsigned int perms )
{
    uc_mem_region *regions;
    uint32_t count, i;
    BOOL found = FALSE;

    if (uc_mem_regions( engine->uc, &regions, &count ) != UC_ERR_OK) return FALSE;
    for (i = 0; i < count; ++i)
    {
        if (guest >= regions[i].begin && guest <= regions[i].end)
        {
            found = regions[i].perms == perms;
            break;
        }
    }
    uc_free( regions );
    return found;
}

static void initialize_simulation( struct simulation *simulation, uint32_t eip,
                                   uint32_t stack, uint32_t teb )
{
    memset( simulation, 0, sizeof(*simulation) );
    simulation->params.context.eip = eip;
    simulation->params.context.esp = stack + TEST_PAGE - 16;
    simulation->params.context.eflags = 0x202;
    simulation->params.context.mxcsr = 0x1f80;
    simulation->params.context.fp_control = 0x037f;
    simulation->params.teb_guest = teb;
}

static void *run_simulation( void *arg )
{
    struct simulation *simulation = arg;

    simulation->init_status = thread_init( NULL );
    atomic_store_explicit( &simulation->ready, 1, memory_order_release );
    while (simulation->start &&
           !atomic_load_explicit( simulation->start, memory_order_acquire ))
        sched_yield();
    if (!simulation->init_status)
        simulation->status = begin_simulation( &simulation->params );
    thread_term( NULL );
    atomic_store_explicit( &simulation->done, 1, memory_order_release );
    return NULL;
}

static void *hold_engine( void *arg )
{
    struct holder *holder = arg;

    holder->status = thread_init( NULL );
    atomic_store_explicit( &holder->ready, 1, memory_order_release );
    while (!atomic_load_explicit( holder->release, memory_order_acquire )) sched_yield();
    thread_term( NULL );
    return NULL;
}

static void emit_u8( unsigned char *code, size_t *offset, unsigned int value )
{
    code[(*offset)++] = value;
}

static void emit_u32( unsigned char *code, size_t *offset, uint32_t value )
{
    memcpy( code + *offset, &value, sizeof(value) );
    *offset += sizeof(value);
}

static void build_peer_code( unsigned char *code, unsigned int self, unsigned int peer )
{
    size_t offset = 0, loop, branch;
    intptr_t displacement;

    emit_u8( code, &offset, 0xb8 );
    emit_u32( code, &offset, TEST_DATA );
    emit_u8( code, &offset, 0xc7 );
    if (self)
    {
        emit_u8( code, &offset, 0x40 );
        emit_u8( code, &offset, self * 4 );
    }
    else emit_u8( code, &offset, 0x00 );
    emit_u32( code, &offset, 1 );
    loop = offset;
    emit_u8( code, &offset, 0x83 );
    emit_u8( code, &offset, 0x78 );
    emit_u8( code, &offset, peer * 4 );
    emit_u8( code, &offset, 1 );
    emit_u8( code, &offset, 0x72 );
    branch = offset++;
    displacement = (intptr_t)loop - (intptr_t)(branch + 1);
    check( displacement >= INT8_MIN && displacement <= INT8_MAX,
           "peer loop branch is out of range\n" );
    code[branch] = (unsigned char)(int8_t)displacement;
    emit_u8( code, &offset, 0xb8 );
    emit_u32( code, &offset, XTAJIT_GUEST_UNIX_BOP );
    emit_u8( code, &offset, 0xff );
    emit_u8( code, &offset, 0xe0 );
}

static void build_stop_code( unsigned char *code )
{
    size_t offset = 0;

    emit_u8( code, &offset, 0xb8 );
    emit_u32( code, &offset, XTAJIT_GUEST_UNIX_BOP );
    emit_u8( code, &offset, 0xff );
    emit_u8( code, &offset, 0xe0 );
}

static void build_marker_code( unsigned char *code, uint32_t marker )
{
    size_t offset = 0;

    emit_u8( code, &offset, 0xb9 );
    emit_u32( code, &offset, marker );
    emit_u8( code, &offset, 0xb8 );
    emit_u32( code, &offset, XTAJIT_GUEST_UNIX_BOP );
    emit_u8( code, &offset, 0xff );
    emit_u8( code, &offset, 0xe0 );
}

static void emit_absolute_store( unsigned char *code, size_t *offset,
                                 uint32_t address, uint32_t value )
{
    emit_u8( code, offset, 0xc7 );
    emit_u8( code, offset, 0x05 );
    emit_u32( code, offset, address );
    emit_u32( code, offset, value );
}

static void emit_jump_equal( unsigned char *code, size_t *offset, size_t target )
{
    size_t displacement_offset;
    int64_t displacement;

    emit_u8( code, offset, 0x0f );
    emit_u8( code, offset, 0x84 );
    displacement_offset = *offset;
    emit_u32( code, offset, 0 );
    displacement = (int64_t)target - (int64_t)*offset;
    check( displacement >= INT32_MIN && displacement <= INT32_MAX,
           "guest relative branch is out of range\n" );
    {
        int32_t rel32 = displacement;
        memcpy( code + displacement_offset, &rel32, sizeof(rel32) );
    }
}

static void emit_stop_flag_compare( unsigned char *code, size_t *offset )
{
    emit_u8( code, offset, 0x83 );
    emit_u8( code, offset, 0x3d );
    emit_u32( code, offset, TEST_STOP_FLAG );
    emit_u8( code, offset, 0x00 );
}

static void emit_wait_for_stop_flag( unsigned char *code, size_t *offset )
{
    size_t loop = *offset;

    emit_stop_flag_compare( code, offset );
    emit_jump_equal( code, offset, loop );
}

static void build_pause_context_code( unsigned char *code )
{
    size_t offset = 0;

    emit_u8( code, &offset, 0xbb );
    emit_u32( code, &offset, 0x11111111 );
    emit_u8( code, &offset, 0x83 );
    emit_u8( code, &offset, 0xc3 );
    emit_u8( code, &offset, 1 );
    emit_u8( code, &offset, 0x8b );
    emit_u8( code, &offset, 0x2d );
    emit_u32( code, &offset, TEST_DATA + 0x100 );
    emit_u8( code, &offset, 0xb9 );
    emit_u32( code, &offset, 0x22222222 );
    emit_u8( code, &offset, 0x41 );
    emit_absolute_store( code, &offset, TEST_ENTERED_FLAG, 1 );
    emit_wait_for_stop_flag( code, &offset );
    emit_u8( code, &offset, 0xba );
    emit_u32( code, &offset, 0x33333333 );
    emit_u8( code, &offset, 0x83 );
    emit_u8( code, &offset, 0xc2 );
    emit_u8( code, &offset, 2 );
    emit_u8( code, &offset, 0xbe );
    emit_u32( code, &offset, 0x44444444 );
    emit_u8( code, &offset, 0xbf );
    emit_u32( code, &offset, 0x55555555 );
    emit_u8( code, &offset, 0xb8 );
    emit_u32( code, &offset, XTAJIT_GUEST_UNIX_BOP );
    emit_u8( code, &offset, 0xff );
    emit_u8( code, &offset, 0xe0 );
}

static void emit_unix_bop( unsigned char *code, size_t *offset )
{
    emit_u8( code, offset, 0xb8 );
    emit_u32( code, offset, XTAJIT_GUEST_UNIX_BOP );
    emit_u8( code, offset, 0xff );
    emit_u8( code, offset, 0xe0 );
}

static void build_tight_loop_code( unsigned char *code )
{
    size_t offset = 0;

    emit_absolute_store( code, &offset, TEST_ENTERED_FLAG, 1 );
    emit_wait_for_stop_flag( code, &offset );
    emit_unix_bop( code, &offset );
}

static void build_straight_line_code( unsigned char *code )
{
    size_t offset = 0, loop, i;

    emit_absolute_store( code, &offset, TEST_ENTERED_FLAG, 1 );
    loop = offset;
    for (i = 0; i < 3000; ++i) emit_u8( code, &offset, 0x90 );
    emit_stop_flag_compare( code, &offset );
    emit_jump_equal( code, &offset, loop );
    emit_unix_bop( code, &offset );
    check( offset <= TEST_PAGE, "straight-line test code exceeds one page\n" );
}

static void build_rep_loop_code( unsigned char *code )
{
    size_t offset = 0, loop;

    emit_absolute_store( code, &offset, TEST_ENTERED_FLAG, 1 );
    loop = offset;
    emit_u8( code, &offset, 0xbf );
    emit_u32( code, &offset, TEST_REP_BUFFER );
    emit_u8( code, &offset, 0xb9 );
    emit_u32( code, &offset, TEST_REP_SIZE );
    emit_u8( code, &offset, 0x31 );
    emit_u8( code, &offset, 0xc0 );
    emit_u8( code, &offset, 0xfc );
    emit_u8( code, &offset, 0xf3 );
    emit_u8( code, &offset, 0xaa );
    emit_stop_flag_compare( code, &offset );
    emit_jump_equal( code, &offset, loop );
    emit_unix_bop( code, &offset );
}

static void build_write_code( unsigned char *code, uint32_t target, uint32_t value )
{
    size_t offset = 0;

    emit_u8( code, &offset, 0xb8 );
    emit_u32( code, &offset, target );
    emit_u8( code, &offset, 0xc7 );
    emit_u8( code, &offset, 0x00 );
    emit_u32( code, &offset, value );
    emit_u8( code, &offset, 0xb8 );
    emit_u32( code, &offset, XTAJIT_GUEST_UNIX_BOP );
    emit_u8( code, &offset, 0xff );
    emit_u8( code, &offset, 0xe0 );
}

static void build_read_code( unsigned char *code, uint32_t source, uint32_t destination )
{
    size_t offset = 0;

    emit_u8( code, &offset, 0xa1 );
    emit_u32( code, &offset, source );
    emit_u8( code, &offset, 0xa3 );
    emit_u32( code, &offset, destination );
    emit_u8( code, &offset, 0xb8 );
    emit_u32( code, &offset, XTAJIT_GUEST_UNIX_BOP );
    emit_u8( code, &offset, 0xff );
    emit_u8( code, &offset, 0xe0 );
}

static NTSTATUS run_mixed_write( uint32_t target, uint32_t value,
                                 struct simulation *simulation )
{
    struct xtajit_memory_params flush = { .guest = TEST_CODE0, .size = TEST_PAGE };
    unsigned char *code = (unsigned char *)(uintptr_t)shadow_address( TEST_CODE0 );

    build_write_code( code, target, value );
    if (flush_instruction_cache( &flush )) return STATUS_UNSUCCESSFUL;
    initialize_simulation( simulation, TEST_CODE0, TEST_STACK0, TEST_TEB0 );
    return begin_simulation( &simulation->params );
}

static NTSTATUS run_mixed_read( uint32_t source, uint32_t destination,
                                struct simulation *simulation )
{
    struct xtajit_memory_params flush = { .guest = TEST_CODE0, .size = TEST_PAGE };
    unsigned char *code = (unsigned char *)(uintptr_t)shadow_address( TEST_CODE0 );

    build_read_code( code, source, destination );
    if (flush_instruction_cache( &flush )) return STATUS_UNSUCCESSFUL;
    initialize_simulation( simulation, TEST_CODE0, TEST_STACK0, TEST_TEB0 );
    return begin_simulation( &simulation->params );
}

static BOOL map_test_page( uint64_t guest, uint32_t protect )
{
    if (!map_shadow_pages( guest, TEST_PAGE )) return FALSE;
    if (publish_page( WINE_WOW64_MEMORY_MAP, guest, guest, MEM_COMMIT,
                      protect, TRUE ))
    {
        return FALSE;
    }
    return TRUE;
}

static BOOL map_test_range( uint64_t guest, uint64_t size, uint32_t protect )
{
    struct wine_wow64_memory_range_v1 range;

    if (!size || (size & (TEST_PAGE - 1)) || !map_shadow_pages( guest, size ))
        return FALSE;
    initialize_range( &range, guest, size, guest, MEM_COMMIT, protect, TRUE );
    return !publish_ranges( WINE_WOW64_MEMORY_MAP, &range, 1,
                            STATUS_SUCCESS, STATUS_SUCCESS );
}

static void test_concurrent_engines(void)
{
    struct simulation simulations[2];
    atomic_int start = 0;
    pthread_t threads[2];
    unsigned char *data, *code0, *code1;

    check( map_test_page( TEST_DATA, PAGE_READWRITE ) &&
           map_test_page( TEST_CODE0, PAGE_EXECUTE_READ ) &&
           map_test_page( TEST_CODE1, PAGE_EXECUTE_READ ) &&
           map_test_page( TEST_STACK0, PAGE_READWRITE ) &&
           map_test_page( TEST_STACK1, PAGE_READWRITE ) &&
           map_test_page( TEST_TEB0, PAGE_READWRITE ) &&
           map_test_page( TEST_TEB1, PAGE_READWRITE ),
           "concurrent engine mapping setup failed\n" );
    data = (unsigned char *)(uintptr_t)shadow_address( TEST_DATA );
    code0 = (unsigned char *)(uintptr_t)shadow_address( TEST_CODE0 );
    code1 = (unsigned char *)(uintptr_t)shadow_address( TEST_CODE1 );
    memset( data, 0, TEST_PAGE );
    build_peer_code( code0, 0, 1 );
    build_peer_code( code1, 1, 0 );
    initialize_simulation( &simulations[0], TEST_CODE0, TEST_STACK0, TEST_TEB0 );
    initialize_simulation( &simulations[1], TEST_CODE1, TEST_STACK1, TEST_TEB1 );
    simulations[0].start = &start;
    simulations[1].start = &start;
    pthread_create( &threads[0], NULL, run_simulation, &simulations[0] );
    pthread_create( &threads[1], NULL, run_simulation, &simulations[1] );
    check( wait_int( &simulations[0].ready, 1, 2000 ) &&
           wait_int( &simulations[1].ready, 1, 2000 ),
           "thread-owned engines did not initialize\n" );
    atomic_store_explicit( &start, 1, memory_order_release );
    check( wait_int( &simulations[0].done, 1, 5000 ) &&
           wait_int( &simulations[1].done, 1, 5000 ),
           "concurrent engines serialized or timed out\n" );
    pthread_join( threads[0], NULL );
    pthread_join( threads[1], NULL );
    check( !simulations[0].status && !simulations[1].status &&
           simulations[0].params.stop_reason == XTAJIT_STOP_UNIX_CALL &&
           simulations[1].params.stop_reason == XTAJIT_STOP_UNIX_CALL,
           "concurrent stop statuses %#x/%#x reasons %u/%u\n",
           (unsigned int)simulations[0].status, (unsigned int)simulations[1].status,
           simulations[0].params.stop_reason, simulations[1].params.stop_reason );

    memset( &simulations[0], 0, sizeof(simulations[0]) );
    initialize_simulation( &simulations[0], TEST_CODE0, TEST_STACK0, TEST_TEB0 );
    pthread_create( &threads[0], NULL, run_simulation, &simulations[0] );
    check( wait_int( &simulations[0].done, 1, 5000 ), "replacement engine timed out\n" );
    pthread_join( threads[0], NULL );
    check( !simulations[0].status &&
           simulations[0].params.stop_reason == XTAJIT_STOP_UNIX_CALL,
           "replacement engine did not clone the canonical registry\n" );
}

static void test_mixed_pages_and_duplicates(void)
{
    struct wine_wow64_memory_range_v1 ranges[4];
    struct xtajit_memory_params legacy;
    struct xtajit_fault_params fault;
    struct simulation simulation;
    struct holder holders[2] = {0};
    atomic_int release = 0;
    pthread_t threads[2];
    struct xtajit_engine *engine;
    const struct mapped_range *mapping;
    uint32_t *ordinary, *execute_only;
    size_t old_count;
    NTSTATUS status;
    unsigned int i;

    check( map_shadow_pages( TEST_MIXED, 4 * TEST_PAGE ) != NULL,
           "mixed host page mapping failed\n" );
    holders[0].release = &release;
    holders[1].release = &release;
    pthread_create( &threads[0], NULL, hold_engine, &holders[0] );
    pthread_create( &threads[1], NULL, hold_engine, &holders[1] );
    check( wait_int( &holders[0].ready, 1, 2000 ) && wait_int( &holders[1].ready, 1, 2000 ),
           "holder engines did not initialize\n" );
    initialize_range( &ranges[0], TEST_MIXED, TEST_PAGE, TEST_MIXED,
                      MEM_COMMIT, PAGE_READWRITE, TRUE );
    initialize_range( &ranges[1], TEST_MIXED + TEST_PAGE, TEST_PAGE, TEST_MIXED,
                      MEM_COMMIT, PAGE_EXECUTE_READWRITE, TRUE );
    ranges[1].flags |= WINE_WOW64_MEMORY_RANGE_LOGICAL_WRITE_FAULT;
    initialize_range( &ranges[2], TEST_MIXED + 2 * TEST_PAGE, TEST_PAGE, TEST_MIXED,
                      MEM_COMMIT, PAGE_READWRITE, TRUE );
    ranges[2].flags |= WINE_WOW64_MEMORY_RANGE_LOGICAL_WRITE_FAULT;
    initialize_range( &ranges[3], TEST_MIXED + 3 * TEST_PAGE, TEST_PAGE, TEST_MIXED,
                      MEM_COMMIT, PAGE_EXECUTE, TRUE );
    check( !publish_ranges( WINE_WOW64_MEMORY_MAP, ranges, 4,
                            STATUS_SUCCESS, STATUS_SUCCESS ),
           "mixed 4K observer publication failed\n" );

    pthread_mutex_lock( &provider.mutex );
    {
        static const unsigned int expected_perms[4] =
        {
            UC_PROT_READ | UC_PROT_WRITE,
            UC_PROT_READ | UC_PROT_EXEC,
            UC_PROT_READ,
            UC_PROT_READ | UC_PROT_EXEC,
        };

        for (engine = provider.engines; engine; engine = engine->next)
            for (i = 0; i < 4; ++i)
                check( engine_page_has_perms( engine, TEST_MIXED + i * TEST_PAGE,
                                              expected_perms[i] ),
                       "engine lost mixed protection page %u\n", i );
    }
    for (i = 0; i < 4; ++i)
    {
        mapping = find_registry_range( TEST_MIXED + i * TEST_PAGE );
        check( mapping && mapping->allocation_base == TEST_MIXED,
               "mixed page %u lost allocation base\n", i );
    }
    old_count = provider.ranges.count;
    pthread_mutex_unlock( &provider.mutex );

    status = thread_init( NULL );
    check( !status, "mixed-lane engine initialization failed %#x\n",
           (unsigned int)status );
    if (!status)
    {
        ordinary = (uint32_t *)(uintptr_t)shadow_address( TEST_MIXED );
        execute_only = (uint32_t *)(uintptr_t)shadow_address( TEST_MIXED + 3 * TEST_PAGE );
        *ordinary = 0;
        status = run_mixed_write( TEST_MIXED, 0x4f52444e, &simulation );
        check( !status && simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL &&
               *ordinary == 0x4f52444e,
               "ordinary mixed-lane write failed %#x/%u\n", (unsigned int)status,
               simulation.params.stop_reason );
        engine = pthread_getspecific( engine_key );
        check( engine &&
               engine_page_has_perms( engine, TEST_MIXED + TEST_PAGE,
                                      UC_PROT_READ | UC_PROT_EXEC ),
               "ordinary sibling write disarmed executable lane\n" );

        status = run_mixed_write( TEST_MIXED + TEST_PAGE, 0x45584543, &simulation );
        check( status == STATUS_ACCESS_VIOLATION &&
               simulation.params.stop_reason == XTAJIT_STOP_MEMORY_FAULT &&
               simulation.params.fault_address == TEST_MIXED + TEST_PAGE &&
               simulation.params.fault_type == UC_MEM_WRITE_PROT,
               "armed executable write did not fault %#x/%u/%#llx/%u\n",
               (unsigned int)status, simulation.params.stop_reason,
               (unsigned long long)simulation.params.fault_address,
               simulation.params.fault_type );
        test_memory_fault_resolver = resolve_test_fault;
        fault_test_case = FAULT_TEST_EXEC_WRITE;
        initialize_fault( &fault, TEST_MIXED + TEST_PAGE, UC_MEM_WRITE_PROT );
        status = resolve_memory_fault( &fault );
        check( !status && fault.result.action == WINE_WOW64_MEMORY_FAULT_RAISE &&
               fault.result.status == STATUS_IN_PAGE_ERROR &&
               fault.result.parameter_count == 3 &&
               (NTSTATUS)fault.result.information[2] == STATUS_EXECUTABLE_MEMORY_WRITE,
               "executable write exception path changed %#x/%u/%#x\n",
               (unsigned int)status, fault.result.action,
               (unsigned int)fault.result.status );
        check( engine_page_has_perms( engine, TEST_MIXED + TEST_PAGE,
                                      UC_PROT_READ | UC_PROT_EXEC ),
               "rejected executable write disarmed its logical lane\n" );

        status = run_mixed_write( TEST_MIXED + 2 * TEST_PAGE, 0x44495341,
                                  &simulation );
        check( status == STATUS_ACCESS_VIOLATION &&
               simulation.params.stop_reason == XTAJIT_STOP_MEMORY_FAULT,
               "armed data lane did not fault %#x/%u\n", (unsigned int)status,
               simulation.params.stop_reason );
        fault_test_case = FAULT_TEST_MIXED_DISARM;
        initialize_fault( &fault, TEST_MIXED + 2 * TEST_PAGE, UC_MEM_WRITE_PROT );
        status = resolve_memory_fault( &fault );
        check( !status && fault.result.action == WINE_WOW64_MEMORY_FAULT_RETRY,
               "logical data-lane disarm failed %#x/%u\n", (unsigned int)status,
               fault.result.action );
        check( engine_page_has_perms( engine, TEST_MIXED + 2 * TEST_PAGE,
                                      UC_PROT_READ | UC_PROT_WRITE ) &&
               engine_page_has_perms( engine, TEST_MIXED + TEST_PAGE,
                                      UC_PROT_READ | UC_PROT_EXEC ),
               "4K disarm changed an adjacent logical lane\n" );
        status = run_mixed_write( TEST_MIXED + 2 * TEST_PAGE, 0x44495341,
                                  &simulation );
        check( !status && simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL,
               "disarmed data-lane write did not retry %#x/%u\n",
               (unsigned int)status, simulation.params.stop_reason );

        *ordinary = 0;
        *execute_only = 0x58454352;
        status = run_mixed_read( TEST_MIXED + 3 * TEST_PAGE, TEST_MIXED,
                                 &simulation );
        check( !status && simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL &&
               *ordinary == *execute_only,
               "PAGE_EXECUTE data read failed %#x/%u/%#x\n",
               (unsigned int)status, simulation.params.stop_reason, *ordinary );
        test_memory_fault_resolver = NULL;
        check( !thread_term( NULL ), "mixed-lane engine cleanup failed\n" );
    }

    memset( &legacy, 0, sizeof(legacy) );
    legacy.guest = TEST_MIXED;
    legacy.host = shadow_address( TEST_MIXED );
    legacy.size = TEST_PAGE;
    legacy.allocation_base = TEST_MIXED;
    legacy.protect = PAGE_NOACCESS;
    check( !memory_map( &legacy ), "observer-active duplicate map validation failed\n" );
    check( !memory_protect( &legacy ), "observer-active duplicate protect validation failed\n" );
    check( !memory_unmap( &legacy ), "observer-active duplicate unmap validation failed\n" );
    pthread_mutex_lock( &provider.mutex );
    check( provider.ranges.count == old_count && !provider.poison_status,
           "duplicate structural callback mutated or poisoned the registry\n" );
    pthread_mutex_unlock( &provider.mutex );
    legacy.host++;
    check( memory_map( &legacy ) == STATUS_INVALID_PARAMETER,
           "malformed duplicate map was not rejected\n" );

    initialize_range( &ranges[0], TEST_MIXED + TEST_PAGE, TEST_PAGE,
                      TEST_MIXED, MEM_COMMIT, PAGE_NOACCESS, TRUE );
    check( !publish_ranges( WINE_WOW64_MEMORY_PROTECT, ranges, 1,
                            STATUS_SUCCESS, STATUS_SUCCESS ),
           "single logical-page protect failed\n" );
    pthread_mutex_lock( &provider.mutex );
    mapping = find_registry_range( TEST_MIXED + TEST_PAGE );
    check( mapping && mapping->perms == UC_PROT_NONE &&
           mapping->allocation_base == TEST_MIXED,
           "logical-page protect did not preserve canonical allocation metadata\n" );
    pthread_mutex_unlock( &provider.mutex );

    initialize_range( &ranges[0], TEST_MIXED + 2 * TEST_PAGE, TEST_PAGE,
                      0, MEM_FREE, PAGE_NOACCESS, FALSE );
    check( !publish_ranges( WINE_WOW64_MEMORY_UNMAP, ranges, 1,
                            STATUS_SUCCESS, STATUS_SUCCESS ),
           "logical-page unmap failed\n" );
    pthread_mutex_lock( &provider.mutex );
    check( !find_registry_range( TEST_MIXED + 2 * TEST_PAGE ),
           "unmapped page remained canonical\n" );
    pthread_mutex_unlock( &provider.mutex );
    memset( (void *)(uintptr_t)shadow_address( TEST_MIXED + 2 * TEST_PAGE ), 0x5a, TEST_PAGE );
    initialize_range( &ranges[0], TEST_MIXED + 2 * TEST_PAGE, TEST_PAGE,
                      TEST_MIXED + 2 * TEST_PAGE, MEM_COMMIT, PAGE_READWRITE, TRUE );
    check( !publish_ranges( WINE_WOW64_MEMORY_ALLOCATE, ranges, 1,
                            STATUS_SUCCESS, STATUS_SUCCESS ),
           "same-address allocation reuse failed\n" );
    pthread_mutex_lock( &provider.mutex );
    for (engine = provider.engines; engine; engine = engine->next)
        check( engine_page_has_perms( engine, TEST_MIXED + 2 * TEST_PAGE,
                                      UC_PROT_READ | UC_PROT_WRITE ),
               "reused page missing from an existing engine\n" );
    mapping = find_registry_range( TEST_MIXED + 2 * TEST_PAGE );
    check( mapping && mapping->allocation_base == TEST_MIXED + 2 * TEST_PAGE,
           "same-address reuse retained a stale allocation base\n" );
    pthread_mutex_unlock( &provider.mutex );

    atomic_store_explicit( &release, 1, memory_order_release );
    pthread_join( threads[0], NULL );
    pthread_join( threads[1], NULL );
}

static void test_authoritative_completion(void)
{
    struct wine_wow64_memory_range_v1 range;
    struct wine_wow64_memory_event_v1 event = {0};
    const struct mapped_range *mapping;
    void *transaction = NULL;
    size_t old_count;
    NTSTATUS status;

    check( map_shadow_pages( TEST_SELECTED, TEST_PAGE ) != NULL,
           "address-selected host page mapping failed\n" );
    initialize_range( &range, TEST_SELECTED, TEST_PAGE, TEST_SELECTED,
                      MEM_COMMIT, PAGE_READWRITE, TRUE );
    event.version = WINE_WOW64_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_ALLOCATE;
    event.status = STATUS_SUCCESS;
    event.snapshot_status = STATUS_SUCCESS;
    event.address = range.address;
    event.size_covered = range.size;
    event.allocation_base = range.allocation_base;
    event.ranges = &range;
    event.range_count = 1;

    status = memory_observer.begin( memory_observer.context,
                                    WINE_WOW64_MEMORY_ALLOCATE, 0,
                                    TEST_PAGE, 0, &transaction );
    check( !status && transaction,
           "address-selected observer begin failed %#x\n", (unsigned int)status );
    if (!status) memory_observer.complete( memory_observer.context, transaction, &event );
    pthread_mutex_lock( &provider.mutex );
    mapping = find_registry_range( TEST_SELECTED );
    check( !provider.poison_status && mapping &&
           mapping->allocation_base == TEST_SELECTED &&
           mapping->perms == (UC_PROT_READ | UC_PROT_WRITE),
           "authoritative address-selected completion was not published\n" );
    pthread_mutex_unlock( &provider.mutex );

    initialize_range( &range, TEST_SELECTED, TEST_PAGE, TEST_SELECTED,
                      MEM_COMMIT, PAGE_READONLY, TRUE );
    check( !publish_ranges( WINE_WOW64_MEMORY_PROTECT, &range, 1,
                            STATUS_ACCESS_DENIED, STATUS_SUCCESS ),
           "failed mutation with an authoritative snapshot was rejected\n" );
    pthread_mutex_lock( &provider.mutex );
    mapping = find_registry_range( TEST_SELECTED );
    check( mapping && mapping->perms == UC_PROT_READ,
           "failed mutation snapshot did not replace the final state\n" );
    old_count = provider.ranges.count;
    pthread_mutex_unlock( &provider.mutex );

    memset( &event, 0, sizeof(event) );
    event.version = WINE_WOW64_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_ALLOCATE;
    event.status = STATUS_NO_MEMORY;
    transaction = NULL;
    status = memory_observer.begin( memory_observer.context,
                                    WINE_WOW64_MEMORY_ALLOCATE, 0,
                                    TEST_PAGE, 0, &transaction );
    check( !status && transaction,
           "failed address-selected observer begin failed %#x\n", (unsigned int)status );
    if (!status) memory_observer.complete( memory_observer.context, transaction, &event );
    pthread_mutex_lock( &provider.mutex );
    check( !provider.poison_status && !provider.mutating &&
           provider.ranges.count == old_count,
           "zero-coverage failed mutation poison %#x mutating %u count %zu/%zu\n",
           (unsigned int)provider.poison_status, provider.mutating,
           provider.ranges.count, old_count );
    pthread_mutex_unlock( &provider.mutex );
}

static void *run_mutation( void *arg )
{
    struct mutation *mutation = arg;

    mutation->status = publish_ranges( mutation->operation, &mutation->range, 1,
                                       STATUS_SUCCESS, STATUS_SUCCESS );
    atomic_store_explicit( &mutation->done, 1, memory_order_release );
    return NULL;
}

static void test_running_mutation_gate(void)
{
    struct simulation simulation;
    struct mutation mutation = {0};
    atomic_int start = 1;
    pthread_t simulation_thread, mutation_thread;
    unsigned char *code;

    check( map_shadow_pages( TEST_MUTATION, 4 * TEST_PAGE ) != NULL &&
           !publish_page( WINE_WOW64_MEMORY_MAP, TEST_MUTATION, TEST_MUTATION,
                          MEM_COMMIT, PAGE_READWRITE, TRUE ),
           "mutation data mapping failed\n" );
    code = (unsigned char *)(uintptr_t)shadow_address( TEST_CODE1 );
    build_stop_code( code );
    initialize_simulation( &simulation, TEST_CODE1, TEST_STACK1, TEST_TEB1 );
    simulation.start = &start;
    atomic_store( &test_hold_progress_hook, 1 );
    atomic_store( &test_progress_hook_entered, 0 );
    atomic_store( &test_release_progress_hook, 0 );
    pthread_create( &simulation_thread, NULL, run_simulation, &simulation );
    check( wait_int( &test_progress_hook_entered, 1, 2000 ),
           "simulation did not enter the deterministic hold hook\n" );
    initialize_range( &mutation.range, TEST_MUTATION, TEST_PAGE,
                      TEST_MUTATION, MEM_COMMIT, PAGE_READONLY, TRUE );
    mutation.operation = WINE_WOW64_MEMORY_PROTECT;
    pthread_create( &mutation_thread, NULL, run_mutation, &mutation );
    check( !wait_int( &mutation.done, 1, 50 ),
           "exclusive mutation completed while an engine was executing\n" );
    atomic_store_explicit( &test_release_progress_hook, 1, memory_order_release );
    check( wait_int( &mutation.done, 1, 5000 ) && wait_int( &simulation.done, 1, 5000 ),
           "mutation barrier or resumed simulation timed out\n" );
    pthread_join( mutation_thread, NULL );
    pthread_join( simulation_thread, NULL );
    check( !mutation.status && !simulation.status &&
           simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL &&
           simulation.params.execution_slice_count >= 2,
           "running mutation statuses %#x/%#x reason %u slices %llu\n",
           (unsigned int)mutation.status, (unsigned int)simulation.status,
           simulation.params.stop_reason,
           (unsigned long long)simulation.params.execution_slice_count );
    atomic_store( &test_hold_progress_hook, 0 );
}

static void test_single_byte_cache_invalidation(void)
{
    unsigned char *code = (unsigned char *)(uintptr_t)shadow_address( TEST_CODE1 );
    struct xtajit_memory_params flush = { .guest = TEST_CODE1 + 1, .size = 1 };
    struct simulation simulation;
    NTSTATUS status;

    status = thread_init( NULL );
    check( !status, "single-byte cache test engine initialization failed %#x\n",
           (unsigned int)status );
    if (status) return;

    build_marker_code( code, 1 );
    initialize_simulation( &simulation, TEST_CODE1, TEST_STACK1, TEST_TEB1 );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL &&
           simulation.params.context.ecx == 1,
           "initial cache marker execution returned %#x reason %u marker %#x\n",
           (unsigned int)status, simulation.params.stop_reason,
           simulation.params.context.ecx );

    code[1] = 2;
    test_cache_remove_calls = 0;
    status = flush_instruction_cache( &flush );
    check( !status, "single-byte cache invalidation failed %#x\n",
           (unsigned int)status );
    check( test_cache_remove_calls == 1 &&
           test_cache_remove_start == TEST_CODE1 &&
           test_cache_remove_end == TEST_CODE1 + TEST_PAGE,
           "aligned single-byte cache range was [%#llx,%#llx) across %u calls, expected [%#x,%#x)\n",
           (unsigned long long)test_cache_remove_start,
           (unsigned long long)test_cache_remove_end, test_cache_remove_calls,
           TEST_CODE1, TEST_CODE1 + TEST_PAGE );

    test_cache_remove_calls = 0;
    initialize_simulation( &simulation, TEST_CODE1, TEST_STACK1, TEST_TEB1 );
    status = begin_simulation( &simulation.params );
    check( !status && simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL &&
           simulation.params.context.ecx == 2 &&
           simulation.params.execution_slice_count == 1,
           "single-byte cache invalidation retained stale code, status %#x reason %u marker %#x\n",
           (unsigned int)status, simulation.params.stop_reason,
           simulation.params.context.ecx );
    check( test_cache_remove_calls == 1 &&
           test_cache_remove_start == TEST_CODE1 &&
           test_cache_remove_end == TEST_CODE1 + 1,
           "cached entry hook invalidated [%#llx,%#llx) across %u calls\n",
           (unsigned long long)test_cache_remove_start,
           (unsigned long long)test_cache_remove_end, test_cache_remove_calls );
    check( !thread_term( NULL ), "single-byte cache test engine cleanup failed\n" );
}

static void test_cross_thread_pause_context(void)
{
    struct wine_wow64_memory_range_v1 range;
    struct simulation simulation;
    I386_CONTEXT converted_context = {0};
    unsigned char *code;
    uint64_t saved_xmm[2] = { 0x0123456789abcdefull, 0xfedcba9876543210ull };
    volatile uint32_t *stop_flag, *entered_flag;
    uint32_t *flat_data;
    uint32_t poison_status;
    pthread_t simulation_thread;
    NTSTATUS status;

    if (!map_test_page( TEST_PAUSE_CODE, PAGE_EXECUTE_READ ) ||
        !map_test_page( TEST_PAUSE_STACK, PAGE_READWRITE ) ||
        !map_test_page( TEST_PAUSE_TEB, PAGE_READWRITE ))
    {
        check( FALSE, "cross-thread pause mapping setup failed\n" );
        return;
    }
    code = (unsigned char *)(uintptr_t)shadow_address( TEST_PAUSE_CODE );
    flat_data = (uint32_t *)(uintptr_t)shadow_address( TEST_DATA + 0x100 );
    stop_flag = (volatile uint32_t *)(uintptr_t)shadow_address( TEST_STOP_FLAG );
    entered_flag = (volatile uint32_t *)(uintptr_t)shadow_address( TEST_ENTERED_FLAG );
    *flat_data = 0x77777777;
    *stop_flag = 0;
    *entered_flag = 0;
    build_pause_context_code( code );
    initialize_simulation( &simulation, TEST_PAUSE_CODE,
                           TEST_PAUSE_STACK, TEST_PAUSE_TEB );
    simulation.params.context.ebp = 0x66666666;
    simulation.params.context.seg_cs = 0x23;
    simulation.params.context.seg_ss = 0x2b;
    simulation.params.context.seg_ds = 0x33;
    simulation.params.context.seg_es = 0x3b;
    simulation.params.context.seg_fs = 0x53;
    simulation.params.context.seg_gs = 0x43;
    simulation.params.context.fp_valid = 1;
    simulation.params.context.fp_tag = 0xffff;
    memcpy( simulation.params.context.xmm[0], saved_xmm, sizeof(saved_xmm) );

    pthread_create( &simulation_thread, NULL, run_simulation, &simulation );
    check( wait_int( &simulation.ready, 1, 2000 ) &&
           wait_guest_u32( entered_flag, 1, 2000 ),
           "cross-thread pause guest did not enter its live loop\n" );
    status = process_term( NULL );
    check( status == STATUS_INVALID_DEVICE_STATE,
           "active emulation teardown returned %#x\n", (unsigned int)status );

    initialize_range( &range, TEST_PAUSE_TEB, TEST_PAGE,
                      TEST_PAUSE_TEB, MEM_COMMIT, PAGE_READONLY, TRUE );
    status = publish_ranges( WINE_WOW64_MEMORY_PROTECT, &range, 1,
                             STATUS_SUCCESS, STATUS_SUCCESS );
    check( !status, "cross-thread pause mutation failed %#x\n",
           (unsigned int)status );
    *stop_flag = 1;
    if (!wait_int( &simulation.done, 1, 5000 ))
    {
        check( FALSE, "cross-thread pause continuation timed out\n" );
        poison( NULL );
    }
    pthread_join( simulation_thread, NULL );

    check( !simulation.init_status && !simulation.status &&
           simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL,
           "cross-thread pause ended init %#x status %#x reason %u\n",
           (unsigned int)simulation.init_status, (unsigned int)simulation.status,
           simulation.params.stop_reason );
    check( simulation.params.execution_slice_count >= 2,
           "cross-thread pause did not resume in a new emulation slice\n" );
    check( simulation.params.context.ebx == 0x11111112 &&
           simulation.params.context.ecx == 0x22222223 &&
           simulation.params.context.edx == 0x33333335 &&
           simulation.params.context.esi == 0x44444444 &&
           simulation.params.context.edi == 0x55555555 &&
           simulation.params.context.ebp == 0x77777777 &&
           simulation.params.context.esp == TEST_PAUSE_STACK + TEST_PAGE - 16 &&
           !memcmp( simulation.params.context.xmm[0], saved_xmm, sizeof(saved_xmm) ),
           "cross-thread pause lost general, stack, or SIMD state\n" );
    xtajit_context_segments_from_unix( &converted_context,
                                        &simulation.params.context );
    check( converted_context.SegCs == 0x23 && converted_context.SegSs == 0x2b &&
           converted_context.SegDs == 0x33 && converted_context.SegEs == 0x3b &&
           converted_context.SegFs == 0x53 && converted_context.SegGs == 0x43,
           "final normal stop lost restored PE segment selector state\n" );
    pthread_mutex_lock( &provider.mutex );
    poison_status = provider.poison_status;
    check( provider.initialized,
           "rejected active-slice teardown disabled the provider\n" );
    pthread_mutex_unlock( &provider.mutex );
    check( !poison_status, "cross-thread pause poisoned provider %#x\n",
           poison_status );
}

static uint64_t run_pause_workload( const char *name, uint32_t eip,
                                    uint32_t mutation_protect, BOOL rep,
                                    BOOL verbose )
{
    struct wine_wow64_memory_range_v1 range;
    struct simulation simulation;
    struct timespec start, end, delay = {0, 1000000};
    volatile uint32_t *stop_flag, *entered_flag;
    pthread_t thread;
    NTSTATUS status;
    uint64_t elapsed;

    stop_flag = (volatile uint32_t *)(uintptr_t)shadow_address( TEST_STOP_FLAG );
    entered_flag = (volatile uint32_t *)(uintptr_t)shadow_address( TEST_ENTERED_FLAG );
    *stop_flag = 0;
    *entered_flag = 0;
    initialize_simulation( &simulation, eip, TEST_STRESS_STACK, TEST_STRESS_TEB );
    pthread_create( &thread, NULL, run_simulation, &simulation );
    check( wait_int( &simulation.ready, 1, 2000 ) &&
           wait_guest_u32( entered_flag, 1, 2000 ),
           "%s guest did not enter its workload\n", name );
    if (rep) nanosleep( &delay, NULL );

    initialize_range( &range, TEST_MUTATION, TEST_PAGE, TEST_MUTATION,
                      MEM_COMMIT, mutation_protect, TRUE );
    clock_gettime( CLOCK_MONOTONIC, &start );
    status = publish_ranges( WINE_WOW64_MEMORY_PROTECT, &range, 1,
                             STATUS_SUCCESS, STATUS_SUCCESS );
    clock_gettime( CLOCK_MONOTONIC, &end );
    elapsed = elapsed_microseconds( &start, &end );
    check( !status, "%s cross-thread stop failed %#x\n", name,
           (unsigned int)status );
    *stop_flag = 1;
    if (!wait_int( &simulation.done, 1, 10000 ))
    {
        check( FALSE, "%s did not resume and finish\n", name );
        poison( NULL );
    }
    pthread_join( thread, NULL );
    check( !simulation.init_status && !simulation.status &&
           simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL &&
           simulation.params.execution_slice_count >= 2,
           "%s returned init %#x status %#x reason %u slices %llu\n", name,
           (unsigned int)simulation.init_status, (unsigned int)simulation.status,
           simulation.params.stop_reason,
           (unsigned long long)simulation.params.execution_slice_count );
    if (verbose)
        printf( "xtajit %s mutation pause: %llu us\n", name,
                (unsigned long long)elapsed );
    return elapsed;
}

static void test_cross_thread_stop_workloads(void)
{
    unsigned char *code;

    if (!map_test_range( TEST_STRESS_CODE, 3 * TEST_PAGE, PAGE_EXECUTE_READ ) ||
        !map_test_page( TEST_STRESS_STACK, PAGE_READWRITE ) ||
        !map_test_page( TEST_STRESS_TEB, PAGE_READWRITE ) ||
        !map_test_range( TEST_REP_BUFFER, TEST_REP_SIZE, PAGE_READWRITE ))
    {
        check( FALSE, "cross-thread stop workload mapping setup failed\n" );
        return;
    }
    code = (unsigned char *)(uintptr_t)shadow_address( TEST_STRESS_CODE );
    build_tight_loop_code( code );
    build_straight_line_code( code + TEST_PAGE );
    build_rep_loop_code( code + 2 * TEST_PAGE );

    run_pause_workload( "tight-loop", TEST_STRESS_CODE,
                        PAGE_READWRITE, FALSE, TRUE );
    run_pause_workload( "straight-line", TEST_STRESS_CODE + TEST_PAGE,
                        PAGE_READONLY, FALSE, TRUE );
    run_pause_workload( "rep-stos", TEST_STRESS_CODE + 2 * TEST_PAGE,
                        PAGE_READWRITE, TRUE, TRUE );
}

static int compare_u64( const void *left, const void *right )
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a > b ? 1 : a < b ? -1 : 0;
}

static size_t percentile_index( size_t count, unsigned int percentile )
{
    return (count * percentile + 99) / 100 - 1;
}

static void report_pause_distribution( const char *name, uint64_t *samples,
                                       size_t count )
{
    qsort( samples, count, sizeof(*samples), compare_u64 );
    printf( "xtajit %s pause latency (%zu): p50 %llu us p95 %llu us "
            "p99 %llu us max %llu us\n", name, count,
            (unsigned long long)samples[percentile_index( count, 50 )],
            (unsigned long long)samples[percentile_index( count, 95 )],
            (unsigned long long)samples[percentile_index( count, 99 )],
            (unsigned long long)samples[count - 1] );
}

static void benchmark_pause_latencies(void)
{
    const char *value = getenv( "XTAJIT_PERF_ITERATIONS" );
    char *end;
    uint64_t *samples;
    unsigned long count;
    unsigned int protect = PAGE_READONLY;
    size_t i;

    if (!value || !*value) return;
    errno = 0;
    count = strtoul( value, &end, 10 );
    if (errno || *end || !count || count > 1000)
    {
        check( FALSE, "XTAJIT_PERF_ITERATIONS must be from 1 through 1000\n" );
        return;
    }
    if (!(samples = malloc( count * sizeof(*samples) )))
    {
        check( FALSE, "pause latency sample allocation failed\n" );
        return;
    }

    for (i = 0; i < count; ++i)
    {
        samples[i] = run_pause_workload( "tight-loop", TEST_STRESS_CODE,
                                         protect, FALSE, FALSE );
        protect = protect == PAGE_READONLY ? PAGE_READWRITE : PAGE_READONLY;
    }
    report_pause_distribution( "tight-loop", samples, count );
    for (i = 0; i < count; ++i)
    {
        samples[i] = run_pause_workload( "straight-line",
                                         TEST_STRESS_CODE + TEST_PAGE,
                                         protect, FALSE, FALSE );
        protect = protect == PAGE_READONLY ? PAGE_READWRITE : PAGE_READONLY;
    }
    report_pause_distribution( "straight-line", samples, count );
    for (i = 0; i < count; ++i)
    {
        samples[i] = run_pause_workload( "rep-stos",
                                         TEST_STRESS_CODE + 2 * TEST_PAGE,
                                         protect, TRUE, FALSE );
        protect = protect == PAGE_READONLY ? PAGE_READWRITE : PAGE_READONLY;
    }
    report_pause_distribution( "rep-stos", samples, count );
    free( samples );
}

static NTSTATUS reset_provider(void)
{
    NTSTATUS status = process_term( NULL );

    if (status) return status;
    process_params.enabled_capabilities = 0;
    return process_init( &process_params );
}

static BOOL execute_fault_retry( uint32_t target, uint32_t value )
{
    struct xtajit_memory_params flush =
    {
        .guest = TEST_FAULT_CODE,
        .size = TEST_PAGE,
    };
    struct simulation simulation;
    unsigned char *code = (unsigned char *)(uintptr_t)shadow_address( TEST_FAULT_CODE );
    uint32_t *destination = (uint32_t *)(uintptr_t)shadow_address( target );

    *destination = 0;
    build_write_code( code, target, value );
    if (flush_instruction_cache( &flush )) return FALSE;
    initialize_simulation( &simulation, TEST_FAULT_CODE,
                           TEST_FAULT_STACK, TEST_FAULT_TEB );
    if (begin_simulation( &simulation.params )) return FALSE;
    return simulation.params.stop_reason == XTAJIT_STOP_UNIX_CALL &&
           *destination == value;
}

static void initialize_fault( struct xtajit_fault_params *params,
                              uint32_t guest, uint32_t unicorn_type )
{
    memset( params, 0, sizeof(*params) );
    params->guest = guest;
    params->unicorn_type = unicorn_type;
    params->result.version = WINE_WOW64_MEMORY_FAULT_VERSION;
    params->result.size = sizeof(params->result);
}

static void test_fault_resolver_actions(void)
{
    struct wine_wow64_memory_range_v1 range;
    struct fault_worker workers[2] =
    {
        { .guest = TEST_CONCURRENT_FAULT0 },
        { .guest = TEST_CONCURRENT_FAULT1 },
    };
    struct xtajit_fault_params fault;
    struct xtajit_engine *engine;
    atomic_int start = 0;
    pthread_t threads[2];
    NTSTATUS status;

    check( map_test_page( TEST_FAULT_CODE, PAGE_EXECUTE_READ ) &&
           map_test_page( TEST_FAULT_STACK, PAGE_READWRITE ) &&
           map_test_page( TEST_FAULT_TEB, PAGE_READWRITE ) &&
           map_test_page( TEST_GUARD, PAGE_READWRITE | PAGE_GUARD ) &&
           map_test_page( TEST_WRITEWATCH, PAGE_READONLY ) &&
           map_test_page( TEST_PROTECTED, PAGE_READONLY ),
           "fault resolver committed mapping setup failed\n" );
    check( map_shadow_pages( TEST_STACK_GROW, 2 * TEST_PAGE ) != NULL &&
           map_shadow_pages( TEST_SEC_RESERVE, TEST_PAGE ) != NULL &&
           map_shadow_pages( TEST_CONCURRENT_FAULT0, TEST_PAGE ) != NULL &&
           map_shadow_pages( TEST_CONCURRENT_FAULT1, TEST_PAGE ) != NULL,
           "fault resolver reserve backing setup failed\n" );
    initialize_range( &range, TEST_STACK_GROW, 2 * TEST_PAGE,
                      TEST_STACK_GROW, MEM_RESERVE, 0, TRUE );
    check( !publish_ranges( WINE_WOW64_MEMORY_MAP, &range, 1,
                            STATUS_SUCCESS, STATUS_SUCCESS ),
           "stack reserve publication failed\n" );
    initialize_range( &range, TEST_SEC_RESERVE, TEST_PAGE,
                      TEST_SEC_RESERVE, MEM_RESERVE, 0, TRUE );
    check( !publish_ranges( WINE_WOW64_MEMORY_MAP, &range, 1,
                            STATUS_SUCCESS, STATUS_SUCCESS ),
           "SEC_RESERVE publication failed\n" );
    initialize_range( &range, TEST_CONCURRENT_FAULT0, TEST_PAGE,
                      TEST_CONCURRENT_FAULT0, MEM_RESERVE, 0, TRUE );
    check( !publish_ranges( WINE_WOW64_MEMORY_MAP, &range, 1,
                            STATUS_SUCCESS, STATUS_SUCCESS ),
           "first concurrent reserve publication failed\n" );
    initialize_range( &range, TEST_CONCURRENT_FAULT1, TEST_PAGE,
                      TEST_CONCURRENT_FAULT1, MEM_RESERVE, 0, TRUE );
    check( !publish_ranges( WINE_WOW64_MEMORY_MAP, &range, 1,
                            STATUS_SUCCESS, STATUS_SUCCESS ),
           "second concurrent reserve publication failed\n" );
    status = thread_init( NULL );
    check( !status, "fault resolver engine initialization failed %#x\n",
           (unsigned int)status );
    if (status) return;

    test_memory_fault_resolver = resolve_test_fault;
    fault_resolver_calls = 0;

    fault_test_case = FAULT_TEST_GUARD;
    initialize_fault( &fault, TEST_GUARD + 8, UC_MEM_WRITE_PROT );
    status = resolve_memory_fault( &fault );
    check( !status && fault.result.action == WINE_WOW64_MEMORY_FAULT_RAISE &&
           fault.result.status == STATUS_GUARD_PAGE_VIOLATION &&
           fault.result.parameter_count == 2 &&
           fault.result.information[1] == shadow_address( TEST_GUARD + 8 ),
           "guard resolution result invalid status %#x action %u exception %#x\n",
           (unsigned int)status, fault.result.action,
           (unsigned int)fault.result.status );

    fault_test_case = FAULT_TEST_IN_PAGE;
    initialize_fault( &fault, TEST_PROTECTED + 8, UC_MEM_WRITE_PROT );
    status = resolve_memory_fault( &fault );
    check( !status && fault.result.action == WINE_WOW64_MEMORY_FAULT_RAISE &&
           fault.result.status == STATUS_IN_PAGE_ERROR &&
           fault.result.parameter_count == 3 &&
           (NTSTATUS)fault.result.information[2] == STATUS_EXECUTABLE_MEMORY_WRITE,
           "executable write fault was not preserved as in-page error %#x/%u/%#x\n",
           (unsigned int)status, fault.result.action,
           (unsigned int)fault.result.status );
    check( execute_fault_retry( TEST_GUARD + 8, 0x47554152 ),
           "guard-cleared instruction retry failed\n" );

    fault_test_case = FAULT_TEST_STACK_GROW;
    initialize_fault( &fault, TEST_STACK_GROW + TEST_PAGE + 8,
                      UC_MEM_WRITE_UNMAPPED );
    status = resolve_memory_fault( &fault );
    check( !status && fault.result.action == WINE_WOW64_MEMORY_FAULT_RETRY,
           "stack-growth resolution did not request retry %#x/%u\n",
           (unsigned int)status, fault.result.action );
    check( execute_fault_retry( TEST_STACK_GROW + TEST_PAGE + 8, 0x5354414b ),
           "expanded stack-growth instruction retry failed\n" );

    fault_test_case = FAULT_TEST_WRITEWATCH;
    initialize_fault( &fault, TEST_WRITEWATCH + 8, UC_MEM_WRITE_PROT );
    status = resolve_memory_fault( &fault );
    check( !status && fault.result.action == WINE_WOW64_MEMORY_FAULT_RETRY &&
           execute_fault_retry( TEST_WRITEWATCH + 8, 0x57524954 ),
           "write-watch resolution/retry failed %#x/%u\n",
           (unsigned int)status, fault.result.action );

    fault_test_case = FAULT_TEST_PROTECTION;
    initialize_fault( &fault, TEST_PROTECTED + 8, UC_MEM_WRITE_PROT );
    status = resolve_memory_fault( &fault );
    engine = pthread_getspecific( engine_key );
    check( !status && fault.result.action == WINE_WOW64_MEMORY_FAULT_RAISE &&
           fault.result.status == STATUS_ACCESS_VIOLATION &&
           fault.result.parameter_count == 2 && engine &&
           engine_page_has_perms( engine, TEST_PROTECTED, UC_PROT_READ ),
           "ordinary protection fault was not preserved as AV %#x/%u/%#x\n",
           (unsigned int)status, fault.result.action,
           (unsigned int)fault.result.status );

    fault_test_case = FAULT_TEST_SEC_RESERVE;
    initialize_fault( &fault, TEST_SEC_RESERVE + 8, UC_MEM_WRITE_UNMAPPED );
    status = resolve_memory_fault( &fault );
    check( !status && fault.result.action == WINE_WOW64_MEMORY_FAULT_RETRY &&
           execute_fault_retry( TEST_SEC_RESERVE + 8, 0x53454352 ),
           "SEC_RESERVE refresh/retry failed %#x/%u\n",
           (unsigned int)status, fault.result.action );

    test_memory_fault_resolver = resolve_concurrent_test_fault;
    atomic_store( &concurrent_resolver_entries, 0 );
    atomic_store( &concurrent_transaction_held, 0 );
    atomic_store( &release_concurrent_transaction, 0 );
    workers[0].start = &start;
    workers[1].start = &start;
    pthread_create( &threads[0], NULL, run_fault_worker, &workers[0] );
    pthread_create( &threads[1], NULL, run_fault_worker, &workers[1] );
    check( wait_int( &workers[0].ready, 1, 2000 ) &&
           wait_int( &workers[1].ready, 1, 2000 ),
           "concurrent fault engines did not initialize\n" );
    atomic_store_explicit( &start, 1, memory_order_release );
    check( wait_int( &concurrent_transaction_held, 1, 2000 ),
           "first resolver did not hold its observer transaction\n" );
    check( wait_int( &concurrent_resolver_entries, 2, 2000 ),
           "second resolver did not enter while the first transaction was held\n" );
    atomic_store_explicit( &release_concurrent_transaction, 1, memory_order_release );
    check( wait_int( &workers[0].done, 1, 5000 ) &&
           wait_int( &workers[1].done, 1, 5000 ),
           "concurrent resolvers did not complete\n" );
    pthread_join( threads[0], NULL );
    pthread_join( threads[1], NULL );
    check( !workers[0].init_status && !workers[1].init_status &&
           !workers[0].status && !workers[1].status &&
           workers[0].action == WINE_WOW64_MEMORY_FAULT_RETRY &&
           workers[1].action == WINE_WOW64_MEMORY_FAULT_RETRY,
           "concurrent resolver statuses %#x/%#x actions %u/%u\n",
           (unsigned int)workers[0].status, (unsigned int)workers[1].status,
           workers[0].action, workers[1].action );
    test_memory_fault_resolver = resolve_test_fault;

    fault_test_case = FAULT_TEST_BRIDGE_FAILURE;
    initialize_fault( &fault, TEST_PROTECTED + 8, UC_MEM_READ_PROT );
    status = resolve_memory_fault( &fault );
    check( status == STATUS_NO_MEMORY && provider.poison_status == STATUS_NO_MEMORY,
           "resolver bridge failure did not poison %#x/%#x\n",
           (unsigned int)status, (unsigned int)provider.poison_status );
    check( fault_resolver_calls == 7,
           "unexpected resolver call count %u\n", fault_resolver_calls );

    test_memory_fault_resolver = NULL;
    check( !thread_term( NULL ), "fault resolver engine cleanup failed\n" );
    check( !reset_provider(), "provider reset after resolver failure failed\n" );
}

static NTSTATUS validate_single_observer_range(
    const struct wine_wow64_memory_range_v1 *range, unsigned int *perms )
{
    struct wine_wow64_memory_event_v1 event = {0};
    struct range_array snapshot = {0};
    uint64_t start, end;
    NTSTATUS status;

    event.version = WINE_WOW64_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_PROTECT;
    event.address = range->address;
    event.size_covered = range->size;
    event.allocation_base = range->allocation_base;
    event.ranges = range;
    event.range_count = 1;
    status = build_event_registry( &event, &start, &end, &snapshot );
    if (!status && perms)
        *perms = snapshot.count ? snapshot.data[0].perms : UC_PROT_NONE;
    range_array_free( &snapshot );
    return status;
}

static void test_observer_range_flag_validation(void)
{
    struct wine_wow64_memory_range_v1 range;
    unsigned int perms = UC_PROT_NONE;
    NTSTATUS status;

    initialize_range( &range, TEST_SELECTED, TEST_PAGE, TEST_SELECTED,
                      MEM_COMMIT, PAGE_READWRITE, TRUE );
    range.flags |= 0x80000000u;
    status = validate_single_observer_range( &range, NULL );
    check( status == STATUS_INVALID_PARAMETER,
           "unknown observer range flag was accepted %#x\n", (unsigned int)status );

    initialize_range( &range, TEST_SELECTED, TEST_PAGE, TEST_SELECTED,
                      MEM_RESERVE, 0, TRUE );
    range.flags |= WINE_WOW64_MEMORY_RANGE_LOGICAL_WRITE_FAULT;
    status = validate_single_observer_range( &range, NULL );
    check( status == STATUS_INVALID_PARAMETER,
           "reserved range accepted logical write-fault state %#x\n",
           (unsigned int)status );

    initialize_range( &range, TEST_SELECTED, TEST_PAGE, TEST_SELECTED,
                      MEM_COMMIT, PAGE_READWRITE, FALSE );
    range.flags = WINE_WOW64_MEMORY_RANGE_LOGICAL_WRITE_FAULT;
    status = validate_single_observer_range( &range, NULL );
    check( status == STATUS_INVALID_PARAMETER,
           "untranslated range accepted logical write-fault state %#x\n",
           (unsigned int)status );

    initialize_range( &range, TEST_SELECTED, TEST_PAGE, 0,
                      MEM_FREE, PAGE_NOACCESS, FALSE );
    range.flags = WINE_WOW64_MEMORY_RANGE_LOGICAL_WRITE_FAULT;
    status = validate_single_observer_range( &range, NULL );
    check( status == STATUS_INVALID_PARAMETER,
           "free range accepted logical write-fault state %#x\n",
           (unsigned int)status );

    initialize_range( &range, TEST_SELECTED, TEST_PAGE, TEST_SELECTED,
                      MEM_COMMIT, PAGE_READWRITE, TRUE );
    range.flags |= WINE_WOW64_MEMORY_RANGE_LOGICAL_WRITE_FAULT;
    status = validate_single_observer_range( &range, &perms );
    check( !status && perms == UC_PROT_READ,
           "logical write-fault range retained Unicorn write permission %#x/%u\n",
           (unsigned int)status, perms );

    initialize_range( &range, TEST_SELECTED, TEST_PAGE, TEST_SELECTED,
                      MEM_COMMIT, PAGE_EXECUTE, TRUE );
    status = validate_single_observer_range( &range, &perms );
    check( !status && perms == (UC_PROT_READ | UC_PROT_EXEC),
           "PAGE_EXECUTE did not retain Windows data-read semantics %#x/%u\n",
           (unsigned int)status, perms );
}

static void test_process_handshake_validation(void)
{
    struct xtajit_process_init_params invalid = process_params;
    NTSTATUS status;

    check( memory_observer.size == sizeof(memory_observer) &&
           memory_observer.capabilities ==
               WINE_WOW64_MEMORY_OBSERVER_CAP_LOGICAL_WRITE_FAULT,
           "observer did not advertise the mandatory logical-fault ABI\n" );

    invalid.version++;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER && !provider.initialized,
           "process ABI version mismatch did not fail closed %#x\n",
           (unsigned int)status );

    invalid = process_params;
    invalid.size--;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER && !provider.initialized,
           "short process ABI did not fail closed %#x\n", (unsigned int)status );

    invalid = process_params;
    invalid.required_capabilities = XTAJIT_PROCESS_CAP_MEMORY_OBSERVER;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER && !provider.initialized,
           "missing logical-write-fault capability did not fail closed %#x\n",
           (unsigned int)status );

    invalid = process_params;
    invalid.required_capabilities = XTAJIT_PROCESS_CAP_LOGICAL_WRITE_FAULT;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER && !provider.initialized,
           "missing observer capability did not fail closed %#x\n",
           (unsigned int)status );

    invalid = process_params;
    invalid.required_capabilities |= 1ull << 63;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER && !provider.initialized,
           "unknown required capability did not fail closed %#x\n",
           (unsigned int)status );

    invalid = process_params;
    invalid.highest_user_address = XTAJIT_GUEST_BOP_PAGE;
    status = process_init( &invalid );
    check( status == STATUS_INVALID_PARAMETER && !provider.initialized,
           "user range overlapping the private BOP page did not fail closed %#x\n",
           (unsigned int)status );
}

static void test_guest_unixlib_handle_gate(void)
{
    UINT64 first, last, newest, malformed;

    first = wine_unixlib_dispatch_handle( 0, 1 );
    last = wine_unixlib_dispatch_handle( WINE_UNIXLIB_DISPATCH_MAX_SLOTS - 1,
                                         WINE_UNIXLIB_DISPATCH_GENERATION_MASK );
    newest = wine_unixlib_dispatch_handle( 0, 2 );
    check( !xtajit_guest_unixlib_handle_is_allowed( 0 ),
           "null guest Unixlib handle passed the native-dispatch gate\n" );
    check( !xtajit_guest_unixlib_handle_is_allowed( 0x123456789abcdef0ull ),
           "raw guest Unixlib table address passed the native-dispatch gate\n" );
    check( xtajit_guest_unixlib_handle_is_allowed( first ),
           "first generation-one guest Unixlib handle failed the native-dispatch gate\n" );
    check( xtajit_guest_unixlib_handle_is_allowed( last ),
           "last slot/max-generation guest Unixlib handle failed the native-dispatch gate\n" );
    /* The provider validates only the immutable handle shape.  Once a slot is
     * reused, the native dispatcher owns rejecting the older generation
     * against its live slot state. */
    check( xtajit_guest_unixlib_handle_is_allowed( newest ),
           "well-formed replacement-generation handle failed the native-dispatch gate\n" );
    check( first != newest,
           "replacement generation did not change the guest Unixlib handle\n" );
    check( !xtajit_guest_unixlib_handle_is_allowed( WINE_UNIXLIB_DISPATCH_HANDLE_TAG ),
           "zero-generation/zero-slot guest Unixlib handle passed the native-dispatch gate\n" );
    malformed = WINE_UNIXLIB_DISPATCH_HANDLE_TAG | 1;
    check( !xtajit_guest_unixlib_handle_is_allowed( malformed ),
           "zero-generation guest Unixlib handle passed the native-dispatch gate\n" );
    malformed = WINE_UNIXLIB_DISPATCH_HANDLE_TAG |
                (1ull << WINE_UNIXLIB_DISPATCH_GENERATION_SHIFT) |
                (WINE_UNIXLIB_DISPATCH_MAX_SLOTS + 1);
    check( !xtajit_guest_unixlib_handle_is_allowed( malformed ),
           "out-of-range guest Unixlib slot passed the native-dispatch gate\n" );
    malformed = WINE_UNIXLIB_DISPATCH_HANDLE_TAG |
                (1ull << WINE_UNIXLIB_DISPATCH_GENERATION_SHIFT) |
                WINE_UNIXLIB_DISPATCH_SLOT_MASK;
    check( !xtajit_guest_unixlib_handle_is_allowed( malformed ),
           "extra masked guest Unixlib slot passed the native-dispatch gate\n" );
    check( !xtajit_guest_unixlib_handle_is_allowed(
               wine_unixlib_dispatch_handle( WINE_UNIXLIB_DISPATCH_MAX_SLOTS, 1 ) ),
           "out-of-range guest Unixlib handle passed the native-dispatch gate\n" );
    check( !xtajit_guest_unixlib_handle_is_allowed(
               wine_unixlib_dispatch_handle( 0,
                                              WINE_UNIXLIB_DISPATCH_GENERATION_MASK + 1 ) ),
           "overflow generation passed the native-dispatch gate\n" );
    check( !xtajit_guest_unixlib_handle_is_allowed( 0x123456789abcdef0ull ),
           "raw direct-map Unixlib handle passed the native-dispatch gate\n" );
}

static void test_private_page_boundaries(void)
{
    struct xtajit_memory_params unmap = { .guest = XTAJIT_GUEST_BOP_PAGE };
    struct xtajit_begin_params simulation = {0};
    NTSTATUS status;

    status = memory_unmap( &unmap );
    check( status == STATUS_INVALID_PARAMETER,
           "private BOP page accepted an allocation-base unmap %#x\n",
           (unsigned int)status );

    simulation.context.eip = XTAJIT_GUEST_UNIX_BOP;
    simulation.context.esp = TEST_STACK0;
    simulation.teb_guest = XTAJIT_GUEST_BOP_PAGE;
    status = begin_simulation( &simulation );
    check( status == STATUS_INVALID_PARAMETER,
           "private BOP page accepted a guest TEB base %#x\n",
           (unsigned int)status );
}

static void test_failure_paths(void)
{
    struct wine_wow64_memory_range_v1 range;
    struct wine_wow64_memory_event_v1 event = {0};
    struct holder holders[2] = {0};
    atomic_int release = 0;
    pthread_t threads[2];
    void *transaction = NULL, *nested = NULL;
    NTSTATUS status;

    holders[0].release = &release;
    holders[1].release = &release;
    pthread_create( &threads[0], NULL, hold_engine, &holders[0] );
    pthread_create( &threads[1], NULL, hold_engine, &holders[1] );
    check( wait_int( &holders[0].ready, 1, 2000 ) && wait_int( &holders[1].ready, 1, 2000 ),
           "failure-path engines did not initialize\n" );
    initialize_range( &range, TEST_MUTATION + TEST_PAGE, TEST_PAGE,
                      TEST_MUTATION + TEST_PAGE, MEM_COMMIT, PAGE_READWRITE, TRUE );
    test_fail_engine_mutation = 1;
    status = publish_ranges( WINE_WOW64_MEMORY_MAP, &range, 1,
                             STATUS_SUCCESS, STATUS_SUCCESS );
    test_fail_engine_mutation = -1;
    check( status == STATUS_UNSUCCESSFUL &&
           provider.poison_status == STATUS_UNSUCCESSFUL &&
           !find_registry_range( TEST_MUTATION + TEST_PAGE ),
           "partial multi-engine publication did not fail-stop before publish %#x/%#x\n",
           (unsigned int)status, (unsigned int)provider.poison_status );
    transaction = NULL;
    status = memory_observer.begin( memory_observer.context, WINE_WOW64_MEMORY_MAP,
                                    range.address, range.size, range.allocation_base,
                                    &transaction );
    check( status == STATUS_UNSUCCESSFUL && !transaction &&
           provider.poison_status == STATUS_UNSUCCESSFUL && !provider.mutating,
           "complete-time poison was not sticky at the next observer begin %#x/%#x\n",
           (unsigned int)status, (unsigned int)provider.poison_status );
    atomic_store_explicit( &release, 1, memory_order_release );
    pthread_join( threads[0], NULL );
    pthread_join( threads[1], NULL );
    check( !reset_provider(), "provider reset after partial failure failed\n" );

    initialize_range( &range, TEST_MUTATION + 2 * TEST_PAGE, TEST_PAGE,
                      0, MEM_FREE, PAGE_NOACCESS, FALSE );
    status = memory_observer.begin( memory_observer.context, WINE_WOW64_MEMORY_UNMAP,
                                    range.address, range.size, 0, &transaction );
    check( !status, "outer observer begin failed\n" );
    status = memory_observer.begin( memory_observer.context, WINE_WOW64_MEMORY_UNMAP,
                                    range.address, range.size, 0, &nested );
    check( status && !nested, "nested observer begin was not rejected\n" );
    event.version = WINE_WOW64_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_UNMAP;
    event.address = range.address;
    event.size_covered = range.size;
    event.ranges = &range;
    event.range_count = 1;
    memory_observer.complete( memory_observer.context, transaction, &event );
    check( provider.poison_status, "nested observer call did not leave provider poisoned\n" );
    check( !reset_provider(), "provider reset after nested begin failed\n" );

    transaction = NULL;
    status = memory_observer.begin( memory_observer.context, WINE_WOW64_MEMORY_UNMAP,
                                    range.address, range.size, 0, &transaction );
    check( !status, "snapshot-failure observer begin failed\n" );
    event.snapshot_status = STATUS_NO_MEMORY;
    memory_observer.complete( memory_observer.context, transaction, &event );
    check( provider.poison_status && !provider.mutating,
           "snapshot failure did not poison and release the gate\n" );
    check( !reset_provider(), "provider reset after snapshot failure failed\n" );

    memset( &event, 0, sizeof(event) );
    event.version = WINE_WOW64_MEMORY_OBSERVER_VERSION;
    event.size = sizeof(event);
    event.operation = WINE_WOW64_MEMORY_UNMAP;
    event.status = STATUS_ACCESS_DENIED;
    transaction = NULL;
    status = memory_observer.begin( memory_observer.context, event.operation,
                                    range.address, range.size, 0, &transaction );
    check( !status, "fixed failed-mutation observer begin failed\n" );
    if (!status) memory_observer.complete( memory_observer.context, transaction, &event );
    check( provider.poison_status && !provider.mutating,
           "fixed mutation accepted a zero-coverage failure snapshot\n" );
}

int main(void)
{
    NTSTATUS status;
    void *shadow;

    shadow = mmap( NULL, WINE_LOW_VA_SHADOW_SIZE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON, -1, 0 );
    check( shadow != MAP_FAILED, "test shadow reservation failed\n" );
    if (shadow == MAP_FAILED) return 1;
    test_shadow_base = (uintptr_t)shadow;
    kuser_page = map_shadow_pages( XTAJIT_GUEST_KUSER, TEST_PAGE );
    check( kuser_page != NULL, "KUSER backing mapping failed\n" );
    if (!kuser_page) return 1;
    memset( &process_params, 0, sizeof(process_params) );
    process_params.version = XTAJIT_PROCESS_ABI_VERSION;
    process_params.size = sizeof(process_params);
    process_params.required_capabilities = XTAJIT_PROCESS_REQUIRED_CAPABILITIES;
    process_params.highest_user_address = TEST_HIGHEST;
    process_params.guest_kuser = XTAJIT_GUEST_KUSER;
    process_params.host_kuser = shadow_address( XTAJIT_GUEST_KUSER );
    process_params.kuser_size = TEST_PAGE;
    process_params.low_va_shadow_base = test_shadow_base;
    process_params.low_va_shadow_size = WINE_LOW_VA_SHADOW_SIZE;
    test_guest_unixlib_handle_gate();
    test_process_handshake_validation();
    status = process_init( &process_params );
    check( !status && provider.observer_active &&
           (process_params.enabled_capabilities & process_params.required_capabilities) ==
           process_params.required_capabilities,
           "provider handshake failed status %#x capabilities %#llx\n",
           (unsigned int)status,
           (unsigned long long)process_params.enabled_capabilities );
    if (status) return 1;

    test_private_page_boundaries();
    test_observer_range_flag_validation();
    test_concurrent_engines();
    test_mixed_pages_and_duplicates();
    test_authoritative_completion();
    test_running_mutation_gate();
    test_single_byte_cache_invalidation();
    test_cross_thread_pause_context();
    test_cross_thread_stop_workloads();
    benchmark_pause_latencies();
    test_fault_resolver_actions();
    test_failure_paths();

    process_term( NULL );
    munmap( shadow, WINE_LOW_VA_SHADOW_SIZE );

    if (failures)
    {
        fprintf( stderr, "%u xtajit provider test failure(s)\n", failures );
        return 1;
    }
    puts( "xtajit observer/concurrency tests passed" );
    return 0;
}
