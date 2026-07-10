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
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
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
#define ROOT_SURFACE_SHM_VERSION 3
#define ROOT_SURFACE_SHM_STABLE_NON_SOLID 0x00000001
#define ROOT_SURFACE_SHM_HELD_TRANSITION  0x00000002

struct root_surface_shm_header
{
    UINT magic;
    UINT version;
    UINT header_size;
    UINT sequence;
    UINT64 session_id;
    UINT flags;
    RECT rect;
    RECT dirty;
    UINT width;
    UINT height;
    UINT stride;
    UINT bits_size;
    UINT alpha_mask;
};

struct root_surface_snapshot
{
    struct root_surface_shm_header header;
    BYTE *bits;
};

static UINT64 root_surface_session_id(void)
{
    SYSTEM_TIMEOFDAY_INFORMATION info;
    static UINT64 cached;
    UINT64 session = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);

    if (session) return session;
    if (!NtQuerySystemInformation(SystemTimeOfDayInformation, &info, sizeof(info), NULL))
    {
        session = info.BootTime.QuadPart;
        __atomic_store_n(&cached, session, __ATOMIC_RELEASE);
        return session;
    }
    return 0;
}

static UINT64 root_surface_hash(HWND root, HWND source)
{
    UINT64 hash = 1469598103934665603ULL;
    UINT64 values[] = { getuid(), root_surface_session_id(), (UINT_PTR)root, (UINT_PTR)source };
    const char *prefix = getenv("WINEPREFIX");
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

    if (prefix)
    {
        while (*prefix)
        {
            hash ^= (unsigned char)*prefix++;
            hash *= 1099511628211ULL;
        }
    }

    return hash;
}

static void root_surface_shm_name(char *name, size_t size, HWND root, HWND source)
{
    snprintf(name, size, "/sy%016llx", (unsigned long long)root_surface_hash(root, source));
}

static void root_surface_lock_path(char *name, size_t size, HWND root, HWND source)
{
    snprintf(name, size, "/tmp/syl%016llx", (unsigned long long)root_surface_hash(root, source));
}

static pthread_mutex_t root_surface_writer_mutex = PTHREAD_MUTEX_INITIALIZER;

static int lock_root_surface_writer(HWND root, HWND source)
{
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    char name[32];
    int fd;

    pthread_mutex_lock(&root_surface_writer_mutex);
    root_surface_lock_path(name, sizeof(name), root, source);
    if ((fd = open(name, O_CREAT | O_RDWR, 0600)) == -1)
    {
        TRACE("Switchyard failed to open Chromium root-surface lock %s: %s\n", name, strerror(errno));
        pthread_mutex_unlock(&root_surface_writer_mutex);
        return -1;
    }
    while (fcntl(fd, F_SETLKW, &lock) == -1)
    {
        if (errno == EINTR) continue;
        TRACE("Switchyard failed to lock Chromium root-surface file %s: %s\n", name, strerror(errno));
        close(fd);
        pthread_mutex_unlock(&root_surface_writer_mutex);
        return -1;
    }
    return fd;
}

static void unlock_root_surface_writer(int fd)
{
    close(fd);
    pthread_mutex_unlock(&root_surface_writer_mutex);
}

static void unlink_root_surface_shm(HWND root, HWND source)
{
    char name[32];

    if (!root || !source) return;
    root_surface_shm_name(name, sizeof(name), root, source);
    shm_unlink(name);

    /* A relayed child or owned popup is composited over the root relay.  Once
       it disappears, re-present the root transport image so the single native
       backing no longer contains that overlay. */
    if (source != root)
        send_message_timeout(root, WM_MACDRV_PRESENT_ROOT_SURFACE, (WPARAM)source, 1,
                             SMTO_ABORTIFHUNG, 500, NULL);
}

void macdrv_release_root_surface_backing(HWND root)
{
    unlink_root_surface_shm(root, root);
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
    BOOL                    root_surface_relay;
    HWND                    root_surface_root;
    void                   *root_surface_backing;
    BOOL                    root_surface_had_non_solid_backing;
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

static enum solid_surface_kind get_nearly_solid_surface_kind(const BITMAPINFO *color_info,
                                                             const void *pixels);
static BOOL root_surface_dirty_covers_frame(const RECT *dirty, const BITMAPINFO *color_info);

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

static BOOL is_official_chrome_process_window(HWND hwnd)
{
    static const WCHAR chrome_exe[] = {'c','h','r','o','m','e','.','e','x','e',0};
    BYTE buffer[sizeof(UNICODE_STRING) + 1024 * sizeof(WCHAR)];
    UNICODE_STRING *image = (UNICODE_STRING *)buffer;
    const WCHAR *name, *end, *p;
    CLIENT_ID cid = {0};
    HANDLE process;
    DWORD pid;
    NTSTATUS status;

    if (!hwnd || !NtUserGetWindowThread(hwnd, &pid) || !pid) return FALSE;
    cid.UniqueProcess = ULongToHandle(pid);
    if (NtOpenProcess(&process, PROCESS_QUERY_LIMITED_INFORMATION, NULL, &cid)) return FALSE;
    status = NtQueryInformationProcess(process, ProcessImageFileNameWin32,
                                       buffer, sizeof(buffer), NULL);
    NtClose(process);
    if (status || !image->Buffer || image->Length < (ARRAY_SIZE(chrome_exe) - 1) * sizeof(WCHAR))
        return FALSE;

    name = image->Buffer;
    end = name + image->Length / sizeof(WCHAR);
    for (p = name; p < end; p++)
        if (*p == '\\' || *p == '/') name = p + 1;

    return end - name == ARRAY_SIZE(chrome_exe) - 1 &&
           !wcsnicmp(name, chrome_exe, ARRAY_SIZE(chrome_exe) - 1);
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
    BOOL relay_to_owner_root, chrome_top_level;

    if (root_ret) *root_ret = NULL;
    if (!is_chromium_cef_child_window(hwnd)) return FALSE;
    source_root = NtUserGetAncestor(hwnd, GA_ROOT);
    chrome_top_level = source_root == hwnd && is_chrome_widget_window(hwnd) &&
                       is_official_chrome_process_window(hwnd);
    if ((is_dcomp_composed_hwnd(hwnd) || is_chromium_dcomp_target_window(hwnd)) &&
        !chrome_top_level)
        return FALSE;
    if (!chrome_top_level && !chromium_hwnd_or_owner_uses_root_surface_composition(hwnd)) return FALSE;

    root = get_chromium_owner_composition_root(hwnd);
    if (!root) return FALSE;
    if (!chrome_top_level && source_root == hwnd && root == hwnd)
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
    if (has_owner_root_window && !relay_to_owner_root && !chrome_top_level) return FALSE;

    if (root_ret) *root_ret = root;
    return TRUE;
}

static BOOL chromium_root_surface_should_preserve_alpha(HWND root, HWND source)
{
    HWND source_root = NtUserGetAncestor(source, GA_ROOT);
    HWND source_owner = source_root ? NtUserGetAncestor(source_root, GA_ROOTOWNER) : NULL;

    return source != root && source_root &&
           (source_root == root || source_owner == root) &&
           is_chromium_cef_child_window(source) && is_chromium_cef_child_window(root) &&
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
        header->session_id != root_surface_session_id() ||
        header->width == 0 || header->height == 0 ||
        header->stride == 0 || header->bits_size == 0 ||
        sizeof(*header) > map_size)
        return FALSE;

    if (header->width > INT_MAX || header->height > INT_MAX) return FALSE;
    if ((SIZE_T)header->width > ((SIZE_T)-1) / 4) return FALSE;
    min_stride = (SIZE_T)header->width * 4;
    if ((SIZE_T)header->stride < min_stride) return FALSE;
    if ((SIZE_T)header->height > ((SIZE_T)-1) / header->stride) return FALSE;

    expected_bits = (SIZE_T)header->stride * header->height;
    if (header->bits_size != expected_bits) return FALSE;
    if (expected_bits > map_size - sizeof(*header)) return FALSE;

    return TRUE;
}

static void release_root_surface_snapshot(struct root_surface_snapshot *snapshot)
{
    free(snapshot->bits);
    snapshot->bits = NULL;
}

static BOOL read_root_surface_snapshot(HWND root, HWND source, struct root_surface_snapshot *snapshot)
{
    const struct root_surface_shm_header *mapped_header;
    struct root_surface_shm_header header;
    unsigned int attempt;
    SIZE_T map_size;
    struct stat st;
    char name[32];
    void *map;
    int fd;

    snapshot->bits = NULL;
    root_surface_shm_name(name, sizeof(name), root, source);
    if ((fd = shm_open(name, O_RDONLY, 0600)) == -1)
    {
        TRACE("Switchyard failed to open Chromium root-surface present shm %s: %s\n", name, strerror(errno));
        return FALSE;
    }
    if (fstat(fd, &st) == -1 || st.st_size < sizeof(header))
    {
        close(fd);
        return FALSE;
    }
    map_size = st.st_size;
    map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return FALSE;

    mapped_header = map;
    for (attempt = 0; attempt < 4; attempt++)
    {
        UINT sequence = __atomic_load_n(&mapped_header->sequence, __ATOMIC_ACQUIRE);
        BYTE *bits;

        if (sequence & 1) continue;
        header = *mapped_header;
        if (header.sequence != sequence || !root_surface_header_is_valid(&header, map_size)) continue;
        if (!(bits = malloc(header.bits_size))) break;
        memcpy(bits, (const BYTE *)map + sizeof(header), header.bits_size);
        if (__atomic_load_n(&mapped_header->sequence, __ATOMIC_SEQ_CST) == sequence)
        {
            snapshot->header = header;
            snapshot->bits = bits;
            munmap(map, map_size);
            return TRUE;
        }
        free(bits);
    }

    munmap(map, map_size);
    TRACE("Switchyard skipped unstable Chromium root-surface snapshot %s\n", name);
    return FALSE;
}

static BOOL update_root_surface_transition_flag(HWND root, HWND source, BOOL set, BOOL *was_set)
{
    struct root_surface_shm_header *header;
    SIZE_T map_size;
    struct stat st;
    char name[32];
    void *map;
    UINT sequence;
    int fd, lock_fd;
    BOOL valid = FALSE;

    if (was_set) *was_set = FALSE;
    root_surface_shm_name(name, sizeof(name), root, source);
    if ((lock_fd = lock_root_surface_writer(root, source)) == -1) return FALSE;
    if ((fd = shm_open(name, O_RDWR, 0600)) == -1)
    {
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }
    if (fstat(fd, &st) == -1 || st.st_size < (off_t)sizeof(*header))
    {
        close(fd);
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }
    map_size = st.st_size;
    if ((map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
    {
        close(fd);
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }
    header = map;
    if (root_surface_header_is_valid(header, map_size))
    {
        if (was_set) *was_set = !!(header->flags & ROOT_SURFACE_SHM_HELD_TRANSITION);
        sequence = __atomic_load_n(&header->sequence, __ATOMIC_RELAXED);
        if (sequence & 1) sequence++;
        __atomic_store_n(&header->sequence, sequence + 1, __ATOMIC_SEQ_CST);
        if (set)
            header->flags |= ROOT_SURFACE_SHM_HELD_TRANSITION;
        else
            header->flags &= ~ROOT_SURFACE_SHM_HELD_TRANSITION;
        __atomic_store_n(&header->sequence, sequence + 2, __ATOMIC_RELEASE);
        valid = TRUE;
    }
    munmap(map, map_size);
    close(fd);
    unlock_root_surface_writer(lock_fd);
    return valid;
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
    struct macdrv_win_data *data;
    HWND root;

    if (!surface->child && !surface->remote_child && !surface->foreign_child) return FALSE;
    if (!is_chromium_cef_child_window(surface->header.hwnd)) return FALSE;
    if (!chromium_hwnd_or_root_uses_root_surface_composition(surface->header.hwnd)) return FALSE;
    root = NtUserGetAncestor(surface->header.hwnd, GA_ROOT);
    if (!chromium_root_has_smaller_hosted_layer(root)) return FALSE;
    if ((data = get_win_data(root)))
    {
        BOOL client_only_root = macdrv_win_data_uses_chrome_client_only_frame(data);

        release_win_data(data);
        if (client_only_root) return FALSE;
    }
    return !!find_full_root_chromium_placeholder(surface->header.hwnd, root);
}

static BOOL get_child_root_offset(HWND hwnd, POINT *offset)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    struct macdrv_win_data *root_data;
    RECT cocoa_rect = {0};
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
        macdrv_offset_client_rect_to_cocoa_frame(root_data, &cocoa_rect);
        offset->x += cocoa_rect.left;
        offset->y += cocoa_rect.top;
        release_win_data(root_data);
    }
    return TRUE;
}

static BOOL get_root_surface_offset(HWND root, HWND source, POINT *offset, macdrv_window *window)
{
    struct macdrv_win_data *root_data;
    RECT cocoa_rect = {0};
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

    macdrv_offset_client_rect_to_cocoa_frame(root_data, &cocoa_rect);
    offset->x += cocoa_rect.left;
    offset->y += cocoa_rect.top;
    *window = root_data->cocoa_window;
    release_win_data(root_data);
    return TRUE;
}

static BOOL root_surface_dirty_covers_client(HWND root, const RECT *dirty,
                                             const BITMAPINFO *color_info)
{
    RECT client, update = *dirty;
    unsigned int width, height;

    if (root_surface_dirty_covers_frame(dirty, color_info)) return TRUE;
    if (!NtUserGetClientRect(root, &client, NtUserGetWinMonitorDpi(root, MDT_RAW_DPI))) return FALSE;
    width = client.right - client.left;
    height = client.bottom - client.top;
    if (!width || !height) return FALSE;

    update.left = max(update.left, client.left);
    update.top = max(update.top, client.top);
    update.right = min(update.right, client.right);
    update.bottom = min(update.bottom, client.bottom);
    if (update.left >= update.right || update.top >= update.bottom) return FALSE;

    return (unsigned int)(update.right - update.left) >= width - width / 10 &&
           (unsigned int)(update.bottom - update.top) >= height - height / 10;
}

static BOOL relay_root_surface_flush(struct macdrv_window_surface *surface, const RECT *rect, const RECT *dirty,
                                     const BITMAPINFO *color_info, const void *pixels)
{
    struct root_surface_shm_header *header;
    HWND root = surface->root_surface_root;
    SIZE_T bits_size, map_size;
    unsigned int width, height, stride, row, copy_width;
    BOOL merge_existing, dirty_covers_frame, shared_was_stable;
    enum solid_surface_kind incoming_solid_kind = SOLID_SURFACE_OTHER;
    RECT update;
    UINT sequence, flags;
    struct stat st;
    char name[32];
    void *map;
    int fd, lock_fd;

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
    if ((lock_fd = lock_root_surface_writer(root, surface->header.hwnd)) == -1) return FALSE;
    if (!NtUserIsWindow(root) || !NtUserIsWindow(surface->header.hwnd))
    {
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }
    if ((fd = shm_open(name, O_CREAT | O_RDWR, 0600)) == -1)
    {
        TRACE("Switchyard failed to open Chromium root-surface shm %s: %s\n", name, strerror(errno));
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }
    if (fstat(fd, &st) == -1)
    {
        TRACE("Switchyard failed to stat Chromium root-surface shm %s: %s\n", name, strerror(errno));
        close(fd);
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }
    if (st.st_size < 0)
    {
        TRACE("Switchyard Chromium root-surface shm %s has invalid size %lld\n",
              name, (long long)st.st_size);
        close(fd);
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }
    if ((SIZE_T)st.st_size < map_size && ftruncate(fd, map_size) == -1)
    {
        TRACE("Switchyard failed to resize Chromium root-surface shm %s from %lld to %lu: %s\n",
              name, (long long)st.st_size, (unsigned long)map_size, strerror(errno));
        close(fd);
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }
    map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
        TRACE("Switchyard failed to map Chromium root-surface shm %s: %s\n", name, strerror(errno));
        close(fd);
        unlock_root_surface_writer(lock_fd);
        return FALSE;
    }

    header = map;
    merge_existing = root_surface_header_is_valid(header, map_size) &&
                     header->width == width && header->height == height &&
                     header->stride == stride && header->bits_size == bits_size;
    flags = merge_existing ? header->flags : 0;
    shared_was_stable = !!(flags & ROOT_SURFACE_SHM_STABLE_NON_SOLID);
    dirty_covers_frame = root_surface_dirty_covers_client(root, dirty, color_info);
    if (dirty_covers_frame)
        incoming_solid_kind = get_nearly_solid_surface_kind(color_info, pixels);
    sequence = __atomic_load_n(&header->sequence, __ATOMIC_RELAXED);
    if (sequence & 1) sequence++;
    __atomic_store_n(&header->sequence, sequence + 1, __ATOMIC_SEQ_CST);
    header->magic = ROOT_SURFACE_SHM_MAGIC;
    header->version = ROOT_SURFACE_SHM_VERSION;
    header->header_size = sizeof(*header);
    header->session_id = root_surface_session_id();
    if (dirty_covers_frame && incoming_solid_kind == SOLID_SURFACE_NONE)
        flags |= ROOT_SURFACE_SHM_STABLE_NON_SOLID;
    header->flags = flags;
    header->rect = *rect;
    header->dirty = *dirty;
    header->width = width;
    header->height = height;
    header->stride = stride;
    header->bits_size = (UINT)bits_size;
    header->alpha_mask = surface->header.alpha_mask;
    if (!header->alpha_mask && chromium_root_surface_should_preserve_alpha(root, surface->header.hwnd))
        header->alpha_mask = 0xff000000;
    if (!merge_existing || (!shared_was_stable && dirty_covers_frame))
        memcpy((BYTE *)map + sizeof(*header), pixels, bits_size);
    else
    {
        BYTE *dst = (BYTE *)map + sizeof(*header);
        const BYTE *src = pixels;

        update = *dirty;
        update.left = max(update.left, 0);
        update.top = max(update.top, 0);
        update.right = min(update.right, width);
        update.bottom = min(update.bottom, height);
        if (update.left < update.right && update.top < update.bottom)
        {
            copy_width = (update.right - update.left) * 4;
            for (row = update.top; row < update.bottom; row++)
                memcpy(dst + row * stride + update.left * 4,
                       src + row * stride + update.left * 4, copy_width);
        }
    }
    __atomic_store_n(&header->sequence, sequence + 2, __ATOMIC_RELEASE);
    munmap(map, map_size);
    close(fd);
    unlock_root_surface_writer(lock_fd);

    TRACE("Switchyard relaying Chromium/CEF surface hwnd %p through DComp root %p via shm %s rect %s dirty %s merge %u stable %u covers-client %u solid %u\n",
          surface->header.hwnd, root, name, wine_dbgstr_rect(rect), wine_dbgstr_rect(dirty),
          merge_existing, !!(flags & ROOT_SURFACE_SHM_STABLE_NON_SOLID), dirty_covers_frame,
          incoming_solid_kind);
    send_message_timeout(root, WM_MACDRV_PRESENT_ROOT_SURFACE, (WPARAM)surface->header.hwnd, 0,
                         SMTO_ABORTIFHUNG, 500, NULL);
    return TRUE;
}

static void remember_root_surface_overlay(HWND root, HWND source)
{
    struct macdrv_win_data *data;
    CFIndex index;

    if (!root || !source || root == source || !(data = get_win_data(root))) return;
    if (!data->chromium_root_surface_overlays)
        data->chromium_root_surface_overlays = CFArrayCreateMutable(NULL, 0, NULL);
    if (data->chromium_root_surface_overlays)
    {
        index = CFArrayGetFirstIndexOfValue(data->chromium_root_surface_overlays,
                CFRangeMake(0, CFArrayGetCount(data->chromium_root_surface_overlays)), source);
        if (index != kCFNotFound)
            CFArrayRemoveValueAtIndex(data->chromium_root_surface_overlays, index);
        CFArrayAppendValue(data->chromium_root_surface_overlays, source);
    }
    release_win_data(data);
}

void macdrv_forget_root_surface_overlay(HWND root, HWND source)
{
    struct macdrv_win_data *data;
    CFIndex index;

    if (!root || !source || !(data = get_win_data(root))) return;
    if (data->chromium_root_surface_overlays)
    {
        index = CFArrayGetFirstIndexOfValue(data->chromium_root_surface_overlays,
                CFRangeMake(0, CFArrayGetCount(data->chromium_root_surface_overlays)), source);
        if (index != kCFNotFound)
            CFArrayRemoveValueAtIndex(data->chromium_root_surface_overlays, index);
    }
    release_win_data(data);
}

static HWND *copy_root_surface_overlays(HWND root, CFIndex *count)
{
    struct macdrv_win_data *data;
    HWND *sources = NULL;

    *count = 0;
    if (!(data = get_win_data(root))) return NULL;
    if (data->chromium_root_surface_overlays &&
        (*count = CFArrayGetCount(data->chromium_root_surface_overlays)) &&
        (sources = malloc(*count * sizeof(*sources))))
        CFArrayGetValues(data->chromium_root_surface_overlays, CFRangeMake(0, *count),
                         (const void **)sources);
    else
        *count = 0;
    release_win_data(data);
    return sources;
}

static BYTE *create_edge_connected_black_mask(const BYTE *bits, unsigned int width,
        unsigned int height, unsigned int stride, SIZE_T *masked_count)
{
    unsigned int *queue;
    SIZE_T count, mask_size, head = 0, tail = 0, light_count = 0, pos;
    BYTE *mask;
    int x, y;

    *masked_count = 0;
    if (!bits || !width || !height || stride < width * 4 ||
        (SIZE_T)width > ((SIZE_T)-1) / height)
        return NULL;
    count = (SIZE_T)width * height;
    if (count > ((SIZE_T)-1) - 7 || count > ((SIZE_T)-1) / sizeof(*queue)) return NULL;
    mask_size = (count + 7) / 8;
    if (!(mask = calloc(1, mask_size)) || !(queue = malloc(count * sizeof(*queue))))
    {
        free(mask);
        return NULL;
    }

#define EDGE_MASKED(pos) (mask[(pos) >> 3] & (1u << ((pos) & 7)))
#define EDGE_PUSH(px, py)                                                        \
    do                                                                           \
    {                                                                            \
        SIZE_T pos = (SIZE_T)(py) * width + (px);                                \
        const BYTE *pixel = bits + (py) * stride + (px) * 4;                     \
        if (!EDGE_MASKED(pos) && pixel[0] <= 12 && pixel[1] <= 12 && pixel[2] <= 12) \
        {                                                                        \
            mask[pos >> 3] |= 1u << (pos & 7);                                   \
            queue[tail++] = pos;                                                 \
        }                                                                        \
    } while (0)

    for (x = 0; x < width; x++)
    {
        EDGE_PUSH(x, 0);
        if (height > 1) EDGE_PUSH(x, height - 1);
    }
    for (y = 1; y < height - 1; y++)
    {
        EDGE_PUSH(0, y);
        if (width > 1) EDGE_PUSH(width - 1, y);
    }
    while (head < tail)
    {
        SIZE_T pos = queue[head++];

        x = pos % width;
        y = pos / width;
        if (x > 0) EDGE_PUSH(x - 1, y);
        if (x + 1 < width) EDGE_PUSH(x + 1, y);
        if (y > 0) EDGE_PUSH(x, y - 1);
        if (y + 1 < height) EDGE_PUSH(x, y + 1);
    }

#undef EDGE_PUSH
#undef EDGE_MASKED

    free(queue);
    for (pos = 0; pos < count; pos++)
    {
        const BYTE *pixel = bits + (pos / width) * stride + (pos % width) * 4;

        if (!(mask[pos >> 3] & (1u << (pos & 7))) &&
            pixel[0] >= 200 && pixel[1] >= 200 && pixel[2] >= 200)
            light_count++;
    }
    /* Only scrub opaque-black transport padding around a predominantly light
       popup.  A true dark popup can legitimately connect near-black content
       to its edge and must retain that background. */
    if (tail * 20 < count || tail * 2 >= count || light_count * 4 < count || count - tail < 64)
    {
        free(mask);
        return NULL;
    }
    *masked_count = tail;
    return mask;
}

static BOOL composite_root_surface_overlay(HWND root, HWND source, macdrv_window window,
        const struct root_surface_shm_header *base_header, const RECT *base_rect, BYTE *dst_bits)
{
    struct root_surface_snapshot snapshot = {0};
    const struct root_surface_shm_header *header;
    macdrv_window overlay_window;
    POINT offset;
    RECT rect, client_rect;
    BYTE *edge_mask;
    SIZE_T masked_count;
    int base_width = base_header->width, base_height = base_header->height;
    int client_width, client_height;
    int dst_x, dst_y, src_x = 0, src_y = 0, width, height, x, y;

    if (!read_root_surface_snapshot(root, source, &snapshot)) return FALSE;
    header = &snapshot.header;
    if (!get_root_surface_offset(root, source, &offset, &overlay_window) || overlay_window != window)
    {
        release_root_surface_snapshot(&snapshot);
        return FALSE;
    }
    if (!NtUserGetClientRect(source, &client_rect, NtUserGetWinMonitorDpi(source, MDT_RAW_DPI)))
    {
        release_root_surface_snapshot(&snapshot);
        return FALSE;
    }
    client_width = client_rect.right - client_rect.left;
    client_height = client_rect.bottom - client_rect.top;
    edge_mask = create_edge_connected_black_mask(snapshot.bits, header->width, header->height,
                                                  header->stride, &masked_count);

    rect = header->rect;
    OffsetRect(&rect, offset.x, offset.y);
    dst_x = rect.left - base_rect->left;
    dst_y = rect.top - base_rect->top;
    width = min((int)header->width, client_width);
    height = min((int)header->height, client_height);
    if (dst_x < 0) { src_x = -dst_x; width += dst_x; dst_x = 0; }
    if (dst_y < 0) { src_y = -dst_y; height += dst_y; dst_y = 0; }
    if (dst_x >= base_width) width = 0;
    else if (width > base_width - dst_x) width = base_width - dst_x;
    if (dst_y >= base_height) height = 0;
    else if (height > base_height - dst_y) height = base_height - dst_y;

    for (y = 0; y < height; y++)
    {
        BYTE *dst_pixel = dst_bits + (dst_y + y) * base_header->stride + dst_x * 4;
        const BYTE *src_pixel = snapshot.bits + (src_y + y) * header->stride + src_x * 4;

        for (x = 0; x < width; x++, dst_pixel += 4, src_pixel += 4)
        {
            SIZE_T src_pos = (SIZE_T)(src_y + y) * header->width + src_x + x;
            unsigned int alpha = edge_mask &&
                    (edge_mask[src_pos >> 3] & (1u << (src_pos & 7))) ? 0 :
                    header->alpha_mask ? src_pixel[3] : 255;
            unsigned int inv_alpha;

            if (!alpha) continue;
            if (alpha == 255)
            {
                dst_pixel[0] = src_pixel[0];
                dst_pixel[1] = src_pixel[1];
                dst_pixel[2] = src_pixel[2];
                dst_pixel[3] = 255;
                continue;
            }

            inv_alpha = 255 - alpha;
            dst_pixel[0] = min(255, src_pixel[0] + (dst_pixel[0] * inv_alpha + 127) / 255);
            dst_pixel[1] = min(255, src_pixel[1] + (dst_pixel[1] * inv_alpha + 127) / 255);
            dst_pixel[2] = min(255, src_pixel[2] + (dst_pixel[2] * inv_alpha + 127) / 255);
            dst_pixel[3] = 255;
        }
    }

    TRACE("Switchyard composited Chromium/CEF relay hwnd %p over single root %p backing rect %s alpha %#x edge-padding %lu\n",
          source, root, wine_dbgstr_rect(base_rect), header->alpha_mask, (unsigned long)masked_count);
    free(edge_mask);
    release_root_surface_snapshot(&snapshot);
    return TRUE;
}

BOOL macdrv_present_root_surface(HWND root, HWND source)
{
    struct root_surface_snapshot base_snapshot = {0};
    const struct root_surface_shm_header *header;
    CGColorSpaceRef colorspace;
    CGDataProviderRef provider;
    CGImageAlphaInfo alpha_info;
    CGImageRef image, cropped_image;
    macdrv_window window;
    POINT offset;
    CGRect image_rect, dirty_rect;
    CFDataRef data;
    RECT rect, dirty;
    BYTE *composed_bits = NULL;
    const BYTE *image_bits;
    HWND *overlays = NULL;
    CFIndex overlay_count = 0, i;
    BOOL first_stable_root_backing = FALSE;

    if (!root || !source) return FALSE;
    if (source != root)
    {
        HWND source_root = NtUserGetAncestor(source, GA_ROOT);
        HWND source_owner = source_root ? NtUserGetAncestor(source_root, GA_ROOTOWNER) : NULL;

        if (source_root != root && source_owner != root) return FALSE;
        if (!chromium_hwnd_or_owner_uses_root_surface_composition(source)) return FALSE;
        remember_root_surface_overlay(root, source);
    }
    else if (!chromium_hwnd_or_owner_uses_root_surface_composition(root)) return FALSE;

    if (!read_root_surface_snapshot(root, root, &base_snapshot))
    {
        TRACE("Switchyard skipped Chromium/CEF root %p presentation because no base relay is ready\n", root);
        return FALSE;
    }
    header = &base_snapshot.header;
    if (header->flags & ROOT_SURFACE_SHM_STABLE_NON_SOLID)
    {
        struct macdrv_win_data *data = get_win_data(root);

        if (data)
        {
            first_stable_root_backing = !data->chromium_root_surface_presented_once;
            data->chromium_root_surface_presented_once = TRUE;
            release_win_data(data);
        }
    }
    if (!get_root_surface_offset(root, root, &offset, &window))
    {
        release_root_surface_snapshot(&base_snapshot);
        return FALSE;
    }
    if (first_stable_root_backing)
        macdrv_window_remove_all_ca_layer_host_views(window);

    rect = header->rect;
    dirty = header->dirty;
    OffsetRect(&rect, offset.x, offset.y);
    OffsetRect(&dirty, offset.x, offset.y);
    image_bits = base_snapshot.bits;

    overlays = copy_root_surface_overlays(root, &overlay_count);
    if (overlay_count)
    {
        if (!(composed_bits = malloc(header->bits_size)))
        {
            free(overlays);
            release_root_surface_snapshot(&base_snapshot);
            return FALSE;
        }
        memcpy(composed_bits, base_snapshot.bits, header->bits_size);
        for (i = 0; i < overlay_count; i++)
        {
            if (!composite_root_surface_overlay(root, overlays[i], window, header, &rect, composed_bits))
                macdrv_forget_root_surface_overlay(root, overlays[i]);
        }
        image_bits = composed_bits;
        dirty = rect;
    }
    free(overlays);

    data = CFDataCreate(NULL, image_bits, header->bits_size);
    if (!data)
    {
        free(composed_bits);
        release_root_surface_snapshot(&base_snapshot);
        return FALSE;
    }
    provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider)
    {
        free(composed_bits);
        release_root_surface_snapshot(&base_snapshot);
        return FALSE;
    }

    alpha_info = header->alpha_mask ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst;
    colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    image = CGImageCreate(header->width, header->height, 8, 32, header->stride, colorspace,
                          alpha_info | kCGBitmapByteOrder32Little, provider, NULL, retina_on,
                          kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorspace);
    CGDataProviderRelease(provider);
    free(composed_bits);
    release_root_surface_snapshot(&base_snapshot);
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
    if (is_chromium_cef_child_window(root))
        macdrv_window_set_root_surface_image(window, image, image_rect, dirty_rect);
    else
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
        if (macdrv_win_data_uses_chrome_client_only_frame(data))
        {
            release_win_data(data);
            return FALSE;
        }
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
    if (!surface->foreign_child || !surface->window) return;
    macdrv_present_foreign_child_window(surface->header.hwnd);
}

static void merge_root_surface_dirty_bits(struct macdrv_window_surface *surface, const RECT *rect,
                                          const RECT *dirty, const BITMAPINFO *color_info,
                                          const void *color_bits)
{
    RECT update;
    unsigned int height, stride, row, copy_width;
    const BYTE *src = color_bits ? color_bits : surface->bits;
    BYTE *dst = surface->root_surface_backing;

    if (!src || !dst) return;
    if (color_info->bmiHeader.biBitCount != 32 || color_info->bmiHeader.biCompression != BI_RGB)
    {
        memcpy(dst, src, color_info->bmiHeader.biSizeImage);
        return;
    }

    height = abs(color_info->bmiHeader.biHeight);
    if (!height || color_info->bmiHeader.biWidth <= 0) return;
    stride = color_info->bmiHeader.biSizeImage / height;
    if (!stride) return;

    /* window_surface_flush() already reports damage in surface-local coordinates. */
    (void)rect;
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

static BOOL root_surface_dirty_covers_frame(const RECT *dirty, const BITMAPINFO *color_info)
{
    RECT update = *dirty;
    unsigned int width, height;

    if (color_info->bmiHeader.biWidth <= 0 || !color_info->bmiHeader.biHeight) return FALSE;
    width = color_info->bmiHeader.biWidth;
    height = abs(color_info->bmiHeader.biHeight);
    update.left = max(update.left, 0);
    update.top = max(update.top, 0);
    update.right = min(update.right, width);
    update.bottom = min(update.bottom, height);
    if (update.left >= update.right || update.top >= update.bottom) return FALSE;

    return (unsigned int)(update.right - update.left) >= width - width / 10 &&
           (unsigned int)(update.bottom - update.top) >= height - height / 10;
}

static BOOL chrome_custom_frame_has_content(const BITMAPINFO *color_info, const void *pixels)
{
    unsigned int width, height, stride, band_height, first_row, x_step, y_step;
    unsigned int x, y, samples = 0, dark = 0, light = 0, chromatic = 0;

    if (!pixels || color_info->bmiHeader.biBitCount != 32 ||
        color_info->bmiHeader.biCompression != BI_RGB ||
        color_info->bmiHeader.biWidth <= 0 || !color_info->bmiHeader.biHeight)
        return FALSE;

    width = color_info->bmiHeader.biWidth;
    height = abs(color_info->bmiHeader.biHeight);
    stride = color_info->bmiHeader.biSizeImage / height;
    if (stride < width * 4) return FALSE;

    band_height = min(96u, max(48u, height / 16));
    band_height = min(band_height, height);
    first_row = color_info->bmiHeader.biHeight < 0 ? 0 : height - band_height;
    x_step = max(1u, width / 256);
    y_step = max(1u, band_height / 32);

    for (y = first_row; y < first_row + band_height; y += y_step)
    {
        const BYTE *row = (const BYTE *)pixels + y * stride;

        for (x = 0; x < width; x += x_step)
        {
            unsigned int blue = row[x * 4], green = row[x * 4 + 1], red = row[x * 4 + 2];
            unsigned int max_channel = max(red, max(green, blue));
            unsigned int min_channel = min(red, min(green, blue));
            unsigned int luma = (red * 3 + green * 6 + blue) / 10;

            samples++;
            if (luma < 80) dark++;
            if (luma > 180) light++;
            if (max_channel - min_channel > 32) chromatic++;
        }
    }

    if (!samples) return FALSE;
    /* A real tab strip has either a chromatic theme surface or opposing
       foreground/background luminance from text and controls.  Solid light
       and dark transition buffers have neither. */
    return chromatic * 20 >= samples ||
           (dark * 200 >= samples && light * 10 >= samples) ||
           (light * 200 >= samples && dark * 10 >= samples);
}

static BOOL hold_chrome_post_placeholder_client_snapshot(struct macdrv_window_surface *surface,
        const RECT *dirty, const BITMAPINFO *color_info, const void *pixels)
{
    unsigned int width, height, protected_height, dirty_width, dirty_height;
    BOOL held;

    if (!update_root_surface_transition_flag(surface->root_surface_root, surface->header.hwnd,
                                             FALSE, &held) || !held)
        return FALSE;

    if (!pixels || !surface->root_surface_backing || !surface->root_surface_had_non_solid_backing)
        return FALSE;
    if (surface->header.hwnd != surface->root_surface_root ||
        NtUserGetAncestor(surface->header.hwnd, GA_ROOT) != surface->header.hwnd ||
        !is_chrome_widget_window(surface->header.hwnd) ||
        !is_official_chrome_process_window(surface->header.hwnd))
        return FALSE;
    if (color_info->bmiHeader.biBitCount != 32 || color_info->bmiHeader.biCompression != BI_RGB ||
        color_info->bmiHeader.biWidth <= 0 || !color_info->bmiHeader.biHeight)
        return FALSE;
    width = color_info->bmiHeader.biWidth;
    height = abs(color_info->bmiHeader.biHeight);
    protected_height = min(96u, max(48u, height / 16));
    dirty_width = max(0, min(dirty->right, (int)width) - max(dirty->left, 0));
    dirty_height = max(0, min(dirty->bottom, (int)height) - max(dirty->top, 0));

    /* Chrome recreates the root surface with a solid transition frame, then
       can issue a sequence of incomplete custom-frame updates from another
       process-local DIB before sending client content.  Preserve only the
       custom-frame and boundary updates in that exact flagged sequence.  The
       first substantial client-only update consumes the flag and resumes
       normal damage merges. */
    if (dirty_width >= width - width / 10 && dirty_height >= height - height / 10 &&
        chrome_custom_frame_has_content(color_info, pixels))
        return FALSE;
    if (dirty->top >= (int)protected_height &&
        dirty_width >= width / 2 && dirty_height >= height / 4)
        return FALSE;
    update_root_surface_transition_flag(surface->root_surface_root, surface->header.hwnd,
                                        TRUE, NULL);
    TRACE("Switchyard held post-placeholder Chrome custom-frame update dirty %s height %u over authoritative root %p backing\n",
          wine_dbgstr_rect(dirty), protected_height, surface->root_surface_root);
    return TRUE;
}

static BOOL promote_chrome_root_surface_to_relay(struct macdrv_window_surface *surface,
        struct window_surface *window_surface, const BITMAPINFO *color_info)
{
    HWND hwnd = surface->header.hwnd;

    if (surface->root_surface_relay || surface->child || surface->remote_child || surface->foreign_child)
        return FALSE;
    if (NtUserGetAncestor(hwnd, GA_ROOT) != hwnd || !is_chrome_widget_window(hwnd) ||
        !is_official_chrome_process_window(hwnd) ||
        !chromium_hwnd_or_root_uses_root_surface_composition(hwnd))
        return FALSE;
    if (!(surface->root_surface_backing = malloc(color_info->bmiHeader.biSizeImage))) return FALSE;

    memcpy(surface->root_surface_backing, surface->bits, color_info->bmiHeader.biSizeImage);
    surface->root_surface_relay = TRUE;
    surface->root_surface_root = hwnd;
    surface->root_surface_had_non_solid_backing =
        get_nearly_solid_surface_kind(color_info, surface->root_surface_backing) == SOLID_SURFACE_NONE;
    window_surface->flush_on_unlock = TRUE;
    TRACE("Switchyard promoted existing Chrome root surface hwnd %p to authoritative single-backing relay\n",
          hwnd);
    return TRUE;
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
    const void *present_bits;
    macdrv_window root_composed_window = NULL;
    POINT root_composed_offset = {0, 0};
    BOOL root_composed_surface = FALSE;
    BOOL hold_client_only_dark_placeholder = FALSE;
    const void *root_relay_bits = NULL;

    promote_chrome_root_surface_to_relay(surface, window_surface, color_info);
    if (surface->root_surface_relay)
    {
        enum solid_surface_kind incoming_solid_kind = SOLID_SURFACE_NONE;
        const void *incoming_bits = color_bits ? color_bits : surface->bits;

        /* A full-frame Chromium transition placeholder must not be committed
           to the persistent relay backing.  Smaller incremental DIBs may have
           invalid bytes outside their dirty rectangle, so only classify input
           that declares near-full damage. */
        if (surface->header.hwnd == surface->root_surface_root &&
            NtUserGetAncestor(surface->header.hwnd, GA_ROOT) == surface->header.hwnd &&
            root_surface_dirty_covers_client(surface->root_surface_root, dirty, color_info))
        {
            if (!surface->root_surface_had_non_solid_backing)
            {
                struct root_surface_snapshot snapshot = {0};

                if (read_root_surface_snapshot(surface->root_surface_root, surface->header.hwnd, &snapshot))
                {
                    if (snapshot.header.width == color_info->bmiHeader.biWidth &&
                        snapshot.header.height == abs(color_info->bmiHeader.biHeight) &&
                        snapshot.header.bits_size == color_info->bmiHeader.biSizeImage &&
                        get_nearly_solid_surface_kind(color_info, snapshot.bits) == SOLID_SURFACE_NONE)
                        surface->root_surface_had_non_solid_backing = TRUE;
                    release_root_surface_snapshot(&snapshot);
                }
            }
            if (surface->root_surface_had_non_solid_backing)
                incoming_solid_kind = get_nearly_solid_surface_kind(color_info, incoming_bits);
        }
        if (incoming_solid_kind != SOLID_SURFACE_NONE)
        {
            update_root_surface_transition_flag(surface->root_surface_root, surface->header.hwnd,
                                                TRUE, NULL);
            TRACE("Switchyard preserving Chromium/CEF root relay backing over full %s placeholder hwnd %p\n",
                  incoming_solid_kind == SOLID_SURFACE_DARK ? "dark" : "light", surface->header.hwnd);
            send_message_timeout(surface->root_surface_root, WM_MACDRV_PRESENT_ROOT_SURFACE,
                                 (WPARAM)surface->header.hwnd, 0, SMTO_ABORTIFHUNG, 500, NULL);
            return TRUE;
        }

        if (hold_chrome_post_placeholder_client_snapshot(surface, dirty, color_info, incoming_bits))
        {
            send_message_timeout(surface->root_surface_root, WM_MACDRV_PRESENT_ROOT_SURFACE,
                                 (WPARAM)surface->header.hwnd, 0, SMTO_ABORTIFHUNG, 500, NULL);
            return TRUE;
        }

        merge_root_surface_dirty_bits(surface, rect, dirty, color_info, color_bits);
        /* The shared transport is the authoritative backing.  Multiple
           Chrome processes can create a surface for the same root HWND, and
           their process-local preservation buffers do not share lifetime or
           completeness.  Merge only this flush's declared dirty pixels into
           shared storage instead of publishing a stale local copy. */
        root_relay_bits = incoming_bits;
    }
    else if (color_bits && color_bits != surface->bits)
        memcpy(surface->bits, color_bits, color_info->bmiHeader.biSizeImage);

    present_bits = surface->root_surface_relay && surface->root_surface_backing ?
                   surface->root_surface_backing : surface->bits;
    solid_kind = get_nearly_solid_surface_kind(color_info, present_bits);
    if (surface->root_surface_relay && solid_kind == SOLID_SURFACE_NONE)
        surface->root_surface_had_non_solid_backing = TRUE;

    if (surface->root_surface_relay)
    {
        if (solid_kind == SOLID_SURFACE_DARK && surface->header.hwnd == surface->root_surface_root &&
            NtUserGetAncestor(surface->header.hwnd, GA_ROOT) == surface->header.hwnd)
        {
            TRACE("Switchyard suppressing dark Chromium/CEF root relay placeholder hwnd %p\n",
                  surface->header.hwnd);
            send_message_timeout(surface->root_surface_root, WM_MACDRV_PRESENT_ROOT_SURFACE,
                                 (WPARAM)surface->header.hwnd, 0, SMTO_ABORTIFHUNG, 500, NULL);
            return TRUE;
        }
        if (!relay_root_surface_flush(surface, rect, dirty, color_info,
                                      root_relay_bits ? root_relay_bits : present_bits))
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
            if (macdrv_win_data_uses_chrome_client_only_frame(root_data))
            {
                if (solid_kind == SOLID_SURFACE_DARK)
                    hold_client_only_dark_placeholder =
                        root_data->chromium_client_only_had_non_dark_backing;
                else
                    root_data->chromium_client_only_had_non_dark_backing = TRUE;
            }

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

    if (hold_client_only_dark_placeholder)
    {
        TRACE("Switchyard holding Chrome client-only root backing over dark transition placeholder hwnd %p\n",
              surface->header.hwnd);
        return TRUE;
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
    if (surface->root_surface_relay &&
        (surface->root_surface_root != window_surface->hwnd ||
         !NtUserIsWindow(surface->root_surface_root)))
        unlink_root_surface_shm(surface->root_surface_root, window_surface->hwnd);
    free(surface->root_surface_backing);
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
                                             BOOL root_surface_relay, HWND root_surface_root,
                                             void *preserved_root_surface_backing,
                                             BOOL preserved_root_surface_had_non_solid_backing)
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

    if (!(provider = data_provider_create(info->bmiHeader.biSizeImage, &bits)))
    {
        free(preserved_root_surface_backing);
        return NULL;
    }
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
        free(preserved_root_surface_backing);
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
        surface->root_surface_relay = root_surface_relay;
        surface->root_surface_root = root_surface_root;
        surface->root_surface_backing = preserved_root_surface_backing;
        surface->root_surface_had_non_solid_backing =
            preserved_root_surface_had_non_solid_backing;
        surface->chromium_blank_owner_transparent = FALSE;
        surface->chromium_blank_owner_had_remote_layer = FALSE;
        surface->chromium_child_had_real_image = FALSE;
        window_surface->flush_on_unlock = child || remote_child || foreign_child || root_surface_relay;

        if (root_surface_relay)
        {
            struct root_surface_snapshot snapshot = {0};
            BOOL restored_shared_backing = FALSE;

            if (!surface->root_surface_backing &&
                !(surface->root_surface_backing = malloc(info->bmiHeader.biSizeImage)))
            {
                window_surface_release(window_surface);
                return NULL;
            }
            if (!preserved_root_surface_backing && root_surface_root == hwnd &&
                read_root_surface_snapshot(root_surface_root, hwnd, &snapshot))
            {
                unsigned int stride = info->bmiHeader.biSizeImage / abs(info->bmiHeader.biHeight);

                if (snapshot.header.width == info->bmiHeader.biWidth &&
                    snapshot.header.height == abs(info->bmiHeader.biHeight) &&
                    snapshot.header.stride == stride &&
                    snapshot.header.bits_size == info->bmiHeader.biSizeImage)
                {
                    memcpy(surface->root_surface_backing, snapshot.bits, snapshot.header.bits_size);
                    surface->root_surface_had_non_solid_backing =
                        get_nearly_solid_surface_kind(info, snapshot.bits) == SOLID_SURFACE_NONE;
                    restored_shared_backing = TRUE;
                    TRACE("Switchyard restored shared Chromium root backing for recreated relay hwnd %p\n",
                          hwnd);
                }
                release_root_surface_snapshot(&snapshot);
            }
            if (!preserved_root_surface_backing && !restored_shared_backing)
                memcpy(surface->root_surface_backing, surface->bits, info->bmiHeader.biSizeImage);
        }

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
    void *preserved_root_surface_backing = NULL;
    BOOL preserved_root_surface_had_non_solid_backing = FALSE;
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

        /* Chromium recreates an identically sized relay surface after native
           moves and some DComp transitions.  Transfer the one stable backing
           to the replacement instead of seeding the new surface from its
           freshly cleared GDI bits. */
        if (root_surface_relay && mac_surface->root_surface_relay &&
            root_surface_root == mac_surface->root_surface_root &&
            EqualRect(surface_rect, &previous->rect))
        {
            preserved_root_surface_backing = mac_surface->root_surface_backing;
            preserved_root_surface_had_non_solid_backing =
                mac_surface->root_surface_had_non_solid_backing;
            mac_surface->root_surface_backing = NULL;
        }
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
        BOOL existing_foreign_child = data->foreign_child;

        window = existing_foreign_child ? NULL : data->cocoa_window;

        if (layered)
        {
            data->layered = TRUE;
            data->ulw_layered = TRUE;
        }

        release_win_data(data);

        if (existing_foreign_child)
            foreign_child = macdrv_acquire_foreign_child_win_data(hwnd, surface_rect, &window);
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
        macdrv_acquire_foreign_child_win_data(hwnd, surface_rect, &window))
    {
        foreign_child = TRUE;
        child = FALSE;
        TRACE("Switchyard created standalone Chromium/CEF child surface window hwnd %p window %p\n",
              hwnd, window);
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
        if (!(*surface = create_surface(hwnd, window, surface_rect, &offset, child, remote_child,
                                        remote_layer_root, foreign_child,
                                        root_surface_relay, root_surface_root,
                                        preserved_root_surface_backing,
                                        preserved_root_surface_had_non_solid_backing)) && foreign_child)
            macdrv_release_foreign_child_win_data(hwnd);
    }

    if (!root_surface_relay && !root_composed_child && !*surface && remote_child && is_chromium_cef_child_window(hwnd) &&
        macdrv_acquire_foreign_child_win_data(hwnd, surface_rect, &window))
    {
        child = FALSE;
        foreign_child = TRUE;
        TRACE("Switchyard falling back to standalone Chromium/CEF child surface window hwnd %p window %p\n",
              hwnd, window);
        if (!(*surface = create_surface(hwnd, window, surface_rect, &offset, child, FALSE, NULL, foreign_child,
                                        FALSE, NULL, NULL, FALSE)) &&
            foreign_child)
            macdrv_release_foreign_child_win_data(hwnd);
    }

    return TRUE;
}
