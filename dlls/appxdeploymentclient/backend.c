/*
 * Windows.Management.Deployment private appxsvc bridge
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

struct appxsvc_private_api
{
    HMODULE module;
    HRESULT (WINAPI *deployment_initialize)(
        const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT **);
    HRESULT (WINAPI *deployment_install)(
        HANDLE, const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT **);
    HRESULT (WINAPI *deployment_update)(
        HANDLE, const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT **);
    HRESULT (WINAPI *deployment_remove)(
        const WCHAR *, const APPX_DEPLOYMENT_OPTIONS *,
        APPX_DEPLOYMENT_RESULT **);
    HRESULT (WINAPI *deployment_query)(
        const WCHAR *, const APPX_DEPLOYMENT_OPTIONS *,
        APPX_CATALOG_SNAPSHOT **);
    void (WINAPI *deployment_result_free)( APPX_DEPLOYMENT_RESULT * );
    void (WINAPI *catalog_snapshot_free)( APPX_CATALOG_SNAPSHOT * );
    UINT32 (WINAPI *catalog_snapshot_get_package_count)(
        const APPX_CATALOG_SNAPSHOT * );
    const struct appx_catalog_package *(WINAPI
        *catalog_snapshot_get_package)(
        const APPX_CATALOG_SNAPSHOT *, UINT32 );
    HRESULT (WINAPI *resolve_direct_dependencies)(
        const APPX_CATALOG_SNAPSHOT *, const WCHAR *,
        enum appx_catalog_architecture, UINT32, UINT32 *, UINT32 * );
};

static INIT_ONCE appxsvc_init_once = INIT_ONCE_STATIC_INIT;
static struct appxsvc_private_api appxsvc_api;
static HRESULT appxsvc_init_hr = E_UNEXPECTED;

static BOOL CALLBACK appxsvc_private_api_initialize(
    INIT_ONCE *once, void *parameter, void **context )
{
    FARPROC proc;

    if (!(appxsvc_api.module = LoadLibraryExW(
            L"appxsvc.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 )))
    {
        appxsvc_init_hr = HRESULT_FROM_WIN32( GetLastError() );
        return TRUE;
    }

#define LOAD_PRIVATE_API(member, name)                                      \
    do                                                                      \
    {                                                                       \
        if (!(proc = GetProcAddress( appxsvc_api.module, #name )))          \
        {                                                                   \
            appxsvc_init_hr = HRESULT_FROM_WIN32( ERROR_PROC_NOT_FOUND );   \
            return TRUE;                                                    \
        }                                                                   \
        memcpy( &appxsvc_api.member, &proc, sizeof(proc) );                 \
    } while (0)

    LOAD_PRIVATE_API( deployment_initialize, appx_deployment_initialize );
    LOAD_PRIVATE_API( deployment_install, appx_deployment_install );
    LOAD_PRIVATE_API( deployment_update, appx_deployment_update );
    LOAD_PRIVATE_API( deployment_remove, appx_deployment_remove );
    LOAD_PRIVATE_API( deployment_query, appx_deployment_query );
    LOAD_PRIVATE_API(
        deployment_result_free, appx_deployment_result_free );
    LOAD_PRIVATE_API(
        catalog_snapshot_free, appx_catalog_snapshot_free );
    LOAD_PRIVATE_API(
        catalog_snapshot_get_package_count,
        appx_catalog_snapshot_get_package_count );
    LOAD_PRIVATE_API(
        catalog_snapshot_get_package, appx_catalog_snapshot_get_package );
    LOAD_PRIVATE_API(
        resolve_direct_dependencies,
        appx_package_graph_resolve_direct_dependencies );
#undef LOAD_PRIVATE_API

    appxsvc_init_hr = S_OK;
    return TRUE;
}

static HRESULT get_appxsvc_private_api(
    const struct appxsvc_private_api **value )
{
    if (!InitOnceExecuteOnce( &appxsvc_init_once,
                             appxsvc_private_api_initialize, NULL, NULL ))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (FAILED(appxsvc_init_hr)) return appxsvc_init_hr;
    *value = &appxsvc_api;
    return S_OK;
}

HRESULT appx_backend_deployment_initialize(
    const APPX_DEPLOYMENT_OPTIONS *options, APPX_DEPLOYMENT_RESULT **result )
{
    const struct appxsvc_private_api *api;
    HRESULT hr;

    if (FAILED( hr = get_appxsvc_private_api( &api ) )) return hr;
    return api->deployment_initialize( options, result );
}

HRESULT appx_backend_deployment_install(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    const struct appxsvc_private_api *api;
    HRESULT hr;

    if (FAILED( hr = get_appxsvc_private_api( &api ) )) return hr;
    return api->deployment_install( package_file, options, result );
}

HRESULT appx_backend_deployment_update(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    const struct appxsvc_private_api *api;
    HRESULT hr;

    if (FAILED( hr = get_appxsvc_private_api( &api ) )) return hr;
    return api->deployment_update( package_file, options, result );
}

HRESULT appx_backend_deployment_remove(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    const struct appxsvc_private_api *api;
    HRESULT hr;

    if (FAILED( hr = get_appxsvc_private_api( &api ) )) return hr;
    return api->deployment_remove( full_name, options, result );
}

HRESULT appx_backend_deployment_query(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_CATALOG_SNAPSHOT **snapshot )
{
    const struct appxsvc_private_api *api;
    HRESULT hr;

    if (FAILED( hr = get_appxsvc_private_api( &api ) )) return hr;
    return api->deployment_query( full_name, options, snapshot );
}

void appx_backend_deployment_result_free( APPX_DEPLOYMENT_RESULT *result )
{
    const struct appxsvc_private_api *api;

    if (!result || FAILED( get_appxsvc_private_api( &api ) )) return;
    api->deployment_result_free( result );
}

void appx_backend_catalog_snapshot_free( APPX_CATALOG_SNAPSHOT *snapshot )
{
    const struct appxsvc_private_api *api;

    if (!snapshot || FAILED( get_appxsvc_private_api( &api ) )) return;
    api->catalog_snapshot_free( snapshot );
}

UINT32 appx_backend_catalog_snapshot_get_package_count(
    const APPX_CATALOG_SNAPSHOT *snapshot )
{
    const struct appxsvc_private_api *api;

    if (!snapshot || FAILED( get_appxsvc_private_api( &api ) )) return 0;
    return api->catalog_snapshot_get_package_count( snapshot );
}

const struct appx_catalog_package *
appx_backend_catalog_snapshot_get_package(
    const APPX_CATALOG_SNAPSHOT *snapshot, UINT32 index )
{
    const struct appxsvc_private_api *api;

    if (!snapshot || FAILED( get_appxsvc_private_api( &api ) )) return NULL;
    return api->catalog_snapshot_get_package( snapshot, index );
}

HRESULT appx_backend_resolve_direct_dependencies(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *package_full_name,
    enum appx_catalog_architecture target_architecture, UINT32 capacity,
    UINT32 *package_indices, UINT32 *count )
{
    const struct appxsvc_private_api *api;
    HRESULT hr;

    if (FAILED( hr = get_appxsvc_private_api( &api ) )) return hr;
    return api->resolve_direct_dependencies(
        snapshot, package_full_name, target_architecture, capacity,
        package_indices, count );
}
