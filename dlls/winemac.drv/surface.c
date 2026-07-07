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
    BOOL                    chromium_blank_owner_had_remote_layer;
    BOOL                    chromium_child_had_real_image;
};

enum solid_surface_kind
{
    SOLID_SURFACE_NONE,
    SOLID_SURFACE_DARK,
    SOLID_SURFACE_LIGHT,
    SOLID_SURFACE_OTHER,
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

static int rect_width(const RECT *rect)
{
    return rect->right - rect->left;
}

static int rect_height(const RECT *rect)
{
    return rect->bottom - rect->top;
}

static BOOL rect_nearly_covers_client(const RECT *rect, int width, int height)
{
    int rect_w = rect_width(rect);
    int rect_h = rect_height(rect);

    return rect->left >= -1 && rect->left <= 1 &&
           rect->top >= -1 && rect->top <= 1 &&
           rect_w >= width - 2 && rect_w <= width + 2 &&
           rect_h >= height - 2 && rect_h <= height + 2;
}

static BOOL has_visible_smaller_chromium_viewport(HWND root, HWND hwnd)
{
    UINT root_dpi = NtUserGetWinMonitorDpi(root, MDT_RAW_DPI);
    HWND child;
    RECT root_client;
    int root_width, root_height;

    if (!NtUserGetClientRect(root, &root_client, root_dpi)) return FALSE;
    root_width = rect_width(&root_client);
    root_height = rect_height(&root_client);
    if (root_width <= 0 || root_height <= 0) return FALSE;

    for (child = NtUserGetWindowRelative(root, GW_CHILD); child;
         child = NtUserGetWindowRelative(child, GW_HWNDNEXT))
    {
        RECT rect;
        int width, height;

        if (child == hwnd) continue;
        if (!NtUserIsWindowVisible(child)) continue;
        if (!is_chromium_cef_child_window(child)) continue;
        if (NtUserGetAncestor(child, GA_ROOT) != root) continue;
        if (!NtUserGetClientRect(child, &rect, NtUserGetWinMonitorDpi(child, MDT_RAW_DPI))) continue;

        NtUserMapWindowPoints(child, root, (POINT *)&rect, 2, root_dpi);
        width = rect_width(&rect);
        height = rect_height(&rect);
        if (width <= 0 || height <= 0) continue;
        if (rect_nearly_covers_client(&rect, root_width, root_height)) continue;
        if (width * 4 < root_width * 3) continue;
        if (height * 2 < root_height) continue;
        return TRUE;
    }

    return FALSE;
}

static BOOL is_full_root_chromium_placeholder(HWND hwnd, HWND root)
{
    UINT root_dpi = NtUserGetWinMonitorDpi(root, MDT_RAW_DPI);
    RECT root_client, rect;
    int root_width, root_height;

    if (!root || root == hwnd) return FALSE;
    if (!NtUserGetClientRect(root, &root_client, root_dpi)) return FALSE;
    root_width = rect_width(&root_client);
    root_height = rect_height(&root_client);
    if (root_width <= 0 || root_height <= 0) return FALSE;
    if (!NtUserGetClientRect(hwnd, &rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI))) return FALSE;

    NtUserMapWindowPoints(hwnd, root, (POINT *)&rect, 2, root_dpi);
    return rect_nearly_covers_client(&rect, root_width, root_height) &&
           has_visible_smaller_chromium_viewport(root, hwnd);
}

static HWND find_full_root_chromium_placeholder(HWND hwnd, HWND root)
{
    HWND candidate;

    if (!root || root == hwnd) return NULL;

    for (candidate = hwnd; candidate && candidate != root;
         candidate = NtUserGetAncestor(candidate, GA_PARENT))
    {
        if (!is_chromium_cef_child_window(candidate)) continue;
        if (NtUserGetAncestor(candidate, GA_ROOT) != root) continue;
        if (is_full_root_chromium_placeholder(candidate, root))
            return candidate;
    }

    return NULL;
}

static BOOL should_suppress_chromium_placeholder_surface(struct macdrv_window_surface *surface)
{
    HWND root;

    if (!surface->child && !surface->remote_child && !surface->foreign_child) return FALSE;
    if (!is_chromium_cef_child_window(surface->header.hwnd)) return FALSE;
    root = NtUserGetAncestor(surface->header.hwnd, GA_ROOT);
    return !!find_full_root_chromium_placeholder(surface->header.hwnd, root);
}

static BOOL get_child_root_offset(HWND hwnd, POINT *offset)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    RECT rect;
    UINT root_dpi;

    if (!root || root == hwnd) return FALSE;
    root_dpi = NtUserGetWinMonitorDpi(root, MDT_RAW_DPI);
    if (!NtUserGetClientRect(hwnd, &rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI))) return FALSE;

    NtUserMapWindowPoints(hwnd, root, (POINT *)&rect, 2, root_dpi);
    offset->x = rect.left;
    offset->y = rect.top;
    return TRUE;
}

static enum solid_surface_kind get_nearly_solid_surface_kind(const BITMAPINFO *color_info, const void *pixels)
{
    unsigned int width, height, stride_pixels, x_step, y_step, samples = 0, solid_samples = 0;
    unsigned int x, y;
    BYTE base_red = 0, base_green = 0, base_blue = 0;
    BOOL have_base = FALSE;

    if (!pixels || color_info->bmiHeader.biBitCount != 32 || color_info->bmiHeader.biCompression != BI_RGB)
        return SOLID_SURFACE_NONE;

    width = color_info->bmiHeader.biWidth;
    height = abs(color_info->bmiHeader.biHeight);
    if (width < 400 || height < 300) return SOLID_SURFACE_NONE;

    stride_pixels = color_info->bmiHeader.biSizeImage / height / sizeof(DWORD);
    if (stride_pixels < width) return SOLID_SURFACE_NONE;

    x_step = width / 32;
    y_step = height / 24;
    if (!x_step) x_step = 1;
    if (!y_step) y_step = 1;

    for (y = 0; y < height; y += y_step)
    {
        const DWORD *row = (const DWORD *)pixels + y * stride_pixels;

        for (x = 0; x < width; x += x_step)
        {
            DWORD pixel = row[x];
            BYTE blue = pixel & 0xff;
            BYTE green = (pixel >> 8) & 0xff;
            BYTE red = (pixel >> 16) & 0xff;

            if (!have_base)
            {
                base_red = red;
                base_green = green;
                base_blue = blue;
                have_base = TRUE;
            }

            samples++;
            if (abs(red - base_red) <= 8 &&
                abs(green - base_green) <= 8 &&
                abs(blue - base_blue) <= 8)
                solid_samples++;
        }
    }

    if (!samples || solid_samples * 100 < samples * 98)
        return SOLID_SURFACE_NONE;

    if (base_red <= 8 && base_green <= 8 && base_blue <= 8)
        return SOLID_SURFACE_DARK;
    if (base_red >= 247 && base_green >= 247 && base_blue >= 247)
        return SOLID_SURFACE_LIGHT;
    return SOLID_SURFACE_OTHER;
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

static BOOL is_solid_chromium_owner_placeholder(struct macdrv_window_surface *surface,
                                                enum solid_surface_kind solid_kind,
                                                BOOL *remote_layer_host)
{
    struct macdrv_win_data *data;
    BOOL has_remote_layer_hosts = FALSE;
    BOOL had_remote_layer_host = FALSE;

    *remote_layer_host = FALSE;

    if (surface->child || surface->remote_child || surface->foreign_child) return FALSE;
    if ((data = get_win_data(surface->header.hwnd)))
    {
        has_remote_layer_hosts = !!data->remote_layer_hosts;
        had_remote_layer_host = !!data->remote_layer_hosted_once;
        release_win_data(data);
    }
    if (!has_remote_layer_hosts &&
        !had_remote_layer_host &&
        !surface->chromium_blank_owner_had_remote_layer &&
        !is_chromium_cef_child_window(surface->header.hwnd) &&
        !has_visible_smaller_chromium_viewport(surface->header.hwnd, NULL)) return FALSE;
    if (solid_kind != SOLID_SURFACE_DARK && solid_kind != SOLID_SURFACE_LIGHT)
        return FALSE;

    *remote_layer_host = has_remote_layer_hosts || had_remote_layer_host;
    return TRUE;
}

static BOOL sync_blank_chromium_owner_surface(struct macdrv_window_surface *surface,
                                              enum solid_surface_kind solid_kind)
{
    BOOL remote_layer_host = FALSE;
    BOOL blank = is_solid_chromium_owner_placeholder(surface, solid_kind, &remote_layer_host);

    if (blank && remote_layer_host)
        surface->chromium_blank_owner_had_remote_layer = TRUE;

    if (blank)
    {
        if (surface->chromium_blank_owner_transparent)
        {
            TRACE("restoring Chromium/CEF owner host hwnd %p window %p opacity before clearing blank backing surface\n",
                  surface->header.hwnd, surface->window);
            macdrv_set_window_alpha(surface->window, 1.0);
            surface->chromium_blank_owner_transparent = FALSE;
        }

        TRACE("clearing blank Chromium/CEF owner host hwnd %p window %p behind remote layer hosts\n",
              surface->header.hwnd, surface->window);
        macdrv_window_clear_color_image(surface->window);
        return TRUE;
    }

    surface->chromium_blank_owner_had_remote_layer = FALSE;

    if (surface->chromium_blank_owner_transparent)
    {
        TRACE("restoring Chromium/CEF owner host hwnd %p window %p opacity\n",
              surface->header.hwnd, surface->window);
        macdrv_set_window_alpha(surface->window, 1.0);
        surface->chromium_blank_owner_transparent = FALSE;
    }

    return FALSE;
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
    CGColorSpaceRef colorspace = NULL;
    CGImageRef image;
    BOOL clear_color_image;
    enum solid_surface_kind solid_kind;

    if (color_bits && color_bits != surface->bits)
        memcpy(surface->bits, color_bits, color_info->bmiHeader.biSizeImage);

    solid_kind = get_nearly_solid_surface_kind(color_info, color_bits ? color_bits : surface->bits);

    if (should_suppress_chromium_placeholder_surface(surface) && solid_kind != SOLID_SURFACE_NONE)
    {
        TRACE("Switchyard suppressing solid full-root Chromium/CEF surface flush hwnd %p with smaller viewport sibling\n",
              surface->header.hwnd);
        return TRUE;
    }

    if (surface->image_layer && solid_kind != SOLID_SURFACE_NONE &&
        is_chromium_cef_child_window(surface->header.hwnd))
    {
        if (surface->chromium_child_had_real_image)
            TRACE("Switchyard holding last Chromium/CEF image-layer frame over solid placeholder hwnd %p\n",
                  surface->header.hwnd);
        else
            TRACE("Switchyard deferring Chromium/CEF image-layer host until real content for hwnd %p\n",
                  surface->header.hwnd);
        return TRUE;
    }

    clear_color_image = sync_blank_chromium_owner_surface(surface, solid_kind);

    if (!clear_color_image)
    {
        colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        image = CGImageCreate(color_info->bmiHeader.biWidth, abs(color_info->bmiHeader.biHeight), 8, 32,
                              color_info->bmiHeader.biSizeImage / abs(color_info->bmiHeader.biHeight), colorspace,
                              alpha_info | kCGBitmapByteOrder32Little, surface->provider, NULL, retina_on, kCGRenderingIntentDefault);
        CGColorSpaceRelease(colorspace);

        if (surface->child && !surface->image_layer)
        {
            POINT offset = surface->offset;

            if (is_chromium_cef_child_window(surface->header.hwnd))
                get_child_root_offset(surface->header.hwnd, &offset);

            OffsetRect(&translated_rect, offset.x, offset.y);
            OffsetRect(&translated_dirty, offset.x, offset.y);
        }
        else if (!surface->remote_child)
            sync_foreign_child_surface_frame(surface);

        if (surface->image_layer)
        {
            macdrv_image_layer_set_color_image(surface->image_layer, image, cgrect_from_rect(*rect));
            if (solid_kind == SOLID_SURFACE_NONE && is_chromium_cef_child_window(surface->header.hwnd))
                surface->chromium_child_had_real_image = TRUE;
        }
        else
            macdrv_window_set_color_image(surface->window, image, cgrect_from_rect(translated_rect),
                                          cgrect_from_rect(translated_dirty));
        CGImageRelease(image);
    }
    else if (!surface->remote_child)
        sync_foreign_child_surface_frame(surface);

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
        surface->chromium_blank_owner_had_remote_layer = FALSE;
        surface->chromium_child_had_real_image = FALSE;
        window_surface->flush_on_unlock = child || remote_child || foreign_child;

        if ((child || remote_child) && remote_layer_root)
        {
            surface->image_layer = macdrv_create_image_layer(hwnd, remote_layer_root, cgrect_from_rect(*rect));
            if (!surface->image_layer && remote_child)
            {
                window_surface_release(window_surface);
                return NULL;
            }
        }
    }

    return window_surface;
}

static BOOL can_host_remote_layer(HWND hwnd, HWND root)
{
    DWORD_PTR result = 0;

    if (!NtUserGetWindowThread(root, NULL)) return FALSE;
    if (!send_message_timeout(root, WM_MACDRV_CAN_HOST_REMOTE_LAYER, (WPARAM)hwnd, 0,
                              SMTO_ABORTIFHUNG, 500, &result))
    {
        TRACE("Switchyard remote layer host check timed out or failed for hwnd %p root %p\n", hwnd, root);
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
                remote_layer_root = root;
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
        can_host_remote_layer(hwnd, remote_layer_root))
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
