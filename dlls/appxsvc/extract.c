/*
 * AppX package payload extraction
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
#include <stddef.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"
#include "winioctl.h"
#include "winternl.h"

#include "extract.h"
#include "wine/appxsvc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define APPX_EXTRACT_MAX_FILES 65536

#define EXTRACT_HASH_INITIAL_CAPACITY 64
#define EXTRACT_HASH_LOAD_NUMERATOR    3
#define EXTRACT_HASH_LOAD_DENOMINATOR  4
#define EXTRACT_INVALID_FILE_INDEX     (~0u)

enum extract_node_kind
{
    EXTRACT_NODE_DIRECTORY,
    EXTRACT_NODE_FILE
};

struct extract_file_identity
{
    DWORD volume_serial;
    DWORD index_high;
    DWORD index_low;
    BOOL valid;
};

struct extract_node
{
    struct extract_node *parent;
    WCHAR *name;
    UINT32 hash;
    UINT32 file_index;
    UINT16 name_length;
    enum extract_node_kind kind;
    HANDLE handle;
    struct extract_file_identity identity;
    BOOL created;
};

struct extract_plan
{
    struct extract_node **nodes;
    struct extract_node **files;
    struct extract_node **hash_table;
    UINT32 node_count;
    UINT32 node_capacity;
    UINT32 file_count;
    UINT32 hash_capacity;
    UINT64 expanded_size;
};

struct extract_cancel_monitor
{
    CRITICAL_SECTION cs;
    HANDLE source_event;
    HANDLE cancelled_event;
    HANDLE done_event;
    HANDLE thread;
    void *stream;
    APPX_EXTRACT_SOURCE_CANCEL_STREAM_CALLBACK cancel_stream;
    LONG cancelled;
    LONG wait_error;
    BOOL initialized;
};

struct extract_context
{
    const void *source;
    APPX_EXTRACT_TEST_SOURCE source_ops;
    APPX_EXTRACT_OPTIONS options;
    APPX_EXTRACT_TEST_IO io;
    void *validation;
    struct extract_cancel_monitor cancel;
    struct extract_plan plan;
    HANDLE root;
    HANDLE write_event;
    BYTE *buffer;
    UINT32 buffer_size;
    BOOL weak_durability;
};

static HRESULT win32_error( DWORD error )
{
    return error ? HRESULT_FROM_WIN32( error ) : E_FAIL;
}

static HRESULT ntstatus_error( NTSTATUS status )
{
    return win32_error( RtlNtStatusToDosError( status ) );
}

static HRESULT complete_nt_io( HANDLE handle, IO_STATUS_BLOCK *io,
                               NTSTATUS status, NTSTATUS *completed_status )
{
    if (status == STATUS_PENDING)
    {
        DWORD wait = WaitForSingleObject( handle, INFINITE );

        if (wait != WAIT_OBJECT_0)
        {
            if (completed_status) *completed_status = STATUS_PENDING;
            return wait == WAIT_FAILED ?
                   win32_error( GetLastError() ) : E_FAIL;
        }
        status = io->Status;
    }
    if (completed_status) *completed_status = status;
    return status ? ntstatus_error( status ) : S_OK;
}

static HRESULT malformed_package( void )
{
    return APPX_E_INVALID_PACKAGING_LAYOUT;
}

static BOOL add_size_t( SIZE_T left, SIZE_T right, SIZE_T *result )
{
    if (~(SIZE_T)0 - left < right) return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL multiply_size_t( SIZE_T left, SIZE_T right, SIZE_T *result )
{
    if (left && right > ~(SIZE_T)0 / left) return FALSE;
    *result = left * right;
    return TRUE;
}

static BOOL add_uint64( UINT64 left, UINT64 right, UINT64 *result )
{
    if (~(UINT64)0 - left < right) return FALSE;
    *result = left + right;
    return TRUE;
}

static HRESULT check_object_attributes( HANDLE handle, BOOL directory )
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

    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                      FILE_ATTRIBUTE_COMPRESSED |
                      FILE_ATTRIBUTE_ENCRYPTED |
                      FILE_ATTRIBUTE_SPARSE_FILE))
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

static HRESULT query_file_identity( HANDLE file,
                                    struct extract_file_identity *identity,
                                    DWORD *links )
{
    BY_HANDLE_FILE_INFORMATION info;

    memset( identity, 0, sizeof(*identity) );
    if (!GetFileInformationByHandle( file, &info ))
        return win32_error( GetLastError() );
    identity->volume_serial = info.dwVolumeSerialNumber;
    identity->index_high = info.nFileIndexHigh;
    identity->index_low = info.nFileIndexLow;
    identity->valid = TRUE;
    if (links) *links = info.nNumberOfLinks;
    return S_OK;
}

static BOOL same_file_identity( const struct extract_file_identity *left,
                                const struct extract_file_identity *right )
{
    return left->valid && right->valid &&
           left->volume_serial == right->volume_serial &&
           left->index_high == right->index_high &&
           left->index_low == right->index_low;
}

static HRESULT delete_open_object( HANDLE handle )
{
    FILE_DISPOSITION_INFORMATION disposition = {TRUE};
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    status = NtSetInformationFile( handle, &io, &disposition,
                                   sizeof(disposition),
                                   FileDispositionInformation );
    return complete_nt_io( handle, &io, status, NULL );
}

static HRESULT set_staging_file_attributes( HANDLE file )
{
    FILE_BASIC_INFORMATION basic;
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    memset( &basic, 0, sizeof(basic) );
    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    status = NtSetInformationFile( file, &io, &basic, sizeof(basic),
                                   FileBasicInformation );
    return complete_nt_io( file, &io, status, NULL );
}

static BOOL is_dot_name( const FILE_NAMES_INFORMATION *entry )
{
    UINT32 length = entry->FileNameLength / sizeof(WCHAR);

    return (length == 1 && entry->FileName[0] == '.') ||
           (length == 2 && entry->FileName[0] == '.' &&
                           entry->FileName[1] == '.');
}

static HRESULT check_directory_empty( HANDLE directory )
{
    BYTE buffer[offsetof(FILE_NAMES_INFORMATION, FileName) +
                (WINE_APPX_MAX_COMPONENT_CHARS + 1) * sizeof(WCHAR)];
    FILE_NAMES_INFORMATION *entry = (FILE_NAMES_INFORMATION *)buffer;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    BOOL restart = TRUE;
    HRESULT hr = S_OK;

    for (;;)
    {
        status = NtQueryDirectoryFile( directory, NULL, NULL, NULL, &io,
                                       buffer, sizeof(buffer),
                                       FileNamesInformation, TRUE, NULL,
                                       restart );
        restart = FALSE;
        if (status == STATUS_PENDING)
        {
            DWORD wait = WaitForSingleObject( directory, INFINITE );

            if (wait != WAIT_OBJECT_0)
            {
                hr = wait == WAIT_FAILED ?
                     win32_error( GetLastError() ) : E_FAIL;
                break;
            }
            status = io.Status;
        }
        if (status == STATUS_NO_MORE_FILES) break;
        if (status)
        {
            hr = ntstatus_error( status );
            break;
        }
        if (entry->FileNameLength % sizeof(WCHAR) ||
            entry->FileNameLength >
            WINE_APPX_MAX_COMPONENT_CHARS * sizeof(WCHAR))
        {
            hr = E_FAIL;
            break;
        }
        if (!is_dot_name( entry ))
        {
            hr = HRESULT_FROM_WIN32( ERROR_DIR_NOT_EMPTY );
            break;
        }
    }
    return hr;
}

static HRESULT duplicate_staging_root( HANDLE source, HANDLE *root )
{
    HRESULT hr;

    *root = INVALID_HANDLE_VALUE;
    if (!source || source == INVALID_HANDLE_VALUE) return E_INVALIDARG;
    if (!DuplicateHandle( GetCurrentProcess(), source, GetCurrentProcess(),
                          root, 0, FALSE, DUPLICATE_SAME_ACCESS ))
        return win32_error( GetLastError() );
    if (FAILED(hr = check_object_attributes( *root, TRUE )) ||
        FAILED(hr = check_directory_empty( *root )))
    {
        CloseHandle( *root );
        *root = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static DWORD WINAPI cancel_monitor_thread( void *parameter )
{
    struct extract_cancel_monitor *monitor = parameter;
    HANDLE handles[2] = {monitor->source_event, monitor->done_event};
    DWORD wait = WaitForMultipleObjects( ARRAY_SIZE(handles), handles,
                                         FALSE, INFINITE );

    if (wait == WAIT_OBJECT_0)
    {
        InterlockedExchange( &monitor->cancelled, 1 );
        SetEvent( monitor->cancelled_event );
        EnterCriticalSection( &monitor->cs );
        if (monitor->stream)
            monitor->cancel_stream( monitor->stream );
        LeaveCriticalSection( &monitor->cs );
    }
    else if (wait != WAIT_OBJECT_0 + 1)
    {
        DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;

        InterlockedExchange( &monitor->wait_error, error ? error :
                                                        ERROR_GEN_FAILURE );
        SetEvent( monitor->cancelled_event );
    }
    return 0;
}

static HRESULT cancel_monitor_init( struct extract_cancel_monitor *monitor,
                                    HANDLE source_event,
                                    APPX_EXTRACT_SOURCE_CANCEL_STREAM_CALLBACK
                                    cancel_stream )
{
    DWORD wait;
    HRESULT hr;

    memset( monitor, 0, sizeof(*monitor) );
    InitializeCriticalSection( &monitor->cs );
    monitor->initialized = TRUE;
    monitor->cancel_stream = cancel_stream;
    if (!source_event) return S_OK;
    if (source_event == INVALID_HANDLE_VALUE)
    {
        hr = E_INVALIDARG;
        goto failed;
    }
    if (!DuplicateHandle( GetCurrentProcess(), source_event,
                          GetCurrentProcess(), &monitor->source_event,
                          SYNCHRONIZE, FALSE, 0 ))
    {
        hr = win32_error( GetLastError() );
        goto failed;
    }
    if (!(monitor->cancelled_event =
          CreateEventW( NULL, TRUE, FALSE, NULL )) ||
        !(monitor->done_event = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        hr = win32_error( GetLastError() );
        goto failed;
    }

    wait = WaitForSingleObject( monitor->source_event, 0 );
    if (wait == WAIT_OBJECT_0)
    {
        InterlockedExchange( &monitor->cancelled, 1 );
        SetEvent( monitor->cancelled_event );
        return S_OK;
    }
    if (wait == WAIT_FAILED)
    {
        hr = win32_error( GetLastError() );
        goto failed;
    }
    if (!(monitor->thread = CreateThread( NULL, 0, cancel_monitor_thread,
                                          monitor, 0, NULL )))
    {
        hr = win32_error( GetLastError() );
        goto failed;
    }
    return S_OK;

failed:
    if (monitor->thread) CloseHandle( monitor->thread );
    if (monitor->done_event) CloseHandle( monitor->done_event );
    if (monitor->cancelled_event) CloseHandle( monitor->cancelled_event );
    if (monitor->source_event) CloseHandle( monitor->source_event );
    DeleteCriticalSection( &monitor->cs );
    memset( monitor, 0, sizeof(*monitor) );
    return hr;
}

static HRESULT cancel_monitor_status(
    const struct extract_cancel_monitor *monitor )
{
    LONG error = InterlockedCompareExchange(
        (LONG *)&monitor->wait_error, 0, 0 );

    if (error) return win32_error( error );
    if (InterlockedCompareExchange( (LONG *)&monitor->cancelled, 0, 0 ))
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    return S_OK;
}

static void cancel_monitor_set_stream(
    struct extract_cancel_monitor *monitor,
    void *stream )
{
    EnterCriticalSection( &monitor->cs );
    monitor->stream = stream;
    if (stream && InterlockedCompareExchange( &monitor->cancelled, 0, 0 ))
        monitor->cancel_stream( stream );
    LeaveCriticalSection( &monitor->cs );
}

static void cancel_monitor_clear_stream(
    struct extract_cancel_monitor *monitor,
    void *stream )
{
    EnterCriticalSection( &monitor->cs );
    if (monitor->stream == stream) monitor->stream = NULL;
    LeaveCriticalSection( &monitor->cs );
}

static void cancel_monitor_destroy( struct extract_cancel_monitor *monitor )
{
    if (!monitor->initialized) return;
    if (monitor->thread)
    {
        SetEvent( monitor->done_event );
        WaitForSingleObject( monitor->thread, INFINITE );
        CloseHandle( monitor->thread );
    }
    if (monitor->done_event) CloseHandle( monitor->done_event );
    if (monitor->cancelled_event) CloseHandle( monitor->cancelled_event );
    if (monitor->source_event) CloseHandle( monitor->source_event );
    DeleteCriticalSection( &monitor->cs );
    memset( monitor, 0, sizeof(*monitor) );
}

static UINT32 WINAPI package_source_get_count( const void *source )
{
    return appx_package_inspection_get_file_count( source );
}

static const APPX_PACKAGE_FILE *WINAPI package_source_get_file(
    const void *source, UINT32 index )
{
    return appx_package_inspection_get_file( source, index );
}

static UINT64 WINAPI package_source_get_expanded_size( const void *source )
{
    return appx_package_inspection_get_expanded_size( source );
}

static HRESULT WINAPI package_source_open_stream(
    const void *source, UINT32 index, void **result )
{
    WINE_APPX_ARCHIVE_STREAM *stream = NULL;
    HRESULT hr;

    if (!result) return E_INVALIDARG;
    *result = NULL;
    hr = appx_package_inspection_open_validation_stream(
        source, index, &stream );
    if (SUCCEEDED(hr)) *result = stream;
    return hr;
}

static HRESULT WINAPI package_source_read_stream(
    void *stream, void *buffer, UINT32 capacity, UINT32 *read )
{
    return wine_appx_archive_stream_read( stream, buffer, capacity, read );
}

static void WINAPI package_source_cancel_stream( void *stream )
{
    wine_appx_archive_stream_cancel( stream );
}

static void WINAPI package_source_close_stream( void *stream )
{
    wine_appx_archive_stream_close( stream );
}

static HRESULT WINAPI package_source_open_validation(
    const void *source, void **result )
{
    APPX_PACKAGE_VALIDATION *validation = NULL;
    HRESULT hr;

    if (!result) return E_INVALIDARG;
    *result = NULL;
    hr = appx_package_inspection_open_validation( source, &validation );
    if (SUCCEEDED(hr)) *result = validation;
    return hr;
}

static HRESULT WINAPI package_source_begin_file(
    void *validation, UINT32 file_index )
{
    return appx_package_validation_begin_file( validation, file_index );
}

static HRESULT WINAPI package_source_validate_data(
    void *validation, const void *data, UINT32 size )
{
    return appx_package_validation_update( validation, data, size );
}

static HRESULT WINAPI package_source_finish_file( void *validation )
{
    return appx_package_validation_finish_file( validation );
}

static void WINAPI package_source_close_validation( void *validation )
{
    appx_package_validation_close( validation );
}

static const APPX_EXTRACT_TEST_SOURCE package_source_ops =
{
    sizeof(package_source_ops),
    package_source_get_count,
    package_source_get_file,
    package_source_get_expanded_size,
    package_source_open_stream,
    package_source_read_stream,
    package_source_cancel_stream,
    package_source_close_stream,
    package_source_open_validation,
    package_source_begin_file,
    package_source_validate_data,
    package_source_finish_file,
    package_source_close_validation
};

static HRESULT validate_canonical_path( const WCHAR *path )
{
    BYTE *utf8 = NULL, *escaped = NULL;
    WCHAR *canonical = NULL;
    UINT32 chars, capacity = 0, escaped_size, i, j, percent_count = 0;
    int utf8_size;
    HRESULT hr;

    if (!path) return malformed_package();
    for (chars = 0; chars < WINE_APPX_MAX_PATH_CHARS; chars++)
        if (!path[chars]) break;
    if (!chars || chars == WINE_APPX_MAX_PATH_CHARS)
        return malformed_package();

    utf8_size = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, path,
                                    chars, NULL, 0, NULL, NULL );
    if (!utf8_size || utf8_size > WINE_APPX_MAX_ENTRY_NAME_BYTES)
        return malformed_package();
    if (!(utf8 = HeapAlloc( GetProcessHeap(), 0, utf8_size )))
        return E_OUTOFMEMORY;
    if (WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, path, chars,
                             (char *)utf8, utf8_size, NULL, NULL ) != utf8_size)
    {
        hr = malformed_package();
        goto done;
    }
    for (i = 0; i < utf8_size; i++)
        if (utf8[i] == '%') percent_count++;
    if (percent_count >
        (WINE_APPX_MAX_ENTRY_NAME_BYTES - (UINT32)utf8_size) / 2)
    {
        hr = malformed_package();
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
        capacity != chars + 1)
    {
        if (hr != E_OUTOFMEMORY) hr = malformed_package();
        goto done;
    }
    if (!(canonical = HeapAlloc( GetProcessHeap(), 0,
                                 (SIZE_T)capacity * sizeof(*canonical) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    hr = wine_appx_validate_archive_path( escaped, escaped_size, 0,
                                          &capacity, canonical );
    if (FAILED(hr) || capacity != chars + 1 || lstrcmpW( path, canonical ))
        hr = hr == E_OUTOFMEMORY ? hr : malformed_package();

done:
    HeapFree( GetProcessHeap(), 0, canonical );
    HeapFree( GetProcessHeap(), 0, escaped );
    HeapFree( GetProcessHeap(), 0, utf8 );
    return hr;
}

static UINT32 hash_node_key( struct extract_node *parent, UINT32 name_hash )
{
    ULONG_PTR value = (ULONG_PTR)parent;
    UINT32 parent_hash;

#ifdef _WIN64
    value ^= value >> 32;
#endif
    parent_hash = (UINT32)value;
    parent_hash ^= parent_hash >> 16;
    parent_hash *= 0x7feb352d;
    parent_hash ^= parent_hash >> 15;
    return name_hash ^ parent_hash;
}

static BOOL node_name_equal( const struct extract_node *node,
                             const WCHAR *name, UINT16 name_length )
{
    return node->name_length == name_length &&
           CompareStringOrdinal( node->name, node->name_length,
                                 name, name_length, TRUE ) == CSTR_EQUAL;
}

static HRESULT plan_resize_hash( struct extract_plan *plan,
                                 UINT32 new_capacity )
{
    struct extract_node **table;
    SIZE_T size;
    UINT32 i;

    if (!new_capacity ||
        !multiply_size_t( new_capacity, sizeof(*table), &size ))
        return E_OUTOFMEMORY;
    if (!(table = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size )))
        return E_OUTOFMEMORY;

    for (i = 0; i < plan->node_count; i++)
    {
        struct extract_node *node = plan->nodes[i];
        UINT32 slot = node->hash & (new_capacity - 1);

        while (table[slot]) slot = (slot + 1) & (new_capacity - 1);
        table[slot] = node;
    }
    HeapFree( GetProcessHeap(), 0, plan->hash_table );
    plan->hash_table = table;
    plan->hash_capacity = new_capacity;
    return S_OK;
}

static HRESULT plan_ensure_hash_capacity( struct extract_plan *plan )
{
    UINT32 capacity;

    if (plan->hash_capacity &&
        plan->node_count + 1 <=
        plan->hash_capacity * EXTRACT_HASH_LOAD_NUMERATOR /
        EXTRACT_HASH_LOAD_DENOMINATOR)
        return S_OK;
    capacity = plan->hash_capacity ? plan->hash_capacity * 2 :
                                     EXTRACT_HASH_INITIAL_CAPACITY;
    if (capacity < plan->hash_capacity) return E_OUTOFMEMORY;
    return plan_resize_hash( plan, capacity );
}

static HRESULT plan_append_node( struct extract_plan *plan,
                                 struct extract_node *node )
{
    struct extract_node **nodes;
    UINT32 capacity;
    SIZE_T size;

    if (plan->node_count == plan->node_capacity)
    {
        capacity = plan->node_capacity ? plan->node_capacity * 2 : 64;
        if (capacity < plan->node_capacity ||
            !multiply_size_t( capacity, sizeof(*nodes), &size ))
            return E_OUTOFMEMORY;
        if (plan->nodes)
            nodes = HeapReAlloc( GetProcessHeap(), 0, plan->nodes, size );
        else
            nodes = HeapAlloc( GetProcessHeap(), 0, size );
        if (!nodes) return E_OUTOFMEMORY;
        plan->nodes = nodes;
        plan->node_capacity = capacity;
    }
    plan->nodes[plan->node_count++] = node;
    return S_OK;
}

static HRESULT plan_find_or_add_node(
    struct extract_plan *plan, struct extract_node *parent,
    const WCHAR *name, UINT16 name_length, enum extract_node_kind kind,
    UINT32 file_index, struct extract_node **result, BOOL *created )
{
    struct extract_node *node;
    UNICODE_STRING string;
    ULONG name_hash;
    UINT32 hash, slot;
    SIZE_T name_bytes, allocation;
    NTSTATUS status;
    HRESULT hr;

    *result = NULL;
    *created = FALSE;
    string.Buffer = (WCHAR *)name;
    string.Length = name_length * sizeof(WCHAR);
    string.MaximumLength = string.Length;
    status = RtlHashUnicodeString( &string, TRUE,
                                   HASH_STRING_ALGORITHM_X65599,
                                   &name_hash );
    if (status) return ntstatus_error( status );
    hash = hash_node_key( parent, name_hash );

    if (FAILED(hr = plan_ensure_hash_capacity( plan ))) return hr;
    slot = hash & (plan->hash_capacity - 1);
    while ((node = plan->hash_table[slot]))
    {
        if (node->hash == hash && node->parent == parent &&
            node_name_equal( node, name, name_length ))
        {
            *result = node;
            return S_OK;
        }
        slot = (slot + 1) & (plan->hash_capacity - 1);
    }

    if (!multiply_size_t( (SIZE_T)name_length + 1, sizeof(WCHAR),
                          &name_bytes ) ||
        !add_size_t( sizeof(*node), name_bytes, &allocation ) ||
        !(node = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                            allocation )))
        return E_OUTOFMEMORY;
    node->name = (WCHAR *)(node + 1);
    memcpy( node->name, name, name_length * sizeof(WCHAR) );
    node->name[name_length] = 0;
    node->parent = parent;
    node->name_length = name_length;
    node->kind = kind;
    node->file_index = file_index;
    node->hash = hash;
    node->handle = INVALID_HANDLE_VALUE;
    if (FAILED(hr = plan_append_node( plan, node )))
    {
        HeapFree( GetProcessHeap(), 0, node );
        return hr;
    }
    plan->hash_table[slot] = node;
    *result = node;
    *created = TRUE;
    return S_OK;
}

static void plan_free( struct extract_plan *plan )
{
    UINT32 i;

    if (!plan) return;
    for (i = 0; i < plan->node_count; i++)
    {
        if (plan->nodes[i]->handle != INVALID_HANDLE_VALUE)
            CloseHandle( plan->nodes[i]->handle );
        HeapFree( GetProcessHeap(), 0, plan->nodes[i] );
    }
    HeapFree( GetProcessHeap(), 0, plan->hash_table );
    HeapFree( GetProcessHeap(), 0, plan->files );
    HeapFree( GetProcessHeap(), 0, plan->nodes );
    memset( plan, 0, sizeof(*plan) );
}

static HRESULT plan_add_file( struct extract_plan *plan, const WCHAR *path,
                              UINT32 file_index )
{
    const WCHAR *component = path, *cursor = path;
    struct extract_node *parent = NULL, *node;
    BOOL created;
    HRESULT hr;

    for (;; cursor++)
    {
        UINT32 length;
        BOOL leaf;

        if (*cursor && *cursor != '\\') continue;
        length = cursor - component;
        leaf = !*cursor;
        if (!length || length > WINE_APPX_MAX_COMPONENT_CHARS)
            return malformed_package();
        if (FAILED(hr = plan_find_or_add_node(
                plan, parent, component, length,
                leaf ? EXTRACT_NODE_FILE : EXTRACT_NODE_DIRECTORY,
                leaf ? file_index : EXTRACT_INVALID_FILE_INDEX,
                &node, &created )))
            return hr;

        /*
         * A leaf must be new.  An ancestor may be reused only if it is a
         * directory.  This catches case-folded duplicates and file/directory
         * prefix collisions before the staging root is touched.
         */
        if ((leaf && !created) ||
            (!leaf && node->kind != EXTRACT_NODE_DIRECTORY))
            return malformed_package();
        if (leaf)
        {
            plan->files[file_index] = node;
            return S_OK;
        }
        parent = node;
        component = cursor + 1;
    }
}

static HRESULT build_extract_plan( struct extract_context *context )
{
    struct extract_plan *plan = &context->plan;
    UINT64 reported_size, total = 0;
    SIZE_T allocation;
    UINT32 count, i;
    HRESULT hr;

    count = context->source_ops.get_count( context->source );
    if (!count || count > APPX_EXTRACT_MAX_FILES)
        return malformed_package();
    if (!multiply_size_t( count, sizeof(*plan->files), &allocation ) ||
        !(plan->files = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   allocation )))
        return E_OUTOFMEMORY;
    plan->file_count = count;

    for (i = 0; i < count; i++)
    {
        const APPX_PACKAGE_FILE *file;

        if (FAILED(hr = cancel_monitor_status( &context->cancel )))
            return hr;
        if (!(file = context->source_ops.get_file(
              context->source, i )) ||
            (file->compression_method != 0 &&
             file->compression_method != 8))
            return malformed_package();
        if (FAILED(hr = validate_canonical_path( file->path )))
            return hr;
        if (!add_uint64( total, file->uncompressed_size, &total ))
            return malformed_package();
        if (FAILED(hr = plan_add_file( plan, file->path, i )))
            return hr;
    }

    reported_size = context->source_ops.get_expanded_size(
        context->source );
    if (reported_size != total) return malformed_package();
    plan->expanded_size = total;
    return S_OK;
}

static HRESULT create_child( HANDLE parent, const WCHAR *name,
                             BOOL directory, HANDLE *handle )
{
    UNICODE_STRING child_name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    ACCESS_MASK access;
    ULONG options;
    NTSTATUS status;
    HRESULT hr;

    *handle = INVALID_HANDLE_VALUE;
    RtlInitUnicodeString( &child_name, name );
    InitializeObjectAttributes( &attributes, &child_name,
                                OBJ_CASE_INSENSITIVE, parent, NULL );
    if (directory)
    {
        access = FILE_LIST_DIRECTORY | FILE_ADD_FILE |
                 FILE_ADD_SUBDIRECTORY | FILE_TRAVERSE |
                 FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                 DELETE | SYNCHRONIZE;
        options = FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                  FILE_OPEN_REPARSE_POINT;
    }
    else
    {
        access = FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                 FILE_WRITE_ATTRIBUTES | DELETE | SYNCHRONIZE;
        options = FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY |
                  FILE_OPEN_REPARSE_POINT;
    }
    status = NtCreateFile( handle, access, &attributes, &io, NULL,
                           directory ? FILE_ATTRIBUTE_DIRECTORY :
                                       FILE_ATTRIBUTE_NORMAL,
                           0, FILE_CREATE, options, NULL, 0 );
    if (status) return ntstatus_error( status );
    if (FAILED(hr = check_object_attributes( *handle, directory )))
    {
        delete_open_object( *handle );
        CloseHandle( *handle );
        *handle = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static HRESULT open_existing_file( const struct extract_node *node,
                                   HANDLE root, HANDLE *file )
{
    UNICODE_STRING child_name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    HANDLE parent = node->parent ? node->parent->handle : root;
    NTSTATUS status;
    HRESULT hr;

    *file = INVALID_HANDLE_VALUE;
    RtlInitUnicodeString( &child_name, node->name );
    InitializeObjectAttributes( &attributes, &child_name,
                                OBJ_CASE_INSENSITIVE, parent, NULL );
    status = NtCreateFile(
        file, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
              SYNCHRONIZE,
        &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN, FILE_NON_DIRECTORY_FILE |
                   FILE_SYNCHRONOUS_IO_NONALERT |
                   FILE_OPEN_REPARSE_POINT, NULL, 0 );
    if (status) return ntstatus_error( status );
    if (FAILED(hr = check_object_attributes( *file, FALSE )))
    {
        CloseHandle( *file );
        *file = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static HRESULT preallocate_file( HANDLE file, UINT64 size )
{
    FILE_ALLOCATION_INFORMATION allocation;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    HRESULT hr;

    if (!size) return S_OK;
    if (size > 0x7fffffffffffffffULL) return malformed_package();
    allocation.AllocationSize.QuadPart = size;
    status = NtSetInformationFile( file, &io, &allocation,
                                   sizeof(allocation),
                                   FileAllocationInformation );
    hr = complete_nt_io( file, &io, status, &status );
    if (SUCCEEDED(hr)) return S_OK;
    if (status == STATUS_INVALID_DEVICE_REQUEST ||
        status == STATUS_INVALID_INFO_CLASS ||
        status == STATUS_NOT_IMPLEMENTED ||
        status == STATUS_NOT_SUPPORTED)
        return S_OK;
    return hr;
}

static HRESULT WINAPI default_query_available_bytes( HANDLE directory,
                                                     UINT64 *available )
{
    FILE_FS_FULL_SIZE_INFORMATION full;
    FILE_FS_SIZE_INFORMATION size;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER units;
    UINT64 bytes_per_unit;
    ULONG sectors, bytes;
    NTSTATUS status;
    HRESULT hr;

    *available = 0;
    status = NtQueryVolumeInformationFile(
        directory, &io, &full, sizeof(full),
        FileFsFullSizeInformation );
    hr = complete_nt_io( directory, &io, status, &status );
    if (SUCCEEDED(hr))
    {
        units = full.CallerAvailableAllocationUnits;
        sectors = full.SectorsPerAllocationUnit;
        bytes = full.BytesPerSector;
    }
    else
    {
        if (status != STATUS_INVALID_DEVICE_REQUEST &&
            status != STATUS_INVALID_INFO_CLASS &&
            status != STATUS_NOT_IMPLEMENTED &&
            status != STATUS_NOT_SUPPORTED)
            return hr;
        status = NtQueryVolumeInformationFile(
            directory, &io, &size, sizeof(size),
            FileFsSizeInformation );
        if (FAILED(hr = complete_nt_io(
              directory, &io, status, &status )))
            return hr;
        units = size.AvailableAllocationUnits;
        sectors = size.SectorsPerAllocationUnit;
        bytes = size.BytesPerSector;
    }
    if (units.QuadPart < 0 || !sectors || !bytes ||
        sectors > ~(UINT64)0 / bytes)
        return E_FAIL;
    bytes_per_unit = (UINT64)sectors * bytes;
    if ((UINT64)units.QuadPart > ~(UINT64)0 / bytes_per_unit)
        *available = ~(UINT64)0;
    else
        *available = (UINT64)units.QuadPart * bytes_per_unit;
    return S_OK;
}

static HRESULT WINAPI default_flush_file( HANDLE file )
{
    return FlushFileBuffers( file ) ? S_OK :
                                      win32_error( GetLastError() );
}

static HRESULT WINAPI default_flush_directory( HANDLE directory )
{
    IO_STATUS_BLOCK io;
    NTSTATUS status = NtFlushBuffersFile( directory, &io );
    HRESULT hr;

    hr = complete_nt_io( directory, &io, status, &status );
    if (SUCCEEDED(hr)) return S_OK;
    if (status == STATUS_INVALID_DEVICE_REQUEST ||
        status == STATUS_NOT_IMPLEMENTED ||
        status == STATUS_NOT_SUPPORTED)
        return APPX_EXTRACT_S_WEAK_DURABILITY;
    return hr;
}

static BOOL WINAPI default_write_file( HANDLE file, const void *buffer,
                                       DWORD size, DWORD *written,
                                       OVERLAPPED *overlapped )
{
    return WriteFile( file, buffer, size, written, overlapped );
}

static HRESULT query_available_bytes( struct extract_context *context,
                                      UINT64 *available )
{
    APPX_EXTRACT_QUERY_SPACE_CALLBACK callback =
        context->io.query_available_bytes ?
        context->io.query_available_bytes : default_query_available_bytes;

    return callback( context->root, available );
}

static HRESULT check_initial_space( struct extract_context *context )
{
    UINT64 required, available;
    HRESULT hr;

    if (context->options.max_expanded_bytes &&
        context->plan.expanded_size >
        context->options.max_expanded_bytes)
        return HRESULT_FROM_WIN32( ERROR_NOT_ENOUGH_QUOTA );
    if (!add_uint64( context->plan.expanded_size,
                     context->options.free_space_floor_bytes,
                     &required ))
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    if (FAILED(hr = query_available_bytes( context, &available )))
        return hr;
    if (available < required)
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    return S_OK;
}

static HRESULT check_final_space( struct extract_context *context )
{
    UINT64 available;
    HRESULT hr;

    if (FAILED(hr = query_available_bytes( context, &available )))
        return hr;
    if (available < context->options.free_space_floor_bytes)
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    return S_OK;
}

static HRESULT write_file_all( struct extract_context *context, HANDLE file,
                               UINT64 offset, const BYTE *buffer, UINT32 size )
{
    APPX_EXTRACT_WRITE_FILE_CALLBACK callback =
        context->io.write_file ? context->io.write_file :
                                 default_write_file;
    UINT32 cursor = 0;

    while (cursor < size)
    {
        OVERLAPPED overlapped;
        HANDLE handles[2];
        DWORD written = 0, error, wait;
        BOOL success;
        HRESULT hr;

        if (FAILED(hr = cancel_monitor_status( &context->cancel )))
            return hr;
        memset( &overlapped, 0, sizeof(overlapped) );
        overlapped.Offset = offset + cursor;
        overlapped.OffsetHigh = (offset + cursor) >> 32;
        overlapped.hEvent = context->write_event;
        ResetEvent( context->write_event );
        success = callback( file, buffer + cursor, size - cursor,
                            &written, &overlapped );
        if (!success)
        {
            error = GetLastError();
            if (error != ERROR_IO_PENDING) return win32_error( error );

            if (context->cancel.cancelled_event)
            {
                handles[0] = context->cancel.cancelled_event;
                handles[1] = context->write_event;
                wait = WaitForMultipleObjects( ARRAY_SIZE(handles), handles,
                                               FALSE, INFINITE );
                if (wait == WAIT_OBJECT_0)
                {
                    CancelIoEx( file, &overlapped );
                    GetOverlappedResult( file, &overlapped, &written, TRUE );
                    return cancel_monitor_status( &context->cancel );
                }
                if (wait != WAIT_OBJECT_0 + 1)
                {
                    error = wait == WAIT_FAILED ? GetLastError() :
                                                 ERROR_GEN_FAILURE;
                    CancelIoEx( file, &overlapped );
                    GetOverlappedResult( file, &overlapped, &written, TRUE );
                    return win32_error( error );
                }
            }
            else
            {
                wait = WaitForSingleObject( context->write_event, INFINITE );
                if (wait != WAIT_OBJECT_0)
                {
                    error = wait == WAIT_FAILED ? GetLastError() :
                                                 ERROR_GEN_FAILURE;
                    CancelIoEx( file, &overlapped );
                    GetOverlappedResult( file, &overlapped, &written, TRUE );
                    return win32_error( error );
                }
            }
            if (!GetOverlappedResult( file, &overlapped, &written, FALSE ))
            {
                error = GetLastError();
                if (error == ERROR_OPERATION_ABORTED &&
                    FAILED(hr = cancel_monitor_status( &context->cancel )))
                    return hr;
                return win32_error( error );
            }
        }
        if (!written || written > size - cursor)
            return HRESULT_FROM_WIN32( ERROR_WRITE_FAULT );
        cursor += written;
        if (FAILED(hr = cancel_monitor_status( &context->cancel )))
            return hr;
    }
    return S_OK;
}

static HRESULT extract_file( struct extract_context *context,
                             struct extract_node *node )
{
    const APPX_PACKAGE_FILE *file_info;
    void *stream = NULL;
    HANDLE parent, file = INVALID_HANDLE_VALUE;
    UINT64 total = 0;
    LARGE_INTEGER size;
    DWORD links;
    HRESULT delete_hr, hr;

    if (!(file_info = context->source_ops.get_file(
          context->source, node->file_index )))
        return malformed_package();
    if (FAILED(hr = context->source_ops.begin_file(
          context->validation, node->file_index )))
        return hr;
    parent = node->parent ? node->parent->handle : context->root;
    if (FAILED(hr = create_child( parent, node->name, FALSE, &file )))
        return hr;
    node->created = TRUE;
    if (FAILED(hr = query_file_identity( file, &node->identity, &links )))
        goto failed;
    if (links != 1)
    {
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
        goto failed;
    }
    if (FAILED(hr = preallocate_file( file,
                                      file_info->uncompressed_size )))
        goto failed;
    if (FAILED(hr = context->source_ops.open_stream(
          context->source, node->file_index, &stream )))
        goto failed;
    if (!stream)
    {
        hr = malformed_package();
        goto failed;
    }
    cancel_monitor_set_stream( &context->cancel, stream );

    for (;;)
    {
        UINT32 read = 0;

        if (FAILED(hr = cancel_monitor_status( &context->cancel )))
            goto failed_stream;
        hr = context->source_ops.read_stream(
            stream, context->buffer, context->buffer_size, &read );
        if (hr == S_FALSE)
        {
            if (read || total != file_info->uncompressed_size)
                hr = APPX_E_CORRUPT_CONTENT;
            else
                hr = context->source_ops.finish_file(
                    context->validation );
            break;
        }
        if (FAILED(hr)) break;
        if (hr != S_OK || !read || read > context->buffer_size ||
            total > file_info->uncompressed_size ||
            read > file_info->uncompressed_size - total)
        {
            hr = APPX_E_CORRUPT_CONTENT;
            break;
        }
        if (FAILED(hr = context->source_ops.validate_data(
              context->validation, context->buffer, read )))
            break;
        if (FAILED(hr = write_file_all( context, file, total,
                                        context->buffer, read )))
            break;
        total += read;
    }

failed_stream:
    cancel_monitor_clear_stream( &context->cancel, stream );
    context->source_ops.close_stream( stream );
    stream = NULL;
    if (FAILED(hr)) goto failed;
    if (FAILED(hr = cancel_monitor_status( &context->cancel )))
        goto failed;
    if (!GetFileSizeEx( file, &size ))
    {
        hr = win32_error( GetLastError() );
        goto failed;
    }
    if (size.QuadPart < 0 ||
        (UINT64)size.QuadPart != file_info->uncompressed_size)
    {
        hr = HRESULT_FROM_WIN32( ERROR_WRITE_FAULT );
        goto failed;
    }
    if (FAILED(hr = set_staging_file_attributes( file )))
        goto failed;
    if (FAILED(hr = check_object_attributes( file, FALSE )))
        goto failed;
    hr = context->io.flush_file ?
         context->io.flush_file( file ) : default_flush_file( file );
    if (hr != S_OK)
    {
        if (SUCCEEDED(hr)) hr = E_FAIL;
        goto failed;
    }
    if (FAILED(hr = query_file_identity( file, &node->identity, &links )))
        goto failed;
    if (links != 1)
    {
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
        goto failed;
    }
    CloseHandle( file );
    return S_OK;

failed:
    if (stream)
    {
        cancel_monitor_clear_stream( &context->cancel, stream );
        context->source_ops.close_stream( stream );
    }
    if (file != INVALID_HANDLE_VALUE)
    {
        set_staging_file_attributes( file );
        delete_hr = delete_open_object( file );
        CloseHandle( file );
        if (SUCCEEDED(delete_hr)) node->created = FALSE;
        else
            WARN( "Failed to discard incomplete file %s, hr %#lx.\n",
                  debugstr_w(node->name), delete_hr );
    }
    return hr;
}

static HRESULT create_directories( struct extract_context *context )
{
    UINT32 i;
    HRESULT hr;

    for (i = 0; i < context->plan.node_count; i++)
    {
        struct extract_node *node = context->plan.nodes[i];
        HANDLE parent;

        if (node->kind != EXTRACT_NODE_DIRECTORY) continue;
        if (FAILED(hr = cancel_monitor_status( &context->cancel )))
            return hr;
        parent = node->parent ? node->parent->handle : context->root;
        if (FAILED(hr = create_child( parent, node->name, TRUE,
                                      &node->handle )))
            return hr;
        node->created = TRUE;
    }
    return S_OK;
}

static HRESULT extract_files( struct extract_context *context )
{
    UINT32 i;
    HRESULT hr;

    for (i = 0; i < context->plan.file_count; i++)
    {
        if (FAILED(hr = cancel_monitor_status( &context->cancel )))
            return hr;
        if (FAILED(hr = extract_file( context, context->plan.files[i] )))
            return hr;
    }
    return S_OK;
}

static HRESULT flush_directories( struct extract_context *context )
{
    APPX_EXTRACT_FLUSH_CALLBACK callback =
        context->io.flush_directory ? context->io.flush_directory :
                                      default_flush_directory;
    UINT32 i = context->plan.node_count;
    HRESULT hr;

    while (i--)
    {
        struct extract_node *node = context->plan.nodes[i];

        if (node->kind != EXTRACT_NODE_DIRECTORY) continue;
        if (FAILED(hr = cancel_monitor_status( &context->cancel )))
            return hr;
        hr = callback( node->handle );
        if (FAILED(hr)) return hr;
        if (hr == APPX_EXTRACT_S_WEAK_DURABILITY)
            context->weak_durability = TRUE;
        else if (hr != S_OK)
            return E_FAIL;
    }
    if (FAILED(hr = cancel_monitor_status( &context->cancel )))
        return hr;
    hr = callback( context->root );
    if (FAILED(hr)) return hr;
    if (hr == APPX_EXTRACT_S_WEAK_DURABILITY)
        context->weak_durability = TRUE;
    else if (hr != S_OK)
        return E_FAIL;
    return S_OK;
}

static void discard_completed_file( struct extract_context *context,
                                    struct extract_node *node )
{
    struct extract_file_identity identity;
    HANDLE file;
    HRESULT hr;

    if (!node->created) return;
    if (FAILED(hr = open_existing_file( node, context->root, &file )))
    {
        WARN( "Failed to reopen staged file %s for cleanup, hr %#lx.\n",
              debugstr_w(node->name), hr );
        return;
    }
    if (FAILED(hr = query_file_identity( file, &identity, NULL )) ||
        !same_file_identity( &identity, &node->identity ))
    {
        WARN( "Refusing to delete a replaced staged file %s.\n",
              debugstr_w(node->name) );
        CloseHandle( file );
        return;
    }
    set_staging_file_attributes( file );
    hr = delete_open_object( file );
    CloseHandle( file );
    if (FAILED(hr))
        WARN( "Failed to delete staged file %s, hr %#lx.\n",
              debugstr_w(node->name), hr );
    else
        node->created = FALSE;
}

static void close_plan_handles( struct extract_context *context,
                                BOOL discard )
{
    UINT32 i = context->plan.node_count;

    while (i--)
    {
        struct extract_node *node = context->plan.nodes[i];

        if (node->kind == EXTRACT_NODE_FILE)
        {
            if (discard) discard_completed_file( context, node );
            continue;
        }
        if (node->handle == INVALID_HANDLE_VALUE) continue;
        if (discard && node->created)
        {
            HRESULT hr = delete_open_object( node->handle );

            if (FAILED(hr))
                WARN( "Failed to delete staged directory %s, hr %#lx.\n",
                      debugstr_w(node->name), hr );
            else
                node->created = FALSE;
        }
        CloseHandle( node->handle );
        node->handle = INVALID_HANDLE_VALUE;
    }
    if (discard && context->root != INVALID_HANDLE_VALUE)
    {
        APPX_EXTRACT_FLUSH_CALLBACK callback =
            context->io.flush_directory ? context->io.flush_directory :
                                          default_flush_directory;
        callback( context->root );
    }
}

static HRESULT validate_options( const APPX_EXTRACT_OPTIONS *options,
                                 APPX_EXTRACT_OPTIONS *copy )
{
    memset( copy, 0, sizeof(*copy) );
    copy->size = sizeof(*copy);
    if (!options) return S_OK;
    if (options->size != sizeof(*options) || options->flags ||
        options->reserved ||
        options->io_buffer_size > APPX_EXTRACT_MAX_BUFFER_SIZE)
        return E_INVALIDARG;
    *copy = *options;
    return S_OK;
}

static HRESULT validate_test_io( const APPX_EXTRACT_TEST_IO *test_io,
                                 APPX_EXTRACT_TEST_IO *copy )
{
    memset( copy, 0, sizeof(*copy) );
    copy->size = sizeof(*copy);
    if (!test_io) return S_OK;
    if (test_io->size != sizeof(*test_io)) return E_INVALIDARG;
    *copy = *test_io;
    return S_OK;
}

static HRESULT validate_source_ops(
    const APPX_EXTRACT_TEST_SOURCE *source_ops,
    APPX_EXTRACT_TEST_SOURCE *copy )
{
    if (!source_ops || source_ops->size != sizeof(*source_ops) ||
        !source_ops->get_count || !source_ops->get_file ||
        !source_ops->get_expanded_size || !source_ops->open_stream ||
        !source_ops->read_stream || !source_ops->cancel_stream ||
        !source_ops->close_stream || !source_ops->open_validation ||
        !source_ops->begin_file || !source_ops->validate_data ||
        !source_ops->finish_file || !source_ops->close_validation)
        return E_INVALIDARG;
    *copy = *source_ops;
    return S_OK;
}

HRESULT WINAPI appx_package_extract_with_test_source(
    const void *source, const APPX_EXTRACT_TEST_SOURCE *source_ops,
    HANDLE staging_root, const APPX_EXTRACT_OPTIONS *options,
    const APPX_EXTRACT_TEST_IO *test_io )
{
    struct extract_context context;
    BOOL discard = TRUE;
    HRESULT hr;

    TRACE( "source %p, source_ops %p, staging_root %p, options %p, "
           "test_io %p.\n", source, source_ops, staging_root, options,
           test_io );

    memset( &context, 0, sizeof(context) );
    context.source = source;
    context.root = INVALID_HANDLE_VALUE;
    if (!source) return E_INVALIDARG;
    if (FAILED(hr = validate_source_ops( source_ops,
                                         &context.source_ops )) ||
        FAILED(hr = validate_options( options, &context.options )) ||
        FAILED(hr = validate_test_io( test_io, &context.io )))
        return hr;
    context.buffer_size = context.options.io_buffer_size ?
                          context.options.io_buffer_size :
                          APPX_EXTRACT_DEFAULT_BUFFER_SIZE;
    if (FAILED(hr = cancel_monitor_init( &context.cancel,
                                         context.options.cancel_event,
                                         context.source_ops.cancel_stream )))
        goto done;
    if (FAILED(hr = cancel_monitor_status( &context.cancel )))
        goto done;
    if (FAILED(hr = build_extract_plan( &context )))
        goto done;
    if (FAILED(hr = context.source_ops.open_validation(
          context.source, &context.validation )))
        goto done;
    if (!context.validation)
    {
        hr = malformed_package();
        goto done;
    }
    if (FAILED(hr = duplicate_staging_root( staging_root, &context.root )))
        goto done;
    if (FAILED(hr = check_initial_space( &context )))
        goto done;
    if (!(context.buffer = HeapAlloc( GetProcessHeap(), 0,
                                      context.buffer_size )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!(context.write_event = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        hr = win32_error( GetLastError() );
        goto done;
    }
    if (FAILED(hr = create_directories( &context )) ||
        FAILED(hr = extract_files( &context )) ||
        FAILED(hr = flush_directories( &context )) ||
        FAILED(hr = check_final_space( &context )) ||
        FAILED(hr = cancel_monitor_status( &context.cancel )))
        goto done;
    discard = FALSE;
    hr = context.weak_durability ?
         APPX_EXTRACT_S_WEAK_DURABILITY : S_OK;

done:
    /*
     * Stop the only helper thread before freeing its synchronization state or
     * closing any file/directory handle.  Its stream pointer was cleared by
     * extract_file() before every stream close.
     */
    cancel_monitor_destroy( &context.cancel );
    close_plan_handles( &context, discard );
    if (context.validation)
        context.source_ops.close_validation( context.validation );
    if (context.write_event) CloseHandle( context.write_event );
    HeapFree( GetProcessHeap(), 0, context.buffer );
    if (context.root != INVALID_HANDLE_VALUE) CloseHandle( context.root );
    plan_free( &context.plan );
    return hr;
}

HRESULT WINAPI appx_package_extract( const APPX_PACKAGE_INSPECTION *inspection,
                                     HANDLE staging_root,
                                     const APPX_EXTRACT_OPTIONS *options )
{
    return appx_package_extract_with_test_source(
        inspection, &package_source_ops, staging_root, options, NULL );
}
