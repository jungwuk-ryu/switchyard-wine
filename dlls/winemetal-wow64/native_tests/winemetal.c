/*
 * Winemetal high-shadow Wow64 companion native tests
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "winemetal_private.h"

#define ARENA_SIZE (2u * 1024u * 1024u)
#define OUTER_TOKEN 0x800u
#define OWNED_OFFSET 0x400000u
#define BUFFER_OBJECT 0x1818181818181818ull
#define BUFFER_GPU 0x1818000018180000ull
#define SAMPLER_OBJECT 0x1919191919191919ull
#define SAMPLER_GPU 0x1919000019190000ull
#define STRING_OBJECT 0x6060606060606060ull
#define FORWARD_OBJECT 0x0202020202020202ull

extern const struct wine_wow64_unixlib_companion_v4
    __wine_unix_call_wow64_companion_v4;
extern NTSTATUS wmt_test_release_backend_snapshot(void);

static const UINT32 expected_sizes[WMT_UNIX_CALL_COUNT] =
{
    8, 8, 24, 16, 8, 16, 16, 16, 40, 24, 8, 16,
    8, 8, 16, 16, 16, 24, 24, 24, 24, 24, 40, 48,
    24, 32, 24, 24, 16, 32, 16, 24, 24, 8, 32, 32,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 96, 24, 16,
    24, 24, 16, 24, 16, 8, 24, 8, 24, 24, 72, 40,
    24, 24, 8, 32, 24, 16, 16, 16, 16, 16, 16, 16,
    32, 8, 24, 8, 32, 16, 8, 24, 8, 40, 40, 0,
    40, 40, 16, 16, 16, 16, 16, 40, 16, 24, 8, 8,
    16, 16, 48, 32, 24, 40, 24, 16, 32, 16, 16, 32,
    8, 8, 8, 24, 32, 24, 24, 24, 32, 24, 32, 16,
    24, 136, 136, 16, 24, 16, 32, 24, 32, 32, 24, 32,
    32, 24, 24, 8, 8, 16
};

static const UINT32 expected_flags[WMT_UNIX_CALL_COUNT] =
{
    1, 1, 5, 5, 5, 5, 5, 5, 7, 5, 1, 5,
    1, 1, 5, 5, 5, 1, 31, 7, 7, 7, 7, 5,
    5, 5, 7, 5, 5, 7, 5, 5, 7, 1, 7, 7,
    3, 3, 3, 5, 5, 5, 5, 5, 5, 3, 1, 1,
    1, 5, 5, 5, 5, 5, 7, 1, 7, 7, 3, 1,
    7, 7, 5, 5, 1, 1, 5, 5, 5, 5, 3, 7,
    5, 1, 7, 1, 7, 7, 1, 7, 1, 7, 7, 1,
    7, 7, 1, 1, 3, 5, 5, 7, 5, 5, 5, 5,
    7, 7, 7, 7, 3, 7, 1, 1, 9, 5, 5, 3,
    5, 1, 1, 5, 7, 7, 7, 7, 7, 7, 3, 7,
    7, 1, 5, 5, 5, 5, 5, 5, 7, 7, 5, 7,
    5, 3, 3, 1, 1, 1
};

static const unsigned int denied[] =
{
    8, 22, 35, 37, 45, 54, 56, 57, 58, 74, 76, 77, 79, 81, 82, 83, 84,
    85, 88, 91, 96, 99, 100, 107, 112, 113, 116, 118, 120, 128, 129, 131,
    133, 134,
};

static unsigned char *arena;
static UINT32 bias, next_offset = 0x1000, owned_guest;
static void *outer_address, *owned_host;
static SIZE_T outer_size, owned_mapped;
static struct ntdll_wow64_unixlib_call_context call_context;
static BOOL context_active, fail_outer_probe, fail_outer_write, fail_owned_guest;
static const void *fail_nested_read;
static UINT64 owned_lease, pending_lease;
static BOOL owned_active, fail_metal;
static unsigned int failures, normal_calls, legacy_calls;
static unsigned int acquire_calls, release_calls, metal_calls, release_object_calls;
static unixlib_entry_t normal[WMT_UNIX_CALL_COUNT], legacy[WMT_UNIX_CALL_COUNT];

static void check( BOOL condition, const char *message )
{
    if (condition) return;
    fprintf( stderr, "%s:%u: %s\n", __FILE__, __LINE__, message );
    ++failures;
}

static BOOL in_range( const void *base, SIZE_T length, const void *ptr, SIZE_T size )
{
    UINT_PTR b = (UINT_PTR)base, p = (UINT_PTR)ptr;

    if (!size) return TRUE;
    if (!base || p < b || p - b > length) return FALSE;
    return size <= length - (p - b);
}

static UINT32 guest_alloc( SIZE_T size, SIZE_T align )
{
    UINT32 offset = (next_offset + align - 1) & ~(UINT32)(align - 1);

    if (!align || (align & (align - 1)) || offset > ARENA_SIZE ||
        size > ARENA_SIZE - offset)
        abort();
    next_offset = offset + size;
    memset( arena + offset, 0, size );
    return bias + offset;
}

static void *decode_guest( UINT32 address )
{
    UINT32 offset;

    if (address < bias) return NULL;
    offset = address - bias;
    if (offset < 0x1000 || offset >= ARENA_SIZE) return NULL;
    return arena + offset;
}

NTSTATUS ntdll_wow64_guest32_to_host( ULONG address, void **host )
{
    UINT64 offset;

    *host = NULL;
    if (context_active && address == OUTER_TOKEN)
    {
        *host = outer_address;
        return outer_address || !outer_size ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
    }
    if (owned_active && address >= owned_guest)
    {
        offset = address - owned_guest;
        if (offset < owned_mapped)
        {
            *host = (char *)owned_host + offset;
            return STATUS_SUCCESS;
        }
    }
    if (!(*host = decode_guest( address ))) return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

NTSTATUS ntdll_wow64_copy_from_user( void *dst, const void *src, SIZE_T size )
{
    if (src == fail_nested_read || !dst || !in_range( arena, ARENA_SIZE, src, size ))
        return STATUS_ACCESS_VIOLATION;
    memcpy( dst, src, size );
    return STATUS_SUCCESS;
}

NTSTATUS ntdll_wow64_copy_to_user( void *dst, const void *src, SIZE_T size )
{
    if (!src || !in_range( arena, ARENA_SIZE, dst, size ))
        return STATUS_ACCESS_VIOLATION;
    memcpy( dst, src, size );
    return STATUS_SUCCESS;
}

NTSTATUS ntdll_wow64_faulting_copy_to_user( void *dst, const void *src, SIZE_T size )
{
    return ntdll_wow64_copy_to_user( dst, src, size );
}

NTSTATUS ntdll_wow64_probe_user_read( const void *src, SIZE_T size )
{
    if (in_range( arena, ARENA_SIZE, src, size ) ||
        (owned_active && in_range( owned_host, owned_mapped, src, size )))
        return STATUS_SUCCESS;
    return STATUS_ACCESS_VIOLATION;
}

NTSTATUS ntdll_wow64_probe_user_write( void *dst, SIZE_T size )
{
    if (dst == outer_address && size == outer_size)
        return fail_outer_probe ? STATUS_ACCESS_VIOLATION : STATUS_SUCCESS;
    return ntdll_wow64_probe_user_read( dst, size );
}

NTSTATUS ntdll_wow64_atomic_write_user( void *dst, const void *src, SIZE_T size )
{
    if (dst == outer_address && size == outer_size)
    {
        if (fail_outer_write) return STATUS_ACCESS_VIOLATION;
        memcpy( dst, src, size );
        return STATUS_SUCCESS;
    }
    return ntdll_wow64_copy_to_user( dst, src, size );
}

NTSTATUS ntdll_wow64_get_unixlib_call_context(
    struct ntdll_wow64_unixlib_call_context *context )
{
    if (!context_active || !context) return STATUS_INVALID_DEVICE_STATE;
    *context = call_context;
    return STATUS_SUCCESS;
}

static NTSTATUS codec_translate( UINT64 guest, UINT64 size, UINT32 access, void **host )
{
    NTSTATUS status;

    if (guest > UINT32_MAX || access &
        ~(WINE_WOW64_UNIXLIB_ACCESS_READ | WINE_WOW64_UNIXLIB_ACCESS_WRITE))
        return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_guest32_to_host( guest, host ))) return status;
    if ((access & WINE_WOW64_UNIXLIB_ACCESS_READ) &&
        (status = ntdll_wow64_probe_user_read( *host, size )))
        return status;
    if (access & WINE_WOW64_UNIXLIB_ACCESS_WRITE)
        return ntdll_wow64_probe_user_write( *host, size );
    return STATUS_SUCCESS;
}

static NTSTATUS codec_read( UINT64 guest, void *dst, UINT64 size )
{
    void *host;
    NTSTATUS status;

    if (guest > UINT32_MAX || size > SIZE_MAX ||
        (status = ntdll_wow64_guest32_to_host( guest, &host )))
        return STATUS_ACCESS_VIOLATION;
    return ntdll_wow64_copy_from_user( dst, host, size );
}

static NTSTATUS codec_write( UINT64 guest, const void *src, UINT64 size )
{
    void *host;
    NTSTATUS status;

    if (guest > UINT32_MAX || size > SIZE_MAX ||
        (status = ntdll_wow64_guest32_to_host( guest, &host )))
        return STATUS_ACCESS_VIOLATION;
    return ntdll_wow64_atomic_write_user( host, src, size );
}

static NTSTATUS acquire_backing( UINT64 length, UINT32 access,
                                 struct wine_unixlib_owned_backing_v2 *backing )
{
    SIZE_T page = getpagesize();
    UINT64 mapped;

    ++acquire_calls;
    memset( backing, 0, sizeof(*backing) );
    if (!length || length > UINT64_MAX - (page - 1) ||
        access != (WINE_WOW64_UNIXLIB_ACCESS_READ | WINE_WOW64_UNIXLIB_ACCESS_WRITE))
        return STATUS_INVALID_PARAMETER;
    mapped = (length + page - 1) & ~(UINT64)(page - 1);
    if (mapped > page) return STATUS_NO_MEMORY;
    if (!owned_host && posix_memalign( &owned_host, page, page )) return STATUS_NO_MEMORY;
    memset( owned_host, 0, page );
    owned_mapped = mapped;
    owned_lease = 0xabc00000ull + acquire_calls;
    owned_active = TRUE;
    backing->version = WINE_UNIXLIB_OWNED_BACKING_V2_VERSION;
    backing->size = sizeof(*backing);
    backing->address = (UINT64)(UINT_PTR)owned_host;
    backing->length = length;
    backing->mapped_length = mapped;
    backing->lease = owned_lease;
    backing->generation = acquire_calls;
    backing->guest_address = fail_owned_guest ? (UINT64)UINT32_MAX + 1 : owned_guest;
    return STATUS_SUCCESS;
}

static NTSTATUS release_backing( UINT64 lease )
{
    if (!owned_active || lease != owned_lease) return STATUS_INVALID_PARAMETER;
    owned_active = FALSE;
    owned_lease = 0;
    ++release_calls;
    return STATUS_SUCCESS;
}

wmt_status_t wmt_metal_buffer_from_alias( wmt_uint64_t device, void *address,
                                          wmt_uint64_t logical_length,
                                          wmt_uint64_t mapped_length,
                                          wmt_uint64_t options, wmt_uint64_t lease,
                                          wmt_alias_release_func release,
                                          wmt_uint64_t *ret, wmt_uint64_t *gpu )
{
    (void)options;
    ++metal_calls;
    *ret = *gpu = 0;
    if (!device || address != owned_host || !logical_length ||
        mapped_length != owned_mapped || !lease || !release)
    {
        if (lease && release) release( lease );
        return STATUS_INVALID_PARAMETER;
    }
    if (fail_metal)
    {
        release( lease );
        return STATUS_NO_MEMORY;
    }
    pending_lease = lease;
    *ret = BUFFER_OBJECT;
    *gpu = BUFFER_GPU;
    return STATUS_SUCCESS;
}

static NTSTATUS normal_generic( void *args )
{
    (void)args;
    ++normal_calls;
    return STATUS_SUCCESS;
}

static NTSTATUS legacy_generic( void *args )
{
    (void)args;
    ++legacy_calls;
    return STATUS_SUCCESS;
}

static NTSTATUS forward_object( void *args )
{
    struct wmt_params32_obj_u64_obj_ret *params = args;

    ++legacy_calls;
    params->ret = FORWARD_OBJECT;
    return STATUS_SUCCESS;
}

static NTSTATUS release_object( void *args )
{
    UINT64 object;

    ++normal_calls;
    ++release_object_calls;
    memcpy( &object, args, sizeof(object) );
    if (object == BUFFER_OBJECT && pending_lease)
    {
        UINT64 lease = pending_lease;

        pending_lease = 0;
        return release_backing( lease );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS new_sampler( void *args )
{
    struct wmt_params_info_ret *params = args;
    struct wmt_sampler_info *info = params->info.ptr;

    ++normal_calls;
    info->gpu_resource_id = SAMPLER_GPU;
    params->ret = SAMPLER_OBJECT;
    return STATUS_SUCCESS;
}

static NTSTATUS make_string( void *args )
{
    struct wmt_params_string *params = args;

    ++normal_calls;
    if (!params->buffer.ptr || strcmp( params->buffer.ptr, "Switchyard" ))
        return STATUS_INVALID_PARAMETER;
    params->ret = STRING_OBJECT;
    return STATUS_SUCCESS;
}

static const struct wine_wow64_unixlib_codec_v2 codec =
{
    WINE_WOW64_UNIXLIB_CODEC_V2_VERSION, sizeof(codec),
    WINE_WOW64_UNIXLIB_CAP_SEPARATE_GUEST_ADDRESS_SPACE,
    codec_translate, codec_read, codec_write, NULL, NULL,
};

static const struct wine_unixlib_owned_backing_codec_v2 backing_codec =
{
    WINE_UNIXLIB_OWNED_BACKING_CODEC_V2_VERSION, sizeof(backing_codec),
    WINE_UNIXLIB_OWNED_BACKING_CAP_ACQUIRE_RELEASE,
    acquire_backing, release_backing,
};

static NTSTATUS call_slot( unsigned int index, void *outer )
{
    const struct wine_unixlib_dispatch_entry_v2 *entry;
    unsigned char snapshot[WINE_UNIXLIB_DISPATCH_MAX_ARGS_SIZE];
    NTSTATUS status;

    entry = &__wine_unix_call_wow64_dispatch_v2.entries[index];
    outer_address = outer;
    outer_size = entry->args_size;
    call_context.guest_args = (const void *)(UINT_PTR)OUTER_TOKEN;
    call_context.args_size = entry->args_size;
    call_context.flags = entry->flags;
    context_active = TRUE;
    if (entry->args_size) memcpy( snapshot, outer, entry->args_size );
    status = __wine_unix_call_wow64_funcs[index]( entry->args_size ? snapshot : NULL );
    context_active = FALSE;
    outer_address = NULL;
    outer_size = 0;
    return status;
}

static int hex_digit( char value )
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static void test_descriptor( const char *sha )
{
    static const BYTE header_hash[32] = WINE_WOW64_UNIXLIB_COMPANION_V4_ABI_SHA256;
    const struct wine_unixlib_dispatch_source_v2 *source =
        &__wine_unix_call_wow64_dispatch_v2;
    BYTE parsed[32];
    unsigned int i;

    for (i = 0; i < 32; ++i)
        parsed[i] = hex_digit( sha[2 * i] ) * 16 + hex_digit( sha[2 * i + 1] );
    check( __wine_unix_call_wow64_companion_v4.version == 4 &&
        __wine_unix_call_wow64_companion_v4.size ==
            sizeof(__wine_unix_call_wow64_companion_v4) &&
        __wine_unix_call_wow64_companion_v4.entry_count == WMT_UNIX_CALL_COUNT &&
        !__wine_unix_call_wow64_companion_v4.flags,
        "v4 companion descriptor mismatch" );
    check( !memcmp( parsed, header_hash, 32 ) &&
        !memcmp( parsed, __wine_unix_call_wow64_companion_v4.abi_sha256, 32 ),
        "schema/header/descriptor hash mismatch" );
    check( source->version == 2 && source->size == sizeof(*source) &&
        source->entry_count == WMT_UNIX_CALL_COUNT &&
        source->entry_size == sizeof(*source->entries) &&
        source->funcs == __wine_unix_call_wow64_funcs &&
        !source->flags && !source->reserved, "v2 source descriptor mismatch" );
    for (i = 0; i < WMT_UNIX_CALL_COUNT; ++i)
        if (source->entries[i].args_size != expected_sizes[i] ||
            source->entries[i].flags != expected_flags[i] || !source->funcs[i])
        {
            fprintf( stderr, "slot %u metadata mismatch\n", i );
            ++failures;
        }
}

static void test_binding(void)
{
    struct wine_wow64_unixlib_binding_v4 binding;
    struct wine_wow64_unixlib_codec_v2 invalid = codec;
    unixlib_entry_t normal_copy[WMT_UNIX_CALL_COUNT];
    unixlib_entry_t legacy_copy[WMT_UNIX_CALL_COUNT];
    NTSTATUS status;
    unsigned int i;

    for (i = 0; i < WMT_UNIX_CALL_COUNT; ++i)
    {
        normal[i] = normal_generic;
        legacy[i] = legacy_generic;
    }
    normal[1] = release_object;
    normal[19] = new_sampler;
    normal[60] = make_string;
    legacy[2] = forward_object;
    normal[83] = NULL;
    legacy[83] = NULL;
    memset( &binding, 0, sizeof(binding) );
    binding.version = 4;
    binding.size = sizeof(binding);
    binding.entry_count = WMT_UNIX_CALL_COUNT;
    binding.normal_funcs = normal;
    binding.legacy_wow64_funcs = legacy;
    binding.owned_backing_codec = &backing_codec;
    invalid.capabilities |= WINE_WOW64_UNIXLIB_CAP_OWNED_MEMORY_ALIAS;
    binding.codec = &invalid;
    status = __wine_unix_call_wow64_companion_v4.bind( &binding );
    check( status == STATUS_INVALID_PARAMETER, "extra alias capability was accepted" );
    binding.codec = &codec;
    normal[82] = NULL;
    status = __wine_unix_call_wow64_companion_v4.bind( &binding );
    check( status == STATUS_INVALID_IMAGE_FORMAT, "malformed source table was accepted" );
    normal[82] = normal_generic;
    status = __wine_unix_call_wow64_companion_v4.bind( &binding );
    check( !status, "valid v4 binding failed" );
    memcpy( normal_copy, normal, sizeof(normal_copy) );
    memcpy( legacy_copy, legacy, sizeof(legacy_copy) );
    binding.normal_funcs = normal_copy;
    binding.legacy_wow64_funcs = legacy_copy;
    status = __wine_unix_call_wow64_companion_v4.bind( &binding );
    check( !status, "content-identical v4 rebind failed" );
    binding.normal_funcs = normal;
    binding.legacy_wow64_funcs = legacy;
    normal[19] = normal_generic;
    legacy[2] = legacy_generic;
    status = __wine_unix_call_wow64_companion_v4.bind( &binding );
    check( status == STATUS_INVALID_DEVICE_STATE, "content-different rebind was accepted" );
    normal[83] = normal_generic;
    legacy[83] = legacy_generic;
    check( wmt_normal_call( 83, NULL ) == STATUS_NOT_SUPPORTED &&
        wmt_forward_call( 83, NULL ) == STATUS_NOT_SUPPORTED,
        "source-table mutation changed snapshotted null entries" );
}

static void test_snapshot_cleanup(void)
{
    struct wine_wow64_unixlib_binding_v4 binding;
    struct wmt_params32_obj_u64_obj_ret params = {2, 3, 0};
    NTSTATUS status;
    unsigned int i;

    status = wmt_test_release_backend_snapshot();
    check( !status, "backend snapshot cleanup failed" );
    check( wmt_forward_call( 2, &params ) == STATUS_INVALID_DEVICE_STATE,
        "cleaned backend remained callable" );

    for (i = 0; i < WMT_UNIX_CALL_COUNT; ++i)
    {
        normal[i] = normal_generic;
        legacy[i] = legacy_generic;
    }
    normal[1] = release_object;
    normal[19] = new_sampler;
    normal[60] = make_string;
    legacy[2] = forward_object;
    normal[83] = NULL;
    legacy[83] = NULL;
    memset( &binding, 0, sizeof(binding) );
    binding.version = 4;
    binding.size = sizeof(binding);
    binding.entry_count = WMT_UNIX_CALL_COUNT;
    binding.normal_funcs = normal;
    binding.legacy_wow64_funcs = legacy;
    binding.codec = &codec;
    binding.owned_backing_codec = &backing_codec;
    status = __wine_unix_call_wow64_companion_v4.bind( &binding );
    check( !status, "backend snapshot rebind after cleanup failed" );
    legacy[2] = legacy_generic;
    status = wmt_forward_call( 2, &params );
    check( !status && params.ret == FORWARD_OBJECT,
        "rebound backend did not use its private snapshot" );
    status = wmt_test_release_backend_snapshot();
    check( !status, "rebound backend snapshot cleanup failed" );
}

static void test_forward_and_context(void)
{
    struct wmt_params32_obj_u64_obj_ret params = {2, 3, 0};
    unsigned char snapshot[24];
    unsigned int before;
    NTSTATUS status;

    status = call_slot( 2, &params );
    check( !status && params.ret == FORWARD_OBJECT, "forward output was not published" );
    before = legacy_calls;
    params.ret = ~0ull;
    fail_outer_probe = TRUE;
    status = call_slot( 2, &params );
    fail_outer_probe = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && legacy_calls == before &&
        params.ret == ~0ull, "outer pre-probe allowed a side effect" );

    memcpy( snapshot, &params, sizeof(snapshot) );
    outer_address = &params;
    outer_size = sizeof(snapshot);
    call_context.guest_args = (const void *)(UINT_PTR)OUTER_TOKEN;
    call_context.args_size = 16;
    call_context.flags = expected_flags[2];
    context_active = TRUE;
    status = __wine_unix_call_wow64_funcs[2]( snapshot );
    context_active = FALSE;
    outer_address = NULL;
    outer_size = 0;
    check( status == STATUS_INVALID_PARAMETER && legacy_calls == before,
        "wrong args_size was accepted" );
}

static void test_adapters(void)
{
    struct wmt_sampler_info32 *sampler;
    struct wmt_params32_info_ret info;
    struct wmt_params32_string string;
    struct wmt_params32_obj_u64_obj_ret blob;
    UINT32 sampler_guest, string_guest;
    unsigned int before;
    NTSTATUS status;

    sampler_guest = guest_alloc( sizeof(*sampler), 8 );
    sampler = decode_guest( sampler_guest );
    sampler->gpu_resource_id = ~0ull;
    info.object = 19;
    info.info = (struct wmt_wire_ptr){sampler_guest, 0};
    info.ret = ~0ull;
    status = call_slot( 19, &info );
    check( !status && info.ret == SAMPLER_OBJECT &&
        sampler->gpu_resource_id == SAMPLER_GPU, "slot19 adaptation failed" );

    sampler->gpu_resource_id = ~0ull;
    info.ret = ~0ull;
    fail_nested_read = sampler;
    status = call_slot( 19, &info );
    fail_nested_read = NULL;
    check( status == STATUS_ACCESS_VIOLATION && !info.ret &&
        !sampler->gpu_resource_id, "slot19 read failure left stale output" );

    string_guest = guest_alloc( sizeof("Switchyard"), 1 );
    memcpy( decode_guest( string_guest ), "Switchyard", sizeof("Switchyard") );
    string.buffer = (struct wmt_wire_ptr){string_guest, 0};
    string.encoding = 4;
    string.ret = ~0ull;
    status = call_slot( 60, &string );
    check( !status && string.ret == STRING_OBJECT, "slot60 adaptation failed" );
    string.buffer.high = 1;
    string.ret = ~0ull;
    before = normal_calls;
    status = call_slot( 60, &string );
    check( status == STATUS_INVALID_PARAMETER && !string.ret &&
        normal_calls == before, "slot60 accepted a high pointer" );

    blob.handle = string_guest;
    blob.arg = WMT_DISPATCH_BLOB_MAX + 1ull;
    blob.ret = ~0ull;
    before = normal_calls;
    status = call_slot( 114, &blob );
    check( status == STATUS_INVALID_PARAMETER && !blob.ret &&
        normal_calls == before, "slot114 bound was not enforced" );
}

static void reset_info( struct wmt_buffer_info32 *info )
{
    memset( info, 0, sizeof(*info) );
    info->length = 64;
    info->options = WMT_RESOURCE_STORAGE_MODE_SHARED;
    info->gpu_address = ~0ull;
}

static void test_buffer(void)
{
    struct wmt_buffer_info32 *info;
    struct wmt_params32_info_ret params;
    UINT32 info_guest, caller_guest;
    unsigned int acquire_before, release_before, metal_before;
    NTSTATUS status;

    info_guest = guest_alloc( sizeof(*info), 8 );
    caller_guest = guest_alloc( 64, 16 );
    info = decode_guest( info_guest );
    params.object = 18;
    params.info = (struct wmt_wire_ptr){info_guest, 0};

    reset_info( info );
    info->memory = (struct wmt_wire_ptr){caller_guest, 0};
    params.ret = ~0ull;
    acquire_before = acquire_calls;
    metal_before = metal_calls;
    status = call_slot( 18, &params );
    check( status == STATUS_NOT_SUPPORTED && !params.ret && !info->gpu_address &&
        info->memory.low == caller_guest && acquire_calls == acquire_before &&
        metal_calls == metal_before, "caller retained alias did not fail closed" );

    reset_info( info );
    params.ret = ~0ull;
    status = call_slot( 18, &params );
    check( !status && params.ret == BUFFER_OBJECT && info->memory.low == owned_guest &&
        !info->memory.high && info->gpu_address == BUFFER_GPU &&
        owned_active && pending_lease, "owned backing publication failed" );
    status = release_object( &params.ret );
    check( !status && !owned_active && !pending_lease,
        "buffer destruction did not release once" );

    reset_info( info );
    params.ret = 0xaaaaaaaaaaaaaaaaull;
    release_before = release_calls;
    fail_outer_write = TRUE;
    status = call_slot( 18, &params );
    fail_outer_write = FALSE;
    check( status == STATUS_ACCESS_VIOLATION &&
        params.ret == 0xaaaaaaaaaaaaaaaaull && !info->memory.low &&
        !info->memory.high && !info->gpu_address && !owned_active &&
        !pending_lease && release_calls == release_before + 1,
        "outer failure rollback was unsafe" );

    reset_info( info );
    params.ret = ~0ull;
    release_before = release_calls;
    fail_owned_guest = TRUE;
    status = call_slot( 18, &params );
    fail_owned_guest = FALSE;
    check( status == STATUS_INVALID_PARAMETER && !params.ret &&
        !info->memory.low && !info->gpu_address && !owned_active &&
        release_calls == release_before + 1, "malformed backing leaked" );

    reset_info( info );
    params.ret = ~0ull;
    release_before = release_calls;
    fail_metal = TRUE;
    status = call_slot( 18, &params );
    fail_metal = FALSE;
    check( status == STATUS_NO_MEMORY && !params.ret && !info->memory.low &&
        !info->gpu_address && release_calls == release_before + 1,
        "Metal failure did not consume one lease" );
}

static void test_denials(void)
{
    unsigned char outer[WINE_UNIXLIB_DISPATCH_MAX_ARGS_SIZE];
    struct wmt_params32_getcstring *cstring = (void *)outer;
    struct wmt_params32_pipeline *pipeline = (void *)outer;
    unsigned int i;
    UINT32 guest;
    NTSTATUS status;

    for (i = 0; i < ARRAY_SIZE(denied); ++i)
    {
        memset( outer, 0, sizeof(outer) );
        status = call_slot( denied[i], expected_sizes[denied[i]] ? outer : NULL );
        if (status != STATUS_NOT_SUPPORTED)
        {
            fprintf( stderr, "deny slot %u returned %#x\n", denied[i], status );
            ++failures;
        }
    }
    memset( outer, 0xa5, 40 );
    guest = guest_alloc( 4, 1 );
    memset( decode_guest( guest ), 0xcc, 4 );
    cstring->buffer = guest;
    cstring->max_length = 4;
    cstring->ret = ~0u;
    status = call_slot( 8, outer );
    check( status == STATUS_NOT_SUPPORTED && !cstring->ret &&
        !*(unsigned char *)decode_guest( guest ), "slot8 outputs were stale" );
    memset( outer, 0xff, 32 );
    status = call_slot( 35, outer );
    check( status == STATUS_NOT_SUPPORTED && !pipeline->ret_error &&
        !pipeline->ret_pso, "slot35 outputs were stale" );
}

static void test_deterministic_denial_outputs(void)
{
    unsigned char outer[136];
    struct wmt_params32_buffer_texture *buffer_texture = (void *)outer;
    struct wmt_params32_capture *capture = (void *)outer;
    struct wmt_params32_info_ret *info_ret = (void *)outer;
    struct wmt_air_initialize32 *initialize = (void *)outer;
    struct wmt_air_compile32 *compile = (void *)outer;
    struct wmt_air_get_bitcode32 *get_bitcode = (void *)outer;
    struct wmt_air_get_error32 *get_error = (void *)outer;
    struct wmt_params32_enumerate *enumerate = (void *)outer;
    struct wmt_params32_obj_ptr *obj_ptr = (void *)outer;
    struct wmt_params32_query_display *query = (void *)outer;
    struct wmt_params32_archive *archive = (void *)outer;
    struct wmt_params32_archive_serialize *serialize = (void *)outer;
    struct wmt_params32_cache_get *cache = (void *)outer;
    struct wmt_params32_resolve_counter_range *range = (void *)outer;
    struct wmt_params32_sample_buffer_encoder *sample = (void *)outer;
    struct wmt_texture_info32 *texture;
    struct wmt_air_reflection32 *reflection;
    struct wmt_air_compiled_bitcode32 *compiled;
    struct wmt_display_description32 *description;
    struct wmt_hdr_metadata32 *metadata;
    unsigned char *bytes;
    UINT64 *shader, *error, *bitcode;
    UINT32 texture_guest, shader_guest, reflection_guest, error_guest;
    UINT32 bitcode_guest, compiled_guest, bytes_guest, description_guest, metadata_guest;
    NTSTATUS status;

    texture_guest = guest_alloc( sizeof(*texture), 8 );
    texture = decode_guest( texture_guest );
    memset( texture, 0x5a, sizeof(*texture) );
    texture->mach_port = 0x12345678;
    texture->gpu_resource_id = ~0ull;
    memset( outer, 0, sizeof(outer) );
    buffer_texture->info = (struct wmt_wire_ptr){texture_guest, 0};
    buffer_texture->ret = ~0ull;
    status = call_slot( 22, outer );
    check( status == STATUS_NOT_SUPPORTED && !buffer_texture->ret &&
        !texture->gpu_resource_id && texture->mach_port == 0x12345678,
        "slot22 outputs were not deterministic" );

    memset( outer, 0xff, 24 );
    capture->info.high = 1;
    capture->ret = 1;
    status = call_slot( 54, outer );
    check( status == STATUS_NOT_SUPPORTED && !capture->ret,
        "slot54 output was not deterministic" );
    memset( outer, 0xff, 24 );
    status = call_slot( 56, outer );
    check( status == STATUS_NOT_SUPPORTED && !info_ret->ret,
        "slot56 output was not deterministic" );

    shader_guest = guest_alloc( sizeof(*shader), 8 );
    reflection_guest = guest_alloc( sizeof(*reflection), 8 );
    error_guest = guest_alloc( sizeof(*error), 8 );
    shader = decode_guest( shader_guest );
    reflection = decode_guest( reflection_guest );
    error = decode_guest( error_guest );
    *shader = *error = ~0ull;
    memset( reflection, 0xff, sizeof(*reflection) );
    memset( outer, 0, 24 );
    initialize->shader = shader_guest;
    initialize->reflection = reflection_guest;
    initialize->error = error_guest;
    initialize->ret = 7;
    status = call_slot( 74, outer );
    check( status == STATUS_NOT_SUPPORTED && initialize->ret == -1 && !*shader && !*error &&
        !memcmp( reflection, &(struct wmt_air_reflection32){0}, sizeof(*reflection) ),
        "slot74 outputs were not deterministic" );

    bitcode_guest = guest_alloc( sizeof(*bitcode), 8 );
    bitcode = decode_guest( bitcode_guest );
    *bitcode = *error = ~0ull;
    memset( outer, 0, 32 );
    compile->bitcode = bitcode_guest;
    compile->error = error_guest;
    compile->ret = 7;
    status = call_slot( 76, outer );
    check( status == STATUS_NOT_SUPPORTED && compile->ret == -1 && !*bitcode && !*error,
        "slot76 outputs were not deterministic" );

    compiled_guest = guest_alloc( sizeof(*compiled), 8 );
    compiled = decode_guest( compiled_guest );
    memset( compiled, 0xff, sizeof(*compiled) );
    memset( outer, 0, 16 );
    get_bitcode->data_out = compiled_guest;
    status = call_slot( 77, outer );
    check( status == STATUS_NOT_SUPPORTED &&
        !memcmp( compiled, &(struct wmt_air_compiled_bitcode32){0}, sizeof(*compiled) ),
        "slot77 output was not deterministic" );

    bytes_guest = guest_alloc( 32, 8 );
    bytes = decode_guest( bytes_guest );
    memset( bytes, 0xcc, 32 );
    memset( outer, 0, 24 );
    get_error->buffer = bytes_guest;
    get_error->buffer_size = 32;
    get_error->ret_size = ~0u;
    status = call_slot( 79, outer );
    check( status == STATUS_NOT_SUPPORTED && !get_error->ret_size && !bytes[0] && bytes[1] == 0xcc,
        "slot79 bounded output was not deterministic" );

    memset( outer, 0xff, 40 );
    enumerate->buffer.high = 1;
    enumerate->ret_read = ~0ull;
    status = call_slot( 91, outer );
    check( status == STATUS_NOT_SUPPORTED && !enumerate->ret_read,
        "slot91 output was not deterministic" );

    description_guest = guest_alloc( sizeof(*description), 8 );
    description = decode_guest( description_guest );
    memset( description, 0xff, sizeof(*description) );
    memset( outer, 0, 16 );
    obj_ptr->arg = (struct wmt_wire_ptr){description_guest, 0};
    status = call_slot( 96, outer );
    check( status == STATUS_NOT_SUPPORTED &&
        !memcmp( description, &(struct wmt_display_description32){0}, sizeof(*description) ),
        "slot96 output was not deterministic" );

    metadata_guest = guest_alloc( sizeof(*metadata), 8 );
    metadata = decode_guest( metadata_guest );
    memset( metadata, 0xff, sizeof(*metadata) );
    memset( outer, 0xff, 32 );
    query->hdr_metadata = (struct wmt_wire_ptr){metadata_guest, 0};
    status = call_slot( 99, outer );
    check( status == STATUS_NOT_SUPPORTED && !query->colorspace && !query->ret &&
        !memcmp( metadata, &(struct wmt_hdr_metadata32){0}, sizeof(*metadata) ),
        "slot99 outputs were not deterministic" );

    memset( outer, 0xff, 32 );
    status = call_slot( 112, outer );
    check( status == STATUS_NOT_SUPPORTED && !archive->ret_object && !archive->ret_error,
        "slot112 outputs were not deterministic" );
    memset( outer, 0xff, 24 );
    status = call_slot( 113, outer );
    check( status == STATUS_NOT_SUPPORTED && !serialize->ret_error,
        "slot113 output was not deterministic" );
    memset( outer, 0xff, 32 );
    status = call_slot( 116, outer );
    check( status == STATUS_NOT_SUPPORTED && !cache->ret_data,
        "slot116 output was not deterministic" );

    memset( texture, 0x5a, sizeof(*texture) );
    texture->mach_port = 0x87654321;
    texture->gpu_resource_id = ~0ull;
    memset( outer, 0, 24 );
    info_ret->info = (struct wmt_wire_ptr){texture_guest, 0};
    info_ret->ret = ~0ull;
    status = call_slot( 120, outer );
    check( status == STATUS_NOT_SUPPORTED && !info_ret->ret && !texture->gpu_resource_id &&
        texture->mach_port == 0x87654321, "slot120 outputs were not deterministic" );

    memset( bytes, 0xcc, 32 );
    memset( outer, 0, 32 );
    range->data_out = (struct wmt_wire_ptr){bytes_guest, 0};
    range->data_length = 32;
    status = call_slot( 128, outer );
    check( status == STATUS_NOT_SUPPORTED &&
        !memcmp( bytes, (unsigned char[32]){0}, 32 ),
        "slot128 output was not deterministic" );
    memset( outer, 0xff, 32 );
    status = call_slot( 129, outer );
    check( status == STATUS_NOT_SUPPORTED && !sample->ret,
        "slot129 output was not deterministic" );
}

int main( int argc, char **argv )
{
    if (argc != 3 || (strcmp( argv[1], "zero" ) && strcmp( argv[1], "high" )) ||
        strlen( argv[2] ) != 64)
    {
        fprintf( stderr, "usage: %s zero|high schema-sha256\n", argv[0] );
        return 2;
    }
    bias = !strcmp( argv[1], "high" ) ? 0x70000000u : 0;
    owned_guest = bias + OWNED_OFFSET;
    if (!(arena = calloc( 1, ARENA_SIZE ))) return 2;
    test_descriptor( argv[2] );
    test_binding();
    test_forward_and_context();
    test_adapters();
    test_buffer();
    test_denials();
    test_deterministic_denial_outputs();
    test_snapshot_cleanup();
    free( owned_host );
    free( arena );
    if (failures)
    {
        fprintf( stderr, "%u v4 test(s) failed (%s bias)\n", failures, argv[1] );
        return 1;
    }
    printf( "winemetal v4 tests passed (%s bias, %u normal, %u legacy calls)\n",
            argv[1], normal_calls, legacy_calls );
    return 0;
}
