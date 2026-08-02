/*
 * Package graph forwarded-export fixture
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

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
