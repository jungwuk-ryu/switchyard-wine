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

#include <stdint.h>

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
typedef void (*wmt_snapshot_release_func)(void *bytes, uint64_t length);
typedef wmt_status_t (*wmt_guarded_call_func)(void *context);

/* Invoke faultable native/Objective-C code on the current Unixlib stack. */
wmt_status_t wmt_guarded_call( wmt_guarded_call_func func, void *context );

/* Make callback code in this companion process-resident before it escapes to Metal. */
wmt_status_t wmt_pin_companion_image_resident(void);

/* A non-NULL release callback and nonzero lease are preconditions; valid ownership is consumed once. */
wmt_status_t wmt_metal_buffer_from_alias( wmt_uint64_t device, void *address,
                                          wmt_uint64_t logical_length, wmt_uint64_t mapped_length,
                                          wmt_uint64_t options, wmt_uint64_t lease,
                                          wmt_alias_release_func release_alias,
                                          wmt_uint64_t *ret_buffer,
                                          wmt_uint64_t *ret_gpu_address );

/* Consumes bytes exactly once on every path after entry. */
wmt_status_t wmt_dispatch_data_from_snapshot( void *bytes, uint64_t length,
                                              wmt_snapshot_release_func release_snapshot,
                                              uint64_t *ret_data );

/* Validate a CPU replace layout and return its physical row count and minimum row span. */
wmt_status_t wmt_metal_texture_snapshot_rows( uint64_t texture, uint64_t origin_x,
                                              uint64_t origin_y, uint64_t origin_z,
                                              uint64_t width, uint64_t height,
                                              uint64_t depth, uint64_t level, uint64_t slice,
                                              uint64_t bytes_per_row, uint64_t bytes_per_image,
                                              uint64_t *ret_rows, uint64_t *ret_row_bytes );

/* Return the allocation length owned by a native Metal buffer. */
wmt_status_t wmt_metal_buffer_length( uint64_t buffer, uint64_t *ret_length );

/* Companion-owned shared-event listener capabilities; tokens are never host pointers. */
wmt_status_t wmt_shared_event_listener_create( uint64_t *ret_token );
wmt_status_t wmt_shared_event_listener_start( uint64_t token );
wmt_status_t wmt_shared_event_listener_destroy( uint64_t token );
wmt_status_t wmt_shared_event_notify_win32( uint64_t shared_event, uint64_t event_handle,
                                            uint64_t listener_token, uint64_t value );
wmt_status_t wmt_quiesce_shared_event_listeners(void);
void wmt_resume_shared_event_listeners(void);

#ifdef WMT_NATIVE_TEST
unsigned int wmt_test_shared_event_listener_pending( uint64_t token );
#endif

#endif /* __WINEMETAL_WOW64_BUFFER_H */
