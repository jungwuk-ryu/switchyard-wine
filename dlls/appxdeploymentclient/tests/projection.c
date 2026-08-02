/*
 * Windows.Management.Deployment package projection tests
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "../projection.c"

#include "wine/test.h"

struct test_catalog_snapshot
{
    UINT32 count;
    const struct appx_catalog_package *packages;
};

static LONG snapshot_frees;
static LONG dependency_resolve_calls;

#define MAIN_GENERATION                                                   \
    L"1111111111111111" L"1111111111111111"                              \
    L"1111111111111111" L"1111111111111111"
#define FRAMEWORK_GENERATION                                              \
    L"2222222222222222" L"2222222222222222"                              \
    L"2222222222222222" L"2222222222222222"
#define RESOURCE_GENERATION                                               \
    L"3333333333333333" L"3333333333333333"                              \
    L"3333333333333333" L"3333333333333333"
#define INACTIVE_GENERATION                                               \
    L"4444444444444444" L"4444444444444444"                              \
    L"4444444444444444" L"4444444444444444"

void appx_backend_catalog_snapshot_free( APPX_CATALOG_SNAPSHOT *snapshot )
{
    InterlockedIncrement( &snapshot_frees );
    free( snapshot );
}

UINT32 appx_backend_catalog_snapshot_get_package_count(
    const APPX_CATALOG_SNAPSHOT *snapshot )
{
    const struct test_catalog_snapshot *catalog = (const void *)snapshot;
    return catalog->count;
}

const struct appx_catalog_package *
appx_backend_catalog_snapshot_get_package(
    const APPX_CATALOG_SNAPSHOT *snapshot, UINT32 index )
{
    const struct test_catalog_snapshot *catalog = (const void *)snapshot;

    if (index >= catalog->count) return NULL;
    return &catalog->packages[index];
}

HRESULT appx_backend_resolve_direct_dependencies(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *package_full_name,
    enum appx_catalog_architecture target_architecture, UINT32 capacity,
    UINT32 *package_indices, UINT32 *count )
{
    const struct test_catalog_snapshot *catalog = (const void *)snapshot;

    InterlockedIncrement( &dependency_resolve_calls );
    if (!count || !package_full_name ||
        wcscmp( package_full_name,
                L"Contoso.Main_1.2.3.4_x64__contoso" ) ||
        target_architecture != APPX_CATALOG_ARCHITECTURE_X64 ||
        catalog->count < 2)
        return E_INVALIDARG;
    *count = 1;
    if (!capacity) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    package_indices[0] = 1;
    return S_OK;
}

static APPX_CATALOG_SNAPSHOT *create_catalog(
    const struct appx_catalog_package *packages, UINT32 count )
{
    struct test_catalog_snapshot *catalog;

    catalog = calloc( 1, sizeof(*catalog) );
    ok( !!catalog, "Failed to allocate catalog.\n" );
    if (!catalog) return NULL;
    catalog->count = count;
    catalog->packages = packages;
    return (APPX_CATALOG_SNAPSHOT *)catalog;
}

static void check_hstring( HSTRING value, const WCHAR *expected )
{
    const WCHAR *actual;

    actual = WindowsGetStringRawBuffer( value, NULL );
    ok( !wcscmp( actual, expected ), "Got %s, expected %s.\n",
        wine_dbgstr_w(actual), wine_dbgstr_w(expected) );
}

static const struct appx_catalog_dependency main_dependencies[] =
{
    {
        L"Contoso.Framework",
        L"CN=Contoso",
        {2, 0, 0, 0},
    },
};

static const struct appx_catalog_package packages[] =
{
    {
        L"Contoso.Main",
        L"CN=Contoso",
        L"",
        L"contoso",
        L"Contoso.Main_1.2.3.4_x64__contoso",
        L"Contoso.Main_contoso",
        L"payloads\\" MAIN_GENERATION,
        {1, 2, 3, 4},
        APPX_CATALOG_ARCHITECTURE_X64,
        APPX_CATALOG_PACKAGE_ACTIVE | APPX_CATALOG_PACKAGE_SIGNED,
        {0},
        0,
        NULL,
        ARRAY_SIZE(main_dependencies),
        main_dependencies,
    },
    {
        L"Contoso.Framework",
        L"CN=Contoso",
        L"",
        L"contoso",
        L"Contoso.Framework_2.1.0.0_neutral__contoso",
        L"Contoso.Framework_contoso",
        L"payloads\\" FRAMEWORK_GENERATION,
        {2, 1, 0, 0},
        APPX_CATALOG_ARCHITECTURE_NEUTRAL,
        APPX_CATALOG_PACKAGE_ACTIVE | APPX_CATALOG_PACKAGE_FRAMEWORK |
        APPX_CATALOG_PACKAGE_SIGNED,
    },
    {
        L"Contoso.Resources",
        L"CN=Contoso",
        L"en-US",
        L"contoso",
        L"Contoso.Resources_1.0.0.0_neutral_en-us_contoso",
        L"Contoso.Resources_contoso",
        L"payloads\\" RESOURCE_GENERATION,
        {1, 0, 0, 0},
        APPX_CATALOG_ARCHITECTURE_NEUTRAL,
        APPX_CATALOG_PACKAGE_ACTIVE | APPX_CATALOG_PACKAGE_RESOURCE |
        APPX_CATALOG_PACKAGE_SIGNED,
    },
    {
        L"Contoso.Inactive",
        L"CN=Contoso",
        L"",
        L"contoso",
        L"Contoso.Inactive_1.0.0.0_neutral__contoso",
        L"Contoso.Inactive_contoso",
        L"payloads\\" INACTIVE_GENERATION,
        {1, 0, 0, 0},
        APPX_CATALOG_ARCHITECTURE_NEUTRAL,
        APPX_CATALOG_PACKAGE_SIGNED,
    },
};

static void test_package_projection(void)
{
    struct package_query_filter filter = {0};
    IAsyncOperation_StorageFolder *folder_operation;
    IIterable_Package *iterable;
    IIterator_Package *iterator;
    IVectorView_Package *dependencies;
    IStorageFolder *folder;
    IStorageItem *item;
    IPackage *package, *dependency;
    IPackage2 *package2;
    IPackageId *id, *dependency_id;
    IUnknown *identity1, *identity2;
    PackageVersion version;
    ProcessorArchitecture architecture;
    HSTRING string = NULL;
    boolean value;
    UINT32 size;
    HRESULT hr;

    snapshot_frees = 0;
    dependency_resolve_calls = 0;
    filter.types = PackageTypes_All;
    hr = package_iterable_create(
        create_catalog( packages, ARRAY_SIZE(packages) ), &filter,
        &iterable );
    ok( hr == S_OK, "package_iterable_create failed, hr %#lx.\n", hr );
    hr = IIterable_Package_First( iterable, &iterator );
    ok( hr == S_OK, "First failed, hr %#lx.\n", hr );
    IIterable_Package_Release( iterable );
    ok( !snapshot_frees, "Catalog freed while iterator was alive.\n" );

    hr = IIterator_Package_get_Current( iterator, &package );
    ok( hr == S_OK, "Current failed, hr %#lx.\n", hr );
    hr = IPackage_get_Id( package, &id );
    ok( hr == S_OK, "get_Id failed, hr %#lx.\n", hr );
    hr = IPackageId_get_Name( id, &string );
    ok( hr == S_OK, "get_Name failed, hr %#lx.\n", hr );
    check_hstring( string, L"Contoso.Main" );
    WindowsDeleteString( string );
    string = NULL;
    hr = IPackageId_get_Version( id, &version );
    ok( hr == S_OK, "get_Version failed, hr %#lx.\n", hr );
    ok( version.Major == 1 && version.Minor == 2 &&
        version.Build == 3 && version.Revision == 4,
        "Unexpected version %u.%u.%u.%u.\n", version.Major, version.Minor,
        version.Build, version.Revision );
    hr = IPackageId_get_Architecture( id, &architecture );
    ok( hr == S_OK && architecture == ProcessorArchitecture_X64,
        "Unexpected architecture %d, hr %#lx.\n", architecture, hr );

    hr = IPackage_QueryInterface(
        package, &IID_IPackage2, (void **)&package2 );
    ok( hr == S_OK, "IPackage2 query failed, hr %#lx.\n", hr );
    hr = IPackage_QueryInterface(
        package, &IID_IUnknown, (void **)&identity1 );
    ok( hr == S_OK, "IUnknown query failed, hr %#lx.\n", hr );
    hr = IPackage2_QueryInterface(
        package2, &IID_IUnknown, (void **)&identity2 );
    ok( hr == S_OK, "IPackage2 IUnknown query failed, hr %#lx.\n", hr );
    ok( identity1 == identity2, "Package interfaces have different identities.\n" );
    IUnknown_Release( identity1 );
    IUnknown_Release( identity2 );
    value = TRUE;
    hr = IPackage2_get_IsResourcePackage( package2, &value );
    ok( hr == S_OK && !value,
        "Unexpected resource state %d, hr %#lx.\n", value, hr );
    value = TRUE;
    hr = IPackage2_get_IsBundle( package2, &value );
    ok( hr == S_OK && !value,
        "Unexpected bundle state %d, hr %#lx.\n", value, hr );
    value = TRUE;
    hr = IPackage2_get_IsDevelopmentMode( package2, &value );
    ok( hr == S_OK && !value,
        "Unexpected development state %d, hr %#lx.\n", value, hr );
    string = (HSTRING)0xdeadbeef;
    hr = IPackage2_get_DisplayName( package2, &string );
    ok( hr == E_NOTIMPL && !string,
        "DisplayName returned hr %#lx, value %p.\n", hr, string );
    IPackage2_Release( package2 );

    hr = IPackage_get_InstalledLocation( package, &folder );
    ok( hr == S_OK, "get_InstalledLocation failed, hr %#lx.\n", hr );
    hr = IStorageFolder_QueryInterface(
        folder, &IID_IStorageItem, (void **)&item );
    ok( hr == S_OK, "IStorageItem query failed, hr %#lx.\n", hr );
    hr = IStorageItem_get_Path( item, &string );
    ok( hr == S_OK, "get_Path failed, hr %#lx.\n", hr );
    check_hstring(
        string,
        APPX_STORE_ROOT L"\\payloads\\" MAIN_GENERATION );
    WindowsDeleteString( string );
    string = NULL;
    value = TRUE;
    hr = IStorageItem_IsOfType(
        item, StorageItemTypes_None, &value );
    ok( hr == S_OK && !value,
        "None type returned %d, hr %#lx.\n", value, hr );
    value = TRUE;
    hr = IStorageItem_IsOfType(
        item, StorageItemTypes_File, &value );
    ok( hr == S_OK && !value,
        "File type returned %d, hr %#lx.\n", value, hr );
    value = FALSE;
    hr = IStorageItem_IsOfType(
        item, StorageItemTypes_Folder, &value );
    ok( hr == S_OK && value,
        "Folder type returned %d, hr %#lx.\n", value, hr );
    value = FALSE;
    hr = IStorageItem_IsOfType(
        item, StorageItemTypes_File | StorageItemTypes_Folder, &value );
    ok( hr == S_OK && value,
        "File|Folder type returned %d, hr %#lx.\n", value, hr );
    folder_operation = (IAsyncOperation_StorageFolder *)0xdeadbeef;
    hr = IStorageFolder_CreateFolderAsyncOverloadDefaultOptions(
        folder, NULL, &folder_operation );
    ok( hr == E_NOTIMPL && !folder_operation,
        "CreateFolderAsync returned hr %#lx, operation %p.\n",
        hr, folder_operation );
    IStorageItem_Release( item );
    IStorageFolder_Release( folder );

    hr = IPackage_get_Dependencies( package, &dependencies );
    ok( hr == S_OK, "get_Dependencies failed, hr %#lx.\n", hr );
    ok( dependency_resolve_calls == 1,
        "Dependency resolver call count %ld.\n",
        dependency_resolve_calls );
    hr = IVectorView_Package_GetRuntimeClassName(
        dependencies, &string );
    ok( hr == S_OK, "Dependency runtime name failed, hr %#lx.\n", hr );
    check_hstring(
        string,
        L"Windows.Foundation.Collections.IVectorView`1<"
        L"Windows.ApplicationModel.Package>" );
    WindowsDeleteString( string );
    string = NULL;
    hr = IVectorView_Package_get_Size( dependencies, &size );
    ok( hr == S_OK && size == 1,
        "Dependency size %u, hr %#lx.\n", size, hr );
    hr = IVectorView_Package_GetAt( dependencies, 0, &dependency );
    ok( hr == S_OK, "Dependency GetAt failed, hr %#lx.\n", hr );
    hr = IPackage_get_Id( dependency, &dependency_id );
    ok( hr == S_OK, "Dependency get_Id failed, hr %#lx.\n", hr );
    hr = IPackageId_get_FullName( dependency_id, &string );
    ok( hr == S_OK, "Dependency get_FullName failed, hr %#lx.\n", hr );
    check_hstring(
        string, L"Contoso.Framework_2.1.0.0_neutral__contoso" );
    WindowsDeleteString( string );
    string = NULL;
    IPackageId_Release( dependency_id );
    IPackage_Release( dependency );
    IVectorView_Package_Release( dependencies );

    IPackage_Release( package );
    IIterator_Package_Release( iterator );
    ok( !snapshot_frees, "Catalog freed while package id was alive.\n" );
    hr = IPackageId_get_FullName( id, &string );
    ok( hr == S_OK, "Retained id failed, hr %#lx.\n", hr );
    check_hstring( string, L"Contoso.Main_1.2.3.4_x64__contoso" );
    WindowsDeleteString( string );
    IPackageId_Release( id );
    ok( snapshot_frees == 1, "Catalog free count %ld.\n", snapshot_frees );
}

static void test_invalid_installed_location(void)
{
    struct appx_catalog_package invalid = packages[0];
    IStorageFolder *folder;
    IPackage *package;
    HRESULT hr;

    snapshot_frees = 0;
    invalid.payload_path = L"C:\\outside\\payload";
    hr = package_from_catalog_create(
        create_catalog( &invalid, 1 ), 0, &package );
    ok( hr == S_OK, "Package create failed, hr %#lx.\n", hr );
    folder = (IStorageFolder *)0xdeadbeef;
    hr = IPackage_get_InstalledLocation( package, &folder );
    ok( hr == HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) && !folder,
        "Absolute payload path returned hr %#lx, folder %p.\n", hr, folder );
    IPackage_Release( package );
    ok( snapshot_frees == 1, "Catalog free count %ld.\n", snapshot_frees );
}

static void test_filters(void)
{
    struct package_query_filter filter = {0};
    IIterable_Package *iterable;
    IIterator_Package *iterator;
    IPackage *package;
    IPackage2 *package2;
    boolean current, value;
    HRESULT hr;

    snapshot_frees = 0;
    filter.name = L"contoso.resources";
    filter.publisher = L"cn=contoso";
    filter.types = PackageTypes_Resource;
    hr = package_iterable_create(
        create_catalog( packages, ARRAY_SIZE(packages) ), &filter,
        &iterable );
    ok( hr == S_OK, "Filtered create failed, hr %#lx.\n", hr );
    hr = IIterable_Package_First( iterable, &iterator );
    ok( hr == S_OK, "First failed, hr %#lx.\n", hr );
    hr = IIterator_Package_get_HasCurrent( iterator, &current );
    ok( hr == S_OK && current, "Expected one resource, hr %#lx.\n", hr );
    hr = IIterator_Package_get_Current( iterator, &package );
    ok( hr == S_OK, "Current failed, hr %#lx.\n", hr );
    hr = IPackage_QueryInterface(
        package, &IID_IPackage2, (void **)&package2 );
    ok( hr == S_OK, "IPackage2 query failed, hr %#lx.\n", hr );
    value = FALSE;
    hr = IPackage2_get_IsResourcePackage( package2, &value );
    ok( hr == S_OK && value,
        "Expected resource package, value %d, hr %#lx.\n", value, hr );
    IPackage2_Release( package2 );
    IPackage_Release( package );
    hr = IIterator_Package_MoveNext( iterator, &current );
    ok( hr == S_OK && !current, "Expected end, hr %#lx.\n", hr );
    IIterator_Package_Release( iterator );
    IIterable_Package_Release( iterable );
    ok( snapshot_frees == 1, "Catalog free count %ld.\n", snapshot_frees );

    filter.name = L"Contoso.*";
    filter.publisher = NULL;
    filter.types = PackageTypes_All;
    hr = package_iterable_create(
        create_catalog( packages, ARRAY_SIZE(packages) ), &filter,
        &iterable );
    ok( hr == S_OK, "Wildcard filter create failed, hr %#lx.\n", hr );
    hr = IIterable_Package_First( iterable, &iterator );
    ok( hr == S_OK, "First failed, hr %#lx.\n", hr );
    hr = IIterator_Package_get_HasCurrent( iterator, &current );
    ok( hr == S_OK && !current,
        "Wildcard unexpectedly matched, hr %#lx.\n", hr );
    IIterator_Package_Release( iterator );
    IIterable_Package_Release( iterable );
    ok( snapshot_frees == 2, "Catalog free count %ld.\n", snapshot_frees );
}

START_TEST(projection)
{
    test_package_projection();
    test_invalid_installed_location();
    test_filters();
}
