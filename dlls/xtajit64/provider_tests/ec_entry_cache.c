/* Native contract harness for the actual PE entry-cache implementation. */
#if 0
#pragma makedep standalone
#endif

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int BOOL, NTSTATUS;
typedef uint32_t UINT32;
typedef int32_t INT32;
typedef uint64_t UINT64;
typedef int64_t INT64;
typedef uintptr_t ULONG_PTR;
typedef struct
{
    void *AllocationBase;
    UINT32 State, Protect;
} MEMORY_BASIC_INFORMATION;

#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER 1
#define STATUS_INVALID_ADDRESS 2
#define STATUS_INVALID_IMAGE_FORMAT 3
#define STATUS_ACCESS_VIOLATION 4
#define XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ 1
#define XTAJIT64_EC_ENTRY_CACHE_SIZE 32
#define MEM_COMMIT 0x1000
#define MemoryBasicInformation 0
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define GetCurrentProcess() ((void *)-1)

struct xtajit64_ec_entry_cache
{
    UINT64 generation, guest_target, native_target, entry;
};

struct xtajit64_thread_state
{
    UINT32 ec_entry_cache_lru;
    struct xtajit64_ec_entry_cache ec_entry_cache[XTAJIT64_EC_ENTRY_CACHE_SIZE];
};

static unsigned char image[65536];
static UINT64 generation = 1, query_count;
static BOOL executable = TRUE, ec_code = TRUE, readable = TRUE, revoke_during_read;
static ULONG_PTR targets[3];

static UINT64 current_transition_cache_generation(void)
{
    return __atomic_load_n( &generation, __ATOMIC_ACQUIRE );
}

static BOOL within_image( ULONG_PTR address, size_t size )
{
    return address >= (ULONG_PTR)image && size <= sizeof(image) &&
           address - (ULONG_PTR)image <= sizeof(image) - size;
}

static BOOL guest_range_to_host( UINT64 guest, size_t size, UINT32 flags, ULONG_PTR *host )
{
    assert( flags == XTAJIT64_MEMORY_TRANSLATE_REQUIRE_READ );
    if (!readable || !within_image( guest, size )) return FALSE;
    *host = guest;
    return TRUE;
}

static BOOL RtlIsEcCode( ULONG_PTR address )
{
    return ec_code && within_image( address, sizeof(UINT32) );
}

static NTSTATUS read_current_process_memory( void *out, const void *in, size_t size )
{
    if (!readable || !within_image( (ULONG_PTR)in, size )) return STATUS_ACCESS_VIOLATION;
    memcpy( out, in, size );
    if (revoke_during_read)
    {
        revoke_during_read = FALSE;
        executable = FALSE;
        __atomic_add_fetch( &generation, 1, __ATOMIC_RELEASE );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NtQueryVirtualMemory( void *process, const void *address, int type,
                                      MEMORY_BASIC_INFORMATION *info, size_t size, void *returned )
{
    assert( process == GetCurrentProcess() && type == MemoryBasicInformation );
    assert( size == sizeof(*info) && !returned );
    __atomic_add_fetch( &query_count, 1, __ATOMIC_RELAXED );
    if (!within_image( (ULONG_PTR)address, 1 )) return STATUS_INVALID_ADDRESS;
    info->AllocationBase = image;
    info->State = MEM_COMMIT;
    info->Protect = executable ? PAGE_EXECUTE_READ : 2;
    return STATUS_SUCCESS;
}

#include "ec_entry_cache_impl.h"

static void resolve_ok( struct xtajit64_thread_state *state, ULONG_PTR target, ULONG_PTR expected_entry )
{
    ULONG_PTR native = 0, entry = 0;

    assert( !resolve_ec_entry_thunk( state, target, &native, &entry ) );
    assert( native == target && entry == expected_entry );
}

static void reset_state( struct xtajit64_thread_state *state )
{
    memset( state, 0, sizeof(*state) );
    executable = ec_code = readable = TRUE;
    revoke_during_read = FALSE;
    query_count = 0;
    ++generation;
}

static void *concurrent_resolver( void *unused )
{
    struct xtajit64_thread_state state = {0};
    unsigned int i;

    (void)unused;
    for (i = 0; i < 20000; ++i)
        resolve_ok( &state, targets[i & 1], targets[i & 1] + 4 );
    return NULL;
}

int main(void)
{
    struct xtajit64_thread_state state;
    ULONG_PTR native, entry, address;
    UINT32 encoded = 4, index = 0;
    unsigned int i, count = 0;
    pthread_t threads[8];

    for (i = 64; i + 16 < sizeof(image) && count < 3; i += 32)
    {
        address = ((ULONG_PTR)image + i + 3) & ~(ULONG_PTR)3;
        if (!count) index = ec_entry_cache_index( address );
        if (ec_entry_cache_index( address ) != index) continue;
        targets[count++] = address;
        memcpy( (void *)(address - 4), &encoded, sizeof(encoded) );
    }
    assert( count == 3 );
    reset_state( &state );
    for (i = 0; i < 2000; ++i) resolve_ok( &state, targets[i & 1], targets[i & 1] + 4 );
    assert( query_count == 4 ); /* The direct-mapped resolver needs 4000 queries. */

    /* Touch B, insert C, then B must survive replacement of the older A. */
    resolve_ok( &state, targets[1], targets[1] + 4 );
    resolve_ok( &state, targets[2], targets[2] + 4 );
    resolve_ok( &state, targets[1], targets[1] + 4 );
    assert( query_count == 6 );
    resolve_ok( &state, targets[0], targets[0] + 4 );
    assert( query_count == 8 );

    /* Invalidated metadata is never reused, including both colliding ways. */
    ++generation;
    resolve_ok( &state, targets[0], targets[0] + 4 );
    resolve_ok( &state, targets[1], targets[1] + 4 );
    assert( query_count == 12 );
    executable = FALSE;
    ++generation;
    assert( resolve_ec_entry_thunk( &state, targets[0], &native, &entry ) == STATUS_INVALID_IMAGE_FORMAT );
    assert( resolve_ec_entry_thunk( &state, targets[1], &native, &entry ) == STATUS_INVALID_IMAGE_FORMAT );

    reset_state( &state );
    resolve_ok( &state, targets[0], targets[0] + 4 );
    readable = FALSE;
    assert( resolve_ec_entry_thunk( &state, targets[0], &native, &entry ) == STATUS_INVALID_ADDRESS );
    readable = TRUE;
    ec_code = FALSE;
    assert( resolve_ec_entry_thunk( &state, targets[0], &native, &entry ) == STATUS_INVALID_ADDRESS );
    ec_code = TRUE;
    encoded = 8;
    memcpy( (void *)(targets[0] - 4), &encoded, sizeof(encoded) );
    resolve_ok( &state, targets[0], targets[0] + 8 );
    encoded = 0;
    memcpy( (void *)(targets[0] - 4), &encoded, sizeof(encoded) );
    assert( resolve_ec_entry_thunk( &state, targets[0], &native, &entry ) == STATUS_INVALID_IMAGE_FORMAT );
    encoded = 4;
    memcpy( (void *)(targets[0] - 4), &encoded, sizeof(encoded) );

    reset_state( &state );
    resolve_ok( &state, targets[0], targets[0] + 4 );
    revoke_during_read = TRUE;
    assert( resolve_ec_entry_thunk( &state, targets[0], &native, &entry ) == STATUS_INVALID_IMAGE_FORMAT );
    assert( resolve_ec_entry_thunk( &state, targets[0], NULL, &entry ) == STATUS_INVALID_PARAMETER );
    assert( resolve_ec_entry_thunk( NULL, targets[0], &native, &entry ) == STATUS_INVALID_ADDRESS );
    assert( !decode_ec_entry_thunk( 3, (UINT32)-4, &entry ) );
    assert( !decode_ec_entry_thunk( ~(ULONG_PTR)0 - 3, 4, &entry ) );
    assert( !decode_ec_entry_thunk( targets[0], 0, &entry ) );
    assert( !decode_ec_entry_thunk( targets[0], 4, NULL ) );

    reset_state( &state );
    for (i = 0; i < 8; ++i) assert( !pthread_create( &threads[i], NULL, concurrent_resolver, NULL ) );
    for (i = 0; i < 8; ++i) assert( !pthread_join( threads[i], NULL ) );
    assert( query_count == 32 );
    puts( "EC entry cache: collision, LRU, invalidation, access, metadata, bounds and eight owners passed" );
    return 0;
}
