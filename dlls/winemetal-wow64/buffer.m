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

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#import <CoreFoundation/CoreFoundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>

#include "buffer.h"

#define WMT_STATUS_SUCCESS            ((wmt_status_t)0x00000000)
#define WMT_STATUS_INVALID_PARAMETER  ((wmt_status_t)0xc000000d)
#define WMT_STATUS_ACCESS_VIOLATION   ((wmt_status_t)0xc0000005)
#define WMT_STATUS_NO_MEMORY          ((wmt_status_t)0xc0000017)
#define WMT_STATUS_INVALID_DEVICE_STATE ((wmt_status_t)0xc0000184)
#define WMT_STATUS_INVALID_HANDLE       ((wmt_status_t)0xc0000008)
#define WMT_STATUS_TOO_MANY_CONTEXT_IDS ((wmt_status_t)0xc000015a)

#define WMT_LISTENER_MAX 64

extern wmt_status_t NtSetEvent( void *handle, void *prev_state );

typedef wmt_status_t (^wmt_guarded_block_t)(void);

struct wmt_guarded_block_context
{
    wmt_guarded_block_t block;
};

static wmt_status_t invoke_guarded_block( void *opaque )
{
    struct wmt_guarded_block_context *context = opaque;

    return context->block();
}

static wmt_status_t invoke_guarded( wmt_guarded_block_t block )
{
    struct wmt_guarded_block_context context = {block};

    return wmt_guarded_call( invoke_guarded_block, &context );
}

enum wmt_alias_state
{
    WMT_ALIAS_CREATING,
    WMT_ALIAS_CALLBACK_SEEN,
    WMT_ALIAS_PUBLISHED,
    WMT_ALIAS_AMBIGUOUS,
    WMT_ALIAS_RELEASING,
    WMT_ALIAS_SETTLED,
    WMT_ALIAS_QUARANTINED,
};

enum wmt_listener_state
{
    WMT_LISTENER_FREE,
    WMT_LISTENER_CREATED,
    WMT_LISTENER_STARTING,
    WMT_LISTENER_RUNNING,
    WMT_LISTENER_STOPPING,
    WMT_LISTENER_RETIRED,
};

enum wmt_listener_callback_state
{
    WMT_LISTENER_CALLBACK_REGISTERING,
    WMT_LISTENER_CALLBACK_ARMED,
    WMT_LISTENER_CALLBACK_FIRED,
    WMT_LISTENER_CALLBACK_CANCELLED,
};

@class WMTListenerCallbackOwner;

struct wmt_listener_entry
{
    uint64_t token;
    enum wmt_listener_state state;
    unsigned int refs;
    BOOL closing;
    BOOL registry_owned;
    MTLSharedEventListener *listener;
    CFRunLoopRef runloop;
    CFRunLoopSourceRef source;
    WMTListenerCallbackOwner *pending;
};

struct wmt_listener_cleanup
{
    MTLSharedEventListener *listener;
    CFRunLoopRef runloop;
    CFRunLoopSourceRef source;
};

static pthread_mutex_t listener_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct wmt_listener_entry listener_entries[WMT_LISTENER_MAX];
static uint64_t next_listener_generation = 1;
static BOOL listeners_quiescing;

static struct wmt_listener_entry *listener_lookup_locked( uint64_t token )
{
    unsigned int slot;
    struct wmt_listener_entry *entry;

    if (!(slot = (unsigned int)(token & 0xff)) || slot > WMT_LISTENER_MAX)
        return NULL;
    entry = &listener_entries[slot - 1];
    return entry->state != WMT_LISTENER_FREE && entry->token == token ? entry : NULL;
}

static void listener_collect_locked( struct wmt_listener_entry *entry,
                                     struct wmt_listener_cleanup *cleanup )
{
    if (entry->refs) return;
    cleanup->listener = entry->listener;
    cleanup->runloop = entry->runloop;
    cleanup->source = entry->source;
    memset( entry, 0, sizeof(*entry) );
}

static void listener_remember_status( wmt_status_t *result, wmt_status_t status )
{
    if (!*result && status) *result = status;
}

static wmt_status_t listener_release_cleanup( struct wmt_listener_cleanup *cleanup )
{
    MTLSharedEventListener *listener = cleanup->listener;
    CFRunLoopRef runloop = cleanup->runloop;
    CFRunLoopSourceRef source = cleanup->source;
    wmt_status_t result = WMT_STATUS_SUCCESS;

    memset( cleanup, 0, sizeof(*cleanup) );
    if (source)
        listener_remember_status( &result, invoke_guarded( ^wmt_status_t{
            CFRelease( source );
            return WMT_STATUS_SUCCESS;
        } ) );
    if (runloop)
        listener_remember_status( &result, invoke_guarded( ^wmt_status_t{
            CFRelease( runloop );
            return WMT_STATUS_SUCCESS;
        } ) );
    if (listener)
        listener_remember_status( &result, invoke_guarded( ^wmt_status_t{
            [listener release];
            return WMT_STATUS_SUCCESS;
        } ) );
    return result;
}

static void listener_put( uint64_t token )
{
    struct wmt_listener_cleanup cleanup = {0};
    struct wmt_listener_entry *entry;

    pthread_mutex_lock( &listener_mutex );
    if ((entry = listener_lookup_locked( token )) && entry->refs)
    {
        entry->refs--;
        listener_collect_locked( entry, &cleanup );
    }
    pthread_mutex_unlock( &listener_mutex );
    (void)listener_release_cleanup( &cleanup );
}

@interface WMTListenerCallbackOwner : NSObject
{
@public
    uint64_t token;
    uint64_t event_handle;
    _Atomic unsigned int callback_state;
    WMTListenerCallbackOwner *next_pending;
}
@end

static BOOL listener_is_live( uint64_t token )
{
    struct wmt_listener_entry *entry;
    BOOL live;

    pthread_mutex_lock( &listener_mutex );
    entry = listener_lookup_locked( token );
    live = entry && !entry->closing;
    pthread_mutex_unlock( &listener_mutex );
    return live;
}

static void listener_deliver_owner( WMTListenerCallbackOwner *owner )
{
    struct wmt_listener_entry *entry;
    CFRunLoopRef runloop = NULL;
    CFRunLoopSourceRef source = NULL;
    BOOL retained_owner = FALSE;
    BOOL retained_entry = FALSE;
    wmt_status_t status;

    status = invoke_guarded( ^wmt_status_t{
        [owner retain];
        return WMT_STATUS_SUCCESS;
    } );
    if (status) return;
    retained_owner = TRUE;

    pthread_mutex_lock( &listener_mutex );
    entry = listener_lookup_locked( owner->token );
    if (entry && !entry->closing)
    {
        owner->next_pending = entry->pending;
        entry->pending = owner;
        retained_owner = FALSE;
        if (entry->runloop && entry->source)
        {
            entry->refs++;
            retained_entry = TRUE;
            runloop = entry->runloop;
            source = entry->source;
        }
    }
    pthread_mutex_unlock( &listener_mutex );

    if (retained_owner)
        (void)invoke_guarded( ^wmt_status_t{
            [owner release];
            return WMT_STATUS_SUCCESS;
        } );
    if (source)
        (void)invoke_guarded( ^wmt_status_t{
            CFRunLoopSourceSignal( source );
            CFRunLoopWakeUp( runloop );
            return WMT_STATUS_SUCCESS;
        } );
    if (retained_entry) listener_put( owner->token );
}

static wmt_status_t listener_release_pending( WMTListenerCallbackOwner *pending )
{
    wmt_status_t result = WMT_STATUS_SUCCESS;

    while (pending)
    {
        WMTListenerCallbackOwner *next = pending->next_pending;
        WMTListenerCallbackOwner *current = pending;

        current->next_pending = nil;
        listener_remember_status( &result, invoke_guarded( ^wmt_status_t{
            [current release];
            return WMT_STATUS_SUCCESS;
        } ) );
        pending = next;
    }
    return result;
}

static void listener_source_perform( void *opaque )
{
    uint64_t token = (uint64_t)(uintptr_t)opaque;
    WMTListenerCallbackOwner *pending;
    struct wmt_listener_entry *entry;
    BOOL closing = TRUE;

    pthread_mutex_lock( &listener_mutex );
    if ((entry = listener_lookup_locked( token )))
    {
        closing = entry->closing;
        pending = entry->pending;
        entry->pending = nil;
    }
    else pending = nil;
    pthread_mutex_unlock( &listener_mutex );

    while (pending)
    {
        WMTListenerCallbackOwner *next = pending->next_pending;

        pending->next_pending = nil;
        if (!closing && listener_is_live( pending->token ))
            NtSetEvent( (void *)(uintptr_t)pending->event_handle, NULL );
        (void)invoke_guarded( ^wmt_status_t{
            [pending release];
            return WMT_STATUS_SUCCESS;
        } );
        pending = next;
    }
    if (closing)
        (void)invoke_guarded( ^wmt_status_t{
            CFRunLoopRef current = CFRunLoopGetCurrent();

            if (current) CFRunLoopStop( current );
            return WMT_STATUS_SUCCESS;
        } );
}

@implementation WMTListenerCallbackOwner
@end

#ifdef WMT_NATIVE_TEST
extern dispatch_data_t wmt_test_dispatch_data_create( const void *bytes, size_t length );
#endif

static pthread_once_t companion_residency_once = PTHREAD_ONCE_INIT;
static void *companion_residency_ref;
static wmt_status_t companion_residency_status = WMT_STATUS_INVALID_DEVICE_STATE;

static void pin_companion_image_resident_once(void)
{
#if defined(__APPLE__) && defined(RTLD_NOLOAD)
    Dl_info info;

    if (!dladdr( (const void *)wmt_pin_companion_image_resident, &info ) || !info.dli_fname)
        return;
    companion_residency_ref = dlopen( info.dli_fname, RTLD_NOW | RTLD_NOLOAD );
    if (!companion_residency_ref) return;
    /* The last release callback can run on arbitrary framework code.  Keep
     * this independent reference for process lifetime; dropping it from the
     * callback would be a self-unload before its invoke code returns. */
    companion_residency_status = WMT_STATUS_SUCCESS;
#endif
}

wmt_status_t wmt_pin_companion_image_resident(void)
{
    if (pthread_once( &companion_residency_once, pin_companion_image_resident_once ))
        return WMT_STATUS_INVALID_DEVICE_STATE;
    return companion_residency_status;
}

static BOOL texture_format_layout( MTLPixelFormat format, NSUInteger *block_width,
                                   NSUInteger *block_height, NSUInteger *bytes_per_block )
{
#define WMT_LAYOUT(w, h, b) do { *block_width = (w); *block_height = (h); \
                                 *bytes_per_block = (b); return YES; } while (0)
    switch (format)
    {
    case MTLPixelFormatA8Unorm:
    case MTLPixelFormatR8Unorm:
    case MTLPixelFormatR8Unorm_sRGB:
    case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR8Uint:
    case MTLPixelFormatR8Sint:
    case MTLPixelFormatStencil8:
        WMT_LAYOUT( 1, 1, 1 );
    case MTLPixelFormatR16Unorm:
    case MTLPixelFormatR16Snorm:
    case MTLPixelFormatR16Uint:
    case MTLPixelFormatR16Sint:
    case MTLPixelFormatR16Float:
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Unorm_sRGB:
    case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRG8Uint:
    case MTLPixelFormatRG8Sint:
    case MTLPixelFormatB5G6R5Unorm:
    case MTLPixelFormatA1BGR5Unorm:
    case MTLPixelFormatABGR4Unorm:
    case MTLPixelFormatBGR5A1Unorm:
    case MTLPixelFormatDepth16Unorm:
        WMT_LAYOUT( 1, 1, 2 );
    case MTLPixelFormatR32Uint:
    case MTLPixelFormatR32Sint:
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRG16Snorm:
    case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint:
    case MTLPixelFormatRG16Float:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatRGB10A2Uint:
    case MTLPixelFormatRG11B10Float:
    case MTLPixelFormatRGB9E5Float:
    case MTLPixelFormatBGR10A2Unorm:
    case MTLPixelFormatBGR10_XR:
    case MTLPixelFormatBGR10_XR_sRGB:
    case MTLPixelFormatDepth32Float:
    case MTLPixelFormatDepth24Unorm_Stencil8:
    case MTLPixelFormatX24_Stencil8:
        WMT_LAYOUT( 1, 1, 4 );
    case MTLPixelFormatRG32Uint:
    case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatBGRA10_XR:
    case MTLPixelFormatBGRA10_XR_sRGB:
    case MTLPixelFormatDepth32Float_Stencil8:
    case MTLPixelFormatX32_Stencil8:
        WMT_LAYOUT( 1, 1, 8 );
    case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGBA32Sint:
    case MTLPixelFormatRGBA32Float:
        WMT_LAYOUT( 1, 1, 16 );
    case MTLPixelFormatGBGR422:
    case MTLPixelFormatBGRG422:
        WMT_LAYOUT( 2, 1, 4 );
    case MTLPixelFormatBC1_RGBA:
    case MTLPixelFormatBC1_RGBA_sRGB:
    case MTLPixelFormatBC4_RUnorm:
    case MTLPixelFormatBC4_RSnorm:
        WMT_LAYOUT( 4, 4, 8 );
    case MTLPixelFormatBC2_RGBA:
    case MTLPixelFormatBC2_RGBA_sRGB:
    case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatBC3_RGBA_sRGB:
    case MTLPixelFormatBC5_RGUnorm:
    case MTLPixelFormatBC5_RGSnorm:
    case MTLPixelFormatBC6H_RGBFloat:
    case MTLPixelFormatBC6H_RGBUfloat:
    case MTLPixelFormatBC7_RGBAUnorm:
    case MTLPixelFormatBC7_RGBAUnorm_sRGB:
        WMT_LAYOUT( 4, 4, 16 );
    case MTLPixelFormatEAC_R11Unorm:
    case MTLPixelFormatEAC_R11Snorm:
    case MTLPixelFormatETC2_RGB8:
    case MTLPixelFormatETC2_RGB8_sRGB:
    case MTLPixelFormatETC2_RGB8A1:
    case MTLPixelFormatETC2_RGB8A1_sRGB:
        WMT_LAYOUT( 4, 4, 8 );
    case MTLPixelFormatEAC_RG11Unorm:
    case MTLPixelFormatEAC_RG11Snorm:
    case MTLPixelFormatEAC_RGBA8:
    case MTLPixelFormatEAC_RGBA8_sRGB:
        WMT_LAYOUT( 4, 4, 16 );
    case MTLPixelFormatASTC_4x4_sRGB:
    case MTLPixelFormatASTC_4x4_LDR:
    case MTLPixelFormatASTC_4x4_HDR:
        WMT_LAYOUT( 4, 4, 16 );
    case MTLPixelFormatASTC_5x4_sRGB:
    case MTLPixelFormatASTC_5x4_LDR:
    case MTLPixelFormatASTC_5x4_HDR:
        WMT_LAYOUT( 5, 4, 16 );
    case MTLPixelFormatASTC_5x5_sRGB:
    case MTLPixelFormatASTC_5x5_LDR:
    case MTLPixelFormatASTC_5x5_HDR:
        WMT_LAYOUT( 5, 5, 16 );
    case MTLPixelFormatASTC_6x5_sRGB:
    case MTLPixelFormatASTC_6x5_LDR:
    case MTLPixelFormatASTC_6x5_HDR:
        WMT_LAYOUT( 6, 5, 16 );
    case MTLPixelFormatASTC_8x5_sRGB:
    case MTLPixelFormatASTC_8x5_LDR:
    case MTLPixelFormatASTC_8x5_HDR:
        WMT_LAYOUT( 8, 5, 16 );
    case MTLPixelFormatASTC_10x5_sRGB:
    case MTLPixelFormatASTC_10x5_LDR:
    case MTLPixelFormatASTC_10x5_HDR:
        WMT_LAYOUT( 10, 5, 16 );
    case MTLPixelFormatASTC_6x6_sRGB:
    case MTLPixelFormatASTC_6x6_LDR:
    case MTLPixelFormatASTC_6x6_HDR:
        WMT_LAYOUT( 6, 6, 16 );
    case MTLPixelFormatASTC_8x6_sRGB:
    case MTLPixelFormatASTC_8x6_LDR:
    case MTLPixelFormatASTC_8x6_HDR:
        WMT_LAYOUT( 8, 6, 16 );
    case MTLPixelFormatASTC_10x6_sRGB:
    case MTLPixelFormatASTC_10x6_LDR:
    case MTLPixelFormatASTC_10x6_HDR:
        WMT_LAYOUT( 10, 6, 16 );
    case MTLPixelFormatASTC_8x8_sRGB:
    case MTLPixelFormatASTC_8x8_LDR:
    case MTLPixelFormatASTC_8x8_HDR:
        WMT_LAYOUT( 8, 8, 16 );
    case MTLPixelFormatASTC_10x8_sRGB:
    case MTLPixelFormatASTC_10x8_LDR:
    case MTLPixelFormatASTC_10x8_HDR:
        WMT_LAYOUT( 10, 8, 16 );
    case MTLPixelFormatASTC_10x10_sRGB:
    case MTLPixelFormatASTC_10x10_LDR:
    case MTLPixelFormatASTC_10x10_HDR:
        WMT_LAYOUT( 10, 10, 16 );
    case MTLPixelFormatASTC_12x10_sRGB:
    case MTLPixelFormatASTC_12x10_LDR:
    case MTLPixelFormatASTC_12x10_HDR:
        WMT_LAYOUT( 12, 10, 16 );
    case MTLPixelFormatASTC_12x12_sRGB:
    case MTLPixelFormatASTC_12x12_LDR:
    case MTLPixelFormatASTC_12x12_HDR:
        WMT_LAYOUT( 12, 12, 16 );
    default:
        return NO;
    }
#undef WMT_LAYOUT
}

static wmt_status_t metal_texture_snapshot_rows_unsafe(
    uint64_t texture, uint64_t origin_x, uint64_t origin_y, uint64_t origin_z,
    uint64_t width, uint64_t height, uint64_t depth, uint64_t level, uint64_t slice,
    uint64_t bytes_per_row, uint64_t bytes_per_image, uint64_t *ret_rows,
    uint64_t *ret_row_bytes )
{
    id<MTLTexture> object = (id<MTLTexture>)(uintptr_t)texture;
    NSUInteger array_length, block_width, block_height, bytes_per_block;
    NSUInteger level_width, level_height, level_depth;
    MTLTextureType type;

    if (ret_rows) *ret_rows = 0;
    if (ret_row_bytes) *ret_row_bytes = 0;
    if (!object || !ret_rows || !ret_row_bytes || !bytes_per_row ||
        !width || !height || !depth || level >= 64 ||
        level >= [object mipmapLevelCount])
        return WMT_STATUS_INVALID_PARAMETER;
    if ([object sampleCount] != 1 || [object isFramebufferOnly] ||
        [object storageMode] == MTLStorageModePrivate ||
        [object storageMode] == MTLStorageModeMemoryless)
        return WMT_STATUS_INVALID_PARAMETER;
    level_width = MAX( (NSUInteger)1, [object width] >> level );
    level_height = MAX( (NSUInteger)1, [object height] >> level );
    level_depth = MAX( (NSUInteger)1, [object depth] >> level );
    if (origin_x > level_width || width > level_width - origin_x ||
        origin_y > level_height || height > level_height - origin_y ||
        origin_z > level_depth || depth > level_depth - origin_z)
        return WMT_STATUS_INVALID_PARAMETER;
    type = [object textureType];
    array_length = [object arrayLength];
    switch (type)
    {
    case MTLTextureType1DArray:
    case MTLTextureType2DArray:
        if (slice >= array_length) return WMT_STATUS_INVALID_PARAMETER;
        break;
    case MTLTextureType2DMultisample:
    case MTLTextureType2DMultisampleArray:
        return WMT_STATUS_INVALID_PARAMETER;
    case MTLTextureTypeCube:
        if (slice >= 6) return WMT_STATUS_INVALID_PARAMETER;
        break;
    case MTLTextureTypeCubeArray:
        if (array_length > UINT64_MAX / 6 || slice >= array_length * 6)
            return WMT_STATUS_INVALID_PARAMETER;
        break;
    default:
        if (slice) return WMT_STATUS_INVALID_PARAMETER;
        break;
    }
    if (!texture_format_layout( [object pixelFormat], &block_width, &block_height,
                                &bytes_per_block ))
        return WMT_STATUS_INVALID_PARAMETER;
    if ((type != MTLTextureType3D && bytes_per_image) || bytes_per_row % bytes_per_block ||
        (bytes_per_image && bytes_per_image % bytes_per_block) ||
        (block_width == 1 && block_height == 1 &&
         bytes_per_row >= 32767ull * bytes_per_block))
        return WMT_STATUS_INVALID_PARAMETER;
    if (origin_x % block_width || origin_y % block_height ||
        (width % block_width && origin_x + width != level_width) ||
        (height % block_height && origin_y + height != level_height))
        return WMT_STATUS_INVALID_PARAMETER;
    *ret_rows = height / block_height + !!(height % block_height);
    if (width / block_width + !!(width % block_width) > UINT64_MAX / bytes_per_block)
        return WMT_STATUS_INVALID_PARAMETER;
    *ret_row_bytes = (width / block_width + !!(width % block_width)) * bytes_per_block;
    return WMT_STATUS_SUCCESS;
}

wmt_status_t wmt_metal_texture_snapshot_rows( uint64_t texture, uint64_t origin_x,
                                              uint64_t origin_y, uint64_t origin_z,
                                              uint64_t width, uint64_t height,
                                              uint64_t depth, uint64_t level, uint64_t slice,
                                              uint64_t bytes_per_row, uint64_t bytes_per_image,
                                              uint64_t *ret_rows, uint64_t *ret_row_bytes )
{
    wmt_status_t status;

    status = invoke_guarded( ^wmt_status_t{
        return metal_texture_snapshot_rows_unsafe(
            texture, origin_x, origin_y, origin_z, width, height, depth, level, slice,
            bytes_per_row, bytes_per_image, ret_rows, ret_row_bytes );
    } );
    if (status && ret_rows) *ret_rows = 0;
    if (status && ret_row_bytes) *ret_row_bytes = 0;
    return status;
}

static wmt_status_t metal_buffer_length_unsafe( uint64_t buffer, uint64_t *ret_length )
{
    id<MTLBuffer> object = (id<MTLBuffer>)(uintptr_t)buffer;

    if (ret_length) *ret_length = 0;
    if (!object || !ret_length || ![object contents]) return WMT_STATUS_INVALID_PARAMETER;
    *ret_length = [object length];
    return WMT_STATUS_SUCCESS;
}

wmt_status_t wmt_metal_buffer_length( uint64_t buffer, uint64_t *ret_length )
{
    wmt_status_t status;

    status = invoke_guarded( ^wmt_status_t{
        return metal_buffer_length_unsafe( buffer, ret_length );
    } );
    if (status && ret_length) *ret_length = 0;
    return status;
}

wmt_status_t wmt_dispatch_data_from_snapshot( void *bytes, uint64_t length,
                                              wmt_snapshot_release_func release_snapshot,
                                              uint64_t *ret_data )
{
    __block dispatch_data_t data = NULL;
    wmt_status_t status;

    if (ret_data) *ret_data = 0;
    if (!bytes || !length || length > SIZE_MAX || !release_snapshot || !ret_data)
    {
        if (bytes && release_snapshot) release_snapshot( bytes, length );
        return WMT_STATUS_INVALID_PARAMETER;
    }
    /* DEFAULT synchronously copies the bytes into dispatch-owned storage, so
     * no destructor callback can escape this companion image. */
    status = invoke_guarded( ^wmt_status_t{
#ifdef WMT_NATIVE_TEST
        data = wmt_test_dispatch_data_create( bytes, (size_t)length );
#else
        data = dispatch_data_create( bytes, (size_t)length, NULL,
                                     DISPATCH_DATA_DESTRUCTOR_DEFAULT );
#endif
        return data ? WMT_STATUS_SUCCESS : WMT_STATUS_NO_MEMORY;
    } );
    release_snapshot( bytes, length );
    if (status) return status;
    *ret_data = (wmt_uint64_t)(uintptr_t)data;
    return WMT_STATUS_SUCCESS;
}

static wmt_status_t release_owned_lease( wmt_uint64_t lease,
                                         wmt_alias_release_func release_alias,
                                         wmt_status_t status )
{
    wmt_status_t release_status = release_alias( lease );

    return release_status ? release_status : status;
}

static wmt_status_t release_metal_buffer_safely( id<MTLBuffer> buffer )
{
    return invoke_guarded( ^wmt_status_t{
        [buffer release];
        return WMT_STATUS_SUCCESS;
    } );
}

wmt_status_t wmt_metal_buffer_from_alias( wmt_uint64_t device, void *address,
                                          wmt_uint64_t logical_length, wmt_uint64_t mapped_length,
                                          wmt_uint64_t options, wmt_uint64_t lease,
                                          wmt_alias_release_func release_alias,
                                          wmt_uint64_t *ret_buffer, wmt_uint64_t *ret_gpu_address )
{
    __block _Atomic wmt_uint64_t owned_lease = lease;
    __block _Atomic wmt_status_t owned_release_status = WMT_STATUS_SUCCESS;
    __block _Atomic unsigned int alias_state = WMT_ALIAS_CREATING;
    __block id<MTLBuffer> buffer;
    uint64_t rounded_length;
    __block wmt_uint64_t gpu_address = 0;
    wmt_status_t call_status;
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
    if (wmt_pin_companion_image_resident())
        return release_owned_lease( lease, release_alias, WMT_STATUS_INVALID_DEVICE_STATE );

    buffer = nil;
    call_status = invoke_guarded( ^wmt_status_t{
        buffer = [(id<MTLDevice>)(uintptr_t)device
            newBufferWithBytesNoCopy:address
                               length:(NSUInteger)logical_length
                              options:(MTLResourceOptions)options
                          deallocator:^(void *bytes, NSUInteger length)
                          {
                              wmt_uint64_t current_lease;
                              unsigned int expected, state;

                              (void)bytes;
                              (void)length;
                              for (;;)
                              {
                                  state = atomic_load_explicit( &alias_state,
                                                                memory_order_acquire );
                                  if (state == WMT_ALIAS_CREATING)
                                  {
                                      expected = state;
                                      if (atomic_compare_exchange_weak_explicit(
                                              &alias_state, &expected,
                                              WMT_ALIAS_CALLBACK_SEEN,
                                              memory_order_acq_rel,
                                              memory_order_acquire ))
                                          return;
                                      continue;
                                  }
                                  if (state != WMT_ALIAS_PUBLISHED &&
                                      state != WMT_ALIAS_AMBIGUOUS)
                                      return;
                                  expected = state;
                                  if (atomic_compare_exchange_weak_explicit(
                                          &alias_state, &expected, WMT_ALIAS_RELEASING,
                                          memory_order_acq_rel, memory_order_acquire ))
                                      break;
                              }
                              current_lease = atomic_exchange_explicit( &owned_lease, 0,
                                                                        memory_order_acq_rel );
                              /* This void Metal callback cannot propagate or safely retry a
                               * release; the codec owns any failure recovery contract. */
                              if (current_lease)
                              {
                                  wmt_status_t status = release_alias( current_lease );

                                  if (status)
                                      atomic_store_explicit( &owned_release_status, status,
                                                             memory_order_release );
                              }
                              atomic_store_explicit( &alias_state, WMT_ALIAS_SETTLED,
                                                     memory_order_release );
                          }];
        return WMT_STATUS_SUCCESS;
    } );
    if (call_status)
    {
        unsigned int expected = WMT_ALIAS_CREATING;

        /* The Objective-C send may fault after Metal copied the deallocator
         * block or retained a partially-created buffer.  Ownership is then
         * ambiguous: the block remains the only safe lease owner.  Releasing
         * here could leave a live native buffer pointing at recycled backing.
         * The backing pool bounds this fail-safe quarantine. */
        if (!atomic_compare_exchange_strong_explicit( &alias_state, &expected,
                                                       WMT_ALIAS_AMBIGUOUS,
                                                       memory_order_acq_rel,
                                                       memory_order_acquire ) &&
            expected == WMT_ALIAS_CALLBACK_SEEN)
            atomic_compare_exchange_strong_explicit( &alias_state, &expected,
                                                      WMT_ALIAS_QUARANTINED,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire );
        return call_status;
    }
    if (!buffer)
    {
        wmt_uint64_t current_lease;
        wmt_status_t release_status;
        unsigned int expected, state;

        for (;;)
        {
            state = atomic_load_explicit( &alias_state, memory_order_acquire );
            if (state != WMT_ALIAS_CREATING && state != WMT_ALIAS_CALLBACK_SEEN)
                return WMT_STATUS_INVALID_DEVICE_STATE;
            expected = state;
            if (atomic_compare_exchange_weak_explicit( &alias_state, &expected,
                                                        WMT_ALIAS_RELEASING,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire ))
                break;
        }
        current_lease = atomic_exchange_explicit( &owned_lease, 0, memory_order_acq_rel );
        if (current_lease)
        {
            release_status = release_alias( current_lease );
            if (release_status) atomic_store_explicit( &owned_release_status, release_status,
                                                        memory_order_release );
        }
        atomic_store_explicit( &alias_state, WMT_ALIAS_SETTLED, memory_order_release );
        release_status = atomic_load_explicit( &owned_release_status, memory_order_acquire );
        return release_status ? release_status : WMT_STATUS_NO_MEMORY;
    }
    {
        unsigned int expected = WMT_ALIAS_CREATING;

        if (!atomic_compare_exchange_strong_explicit( &alias_state, &expected,
                                                       WMT_ALIAS_PUBLISHED,
                                                       memory_order_acq_rel,
                                                       memory_order_acquire ))
        {
            wmt_status_t release_status;

            if (expected == WMT_ALIAS_CALLBACK_SEEN)
            {
                atomic_compare_exchange_strong_explicit( &alias_state, &expected,
                                                          WMT_ALIAS_QUARANTINED,
                                                          memory_order_acq_rel,
                                                          memory_order_acquire );
            }
            release_status = release_metal_buffer_safely( buffer );
            if (!release_status) release_status = WMT_STATUS_INVALID_DEVICE_STATE;
            return release_status;
        }
    }
    if (atomic_load_explicit( &alias_state, memory_order_acquire ) != WMT_ALIAS_PUBLISHED)
    {
        wmt_status_t release_status;

        /* A callback after publication contradicts the returned ownership.
         * Consume our object reference, but never reclaim backing manually. */
        release_metal_buffer_safely( buffer );
        release_status = atomic_load_explicit( &owned_release_status, memory_order_acquire );
        return release_status ? release_status : WMT_STATUS_INVALID_DEVICE_STATE;
    }

    call_status = invoke_guarded( ^wmt_status_t{
        gpu_address = (wmt_uint64_t)[buffer gpuAddress];
        return WMT_STATUS_SUCCESS;
    } );
    if (call_status)
    {
        wmt_status_t release_status;

        release_status = release_metal_buffer_safely( buffer );
        /* A successful or faulting release may leave another native retain.
         * Only the Metal deallocator may consume the lease after a nonnil
         * buffer has been returned from creation. */
        return release_status ? release_status : call_status;
    }

    *ret_buffer = (wmt_uint64_t)(uintptr_t)buffer;
    *ret_gpu_address = gpu_address;
    return WMT_STATUS_SUCCESS;
}

static wmt_status_t listener_retire( uint64_t token )
{
    struct wmt_listener_cleanup cleanup = {0};
    WMTListenerCallbackOwner *pending = nil;
    struct wmt_listener_entry *entry;
    CFRunLoopRef runloop = NULL;
    BOOL retained_entry = FALSE;
    wmt_status_t result = WMT_STATUS_SUCCESS;
    wmt_status_t stop_status = WMT_STATUS_SUCCESS;

    pthread_mutex_lock( &listener_mutex );
    if (!(entry = listener_lookup_locked( token )))
    {
        pthread_mutex_unlock( &listener_mutex );
        return WMT_STATUS_INVALID_HANDLE;
    }
    if (entry->closing)
    {
        pthread_mutex_unlock( &listener_mutex );
        return WMT_STATUS_INVALID_HANDLE;
    }
    entry->closing = TRUE;
    entry->state = WMT_LISTENER_STOPPING;
    pending = entry->pending;
    entry->pending = nil;
    if (entry->runloop)
    {
        entry->refs++;
        retained_entry = TRUE;
        runloop = entry->runloop;
    }
    if (entry->registry_owned)
    {
        entry->registry_owned = FALSE;
        entry->refs--;
    }
    listener_collect_locked( entry, &cleanup );
    pthread_mutex_unlock( &listener_mutex );

    listener_remember_status( &result, listener_release_pending( pending ) );
    if (runloop)
    {
        stop_status = invoke_guarded( ^wmt_status_t{
            CFRunLoopStop( runloop );
            CFRunLoopWakeUp( runloop );
            return WMT_STATUS_SUCCESS;
        } );
    }
    listener_remember_status( &result, stop_status );
    if (retained_entry) listener_put( token );
    listener_remember_status( &result, listener_release_cleanup( &cleanup ) );
    return result;
}

wmt_status_t wmt_shared_event_listener_create( uint64_t *ret_token )
{
    __block MTLSharedEventListener *listener = nil;
    struct wmt_listener_entry *entry = NULL;
    uint64_t generation, token;
    unsigned int i;
    wmt_status_t status;

    if (ret_token) *ret_token = 0;
    if (!ret_token) return WMT_STATUS_INVALID_PARAMETER;
    if (wmt_pin_companion_image_resident()) return WMT_STATUS_INVALID_DEVICE_STATE;

    pthread_mutex_lock( &listener_mutex );
    if (listeners_quiescing)
    {
        pthread_mutex_unlock( &listener_mutex );
        return WMT_STATUS_INVALID_DEVICE_STATE;
    }
    pthread_mutex_unlock( &listener_mutex );

    status = invoke_guarded( ^wmt_status_t{
        listener = [[MTLSharedEventListener alloc] init];
        return listener ? WMT_STATUS_SUCCESS : WMT_STATUS_NO_MEMORY;
    } );
    if (status) return status;

    pthread_mutex_lock( &listener_mutex );
    if (!listeners_quiescing)
    {
        for (i = 0; i < WMT_LISTENER_MAX; i++)
            if (listener_entries[i].state == WMT_LISTENER_FREE)
            {
                entry = &listener_entries[i];
                break;
            }
    }
    if (!entry || next_listener_generation > (UINT64_MAX >> 8))
    {
        struct wmt_listener_cleanup cleanup = {listener, NULL, NULL};
        wmt_status_t cleanup_status;
        wmt_status_t result;

        pthread_mutex_unlock( &listener_mutex );
        result = entry ? WMT_STATUS_TOO_MANY_CONTEXT_IDS :
                 (listeners_quiescing ? WMT_STATUS_INVALID_DEVICE_STATE :
                                        WMT_STATUS_TOO_MANY_CONTEXT_IDS);
        cleanup_status = listener_release_cleanup( &cleanup );
        return cleanup_status ? cleanup_status : result;
    }
    generation = next_listener_generation++;
    token = (generation << 8) | (i + 1);
    entry->token = token;
    entry->state = WMT_LISTENER_CREATED;
    entry->refs = 1;
    entry->registry_owned = TRUE;
    entry->listener = listener;
    pthread_mutex_unlock( &listener_mutex );

    *ret_token = token;
    return WMT_STATUS_SUCCESS;
}

wmt_status_t wmt_shared_event_listener_start( uint64_t token )
{
    struct wmt_listener_cleanup cleanup = {0};
    struct wmt_listener_cleanup runloop_cleanup = {0};
    WMTListenerCallbackOwner *pending = nil;
    struct wmt_listener_entry *entry;
    CFRunLoopSourceContext source_context = {0};
    __block CFRunLoopSourceRef source = NULL;
    __block CFRunLoopRef runloop = NULL;
    BOOL stop_requested;
    wmt_status_t cleanup_status;
    wmt_status_t remove_status;
    wmt_status_t run_status;

    pthread_mutex_lock( &listener_mutex );
    if (!(entry = listener_lookup_locked( token )) || entry->closing ||
        entry->state != WMT_LISTENER_CREATED)
    {
        pthread_mutex_unlock( &listener_mutex );
        return WMT_STATUS_INVALID_HANDLE;
    }
    entry->state = WMT_LISTENER_STARTING;
    entry->refs++;
    pthread_mutex_unlock( &listener_mutex );

    source_context.info = (void *)(uintptr_t)token;
    source_context.perform = listener_source_perform;
    run_status = invoke_guarded( ^wmt_status_t{
        CFRunLoopSourceContext context = source_context;
        CFRunLoopRef current = CFRunLoopGetCurrent();

        if (!current) return WMT_STATUS_INVALID_DEVICE_STATE;
        runloop = (CFRunLoopRef)CFRetain( current );
        source = CFRunLoopSourceCreate( NULL, 0, &context );
        if (!source) return WMT_STATUS_NO_MEMORY;
        CFRunLoopAddSource( runloop, source, kCFRunLoopCommonModes );
        return WMT_STATUS_SUCCESS;
    } );
    if (run_status)
    {
        if (source && runloop)
            (void)invoke_guarded( ^wmt_status_t{
                CFRunLoopRemoveSource( runloop, source, kCFRunLoopCommonModes );
                return WMT_STATUS_SUCCESS;
            } );
        runloop_cleanup.source = source;
        runloop_cleanup.runloop = runloop;
        cleanup_status = listener_release_cleanup( &runloop_cleanup );
        pthread_mutex_lock( &listener_mutex );
        if ((entry = listener_lookup_locked( token )))
        {
            if (entry->state == WMT_LISTENER_STARTING && !entry->closing)
                entry->state = WMT_LISTENER_CREATED;
            entry->refs--;
            listener_collect_locked( entry, &cleanup );
        }
        pthread_mutex_unlock( &listener_mutex );
        listener_remember_status( &run_status, cleanup_status );
        listener_remember_status( &run_status, listener_release_cleanup( &cleanup ) );
        return run_status;
    }

    pthread_mutex_lock( &listener_mutex );
    entry = listener_lookup_locked( token );
    if (!entry)
    {
        pthread_mutex_unlock( &listener_mutex );
        invoke_guarded( ^wmt_status_t{
            CFRunLoopRemoveSource( runloop, source, kCFRunLoopCommonModes );
            return WMT_STATUS_SUCCESS;
        } );
        runloop_cleanup.source = source;
        runloop_cleanup.runloop = runloop;
        (void)listener_release_cleanup( &runloop_cleanup );
        return WMT_STATUS_INVALID_HANDLE;
    }
    entry->runloop = runloop;
    entry->source = source;
    pending = entry->pending;
    entry->pending = NULL;
    stop_requested = entry->closing;
    entry->state = stop_requested ? WMT_LISTENER_STOPPING : WMT_LISTENER_RUNNING;
    pthread_mutex_unlock( &listener_mutex );

    if (pending)
    {
        pthread_mutex_lock( &listener_mutex );
        if ((entry = listener_lookup_locked( token )) && !entry->closing)
        {
            WMTListenerCallbackOwner *tail = pending;

            while (tail->next_pending) tail = tail->next_pending;
            tail->next_pending = entry->pending;
            entry->pending = pending;
            pending = nil;
        }
        pthread_mutex_unlock( &listener_mutex );
        (void)listener_release_pending( pending );
        (void)invoke_guarded( ^wmt_status_t{
            CFRunLoopSourceSignal( source );
            return WMT_STATUS_SUCCESS;
        } );
    }
    if (stop_requested)
        (void)invoke_guarded( ^wmt_status_t{
            CFRunLoopSourceSignal( source );
            CFRunLoopWakeUp( runloop );
            return WMT_STATUS_SUCCESS;
        } );
    run_status = invoke_guarded( ^wmt_status_t{
        CFRunLoopRun();
        return WMT_STATUS_SUCCESS;
    } );
    remove_status = invoke_guarded( ^wmt_status_t{
        CFRunLoopRemoveSource( runloop, source, kCFRunLoopCommonModes );
        return WMT_STATUS_SUCCESS;
    } );
    listener_remember_status( &run_status, remove_status );

    pending = nil;
    pthread_mutex_lock( &listener_mutex );
    if ((entry = listener_lookup_locked( token )))
    {
        if (entry->runloop == runloop) entry->runloop = NULL;
        if (entry->source == source) entry->source = NULL;
        pending = entry->pending;
        entry->pending = NULL;
        entry->closing = TRUE;
        entry->state = WMT_LISTENER_RETIRED;
        if (entry->registry_owned)
        {
            entry->registry_owned = FALSE;
            entry->refs--;
        }
        entry->refs--;
        listener_collect_locked( entry, &cleanup );
    }
    pthread_mutex_unlock( &listener_mutex );

    listener_remember_status( &run_status, listener_release_pending( pending ) );
    runloop_cleanup.source = source;
    runloop_cleanup.runloop = runloop;
    listener_remember_status( &run_status, listener_release_cleanup( &runloop_cleanup ) );
    listener_remember_status( &run_status, listener_release_cleanup( &cleanup ) );
    return run_status;
}

wmt_status_t wmt_shared_event_listener_destroy( uint64_t token )
{
    return listener_retire( token );
}

wmt_status_t wmt_shared_event_notify_win32( uint64_t shared_event, uint64_t event_handle,
                                            uint64_t listener_token, uint64_t value )
{
    __block WMTListenerCallbackOwner *owner = nil;
    WMTListenerCallbackOwner *strong_owner;
    id<MTLSharedEvent> event = (id<MTLSharedEvent>)(uintptr_t)shared_event;
    MTLSharedEventListener *listener;
    struct wmt_listener_entry *entry;
    unsigned int expected;
    wmt_status_t status;

    if (!event || !event_handle) return WMT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock( &listener_mutex );
    entry = listener_lookup_locked( listener_token );
    if (!entry || entry->closing)
    {
        pthread_mutex_unlock( &listener_mutex );
        return WMT_STATUS_INVALID_HANDLE;
    }
    entry->refs++;
    listener = entry->listener;
    pthread_mutex_unlock( &listener_mutex );

    status = invoke_guarded( ^wmt_status_t{
        [listener retain];
        return WMT_STATUS_SUCCESS;
    } );
    listener_put( listener_token );
    if (status) return status;

    status = invoke_guarded( ^wmt_status_t{
        owner = [[WMTListenerCallbackOwner alloc] init];
        return owner ? WMT_STATUS_SUCCESS : WMT_STATUS_NO_MEMORY;
    } );
    if (status)
    {
        (void)invoke_guarded( ^wmt_status_t{
            [listener release];
            return WMT_STATUS_SUCCESS;
        } );
        return status;
    }
    owner->token = listener_token;
    owner->event_handle = event_handle;
    atomic_init( &owner->callback_state, WMT_LISTENER_CALLBACK_REGISTERING );
    owner->next_pending = nil;
    strong_owner = owner;
    status = invoke_guarded( ^wmt_status_t{
        [event notifyListener:listener atValue:value block:^(id<MTLSharedEvent> unused,
                                                             uint64_t unused_value)
        {
            unsigned int previous;

            (void)unused;
            (void)unused_value;
            previous = atomic_exchange_explicit( &strong_owner->callback_state,
                                                  WMT_LISTENER_CALLBACK_FIRED,
                                                  memory_order_acq_rel );
            if (previous == WMT_LISTENER_CALLBACK_ARMED)
                listener_deliver_owner( strong_owner );
        }];
        return WMT_STATUS_SUCCESS;
    } );
    if (status)
        atomic_store_explicit( &strong_owner->callback_state,
                               WMT_LISTENER_CALLBACK_CANCELLED, memory_order_release );
    else
    {
        expected = WMT_LISTENER_CALLBACK_REGISTERING;
        if (!atomic_compare_exchange_strong_explicit(
                &strong_owner->callback_state, &expected,
                WMT_LISTENER_CALLBACK_ARMED, memory_order_acq_rel,
                memory_order_acquire ) && expected == WMT_LISTENER_CALLBACK_FIRED)
            listener_deliver_owner( strong_owner );
    }
    (void)invoke_guarded( ^wmt_status_t{
        [strong_owner release];
        return WMT_STATUS_SUCCESS;
    } );
    (void)invoke_guarded( ^wmt_status_t{
        [listener release];
        return WMT_STATUS_SUCCESS;
    } );
    return status;
}

wmt_status_t wmt_quiesce_shared_event_listeners(void)
{
    uint64_t tokens[WMT_LISTENER_MAX];
    unsigned int count = 0, i;
    wmt_status_t result = WMT_STATUS_SUCCESS;
    wmt_status_t status;

    pthread_mutex_lock( &listener_mutex );
    listeners_quiescing = TRUE;
    for (i = 0; i < WMT_LISTENER_MAX; i++)
        if (listener_entries[i].state != WMT_LISTENER_FREE &&
            !listener_entries[i].closing)
            tokens[count++] = listener_entries[i].token;
    pthread_mutex_unlock( &listener_mutex );

    for (i = 0; i < count; i++)
    {
        status = listener_retire( tokens[i] );
        if (status != WMT_STATUS_INVALID_HANDLE)
            listener_remember_status( &result, status );
    }
    return result;
}

void wmt_resume_shared_event_listeners(void)
{
    pthread_mutex_lock( &listener_mutex );
    listeners_quiescing = FALSE;
    pthread_mutex_unlock( &listener_mutex );
}

#ifdef WMT_NATIVE_TEST
unsigned int wmt_test_shared_event_listener_pending( uint64_t token )
{
    WMTListenerCallbackOwner *pending;
    struct wmt_listener_entry *entry;
    unsigned int count = 0;

    pthread_mutex_lock( &listener_mutex );
    if ((entry = listener_lookup_locked( token )))
        for (pending = entry->pending; pending; pending = pending->next_pending) count++;
    pthread_mutex_unlock( &listener_mutex );
    return count;
}
#endif
