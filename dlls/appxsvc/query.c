/*
 * AppX installed package query helpers
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * These private entry points keep the package store path and catalog snapshot
 * representation out of kernelbase.  Every successful query is made against
 * one verified immutable deployment snapshot, and all snapshot-owned data is
 * copied before the snapshot is released.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"

#include "deployment.h"
#include "query.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appx);

static const WCHAR default_store_root[] =
    L"C:\\Program Files\\WindowsApps\\.wine-msix-store";

static enum appx_catalog_architecture get_native_architecture( void )
{
    SYSTEM_INFO info;

    GetNativeSystemInfo( &info );
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_INTEL:
        return APPX_CATALOG_ARCHITECTURE_X86;
    case PROCESSOR_ARCHITECTURE_AMD64:
        return APPX_CATALOG_ARCHITECTURE_X64;
    case PROCESSOR_ARCHITECTURE_ARM:
        return APPX_CATALOG_ARCHITECTURE_ARM;
    case PROCESSOR_ARCHITECTURE_ARM64:
        return APPX_CATALOG_ARCHITECTURE_ARM64;
    default:
        return APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    }
}

static void init_query_options( APPX_DEPLOYMENT_OPTIONS *options )
{
    memset( options, 0, sizeof(*options) );
    options->size = sizeof(*options);
    options->version = APPX_DEPLOYMENT_OPTIONS_VERSION;
    options->store_root = default_store_root;
    options->target_architecture = get_native_architecture();
}

static LONG query_error( HRESULT hr )
{
    DWORD error;

    if (SUCCEEDED(hr)) return ERROR_SUCCESS;
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
    {
        error = HRESULT_CODE(hr);
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_INSTALL_PACKAGE_NOT_FOUND)
            return ERROR_NOT_FOUND;
        return error;
    }
    if (hr == E_OUTOFMEMORY) return ERROR_NOT_ENOUGH_MEMORY;
    if (hr == E_INVALIDARG) return ERROR_INVALID_PARAMETER;
    if (hr == APPX_E_INVALID_PACKAGING_LAYOUT ||
        hr == APPX_E_CORRUPT_CONTENT)
        return ERROR_BAD_FORMAT;
    WARN( "Unexpected package query failure %#lx.\n", hr );
    return ERROR_INVALID_DATA;
}

static HRESULT load_query_snapshot( const WCHAR *full_name,
                                    APPX_CATALOG_SNAPSHOT **snapshot )
{
    APPX_DEPLOYMENT_OPTIONS options;

    init_query_options( &options );
    return appx_deployment_query( full_name, &options, snapshot );
}

static const struct appx_catalog_package *get_active_package(
    const APPX_CATALOG_SNAPSHOT *snapshot )
{
    const struct appx_catalog_package *package;

    if (appx_catalog_snapshot_get_package_count( snapshot ) != 1)
        return NULL;
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    if (!package || !(package->flags & APPX_CATALOG_PACKAGE_ACTIVE))
        return NULL;
    return package;
}

static LONG copy_package_string( const WCHAR *string, UINT32 *length,
                                 WCHAR *buffer )
{
    UINT32 capacity, required;

    if (!string || !length) return ERROR_INVALID_PARAMETER;
    required = lstrlenW( string ) + 1;
    capacity = *length;
    *length = required;
    if (!buffer || capacity < required) return ERROR_INSUFFICIENT_BUFFER;
    memcpy( buffer, string, required * sizeof(*buffer) );
    return ERROR_SUCCESS;
}

LONG WINAPI wine_appx_get_package_path_by_full_name(
    const WCHAR *full_name, UINT32 *length, WCHAR *path )
{
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    const struct appx_catalog_package *package;
    WCHAR absolute_path[WINE_APPX_MAX_PATH_CHARS];
    UINT32 root_length, payload_length;
    HRESULT hr;
    LONG ret;

    TRACE( "full_name %s, length %p, path %p.\n",
           debugstr_w(full_name), length, path );

    if (!full_name || !length) return ERROR_INVALID_PARAMETER;
    if (FAILED(hr = load_query_snapshot( full_name, &snapshot )))
        return query_error( hr );
    if (!(package = get_active_package( snapshot )))
    {
        ret = ERROR_NOT_FOUND;
        goto done;
    }

    root_length = ARRAY_SIZE(default_store_root) - 1;
    payload_length = lstrlenW( package->payload_path );
    if (!payload_length ||
        root_length >= ARRAY_SIZE(absolute_path) - 1 ||
        payload_length >= ARRAY_SIZE(absolute_path) - root_length - 1)
    {
        ret = ERROR_BAD_FORMAT;
        goto done;
    }
    memcpy( absolute_path, default_store_root,
            root_length * sizeof(*absolute_path) );
    absolute_path[root_length] = '\\';
    memcpy( absolute_path + root_length + 1, package->payload_path,
            (payload_length + 1) * sizeof(*absolute_path) );
    ret = copy_package_string( absolute_path, length, path );

done:
    appx_catalog_snapshot_free( snapshot );
    return ret;
}

LONG WINAPI wine_appx_get_package_publisher_by_full_name(
    const WCHAR *full_name, UINT32 *length, WCHAR *publisher )
{
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    const struct appx_catalog_package *package;
    HRESULT hr;
    LONG ret;

    TRACE( "full_name %s, length %p, publisher %p.\n",
           debugstr_w(full_name), length, publisher );

    if (!full_name || !length) return ERROR_INVALID_PARAMETER;
    if (FAILED(hr = load_query_snapshot( full_name, &snapshot )))
        return query_error( hr );
    if (!(package = get_active_package( snapshot )))
        ret = ERROR_NOT_FOUND;
    else
        ret = copy_package_string( package->publisher, length, publisher );
    appx_catalog_snapshot_free( snapshot );
    return ret;
}

LONG WINAPI wine_appx_get_packages_by_family(
    const WCHAR *family_name, UINT32 *count, WCHAR **full_names,
    UINT32 *buffer_length, WCHAR *buffer )
{
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    UINT32 capacity, buffer_capacity, package_count, required_count = 0;
    UINT32 required_chars = 0, i;
    HRESULT hr;
    LONG ret = ERROR_SUCCESS;
    WCHAR *cursor;

    TRACE( "family_name %s, count %p, full_names %p, buffer_length %p, "
           "buffer %p.\n", debugstr_w(family_name), count, full_names,
           buffer_length, buffer );

    if (!family_name || !count || !buffer_length)
        return ERROR_INVALID_PARAMETER;
    if (FAILED(hr = load_query_snapshot( NULL, &snapshot )))
    {
        ret = query_error( hr );
        if (ret != ERROR_NOT_FOUND) return ret;
        *count = 0;
        *buffer_length = 0;
        return ERROR_SUCCESS;
    }

    package_count = appx_catalog_snapshot_get_package_count( snapshot );
    for (i = 0; i < package_count; i++)
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( snapshot, i );
        UINT32 chars;

        if (!package || !(package->flags & APPX_CATALOG_PACKAGE_ACTIVE) ||
            CompareStringOrdinal( family_name, -1, package->family_name,
                                  -1, TRUE ) != CSTR_EQUAL)
            continue;
        chars = lstrlenW( package->full_name ) + 1;
        if (required_count == MAXDWORD ||
            required_chars > MAXDWORD - chars)
        {
            ret = ERROR_ARITHMETIC_OVERFLOW;
            goto done;
        }
        required_count++;
        required_chars += chars;
    }

    capacity = *count;
    buffer_capacity = *buffer_length;
    *count = required_count;
    *buffer_length = required_chars;
    if (!required_count) goto done;
    if (!full_names || !buffer || capacity < required_count ||
        buffer_capacity < required_chars)
    {
        ret = ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }

    cursor = buffer;
    required_count = 0;
    for (i = 0; i < package_count; i++)
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( snapshot, i );
        UINT32 chars;

        if (!package || !(package->flags & APPX_CATALOG_PACKAGE_ACTIVE) ||
            CompareStringOrdinal( family_name, -1, package->family_name,
                                  -1, TRUE ) != CSTR_EQUAL)
            continue;
        chars = lstrlenW( package->full_name ) + 1;
        full_names[required_count++] = cursor;
        memcpy( cursor, package->full_name, chars * sizeof(*cursor) );
        cursor += chars;
    }

done:
    appx_catalog_snapshot_free( snapshot );
    return ret;
}
