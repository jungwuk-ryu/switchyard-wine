/*
 * AppX archive entry stream tests
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
#define ZIP_CENTRAL_DIRECTORY_SIGNATURE          0x02014b50
#define ZIP_END_DIRECTORY_SIGNATURE              0x06054b50

#define ZIP_METHOD_STORE                          0
#define ZIP_METHOD_DEFLATE                        8

static HRESULT (WINAPI *p_wine_appx_archive_open)( HANDLE, const WINE_APPX_ARCHIVE_LIMITS *,
                                                   UINT32, WINE_APPX_ARCHIVE ** );
static void (WINAPI *p_wine_appx_archive_close)( WINE_APPX_ARCHIVE * );
static HRESULT (WINAPI *p_wine_appx_archive_stream_open)( WINE_APPX_ARCHIVE *, UINT32,
                                                          WINE_APPX_ARCHIVE_STREAM ** );
static HRESULT (WINAPI *p_wine_appx_archive_stream_read)( WINE_APPX_ARCHIVE_STREAM *, void *,
                                                          UINT32, UINT32 * );
static void (WINAPI *p_wine_appx_archive_stream_cancel)( WINE_APPX_ARCHIVE_STREAM * );
static void (WINAPI *p_wine_appx_archive_stream_close)( WINE_APPX_ARCHIVE_STREAM * );

struct buffer
{
    BYTE *data;
    UINT32 size;
    UINT32 capacity;
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
    if (!(destination = append( buffer, length ))) return FALSE;
    memcpy( destination, data, length );
    return TRUE;
}

static UINT32 calculate_crc32( const BYTE *data, UINT32 size )
{
    UINT32 crc = ~0u, i, bit;

    for (i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & -(INT32)(crc & 1));
    }
    return ~crc;
}

static BOOL build_archive( struct buffer *archive, const char *name, UINT16 method,
                           const BYTE *compressed, UINT32 compressed_size,
                           UINT32 uncompressed_size, UINT32 crc32 )
{
    UINT16 name_length = strlen( name );
    UINT16 version = method == ZIP_METHOD_DEFLATE ? 20 : 10;
    UINT32 central_offset, central_size;
    BYTE *header;

    if (!(header = append( archive, 30 ))) return FALSE;
    put_uint32( header, ZIP_LOCAL_FILE_HEADER_SIGNATURE );
    put_uint16( header + 4, version );
    put_uint16( header + 8, method );
    put_uint32( header + 14, crc32 );
    put_uint32( header + 18, compressed_size );
    put_uint32( header + 22, uncompressed_size );
    put_uint16( header + 26, name_length );
    if (!append_bytes( archive, name, name_length ) ||
        !append_bytes( archive, compressed, compressed_size ))
        return FALSE;

    central_offset = archive->size;
    if (!(header = append( archive, 46 ))) return FALSE;
    put_uint32( header, ZIP_CENTRAL_DIRECTORY_SIGNATURE );
    put_uint16( header + 4, 20 );
    put_uint16( header + 6, version );
    put_uint16( header + 10, method );
    put_uint32( header + 16, crc32 );
    put_uint32( header + 20, compressed_size );
    put_uint32( header + 24, uncompressed_size );
    put_uint16( header + 28, name_length );
    if (name_length && name[name_length - 1] == '/') put_uint32( header + 38, 0x10 );
    if (!append_bytes( archive, name, name_length )) return FALSE;
    central_size = archive->size - central_offset;

    if (!(header = append( archive, 22 ))) return FALSE;
    put_uint32( header, ZIP_END_DIRECTORY_SIGNATURE );
    put_uint16( header + 8, 1 );
    put_uint16( header + 10, 1 );
    put_uint32( header + 12, central_size );
    put_uint32( header + 16, central_offset );
    return TRUE;
}

static BOOL build_stored_deflate( struct buffer *compressed, const BYTE *data, UINT32 size )
{
    UINT32 offset = 0;

    do
    {
        UINT32 count = size - offset;
        BOOL final;
        BYTE *header;

        if (count > 0xffff) count = 0xffff;
        final = offset + count == size;
        if (!(header = append( compressed, 5 ))) return FALSE;
        header[0] = final;
        put_uint16( header + 1, count );
        put_uint16( header + 3, ~count );
        if (!append_bytes( compressed, data + offset, count )) return FALSE;
        offset += count;
    } while (offset < size);

    return TRUE;
}

static HRESULT open_archive( const struct buffer *buffer, WINE_APPX_ARCHIVE **archive )
{
    WCHAR directory[MAX_PATH], path[MAX_PATH];
    HANDLE file;
    DWORD written;
    HRESULT hr;

    if (!GetTempPathW( ARRAY_SIZE(directory), directory ))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!GetTempFileNameW( directory, L"was", 0, path ))
        return HRESULT_FROM_WIN32( GetLastError() );
    file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    if (!WriteFile( file, buffer->data, buffer->size, &written, NULL ) ||
        written != buffer->size)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        CloseHandle( file );
        return hr;
    }
    hr = p_wine_appx_archive_open( file, NULL, 0, archive );
    CloseHandle( file );
    return hr;
}

static HRESULT open_stream( const char *name, UINT16 method, const BYTE *compressed,
                            UINT32 compressed_size, UINT32 uncompressed_size, UINT32 crc32,
                            WINE_APPX_ARCHIVE **archive,
                            WINE_APPX_ARCHIVE_STREAM **stream )
{
    struct buffer buffer = {0};
    HRESULT hr;

    *archive = NULL;
    *stream = NULL;
    ok( build_archive( &buffer, name, method, compressed, compressed_size,
                       uncompressed_size, crc32 ), "failed to build archive.\n" );
    hr = open_archive( &buffer, archive );
    ok( hr == S_OK, "failed to open archive, hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
        hr = p_wine_appx_archive_stream_open( *archive, 0, stream );
    HeapFree( GetProcessHeap(), 0, buffer.data );
    return hr;
}

static void close_stream( WINE_APPX_ARCHIVE *archive, WINE_APPX_ARCHIVE_STREAM *stream )
{
    if (stream) p_wine_appx_archive_stream_close( stream );
    if (archive) p_wine_appx_archive_close( archive );
}

static void test_stored_chunks( void )
{
    BYTE source[150003], *result;
    WINE_APPX_ARCHIVE_STREAM *stream;
    WINE_APPX_ARCHIVE *archive;
    static const UINT32 capacities[] = {1, 7, 4093, 65536, 3};
    UINT32 i, offset = 0, read;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(source); i++) source[i] = i * 29 + (i >> 8);
    result = HeapAlloc( GetProcessHeap(), 0, sizeof(source) );
    ok( !!result, "failed to allocate result.\n" );
    if (!result) return;

    hr = open_stream( "payload.bin", ZIP_METHOD_STORE, source, sizeof(source),
                      sizeof(source), calculate_crc32( source, sizeof(source) ),
                      &archive, &stream );
    ok( hr == S_OK, "failed to open stream, hr %#lx.\n", hr );
    while (SUCCEEDED(hr) && hr != S_FALSE)
    {
        UINT32 capacity = capacities[offset % ARRAY_SIZE(capacities)];

        if (capacity > sizeof(source) - offset) capacity = sizeof(source) - offset;
        if (!capacity) capacity = 1;
        read = 0xdeadbeef;
        hr = p_wine_appx_archive_stream_read( stream, result + offset, capacity, &read );
        if (hr == S_OK)
        {
            ok( read && read <= capacity, "read %u bytes with capacity %u.\n", read, capacity );
            offset += read;
        }
    }
    ok( hr == S_FALSE, "got final hr %#lx.\n", hr );
    ok( offset == sizeof(source), "read %u bytes.\n", offset );
    ok( !memcmp( source, result, sizeof(source) ), "stored data differs.\n" );
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, result, 1, &read );
    ok( hr == S_FALSE, "got repeated EOF hr %#lx.\n", hr );
    ok( !read, "read %u bytes after EOF.\n", read );

    close_stream( archive, stream );
    HeapFree( GetProcessHeap(), 0, result );
}

static void test_deflate_chunks( void )
{
    struct buffer compressed = {0};
    BYTE source[150003], *result;
    WINE_APPX_ARCHIVE_STREAM *stream;
    WINE_APPX_ARCHIVE *archive;
    static const UINT32 capacities[] = {1, 2, 17, 4091, 65535};
    UINT32 i, iteration = 0, offset = 0, read;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(source); i++) source[i] = i * 13 ^ (i >> 3);
    ok( build_stored_deflate( &compressed, source, sizeof(source) ),
        "failed to build raw deflate stream.\n" );
    result = HeapAlloc( GetProcessHeap(), 0, sizeof(source) );
    ok( !!result, "failed to allocate result.\n" );
    if (!result) goto done;

    hr = open_stream( "payload.bin", ZIP_METHOD_DEFLATE,
                      compressed.data, compressed.size, sizeof(source),
                      calculate_crc32( source, sizeof(source) ), &archive, &stream );
    ok( hr == S_OK, "failed to open stream, hr %#lx.\n", hr );
    while (SUCCEEDED(hr) && hr != S_FALSE)
    {
        UINT32 capacity = capacities[iteration++ % ARRAY_SIZE(capacities)];

        if (capacity > sizeof(source) - offset) capacity = sizeof(source) - offset;
        if (!capacity) capacity = 1;
        read = 0xdeadbeef;
        hr = p_wine_appx_archive_stream_read( stream, result + offset, capacity, &read );
        if (hr == S_OK)
        {
            ok( read && read <= capacity, "read %u bytes with capacity %u.\n", read, capacity );
            offset += read;
        }
    }
    ok( hr == S_FALSE, "got final hr %#lx.\n", hr );
    ok( offset == sizeof(source), "read %u bytes.\n", offset );
    ok( !memcmp( source, result, sizeof(source) ), "inflated data differs.\n" );

    close_stream( archive, stream );
    HeapFree( GetProcessHeap(), 0, result );
done:
    HeapFree( GetProcessHeap(), 0, compressed.data );
}

static void test_small_and_empty_entries( void )
{
    static const BYTE hello[] = "hello";
    static const BYTE deflated_hello[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};
    static const BYTE deflated_empty[] = {0x03, 0x00};
    WINE_APPX_ARCHIVE_STREAM *stream;
    WINE_APPX_ARCHIVE *archive;
    BYTE output[8] = {0};
    UINT32 read, i;
    HRESULT hr;

    hr = open_stream( "hello.txt", ZIP_METHOD_DEFLATE, deflated_hello,
                      sizeof(deflated_hello), sizeof(hello) - 1,
                      calculate_crc32( hello, sizeof(hello) - 1 ), &archive, &stream );
    ok( hr == S_OK, "failed to open stream, hr %#lx.\n", hr );
    for (i = 0; i < sizeof(hello) - 1; i++)
    {
        read = 0;
        hr = p_wine_appx_archive_stream_read( stream, output + i, 1, &read );
        ok( hr == S_OK, "got hr %#lx at byte %u.\n", hr, i );
        ok( read == 1, "read %u bytes.\n", read );
    }
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output, 1, &read );
    ok( hr == S_FALSE, "got EOF hr %#lx.\n", hr );
    ok( !read, "read %u bytes at EOF.\n", read );
    ok( !memcmp( output, hello, sizeof(hello) - 1 ), "hello data differs.\n" );
    close_stream( archive, stream );

    hr = open_stream( "empty.bin", ZIP_METHOD_STORE, NULL, 0, 0, 0, &archive, &stream );
    ok( hr == S_OK, "failed to open empty stored stream, hr %#lx.\n", hr );
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == S_FALSE, "got empty stored hr %#lx.\n", hr );
    ok( !read, "read %u bytes.\n", read );
    close_stream( archive, stream );

    hr = open_stream( "empty.bin", ZIP_METHOD_DEFLATE, deflated_empty,
                      sizeof(deflated_empty), 0, 0, &archive, &stream );
    ok( hr == S_OK, "failed to open empty deflate stream, hr %#lx.\n", hr );
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == S_FALSE, "got empty deflate hr %#lx.\n", hr );
    ok( !read, "read %u bytes.\n", read );
    close_stream( archive, stream );
}

static void expect_stream_error( UINT16 method, const BYTE *compressed, UINT32 compressed_size,
                                 UINT32 uncompressed_size, UINT32 crc32 )
{
    WINE_APPX_ARCHIVE_STREAM *stream;
    WINE_APPX_ARCHIVE *archive;
    BYTE output[32];
    UINT32 read;
    HRESULT hr;

    hr = open_stream( "bad.bin", method, compressed, compressed_size,
                      uncompressed_size, crc32, &archive, &stream );
    ok( hr == S_OK, "failed to open stream, hr %#lx.\n", hr );
    if (FAILED(hr))
    {
        close_stream( archive, stream );
        return;
    }

    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == APPX_E_CORRUPT_CONTENT, "got corruption hr %#lx.\n", hr );
    ok( !read, "reported %u bytes on failure.\n", read );
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == APPX_E_CORRUPT_CONTENT, "got non-sticky hr %#lx.\n", hr );
    ok( !read, "reported %u bytes after failure.\n", read );
    close_stream( archive, stream );
}

static void test_corrupt_content( void )
{
    static const BYTE hello[] = "hello";
    static const BYTE deflated_hello[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};
    static const BYTE trailing[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x00};
    static const BYTE invalid[] = {0x07};
    UINT32 crc = calculate_crc32( hello, sizeof(hello) - 1 );

    expect_stream_error( ZIP_METHOD_STORE, hello, sizeof(hello) - 1,
                         sizeof(hello) - 1, crc ^ 1 );
    expect_stream_error( ZIP_METHOD_DEFLATE, deflated_hello,
                         sizeof(deflated_hello) - 1, sizeof(hello) - 1, crc );
    expect_stream_error( ZIP_METHOD_DEFLATE, trailing, sizeof(trailing),
                         sizeof(hello) - 1, crc );
    expect_stream_error( ZIP_METHOD_DEFLATE, deflated_hello, sizeof(deflated_hello),
                         sizeof(hello) - 2, crc );
    expect_stream_error( ZIP_METHOD_DEFLATE, deflated_hello, sizeof(deflated_hello),
                         sizeof(hello), crc );
    expect_stream_error( ZIP_METHOD_DEFLATE, invalid, sizeof(invalid), 1, 0 );
}

static void test_partial_crc_failure( void )
{
    static const BYTE hello[] = "hello";
    WINE_APPX_ARCHIVE_STREAM *stream;
    WINE_APPX_ARCHIVE *archive;
    BYTE output[8];
    UINT32 read;
    HRESULT hr;

    hr = open_stream( "bad-crc.bin", ZIP_METHOD_STORE, hello, sizeof(hello) - 1,
                      sizeof(hello) - 1,
                      calculate_crc32( hello, sizeof(hello) - 1 ) ^ 1, &archive, &stream );
    ok( hr == S_OK, "failed to open stream, hr %#lx.\n", hr );
    read = 0;
    hr = p_wine_appx_archive_stream_read( stream, output, 4, &read );
    ok( hr == S_OK, "got first read hr %#lx.\n", hr );
    ok( read == 4, "read %u bytes.\n", read );
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output + 4, 1, &read );
    ok( hr == APPX_E_CORRUPT_CONTENT, "got final read hr %#lx.\n", hr );
    ok( !read, "reported %u bytes for the failing chunk.\n", read );
    close_stream( archive, stream );
}

static void test_cancellation( void )
{
    BYTE source[1024], output[32];
    WINE_APPX_ARCHIVE_STREAM *stream;
    WINE_APPX_ARCHIVE *archive;
    UINT32 read, i;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(source); i++) source[i] = i;
    hr = open_stream( "cancel.bin", ZIP_METHOD_STORE, source, sizeof(source), sizeof(source),
                      calculate_crc32( source, sizeof(source) ), &archive, &stream );
    ok( hr == S_OK, "failed to open stream, hr %#lx.\n", hr );
    p_wine_appx_archive_stream_cancel( stream );
    p_wine_appx_archive_stream_cancel( stream );
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED), "got cancellation hr %#lx.\n", hr );
    ok( !read, "read %u bytes.\n", read );
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED), "got non-sticky hr %#lx.\n", hr );
    ok( !read, "read %u bytes.\n", read );
    close_stream( archive, stream );

    hr = open_stream( "cancel.bin", ZIP_METHOD_STORE, source, sizeof(source), sizeof(source),
                      calculate_crc32( source, sizeof(source) ), &archive, &stream );
    ok( hr == S_OK, "failed to open stream, hr %#lx.\n", hr );
    read = 0;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == S_OK, "got first read hr %#lx.\n", hr );
    ok( read == sizeof(output), "read %u bytes.\n", read );
    p_wine_appx_archive_stream_cancel( stream );
    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED), "got cancellation hr %#lx.\n", hr );
    ok( !read, "read %u bytes.\n", read );
    close_stream( archive, stream );

    p_wine_appx_archive_stream_cancel( NULL );
    p_wine_appx_archive_stream_close( NULL );
}

static void test_archive_lifetime( void )
{
    static const BYTE payload[] = "independent stream";
    WINE_APPX_ARCHIVE_STREAM *stream;
    WINE_APPX_ARCHIVE *archive;
    BYTE output[sizeof(payload)];
    UINT32 read;
    HRESULT hr;

    hr = open_stream( "lifetime.bin", ZIP_METHOD_STORE, payload, sizeof(payload) - 1,
                      sizeof(payload) - 1, calculate_crc32( payload, sizeof(payload) - 1 ),
                      &archive, &stream );
    ok( hr == S_OK, "failed to open stream, hr %#lx.\n", hr );
    p_wine_appx_archive_close( archive );
    archive = NULL;
    read = 0;
    hr = p_wine_appx_archive_stream_read( stream, output, sizeof(output), &read );
    ok( hr == S_OK, "got read hr %#lx.\n", hr );
    ok( read == sizeof(payload) - 1, "read %u bytes.\n", read );
    ok( !memcmp( output, payload, read ), "stream did not survive archive close.\n" );
    close_stream( archive, stream );
}

static void test_arguments( void )
{
    static const BYTE payload[] = "x";
    struct buffer buffer = {0};
    WINE_APPX_ARCHIVE_STREAM *stream = (WINE_APPX_ARCHIVE_STREAM *)0xdeadbeef;
    WINE_APPX_ARCHIVE *archive = NULL;
    BYTE output;
    UINT32 read;
    HRESULT hr;

    hr = p_wine_appx_archive_stream_open( NULL, 0, &stream );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( !stream, "got stream %p.\n", stream );
    hr = p_wine_appx_archive_stream_open( NULL, 0, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    ok( build_archive( &buffer, "payload.bin", ZIP_METHOD_STORE,
                       payload, sizeof(payload) - 1, sizeof(payload) - 1,
                       calculate_crc32( payload, sizeof(payload) - 1 ) ),
        "failed to build archive.\n" );
    hr = open_archive( &buffer, &archive );
    ok( hr == S_OK, "failed to open archive, hr %#lx.\n", hr );
    stream = (WINE_APPX_ARCHIVE_STREAM *)0xdeadbeef;
    hr = p_wine_appx_archive_stream_open( archive, 1, &stream );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
    ok( !stream, "got stream %p.\n", stream );
    hr = p_wine_appx_archive_stream_open( archive, 0, &stream );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    read = 0xdeadbeef;
    hr = p_wine_appx_archive_stream_read( NULL, &output, 1, &read );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_wine_appx_archive_stream_read( stream, NULL, 1, &read );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_wine_appx_archive_stream_read( stream, &output, 0, &read );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_wine_appx_archive_stream_read( stream, &output, 1, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    close_stream( archive, stream );
    HeapFree( GetProcessHeap(), 0, buffer.data );

    memset( &buffer, 0, sizeof(buffer) );
    ok( build_archive( &buffer, "directory/", ZIP_METHOD_STORE, NULL, 0, 0, 0 ),
        "failed to build directory archive.\n" );
    hr = open_archive( &buffer, &archive );
    ok( hr == S_OK, "failed to open archive, hr %#lx.\n", hr );
    stream = (WINE_APPX_ARCHIVE_STREAM *)0xdeadbeef;
    hr = p_wine_appx_archive_stream_open( archive, 0, &stream );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DIRECTORY), "got directory hr %#lx.\n", hr );
    ok( !stream, "got stream %p.\n", stream );
    close_stream( archive, stream );
    HeapFree( GetProcessHeap(), 0, buffer.data );
}

START_TEST(stream)
{
    HMODULE module = LoadLibraryW( L"appxsvc.dll" );

    if (!module)
    {
        ok( 0, "appxsvc.dll is not available, error %lu.\n", GetLastError() );
        return;
    }

    p_wine_appx_archive_open = (void *)GetProcAddress( module, "wine_appx_archive_open" );
    p_wine_appx_archive_close = (void *)GetProcAddress( module, "wine_appx_archive_close" );
    p_wine_appx_archive_stream_open =
        (void *)GetProcAddress( module, "wine_appx_archive_stream_open" );
    p_wine_appx_archive_stream_read =
        (void *)GetProcAddress( module, "wine_appx_archive_stream_read" );
    p_wine_appx_archive_stream_cancel =
        (void *)GetProcAddress( module, "wine_appx_archive_stream_cancel" );
    p_wine_appx_archive_stream_close =
        (void *)GetProcAddress( module, "wine_appx_archive_stream_close" );
    if (!p_wine_appx_archive_open || !p_wine_appx_archive_close ||
        !p_wine_appx_archive_stream_open || !p_wine_appx_archive_stream_read ||
        !p_wine_appx_archive_stream_cancel || !p_wine_appx_archive_stream_close)
    {
        ok( 0, "archive stream exports are not available, error %lu.\n", GetLastError() );
        FreeLibrary( module );
        return;
    }

    test_stored_chunks();
    test_deflate_chunks();
    test_small_and_empty_entries();
    test_corrupt_content();
    test_partial_crc_failure();
    test_cancellation();
    test_archive_lifetime();
    test_arguments();

    FreeLibrary( module );
}
