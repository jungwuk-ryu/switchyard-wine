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


struct macdrv_window_surface
{
    struct window_surface   header;
    macdrv_window           window;
    macdrv_image_layer      image_layer;
    HWND                    remote_layer_root;
    CGDataProviderRef       provider;
    void                   *bits;
    POINT                   offset;
    BOOL                    child;
    BOOL                    remote_child;
    BOOL                    foreign_child;
    BOOL                    foreign_child_fronted;
    BOOL                    chromium_blank_owner_transparent;
};

static BOOL is_chromium_cef_child_window(HWND hwnd)
{
    static const WCHAR cef_browser_window[] =
        {'C','e','f','B','r','o','w','s','e','r','W','i','n','d','o','w',0};
    static const WCHAR chrome_render_widget[] =
        {'C','h','r','o','m','e','_','R','e','n','d','e','r','W','i','d','g','e','t','H','o','s','t','H','W','N','D',0};
    static const WCHAR chrome_widget_prefix[] =
        {'C','h','r','o','m','e','_','W','i','d','g','e','t','W','i','n','_',0};
    static const WCHAR intermediate_d3d_window[] =
        {'I','n','t','e','r','m','e','d','i','a','t','e',' ','D','3','D',' ','W','i','n','d','o','w',0};
    WCHAR class_name[64];
    UNICODE_STRING name =
    {
        .Buffer = class_name,
        .MaximumLength = sizeof(class_name),
    };
    int len;

    if (!(len = NtUserGetClassName(hwnd, FALSE, &name))) return FALSE;

    if (len >= ARRAY_SIZE(class_name)) len = ARRAY_SIZE(class_name) - 1;
    class_name[len] = 0;

    return !wcscmp(class_name, cef_browser_window)
        || !wcscmp(class_name, chrome_render_widget)
        || !wcscmp(class_name, intermediate_d3d_window)
        || !wcsncmp(class_name, chrome_widget_prefix, ARRAY_SIZE(chrome_widget_prefix) - 1);
}

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface);

static CGDataProviderRef data_provider_create(size_t size, void **bits)
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

/***********************************************************************
 *              macdrv_surface_set_clip
 */
static void macdrv_surface_set_clip(struct window_surface *window_surface, const RECT *rects, UINT count)
{
}

static BOOL is_blank_chromium_owner_surface(struct macdrv_window_surface *surface,
                                            const BITMAPINFO *color_info, const void *color_bits)
{
    const DWORD *pixels = color_bits ? color_bits : surface->bits;
    unsigned int width, height, stride_pixels, x_step, y_step, samples = 0, blank_samples = 0;
    unsigned int x, y;
    struct macdrv_win_data *data;
    BOOL has_remote_layer_hosts = FALSE;

    if (surface->child || surface->remote_child || surface->foreign_child) return FALSE;
    if (!is_chromium_cef_child_window(surface->header.hwnd)) return FALSE;
    if ((data = get_win_data(surface->header.hwnd)))
    {
        has_remote_layer_hosts = !!data->remote_layer_hosts;
        release_win_data(data);
    }
    if (has_remote_layer_hosts) return FALSE;
    if (!pixels || color_info->bmiHeader.biBitCount != 32 || color_info->bmiHeader.biCompression != BI_RGB)
        return FALSE;

    width = color_info->bmiHeader.biWidth;
    height = abs(color_info->bmiHeader.biHeight);
    if (width < 400 || height < 300) return FALSE;

    stride_pixels = color_info->bmiHeader.biSizeImage / height / sizeof(*pixels);
    if (stride_pixels < width) return FALSE;

    x_step = width / 32;
    y_step = height / 24;
    if (!x_step) x_step = 1;
    if (!y_step) y_step = 1;

    for (y = 0; y < height; y += y_step)
    {
        const DWORD *row = pixels + y * stride_pixels;

        for (x = 0; x < width; x += x_step)
        {
            DWORD pixel = row[x];
            BYTE blue = pixel & 0xff;
            BYTE green = (pixel >> 8) & 0xff;
            BYTE red = (pixel >> 16) & 0xff;

            samples++;
            if (red <= 8 && green <= 8 && blue <= 8)
                blank_samples++;
        }
    }

    return samples && blank_samples * 100 >= samples * 98;
}

static void sync_blank_chromium_owner_opacity(struct macdrv_window_surface *surface,
                                             const BITMAPINFO *color_info, const void *color_bits)
{
    BOOL blank = is_blank_chromium_owner_surface(surface, color_info, color_bits);

    if (blank && !surface->chromium_blank_owner_transparent)
    {
        TRACE("making blank Chromium/CEF owner host hwnd %p window %p nearly transparent\n",
              surface->header.hwnd, surface->window);
        macdrv_set_window_alpha(surface->window, 0.01);
        surface->chromium_blank_owner_transparent = TRUE;
    }
    else if (!blank && surface->chromium_blank_owner_transparent)
    {
        TRACE("restoring Chromium/CEF owner host hwnd %p window %p opacity\n",
              surface->header.hwnd, surface->window);
        macdrv_set_window_alpha(surface->window, 1.0);
        surface->chromium_blank_owner_transparent = FALSE;
    }
}

static void sync_foreign_child_surface_frame(struct macdrv_window_surface *surface)
{
    HWND hwnd = surface->header.hwnd;
    struct macdrv_win_data *data;
    RECT rect;
    UINT dpi;
    CGRect frame;

    if (!surface->foreign_child || !surface->window) return;

    dpi = NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI);
    if (!NtUserGetClientRect(hwnd, &rect, dpi)) return;
    NtUserMapWindowPoints(hwnd, 0, (POINT *)&rect, 2, dpi);
    if (IsRectEmpty(&rect)) return;

    if ((data = get_win_data(hwnd)))
    {
        data->rects.window = rect;
        data->rects.client = rect;
        data->rects.visible = rect;
        release_win_data(data);
    }

    frame = cgrect_from_rect(rect);
    macdrv_set_cocoa_window_frame(surface->window, &frame);
    if (!surface->foreign_child_fronted)
    {
        macdrv_set_cocoa_window_ignores_mouse_events(surface->window, TRUE);
        surface->foreign_child_fronted = TRUE;
    }
    macdrv_order_cocoa_window(surface->window, NULL, NULL, FALSE);
}

/***********************************************************************
 *              macdrv_surface_flush
 */
static BOOL macdrv_surface_flush(struct window_surface *window_surface, const RECT *rect, const RECT *dirty,
                                 const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                 const BITMAPINFO *shape_info, const void *shape_bits)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);
    CGImageAlphaInfo alpha_info = (window_surface->alpha_mask ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst);
    RECT translated_rect = *rect, translated_dirty = *dirty;
    CGColorSpaceRef colorspace;
    CGImageRef image;

    colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    if (color_bits && color_bits != surface->bits)
        memcpy(surface->bits, color_bits, color_info->bmiHeader.biSizeImage);

    sync_blank_chromium_owner_opacity(surface, color_info, color_bits);

    image = CGImageCreate(color_info->bmiHeader.biWidth, abs(color_info->bmiHeader.biHeight), 8, 32,
                          color_info->bmiHeader.biSizeImage / abs(color_info->bmiHeader.biHeight), colorspace,
                          alpha_info | kCGBitmapByteOrder32Little, surface->provider, NULL, retina_on, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorspace);

    if (surface->child)
    {
        OffsetRect(&translated_rect, surface->offset.x, surface->offset.y);
        OffsetRect(&translated_dirty, surface->offset.x, surface->offset.y);
    }
    else if (!surface->remote_child)
        sync_foreign_child_surface_frame(surface);

    if (surface->remote_child)
        macdrv_image_layer_set_color_image(surface->image_layer, image, cgrect_from_rect(*rect));
    else
        macdrv_window_set_color_image(surface->window, image, cgrect_from_rect(translated_rect),
                                      cgrect_from_rect(translated_dirty));
    CGImageRelease(image);

    if (shape_changed && !surface->child && !surface->remote_child)
    {
        if (!shape_bits)
            macdrv_window_set_shape_image(surface->window, NULL);
        else
        {
            const BYTE *src = shape_bits;
            CGDataProviderRef provider;
            CGImageRef image;
            BYTE *dst;
            UINT i;

            if (!(provider = data_provider_create(shape_info->bmiHeader.biSizeImage, (void **)&dst))) return TRUE;
            for (i = 0; i < shape_info->bmiHeader.biSizeImage; i++) dst[i] = ~src[i]; /* CGImage mask bits are inverted */

            image = CGImageMaskCreate(shape_info->bmiHeader.biWidth, abs(shape_info->bmiHeader.biHeight), 1, 1,
                                      shape_info->bmiHeader.biSizeImage / abs(shape_info->bmiHeader.biHeight),
                                      provider, NULL, retina_on);
            CGDataProviderRelease(provider);

            macdrv_window_set_shape_image(surface->window, image);
            CGImageRelease(image);
        }
    }

    return TRUE;
}

/***********************************************************************
 *              macdrv_surface_destroy
 */
static void macdrv_surface_destroy(struct window_surface *window_surface)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);

    TRACE("freeing %p\n", surface);
    if (surface->image_layer)
        macdrv_destroy_image_layer(surface->image_layer);
    CGDataProviderRelease(surface->provider);
    if (surface->foreign_child)
        macdrv_release_foreign_child_win_data(window_surface->hwnd);
    if (surface->chromium_blank_owner_transparent)
        macdrv_set_window_alpha(surface->window, 1.0);
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

/***********************************************************************
 *              create_surface
 */
static struct window_surface *create_surface(HWND hwnd, macdrv_window window, const RECT *rect,
                                             const POINT *offset, BOOL child, BOOL remote_child,
                                             HWND remote_layer_root, BOOL foreign_child)
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

    memset(info, 0, sizeof(*info));
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height; /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = get_dib_image_size(info);
    info->bmiHeader.biCompression = BI_RGB;

    if (!(provider = data_provider_create(info->bmiHeader.biSizeImage, &bits))) return NULL;
    window_background = macdrv_window_background_color();
    memset_pattern4(bits, &window_background, info->bmiHeader.biSizeImage);

    /* wrap the data in a HBITMAP so we can write to the surface pixels directly */
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

    if (!(window_surface = window_surface_create(sizeof(*surface), &macdrv_surface_funcs, hwnd, rect, info, bitmap)))
    {
        if (bitmap) NtGdiDeleteObjectApp(bitmap);
        CGDataProviderRelease(provider);
    }
    else
    {
        surface = get_mac_surface(window_surface);
        surface->window = window;
        surface->image_layer = NULL;
        surface->remote_layer_root = remote_layer_root;
        surface->provider = provider;
        surface->bits = bits;
        surface->offset = *offset;
        surface->child = child;
        surface->remote_child = remote_child;
        surface->foreign_child = foreign_child;
        surface->foreign_child_fronted = FALSE;
        surface->chromium_blank_owner_transparent = FALSE;
        window_surface->flush_on_unlock = child || remote_child || foreign_child;

        if (remote_child &&
            !(surface->image_layer = macdrv_create_image_layer(hwnd, remote_layer_root, cgrect_from_rect(*rect))))
        {
            window_surface_release(window_surface);
            return NULL;
        }
    }

    return window_surface;
}

static BOOL can_host_remote_layer(HWND hwnd)
{
    DWORD_PTR result = 0;

    if (!NtUserGetWindowThread(hwnd, NULL)) return FALSE;
    if (!send_message_timeout(hwnd, WM_MACDRV_CAN_HOST_REMOTE_LAYER, 0, 0,
                              SMTO_ABORTIFHUNG, 500, &result))
    {
        TRACE("Switchyard remote layer host check timed out or failed for hwnd %p\n", hwnd);
        return FALSE;
    }

    return !!result;
}

/***********************************************************************
 *              CreateWindowSurface   (MACDRV.@)
 */
BOOL macdrv_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface)
{
    struct window_surface *previous;
    struct macdrv_win_data *data;
    macdrv_window window = NULL;
    POINT offset = {0, 0};
    BOOL child = FALSE;
    BOOL remote_child = FALSE;
    BOOL foreign_child = FALSE;
    HWND remote_layer_root = NULL;

    TRACE("hwnd %p, layered %u, surface_rect %s, surface %p\n", hwnd, layered, wine_dbgstr_rect(surface_rect), surface);

    if ((previous = *surface) && previous->funcs == &macdrv_surface_funcs)
    {
        struct macdrv_window_surface *mac_surface = get_mac_surface(previous);

        if (!mac_surface->child) return TRUE;
    }
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
    else if (!is_chromium_cef_child_window(hwnd)) return TRUE; /* use default surface */
    else TRACE("Switchyard Chromium/CEF child hwnd %p has no local mac win data; trying root redirection\n", hwnd);

    if (!window && is_chromium_cef_child_window(hwnd))
    {
        HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
        struct macdrv_win_data *root_data;

        if (root && root != hwnd && (root_data = get_win_data(root)))
        {
            if (root_data->cocoa_window)
            {
                UINT dpi = NtUserGetWinMonitorDpi(root, MDT_RAW_DPI);

                NtUserMapWindowPoints(hwnd, root, &offset, 1, dpi);
                window = root_data->cocoa_window;
                child = TRUE;
                TRACE("Switchyard redirecting Chromium/CEF child surface hwnd %p to root %p offset %s\n",
                      hwnd, root, wine_dbgstr_point(&offset));
            }
            release_win_data(root_data);
        }
        else TRACE("Switchyard Chromium/CEF child surface hwnd %p has no root mac win data for root %p\n",
                   hwnd, root);
    }

    if (!window && is_chromium_cef_child_window(hwnd) &&
        (remote_layer_root = NtUserGetAncestor(hwnd, GA_ROOT)) && remote_layer_root != hwnd &&
        can_host_remote_layer(hwnd))
    {
        remote_child = TRUE;
        TRACE("Switchyard exporting Chromium/CEF foreign child surface hwnd %p to root %p remote layer\n",
              hwnd, remote_layer_root);
    }

    if (!remote_child && !window && is_chromium_cef_child_window(hwnd) &&
        (data = macdrv_create_foreign_child_win_data(hwnd, surface_rect)))
    {
        window = data->cocoa_window;
        child = FALSE;
        TRACE("Switchyard created standalone Chromium/CEF child surface window hwnd %p window %p\n",
              hwnd, window);
        release_win_data(data);
    }

    if (previous)
    {
        window_surface_release(previous);
        *surface = NULL;
    }

    if (window || remote_child)
    {
        if (window)
            foreign_child = macdrv_retain_foreign_child_win_data(hwnd);
        if (!(*surface = create_surface(hwnd, window, surface_rect, &offset, child, remote_child,
                                        remote_layer_root, foreign_child)) && foreign_child)
            macdrv_release_foreign_child_win_data(hwnd);
    }

    if (!*surface && remote_child && is_chromium_cef_child_window(hwnd) &&
        (data = macdrv_create_foreign_child_win_data(hwnd, surface_rect)))
    {
        window = data->cocoa_window;
        child = FALSE;
        foreign_child = macdrv_retain_foreign_child_win_data(hwnd);
        TRACE("Switchyard falling back to standalone Chromium/CEF child surface window hwnd %p window %p\n",
              hwnd, window);
        if (!(*surface = create_surface(hwnd, window, surface_rect, &offset, child, FALSE, NULL, foreign_child)) &&
            foreign_child)
            macdrv_release_foreign_child_win_data(hwnd);
        release_win_data(data);
    }

    return TRUE;
}
