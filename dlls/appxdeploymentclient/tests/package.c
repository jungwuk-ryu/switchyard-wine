/*
 * Windows.Management.Deployment PackageManager tests
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include "roapi.h"

/*
 * The test executable also links async.c and projection.c tests, each of which
 * includes its production implementation.  Give this translation unit's
 * backend seam private symbol names so it cannot interpose on those tests.
 */
#define package_deployment_options_init \
    test_package_deployment_options_init
#define package_async_operation_create \
    test_package_async_operation_create
#define package_iterable_create test_package_iterable_create
#define package_from_catalog_create test_package_from_catalog_create
#define appx_backend_deployment_initialize \
    test_package_appx_backend_deployment_initialize
#define appx_backend_deployment_result_free \
    test_package_appx_backend_deployment_result_free
#define appx_backend_deployment_query \
    test_package_appx_backend_deployment_query

#include "../package.c"

#include "wine/test.h"

static enum package_deployment_operation captured_operation;
static UINT32 captured_deployment_flags;
static BOOL captured_package_handle;
static WCHAR captured_full_name[256];
static struct package_query_filter captured_filter;
static WCHAR captured_filter_name[128];
static WCHAR captured_filter_publisher[128];
static WCHAR captured_filter_family[128];
static const APPX_CATALOG_SNAPSHOT *captured_catalog;
static HRESULT query_hr;
static LONG initialize_calls;
static LONG result_free_calls;
static LONG query_calls;
static LONG iterable_calls;
static LONG package_create_calls;

static void copy_optional_string( WCHAR *destination, UINT32 capacity,
                                  const WCHAR *source )
{
    if (!source)
    {
        destination[0] = 0;
        return;
    }
    lstrcpynW( destination, source, capacity );
}

static HSTRING create_test_hstring( const WCHAR *text, UINT32 length )
{
    HSTRING value = NULL;
    HRESULT hr;

    hr = WindowsCreateString( text, length, &value );
    ok( hr == S_OK, "WindowsCreateString returned hr %#lx.\n", hr );
    return value;
}

void package_deployment_options_init( APPX_DEPLOYMENT_OPTIONS *options,
                                      UINT32 flags, HANDLE cancel_event )
{
    memset( options, 0, sizeof(*options) );
    options->size = sizeof(*options);
    options->flags = flags;
    options->cancel_event = cancel_event;
}

HRESULT package_async_operation_create(
    enum package_deployment_operation operation, HANDLE package_file,
    const WCHAR *full_name, UINT32 deployment_flags,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **value )
{
    captured_operation = operation;
    captured_deployment_flags = deployment_flags;
    captured_package_handle =
        package_file && package_file != INVALID_HANDLE_VALUE;
    copy_optional_string( captured_full_name, ARRAY_SIZE(captured_full_name),
                          full_name );
    if (captured_package_handle) CloseHandle( package_file );
    *value = (void *)0x12345678;
    return S_OK;
}

HRESULT appx_backend_deployment_initialize(
    const APPX_DEPLOYMENT_OPTIONS *options, APPX_DEPLOYMENT_RESULT **result )
{
    InterlockedIncrement( &initialize_calls );
    *result = (APPX_DEPLOYMENT_RESULT *)0x87654321;
    return S_OK;
}

void appx_backend_deployment_result_free( APPX_DEPLOYMENT_RESULT *result )
{
    ok( result == (APPX_DEPLOYMENT_RESULT *)0x87654321,
        "Unexpected deployment result %p.\n", result );
    InterlockedIncrement( &result_free_calls );
}

HRESULT appx_backend_deployment_query(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_CATALOG_SNAPSHOT **snapshot )
{
    InterlockedIncrement( &query_calls );
    copy_optional_string( captured_full_name, ARRAY_SIZE(captured_full_name),
                          full_name );
    if (FAILED(query_hr))
    {
        *snapshot = NULL;
        return query_hr;
    }
    *snapshot = (APPX_CATALOG_SNAPSHOT *)0xabcdef01;
    return S_OK;
}

HRESULT package_iterable_create( APPX_CATALOG_SNAPSHOT *catalog,
                                 const struct package_query_filter *filter,
                                 IIterable_Package **value )
{
    InterlockedIncrement( &iterable_calls );
    captured_catalog = catalog;
    captured_filter = *filter;
    copy_optional_string( captured_filter_name,
                          ARRAY_SIZE(captured_filter_name), filter->name );
    copy_optional_string( captured_filter_publisher,
                          ARRAY_SIZE(captured_filter_publisher),
                          filter->publisher );
    copy_optional_string( captured_filter_family,
                          ARRAY_SIZE(captured_filter_family),
                          filter->family_name );
    captured_filter.name = filter->name ? captured_filter_name : NULL;
    captured_filter.publisher =
        filter->publisher ? captured_filter_publisher : NULL;
    captured_filter.family_name =
        filter->family_name ? captured_filter_family : NULL;
    *value = (IIterable_Package *)0x13572468;
    return S_OK;
}

HRESULT package_from_catalog_create( APPX_CATALOG_SNAPSHOT *catalog,
                                     UINT32 index, IPackage **value )
{
    ok( catalog == (APPX_CATALOG_SNAPSHOT *)0xabcdef01,
        "Unexpected catalog %p.\n", catalog );
    ok( !index, "Unexpected catalog index %u.\n", index );
    InterlockedIncrement( &package_create_calls );
    *value = (IPackage *)0x24681357;
    return S_OK;
}

struct test_uri_iterator
{
    IIterator_Uri IIterator_Uri_iface;
    LONG ref;
    boolean has_current;
};

struct test_uri_iterable
{
    IIterable_Uri IIterable_Uri_iface;
    struct test_uri_iterator iterator;
    LONG ref;
};

static HRESULT WINAPI inspectable_QueryInterface( IInspectable *iface,
                                                   REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID( iid, &IID_IUnknown ) &&
        !IsEqualGUID( iid, &IID_IInspectable ))
        return E_NOINTERFACE;
    *out = iface;
    IInspectable_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI iterator_AddRef( IIterator_Uri *iface )
{
    struct test_uri_iterator *iterator =
        CONTAINING_RECORD( iface, struct test_uri_iterator,
                           IIterator_Uri_iface );
    return InterlockedIncrement( &iterator->ref );
}

static ULONG WINAPI iterator_Release( IIterator_Uri *iface )
{
    struct test_uri_iterator *iterator =
        CONTAINING_RECORD( iface, struct test_uri_iterator,
                           IIterator_Uri_iface );
    return InterlockedDecrement( &iterator->ref );
}

static HRESULT WINAPI iterator_QueryInterface( IIterator_Uri *iface,
                                               REFIID iid, void **out )
{
    return inspectable_QueryInterface( (IInspectable *)iface, iid, out );
}

static HRESULT WINAPI inspectable_GetIids( IInspectable *iface,
                                           ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI inspectable_GetRuntimeClassName( IInspectable *iface,
                                                       HSTRING *value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI inspectable_GetTrustLevel( IInspectable *iface,
                                                 TrustLevel *value )
{
    if (!value) return E_POINTER;
    *value = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI iterator_get_Current( IIterator_Uri *iface,
                                            IUriRuntimeClass **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return E_BOUNDS;
}

static HRESULT WINAPI iterator_get_HasCurrent( IIterator_Uri *iface,
                                               boolean *value )
{
    struct test_uri_iterator *iterator =
        CONTAINING_RECORD( iface, struct test_uri_iterator,
                           IIterator_Uri_iface );
    if (!value) return E_POINTER;
    *value = iterator->has_current;
    return S_OK;
}

static HRESULT WINAPI iterator_MoveNext( IIterator_Uri *iface,
                                         boolean *value )
{
    struct test_uri_iterator *iterator =
        CONTAINING_RECORD( iface, struct test_uri_iterator,
                           IIterator_Uri_iface );
    if (!value) return E_POINTER;
    iterator->has_current = FALSE;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI iterator_GetMany( IIterator_Uri *iface,
                                        UINT32 capacity,
                                        IUriRuntimeClass **items,
                                        UINT32 *count )
{
    if (!count) return E_POINTER;
    *count = 0;
    return S_OK;
}

static const IIterator_UriVtbl iterator_vtbl =
{
    iterator_QueryInterface,
    iterator_AddRef,
    iterator_Release,
    (void *)inspectable_GetIids,
    (void *)inspectable_GetRuntimeClassName,
    (void *)inspectable_GetTrustLevel,
    iterator_get_Current,
    iterator_get_HasCurrent,
    iterator_MoveNext,
    iterator_GetMany,
};

static ULONG WINAPI iterable_AddRef( IIterable_Uri *iface )
{
    struct test_uri_iterable *iterable =
        CONTAINING_RECORD( iface, struct test_uri_iterable,
                           IIterable_Uri_iface );
    return InterlockedIncrement( &iterable->ref );
}

static ULONG WINAPI iterable_Release( IIterable_Uri *iface )
{
    struct test_uri_iterable *iterable =
        CONTAINING_RECORD( iface, struct test_uri_iterable,
                           IIterable_Uri_iface );
    return InterlockedDecrement( &iterable->ref );
}

static HRESULT WINAPI iterable_QueryInterface( IIterable_Uri *iface,
                                               REFIID iid, void **out )
{
    return inspectable_QueryInterface( (IInspectable *)iface, iid, out );
}

static HRESULT WINAPI iterable_First( IIterable_Uri *iface,
                                      IIterator_Uri **value )
{
    struct test_uri_iterable *iterable =
        CONTAINING_RECORD( iface, struct test_uri_iterable,
                           IIterable_Uri_iface );
    if (!value) return E_POINTER;
    *value = &iterable->iterator.IIterator_Uri_iface;
    IIterator_Uri_AddRef( *value );
    return S_OK;
}

static const IIterable_UriVtbl iterable_vtbl =
{
    iterable_QueryInterface,
    iterable_AddRef,
    iterable_Release,
    (void *)inspectable_GetIids,
    (void *)inspectable_GetRuntimeClassName,
    (void *)inspectable_GetTrustLevel,
    iterable_First,
};

static HRESULT uri_create( const WCHAR *text, IUriRuntimeClass **value )
{
    IUriRuntimeClassFactory *uri_factory = NULL;
    IActivationFactory *activation = NULL;
    HSTRING class_name = NULL, uri = NULL;
    HRESULT hr;

    *value = NULL;
    if (FAILED(hr = WindowsCreateString(
            RuntimeClass_Windows_Foundation_Uri,
            wcslen(RuntimeClass_Windows_Foundation_Uri), &class_name )))
        goto done;
    if (FAILED(hr = RoGetActivationFactory(
            class_name, &IID_IActivationFactory, (void **)&activation )))
        goto done;
    if (FAILED(hr = IActivationFactory_QueryInterface(
            activation, &IID_IUriRuntimeClassFactory,
            (void **)&uri_factory )))
        goto done;
    if (FAILED(hr = WindowsCreateString( text, wcslen(text), &uri )))
        goto done;
    hr = IUriRuntimeClassFactory_CreateUri( uri_factory, uri, value );

done:
    WindowsDeleteString( uri );
    WindowsDeleteString( class_name );
    if (uri_factory) IUriRuntimeClassFactory_Release( uri_factory );
    if (activation) IActivationFactory_Release( activation );
    return hr;
}

struct test_uri
{
    IUriRuntimeClass IUriRuntimeClass_iface;
    LONG ref;
    HSTRING absolute;
    HSTRING scheme;
    HSTRING empty;
};

static inline struct test_uri *impl_from_test_uri( IUriRuntimeClass *iface )
{
    return CONTAINING_RECORD( iface, struct test_uri, IUriRuntimeClass_iface );
}

static HRESULT WINAPI test_uri_QueryInterface(
    IUriRuntimeClass *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IUriRuntimeClass ))
        *out = iface;
    else
        return E_NOINTERFACE;
    IUriRuntimeClass_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI test_uri_AddRef( IUriRuntimeClass *iface )
{
    return InterlockedIncrement( &impl_from_test_uri(iface)->ref );
}

static ULONG WINAPI test_uri_Release( IUriRuntimeClass *iface )
{
    return InterlockedDecrement( &impl_from_test_uri(iface)->ref );
}

static HRESULT WINAPI test_uri_GetIids(
    IUriRuntimeClass *iface, ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    *count = 0;
    *iids = CoTaskMemAlloc( sizeof(**iids) );
    if (!*iids) return E_OUTOFMEMORY;
    **iids = IID_IUriRuntimeClass;
    *count = 1;
    return S_OK;
}

static HRESULT WINAPI test_uri_GetRuntimeClassName(
    IUriRuntimeClass *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsCreateString(
        RuntimeClass_Windows_Foundation_Uri,
        wcslen(RuntimeClass_Windows_Foundation_Uri), value );
}

static HRESULT WINAPI test_uri_GetTrustLevel(
    IUriRuntimeClass *iface, TrustLevel *value )
{
    if (!value) return E_POINTER;
    *value = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI test_uri_get_AbsoluteUri(
    IUriRuntimeClass *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString(
        impl_from_test_uri(iface)->absolute, value );
}

static HRESULT WINAPI test_uri_get_empty(
    IUriRuntimeClass *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_test_uri(iface)->empty, value );
}

static HRESULT WINAPI test_uri_get_QueryParsed(
    IUriRuntimeClass *iface, IWwwFormUrlDecoderRuntimeClass **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI test_uri_get_RawUri(
    IUriRuntimeClass *iface, HSTRING *value )
{
    return test_uri_get_AbsoluteUri( iface, value );
}

static HRESULT WINAPI test_uri_get_SchemeName(
    IUriRuntimeClass *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_test_uri(iface)->scheme, value );
}

static HRESULT WINAPI test_uri_get_Port(
    IUriRuntimeClass *iface, INT32 *value )
{
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI test_uri_get_Suspicious(
    IUriRuntimeClass *iface, boolean *value )
{
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI test_uri_Equals(
    IUriRuntimeClass *iface, IUriRuntimeClass *other, boolean *value )
{
    if (!value) return E_POINTER;
    *value = iface == other;
    return S_OK;
}

static HRESULT WINAPI test_uri_CombineUri(
    IUriRuntimeClass *iface, HSTRING relative, IUriRuntimeClass **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
}

static const IUriRuntimeClassVtbl test_uri_vtbl =
{
    test_uri_QueryInterface,
    test_uri_AddRef,
    test_uri_Release,
    test_uri_GetIids,
    test_uri_GetRuntimeClassName,
    test_uri_GetTrustLevel,
    test_uri_get_AbsoluteUri,
    test_uri_get_empty,
    test_uri_get_empty,
    test_uri_get_empty,
    test_uri_get_empty,
    test_uri_get_empty,
    test_uri_get_empty,
    test_uri_get_empty,
    test_uri_get_empty,
    test_uri_get_QueryParsed,
    test_uri_get_RawUri,
    test_uri_get_SchemeName,
    test_uri_get_empty,
    test_uri_get_Port,
    test_uri_get_Suspicious,
    test_uri_Equals,
    test_uri_CombineUri,
};

static HRESULT test_uri_init(
    struct test_uri *uri, const WCHAR *absolute, UINT32 absolute_length,
    const WCHAR *scheme )
{
    HRESULT hr;

    memset( uri, 0, sizeof(*uri) );
    uri->IUriRuntimeClass_iface.lpVtbl = &test_uri_vtbl;
    uri->ref = 1;
    if (FAILED( hr = WindowsCreateString(
            absolute, absolute_length, &uri->absolute ) ) ||
        FAILED( hr = WindowsCreateString(
            scheme, wcslen(scheme), &uri->scheme ) ) ||
        FAILED( hr = WindowsCreateString( L"", 0, &uri->empty ) ))
    {
        WindowsDeleteString( uri->empty );
        WindowsDeleteString( uri->scheme );
        WindowsDeleteString( uri->absolute );
    }
    return hr;
}

static void test_uri_cleanup( struct test_uri *uri )
{
    ok( uri->ref == 1, "URI reference count %ld.\n", uri->ref );
    WindowsDeleteString( uri->empty );
    WindowsDeleteString( uri->scheme );
    WindowsDeleteString( uri->absolute );
}

static void check_rejected_local_uri(
    const WCHAR *absolute, UINT32 length, const WCHAR *scheme,
    const char *description )
{
    struct test_uri uri;
    HANDLE file = (HANDLE)0xdeadbeef;
    HRESULT hr;

    hr = test_uri_init( &uri, absolute, length, scheme );
    ok( hr == S_OK, "%s URI initialization returned %#lx.\n",
        description, hr );
    if (FAILED(hr)) return;
    hr = open_local_package_uri( &uri.IUriRuntimeClass_iface, &file );
    ok( hr == E_INVALIDARG && file == INVALID_HANDLE_VALUE,
        "%s URI returned %#lx, handle %p.\n", description, hr, file );
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    test_uri_cleanup( &uri );
}

static void test_local_uri_validation( void )
{
    static const WCHAR embedded_nul[] =
        L"file:///C:/chosen.msix\0.approved.msix";
    static const WCHAR * const rejected[] =
    {
        L"file:///C:/chosen%00.approved.msix",
        L"file:///C:/chosen%u0000.approved.msix",
        L"https://example.invalid/chosen.msix",
        L"file:///C:/chosen.msix%3Astream",
        L"file:///C:/windows/%2e%2e/system32/chosen.msix",
        L"file:///C:/chosen.msix?ignored=1",
        L"file:///C:/chosen.msix#ignored",
    };
    static const char * const descriptions[] =
    {
        "percent NUL", "percent-u NUL", "contradictory scheme",
        "encoded ADS", "encoded traversal", "hidden query",
        "hidden fragment",
    };
    WCHAR temp_path[MAX_PATH], temp_file[MAX_PATH], url[2048];
    struct test_uri uri;
    DWORD url_length = ARRAY_SIZE(url);
    HANDLE file = INVALID_HANDLE_VALUE;
    UINT32 i;
    HRESULT hr;

    GetTempPathW( ARRAY_SIZE(temp_path), temp_path );
    ok( GetTempFileNameW( temp_path, L"uri", 0, temp_file ),
        "GetTempFileName failed, error %lu.\n", GetLastError() );
    hr = UrlCreateFromPathW( temp_file, url, &url_length, 0 );
    ok( hr == S_OK, "UrlCreateFromPath failed, hr %#lx.\n", hr );
    if (FAILED(hr)) goto done;
    hr = test_uri_init( &uri, url, wcslen(url), L"file" );
    ok( hr == S_OK, "Valid URI initialization returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        hr = open_local_package_uri( &uri.IUriRuntimeClass_iface, &file );
        ok( hr == S_OK && file != INVALID_HANDLE_VALUE,
            "Valid custom URI returned %#lx, handle %p.\n", hr, file );
        if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
        test_uri_cleanup( &uri );
    }

    check_rejected_local_uri(
        embedded_nul, ARRAY_SIZE(embedded_nul) - 1, L"file",
        "embedded NUL" );
    for (i = 0; i < ARRAY_SIZE(rejected); i++)
        check_rejected_local_uri(
            rejected[i], wcslen(rejected[i]), L"file", descriptions[i] );

done:
    DeleteFileW( temp_file );
}

static void reset_capture(void)
{
    captured_operation = PACKAGE_DEPLOYMENT_INSTALL;
    captured_deployment_flags = 0;
    captured_package_handle = FALSE;
    captured_full_name[0] = 0;
    memset( &captured_filter, 0, sizeof(captured_filter) );
    captured_filter_name[0] = 0;
    captured_filter_publisher[0] = 0;
    captured_filter_family[0] = 0;
    captured_catalog = NULL;
    query_hr = S_OK;
    initialize_calls = 0;
    result_free_calls = 0;
    query_calls = 0;
    iterable_calls = 0;
    package_create_calls = 0;
}

static WCHAR *get_current_user_sid_string(void)
{
    TOKEN_USER *user = NULL;
    WCHAR *sid = NULL;
    HANDLE token = NULL;
    DWORD size = 0;

    if (!OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ))
    {
        ok( 0, "OpenProcessToken failed, error %lu.\n", GetLastError() );
        return NULL;
    }
    GetTokenInformation( token, TokenUser, NULL, 0, &size );
    ok( GetLastError() == ERROR_INSUFFICIENT_BUFFER && !!size,
        "Unexpected token size query result, error %lu, size %lu.\n",
        GetLastError(), size );
    if (!(user = malloc( size )))
    {
        ok( 0, "Failed to allocate token user buffer.\n" );
        CloseHandle( token );
        return NULL;
    }
    if (!GetTokenInformation( token, TokenUser, user, size, &size ) ||
        !ConvertSidToStringSidW( user->User.Sid, &sid ))
    {
        ok( 0, "Failed to query current user SID, error %lu.\n",
            GetLastError() );
        LocalFree( sid );
        sid = NULL;
    }
    free( user );
    CloseHandle( token );
    return sid;
}

static void test_factory(void)
{
    IActivationFactory *factory = package_manager_factory;
    IPackageManager2 *manager2 = NULL;
    IInspectable *inspectable = NULL;
    IPackageManager *manager = NULL;
    IAgileObject *agile = NULL;
    HRESULT hr;

    hr = IActivationFactory_QueryInterface(
        factory, &IID_IAgileObject, (void **)&agile );
    ok( hr == S_OK, "Factory agility query failed, hr %#lx.\n", hr );
    IAgileObject_Release( agile );
    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    ok( hr == S_OK, "ActivateInstance failed, hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface(
        inspectable, &IID_IPackageManager, (void **)&manager );
    ok( hr == S_OK, "IPackageManager query failed, hr %#lx.\n", hr );
    hr = IPackageManager_QueryInterface(
        manager, &IID_IPackageManager2, (void **)&manager2 );
    ok( hr == S_OK, "IPackageManager2 query failed, hr %#lx.\n", hr );
    IPackageManager2_Release( manager2 );
    IPackageManager_Release( manager );
    IInspectable_Release( inspectable );
}

static void test_deployment_methods(void)
{
    static const WCHAR embedded_full_name_text[] =
        L"Contoso.Main_1.2.3.4_x64__contoso\0_truncated";
    struct test_uri_iterable dependencies =
    {
        {&iterable_vtbl},
        {{&iterator_vtbl}, 1, TRUE},
        1,
    };
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress
        *operation;
    IActivationFactory *factory = package_manager_factory;
    IUriRuntimeClass *file_uri = NULL, *http_uri = NULL;
    IInspectable *inspectable = NULL;
    IPackageManager2 *manager2 = NULL;
    IPackageManager *manager = NULL;
    WCHAR temp_path[MAX_PATH] = {0}, temp_file[MAX_PATH] = {0}, url[2048];
    HSTRING full_name = NULL, empty = NULL, embedded_full_name = NULL;
    DWORD url_length = ARRAY_SIZE(url);
    HANDLE file;
    HRESULT hr;

    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    ok( hr == S_OK, "ActivateInstance failed, hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface(
        inspectable, &IID_IPackageManager, (void **)&manager );
    ok( hr == S_OK, "IPackageManager query failed, hr %#lx.\n", hr );
    hr = IPackageManager_QueryInterface(
        manager, &IID_IPackageManager2, (void **)&manager2 );
    ok( hr == S_OK, "IPackageManager2 query failed, hr %#lx.\n", hr );

    operation = (void *)0xdeadbeef;
    hr = IPackageManager_AddPackageAsync(
        manager, NULL, NULL, DeploymentOptions_None, &operation );
    ok( hr == E_INVALIDARG && !operation,
        "Null URI returned hr %#lx, operation %p.\n", hr, operation );
    operation = (void *)0xdeadbeef;
    hr = IPackageManager_AddPackageAsync(
        manager, NULL, NULL, DeploymentOptions_DevelopmentMode, &operation );
    ok( hr == E_INVALIDARG && !operation,
        "Development mode returned hr %#lx, operation %p.\n", hr, operation );
    operation = (void *)0xdeadbeef;
    hr = IPackageManager_AddPackageAsync(
        manager, NULL, NULL, DeploymentOptions_ForceApplicationShutdown,
        &operation );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && !operation,
        "Unsupported option returned hr %#lx, operation %p.\n",
        hr, operation );

    GetTempPathW( ARRAY_SIZE(temp_path), temp_path );
    ok( GetTempFileNameW( temp_path, L"pmt", 0, temp_file ),
        "GetTempFileName failed, error %lu.\n", GetLastError() );
    file = CreateFileW( temp_file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE,
        "CreateFile failed, error %lu.\n", GetLastError() );
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    hr = UrlCreateFromPathW( temp_file, url, &url_length, 0 );
    ok( hr == S_OK, "UrlCreateFromPath failed, hr %#lx.\n", hr );
    hr = uri_create( url, &file_uri );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip("Windows.Foundation.Uri is not registered.\n");
        goto cleanup;
    }
    ok( hr == S_OK, "File URI creation failed, hr %#lx.\n", hr );
    hr = uri_create( L"https://example.invalid/package.msix", &http_uri );
    ok( hr == S_OK, "HTTP URI creation failed, hr %#lx.\n", hr );

    operation = (void *)0xdeadbeef;
    hr = IPackageManager_AddPackageAsync(
        manager, http_uri, NULL, DeploymentOptions_None, &operation );
    ok( hr == E_INVALIDARG && !operation,
        "HTTP URI returned hr %#lx, operation %p.\n", hr, operation );
    operation = (void *)0xdeadbeef;
    hr = IPackageManager_AddPackageAsync(
        manager, file_uri, &dependencies.IIterable_Uri_iface,
        DeploymentOptions_None, &operation );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && !operation,
        "Dependency returned hr %#lx, operation %p.\n", hr, operation );

    reset_capture();
    operation = NULL;
    hr = IPackageManager_AddPackageAsync(
        manager, file_uri, NULL, DeploymentOptions_None, &operation );
    ok( hr == S_OK && operation == (void *)0x12345678,
        "Add returned hr %#lx, operation %p.\n", hr, operation );
    ok( captured_operation == PACKAGE_DEPLOYMENT_INSTALL &&
        captured_package_handle && !captured_deployment_flags,
        "Unexpected add capture operation %u, handle %d, flags %#x.\n",
        captured_operation, captured_package_handle,
        captured_deployment_flags );

    reset_capture();
    operation = NULL;
    hr = IPackageManager_UpdatePackageAsync(
        manager, file_uri, NULL,
        DeploymentOptions_ForceUpdateFromAnyVersion, &operation );
    ok( hr == S_OK && operation == (void *)0x12345678,
        "Update returned hr %#lx, operation %p.\n", hr, operation );
    ok( captured_operation == PACKAGE_DEPLOYMENT_UPDATE &&
        captured_package_handle &&
        captured_deployment_flags == APPX_DEPLOYMENT_ALLOW_DOWNGRADE,
        "Unexpected update capture operation %u, handle %d, flags %#x.\n",
        captured_operation, captured_package_handle,
        captured_deployment_flags );

    WindowsCreateString(
        L"Contoso.Main_1.2.3.4_x64__contoso",
        wcslen(L"Contoso.Main_1.2.3.4_x64__contoso"), &full_name );
    WindowsCreateString( L"", 0, &empty );
    embedded_full_name = create_test_hstring(
        embedded_full_name_text, ARRAY_SIZE(embedded_full_name_text) - 1 );
    operation = (void *)0xdeadbeef;
    hr = IPackageManager_RemovePackageAsync( manager, empty, &operation );
    ok( hr == E_INVALIDARG && !operation,
        "Empty remove returned hr %#lx, operation %p.\n", hr, operation );
    reset_capture();
    operation = (void *)0xdeadbeef;
    hr = IPackageManager_RemovePackageAsync(
        manager, embedded_full_name, &operation );
    ok( hr == E_INVALIDARG && !operation,
        "Embedded-nul remove returned hr %#lx, operation %p.\n",
        hr, operation );
    ok( !captured_full_name[0] && !captured_package_handle,
        "Embedded-nul remove reached backend with handle %d, name %s.\n",
        captured_package_handle, wine_dbgstr_w(captured_full_name) );
    reset_capture();
    operation = NULL;
    hr = IPackageManager_RemovePackageAsync(
        manager, full_name, &operation );
    ok( hr == S_OK && operation == (void *)0x12345678,
        "Remove returned hr %#lx, operation %p.\n", hr, operation );
    ok( captured_operation == PACKAGE_DEPLOYMENT_REMOVE &&
        !captured_package_handle &&
        !wcscmp( captured_full_name,
                 L"Contoso.Main_1.2.3.4_x64__contoso" ),
        "Unexpected remove capture operation %u, handle %d, name %s.\n",
        captured_operation, captured_package_handle,
        wine_dbgstr_w(captured_full_name) );
    operation = (void *)0xdeadbeef;
    hr = IPackageManager2_RemovePackageWithOptionsAsync(
        manager2, full_name, RemovalOptions_PreserveApplicationData,
        &operation );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && !operation,
        "Unsupported removal returned hr %#lx, operation %p.\n",
        hr, operation );
    operation = (void *)0xdeadbeef;
    hr = IPackageManager_StagePackageAsync(
        manager, file_uri, NULL, &operation );
    ok( hr == E_NOTIMPL && !operation,
        "Stage returned hr %#lx, operation %p.\n", hr, operation );
    operation = (void *)0xdeadbeef;
    hr = IPackageManager_RegisterPackageAsync(
        manager, file_uri, NULL, DeploymentOptions_None, &operation );
    ok( hr == E_NOTIMPL && !operation,
        "Register returned hr %#lx, operation %p.\n", hr, operation );

cleanup:
    WindowsDeleteString( embedded_full_name );
    WindowsDeleteString( empty );
    WindowsDeleteString( full_name );
    if (http_uri) IUriRuntimeClass_Release( http_uri );
    if (file_uri) IUriRuntimeClass_Release( file_uri );
    DeleteFileW( temp_file );
    IPackageManager2_Release( manager2 );
    IPackageManager_Release( manager );
    IInspectable_Release( inspectable );
}

static void test_query_methods(void)
{
    static const WCHAR embedded_name_text[] =
        L"Contoso.Main\0_truncated";
    static const WCHAR embedded_publisher_text[] =
        L"CN=Contoso\0_truncated";
    static const WCHAR embedded_family_text[] =
        L"Contoso.Main_contoso\0_truncated";
    static const WCHAR embedded_full_name_text[] =
        L"Contoso.Main_1.2.3.4_x64__contoso\0_truncated";
    IActivationFactory *factory = package_manager_factory;
    IInspectable *inspectable = NULL;
    IPackageManager2 *manager2 = NULL;
    IPackageManager *manager = NULL;
    IIterable_Package *packages;
    IPackage *package;
    HSTRING name = NULL, publisher = NULL, family = NULL, full_name = NULL;
    HSTRING embedded_name = NULL, embedded_publisher = NULL;
    HSTRING embedded_family = NULL, embedded_full_name = NULL;
    HRESULT hr;

    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    ok( hr == S_OK, "ActivateInstance failed, hr %#lx.\n", hr );
    hr = IInspectable_QueryInterface(
        inspectable, &IID_IPackageManager, (void **)&manager );
    ok( hr == S_OK, "IPackageManager query failed, hr %#lx.\n", hr );
    hr = IPackageManager_QueryInterface(
        manager, &IID_IPackageManager2, (void **)&manager2 );
    ok( hr == S_OK, "IPackageManager2 query failed, hr %#lx.\n", hr );
    WindowsCreateString( L"Contoso.Main", wcslen(L"Contoso.Main"), &name );
    WindowsCreateString( L"CN=Contoso", wcslen(L"CN=Contoso"), &publisher );
    WindowsCreateString( L"Contoso.Main_contoso",
                         wcslen(L"Contoso.Main_contoso"), &family );
    WindowsCreateString( L"Contoso.Main_1.2.3.4_x64__contoso",
                         wcslen(L"Contoso.Main_1.2.3.4_x64__contoso"),
                         &full_name );
    embedded_name = create_test_hstring(
        embedded_name_text, ARRAY_SIZE(embedded_name_text) - 1 );
    embedded_publisher = create_test_hstring(
        embedded_publisher_text, ARRAY_SIZE(embedded_publisher_text) - 1 );
    embedded_family = create_test_hstring(
        embedded_family_text, ARRAY_SIZE(embedded_family_text) - 1 );
    embedded_full_name = create_test_hstring(
        embedded_full_name_text, ARRAY_SIZE(embedded_full_name_text) - 1 );

    packages = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackagesByNamePublisher(
        manager, NULL, publisher, &packages );
    ok( hr == E_INVALIDARG && !packages,
        "Invalid name returned hr %#lx, packages %p.\n", hr, packages );
    packages = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackagesByPackageFamilyName(
        manager, NULL, &packages );
    ok( hr == E_INVALIDARG && !packages,
        "Invalid family returned hr %#lx, packages %p.\n", hr, packages );
    hr = IPackageManager_FindPackagesByNamePublisher(
        manager, name, publisher, NULL );
    ok( hr == E_POINTER, "Null output returned hr %#lx.\n", hr );
    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackagesByNamePublisher(
        manager, embedded_name, publisher, &packages );
    ok( hr == E_INVALIDARG && !packages && !query_calls && !iterable_calls,
        "Embedded-nul name query returned hr %#lx, packages %p, queries %ld, iterables %ld.\n",
        hr, packages, query_calls, iterable_calls );
    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackagesByNamePublisher(
        manager, name, embedded_publisher, &packages );
    ok( hr == E_INVALIDARG && !packages && !query_calls && !iterable_calls,
        "Embedded-nul publisher query returned hr %#lx, packages %p, queries %ld, iterables %ld.\n",
        hr, packages, query_calls, iterable_calls );
    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackagesByPackageFamilyName(
        manager, embedded_family, &packages );
    ok( hr == E_INVALIDARG && !packages && !query_calls && !iterable_calls,
        "Embedded-nul family query returned hr %#lx, packages %p, queries %ld, iterables %ld.\n",
        hr, packages, query_calls, iterable_calls );
    reset_capture();
    package = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackageByPackageFullName(
        manager, embedded_full_name, &package );
    ok( hr == E_INVALIDARG && !package && !query_calls && !package_create_calls,
        "Embedded-nul full-name query returned hr %#lx, package %p, queries %ld, creates %ld.\n",
        hr, package, query_calls, package_create_calls );
    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager2_FindPackagesByNamePublisherWithPackageTypes(
        manager2, embedded_name, publisher, PackageTypes_Main, &packages );
    ok( hr == E_INVALIDARG && !packages && !query_calls && !iterable_calls,
        "Typed embedded-nul name query returned hr %#lx, packages %p, queries %ld, iterables %ld.\n",
        hr, packages, query_calls, iterable_calls );
    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager2_FindPackagesByNamePublisherWithPackageTypes(
        manager2, name, embedded_publisher, PackageTypes_Main, &packages );
    ok( hr == E_INVALIDARG && !packages && !query_calls && !iterable_calls,
        "Typed embedded-nul publisher query returned hr %#lx, packages %p, queries %ld, iterables %ld.\n",
        hr, packages, query_calls, iterable_calls );
    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager2_FindPackagesByPackageFamilyNameWithPackageTypes(
        manager2, embedded_family, PackageTypes_Main, &packages );
    ok( hr == E_INVALIDARG && !packages && !query_calls && !iterable_calls,
        "Typed embedded-nul family query returned hr %#lx, packages %p, queries %ld, iterables %ld.\n",
        hr, packages, query_calls, iterable_calls );

    reset_capture();
    packages = NULL;
    hr = IPackageManager_FindPackages( manager, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468,
        "FindPackages returned hr %#lx, packages %p.\n", hr, packages );
    ok( initialize_calls == 0 && result_free_calls == 0 &&
        query_calls == 1 && iterable_calls == 1 &&
        captured_catalog == (APPX_CATALOG_SNAPSHOT *)0xabcdef01 &&
        captured_filter.types ==
            (PackageTypes)(PackageTypes_Main | PackageTypes_Framework |
                           PackageTypes_Resource) &&
        !captured_filter.name && !captured_filter.publisher &&
        !captured_filter.family_name,
        "Unexpected unfiltered query capture.\n" );

    reset_capture();
    packages = NULL;
    hr = IPackageManager_FindPackagesByNamePublisher(
        manager, name, publisher, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468,
        "Name query returned hr %#lx, packages %p.\n", hr, packages );
    ok( !wcscmp(captured_filter.name, L"Contoso.Main") &&
        !wcscmp(captured_filter.publisher, L"CN=Contoso"),
        "Unexpected name/publisher filter %s / %s.\n",
        wine_dbgstr_w(captured_filter.name),
        wine_dbgstr_w(captured_filter.publisher) );

    reset_capture();
    packages = NULL;
    hr = IPackageManager_FindPackagesByPackageFamilyName(
        manager, family, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468,
        "Family query returned hr %#lx, packages %p.\n", hr, packages );
    ok( !wcscmp(captured_filter.family_name, L"Contoso.Main_contoso"),
        "Unexpected family filter %s.\n",
        wine_dbgstr_w(captured_filter.family_name) );

    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager2_FindPackagesWithPackageTypes(
        manager2, PackageTypes_Bundle, &packages );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && !packages,
        "Bundle query returned hr %#lx, packages %p.\n", hr, packages );

    reset_capture();
    query_hr = HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_NOT_FOUND);
    package = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackageByPackageFullName(
        manager, full_name, &package );
    ok( hr == S_OK && !package && !package_create_calls,
        "Missing package returned hr %#lx, package %p, creates %ld.\n",
        hr, package, package_create_calls );

    reset_capture();
    package = NULL;
    hr = IPackageManager_FindPackageByPackageFullName(
        manager, full_name, &package );
    ok( hr == S_OK && package == (void *)0x24681357 &&
        package_create_calls == 1 &&
        !wcscmp( captured_full_name,
                 L"Contoso.Main_1.2.3.4_x64__contoso" ),
        "Package query returned hr %#lx, package %p, creates %ld, name %s.\n",
        hr, package, package_create_calls,
        wine_dbgstr_w(captured_full_name) );

    WindowsDeleteString( embedded_full_name );
    WindowsDeleteString( embedded_family );
    WindowsDeleteString( embedded_publisher );
    WindowsDeleteString( embedded_name );
    WindowsDeleteString( full_name );
    WindowsDeleteString( family );
    WindowsDeleteString( publisher );
    WindowsDeleteString( name );
    IPackageManager2_Release( manager2 );
    IPackageManager_Release( manager );
    IInspectable_Release( inspectable );
}

static void test_user_sid_queries(void)
{
    static const WCHAR other_sid_text[] = L"S-1-1-0";
    static const WCHAR invalid_sid_text[] = L"not-a-sid";
    static const WCHAR embedded_sid_text[] = L"S-1-5-21\0-1234";
    IActivationFactory *factory = package_manager_factory;
    IInspectable *inspectable = NULL;
    IPackageManager2 *manager2 = NULL;
    IPackageManager *manager = NULL;
    IIterable_Package *packages;
    IPackage *package;
    WCHAR *current_sid_text = NULL;
    HSTRING current_sid = NULL, other_sid = NULL, invalid_sid = NULL;
    HSTRING embedded_sid = NULL, name = NULL, publisher = NULL;
    HSTRING family = NULL, full_name = NULL;
    HRESULT hr;

    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    ok( hr == S_OK, "ActivateInstance failed, hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    hr = IInspectable_QueryInterface(
        inspectable, &IID_IPackageManager, (void **)&manager );
    ok( hr == S_OK, "IPackageManager query failed, hr %#lx.\n", hr );
    if (FAILED(hr)) goto done;
    hr = IPackageManager_QueryInterface(
        manager, &IID_IPackageManager2, (void **)&manager2 );
    ok( hr == S_OK, "IPackageManager2 query failed, hr %#lx.\n", hr );
    if (FAILED(hr)) goto done;

    current_sid_text = get_current_user_sid_string();
    if (!current_sid_text) goto done;
    current_sid = create_test_hstring(
        current_sid_text, wcslen(current_sid_text) );
    other_sid = create_test_hstring(
        other_sid_text, ARRAY_SIZE(other_sid_text) - 1 );
    invalid_sid = create_test_hstring(
        invalid_sid_text, ARRAY_SIZE(invalid_sid_text) - 1 );
    embedded_sid = create_test_hstring(
        embedded_sid_text, ARRAY_SIZE(embedded_sid_text) - 1 );
    name = create_test_hstring( L"Contoso.Main", wcslen(L"Contoso.Main") );
    publisher = create_test_hstring(
        L"CN=Contoso", wcslen(L"CN=Contoso") );
    family = create_test_hstring(
        L"Contoso.Main_contoso", wcslen(L"Contoso.Main_contoso") );
    full_name = create_test_hstring(
        L"Contoso.Main_1.2.3.4_x64__contoso",
        wcslen(L"Contoso.Main_1.2.3.4_x64__contoso") );

    reset_capture();
    packages = NULL;
    hr = IPackageManager_FindPackagesByUserSecurityId(
        manager, NULL, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468 && query_calls == 1,
        "Empty SID query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    packages = NULL;
    hr = IPackageManager_FindPackagesByUserSecurityId(
        manager, current_sid, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468 && query_calls == 1,
        "Current SID query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    packages = NULL;
    hr = IPackageManager_FindPackagesByUserSecurityIdNamePublisher(
        manager, current_sid, name, publisher, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468 && query_calls == 1,
        "Current SID name query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    packages = NULL;
    hr = IPackageManager_FindPackagesByUserSecurityIdPackageFamilyName(
        manager, current_sid, family, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468 && query_calls == 1,
        "Current SID family query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    package = NULL;
    hr = IPackageManager_FindPackageByUserSecurityIdPackageFullName(
        manager, current_sid, full_name, &package );
    ok( hr == S_OK && package == (void *)0x24681357 && query_calls == 1,
        "Current SID package query returned %#lx, package %p, queries %ld.\n",
        hr, package, query_calls );

    reset_capture();
    packages = NULL;
    hr = IPackageManager2_FindPackagesByUserSecurityIdWithPackageTypes(
        manager2, current_sid, PackageTypes_Main, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468 && query_calls == 1,
        "Typed current SID query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    packages = NULL;
    hr = IPackageManager2_FindPackagesByUserSecurityIdNamePublisherWithPackageTypes(
        manager2, current_sid, name, publisher, PackageTypes_Main,
        &packages );
    ok( hr == S_OK && packages == (void *)0x13572468 && query_calls == 1,
        "Typed SID name query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    packages = NULL;
    hr = IPackageManager2_FindPackagesByUserSecurityIdPackageFamilyNameWithPackageTypes(
        manager2, current_sid, family, PackageTypes_Framework, &packages );
    ok( hr == S_OK && packages == (void *)0x13572468 && query_calls == 1,
        "Typed SID family query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackagesByUserSecurityId(
        manager, other_sid, &packages );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && !packages &&
        !query_calls,
        "Other SID query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackagesByUserSecurityId(
        manager, invalid_sid, &packages );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INVALID_SID) && !packages &&
        !query_calls,
        "Invalid SID query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

    reset_capture();
    packages = (void *)0xdeadbeef;
    hr = IPackageManager_FindPackagesByUserSecurityId(
        manager, embedded_sid, &packages );
    ok( hr == E_INVALIDARG && !packages && !query_calls,
        "Embedded-NUL SID query returned %#lx, packages %p, queries %ld.\n",
        hr, packages, query_calls );

done:
    WindowsDeleteString( full_name );
    WindowsDeleteString( family );
    WindowsDeleteString( publisher );
    WindowsDeleteString( name );
    WindowsDeleteString( embedded_sid );
    WindowsDeleteString( invalid_sid );
    WindowsDeleteString( other_sid );
    WindowsDeleteString( current_sid );
    LocalFree( current_sid_text );
    if (manager2) IPackageManager2_Release( manager2 );
    if (manager) IPackageManager_Release( manager );
    IInspectable_Release( inspectable );
}

static void test_hstring_input_bounds( void )
{
    static const WCHAR high_surrogate[] = {'x', 0xd800};
    static const WCHAR low_surrogate[] = {'x', 0xdc00};
    static const WCHAR valid_pair[] = {'x', 0xd83d, 0xde00};
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress
        *operation;
    IActivationFactory *factory = package_manager_factory;
    IInspectable *inspectable = NULL;
    IPackageManager *manager = NULL;
    IIterable_Package *packages;
    HSTRING value = NULL;
    WCHAR *too_long;
    HRESULT hr;

    hr = IActivationFactory_ActivateInstance( factory, &inspectable );
    ok( hr == S_OK, "ActivateInstance failed, hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    hr = IInspectable_QueryInterface(
        inspectable, &IID_IPackageManager, (void **)&manager );
    ok( hr == S_OK, "IPackageManager query failed, hr %#lx.\n", hr );
    if (FAILED(hr)) goto done;

    too_long = malloc(
        (WINE_APPX_MAX_PATH_CHARS + 1) * sizeof(*too_long) );
    ok( !!too_long, "Failed to allocate overlong HSTRING input.\n" );
    if (too_long)
    {
        memset( too_long, 'a',
                (WINE_APPX_MAX_PATH_CHARS + 1) * sizeof(*too_long) );
        hr = WindowsCreateString(
            too_long, WINE_APPX_MAX_PATH_CHARS + 1, &value );
        ok( hr == S_OK, "Overlong HSTRING creation returned %#lx.\n", hr );
        if (SUCCEEDED(hr))
        {
            reset_capture();
            packages = (void *)0xdeadbeef;
            hr = IPackageManager_FindPackagesByPackageFamilyName(
                manager, value, &packages );
            ok( hr == E_INVALIDARG && !packages && !query_calls,
                "Overlong query returned %#lx, packages %p, queries %ld.\n",
                hr, packages, query_calls );
            reset_capture();
            operation = (void *)0xdeadbeef;
            hr = IPackageManager_RemovePackageAsync(
                manager, value, &operation );
            ok( hr == E_INVALIDARG && !operation,
                "Overlong removal returned %#lx, operation %p.\n",
                hr, operation );
        }
        WindowsDeleteString( value );
        value = NULL;
        free( too_long );
    }

    hr = WindowsCreateString(
        high_surrogate, ARRAY_SIZE(high_surrogate), &value );
    ok( hr == S_OK, "High-surrogate HSTRING creation returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        reset_capture();
        packages = (void *)0xdeadbeef;
        hr = IPackageManager_FindPackagesByPackageFamilyName(
            manager, value, &packages );
        ok( hr == E_INVALIDARG && !packages && !query_calls,
            "High-surrogate query returned %#lx, packages %p, queries %ld.\n",
            hr, packages, query_calls );
    }
    WindowsDeleteString( value );
    value = NULL;

    hr = WindowsCreateString(
        low_surrogate, ARRAY_SIZE(low_surrogate), &value );
    ok( hr == S_OK, "Low-surrogate HSTRING creation returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        reset_capture();
        packages = (void *)0xdeadbeef;
        hr = IPackageManager_FindPackagesByPackageFamilyName(
            manager, value, &packages );
        ok( hr == E_INVALIDARG && !packages && !query_calls,
            "Low-surrogate query returned %#lx, packages %p, queries %ld.\n",
            hr, packages, query_calls );
    }
    WindowsDeleteString( value );
    value = NULL;

    hr = WindowsCreateString(
        valid_pair, ARRAY_SIZE(valid_pair), &value );
    ok( hr == S_OK, "Valid-pair HSTRING creation returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        reset_capture();
        packages = NULL;
        hr = IPackageManager_FindPackagesByPackageFamilyName(
            manager, value, &packages );
        ok( hr == S_OK && packages == (void *)0x13572468 &&
            query_calls == 1,
            "Valid-pair query returned %#lx, packages %p, queries %ld.\n",
            hr, packages, query_calls );
    }
    WindowsDeleteString( value );

done:
    if (manager) IPackageManager_Release( manager );
    IInspectable_Release( inspectable );
}

START_TEST(package)
{
    HRESULT hr = RoInitialize( RO_INIT_MULTITHREADED );

    ok( hr == S_OK || hr == S_FALSE,
        "RoInitialize returned hr %#lx.\n", hr );
    test_factory();
    test_local_uri_validation();
    test_deployment_methods();
    test_query_methods();
    test_user_sid_queries();
    test_hstring_input_bounds();
    if (SUCCEEDED(hr)) RoUninitialize();
}
