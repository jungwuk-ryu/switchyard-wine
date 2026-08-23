/*
 * Winemetal Metal owned-memory buffer helper
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

#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#import <Metal/Metal.h>

#include "buffer.h"

#define WMT_STATUS_SUCCESS            ((wmt_status_t)0x00000000)
#define WMT_STATUS_INVALID_PARAMETER  ((wmt_status_t)0xc000000d)
#define WMT_STATUS_NO_MEMORY          ((wmt_status_t)0xc0000017)
#define WMT_STATUS_INVALID_DEVICE_STATE ((wmt_status_t)0xc0000184)

static wmt_status_t release_owned_lease( wmt_uint64_t lease,
                                         wmt_alias_release_func release_alias,
                                         wmt_status_t status )
{
    wmt_status_t release_status = release_alias( lease );

    return release_status ? release_status : status;
}

wmt_status_t wmt_metal_buffer_from_alias( wmt_uint64_t device, void *address,
                                          wmt_uint64_t logical_length, wmt_uint64_t mapped_length,
                                          wmt_uint64_t options, wmt_uint64_t lease,
                                          wmt_alias_release_func release_alias,
                                          wmt_uint64_t *ret_buffer, wmt_uint64_t *ret_gpu_address )
{
    __block _Atomic wmt_uint64_t owned_lease = lease;
    __block _Atomic wmt_status_t owned_release_status = WMT_STATUS_SUCCESS;
    id<MTLBuffer> buffer;
    uint64_t rounded_length;
    size_t page_size;

    if (ret_buffer) *ret_buffer = 0;
    if (ret_gpu_address) *ret_gpu_address = 0;
    if (!release_alias || !lease) return WMT_STATUS_INVALID_PARAMETER;

    page_size = getpagesize();
    if (!ret_buffer || !ret_gpu_address || !device || !address || !logical_length ||
        mapped_length < logical_length || mapped_length > NSUIntegerMax ||
        !page_size || (uintptr_t)address % page_size || mapped_length % page_size)
    {
        return release_owned_lease( lease, release_alias, WMT_STATUS_INVALID_PARAMETER );
    }
    rounded_length = logical_length;
    if (rounded_length % page_size)
    {
        uint64_t increment = page_size - rounded_length % page_size;

        if (rounded_length > UINT64_MAX - increment)
        {
            return release_owned_lease( lease, release_alias, WMT_STATUS_INVALID_PARAMETER );
        }
        rounded_length += increment;
        if (rounded_length != mapped_length)
        {
            return release_owned_lease( lease, release_alias, WMT_STATUS_INVALID_PARAMETER );
        }
    }
    else if (rounded_length != mapped_length)
    {
        return release_owned_lease( lease, release_alias, WMT_STATUS_INVALID_PARAMETER );
    }

    buffer = [(id<MTLDevice>)(uintptr_t)device
        newBufferWithBytesNoCopy:address
                           length:(NSUInteger)logical_length
                          options:(MTLResourceOptions)options
                      deallocator:^(void *bytes, NSUInteger length)
                      {
                          wmt_uint64_t current_lease;

                          (void)bytes;
                          (void)length;
                          current_lease = atomic_exchange_explicit( &owned_lease, 0,
                                                                    memory_order_acq_rel );
                          /* This void Metal callback cannot propagate or safely retry a
                           * release; the codec owns any failure recovery contract. */
                          if (current_lease)
                          {
                              wmt_status_t status = release_alias( current_lease );

                              if (status) atomic_store_explicit( &owned_release_status, status,
                                                                 memory_order_release );
                          }
                      }];
    if (!buffer)
    {
        wmt_uint64_t current_lease;
        wmt_status_t release_status;

        current_lease = atomic_exchange_explicit( &owned_lease, 0, memory_order_acq_rel );
        if (current_lease)
        {
            release_status = release_alias( current_lease );
            if (release_status) atomic_store_explicit( &owned_release_status, release_status,
                                                        memory_order_release );
        }
        release_status = atomic_load_explicit( &owned_release_status, memory_order_acquire );
        return release_status ? release_status : WMT_STATUS_NO_MEMORY;
    }
    if (!atomic_load_explicit( &owned_lease, memory_order_acquire ))
    {
        wmt_status_t release_status;

        /* A no-copy buffer cannot remain valid after an early deallocator.
         * Consume the returned object while the wrapper still owns it. */
        [buffer release];
        release_status = atomic_load_explicit( &owned_release_status, memory_order_acquire );
        return release_status ? release_status : WMT_STATUS_INVALID_DEVICE_STATE;
    }

    *ret_buffer = (wmt_uint64_t)(uintptr_t)buffer;
    *ret_gpu_address = (wmt_uint64_t)[buffer gpuAddress];
    return WMT_STATUS_SUCCESS;
}
