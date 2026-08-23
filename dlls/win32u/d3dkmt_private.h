/*
 * Private D3DKMT cross-DLL helpers
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_D3DKMT_PRIVATE_H
#define __WINE_D3DKMT_PRIVATE_H

#include <stdint.h>

#define WINE_D3DKMT_CREATE_ALLOCATION32_VERSION 1u
#define WINE_D3DKMT_OPEN_RESOURCE32_VERSION 1u
#define WINE_D3DKMT_LIFECYCLE32_VERSION 1u

enum wine_d3dkmt_lifecycle32_variant
{
    WINE_D3DKMT_LIFECYCLE32_OPEN_ADAPTER_FROM_LUID = 1,
    WINE_D3DKMT_LIFECYCLE32_CLOSE_ADAPTER,
    WINE_D3DKMT_LIFECYCLE32_CREATE_DEVICE,
    WINE_D3DKMT_LIFECYCLE32_DESTROY_DEVICE,
};

/* Fixed-width packet for the handle-only adapter/device lifecycle calls.  The
 * outer descriptor is captured by wow64win before this syscall; no guest
 * callback, command-buffer, or caller-sized pointer is carried across it. */
struct wine_d3dkmt_lifecycle32
{
    uint32_t version;
    uint32_t size;
    uint32_t variant;
    uint32_t guest_desc;
    uint32_t input_handle;
    uint32_t flags;
    uint32_t luid_low;
    int32_t luid_high;
    uint32_t output_handle;
    int32_t fault_status;
    uint32_t reserved[2];
};

C_ASSERT( sizeof(struct wine_d3dkmt_lifecycle32) == 48 );
C_ASSERT( offsetof(struct wine_d3dkmt_lifecycle32, guest_desc) == 12 );
C_ASSERT( offsetof(struct wine_d3dkmt_lifecycle32, output_handle) == 32 );
C_ASSERT( offsetof(struct wine_d3dkmt_lifecycle32, fault_status) == 36 );
C_ASSERT( offsetof(struct wine_d3dkmt_lifecycle32, reserved) == 40 );

enum wine_d3dkmt_create_allocation32_variant
{
    WINE_D3DKMT_CREATE_ALLOCATION32_V1 = 1,
    WINE_D3DKMT_CREATE_ALLOCATION32_V2,
};

/* Fixed-width private syscall packet.  Every guest_* field is an untrusted
 * 32-bit address in the current WoW64 process; it is never a host pointer. */
struct wine_d3dkmt_create_allocation32
{
    uint32_t version;
    uint32_t size;
    uint32_t variant;
    uint32_t guest_desc;
    uint32_t guest_standard_or_private;
    uint32_t guest_allocation;
    uint32_t guest_runtime;
    uint32_t hDevice;
    uint32_t hResource;
    uint32_t hGlobalShare;
    uint32_t private_runtime_data_size;
    uint32_t private_driver_data_size;
    uint32_t num_allocations;
    uint32_t flags;
    uint32_t private_runtime_resource_handle;
    int32_t fault_status;
    uint32_t reserved[2];
};

C_ASSERT( sizeof(struct wine_d3dkmt_create_allocation32) == 72 );
C_ASSERT( offsetof(struct wine_d3dkmt_create_allocation32, guest_desc) == 12 );
C_ASSERT( offsetof(struct wine_d3dkmt_create_allocation32, fault_status) == 60 );
C_ASSERT( offsetof(struct wine_d3dkmt_create_allocation32, reserved) == 64 );

enum wine_d3dkmt_open_resource32_variant
{
    WINE_D3DKMT_OPEN_RESOURCE32_V1 = 1,
    WINE_D3DKMT_OPEN_RESOURCE32_V2,
    WINE_D3DKMT_OPEN_RESOURCE32_NT_HANDLE,
};

struct wine_d3dddi_openallocationinfo32
{
    uint32_t allocation;
    uint32_t private_driver_data;
    uint32_t private_driver_data_size;
};

struct wine_d3dddi_openallocationinfo2_32
{
    uint32_t allocation;
    uint32_t private_driver_data;
    uint32_t private_driver_data_size;
    uint32_t alignment_padding;
    uint64_t gpu_address;
    uint32_t reserved[6];
};

struct wine_d3dkmt_open_resource32_desc
{
    uint32_t device;
    uint32_t global_share;
    uint32_t allocation_count;
    uint32_t allocation_info;
    uint32_t private_runtime_data;
    uint32_t private_runtime_data_size;
    uint32_t resource_private_driver_data;
    uint32_t resource_private_driver_data_size;
    uint32_t total_private_driver_data;
    uint32_t total_private_driver_data_size;
    uint32_t resource;
};

struct wine_d3dkmt_open_resource_nt32_desc
{
    uint32_t device;
    uint32_t nt_handle;
    uint32_t allocation_count;
    uint32_t allocation_info;
    uint32_t private_runtime_data_size;
    uint32_t private_runtime_data;
    uint32_t resource_private_driver_data_size;
    uint32_t resource_private_driver_data;
    uint32_t total_private_driver_data_size;
    uint32_t total_private_driver_data;
    uint32_t resource;
    uint32_t keyed_mutex;
    uint32_t keyed_mutex_private_runtime_data;
    uint32_t keyed_mutex_private_runtime_data_size;
    uint32_t sync_object;
};

/* Fixed-width private syscall packet.  Every guest_* field is an untrusted
 * 32-bit address in the current WoW64 process; it is never a host pointer. */
struct wine_d3dkmt_open_resource32
{
    uint32_t version;
    uint32_t size;
    uint32_t variant;
    uint32_t guest_desc;
    uint32_t guest_allocations;
    uint32_t guest_private_runtime;
    uint32_t guest_resource_private;
    uint32_t guest_total_private;
    uint32_t guest_keyed_mutex_runtime;
    uint32_t device;
    uint32_t shared_handle;
    uint32_t allocation_count;
    uint32_t private_runtime_data_size;
    uint32_t resource_private_driver_data_size;
    uint32_t total_private_driver_data_size;
    uint32_t keyed_mutex_private_runtime_data_size;
    uint32_t resource;
    uint32_t keyed_mutex;
    uint32_t sync_object;
    int32_t fault_status;
    uint32_t reserved[2];
};

C_ASSERT( sizeof(struct wine_d3dddi_openallocationinfo32) == 12 );
C_ASSERT( sizeof(struct wine_d3dddi_openallocationinfo2_32) == 48 );
C_ASSERT( offsetof(struct wine_d3dddi_openallocationinfo2_32, gpu_address) == 16 );
C_ASSERT( sizeof(struct wine_d3dkmt_open_resource32_desc) == 44 );
C_ASSERT( offsetof(struct wine_d3dkmt_open_resource32_desc, resource) == 40 );
C_ASSERT( sizeof(struct wine_d3dkmt_open_resource_nt32_desc) == 60 );
C_ASSERT( offsetof(struct wine_d3dkmt_open_resource_nt32_desc, resource) == 40 );
C_ASSERT( offsetof(struct wine_d3dkmt_open_resource_nt32_desc, sync_object) == 56 );
C_ASSERT( sizeof(struct wine_d3dkmt_open_resource32) == 88 );
C_ASSERT( offsetof(struct wine_d3dkmt_open_resource32, guest_desc) == 12 );
C_ASSERT( offsetof(struct wine_d3dkmt_open_resource32, fault_status) == 76 );
C_ASSERT( offsetof(struct wine_d3dkmt_open_resource32, reserved) == 80 );

#ifdef WINE_UNIX_LIB
NTSTATUS WINAPI __wine_win32u_d3dkmt_lifecycle(
    struct wine_d3dkmt_lifecycle32 *params );
NTSTATUS WINAPI __wine_win32u_d3dkmt_create_allocation(
    struct wine_d3dkmt_create_allocation32 *params );
NTSTATUS WINAPI __wine_win32u_d3dkmt_open_resource(
    struct wine_d3dkmt_open_resource32 *params );
#else
DECLSPEC_IMPORT NTSTATUS WINAPI __wine_win32u_d3dkmt_lifecycle(
    struct wine_d3dkmt_lifecycle32 *params );
DECLSPEC_IMPORT NTSTATUS WINAPI __wine_win32u_d3dkmt_create_allocation(
    struct wine_d3dkmt_create_allocation32 *params );
DECLSPEC_IMPORT NTSTATUS WINAPI __wine_win32u_d3dkmt_open_resource(
    struct wine_d3dkmt_open_resource32 *params );
#endif

#endif /* __WINE_D3DKMT_PRIVATE_H */
