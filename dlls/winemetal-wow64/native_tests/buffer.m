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
#include <setjmp.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>

#include "buffer.h"

#define STATUS_INVALID_PARAMETER_VALUE ((wmt_status_t)0xc000000d)
#define STATUS_NO_MEMORY_VALUE         ((wmt_status_t)0xc0000017)
#define STATUS_UNSUCCESSFUL_VALUE      ((wmt_status_t)0xc0000001)
#define STATUS_INVALID_DEVICE_STATE_VALUE ((wmt_status_t)0xc0000184)
#define STATUS_INVALID_HANDLE_VALUE    ((wmt_status_t)0xc0000008)

static unsigned int failures, releases;
static wmt_uint64_t expected_lease;
static wmt_status_t release_result;
static NSUInteger observed_metal_length;
static _Atomic unsigned int snapshot_releases;
static jmp_buf *active_exception_jmp;
static BOOL fault_dispatch_create, fault_gpu_address;
static BOOL fault_buffer_query, fault_texture_query;
static BOOL fault_buffer_release;
static id escaped_fault_buffer;
static _Atomic unsigned int event_signals;

#define ok(condition, message) \
    do { if (!(condition)) { fprintf( stderr, "%s:%u: %s\n", __FILE__, __LINE__, message ); \
         ++failures; } } while (0)

void ntdll_set_exception_jmp_buf( jmp_buf jmp )
{
    if (jmp)
    {
        ok( !active_exception_jmp, "nested exception jump target was installed" );
        if (active_exception_jmp) abort();
        active_exception_jmp = (jmp_buf *)jmp;
    }
    else active_exception_jmp = NULL;
}

wmt_status_t wmt_guarded_call( wmt_guarded_call_func func, void *context )
{
    jmp_buf jmp;
    wmt_status_t status;

    if (!func) return STATUS_INVALID_PARAMETER_VALUE;
    if (setjmp( jmp ))
    {
        ntdll_set_exception_jmp_buf( NULL );
        return (wmt_status_t)0xc0000005;
    }
    ntdll_set_exception_jmp_buf( jmp );
    status = func( context );
    ntdll_set_exception_jmp_buf( NULL );
    return status;
}

wmt_status_t NtSetEvent( void *handle, void *prev_state )
{
    (void)prev_state;
    if (!handle) return STATUS_INVALID_PARAMETER_VALUE;
    atomic_fetch_add_explicit( &event_signals, 1, memory_order_release );
    return 0;
}

dispatch_data_t wmt_test_dispatch_data_create( const void *bytes, size_t length )
{
    if (fault_dispatch_create)
    {
        ok( active_exception_jmp != NULL, "dispatch-data fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    return dispatch_data_create( bytes, length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT );
}

static wmt_status_t release_alias( wmt_uint64_t lease )
{
    ok( lease == expected_lease, "helper released the wrong lease" );
    ++releases;
    return release_result;
}

static void release_snapshot( void *bytes, uint64_t length )
{
    ok( bytes != NULL && length == 4, "dispatch snapshot release contract changed" );
    atomic_fetch_add_explicit( &snapshot_releases, 1, memory_order_release );
    free( bytes );
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
- (NSUInteger)length;
- (void *)contents;
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
- (NSUInteger)length
{
    return length;
}
- (void *)contents
{
    if (fault_buffer_query)
    {
        ok( active_exception_jmp != NULL, "buffer query fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    return bytes;
}
- (MTLGPUAddress)gpuAddress
{
    if (fault_gpu_address)
    {
        ok( active_exception_jmp != NULL, "GPU-address fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    return 0xfeedfacecafebeefull;
}
- (oneway void)release
{
    if (fault_buffer_release)
    {
        ok( active_exception_jmp != NULL, "Metal release fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    [super release];
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

@interface WmtTestSharedEvent : NSObject
{
    NSMutableArray *callbacks;
    BOOL fault_after_callback;
}
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value
                 block:(MTLSharedEventNotificationBlock)block;
- (void)fireAll;
- (void)setFaultAfterCallback:(BOOL)value;
@end

@implementation WmtTestSharedEvent
- (id)init
{
    if (!(self = [super init])) return nil;
    callbacks = [[NSMutableArray alloc] init];
    return self;
}
- (void)notifyListener:(MTLSharedEventListener *)listener atValue:(uint64_t)value
                 block:(MTLSharedEventNotificationBlock)block
{
    MTLSharedEventNotificationBlock copy;

    (void)listener;
    if (fault_after_callback)
    {
        block( (id<MTLSharedEvent>)self, value );
        ok( active_exception_jmp != NULL,
            "post-callback notification fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    copy = [block copy];
    [callbacks addObject:copy];
    [copy release];
}
- (void)fireAll
{
    NSArray *pending = [callbacks copy];
    MTLSharedEventNotificationBlock callback;

    [callbacks removeAllObjects];
    for (callback in pending) callback((id<MTLSharedEvent>)self, 1);
    [pending release];
}
- (void)setFaultAfterCallback:(BOOL)value
{
    fault_after_callback = value;
}
- (void)dealloc
{
    [callbacks release];
    [super dealloc];
}
@end

@interface WmtTestTexture : NSObject
@end

@implementation WmtTestTexture
- (NSUInteger)width
{
    if (fault_texture_query)
    {
        ok( active_exception_jmp != NULL, "texture query fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    return 16;
}
- (NSUInteger)height { return 7; }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return 5; }
- (NSUInteger)arrayLength { return 1; }
- (NSUInteger)sampleCount { return 1; }
- (BOOL)isFramebufferOnly { return NO; }
- (MTLStorageMode)storageMode { return MTLStorageModeManaged; }
- (MTLTextureType)textureType { return MTLTextureType2D; }
- (MTLPixelFormat)pixelFormat { return MTLPixelFormatBC1_RGBA; }
@end

@interface WmtPrivateTexture : WmtTestTexture
@end
@implementation WmtPrivateTexture
- (MTLStorageMode)storageMode { return MTLStorageModePrivate; }
@end

@interface WmtMemorylessTexture : WmtTestTexture
@end
@implementation WmtMemorylessTexture
- (MTLStorageMode)storageMode { return MTLStorageModeMemoryless; }
@end

@interface WmtSharedTexture : WmtTestTexture
@end
@implementation WmtSharedTexture
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
@end

@interface WmtFramebufferTexture : WmtTestTexture
@end
@implementation WmtFramebufferTexture
- (BOOL)isFramebufferOnly { return YES; }
@end

@interface WmtMultisampleTexture : WmtTestTexture
@end
@implementation WmtMultisampleTexture
- (NSUInteger)sampleCount { return 4; }
- (MTLTextureType)textureType { return MTLTextureType2DMultisample; }
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
    if (mode == 4)
    {
        ok( active_exception_jmp != NULL, "Metal allocation fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    if (mode == 5)
    {
        escaped_fault_buffer = [[WmtTestBuffer alloc] initWithBytes:pointer length:new_length
                                                            callback:deallocator];
        ok( active_exception_jmp != NULL, "post-create Metal fault lacked an exception guard" );
        if (!active_exception_jmp) abort();
        longjmp( *active_exception_jmp, 1 );
    }
    if (mode == 6)
    {
        WmtTestBuffer *buffer = [[WmtTestBuffer alloc] initWithBytes:pointer length:new_length
                                                             callback:deallocator];

        escaped_fault_buffer = [buffer retain];
        return (id<MTLBuffer>)buffer;
    }
    if (mode == 7)
    {
        WmtTestBuffer *buffer = [[WmtTestBuffer alloc] initWithBytes:pointer length:new_length
                                                             callback:deallocator];

        escaped_fault_buffer = buffer;
        return (id<MTLBuffer>)buffer;
    }
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
    WmtTestBuffer *length_buffer;
    WmtTestSharedEvent *shared_event;
    WmtTestTexture *texture;
    pthread_t release_thread;
    int thread_status;
    wmt_status_t status;
    unsigned int i;
    uint64_t dispatch_object;
    uint64_t length, row_bytes, rows;
    uint64_t listener_token;
    void *snapshot;

    if (posix_memalign( &page, page_size, page_size )) return 2;

    snapshot = malloc( 4 );
    memcpy( snapshot, "MTLB", 4 );
    fault_dispatch_create = TRUE;
    status = wmt_dispatch_data_from_snapshot( snapshot, 4, release_snapshot,
                                              &dispatch_object );
    fault_dispatch_create = FALSE;
    ok( status == (wmt_status_t)0xc0000005 && !dispatch_object &&
        atomic_load_explicit( &snapshot_releases, memory_order_acquire ) == 1,
        "faulting dispatch-data copy did not release its snapshot exactly once" );
    snapshot = malloc( 4 );
    memcpy( snapshot, "MTLB", 4 );
    status = wmt_dispatch_data_from_snapshot( snapshot, 4, release_snapshot,
                                              &dispatch_object );
    ok( !status && dispatch_object &&
        atomic_load_explicit( &snapshot_releases, memory_order_acquire ) == 2,
        "dispatch data did not synchronously copy and release its snapshot" );
    if (dispatch_object) dispatch_release( (dispatch_data_t)(uintptr_t)dispatch_object );
    ok( atomic_load_explicit( &snapshot_releases, memory_order_acquire ) == 2,
        "dispatch data destruction released the snapshot more than once" );

    length_buffer = [[WmtTestBuffer alloc] initWithBytes:page length:64 callback:nil];
    fault_buffer_query = TRUE;
    length = ~0ull;
    status = wmt_metal_buffer_length( (uint64_t)(uintptr_t)length_buffer, &length );
    fault_buffer_query = FALSE;
    ok( status == (wmt_status_t)0xc0000005 && !length,
        "faulting Metal buffer query escaped its guard" );
    status = wmt_metal_buffer_length( (uint64_t)(uintptr_t)length_buffer, &length );
    ok( !status && length == 64, "Metal buffer length query failed" );
    [length_buffer release];
    length_buffer = [[WmtTestBuffer alloc] initWithBytes:NULL length:64 callback:nil];
    length = ~0ull;
    status = wmt_metal_buffer_length( (uint64_t)(uintptr_t)length_buffer, &length );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !length,
        "non-CPU-addressable Metal buffer was accepted" );
    [length_buffer release];
    texture = [[WmtTestTexture alloc] init];
    fault_texture_query = TRUE;
    rows = row_bytes = ~0ull;
    status = wmt_metal_texture_snapshot_rows( (uint64_t)(uintptr_t)texture,
                                              0, 0, 0, 16, 7, 1, 0, 0,
                                              32, 0, &rows, &row_bytes );
    fault_texture_query = FALSE;
    ok( status == (wmt_status_t)0xc0000005 && !rows && !row_bytes,
        "faulting Metal texture query escaped its guard" );
    status = wmt_metal_texture_snapshot_rows( (uint64_t)(uintptr_t)texture,
                                              0, 0, 0, 16, 7, 1, 0, 0,
                                              32, 0, &rows, &row_bytes );
    ok( !status && rows == 2 && row_bytes == 32,
        "compressed Metal row layout was not block aware" );
    status = wmt_metal_texture_snapshot_rows( (uint64_t)(uintptr_t)texture,
                                              0, 4, 0, 16, 7, 1, 0, 0,
                                              32, 0, &rows, &row_bytes );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !rows && !row_bytes,
        "out-of-range Metal texture region was accepted" );
    [texture release];
    texture = (WmtTestTexture *)[[WmtPrivateTexture alloc] init];
    status = wmt_metal_texture_snapshot_rows( (uint64_t)(uintptr_t)texture,
                                              0, 0, 0, 16, 7, 1, 0, 0,
                                              32, 0, &rows, &row_bytes );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !rows && !row_bytes,
        "private Metal texture accepted a CPU replace layout" );
    [texture release];
    texture = (WmtTestTexture *)[[WmtMemorylessTexture alloc] init];
    status = wmt_metal_texture_snapshot_rows( (uint64_t)(uintptr_t)texture,
                                              0, 0, 0, 16, 7, 1, 0, 0,
                                              32, 0, &rows, &row_bytes );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !rows && !row_bytes,
        "memoryless Metal texture accepted a CPU replace layout" );
    [texture release];
    texture = (WmtTestTexture *)[[WmtFramebufferTexture alloc] init];
    status = wmt_metal_texture_snapshot_rows( (uint64_t)(uintptr_t)texture,
                                              0, 0, 0, 16, 7, 1, 0, 0,
                                              32, 0, &rows, &row_bytes );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !rows && !row_bytes,
        "framebuffer-only Metal texture accepted a CPU replace layout" );
    [texture release];
    texture = (WmtTestTexture *)[[WmtMultisampleTexture alloc] init];
    status = wmt_metal_texture_snapshot_rows( (uint64_t)(uintptr_t)texture,
                                              0, 0, 0, 16, 7, 1, 0, 0,
                                              32, 0, &rows, &row_bytes );
    ok( status == STATUS_INVALID_PARAMETER_VALUE && !rows && !row_bytes,
        "multisample Metal texture accepted a CPU replace layout" );
    [texture release];
    texture = (WmtTestTexture *)[[WmtSharedTexture alloc] init];
    status = wmt_metal_texture_snapshot_rows( (uint64_t)(uintptr_t)texture,
                                              0, 0, 0, 16, 7, 1, 0, 0,
                                              32, 0, &rows, &row_bytes );
    ok( !status && rows == 2 && row_bytes == 32,
        "shared Metal texture rejected a CPU replace layout" );
    [texture release];

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

    device = [[WmtTestDevice alloc] initWithMode:4];
    expected_lease = 12;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( status == (wmt_status_t)0xc0000005 && releases == 5 && !ret && !gpu,
        "pre-object Metal allocation fault unsafely recycled ambiguous backing" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:5];
    expected_lease = 14;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( status == (wmt_status_t)0xc0000005 && releases == 5 && escaped_fault_buffer &&
        !ret && !gpu, "post-create Metal fault released backing before its buffer" );
    [escaped_fault_buffer release];
    escaped_fault_buffer = nil;
    ok( releases == 6, "delayed post-create Metal deallocator did not release once" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:0];
    expected_lease = 7;
    ret = gpu = 0;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( !status && ret && gpu == 0xfeedfacecafebeefull && releases == 6 &&
        observed_metal_length == 1,
        "successful helper did not preserve logical length or defer lease release" );
    [(id)(uintptr_t)ret release];
    ok( releases == 7, "buffer destruction did not release once" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:6];
    expected_lease = 13;
    fault_gpu_address = TRUE;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    fault_gpu_address = FALSE;
    ok( status == (wmt_status_t)0xc0000005 && releases == 7 && escaped_fault_buffer &&
        !ret && !gpu, "faulting GPU-address query released multiply-retained backing early" );
    [escaped_fault_buffer release];
    escaped_fault_buffer = nil;
    ok( releases == 8, "delayed GPU-fault buffer deallocator did not release once" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:7];
    expected_lease = 15;
    fault_gpu_address = TRUE;
    fault_buffer_release = TRUE;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    fault_buffer_release = FALSE;
    fault_gpu_address = FALSE;
    ok( status == (wmt_status_t)0xc0000005 && releases == 8 && escaped_fault_buffer &&
        !ret && !gpu, "faulting Metal release recycled backing while the buffer survived" );
    [escaped_fault_buffer release];
    escaped_fault_buffer = nil;
    ok( releases == 9, "buffer surviving a release fault did not release once later" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:0];
    expected_lease = 8;
    release_result = STATUS_UNSUCCESSFUL_VALUE;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( !status && releases == 9, "release failure test did not create a buffer" );
    [(id)(uintptr_t)ret release];
    ok( releases == 10, "void deallocator retried or skipped a failed release" );
    release_result = 0;
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:3];
    expected_lease = 9;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( status == STATUS_INVALID_DEVICE_STATE_VALUE && releases == 10 && !ret && !gpu,
        "synchronous deallocator with a nonnil buffer was published" );
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:2];
    expected_lease = 10;
    release_result = STATUS_UNSUCCESSFUL_VALUE;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( status == STATUS_UNSUCCESSFUL_VALUE && releases == 11 && !ret && !gpu,
        "synchronous release failure was not surfaced without publishing" );
    release_result = 0;
    [device release];

    device = [[WmtTestDevice alloc] initWithMode:0];
    expected_lease = 11;
    status = wmt_metal_buffer_from_alias( (wmt_uint64_t)(uintptr_t)device, page, 1,
                                          page_size, 0, expected_lease, release_alias, &ret, &gpu );
    ok( !status && ret && releases == 11,
        "foreign-thread test did not create a buffer" );
    thread_status = pthread_create( &release_thread, NULL, release_buffer_on_foreign_thread,
                                    (void *)(uintptr_t)ret );
    ok( !thread_status, "foreign-thread buffer release did not start" );
    if (!thread_status) thread_status = pthread_join( release_thread, NULL );
    ok( !thread_status && releases == 12,
        "foreign-thread Metal deallocator did not release exactly once" );
    [device release];

    shared_event = [[WmtTestSharedEvent alloc] init];
    listener_token = 0;
    status = wmt_shared_event_listener_create( &listener_token );
    ok( !status && listener_token,
        "post-callback registration-fault test did not create a listener" );
    [shared_event setFaultAfterCallback:YES];
    status = wmt_shared_event_notify_win32(
        (uint64_t)(uintptr_t)shared_event, 1, listener_token, 1 );
    ok( status == (wmt_status_t)0xc0000005,
        "post-callback registration fault escaped its guard" );
    ok( !wmt_test_shared_event_listener_pending( listener_token ),
        "failed registration published a synchronously-fired callback" );
    [shared_event setFaultAfterCallback:NO];
    ok( !wmt_shared_event_listener_destroy( listener_token ),
        "post-callback registration-fault listener did not destroy" );
    for (i = 0; i < 128; i++)
    {
        listener_token = 0;
        status = wmt_shared_event_listener_create( &listener_token );
        ok( !status && listener_token, "shared-event listener slot did not recycle" );
        if (status || !listener_token) break;
        status = wmt_shared_event_notify_win32(
            (uint64_t)(uintptr_t)shared_event, (uint64_t)i + 1,
            listener_token, (uint64_t)i + 1 );
        ok( !status, "shared-event notification registration failed" );
        status = wmt_shared_event_listener_destroy( listener_token );
        ok( !status, "shared-event listener destruction failed" );
        status = wmt_shared_event_listener_destroy( listener_token );
        ok( status == STATUS_INVALID_HANDLE_VALUE,
            "stale shared-event listener token was accepted" );
    }
    [shared_event fireAll];
    ok( !atomic_load_explicit( &event_signals, memory_order_acquire ),
        "late shared-event callbacks escaped destroyed token generations" );
    [shared_event release];

    listener_token = 0;
    status = wmt_shared_event_listener_create( &listener_token );
    ok( !status && listener_token, "listener quiesce test did not create a token" );
    status = wmt_quiesce_shared_event_listeners();
    ok( !status, "shared-event listener quiesce failed" );
    status = wmt_shared_event_listener_destroy( listener_token );
    ok( status == STATUS_INVALID_HANDLE_VALUE,
        "quiesce left its shared-event listener token live" );
    listener_token = ~0ull;
    status = wmt_shared_event_listener_create( &listener_token );
    ok( status == STATUS_INVALID_DEVICE_STATE_VALUE && !listener_token,
        "listener creation ignored the quiescing state" );
    wmt_resume_shared_event_listeners();
    status = wmt_shared_event_listener_create( &listener_token );
    ok( !status && listener_token, "listener creation did not resume after quiesce" );
    if (!status && listener_token)
        ok( !wmt_shared_event_listener_destroy( listener_token ),
            "resumed shared-event listener did not destroy" );

    free( page );
    if (failures) return 1;
    puts( "winemetal Metal alias helper native tests passed" );
    return 0;
}
