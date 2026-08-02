/*
 * Windows.ApplicationModel.Package implementation
 *
 * Copyright (C) 2023 Mohamad Al-Jaf
 * Copyright (C) 2026 Jungwuk Ryu
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

#include "appmodel.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(model);

#define PACKAGE_FILTER_HEAD       0x00000010
#define PACKAGE_FILTER_DIRECT     0x00000020
#define PACKAGE_FILTER_STATIC     0x00080000

#define PACKAGE_PROPERTY_FRAMEWORK 0x00000001
#define PACKAGE_PROPERTY_RESOURCE  0x00000002
#define PACKAGE_PROPERTY_STATIC    0x00080000
#define PACKAGE_PROPERTY_KNOWN     \
    (PACKAGE_PROPERTY_FRAMEWORK | PACKAGE_PROPERTY_RESOURCE | PACKAGE_PROPERTY_STATIC)

#define PACKAGE_INFO_LIMIT (32u * 1024u * 1024u)
#define PACKAGE_COUNT_LIMIT 256u

/*
 * These declarations are intentionally local until Wine's public appmodel.h
 * grows the Windows 8 package-information API declarations.  The layout is
 * the documented PACKAGE_INFO ABI.
 */
struct package_info_native
{
    UINT32 reserved;
    UINT32 flags;
    WCHAR *path;
    WCHAR *package_full_name;
    WCHAR *package_family_name;
    PACKAGE_ID package_id;
};

LONG WINAPI GetCurrentPackageInfo( UINT32 flags, UINT32 *buffer_size, BYTE *buffer,
                                   UINT32 *count );

struct package_data
{
    HSTRING name;
    HSTRING publisher;
    HSTRING resource_id;
    HSTRING publisher_id;
    HSTRING full_name;
    HSTRING family_name;
    HSTRING root;
    PackageVersion version;
    ProcessorArchitecture architecture;
    UINT32 flags;
    boolean direct;
};

struct package_snapshot
{
    LONG ref;
    UINT32 count;
    struct package_data *packages;
};

struct package
{
    IPackage IPackage_iface;
    IPackage2 IPackage2_iface;
    IPackage3 IPackage3_iface;
    LONG ref;
    struct package_snapshot *snapshot;
    UINT32 index;
};

static const IPackageVtbl package_vtbl;
static HRESULT package_create( struct package_snapshot *snapshot, UINT32 index,
                               IPackage **value );
static HRESULT package_vector_create( struct package_snapshot *snapshot,
                                      IVectorView_Package **value );

static HRESULT copy_iids( const IID *source, ULONG count, ULONG *iid_count, IID **iids )
{
    IID *copy;

    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    if (!(copy = CoTaskMemAlloc( count * sizeof(*copy) ))) return E_OUTOFMEMORY;
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

static HRESULT unavailable_hstring( HSTRING *value )
{
    if (!value) return E_INVALIDARG;
    *value = NULL;
    return E_NOTIMPL;
}

static void package_data_destroy( struct package_data *data )
{
    WindowsDeleteString( data->name );
    WindowsDeleteString( data->publisher );
    WindowsDeleteString( data->resource_id );
    WindowsDeleteString( data->publisher_id );
    WindowsDeleteString( data->full_name );
    WindowsDeleteString( data->family_name );
    WindowsDeleteString( data->root );
}

static struct package_snapshot *package_snapshot_addref( struct package_snapshot *snapshot )
{
    InterlockedIncrement( &snapshot->ref );
    return snapshot;
}

static void package_snapshot_release( struct package_snapshot *snapshot )
{
    UINT32 i;

    if (InterlockedDecrement( &snapshot->ref )) return;
    for (i = 0; i < snapshot->count; i++) package_data_destroy( &snapshot->packages[i] );
    free( snapshot->packages );
    free( snapshot );
}

static HRESULT hstring_from_package_info( const BYTE *buffer, UINT32 size,
                                          const WCHAR *string, boolean allow_empty,
                                          HSTRING *value )
{
    ULONG_PTR start = (ULONG_PTR)buffer;
    ULONG_PTR address = (ULONG_PTR)string;
    ULONG_PTR end = start + size;
    UINT32 chars, limit;

    *value = NULL;
    if (!string || end < start || address < start || address >= end ||
        (address & (sizeof(WCHAR) - 1)))
        return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
    limit = (end - address) / sizeof(WCHAR);
    for (chars = 0; chars < limit && string[chars]; chars++);
    if (chars == limit || (!allow_empty && !chars))
        return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT );
    return WindowsCreateString( string, chars, value );
}

static boolean valid_processor_architecture( UINT32 architecture )
{
    return architecture == PROCESSOR_ARCHITECTURE_INTEL ||
           architecture == PROCESSOR_ARCHITECTURE_ARM ||
           architecture == PROCESSOR_ARCHITECTURE_AMD64 ||
           architecture == PROCESSOR_ARCHITECTURE_NEUTRAL ||
           architecture == PROCESSOR_ARCHITECTURE_ARM64 ||
           architecture == PROCESSOR_ARCHITECTURE_IA32_ON_ARM64;
}

static HRESULT query_current_package_info( UINT32 flags, BYTE **buffer_out,
                                           UINT32 *size_out, UINT32 *count_out )
{
    UINT32 size = 0, count = 0, capacity;
    BYTE *buffer = NULL;
    LONG ret;

    *buffer_out = NULL;
    *size_out = 0;
    *count_out = 0;
    ret = GetCurrentPackageInfo( flags, &size, NULL, &count );
    if (ret == ERROR_SUCCESS)
    {
        if (size || count)
            return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
        return S_OK;
    }
    if (ret != ERROR_INSUFFICIENT_BUFFER) return HRESULT_FROM_WIN32( ret );
    if (!size || size > PACKAGE_INFO_LIMIT || count > PACKAGE_COUNT_LIMIT ||
        count > size / sizeof(struct package_info_native))
        return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
    if (!(buffer = malloc( size ))) return E_OUTOFMEMORY;

    capacity = size;
    ret = GetCurrentPackageInfo( flags, &capacity, buffer, &count );
    if (ret != ERROR_SUCCESS)
    {
        free( buffer );
        return HRESULT_FROM_WIN32( ret );
    }
    if (capacity != size || count > PACKAGE_COUNT_LIMIT ||
        count > size / sizeof(struct package_info_native))
    {
        free( buffer );
        return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
    }

    *buffer_out = buffer;
    *size_out = size;
    *count_out = count;
    return S_OK;
}

static HRESULT initialize_package_data( struct package_data *data,
                                        const struct package_info_native *info,
                                        const BYTE *buffer, UINT32 size,
                                        UINT32 index )
{
    const PACKAGE_ID *id = &info->package_id;
    HRESULT hr;

    if (info->reserved || id->reserved || (info->flags & ~PACKAGE_PROPERTY_KNOWN) ||
        !valid_processor_architecture( id->processorArchitecture ) ||
        (!index && info->flags) ||
        (index && (info->flags & (PACKAGE_PROPERTY_FRAMEWORK | PACKAGE_PROPERTY_STATIC)) !=
                  (PACKAGE_PROPERTY_FRAMEWORK | PACKAGE_PROPERTY_STATIC)) ||
        (info->flags & PACKAGE_PROPERTY_RESOURCE))
        return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT );

    if (FAILED( hr = hstring_from_package_info( buffer, size, id->name, FALSE,
                                                &data->name ) ) ||
        FAILED( hr = hstring_from_package_info( buffer, size, id->publisher, FALSE,
                                                &data->publisher ) ) ||
        FAILED( hr = hstring_from_package_info( buffer, size, id->resourceId, TRUE,
                                                &data->resource_id ) ) ||
        FAILED( hr = hstring_from_package_info( buffer, size, id->publisherId, FALSE,
                                                &data->publisher_id ) ) ||
        FAILED( hr = hstring_from_package_info( buffer, size, info->package_full_name,
                                                FALSE, &data->full_name ) ) ||
        FAILED( hr = hstring_from_package_info( buffer, size, info->package_family_name,
                                                FALSE, &data->family_name ) ) ||
        FAILED( hr = hstring_from_package_info( buffer, size, info->path, FALSE,
                                                &data->root ) ))
        return hr;

    data->version.Major = id->version.Major;
    data->version.Minor = id->version.Minor;
    data->version.Build = id->version.Build;
    data->version.Revision = id->version.Revision;
    data->architecture = (ProcessorArchitecture)id->processorArchitecture;
    data->flags = info->flags;
    return S_OK;
}

static HRESULT validate_package_uniqueness( const struct package_snapshot *snapshot )
{
    UINT32 i, j;
    INT32 comparison;
    HRESULT hr;

    for (i = 0; i < snapshot->count; i++)
    {
        for (j = i + 1; j < snapshot->count; j++)
        {
            if (FAILED( hr = WindowsCompareStringOrdinal(
                            snapshot->packages[i].full_name,
                            snapshot->packages[j].full_name, &comparison ) ))
                return hr;
            if (!comparison)
                return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
            if (FAILED( hr = WindowsCompareStringOrdinal(
                            snapshot->packages[i].family_name,
                            snapshot->packages[j].family_name, &comparison ) ))
                return hr;
            if (!comparison)
                return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
        }
    }
    return S_OK;
}

static HRESULT mark_direct_dependencies( struct package_snapshot *snapshot )
{
    struct package_info_native *info;
    BYTE *buffer;
    UINT32 size, count, i, j;
    HRESULT hr;

    if (FAILED( hr = query_current_package_info( PACKAGE_FILTER_STATIC | PACKAGE_FILTER_DIRECT,
                                                 &buffer, &size, &count ) ))
        return hr;
    info = (struct package_info_native *)buffer;
    for (i = 0; i < count; i++)
    {
        HSTRING full_name = NULL;
        INT32 comparison;

        hr = S_OK;
        if (info[i].reserved || info[i].package_id.reserved ||
            (info[i].flags & (PACKAGE_PROPERTY_FRAMEWORK | PACKAGE_PROPERTY_STATIC)) !=
            (PACKAGE_PROPERTY_FRAMEWORK | PACKAGE_PROPERTY_STATIC) ||
            (info[i].flags & ~PACKAGE_PROPERTY_KNOWN) ||
            FAILED( hr = hstring_from_package_info( buffer, size,
                                                    info[i].package_full_name,
                                                    FALSE, &full_name ) ))
        {
            if (SUCCEEDED(hr))
                hr = HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_IDENTITY_CORRUPT );
            WindowsDeleteString( full_name );
            free( buffer );
            return hr;
        }

        for (j = 1; j < snapshot->count; j++)
        {
            if (FAILED( hr = WindowsCompareStringOrdinal(
                            full_name, snapshot->packages[j].full_name, &comparison ) ))
                break;
            if (!comparison)
            {
                if (snapshot->packages[j].direct)
                    hr = HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
                else
                    snapshot->packages[j].direct = TRUE;
                break;
            }
        }
        WindowsDeleteString( full_name );
        if (FAILED(hr) || j == snapshot->count)
        {
            free( buffer );
            return FAILED(hr) ? hr :
                HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
        }
    }
    free( buffer );
    return S_OK;
}

static HRESULT package_snapshot_create_current( struct package_snapshot **value )
{
    struct package_info_native *info;
    struct package_snapshot *snapshot;
    BYTE *buffer;
    UINT32 size, count, i;
    HRESULT hr;

    *value = NULL;
    if (FAILED( hr = query_current_package_info( PACKAGE_FILTER_STATIC,
                                                 &buffer, &size, &count ) ))
        return hr;
    if (!count)
    {
        free( buffer );
        return HRESULT_FROM_WIN32( APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT );
    }
    if (!(snapshot = calloc( 1, sizeof(*snapshot) )))
    {
        free( buffer );
        return E_OUTOFMEMORY;
    }
    if (!(snapshot->packages = calloc( count, sizeof(*snapshot->packages) )))
    {
        free( snapshot );
        free( buffer );
        return E_OUTOFMEMORY;
    }
    snapshot->ref = 1;
    snapshot->count = count;
    info = (struct package_info_native *)buffer;
    for (i = 0; i < count; i++)
    {
        if (FAILED( hr = initialize_package_data( &snapshot->packages[i], &info[i],
                                                  buffer, size, i ) ))
        {
            free( buffer );
            package_snapshot_release( snapshot );
            return hr;
        }
    }
    free( buffer );
    if (FAILED( hr = validate_package_uniqueness( snapshot ) ))
    {
        package_snapshot_release( snapshot );
        return hr;
    }
    if (FAILED( hr = mark_direct_dependencies( snapshot ) ))
    {
        package_snapshot_release( snapshot );
        return hr;
    }
    *value = snapshot;
    return S_OK;
}

struct package_statics
{
    IActivationFactory IActivationFactory_iface;
    IPackageStatics IPackageStatics_iface;
    LONG ref;
};

static inline struct package_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct package_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct package_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IPackageStatics ))
        *out = &impl->IPackageStatics_iface;
    else
        return E_NOINTERFACE;
    IInspectable_AddRef( *out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct package_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct package_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IPackageStatics};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return runtime_class_name( RuntimeClass_Windows_ApplicationModel_Package, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    if (!instance) return E_POINTER;
    *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

struct storage_folder
{
    IStorageFolder IStorageFolder_iface;
    IStorageItem IStorageItem_iface;
    LONG ref;
    HSTRING path;
    HSTRING name;
};

static inline struct storage_folder *impl_from_IStorageFolder( IStorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct storage_folder, IStorageFolder_iface );
}

static HRESULT WINAPI storage_folder_QueryInterface( IStorageFolder *iface, REFIID iid, void **out )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IStorageFolder ))
        *out = &impl->IStorageFolder_iface;
    else if (IsEqualGUID( iid, &IID_IStorageItem ))
        *out = &impl->IStorageItem_iface;
    else
        return E_NOINTERFACE;
    IInspectable_AddRef( *out );
    return S_OK;
}

static ULONG WINAPI storage_folder_AddRef( IStorageFolder *iface )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI storage_folder_Release( IStorageFolder *iface )
{
    struct storage_folder *impl = impl_from_IStorageFolder( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->path );
        WindowsDeleteString( impl->name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI storage_folder_GetIids( IStorageFolder *iface, ULONG *iid_count,
                                               IID **iids )
{
    const IID supported[] = {IID_IStorageFolder, IID_IStorageItem};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI storage_folder_GetRuntimeClassName( IStorageFolder *iface,
                                                          HSTRING *class_name )
{
    return runtime_class_name( RuntimeClass_Windows_Storage_StorageFolder, class_name );
}

static HRESULT WINAPI storage_folder_GetTrustLevel( IStorageFolder *iface,
                                                     TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

#define STORAGE_FOLDER_ASYNC_STUB(name, args, operation) \
    static HRESULT WINAPI name args                      \
    {                                                    \
        if (!(operation)) return E_INVALIDARG;           \
        *(operation) = NULL;                             \
        return E_NOTIMPL;                                \
    }

STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_CreateFileAsyncOverloadDefaultOptions,
    (IStorageFolder *iface, HSTRING name, IAsyncOperation_StorageFile **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_CreateFileAsync,
    (IStorageFolder *iface, HSTRING name, CreationCollisionOption options,
     IAsyncOperation_StorageFile **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_CreateFolderAsyncOverloadDefaultOptions,
    (IStorageFolder *iface, HSTRING name, IAsyncOperation_StorageFolder **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_CreateFolderAsync,
    (IStorageFolder *iface, HSTRING name, CreationCollisionOption options,
     IAsyncOperation_StorageFolder **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetFileAsync,
    (IStorageFolder *iface, HSTRING name, IAsyncOperation_StorageFile **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetFolderAsync,
    (IStorageFolder *iface, HSTRING name, IAsyncOperation_StorageFolder **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetItemAsync,
    (IStorageFolder *iface, HSTRING name, IAsyncOperation_IStorageItem **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetFilesAsyncOverloadDefaultOptionsStartAndCount,
    (IStorageFolder *iface, IAsyncOperation_IVectorView_StorageFile **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetFoldersAsyncOverloadDefaultOptionsStartAndCount,
    (IStorageFolder *iface, IAsyncOperation_IVectorView_StorageFolder **operation),
    operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetItemsAsyncOverloadDefaultStartAndCount,
    (IStorageFolder *iface, IAsyncOperation_IVectorView_IStorageItem **operation),
    operation)

static const IStorageFolderVtbl storage_folder_vtbl =
{
    storage_folder_QueryInterface,
    storage_folder_AddRef,
    storage_folder_Release,
    storage_folder_GetIids,
    storage_folder_GetRuntimeClassName,
    storage_folder_GetTrustLevel,
    storage_folder_CreateFileAsyncOverloadDefaultOptions,
    storage_folder_CreateFileAsync,
    storage_folder_CreateFolderAsyncOverloadDefaultOptions,
    storage_folder_CreateFolderAsync,
    storage_folder_GetFileAsync,
    storage_folder_GetFolderAsync,
    storage_folder_GetItemAsync,
    storage_folder_GetFilesAsyncOverloadDefaultOptionsStartAndCount,
    storage_folder_GetFoldersAsyncOverloadDefaultOptionsStartAndCount,
    storage_folder_GetItemsAsyncOverloadDefaultStartAndCount,
};

DEFINE_IINSPECTABLE( storage_item, IStorageItem, struct storage_folder,
                     IStorageFolder_iface )

static HRESULT WINAPI storage_item_RenameAsyncOverloadDefaultOptions(
    IStorageItem *iface, HSTRING name, IAsyncAction **operation )
{
    if (!operation) return E_INVALIDARG;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_RenameAsync( IStorageItem *iface, HSTRING name,
                                                NameCollisionOption option,
                                                IAsyncAction **operation )
{
    if (!operation) return E_INVALIDARG;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_DeleteAsyncOverloadDefaultOptions(
    IStorageItem *iface, IAsyncAction **operation )
{
    if (!operation) return E_INVALIDARG;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_DeleteAsync( IStorageItem *iface,
                                                StorageDeleteOption option,
                                                IAsyncAction **operation )
{
    if (!operation) return E_INVALIDARG;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_GetBasicPropertiesAsync(
    IStorageItem *iface, IAsyncOperation_BasicProperties **operation )
{
    if (!operation) return E_INVALIDARG;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_get_Name( IStorageItem *iface, HSTRING *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    if (!value) return E_INVALIDARG;
    return WindowsDuplicateString( impl->name, value );
}

static HRESULT WINAPI storage_item_get_Path( IStorageItem *iface, HSTRING *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    if (!value) return E_INVALIDARG;
    return WindowsDuplicateString( impl->path, value );
}

static HRESULT WINAPI storage_item_get_Attributes( IStorageItem *iface,
                                                   FileAttributes *value )
{
    if (!value) return E_INVALIDARG;
    *value = FileAttributes_Directory;
    return S_OK;
}

static HRESULT WINAPI storage_item_get_DateCreated( IStorageItem *iface, DateTime *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    const WCHAR *path = WindowsGetStringRawBuffer( impl->path, NULL );
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    ULARGE_INTEGER time;

    if (!value) return E_INVALIDARG;
    if (!GetFileAttributesExW( path, GetFileExInfoStandard, &attributes ))
        return HRESULT_FROM_WIN32( GetLastError() );
    time.LowPart = attributes.ftCreationTime.dwLowDateTime;
    time.HighPart = attributes.ftCreationTime.dwHighDateTime;
    value->UniversalTime = time.QuadPart;
    return S_OK;
}

static HRESULT WINAPI storage_item_IsOfType( IStorageItem *iface, StorageItemTypes type,
                                             boolean *value )
{
    if (!value) return E_INVALIDARG;
    *value = !!(type & StorageItemTypes_Folder);
    return S_OK;
}

static const IStorageItemVtbl storage_item_vtbl =
{
    storage_item_QueryInterface,
    storage_item_AddRef,
    storage_item_Release,
    storage_item_GetIids,
    storage_item_GetRuntimeClassName,
    storage_item_GetTrustLevel,
    storage_item_RenameAsyncOverloadDefaultOptions,
    storage_item_RenameAsync,
    storage_item_DeleteAsyncOverloadDefaultOptions,
    storage_item_DeleteAsync,
    storage_item_GetBasicPropertiesAsync,
    storage_item_get_Name,
    storage_item_get_Path,
    storage_item_get_Attributes,
    storage_item_get_DateCreated,
    storage_item_IsOfType,
};

static HRESULT storage_folder_create( HSTRING path, IStorageFolder **value )
{
    struct storage_folder *impl;
    const WCHAR *buffer, *name;
    UINT32 length;
    HRESULT hr;

    *value = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IStorageFolder_iface.lpVtbl = &storage_folder_vtbl;
    impl->IStorageItem_iface.lpVtbl = &storage_item_vtbl;
    impl->ref = 1;
    if (FAILED( hr = WindowsDuplicateString( path, &impl->path ) )) goto failed;
    buffer = WindowsGetStringRawBuffer( path, &length );
    while (length && (buffer[length - 1] == '\\' || buffer[length - 1] == '/')) length--;
    name = buffer + length;
    while (name > buffer && name[-1] != '\\' && name[-1] != '/') name--;
    if (FAILED( hr = WindowsCreateString( name, buffer + length - name, &impl->name ) ))
        goto failed;
    *value = &impl->IStorageFolder_iface;
    return S_OK;

failed:
    IStorageFolder_Release( &impl->IStorageFolder_iface );
    return hr;
}

struct package_id
{
    IPackageId IPackageId_iface;
    IPackageIdWithMetadata IPackageIdWithMetadata_iface;
    LONG ref;
    struct package_snapshot *snapshot;
    UINT32 index;
};

static inline struct package_id *impl_from_IPackageId( IPackageId *iface )
{
    return CONTAINING_RECORD( iface, struct package_id, IPackageId_iface );
}

static HRESULT WINAPI package_id_QueryInterface( IPackageId *iface, REFIID iid, void **out )
{
    struct package_id *impl = impl_from_IPackageId( iface );

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IPackageId ))
        *out = &impl->IPackageId_iface;
    else if (IsEqualGUID( iid, &IID_IPackageIdWithMetadata ))
        *out = &impl->IPackageIdWithMetadata_iface;
    else
        return E_NOINTERFACE;
    IInspectable_AddRef( *out );
    return S_OK;
}

static ULONG WINAPI package_id_AddRef( IPackageId *iface )
{
    struct package_id *impl = impl_from_IPackageId( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI package_id_Release( IPackageId *iface )
{
    struct package_id *impl = impl_from_IPackageId( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        package_snapshot_release( impl->snapshot );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI package_id_GetIids( IPackageId *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IPackageId, IID_IPackageIdWithMetadata};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_id_GetRuntimeClassName( IPackageId *iface, HSTRING *class_name )
{
    return runtime_class_name( RuntimeClass_Windows_ApplicationModel_PackageId, class_name );
}

static HRESULT WINAPI package_id_GetTrustLevel( IPackageId *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

#define PACKAGE_ID_HSTRING_GETTER(name, member)                                  \
    static HRESULT WINAPI name( IPackageId *iface, HSTRING *value )              \
    {                                                                             \
        struct package_id *impl = impl_from_IPackageId( iface );                  \
        if (!value) return E_INVALIDARG;                                          \
        return WindowsDuplicateString(                                            \
            impl->snapshot->packages[impl->index].member, value );                \
    }

PACKAGE_ID_HSTRING_GETTER( package_id_get_Name, name )
PACKAGE_ID_HSTRING_GETTER( package_id_get_ResourceId, resource_id )
PACKAGE_ID_HSTRING_GETTER( package_id_get_Publisher, publisher )
PACKAGE_ID_HSTRING_GETTER( package_id_get_PublisherId, publisher_id )
PACKAGE_ID_HSTRING_GETTER( package_id_get_FullName, full_name )
PACKAGE_ID_HSTRING_GETTER( package_id_get_FamilyName, family_name )

static HRESULT WINAPI package_id_get_Version( IPackageId *iface, PackageVersion *value )
{
    struct package_id *impl = impl_from_IPackageId( iface );
    if (!value) return E_INVALIDARG;
    *value = impl->snapshot->packages[impl->index].version;
    return S_OK;
}

static HRESULT WINAPI package_id_get_Architecture( IPackageId *iface,
                                                   ProcessorArchitecture *value )
{
    struct package_id *impl = impl_from_IPackageId( iface );
    if (!value) return E_INVALIDARG;
    *value = impl->snapshot->packages[impl->index].architecture;
    return S_OK;
}

static const IPackageIdVtbl package_id_vtbl =
{
    package_id_QueryInterface,
    package_id_AddRef,
    package_id_Release,
    package_id_GetIids,
    package_id_GetRuntimeClassName,
    package_id_GetTrustLevel,
    package_id_get_Name,
    package_id_get_Version,
    package_id_get_Architecture,
    package_id_get_ResourceId,
    package_id_get_Publisher,
    package_id_get_PublisherId,
    package_id_get_FullName,
    package_id_get_FamilyName,
};

DEFINE_IINSPECTABLE_( package_id_metadata, IPackageIdWithMetadata,
                      struct package_id, impl_from_IPackageIdWithMetadata,
                      IPackageIdWithMetadata_iface, &impl->IPackageId_iface )

static HRESULT WINAPI package_id_metadata_get_ProductId(
    IPackageIdWithMetadata *iface, HSTRING *value )
{
    return unavailable_hstring( value );
}

static HRESULT WINAPI package_id_metadata_get_Author(
    IPackageIdWithMetadata *iface, HSTRING *value )
{
    return unavailable_hstring( value );
}

static const IPackageIdWithMetadataVtbl package_id_metadata_vtbl =
{
    package_id_metadata_QueryInterface,
    package_id_metadata_AddRef,
    package_id_metadata_Release,
    package_id_metadata_GetIids,
    package_id_metadata_GetRuntimeClassName,
    package_id_metadata_GetTrustLevel,
    package_id_metadata_get_ProductId,
    package_id_metadata_get_Author,
};

static HRESULT package_id_create( struct package_snapshot *snapshot, UINT32 index,
                                  IPackageId **value )
{
    struct package_id *impl;

    *value = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPackageId_iface.lpVtbl = &package_id_vtbl;
    impl->IPackageIdWithMetadata_iface.lpVtbl = &package_id_metadata_vtbl;
    impl->ref = 1;
    impl->snapshot = package_snapshot_addref( snapshot );
    impl->index = index;
    *value = &impl->IPackageId_iface;
    return S_OK;
}

static inline struct package *impl_from_IPackage( IPackage *iface )
{
    return CONTAINING_RECORD( iface, struct package, IPackage_iface );
}

static HRESULT WINAPI package_QueryInterface( IPackage *iface, REFIID iid, void **out )
{
    struct package *impl = impl_from_IPackage( iface );

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IPackage ))
        *out = &impl->IPackage_iface;
    else if (IsEqualGUID( iid, &IID_IPackage2 ))
        *out = &impl->IPackage2_iface;
    else if (IsEqualGUID( iid, &IID_IPackage3 ))
        *out = &impl->IPackage3_iface;
    else
        return E_NOINTERFACE;
    IInspectable_AddRef( *out );
    return S_OK;
}

static ULONG WINAPI package_AddRef( IPackage *iface )
{
    struct package *impl = impl_from_IPackage( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI package_Release( IPackage *iface )
{
    struct package *impl = impl_from_IPackage( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        package_snapshot_release( impl->snapshot );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI package_GetIids( IPackage *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IPackage, IID_IPackage2, IID_IPackage3};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_GetRuntimeClassName( IPackage *iface, HSTRING *class_name )
{
    return runtime_class_name( RuntimeClass_Windows_ApplicationModel_Package, class_name );
}

static HRESULT WINAPI package_GetTrustLevel( IPackage *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI package_get_Id( IPackage *iface, IPackageId **value )
{
    struct package *impl = impl_from_IPackage( iface );
    if (!value) return E_INVALIDARG;
    return package_id_create( impl->snapshot, impl->index, value );
}

static HRESULT WINAPI package_get_InstalledLocation( IPackage *iface,
                                                     IStorageFolder **value )
{
    struct package *impl = impl_from_IPackage( iface );
    if (!value) return E_INVALIDARG;
    return storage_folder_create( impl->snapshot->packages[impl->index].root, value );
}

static HRESULT WINAPI package_get_IsFramework( IPackage *iface, boolean *value )
{
    struct package *impl = impl_from_IPackage( iface );
    if (!value) return E_INVALIDARG;
    *value = !!(impl->snapshot->packages[impl->index].flags &
                PACKAGE_PROPERTY_FRAMEWORK);
    return S_OK;
}

static HRESULT WINAPI package_get_Dependencies( IPackage *iface,
                                                IVectorView_Package **value )
{
    struct package *impl = impl_from_IPackage( iface );
    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (impl->index)
    {
        /*
         * The process graph encodes direct dependencies of the head package,
         * not dependency-to-dependency edges.  Returning an empty list for a
         * framework package would silently invent an edge set.
         */
        return E_NOTIMPL;
    }
    return package_vector_create( impl->snapshot, value );
}

static const IPackageVtbl package_vtbl =
{
    package_QueryInterface,
    package_AddRef,
    package_Release,
    package_GetIids,
    package_GetRuntimeClassName,
    package_GetTrustLevel,
    package_get_Id,
    package_get_InstalledLocation,
    package_get_IsFramework,
    package_get_Dependencies,
};

DEFINE_IINSPECTABLE_( package2, IPackage2, struct package, impl_from_IPackage2,
                      IPackage2_iface, &impl->IPackage_iface )

static HRESULT WINAPI package2_get_DisplayName( IPackage2 *iface, HSTRING *value )
{
    return unavailable_hstring( value );
}

static HRESULT WINAPI package2_get_PublisherDisplayName( IPackage2 *iface, HSTRING *value )
{
    return unavailable_hstring( value );
}

static HRESULT WINAPI package2_get_Description( IPackage2 *iface, HSTRING *value )
{
    return unavailable_hstring( value );
}

static HRESULT WINAPI package2_get_Logo( IPackage2 *iface, IUriRuntimeClass **value )
{
    if (!value) return E_INVALIDARG;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package2_get_IsResourcePackage( IPackage2 *iface, boolean *value )
{
    struct package *impl = impl_from_IPackage2( iface );
    if (!value) return E_INVALIDARG;
    *value = !!(impl->snapshot->packages[impl->index].flags &
                PACKAGE_PROPERTY_RESOURCE);
    return S_OK;
}

static HRESULT WINAPI package2_get_IsBundle( IPackage2 *iface, boolean *value )
{
    if (!value) return E_INVALIDARG;
    *value = FALSE;
    return E_NOTIMPL;
}

static HRESULT WINAPI package2_get_IsDevelopmentMode( IPackage2 *iface, boolean *value )
{
    if (!value) return E_INVALIDARG;
    *value = FALSE;
    return S_OK;
}

static const IPackage2Vtbl package2_vtbl =
{
    package2_QueryInterface,
    package2_AddRef,
    package2_Release,
    package2_GetIids,
    package2_GetRuntimeClassName,
    package2_GetTrustLevel,
    package2_get_DisplayName,
    package2_get_PublisherDisplayName,
    package2_get_Description,
    package2_get_Logo,
    package2_get_IsResourcePackage,
    package2_get_IsBundle,
    package2_get_IsDevelopmentMode,
};

DEFINE_IINSPECTABLE_( package3, IPackage3, struct package, impl_from_IPackage3,
                      IPackage3_iface, &impl->IPackage_iface )

static HRESULT WINAPI package3_get_Status( IPackage3 *iface, IPackageStatus **value )
{
    if (!value) return E_INVALIDARG;
    *value = NULL;
    /*
     * The launch graph proves identity and payload provenance, but it does
     * not carry the deployment, servicing, licensing, or tamper state needed
     * to answer IPackageStatus.  Do not turn graph validity into a fabricated
     * all-good package status.
     */
    return E_NOTIMPL;
}

static HRESULT WINAPI package3_get_InstalledDate( IPackage3 *iface, DateTime *value )
{
    if (!value) return E_INVALIDARG;
    value->UniversalTime = 0;
    return E_NOTIMPL;
}

static HRESULT WINAPI package3_GetAppListEntriesAsync(
    IPackage3 *iface, IAsyncOperation_IVectorView_AppListEntry **operation )
{
    if (!operation) return E_INVALIDARG;
    *operation = NULL;
    /*
     * Graph v2 intentionally contains executable identity and launch data but
     * no display metadata.  IAppListEntry cannot be projected faithfully
     * without its required IAppDisplayInfo surface.
     */
    return E_NOTIMPL;
}

static const IPackage3Vtbl package3_vtbl =
{
    package3_QueryInterface,
    package3_AddRef,
    package3_Release,
    package3_GetIids,
    package3_GetRuntimeClassName,
    package3_GetTrustLevel,
    package3_get_Status,
    package3_get_InstalledDate,
    package3_GetAppListEntriesAsync,
};

static HRESULT package_create( struct package_snapshot *snapshot, UINT32 index,
                               IPackage **value )
{
    struct package *impl;

    *value = NULL;
    if (index >= snapshot->count)
        return HRESULT_FROM_WIN32( ERROR_INVALID_INDEX );
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPackage_iface.lpVtbl = &package_vtbl;
    impl->IPackage2_iface.lpVtbl = &package2_vtbl;
    impl->IPackage3_iface.lpVtbl = &package3_vtbl;
    impl->ref = 1;
    impl->snapshot = package_snapshot_addref( snapshot );
    impl->index = index;
    *value = &impl->IPackage_iface;
    return S_OK;
}

struct package_vector
{
    IVectorView_Package IVectorView_Package_iface;
    LONG ref;
    UINT32 count;
    IPackage **items;
};

static inline struct package_vector *impl_from_IVectorView_Package(
    IVectorView_Package *iface )
{
    return CONTAINING_RECORD( iface, struct package_vector, IVectorView_Package_iface );
}

static HRESULT WINAPI package_vector_QueryInterface( IVectorView_Package *iface,
                                                      REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID( iid, &IID_IUnknown ) &&
        !IsEqualGUID( iid, &IID_IInspectable ) &&
        !IsEqualGUID( iid, &IID_IAgileObject ) &&
        !IsEqualGUID( iid, &IID_IVectorView_Package ))
        return E_NOINTERFACE;
    *out = iface;
    IVectorView_Package_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI package_vector_AddRef( IVectorView_Package *iface )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI package_vector_Release( IVectorView_Package *iface )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++) IPackage_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI package_vector_GetIids( IVectorView_Package *iface,
                                              ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IVectorView_Package};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_vector_GetRuntimeClassName(
    IVectorView_Package *iface, HSTRING *class_name )
{
    static const WCHAR name[] =
        L"Windows.Foundation.Collections.IVectorView`1<"
        L"Windows.ApplicationModel.Package>";
    return runtime_class_name( name, class_name );
}

static HRESULT WINAPI package_vector_GetTrustLevel( IVectorView_Package *iface,
                                                     TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI package_vector_GetAt( IVectorView_Package *iface, UINT32 index,
                                            IPackage **value )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );
    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    IPackage_AddRef( impl->items[index] );
    *value = impl->items[index];
    return S_OK;
}

static HRESULT WINAPI package_vector_get_Size( IVectorView_Package *iface, UINT32 *value )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );
    if (!value) return E_INVALIDARG;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI package_vector_IndexOf( IVectorView_Package *iface,
                                              IPackage *element, UINT32 *index,
                                              boolean *value )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );
    IUnknown *identity = NULL, *candidate = NULL;
    UINT32 i;
    HRESULT hr = S_OK;

    if (!index || !value) return E_INVALIDARG;
    *index = 0;
    *value = FALSE;
    if (!element) return S_OK;
    if (FAILED( hr = IPackage_QueryInterface( element, &IID_IUnknown,
                                              (void **)&identity ) ))
        return hr;
    for (i = 0; i < impl->count; i++)
    {
        hr = IPackage_QueryInterface( impl->items[i], &IID_IUnknown,
                                      (void **)&candidate );
        if (FAILED(hr)) break;
        if (candidate == identity)
        {
            *index = i;
            *value = TRUE;
            IUnknown_Release( candidate );
            break;
        }
        IUnknown_Release( candidate );
        candidate = NULL;
    }
    IUnknown_Release( identity );
    return hr;
}

static HRESULT WINAPI package_vector_GetMany( IVectorView_Package *iface,
                                              UINT32 start_index, UINT32 items_size,
                                              IPackage **items, UINT32 *value )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );
    UINT32 count, i;

    if (!value || (items_size && !items)) return E_INVALIDARG;
    *value = 0;
    if (start_index >= impl->count) return S_OK;
    count = min( items_size, impl->count - start_index );
    for (i = 0; i < count; i++)
    {
        items[i] = impl->items[start_index + i];
        IPackage_AddRef( items[i] );
    }
    *value = count;
    return S_OK;
}

static const IVectorView_PackageVtbl package_vector_vtbl =
{
    package_vector_QueryInterface,
    package_vector_AddRef,
    package_vector_Release,
    package_vector_GetIids,
    package_vector_GetRuntimeClassName,
    package_vector_GetTrustLevel,
    package_vector_GetAt,
    package_vector_get_Size,
    package_vector_IndexOf,
    package_vector_GetMany,
};

static HRESULT package_vector_create( struct package_snapshot *snapshot,
                                      IVectorView_Package **value )
{
    struct package_vector *impl;
    UINT32 i, count = 0, output = 0;
    HRESULT hr;

    *value = NULL;
    for (i = 1; i < snapshot->count; i++)
        if (snapshot->packages[i].direct) count++;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_Package_iface.lpVtbl = &package_vector_vtbl;
    impl->ref = 1;
    impl->count = count;
    if (count && !(impl->items = calloc( count, sizeof(*impl->items) )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    for (i = 1; i < snapshot->count; i++)
    {
        if (!snapshot->packages[i].direct) continue;
        if (FAILED( hr = package_create( snapshot, i, &impl->items[output] ) ))
        {
            impl->count = output;
            IVectorView_Package_Release( &impl->IVectorView_Package_iface );
            return hr;
        }
        output++;
    }
    *value = &impl->IVectorView_Package_iface;
    return S_OK;
}

DEFINE_IINSPECTABLE( package_statics, IPackageStatics, struct package_statics,
                     IActivationFactory_iface )

static HRESULT WINAPI package_statics_get_Current( IPackageStatics *iface, IPackage **value )
{
    struct package_snapshot *snapshot;
    HRESULT hr;

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (FAILED( hr = package_snapshot_create_current( &snapshot ) )) return hr;
    hr = package_create( snapshot, 0, value );
    package_snapshot_release( snapshot );
    return hr;
}

static const IPackageStaticsVtbl package_statics_vtbl =
{
    package_statics_QueryInterface,
    package_statics_AddRef,
    package_statics_Release,
    package_statics_GetIids,
    package_statics_GetRuntimeClassName,
    package_statics_GetTrustLevel,
    package_statics_get_Current,
};

static struct package_statics package_statics =
{
    {&factory_vtbl},
    {&package_statics_vtbl},
    1,
};

IActivationFactory *package_factory = &package_statics.IActivationFactory_iface;
