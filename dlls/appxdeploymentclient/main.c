/* WinRT Windows.Management.Deployment Implementation
 *
 * Copyright (C) 2023 Mohamad Al-Jaf
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

#include "initguid.h"
#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out );
    if (out) *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *buffer;
    const WCHAR *package_manager_name =
        RuntimeClass_Windows_Management_Deployment_PackageManager;
    UINT32 classid_length, package_manager_name_length;

    TRACE( "class %s, factory %p.\n", debugstr_hstring(classid), factory );

    if (!factory) return E_POINTER;
    *factory = NULL;
    if (!classid) return CLASS_E_CLASSNOTAVAILABLE;
    buffer = WindowsGetStringRawBuffer( classid, &classid_length );
    package_manager_name_length = wcslen( package_manager_name );

    if (classid_length == package_manager_name_length &&
        CompareStringOrdinal(
            buffer, classid_length, package_manager_name,
            package_manager_name_length, FALSE ) == CSTR_EQUAL)
        IActivationFactory_QueryInterface( package_manager_factory, &IID_IActivationFactory, (void **)factory );

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}
