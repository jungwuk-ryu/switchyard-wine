/*
 * Unicorn-backed x86-64 emulation on ARM64
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef XTAJIT64_UNIXLIB_TEST
# include <sched.h>
#endif

#ifdef HAVE_UNICORN
# include <pthread.h>
# include <unistd.h>
# include <unicorn/unicorn.h>
# include <unicorn/x86.h>
# ifdef __APPLE__
#  include <mach/mach.h>
#  include <mach/mach_vm.h>
# endif
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "wine/debug.h"
#include "unixlib.h"

#ifdef HAVE_UNICORN

#if !defined(UC_SWITCHYARD_INSTRUCTION_BOUNDARY_STOP) || \
    !defined(UC_SWITCHYARD_SHARED_MEMORY_ATOMICS) || \
    !defined(UC_SWITCHYARD_SHARED_CODE_COHERENCE)
# error Switchyard Unicorn instruction-boundary stop, shared-memory atomics, and shared-code coherence are required
#endif

WINE_DEFAULT_DEBUG_CHANNEL(xtajit);
#ifndef XTAJIT64_UNIXLIB_TEST
WINE_DECLARE_DEBUG_CHANNEL(xtajitmap);
#endif

#define XTAJIT64_MAX_RESYNC_RANGES (1u << 20)
#define XTAJIT64_MAX_SYSCALL_COUNT  (1u << 16)
#define XTAJIT64_TEB_SELF_END 0x38

/* Provider-private address domains remain internal until a coordinated,
 * capability-negotiated low-memory observer publishes the translated lane
 * through the foundation contract. */
enum xtajit64_memory_domain
{
    XTAJIT64_MEMORY_ADDRESS_INVALID,
    XTAJIT64_MEMORY_ADDRESS_IDENTITY,
    XTAJIT64_MEMORY_ADDRESS_AMD64_LOW,
};

struct mapped_range
{
    uint64_t guest;
    uint64_t host;
    uint64_t size;
    uint64_t allocation_base;
    unsigned int perms;
    unsigned int state;
    unsigned int domain;
    unsigned int flags;
    BOOL permanent;
    /* An engine can miss a complete unmap/remap cycle while it is idle. */
    BOOL stale;
};

struct range_array
{
    struct mapped_range *data;
    size_t count;
    size_t capacity;
};

struct thread_binding
{
    uc_context *context;
    uint64_t process_instance;
    uint64_t id;
    BOOL context_valid;
    BOOL active;
};

struct thread_engine
{
    struct thread_engine *next;
    uc_engine *uc;
    /* Actual Unicorn mappings, not a copy of the canonical process registry.
     * Keeping untouched canonical ranges absent lets each pooled engine fault
     * in only the guest regions reached by concurrent translated execution. */
    struct range_array mapped_ranges;
    uint64_t mapping_generation;
    uint64_t resident_binding_id;
    uint64_t diagnostic_id;
    unsigned int diagnostic_pool_size;
    unsigned int diagnostic_pool_in_use;
    unsigned int diagnostic_pool_high_water;
    uint64_t demand_map_calls;
    uint64_t demand_map_bytes;
    uint64_t demand_map_4k_calls;
    uint64_t demand_map_16k_calls;
    uint64_t demand_map_64k_calls;
    uint64_t demand_map_1m_calls;
    uint64_t demand_map_large_calls;
    uint64_t demand_map_max_size;
    uint64_t registry_sync_calls;
    uint64_t resync_unmap_calls;
    uint64_t resync_unmap_bytes;
    pthread_t owner;
    BOOL linked;
    BOOL in_use;
    BOOL running;
    atomic_bool pause_requested;
    volatile uint32_t *suspend_doorbell;
    uint64_t stack_limit;
    uint64_t stack_base;
    uint64_t transition_target;
    uint64_t fault_address;
    uint32_t fault_access;
    uc_err mapping_error;
    enum xtajit64_stop_reason stop_reason;
};

enum mutation_kind
{
    MUTATION_NONE,
    MUTATION_MAP,
    MUTATION_UNMAP,
    MUTATION_PROTECT,
    MUTATION_RESYNC,
    MUTATION_FLUSH,
    MUTATION_POISON,
};

enum mutation_stage
{
    MUTATION_STAGE_IDLE,
    MUTATION_STAGE_PAUSE,
    MUTATION_STAGE_WAIT,
    MUTATION_STAGE_PREPARE,
    MUTATION_STAGE_APPLY,
    MUTATION_STAGE_PUBLISH,
};

struct arm64ec_low_observer_transaction
{
    uint64_t generation;
    uint32_t operation;
    uint32_t reserved;
};

struct provider_process
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    BOOL initialized;
    BOOL mutating;
    BOOL mutation_owner_valid;
    BOOL shutting_down;
    BOOL observer_active;
    uint64_t generation;
    pthread_t mutation_owner;
    enum mutation_kind mutation_kind;
    enum mutation_stage mutation_stage;
    enum mutation_kind last_fault_kind;
    enum mutation_stage last_fault_stage;
    uint64_t last_fault_generation;
    NTSTATUS poison_status;
    const uint64_t *ec_bitmap;
    uint64_t ec_page_size;
    uint64_t highest_user_address;
    uint64_t rtl_exit_user_thread;
    uint64_t x64_syscall_dispatcher;
    uint32_t x64_syscall_count;
    uint64_t guest_kuser;
    uint64_t host_kuser;
    uint64_t kuser_size;
    uint64_t instance;
    uint64_t next_binding_id;
    uint64_t next_diagnostic_id;
    unsigned int engine_count;
    unsigned int engines_in_use;
    unsigned int engine_high_water;
    uc_context *initial_context;
    struct range_array ranges;
    struct thread_engine *engines;
    struct arm64ec_low_observer_transaction *observer_transaction;
};

static struct provider_process provider =
{
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};
static pthread_key_t engine_key;
static pthread_once_t engine_key_once = PTHREAD_ONCE_INIT;
static int engine_key_error;

#define XTAJIT64_DEMAND_MAP_MAX_SIZE \
    ((uint64_t)4096 * XTAJIT64_GUEST_PAGE_SIZE)

static NTSTATUS memory_map( void *args );
static NTSTATUS memory_map_internal( void *args );
static BOOL legacy_mutation_selects_low_locked( uint64_t guest, uint64_t size );
static NTSTATUS process_term( void *args );

C_ASSERT( sizeof(struct wine_arm64ec_low_memory_range_v1) == 40 );
C_ASSERT( sizeof(struct wine_arm64ec_low_memory_event_v1) == 72 );
C_ASSERT( sizeof(struct wine_arm64ec_low_memory_observer_v1) == 40 );

#ifdef XTAJIT64_UNIXLIB_TEST
static int test_fail_flush_interval_append = -1;
static int test_flush_interval_append_count;
enum test_mutation_fault_point
{
    TEST_MUTATION_FAULT_NONE,
    TEST_MUTATION_FAULT_AFTER_BEGIN,
    TEST_MUTATION_FAULT_ENGINE_PAUSE,
};
static atomic_int test_mutation_fault_point;
static atomic_int test_mutation_fault_entered;
static atomic_int test_mutation_fault_release;
static atomic_int test_mutation_waiters;
static atomic_int test_hold_ec_hook;
static atomic_int test_ec_hook_entered;
static atomic_int test_release_ec_hook;
static atomic_int test_hold_non_ec_hook;
static atomic_int test_non_ec_hook_entered;
static atomic_int test_release_non_ec_hook;
static atomic_int test_pause_stop_count;
static atomic_int test_pause_stop_owner_violation;
static atomic_int test_emu_start_count;
static atomic_int test_context_write_count;
static atomic_int test_context_read_count;
static struct thread_engine *test_last_acquired_engine;
static atomic_int test_check_context_read_lock;
static atomic_int test_context_read_lock_violation;
#endif

static const int integer_regs[] =
{
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_RSP,
    UC_X86_REG_R8,  UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
    UC_X86_REG_RIP, UC_X86_REG_EFLAGS,
};

static const int xmm_regs[] =
{
    UC_X86_REG_XMM0,  UC_X86_REG_XMM1,  UC_X86_REG_XMM2,  UC_X86_REG_XMM3,
    UC_X86_REG_XMM4,  UC_X86_REG_XMM5,  UC_X86_REG_XMM6,  UC_X86_REG_XMM7,
    UC_X86_REG_XMM8,  UC_X86_REG_XMM9,  UC_X86_REG_XMM10, UC_X86_REG_XMM11,
    UC_X86_REG_XMM12, UC_X86_REG_XMM13, UC_X86_REG_XMM14, UC_X86_REG_XMM15,
};

static uint64_t align_down( uint64_t value )
{
    return value & ~(uint64_t)(XTAJIT64_GUEST_PAGE_SIZE - 1);
}

static uint64_t align_up( uint64_t value )
{
    return (value + XTAJIT64_GUEST_PAGE_SIZE - 1) &
           ~(uint64_t)(XTAJIT64_GUEST_PAGE_SIZE - 1);
}

static BOOL align_range( uint64_t address, uint64_t size, uint64_t *start, uint64_t *end )
{
    uint64_t limit;

    if (!size || address > UINT64_MAX - size) return FALSE;
    limit = address + size;
    if (limit > UINT64_MAX - (XTAJIT64_GUEST_PAGE_SIZE - 1)) return FALSE;
    *start = align_down( address );
    *end = align_up( limit );
    return *start < *end;
}

static unsigned int protection_to_unicorn( unsigned int protect )
{
    /* Guest guard handling is not bridged to Wine's one-shot exception path;
     * never turn a guarded logical page into directly accessible backing. */
    if (protect & PAGE_GUARD) return UC_PROT_NONE;
    switch (protect & 0xff)
    {
    case PAGE_READONLY:
        return UC_PROT_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        return UC_PROT_READ | UC_PROT_WRITE;
    case PAGE_EXECUTE:
        /* Windows permits data reads from execute-only pages. */
        return UC_PROT_READ | UC_PROT_EXEC;
    case PAGE_EXECUTE_READ:
        return UC_PROT_READ | UC_PROT_EXEC;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC;
    default:
        return UC_PROT_NONE;
    }
}

static BOOL range_array_reserve( struct range_array *array, size_t capacity )
{
    struct mapped_range *data;

    if (capacity <= array->capacity) return TRUE;
    if (capacity > SIZE_MAX / sizeof(*data)) return FALSE;
    if (!(data = realloc( array->data, capacity * sizeof(*data) ))) return FALSE;
    array->data = data;
    array->capacity = capacity;
    return TRUE;
}

static BOOL ranges_can_merge( const struct mapped_range *left,
                              const struct mapped_range *right )
{
    return !left->permanent && !right->permanent &&
           left->guest + left->size == right->guest &&
           left->host + left->size == right->host &&
           left->allocation_base == right->allocation_base &&
           left->perms == right->perms && left->state == right->state &&
           left->domain == right->domain && left->flags == right->flags &&
           left->stale == right->stale;
}

static BOOL range_array_append( struct range_array *array, const struct mapped_range *range )
{
    struct mapped_range *last;

    if (!range->size) return TRUE;
    if (array->count)
    {
        last = &array->data[array->count - 1];
        if (last->guest + last->size > range->guest) return FALSE;
        if (ranges_can_merge( last, range ))
        {
            if (last->size > UINT64_MAX - range->size) return FALSE;
            last->size += range->size;
            return TRUE;
        }
    }
    if (array->count == SIZE_MAX ||
        !range_array_reserve( array, array->count + 1 )) return FALSE;
    array->data[array->count++] = *range;
    return TRUE;
}

static struct mapped_range range_slice( const struct mapped_range *range,
                                        uint64_t start, uint64_t end )
{
    struct mapped_range result = *range;

    result.host += start - range->guest;
    result.guest = start;
    result.size = end - start;
    return result;
}

static BOOL range_overlaps( const struct mapped_range *range, uint64_t start, uint64_t end )
{
    return range->guest < end && start < range->guest + range->size;
}

static BOOL registry_covers_range( const struct range_array *ranges,
                                   uint64_t start, uint64_t end )
{
    uint64_t cursor = start;
    size_t i;

    for (i = 0; i < ranges->count && cursor < end; ++i)
    {
        const struct mapped_range *range = &ranges->data[i];
        uint64_t range_end = range->guest + range->size;

        if (range_end <= cursor || range->state != MEM_COMMIT) continue;
        if (range->guest > cursor) return FALSE;
        cursor = min( range_end, end );
    }
    return cursor == end;
}

static BOOL registry_covers_readable_range( const struct range_array *ranges,
                                            uint64_t start, uint64_t end )
{
    uint64_t cursor = start;
    size_t i, left = 0, right = ranges->count;

    while (left < right)
    {
        size_t mid = left + (right - left) / 2;
        const struct mapped_range *range = &ranges->data[mid];

        if (range->guest + range->size <= start) left = mid + 1;
        else right = mid;
    }

    for (i = left; i < ranges->count && cursor < end; ++i)
    {
        const struct mapped_range *range = &ranges->data[i];
        uint64_t range_end = range->guest + range->size;

        if (range_end <= cursor || range->state != MEM_COMMIT) continue;
        if (range->guest > cursor || !(range->perms & UC_PROT_READ)) return FALSE;
        cursor = min( range_end, end );
    }
    return cursor == end;
}

static unsigned int translation_required_perms( unsigned int flags )
{
    unsigned int perms = 0;

    if (flags & XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ) perms |= UC_PROT_READ;
    if (flags & XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE) perms |= UC_PROT_WRITE;
    if (flags & XTAJIT64_MEMORY_TRANSLATE_REQUIRE_EXECUTE) perms |= UC_PROT_EXEC;
    return perms;
}

static BOOL translate_guest_range_locked( uint64_t guest, uint64_t size,
                                          unsigned int required_perms,
                                          uint64_t *host,
                                          uint64_t *allocation_base,
                                          unsigned int *domain )
{
    const struct mapped_range *range;
    uint64_t cursor, end, host_cursor, host_start, allocation;
    size_t i, left = 0, right = provider.ranges.count;

    if (!guest || !size || guest > UINT64_MAX - size ||
        guest + size - 1 > provider.highest_user_address)
        return FALSE;
    end = guest + size;
    while (left < right)
    {
        size_t mid = left + (right - left) / 2;

        range = &provider.ranges.data[mid];
        if (range->guest + range->size <= guest) left = mid + 1;
        else right = mid;
    }
    if (left == provider.ranges.count) return FALSE;
    range = &provider.ranges.data[left];
    if (range->guest > guest || range->host > UINT64_MAX - (guest - range->guest))
        return FALSE;
    host_start = range->host + guest - range->guest;
    host_cursor = host_start;
    allocation = range->allocation_base;
    cursor = guest;

    for (i = left; i < provider.ranges.count && cursor < end; ++i)
    {
        uint64_t range_end, next, offset, chunk;

        range = &provider.ranges.data[i];
        range_end = range->guest + range->size;
        if (range_end <= cursor) continue;
        if (range->state != MEM_COMMIT || range->guest > cursor ||
            range->allocation_base != allocation ||
            range->domain != provider.ranges.data[left].domain ||
            (range->perms & required_perms) != required_perms)
            return FALSE;
        offset = cursor - range->guest;
        if (range->host > UINT64_MAX - offset || range->host + offset != host_cursor)
            return FALSE;
        next = min( range_end, end );
        chunk = next - cursor;
        if (host_cursor > UINT64_MAX - chunk) return FALSE;
        host_cursor += chunk;
        cursor = next;
    }
    if (cursor != end) return FALSE;
    *host = host_start;
    *allocation_base = allocation;
    if (domain) *domain = provider.ranges.data[left].domain;
    return TRUE;
}

static NTSTATUS translate_host_range_locked( uint64_t address, uint64_t size,
                                              unsigned int required_perms,
                                              uint64_t *guest,
                                              uint64_t *allocation_base,
                                              unsigned int *domain )
{
    NTSTATUS status = STATUS_INVALID_ADDRESS;
    uint64_t found_guest = 0, found_allocation = 0;
    unsigned int found_domain = XTAJIT64_MEMORY_ADDRESS_INVALID;
    size_t i;

    for (i = 0; i < provider.ranges.count; ++i)
    {
        const struct mapped_range *range = &provider.ranges.data[i];
        uint64_t range_end, candidate_guest, candidate_host, candidate_allocation;
        unsigned int candidate_domain;

        if (range->state != MEM_COMMIT || range->host > UINT64_MAX - range->size) continue;
        range_end = range->host + range->size;
        if (address < range->host || address >= range_end) continue;
        if (range->guest > UINT64_MAX - (address - range->host)) continue;
        candidate_guest = range->guest + address - range->host;
        if (!translate_guest_range_locked( candidate_guest, size, required_perms,
                                           &candidate_host, &candidate_allocation,
                                           &candidate_domain ) ||
            candidate_host != address)
            continue;
        if (!status && candidate_guest != found_guest)
            return STATUS_OBJECT_NAME_COLLISION;
        found_guest = candidate_guest;
        found_allocation = candidate_allocation;
        found_domain = candidate_domain;
        status = STATUS_SUCCESS;
    }
    if (!status)
    {
        *guest = found_guest;
        *allocation_base = found_allocation;
        if (domain) *domain = found_domain;
    }
    return status;
}

static NTSTATUS memory_translate( void *args )
{
    struct xtajit64_memory_translate_params *params = args;
    uint64_t address, size, guest = 0, host = 0, allocation_base = 0;
    unsigned int direction, required_perms;
    NTSTATUS status = STATUS_INVALID_ADDRESS;

    if (!params || params->domain ||
        (params->flags & ~XTAJIT64_MEMORY_TRANSLATE_VALID_FLAGS))
        return STATUS_INVALID_PARAMETER;
    direction = params->flags & XTAJIT64_MEMORY_TRANSLATE_DIRECTION_MASK;
    if (direction != XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST &&
        direction != XTAJIT64_MEMORY_TRANSLATE_HOST_TO_GUEST)
        return STATUS_INVALID_PARAMETER;
    address = params->address;
    size = params->size;
    if (!address || !size || address > UINT64_MAX - size)
        return STATUS_INVALID_PARAMETER;
    required_perms = translation_required_perms( params->flags );
    params->guest = params->host = params->allocation_base = 0;

    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down) status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else if (direction == XTAJIT64_MEMORY_TRANSLATE_GUEST_TO_HOST)
    {
        guest = address;
        if (translate_guest_range_locked( guest, size, required_perms,
                                          &host, &allocation_base,
                                          &params->domain ))
            status = STATUS_SUCCESS;
    }
    else if (!(status = translate_host_range_locked( address, size, required_perms,
                                                     &guest, &allocation_base,
                                                     &params->domain )))
        host = address;
    pthread_mutex_unlock( &provider.mutex );

    if (!status)
    {
        params->guest = guest;
        params->host = host;
        params->allocation_base = allocation_base;
    }
    return status;
}

static void range_array_free( struct range_array *array )
{
    free( array->data );
    memset( array, 0, sizeof(*array) );
}

static NTSTATUS build_mapped_registry( const struct range_array *old,
                                       const struct mapped_range *mapping,
                                       struct range_array *result )
{
    size_t i;
    BOOL inserted = FALSE;
    uint64_t start = mapping->guest, end = start + mapping->size;

    if (old->count > SIZE_MAX - 3 ||
        !range_array_reserve( result, old->count + 3 )) return STATUS_NO_MEMORY;
    for (i = 0; i < old->count; ++i)
    {
        const struct mapped_range *range = &old->data[i];
        uint64_t range_end = range->guest + range->size;
        struct mapped_range slice;

        if (range_end <= start)
        {
            if (!range_array_append( result, range )) return STATUS_INVALID_ADDRESS;
            continue;
        }
        if (range->guest >= end)
        {
            if (!inserted)
            {
                if (!range_array_append( result, mapping )) return STATUS_INVALID_ADDRESS;
                inserted = TRUE;
            }
            if (!range_array_append( result, range )) return STATUS_INVALID_ADDRESS;
            continue;
        }
        if (range->permanent) return STATUS_ACCESS_DENIED;
        if (range->guest < start)
        {
            slice = range_slice( range, range->guest, start );
            if (!range_array_append( result, &slice )) return STATUS_INVALID_ADDRESS;
        }
        if (!inserted)
        {
            if (!range_array_append( result, mapping )) return STATUS_INVALID_ADDRESS;
            inserted = TRUE;
        }
        if (range_end > end)
        {
            slice = range_slice( range, end, range_end );
            if (!range_array_append( result, &slice )) return STATUS_INVALID_ADDRESS;
        }
    }
    if (!inserted && !range_array_append( result, mapping )) return STATUS_INVALID_ADDRESS;
    return STATUS_SUCCESS;
}

static NTSTATUS build_unmapped_registry( const struct range_array *old, uint64_t start,
                                         uint64_t size, uint64_t allocation_base,
                                         struct range_array *result )
{
    size_t i;
    uint64_t end = size ? start + size : 0;

    if (old->count == SIZE_MAX ||
        !range_array_reserve( result, old->count + 1 )) return STATUS_NO_MEMORY;
    for (i = 0; i < old->count; ++i)
    {
        const struct mapped_range *range = &old->data[i];
        uint64_t range_end = range->guest + range->size;
        struct mapped_range slice;
        BOOL remove = size ? range_overlaps( range, start, end ) :
                             range->allocation_base == allocation_base;

        if (!remove)
        {
            if (!range_array_append( result, range )) return STATUS_INVALID_ADDRESS;
            continue;
        }
        if (range->permanent) return STATUS_ACCESS_DENIED;
        if (!size) continue;
        if (range->guest < start)
        {
            slice = range_slice( range, range->guest, start );
            if (!range_array_append( result, &slice )) return STATUS_INVALID_ADDRESS;
        }
        if (range_end > end)
        {
            slice = range_slice( range, end, range_end );
            if (!range_array_append( result, &slice )) return STATUS_INVALID_ADDRESS;
        }
    }
    return STATUS_SUCCESS;
}

static void mark_engine_mappings_stale_locked( uint64_t start, uint64_t size,
                                               uint64_t allocation_base )
{
    struct thread_engine *engine;
    uint64_t end = size ? start + size : 0;

    for (engine = provider.engines; engine; engine = engine->next)
    {
        size_t i = 0;

        if (size)
        {
            size_t left = 0, right = engine->mapped_ranges.count;

            while (left < right)
            {
                size_t mid = left + (right - left) / 2;
                const struct mapped_range *range = &engine->mapped_ranges.data[mid];

                if (range->guest + range->size <= start) left = mid + 1;
                else right = mid;
            }
            i = left;
        }
        for (; i < engine->mapped_ranges.count; ++i)
        {
            struct mapped_range *range = &engine->mapped_ranges.data[i];

            if (size)
            {
                if (range->guest >= end) break;
                if (!range_overlaps( range, start, end )) continue;
            }
            else if (range->allocation_base != allocation_base) continue;
            range->stale = TRUE;
        }
    }
}

static NTSTATUS build_protected_registry( const struct range_array *old, uint64_t start,
                                          uint64_t end, unsigned int perms,
                                          struct range_array *result )
{
    size_t i;

    if (old->count > (SIZE_MAX - 1) / 3 ||
        !range_array_reserve( result, old->count * 3 + 1 ))
        return STATUS_NO_MEMORY;
    for (i = 0; i < old->count; ++i)
    {
        const struct mapped_range *range = &old->data[i];
        uint64_t range_end = range->guest + range->size;
        uint64_t overlap_start, overlap_end;
        struct mapped_range slice;

        if (!range_overlaps( range, start, end ))
        {
            if (!range_array_append( result, range )) return STATUS_INVALID_ADDRESS;
            continue;
        }
        if (range->permanent) return STATUS_ACCESS_DENIED;
        overlap_start = max( range->guest, start );
        overlap_end = min( range_end, end );
        if (range->guest < overlap_start)
        {
            slice = range_slice( range, range->guest, overlap_start );
            if (!range_array_append( result, &slice )) return STATUS_INVALID_ADDRESS;
        }
        slice = range_slice( range, overlap_start, overlap_end );
        slice.perms = perms;
        if (!range_array_append( result, &slice )) return STATUS_INVALID_ADDRESS;
        if (overlap_end < range_end)
        {
            slice = range_slice( range, overlap_end, range_end );
            if (!range_array_append( result, &slice )) return STATUS_INVALID_ADDRESS;
        }
    }
    return STATUS_SUCCESS;
}

static uc_err map_host_range( struct thread_engine *engine, uint64_t guest, uint64_t host,
                              uint64_t size, unsigned int perms )
{
    uint64_t guest_page, guest_end, host_page, host_end;

    if (!size) return UC_ERR_OK;
    if (((guest ^ host) & (XTAJIT64_GUEST_PAGE_SIZE - 1))) return UC_ERR_ARG;
    if (!align_range( guest, size, &guest_page, &guest_end ) ||
        !align_range( host, size, &host_page, &host_end ) ||
        guest_end - guest_page != host_end - host_page)
        return UC_ERR_ARG;

    /* A collision here means the engine no longer matches the canonical
     * registry.  Protecting an unknown existing region would silently retain
     * the wrong host backing, so let the caller poison the provider. */
    return uc_mem_map_ptr( engine->uc, guest_page, guest_end - guest_page, perms,
                           (void *)(uintptr_t)host_page );
}

static uc_err unmap_range( struct thread_engine *engine, uint64_t guest, uint64_t size )
{
    if (!size) return UC_ERR_OK;
    return uc_mem_unmap( engine->uc, guest, size );
}

static void trace_mapping_diagnostic( const struct thread_engine *engine,
                                      const char *event, uint64_t latest_size )
{
#ifdef XTAJIT64_UNIXLIB_TEST
    (void)engine;
    (void)event;
    (void)latest_size;
#else
    TRACE_(xtajitmap)(
        "pid %ld engine %llu pool=%u/%u/%u event %s maps %llu bytes %llu "
        "buckets 4k=%llu 16k=%llu "
        "64k=%llu 1m=%llu large=%llu max=%llu latest=%llu mapped=%zu "
        "syncs=%llu resync-unmaps=%llu/%llu generation=%llu\n",
        (long)getpid(), (unsigned long long)engine->diagnostic_id,
        engine->diagnostic_pool_in_use, engine->diagnostic_pool_size,
        engine->diagnostic_pool_high_water, event,
        (unsigned long long)engine->demand_map_calls,
        (unsigned long long)engine->demand_map_bytes,
        (unsigned long long)engine->demand_map_4k_calls,
        (unsigned long long)engine->demand_map_16k_calls,
        (unsigned long long)engine->demand_map_64k_calls,
        (unsigned long long)engine->demand_map_1m_calls,
        (unsigned long long)engine->demand_map_large_calls,
        (unsigned long long)engine->demand_map_max_size,
        (unsigned long long)latest_size, engine->mapped_ranges.count,
        (unsigned long long)engine->registry_sync_calls,
        (unsigned long long)engine->resync_unmap_calls,
        (unsigned long long)engine->resync_unmap_bytes,
        (unsigned long long)engine->mapping_generation );
#endif
}

static const struct mapped_range *find_canonical_mapping( uint64_t address,
                                                          uint64_t size,
                                                          unsigned int perms )
{
    const struct mapped_range *range;
    size_t left = 0, right = provider.ranges.count;
    uint64_t end;

    if (!size || address > UINT64_MAX - size) return NULL;
    end = address + size;
    while (left < right)
    {
        size_t mid = left + (right - left) / 2;

        if (provider.ranges.data[mid].guest <= address) left = mid + 1;
        else right = mid;
    }
    if (!left) return NULL;
    range = &provider.ranges.data[left - 1];
    if (range->state != MEM_COMMIT || range->size > UINT64_MAX - range->guest ||
        address < range->guest || end > range->guest + range->size ||
        (range->perms & perms) != perms)
        return NULL;
    return range;
}

static uc_err demand_map_canonical_range( struct thread_engine *engine,
                                          uint64_t address, uint64_t size,
                                          unsigned int perms, BOOL *found,
                                          BOOL *mapped )
{
    const struct mapped_range *range;
    struct mapped_range mapping;
    struct range_array replacement = {0}, old;
    uint64_t start, end, mapping_start, mapping_end, range_end;
    NTSTATUS status;
    uc_err err;
    size_t i;

    *found = FALSE;
    *mapped = FALSE;
    if (!(range = find_canonical_mapping( address, size, 0 ))) return UC_ERR_MAP;
    *found = TRUE;
    /* An engine that has not touched a canonical page reports UNMAPPED even
     * when the requested guest access violates that page's protection.  Keep
     * this as a Windows access violation instead of misclassifying it as a
     * late native mapping that should be registered again. */
    if ((range->perms & perms) != perms) return UC_ERR_OK;

    range_end = range->guest + range->size;
    if (!align_range( address, size, &start, &end ) ||
        start < range->guest || end > range_end)
        return UC_ERR_ARG;

    /* Canonical ranges can split or coalesce between generations.  Map the
     * entire still-unmapped gap containing the fault, bounded by retained
     * actual mappings.  Mapping the whole enlarged canonical range would
     * collide with a retained prefix or suffix; mapping one guest page per
     * fault makes Unicorn rebuild its MemoryRegion topology thousands of times
     * during a large application startup. */
    mapping_start = range->guest;
    mapping_end = range_end;
    for (i = 0; i < engine->mapped_ranges.count; ++i)
    {
        const struct mapped_range *mapped = &engine->mapped_ranges.data[i];
        uint64_t mapped_end;

        if (mapped->size > UINT64_MAX - mapped->guest) return UC_ERR_ARG;
        mapped_end = mapped->guest + mapped->size;
        if (mapped_end <= start)
        {
            mapping_start = max( mapping_start, mapped_end );
            continue;
        }
        if (mapped->guest >= end)
        {
            mapping_end = min( mapping_end, mapped->guest );
            break;
        }
        return UC_ERR_MAP;
    }
    if (mapping_start > start || mapping_end < end || mapping_start >= mapping_end)
        return UC_ERR_MAP;
    if (mapping_end - mapping_start > XTAJIT64_DEMAND_MAP_MAX_SIZE)
    {
        /* Keep large image and sparse reservation faults coarse enough to avoid
         * per-page hook churn, but do not force every pooled engine to
         * instantiate hundreds of megabytes after a single code or data touch.
         * The required Unicorn contract invokes invalid-memory hooks for an
         * unmapped atomic read-modify-write target, so writable data can follow
         * this same bounded first-touch path without eager process-wide
         * cloning into every engine. */
        mapping_start = start;
        mapping_end = min( mapping_end,
                           mapping_start + XTAJIT64_DEMAND_MAP_MAX_SIZE );
        if (mapping_end < end) return UC_ERR_ARG;
    }
    mapping = range_slice( range, mapping_start, mapping_end );

    for (i = 0; i < engine->mapped_ranges.count; ++i)
        if (range_overlaps( &engine->mapped_ranges.data[i], mapping.guest,
                            mapping.guest + mapping.size ))
            return UC_ERR_MAP;
    status = build_mapped_registry( &engine->mapped_ranges, &mapping, &replacement );
    if (status) return status == STATUS_NO_MEMORY ? UC_ERR_NOMEM : UC_ERR_ARG;
    if ((err = map_host_range( engine, mapping.guest, mapping.host, mapping.size,
                               mapping.perms )) != UC_ERR_OK)
    {
        range_array_free( &replacement );
        return err;
    }

    old = engine->mapped_ranges;
    engine->mapped_ranges = replacement;
    range_array_free( &old );

    ++engine->demand_map_calls;
    engine->demand_map_bytes += mapping.size;
    if (mapping.size <= XTAJIT64_GUEST_PAGE_SIZE) ++engine->demand_map_4k_calls;
    else if (mapping.size <= 4 * XTAJIT64_GUEST_PAGE_SIZE)
        ++engine->demand_map_16k_calls;
    else if (mapping.size <= 16 * XTAJIT64_GUEST_PAGE_SIZE)
        ++engine->demand_map_64k_calls;
    else if (mapping.size <= 256 * XTAJIT64_GUEST_PAGE_SIZE)
        ++engine->demand_map_1m_calls;
    else ++engine->demand_map_large_calls;
    engine->demand_map_max_size = max( engine->demand_map_max_size, mapping.size );
    if (!(engine->demand_map_calls & (engine->demand_map_calls - 1)))
        trace_mapping_diagnostic( engine, "map", mapping.size );
    *mapped = TRUE;
    return UC_ERR_OK;
}

static void poison_provider_locked( NTSTATUS status )
{
    if (!provider.poison_status)
        provider.poison_status = status ? status : STATUS_UNSUCCESSFUL;
}

static const char *mutation_kind_name( enum mutation_kind kind )
{
    switch (kind)
    {
    case MUTATION_MAP: return "map";
    case MUTATION_UNMAP: return "unmap";
    case MUTATION_PROTECT: return "protect";
    case MUTATION_RESYNC: return "resync";
    case MUTATION_FLUSH: return "flush";
    case MUTATION_POISON: return "poison";
    default: return "none";
    }
}

static const char *mutation_stage_name( enum mutation_stage stage )
{
    switch (stage)
    {
    case MUTATION_STAGE_PAUSE: return "pause";
    case MUTATION_STAGE_WAIT: return "wait";
    case MUTATION_STAGE_PREPARE: return "prepare";
    case MUTATION_STAGE_APPLY: return "apply";
    case MUTATION_STAGE_PUBLISH: return "publish";
    default: return "idle";
    }
}

static BOOL current_thread_owns_mutation_locked(void)
{
    return provider.mutating && provider.mutation_owner_valid &&
           pthread_equal( provider.mutation_owner, pthread_self() );
}

static void set_mutation_stage_locked( enum mutation_stage stage )
{
    if (current_thread_owns_mutation_locked()) provider.mutation_stage = stage;
}

#ifdef XTAJIT64_UNIXLIB_TEST
static void test_mutation_fault_checkpoint( enum test_mutation_fault_point point )
{
    if (atomic_load_explicit( &test_mutation_fault_point, memory_order_acquire ) != point)
        return;
    atomic_store_explicit( &test_mutation_fault_entered, 1, memory_order_release );
    while (!atomic_load_explicit( &test_mutation_fault_release, memory_order_acquire ))
        sched_yield();
    XTAJIT64_TEST_RAISE_EXCEPTION();
}
#else
static void test_mutation_fault_checkpoint( int point )
{
    (void)point;
}
# define TEST_MUTATION_FAULT_AFTER_BEGIN 0
# define TEST_MUTATION_FAULT_ENGINE_PAUSE 0
#endif

static BOOL any_engine_running_locked(void)
{
    struct thread_engine *engine;

    for (engine = provider.engines; engine; engine = engine->next)
        if (engine->running) return TRUE;
    return FALSE;
}

static BOOL any_engine_in_use_locked(void)
{
    struct thread_engine *engine;

    for (engine = provider.engines; engine; engine = engine->next)
        if (engine->in_use) return TRUE;
    return FALSE;
}

static void request_engine_pause_locked( struct thread_engine *engine )
{
    if (!engine->running) return;
    atomic_store_explicit( &engine->pause_requested, true, memory_order_release );
    test_mutation_fault_checkpoint( TEST_MUTATION_FAULT_ENGINE_PAUSE );
}

static NTSTATUS claim_mutation_locked( enum mutation_kind kind, BOOL advance_generation )
{
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down) return STATUS_INVALID_HANDLE;
    if (provider.poison_status) return provider.poison_status;

    provider.mutating = TRUE;
    provider.mutation_owner = pthread_self();
    provider.mutation_owner_valid = TRUE;
    provider.mutation_kind = kind;
    provider.mutation_stage = MUTATION_STAGE_PAUSE;
    if (advance_generation) ++provider.generation;
    return STATUS_SUCCESS;
}

static void pause_mutation_engines_locked(void)
{
    struct thread_engine *engine;

    for (engine = provider.engines; engine; engine = engine->next)
        request_engine_pause_locked( engine );
}

static NTSTATUS wait_for_mutation_engines_locked(void)
{
    provider.mutation_stage = MUTATION_STAGE_WAIT;
    while (any_engine_running_locked())
        pthread_cond_wait( &provider.cond, &provider.mutex );
    provider.mutation_stage = MUTATION_STAGE_PREPARE;
    return provider.poison_status;
}

static void finish_mutation_locked(void)
{
    provider.mutation_owner_valid = FALSE;
    provider.mutation_kind = MUTATION_NONE;
    provider.mutation_stage = MUTATION_STAGE_IDLE;
    provider.mutating = FALSE;
    pthread_cond_broadcast( &provider.cond );
}

static NTSTATUS record_mutation_access_violation_locked( enum mutation_kind kind,
                                                         enum mutation_stage stage,
                                                         uint64_t generation )
{
    provider.last_fault_kind = kind;
    provider.last_fault_stage = stage;
    provider.last_fault_generation = generation;
    if (provider.initialized) poison_provider_locked( STATUS_ACCESS_VIOLATION );
    if (current_thread_owns_mutation_locked()) finish_mutation_locked();
    return STATUS_ACCESS_VIOLATION;
}

static NTSTATUS recover_mutation_access_violation_locked(void)
{
    return record_mutation_access_violation_locked( provider.mutation_kind,
                                                    provider.mutation_stage,
                                                    provider.generation );
}

static void report_mutation_access_violation( enum mutation_kind kind,
                                              enum mutation_stage stage,
                                              uint64_t generation )
{
    ERR( "access violation during x64 %s mutation stage %s at generation %llu\n",
         mutation_kind_name( kind ), mutation_stage_name( stage ),
         (unsigned long long)generation );
}

static BOOL is_ec_code( uint64_t address )
{
    uint64_t page, word;

    if (!provider.ec_bitmap || address > provider.highest_user_address) return FALSE;
    page = address / provider.ec_page_size;
    word = provider.ec_bitmap[page / 64];
    return (word >> (page & 63)) & 1;
}

#ifdef __APPLE__
static BOOL query_host_page( uint64_t address, unsigned int *perms )
{
    vm_region_submap_info_data_64_t info;
    mach_vm_address_t region = address;
    mach_vm_size_t size;
    natural_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    uint32_t depth = 0;
    kern_return_t ret;

    ret = mach_vm_region_recurse( mach_task_self(), &region, &size, &depth,
                                  (vm_region_recurse_info_t)&info, &count );
    if (ret != KERN_SUCCESS || region > address || address - region >= size) return FALSE;

    *perms = 0;
    if (info.protection & VM_PROT_READ) *perms |= UC_PROT_READ;
    if (info.protection & VM_PROT_WRITE) *perms |= UC_PROT_WRITE;
    if (info.protection & VM_PROT_EXECUTE) *perms |= UC_PROT_EXEC;
    return TRUE;
}
#endif

static BOOL unmapped_ec_target_is_executable( uint64_t address )
{
#ifdef __APPLE__
    unsigned int perms;

    return query_host_page( address, &perms ) && (perms & UC_PROT_EXEC);
#else
    return FALSE;
#endif
}

static void stop_at_ec_target( struct thread_engine *engine, uc_engine *uc,
                               uint64_t address )
{
    engine->transition_target = address;
    engine->stop_reason = XTAJIT64_STOP_EC_TRANSITION;
    uc_emu_stop( uc );
}

static void stop_at_instruction_boundary( struct thread_engine *engine,
                                          uc_engine *uc )
{
    uc_err err;

    /* An ordinary hook-time stop can commit the current translation block's
     * side effects while restoring RIP to its beginning.  Resuming that
     * context would repeat stores, stack updates, calls, or returns. */
    err = uc_emu_stop_at_instruction_boundary( uc );
    if (err == UC_ERR_OK) return;

    engine->mapping_error = err;
    engine->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
    uc_emu_stop( uc );
}

static void block_hook( uc_engine *uc, uint64_t address, uint32_t size, void *user )
{
    struct thread_engine *engine = user;

    (void)size;
    if (is_ec_code( address ))
    {
#ifdef XTAJIT64_UNIXLIB_TEST
        if (atomic_load_explicit( &test_hold_ec_hook, memory_order_acquire ))
        {
            atomic_store_explicit( &test_ec_hook_entered, 1, memory_order_release );
            while (!atomic_load_explicit( &test_release_ec_hook, memory_order_acquire ))
                sched_yield();
        }
#endif
        stop_at_ec_target( engine, uc, address );
        return;
    }

#ifdef XTAJIT64_UNIXLIB_TEST
    if (atomic_load_explicit( &test_hold_non_ec_hook, memory_order_acquire ))
    {
        atomic_store_explicit( &test_non_ec_hook_entered, 1, memory_order_release );
        while (!atomic_load_explicit( &test_release_non_ec_hook, memory_order_acquire ))
            sched_yield();
    }
#endif
    if (engine->suspend_doorbell && *engine->suspend_doorbell)
    {
        engine->stop_reason = XTAJIT64_STOP_SUSPEND;
        stop_at_instruction_boundary( engine, uc );
        return;
    }
    if (!atomic_load_explicit( &engine->pause_requested, memory_order_acquire )) return;
#ifdef XTAJIT64_UNIXLIB_TEST
    if (!pthread_equal( engine->owner, pthread_self() ))
        atomic_store_explicit( &test_pause_stop_owner_violation, 1, memory_order_relaxed );
    atomic_fetch_add_explicit( &test_pause_stop_count, 1, memory_order_relaxed );
#endif
    stop_at_instruction_boundary( engine, uc );
}

static void syscall_hook( uc_engine *uc, void *user )
{
    struct thread_engine *engine = user;

    engine->stop_reason = XTAJIT64_STOP_SYSCALL;
    uc_emu_stop( uc );
}

static void interrupt_hook( uc_engine *uc, uint32_t intno, void *user )
{
    struct thread_engine *engine = user;
    uint64_t eflags = 0;

    if (intno == 0x2e)
        engine->stop_reason = XTAJIT64_STOP_SYSCALL;
    else if (intno == 1 &&
             uc_reg_read( uc, UC_X86_REG_EFLAGS, &eflags ) == UC_ERR_OK &&
             (eflags & 0x100))
        engine->stop_reason = XTAJIT64_STOP_SINGLE_STEP;
    else
        engine->stop_reason = XTAJIT64_STOP_INVALID_INSTRUCTION;
    uc_emu_stop( uc );
}

static bool invalid_instruction_hook( uc_engine *uc, void *user )
{
    struct thread_engine *engine = user;

    engine->stop_reason = XTAJIT64_STOP_INVALID_INSTRUCTION;
    uc_emu_stop( uc );
    return false;
}

static bool invalid_memory_hook( uc_engine *uc, uc_mem_type type, uint64_t address,
                                 int size, int64_t value, void *user )
{
    struct thread_engine *engine = user;
    unsigned int required_perms = 0;
    uint32_t fault_access = EXCEPTION_READ_FAULT;
    BOOL found = FALSE, mapped = FALSE;
    uc_err err;

    (void)value;
    if (type == UC_MEM_FETCH_UNMAPPED && is_ec_code( address ) &&
        unmapped_ec_target_is_executable( address ))
    {
        stop_at_ec_target( engine, uc, address );
        return false;
    }

    switch (type)
    {
    case UC_MEM_READ_UNMAPPED:
    case UC_MEM_READ_PROT:
        required_perms = type == UC_MEM_READ_UNMAPPED ? UC_PROT_READ : 0;
        fault_access = EXCEPTION_READ_FAULT;
        break;
    case UC_MEM_WRITE_UNMAPPED:
    case UC_MEM_WRITE_PROT:
        required_perms = type == UC_MEM_WRITE_UNMAPPED ? UC_PROT_WRITE : 0;
        fault_access = EXCEPTION_WRITE_FAULT;
        break;
    case UC_MEM_FETCH_UNMAPPED:
    case UC_MEM_FETCH_PROT:
        required_perms = type == UC_MEM_FETCH_UNMAPPED ? UC_PROT_EXEC : 0;
        fault_access = EXCEPTION_EXECUTE_FAULT;
        break;
    default: break;
    }
    if (required_perms && size > 0)
    {
        err = demand_map_canonical_range( engine, address, size,
                                          required_perms, &found, &mapped );
        if (err == UC_ERR_OK && mapped) return true;
        if (err == UC_ERR_OK && found)
        {
            engine->fault_address = address;
            engine->fault_access = fault_access;
            engine->stop_reason = XTAJIT64_STOP_MEMORY_FAULT;
            uc_emu_stop( uc );
            return false;
        }
        if (found)
        {
            engine->fault_address = address;
            engine->fault_access = fault_access;
            engine->mapping_error = err;
            engine->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
            uc_emu_stop( uc );
            return false;
        }
    }

    engine->fault_address = address;
    engine->fault_access = fault_access;
    /* A committed identity mapping can be created by a native Unixlib through
     * ntdll.so without traversing the ARM64EC PE syscall notification.  Let
     * the PE side query this exact address once before deciding that the guest
     * access is invalid.  Protection faults already have an authoritative
     * engine mapping and remain ordinary Windows access violations. */
    engine->stop_reason = required_perms ? XTAJIT64_STOP_MAPPING_MISS :
                                           XTAJIT64_STOP_MEMORY_FAULT;
    uc_emu_stop( uc );
    return false;
}

static uc_err install_engine_hooks( struct thread_engine *engine )
{
    uc_hook hook;
    uc_err err;

    /* ARM64EC transitions occur only at translated-block boundaries.  A block
     * hook preserves the EC bitmap contract without the old per-instruction
     * callback on the emulation hot path. */
    if ((err = uc_hook_add( engine->uc, &hook, UC_HOOK_BLOCK, block_hook,
                            engine, 1, 0 )) != UC_ERR_OK)
        return err;
    if ((err = uc_hook_add( engine->uc, &hook, UC_HOOK_INSN, syscall_hook,
                            engine, 1, 0, UC_X86_INS_SYSCALL )) != UC_ERR_OK)
        return err;
    if ((err = uc_hook_add( engine->uc, &hook, UC_HOOK_INTR, interrupt_hook,
                            engine, 1, 0 )) != UC_ERR_OK)
        return err;
    if ((err = uc_hook_add( engine->uc, &hook, UC_HOOK_INSN_INVALID,
                            invalid_instruction_hook, engine, 1, 0 )) != UC_ERR_OK)
        return err;
    return uc_hook_add( engine->uc, &hook, UC_HOOK_MEM_INVALID,
                        invalid_memory_hook, engine, 1, 0 );
}

static uc_err open_thread_engine( struct thread_engine *engine )
{
    uc_err err;

    if ((err = uc_open( UC_ARCH_X86, UC_MODE_64, &engine->uc )) != UC_ERR_OK) return err;
    if ((err = uc_enable_shared_memory_atomics( engine->uc )) != UC_ERR_OK)
    {
        uc_close( engine->uc );
        engine->uc = NULL;
        return err;
    }
    if ((err = install_engine_hooks( engine )) != UC_ERR_OK)
    {
        range_array_free( &engine->mapped_ranges );
        uc_close( engine->uc );
        engine->uc = NULL;
    }
    else engine->mapping_generation = 0;
    return err;
}

static uc_err write_context( struct thread_engine *engine,
                             const struct xtajit64_x64_context *context,
                             uint64_t gs_base )
{
    const UINT64 *values = &context->rax;
    uc_err err;
    unsigned int i;

#ifdef XTAJIT64_UNIXLIB_TEST
    atomic_fetch_add_explicit( &test_context_write_count, 1, memory_order_relaxed );
#endif
    for (i = 0; i < ARRAY_SIZE(integer_regs); ++i)
        if ((err = uc_reg_write( engine->uc, integer_regs[i], &values[i] )) != UC_ERR_OK)
            return err;
    if ((err = uc_reg_write( engine->uc, UC_X86_REG_GS_BASE, &gs_base )) != UC_ERR_OK)
        return err;
    if ((err = uc_reg_write( engine->uc, UC_X86_REG_MXCSR, &context->mxcsr )) != UC_ERR_OK)
        return err;
    for (i = 0; i < ARRAY_SIZE(xmm_regs); ++i)
        if ((err = uc_reg_write( engine->uc, xmm_regs[i], context->xmm[i] )) != UC_ERR_OK)
            return err;
    return UC_ERR_OK;
}

static uc_err read_context( struct thread_engine *engine,
                            struct xtajit64_x64_context *context )
{
    UINT64 *values = &context->rax;
    uc_err err;
    unsigned int i;

#ifdef XTAJIT64_UNIXLIB_TEST
    atomic_fetch_add_explicit( &test_context_read_count, 1, memory_order_relaxed );
    if (atomic_load_explicit( &test_check_context_read_lock, memory_order_acquire ))
    {
        int ret = pthread_mutex_trylock( &provider.mutex );

        if (!ret)
        {
            atomic_store_explicit( &test_context_read_lock_violation, 1,
                                   memory_order_release );
            pthread_mutex_unlock( &provider.mutex );
        }
        else if (ret != EBUSY)
            atomic_store_explicit( &test_context_read_lock_violation, 1,
                                   memory_order_release );
    }
#endif
    for (i = 0; i < ARRAY_SIZE(integer_regs); ++i)
        if ((err = uc_reg_read( engine->uc, integer_regs[i], &values[i] )) != UC_ERR_OK)
            return err;
    if ((err = uc_reg_read( engine->uc, UC_X86_REG_MXCSR, &context->mxcsr )) != UC_ERR_OK)
        return err;
    for (i = 0; i < ARRAY_SIZE(xmm_regs); ++i)
        if ((err = uc_reg_read( engine->uc, xmm_regs[i], context->xmm[i] )) != UC_ERR_OK)
            return err;
    return UC_ERR_OK;
}

static uc_err prepare_x64_syscall_engine( struct thread_engine *engine,
                                          uint64_t dispatcher, uint32_t count,
                                          uint64_t *next_rip )
{
    uint64_t rax, r10, rip;
    uc_err err;

    if ((err = uc_reg_read( engine->uc, UC_X86_REG_RAX, &rax )) != UC_ERR_OK)
        return err;
    if ((err = uc_reg_read( engine->uc, UC_X86_REG_RIP, &rip )) != UC_ERR_OK)
        return err;
    if (rax >= count)
    {
        rax = (uint64_t)(int64_t)STATUS_INVALID_SYSTEM_SERVICE;
        if ((err = uc_reg_write( engine->uc, UC_X86_REG_RAX, &rax )) != UC_ERR_OK)
            return err;
        *next_rip = rip;
        return UC_ERR_OK;
    }
    if ((err = uc_reg_read( engine->uc, UC_X86_REG_R10, &r10 )) != UC_ERR_OK)
        return err;

    /* Match ntdll's ARM64EC STATUS_EMULATION_SYSCALL conversion.  Unicorn
     * reports RIP after both SYSCALL and INT 2E once the stop hook returns. */
    if ((err = uc_reg_write( engine->uc, UC_X86_REG_RCX, &r10 )) != UC_ERR_OK ||
        (err = uc_reg_write( engine->uc, UC_X86_REG_R10, &rip )) != UC_ERR_OK ||
        (err = uc_reg_write( engine->uc, UC_X86_REG_RIP, &dispatcher )) != UC_ERR_OK)
        return err;
    *next_rip = dispatcher;
    return UC_ERR_OK;
}

static uc_err create_pool_engine_locked( struct thread_engine **result )
{
    struct thread_engine *engine;
    uc_context *initial_context = NULL;
    uc_err err;

    if (!(engine = calloc( 1, sizeof(*engine) ))) return UC_ERR_NOMEM;
    atomic_init( &engine->pause_requested, false );
    if ((err = open_thread_engine( engine )) != UC_ERR_OK) goto failed;
    if (!provider.initial_context)
    {
        if ((err = uc_context_alloc( engine->uc, &initial_context )) != UC_ERR_OK ||
            (err = uc_context_save( engine->uc, initial_context )) != UC_ERR_OK)
            goto failed;
        provider.initial_context = initial_context;
    }
    engine->diagnostic_id = ++provider.next_diagnostic_id;
    engine->next = provider.engines;
    provider.engines = engine;
    engine->linked = TRUE;
    ++provider.engine_count;
    if (result) *result = engine;
    return UC_ERR_OK;

failed:
    if (initial_context) uc_context_free( initial_context );
    if (engine->uc) uc_close( engine->uc );
    range_array_free( &engine->mapped_ranges );
    free( engine );
    return err;
}

static uc_err acquire_pool_engine_locked( struct thread_binding *binding,
                                          struct thread_engine **result )
{
    struct thread_engine *engine, *idle = NULL;
    uc_err err;

    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down || provider.poison_status)
        return UC_ERR_HANDLE;
    idle = NULL;
    for (engine = provider.engines; engine; engine = engine->next)
    {
        if (engine->in_use) continue;
        if (!idle) idle = engine;
        if (engine->resident_binding_id == binding->id) break;
    }
    if (!engine) engine = idle;
    if (!engine && (err = create_pool_engine_locked( &engine )) != UC_ERR_OK)
        return err;
    if (!binding->context &&
        (err = uc_context_alloc( engine->uc, &binding->context )) != UC_ERR_OK)
        return err;
    if (engine->resident_binding_id != binding->id &&
        (err = uc_context_restore( engine->uc, binding->context_valid ?
                                   binding->context : provider.initial_context )) != UC_ERR_OK)
        return err;

    engine->resident_binding_id = 0;
    engine->owner = pthread_self();
    engine->in_use = TRUE;
    ++provider.engines_in_use;
    provider.engine_high_water = max( provider.engine_high_water,
                                      provider.engines_in_use );
    engine->diagnostic_pool_size = provider.engine_count;
    engine->diagnostic_pool_in_use = provider.engines_in_use;
    engine->diagnostic_pool_high_water = provider.engine_high_water;
    binding->active = TRUE;
#ifdef XTAJIT64_UNIXLIB_TEST
    test_last_acquired_engine = engine;
#endif
    *result = engine;
    return UC_ERR_OK;
}

static uc_err release_pool_engine_locked( struct thread_binding *binding,
                                          struct thread_engine *engine,
                                          BOOL save_context )
{
    uc_err err = UC_ERR_OK;

    if (save_context)
    {
        err = uc_context_save( engine->uc, binding->context );
        if (err == UC_ERR_OK)
        {
            binding->context_valid = TRUE;
            engine->resident_binding_id = binding->id;
        }
    }
    else engine->resident_binding_id = 0;
    engine->running = FALSE;
    engine->in_use = FALSE;
    engine->suspend_doorbell = NULL;
    --provider.engines_in_use;
    binding->active = FALSE;
    pthread_cond_broadcast( &provider.cond );
    return err;
}

static void destroy_thread_binding( void *value )
{
    struct thread_binding *binding = value;

    if (!binding) return;
    if (binding->context) uc_context_free( binding->context );
    free( binding );
}

static void make_engine_key(void)
{
    engine_key_error = pthread_key_create( &engine_key, destroy_thread_binding );
}

static NTSTATUS merge_range_arrays( const struct range_array *left,
                                       const struct range_array *right,
                                       struct range_array *result )
{
    size_t i = 0, j = 0;

    if (left->count > SIZE_MAX - right->count ||
        !range_array_reserve( result, left->count + right->count ))
        return STATUS_NO_MEMORY;
    while (i < left->count || j < right->count)
    {
        const struct mapped_range *range;

        if (j == right->count ||
            (i < left->count && left->data[i].guest <= right->data[j].guest))
            range = &left->data[i++];
        else range = &right->data[j++];
        if (!range_array_append( result, range )) return STATUS_INVALID_ADDRESS;
    }
    return STATUS_SUCCESS;
}

static enum mutation_kind observer_mutation_kind( uint32_t operation )
{
    switch (operation)
    {
    case WINE_WOW64_MEMORY_ALLOCATE:
    case WINE_WOW64_MEMORY_COMMIT:
    case WINE_WOW64_MEMORY_MAP:
        return MUTATION_MAP;
    case WINE_WOW64_MEMORY_DECOMMIT:
    case WINE_WOW64_MEMORY_RELEASE:
    case WINE_WOW64_MEMORY_UNMAP:
        return MUTATION_UNMAP;
    case WINE_WOW64_MEMORY_PROTECT:
        return MUTATION_PROTECT;
    default:
        return MUTATION_RESYNC;
    }
}

static BOOL observer_operation_is_valid( uint32_t operation )
{
    return operation >= WINE_WOW64_MEMORY_RESYNC &&
           operation <= WINE_WOW64_MEMORY_UNMAP;
}

static BOOL low_host_interval_to_guest( uint64_t host, uint64_t size,
                                        uint64_t *guest_start, uint64_t *guest_end )
{
    uint64_t guest;

    if (!size || (host & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
        (size & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
        host < WINE_LOW_VA_SHADOW_BASE)
        return FALSE;
    guest = host - WINE_LOW_VA_SHADOW_BASE;
    if (guest >= WINE_LOW_VA_SHADOW_SIZE ||
        size > WINE_LOW_VA_SHADOW_SIZE - guest)
        return FALSE;
    *guest_start = guest;
    *guest_end = guest + size;
    return TRUE;
}

static BOOL low_host_allocation_base_is_valid( uint64_t host_allocation_base )
{
    return !host_allocation_base ||
           (!(host_allocation_base & (XTAJIT64_GUEST_PAGE_SIZE - 1)) &&
            host_allocation_base >= WINE_LOW_VA_SHADOW_BASE &&
            host_allocation_base - WINE_LOW_VA_SHADOW_BASE <
                WINE_LOW_VA_SHADOW_SIZE);
}

static BOOL observer_protection_is_valid( uint32_t protect )
{
    uint32_t base = protect & 0xff;

    if (protect & ~(0xffu | PAGE_GUARD | PAGE_NOCACHE)) return FALSE;
    switch (base)
    {
    case PAGE_NOACCESS:
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS validate_observer_event_header(
    const struct wine_arm64ec_low_memory_event_v1 *event,
    const struct arm64ec_low_observer_transaction *transaction,
    uint64_t *guest_start, uint64_t *guest_end, BOOL *full_snapshot )
{
    if (!event || event->version != WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION ||
        event->size != sizeof(*event) || event->operation != transaction->operation ||
        !observer_operation_is_valid( event->operation ) ||
        (event->flags & ~WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT) ||
        event->reserved[0] || event->reserved[1] ||
        event->range_count > XTAJIT64_MAX_RESYNC_RANGES ||
        event->range_count > SIZE_MAX ||
        (event->range_count && !event->ranges) ||
        !low_host_interval_to_guest( event->host_address, event->size_covered,
                                     guest_start, guest_end ) ||
        !low_host_allocation_base_is_valid( event->host_allocation_base ))
        return STATUS_INVALID_PARAMETER;

    *full_snapshot = !!(event->flags & WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT);
    if (*full_snapshot)
    {
        if (event->host_address != WINE_LOW_VA_SHADOW_BASE ||
            event->size_covered != WINE_LOW_VA_SHADOW_SIZE ||
            event->host_allocation_base)
            return STATUS_INVALID_PARAMETER;
    }
    else if (!provider.observer_active)
        return STATUS_INVALID_DEVICE_STATE;
    return STATUS_SUCCESS;
}

static NTSTATUS build_observer_ranges(
    const struct wine_arm64ec_low_memory_event_v1 *event,
    uint64_t guest_start, uint64_t guest_end, struct range_array *result )
{
    uint64_t cursor = guest_start;
    size_t i, count = (size_t)event->range_count;

    if (count && !range_array_reserve( result, count )) return STATUS_NO_MEMORY;
    for (i = 0; i < count; ++i)
    {
        const struct wine_arm64ec_low_memory_range_v1 *input = &event->ranges[i];
        struct mapped_range mapping;
        uint64_t input_guest, input_end, allocation_guest = 0;

        if (input->flags & ~WINE_ARM64EC_LOW_MEMORY_RANGE_VALID_FLAGS ||
            input->reserved || !input->size ||
            !low_host_interval_to_guest( input->host_address, input->size,
                                         &input_guest, &input_end ) ||
            input_guest != cursor || input_end > guest_end)
            return STATUS_INVALID_PARAMETER;
        switch (input->state)
        {
        case MEM_FREE:
            if (input->host_allocation_base || input->protect != PAGE_NOACCESS)
                return STATUS_INVALID_PARAMETER;
            break;
        case MEM_RESERVE:
            if (!input->host_allocation_base || input->protect ||
                !low_host_allocation_base_is_valid( input->host_allocation_base ) ||
                input->host_allocation_base > input->host_address)
                return STATUS_INVALID_PARAMETER;
            allocation_guest = input->host_allocation_base - WINE_LOW_VA_SHADOW_BASE;
            break;
        case MEM_COMMIT:
            if (!input->host_allocation_base ||
                !low_host_allocation_base_is_valid( input->host_allocation_base ) ||
                input->host_allocation_base > input->host_address ||
                !observer_protection_is_valid( input->protect ))
                return STATUS_INVALID_PARAMETER;
            allocation_guest = input->host_allocation_base - WINE_LOW_VA_SHADOW_BASE;
            break;
        default:
            return STATUS_INVALID_PARAMETER;
        }

        if (input->state != MEM_FREE)
        {
            memset( &mapping, 0, sizeof(mapping) );
            mapping.guest = input_guest;
            mapping.host = input->host_address;
            mapping.size = input->size;
            mapping.allocation_base = allocation_guest;
            mapping.perms = input->state == MEM_COMMIT ?
                            protection_to_unicorn( input->protect ) : UC_PROT_NONE;
            mapping.state = input->state;
            mapping.domain = XTAJIT64_MEMORY_ADDRESS_AMD64_LOW;
            mapping.stale = FALSE;
            if (!range_array_append( result, &mapping )) return STATUS_INVALID_PARAMETER;
        }
        cursor = input_end;
    }
    return cursor == guest_end ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static NTSTATUS build_observer_replacement( const struct range_array *old,
                                             const struct range_array *captured,
                                             uint64_t start, uint64_t end,
                                             BOOL full_snapshot,
                                             struct range_array *result )
{
    struct range_array retained = {0};
    NTSTATUS status = STATUS_SUCCESS;
    size_t i;

    if (old->count > (SIZE_MAX - 1) / 2 ||
        !range_array_reserve( &retained, old->count * 2 + 1 ))
        return STATUS_NO_MEMORY;
    for (i = 0; i < old->count; ++i)
    {
        const struct mapped_range *range = &old->data[i];
        uint64_t range_end = range->guest + range->size;
        struct mapped_range slice;

        if (range->domain != XTAJIT64_MEMORY_ADDRESS_AMD64_LOW)
        {
            if (!range_array_append( &retained, range ))
            {
                status = STATUS_INVALID_ADDRESS;
                goto done;
            }
            continue;
        }
        if (range->permanent)
        {
            status = STATUS_INVALID_DEVICE_STATE;
            goto done;
        }
        if (full_snapshot) continue;
        if (!range_overlaps( range, start, end ))
        {
            if (!range_array_append( &retained, range ))
            {
                status = STATUS_INVALID_ADDRESS;
                goto done;
            }
            continue;
        }
        if (range->guest < start)
        {
            slice = range_slice( range, range->guest, start );
            if (!range_array_append( &retained, &slice ))
            {
                status = STATUS_INVALID_ADDRESS;
                goto done;
            }
        }
        if (range_end > end)
        {
            slice = range_slice( range, end, range_end );
            if (!range_array_append( &retained, &slice ))
            {
                status = STATUS_INVALID_ADDRESS;
                goto done;
            }
        }
    }
    status = merge_range_arrays( &retained, captured, result );

done:
    range_array_free( &retained );
    return status;
}

static int32_t arm64ec_low_observer_begin_callback(
    void *context, uint32_t operation, uint64_t host_address, uint64_t size,
    uint64_t host_allocation_base, void **transaction_ret )
{
    struct arm64ec_low_observer_transaction *transaction;
    volatile NTSTATUS status = STATUS_SUCCESS;
    volatile BOOL faulted = FALSE;
    BOOL claimed = FALSE;
    enum mutation_kind fault_kind = MUTATION_NONE;
    enum mutation_stage fault_stage = MUTATION_STAGE_IDLE;
    uint64_t fault_generation = 0, guest_start, guest_end;

    if (!transaction_ret) return STATUS_INVALID_PARAMETER;
    *transaction_ret = NULL;
    if (context != &provider || !observer_operation_is_valid( operation ) ||
        !low_host_interval_to_guest( host_address, size, &guest_start, &guest_end ) ||
        !low_host_allocation_base_is_valid( host_allocation_base ))
        return STATUS_INVALID_PARAMETER;
    if (!(transaction = calloc( 1, sizeof(*transaction) ))) return STATUS_NO_MEMORY;

    pthread_mutex_lock( &provider.mutex );
    if (current_thread_owns_mutation_locked() || provider.observer_transaction)
        status = STATUS_INVALID_DEVICE_STATE;
    else
    {
        status = claim_mutation_locked( observer_mutation_kind( operation ), FALSE );
        claimed = !status;
    }
    if (!status) __TRY
    {
        pause_mutation_engines_locked();
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted && !status && claimed && current_thread_owns_mutation_locked())
        status = wait_for_mutation_engines_locked();
    if (!faulted && !status)
    {
        transaction->generation = provider.generation;
        transaction->operation = operation;
        provider.observer_transaction = transaction;
        *transaction_ret = transaction;
    }
    else if (!faulted && claimed && current_thread_owns_mutation_locked())
        finish_mutation_locked();
    if (faulted)
    {
        fault_kind = provider.last_fault_kind;
        fault_stage = provider.last_fault_stage;
        fault_generation = provider.last_fault_generation;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (faulted)
        report_mutation_access_violation( fault_kind, fault_stage, fault_generation );
    if (status) free( transaction );
    return status;
}

static void arm64ec_low_observer_complete_callback(
    void *context, void *token,
    const struct wine_arm64ec_low_memory_event_v1 *event )
{
    struct arm64ec_low_observer_transaction *transaction = NULL;
    struct range_array captured = {0}, replacement = {0};
    volatile NTSTATUS status = STATUS_SUCCESS;
    volatile BOOL faulted = FALSE;
    enum mutation_kind fault_kind = MUTATION_NONE;
    enum mutation_stage fault_stage = MUTATION_STAGE_IDLE;
    uint64_t fault_generation = 0, guest_start = 0, guest_end = 0;
    uint64_t free_bytes = 0, reserve_bytes = 0, commit_bytes = 0;
    uint64_t free_ranges = 0, reserve_ranges = 0, commit_ranges = 0;
    BOOL full_snapshot = FALSE;
    size_t i;

    pthread_mutex_lock( &provider.mutex );
    transaction = provider.observer_transaction;
    if (context != &provider || !token || token != transaction ||
        !transaction || !current_thread_owns_mutation_locked() ||
        transaction->generation != provider.generation)
    {
        status = STATUS_INVALID_DEVICE_STATE;
        poison_provider_locked( status );
        pthread_mutex_unlock( &provider.mutex );
        WARN( "rejected non-owning ARM64EC LOW memory completion\n" );
        return;
    }
    if (!status) __TRY
    {
        status = validate_observer_event_header( event, transaction,
                                                 &guest_start, &guest_end,
                                                 &full_snapshot );
        if (!status && event->snapshot_status) status = event->snapshot_status;
        if (!status)
            status = build_observer_ranges( event, guest_start, guest_end, &captured );
        if (!status)
            status = build_observer_replacement( &provider.ranges, &captured,
                                                 guest_start, guest_end,
                                                 full_snapshot, &replacement );
        if (!status) set_mutation_stage_locked( MUTATION_STAGE_APPLY );
        if (!status)
        {
            struct range_array old = provider.ranges;

            set_mutation_stage_locked( MUTATION_STAGE_PUBLISH );
            mark_engine_mappings_stale_locked( guest_start,
                                               guest_end - guest_start, 0 );
            for (i = 0; i < (size_t)event->range_count; ++i)
            {
                switch (event->ranges[i].state)
                {
                case MEM_FREE:
                    ++free_ranges;
                    free_bytes += event->ranges[i].size;
                    break;
                case MEM_RESERVE:
                    ++reserve_ranges;
                    reserve_bytes += event->ranges[i].size;
                    break;
                case MEM_COMMIT:
                    ++commit_ranges;
                    commit_bytes += event->ranges[i].size;
                    break;
                }
            }
            provider.ranges = replacement;
            memset( &replacement, 0, sizeof(replacement) );
            ++provider.generation;
            if (full_snapshot) provider.observer_active = TRUE;
            TRACE( "published ARM64EC LOW event operation %u mutation status %#x, "
                   "full %u covered %#llx ranges %llu, free %llu/%#llx "
                   "reserve %llu/%#llx commit %llu/%#llx generation %llu\n",
                   event->operation,
                   (unsigned int)event->status, full_snapshot,
                   (unsigned long long)event->size_covered,
                   (unsigned long long)event->range_count,
                   (unsigned long long)free_ranges,
                   (unsigned long long)free_bytes,
                   (unsigned long long)reserve_ranges,
                   (unsigned long long)reserve_bytes,
                   (unsigned long long)commit_ranges,
                   (unsigned long long)commit_bytes,
                   (unsigned long long)provider.generation );
            range_array_free( &old );
        }
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted)
    {
        if (status) poison_provider_locked( status );
        if (provider.mutating) finish_mutation_locked();
    }
    if (faulted)
    {
        fault_kind = provider.last_fault_kind;
        fault_stage = provider.last_fault_stage;
        fault_generation = provider.last_fault_generation;
    }
    provider.observer_transaction = NULL;
    pthread_mutex_unlock( &provider.mutex );

    if (faulted)
        report_mutation_access_violation( fault_kind, fault_stage, fault_generation );
    else if (status)
        WARN( "cannot publish ARM64EC LOW memory event status %#x\n",
              (unsigned int)status );
    range_array_free( &captured );
    range_array_free( &replacement );
    free( transaction );
}

static const struct wine_arm64ec_low_memory_observer_v1 arm64ec_low_memory_observer =
{
    WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION,
    sizeof(arm64ec_low_memory_observer),
    &provider,
    arm64ec_low_observer_begin_callback,
    arm64ec_low_observer_complete_callback,
    WINE_ARM64EC_LOW_MEMORY_OBSERVER_CAP_EXACT_POST_SNAPSHOT,
};

static int32_t register_xtajit64_memory_observer(void)
{
#ifdef XTAJIT64_UNIXLIB_TEST
    struct wine_arm64ec_low_memory_range_v1 range =
    {
        WINE_LOW_VA_SHADOW_BASE,
        WINE_LOW_VA_SHADOW_SIZE,
        0,
        MEM_FREE,
        PAGE_NOACCESS,
        0,
        0,
    };
    struct wine_arm64ec_low_memory_event_v1 event =
    {
        WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION,
        sizeof(event),
        WINE_WOW64_MEMORY_RESYNC,
        WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT,
        STATUS_SUCCESS,
        STATUS_SUCCESS,
        {0, 0},
        WINE_LOW_VA_SHADOW_BASE,
        WINE_LOW_VA_SHADOW_SIZE,
        0,
        &range,
        1,
    };
    void *transaction = NULL;
    int32_t status;

    status = arm64ec_low_memory_observer.begin( arm64ec_low_memory_observer.context,
                                                WINE_WOW64_MEMORY_RESYNC,
                                                WINE_LOW_VA_SHADOW_BASE,
                                                WINE_LOW_VA_SHADOW_SIZE, 0,
                                                &transaction );
    if (!status)
        arm64ec_low_memory_observer.complete( arm64ec_low_memory_observer.context,
                                              transaction, &event );
    return status;
#else
    return __wine_register_arm64ec_low_memory_observer_v1(
        &arm64ec_low_memory_observer );
#endif
}

static NTSTATUS process_init( void *args )
{
    struct xtajit64_process_init_params *params = args;
    struct mapped_range kuser;
    NTSTATUS status;
    unsigned int major, minor;

    TRACE( "CPU provider interface %s\n", XTAJIT64_PROVIDER_ABI_IDENTITY );
    if (!params) return STATUS_INVALID_PARAMETER;
    if (params->abi_version != XTAJIT64_PROCESS_ABI_VERSION ||
        params->abi_size != sizeof(*params) ||
        (params->required_capabilities & XTAJIT64_CAPABILITIES) != XTAJIT64_CAPABILITIES ||
        (params->required_capabilities & ~XTAJIT64_CAPABILITIES) ||
        params->enabled_capabilities)
        return STATUS_REVISION_MISMATCH;
    if (!params->ec_bitmap || (params->ec_bitmap & (sizeof(uint64_t) - 1)) ||
        !params->highest_user_address ||
        params->highest_user_address > XTAJIT64_X64_USER_ADDRESS_MAX ||
        !params->rtl_exit_user_thread ||
        params->rtl_exit_user_thread > XTAJIT64_X64_USER_ADDRESS_MAX ||
        params->rtl_exit_user_thread > params->highest_user_address ||
        !params->x64_syscall_dispatcher ||
        params->x64_syscall_dispatcher > XTAJIT64_X64_USER_ADDRESS_MAX ||
        params->x64_syscall_dispatcher > params->highest_user_address ||
        !params->x64_syscall_count ||
        params->x64_syscall_count > XTAJIT64_MAX_SYSCALL_COUNT || params->reserved ||
        params->guest_kuser != XTAJIT64_GUEST_KUSER || !params->host_kuser ||
        params->kuser_size < XTAJIT64_GUEST_PAGE_SIZE ||
        params->kuser_size > XTAJIT64_MAX_HOST_PAGE_SIZE ||
        (params->kuser_size & (params->kuser_size - 1)) ||
        (params->guest_kuser & (params->kuser_size - 1)) ||
        (params->host_kuser & (params->kuser_size - 1)) ||
        params->guest_kuser > params->highest_user_address ||
        params->kuser_size - 1 > params->highest_user_address - params->guest_kuser ||
        params->host_kuser > UINT64_MAX - params->kuser_size)
        return STATUS_INVALID_PARAMETER;

    pthread_once( &engine_key_once, make_engine_key );
    if (engine_key_error) return STATUS_NO_MEMORY;
    uc_version( &major, &minor );
    if (major != UC_API_MAJOR || (major == 2 && minor < 1))
    {
        ERR( "unsupported Unicorn API %u.%u\n", major, minor );
        return STATUS_REVISION_MISMATCH;
    }

    pthread_mutex_lock( &provider.mutex );
    while (provider.shutting_down)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (provider.initialized)
    {
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_ALREADY_INITIALIZED;
    }
    provider.ec_bitmap = (const uint64_t *)(uintptr_t)params->ec_bitmap;
    provider.ec_page_size = params->kuser_size;
    provider.highest_user_address = params->highest_user_address;
    provider.rtl_exit_user_thread = params->rtl_exit_user_thread;
    provider.x64_syscall_dispatcher = params->x64_syscall_dispatcher;
    provider.x64_syscall_count = params->x64_syscall_count;
    provider.guest_kuser = params->guest_kuser;
    provider.host_kuser = params->host_kuser;
    provider.kuser_size = params->kuser_size;
    provider.next_diagnostic_id = 0;
    provider.next_binding_id = 0;
    provider.engine_count = 0;
    provider.engines_in_use = 0;
    provider.engine_high_water = 0;
    if (!++provider.instance) ++provider.instance;
    provider.poison_status = STATUS_SUCCESS;
    provider.last_fault_kind = MUTATION_NONE;
    provider.last_fault_stage = MUTATION_STAGE_IDLE;
    provider.last_fault_generation = 0;
    provider.shutting_down = FALSE;
    provider.observer_active = FALSE;
    provider.observer_transaction = NULL;
    provider.generation = 1;

    kuser.guest = params->guest_kuser;
    kuser.host = params->host_kuser;
    kuser.size = params->kuser_size;
    kuser.allocation_base = params->guest_kuser;
    kuser.perms = UC_PROT_READ;
    kuser.state = MEM_COMMIT;
    kuser.domain = XTAJIT64_MEMORY_ADDRESS_INVALID;
    kuser.flags = 0;
    kuser.permanent = TRUE;
    kuser.stale = FALSE;
    if (!range_array_append( &provider.ranges, &kuser ))
    {
        range_array_free( &provider.ranges );
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_NO_MEMORY;
    }
    provider.initialized = TRUE;
    pthread_mutex_unlock( &provider.mutex );

    status = register_xtajit64_memory_observer();
    if (status)
    {
        process_term( NULL );
        return status;
    }
    pthread_mutex_lock( &provider.mutex );
    if (provider.poison_status) status = provider.poison_status;
    else if (!provider.observer_active)
    {
        status = STATUS_INVALID_DEVICE_STATE;
        poison_provider_locked( status );
    }
    else
    {
        params->enabled_capabilities = XTAJIT64_CAPABILITIES;
#ifndef XTAJIT64_UNIXLIB_TEST
        TRACE_(xtajitmap)( "schema 1 initialized\n" );
#endif
        TRACE( "initialized Unicorn %u.%u provider registry, KUSER guest %p host %p, "
               "RtlExitUserThread %p, x64 syscall dispatcher %p count %u\n", major, minor,
               (void *)(uintptr_t)params->guest_kuser,
               (void *)(uintptr_t)params->host_kuser,
               (void *)(uintptr_t)params->rtl_exit_user_thread,
               (void *)(uintptr_t)params->x64_syscall_dispatcher,
               params->x64_syscall_count );
    }
    pthread_mutex_unlock( &provider.mutex );
    if (status)
    {
        process_term( NULL );
        return status;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS process_term( void *args )
{
    struct thread_engine *engine, *next;
    (void)args;
    pthread_mutex_lock( &provider.mutex );
    if (!provider.initialized)
    {
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_SUCCESS;
    }
    while (provider.mutating) pthread_cond_wait( &provider.cond, &provider.mutex );
    provider.mutating = TRUE;
    provider.shutting_down = TRUE;
    ++provider.generation;
    for (engine = provider.engines; engine; engine = engine->next)
        request_engine_pause_locked( engine );
    while (any_engine_running_locked())
        pthread_cond_wait( &provider.cond, &provider.mutex );

    /* A simulation paused for this mutation still owns its engine while it
     * waits to discover the terminal provider state.  Publish that state first,
     * then wait for every borrower to return before closing pooled engines. */
    provider.initialized = FALSE;
    pthread_cond_broadcast( &provider.cond );
    while (any_engine_in_use_locked())
        pthread_cond_wait( &provider.cond, &provider.mutex );

    for (engine = provider.engines; engine; engine = next)
    {
        next = engine->next;
        trace_mapping_diagnostic( engine, "final", 0 );
        if (engine->uc) uc_close( engine->uc );
        range_array_free( &engine->mapped_ranges );
        free( engine );
    }
    provider.engines = NULL;
    provider.engine_count = 0;
    provider.engines_in_use = 0;
    provider.engine_high_water = 0;
    if (provider.initial_context) uc_context_free( provider.initial_context );
    provider.initial_context = NULL;
    range_array_free( &provider.ranges );
    provider.observer_active = FALSE;
    provider.observer_transaction = NULL;
    provider.ec_bitmap = NULL;
    provider.ec_page_size = 0;
    provider.highest_user_address = 0;
    provider.rtl_exit_user_thread = 0;
    provider.x64_syscall_dispatcher = 0;
    provider.x64_syscall_count = 0;
    provider.guest_kuser = 0;
    provider.host_kuser = 0;
    provider.kuser_size = 0;
    provider.poison_status = STATUS_SUCCESS;
    provider.shutting_down = FALSE;
    finish_mutation_locked();
    pthread_mutex_unlock( &provider.mutex );
    return STATUS_SUCCESS;
}

static NTSTATUS thread_init( void *args )
{
    struct thread_binding *binding, *current;
    NTSTATUS status = STATUS_SUCCESS;
    uc_err err;
    int ret;

    (void)args;
    pthread_once( &engine_key_once, make_engine_key );
    if (engine_key_error) return STATUS_NO_MEMORY;
    current = pthread_getspecific( engine_key );
    if (!(binding = calloc( 1, sizeof(*binding) ))) return STATUS_NO_MEMORY;

    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down)
        status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else if (current && current->process_instance == provider.instance)
        status = STATUS_SUCCESS;
    else if (!provider.engines &&
             (err = create_pool_engine_locked( NULL )) != UC_ERR_OK)
    {
        WARN( "cannot initialize pooled x64 engine: %s\n", uc_strerror( err ) );
        status = STATUS_NOT_SUPPORTED;
    }
    if (!status && (!current || current->process_instance != provider.instance))
    {
        binding->process_instance = provider.instance;
        if (!(binding->id = ++provider.next_binding_id))
            binding->id = ++provider.next_binding_id;
        if ((ret = pthread_setspecific( engine_key, binding )))
            status = ret == ENOMEM ? STATUS_NO_MEMORY : STATUS_UNSUCCESSFUL;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (!status && current && current->process_instance == provider.instance)
        free( binding );
    else if (status) free( binding );
    else if (current) destroy_thread_binding( current );
    return status;
}

static NTSTATUS thread_term( void *args )
{
    struct thread_binding *binding;

    (void)args;
    pthread_once( &engine_key_once, make_engine_key );
    if (engine_key_error) return STATUS_UNSUCCESSFUL;
    if (!(binding = pthread_getspecific( engine_key ))) return STATUS_SUCCESS;
    if (binding->active) return STATUS_INVALID_DEVICE_STATE;
    pthread_setspecific( engine_key, NULL );
    destroy_thread_binding( binding );
    return STATUS_SUCCESS;
}

static NTSTATUS memory_map_internal( void *args )
{
    const struct xtajit64_memory_params *params = args;
    struct mapped_range mapping;
    struct range_array replacement = {0};
    uint64_t start, end, host_start, host_end;
    volatile NTSTATUS status = STATUS_SUCCESS;
    volatile BOOL faulted = FALSE;
    BOOL report_error = FALSE;
    enum mutation_kind fault_kind = MUTATION_NONE;
    enum mutation_stage fault_stage = MUTATION_STAGE_IDLE;
    uint64_t fault_generation = 0;

    if (!params || params->flags || !params->guest || !params->host || !params->size ||
        !params->allocation_base ||
        !align_range( params->guest, params->size, &start, &end ) ||
        !align_range( params->host, params->size, &host_start, &host_end ) ||
        end - start != host_end - host_start ||
        end - 1 > XTAJIT64_X64_USER_ADDRESS_MAX ||
        ((start ^ host_start) & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
        (params->allocation_base & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
        params->allocation_base > start || start != host_start)
        return STATUS_INVALID_PARAMETER;

    mapping.guest = start;
    mapping.host = host_start;
    mapping.size = end - start;
    mapping.allocation_base = params->allocation_base;
    mapping.perms = protection_to_unicorn( params->protect );
    mapping.state = MEM_COMMIT;
    mapping.domain = XTAJIT64_MEMORY_ADDRESS_IDENTITY;
    mapping.flags = 0;
    mapping.permanent = FALSE;
    mapping.stale = FALSE;

    pthread_mutex_lock( &provider.mutex );
    if (legacy_mutation_selects_low_locked( start, end - start ))
        status = STATUS_ACCESS_DENIED;
    else status = claim_mutation_locked( MUTATION_MAP, TRUE );
    if (!status) __TRY
    {
        pause_mutation_engines_locked();
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted && current_thread_owns_mutation_locked())
        status = wait_for_mutation_engines_locked();
    if (!faulted && !status) __TRY
    {
        test_mutation_fault_checkpoint( TEST_MUTATION_FAULT_AFTER_BEGIN );
        if (!status)
            status = build_mapped_registry( &provider.ranges, &mapping, &replacement );
        if (!status) set_mutation_stage_locked( MUTATION_STAGE_APPLY );
        if (!status)
        {
            struct range_array old = provider.ranges;

            set_mutation_stage_locked( MUTATION_STAGE_PUBLISH );
            provider.ranges = replacement;
            memset( &replacement, 0, sizeof(replacement) );
            range_array_free( &old );
        }
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted)
    {
        report_error = status && status != STATUS_ACCESS_DENIED && provider.initialized;
        if (report_error) poison_provider_locked( status );
        if (current_thread_owns_mutation_locked()) finish_mutation_locked();
    }
    if (faulted)
    {
        fault_kind = provider.last_fault_kind;
        fault_stage = provider.last_fault_stage;
        fault_generation = provider.last_fault_generation;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (faulted)
        report_mutation_access_violation( fault_kind, fault_stage, fault_generation );
    else
    {
        if (report_error)
            WARN( "cannot map guest %p size %#llx: status %#x\n",
                  (void *)(uintptr_t)start, (unsigned long long)(end - start),
                  (unsigned int)status );
        range_array_free( &replacement );
    }
    return status;
}

static NTSTATUS memory_map( void *args )
{
    return memory_map_internal( args );
}

static BOOL legacy_mutation_selects_low_locked( uint64_t address, uint64_t size )
{
    uint64_t end = size ? address + size : 0;
    size_t i;

    for (i = 0; i < provider.ranges.count; ++i)
    {
        const struct mapped_range *range = &provider.ranges.data[i];
        uint64_t host_end, host_allocation_base;

        if (range->domain != XTAJIT64_MEMORY_ADDRESS_AMD64_LOW) continue;
        if (size)
        {
            if (range_overlaps( range, address, end )) return TRUE;
            host_end = range->host + range->size;
            if (range->host < end && address < host_end) return TRUE;
        }
        else
        {
            host_allocation_base = range->allocation_base + WINE_LOW_VA_SHADOW_BASE;
            if (range->allocation_base == address || host_allocation_base == address)
                return TRUE;
        }
    }
    return FALSE;
}

static NTSTATUS memory_unmap( void *args )
{
    const struct xtajit64_memory_params *params = args;
    struct range_array replacement = {0};
    uint64_t guest = 0, size = 0;
    volatile NTSTATUS status = STATUS_SUCCESS;
    volatile BOOL faulted = FALSE;
    BOOL report_error = FALSE;
    enum mutation_kind fault_kind = MUTATION_NONE;
    enum mutation_stage fault_stage = MUTATION_STAGE_IDLE;
    uint64_t fault_generation = 0;

    if (!params || (params->flags & ~XTAJIT64_MEMORY_VALID_FLAGS) ||
        !params->guest || params->guest > XTAJIT64_X64_USER_ADDRESS_MAX)
        return STATUS_INVALID_PARAMETER;
    if (params->size)
    {
        uint64_t end;

        if (!align_range( params->guest, params->size, &guest, &end ))
            return STATUS_INVALID_PARAMETER;
        if (end - 1 > XTAJIT64_X64_USER_ADDRESS_MAX)
            return STATUS_INVALID_PARAMETER;
        size = end - guest;
    }
    else
    {
        guest = align_down( params->guest );
        size = 0;
    }

    pthread_mutex_lock( &provider.mutex );
    if (legacy_mutation_selects_low_locked( guest, size ))
        status = STATUS_ACCESS_DENIED;
    else status = claim_mutation_locked( MUTATION_UNMAP, TRUE );
    if (!status) __TRY
    {
        pause_mutation_engines_locked();
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted && current_thread_owns_mutation_locked())
        status = wait_for_mutation_engines_locked();
    if (!faulted && !status) __TRY
    {
        test_mutation_fault_checkpoint( TEST_MUTATION_FAULT_AFTER_BEGIN );
        if (!status)
            status = build_unmapped_registry( &provider.ranges, guest, size, guest, &replacement );
        if (!status) set_mutation_stage_locked( MUTATION_STAGE_APPLY );
        if (!status)
        {
            struct range_array old = provider.ranges;

            set_mutation_stage_locked( MUTATION_STAGE_PUBLISH );
            mark_engine_mappings_stale_locked( guest, size, guest );
            provider.ranges = replacement;
            memset( &replacement, 0, sizeof(replacement) );
            range_array_free( &old );
        }
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted)
    {
        report_error = status && status != STATUS_ACCESS_DENIED && provider.initialized;
        if (report_error) poison_provider_locked( status );
        if (current_thread_owns_mutation_locked()) finish_mutation_locked();
    }
    if (faulted)
    {
        fault_kind = provider.last_fault_kind;
        fault_stage = provider.last_fault_stage;
        fault_generation = provider.last_fault_generation;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (faulted)
        report_mutation_access_violation( fault_kind, fault_stage, fault_generation );
    else
    {
        if (report_error)
            WARN( "cannot unmap guest %p size %#llx: status %#x\n",
                  (void *)(uintptr_t)guest, (unsigned long long)size,
                  (unsigned int)status );
        range_array_free( &replacement );
    }
    return status;
}

static NTSTATUS memory_protect( void *args )
{
    const struct xtajit64_memory_params *params = args;
    struct range_array replacement = {0};
    uint64_t start = 0, end = 0;
    unsigned int perms;
    volatile NTSTATUS status = STATUS_SUCCESS;
    volatile BOOL faulted = FALSE;
    BOOL report_error = FALSE;
    enum mutation_kind fault_kind = MUTATION_NONE;
    enum mutation_stage fault_stage = MUTATION_STAGE_IDLE;
    uint64_t fault_generation = 0;

    if (!params || (params->flags & ~XTAJIT64_MEMORY_VALID_FLAGS) ||
        !params->guest || !params->size)
        return STATUS_INVALID_PARAMETER;
    if (!align_range( params->guest, params->size, &start, &end ))
        return STATUS_INVALID_PARAMETER;
    if (end - 1 > XTAJIT64_X64_USER_ADDRESS_MAX)
        return STATUS_INVALID_PARAMETER;
    perms = protection_to_unicorn( params->protect );

    pthread_mutex_lock( &provider.mutex );
    if (legacy_mutation_selects_low_locked( start, end - start ))
        status = STATUS_ACCESS_DENIED;
    else status = claim_mutation_locked( MUTATION_PROTECT, TRUE );
    if (!status) __TRY
    {
        pause_mutation_engines_locked();
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted && current_thread_owns_mutation_locked())
        status = wait_for_mutation_engines_locked();
    if (!faulted && !status) __TRY
    {
        test_mutation_fault_checkpoint( TEST_MUTATION_FAULT_AFTER_BEGIN );
        if (!status && !registry_covers_range( &provider.ranges, start, end ))
            status = STATUS_INVALID_ADDRESS;
        if (!status)
            status = build_protected_registry( &provider.ranges, start, end, perms,
                                               &replacement );
        if (!status) set_mutation_stage_locked( MUTATION_STAGE_APPLY );
        if (!status)
        {
            struct range_array old = provider.ranges;

            set_mutation_stage_locked( MUTATION_STAGE_PUBLISH );
            provider.ranges = replacement;
            memset( &replacement, 0, sizeof(replacement) );
            range_array_free( &old );
        }
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted)
    {
        report_error = status && status != STATUS_ACCESS_DENIED && provider.initialized;
        if (report_error) poison_provider_locked( status );
        if (current_thread_owns_mutation_locked()) finish_mutation_locked();
    }
    if (faulted)
    {
        fault_kind = provider.last_fault_kind;
        fault_stage = provider.last_fault_stage;
        fault_generation = provider.last_fault_generation;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (faulted)
        report_mutation_access_violation( fault_kind, fault_stage, fault_generation );
    else
    {
        if (report_error)
            WARN( "cannot protect guest %p-%p: status %#x\n",
                  (void *)(uintptr_t)start, (void *)(uintptr_t)end,
                  (unsigned int)status );
        range_array_free( &replacement );
    }
    return status;
}

static int compare_memory_params( const void *left, const void *right )
{
    const struct xtajit64_memory_params *a = left, *b = right;

    if (a->guest < b->guest) return -1;
    if (a->guest > b->guest) return 1;
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;
    return 0;
}

static NTSTATUS memory_resync_begin( void *args )
{
    struct xtajit64_memory_resync_begin_params *params = args;
    NTSTATUS status = STATUS_SUCCESS;

    /* The PE side must inspect the host address space without holding this
     * mutex.  Its commit is accepted only if no incremental notification has
     * advanced the canonical generation while that snapshot was collected. */
    if (!params) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
    {
#ifdef XTAJIT64_UNIXLIB_TEST
        atomic_fetch_add_explicit( &test_mutation_waiters, 1, memory_order_release );
#endif
        pthread_cond_wait( &provider.cond, &provider.mutex );
#ifdef XTAJIT64_UNIXLIB_TEST
        atomic_fetch_sub_explicit( &test_mutation_waiters, 1, memory_order_release );
#endif
    }
    if (!provider.initialized || provider.shutting_down) status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else params->generation = provider.generation;
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static NTSTATUS build_resync_registry( const struct xtajit64_memory_resync_params *params,
                                       struct range_array *result )
{
    const struct xtajit64_memory_params *input;
    struct xtajit64_memory_params *copy = NULL;
    struct mapped_range kuser, range;
    struct range_array retained = {0}, merged = {0};
    uint64_t start, end, host_start, host_end, previous_end = 0;
    size_t i;
    BOOL inserted_kuser = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (!params || params->reserved || params->count > XTAJIT64_MAX_RESYNC_RANGES ||
        (params->count && !params->ranges))
        return STATUS_INVALID_PARAMETER;
    input = (const struct xtajit64_memory_params *)(uintptr_t)params->ranges;
    if (params->count)
    {
        if (!(copy = malloc( params->count * sizeof(*copy) )))
            return STATUS_NO_MEMORY;
        memcpy( copy, input, params->count * sizeof(*copy) );
        qsort( copy, params->count, sizeof(*copy), compare_memory_params );
    }

    kuser.guest = provider.guest_kuser;
    kuser.host = provider.host_kuser;
    kuser.size = provider.kuser_size;
    kuser.allocation_base = provider.guest_kuser;
    kuser.perms = UC_PROT_READ;
    kuser.state = MEM_COMMIT;
    kuser.domain = XTAJIT64_MEMORY_ADDRESS_INVALID;
    kuser.flags = 0;
    kuser.permanent = TRUE;
    kuser.stale = FALSE;
    if (!range_array_reserve( result, params->count + 1 ))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }

    for (i = 0; i < params->count; ++i)
    {
        if (!copy[i].guest || !copy[i].host || !copy[i].size ||
            !copy[i].allocation_base || copy[i].flags ||
            !align_range( copy[i].guest, copy[i].size, &start, &end ) ||
            !align_range( copy[i].host, copy[i].size, &host_start, &host_end ) ||
            end - start != host_end - host_start ||
            end - 1 > provider.highest_user_address ||
            start != host_start ||
            ((start ^ host_start) & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
            (copy[i].allocation_base & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
            copy[i].allocation_base > start ||
            start < previous_end)
        {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
        if (start < kuser.guest + kuser.size && kuser.guest < end)
        {
            status = STATUS_ACCESS_DENIED;
            goto done;
        }
        if (!inserted_kuser && start > kuser.guest)
        {
            if (!range_array_append( result, &kuser ))
            {
                status = STATUS_NO_MEMORY;
                goto done;
            }
            inserted_kuser = TRUE;
        }
        range.guest = start;
        range.host = host_start;
        range.size = end - start;
        range.allocation_base = copy[i].allocation_base;
        range.perms = protection_to_unicorn( copy[i].protect );
        range.state = MEM_COMMIT;
        range.domain = XTAJIT64_MEMORY_ADDRESS_IDENTITY;
        range.flags = 0;
        range.permanent = FALSE;
        range.stale = FALSE;
        if (!range_array_append( result, &range ))
        {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
        previous_end = end;
    }
    if (!inserted_kuser && !range_array_append( result, &kuser )) status = STATUS_NO_MEMORY;
    if (status) goto done;

    if (!range_array_reserve( &retained, provider.ranges.count ))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    for (i = 0; i < provider.ranges.count; ++i)
    {
        const struct mapped_range *old = &provider.ranges.data[i];

        if (old->domain != XTAJIT64_MEMORY_ADDRESS_AMD64_LOW) continue;
        if (!range_array_append( &retained, old ))
        {
            status = STATUS_INVALID_ADDRESS;
            goto done;
        }
    }
    if ((status = merge_range_arrays( result, &retained, &merged ))) goto done;
    range_array_free( result );
    *result = merged;
    memset( &merged, 0, sizeof(merged) );

done:
    free( copy );
    range_array_free( &retained );
    range_array_free( &merged );
    return status;
}

static BOOL ranges_have_same_engine_mapping( const struct mapped_range *left,
                                             const struct mapped_range *right,
                                             uint64_t guest )
{
    uint64_t left_offset, right_offset;

    if (guest < left->guest || guest < right->guest) return FALSE;
    left_offset = guest - left->guest;
    right_offset = guest - right->guest;
    return left->state == MEM_COMMIT && right->state == MEM_COMMIT &&
           left->perms == right->perms &&
           left->host <= UINT64_MAX - left_offset &&
           right->host <= UINT64_MAX - right_offset &&
           left->host + left_offset == right->host + right_offset;
}

static NTSTATUS append_resync_mapping_changes( const struct range_array *source,
                                                const struct range_array *reference,
                                                struct range_array *changes )
{
    size_t i, reference_index = 0;

    if (source->count > SIZE_MAX - reference->count ||
        !range_array_reserve( changes, source->count + reference->count ))
        return STATUS_NO_MEMORY;
    for (i = 0; i < source->count; ++i)
    {
        const struct mapped_range *range = &source->data[i];
        uint64_t cursor, end;
        size_t j;

        if (range->state != MEM_COMMIT) continue;
        cursor = range->guest;
        end = cursor + range->size;
        while (reference_index < reference->count &&
               reference->data[reference_index].guest +
                   reference->data[reference_index].size <= cursor)
            ++reference_index;
        j = reference_index;
        while (cursor < end)
        {
            const struct mapped_range *other;
            struct mapped_range slice;
            uint64_t next;

            while (j < reference->count &&
                   reference->data[j].guest + reference->data[j].size <= cursor)
                ++j;
            if (j == reference->count || reference->data[j].guest >= end)
                next = end;
            else if (reference->data[j].guest > cursor)
                next = min( end, reference->data[j].guest );
            else
            {
                other = &reference->data[j];
                next = min( end, other->guest + other->size );
                if (ranges_have_same_engine_mapping( range, other, cursor ))
                {
                    cursor = next;
                    continue;
                }
            }
            slice = range_slice( range, cursor, next );
            if (!range_array_append( changes, &slice ))
                return STATUS_INVALID_ADDRESS;
            cursor = next;
        }
        reference_index = j;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS build_resync_mapping_changes( const struct range_array *old,
                                               const struct range_array *replacement,
                                               struct range_array *removals,
                                               struct range_array *additions )
{
    NTSTATUS status;

    if ((status = append_resync_mapping_changes( old, replacement, removals )))
        return status;
    return append_resync_mapping_changes( replacement, old, additions );
}

static uc_err synchronize_engine_registry_locked( struct thread_engine *engine )
{
    struct range_array retained = {0}, old;
    size_t i;
    uc_err err = UC_ERR_OK;

    if (engine->mapping_generation == provider.generation) return UC_ERR_OK;
    ++engine->registry_sync_calls;
    if (!range_array_reserve( &retained, engine->mapped_ranges.count ))
        return UC_ERR_NOMEM;

    for (i = 0; i < engine->mapped_ranges.count; ++i)
    {
        const struct mapped_range *mapped = &engine->mapped_ranges.data[i];
        const struct mapped_range *canonical =
            find_canonical_mapping( mapped->guest, mapped->size, mapped->perms );
        uint64_t offset = canonical ? mapped->guest - canonical->guest : 0;

        if (!mapped->stale && canonical &&
            canonical->host <= UINT64_MAX - offset &&
            canonical->host + offset == mapped->host &&
            canonical->allocation_base == mapped->allocation_base &&
            canonical->perms == mapped->perms &&
            canonical->state == mapped->state &&
            canonical->domain == mapped->domain &&
            canonical->flags == mapped->flags &&
            canonical->permanent == mapped->permanent)
        {
            if (!range_array_append( &retained, mapped ))
            {
                err = UC_ERR_ARG;
                goto done;
            }
            continue;
        }
        if ((err = unmap_range( engine, mapped->guest, mapped->size )) != UC_ERR_OK)
            goto done;
        /* uc_mem_unmap() invalidates translated blocks and TLB entries for
         * every affected mapped region.  A full-engine flush here would
         * discard unrelated translations on every generation change. */
        ++engine->resync_unmap_calls;
        engine->resync_unmap_bytes += mapped->size;
    }

    old = engine->mapped_ranges;
    engine->mapped_ranges = retained;
    memset( &retained, 0, sizeof(retained) );
    range_array_free( &old );
    engine->mapping_generation = provider.generation;
    if (!(engine->registry_sync_calls & (engine->registry_sync_calls - 1)))
        trace_mapping_diagnostic( engine, "sync", 0 );

done:
    range_array_free( &retained );
    return err;
}

static NTSTATUS memory_resync( void *args )
{
    const struct xtajit64_memory_resync_params *params = args;
    struct range_array replacement = {0}, removals = {0}, additions = {0};
    volatile NTSTATUS status = STATUS_SUCCESS;
    volatile BOOL faulted = FALSE;
    BOOL report_error = FALSE;
    enum mutation_kind fault_kind = MUTATION_NONE;
    enum mutation_stage fault_stage = MUTATION_STAGE_IDLE;
    uint64_t fault_generation = 0;
    uint64_t completed_generation = 0;
    size_t completed_range_count = 0;
    size_t completed_removal_count = 0, completed_addition_count = 0;

    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!params) status = STATUS_INVALID_PARAMETER;
    else if (!provider.initialized || provider.shutting_down) status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else if (params->generation != provider.generation) status = STATUS_RETRY;
    else status = claim_mutation_locked( MUTATION_RESYNC, TRUE );
    if (!status) __TRY
    {
        pause_mutation_engines_locked();
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted && current_thread_owns_mutation_locked())
        status = wait_for_mutation_engines_locked();
    if (!faulted && !status) __TRY
    {
        test_mutation_fault_checkpoint( TEST_MUTATION_FAULT_AFTER_BEGIN );
        if (!status) status = build_resync_registry( params, &replacement );
        if (!status)
            status = build_resync_mapping_changes( &provider.ranges, &replacement,
                                                   &removals, &additions );
        if (!status) set_mutation_stage_locked( MUTATION_STAGE_APPLY );
        if (!status)
        {
            struct range_array old = provider.ranges;
            size_t i;

            set_mutation_stage_locked( MUTATION_STAGE_PUBLISH );
            for (i = 0; i < removals.count; ++i)
                mark_engine_mappings_stale_locked( removals.data[i].guest,
                                                   removals.data[i].size, 0 );
            provider.ranges = replacement;
            memset( &replacement, 0, sizeof(replacement) );
            range_array_free( &old );
        }
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted)
    {
        report_error = status && provider.initialized && status != STATUS_RETRY;
        if (report_error) poison_provider_locked( status );
        if (!status)
        {
            completed_range_count = provider.ranges.count;
            completed_generation = provider.generation;
            completed_removal_count = removals.count;
            completed_addition_count = additions.count;
        }
        if (current_thread_owns_mutation_locked()) finish_mutation_locked();
    }
    if (faulted)
    {
        fault_kind = provider.last_fault_kind;
        fault_stage = provider.last_fault_stage;
        fault_generation = provider.last_fault_generation;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (faulted)
        report_mutation_access_violation( fault_kind, fault_stage, fault_generation );
    else
    {
        if (report_error)
            WARN( "cannot authoritatively resynchronize x64 mappings: status %#x\n",
                  (unsigned int)status );
        range_array_free( &removals );
        range_array_free( &additions );
        range_array_free( &replacement );
        if (!status)
            TRACE( "resynchronized %zu canonical x64 mapping ranges with %zu removals and "
                   "%zu additions across generation %llu\n", completed_range_count,
                   completed_removal_count, completed_addition_count,
                   (unsigned long long)completed_generation );
    }
    return status;
}

struct flush_interval
{
    uint64_t start;
    uint64_t end;
};

struct flush_interval_array
{
    struct flush_interval *data;
    size_t count;
    size_t capacity;
};

static int compare_flush_interval( const void *left, const void *right )
{
    const struct flush_interval *a = left, *b = right;

    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    if (a->end < b->end) return -1;
    if (a->end > b->end) return 1;
    return 0;
}

static NTSTATUS normalize_flush_intervals( struct flush_interval_array *array )
{
    size_t i, out = 0;

    if (array->count < 2) return STATUS_SUCCESS;
    qsort( array->data, array->count, sizeof(*array->data), compare_flush_interval );
    for (i = 0; i < array->count; ++i)
    {
        if (out && array->data[i].start <= array->data[out - 1].end)
        {
            if (array->data[i].end > array->data[out - 1].end)
                array->data[out - 1].end = array->data[i].end;
        }
        else array->data[out++] = array->data[i];
    }
    array->count = out;
    return STATUS_SUCCESS;
}

static NTSTATUS append_flush_interval( struct flush_interval_array *array,
                                       uint64_t start, uint64_t end )
{
    struct flush_interval *data;
    size_t capacity;

    if (start >= end) return STATUS_SUCCESS;
#ifdef XTAJIT64_UNIXLIB_TEST
    if (test_fail_flush_interval_append >= 0 &&
        test_flush_interval_append_count++ == test_fail_flush_interval_append)
        return STATUS_NO_MEMORY;
#endif
    if (array->count == array->capacity)
    {
        capacity = array->capacity ? array->capacity * 2 : 16;
        if (capacity < array->capacity || capacity > SIZE_MAX / sizeof(*data))
            return STATUS_NO_MEMORY;
        if (!(data = realloc( array->data, capacity * sizeof(*data) )))
            return STATUS_NO_MEMORY;
        array->data = data;
        array->capacity = capacity;
    }
    array->data[array->count].start = start;
    array->data[array->count].end = end;
    ++array->count;
    return STATUS_SUCCESS;
}

static NTSTATUS collect_flush_intervals_locked(
    uint64_t address, uint64_t size, BOOL host_domain,
    struct flush_interval_array *result )
{
    uint64_t end = address + size;
    size_t i;
    NTSTATUS status;

    for (i = 0; i < provider.ranges.count; ++i)
    {
        const struct mapped_range *range = &provider.ranges.data[i];
        uint64_t range_start, range_end, overlap_start, overlap_end, guest_start;

        if (range->state != MEM_COMMIT) continue;
        range_start = host_domain ? range->host : range->guest;
        if (range_start > UINT64_MAX - range->size) return STATUS_INVALID_ADDRESS;
        range_end = range_start + range->size;
        if (range_end <= address || range_start >= end) continue;
        overlap_start = max( address, range_start );
        overlap_end = min( end, range_end );
        if (range->guest > UINT64_MAX - (overlap_start - range_start))
            return STATUS_INVALID_ADDRESS;
        guest_start = range->guest + overlap_start - range_start;
        status = append_flush_interval( result, guest_start,
                                        guest_start + overlap_end - overlap_start );
        if (status) return status;
    }
    return normalize_flush_intervals( result );
}

static BOOL flush_intervals_equal( const struct flush_interval_array *left,
                                   const struct flush_interval_array *right )
{
    size_t i;

    if (left->count != right->count) return FALSE;
    for (i = 0; i < left->count; ++i)
        if (left->data[i].start != right->data[i].start ||
            left->data[i].end != right->data[i].end)
            return FALSE;
    return TRUE;
}

static NTSTATUS flush_instruction_cache( void *args )
{
    const struct xtajit64_memory_params *params = args;
    struct flush_interval_array guest_intervals = {0}, host_intervals = {0};
    const struct flush_interval_array *intervals = NULL;
    struct thread_engine *engine;
    BOOL full_flush;
    volatile BOOL barrier_started = FALSE;
    volatile NTSTATUS status = STATUS_SUCCESS;
    volatile BOOL faulted = FALSE;
    enum mutation_kind fault_kind = MUTATION_NONE;
    enum mutation_stage fault_stage = MUTATION_STAGE_IDLE;
    uint64_t fault_generation = 0;
    uc_err err = UC_ERR_OK;
    size_t i;

    if (params && (params->flags || params->host || params->allocation_base ||
                   params->protect ||
                   (params->guest && params->guest > UINT64_MAX - params->size)))
        return STATUS_INVALID_PARAMETER;
    /* Windows ignores the region size when the base is NULL.  Preserve the
     * original size across the PE callback while treating every NULL-base
     * form as a provider-wide flush. */
    full_flush = !params || !params->guest;

    /* FlushInstructionCache accepts a non-NULL address with a zero length.
     * It is an exact empty interval, not the NULL/zero whole-cache sentinel. */
    if (params && params->guest && !params->size) return STATUS_SUCCESS;

    pthread_mutex_lock( &provider.mutex );
    status = claim_mutation_locked( MUTATION_FLUSH, FALSE );
    if (!status && !full_flush)
    {
#ifdef XTAJIT64_UNIXLIB_TEST
        test_flush_interval_append_count = 0;
#endif
        status = collect_flush_intervals_locked( params->guest, params->size,
                                                  FALSE, &guest_intervals );
        if (!status)
            status = collect_flush_intervals_locked( params->guest, params->size,
                                                      TRUE, &host_intervals );
        if (!status && guest_intervals.count && host_intervals.count &&
            !flush_intervals_equal( &guest_intervals, &host_intervals ))
            full_flush = TRUE;
        else if (!status)
            intervals = guest_intervals.count ? &guest_intervals : &host_intervals;
    }
    if (!status) __TRY
    {
        pause_mutation_engines_locked();
        barrier_started = TRUE;
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted && !status && barrier_started &&
        current_thread_owns_mutation_locked())
        status = wait_for_mutation_engines_locked();
    if (!faulted && !status) __TRY
    {
        test_mutation_fault_checkpoint( TEST_MUTATION_FAULT_AFTER_BEGIN );
        set_mutation_stage_locked( MUTATION_STAGE_APPLY );
        for (engine = provider.engines; engine; engine = engine->next)
        {
            if (full_flush) err = uc_ctl_flush_tb( engine->uc );
            else if (intervals)
            {
                /* Unicorn resolves targeted TB invalidations through the
                 * engine's software TLB.  A pooled engine may retain a TLB
                 * entry across an unmap/remap generation even when the final
                 * canonical host mapping is byte-for-byte identical. */
                err = uc_ctl_flush_tlb( engine->uc );
                for (i = 0; i < intervals->count; ++i)
                {
                    if (err != UC_ERR_OK) break;
                    if ((err = uc_ctl_remove_cache( engine->uc,
                                                    intervals->data[i].start,
                                                    intervals->data[i].end )) != UC_ERR_OK)
                        break;
                }
            }
            if (err != UC_ERR_OK)
            {
                status = STATUS_UNSUCCESSFUL;
                break;
            }
        }
    }
    __EXCEPT
    {
        status = recover_mutation_access_violation_locked();
        faulted = TRUE;
    }
    __ENDTRY
    if (!faulted)
    {
        /* Once engine quiescence has begun, a failed invalidation cannot prove
         * that every engine discarded the requested code.  Preflight failures
         * happen before any pause request or cache operation and are safe for
         * the caller to retry. */
        if (status && barrier_started && provider.initialized)
            poison_provider_locked( status );
        if (current_thread_owns_mutation_locked()) finish_mutation_locked();
    }
    if (faulted)
    {
        fault_kind = provider.last_fault_kind;
        fault_stage = provider.last_fault_stage;
        fault_generation = provider.last_fault_generation;
    }
    pthread_mutex_unlock( &provider.mutex );
    free( guest_intervals.data );
    free( host_intervals.data );
    if (faulted)
        report_mutation_access_violation( fault_kind, fault_stage, fault_generation );
    return status;
}

static NTSTATUS poison( void *args )
{
    const struct xtajit64_poison_params *params = args;
    struct thread_engine *engine;
    volatile NTSTATUS status = params && params->status ?
                               params->status : STATUS_UNSUCCESSFUL;
    volatile BOOL faulted = FALSE;
    enum mutation_kind fault_kind = MUTATION_NONE;
    enum mutation_stage fault_stage = MUTATION_STAGE_IDLE;
    uint64_t fault_generation = 0;

    pthread_mutex_lock( &provider.mutex );
    if (!provider.initialized) status = STATUS_INVALID_HANDLE;
    else
    {
        poison_provider_locked( status );
        __TRY
        {
            for (engine = provider.engines; engine; engine = engine->next)
                request_engine_pause_locked( engine );
        }
        __EXCEPT
        {
            status = record_mutation_access_violation_locked( MUTATION_POISON,
                                                              MUTATION_STAGE_PAUSE,
                                                              provider.generation );
            faulted = TRUE;
        }
        __ENDTRY
        if (!faulted) status = provider.poison_status;
        pthread_cond_broadcast( &provider.cond );
    }
    if (faulted)
    {
        fault_kind = provider.last_fault_kind;
        fault_stage = provider.last_fault_stage;
        fault_generation = provider.last_fault_generation;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (faulted)
        report_mutation_access_violation( fault_kind, fault_stage, fault_generation );
    return status;
}

static NTSTATUS begin_simulation( void *args )
{
    struct xtajit64_begin_params *params = args;
    struct thread_binding *binding;
    struct thread_engine *engine = NULL;
    volatile uint32_t *suspend_doorbell = NULL;
    uint64_t doorbell_host, doorbell_allocation;
    unsigned int doorbell_domain;
    uc_err err = UC_ERR_OK, read_err = UC_ERR_OK, context_err = UC_ERR_OK;
    NTSTATUS status = STATUS_SUCCESS;
    uint64_t next_rip;
    BOOL resume;

    if (!params || params->reserved || !params->context.rip ||
        params->context.rip > XTAJIT64_X64_USER_ADDRESS_MAX ||
        !params->context.rsp ||
        params->context.rsp < params->stack_limit ||
        params->context.rsp >= params->stack_base ||
        params->stack_base > XTAJIT64_X64_USER_ADDRESS_MAX + 1 ||
        !params->gs_base ||
        params->gs_base > XTAJIT64_X64_USER_ADDRESS_MAX ||
        params->gs_base > UINT64_MAX - XTAJIT64_TEB_SELF_END ||
        !params->suspend_doorbell ||
        (params->suspend_doorbell & (sizeof(uint32_t) - 1)) ||
        params->stack_limit >= params->stack_base)
        return STATUS_INVALID_PARAMETER;
    pthread_once( &engine_key_once, make_engine_key );
    if (engine_key_error || !(binding = pthread_getspecific( engine_key )))
        return STATUS_INVALID_HANDLE;

    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down ||
        binding->process_instance != provider.instance)
        status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else if (binding->active) status = STATUS_INVALID_DEVICE_STATE;
    else if (!translate_guest_range_locked(
                 params->suspend_doorbell, sizeof(uint32_t),
                 UC_PROT_READ | UC_PROT_WRITE, &doorbell_host,
                 &doorbell_allocation, &doorbell_domain ) ||
             doorbell_host != params->suspend_doorbell ||
             doorbell_domain != XTAJIT64_MEMORY_ADDRESS_IDENTITY)
        status = STATUS_INVALID_ADDRESS;
    else if (*(suspend_doorbell =
                   (volatile uint32_t *)(uintptr_t)doorbell_host))
    {
        params->transition_target = 0;
        params->fault_address = 0;
        params->fault_access = EXCEPTION_READ_FAULT;
        params->stop_reason = XTAJIT64_STOP_SUSPEND;
        params->unicorn_error = UC_ERR_OK;
    }
    else if ((err = acquire_pool_engine_locked( binding, &engine )) != UC_ERR_OK)
        status = err == UC_ERR_NOMEM ? STATUS_NO_MEMORY : STATUS_UNSUCCESSFUL;
    if (!status && !engine)
    {
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_SUCCESS;
    }
    if (status)
    {
        params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
        params->unicorn_error = err;
        pthread_mutex_unlock( &provider.mutex );
        return status;
    }
    pthread_mutex_unlock( &provider.mutex );

    do
    {
        resume = FALSE;
        pthread_mutex_lock( &provider.mutex );
        while (provider.mutating && provider.initialized)
            pthread_cond_wait( &provider.cond, &provider.mutex );
        if (!provider.initialized || !engine->uc || !engine->linked)
            status = STATUS_INVALID_HANDLE;
        else if (provider.poison_status) status = provider.poison_status;
        else if (!engine->in_use || !binding->active)
            status = STATUS_INVALID_DEVICE_STATE;
        else if (!translate_guest_range_locked(
                     params->suspend_doorbell, sizeof(uint32_t),
                     UC_PROT_READ | UC_PROT_WRITE, &doorbell_host,
                     &doorbell_allocation, &doorbell_domain ) ||
                 doorbell_host != params->suspend_doorbell ||
                 doorbell_domain != XTAJIT64_MEMORY_ADDRESS_IDENTITY)
            status = STATUS_INVALID_ADDRESS;
        else if (!registry_covers_readable_range( &provider.ranges, params->gs_base,
                                                   params->gs_base +
                                                   XTAJIT64_TEB_SELF_END ))
            status = STATUS_INVALID_ADDRESS;
        else if ((err = synchronize_engine_registry_locked( engine )) != UC_ERR_OK)
        {
            status = STATUS_UNSUCCESSFUL;
            poison_provider_locked( status );
        }
        else if ((err = write_context( engine, &params->context,
                                       params->gs_base )) != UC_ERR_OK)
        {
            status = STATUS_UNSUCCESSFUL;
            poison_provider_locked( status );
        }
        if (status)
        {
            params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
            params->unicorn_error = err;
            release_pool_engine_locked( binding, engine, FALSE );
            pthread_mutex_unlock( &provider.mutex );
            return status;
        }

        suspend_doorbell = (volatile uint32_t *)(uintptr_t)doorbell_host;
        engine->suspend_doorbell = suspend_doorbell;
        if (*suspend_doorbell)
        {
            params->transition_target = 0;
            params->fault_address = 0;
            params->fault_access = EXCEPTION_READ_FAULT;
            params->stop_reason = XTAJIT64_STOP_SUSPEND;
            params->unicorn_error = UC_ERR_OK;
            release_pool_engine_locked( binding, engine, FALSE );
            pthread_mutex_unlock( &provider.mutex );
            return STATUS_SUCCESS;
        }

        engine->stack_limit = params->stack_limit;
        engine->stack_base = params->stack_base;
        engine->transition_target = 0;
        engine->fault_address = 0;
        engine->fault_access = EXCEPTION_READ_FAULT;
        engine->mapping_error = UC_ERR_OK;
        engine->stop_reason = XTAJIT64_STOP_NONE;
        atomic_store_explicit( &engine->pause_requested, false, memory_order_release );
        if (*suspend_doorbell)
        {
            params->transition_target = 0;
            params->fault_address = 0;
            params->fault_access = EXCEPTION_READ_FAULT;
            params->stop_reason = XTAJIT64_STOP_SUSPEND;
            params->unicorn_error = UC_ERR_OK;
            release_pool_engine_locked( binding, engine, FALSE );
            pthread_mutex_unlock( &provider.mutex );
            return STATUS_SUCCESS;
        }
        engine->running = TRUE;
        pthread_mutex_unlock( &provider.mutex );

        next_rip = params->context.rip;
        for (;;)
        {
#ifdef XTAJIT64_UNIXLIB_TEST
            atomic_fetch_add_explicit( &test_emu_start_count, 1, memory_order_relaxed );
#endif
            err = uc_emu_start( engine->uc, next_rip, UINT64_MAX, 0, 0 );
            pthread_mutex_lock( &provider.mutex );
            if (engine->stop_reason == XTAJIT64_STOP_INTERNAL_ERROR &&
                engine->mapping_error != UC_ERR_OK)
                err = engine->mapping_error;
            if (!provider.poison_status && provider.initialized && engine->uc &&
                engine->linked && engine->stop_reason == XTAJIT64_STOP_SYSCALL &&
                err == UC_ERR_OK)
            {
                /* Syscalls are a hot path.  Keep the stopped engine's full
                 * register file resident and rewrite only the ARM64EC ABI
                 * registers before resuming in the x64 ntdll helper. */
                uc_err syscall_err = prepare_x64_syscall_engine(
                    engine, provider.x64_syscall_dispatcher,
                    provider.x64_syscall_count, &next_rip );

                if (syscall_err != UC_ERR_OK)
                {
                    err = syscall_err;
                    poison_provider_locked( STATUS_UNSUCCESSFUL );
                }
                else
                {
                    engine->stop_reason = XTAJIT64_STOP_NONE;
                    if (engine->suspend_doorbell && *engine->suspend_doorbell)
                        engine->stop_reason = XTAJIT64_STOP_SUSPEND;
                    if (engine->stop_reason == XTAJIT64_STOP_NONE &&
                        !atomic_load_explicit( &engine->pause_requested,
                                               memory_order_acquire ))
                    {
                        pthread_mutex_unlock( &provider.mutex );
                        continue;
                    }
                }
            }

            /* Keep the engine logically running until its registers are captured.
             * Mutators only publish a pause request, so the owner callback has
             * already completed uc_emu_stop before this read can begin. */
            read_err = read_context( engine, &params->context );
            if (read_err != UC_ERR_OK) poison_provider_locked( STATUS_UNSUCCESSFUL );
            if (provider.poison_status) status = provider.poison_status;
            else if (!provider.initialized || !engine->uc || !engine->linked)
                status = STATUS_INVALID_HANDLE;
            else if (atomic_load_explicit( &engine->pause_requested,
                                            memory_order_acquire ) &&
                     engine->stop_reason == XTAJIT64_STOP_NONE &&
                     err == UC_ERR_OK && read_err == UC_ERR_OK)
                resume = TRUE;

            if (resume)
            {
                engine->running = FALSE;
                pthread_cond_broadcast( &provider.cond );
            }
            else
            {
                params->transition_target = engine->transition_target;
                params->fault_address = engine->fault_address;
                params->fault_access = engine->fault_access;
                params->stop_reason = status ? XTAJIT64_STOP_INTERNAL_ERROR :
                                               engine->stop_reason;
                params->unicorn_error = err != UC_ERR_OK ? err : read_err;
#ifndef XTAJIT64_UNIXLIB_TEST
                if (status || params->stop_reason != XTAJIT64_STOP_EC_TRANSITION)
                    TRACE_(xtajitmap)(
                        "pid %ld engine %llu result status=%#x reason=%u "
                        "unicorn=%u emu=%u read=%u mapping=%u pause=%u "
                        "doorbell=%u rip=%#llx fault=%#llx access=%u\n",
                        (long)getpid(),
                        (unsigned long long)engine->diagnostic_id,
                        (unsigned int)status, params->stop_reason,
                        params->unicorn_error, err, read_err,
                        engine->mapping_error,
                        atomic_load_explicit( &engine->pause_requested,
                                              memory_order_acquire ),
                        engine->suspend_doorbell ? *engine->suspend_doorbell : 0,
                        (unsigned long long)params->context.rip,
                        (unsigned long long)params->fault_address,
                        params->fault_access );
#endif
                if (!status && params->stop_reason == XTAJIT64_STOP_NONE)
                    params->stop_reason = err == UC_ERR_INSN_INVALID ?
                                          XTAJIT64_STOP_INVALID_INSTRUCTION :
                                          XTAJIT64_STOP_INTERNAL_ERROR;
                if (!status && params->stop_reason == XTAJIT64_STOP_INTERNAL_ERROR)
                {
                    poison_provider_locked( STATUS_UNSUCCESSFUL );
                    status = provider.poison_status;
                }
                context_err = release_pool_engine_locked(
                    binding, engine, read_err == UC_ERR_OK );
                if (context_err != UC_ERR_OK)
                {
                    params->unicorn_error = context_err;
                    params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
                    poison_provider_locked( STATUS_UNSUCCESSFUL );
                    status = provider.poison_status;
                }
            }
            pthread_mutex_unlock( &provider.mutex );
            break;
        }
    } while (resume);

    if (status) return status;
    if (params->stop_reason == XTAJIT64_STOP_EC_TRANSITION) return STATUS_SUCCESS;
    if (params->stop_reason == XTAJIT64_STOP_SUSPEND) return STATUS_SUCCESS;
    if (params->stop_reason == XTAJIT64_STOP_MEMORY_FAULT) return STATUS_ACCESS_VIOLATION;
    if (params->stop_reason == XTAJIT64_STOP_MAPPING_MISS) return STATUS_RETRY;
    if (params->stop_reason == XTAJIT64_STOP_SINGLE_STEP) return STATUS_SINGLE_STEP;
    return STATUS_NOT_SUPPORTED;
}

#else /* HAVE_UNICORN */

static NTSTATUS unicorn_not_supported( void *args )
{
    return STATUS_NOT_SUPPORTED;
}

#define process_init             unicorn_not_supported
#define process_term             unicorn_not_supported
#define thread_init              unicorn_not_supported
#define thread_term              unicorn_not_supported
#define memory_map               unicorn_not_supported
#define memory_unmap             unicorn_not_supported
#define memory_protect           unicorn_not_supported
#define memory_resync_begin      unicorn_not_supported
#define memory_resync            unicorn_not_supported
#define memory_translate         unicorn_not_supported
#define flush_instruction_cache  unicorn_not_supported
#define poison                   unicorn_not_supported
#define begin_simulation         unicorn_not_supported

#endif /* HAVE_UNICORN */

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    process_init,
    process_term,
    thread_init,
    thread_term,
    memory_map,
    memory_unmap,
    memory_protect,
    memory_resync,
    flush_instruction_cache,
    poison,
    begin_simulation,
    memory_resync_begin,
    memory_translate,
};

C_ASSERT( ARRAY_SIZE(__wine_unix_call_funcs) == unix_funcs_count );
