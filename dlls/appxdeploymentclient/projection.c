/*
 * Windows.Management.Deployment package query projections
 *
 * Copyright 2026 Jungwuk Ryu
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

#include <stdint.h>

struct package_snapshot
{
    LONG ref;
    APPX_CATALOG_SNAPSHOT *catalog;
};

struct package
{
    IPackage IPackage_iface;
    IPackage2 IPackage2_iface;
    LONG ref;
    struct package_snapshot *snapshot;
    UINT32 index;
};

struct package_id
{
    IPackageId IPackageId_iface;
    LONG ref;
    struct package_snapshot *snapshot;
    UINT32 index;
};

struct package_iterable
{
    IIterable_Package IIterable_Package_iface;
    LONG ref;
    UINT32 count;
    IPackage **items;
};

struct package_iterator
{
    IIterator_Package IIterator_Package_iface;
    LONG ref;
    struct package_iterable *iterable;
    UINT32 index;
};

struct package_vector
{
    IVectorView_Package IVectorView_Package_iface;
    LONG ref;
    UINT32 count;
    IPackage **items;
};

struct storage_folder
{
    IStorageFolder IStorageFolder_iface;
    IStorageItem IStorageItem_iface;
    LONG ref;
    HSTRING path;
    HSTRING name;
};

static HRESULT package_create_from_catalog(
    struct package_snapshot *snapshot, UINT32 index, IPackage **value );

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

static struct package_snapshot *package_snapshot_addref(
    struct package_snapshot *snapshot )
{
    InterlockedIncrement( &snapshot->ref );
    return snapshot;
}

static void package_snapshot_release( struct package_snapshot *snapshot )
{
    if (InterlockedDecrement( &snapshot->ref )) return;
    appx_backend_catalog_snapshot_free( snapshot->catalog );
    free( snapshot );
}

static HRESULT package_snapshot_create( APPX_CATALOG_SNAPSHOT *catalog,
                                        struct package_snapshot **value )
{
    struct package_snapshot *snapshot;

    *value = NULL;
    if (!(snapshot = calloc( 1, sizeof(*snapshot) )))
    {
        appx_backend_catalog_snapshot_free( catalog );
        return E_OUTOFMEMORY;
    }
    snapshot->ref = 1;
    snapshot->catalog = catalog;
    *value = snapshot;
    return S_OK;
}

static const struct appx_catalog_package *package_get_catalog(
    struct package_snapshot *snapshot, UINT32 index )
{
    return appx_backend_catalog_snapshot_get_package(
        snapshot->catalog, index );
}

static HRESULT duplicate_catalog_string( const WCHAR *source, HSTRING *value )
{
    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (!source) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    return WindowsCreateString( source, wcslen(source), value );
}

static HRESULT compare_catalog_string( const WCHAR *left, const WCHAR *right,
                                       BOOL *equal )
{
    int result;

    *equal = FALSE;
    if (!left || !right) return E_INVALIDARG;
    result = CompareStringOrdinal( left, -1, right, -1, TRUE );
    if (!result) return HRESULT_FROM_WIN32( GetLastError() );
    *equal = result == CSTR_EQUAL;
    return S_OK;
}

static ProcessorArchitecture catalog_architecture_to_winrt(
    enum appx_catalog_architecture architecture )
{
    switch (architecture)
    {
    case APPX_CATALOG_ARCHITECTURE_X86:
        return ProcessorArchitecture_X86;
    case APPX_CATALOG_ARCHITECTURE_X64:
        return ProcessorArchitecture_X64;
    case APPX_CATALOG_ARCHITECTURE_ARM:
        return ProcessorArchitecture_Arm;
    case APPX_CATALOG_ARCHITECTURE_ARM64:
        return ProcessorArchitecture_Arm64;
    case APPX_CATALOG_ARCHITECTURE_X86A64:
        return ProcessorArchitecture_X86OnArm64;
    case APPX_CATALOG_ARCHITECTURE_NEUTRAL:
        return ProcessorArchitecture_Neutral;
    }
    return ProcessorArchitecture_Unknown;
}

static inline struct storage_folder *impl_from_IStorageFolder(
    IStorageFolder *iface )
{
    return CONTAINING_RECORD( iface, struct storage_folder,
                              IStorageFolder_iface );
}

static inline struct storage_folder *impl_from_IStorageItem(
    IStorageItem *iface )
{
    return CONTAINING_RECORD( iface, struct storage_folder,
                              IStorageItem_iface );
}

static HRESULT WINAPI storage_folder_QueryInterface(
    IStorageFolder *iface, REFIID iid, void **out )
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

static HRESULT WINAPI storage_folder_GetIids(
    IStorageFolder *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IStorageFolder, IID_IStorageItem};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI storage_folder_GetRuntimeClassName(
    IStorageFolder *iface, HSTRING *class_name )
{
    return runtime_class_name( RuntimeClass_Windows_Storage_StorageFolder,
                               class_name );
}

static HRESULT WINAPI storage_folder_GetTrustLevel(
    IStorageFolder *iface, TrustLevel *trust_level )
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
    (IStorageFolder *iface, HSTRING name,
     IAsyncOperation_StorageFile **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_CreateFileAsync,
    (IStorageFolder *iface, HSTRING name, CreationCollisionOption options,
     IAsyncOperation_StorageFile **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_CreateFolderAsyncOverloadDefaultOptions,
    (IStorageFolder *iface, HSTRING name,
     IAsyncOperation_StorageFolder **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_CreateFolderAsync,
    (IStorageFolder *iface, HSTRING name, CreationCollisionOption options,
     IAsyncOperation_StorageFolder **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetFileAsync,
    (IStorageFolder *iface, HSTRING name,
     IAsyncOperation_StorageFile **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetFolderAsync,
    (IStorageFolder *iface, HSTRING name,
     IAsyncOperation_StorageFolder **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetItemAsync,
    (IStorageFolder *iface, HSTRING name,
     IAsyncOperation_IStorageItem **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetFilesAsyncOverloadDefaultOptionsStartAndCount,
    (IStorageFolder *iface,
     IAsyncOperation_IVectorView_StorageFile **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetFoldersAsyncOverloadDefaultOptionsStartAndCount,
    (IStorageFolder *iface,
     IAsyncOperation_IVectorView_StorageFolder **operation), operation)
STORAGE_FOLDER_ASYNC_STUB(
    storage_folder_GetItemsAsyncOverloadDefaultStartAndCount,
    (IStorageFolder *iface,
     IAsyncOperation_IVectorView_IStorageItem **operation), operation)

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

static HRESULT WINAPI storage_item_QueryInterface(
    IStorageItem *iface, REFIID iid, void **out )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    return IStorageFolder_QueryInterface( &impl->IStorageFolder_iface,
                                          iid, out );
}

static ULONG WINAPI storage_item_AddRef( IStorageItem *iface )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    return IStorageFolder_AddRef( &impl->IStorageFolder_iface );
}

static ULONG WINAPI storage_item_Release( IStorageItem *iface )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    return IStorageFolder_Release( &impl->IStorageFolder_iface );
}

static HRESULT WINAPI storage_item_GetIids(
    IStorageItem *iface, ULONG *iid_count, IID **iids )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    return IStorageFolder_GetIids( &impl->IStorageFolder_iface,
                                   iid_count, iids );
}

static HRESULT WINAPI storage_item_GetRuntimeClassName(
    IStorageItem *iface, HSTRING *class_name )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    return IStorageFolder_GetRuntimeClassName(
        &impl->IStorageFolder_iface, class_name );
}

static HRESULT WINAPI storage_item_GetTrustLevel(
    IStorageItem *iface, TrustLevel *trust_level )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    return IStorageFolder_GetTrustLevel( &impl->IStorageFolder_iface,
                                         trust_level );
}

static HRESULT WINAPI storage_item_RenameAsyncOverloadDefaultOptions(
    IStorageItem *iface, HSTRING name, IAsyncAction **operation )
{
    if (!operation) return E_INVALIDARG;
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI storage_item_RenameAsync(
    IStorageItem *iface, HSTRING name, NameCollisionOption option,
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

static HRESULT WINAPI storage_item_DeleteAsync(
    IStorageItem *iface, StorageDeleteOption option,
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

static HRESULT WINAPI storage_item_get_Name(
    IStorageItem *iface, HSTRING *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );

    if (!value) return E_INVALIDARG;
    return WindowsDuplicateString( impl->name, value );
}

static HRESULT WINAPI storage_item_get_Path(
    IStorageItem *iface, HSTRING *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );

    if (!value) return E_INVALIDARG;
    return WindowsDuplicateString( impl->path, value );
}

static HRESULT WINAPI storage_item_get_Attributes(
    IStorageItem *iface, FileAttributes *value )
{
    if (!value) return E_INVALIDARG;
    *value = FileAttributes_Directory;
    return S_OK;
}

static HRESULT WINAPI storage_item_get_DateCreated(
    IStorageItem *iface, DateTime *value )
{
    struct storage_folder *impl = impl_from_IStorageItem( iface );
    const WCHAR *path;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    ULARGE_INTEGER time;

    if (!value) return E_INVALIDARG;
    path = WindowsGetStringRawBuffer( impl->path, NULL );
    if (!GetFileAttributesExW( path, GetFileExInfoStandard, &attributes ))
        return HRESULT_FROM_WIN32( GetLastError() );
    time.LowPart = attributes.ftCreationTime.dwLowDateTime;
    time.HighPart = attributes.ftCreationTime.dwHighDateTime;
    value->UniversalTime = time.QuadPart;
    return S_OK;
}

static HRESULT WINAPI storage_item_IsOfType(
    IStorageItem *iface, StorageItemTypes type, boolean *value )
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

static HRESULT storage_folder_create( const WCHAR *path,
                                      IStorageFolder **value )
{
    struct storage_folder *impl;
    const WCHAR *name;
    UINT32 length;
    HRESULT hr = S_OK;

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (!path) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IStorageFolder_iface.lpVtbl = &storage_folder_vtbl;
    impl->IStorageItem_iface.lpVtbl = &storage_item_vtbl;
    impl->ref = 1;
    length = wcslen( path );
    if (FAILED( hr = WindowsCreateString( path, length, &impl->path ) ))
        goto failed;
    while (length && (path[length - 1] == '\\' || path[length - 1] == '/'))
        length--;
    name = path + length;
    while (name > path && name[-1] != '\\' && name[-1] != '/') name--;
    if (FAILED( hr = WindowsCreateString( name, path + length - name,
                                          &impl->name ) ))
        goto failed;
    *value = &impl->IStorageFolder_iface;
    return S_OK;

failed:
    IStorageFolder_Release( &impl->IStorageFolder_iface );
    return hr;
}

static HRESULT installed_location_create( const WCHAR *payload_path,
                                          IStorageFolder **value )
{
    static const WCHAR payload_prefix[] = L"payloads\\";
    const UINT32 generation_chars = APPX_DEPLOYMENT_CONTENT_ID_SIZE * 2;
    const UINT32 prefix_chars = ARRAY_SIZE(payload_prefix) - 1;
    const UINT32 root_chars = ARRAY_SIZE(APPX_STORE_ROOT) - 1;
    const UINT32 expected_chars = prefix_chars + generation_chars;
    WCHAR *path;
    UINT32 payload_chars, total_chars, i;
    HRESULT hr;

    if (!payload_path) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    for (payload_chars = 0; payload_chars <= expected_chars; payload_chars++)
        if (!payload_path[payload_chars]) break;
    if (payload_chars != expected_chars ||
        memcmp( payload_path, payload_prefix,
                prefix_chars * sizeof(*payload_path) ))
        return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    for (i = prefix_chars; i < payload_chars; i++)
        if (!((payload_path[i] >= '0' && payload_path[i] <= '9') ||
              (payload_path[i] >= 'a' && payload_path[i] <= 'f')))
            return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (root_chars > WINE_APPX_MAX_PATH_CHARS - 1 ||
        payload_chars > WINE_APPX_MAX_PATH_CHARS - root_chars - 1)
        return HRESULT_FROM_WIN32( ERROR_FILENAME_EXCED_RANGE );
    total_chars = root_chars + 1 + payload_chars;
    if (!(path = malloc( (total_chars + 1) * sizeof(*path) )))
        return E_OUTOFMEMORY;
    memcpy( path, APPX_STORE_ROOT, root_chars * sizeof(*path) );
    path[root_chars] = '\\';
    memcpy( path + root_chars + 1, payload_path,
            (payload_chars + 1) * sizeof(*path) );
    hr = storage_folder_create( path, value );
    free( path );
    return hr;
}

static inline struct package_id *impl_from_IPackageId( IPackageId *iface )
{
    return CONTAINING_RECORD( iface, struct package_id, IPackageId_iface );
}

static HRESULT WINAPI package_id_QueryInterface(
    IPackageId *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IPackageId ))
        *out = iface;
    else
        return E_NOINTERFACE;
    IPackageId_AddRef( iface );
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

static HRESULT WINAPI package_id_GetIids(
    IPackageId *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IPackageId};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_id_GetRuntimeClassName(
    IPackageId *iface, HSTRING *class_name )
{
    return runtime_class_name( RuntimeClass_Windows_ApplicationModel_PackageId,
                               class_name );
}

static HRESULT WINAPI package_id_GetTrustLevel(
    IPackageId *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

#define PACKAGE_ID_STRING_GETTER(name, member)                              \
    static HRESULT WINAPI name( IPackageId *iface, HSTRING *value )         \
    {                                                                        \
        struct package_id *impl = impl_from_IPackageId( iface );             \
        const struct appx_catalog_package *package = package_get_catalog(     \
            impl->snapshot, impl->index );                                    \
        if (!package) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );          \
        return duplicate_catalog_string( package->member, value );            \
    }

PACKAGE_ID_STRING_GETTER( package_id_get_Name, name )
PACKAGE_ID_STRING_GETTER( package_id_get_ResourceId, resource_id )
PACKAGE_ID_STRING_GETTER( package_id_get_Publisher, publisher )
PACKAGE_ID_STRING_GETTER( package_id_get_PublisherId, publisher_id )
PACKAGE_ID_STRING_GETTER( package_id_get_FullName, full_name )
PACKAGE_ID_STRING_GETTER( package_id_get_FamilyName, family_name )

static HRESULT WINAPI package_id_get_Version(
    IPackageId *iface, PackageVersion *value )
{
    struct package_id *impl = impl_from_IPackageId( iface );
    const struct appx_catalog_package *package;

    if (!value) return E_INVALIDARG;
    package = package_get_catalog( impl->snapshot, impl->index );
    if (!package) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    value->Major = package->version.major;
    value->Minor = package->version.minor;
    value->Build = package->version.build;
    value->Revision = package->version.revision;
    return S_OK;
}

static HRESULT WINAPI package_id_get_Architecture(
    IPackageId *iface, ProcessorArchitecture *value )
{
    struct package_id *impl = impl_from_IPackageId( iface );
    const struct appx_catalog_package *package;

    if (!value) return E_INVALIDARG;
    package = package_get_catalog( impl->snapshot, impl->index );
    if (!package) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    *value = catalog_architecture_to_winrt( package->architecture );
    if (*value == ProcessorArchitecture_Unknown)
        return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
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

static HRESULT package_id_create( struct package_snapshot *snapshot,
                                  UINT32 index, IPackageId **value )
{
    struct package_id *impl;

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (!package_get_catalog( snapshot, index ))
        return E_BOUNDS;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPackageId_iface.lpVtbl = &package_id_vtbl;
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

static HRESULT WINAPI package_QueryInterface(
    IPackage *iface, REFIID iid, void **out )
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
    else
        return E_NOINTERFACE;
    IPackage_AddRef( iface );
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

static HRESULT WINAPI package_GetIids(
    IPackage *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IPackage, IID_IPackage2};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_GetRuntimeClassName(
    IPackage *iface, HSTRING *class_name )
{
    return runtime_class_name( RuntimeClass_Windows_ApplicationModel_Package,
                               class_name );
}

static HRESULT WINAPI package_GetTrustLevel(
    IPackage *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI package_get_Id( IPackage *iface, IPackageId **value )
{
    struct package *impl = impl_from_IPackage( iface );
    return package_id_create( impl->snapshot, impl->index, value );
}

static HRESULT WINAPI package_get_InstalledLocation(
    IPackage *iface, IStorageFolder **value )
{
    struct package *impl = impl_from_IPackage( iface );
    const struct appx_catalog_package *package;

    if (!value) return E_INVALIDARG;
    *value = NULL;
    package = package_get_catalog( impl->snapshot, impl->index );
    if (!package) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    return installed_location_create( package->payload_path, value );
}

static HRESULT WINAPI package_get_IsFramework(
    IPackage *iface, boolean *value )
{
    struct package *impl = impl_from_IPackage( iface );
    const struct appx_catalog_package *package;

    if (!value) return E_INVALIDARG;
    package = package_get_catalog( impl->snapshot, impl->index );
    if (!package) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    *value = !!(package->flags & APPX_CATALOG_PACKAGE_FRAMEWORK);
    return S_OK;
}

static inline struct package_vector *impl_from_IVectorView_Package(
    IVectorView_Package *iface )
{
    return CONTAINING_RECORD( iface, struct package_vector,
                              IVectorView_Package_iface );
}

static HRESULT WINAPI package_vector_QueryInterface(
    IVectorView_Package *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_Package ))
        *out = iface;
    else
        return E_NOINTERFACE;
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
        for (i = 0; i < impl->count; i++)
            IPackage_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI package_vector_GetIids(
    IVectorView_Package *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IVectorView_Package};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_vector_GetRuntimeClassName(
    IVectorView_Package *iface, HSTRING *class_name )
{
    static const WCHAR name[] =
        L"Windows.Foundation.Collections.IVectorView`1<Windows.ApplicationModel.Package>";
    return runtime_class_name( name, class_name );
}

static HRESULT WINAPI package_vector_GetTrustLevel(
    IVectorView_Package *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI package_vector_GetAt(
    IVectorView_Package *iface, UINT32 index, IPackage **value )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (index >= impl->count) return E_BOUNDS;
    *value = impl->items[index];
    IPackage_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI package_vector_get_Size(
    IVectorView_Package *iface, UINT32 *value )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );

    if (!value) return E_INVALIDARG;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI package_vector_IndexOf(
    IVectorView_Package *iface, IPackage *element, UINT32 *index,
    boolean *found )
{
    struct package_vector *impl = impl_from_IVectorView_Package( iface );
    IUnknown *identity = NULL, *candidate = NULL;
    UINT32 i;
    HRESULT hr = S_OK;

    if (!index || !found) return E_INVALIDARG;
    *index = 0;
    *found = FALSE;
    if (!element) return S_OK;
    if (FAILED( hr = IPackage_QueryInterface(
            element, &IID_IUnknown, (void **)&identity ) ))
        return hr;
    for (i = 0; i < impl->count; i++)
    {
        if (FAILED( hr = IPackage_QueryInterface(
                impl->items[i], &IID_IUnknown, (void **)&candidate ) ))
            break;
        if (candidate == identity)
        {
            *index = i;
            *found = TRUE;
            IUnknown_Release( candidate );
            hr = S_OK;
            break;
        }
        IUnknown_Release( candidate );
        candidate = NULL;
    }
    IUnknown_Release( identity );
    return hr;
}

static HRESULT WINAPI package_vector_GetMany(
    IVectorView_Package *iface, UINT32 start_index, UINT32 items_size,
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

static HRESULT get_dependency_target_architecture(
    const struct appx_catalog_package *package,
    enum appx_catalog_architecture *target )
{
    SYSTEM_INFO info;

    switch (package->architecture)
    {
    case APPX_CATALOG_ARCHITECTURE_X86:
    case APPX_CATALOG_ARCHITECTURE_X64:
    case APPX_CATALOG_ARCHITECTURE_ARM:
    case APPX_CATALOG_ARCHITECTURE_ARM64:
    case APPX_CATALOG_ARCHITECTURE_X86A64:
        *target = package->architecture;
        return S_OK;
    case APPX_CATALOG_ARCHITECTURE_NEUTRAL:
        break;
    default:
        return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    }

    GetNativeSystemInfo( &info );
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_INTEL:
        *target = APPX_CATALOG_ARCHITECTURE_X86;
        return S_OK;
    case PROCESSOR_ARCHITECTURE_AMD64:
        *target = APPX_CATALOG_ARCHITECTURE_X64;
        return S_OK;
    case PROCESSOR_ARCHITECTURE_ARM:
        *target = APPX_CATALOG_ARCHITECTURE_ARM;
        return S_OK;
    case PROCESSOR_ARCHITECTURE_ARM64:
        *target = APPX_CATALOG_ARCHITECTURE_ARM64;
        return S_OK;
    default:
        return HRESULT_FROM_WIN32(
            ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
    }
}

static HRESULT package_vector_create_dependencies(
    struct package_snapshot *snapshot,
    const struct appx_catalog_package *package,
    IVectorView_Package **value )
{
    struct package_vector *impl;
    UINT32 package_indices[APPX_CATALOG_MAX_DEPENDENCIES_PER_PACKAGE];
    enum appx_catalog_architecture target;
    UINT32 count, i;
    HRESULT hr = S_OK;

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (FAILED( hr = get_dependency_target_architecture(
            package, &target ) ) ||
        FAILED( hr = appx_backend_resolve_direct_dependencies(
            snapshot->catalog, package->full_name, target,
            ARRAY_SIZE(package_indices), package_indices, &count ) ))
        return hr;
    if (count != package->dependency_count)
        return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_Package_iface.lpVtbl = &package_vector_vtbl;
    impl->ref = 1;
    if (count &&
        !(impl->items = calloc( count,
                                sizeof(*impl->items) )))
    {
        free( impl );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++)
    {
        if (FAILED( hr = package_create_from_catalog(
                snapshot, package_indices[i],
                &impl->items[impl->count] ) ))
            goto failed;
        impl->count++;
    }
    *value = &impl->IVectorView_Package_iface;
    return S_OK;

failed:
    IVectorView_Package_Release( &impl->IVectorView_Package_iface );
    return hr;
}

static HRESULT WINAPI package_get_Dependencies(
    IPackage *iface, IVectorView_Package **value )
{
    struct package *impl = impl_from_IPackage( iface );
    const struct appx_catalog_package *package;

    if (!value) return E_INVALIDARG;
    *value = NULL;
    package = package_get_catalog( impl->snapshot, impl->index );
    if (!package) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    return package_vector_create_dependencies( impl->snapshot, package, value );
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

static inline struct package *impl_from_IPackage2( IPackage2 *iface )
{
    return CONTAINING_RECORD( iface, struct package, IPackage2_iface );
}

static HRESULT WINAPI package2_QueryInterface(
    IPackage2 *iface, REFIID iid, void **out )
{
    struct package *impl = impl_from_IPackage2( iface );
    return IPackage_QueryInterface( &impl->IPackage_iface, iid, out );
}

static ULONG WINAPI package2_AddRef( IPackage2 *iface )
{
    struct package *impl = impl_from_IPackage2( iface );
    return IPackage_AddRef( &impl->IPackage_iface );
}

static ULONG WINAPI package2_Release( IPackage2 *iface )
{
    struct package *impl = impl_from_IPackage2( iface );
    return IPackage_Release( &impl->IPackage_iface );
}

static HRESULT WINAPI package2_GetIids(
    IPackage2 *iface, ULONG *iid_count, IID **iids )
{
    struct package *impl = impl_from_IPackage2( iface );
    return IPackage_GetIids(
        &impl->IPackage_iface, iid_count, iids );
}

static HRESULT WINAPI package2_GetRuntimeClassName(
    IPackage2 *iface, HSTRING *class_name )
{
    struct package *impl = impl_from_IPackage2( iface );
    return IPackage_GetRuntimeClassName(
        &impl->IPackage_iface, class_name );
}

static HRESULT WINAPI package2_GetTrustLevel(
    IPackage2 *iface, TrustLevel *trust_level )
{
    struct package *impl = impl_from_IPackage2( iface );
    return IPackage_GetTrustLevel(
        &impl->IPackage_iface, trust_level );
}

static HRESULT package2_unavailable_string( HSTRING *value )
{
    if (!value) return E_INVALIDARG;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package2_get_DisplayName(
    IPackage2 *iface, HSTRING *value )
{
    return package2_unavailable_string( value );
}

static HRESULT WINAPI package2_get_PublisherDisplayName(
    IPackage2 *iface, HSTRING *value )
{
    return package2_unavailable_string( value );
}

static HRESULT WINAPI package2_get_Description(
    IPackage2 *iface, HSTRING *value )
{
    return package2_unavailable_string( value );
}

static HRESULT WINAPI package2_get_Logo(
    IPackage2 *iface, IUriRuntimeClass **value )
{
    if (!value) return E_INVALIDARG;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI package2_get_IsResourcePackage(
    IPackage2 *iface, boolean *value )
{
    struct package *impl = impl_from_IPackage2( iface );
    const struct appx_catalog_package *package;

    if (!value) return E_INVALIDARG;
    package = package_get_catalog( impl->snapshot, impl->index );
    if (!package) return HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
    *value = !!(package->flags & APPX_CATALOG_PACKAGE_RESOURCE);
    return S_OK;
}

static HRESULT WINAPI package2_get_IsBundle(
    IPackage2 *iface, boolean *value )
{
    if (!value) return E_INVALIDARG;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI package2_get_IsDevelopmentMode(
    IPackage2 *iface, boolean *value )
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

static HRESULT package_create_from_catalog(
    struct package_snapshot *snapshot, UINT32 index, IPackage **value )
{
    struct package *impl;

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (!package_get_catalog( snapshot, index )) return E_BOUNDS;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IPackage_iface.lpVtbl = &package_vtbl;
    impl->IPackage2_iface.lpVtbl = &package2_vtbl;
    impl->ref = 1;
    impl->snapshot = package_snapshot_addref( snapshot );
    impl->index = index;
    *value = &impl->IPackage_iface;
    return S_OK;
}

static inline struct package_iterable *impl_from_IIterable_Package(
    IIterable_Package *iface )
{
    return CONTAINING_RECORD( iface, struct package_iterable,
                              IIterable_Package_iface );
}

static HRESULT WINAPI package_iterable_QueryInterface(
    IIterable_Package *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterable_Package ))
        *out = iface;
    else
        return E_NOINTERFACE;
    IIterable_Package_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI package_iterable_AddRef( IIterable_Package *iface )
{
    struct package_iterable *impl = impl_from_IIterable_Package( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI package_iterable_Release( IIterable_Package *iface )
{
    struct package_iterable *impl = impl_from_IIterable_Package( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    UINT32 i;

    if (!ref)
    {
        for (i = 0; i < impl->count; i++)
            IPackage_Release( impl->items[i] );
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI package_iterable_GetIids(
    IIterable_Package *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IIterable_Package};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_iterable_GetRuntimeClassName(
    IIterable_Package *iface, HSTRING *class_name )
{
    static const WCHAR name[] =
        L"Windows.Foundation.Collections.IIterable`1<Windows.ApplicationModel.Package>";
    return runtime_class_name( name, class_name );
}

static HRESULT WINAPI package_iterable_GetTrustLevel(
    IIterable_Package *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static inline struct package_iterator *impl_from_IIterator_Package(
    IIterator_Package *iface )
{
    return CONTAINING_RECORD( iface, struct package_iterator,
                              IIterator_Package_iface );
}

static HRESULT WINAPI package_iterator_QueryInterface(
    IIterator_Package *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterator_Package ))
        *out = iface;
    else
        return E_NOINTERFACE;
    IIterator_Package_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI package_iterator_AddRef( IIterator_Package *iface )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI package_iterator_Release( IIterator_Package *iface )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IIterable_Package_Release( &impl->iterable->IIterable_Package_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI package_iterator_GetIids(
    IIterator_Package *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IIterator_Package};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI package_iterator_GetRuntimeClassName(
    IIterator_Package *iface, HSTRING *class_name )
{
    static const WCHAR name[] =
        L"Windows.Foundation.Collections.IIterator`1<Windows.ApplicationModel.Package>";
    return runtime_class_name( name, class_name );
}

static HRESULT WINAPI package_iterator_GetTrustLevel(
    IIterator_Package *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI package_iterator_get_Current(
    IIterator_Package *iface, IPackage **value )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (impl->index >= impl->iterable->count) return E_BOUNDS;
    *value = impl->iterable->items[impl->index];
    IPackage_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI package_iterator_get_HasCurrent(
    IIterator_Package *iface, boolean *value )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );

    if (!value) return E_INVALIDARG;
    *value = impl->index < impl->iterable->count;
    return S_OK;
}

static HRESULT WINAPI package_iterator_MoveNext(
    IIterator_Package *iface, boolean *value )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );

    if (!value) return E_INVALIDARG;
    if (impl->index < impl->iterable->count) impl->index++;
    *value = impl->index < impl->iterable->count;
    return S_OK;
}

static HRESULT WINAPI package_iterator_GetMany(
    IIterator_Package *iface, UINT32 items_size, IPackage **items,
    UINT32 *value )
{
    struct package_iterator *impl = impl_from_IIterator_Package( iface );
    UINT32 count, i;

    if (!value || (items_size && !items)) return E_INVALIDARG;
    *value = 0;
    if (impl->index >= impl->iterable->count) return S_OK;
    count = min( items_size, impl->iterable->count - impl->index );
    for (i = 0; i < count; i++)
    {
        items[i] = impl->iterable->items[impl->index + i];
        IPackage_AddRef( items[i] );
    }
    impl->index += count;
    *value = count;
    return S_OK;
}

static const IIterator_PackageVtbl package_iterator_vtbl =
{
    package_iterator_QueryInterface,
    package_iterator_AddRef,
    package_iterator_Release,
    package_iterator_GetIids,
    package_iterator_GetRuntimeClassName,
    package_iterator_GetTrustLevel,
    package_iterator_get_Current,
    package_iterator_get_HasCurrent,
    package_iterator_MoveNext,
    package_iterator_GetMany,
};

static HRESULT WINAPI package_iterable_First(
    IIterable_Package *iface, IIterator_Package **value )
{
    struct package_iterable *iterable = impl_from_IIterable_Package( iface );
    struct package_iterator *impl;

    if (!value) return E_INVALIDARG;
    *value = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IIterator_Package_iface.lpVtbl = &package_iterator_vtbl;
    impl->ref = 1;
    impl->iterable = iterable;
    IIterable_Package_AddRef( iface );
    *value = &impl->IIterator_Package_iface;
    return S_OK;
}

static const IIterable_PackageVtbl package_iterable_vtbl =
{
    package_iterable_QueryInterface,
    package_iterable_AddRef,
    package_iterable_Release,
    package_iterable_GetIids,
    package_iterable_GetRuntimeClassName,
    package_iterable_GetTrustLevel,
    package_iterable_First,
};

static BOOL package_type_matches( const struct appx_catalog_package *package,
                                  PackageTypes types )
{
    UINT32 mask = types;
    UINT32 package_type;

    if (!mask) return FALSE;
    if (package->flags & APPX_CATALOG_PACKAGE_RESOURCE)
        package_type = PackageTypes_Resource;
    else if (package->flags & APPX_CATALOG_PACKAGE_FRAMEWORK)
        package_type = PackageTypes_Framework;
    else
        package_type = PackageTypes_Main;
    return !!(mask & package_type);
}

static HRESULT package_filter_matches(
    const struct appx_catalog_package *package,
    const struct package_query_filter *filter, BOOL *matches )
{
    BOOL equal;
    HRESULT hr;

    *matches = FALSE;
    if (!(package->flags & APPX_CATALOG_PACKAGE_ACTIVE) ||
        !package_type_matches( package, filter->types ))
        return S_OK;
    if (filter->name)
    {
        if (FAILED( hr = compare_catalog_string(
                package->name, filter->name, &equal ) ))
            return hr;
        if (!equal) return S_OK;
    }
    if (filter->publisher)
    {
        if (FAILED( hr = compare_catalog_string(
                package->publisher, filter->publisher, &equal ) ))
            return hr;
        if (!equal) return S_OK;
    }
    if (filter->family_name)
    {
        if (FAILED( hr = compare_catalog_string(
                package->family_name, filter->family_name, &equal ) ))
            return hr;
        if (!equal) return S_OK;
    }
    *matches = TRUE;
    return S_OK;
}

HRESULT package_iterable_create( APPX_CATALOG_SNAPSHOT *catalog,
                                 const struct package_query_filter *filter,
                                 IIterable_Package **value )
{
    struct package_iterable *impl = NULL;
    struct package_snapshot *snapshot = NULL;
    const struct appx_catalog_package *package;
    UINT32 count, i;
    BOOL matches;
    HRESULT hr;

    if (!catalog || !filter || !value)
    {
        appx_backend_catalog_snapshot_free( catalog );
        return E_INVALIDARG;
    }
    *value = NULL;
    if (FAILED( hr = package_snapshot_create( catalog, &snapshot ) ))
        return hr;
    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        package_snapshot_release( snapshot );
        return E_OUTOFMEMORY;
    }
    impl->IIterable_Package_iface.lpVtbl = &package_iterable_vtbl;
    impl->ref = 1;
    count = appx_backend_catalog_snapshot_get_package_count(
        snapshot->catalog );
    if (count && !(impl->items = calloc( count, sizeof(*impl->items) )))
    {
        package_snapshot_release( snapshot );
        free( impl );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++)
    {
        package = package_get_catalog( snapshot, i );
        if (!package)
        {
            hr = HRESULT_FROM_WIN32( ERROR_BAD_FORMAT );
            goto failed;
        }
        if (FAILED( hr = package_filter_matches(
                package, filter, &matches ) ))
            goto failed;
        if (!matches) continue;
        if (FAILED( hr = package_create_from_catalog(
                snapshot, i, &impl->items[impl->count] ) ))
            goto failed;
        impl->count++;
    }
    package_snapshot_release( snapshot );
    *value = &impl->IIterable_Package_iface;
    return S_OK;

failed:
    package_snapshot_release( snapshot );
    IIterable_Package_Release( &impl->IIterable_Package_iface );
    return hr;
}

HRESULT package_from_catalog_create( APPX_CATALOG_SNAPSHOT *catalog,
                                     UINT32 index, IPackage **value )
{
    struct package_snapshot *snapshot;
    HRESULT hr;

    if (!catalog || !value)
    {
        appx_backend_catalog_snapshot_free( catalog );
        return E_INVALIDARG;
    }
    *value = NULL;
    if (FAILED( hr = package_snapshot_create( catalog, &snapshot ) ))
        return hr;
    hr = package_create_from_catalog( snapshot, index, value );
    package_snapshot_release( snapshot );
    return hr;
}
