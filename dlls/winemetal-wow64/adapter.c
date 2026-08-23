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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "winemetal_private.h"

static _Atomic uint64_t snapshot_live_bytes;

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
    if (status) return status;
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
    if (status) return status;
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

NTSTATUS wmt_call_114( void *args )
{
    struct wmt_params32_obj_u64_obj_ret wire;
    struct wmt_params_obj_u64_obj_ret native;
    NTSTATUS status;
    void *bytes = NULL;

    memcpy( &wire, args, sizeof(wire) );
    wire.ret = 0;
    memcpy( args, &wire, sizeof(wire) );
    if (wire.handle > UINT32_MAX || wire.arg > WMT_DISPATCH_BLOB_MAX)
        return STATUS_INVALID_PARAMETER;
    if (wire.arg && (status = wmt_snapshot_bytes( wire.handle, wire.arg, &bytes ))) return status;
    native.handle = (uintptr_t)bytes;
    native.arg = wire.arg;
    native.ret = 0;
    status = wmt_normal_call( WMT_CALL_DISPATCH_DATA, &native );
    wmt_snapshot_free( bytes, wire.arg );
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_obj_u64_obj_ret, ret),
                         &native.ret, sizeof(native.ret) );
    return status;
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
    if (!status) memcpy( (char *)args + offsetof(struct wmt_params32_cache_init, ret),
                         &native.ret, sizeof(native.ret) );
    return status;
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
