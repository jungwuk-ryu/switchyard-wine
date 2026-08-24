/*
 * Winemetal companion fixed-schema adapters
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

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
# include <mach/mach.h>
#endif

#include "buffer.h"
#include "winemetal_private.h"

static _Atomic uint64_t snapshot_live_bytes;

#ifdef WMT_NATIVE_TEST
extern void wmt_test_before_native_snapshot_copy(void);

uint64_t wmt_test_snapshot_live_bytes(void)
{
    return atomic_load_explicit( &snapshot_live_bytes, memory_order_acquire );
}
#endif

struct wmt_air_shader_record
{
    uint64_t shader;
    uint32_t constant_buffers;
    uint32_t arguments;
    unsigned int references;
    BOOL destroying;
    struct wmt_air_shader_record *next;
};

struct wmt_air_bitcode_record
{
    uint64_t bitcode;
    uint64_t token;
    uint64_t dispatch_data;
    uint64_t size;
    BOOL destroying;
    struct wmt_air_bitcode_record *next;
};

struct wmt_air_error_record
{
    uint64_t error;
    BOOL destroying;
    struct wmt_air_error_record *next;
};

static pthread_mutex_t air_shader_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t air_shader_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t air_bitcode_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t air_error_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct wmt_air_shader_record *air_shaders;
static struct wmt_air_bitcode_record *air_bitcodes;
static struct wmt_air_error_record *air_errors;
static uint64_t air_next_token = 1;

#define WMT_AIR_TOKEN_PREFIX 0x574d540000000000ull
#define WMT_AIR_TOKEN_MAX    0x000000ffffffffffull

static BOOL valid_guest_range( uint64_t guest, uint64_t size )
{
    return guest <= UINT32_MAX && size <= UINT32_MAX + 1ull - guest;
}

static BOOL reserve_snapshot_bytes( uint64_t size )
{
    uint64_t current = atomic_load_explicit( &snapshot_live_bytes, memory_order_relaxed );

    if (size > WMT_SNAPSHOT_LIVE_MAX) return FALSE;
    for (;;)
    {
        if (current > WMT_SNAPSHOT_LIVE_MAX - size) return FALSE;
        if (atomic_compare_exchange_weak_explicit( &snapshot_live_bytes, &current, current + size,
                                                   memory_order_acq_rel, memory_order_relaxed ))
            return TRUE;
    }
}

static void release_snapshot_bytes( uint64_t size )
{
    uint64_t previous;

    previous = atomic_fetch_sub_explicit( &snapshot_live_bytes, size, memory_order_acq_rel );
    if (previous < size) abort();
}

static NTSTATUS copy_from_wire( struct wmt_wire_ptr ptr, void *dst, uint64_t size )
{
    void *host;
    NTSTATUS status;

    if (ptr.high || !valid_guest_range( ptr.low, size )) return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_guest32_to_host( ptr.low, &host ))) return status;
    return ntdll_wow64_copy_from_user( dst, host, size );
}

static NTSTATUS copy_to_wire( struct wmt_wire_ptr ptr, const void *src, uint64_t size )
{
    void *host;
    NTSTATUS status;

    if (ptr.high || !valid_guest_range( ptr.low, size )) return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_guest32_to_host( ptr.low, &host ))) return status;
    return ntdll_wow64_atomic_write_user( host, src, size );
}

static NTSTATUS validate_wire( struct wmt_wire_ptr ptr, uint64_t size, uint32_t access )
{
    void *host;
    NTSTATUS status;

    if (ptr.high || !valid_guest_range( ptr.low, size )) return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_guest32_to_host( ptr.low, &host ))) return status;
    if (access & WINE_WOW64_UNIXLIB_ACCESS_READ)
        if ((status = ntdll_wow64_probe_user_read( host, size ))) return status;
    if (access & WINE_WOW64_UNIXLIB_ACCESS_WRITE)
        if ((status = ntdll_wow64_probe_user_write( host, size ))) return status;
    if (access & ~(WINE_WOW64_UNIXLIB_ACCESS_READ | WINE_WOW64_UNIXLIB_ACCESS_WRITE))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS copy_to_wire_offset( struct wmt_wire_ptr ptr, size_t offset,
                                     const void *src, size_t size )
{
    struct wmt_wire_ptr field = ptr;

    if (field.high || offset > UINT32_MAX - field.low) return STATUS_INVALID_PARAMETER;
    field.low += offset;
    return copy_to_wire( field, src, size );
}

static NTSTATUS publish_air_argument_outputs( uint32_t constant_guest, const void *constant_data,
                                              uint64_t constant_size, uint32_t argument_guest,
                                              const void *argument_data, uint64_t argument_size )
{
    struct ntdll_wow64_user_write_range ranges[2];
    ULONG count = 0;
    NTSTATUS status;

    if (constant_size)
    {
        if (!valid_guest_range( constant_guest, constant_size ) || constant_size > SIZE_MAX)
            return STATUS_INVALID_PARAMETER;
        if ((status = ntdll_wow64_guest32_to_host( constant_guest, &ranges[count].dst ))) return status;
        ranges[count].src = constant_data;
        ranges[count++].size = constant_size;
    }
    if (argument_size)
    {
        if (!valid_guest_range( argument_guest, argument_size ) || argument_size > SIZE_MAX)
            return STATUS_INVALID_PARAMETER;
        if ((status = ntdll_wow64_guest32_to_host( argument_guest, &ranges[count].dst ))) return status;
        ranges[count].src = argument_data;
        ranges[count++].size = argument_size;
    }
    if (!count) return STATUS_SUCCESS;
    return ntdll_wow64_atomic_writev( ranges, count );
}

static NTSTATUS zero_guest_range( uint32_t guest, uint64_t size, uint64_t maximum )
{
    static const unsigned char zero[4096];
    struct wmt_wire_ptr ptr = {guest, 0};
    uint64_t offset = 0;
    size_t chunk;
    NTSTATUS status;

    if (!size) return STATUS_SUCCESS;
    if (!guest || size > maximum || !valid_guest_range( guest, size ))
        return STATUS_INVALID_PARAMETER;
    if ((status = validate_wire( ptr, size, WINE_WOW64_UNIXLIB_ACCESS_WRITE ))) return status;
    while (offset < size)
    {
        chunk = sizeof(zero);
        if (chunk > size - offset) chunk = size - offset;
        ptr.low = guest + offset;
        if ((status = copy_to_wire( ptr, zero, chunk ))) return status;
        offset += chunk;
    }
    return STATUS_SUCCESS;
}

NTSTATUS wmt_snapshot_bytes( uint64_t guest, uint64_t size, void **ret )
{
    void *host;
    NTSTATUS status;
    void *buffer;

    *ret = NULL;
    if (!size) return STATUS_SUCCESS;
    if (!valid_guest_range( guest, size ) || size > SIZE_MAX) return STATUS_INVALID_PARAMETER;
    if (!reserve_snapshot_bytes( size )) return STATUS_NO_MEMORY;
    if (!(buffer = malloc( size )))
    {
        release_snapshot_bytes( size );
        return STATUS_NO_MEMORY;
    }
    if ((status = ntdll_wow64_guest32_to_host( guest, &host )) ||
        (status = ntdll_wow64_copy_from_user( buffer, host, size )))
    {
        free( buffer );
        release_snapshot_bytes( size );
        return status;
    }
    *ret = buffer;
    return STATUS_SUCCESS;
}

NTSTATUS wmt_snapshot_alloc( uint64_t size, void **ret )
{
    void *buffer;

    *ret = NULL;
    if (!size) return STATUS_SUCCESS;
    if (size > SIZE_MAX) return STATUS_INVALID_PARAMETER;
    if (!reserve_snapshot_bytes( size )) return STATUS_NO_MEMORY;
    if (!(buffer = malloc( size )))
    {
        release_snapshot_bytes( size );
        return STATUS_NO_MEMORY;
    }
    *ret = buffer;
    return STATUS_SUCCESS;
}

NTSTATUS wmt_snapshot_realloc( void *ptr, uint64_t old_size, uint64_t new_size, void **ret )
{
    void *buffer;

    *ret = ptr;
    if (new_size < old_size || new_size > SIZE_MAX) return STATUS_INVALID_PARAMETER;
    if (new_size == old_size) return STATUS_SUCCESS;
    if (!reserve_snapshot_bytes( new_size - old_size )) return STATUS_NO_MEMORY;
    if (!(buffer = realloc( ptr, new_size )))
    {
        release_snapshot_bytes( new_size - old_size );
        return STATUS_NO_MEMORY;
    }
    *ret = buffer;
    return STATUS_SUCCESS;
}

void wmt_snapshot_free( void *ptr, uint64_t size )
{
    if (!ptr) return;
    free( ptr );
    release_snapshot_bytes( size );
}

static NTSTATUS snapshot_native_bytes( void *dst, const void *src, size_t size )
{
    NTSTATUS status = STATUS_ACCESS_VIOLATION;

    __TRY
    {
#ifdef WMT_NATIVE_TEST
        wmt_test_before_native_snapshot_copy();
#endif
        memcpy( dst, src, size );
        status = STATUS_SUCCESS;
    }
    __EXCEPT
    {
    }
    __ENDTRY
    return status;
}

NTSTATUS wmt_snapshot_cstring( uint64_t guest, size_t max_length, char **ret )
{
    size_t chunk, limit, offset = 0;
    NTSTATUS status;
    void *host;
    char *buffer;

    *ret = NULL;
    if (!guest || guest > UINT32_MAX || !max_length) return STATUS_INVALID_PARAMETER;
    limit = max_length;
    if (limit > UINT32_MAX + 1ull - guest) limit = UINT32_MAX + 1ull - guest;
    if (!reserve_snapshot_bytes( max_length )) return STATUS_NO_MEMORY;
    if (!(buffer = malloc( max_length )))
    {
        release_snapshot_bytes( max_length );
        return STATUS_NO_MEMORY;
    }

    while (offset < limit)
    {
        chunk = 0x1000 - ((guest + offset) & 0xfff);
        if (chunk > limit - offset) chunk = limit - offset;
        if ((status = ntdll_wow64_guest32_to_host( guest + offset, &host )) ||
            (status = ntdll_wow64_copy_from_user( buffer + offset, host, chunk )))
        {
            wmt_snapshot_free( buffer, max_length );
            return status;
        }
        if (memchr( buffer + offset, 0, chunk ))
        {
            *ret = buffer;
            return STATUS_SUCCESS;
        }
        offset += chunk;
    }

    wmt_snapshot_free( buffer, max_length );
    return STATUS_BUFFER_OVERFLOW;
}

void wmt_buffer_info32_to_native( const struct wmt_buffer_info32 *src,
                                  struct wmt_buffer_info *dst )
{
    dst->length = src->length;
    dst->options = src->options;
    dst->memory.ptr = NULL;
    dst->gpu_address = 0;
}

static NTSTATUS release_native_object( uint64_t object )
{
    if (!object) return STATUS_SUCCESS;
    return wmt_normal_call( 1, &object );
}

static NTSTATUS release_native_object_on_failure( uint64_t object, NTSTATUS status )
{
    NTSTATUS release_status;

    if (!object) return status;
    release_status = release_native_object( object );
    return release_status ? release_status : status;
}

static void sampler_info32_to_native( const struct wmt_sampler_info32 *src,
                                      struct wmt_sampler_info *dst )
{
    dst->min_filter = src->min_filter;
    dst->mag_filter = src->mag_filter;
    dst->mip_filter = src->mip_filter;
    dst->r_address_mode = src->r_address_mode;
    dst->s_address_mode = src->s_address_mode;
    dst->t_address_mode = src->t_address_mode;
    dst->border_color = src->border_color;
    dst->compare_function = src->compare_function;
    dst->lod_min_clamp = src->lod_min_clamp;
    dst->lod_max_clamp = src->lod_max_clamp;
    dst->max_anisotropy = src->max_anisotropy;
    dst->normalized_coords = src->normalized_coords;
    dst->lod_average = src->lod_average;
    dst->support_argument_buffers = src->support_argument_buffers;
    dst->reserved = 0;
    dst->gpu_resource_id = 0;
}

static void stencil_info32_to_native( const struct wmt_stencil_info32 *src,
                                      struct wmt_stencil_info *dst )
{
    dst->enabled = src->enabled;
    dst->depth_stencil_pass_op = src->depth_stencil_pass_op;
    dst->stencil_fail_op = src->stencil_fail_op;
    dst->depth_fail_op = src->depth_fail_op;
    dst->stencil_compare_function = src->stencil_compare_function;
    dst->write_mask = src->write_mask;
    dst->read_mask = src->read_mask;
}

static void depth_info32_to_native( const struct wmt_depth_stencil_info32 *src,
                                    struct wmt_depth_stencil_info *dst )
{
    dst->depth_compare_function = src->depth_compare_function;
    dst->depth_write_enabled = src->depth_write_enabled;
    stencil_info32_to_native( &src->front_stencil, &dst->front_stencil );
    stencil_info32_to_native( &src->back_stencil, &dst->back_stencil );
}

static void texture_info32_to_native( const struct wmt_texture_info32 *src,
                                      struct wmt_texture_info *dst )
{
    dst->pixel_format = src->pixel_format;
    dst->width = src->width;
    dst->height = src->height;
    dst->depth = src->depth;
    dst->array_length = src->array_length;
    dst->type_levels_samples_usage = src->type_levels_samples_usage;
    dst->options = src->options;
    dst->reserved = src->reserved;
    dst->mach_port = src->mach_port;
    dst->gpu_resource_id = 0;
}

static NTSTATUS snapshot_handle_array( struct wmt_wire_ptr ptr, uint64_t count,
                                       uint64_t **ret, uint64_t *ret_size )
{
    uint64_t size;

    *ret = NULL;
    *ret_size = 0;
    if (!count) return STATUS_SUCCESS;
    if (count > SIZE_MAX / sizeof(uint64_t)) return STATUS_INVALID_PARAMETER;
    size = count * sizeof(uint64_t);
    if (ptr.high) return STATUS_INVALID_PARAMETER;
    *ret_size = size;
    return wmt_snapshot_bytes( ptr.low, size, (void **)ret );
}

static void color_attachment32_to_native( const struct wmt_color_attachment_info32 *src,
                                          struct wmt_color_attachment_info *dst )
{
    dst->texture = src->texture;
    dst->load_action = src->load_action;
    dst->store_action = src->store_action;
    dst->level = src->level;
    dst->slice = src->slice;
    dst->depth_plane = src->depth_plane;
    dst->clear_color.r = src->clear_color.r;
    dst->clear_color.g = src->clear_color.g;
    dst->clear_color.b = src->clear_color.b;
    dst->clear_color.a = src->clear_color.a;
    dst->resolve_texture = src->resolve_texture;
    dst->resolve_level = src->resolve_level;
    dst->resolve_slice = src->resolve_slice;
    dst->resolve_depth_plane = src->resolve_depth_plane;
}

static void render_pass32_to_native( const struct wmt_render_pass_info32 *src,
                                     struct wmt_render_pass_info *dst )
{
    unsigned int i;

    memset( dst, 0, sizeof(*dst) );
    for (i = 0; i < 8; ++i) color_attachment32_to_native( &src->colors[i], &dst->colors[i] );
    dst->depth.texture = src->depth.texture;
    dst->depth.load_action = src->depth.load_action;
    dst->depth.store_action = src->depth.store_action;
    dst->depth.level = src->depth.level;
    dst->depth.slice = src->depth.slice;
    dst->depth.depth_plane = src->depth.depth_plane;
    dst->depth.clear_depth = src->depth.clear_depth;
    dst->stencil.texture = src->stencil.texture;
    dst->stencil.load_action = src->stencil.load_action;
    dst->stencil.store_action = src->stencil.store_action;
    dst->stencil.level = src->stencil.level;
    dst->stencil.slice = src->stencil.slice;
    dst->stencil.depth_plane = src->stencil.depth_plane;
    dst->stencil.clear_stencil = src->stencil.clear_stencil;
    dst->default_raster_sample_count = src->default_raster_sample_count;
    dst->render_target_array_length = src->render_target_array_length;
    dst->tile_width = src->tile_width;
    dst->tile_height = src->tile_height;
    dst->render_target_height = src->render_target_height;
    dst->render_target_width = src->render_target_width;
    dst->visibility_buffer = src->visibility_buffer;
}

static void blend_info32_to_native( const struct wmt_color_attachment_blend_info32 *src,
                                    struct wmt_color_attachment_blend_info *dst )
{
    dst->pixel_format = src->pixel_format;
    dst->rgb_blend_operation = src->rgb_blend_operation;
    dst->alpha_blend_operation = src->alpha_blend_operation;
    dst->src_rgb_blend_factor = src->src_rgb_blend_factor;
    dst->dst_rgb_blend_factor = src->dst_rgb_blend_factor;
    dst->src_alpha_blend_factor = src->src_alpha_blend_factor;
    dst->dst_alpha_blend_factor = src->dst_alpha_blend_factor;
    dst->write_mask = src->write_mask;
    dst->blending_enabled = src->blending_enabled;
}

static void render_pipeline32_to_native( const struct wmt_render_pipeline_info32 *src,
                                         struct wmt_render_pipeline_info *dst )
{
    unsigned int i;

    memset( dst, 0, sizeof(*dst) );
    for (i = 0; i < 8; ++i) blend_info32_to_native( &src->colors[i], &dst->colors[i] );
    dst->alpha_to_coverage_enabled = src->alpha_to_coverage_enabled;
    dst->logic_operation_enabled = src->logic_operation_enabled;
    dst->logic_operation = src->logic_operation;
    dst->rasterization_enabled = src->rasterization_enabled;
    dst->raster_sample_count = src->raster_sample_count;
    dst->depth_pixel_format = src->depth_pixel_format;
    dst->stencil_pixel_format = src->stencil_pixel_format;
    dst->vertex_function = src->vertex_function;
    dst->fragment_function = src->fragment_function;
    dst->immutable_vertex_buffers = src->immutable_vertex_buffers;
    dst->immutable_fragment_buffers = src->immutable_fragment_buffers;
    dst->input_primitive_topology = src->input_primitive_topology;
    dst->tessellation_partition_mode = src->tessellation_partition_mode;
    dst->max_tessellation_factor = src->max_tessellation_factor;
    dst->tessellation_output_winding_order = src->tessellation_output_winding_order;
    dst->tessellation_factor_step = src->tessellation_factor_step;
    dst->binary_archive_for_serialization = src->binary_archive_for_serialization;
    dst->num_binary_archives_for_lookup = src->num_binary_archives_for_lookup;
    dst->fail_on_binary_archive_miss = src->fail_on_binary_archive_miss;
}

static void layer_props32_to_native( const struct wmt_layer_props32 *src,
                                     struct wmt_layer_props *dst )
{
    dst->device = src->device;
    dst->contents_scale = src->contents_scale;
    dst->drawable_width = src->drawable_width;
    dst->drawable_height = src->drawable_height;
    dst->opaque = src->opaque;
    dst->display_sync_enabled = src->display_sync_enabled;
    dst->framebuffer_only = src->framebuffer_only;
    dst->pixel_format = src->pixel_format;
}

static void layer_props_native_to32( const struct wmt_layer_props *src,
                                     struct wmt_layer_props32 *dst )
{
    memset( dst, 0, sizeof(*dst) );
    dst->device = src->device;
    dst->contents_scale = src->contents_scale;
    dst->drawable_width = src->drawable_width;
    dst->drawable_height = src->drawable_height;
    dst->opaque = src->opaque;
    dst->display_sync_enabled = src->display_sync_enabled;
    dst->framebuffer_only = src->framebuffer_only;
    dst->pixel_format = src->pixel_format;
}

static unsigned int constant_data_size( uint16_t type )
{
    static const uint8_t float_components[] =
    {
        1, 2, 3, 4, 4, 6, 8, 6, 9, 12, 8, 12, 16
    };

    if (type >= 3 && type <= 15) return float_components[type - 3] * 4;
    if (type >= 16 && type <= 28) return float_components[type - 16] * 2;
    if (type >= 29 && type <= 36) return ((type - 29) % 4 + 1) * 4;
    if (type >= 37 && type <= 44) return ((type - 37) % 4 + 1) * 2;
    if (type >= 45 && type <= 56) return (type - 45) % 4 + 1;
    return 0;
}

NTSTATUS wmt_call_19( void *args )
{
    struct wmt_params32_info_ret wire;
    struct wmt_sampler_info32 info32;
    struct wmt_sampler_info info;
    struct wmt_params_info_ret native;
    NTSTATUS status;
    uint64_t zero = 0;

    memcpy( &wire, args, sizeof(wire) );
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret), &zero, sizeof(zero) );
    if ((status = copy_to_wire_offset( wire.info, offsetof(struct wmt_sampler_info32, gpu_resource_id),
                                       &zero, sizeof(zero) )))
        return status;
    if ((status = copy_from_wire( wire.info, &info32, sizeof(info32) ))) return status;
    sampler_info32_to_native( &info32, &info );
    native.object = wire.object;
    native.info.ptr = &info;
    native.ret = 0;
    status = wmt_normal_call( WMT_CALL_NEW_SAMPLER, &native );
    if (status) return release_native_object_on_failure( native.ret, status );
    if ((status = copy_to_wire_offset( wire.info, offsetof(struct wmt_sampler_info32, gpu_resource_id),
                                       &info.gpu_resource_id, sizeof(info.gpu_resource_id) )))
    {
        release_native_object( native.ret );
        return status;
    }
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret), &native.ret, sizeof(native.ret) );
    return STATUS_SUCCESS;
}

NTSTATUS wmt_call_20( void *args )
{
    struct wmt_params32_info_ret wire;
    struct wmt_depth_stencil_info32 info32;
    struct wmt_depth_stencil_info info;
    struct wmt_params_const_info_ret native;
    NTSTATUS status;
    uint64_t zero = 0;

    memcpy( &wire, args, sizeof(wire) );
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret), &zero, sizeof(zero) );
    if ((status = copy_from_wire( wire.info, &info32, sizeof(info32) ))) return status;
    depth_info32_to_native( &info32, &info );
    native.object = wire.object;
    native.info.ptr = &info;
    native.ret = 0;
    status = wmt_normal_call( WMT_CALL_NEW_DEPTH_STENCIL, &native );
    if (status) return release_native_object_on_failure( native.ret, status );
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret),
                         &native.ret, sizeof(native.ret) );
    return status;
}

NTSTATUS wmt_call_21( void *args )
{
    struct wmt_params32_info_ret wire;
    struct wmt_texture_info32 info32;
    struct wmt_texture_info info;
    struct wmt_params_info_ret native;
    NTSTATUS status;
    uint64_t zero = 0;

    memcpy( &wire, args, sizeof(wire) );
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret), &zero, sizeof(zero) );
    if ((status = copy_to_wire_offset( wire.info, offsetof(struct wmt_texture_info32, gpu_resource_id),
                                       &zero, sizeof(zero) )))
        return status;
    if ((status = copy_from_wire( wire.info, &info32, sizeof(info32) ))) return status;
    texture_info32_to_native( &info32, &info );
    native.object = wire.object;
    native.info.ptr = &info;
    native.ret = 0;
    status = wmt_normal_call( WMT_CALL_NEW_TEXTURE, &native );
    if (status) return release_native_object_on_failure( native.ret, status );
    if ((status = copy_to_wire_offset( wire.info, offsetof(struct wmt_texture_info32, gpu_resource_id),
                                       &info.gpu_resource_id, sizeof(info.gpu_resource_id) )))
    {
        release_native_object( native.ret );
        return status;
    }
    if ((status = copy_to_wire_offset( wire.info, offsetof(struct wmt_texture_info32, mach_port),
                                       &info.mach_port, sizeof(info.mach_port) )))
    {
        copy_to_wire_offset( wire.info, offsetof(struct wmt_texture_info32, gpu_resource_id),
                             &zero, sizeof(zero) );
        release_native_object( native.ret );
        return status;
    }
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret), &native.ret, sizeof(native.ret) );
    return STATUS_SUCCESS;
}

NTSTATUS wmt_call_26( void *args )
{
    struct wmt_params32_obj_u64_obj_ret wire;
    struct wmt_params_obj_u64_obj_ret native;
    NTSTATUS status;
    char *name;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret = 0;
    memcpy( args, &wire, sizeof(wire) );
    if (wire.arg > UINT32_MAX) return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_cstring( wire.arg, WMT_STRING_MAX + 1u, &name ))) return status;
    native.handle = wire.handle;
    native.arg = (uintptr_t)name;
    native.ret = 0;
    status = wmt_normal_call( WMT_CALL_LIBRARY_NEW_FUNCTION, &native );
    wmt_snapshot_free( name, WMT_STRING_MAX + 1u );
    if (status) return release_native_object_on_failure( native.ret, status );
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_obj_u64_obj_ret, ret),
                         &native.ret, sizeof(native.ret) );
    return status;
}

NTSTATUS wmt_call_29( void *args )
{
    struct wmt_params32_pipeline wire;
    struct wmt_compute_pipeline_info32 info32;
    struct wmt_compute_pipeline_info info;
    struct wmt_params_pipeline native;
    uint64_t *archives = NULL, archive_size = 0;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret_error = wire.ret_pso = 0;
    memcpy( args, &wire, sizeof(wire) );
    if ((status = copy_from_wire( wire.info, &info32, sizeof(info32) ))) return status;
    memset( &info, 0, sizeof(info) );
    info.compute_function = info32.compute_function;
    info.binary_archive_for_serialization = info32.binary_archive_for_serialization;
    info.num_binary_archives_for_lookup = info32.num_binary_archives_for_lookup;
    info.fail_on_binary_archive_miss = info32.fail_on_binary_archive_miss;
    info.padding = info32.padding;
    info.tgsize_is_multiple_of_sgwidth = info32.tgsize_is_multiple_of_sgwidth;
    info.immutable_buffers = info32.immutable_buffers;
    if ((status = snapshot_handle_array( info32.binary_archives_for_lookup,
                                         info32.num_binary_archives_for_lookup,
                                         &archives, &archive_size )))
        return status;
    info.binary_archives_for_lookup.ptr = archives;
    native.device = wire.device;
    native.info.ptr = &info;
    native.ret_error = native.ret_pso = 0;
    status = wmt_normal_call( WMT_CALL_NEW_COMPUTE_PSO, &native );
    wmt_snapshot_free( archives, archive_size );
    if (status) return release_native_object_on_failure( native.ret_pso, status );
    if (!status)
    {
        memcpy( (char *)args + offsetof(struct wmt_params32_pipeline, ret_error),
                &native.ret_error, sizeof(native.ret_error) );
        memcpy( (char *)args + offsetof(struct wmt_params32_pipeline, ret_pso),
                &native.ret_pso, sizeof(native.ret_pso) );
    }
    return status;
}

NTSTATUS wmt_call_32( void *args )
{
    struct wmt_params32_obj_u64_obj_ret wire;
    struct wmt_render_pass_info32 info32;
    struct wmt_render_pass_info info;
    struct wmt_params_obj_u64_obj_ret native;
    struct wmt_wire_ptr info_ptr;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret = 0;
    memcpy( args, &wire, sizeof(wire) );
    if (wire.arg > UINT32_MAX) return STATUS_INVALID_PARAMETER;
    info_ptr.low = wire.arg;
    info_ptr.high = 0;
    if ((status = copy_from_wire( info_ptr, &info32, sizeof(info32) ))) return status;
    render_pass32_to_native( &info32, &info );
    native.handle = wire.handle;
    native.arg = (uintptr_t)&info;
    native.ret = 0;
    status = wmt_normal_call( WMT_CALL_RENDER_ENCODER, &native );
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_obj_u64_obj_ret, ret),
                         &native.ret, sizeof(native.ret) );
    return status;
}

NTSTATUS wmt_call_34( void *args )
{
    struct wmt_params32_pipeline wire;
    struct wmt_render_pipeline_info32 info32;
    struct wmt_render_pipeline_info info;
    struct wmt_params_pipeline native;
    uint64_t *archives = NULL, archive_size = 0;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret_error = wire.ret_pso = 0;
    memcpy( args, &wire, sizeof(wire) );
    if ((status = copy_from_wire( wire.info, &info32, sizeof(info32) ))) return status;
    render_pipeline32_to_native( &info32, &info );
    if ((status = snapshot_handle_array( info32.binary_archives_for_lookup,
                                         info32.num_binary_archives_for_lookup,
                                         &archives, &archive_size )))
        return status;
    info.binary_archives_for_lookup.ptr = archives;
    native.device = wire.device;
    native.info.ptr = &info;
    native.ret_error = native.ret_pso = 0;
    status = wmt_normal_call( WMT_CALL_NEW_RENDER_PSO, &native );
    wmt_snapshot_free( archives, archive_size );
    if (status) return release_native_object_on_failure( native.ret_pso, status );
    if (!status)
    {
        memcpy( (char *)args + offsetof(struct wmt_params32_pipeline, ret_error),
                &native.ret_error, sizeof(native.ret_error) );
        memcpy( (char *)args + offsetof(struct wmt_params32_pipeline, ret_pso),
                &native.ret_pso, sizeof(native.ret_pso) );
    }
    return status;
}

static NTSTATUS call_string( unsigned int index, void *args )
{
    struct wmt_params32_string wire;
    struct wmt_params_string native;
    NTSTATUS status;
    char *string;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret = 0;
    memcpy( args, &wire, sizeof(wire) );
    if (wire.buffer.high) return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_cstring( wire.buffer.low, WMT_STRING_MAX + 1u, &string ))) return status;
    native.buffer.ptr = string;
    native.encoding = wire.encoding;
    native.ret = 0;
    status = wmt_normal_call( index, &native );
    wmt_snapshot_free( string, WMT_STRING_MAX + 1u );
    if (status && index == WMT_CALL_NSSTRING_ALLOC_INIT)
        return release_native_object_on_failure( native.ret, status );
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_string, ret),
                         &native.ret, sizeof(native.ret) );
    return status;
}

NTSTATUS wmt_call_60( void *args )
{
    return call_string( WMT_CALL_NSSTRING_STRING, args );
}

NTSTATUS wmt_call_61( void *args )
{
    return call_string( WMT_CALL_NSSTRING_ALLOC_INIT, args );
}

NTSTATUS wmt_call_70( void *args )
{
    struct wmt_params32_obj_ptr wire;
    struct wmt_layer_props32 props32;
    struct wmt_layer_props props;
    struct wmt_params_obj_const_ptr native;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    if ((status = copy_from_wire( wire.arg, &props32, sizeof(props32) ))) return status;
    layer_props32_to_native( &props32, &props );
    native.handle = wire.handle;
    native.arg.ptr = &props;
    return wmt_normal_call( WMT_CALL_LAYER_SET_PROPS, &native );
}

NTSTATUS wmt_call_71( void *args )
{
    struct wmt_params32_obj_ptr wire;
    struct wmt_layer_props32 props32;
    struct wmt_layer_props props;
    struct wmt_params_obj_ptr native;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    memset( &props32, 0, sizeof(props32) );
    if ((status = validate_wire( wire.arg, sizeof(props32), WINE_WOW64_UNIXLIB_ACCESS_WRITE ))) return status;
    if ((status = copy_to_wire( wire.arg, &props32, sizeof(props32) ))) return status;
    memset( &props, 0, sizeof(props) );
    native.handle = wire.handle;
    native.arg.ptr = &props;
    if ((status = wmt_normal_call( WMT_CALL_LAYER_GET_PROPS, &native ))) return status;
    layer_props_native_to32( &props, &props32 );
    return copy_to_wire( wire.arg, &props32, sizeof(props32) );
}

NTSTATUS wmt_call_97( void *args )
{
    struct wmt_params32_obj_ptr wire;
    struct wmt_edr_value32 value32;
    struct wmt_edr_value value;
    struct wmt_params_obj_ptr native;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    memset( &value32, 0, sizeof(value32) );
    if ((status = validate_wire( wire.arg, sizeof(value32), WINE_WOW64_UNIXLIB_ACCESS_WRITE ))) return status;
    if ((status = copy_to_wire( wire.arg, &value32, sizeof(value32) ))) return status;
    memset( &value, 0, sizeof(value) );
    native.handle = wire.handle;
    native.arg.ptr = &value;
    if ((status = wmt_normal_call( WMT_CALL_LAYER_EDR, &native ))) return status;
    value32.maximum_edr_color_component_value = value.maximum_edr_color_component_value;
    value32.maximum_potential_edr_color_component_value = value.maximum_potential_edr_color_component_value;
    return copy_to_wire( wire.arg, &value32, sizeof(value32) );
}

NTSTATUS wmt_call_98( void *args )
{
    struct wmt_params32_function_constants wire;
    struct wmt_params_function_constants native;
    struct wmt_function_constant32 *constants32 = NULL;
    struct wmt_function_constant *constants = NULL;
    uint8_t *data = NULL;
    uint64_t constants_size = 0, data_size = 0, offset = 0;
    NTSTATUS status = STATUS_SUCCESS;
    char *name = NULL;
    unsigned int size;
    uint64_t i;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret = wire.ret_error = 0;
    memcpy( args, &wire, sizeof(wire) );
    if (wire.name.high || wire.constants.high || wire.count > WMT_CONSTANT_COUNT_MAX)
        return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_cstring( wire.name.low, WMT_STRING_MAX + 1u, &name ))) return status;
    if (wire.count > SIZE_MAX / sizeof(*constants32))
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    constants_size = wire.count * sizeof(*constants32);
    if (constants_size)
    {
        status = wmt_snapshot_bytes( wire.constants.low, constants_size, (void **)&constants32 );
        if (status) goto done;
    }
    if (wire.count && !constants32)
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }
    for (i = 0; i < wire.count; ++i)
    {
        size = constant_data_size( constants32[i].type );
        if (!size || constants32[i].data.high || data_size > WMT_CONSTANT_BYTE_MAX - size)
        {
            status = STATUS_NOT_SUPPORTED;
            goto done;
        }
        data_size += size;
    }
    if (wire.count)
    {
        if ((status = wmt_snapshot_alloc( wire.count * sizeof(*constants), (void **)&constants )))
            goto done;
        memset( constants, 0, wire.count * sizeof(*constants) );
    }
    if (data_size)
    {
        if (!reserve_snapshot_bytes( data_size ))
        {
            status = STATUS_NO_MEMORY;
            goto done;
        }
        if (!(data = malloc( data_size )))
        {
            release_snapshot_bytes( data_size );
            data_size = 0;
            status = STATUS_NO_MEMORY;
            goto done;
        }
    }
    for (i = 0; i < wire.count; ++i)
    {
        size = constant_data_size( constants32[i].type );
        if ((status = copy_from_wire( constants32[i].data, data + offset, size )))
            goto done;
        constants[i].data.ptr = data + offset;
        constants[i].type = constants32[i].type;
        constants[i].index = constants32[i].index;
        constants[i].reserved = constants32[i].reserved;
        offset += size;
    }
    native.library = wire.library;
    native.name.ptr = name;
    native.constants.ptr = constants;
    native.count = wire.count;
    native.ret = native.ret_error = 0;
    status = wmt_normal_call( WMT_CALL_FUNCTION_CONSTANTS, &native );
    if (status && native.ret)
        status = release_native_object_on_failure( native.ret, status );
    if (!status)
    {
        memcpy( (char *)args + offsetof(struct wmt_params32_function_constants, ret),
                &native.ret, sizeof(native.ret) );
        memcpy( (char *)args + offsetof(struct wmt_params32_function_constants, ret_error),
                &native.ret_error, sizeof(native.ret_error) );
    }

done:
    if (data) wmt_snapshot_free( data, data_size );
    wmt_snapshot_free( constants, wire.count * sizeof(*constants) );
    wmt_snapshot_free( constants32, constants_size );
    wmt_snapshot_free( name, WMT_STRING_MAX + 1u );
    return status;
}

NTSTATUS wmt_call_101( void *args )
{
    struct wmt_params32_query_display_layer wire;
    struct wmt_params_query_display_layer native;
    struct wmt_hdr_metadata32 metadata32;
    struct wmt_hdr_metadata metadata;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    wire.version = wire.colorspace = 0;
    memset( &wire.edr_value, 0, sizeof(wire.edr_value) );
    memcpy( args, &wire, sizeof(wire) );
    memset( &metadata32, 0, sizeof(metadata32) );
    if ((status = validate_wire( wire.hdr_metadata, sizeof(metadata32),
                                 WINE_WOW64_UNIXLIB_ACCESS_WRITE )))
        return status;
    if ((status = copy_to_wire( wire.hdr_metadata, &metadata32, sizeof(metadata32) ))) return status;
    memset( &metadata, 0, sizeof(metadata) );
    memset( &native, 0, sizeof(native) );
    native.layer = wire.layer;
    native.hdr_metadata.ptr = &metadata;
    if ((status = wmt_normal_call( WMT_CALL_QUERY_DISPLAY_LAYER, &native ))) return status;
    memcpy( &metadata32, &metadata, sizeof(metadata32) );
    if ((status = copy_to_wire( wire.hdr_metadata, &metadata32, sizeof(metadata32) ))) return status;
    memcpy( (char *)args + offsetof(struct wmt_params32_query_display_layer, version),
            &native.version, sizeof(native.version) );
    memcpy( (char *)args + offsetof(struct wmt_params32_query_display_layer, colorspace),
            &native.colorspace, sizeof(native.colorspace) );
    memcpy( (char *)args + offsetof(struct wmt_params32_query_display_layer, edr_value),
            &native.edr_value, sizeof(native.edr_value) );
    return STATUS_SUCCESS;
}

NTSTATUS wmt_call_45( void *args )
{
    struct wmt_params32_texture_replace wire;
    struct wmt_params_texture_replace native;
    uint64_t image_bytes, row_bytes, rows, size;
    void *data = NULL;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    if (wire.data.high || !wire.data.low || !wire.texture || !wire.bytes_per_row)
        return STATUS_INVALID_PARAMETER;
    if ((status = wmt_metal_texture_snapshot_rows( wire.texture, wire.origin.x,
                                                   wire.origin.y, wire.origin.z,
                                                   wire.size.width, wire.size.height,
                                                   wire.size.depth, wire.level, wire.slice,
                                                   wire.bytes_per_row, wire.bytes_per_image,
                                                   &rows, &row_bytes )))
        return status;
    if (wire.bytes_per_row < row_bytes) return STATUS_INVALID_PARAMETER;
    if (rows - 1 > (UINT64_MAX - row_bytes) / wire.bytes_per_row)
        return STATUS_INVALID_PARAMETER;
    image_bytes = (rows - 1) * wire.bytes_per_row + row_bytes;
    if (wire.size.depth > 1)
    {
        if (wire.bytes_per_image < image_bytes || wire.size.depth - 1 >
            (UINT64_MAX - image_bytes) / wire.bytes_per_image)
            return STATUS_INVALID_PARAMETER;
        size = (wire.size.depth - 1) * wire.bytes_per_image + image_bytes;
    }
    else size = image_bytes;
    if (!size || size > WMT_DISPATCH_BLOB_MAX) return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_bytes( wire.data.low, size, &data ))) return status;
    native.texture = wire.texture;
    native.origin = wire.origin;
    native.size = wire.size;
    native.level = wire.level;
    native.slice = wire.slice;
    native.data.ptr = data;
    native.bytes_per_row = wire.bytes_per_row;
    native.bytes_per_image = wire.bytes_per_image;
    status = wmt_normal_call( 45, &native );
    wmt_snapshot_free( data, size );
    return status;
}

NTSTATUS wmt_call_107( void *args )
{
    struct wmt_params32_buffer_update wire;
    struct wmt_params_buffer_update native;
    void *data = NULL;
    uint64_t buffer_length;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    if (wire.data.high || !wire.data.low || !wire.buffer || !wire.length ||
        wire.length > WMT_DISPATCH_BLOB_MAX || wire.offset > UINT64_MAX - wire.length)
        return STATUS_INVALID_PARAMETER;
    if ((status = wmt_metal_buffer_length( wire.buffer, &buffer_length ))) return status;
    if (wire.offset + wire.length > buffer_length) return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_bytes( wire.data.low, wire.length, &data ))) return status;
    native.buffer = wire.buffer;
    native.offset = wire.offset;
    native.data.ptr = data;
    native.length = wire.length;
    status = wmt_normal_call( 107, &native );
    wmt_snapshot_free( data, wire.length );
    return status;
}

NTSTATUS wmt_call_114( void *args )
{
    struct wmt_params32_obj_u64_obj_ret wire;
    struct wmt_air_bitcode_record *record;
    uint64_t token, retained;
    NTSTATUS status;
    void *bytes = NULL;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret = 0;
    memcpy( args, &wire, sizeof(wire) );
    if (wire.arg > WMT_DISPATCH_BLOB_MAX) return STATUS_INVALID_PARAMETER;
    if (wire.handle > UINT32_MAX)
    {
        pthread_mutex_lock( &air_bitcode_mutex );
        for (record = air_bitcodes; record && record->token != wire.handle; record = record->next);
        if (!record || record->destroying || record->size != wire.arg)
        {
            pthread_mutex_unlock( &air_bitcode_mutex );
            return STATUS_INVALID_PARAMETER;
        }
        retained = record->dispatch_data;
        status = wmt_normal_call( 0, &retained );
        if (!status) wire.ret = retained;
        pthread_mutex_unlock( &air_bitcode_mutex );
        if (!status) memcpy( args, &wire, sizeof(wire) );
        return status;
    }
    if (wire.arg && (status = wmt_snapshot_bytes( wire.handle, wire.arg, &bytes ))) return status;
    if (!wire.arg) return STATUS_INVALID_PARAMETER;
    token = 0;
    status = wmt_dispatch_data_from_snapshot( bytes, wire.arg, wmt_snapshot_free, &token );
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_obj_u64_obj_ret, ret),
                         &token, sizeof(token) );
    return status;
}

NTSTATUS wmt_rollback_114( void *args, NTSTATUS status )
{
    uint64_t object, zero = 0;

    memcpy( &object, (char *)args + offsetof(struct wmt_params32_obj_u64_obj_ret, ret),
            sizeof(object) );
    if (!object) return status;
    memcpy( (char *)args + offsetof(struct wmt_params32_obj_u64_obj_ret, ret),
            &zero, sizeof(zero) );
    return release_native_object_on_failure( object, status );
}

static NTSTATUS call_cache_init( unsigned int index, void *args )
{
    struct wmt_params32_cache_init wire;
    struct wmt_params_cache_init native;
    NTSTATUS status;
    char *path;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret = 0;
    memcpy( args, &wire, sizeof(wire) );
    if (wire.path.high) return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_cstring( wire.path.low, PATH_MAX + 1u, &path ))) return status;
    native.path.ptr = path;
    native.version = wire.version;
    native.ret = 0;
    status = wmt_normal_call( index, &native );
    wmt_snapshot_free( path, PATH_MAX + 1u );
    if (status && native.ret)
        return release_native_object_on_failure( native.ret, status );
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_cache_init, ret),
                         &native.ret, sizeof(native.ret) );
    return status;
}

NTSTATUS wmt_rollback_cache_init( void *args, NTSTATUS status )
{
    uint64_t object, zero = 0;

    memcpy( &object, (char *)args + offsetof(struct wmt_params32_cache_init, ret),
            sizeof(object) );
    if (!object) return status;
    memcpy( (char *)args + offsetof(struct wmt_params32_cache_init, ret), &zero, sizeof(zero) );
    return release_native_object_on_failure( object, status );
}

void wmt_prepare_owned_output( unsigned int index, void *args )
{
    uint64_t zero = 0;
    uint32_t zero32 = 0;
    size_t offset;

    switch (index)
    {
    case WMT_CALL_COPY_ALL_DEVICES:
    case WMT_CALL_AUTORELEASE_POOL_INIT:
    case WMT_CALL_SHARED_EVENT_LISTENER_CREATE:
        offset = 0;
        break;
    case WMT_CALL_NEW_SHARED_EVENT:
    case WMT_CALL_NEW_FENCE:
    case WMT_CALL_NEW_EVENT:
        offset = 8;
        break;
    case WMT_CALL_NEW_COMMAND_QUEUE:
    case WMT_CALL_NEW_SAMPLER:
    case WMT_CALL_NEW_DEPTH_STENCIL:
    case WMT_CALL_NEW_TEXTURE:
    case WMT_CALL_LIBRARY_NEW_FUNCTION:
    case WMT_CALL_NSSTRING_ALLOC_INIT:
    case WMT_CALL_CACHE_READER_INIT:
    case WMT_CALL_CACHE_WRITER_INIT:
    case WMT_CALL_NEW_SHARED_EVENT_WITH_PORT:
    case WMT_CALL_NEW_TIMESTAMP_BUFFER:
        offset = 16;
        break;
    case WMT_CALL_NEW_COMPUTE_PSO:
    case WMT_CALL_NEW_RENDER_PSO:
    case WMT_CALL_NEW_LIBRARY:
    case WMT_CALL_NEW_RESIDENCY_SET:
        offset = 24;
        break;
    case WMT_CALL_NEW_TEXTURE_VIEW:
    case WMT_CALL_FUNCTION_CONSTANTS:
        offset = 32;
        break;
    case WMT_CALL_CREATE_METAL_VIEW:
        offset = 16;
        memcpy( (char *)args + 24, &zero, sizeof(zero) );
        break;
    case WMT_CALL_BOOTSTRAP_LOOKUP:
        memcpy( (char *)args + 128, &zero32, sizeof(zero32) );
        return;
    case WMT_CALL_SHARED_EVENT_CREATE_PORT:
        memcpy( (char *)args + 8, &zero32, sizeof(zero32) );
        return;
    default:
        return;
    }
    memcpy( (char *)args + offset, &zero, sizeof(zero) );
}

NTSTATUS wmt_rollback_owned_output( unsigned int index, void *args, NTSTATUS status )
{
    uint64_t object = 0, zero = 0;
    NTSTATUS cleanup_status = STATUS_SUCCESS;
    size_t object_offset = 0;

    switch (index)
    {
    case WMT_CALL_COPY_ALL_DEVICES:
    case WMT_CALL_AUTORELEASE_POOL_INIT:
        object_offset = 0;
        goto take_offset_object;
    case WMT_CALL_NEW_COMMAND_QUEUE:
        object_offset = 16;
        goto take_offset_object;
    case WMT_CALL_NEW_SHARED_EVENT:
    case WMT_CALL_NEW_FENCE:
    case WMT_CALL_NEW_EVENT:
        object_offset = 8;
        goto take_offset_object;
    case WMT_CALL_NEW_TEXTURE_VIEW:
        object_offset = 32;
        goto take_offset_object;
    case WMT_CALL_NEW_LIBRARY:
        object_offset = 24;
        goto take_offset_object;
    case WMT_CALL_NEW_SHARED_EVENT_WITH_PORT:
    case WMT_CALL_NEW_TIMESTAMP_BUFFER:
        object_offset = 16;
        goto take_offset_object;
    case WMT_CALL_NEW_RESIDENCY_SET:
        object_offset = 24;
take_offset_object:
        memcpy( &object, (char *)args + object_offset, sizeof(object) );
        memcpy( (char *)args + object_offset, &zero, sizeof(zero) );
        break;
    case WMT_CALL_CREATE_METAL_VIEW:
        memcpy( &object, (char *)args + 16, sizeof(object) );
        memcpy( (char *)args + 16, &zero, sizeof(zero) );
        memcpy( (char *)args + 24, &zero, sizeof(zero) );
        if (object)
        {
            NTSTATUS release_status = wmt_normal_call( WMT_CALL_RELEASE_METAL_VIEW, &object );

            if (release_status) return release_status;
        }
        return status;
    case WMT_CALL_BOOTSTRAP_LOOKUP:
    case WMT_CALL_SHARED_EVENT_CREATE_PORT:
    {
        uint32_t port = 0, zero32 = 0;
        size_t port_offset = index == WMT_CALL_BOOTSTRAP_LOOKUP ? 128 : 8;

        memcpy( &port, (char *)args + port_offset, sizeof(port) );
        memcpy( (char *)args + port_offset, &zero32, sizeof(zero32) );
#ifdef __APPLE__
        if (port && mach_port_deallocate( mach_task_self(), port ) != KERN_SUCCESS)
            return STATUS_UNSUCCESSFUL;
#else
        if (port) return STATUS_NOT_SUPPORTED;
#endif
        return status;
    }
    case WMT_CALL_SHARED_EVENT_LISTENER_CREATE:
        memcpy( &object, args, sizeof(object) );
        memcpy( args, &zero, sizeof(zero) );
        if (object)
        {
            NTSTATUS release_status = wmt_shared_event_listener_destroy( object );

            if (release_status) return release_status;
        }
        return status;
    case WMT_CALL_NEW_SAMPLER:
    case WMT_CALL_NEW_DEPTH_STENCIL:
    case WMT_CALL_NEW_TEXTURE:
    {
        struct wmt_params32_info_ret *wire = args;

        object = wire->ret;
        wire->ret = 0;
        if (index == WMT_CALL_NEW_SAMPLER)
            cleanup_status = copy_to_wire_offset( wire->info,
                    offsetof(struct wmt_sampler_info32, gpu_resource_id), &zero, sizeof(zero) );
        else if (index == WMT_CALL_NEW_TEXTURE)
        {
            cleanup_status = copy_to_wire_offset( wire->info,
                    offsetof(struct wmt_texture_info32, gpu_resource_id), &zero, sizeof(zero) );
            if (!cleanup_status)
            {
                uint32_t zero32 = 0;

                cleanup_status = copy_to_wire_offset( wire->info,
                        offsetof(struct wmt_texture_info32, mach_port), &zero32, sizeof(zero32) );
            }
        }
        break;
    }
    case WMT_CALL_LIBRARY_NEW_FUNCTION:
    {
        struct wmt_params32_obj_u64_obj_ret *wire = args;

        object = wire->ret;
        wire->ret = 0;
        break;
    }
    case WMT_CALL_NEW_COMPUTE_PSO:
    case WMT_CALL_NEW_RENDER_PSO:
    {
        struct wmt_params32_pipeline *wire = args;

        object = wire->ret_pso;
        wire->ret_pso = 0;
        break;
    }
    case WMT_CALL_NSSTRING_ALLOC_INIT:
    {
        struct wmt_params32_string *wire = args;

        object = wire->ret;
        wire->ret = 0;
        break;
    }
    case WMT_CALL_FUNCTION_CONSTANTS:
    {
        struct wmt_params32_function_constants *wire = args;

        object = wire->ret;
        wire->ret = 0;
        break;
    }
    case WMT_CALL_CACHE_READER_INIT:
    case WMT_CALL_CACHE_WRITER_INIT:
        return wmt_rollback_cache_init( args, status );
    default:
        return status;
    }
    if (object)
    {
        NTSTATUS release_status = release_native_object( object );

        if (release_status) return release_status;
    }
    return cleanup_status ? cleanup_status : status;
}

NTSTATUS wmt_call_115( void *args )
{
    return call_cache_init( WMT_CALL_CACHE_READER_INIT, args );
}

NTSTATUS wmt_call_117( void *args )
{
    return call_cache_init( WMT_CALL_CACHE_WRITER_INIT, args );
}

NTSTATUS wmt_call_119( void *args )
{
    struct wmt_params32_set_cache_path wire;
    struct wmt_params_set_cache_path native;
    NTSTATUS status;
    char *path;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret_success = 0;
    memcpy( args, &wire, sizeof(wire) );
    if (wire.path.high) return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_cstring( wire.path.low, PATH_MAX + 1u, &path ))) return status;
    native.path.ptr = path;
    native.ret_success = 0;
    status = wmt_normal_call( WMT_CALL_SET_CACHE_PATH, &native );
    wmt_snapshot_free( path, PATH_MAX + 1u );
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_set_cache_path, ret_success),
                         &native.ret_success, sizeof(native.ret_success) );
    return status;
}

struct wmt_wire_buffer_outputs
{
    struct wmt_wire_ptr memory;
    uint64_t gpu_address;
};

C_ASSERT( sizeof(struct wmt_wire_buffer_outputs) == 16 );
C_ASSERT( offsetof(struct wmt_buffer_info32, memory) +
          sizeof(struct wmt_wire_buffer_outputs) == sizeof(struct wmt_buffer_info32) );

static NTSTATUS release_wow_owned_backing(
    const struct wine_unixlib_owned_backing_codec_v2 *codec, uint64_t lease,
    NTSTATUS status )
{
    NTSTATUS release_status;

    if (!lease) return status;
    release_status = codec->release_backing( lease );
    return release_status ? release_status : status;
}

static NTSTATUS call_wow_owned_buffer( void *args,
                                       const struct wmt_params32_info_ret *wire,
                                       const struct wmt_buffer_info32 *info32 )
{
    const struct wine_unixlib_owned_backing_codec_v2 *owned_codec =
        wmt_get_owned_backing_codec();
    struct wine_unixlib_owned_backing_v2 backing;
    struct wmt_wire_buffer_outputs outputs, zero_outputs;
    uint64_t rounded_length;
    wmt_uint64_t buffer = 0, gpu_address = 0;
    void *translated_address;
    size_t page_size;
    NTSTATUS status;

    if (!owned_codec ||
        owned_codec->version != WINE_UNIXLIB_OWNED_BACKING_CODEC_V2_VERSION ||
        owned_codec->size != sizeof(*owned_codec) ||
        owned_codec->capabilities != WINE_UNIXLIB_OWNED_BACKING_CAP_ACQUIRE_RELEASE ||
        !owned_codec->acquire_backing || !owned_codec->release_backing)
        return STATUS_INVALID_DEVICE_STATE;

    memset( &zero_outputs, 0, sizeof(zero_outputs) );
    if ((status = copy_to_wire_offset( wire->info, offsetof(struct wmt_buffer_info32, memory),
                                       &zero_outputs, sizeof(zero_outputs) )))
        return status;

    memset( &backing, 0, sizeof(backing) );
    status = owned_codec->acquire_backing( info32->length,
                                           WINE_WOW64_UNIXLIB_ACCESS_READ |
                                           WINE_WOW64_UNIXLIB_ACCESS_WRITE, &backing );
    if (status) return status;
    page_size = getpagesize();
    if (backing.version != WINE_UNIXLIB_OWNED_BACKING_V2_VERSION ||
        backing.size != sizeof(backing) || !backing.address || !backing.guest_address ||
        !backing.lease || !backing.generation || backing.length != info32->length ||
        !page_size || (page_size & (page_size - 1)) ||
        (uint64_t)(uintptr_t)backing.address != backing.address ||
        backing.address & (page_size - 1) || backing.guest_address & (page_size - 1) ||
        backing.guest_address == backing.address ||
        backing.length > UINT64_MAX - (page_size - 1) ||
        backing.address > UINT64_MAX - backing.mapped_length ||
        backing.guest_address > UINT32_MAX ||
        backing.mapped_length > UINT32_MAX + 1ull - backing.guest_address)
        return release_wow_owned_backing( owned_codec, backing.lease, STATUS_INVALID_PARAMETER );
    rounded_length = (backing.length + page_size - 1) & ~(uint64_t)(page_size - 1);
    if (!rounded_length || backing.mapped_length != rounded_length)
        return release_wow_owned_backing( owned_codec, backing.lease,
                                           STATUS_INVALID_PARAMETER );
    translated_address = NULL;
    status = ntdll_wow64_guest32_to_host( (ULONG)backing.guest_address,
                                          &translated_address );
    if (!status) status = ntdll_wow64_probe_user_read( translated_address,
                                                       backing.mapped_length );
    if (!status) status = ntdll_wow64_probe_user_write( translated_address,
                                                        backing.mapped_length );
    if (status || translated_address != (void *)(uintptr_t)backing.address)
        return release_wow_owned_backing( owned_codec, backing.lease,
                                           status ? status : STATUS_INVALID_PARAMETER );

    status = wmt_metal_buffer_from_alias( wire->object,
                                          (void *)(uintptr_t)backing.address,
                                          backing.length, backing.mapped_length,
                                          info32->options, backing.lease,
                                          owned_codec->release_backing,
                                          &buffer, &gpu_address );
    /* The Metal helper consumes the lease on every failure after entry. */
    if (status) return status;

    outputs.memory.low = (uint32_t)backing.guest_address;
    outputs.memory.high = 0;
    outputs.gpu_address = gpu_address;
    status = copy_to_wire_offset( wire->info, offsetof(struct wmt_buffer_info32, memory),
                                  &outputs, sizeof(outputs) );
    if (status)
    {
        NTSTATUS cleanup_status;

        copy_to_wire_offset( wire->info, offsetof(struct wmt_buffer_info32, memory),
                             &zero_outputs, sizeof(zero_outputs) );
        cleanup_status = release_native_object( buffer );
        return cleanup_status ? cleanup_status : status;
    }

    /* Publishing the object last commits ownership of the nested guest pointer. */
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret),
            &buffer, sizeof(buffer) );
    return STATUS_SUCCESS;
}

NTSTATUS wmt_call_18( void *args )
{
    struct wmt_params32_info_ret wire;
    struct wmt_buffer_info32 info32;
    struct wmt_buffer_info info;
    struct wmt_params_info_ret native;
    struct wmt_wire_ptr contents;
    NTSTATUS status;
    uintptr_t address;
    uint64_t zero = 0;

    memcpy( &wire, args, sizeof(wire) );
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret), &zero, sizeof(zero) );
    if ((status = copy_to_wire_offset( wire.info, offsetof(struct wmt_buffer_info32, gpu_address),
                                       &zero, sizeof(zero) )))
        return status;
    if ((status = copy_from_wire( wire.info, &info32, sizeof(info32) ))) return status;
    wmt_buffer_info32_to_native( &info32, &info );

    if (!info32.memory.low && !info32.memory.high)
    {
        uint64_t storage_mode = info32.options & WMT_RESOURCE_STORAGE_MODE_MASK;

        if (info32.length &&
            (storage_mode == WMT_RESOURCE_STORAGE_MODE_SHARED ||
             storage_mode == WMT_RESOURCE_STORAGE_MODE_MANAGED))
        {
            /* args is the loader-owned native call frame; publish ret only after
             * the guest info outputs have committed successfully. */
            return call_wow_owned_buffer( args, &wire, &info32 );
        }
        native.object = wire.object;
        native.info.ptr = &info;
        native.ret = 0;
        status = wmt_normal_call( WMT_CALL_NEW_BUFFER, &native );
        if (status) return release_native_object_on_failure( native.ret, status );
        if ((status = copy_to_wire_offset( wire.info, offsetof(struct wmt_buffer_info32, gpu_address),
                                           &info.gpu_address, sizeof(info.gpu_address) )))
        {
            copy_to_wire_offset( wire.info, offsetof(struct wmt_buffer_info32, gpu_address),
                                 &zero, sizeof(zero) );
            return release_native_object_on_failure( native.ret, status );
        }
        address = (uintptr_t)info.memory.ptr;
        contents.low = (uint32_t)address;
        contents.high = (uint32_t)((uint64_t)address >> 32);
        if ((status = copy_to_wire_offset( wire.info, offsetof(struct wmt_buffer_info32, memory),
                                           &contents, sizeof(contents) )))
        {
            copy_to_wire_offset( wire.info, offsetof(struct wmt_buffer_info32, memory),
                                 &zero, sizeof(zero) );
            copy_to_wire_offset( wire.info, offsetof(struct wmt_buffer_info32, gpu_address),
                                 &zero, sizeof(zero) );
            return release_native_object_on_failure( native.ret, status );
        }
        memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret),
                &native.ret, sizeof(native.ret) );
        return STATUS_SUCCESS;
    }

    if (info32.memory.high || !info32.length ||
        !valid_guest_range( info32.memory.low, info32.length ))
        return STATUS_INVALID_PARAMETER;
    /* Metal may retain caller storage beyond this call.  The v4 loader has no
     * safe pin-manager capability for caller-owned mappings, so fail closed. */
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_rollback_18( void *args, NTSTATUS status )
{
    struct wmt_params32_info_ret wire;
    struct wmt_wire_buffer_outputs zero_outputs;
    uint64_t buffer, zero = 0;
    NTSTATUS clear_status, release_status;

    memcpy( &wire, args, sizeof(wire) );
    memcpy( &buffer, (char *)args + offsetof(struct wmt_params32_info_ret, ret),
            sizeof(buffer) );
    if (!buffer) return status;

    memset( &zero_outputs, 0, sizeof(zero_outputs) );
    clear_status = copy_to_wire_offset( wire.info, offsetof(struct wmt_buffer_info32, memory),
                                        &zero_outputs, sizeof(zero_outputs) );
    release_status = release_native_object( buffer );
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret), &zero, sizeof(zero) );
    if (release_status) return release_status;
    if (clear_status) return clear_status;
    return status;
}

struct wmt_air_argument_allocation
{
    union
    {
        struct wmt_air_argument base;
        struct wmt_air_stream_output_argument stream_output;
        struct wmt_air_common_argument common;
        struct wmt_air_pixel_argument pixel;
        struct wmt_air_input_layout_argument input_layout;
        struct wmt_air_gs_passthrough_argument gs_passthrough;
        struct wmt_air_bool_argument boolean;
        struct wmt_air_u32_argument u32;
        struct wmt_air_root_signature_argument root_signature;
    } value;
    void *payload;
    uint64_t payload_size;
    struct wmt_air_argument_allocation *next;
};

static NTSTATUS release_air_handle( unsigned int index, uint64_t handle )
{
    if (!handle) return STATUS_SUCCESS;
    return wmt_normal_call( index, &handle );
}

static NTSTATUS register_air_shader( uint64_t shader, const struct wmt_air_reflection *reflection )
{
    struct wmt_air_shader_record *current, *record;

    if (!shader) return STATUS_INVALID_PARAMETER;
    if (!(record = malloc( sizeof(*record) ))) return STATUS_NO_MEMORY;
    record->shader = shader;
    record->constant_buffers = reflection->num_constant_buffers;
    record->arguments = reflection->num_arguments;
    record->references = 0;
    record->destroying = FALSE;
    pthread_mutex_lock( &air_shader_mutex );
    for (current = air_shaders; current && current->shader != shader; current = current->next);
    if (current)
    {
        pthread_mutex_unlock( &air_shader_mutex );
        free( record );
        return STATUS_INVALID_HANDLE;
    }
    record->next = air_shaders;
    air_shaders = record;
    pthread_mutex_unlock( &air_shader_mutex );
    return STATUS_SUCCESS;
}

static NTSTATUS retain_air_shaders( uint64_t first, uint64_t second,
                                    struct wmt_air_shader_record **ret_first,
                                    struct wmt_air_shader_record **ret_second )
{
    struct wmt_air_shader_record *record;

    *ret_first = *ret_second = NULL;
    pthread_mutex_lock( &air_shader_mutex );
    for (record = air_shaders; record && record->shader != first; record = record->next);
    if (!record || record->destroying) goto invalid;
    ++record->references;
    *ret_first = record;
    if (!second || second == first)
    {
        *ret_second = second ? record : NULL;
        pthread_mutex_unlock( &air_shader_mutex );
        return STATUS_SUCCESS;
    }
    for (record = air_shaders; record && record->shader != second; record = record->next);
    if (!record || record->destroying)
    {
        --(*ret_first)->references;
        *ret_first = NULL;
        goto invalid;
    }
    ++record->references;
    *ret_second = record;
    pthread_mutex_unlock( &air_shader_mutex );
    return STATUS_SUCCESS;

invalid:
    pthread_mutex_unlock( &air_shader_mutex );
    return STATUS_INVALID_HANDLE;
}

static void release_air_shaders( struct wmt_air_shader_record *first,
                                 struct wmt_air_shader_record *second )
{
    pthread_mutex_lock( &air_shader_mutex );
    if (second && second != first)
    {
        if (!second->references) abort();
        --second->references;
    }
    if (first)
    {
        if (!first->references) abort();
        --first->references;
    }
    pthread_cond_broadcast( &air_shader_cond );
    pthread_mutex_unlock( &air_shader_mutex );
}

static NTSTATUS destroy_registered_air_shader( uint64_t shader )
{
    struct wmt_air_shader_record **cursor, *record;
    NTSTATUS status;

    pthread_mutex_lock( &air_shader_mutex );
    for (record = air_shaders; record && record->shader != shader; record = record->next);
    if (!record || record->destroying)
    {
        pthread_mutex_unlock( &air_shader_mutex );
        return STATUS_INVALID_HANDLE;
    }
    record->destroying = TRUE;
    while (record->references) pthread_cond_wait( &air_shader_cond, &air_shader_mutex );
    for (cursor = &air_shaders; *cursor != record; cursor = &(*cursor)->next);
    *cursor = record->next;
    pthread_mutex_unlock( &air_shader_mutex );
    /* Native destroy is destructive even when a fault is caught after delete.
     * Detach first and never make the raw owner reachable for a retry. */
    status = wmt_normal_call( 75, &shader );
    free( record );
    return status;
}

static NTSTATUS register_air_error( uint64_t error )
{
    struct wmt_air_error_record *current, *record;

    if (!error) return STATUS_SUCCESS;
    if (!(record = malloc( sizeof(*record) ))) return STATUS_NO_MEMORY;
    record->error = error;
    record->destroying = FALSE;
    pthread_mutex_lock( &air_error_mutex );
    for (current = air_errors; current && current->error != error; current = current->next);
    if (current)
    {
        pthread_mutex_unlock( &air_error_mutex );
        free( record );
        return STATUS_INVALID_HANDLE;
    }
    record->next = air_errors;
    air_errors = record;
    pthread_mutex_unlock( &air_error_mutex );
    return STATUS_SUCCESS;
}

static NTSTATUS destroy_registered_air_error( uint64_t error )
{
    struct wmt_air_error_record **cursor, *record;
    NTSTATUS status;

    pthread_mutex_lock( &air_error_mutex );
    for (record = air_errors; record && record->error != error; record = record->next);
    if (!record || record->destroying)
    {
        pthread_mutex_unlock( &air_error_mutex );
        return STATUS_INVALID_HANDLE;
    }
    record->destroying = TRUE;
    for (cursor = &air_errors; *cursor != record; cursor = &(*cursor)->next);
    *cursor = record->next;
    pthread_mutex_unlock( &air_error_mutex );
    status = wmt_normal_call( 80, &error );
    free( record );
    return status;
}

static NTSTATUS register_air_bitcode( uint64_t bitcode )
{
    struct wmt_air_bitcode_record *current, *record;

    if (!bitcode) return STATUS_INVALID_PARAMETER;
    if (!(record = calloc( 1, sizeof(*record) ))) return STATUS_NO_MEMORY;
    record->bitcode = bitcode;
    pthread_mutex_lock( &air_bitcode_mutex );
    for (current = air_bitcodes; current && current->bitcode != bitcode; current = current->next);
    if (current)
    {
        pthread_mutex_unlock( &air_bitcode_mutex );
        free( record );
        return STATUS_INVALID_HANDLE;
    }
    record->next = air_bitcodes;
    air_bitcodes = record;
    pthread_mutex_unlock( &air_bitcode_mutex );
    return STATUS_SUCCESS;
}

static NTSTATUS destroy_registered_air_bitcode( uint64_t bitcode )
{
    struct wmt_air_bitcode_record **cursor, *record;
    NTSTATUS release_status = STATUS_SUCCESS, status;

    pthread_mutex_lock( &air_bitcode_mutex );
    for (record = air_bitcodes; record && record->bitcode != bitcode; record = record->next);
    if (!record || record->destroying)
    {
        pthread_mutex_unlock( &air_bitcode_mutex );
        return STATUS_INVALID_HANDLE;
    }
    record->destroying = TRUE;
    for (cursor = &air_bitcodes; *cursor != record; cursor = &(*cursor)->next);
    *cursor = record->next;
    pthread_mutex_unlock( &air_bitcode_mutex );
    status = wmt_normal_call( 78, &bitcode );
    if (record->dispatch_data) release_status = release_native_object( record->dispatch_data );
    free( record );
    return status ? status : release_status;
}

NTSTATUS wmt_drain_air_registries(void)
{
    uint64_t handle;
    NTSTATUS status;

    for (;;)
    {
        pthread_mutex_lock( &air_bitcode_mutex );
        handle = air_bitcodes ? air_bitcodes->bitcode : 0;
        pthread_mutex_unlock( &air_bitcode_mutex );
        if (!handle) break;
        if ((status = destroy_registered_air_bitcode( handle ))) return status;
    }
    for (;;)
    {
        pthread_mutex_lock( &air_error_mutex );
        handle = air_errors ? air_errors->error : 0;
        pthread_mutex_unlock( &air_error_mutex );
        if (!handle) break;
        if ((status = destroy_registered_air_error( handle ))) return status;
    }
    for (;;)
    {
        pthread_mutex_lock( &air_shader_mutex );
        handle = air_shaders ? air_shaders->shader : 0;
        pthread_mutex_unlock( &air_shader_mutex );
        if (!handle) break;
        if ((status = destroy_registered_air_shader( handle ))) return status;
    }
    return STATUS_SUCCESS;
}

static void free_air_arguments( struct wmt_air_argument_allocation *allocation )
{
    struct wmt_air_argument_allocation *next;

    while (allocation)
    {
        next = allocation->next;
        wmt_snapshot_free( allocation->payload, allocation->payload_size );
        wmt_snapshot_free( allocation, sizeof(*allocation) );
        allocation = next;
    }
}

static BOOL valid_air_attribute_format( uint32_t format )
{
    switch (format)
    {
    case 1: case 3: case 4: case 6: case 7: case 9: case 10: case 12:
    case 13: case 15: case 16: case 18: case 19: case 21: case 22: case 24:
    case 25: case 27: case 28: case 29: case 30: case 31: case 32: case 33:
    case 34: case 35: case 36: case 37: case 38: case 39: case 40: case 41:
    case 42: case 45: case 46: case 47: case 48: case 49: case 50: case 51:
    case 52: case 53: case 54: case 55:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL valid_air_gs_passthrough_pair( uint32_t data, unsigned int shift )
{
    uint8_t reg = data >> shift;
    uint8_t component = data >> (shift + 8);

    return (reg == 0xff && component == 0xff) || (reg < 32 && component < 4);
}

static NTSTATUS copy_air_arguments( uint32_t guest, struct wmt_air_argument *sentinel,
                                    struct wmt_air_argument_allocation **ret )
{
    struct wmt_air_argument_allocation *allocation, **tail = ret;
    struct wmt_air_argument32 base;
    uint32_t *visited = NULL;
    uint32_t address, slot;
    uint64_t graph_size, payload_size;
    uint32_t seen_types = 0;
    unsigned int count = 0;
    NTSTATUS status = STATUS_SUCCESS;

    *ret = NULL;
    sentinel->next = NULL;
    sentinel->type = UINT32_MAX;
    if (!guest) return STATUS_SUCCESS;
    graph_size = WMT_AIR_ARGUMENT_MAX * 2u * sizeof(*visited);
    if ((status = wmt_snapshot_alloc( graph_size,
                                      (void **)&visited )))
        return status;
    memset( visited, 0, graph_size );

    while (guest)
    {
        if (++count > WMT_AIR_ARGUMENT_MAX)
        {
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        address = guest;
        slot = (address * 2654435761u) & (WMT_AIR_ARGUMENT_MAX * 2u - 1u);
        while (visited[slot] && visited[slot] != address)
            slot = (slot + 1u) & (WMT_AIR_ARGUMENT_MAX * 2u - 1u);
        if (visited[slot] == address)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        visited[slot] = address;
        if ((status = wmt_snapshot_bytes( address, sizeof(base), (void **)&allocation ))) break;
        memcpy( &base, allocation, sizeof(base) );
        wmt_snapshot_free( allocation, sizeof(base) );
        allocation = NULL;
        if (base.type < 1 || base.type > 8 || (seen_types & (1u << base.type)))
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        seen_types |= 1u << base.type;
        if (graph_size > WMT_AIR_GRAPH_MAX - sizeof(*allocation))
        {
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        graph_size += sizeof(*allocation);
        if ((status = wmt_snapshot_alloc( sizeof(*allocation), (void **)&allocation ))) break;
        memset( allocation, 0, sizeof(*allocation) );
        allocation->value.base.type = base.type;
        guest = base.next;

        switch (base.type)
        {
        case 1:
        {
            struct wmt_air_stream_output_argument32 wire;

            if ((status = wmt_snapshot_bytes( address, sizeof(wire), (void **)&allocation->payload ))) break;
            memcpy( &wire, allocation->payload, sizeof(wire) );
            wmt_snapshot_free( allocation->payload, sizeof(wire) );
            allocation->payload = NULL;
            allocation->value.stream_output.num_output_slots = wire.num_output_slots;
            allocation->value.stream_output.num_elements = wire.num_elements;
            memcpy( allocation->value.stream_output.strides, wire.strides, sizeof(wire.strides) );
            if (!wire.num_output_slots || wire.num_output_slots > 4 ||
                wire.num_elements > WMT_CONSTANT_COUNT_MAX)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            payload_size = wire.num_elements * sizeof(struct wmt_air_stream_output_element);
            if (payload_size > WMT_AIR_GRAPH_MAX - graph_size)
            {
                status = STATUS_BUFFER_OVERFLOW;
                break;
            }
            graph_size += payload_size;
            if (payload_size && (status = wmt_snapshot_bytes( wire.elements, payload_size,
                                                              &allocation->payload )))
                break;
            allocation->payload_size = payload_size;
            allocation->value.stream_output.elements = allocation->payload;
            if (payload_size)
            {
                const struct wmt_air_stream_output_element *elements = allocation->payload;
                unsigned int i;

                for (i = 0; i < wire.num_elements; ++i)
                    if (elements[i].component >= 4 ||
                        elements[i].output_slot >= wire.num_output_slots ||
                        elements[i].output_slot >= 4 ||
                        (elements[i].reg_id != UINT32_MAX && elements[i].reg_id >= 32) ||
                        (elements[i].offset & 3) ||
                        (wire.strides[elements[i].output_slot] & 3) ||
                        wire.strides[elements[i].output_slot] < 4 ||
                        elements[i].offset > wire.strides[elements[i].output_slot] - 4)
                    {
                        status = STATUS_INVALID_PARAMETER;
                        break;
                    }
            }
            break;
        }
        case 2:
        {
            struct wmt_air_common_argument32 wire;

            if ((status = wmt_snapshot_bytes( address, sizeof(wire), (void **)&allocation->payload ))) break;
            memcpy( &wire, allocation->payload, sizeof(wire) );
            wmt_snapshot_free( allocation->payload, sizeof(wire) );
            allocation->payload = NULL;
            allocation->value.common.metal_version = wire.metal_version;
            allocation->value.common.flags = wire.flags;
            if ((wire.metal_version != 310 && wire.metal_version != 320) || (wire.flags & ~3u))
                status = STATUS_INVALID_PARAMETER;
            break;
        }
        case 3:
        {
            struct wmt_air_pixel_argument32 wire;

            if ((status = wmt_snapshot_bytes( address, sizeof(wire), (void **)&allocation->payload ))) break;
            memcpy( &wire, allocation->payload, sizeof(wire) );
            wmt_snapshot_free( allocation->payload, sizeof(wire) );
            allocation->payload = NULL;
            allocation->value.pixel.sample_mask = wire.sample_mask;
            allocation->value.pixel.dual_source_blending = wire.dual_source_blending;
            allocation->value.pixel.disable_depth_output = wire.disable_depth_output;
            allocation->value.pixel.unorm_output_reg_mask = wire.unorm_output_reg_mask;
            if (wire.dual_source_blending > 1 || wire.disable_depth_output > 1)
                status = STATUS_INVALID_PARAMETER;
            break;
        }
        case 4:
        {
            struct wmt_air_input_layout_argument32 wire;

            if ((status = wmt_snapshot_bytes( address, sizeof(wire), (void **)&allocation->payload ))) break;
            memcpy( &wire, allocation->payload, sizeof(wire) );
            wmt_snapshot_free( allocation->payload, sizeof(wire) );
            allocation->payload = NULL;
            allocation->value.input_layout.index_buffer_format = wire.index_buffer_format;
            allocation->value.input_layout.slot_mask = wire.slot_mask;
            allocation->value.input_layout.num_elements = wire.num_elements;
            if (wire.index_buffer_format > 2 || wire.num_elements > WMT_CONSTANT_COUNT_MAX)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            payload_size = wire.num_elements * sizeof(struct wmt_air_input_element);
            if (payload_size > WMT_AIR_GRAPH_MAX - graph_size)
            {
                status = STATUS_BUFFER_OVERFLOW;
                break;
            }
            graph_size += payload_size;
            if (payload_size && (status = wmt_snapshot_bytes( wire.elements, payload_size,
                                                              &allocation->payload )))
                break;
            allocation->payload_size = payload_size;
            allocation->value.input_layout.elements = allocation->payload;
            if (payload_size)
            {
                const struct wmt_air_input_element *elements = allocation->payload;
                unsigned int i;

                for (i = 0; i < wire.num_elements; ++i)
                    if (elements[i].reg >= 32 || elements[i].slot >= 32 ||
                        !(wire.slot_mask & (1u << elements[i].slot)) ||
                        !valid_air_attribute_format( elements[i].format ))
                    {
                        status = STATUS_INVALID_PARAMETER;
                        break;
                    }
            }
            break;
        }
        case 5:
        {
            struct wmt_air_gs_passthrough_argument32 wire;

            if ((status = wmt_snapshot_bytes( address, sizeof(wire), (void **)&allocation->payload ))) break;
            memcpy( &wire, allocation->payload, sizeof(wire) );
            wmt_snapshot_free( allocation->payload, sizeof(wire) );
            allocation->payload = NULL;
            allocation->value.gs_passthrough.data = wire.data;
            allocation->value.gs_passthrough.rasterization_disabled = wire.rasterization_disabled;
            if (wire.rasterization_disabled > 1 ||
                !valid_air_gs_passthrough_pair( wire.data, 0 ) ||
                !valid_air_gs_passthrough_pair( wire.data, 16 ))
                status = STATUS_INVALID_PARAMETER;
            break;
        }
        case 6:
        {
            struct wmt_air_bool_argument32 wire;

            if ((status = wmt_snapshot_bytes( address, sizeof(wire), (void **)&allocation->payload ))) break;
            memcpy( &wire, allocation->payload, sizeof(wire) );
            wmt_snapshot_free( allocation->payload, sizeof(wire) );
            allocation->payload = NULL;
            allocation->value.boolean.value = wire.value;
            if (wire.value > 1) status = STATUS_INVALID_PARAMETER;
            break;
        }
        case 7:
        {
            struct wmt_air_u32_argument32 wire;

            if ((status = wmt_snapshot_bytes( address, sizeof(wire), (void **)&allocation->payload ))) break;
            memcpy( &wire, allocation->payload, sizeof(wire) );
            wmt_snapshot_free( allocation->payload, sizeof(wire) );
            allocation->payload = NULL;
            allocation->value.u32.value = wire.value;
            break;
        }
        case 8:
        {
            struct wmt_air_root_signature_argument32 wire;

            if ((status = wmt_snapshot_bytes( address, sizeof(wire), (void **)&allocation->payload ))) break;
            memcpy( &wire, allocation->payload, sizeof(wire) );
            wmt_snapshot_free( allocation->payload, sizeof(wire) );
            allocation->payload = NULL;
            if (wire.bytecode_length > WMT_AIR_GRAPH_MAX - graph_size)
            {
                status = STATUS_BUFFER_OVERFLOW;
                break;
            }
            graph_size += wire.bytecode_length;
            if (wire.bytecode_length &&
                (status = wmt_snapshot_bytes( wire.bytecode, wire.bytecode_length,
                                              &allocation->payload )))
                break;
            allocation->payload_size = wire.bytecode_length;
            allocation->value.root_signature.bytecode = allocation->payload;
            allocation->value.root_signature.bytecode_length = wire.bytecode_length;
            break;
        }
        default:
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (status)
        {
            wmt_snapshot_free( allocation->payload, allocation->payload_size );
            wmt_snapshot_free( allocation, sizeof(*allocation) );
            break;
        }
        *tail = allocation;
        tail = &allocation->next;
    }

    if (!status)
    {
        struct wmt_air_argument *previous = sentinel;

        for (allocation = *ret; allocation; allocation = allocation->next)
        {
            previous->next = &allocation->value.base;
            previous = &allocation->value.base;
        }
    }
    wmt_snapshot_free( visited, WMT_AIR_ARGUMENT_MAX * 2u * sizeof(*visited) );
    if (status)
    {
        free_air_arguments( *ret );
        *ret = NULL;
    }
    return status;
}

NTSTATUS wmt_call_74( void *args )
{
    struct wmt_air_initialize32 wire;
    struct wmt_air_initialize native;
    struct wmt_air_reflection reflection;
    void *bytecode = NULL;
    uint64_t shader = 0, error = 0, zero = 0;
    int32_t ret = -1;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    memcpy( (char *)args + offsetof(struct wmt_air_initialize32, ret), &ret, sizeof(ret) );
    if (wire.shader && (status = zero_guest_range( wire.shader, sizeof(zero), sizeof(zero) ))) return status;
    if (wire.error && (status = zero_guest_range( wire.error, sizeof(zero), sizeof(zero) ))) return status;
    if (wire.reflection && (status = zero_guest_range( wire.reflection, sizeof(reflection), sizeof(reflection) )))
        return status;
    if (!wire.bytecode || !wire.bytecode_size || wire.bytecode_size > WMT_DISPATCH_BLOB_MAX ||
        !wire.shader || !wire.reflection || !wire.error)
        return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_bytes( wire.bytecode, wire.bytecode_size, &bytecode ))) return status;
    memset( &reflection, 0, sizeof(reflection) );
    native.bytecode = bytecode;
    native.bytecode_size = wire.bytecode_size;
    native.shader = &shader;
    native.reflection = &reflection;
    native.error = &error;
    native.ret = -1;
    status = wmt_normal_call( 74, &native );
    wmt_snapshot_free( bytecode, wire.bytecode_size );
    if (status)
    {
        release_air_handle( 75, shader );
        release_air_handle( 80, error );
        return status;
    }
    if (!native.ret)
    {
        if (!shader || error)
        {
            release_air_handle( 75, shader );
            release_air_handle( 80, error );
            return STATUS_INVALID_PARAMETER;
        }
        if ((status = register_air_shader( shader, &reflection )))
        {
            release_air_handle( 75, shader );
            return status;
        }
        if ((status = copy_to_wire( (struct wmt_wire_ptr){wire.reflection, 0},
                                     &reflection, sizeof(reflection) )) ||
            (status = copy_to_wire( (struct wmt_wire_ptr){wire.shader, 0},
                                     &shader, sizeof(shader) )))
        {
            destroy_registered_air_shader( shader );
            zero_guest_range( wire.shader, sizeof(zero), sizeof(zero) );
            zero_guest_range( wire.reflection, sizeof(reflection), sizeof(reflection) );
            return status;
        }
    }
    else
    {
        release_air_handle( 75, shader );
        if (!error) return STATUS_INVALID_PARAMETER;
        if ((status = register_air_error( error )))
        {
            release_air_handle( 80, error );
            return status;
        }
        if ((status = copy_to_wire( (struct wmt_wire_ptr){wire.error, 0},
                                     &error, sizeof(error) )))
        {
            if (error) destroy_registered_air_error( error );
            return status;
        }
    }
    ret = native.ret;
    memcpy( (char *)args + offsetof(struct wmt_air_initialize32, ret), &ret, sizeof(ret) );
    return STATUS_SUCCESS;
}

NTSTATUS wmt_call_75( void *args )
{
    uint64_t shader;

    memcpy( &shader, args, sizeof(shader) );
    return destroy_registered_air_shader( shader );
}

static NTSTATUS call_air_compile( unsigned int index, void *args, BOOL pipeline )
{
    struct wmt_air_pipeline_compile32 pipeline_wire;
    struct wmt_air_compile32 compile_wire;
    struct wmt_air_pipeline_compile pipeline_native;
    struct wmt_air_compile compile_native;
    struct wmt_air_argument_allocation *allocations = NULL;
    struct wmt_air_argument sentinel;
    char *function_name = NULL;
    uint32_t argument_guest, function_guest, bitcode_guest, error_guest;
    uint64_t bitcode = 0, error = 0, zero = 0;
    int32_t ret = -1;
    struct wmt_air_shader_record *first_record = NULL, *second_record = NULL;
    BOOL bitcode_registered = FALSE, error_registered = FALSE;
    NTSTATUS status;

    if (pipeline)
    {
        memcpy( &pipeline_wire, args, sizeof(pipeline_wire) );
        argument_guest = pipeline_wire.arguments;
        function_guest = pipeline_wire.function_name;
        bitcode_guest = pipeline_wire.bitcode;
        error_guest = pipeline_wire.error;
        memcpy( (char *)args + offsetof(struct wmt_air_pipeline_compile32, ret), &ret, sizeof(ret) );
    }
    else
    {
        memcpy( &compile_wire, args, sizeof(compile_wire) );
        argument_guest = compile_wire.arguments;
        function_guest = compile_wire.function_name;
        bitcode_guest = compile_wire.bitcode;
        error_guest = compile_wire.error;
        memcpy( (char *)args + offsetof(struct wmt_air_compile32, ret), &ret, sizeof(ret) );
    }
    if (!bitcode_guest || !error_guest || !function_guest) return STATUS_INVALID_PARAMETER;
    if ((status = zero_guest_range( bitcode_guest, sizeof(zero), sizeof(zero) )) ||
        (status = zero_guest_range( error_guest, sizeof(zero), sizeof(zero) )) ||
        (status = copy_air_arguments( argument_guest, &sentinel, &allocations )) ||
        (status = wmt_snapshot_cstring( function_guest, WMT_STRING_MAX + 1u, &function_name )))
        goto done;

    if (pipeline)
    {
        if ((status = retain_air_shaders( pipeline_wire.first_shader,
                                          pipeline_wire.second_shader,
                                          &first_record, &second_record )))
            goto done;
        pipeline_native.first_shader = pipeline_wire.first_shader;
        pipeline_native.second_shader = pipeline_wire.second_shader;
        pipeline_native.arguments = &sentinel;
        pipeline_native.function_name = function_name;
        pipeline_native.bitcode = &bitcode;
        pipeline_native.error = &error;
        pipeline_native.ret = -1;
        status = wmt_normal_call( index, &pipeline_native );
        ret = pipeline_native.ret;
    }
    else
    {
        if ((status = retain_air_shaders( compile_wire.shader, 0,
                                          &first_record, &second_record )))
            goto done;
        compile_native.shader = compile_wire.shader;
        compile_native.arguments = &sentinel;
        compile_native.function_name = function_name;
        compile_native.bitcode = &bitcode;
        compile_native.error = &error;
        compile_native.ret = -1;
        status = wmt_normal_call( index, &compile_native );
        ret = compile_native.ret;
    }
    release_air_shaders( first_record, second_record );
    first_record = second_record = NULL;
    if (status) goto cleanup_outputs;
    if (!ret)
    {
        if (!bitcode || error)
        {
            status = STATUS_INVALID_PARAMETER;
            goto cleanup_outputs;
        }
        if ((status = register_air_bitcode( bitcode ))) goto cleanup_outputs;
        bitcode_registered = TRUE;
        status = copy_to_wire( (struct wmt_wire_ptr){bitcode_guest, 0}, &bitcode, sizeof(bitcode) );
        if (status) goto cleanup_outputs;
    }
    else
    {
        release_air_handle( 78, bitcode );
        bitcode = 0;
        if (!error)
        {
            status = STATUS_INVALID_PARAMETER;
            goto cleanup_outputs;
        }
        if ((status = register_air_error( error ))) goto cleanup_outputs;
        error_registered = TRUE;
        status = copy_to_wire( (struct wmt_wire_ptr){error_guest, 0}, &error, sizeof(error) );
        if (status) goto cleanup_outputs;
    }
    if (pipeline)
        memcpy( (char *)args + offsetof(struct wmt_air_pipeline_compile32, ret), &ret, sizeof(ret) );
    else
        memcpy( (char *)args + offsetof(struct wmt_air_compile32, ret), &ret, sizeof(ret) );
    status = STATUS_SUCCESS;
    goto done;

cleanup_outputs:
    if (first_record) release_air_shaders( first_record, second_record );
    if (bitcode_registered) destroy_registered_air_bitcode( bitcode );
    else release_air_handle( 78, bitcode );
    if (error_registered) destroy_registered_air_error( error );
    else release_air_handle( 80, error );
    zero_guest_range( bitcode_guest, sizeof(zero), sizeof(zero) );
    zero_guest_range( error_guest, sizeof(zero), sizeof(zero) );
done:
    wmt_snapshot_free( function_name, WMT_STRING_MAX + 1u );
    free_air_arguments( allocations );
    return status;
}

NTSTATUS wmt_call_76( void *args ) { return call_air_compile( 76, args, FALSE ); }
NTSTATUS wmt_call_81( void *args ) { return call_air_compile( 81, args, TRUE ); }
NTSTATUS wmt_call_82( void *args ) { return call_air_compile( 82, args, TRUE ); }
NTSTATUS wmt_call_84( void *args ) { return call_air_compile( 84, args, TRUE ); }
NTSTATUS wmt_call_85( void *args ) { return call_air_compile( 85, args, TRUE ); }

NTSTATUS wmt_call_77( void *args )
{
    struct wmt_air_get_bitcode32 wire;
    struct wmt_air_get_bitcode native;
    struct wmt_air_compiled_bitcode data, output;
    struct wmt_air_bitcode_record *record;
    void *snapshot = NULL;
    uint64_t dispatch_data = 0;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    memset( &output, 0, sizeof(output) );
    if (!wire.bitcode || !wire.data_out) return STATUS_INVALID_PARAMETER;
    if ((status = copy_to_wire( (struct wmt_wire_ptr){wire.data_out, 0}, &output, sizeof(output) )))
        return status;

    pthread_mutex_lock( &air_bitcode_mutex );
    for (record = air_bitcodes; record && record->bitcode != wire.bitcode; record = record->next);
    if (!record || record->destroying)
    {
        pthread_mutex_unlock( &air_bitcode_mutex );
        return STATUS_INVALID_HANDLE;
    }
    if (record->dispatch_data)
    {
        output.data = record->token;
        output.size = record->size;
        status = copy_to_wire( (struct wmt_wire_ptr){wire.data_out, 0}, &output, sizeof(output) );
        pthread_mutex_unlock( &air_bitcode_mutex );
        return status;
    }
    memset( &data, 0, sizeof(data) );
    native.bitcode = wire.bitcode;
    native.data_out = &data;
    if ((status = wmt_normal_call( 77, &native ))) goto done;
    if (!data.data || !data.size || data.size > WMT_DISPATCH_BLOB_MAX || data.size > SIZE_MAX)
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if ((status = wmt_snapshot_alloc( data.size, &snapshot ))) goto done;
    if ((status = snapshot_native_bytes( snapshot, (const void *)(uintptr_t)data.data,
                                         data.size )))
        goto done;
    status = wmt_dispatch_data_from_snapshot( snapshot, data.size, wmt_snapshot_free,
                                              &dispatch_data );
    snapshot = NULL; /* The dispatch helper consumes the snapshot on every path. */
    if (status) goto done;
    if (air_next_token > WMT_AIR_TOKEN_MAX)
    {
        release_native_object( dispatch_data );
        status = STATUS_NO_MEMORY;
        goto done;
    }
    record->token = WMT_AIR_TOKEN_PREFIX | air_next_token++;
    record->dispatch_data = dispatch_data;
    record->size = data.size;
    output.data = record->token;
    output.size = record->size;
    status = copy_to_wire( (struct wmt_wire_ptr){wire.data_out, 0}, &output, sizeof(output) );
done:
    wmt_snapshot_free( snapshot, data.size );
    pthread_mutex_unlock( &air_bitcode_mutex );
    return status;
}

NTSTATUS wmt_call_78( void *args )
{
    uint64_t bitcode;

    memcpy( &bitcode, args, sizeof(bitcode) );
    return destroy_registered_air_bitcode( bitcode );
}

NTSTATUS wmt_call_79( void *args )
{
    struct wmt_air_get_error32 wire;
    struct wmt_air_get_error native;
    char *buffer = NULL;
    struct wmt_air_error_record *record;
    uint32_t ret_size = 0;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    memcpy( (char *)args + offsetof(struct wmt_air_get_error32, ret_size),
            &ret_size, sizeof(ret_size) );
    if (!wire.buffer_size) return STATUS_SUCCESS;
    if (!wire.buffer || wire.buffer_size > WMT_STRING_MAX) return STATUS_INVALID_PARAMETER;
    if ((status = wmt_snapshot_alloc( wire.buffer_size, (void **)&buffer ))) return status;
    memset( buffer, 0, wire.buffer_size );
    pthread_mutex_lock( &air_error_mutex );
    for (record = air_errors; record && record->error != wire.error; record = record->next);
    if (!record || record->destroying)
    {
        status = STATUS_INVALID_HANDLE;
        goto done;
    }
    native.error = wire.error;
    native.buffer = buffer;
    native.buffer_size = wire.buffer_size;
    native.ret_size = 0;
    status = wmt_normal_call( 79, &native );
    if (!status)
    {
        if (native.ret_size >= wire.buffer_size || native.ret_size > UINT32_MAX)
            status = STATUS_INVALID_PARAMETER;
        else
        {
            status = copy_to_wire( (struct wmt_wire_ptr){wire.buffer, 0},
                                   buffer, native.ret_size + 1u );
            if (!status)
            {
                ret_size = native.ret_size;
                memcpy( (char *)args + offsetof(struct wmt_air_get_error32, ret_size),
                        &ret_size, sizeof(ret_size) );
            }
        }
    }
done:
    pthread_mutex_unlock( &air_error_mutex );
    wmt_snapshot_free( buffer, wire.buffer_size );
    return status;
}

NTSTATUS wmt_call_80( void *args )
{
    uint64_t error;

    memcpy( &error, args, sizeof(error) );
    if (!error) return STATUS_SUCCESS;
    return destroy_registered_air_error( error );
}

NTSTATUS wmt_call_88( void *args )
{
    struct wmt_air_get_arguments32 wire;
    struct wmt_air_get_arguments native;
    struct wmt_air_shader_record *record;
    struct wmt_air_shader_argument *constant_buffers = NULL, *arguments = NULL;
    uint64_t constant_size = 0, argument_size = 0;
    BOOL retained = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    memcpy( &wire, args, sizeof(wire) );
    pthread_mutex_lock( &air_shader_mutex );
    for (record = air_shaders; record && record->shader != wire.shader; record = record->next);
    if (!record || record->destroying)
    {
        status = STATUS_INVALID_HANDLE;
        goto done;
    }
    ++record->references;
    retained = TRUE;
    constant_size = (uint64_t)record->constant_buffers * sizeof(*constant_buffers);
    argument_size = (uint64_t)record->arguments * sizeof(*arguments);
    pthread_mutex_unlock( &air_shader_mutex );
    if (constant_size > WMT_DISPATCH_BLOB_MAX || argument_size > WMT_DISPATCH_BLOB_MAX ||
        (constant_size && (!wire.constant_buffers ||
                           !valid_guest_range( wire.constant_buffers, constant_size ))) ||
        (argument_size && (!wire.arguments ||
                           !valid_guest_range( wire.arguments, argument_size ))))
    {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if ((constant_size && (status = wmt_snapshot_alloc( constant_size, (void **)&constant_buffers ))) ||
        (argument_size && (status = wmt_snapshot_alloc( argument_size, (void **)&arguments ))))
        goto done;
    if (constant_size) memset( constant_buffers, 0, constant_size );
    if (argument_size) memset( arguments, 0, argument_size );
    if ((status = publish_air_argument_outputs( wire.constant_buffers, constant_buffers, constant_size,
                                                wire.arguments, arguments, argument_size )))
        goto done;
    native.shader = wire.shader;
    native.constant_buffers = constant_buffers;
    native.arguments = arguments;
    if (!(status = wmt_normal_call( 88, &native )))
        status = publish_air_argument_outputs( wire.constant_buffers, constant_buffers, constant_size,
                                               wire.arguments, arguments, argument_size );
done:
    if (retained) release_air_shaders( record, NULL );
    else pthread_mutex_unlock( &air_shader_mutex );
    wmt_snapshot_free( constant_buffers, constant_size );
    wmt_snapshot_free( arguments, argument_size );
    return status;
}

NTSTATUS wmt_unsupported_no_output( void *args )
{
    (void)args;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_8( void *args )
{
    struct wmt_params32_getcstring params;
    struct wmt_wire_ptr buffer;
    char zero = 0;

    memcpy( &params, args, sizeof(params) );
    params.ret = 0;
    memcpy( (char *)args + offsetof(struct wmt_params32_getcstring, ret),
            &params.ret, sizeof(params.ret) );
    buffer.low = (uint32_t)params.buffer;
    buffer.high = params.buffer > UINT32_MAX;
    if (!buffer.high && params.max_length) copy_to_wire( buffer, &zero, 1 );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_22( void *args )
{
    struct wmt_params32_buffer_texture params;
    uint64_t zero64 = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_buffer_texture, ret),
            &zero64, sizeof(zero64) );
    if (!params.info.high)
        copy_to_wire_offset( params.info, offsetof(struct wmt_texture_info32, gpu_resource_id),
                             &zero64, sizeof(zero64) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_info_ret( void *args )
{
    struct wmt_params32_info_ret params;
    uint64_t zero = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret), &zero, sizeof(zero) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_capture( void *args )
{
    struct wmt_params32_capture params;
    uint8_t zero = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_capture, ret), &zero, sizeof(zero) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_pipeline( void *args )
{
    struct wmt_params32_pipeline params;

    memcpy( &params, args, sizeof(params) );
    params.ret_error = params.ret_pso = 0;
    memcpy( args, &params, sizeof(params) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_enumerate( void *args )
{
    struct wmt_params32_enumerate params;
    uint64_t zero = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_enumerate, ret_read), &zero, sizeof(zero) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_display_description( void *args )
{
    struct wmt_params32_obj_ptr params;
    struct wmt_display_description32 zero;

    memcpy( &params, args, sizeof(params) );
    memset( &zero, 0, sizeof(zero) );
    if (!params.arg.high) copy_to_wire( params.arg, &zero, sizeof(zero) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_query_display( void *args )
{
    struct wmt_params32_query_display params;
    struct wmt_hdr_metadata32 metadata;
    uint64_t zero64 = 0;
    uint8_t zero8 = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_query_display, colorspace),
            &zero64, sizeof(zero64) );
    memcpy( (char *)args + offsetof(struct wmt_params32_query_display, ret),
            &zero8, sizeof(zero8) );
    memset( &metadata, 0, sizeof(metadata) );
    if (!params.hdr_metadata.high) copy_to_wire( params.hdr_metadata, &metadata, sizeof(metadata) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_archive( void *args )
{
    struct wmt_params32_archive params;

    memcpy( &params, args, sizeof(params) );
    params.ret_object = params.ret_error = 0;
    memcpy( (char *)args + offsetof(struct wmt_params32_archive, ret_object),
            &params.ret_object, sizeof(params.ret_object) );
    memcpy( (char *)args + offsetof(struct wmt_params32_archive, ret_error),
            &params.ret_error, sizeof(params.ret_error) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_archive_serialize( void *args )
{
    struct wmt_params32_archive_serialize params;
    uint64_t zero = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_archive_serialize, ret_error),
            &zero, sizeof(zero) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_cache_get( void *args )
{
    struct wmt_params32_cache_get params;
    uint64_t zero = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_cache_get, ret_data),
            &zero, sizeof(zero) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_sample_buffer_encoder( void *args )
{
    struct wmt_params32_sample_buffer_encoder params;
    uint64_t zero = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_sample_buffer_encoder, ret),
            &zero, sizeof(zero) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_shared_texture( void *args )
{
    struct wmt_params32_info_ret params;
    uint64_t zero64 = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_params32_info_ret, ret),
            &zero64, sizeof(zero64) );
    if (!params.info.high)
        copy_to_wire_offset( params.info, offsetof(struct wmt_texture_info32, gpu_resource_id),
                             &zero64, sizeof(zero64) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_resolve_counter_range( void *args )
{
    struct wmt_params32_resolve_counter_range params;

    memcpy( &params, args, sizeof(params) );
    if (!params.data_out.high && params.data_length <= WMT_DISPATCH_BLOB_MAX)
        zero_guest_range( params.data_out.low, params.data_length, WMT_DISPATCH_BLOB_MAX );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_air_initialize( void *args )
{
    struct wmt_air_initialize32 params;

    memcpy( &params, args, sizeof(params) );
    params.ret = -1;
    memcpy( (char *)args + offsetof(struct wmt_air_initialize32, ret),
            &params.ret, sizeof(params.ret) );
    if (params.shader) zero_guest_range( params.shader, sizeof(uint64_t), sizeof(uint64_t) );
    if (params.reflection)
        zero_guest_range( params.reflection, sizeof(struct wmt_air_reflection32),
                          sizeof(struct wmt_air_reflection32) );
    if (params.error) zero_guest_range( params.error, sizeof(uint64_t), sizeof(uint64_t) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_air_compile( void *args )
{
    struct wmt_air_compile32 params;

    memcpy( &params, args, sizeof(params) );
    params.ret = -1;
    memcpy( (char *)args + offsetof(struct wmt_air_compile32, ret),
            &params.ret, sizeof(params.ret) );
    if (params.bitcode) zero_guest_range( params.bitcode, sizeof(uint64_t), sizeof(uint64_t) );
    if (params.error) zero_guest_range( params.error, sizeof(uint64_t), sizeof(uint64_t) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_air_pipeline_compile( void *args )
{
    struct wmt_air_pipeline_compile32 params;

    memcpy( &params, args, sizeof(params) );
    params.ret = -1;
    memcpy( (char *)args + offsetof(struct wmt_air_pipeline_compile32, ret),
            &params.ret, sizeof(params.ret) );
    if (params.bitcode) zero_guest_range( params.bitcode, sizeof(uint64_t), sizeof(uint64_t) );
    if (params.error) zero_guest_range( params.error, sizeof(uint64_t), sizeof(uint64_t) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_air_get_bitcode( void *args )
{
    struct wmt_air_get_bitcode32 params;

    memcpy( &params, args, sizeof(params) );
    if (params.data_out)
        zero_guest_range( params.data_out, sizeof(struct wmt_air_compiled_bitcode32),
                          sizeof(struct wmt_air_compiled_bitcode32) );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_air_get_error( void *args )
{
    struct wmt_air_get_error32 params;
    uint32_t zero = 0;

    memcpy( &params, args, sizeof(params) );
    memcpy( (char *)args + offsetof(struct wmt_air_get_error32, ret_size),
            &zero, sizeof(zero) );
    if (params.buffer && params.buffer_size) zero_guest_range( params.buffer, 1, 1 );
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS wmt_unsupported_air_get_arguments( void *args )
{
    /* The pinned outer block carries no array counts, so bounded zeroing is impossible. */
    (void)args;
    return STATUS_NOT_SUPPORTED;
}
