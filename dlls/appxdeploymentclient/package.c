/* WinRT Windows.Management.Deployment.PackageManager Implementation
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

#include "private.h"

#include "sddl.h"
#include "shlwapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

#define SUPPORTED_PACKAGE_TYPES \
    (PackageTypes_Main | PackageTypes_Framework | PackageTypes_Resource)

static HRESULT copy_iids( const IID *source, ULONG count, ULONG *iid_count,
                          IID **iids )
{
    IID *copy;

    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    if (!(copy = CoTaskMemAlloc( count * sizeof(*copy) )))
        return E_OUTOFMEMORY;
    memcpy( copy, source, count * sizeof(*copy) );
    *iid_count = count;
    *iids = copy;
    return S_OK;
}

static HRESULT runtime_class_name( const WCHAR *name, HSTRING *value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return WindowsCreateString( name, wcslen(name), value );
}

static HRESULT get_base_trust( TrustLevel *value )
{
    if (!value) return E_POINTER;
    *value = BaseTrust;
    return S_OK;
}

static HRESULT unsupported_mode(void)
{
    return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
}

static HRESULT get_package_hstring( HSTRING string, BOOL allow_empty,
                                    const WCHAR **value )
{
    const WCHAR *buffer;
    UINT32 length;
    HRESULT hr;

    if (!string) return E_INVALIDARG;
    buffer = WindowsGetStringRawBuffer( string, &length );
    if (FAILED( hr = appxclient_validate_utf16_buffer(
            buffer, length, allow_empty ) ))
        return hr;
    *value = buffer;
    return S_OK;
}

static HRESULT check_empty_uri_iterable( IIterable_Uri *iterable )
{
    IIterator_Uri *iterator = NULL;
    boolean has_current = FALSE;
    HRESULT hr;

    if (!iterable) return S_OK;
    if (FAILED( hr = IIterable_Uri_First( iterable, &iterator ) ))
        return hr;
    if (!iterator) return E_UNEXPECTED;
    hr = IIterator_Uri_get_HasCurrent( iterator, &has_current );
    IIterator_Uri_Release( iterator );
    if (FAILED(hr)) return hr;
    return has_current ? unsupported_mode() : S_OK;
}

static HRESULT get_uri_property(
    IUriRuntimeClass *uri,
    HRESULT (*getter)(IUriRuntimeClass *, HSTRING *),
    HSTRING *value )
{
    HRESULT hr;

    *value = NULL;
    if (FAILED( hr = getter( uri, value ) )) return hr;
    return S_OK;
}

static HRESULT require_empty_uri_property(
    IUriRuntimeClass *uri,
    HRESULT (*getter)(IUriRuntimeClass *, HSTRING *) )
{
    HSTRING value;
    UINT32 length;
    HRESULT hr;

    if (FAILED( hr = get_uri_property( uri, getter, &value ) ))
        return hr;
    WindowsGetStringRawBuffer( value, &length );
    WindowsDeleteString( value );
    return length ? E_INVALIDARG : S_OK;
}

static BOOL absolute_uri_has_nul_escape(
    const WCHAR *buffer, UINT32 length )
{
    UINT32 i;

    for (i = 0; i + 2 < length; i++)
        if (buffer[i] == '%' && buffer[i + 1] == '0' &&
            buffer[i + 2] == '0')
            return TRUE;
    for (i = 0; i + 5 < length; i++)
        if (buffer[i] == '%' &&
            (buffer[i + 1] == 'u' || buffer[i + 1] == 'U') &&
            buffer[i + 2] == '0' && buffer[i + 3] == '0' &&
            buffer[i + 4] == '0' && buffer[i + 5] == '0')
            return TRUE;
    return FALSE;
}

static HRESULT open_local_package_uri( IUriRuntimeClass *uri, HANDLE *value )
{
    HSTRING scheme = NULL, host = NULL, absolute = NULL;
    const WCHAR *buffer;
    WCHAR *path = NULL, *canonical = NULL, *canonical_uri = NULL;
    BY_HANDLE_FILE_INFORMATION info;
    UINT32 length;
    DWORD capacity, result, uri_capacity;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;
    UINT32 i;

    if (!value) return E_POINTER;
    *value = INVALID_HANDLE_VALUE;
    if (!uri) return E_INVALIDARG;
    if (FAILED( hr = get_uri_property(
            uri, IUriRuntimeClass_get_SchemeName, &scheme ) ))
        goto done;
    buffer = WindowsGetStringRawBuffer( scheme, &length );
    if (length != 4 || CompareStringOrdinal(
            buffer, length, L"file", 4, TRUE ) != CSTR_EQUAL)
    {
        hr = E_INVALIDARG;
        goto done;
    }
    if (FAILED( hr = get_uri_property(
            uri, IUriRuntimeClass_get_Host, &host ) ))
        goto done;
    WindowsGetStringRawBuffer( host, &length );
    if (length)
    {
        hr = E_INVALIDARG;
        goto done;
    }
    if (FAILED( hr = require_empty_uri_property(
            uri, IUriRuntimeClass_get_UserName ) ) ||
        FAILED( hr = require_empty_uri_property(
            uri, IUriRuntimeClass_get_Password ) ) ||
        FAILED( hr = require_empty_uri_property(
            uri, IUriRuntimeClass_get_Query ) ) ||
        FAILED( hr = require_empty_uri_property(
            uri, IUriRuntimeClass_get_Fragment ) ))
        goto done;
    if (FAILED( hr = get_uri_property(
            uri, IUriRuntimeClass_get_AbsoluteUri, &absolute ) ))
        goto done;
    buffer = WindowsGetStringRawBuffer( absolute, &length );
    if (FAILED( hr = appxclient_validate_utf16_buffer(
            buffer, length, FALSE ) ) ||
        length < 8 ||
        CompareStringOrdinal(
            buffer, 8, L"file:///", 8, TRUE ) != CSTR_EQUAL ||
        absolute_uri_has_nul_escape( buffer, length ))
    {
        hr = E_INVALIDARG;
        goto done;
    }
    for (i = 8; i < length; i++)
        if (buffer[i] == '?' || buffer[i] == '#' || buffer[i] == '\\' ||
            buffer[i] < 0x20 || buffer[i] == 0x7f)
        {
            hr = E_INVALIDARG;
            goto done;
        }
    if (!(path = malloc( (length + 3) * sizeof(*path) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    capacity = length + 3;
    if (FAILED( hr = PathCreateFromUrlW(
            buffer, path, &capacity, 0 ) ))
    {
        hr = E_INVALIDARG;
        goto done;
    }
    if (capacity < 3 || path[1] != ':' ||
        (path[2] != '\\' && path[2] != '/') ||
        path[0] == '\\' || path[0] == '/' ||
        !((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')))
    {
        hr = E_INVALIDARG;
        goto done;
    }
    for (i = 2; path[i]; i++)
    {
        if (path[i] == ':')
        {
            hr = E_INVALIDARG;
            goto done;
        }
    }
    for (i = 2; path[i]; i++)
        if (path[i] == '/') path[i] = '\\';
    result = GetFullPathNameW( path, 0, NULL, NULL );
    if (!result || result > WINE_APPX_MAX_PATH_CHARS)
    {
        hr = E_INVALIDARG;
        goto done;
    }
    if (!(canonical = malloc( result * sizeof(*canonical) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    capacity = result;
    result = GetFullPathNameW( path, capacity, canonical, NULL );
    if (!result || result >= capacity)
    {
        hr = E_INVALIDARG;
        goto done;
    }
    if (canonical[1] != ':' ||
        (canonical[2] != '\\' && canonical[2] != '/') ||
        result != wcslen(path) ||
        CompareStringOrdinal(
            canonical, result, path, result, TRUE ) != CSTR_EQUAL)
    {
        hr = E_INVALIDARG;
        goto done;
    }
    if (!(canonical_uri = malloc(
            (WINE_APPX_MAX_PATH_CHARS + 1) * sizeof(*canonical_uri) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    uri_capacity = WINE_APPX_MAX_PATH_CHARS + 1;
    if (UrlCreateFromPathW(
            canonical, canonical_uri, &uri_capacity, 0 ) != S_OK ||
        uri_capacity != length ||
        CompareStringOrdinal(
            canonical_uri, 8, buffer, 8, TRUE ) != CSTR_EQUAL ||
        CompareStringOrdinal(
            canonical_uri + 8, uri_capacity - 8,
            buffer + 8, length - 8, FALSE ) != CSTR_EQUAL)
    {
        hr = E_INVALIDARG;
        goto done;
    }
    file = CreateFileW(
        canonical, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
        FILE_FLAG_SEQUENTIAL_SCAN, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (!GetFileInformationByHandle( file, &info ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (info.dwFileAttributes &
        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
    {
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
        goto done;
    }
    *value = file;
    file = INVALID_HANDLE_VALUE;
    hr = S_OK;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    free( canonical_uri );
    free( canonical );
    free( path );
    WindowsDeleteString( absolute );
    WindowsDeleteString( host );
    WindowsDeleteString( scheme );
    return hr;
}

static HRESULT validate_package_types( PackageTypes types,
                                       PackageTypes *normalized )
{
    UINT32 mask = types;

    if (mask == (UINT32)PackageTypes_All)
        mask = SUPPORTED_PACKAGE_TYPES;
    if (mask & ~(UINT32)SUPPORTED_PACKAGE_TYPES)
        return unsupported_mode();
    *normalized = (PackageTypes)mask;
    return S_OK;
}

static HRESULT validate_current_user_sid( HSTRING sid )
{
    TOKEN_USER *user = NULL;
    const WCHAR *requested;
    PSID requested_sid = NULL;
    HANDLE token = NULL;
    DWORD size = 0;
    UINT32 length;
    HRESULT hr = S_OK;

    requested = WindowsGetStringRawBuffer( sid, &length );
    if (!length) return S_OK;
    if (FAILED( hr = appxclient_validate_utf16_buffer(
            requested, length, FALSE ) ))
        return hr;
    if (!ConvertStringSidToSidW( requested, &requested_sid ))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    GetTokenInformation( token, TokenUser, NULL, 0, &size );
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (!(user = malloc( size )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!GetTokenInformation( token, TokenUser, user, size, &size ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (!EqualSid( requested_sid, user->User.Sid ))
        hr = unsupported_mode();

done:
    LocalFree( requested_sid );
    free( user );
    if (token) CloseHandle( token );
    return hr;
}

static HRESULT query_catalog( const WCHAR *full_name,
                              APPX_CATALOG_SNAPSHOT **catalog )
{
    APPX_DEPLOYMENT_OPTIONS options;

    *catalog = NULL;
    package_deployment_options_init( &options, 0, NULL );
    return appx_backend_deployment_query( full_name, &options, catalog );
}

static HRESULT find_packages( const struct package_query_filter *filter,
                              IIterable_Package **packages )
{
    APPX_CATALOG_SNAPSHOT *catalog = NULL;
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = query_catalog( NULL, &catalog ) )) return hr;
    return package_iterable_create( catalog, filter, packages );
}

struct package_manager
{
    IPackageManager IPackageManager_iface;
    IPackageManager2 IPackageManager2_iface;
    LONG ref;
};

static inline struct package_manager *impl_from_IPackageManager( IPackageManager *iface )
{
    return CONTAINING_RECORD( iface, struct package_manager, IPackageManager_iface );
}

static HRESULT WINAPI package_manager_QueryInterface( IPackageManager *iface, REFIID iid, void **out )
{
    struct package_manager *impl = impl_from_IPackageManager( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IPackageManager ))
    {
        *out = &impl->IPackageManager_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IPackageManager2 ))
    {
        *out = &impl->IPackageManager2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI package_manager_AddRef( IPackageManager *iface )
{
    struct package_manager *impl = impl_from_IPackageManager( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI package_manager_Release( IPackageManager *iface )
{
    struct package_manager *impl = impl_from_IPackageManager( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI package_manager_GetIids( IPackageManager *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IPackageManager, IID_IPackageManager2};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_manager_GetRuntimeClassName( IPackageManager *iface, HSTRING *class_name )
{
    return runtime_class_name(
        RuntimeClass_Windows_Management_Deployment_PackageManager,
        class_name );
}

static HRESULT WINAPI package_manager_GetTrustLevel( IPackageManager *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI package_manager_AddPackageAsync( IPackageManager *iface, IUriRuntimeClass *uri,
    IIterable_Uri *dependencies, DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    HANDLE package_file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    TRACE( "iface %p, uri %p, dependencies %p, options %#x, operation %p.\n",
           iface, uri, dependencies, options, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (options != DeploymentOptions_None)
        return options & DeploymentOptions_DevelopmentMode ?
               E_INVALIDARG : unsupported_mode();
    if (FAILED( hr = check_empty_uri_iterable( dependencies ) ) ||
        FAILED( hr = open_local_package_uri( uri, &package_file ) ))
        return hr;
    return package_async_operation_create(
        PACKAGE_DEPLOYMENT_INSTALL, package_file, NULL, 0, operation );
}

static HRESULT WINAPI package_manager_UpdatePackageAsync( IPackageManager *iface, IUriRuntimeClass *uri, IIterable_Uri *dependencies,
    DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    const UINT32 supported = DeploymentOptions_ForceUpdateFromAnyVersion;
    HANDLE package_file = INVALID_HANDLE_VALUE;
    UINT32 deployment_flags = 0;
    HRESULT hr;

    TRACE( "iface %p, uri %p, dependencies %p, options %#x, operation %p.\n",
           iface, uri, dependencies, options, operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if ((UINT32)options & ~supported) return unsupported_mode();
    if (options & DeploymentOptions_ForceUpdateFromAnyVersion)
        deployment_flags |= APPX_DEPLOYMENT_ALLOW_DOWNGRADE;
    if (FAILED( hr = check_empty_uri_iterable( dependencies ) ) ||
        FAILED( hr = open_local_package_uri( uri, &package_file ) ))
        return hr;
    return package_async_operation_create(
        PACKAGE_DEPLOYMENT_UPDATE, package_file, NULL,
        deployment_flags, operation );
}

static HRESULT WINAPI package_manager_RemovePackageAsync( IPackageManager *iface, HSTRING name,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    const WCHAR *full_name;
    HRESULT hr;

    TRACE( "iface %p, name %s, operation %p.\n",
           iface, debugstr_hstring(name), operation );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (FAILED( hr = get_package_hstring( name, FALSE, &full_name ) ))
        return hr;
    return package_async_operation_create(
        PACKAGE_DEPLOYMENT_REMOVE, INVALID_HANDLE_VALUE,
        full_name, 0, operation );
}

static HRESULT WINAPI package_manager_StagePackageAsync( IPackageManager *iface, IUriRuntimeClass *uri, IIterable_Uri *dependencies,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_RegisterPackageAsync( IPackageManager *iface, IUriRuntimeClass *uri, IIterable_Uri *dependencies,
    DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_FindPackages( IPackageManager *iface, IIterable_Package **packages )
{
    const struct package_query_filter filter = {
        .types = (PackageTypes)SUPPORTED_PACKAGE_TYPES,
    };
    return find_packages( &filter, packages );
}

static HRESULT WINAPI package_manager_FindPackagesByUserSecurityId( IPackageManager *iface, HSTRING sid, IIterable_Package **packages )
{
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = validate_current_user_sid( sid ) )) return hr;
    return package_manager_FindPackages( iface, packages );
}

static HRESULT WINAPI package_manager_FindPackagesByNamePublisher( IPackageManager *iface, HSTRING name, HSTRING publisher, IIterable_Package **packages )
{
    struct package_query_filter filter = {
        .types = (PackageTypes)SUPPORTED_PACKAGE_TYPES,
    };
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = get_package_hstring(
            name, TRUE, &filter.name ) ) ||
        FAILED( hr = get_package_hstring(
            publisher, TRUE, &filter.publisher ) ))
        return hr;
    return find_packages( &filter, packages );
}

static HRESULT WINAPI package_manager_FindPackagesByUserSecurityIdNamePublisher( IPackageManager *iface, HSTRING sid,
    HSTRING name, HSTRING publisher, IIterable_Package **packages )
{
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = validate_current_user_sid( sid ) )) return hr;
    return package_manager_FindPackagesByNamePublisher(
        iface, name, publisher, packages );
}

static HRESULT WINAPI package_manager_FindUsers( IPackageManager *iface, HSTRING name, IIterable_PackageUserInformation **users )
{
    if (!users) return E_POINTER;
    *users = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_SetPackageState( IPackageManager *iface, HSTRING name, PackageState state )
{
    FIXME("iface %p, name %s, state %d stub!\n", iface, debugstr_hstring(name), state);
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_FindPackageByPackageFullName( IPackageManager *iface, HSTRING name, IPackage **package )
{
    APPX_CATALOG_SNAPSHOT *catalog = NULL;
    const WCHAR *full_name;
    HRESULT hr;

    if (!package) return E_POINTER;
    *package = NULL;
    if (FAILED( hr = get_package_hstring( name, FALSE, &full_name ) ))
        return hr;
    hr = query_catalog( full_name, &catalog );
    if (hr == HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_NOT_FOUND))
        return S_OK;
    if (FAILED(hr)) return hr;
    return package_from_catalog_create( catalog, 0, package );
}

static HRESULT WINAPI package_manager_CleanupPackageForUserAsync( IPackageManager *iface, HSTRING name, HSTRING sid,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager_FindPackagesByPackageFamilyName( IPackageManager *iface, HSTRING family_name,
    IIterable_Package **packages )
{
    struct package_query_filter filter = {
        .types = (PackageTypes)SUPPORTED_PACKAGE_TYPES,
    };
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = get_package_hstring(
            family_name, TRUE, &filter.family_name ) ))
        return hr;
    return find_packages( &filter, packages );
}

static HRESULT WINAPI package_manager_FindPackagesByUserSecurityIdPackageFamilyName( IPackageManager *iface, HSTRING sid,
    HSTRING family_name, IIterable_Package **packages )
{
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = validate_current_user_sid( sid ) )) return hr;
    return package_manager_FindPackagesByPackageFamilyName(
        iface, family_name, packages );
}

static HRESULT WINAPI package_manager_FindPackageByUserSecurityIdPackageFullName( IPackageManager *iface, HSTRING sid, HSTRING name, IPackage **package )
{
    HRESULT hr;

    if (!package) return E_POINTER;
    *package = NULL;
    if (FAILED( hr = validate_current_user_sid( sid ) )) return hr;
    return package_manager_FindPackageByPackageFullName( iface, name, package );
}

static const struct IPackageManagerVtbl package_manager_vtbl =
{
    package_manager_QueryInterface,
    package_manager_AddRef,
    package_manager_Release,
    /* IInspectable methods */
    package_manager_GetIids,
    package_manager_GetRuntimeClassName,
    package_manager_GetTrustLevel,
    /* IPackageManager methods */
    package_manager_AddPackageAsync,
    package_manager_UpdatePackageAsync,
    package_manager_RemovePackageAsync,
    package_manager_StagePackageAsync,
    package_manager_RegisterPackageAsync,
    package_manager_FindPackages,
    package_manager_FindPackagesByUserSecurityId,
    package_manager_FindPackagesByNamePublisher,
    package_manager_FindPackagesByUserSecurityIdNamePublisher,
    package_manager_FindUsers,
    package_manager_SetPackageState,
    package_manager_FindPackageByPackageFullName,
    package_manager_CleanupPackageForUserAsync,
    package_manager_FindPackagesByPackageFamilyName,
    package_manager_FindPackagesByUserSecurityIdPackageFamilyName,
    package_manager_FindPackageByUserSecurityIdPackageFullName
};

DEFINE_IINSPECTABLE( package_manager2, IPackageManager2, struct package_manager, IPackageManager_iface );

static HRESULT WINAPI package_manager2_RemovePackageWithOptionsAsync( IPackageManager2 *iface, HSTRING name, RemovalOptions options,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    struct package_manager *impl = impl_from_IPackageManager2( iface );

    if (!operation) return E_POINTER;
    *operation = NULL;
    if (options != RemovalOptions_None) return unsupported_mode();
    return package_manager_RemovePackageAsync(
        &impl->IPackageManager_iface, name, operation );
}

static HRESULT WINAPI package_manager2_StagePackageWithOptionsAsync( IPackageManager2 *iface, IUriRuntimeClass *uri, IIterable_Uri *dependencies,
    DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager2_RegisterPackageByFullNameAsync( IPackageManager2 *iface, HSTRING name, IIterable_HSTRING *dependencies,
    DeploymentOptions options, IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package_manager2_FindPackagesWithPackageTypes( IPackageManager2 *iface, PackageTypes types, IIterable_Package **packages )
{
    struct package_query_filter filter = {0};
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = validate_package_types( types, &filter.types ) ))
        return hr;
    return find_packages( &filter, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByUserSecurityIdWithPackageTypes( IPackageManager2 *iface, HSTRING sid,
    PackageTypes types, IIterable_Package **packages )
{
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = validate_current_user_sid( sid ) )) return hr;
    return package_manager2_FindPackagesWithPackageTypes(
        iface, types, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByNamePublisherWithPackageTypes( IPackageManager2 *iface, HSTRING name, HSTRING publisher,
    PackageTypes types, IIterable_Package **packages )
{
    struct package_query_filter filter = {0};
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = get_package_hstring(
            name, TRUE, &filter.name ) ) ||
        FAILED( hr = get_package_hstring(
            publisher, TRUE, &filter.publisher ) ))
        return hr;
    if (FAILED( hr = validate_package_types( types, &filter.types ) ))
        return hr;
    return find_packages( &filter, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByUserSecurityIdNamePublisherWithPackageTypes( IPackageManager2 *iface, HSTRING sid, HSTRING name,
    HSTRING publisher, PackageTypes types, IIterable_Package **packages )
{
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = validate_current_user_sid( sid ) )) return hr;
    return package_manager2_FindPackagesByNamePublisherWithPackageTypes(
        iface, name, publisher, types, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByPackageFamilyNameWithPackageTypes( IPackageManager2 *iface, HSTRING family_name, PackageTypes types,
   IIterable_Package **packages )
{
    struct package_query_filter filter = {0};
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = get_package_hstring(
            family_name, TRUE, &filter.family_name ) ))
        return hr;
    if (FAILED( hr = validate_package_types( types, &filter.types ) ))
        return hr;
    return find_packages( &filter, packages );
}

static HRESULT WINAPI package_manager2_FindPackagesByUserSecurityIdPackageFamilyNameWithPackageTypes( IPackageManager2 *iface, HSTRING sid, HSTRING family_name,
    PackageTypes types, IIterable_Package **packages )
{
    HRESULT hr;

    if (!packages) return E_POINTER;
    *packages = NULL;
    if (FAILED( hr = validate_current_user_sid( sid ) )) return hr;
    return package_manager2_FindPackagesByPackageFamilyNameWithPackageTypes(
        iface, family_name, types, packages );
}

static HRESULT WINAPI package_manager2_StageUserDataAsync( IPackageManager2 *iface, HSTRING name,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **operation )
{
    if (!operation) return E_POINTER;
    *operation = NULL;
    return E_NOTIMPL;
}

static const struct IPackageManager2Vtbl package_manager2_vtbl =
{
    package_manager2_QueryInterface,
    package_manager2_AddRef,
    package_manager2_Release,
    /* IInspectable methods */
    package_manager2_GetIids,
    package_manager2_GetRuntimeClassName,
    package_manager2_GetTrustLevel,
    /* IPackageManager2 methods */
    package_manager2_RemovePackageWithOptionsAsync,
    package_manager2_StagePackageWithOptionsAsync,
    package_manager2_RegisterPackageByFullNameAsync,
    package_manager2_FindPackagesWithPackageTypes,
    package_manager2_FindPackagesByUserSecurityIdWithPackageTypes,
    package_manager2_FindPackagesByNamePublisherWithPackageTypes,
    package_manager2_FindPackagesByUserSecurityIdNamePublisherWithPackageTypes,
    package_manager2_FindPackagesByPackageFamilyNameWithPackageTypes,
    package_manager2_FindPackagesByUserSecurityIdPackageFamilyNameWithPackageTypes,
    package_manager2_StageUserDataAsync,
};

struct package_manager_statics
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct package_manager_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct package_manager_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct package_manager_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct package_manager_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct package_manager_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IActivationFactory};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return runtime_class_name(
        RuntimeClass_Windows_Management_Deployment_PackageManager,
        class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct package_manager *impl;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    if (!instance) return E_POINTER;
    *instance = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) )))
        return E_OUTOFMEMORY;

    impl->IPackageManager_iface.lpVtbl = &package_manager_vtbl;
    impl->IPackageManager2_iface.lpVtbl = &package_manager2_vtbl;
    impl->ref = 1;

    *instance = (IInspectable *)&impl->IPackageManager_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

static struct package_manager_statics package_manager_statics =
{
    {&factory_vtbl},
    1,
};

IActivationFactory *package_manager_factory = &package_manager_statics.IActivationFactory_iface;
