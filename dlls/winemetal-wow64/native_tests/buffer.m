/*
 * Winemetal Metal owned-memory helper native tests
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep standalone
#endif

#include "config.h"

#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "buffer.h"

#define STATUS_INVALID_PARAMETER_VALUE ((wmt_status_t)0xc000000d)
#define STATUS_NO_MEMORY_VALUE         ((wmt_status_t)0xc0000017)
#define STATUS_UNSUCCESSFUL_VALUE      ((wmt_status_t)0xc0000001)
#define STATUS_INVALID_DEVICE_STATE_VALUE ((wmt_status_t)0xc0000184)

static unsigned int failures, releases;
static wmt_uint64_t expected_lease;
static wmt_status_t release_result;
static NSUInteger observed_metal_length;

#define ok(condition, message) \
    do { if (!(condition)) { fprintf( stderr, "%s:%u: %s\n", __FILE__, __LINE__, message ); \
         ++failures; } } while (0)

static wmt_status_t release_alias( wmt_uint64_t lease )
{
    ok( lease == expected_lease, "helper released the wrong lease" );
    ++releases;
    return release_result;
}

static void *release_buffer_on_foreign_thread( void *arg )
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    [(id)arg release];
    [pool drain];
    return NULL;
}

@interface WmtTestBuffer : NSObject
{
    void (^callback)(void *, NSUInteger);
    void *bytes;
    NSUInteger length;
}
- (id)initWithBytes:(void *)new_bytes length:(NSUInteger)new_length
          callback:(void (^)(void *, NSUInteger))new_callback;
- (MTLGPUAddress)gpuAddress;
@end

@implementation WmtTestBuffer
- (id)initWithBytes:(void *)new_bytes length:(NSUInteger)new_length
          callback:(void (^)(void *, NSUInteger))new_callback
{
    if (!(self = [super init])) return nil;
    bytes = new_bytes;
    length = new_length;
    callback = [new_callback copy];
    return self;
}
- (MTLGPUAddress)gpuAddress
{
    return 0xfeedfacecafebeefull;
}
- (void)dealloc
{
    if (callback)
    {
        callback( bytes, length );
        [callback release];
    }
    [super dealloc];
}
@end

@interface WmtTestDevice : NSObject
{
    unsigned int mode;
}
- (id)initWithMode:(unsigned int)new_mode;
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)new_length
                                  options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *, NSUInteger))deallocator;
@end

@implementation WmtTestDevice
- (id)initWithMode:(unsigned int)new_mode
{
    if (!(self = [super init])) return nil;
    mode = new_mode;
    return self;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)new_length
                                  options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *, NSUInteger))deallocator
{
    (void)options;
    observed_metal_length = new_length;
    if (mode == 1)
    {
        deallocator( pointer, new_length );
        return nil;
    }
    if (mode == 2) return nil;
    if (mode == 3)
    {
        deallocator( pointer, new_length );
        return (id<MTLBuffer>)[[WmtTestBuffer alloc] initWithBytes:pointer length:new_length
                                                         callback:nil];
    }
    return (id<MTLBuffer>)[[WmtTestBuffer alloc] initWithBytes:pointer length:new_length
                                                     callback:deallocator];
}
@end

int main(void)
{
    wmt_uint64_t ret = ~0ul, gpu = ~0ul;
    size_t page_size = getpagesize();
    void *page = NULL;
    WmtTestDevice *device;
    pthread_t release_thread;
    int thread_status;
    wmt_status_t status;

    if (posix_memalign( &page, page_size, page_size )) return 2;

    expected_lease = 1;
    status = wmt_metal_buffer_from_alias( 0, page, 1, page_size, 0, expected_lease,
                                          NULL, &ret, &gpu );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !ret && !gpu && !releases,
        "NULL release precondition changed" );

    expected_lease = 0;
    status = wmt_metal_buffer_from_alias( 1, page, 1, page_size, 0, 0,
                                          release_alias, &ret, &gpu );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !ret && !gpu && !releases,
        "zero lease was accepted or released" );

    expected_lease = 2;
    ret = gpu = ~0ul;
    status = wmt_metal_buffer_from_alias( 0, page, 1, page_size, 0, expected_lease,
                                          release_alias, &ret, &gpu );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !ret && !gpu && releases == 1,
        "invalid device did not consume one lease" );

    expected_lease = 3;
    status = wmt_metal_buffer_from_alias( 1, page, 1, page_size * 2, 0, expected_lease,
                                          release_alias, &ret, &gpu );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && releases == 2,
        "non-canonical mapped length did not consume one lease" );

    expected_lease = 4;
    status = wmt_metal_buffer_from_alias( 1, (char *)page + 1, 1, page_size, 0,
                                          expected_lease, release_alias, &ret, &gpu );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && releases == 3,
        "misaligned alias did not consume one lease" );

    device = [[WmtTestDevice alloc] initWithMode:1];
    expected_lease = 5;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( status == STATUS_NO_MEMORY_VALUE && releases == 4,
        "synchronous deallocator plus nil return released more than once" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:2];
    expected_lease = 6;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( status == STATUS_NO_MEMORY_VALUE && releases == 5,
        "nil return without deallocator did not release once" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:0];
    expected_lease = 7;
    ret = gpu = 0;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( !status && ret && gpu == 0xfeedfacecafebeefull && releases == 5 &&
        observed_metal_length == 1,
        "successful helper did not preserve logical length or defer lease release" );
    [(id)(uintptr_t)ret release];
    ok( releases == 6, "buffer destruction did not release once" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:0];
    expected_lease = 8;
    release_result = STATUS_UNSUCCESSFUL_VALUE;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( !status && releases == 6, "release failure test did not create a buffer" );
    [(id)(uintptr_t)ret release];
    ok( releases == 7, "void deallocator retried or skipped a failed release" );
    release_result = 0;
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:3];
    expected_lease = 9;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( status == STATUS_INVALID_DEVICE_STATE_VALUE && releases == 8 && !ret && !gpu,
        "synchronous deallocator with a nonnil buffer was published" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:2];
    expected_lease = 10;
    release_result = STATUS_UNSUCCESSFUL_VALUE;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( status == STATUS_UNSUCCESSFUL_VALUE && releases == 9 && !ret && !gpu,
        "synchronous release failure was not surfaced without publishing" );
    release_result = 0;
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:0];
    expected_lease = 11;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( !status && ret && releases == 9,
        "foreign-thread test did not create a buffer" );
    thread_status = pthread_create( &release_thread, NULL, release_buffer_on_foreign_thread,
                                    (void *)(uintptr_t)ret );
    ok( !thread_status, "foreign-thread buffer release did not start" );
    if (!thread_status) thread_status = pthread_join( release_thread, NULL );
    ok( !thread_status && releases == 10,
        "foreign-thread Metal deallocator did not release exactly once" );
    [device release];

    free( page );
    if (failures) return 1;
    puts( "winemetal Metal alias helper native tests passed" );
    return 0;
}
