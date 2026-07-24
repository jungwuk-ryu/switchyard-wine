/*
 * Copyright 2026 Switchyard Wine project
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

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "d3d12.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d12);

UINT D3D12SDKVersion = D3D12_SDK_VERSION;

HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object)
{
    PFN_D3D12_GET_INTERFACE get_interface;
    HMODULE module;

    TRACE("clsid %s, iid %s, object %p.\n",
            debugstr_guid(clsid), debugstr_guid(iid), object);

    if (!(module = GetModuleHandleW(L"d3d12.dll"))
            || !(get_interface = (void *)GetProcAddress(module, "D3D12GetInterface")))
    {
        if (object)
            *object = NULL;
        return object ? E_NOINTERFACE : E_POINTER;
    }

    return get_interface(clsid, iid, object);
}
