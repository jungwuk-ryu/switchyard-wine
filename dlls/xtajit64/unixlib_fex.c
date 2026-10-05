/*
 * FEX-backed x86-64 emulation on ARM64
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
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <switchyard_fex.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "wine/debug.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(xtajit);

#define XTAJIT64_MAX_RESYNC_RANGES (1u << 20)
#define XTAJIT64_MAX_SYSCALL_COUNT  (1u << 16)
#define XTAJIT64_INTERNAL_PAUSE     0x46585041u /* "FXPA" */

#define FEX_PERM_READ    0x01u
#define FEX_PERM_WRITE   0x02u
#define FEX_PERM_EXECUTE 0x04u

#ifdef XTAJIT64_FEX_UNIXLIB_TEST
extern void xtajit64_fex_test_mutation_waiting(void);
extern void xtajit64_fex_test_mutation_quiesced(void);
extern void xtajit64_fex_test_execution_prepared(void);
extern void xtajit64_fex_test_execution_wake(void);
extern void xtajit64_fex_test_observer_status( const char *observer,
                                               NTSTATUS status,
                                               unsigned int stage );
#endif

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
};

struct range_array
{
    struct mapped_range *data;
    size_t count;
    size_t capacity;
};

struct thread_binding
{
    struct thread_binding *next;
    struct switchyard_fex_thread *thread;
    pthread_t owner;
    uint64_t process_instance;
    uint64_t id;
    volatile uint32_t *doorbell;
    struct switchyard_fex_admission *admission;
    uint64_t pause_generation;
    BOOL internal_pause_owned;
    struct switchyard_fex_dispatch dispatch;

    struct xtajit64_flight_recorder *flight_recorder;
    uint64_t flight_causal_boundary_id;
    uint64_t flight_context_generation;
    uint64_t flight_transition_generation;
    uint64_t flight_last_context_generation;
    uint64_t flight_expected_teb;
    uint64_t flight_claimed_teb;
    uint64_t flight_guest_rip;
    uint64_t flight_guest_rsp;
    uint64_t flight_guest_stack_limit;
    uint64_t flight_guest_stack_base;
    uint64_t flight_control_stack_limit;
    uint64_t flight_control_stack_top;
};

struct low_observer_transaction
{
    uint64_t generation;
    uint32_t operation;
};

struct code_observer_transaction
{
    uint64_t generation;
    uint32_t operation;
};

struct provider_process
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    BOOL initialized;
    BOOL forked_child;
    BOOL mutating;
    BOOL shutting_down;
    BOOL observer_active;
    BOOL code_observer_active;
    BOOL signal_observer_active;
    BOOL custom_dispatch;
    BOOL mutation_owner_valid;
    pthread_t mutation_owner;
    NTSTATUS poison_status;
    uint64_t generation;
    uint64_t instance;
    uint64_t next_binding_id;

    struct switchyard_fex_process *process;
    const uint64_t *ec_bitmap;
    uint64_t highest_user_address;
    unsigned int ec_page_shift;
    unsigned int native_page_size;
    uint64_t x64_syscall_dispatcher;
    uint32_t x64_syscall_count;
    uint64_t guest_kuser;
    uint64_t host_kuser;
    uint64_t kuser_size;

    struct range_array ranges;
    struct thread_binding *bindings;
    struct low_observer_transaction *low_transaction;
    struct code_observer_transaction *code_transaction;
};

static struct provider_process provider =
{
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static BOOL binding_is_active( const struct thread_binding *binding )
{
    return binding && binding->admission &&
           (switchyard_fex_admission_load( binding->admission ) & SWITCHYARD_FEX_ADMISSION_ACTIVE);
}

/* Completion has consumed the shared generation before entering this callback.
 * Using the predicate's mutex prevents a lost wake against cond_wait. The
 * attached owner pins the cell until this callback and its bridge have returned. */
static void wake_shared_execution( void *context )
{
    struct provider_process *process = context;

    pthread_mutex_lock( &process->mutex );
    pthread_cond_broadcast( &process->cond );
    pthread_mutex_unlock( &process->mutex );
#ifdef XTAJIT64_FEX_UNIXLIB_TEST
    xtajit64_fex_test_execution_wake();
#endif
}

static pthread_key_t binding_key;
static pthread_once_t binding_key_once = PTHREAD_ONCE_INIT;
static int binding_key_error;
static pthread_once_t atfork_once = PTHREAD_ONCE_INIT;
static int atfork_error;
static _Thread_local struct thread_binding *active_signal_binding;

C_ASSERT( sizeof(struct wine_arm64ec_jit_host_context_v1) ==
          sizeof(struct switchyard_fex_arm64_host_context) );
C_ASSERT( offsetof(struct wine_arm64ec_jit_host_context_v1, gpr) ==
          offsetof(struct switchyard_fex_arm64_host_context, gpr) );
C_ASSERT( offsetof(struct wine_arm64ec_jit_host_context_v1, vector) ==
          offsetof(struct switchyard_fex_arm64_host_context, vector) );
C_ASSERT( offsetof(struct wine_arm64ec_jit_host_context_v1, pc) ==
          offsetof(struct switchyard_fex_arm64_host_context, pc) );

/* A fork child may exec or exit, but it must never reuse C++ runtime objects
 * copied while other threads could have owned their internal locks.  Keep the
 * inherited allocations untouched and make every provider entry fail closed;
 * exec installs a fresh image and fresh provider state. */
static void provider_fork_prepare(void)
{
    pthread_mutex_lock( &provider.mutex );
}

static void provider_fork_parent(void)
{
    pthread_mutex_unlock( &provider.mutex );
}

static void provider_fork_child(void)
{
    __atomic_store_n( &active_signal_binding, NULL, __ATOMIC_RELEASE );
    provider.forked_child = TRUE;
    provider.initialized = FALSE;
    provider.mutating = FALSE;
    provider.shutting_down = FALSE;
    provider.observer_active = FALSE;
    provider.code_observer_active = FALSE;
    provider.signal_observer_active = FALSE;
    provider.mutation_owner_valid = FALSE;
    provider.poison_status = STATUS_NOT_SUPPORTED;
    provider.process = NULL;
    provider.ec_bitmap = NULL;
    provider.highest_user_address = 0;
    provider.ec_page_shift = 0;
    provider.x64_syscall_dispatcher = 0;
    provider.x64_syscall_count = 0;
    provider.guest_kuser = 0;
    provider.host_kuser = 0;
    provider.kuser_size = 0;
    memset( &provider.ranges, 0, sizeof(provider.ranges) );
    provider.bindings = NULL;
    provider.low_transaction = NULL;
    provider.code_transaction = NULL;
    pthread_mutex_unlock( &provider.mutex );
}

static void register_atfork_handlers(void)
{
    atfork_error = pthread_atfork( provider_fork_prepare, provider_fork_parent,
                                  provider_fork_child );
}

static uint64_t align_down( uint64_t value )
{
    return value & ~(uint64_t)(XTAJIT64_GUEST_PAGE_SIZE - 1);
}

static uint64_t align_up( uint64_t value )
{
    return (value + XTAJIT64_GUEST_PAGE_SIZE - 1) &
           ~(uint64_t)(XTAJIT64_GUEST_PAGE_SIZE - 1);
}

static BOOL align_range( uint64_t address, uint64_t size,
                         uint64_t *start, uint64_t *end )
{
    uint64_t limit;

    if (!start || !end || !size || address > UINT64_MAX - size) return FALSE;
    limit = address + size;
    if (limit > UINT64_MAX - (XTAJIT64_GUEST_PAGE_SIZE - 1)) return FALSE;
    *start = align_down( address );
    *end = align_up( limit );
    return *start < *end;
}

static unsigned int protection_to_perms( unsigned int protect )
{
    if (protect & PAGE_GUARD) return 0;
    switch (protect & 0xff)
    {
    case PAGE_READONLY:
        return FEX_PERM_READ;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        return FEX_PERM_READ | FEX_PERM_WRITE;
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
        return FEX_PERM_READ | FEX_PERM_EXECUTE;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return FEX_PERM_READ | FEX_PERM_WRITE | FEX_PERM_EXECUTE;
    default:
        return 0;
    }
}

static NTSTATUS fex_result_to_status( enum switchyard_fex_result result )
{
    switch (result)
    {
    case SWITCHYARD_FEX_OK: return STATUS_SUCCESS;
    case SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT: return STATUS_INVALID_PARAMETER;
    case SWITCHYARD_FEX_ERROR_ABI_MISMATCH: return STATUS_REVISION_MISMATCH;
    case SWITCHYARD_FEX_ERROR_UNSUPPORTED: return STATUS_NOT_SUPPORTED;
    case SWITCHYARD_FEX_ERROR_NO_MEMORY: return STATUS_NO_MEMORY;
    case SWITCHYARD_FEX_ERROR_BUSY: return STATUS_DEVICE_BUSY;
    case SWITCHYARD_FEX_ERROR_INITIALIZATION: return STATUS_DLL_INIT_FAILED;
    case SWITCHYARD_FEX_ERROR_GUEST_FAULT: return STATUS_UNHANDLED_EXCEPTION;
    case SWITCHYARD_FEX_ERROR_INTERNAL: return STATUS_UNSUCCESSFUL;
    default: return STATUS_UNSUCCESSFUL;
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
           left->domain == right->domain && left->flags == right->flags;
}

static BOOL range_array_append( struct range_array *array,
                                const struct mapped_range *range )
{
    struct mapped_range *last;

    if (!range->size) return TRUE;
    if (array->count)
    {
        last = &array->data[array->count - 1];
        if (last->guest > UINT64_MAX - last->size ||
            last->guest + last->size > range->guest)
            return FALSE;
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

static BOOL range_overlaps( const struct mapped_range *range,
                            uint64_t start, uint64_t end )
{
    return range->guest < end && start < range->guest + range->size;
}

static void range_array_free( struct range_array *array )
{
    free( array->data );
    memset( array, 0, sizeof(*array) );
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
    if (!inserted && !range_array_append( result, mapping ))
        return STATUS_INVALID_ADDRESS;
    return STATUS_SUCCESS;
}

static NTSTATUS build_unmapped_registry( const struct range_array *old,
                                         uint64_t start, uint64_t size,
                                         uint64_t allocation_base,
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

static NTSTATUS build_protected_registry( const struct range_array *old,
                                          uint64_t start, uint64_t end,
                                          unsigned int perms,
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

static unsigned int translation_required_perms( unsigned int flags )
{
    unsigned int perms = 0;

    if (flags & XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ) perms |= FEX_PERM_READ;
    if (flags & XTAJIT64_MEMORY_TRANSLATE_REQUIRE_WRITE) perms |= FEX_PERM_WRITE;
    if (flags & XTAJIT64_MEMORY_TRANSLATE_REQUIRE_EXECUTE) perms |= FEX_PERM_EXECUTE;
    return perms;
}

static BOOL translate_guest_range_locked( uint64_t guest, uint64_t size,
                                          unsigned int required_perms,
                                          uint64_t *host,
                                          uint64_t *allocation_base,
                                          unsigned int *domain )
{
    const struct mapped_range *range, *first;
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
    first = range = &provider.ranges.data[left];
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
            range->allocation_base != allocation || range->domain != first->domain ||
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
    if (domain) *domain = first->domain;
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

        if (range->state != MEM_COMMIT || range->host > UINT64_MAX - range->size)
            continue;
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

/* FEX caches this answer only until the next code invalidation. The active
 * execution pins the registry and its host backing; a mutation must suspend
 * this thread before it can replace either. Never wait for that mutation here:
 * the compiling thread must be able to reach its suspension doorbell. */
static enum switchyard_fex_result query_guest_executable_range(
    void *context, uint64_t address, struct switchyard_fex_executable_range *output )
{
    struct thread_binding *binding;
    enum switchyard_fex_result result = SWITCHYARD_FEX_ERROR_UNSUPPORTED;
    size_t left = 0, right;

    if (context != &provider || !output || output->size != sizeof(*output) ||
        output->version != SWITCHYARD_FEX_EXECUTABLE_RANGE_VERSION)
        return SWITCHYARD_FEX_ERROR_INVALID_ARGUMENT;
    binding = __atomic_load_n( &active_signal_binding, __ATOMIC_ACQUIRE );
    pthread_mutex_lock( &provider.mutex );
    if (!provider.initialized || provider.shutting_down || provider.poison_status ||
        !binding_is_active( binding ) || binding->process_instance != provider.instance ||
        !address || address > provider.highest_user_address)
        goto done;
    right = provider.ranges.count;
    while (left < right)
    {
        size_t mid = left + (right - left) / 2;
        const struct mapped_range *range = &provider.ranges.data[mid];

        if (range->guest + range->size <= address) left = mid + 1;
        else right = mid;
    }
    if (left < provider.ranges.count)
    {
        const struct mapped_range *range = &provider.ranges.data[left];

        if (range->guest <= address && range->state == MEM_COMMIT &&
            (range->perms & (FEX_PERM_READ | FEX_PERM_EXECUTE)) ==
                (FEX_PERM_READ | FEX_PERM_EXECUTE))
        {
            output->flags = (range->perms & FEX_PERM_WRITE) ?
                SWITCHYARD_FEX_EXECUTABLE_RANGE_WRITABLE : 0;
            output->reserved = 0;
            output->base = range->guest;
            output->length = range->size;
            result = SWITCHYARD_FEX_OK;
        }
    }
done:
    pthread_mutex_unlock( &provider.mutex );
    return result;
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

static BOOL any_binding_active_locked(void)
{
    const struct thread_binding *binding;

    for (binding = provider.bindings; binding; binding = binding->next)
        if (binding_is_active( binding )) return TRUE;
    return FALSE;
}

static BOOL current_thread_owns_mutation_locked(void)
{
    return provider.mutating && provider.mutation_owner_valid &&
           pthread_equal( provider.mutation_owner, pthread_self() );
}

static void request_binding_pause_locked( struct thread_binding *binding )
{
    uint32_t expected = 0;

    if (!binding_is_active( binding ) || !binding->doorbell || binding->internal_pause_owned)
        return;
    if (__atomic_compare_exchange_n( (uint32_t *)(uintptr_t)binding->doorbell,
                                     &expected, XTAJIT64_INTERNAL_PAUSE, FALSE,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
    {
        binding->internal_pause_owned = TRUE;
        /* Wake may let mutation finish before execution_complete takes the
         * mutex. This generation remains a stop cause after the doorbell is
         * cleared; it is not a second admission/active authority. */
        binding->pause_generation = switchyard_fex_admission_load( binding->admission ) >> 3;
    }
}

static NTSTATUS begin_mutation_locked(void)
{
    struct thread_binding *binding;

    if (current_thread_owns_mutation_locked())
        return STATUS_INVALID_DEVICE_STATE;
    while (provider.mutating && provider.initialized)
    {
#ifdef XTAJIT64_FEX_UNIXLIB_TEST
        xtajit64_fex_test_mutation_waiting();
#endif
        pthread_cond_wait( &provider.cond, &provider.mutex );
    }
    if (provider.forked_child) return STATUS_NOT_SUPPORTED;
    if (!provider.initialized || provider.shutting_down) return STATUS_INVALID_HANDLE;
    if (provider.poison_status) return provider.poison_status;
    /* Another thread's observer is ordinary contention, not recursive entry.
     * Its transaction pointers are cleared before finish_mutation_locked()
     * wakes us. Reject only inconsistent leftovers after that ownership wait. */
    if (provider.low_transaction || provider.code_transaction)
        return STATUS_INVALID_DEVICE_STATE;

    provider.mutating = TRUE;
    provider.mutation_owner = pthread_self();
    provider.mutation_owner_valid = TRUE;
    if (!++provider.generation) ++provider.generation;
    for (binding = provider.bindings; binding; binding = binding->next)
        switchyard_fex_admission_close( binding->admission );
    for (binding = provider.bindings; binding; binding = binding->next)
        request_binding_pause_locked( binding );
    while (any_binding_active_locked())
        pthread_cond_wait( &provider.cond, &provider.mutex );
#ifdef XTAJIT64_FEX_UNIXLIB_TEST
    xtajit64_fex_test_mutation_quiesced();
#endif
    return STATUS_SUCCESS;
}

static void finish_mutation_locked(void)
{
    struct thread_binding *binding;

    for (binding = provider.bindings; binding; binding = binding->next)
    {
        uint32_t expected = XTAJIT64_INTERNAL_PAUSE;

        if (binding->internal_pause_owned && binding->doorbell)
            __atomic_compare_exchange_n( (uint32_t *)(uintptr_t)binding->doorbell,
                                         &expected, 0, FALSE,
                                         __ATOMIC_RELEASE, __ATOMIC_ACQUIRE );
        binding->internal_pause_owned = FALSE;
        switchyard_fex_admission_reopen( binding->admission );
    }
    provider.mutation_owner_valid = FALSE;
    provider.mutating = FALSE;
    pthread_cond_broadcast( &provider.cond );
}

static NTSTATUS invalidate_code_locked( uint64_t address, uint64_t size )
{
    enum switchyard_fex_result result;

    if (!size || address > UINT64_MAX - size) return STATUS_INVALID_PARAMETER;
    if (!provider.process) return STATUS_INVALID_HANDLE;
    result = switchyard_fex_process_invalidate_code( provider.process, address, size );
    return fex_result_to_status( result );
}

static NTSTATUS invalidate_all_code_locked(void)
{
    if (provider.highest_user_address == UINT64_MAX) return STATUS_INVALID_PARAMETER;
    return invalidate_code_locked( 0, provider.highest_user_address + 1 );
}

static void poison_provider_locked( NTSTATUS status )
{
    if (!status) status = STATUS_UNSUCCESSFUL;
    if (!provider.poison_status) provider.poison_status = status;
}

#ifdef XTAJIT64_FEX_UNIXLIB_TEST
/* Immutable legacy converter oracle; normal entry now borrows a bounded view. */
static void import_fex_state( struct switchyard_fex_x64_state *dst,
                              const struct xtajit64_x64_context *src,
                              uint64_t gs_base )
{
    /* Initialize the complete payload without clearing registers that are
     * immediately overwritten. Keep even the invalid YMM high halves defined
     * for all adapter vector layouts. */
    dst->size = sizeof(*dst);
    dst->version = SWITCHYARD_FEX_STATE_VERSION;
    dst->flags = 0;
    dst->reserved = 0;
    dst->gpr[0] = src->rax;
    dst->gpr[1] = src->rcx;
    dst->gpr[2] = src->rdx;
    dst->gpr[3] = src->rbx;
    dst->gpr[4] = src->rsp;
    dst->gpr[5] = src->rbp;
    dst->gpr[6] = src->rsi;
    dst->gpr[7] = src->rdi;
    dst->gpr[8] = src->r8;
    dst->gpr[9] = src->r9;
    dst->gpr[10] = src->r10;
    dst->gpr[11] = src->r11;
    dst->gpr[12] = src->r12;
    dst->gpr[13] = src->r13;
    dst->gpr[14] = src->r14;
    dst->gpr[15] = src->r15;
    dst->rip = src->rip;
    dst->rflags = src->eflags;
    dst->mxcsr = src->mxcsr;
    dst->fcw = 0x037f;
    dst->abridged_ftw = 0;
    dst->reserved_fp = 0;
    memset( dst->segment, 0, sizeof(dst->segment) );
    dst->segment[1] = 0x30;
    dst->reserved_segment = 0;
    memset( dst->segment_base, 0, sizeof(dst->segment_base) );
    dst->segment_base[5] = gs_base;
    memcpy( dst->xmm, src->xmm, sizeof(src->xmm) );
    memset( dst->ymm_high, 0, sizeof(dst->ymm_high) );
    memset( dst->x87, 0, sizeof(dst->x87) );
}

#endif

static void export_fex_state( struct xtajit64_x64_context *dst,
                              const struct switchyard_fex_x64_state *src )
{
    dst->rax = src->gpr[0];
    dst->rcx = src->gpr[1];
    dst->rdx = src->gpr[2];
    dst->rbx = src->gpr[3];
    dst->rsp = src->gpr[4];
    dst->rbp = src->gpr[5];
    dst->rsi = src->gpr[6];
    dst->rdi = src->gpr[7];
    dst->r8 = src->gpr[8];
    dst->r9 = src->gpr[9];
    dst->r10 = src->gpr[10];
    dst->r11 = src->gpr[11];
    dst->r12 = src->gpr[12];
    dst->r13 = src->gpr[13];
    dst->r14 = src->gpr[14];
    dst->r15 = src->gpr[15];
    dst->rip = src->rip;
    dst->eflags = src->rflags;
    dst->mxcsr = src->mxcsr;
    dst->reserved = 0;
    memcpy( dst->xmm, src->xmm, sizeof(dst->xmm) );
}

static void init_fex_state_output( struct switchyard_fex_x64_state *state )
{
    /* Export validates only these fields; consume the complete payload on OK. */
    state->size = sizeof(*state);
    state->version = SWITCHYARD_FEX_STATE_VERSION;
}

static void clear_flight_binding( struct thread_binding *binding )
{
    binding->flight_recorder = NULL;
    binding->flight_causal_boundary_id = 0;
    binding->flight_context_generation = 0;
    binding->flight_transition_generation = 0;
    binding->flight_last_context_generation = 0;
    binding->flight_expected_teb = 0;
    binding->flight_claimed_teb = 0;
    binding->flight_guest_rip = 0;
    binding->flight_guest_rsp = 0;
    binding->flight_guest_stack_limit = 0;
    binding->flight_guest_stack_base = 0;
    binding->flight_control_stack_limit = 0;
    binding->flight_control_stack_top = 0;
}

static void record_flight_event( struct thread_binding *binding, uint32_t type,
                                 uint32_t reason, uint32_t stop_reason,
                                 uint64_t detail0, uint64_t detail1 )
{
    struct xtajit64_flight_recorder *recorder;
    struct xtajit64_flight_scratch *scratch;
    struct xtajit64_flight_event *event;
    uint64_t pthread_identity = XTAJIT64_FLIGHT_UNKNOWN_U64;

    if (!binding || !(recorder = binding->flight_recorder) ||
        !xtajit64_flight_recorder_is_active( recorder ) ||
        !(event = xtajit64_flight_acquire_scratch( recorder, &scratch )))
        return;
    xtajit64_flight_event_init( event, type, XTAJIT64_FLIGHT_SOURCE_UNIX_PROVIDER );
    event->causal_boundary_id = binding->flight_causal_boundary_id;
    event->binding_id = binding->id;
    event->engine_id = binding->id;
    event->engine_generation = provider.instance;
    event->mapping_generation = provider.generation;
    event->context_generation = binding->flight_context_generation;
    event->transition_generation = binding->flight_transition_generation;
    event->guest_rip = binding->flight_guest_rip;
    event->guest_rsp = binding->flight_guest_rsp;
    event->guest_stack_limit = binding->flight_guest_stack_limit;
    event->guest_stack_base = binding->flight_guest_stack_base;
    event->control_stack_limit = binding->flight_control_stack_limit;
    event->control_stack_top = binding->flight_control_stack_top;
    event->expected_teb = binding->flight_expected_teb;
    event->pid = getpid();
    memcpy( &pthread_identity, &binding->owner,
            min( sizeof(pthread_identity), sizeof(binding->owner) ) );
    event->pthread_identity = pthread_identity;
    event->reason = reason;
    event->stop_reason = stop_reason;
    event->detail0 = detail0;
    event->detail1 = detail1;
    xtajit64_flight_record( recorder, event );
    xtajit64_flight_release_scratch( scratch );
}

static void refresh_flight_binding_locked( struct thread_binding *binding,
                                           const struct xtajit64_begin_params *params )
{
    struct xtajit64_flight_recorder *recorder;
    uint64_t boundary;

    if (!binding || !(recorder = binding->flight_recorder) ||
        !xtajit64_flight_recorder_is_active( recorder )) return;
    boundary = xtajit64_flight_current_boundary( recorder );
    if (boundary && boundary != XTAJIT64_FLIGHT_UNKNOWN_U64 &&
        boundary >= binding->flight_last_context_generation)
    {
        binding->flight_causal_boundary_id = boundary;
        binding->flight_context_generation = boundary;
        binding->flight_transition_generation = boundary;
        binding->flight_last_context_generation = boundary;
    }
    binding->flight_guest_rip = params->context.rip;
    binding->flight_guest_rsp = params->context.rsp;
    binding->flight_guest_stack_limit = params->stack_limit;
    binding->flight_guest_stack_base = params->stack_base;
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

static NTSTATUS control_stack_protect( void *args )
{
    struct xtajit64_control_stack_params params;
    WINE_TRANSLATED_VIEW_INFORMATION translated = {0};
    MEMORY_BASIC_INFORMATION info;
    unsigned int page_size;
    ULONG old_protect;
    SIZE_T size;
    void *base, *guard, *expected_guard;
    NTSTATUS status = STATUS_SUCCESS;

    if (!args) return STATUS_INVALID_PARAMETER;
    memcpy( &params, args, sizeof(params) );
    if (!params.allocation || params.size != XTAJIT64_CONTROL_STACK_SIZE ||
        params.allocation > XTAJIT64_X64_USER_ADDRESS_MAX - params.size + 1)
        return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &provider.mutex );
    page_size = provider.native_page_size;
    if (!provider.initialized || provider.shutting_down) status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    pthread_mutex_unlock( &provider.mutex );
    if (status) return status;
    if (page_size < XTAJIT64_GUEST_PAGE_SIZE || page_size > XTAJIT64_MAX_HOST_PAGE_SIZE ||
        (page_size & (page_size - 1)) || (params.allocation & (page_size - 1)))
        return STATUS_INVALID_PARAMETER;

    /* Authenticate the complete fresh allocation, not a PE-supplied page
     * size or an arbitrary interior address. Do not hold the provider mutex
     * across Wine VM calls: their native observers enter this provider. */
    base = (void *)(uintptr_t)params.allocation;
    status = NtQueryVirtualMemory( NtCurrentProcess(), base, MemoryBasicInformation,
                                   &info, sizeof(info), NULL );
    if (status) return status;
    if (info.BaseAddress != base || info.AllocationBase != base ||
        info.RegionSize != params.size || info.State != MEM_COMMIT ||
        info.Type != MEM_PRIVATE || info.Protect != PAGE_READWRITE)
        return STATUS_INVALID_ADDRESS;
    status = NtQueryVirtualMemory( NtCurrentProcess(), base, MemoryWineTranslatedViewInformation,
                                   &translated, sizeof(translated), NULL );
    if (status) return status;
    if (translated.Version != WINE_TRANSLATED_VIEW_INFORMATION_VERSION ||
        translated.Reserved || translated.Flags ||
        translated.GuestBase != base || translated.HostBase != base ||
        translated.AllocationBase != base ||
        translated.RegionSize != params.size)
        return STATUS_INVALID_ADDRESS;

    expected_guard = (char *)base + page_size;
    guard = expected_guard;
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &guard, &size,
                                     PAGE_NOACCESS, &old_protect );
    if (!status && (guard != expected_guard || size != page_size || old_protect != PAGE_READWRITE))
        status = STATUS_INVALID_ADDRESS;
    return status;
}

static NTSTATUS memory_map( void *args )
{
    const struct xtajit64_memory_params *params = args;
    struct mapped_range mapping;
    struct range_array replacement = {0};
    uint64_t start, end, host_start, host_end;
    NTSTATUS status;
    BOOL mutation = FALSE;

    if (!params || params->flags || !params->guest || !params->host ||
        !params->size || !params->allocation_base ||
        !align_range( params->guest, params->size, &start, &end ) ||
        !align_range( params->host, params->size, &host_start, &host_end ) ||
        end - start != host_end - host_start ||
        end - 1 > XTAJIT64_X64_USER_ADDRESS_MAX || start != host_start ||
        ((start ^ host_start) & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
        (params->allocation_base & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
        params->allocation_base > start)
        return STATUS_INVALID_PARAMETER;

    memset( &mapping, 0, sizeof(mapping) );
    mapping.guest = start;
    mapping.host = host_start;
    mapping.size = end - start;
    mapping.allocation_base = params->allocation_base;
    mapping.perms = protection_to_perms( params->protect );
    mapping.state = MEM_COMMIT;
    mapping.domain = XTAJIT64_MEMORY_ADDRESS_IDENTITY;

    pthread_mutex_lock( &provider.mutex );
    if (legacy_mutation_selects_low_locked( start, end - start ))
        status = STATUS_ACCESS_DENIED;
    else if (!(status = begin_mutation_locked())) mutation = TRUE;
    if (!status) status = build_mapped_registry( &provider.ranges, &mapping,
                                                 &replacement );
    if (!status) status = invalidate_code_locked( start, end - start );
    if (!status)
    {
        struct range_array old = provider.ranges;

        provider.ranges = replacement;
        memset( &replacement, 0, sizeof(replacement) );
        range_array_free( &old );
    }
    if (status && status != STATUS_ACCESS_DENIED && provider.initialized)
        poison_provider_locked( status );
    if (mutation) finish_mutation_locked();
    pthread_mutex_unlock( &provider.mutex );
    range_array_free( &replacement );
    return status;
}

static NTSTATUS memory_unmap( void *args )
{
    const struct xtajit64_memory_params *params = args;
    struct range_array replacement = {0};
    uint64_t guest, size = 0;
    NTSTATUS status;
    BOOL mutation = FALSE;

    if (!params || (params->flags & ~XTAJIT64_MEMORY_VALID_FLAGS) ||
        !params->guest || params->guest > XTAJIT64_X64_USER_ADDRESS_MAX)
        return STATUS_INVALID_PARAMETER;
    if (params->size)
    {
        uint64_t end;

        if (!align_range( params->guest, params->size, &guest, &end ) ||
            end - 1 > XTAJIT64_X64_USER_ADDRESS_MAX)
            return STATUS_INVALID_PARAMETER;
        size = end - guest;
    }
    else guest = align_down( params->guest );

    pthread_mutex_lock( &provider.mutex );
    if (legacy_mutation_selects_low_locked( guest, size ))
        status = STATUS_ACCESS_DENIED;
    else if (!(status = begin_mutation_locked())) mutation = TRUE;
    if (!status) status = build_unmapped_registry( &provider.ranges, guest, size,
                                                   guest, &replacement );
    if (!status)
        status = size ? invalidate_code_locked( guest, size ) :
                        invalidate_all_code_locked();
    if (!status)
    {
        struct range_array old = provider.ranges;

        provider.ranges = replacement;
        memset( &replacement, 0, sizeof(replacement) );
        range_array_free( &old );
    }
    if (status && status != STATUS_ACCESS_DENIED && provider.initialized)
        poison_provider_locked( status );
    if (mutation) finish_mutation_locked();
    pthread_mutex_unlock( &provider.mutex );
    range_array_free( &replacement );
    return status;
}

static NTSTATUS memory_protect( void *args )
{
    const struct xtajit64_memory_params *params = args;
    struct range_array replacement = {0};
    uint64_t start, end;
    NTSTATUS status;
    BOOL mutation = FALSE;

    if (!params || (params->flags & ~XTAJIT64_MEMORY_VALID_FLAGS) ||
        !params->guest || !params->size ||
        !align_range( params->guest, params->size, &start, &end ) ||
        end - 1 > XTAJIT64_X64_USER_ADDRESS_MAX)
        return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &provider.mutex );
    if (legacy_mutation_selects_low_locked( start, end - start ))
        status = STATUS_ACCESS_DENIED;
    else if (!(status = begin_mutation_locked())) mutation = TRUE;
    if (!status && !registry_covers_range( &provider.ranges, start, end ))
        status = STATUS_INVALID_ADDRESS;
    if (!status)
        status = build_protected_registry( &provider.ranges, start, end,
                                           protection_to_perms( params->protect ),
                                           &replacement );
    if (!status) status = invalidate_code_locked( start, end - start );
    if (!status)
    {
        struct range_array old = provider.ranges;

        provider.ranges = replacement;
        memset( &replacement, 0, sizeof(replacement) );
        range_array_free( &old );
    }
    if (status && status != STATUS_ACCESS_DENIED && provider.initialized)
        poison_provider_locked( status );
    if (mutation) finish_mutation_locked();
    pthread_mutex_unlock( &provider.mutex );
    range_array_free( &replacement );
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

static NTSTATUS build_resync_registry( const struct xtajit64_memory_resync_params *params,
                                       struct range_array *result )
{
    const struct xtajit64_memory_params *input;
    struct xtajit64_memory_params *copy = NULL;
    struct mapped_range kuser, range;
    struct range_array identity = {0}, low = {0}, merged = {0};
    uint64_t start, end, host_start, host_end, previous_end = 0;
    size_t i;
    BOOL inserted_kuser = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (!params || params->reserved || params->count > XTAJIT64_MAX_RESYNC_RANGES ||
        (params->count && !params->ranges)) return STATUS_INVALID_PARAMETER;
    input = (const struct xtajit64_memory_params *)(uintptr_t)params->ranges;
    if (params->count)
    {
        if (!(copy = malloc( (size_t)params->count * sizeof(*copy) )))
            return STATUS_NO_MEMORY;
        memcpy( copy, input, params->count * sizeof(*copy) );
        qsort( copy, params->count, sizeof(*copy), compare_memory_params );
    }

    memset( &kuser, 0, sizeof(kuser) );
    kuser.guest = provider.guest_kuser;
    kuser.host = provider.host_kuser;
    kuser.size = provider.kuser_size;
    kuser.allocation_base = provider.guest_kuser;
    kuser.perms = FEX_PERM_READ;
    kuser.state = MEM_COMMIT;
    kuser.domain = XTAJIT64_MEMORY_ADDRESS_INVALID;
    kuser.permanent = TRUE;
    if (!range_array_reserve( &identity, params->count + 1 ))
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
            end - 1 > provider.highest_user_address || start != host_start ||
            (copy[i].allocation_base & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
            copy[i].allocation_base > start || start < previous_end)
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
            if (!range_array_append( &identity, &kuser ))
            {
                status = STATUS_NO_MEMORY;
                goto done;
            }
            inserted_kuser = TRUE;
        }
        memset( &range, 0, sizeof(range) );
        range.guest = start;
        range.host = host_start;
        range.size = end - start;
        range.allocation_base = copy[i].allocation_base;
        range.perms = protection_to_perms( copy[i].protect );
        range.state = MEM_COMMIT;
        range.domain = XTAJIT64_MEMORY_ADDRESS_IDENTITY;
        if (!range_array_append( &identity, &range ))
        {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
        previous_end = end;
    }
    if (!inserted_kuser && !range_array_append( &identity, &kuser ))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }

    if (!range_array_reserve( &low, provider.ranges.count ))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    for (i = 0; i < provider.ranges.count; ++i)
    {
        const struct mapped_range *old = &provider.ranges.data[i];

        if (old->domain != XTAJIT64_MEMORY_ADDRESS_AMD64_LOW) continue;
        if (!range_array_append( &low, old ))
        {
            status = STATUS_INVALID_ADDRESS;
            goto done;
        }
    }
    if (!(status = merge_range_arrays( &identity, &low, &merged )))
    {
        *result = merged;
        memset( &merged, 0, sizeof(merged) );
    }

done:
    free( copy );
    range_array_free( &identity );
    range_array_free( &low );
    range_array_free( &merged );
    return status;
}

static NTSTATUS memory_resync_begin( void *args )
{
    struct xtajit64_memory_resync_begin_params *params = args;
    NTSTATUS status = STATUS_SUCCESS;

    if (!params) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down) status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else params->generation = provider.generation;
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static NTSTATUS memory_resync( void *args )
{
    const struct xtajit64_memory_resync_params *params = args;
    struct range_array replacement = {0};
    NTSTATUS status = STATUS_SUCCESS;
    BOOL mutation = FALSE;

    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!params) status = STATUS_INVALID_PARAMETER;
    else if (!provider.initialized || provider.shutting_down) status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else if (params->generation != provider.generation) status = STATUS_RETRY;
    else if (!(status = begin_mutation_locked())) mutation = TRUE;
    if (!status) status = build_resync_registry( params, &replacement );
    if (!status) status = invalidate_all_code_locked();
    if (!status)
    {
        struct range_array old = provider.ranges;

        provider.ranges = replacement;
        memset( &replacement, 0, sizeof(replacement) );
        range_array_free( &old );
    }
    if (status && status != STATUS_RETRY && provider.initialized)
        poison_provider_locked( status );
    if (mutation) finish_mutation_locked();
    pthread_mutex_unlock( &provider.mutex );
    range_array_free( &replacement );
    return status;
}

static NTSTATUS flush_instruction_cache( void *args )
{
    const struct xtajit64_memory_params *params = args;
    uint64_t start, end;
    NTSTATUS status;
    BOOL mutation = FALSE;

    if (!params || !params->guest || !params->size ||
        !align_range( params->guest, params->size, &start, &end ) ||
        end - 1 > XTAJIT64_X64_USER_ADDRESS_MAX)
        return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &provider.mutex );
    if (!(status = begin_mutation_locked())) mutation = TRUE;
    if (!status) status = invalidate_code_locked( start, end - start );
    if (status && provider.initialized) poison_provider_locked( status );
    if (mutation) finish_mutation_locked();
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static NTSTATUS poison( void *args )
{
    const struct xtajit64_poison_params *params = args;
    NTSTATUS status;

    if (!params || !params->status || params->reserved)
        return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &provider.mutex );
    status = provider.initialized ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
    if (!status) poison_provider_locked( params->status );
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static BOOL observer_operation_is_valid( uint32_t operation )
{
    return operation >= WINE_WOW64_MEMORY_RESYNC &&
           operation <= WINE_WOW64_MEMORY_UNMAP;
}

static BOOL code_operation_is_valid( uint32_t operation )
{
    return operation >= WINE_ARM64EC_CODE_RESYNC &&
           operation <= WINE_ARM64EC_CODE_UNMAP;
}

static BOOL low_host_interval_to_guest( uint64_t host, uint64_t size,
                                        uint64_t *guest_start,
                                        uint64_t *guest_end )
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
            host_allocation_base - WINE_LOW_VA_SHADOW_BASE < WINE_LOW_VA_SHADOW_SIZE);
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

static NTSTATUS build_low_observer_ranges(
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
                            protection_to_perms( input->protect ) : 0;
            mapping.state = input->state;
            mapping.domain = XTAJIT64_MEMORY_ADDRESS_AMD64_LOW;
            if (!range_array_append( result, &mapping ))
                return STATUS_INVALID_PARAMETER;
        }
        cursor = input_end;
    }
    return cursor == guest_end ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static NTSTATUS build_low_observer_replacement( const struct range_array *captured,
                                                uint64_t start, uint64_t end,
                                                BOOL full_snapshot,
                                                struct range_array *result )
{
    struct range_array retained = {0};
    NTSTATUS status = STATUS_SUCCESS;
    size_t i;

    if (provider.ranges.count > (SIZE_MAX - 1) / 2 ||
        !range_array_reserve( &retained, provider.ranges.count * 2 + 1 ))
        return STATUS_NO_MEMORY;
    for (i = 0; i < provider.ranges.count; ++i)
    {
        const struct mapped_range *range = &provider.ranges.data[i];
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

static int32_t low_observer_begin( void *context, uint32_t operation,
                                   uint64_t host_address, uint64_t size,
                                   uint64_t host_allocation_base,
                                   void **transaction_ret )
{
    struct low_observer_transaction *transaction;
    uint64_t guest_start, guest_end;
    NTSTATUS status;

    if (!transaction_ret) return STATUS_INVALID_PARAMETER;
    *transaction_ret = NULL;
    if (context != &provider || !observer_operation_is_valid( operation ) ||
        !low_host_interval_to_guest( host_address, size, &guest_start, &guest_end ) ||
        !low_host_allocation_base_is_valid( host_allocation_base ))
        return STATUS_INVALID_PARAMETER;
    if (!(transaction = calloc( 1, sizeof(*transaction) ))) return STATUS_NO_MEMORY;

    pthread_mutex_lock( &provider.mutex );
    if (!(status = begin_mutation_locked()))
    {
        transaction->generation = provider.generation;
        transaction->operation = operation;
        provider.low_transaction = transaction;
        *transaction_ret = transaction;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (status) free( transaction );
    return status;
}

static void low_observer_complete(
    void *context, void *transaction_ptr,
    const struct wine_arm64ec_low_memory_event_v1 *event )
{
    struct low_observer_transaction *transaction = transaction_ptr;
    struct range_array captured = {0}, replacement = {0};
    uint64_t guest_start = 0, guest_end = 0;
    NTSTATUS status = STATUS_SUCCESS;
    BOOL full_snapshot = FALSE;
#ifdef XTAJIT64_FEX_UNIXLIB_TEST
    unsigned int test_stage = 1;
#endif

    pthread_mutex_lock( &provider.mutex );
    if (context != &provider || !transaction ||
        provider.low_transaction != transaction || !provider.mutating ||
        !provider.mutation_owner_valid ||
        !pthread_equal( provider.mutation_owner, pthread_self() ))
        status = STATUS_INVALID_DEVICE_STATE;
    else if (!event ||
             event->version != WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION ||
             event->size != sizeof(*event) ||
             event->operation != transaction->operation ||
             !observer_operation_is_valid( event->operation ) ||
             (event->flags & ~WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT) ||
             event->reserved[0] || event->reserved[1] ||
             event->range_count > XTAJIT64_MAX_RESYNC_RANGES ||
             (event->range_count && !event->ranges) ||
             !low_host_interval_to_guest( event->host_address,
                                          event->size_covered,
                                          &guest_start, &guest_end ) ||
             !low_host_allocation_base_is_valid( event->host_allocation_base ))
        status = STATUS_INVALID_PARAMETER;
    else
    {
#ifdef XTAJIT64_FEX_UNIXLIB_TEST
        test_stage = 2;
#endif
        full_snapshot = !!(event->flags & WINE_ARM64EC_LOW_MEMORY_EVENT_FULL_SNAPSHOT);
        if (full_snapshot &&
            (event->host_address != WINE_LOW_VA_SHADOW_BASE ||
             event->size_covered != WINE_LOW_VA_SHADOW_SIZE ||
             event->host_allocation_base))
            status = STATUS_INVALID_PARAMETER;
        else if (!full_snapshot && !provider.observer_active)
            status = STATUS_INVALID_DEVICE_STATE;
        else if (event->status)
            status = STATUS_SUCCESS; /* failed mutations have no post-state change */
        else if (event->snapshot_status)
            status = event->snapshot_status;
        else if (!(status = build_low_observer_ranges( event, guest_start,
                                                       guest_end, &captured )))
        {
#ifdef XTAJIT64_FEX_UNIXLIB_TEST
            test_stage = 3;
#endif
            status = build_low_observer_replacement( &captured, guest_start,
                                                     guest_end, full_snapshot,
                                                     &replacement );
        }
        if (!status && !event->status)
        {
#ifdef XTAJIT64_FEX_UNIXLIB_TEST
            test_stage = 4;
#endif
            status = full_snapshot ? invalidate_all_code_locked() :
                                     invalidate_code_locked( guest_start,
                                                             guest_end - guest_start );
        }
        if (!status && !event->status)
        {
            struct range_array old = provider.ranges;

            provider.ranges = replacement;
            memset( &replacement, 0, sizeof(replacement) );
            range_array_free( &old );
            if (full_snapshot) provider.observer_active = TRUE;
        }
    }

#ifdef XTAJIT64_FEX_UNIXLIB_TEST
    if (status) xtajit64_fex_test_observer_status( "low", status, test_stage );
#endif
    if (status) poison_provider_locked( status );
    if (provider.low_transaction == transaction) provider.low_transaction = NULL;
    if (provider.mutating && provider.mutation_owner_valid &&
        pthread_equal( provider.mutation_owner, pthread_self() ))
        finish_mutation_locked();
    pthread_mutex_unlock( &provider.mutex );
    range_array_free( &captured );
    range_array_free( &replacement );
    free( transaction );
}

static int32_t code_observer_begin( void *context, uint32_t operation,
                                    void **transaction_ret )
{
    struct code_observer_transaction *transaction;
    NTSTATUS status;

    if (!transaction_ret) return STATUS_INVALID_PARAMETER;
    *transaction_ret = NULL;
    if (context != &provider || !code_operation_is_valid( operation ))
        return STATUS_INVALID_PARAMETER;
    if (!(transaction = calloc( 1, sizeof(*transaction) ))) return STATUS_NO_MEMORY;

    pthread_mutex_lock( &provider.mutex );
    if (!(status = begin_mutation_locked()))
    {
        transaction->generation = provider.generation;
        transaction->operation = operation;
        provider.code_transaction = transaction;
        *transaction_ret = transaction;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (status) free( transaction );
    return status;
}

static void code_observer_complete(
    void *context, void *transaction_ptr,
    const struct wine_arm64ec_code_event_v1 *event )
{
    struct code_observer_transaction *transaction = transaction_ptr;
    NTSTATUS status = STATUS_SUCCESS;
    uint64_t previous_end = 0;
    BOOL full_invalidation = FALSE;
    size_t i;

    pthread_mutex_lock( &provider.mutex );
    if (context != &provider || !transaction ||
        provider.code_transaction != transaction || !provider.mutating ||
        !provider.mutation_owner_valid ||
        !pthread_equal( provider.mutation_owner, pthread_self() ))
        status = STATUS_INVALID_DEVICE_STATE;
    else if (!event || event->version != WINE_ARM64EC_CODE_OBSERVER_VERSION ||
             event->size != sizeof(*event) ||
             event->operation != transaction->operation ||
             !code_operation_is_valid( event->operation ) ||
             (event->flags & ~WINE_ARM64EC_CODE_EVENT_FULL_INVALIDATION) ||
             event->reserved || event->range_count > XTAJIT64_MAX_RESYNC_RANGES ||
             (event->range_count && !event->ranges))
        status = STATUS_INVALID_PARAMETER;
    else
    {
        full_invalidation = !!(event->flags & WINE_ARM64EC_CODE_EVENT_FULL_INVALIDATION);
        if (!full_invalidation && !provider.code_observer_active)
            status = STATUS_INVALID_DEVICE_STATE;
        for (i = 0; !status && i < event->range_count; ++i)
        {
            const struct wine_arm64ec_code_range_v1 *range = &event->ranges[i];
            uint64_t end = 0;

            if (!range->address || !range->size ||
                (range->address & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
                (range->size & (XTAJIT64_GUEST_PAGE_SIZE - 1)) ||
                range->address > UINT64_MAX - range->size ||
                range->address < previous_end ||
                (end = range->address + range->size) == 0 ||
                end - 1 > provider.highest_user_address)
                status = STATUS_INVALID_PARAMETER;
            if (!status) previous_end = end;
        }
        if (!status && !event->status)
        {
            if (full_invalidation) status = invalidate_all_code_locked();
            else for (i = 0; !status && i < event->range_count; ++i)
                status = invalidate_code_locked( event->ranges[i].address,
                                                 event->ranges[i].size );
            if (!status && full_invalidation) provider.code_observer_active = TRUE;
        }
    }

    if (status) poison_provider_locked( status );
    if (provider.code_transaction == transaction) provider.code_transaction = NULL;
    if (provider.mutating && provider.mutation_owner_valid &&
        pthread_equal( provider.mutation_owner, pthread_self() ))
        finish_mutation_locked();
    pthread_mutex_unlock( &provider.mutex );
    free( transaction );
}

static const struct wine_arm64ec_low_memory_observer_v1 low_memory_observer =
{
    WINE_ARM64EC_LOW_MEMORY_OBSERVER_VERSION,
    sizeof(low_memory_observer),
    &provider,
    low_observer_begin,
    low_observer_complete,
    WINE_ARM64EC_LOW_MEMORY_OBSERVER_CAP_EXACT_POST_SNAPSHOT,
};

static const struct wine_arm64ec_code_observer_v1 code_observer =
{
    WINE_ARM64EC_CODE_OBSERVER_VERSION,
    sizeof(code_observer),
    &provider,
    code_observer_begin,
    code_observer_complete,
    WINE_ARM64EC_CODE_OBSERVER_CAP_EXACT_INVALIDATION_RANGES,
};

static int32_t repair_jit_fault_signal(
    void *context, struct wine_arm64ec_jit_host_context_v1 *host_context,
    uint32_t access, uint32_t flags, uint64_t fault_address )
{
    struct switchyard_fex_arm64_host_context fex_context;
    struct thread_binding *binding;
    enum switchyard_fex_result result;

    binding = __atomic_load_n( &active_signal_binding, __ATOMIC_ACQUIRE );
    if (context != &provider || !binding || !binding->thread)
        return STATUS_NOT_SUPPORTED;
    if (!host_context || host_context->size != sizeof(*host_context) ||
        host_context->version != WINE_ARM64EC_JIT_HOST_CONTEXT_VERSION ||
        host_context->flags || host_context->reserved || !host_context->pc ||
        (host_context->pc & 3) || access > 1 ||
        (flags & ~WINE_ARM64EC_JIT_SIGNAL_ALIGNMENT_FAULT))
        return STATUS_INVALID_PARAMETER;

    memcpy( &fex_context, host_context, sizeof(fex_context) );
    fex_context.version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION;
    if (flags & WINE_ARM64EC_JIT_SIGNAL_ALIGNMENT_FAULT)
        result = switchyard_fex_thread_repair_unaligned_tso( binding->thread,
                                                             &fex_context );
    else
        result = switchyard_fex_thread_repair_callret_fault( binding->thread,
            &fex_context, access, fault_address );
    if (result != SWITCHYARD_FEX_OK) return fex_result_to_status( result );
    if (fex_context.size != sizeof(fex_context) ||
        fex_context.version != SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION ||
        fex_context.flags || fex_context.reserved || !fex_context.pc ||
        (fex_context.pc & 3))
        return STATUS_UNSUCCESSFUL;

    fex_context.version = WINE_ARM64EC_JIT_HOST_CONTEXT_VERSION;
    memcpy( host_context, &fex_context, sizeof(*host_context) );
    return STATUS_SUCCESS;
}

static int32_t query_exception_stack_signal(
    void *context, const struct wine_arm64ec_jit_host_context_v1 *host_context,
    uint32_t access, uint64_t fault_address, uint64_t *guest_stack )
{
    struct switchyard_fex_arm64_host_context fex_context;
    struct thread_binding *binding;

    binding = __atomic_load_n( &active_signal_binding, __ATOMIC_ACQUIRE );
    if (context != &provider || !binding || !binding->thread)
        return STATUS_NOT_SUPPORTED;
    if (!host_context || host_context->size != sizeof(*host_context) ||
        host_context->version != WINE_ARM64EC_JIT_HOST_CONTEXT_VERSION ||
        host_context->flags || host_context->reserved || !guest_stack)
        return STATUS_INVALID_PARAMETER;
    memcpy( &fex_context, host_context, sizeof(fex_context) );
    fex_context.version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION;
    return fex_result_to_status( switchyard_fex_thread_query_jit_stack(
        binding->thread, &fex_context, access, fault_address, guest_stack ) );
}

static const struct wine_arm64ec_jit_signal_observer_v3 jit_signal_observer =
{
    WINE_ARM64EC_JIT_SIGNAL_OBSERVER_VERSION,
    sizeof(jit_signal_observer),
    0,
    0,
    &provider,
    repair_jit_fault_signal,
    query_exception_stack_signal,
    WINE_ARM64EC_JIT_SIGNAL_OBSERVER_CAPABILITIES,
};

static NTSTATUS register_provider_observers(void)
{
    int32_t status;

    status = __wine_register_arm64ec_low_memory_observer_v1( &low_memory_observer );
    if (status) return status;
    status = __wine_register_arm64ec_code_observer_v1( &code_observer );
    if (status) return status;
    return __wine_register_arm64ec_jit_signal_observer_v3( &jit_signal_observer );
}

static void remove_binding_locked( struct thread_binding *binding )
{
    struct thread_binding **cursor;

    for (cursor = &provider.bindings; *cursor; cursor = &(*cursor)->next)
    {
        if (*cursor != binding) continue;
        /* Keep unlisted adapter membership closed until thread destruction.
         * A concurrent invalidator also scans these not-yet-freed cells. */
        if (binding->admission) switchyard_fex_admission_close( binding->admission );
        *cursor = binding->next;
        binding->next = NULL;
        break;
    }
}

/* Membership also pins process lifetime. Keep the provider mutex through the
 * cold adapter teardown so process_term cannot overtake the last detach. */
static NTSTATUS destroy_binding_locked( struct thread_binding *binding )
{
    enum switchyard_fex_result result;
    NTSTATUS status;

    if (binding->admission) switchyard_fex_admission_close( binding->admission );
    result = switchyard_fex_thread_destroy( binding->thread );
    if (result != SWITCHYARD_FEX_OK)
    {
        status = fex_result_to_status( result );
        poison_provider_locked( status );
        return status; /* Keep a failed teardown pinned and fail closed. */
    }
    binding->admission = NULL;
    binding->thread = NULL;
    remove_binding_locked( binding );
    return STATUS_SUCCESS;
}

static void destroy_thread_binding( void *value )
{
    struct thread_binding *binding = value;
    BOOL release_binding = FALSE;

    if (!binding) return;
    if (__atomic_load_n( &active_signal_binding, __ATOMIC_ACQUIRE ) == binding)
        __atomic_store_n( &active_signal_binding, NULL, __ATOMIC_RELEASE );
    pthread_mutex_lock( &provider.mutex );
    if (provider.forked_child)
    {
        binding->thread = NULL;
        release_binding = TRUE;
    }
    else if (!binding_is_active( binding ))
    {
        release_binding = !destroy_binding_locked( binding );
    }
    pthread_mutex_unlock( &provider.mutex );
    if (release_binding) free( binding );
}

static void make_binding_key(void)
{
    binding_key_error = pthread_key_create( &binding_key, destroy_thread_binding );
}

static NTSTATUS process_init( void *args )
{
    struct xtajit64_process_init_params *params = args;
    struct switchyard_fex_config config;
    struct switchyard_fex_process *process = NULL;
    struct mapped_range kuser;
    enum switchyard_fex_result result;
    NTSTATUS status;
    long native_page_size;

    TRACE( "CPU provider interface %s\n", XTAJIT64_PROVIDER_ABI_IDENTITY );
    if (!params) return STATUS_INVALID_PARAMETER;
    if (params->abi_version != XTAJIT64_PROCESS_ABI_VERSION ||
        params->abi_size != sizeof(*params) ||
        (params->required_capabilities & XTAJIT64_CAPABILITIES) != XTAJIT64_CAPABILITIES ||
        (params->required_capabilities & ~XTAJIT64_CAPABILITIES) ||
        params->enabled_capabilities || params->native_page_size)
        return STATUS_REVISION_MISMATCH;
    if (!params->ec_bitmap || (params->ec_bitmap & (sizeof(uint64_t) - 1)) ||
        !params->highest_user_address ||
        params->highest_user_address > XTAJIT64_X64_USER_ADDRESS_MAX ||
        !params->rtl_exit_user_thread ||
        params->rtl_exit_user_thread > params->highest_user_address ||
        (params->rtl_query_performance_counter &&
         params->rtl_query_performance_counter > params->highest_user_address) ||
        (params->nt_query_performance_counter &&
         params->nt_query_performance_counter > params->highest_user_address) ||
        !params->x64_syscall_dispatcher ||
        params->x64_syscall_dispatcher > params->highest_user_address ||
        !params->x64_syscall_count ||
        params->x64_syscall_count > XTAJIT64_MAX_SYSCALL_COUNT ||
        (params->reserved & ~XTAJIT64_PROCESS_CUSTOM_DISPATCH) ||
        params->guest_kuser != XTAJIT64_GUEST_KUSER ||
        !params->host_kuser ||
        params->kuser_size < XTAJIT64_GUEST_PAGE_SIZE ||
        params->kuser_size > XTAJIT64_MAX_HOST_PAGE_SIZE ||
        (params->kuser_size & (params->kuser_size - 1)) ||
        (params->guest_kuser & (params->kuser_size - 1)) ||
        (params->host_kuser & (params->kuser_size - 1)) ||
        params->guest_kuser > params->highest_user_address ||
        params->kuser_size - 1 > params->highest_user_address - params->guest_kuser ||
        params->host_kuser > UINT64_MAX - params->kuser_size)
        return STATUS_INVALID_PARAMETER;
    if (switchyard_fex_abi_version() != SWITCHYARD_FEX_ABI_VERSION ||
        strcmp( switchyard_fex_provider_abi_identity(),
                SWITCHYARD_FEX_PROVIDER_ABI_IDENTITY ))
        return STATUS_REVISION_MISMATCH;

    native_page_size = sysconf( _SC_PAGESIZE );
    if (native_page_size < XTAJIT64_GUEST_PAGE_SIZE ||
        native_page_size > XTAJIT64_MAX_HOST_PAGE_SIZE ||
        (native_page_size & (native_page_size - 1)))
        return STATUS_NOT_SUPPORTED;

    pthread_once( &binding_key_once, make_binding_key );
    if (binding_key_error) return STATUS_NO_MEMORY;
    pthread_once( &atfork_once, register_atfork_handlers );
    if (atfork_error)
        return atfork_error == ENOMEM ? STATUS_NO_MEMORY : STATUS_UNSUCCESSFUL;

    pthread_mutex_lock( &provider.mutex );
    status = provider.forked_child ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    pthread_mutex_unlock( &provider.mutex );
    if (status) return status;

    memset( &config, 0, sizeof(config) );
    config.size = sizeof(config);
    config.abi_version = SWITCHYARD_FEX_ABI_VERSION;
    config.flags = SWITCHYARD_FEX_CONFIG_MULTIBLOCK | SWITCHYARD_FEX_CONFIG_EXTERNAL_ADMISSION;
    if (params->reserved & XTAJIT64_PROCESS_CUSTOM_DISPATCH)
        config.flags |= SWITCHYARD_FEX_CONFIG_CUSTOM_DISPATCH;
    config.low_va_shadow_base = WINE_LOW_VA_SHADOW_BASE;
    config.low_va_shadow_size = WINE_LOW_VA_SHADOW_SIZE;
    config.ec_code_bitmap = params->ec_bitmap;
    config.highest_user_address = params->highest_user_address;
    config.ec_page_shift = __builtin_ctzll( params->kuser_size );
    result = switchyard_fex_process_create( &config, &process );
    if (result != SWITCHYARD_FEX_OK)
    {
        ERR( "cannot create Switchyard FEX process: %s\n",
             switchyard_fex_result_string( result ) );
        return fex_result_to_status( result );
    }
    result = switchyard_fex_process_set_executable_range_query(
        process, query_guest_executable_range, &provider );
    if (result != SWITCHYARD_FEX_OK)
    {
        switchyard_fex_process_destroy( process );
        return fex_result_to_status( result );
    }

    memset( &kuser, 0, sizeof(kuser) );
    kuser.guest = params->guest_kuser;
    kuser.host = params->host_kuser;
    kuser.size = params->kuser_size;
    kuser.allocation_base = params->guest_kuser;
    kuser.perms = FEX_PERM_READ;
    kuser.state = MEM_COMMIT;
    kuser.domain = XTAJIT64_MEMORY_ADDRESS_INVALID;
    kuser.permanent = TRUE;

    pthread_mutex_lock( &provider.mutex );
    while (provider.shutting_down)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (provider.initialized) status = STATUS_ALREADY_INITIALIZED;
    else if (provider.instance == UINT64_MAX) status = STATUS_INTEGER_OVERFLOW;
    else if (!range_array_append( &provider.ranges, &kuser )) status = STATUS_NO_MEMORY;
    else
    {
        ++provider.instance;
        provider.process = process;
        provider.custom_dispatch = !!(params->reserved & XTAJIT64_PROCESS_CUSTOM_DISPATCH);
        provider.ec_bitmap = (const uint64_t *)(uintptr_t)params->ec_bitmap;
        provider.highest_user_address = params->highest_user_address;
        provider.ec_page_shift = config.ec_page_shift;
        provider.native_page_size = native_page_size;
        provider.x64_syscall_dispatcher = params->x64_syscall_dispatcher;
        provider.x64_syscall_count = params->x64_syscall_count;
        provider.guest_kuser = params->guest_kuser;
        provider.host_kuser = params->host_kuser;
        provider.kuser_size = params->kuser_size;
        provider.generation = 1;
        provider.next_binding_id = 0;
        provider.poison_status = STATUS_SUCCESS;
        provider.observer_active = FALSE;
        provider.code_observer_active = FALSE;
        provider.signal_observer_active = FALSE;
        provider.shutting_down = FALSE;
        provider.initialized = TRUE;
        status = STATUS_SUCCESS;
    }
    pthread_mutex_unlock( &provider.mutex );
    if (status)
    {
        switchyard_fex_process_destroy( process );
        return status;
    }

    status = register_provider_observers();
    pthread_mutex_lock( &provider.mutex );
    if (!status) provider.signal_observer_active = TRUE;
    if (!status && provider.poison_status) status = provider.poison_status;
    if (!status && (!provider.observer_active || !provider.code_observer_active ||
                    !provider.signal_observer_active))
        status = STATUS_INVALID_DEVICE_STATE;
    if (!status)
    {
        params->enabled_capabilities = XTAJIT64_CAPABILITIES;
        params->native_page_size = native_page_size;
    }
    else poison_provider_locked( status );
    pthread_mutex_unlock( &provider.mutex );
    if (status) return status;

    TRACE( "initialized FEX provider %s upstream %s, low shadow %p-%p, "
           "syscall dispatcher %p count %u\n",
           switchyard_fex_provider_abi_identity(),
           switchyard_fex_upstream_revision(),
           (void *)(uintptr_t)WINE_LOW_VA_SHADOW_BASE,
           (void *)(uintptr_t)(WINE_LOW_VA_SHADOW_BASE + WINE_LOW_VA_SHADOW_SIZE),
           (void *)(uintptr_t)params->x64_syscall_dispatcher,
           params->x64_syscall_count );
    return STATUS_SUCCESS;
}

static NTSTATUS process_term( void *args )
{
    struct switchyard_fex_process *process = NULL;
    enum switchyard_fex_result result;
    NTSTATUS status = STATUS_SUCCESS;

    (void)args;
    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized)
    {
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_SUCCESS;
    }
    if (provider.bindings)
    {
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_DEVICE_BUSY;
    }
    provider.shutting_down = TRUE;
    provider.initialized = FALSE;
    process = provider.process;
    provider.process = NULL;
    range_array_free( &provider.ranges );
    provider.ec_bitmap = NULL;
    provider.highest_user_address = 0;
    provider.ec_page_shift = 0;
    provider.native_page_size = 0;
    provider.x64_syscall_dispatcher = 0;
    provider.x64_syscall_count = 0;
    provider.guest_kuser = 0;
    provider.host_kuser = 0;
    provider.kuser_size = 0;
    provider.observer_active = FALSE;
    provider.code_observer_active = FALSE;
    provider.signal_observer_active = FALSE;
    provider.poison_status = STATUS_SUCCESS;
    pthread_mutex_unlock( &provider.mutex );

    result = switchyard_fex_process_destroy( process );
    status = fex_result_to_status( result );
    pthread_mutex_lock( &provider.mutex );
    provider.shutting_down = FALSE;
    pthread_cond_broadcast( &provider.cond );
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static NTSTATUS thread_init( void *args )
{
    const struct switchyard_fex_execution_domain domain =
    {
        .size = sizeof(domain), .version = SWITCHYARD_FEX_DOMAIN_VERSION,
        .wake = wake_shared_execution, .context = &provider,
    };
    struct thread_binding *binding, *current;
    struct switchyard_fex_thread *thread = NULL;
    enum switchyard_fex_result result;
    NTSTATUS status = STATUS_SUCCESS;
    int ret;

    (void)args;
    /* Materialize Darwin TLS in ordinary context before any JIT signal. */
    (void)__atomic_load_n( &active_signal_binding, __ATOMIC_ACQUIRE );
    pthread_once( &binding_key_once, make_binding_key );
    if (binding_key_error) return STATUS_NO_MEMORY;
    current = pthread_getspecific( binding_key );
    if (!(binding = calloc( 1, sizeof(*binding) ))) return STATUS_NO_MEMORY;

    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down) status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else if (current && current->process_instance == provider.instance)
        status = STATUS_SUCCESS;
    else if (provider.next_binding_id == UINT64_MAX) status = STATUS_INTEGER_OVERFLOW;
    else
    {
        result = switchyard_fex_thread_create_with_domain( provider.process, &domain,
                                                          &thread, &binding->admission );
        if (result != SWITCHYARD_FEX_OK) status = fex_result_to_status( result );
    }
    if (!status && (!current || current->process_instance != provider.instance))
    {
        binding->thread = thread;
        binding->owner = pthread_self();
        binding->process_instance = provider.instance;
        binding->id = ++provider.next_binding_id;
        binding->next = provider.bindings;
        provider.bindings = binding;
        if ((ret = pthread_setspecific( binding_key, binding )))
        {
            if (destroy_binding_locked( binding )) binding = NULL;
            status = ret == ENOMEM ? STATUS_NO_MEMORY : STATUS_UNSUCCESSFUL;
        }
    }
    pthread_mutex_unlock( &provider.mutex );

    if (!status && current && current->process_instance == provider.instance)
        free( binding );
    else if (status)
    {
        free( binding );
    }
    else if (current) destroy_thread_binding( current );
    return status;
}

static NTSTATUS thread_term( void *args )
{
    struct thread_binding *binding;
    NTSTATUS status;
    int ret;

    (void)args;
    pthread_once( &binding_key_once, make_binding_key );
    if (binding_key_error) return STATUS_UNSUCCESSFUL;
    if (!(binding = pthread_getspecific( binding_key ))) return STATUS_SUCCESS;

    pthread_mutex_lock( &provider.mutex );
    if (provider.forked_child)
    {
        pthread_mutex_unlock( &provider.mutex );
        pthread_setspecific( binding_key, NULL );
        binding->thread = NULL;
        free( binding );
        return STATUS_SUCCESS;
    }
    if (binding_is_active( binding ))
    {
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_INVALID_DEVICE_STATE;
    }
    if ((ret = pthread_setspecific( binding_key, NULL )))
    {
        pthread_mutex_unlock( &provider.mutex );
        return ret == ENOMEM ? STATUS_NO_MEMORY : STATUS_UNSUCCESSFUL;
    }
    status = destroy_binding_locked( binding );
    pthread_mutex_unlock( &provider.mutex );
    if (!status) free( binding );
    return status;
}

static NTSTATUS flight_bind( void *args )
{
    const struct xtajit64_flight_bind_params *params = args;
    struct thread_binding *binding;
    uint64_t host, allocation, authenticated_teb, boundary;
    unsigned int domain;
    uint32_t reason;
    NTSTATUS status = STATUS_SUCCESS;

    pthread_once( &binding_key_once, make_binding_key );
    if (binding_key_error || !(binding = pthread_getspecific( binding_key )))
        return STATUS_INVALID_HANDLE;
    if (!params) return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down ||
        binding->process_instance != provider.instance)
        status = STATUS_INVALID_HANDLE;
    else if (binding_is_active( binding )) status = STATUS_INVALID_DEVICE_STATE;
    else if (!params->recorder) clear_flight_binding( binding );
    else if ((params->recorder & 63) || !params->causal_boundary_id ||
             params->recorder > XTAJIT64_X64_USER_ADDRESS_MAX ||
             params->recorder > UINT64_MAX - sizeof(*binding->flight_recorder) ||
             !translate_guest_range_locked( params->recorder,
                                            sizeof(*binding->flight_recorder),
                                            FEX_PERM_READ | FEX_PERM_WRITE,
                                            &host, &allocation, &domain ) ||
             host != params->recorder ||
             domain != XTAJIT64_MEMORY_ADDRESS_IDENTITY ||
             !xtajit64_flight_recorder_is_valid(
                 (const struct xtajit64_flight_recorder *)(uintptr_t)host ))
        status = STATUS_INVALID_ADDRESS;
    else
    {
        binding->flight_recorder =
            (struct xtajit64_flight_recorder *)(uintptr_t)host;
        binding->flight_causal_boundary_id = params->causal_boundary_id;
        binding->flight_context_generation = params->context_generation;
        binding->flight_transition_generation = params->transition_generation;
        binding->flight_claimed_teb = params->claimed_teb;
        authenticated_teb = (uint64_t)(uintptr_t)NtCurrentTeb();
        binding->flight_expected_teb = authenticated_teb;
        __atomic_store_n( &binding->flight_recorder->authenticated_teb,
                          authenticated_teb, __ATOMIC_RELEASE );
        binding->flight_guest_rip = params->guest_rip;
        binding->flight_guest_rsp = params->guest_rsp;
        binding->flight_guest_stack_limit = params->guest_stack_limit;
        binding->flight_guest_stack_base = params->guest_stack_base;
        binding->flight_control_stack_limit = params->control_stack_limit;
        binding->flight_control_stack_top = params->control_stack_top;
        boundary = xtajit64_flight_current_boundary( binding->flight_recorder );
        if (params->context_generation > binding->flight_last_context_generation)
            binding->flight_last_context_generation = params->context_generation;
        reason = xtajit64_flight_validate_pe_x18_claim( params->claimed_teb,
                                                        authenticated_teb );
        if (!reason && (boundary != params->causal_boundary_id ||
                        params->context_generation != params->causal_boundary_id ||
                        params->transition_generation != params->causal_boundary_id))
            reason = XTAJIT64_FLIGHT_REASON_CONTEXT_STALE_PUBLICATION;
        record_flight_event( binding, XTAJIT64_FLIGHT_EVENT_BINDING, reason,
                             XTAJIT64_FLIGHT_UNKNOWN_U32,
                             params->claimed_teb, authenticated_teb );
    }
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static NTSTATUS export_thread_state( struct thread_binding *binding,
                                     struct switchyard_fex_x64_state *state,
                                     struct xtajit64_begin_params *params )
{
    enum switchyard_fex_result result;

    init_fex_state_output( state );
    result = switchyard_fex_thread_export_state( binding->thread, state );
    if (result != SWITCHYARD_FEX_OK) return fex_result_to_status( result );
    export_fex_state( &params->context, state );
    binding->flight_guest_rip = params->context.rip;
    binding->flight_guest_rsp = params->context.rsp;
    return STATUS_SUCCESS;
}

#define ASSERT_REGISTER_WINDOW_OFFSET(field, offset) \
    C_ASSERT( offsetof(struct xtajit64_x64_context, field) == SWITCHYARD_FEX_WINDOW_##offset )
C_ASSERT( sizeof(struct xtajit64_x64_context) == SWITCHYARD_FEX_REGISTER_WINDOW_DATA_SIZE_V1 );
ASSERT_REGISTER_WINDOW_OFFSET( rax, RAX );
ASSERT_REGISTER_WINDOW_OFFSET( rbx, RBX );
ASSERT_REGISTER_WINDOW_OFFSET( rcx, RCX );
ASSERT_REGISTER_WINDOW_OFFSET( rdx, RDX );
ASSERT_REGISTER_WINDOW_OFFSET( rsi, RSI );
ASSERT_REGISTER_WINDOW_OFFSET( rdi, RDI );
ASSERT_REGISTER_WINDOW_OFFSET( rbp, RBP );
ASSERT_REGISTER_WINDOW_OFFSET( rsp, RSP );
ASSERT_REGISTER_WINDOW_OFFSET( r8, R8 );
ASSERT_REGISTER_WINDOW_OFFSET( r9, R9 );
ASSERT_REGISTER_WINDOW_OFFSET( r10, R10 );
ASSERT_REGISTER_WINDOW_OFFSET( r11, R11 );
ASSERT_REGISTER_WINDOW_OFFSET( r12, R12 );
ASSERT_REGISTER_WINDOW_OFFSET( r13, R13 );
ASSERT_REGISTER_WINDOW_OFFSET( r14, R14 );
ASSERT_REGISTER_WINDOW_OFFSET( r15, R15 );
ASSERT_REGISTER_WINDOW_OFFSET( rip, RIP );
ASSERT_REGISTER_WINDOW_OFFSET( eflags, EFLAGS );
ASSERT_REGISTER_WINDOW_OFFSET( mxcsr, MXCSR );
ASSERT_REGISTER_WINDOW_OFFSET( reserved, RESERVED );
ASSERT_REGISTER_WINDOW_OFFSET( xmm, XMM );
#undef ASSERT_REGISTER_WINDOW_OFFSET

static NTSTATUS export_thread_register_window( struct thread_binding *binding,
                                              struct xtajit64_begin_params *params )
{
    const struct switchyard_fex_register_window window = {
        .size = sizeof(window), .version = SWITCHYARD_FEX_REGISTER_WINDOW_VERSION,
        .data = (uintptr_t)&params->context, .data_size = sizeof(params->context),
    };
    enum switchyard_fex_result result;

    result = switchyard_fex_thread_export_register_window( binding->thread, &window );
    if (result != SWITCHYARD_FEX_OK) return fex_result_to_status( result );
    binding->flight_guest_rip = params->context.rip;
    binding->flight_guest_rsp = params->context.rsp;
    return STATUS_SUCCESS;
}

/* Only this cold continuation materializes the complete architectural state.
 * Native-entry defaults would destroy the guest's x87/segment/YMM state here. */
static NTSTATUS __attribute__((noinline)) resume_syscall_locked(
    struct thread_binding *binding, struct xtajit64_begin_params *params,
    const struct switchyard_fex_stop *stop, enum switchyard_fex_result *result )
{
    struct switchyard_fex_x64_state state;
    NTSTATUS status = export_thread_state( binding, &state, params );

    if (status) return status;
    if (state.gpr[0] >= provider.x64_syscall_count)
    {
        state.gpr[0] = STATUS_INVALID_SYSTEM_SERVICE;
        state.rip = stop->rip;
    }
    else
    {
        state.gpr[1] = state.gpr[10];
        state.gpr[10] = stop->rip;
        state.rip = provider.x64_syscall_dispatcher;
    }
    *result = switchyard_fex_thread_import_state( binding->thread, &state );
    if (*result != SWITCHYARD_FEX_OK) return fex_result_to_status( *result );
    export_fex_state( &params->context, &state );
    return STATUS_SUCCESS;
}

static NTSTATUS run_simulation( struct xtajit64_begin_params *params,
                                struct xtajit64_dispatch_params *dispatch,
                                BOOL complete )
{
    struct switchyard_fex_execution execution;
    struct switchyard_fex_register_window window;
    struct switchyard_fex_stop stop;
    struct thread_binding *binding;
    enum switchyard_fex_result result;
    uint64_t doorbell_host, doorbell_allocation, execution_generation = 0;
    unsigned int doorbell_domain;
    NTSTATUS status = STATUS_SUCCESS;
    BOOL internal_suspend;

    pthread_once( &binding_key_once, make_binding_key );
    if (binding_key_error || !(binding = pthread_getspecific( binding_key )))
        return STATUS_INVALID_HANDLE;
    memset( &stop, 0, sizeof(stop) );
    stop.size = sizeof(stop);
    stop.version = SWITCHYARD_FEX_STOP_VERSION;
    if (complete)
    {
        if (!dispatch) return STATUS_INVALID_PARAMETER;
        /* Do not wait for a mutator here: it is waiting for this execution.
         * Validate before consuming either lease; an old PE stack must not
         * release a newer invocation on the same attached thread. */
        pthread_mutex_lock( &provider.mutex );
        if (!provider.initialized || !provider.custom_dispatch ||
            !binding_is_active( binding ) || !binding->thread ||
            !binding->dispatch.generation ||
            dispatch->generation != binding->dispatch.generation ||
            dispatch->binding_id != binding->id ||
            dispatch->process_instance != provider.instance ||
            binding->process_instance != provider.instance ||
            __atomic_load_n( &active_signal_binding, __ATOMIC_ACQUIRE ) != binding)
        {
            pthread_mutex_unlock( &provider.mutex );
            return STATUS_INVALID_DEVICE_STATE;
        }
        pthread_mutex_unlock( &provider.mutex );
        execution_generation = dispatch->generation;
        result = switchyard_fex_thread_complete_dispatch(
            binding->thread, dispatch->generation, &stop );
        /* BUSY means ownership was not consumed. Keep the shared lease. */
        if (result == SWITCHYARD_FEX_ERROR_BUSY) return STATUS_INVALID_DEVICE_STATE;
        dispatch->entry = dispatch->frame = dispatch->generation = 0;
        goto execution_complete;
    }
    if (dispatch)
    {
        dispatch->entry = dispatch->frame = dispatch->generation = 0;
        dispatch->binding_id = dispatch->process_instance = 0;
    }
    if (!params || !params->context.rip ||
        params->context.rip > XTAJIT64_X64_USER_ADDRESS_MAX ||
        !params->context.rsp ||
        params->context.rsp > XTAJIT64_X64_USER_ADDRESS_MAX ||
        !params->gs_base || params->gs_base > XTAJIT64_X64_USER_ADDRESS_MAX ||
        !params->suspend_doorbell ||
        (params->suspend_doorbell & (sizeof(uint32_t) - 1)))
        return STATUS_INVALID_PARAMETER;

    memset( &execution, 0, sizeof(execution) );
    execution.size = sizeof(execution);
    execution.version = SWITCHYARD_FEX_EXECUTION_VERSION;
    execution.suspend_doorbell = params->suspend_doorbell;

    pthread_mutex_lock( &provider.mutex );
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (!provider.initialized || provider.shutting_down ||
        binding->process_instance != provider.instance || !binding->thread)
        status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else if (binding_is_active( binding )) status = STATUS_INVALID_DEVICE_STATE;
    else if (!!dispatch != provider.custom_dispatch) status = STATUS_NOT_SUPPORTED;
    else if (dispatch && params->gs_base != (uint64_t)(uintptr_t)NtCurrentTeb())
        status = STATUS_INVALID_PARAMETER;
    else if (!translate_guest_range_locked( params->suspend_doorbell,
                                            sizeof(uint32_t),
                                            FEX_PERM_READ | FEX_PERM_WRITE,
                                            &doorbell_host,
                                            &doorbell_allocation,
                                            &doorbell_domain ) ||
             doorbell_host != params->suspend_doorbell ||
             doorbell_domain != XTAJIT64_MEMORY_ADDRESS_IDENTITY)
        status = STATUS_INVALID_ADDRESS;
    else
    {
        binding->doorbell = (volatile uint32_t *)(uintptr_t)doorbell_host;
        refresh_flight_binding_locked( binding, params );
    }
    if (status)
    {
        params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
        params->provider_error = status;
        pthread_mutex_unlock( &provider.mutex );
        return status;
    }

    if (__atomic_load_n( (uint32_t *)(uintptr_t)binding->doorbell,
                         __ATOMIC_ACQUIRE ))
    {
        params->transition_target = 0;
        params->fault_address = 0;
        params->fault_access = EXCEPTION_READ_FAULT;
        params->stop_reason = XTAJIT64_STOP_SUSPEND;
        params->provider_error = SWITCHYARD_FEX_OK;
        binding->doorbell = NULL;
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_SUCCESS;
    }

    window = (struct switchyard_fex_register_window){
        .size = sizeof(window), .version = SWITCHYARD_FEX_REGISTER_WINDOW_VERSION,
        .data = (uintptr_t)&params->context, .data_size = sizeof(params->context),
        .gs_base = params->gs_base,
    };
    result = switchyard_fex_thread_import_register_window( binding->thread, &window );
    if (result != SWITCHYARD_FEX_OK)
    {
        status = fex_result_to_status( result );
        params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
        params->provider_error = result;
        binding->doorbell = NULL;
        poison_provider_locked( status );
        pthread_mutex_unlock( &provider.mutex );
        return status;
    }

execute_again:
    if (provider.poison_status) status = provider.poison_status;
    else if (!provider.initialized || provider.shutting_down)
        status = STATUS_INVALID_HANDLE;
    if (status)
    {
        params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
        params->provider_error = status;
        binding->doorbell = NULL;
        pthread_mutex_unlock( &provider.mutex );
        return status;
    }

    /* Reserve the SAME cell observed by mutation before dropping its mutex.
     * Neither path may create an admission gap between the provider and FEX. */
    memset( &execution, 0, sizeof(execution) );
    execution.size = sizeof(execution);
    execution.version = SWITCHYARD_FEX_EXECUTION_VERSION;
    execution.suspend_doorbell = (uintptr_t)binding->doorbell;
    if (dispatch)
    {
        memset( &binding->dispatch, 0, sizeof(binding->dispatch) );
        binding->dispatch.size = sizeof(binding->dispatch);
        binding->dispatch.version = SWITCHYARD_FEX_DISPATCH_VERSION;
        /* Only Unix NtCurrentTeb authenticates custom x18. */
        result = switchyard_fex_thread_prepare_dispatch(
            binding->thread, &execution, (uintptr_t)NtCurrentTeb(), &binding->dispatch );
        execution_generation = binding->dispatch.generation;
    }
    else result = switchyard_fex_thread_prepare_execution( binding->thread, &execution, &execution_generation );
    if (result != SWITCHYARD_FEX_OK)
    {
        status = fex_result_to_status( result );
        params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
        params->provider_error = result;
        binding->doorbell = NULL;
        poison_provider_locked( status );
        pthread_mutex_unlock( &provider.mutex );
        return status;
    }
    record_flight_event( binding, XTAJIT64_FLIGHT_EVENT_PROVIDER_BEGIN,
                         XTAJIT64_FLIGHT_REASON_NONE,
                         XTAJIT64_FLIGHT_UNKNOWN_U32,
                         params->context.rip, params->context.rsp );
#ifdef XTAJIT64_FEX_UNIXLIB_TEST
    xtajit64_fex_test_execution_prepared();
#endif
    pthread_mutex_unlock( &provider.mutex );

    memset( &stop, 0, sizeof(stop) );
    stop.size = sizeof(stop);
    stop.version = SWITCHYARD_FEX_STOP_VERSION;
    __atomic_store_n( &active_signal_binding, binding, __ATOMIC_RELEASE );
    if (dispatch)
    {
        dispatch->entry = binding->dispatch.entry;
        dispatch->frame = binding->dispatch.frame;
        dispatch->generation = binding->dispatch.generation;
        dispatch->binding_id = binding->id;
        dispatch->process_instance = binding->process_instance;
        return STATUS_SUCCESS;
    }
    else result = switchyard_fex_thread_execute_prepared( binding->thread, execution_generation, &stop );

execution_complete:
    __atomic_store_n( &active_signal_binding, NULL, __ATOMIC_RELEASE );

    pthread_mutex_lock( &provider.mutex );
    binding->dispatch.generation = 0;
    internal_suspend = stop.reason == SWITCHYARD_FEX_STOP_SUSPEND &&
                       binding->pause_generation == execution_generation;
    while (provider.mutating && provider.initialized)
        pthread_cond_wait( &provider.cond, &provider.mutex );
    if (internal_suspend && provider.initialized && !provider.poison_status &&
        !__atomic_load_n( (uint32_t *)(uintptr_t)binding->doorbell,
                          __ATOMIC_ACQUIRE ))
        goto execute_again;

    if (provider.poison_status) status = provider.poison_status;
    else if (!provider.initialized || provider.shutting_down)
        status = STATUS_INVALID_HANDLE;
    else if (result != SWITCHYARD_FEX_OK &&
             result != SWITCHYARD_FEX_ERROR_GUEST_FAULT)
        status = fex_result_to_status( result );
    else if (stop.reason == SWITCHYARD_FEX_STOP_SYSCALL)
    {
        status = resume_syscall_locked( binding, params, &stop, &result );
        if (!status) goto execute_again;
    }
    else status = export_thread_register_window( binding, params );

    params->transition_target = 0;
    params->fault_address = 0;
    params->fault_access = EXCEPTION_READ_FAULT;
    params->reserved = 0;
    params->provider_error = result;
    if (status)
    {
        params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
        poison_provider_locked( status );
    }
    else switch (stop.reason)
    {
    case SWITCHYARD_FEX_STOP_EC_TRANSITION:
        params->transition_target = stop.rip;
        params->stop_reason = XTAJIT64_STOP_EC_TRANSITION;
        break;
    case SWITCHYARD_FEX_STOP_SUSPEND:
        params->stop_reason = XTAJIT64_STOP_SUSPEND;
        break;
    case SWITCHYARD_FEX_STOP_INVALID_INSTRUCTION:
        params->stop_reason = XTAJIT64_STOP_INVALID_INSTRUCTION;
        status = STATUS_ILLEGAL_INSTRUCTION;
        break;
    case SWITCHYARD_FEX_STOP_SINGLE_STEP:
        params->stop_reason = XTAJIT64_STOP_SINGLE_STEP;
        status = STATUS_SINGLE_STEP;
        break;
    case SWITCHYARD_FEX_STOP_GUEST_FAULT:
    case SWITCHYARD_FEX_STOP_HLT:
        params->stop_reason = XTAJIT64_STOP_GUEST_EXCEPTION;
        params->fault_address = params->context.rip;
        params->fault_access = stop.error_code;
        params->reserved = stop.signal | ((uint32_t)stop.trap_number << 8) |
                           ((uint32_t)stop.signal_code << 16);
        status = STATUS_UNHANDLED_EXCEPTION;
        break;
    default:
        params->stop_reason = XTAJIT64_STOP_INTERNAL_ERROR;
        status = STATUS_UNSUCCESSFUL;
        poison_provider_locked( status );
        break;
    }

    record_flight_event( binding, XTAJIT64_FLIGHT_EVENT_PROVIDER_STOP,
                         XTAJIT64_FLIGHT_REASON_NONE, params->stop_reason,
                         ((uint64_t)stop.signal << 32) |
                         ((uint64_t)stop.trap_number << 24) |
                         ((uint64_t)stop.signal_code << 16) | stop.error_code,
                         params->context.rip );
    binding->doorbell = NULL;
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static NTSTATUS begin_simulation( void *args )
{
    return run_simulation( args, NULL, FALSE );
}

static NTSTATUS prepare_dispatch( void *args )
{
    struct xtajit64_dispatch_params *params = args;
    if (!params) return STATUS_INVALID_PARAMETER;
    return run_simulation( &params->simulation, params, FALSE );
}

static NTSTATUS complete_dispatch( void *args )
{
    struct xtajit64_dispatch_params *params = args;
    if (!params) return STATUS_INVALID_PARAMETER;
    return run_simulation( &params->simulation, params, TRUE );
}

static NTSTATUS run_direct_dispatch( void *args, BOOL complete )
{
    struct xtajit64_direct_params *params = args;
    struct thread_binding *binding;

    /* Thread attachment pins the immutable IDs until this owner detaches.
     * Never dereference a PE-supplied binding or adapter pointer. */
    if (!params) return STATUS_INVALID_PARAMETER;
    pthread_once( &binding_key_once, make_binding_key );
    if (binding_key_error || !(binding = pthread_getspecific( binding_key )))
        return STATUS_INVALID_HANDLE;
    if (!params->binding_id || params->binding_id != binding->id ||
        !params->process_instance || params->process_instance != binding->process_instance)
        return STATUS_INVALID_DEVICE_STATE;
    /* The attached binding pins process configuration. In ordinary mode the
     * entire execution returns before any native guest call; moving its host
     * activation to the control stack does not leave it paused across reentry. */
    if (!complete && !provider.custom_dispatch)
    {
        params->dispatch.entry = params->dispatch.frame = params->dispatch.generation = 0;
        params->dispatch.binding_id = params->dispatch.process_instance = 0;
        return run_simulation( &params->dispatch.simulation, NULL, FALSE );
    }
    return run_simulation( &params->dispatch.simulation, &params->dispatch, complete );
}

static NTSTATUS prepare_direct_dispatch( void *args )
{
    return run_direct_dispatch( args, FALSE );
}

static NTSTATUS complete_direct_dispatch( void *args )
{
    return run_direct_dispatch( args, TRUE );
}

static NTSTATUS bind_direct_dispatch( void *args )
{
    struct xtajit64_direct_capsule *params = args;
    struct thread_binding *binding;
    NTSTATUS status = STATUS_SUCCESS;

    if (!params || params->size != sizeof(*params) ||
        params->version != XTAJIT64_DIRECT_CAPSULE_VERSION || params->reserved ||
        params->bridge || params->prepare || params->complete ||
        params->authenticated_teb || params->binding_id || params->process_instance)
        return STATUS_INVALID_PARAMETER;
    pthread_once( &binding_key_once, make_binding_key );
    if (binding_key_error || !(binding = pthread_getspecific( binding_key )))
        return STATUS_INVALID_HANDLE;
    pthread_mutex_lock( &provider.mutex );
    if (!provider.initialized || provider.shutting_down ||
        binding->process_instance != provider.instance || !binding->thread)
        status = STATUS_INVALID_HANDLE;
    else if (provider.poison_status) status = provider.poison_status;
    else if (binding_is_active( binding )) status = STATUS_INVALID_DEVICE_STATE;
    else
    {
#if defined(__APPLE__) && defined(__aarch64__)
        if (!(params->bridge = __wine_get_arm64ec_native_call_v1()))
            status = STATUS_NOT_SUPPORTED;
        else
        {
            params->prepare = (uintptr_t)prepare_direct_dispatch;
            params->complete = (uintptr_t)complete_direct_dispatch;
            params->authenticated_teb = (uintptr_t)NtCurrentTeb();
            params->binding_id = binding->id;
            params->process_instance = binding->process_instance;
        }
#else
        status = STATUS_NOT_SUPPORTED;
#endif
    }
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

static NTSTATUS reconstruct_jit_fault( void *args )
{
    struct xtajit64_reconstruct_params *params = args;
    struct switchyard_fex_arm64_host_context host;
    struct switchyard_fex_x64_state state;
    struct switchyard_fex_fault fault;
    struct thread_binding *binding;
    enum switchyard_fex_result result;
    uint32_t signal;
    BOOL memory_fault;
    NTSTATUS status;

    pthread_once( &binding_key_once, make_binding_key );
    if (binding_key_error || !(binding = pthread_getspecific( binding_key )))
        return STATUS_INVALID_HANDLE;
    if (!params || params->host_context.size != sizeof(params->host_context) ||
        params->host_context.version != XTAJIT64_ARM64_HOST_CONTEXT_VERSION ||
        params->host_context.flags || params->host_context.reserved ||
        !params->exception_code)
        return STATUS_INVALID_PARAMETER;
    memory_fault = params->exception_code == STATUS_ACCESS_VIOLATION;
    if ((memory_fault &&
         params->fault_access != EXCEPTION_READ_FAULT &&
         params->fault_access != EXCEPTION_WRITE_FAULT &&
         params->fault_access != EXCEPTION_EXECUTE_FAULT) ||
        (!memory_fault && (params->host_fault_address || params->fault_access)))
        return STATUS_INVALID_PARAMETER;

    switch (params->exception_code)
    {
    case STATUS_ACCESS_VIOLATION:
        signal = SIGSEGV;
        break;
    case STATUS_DATATYPE_MISALIGNMENT:
        signal = SIGBUS;
        break;
    case STATUS_ILLEGAL_INSTRUCTION:
        signal = SIGILL;
        break;
    case STATUS_BREAKPOINT:
    case STATUS_SINGLE_STEP:
        signal = SIGTRAP;
        break;
    default:
        signal = 0;
        break;
    }

    memset( &host, 0, sizeof(host) );
    host.size = sizeof(host);
    host.version = SWITCHYARD_FEX_ARM64_HOST_CONTEXT_VERSION;
    memcpy( host.gpr, params->host_context.gpr, sizeof(host.gpr) );
    memcpy( host.vector, params->host_context.vector, sizeof(host.vector) );
    host.pc = params->host_context.pc;
    host.pstate = params->host_context.pstate;
    host.fpcr = params->host_context.fpcr;
    host.fpsr = params->host_context.fpsr;
    init_fex_state_output( &state );
    memset( &fault, 0, sizeof(fault) );
    fault.size = sizeof(fault);
    fault.version = SWITCHYARD_FEX_FAULT_VERSION;

    pthread_mutex_lock( &provider.mutex );
    if (!provider.initialized || provider.shutting_down ||
        binding->process_instance != provider.instance ||
        !binding->thread || !binding_is_active( binding ))
    {
        pthread_mutex_unlock( &provider.mutex );
        return STATUS_NOT_SUPPORTED;
    }
    pthread_mutex_unlock( &provider.mutex );

    result = switchyard_fex_thread_reconstruct_jit_fault(
        binding->thread, &host, signal, params->fault_access,
        params->host_fault_address, &state, &fault );
    params->provider_error = result;
    status = fex_result_to_status( result );
    if (result == SWITCHYARD_FEX_ERROR_UNSUPPORTED ||
        result == SWITCHYARD_FEX_ERROR_BUSY)
        return status;

    __atomic_store_n( &active_signal_binding, NULL, __ATOMIC_RELEASE );

    pthread_mutex_lock( &provider.mutex );
    binding->dispatch.generation = 0;
    if (!provider.mutating) binding->doorbell = NULL;
    if (!status)
    {
        export_fex_state( &params->context, &state );
        params->guest_fault_address = fault.guest_address;
        params->stop_reason = memory_fault ? XTAJIT64_STOP_MEMORY_FAULT :
                                             XTAJIT64_STOP_GUEST_EXCEPTION;
        binding->flight_guest_rip = params->context.rip;
        binding->flight_guest_rsp = params->context.rsp;
        record_flight_event( binding, XTAJIT64_FLIGHT_EVENT_PROVIDER_STOP,
                             XTAJIT64_FLIGHT_REASON_NONE,
                             params->stop_reason,
                             memory_fault ? fault.host_address :
                                            params->exception_code,
                             memory_fault ? fault.guest_address : fault.host_pc );
    }
    else poison_provider_locked( status );
    pthread_mutex_unlock( &provider.mutex );
    return status;
}

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
    flight_bind,
    reconstruct_jit_fault,
    prepare_dispatch,
    complete_dispatch,
    bind_direct_dispatch,
    control_stack_protect,
};

C_ASSERT( ARRAY_SIZE(__wine_unix_call_funcs) == unix_funcs_count );
