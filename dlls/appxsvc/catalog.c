/*
 * AppX package catalog persistence
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

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"
#include "winternl.h"
#include "bcrypt.h"

#include "catalog.h"
#include "wine/appxsvc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define CATALOG_PACKAGE_RECORD_SIZE_OFFSET          0
#define CATALOG_PACKAGE_FLAGS_OFFSET                4
#define CATALOG_PACKAGE_ARCHITECTURE_OFFSET         8
#define CATALOG_PACKAGE_APPLICATION_COUNT_OFFSET    12
#define CATALOG_PACKAGE_DEPENDENCY_COUNT_OFFSET     16
#define CATALOG_PACKAGE_APPLICATIONS_OFFSET         20
#define CATALOG_PACKAGE_DEPENDENCIES_OFFSET         24
#define CATALOG_PACKAGE_STRINGS_OFFSET              28
#define CATALOG_PACKAGE_STRINGS_SIZE_OFFSET         32
#define CATALOG_PACKAGE_VERSION_OFFSET              36
#define CATALOG_PACKAGE_CONTENT_ID_OFFSET           44
#define CATALOG_PACKAGE_STRING_REFS_OFFSET          76
#define CATALOG_PACKAGE_FIXED_SIZE                  132

#define CATALOG_APPLICATION_RECORD_SIZE             44
#define CATALOG_APPLICATION_KIND_OFFSET             0
#define CATALOG_APPLICATION_ID_REF_OFFSET           4
#define CATALOG_APPLICATION_EXECUTABLE_REF_OFFSET   12
#define CATALOG_APPLICATION_ENTRY_POINT_REF_OFFSET  20
#define CATALOG_APPLICATION_PARAMETERS_REF_OFFSET   28
#define CATALOG_APPLICATION_CURRENT_DIR_REF_OFFSET  36

#define CATALOG_DEPENDENCY_RECORD_SIZE              24
#define CATALOG_DEPENDENCY_VERSION_OFFSET           0
#define CATALOG_DEPENDENCY_NAME_REF_OFFSET          8
#define CATALOG_DEPENDENCY_PUBLISHER_REF_OFFSET     16

#define CATALOG_HEADER_VERSION_OFFSET               8
#define CATALOG_HEADER_SIZE_OFFSET                  12
#define CATALOG_HEADER_FILE_SIZE_OFFSET             16
#define CATALOG_HEADER_EPOCH_OFFSET                 24
#define CATALOG_HEADER_PACKAGE_COUNT_OFFSET         32
#define CATALOG_HEADER_RECORDS_OFFSET               40
#define CATALOG_HEADER_DIGEST_OFFSET                48
#define CATALOG_HEADER_RESERVED_OFFSET              80
#define CATALOG_HEADER_RESERVED_SIZE                16

#define CATALOG_DIGEST_SIZE                         32
#define CATALOG_LOCK_POLL_MS                        20

static const BYTE catalog_magic[8] = {'S','W','X','C','A','T','L','G'};

enum package_string_index
{
    PACKAGE_STRING_NAME,
    PACKAGE_STRING_PUBLISHER,
    PACKAGE_STRING_RESOURCE_ID,
    PACKAGE_STRING_PUBLISHER_ID,
    PACKAGE_STRING_FULL_NAME,
    PACKAGE_STRING_FAMILY_NAME,
    PACKAGE_STRING_PAYLOAD_PATH,
    PACKAGE_STRING_COUNT
};

struct catalog_application_storage
{
    WCHAR *strings[5];
};

struct catalog_dependency_storage
{
    WCHAR *strings[2];
};

struct catalog_package
{
    struct appx_catalog_package view;
    WCHAR *strings[PACKAGE_STRING_COUNT];
    struct appx_catalog_application *applications;
    struct catalog_application_storage *application_storage;
    struct appx_catalog_dependency *dependencies;
    struct catalog_dependency_storage *dependency_storage;
};

struct appx_catalog_snapshot
{
    UINT64 epoch;
    UINT32 count;
    struct catalog_package *packages;
};

struct catalog_buffer
{
    BYTE *data;
    UINT32 size;
    UINT32 capacity;
};

struct catalog_store
{
    HANDLE root;
};

struct catalog_lock
{
    HANDLE handle;
    OVERLAPPED overlapped;
    BOOL locked;
};

static UINT16 read_uint16( const BYTE *data )
{
    return data[0] | ((UINT16)data[1] << 8);
}

static UINT32 read_uint32( const BYTE *data )
{
    return read_uint16( data ) | ((UINT32)read_uint16( data + 2 ) << 16);
}

static UINT64 read_uint64( const BYTE *data )
{
    return read_uint32( data ) | ((UINT64)read_uint32( data + 4 ) << 32);
}

static void write_uint16( BYTE *data, UINT16 value )
{
    data[0] = value;
    data[1] = value >> 8;
}

static void write_uint32( BYTE *data, UINT32 value )
{
    write_uint16( data, value );
    write_uint16( data + 2, value >> 16 );
}

static void write_uint64( BYTE *data, UINT64 value )
{
    write_uint32( data, value );
    write_uint32( data + 4, value >> 32 );
}

static BOOL add_uint32( UINT32 left, UINT32 right, UINT32 *result )
{
    if (MAXDWORD - left < right) return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL multiply_uint32( UINT32 left, UINT32 right, UINT32 *result )
{
    if (left && right > MAXDWORD / left) return FALSE;
    *result = left * right;
    return TRUE;
}

static HRESULT malformed_catalog( void )
{
    return APPX_E_INVALID_PACKAGING_LAYOUT;
}

static HRESULT win32_error( DWORD error )
{
    return error ? HRESULT_FROM_WIN32( error ) : E_FAIL;
}

static HRESULT ntstatus_error( NTSTATUS status )
{
    return win32_error( RtlNtStatusToDosError( status ) );
}

static UINT64 pack_version( const struct appx_catalog_version *version )
{
    return version->major | ((UINT64)version->minor << 16) |
           ((UINT64)version->build << 32) | ((UINT64)version->revision << 48);
}

static struct appx_catalog_version unpack_version( UINT64 packed )
{
    struct appx_catalog_version version;

    version.major = packed;
    version.minor = packed >> 16;
    version.build = packed >> 32;
    version.revision = packed >> 48;
    return version;
}

static BOOL versions_equal( const struct appx_catalog_version *left,
                            const struct appx_catalog_version *right )
{
    return left->major == right->major && left->minor == right->minor &&
           left->build == right->build && left->revision == right->revision;
}

static INT compare_string_ci( const WCHAR *left, const WCHAR *right )
{
    INT result = CompareStringOrdinal( left, -1, right, -1, TRUE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static INT compare_string_canonical( const WCHAR *left, const WCHAR *right )
{
    INT result = compare_string_ci( left, right );

    if (result) return result;
    result = CompareStringOrdinal( left, -1, right, -1, FALSE );
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static INT compare_package_full_name( const void *left, const void *right )
{
    const struct catalog_package *left_package = left;
    const struct catalog_package *right_package = right;

    return compare_string_canonical( left_package->view.full_name,
                                     right_package->view.full_name );
}

static BOOL is_valid_architecture( enum appx_catalog_architecture architecture )
{
    return architecture == APPX_CATALOG_ARCHITECTURE_NEUTRAL ||
           architecture == APPX_CATALOG_ARCHITECTURE_X86 ||
           architecture == APPX_CATALOG_ARCHITECTURE_X64 ||
           architecture == APPX_CATALOG_ARCHITECTURE_ARM ||
           architecture == APPX_CATALOG_ARCHITECTURE_ARM64 ||
           architecture == APPX_CATALOG_ARCHITECTURE_X86A64;
}

static BOOL is_valid_activation_kind( enum appx_catalog_activation_kind kind )
{
    return kind == APPX_CATALOG_ACTIVATION_UNSUPPORTED ||
           kind == APPX_CATALOG_ACTIVATION_FULL_TRUST ||
           kind == APPX_CATALOG_ACTIVATION_PACKAGED_CLASSIC ||
           kind == APPX_CATALOG_ACTIVATION_WIN32;
}

static HRESULT bounded_string_length( const WCHAR *string, BOOL allow_empty,
                                      UINT32 *chars )
{
    UINT32 length;

    if (!string) return E_INVALIDARG;
    for (length = 0; length <= APPX_CATALOG_MAX_STRING_CHARS; length++)
        if (!string[length]) break;
    if (length > APPX_CATALOG_MAX_STRING_CHARS)
        return malformed_catalog();
    if (!allow_empty && !length) return malformed_catalog();
    *chars = length + 1;
    return S_OK;
}

static HRESULT copy_catalog_string( const WCHAR *source, BOOL allow_empty,
                                    WCHAR **destination )
{
    UINT32 chars;
    HRESULT hr;

    *destination = NULL;
    if (FAILED(hr = bounded_string_length( source, allow_empty, &chars )))
        return hr;
    if (!(*destination = HeapAlloc( GetProcessHeap(), 0,
                                    chars * sizeof(**destination) )))
        return E_OUTOFMEMORY;
    memcpy( *destination, source, chars * sizeof(**destination) );
    return S_OK;
}

static HRESULT validate_catalog_string_max( const WCHAR *string,
                                            BOOL allow_empty, UINT32 maximum )
{
    UINT32 chars;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length( string, allow_empty, &chars )))
        return hr;
    return chars <= maximum + 1 ? S_OK : malformed_catalog();
}

static HRESULT validate_payload_path( const WCHAR *path )
{
    BYTE *utf8 = NULL, *escaped = NULL;
    WCHAR *canonical = NULL;
    UINT32 chars, capacity = 0, escaped_size, i, j, percent_count = 0;
    int utf8_size;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length( path, FALSE, &chars )))
        return hr;
    if (chars > WINE_APPX_MAX_PATH_CHARS) return malformed_catalog();

    utf8_size = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS,
                                    path, chars - 1, NULL, 0, NULL, NULL );
    if (!utf8_size || utf8_size > WINE_APPX_MAX_ENTRY_NAME_BYTES)
        return malformed_catalog();
    if (!(utf8 = HeapAlloc( GetProcessHeap(), 0, utf8_size )))
        return E_OUTOFMEMORY;
    if (WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS,
                             path, chars - 1, (char *)utf8, utf8_size,
                             NULL, NULL ) != utf8_size)
    {
        hr = malformed_catalog();
        goto done;
    }
    for (i = 0; i < utf8_size; i++)
        if (utf8[i] == '%') percent_count++;
    if (percent_count > (WINE_APPX_MAX_ENTRY_NAME_BYTES - utf8_size) / 2)
    {
        hr = malformed_catalog();
        goto done;
    }
    escaped_size = utf8_size + percent_count * 2;
    if (!(escaped = HeapAlloc( GetProcessHeap(), 0, escaped_size )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = j = 0; i < utf8_size; i++)
    {
        if (utf8[i] == '%')
        {
            escaped[j++] = '%';
            escaped[j++] = '2';
            escaped[j++] = '5';
        }
        else
            escaped[j++] = utf8[i] == '\\' ? '/' : utf8[i];
    }

    hr = wine_appx_validate_archive_path( escaped, escaped_size, 0,
                                          &capacity, NULL );
    if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) ||
        capacity != chars)
    {
        if (hr != E_OUTOFMEMORY) hr = malformed_catalog();
        goto done;
    }
    if (!(canonical = HeapAlloc( GetProcessHeap(), 0,
                                 capacity * sizeof(*canonical) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    hr = wine_appx_validate_archive_path( escaped, escaped_size, 0,
                                          &capacity, canonical );
    if (FAILED(hr) || capacity != chars || lstrcmpW( path, canonical ))
        hr = hr == E_OUTOFMEMORY ? hr : malformed_catalog();

done:
    HeapFree( GetProcessHeap(), 0, canonical );
    HeapFree( GetProcessHeap(), 0, escaped );
    HeapFree( GetProcessHeap(), 0, utf8 );
    return hr;
}

static HRESULT validate_optional_payload_path( const WCHAR *path,
                                               UINT32 maximum )
{
    HRESULT hr;

    if (FAILED(hr = validate_catalog_string_max( path, TRUE, maximum )))
        return hr;
    return path[0] ? validate_payload_path( path ) : S_OK;
}

static void free_catalog_package( struct catalog_package *package )
{
    UINT32 i;

    for (i = 0; i < PACKAGE_STRING_COUNT; i++)
        HeapFree( GetProcessHeap(), 0, package->strings[i] );
    if (package->application_storage)
    {
        for (i = 0; i < package->view.application_count; i++)
        {
            HeapFree( GetProcessHeap(), 0, package->application_storage[i].strings[0] );
            HeapFree( GetProcessHeap(), 0, package->application_storage[i].strings[1] );
            HeapFree( GetProcessHeap(), 0, package->application_storage[i].strings[2] );
            HeapFree( GetProcessHeap(), 0, package->application_storage[i].strings[3] );
            HeapFree( GetProcessHeap(), 0, package->application_storage[i].strings[4] );
        }
    }
    if (package->dependency_storage)
    {
        for (i = 0; i < package->view.dependency_count; i++)
        {
            HeapFree( GetProcessHeap(), 0, package->dependency_storage[i].strings[0] );
            HeapFree( GetProcessHeap(), 0, package->dependency_storage[i].strings[1] );
        }
    }
    HeapFree( GetProcessHeap(), 0, package->application_storage );
    HeapFree( GetProcessHeap(), 0, package->applications );
    HeapFree( GetProcessHeap(), 0, package->dependency_storage );
    HeapFree( GetProcessHeap(), 0, package->dependencies );
    memset( package, 0, sizeof(*package) );
}

void WINAPI appx_catalog_snapshot_free( APPX_CATALOG_SNAPSHOT *snapshot )
{
    UINT32 i;

    if (!snapshot) return;
    for (i = 0; i < snapshot->count; i++)
        free_catalog_package( snapshot->packages + i );
    HeapFree( GetProcessHeap(), 0, snapshot->packages );
    memset( snapshot, 0, sizeof(*snapshot) );
    HeapFree( GetProcessHeap(), 0, snapshot );
}

static HRESULT copy_catalog_package( struct catalog_package *destination,
                                     const struct appx_catalog_package *source )
{
    UINT32 i;
    HRESULT hr;

    memset( destination, 0, sizeof(*destination) );
    if ((source->flags & ~APPX_CATALOG_PACKAGE_KNOWN_FLAGS) ||
        !is_valid_architecture( source->architecture ) ||
        source->application_count > APPX_CATALOG_MAX_APPLICATIONS_PER_PACKAGE ||
        source->dependency_count > APPX_CATALOG_MAX_DEPENDENCIES_PER_PACKAGE ||
        (source->application_count && !source->applications) ||
        (source->dependency_count && !source->dependencies))
        return malformed_catalog();

    destination->view.version = source->version;
    destination->view.architecture = source->architecture;
    destination->view.flags = source->flags;
    destination->view.application_count = source->application_count;
    destination->view.dependency_count = source->dependency_count;
    memcpy( destination->view.content_id, source->content_id,
            sizeof(destination->view.content_id) );

    if (FAILED(hr = copy_catalog_string( source->name, FALSE,
                                         &destination->strings[PACKAGE_STRING_NAME] )) ||
        FAILED(hr = copy_catalog_string( source->publisher, FALSE,
                                         &destination->strings[PACKAGE_STRING_PUBLISHER] )) ||
        FAILED(hr = copy_catalog_string( source->resource_id, TRUE,
                                         &destination->strings[PACKAGE_STRING_RESOURCE_ID] )) ||
        FAILED(hr = copy_catalog_string( source->publisher_id, FALSE,
                                         &destination->strings[PACKAGE_STRING_PUBLISHER_ID] )) ||
        FAILED(hr = copy_catalog_string( source->full_name, FALSE,
                                         &destination->strings[PACKAGE_STRING_FULL_NAME] )) ||
        FAILED(hr = copy_catalog_string( source->family_name, FALSE,
                                         &destination->strings[PACKAGE_STRING_FAMILY_NAME] )) ||
        FAILED(hr = copy_catalog_string( source->payload_path, FALSE,
                                         &destination->strings[PACKAGE_STRING_PAYLOAD_PATH] )))
        goto failed;

    if (FAILED(hr = validate_payload_path(
            destination->strings[PACKAGE_STRING_PAYLOAD_PATH] )))
        goto failed;

    destination->view.name = destination->strings[PACKAGE_STRING_NAME];
    destination->view.publisher = destination->strings[PACKAGE_STRING_PUBLISHER];
    destination->view.resource_id = destination->strings[PACKAGE_STRING_RESOURCE_ID];
    destination->view.publisher_id = destination->strings[PACKAGE_STRING_PUBLISHER_ID];
    destination->view.full_name = destination->strings[PACKAGE_STRING_FULL_NAME];
    destination->view.family_name = destination->strings[PACKAGE_STRING_FAMILY_NAME];
    destination->view.payload_path = destination->strings[PACKAGE_STRING_PAYLOAD_PATH];

    if (source->application_count)
    {
        destination->applications = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
            source->application_count * sizeof(*destination->applications) );
        destination->application_storage = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
            source->application_count * sizeof(*destination->application_storage) );
        if (!destination->applications || !destination->application_storage)
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
        destination->view.applications = destination->applications;

        for (i = 0; i < source->application_count; i++)
        {
            const struct appx_catalog_application *application = source->applications + i;

            if (!is_valid_activation_kind( application->activation_kind ))
            {
                hr = malformed_catalog();
                goto failed;
            }
            destination->applications[i].activation_kind = application->activation_kind;
            if (FAILED(hr = copy_catalog_string( application->id, FALSE,
                    &destination->application_storage[i].strings[0] )) ||
                FAILED(hr = copy_catalog_string( application->executable, FALSE,
                    &destination->application_storage[i].strings[1] )) ||
                FAILED(hr = copy_catalog_string( application->entry_point, TRUE,
                    &destination->application_storage[i].strings[2] )) ||
                FAILED(hr = copy_catalog_string(
                    application->parameters ? application->parameters : L"",
                    TRUE, &destination->application_storage[i].strings[3] )) ||
                FAILED(hr = copy_catalog_string(
                    application->current_directory_path ?
                    application->current_directory_path : L"", TRUE,
                    &destination->application_storage[i].strings[4] )))
                goto failed;
            if (FAILED(hr = validate_payload_path(
                    destination->application_storage[i].strings[1] )) ||
                FAILED(hr = validate_catalog_string_max(
                    destination->application_storage[i].strings[3],
                    TRUE, 1024 )) ||
                FAILED(hr = validate_optional_payload_path(
                    destination->application_storage[i].strings[4], 256 )))
                goto failed;
            destination->applications[i].id =
                destination->application_storage[i].strings[0];
            destination->applications[i].executable =
                destination->application_storage[i].strings[1];
            destination->applications[i].entry_point =
                destination->application_storage[i].strings[2];
            destination->applications[i].parameters =
                destination->application_storage[i].strings[3];
            destination->applications[i].current_directory_path =
                destination->application_storage[i].strings[4];
        }
    }

    if (source->dependency_count)
    {
        destination->dependencies = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
            source->dependency_count * sizeof(*destination->dependencies) );
        destination->dependency_storage = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
            source->dependency_count * sizeof(*destination->dependency_storage) );
        if (!destination->dependencies || !destination->dependency_storage)
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
        destination->view.dependencies = destination->dependencies;

        for (i = 0; i < source->dependency_count; i++)
        {
            const struct appx_catalog_dependency *dependency = source->dependencies + i;

            destination->dependencies[i].min_version = dependency->min_version;
            if (FAILED(hr = copy_catalog_string( dependency->name, FALSE,
                    &destination->dependency_storage[i].strings[0] )) ||
                FAILED(hr = copy_catalog_string( dependency->publisher, FALSE,
                    &destination->dependency_storage[i].strings[1] )))
                goto failed;
            destination->dependencies[i].name =
                destination->dependency_storage[i].strings[0];
            destination->dependencies[i].publisher =
                destination->dependency_storage[i].strings[1];
        }
    }
    return S_OK;

failed:
    free_catalog_package( destination );
    return hr;
}

struct active_package_index
{
    const struct catalog_package *package;
};

static INT compare_active_family_version( const void *left, const void *right )
{
    const struct active_package_index *left_index = left;
    const struct active_package_index *right_index = right;
    const struct appx_catalog_version *left_version =
        &left_index->package->view.version;
    const struct appx_catalog_version *right_version =
        &right_index->package->view.version;
    INT result;

    if ((result = compare_string_canonical( left_index->package->view.family_name,
                                            right_index->package->view.family_name )))
        return result;
    if (left_version->major != right_version->major)
        return left_version->major < right_version->major ? -1 : 1;
    if (left_version->minor != right_version->minor)
        return left_version->minor < right_version->minor ? -1 : 1;
    if (left_version->build != right_version->build)
        return left_version->build < right_version->build ? -1 : 1;
    if (left_version->revision != right_version->revision)
        return left_version->revision < right_version->revision ? -1 : 1;
    return 0;
}

static HRESULT validate_catalog_uniqueness( const APPX_CATALOG_SNAPSHOT *snapshot )
{
    struct active_package_index *active = NULL;
    UINT32 active_count = 0, i;
    HRESULT hr = S_OK;

    for (i = 1; i < snapshot->count; i++)
    {
        if (!compare_string_ci( snapshot->packages[i - 1].view.full_name,
                                snapshot->packages[i].view.full_name ))
            return malformed_catalog();
    }

    if (!(active = HeapAlloc( GetProcessHeap(), 0,
                              snapshot->count * sizeof(*active) )))
        return snapshot->count ? E_OUTOFMEMORY : S_OK;
    for (i = 0; i < snapshot->count; i++)
        if (snapshot->packages[i].view.flags & APPX_CATALOG_PACKAGE_ACTIVE)
            active[active_count++].package = snapshot->packages + i;
    qsort( active, active_count, sizeof(*active), compare_active_family_version );
    for (i = 1; i < active_count; i++)
    {
        if (!compare_string_ci( active[i - 1].package->view.family_name,
                                active[i].package->view.family_name ) &&
            versions_equal( &active[i - 1].package->view.version,
                            &active[i].package->view.version ))
        {
            hr = malformed_catalog();
            break;
        }
    }
    HeapFree( GetProcessHeap(), 0, active );
    return hr;
}

HRESULT WINAPI appx_catalog_snapshot_create(
    UINT64 epoch, const struct appx_catalog_package *packages, UINT32 count,
    APPX_CATALOG_SNAPSHOT **snapshot )
{
    APPX_CATALOG_SNAPSHOT *object = NULL;
    UINT32 i;
    HRESULT hr = S_OK;

    TRACE( "epoch %s, packages %p, count %u, snapshot %p.\n",
           wine_dbgstr_longlong(epoch), packages, count, snapshot );

    if (!snapshot) return E_INVALIDARG;
    *snapshot = NULL;
    if ((count && !packages) || count > APPX_CATALOG_MAX_PACKAGES)
        return E_INVALIDARG;

    if (!(object = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object) )))
        return E_OUTOFMEMORY;
    object->epoch = epoch;
    object->count = count;
    if (count)
    {
        object->packages = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      count * sizeof(*object->packages) );
        if (!object->packages)
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
    }

    for (i = 0; i < count; i++)
        if (FAILED(hr = copy_catalog_package( object->packages + i, packages + i )))
            goto failed;
    if (count > 1)
        qsort( object->packages, count, sizeof(*object->packages),
               compare_package_full_name );
    if (FAILED(hr = validate_catalog_uniqueness( object )))
        goto failed;

    *snapshot = object;
    return S_OK;

failed:
    appx_catalog_snapshot_free( object );
    return hr;
}

HRESULT WINAPI appx_catalog_snapshot_deep_copy(
    const APPX_CATALOG_SNAPSHOT *source, APPX_CATALOG_SNAPSHOT **snapshot )
{
    struct appx_catalog_package *packages = NULL;
    UINT32 i;
    HRESULT hr;

    if (!snapshot) return E_INVALIDARG;
    *snapshot = NULL;
    if (!source) return E_INVALIDARG;
    if (!(packages = HeapAlloc( GetProcessHeap(), 0,
                                source->count * sizeof(*packages) )))
        return source->count ? E_OUTOFMEMORY : appx_catalog_snapshot_create(
            source->epoch, NULL, 0, snapshot );
    for (i = 0; i < source->count; i++)
        packages[i] = source->packages[i].view;
    hr = appx_catalog_snapshot_create( source->epoch, packages, source->count, snapshot );
    HeapFree( GetProcessHeap(), 0, packages );
    return hr;
}

UINT64 WINAPI appx_catalog_snapshot_get_epoch( const APPX_CATALOG_SNAPSHOT *snapshot )
{
    return snapshot ? snapshot->epoch : 0;
}

UINT32 WINAPI appx_catalog_snapshot_get_package_count( const APPX_CATALOG_SNAPSHOT *snapshot )
{
    return snapshot ? snapshot->count : 0;
}

const struct appx_catalog_package * WINAPI appx_catalog_snapshot_get_package(
    const APPX_CATALOG_SNAPSHOT *snapshot, UINT32 index )
{
    if (!snapshot || index >= snapshot->count) return NULL;
    return &snapshot->packages[index].view;
}

static HRESULT buffer_reserve( struct catalog_buffer *buffer, UINT32 size )
{
    BYTE *data;
    UINT32 capacity;

    if (size <= buffer->capacity - buffer->size) return S_OK;
    capacity = buffer->capacity ? buffer->capacity : 4096;
    while (capacity - buffer->size < size)
    {
        if (capacity > APPX_CATALOG_MAX_FILE_SIZE / 2)
            return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
        capacity *= 2;
    }
    if (capacity > APPX_CATALOG_MAX_FILE_SIZE)
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    if (!(data = HeapReAlloc( GetProcessHeap(), 0, buffer->data, capacity )))
    {
        if (buffer->data) return E_OUTOFMEMORY;
        if (!(data = HeapAlloc( GetProcessHeap(), 0, capacity )))
            return E_OUTOFMEMORY;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return S_OK;
}

static HRESULT buffer_append_zero( struct catalog_buffer *buffer, UINT32 size, BYTE **data )
{
    HRESULT hr;

    if (buffer->size > APPX_CATALOG_MAX_FILE_SIZE - size)
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    if (FAILED(hr = buffer_reserve( buffer, size ))) return hr;
    if (data) *data = buffer->data + buffer->size;
    memset( buffer->data + buffer->size, 0, size );
    buffer->size += size;
    return S_OK;
}

static HRESULT buffer_append_data( struct catalog_buffer *buffer, const void *data,
                                   UINT32 size )
{
    BYTE *destination;
    HRESULT hr;

    if (FAILED(hr = buffer_append_zero( buffer, size, &destination ))) return hr;
    memcpy( destination, data, size );
    return S_OK;
}

static HRESULT append_catalog_string( struct catalog_buffer *record, const WCHAR *string,
                                      UINT32 *offset, UINT32 *chars )
{
    UINT32 length, bytes, i;
    BYTE *data;
    HRESULT hr;

    length = lstrlenW( string ) + 1;
    if (!multiply_uint32( length, sizeof(WCHAR), &bytes ))
        return malformed_catalog();
    *offset = record->size;
    *chars = length;
    if (FAILED(hr = buffer_append_zero( record, bytes, &data ))) return hr;
    for (i = 0; i < length; i++)
        write_uint16( data + i * sizeof(WCHAR), string[i] );
    return S_OK;
}

static void write_string_ref( BYTE *record, UINT32 offset, UINT32 chars )
{
    write_uint32( record, offset );
    write_uint32( record + 4, chars );
}

static HRESULT serialize_package( const struct catalog_package *package,
                                  struct catalog_buffer *output )
{
    struct catalog_buffer record = {0};
    UINT32 apps_offset, deps_offset, strings_offset, strings_size, offset, chars;
    UINT32 i, array_size;
    HRESULT hr;

    if (FAILED(hr = buffer_append_zero( &record, CATALOG_PACKAGE_FIXED_SIZE, NULL )))
        goto done;
    apps_offset = record.size;
    if (!multiply_uint32( package->view.application_count,
                          CATALOG_APPLICATION_RECORD_SIZE, &array_size ) ||
        FAILED(hr = buffer_append_zero( &record, array_size, NULL )))
    {
        if (SUCCEEDED(hr)) hr = malformed_catalog();
        goto done;
    }
    deps_offset = record.size;
    if (!multiply_uint32( package->view.dependency_count,
                          CATALOG_DEPENDENCY_RECORD_SIZE, &array_size ) ||
        FAILED(hr = buffer_append_zero( &record, array_size, NULL )))
    {
        if (SUCCEEDED(hr)) hr = malformed_catalog();
        goto done;
    }
    strings_offset = record.size;

    for (i = 0; i < PACKAGE_STRING_COUNT; i++)
    {
        if (FAILED(hr = append_catalog_string( &record, package->strings[i],
                                               &offset, &chars )))
            goto done;
        write_string_ref( record.data + CATALOG_PACKAGE_STRING_REFS_OFFSET + i * 8,
                          offset, chars );
    }

    for (i = 0; i < package->view.application_count; i++)
    {
        UINT32 application_offset =
            apps_offset + i * CATALOG_APPLICATION_RECORD_SIZE;

        write_uint32( record.data + application_offset +
                      CATALOG_APPLICATION_KIND_OFFSET,
                      package->applications[i].activation_kind );
        if (FAILED(hr = append_catalog_string( &record, package->applications[i].id,
                                               &offset, &chars )))
            goto done;
        write_string_ref( record.data + application_offset +
                          CATALOG_APPLICATION_ID_REF_OFFSET,
                          offset, chars );
        if (FAILED(hr = append_catalog_string( &record,
                                               package->applications[i].executable,
                                               &offset, &chars )))
            goto done;
        write_string_ref( record.data + application_offset +
                          CATALOG_APPLICATION_EXECUTABLE_REF_OFFSET,
                          offset, chars );
        if (FAILED(hr = append_catalog_string( &record,
                                               package->applications[i].entry_point,
                                               &offset, &chars )))
            goto done;
        write_string_ref( record.data + application_offset +
                          CATALOG_APPLICATION_ENTRY_POINT_REF_OFFSET,
                          offset, chars );
        if (FAILED(hr = append_catalog_string( &record,
                                               package->applications[i].parameters,
                                               &offset, &chars )))
            goto done;
        write_string_ref( record.data + application_offset +
                          CATALOG_APPLICATION_PARAMETERS_REF_OFFSET,
                          offset, chars );
        if (FAILED(hr = append_catalog_string( &record,
                                               package->applications[i].current_directory_path,
                                               &offset, &chars )))
            goto done;
        write_string_ref( record.data + application_offset +
                          CATALOG_APPLICATION_CURRENT_DIR_REF_OFFSET,
                          offset, chars );
    }

    for (i = 0; i < package->view.dependency_count; i++)
    {
        UINT32 dependency_offset =
            deps_offset + i * CATALOG_DEPENDENCY_RECORD_SIZE;

        write_uint64( record.data + dependency_offset +
                      CATALOG_DEPENDENCY_VERSION_OFFSET,
                      pack_version( &package->dependencies[i].min_version ) );
        if (FAILED(hr = append_catalog_string( &record, package->dependencies[i].name,
                                               &offset, &chars )))
            goto done;
        write_string_ref( record.data + dependency_offset +
                          CATALOG_DEPENDENCY_NAME_REF_OFFSET,
                          offset, chars );
        if (FAILED(hr = append_catalog_string( &record,
                                               package->dependencies[i].publisher,
                                               &offset, &chars )))
            goto done;
        write_string_ref( record.data + dependency_offset +
                          CATALOG_DEPENDENCY_PUBLISHER_REF_OFFSET,
                          offset, chars );
    }

    strings_size = record.size - strings_offset;
    write_uint32( record.data + CATALOG_PACKAGE_RECORD_SIZE_OFFSET, record.size );
    write_uint32( record.data + CATALOG_PACKAGE_FLAGS_OFFSET, package->view.flags );
    write_uint32( record.data + CATALOG_PACKAGE_ARCHITECTURE_OFFSET,
                  package->view.architecture );
    write_uint32( record.data + CATALOG_PACKAGE_APPLICATION_COUNT_OFFSET,
                  package->view.application_count );
    write_uint32( record.data + CATALOG_PACKAGE_DEPENDENCY_COUNT_OFFSET,
                  package->view.dependency_count );
    write_uint32( record.data + CATALOG_PACKAGE_APPLICATIONS_OFFSET, apps_offset );
    write_uint32( record.data + CATALOG_PACKAGE_DEPENDENCIES_OFFSET, deps_offset );
    write_uint32( record.data + CATALOG_PACKAGE_STRINGS_OFFSET, strings_offset );
    write_uint32( record.data + CATALOG_PACKAGE_STRINGS_SIZE_OFFSET, strings_size );
    write_uint64( record.data + CATALOG_PACKAGE_VERSION_OFFSET,
                  pack_version( &package->view.version ) );
    memcpy( record.data + CATALOG_PACKAGE_CONTENT_ID_OFFSET,
            package->view.content_id,
            APPX_CATALOG_CONTENT_ID_SIZE );

    hr = buffer_append_data( output, record.data, record.size );

done:
    HeapFree( GetProcessHeap(), 0, record.data );
    return hr;
}

struct sha256_context
{
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
};

static HRESULT sha256_init( struct sha256_context *context )
{
    NTSTATUS status;

    memset( context, 0, sizeof(*context) );
    if ((status = BCryptOpenAlgorithmProvider( &context->algorithm,
                                               BCRYPT_SHA256_ALGORITHM, NULL, 0 )))
        return HRESULT_FROM_NT( status );
    if ((status = BCryptCreateHash( context->algorithm, &context->hash,
                                    NULL, 0, NULL, 0, 0 )))
    {
        BCryptCloseAlgorithmProvider( context->algorithm, 0 );
        context->algorithm = NULL;
        return HRESULT_FROM_NT( status );
    }
    return S_OK;
}

static void sha256_destroy( struct sha256_context *context )
{
    if (context->hash) BCryptDestroyHash( context->hash );
    if (context->algorithm) BCryptCloseAlgorithmProvider( context->algorithm, 0 );
}

static HRESULT sha256_update( struct sha256_context *context, const void *data, UINT32 size )
{
    NTSTATUS status;

    if (size && (status = BCryptHashData( context->hash, (BYTE *)data, size, 0 )))
        return HRESULT_FROM_NT( status );
    return S_OK;
}

static HRESULT sha256_finish( struct sha256_context *context, BYTE digest[CATALOG_DIGEST_SIZE] )
{
    NTSTATUS status;

    if ((status = BCryptFinishHash( context->hash, digest, CATALOG_DIGEST_SIZE, 0 )))
        return HRESULT_FROM_NT( status );
    return S_OK;
}

static HRESULT hash_catalog_bytes( const BYTE *data, UINT32 size,
                                   BYTE digest[CATALOG_DIGEST_SIZE] )
{
    static const BYTE zero_digest[CATALOG_DIGEST_SIZE] = {0};
    struct sha256_context hash;
    HRESULT hr;

    if (size < CATALOG_HEADER_DIGEST_OFFSET + CATALOG_DIGEST_SIZE)
        return malformed_catalog();
    if (FAILED(hr = sha256_init( &hash ))) return hr;
    if (SUCCEEDED(hr))
        hr = sha256_update( &hash, data, CATALOG_HEADER_DIGEST_OFFSET );
    if (SUCCEEDED(hr))
        hr = sha256_update( &hash, zero_digest, sizeof(zero_digest) );
    if (SUCCEEDED(hr))
        hr = sha256_update( &hash,
                            data + CATALOG_HEADER_DIGEST_OFFSET + CATALOG_DIGEST_SIZE,
                            size - CATALOG_HEADER_DIGEST_OFFSET -
                            CATALOG_DIGEST_SIZE );
    if (SUCCEEDED(hr))
        hr = sha256_finish( &hash, digest );
    sha256_destroy( &hash );
    return hr;
}

static BOOL equal_digest( const BYTE *left, const BYTE *right )
{
    BYTE diff = 0;
    UINT32 i;

    for (i = 0; i < CATALOG_DIGEST_SIZE; i++)
        diff |= left[i] ^ right[i];
    return !diff;
}

static HRESULT serialize_catalog( const APPX_CATALOG_SNAPSHOT *snapshot,
                                  struct catalog_buffer *output )
{
    BYTE digest[CATALOG_DIGEST_SIZE], *header;
    UINT32 i;
    HRESULT hr;

    memset( output, 0, sizeof(*output) );
    if (FAILED(hr = buffer_append_zero( output, APPX_CATALOG_HEADER_SIZE, NULL )))
        return hr;
    for (i = 0; i < snapshot->count; i++)
        if (FAILED(hr = serialize_package( snapshot->packages + i, output )))
            goto failed;

    header = output->data;
    memcpy( header, catalog_magic, sizeof(catalog_magic) );
    write_uint32( header + CATALOG_HEADER_VERSION_OFFSET, APPX_CATALOG_VERSION );
    write_uint32( header + CATALOG_HEADER_SIZE_OFFSET, APPX_CATALOG_HEADER_SIZE );
    write_uint64( header + CATALOG_HEADER_FILE_SIZE_OFFSET, output->size );
    write_uint64( header + CATALOG_HEADER_EPOCH_OFFSET, snapshot->epoch );
    write_uint32( header + CATALOG_HEADER_PACKAGE_COUNT_OFFSET, snapshot->count );
    write_uint64( header + CATALOG_HEADER_RECORDS_OFFSET, APPX_CATALOG_HEADER_SIZE );

    if (FAILED(hr = hash_catalog_bytes( output->data, output->size, digest )))
        goto failed;
    memcpy( header + CATALOG_HEADER_DIGEST_OFFSET, digest, sizeof(digest) );
    return S_OK;

failed:
    HeapFree( GetProcessHeap(), 0, output->data );
    memset( output, 0, sizeof(*output) );
    return hr;
}

static BOOL read_string_ref( const BYTE *record, UINT32 offset,
                             UINT32 *string_offset, UINT32 *chars )
{
    *string_offset = read_uint32( record + offset );
    *chars = read_uint32( record + offset + 4 );
    return TRUE;
}

static HRESULT read_next_catalog_string( const BYTE *record, UINT32 record_size,
                                         UINT32 strings_offset, UINT32 strings_size,
                                         UINT32 ref_offset, BOOL allow_empty,
                                         UINT32 *expected_offset,
                                         WCHAR **string )
{
    UINT32 offset, chars, bytes, end, i;
    WCHAR *copy;

    *string = NULL;
    read_string_ref( record, ref_offset, &offset, &chars );
    if (!chars || chars > APPX_CATALOG_MAX_STRING_CHARS + 1 ||
        offset != *expected_offset ||
        !multiply_uint32( chars, sizeof(WCHAR), &bytes ) ||
        !add_uint32( offset, bytes, &end ) ||
        offset < strings_offset ||
        end > strings_offset + strings_size ||
        end > record_size)
        return malformed_catalog();
    if (!allow_empty && chars == 1) return malformed_catalog();
    if (!(copy = HeapAlloc( GetProcessHeap(), 0, chars * sizeof(*copy) )))
        return E_OUTOFMEMORY;
    for (i = 0; i < chars; i++)
        copy[i] = read_uint16( record + offset + i * sizeof(WCHAR) );
    if (copy[chars - 1])
    {
        HeapFree( GetProcessHeap(), 0, copy );
        return malformed_catalog();
    }
    for (i = 0; i + 1 < chars; i++)
    {
        if (!copy[i])
        {
            HeapFree( GetProcessHeap(), 0, copy );
            return malformed_catalog();
        }
    }
    *expected_offset = end;
    *string = copy;
    return S_OK;
}

static BOOL range_inside_record( UINT32 offset, UINT32 count, UINT32 size,
                                 UINT32 record_size, UINT32 *end )
{
    UINT32 bytes;

    return multiply_uint32( count, size, &bytes ) &&
           add_uint32( offset, bytes, end ) &&
           offset <= record_size && *end <= record_size;
}

static HRESULT parse_catalog_package( const BYTE *record, UINT32 record_size,
                                      struct catalog_package *package )
{
    UINT32 app_count, dep_count, apps_offset, deps_offset, strings_offset;
    UINT32 strings_size, expected_offset, apps_end, deps_end, strings_end;
    UINT32 i;
    HRESULT hr;

    memset( package, 0, sizeof(*package) );
    if (record_size < CATALOG_PACKAGE_FIXED_SIZE ||
        read_uint32( record + CATALOG_PACKAGE_RECORD_SIZE_OFFSET ) != record_size)
        return malformed_catalog();

    package->view.flags = read_uint32( record + CATALOG_PACKAGE_FLAGS_OFFSET );
    package->view.architecture =
        read_uint32( record + CATALOG_PACKAGE_ARCHITECTURE_OFFSET );
    app_count = read_uint32( record + CATALOG_PACKAGE_APPLICATION_COUNT_OFFSET );
    dep_count = read_uint32( record + CATALOG_PACKAGE_DEPENDENCY_COUNT_OFFSET );
    apps_offset = read_uint32( record + CATALOG_PACKAGE_APPLICATIONS_OFFSET );
    deps_offset = read_uint32( record + CATALOG_PACKAGE_DEPENDENCIES_OFFSET );
    strings_offset = read_uint32( record + CATALOG_PACKAGE_STRINGS_OFFSET );
    strings_size = read_uint32( record + CATALOG_PACKAGE_STRINGS_SIZE_OFFSET );
    package->view.version =
        unpack_version( read_uint64( record + CATALOG_PACKAGE_VERSION_OFFSET ) );
    memcpy( package->view.content_id, record + CATALOG_PACKAGE_CONTENT_ID_OFFSET,
            sizeof(package->view.content_id) );

    if ((package->view.flags & ~APPX_CATALOG_PACKAGE_KNOWN_FLAGS) ||
        !is_valid_architecture( package->view.architecture ) ||
        app_count > APPX_CATALOG_MAX_APPLICATIONS_PER_PACKAGE ||
        dep_count > APPX_CATALOG_MAX_DEPENDENCIES_PER_PACKAGE ||
        apps_offset != CATALOG_PACKAGE_FIXED_SIZE ||
        !range_inside_record( apps_offset, app_count, CATALOG_APPLICATION_RECORD_SIZE,
                              record_size, &apps_end ) ||
        deps_offset != apps_end ||
        !range_inside_record( deps_offset, dep_count, CATALOG_DEPENDENCY_RECORD_SIZE,
                              record_size, &deps_end ) ||
        strings_offset != deps_end ||
        !add_uint32( strings_offset, strings_size, &strings_end ) ||
        strings_end != record_size)
        return malformed_catalog();

    package->view.application_count = app_count;
    package->view.dependency_count = dep_count;
    if (app_count)
    {
        package->applications = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                           app_count * sizeof(*package->applications) );
        package->application_storage = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
            app_count * sizeof(*package->application_storage) );
        if (!package->applications || !package->application_storage)
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
        package->view.applications = package->applications;
    }
    if (dep_count)
    {
        package->dependencies = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                           dep_count * sizeof(*package->dependencies) );
        package->dependency_storage = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
            dep_count * sizeof(*package->dependency_storage) );
        if (!package->dependencies || !package->dependency_storage)
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
        package->view.dependencies = package->dependencies;
    }

    expected_offset = strings_offset;
    for (i = 0; i < PACKAGE_STRING_COUNT; i++)
    {
        BOOL allow_empty = i == PACKAGE_STRING_RESOURCE_ID;

        if (FAILED(hr = read_next_catalog_string( record, record_size,
                strings_offset, strings_size,
                CATALOG_PACKAGE_STRING_REFS_OFFSET + i * 8, allow_empty,
                &expected_offset, &package->strings[i] )))
            goto failed;
    }
    package->view.name = package->strings[PACKAGE_STRING_NAME];
    package->view.publisher = package->strings[PACKAGE_STRING_PUBLISHER];
    package->view.resource_id = package->strings[PACKAGE_STRING_RESOURCE_ID];
    package->view.publisher_id = package->strings[PACKAGE_STRING_PUBLISHER_ID];
    package->view.full_name = package->strings[PACKAGE_STRING_FULL_NAME];
    package->view.family_name = package->strings[PACKAGE_STRING_FAMILY_NAME];
    package->view.payload_path = package->strings[PACKAGE_STRING_PAYLOAD_PATH];
    if (FAILED(hr = validate_payload_path( package->view.payload_path )))
        goto failed;

    for (i = 0; i < app_count; i++)
    {
        const BYTE *application = record + apps_offset + i * CATALOG_APPLICATION_RECORD_SIZE;

        package->applications[i].activation_kind =
            read_uint32( application + CATALOG_APPLICATION_KIND_OFFSET );
        if (!is_valid_activation_kind( package->applications[i].activation_kind ))
        {
            hr = malformed_catalog();
            goto failed;
        }
        if (FAILED(hr = read_next_catalog_string( record, record_size,
                strings_offset, strings_size,
                apps_offset + i * CATALOG_APPLICATION_RECORD_SIZE +
                CATALOG_APPLICATION_ID_REF_OFFSET, FALSE, &expected_offset,
                &package->application_storage[i].strings[0] )) ||
            FAILED(hr = read_next_catalog_string( record, record_size,
                strings_offset, strings_size,
                apps_offset + i * CATALOG_APPLICATION_RECORD_SIZE +
                CATALOG_APPLICATION_EXECUTABLE_REF_OFFSET, FALSE, &expected_offset,
                &package->application_storage[i].strings[1] )) ||
            FAILED(hr = read_next_catalog_string( record, record_size,
                strings_offset, strings_size,
                apps_offset + i * CATALOG_APPLICATION_RECORD_SIZE +
                CATALOG_APPLICATION_ENTRY_POINT_REF_OFFSET, TRUE, &expected_offset,
                &package->application_storage[i].strings[2] )) ||
            FAILED(hr = read_next_catalog_string( record, record_size,
                strings_offset, strings_size,
                apps_offset + i * CATALOG_APPLICATION_RECORD_SIZE +
                CATALOG_APPLICATION_PARAMETERS_REF_OFFSET, TRUE, &expected_offset,
                &package->application_storage[i].strings[3] )) ||
            FAILED(hr = read_next_catalog_string( record, record_size,
                strings_offset, strings_size,
                apps_offset + i * CATALOG_APPLICATION_RECORD_SIZE +
                CATALOG_APPLICATION_CURRENT_DIR_REF_OFFSET, TRUE, &expected_offset,
                &package->application_storage[i].strings[4] )))
            goto failed;
        package->applications[i].id = package->application_storage[i].strings[0];
        package->applications[i].executable = package->application_storage[i].strings[1];
        package->applications[i].entry_point =
            package->application_storage[i].strings[2];
        package->applications[i].parameters =
            package->application_storage[i].strings[3];
        package->applications[i].current_directory_path =
            package->application_storage[i].strings[4];
        if (FAILED(hr = validate_payload_path( package->applications[i].executable )) ||
            FAILED(hr = validate_catalog_string_max(
                package->applications[i].parameters, TRUE, 1024 )) ||
            FAILED(hr = validate_optional_payload_path(
                package->applications[i].current_directory_path, 256 )))
            goto failed;
    }

    for (i = 0; i < dep_count; i++)
    {
        const BYTE *dependency = record + deps_offset + i * CATALOG_DEPENDENCY_RECORD_SIZE;

        package->dependencies[i].min_version =
            unpack_version( read_uint64( dependency + CATALOG_DEPENDENCY_VERSION_OFFSET ) );
        if (FAILED(hr = read_next_catalog_string( record, record_size,
                strings_offset, strings_size,
                deps_offset + i * CATALOG_DEPENDENCY_RECORD_SIZE +
                CATALOG_DEPENDENCY_NAME_REF_OFFSET, FALSE, &expected_offset,
                &package->dependency_storage[i].strings[0] )) ||
            FAILED(hr = read_next_catalog_string( record, record_size,
                strings_offset, strings_size,
                deps_offset + i * CATALOG_DEPENDENCY_RECORD_SIZE +
                CATALOG_DEPENDENCY_PUBLISHER_REF_OFFSET, FALSE, &expected_offset,
                &package->dependency_storage[i].strings[1] )))
            goto failed;
        package->dependencies[i].name = package->dependency_storage[i].strings[0];
        package->dependencies[i].publisher =
            package->dependency_storage[i].strings[1];
    }

    if (expected_offset != strings_end)
    {
        hr = malformed_catalog();
        goto failed;
    }
    return S_OK;

failed:
    free_catalog_package( package );
    return hr;
}

static HRESULT parse_catalog( const BYTE *data, UINT32 size,
                              APPX_CATALOG_SNAPSHOT **snapshot )
{
    BYTE digest[CATALOG_DIGEST_SIZE];
    APPX_CATALOG_SNAPSHOT *object = NULL;
    UINT64 file_size, records_offset;
    UINT32 package_count, offset, version, i;
    HRESULT hr;

    *snapshot = NULL;
    if (size < APPX_CATALOG_HEADER_SIZE ||
        memcmp( data, catalog_magic, sizeof(catalog_magic) ))
        return malformed_catalog();
    version = read_uint32( data + CATALOG_HEADER_VERSION_OFFSET );
    if (version != APPX_CATALOG_VERSION)
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    if (read_uint32( data + CATALOG_HEADER_SIZE_OFFSET ) !=
            APPX_CATALOG_HEADER_SIZE)
        return malformed_catalog();
    file_size = read_uint64( data + CATALOG_HEADER_FILE_SIZE_OFFSET );
    if (file_size != size) return malformed_catalog();
    package_count = read_uint32( data + CATALOG_HEADER_PACKAGE_COUNT_OFFSET );
    records_offset = read_uint64( data + CATALOG_HEADER_RECORDS_OFFSET );
    if (package_count > APPX_CATALOG_MAX_PACKAGES ||
        records_offset != APPX_CATALOG_HEADER_SIZE)
        return malformed_catalog();
    for (i = 0; i < CATALOG_HEADER_RESERVED_SIZE; i++)
        if (data[CATALOG_HEADER_RESERVED_OFFSET + i])
            return malformed_catalog();
    if (FAILED(hr = hash_catalog_bytes( data, size, digest )))
        return hr;
    if (!equal_digest( digest, data + CATALOG_HEADER_DIGEST_OFFSET ))
        return APPX_E_DIGEST_MISMATCH;

    if (!(object = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object) )))
        return E_OUTOFMEMORY;
    object->epoch = read_uint64( data + CATALOG_HEADER_EPOCH_OFFSET );
    object->count = package_count;
    if (package_count)
    {
        object->packages = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
            package_count * sizeof(*object->packages) );
        if (!object->packages)
        {
            hr = E_OUTOFMEMORY;
            goto failed;
        }
    }

    offset = APPX_CATALOG_HEADER_SIZE;
    for (i = 0; i < package_count; i++)
    {
        UINT32 record_size;

        if (size - offset < CATALOG_PACKAGE_FIXED_SIZE)
        {
            hr = malformed_catalog();
            goto failed;
        }
        record_size = read_uint32( data + offset + CATALOG_PACKAGE_RECORD_SIZE_OFFSET );
        if (record_size > size - offset)
        {
            hr = malformed_catalog();
            goto failed;
        }
        if (FAILED(hr = parse_catalog_package( data + offset, record_size,
                                               object->packages + i )))
            goto failed;
        offset += record_size;
    }
    if (offset != size)
    {
        hr = malformed_catalog();
        goto failed;
    }
    if (FAILED(hr = validate_catalog_uniqueness( object )))
        goto failed;

    *snapshot = object;
    return S_OK;

failed:
    appx_catalog_snapshot_free( object );
    return hr;
}

static HRESULT check_file_attributes( HANDLE handle, BOOL directory )
{
    FILE_ATTRIBUTE_TAG_INFO tag_info;
    DWORD attributes;

    if (GetFileInformationByHandleEx( handle, FileAttributeTagInfo,
                                      &tag_info, sizeof(tag_info) ))
        attributes = tag_info.FileAttributes;
    else
    {
        BY_HANDLE_FILE_INFORMATION info;

        if (!GetFileInformationByHandle( handle, &info ))
            return win32_error( GetLastError() );
        attributes = info.dwFileAttributes;
    }

    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    if (directory)
    {
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
            return HRESULT_FROM_WIN32( ERROR_DIRECTORY );
    }
    else if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    return S_OK;
}

static HRESULT open_catalog_store( const WCHAR *store_root,
                                   struct catalog_store *store )
{
    UNICODE_STRING nt_path;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    HRESULT hr;

    store->root = INVALID_HANDLE_VALUE;
    if (!store_root || !store_root[0]) return E_INVALIDARG;
    status = RtlDosPathNameToNtPathName_U_WithStatus( store_root, &nt_path,
                                                      NULL, NULL );
    if (status) return ntstatus_error( status );
    InitializeObjectAttributes( &attributes, &nt_path, OBJ_CASE_INSENSITIVE,
                                NULL, NULL );
    status = NtCreateFile( &store->root,
                           FILE_LIST_DIRECTORY | FILE_ADD_FILE |
                           FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_OPEN, FILE_DIRECTORY_FILE |
                           FILE_SYNCHRONOUS_IO_NONALERT |
                           FILE_OPEN_REPARSE_POINT, NULL, 0 );
    RtlFreeUnicodeString( &nt_path );
    if (status) return ntstatus_error( status );
    if (FAILED(hr = check_file_attributes( store->root, TRUE )))
    {
        CloseHandle( store->root );
        store->root = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static void close_catalog_store( struct catalog_store *store )
{
    if (store->root != INVALID_HANDLE_VALUE) CloseHandle( store->root );
    store->root = INVALID_HANDLE_VALUE;
}

static HRESULT open_store_child( const struct catalog_store *store,
                                 const WCHAR *name, ACCESS_MASK access,
                                 ULONG sharing, ULONG disposition,
                                 ULONG options, HANDLE *handle )
{
    UNICODE_STRING child_name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    HRESULT hr;

    *handle = INVALID_HANDLE_VALUE;
    RtlInitUnicodeString( &child_name, name );
    InitializeObjectAttributes( &attributes, &child_name, OBJ_CASE_INSENSITIVE,
                                store->root, NULL );
    status = NtCreateFile( handle, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL, sharing,
                           disposition, options | FILE_NON_DIRECTORY_FILE |
                           FILE_SYNCHRONOUS_IO_NONALERT |
                           FILE_OPEN_REPARSE_POINT, NULL, 0 );
    if (status)
    {
        TRACE( "Opening store child %s failed, status %#lx.\n",
               debugstr_w(name), status );
        return ntstatus_error( status );
    }
    if (FAILED(hr = check_file_attributes( *handle, FALSE )))
    {
        CloseHandle( *handle );
        *handle = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static HRESULT acquire_catalog_lock( const struct catalog_store *store,
                                     DWORD timeout_ms, HANDLE cancel_event,
                                     struct catalog_lock *lock )
{
    ULONGLONG start = GetTickCount64();
    HRESULT hr;

    memset( lock, 0, sizeof(*lock) );
    lock->handle = INVALID_HANDLE_VALUE;
    if (FAILED(hr = open_store_child( store, APPX_CATALOG_LOCK_FILE_NAME,
                                      FILE_READ_DATA | FILE_WRITE_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      FILE_OPEN_IF, 0, &lock->handle )))
        goto failed;
    for (;;)
    {
        DWORD wait;

        if (cancel_event)
        {
            wait = WaitForSingleObject( cancel_event, 0 );
            if (wait == WAIT_OBJECT_0)
            {
                hr = HRESULT_FROM_WIN32( ERROR_CANCELLED );
                goto failed;
            }
            if (wait != WAIT_TIMEOUT)
            {
                hr = win32_error( GetLastError() );
                goto failed;
            }
        }
        if (LockFileEx(
                lock->handle,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0, 1, 0, &lock->overlapped ))
            break;
        if (GetLastError() != ERROR_LOCK_VIOLATION)
        {
            hr = win32_error( GetLastError() );
            TRACE( "Locking the catalog store failed, hr %#lx.\n", hr );
            goto failed;
        }
        if (timeout_ms != INFINITE)
        {
            ULONGLONG elapsed = GetTickCount64() - start;

            if (elapsed >= timeout_ms)
            {
                hr = HRESULT_FROM_WIN32( ERROR_TIMEOUT );
                TRACE( "Timed out locking the catalog store.\n" );
                goto failed;
            }
            wait = timeout_ms - elapsed;
            if (wait > CATALOG_LOCK_POLL_MS) wait = CATALOG_LOCK_POLL_MS;
        }
        else
            wait = CATALOG_LOCK_POLL_MS;
        if (cancel_event)
        {
            wait = WaitForSingleObject( cancel_event, wait );
            if (wait == WAIT_OBJECT_0)
            {
                hr = HRESULT_FROM_WIN32( ERROR_CANCELLED );
                goto failed;
            }
            if (wait != WAIT_TIMEOUT)
            {
                hr = win32_error( GetLastError() );
                goto failed;
            }
        }
        else
            Sleep( wait );
    }
    lock->locked = TRUE;
    return S_OK;

failed:
    if (lock->handle != INVALID_HANDLE_VALUE) CloseHandle( lock->handle );
    memset( lock, 0, sizeof(*lock) );
    lock->handle = INVALID_HANDLE_VALUE;
    return hr;
}

static void release_catalog_lock( struct catalog_lock *lock )
{
    if (lock->handle != INVALID_HANDLE_VALUE)
    {
        if (lock->locked)
            UnlockFileEx( lock->handle, 0, 1, 0, &lock->overlapped );
        CloseHandle( lock->handle );
    }
    memset( lock, 0, sizeof(*lock) );
    lock->handle = INVALID_HANDLE_VALUE;
}

static HRESULT delete_open_file( HANDLE file )
{
    FILE_DISPOSITION_INFORMATION disposition = {TRUE};
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    status = NtSetInformationFile( file, &io, &disposition,
                                   sizeof(disposition),
                                   FileDispositionInformation );
    return status ? ntstatus_error( status ) : S_OK;
}

static HRESULT discard_pending_catalog( const struct catalog_store *store )
{
    HANDLE file;
    HRESULT hr;

    hr = open_store_child( store, APPX_CATALOG_PENDING_FILE_NAME, DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                           FILE_SHARE_DELETE, FILE_OPEN, 0, &file );
    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            return S_OK;
        return hr;
    }
    hr = delete_open_file( file );
    CloseHandle( file );
    return hr;
}

static HRESULT load_catalog_file( const struct catalog_store *store,
                                  APPX_CATALOG_SNAPSHOT **snapshot )
{
    LARGE_INTEGER size;
    HANDLE file;
    BYTE *data = NULL;
    DWORD read;
    HRESULT hr;

    *snapshot = NULL;
    hr = open_store_child( store, APPX_CATALOG_FILE_NAME, FILE_READ_DATA,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN,
                           FILE_SEQUENTIAL_ONLY, &file );
    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            return appx_catalog_snapshot_create( 0, NULL, 0, snapshot );
        return hr;
    }

    if (!GetFileSizeEx( file, &size ))
    {
        hr = win32_error( GetLastError() );
        goto done;
    }
    if (size.QuadPart > APPX_CATALOG_MAX_FILE_SIZE ||
        size.QuadPart > MAXDWORD)
    {
        hr = malformed_catalog();
        goto done;
    }
    if (!(data = HeapAlloc( GetProcessHeap(), 0, size.u.LowPart )))
    {
        hr = size.u.LowPart ? E_OUTOFMEMORY : malformed_catalog();
        goto done;
    }
    if (!ReadFile( file, data, size.u.LowPart, &read, NULL ))
    {
        hr = win32_error( GetLastError() );
        goto done;
    }
    if (read != size.u.LowPart)
    {
        hr = HRESULT_FROM_WIN32( ERROR_HANDLE_EOF );
        goto done;
    }
    hr = parse_catalog( data, size.u.LowPart, snapshot );

done:
    HeapFree( GetProcessHeap(), 0, data );
    CloseHandle( file );
    return hr;
}

HRESULT WINAPI appx_catalog_load( const WCHAR *store_root,
                                  APPX_CATALOG_SNAPSHOT **snapshot )
{
    return appx_catalog_load_bounded( store_root, INFINITE, NULL, snapshot );
}

HRESULT WINAPI appx_catalog_load_bounded( const WCHAR *store_root,
                                          DWORD timeout_ms,
                                          HANDLE cancel_event,
                                          APPX_CATALOG_SNAPSHOT **snapshot )
{
    struct catalog_store store;
    struct catalog_lock lock;
    HRESULT hr;

    TRACE( "store_root %s, snapshot %p.\n", debugstr_w(store_root), snapshot );

    if (!snapshot) return E_INVALIDARG;
    *snapshot = NULL;
    if (FAILED(hr = open_catalog_store( store_root, &store ))) return hr;
    if (FAILED(hr = acquire_catalog_lock(
            &store, timeout_ms, cancel_event, &lock )))
    {
        close_catalog_store( &store );
        return hr;
    }
    if (SUCCEEDED(hr = discard_pending_catalog( &store )))
        hr = load_catalog_file( &store, snapshot );
    release_catalog_lock( &lock );
    close_catalog_store( &store );
    return hr;
}

static HRESULT write_pending_catalog( const struct catalog_store *store,
                                      const struct catalog_buffer *buffer,
                                      HANDLE *pending )
{
    DWORD written, offset = 0;
    HRESULT hr = S_OK;

    *pending = INVALID_HANDLE_VALUE;
    if (FAILED(hr = open_store_child( store, APPX_CATALOG_PENDING_FILE_NAME,
                                      FILE_WRITE_DATA | DELETE, 0,
                                      FILE_CREATE,
                                      FILE_SEQUENTIAL_ONLY |
                                      FILE_WRITE_THROUGH, pending )))
        return hr;
    while (offset < buffer->size)
    {
        DWORD chunk = buffer->size - offset;

        if (!WriteFile( *pending, buffer->data + offset, chunk, &written, NULL ))
        {
            hr = win32_error( GetLastError() );
            goto failed;
        }
        if (written != chunk)
        {
            hr = HRESULT_FROM_WIN32( ERROR_WRITE_FAULT );
            goto failed;
        }
        offset += written;
    }
    if (!FlushFileBuffers( *pending ))
    {
        hr = win32_error( GetLastError() );
        goto failed;
    }
    return S_OK;

failed:
    delete_open_file( *pending );
    CloseHandle( *pending );
    *pending = INVALID_HANDLE_VALUE;
    return hr;
}

static HRESULT flush_store_directory( HANDLE directory )
{
    IO_STATUS_BLOCK io;
    NTSTATUS status = NtFlushBuffersFile( directory, &io );

    if (!status) return S_OK;
    if (status == STATUS_INVALID_DEVICE_REQUEST ||
        status == STATUS_NOT_IMPLEMENTED || status == STATUS_NOT_SUPPORTED)
    {
        WARN( "Directory metadata flush is unavailable, status %#lx; "
              "the catalog is visible but has weak crash durability.\n", status );
        return APPX_CATALOG_S_WEAK_DURABILITY;
    }
    WARN( "Directory metadata flush failed, status %#lx; the catalog is "
          "visible but may not survive a crash.\n", status );
    return APPX_CATALOG_S_WEAK_DURABILITY;
}

static HRESULT replace_catalog_with_pending( const struct catalog_store *store,
                                             HANDLE pending )
{
    const WCHAR *name = APPX_CATALOG_FILE_NAME;
    UINT32 name_size = lstrlenW( name ) * sizeof(*name);
    UINT32 size = offsetof( FILE_RENAME_INFORMATION, FileName ) + name_size;
    FILE_RENAME_INFORMATION *rename;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    HRESULT hr;

    if (!(rename = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size )))
        return E_OUTOFMEMORY;
    rename->ReplaceIfExists = TRUE;
    rename->RootDirectory = store->root;
    rename->FileNameLength = name_size;
    memcpy( rename->FileName, name, name_size );
    status = NtSetInformationFile( pending, &io, rename, size,
                                   FileRenameInformation );
    HeapFree( GetProcessHeap(), 0, rename );
    if (status)
    {
        TRACE( "Renaming the pending catalog failed, status %#lx.\n", status );
        return ntstatus_error( status );
    }
    hr = flush_store_directory( store->root );
    return hr;
}

HRESULT WINAPI appx_catalog_publish( const WCHAR *store_root,
                                     UINT64 expected_epoch,
                                     const APPX_CATALOG_SNAPSHOT *replacement )
{
    return appx_catalog_publish_bounded(
        store_root, expected_epoch, replacement, INFINITE, NULL );
}

HRESULT WINAPI appx_catalog_publish_bounded(
    const WCHAR *store_root, UINT64 expected_epoch,
    const APPX_CATALOG_SNAPSHOT *replacement,
    DWORD timeout_ms, HANDLE cancel_event )
{
    APPX_CATALOG_SNAPSHOT *current = NULL;
    struct catalog_buffer serialized = {0};
    struct catalog_store store;
    struct catalog_lock lock;
    HANDLE pending = INVALID_HANDLE_VALUE;
    HRESULT hr;

    TRACE( "store_root %s, expected_epoch %s, replacement %p.\n",
           debugstr_w(store_root), wine_dbgstr_longlong(expected_epoch), replacement );

    if (!replacement || expected_epoch == ~(UINT64)0 ||
        replacement->epoch != expected_epoch + 1)
        return E_INVALIDARG;
    if (FAILED(hr = open_catalog_store( store_root, &store ))) return hr;
    if (FAILED(hr = acquire_catalog_lock(
            &store, timeout_ms, cancel_event, &lock )))
    {
        close_catalog_store( &store );
        return hr;
    }

    if (FAILED(hr = discard_pending_catalog( &store )))
        goto done;
    if (FAILED(hr = load_catalog_file( &store, &current )))
        goto done;
    if (current->epoch != expected_epoch)
    {
        hr = APPX_CATALOG_E_EPOCH_CONFLICT;
        goto done;
    }
    if (FAILED(hr = serialize_catalog( replacement, &serialized )))
        goto done;
    if (FAILED(hr = write_pending_catalog( &store, &serialized, &pending )))
        goto done;
    hr = replace_catalog_with_pending( &store, pending );

done:
    if (pending != INVALID_HANDLE_VALUE)
    {
        if (FAILED(hr)) delete_open_file( pending );
        CloseHandle( pending );
    }
    if (FAILED(hr)) discard_pending_catalog( &store );
    HeapFree( GetProcessHeap(), 0, serialized.data );
    appx_catalog_snapshot_free( current );
    release_catalog_lock( &lock );
    close_catalog_store( &store );
    return hr;
}
