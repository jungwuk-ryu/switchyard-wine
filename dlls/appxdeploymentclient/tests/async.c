/*
 * Windows.Management.Deployment asynchronous operation tests
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "initguid.h"
#include "../async.c"

#include "wine/test.h"

static const GUID null_guid;

static HANDLE backend_entered;
static HANDLE backend_release;
static HRESULT backend_hr;
static BOOL backend_wait_for_cancel;
static LONG backend_result_frees;
static LONG backend_calls;
static LONG backend_active;
static LONG backend_peak_active;

static void update_backend_peak( LONG active )
{
    LONG observed = InterlockedCompareExchange(
        &backend_peak_active, 0, 0 );

    while (active > observed)
    {
        LONG previous = InterlockedCompareExchange(
            &backend_peak_active, active, observed );
        if (previous == observed) break;
        observed = previous;
    }
}

static HRESULT run_backend(
    const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    DWORD wait;
    LONG active;
    HRESULT hr;

    if (result) *result = NULL;
    InterlockedIncrement( &backend_calls );
    active = InterlockedIncrement( &backend_active );
    update_backend_peak( active );
    SetEvent( backend_entered );
    if (backend_wait_for_cancel)
    {
        wait = WaitForSingleObject( options->cancel_event, 5000 );
        hr = wait == WAIT_OBJECT_0 ?
             HRESULT_FROM_WIN32( ERROR_CANCELLED ) : E_FAIL;
        InterlockedDecrement( &backend_active );
        return hr;
    }
    wait = WaitForSingleObject( backend_release, 5000 );
    if (wait == WAIT_OBJECT_0)
    {
        if (result) *result = (APPX_DEPLOYMENT_RESULT *)0xdeadbeef;
        hr = backend_hr;
    }
    else
        hr = E_FAIL;
    InterlockedDecrement( &backend_active );
    return hr;
}

HRESULT appx_backend_deployment_install(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    return run_backend( options, result );
}

HRESULT appx_backend_deployment_update(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    return run_backend( options, result );
}

HRESULT appx_backend_deployment_remove(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    return run_backend( options, result );
}

void appx_backend_deployment_result_free( APPX_DEPLOYMENT_RESULT *result )
{
    if (result) InterlockedIncrement( &backend_result_frees );
}

struct progress_handler
{
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress iface;
    LONG ref;
    LONG calls;
    DeploymentProgress last;
};

static inline struct progress_handler *impl_from_progress_handler(
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress *iface )
{
    return CONTAINING_RECORD( iface, struct progress_handler, iface );
}

static HRESULT WINAPI progress_handler_QueryInterface(
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress *iface,
    REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID(
            iid,
            &IID_IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress ))
        *out = iface;
    else
        return E_NOINTERFACE;
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress_AddRef(
        iface );
    return S_OK;
}

static ULONG WINAPI progress_handler_AddRef(
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress *iface )
{
    struct progress_handler *impl = impl_from_progress_handler( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI progress_handler_Release(
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress *iface )
{
    struct progress_handler *impl = impl_from_progress_handler( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI progress_handler_Invoke(
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress *iface,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *operation,
    DeploymentProgress progress )
{
    struct progress_handler *impl = impl_from_progress_handler( iface );

    InterlockedIncrement( &impl->calls );
    impl->last = progress;
    return S_OK;
}

static const
IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgressVtbl
progress_handler_vtbl =
{
    progress_handler_QueryInterface,
    progress_handler_AddRef,
    progress_handler_Release,
    progress_handler_Invoke,
};

struct completed_handler
{
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        iface;
    LONG ref;
    LONG calls;
    AsyncStatus status;
    HANDLE invoked;
};

static inline struct completed_handler *impl_from_completed_handler(
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *iface )
{
    return CONTAINING_RECORD( iface, struct completed_handler, iface );
}

static HRESULT WINAPI completed_handler_QueryInterface(
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *iface,
    REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID(
            iid,
            &IID_IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress ))
        *out = iface;
    else
        return E_NOINTERFACE;
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_AddRef(
        iface );
    return S_OK;
}

static ULONG WINAPI completed_handler_AddRef(
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *iface )
{
    struct completed_handler *impl = impl_from_completed_handler( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI completed_handler_Release(
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *iface )
{
    struct completed_handler *impl = impl_from_completed_handler( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI completed_handler_Invoke(
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *iface,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *operation,
    AsyncStatus status )
{
    struct completed_handler *impl = impl_from_completed_handler( iface );

    impl->status = status;
    InterlockedIncrement( &impl->calls );
    SetEvent( impl->invoked );
    return S_OK;
}

static const
IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgressVtbl
completed_handler_vtbl =
{
    completed_handler_QueryInterface,
    completed_handler_AddRef,
    completed_handler_Release,
    completed_handler_Invoke,
};

static void setup_backend( HRESULT result, BOOL wait_for_cancel )
{
    backend_entered = CreateEventW( NULL, TRUE, FALSE, NULL );
    backend_release = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!backend_entered && !!backend_release,
        "Failed to create backend events, error %lu.\n", GetLastError() );
    backend_hr = result;
    backend_wait_for_cancel = wait_for_cancel;
    backend_result_frees = 0;
    backend_calls = 0;
    backend_active = 0;
    backend_peak_active = 0;
}

static void cleanup_backend(void)
{
    CloseHandle( backend_release );
    CloseHandle( backend_entered );
}

static void wait_for_operation(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *operation )
{
    struct deployment_async *impl = impl_from_async_operation( operation );

    WaitForThreadpoolWorkCallbacks( impl->work, FALSE );
}

static void test_success(void)
{
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress
        *operation;
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress
        *progress_iface;
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *completed_iface;
    struct progress_handler progress = {{&progress_handler_vtbl}, 1};
    struct completed_handler completed = {{&completed_handler_vtbl}, 1};
    IDeploymentResult *result1, *result2;
    DeploymentProgress reported;
    IAsyncInfo *info;
    struct deployment_async *impl;
    AsyncStatus status;
    HRESULT error;
    GUID activity;
    HANDLE package_file;
    HRESULT hr;
    DWORD wait;

    setup_backend( S_OK, FALSE );
    completed.invoked = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!completed.invoked,
        "Failed to create completion event, error %lu.\n", GetLastError() );
    package_file = CreateEventW( NULL, TRUE, FALSE, NULL );
    hr = package_async_operation_create(
        PACKAGE_DEPLOYMENT_INSTALL, package_file, NULL, 0, &operation );
    ok( hr == S_OK, "Operation create failed, hr %#lx.\n", hr );
    wait = WaitForSingleObject( backend_entered, 5000 );
    ok( wait == WAIT_OBJECT_0, "Backend did not start, wait %#lx.\n", wait );

    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_QueryInterface(
        operation, &IID_IAsyncInfo, (void **)&info );
    ok( hr == S_OK, "IAsyncInfo query failed, hr %#lx.\n", hr );
    hr = IAsyncInfo_Close( info );
    ok( hr == E_ILLEGAL_STATE_CHANGE,
        "Close while started returned hr %#lx.\n", hr );
    impl = impl_from_async_operation( operation );
    EnterCriticalSection( &impl->cs );
    reported = impl->last_progress;
    LeaveCriticalSection( &impl->cs );
    ok( reported.state == DeploymentProgressState_Processing &&
        reported.percentage == 0,
        "Unexpected blocked progress %d/%u.\n",
        reported.state, reported.percentage );
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_put_Progress(
        operation, &progress.iface );
    ok( hr == S_OK, "put_Progress failed, hr %#lx.\n", hr );
    progress_iface = (void *)0xdeadbeef;
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_get_Progress(
        operation, &progress_iface );
    ok( hr == S_OK && progress_iface,
        "get_Progress returned hr %#lx, handler %p.\n", hr, progress_iface );
    if (progress_iface)
        IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress_Release(
            progress_iface );
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_put_Completed(
        operation, &completed.iface );
    ok( hr == S_OK, "put_Completed failed, hr %#lx.\n", hr );
    completed_iface = (void *)0xdeadbeef;
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_get_Completed(
        operation, &completed_iface );
    ok( hr == S_OK && completed_iface,
        "get_Completed returned hr %#lx, handler %p.\n",
        hr, completed_iface );
    if (completed_iface)
        IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_Release(
            completed_iface );
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_put_Completed(
        operation, &completed.iface );
    ok( hr == E_ILLEGAL_DELEGATE_ASSIGNMENT,
        "Duplicate put_Completed returned hr %#lx.\n", hr );
    SetEvent( backend_release );
    wait_for_operation( operation );
    wait = WaitForSingleObject( completed.invoked, 5000 );
    ok( wait == WAIT_OBJECT_0, "Completion handler wait %#lx.\n", wait );
    ok( completed.calls == 1 && completed.status == Completed,
        "Completion calls %ld, status %d.\n",
        completed.calls, completed.status );
    ok( progress.calls == 1 &&
        progress.last.state == DeploymentProgressState_Processing &&
        progress.last.percentage == 100,
        "Progress calls %ld, last %d/%u.\n", progress.calls,
        progress.last.state, progress.last.percentage );

    hr = IAsyncInfo_get_Status( info, &status );
    ok( hr == S_OK && status == Completed,
        "Status %d, hr %#lx.\n", status, hr );
    hr = IAsyncInfo_get_ErrorCode( info, &error );
    ok( hr == S_OK && error == S_OK,
        "ErrorCode %#lx, hr %#lx.\n", error, hr );
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_GetResults(
        operation, &result1 );
    ok( hr == S_OK, "GetResults failed, hr %#lx.\n", hr );
    hr = IDeploymentResult_get_ExtendedErrorCode( result1, &error );
    ok( hr == S_OK && error == S_OK,
        "Extended error %#lx, hr %#lx.\n", error, hr );
    hr = IDeploymentResult_get_ActivityId( result1, &activity );
    ok( hr == S_OK && !IsEqualGUID( &activity, &null_guid ),
        "Invalid activity id, hr %#lx.\n", hr );
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_GetResults(
        operation, &result2 );
    ok( hr == S_OK && result1 == result2,
        "Repeated result differs, hr %#lx.\n", hr );
    IDeploymentResult_Release( result2 );
    IDeploymentResult_Release( result1 );
    ok( backend_result_frees == 1,
        "Backend result free count %ld.\n", backend_result_frees );

    hr = IAsyncInfo_Close( info );
    ok( hr == S_OK, "Close failed, hr %#lx.\n", hr );
    hr = IAsyncInfo_get_Status( info, &status );
    ok( hr == E_ILLEGAL_METHOD_CALL,
        "Closed status returned hr %#lx.\n", hr );
    IAsyncInfo_Release( info );
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
        operation );
    ok( progress.ref == 1, "Progress refcount %ld.\n", progress.ref );
    ok( completed.ref == 1, "Completed refcount %ld.\n", completed.ref );
    CloseHandle( completed.invoked );
    cleanup_backend();
}

static void test_deployment_failure(void)
{
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress
        *operation;
    IDeploymentResult *result;
    IAsyncInfo *info;
    HSTRING text;
    AsyncStatus status;
    HRESULT error, hr;
    HANDLE package_file;

    setup_backend( E_ACCESSDENIED, FALSE );
    package_file = CreateEventW( NULL, TRUE, FALSE, NULL );
    hr = package_async_operation_create(
        PACKAGE_DEPLOYMENT_UPDATE, package_file, NULL,
        APPX_DEPLOYMENT_ALLOW_DOWNGRADE, &operation );
    ok( hr == S_OK, "Operation create failed, hr %#lx.\n", hr );
    ok( WaitForSingleObject( backend_entered, 5000 ) == WAIT_OBJECT_0,
        "Backend did not start.\n" );
    SetEvent( backend_release );
    wait_for_operation( operation );

    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_QueryInterface(
        operation, &IID_IAsyncInfo, (void **)&info );
    ok( hr == S_OK, "IAsyncInfo query failed, hr %#lx.\n", hr );
    hr = IAsyncInfo_get_Status( info, &status );
    ok( hr == S_OK && status == Completed,
        "Deployment failure status %d, hr %#lx.\n", status, hr );
    hr = IAsyncInfo_get_ErrorCode( info, &error );
    ok( hr == S_OK && error == S_OK,
        "Async error %#lx, hr %#lx.\n", error, hr );
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_GetResults(
        operation, &result );
    ok( hr == S_OK, "GetResults failed, hr %#lx.\n", hr );
    hr = IDeploymentResult_get_ExtendedErrorCode( result, &error );
    ok( hr == S_OK && error == E_ACCESSDENIED,
        "Extended error %#lx, hr %#lx.\n", error, hr );
    hr = IDeploymentResult_get_ErrorText( result, &text );
    ok( hr == S_OK && WindowsGetStringLen( text ),
        "Missing failure text, hr %#lx.\n", hr );
    WindowsDeleteString( text );
    IDeploymentResult_Release( result );
    hr = IAsyncInfo_Close( info );
    ok( hr == S_OK, "Failure Close returned hr %#lx.\n", hr );
    IAsyncInfo_Release( info );
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
        operation );
    cleanup_backend();
}

static void test_cancel(void)
{
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress
        *operation;
    IDeploymentResult *result = (IDeploymentResult *)0xdeadbeef;
    IAsyncInfo *info;
    AsyncStatus status;
    HRESULT error, hr;

    setup_backend( S_OK, TRUE );
    hr = package_async_operation_create(
        PACKAGE_DEPLOYMENT_REMOVE, INVALID_HANDLE_VALUE,
        L"Contoso.Main_1.2.3.4_x64__contoso", 0, &operation );
    ok( hr == S_OK, "Operation create failed, hr %#lx.\n", hr );
    ok( WaitForSingleObject( backend_entered, 5000 ) == WAIT_OBJECT_0,
        "Backend did not start.\n" );
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_QueryInterface(
        operation, &IID_IAsyncInfo, (void **)&info );
    ok( hr == S_OK, "IAsyncInfo query failed, hr %#lx.\n", hr );
    hr = IAsyncInfo_Cancel( info );
    ok( hr == S_OK, "Cancel failed, hr %#lx.\n", hr );
    wait_for_operation( operation );
    hr = IAsyncInfo_get_Status( info, &status );
    ok( hr == S_OK && status == Canceled,
        "Canceled status %d, hr %#lx.\n", status, hr );
    hr = IAsyncInfo_get_ErrorCode( info, &error );
    ok( hr == S_OK && error == E_ABORT,
        "Canceled error %#lx, hr %#lx.\n", error, hr );
    hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_GetResults(
        operation, &result );
    ok( hr == E_ILLEGAL_METHOD_CALL && !result,
        "Canceled GetResults hr %#lx, result %p.\n", hr, result );
    ok( !backend_result_frees,
        "Unexpected backend result free count %ld.\n", backend_result_frees );
    hr = IAsyncInfo_Close( info );
    ok( hr == S_OK, "Canceled Close returned hr %#lx.\n", hr );
    IAsyncInfo_Release( info );
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
        operation );
    cleanup_backend();
}

static void test_cancel_close_race(void)
{
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress
        *operation;
    IAsyncInfo *info;
    AsyncStatus status;
    HRESULT hr;
    UINT32 i, attempts;

    for (i = 0; i < 16; i++)
    {
        setup_backend( S_OK, TRUE );
        hr = package_async_operation_create(
            PACKAGE_DEPLOYMENT_REMOVE, INVALID_HANDLE_VALUE,
            L"Contoso.Main_1.2.3.4_x64__contoso", 0, &operation );
        ok( hr == S_OK, "Iteration %u creation failed, hr %#lx.\n", i, hr );
        if (FAILED(hr))
        {
            cleanup_backend();
            continue;
        }
        ok( WaitForSingleObject( backend_entered, 5000 ) == WAIT_OBJECT_0,
            "Iteration %u backend did not start.\n", i );
        hr = IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_QueryInterface(
            operation, &IID_IAsyncInfo, (void **)&info );
        ok( hr == S_OK, "Iteration %u IAsyncInfo query failed, hr %#lx.\n",
            i, hr );
        if (FAILED(hr))
        {
            IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
                operation );
            cleanup_backend();
            continue;
        }
        hr = IAsyncInfo_Cancel( info );
        ok( hr == S_OK, "Iteration %u Cancel failed, hr %#lx.\n", i, hr );

        for (attempts = 0; attempts < 5000; attempts++)
        {
            hr = IAsyncInfo_Close( info );
            if (hr == S_OK) break;
            if (hr != E_ILLEGAL_STATE_CHANGE)
            {
                ok( 0, "Iteration %u racing Close returned %#lx.\n", i, hr );
                break;
            }
            Sleep( 0 );
        }
        ok( hr == S_OK, "Iteration %u never reached closable state, hr %#lx.\n",
            i, hr );
        wait_for_operation( operation );
        status = Started;
        hr = IAsyncInfo_get_Status( info, &status );
        ok( hr == E_ILLEGAL_METHOD_CALL && status == ASYNC_STATUS_CLOSED,
            "Iteration %u closed status returned %#lx, state %d.\n",
            i, hr, status );
        IAsyncInfo_Release( info );
        IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
            operation );
        cleanup_backend();
    }
}

static void test_bounded_worker_pool(void)
{
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress
        *operations[32] = {0};
    UINT32 created = 0, i;
    HRESULT hr;

    setup_backend( S_OK, FALSE );
    for (i = 0; i < ARRAY_SIZE(operations); i++)
    {
        hr = package_async_operation_create(
            PACKAGE_DEPLOYMENT_REMOVE, INVALID_HANDLE_VALUE,
            L"Contoso.Main_1.2.3.4_x64__contoso", 0,
            operations + i );
        ok( hr == S_OK, "Operation %u creation failed, hr %#lx.\n", i, hr );
        if (FAILED(hr)) break;
        created++;
    }

    for (i = 0; i < 500 &&
         InterlockedCompareExchange(&backend_calls, 0, 0) <
             DEPLOYMENT_ASYNC_MAX_WORKERS; i++)
        Sleep( 10 );
    ok( backend_calls == DEPLOYMENT_ASYNC_MAX_WORKERS,
        "Blocked pool started %ld workers, expected %u.\n",
        backend_calls, DEPLOYMENT_ASYNC_MAX_WORKERS );
    ok( backend_active <= DEPLOYMENT_ASYNC_MAX_WORKERS &&
        backend_peak_active <= DEPLOYMENT_ASYNC_MAX_WORKERS,
        "Blocked pool active/peak workers %ld/%ld, limit %u.\n",
        backend_active, backend_peak_active,
        DEPLOYMENT_ASYNC_MAX_WORKERS );

    SetEvent( backend_release );
    for (i = 0; i < created; i++)
        wait_for_operation( operations[i] );
    ok( backend_calls == created,
        "Completed pool ran %ld of %u operations.\n",
        backend_calls, created );
    ok( !backend_active &&
        backend_peak_active <= DEPLOYMENT_ASYNC_MAX_WORKERS,
        "Completed pool active/peak workers %ld/%ld, limit %u.\n",
        backend_active, backend_peak_active,
        DEPLOYMENT_ASYNC_MAX_WORKERS );
    ok( backend_result_frees == created,
        "Backend result free count %ld, expected %u.\n",
        backend_result_frees, created );
    for (i = 0; i < created; i++)
        IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
            operations[i] );
    cleanup_backend();
}

START_TEST(async)
{
    HRESULT hr = RoInitialize( RO_INIT_MULTITHREADED );

    ok( hr == S_OK || hr == S_FALSE,
        "RoInitialize returned hr %#lx.\n", hr );
    test_success();
    test_deployment_failure();
    test_cancel();
    test_cancel_close_race();
    test_bounded_worker_pool();
    if (SUCCEEDED(hr)) RoUninitialize();

    hr = RoInitialize( RO_INIT_SINGLETHREADED );
    ok( hr == S_OK || hr == S_FALSE,
        "STA RoInitialize returned hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        winetest_push_context( "STA" );
        test_success();
        winetest_pop_context();
        RoUninitialize();
    }
}
