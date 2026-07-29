/*
 * Fake CEF library for ntdll loader tests
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

#if 0
#pragma makedep testdll
#endif

#include <string.h>

#include "windef.h"

enum
{
    cef_window_info_size = 96,
    cef_argument_count = 5
};

static BYTE last_window_info[cef_window_info_size];
static void *last_arguments[cef_argument_count];
static const char *platform_api_hash =
    "4150bd26e7bf639a9b1f3e5860af8c76eeae8570";
static const char *universal_api_hash =
    "d026196d35d8894a836ab3a3d033b84195cdb835";
static unsigned int call_count;

const char *__cdecl cef_api_hash(int entry)
{
    if (entry == 0) return platform_api_hash;
    if (entry == 1) return universal_api_hash;
    return NULL;
}

int __cdecl cef_initialize(const void *args, const void *settings,
        void *application, void *sandbox_info)
{
    (void)args;
    (void)settings;
    (void)application;
    (void)sandbox_info;
    return TRUE;
}

void *__cdecl cef_browser_host_create_browser_sync(const void *window_info,
        void *client, const void *url, const void *settings, void *extra_info,
        void *request_context)
{
    memset(last_window_info, 0, sizeof(last_window_info));
    if (window_info)
        memcpy(last_window_info, window_info, sizeof(last_window_info));

    last_arguments[0] = client;
    last_arguments[1] = (void *)url;
    last_arguments[2] = (void *)settings;
    last_arguments[3] = extra_info;
    last_arguments[4] = request_context;
    ++call_count;

    return client;
}

unsigned int __cdecl cef_test_get_last_call(void *window_info,
        unsigned int size, void **arguments)
{
    if (size > sizeof(last_window_info)) size = sizeof(last_window_info);
    if (window_info) memcpy(window_info, last_window_info, size);
    if (arguments)
        memcpy(arguments, last_arguments, sizeof(last_arguments));
    return call_count;
}

void __cdecl cef_test_set_hash(int valid)
{
    platform_api_hash = valid ?
        "4150bd26e7bf639a9b1f3e5860af8c76eeae8570" :
        "0000000000000000000000000000000000000000";
    universal_api_hash = valid ?
        "d026196d35d8894a836ab3a3d033b84195cdb835" :
        "0000000000000000000000000000000000000000";
}
