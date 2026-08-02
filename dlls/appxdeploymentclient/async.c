/*
 * Windows.Management.Deployment asynchronous deployment operations
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

#include "combaseapi.h"
#include "roapi.h"

#include <stdio.h>

#define ASYNC_STATUS_CLOSED ((AsyncStatus)4)
#define DEPLOYMENT_ASYNC_MAX_WORKERS 4

struct deployment_result
{
    IDeploymentResult IDeploymentResult_iface;
    LONG ref;
    HSTRING error_text;
    GUID activity_id;
    HRESULT extended_error;
};

struct deployment_async
{
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress
        IAsyncOperationWithProgress_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    UINT32 id;
    AsyncStatus status;
    HRESULT error;
    BOOL completed_assigned;
    IAgileReference *progress_handler;
    IAgileReference *completed_handler;
    IDeploymentResult *result;
    DeploymentProgress last_progress;

    enum package_deployment_operation operation;
    HANDLE package_file;
    WCHAR *full_name;
    UINT32 deployment_flags;
    HANDLE cancel_event;
    PTP_WORK work;
    GUID activity_id;
};

static LONG next_async_id;
static INIT_ONCE deployment_pool_init_once = INIT_ONCE_STATIC_INIT;
static TP_POOL *deployment_pool;
static TP_CALLBACK_ENVIRON deployment_pool_environment;
static HRESULT deployment_pool_hr = E_UNEXPECTED;

static BOOL CALLBACK deployment_pool_initialize(
    INIT_ONCE *once, void *parameter, void **context )
{
    UNREFERENCED_PARAMETER( once );
    UNREFERENCED_PARAMETER( parameter );
    UNREFERENCED_PARAMETER( context );

    memset( &deployment_pool_environment, 0,
            sizeof(deployment_pool_environment) );
    deployment_pool_environment.Version = 1;
    if (!(deployment_pool = CreateThreadpool( NULL )))
    {
        deployment_pool_hr = HRESULT_FROM_WIN32( GetLastError() );
        return TRUE;
    }
    SetThreadpoolThreadMaximum(
        deployment_pool, DEPLOYMENT_ASYNC_MAX_WORKERS );
    deployment_pool_environment.Pool = deployment_pool;
    deployment_pool_hr = S_OK;
    return TRUE;
}

static HRESULT get_deployment_pool_environment(
    TP_CALLBACK_ENVIRON **environment )
{
    if (!InitOnceExecuteOnce(
            &deployment_pool_init_once, deployment_pool_initialize,
            NULL, NULL ))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (FAILED(deployment_pool_hr)) return deployment_pool_hr;
    *environment = &deployment_pool_environment;
    return S_OK;
}

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

static inline struct deployment_result *impl_from_IDeploymentResult(
    IDeploymentResult *iface )
{
    return CONTAINING_RECORD( iface, struct deployment_result,
                              IDeploymentResult_iface );
}

static HRESULT WINAPI deployment_result_QueryInterface(
    IDeploymentResult *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDeploymentResult ))
        *out = iface;
    else
        return E_NOINTERFACE;
    IDeploymentResult_AddRef( iface );
    return S_OK;
}

static ULONG WINAPI deployment_result_AddRef( IDeploymentResult *iface )
{
    struct deployment_result *impl = impl_from_IDeploymentResult( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI deployment_result_Release( IDeploymentResult *iface )
{
    struct deployment_result *impl = impl_from_IDeploymentResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->error_text );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI deployment_result_GetIids(
    IDeploymentResult *iface, ULONG *iid_count, IID **iids )
{
    const IID supported[] = {IID_IDeploymentResult};
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI deployment_result_GetRuntimeClassName(
    IDeploymentResult *iface, HSTRING *class_name )
{
    return runtime_class_name(
        RuntimeClass_Windows_Management_Deployment_DeploymentResult,
        class_name );
}

static HRESULT WINAPI deployment_result_GetTrustLevel(
    IDeploymentResult *iface, TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI deployment_result_get_ErrorText(
    IDeploymentResult *iface, HSTRING *value )
{
    struct deployment_result *impl = impl_from_IDeploymentResult( iface );

    if (!value) return E_INVALIDARG;
    return WindowsDuplicateString( impl->error_text, value );
}

static HRESULT WINAPI deployment_result_get_ActivityId(
    IDeploymentResult *iface, GUID *value )
{
    struct deployment_result *impl = impl_from_IDeploymentResult( iface );

    if (!value) return E_INVALIDARG;
    *value = impl->activity_id;
    return S_OK;
}

static HRESULT WINAPI deployment_result_get_ExtendedErrorCode(
    IDeploymentResult *iface, HRESULT *value )
{
    struct deployment_result *impl = impl_from_IDeploymentResult( iface );

    if (!value) return E_INVALIDARG;
    *value = impl->extended_error;
    return S_OK;
}

static const IDeploymentResultVtbl deployment_result_vtbl =
{
    deployment_result_QueryInterface,
    deployment_result_AddRef,
    deployment_result_Release,
    deployment_result_GetIids,
    deployment_result_GetRuntimeClassName,
    deployment_result_GetTrustLevel,
    deployment_result_get_ErrorText,
    deployment_result_get_ActivityId,
    deployment_result_get_ExtendedErrorCode,
};

static HRESULT create_error_text( HRESULT error, HSTRING *value )
{
    WCHAR fallback[64], *message = NULL;
    DWORD length;
    HRESULT hr;

    *value = NULL;
    if (SUCCEEDED(error)) return WindowsCreateString( NULL, 0, value );
    length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, NULL, HRESULT_CODE(error), 0,
        (WCHAR *)&message, 0, NULL );
    if (length)
    {
        while (length && (message[length - 1] == '\r' ||
                          message[length - 1] == '\n'))
            length--;
        hr = WindowsCreateString( message, length, value );
        LocalFree( message );
        return hr;
    }
    swprintf( fallback, ARRAY_SIZE(fallback),
              L"Package deployment failed with error 0x%08lx.",
              (ULONG)error );
    return WindowsCreateString( fallback, wcslen(fallback), value );
}

static HRESULT deployment_result_create( HRESULT extended_error,
                                         const GUID *activity_id,
                                         IDeploymentResult **value )
{
    struct deployment_result *impl;
    HRESULT hr;

    *value = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IDeploymentResult_iface.lpVtbl = &deployment_result_vtbl;
    impl->ref = 1;
    impl->extended_error = extended_error;
    impl->activity_id = *activity_id;
    if (FAILED( hr = create_error_text( extended_error,
                                        &impl->error_text ) ))
    {
        IDeploymentResult_Release( &impl->IDeploymentResult_iface );
        return hr;
    }
    *value = &impl->IDeploymentResult_iface;
    return S_OK;
}

static inline struct deployment_async *impl_from_async_operation(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface )
{
    return CONTAINING_RECORD(
        iface, struct deployment_async,
        IAsyncOperationWithProgress_iface );
}

static inline struct deployment_async *impl_from_IAsyncInfo(
    IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct deployment_async,
                              IAsyncInfo_iface );
}

static HRESULT create_progress_handler_reference(
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress *handler,
    IAgileReference **reference )
{
    if (!reference) return E_POINTER;
    *reference = NULL;
    if (!handler) return S_OK;
    return RoGetAgileReference(
        AGILEREFERENCE_DEFAULT,
        &IID_IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress,
        (IUnknown *)handler, reference );
}

static HRESULT create_completed_handler_reference(
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *handler,
    IAgileReference **reference )
{
    if (!reference) return E_POINTER;
    *reference = NULL;
    if (!handler) return S_OK;
    return RoGetAgileReference(
        AGILEREFERENCE_DEFAULT,
        &IID_IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress,
        (IUnknown *)handler, reference );
}

static HRESULT resolve_progress_handler_reference(
    IAgileReference *reference,
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress
        **handler )
{
    if (!handler) return E_POINTER;
    *handler = NULL;
    if (!reference) return S_OK;
    return IAgileReference_Resolve(
        reference,
        &IID_IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress,
        (void **)handler );
}

static HRESULT resolve_completed_handler_reference(
    IAgileReference *reference,
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        **handler )
{
    if (!handler) return E_POINTER;
    *handler = NULL;
    if (!reference) return S_OK;
    return IAgileReference_Resolve(
        reference,
        &IID_IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress,
        (void **)handler );
}

static HRESULT WINAPI deployment_async_QueryInterface(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    REFIID iid, void **out )
{
    struct deployment_async *impl = impl_from_async_operation( iface );

    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID(
            iid,
            &IID_IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress ))
        *out = &impl->IAsyncOperationWithProgress_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo ))
        *out = &impl->IAsyncInfo_iface;
    else
        return E_NOINTERFACE;
    IInspectable_AddRef( *out );
    return S_OK;
}

static ULONG WINAPI deployment_async_AddRef(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface )
{
    struct deployment_async *impl = impl_from_async_operation( iface );
    return InterlockedIncrement( &impl->ref );
}

static void deployment_async_destroy( struct deployment_async *impl )
{
    IAgileReference *progress;
    IAgileReference *completed;
    IDeploymentResult *result;

    EnterCriticalSection( &impl->cs );
    progress = impl->progress_handler;
    impl->progress_handler = NULL;
    completed = impl->completed_handler;
    impl->completed_handler = NULL;
    result = impl->result;
    impl->result = NULL;
    LeaveCriticalSection( &impl->cs );

    if (progress) IAgileReference_Release( progress );
    if (completed) IAgileReference_Release( completed );
    if (result) IDeploymentResult_Release( result );
    if (impl->work) CloseThreadpoolWork( impl->work );
    if (impl->cancel_event) CloseHandle( impl->cancel_event );
    if (impl->package_file != INVALID_HANDLE_VALUE)
        CloseHandle( impl->package_file );
    free( impl->full_name );
    DeleteCriticalSection( &impl->cs );
    free( impl );
}

static ULONG WINAPI deployment_async_Release(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface )
{
    struct deployment_async *impl = impl_from_async_operation( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref) deployment_async_destroy( impl );
    return ref;
}

static HRESULT WINAPI deployment_async_GetIids(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    ULONG *iid_count, IID **iids )
{
    const IID supported[] = {
        IID_IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress,
        IID_IAsyncInfo,
    };
    return copy_iids( supported, ARRAY_SIZE(supported), iid_count, iids );
}

static HRESULT WINAPI deployment_async_GetRuntimeClassName(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    HSTRING *class_name )
{
    static const WCHAR name[] =
        L"Windows.Foundation.IAsyncOperationWithProgress`2<"
        L"Windows.Management.Deployment.DeploymentResult,"
        L"Windows.Management.Deployment.DeploymentProgress>";
    return runtime_class_name( name, class_name );
}

static HRESULT WINAPI deployment_async_GetTrustLevel(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    TrustLevel *trust_level )
{
    return get_base_trust( trust_level );
}

static HRESULT WINAPI deployment_async_put_Progress(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress *handler )
{
    struct deployment_async *impl = impl_from_async_operation( iface );
    IAgileReference *reference = NULL, *old;
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    if (impl->status != Started)
    {
        LeaveCriticalSection( &impl->cs );
        return E_ILLEGAL_METHOD_CALL;
    }
    LeaveCriticalSection( &impl->cs );

    if (FAILED( hr = create_progress_handler_reference(
            handler, &reference ) ))
        return hr;

    EnterCriticalSection( &impl->cs );
    if (impl->status != Started)
    {
        LeaveCriticalSection( &impl->cs );
        if (reference) IAgileReference_Release( reference );
        return E_ILLEGAL_METHOD_CALL;
    }
    old = impl->progress_handler;
    impl->progress_handler = reference;
    LeaveCriticalSection( &impl->cs );
    if (old) IAgileReference_Release( old );
    return S_OK;
}

static HRESULT WINAPI deployment_async_get_Progress(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress **handler )
{
    struct deployment_async *impl = impl_from_async_operation( iface );
    IAgileReference *reference = NULL;
    HRESULT hr = S_OK;

    if (!handler) return E_POINTER;
    *handler = NULL;
    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_STATUS_CLOSED)
        hr = E_ILLEGAL_METHOD_CALL;
    else if (impl->progress_handler)
    {
        reference = impl->progress_handler;
        IAgileReference_AddRef( reference );
    }
    LeaveCriticalSection( &impl->cs );
    if (reference)
    {
        hr = resolve_progress_handler_reference( reference, handler );
        IAgileReference_Release( reference );
    }
    return hr;
}

static HRESULT call_completed_handler(
    struct deployment_async *impl,
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *handler,
    AsyncStatus status )
{
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *operation =
        &impl->IAsyncOperationWithProgress_iface;
    HRESULT hr;

    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_AddRef(
        operation );
    hr = IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_Invoke(
        handler, operation, status );
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
        operation );
    return hr;
}

static HRESULT call_completed_handler_reference(
    struct deployment_async *impl, IAgileReference *reference,
    AsyncStatus status )
{
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *handler;
    HRESULT hr;

    if (FAILED( hr = resolve_completed_handler_reference(
            reference, &handler ) ))
        return hr;
    hr = call_completed_handler( impl, handler, status );
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_Release(
        handler );
    return hr;
}

static HRESULT WINAPI deployment_async_put_Completed(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        *handler )
{
    struct deployment_async *impl = impl_from_async_operation( iface );
    IAgileReference *reference = NULL;
    AsyncStatus status;
    BOOL call_now = FALSE;
    HRESULT hr, ret;

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_STATUS_CLOSED)
    {
        LeaveCriticalSection( &impl->cs );
        return E_ILLEGAL_METHOD_CALL;
    }
    if (impl->completed_assigned)
    {
        LeaveCriticalSection( &impl->cs );
        return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    }
    if (!handler)
    {
        impl->completed_assigned = TRUE;
        LeaveCriticalSection( &impl->cs );
        return S_OK;
    }
    status = impl->status;
    if (status != Started)
    {
        impl->completed_assigned = TRUE;
        IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_AddRef(
            handler );
        LeaveCriticalSection( &impl->cs );
        call_completed_handler( impl, handler, status );
        IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_Release(
            handler );
        return S_OK;
    }
    LeaveCriticalSection( &impl->cs );

    if (FAILED( hr = create_completed_handler_reference(
            handler, &reference ) ))
    {
        EnterCriticalSection( &impl->cs );
        if (impl->status == ASYNC_STATUS_CLOSED)
            ret = E_ILLEGAL_METHOD_CALL;
        else if (impl->completed_assigned)
            ret = E_ILLEGAL_DELEGATE_ASSIGNMENT;
        else if (impl->status != Started)
        {
            status = impl->status;
            impl->completed_assigned = TRUE;
            IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_AddRef(
                handler );
            call_now = TRUE;
            ret = S_OK;
        }
        else
            ret = hr;
        LeaveCriticalSection( &impl->cs );
        if (call_now)
        {
            call_completed_handler( impl, handler, status );
            IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_Release(
                handler );
        }
        return ret;
    }

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_STATUS_CLOSED)
        ret = E_ILLEGAL_METHOD_CALL;
    else if (impl->completed_assigned)
        ret = E_ILLEGAL_DELEGATE_ASSIGNMENT;
    else
    {
        status = impl->status;
        impl->completed_assigned = TRUE;
        ret = S_OK;
        if (status == Started)
        {
            impl->completed_handler = reference;
            reference = NULL;
        }
        else
        {
            IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_AddRef(
                handler );
            call_now = TRUE;
        }
    }
    LeaveCriticalSection( &impl->cs );
    if (reference) IAgileReference_Release( reference );
    if (call_now)
    {
        call_completed_handler( impl, handler, status );
        IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress_Release(
            handler );
    }
    return ret;
}

static HRESULT WINAPI deployment_async_get_Completed(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    IAsyncOperationWithProgressCompletedHandler_DeploymentResult_DeploymentProgress
        **handler )
{
    struct deployment_async *impl = impl_from_async_operation( iface );
    IAgileReference *reference = NULL;
    HRESULT hr = S_OK;

    if (!handler) return E_POINTER;
    *handler = NULL;
    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_STATUS_CLOSED)
        hr = E_ILLEGAL_METHOD_CALL;
    else if (impl->completed_handler)
    {
        reference = impl->completed_handler;
        IAgileReference_AddRef( reference );
    }
    LeaveCriticalSection( &impl->cs );
    if (reference)
    {
        hr = resolve_completed_handler_reference( reference, handler );
        IAgileReference_Release( reference );
    }
    return hr;
}

static HRESULT WINAPI deployment_async_GetResults(
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *iface,
    IDeploymentResult **results )
{
    struct deployment_async *impl = impl_from_async_operation( iface );
    HRESULT hr;

    if (!results) return E_POINTER;
    *results = NULL;
    EnterCriticalSection( &impl->cs );
    if (impl->status == Completed)
    {
        if (impl->result)
        {
            *results = impl->result;
            IDeploymentResult_AddRef( *results );
            hr = S_OK;
        }
        else
            hr = E_UNEXPECTED;
    }
    else if (impl->status == Error)
        hr = impl->error;
    else
        hr = E_ILLEGAL_METHOD_CALL;
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static const
IAsyncOperationWithProgress_DeploymentResult_DeploymentProgressVtbl
deployment_async_vtbl =
{
    deployment_async_QueryInterface,
    deployment_async_AddRef,
    deployment_async_Release,
    deployment_async_GetIids,
    deployment_async_GetRuntimeClassName,
    deployment_async_GetTrustLevel,
    deployment_async_put_Progress,
    deployment_async_get_Progress,
    deployment_async_put_Completed,
    deployment_async_get_Completed,
    deployment_async_GetResults,
};

static HRESULT WINAPI deployment_async_info_QueryInterface(
    IAsyncInfo *iface, REFIID iid, void **out )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_QueryInterface(
        &impl->IAsyncOperationWithProgress_iface, iid, out );
}

static ULONG WINAPI deployment_async_info_AddRef( IAsyncInfo *iface )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_AddRef(
        &impl->IAsyncOperationWithProgress_iface );
}

static ULONG WINAPI deployment_async_info_Release( IAsyncInfo *iface )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
        &impl->IAsyncOperationWithProgress_iface );
}

static HRESULT WINAPI deployment_async_info_GetIids(
    IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_GetIids(
        &impl->IAsyncOperationWithProgress_iface, iid_count, iids );
}

static HRESULT WINAPI deployment_async_info_GetRuntimeClassName(
    IAsyncInfo *iface, HSTRING *class_name )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_GetRuntimeClassName(
        &impl->IAsyncOperationWithProgress_iface, class_name );
}

static HRESULT WINAPI deployment_async_info_GetTrustLevel(
    IAsyncInfo *iface, TrustLevel *trust_level )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_GetTrustLevel(
        &impl->IAsyncOperationWithProgress_iface, trust_level );
}

static HRESULT WINAPI deployment_async_info_get_Id(
    IAsyncInfo *iface, UINT32 *value )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_STATUS_CLOSED)
    {
        LeaveCriticalSection( &impl->cs );
        return E_ILLEGAL_METHOD_CALL;
    }
    *value = impl->id;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI deployment_async_info_get_Status(
    IAsyncInfo *iface, AsyncStatus *value )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *value = impl->status;
    if (impl->status == ASYNC_STATUS_CLOSED)
    {
        LeaveCriticalSection( &impl->cs );
        return E_ILLEGAL_METHOD_CALL;
    }
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI deployment_async_info_get_ErrorCode(
    IAsyncInfo *iface, HRESULT *value )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );

    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_STATUS_CLOSED)
    {
        *value = E_ILLEGAL_METHOD_CALL;
        LeaveCriticalSection( &impl->cs );
        return E_ILLEGAL_METHOD_CALL;
    }
    *value = impl->error;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI deployment_async_info_Cancel( IAsyncInfo *iface )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (impl->status == ASYNC_STATUS_CLOSED)
        hr = E_ILLEGAL_METHOD_CALL;
    else if (impl->status == Started && !SetEvent( impl->cancel_event ))
        hr = HRESULT_FROM_WIN32( GetLastError() );
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI deployment_async_info_Close( IAsyncInfo *iface )
{
    struct deployment_async *impl = impl_from_IAsyncInfo( iface );
    IAgileReference *progress = NULL, *completed = NULL;
    IDeploymentResult *result = NULL;
    HRESULT hr = S_OK;

    EnterCriticalSection( &impl->cs );
    if (impl->status == Started)
        hr = E_ILLEGAL_STATE_CHANGE;
    else if (impl->status != ASYNC_STATUS_CLOSED)
    {
        impl->status = ASYNC_STATUS_CLOSED;
        impl->error = E_ILLEGAL_METHOD_CALL;
        progress = impl->progress_handler;
        impl->progress_handler = NULL;
        completed = impl->completed_handler;
        impl->completed_handler = NULL;
        result = impl->result;
        impl->result = NULL;
    }
    LeaveCriticalSection( &impl->cs );
    if (progress) IAgileReference_Release( progress );
    if (completed) IAgileReference_Release( completed );
    if (result) IDeploymentResult_Release( result );
    return hr;
}

static const IAsyncInfoVtbl deployment_async_info_vtbl =
{
    deployment_async_info_QueryInterface,
    deployment_async_info_AddRef,
    deployment_async_info_Release,
    deployment_async_info_GetIids,
    deployment_async_info_GetRuntimeClassName,
    deployment_async_info_GetTrustLevel,
    deployment_async_info_get_Id,
    deployment_async_info_get_Status,
    deployment_async_info_get_ErrorCode,
    deployment_async_info_Cancel,
    deployment_async_info_Close,
};

static void report_progress( struct deployment_async *impl,
                             DeploymentProgressState state,
                             UINT32 percentage )
{
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress
        *handler = NULL;
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress *operation =
        &impl->IAsyncOperationWithProgress_iface;
    IAgileReference *reference = NULL;
    DeploymentProgress progress;
    HRESULT hr;

    progress.state = state;
    progress.percentage = percentage;
    EnterCriticalSection( &impl->cs );
    if (impl->status == Started)
    {
        impl->last_progress = progress;
        if (impl->progress_handler)
        {
            reference = impl->progress_handler;
            IAgileReference_AddRef( reference );
        }
    }
    LeaveCriticalSection( &impl->cs );
    if (!reference) return;
    if (FAILED( hr = resolve_progress_handler_reference(
            reference, &handler ) ))
    {
        IAgileReference_Release( reference );
        return;
    }
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_AddRef(
        operation );
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress_Invoke(
        handler, operation, progress );
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
        operation );
    IAsyncOperationProgressHandler_DeploymentResult_DeploymentProgress_Release(
        handler );
    IAgileReference_Release( reference );
}

static void CALLBACK deployment_async_worker(
    TP_CALLBACK_INSTANCE *instance, void *parameter, PTP_WORK work )
{
    struct deployment_async *impl = parameter;
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *service_result = NULL;
    IDeploymentResult *result = NULL;
    IAgileReference *progress = NULL, *completed = NULL;
    AsyncStatus status;
    HRESULT hr, init_hr, result_hr;
    BOOL initialized = FALSE;

    UNREFERENCED_PARAMETER( instance );
    UNREFERENCED_PARAMETER( work );
    init_hr = RoInitialize( RO_INIT_MULTITHREADED );
    if (SUCCEEDED(init_hr)) initialized = TRUE;

    report_progress( impl, DeploymentProgressState_Processing, 0 );
    package_deployment_options_init(
        &options, impl->deployment_flags, impl->cancel_event );
    if (WaitForSingleObject( impl->cancel_event, 0 ) == WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32( ERROR_CANCELLED );
    else
    {
        switch (impl->operation)
        {
        case PACKAGE_DEPLOYMENT_INSTALL:
            hr = appx_backend_deployment_install(
                impl->package_file, &options, &service_result );
            break;
        case PACKAGE_DEPLOYMENT_UPDATE:
            hr = appx_backend_deployment_update(
                impl->package_file, &options, &service_result );
            break;
        case PACKAGE_DEPLOYMENT_REMOVE:
            hr = appx_backend_deployment_remove(
                impl->full_name, &options, &service_result );
            break;
        default:
            hr = E_UNEXPECTED;
            break;
        }
    }
    if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        result_hr = deployment_result_create(
            hr, &impl->activity_id, &result );
    else
        result_hr = S_OK;
    appx_backend_deployment_result_free( service_result );

    if (SUCCEEDED(hr)) report_progress(
        impl, DeploymentProgressState_Processing, 100 );

    EnterCriticalSection( &impl->cs );
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        impl->status = Canceled;
        impl->error = E_ABORT;
    }
    else if (FAILED(result_hr))
    {
        impl->status = Error;
        impl->error = result_hr;
    }
    else
    {
        impl->status = Completed;
        impl->error = S_OK;
        impl->result = result;
        result = NULL;
    }
    status = impl->status;
    progress = impl->progress_handler;
    impl->progress_handler = NULL;
    completed = impl->completed_handler;
    impl->completed_handler = NULL;
    LeaveCriticalSection( &impl->cs );

    if (progress) IAgileReference_Release( progress );
    if (result) IDeploymentResult_Release( result );
    if (completed)
    {
        call_completed_handler_reference( impl, completed, status );
        IAgileReference_Release( completed );
    }
    if (initialized) RoUninitialize();
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress_Release(
        &impl->IAsyncOperationWithProgress_iface );
}

void package_deployment_options_init( APPX_DEPLOYMENT_OPTIONS *options,
                                      UINT32 flags, HANDLE cancel_event )
{
    SYSTEM_INFO info;

    memset( options, 0, sizeof(*options) );
    options->size = sizeof(*options);
    options->version = APPX_DEPLOYMENT_OPTIONS_VERSION;
    options->store_root = APPX_STORE_ROOT;
    options->flags = flags;
    options->cancel_event = cancel_event;
    GetNativeSystemInfo( &info );
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_INTEL:
        options->target_architecture = APPX_CATALOG_ARCHITECTURE_X86;
        break;
    case PROCESSOR_ARCHITECTURE_AMD64:
        options->target_architecture = APPX_CATALOG_ARCHITECTURE_X64;
        break;
    case PROCESSOR_ARCHITECTURE_ARM:
        options->target_architecture = APPX_CATALOG_ARCHITECTURE_ARM;
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        options->target_architecture = APPX_CATALOG_ARCHITECTURE_ARM64;
        break;
    default:
        options->target_architecture = APPX_CATALOG_ARCHITECTURE_NEUTRAL;
        break;
    }
}

HRESULT package_async_operation_create(
    enum package_deployment_operation operation, HANDLE package_file,
    const WCHAR *full_name, UINT32 deployment_flags,
    IAsyncOperationWithProgress_DeploymentResult_DeploymentProgress **value )
{
    struct deployment_async *impl;
    TP_CALLBACK_ENVIRON *environment;
    UINT32 full_name_length;
    SIZE_T bytes;
    HRESULT hr;

    if (!value)
    {
        if (package_file && package_file != INVALID_HANDLE_VALUE)
            CloseHandle( package_file );
        return E_POINTER;
    }
    *value = NULL;
    if ((operation == PACKAGE_DEPLOYMENT_REMOVE && !full_name) ||
        (operation != PACKAGE_DEPLOYMENT_REMOVE &&
         (!package_file || package_file == INVALID_HANDLE_VALUE)))
    {
        if (package_file && package_file != INVALID_HANDLE_VALUE)
            CloseHandle( package_file );
        return E_INVALIDARG;
    }
    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        if (package_file && package_file != INVALID_HANDLE_VALUE)
            CloseHandle( package_file );
        return E_OUTOFMEMORY;
    }
    impl->IAsyncOperationWithProgress_iface.lpVtbl = &deployment_async_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &deployment_async_info_vtbl;
    impl->ref = 1;
    impl->id = InterlockedIncrement( &next_async_id );
    if (!impl->id) impl->id = InterlockedIncrement( &next_async_id );
    impl->status = Started;
    impl->error = S_OK;
    impl->last_progress.state = DeploymentProgressState_Queued;
    impl->last_progress.percentage = 0;
    impl->operation = operation;
    impl->package_file = package_file;
    impl->deployment_flags = deployment_flags;
    InitializeCriticalSection( &impl->cs );

    if (full_name)
    {
        if (FAILED( hr = appxclient_bounded_wstring_length(
                full_name, FALSE, &full_name_length ) ))
            goto failed;
        bytes = (full_name_length + 1) * sizeof(*full_name);
        if (!(impl->full_name = malloc( bytes )))
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
        memcpy( impl->full_name, full_name, bytes );
    }
    if (!(impl->cancel_event = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto failed;
    }
    if (FAILED( hr = CoCreateGuid( &impl->activity_id ) )) goto failed;
    if (FAILED( hr = get_deployment_pool_environment(
            &environment ) ))
        goto failed;

    InterlockedIncrement( &impl->ref );
    impl->work = CreateThreadpoolWork(
        deployment_async_worker, impl, environment );
    if (!impl->work)
    {
        InterlockedDecrement( &impl->ref );
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto failed;
    }
    SubmitThreadpoolWork( impl->work );
    *value = &impl->IAsyncOperationWithProgress_iface;
    return S_OK;

failed:
    deployment_async_destroy( impl );
    return hr;
}
