/*
 * AppX archive index tests
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

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "wine/appxsvc.h"
#include "wine/test.h"

#define ZIP_LOCAL_FILE_HEADER_SIGNATURE          0x04034b50
#define ZIP_DATA_DESCRIPTOR_SIGNATURE            0x08074b50
#define ZIP_CENTRAL_DIRECTORY_SIGNATURE          0x02014b50
#define ZIP64_END_DIRECTORY_SIGNATURE            0x06064b50
#define ZIP64_END_DIRECTORY_LOCATOR_SIGNATURE    0x07064b50
#define ZIP_END_DIRECTORY_SIGNATURE              0x06054b50

#define ZIP_FLAG_DATA_DESCRIPTOR                  0x0008
#define ZIP_FLAG_UTF8                             0x0800

#define MAX_TEST_ENTRIES                          16

static HRESULT (WINAPI *p_wine_appx_archive_open)( HANDLE, const WINE_APPX_ARCHIVE_LIMITS *,
                                                   UINT32, WINE_APPX_ARCHIVE ** );
static void (WINAPI *p_wine_appx_archive_close)( WINE_APPX_ARCHIVE * );
static HRESULT (WINAPI *p_wine_appx_archive_get_count)( WINE_APPX_ARCHIVE *, UINT32 * );
static HRESULT (WINAPI *p_wine_appx_archive_get_entry)( WINE_APPX_ARCHIVE *, UINT32,
                                                        WINE_APPX_ARCHIVE_ENTRY *,
                                                        UINT32 *, WCHAR * );

struct buffer
{
    BYTE *data;
    UINT32 size;
    UINT32 capacity;
};

struct test_entry
{
    const BYTE *name;
    UINT16 name_length;
    const BYTE *data;
    UINT32 data_length;
    UINT16 flags;
    UINT16 method;
    UINT32 crc32;
    UINT64 compressed_size;
    UINT64 uncompressed_size;
    UINT64 local_offset;
    UINT64 central_offset;
    UINT64 descriptor_offset;
    const BYTE *local_extra;
    const BYTE *central_extra;
    UINT16 local_extra_length;
    UINT16 central_extra_length;
    BOOL descriptor_signature;
    BOOL zip64;
};

struct zip_builder
{
    struct buffer buffer;
    struct test_entry entries[MAX_TEST_ENTRIES];
    UINT32 count;
    UINT64 central_offset;
    UINT64 eocd_offset;
};

static void put_uint16( BYTE *buffer, UINT16 value )
{
    buffer[0] = value;
    buffer[1] = value >> 8;
}

static void put_uint32( BYTE *buffer, UINT32 value )
{
    put_uint16( buffer, value );
    put_uint16( buffer + 2, value >> 16 );
}

static void put_uint64( BYTE *buffer, UINT64 value )
{
    put_uint32( buffer, value );
    put_uint32( buffer + 4, value >> 32 );
}

static UINT32 read_uint32( const BYTE *buffer )
{
    return buffer[0] | ((UINT32)buffer[1] << 8) | ((UINT32)buffer[2] << 16) |
           ((UINT32)buffer[3] << 24);
}

static BOOL reserve( struct buffer *buffer, UINT32 length )
{
    BYTE *data;
    UINT32 capacity;

    if (length <= buffer->capacity - buffer->size) return TRUE;
    capacity = buffer->capacity ? buffer->capacity : 1024;
    while (capacity - buffer->size < length)
    {
        if (capacity > MAXDWORD / 2) return FALSE;
        capacity *= 2;
    }
    if (!(data = HeapReAlloc( GetProcessHeap(), 0, buffer->data, capacity )))
    {
        if (!buffer->data) data = HeapAlloc( GetProcessHeap(), 0, capacity );
        if (!data) return FALSE;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return TRUE;
}

static BYTE *append( struct buffer *buffer, UINT32 length )
{
    BYTE *result;

    if (!reserve( buffer, length )) return NULL;
    result = buffer->data + buffer->size;
    memset( result, 0, length );
    buffer->size += length;
    return result;
}

static BOOL append_bytes( struct buffer *buffer, const void *data, UINT32 length )
{
    BYTE *destination;

    if (!length) return TRUE;
    destination = append( buffer, length );
    if (!destination) return FALSE;
    memcpy( destination, data, length );
    return TRUE;
}

static BOOL append_uint32( struct buffer *buffer, UINT32 value )
{
    BYTE *destination = append( buffer, sizeof(value) );

    if (!destination) return FALSE;
    put_uint32( destination, value );
    return TRUE;
}

static BOOL append_uint64( struct buffer *buffer, UINT64 value )
{
    BYTE *destination = append( buffer, sizeof(value) );

    if (!destination) return FALSE;
    put_uint64( destination, value );
    return TRUE;
}

static void free_builder( struct zip_builder *builder )
{
    HeapFree( GetProcessHeap(), 0, builder->buffer.data );
    memset( builder, 0, sizeof(*builder) );
}

static struct test_entry *add_entry_with_extras( struct zip_builder *builder, const char *name,
                                                 const BYTE *data, UINT32 data_length,
                                                 UINT16 method, UINT64 uncompressed_size,
                                                 UINT32 crc32, UINT16 flags,
                                                 BOOL descriptor_signature, BOOL zip64,
                                                 const BYTE *local_extra,
                                                 UINT16 local_extra_length,
                                                 const BYTE *central_extra,
                                                 UINT16 central_extra_length )
{
    struct test_entry *entry;
    BYTE *header, *extra;
    UINT32 total_extra_length = (zip64 && !(flags & ZIP_FLAG_DATA_DESCRIPTOR) ? 20 : 0) +
                                local_extra_length;
    UINT16 extra_length;

    ok( builder->count < ARRAY_SIZE(builder->entries), "too many test entries.\n" );
    if (builder->count >= ARRAY_SIZE(builder->entries)) return NULL;
    ok( total_extra_length <= 0xffff, "local extra fields are too large.\n" );
    if (total_extra_length > 0xffff) return NULL;
    extra_length = total_extra_length;

    entry = builder->entries + builder->count++;
    memset( entry, 0, sizeof(*entry) );
    entry->name = (const BYTE *)name;
    entry->name_length = strlen( name );
    entry->data = data;
    entry->data_length = data_length;
    entry->flags = flags;
    entry->method = method;
    entry->crc32 = crc32;
    entry->compressed_size = data_length;
    entry->uncompressed_size = uncompressed_size;
    entry->local_extra = local_extra;
    entry->local_extra_length = local_extra_length;
    entry->central_extra = central_extra;
    entry->central_extra_length = central_extra_length;
    entry->descriptor_signature = descriptor_signature;
    entry->zip64 = zip64;
    entry->local_offset = builder->buffer.size;

    header = append( &builder->buffer, 30 );
    ok( !!header, "failed to append local header.\n" );
    if (!header) return NULL;
    put_uint32( header, ZIP_LOCAL_FILE_HEADER_SIGNATURE );
    put_uint16( header + 4, zip64 ? 45 : method == 8 ? 20 : 10 );
    put_uint16( header + 6, flags );
    put_uint16( header + 8, method );
    if (flags & ZIP_FLAG_DATA_DESCRIPTOR)
    {
        put_uint32( header + 14, 0 );
        put_uint32( header + 18, 0 );
        put_uint32( header + 22, 0 );
    }
    else
    {
        put_uint32( header + 14, crc32 );
        put_uint32( header + 18, zip64 ? 0xffffffff : data_length );
        put_uint32( header + 22, zip64 ? 0xffffffff : uncompressed_size );
    }
    put_uint16( header + 26, entry->name_length );
    put_uint16( header + 28, extra_length );
    ok( append_bytes( &builder->buffer, name, entry->name_length ), "failed to append name.\n" );
    if (zip64 && !(flags & ZIP_FLAG_DATA_DESCRIPTOR))
    {
        extra = append( &builder->buffer, 20 );
        ok( !!extra, "failed to append ZIP64 local extra field.\n" );
        if (!extra) return NULL;
        put_uint16( extra, 1 );
        put_uint16( extra + 2, 16 );
        put_uint64( extra + 4, uncompressed_size );
        put_uint64( extra + 12, data_length );
    }
    ok( append_bytes( &builder->buffer, local_extra, local_extra_length ),
        "failed to append local extra fields.\n" );
    ok( append_bytes( &builder->buffer, data, data_length ), "failed to append data.\n" );

    if (flags & ZIP_FLAG_DATA_DESCRIPTOR)
    {
        entry->descriptor_offset = builder->buffer.size;
        if (descriptor_signature)
            ok( append_uint32( &builder->buffer, ZIP_DATA_DESCRIPTOR_SIGNATURE ),
                "failed to append descriptor signature.\n" );
        ok( append_uint32( &builder->buffer, crc32 ), "failed to append descriptor CRC.\n" );
        if (zip64)
        {
            ok( append_uint64( &builder->buffer, data_length ),
                "failed to append ZIP64 descriptor size.\n" );
            ok( append_uint64( &builder->buffer, uncompressed_size ),
                "failed to append ZIP64 descriptor size.\n" );
        }
        else
        {
            ok( append_uint32( &builder->buffer, data_length ),
                "failed to append descriptor size.\n" );
            ok( append_uint32( &builder->buffer, uncompressed_size ),
                "failed to append descriptor size.\n" );
        }
    }
    return entry;
}

static struct test_entry *add_entry( struct zip_builder *builder, const char *name,
                                     const BYTE *data, UINT32 data_length, UINT16 method,
                                     UINT64 uncompressed_size, UINT32 crc32, UINT16 flags,
                                     BOOL descriptor_signature, BOOL zip64 )
{
    return add_entry_with_extras( builder, name, data, data_length, method, uncompressed_size,
                                  crc32, flags, descriptor_signature, zip64,
                                  NULL, 0, NULL, 0 );
}

static BOOL finish_archive( struct zip_builder *builder, BOOL zip64_directory )
{
    UINT64 central_size;
    UINT32 i;
    BYTE *header, *extra;

    builder->central_offset = builder->buffer.size;
    for (i = 0; i < builder->count; i++)
    {
        struct test_entry *entry = builder->entries + i;
        UINT32 total_extra_length = (entry->zip64 ? 28 : 0) +
                                    entry->central_extra_length;
        UINT16 extra_length;
        BOOL directory = entry->name[entry->name_length - 1] == '/';

        if (total_extra_length > 0xffff) return FALSE;
        extra_length = total_extra_length;
        entry->central_offset = builder->buffer.size;
        header = append( &builder->buffer, 46 );
        if (!header) return FALSE;
        put_uint32( header, ZIP_CENTRAL_DIRECTORY_SIGNATURE );
        put_uint16( header + 4, 20 );
        put_uint16( header + 6, entry->zip64 ? 45 : entry->method == 8 ? 20 : 10 );
        put_uint16( header + 8, entry->flags );
        put_uint16( header + 10, entry->method );
        put_uint32( header + 16, entry->crc32 );
        put_uint32( header + 20, entry->zip64 ? 0xffffffff : entry->compressed_size );
        put_uint32( header + 24, entry->zip64 ? 0xffffffff : entry->uncompressed_size );
        put_uint16( header + 28, entry->name_length );
        put_uint16( header + 30, extra_length );
        put_uint32( header + 38, directory ? 0x10 : 0 );
        put_uint32( header + 42, entry->zip64 ? 0xffffffff : entry->local_offset );
        if (!append_bytes( &builder->buffer, entry->name, entry->name_length )) return FALSE;
        if (entry->zip64)
        {
            extra = append( &builder->buffer, 28 );
            if (!extra) return FALSE;
            put_uint16( extra, 1 );
            put_uint16( extra + 2, 24 );
            put_uint64( extra + 4, entry->uncompressed_size );
            put_uint64( extra + 12, entry->compressed_size );
            put_uint64( extra + 20, entry->local_offset );
        }
        if (!append_bytes( &builder->buffer, entry->central_extra,
                           entry->central_extra_length ))
            return FALSE;
    }
    central_size = builder->buffer.size - builder->central_offset;

    if (zip64_directory)
    {
        UINT64 zip64_offset = builder->buffer.size;

        if (!append_uint32( &builder->buffer, ZIP64_END_DIRECTORY_SIGNATURE ) ||
            !append_uint64( &builder->buffer, 44 ) ||
            !append_uint32( &builder->buffer, 45 | (45 << 16) ) ||
            !append_uint32( &builder->buffer, 0 ) ||
            !append_uint32( &builder->buffer, 0 ) ||
            !append_uint64( &builder->buffer, builder->count ) ||
            !append_uint64( &builder->buffer, builder->count ) ||
            !append_uint64( &builder->buffer, central_size ) ||
            !append_uint64( &builder->buffer, builder->central_offset ) ||
            !append_uint32( &builder->buffer, ZIP64_END_DIRECTORY_LOCATOR_SIGNATURE ) ||
            !append_uint32( &builder->buffer, 0 ) ||
            !append_uint64( &builder->buffer, zip64_offset ) ||
            !append_uint32( &builder->buffer, 1 ))
            return FALSE;
    }

    builder->eocd_offset = builder->buffer.size;
    header = append( &builder->buffer, 22 );
    if (!header) return FALSE;
    put_uint32( header, ZIP_END_DIRECTORY_SIGNATURE );
    put_uint16( header + 8, zip64_directory ? 0xffff : builder->count );
    put_uint16( header + 10, zip64_directory ? 0xffff : builder->count );
    put_uint32( header + 12, zip64_directory ? 0xffffffff : central_size );
    put_uint32( header + 16, zip64_directory ? 0xffffffff : builder->central_offset );
    return TRUE;
}

static void add_required_entries( struct zip_builder *builder )
{
    static const char *names[] =
    {
        "AppxManifest.xml",
        "AppxBlockMap.xml",
        "[Content_Types].xml",
        "AppxSignature.p7x",
    };
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(names); i++)
        add_entry( builder, names[i], NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
}

static HANDLE create_archive_file( const struct buffer *buffer, WCHAR *path )
{
    WCHAR directory[MAX_PATH];
    HANDLE file;
    DWORD written;

    if (!GetTempPathW( ARRAY_SIZE(directory), directory )) return INVALID_HANDLE_VALUE;
    if (!GetTempFileNameW( directory, L"wax", 0, path )) return INVALID_HANDLE_VALUE;
    file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL );
    if (file == INVALID_HANDLE_VALUE) return file;
    if (!WriteFile( file, buffer->data, buffer->size, &written, NULL ) || written != buffer->size)
    {
        CloseHandle( file );
        return INVALID_HANDLE_VALUE;
    }
    return file;
}

static HRESULT open_builder( struct zip_builder *builder,
                             const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
                             WINE_APPX_ARCHIVE **archive )
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    HRESULT hr;

    file = create_archive_file( &builder->buffer, path );
    ok( file != INVALID_HANDLE_VALUE, "failed to create archive file, error %lu.\n",
        GetLastError() );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    hr = p_wine_appx_archive_open( file, limits, flags, archive );
    CloseHandle( file );
    return hr;
}

static void expect_invalid( struct zip_builder *builder, UINT32 flags )
{
    WINE_APPX_ARCHIVE *archive = (WINE_APPX_ARCHIVE *)0xdeadbeef;
    HRESULT hr = open_builder( builder, NULL, flags, &archive );

    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "got hr %#lx.\n", hr );
    ok( !archive, "got archive %p.\n", archive );
    if (archive) p_wine_appx_archive_close( archive );
}

static void init_limits( WINE_APPX_ARCHIVE_LIMITS *limits )
{
    limits->size = sizeof(*limits);
    limits->max_entries = 65536;
    limits->max_archive_size = 128ULL * 1024 * 1024 * 1024;
    limits->max_central_directory_size = 64ULL * 1024 * 1024;
    limits->max_entry_compressed_size = 16ULL * 1024 * 1024 * 1024;
    limits->max_entry_uncompressed_size = 16ULL * 1024 * 1024 * 1024;
    limits->max_total_uncompressed_size = 64ULL * 1024 * 1024 * 1024;
    limits->max_compression_ratio = 1000;
    limits->compression_ratio_slack = 1024 * 1024;
}

static void test_valid_archive( BOOL zip64_directory, BOOL zip64_entry )
{
    static const BYTE empty_deflate[] = {0x03, 0x00};
    struct zip_builder builder = {0};
    WINE_APPX_ARCHIVE_ENTRY entry;
    WINE_APPX_ARCHIVE *archive;
    WCHAR path[64];
    UINT32 count, length, i;
    HRESULT hr;
    BOOL found = FALSE;

    add_required_entries( &builder );
    add_entry( &builder, "VFS/ProgramFilesX64/App/app.exe", empty_deflate,
               sizeof(empty_deflate), 8, 0, 0, ZIP_FLAG_UTF8, FALSE, zip64_entry );
    ok( finish_archive( &builder, zip64_directory ), "failed to finish archive.\n" );

    hr = open_builder( &builder, NULL, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        hr = p_wine_appx_archive_get_count( archive, &count );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( count == 5, "got count %u.\n", count );

        for (i = 0; i < count; i++)
        {
            memset( &entry, 0xcc, sizeof(entry) );
            entry.size = sizeof(entry);
            length = 0;
            hr = p_wine_appx_archive_get_entry( archive, i, &entry, &length, NULL );
            ok( hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), "got hr %#lx.\n", hr );
            ok( length > 1 && length <= ARRAY_SIZE(path), "got length %u.\n", length );
            if (length > ARRAY_SIZE(path)) continue;

            entry.size = sizeof(entry);
            hr = p_wine_appx_archive_get_entry( archive, i, &entry, &length, path );
            ok( hr == S_OK, "got hr %#lx.\n", hr );
            if (!lstrcmpW( path, L"VFS\\ProgramFilesX64\\App\\app.exe" ))
            {
                found = TRUE;
                ok( entry.compression_method == 8, "got method %u.\n",
                    entry.compression_method );
                ok( entry.compressed_size == 2, "got compressed size %s.\n",
                    wine_dbgstr_longlong(entry.compressed_size) );
                ok( !entry.uncompressed_size, "got uncompressed size %s.\n",
                    wine_dbgstr_longlong(entry.uncompressed_size) );
                ok( entry.data_offset > entry.local_header_offset,
                    "invalid data offset.\n" );
            }
        }
        ok( found, "payload entry was not found.\n" );

        entry.size = sizeof(entry);
        length = ARRAY_SIZE(path);
        hr = p_wine_appx_archive_get_entry( archive, count, &entry, &length, path );
        ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
        p_wine_appx_archive_close( archive );
    }
    free_builder( &builder );
}

static void test_descriptors( BOOL signature )
{
    struct zip_builder builder = {0};
    WINE_APPX_ARCHIVE *archive;
    struct test_entry *entry;
    HRESULT hr;

    add_required_entries( &builder );
    entry = add_entry( &builder, "Assets/empty.dat", NULL, 0, 0, 0, 0,
                       ZIP_FLAG_DATA_DESCRIPTOR, signature, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    hr = open_builder( &builder, NULL, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_wine_appx_archive_close( archive );

    builder.buffer.data[entry->descriptor_offset + (signature ? 4 : 0)] ^= 1;
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );
}

static void test_descriptor_signature_crc( void )
{
    struct zip_builder builder = {0};
    WINE_APPX_ARCHIVE *archive;
    HRESULT hr;

    add_required_entries( &builder );
    add_entry( &builder, "Assets/ambiguous.dat", NULL, 0, 0, 0,
               ZIP_DATA_DESCRIPTOR_SIGNATURE, ZIP_FLAG_DATA_DESCRIPTOR, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    hr = open_builder( &builder, NULL, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_wine_appx_archive_close( archive );
    free_builder( &builder );
}

static void test_descriptor_metadata( void )
{
    static const BYTE conflicting_zip64[] =
    {
        0x01, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    struct zip_builder builder = {0};
    struct test_entry *entry;
    WINE_APPX_ARCHIVE *archive;
    HRESULT hr;

    add_required_entries( &builder );
    add_entry( &builder, "Assets/zip64.dat", NULL, 0, 0, 0, 0,
               ZIP_FLAG_DATA_DESCRIPTOR, TRUE, TRUE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    hr = open_builder( &builder, NULL, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_wine_appx_archive_close( archive );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry( &builder, "Assets/zip32.dat", NULL, 0, 0, 0, 0,
                       ZIP_FLAG_DATA_DESCRIPTOR, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    put_uint32( builder.buffer.data + entry->local_offset + 18, 0xffffffff );
    put_uint32( builder.buffer.data + entry->local_offset + 22, 0xffffffff );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry( &builder, "Assets/missing.dat", NULL, 0, 0, 0, 0,
                       ZIP_FLAG_DATA_DESCRIPTOR, FALSE, TRUE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    put_uint32( builder.buffer.data + entry->local_offset + 18, 0xffffffff );
    put_uint32( builder.buffer.data + entry->local_offset + 22, 0xffffffff );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry_with_extras( &builder, "Assets/conflict.dat", (const BYTE *)"x", 1,
                                   0, 1, 0x8cdc1683, ZIP_FLAG_DATA_DESCRIPTOR,
                                   FALSE, TRUE, conflicting_zip64,
                                   sizeof(conflicting_zip64), NULL, 0 );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    put_uint32( builder.buffer.data + entry->local_offset + 18, 0xffffffff );
    put_uint32( builder.buffer.data + entry->local_offset + 22, 0xffffffff );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry( &builder, "Assets/", NULL, 0, 0, 0, 0,
               ZIP_FLAG_DATA_DESCRIPTOR, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );
}

static void test_extra_fields( void )
{
    static const BYTE unicode_path[] = {0x75, 0x70, 0x01, 0x00, 0x01};
    static const BYTE malformed[] = {0x34, 0x12, 0x05, 0x00, 0x00};
    static const BYTE empty_zip64[] = {0x01, 0x00, 0x00, 0x00};
    static const BYTE locator_like[] =
    {
        0xef, 0xbe, 0x14, 0x00,
        0x50, 0x4b, 0x06, 0x07,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    struct zip_builder builder = {0};

    add_required_entries( &builder );
    add_entry_with_extras( &builder, "Assets/central-name.dat", NULL, 0, 0, 0, 0, 0,
                           FALSE, FALSE, NULL, 0, unicode_path, sizeof(unicode_path) );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry_with_extras( &builder, "Assets/local-name.dat", NULL, 0, 0, 0, 0, 0,
                           FALSE, FALSE, unicode_path, sizeof(unicode_path), NULL, 0 );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry_with_extras( &builder, "Assets/malformed.dat", NULL, 0, 0, 0, 0,
                           ZIP_FLAG_DATA_DESCRIPTOR, FALSE, FALSE,
                           malformed, sizeof(malformed), NULL, 0 );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry_with_extras( &builder, "Assets/central-zip64.dat", NULL, 0, 0, 0, 0, 0,
                           FALSE, FALSE, NULL, 0, empty_zip64, sizeof(empty_zip64) );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry_with_extras( &builder, "Assets/local-zip64.dat", NULL, 0, 0, 0, 0, 0,
                           FALSE, FALSE, empty_zip64, sizeof(empty_zip64), NULL, 0 );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry_with_extras( &builder, "Assets/central-duplicate.dat", NULL, 0, 0, 0, 0,
                           0, FALSE, TRUE, NULL, 0, empty_zip64, sizeof(empty_zip64) );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry_with_extras( &builder, "Assets/local-duplicate.dat", NULL, 0, 0, 0, 0,
                           0, FALSE, TRUE, empty_zip64, sizeof(empty_zip64), NULL, 0 );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry_with_extras( &builder, "Assets/locator-like.dat", NULL, 0, 0, 0, 0, 0,
                           FALSE, FALSE, NULL, 0, locator_like, sizeof(locator_like) );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );
}

static void test_file_position( void )
{
    struct zip_builder builder = {0};
    WINE_APPX_ARCHIVE *archive;
    LARGE_INTEGER position, current;
    WCHAR path[MAX_PATH];
    HANDLE file;
    HRESULT hr;

    add_required_entries( &builder );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    file = create_archive_file( &builder.buffer, path );
    ok( file != INVALID_HANDLE_VALUE, "failed to create archive file, error %lu.\n",
        GetLastError() );
    position.QuadPart = 7;
    ok( SetFilePointerEx( file, position, NULL, FILE_BEGIN ),
        "failed to set file pointer, error %lu.\n", GetLastError() );
    hr = p_wine_appx_archive_open( file, NULL, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    position.QuadPart = 0;
    ok( SetFilePointerEx( file, position, &current, FILE_CURRENT ),
        "failed to query file pointer, error %lu.\n", GetLastError() );
    ok( current.QuadPart == 7, "got file pointer %s.\n",
        wine_dbgstr_longlong(current.QuadPart) );
    if (SUCCEEDED(hr)) p_wine_appx_archive_close( archive );
    CloseHandle( file );
    free_builder( &builder );

    add_required_entries( &builder );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    builder.buffer.data[builder.central_offset] ^= 1;
    file = create_archive_file( &builder.buffer, path );
    ok( file != INVALID_HANDLE_VALUE, "failed to create archive file, error %lu.\n",
        GetLastError() );
    position.QuadPart = 11;
    ok( SetFilePointerEx( file, position, NULL, FILE_BEGIN ),
        "failed to set file pointer, error %lu.\n", GetLastError() );
    archive = (WINE_APPX_ARCHIVE *)0xdeadbeef;
    hr = p_wine_appx_archive_open( file, NULL, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "got hr %#lx.\n", hr );
    ok( !archive, "got archive %p.\n", archive );
    position.QuadPart = 0;
    ok( SetFilePointerEx( file, position, &current, FILE_CURRENT ),
        "failed to query file pointer, error %lu.\n", GetLastError() );
    ok( current.QuadPart == 11, "got file pointer %s.\n",
        wine_dbgstr_longlong(current.QuadPart) );
    CloseHandle( file );
    free_builder( &builder );
}

static void test_file_sharing_contract( void )
{
    struct zip_builder builder = {0};
    WINE_APPX_ARCHIVE *archive = (WINE_APPX_ARCHIVE *)0xdeadbeef;
    WCHAR directory[MAX_PATH], path[MAX_PATH] = {0};
    HANDLE file;
    DWORD written = 0;
    HRESULT hr;

    add_required_entries( &builder );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    if (!GetTempPathW( ARRAY_SIZE(directory), directory ))
    {
        ok( 0, "failed to get temporary path, error %lu.\n", GetLastError() );
        goto done;
    }
    if (!GetTempFileNameW( directory, L"wax", 0, path ))
    {
        ok( 0, "failed to create temporary name, error %lu.\n", GetLastError() );
        goto done;
    }
    file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to create archive file, error %lu.\n",
        GetLastError() );
    if (file != INVALID_HANDLE_VALUE)
    {
        ok( WriteFile( file, builder.buffer.data, builder.buffer.size, &written, NULL ),
            "failed to write archive, error %lu.\n", GetLastError() );
        ok( written == builder.buffer.size, "wrote %lu bytes.\n", written );
        hr = p_wine_appx_archive_open( file, NULL, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
        ok( hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "got hr %#lx.\n", hr );
        ok( !archive, "got archive %p.\n", archive );
        CloseHandle( file );
    }
done:
    if (path[0]) DeleteFileW( path );
    free_builder( &builder );
}

static void test_path_sets( void )
{
    struct zip_builder builder = {0};

    add_required_entries( &builder );
    add_entry( &builder, "VFS/Foo.dll", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    add_entry( &builder, "vfs/foo.DLL", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry( &builder, "Assets", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    add_entry( &builder, "Assets/logo.png", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry( &builder, "Assets/", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    add_entry( &builder, "Assets/logo.png", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    {
        WINE_APPX_ARCHIVE *archive;
        HRESULT hr = open_builder( &builder, NULL, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        if (SUCCEEDED(hr)) p_wine_appx_archive_close( archive );
    }
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry( &builder, "../escape", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );
}

static void test_required_layout( void )
{
    struct zip_builder builder = {0};
    WINE_APPX_ARCHIVE *archive;
    HRESULT hr;

    add_entry( &builder, "AppxManifest.xml", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    hr = open_builder( &builder, NULL, 0, &archive );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_wine_appx_archive_close( archive );
    free_builder( &builder );

    add_entry( &builder, "AppxMetadata/AppxBundleManifest.xml", NULL, 0, 0, 0, 0,
               0, FALSE, FALSE );
    add_entry( &builder, "AppxBlockMap.xml", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    add_entry( &builder, "[Content_Types].xml", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    add_entry( &builder, "AppxSignature.p7x", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    hr = open_builder( &builder, NULL, WINE_APPX_ARCHIVE_OPEN_BUNDLE, &archive );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_wine_appx_archive_close( archive );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );
}

static void test_header_validation( void )
{
    static const BYTE invalid_utf8_name[] = {'A', 0xc3, 0x28, 0};
    struct zip_builder builder = {0};
    struct test_entry *entry;

    add_required_entries( &builder );
    entry = add_entry( &builder, "Payload.exe", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    put_uint16( builder.buffer.data + entry->central_offset + 8, 1 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry( &builder, "Payload.exe", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    put_uint16( builder.buffer.data + entry->central_offset + 10, 99 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry( &builder, "Payload.exe", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    builder.buffer.data[entry->local_offset + 30] ^= 1;
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry( &builder, "Payload.exe", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    put_uint16( builder.buffer.data + entry->local_offset + 4, 20 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry( &builder, "Payload.exe", NULL, 0, 0, 0, 0, 0, FALSE, TRUE );
    ok( finish_archive( &builder, TRUE ), "failed to finish archive.\n" );
    put_uint16( builder.buffer.data + entry->local_offset + 30 + entry->name_length + 2, 17 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry( &builder, "Payload.exe", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    put_uint32( builder.buffer.data + entry->central_offset + 42, 0 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    entry = add_entry( &builder, "Payload.exe", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    put_uint16( builder.buffer.data + entry->central_offset + 4, 0x031e );
    put_uint32( builder.buffer.data + entry->central_offset + 38, (UINT32)0120777 << 16 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry( &builder, (const char *)invalid_utf8_name, NULL, 0, 0, 0, 0, 0, FALSE, FALSE )
        ->name_length = sizeof(invalid_utf8_name) - 1;
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry( &builder, "Assets/", (const BYTE *)"x", 1, 0, 1, 0x8cdc1683,
               0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );
}

static void test_directory_records( void )
{
    struct zip_builder builder = {0};
    BYTE *eocd;

    add_required_entries( &builder );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    append( &builder.buffer, 1 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    eocd = builder.buffer.data + builder.eocd_offset;
    put_uint16( eocd + 4, 1 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    eocd = builder.buffer.data + builder.eocd_offset;
    put_uint32( eocd + 12, read_uint32(eocd + 12) - 1 );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    ok( finish_archive( &builder, TRUE ), "failed to finish archive.\n" );
    builder.buffer.data[builder.eocd_offset - 20] ^= 1;
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );
}

static void test_resource_limits( void )
{
    static const BYTE empty_deflate[] = {0x03, 0x00};
    struct zip_builder builder = {0};
    WINE_APPX_ARCHIVE_LIMITS limits;
    WINE_APPX_ARCHIVE *archive;
    HRESULT hr;

    add_required_entries( &builder );
    add_entry( &builder, "Payload.exe", NULL, 0, 0, 0, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    init_limits( &limits );
    limits.max_entries = 4;
    hr = open_builder( &builder, &limits, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "got hr %#lx.\n", hr );
    ok( !archive, "got archive %p.\n", archive );
    free_builder( &builder );

    add_required_entries( &builder );
    add_entry( &builder, "Bomb.dat", empty_deflate, sizeof(empty_deflate), 8,
               4 * 1024 * 1024, 0, 0, FALSE, FALSE );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    expect_invalid( &builder, WINE_APPX_ARCHIVE_OPEN_PACKAGE );
    free_builder( &builder );

    add_required_entries( &builder );
    ok( finish_archive( &builder, FALSE ), "failed to finish archive.\n" );
    init_limits( &limits );
    limits.size--;
    archive = (WINE_APPX_ARCHIVE *)0xdeadbeef;
    hr = open_builder( &builder, &limits, WINE_APPX_ARCHIVE_OPEN_PACKAGE, &archive );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( !archive, "got archive %p.\n", archive );
    free_builder( &builder );
}

static void test_arguments( void )
{
    WINE_APPX_ARCHIVE_ENTRY entry;
    WINE_APPX_ARCHIVE *archive = (WINE_APPX_ARCHIVE *)0xdeadbeef;
    UINT32 length = 0;
    HRESULT hr;

    hr = p_wine_appx_archive_open( INVALID_HANDLE_VALUE, NULL, 0, &archive );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( archive == (WINE_APPX_ARCHIVE *)0xdeadbeef, "archive was modified.\n" );

    hr = p_wine_appx_archive_open( GetCurrentProcess(), NULL, 0, &archive );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    hr = p_wine_appx_archive_open( GetCurrentProcess(), NULL,
                                  WINE_APPX_ARCHIVE_OPEN_PACKAGE |
                                  WINE_APPX_ARCHIVE_OPEN_BUNDLE, &archive );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    hr = p_wine_appx_archive_get_count( NULL, &length );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_wine_appx_archive_get_entry( NULL, 0, &entry, &length, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    p_wine_appx_archive_close( NULL );
}

START_TEST(archive)
{
    HMODULE module = LoadLibraryW( L"appxsvc.dll" );

    if (!module)
    {
        ok( 0, "appxsvc.dll is not available, error %lu.\n", GetLastError() );
        return;
    }

    p_wine_appx_archive_open = (void *)GetProcAddress( module, "wine_appx_archive_open" );
    p_wine_appx_archive_close = (void *)GetProcAddress( module, "wine_appx_archive_close" );
    p_wine_appx_archive_get_count =
        (void *)GetProcAddress( module, "wine_appx_archive_get_count" );
    p_wine_appx_archive_get_entry =
        (void *)GetProcAddress( module, "wine_appx_archive_get_entry" );
    if (!p_wine_appx_archive_open || !p_wine_appx_archive_close ||
        !p_wine_appx_archive_get_count || !p_wine_appx_archive_get_entry)
    {
        ok( 0, "archive exports are not available, error %lu.\n", GetLastError() );
        FreeLibrary( module );
        return;
    }

    test_valid_archive( FALSE, FALSE );
    test_valid_archive( TRUE, FALSE );
    test_valid_archive( TRUE, TRUE );
    test_descriptors( FALSE );
    test_descriptors( TRUE );
    test_descriptor_signature_crc();
    test_descriptor_metadata();
    test_extra_fields();
    test_file_position();
    test_file_sharing_contract();
    test_path_sets();
    test_required_layout();
    test_header_validation();
    test_directory_records();
    test_resource_limits();
    test_arguments();

    FreeLibrary( module );
}
