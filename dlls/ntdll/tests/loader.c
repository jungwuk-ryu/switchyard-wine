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

#include <string.h>

#include "windows.h"
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

START_TEST(loader)
{
#ifdef _WIN64
    test_cef_loader_hooks();
#else
    win_skip("The CEF compatibility hook is only available to 64-bit processes.\n");
#endif
}
