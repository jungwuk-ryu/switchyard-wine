/*
 * AppX package payload extraction tests
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
#include "wincon.h"
#include "winerror.h"
#include "winternl.h"

#include "wine/test.h"

#include "../extract.h"

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

#define TEST_MAX_FILES 8
#define TEST_SPACE_UNLIMITED (~(UINT64)0)
#define TEST_WAIT_MS 5000

enum fake_stream_mode
{
    FAKE_STREAM_NORMAL,
    FAKE_STREAM_EARLY_TERMINAL,
    FAKE_STREAM_ZERO_SUCCESS,
    FAKE_STREAM_BLOCK
};

struct fake_file
{
    APPX_PACKAGE_FILE view;
    const BYTE *data;
    UINT64 data_size;
    UINT32 chunk_size;
    HRESULT terminal_hr;
    enum fake_stream_mode mode;
    HANDLE read_entered;
    HANDLE block_release;
};

struct appx_package_inspection
{
    struct fake_file files[TEST_MAX_FILES];
    UINT32 count;
    UINT64 expanded_size;
    LONG streams_opened;
    LONG streams_closed;
    LONG streams_cancelled;
};

struct wine_appx_archive_stream
{
    struct appx_package_inspection *inspection;
    struct fake_file *file;
    UINT64 offset;
    LONG cancelled;
};

struct test_directory
{
    WCHAR path[MAX_PATH];
    HANDLE handle;
};

struct space_script
{
    UINT64 values[4];
    UINT32 count;
    LONG calls;
    HRESULT failure;
};

struct cancel_signal
{
    HANDLE read_entered;
    HANDLE cancel_event;
    HANDLE block_release;
};

static struct space_script space_script;
static DWORD write_limit;
static LONG write_calls;
static LONG flush_file_calls;
static LONG flush_directory_calls;
static LONG fail_write_call;
static BOOL return_zero_write;
static HRESULT directory_flush_result;
static HRESULT file_flush_result;
static WCHAR injection_root[MAX_PATH];
static WCHAR injection_other[MAX_PATH];
static WCHAR injection_target[MAX_PATH];
static LONG injection_calls;
static BOOL injection_succeeded;

static HRESULT (WINAPI *p_appx_package_extract)(
    const APPX_PACKAGE_INSPECTION *inspection, HANDLE staging_root,
    const APPX_EXTRACT_OPTIONS *options );
static HRESULT (WINAPI *p_appx_package_extract_with_test_source)(
    const void *source, const APPX_EXTRACT_TEST_SOURCE *source_ops,
    HANDLE staging_root, const APPX_EXTRACT_OPTIONS *options,
    const APPX_EXTRACT_TEST_IO *test_io );

static UINT32 WINAPI fake_source_get_count( const void *source )
{
    const struct appx_package_inspection *inspection = source;

    return inspection ? inspection->count : 0;
}

static const APPX_PACKAGE_FILE *WINAPI fake_source_get_file(
    const void *source, UINT32 index )
{
    const struct appx_package_inspection *inspection = source;

    if (!inspection || index >= inspection->count) return NULL;
    return &inspection->files[index].view;
}

static UINT64 WINAPI fake_source_get_expanded_size( const void *source )
{
    const struct appx_package_inspection *inspection = source;

    return inspection ? inspection->expanded_size : 0;
}

static HRESULT WINAPI fake_source_open_stream(
    const void *source, UINT32 file_index, void **result )
{
    const struct appx_package_inspection *inspection = source;
    struct wine_appx_archive_stream *stream;

    if (!result) return E_INVALIDARG;
    *result = NULL;
    if (!inspection || file_index >= inspection->count) return E_BOUNDS;
    if (!(stream = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                              sizeof(*stream) )))
        return E_OUTOFMEMORY;
    stream->inspection = (struct appx_package_inspection *)inspection;
    stream->file = &stream->inspection->files[file_index];
    InterlockedIncrement( &stream->inspection->streams_opened );
    *result = stream;
    return S_OK;
}

static HRESULT WINAPI fake_source_open_null_stream(
    const void *source, UINT32 file_index, void **result )
{
    if (!source || !result || file_index) return E_INVALIDARG;
    *result = NULL;
    return S_OK;
}

static HRESULT WINAPI fake_source_read_stream(
    void *opaque, void *buffer, UINT32 capacity, UINT32 *read )
{
    struct wine_appx_archive_stream *stream = opaque;
    UINT64 remaining;
    UINT32 count;

    if (!read) return E_INVALIDARG;
    *read = 0;
    if (!stream || !buffer || !capacity) return E_INVALIDARG;
    if (InterlockedCompareExchange( &stream->cancelled, 0, 0 ))
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );

    if (stream->file->mode == FAKE_STREAM_BLOCK && !stream->offset)
    {
        DWORD wait;

        SetEvent( stream->file->read_entered );
        wait = WaitForSingleObject( stream->file->block_release,
                                    TEST_WAIT_MS * 2 );
        if (wait != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32( wait == WAIT_FAILED ?
                                       GetLastError() : ERROR_TIMEOUT );
        if (InterlockedCompareExchange( &stream->cancelled, 0, 0 ))
            return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    }
    if (stream->file->mode == FAKE_STREAM_EARLY_TERMINAL)
        return S_FALSE;
    if (stream->file->mode == FAKE_STREAM_ZERO_SUCCESS)
        return S_OK;

    remaining = stream->file->data_size - stream->offset;
    if (remaining)
    {
        count = remaining < capacity ? (UINT32)remaining : capacity;
        if (stream->file->chunk_size &&
            count > stream->file->chunk_size)
            count = stream->file->chunk_size;
        memcpy( buffer, stream->file->data + stream->offset, count );
        stream->offset += count;
        *read = count;
        return S_OK;
    }
    return stream->file->terminal_hr;
}

static void WINAPI fake_source_cancel_stream( void *opaque )
{
    struct wine_appx_archive_stream *stream = opaque;

    if (!stream) return;
    if (!InterlockedExchange( &stream->cancelled, 1 ))
        InterlockedIncrement( &stream->inspection->streams_cancelled );
    if (stream->file->block_release) SetEvent( stream->file->block_release );
}

static void WINAPI fake_source_close_stream( void *opaque )
{
    struct wine_appx_archive_stream *stream = opaque;

    if (!stream) return;
    InterlockedIncrement( &stream->inspection->streams_closed );
    HeapFree( GetProcessHeap(), 0, stream );
}

static const APPX_EXTRACT_TEST_SOURCE fake_source_ops =
{
    sizeof(fake_source_ops),
    fake_source_get_count,
    fake_source_get_file,
    fake_source_get_expanded_size,
    fake_source_open_stream,
    fake_source_read_stream,
    fake_source_cancel_stream,
    fake_source_close_stream
};

static HRESULT extract_fake( const struct appx_package_inspection *inspection,
                             HANDLE staging_root,
                             const APPX_EXTRACT_OPTIONS *options,
                             const APPX_EXTRACT_TEST_IO *test_io )
{
    return p_appx_package_extract_with_test_source(
        inspection, &fake_source_ops, staging_root, options, test_io );
}

static void reset_hooks( void )
{
    memset( &space_script, 0, sizeof(space_script) );
    space_script.values[0] = TEST_SPACE_UNLIMITED;
    space_script.count = 1;
    write_limit = MAXDWORD;
    write_calls = 0;
    flush_file_calls = 0;
    flush_directory_calls = 0;
    fail_write_call = 0;
    return_zero_write = FALSE;
    directory_flush_result = S_OK;
    file_flush_result = S_OK;
    injection_root[0] = 0;
    injection_other[0] = 0;
    injection_target[0] = 0;
    injection_calls = 0;
    injection_succeeded = FALSE;
}

static void init_inspection( struct appx_package_inspection *inspection )
{
    memset( inspection, 0, sizeof(*inspection) );
}

static void add_fake_file( struct appx_package_inspection *inspection,
                           const WCHAR *path, const BYTE *data,
                           UINT64 declared_size, UINT64 data_size,
                           UINT32 chunk_size )
{
    struct fake_file *file;

    ok( inspection->count < TEST_MAX_FILES, "too many fake files.\n" );
    if (inspection->count >= TEST_MAX_FILES) return;
    file = &inspection->files[inspection->count++];
    memset( file, 0, sizeof(*file) );
    file->view.path = path;
    file->view.archive_index = inspection->count;
    file->view.compression_method = 0;
    file->view.compressed_size = declared_size;
    file->view.uncompressed_size = declared_size;
    file->data = data;
    file->data_size = data_size;
    file->chunk_size = chunk_size;
    file->terminal_hr = S_FALSE;
    inspection->expanded_size += declared_size;
}

static BOOL append_path( WCHAR path[MAX_PATH], const WCHAR *name )
{
    UINT32 length = lstrlenW( path ), name_length = lstrlenW( name );

    if (length + 1 + name_length >= MAX_PATH) return FALSE;
    if (length && path[length - 1] != '\\') path[length++] = '\\';
    memcpy( path + length, name, (name_length + 1) * sizeof(WCHAR) );
    return TRUE;
}

static BOOL create_unique_directory( struct test_directory *directory )
{
    WCHAR temp[MAX_PATH];

    memset( directory, 0, sizeof(*directory) );
    directory->handle = INVALID_HANDLE_VALUE;
    if (!GetTempPathW( ARRAY_SIZE(temp), temp ) ||
        !GetTempFileNameW( temp, L"axe", 0, directory->path ))
        return FALSE;
    DeleteFileW( directory->path );
    if (!CreateDirectoryW( directory->path, NULL )) return FALSE;
    directory->handle = CreateFileW(
        directory->path, GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL );
    if (directory->handle == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW( directory->path );
        directory->path[0] = 0;
        return FALSE;
    }
    return TRUE;
}

static void remove_tree( const WCHAR *path )
{
    WIN32_FIND_DATAW data;
    WCHAR pattern[MAX_PATH], child[MAX_PATH];
    HANDLE find;

    lstrcpynW( pattern, path, ARRAY_SIZE(pattern) );
    if (!append_path( pattern, L"*" )) return;
    find = FindFirstFileW( pattern, &data );
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!lstrcmpW( data.cFileName, L"." ) ||
                !lstrcmpW( data.cFileName, L".." ))
                continue;
            lstrcpynW( child, path, ARRAY_SIZE(child) );
            if (!append_path( child, data.cFileName )) continue;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_tree( child );
            else
            {
                SetFileAttributesW( child, FILE_ATTRIBUTE_NORMAL );
                DeleteFileW( child );
            }
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    SetFileAttributesW( path, FILE_ATTRIBUTE_NORMAL );
    RemoveDirectoryW( path );
}

static void close_directory( struct test_directory *directory )
{
    if (directory->handle != INVALID_HANDLE_VALUE)
        CloseHandle( directory->handle );
    directory->handle = INVALID_HANDLE_VALUE;
    if (directory->path[0]) remove_tree( directory->path );
    directory->path[0] = 0;
}

static BOOL directory_is_empty( const WCHAR *path )
{
    BYTE buffer[offsetof(FILE_NAMES_INFORMATION, FileName) +
                (WINE_APPX_MAX_COMPONENT_CHARS + 1) * sizeof(WCHAR)];
    FILE_NAMES_INFORMATION *entry = (FILE_NAMES_INFORMATION *)buffer;
    IO_STATUS_BLOCK io;
    HANDLE directory;
    NTSTATUS status;
    BOOL restart = TRUE;
    BOOL empty = TRUE;

    directory = CreateFileW(
        path, FILE_LIST_DIRECTORY | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL );
    if (directory == INVALID_HANDLE_VALUE) return FALSE;
    for (;;)
    {
        UINT32 length;

        status = NtQueryDirectoryFile(
            directory, NULL, NULL, NULL, &io, buffer, sizeof(buffer),
            FileNamesInformation, TRUE, NULL, restart );
        restart = FALSE;
        if (status == STATUS_NO_MORE_FILES) break;
        if (status)
        {
            empty = FALSE;
            break;
        }
        length = entry->FileNameLength / sizeof(WCHAR);
        if (!((length == 1 && entry->FileName[0] == '.') ||
              (length == 2 && entry->FileName[0] == '.' &&
                              entry->FileName[1] == '.')))
        {
            empty = FALSE;
            break;
        }
    }
    CloseHandle( directory );
    return empty;
}

static BOOL read_file_data( const WCHAR *path, BYTE *buffer,
                            DWORD capacity, DWORD *size )
{
    HANDLE file;
    BOOL success;

    *size = 0;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    success = ReadFile( file, buffer, capacity, size, NULL );
    CloseHandle( file );
    return success;
}

static BOOL write_small_file( const WCHAR *path, const BYTE *data, DWORD size )
{
    HANDLE file;
    DWORD written;
    BOOL success;

    file = CreateFileW( path, GENERIC_WRITE, FILE_SHARE_READ |
                        FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    success = WriteFile( file, data, size, &written, NULL ) &&
              written == size && FlushFileBuffers( file );
    CloseHandle( file );
    return success;
}

static BOOL WINAPI test_write_file( HANDLE file, const void *buffer,
                                    DWORD size, DWORD *written,
                                    OVERLAPPED *overlapped )
{
    LONG call = InterlockedIncrement( &write_calls );
    DWORD request = size < write_limit ? size : write_limit;

    if (return_zero_write)
    {
        *written = 0;
        return TRUE;
    }
    if (fail_write_call && call == fail_write_call)
    {
        SetLastError( ERROR_WRITE_FAULT );
        return FALSE;
    }
    return WriteFile( file, buffer, request, written, overlapped );
}

static HRESULT WINAPI test_flush_file( HANDLE file )
{
    InterlockedIncrement( &flush_file_calls );
    if (file_flush_result != S_OK) return file_flush_result;
    return FlushFileBuffers( file ) ? S_OK :
                                      HRESULT_FROM_WIN32( GetLastError() );
}

static HRESULT WINAPI test_flush_directory( HANDLE directory )
{
    InterlockedIncrement( &flush_directory_calls );
    return directory_flush_result;
}

static HRESULT WINAPI scripted_space( HANDLE directory, UINT64 *available )
{
    LONG call = InterlockedIncrement( &space_script.calls ) - 1;
    UINT32 index;

    if (space_script.failure) return space_script.failure;
    if (!space_script.count) return E_FAIL;
    index = call < space_script.count ? call :
                                       space_script.count - 1;
    *available = space_script.values[index];
    return S_OK;
}

static HRESULT WINAPI inject_hardlink_space( HANDLE directory,
                                             UINT64 *available )
{
    WCHAR link[MAX_PATH];

    if (!InterlockedIncrement( &injection_calls ))
        return E_FAIL;
    if (injection_calls == 1)
    {
        lstrcpynW( link, injection_root, ARRAY_SIZE(link) );
        append_path( link, L"payload.bin" );
        injection_succeeded = CreateHardLinkW(
            link, injection_target, NULL );
    }
    *available = TEST_SPACE_UNLIMITED;
    return S_OK;
}

static HRESULT WINAPI inject_reparse_space( HANDLE directory,
                                            UINT64 *available )
{
    WCHAR link[MAX_PATH];

    InterlockedIncrement( &injection_calls );
    if (injection_calls == 1)
    {
        lstrcpynW( link, injection_root, ARRAY_SIZE(link) );
        append_path( link, L"Dir" );
        injection_succeeded = CreateSymbolicLinkW(
            link, injection_target,
            SYMBOLIC_LINK_FLAG_DIRECTORY |
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE );
    }
    *available = TEST_SPACE_UNLIMITED;
    return S_OK;
}

static HRESULT WINAPI inject_root_rename_space( HANDLE directory,
                                                UINT64 *available )
{
    InterlockedIncrement( &injection_calls );
    if (injection_calls == 1)
    {
        injection_succeeded =
            MoveFileExW( injection_root, injection_other,
                         MOVEFILE_WRITE_THROUGH ) &&
            CreateDirectoryW( injection_root, NULL );
    }
    *available = TEST_SPACE_UNLIMITED;
    return S_OK;
}

static APPX_EXTRACT_OPTIONS default_options( void )
{
    APPX_EXTRACT_OPTIONS options;

    memset( &options, 0, sizeof(options) );
    options.size = sizeof(options);
    return options;
}

static APPX_EXTRACT_TEST_IO default_test_io( void )
{
    APPX_EXTRACT_TEST_IO io;

    memset( &io, 0, sizeof(io) );
    io.size = sizeof(io);
    io.query_available_bytes = scripted_space;
    return io;
}

static void test_basic_streaming_and_attributes( void )
{
    static const BYTE executable[] =
        "MZ-streamed-full-trust-payload-with-partial-writes";
    static const BYTE manifest[] = "<Package/>";
    struct appx_package_inspection inspection;
    struct test_directory staging;
    APPX_EXTRACT_OPTIONS options = default_options();
    APPX_EXTRACT_TEST_IO io = default_test_io();
    BYTE buffer[128];
    WCHAR path[MAX_PATH];
    DWORD attributes, size;
    HRESULT hr;

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection,
                   L"VFS\\ProgramFilesX64\\Example\\app.exe",
                   executable, sizeof(executable) - 1,
                   sizeof(executable) - 1, 11 );
    add_fake_file( &inspection, L"Assets\\empty.dat",
                   NULL, 0, 0, 1 );
    add_fake_file( &inspection, L"AppxManifest.xml",
                   manifest, sizeof(manifest) - 1,
                   sizeof(manifest) - 1, 3 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory, error %lu.\n",
        GetLastError() );
    if (staging.handle == INVALID_HANDLE_VALUE) return;

    options.max_expanded_bytes = inspection.expanded_size;
    options.io_buffer_size = 13;
    write_limit = 4;
    io.write_file = test_write_file;
    io.flush_file = test_flush_file;
    io.flush_directory = test_flush_directory;
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == S_OK, "basic extraction returned %#lx.\n", hr );
    ok( inspection.streams_opened == 3,
        "opened %ld streams.\n", inspection.streams_opened );
    ok( inspection.streams_closed == 3,
        "closed %ld streams.\n", inspection.streams_closed );
    ok( write_calls > 3, "only %ld writes were issued.\n", write_calls );
    ok( flush_file_calls == 3, "got %ld file flushes.\n",
        flush_file_calls );
    ok( flush_directory_calls == 5,
        "got %ld directory flushes.\n", flush_directory_calls );
    ok( space_script.calls == 2, "got %ld space queries.\n",
        space_script.calls );

    lstrcpynW( path, staging.path, ARRAY_SIZE(path) );
    append_path( path, L"VFS\\ProgramFilesX64\\Example\\app.exe" );
    ok( read_file_data( path, buffer, sizeof(buffer), &size ),
        "failed to read executable, error %lu.\n", GetLastError() );
    ok( size == sizeof(executable) - 1 &&
        !memcmp( buffer, executable, size ),
        "executable contents differ, size %lu.\n", size );
    attributes = GetFileAttributesW( path );
    ok( attributes != INVALID_FILE_ATTRIBUTES,
        "missing executable, error %lu.\n", GetLastError() );
    ok( !(attributes & (FILE_ATTRIBUTE_DIRECTORY |
                        FILE_ATTRIBUTE_REPARSE_POINT |
                        FILE_ATTRIBUTE_COMPRESSED |
                        FILE_ATTRIBUTE_ENCRYPTED |
                        FILE_ATTRIBUTE_SPARSE_FILE |
                        FILE_ATTRIBUTE_READONLY)),
        "unsafe executable attributes %#lx.\n", attributes );

    lstrcpynW( path, staging.path, ARRAY_SIZE(path) );
    append_path( path, L"Assets\\empty.dat" );
    ok( read_file_data( path, buffer, sizeof(buffer), &size ) && !size,
        "empty file size is %lu, error %lu.\n", size, GetLastError() );
    close_directory( &staging );
}

static void check_failed_stream_cleanup( enum fake_stream_mode mode,
                                         UINT64 declared_size,
                                         UINT64 data_size,
                                         HRESULT terminal_hr,
                                         HRESULT expected )
{
    static const BYTE data[] = "abcdef";
    struct appx_package_inspection inspection;
    struct test_directory staging;
    APPX_EXTRACT_TEST_IO io = default_test_io();
    HRESULT hr;

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"Dir\\payload.bin", data,
                   declared_size, data_size, 2 );
    inspection.files[0].mode = mode;
    inspection.files[0].terminal_hr = terminal_hr;
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    ok( hr == expected, "stream mode %u returned %#lx, expected %#lx.\n",
        mode, hr, expected );
    ok( directory_is_empty( staging.path ),
        "failed stream left staging entries.\n" );
    ok( inspection.streams_opened == inspection.streams_closed,
        "stream leak: opened %ld, closed %ld.\n",
        inspection.streams_opened, inspection.streams_closed );
    close_directory( &staging );
}

static void test_terminal_verification_and_failures( void )
{
    static const BYTE data[] = "payload-data";
    struct appx_package_inspection inspection;
    struct test_directory staging;
    APPX_EXTRACT_TEST_IO io = default_test_io();
    HRESULT hr;

    check_failed_stream_cleanup(
        FAKE_STREAM_EARLY_TERMINAL, 6, 6, S_FALSE,
        APPX_E_CORRUPT_CONTENT );
    check_failed_stream_cleanup(
        FAKE_STREAM_ZERO_SUCCESS, 6, 6, S_FALSE,
        APPX_E_CORRUPT_CONTENT );
    check_failed_stream_cleanup(
        FAKE_STREAM_NORMAL, 6, 6, APPX_E_CORRUPT_CONTENT,
        APPX_E_CORRUPT_CONTENT );
    check_failed_stream_cleanup(
        FAKE_STREAM_NORMAL, 5, 6, S_FALSE,
        APPX_E_CORRUPT_CONTENT );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data) - 1, sizeof(data) - 1, 4 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    return_zero_write = TRUE;
    io.write_file = test_write_file;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_WRITE_FAULT),
        "zero write returned %#lx.\n", hr );
    ok( directory_is_empty( staging.path ),
        "zero write left staging entries.\n" );
    close_directory( &staging );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data) - 1, sizeof(data) - 1, 4 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    fail_write_call = 2;
    write_limit = 2;
    io = default_test_io();
    io.write_file = test_write_file;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_WRITE_FAULT),
        "failed write returned %#lx.\n", hr );
    ok( directory_is_empty( staging.path ),
        "failed write left staging entries.\n" );
    close_directory( &staging );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data) - 1, sizeof(data) - 1, 4 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    file_flush_result = E_FAIL;
    io = default_test_io();
    io.flush_file = test_flush_file;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    ok( hr == E_FAIL, "file flush failure returned %#lx.\n", hr );
    ok( directory_is_empty( staging.path ),
        "file flush failure left staging entries.\n" );
    close_directory( &staging );
}

static void test_plan_validation( void )
{
    static const BYTE byte = 0x5a;
    static const struct
    {
        const WCHAR *first;
        const WCHAR *second;
    } collisions[] =
    {
        {L"A.txt", L"a.TXT"},
        {L"file", L"file\\child"},
        {L"Dir\\child", L"dir"},
    };
    static const WCHAR *invalid_paths[] =
    {
        L"..\\escape",
        L"absolute/path",
        L"C:\\drive",
        L"Dir\\\\double",
        L"CON.txt",
        L"Dir\\trailing. ",
    };
    struct appx_package_inspection inspection;
    struct test_directory staging;
    APPX_EXTRACT_TEST_IO io = default_test_io();
    HRESULT hr;
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(collisions); i++)
    {
        reset_hooks();
        init_inspection( &inspection );
        add_fake_file( &inspection, collisions[i].first,
                       &byte, 1, 1, 1 );
        add_fake_file( &inspection, collisions[i].second,
                       &byte, 1, 1, 1 );
        ok( create_unique_directory( &staging ),
            "failed to create staging directory.\n" );
        if (staging.handle == INVALID_HANDLE_VALUE) continue;
        hr = extract_fake(
            &inspection, staging.handle, NULL, &io );
        ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
            "collision %u returned %#lx.\n", i, hr );
        ok( !inspection.streams_opened,
            "collision %u opened %ld streams.\n",
            i, inspection.streams_opened );
        ok( directory_is_empty( staging.path ),
            "collision %u touched staging.\n", i );
        close_directory( &staging );
    }

    for (i = 0; i < ARRAY_SIZE(invalid_paths); i++)
    {
        reset_hooks();
        init_inspection( &inspection );
        add_fake_file( &inspection, invalid_paths[i],
                       &byte, 1, 1, 1 );
        ok( create_unique_directory( &staging ),
            "failed to create staging directory.\n" );
        if (staging.handle == INVALID_HANDLE_VALUE) continue;
        hr = extract_fake(
            &inspection, staging.handle, NULL, &io );
        ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
            "invalid path %s returned %#lx.\n",
            wine_dbgstr_w(invalid_paths[i]), hr );
        ok( directory_is_empty( staging.path ),
            "invalid path touched staging.\n" );
        close_directory( &staging );
    }

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", &byte, 1, 1, 1 );
    inspection.expanded_size++;
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle != INVALID_HANDLE_VALUE)
    {
        hr = extract_fake(
            &inspection, staging.handle, NULL, &io );
        ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
            "expanded-size mismatch returned %#lx.\n", hr );
        ok( directory_is_empty( staging.path ),
            "expanded-size mismatch touched staging.\n" );
        close_directory( &staging );
    }
}

static void test_quota_and_free_space( void )
{
    static const BYTE data[10] = {0,1,2,3,4,5,6,7,8,9};
    struct appx_package_inspection inspection;
    struct test_directory staging;
    APPX_EXTRACT_OPTIONS options;
    APPX_EXTRACT_TEST_IO io;
    HRESULT hr;

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data), sizeof(data), 3 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    options = default_options();
    options.max_expanded_bytes = sizeof(data) - 1;
    io = default_test_io();
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA),
        "quota returned %#lx.\n", hr );
    ok( !space_script.calls && !inspection.streams_opened,
        "quota consumed I/O: space %ld, streams %ld.\n",
        space_script.calls, inspection.streams_opened );
    ok( directory_is_empty( staging.path ),
        "quota failure touched staging.\n" );
    close_directory( &staging );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data), sizeof(data), 3 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    options = default_options();
    options.free_space_floor_bytes = 5;
    io = default_test_io();
    space_script.values[0] = 14;
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL),
        "initial disk-full returned %#lx.\n", hr );
    ok( !inspection.streams_opened,
        "disk-full opened %ld streams.\n", inspection.streams_opened );
    ok( directory_is_empty( staging.path ),
        "initial disk-full touched staging.\n" );
    close_directory( &staging );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data), sizeof(data), 3 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    options = default_options();
    options.free_space_floor_bytes = 5;
    io = default_test_io();
    space_script.values[0] = 15;
    space_script.values[1] = 4;
    space_script.count = 2;
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL),
        "final disk-full returned %#lx.\n", hr );
    ok( directory_is_empty( staging.path ),
        "final disk-full left staging entries.\n" );
    close_directory( &staging );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data), sizeof(data), 3 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    options = default_options();
    options.max_expanded_bytes = sizeof(data);
    options.free_space_floor_bytes = 5;
    io = default_test_io();
    io.flush_directory = test_flush_directory;
    space_script.values[0] = 15;
    space_script.values[1] = 5;
    space_script.count = 2;
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == S_OK, "exact quota/space returned %#lx.\n", hr );
    close_directory( &staging );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data), sizeof(data), 3 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    options = default_options();
    options.free_space_floor_bytes = ~(UINT64)0;
    io = default_test_io();
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL),
        "space overflow returned %#lx.\n", hr );
    ok( directory_is_empty( staging.path ),
        "space overflow touched staging.\n" );
    close_directory( &staging );
}

static DWORD WINAPI signal_cancel_thread( void *parameter )
{
    const struct cancel_signal *signal = parameter;
    DWORD wait;

    wait = WaitForSingleObject( signal->read_entered, TEST_WAIT_MS );
    if (wait != WAIT_OBJECT_0)
    {
        SetEvent( signal->cancel_event );
        SetEvent( signal->block_release );
        return wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    }
    if (!SetEvent( signal->cancel_event ))
    {
        SetEvent( signal->block_release );
        return GetLastError();
    }
    wait = WaitForSingleObject( signal->block_release, TEST_WAIT_MS );
    if (wait != WAIT_OBJECT_0)
    {
        SetEvent( signal->block_release );
        return wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    }
    return 0;
}

static void test_cancellation( void )
{
    static const BYTE data[] = "blocked payload";
    struct appx_package_inspection inspection;
    struct test_directory staging;
    APPX_EXTRACT_OPTIONS options;
    APPX_EXTRACT_TEST_IO io;
    struct cancel_signal signal;
    HANDLE cancel_event, signal_thread = NULL;
    DWORD signal_result, wait;
    HRESULT hr;

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data) - 1, sizeof(data) - 1, 3 );
    inspection.files[0].mode = FAKE_STREAM_BLOCK;
    inspection.files[0].read_entered =
        CreateEventW( NULL, TRUE, FALSE, NULL );
    inspection.files[0].block_release =
        CreateEventW( NULL, TRUE, FALSE, NULL );
    cancel_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    /*
     * The fake stream uses block_release as the cancellation source in this
     * test.  The signal helper waits until read() is actually blocked, then
     * signals the caller event observed by the extraction monitor.
     */
    ok( inspection.files[0].read_entered &&
        inspection.files[0].block_release && cancel_event,
        "failed to create cancellation events.\n" );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (!inspection.files[0].read_entered ||
        !inspection.files[0].block_release || !cancel_event ||
        staging.handle == INVALID_HANDLE_VALUE)
        goto done;
    signal.read_entered = inspection.files[0].read_entered;
    signal.cancel_event = cancel_event;
    signal.block_release = inspection.files[0].block_release;
    signal_thread = CreateThread( NULL, 0, signal_cancel_thread,
                                  &signal, 0, NULL );
    ok( !!signal_thread, "failed to create signal thread.\n" );
    if (!signal_thread) goto done;
    options = default_options();
    options.cancel_event = cancel_event;
    io = default_test_io();
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "cancelled extraction returned %#lx.\n", hr );
    wait = WaitForSingleObject( signal_thread, TEST_WAIT_MS * 2 );
    ok( wait == WAIT_OBJECT_0, "signal thread did not exit, wait %#lx.\n",
        wait );
    if (wait != WAIT_OBJECT_0)
    {
        SetEvent( inspection.files[0].read_entered );
        SetEvent( inspection.files[0].block_release );
        SetEvent( cancel_event );
        wait = WaitForSingleObject( signal_thread, TEST_WAIT_MS );
        ok( wait == WAIT_OBJECT_0,
            "signal thread ignored failsafe release, wait %#lx.\n", wait );
        if (wait != WAIT_OBJECT_0)
        {
            TerminateThread( signal_thread, ERROR_TIMEOUT );
            WaitForSingleObject( signal_thread, TEST_WAIT_MS );
        }
    }
    signal_result = ERROR_TIMEOUT;
    ok( GetExitCodeThread( signal_thread, &signal_result ),
        "failed to query signal thread, error %lu.\n", GetLastError() );
    ok( !signal_result, "signal thread returned %lu.\n", signal_result );
    CloseHandle( signal_thread );
    signal_thread = NULL;
    ok( inspection.streams_cancelled == 1,
        "cancelled %ld streams.\n", inspection.streams_cancelled );
    ok( inspection.streams_opened == inspection.streams_closed,
        "cancel stream leak: opened %ld, closed %ld.\n",
        inspection.streams_opened, inspection.streams_closed );
    ok( directory_is_empty( staging.path ),
        "cancellation left staging entries.\n" );

done:
    if (signal_thread)
    {
        if (inspection.files[0].read_entered)
            SetEvent( inspection.files[0].read_entered );
        if (inspection.files[0].block_release)
            SetEvent( inspection.files[0].block_release );
        if (cancel_event) SetEvent( cancel_event );
        wait = WaitForSingleObject( signal_thread, TEST_WAIT_MS * 2 );
        if (wait != WAIT_OBJECT_0)
        {
            ok( 0, "signal thread cleanup timed out, wait %#lx.\n", wait );
            TerminateThread( signal_thread, ERROR_TIMEOUT );
            WaitForSingleObject( signal_thread, TEST_WAIT_MS );
        }
        CloseHandle( signal_thread );
    }
    if (inspection.files[0].read_entered)
        CloseHandle( inspection.files[0].read_entered );
    if (inspection.files[0].block_release)
        CloseHandle( inspection.files[0].block_release );
    if (cancel_event) CloseHandle( cancel_event );
    close_directory( &staging );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", data,
                   sizeof(data) - 1, sizeof(data) - 1, 3 );
    cancel_event = CreateEventW( NULL, TRUE, TRUE, NULL );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (cancel_event && staging.handle != INVALID_HANDLE_VALUE)
    {
        options = default_options();
        options.cancel_event = cancel_event;
        io = default_test_io();
        hr = extract_fake(
            &inspection, staging.handle, &options, &io );
        ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
            "pre-cancelled extraction returned %#lx.\n", hr );
        ok( !inspection.streams_opened,
            "pre-cancel opened %ld streams.\n",
            inspection.streams_opened );
        ok( directory_is_empty( staging.path ),
            "pre-cancel touched staging.\n" );
    }
    if (cancel_event) CloseHandle( cancel_event );
    close_directory( &staging );
}

static void test_root_validation_and_collisions( void )
{
    static const BYTE original[] = "do-not-overwrite";
    static const BYTE payload[] = "new payload";
    struct appx_package_inspection inspection;
    struct test_directory staging, outside;
    APPX_EXTRACT_TEST_IO io;
    BYTE buffer[64];
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD size;
    HRESULT hr;

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", payload,
                   sizeof(payload) - 1, sizeof(payload) - 1, 3 );
    ok( create_unique_directory( &staging ),
        "failed to create staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    lstrcpynW( path, staging.path, ARRAY_SIZE(path) );
    append_path( path, L"unrelated.dat" );
    ok( write_small_file( path, original, sizeof(original) - 1 ),
        "failed to create unrelated file.\n" );
    io = default_test_io();
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY),
        "nonempty root returned %#lx.\n", hr );
    ok( read_file_data( path, buffer, sizeof(buffer), &size ) &&
        size == sizeof(original) - 1 &&
        !memcmp( buffer, original, size ),
        "nonempty root file was modified.\n" );
    close_directory( &staging );

    ok( create_unique_directory( &staging ) &&
        create_unique_directory( &outside ),
        "failed to create hardlink test directories.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE ||
        outside.handle == INVALID_HANDLE_VALUE)
        goto hardlink_done;
    lstrcpynW( injection_root, staging.path,
               ARRAY_SIZE(injection_root) );
    lstrcpynW( injection_target, outside.path,
               ARRAY_SIZE(injection_target) );
    append_path( injection_target, L"target.bin" );
    ok( write_small_file( injection_target, original,
                          sizeof(original) - 1 ),
        "failed to create hardlink target.\n" );
    io = default_test_io();
    io.query_available_bytes = inject_hardlink_space;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    if (!injection_succeeded)
        win_skip( "hardlink creation is unavailable, error %lu.\n",
                  GetLastError() );
    else
    {
        ok( FAILED(hr), "hardlink collision returned %#lx.\n", hr );
        ok( read_file_data( injection_target, buffer, sizeof(buffer), &size ) &&
            size == sizeof(original) - 1 &&
            !memcmp( buffer, original, size ),
            "hardlink target was modified.\n" );
    }
hardlink_done:
    close_directory( &staging );
    close_directory( &outside );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"Dir\\payload.bin", payload,
                   sizeof(payload) - 1, sizeof(payload) - 1, 3 );
    ok( create_unique_directory( &staging ) &&
        create_unique_directory( &outside ),
        "failed to create reparse test directories.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE ||
        outside.handle == INVALID_HANDLE_VALUE)
        goto reparse_done;
    lstrcpynW( injection_root, staging.path,
               ARRAY_SIZE(injection_root) );
    lstrcpynW( injection_target, outside.path,
               ARRAY_SIZE(injection_target) );
    io = default_test_io();
    io.query_available_bytes = inject_reparse_space;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    if (!injection_succeeded)
        win_skip( "directory symlink creation is unavailable, error %lu.\n",
                  GetLastError() );
    else
    {
        ok( FAILED(hr), "reparse collision returned %#lx.\n", hr );
        ok( directory_is_empty( outside.path ),
            "reparse target received payload entries.\n" );
    }
reparse_done:
    close_directory( &staging );
    close_directory( &outside );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", payload,
                   sizeof(payload) - 1, sizeof(payload) - 1, 3 );
    ok( create_unique_directory( &staging ),
        "failed to create root-file test directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    lstrcpynW( path, staging.path, ARRAY_SIZE(path) );
    append_path( path, L"regular.file" );
    ok( write_small_file( path, original, sizeof(original) - 1 ),
        "failed to create root test file.\n" );
    file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                        FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to open root test file.\n" );
    io = default_test_io();
    hr = extract_fake(
        &inspection, file, NULL, &io );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DIRECTORY),
        "regular root file returned %#lx.\n", hr );
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    close_directory( &staging );
}

static void test_root_handle_rename_confinement( void )
{
    static const BYTE payload[] = "pinned-root-payload";
    struct appx_package_inspection inspection;
    struct test_directory staging;
    APPX_EXTRACT_TEST_IO io;
    BYTE buffer[64];
    WCHAR path[MAX_PATH];
    DWORD size;
    HRESULT hr;

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"Dir\\payload.bin", payload,
                   sizeof(payload) - 1, sizeof(payload) - 1, 4 );
    ok( create_unique_directory( &staging ),
        "failed to create rename staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    lstrcpynW( injection_root, staging.path,
               ARRAY_SIZE(injection_root) );
    lstrcpynW( injection_other, staging.path,
               ARRAY_SIZE(injection_other) );
    if (lstrlenW( injection_other ) + 6 < ARRAY_SIZE(injection_other))
        lstrcatW( injection_other, L".moved" );
    else
    {
        close_directory( &staging );
        return;
    }
    io = default_test_io();
    io.query_available_bytes = inject_root_rename_space;
    io.flush_directory = test_flush_directory;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    if (!injection_succeeded)
    {
        win_skip( "open-directory rename is unavailable, error %lu.\n",
                  GetLastError() );
        close_directory( &staging );
        return;
    }
    ok( hr == S_OK, "renamed-root extraction returned %#lx.\n", hr );
    ok( directory_is_empty( injection_root ),
        "replacement root received payload entries.\n" );
    lstrcpynW( path, injection_other, ARRAY_SIZE(path) );
    append_path( path, L"Dir\\payload.bin" );
    ok( read_file_data( path, buffer, sizeof(buffer), &size ) &&
        size == sizeof(payload) - 1 &&
        !memcmp( buffer, payload, size ),
        "pinned root did not receive expected payload.\n" );

    CloseHandle( staging.handle );
    staging.handle = INVALID_HANDLE_VALUE;
    remove_tree( injection_root );
    remove_tree( injection_other );
    staging.path[0] = 0;
}

static void test_options_and_durability( void )
{
    static const BYTE byte = 0x42;
    struct appx_package_inspection inspection;
    struct test_directory staging;
    APPX_EXTRACT_OPTIONS options;
    APPX_EXTRACT_TEST_IO io;
    APPX_EXTRACT_TEST_SOURCE source_ops;
    HRESULT hr;

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", &byte, 1, 1, 1 );
    ok( create_unique_directory( &staging ),
        "failed to create options staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    hr = p_appx_package_extract_with_test_source(
        NULL, &fake_source_ops, staging.handle, NULL, NULL );
    ok( hr == E_INVALIDARG, "NULL source returned %#lx.\n", hr );
    hr = p_appx_package_extract_with_test_source(
        &inspection, NULL, staging.handle, NULL, NULL );
    ok( hr == E_INVALIDARG, "NULL source callbacks returned %#lx.\n", hr );
    source_ops = fake_source_ops;
    source_ops.size--;
    hr = p_appx_package_extract_with_test_source(
        &inspection, &source_ops, staging.handle, NULL, NULL );
    ok( hr == E_INVALIDARG, "short source callbacks returned %#lx.\n", hr );
    source_ops = fake_source_ops;
    source_ops.read_stream = NULL;
    hr = p_appx_package_extract_with_test_source(
        &inspection, &source_ops, staging.handle, NULL, NULL );
    ok( hr == E_INVALIDARG, "missing source callback returned %#lx.\n", hr );
    inspection.count = 65537;
    hr = p_appx_package_extract_with_test_source(
        &inspection, &fake_source_ops, staging.handle, NULL, NULL );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "oversize source file count returned %#lx.\n", hr );
    inspection.count = 1;
    source_ops = fake_source_ops;
    source_ops.open_stream = fake_source_open_null_stream;
    hr = p_appx_package_extract_with_test_source(
        &inspection, &source_ops, staging.handle, NULL, NULL );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "successful NULL source stream returned %#lx.\n", hr );
    options = default_options();
    options.size--;
    io = default_test_io();
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == E_INVALIDARG, "short options returned %#lx.\n", hr );
    options = default_options();
    options.flags = 1;
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == E_INVALIDARG, "unknown options flags returned %#lx.\n", hr );
    options = default_options();
    options.io_buffer_size = APPX_EXTRACT_MAX_BUFFER_SIZE + 1;
    hr = extract_fake(
        &inspection, staging.handle, &options, &io );
    ok( hr == E_INVALIDARG, "oversize buffer returned %#lx.\n", hr );
    io.size--;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    ok( hr == E_INVALIDARG, "short test I/O returned %#lx.\n", hr );
    ok( directory_is_empty( staging.path ),
        "invalid options touched staging.\n" );
    close_directory( &staging );

    reset_hooks();
    init_inspection( &inspection );
    add_fake_file( &inspection, L"payload.bin", &byte, 1, 1, 1 );
    ok( create_unique_directory( &staging ),
        "failed to create durability staging directory.\n" );
    if (staging.handle == INVALID_HANDLE_VALUE) return;
    directory_flush_result = APPX_EXTRACT_S_WEAK_DURABILITY;
    io = default_test_io();
    io.flush_directory = test_flush_directory;
    hr = extract_fake(
        &inspection, staging.handle, NULL, &io );
    ok( hr == APPX_EXTRACT_S_WEAK_DURABILITY,
        "weak durability returned %#lx.\n", hr );
    ok( !directory_is_empty( staging.path ),
        "weak durability discarded verified payload.\n" );
    close_directory( &staging );
}

START_TEST(extract)
{
    HMODULE module = LoadLibraryA( "appxsvc.dll" );

    ok( !!module, "failed to load appxsvc.dll, error %lu.\n",
        GetLastError() );
    if (!module) return;
    p_appx_package_extract = (void *)GetProcAddress(
        module, "appx_package_extract" );
    p_appx_package_extract_with_test_source = (void *)GetProcAddress(
        module, "appx_package_extract_with_test_source" );
    ok( !!p_appx_package_extract,
        "appx_package_extract is not exported.\n" );
    ok( !!p_appx_package_extract_with_test_source,
        "appx_package_extract_with_test_source is not exported.\n" );
    if (!p_appx_package_extract ||
        !p_appx_package_extract_with_test_source)
    {
        FreeLibrary( module );
        return;
    }

    ok( p_appx_package_extract( NULL, NULL, NULL ) == E_INVALIDARG,
        "NULL inspection was accepted.\n" );
    test_basic_streaming_and_attributes();
    test_terminal_verification_and_failures();
    test_plan_validation();
    test_quota_and_free_space();
    test_cancellation();
    test_root_validation_and_collisions();
    test_root_handle_rename_confinement();
    test_options_and_durability();
    FreeLibrary( module );
}
