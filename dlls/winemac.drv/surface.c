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

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

#define ROOT_SURFACE_SHM_MAGIC 0x53595253 /* "SYRS" */
#define ROOT_SURFACE_SHM_VERSION 1

struct root_surface_shm_header
{
    UINT magic;
    UINT version;
    UINT header_size;
    UINT sequence;
    RECT rect;
    RECT dirty;
    UINT width;
    UINT height;
    UINT stride;
    UINT bits_size;
    UINT alpha_mask;
};

static UINT64 root_surface_hash(HWND root, HWND source)
{
    UINT64 hash = 1469598103934665603ULL;
    UINT64 values[] = { getuid(), (UINT_PTR)root, (UINT_PTR)source };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(values); i++)
    {
        UINT64 value = values[i];
        unsigned int byte;

        for (byte = 0; byte < sizeof(value); byte++)
        {
            hash ^= value & 0xff;
            hash *= 1099511628211ULL;
            value >>= 8;
        }
    }

    return hash;
}

static void root_surface_shm_name(char *name, size_t size, HWND root, HWND source)
{
    snprintf(name, size, "/sy%016llx", (unsigned long long)root_surface_hash(root, source));
}

static void unlink_root_surface_shm(HWND root, HWND source)
{
    char name[32];

    if (!root || !source) return;
    root_surface_shm_name(name, sizeof(name), root, source);
    shm_unlink(name);
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
    BOOL                    root_surface_relay;
    HWND                    root_surface_root;
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

static BOOL is_chromium_dcomp_target_window(HWND hwnd)
{
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

    return !wcscmp(class_name, intermediate_d3d_window);
}

static const WCHAR wine_window_topmost_composed[] =
    {'w','i','n','e','_','w','i','n','d','o','w','_','t','o','p','m','o','s','t','_','c','o','m','p','o','s','e','d',0};
static const WCHAR wine_window_non_topmost_composed[] =
    {'w','i','n','e','_','w','i','n','d','o','w','_','n','o','n','_','t','o','p','m','o','s','t','_','c','o','m','p','o','s','e','d',0};

static BOOL is_dcomp_composed_hwnd(HWND hwnd)
{
    return hwnd && (NtUserGetProp(hwnd, wine_window_topmost_composed) ||
                   NtUserGetProp(hwnd, wine_window_non_topmost_composed));
}

static BOOL is_chrome_widget_window(HWND hwnd)
{
    static const WCHAR chrome_widget_prefix[] =
        {'C','h','r','o','m','e','_','W','i','d','g','e','t','W','i','n','_',0};
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

    return !wcsncmp(class_name, chrome_widget_prefix, ARRAY_SIZE(chrome_widget_prefix) - 1);
}

static BOOL is_chrome_render_widget_host_window(HWND hwnd)
{
    static const WCHAR chrome_render_widget[] =
        {'C','h','r','o','m','e','_','R','e','n','d','e','r','W','i','d','g','e','t','H','o','s','t','H','W','N','D',0};
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

    return !wcscmp(class_name, chrome_render_widget);
}

static BOOL chrome_subtree_has_render_widget(HWND hwnd)
{
    HWND child;

    if (is_chrome_render_widget_host_window(hwnd)) return TRUE;

    for (child = NtUserGetWindowRelative(hwnd, GW_CHILD); child;
         child = NtUserGetWindowRelative(child, GW_HWNDNEXT))
    {
        if (chrome_subtree_has_render_widget(child)) return TRUE;
    }

    return FALSE;
}

static BOOL chrome_root_has_render_widget(HWND root)
{
    return root && is_chrome_widget_window(root) && chrome_subtree_has_render_widget(root);
}

static BOOL chromium_subtree_has_dcomp_target(HWND hwnd)
{
    HWND child;

    if (is_chromium_cef_child_window(hwnd) && is_dcomp_composed_hwnd(hwnd)) return TRUE;

    for (child = NtUserGetWindowRelative(hwnd, GW_CHILD); child;
         child = NtUserGetWindowRelative(child, GW_HWNDNEXT))
    {
        if (chromium_subtree_has_dcomp_target(child)) return TRUE;
    }

    return FALSE;
}

static BOOL chromium_root_uses_dcomp_composition(HWND root)
{
    HWND child;

    if (!root) return FALSE;
    if (is_dcomp_composed_hwnd(root)) return TRUE;

    for (child = NtUserGetWindowRelative(root, GW_CHILD); child;
         child = NtUserGetWindowRelative(child, GW_HWNDNEXT))
    {
        if (chromium_subtree_has_dcomp_target(child)) return TRUE;
    }

    return FALSE;
}

static BOOL chromium_hwnd_uses_dcomp_root_composition(HWND hwnd)
{
    HWND root;

    if (!is_chromium_cef_child_window(hwnd)) return FALSE;
    if (is_dcomp_composed_hwnd(hwnd)) return TRUE;

    root = NtUserGetAncestor(hwnd, GA_ROOT);
    return root && root != hwnd && chromium_root_uses_dcomp_composition(root);
}

static BOOL chromium_hwnd_or_root_uses_dcomp_composition(HWND hwnd)
{
    HWND root;

    if (!is_chromium_cef_child_window(hwnd)) return FALSE;
    if (is_dcomp_composed_hwnd(hwnd)) return TRUE;

    root = NtUserGetAncestor(hwnd, GA_ROOT);
    return root && chromium_root_uses_dcomp_composition(root);
}

static BOOL chromium_hwnd_or_root_uses_root_surface_composition(HWND hwnd)
{
    HWND root;

    if (chromium_hwnd_or_root_uses_dcomp_composition(hwnd)) return TRUE;
    if (!is_chromium_cef_child_window(hwnd)) return FALSE;

    root = NtUserGetAncestor(hwnd, GA_ROOT);
    return root && chrome_root_has_render_widget(root);
}

static BOOL chromium_hwnd_or_owner_uses_root_surface_composition(HWND hwnd)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    HWND owner;

    if (!root) return FALSE;
    if (chromium_hwnd_or_root_uses_root_surface_composition(hwnd)) return TRUE;

    owner = NtUserGetAncestor(root, GA_ROOTOWNER);
    return owner && owner != root &&
           is_chromium_cef_child_window(root) && is_chromium_cef_child_window(owner) &&
           (chromium_root_uses_dcomp_composition(owner) || chrome_root_has_render_widget(owner));
}

static HWND get_chromium_owner_composition_root(HWND hwnd)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    HWND owner;

    if (!root) return NULL;
    owner = NtUserGetAncestor(root, GA_ROOTOWNER);
    if (owner && owner != root &&
        is_chromium_cef_child_window(root) && is_chromium_cef_child_window(owner) &&
        (chromium_root_uses_dcomp_composition(root) || chromium_root_uses_dcomp_composition(owner) ||
         chrome_root_has_render_widget(root) || chrome_root_has_render_widget(owner)))
        return owner;

    return root;
}

static BOOL chromium_hwnd_should_root_compose_surface(HWND hwnd)
{
    HWND root;

    if (!is_chromium_cef_child_window(hwnd)) return FALSE;
    if (is_dcomp_composed_hwnd(hwnd) || is_chromium_dcomp_target_window(hwnd)) return FALSE;

    root = NtUserGetAncestor(hwnd, GA_ROOT);
    return root && root != hwnd &&
           (chromium_root_uses_dcomp_composition(root) || chrome_root_has_render_widget(root));
}

static BOOL chromium_hwnd_should_relay_root_surface(HWND hwnd, HWND *root_ret)
{
    HWND source_root, root;
    struct macdrv_win_data *root_data;
    DWORD root_thread_id;
    BOOL has_owner_root_window = FALSE;
    BOOL relay_to_owner_root;

    if (root_ret) *root_ret = NULL;
    if (!is_chromium_cef_child_window(hwnd)) return FALSE;
    if (is_dcomp_composed_hwnd(hwnd) || is_chromium_dcomp_target_window(hwnd)) return FALSE;
    if (!chromium_hwnd_or_owner_uses_root_surface_composition(hwnd)) return FALSE;

    source_root = NtUserGetAncestor(hwnd, GA_ROOT);
    root = get_chromium_owner_composition_root(hwnd);
    if (!root) return FALSE;
    if (source_root == hwnd && root == hwnd)
    {
        struct macdrv_win_data *data = get_win_data(hwnd);

        if (data)
        {
            release_win_data(data);
            return FALSE;
        }
    }
    relay_to_owner_root = source_root && source_root != root;
    root_thread_id = NtUserGetWindowThread(root, NULL);

    if ((root_data = get_win_data(root)))
    {
        if (root_data->foreign_child && root_data->cocoa_window && root_data->on_screen)
        {
            TRACE("Switchyard hiding stale Chromium/CEF foreign root window %p for relay source %p\n",
                  root, hwnd);
            macdrv_hide_cocoa_window(root_data->cocoa_window);
            root_data->on_screen = FALSE;
        }
        has_owner_root_window = root_thread_id == GetCurrentThreadId() &&
                                root_data->cocoa_window && !root_data->foreign_child;
        release_win_data(root_data);
    }
    if (has_owner_root_window && !relay_to_owner_root) return FALSE;

    if (root_ret) *root_ret = root;
    return TRUE;
}

static BOOL chromium_root_surface_should_preserve_alpha(HWND root, HWND source)
{
    HWND source_root = NtUserGetAncestor(source, GA_ROOT);
    HWND source_owner = source_root ? NtUserGetAncestor(source_root, GA_ROOTOWNER) : NULL;

    return source_root && source_owner && source_owner == root && source_owner != source_root &&
           is_chromium_cef_child_window(source_root) && is_chromium_cef_child_window(root) &&
           (chromium_root_uses_dcomp_composition(source_root) ||
            chromium_root_uses_dcomp_composition(root) ||
            chrome_root_has_render_widget(source_root) ||
            chrome_root_has_render_widget(root));
}

static BOOL root_surface_header_is_valid(const struct root_surface_shm_header *header, SIZE_T map_size)
{
    SIZE_T min_stride, expected_bits;

    if (header->magic != ROOT_SURFACE_SHM_MAGIC ||
        header->version != ROOT_SURFACE_SHM_VERSION ||
        header->header_size != sizeof(*header) ||
        header->width == 0 || header->height == 0 ||
        header->stride == 0 || header->bits_size == 0 ||
        sizeof(*header) > map_size)
        return FALSE;

    if ((SIZE_T)header->width > ((SIZE_T)-1) / 4) return FALSE;
    min_stride = (SIZE_T)header->width * 4;
    if ((SIZE_T)header->stride < min_stride) return FALSE;
    if ((SIZE_T)header->height > ((SIZE_T)-1) / header->stride) return FALSE;

    expected_bits = (SIZE_T)header->stride * header->height;
    if (header->bits_size != expected_bits) return FALSE;
    if (expected_bits > map_size - sizeof(*header)) return FALSE;

    return TRUE;
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

static BOOL chromium_root_has_smaller_hosted_layer(HWND root)
{
    struct macdrv_win_data *data;
    BOOL ret = FALSE;

    if (!root) return FALSE;

    if ((data = get_win_data(root)))
    {
        ret = !!data->chromium_smaller_layer_hosted_once;
        release_win_data(data);
    }

    return ret;
}

static BOOL should_suppress_chromium_placeholder_surface(struct macdrv_window_surface *surface)
{
    HWND root;

    if (!surface->child && !surface->remote_child && !surface->foreign_child) return FALSE;
    if (!is_chromium_cef_child_window(surface->header.hwnd)) return FALSE;
    if (!chromium_hwnd_or_root_uses_root_surface_composition(surface->header.hwnd)) return FALSE;
    root = NtUserGetAncestor(surface->header.hwnd, GA_ROOT);
    if (!chromium_root_has_smaller_hosted_layer(root)) return FALSE;
    return !!find_full_root_chromium_placeholder(surface->header.hwnd, root);
}

static BOOL get_child_root_offset(HWND hwnd, POINT *offset)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    struct macdrv_win_data *root_data;
    RECT rect;
    UINT root_dpi;

    if (!root || root == hwnd) return FALSE;
    root_dpi = NtUserGetWinMonitorDpi(root, MDT_RAW_DPI);
    if (!NtUserGetClientRect(hwnd, &rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI))) return FALSE;

    NtUserMapWindowPoints(hwnd, root, (POINT *)&rect, 2, root_dpi);
    offset->x = rect.left;
    offset->y = rect.top;
    if ((root_data = get_win_data(root)))
    {
        offset->x += root_data->rects.client.left - root_data->rects.visible.left;
        offset->y += root_data->rects.client.top - root_data->rects.visible.top;
        release_win_data(root_data);
    }
    return TRUE;
}

static BOOL get_root_surface_offset(HWND root, HWND source, POINT *offset, macdrv_window *window)
{
    struct macdrv_win_data *root_data;
    RECT rect;

    offset->x = 0;
    offset->y = 0;
    *window = NULL;

    if (!root || !source) return FALSE;
    if (!(root_data = get_win_data(root))) return FALSE;
    if (!root_data->cocoa_window)
    {
        release_win_data(root_data);
        return FALSE;
    }

    if (source != root)
    {
        UINT root_dpi = NtUserGetWinMonitorDpi(root, MDT_RAW_DPI);

        if (!NtUserGetClientRect(source, &rect, NtUserGetWinMonitorDpi(source, MDT_RAW_DPI)))
        {
            release_win_data(root_data);
            return FALSE;
        }
        NtUserMapWindowPoints(source, root, (POINT *)&rect, 2, root_dpi);
        offset->x = rect.left;
        offset->y = rect.top;
    }

    offset->x += root_data->rects.client.left - root_data->rects.visible.left;
    offset->y += root_data->rects.client.top - root_data->rects.visible.top;
    *window = root_data->cocoa_window;
    release_win_data(root_data);
    return TRUE;
}

static BOOL relay_root_surface_flush(struct macdrv_window_surface *surface, const RECT *rect, const RECT *dirty,
                                     const BITMAPINFO *color_info, const void *pixels)
{
    struct root_surface_shm_header *header;
    HWND root = surface->root_surface_root;
    SIZE_T bits_size, map_size;
    unsigned int width, height, stride;
    struct stat st;
    char name[32];
    void *map;
    int fd;

    if (!root || !pixels) return FALSE;
    if (color_info->bmiHeader.biBitCount != 32 || color_info->bmiHeader.biCompression != BI_RGB) return FALSE;
    if (color_info->bmiHeader.biWidth <= 0 || color_info->bmiHeader.biHeight == 0) return FALSE;
    width = color_info->bmiHeader.biWidth;
    height = abs(color_info->bmiHeader.biHeight);
    stride = get_dib_stride(width, color_info->bmiHeader.biBitCount);
    if (!stride || (SIZE_T)height > ((SIZE_T)-1) / stride) return FALSE;
    bits_size = (SIZE_T)stride * height;
    if (!bits_size || bits_size > (UINT)-1 || bits_size > ((SIZE_T)-1) - sizeof(*header)) return FALSE;
    map_size = sizeof(*header) + bits_size;

    root_surface_shm_name(name, sizeof(name), root, surface->header.hwnd);
    if ((fd = shm_open(name, O_CREAT | O_RDWR, 0600)) == -1)
    {
        TRACE("Switchyard failed to open Chromium root-surface shm %s: %s\n", name, strerror(errno));
        return FALSE;
    }
    if (fstat(fd, &st) == -1)
    {
        TRACE("Switchyard failed to stat Chromium root-surface shm %s: %s\n", name, strerror(errno));
        close(fd);
        return FALSE;
    }
    if (st.st_size < 0)
    {
        TRACE("Switchyard Chromium root-surface shm %s has invalid size %lld\n",
              name, (long long)st.st_size);
        close(fd);
        return FALSE;
    }
    if ((SIZE_T)st.st_size < map_size && ftruncate(fd, map_size) == -1)
    {
        TRACE("Switchyard failed to resize Chromium root-surface shm %s from %lld to %lu: %s\n",
              name, (long long)st.st_size, (unsigned long)map_size, strerror(errno));
        close(fd);
        shm_unlink(name);
        if ((fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600)) == -1)
        {
            TRACE("Switchyard failed to recreate Chromium root-surface shm %s: %s\n", name, strerror(errno));
            return FALSE;
        }
        if (ftruncate(fd, map_size) == -1)
        {
            TRACE("Switchyard failed to resize recreated Chromium root-surface shm %s to %lu: %s\n",
                  name, (unsigned long)map_size, strerror(errno));
            close(fd);
            shm_unlink(name);
            return FALSE;
        }
    }
    map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
    {
        TRACE("Switchyard failed to map Chromium root-surface shm %s: %s\n", name, strerror(errno));
        return FALSE;
    }

    header = map;
    header->sequence++;
    header->magic = ROOT_SURFACE_SHM_MAGIC;
    header->version = ROOT_SURFACE_SHM_VERSION;
    header->header_size = sizeof(*header);
    header->rect = *rect;
    header->dirty = *dirty;
    header->width = width;
    header->height = height;
    header->stride = stride;
    header->bits_size = (UINT)bits_size;
    header->alpha_mask = surface->header.alpha_mask;
    if (!header->alpha_mask && chromium_root_surface_should_preserve_alpha(root, surface->header.hwnd))
        header->alpha_mask = 0xff000000;
    memcpy((BYTE *)map + sizeof(*header), pixels, bits_size);
    header->sequence++;
    munmap(map, map_size);

    TRACE("Switchyard relaying Chromium/CEF surface hwnd %p through DComp root %p via shm %s rect %s dirty %s\n",
          surface->header.hwnd, root, name, wine_dbgstr_rect(rect), wine_dbgstr_rect(dirty));
    send_message_timeout(root, WM_MACDRV_PRESENT_ROOT_SURFACE, (WPARAM)surface->header.hwnd, 0,
                         SMTO_ABORTIFHUNG, 500, NULL);
    return TRUE;
}

BOOL macdrv_present_root_surface(HWND root, HWND source)
{
    const struct root_surface_shm_header *header;
    CGColorSpaceRef colorspace;
    CGDataProviderRef provider;
    CGImageAlphaInfo alpha_info;
    CGImageRef image, cropped_image;
    macdrv_window window;
    POINT offset;
    SIZE_T map_size;
    CGRect image_rect, dirty_rect;
    CFDataRef data;
    RECT rect, dirty;
    struct stat st;
    char name[32];
    void *map;
    int fd;

    if (!root || !source) return FALSE;
    if (source != root)
    {
        HWND source_root = NtUserGetAncestor(source, GA_ROOT);
        HWND source_owner = source_root ? NtUserGetAncestor(source_root, GA_ROOTOWNER) : NULL;

        if (source_root != root && source_owner != root) return FALSE;
    }
    if (!chromium_hwnd_or_owner_uses_root_surface_composition(source)) return FALSE;

    root_surface_shm_name(name, sizeof(name), root, source);
    if ((fd = shm_open(name, O_RDONLY, 0600)) == -1)
    {
        TRACE("Switchyard failed to open Chromium root-surface present shm %s: %s\n", name, strerror(errno));
        return FALSE;
    }
    if (fstat(fd, &st) == -1 || st.st_size < sizeof(*header))
    {
        close(fd);
        return FALSE;
    }
    map_size = st.st_size;
    map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return FALSE;

    header = map;
    if (!root_surface_header_is_valid(header, map_size))
    {
        munmap(map, map_size);
        return FALSE;
    }
    if (!get_root_surface_offset(root, source, &offset, &window))
    {
        munmap(map, map_size);
        return FALSE;
    }

    rect = header->rect;
    dirty = header->dirty;
    OffsetRect(&rect, offset.x, offset.y);
    OffsetRect(&dirty, offset.x, offset.y);

    data = CFDataCreate(NULL, (const UInt8 *)map + sizeof(*header), header->bits_size);
    if (!data)
    {
        munmap(map, map_size);
        return FALSE;
    }
    provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider)
    {
        munmap(map, map_size);
        return FALSE;
    }

    alpha_info = header->alpha_mask ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst;
    colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    image = CGImageCreate(header->width, header->height, 8, 32, header->stride, colorspace,
                          alpha_info | kCGBitmapByteOrder32Little, provider, NULL, retina_on,
                          kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorspace);
    CGDataProviderRelease(provider);
    munmap(map, map_size);
    if (!image) return FALSE;

    if (rect.left < 0 || rect.top < 0)
    {
        int crop_x = rect.left < 0 ? -rect.left : 0;
        int crop_y = rect.top < 0 ? -rect.top : 0;
        size_t image_width = CGImageGetWidth(image);
        size_t image_height = CGImageGetHeight(image);
        CGRect crop_rect;

        if ((size_t)crop_x >= image_width || (size_t)crop_y >= image_height)
        {
            CGImageRelease(image);
            return FALSE;
        }

        crop_rect = CGRectMake(crop_x, crop_y, image_width - crop_x, image_height - crop_y);
        if (!(cropped_image = CGImageCreateWithImageInRect(image, crop_rect)))
        {
            CGImageRelease(image);
            return FALSE;
        }
        CGImageRelease(image);
        image = cropped_image;

        if (crop_x) rect.left = 0;
        if (crop_y) rect.top = 0;
        if (dirty.left < rect.left) dirty.left = rect.left;
        if (dirty.top < rect.top) dirty.top = rect.top;
        if (dirty.right > rect.right) dirty.right = rect.right;
        if (dirty.bottom > rect.bottom) dirty.bottom = rect.bottom;
    }

    image_rect = cgrect_from_rect(rect);
    dirty_rect = cgrect_from_rect(dirty);
    TRACE("Switchyard presenting Chromium/CEF surface hwnd %p into DComp root %p backing rect %s dirty %s alpha %#x\n",
          source, root, wine_dbgstr_rect(&rect), wine_dbgstr_rect(&dirty), header->alpha_mask);
    macdrv_window_set_color_image(window, image, image_rect, dirty_rect);
    CGImageRelease(image);
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
    HWND root;
    BOOL has_remote_layer_hosts = FALSE;
    BOOL had_remote_layer_host = FALSE;

    *remote_layer_host = FALSE;

    if (surface->child || surface->remote_child || surface->foreign_child) return FALSE;
    root = NtUserGetAncestor(surface->header.hwnd, GA_ROOT);
    if (!chromium_root_has_smaller_hosted_layer(root)) return FALSE;
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
    macdrv_window root_composed_window = NULL;
    POINT root_composed_offset = {0, 0};
    BOOL root_composed_surface = FALSE;

    if (color_bits && color_bits != surface->bits)
        memcpy(surface->bits, color_bits, color_info->bmiHeader.biSizeImage);

    solid_kind = get_nearly_solid_surface_kind(color_info, color_bits ? color_bits : surface->bits);

    if (surface->root_surface_relay)
    {
        if (solid_kind == SOLID_SURFACE_DARK && surface->header.hwnd == surface->root_surface_root &&
            NtUserGetAncestor(surface->header.hwnd, GA_ROOT) == surface->header.hwnd)
        {
            TRACE("Switchyard suppressing dark Chromium/CEF root relay placeholder hwnd %p\n",
                  surface->header.hwnd);
            return TRUE;
        }
        if (!relay_root_surface_flush(surface, rect, dirty, color_info, color_bits ? color_bits : surface->bits))
            TRACE("Switchyard failed to relay Chromium/CEF surface hwnd %p into DComp root %p\n",
                  surface->header.hwnd, surface->root_surface_root);
        return TRUE;
    }

    if ((surface->child || surface->remote_child || surface->foreign_child) &&
        chromium_hwnd_should_root_compose_surface(surface->header.hwnd))
    {
        HWND root = NtUserGetAncestor(surface->header.hwnd, GA_ROOT);
        struct macdrv_win_data *root_data = root ? get_win_data(root) : NULL;

        if (root_data && root_data->cocoa_window &&
            get_child_root_offset(surface->header.hwnd, &root_composed_offset))
        {
            struct macdrv_win_data *data;

            root_composed_window = root_data->cocoa_window;
            root_composed_surface = TRUE;

            if (surface->image_layer)
            {
                macdrv_destroy_image_layer(surface->image_layer);
                surface->image_layer = NULL;
            }
            if (surface->foreign_child && surface->window)
            {
                macdrv_hide_cocoa_window(surface->window);
                if ((data = get_win_data(surface->header.hwnd)))
                {
                    data->on_screen = FALSE;
                    release_win_data(data);
                }
            }
            TRACE("Switchyard composing Chromium/CEF child surface hwnd %p into DComp root %p backing at offset %s\n",
                  surface->header.hwnd, root, wine_dbgstr_point(&root_composed_offset));
        }
        if (root_data)
            release_win_data(root_data);
    }

    if ((surface->child || surface->remote_child || surface->foreign_child) &&
        chromium_hwnd_uses_dcomp_root_composition(surface->header.hwnd) && !root_composed_surface)
    {
        if (surface->image_layer)
        {
            macdrv_destroy_image_layer(surface->image_layer);
            surface->image_layer = NULL;
        }
        if (surface->foreign_child && surface->window)
            macdrv_hide_cocoa_window(surface->window);
        TRACE("Switchyard suppressing Chromium/CEF DComp target surface flush hwnd %p because DComp owns root composition\n",
              surface->header.hwnd);
        return TRUE;
    }

    if (should_suppress_chromium_placeholder_surface(surface) && solid_kind != SOLID_SURFACE_NONE)
    {
        TRACE("Switchyard suppressing solid full-root Chromium/CEF surface flush hwnd %p with smaller viewport sibling\n",
              surface->header.hwnd);
        return TRUE;
    }

    if (surface->image_layer && solid_kind != SOLID_SURFACE_NONE &&
        is_chromium_cef_child_window(surface->header.hwnd) &&
        chromium_root_has_smaller_hosted_layer(NtUserGetAncestor(surface->header.hwnd, GA_ROOT)))
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

        if (!root_composed_surface && surface->child && !surface->image_layer)
        {
            POINT offset = surface->offset;

            if (is_chromium_cef_child_window(surface->header.hwnd))
                get_child_root_offset(surface->header.hwnd, &offset);

            OffsetRect(&translated_rect, offset.x, offset.y);
            OffsetRect(&translated_dirty, offset.x, offset.y);
        }
        else if (!root_composed_surface && !surface->remote_child)
            sync_foreign_child_surface_frame(surface);

        if (root_composed_surface)
        {
            OffsetRect(&translated_rect, root_composed_offset.x, root_composed_offset.y);
            OffsetRect(&translated_dirty, root_composed_offset.x, root_composed_offset.y);
            macdrv_window_set_color_image(root_composed_window, image, cgrect_from_rect(translated_rect),
                                          cgrect_from_rect(translated_dirty));
        }
        else if (surface->image_layer)
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
    if (surface->root_surface_relay)
        unlink_root_surface_shm(surface->root_surface_root, window_surface->hwnd);
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
                                             HWND remote_layer_root, BOOL foreign_child,
                                             BOOL root_surface_relay, HWND root_surface_root)
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
        surface->root_surface_relay = root_surface_relay;
        surface->root_surface_root = root_surface_root;
        surface->chromium_blank_owner_transparent = FALSE;
        surface->chromium_blank_owner_had_remote_layer = FALSE;
        surface->chromium_child_had_real_image = FALSE;
        window_surface->flush_on_unlock = child || remote_child || foreign_child || root_surface_relay;

        if (!root_surface_relay && (child || remote_child) && remote_layer_root)
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
    BOOL root_composed_child = chromium_hwnd_should_root_compose_surface(hwnd);
    HWND remote_layer_root = NULL;
    HWND root_surface_root = NULL;
    BOOL root_surface_relay = chromium_hwnd_should_relay_root_surface(hwnd, &root_surface_root);

    TRACE("hwnd %p, layered %u, surface_rect %s, surface %p\n", hwnd, layered, wine_dbgstr_rect(surface_rect), surface);

    if ((previous = *surface) && previous->funcs == &macdrv_surface_funcs)
    {
        struct macdrv_window_surface *mac_surface = get_mac_surface(previous);

        if (!mac_surface->child && !mac_surface->remote_child && !mac_surface->foreign_child &&
            !mac_surface->root_surface_relay)
            return TRUE;
    }
    if (chromium_hwnd_uses_dcomp_root_composition(hwnd) && !root_composed_child && !root_surface_relay)
    {
        TRACE("Switchyard leaving Chromium/CEF child hwnd %p on Wine DComp root composition\n", hwnd);
        if (previous)
        {
            window_surface_release(previous);
            *surface = NULL;
        }
        return TRUE;
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

    if (!window && !root_surface_relay && !root_composed_child &&
        chromium_hwnd_or_root_uses_root_surface_composition(hwnd) &&
        NtUserGetAncestor(hwnd, GA_ROOT) == hwnd)
    {
        TRACE("Switchyard leaving Chromium/CEF root hwnd %p on its owner root window\n", hwnd);
        if (previous)
        {
            window_surface_release(previous);
            *surface = NULL;
        }
        return TRUE;
    }

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
                remote_layer_root = root_composed_child ? NULL : root;
                child = TRUE;
                TRACE("Switchyard redirecting Chromium/CEF child surface hwnd %p to root %p offset %s%s\n",
                      hwnd, root, wine_dbgstr_point(&offset),
                      root_composed_child ? " for DComp root backing composition" : "");
            }
            release_win_data(root_data);
        }
        else TRACE("Switchyard Chromium/CEF child surface hwnd %p has no root mac win data for root %p\n",
                   hwnd, root);
    }

    if (!root_surface_relay && !root_composed_child && !window && is_chromium_cef_child_window(hwnd) &&
        (remote_layer_root = NtUserGetAncestor(hwnd, GA_ROOT)) && remote_layer_root != hwnd &&
        can_host_remote_layer(hwnd, remote_layer_root))
    {
        remote_child = TRUE;
        TRACE("Switchyard exporting Chromium/CEF foreign child surface hwnd %p to root %p remote layer\n",
              hwnd, remote_layer_root);
    }

    if (!root_surface_relay && !root_composed_child && !remote_child && !window && is_chromium_cef_child_window(hwnd) &&
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

    if (root_surface_relay)
        TRACE("Switchyard creating Chromium/CEF root-surface relay hwnd %p to DComp root %p\n",
              hwnd, root_surface_root);

    if (window || remote_child || root_surface_relay)
    {
        if (window)
            foreign_child = macdrv_retain_foreign_child_win_data(hwnd);
        if (!(*surface = create_surface(hwnd, window, surface_rect, &offset, child, remote_child,
                                        remote_layer_root, foreign_child,
                                        root_surface_relay, root_surface_root)) && foreign_child)
            macdrv_release_foreign_child_win_data(hwnd);
    }

    if (!root_surface_relay && !root_composed_child && !*surface && remote_child && is_chromium_cef_child_window(hwnd) &&
        (data = macdrv_create_foreign_child_win_data(hwnd, surface_rect)))
    {
        window = data->cocoa_window;
        child = FALSE;
        foreign_child = macdrv_retain_foreign_child_win_data(hwnd);
        TRACE("Switchyard falling back to standalone Chromium/CEF child surface window hwnd %p window %p\n",
              hwnd, window);
        if (!(*surface = create_surface(hwnd, window, surface_rect, &offset, child, FALSE, NULL, foreign_child,
                                        FALSE, NULL)) &&
            foreign_child)
            macdrv_release_foreign_child_win_data(hwnd);
        release_win_data(data);
    }

    return TRUE;
}
