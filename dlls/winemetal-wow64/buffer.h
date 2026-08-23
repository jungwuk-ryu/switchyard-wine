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

#ifndef __WINEMETAL_WOW64_BUFFER_H
#define __WINEMETAL_WOW64_BUFFER_H

#ifdef __OBJC__
typedef int wmt_status_t;
typedef unsigned long wmt_uint64_t;
#else
# include "ntstatus.h"
# include "windef.h"
# include "winternl.h"
typedef NTSTATUS wmt_status_t;
typedef UINT64 wmt_uint64_t;
#endif

typedef wmt_status_t (*wmt_alias_release_func)(wmt_uint64_t lease);

/* A non-NULL release callback and nonzero lease are preconditions; valid ownership is consumed once. */
wmt_status_t wmt_metal_buffer_from_alias( wmt_uint64_t device, void *address,
                                          wmt_uint64_t logical_length, wmt_uint64_t mapped_length,
                                          wmt_uint64_t options, wmt_uint64_t lease,
                                          wmt_alias_release_func release_alias,
                                          wmt_uint64_t *ret_buffer,
                                          wmt_uint64_t *ret_gpu_address );

#endif /* __WINEMETAL_WOW64_BUFFER_H */
