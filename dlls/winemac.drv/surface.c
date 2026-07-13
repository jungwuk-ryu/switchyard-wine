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
struct macdrv_window_surface
{
    struct window_surface header;
    macdrv_window         window;
    macdrv_image_layer    image_layer;
    struct window_surface *replaced_surface; /* retained until the first complete replacement frame */
    CGDataProviderRef     backing_provider;
    void                 *bits;
};

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface);

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
    size_t image_size = color_info->bmiHeader.biSizeImage;
    BOOL presented = TRUE;

    TRACE("Switchyard submitting %s frame source %p rect %s dirty %s\n",
          surface->image_layer ? "remote-DIB" : "native-root", window_surface->hwnd,
          wine_dbgstr_rect(rect), wine_dbgstr_rect(dirty));

    if (color_bits && color_bits != surface->bits)
        memcpy(surface->bits, color_bits, image_size);

    if (!(provider = snapshot_data_provider_create(surface->bits, image_size))) return TRUE;
    if (!(colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB)))
    {
        CGDataProviderRelease(provider);
        return TRUE;
    }

    image = CGImageCreate(color_info->bmiHeader.biWidth, abs(color_info->bmiHeader.biHeight), 8, 32,
                          image_size / abs(color_info->bmiHeader.biHeight), colorspace,
                          alpha_info | kCGBitmapByteOrder32Little, provider, NULL,
                          retina_on, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorspace);
    CGDataProviderRelease(provider);
    if (!image) return TRUE;

    if (surface->image_layer)
        presented = macdrv_image_layer_set_color_image(surface->image_layer, image,
                                                        cgrect_from_rect(*rect));
    else if (surface->window)
        macdrv_window_set_color_image(surface->window, image, cgrect_from_rect(*rect),
                                      cgrect_from_rect(*dirty));
    CGImageRelease(image);

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

    TRACE("freeing %p\n", surface);
    if (surface->image_layer) macdrv_destroy_image_layer(surface->image_layer);
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
    surface->image_layer = NULL;
    surface->backing_provider = provider;
    surface->bits = bits;

    if (!window)
    {
        surface->image_layer = macdrv_create_image_layer(hwnd, cgrect_from_rect(*rect));
        if (!surface->image_layer)
        {
            window_surface_release(window_surface);
            return NULL;
        }
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

    TRACE("hwnd %p, layered %u, surface_rect %s, surface %p\n",
          hwnd, layered, wine_dbgstr_rect(surface_rect), surface);

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
