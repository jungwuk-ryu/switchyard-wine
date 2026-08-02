/*
 * Ntdll loader tests
 *
 * Copyright 2026 Jungwuk Park
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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windows.h"
#include "winioctl.h"
#include "delayloadhandler.h"
#include "winternl.h"
#include "wine/appx_package_graph.h"
#include "wine/test.h"

#ifdef _WIN64

struct cef_string
{
    WCHAR *str;
    SIZE_T length;
    void (*dtor)(WCHAR *);
};

struct cef_window_info
{
    DWORD ex_style;
    struct cef_string window_name;
    DWORD style;
    int x;
    int y;
    int width;
    int height;
    HANDLE parent_window;
    HANDLE menu;
    int windowless_rendering_enabled;
    int shared_texture_enabled;
    int external_begin_frame_enabled;
    HANDLE window;
};

struct cef_base_ref_counted
{
    SIZE_T size;
    void (WINAPI *add_ref)(struct cef_base_ref_counted *);
    int (WINAPI *release)(struct cef_base_ref_counted *);
    int (WINAPI *has_one_ref)(struct cef_base_ref_counted *);
    int (WINAPI *has_at_least_one_ref)(struct cef_base_ref_counted *);
};

struct cef_render_handler
{
    struct cef_base_ref_counted base;
    void (WINAPI *get_accessibility_handler)(void);
    void (WINAPI *get_root_screen_rect)(void);
    void (WINAPI *get_view_rect)(void);
    void (WINAPI *get_screen_point)(void);
    void (WINAPI *get_screen_info)(void);
    void (WINAPI *on_popup_show)(void);
    void (WINAPI *on_popup_size)(void);
    void (WINAPI *on_paint)(void);
};

struct cef_client
{
    struct cef_base_ref_counted base;
    void *handlers_before_render[13];
    struct cef_render_handler *(WINAPI *get_render_handler)(
        struct cef_client *);
};

typedef int (__cdecl *cef_initialize_func)(const void *, const void *,
        void *, void *);
typedef void *(__cdecl *cef_create_browser_sync_func)(
        const struct cef_window_info *, struct cef_client *, const void *,
        const void *, void *, void *);
typedef unsigned int (__cdecl *cef_get_last_call_func)(void *, unsigned int,
        void **);
typedef void (__cdecl *cef_set_hash_func)(int);
typedef void *(WINAPI *rtl_find_exported_routine_by_name_func)(
        HMODULE, const char *);

C_ASSERT(FIELD_OFFSET(struct cef_window_info, parent_window) == 56);
C_ASSERT(FIELD_OFFSET(struct cef_window_info, windowless_rendering_enabled) == 72);
C_ASSERT(FIELD_OFFSET(struct cef_window_info, window) == 88);
C_ASSERT(sizeof(struct cef_window_info) == 96);
C_ASSERT(sizeof(struct cef_base_ref_counted) == 40);
C_ASSERT(FIELD_OFFSET(struct cef_client, get_render_handler) == 144);
C_ASSERT(sizeof(struct cef_client) == 152);
C_ASSERT(FIELD_OFFSET(struct cef_render_handler, get_view_rect) == 56);
C_ASSERT(FIELD_OFFSET(struct cef_render_handler, on_paint) == 96);
C_ASSERT(sizeof(struct cef_render_handler) == 104);

static const WCHAR disable_cef_osr_fallback[] =
    L"SWITCHYARD_DISABLE_CEF_OSR_FALLBACK";
static struct cef_render_handler render_handler;
static struct cef_client client;
static unsigned int render_handler_get_count;
static unsigned int render_handler_release_count;
static BOOL return_render_handler;

static void cef_string_dtor(WCHAR *str)
{
    (void)str;
}

static void WINAPI render_callback_stub(void)
{
}

static int WINAPI release_render_handler(struct cef_base_ref_counted *base)
{
    ok(base == &render_handler.base, "Got release base %p, expected %p.\n",
            base, &render_handler.base);
    ++render_handler_release_count;
    return 0;
}

static struct cef_render_handler *WINAPI get_render_handler(
        struct cef_client *self)
{
    ok(self == &client, "Got client %p, expected %p.\n", self, &client);
    ++render_handler_get_count;
    return return_render_handler ? &render_handler : NULL;
}

static void init_client(void)
{
    memset(&client, 0, sizeof(client));
    memset(&render_handler, 0, sizeof(render_handler));

    client.base.size = sizeof(client);
    client.get_render_handler = get_render_handler;

    render_handler.base.size = sizeof(render_handler);
    render_handler.base.release = release_render_handler;
    render_handler.get_view_rect = render_callback_stub;
    render_handler.on_paint = render_callback_stub;
    return_render_handler = TRUE;
}

static void init_window_info(struct cef_window_info *info)
{
    static WCHAR window_name[] = L"winetest CEF child";

    memset(info, 0, sizeof(*info));
    info->ex_style = WS_EX_NOPARENTNOTIFY;
    info->window_name.str = window_name;
    info->window_name.length = ARRAY_SIZE(window_name) - 1;
    info->window_name.dtor = cef_string_dtor;
    info->style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    info->x = 11;
    info->y = 22;
    info->width = 1280;
    info->height = 720;
    info->parent_window = (HANDLE)(ULONG_PTR)0x12345678;
    info->menu = (HANDLE)(ULONG_PTR)0x23456789;
}

static void check_last_call(cef_get_last_call_func get_last_call,
        unsigned int expected_count, const struct cef_window_info *expected_info,
        void **expected_arguments)
{
    struct cef_window_info actual_info;
    void *actual_arguments[5];
    unsigned int count, i;

    memset(&actual_info, 0xcc, sizeof(actual_info));
    memset(actual_arguments, 0xcc, sizeof(actual_arguments));
    count = get_last_call(&actual_info, sizeof(actual_info), actual_arguments);
    ok(count == expected_count, "Got call count %u, expected %u.\n",
            count, expected_count);
    ok(!memcmp(&actual_info, expected_info, sizeof(actual_info)),
            "The CEF window information was changed unexpectedly.\n");
    for (i = 0; i < ARRAY_SIZE(actual_arguments); ++i)
        ok(actual_arguments[i] == expected_arguments[i],
                "Argument %u is %p, expected %p.\n", i + 2,
                actual_arguments[i], expected_arguments[i]);
}

static void call_fake_cef(cef_create_browser_sync_func create_browser,
        cef_get_last_call_func get_last_call, unsigned int call_count,
        const struct cef_window_info *input, BOOL expect_windowless,
        unsigned int expected_get_count, unsigned int expected_release_count)
{
    struct cef_window_info original = *input;
    struct cef_window_info expected = *input;
    void *arguments[] =
    {
        (void *)(ULONG_PTR)0x456789ab,
        (void *)(ULONG_PTR)0x56789abc,
        (void *)(ULONG_PTR)0x6789abcd,
        (void *)(ULONG_PTR)0x789abcde
    };
    void *expected_arguments[] =
    {
        &client, arguments[0], arguments[1], arguments[2], arguments[3]
    };
    void *ret;

    render_handler_get_count = 0;
    render_handler_release_count = 0;
    if (expect_windowless)
    {
        expected.parent_window = NULL;
        expected.windowless_rendering_enabled = 1;
    }
    ret = create_browser(input, &client, arguments[0], arguments[1],
            arguments[2], arguments[3]);
    ok(ret == &client, "Got return value %p, expected %p.\n", ret, &client);
    ok(!memcmp(input, &original, sizeof(original)),
            "The caller's CEF window information was modified.\n");
    ok(render_handler_get_count == expected_get_count,
            "Got %u render-handler queries, expected %u.\n",
            render_handler_get_count, expected_get_count);
    ok(render_handler_release_count == expected_release_count,
            "Got %u render-handler releases, expected %u.\n",
            render_handler_release_count, expected_release_count);
    check_last_call(get_last_call, call_count, &expected, expected_arguments);
}

static BOOL extract_libcef(const WCHAR *path)
{
    DWORD size, written;
    HGLOBAL loaded;
    HANDLE file;
    HRSRC resource;
    void *data;
    BOOL ret;

    resource = FindResourceW(NULL, L"libcef.dll", L"TESTDLL");
    ok(!!resource, "The fake libcef.dll resource is missing.\n");
    if (!resource) return FALSE;
    loaded = LoadResource(GetModuleHandleW(NULL), resource);
    data = LockResource(loaded);
    size = SizeofResource(GetModuleHandleW(NULL), resource);
    ok(!!data && !!size, "Could not load the fake libcef.dll resource.\n");
    if (!data || !size) return FALSE;

    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
    ok(file != INVALID_HANDLE_VALUE,
            "CreateFileW(%s) failed, error %lu.\n",
            wine_dbgstr_w(path), GetLastError());
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    ret = WriteFile(file, data, size, &written, NULL);
    ok(ret && written == size,
            "WriteFile wrote %lu of %lu bytes, error %lu.\n",
            written, size, GetLastError());
    CloseHandle(file);
    return ret && written == size;
}

static void test_unmatched_cef_module(const WCHAR *path)
{
    cef_create_browser_sync_func create_browser;
    cef_get_last_call_func get_last_call;
    struct cef_window_info info;
    HMODULE module;

    module = LoadLibraryW(path);
    ok(!!module, "LoadLibraryW(%s) failed, error %lu.\n",
            wine_dbgstr_w(path), GetLastError());
    if (!module) return;

    create_browser = (void *)GetProcAddress(module,
            "cef_browser_host_create_browser_sync");
    get_last_call = (void *)GetProcAddress(module, "cef_test_get_last_call");
    ok(!!create_browser && !!get_last_call,
            "The alternate fake CEF exports are incomplete.\n");
    if (create_browser && get_last_call)
    {
        init_client();
        init_window_info(&info);
        call_fake_cef(create_browser, get_last_call, 1, &info, FALSE, 0, 0);
    }
    FreeLibrary(module);
}

static void test_cef_osr_fallback(const WCHAR *path)
{
    cef_create_browser_sync_func create_browser, create_browser_again;
    cef_create_browser_sync_func unsupported_create_browser;
    cef_get_last_call_func get_last_call;
    cef_initialize_func initialize, initialize_again;
    cef_initialize_func unsupported_initialize;
    cef_set_hash_func set_hash;
    rtl_find_exported_routine_by_name_func rtl_find_export;
    struct cef_window_info info;
    HMODULE module;
    unsigned int call_count = 0;

    rtl_find_export = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
            "RtlFindExportedRoutineByName");
    ok(!!rtl_find_export,
            "RtlFindExportedRoutineByName is unavailable.\n");
    if (!rtl_find_export) return;

    module = LoadLibraryW(path);
    ok(!!module, "LoadLibraryW(%s) failed, error %lu.\n",
            wine_dbgstr_w(path), GetLastError());
    if (!module) return;

    get_last_call = (void *)GetProcAddress(module, "cef_test_get_last_call");
    set_hash = (void *)GetProcAddress(module, "cef_test_set_hash");
    ok(!!get_last_call && !!set_hash,
            "The fake CEF test exports are incomplete.\n");
    if (!get_last_call || !set_hash)
    {
        FreeLibrary(module);
        return;
    }

    set_hash(FALSE);
    unsupported_initialize = (void *)GetProcAddress(module, "cef_initialize");
    unsupported_create_browser = (void *)GetProcAddress(module,
            "cef_browser_host_create_browser_sync");
    ok(!!unsupported_initialize && !!unsupported_create_browser,
            "The unsupported fake CEF exports are incomplete.\n");
    if (!unsupported_initialize || !unsupported_create_browser)
    {
        FreeLibrary(module);
        return;
    }
    ok(rtl_find_export(module, "cef_initialize") == unsupported_initialize,
            "The unsupported CEF initialize export was wrapped on the "
            "delay-load path.\n");
    ok(rtl_find_export(module, "cef_browser_host_create_browser_sync") ==
            unsupported_create_browser,
            "The unsupported CEF browser export was wrapped on the "
            "delay-load path.\n");
    init_client();
    init_window_info(&info);
    call_fake_cef(unsupported_create_browser, get_last_call, 1, &info, FALSE,
            0, 0);
    call_count = 1;

    set_hash(TRUE);
    initialize = rtl_find_export(module, "cef_initialize");
    initialize_again = (void *)GetProcAddress(module, "cef_initialize");
    create_browser = rtl_find_export(module,
            "cef_browser_host_create_browser_sync");
    create_browser_again = (void *)GetProcAddress(module,
            "cef_browser_host_create_browser_sync");
    ok(!!initialize, "The fake CEF initialize entry point is missing.\n");
    ok(initialize == unsupported_initialize,
            "The supported CEF initialize export was unexpectedly wrapped.\n");
    ok(initialize_again == unsupported_initialize,
            "The dynamic CEF initialize lookup returned %p, expected raw "
            "export %p.\n", initialize_again, unsupported_initialize);
    ok(!!create_browser, "The fake CEF browser entry point is missing.\n");
    ok(create_browser != unsupported_create_browser,
            "The unsupported CEF browser export was wrapped.\n");
    ok(create_browser_again == create_browser,
            "Repeated CEF browser lookup returned %p, expected %p.\n",
            create_browser_again, create_browser);
    if (!initialize || !create_browser)
    {
        FreeLibrary(module);
        return;
    }

    call_fake_cef(create_browser, get_last_call, ++call_count, &info, TRUE,
            1, 1);

    return_render_handler = FALSE;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            1, 0);
    return_render_handler = TRUE;

    client.base.size = FIELD_OFFSET(struct cef_client, get_render_handler);
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);
    client.base.size = sizeof(client);

    render_handler.base.size =
        FIELD_OFFSET(struct cef_render_handler, on_paint);
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            1, 1);
    render_handler.base.size = sizeof(render_handler);

    render_handler.on_paint = NULL;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            1, 1);
    render_handler.on_paint = render_callback_stub;

    render_handler.get_view_rect = NULL;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            1, 1);
    render_handler.get_view_rect = render_callback_stub;

    SetEnvironmentVariableW(disable_cef_osr_fallback, L"1");
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);
    SetEnvironmentVariableW(disable_cef_osr_fallback, NULL);

    info.windowless_rendering_enabled = 1;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);

    init_window_info(&info);
    info.parent_window = NULL;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);

    init_window_info(&info);
    info.style &= ~WS_CHILD;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);

    init_window_info(&info);
    info.shared_texture_enabled = 1;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);

    init_window_info(&info);
    info.external_begin_frame_enabled = 1;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);

    init_window_info(&info);
    info.window = (HANDLE)(ULONG_PTR)0x3456789a;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);

    init_window_info(&info);
    info.width = 1;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);

    init_window_info(&info);
    info.height = 1;
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);

    init_window_info(&info);
    set_hash(FALSE);
    call_fake_cef(create_browser, get_last_call, ++call_count, &info, FALSE,
            0, 0);
    set_hash(TRUE);

    FreeLibrary(module);
}

static void test_cef_loader_hooks(void)
{
    WCHAR temp_path[MAX_PATH], directory[MAX_PATH];
    WCHAR libcef_path[MAX_PATH], alternate_path[MAX_PATH];
    WCHAR *old_disable_value = NULL;
    DWORD len;
    BOOL had_disable_value;

    if (!winetest_platform_is_wine)
    {
        win_skip("The CEF compatibility hook is specific to Switchyard Wine.\n");
        return;
    }

    SetLastError(ERROR_SUCCESS);
    len = GetEnvironmentVariableW(disable_cef_osr_fallback, NULL, 0);
    had_disable_value = !!len;
    if (len)
    {
        old_disable_value = HeapAlloc(GetProcessHeap(), 0,
                len * sizeof(WCHAR));
        ok(!!old_disable_value,
                "Could not save the opt-out environment value.\n");
        if (!old_disable_value) return;
        GetEnvironmentVariableW(disable_cef_osr_fallback, old_disable_value,
                len);
    }
    else
        ok(GetLastError() == ERROR_ENVVAR_NOT_FOUND,
                "GetEnvironmentVariableW failed, error %lu.\n",
                GetLastError());
    SetEnvironmentVariableW(disable_cef_osr_fallback, NULL);

    len = GetTempPathW(ARRAY_SIZE(temp_path), temp_path);
    ok(len && len < ARRAY_SIZE(temp_path),
            "GetTempPathW failed, error %lu.\n", GetLastError());
    if (!len || len >= ARRAY_SIZE(temp_path)) goto restore_environment;
    if (!GetTempFileNameW(temp_path, L"cef", 0, directory))
    {
        ok(0, "GetTempFileNameW failed, error %lu.\n", GetLastError());
        goto restore_environment;
    }
    DeleteFileW(directory);
    if (!CreateDirectoryW(directory, NULL))
    {
        ok(0, "CreateDirectoryW(%s) failed, error %lu.\n",
                wine_dbgstr_w(directory), GetLastError());
        goto restore_environment;
    }

    lstrcpyW(alternate_path, directory);
    lstrcatW(alternate_path, L"\\cefalternate.dll");
    lstrcpyW(libcef_path, directory);
    lstrcatW(libcef_path, L"\\libcef.dll");
    if (!extract_libcef(alternate_path)) goto cleanup_files;
    test_unmatched_cef_module(alternate_path);
    if (!extract_libcef(libcef_path)) goto cleanup_files;
    test_cef_osr_fallback(libcef_path);

cleanup_files:
    DeleteFileW(libcef_path);
    DeleteFileW(alternate_path);
    RemoveDirectoryW(directory);
restore_environment:
    SetEnvironmentVariableW(disable_cef_osr_fallback,
            had_disable_value ? old_disable_value : NULL);
    if (old_disable_value)
        HeapFree(GetProcessHeap(), 0, old_disable_value);
}

#endif /* _WIN64 */

struct package_loader_file
{
    const WCHAR *basename;
    WCHAR relative[MAX_PATH];
    WCHAR path[MAX_PATH];
    DWORD volume_serial;
    DWORD file_index_high;
    DWORD file_index_low;
    LONGLONG change_time;
    ULONGLONG file_size;
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
};

struct package_loader_rewrite_info
{
    LONGLONG before_change_time;
    LONGLONG after_change_time;
    ULONGLONG before_size;
    ULONGLONG after_size;
    BOOL write_time_restored;
};

enum package_loader_index
{
    PACKAGE_LOADER_KERNEL32,
    PACKAGE_LOADER_OPENGL32,
    PACKAGE_LOADER_IMPORTER,
    PACKAGE_LOADER_BLOCKED,
    PACKAGE_LOADER_DELAY,
    PACKAGE_LOADER_EXPLICIT,
    PACKAGE_LOADER_FORWARD,
    PACKAGE_LOADER_FORWARD_TARGET,
    PACKAGE_LOADER_INIT,
    PACKAGE_LOADER_LOADED,
    PACKAGE_LOADER_MISSING,
    PACKAGE_LOADER_ORDER_A,
    PACKAGE_LOADER_ORDER_Z,
    PACKAGE_LOADER_REPARSE,
    PACKAGE_LOADER_SUBSTITUTE,
    PACKAGE_LOADER_COUNT,
};

struct package_loader_fixture
{
    WCHAR root[MAX_PATH];
    WCHAR bin[MAX_PATH];
    WCHAR ambient[MAX_PATH];
    WCHAR mesa_root[MAX_PATH];
    WCHAR mesa_arch[MAX_PATH];
    WCHAR mesa_opengl[MAX_PATH];
    WCHAR lease_path[MAX_PATH];
    WCHAR importer[MAX_PATH];
    WCHAR hardlink_alias[MAX_PATH];
    WCHAR unregistered[MAX_PATH];
    WCHAR outside_root[MAX_PATH];
    WCHAR outside_module[MAX_PATH];
    WCHAR missing_saved[MAX_PATH];
    WCHAR reparse_target[MAX_PATH];
    WCHAR substitute_source[MAX_PATH];
    HANDLE lease;
    BYTE *graph;
    unsigned int graph_size;
    BOOL hardlink_ready;
    DWORD hardlink_error;
    struct package_loader_file files[PACKAGE_LOADER_COUNT];
};

static const WCHAR * const package_loader_names[] =
{
    L"kernel32.dll",
    L"opengl32.dll",
    L"package_importer.dll",
    L"pkgblocked.dll",
    L"pkgdelay.dll",
    L"pkgexplicit.dll",
    L"pkgforward.dll",
    L"pkgforwardtarget.dll",
    L"pkginit.dll",
    L"pkgloaded.dll",
    L"pkgmissing.dll",
    L"pkgordera.dll",
    L"pkgorderz.dll",
    L"pkgreparse.dll",
    L"pkgsubstitute.dll",
};

C_ASSERT( ARRAY_SIZE(package_loader_names) == PACKAGE_LOADER_COUNT );
static const WCHAR package_loader_full_name[] =
    L"Wine.Loader_1.0.0.0_neutral__pub";
static const BYTE package_loader_content_id[32] =
{
    0x9e, 0x6b, 0x2c, 0x7f, 0x41, 0x05, 0xb8, 0xd3,
    0x7a, 0x11, 0xe4, 0x92, 0xc6, 0x5d, 0x38, 0xaf,
    0x52, 0x94, 0x0b, 0xed, 0x73, 0x26, 0xa1, 0x4c,
    0xf8, 0x60, 0x35, 0x9b, 0x17, 0xda, 0x84, 0x2e,
};
C_ASSERT( offsetof(struct wine_appx_graph_attach, blob) == 16 );
C_ASSERT( offsetof(struct wine_appx_graph_attach, leases) == 24 );
C_ASSERT( offsetof(struct wine_appx_graph_attach, lease_count) == 32 );
C_ASSERT( sizeof(struct wine_appx_graph_attach) == 40 );

static void package_graph_write_u16( BYTE *data, unsigned int value )
{
    data[0] = value;
    data[1] = value >> 8;
}

static void package_graph_write_u32( BYTE *data, unsigned int value )
{
    package_graph_write_u16( data, value );
    package_graph_write_u16( data + 2, value >> 16 );
}

static void package_graph_write_u64( BYTE *data, ULONGLONG value )
{
    package_graph_write_u32( data, value );
    package_graph_write_u32( data + 4, value >> 32 );
}

static BOOL append_path_component( WCHAR *path, unsigned int count,
                                   const WCHAR *component )
{
    unsigned int length = lstrlenW( path );
    unsigned int component_length = lstrlenW( component );

    if (length && path[length - 1] != '\\')
    {
        if (length + 1 >= count) return FALSE;
        path[length++] = '\\';
        path[length] = 0;
    }
    if (component_length >= count - length) return FALSE;
    memcpy( path + length, component,
            (component_length + 1) * sizeof(*path) );
    return TRUE;
}

static BOOL get_file_identity( const WCHAR *path, DWORD *volume_serial,
                               DWORD *file_index_high, DWORD *file_index_low,
                               BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE] )
{
    BY_HANDLE_FILE_INFORMATION info;
    FILE_OBJECTID_BUFFER native_id;
    IO_STATUS_BLOCK io;
    HANDLE file;
    BOOL ret;

    file = CreateFileW( path, FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                        FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ret = GetFileInformationByHandle( file, &info );
    if (ret && object_id)
    {
        ret = !NtFsControlFile(
            file, NULL, NULL, NULL, &io, FSCTL_GET_OBJECT_ID,
            NULL, 0, &native_id, sizeof(native_id) );
        if (ret)
            memcpy( object_id, native_id.ObjectId,
                    WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    }
    CloseHandle( file );
    if (!ret || (!info.nFileIndexHigh && !info.nFileIndexLow))
        return FALSE;
    *volume_serial = info.dwVolumeSerialNumber;
    *file_index_high = info.nFileIndexHigh;
    *file_index_low = info.nFileIndexLow;
    return TRUE;
}

static BOOL get_file_stamp( const WCHAR *path, LONGLONG *change_time,
                            ULONGLONG *file_size )
{
    FILE_STANDARD_INFORMATION standard;
    FILE_BASIC_INFORMATION basic;
    IO_STATUS_BLOCK io;
    HANDLE file;
    NTSTATUS status;

    file = CreateFileW(
        path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    status = NtQueryInformationFile(
        file, &io, &basic, sizeof(basic), FileBasicInformation );
    if (!status)
        status = NtQueryInformationFile(
            file, &io, &standard, sizeof(standard),
            FileStandardInformation );
    CloseHandle( file );
    if (status || basic.ChangeTime.QuadPart <= 0 ||
        standard.EndOfFile.QuadPart <= 0)
        return FALSE;
    *change_time = basic.ChangeTime.QuadPart;
    *file_size = standard.EndOfFile.QuadPart;
    return TRUE;
}

static BOOL rewrite_file_preserving_size_and_write_time(
    const WCHAR *path, struct package_loader_rewrite_info *info )
{
    FILE_STANDARD_INFORMATION before_standard, after_standard;
    FILE_BASIC_INFORMATION before_basic, after_basic;
    FILETIME original_write_time;
    LARGE_INTEGER position;
    IO_STATUS_BLOCK io;
    BYTE original, replacement;
    DWORD transferred, error = ERROR_SUCCESS;
    HANDLE file;
    NTSTATUS status;
    BOOL restore_needed = FALSE, ret = FALSE;

    memset( info, 0, sizeof(*info) );
    file = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    status = NtQueryInformationFile(
        file, &io, &before_basic, sizeof(before_basic),
        FileBasicInformation );
    if (!status)
        status = NtQueryInformationFile(
            file, &io, &before_standard, sizeof(before_standard),
            FileStandardInformation );
    if (status)
    {
        error = RtlNtStatusToDosError( status );
        goto done;
    }
    if (before_standard.Directory ||
        before_standard.EndOfFile.QuadPart <= 0)
    {
        error = ERROR_INVALID_DATA;
        goto done;
    }

    position.QuadPart = before_standard.EndOfFile.QuadPart / 2;
    if (!SetFilePointerEx( file, position, NULL, FILE_BEGIN ) ||
        !ReadFile( file, &original, sizeof(original), &transferred, NULL ) ||
        transferred != sizeof(original))
    {
        error = GetLastError();
        goto done;
    }
    replacement = original ^ 0x5a;
    if (!SetFilePointerEx( file, position, NULL, FILE_BEGIN ) ||
        !WriteFile(
            file, &replacement, sizeof(replacement), &transferred, NULL ) ||
        transferred != sizeof(replacement))
    {
        error = GetLastError();
        goto done;
    }
    restore_needed = TRUE;
    if (!FlushFileBuffers( file ))
    {
        error = GetLastError();
        goto done;
    }
    if (!SetFilePointerEx( file, position, NULL, FILE_BEGIN ) ||
        !WriteFile(
            file, &original, sizeof(original), &transferred, NULL ) ||
        transferred != sizeof(original) ||
        !FlushFileBuffers( file ))
    {
        error = GetLastError();
        goto done;
    }
    restore_needed = FALSE;
    original_write_time.dwLowDateTime =
        before_basic.LastWriteTime.LowPart;
    original_write_time.dwHighDateTime =
        before_basic.LastWriteTime.HighPart;
    if (!SetFileTime( file, NULL, NULL, &original_write_time ))
    {
        error = GetLastError();
        goto done;
    }

    status = NtQueryInformationFile(
        file, &io, &after_basic, sizeof(after_basic),
        FileBasicInformation );
    if (!status)
        status = NtQueryInformationFile(
            file, &io, &after_standard, sizeof(after_standard),
            FileStandardInformation );
    if (status)
    {
        error = RtlNtStatusToDosError( status );
        goto done;
    }
    info->before_change_time = before_basic.ChangeTime.QuadPart;
    info->after_change_time = after_basic.ChangeTime.QuadPart;
    info->before_size = before_standard.EndOfFile.QuadPart;
    info->after_size = after_standard.EndOfFile.QuadPart;
    info->write_time_restored =
        before_basic.LastWriteTime.QuadPart ==
        after_basic.LastWriteTime.QuadPart;
    ret = TRUE;

done:
    if (restore_needed)
    {
        SetFilePointerEx( file, position, NULL, FILE_BEGIN );
        WriteFile( file, &original, sizeof(original), &transferred, NULL );
        FlushFileBuffers( file );
    }
    CloseHandle( file );
    if (!ret) SetLastError( error ? error : ERROR_WRITE_FAULT );
    return ret;
}

static BOOL truncate_file_in_place( const WCHAR *path, ULONGLONG size )
{
    LARGE_INTEGER position;
    DWORD error;
    HANDLE file;
    BOOL ret;

    if (!size || size >> 63)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    file = CreateFileW(
        path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    position.QuadPart = size;
    ret = SetFilePointerEx( file, position, NULL, FILE_BEGIN ) &&
          SetEndOfFile( file ) && FlushFileBuffers( file );
    error = GetLastError();
    CloseHandle( file );
    if (!ret) SetLastError( error );
    return ret;
}

static BOOL write_package_generation_marker( HANDLE marker )
{
    BYTE header[40] = {'S','W','L','M',1};
    DWORD written;

    memcpy( header + 8, package_loader_content_id,
            sizeof(package_loader_content_id) );
    if (!WriteFile( marker, header, sizeof(header), &written, NULL ) ||
        written != sizeof(header) ||
        !WriteFile( marker, package_loader_full_name,
                    sizeof(package_loader_full_name), &written, NULL ) ||
        written != sizeof(package_loader_full_name) ||
        !FlushFileBuffers( marker ))
        return FALSE;
    return TRUE;
}

static BOOL extract_package_test_dll( const WCHAR *resource_name,
                                      const WCHAR *path,
                                      const char *replace_from,
                                      const char *replace_to )
{
    DWORD size, written, replacement_count = 0, i;
    HMODULE module = GetModuleHandleW( NULL );
    BYTE *copy = NULL;
    const BYTE *data;
    HGLOBAL loaded;
    HANDLE file;
    HRSRC resource;
    BOOL ret;

    resource = FindResourceW( module, resource_name, L"TESTDLL" );
    ok( !!resource, "The %s resource is missing.\n",
        wine_dbgstr_w(resource_name) );
    if (!resource) return FALSE;
    loaded = LoadResource( module, resource );
    data = LockResource( loaded );
    size = SizeofResource( module, resource );
    ok( !!data && !!size, "Could not load %s.\n",
        wine_dbgstr_w(resource_name) );
    if (!data || !size) return FALSE;

    if (replace_from)
    {
        unsigned int length = strlen( replace_from );

        ok( replace_to && strlen( replace_to ) == length,
            "Replacement names must have equal lengths.\n" );
        if (!replace_to || strlen( replace_to ) != length) return FALSE;
        copy = HeapAlloc( GetProcessHeap(), 0, size );
        ok( !!copy, "Could not allocate %lu bytes for %s.\n",
            size, wine_dbgstr_w(resource_name) );
        if (!copy) return FALSE;
        memcpy( copy, data, size );
        for (i = 0; i + length < size; i++)
        {
            unsigned int j;

            for (j = 0; j < length; j++)
            {
                char left = copy[i + j];
                char right = replace_from[j];

                if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
                if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
                if (left != right) break;
            }
            if (j == length && !copy[i + length])
            {
                memcpy( copy + i, replace_to, length );
                replacement_count++;
                i += length;
            }
        }
        ok( replacement_count == 1,
            "Replaced %lu import names in %s, expected one.\n",
            replacement_count, wine_dbgstr_w(resource_name) );
        if (replacement_count != 1)
        {
            HeapFree( GetProcessHeap(), 0, copy );
            return FALSE;
        }
        data = copy;
    }

    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE,
        "CreateFileW(%s) failed, error %lu.\n",
        wine_dbgstr_w(path), GetLastError() );
    if (file == INVALID_HANDLE_VALUE)
    {
        HeapFree( GetProcessHeap(), 0, copy );
        return FALSE;
    }
    ret = WriteFile( file, data, size, &written, NULL );
    ok( ret && written == size,
        "WriteFile(%s) wrote %lu of %lu bytes, error %lu.\n",
        wine_dbgstr_w(path), written, size, GetLastError() );
    if (ret && written == size)
        ret = FlushFileBuffers( file );
    CloseHandle( file );
    HeapFree( GetProcessHeap(), 0, copy );
    return ret && written == size;
}

static BOOL package_graph_append_string( BYTE *graph, unsigned int capacity,
                                         unsigned int *position,
                                         unsigned int ref_offset,
                                         const WCHAR *string )
{
    unsigned int chars = lstrlenW( string ) + 1, i;

    if (*position > capacity ||
        chars > WINE_APPX_GRAPH_MAX_STRING_CHARS + 1 ||
        chars > (capacity - *position) / sizeof(WCHAR))
        return FALSE;
    package_graph_write_u32( graph + ref_offset, *position );
    package_graph_write_u32( graph + ref_offset + 4, chars );
    for (i = 0; i < chars; i++)
        package_graph_write_u16(
            graph + *position + i * sizeof(WCHAR), string[i] );
    *position += chars * sizeof(WCHAR);
    return TRUE;
}

static BYTE *build_package_loader_graph( struct package_loader_fixture *fixture,
                                         unsigned int *graph_size )
{
    static const unsigned int capacity = 64 * 1024;
    const unsigned int package_offset =
        WINE_APPX_GRAPH_BLOB_HEADER_SIZE;
    const unsigned int loaders_offset = package_offset +
        WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    const unsigned int classes_offset = loaders_offset +
        ARRAY_SIZE(package_loader_names) *
        WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
    const unsigned int strings_offset = classes_offset;
    WCHAR module[MAX_PATH];
    DWORD application_volume, application_index_high, application_index_low;
    BYTE application_object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
    unsigned int position = strings_offset, i;
    BYTE *graph;

    if (!GetModuleFileNameW( NULL, module, ARRAY_SIZE(module) ) ||
        !get_file_identity( module, &application_volume,
                            &application_index_high,
                            &application_index_low,
                            application_object_id ))
        return NULL;
    graph = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, capacity );
    if (!graph) return NULL;

    memcpy( graph, "SWXGRAPH", 8 );
    package_graph_write_u32( graph + 8, WINE_APPX_GRAPH_BLOB_VERSION );
    package_graph_write_u32(
        graph + 12, WINE_APPX_GRAPH_BLOB_HEADER_SIZE );
    package_graph_write_u32(
        graph + 40, WINE_APPX_GRAPH_CURRENT_ARCHITECTURE );
    package_graph_write_u32( graph + 44, 1 );
    package_graph_write_u32( graph + 48, package_offset );
    package_graph_write_u32(
        graph + 52, ARRAY_SIZE(package_loader_names) );
    package_graph_write_u32( graph + 56, loaders_offset );
    package_graph_write_u32( graph + 60, strings_offset );
    package_graph_write_u32( graph + 68, 1 ); /* full trust */
    package_graph_write_u32(
        graph + WINE_APPX_GRAPH_HEADER_CLASS_COUNT_OFFSET, 0 );
    package_graph_write_u32(
        graph + WINE_APPX_GRAPH_HEADER_CLASSES_OFFSET, classes_offset );
    package_graph_write_u32(
        graph + WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET,
        application_volume );
    package_graph_write_u32(
        graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET,
        application_index_high );
    package_graph_write_u32(
        graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET,
        application_index_low );
    memcpy( graph + WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET,
            application_object_id, sizeof(application_object_id) );

    package_graph_write_u32( graph + package_offset + 8, 0 );
    package_graph_write_u32(
        graph + package_offset + 12,
        WINE_APPX_GRAPH_PACKAGE_ACTIVE |
        WINE_APPX_GRAPH_PACKAGE_SIGNED );
    package_graph_write_u32( graph + package_offset + 16, 0 );
    memcpy( graph + package_offset + 24, package_loader_content_id,
            sizeof(package_loader_content_id) );

    for (i = 0; i < ARRAY_SIZE(package_loader_names); i++)
    {
        BYTE *record = graph + loaders_offset +
                       i * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;

        package_graph_write_u32( record, 0 );
        package_graph_write_u32(
            record + WINE_APPX_GRAPH_LOADER_VOLUME_SERIAL_OFFSET,
            fixture->files[i].volume_serial );
        package_graph_write_u32(
            record + WINE_APPX_GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET,
            fixture->files[i].file_index_high );
        package_graph_write_u32(
            record + WINE_APPX_GRAPH_LOADER_FILE_INDEX_LOW_OFFSET,
            fixture->files[i].file_index_low );
        package_graph_write_u64(
            record + WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET,
            fixture->files[i].change_time );
        package_graph_write_u64(
            record + WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET,
            fixture->files[i].file_size );
        memcpy( record + WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET,
                fixture->files[i].object_id,
                sizeof(fixture->files[i].object_id) );
    }

    if (!package_graph_append_string(
            graph, capacity, &position, 72, L"LoaderTest" ) ||
        !package_graph_append_string(
            graph, capacity, &position, 80,
            L"Wine.Loader_pub!App" ) ||
        !package_graph_append_string(
            graph, capacity, &position, 88, L"loader.exe" ) ||
        !package_graph_append_string(
            graph, capacity, &position, 96, L"" ) ||
        !package_graph_append_string(
            graph, capacity, &position, package_offset + 56,
            L"Wine.Loader" ) ||
        !package_graph_append_string(
            graph, capacity, &position, package_offset + 64,
            L"CN=Test" ) ||
        !package_graph_append_string(
            graph, capacity, &position, package_offset + 72, L"" ) ||
        !package_graph_append_string(
            graph, capacity, &position, package_offset + 80, L"pub" ) ||
        !package_graph_append_string(
            graph, capacity, &position, package_offset + 88,
            package_loader_full_name ) ||
        !package_graph_append_string(
            graph, capacity, &position, package_offset + 96,
            L"Wine.Loader_pub" ) ||
        !package_graph_append_string(
            graph, capacity, &position, package_offset + 104,
            fixture->root ))
        goto invalid;

    for (i = 0; i < ARRAY_SIZE(package_loader_names); i++)
    {
        unsigned int record = loaders_offset +
            i * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;

        if (!package_graph_append_string(
                graph, capacity, &position, record + 8,
                fixture->files[i].basename ) ||
            !package_graph_append_string(
                graph, capacity, &position, record + 16,
                fixture->files[i].relative ))
            goto invalid;
    }

    package_graph_write_u32( graph + 16, position );
    package_graph_write_u32( graph + 64, position - strings_offset );
    if (!wine_appx_graph_validate_blob( graph, position )) goto invalid;
    *graph_size = position;
    return graph;

invalid:
    HeapFree( GetProcessHeap(), 0, graph );
    return NULL;
}

typedef DWORD (WINAPI *package_value_func)(void);
typedef void *(WINAPI *ldr_resolve_delay_loaded_api_func)(
    void *, const IMAGE_DELAYLOAD_DESCRIPTOR *,
    PDELAYLOAD_FAILURE_DLL_CALLBACK, PDELAYLOAD_FAILURE_SYSTEM_ROUTINE,
    IMAGE_THUNK_DATA *, ULONG );

struct package_delay_import
{
    WORD hint;
    char name[14];
};

struct package_delay_image
{
    IMAGE_DELAYLOAD_DESCRIPTOR descriptor;
    HMODULE module;
    IMAGE_THUNK_DATA iat[2];
    IMAGE_THUNK_DATA names[2];
    char dll_name[13];
    struct package_delay_import import;
};

static NTSTATUS package_delay_failure_status;

static void *WINAPI package_delay_dll_failure(
    ULONG reason, DELAYLOAD_INFO *info )
{
    (void)reason;
    package_delay_failure_status = info->LastError;
    return NULL;
}

static void *WINAPI package_delay_system_failure(
    const char *dll_name, const char *function_name )
{
    (void)dll_name;
    (void)function_name;
    return NULL;
}

static NTSTATUS load_package_module( const WCHAR *name, HMODULE *module )
{
    UNICODE_STRING string;

    *module = NULL;
    RtlInitUnicodeString( &string, name );
    return LdrLoadDll( NULL, 0, &string, module );
}

static NTSTATUS get_package_export( HMODULE module, const char *name,
                                    void **function )
{
    ANSI_STRING string;

    RtlInitAnsiString( &string, name );
    return LdrGetProcedureAddress( module, &string, 0, function );
}

static BOOL get_child_package_root( WCHAR *root, unsigned int count )
{
    const BYTE *graph = NtCurrentTeb()->Peb->ProcessParameters->
                        PackageDependencyData;
    struct wine_appx_graph_string_ref ref;
    unsigned int package_offset, size, i;

    if (!graph) return FALSE;
    size = wine_appx_graph_read_u32(
        graph + WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET );
    if (!wine_appx_graph_validate_blob( graph, size )) return FALSE;
    package_offset = wine_appx_graph_read_u32( graph + 48 );
    ref = wine_appx_graph_get_ref( graph + package_offset, 104 );
    if (!ref.chars || ref.chars > count) return FALSE;
    for (i = 0; i < ref.chars; i++)
        root[i] = wine_appx_graph_read_u16(
            graph + ref.offset + i * sizeof(WCHAR) );
    return TRUE;
}

static BOOL package_module_path_matches( HMODULE module,
                                         const WCHAR *expected )
{
    WCHAR path[MAX_PATH];
    DWORD length;

    length = GetModuleFileNameW( module, path, ARRAY_SIZE(path) );
    return length && length < ARRAY_SIZE(path) &&
           !lstrcmpiW( path, expected );
}

static DWORD test_package_loader_happy_child(void)
{
    struct package_delay_image delay_image;
    ldr_resolve_delay_loaded_api_func resolve_delay;
    WCHAR root[MAX_PATH], path[MAX_PATH];
    HMODULE module, second, system_module;
    NTSTATUS status;
    void *address;
    DWORD value;

    if (!get_child_package_root( root, ARRAY_SIZE(root) )) return 0x101;

    lstrcpyW( path, root );
    if (!append_path_component( path, ARRAY_SIZE(path), L"bin" ) ||
        !append_path_component(
            path, ARRAY_SIZE(path), L"package_importer.dll" ))
        return 0x102;
    status = load_package_module( path, &module );
    if (status) return 0x1030000 | (status & 0xffff);
    status = get_package_export(
        module, "package_importer_value", &address );
    if (status)
    {
        LdrUnloadDll( module );
        return 0x1040000 | (status & 0xffff);
    }
    value = ((package_value_func)address)();
    LdrUnloadDll( module );
    if (value != 0x1a17c0de) return 0x105;

    status = load_package_module( L"pkgexplicit.dll", &module );
    if (status) return 0x1060000 | (status & 0xffff);
    lstrcpyW( path, root );
    append_path_component( path, ARRAY_SIZE(path), L"bin" );
    append_path_component( path, ARRAY_SIZE(path), L"pkgexplicit.dll" );
    if (!package_module_path_matches( module, path ))
    {
        LdrUnloadDll( module );
        return 0x107;
    }
    status = get_package_export( module, "package_value", &address );
    if (status)
    {
        LdrUnloadDll( module );
        return 0x1080000 | (status & 0xffff);
    }
    value = ((package_value_func)address)();
    LdrUnloadDll( module );
    if (value != 0x51a7c0de) return 0x109;

    resolve_delay = (void *)GetProcAddress(
        GetModuleHandleW( L"ntdll.dll" ), "LdrResolveDelayLoadedAPI" );
    if (!resolve_delay) return 0x10a;
    memset( &delay_image, 0, sizeof(delay_image) );
    delay_image.descriptor.Attributes.RvaBased = 1;
    delay_image.descriptor.DllNameRVA =
        offsetof(struct package_delay_image, dll_name);
    delay_image.descriptor.ModuleHandleRVA =
        offsetof(struct package_delay_image, module);
    delay_image.descriptor.ImportAddressTableRVA =
        offsetof(struct package_delay_image, iat);
    delay_image.descriptor.ImportNameTableRVA =
        offsetof(struct package_delay_image, names);
    delay_image.names[0].u1.AddressOfData =
        offsetof(struct package_delay_image, import);
    lstrcpyA( delay_image.dll_name, "pkgdelay.dll" );
    lstrcpyA( delay_image.import.name, "package_value" );
    package_delay_failure_status = STATUS_SUCCESS;
    address = resolve_delay(
        &delay_image, &delay_image.descriptor, package_delay_dll_failure,
        package_delay_system_failure, &delay_image.iat[0], 0 );
    if (!address)
        return 0x10b0000 | (package_delay_failure_status & 0xffff);
    value = ((package_value_func)address)();
    if (delay_image.module) LdrUnloadDll( delay_image.module );
    if (value != 0x51a7c0de ||
        delay_image.iat[0].u1.Function != (ULONG_PTR)address)
        return 0x10c;

    status = load_package_module( L"pkgforward.dll", &module );
    if (status) return 0x10d0000 | (status & 0xffff);
    status = get_package_export( module, "package_forwarded", &address );
    if (status)
    {
        LdrUnloadDll( module );
        return 0x10e0000 | (status & 0xffff);
    }
    value = ((package_value_func)address)();
    LdrUnloadDll( module );
    if (value != 0x51a7c0de) return 0x10f;

    status = load_package_module( L"pkgloaded.dll", &module );
    if (status) return 0x1100000 | (status & 0xffff);
    status = load_package_module( L"PKGLOADED.DLL", &second );
    if (status || module != second)
    {
        if (!status) LdrUnloadDll( second );
        LdrUnloadDll( module );
        return status ? 0x1110000 | (status & 0xffff) : 0x112;
    }
    LdrUnloadDll( second );
    LdrUnloadDll( module );

    system_module = GetModuleHandleW( L"kernel32.dll" );
    if (!system_module) return 0x113;
    status = load_package_module( L"kernel32.dll", &module );
    if (status || module != system_module)
    {
        if (!status) LdrUnloadDll( module );
        return status ? 0x1140000 | (status & 0xffff) : 0x115;
    }
    if (!get_package_export( module, "package_value", &address ))
    {
        LdrUnloadDll( module );
        return 0x116;
    }
    LdrUnloadDll( module );

    status = load_package_module( L"opengl32.dll", &module );
    if (status) return 0x11610000 | (status & 0xffff);
    lstrcpyW( path, root );
    append_path_component( path, ARRAY_SIZE(path), L"bin" );
    append_path_component( path, ARRAY_SIZE(path), L"opengl32.dll" );
    if (!package_module_path_matches( module, path ))
    {
        LdrUnloadDll( module );
        return 0x1162;
    }
    LdrUnloadDll( module );

    lstrcpyW( path, root );
    if (!append_path_component( path, ARRAY_SIZE(path), L"ambient" ) ||
        !append_path_component(
            path, ARRAY_SIZE(path), L"pkgblocked.dll" ))
        return 0x117;
    status = load_package_module( path, &module );
    if (!status)
    {
        LdrUnloadDll( module );
        return 0x118;
    }
    if (status != STATUS_INVALID_IMAGE_HASH)
        return 0x1190000 | (status & 0xffff);

    status = load_package_module( L"pkgblocked.dll", &module );
    if (status) return 0x11a0000 | (status & 0xffff);
    lstrcpyW( path, root );
    append_path_component( path, ARRAY_SIZE(path), L"bin" );
    append_path_component( path, ARRAY_SIZE(path), L"pkgblocked.dll" );
    if (!package_module_path_matches( module, path ))
    {
        LdrUnloadDll( module );
        return 0x11b;
    }
    LdrUnloadDll( module );
    return 0;
}

static DWORD test_package_loader_failure_child( const WCHAR *name,
                                                NTSTATUS expected )
{
    HMODULE module;
    NTSTATUS status = load_package_module( name, &module );

    if (!status) LdrUnloadDll( module );
    if (status != expected) return 0x2000000 | (status & 0xffff);
    return 0;
}

static DWORD test_package_loader_unpackaged_child(void)
{
    WCHAR path[MAX_PATH];
    HMODULE module, second;
    NTSTATUS status;
    DWORD length;

    if (NtCurrentTeb()->Peb->ProcessParameters->PackageDependencyData)
        return 0x301;
    length = GetCurrentDirectoryW( ARRAY_SIZE(path), path );
    if (!length || length >= ARRAY_SIZE(path) ||
        !append_path_component(
            path, ARRAY_SIZE(path), L"pkgblocked.dll" ))
        return 0x302;
    module = LoadLibraryW( path );
    if (!module) return 0x3030000 | (GetLastError() & 0xffff);
    status = load_package_module( L"pkgblocked.dll", &second );
    if (status || module != second)
    {
        if (!status) LdrUnloadDll( second );
        FreeLibrary( module );
        return status ? 0x3040000 | (status & 0xffff) : 0x305;
    }
    LdrUnloadDll( second );
    FreeLibrary( module );
    return 0;
}

static DWORD test_package_loader_root_policy_child(void)
{
    WCHAR root[MAX_PATH], current[MAX_PATH], path[MAX_PATH];
    WCHAR system_path[MAX_PATH];
    HMODULE module;
    NTSTATUS status;
    void *address;
    DWORD length, value;

    if (!get_child_package_root( root, ARRAY_SIZE(root) )) return 0x321;

    status = load_package_module( L"winmm.dll", &module );
    if (status) return 0x3220000 | (status & 0xffff);
    length = GetSystemDirectoryW( system_path, ARRAY_SIZE(system_path) );
    if (!length || length >= ARRAY_SIZE(system_path) ||
        !append_path_component(
            system_path, ARRAY_SIZE(system_path), L"winmm.dll" ) ||
        !package_module_path_matches( module, system_path ))
    {
        LdrUnloadDll( module );
        return 0x323;
    }
    LdrUnloadDll( module );

    length = GetCurrentDirectoryW( ARRAY_SIZE(current), current );
    if (!length || length >= ARRAY_SIZE(current)) return 0x324;
    status = load_package_module( L"pkgambient.dll", &module );
    if (!status)
    {
        LdrUnloadDll( module );
        return 0x325;
    }
    if (status != STATUS_INVALID_IMAGE_HASH)
        return 0x3260000 | (status & 0xffff);

    status = load_package_module( L".\\pkgblocked.dll", &module );
    if (!status)
    {
        LdrUnloadDll( module );
        return 0x327;
    }
    if (status != STATUS_INVALID_IMAGE_HASH)
        return 0x3280000 | (status & 0xffff);

    lstrcpyW( path, current );
    if (!append_path_component(
            path, ARRAY_SIZE(path), L"pkgambient.dll" ))
        return 0x329;
    status = load_package_module( path, &module );
    if (!status)
    {
        LdrUnloadDll( module );
        return 0x32a;
    }
    if (status != STATUS_INVALID_IMAGE_HASH)
        return 0x32b0000 | (status & 0xffff);

    if (lstrlenW( root ) + ARRAY_SIZE(L"-outside") >
        ARRAY_SIZE(path))
        return 0x32c;
    lstrcpyW( path, root );
    lstrcatW( path, L"-outside" );
    if (!append_path_component(
            path, ARRAY_SIZE(path), L"pkgoutside.dll" ))
        return 0x32d;
    status = load_package_module( path, &module );
    if (status) return 0x32e0000 | (status & 0xffff);
    status = get_package_export( module, "package_value", &address );
    if (status)
    {
        LdrUnloadDll( module );
        return 0x32f0000 | (status & 0xffff);
    }
    value = ((package_value_func)address)();
    LdrUnloadDll( module );
    if (value != 0x51a7c0de) return 0x330;
    return 0;
}

static DWORD test_package_loader_hardlink_child(void)
{
    WCHAR root[MAX_PATH], expected[MAX_PATH], alias[MAX_PATH];
    HMODULE alias_module, package_module;
    NTSTATUS status;

    if (!get_child_package_root( root, ARRAY_SIZE(root) )) return 0x351;
    lstrcpyW( alias, root );
    if (!append_path_component( alias, ARRAY_SIZE(alias), L"ambient" ) ||
        !append_path_component( alias, ARRAY_SIZE(alias), L"pkgalias.dll" ))
        return 0x352;
    status = load_package_module( alias, &alias_module );
    if (!status)
    {
        LdrUnloadDll( alias_module );
        return 0x353;
    }
    if (status != STATUS_INVALID_IMAGE_HASH)
        return 0x3540000 | (status & 0xffff);

    status = load_package_module( L"pkgloaded.dll", &package_module );
    if (status) return 0x3550000 | (status & 0xffff);
    lstrcpyW( expected, root );
    append_path_component( expected, ARRAY_SIZE(expected), L"bin" );
    append_path_component(
        expected, ARRAY_SIZE(expected), L"pkgloaded.dll" );
    if (!package_module_path_matches( package_module, expected ))
    {
        LdrUnloadDll( package_module );
        return 0x356;
    }
    LdrUnloadDll( package_module );
    return 0;
}

static DWORD test_package_loader_preloaded_unverified_child(void)
{
    RTL_USER_PROCESS_PARAMETERS *params =
        NtCurrentTeb()->Peb->ProcessParameters;
    void *graph = params->PackageDependencyData;
    WCHAR root[MAX_PATH], path[MAX_PATH];
    HMODULE module, second;
    NTSTATUS status;

    if (!graph || !get_child_package_root( root, ARRAY_SIZE(root) ))
        return 0x371;
    lstrcpyW( path, root );
    if (!append_path_component( path, ARRAY_SIZE(path), L"bin" ) ||
        !append_path_component(
            path, ARRAY_SIZE(path), L"pkgloaded.dll" ))
        return 0x372;

    params->PackageDependencyData = NULL;
    module = LoadLibraryW( path );
    params->PackageDependencyData = graph;
    if (!module) return 0x3730000 | (GetLastError() & 0xffff);

    status = load_package_module( L"pkgloaded.dll", &second );
    if (!status) LdrUnloadDll( second );
    FreeLibrary( module );
    if (status != STATUS_INVALID_IMAGE_HASH)
        return 0x3740000 | (status & 0xffff);
    return 0;
}

static DWORD test_package_loader_corrupt_child( BOOL ordering )
{
    BYTE *copy;
    BYTE *record_a, *record_z;
    const BYTE *graph = NtCurrentTeb()->Peb->ProcessParameters->
                        PackageDependencyData;
    struct wine_appx_graph_string_ref basename, path;
    UNICODE_STRING ntdll_name;
    WCHAR root[MAX_PATH], filename[MAX_PATH];
    HMODULE expected_ntdll, module;
    NTSTATUS status;
    unsigned int loaders_offset, size;

    if (!graph) return 0x401;
    size = wine_appx_graph_read_u32(
        graph + WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET );
    if (!wine_appx_graph_validate_blob( graph, size )) return 0x402;
    if (!(expected_ntdll = GetModuleHandleW( L"ntdll.dll" ))) return 0x403;
    copy = HeapAlloc( GetProcessHeap(), 0, size );
    if (!copy) return 0x404;
    memcpy( copy, graph, size );
    loaders_offset = wine_appx_graph_read_u32( copy + 56 );

    if (!ordering)
    {
        package_graph_write_u32( copy + loaders_offset + 8, size );
        if (wine_appx_graph_validate_blob( copy, size )) return 0x405;
    }
    else
    {
        record_a = copy + loaders_offset +
            PACKAGE_LOADER_ORDER_A *
            WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        record_z = copy + loaders_offset +
            PACKAGE_LOADER_ORDER_Z *
            WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        basename = wine_appx_graph_get_ref( record_a, 8 );
        path = wine_appx_graph_get_ref( record_a, 16 );
        package_graph_write_u16(
            copy + basename.offset + (basename.chars - 6) * sizeof(WCHAR),
            'z' );
        package_graph_write_u16(
            copy + path.offset + (path.chars - 6) * sizeof(WCHAR), 'z' );
        basename = wine_appx_graph_get_ref( record_z, 8 );
        path = wine_appx_graph_get_ref( record_z, 16 );
        package_graph_write_u16(
            copy + basename.offset + (basename.chars - 6) * sizeof(WCHAR),
            'a' );
        package_graph_write_u16(
            copy + path.offset + (path.chars - 6) * sizeof(WCHAR), 'a' );
        if (!wine_appx_graph_validate_blob( copy, size )) return 0x406;
    }

    NtCurrentTeb()->Peb->ProcessParameters->PackageDependencyData = copy;
    RtlInitUnicodeString( &ntdll_name, L"ntdll.dll" );
    status = LdrGetDllHandle( NULL, 0, &ntdll_name, &module );
    if (status) return 0x4070000 | (status & 0xffff);
    if (module != expected_ntdll) return 0x408;

    status = load_package_module( L"pkgcorrupt.dll", &module );
    if (!status) LdrUnloadDll( module );
    if (status != STATUS_FILE_CORRUPT_ERROR)
        return 0x4090000 | (status & 0xffff);
    if (!ordering) return 0;

    if (!get_child_package_root( root, ARRAY_SIZE(root) )) return 0x40a;
    lstrcpyW( filename, root );
    if (!append_path_component(
            filename, ARRAY_SIZE(filename), L"ambient" ) ||
        !append_path_component(
            filename, ARRAY_SIZE(filename), L"pkgambient.dll" ))
        return 0x40b;
    status = load_package_module( filename, &module );
    if (!status)
    {
        LdrUnloadDll( module );
        return 0x40c;
    }
    if (status != STATUS_FILE_CORRUPT_ERROR)
        return 0x40d0000 | (status & 0xffff);

    if (lstrlenW( root ) + ARRAY_SIZE(L"-outside") >
        ARRAY_SIZE(filename))
        return 0x40e;
    lstrcpyW( filename, root );
    lstrcatW( filename, L"-outside" );
    if (!append_path_component(
            filename, ARRAY_SIZE(filename), L"pkgoutside.dll" ))
        return 0x40f;
    status = load_package_module( filename, &module );
    if (status) return 0x4100000 | (status & 0xffff);
    LdrUnloadDll( module );
    return 0;
}

static DWORD test_package_loader_inherited_grandchild(void)
{
    const BYTE *graph = NtCurrentTeb()->Peb->ProcessParameters->
                        PackageDependencyData;
    unsigned int size;

    if (!graph) return 0x451;
    size = wine_appx_graph_read_u32(
        graph + WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET );
    if (!wine_appx_graph_validate_blob( graph, size )) return 0x452;
    return 0;
}

static DWORD test_package_loader_inheritance_child(void)
{
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    WCHAR module[MAX_PATH], command[MAX_PATH + 64];
    DWORD result = ~0u, wait;

    if (!GetModuleFileNameW( NULL, module, ARRAY_SIZE(module) ) ||
        swprintf( command, ARRAY_SIZE(command),
                  L"\"%s\" loader package-child inherited", module ) < 0)
        return 0x461;
    if (!CreateProcessW( module, command, NULL, NULL, FALSE, 0, NULL, NULL,
                         &startup, &process ))
        return 0x4620000 | (GetLastError() & 0xffff);
    wait = WaitForSingleObject( process.hProcess, 10000 );
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess( process.hProcess, STATUS_TIMEOUT );
        WaitForSingleObject( process.hProcess, 5000 );
        result = STATUS_TIMEOUT;
    }
    else if (!GetExitCodeProcess( process.hProcess, &result ))
        result = HRESULT_FROM_WIN32( GetLastError() );
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );
    return result;
}

static DWORD package_loader_child( int argc, char **argv )
{
    if (argc < 4) return 0x501;
    if (!strcmp( argv[3], "happy" ))
        return test_package_loader_happy_child();
    if (!strcmp( argv[3], "substitute" ))
        return test_package_loader_failure_child(
            L"pkgsubstitute.dll", STATUS_INVALID_IMAGE_HASH );
    if (!strcmp( argv[3], "missing" ))
        return test_package_loader_failure_child(
            L"pkgmissing.dll", STATUS_DLL_NOT_FOUND );
    if (!strcmp( argv[3], "reparse" ))
        return test_package_loader_failure_child(
            L"pkgreparse.dll", STATUS_INVALID_IMAGE_HASH );
    if (!strcmp( argv[3], "bad-offset" ))
        return test_package_loader_corrupt_child( FALSE );
    if (!strcmp( argv[3], "bad-order" ))
        return test_package_loader_corrupt_child( TRUE );
    if (!strcmp( argv[3], "unpackaged" ))
        return test_package_loader_unpackaged_child();
    if (!strcmp( argv[3], "root-policy" ))
        return test_package_loader_root_policy_child();
    if (!strcmp( argv[3], "hardlink" ))
        return test_package_loader_hardlink_child();
    if (!strcmp( argv[3], "preloaded" ))
        return test_package_loader_preloaded_unverified_child();
    if (!strcmp( argv[3], "bad-token" ))
        return test_package_loader_failure_child(
            L"pkgexplicit.dll", STATUS_INVALID_IMAGE_HASH );
    if (!strcmp( argv[3], "truncate" ))
        return test_package_loader_failure_child(
            L"pkgmissing.dll", STATUS_INVALID_IMAGE_HASH );
    if (!strcmp( argv[3], "inherit" ))
        return test_package_loader_inheritance_child();
    if (!strcmp( argv[3], "inherited" ))
        return test_package_loader_inherited_grandchild();
    return 0x502;
}

static NTSTATUS create_package_loader_process(
    const WCHAR *mode, const struct package_loader_fixture *fixture,
    BOOL packaged, HANDLE parent, HANDLE *process, HANDLE *thread )
{
    ULONG_PTR attr_buffer[offsetof(PS_ATTRIBUTE_LIST, Attributes[2]) /
                          sizeof(ULONG_PTR)];
    PS_ATTRIBUTE_LIST *attr = (PS_ATTRIBUTE_LIST *)attr_buffer;
    struct wine_appx_graph_attach attach;
    unsigned long long lease;
    RTL_USER_PROCESS_PARAMETERS *params;
    PS_CREATE_INFO create_info;
    UNICODE_STRING image, command, current_directory;
    WCHAR module[MAX_PATH], nt_path[MAX_PATH + 4];
    WCHAR command_line[MAX_PATH + 64];
    NTSTATUS status;

    *process = *thread = NULL;
    if (!GetModuleFileNameW( NULL, module, ARRAY_SIZE(module) ) ||
        lstrlenW( module ) + 4 >= ARRAY_SIZE(nt_path))
        return STATUS_NAME_TOO_LONG;
    lstrcpyW( nt_path, L"\\??\\" );
    lstrcatW( nt_path, module );
    if (swprintf( command_line, ARRAY_SIZE(command_line),
                  L"\"%s\" loader package-child %s", module, mode ) < 0)
        return STATUS_NAME_TOO_LONG;
    RtlInitUnicodeString( &image, nt_path );
    RtlInitUnicodeString( &command, command_line );
    RtlInitUnicodeString( &current_directory, fixture->ambient );
    status = RtlCreateProcessParametersEx(
        &params, &image, NULL, &current_directory, &command, NULL, NULL,
        NULL, NULL, NULL, PROCESS_PARAMS_FLAG_NORMALIZED );
    if (status) return status;

    if (packaged)
    {
        lease = (ULONG_PTR)fixture->lease;
        memset( &attach, 0, sizeof(attach) );
        attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
        attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
        attach.size = fixture->graph_size;
        attach.blob = (ULONG_PTR)fixture->graph;
        attach.leases = (ULONG_PTR)&lease;
        attach.lease_count = 1;
        params->PackageDependencyData = &attach;
    }

    memset( attr_buffer, 0, sizeof(attr_buffer) );
    attr->TotalLength = offsetof(PS_ATTRIBUTE_LIST, Attributes[1]);
    if (parent) attr->TotalLength += sizeof(attr->Attributes[0]);
    attr->Attributes[0].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
    attr->Attributes[0].Size = image.Length;
    attr->Attributes[0].ValuePtr = image.Buffer;
    if (parent)
    {
        attr->Attributes[1].Attribute = PS_ATTRIBUTE_PARENT_PROCESS;
        attr->Attributes[1].Size = sizeof(parent);
        attr->Attributes[1].ValuePtr = parent;
    }

    memset( &create_info, 0, sizeof(create_info) );
    create_info.Size = sizeof(create_info);
    status = NtCreateUserProcess(
        process, thread, PROCESS_ALL_ACCESS, THREAD_ALL_ACCESS, NULL, NULL,
        0, THREAD_CREATE_FLAGS_CREATE_SUSPENDED, params, &create_info,
        attr );
    RtlDestroyProcessParameters( params );
    return status;
}

static DWORD run_package_loader_child(
    const WCHAR *mode, const struct package_loader_fixture *fixture,
    BOOL packaged )
{
    HANDLE process, thread;
    NTSTATUS status;
    DWORD result = ~0u, wait;

    status = create_package_loader_process(
        mode, fixture, packaged, NULL, &process, &thread );
    if (status) return status;
    status = NtResumeThread( thread, NULL );
    if (status)
    {
        NtTerminateProcess( process, status );
        result = status;
        goto done;
    }
    wait = WaitForSingleObject( process, 10000 );
    if (wait != WAIT_OBJECT_0)
    {
        NtTerminateProcess( process, STATUS_TIMEOUT );
        WaitForSingleObject( process, 5000 );
        result = STATUS_TIMEOUT;
        goto done;
    }
    if (!GetExitCodeProcess( process, &result ))
        result = HRESULT_FROM_WIN32( GetLastError() );

done:
    CloseHandle( thread );
    CloseHandle( process );
    return result;
}

static void delete_package_loader_fixture(
    struct package_loader_fixture *fixture )
{
    WCHAR path[MAX_PATH];
    unsigned int i;

    if (fixture->lease != INVALID_HANDLE_VALUE)
    {
        CloseHandle( fixture->lease );
        fixture->lease = INVALID_HANDLE_VALUE;
    }
    HeapFree( GetProcessHeap(), 0, fixture->graph );
    fixture->graph = NULL;

    DeleteFileW( fixture->importer );
    DeleteFileW( fixture->hardlink_alias );
    DeleteFileW( fixture->unregistered );
    DeleteFileW( fixture->outside_module );
    DeleteFileW( fixture->missing_saved );
    DeleteFileW( fixture->reparse_target );
    DeleteFileW( fixture->substitute_source );
    DeleteFileW( fixture->mesa_opengl );
    for (i = 0; i < ARRAY_SIZE(fixture->files); i++)
        DeleteFileW( fixture->files[i].path );
    for (i = 0; i < ARRAY_SIZE(package_loader_names); i++)
    {
        lstrcpyW( path, fixture->ambient );
        if (append_path_component(
                path, ARRAY_SIZE(path), package_loader_names[i] ))
            DeleteFileW( path );
    }
    DeleteFileW( fixture->lease_path );
    RemoveDirectoryW( fixture->mesa_arch );
    RemoveDirectoryW( fixture->mesa_root );
    RemoveDirectoryW( fixture->bin );
    RemoveDirectoryW( fixture->ambient );
    RemoveDirectoryW( fixture->root );
    RemoveDirectoryW( fixture->outside_root );
}

static BOOL create_package_loader_fixture(
    struct package_loader_fixture *fixture )
{
    HANDLE marker = INVALID_HANDLE_VALUE;
    WCHAR temp[MAX_PATH];
    unsigned int i;

    memset( fixture, 0, sizeof(*fixture) );
    fixture->lease = INVALID_HANDLE_VALUE;
    if (!GetTempPathW( ARRAY_SIZE(temp), temp ) ||
        !GetTempFileNameW( temp, L"pgl", 0, fixture->root ))
        goto failed;
    DeleteFileW( fixture->root );
    if (!CreateDirectoryW( fixture->root, NULL )) goto failed;
    if (lstrlenW( fixture->root ) + ARRAY_SIZE(L"-outside") >
        ARRAY_SIZE(fixture->outside_root))
        goto failed;
    lstrcpyW( fixture->bin, fixture->root );
    lstrcpyW( fixture->ambient, fixture->root );
    lstrcpyW( fixture->mesa_root, fixture->root );
    lstrcpyW( fixture->mesa_arch, fixture->root );
    lstrcpyW( fixture->outside_root, fixture->root );
    lstrcatW( fixture->outside_root, L"-outside" );
    if (!append_path_component(
            fixture->bin, ARRAY_SIZE(fixture->bin), L"bin" ) ||
        !append_path_component(
            fixture->ambient, ARRAY_SIZE(fixture->ambient), L"ambient" ) ||
        !append_path_component(
            fixture->mesa_root, ARRAY_SIZE(fixture->mesa_root), L"mesa" ) ||
        !append_path_component(
            fixture->mesa_arch, ARRAY_SIZE(fixture->mesa_arch), L"mesa" ) ||
#ifdef _WIN64
        !append_path_component(
            fixture->mesa_arch, ARRAY_SIZE(fixture->mesa_arch),
            L"x86_64-windows" ) ||
#else
        !append_path_component(
            fixture->mesa_arch, ARRAY_SIZE(fixture->mesa_arch),
            L"i386-windows" ) ||
#endif
        !CreateDirectoryW( fixture->bin, NULL ) ||
        !CreateDirectoryW( fixture->ambient, NULL ) ||
        !CreateDirectoryW( fixture->mesa_root, NULL ) ||
        !CreateDirectoryW( fixture->mesa_arch, NULL ) ||
        !CreateDirectoryW( fixture->outside_root, NULL ))
        goto failed;

    lstrcpyW( fixture->mesa_opengl, fixture->mesa_arch );
    if (!append_path_component(
            fixture->mesa_opengl, ARRAY_SIZE(fixture->mesa_opengl),
            L"opengl32.dll" ) ||
        !extract_package_test_dll(
            L"package_target.dll", fixture->mesa_opengl, NULL, NULL ))
        goto failed;
    lstrcpyW( fixture->outside_module, fixture->outside_root );
    if (!append_path_component(
            fixture->outside_module, ARRAY_SIZE(fixture->outside_module),
            L"pkgoutside.dll" ) ||
        !extract_package_test_dll(
            L"package_target.dll", fixture->outside_module, NULL, NULL ))
        goto failed;

    lstrcpyW( fixture->lease_path, fixture->root );
    if (!append_path_component(
            fixture->lease_path, ARRAY_SIZE(fixture->lease_path),
            L"generation.lease" ))
        goto failed;
    marker = CreateFileW(
        fixture->lease_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, NULL );
    if (marker == INVALID_HANDLE_VALUE ||
        !write_package_generation_marker( marker ))
        goto failed;
    CloseHandle( marker );
    marker = INVALID_HANDLE_VALUE;
    fixture->lease = CreateFileW(
        fixture->lease_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL );
    if (fixture->lease == INVALID_HANDLE_VALUE) goto failed;

    for (i = 0; i < ARRAY_SIZE(package_loader_names); i++)
    {
        struct package_loader_file *file = &fixture->files[i];
        const WCHAR *resource = L"package_target.dll";
        const char *replace_from = NULL, *replace_to = NULL;

        file->basename = package_loader_names[i];
        lstrcpyW( file->relative, L"bin" );
        if (!append_path_component(
                file->relative, ARRAY_SIZE(file->relative),
                file->basename ))
            goto failed;
        lstrcpyW( file->path, fixture->root );
        if (!append_path_component(
                file->path, ARRAY_SIZE(file->path), file->relative ))
            goto failed;
        if (!lstrcmpW( file->basename, L"pkgforward.dll" ))
            resource = L"package_forwarder.dll";
        else if (i == PACKAGE_LOADER_IMPORTER)
        {
            resource = L"package_importer.dll";
            replace_from = "version.dll";
            replace_to = "pkginit.dll";
            lstrcpyW( fixture->importer, file->path );
        }
        if (!extract_package_test_dll(
                resource, file->path, replace_from, replace_to ))
            goto failed;
        if (!get_file_identity(
                file->path, &file->volume_serial, &file->file_index_high,
                &file->file_index_low, file->object_id ) ||
            !get_file_stamp(
                file->path, &file->change_time, &file->file_size ))
            goto failed;
        if (i && lstrcmpiW( fixture->files[i - 1].basename,
                           file->basename ) >= 0)
            goto failed;
    }

    lstrcpyW( temp, fixture->ambient );
    append_path_component( temp, ARRAY_SIZE(temp), L"pkgblocked.dll" );
    if (!extract_package_test_dll(
            L"package_target.dll", temp, NULL, NULL ))
        goto failed;
    lstrcpyW( fixture->unregistered, fixture->ambient );
    if (!append_path_component(
            fixture->unregistered, ARRAY_SIZE(fixture->unregistered),
            L"pkgambient.dll" ) ||
        !extract_package_test_dll(
            L"package_target.dll", fixture->unregistered, NULL, NULL ))
        goto failed;
    lstrcpyW( temp, fixture->ambient );
    append_path_component( temp, ARRAY_SIZE(temp), L"pkgmissing.dll" );
    if (!extract_package_test_dll(
            L"package_target.dll", temp, NULL, NULL ))
        goto failed;
    lstrcpyW( temp, fixture->ambient );
    append_path_component( temp, ARRAY_SIZE(temp), L"pkgsubstitute.dll" );
    if (!extract_package_test_dll(
            L"package_target.dll", temp, NULL, NULL ))
        goto failed;

    lstrcpyW( fixture->reparse_target, fixture->ambient );
    if (!append_path_component(
            fixture->reparse_target, ARRAY_SIZE(fixture->reparse_target),
            L"reparse-target.dll" ) ||
        !extract_package_test_dll(
            L"package_target.dll", fixture->reparse_target, NULL, NULL ))
        goto failed;
    lstrcpyW( fixture->substitute_source, fixture->root );
    if (!append_path_component(
            fixture->substitute_source,
            ARRAY_SIZE(fixture->substitute_source),
            L"substitute-new.dll" ) ||
        !extract_package_test_dll(
            L"package_target.dll", fixture->substitute_source, NULL, NULL ))
        goto failed;
    lstrcpyW( fixture->missing_saved, fixture->root );
    if (!append_path_component(
            fixture->missing_saved, ARRAY_SIZE(fixture->missing_saved),
            L"missing-saved.dll" ))
        goto failed;

    lstrcpyW( fixture->hardlink_alias, fixture->ambient );
    if (!append_path_component(
            fixture->hardlink_alias, ARRAY_SIZE(fixture->hardlink_alias),
            L"pkgalias.dll" ))
        goto failed;
    fixture->hardlink_ready = CreateHardLinkW(
        fixture->hardlink_alias,
        fixture->files[PACKAGE_LOADER_LOADED].path, NULL );
    fixture->hardlink_error =
        fixture->hardlink_ready ? ERROR_SUCCESS : GetLastError();
    if (fixture->hardlink_ready &&
        (!get_file_identity(
             fixture->files[PACKAGE_LOADER_LOADED].path,
             &fixture->files[PACKAGE_LOADER_LOADED].volume_serial,
             &fixture->files[PACKAGE_LOADER_LOADED].file_index_high,
             &fixture->files[PACKAGE_LOADER_LOADED].file_index_low,
             fixture->files[PACKAGE_LOADER_LOADED].object_id ) ||
         !get_file_stamp(
             fixture->files[PACKAGE_LOADER_LOADED].path,
             &fixture->files[PACKAGE_LOADER_LOADED].change_time,
             &fixture->files[PACKAGE_LOADER_LOADED].file_size )))
        goto failed;

    fixture->graph = build_package_loader_graph(
        fixture, &fixture->graph_size );
    if (!fixture->graph) goto failed;
    return TRUE;

failed:
    if (marker != INVALID_HANDLE_VALUE) CloseHandle( marker );
    ok( 0, "Could not create package loader fixture, error %lu.\n",
        GetLastError() );
    delete_package_loader_fixture( fixture );
    return FALSE;
}

static void test_package_graph_loader(void)
{
    struct package_loader_fixture fixture;
    struct package_loader_fixture writable_fixture;
    struct package_loader_rewrite_info rewrite_info;
    static const WCHAR mesa_path_var[] = L"SWITCHYARD_OPENGL_DLL_PATH";
    struct wine_appx_graph_string_ref package_full_name;
    WCHAR *old_mesa_path = NULL;
    DWORD volume_serial, index_high, index_low, package_offset, loaders_offset;
    DWORD saved_index, saved_volume;
    DWORD result, error, old_mesa_path_size;
    LONGLONG change_time;
    ULONGLONG file_size;
    BYTE saved_byte;
    HANDLE borrowed_process = NULL, borrowed_thread = NULL;
    HANDLE packaged_process = NULL, packaged_thread = NULL;
    HANDLE writable_lease;
    NTSTATUS status;
    BOOL had_mesa_path = FALSE, mesa_path_saved = FALSE;
    BOOL mesa_override_set = FALSE, ret;
    BOOL identity_unchanged, stamp_changed;

    if (!winetest_platform_is_wine)
    {
        win_skip( "Package graph loader integration is Wine-specific.\n" );
        return;
    }
    if (!create_package_loader_fixture( &fixture )) return;

    writable_lease = CreateFileW(
        fixture.lease_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( writable_lease != INVALID_HANDLE_VALUE,
        "Could not open a writable generation lease, error %lu.\n",
        GetLastError() );
    if (writable_lease != INVALID_HANDLE_VALUE)
    {
        writable_fixture = fixture;
        writable_fixture.lease = writable_lease;
        result = run_package_loader_child(
            L"happy", &writable_fixture, TRUE );
        ok( result == STATUS_INVALID_PARAMETER,
            "Writable generation lease returned %#lx.\n", result );
        CloseHandle( writable_lease );
    }

    writable_lease = CreateFileW(
        fixture.lease_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( writable_lease != INVALID_HANDLE_VALUE,
        "Could not open a delete-shared generation lease, error %lu.\n",
        GetLastError() );
    if (writable_lease != INVALID_HANDLE_VALUE)
    {
        writable_fixture = fixture;
        writable_fixture.lease = writable_lease;
        result = run_package_loader_child(
            L"happy", &writable_fixture, TRUE );
        ok( result == STATUS_INVALID_PARAMETER,
            "Delete-shared generation lease returned %#lx.\n", result );
        CloseHandle( writable_lease );
    }

    writable_lease = CreateFileW(
        fixture.lease_path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( writable_lease != INVALID_HANDLE_VALUE,
        "Could not open an attributes-only generation lease, error %lu.\n",
        GetLastError() );
    if (writable_lease != INVALID_HANDLE_VALUE)
    {
        writable_fixture = fixture;
        writable_fixture.lease = writable_lease;
        result = run_package_loader_child(
            L"happy", &writable_fixture, TRUE );
        ok( result == STATUS_ACCESS_DENIED,
            "Attributes-only generation lease returned %#lx.\n", result );
        CloseHandle( writable_lease );
    }

    CloseHandle( fixture.lease );
    fixture.lease = CreateFileW(
        fixture.lease_path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( fixture.lease != INVALID_HANDLE_VALUE,
        "Could not reopen protected generation lease, error %lu.\n",
        GetLastError() );
    if (fixture.lease == INVALID_HANDLE_VALUE)
    {
        delete_package_loader_fixture( &fixture );
        return;
    }

    status = create_package_loader_process(
        L"happy", &fixture, TRUE, NULL,
        &packaged_process, &packaged_thread );
    ok( !status, "Could not create packaged parent, status %#lx.\n", status );
    if (!status)
    {
        status = create_package_loader_process(
            L"happy", &fixture, FALSE, packaged_process,
            &borrowed_process, &borrowed_thread );
        ok( status == STATUS_ACCESS_DENIED,
            "Package graph borrowing returned %#lx.\n", status );
        if (!status)
        {
            NtTerminateProcess( borrowed_process, STATUS_SUCCESS );
            WaitForSingleObject( borrowed_process, 5000 );
            CloseHandle( borrowed_thread );
            CloseHandle( borrowed_process );
        }
        NtTerminateProcess( packaged_process, STATUS_SUCCESS );
        WaitForSingleObject( packaged_process, 5000 );
        CloseHandle( packaged_thread );
        CloseHandle( packaged_process );
    }

    package_offset = wine_appx_graph_read_u32( fixture.graph + 48 );
    saved_byte = fixture.graph[package_offset + 24];
    fixture.graph[package_offset + 24] ^= 0x80;
    result = run_package_loader_child( L"happy", &fixture, TRUE );
    ok( result == STATUS_INVALID_PARAMETER,
        "Mismatched package content ID returned %#lx.\n", result );
    fixture.graph[package_offset + 24] = saved_byte;

    package_full_name = wine_appx_graph_get_ref(
        fixture.graph + package_offset, 88 );
    saved_byte = fixture.graph[package_full_name.offset];
    fixture.graph[package_full_name.offset] =
        saved_byte == 'W' ? 'X' : 'W';
    result = run_package_loader_child( L"happy", &fixture, TRUE );
    ok( result == STATUS_INVALID_PARAMETER,
        "Mismatched package full name returned %#lx.\n", result );
    fixture.graph[package_full_name.offset] = saved_byte;

    saved_index = wine_appx_graph_read_u32(
        fixture.graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET );
    package_graph_write_u32(
        fixture.graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET,
        saved_index ^ 1 );
    result = run_package_loader_child( L"happy", &fixture, TRUE );
    ok( result == STATUS_INVALID_IMAGE_HASH,
        "Mismatched application identity returned %#lx.\n", result );
    package_graph_write_u32(
        fixture.graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET,
        saved_index );

    saved_byte =
        fixture.graph[WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET];
    fixture.graph[WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET] ^= 0x80;
    ok( wine_appx_graph_validate_blob( fixture.graph, fixture.graph_size ),
        "Mutated application object token malformed the graph.\n" );
    result = run_package_loader_child( L"happy", &fixture, TRUE );
    ok( result == STATUS_INVALID_IMAGE_HASH,
        "Mismatched application object token returned %#lx.\n", result );
    fixture.graph[WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET] = saved_byte;

    saved_volume = wine_appx_graph_read_u32(
        fixture.graph + WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET );
    package_graph_write_u32(
        fixture.graph + WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET,
        saved_volume == 1 ? 2 : 1 );
    result = run_package_loader_child( L"happy", &fixture, TRUE );
    ok( result == STATUS_INVALID_IMAGE_HASH,
        "Mismatched application volume returned %#lx.\n", result );
    package_graph_write_u32(
        fixture.graph + WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET,
        saved_volume );

    loaders_offset = wine_appx_graph_read_u32( fixture.graph + 56 );
    saved_byte = fixture.graph[
        loaders_offset +
        PACKAGE_LOADER_EXPLICIT * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE +
        WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET];
    fixture.graph[
        loaders_offset +
        PACKAGE_LOADER_EXPLICIT * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE +
        WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET] ^= 0x80;
    ok( wine_appx_graph_validate_blob( fixture.graph, fixture.graph_size ),
        "Mutated loader object token malformed the graph.\n" );
    result = run_package_loader_child( L"bad-token", &fixture, TRUE );
    ok( !result, "Mismatched loader object token child failed %#lx.\n",
        result );
    fixture.graph[
        loaders_offset +
        PACKAGE_LOADER_EXPLICIT * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE +
        WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET] = saved_byte;

    SetLastError( ERROR_SUCCESS );
    old_mesa_path_size = GetEnvironmentVariableW( mesa_path_var, NULL, 0 );
    error = GetLastError();
    if (old_mesa_path_size)
    {
        old_mesa_path = HeapAlloc(
            GetProcessHeap(), 0, old_mesa_path_size * sizeof(WCHAR) );
        ok( !!old_mesa_path, "Could not save the Mesa path.\n" );
        if (old_mesa_path &&
            GetEnvironmentVariableW(
                mesa_path_var, old_mesa_path, old_mesa_path_size ))
            had_mesa_path = mesa_path_saved = TRUE;
    }
    else if (error == ERROR_ENVVAR_NOT_FOUND)
        mesa_path_saved = TRUE;
    else if (error == ERROR_SUCCESS)
    {
        old_mesa_path = HeapAlloc( GetProcessHeap(), 0, sizeof(WCHAR) );
        ok( !!old_mesa_path, "Could not save the empty Mesa path.\n" );
        if (old_mesa_path)
        {
            old_mesa_path[0] = 0;
            had_mesa_path = mesa_path_saved = TRUE;
        }
    }
    if (mesa_path_saved)
    {
        mesa_override_set =
            SetEnvironmentVariableW( mesa_path_var, fixture.mesa_root );
        ok( mesa_override_set, "Could not set the Mesa path, error %lu.\n",
            GetLastError() );
    }
    else
        win_skip( "Could not preserve the existing Mesa path.\n" );
    result = run_package_loader_child( L"happy", &fixture, TRUE );
    ok( !result, "Package loader happy-path child failed %#lx.\n", result );
    result = run_package_loader_child( L"root-policy", &fixture, TRUE );
    ok( !result, "Package-root policy child failed %#lx.\n", result );
    result = run_package_loader_child( L"inherit", &fixture, TRUE );
    ok( !result, "Inherited package graph child failed %#lx.\n", result );
    if (mesa_override_set)
    {
        ret = SetEnvironmentVariableW(
            mesa_path_var, had_mesa_path ? old_mesa_path : NULL );
        ok( ret, "Could not restore the Mesa path, error %lu.\n",
            GetLastError() );
    }
    HeapFree( GetProcessHeap(), 0, old_mesa_path );
    result = run_package_loader_child( L"bad-offset", &fixture, TRUE );
    ok( !result, "Corrupt-offset child failed %#lx.\n", result );
    result = run_package_loader_child( L"bad-order", &fixture, TRUE );
    ok( !result, "Corrupt-order child failed %#lx.\n", result );
    result = run_package_loader_child( L"unpackaged", &fixture, FALSE );
    ok( !result, "Unpackaged loader child failed %#lx.\n", result );
    result = run_package_loader_child( L"preloaded", &fixture, TRUE );
    ok( !result, "Unverified preload child failed %#lx.\n", result );
    if (fixture.hardlink_ready)
    {
        result = run_package_loader_child( L"hardlink", &fixture, TRUE );
        ok( !result, "Hard-link provenance child failed %#lx.\n", result );
    }
    else
        win_skip( "Hard links are unavailable, error %lu.\n",
                  fixture.hardlink_error );

    ret = rewrite_file_preserving_size_and_write_time(
        fixture.files[PACKAGE_LOADER_SUBSTITUTE].path, &rewrite_info );
    ok( ret, "Could not rewrite %s in place, error %lu.\n",
        wine_dbgstr_w(fixture.files[PACKAGE_LOADER_SUBSTITUTE].path),
        GetLastError() );
    if (ret)
    {
        ret = get_file_identity(
            fixture.files[PACKAGE_LOADER_SUBSTITUTE].path,
            &volume_serial, &index_high, &index_low, NULL );
        ok( ret, "Could not query rewritten identity, error %lu.\n",
            GetLastError() );
        identity_unchanged = ret &&
            volume_serial ==
                fixture.files[PACKAGE_LOADER_SUBSTITUTE].volume_serial &&
            index_high ==
                fixture.files[PACKAGE_LOADER_SUBSTITUTE].file_index_high &&
            index_low ==
                fixture.files[PACKAGE_LOADER_SUBSTITUTE].file_index_low;
        ok( identity_unchanged,
            "In-place rewrite changed the graph file identity.\n" );
        ok( rewrite_info.before_size ==
                fixture.files[PACKAGE_LOADER_SUBSTITUTE].file_size &&
            rewrite_info.after_size == rewrite_info.before_size,
            "In-place rewrite changed the file size from %s to %s, "
            "graph size %s.\n",
            wine_dbgstr_longlong(rewrite_info.before_size),
            wine_dbgstr_longlong(rewrite_info.after_size),
            wine_dbgstr_longlong(
                fixture.files[PACKAGE_LOADER_SUBSTITUTE].file_size) );
        ok( rewrite_info.write_time_restored,
            "In-place rewrite did not restore LastWriteTime.\n" );
        stamp_changed =
            rewrite_info.before_change_time ==
                fixture.files[PACKAGE_LOADER_SUBSTITUTE].change_time &&
            rewrite_info.after_change_time != rewrite_info.before_change_time;
        ok( stamp_changed,
            "In-place rewrite ChangeTime was %s -> %s, graph ChangeTime %s.\n",
            wine_dbgstr_longlong(rewrite_info.before_change_time),
            wine_dbgstr_longlong(rewrite_info.after_change_time),
            wine_dbgstr_longlong(
                fixture.files[PACKAGE_LOADER_SUBSTITUTE].change_time) );
        if (identity_unchanged && stamp_changed &&
            rewrite_info.write_time_restored &&
            rewrite_info.after_size == rewrite_info.before_size)
        {
            result = run_package_loader_child(
                L"substitute", &fixture, TRUE );
            ok( !result, "In-place rewrite child failed %#lx.\n", result );
        }
    }

    ret = MoveFileExW(
        fixture.substitute_source,
        fixture.files[PACKAGE_LOADER_SUBSTITUTE].path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH );
    ok( ret, "Could not substitute %s, error %lu.\n",
        wine_dbgstr_w(fixture.files[PACKAGE_LOADER_SUBSTITUTE].path),
        GetLastError() );
    if (ret)
    {
        ret = get_file_identity(
            fixture.files[PACKAGE_LOADER_SUBSTITUTE].path,
            &volume_serial, &index_high, &index_low, NULL );
        ok( ret, "Could not query substitute identity, error %lu.\n",
            GetLastError() );
        ret = ret &&
            (volume_serial !=
                 fixture.files[PACKAGE_LOADER_SUBSTITUTE].volume_serial ||
             index_high !=
                 fixture.files[PACKAGE_LOADER_SUBSTITUTE].file_index_high ||
             index_low !=
                 fixture.files[PACKAGE_LOADER_SUBSTITUTE].file_index_low);
        ok( ret, "Substitute unexpectedly retained the graph identity.\n" );
        if (ret)
        {
            result = run_package_loader_child(
                L"substitute", &fixture, TRUE );
            ok( !result, "Substitution child failed %#lx.\n", result );
        }
    }

    ret = truncate_file_in_place(
        fixture.files[PACKAGE_LOADER_MISSING].path,
        fixture.files[PACKAGE_LOADER_MISSING].file_size / 2 );
    ok( ret, "Could not truncate %s in place, error %lu.\n",
        wine_dbgstr_w(fixture.files[PACKAGE_LOADER_MISSING].path),
        GetLastError() );
    if (ret)
    {
        ret = get_file_identity(
            fixture.files[PACKAGE_LOADER_MISSING].path,
            &volume_serial, &index_high, &index_low, NULL );
        ok( ret, "Could not query truncated identity, error %lu.\n",
            GetLastError() );
        identity_unchanged = ret &&
            volume_serial ==
                fixture.files[PACKAGE_LOADER_MISSING].volume_serial &&
            index_high ==
                fixture.files[PACKAGE_LOADER_MISSING].file_index_high &&
            index_low ==
                fixture.files[PACKAGE_LOADER_MISSING].file_index_low;
        ok( identity_unchanged,
            "In-place truncation changed the graph file identity.\n" );
        ret = get_file_stamp(
            fixture.files[PACKAGE_LOADER_MISSING].path,
            &change_time, &file_size );
        ok( ret, "Could not query truncated stamp, error %lu.\n",
            GetLastError() );
        stamp_changed = ret &&
            change_time !=
                fixture.files[PACKAGE_LOADER_MISSING].change_time &&
            file_size <
                fixture.files[PACKAGE_LOADER_MISSING].file_size;
        ok( stamp_changed,
            "Truncated stamp is ChangeTime %s, size %s; "
            "graph ChangeTime %s, size %s.\n",
            wine_dbgstr_longlong(change_time),
            wine_dbgstr_longlong(file_size),
            wine_dbgstr_longlong(
                fixture.files[PACKAGE_LOADER_MISSING].change_time),
            wine_dbgstr_longlong(
                fixture.files[PACKAGE_LOADER_MISSING].file_size) );
        if (identity_unchanged && stamp_changed)
        {
            result = run_package_loader_child(
                L"truncate", &fixture, TRUE );
            ok( !result, "Truncation child failed %#lx.\n", result );
        }
    }

    ret = MoveFileW( fixture.files[PACKAGE_LOADER_MISSING].path,
                     fixture.missing_saved );
    ok( ret, "Could not move the missing-file fixture, error %lu.\n",
        GetLastError() );
    if (ret)
    {
        result = run_package_loader_child( L"missing", &fixture, TRUE );
        ok( !result, "Missing-file child failed %#lx.\n", result );
    }

    ret = DeleteFileW( fixture.files[PACKAGE_LOADER_REPARSE].path );
    ok( ret, "Could not remove the reparse fixture, error %lu.\n",
        GetLastError() );
    if (ret)
    {
        SetLastError( ERROR_SUCCESS );
        ret = CreateSymbolicLinkW(
            fixture.files[PACKAGE_LOADER_REPARSE].path,
            fixture.reparse_target, 0 );
        error = GetLastError();
        if (!ret && (error == ERROR_PRIVILEGE_NOT_HELD ||
                     error == ERROR_NOT_SUPPORTED ||
                     error == ERROR_CALL_NOT_IMPLEMENTED))
            win_skip( "Symbolic links are unavailable, error %lu.\n", error );
        else
        {
            ok( ret, "Could not create the reparse fixture, error %lu.\n",
                error );
            if (ret)
            {
                result = run_package_loader_child(
                    L"reparse", &fixture, TRUE );
                ok( !result, "Reparse child failed %#lx.\n", result );
            }
        }
    }

    delete_package_loader_fixture( &fixture );
}

START_TEST(loader)
{
    char **argv;
    int argc = winetest_get_mainargs( &argv );

    if (argc >= 4 && !strcmp( argv[2], "package-child" ))
        ExitProcess( package_loader_child( argc, argv ) );

    test_package_graph_loader();
#ifdef _WIN64
    test_cef_loader_hooks();
#else
    skip("The CEF compatibility hook is only available to 64-bit processes.\n");
#endif
}
