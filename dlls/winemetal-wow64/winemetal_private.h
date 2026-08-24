/*
 * Winemetal high-shadow Wow64 companion ABI
 *
 * Copyright (c) 2023-2026 Feifan He for CodeWeavers
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINEMETAL_WOW64_PRIVATE_H
#define __WINEMETAL_WOW64_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

#define WMT_UNIX_CALL_COUNT              138u
#define WMT_STRING_MAX                   (1024u * 1024u)
#define WMT_GRAPH_NODE_MAX               65536u
#define WMT_GRAPH_BYTE_MAX               (16u * 1024u * 1024u)
#define WMT_CONSTANT_COUNT_MAX           4096u
#define WMT_CONSTANT_BYTE_MAX            (1024u * 1024u)
#define WMT_DISPATCH_BLOB_MAX            (64u * 1024u * 1024u)
#define WMT_SNAPSHOT_LIVE_MAX            (256u * 1024u * 1024u)
#define WMT_AIR_GRAPH_MAX                 (16u * 1024u * 1024u)
#define WMT_AIR_ARGUMENT_MAX              4096u

typedef uint64_t wmt_handle_t;

/*
 * These schema-only declarations mirror the public Winemetal ABI in pinned
 * DXMT 856d9f3 (winemetal.h and winemetal_thunks.h).  They intentionally do
 * not include or link the external checkout: the companion must compile from
 * the Wine source tree and describe both the i386 wire and native ARM64 ABI.
 */

/* Pinned DXMT encodes an i386 pointer as low/high 32-bit words. */
struct wmt_wire_ptr
{
    uint32_t low;
    uint32_t high;
};

struct wmt_native_ptr
{
    void *ptr;
};

struct wmt_native_const_ptr
{
    const void *ptr;
};

struct wmt_size
{
    uint64_t width;
    uint64_t height;
    uint64_t depth;
};

struct wmt_origin
{
    uint64_t x;
    uint64_t y;
    uint64_t z;
};

struct wmt_viewport
{
    double origin_x;
    double origin_y;
    double width;
    double height;
    double znear;
    double zfar;
};

struct wmt_scissor_rect
{
    uint64_t x;
    uint64_t y;
    uint64_t width;
    uint64_t height;
};

#pragma pack(push, 4)

struct wmt_buffer_info32
{
    uint64_t length;
    uint64_t options;
    struct wmt_wire_ptr memory;
    uint64_t gpu_address;
};

struct wmt_sampler_info32
{
    uint8_t min_filter;
    uint8_t mag_filter;
    uint8_t mip_filter;
    uint8_t r_address_mode;
    uint8_t s_address_mode;
    uint8_t t_address_mode;
    uint8_t border_color;
    uint8_t compare_function;
    float lod_min_clamp;
    float lod_max_clamp;
    uint32_t max_anisotropy;
    uint8_t normalized_coords;
    uint8_t lod_average;
    uint8_t support_argument_buffers;
    uint8_t reserved;
    uint64_t gpu_resource_id;
};

struct wmt_stencil_info32
{
    uint8_t enabled;
    uint8_t depth_stencil_pass_op;
    uint8_t stencil_fail_op;
    uint8_t depth_fail_op;
    uint8_t stencil_compare_function;
    uint8_t write_mask;
    uint8_t read_mask;
};

struct wmt_depth_stencil_info32
{
    uint8_t depth_compare_function;
    uint8_t depth_write_enabled;
    struct wmt_stencil_info32 front_stencil;
    struct wmt_stencil_info32 back_stencil;
};

struct wmt_texture_info32
{
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_length;
    uint32_t type_levels_samples_usage;
    uint64_t options;
    uint32_t reserved;
    uint32_t mach_port;
    uint64_t gpu_resource_id;
};

struct wmt_compute_pipeline_info32
{
    uint64_t compute_function;
    struct wmt_wire_ptr binary_archives_for_lookup;
    uint64_t binary_archive_for_serialization;
    uint8_t num_binary_archives_for_lookup;
    uint8_t fail_on_binary_archive_miss;
    uint8_t padding;
    uint8_t tgsize_is_multiple_of_sgwidth;
    uint32_t immutable_buffers;
};

struct wmt_clear_color32
{
    double r;
    double g;
    double b;
    double a;
};

struct wmt_color_attachment_info32
{
    uint64_t texture;
    uint32_t load_action;
    uint32_t store_action;
    uint16_t level;
    uint16_t slice;
    uint32_t depth_plane;
    struct wmt_clear_color32 clear_color;
    uint64_t resolve_texture;
    uint16_t resolve_level;
    uint16_t resolve_slice;
    uint32_t resolve_depth_plane;
};

struct wmt_depth_attachment_info32
{
    uint64_t texture;
    uint32_t load_action;
    uint32_t store_action;
    uint16_t level;
    uint16_t slice;
    uint32_t depth_plane;
    float clear_depth;
};

struct wmt_stencil_attachment_info32
{
    uint64_t texture;
    uint32_t load_action;
    uint32_t store_action;
    uint16_t level;
    uint16_t slice;
    uint32_t depth_plane;
    uint8_t clear_stencil;
};

struct wmt_render_pass_info32
{
    struct wmt_color_attachment_info32 colors[8];
    struct wmt_depth_attachment_info32 depth;
    struct wmt_stencil_attachment_info32 stencil;
    uint8_t default_raster_sample_count;
    uint8_t render_target_array_length;
    uint8_t tile_width;
    uint8_t tile_height;
    uint32_t render_target_height;
    uint32_t render_target_width;
    uint64_t visibility_buffer;
};

struct wmt_color_attachment_blend_info32
{
    uint32_t pixel_format;
    uint8_t rgb_blend_operation;
    uint8_t alpha_blend_operation;
    uint8_t src_rgb_blend_factor;
    uint8_t dst_rgb_blend_factor;
    uint8_t src_alpha_blend_factor;
    uint8_t dst_alpha_blend_factor;
    uint8_t write_mask;
    uint8_t blending_enabled;
};

struct wmt_render_pipeline_info32
{
    struct wmt_color_attachment_blend_info32 colors[8];
    uint8_t alpha_to_coverage_enabled;
    uint8_t logic_operation_enabled;
    uint8_t logic_operation;
    uint8_t rasterization_enabled;
    uint8_t raster_sample_count;
    uint32_t depth_pixel_format;
    uint32_t stencil_pixel_format;
    uint64_t vertex_function;
    uint64_t fragment_function;
    uint32_t immutable_vertex_buffers;
    uint32_t immutable_fragment_buffers;
    uint32_t input_primitive_topology;
    uint8_t tessellation_partition_mode;
    uint8_t max_tessellation_factor;
    uint8_t tessellation_output_winding_order;
    uint8_t tessellation_factor_step;
    uint64_t binary_archive_for_serialization;
    struct wmt_wire_ptr binary_archives_for_lookup;
    uint8_t num_binary_archives_for_lookup;
    uint8_t fail_on_binary_archive_miss;
    uint8_t padding[6];
};

struct wmt_layer_props32
{
    uint64_t device;
    double contents_scale;
    double drawable_width;
    double drawable_height;
    uint8_t opaque;
    uint8_t display_sync_enabled;
    uint8_t framebuffer_only;
    uint32_t pixel_format;
};

struct wmt_edr_value32
{
    float maximum_edr_color_component_value;
    float maximum_potential_edr_color_component_value;
};

struct wmt_function_constant32
{
    struct wmt_wire_ptr data;
    uint16_t type;
    uint16_t index;
    uint32_t reserved;
};

struct wmt_hdr_metadata32
{
    uint16_t red_primary[2];
    uint16_t green_primary[2];
    uint16_t blue_primary[2];
    uint16_t white_point[2];
    uint32_t max_mastering_luminance;
    uint32_t min_mastering_luminance;
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
};

struct wmt_cmd_base32
{
    uint16_t type;
    uint16_t reserved[3];
    struct wmt_wire_ptr next;
};

struct wmt_blit_copy_buffer32
{
    struct wmt_cmd_base32 base;
    uint64_t src;
    uint64_t src_offset;
    uint64_t dst;
    uint64_t dst_offset;
    uint64_t copy_length;
};

struct wmt_blit_copy_buffer_texture32
{
    struct wmt_cmd_base32 base;
    uint64_t src;
    uint64_t src_offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
    struct wmt_size size;
    uint64_t dst;
    uint32_t slice;
    uint32_t level;
    struct wmt_origin origin;
};

struct wmt_blit_copy_buffer_texture_option32
{
    struct wmt_cmd_base32 base;
    uint64_t src;
    uint64_t src_offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
    struct wmt_size size;
    uint64_t dst;
    uint32_t slice;
    uint16_t level;
    uint16_t options;
    struct wmt_origin origin;
};

struct wmt_blit_copy_texture_texture32
{
    struct wmt_cmd_base32 base;
    uint64_t src;
    uint32_t src_slice;
    uint32_t src_level;
    struct wmt_origin src_origin;
    struct wmt_size src_size;
    uint64_t dst;
    uint32_t dst_slice;
    uint32_t dst_level;
    struct wmt_origin dst_origin;
};

struct wmt_blit_copy_texture_buffer32
{
    struct wmt_cmd_base32 base;
    uint64_t src;
    uint32_t slice;
    uint32_t level;
    struct wmt_origin origin;
    struct wmt_size size;
    uint64_t dst;
    uint64_t offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
};

struct wmt_blit_object32
{
    struct wmt_cmd_base32 base;
    uint64_t object;
};

struct wmt_blit_fill_buffer32
{
    struct wmt_cmd_base32 base;
    uint64_t buffer;
    uint64_t offset;
    uint64_t length;
    uint8_t value;
};

struct wmt_blit_resolve_counters32
{
    struct wmt_cmd_base32 base;
    uint64_t sample_buffer;
    uint32_t start;
    uint32_t length;
    uint64_t dst_buffer;
    uint64_t dst_offset;
};

struct wmt_params32_obj_ptr
{
    uint64_t handle;
    struct wmt_wire_ptr arg;
};

struct wmt_params32_obj_u64_obj_ret
{
    uint64_t handle;
    uint64_t arg;
    uint64_t ret;
};

struct wmt_params32_info_ret
{
    uint64_t object;
    struct wmt_wire_ptr info;
    uint64_t ret;
};

struct wmt_params32_pipeline
{
    uint64_t device;
    struct wmt_wire_ptr info;
    uint64_t ret_error;
    uint64_t ret_pso;
};

struct wmt_params32_command
{
    uint64_t encoder;
    struct wmt_wire_ptr command_head;
};

struct wmt_params32_string
{
    struct wmt_wire_ptr buffer;
    uint64_t encoding;
    uint64_t ret;
};

struct wmt_params32_function_constants
{
    uint64_t library;
    struct wmt_wire_ptr name;
    struct wmt_wire_ptr constants;
    uint64_t count;
    uint64_t ret;
    uint64_t ret_error;
};

struct wmt_params32_query_display_layer
{
    uint64_t layer;
    uint64_t version;
    uint64_t colorspace;
    struct wmt_wire_ptr hdr_metadata;
    struct wmt_edr_value32 edr_value;
};

struct wmt_params32_cache_init
{
    struct wmt_wire_ptr path;
    uint64_t version;
    uint64_t ret;
};

struct wmt_params32_set_cache_path
{
    struct wmt_wire_ptr path;
    uint64_t ret_success;
};

struct wmt_params32_getcstring
{
    uint64_t string;
    uint64_t buffer;
    uint64_t max_length;
    uint64_t encoding;
    uint32_t ret;
};

struct wmt_params32_buffer_texture
{
    uint64_t buffer;
    struct wmt_wire_ptr info;
    uint64_t offset;
    uint64_t bytes_per_row;
    uint64_t ret;
};

struct wmt_params32_capture
{
    uint64_t manager;
    struct wmt_wire_ptr info;
    uint8_t ret;
};

struct wmt_params32_enumerate
{
    uint64_t enumerable;
    uint64_t start;
    uint64_t buffer_size;
    struct wmt_wire_ptr buffer;
    uint64_t ret_read;
};

struct wmt_display_description32
{
    float red_primaries[2];
    float blue_primaries[2];
    float green_primaries[2];
    float white_points[2];
    float maximum_edr_color_component_value;
    float maximum_potential_edr_color_component_value;
    float maximum_reference_edr_color_component_value;
};

struct wmt_params32_query_display
{
    uint64_t display_id;
    uint64_t colorspace;
    struct wmt_wire_ptr hdr_metadata;
    uint8_t ret;
};

struct wmt_params32_archive
{
    uint64_t object;
    struct wmt_wire_ptr path;
    uint64_t ret_object;
    uint64_t ret_error;
};

struct wmt_params32_archive_serialize
{
    uint64_t archive;
    struct wmt_wire_ptr path;
    uint64_t ret_error;
};

struct wmt_params32_cache_get
{
    uint64_t cache;
    struct wmt_wire_ptr key;
    uint64_t key_length;
    uint64_t ret_data;
};

struct wmt_params32_sample_buffer_encoder
{
    uint64_t command_buffer;
    struct wmt_wire_ptr attachments;
    uint64_t count;
    uint64_t ret;
};

struct wmt_params32_resolve_counter_range
{
    uint64_t sample_buffer;
    uint32_t start;
    uint32_t length;
    struct wmt_wire_ptr data_out;
    uint64_t data_length;
};

struct wmt_params32_texture_replace
{
    uint64_t texture;
    struct wmt_origin origin;
    struct wmt_size size;
    uint64_t level;
    uint64_t slice;
    struct wmt_wire_ptr data;
    uint64_t bytes_per_row;
    uint64_t bytes_per_image;
};

struct wmt_params32_buffer_update
{
    uint64_t buffer;
    uint64_t offset;
    struct wmt_wire_ptr data;
    uint64_t length;
};

#pragma pack(pop)

/* Pinned AIR thunk32 outer blocks.  Guest pointers remain explicit uint32_t. */
struct wmt_air_initialize32
{
    uint32_t bytecode;
    uint32_t bytecode_size;
    uint32_t shader;
    uint32_t reflection;
    uint32_t error;
    int32_t ret;
};

struct wmt_air_compile32
{
    uint64_t shader;
    uint32_t arguments;
    uint32_t function_name;
    uint32_t bitcode;
    uint32_t error;
    int32_t ret;
};

struct wmt_air_pipeline_compile32
{
    uint64_t first_shader;
    uint64_t second_shader;
    uint32_t arguments;
    uint32_t function_name;
    uint32_t bitcode;
    uint32_t error;
    int32_t ret;
};

struct wmt_air_get_bitcode32
{
    uint64_t bitcode;
    uint32_t data_out;
};

struct wmt_air_get_error32
{
    uint64_t error;
    uint32_t buffer;
    uint32_t buffer_size;
    uint32_t ret_size;
};

struct wmt_air_get_arguments32
{
    uint64_t shader;
    uint32_t constant_buffers;
    uint32_t arguments;
};

struct wmt_air_reflection32
{
    uint32_t constant_buffer_table_bind_index;
    uint32_t argument_buffer_bind_index;
    uint32_t num_constant_buffers;
    uint32_t num_arguments;
    uint32_t stage_data[3];
    uint16_t constant_buffer_slot_mask;
    uint16_t sampler_slot_mask;
    uint64_t uav_slot_mask;
    uint64_t srv_slot_mask_hi;
    uint64_t srv_slot_mask_lo;
    uint32_t num_output_elements;
    uint32_t threads_per_patch;
    uint32_t argument_table_qwords;
};

struct wmt_air_compiled_bitcode32
{
    uint64_t data;
    uint64_t size;
};

#pragma pack(push, 4)
struct wmt_air_argument32
{
    uint32_t next;
    uint32_t type;
};

struct wmt_air_stream_output_argument32
{
    uint32_t next;
    uint32_t type;
    uint32_t num_output_slots;
    uint32_t num_elements;
    uint32_t strides[4];
    uint32_t elements;
};

struct wmt_air_common_argument32
{
    uint32_t next;
    uint32_t type;
    uint32_t metal_version;
    uint32_t flags;
};

struct wmt_air_pixel_argument32
{
    uint32_t next;
    uint32_t type;
    uint32_t sample_mask;
    uint8_t dual_source_blending;
    uint8_t disable_depth_output;
    uint8_t padding[2];
    uint32_t unorm_output_reg_mask;
};

struct wmt_air_input_layout_argument32
{
    uint32_t next;
    uint32_t type;
    uint32_t index_buffer_format;
    uint32_t slot_mask;
    uint32_t num_elements;
    uint32_t elements;
};

struct wmt_air_gs_passthrough_argument32
{
    uint32_t next;
    uint32_t type;
    uint32_t data;
    uint8_t rasterization_disabled;
    uint8_t padding[3];
};

struct wmt_air_bool_argument32
{
    uint32_t next;
    uint32_t type;
    uint8_t value;
    uint8_t padding[3];
};

struct wmt_air_u32_argument32
{
    uint32_t next;
    uint32_t type;
    uint32_t value;
};

struct wmt_air_root_signature_argument32
{
    uint32_t next;
    uint32_t type;
    uint32_t bytecode;
    uint32_t bytecode_length;
};
#pragma pack(pop)

struct wmt_air_stream_output_element
{
    uint32_t reg_id;
    uint32_t component;
    uint32_t output_slot;
    uint32_t offset;
};

struct wmt_air_input_element
{
    uint32_t reg;
    uint32_t slot;
    uint32_t aligned_byte_offset;
    uint32_t format;
    uint32_t step;
};

struct wmt_air_argument
{
    void *next;
    uint32_t type;
};

struct wmt_air_stream_output_argument
{
    void *next;
    uint32_t type;
    uint32_t num_output_slots;
    uint32_t num_elements;
    uint32_t strides[4];
    struct wmt_air_stream_output_element *elements;
};

struct wmt_air_common_argument
{
    void *next;
    uint32_t type;
    uint32_t metal_version;
    uint32_t flags;
};

struct wmt_air_pixel_argument
{
    void *next;
    uint32_t type;
    uint32_t sample_mask;
    bool dual_source_blending;
    bool disable_depth_output;
    uint32_t unorm_output_reg_mask;
};

struct wmt_air_input_layout_argument
{
    void *next;
    uint32_t type;
    uint32_t index_buffer_format;
    uint32_t slot_mask;
    uint32_t num_elements;
    struct wmt_air_input_element *elements;
};

struct wmt_air_gs_passthrough_argument
{
    void *next;
    uint32_t type;
    uint32_t data;
    bool rasterization_disabled;
};

struct wmt_air_bool_argument
{
    void *next;
    uint32_t type;
    bool value;
};

struct wmt_air_u32_argument
{
    void *next;
    uint32_t type;
    uint32_t value;
};

struct wmt_air_root_signature_argument
{
    void *next;
    uint32_t type;
    const void *bytecode;
    size_t bytecode_length;
};

struct wmt_air_reflection
{
    uint32_t constant_buffer_table_bind_index;
    uint32_t argument_buffer_bind_index;
    uint32_t num_constant_buffers;
    uint32_t num_arguments;
    uint32_t stage_data[3];
    uint16_t constant_buffer_slot_mask;
    uint16_t sampler_slot_mask;
    uint64_t uav_slot_mask;
    uint64_t srv_slot_mask_hi;
    uint64_t srv_slot_mask_lo;
    uint32_t num_output_elements;
    uint32_t threads_per_patch;
    uint32_t argument_table_qwords;
};

struct wmt_air_compiled_bitcode
{
    uint64_t data;
    uint64_t size;
};

struct wmt_air_initialize
{
    const void *bytecode;
    size_t bytecode_size;
    uint64_t *shader;
    struct wmt_air_reflection *reflection;
    uint64_t *error;
    int32_t ret;
};

struct wmt_air_compile
{
    uint64_t shader;
    struct wmt_air_argument *arguments;
    const char *function_name;
    uint64_t *bitcode;
    uint64_t *error;
    int32_t ret;
};

struct wmt_air_pipeline_compile
{
    uint64_t first_shader;
    uint64_t second_shader;
    struct wmt_air_argument *arguments;
    const char *function_name;
    uint64_t *bitcode;
    uint64_t *error;
    int32_t ret;
};

struct wmt_air_get_bitcode
{
    uint64_t bitcode;
    struct wmt_air_compiled_bitcode *data_out;
};

struct wmt_air_get_error
{
    uint64_t error;
    char *buffer;
    size_t buffer_size;
    size_t ret_size;
};

struct wmt_air_shader_argument
{
    uint32_t type;
    uint32_t slot;
    uint32_t flags;
    uint32_t structure_ptr_offset;
};

struct wmt_air_get_arguments
{
    uint64_t shader;
    struct wmt_air_shader_argument *constant_buffers;
    struct wmt_air_shader_argument *arguments;
};

C_ASSERT( sizeof(struct wmt_air_initialize32) == 24 );
C_ASSERT( sizeof(struct wmt_air_compile32) == 32 );
C_ASSERT( sizeof(struct wmt_air_pipeline_compile32) == 40 );
C_ASSERT( sizeof(struct wmt_air_get_bitcode32) == 16 );
C_ASSERT( sizeof(struct wmt_air_get_error32) == 24 );
C_ASSERT( sizeof(struct wmt_air_get_arguments32) == 16 );
C_ASSERT( sizeof(struct wmt_air_reflection32) == 72 );
C_ASSERT( sizeof(struct wmt_air_compiled_bitcode32) == 16 );
C_ASSERT( sizeof(struct wmt_air_stream_output_argument32) == 36 );
C_ASSERT( sizeof(struct wmt_air_common_argument32) == 16 );
C_ASSERT( sizeof(struct wmt_air_pixel_argument32) == 20 );
C_ASSERT( sizeof(struct wmt_air_input_layout_argument32) == 24 );
C_ASSERT( sizeof(struct wmt_air_gs_passthrough_argument32) == 16 );
C_ASSERT( sizeof(struct wmt_air_bool_argument32) == 12 );
C_ASSERT( sizeof(struct wmt_air_u32_argument32) == 12 );
C_ASSERT( sizeof(struct wmt_air_root_signature_argument32) == 16 );
C_ASSERT( sizeof(struct wmt_air_argument) == 16 );
C_ASSERT( sizeof(struct wmt_air_stream_output_argument) == 48 );
C_ASSERT( sizeof(struct wmt_air_common_argument) == 24 );
C_ASSERT( sizeof(struct wmt_air_pixel_argument) == 24 );
C_ASSERT( sizeof(struct wmt_air_input_layout_argument) == 32 );
C_ASSERT( sizeof(struct wmt_air_gs_passthrough_argument) == 24 );
C_ASSERT( sizeof(struct wmt_air_bool_argument) == 16 );
C_ASSERT( sizeof(struct wmt_air_u32_argument) == 16 );
C_ASSERT( sizeof(struct wmt_air_root_signature_argument) == 32 );
C_ASSERT( sizeof(struct wmt_air_stream_output_element) == 16 );
C_ASSERT( sizeof(struct wmt_air_input_element) == 20 );
C_ASSERT( sizeof(struct wmt_air_reflection) == 72 );
C_ASSERT( sizeof(struct wmt_air_initialize) == 48 );
C_ASSERT( sizeof(struct wmt_air_compile) == 48 );
C_ASSERT( sizeof(struct wmt_air_pipeline_compile) == 56 );
C_ASSERT( sizeof(struct wmt_air_get_bitcode) == 16 );
C_ASSERT( sizeof(struct wmt_air_get_error) == 32 );
C_ASSERT( sizeof(struct wmt_air_get_arguments) == 24 );
C_ASSERT( sizeof(struct wmt_air_shader_argument) == 16 );
C_ASSERT( sizeof(struct wmt_params32_texture_replace) == 96 );
C_ASSERT( sizeof(struct wmt_params32_buffer_update) == 32 );

struct wmt_buffer_info
{
    uint64_t length;
    uint64_t options;
    struct wmt_native_ptr memory;
    uint64_t gpu_address;
};

#define WMT_RESOURCE_STORAGE_MODE_MASK       0x00000000000000f0ull
#define WMT_RESOURCE_STORAGE_MODE_SHARED     0x0000000000000000ull
#define WMT_RESOURCE_STORAGE_MODE_MANAGED    0x0000000000000010ull
#define WMT_RESOURCE_STORAGE_MODE_PRIVATE    0x0000000000000020ull
#define WMT_RESOURCE_STORAGE_MODE_MEMORYLESS 0x0000000000000030ull

struct wmt_sampler_info
{
    uint8_t min_filter;
    uint8_t mag_filter;
    uint8_t mip_filter;
    uint8_t r_address_mode;
    uint8_t s_address_mode;
    uint8_t t_address_mode;
    uint8_t border_color;
    uint8_t compare_function;
    float lod_min_clamp;
    float lod_max_clamp;
    uint32_t max_anisotropy;
    bool normalized_coords;
    bool lod_average;
    bool support_argument_buffers;
    uint8_t reserved;
    uint64_t gpu_resource_id;
};

struct wmt_stencil_info
{
    bool enabled;
    uint8_t depth_stencil_pass_op;
    uint8_t stencil_fail_op;
    uint8_t depth_fail_op;
    uint8_t stencil_compare_function;
    uint8_t write_mask;
    uint8_t read_mask;
};

struct wmt_depth_stencil_info
{
    uint8_t depth_compare_function;
    bool depth_write_enabled;
    struct wmt_stencil_info front_stencil;
    struct wmt_stencil_info back_stencil;
};

struct wmt_texture_info
{
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_length;
    uint32_t type_levels_samples_usage;
    uint64_t options;
    uint32_t reserved;
    uint32_t mach_port;
    uint64_t gpu_resource_id;
};

struct wmt_compute_pipeline_info
{
    uint64_t compute_function;
    struct wmt_native_const_ptr binary_archives_for_lookup;
    uint64_t binary_archive_for_serialization;
    uint8_t num_binary_archives_for_lookup;
    bool fail_on_binary_archive_miss;
    uint8_t padding;
    bool tgsize_is_multiple_of_sgwidth;
    uint32_t immutable_buffers;
};

struct wmt_clear_color
{
    double r;
    double g;
    double b;
    double a;
};

struct wmt_color_attachment_info
{
    uint64_t texture;
    uint32_t load_action;
    uint32_t store_action;
    uint16_t level;
    uint16_t slice;
    uint32_t depth_plane;
    struct wmt_clear_color clear_color;
    uint64_t resolve_texture;
    uint16_t resolve_level;
    uint16_t resolve_slice;
    uint32_t resolve_depth_plane;
};

struct wmt_depth_attachment_info
{
    uint64_t texture;
    uint32_t load_action;
    uint32_t store_action;
    uint16_t level;
    uint16_t slice;
    uint32_t depth_plane;
    float clear_depth;
};

struct wmt_stencil_attachment_info
{
    uint64_t texture;
    uint32_t load_action;
    uint32_t store_action;
    uint16_t level;
    uint16_t slice;
    uint32_t depth_plane;
    uint8_t clear_stencil;
};

struct wmt_render_pass_info
{
    struct wmt_color_attachment_info colors[8];
    struct wmt_depth_attachment_info depth;
    struct wmt_stencil_attachment_info stencil;
    uint8_t default_raster_sample_count;
    uint8_t render_target_array_length;
    uint8_t tile_width;
    uint8_t tile_height;
    uint32_t render_target_height;
    uint32_t render_target_width;
    uint64_t visibility_buffer;
};

struct wmt_color_attachment_blend_info
{
    uint32_t pixel_format;
    uint8_t rgb_blend_operation;
    uint8_t alpha_blend_operation;
    uint8_t src_rgb_blend_factor;
    uint8_t dst_rgb_blend_factor;
    uint8_t src_alpha_blend_factor;
    uint8_t dst_alpha_blend_factor;
    uint8_t write_mask;
    bool blending_enabled;
};

struct wmt_render_pipeline_info
{
    struct wmt_color_attachment_blend_info colors[8];
    bool alpha_to_coverage_enabled;
    bool logic_operation_enabled;
    uint8_t logic_operation;
    bool rasterization_enabled;
    uint8_t raster_sample_count;
    uint32_t depth_pixel_format;
    uint32_t stencil_pixel_format;
    uint64_t vertex_function;
    uint64_t fragment_function;
    uint32_t immutable_vertex_buffers;
    uint32_t immutable_fragment_buffers;
    uint32_t input_primitive_topology;
    uint8_t tessellation_partition_mode;
    uint8_t max_tessellation_factor;
    uint8_t tessellation_output_winding_order;
    uint8_t tessellation_factor_step;
    uint64_t binary_archive_for_serialization;
    struct wmt_native_const_ptr binary_archives_for_lookup;
    uint8_t num_binary_archives_for_lookup;
    bool fail_on_binary_archive_miss;
    uint8_t padding[6];
};

struct wmt_layer_props
{
    uint64_t device;
    double contents_scale;
    double drawable_width;
    double drawable_height;
    bool opaque;
    bool display_sync_enabled;
    bool framebuffer_only;
    uint32_t pixel_format;
};

struct wmt_edr_value
{
    float maximum_edr_color_component_value;
    float maximum_potential_edr_color_component_value;
};

struct wmt_function_constant
{
    struct wmt_native_const_ptr data;
    uint16_t type;
    uint16_t index;
    uint32_t reserved;
};

struct wmt_hdr_metadata
{
    uint16_t red_primary[2];
    uint16_t green_primary[2];
    uint16_t blue_primary[2];
    uint16_t white_point[2];
    uint32_t max_mastering_luminance;
    uint32_t min_mastering_luminance;
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
};

struct wmt_cmd_base
{
    uint16_t type;
    uint16_t reserved[3];
    struct wmt_native_ptr next;
};

struct wmt_blit_copy_buffer
{
    struct wmt_cmd_base base;
    uint64_t src;
    uint64_t src_offset;
    uint64_t dst;
    uint64_t dst_offset;
    uint64_t copy_length;
};

struct wmt_blit_copy_buffer_texture
{
    struct wmt_cmd_base base;
    uint64_t src;
    uint64_t src_offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
    struct wmt_size size;
    uint64_t dst;
    uint32_t slice;
    uint32_t level;
    struct wmt_origin origin;
};

struct wmt_blit_copy_buffer_texture_option
{
    struct wmt_cmd_base base;
    uint64_t src;
    uint64_t src_offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
    struct wmt_size size;
    uint64_t dst;
    uint32_t slice;
    uint16_t level;
    uint16_t options;
    struct wmt_origin origin;
};

struct wmt_blit_copy_texture_texture
{
    struct wmt_cmd_base base;
    uint64_t src;
    uint32_t src_slice;
    uint32_t src_level;
    struct wmt_origin src_origin;
    struct wmt_size src_size;
    uint64_t dst;
    uint32_t dst_slice;
    uint32_t dst_level;
    struct wmt_origin dst_origin;
};

struct wmt_blit_copy_texture_buffer
{
    struct wmt_cmd_base base;
    uint64_t src;
    uint32_t slice;
    uint32_t level;
    struct wmt_origin origin;
    struct wmt_size size;
    uint64_t dst;
    uint64_t offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
};

struct wmt_blit_object
{
    struct wmt_cmd_base base;
    uint64_t object;
};

struct wmt_blit_fill_buffer
{
    struct wmt_cmd_base base;
    uint64_t buffer;
    uint64_t offset;
    uint64_t length;
    uint8_t value;
};

struct wmt_blit_resolve_counters
{
    struct wmt_cmd_base base;
    uint64_t sample_buffer;
    uint32_t start;
    uint32_t length;
    uint64_t dst_buffer;
    uint64_t dst_offset;
};

#pragma pack(push, 4)

struct wmt_render_use_resource32
{
    struct wmt_cmd_base32 base;
    uint64_t resource;
    uint32_t usage;
    uint8_t stages;
};

struct wmt_render_set_buffer32
{
    struct wmt_cmd_base32 base;
    uint64_t buffer;
    uint64_t offset;
    uint8_t index;
};

struct wmt_render_set_buffer_offset32
{
    struct wmt_cmd_base32 base;
    uint64_t offset;
    uint8_t index;
};

struct wmt_render_set_bytes32
{
    struct wmt_cmd_base32 base;
    struct wmt_wire_ptr bytes;
    uint64_t length;
    uint8_t index;
};

struct wmt_render_set_texture32
{
    struct wmt_cmd_base32 base;
    uint64_t texture;
    uint8_t index;
};

struct wmt_render_rasterizer32
{
    struct wmt_cmd_base32 base;
    uint8_t fill_mode;
    uint8_t cull_mode;
    uint8_t depth_clip_mode;
    uint8_t winding;
    float depth_bias;
    float slope_scale;
    float depth_bias_clamp;
};

struct wmt_render_object32
{
    struct wmt_cmd_base32 base;
    uint64_t object;
};

struct wmt_render_visibility32
{
    struct wmt_cmd_base32 base;
    uint64_t offset;
    uint8_t mode;
};

struct wmt_render_draw32
{
    struct wmt_cmd_base32 base;
    uint8_t primitive_type;
    uint64_t vertex_start;
    uint64_t vertex_count;
    uint32_t instance_count;
    uint32_t base_instance;
};

struct wmt_render_draw_indirect32
{
    struct wmt_cmd_base32 base;
    uint8_t primitive_type;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
};

struct wmt_render_draw_indexed32
{
    struct wmt_cmd_base32 base;
    uint8_t primitive_type;
    uint8_t index_type;
    uint64_t index_count;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint32_t instance_count;
    int32_t base_vertex;
    uint32_t base_instance;
};

struct wmt_render_draw_indexed_indirect32
{
    struct wmt_cmd_base32 base;
    uint8_t primitive_type;
    uint8_t index_type;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
};

struct wmt_render_mesh32
{
    struct wmt_cmd_base32 base;
    struct wmt_size threadgroups_per_grid;
    struct wmt_size object_threadgroup_size;
    struct wmt_size mesh_threadgroup_size;
};

struct wmt_render_mesh_indirect32
{
    struct wmt_cmd_base32 base;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    struct wmt_size object_threadgroup_size;
    struct wmt_size mesh_threadgroup_size;
};

struct wmt_render_barrier32
{
    struct wmt_cmd_base32 base;
    uint8_t scope;
    uint8_t stages_before;
    uint8_t stages_after;
};

struct wmt_render_viewports32
{
    struct wmt_cmd_base32 base;
    struct wmt_wire_ptr viewports;
    uint8_t count;
};

struct wmt_render_viewport32
{
    struct wmt_cmd_base32 base;
    struct wmt_viewport viewport;
};

struct wmt_render_scissors32
{
    struct wmt_cmd_base32 base;
    struct wmt_wire_ptr scissors;
    uint8_t count;
};

struct wmt_render_scissor32
{
    struct wmt_cmd_base32 base;
    struct wmt_scissor_rect scissor;
};

struct wmt_render_blend32
{
    struct wmt_cmd_base32 base;
    float red;
    float green;
    float blue;
    float alpha;
    uint8_t stencil_ref;
};

struct wmt_render_geometry32
{
    struct wmt_cmd_base32 base;
    uint64_t draw_arguments_offset;
    uint32_t warp_count;
    uint32_t instance_count;
    uint32_t vertices_per_warp;
};

struct wmt_render_geometry_indexed32
{
    struct wmt_cmd_base32 base;
    uint64_t draw_arguments_offset;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint32_t warp_count;
    uint32_t instance_count;
    uint32_t vertices_per_warp;
};

struct wmt_render_geometry_indirect32
{
    struct wmt_cmd_base32 base;
    uint64_t immediate_draw_arguments;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    uint64_t dispatch_args_buffer;
    uint64_t dispatch_args_offset;
    uint32_t vertices_per_warp;
};

struct wmt_render_geometry_indexed_indirect32
{
    struct wmt_cmd_base32 base;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint64_t immediate_draw_arguments;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    uint64_t dispatch_args_buffer;
    uint64_t dispatch_args_offset;
    uint32_t vertices_per_warp;
};

struct wmt_render_fence32
{
    struct wmt_cmd_base32 base;
    uint64_t fence;
    uint8_t stages;
};

struct wmt_render_tessellation32
{
    struct wmt_cmd_base32 base;
    uint64_t draw_arguments_offset;
    uint32_t instance_count;
    uint32_t threads_per_patch;
    uint32_t patches_per_group;
    uint32_t patches_per_mesh_instance;
};

struct wmt_render_tessellation_indexed32
{
    struct wmt_cmd_base32 base;
    uint64_t draw_arguments_offset;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint32_t instance_count;
    uint32_t threads_per_patch;
    uint32_t patches_per_group;
    uint32_t patches_per_mesh_instance;
};

struct wmt_render_tessellation_indirect32
{
    struct wmt_cmd_base32 base;
    uint64_t immediate_draw_arguments;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    uint64_t dispatch_args_buffer;
    uint64_t dispatch_args_offset;
    uint32_t threads_per_patch;
    uint32_t patches_per_group;
};

struct wmt_render_tessellation_indexed_indirect32
{
    struct wmt_cmd_base32 base;
    uint64_t immediate_draw_arguments;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    uint64_t dispatch_args_buffer;
    uint64_t dispatch_args_offset;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint32_t threads_per_patch;
    uint32_t patches_per_group;
};

struct wmt_render_tile_threads32
{
    struct wmt_cmd_base32 base;
    uint32_t width;
    uint32_t height;
};

#pragma pack(pop)

struct wmt_render_use_resource
{
    struct wmt_cmd_base base;
    uint64_t resource;
    uint32_t usage;
    uint8_t stages;
};

struct wmt_render_set_buffer
{
    struct wmt_cmd_base base;
    uint64_t buffer;
    uint64_t offset;
    uint8_t index;
};

struct wmt_render_set_buffer_offset
{
    struct wmt_cmd_base base;
    uint64_t offset;
    uint8_t index;
};

struct wmt_render_set_bytes
{
    struct wmt_cmd_base base;
    struct wmt_native_ptr bytes;
    uint64_t length;
    uint8_t index;
};

struct wmt_render_set_texture
{
    struct wmt_cmd_base base;
    uint64_t texture;
    uint8_t index;
};

struct wmt_render_rasterizer
{
    struct wmt_cmd_base base;
    uint8_t fill_mode;
    uint8_t cull_mode;
    uint8_t depth_clip_mode;
    uint8_t winding;
    float depth_bias;
    float slope_scale;
    float depth_bias_clamp;
};

struct wmt_render_object
{
    struct wmt_cmd_base base;
    uint64_t object;
};

struct wmt_render_visibility
{
    struct wmt_cmd_base base;
    uint64_t offset;
    uint8_t mode;
};

struct wmt_render_draw
{
    struct wmt_cmd_base base;
    uint8_t primitive_type;
    uint64_t vertex_start;
    uint64_t vertex_count;
    uint32_t instance_count;
    uint32_t base_instance;
};

struct wmt_render_draw_indirect
{
    struct wmt_cmd_base base;
    uint8_t primitive_type;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
};

struct wmt_render_draw_indexed
{
    struct wmt_cmd_base base;
    uint8_t primitive_type;
    uint8_t index_type;
    uint64_t index_count;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint32_t instance_count;
    int32_t base_vertex;
    uint32_t base_instance;
};

struct wmt_render_draw_indexed_indirect
{
    struct wmt_cmd_base base;
    uint8_t primitive_type;
    uint8_t index_type;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
};

struct wmt_render_mesh
{
    struct wmt_cmd_base base;
    struct wmt_size threadgroups_per_grid;
    struct wmt_size object_threadgroup_size;
    struct wmt_size mesh_threadgroup_size;
};

struct wmt_render_mesh_indirect
{
    struct wmt_cmd_base base;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    struct wmt_size object_threadgroup_size;
    struct wmt_size mesh_threadgroup_size;
};

struct wmt_render_barrier
{
    struct wmt_cmd_base base;
    uint8_t scope;
    uint8_t stages_before;
    uint8_t stages_after;
};

struct wmt_render_viewports
{
    struct wmt_cmd_base base;
    struct wmt_native_ptr viewports;
    uint8_t count;
};

struct wmt_render_viewport
{
    struct wmt_cmd_base base;
    struct wmt_viewport viewport;
};

struct wmt_render_scissors
{
    struct wmt_cmd_base base;
    struct wmt_native_ptr scissors;
    uint8_t count;
};

struct wmt_render_scissor
{
    struct wmt_cmd_base base;
    struct wmt_scissor_rect scissor;
};

struct wmt_render_blend
{
    struct wmt_cmd_base base;
    float red;
    float green;
    float blue;
    float alpha;
    uint8_t stencil_ref;
};

struct wmt_render_geometry
{
    struct wmt_cmd_base base;
    uint64_t draw_arguments_offset;
    uint32_t warp_count;
    uint32_t instance_count;
    uint32_t vertices_per_warp;
};

struct wmt_render_geometry_indexed
{
    struct wmt_cmd_base base;
    uint64_t draw_arguments_offset;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint32_t warp_count;
    uint32_t instance_count;
    uint32_t vertices_per_warp;
};

struct wmt_render_geometry_indirect
{
    struct wmt_cmd_base base;
    uint64_t immediate_draw_arguments;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    uint64_t dispatch_args_buffer;
    uint64_t dispatch_args_offset;
    uint32_t vertices_per_warp;
};

struct wmt_render_geometry_indexed_indirect
{
    struct wmt_cmd_base base;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint64_t immediate_draw_arguments;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    uint64_t dispatch_args_buffer;
    uint64_t dispatch_args_offset;
    uint32_t vertices_per_warp;
};

struct wmt_render_fence
{
    struct wmt_cmd_base base;
    uint64_t fence;
    uint8_t stages;
};

struct wmt_render_tessellation
{
    struct wmt_cmd_base base;
    uint64_t draw_arguments_offset;
    uint32_t instance_count;
    uint32_t threads_per_patch;
    uint32_t patches_per_group;
    uint32_t patches_per_mesh_instance;
};

struct wmt_render_tessellation_indexed
{
    struct wmt_cmd_base base;
    uint64_t draw_arguments_offset;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint32_t instance_count;
    uint32_t threads_per_patch;
    uint32_t patches_per_group;
    uint32_t patches_per_mesh_instance;
};

struct wmt_render_tessellation_indirect
{
    struct wmt_cmd_base base;
    uint64_t immediate_draw_arguments;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    uint64_t dispatch_args_buffer;
    uint64_t dispatch_args_offset;
    uint32_t threads_per_patch;
    uint32_t patches_per_group;
};

struct wmt_render_tessellation_indexed_indirect
{
    struct wmt_cmd_base base;
    uint64_t immediate_draw_arguments;
    uint64_t indirect_args_buffer;
    uint64_t indirect_args_offset;
    uint64_t dispatch_args_buffer;
    uint64_t dispatch_args_offset;
    uint64_t index_buffer;
    uint64_t index_buffer_offset;
    uint32_t threads_per_patch;
    uint32_t patches_per_group;
};

struct wmt_render_tile_threads
{
    struct wmt_cmd_base base;
    uint32_t width;
    uint32_t height;
};

/* The pinned i386 and native layouts are different ABIs, not memcpy peers. */
C_ASSERT( sizeof(struct wmt_wire_ptr) == 8 );
C_ASSERT( __alignof__(struct wmt_wire_ptr) == 4 );
C_ASSERT( sizeof(struct wmt_native_ptr) == 8 );
C_ASSERT( __alignof__(struct wmt_native_ptr) == 8 );
C_ASSERT( sizeof(struct wmt_buffer_info32) == 32 );
C_ASSERT( offsetof(struct wmt_buffer_info32, memory) == 16 );
C_ASSERT( sizeof(struct wmt_buffer_info) == 32 );
C_ASSERT( offsetof(struct wmt_buffer_info, memory) == 16 );
C_ASSERT( sizeof(struct wmt_sampler_info32) == 32 );
C_ASSERT( sizeof(struct wmt_sampler_info) == 32 );
C_ASSERT( sizeof(struct wmt_depth_stencil_info32) == 16 );
C_ASSERT( sizeof(struct wmt_depth_stencil_info) == 16 );
C_ASSERT( sizeof(struct wmt_texture_info32) == 48 );
C_ASSERT( sizeof(struct wmt_texture_info) == 48 );
C_ASSERT( sizeof(struct wmt_compute_pipeline_info32) == 32 );
C_ASSERT( sizeof(struct wmt_compute_pipeline_info) == 32 );
C_ASSERT( sizeof(struct wmt_render_pass_info32) == 652 );
C_ASSERT( __alignof__(struct wmt_render_pass_info32) == 4 );
C_ASSERT( offsetof(struct wmt_render_pass_info32, visibility_buffer) == 644 );
C_ASSERT( sizeof(struct wmt_render_pass_info) == 664 );
C_ASSERT( __alignof__(struct wmt_render_pass_info) == 8 );
C_ASSERT( offsetof(struct wmt_render_pass_info, visibility_buffer) == 656 );
C_ASSERT( sizeof(struct wmt_render_pipeline_info32) == 168 );
C_ASSERT( __alignof__(struct wmt_render_pipeline_info32) == 4 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, logic_operation) == 98 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, rasterization_enabled) == 99 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, raster_sample_count) == 100 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, depth_pixel_format) == 104 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, stencil_pixel_format) == 108 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, vertex_function) == 112 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, fragment_function) == 120 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, immutable_vertex_buffers) == 128 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, immutable_fragment_buffers) == 132 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, input_primitive_topology) == 136 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, tessellation_partition_mode) == 140 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, max_tessellation_factor) == 141 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, tessellation_output_winding_order) == 142 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, tessellation_factor_step) == 143 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, binary_archive_for_serialization) == 144 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, binary_archives_for_lookup) == 152 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info32, num_binary_archives_for_lookup) == 160 );
C_ASSERT( sizeof(struct wmt_render_pipeline_info) == 168 );
C_ASSERT( __alignof__(struct wmt_render_pipeline_info) == 8 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, logic_operation) == 98 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, rasterization_enabled) == 99 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, raster_sample_count) == 100 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, depth_pixel_format) == 104 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, stencil_pixel_format) == 108 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, vertex_function) == 112 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, fragment_function) == 120 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, immutable_vertex_buffers) == 128 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, immutable_fragment_buffers) == 132 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, input_primitive_topology) == 136 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, tessellation_partition_mode) == 140 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, max_tessellation_factor) == 141 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, tessellation_output_winding_order) == 142 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, tessellation_factor_step) == 143 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, binary_archive_for_serialization) == 144 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, binary_archives_for_lookup) == 152 );
C_ASSERT( offsetof(struct wmt_render_pipeline_info, num_binary_archives_for_lookup) == 160 );
C_ASSERT( sizeof(struct wmt_layer_props32) == 40 );
C_ASSERT( sizeof(struct wmt_layer_props) == 40 );
C_ASSERT( sizeof(struct wmt_function_constant32) == 16 );
C_ASSERT( sizeof(struct wmt_function_constant) == 16 );
C_ASSERT( sizeof(struct wmt_hdr_metadata32) == 28 );
C_ASSERT( sizeof(struct wmt_hdr_metadata) == 28 );
C_ASSERT( sizeof(struct wmt_cmd_base32) == 16 );
C_ASSERT( sizeof(struct wmt_cmd_base) == 16 );
C_ASSERT( sizeof(struct wmt_blit_copy_buffer32) == 56 );
C_ASSERT( sizeof(struct wmt_blit_copy_buffer) == 56 );
C_ASSERT( sizeof(struct wmt_blit_copy_buffer_texture32) == 104 );
C_ASSERT( sizeof(struct wmt_blit_copy_buffer_texture) == 104 );
C_ASSERT( sizeof(struct wmt_blit_copy_texture_buffer32) == 104 );
C_ASSERT( sizeof(struct wmt_blit_copy_texture_buffer) == 104 );
C_ASSERT( sizeof(struct wmt_blit_copy_texture_texture32) == 120 );
C_ASSERT( sizeof(struct wmt_blit_copy_texture_texture) == 120 );
C_ASSERT( sizeof(struct wmt_blit_object32) == 24 );
C_ASSERT( sizeof(struct wmt_blit_object) == 24 );
C_ASSERT( sizeof(struct wmt_blit_fill_buffer32) == 44 );
C_ASSERT( sizeof(struct wmt_blit_fill_buffer) == 48 );
C_ASSERT( sizeof(struct wmt_blit_resolve_counters32) == 48 );
C_ASSERT( sizeof(struct wmt_blit_resolve_counters) == 48 );
C_ASSERT( sizeof(struct wmt_blit_copy_buffer_texture_option32) == 104 );
C_ASSERT( sizeof(struct wmt_blit_copy_buffer_texture_option) == 104 );
C_ASSERT( offsetof(struct wmt_render_set_bytes32, bytes) == 16 );
C_ASSERT( offsetof(struct wmt_render_set_bytes, bytes) == 16 );
C_ASSERT( sizeof(struct wmt_render_use_resource32) == 32 );
C_ASSERT( sizeof(struct wmt_render_use_resource) == 32 );
C_ASSERT( sizeof(struct wmt_render_set_buffer32) == 36 );
C_ASSERT( sizeof(struct wmt_render_set_buffer) == 40 );
C_ASSERT( sizeof(struct wmt_render_set_buffer_offset32) == 28 );
C_ASSERT( sizeof(struct wmt_render_set_buffer_offset) == 32 );
C_ASSERT( sizeof(struct wmt_render_set_bytes32) == 36 );
C_ASSERT( sizeof(struct wmt_render_set_bytes) == 40 );
C_ASSERT( sizeof(struct wmt_render_set_texture32) == 28 );
C_ASSERT( sizeof(struct wmt_render_set_texture) == 32 );
C_ASSERT( sizeof(struct wmt_render_rasterizer32) == 32 );
C_ASSERT( sizeof(struct wmt_render_rasterizer) == 32 );
C_ASSERT( sizeof(struct wmt_render_object32) == 24 );
C_ASSERT( sizeof(struct wmt_render_object) == 24 );
C_ASSERT( sizeof(struct wmt_render_visibility32) == 28 );
C_ASSERT( sizeof(struct wmt_render_visibility) == 32 );
C_ASSERT( offsetof(struct wmt_render_draw32, vertex_start) == 20 );
C_ASSERT( offsetof(struct wmt_render_draw, vertex_start) == 24 );
C_ASSERT( sizeof(struct wmt_render_draw32) == 44 );
C_ASSERT( sizeof(struct wmt_render_draw) == 48 );
C_ASSERT( sizeof(struct wmt_render_draw_indirect32) == 36 );
C_ASSERT( sizeof(struct wmt_render_draw_indirect) == 40 );
C_ASSERT( sizeof(struct wmt_render_draw_indexed32) == 56 );
C_ASSERT( sizeof(struct wmt_render_draw_indexed) == 64 );
C_ASSERT( sizeof(struct wmt_render_draw_indexed_indirect32) == 52 );
C_ASSERT( sizeof(struct wmt_render_draw_indexed_indirect) == 56 );
C_ASSERT( sizeof(struct wmt_render_mesh32) == 88 );
C_ASSERT( sizeof(struct wmt_render_mesh) == 88 );
C_ASSERT( sizeof(struct wmt_render_mesh_indirect32) == 80 );
C_ASSERT( sizeof(struct wmt_render_mesh_indirect) == 80 );
C_ASSERT( sizeof(struct wmt_render_barrier32) == 20 );
C_ASSERT( sizeof(struct wmt_render_barrier) == 24 );
C_ASSERT( sizeof(struct wmt_render_viewports32) == 28 );
C_ASSERT( sizeof(struct wmt_render_viewports) == 32 );
C_ASSERT( sizeof(struct wmt_render_viewport32) == 64 );
C_ASSERT( sizeof(struct wmt_render_viewport) == 64 );
C_ASSERT( sizeof(struct wmt_render_scissors32) == 28 );
C_ASSERT( sizeof(struct wmt_render_scissors) == 32 );
C_ASSERT( sizeof(struct wmt_render_scissor32) == 48 );
C_ASSERT( sizeof(struct wmt_render_scissor) == 48 );
C_ASSERT( sizeof(struct wmt_render_blend32) == 36 );
C_ASSERT( sizeof(struct wmt_render_blend) == 40 );
C_ASSERT( sizeof(struct wmt_render_geometry32) == 36 );
C_ASSERT( sizeof(struct wmt_render_geometry) == 40 );
C_ASSERT( sizeof(struct wmt_render_geometry_indexed32) == 52 );
C_ASSERT( sizeof(struct wmt_render_geometry_indexed) == 56 );
C_ASSERT( sizeof(struct wmt_render_geometry_indirect32) == 60 );
C_ASSERT( sizeof(struct wmt_render_geometry_indirect) == 64 );
C_ASSERT( sizeof(struct wmt_render_geometry_indexed_indirect32) == 76 );
C_ASSERT( sizeof(struct wmt_render_geometry_indexed_indirect) == 80 );
C_ASSERT( sizeof(struct wmt_render_fence32) == 28 );
C_ASSERT( sizeof(struct wmt_render_fence) == 32 );
C_ASSERT( sizeof(struct wmt_render_tessellation32) == 40 );
C_ASSERT( sizeof(struct wmt_render_tessellation) == 40 );
C_ASSERT( sizeof(struct wmt_render_tessellation_indexed32) == 56 );
C_ASSERT( sizeof(struct wmt_render_tessellation_indexed) == 56 );
C_ASSERT( sizeof(struct wmt_render_tessellation_indirect32) == 64 );
C_ASSERT( sizeof(struct wmt_render_tessellation_indirect) == 64 );
C_ASSERT( sizeof(struct wmt_render_tessellation_indexed_indirect32) == 80 );
C_ASSERT( sizeof(struct wmt_render_tessellation_indexed_indirect) == 80 );
C_ASSERT( sizeof(struct wmt_render_tile_threads32) == 24 );
C_ASSERT( sizeof(struct wmt_render_tile_threads) == 24 );
C_ASSERT( sizeof(struct wmt_params32_info_ret) == 24 );
C_ASSERT( offsetof(struct wmt_params32_info_ret, ret) == 16 );
C_ASSERT( sizeof(struct wmt_params32_pipeline) == 32 );
C_ASSERT( offsetof(struct wmt_params32_pipeline, ret_error) == 16 );
C_ASSERT( sizeof(struct wmt_params32_function_constants) == 48 );
C_ASSERT( sizeof(struct wmt_params32_query_display_layer) == 40 );
C_ASSERT( sizeof(struct wmt_params32_getcstring) == 36 );
C_ASSERT( offsetof(struct wmt_params32_getcstring, ret) == 32 );
C_ASSERT( sizeof(struct wmt_params32_capture) == 20 );
C_ASSERT( offsetof(struct wmt_params32_capture, ret) == 16 );
C_ASSERT( sizeof(struct wmt_display_description32) == 44 );
C_ASSERT( sizeof(struct wmt_params32_query_display) == 28 );
C_ASSERT( offsetof(struct wmt_params32_query_display, ret) == 24 );
C_ASSERT( sizeof(struct wmt_params32_resolve_counter_range) == 32 );
C_ASSERT( sizeof(struct wmt_air_initialize32) == 24 );
C_ASSERT( sizeof(struct wmt_air_compile32) == 32 );
C_ASSERT( offsetof(struct wmt_air_compile32, ret) == 24 );
C_ASSERT( sizeof(struct wmt_air_pipeline_compile32) == 40 );
C_ASSERT( offsetof(struct wmt_air_pipeline_compile32, ret) == 32 );
C_ASSERT( sizeof(struct wmt_air_get_bitcode32) == 16 );
C_ASSERT( sizeof(struct wmt_air_get_error32) == 24 );
C_ASSERT( sizeof(struct wmt_air_get_arguments32) == 16 );
C_ASSERT( sizeof(struct wmt_air_reflection32) == 72 );
C_ASSERT( sizeof(struct wmt_air_compiled_bitcode32) == 16 );

/* Pinned table indices used by the companion. */
enum wmt_unix_call
{
    WMT_CALL_COPY_ALL_DEVICES = 4,
    WMT_CALL_NEW_COMMAND_QUEUE = 9,
    WMT_CALL_AUTORELEASE_POOL_INIT = 10,
    WMT_CALL_NEW_SHARED_EVENT = 15,
    WMT_CALL_NSSTRING_GETCSTRING = 8,
    WMT_CALL_NEW_BUFFER = 18,
    WMT_CALL_NEW_SAMPLER = 19,
    WMT_CALL_NEW_DEPTH_STENCIL = 20,
    WMT_CALL_NEW_TEXTURE = 21,
    WMT_CALL_BUFFER_NEW_TEXTURE = 22,
    WMT_CALL_NEW_TEXTURE_VIEW = 23,
    WMT_CALL_NEW_LIBRARY = 25,
    WMT_CALL_LIBRARY_NEW_FUNCTION = 26,
    WMT_CALL_NEW_COMPUTE_PSO = 29,
    WMT_CALL_RENDER_ENCODER = 32,
    WMT_CALL_NEW_RENDER_PSO = 34,
    WMT_CALL_NEW_MESH_RENDER_PSO = 35,
    WMT_CALL_BLIT_COMMANDS = 36,
    WMT_CALL_COMPUTE_COMMANDS = 37,
    WMT_CALL_RENDER_COMMANDS = 38,
    WMT_CALL_REPLACE_REGION = 45,
    WMT_CALL_START_CAPTURE = 54,
    WMT_CALL_NEW_TEMPORAL_SCALER = 56,
    WMT_CALL_NEW_SPATIAL_SCALER = 57,
    WMT_CALL_ENCODE_TEMPORAL_SCALE = 58,
    WMT_CALL_NSSTRING_STRING = 60,
    WMT_CALL_NSSTRING_ALLOC_INIT = 61,
    WMT_CALL_LAYER_SET_PROPS = 70,
    WMT_CALL_LAYER_GET_PROPS = 71,
    WMT_CALL_CREATE_METAL_VIEW = 72,
    WMT_CALL_RELEASE_METAL_VIEW = 73,
    WMT_CALL_NULL_83 = 83,
    WMT_CALL_LOG_ENUMERATE = 91,
    WMT_CALL_DISPLAY_DESCRIPTION = 96,
    WMT_CALL_LAYER_EDR = 97,
    WMT_CALL_FUNCTION_CONSTANTS = 98,
    WMT_CALL_QUERY_DISPLAY = 99,
    WMT_CALL_UPDATE_DISPLAY = 100,
    WMT_CALL_QUERY_DISPLAY_LAYER = 101,
    WMT_CALL_SHARED_EVENT_SET_WIN32_EVENT = 104,
    WMT_CALL_NEW_FENCE = 105,
    WMT_CALL_NEW_EVENT = 106,
    WMT_CALL_BUFFER_UPDATE = 107,
    WMT_CALL_SHARED_EVENT_LISTENER_CREATE = 108,
    WMT_CALL_SHARED_EVENT_LISTENER_DESTROY = 110,
    WMT_CALL_NEW_BINARY_ARCHIVE = 112,
    WMT_CALL_SERIALIZE_BINARY_ARCHIVE = 113,
    WMT_CALL_DISPATCH_DATA = 114,
    WMT_CALL_CACHE_READER_INIT = 115,
    WMT_CALL_CACHE_READER_GET = 116,
    WMT_CALL_CACHE_WRITER_INIT = 117,
    WMT_CALL_CACHE_WRITER_SET = 118,
    WMT_CALL_SET_CACHE_PATH = 119,
    WMT_CALL_NEW_SHARED_TEXTURE = 120,
    WMT_CALL_BOOTSTRAP_LOOKUP = 122,
    WMT_CALL_SHARED_EVENT_CREATE_PORT = 123,
    WMT_CALL_NEW_SHARED_EVENT_WITH_PORT = 124,
    WMT_CALL_NEW_TIMESTAMP_BUFFER = 127,
    WMT_CALL_RESOLVE_COUNTER_RANGE = 128,
    WMT_CALL_BLIT_WITH_SAMPLE_BUFFERS = 129,
    WMT_CALL_NEW_TILE_RENDER_PSO = 131,
    WMT_CALL_NEW_RESIDENCY_SET = 132,
    WMT_CALL_RESIDENCY_ADD = 133,
    WMT_CALL_RESIDENCY_REMOVE = 134,
};

/* Native outer parameter blocks are fixed-width and packed to at most 8. */
struct wmt_params_obj_ptr
{
    uint64_t handle;
    struct wmt_native_ptr arg;
};

struct wmt_params_obj_const_ptr
{
    uint64_t handle;
    struct wmt_native_const_ptr arg;
};

struct wmt_params_obj_u64_obj_ret
{
    uint64_t handle;
    uint64_t arg;
    uint64_t ret;
};

struct wmt_params_info_ret
{
    uint64_t object;
    struct wmt_native_ptr info;
    uint64_t ret;
};

struct wmt_params_const_info_ret
{
    uint64_t object;
    struct wmt_native_const_ptr info;
    uint64_t ret;
};

struct wmt_params_pipeline
{
    uint64_t device;
    struct wmt_native_const_ptr info;
    uint64_t ret_error;
    uint64_t ret_pso;
};

struct wmt_params_command
{
    uint64_t encoder;
    struct wmt_native_const_ptr command_head;
};

struct wmt_params_string
{
    struct wmt_native_const_ptr buffer;
    uint64_t encoding;
    uint64_t ret;
};

struct wmt_params_function_constants
{
    uint64_t library;
    struct wmt_native_const_ptr name;
    struct wmt_native_const_ptr constants;
    uint64_t count;
    uint64_t ret;
    uint64_t ret_error;
};

struct wmt_params_query_display_layer
{
    uint64_t layer;
    uint64_t version;
    uint64_t colorspace;
    struct wmt_native_ptr hdr_metadata;
    struct wmt_edr_value edr_value;
};

struct wmt_params_texture_replace
{
    uint64_t texture;
    struct wmt_origin origin;
    struct wmt_size size;
    uint64_t level;
    uint64_t slice;
    struct wmt_native_ptr data;
    uint64_t bytes_per_row;
    uint64_t bytes_per_image;
};

struct wmt_params_buffer_update
{
    uint64_t buffer;
    uint64_t offset;
    struct wmt_native_const_ptr data;
    uint64_t length;
};

C_ASSERT( sizeof(struct wmt_params_texture_replace) == 96 );
C_ASSERT( sizeof(struct wmt_params_buffer_update) == 32 );

struct wmt_params_cache_init
{
    struct wmt_native_const_ptr path;
    uint64_t version;
    uint64_t ret;
};

struct wmt_params_set_cache_path
{
    struct wmt_native_const_ptr path;
    uint64_t ret_success;
};

NTSTATUS wmt_call_18( void *args );
NTSTATUS wmt_call_19( void *args );
NTSTATUS wmt_call_20( void *args );
NTSTATUS wmt_call_21( void *args );
NTSTATUS wmt_call_26( void *args );
NTSTATUS wmt_call_29( void *args );
NTSTATUS wmt_call_32( void *args );
NTSTATUS wmt_call_34( void *args );
NTSTATUS wmt_call_36( void *args );
NTSTATUS wmt_call_38( void *args );
NTSTATUS wmt_call_60( void *args );
NTSTATUS wmt_call_61( void *args );
NTSTATUS wmt_call_70( void *args );
NTSTATUS wmt_call_71( void *args );
NTSTATUS wmt_call_97( void *args );
NTSTATUS wmt_call_98( void *args );
NTSTATUS wmt_call_101( void *args );
NTSTATUS wmt_call_114( void *args );
NTSTATUS wmt_call_45( void *args );
NTSTATUS wmt_call_107( void *args );
NTSTATUS wmt_call_74( void *args );
NTSTATUS wmt_call_75( void *args );
NTSTATUS wmt_call_76( void *args );
NTSTATUS wmt_call_77( void *args );
NTSTATUS wmt_call_78( void *args );
NTSTATUS wmt_call_79( void *args );
NTSTATUS wmt_call_80( void *args );
NTSTATUS wmt_call_81( void *args );
NTSTATUS wmt_call_82( void *args );
NTSTATUS wmt_call_84( void *args );
NTSTATUS wmt_call_85( void *args );
NTSTATUS wmt_call_88( void *args );
NTSTATUS wmt_call_115( void *args );
NTSTATUS wmt_call_117( void *args );
NTSTATUS wmt_call_119( void *args );
NTSTATUS wmt_rollback_18( void *args, NTSTATUS status );
NTSTATUS wmt_rollback_cache_init( void *args, NTSTATUS status );
NTSTATUS wmt_rollback_114( void *args, NTSTATUS status );
NTSTATUS wmt_rollback_owned_output( unsigned int index, void *args, NTSTATUS status );
void wmt_prepare_owned_output( unsigned int index, void *args );
NTSTATUS wmt_drain_air_registries(void);

NTSTATUS wmt_unsupported_8( void *args );
NTSTATUS wmt_unsupported_22( void *args );
NTSTATUS wmt_unsupported_info_ret( void *args );
NTSTATUS wmt_unsupported_capture( void *args );
NTSTATUS wmt_unsupported_pipeline( void *args );
NTSTATUS wmt_unsupported_enumerate( void *args );
NTSTATUS wmt_unsupported_display_description( void *args );
NTSTATUS wmt_unsupported_query_display( void *args );
NTSTATUS wmt_unsupported_archive( void *args );
NTSTATUS wmt_unsupported_archive_serialize( void *args );
NTSTATUS wmt_unsupported_cache_get( void *args );
NTSTATUS wmt_unsupported_sample_buffer_encoder( void *args );
NTSTATUS wmt_unsupported_shared_texture( void *args );
NTSTATUS wmt_unsupported_resolve_counter_range( void *args );
NTSTATUS wmt_unsupported_air_initialize( void *args );
NTSTATUS wmt_unsupported_air_compile( void *args );
NTSTATUS wmt_unsupported_air_pipeline_compile( void *args );
NTSTATUS wmt_unsupported_air_get_bitcode( void *args );
NTSTATUS wmt_unsupported_air_get_error( void *args );
NTSTATUS wmt_unsupported_air_get_arguments( void *args );
NTSTATUS wmt_unsupported_no_output( void *args );

NTSTATUS wmt_forward_call( unsigned int index, void *args );
NTSTATUS wmt_normal_call( unsigned int index, void *args );
const struct wine_unixlib_owned_backing_codec_v2 *wmt_get_owned_backing_codec(void);

NTSTATUS wmt_snapshot_bytes( uint64_t guest, uint64_t size, void **ret );
NTSTATUS wmt_snapshot_cstring( uint64_t guest, size_t max_length, char **ret );
NTSTATUS wmt_snapshot_alloc( uint64_t size, void **ret );
NTSTATUS wmt_snapshot_realloc( void *ptr, uint64_t old_size, uint64_t new_size, void **ret );
void wmt_snapshot_free( void *ptr, uint64_t size );
void wmt_buffer_info32_to_native( const struct wmt_buffer_info32 *src,
                                  struct wmt_buffer_info *dst );

#endif /* __WINEMETAL_WOW64_PRIVATE_H */
