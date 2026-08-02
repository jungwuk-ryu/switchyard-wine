/*
 * Windows.ApplicationModel.Package tests
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "winternl.h"

#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_System
#include "windows.foundation.h"
#define WIDL_using_Windows_ApplicationModel
#define WIDL_using_Windows_Storage
#include "windows.applicationmodel.h"

#include "wine/appx_package_graph.h"
#include "wine/test.h"

#define GRAPH_PACKAGE_VERSION_OFFSET       0
#define GRAPH_PACKAGE_ARCHITECTURE_OFFSET  8
#define GRAPH_PACKAGE_FLAGS_OFFSET         12
#define GRAPH_PACKAGE_INDEX_OFFSET         16
#define GRAPH_PACKAGE_NAME_REF_OFFSET      56
#define GRAPH_PACKAGE_PUBLISHER_REF_OFFSET 64
#define GRAPH_PACKAGE_RESOURCE_REF_OFFSET  72
#define GRAPH_PACKAGE_PUBLISHER_ID_OFFSET  80
#define GRAPH_PACKAGE_FULL_NAME_OFFSET     88
#define GRAPH_PACKAGE_FAMILY_NAME_OFFSET   96
#define GRAPH_PACKAGE_ROOT_OFFSET          104

static void write_u32( BYTE *data, UINT32 value )
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static void write_u64( BYTE *data, UINT64 value )
{
    write_u32( data, value );
    write_u32( data + 4, value >> 32 );
}

static void append_string( BYTE *blob, UINT32 *cursor, BYTE *record,
                           UINT32 ref_offset, const WCHAR *value )
{
    UINT32 chars = wcslen(value) + 1;

    write_u32( record + ref_offset, *cursor );
    write_u32( record + ref_offset + 4, chars );
    memcpy( blob + *cursor, value, chars * sizeof(WCHAR) );
    *cursor += chars * sizeof(WCHAR);
}

static BYTE *create_test_graph( UINT32 *size, boolean include_direct )
{
    static const WCHAR *names[] =
    {
        L"Wine.Projection.Head",
        L"Wine.Projection.Framework",
        L"Wine.Projection.Transitive",
    };
    static const WCHAR *full_names[] =
    {
        L"Wine.Projection.Head_1.2.3.4_neutral__1234567890123",
        L"Wine.Projection.Framework_2.0.0.0_neutral__1234567890123",
        L"Wine.Projection.Transitive_3.0.0.0_neutral__1234567890123",
    };
    static const WCHAR *family_names[] =
    {
        L"Wine.Projection.Head_1234567890123",
        L"Wine.Projection.Framework_1234567890123",
        L"Wine.Projection.Transitive_1234567890123",
    };
    static const WCHAR *roots[] =
    {
        L"C:\\Program Files\\WindowsApps\\Wine.Projection.Head_1.2.3.4_neutral__1234567890123",
        L"C:\\Program Files\\WindowsApps\\Wine.Projection.Framework_2.0.0.0_neutral__1234567890123",
        L"C:\\Program Files\\WindowsApps\\Wine.Projection.Transitive_3.0.0.0_neutral__1234567890123",
    };
    static const WCHAR publisher[] = L"CN=Wine Projection";
    static const WCHAR publisher_id[] = L"1234567890123";
    static const WCHAR aumid[] = L"Wine.Projection.Head_1234567890123!App";
    static const UINT32 flags[] =
    {
        WINE_APPX_GRAPH_PACKAGE_ACTIVE | WINE_APPX_GRAPH_PACKAGE_SIGNED,
        WINE_APPX_GRAPH_PACKAGE_ACTIVE | WINE_APPX_GRAPH_PACKAGE_SIGNED |
            WINE_APPX_GRAPH_PACKAGE_FRAMEWORK,
        WINE_APPX_GRAPH_PACKAGE_ACTIVE | WINE_APPX_GRAPH_PACKAGE_SIGNED |
            WINE_APPX_GRAPH_PACKAGE_FRAMEWORK,
    };
    static const UINT64 versions[] =
    {
        1ull | (2ull << 16) | (3ull << 32) | (4ull << 48),
        2ull,
        3ull,
    };
    const UINT32 package_count = ARRAY_SIZE(names);
    const UINT32 packages_offset = WINE_APPX_GRAPH_BLOB_HEADER_SIZE;
    const UINT32 loaders_offset = packages_offset +
        package_count * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    const UINT32 classes_offset = loaders_offset;
    const UINT32 strings_offset = classes_offset;
    BYTE *blob, *record;
    UINT32 cursor, i;

    if (!(blob = calloc( 1, 4096 ))) return NULL;
    memcpy( blob, "SWXGRAPH", 8 );
    write_u32( blob + 8, WINE_APPX_GRAPH_BLOB_VERSION );
    write_u32( blob + 12, WINE_APPX_GRAPH_BLOB_HEADER_SIZE );
    write_u32( blob + 40, WINE_APPX_GRAPH_CURRENT_ARCHITECTURE );
    write_u32( blob + 44, package_count );
    write_u32( blob + 48, packages_offset );
    write_u32( blob + 52, 0 );
    write_u32( blob + 56, loaders_offset );
    write_u32( blob + 60, strings_offset );
    write_u32( blob + 68, 1 );
    write_u32( blob + WINE_APPX_GRAPH_HEADER_CLASS_COUNT_OFFSET, 0 );
    write_u32( blob + WINE_APPX_GRAPH_HEADER_CLASSES_OFFSET, classes_offset );
    write_u32( blob + WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET, 1 );
    write_u32( blob + WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET, 1 );
    for (i = 0; i < WINE_APPX_GRAPH_OBJECT_ID_SIZE; i++)
        blob[WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET + i] = 0x20 + i;

    cursor = strings_offset;
    append_string( blob, &cursor, blob, 72, L"App" );
    append_string( blob, &cursor, blob, 80, aumid );
    append_string( blob, &cursor, blob, 88, L"application.exe" );
    append_string( blob, &cursor, blob, 96, L"" );
    for (i = 0; i < package_count; i++)
    {
        record = blob + packages_offset +
                 i * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
        write_u64( record + GRAPH_PACKAGE_VERSION_OFFSET, versions[i] );
        write_u32( record + GRAPH_PACKAGE_ARCHITECTURE_OFFSET, 0 );
        write_u32( record + GRAPH_PACKAGE_FLAGS_OFFSET,
                   flags[i] | (i == 1 && include_direct ?
                               WINE_APPX_GRAPH_PACKAGE_DIRECT : 0) );
        write_u32( record + GRAPH_PACKAGE_INDEX_OFFSET, i );
        append_string( blob, &cursor, record, GRAPH_PACKAGE_NAME_REF_OFFSET,
                       names[i] );
        append_string( blob, &cursor, record, GRAPH_PACKAGE_PUBLISHER_REF_OFFSET,
                       publisher );
        append_string( blob, &cursor, record, GRAPH_PACKAGE_RESOURCE_REF_OFFSET,
                       L"" );
        append_string( blob, &cursor, record, GRAPH_PACKAGE_PUBLISHER_ID_OFFSET,
                       publisher_id );
        append_string( blob, &cursor, record, GRAPH_PACKAGE_FULL_NAME_OFFSET,
                       full_names[i] );
        append_string( blob, &cursor, record, GRAPH_PACKAGE_FAMILY_NAME_OFFSET,
                       family_names[i] );
        append_string( blob, &cursor, record, GRAPH_PACKAGE_ROOT_OFFSET, roots[i] );
    }
    write_u32( blob + WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET, cursor );
    write_u32( blob + 64, cursor - strings_offset );
    *size = cursor;
    return blob;
}

static HRESULT get_package_factory( IActivationFactory **value )
{
    HSTRING class_name;
    HRESULT hr;

    hr = WindowsCreateString( RuntimeClass_Windows_ApplicationModel_Package,
                              wcslen(RuntimeClass_Windows_ApplicationModel_Package),
                              &class_name );
    ok( hr == S_OK, "WindowsCreateString returned %#lx.\n", hr );
    if (FAILED(hr)) return hr;
    hr = RoGetActivationFactory( class_name, &IID_IActivationFactory,
                                 (void **)value );
    WindowsDeleteString( class_name );
    return hr;
}

static void check_hstring( HSTRING value, const WCHAR *expected )
{
    const WCHAR *buffer;
    UINT32 length;

    buffer = WindowsGetStringRawBuffer( value, &length );
    ok( length == wcslen(expected), "Got length %u, expected %u.\n",
        length, (unsigned int)wcslen(expected) );
    ok( !wcscmp( buffer, expected ), "Got %s, expected %s.\n",
        debugstr_w(buffer), debugstr_w(expected) );
}

static void test_unpacked_and_factory( IActivationFactory *factory,
                                       IPackageStatics *statics )
{
    RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
    void *saved_graph = params->PackageDependencyData;
    BYTE invalid_graph[WINE_APPX_GRAPH_BLOB_HEADER_SIZE] = {0};
    IPackage *package = (IPackage *)0xdeadbeef;
    TrustLevel trust = FullTrust;
    HSTRING class_name = NULL;
    IID *iids = NULL;
    ULONG iid_count = 0;
    HRESULT hr;

    hr = IActivationFactory_GetIids( factory, &iid_count, &iids );
    ok( hr == S_OK, "GetIids returned %#lx.\n", hr );
    ok( iid_count == 1, "Got %lu IIDs.\n", iid_count );
    if (iid_count) ok( IsEqualGUID( &iids[0], &IID_IPackageStatics ),
                       "Got unexpected IID %s.\n", debugstr_guid(&iids[0]) );
    CoTaskMemFree( iids );

    hr = IActivationFactory_GetRuntimeClassName( factory, &class_name );
    ok( hr == S_OK, "GetRuntimeClassName returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        check_hstring( class_name, RuntimeClass_Windows_ApplicationModel_Package );
        WindowsDeleteString( class_name );
    }
    hr = IActivationFactory_GetTrustLevel( factory, &trust );
    ok( hr == S_OK, "GetTrustLevel returned %#lx.\n", hr );
    ok( trust == BaseTrust, "Got trust %u.\n", trust );

    if (saved_graph)
    {
        win_skip( "Test process already has package identity.\n" );
        return;
    }

    hr = IPackageStatics_get_Current( statics, &package );
    ok( hr == HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE),
        "get_Current returned %#lx.\n", hr );
    ok( !package, "Got package %p.\n", package );

    if (strcmp( winetest_platform, "wine" )) return;
    params->PackageDependencyData = invalid_graph;
    package = (IPackage *)0xdeadbeef;
    hr = IPackageStatics_get_Current( statics, &package );
    ok( hr == HRESULT_FROM_WIN32(APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT),
        "Malformed graph returned %#lx.\n", hr );
    ok( !package, "Got package %p from malformed graph.\n", package );
    params->PackageDependencyData = saved_graph;
}

static void test_package_id( IPackage *package )
{
    static const WCHAR expected_name[] = L"Wine.Projection.Head";
    static const WCHAR expected_full_name[] =
        L"Wine.Projection.Head_1.2.3.4_neutral__1234567890123";
    static const WCHAR expected_family_name[] =
        L"Wine.Projection.Head_1234567890123";
    IPackageIdWithMetadata *metadata = (IPackageIdWithMetadata *)0xdeadbeef;
    ProcessorArchitecture architecture = ProcessorArchitecture_Unknown;
    HSTRING name = NULL, duplicate = NULL, value = NULL;
    PackageVersion version = {0};
    IPackageId *id = NULL;
    HRESULT hr;

    hr = IPackage_get_Id( package, &id );
    ok( hr == S_OK, "get_Id returned %#lx.\n", hr );
    hr = IPackageId_QueryInterface( id, &IID_IPackageIdWithMetadata,
                                    (void **)&metadata );
    ok( hr == S_OK, "Metadata QI returned %#lx.\n", hr );
    value = (HSTRING)0xdeadbeef;
    hr = IPackageIdWithMetadata_get_ProductId( metadata, &value );
    ok( hr == E_NOTIMPL && !value, "get_ProductId returned %#lx, %p.\n",
        hr, value );
    value = (HSTRING)0xdeadbeef;
    hr = IPackageIdWithMetadata_get_Author( metadata, &value );
    ok( hr == E_NOTIMPL && !value, "get_Author returned %#lx, %p.\n",
        hr, value );
    IPackageIdWithMetadata_Release( metadata );

    hr = IPackageId_get_Name( id, &name );
    ok( hr == S_OK, "get_Name returned %#lx.\n", hr );
    hr = IPackageId_get_Name( id, &duplicate );
    ok( hr == S_OK, "second get_Name returned %#lx.\n", hr );
    WindowsDeleteString( name );
    check_hstring( duplicate, expected_name );
    WindowsDeleteString( duplicate );

    hr = IPackageId_get_Version( id, &version );
    ok( hr == S_OK, "get_Version returned %#lx.\n", hr );
    ok( version.Major == 1 && version.Minor == 2 && version.Build == 3 &&
        version.Revision == 4, "Got version %u.%u.%u.%u.\n",
        version.Major, version.Minor, version.Build, version.Revision );
    hr = IPackageId_get_Architecture( id, &architecture );
    ok( hr == S_OK, "get_Architecture returned %#lx.\n", hr );
    ok( architecture == ProcessorArchitecture_Neutral,
        "Got architecture %u.\n", architecture );

    hr = IPackageId_get_ResourceId( id, &value );
    ok( hr == S_OK, "get_ResourceId returned %#lx.\n", hr );
    check_hstring( value, L"" );
    WindowsDeleteString( value );
    hr = IPackageId_get_Publisher( id, &value );
    ok( hr == S_OK, "get_Publisher returned %#lx.\n", hr );
    check_hstring( value, L"CN=Wine Projection" );
    WindowsDeleteString( value );
    hr = IPackageId_get_PublisherId( id, &value );
    ok( hr == S_OK, "get_PublisherId returned %#lx.\n", hr );
    check_hstring( value, L"1234567890123" );
    WindowsDeleteString( value );
    hr = IPackageId_get_FullName( id, &value );
    ok( hr == S_OK, "get_FullName returned %#lx.\n", hr );
    check_hstring( value, expected_full_name );
    WindowsDeleteString( value );
    hr = IPackageId_get_FamilyName( id, &value );
    ok( hr == S_OK, "get_FamilyName returned %#lx.\n", hr );
    check_hstring( value, expected_family_name );
    WindowsDeleteString( value );
    IPackageId_Release( id );
}

static void test_installed_location( IPackage *package )
{
    static const WCHAR expected_full_name[] =
        L"Wine.Projection.Head_1.2.3.4_neutral__1234567890123";
    static const WCHAR expected_path[] =
        L"C:\\Program Files\\WindowsApps\\Wine.Projection.Head_1.2.3.4_neutral__1234567890123";
    IStorageFolder *folder = NULL;
    IStorageItem *item = NULL;
    IAgileObject *agile = NULL;
    FileAttributes attributes = 0;
    StorageItemTypes type;
    boolean result = FALSE;
    HSTRING value = NULL;
    IID *iids = NULL;
    ULONG count = 0;
    HRESULT hr;

    hr = IPackage_get_InstalledLocation( package, &folder );
    ok( hr == S_OK, "get_InstalledLocation returned %#lx.\n", hr );
    hr = IStorageFolder_QueryInterface( folder, &IID_IAgileObject,
                                        (void **)&agile );
    ok( hr == S_OK, "Agile QI returned %#lx.\n", hr );
    if (agile) IAgileObject_Release( agile );
    hr = IStorageFolder_GetIids( folder, &count, &iids );
    ok( hr == S_OK, "GetIids returned %#lx.\n", hr );
    ok( count == 2, "Got %lu IIDs.\n", count );
    CoTaskMemFree( iids );

    hr = IStorageFolder_QueryInterface( folder, &IID_IStorageItem,
                                        (void **)&item );
    ok( hr == S_OK, "StorageItem QI returned %#lx.\n", hr );
    hr = IStorageItem_get_Path( item, &value );
    ok( hr == S_OK, "get_Path returned %#lx.\n", hr );
    check_hstring( value, expected_path );
    WindowsDeleteString( value );
    hr = IStorageItem_get_Name( item, &value );
    ok( hr == S_OK, "get_Name returned %#lx.\n", hr );
    check_hstring( value, expected_full_name );
    WindowsDeleteString( value );
    hr = IStorageItem_get_Attributes( item, &attributes );
    ok( hr == S_OK, "get_Attributes returned %#lx.\n", hr );
    ok( attributes == FileAttributes_Directory, "Got attributes %#x.\n",
        attributes );
    type = StorageItemTypes_Folder;
    hr = IStorageItem_IsOfType( item, type, &result );
    ok( hr == S_OK && result, "IsOfType returned %#lx, %u.\n", hr, result );
    IStorageItem_Release( item );
    IStorageFolder_Release( folder );
}

static void test_dependencies( IPackage *package )
{
    static const WCHAR vector_class_name[] =
        L"Windows.Foundation.Collections.IVectorView`1<"
        L"Windows.ApplicationModel.Package>";
    IVectorView_Package *dependencies = NULL;
    IPackage *dependency = NULL, *many[2] = {NULL};
    boolean framework = FALSE, found = FALSE;
    HSTRING class_name = NULL;
    UINT32 size = 0, index = ~0u, count = 0;
    HRESULT hr;

    hr = IPackage_get_Dependencies( package, &dependencies );
    ok( hr == S_OK, "get_Dependencies returned %#lx.\n", hr );
    hr = IVectorView_Package_GetRuntimeClassName( dependencies, &class_name );
    ok( hr == S_OK, "GetRuntimeClassName returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        check_hstring( class_name, vector_class_name );
        WindowsDeleteString( class_name );
    }
    hr = IVectorView_Package_get_Size( dependencies, &size );
    ok( hr == S_OK && size == 1, "get_Size returned %#lx, %u.\n", hr, size );
    hr = IVectorView_Package_GetAt( dependencies, 1, &dependency );
    ok( hr == E_BOUNDS && !dependency, "GetAt(1) returned %#lx, %p.\n",
        hr, dependency );
    hr = IVectorView_Package_GetAt( dependencies, 0, &dependency );
    ok( hr == S_OK, "GetAt returned %#lx.\n", hr );
    hr = IPackage_get_IsFramework( dependency, &framework );
    ok( hr == S_OK && framework, "get_IsFramework returned %#lx, %u.\n",
        hr, framework );
    hr = IVectorView_Package_IndexOf( dependencies, dependency, &index, &found );
    ok( hr == S_OK && found && !index,
        "IndexOf returned %#lx, %u, %u.\n", hr, index, found );
    hr = IVectorView_Package_GetMany( dependencies, 0, ARRAY_SIZE(many),
                                     many, &count );
    ok( hr == S_OK && count == 1, "GetMany returned %#lx, %u.\n", hr, count );
    if (count) IPackage_Release( many[0] );
    IVectorView_Package_Release( dependencies );

    dependencies = (IVectorView_Package *)0xdeadbeef;
    hr = IPackage_get_Dependencies( dependency, &dependencies );
    ok( hr == E_NOTIMPL && !dependencies,
        "Framework dependencies returned %#lx, %p.\n", hr, dependencies );
    IPackage_Release( dependency );
}

static void test_package_interfaces( IPackage *package )
{
    IAsyncOperation_IVectorView_AppListEntry *operation =
        (IAsyncOperation_IVectorView_AppListEntry *)0xdeadbeef;
    IUnknown *identity = NULL, *secondary_identity = NULL;
    IPackageStatus *status = (IPackageStatus *)0xdeadbeef;
    IPackage2 *package2 = NULL;
    IPackage3 *package3 = NULL;
    boolean value = TRUE;
    DateTime date = {1};
    HSTRING string = (HSTRING)0xdeadbeef;
    IID *iids = NULL;
    ULONG count = 0;
    HRESULT hr;

    hr = IPackage_GetIids( package, &count, &iids );
    ok( hr == S_OK, "GetIids returned %#lx.\n", hr );
    ok( count == 3, "Got %lu IIDs.\n", count );
    CoTaskMemFree( iids );
    hr = IPackage_QueryInterface( package, &IID_IPackage2, (void **)&package2 );
    ok( hr == S_OK, "IPackage2 QI returned %#lx.\n", hr );
    hr = IPackage_QueryInterface( package, &IID_IPackage3, (void **)&package3 );
    ok( hr == S_OK, "IPackage3 QI returned %#lx.\n", hr );
    hr = IPackage_QueryInterface( package, &IID_IUnknown, (void **)&identity );
    ok( hr == S_OK, "IUnknown QI returned %#lx.\n", hr );
    hr = IPackage2_QueryInterface( package2, &IID_IUnknown,
                                   (void **)&secondary_identity );
    ok( hr == S_OK, "secondary IUnknown QI returned %#lx.\n", hr );
    ok( identity == secondary_identity, "COM identity differs, %p vs %p.\n",
        identity, secondary_identity );
    IUnknown_Release( secondary_identity );
    IUnknown_Release( identity );

    hr = IPackage2_get_DisplayName( package2, &string );
    ok( hr == E_NOTIMPL && !string, "get_DisplayName returned %#lx, %p.\n",
        hr, string );
    hr = IPackage2_get_IsResourcePackage( package2, &value );
    ok( hr == S_OK && !value, "get_IsResourcePackage returned %#lx, %u.\n",
        hr, value );
    value = TRUE;
    hr = IPackage2_get_IsBundle( package2, &value );
    ok( hr == E_NOTIMPL && !value, "get_IsBundle returned %#lx, %u.\n",
        hr, value );
    value = TRUE;
    hr = IPackage2_get_IsDevelopmentMode( package2, &value );
    ok( hr == S_OK && !value, "get_IsDevelopmentMode returned %#lx, %u.\n",
        hr, value );

    hr = IPackage3_get_Status( package3, &status );
    ok( hr == E_NOTIMPL && !status, "get_Status returned %#lx, %p.\n",
        hr, status );
    hr = IPackage3_get_InstalledDate( package3, &date );
    ok( hr == E_NOTIMPL && !date.UniversalTime,
        "get_InstalledDate returned %#lx, %s.\n", hr,
        wine_dbgstr_longlong(date.UniversalTime) );
    hr = IPackage3_GetAppListEntriesAsync( package3, &operation );
    ok( hr == E_NOTIMPL && !operation,
        "GetAppListEntriesAsync returned %#lx, %p.\n", hr, operation );

    IPackage3_Release( package3 );
    IPackage2_Release( package2 );
}

static void test_packaged_projection( IPackageStatics *statics )
{
    RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
    void *saved_graph = params->PackageDependencyData;
    IPackage *package = NULL;
    BYTE *graph;
    UINT32 graph_size;
    boolean framework = TRUE;
    HRESULT hr;

    if (saved_graph || strcmp( winetest_platform, "wine" ))
    {
        win_skip( "Synthetic graph injection is Wine-only and requires an unpackaged process.\n" );
        return;
    }
    graph = create_test_graph( &graph_size, TRUE );
    ok( !!graph, "Failed to allocate graph.\n" );
    if (!graph) return;
    ok( wine_appx_graph_validate_blob( graph, graph_size ),
        "Generated invalid package graph.\n" );

    params->PackageDependencyData = graph;
    hr = IPackageStatics_get_Current( statics, &package );
    params->PackageDependencyData = saved_graph;
    ok( hr == S_OK, "get_Current returned %#lx.\n", hr );
    ok( !!package, "Got no package.\n" );
    if (!package) return;

    hr = IPackage_get_IsFramework( package, &framework );
    ok( hr == S_OK && !framework, "get_IsFramework returned %#lx, %u.\n",
        hr, framework );
    test_package_id( package );
    test_installed_location( package );
    test_dependencies( package );
    test_package_interfaces( package );
    IPackage_Release( package );

    /*
     * kernelbase caches validated immutable graph pointers.  Keep this small
     * test graph alive until process teardown instead of leaving a dangling
     * cache entry.
     */
}

static void test_empty_dependency_vector( IPackageStatics *statics )
{
    static const WCHAR vector_class_name[] =
        L"Windows.Foundation.Collections.IVectorView`1<"
        L"Windows.ApplicationModel.Package>";
    RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
    void *saved_graph = params->PackageDependencyData;
    IVectorView_Package *dependencies = NULL;
    IPackage *package = NULL;
    HSTRING class_name = NULL;
    BYTE *graph;
    UINT32 graph_size, size = ~0u, index = ~0u;
    boolean found = TRUE;
    HRESULT hr;

    if (saved_graph || strcmp( winetest_platform, "wine" )) return;
    graph = create_test_graph( &graph_size, FALSE );
    ok( !!graph, "Failed to allocate graph.\n" );
    if (!graph) return;
    ok( wine_appx_graph_validate_blob( graph, graph_size ),
        "Generated invalid package graph.\n" );

    params->PackageDependencyData = graph;
    hr = IPackageStatics_get_Current( statics, &package );
    params->PackageDependencyData = saved_graph;
    ok( hr == S_OK, "get_Current returned %#lx.\n", hr );
    if (FAILED(hr)) return;
    hr = IPackage_get_Dependencies( package, &dependencies );
    ok( hr == S_OK, "get_Dependencies returned %#lx.\n", hr );
    hr = IVectorView_Package_GetRuntimeClassName( dependencies, &class_name );
    ok( hr == S_OK, "GetRuntimeClassName returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        check_hstring( class_name, vector_class_name );
        WindowsDeleteString( class_name );
    }
    hr = IVectorView_Package_get_Size( dependencies, &size );
    ok( hr == S_OK && !size, "get_Size returned %#lx, %u.\n", hr, size );
    hr = IVectorView_Package_IndexOf( dependencies, package, &index, &found );
    ok( hr == S_OK && !found && !index,
        "IndexOf returned %#lx, %u, %u.\n", hr, index, found );
    IVectorView_Package_Release( dependencies );
    IPackage_Release( package );
}

START_TEST(package)
{
    IActivationFactory *factory = NULL;
    IPackageStatics *statics = NULL;
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize returned %#lx.\n", hr );
    hr = get_package_factory( &factory );
    ok( hr == S_OK, "RoGetActivationFactory returned %#lx.\n", hr );
    if (FAILED(hr)) goto done;
    hr = IActivationFactory_QueryInterface( factory, &IID_IPackageStatics,
                                            (void **)&statics );
    ok( hr == S_OK, "IPackageStatics QI returned %#lx.\n", hr );
    if (FAILED(hr)) goto done;

    test_unpacked_and_factory( factory, statics );
    test_packaged_projection( statics );
    test_empty_dependency_vector( statics );

done:
    if (statics) IPackageStatics_Release( statics );
    if (factory) IActivationFactory_Release( factory );
    RoUninitialize();
}
