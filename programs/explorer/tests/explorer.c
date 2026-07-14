/*
 * Explorer.exe tests
 *
 * Copyright 2022 Zhiyi Zhang for CodeWeavers
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

#include <windows.h>
#include <shellapi.h>
#include "wine/test.h"

struct icon_owner_context
{
    HANDLE ready;
    HANDLE quit;
    HWND hwnd;
    BOOL added;
    DWORD error;
};

static DWORD CALLBACK icon_owner_thread(void *arg)
{
    struct icon_owner_context *context = arg;
    NOTIFYICONDATAW nid = {.cbSize = sizeof(nid)};

    context->hwnd = CreateWindowExW( 0, L"Static", NULL, 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, NULL, NULL, NULL );
    nid.hWnd = context->hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON;
    nid.uCallbackMessage = WM_USER;
    nid.hIcon = LoadIconW( NULL, (const WCHAR *)IDI_APPLICATION );
    context->added = Shell_NotifyIconW( NIM_ADD, &nid );
    context->error = GetLastError();
    SetEvent( context->ready );
    WaitForSingleObject( context->quit, 5000 );
    return 0;
}

static void test_taskbar(void)
{
    RECT taskbar_rect, expected_rect, primary_rect, work_rect;
    HWND hwnd;
    BOOL ret;

    hwnd = FindWindowA("Shell_TrayWnd", NULL);
    if (!hwnd)
    {
        skip("Failed to find the taskbar window.\n");
        return;
    }

    ret = GetWindowRect(hwnd, &taskbar_rect);
    ok(ret, "GetWindowRect failed, error %ld.\n", GetLastError());

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rect, 0);
    SetRect(&primary_rect, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    SubtractRect(&expected_rect, &primary_rect, &work_rect);

    /* In standalone mode, the systray window is floating */
    todo_wine_if(!(GetWindowLongW(hwnd, GWL_STYLE) & WS_POPUP))
    ok(EqualRect(&taskbar_rect, &expected_rect), "Expected %s, got %s.\n",
       wine_dbgstr_rect(&expected_rect), wine_dbgstr_rect(&taskbar_rect));
}

static void test_message_only_icon_cleanup(void)
{
    struct icon_owner_context context = {0};
    NOTIFYICONDATAW nid = {.cbSize = sizeof(nid)};
    HANDLE thread;
    DWORD wait;
    BOOL ret = TRUE;
    unsigned int i;

    if (!FindWindowA( "Shell_TrayWnd", NULL ))
    {
        skip( "Failed to find the taskbar window.\n" );
        return;
    }

    context.ready = CreateEventW( NULL, TRUE, FALSE, NULL );
    context.quit = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!context.ready && !!context.quit, "Failed to create events, error %lu.\n", GetLastError() );
    if (!context.ready || !context.quit) goto done;

    thread = CreateThread( NULL, 0, icon_owner_thread, &context, 0, NULL );
    ok( !!thread, "Failed to create icon owner thread, error %lu.\n", GetLastError() );
    if (!thread) goto done;

    wait = WaitForSingleObject( context.ready, 5000 );
    ok( wait == WAIT_OBJECT_0, "Icon owner did not become ready, wait result %#lx.\n", wait );
    ok( !!context.hwnd, "Failed to create message-only owner window.\n" );
    ok( context.added, "Failed to add notification icon, error %lu.\n", context.error );

    nid.hWnd = context.hwnd;
    nid.uID = 1;
    ret = Shell_NotifyIconW( NIM_MODIFY, &nid );
    ok( ret, "Failed to find notification icon while its owner was alive.\n" );

    SetEvent( context.quit );
    wait = WaitForSingleObject( thread, 5000 );
    ok( wait == WAIT_OBJECT_0, "Icon owner thread did not exit, wait result %#lx.\n", wait );

    if (context.added)
    {
        for (i = 0; i < 100; ++i)
        {
            if (!(ret = Shell_NotifyIconW( NIM_MODIFY, &nid ))) break;
            Sleep( 10 );
        }
        ok( !ret, "Notification icon still exists after its message-only owner was destroyed.\n" );
        if (ret) Shell_NotifyIconW( NIM_DELETE, &nid );
    }

    CloseHandle( thread );

done:
    if (context.quit) SetEvent( context.quit );
    if (context.ready) CloseHandle( context.ready );
    if (context.quit) CloseHandle( context.quit );
}

START_TEST(explorer)
{
    test_taskbar();
    test_message_only_icon_cleanup();
}
