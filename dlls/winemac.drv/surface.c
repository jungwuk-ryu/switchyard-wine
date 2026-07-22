/*
 * Mac driver window surface implementation
 *
 * Copyright 1993, 1994, 2011 Alexandre Julliard
 * Copyright 2006 Damjan Jovanovic
 * Copyright 2012, 2013 Ken Thomases for CodeWeavers, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "macdrv.h"
#include "winuser.h"

WINE_DEFAULT_DEBUG_CHANNEL(bitblt);

static inline int get_dib_stride(int width, int bpp)
{
    return ((width * bpp + 31) >> 3) & ~3;
}

static inline int get_dib_image_size(const BITMAPINFO *info)
{
    return get_dib_stride(info->bmiHeader.biWidth, info->bmiHeader.biBitCount)
        * abs(info->bmiHeader.biHeight);
}

/*
 * A window surface has exactly one presentation destination:
 *
 *  - a process-local Cocoa top-level updates that window's permanent base
 *    layer; or
 *  - every other HWND exports one CAContext-backed DIB layer to the nearest
 *    native root selected from the real Win32 root/owner tree.
 *
 * There is no executable/class detection, shared-memory root relay, surface
 * colour classification, or CPU-side flattening in this contract.
 */
struct macdrv_dib_endpoint
{
    LONG                       refs;
    HWND                       hwnd;
    DWORD                      source_thread;
    DWORD                      source_process;
    RECT                       rect;
    size_t                     image_size;
    pthread_mutex_t            mutex;
    macdrv_image_layer         image_layer;
    struct macdrv_dib_endpoint *replaced_endpoint;
    struct macdrv_dib_endpoint *next;
    void                       *bits;
};

struct macdrv_window_surface
{
    struct window_surface      header;
    macdrv_window              window;
    struct macdrv_dib_endpoint *dib_endpoint;
    struct window_surface      *replaced_surface;
    CGDataProviderRef          backing_provider;
    void                       *bits;
};

static pthread_mutex_t dib_endpoints_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macdrv_dib_endpoint *dib_endpoints;

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface);

static void dib_endpoint_add_ref(struct macdrv_dib_endpoint *endpoint)
{
    InterlockedIncrement(&endpoint->refs);
}

static void dib_endpoint_release(struct macdrv_dib_endpoint *endpoint)
{
    struct macdrv_dib_endpoint *replaced;

    if (InterlockedDecrement(&endpoint->refs)) return;

    replaced = endpoint->replaced_endpoint;
    macdrv_destroy_image_layer(endpoint->image_layer);
    pthread_mutex_destroy(&endpoint->mutex);
    free(endpoint->bits);
    free(endpoint);
    if (replaced) dib_endpoint_release(replaced);
}

static struct macdrv_dib_endpoint *create_dib_endpoint(HWND hwnd, DWORD source_thread,
                                                        DWORD source_process, const RECT *rect,
                                                        size_t image_size, DWORD background)
{
    struct macdrv_dib_endpoint *endpoint;

    if (!(endpoint = calloc(1, sizeof(*endpoint)))) return NULL;
    if (!(endpoint->bits = malloc(image_size)))
    {
        free(endpoint);
        return NULL;
    }

    endpoint->refs = 2; /* one registry reference and one returned surface reference */
    endpoint->hwnd = hwnd;
    endpoint->source_thread = source_thread;
    endpoint->source_process = source_process;
    endpoint->rect = *rect;
    endpoint->image_size = image_size;
    pthread_mutex_init(&endpoint->mutex, NULL);
    memset_pattern4(endpoint->bits, &background, image_size);

    if (!(endpoint->image_layer = macdrv_create_image_layer(hwnd, cgrect_from_rect(*rect))))
    {
        pthread_mutex_destroy(&endpoint->mutex);
        free(endpoint->bits);
        free(endpoint);
        return NULL;
    }
    return endpoint;
}

static void seed_dib_endpoint(struct macdrv_dib_endpoint *replacement,
                              struct macdrv_dib_endpoint *previous)
{
    RECT overlap;
    size_t new_stride, old_stride, row_bytes;
    int y;

    if (!intersect_rect(&overlap, &replacement->rect, &previous->rect)) return;

    new_stride = (replacement->rect.right - replacement->rect.left) * 4;
    old_stride = (previous->rect.right - previous->rect.left) * 4;
    row_bytes = (overlap.right - overlap.left) * 4;

    pthread_mutex_lock(&previous->mutex);
    for (y = overlap.top; y < overlap.bottom; y++)
    {
        BYTE *dst = (BYTE *)replacement->bits + (y - replacement->rect.top) * new_stride +
                    (overlap.left - replacement->rect.left) * 4;
        const BYTE *src = (const BYTE *)previous->bits + (y - previous->rect.top) * old_stride +
                          (overlap.left - previous->rect.left) * 4;

        memcpy(dst, src, row_bytes);
    }
    pthread_mutex_unlock(&previous->mutex);
}

static struct macdrv_dib_endpoint *acquire_dib_endpoint(HWND hwnd, const RECT *rect,
                                                         size_t image_size, DWORD background)
{
    struct macdrv_dib_endpoint **cursor, *endpoint, *previous = NULL, *retired = NULL;
    DWORD source_process = 0, source_thread;
    BOOL same_generation = FALSE;

    if (!(source_thread = NtUserGetWindowThread(hwnd, &source_process)) || !source_process)
        return NULL;

    pthread_mutex_lock(&dib_endpoints_mutex);
    for (cursor = &dib_endpoints; *cursor; cursor = &(*cursor)->next)
    {
        if ((*cursor)->hwnd != hwnd) continue;
        previous = *cursor;
        same_generation = previous->source_thread == source_thread &&
                          previous->source_process == source_process;
        if (same_generation && previous->image_size == image_size &&
            EqualRect(&previous->rect, rect))
        {
            dib_endpoint_add_ref(previous);
            pthread_mutex_unlock(&dib_endpoints_mutex);
            return previous;
        }
        break;
    }

    if (!(endpoint = create_dib_endpoint(hwnd, source_thread, source_process, rect,
                                         image_size, background)))
    {
        pthread_mutex_unlock(&dib_endpoints_mutex);
        return NULL;
    }
    if (same_generation)
    {
        seed_dib_endpoint(endpoint, previous);
        endpoint->replaced_endpoint = previous; /* transfer the registry reference */
    }
    else if (previous)
        retired = previous;

    if (previous)
    {
        endpoint->next = previous->next;
        *cursor = endpoint;
        previous->next = NULL;
    }
    else
    {
        endpoint->next = dib_endpoints;
        dib_endpoints = endpoint;
    }
    pthread_mutex_unlock(&dib_endpoints_mutex);
    if (retired) dib_endpoint_release(retired);

    TRACE("Switchyard created persistent remote-DIB endpoint source %p rect %s\n",
          hwnd, wine_dbgstr_rect(rect));
    return endpoint;
}

void macdrv_purge_dib_endpoints(HWND hwnd)
{
    struct macdrv_dib_endpoint **cursor, *endpoint = NULL;

    pthread_mutex_lock(&dib_endpoints_mutex);
    for (cursor = &dib_endpoints; *cursor; cursor = &(*cursor)->next)
    {
        if ((*cursor)->hwnd != hwnd) continue;
        endpoint = *cursor;
        *cursor = endpoint->next;
        endpoint->next = NULL;
        break;
    }
    pthread_mutex_unlock(&dib_endpoints_mutex);

    if (endpoint) dib_endpoint_release(endpoint);
}

static void seed_replacement_surface(struct window_surface *replacement,
                                     struct window_surface *previous)
{
    struct macdrv_window_surface *new_mac = get_mac_surface(replacement);
    struct macdrv_window_surface *old_mac = get_mac_surface(previous);
    RECT overlap;
    size_t new_stride, old_stride, row_bytes;
    int y;

    if (!new_mac || !old_mac || replacement->hwnd != previous->hwnd ||
        !intersect_rect(&overlap, &replacement->rect, &previous->rect))
        return;

    new_stride = (replacement->rect.right - replacement->rect.left) * 4;
    old_stride = (previous->rect.right - previous->rect.left) * 4;
    row_bytes = (overlap.right - overlap.left) * 4;

    pthread_mutex_lock(&previous->mutex);
    for (y = overlap.top; y < overlap.bottom; y++)
    {
        BYTE *dst = (BYTE *)new_mac->bits + (y - replacement->rect.top) * new_stride +
                    (overlap.left - replacement->rect.left) * 4;
        const BYTE *src = (const BYTE *)old_mac->bits + (y - previous->rect.top) * old_stride +
                          (overlap.left - previous->rect.left) * 4;

        memcpy(dst, src, row_bytes);
    }
    pthread_mutex_unlock(&previous->mutex);
}

static CGDataProviderRef mutable_data_provider_create(size_t size, void **bits)
{
    CGDataProviderRef provider;
    CFMutableDataRef data;

    if (!(data = CFDataCreateMutable(kCFAllocatorDefault, size))) return NULL;
    CFDataSetLength(data, size);

    if ((provider = CGDataProviderCreateWithCFData(data)))
        *bits = CFDataGetMutableBytePtr(data);
    CFRelease(data);
    return provider;
}

/* Core Animation may consume a CGImage after the GDI surface is unlocked.
 * Always copy the submitted pixels so a later producer write cannot mutate an
 * in-flight frame (caret blinking and transient popups exercise this heavily). */
static CGDataProviderRef snapshot_data_provider_create(const void *bits, size_t size)
{
    CGDataProviderRef provider = NULL;
    CFDataRef data;

    if (!(data = CFDataCreate(kCFAllocatorDefault, bits, size))) return NULL;
    provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    return provider;
}

static void merge_dirty_bits(void *dst_bits, const RECT *dirty,
                             const BITMAPINFO *color_info, const void *color_bits)
{
    RECT update;
    unsigned int height, stride, row, copy_width;
    const BYTE *src = color_bits;
    BYTE *dst = dst_bits;

    if (!color_bits || color_bits == dst_bits) return;
    if (color_info->bmiHeader.biBitCount != 32 || color_info->bmiHeader.biCompression != BI_RGB)
    {
        memcpy(dst_bits, color_bits, color_info->bmiHeader.biSizeImage);
        return;
    }

    height = abs(color_info->bmiHeader.biHeight);
    if (!height) return;
    stride = color_info->bmiHeader.biSizeImage / height;
    if (!stride) return;

    /* window_surface_flush() reports damage in surface-local coordinates.
       Bytes outside that rectangle in a temporary DIB are not guaranteed to
       belong to this frame, so preserve the persistent backing there. */
    update = *dirty;
    update.left = max(update.left, 0);
    update.top = max(update.top, 0);
    update.right = min(update.right, color_info->bmiHeader.biWidth);
    update.bottom = min(update.bottom, height);
    if (update.left >= update.right || update.top >= update.bottom) return;

    copy_width = (update.right - update.left) * 4;
    for (row = update.top; row < update.bottom; row++)
    {
        memcpy(dst + row * stride + update.left * 4,
               src + row * stride + update.left * 4, copy_width);
    }
}

static void macdrv_surface_set_clip(struct window_surface *window_surface, const RECT *rects, UINT count)
{
}

static BOOL macdrv_surface_flush(struct window_surface *window_surface, const RECT *rect, const RECT *dirty,
                                 const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                 const BITMAPINFO *shape_info, const void *shape_bits)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);
    CGImageAlphaInfo alpha_info = window_surface->alpha_mask ?
                                  kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst;
    CGDataProviderRef provider;
    CGColorSpaceRef colorspace;
    CGImageRef image;
    struct macdrv_dib_endpoint *replaced_endpoint = NULL;
    size_t image_size = color_info->bmiHeader.biSizeImage;
    BOOL presented = TRUE;

    TRACE("Switchyard submitting %s frame source %p rect %s dirty %s\n",
          surface->dib_endpoint ? "remote-DIB" : "native-root", window_surface->hwnd,
          wine_dbgstr_rect(rect), wine_dbgstr_rect(dirty));

    merge_dirty_bits(surface->bits, dirty, color_info, color_bits);

    if (surface->dib_endpoint)
    {
        pthread_mutex_lock(&surface->dib_endpoint->mutex);
        merge_dirty_bits(surface->dib_endpoint->bits, dirty, color_info, surface->bits);
        provider = snapshot_data_provider_create(surface->dib_endpoint->bits, image_size);
    }
    else
        provider = snapshot_data_provider_create(surface->bits, image_size);

    if (!provider)
    {
        if (surface->dib_endpoint) pthread_mutex_unlock(&surface->dib_endpoint->mutex);
        return TRUE;
    }
    if (!(colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB)))
    {
        CGDataProviderRelease(provider);
        if (surface->dib_endpoint) pthread_mutex_unlock(&surface->dib_endpoint->mutex);
        return TRUE;
    }

    image = CGImageCreate(color_info->bmiHeader.biWidth, abs(color_info->bmiHeader.biHeight), 8, 32,
                          image_size / abs(color_info->bmiHeader.biHeight), colorspace,
                          alpha_info | kCGBitmapByteOrder32Little, provider, NULL,
                          retina_on, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorspace);
    CGDataProviderRelease(provider);
    if (!image)
    {
        if (surface->dib_endpoint) pthread_mutex_unlock(&surface->dib_endpoint->mutex);
        return TRUE;
    }

    if (surface->dib_endpoint)
        presented = macdrv_image_layer_set_color_image(surface->dib_endpoint->image_layer, image,
                                                        cgrect_from_rect(*rect));
    else if (surface->window)
        macdrv_window_set_color_image(surface->window, image, cgrect_from_rect(*rect),
                                      cgrect_from_rect(*dirty));
    CGImageRelease(image);

    if (surface->dib_endpoint)
    {
        if (presented)
        {
            replaced_endpoint = surface->dib_endpoint->replaced_endpoint;
            surface->dib_endpoint->replaced_endpoint = NULL;
        }
        pthread_mutex_unlock(&surface->dib_endpoint->mutex);
        if (replaced_endpoint) dib_endpoint_release(replaced_endpoint);
    }

    if (presented && surface->replaced_surface)
    {
        window_surface_release(surface->replaced_surface);
        surface->replaced_surface = NULL;
    }

    if (shape_changed && surface->window)
    {
        if (!shape_bits)
            macdrv_window_set_shape_image(surface->window, NULL);
        else
        {
            const BYTE *src = shape_bits;
            CGDataProviderRef shape_provider;
            BYTE *dst;
            UINT i;

            if (!(shape_provider = mutable_data_provider_create(shape_info->bmiHeader.biSizeImage,
                                                                (void **)&dst)))
                return TRUE;
            for (i = 0; i < shape_info->bmiHeader.biSizeImage; i++)
                dst[i] = ~src[i]; /* CGImage mask bits are inverted */

            image = CGImageMaskCreate(shape_info->bmiHeader.biWidth,
                                      abs(shape_info->bmiHeader.biHeight), 1, 1,
                                      shape_info->bmiHeader.biSizeImage /
                                      abs(shape_info->bmiHeader.biHeight),
                                      shape_provider, NULL, retina_on);
            CGDataProviderRelease(shape_provider);
            macdrv_window_set_shape_image(surface->window, image);
            CGImageRelease(image);
        }
    }

    return TRUE;
}

static void macdrv_surface_destroy(struct window_surface *window_surface)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);

    TRACE("freeing %p source %p replacement %p\n", surface, window_surface->hwnd,
          surface->replaced_surface);
    if (surface->dib_endpoint) dib_endpoint_release(surface->dib_endpoint);
    if (surface->replaced_surface) window_surface_release(surface->replaced_surface);
    CGDataProviderRelease(surface->backing_provider);
}

static const struct window_surface_funcs macdrv_surface_funcs =
{
    macdrv_surface_set_clip,
    macdrv_surface_flush,
    macdrv_surface_destroy,
};

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface)
{
    if (!surface || surface->funcs != &macdrv_surface_funcs) return NULL;
    return (struct macdrv_window_surface *)surface;
}

static struct window_surface *create_surface(HWND hwnd, macdrv_window window, const RECT *rect)
{
    struct macdrv_window_surface *surface;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    DWORD window_background;
    D3DKMT_CREATEDCFROMMEMORY desc = {.Format = D3DDDIFMT_A8R8G8B8};
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    struct window_surface *window_surface;
    CGDataProviderRef provider;
    HBITMAP bitmap = 0;
    UINT status;
    void *bits;

    if (width <= 0 || height <= 0) return NULL;

    memset(info, 0, sizeof(*info));
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height; /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = get_dib_image_size(info);
    info->bmiHeader.biCompression = BI_RGB;

    if (!(provider = mutable_data_provider_create(info->bmiHeader.biSizeImage, &bits))) return NULL;
    window_background = macdrv_window_background_color();
    memset_pattern4(bits, &window_background, info->bmiHeader.biSizeImage);

    desc.Width = info->bmiHeader.biWidth;
    desc.Height = abs(info->bmiHeader.biHeight);
    desc.Pitch = info->bmiHeader.biSizeImage / abs(info->bmiHeader.biHeight);
    desc.pMemory = bits;
    desc.hDeviceDc = NtUserGetDCEx(hwnd, 0, DCX_CACHE | DCX_WINDOW);
    if ((status = NtGdiDdDDICreateDCFromMemory(&desc)))
        ERR("Failed to create HBITMAP, status %#x\n", status);
    else
    {
        bitmap = desc.hBitmap;
        NtGdiDeleteObjectApp(desc.hDc);
    }
    if (desc.hDeviceDc) NtUserReleaseDC(hwnd, desc.hDeviceDc);

    if (!(window_surface = window_surface_create(sizeof(*surface), &macdrv_surface_funcs,
                                                 hwnd, rect, info, bitmap)))
    {
        if (bitmap) NtGdiDeleteObjectApp(bitmap);
        CGDataProviderRelease(provider);
        return NULL;
    }

    surface = get_mac_surface(window_surface);
    surface->window = window;
    surface->dib_endpoint = NULL;
    surface->backing_provider = provider;
    surface->bits = bits;

    if (!window)
    {
        surface->dib_endpoint = acquire_dib_endpoint(hwnd, rect, info->bmiHeader.biSizeImage,
                                                     window_background);
        if (!surface->dib_endpoint)
        {
            window_surface_release(window_surface);
            return NULL;
        }
        pthread_mutex_lock(&surface->dib_endpoint->mutex);
        memcpy(surface->bits, surface->dib_endpoint->bits, info->bmiHeader.biSizeImage);
        pthread_mutex_unlock(&surface->dib_endpoint->mutex);
        window_surface->flush_on_unlock = TRUE;
    }

    return window_surface;
}

BOOL macdrv_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect,
                                struct window_surface **surface)
{
    struct window_surface *previous = *surface;
    struct window_surface *replacement;
    struct macdrv_window_surface *previous_mac;
    struct macdrv_win_data *data;
    macdrv_window window = NULL;

    TRACE("hwnd %p, layered %u, surface_rect %s, surface %p previous %p\n",
          hwnd, layered, wine_dbgstr_rect(surface_rect), surface, previous);

    if ((data = get_win_data(hwnd)))
    {
        window = data->cocoa_window;
        if (layered)
        {
            data->layered = TRUE;
            data->ulw_layered = TRUE;
        }
        release_win_data(data);
    }

    if ((previous_mac = get_mac_surface(previous)) &&
        EqualRect(surface_rect, &previous->rect) &&
        previous_mac->window == window)
        return TRUE;

    if (!(replacement = create_surface(hwnd, window, surface_rect))) return TRUE;
    if (previous) seed_replacement_surface(replacement, previous);
    get_mac_surface(replacement)->replaced_surface = previous;
    *surface = replacement;

    TRACE("Switchyard created %s compositor surface source %p\n",
          window ? "native-root" : "remote-DIB", hwnd);
    return TRUE;
}
