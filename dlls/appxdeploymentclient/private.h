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

#ifndef __WINE_WINDOWS_MANAGEMENT_DEPLOYMENT_PRIVATE_H
#define __WINE_WINDOWS_MANAGEMENT_DEPLOYMENT_PRIVATE_H

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winstring.h"

#include "activation.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_System
#define WIDL_using_Windows_Storage
#include "windows.foundation.h"
#define WIDL_using_Windows_Management_Deployment
#define WIDL_using_Windows_ApplicationModel
#include "windows.management.deployment.h"

#include "../appxsvc/deployment.h"

#define APPX_STORE_ROOT L"C:\\Program Files\\WindowsApps\\.wine-msix-store"

static inline HRESULT appxclient_validate_utf16_buffer(
    const WCHAR *buffer, UINT32 length, BOOL allow_empty )
{
    UINT32 i;

    if (!buffer || (!allow_empty && !length) ||
        length > WINE_APPX_MAX_PATH_CHARS)
        return E_INVALIDARG;
    for (i = 0; i < length; i++)
    {
        WCHAR ch = buffer[i];

        if (!ch) return E_INVALIDARG;
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            if (++i >= length || buffer[i] < 0xdc00 ||
                buffer[i] > 0xdfff)
                return E_INVALIDARG;
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff)
            return E_INVALIDARG;
    }
    return S_OK;
}

static inline HRESULT appxclient_bounded_wstring_length(
    const WCHAR *buffer, BOOL allow_empty, UINT32 *length )
{
    UINT32 i;

    if (!buffer || !length) return E_INVALIDARG;
    for (i = 0; i <= WINE_APPX_MAX_PATH_CHARS; i++)
    {
        WCHAR ch = buffer[i];

        if (!ch)
        {
            if (!allow_empty && !i) return E_INVALIDARG;
            *length = i;
            return S_OK;
        }
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            if (i == WINE_APPX_MAX_PATH_CHARS ||
                buffer[++i] < 0xdc00 || buffer[i] > 0xdfff)
                return E_INVALIDARG;
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff)
            return E_INVALIDARG;
    }
    return E_INVALIDARG;
}

extern IActivationFactory *package_manager_factory;

enum package_deployment_operation
{
    PACKAGE_DEPLOYMENT_INSTALL,
    PACKAGE_DEPLOYMENT_UPDATE,
    PACKAGE_DEPLOYMENT_REMOVE,
};

struct package_query_filter
{
    const WCHAR *name;
    const WCHAR *publisher;
    const WCHAR *family_name;
    PackageTypes types;
};

void package_deployment_options_init( APPX_DEPLOYMENT_OPTIONS *options,
                                      UINT32 flags, HANDLE cancel_event );
HRESULT package_async_operation_create(
    enum package_deployment_operation operation, HANDLE package_file,
    const WCHAR *full_name, UINT32 deployment_flags,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **value );
HRESULT package_iterable_create( APPX_CATALOG_SNAPSHOT *catalog,
                                 const struct package_query_filter *filter,
                                 IIterable_Package **value );
HRESULT package_from_catalog_create( APPX_CATALOG_SNAPSHOT *catalog,
                                     UINT32 index, IPackage **value );

HRESULT appx_backend_deployment_initialize(
    const APPX_DEPLOYMENT_OPTIONS *options, APPX_DEPLOYMENT_RESULT **result );
HRESULT appx_backend_deployment_install(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result );
HRESULT appx_backend_deployment_update(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result );
HRESULT appx_backend_deployment_remove(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result );
HRESULT appx_backend_deployment_query(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_CATALOG_SNAPSHOT **snapshot );
void appx_backend_deployment_result_free( APPX_DEPLOYMENT_RESULT *result );
void appx_backend_catalog_snapshot_free( APPX_CATALOG_SNAPSHOT *snapshot );
UINT32 appx_backend_catalog_snapshot_get_package_count(
    const APPX_CATALOG_SNAPSHOT *snapshot );
const struct appx_catalog_package *
appx_backend_catalog_snapshot_get_package(
    const APPX_CATALOG_SNAPSHOT *snapshot, UINT32 index );
HRESULT appx_backend_resolve_direct_dependencies(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *package_full_name,
    enum appx_catalog_architecture target_architecture, UINT32 capacity,
    UINT32 *package_indices, UINT32 *count );

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )

#endif
