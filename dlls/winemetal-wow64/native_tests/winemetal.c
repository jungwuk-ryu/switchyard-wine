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
#define DEPTH_OBJECT 0x2020202020202020ull
#define TEXTURE_OBJECT 0x2121212121212121ull
#define STRING_OBJECT 0x6060606060606060ull
#define FORWARD_OBJECT 0x0202020202020202ull
#define DISPATCH_OBJECT 0x1141141141141141ull
#define AIR_SHADER 0x7474747474747474ull
#define AIR_BITCODE 0x7676767676767676ull
#define AIR_ERROR 0x7979797979797979ull
#define CACHE_OBJECT 0x1151171151171151ull
#define FORWARD_OWNED_OBJECT 0x0909090909090909ull

extern const struct wine_wow64_unixlib_companion_v6
    __wine_unix_call_wow64_companion_v6;
extern NTSTATUS wmt_test_release_backend_snapshot(void);
extern uint64_t wmt_test_snapshot_live_bytes(void);

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
    1, 1, 5, 5, 5, 5, 5, 5, 7, 5, 5, 5,
    1, 1, 5, 5, 5, 1, 31, 7, 7, 7, 7, 5,
    5, 5, 7, 5, 5, 7, 5, 5, 7, 1, 7, 7,
    3, 3, 3, 5, 5, 5, 5, 5, 5, 3, 1, 1,
    1, 5, 5, 5, 5, 5, 7, 1, 7, 7, 3, 1,
    7, 7, 5, 5, 1, 1, 5, 5, 5, 5, 3, 3,
    5, 1, 7, 1, 7, 3, 1, 7, 1, 7, 7, 1,
    7, 7, 1, 1, 3, 5, 5, 7, 5, 5, 5, 5,
    3, 3, 7, 7, 3, 7, 1, 1, 9, 5, 5, 3,
    5, 1, 1, 5, 7, 7, 7, 7, 7, 7, 3, 7,
    7, 1, 5, 5, 5, 5, 5, 5, 3, 7, 5, 7,
    5, 3, 3, 1, 1, 1
};

static const unsigned int denied[] =
{
    8, 22, 35, 37, 54, 56, 57, 58, 83,
    91, 96, 99, 100, 112, 113, 116, 118, 120, 128, 129, 131,
    133, 134,
};

static unsigned char *arena;
static UINT32 bias, next_offset = 0x1000, owned_guest;
static void *outer_address, *owned_host;
static SIZE_T outer_size, owned_mapped;
static struct ntdll_wow64_unixlib_call_context call_context;
static BOOL context_active, fail_outer_probe, fail_outer_write, fail_owned_guest;
static jmp_buf *active_exception_jmp;
static const void *fail_nested_read;
static void *fail_nested_write;
static unsigned int fail_nested_write_after;
static UINT64 owned_lease, pending_lease;
static BOOL owned_active, fail_metal;
static BOOL fail_air_compile, fail_air_get_arguments;
static BOOL fault_air_compile, fault_texture_replace, fault_buffer_update;
static BOOL fault_native_snapshot_copy;
static BOOL fault_air_shader_destroy, fault_air_bitcode_destroy, fault_air_error_destroy;
static BOOL fault_cache_init;
static BOOL fault_new_texture;
static BOOL fault_forward_owned;
static BOOL fault_forward_before_owned;
static BOOL fail_backend_image_retain;
static unsigned int failures, normal_calls, legacy_calls;
static unsigned int acquire_calls, release_calls, metal_calls, release_object_calls;
static unsigned int dispatch_snapshot_releases;
static unsigned int backend_image_retain_calls, backend_image_retain_publications;
static const void *resident_backend_image_base;
static unsigned int companion_image_pin_calls, companion_image_pin_publications;
static BOOL companion_image_pinned;
static unsigned int air_compile_calls, texture_replace_calls, buffer_update_calls;
static unsigned int air_shader_destroy_calls, air_bitcode_destroy_calls, air_error_destroy_calls;
static unsigned int air_constant_buffer_count = 1, air_argument_count = 2;
static BOOL fake_texture_is_3d;
static const void *texture_expected_data = "12345678";
static uint64_t texture_expected_size = 8, texture_expected_row = 4;
static uint64_t texture_expected_image, texture_expected_height = 2, texture_expected_depth = 1;
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
    if (dst == fail_nested_write && fail_nested_write_after && !--fail_nested_write_after)
        return STATUS_ACCESS_VIOLATION;
    if (dst == outer_address && size == outer_size)
    {
        if (fail_outer_write) return STATUS_ACCESS_VIOLATION;
        memcpy( dst, src, size );
        return STATUS_SUCCESS;
    }
    return ntdll_wow64_copy_to_user( dst, src, size );
}

NTSTATUS ntdll_wow64_atomic_writev( const struct ntdll_wow64_user_write_range *ranges,
                                    ULONG count )
{
    ULONG i;
    NTSTATUS status;

    if (!count) return STATUS_SUCCESS;
    if (!ranges || count > NTDLL_WOW64_USER_WRITEV_MAX)
        return STATUS_INVALID_PARAMETER;
    for (i = 0; i < count; ++i)
    {
        if (!ranges[i].size) continue;
        if (!ranges[i].src) return STATUS_ACCESS_VIOLATION;
        if ((status = ntdll_wow64_probe_user_write( ranges[i].dst, ranges[i].size ))) return status;
        if (ranges[i].dst == fail_nested_write && fail_nested_write_after &&
            !--fail_nested_write_after)
            return STATUS_ACCESS_VIOLATION;
    }
    for (i = 0; i < count; ++i)
        if (ranges[i].size) memcpy( ranges[i].dst, ranges[i].src, ranges[i].size );
    return STATUS_SUCCESS;
}

NTSTATUS ntdll_wow64_get_unixlib_call_context(
    struct ntdll_wow64_unixlib_call_context *context )
{
    if (!context_active || !context) return STATUS_INVALID_DEVICE_STATE;
    *context = call_context;
    return STATUS_SUCCESS;
}

void ntdll_set_exception_jmp_buf( jmp_buf jmp )
{
    if (jmp)
    {
        check( !active_exception_jmp, "nested exception jump target was installed" );
        if (active_exception_jmp) abort();
        active_exception_jmp = (jmp_buf *)jmp;
    }
    else active_exception_jmp = NULL;
}

void wmt_test_before_native_snapshot_copy(void)
{
    if (!fault_native_snapshot_copy) return;
    check( active_exception_jmp != NULL, "native snapshot fault lacked an exception guard" );
    if (!active_exception_jmp) abort();
    longjmp( *active_exception_jmp, 1 );
}

NTSTATUS wmt_test_retain_backend_image( const void *base )
{
    ++backend_image_retain_calls;
    if (!base) return STATUS_INVALID_PARAMETER;
    if (resident_backend_image_base)
        return resident_backend_image_base == base ? STATUS_SUCCESS : STATUS_INVALID_IMAGE_FORMAT;
    if (fail_backend_image_retain) return STATUS_INVALID_IMAGE_FORMAT;
    resident_backend_image_base = base;
    ++backend_image_retain_publications;
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

wmt_status_t wmt_pin_companion_image_resident(void)
{
    ++companion_image_pin_calls;
    if (!companion_image_pinned)
    {
        companion_image_pinned = TRUE;
        ++companion_image_pin_publications;
    }
    return STATUS_SUCCESS;
}

wmt_status_t wmt_dispatch_data_from_snapshot( void *bytes, uint64_t length,
                                              wmt_snapshot_release_func release_snapshot,
                                              uint64_t *ret_data )
{
    if (!bytes || !length || !release_snapshot || !ret_data) return STATUS_INVALID_PARAMETER;
    release_snapshot( bytes, length );
    ++dispatch_snapshot_releases;
    *ret_data = DISPATCH_OBJECT;
    return STATUS_SUCCESS;
}

wmt_status_t wmt_metal_texture_snapshot_rows( uint64_t texture, uint64_t origin_x,
                                              uint64_t origin_y, uint64_t origin_z,
                                              uint64_t width, uint64_t height,
                                              uint64_t depth, uint64_t level, uint64_t slice,
                                              uint64_t bytes_per_row, uint64_t bytes_per_image,
                                              uint64_t *ret_rows, uint64_t *ret_row_bytes )
{
    if (texture != 45 || origin_x || origin_y || origin_z || width != 1 || !height ||
        (depth != 1 && depth != 2) || level || slice || !ret_rows || !ret_row_bytes ||
        bytes_per_row % 4 || (bytes_per_image && bytes_per_image % 4) ||
        (!fake_texture_is_3d && bytes_per_image) || bytes_per_row >= 32767u * 4u)
        return STATUS_INVALID_PARAMETER;
    *ret_rows = height;
    *ret_row_bytes = 4;
    return STATUS_SUCCESS;
}

wmt_status_t wmt_metal_buffer_length( uint64_t buffer, uint64_t *ret_length )
{
    if (buffer != 107 || !ret_length) return STATUS_INVALID_PARAMETER;
    *ret_length = 16;
    return STATUS_SUCCESS;
}

static uint64_t fake_listener_token;

wmt_status_t wmt_shared_event_listener_create( uint64_t *ret_token )
{
    if (!ret_token) return STATUS_INVALID_PARAMETER;
    *ret_token = ++fake_listener_token;
    return STATUS_SUCCESS;
}

wmt_status_t wmt_shared_event_listener_start( uint64_t token )
{
    return token ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

wmt_status_t wmt_shared_event_listener_destroy( uint64_t token )
{
    return token ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

wmt_status_t wmt_shared_event_notify_win32( uint64_t shared_event, uint64_t event_handle,
                                            uint64_t listener_token, uint64_t value )
{
    (void)value;
    return shared_event && event_handle && listener_token ? STATUS_SUCCESS :
                                                           STATUS_INVALID_PARAMETER;
}

wmt_status_t wmt_quiesce_shared_event_listeners(void)
{
    return STATUS_SUCCESS;
}

void wmt_resume_shared_event_listeners(void)
{
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

static NTSTATUS forward_owned_object( void *args )
{
    struct wmt_params32_obj_u64_obj_ret *params = args;

    ++legacy_calls;
    if (fault_forward_before_owned)
    {
        check( active_exception_jmp != NULL, "early forwarded object fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    params->ret = FORWARD_OWNED_OBJECT;
    if (fault_forward_owned)
    {
        check( active_exception_jmp != NULL, "forwarded object fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
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

static NTSTATUS new_depth_stencil( void *args )
{
    struct wmt_params_const_info_ret *params = args;

    ++normal_calls;
    params->ret = DEPTH_OBJECT;
    return STATUS_SUCCESS;
}

static NTSTATUS new_texture( void *args )
{
    struct wmt_params_info_ret *params = args;
    struct wmt_texture_info *info = params->info.ptr;

    ++normal_calls;
    params->ret = TEXTURE_OBJECT;
    info->gpu_resource_id = 0x2121000021210000ull;
    if (fault_new_texture)
    {
        check( active_exception_jmp != NULL, "texture creation fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
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

static NTSTATUS air_initialize( void *args )
{
    struct wmt_air_initialize *params = args;

    ++normal_calls;
    if (params->bytecode_size != 4 || memcmp( params->bytecode, "DXBC", 4 ))
        return STATUS_INVALID_PARAMETER;
    memset( params->reflection, 0, sizeof(*params->reflection) );
    params->reflection->num_constant_buffers = air_constant_buffer_count;
    params->reflection->num_arguments = air_argument_count;
    *params->shader = AIR_SHADER;
    *params->error = 0;
    params->ret = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS air_compile( void *args )
{
    struct wmt_air_compile *params = args;
    struct wmt_air_argument *common;

    ++normal_calls;
    ++air_compile_calls;
    if (fault_air_compile)
    {
        check( active_exception_jmp != NULL, "AIR backend fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    if (params->shader != AIR_SHADER || strcmp( params->function_name, "main" ) ||
        params->arguments->type != UINT32_MAX ||
        !(common = params->arguments->next) || common->next ||
        (common->type != 2 && common->type != 5))
        return STATUS_INVALID_PARAMETER;
    if (fail_air_compile)
    {
        *params->bitcode = 0;
        *params->error = AIR_ERROR;
        params->ret = 1;
    }
    else
    {
        *params->bitcode = AIR_BITCODE;
        *params->error = 0;
        params->ret = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS air_pipeline_compile( void *args )
{
    struct wmt_air_pipeline_compile *params = args;

    ++normal_calls;
    ++air_compile_calls;
    if (params->first_shader != AIR_SHADER || params->second_shader != AIR_SHADER ||
        strcmp( params->function_name, "main" ) || params->arguments->type != UINT32_MAX)
        return STATUS_INVALID_PARAMETER;
    *params->bitcode = AIR_BITCODE;
    *params->error = 0;
    params->ret = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS air_get_bitcode( void *args )
{
    static const unsigned char metallib[] = {0x4d, 0x54, 0x4c, 0x42};
    struct wmt_air_get_bitcode *params = args;

    ++normal_calls;
    if (params->bitcode != AIR_BITCODE) return STATUS_INVALID_PARAMETER;
    params->data_out->data = (UINT_PTR)metallib;
    params->data_out->size = sizeof(metallib);
    return STATUS_SUCCESS;
}

static NTSTATUS air_get_error( void *args )
{
    struct wmt_air_get_error *params = args;

    ++normal_calls;
    if (params->error != AIR_ERROR || params->buffer_size < 4) return STATUS_INVALID_PARAMETER;
    memcpy( params->buffer, "bad", 4 );
    params->ret_size = 3;
    return STATUS_SUCCESS;
}

static NTSTATUS air_get_arguments( void *args )
{
    struct wmt_air_get_arguments *params = args;

    ++normal_calls;
    if (params->shader != AIR_SHADER) return STATUS_INVALID_PARAMETER;
    if (fail_air_get_arguments) return STATUS_UNSUCCESSFUL;
    if (!!params->constant_buffers != !!air_constant_buffer_count ||
        !!params->arguments != !!air_argument_count)
        return STATUS_INVALID_PARAMETER;
    if (air_constant_buffer_count)
        params->constant_buffers[0] = (struct wmt_air_shader_argument){1, 2, 3, 4};
    if (air_argument_count)
    {
        params->arguments[0] = (struct wmt_air_shader_argument){5, 6, 7, 8};
        params->arguments[1] = (struct wmt_air_shader_argument){9, 10, 11, 12};
    }
    return STATUS_SUCCESS;
}

static NTSTATUS air_destroy_shader( void *args )
{
    (void)args;
    ++normal_calls;
    ++air_shader_destroy_calls;
    if (fault_air_shader_destroy)
    {
        check( active_exception_jmp != NULL, "shader destroy fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS air_destroy_bitcode( void *args )
{
    (void)args;
    ++normal_calls;
    ++air_bitcode_destroy_calls;
    if (fault_air_bitcode_destroy)
    {
        check( active_exception_jmp != NULL, "bitcode destroy fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS air_destroy_error( void *args )
{
    (void)args;
    ++normal_calls;
    ++air_error_destroy_calls;
    if (fault_air_error_destroy)
    {
        check( active_exception_jmp != NULL, "error destroy fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS texture_replace( void *args )
{
    struct wmt_params_texture_replace *params = args;

    ++normal_calls;
    ++texture_replace_calls;
    if (fault_texture_replace)
    {
        check( active_exception_jmp != NULL, "texture backend fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    if (params->texture != 45 || params->size.height != texture_expected_height ||
        params->size.depth != texture_expected_depth ||
        params->bytes_per_row != texture_expected_row ||
        params->bytes_per_image != texture_expected_image ||
        memcmp( params->data.ptr, texture_expected_data, texture_expected_size ))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS buffer_update( void *args )
{
    struct wmt_params_buffer_update *params = args;

    ++normal_calls;
    ++buffer_update_calls;
    if (fault_buffer_update)
    {
        check( active_exception_jmp != NULL, "buffer backend fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    if (params->buffer != 107 || params->offset != 8 || params->length != 4 ||
        memcmp( params->data.ptr, "data", 4 )) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS cache_init( void *args )
{
    struct wmt_params_cache_init *params = args;

    ++normal_calls;
    if (!params->path.ptr || strcmp( params->path.ptr, "/tmp/cache" ) || params->version != 7)
        return STATUS_INVALID_PARAMETER;
    params->ret = CACHE_OBJECT;
    if (fault_cache_init)
    {
        check( active_exception_jmp != NULL, "cache init fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    return STATUS_SUCCESS;
}

static void install_test_handlers(void)
{
    normal[45] = texture_replace;
    normal[20] = new_depth_stencil;
    normal[21] = new_texture;
    normal[74] = air_initialize;
    normal[75] = air_destroy_shader;
    normal[76] = air_compile;
    normal[77] = air_get_bitcode;
    normal[78] = air_destroy_bitcode;
    normal[79] = air_get_error;
    normal[80] = air_destroy_error;
    normal[81] = air_pipeline_compile;
    normal[82] = air_pipeline_compile;
    normal[84] = air_pipeline_compile;
    normal[85] = air_pipeline_compile;
    normal[88] = air_get_arguments;
    normal[107] = buffer_update;
    normal[115] = cache_init;
    normal[117] = cache_init;
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
    static const BYTE header_hash[32] = WINE_WOW64_UNIXLIB_COMPANION_V6_ABI_SHA256;
    const struct wine_unixlib_dispatch_source_v2 *source =
        &__wine_unix_call_wow64_dispatch_v2;
    BYTE parsed[32];
    unsigned int i;

    for (i = 0; i < 32; ++i)
        parsed[i] = hex_digit( sha[2 * i] ) * 16 + hex_digit( sha[2 * i + 1] );
    check( __wine_unix_call_wow64_companion_v6.version == 6 &&
        __wine_unix_call_wow64_companion_v6.size ==
            sizeof(__wine_unix_call_wow64_companion_v6) &&
        __wine_unix_call_wow64_companion_v6.entry_count == WMT_UNIX_CALL_COUNT &&
        !__wine_unix_call_wow64_companion_v6.flags &&
        __wine_unix_call_wow64_companion_v6.quiesce,
        "v6 companion descriptor mismatch" );
    check( !memcmp( parsed, header_hash, 32 ) &&
        !memcmp( parsed, __wine_unix_call_wow64_companion_v6.abi_sha256, 32 ),
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
    struct wine_wow64_unixlib_binding_v6 binding;
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
    install_test_handlers();
    legacy[2] = forward_object;
    legacy[9] = forward_owned_object;
    normal[83] = NULL;
    legacy[83] = NULL;
    memset( &binding, 0, sizeof(binding) );
    binding.version = 6;
    binding.size = sizeof(binding);
    binding.entry_count = WMT_UNIX_CALL_COUNT;
    binding.normal_funcs = normal;
    binding.legacy_wow64_funcs = legacy;
    binding.owned_backing_codec = &backing_codec;
    invalid.capabilities |= WINE_WOW64_UNIXLIB_CAP_OWNED_MEMORY_ALIAS;
    binding.codec = &invalid;
    status = __wine_unix_call_wow64_companion_v6.bind( &binding );
    check( status == STATUS_INVALID_PARAMETER, "extra alias capability was accepted" );
    binding.codec = &codec;
    normal[82] = NULL;
    status = __wine_unix_call_wow64_companion_v6.bind( &binding );
    check( status == STATUS_INVALID_IMAGE_FORMAT, "malformed source table was accepted" );
    normal[82] = air_pipeline_compile;
    fail_backend_image_retain = TRUE;
    status = __wine_unix_call_wow64_companion_v6.bind( &binding );
    fail_backend_image_retain = FALSE;
    check( status == STATUS_INVALID_IMAGE_FORMAT && backend_image_retain_calls == 1 &&
        !backend_image_retain_publications && companion_image_pin_calls == 1 &&
        companion_image_pin_publications == 1,
        "backend callback-code residency failure published a binding" );
    status = __wine_unix_call_wow64_companion_v6.bind( &binding );
    check( !status && backend_image_retain_calls == 2 && backend_image_retain_publications == 1 &&
        companion_image_pin_calls == 2 && companion_image_pin_publications == 1,
        "valid v6 binding did not retain one backend callback image" );
    memcpy( normal_copy, normal, sizeof(normal_copy) );
    memcpy( legacy_copy, legacy, sizeof(legacy_copy) );
    binding.normal_funcs = normal_copy;
    binding.legacy_wow64_funcs = legacy_copy;
    status = __wine_unix_call_wow64_companion_v6.bind( &binding );
    check( !status, "content-identical v6 rebind failed" );
    binding.normal_funcs = normal;
    binding.legacy_wow64_funcs = legacy;
    normal[19] = normal_generic;
    legacy[2] = legacy_generic;
    status = __wine_unix_call_wow64_companion_v6.bind( &binding );
    check( status == STATUS_INVALID_DEVICE_STATE, "content-different rebind was accepted" );
    normal[83] = normal_generic;
    legacy[83] = legacy_generic;
    check( wmt_normal_call( 83, NULL ) == STATUS_NOT_SUPPORTED &&
        wmt_forward_call( 83, NULL ) == STATUS_NOT_SUPPORTED,
        "source-table mutation changed snapshotted null entries" );
}

static void test_snapshot_cleanup(void)
{
    struct wine_wow64_unixlib_binding_v6 binding;
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
    install_test_handlers();
    legacy[2] = forward_object;
    legacy[9] = forward_owned_object;
    normal[83] = NULL;
    legacy[83] = NULL;
    memset( &binding, 0, sizeof(binding) );
    binding.version = 6;
    binding.size = sizeof(binding);
    binding.entry_count = WMT_UNIX_CALL_COUNT;
    binding.normal_funcs = normal;
    binding.legacy_wow64_funcs = legacy;
    binding.codec = &codec;
    binding.owned_backing_codec = &backing_codec;
    status = __wine_unix_call_wow64_companion_v6.bind( &binding );
    check( !status && backend_image_retain_calls == 3 &&
        backend_image_retain_publications == 1 && companion_image_pin_calls == 3 &&
        companion_image_pin_publications == 1,
        "backend snapshot rebind acquired another permanent callback-image ref" );
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
    struct wmt_depth_stencil_info32 *depth;
    struct wmt_texture_info32 *texture;
    struct wmt_params32_info_ret info;
    struct wmt_params32_string string;
    struct wmt_params32_obj_u64_obj_ret blob;
    struct wmt_params32_cache_init cache;
    UINT32 sampler_guest, depth_guest, texture_guest, string_guest;
    UINT32 cache_guest;
    unsigned int before;
    uint64_t object;
    NTSTATUS status;

    object = 0x0101010101010101ull;
    before = legacy_calls;
    fail_outer_write = TRUE;
    status = call_slot( 0, &object );
    fail_outer_write = FALSE;
    check( !status && legacy_calls == before + 1,
        "input-only slot0 attempted a final outer publication" );
    before = legacy_calls;
    fail_outer_write = TRUE;
    status = call_slot( 1, &object );
    fail_outer_write = FALSE;
    check( !status && legacy_calls == before + 1,
        "input-only slot1 attempted a final outer publication" );

    blob.handle = 9;
    blob.arg = 1;
    blob.ret = ~0ull;
    before = release_object_calls;
    fault_forward_before_owned = TRUE;
    status = call_slot( WMT_CALL_NEW_COMMAND_QUEUE, &blob );
    fault_forward_before_owned = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && !blob.ret &&
        release_object_calls == before,
        "early forwarded fault released an untrusted old output" );
    blob.ret = ~0ull;
    fault_forward_owned = TRUE;
    status = call_slot( WMT_CALL_NEW_COMMAND_QUEUE, &blob );
    fault_forward_owned = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && !blob.ret &&
        release_object_calls == before + 1,
        "forwarded owned output fault leaked or published its object" );
    blob.ret = 0xccccccccccccccccull;
    before = release_object_calls;
    fail_outer_write = TRUE;
    status = call_slot( WMT_CALL_NEW_COMMAND_QUEUE, &blob );
    fail_outer_write = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && blob.ret == 0xccccccccccccccccull &&
        release_object_calls == before + 1,
        "forwarded owned output outer failure leaked its object" );

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

    depth_guest = guest_alloc( sizeof(*depth), 8 );
    depth = decode_guest( depth_guest );
    memset( depth, 0, sizeof(*depth) );
    info.object = 20;
    info.info = (struct wmt_wire_ptr){depth_guest, 0};
    info.ret = 0xccccccccccccccccull;
    before = release_object_calls;
    fail_outer_write = TRUE;
    status = call_slot( 20, &info );
    fail_outer_write = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && info.ret == 0xccccccccccccccccull &&
        release_object_calls == before + 1,
        "slot20 outer publication failure leaked its owned object" );

    texture_guest = guest_alloc( sizeof(*texture), 8 );
    texture = decode_guest( texture_guest );
    memset( texture, 0, sizeof(*texture) );
    info.object = 21;
    info.info = (struct wmt_wire_ptr){texture_guest, 0};
    info.ret = ~0ull;
    before = release_object_calls;
    fault_new_texture = TRUE;
    status = call_slot( 21, &info );
    fault_new_texture = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && !info.ret &&
        !texture->gpu_resource_id && release_object_calls == before + 1,
        "slot21 fault after native object creation leaked or published the object" );

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

    cache_guest = guest_alloc( sizeof("/tmp/cache"), 1 );
    memcpy( decode_guest( cache_guest ), "/tmp/cache", sizeof("/tmp/cache") );
    cache.path = (struct wmt_wire_ptr){cache_guest, 0};
    cache.version = 7;
    cache.ret = ~0ull;
    before = release_object_calls;
    fault_cache_init = TRUE;
    status = call_slot( WMT_CALL_CACHE_READER_INIT, &cache );
    fault_cache_init = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && !cache.ret &&
        release_object_calls == before + 1 && !wmt_test_snapshot_live_bytes(),
        "faulting cache init leaked its partially-created native object" );
    cache.ret = 0xccccccccccccccccull;
    before = release_object_calls;
    fail_outer_write = TRUE;
    status = call_slot( WMT_CALL_CACHE_WRITER_INIT, &cache );
    fail_outer_write = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && cache.ret == 0xccccccccccccccccull &&
        release_object_calls == before + 1,
        "cache init outer publication failure leaked its native object" );
    cache.ret = 0;
    status = call_slot( WMT_CALL_CACHE_READER_INIT, &cache );
    check( !status && cache.ret == CACHE_OBJECT,
        "cache init did not recover after guarded cleanup failures" );
    check( !call_slot( 1, &cache.ret ), "cache object could not be released" );
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

static void test_air_and_upload(void)
{
    struct wmt_air_initialize32 initialize;
    struct wmt_air_compile32 compile;
    struct wmt_air_pipeline_compile32 pipeline;
    struct wmt_air_get_bitcode32 get_bitcode;
    struct wmt_air_get_error32 get_error;
    struct wmt_air_get_arguments32 get_arguments;
    struct wmt_params32_obj_u64_obj_ret dispatch;
    struct wmt_params32_texture_replace replace;
    struct wmt_params32_buffer_update update;
    struct wmt_air_common_argument32 *common, *duplicate_common;
    struct wmt_air_stream_output_argument32 *stream_output;
    struct wmt_air_input_layout_argument32 *input_layout;
    struct wmt_air_gs_passthrough_argument32 *gs_passthrough;
    struct wmt_air_stream_output_element *stream_element;
    struct wmt_air_input_element *input_element;
    struct wmt_air_reflection32 *reflection;
    struct wmt_air_compiled_bitcode32 *compiled;
    struct wmt_air_shader_argument *constant_buffers, *arguments;
    UINT64 *shader, *error, *bitcode;
    char *message;
    UINT32 bytecode_guest, shader_guest, error_guest, reflection_guest;
    UINT32 common_guest, duplicate_common_guest, function_guest, bitcode_guest, compiled_guest;
    UINT32 stream_guest, stream_element_guest, input_guest, input_element_guest;
    UINT32 gs_passthrough_guest;
    UINT32 constant_guest, arguments_guest, message_guest, data_guest, data_base_guest;
    UINT32 direct_data_guest;
    unsigned int before, index, release_before, snapshot_before;
    NTSTATUS status;

    bytecode_guest = guest_alloc( 4, 4 );
    memcpy( decode_guest( bytecode_guest ), "DXBC", 4 );
    shader_guest = guest_alloc( sizeof(*shader), 8 );
    error_guest = guest_alloc( sizeof(*error), 8 );
    reflection_guest = guest_alloc( sizeof(*reflection), 8 );
    shader = decode_guest( shader_guest );
    error = decode_guest( error_guest );
    reflection = decode_guest( reflection_guest );
    memset( &initialize, 0, sizeof(initialize) );
    initialize.bytecode = bytecode_guest;
    initialize.bytecode_size = 4;
    initialize.shader = shader_guest;
    initialize.reflection = reflection_guest;
    initialize.error = error_guest;
    status = call_slot( 74, &initialize );
    check( !status && !initialize.ret && *shader == AIR_SHADER && !*error &&
        reflection->num_constant_buffers == 1 && reflection->num_arguments == 2,
        "slot74 did not bridge shader initialization" );

    constant_guest = guest_alloc( sizeof(*constant_buffers), 8 );
    arguments_guest = guest_alloc( 2 * sizeof(*arguments), 8 );
    constant_buffers = decode_guest( constant_guest );
    arguments = decode_guest( arguments_guest );
    memset( &get_arguments, 0, sizeof(get_arguments) );
    get_arguments.shader = *shader;
    get_arguments.constant_buffers = constant_guest;
    get_arguments.arguments = arguments_guest;
    status = call_slot( 88, &get_arguments );
    check( !status && constant_buffers[0].type == 1 && constant_buffers[0].slot == 2 &&
        arguments[0].type == 5 && arguments[1].structure_ptr_offset == 12,
        "slot88 did not use registered reflection bounds" );
    memset( constant_buffers, 0xaa, sizeof(*constant_buffers) );
    memset( arguments, 0xbb, 2 * sizeof(*arguments) );
    before = normal_calls;
    fail_nested_write = arguments;
    fail_nested_write_after = 1;
    status = call_slot( 88, &get_arguments );
    fail_nested_write = NULL;
    fail_nested_write_after = 0;
    check( status == STATUS_ACCESS_VIOLATION && normal_calls == before &&
        !memcmp( constant_buffers,
                 (struct wmt_air_shader_argument[]){{0xaaaaaaaa, 0xaaaaaaaa,
                                                     0xaaaaaaaa, 0xaaaaaaaa}},
                 sizeof(*constant_buffers) ) &&
        !memcmp( arguments,
                 (struct wmt_air_shader_argument[]){{0xbbbbbbbb, 0xbbbbbbbb,
                                                     0xbbbbbbbb, 0xbbbbbbbb},
                                                    {0xbbbbbbbb, 0xbbbbbbbb,
                                                     0xbbbbbbbb, 0xbbbbbbbb}},
                 2 * sizeof(*arguments) ),
        "slot88 zero publication partially modified output" );
    memset( constant_buffers, 0xaa, sizeof(*constant_buffers) );
    memset( arguments, 0xaa, 2 * sizeof(*arguments) );
    fail_nested_write = arguments;
    fail_nested_write_after = 2;
    status = call_slot( 88, &get_arguments );
    fail_nested_write = NULL;
    fail_nested_write_after = 0;
    check( status == STATUS_ACCESS_VIOLATION &&
        !memcmp( constant_buffers, &(struct wmt_air_shader_argument){0}, sizeof(*constant_buffers) ) &&
        !memcmp( arguments, (struct wmt_air_shader_argument[2]){{0}, {0}}, 2 * sizeof(*arguments) ),
        "slot88 second result range failure did not preserve atomic zeros" );
    memset( constant_buffers, 0xaa, sizeof(*constant_buffers) );
    memset( arguments, 0xaa, 2 * sizeof(*arguments) );
    fail_nested_write = constant_buffers;
    fail_nested_write_after = 2;
    status = call_slot( 88, &get_arguments );
    fail_nested_write = NULL;
    fail_nested_write_after = 0;
    check( status == STATUS_ACCESS_VIOLATION &&
        !memcmp( constant_buffers, &(struct wmt_air_shader_argument){0}, sizeof(*constant_buffers) ) &&
        !memcmp( arguments, (struct wmt_air_shader_argument[2]){{0}, {0}}, 2 * sizeof(*arguments) ),
        "slot88 first result range failure did not preserve atomic zeros" );
    memset( constant_buffers, 0xaa, sizeof(*constant_buffers) );
    memset( arguments, 0xbb, 2 * sizeof(*arguments) );
    fail_air_get_arguments = TRUE;
    status = call_slot( 88, &get_arguments );
    fail_air_get_arguments = FALSE;
    check( status == STATUS_UNSUCCESSFUL &&
        !memcmp( constant_buffers, &(struct wmt_air_shader_argument){0}, sizeof(*constant_buffers) ) &&
        !memcmp( arguments, (struct wmt_air_shader_argument[2]){{0}, {0}}, 2 * sizeof(*arguments) ),
        "slot88 native failure did not leave both outputs zero" );
    memset( constant_buffers, 0xaa, sizeof(*constant_buffers) );
    get_arguments.arguments = UINT32_MAX - 7;
    before = normal_calls;
    status = call_slot( 88, &get_arguments );
    get_arguments.arguments = arguments_guest;
    check( status == STATUS_INVALID_PARAMETER && normal_calls == before &&
        !memcmp( constant_buffers,
                 (struct wmt_air_shader_argument[]){{0xaaaaaaaa, 0xaaaaaaaa,
                                                     0xaaaaaaaa, 0xaaaaaaaa}},
                 sizeof(*constant_buffers) ),
        "slot88 accepted a wrapping second output range" );

    function_guest = guest_alloc( 5, 1 );
    memcpy( decode_guest( function_guest ), "main", 5 );
    bitcode_guest = guest_alloc( sizeof(*bitcode), 8 );
    bitcode = decode_guest( bitcode_guest );
    memset( &compile, 0, sizeof(compile) );
    compile.shader = *shader;
    compile.function_name = function_guest;
    compile.bitcode = bitcode_guest;
    compile.error = error_guest;

    common_guest = guest_alloc( sizeof(*common), 4 );
    common = decode_guest( common_guest );
    memset( common, 0, sizeof(*common) );
    common->type = 2;
    common->metal_version = 320;

    stream_guest = guest_alloc( sizeof(*stream_output), 4 );
    stream_element_guest = guest_alloc( sizeof(*stream_element), 4 );
    stream_output = decode_guest( stream_guest );
    stream_element = decode_guest( stream_element_guest );
    memset( stream_output, 0, sizeof(*stream_output) );
    memset( stream_element, 0, sizeof(*stream_element) );
    stream_output->type = 1;
    stream_output->num_output_slots = 4;
    stream_output->num_elements = 1;
    stream_output->strides[0] = 4;
    stream_output->elements = stream_element_guest;
    stream_element->output_slot = 4;
    compile.arguments = stream_guest;
    before = air_compile_calls;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "out-of-range AIR stream-output slot reached the backend" );
    stream_element->output_slot = 0;
    stream_element->offset = 4;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "out-of-range AIR stream-output span reached the backend" );
    stream_element->offset = 0;

    input_guest = guest_alloc( sizeof(*input_layout), 4 );
    input_element_guest = guest_alloc( sizeof(*input_element), 4 );
    input_layout = decode_guest( input_guest );
    input_element = decode_guest( input_element_guest );
    memset( input_layout, 0, sizeof(*input_layout) );
    memset( input_element, 0, sizeof(*input_element) );
    input_layout->type = 4;
    input_layout->slot_mask = 1;
    input_layout->num_elements = 1;
    input_layout->elements = input_element_guest;
    input_element->format = 0;
    compile.arguments = input_guest;
    before = air_compile_calls;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "invalid AIR input attribute format reached the backend" );

    gs_passthrough_guest = guest_alloc( sizeof(*gs_passthrough), 4 );
    gs_passthrough = decode_guest( gs_passthrough_guest );
    memset( gs_passthrough, 0, sizeof(*gs_passthrough) );
    gs_passthrough->type = 5;
    gs_passthrough->data = 0xffff031f; /* reg 31/component 3, unused second pair */
    compile.arguments = gs_passthrough_guest;
    status = call_slot( 76, &compile );
    check( !status && air_compile_calls == before + 1,
        "valid boundary AIR GS pass-through registers were rejected" );
    check( !call_slot( 78, bitcode ),
        "valid boundary AIR GS pass-through bitcode was not destroyed" );
    before = air_compile_calls;
    gs_passthrough->data = 0xffff0320;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "out-of-range AIR GS pass-through register reached the backend" );
    gs_passthrough->data = 0xffff041f;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "out-of-range AIR GS pass-through component reached the backend" );
    gs_passthrough->data = 0xffff03ff;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "mixed AIR GS pass-through sentinel reached the backend" );

    compile.arguments = common_guest;
    common->flags = 1;
    fault_air_compile = TRUE;
    status = call_slot( 76, &compile );
    fault_air_compile = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && !*bitcode && !*error &&
        !wmt_test_snapshot_live_bytes(),
        "faulting AIR backend leaked a graph, output owner, or snapshot quota" );
    status = call_slot( 76, &compile );
    check( !status && !compile.ret && *bitcode == AIR_BITCODE && !*error,
        "slot76 did not recover after a guarded backend fault" );

    direct_data_guest = guest_alloc( 4, 4 );
    memcpy( decode_guest( direct_data_guest ), "MTLB", 4 );
    dispatch = (struct wmt_params32_obj_u64_obj_ret){direct_data_guest, 4,
                                                      0xaaaaaaaaaaaaaaaaull};
    release_before = release_object_calls;
    snapshot_before = dispatch_snapshot_releases;
    fail_outer_write = TRUE;
    status = call_slot( 114, &dispatch );
    fail_outer_write = FALSE;
    check( status == STATUS_ACCESS_VIOLATION &&
        dispatch.ret == 0xaaaaaaaaaaaaaaaaull &&
        dispatch_snapshot_releases == snapshot_before + 1 &&
        release_object_calls == release_before + 1,
        "direct slot114 outer failure did not release its copied object exactly once" );
    dispatch.ret = 0;
    status = call_slot( 114, &dispatch );
    check( !status && dispatch.ret == DISPATCH_OBJECT,
        "direct slot114 did not publish dispatch-owned copied data" );
    check( !call_slot( 1, &dispatch.ret ),
        "direct slot114 dispatch data could not be released" );

    compiled_guest = guest_alloc( sizeof(*compiled), 8 );
    compiled = decode_guest( compiled_guest );
    memset( &get_bitcode, 0, sizeof(get_bitcode) );
    get_bitcode.bitcode = *bitcode;
    get_bitcode.data_out = compiled_guest;
    fault_native_snapshot_copy = TRUE;
    status = call_slot( 77, &get_bitcode );
    fault_native_snapshot_copy = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && !compiled->data && !compiled->size &&
        !wmt_test_snapshot_live_bytes(),
        "faulting native bitcode snapshot leaked quota or left the AIR mutex locked" );
    status = call_slot( 77, &get_bitcode );
    check( !status && compiled->data > UINT32_MAX && compiled->size == 4,
        "slot77 did not recover after a guarded native snapshot fault" );
    dispatch = (struct wmt_params32_obj_u64_obj_ret){compiled->data, compiled->size,
                                                      0xbbbbbbbbbbbbbbbbull};
    release_before = release_object_calls;
    fail_outer_write = TRUE;
    status = call_slot( 114, &dispatch );
    fail_outer_write = FALSE;
    check( status == STATUS_ACCESS_VIOLATION &&
        dispatch.ret == 0xbbbbbbbbbbbbbbbbull &&
        release_object_calls == release_before + 1,
        "slot77 token retain leaked after slot114 outer publication failure" );
    dispatch.ret = 0;
    status = call_slot( 114, &dispatch );
    check( !status && dispatch.ret == DISPATCH_OBJECT,
        "slot114 did not resolve the bitcode token" );
    status = call_slot( 78, bitcode );
    check( !status, "slot78 did not destroy registered bitcode" );
    status = call_slot( 114, &dispatch );
    check( status == STATUS_INVALID_PARAMETER && !dispatch.ret,
        "slot114 resolved a token after bitcode destruction" );
    dispatch.ret = DISPATCH_OBJECT;
    check( !call_slot( 1, &dispatch.ret ),
        "retained slot77 dispatch data could not be released" );

    fail_air_compile = TRUE;
    status = call_slot( 76, &compile );
    fail_air_compile = FALSE;
    check( !status && compile.ret == 1 && !*bitcode && *error == AIR_ERROR,
        "slot76 did not publish the compiler error owner" );
    message_guest = guest_alloc( 16, 1 );
    message = decode_guest( message_guest );
    memset( &get_error, 0, sizeof(get_error) );
    get_error.error = *error;
    get_error.buffer = message_guest;
    get_error.buffer_size = 16;
    status = call_slot( 79, &get_error );
    check( !status && get_error.ret_size == 3 && !strcmp( message, "bad" ),
        "slot79 did not copy the bounded error string" );
    before = normal_calls;
    get_error.buffer = 0;
    get_error.buffer_size = 0;
    get_error.ret_size = ~0u;
    status = call_slot( 79, &get_error );
    check( !status && !get_error.ret_size && normal_calls == before,
        "slot79 zero-size query reached the underflowing backend" );
    status = call_slot( 80, error );
    check( !status, "slot80 did not free the error" );

    for (index = 81; index <= 85; ++index)
    {
        if (index == 83) continue;
        memset( &pipeline, 0, sizeof(pipeline) );
        pipeline.first_shader = AIR_SHADER;
        pipeline.second_shader = AIR_SHADER;
        pipeline.arguments = common_guest;
        pipeline.function_name = function_guest;
        pipeline.bitcode = bitcode_guest;
        pipeline.error = error_guest;
        status = call_slot( index, &pipeline );
        check( !status && !pipeline.ret && *bitcode == AIR_BITCODE,
            "pipeline AIR slot did not bridge the argument graph" );
        call_slot( 78, bitcode );
    }

    common->next = common_guest;
    before = air_compile_calls;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "cyclic AIR argument graph reached the backend" );
    common->next = 0;
    common->metal_version = 999;
    before = air_compile_calls;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "invalid AIR Metal version reached the backend" );
    common->metal_version = 320;
    duplicate_common_guest = guest_alloc( sizeof(*duplicate_common), 4 );
    duplicate_common = decode_guest( duplicate_common_guest );
    *duplicate_common = *common;
    common->next = duplicate_common_guest;
    status = call_slot( 76, &compile );
    check( status == STATUS_INVALID_PARAMETER && air_compile_calls == before,
        "duplicate AIR argument type reached the backend" );
    common->next = 0;

    data_guest = guest_alloc( 32, 4 );
    data_base_guest = data_guest;
    memcpy( decode_guest( data_guest ), "12345678", 8 );
    memset( &replace, 0, sizeof(replace) );
    replace.texture = 45;
    replace.size = (struct wmt_size){1, 2, 1};
    replace.data = (struct wmt_wire_ptr){data_guest, 0};
    replace.bytes_per_row = 4;
    replace.bytes_per_image = 0;
    status = call_slot( 45, &replace );
    check( !status && texture_replace_calls, "slot45 did not snapshot texture input" );
    fault_texture_replace = TRUE;
    status = call_slot( 45, &replace );
    fault_texture_replace = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && !wmt_test_snapshot_live_bytes(),
        "faulting texture backend leaked snapshot quota" );
    status = call_slot( 45, &replace );
    check( !status, "slot45 did not recover after a guarded backend fault" );
    before = texture_replace_calls;
    replace.bytes_per_row = 3;
    status = call_slot( 45, &replace );
    check( status == STATUS_INVALID_PARAMETER && texture_replace_calls == before,
        "slot45 undersized row pitch reached the backend" );
    replace.bytes_per_row = 6;
    status = call_slot( 45, &replace );
    check( status == STATUS_INVALID_PARAMETER && texture_replace_calls == before,
        "slot45 misaligned row pitch reached the backend" );
    replace.bytes_per_row = 4;
    replace.bytes_per_image = 8;
    status = call_slot( 45, &replace );
    check( status == STATUS_INVALID_PARAMETER && texture_replace_calls == before,
        "slot45 non-3D image pitch reached the backend" );
    fake_texture_is_3d = TRUE;
    replace.size.depth = 2;
    replace.bytes_per_image = 10;
    status = call_slot( 45, &replace );
    check( status == STATUS_INVALID_PARAMETER && texture_replace_calls == before,
        "slot45 misaligned image pitch reached the backend" );
    fake_texture_is_3d = FALSE;
    replace.size.depth = 1;
    replace.bytes_per_image = 0;
    replace.bytes_per_row = 32767u * 4u;
    status = call_slot( 45, &replace );
    check( status == STATUS_INVALID_PARAMETER && texture_replace_calls == before,
        "slot45 oversized raw row pitch reached the backend" );
    replace.bytes_per_row = 4;
    replace.origin.x = 1;
    status = call_slot( 45, &replace );
    check( status == STATUS_INVALID_PARAMETER && texture_replace_calls == before,
        "slot45 invalid region reached the backend" );
    replace.origin.x = 0;

    data_guest = bias + ARENA_SIZE - 12;
    texture_expected_data = decode_guest( data_guest );
    texture_expected_size = 12;
    texture_expected_row = 8;
    texture_expected_height = 2;
    memcpy( (void *)texture_expected_data, "abcdefghijkl", 12 );
    replace.data.low = data_guest;
    replace.bytes_per_row = 8;
    status = call_slot( 45, &replace );
    check( !status, "slot45 copied padding after the final texture row" );

    data_guest = bias + ARENA_SIZE - 28;
    texture_expected_data = decode_guest( data_guest );
    texture_expected_size = 28;
    texture_expected_image = 16;
    texture_expected_depth = 2;
    memcpy( (void *)texture_expected_data, "abcdefghijklmnopqrstuvwxyz12", 28 );
    replace.data.low = data_guest;
    replace.size.depth = 2;
    replace.bytes_per_image = 16;
    fake_texture_is_3d = TRUE;
    status = call_slot( 45, &replace );
    check( !status, "slot45 copied padding after the final texture image" );

    data_guest = bias + ARENA_SIZE - 8;
    texture_expected_data = decode_guest( data_guest );
    texture_expected_size = 8;
    texture_expected_row = 4;
    texture_expected_image = 12;
    texture_expected_depth = 1;
    memcpy( (void *)texture_expected_data, "12345678", 8 );
    replace.data.low = data_guest;
    replace.size.depth = 1;
    replace.bytes_per_row = 4;
    replace.bytes_per_image = 12;
    status = call_slot( 45, &replace );
    check( !status, "slot45 rejected a 3D depth-one image pitch" );
    fake_texture_is_3d = FALSE;

    data_guest = bias + ARENA_SIZE - 4;
    texture_expected_data = decode_guest( data_guest );
    texture_expected_size = 4;
    texture_expected_row = 16;
    texture_expected_image = 0;
    texture_expected_height = 1;
    memcpy( (void *)texture_expected_data, "data", 4 );
    replace.data.low = data_guest;
    replace.size.height = 1;
    replace.bytes_per_row = 16;
    replace.bytes_per_image = 0;
    status = call_slot( 45, &replace );
    check( !status, "slot45 rejected a pinned single-row nonzero pitch" );

    data_guest = data_base_guest;
    replace.data.low = data_guest;

    memcpy( decode_guest( data_guest ), "data", 4 );
    memset( &update, 0, sizeof(update) );
    update.buffer = 107;
    update.offset = 8;
    update.data = (struct wmt_wire_ptr){data_guest, 0};
    update.length = 4;
    status = call_slot( 107, &update );
    check( !status && buffer_update_calls, "slot107 did not snapshot buffer input" );
    fault_buffer_update = TRUE;
    status = call_slot( 107, &update );
    fault_buffer_update = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && !wmt_test_snapshot_live_bytes(),
        "faulting buffer backend leaked snapshot quota" );
    status = call_slot( 107, &update );
    check( !status, "slot107 did not recover after a guarded backend fault" );
    before = buffer_update_calls;
    update.offset = 13;
    status = call_slot( 107, &update );
    check( status == STATUS_INVALID_PARAMETER && buffer_update_calls == before,
        "slot107 out-of-range update reached the backend" );
    status = call_slot( 75, shader );
    check( !status, "slot75 did not destroy registered shader" );

    air_constant_buffer_count = 0;
    air_argument_count = 0;
    status = call_slot( 74, &initialize );
    check( !status, "slot74 did not register a zero-output shader" );
    get_arguments.shader = *shader;
    get_arguments.constant_buffers = 0;
    get_arguments.arguments = 0;
    before = normal_calls;
    status = call_slot( 88, &get_arguments );
    check( !status && normal_calls == before + 1,
        "slot88 did not accept zero reflection outputs" );
    check( !call_slot( 75, shader ), "slot75 did not destroy a zero-output shader" );

    air_constant_buffer_count = 1;
    status = call_slot( 74, &initialize );
    check( !status, "slot74 did not register a constant-only shader" );
    get_arguments.shader = *shader;
    get_arguments.constant_buffers = constant_guest;
    get_arguments.arguments = 0;
    memset( constant_buffers, 0xaa, sizeof(*constant_buffers) );
    status = call_slot( 88, &get_arguments );
    check( !status && constant_buffers[0].type == 1,
        "slot88 did not publish its one constant-buffer range" );
    check( !call_slot( 75, shader ), "slot75 did not destroy a constant-only shader" );

    air_constant_buffer_count = 0;
    air_argument_count = 2;
    status = call_slot( 74, &initialize );
    check( !status, "slot74 did not register an argument-only shader" );
    get_arguments.shader = *shader;
    get_arguments.constant_buffers = 0;
    get_arguments.arguments = arguments_guest;
    memset( arguments, 0xaa, 2 * sizeof(*arguments) );
    status = call_slot( 88, &get_arguments );
    check( !status && arguments[0].type == 5 && arguments[1].structure_ptr_offset == 12,
        "slot88 did not publish its one argument range" );
    check( !call_slot( 75, shader ), "slot75 did not destroy an argument-only shader" );
    air_constant_buffer_count = 1;

    status = call_slot( 74, &initialize );
    check( !status, "slot74 did not register the shader destroy-fault fixture" );
    before = air_shader_destroy_calls;
    fault_air_shader_destroy = TRUE;
    status = call_slot( 75, shader );
    fault_air_shader_destroy = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && air_shader_destroy_calls == before + 1,
        "faulting shader destroy did not return through the backend guard" );
    status = call_slot( 75, shader );
    check( status == STATUS_INVALID_HANDLE && air_shader_destroy_calls == before + 1,
        "faulting shader destroy republished a potentially deleted handle" );

    status = call_slot( 74, &initialize );
    check( !status, "slot74 did not register the bitcode destroy-fault shader" );
    compile.arguments = common_guest;
    status = call_slot( 76, &compile );
    check( !status && *bitcode == AIR_BITCODE,
        "slot76 did not register the bitcode destroy-fault fixture" );
    memset( compiled, 0, sizeof(*compiled) );
    status = call_slot( 77, &get_bitcode );
    check( !status && compiled->data, "slot77 did not attach destroy-fault dispatch data" );
    before = air_bitcode_destroy_calls;
    release_before = release_object_calls;
    fault_air_bitcode_destroy = TRUE;
    status = call_slot( 78, bitcode );
    fault_air_bitcode_destroy = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && air_bitcode_destroy_calls == before + 1 &&
        release_object_calls == release_before + 1,
        "faulting bitcode destroy did not detach native and dispatch owners once" );
    status = call_slot( 78, bitcode );
    check( status == STATUS_INVALID_HANDLE && air_bitcode_destroy_calls == before + 1,
        "faulting bitcode destroy was retried" );
    check( !call_slot( 75, shader ), "bitcode destroy-fault shader cleanup failed" );

    status = call_slot( 74, &initialize );
    check( !status, "slot74 did not register the error destroy-fault shader" );
    fail_air_compile = TRUE;
    status = call_slot( 76, &compile );
    fail_air_compile = FALSE;
    check( !status && *error == AIR_ERROR,
        "slot76 did not register the error destroy-fault fixture" );
    before = air_error_destroy_calls;
    fault_air_error_destroy = TRUE;
    status = call_slot( 80, error );
    fault_air_error_destroy = FALSE;
    check( status == STATUS_ACCESS_VIOLATION && air_error_destroy_calls == before + 1,
        "faulting error destroy did not return through the backend guard" );
    status = call_slot( 80, error );
    check( status == STATUS_INVALID_HANDLE && air_error_destroy_calls == before + 1,
        "faulting error destroy was retried" );
    check( !call_slot( 75, shader ), "error destroy-fault shader cleanup failed" );
    check( !wmt_test_snapshot_live_bytes(), "AIR destroy-fault fixtures leaked snapshot quota" );
}

static void test_deterministic_denial_outputs(void)
{
    unsigned char outer[136];
    struct wmt_params32_buffer_texture *buffer_texture = (void *)outer;
    struct wmt_params32_capture *capture = (void *)outer;
    struct wmt_params32_info_ret *info_ret = (void *)outer;
    struct wmt_params32_enumerate *enumerate = (void *)outer;
    struct wmt_params32_obj_ptr *obj_ptr = (void *)outer;
    struct wmt_params32_query_display *query = (void *)outer;
    struct wmt_params32_archive *archive = (void *)outer;
    struct wmt_params32_archive_serialize *serialize = (void *)outer;
    struct wmt_params32_cache_get *cache = (void *)outer;
    struct wmt_params32_resolve_counter_range *range = (void *)outer;
    struct wmt_params32_sample_buffer_encoder *sample = (void *)outer;
    struct wmt_texture_info32 *texture;
    struct wmt_display_description32 *description;
    struct wmt_hdr_metadata32 *metadata;
    unsigned char *bytes;
    UINT32 texture_guest, bytes_guest, description_guest, metadata_guest;
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

    bytes_guest = guest_alloc( 32, 8 );
    bytes = decode_guest( bytes_guest );

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
    test_air_and_upload();
    test_denials();
    test_deterministic_denial_outputs();
    test_snapshot_cleanup();
    free( owned_host );
    free( arena );
    if (failures)
    {
        fprintf( stderr, "%u v6 test(s) failed (%s bias)\n", failures, argv[1] );
        return 1;
    }
    printf( "winemetal v6 tests passed (%s bias, %u normal, %u legacy calls)\n",
            argv[1], normal_calls, legacy_calls );
    return 0;
}
