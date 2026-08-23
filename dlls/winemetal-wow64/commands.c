/*
 * Winemetal high-shadow Wow64 command graph snapshot and conversion
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

#include <stdlib.h>
#include <string.h>

#include "winemetal_private.h"

#define WMT_GRAPH_HASH_INITIAL 128u
#define WMT_GRAPH_HASH_MAX     (WMT_GRAPH_NODE_MAX * 2u)
#define WMT_WIRE_NODE_MAX      120u

struct graph_record
{
    uint64_t guest;
    uint32_t wire_size;
    uint32_t native_size;
    uint64_t payload_size;
    uint64_t native_offset;
    uint64_t payload_offset;
    union
    {
        uint64_t align;
        unsigned char bytes[WMT_WIRE_NODE_MAX];
    } wire;
};

struct command_graph
{
    struct graph_record *records;
    uint64_t *visited;
    unsigned char *native;
    uint32_t count;
    uint32_t capacity;
    uint32_t visited_capacity;
    uint64_t records_size;
    uint64_t visited_size;
    uint64_t native_size;
};

C_ASSERT( sizeof(struct graph_record) == 160 );
C_ASSERT( WMT_WIRE_NODE_MAX >= sizeof(struct wmt_blit_copy_texture_texture32) );
C_ASSERT( WMT_WIRE_NODE_MAX >= sizeof(struct wmt_render_mesh32) );

static uint64_t align_eight( uint64_t value )
{
    return (value + 7) & ~7ull;
}

static NTSTATUS copy_guest32( uint32_t guest, void *dst, uint32_t size )
{
    void *host;
    NTSTATUS status;

    if ((uint64_t)guest + size > UINT32_MAX + 1ull) return STATUS_INVALID_PARAMETER;
    if ((status = ntdll_wow64_guest32_to_host( guest, &host ))) return status;
    return ntdll_wow64_copy_from_user( dst, host, size );
}

static void hash_insert( uint64_t *visited, uint32_t capacity, uint32_t guest )
{
    uint32_t index = guest * 2654435761u & (capacity - 1);

    while (visited[index])
        index = (index + 1) & (capacity - 1);
    visited[index] = guest;
}

static NTSTATUS grow_visited( struct command_graph *graph )
{
    uint32_t capacity, i;
    uint64_t size;
    uint64_t *visited;
    NTSTATUS status;

    if (graph->visited_capacity >= WMT_GRAPH_HASH_MAX) return STATUS_INVALID_PARAMETER;
    capacity = graph->visited_capacity ? graph->visited_capacity * 2 : WMT_GRAPH_HASH_INITIAL;
    if (capacity > WMT_GRAPH_HASH_MAX) capacity = WMT_GRAPH_HASH_MAX;
    size = (uint64_t)capacity * sizeof(*graph->visited);
    if (graph->records_size + size + graph->native_size > WMT_GRAPH_BYTE_MAX)
        return STATUS_BUFFER_OVERFLOW;
    if ((status = wmt_snapshot_alloc( size, (void **)&visited ))) return status;
    memset( visited, 0, size );
    for (i = 0; i < graph->visited_capacity; ++i)
        if (graph->visited[i]) hash_insert( visited, capacity, graph->visited[i] );
    wmt_snapshot_free( graph->visited, graph->visited_size );
    graph->visited = visited;
    graph->visited_capacity = capacity;
    graph->visited_size = size;
    return STATUS_SUCCESS;
}

static NTSTATUS insert_visited( struct command_graph *graph, uint32_t guest )
{
    uint32_t index;
    NTSTATUS status;

    if (!graph->visited_capacity || (uint64_t)(graph->count + 1) * 2 > graph->visited_capacity)
        if ((status = grow_visited( graph ))) return status;
    index = guest * 2654435761u & (graph->visited_capacity - 1);
    while (graph->visited[index])
    {
        if (graph->visited[index] == guest) return STATUS_INVALID_PARAMETER;
        index = (index + 1) & (graph->visited_capacity - 1);
    }
    graph->visited[index] = guest;
    return STATUS_SUCCESS;
}

static NTSTATUS grow_records( struct command_graph *graph )
{
    uint32_t capacity;
    uint64_t size;
    void *records;
    NTSTATUS status;

    if (graph->capacity >= WMT_GRAPH_NODE_MAX) return STATUS_INVALID_PARAMETER;
    capacity = graph->capacity ? graph->capacity * 2 : 64;
    if (capacity > WMT_GRAPH_NODE_MAX) capacity = WMT_GRAPH_NODE_MAX;
    size = (uint64_t)capacity * sizeof(*graph->records);
    if (graph->visited_size + size + graph->native_size > WMT_GRAPH_BYTE_MAX)
        return STATUS_BUFFER_OVERFLOW;
    records = graph->records;
    if ((status = wmt_snapshot_realloc( records, graph->records_size, size, &records ))) return status;
    graph->records = records;
    graph->records_size = size;
    graph->capacity = capacity;
    return STATUS_SUCCESS;
}

static BOOL get_blit_layout( uint16_t type, uint32_t *wire_size, uint32_t *native_size )
{
    switch (type)
    {
    case 0:
        *wire_size = sizeof(struct wmt_cmd_base32);
        *native_size = sizeof(struct wmt_cmd_base);
        return TRUE;
    case 1:
        *wire_size = sizeof(struct wmt_blit_copy_buffer32);
        *native_size = sizeof(struct wmt_blit_copy_buffer);
        return TRUE;
    case 2:
        *wire_size = sizeof(struct wmt_blit_copy_buffer_texture32);
        *native_size = sizeof(struct wmt_blit_copy_buffer_texture);
        return TRUE;
    case 3:
        *wire_size = sizeof(struct wmt_blit_copy_texture_buffer32);
        *native_size = sizeof(struct wmt_blit_copy_texture_buffer);
        return TRUE;
    case 4:
        *wire_size = sizeof(struct wmt_blit_copy_texture_texture32);
        *native_size = sizeof(struct wmt_blit_copy_texture_texture);
        return TRUE;
    case 5:
    case 6:
    case 7:
        *wire_size = sizeof(struct wmt_blit_object32);
        *native_size = sizeof(struct wmt_blit_object);
        return TRUE;
    case 8:
        *wire_size = sizeof(struct wmt_blit_fill_buffer32);
        *native_size = sizeof(struct wmt_blit_fill_buffer);
        return TRUE;
    case 9:
        *wire_size = sizeof(struct wmt_blit_resolve_counters32);
        *native_size = sizeof(struct wmt_blit_resolve_counters);
        return TRUE;
    case 10:
        *wire_size = sizeof(struct wmt_blit_copy_buffer_texture_option32);
        *native_size = sizeof(struct wmt_blit_copy_buffer_texture_option);
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL get_render_layout( uint16_t type, uint32_t *wire_size, uint32_t *native_size )
{
    switch (type)
    {
    case 0:
        *wire_size = sizeof(struct wmt_cmd_base32);
        *native_size = sizeof(struct wmt_cmd_base);
        return TRUE;
    case 1:
        *wire_size = sizeof(struct wmt_render_use_resource32);
        *native_size = sizeof(struct wmt_render_use_resource);
        return TRUE;
    case 2: case 4: case 6: case 8:
        *wire_size = sizeof(struct wmt_render_set_buffer32);
        *native_size = sizeof(struct wmt_render_set_buffer);
        return TRUE;
    case 3: case 5: case 7: case 9:
        *wire_size = sizeof(struct wmt_render_set_buffer_offset32);
        *native_size = sizeof(struct wmt_render_set_buffer_offset);
        return TRUE;
    case 10:
        *wire_size = sizeof(struct wmt_render_set_texture32);
        *native_size = sizeof(struct wmt_render_set_texture);
        return TRUE;
    case 11:
        *wire_size = sizeof(struct wmt_render_set_bytes32);
        *native_size = sizeof(struct wmt_render_set_bytes);
        return TRUE;
    case 12:
        *wire_size = sizeof(struct wmt_render_rasterizer32);
        *native_size = sizeof(struct wmt_render_rasterizer);
        return TRUE;
    case 13:
        *wire_size = sizeof(struct wmt_render_viewports32);
        *native_size = sizeof(struct wmt_render_viewports);
        return TRUE;
    case 14:
        *wire_size = sizeof(struct wmt_render_scissors32);
        *native_size = sizeof(struct wmt_render_scissors);
        return TRUE;
    case 15:
        *wire_size = sizeof(struct wmt_render_object32);
        *native_size = sizeof(struct wmt_render_object);
        return TRUE;
    case 16:
        *wire_size = sizeof(struct wmt_render_fence32);
        *native_size = sizeof(struct wmt_render_fence);
        return TRUE;
    case 17:
        *wire_size = sizeof(struct wmt_render_blend32);
        *native_size = sizeof(struct wmt_render_blend);
        return TRUE;
    case 18:
        *wire_size = sizeof(struct wmt_render_visibility32);
        *native_size = sizeof(struct wmt_render_visibility);
        return TRUE;
    case 19:
        *wire_size = sizeof(struct wmt_render_draw32);
        *native_size = sizeof(struct wmt_render_draw);
        return TRUE;
    case 20:
        *wire_size = sizeof(struct wmt_render_draw_indexed32);
        *native_size = sizeof(struct wmt_render_draw_indexed);
        return TRUE;
    case 21:
        *wire_size = sizeof(struct wmt_render_draw_indirect32);
        *native_size = sizeof(struct wmt_render_draw_indirect);
        return TRUE;
    case 22:
        *wire_size = sizeof(struct wmt_render_draw_indexed_indirect32);
        *native_size = sizeof(struct wmt_render_draw_indexed_indirect);
        return TRUE;
    case 23:
        *wire_size = sizeof(struct wmt_render_mesh32);
        *native_size = sizeof(struct wmt_render_mesh);
        return TRUE;
    case 24:
        *wire_size = sizeof(struct wmt_render_mesh_indirect32);
        *native_size = sizeof(struct wmt_render_mesh_indirect);
        return TRUE;
    case 25:
        *wire_size = sizeof(struct wmt_render_barrier32);
        *native_size = sizeof(struct wmt_render_barrier);
        return TRUE;
    case 29:
        *wire_size = sizeof(struct wmt_render_geometry32);
        *native_size = sizeof(struct wmt_render_geometry);
        return TRUE;
    case 30:
        *wire_size = sizeof(struct wmt_render_geometry_indexed32);
        *native_size = sizeof(struct wmt_render_geometry_indexed);
        return TRUE;
    case 31:
        *wire_size = sizeof(struct wmt_render_geometry_indirect32);
        *native_size = sizeof(struct wmt_render_geometry_indirect);
        return TRUE;
    case 32:
        *wire_size = sizeof(struct wmt_render_geometry_indexed_indirect32);
        *native_size = sizeof(struct wmt_render_geometry_indexed_indirect);
        return TRUE;
    case 33: case 34:
        *wire_size = sizeof(struct wmt_render_fence32);
        *native_size = sizeof(struct wmt_render_fence);
        return TRUE;
    case 35:
        *wire_size = sizeof(struct wmt_render_viewport32);
        *native_size = sizeof(struct wmt_render_viewport);
        return TRUE;
    case 36:
        *wire_size = sizeof(struct wmt_render_scissor32);
        *native_size = sizeof(struct wmt_render_scissor);
        return TRUE;
    case 37:
        *wire_size = sizeof(struct wmt_render_tessellation32);
        *native_size = sizeof(struct wmt_render_tessellation);
        return TRUE;
    case 38:
        *wire_size = sizeof(struct wmt_render_tessellation_indexed32);
        *native_size = sizeof(struct wmt_render_tessellation_indexed);
        return TRUE;
    case 39:
        *wire_size = sizeof(struct wmt_render_tessellation_indirect32);
        *native_size = sizeof(struct wmt_render_tessellation_indirect);
        return TRUE;
    case 40:
        *wire_size = sizeof(struct wmt_render_tessellation_indexed_indirect32);
        *native_size = sizeof(struct wmt_render_tessellation_indexed_indirect);
        return TRUE;
    case 41:
        *wire_size = sizeof(struct wmt_render_tile_threads32);
        *native_size = sizeof(struct wmt_render_tile_threads);
        return TRUE;
    default:
        /* 26..28 are pinned unused values and fail closed too. */
        return FALSE;
    }
}

static NTSTATUS render_payload_size( const struct graph_record *record, uint64_t *size )
{
    const struct wmt_cmd_base32 *base = (const void *)record->wire.bytes;

    *size = 0;
    switch (base->type)
    {
    case 11:
    {
        const struct wmt_render_set_bytes32 *node = (const void *)record->wire.bytes;

        if (node->bytes.high) return STATUS_INVALID_PARAMETER;
        *size = node->length;
        break;
    }
    case 13:
    {
        const struct wmt_render_viewports32 *node = (const void *)record->wire.bytes;

        if (node->viewports.high) return STATUS_INVALID_PARAMETER;
        *size = (uint64_t)node->count * sizeof(struct wmt_viewport);
        break;
    }
    case 14:
    {
        const struct wmt_render_scissors32 *node = (const void *)record->wire.bytes;

        if (node->scissors.high) return STATUS_INVALID_PARAMETER;
        *size = (uint64_t)node->count * sizeof(struct wmt_scissor_rect);
        break;
    }
    }
    if (*size > WMT_GRAPH_BYTE_MAX) return STATUS_BUFFER_OVERFLOW;
    return STATUS_SUCCESS;
}

static NTSTATUS snapshot_graph( uint32_t head, BOOL render, struct command_graph *graph )
{
    struct wmt_cmd_base32 base;
    struct graph_record *record;
    uint64_t total;
    uint32_t guest = head;
    NTSTATUS status;

    memset( graph, 0, sizeof(*graph) );

    while (guest)
    {
        if (graph->count >= WMT_GRAPH_NODE_MAX)
        {
            status = STATUS_INVALID_PARAMETER;
            goto failed;
        }
        if ((status = insert_visited( graph, guest ))) goto failed;
        if (graph->count == graph->capacity && (status = grow_records( graph ))) goto failed;
        if ((status = copy_guest32( guest, &base, sizeof(base) ))) goto failed;
        if (base.next.high)
        {
            status = STATUS_INVALID_PARAMETER;
            goto failed;
        }

        record = &graph->records[graph->count];
        memset( record, 0, sizeof(*record) );
        record->guest = guest;
        if (!(render ? get_render_layout( base.type, &record->wire_size, &record->native_size )
                     : get_blit_layout( base.type, &record->wire_size, &record->native_size )))
        {
            status = STATUS_NOT_SUPPORTED;
            goto failed;
        }
        if (record->wire_size > sizeof(record->wire.bytes))
        {
            status = STATUS_BUFFER_OVERFLOW;
            goto failed;
        }
        if ((uint64_t)guest + record->wire_size > UINT32_MAX + 1ull)
        {
            status = STATUS_INVALID_PARAMETER;
            goto failed;
        }
        memcpy( record->wire.bytes, &base, sizeof(base) );
        if (record->wire_size > sizeof(base) &&
            (status = copy_guest32( guest + sizeof(base), record->wire.bytes + sizeof(base),
                                    record->wire_size - sizeof(base) )))
            goto failed;
        if (render && (status = render_payload_size( record, &record->payload_size ))) goto failed;

        record->native_offset = align_eight( graph->native_size );
        total = record->native_offset + record->native_size;
        if (total < record->native_offset)
        {
            status = STATUS_INTEGER_OVERFLOW;
            goto failed;
        }
        if (record->payload_size)
        {
            record->payload_offset = align_eight( total );
            total = record->payload_offset + record->payload_size;
            if (total < record->payload_offset)
            {
                status = STATUS_INTEGER_OVERFLOW;
                goto failed;
            }
        }
        graph->native_size = total;
        ++graph->count;
        if (graph->visited_size + graph->records_size + graph->native_size > WMT_GRAPH_BYTE_MAX)
        {
            status = STATUS_BUFFER_OVERFLOW;
            goto failed;
        }
        guest = base.next.low;
    }

    if (graph->native_size &&
        (status = wmt_snapshot_alloc( graph->native_size, (void **)&graph->native )))
        goto failed;
    if (graph->native) memset( graph->native, 0, graph->native_size );
    return STATUS_SUCCESS;

failed:
    wmt_snapshot_free( graph->records, graph->records_size );
    wmt_snapshot_free( graph->visited, graph->visited_size );
    memset( graph, 0, sizeof(*graph) );
    return status;
}

static void free_graph( struct command_graph *graph )
{
    wmt_snapshot_free( graph->native, graph->native_size );
    wmt_snapshot_free( graph->records, graph->records_size );
    wmt_snapshot_free( graph->visited, graph->visited_size );
    memset( graph, 0, sizeof(*graph) );
}

static void copy_size( const struct wmt_size *src, struct wmt_size *dst )
{
    dst->width = src->width;
    dst->height = src->height;
    dst->depth = src->depth;
}

static void copy_origin( const struct wmt_origin *src, struct wmt_origin *dst )
{
    dst->x = src->x;
    dst->y = src->y;
    dst->z = src->z;
}

static void convert_base( const struct wmt_cmd_base32 *src, struct wmt_cmd_base *dst,
                          struct wmt_cmd_base *next )
{
    dst->type = src->type;
    dst->reserved[0] = src->reserved[0];
    dst->reserved[1] = src->reserved[1];
    dst->reserved[2] = src->reserved[2];
    dst->next.ptr = next;
}

static void convert_blit_node( const struct graph_record *record, void *native,
                               struct wmt_cmd_base *next )
{
    const struct wmt_cmd_base32 *base = (const void *)record->wire.bytes;

    convert_base( base, native, next );
    switch (base->type)
    {
    case 0:
        break;
    case 1:
    {
        const struct wmt_blit_copy_buffer32 *src = (const void *)record->wire.bytes;
        struct wmt_blit_copy_buffer *dst = native;

        dst->src = src->src;
        dst->src_offset = src->src_offset;
        dst->dst = src->dst;
        dst->dst_offset = src->dst_offset;
        dst->copy_length = src->copy_length;
        break;
    }
    case 2:
    {
        const struct wmt_blit_copy_buffer_texture32 *src = (const void *)record->wire.bytes;
        struct wmt_blit_copy_buffer_texture *dst = native;

        dst->src = src->src;
        dst->src_offset = src->src_offset;
        dst->bytes_per_row = src->bytes_per_row;
        dst->bytes_per_image = src->bytes_per_image;
        copy_size( &src->size, &dst->size );
        dst->dst = src->dst;
        dst->slice = src->slice;
        dst->level = src->level;
        copy_origin( &src->origin, &dst->origin );
        break;
    }
    case 3:
    {
        const struct wmt_blit_copy_texture_buffer32 *src = (const void *)record->wire.bytes;
        struct wmt_blit_copy_texture_buffer *dst = native;

        dst->src = src->src;
        dst->slice = src->slice;
        dst->level = src->level;
        copy_origin( &src->origin, &dst->origin );
        copy_size( &src->size, &dst->size );
        dst->dst = src->dst;
        dst->offset = src->offset;
        dst->bytes_per_row = src->bytes_per_row;
        dst->bytes_per_image = src->bytes_per_image;
        break;
    }
    case 4:
    {
        const struct wmt_blit_copy_texture_texture32 *src = (const void *)record->wire.bytes;
        struct wmt_blit_copy_texture_texture *dst = native;

        dst->src = src->src;
        dst->src_slice = src->src_slice;
        dst->src_level = src->src_level;
        copy_origin( &src->src_origin, &dst->src_origin );
        copy_size( &src->src_size, &dst->src_size );
        dst->dst = src->dst;
        dst->dst_slice = src->dst_slice;
        dst->dst_level = src->dst_level;
        copy_origin( &src->dst_origin, &dst->dst_origin );
        break;
    }
    case 5: case 6: case 7:
    {
        const struct wmt_blit_object32 *src = (const void *)record->wire.bytes;
        struct wmt_blit_object *dst = native;

        dst->object = src->object;
        break;
    }
    case 8:
    {
        const struct wmt_blit_fill_buffer32 *src = (const void *)record->wire.bytes;
        struct wmt_blit_fill_buffer *dst = native;

        dst->buffer = src->buffer;
        dst->offset = src->offset;
        dst->length = src->length;
        dst->value = src->value;
        break;
    }
    case 9:
    {
        const struct wmt_blit_resolve_counters32 *src = (const void *)record->wire.bytes;
        struct wmt_blit_resolve_counters *dst = native;

        dst->sample_buffer = src->sample_buffer;
        dst->start = src->start;
        dst->length = src->length;
        dst->dst_buffer = src->dst_buffer;
        dst->dst_offset = src->dst_offset;
        break;
    }
    case 10:
    {
        const struct wmt_blit_copy_buffer_texture_option32 *src = (const void *)record->wire.bytes;
        struct wmt_blit_copy_buffer_texture_option *dst = native;

        dst->src = src->src;
        dst->src_offset = src->src_offset;
        dst->bytes_per_row = src->bytes_per_row;
        dst->bytes_per_image = src->bytes_per_image;
        copy_size( &src->size, &dst->size );
        dst->dst = src->dst;
        dst->slice = src->slice;
        dst->level = src->level;
        dst->options = src->options;
        copy_origin( &src->origin, &dst->origin );
        break;
    }
    }
}

static NTSTATUS copy_render_payload( const struct graph_record *record, unsigned char *arena )
{
    const struct wmt_cmd_base32 *base = (const void *)record->wire.bytes;
    unsigned char *payload = arena + record->payload_offset;
    void *host;
    uint32_t i;
    uint32_t guest = 0;
    NTSTATUS status;

    if (!record->payload_size) return STATUS_SUCCESS;
    switch (base->type)
    {
    case 11:
        guest = ((const struct wmt_render_set_bytes32 *)record->wire.bytes)->bytes.low;
        break;
    case 13:
        guest = ((const struct wmt_render_viewports32 *)record->wire.bytes)->viewports.low;
        break;
    case 14:
        guest = ((const struct wmt_render_scissors32 *)record->wire.bytes)->scissors.low;
        break;
    }
    if ((uint64_t)guest + record->payload_size > UINT32_MAX + 1ull)
        return STATUS_INVALID_PARAMETER;
    if (base->type == 11)
    {
        if ((status = ntdll_wow64_guest32_to_host( guest, &host ))) return status;
        return ntdll_wow64_copy_from_user( payload, host, record->payload_size );
    }
    if (base->type == 13)
    {
        struct wmt_viewport wire, *native = (void *)payload;
        uint32_t count = ((const struct wmt_render_viewports32 *)record->wire.bytes)->count;

        for (i = 0; i < count; ++i)
        {
            if ((status = copy_guest32( guest + i * sizeof(wire), &wire, sizeof(wire) )))
                return status;
            native[i].origin_x = wire.origin_x;
            native[i].origin_y = wire.origin_y;
            native[i].width = wire.width;
            native[i].height = wire.height;
            native[i].znear = wire.znear;
            native[i].zfar = wire.zfar;
        }
        return STATUS_SUCCESS;
    }
    if (base->type == 14)
    {
        struct wmt_scissor_rect wire, *native = (void *)payload;
        uint32_t count = ((const struct wmt_render_scissors32 *)record->wire.bytes)->count;

        for (i = 0; i < count; ++i)
        {
            if ((status = copy_guest32( guest + i * sizeof(wire), &wire, sizeof(wire) )))
                return status;
            native[i].x = wire.x;
            native[i].y = wire.y;
            native[i].width = wire.width;
            native[i].height = wire.height;
        }
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

static void convert_render_node( const struct graph_record *record, unsigned char *arena,
                                 struct wmt_cmd_base *next )
{
    const struct wmt_cmd_base32 *base = (const void *)record->wire.bytes;
    void *native = arena + record->native_offset;

    convert_base( base, native, next );
    switch (base->type)
    {
    case 0:
        break;
    case 1:
    {
        const struct wmt_render_use_resource32 *src = (const void *)record->wire.bytes;
        struct wmt_render_use_resource *dst = native;

        dst->resource = src->resource;
        dst->usage = src->usage;
        dst->stages = src->stages;
        break;
    }
    case 2: case 4: case 6: case 8:
    {
        const struct wmt_render_set_buffer32 *src = (const void *)record->wire.bytes;
        struct wmt_render_set_buffer *dst = native;

        dst->buffer = src->buffer;
        dst->offset = src->offset;
        dst->index = src->index;
        break;
    }
    case 3: case 5: case 7: case 9:
    {
        const struct wmt_render_set_buffer_offset32 *src = (const void *)record->wire.bytes;
        struct wmt_render_set_buffer_offset *dst = native;

        dst->offset = src->offset;
        dst->index = src->index;
        break;
    }
    case 10:
    {
        const struct wmt_render_set_texture32 *src = (const void *)record->wire.bytes;
        struct wmt_render_set_texture *dst = native;

        dst->texture = src->texture;
        dst->index = src->index;
        break;
    }
    case 11:
    {
        const struct wmt_render_set_bytes32 *src = (const void *)record->wire.bytes;
        struct wmt_render_set_bytes *dst = native;

        dst->bytes.ptr = record->payload_size ? arena + record->payload_offset : NULL;
        dst->length = src->length;
        dst->index = src->index;
        break;
    }
    case 12:
    {
        const struct wmt_render_rasterizer32 *src = (const void *)record->wire.bytes;
        struct wmt_render_rasterizer *dst = native;

        dst->fill_mode = src->fill_mode;
        dst->cull_mode = src->cull_mode;
        dst->depth_clip_mode = src->depth_clip_mode;
        dst->winding = src->winding;
        dst->depth_bias = src->depth_bias;
        dst->slope_scale = src->slope_scale;
        dst->depth_bias_clamp = src->depth_bias_clamp;
        break;
    }
    case 13:
    {
        const struct wmt_render_viewports32 *src = (const void *)record->wire.bytes;
        struct wmt_render_viewports *dst = native;

        dst->viewports.ptr = record->payload_size ? arena + record->payload_offset : NULL;
        dst->count = src->count;
        break;
    }
    case 14:
    {
        const struct wmt_render_scissors32 *src = (const void *)record->wire.bytes;
        struct wmt_render_scissors *dst = native;

        dst->scissors.ptr = record->payload_size ? arena + record->payload_offset : NULL;
        dst->count = src->count;
        break;
    }
    case 15:
    {
        const struct wmt_render_object32 *src = (const void *)record->wire.bytes;
        struct wmt_render_object *dst = native;

        dst->object = src->object;
        break;
    }
    case 16: case 33: case 34:
    {
        const struct wmt_render_fence32 *src = (const void *)record->wire.bytes;
        struct wmt_render_fence *dst = native;

        dst->fence = src->fence;
        dst->stages = src->stages;
        break;
    }
    case 17:
    {
        const struct wmt_render_blend32 *src = (const void *)record->wire.bytes;
        struct wmt_render_blend *dst = native;

        dst->red = src->red;
        dst->green = src->green;
        dst->blue = src->blue;
        dst->alpha = src->alpha;
        dst->stencil_ref = src->stencil_ref;
        break;
    }
    case 18:
    {
        const struct wmt_render_visibility32 *src = (const void *)record->wire.bytes;
        struct wmt_render_visibility *dst = native;

        dst->offset = src->offset;
        dst->mode = src->mode;
        break;
    }
    case 19:
    {
        const struct wmt_render_draw32 *src = (const void *)record->wire.bytes;
        struct wmt_render_draw *dst = native;

        dst->primitive_type = src->primitive_type;
        dst->vertex_start = src->vertex_start;
        dst->vertex_count = src->vertex_count;
        dst->instance_count = src->instance_count;
        dst->base_instance = src->base_instance;
        break;
    }
    case 20:
    {
        const struct wmt_render_draw_indexed32 *src = (const void *)record->wire.bytes;
        struct wmt_render_draw_indexed *dst = native;

        dst->primitive_type = src->primitive_type;
        dst->index_type = src->index_type;
        dst->index_count = src->index_count;
        dst->index_buffer = src->index_buffer;
        dst->index_buffer_offset = src->index_buffer_offset;
        dst->instance_count = src->instance_count;
        dst->base_vertex = src->base_vertex;
        dst->base_instance = src->base_instance;
        break;
    }
    case 21:
    {
        const struct wmt_render_draw_indirect32 *src = (const void *)record->wire.bytes;
        struct wmt_render_draw_indirect *dst = native;

        dst->primitive_type = src->primitive_type;
        dst->indirect_args_buffer = src->indirect_args_buffer;
        dst->indirect_args_offset = src->indirect_args_offset;
        break;
    }
    case 22:
    {
        const struct wmt_render_draw_indexed_indirect32 *src = (const void *)record->wire.bytes;
        struct wmt_render_draw_indexed_indirect *dst = native;

        dst->primitive_type = src->primitive_type;
        dst->index_type = src->index_type;
        dst->index_buffer = src->index_buffer;
        dst->index_buffer_offset = src->index_buffer_offset;
        dst->indirect_args_buffer = src->indirect_args_buffer;
        dst->indirect_args_offset = src->indirect_args_offset;
        break;
    }
    case 23:
    {
        const struct wmt_render_mesh32 *src = (const void *)record->wire.bytes;
        struct wmt_render_mesh *dst = native;

        copy_size( &src->threadgroups_per_grid, &dst->threadgroups_per_grid );
        copy_size( &src->object_threadgroup_size, &dst->object_threadgroup_size );
        copy_size( &src->mesh_threadgroup_size, &dst->mesh_threadgroup_size );
        break;
    }
    case 24:
    {
        const struct wmt_render_mesh_indirect32 *src = (const void *)record->wire.bytes;
        struct wmt_render_mesh_indirect *dst = native;

        dst->indirect_args_buffer = src->indirect_args_buffer;
        dst->indirect_args_offset = src->indirect_args_offset;
        copy_size( &src->object_threadgroup_size, &dst->object_threadgroup_size );
        copy_size( &src->mesh_threadgroup_size, &dst->mesh_threadgroup_size );
        break;
    }
    case 25:
    {
        const struct wmt_render_barrier32 *src = (const void *)record->wire.bytes;
        struct wmt_render_barrier *dst = native;

        dst->scope = src->scope;
        dst->stages_before = src->stages_before;
        dst->stages_after = src->stages_after;
        break;
    }
    case 29:
    {
        const struct wmt_render_geometry32 *src = (const void *)record->wire.bytes;
        struct wmt_render_geometry *dst = native;

        dst->draw_arguments_offset = src->draw_arguments_offset;
        dst->warp_count = src->warp_count;
        dst->instance_count = src->instance_count;
        dst->vertices_per_warp = src->vertices_per_warp;
        break;
    }
    case 30:
    {
        const struct wmt_render_geometry_indexed32 *src = (const void *)record->wire.bytes;
        struct wmt_render_geometry_indexed *dst = native;

        dst->draw_arguments_offset = src->draw_arguments_offset;
        dst->index_buffer = src->index_buffer;
        dst->index_buffer_offset = src->index_buffer_offset;
        dst->warp_count = src->warp_count;
        dst->instance_count = src->instance_count;
        dst->vertices_per_warp = src->vertices_per_warp;
        break;
    }
    case 31:
    {
        const struct wmt_render_geometry_indirect32 *src = (const void *)record->wire.bytes;
        struct wmt_render_geometry_indirect *dst = native;

        dst->immediate_draw_arguments = src->immediate_draw_arguments;
        dst->indirect_args_buffer = src->indirect_args_buffer;
        dst->indirect_args_offset = src->indirect_args_offset;
        dst->dispatch_args_buffer = src->dispatch_args_buffer;
        dst->dispatch_args_offset = src->dispatch_args_offset;
        dst->vertices_per_warp = src->vertices_per_warp;
        break;
    }
    case 32:
    {
        const struct wmt_render_geometry_indexed_indirect32 *src = (const void *)record->wire.bytes;
        struct wmt_render_geometry_indexed_indirect *dst = native;

        dst->index_buffer = src->index_buffer;
        dst->index_buffer_offset = src->index_buffer_offset;
        dst->immediate_draw_arguments = src->immediate_draw_arguments;
        dst->indirect_args_buffer = src->indirect_args_buffer;
        dst->indirect_args_offset = src->indirect_args_offset;
        dst->dispatch_args_buffer = src->dispatch_args_buffer;
        dst->dispatch_args_offset = src->dispatch_args_offset;
        dst->vertices_per_warp = src->vertices_per_warp;
        break;
    }
    case 35:
    {
        const struct wmt_render_viewport32 *src = (const void *)record->wire.bytes;
        struct wmt_render_viewport *dst = native;

        dst->viewport.origin_x = src->viewport.origin_x;
        dst->viewport.origin_y = src->viewport.origin_y;
        dst->viewport.width = src->viewport.width;
        dst->viewport.height = src->viewport.height;
        dst->viewport.znear = src->viewport.znear;
        dst->viewport.zfar = src->viewport.zfar;
        break;
    }
    case 36:
    {
        const struct wmt_render_scissor32 *src = (const void *)record->wire.bytes;
        struct wmt_render_scissor *dst = native;

        dst->scissor.x = src->scissor.x;
        dst->scissor.y = src->scissor.y;
        dst->scissor.width = src->scissor.width;
        dst->scissor.height = src->scissor.height;
        break;
    }
    case 37:
    {
        const struct wmt_render_tessellation32 *src = (const void *)record->wire.bytes;
        struct wmt_render_tessellation *dst = native;

        dst->draw_arguments_offset = src->draw_arguments_offset;
        dst->instance_count = src->instance_count;
        dst->threads_per_patch = src->threads_per_patch;
        dst->patches_per_group = src->patches_per_group;
        dst->patches_per_mesh_instance = src->patches_per_mesh_instance;
        break;
    }
    case 38:
    {
        const struct wmt_render_tessellation_indexed32 *src = (const void *)record->wire.bytes;
        struct wmt_render_tessellation_indexed *dst = native;

        dst->draw_arguments_offset = src->draw_arguments_offset;
        dst->index_buffer = src->index_buffer;
        dst->index_buffer_offset = src->index_buffer_offset;
        dst->instance_count = src->instance_count;
        dst->threads_per_patch = src->threads_per_patch;
        dst->patches_per_group = src->patches_per_group;
        dst->patches_per_mesh_instance = src->patches_per_mesh_instance;
        break;
    }
    case 39:
    {
        const struct wmt_render_tessellation_indirect32 *src = (const void *)record->wire.bytes;
        struct wmt_render_tessellation_indirect *dst = native;

        dst->immediate_draw_arguments = src->immediate_draw_arguments;
        dst->indirect_args_buffer = src->indirect_args_buffer;
        dst->indirect_args_offset = src->indirect_args_offset;
        dst->dispatch_args_buffer = src->dispatch_args_buffer;
        dst->dispatch_args_offset = src->dispatch_args_offset;
        dst->threads_per_patch = src->threads_per_patch;
        dst->patches_per_group = src->patches_per_group;
        break;
    }
    case 40:
    {
        const struct wmt_render_tessellation_indexed_indirect32 *src = (const void *)record->wire.bytes;
        struct wmt_render_tessellation_indexed_indirect *dst = native;

        dst->immediate_draw_arguments = src->immediate_draw_arguments;
        dst->indirect_args_buffer = src->indirect_args_buffer;
        dst->indirect_args_offset = src->indirect_args_offset;
        dst->dispatch_args_buffer = src->dispatch_args_buffer;
        dst->dispatch_args_offset = src->dispatch_args_offset;
        dst->index_buffer = src->index_buffer;
        dst->index_buffer_offset = src->index_buffer_offset;
        dst->threads_per_patch = src->threads_per_patch;
        dst->patches_per_group = src->patches_per_group;
        break;
    }
    case 41:
    {
        const struct wmt_render_tile_threads32 *src = (const void *)record->wire.bytes;
        struct wmt_render_tile_threads *dst = native;

        dst->width = src->width;
        dst->height = src->height;
        break;
    }
    }
}

static NTSTATUS prepare_graph( uint32_t head, BOOL render, struct command_graph *graph )
{
    struct wmt_cmd_base *next;
    struct graph_record *record;
    uint32_t i;
    NTSTATUS status;

    if ((status = snapshot_graph( head, render, graph ))) return status;
    for (i = 0; i < graph->count; ++i)
    {
        record = &graph->records[i];
        next = i + 1 < graph->count
             ? (struct wmt_cmd_base *)(graph->native + graph->records[i + 1].native_offset) : NULL;
        if (render)
        {
            if ((status = copy_render_payload( record, graph->native )))
            {
                free_graph( graph );
                return status;
            }
            convert_render_node( record, graph->native, next );
        }
        else
            convert_blit_node( record, graph->native + record->native_offset, next );
    }
    return STATUS_SUCCESS;
}

static NTSTATUS call_command_graph( unsigned int index, BOOL render, void *args )
{
    struct wmt_params32_command wire;
    struct wmt_params_command native;
    struct command_graph graph;
    NTSTATUS status;

    memcpy( &wire, args, sizeof(wire) );
    if (wire.command_head.high) return STATUS_INVALID_PARAMETER;
    if ((status = prepare_graph( wire.command_head.low, render, &graph ))) return status;
    native.encoder = wire.encoder;
    native.command_head.ptr = graph.count ? graph.native + graph.records[0].native_offset : NULL;
    status = wmt_normal_call( index, &native );
    free_graph( &graph );
    return status;
}

NTSTATUS wmt_call_36( void *args )
{
    return call_command_graph( WMT_CALL_BLIT_COMMANDS, FALSE, args );
}

NTSTATUS wmt_call_38( void *args )
{
    return call_command_graph( WMT_CALL_RENDER_COMMANDS, TRUE, args );
}
