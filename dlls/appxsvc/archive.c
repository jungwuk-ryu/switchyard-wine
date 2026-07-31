/*
 * AppX archive index
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
#include "winnls.h"
#include "winerror.h"
#include "bcrypt.h"

#include "signature.h"
#include "wine/appxsvc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define ZIP_LOCAL_FILE_HEADER_SIGNATURE          0x04034b50
#define ZIP_DATA_DESCRIPTOR_SIGNATURE            0x08074b50
#define ZIP_CENTRAL_DIRECTORY_SIGNATURE          0x02014b50
#define ZIP64_END_DIRECTORY_SIGNATURE            0x06064b50
#define ZIP64_END_DIRECTORY_LOCATOR_SIGNATURE    0x07064b50
#define ZIP_END_DIRECTORY_SIGNATURE              0x06054b50

#define ZIP_LOCAL_FILE_HEADER_SIZE                30
#define ZIP_CENTRAL_DIRECTORY_HEADER_SIZE         46
#define ZIP64_END_DIRECTORY_MIN_SIZE              56
#define ZIP64_END_DIRECTORY_LOCATOR_SIZE          20
#define ZIP_END_DIRECTORY_SIZE                    22
#define ZIP_END_DIRECTORY_MAX_SEARCH              (ZIP_END_DIRECTORY_SIZE + 0xffff)

#define ZIP_FLAG_ENCRYPTED                        0x0001
#define ZIP_FLAG_COMPRESSION_OPTIONS              0x0006
#define ZIP_FLAG_DATA_DESCRIPTOR                  0x0008
#define ZIP_FLAG_PATCHED_DATA                     0x0020
#define ZIP_FLAG_STRONG_ENCRYPTION                0x0040
#define ZIP_FLAG_UTF8                             0x0800
#define ZIP_FLAG_MASKED_HEADER                    0x2000
#define ZIP_ALLOWED_FLAGS                         (ZIP_FLAG_COMPRESSION_OPTIONS | \
                                                   ZIP_FLAG_DATA_DESCRIPTOR | ZIP_FLAG_UTF8)

#define ZIP_METHOD_STORE                          0
#define ZIP_METHOD_DEFLATE                        8
#define ZIP_EXTRA_ZIP64                           0x0001

#define DEFAULT_MAX_ENTRIES                       65536
#define DEFAULT_MAX_ARCHIVE_SIZE                  (128ULL * 1024 * 1024 * 1024)
#define DEFAULT_MAX_CENTRAL_DIRECTORY_SIZE        (64ULL * 1024 * 1024)
#define DEFAULT_MAX_ENTRY_COMPRESSED_SIZE         (16ULL * 1024 * 1024 * 1024)
#define DEFAULT_MAX_ENTRY_UNCOMPRESSED_SIZE       (16ULL * 1024 * 1024 * 1024)
#define DEFAULT_MAX_TOTAL_UNCOMPRESSED_SIZE       (64ULL * 1024 * 1024 * 1024)
#define DEFAULT_MAX_COMPRESSION_RATIO             1000
#define DEFAULT_COMPRESSION_RATIO_SLACK           (1024 * 1024)

#define HASH_IO_BUFFER_SIZE                       (64 * 1024)

struct archive_entry
{
    WCHAR *path;
    BYTE *name;
    UINT32 path_length;
    UINT32 name_length;
    UINT32 flags;
    UINT32 crc32;
    UINT16 compression_method;
    UINT16 zip_flags;
    UINT16 version_needed;
    UINT16 modified_time;
    UINT16 modified_date;
    UINT64 compressed_size;
    UINT64 uncompressed_size;
    UINT64 local_header_offset;
    UINT64 central_header_offset;
    UINT64 central_header_size;
    UINT64 data_offset;
    UINT64 record_end;
    BOOL zip64_sizes;
};

struct wine_appx_archive
{
    HANDLE file;
    UINT64 file_size;
    UINT64 central_directory_offset;
    UINT64 central_directory_size;
    UINT64 zip64_extensible_offset;
    UINT64 zip64_extensible_size;
    UINT32 count;
    UINT16 zip64_version_made;
    UINT16 zip64_version_needed;
    BOOL zip64_directory;
    struct archive_entry *entries;
};

struct directory_info
{
    UINT64 offset;
    UINT64 size;
    UINT64 entries;
    UINT64 boundary;
    UINT64 zip64_extensible_offset;
    UINT64 zip64_extensible_size;
    UINT16 zip64_version_made;
    UINT16 zip64_version_needed;
    BOOL zip64;
};

static UINT16 read_uint16( const BYTE *buffer )
{
    return buffer[0] | ((UINT16)buffer[1] << 8);
}

static UINT32 read_uint32( const BYTE *buffer )
{
    return read_uint16( buffer ) | ((UINT32)read_uint16( buffer + 2 ) << 16);
}

static UINT64 read_uint64( const BYTE *buffer )
{
    return read_uint32( buffer ) | ((UINT64)read_uint32( buffer + 4 ) << 32);
}

static void write_uint16( BYTE *buffer, UINT16 value )
{
    buffer[0] = value;
    buffer[1] = value >> 8;
}

static void write_uint32( BYTE *buffer, UINT32 value )
{
    write_uint16( buffer, value );
    write_uint16( buffer + 2, value >> 16 );
}

static void write_uint64( BYTE *buffer, UINT64 value )
{
    write_uint32( buffer, value );
    write_uint32( buffer + 4, value >> 32 );
}

static BOOL add_uint64( UINT64 left, UINT64 right, UINT64 *result )
{
    if (~(UINT64)0 - left < right) return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL multiply_uint64( UINT64 left, UINT64 right, UINT64 *result )
{
    if (left && ~(UINT64)0 / left < right) return FALSE;
    *result = left * right;
    return TRUE;
}

static HRESULT read_at( HANDLE file, UINT64 offset, void *buffer, SIZE_T size )
{
    OVERLAPPED overlapped = {0};
    BYTE *cursor = buffer;
    HRESULT hr = S_OK;

    if (!(overlapped.hEvent = CreateEventW( NULL, TRUE, FALSE, NULL )))
        return HRESULT_FROM_WIN32( GetLastError() );

    while (size)
    {
        DWORD chunk = size > MAXDWORD ? MAXDWORD : size;
        DWORD error, read = 0;

        overlapped.Offset = offset;
        overlapped.OffsetHigh = offset >> 32;
        ResetEvent( overlapped.hEvent );
        if (!ReadFile( file, cursor, chunk, &read, &overlapped ))
        {
            error = GetLastError();
            if (error != ERROR_IO_PENDING ||
                !GetOverlappedResult( file, &overlapped, &read, TRUE ))
            {
                error = GetLastError();
                hr = error == ERROR_HANDLE_EOF ? APPX_E_INVALID_PACKAGING_LAYOUT :
                                                 HRESULT_FROM_WIN32( error );
                break;
            }
        }
        if (read != chunk)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            break;
        }
        cursor += read;
        size -= read;
        offset += read;
    }
    CloseHandle( overlapped.hEvent );
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

    if ((status = BCryptHashData( context->hash, (BYTE *)data, size, 0 )))
        return HRESULT_FROM_NT( status );
    return S_OK;
}

static HRESULT sha256_finish( struct sha256_context *context,
                              BYTE digest[APPX_SIGNATURE_SHA256_SIZE] )
{
    NTSTATUS status;

    if ((status = BCryptFinishHash( context->hash, digest,
                                    APPX_SIGNATURE_SHA256_SIZE, 0 )))
        return HRESULT_FROM_NT( status );
    return S_OK;
}

static HRESULT hash_file_range( HANDLE file, UINT64 offset, UINT64 size,
                                struct sha256_context *hash )
{
    OVERLAPPED overlapped = {0};
    BYTE *buffer;
    HRESULT hr;

    if (!(buffer = HeapAlloc( GetProcessHeap(), 0, HASH_IO_BUFFER_SIZE )))
        return E_OUTOFMEMORY;
    if (!(overlapped.hEvent = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    hr = S_OK;

    while (size)
    {
        UINT32 chunk = size > HASH_IO_BUFFER_SIZE ? HASH_IO_BUFFER_SIZE : (UINT32)size;
        DWORD error, read = 0;
        UINT64 next_offset;

        if (!add_uint64( offset, chunk, &next_offset ))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            break;
        }

        overlapped.Offset = offset;
        overlapped.OffsetHigh = offset >> 32;
        ResetEvent( overlapped.hEvent );
        if (!ReadFile( file, buffer, chunk, &read, &overlapped ))
        {
            error = GetLastError();
            if (error != ERROR_IO_PENDING ||
                !GetOverlappedResult( file, &overlapped, &read, TRUE ))
            {
                error = GetLastError();
                hr = error == ERROR_HANDLE_EOF ? APPX_E_INVALID_PACKAGING_LAYOUT :
                                                 HRESULT_FROM_WIN32( error );
                break;
            }
        }
        if (read != chunk)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            break;
        }
        if (FAILED(hr = sha256_update( hash, buffer, chunk ))) break;
        offset = next_offset;
        size -= chunk;
    }

done:
    if (overlapped.hEvent) CloseHandle( overlapped.hEvent );
    HeapFree( GetProcessHeap(), 0, buffer );
    return hr;
}

static HRESULT validate_limits( const WINE_APPX_ARCHIVE_LIMITS *input,
                                WINE_APPX_ARCHIVE_LIMITS *limits )
{
    memset( limits, 0, sizeof(*limits) );
    limits->size = sizeof(*limits);
    limits->max_entries = DEFAULT_MAX_ENTRIES;
    limits->max_archive_size = DEFAULT_MAX_ARCHIVE_SIZE;
    limits->max_central_directory_size = DEFAULT_MAX_CENTRAL_DIRECTORY_SIZE;
    limits->max_entry_compressed_size = DEFAULT_MAX_ENTRY_COMPRESSED_SIZE;
    limits->max_entry_uncompressed_size = DEFAULT_MAX_ENTRY_UNCOMPRESSED_SIZE;
    limits->max_total_uncompressed_size = DEFAULT_MAX_TOTAL_UNCOMPRESSED_SIZE;
    limits->max_compression_ratio = DEFAULT_MAX_COMPRESSION_RATIO;
    limits->compression_ratio_slack = DEFAULT_COMPRESSION_RATIO_SLACK;

    if (!input) return S_OK;
    if (input->size != sizeof(*input) || !input->max_entries || !input->max_archive_size ||
        !input->max_central_directory_size || !input->max_entry_compressed_size ||
        !input->max_entry_uncompressed_size || !input->max_total_uncompressed_size ||
        !input->max_compression_ratio || input->max_entries > DEFAULT_MAX_ENTRIES ||
        input->max_archive_size > DEFAULT_MAX_ARCHIVE_SIZE ||
        input->max_central_directory_size > DEFAULT_MAX_CENTRAL_DIRECTORY_SIZE ||
        input->max_entry_compressed_size > DEFAULT_MAX_ENTRY_COMPRESSED_SIZE ||
        input->max_entry_uncompressed_size > DEFAULT_MAX_ENTRY_UNCOMPRESSED_SIZE ||
        input->max_total_uncompressed_size > DEFAULT_MAX_TOTAL_UNCOMPRESSED_SIZE ||
        input->max_compression_ratio > DEFAULT_MAX_COMPRESSION_RATIO ||
        input->compression_ratio_slack > DEFAULT_COMPRESSION_RATIO_SLACK)
        return E_INVALIDARG;

    *limits = *input;
    return S_OK;
}

static HRESULT find_directory( HANDLE file, UINT64 file_size, struct directory_info *directory )
{
    BYTE locator[ZIP64_END_DIRECTORY_LOCATOR_SIZE], zip64[ZIP64_END_DIRECTORY_MIN_SIZE];
    UINT64 eocd_offset, zip64_offset, zip64_size, search_offset;
    UINT32 search_size, size32, offset32;
    UINT16 entries16, entries_disk16, comment_length;
    BYTE *tail;
    INT64 i;
    HRESULT hr;
    BOOL need_zip64, has_locator = FALSE;

    if (file_size < ZIP_END_DIRECTORY_SIZE) return APPX_E_INVALID_PACKAGING_LAYOUT;
    search_size = file_size > ZIP_END_DIRECTORY_MAX_SEARCH ? ZIP_END_DIRECTORY_MAX_SEARCH : file_size;
    search_offset = file_size - search_size;

    if (!(tail = HeapAlloc( GetProcessHeap(), 0, search_size )))
        return E_OUTOFMEMORY;
    if (FAILED(hr = read_at( file, search_offset, tail, search_size )))
    {
        HeapFree( GetProcessHeap(), 0, tail );
        return hr;
    }

    for (i = search_size - ZIP_END_DIRECTORY_SIZE; i >= 0; i--)
    {
        if (read_uint32( tail + i ) != ZIP_END_DIRECTORY_SIGNATURE) continue;
        comment_length = read_uint16( tail + i + 20 );
        if ((UINT64)i + ZIP_END_DIRECTORY_SIZE + comment_length == search_size) break;
    }
    if (i < 0)
    {
        HeapFree( GetProcessHeap(), 0, tail );
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    eocd_offset = search_offset + i;
    if (comment_length || read_uint16( tail + i + 4 ) || read_uint16( tail + i + 6 ))
    {
        HeapFree( GetProcessHeap(), 0, tail );
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    entries_disk16 = read_uint16( tail + i + 8 );
    entries16 = read_uint16( tail + i + 10 );
    size32 = read_uint32( tail + i + 12 );
    offset32 = read_uint32( tail + i + 16 );
    if (entries_disk16 != entries16)
    {
        HeapFree( GetProcessHeap(), 0, tail );
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    need_zip64 = entries16 == 0xffff || size32 == 0xffffffff || offset32 == 0xffffffff;
    if (eocd_offset >= ZIP64_END_DIRECTORY_LOCATOR_SIZE)
    {
        if (i >= ZIP64_END_DIRECTORY_LOCATOR_SIZE)
            memcpy( locator, tail + i - ZIP64_END_DIRECTORY_LOCATOR_SIZE, sizeof(locator) );
        else if (FAILED(hr = read_at( file, eocd_offset - ZIP64_END_DIRECTORY_LOCATOR_SIZE,
                                     locator, sizeof(locator) )))
        {
            HeapFree( GetProcessHeap(), 0, tail );
            return hr;
        }
        has_locator = read_uint32( locator ) == ZIP64_END_DIRECTORY_LOCATOR_SIGNATURE;
    }
    HeapFree( GetProcessHeap(), 0, tail );

    /*
     * A ZIP32 central-directory extra may end in bytes that look like a
     * ZIP64 locator.  ZIP64 records are authoritative only when a classic
     * field uses its sentinel; otherwise the ZIP32 directory boundary below
     * decides whether locator-like bytes are ordinary central-directory data.
     */
    if (!need_zip64)
    {
        directory->entries = entries16;
        directory->size = size32;
        directory->offset = offset32;
        directory->boundary = eocd_offset;
        directory->zip64_extensible_offset = 0;
        directory->zip64_extensible_size = 0;
        directory->zip64_version_made = 0;
        directory->zip64_version_needed = 0;
        directory->zip64 = FALSE;
    }
    else
    {
        UINT64 entries_disk64, entries64;

        if (!has_locator || read_uint32( locator + 4 ) || read_uint32( locator + 16 ) != 1)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        zip64_offset = read_uint64( locator + 8 );
        if (zip64_offset >= eocd_offset - ZIP64_END_DIRECTORY_LOCATOR_SIZE)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        if (FAILED(hr = read_at( file, zip64_offset, zip64, sizeof(zip64) ))) return hr;
        if (read_uint32( zip64 ) != ZIP64_END_DIRECTORY_SIGNATURE)
            return APPX_E_INVALID_PACKAGING_LAYOUT;

        zip64_size = read_uint64( zip64 + 4 );
        if (zip64_size < ZIP64_END_DIRECTORY_MIN_SIZE - 12 ||
            !add_uint64( zip64_offset, 12, &search_offset ) ||
            !add_uint64( search_offset, zip64_size, &search_offset ) ||
            search_offset != eocd_offset - ZIP64_END_DIRECTORY_LOCATOR_SIZE)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        if (read_uint16( zip64 + 14 ) < 45 || read_uint32( zip64 + 16 ) ||
            read_uint32( zip64 + 20 ))
            return APPX_E_INVALID_PACKAGING_LAYOUT;

        entries_disk64 = read_uint64( zip64 + 24 );
        entries64 = read_uint64( zip64 + 32 );
        if (entries_disk64 != entries64) return APPX_E_INVALID_PACKAGING_LAYOUT;

        directory->entries = entries64;
        directory->size = read_uint64( zip64 + 40 );
        directory->offset = read_uint64( zip64 + 48 );
        directory->boundary = zip64_offset;
        if (!add_uint64( zip64_offset, ZIP64_END_DIRECTORY_MIN_SIZE,
                         &directory->zip64_extensible_offset ))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        directory->zip64_extensible_size = zip64_size -
                                           (ZIP64_END_DIRECTORY_MIN_SIZE - 12);
        directory->zip64_version_made = read_uint16( zip64 + 12 );
        directory->zip64_version_needed = read_uint16( zip64 + 14 );
        directory->zip64 = TRUE;

        if ((entries16 != 0xffff && entries16 != entries64) ||
            (size32 != 0xffffffff && size32 != directory->size) ||
            (offset32 != 0xffffffff && offset32 != directory->offset))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    if (!add_uint64( directory->offset, directory->size, &search_offset ) ||
        search_offset != directory->boundary)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    return S_OK;
}

static HRESULT read_zip64_extra( const BYTE *extra, UINT16 extra_length,
                                 UINT32 compressed32, UINT32 uncompressed32,
                                 UINT32 offset32, UINT16 disk16, UINT64 *compressed,
                                 UINT64 *uncompressed, UINT64 *offset, UINT32 *disk,
                                 BOOL *used_sizes )
{
    BOOL found = FALSE;
    UINT32 cursor = 0;

    *compressed = compressed32;
    *uncompressed = uncompressed32;
    *offset = offset32;
    *disk = disk16;
    *used_sizes = FALSE;

    while (cursor < extra_length)
    {
        UINT16 id, size;
        const BYTE *data;
        UINT32 end;

        if (extra_length - cursor < 4) return APPX_E_INVALID_PACKAGING_LAYOUT;
        id = read_uint16( extra + cursor );
        size = read_uint16( extra + cursor + 2 );
        cursor += 4;
        end = cursor + size;
        if (end > extra_length) return APPX_E_INVALID_PACKAGING_LAYOUT;
        data = extra + cursor;

        if (id == ZIP_EXTRA_ZIP64)
        {
            UINT32 position = 0;

            if (found || (compressed32 != 0xffffffff && uncompressed32 != 0xffffffff &&
                          offset32 != 0xffffffff && disk16 != 0xffff))
                return APPX_E_INVALID_PACKAGING_LAYOUT;
            found = TRUE;
            if (uncompressed32 == 0xffffffff)
            {
                if (size - position < 8) return APPX_E_INVALID_PACKAGING_LAYOUT;
                *uncompressed = read_uint64( data + position );
                position += 8;
                *used_sizes = TRUE;
            }
            if (compressed32 == 0xffffffff)
            {
                if (size - position < 8) return APPX_E_INVALID_PACKAGING_LAYOUT;
                *compressed = read_uint64( data + position );
                position += 8;
                *used_sizes = TRUE;
            }
            if (offset32 == 0xffffffff)
            {
                if (size - position < 8) return APPX_E_INVALID_PACKAGING_LAYOUT;
                *offset = read_uint64( data + position );
                position += 8;
            }
            if (disk16 == 0xffff)
            {
                if (size - position < 4) return APPX_E_INVALID_PACKAGING_LAYOUT;
                *disk = read_uint32( data + position );
                position += 4;
            }
            if (position != size) return APPX_E_INVALID_PACKAGING_LAYOUT;
        }
        /* No downstream extractor may reinterpret an alternate metadata field. */
        else
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        cursor = end;
    }

    if ((compressed32 == 0xffffffff || uncompressed32 == 0xffffffff ||
         offset32 == 0xffffffff || disk16 == 0xffff) && !found)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    return S_OK;
}

static BOOL contains_non_ascii( const BYTE *name, UINT32 length )
{
    UINT32 i;

    for (i = 0; i < length; i++) if (name[i] & 0x80) return TRUE;
    return FALSE;
}

static BOOL valid_unix_attributes( UINT16 version_made, UINT32 attributes, BOOL directory )
{
    UINT32 type;

    if ((version_made >> 8) != 3) return TRUE;
    type = (attributes >> 16) & 0170000;
    if (!type) return TRUE;
    if (directory) return type == 0040000;
    return type == 0100000;
}

static HRESULT parse_central_entry( const BYTE *buffer, UINT64 available,
                                    const WINE_APPX_ARCHIVE_LIMITS *limits,
                                    struct archive_entry *entry, UINT64 *consumed,
                                    UINT64 *total_uncompressed )
{
    UINT32 compressed32, uncompressed32, offset32, disk;
    UINT16 version_made, name_length, extra_length, comment_length, disk16;
    UINT64 record_length, ratio_limit;
    const BYTE *name, *extra;
    UINT32 path_flags = 0;
    HRESULT hr;

    if (available < ZIP_CENTRAL_DIRECTORY_HEADER_SIZE ||
        read_uint32( buffer ) != ZIP_CENTRAL_DIRECTORY_SIGNATURE)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    version_made = read_uint16( buffer + 4 );
    entry->version_needed = read_uint16( buffer + 6 );
    entry->zip_flags = read_uint16( buffer + 8 );
    entry->compression_method = read_uint16( buffer + 10 );
    entry->modified_time = read_uint16( buffer + 12 );
    entry->modified_date = read_uint16( buffer + 14 );
    entry->crc32 = read_uint32( buffer + 16 );
    compressed32 = read_uint32( buffer + 20 );
    uncompressed32 = read_uint32( buffer + 24 );
    name_length = read_uint16( buffer + 28 );
    extra_length = read_uint16( buffer + 30 );
    comment_length = read_uint16( buffer + 32 );
    disk16 = read_uint16( buffer + 34 );
    offset32 = read_uint32( buffer + 42 );

    record_length = ZIP_CENTRAL_DIRECTORY_HEADER_SIZE;
    if (!add_uint64( record_length, name_length, &record_length ) ||
        !add_uint64( record_length, extra_length, &record_length ) ||
        !add_uint64( record_length, comment_length, &record_length ) ||
        record_length > available || !name_length || comment_length)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    if ((entry->zip_flags & ~ZIP_ALLOWED_FLAGS) ||
        (entry->zip_flags & (ZIP_FLAG_ENCRYPTED | ZIP_FLAG_PATCHED_DATA |
                             ZIP_FLAG_STRONG_ENCRYPTION | ZIP_FLAG_MASKED_HEADER)) ||
        (entry->compression_method != ZIP_METHOD_STORE &&
         entry->compression_method != ZIP_METHOD_DEFLATE) ||
        (entry->compression_method == ZIP_METHOD_STORE &&
         (entry->zip_flags & ZIP_FLAG_COMPRESSION_OPTIONS)))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (entry->version_needed > 63 ||
        (entry->compression_method == ZIP_METHOD_STORE && entry->version_needed < 10) ||
        (entry->compression_method == ZIP_METHOD_DEFLATE && entry->version_needed < 20))
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    name = buffer + ZIP_CENTRAL_DIRECTORY_HEADER_SIZE;
    extra = name + name_length;
    if (contains_non_ascii( name, name_length ) && !(entry->zip_flags & ZIP_FLAG_UTF8))
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    if (FAILED(hr = read_zip64_extra( extra, extra_length, compressed32, uncompressed32,
                                     offset32, disk16, &entry->compressed_size,
                                     &entry->uncompressed_size, &entry->local_header_offset,
                                     &disk, &entry->zip64_sizes )))
        return hr;
    if (disk) return APPX_E_INVALID_PACKAGING_LAYOUT;
    if ((compressed32 == 0xffffffff || uncompressed32 == 0xffffffff ||
         offset32 == 0xffffffff || disk16 == 0xffff) && entry->version_needed < 45)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    if (entry->compressed_size > limits->max_entry_compressed_size ||
        entry->uncompressed_size > limits->max_entry_uncompressed_size ||
        !add_uint64( *total_uncompressed, entry->uncompressed_size, total_uncompressed ) ||
        *total_uncompressed > limits->max_total_uncompressed_size)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (entry->compression_method == ZIP_METHOD_STORE &&
        entry->compressed_size != entry->uncompressed_size)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!entry->compressed_size && entry->uncompressed_size)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (entry->compression_method == ZIP_METHOD_DEFLATE &&
        !multiply_uint64( entry->compressed_size, limits->max_compression_ratio, &ratio_limit ))
        ratio_limit = ~(UINT64)0;
    else if (entry->compression_method != ZIP_METHOD_DEFLATE)
        ratio_limit = ~(UINT64)0;
    if (ratio_limit != ~(UINT64)0)
    {
        if (!add_uint64( ratio_limit, limits->compression_ratio_slack, &ratio_limit ))
            ratio_limit = ~(UINT64)0;
        if (entry->uncompressed_size > ratio_limit) return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    if (name[name_length - 1] == '/')
    {
        path_flags = WINE_APPX_PATH_DIRECTORY;
        entry->flags |= WINE_APPX_ENTRY_DIRECTORY;
        if (entry->compressed_size || entry->uncompressed_size)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    }
    if (entry->zip_flags & ZIP_FLAG_DATA_DESCRIPTOR)
    {
        if (entry->flags & WINE_APPX_ENTRY_DIRECTORY)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        entry->flags |= WINE_APPX_ENTRY_DATA_DESCRIPTOR;
    }
    if (!valid_unix_attributes( version_made, read_uint32( buffer + 38 ),
                                !!(entry->flags & WINE_APPX_ENTRY_DIRECTORY) ))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if ((read_uint32( buffer + 38 ) & 0x10) &&
        !(entry->flags & WINE_APPX_ENTRY_DIRECTORY))
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    entry->path_length = 0;
    hr = wine_appx_validate_archive_path( name, name_length, path_flags,
                                          &entry->path_length, NULL );
    if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) return hr;
    if (!(entry->path = HeapAlloc( GetProcessHeap(), 0,
                                   entry->path_length * sizeof(*entry->path) )))
        return E_OUTOFMEMORY;
    if (!(entry->name = HeapAlloc( GetProcessHeap(), 0, name_length )))
        return E_OUTOFMEMORY;
    memcpy( entry->name, name, name_length );
    entry->name_length = name_length;
    hr = wine_appx_validate_archive_path( name, name_length, path_flags,
                                          &entry->path_length, entry->path );
    if (FAILED(hr)) return hr;

    *consumed = record_length;
    return S_OK;
}

static INT compare_path( const void *left, const void *right )
{
    const struct archive_entry *left_entry = left;
    const struct archive_entry *right_entry = right;
    INT result;

    result = CompareStringOrdinal( left_entry->path, left_entry->path_length - 1,
                                   right_entry->path, right_entry->path_length - 1, TRUE );
    return result == CSTR_LESS_THAN ? -1 : result == CSTR_GREATER_THAN ? 1 : 0;
}

static INT compare_offset( const void *left, const void *right )
{
    const struct archive_entry *const *left_entry = left;
    const struct archive_entry *const *right_entry = right;

    if ((*left_entry)->local_header_offset < (*right_entry)->local_header_offset) return -1;
    if ((*left_entry)->local_header_offset > (*right_entry)->local_header_offset) return 1;
    return 0;
}

static INT compare_path_segment( const WCHAR *path, UINT32 length, const struct archive_entry *entry )
{
    INT result = CompareStringOrdinal( path, length, entry->path, entry->path_length - 1, TRUE );

    return result == CSTR_LESS_THAN ? -1 : result == CSTR_GREATER_THAN ? 1 : 0;
}

static struct archive_entry *find_path( struct archive_entry *entries, UINT32 count,
                                       const WCHAR *path, UINT32 length )
{
    UINT32 low = 0, high = count;

    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;
        INT result = compare_path_segment( path, length, entries + middle );

        if (!result) return entries + middle;
        if (result < 0) high = middle;
        else low = middle + 1;
    }
    return NULL;
}

static HRESULT find_file_entry( WINE_APPX_ARCHIVE *archive, const WCHAR *path,
                                struct archive_entry **entry, UINT32 *index )
{
    UINT32 length = lstrlenW( path );

    if (!(*entry = find_path( archive->entries, archive->count, path, length )))
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    if ((*entry)->flags & WINE_APPX_ENTRY_DIRECTORY)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (index) *index = *entry - archive->entries;
    return S_OK;
}

static HRESULT validate_paths( struct archive_entry *entries, UINT32 count, UINT32 flags )
{
    static const WCHAR *package_files[] =
    {
        L"AppxManifest.xml",
        L"AppxBlockMap.xml",
        L"[Content_Types].xml",
        L"AppxSignature.p7x",
    };
    static const WCHAR *bundle_files[] =
    {
        L"AppxMetadata\\AppxBundleManifest.xml",
        L"AppxBlockMap.xml",
        L"[Content_Types].xml",
        L"AppxSignature.p7x",
    };
    const WCHAR **required = flags & WINE_APPX_ARCHIVE_OPEN_BUNDLE ? bundle_files : package_files;
    UINT32 i, j;

    qsort( entries, count, sizeof(*entries), compare_path );
    for (i = 1; i < count; i++)
    {
        if (!compare_path( entries + i - 1, entries + i ))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    for (i = 0; i < count; i++)
    {
        for (j = 0; j + 1 < entries[i].path_length; j++)
        {
            struct archive_entry *ancestor;

            if (entries[i].path[j] != '\\') continue;
            ancestor = find_path( entries, count, entries[i].path, j );
            if (ancestor && !(ancestor->flags & WINE_APPX_ENTRY_DIRECTORY))
                return APPX_E_INVALID_PACKAGING_LAYOUT;
        }
    }

    if (flags & (WINE_APPX_ARCHIVE_OPEN_PACKAGE | WINE_APPX_ARCHIVE_OPEN_BUNDLE))
    {
        for (i = 0; i < ARRAY_SIZE(package_files); i++)
        {
            UINT32 length = lstrlenW( required[i] );
            struct archive_entry *entry = find_path( entries, count, required[i], length );

            if (!entry || (entry->flags & WINE_APPX_ENTRY_DIRECTORY))
                return APPX_E_INVALID_PACKAGING_LAYOUT;
        }
    }
    return S_OK;
}

static BOOL data_descriptor_matches( const BYTE *descriptor, UINT32 cursor,
                                     const struct archive_entry *entry, BOOL zip64 )
{
    if (read_uint32( descriptor + cursor ) != entry->crc32) return FALSE;
    cursor += 4;
    if (zip64)
    {
        if (read_uint64( descriptor + cursor ) != entry->compressed_size ||
            read_uint64( descriptor + cursor + 8 ) != entry->uncompressed_size)
            return FALSE;
    }
    else
    {
        if (read_uint32( descriptor + cursor ) != entry->compressed_size ||
            read_uint32( descriptor + cursor + 4 ) != entry->uncompressed_size)
            return FALSE;
    }
    return TRUE;
}

static HRESULT validate_data_descriptor( HANDLE file, struct archive_entry *entry,
                                         UINT64 data_end, UINT64 record_limit,
                                         UINT64 *record_end )
{
    BYTE descriptor[24];
    UINT64 remaining;
    UINT32 descriptor_size;
    UINT32 cursor;
    BOOL zip64;
    HRESULT hr;

    if (data_end > record_limit)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    remaining = record_limit - data_end;
    switch (remaining)
    {
    case 12:
        descriptor_size = 12;
        cursor = 0;
        zip64 = FALSE;
        break;
    case 16:
        descriptor_size = 16;
        cursor = 4;
        zip64 = FALSE;
        break;
    case 20:
        descriptor_size = 20;
        cursor = 0;
        zip64 = TRUE;
        break;
    case 24:
        descriptor_size = 24;
        cursor = 4;
        zip64 = TRUE;
        break;
    default:
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    }

    if (entry->zip64_sizes && !zip64)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    /* ZIP64 descriptors may be used even when the central sizes fit in 32 bits.
     * Version 4.5 is still required so consumers know to read eight-byte sizes. */
    if (zip64 && entry->version_needed < 45)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (FAILED(hr = read_at( file, data_end, descriptor, descriptor_size ))) return hr;
    if (cursor && read_uint32( descriptor ) != ZIP_DATA_DESCRIPTOR_SIGNATURE)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!data_descriptor_matches( descriptor, cursor, entry, zip64 ))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    *record_end = record_limit;
    return S_OK;
}

static HRESULT validate_local_entry( HANDLE file, UINT64 directory_offset,
                                     UINT64 record_limit, struct archive_entry *entry )
{
    BYTE header[ZIP_LOCAL_FILE_HEADER_SIZE], *variable = NULL;
    UINT32 crc32, compressed32, uncompressed32, variable_length;
    UINT16 name_length, extra_length, flags, method;
    UINT64 compressed, uncompressed, ignored_offset, data_end;
    UINT32 ignored_disk;
    BOOL used_sizes;
    HRESULT hr;

    if (record_limit > directory_offset ||
        entry->local_header_offset >= record_limit ||
        record_limit - entry->local_header_offset < sizeof(header))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (FAILED(hr = read_at( file, entry->local_header_offset, header, sizeof(header) ))) return hr;
    if (read_uint32( header ) != ZIP_LOCAL_FILE_HEADER_SIGNATURE)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    flags = read_uint16( header + 6 );
    method = read_uint16( header + 8 );
    crc32 = read_uint32( header + 14 );
    compressed32 = read_uint32( header + 18 );
    uncompressed32 = read_uint32( header + 22 );
    name_length = read_uint16( header + 26 );
    extra_length = read_uint16( header + 28 );
    variable_length = name_length + extra_length;

    if (read_uint16( header + 4 ) != entry->version_needed ||
        flags != entry->zip_flags || method != entry->compression_method ||
        read_uint16( header + 10 ) != entry->modified_time ||
        read_uint16( header + 12 ) != entry->modified_date ||
        name_length != entry->name_length)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!add_uint64( entry->local_header_offset, sizeof(header), &entry->data_offset ) ||
        !add_uint64( entry->data_offset, variable_length, &entry->data_offset ) ||
        entry->data_offset > record_limit)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    if (!(variable = HeapAlloc( GetProcessHeap(), 0, variable_length ? variable_length : 1 )))
        return E_OUTOFMEMORY;
    if (FAILED(hr = read_at( file, entry->local_header_offset + sizeof(header),
                            variable, variable_length )))
        goto done;
    if (memcmp( variable, entry->name, name_length ))
    {
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
        goto done;
    }

    if (FAILED(hr = read_zip64_extra( variable + name_length, extra_length,
                                     compressed32, uncompressed32, 0, 0,
                                     &compressed, &uncompressed, &ignored_offset,
                                     &ignored_disk, &used_sizes )))
        goto done;
    if (flags & ZIP_FLAG_DATA_DESCRIPTOR)
    {
        if (crc32 || (compressed32 && compressed32 != 0xffffffff) ||
            (uncompressed32 && uncompressed32 != 0xffffffff) ||
            (!entry->zip64_sizes &&
             (compressed32 == 0xffffffff || uncompressed32 == 0xffffffff)) ||
            (compressed32 == 0xffffffff && compressed != entry->compressed_size) ||
            (uncompressed32 == 0xffffffff && uncompressed != entry->uncompressed_size))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
    }
    else
    {
        if (crc32 != entry->crc32 || compressed != entry->compressed_size ||
            uncompressed != entry->uncompressed_size)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
    }

    if (!add_uint64( entry->data_offset, entry->compressed_size, &data_end ) ||
        data_end > record_limit)
    {
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
        goto done;
    }
    if (flags & ZIP_FLAG_DATA_DESCRIPTOR)
        hr = validate_data_descriptor( file, entry, data_end, record_limit,
                                       &entry->record_end );
    else
    {
        entry->record_end = data_end;
        hr = S_OK;
    }

done:
    HeapFree( GetProcessHeap(), 0, variable );
    return hr;
}

static HRESULT validate_local_layout( HANDLE file, UINT64 directory_offset,
                                      struct archive_entry *entries, UINT32 count )
{
    struct archive_entry **ordered;
    HRESULT hr = S_OK;
    UINT32 i;

    if (!(ordered = HeapAlloc( GetProcessHeap(), 0, count * sizeof(*ordered) )))
        return E_OUTOFMEMORY;
    for (i = 0; i < count; i++) ordered[i] = entries + i;

    qsort( ordered, count, sizeof(*ordered), compare_offset );
    if (ordered[0]->local_header_offset)
    {
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
        goto done;
    }
    for (i = 0; i < count; i++)
    {
        UINT64 record_limit = i + 1 < count ? ordered[i + 1]->local_header_offset :
                                             directory_offset;

        if (record_limit <= ordered[i]->local_header_offset ||
            FAILED(hr = validate_local_entry( file, directory_offset, record_limit,
                                              ordered[i] )) ||
            ordered[i]->record_end != record_limit)
        {
            if (SUCCEEDED(hr)) hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
    }

done:
    HeapFree( GetProcessHeap(), 0, ordered );
    return hr;
}

static void free_entries( struct archive_entry *entries, UINT32 count )
{
    UINT32 i;

    if (!entries) return;
    for (i = 0; i < count; i++)
    {
        HeapFree( GetProcessHeap(), 0, entries[i].path );
        HeapFree( GetProcessHeap(), 0, entries[i].name );
    }
    HeapFree( GetProcessHeap(), 0, entries );
}

HRESULT WINAPI wine_appx_archive_open( HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *input_limits,
                                      UINT32 flags, WINE_APPX_ARCHIVE **archive )
{
    WINE_APPX_ARCHIVE_LIMITS limits;
    struct directory_info directory;
    LARGE_INTEGER size, current_size;
    OVERLAPPED lock = {0};
    WINE_APPX_ARCHIVE *object = NULL;
    struct archive_entry *entries = NULL;
    BYTE *central = NULL;
    UINT64 cursor = 0, total_uncompressed = 0, consumed;
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    HRESULT hr;
    UINT32 i;

    TRACE( "file %p, limits %p, flags %#x, archive %p.\n", file, input_limits, flags, archive );

    if (!archive || !file || file == INVALID_HANDLE_VALUE ||
        (flags & ~(WINE_APPX_ARCHIVE_OPEN_PACKAGE | WINE_APPX_ARCHIVE_OPEN_BUNDLE)) ||
        (flags & WINE_APPX_ARCHIVE_OPEN_PACKAGE && flags & WINE_APPX_ARCHIVE_OPEN_BUNDLE))
        return E_INVALIDARG;
    *archive = NULL;
    if (FAILED(hr = validate_limits( input_limits, &limits ))) return hr;
    if (GetFileType( file ) != FILE_TYPE_DISK) return E_INVALIDARG;
    /*
     * DuplicateHandle() shares the underlying file position.  Reopen an
     * overlapped read-only file object so every archive read is positional
     * and neither changes nor races the caller's file pointer.
     */
    duplicate = ReOpenFile( file, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            FILE_FLAG_OVERLAPPED );
    if (duplicate == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!(lock.hEvent = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (!LockFileEx( duplicate, LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &lock ))
    {
        DWORD error = GetLastError(), transferred;

        if (error != ERROR_IO_PENDING ||
            !GetOverlappedResult( duplicate, &lock, &transferred, TRUE ))
        {
            hr = HRESULT_FROM_WIN32( error == ERROR_IO_PENDING ? GetLastError() : error );
            goto done;
        }
    }
    CloseHandle( lock.hEvent );
    lock.hEvent = NULL;
    if (!GetFileSizeEx( duplicate, &size ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (size.QuadPart < 0 || (UINT64)size.QuadPart > limits.max_archive_size)
    {
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
        goto done;
    }
    if (FAILED(hr = find_directory( duplicate, size.QuadPart, &directory ))) goto done;
    if (!directory.entries || directory.entries > limits.max_entries ||
        directory.entries > MAXDWORD ||
        directory.size > limits.max_central_directory_size ||
        directory.size > ~(SIZE_T)0)
    {
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
        goto done;
    }
    if (directory.entries > ~(SIZE_T)0 / sizeof(*entries))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    if (!(central = HeapAlloc( GetProcessHeap(), 0, (SIZE_T)directory.size )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (FAILED(hr = read_at( duplicate, directory.offset, central, (SIZE_T)directory.size )))
        goto done;
    if (!(entries = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                               (SIZE_T)directory.entries * sizeof(*entries) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    for (i = 0; i < directory.entries; i++)
    {
        consumed = 0;
        if (FAILED(hr = parse_central_entry( central + cursor, directory.size - cursor, &limits,
                                             entries + i, &consumed, &total_uncompressed )))
            goto done;
        entries[i].central_header_offset = directory.offset + cursor;
        entries[i].central_header_size = consumed;
        cursor += consumed;
    }
    if (cursor != directory.size)
    {
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
        goto done;
    }
    if (FAILED(hr = validate_paths( entries, directory.entries, flags ))) goto done;
    if (FAILED(hr = validate_local_layout( duplicate, directory.offset, entries,
                                           directory.entries )))
        goto done;
    if (!GetFileSizeEx( duplicate, &current_size ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (current_size.QuadPart != size.QuadPart)
    {
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
        goto done;
    }
    if (!(object = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    object->file = duplicate;
    object->file_size = size.QuadPart;
    object->central_directory_offset = directory.offset;
    object->central_directory_size = directory.size;
    object->zip64_extensible_offset = directory.zip64_extensible_offset;
    object->zip64_extensible_size = directory.zip64_extensible_size;
    object->count = directory.entries;
    object->zip64_version_made = directory.zip64_version_made;
    object->zip64_version_needed = directory.zip64_version_needed;
    object->zip64_directory = directory.zip64;
    object->entries = entries;
    entries = NULL;
    duplicate = INVALID_HANDLE_VALUE;
    *archive = object;
    hr = S_OK;

done:
    if (lock.hEvent) CloseHandle( lock.hEvent );
    if (duplicate != INVALID_HANDLE_VALUE) CloseHandle( duplicate );
    free_entries( entries, entries ? directory.entries : 0 );
    HeapFree( GetProcessHeap(), 0, central );
    return hr;
}

void WINAPI wine_appx_archive_close( WINE_APPX_ARCHIVE *archive )
{
    if (!archive) return;
    CloseHandle( archive->file );
    free_entries( archive->entries, archive->count );
    HeapFree( GetProcessHeap(), 0, archive );
}

HRESULT WINAPI wine_appx_archive_get_count( WINE_APPX_ARCHIVE *archive, UINT32 *count )
{
    if (!archive || !count) return E_INVALIDARG;
    *count = archive->count;
    return S_OK;
}

HRESULT WINAPI wine_appx_archive_find_entry( WINE_APPX_ARCHIVE *archive, const WCHAR *path,
                                             UINT32 *index )
{
    struct archive_entry *entry;
    UINT32 length = 0;

    if (!archive || !path || !index) return E_INVALIDARG;
    *index = 0;
    while (length < WINE_APPX_MAX_PATH_CHARS && path[length])
    {
        if (path[length] == '/') return E_INVALIDARG;
        length++;
    }
    if (!length || length == WINE_APPX_MAX_PATH_CHARS) return E_INVALIDARG;

    if (!(entry = find_path( archive->entries, archive->count, path, length )))
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    *index = entry - archive->entries;
    return S_OK;
}

static void copy_entry_info( const struct archive_entry *source,
                             WINE_APPX_ARCHIVE_ENTRY *entry )
{
    memset( entry, 0, sizeof(*entry) );
    entry->size = sizeof(*entry);
    entry->flags = source->flags;
    entry->crc32 = source->crc32;
    entry->compression_method = source->compression_method;
    entry->compressed_size = source->compressed_size;
    entry->uncompressed_size = source->uncompressed_size;
    entry->local_header_offset = source->local_header_offset;
    entry->data_offset = source->data_offset;
}

HRESULT appx_archive_acquire_entry_source( WINE_APPX_ARCHIVE *archive, UINT32 index,
                                           WINE_APPX_ARCHIVE_ENTRY *entry, HANDLE *file )
{
    if (!archive || !entry || !file) return E_INVALIDARG;
    *file = INVALID_HANDLE_VALUE;
    if (index >= archive->count) return E_BOUNDS;

    copy_entry_info( archive->entries + index, entry );
    /*
     * Use a distinct file object rather than DuplicateHandle().  Besides
     * keeping the file position independent, this scopes CancelIoEx() from an
     * entry stream to that stream's I/O instead of another stream or archive
     * read using the same underlying handle.
     */
    *file = ReOpenFile( archive->file, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_FLAG_OVERLAPPED );
    if (*file == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

HRESULT WINAPI wine_appx_archive_get_entry( WINE_APPX_ARCHIVE *archive, UINT32 index,
                                            WINE_APPX_ARCHIVE_ENTRY *entry,
                                            UINT32 *path_length, WCHAR *path )
{
    const struct archive_entry *source;
    UINT32 capacity;

    if (!archive || !entry || entry->size != sizeof(*entry) || !path_length)
        return E_INVALIDARG;
    if (index >= archive->count) return E_BOUNDS;

    source = archive->entries + index;
    capacity = *path_length;
    *path_length = source->path_length;
    copy_entry_info( source, entry );

    if (!path || capacity < source->path_length)
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( path, source->path, source->path_length * sizeof(*path) );
    return S_OK;
}

static void write_appx_zip32_presignature_footer( BYTE *buffer, UINT16 entries,
                                                  UINT32 directory_size,
                                                  UINT32 directory_offset )
{
    write_uint32( buffer, ZIP_END_DIRECTORY_SIGNATURE );
    write_uint16( buffer + 4, 0 );
    write_uint16( buffer + 6, 0 );
    write_uint16( buffer + 8, entries );
    write_uint16( buffer + 10, entries );
    write_uint32( buffer + 12, directory_size );
    write_uint32( buffer + 16, directory_offset );
    write_uint16( buffer + 20, 0 );
}

static void write_appx_zip64_presignature_header( BYTE *buffer, UINT64 record_size,
                                                  UINT16 version_made,
                                                  UINT16 version_needed,
                                                  UINT64 entries,
                                                  UINT64 directory_size,
                                                  UINT64 directory_offset )
{
    write_uint32( buffer, ZIP64_END_DIRECTORY_SIGNATURE );
    write_uint64( buffer + 4, record_size );
    write_uint16( buffer + 12, version_made );
    write_uint16( buffer + 14, version_needed );
    write_uint32( buffer + 16, 0 );
    write_uint32( buffer + 20, 0 );
    write_uint64( buffer + 24, entries );
    write_uint64( buffer + 32, entries );
    write_uint64( buffer + 40, directory_size );
    write_uint64( buffer + 48, directory_offset );
}

static void write_appx_zip64_presignature_locator( BYTE *buffer, UINT64 zip64_offset )
{
    write_uint32( buffer, ZIP64_END_DIRECTORY_LOCATOR_SIGNATURE );
    write_uint32( buffer + 4, 0 );
    write_uint64( buffer + 8, zip64_offset );
    write_uint32( buffer + 16, 1 );
}

static void write_appx_zip64_presignature_eocd( BYTE *buffer )
{
    write_uint32( buffer, ZIP_END_DIRECTORY_SIGNATURE );
    write_uint16( buffer + 4, 0 );
    write_uint16( buffer + 6, 0 );
    write_uint16( buffer + 8, 0xffff );
    write_uint16( buffer + 10, 0xffff );
    write_uint32( buffer + 12, 0xffffffff );
    write_uint32( buffer + 16, 0xffffffff );
    write_uint16( buffer + 20, 0 );
}

static HRESULT calculate_presignature_central_digest(
    WINE_APPX_ARCHIVE *archive, const struct archive_entry *signature,
    BYTE digest[APPX_SIGNATURE_SHA256_SIZE] )
{
    BYTE zip64_header[ZIP64_END_DIRECTORY_MIN_SIZE];
    BYTE locator[ZIP64_END_DIRECTORY_LOCATOR_SIZE];
    BYTE eocd[ZIP_END_DIRECTORY_SIZE];
    struct sha256_context hash;
    UINT64 directory_end, signature_end, directory_size, zip64_offset;
    UINT64 zip64_record_size;
    UINT32 entries;
    HRESULT hr;

    if (archive->count < 2 ||
        signature->record_end != archive->central_directory_offset ||
        !add_uint64( archive->central_directory_offset,
                     archive->central_directory_size, &directory_end ) ||
        !add_uint64( signature->central_header_offset,
                     signature->central_header_size, &signature_end ) ||
        signature_end != directory_end ||
        signature->central_header_offset < archive->central_directory_offset)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    entries = archive->count - 1;
    directory_size = signature->central_header_offset -
                     archive->central_directory_offset;
    if (!add_uint64( signature->local_header_offset, directory_size, &zip64_offset ))
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    if (FAILED(hr = sha256_init( &hash ))) return hr;
    if (FAILED(hr = hash_file_range( archive->file, archive->central_directory_offset,
                                     directory_size, &hash )))
        goto done;

    if (!archive->zip64_directory)
    {
        if (entries > 0xffff || directory_size > MAXDWORD ||
            signature->local_header_offset > MAXDWORD)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
        write_appx_zip32_presignature_footer( eocd, (UINT16)entries,
                                              (UINT32)directory_size,
                                              (UINT32)signature->local_header_offset );
        if (SUCCEEDED(hr = sha256_update( &hash, eocd, sizeof(eocd) )))
            hr = sha256_finish( &hash, digest );
    }
    else
    {
        if (!add_uint64( ZIP64_END_DIRECTORY_MIN_SIZE - 12,
                         archive->zip64_extensible_size, &zip64_record_size ))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
        write_appx_zip64_presignature_header(
            zip64_header, zip64_record_size, archive->zip64_version_made,
            archive->zip64_version_needed, entries, directory_size,
            signature->local_header_offset );
        write_appx_zip64_presignature_locator( locator, zip64_offset );
        write_appx_zip64_presignature_eocd( eocd );

        if (FAILED(hr = sha256_update( &hash, zip64_header, sizeof(zip64_header) )) ||
            FAILED(hr = hash_file_range( archive->file,
                                         archive->zip64_extensible_offset,
                                         archive->zip64_extensible_size, &hash )) ||
            FAILED(hr = sha256_update( &hash, locator, sizeof(locator) )) ||
            FAILED(hr = sha256_update( &hash, eocd, sizeof(eocd) )))
            goto done;
        if (SUCCEEDED(hr))
            hr = sha256_finish( &hash, digest );
    }

done:
    sha256_destroy( &hash );
    return hr;
}

static HRESULT calculate_raw_range_digest( WINE_APPX_ARCHIVE *archive, UINT64 offset,
                                           UINT64 size,
                                           BYTE digest[APPX_SIGNATURE_SHA256_SIZE] )
{
    struct sha256_context hash;
    HRESULT hr;

    if (FAILED(hr = sha256_init( &hash ))) return hr;
    if (SUCCEEDED(hr = hash_file_range( archive->file, offset, size, &hash )))
        hr = sha256_finish( &hash, digest );
    sha256_destroy( &hash );
    return hr;
}

static HRESULT calculate_entry_digest( WINE_APPX_ARCHIVE *archive, UINT32 index,
                                       BYTE digest[APPX_SIGNATURE_SHA256_SIZE] )
{
    BYTE buffer[HASH_IO_BUFFER_SIZE];
    struct sha256_context hash;
    WINE_APPX_ARCHIVE_STREAM *stream = NULL;
    HRESULT hr;

    if (FAILED(hr = sha256_init( &hash ))) return hr;
    if (FAILED(hr = wine_appx_archive_stream_open( archive, index, &stream )))
        goto done;

    for (;;)
    {
        UINT32 read = 0;

        hr = wine_appx_archive_stream_read( stream, buffer, sizeof(buffer), &read );
        if (hr == S_FALSE)
        {
            hr = sha256_finish( &hash, digest );
            break;
        }
        if (FAILED(hr)) break;
        if (!read)
        {
            hr = APPX_E_CORRUPT_CONTENT;
            break;
        }
        if (FAILED(hr = sha256_update( &hash, buffer, read ))) break;
    }

done:
    if (stream) wine_appx_archive_stream_close( stream );
    sha256_destroy( &hash );
    return hr;
}

HRESULT WINAPI appx_archive_calculate_digest_set(
    WINE_APPX_ARCHIVE *archive, struct appx_signature_digest_set *set )
{
    static const WCHAR signature_path[] =
        {'A','p','p','x','S','i','g','n','a','t','u','r','e','.','p','7','x',0};
    static const WCHAR content_types_path[] =
        {'[','C','o','n','t','e','n','t','_','T','y','p','e','s',']','.','x','m','l',0};
    static const WCHAR block_map_path[] =
        {'A','p','p','x','B','l','o','c','k','M','a','p','.','x','m','l',0};
    static const WCHAR code_integrity_path[] =
        {'A','p','p','x','M','e','t','a','d','a','t','a','\\',
         'C','o','d','e','I','n','t','e','g','r','i','t','y','.','c','a','t',0};
    struct archive_entry *signature, *entry;
    UINT32 index;
    HRESULT hr;

    if (!set) return E_INVALIDARG;
    memset( set, 0, sizeof(*set) );
    if (!archive) return E_INVALIDARG;

    if (FAILED(hr = find_file_entry( archive, signature_path, &signature, NULL )))
        goto failed;
    /* Validate the signature position before hashing a potentially huge AXPC range. */
    if (FAILED(hr = calculate_presignature_central_digest(
            archive, signature, set->central_directory )))
        goto failed;
    set->flags |= APPX_SIGNATURE_DIGEST_CENTRAL_DIRECTORY;

    if (FAILED(hr = calculate_raw_range_digest( archive, 0,
                                                signature->local_header_offset,
                                                set->package_contents )))
        goto failed;
    set->flags |= APPX_SIGNATURE_DIGEST_PACKAGE_CONTENTS;

    if (FAILED(hr = find_file_entry( archive, content_types_path, &entry, &index )) ||
        FAILED(hr = calculate_entry_digest( archive, index, set->content_types )))
        goto failed;
    set->flags |= APPX_SIGNATURE_DIGEST_CONTENT_TYPES;

    if (FAILED(hr = find_file_entry( archive, block_map_path, &entry, &index )) ||
        FAILED(hr = calculate_entry_digest( archive, index, set->block_map )))
        goto failed;
    set->flags |= APPX_SIGNATURE_DIGEST_BLOCK_MAP;

    hr = find_file_entry( archive, code_integrity_path, &entry, &index );
    if (hr == HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ))
        return S_OK;
    if (FAILED(hr) || FAILED(hr = calculate_entry_digest( archive, index,
                                                          set->code_integrity )))
        goto failed;
    set->flags |= APPX_SIGNATURE_DIGEST_CODE_INTEGRITY;
    return S_OK;

failed:
    memset( set, 0, sizeof(*set) );
    return hr;
}
