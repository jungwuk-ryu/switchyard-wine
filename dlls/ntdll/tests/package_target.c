/*
 * Package graph loader target
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep testdll
#endif

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"

#define PACKAGE_TARGET_VALUE 0x51a7c0de
#define PACKAGE_INITIAL_IMPORT_VALUE 0x1a17c0de

DWORD WINAPI package_value(void)
{
    return PACKAGE_TARGET_VALUE;
}

DWORD WINAPI GetFileVersionInfoSizeW( const WCHAR *filename, DWORD *handle )
{
    if (handle) *handle = 0;
    return filename ? PACKAGE_INITIAL_IMPORT_VALUE : 0;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
