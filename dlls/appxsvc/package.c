/*
 * AppX package inspection and integrity reconciliation
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

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"
#include "bcrypt.h"

#include "blockmap.h"
#include "content_types.h"
#include "manifest.h"
#include "package.h"
#include "signature.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define METADATA_READ_CHUNK (1024 * 1024)
#define ZIP_METHOD_STORE    0
#define ZIP_METHOD_DEFLATE  8

static const WCHAR manifest_path[] = L"AppxManifest.xml";
static const WCHAR block_map_path[] = L"AppxBlockMap.xml";
static const WCHAR content_types_path[] = L"[Content_Types].xml";
static const WCHAR signature_path[] = L"AppxSignature.p7x";
static const WCHAR code_integrity_path[] = L"AppxMetadata\\CodeIntegrity.cat";

struct appx_package_inspection
{
    WINE_APPX_ARCHIVE *archive;
    APPX_MANIFEST *manifest;
    APPX_PACKAGE_FILE *files;
    UINT32 file_count;
    UINT64 expanded_size;
    UINT64 archive_expanded_size;
    BYTE content_id[APPX_PACKAGE_CONTENT_ID_SIZE];
    BYTE signer_id[APPX_PACKAGE_SIGNER_ID_SIZE];
};

static HRESULT check_cancel_event( HANDLE cancel_event )
{
    DWORD wait;

    if (!cancel_event) return S_OK;
    wait = WaitForSingleObject( cancel_event, 0 );
    if (wait == WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    if (wait == WAIT_FAILED)
        return HRESULT_FROM_WIN32( GetLastError() );
    return wait == WAIT_TIMEOUT ? S_OK : E_FAIL;
}

struct stream_cancel_monitor
{
    WINE_APPX_ARCHIVE_STREAM *stream;
    HANDLE cancel_event;
    HANDLE done_event;
    HANDLE thread;
    LONG cancelled;
    LONG status;
};

static void stream_cancel_monitor_record( struct stream_cancel_monitor *monitor,
                                          HRESULT hr )
{
    InterlockedCompareExchange( &monitor->status, hr, S_OK );
}

static DWORD WINAPI stream_cancel_monitor_proc( void *parameter )
{
    struct stream_cancel_monitor *monitor = parameter;
    HANDLE handles[2] = {monitor->cancel_event, monitor->done_event};
    DWORD wait = WaitForMultipleObjects( 2, handles, FALSE, INFINITE );

    if (wait == WAIT_OBJECT_0)
    {
        InterlockedExchange( &monitor->cancelled, 1 );
        wine_appx_archive_stream_cancel( monitor->stream );
    }
    else if (wait == WAIT_FAILED)
    {
        stream_cancel_monitor_record(
            monitor, HRESULT_FROM_WIN32(GetLastError()) );
        wine_appx_archive_stream_cancel( monitor->stream );
    }
    else if (wait != WAIT_OBJECT_0 + 1)
    {
        stream_cancel_monitor_record( monitor, E_FAIL );
        wine_appx_archive_stream_cancel( monitor->stream );
    }
    return 0;
}

static HRESULT stream_cancel_monitor_start(
    struct stream_cancel_monitor *monitor, WINE_APPX_ARCHIVE_STREAM *stream,
    HANDLE cancel_event )
{
    HRESULT hr;

    memset( monitor, 0, sizeof(*monitor) );
    monitor->stream = stream;
    monitor->cancel_event = cancel_event;
    monitor->status = S_OK;
    if (!cancel_event) return S_OK;
    if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
    if (!(monitor->done_event = CreateEventW( NULL, TRUE, FALSE, NULL )))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!(monitor->thread = CreateThread( NULL, 0, stream_cancel_monitor_proc,
                                          monitor, 0, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        CloseHandle( monitor->done_event );
        monitor->done_event = NULL;
        return hr;
    }
    return S_OK;
}

static HRESULT stream_cancel_monitor_check(
    struct stream_cancel_monitor *monitor )
{
    HRESULT hr;

    if (!monitor->cancel_event) return S_OK;
    if (InterlockedCompareExchange( &monitor->cancelled, 0, 0 ))
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    hr = InterlockedCompareExchange( &monitor->status, S_OK, S_OK );
    if (FAILED(hr)) return hr;
    hr = check_cancel_event( monitor->cancel_event );
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        InterlockedExchange( &monitor->cancelled, 1 );
        wine_appx_archive_stream_cancel( monitor->stream );
    }
    else if (FAILED(hr))
    {
        stream_cancel_monitor_record( monitor, hr );
        wine_appx_archive_stream_cancel( monitor->stream );
    }
    return hr;
}

static HRESULT stream_cancel_monitor_finish(
    struct stream_cancel_monitor *monitor, HRESULT hr )
{
    HRESULT status;
    DWORD wait;

    if (!monitor->thread) return hr;
    if (!SetEvent( monitor->done_event ))
        stream_cancel_monitor_record(
            monitor, HRESULT_FROM_WIN32(GetLastError()) );
    wait = WaitForSingleObject( monitor->thread, INFINITE );
    if (wait == WAIT_FAILED)
        stream_cancel_monitor_record(
            monitor, HRESULT_FROM_WIN32(GetLastError()) );
    else if (wait != WAIT_OBJECT_0)
        stream_cancel_monitor_record( monitor, E_FAIL );
    CloseHandle( monitor->thread );
    CloseHandle( monitor->done_event );
    monitor->thread = NULL;
    monitor->done_event = NULL;

    if (InterlockedCompareExchange( &monitor->cancelled, 0, 0 ))
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    status = InterlockedCompareExchange( &monitor->status, S_OK, S_OK );
    return FAILED(status) ? status : hr;
}

struct hash_engine
{
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE block;
    BCRYPT_HASH_HANDLE file;
};

static BOOL equal_path( const WCHAR *left, const WCHAR *right )
{
    return CompareStringOrdinal( left, -1, right, -1, TRUE ) == CSTR_EQUAL;
}

static BOOL exact_path( const WCHAR *left, const WCHAR *right )
{
    return CompareStringOrdinal( left, -1, right, -1, FALSE ) == CSTR_EQUAL;
}

static BOOL is_reserved_metadata_path( const WCHAR *path )
{
    static const WCHAR appx_metadata[] = L"AppxMetadata\\";
    static const WCHAR system_metadata[] = L"Microsoft.System.Package.Metadata\\";
    UINT32 length = lstrlenW( path );

    if (length >= ARRAY_SIZE(appx_metadata) - 1 &&
        CompareStringOrdinal( path, ARRAY_SIZE(appx_metadata) - 1,
                              appx_metadata, ARRAY_SIZE(appx_metadata) - 1,
                              TRUE ) == CSTR_EQUAL)
        return !equal_path( path, code_integrity_path );
    return length >= ARRAY_SIZE(system_metadata) - 1 &&
           CompareStringOrdinal( path, ARRAY_SIZE(system_metadata) - 1,
                                 system_metadata,
                                 ARRAY_SIZE(system_metadata) - 1,
                                 TRUE ) == CSTR_EQUAL;
}

static BOOL is_block_map_footprint( const WCHAR *path )
{
    return equal_path( path, block_map_path ) ||
           equal_path( path, content_types_path ) ||
           equal_path( path, signature_path ) ||
           equal_path( path, code_integrity_path );
}

static BOOL constant_equal( const BYTE *left, const BYTE *right, UINT32 size )
{
    BYTE difference = 0;
    UINT32 i;

    for (i = 0; i < size; i++) difference |= left[i] ^ right[i];
    return !difference;
}

static HRESULT get_entry_path( WINE_APPX_ARCHIVE *archive, UINT32 index,
                               WINE_APPX_ARCHIVE_ENTRY *entry, WCHAR **path )
{
    UINT32 length = 0, capacity;
    HRESULT hr;

    *path = NULL;
    memset( entry, 0, sizeof(*entry) );
    entry->size = sizeof(*entry);
    hr = wine_appx_archive_get_entry( archive, index, entry, &length, NULL );
    if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) ||
        !length || length > WINE_APPX_MAX_PATH_CHARS)
        return FAILED(hr) ? hr : APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!(*path = HeapAlloc( GetProcessHeap(), 0,
                             (SIZE_T)length * sizeof(**path) )))
        return E_OUTOFMEMORY;
    capacity = length;
    entry->size = sizeof(*entry);
    hr = wine_appx_archive_get_entry( archive, index, entry, &capacity, *path );
    if (FAILED(hr) || capacity != length || (*path)[length - 1])
    {
        HeapFree( GetProcessHeap(), 0, *path );
        *path = NULL;
        return FAILED(hr) ? hr : APPX_E_INVALID_PACKAGING_LAYOUT;
    }
    return S_OK;
}

static HRESULT get_named_entry( WINE_APPX_ARCHIVE *archive, const WCHAR *path,
                                UINT32 limit, BOOL required, UINT32 *index,
                                WINE_APPX_ARCHIVE_ENTRY *entry )
{
    WCHAR *actual = NULL;
    HRESULT hr;

    *index = 0;
    memset( entry, 0, sizeof(*entry) );
    entry->size = sizeof(*entry);
    if (FAILED(hr = wine_appx_archive_find_entry( archive, path, index )))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            return required ? APPX_E_MISSING_REQUIRED_FILE : S_FALSE;
        return hr;
    }
    if (FAILED(hr = get_entry_path( archive, *index, entry, &actual )))
        return hr;
    if (!exact_path( actual, path ) ||
        (entry->flags & WINE_APPX_ENTRY_DIRECTORY) ||
        !entry->uncompressed_size || entry->uncompressed_size > limit ||
        entry->uncompressed_size > MAXDWORD)
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
    HeapFree( GetProcessHeap(), 0, actual );
    return hr;
}

static HRESULT read_entry( WINE_APPX_ARCHIVE *archive, const WCHAR *path,
                           UINT32 limit, HANDLE cancel_event,
                           BYTE **data, UINT32 *size )
{
    WINE_APPX_ARCHIVE_ENTRY entry;
    WINE_APPX_ARCHIVE_STREAM *stream = NULL;
    struct stream_cancel_monitor monitor;
    BYTE terminal;
    BYTE *buffer = NULL;
    UINT64 offset = 0;
    UINT32 index;
    HRESULT hr;

    memset( &monitor, 0, sizeof(monitor) );
    *data = NULL;
    *size = 0;
    if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
    if (FAILED(hr = get_named_entry( archive, path, limit, TRUE,
                                     &index, &entry )))
        return hr;
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0, (SIZE_T)entry.uncompressed_size )))
        return E_OUTOFMEMORY;
    if (FAILED(hr = wine_appx_archive_stream_open( archive, index, &stream )))
        goto done;
    if (FAILED(hr = stream_cancel_monitor_start( &monitor, stream,
                                                  cancel_event )))
        goto done;

    while (offset < entry.uncompressed_size)
    {
        UINT64 remaining = entry.uncompressed_size - offset;
        UINT32 capacity = remaining < METADATA_READ_CHUNK ?
                          (UINT32)remaining : METADATA_READ_CHUNK;
        UINT32 read = 0;
        HRESULT cancel_hr;

        if (FAILED(hr = stream_cancel_monitor_check( &monitor ))) goto done;
        hr = wine_appx_archive_stream_read( stream, buffer + offset, capacity, &read );
        if (FAILED(cancel_hr = stream_cancel_monitor_check( &monitor )))
        {
            hr = cancel_hr;
            goto done;
        }
        if (hr == S_FALSE || FAILED(hr) || !read || read > capacity)
        {
            if (hr == S_FALSE || SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
        offset += read;
    }
    {
        UINT32 read = 0;
        HRESULT cancel_hr;

        if (FAILED(hr = stream_cancel_monitor_check( &monitor ))) goto done;
        hr = wine_appx_archive_stream_read( stream, &terminal, 1, &read );
        if (FAILED(cancel_hr = stream_cancel_monitor_check( &monitor )))
        {
            hr = cancel_hr;
            goto done;
        }
        if (hr != S_FALSE || read)
        {
            if (SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
    }

    hr = S_OK;

done:
    hr = stream_cancel_monitor_finish( &monitor, hr );
    if (SUCCEEDED(hr))
    {
        *data = buffer;
        *size = (UINT32)entry.uncompressed_size;
        buffer = NULL;
    }
    if (stream) wine_appx_archive_stream_close( stream );
    HeapFree( GetProcessHeap(), 0, buffer );
    return hr;
}

static HRESULT hash_engine_open( struct hash_engine *engine )
{
    NTSTATUS status;

    memset( engine, 0, sizeof(*engine) );
    if ((status = BCryptOpenAlgorithmProvider( &engine->algorithm,
                                               BCRYPT_SHA256_ALGORITHM, NULL,
                                               BCRYPT_HASH_REUSABLE_FLAG )))
        return HRESULT_FROM_NT( status );
    if ((status = BCryptCreateHash( engine->algorithm, &engine->block,
                                    NULL, 0, NULL, 0,
                                    BCRYPT_HASH_REUSABLE_FLAG )) ||
        (status = BCryptCreateHash( engine->algorithm, &engine->file,
                                    NULL, 0, NULL, 0,
                                    BCRYPT_HASH_REUSABLE_FLAG )))
    {
        if (engine->block) BCryptDestroyHash( engine->block );
        BCryptCloseAlgorithmProvider( engine->algorithm, 0 );
        memset( engine, 0, sizeof(*engine) );
        return HRESULT_FROM_NT( status );
    }
    return S_OK;
}

static void hash_engine_close( struct hash_engine *engine )
{
    if (engine->file) BCryptDestroyHash( engine->file );
    if (engine->block) BCryptDestroyHash( engine->block );
    if (engine->algorithm) BCryptCloseAlgorithmProvider( engine->algorithm, 0 );
}

static HRESULT hash_data( BCRYPT_HASH_HANDLE hash, const BYTE *data, UINT32 size )
{
    NTSTATUS status = BCryptHashData( hash, (BYTE *)data, size, 0 );

    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

static HRESULT finish_hash( BCRYPT_HASH_HANDLE hash,
                            BYTE digest[APPX_BLOCK_MAP_HASH_SIZE] )
{
    NTSTATUS status = BCryptFinishHash( hash, digest, APPX_BLOCK_MAP_HASH_SIZE, 0 );

    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

static HRESULT reconcile_entry( const APPX_BLOCK_MAP *map, UINT32 file_index,
                                const APPX_BLOCK_MAP_FILE *file,
                                const WINE_APPX_ARCHIVE_ENTRY *entry,
                                HANDLE cancel_event )
{
    UINT64 local_header_size, compressed_blocks = 0;
    UINT32 i;
    HRESULT hr;

    if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
    if (entry->flags & WINE_APPX_ENTRY_DIRECTORY)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (entry->uncompressed_size != file->size ||
        entry->data_offset < entry->local_header_offset)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    local_header_size = entry->data_offset - entry->local_header_offset;
    if (local_header_size != file->local_file_header_size)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    if (entry->compression_method == ZIP_METHOD_STORE)
    {
        if (entry->compressed_size != entry->uncompressed_size)
            return APPX_E_FILE_COMPRESSION_MISMATCH;
        for (i = 0; i < file->block_count; i++)
        {
            const APPX_BLOCK_MAP_BLOCK *block =
                appx_block_map_get_block( map, file_index, i );

            if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
            if (!block || block->has_compressed_size)
                return APPX_E_FILE_COMPRESSION_MISMATCH;
        }
        return S_OK;
    }
    if (entry->compression_method != ZIP_METHOD_DEFLATE)
        return APPX_E_FILE_COMPRESSION_MISMATCH;

    for (i = 0; i < file->block_count; i++)
    {
        const APPX_BLOCK_MAP_BLOCK *block =
            appx_block_map_get_block( map, file_index, i );

        if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
        if (!block || !block->has_compressed_size ||
            compressed_blocks > ~(UINT64)0 - block->compressed_size)
            return APPX_E_FILE_COMPRESSION_MISMATCH;
        compressed_blocks += block->compressed_size;
    }
    if (compressed_blocks != entry->compressed_size &&
        (compressed_blocks > ~(UINT64)0 - 2 ||
         compressed_blocks + 2 != entry->compressed_size))
        return APPX_E_FILE_COMPRESSION_MISMATCH;
    return S_OK;
}

static HRESULT verify_file_stream( WINE_APPX_ARCHIVE *archive,
                                   const APPX_BLOCK_MAP *map,
                                   UINT32 file_index, UINT32 archive_index,
                                   const APPX_BLOCK_MAP_FILE *file,
                                   struct hash_engine *engine,
                                   HANDLE cancel_event )
{
    WINE_APPX_ARCHIVE_STREAM *stream = NULL;
    struct stream_cancel_monitor monitor;
    BYTE buffer[APPX_BLOCK_MAP_BLOCK_SIZE], digest[APPX_BLOCK_MAP_HASH_SIZE];
    UINT32 i;
    HRESULT hr;

    memset( &monitor, 0, sizeof(monitor) );
    if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
    if (FAILED(hr = wine_appx_archive_stream_open( archive, archive_index, &stream )))
        return hr;
    if (FAILED(hr = stream_cancel_monitor_start( &monitor, stream,
                                                  cancel_event )))
        goto done;

    for (i = 0; i < file->block_count; i++)
    {
        const APPX_BLOCK_MAP_BLOCK *block =
            appx_block_map_get_block( map, file_index, i );
        UINT32 offset = 0;

        if (FAILED(hr = stream_cancel_monitor_check( &monitor ))) goto done;
        if (!block)
        {
            hr = APPX_E_INVALID_BLOCKMAP;
            goto done;
        }
        while (offset < block->logical_size)
        {
            UINT32 read = 0, remaining = block->logical_size - offset;
            HRESULT cancel_hr;

            if (FAILED(hr = stream_cancel_monitor_check( &monitor ))) goto done;
            hr = wine_appx_archive_stream_read( stream, buffer + offset,
                                                remaining, &read );
            if (FAILED(cancel_hr = stream_cancel_monitor_check( &monitor )))
            {
                hr = cancel_hr;
                goto done;
            }
            if (hr == S_FALSE || FAILED(hr) || !read || read > remaining)
            {
                if (hr == S_FALSE || SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
                goto done;
            }
            if (FAILED(hr = hash_data( engine->block, buffer + offset, read )))
                goto done;
            if (file->has_file_hash &&
                FAILED(hr = hash_data( engine->file, buffer + offset, read )))
                goto done;
            offset += read;
        }
        if (FAILED(hr = stream_cancel_monitor_check( &monitor ))) goto done;
        if (FAILED(hr = finish_hash( engine->block, digest ))) goto done;
        if (!constant_equal( digest, block->hash, sizeof(digest) ))
        {
            hr = APPX_E_BLOCK_HASH_INVALID;
            goto done;
        }
    }
    {
        UINT32 read = 0;
        HRESULT cancel_hr;

        if (FAILED(hr = stream_cancel_monitor_check( &monitor ))) goto done;
        hr = wine_appx_archive_stream_read( stream, buffer, 1, &read );
        if (FAILED(cancel_hr = stream_cancel_monitor_check( &monitor )))
        {
            hr = cancel_hr;
            goto done;
        }
        if (hr != S_FALSE || read)
        {
            if (SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
    }
    if (file->has_file_hash)
    {
        if (FAILED(hr = stream_cancel_monitor_check( &monitor ))) goto done;
        if (FAILED(hr = finish_hash( engine->file, digest ))) goto done;
        if (!constant_equal( digest, file->file_hash, sizeof(digest) ))
        {
            hr = APPX_E_BLOCK_HASH_INVALID;
            goto done;
        }
    }
    hr = S_OK;

done:
    hr = stream_cancel_monitor_finish( &monitor, hr );
    wine_appx_archive_stream_close( stream );
    return hr;
}

static int compare_package_files( const void *left, const void *right )
{
    const APPX_PACKAGE_FILE *const *a = left;
    const APPX_PACKAGE_FILE *const *b = right;
    int result = CompareStringOrdinal( (*a)->path, -1, (*b)->path, -1, FALSE );

    return result == CSTR_LESS_THAN ? -1 : result == CSTR_GREATER_THAN ? 1 : 0;
}

static const APPX_PACKAGE_FILE *find_package_file( APPX_PACKAGE_FILE *const *files,
                                                  UINT32 count, const WCHAR *path )
{
    UINT32 low = 0, high = count;

    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;
        int result = CompareStringOrdinal( path, -1, files[middle]->path, -1, FALSE );

        if (result == CSTR_EQUAL) return files[middle];
        if (result == CSTR_LESS_THAN) high = middle;
        else low = middle + 1;
    }
    return NULL;
}

static HRESULT validate_content_type_coverage( WINE_APPX_ARCHIVE *archive,
                                               const APPX_CONTENT_TYPES *types,
                                               APPX_PACKAGE_FILE *const *mapped,
                                               UINT32 mapped_count,
                                               HANDLE cancel_event )
{
    WINE_APPX_ARCHIVE_ENTRY entry;
    WCHAR *path = NULL, *part = NULL;
    UINT32 count, i, j, length;
    HRESULT hr;

    if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
    if (FAILED(hr = wine_appx_archive_get_count( archive, &count ))) return hr;
    for (i = 0; i < count; i++)
    {
        const WCHAR *content_type;
        BOOL should_be_mapped;

        if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
        if (FAILED(hr = get_entry_path( archive, i, &entry, &path ))) goto done;
        if (entry.flags & WINE_APPX_ENTRY_DIRECTORY)
        {
            HeapFree( GetProcessHeap(), 0, path );
            path = NULL;
            continue;
        }
        if (is_reserved_metadata_path( path ))
        {
            hr = HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
            goto done;
        }

        should_be_mapped = !is_block_map_footprint( path );
        if (should_be_mapped != !!find_package_file( mapped, mapped_count, path ))
        {
            hr = APPX_E_INVALID_BLOCKMAP;
            goto done;
        }
        if (equal_path( path, content_types_path ))
        {
            HeapFree( GetProcessHeap(), 0, path );
            path = NULL;
            continue;
        }

        length = lstrlenW( path );
        if (length >= WINE_APPX_MAX_PATH_CHARS - 1 ||
            !(part = HeapAlloc( GetProcessHeap(), 0,
                                (length + 2) * sizeof(*part) )))
        {
            hr = length >= WINE_APPX_MAX_PATH_CHARS - 1 ?
                 APPX_E_INVALID_PACKAGING_LAYOUT : E_OUTOFMEMORY;
            goto done;
        }
        part[0] = '/';
        for (j = 0; j < length; j++)
            part[j + 1] = path[j] == '\\' ? '/' : path[j];
        part[length + 1] = 0;
        content_type = appx_content_types_get_content_type( types, part );
        if (!content_type)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
        HeapFree( GetProcessHeap(), 0, part );
        HeapFree( GetProcessHeap(), 0, path );
        part = NULL;
        path = NULL;
    }
    hr = check_cancel_event( cancel_event );

done:
    HeapFree( GetProcessHeap(), 0, part );
    HeapFree( GetProcessHeap(), 0, path );
    return hr;
}

static void free_files( APPX_PACKAGE_FILE *files, UINT32 count )
{
    UINT32 i;

    if (!files) return;
    for (i = 0; i < count; i++)
        HeapFree( GetProcessHeap(), 0, (void *)files[i].path );
    HeapFree( GetProcessHeap(), 0, files );
}

static HRESULT validate_block_map( WINE_APPX_ARCHIVE *archive,
                                   const APPX_BLOCK_MAP *map,
                                   const APPX_CONTENT_TYPES *types,
                                   APPX_PACKAGE_FILE **result_files,
                                   UINT32 *result_count, UINT64 *expanded_size,
                                   HANDLE cancel_event )
{
    APPX_PACKAGE_FILE **ordered = NULL, *files = NULL;
    struct hash_engine engine;
    UINT32 count, i;
    UINT64 total = 0;
    HRESULT hr;

    *result_files = NULL;
    *result_count = 0;
    *expanded_size = 0;
    if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
    count = appx_block_map_get_file_count( map );
    if (!count || count > APPX_BLOCK_MAP_MAX_FILES ||
        !(files = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                             (SIZE_T)count * sizeof(*files) )))
        return count && count <= APPX_BLOCK_MAP_MAX_FILES ?
               E_OUTOFMEMORY : APPX_E_INVALID_BLOCKMAP;
    if (!(ordered = HeapAlloc( GetProcessHeap(), 0,
                               (SIZE_T)count * sizeof(*ordered) )))
    {
        HeapFree( GetProcessHeap(), 0, files );
        return E_OUTOFMEMORY;
    }
    if (FAILED(hr = hash_engine_open( &engine ))) goto done;

    for (i = 0; i < count; i++)
    {
        const APPX_BLOCK_MAP_FILE *file = appx_block_map_get_file( map, i );
        WINE_APPX_ARCHIVE_ENTRY entry;
        WCHAR *path = NULL;
        UINT32 archive_index;

        if (FAILED(hr = check_cancel_event( cancel_event ))) goto close_engine;
        if (!file || is_block_map_footprint( file->name ) ||
            FAILED(hr = wine_appx_archive_find_entry( archive, file->name,
                                                       &archive_index )) ||
            FAILED(hr = get_entry_path( archive, archive_index, &entry, &path )))
        {
            if (SUCCEEDED(hr)) hr = APPX_E_INVALID_BLOCKMAP;
            else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                hr = APPX_E_INVALID_BLOCKMAP;
            HeapFree( GetProcessHeap(), 0, path );
            goto close_engine;
        }
        if (!exact_path( path, file->name ))
        {
            HeapFree( GetProcessHeap(), 0, path );
            hr = APPX_E_INVALID_BLOCKMAP;
            goto close_engine;
        }
        if (FAILED(hr = reconcile_entry( map, i, file, &entry,
                                          cancel_event )) ||
            FAILED(hr = verify_file_stream( archive, map, i, archive_index,
                                             file, &engine, cancel_event )))
        {
            HeapFree( GetProcessHeap(), 0, path );
            goto close_engine;
        }
        if (total > ~(UINT64)0 - entry.uncompressed_size)
        {
            HeapFree( GetProcessHeap(), 0, path );
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto close_engine;
        }

        files[i].path = path;
        files[i].archive_index = archive_index;
        files[i].compression_method = entry.compression_method;
        files[i].compressed_size = entry.compressed_size;
        files[i].uncompressed_size = entry.uncompressed_size;
        ordered[i] = files + i;
        total += entry.uncompressed_size;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto close_engine;
    qsort( ordered, count, sizeof(*ordered), compare_package_files );
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto close_engine;
    for (i = 1; i < count; i++)
    {
        if (FAILED(hr = check_cancel_event( cancel_event ))) goto close_engine;
        if (!compare_package_files( ordered + i - 1, ordered + i ))
        {
            hr = APPX_E_INVALID_BLOCKMAP;
            goto close_engine;
        }
    }
    if (FAILED(hr = validate_content_type_coverage( archive, types,
                                                    ordered, count,
                                                    cancel_event )))
        goto close_engine;

    if (FAILED(hr = check_cancel_event( cancel_event ))) goto close_engine;
    *result_files = files;
    *result_count = count;
    *expanded_size = total;
    files = NULL;
    hr = S_OK;

close_engine:
    hash_engine_close( &engine );
done:
    HeapFree( GetProcessHeap(), 0, ordered );
    free_files( files, count );
    return hr;
}

HRESULT WINAPI appx_package_inspect_ex(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    HANDLE cancel_event, APPX_PACKAGE_INSPECTION **result )
{
    struct appx_signature_digest_set digests;
    APPX_PACKAGE_INSPECTION *inspection = NULL;
    APPX_CONTENT_TYPES *types = NULL;
    APPX_BLOCK_MAP *block_map = NULL;
    APPX_SIGNATURE *signature = NULL;
    APPX_MANIFEST *manifest = NULL;
    WINE_APPX_ARCHIVE *archive = NULL;
    WINE_APPX_ARCHIVE_ENTRY metadata_entry;
    BYTE *document = NULL;
    UINT32 metadata_index, size;
    HRESULT hr;

    TRACE( "file %p, limits %p, flags %#x, cancel_event %p, result %p.\n",
           file, limits, flags, cancel_event, result );

    if (!result) return E_INVALIDARG;
    *result = NULL;
    if (!file || file == INVALID_HANDLE_VALUE ||
        (flags & ~APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN))
        return E_INVALIDARG;
    if (FAILED(hr = check_cancel_event( cancel_event ))) return hr;
    if (FAILED(hr = wine_appx_archive_open_ex(
        file, limits, WINE_APPX_ARCHIVE_OPEN_PACKAGE, cancel_event, &archive )))
    {
        TRACE( "archive validation failed, hr %#lx.\n", hr );
        goto done;
    }

    if (FAILED(hr = read_entry( archive, signature_path,
                                APPX_SIGNATURE_MAX_SIZE, cancel_event,
                                &document, &size )))
    {
        TRACE( "signature read failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (FAILED(hr = appx_signature_parse_and_verify(
        document, size,
        flags & APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN ?
        APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN : 0,
        &signature )))
    {
        TRACE( "CMS validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    HeapFree( GetProcessHeap(), 0, document );
    document = NULL;

    /*
     * Bound every signed metadata stream before hashing it.  A valid signer
     * must not be able to turn digest verification into an unbounded metadata
     * read, and footprint spelling is canonical rather than case-folded.
     */
    if (FAILED(hr = get_named_entry( archive, content_types_path,
                                     APPX_CONTENT_TYPES_MAX_DOCUMENT_SIZE,
                                     TRUE, &metadata_index,
                                     &metadata_entry )))
    {
        TRACE( "content-types metadata validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (FAILED(hr = get_named_entry( archive, block_map_path,
                                     APPX_BLOCK_MAP_MAX_DOCUMENT_SIZE,
                                     TRUE, &metadata_index,
                                     &metadata_entry )))
    {
        TRACE( "block-map metadata validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (FAILED(hr = get_named_entry( archive, manifest_path,
                                     APPX_MANIFEST_MAX_SIZE, TRUE,
                                     &metadata_index, &metadata_entry )))
    {
        TRACE( "manifest metadata validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    hr = get_named_entry( archive, code_integrity_path,
                          APPX_PACKAGE_MAX_CODE_INTEGRITY_SIZE, FALSE,
                          &metadata_index, &metadata_entry );
    if (FAILED(hr))
    {
        TRACE( "code-integrity metadata validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;

    if (FAILED(hr = appx_archive_calculate_digest_set_ex(
        archive, cancel_event, &digests )))
    {
        TRACE( "signed package digest calculation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (FAILED(hr = appx_signature_verify_digest_set( signature, &digests )))
    {
        TRACE( "signed package digest comparison failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;

    if (FAILED(hr = read_entry( archive, content_types_path,
                                APPX_CONTENT_TYPES_MAX_DOCUMENT_SIZE,
                                cancel_event, &document, &size )))
    {
        TRACE( "content-types read failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (FAILED(hr = appx_content_types_parse( document, size,
                                              APPX_CONTENT_TYPES_MODE_PACKAGE,
                                              &types )))
    {
        TRACE( "content-types parse failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    HeapFree( GetProcessHeap(), 0, document );
    document = NULL;

    if (FAILED(hr = read_entry( archive, block_map_path,
                                APPX_BLOCK_MAP_MAX_DOCUMENT_SIZE,
                                cancel_event, &document, &size )))
    {
        TRACE( "block-map read failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (FAILED(hr = appx_block_map_parse( document, size, &block_map )))
    {
        TRACE( "block-map parse failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    HeapFree( GetProcessHeap(), 0, document );
    document = NULL;

    if (FAILED(hr = read_entry( archive, manifest_path,
                                APPX_MANIFEST_MAX_SIZE, cancel_event,
                                &document, &size )))
    {
        TRACE( "manifest read failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (FAILED(hr = appx_manifest_parse( document, size, &manifest )))
    {
        TRACE( "manifest parse failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (!appx_manifest_get_identity( manifest ))
    {
        hr = APPX_E_INVALID_MANIFEST;
        goto done;
    }
    if (FAILED(hr = appx_signature_check_publisher(
        signature, appx_manifest_get_identity( manifest )->publisher )))
    {
        TRACE( "publisher binding failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    HeapFree( GetProcessHeap(), 0, document );
    document = NULL;

    if (!(inspection = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  sizeof(*inspection) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    memcpy( inspection->content_id, digests.package_contents,
            sizeof(inspection->content_id) );
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    if (FAILED(hr = appx_signature_get_signer_certificate_id(
        signature, inspection->signer_id, sizeof(inspection->signer_id) )))
        goto done;
    if (FAILED(hr = validate_block_map( archive, block_map, types,
                                        &inspection->files,
                                        &inspection->file_count,
                                        &inspection->expanded_size,
                                        cancel_event )))
    {
        TRACE( "block-map reconciliation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = wine_appx_archive_get_total_uncompressed_size(
            archive, &inspection->archive_expanded_size )))
        goto done;
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;

    inspection->archive = archive;
    inspection->manifest = manifest;
    archive = NULL;
    manifest = NULL;
    if (FAILED(hr = check_cancel_event( cancel_event ))) goto done;
    *result = inspection;
    inspection = NULL;
    hr = S_OK;

done:
    HeapFree( GetProcessHeap(), 0, document );
    appx_package_inspection_free( inspection );
    appx_manifest_free( manifest );
    appx_signature_free( signature );
    appx_block_map_free( block_map );
    appx_content_types_free( types );
    wine_appx_archive_close( archive );
    return hr;
}

HRESULT WINAPI appx_package_inspect(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    APPX_PACKAGE_INSPECTION **result )
{
    return appx_package_inspect_ex( file, limits, flags, NULL, result );
}

void WINAPI appx_package_inspection_free( APPX_PACKAGE_INSPECTION *inspection )
{
    if (!inspection) return;
    free_files( inspection->files, inspection->file_count );
    appx_manifest_free( inspection->manifest );
    wine_appx_archive_close( inspection->archive );
    HeapFree( GetProcessHeap(), 0, inspection );
}

const APPX_MANIFEST *WINAPI appx_package_inspection_get_manifest(
    const APPX_PACKAGE_INSPECTION *inspection )
{
    return inspection ? inspection->manifest : NULL;
}

UINT32 WINAPI appx_package_inspection_get_file_count(
    const APPX_PACKAGE_INSPECTION *inspection )
{
    return inspection ? inspection->file_count : 0;
}

const APPX_PACKAGE_FILE *WINAPI appx_package_inspection_get_file(
    const APPX_PACKAGE_INSPECTION *inspection, UINT32 index )
{
    if (!inspection || index >= inspection->file_count) return NULL;
    return inspection->files + index;
}

UINT64 WINAPI appx_package_inspection_get_expanded_size(
    const APPX_PACKAGE_INSPECTION *inspection )
{
    return inspection ? inspection->expanded_size : 0;
}

UINT64 WINAPI appx_package_inspection_get_archive_expanded_size(
    const APPX_PACKAGE_INSPECTION *inspection )
{
    return inspection ? inspection->archive_expanded_size : 0;
}

HRESULT WINAPI appx_package_inspection_get_content_id(
    const APPX_PACKAGE_INSPECTION *inspection, BYTE *content_id, UINT32 size )
{
    if (!inspection || !content_id || size != APPX_PACKAGE_CONTENT_ID_SIZE)
        return E_INVALIDARG;
    memcpy( content_id, inspection->content_id, size );
    return S_OK;
}

HRESULT WINAPI appx_package_inspection_get_signer_id(
    const APPX_PACKAGE_INSPECTION *inspection, BYTE *signer_id, UINT32 size )
{
    if (!inspection || !signer_id || size != APPX_PACKAGE_SIGNER_ID_SIZE)
        return E_INVALIDARG;
    memcpy( signer_id, inspection->signer_id, size );
    return S_OK;
}

HRESULT WINAPI appx_package_inspection_open_stream(
    const APPX_PACKAGE_INSPECTION *inspection, UINT32 file_index,
    WINE_APPX_ARCHIVE_STREAM **stream )
{
    if (!stream) return E_INVALIDARG;
    *stream = NULL;
    if (!inspection) return E_INVALIDARG;
    if (file_index >= inspection->file_count) return E_BOUNDS;
    return wine_appx_archive_stream_open(
        inspection->archive, inspection->files[file_index].archive_index, stream );
}
