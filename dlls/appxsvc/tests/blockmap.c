/*
 * AppX block map parser tests
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
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "wine/test.h"

#include "../blockmap.h"

#define BLOCK_MAP_NS "http://schemas.microsoft.com/appx/2010/blockmap"
#define BLOCK_MAP_NS_2015 "http://schemas.microsoft.com/appx/2015/blockmap"
#define BLOCK_MAP_NS_2017 "http://schemas.microsoft.com/appx/2017/blockmap"
#define BLOCK_MAP_NS_2021 "http://schemas.microsoft.com/appx/2021/blockmap"
#define SHA256_METHOD "http://www.w3.org/2001/04/xmlenc#sha256"
#define SHA384_METHOD "http://www.w3.org/2001/04/xmldsig-more#sha384"
#define SHA512_METHOD "http://www.w3.org/2001/04/xmlenc#sha512"
#define ZERO_HASH "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="

#define ROOT_OPEN \
    "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\"" SHA256_METHOD "\">"
#define ROOT_CLOSE "</BlockMap>"

static HRESULT (WINAPI *p_appx_block_map_parse)( const BYTE *, UINT32, APPX_BLOCK_MAP ** );
static void (WINAPI *p_appx_block_map_free)( APPX_BLOCK_MAP * );
static UINT32 (WINAPI *p_appx_block_map_get_file_count)( const APPX_BLOCK_MAP * );
static UINT32 (WINAPI *p_appx_block_map_get_block_count)( const APPX_BLOCK_MAP * );
static const APPX_BLOCK_MAP_FILE *(WINAPI *p_appx_block_map_get_file)(
    const APPX_BLOCK_MAP *, UINT32 );
static const APPX_BLOCK_MAP_BLOCK *(WINAPI *p_appx_block_map_get_block)(
    const APPX_BLOCK_MAP *, UINT32, UINT32 );
static HRESULT (WINAPI *p_wine_appx_validate_archive_path)(
    const BYTE *, UINT32, UINT32, UINT32 *, WCHAR * );

struct buffer
{
    char *data;
    SIZE_T size;
    SIZE_T capacity;
};

static BOOL reserve( struct buffer *buffer, SIZE_T length )
{
    char *data;
    SIZE_T required, capacity;

    if (~(SIZE_T)0 - buffer->size < length) return FALSE;
    required = buffer->size + length;
    if (required <= buffer->capacity) return TRUE;
    capacity = buffer->capacity ? buffer->capacity : 1024;
    while (capacity < required)
    {
        if (capacity > ~(SIZE_T)0 / 2)
        {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    if (buffer->data)
        data = HeapReAlloc( GetProcessHeap(), 0, buffer->data, capacity );
    else
        data = HeapAlloc( GetProcessHeap(), 0, capacity );
    if (!data) return FALSE;
    buffer->data = data;
    buffer->capacity = capacity;
    return TRUE;
}

static BOOL append_bytes( struct buffer *buffer, const void *data, SIZE_T length )
{
    if (!reserve( buffer, length )) return FALSE;
    memcpy( buffer->data + buffer->size, data, length );
    buffer->size += length;
    return TRUE;
}

static BOOL append_string( struct buffer *buffer, const char *string )
{
    return append_bytes( buffer, string, strlen(string) );
}

static BOOL append_repeat( struct buffer *buffer, char value, SIZE_T count )
{
    if (!reserve( buffer, count )) return FALSE;
    memset( buffer->data + buffer->size, value, count );
    buffer->size += count;
    return TRUE;
}

static void free_buffer( struct buffer *buffer )
{
    if (buffer->data) HeapFree( GetProcessHeap(), 0, buffer->data );
    memset( buffer, 0, sizeof(*buffer) );
}

static HRESULT parse_xml( const void *document, SIZE_T size, APPX_BLOCK_MAP **map )
{
    if (size > MAXDWORD) return E_INVALIDARG;
    return p_appx_block_map_parse( document, (UINT32)size, map );
}

static void check_xml_data_( const void *document, SIZE_T size, HRESULT expected,
                             const char *file, int line )
{
    APPX_BLOCK_MAP *map = (APPX_BLOCK_MAP *)0xdeadbeef;
    HRESULT hr = parse_xml( document, size, &map );

    ok_(file, line)( hr == expected, "got hr %#lx, expected %#lx.\n", hr, expected );
    if (SUCCEEDED(hr))
    {
        ok_(file, line)( map != NULL, "successful parse returned no map.\n" );
        p_appx_block_map_free( map );
    }
    else
    {
        ok_(file, line)( map == NULL, "failed parse returned map %p.\n", map );
    }
}

#define check_xml_data(document, size, expected) \
    check_xml_data_( document, size, expected, __FILE__, __LINE__ )
#define check_xml(document, expected) \
    check_xml_data_( document, strlen(document), expected, __FILE__, __LINE__ )

static void test_normal(void)
{
    static const char document[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        ROOT_OPEN
        "<File Name=\"bin\\app.exe\" Size=\"65537\" LfhSize=\"42\">"
        "<Block Hash=\"" ZERO_HASH "\"/>"
        "<Block Size=\"123\" Hash=\"" ZERO_HASH "\"></Block>"
        "</File>"
        "<File LfhSize=\"39\" Size=\"0\" Name=\"empty.dat\"/>"
        ROOT_CLOSE;
    static const char escaped_name[] =
        ROOT_OPEN
        "<File Name=\"100%25&amp;safe.dat\" Size=\"0\" LfhSize=\"30\"/>"
        ROOT_CLOSE;
    APPX_BLOCK_MAP *map;
    const APPX_BLOCK_MAP_FILE *file;
    const APPX_BLOCK_MAP_BLOCK *block;
    static const char archive_name[] = "100%2525%26safe.dat";
    WCHAR archive_path[64];
    UINT32 archive_path_length;
    HRESULT hr;
    UINT32 i;

    ok( strlen(ZERO_HASH) == 44, "zero hash has length %Iu.\n", strlen(ZERO_HASH) );
    hr = parse_xml( document, sizeof(document) - 1, &map );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;

    ok( p_appx_block_map_get_file_count( map ) == 2, "got %u files.\n",
        p_appx_block_map_get_file_count( map ) );
    ok( p_appx_block_map_get_block_count( map ) == 2, "got %u blocks.\n",
        p_appx_block_map_get_block_count( map ) );
    file = p_appx_block_map_get_file( map, 0 );
    ok( file != NULL, "missing first file.\n" );
    if (file)
    {
        ok( file->name_length == 11, "got name length %u.\n", file->name_length );
        ok( !lstrcmpW( file->name, L"bin\\app.exe" ), "got name %s.\n",
            debugstr_w(file->name) );
        ok( file->size == 65537, "got size %I64u.\n", file->size );
        ok( file->local_file_header_size == 42, "got local header size %u.\n",
            file->local_file_header_size );
        ok( file->block_count == 2, "got block count %u.\n", file->block_count );
        ok( !file->has_file_hash, "unexpected whole-file hash.\n" );
    }

    block = p_appx_block_map_get_block( map, 0, 0 );
    ok( block != NULL, "missing first block.\n" );
    if (block)
    {
        for (i = 0; i < APPX_BLOCK_MAP_HASH_SIZE; i++)
            ok( !block->hash[i], "hash byte %u is %#x.\n", i, block->hash[i] );
        ok( block->logical_size == APPX_BLOCK_MAP_BLOCK_SIZE,
            "got logical size %u.\n", block->logical_size );
        ok( !block->has_compressed_size, "unexpected compressed size.\n" );
    }
    block = p_appx_block_map_get_block( map, 0, 1 );
    ok( block != NULL, "missing second block.\n" );
    if (block)
    {
        ok( block->logical_size == 1, "got logical size %u.\n", block->logical_size );
        ok( block->has_compressed_size, "missing compressed size.\n" );
        ok( block->compressed_size == 123, "got compressed size %u.\n",
            block->compressed_size );
    }

    file = p_appx_block_map_get_file( map, 1 );
    ok( file != NULL, "missing empty file.\n" );
    if (file)
    {
        ok( !lstrcmpW( file->name, L"empty.dat" ), "got name %s.\n", debugstr_w(file->name) );
        ok( !file->size, "got size %I64u.\n", file->size );
        ok( !file->block_count, "got block count %u.\n", file->block_count );
    }
    ok( !p_appx_block_map_get_file( map, 2 ), "found out-of-range file.\n" );
    ok( !p_appx_block_map_get_block( map, 0, 2 ), "found out-of-range block.\n" );
    ok( !p_appx_block_map_get_block( map, 1, 0 ), "found empty-file block.\n" );
    ok( !p_appx_block_map_get_block( map, 2, 0 ), "found invalid-file block.\n" );
    p_appx_block_map_free( map );

    check_xml( "<b:BlockMap xmlns:b=\"" BLOCK_MAP_NS "\" HashMethod=\"" SHA256_METHOD "\">"
               "<b:File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
               "<b:Block Hash=\"" ZERO_HASH "\"/>"
               "</b:File></b:BlockMap>", S_OK );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\"" SHA256_METHOD "\"/>", S_OK );
    check_xml( ROOT_OPEN "<File Name=\"empty\" Size=\"0\" LfhSize=\"30\"></File>" ROOT_CLOSE,
               S_OK );
    check_xml( ROOT_OPEN "<File Name=\"one\" Size=\"1\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\"> \r\n\t</Block></File>" ROOT_CLOSE, S_OK );
    check_xml( ROOT_OPEN "<File Name=\"leading\" Size=\"000\" LfhSize=\"0000000030\"/>"
               ROOT_CLOSE, S_OK );

    hr = parse_xml( escaped_name, sizeof(escaped_name) - 1, &map );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        file = p_appx_block_map_get_file( map, 0 );
        ok( file != NULL, "missing escaped-name file.\n" );
        if (file)
        {
            ok( !lstrcmpW( file->name, L"100%25&safe.dat" ), "got name %s.\n",
                debugstr_w(file->name) );
            archive_path_length = ARRAY_SIZE(archive_path);
            hr = p_wine_appx_validate_archive_path(
                (const BYTE *)archive_name, sizeof(archive_name) - 1, 0,
                &archive_path_length, archive_path );
            ok( hr == S_OK, "archive path validation got hr %#lx.\n", hr );
            ok( hr != S_OK || !lstrcmpW( archive_path, file->name ),
                "archive path %s does not match block-map path %s.\n",
                debugstr_w(archive_path), debugstr_w(file->name) );
        }
        p_appx_block_map_free( map );
    }
}

static void test_arguments_and_empty(void)
{
    static const char empty[] = "";
    APPX_BLOCK_MAP *map = (APPX_BLOCK_MAP *)0xdeadbeef;
    HRESULT hr;

    hr = p_appx_block_map_parse( NULL, 1, &map );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( map == NULL, "map changed to %p.\n", map );
    hr = p_appx_block_map_parse( (const BYTE *)"x", 1, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    check_xml_data( empty, 0, APPX_E_INVALID_BLOCKMAP );
    check_xml( " \r\n\t", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap/>", APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN, APPX_E_INVALID_BLOCKMAP );
    check_xml( "<?xml version=\"1.1\"?>" ROOT_OPEN ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml_data( "x", APPX_BLOCK_MAP_MAX_DOCUMENT_SIZE + (SIZE_T)1,
                    APPX_E_INVALID_BLOCKMAP );

    ok( !p_appx_block_map_get_file_count( NULL ), "null map has files.\n" );
    ok( !p_appx_block_map_get_block_count( NULL ), "null map has blocks.\n" );
    ok( !p_appx_block_map_get_file( NULL, 0 ), "got file for null map.\n" );
    ok( !p_appx_block_map_get_block( NULL, 0, 0 ), "got block for null map.\n" );
    p_appx_block_map_free( NULL );
}

static void test_namespaces_and_methods(void)
{
    check_xml( "<BlockMap HashMethod=\"" SHA256_METHOD "\"/>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"urn:wrong\" HashMethod=\"" SHA256_METHOD "\"/>",
               APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS_2015 "\" HashMethod=\"" SHA256_METHOD "\"/>",
               HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS_2017 "\" HashMethod=\"" SHA256_METHOD "\"/>",
               HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\"" SHA384_METHOD "\"/>",
               HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\"" SHA512_METHOD "\"/>",
               HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\"urn:unknown\"/>",
               APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\"/>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" x:HashMethod=\"" SHA256_METHOD "\""
               " xmlns:x=\"urn:x\"/>", APPX_E_INVALID_BLOCKMAP );
}

static void test_file_hash_extension(void)
{
    static const char document[] =
        "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
        " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
        "<File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
        "<Block Hash=\"" ZERO_HASH "\"/>"
        "<b4:FileHash Hash=\"" ZERO_HASH "\"/>"
        "</File>"
        "<File Name=\"empty\" Size=\"0\" LfhSize=\"30\">"
        "<b4:FileHash Hash=\"" ZERO_HASH "\"> \n</b4:FileHash>"
        "</File>"
        "</BlockMap>";
    APPX_BLOCK_MAP *map;
    const APPX_BLOCK_MAP_FILE *file;
    HRESULT hr;
    UINT32 i;

    hr = parse_xml( document, sizeof(document) - 1, &map );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( p_appx_block_map_get_file_count( map ) == 2, "got %u files.\n",
            p_appx_block_map_get_file_count( map ) );
        file = p_appx_block_map_get_file( map, 0 );
        ok( file && file->has_file_hash, "first file has no file hash.\n" );
        if (file)
        {
            for (i = 0; i < APPX_BLOCK_MAP_HASH_SIZE; i++)
                ok( !file->file_hash[i], "file hash byte %u is %#x.\n",
                    i, file->file_hash[i] );
        }
        file = p_appx_block_map_get_file( map, 1 );
        ok( file && file->has_file_hash, "empty file has no file hash.\n" );
        p_appx_block_map_free( map );
    }

    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\"/>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\"" SHA256_METHOD "\""
               " IgnorableNamespaces=\"b4\"/>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"urn:wrong\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\"/>",
               APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:x=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"x\"/>",
               APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4 other\"/>",
               APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<b4:FileHash xmlns:b4=\"" BLOCK_MAP_NS_2021 "\" Hash=\"" ZERO_HASH "\"/>"
               "</File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
               "<File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
               "<b4:FileHash Hash=\"" ZERO_HASH "\"/>"
               "<Block Hash=\"" ZERO_HASH "\"/>"
               "</File></BlockMap>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
               "<File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\"/>"
               "<b4:FileHash Hash=\"" ZERO_HASH "\"/>"
               "<Block Hash=\"" ZERO_HASH "\"/>"
               "</File></BlockMap>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
               "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<b4:FileHash Hash=\"" ZERO_HASH "\"/>"
               "<b4:FileHash Hash=\"" ZERO_HASH "\"/>"
               "</File></BlockMap>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
               "<File Name=\"a\" Size=\"0\" LfhSize=\"30\""
               " xmlns:x=\"" BLOCK_MAP_NS_2021 "\">"
               "<x:FileHash Hash=\"" ZERO_HASH "\"/>"
               "</File></BlockMap>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
               "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<b4:FileHash Hash=\"" ZERO_HASH "\" Size=\"1\"/>"
               "</File></BlockMap>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
               "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<b4:FileHash/>"
               "</File></BlockMap>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
               "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<b4:FileHash Hash=\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB=\"/>"
               "</File></BlockMap>", APPX_E_INVALID_BLOCKMAP );
    check_xml( "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" xmlns:b4=\"" BLOCK_MAP_NS_2021 "\""
               " HashMethod=\"" SHA256_METHOD "\" IgnorableNamespaces=\"b4\">"
               "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<b4:Unknown Hash=\"" ZERO_HASH "\"/>"
               "</File></BlockMap>", APPX_E_INVALID_BLOCKMAP );
}

static void test_hashes(void)
{
    static const char *invalid_hashes[] =
    {
        "",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA!=",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB=",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA A=",
    };
    struct buffer buffer = {0};
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(invalid_hashes); i++)
    {
        append_string( &buffer, ROOT_OPEN "<File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
                       "<Block Hash=\"" );
        append_string( &buffer, invalid_hashes[i] );
        append_string( &buffer, "\"/></File>" ROOT_CLOSE );
        check_xml_data( buffer.data, buffer.size, APPX_E_INVALID_BLOCKMAP );
        buffer.size = 0;
    }
    free_buffer( &buffer );

    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
               "<Block/></File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\" Size=\"4294967296\"/>"
               "</File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\" Size=\"+1\"/>"
               "</File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
}

static void test_block_counts(void)
{
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"1\" LfhSize=\"30\"/>" ROOT_CLOSE,
               APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\"/></File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"65536\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\"/></File>" ROOT_CLOSE, S_OK );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"65537\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\"/></File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"65536\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\"/><Block Hash=\"" ZERO_HASH "\"/>"
               "</File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"1\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\">x</Block></File>" ROOT_CLOSE,
               APPX_E_INVALID_BLOCKMAP );
}

static void check_path_length( UINT32 first, UINT32 second, HRESULT expected )
{
    struct buffer buffer = {0};
    APPX_BLOCK_MAP *map = NULL;
    const APPX_BLOCK_MAP_FILE *file;
    HRESULT hr;

    append_string( &buffer, ROOT_OPEN "<File Name=\"" );
    append_repeat( &buffer, 'a', first );
    append_string( &buffer, "\\" );
    append_repeat( &buffer, 'b', second );
    append_string( &buffer, "\" Size=\"0\" LfhSize=\"30\"/>" ROOT_CLOSE );
    hr = parse_xml( buffer.data, buffer.size, &map );
    ok( hr == expected, "got hr %#lx for %u+1+%u chars, expected %#lx.\n",
        hr, first, second, expected );
    if (SUCCEEDED(hr))
    {
        file = p_appx_block_map_get_file( map, 0 );
        ok( file && file->name_length == first + second + 1,
            "got name length %u.\n", file ? file->name_length : 0 );
        if (file) ok( file->name[first] == '\\', "separator is %#x.\n", file->name[first] );
    }
    p_appx_block_map_free( map );
    free_buffer( &buffer );
}

static void test_file_names(void)
{
    static const char *invalid_names[] =
    {
        "",
        "\\absolute",
        "trailing\\",
        "double\\\\separator",
        ".",
        "..",
        "a\\.\\b",
        "a\\..\\b",
        "a/b",
        "CON",
        "a\\NUL.txt",
        "trailing.",
        "trailing ",
        "colon:name",
    };
    struct buffer buffer = {0};
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(invalid_names); i++)
    {
        append_string( &buffer, ROOT_OPEN "<File Name=\"" );
        append_string( &buffer, invalid_names[i] );
        append_string( &buffer, "\" Size=\"0\" LfhSize=\"30\"/>" ROOT_CLOSE );
        check_xml_data( buffer.data, buffer.size, APPX_E_INVALID_BLOCKMAP );
        buffer.size = 0;
    }
    free_buffer( &buffer );

    check_path_length( 129, 130, S_OK );
    check_path_length( 130, 130, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN
               "<File Name=\"Folder\\Name.txt\" Size=\"0\" LfhSize=\"30\"/>"
               "<File Name=\"folder\\name.TXT\" Size=\"0\" LfhSize=\"30\"/>"
               ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
}

static void test_decimals(void)
{
    static const char *invalid_sizes[] =
    {
        "",
        "-1",
        "+1",
        " 1",
        "1 ",
        "0x1",
        "1.0",
        "17179869185",
        "18446744073709551616",
    };
    static const char *invalid_lfh_sizes[] =
    {
        "",
        "-1",
        "+1",
        " 1",
        "1 ",
        "0",
        "29",
        "65537",
        "4294967296",
    };
    struct buffer buffer = {0};
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(invalid_sizes); i++)
    {
        append_string( &buffer, ROOT_OPEN "<File Name=\"a\" Size=\"" );
        append_string( &buffer, invalid_sizes[i] );
        append_string( &buffer, "\" LfhSize=\"30\"/>" ROOT_CLOSE );
        check_xml_data( buffer.data, buffer.size, APPX_E_INVALID_BLOCKMAP );
        buffer.size = 0;
    }
    for (i = 0; i < ARRAY_SIZE(invalid_lfh_sizes); i++)
    {
        append_string( &buffer, ROOT_OPEN "<File Name=\"a\" Size=\"0\" LfhSize=\"" );
        append_string( &buffer, invalid_lfh_sizes[i] );
        append_string( &buffer, "\"/>" ROOT_CLOSE );
        check_xml_data( buffer.data, buffer.size, APPX_E_INVALID_BLOCKMAP );
        buffer.size = 0;
    }
    free_buffer( &buffer );

    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"0\" LfhSize=\"65536\"/>"
               ROOT_CLOSE, S_OK );
    check_xml( ROOT_OPEN "<File Name=\"a\" LfhSize=\"30\"/>" ROOT_CLOSE,
               APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"0\"/>" ROOT_CLOSE,
               APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Size=\"0\" LfhSize=\"30\"/>" ROOT_CLOSE,
               APPX_E_INVALID_BLOCKMAP );
}

static void test_xml_security(void)
{
    check_xml( "<?xml version=\"1.0\"?><!DOCTYPE BlockMap>"
               ROOT_OPEN ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( "<?xml version=\"1.0\"?><!DOCTYPE BlockMap SYSTEM=\"file:///etc/passwd\">"
               ROOT_OPEN ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( "<?xml version=\"1.0\"?><!DOCTYPE BlockMap ["
               "<!ENTITY x \"empty\">]>" ROOT_OPEN
               "<File Name=\"&x;\" Size=\"0\" LfhSize=\"30\"/>" ROOT_CLOSE,
               APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a&amp;b\" Size=\"0\" LfhSize=\"30\"/>"
               ROOT_CLOSE, S_OK );
    check_xml( "<?before root?>" ROOT_OPEN ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<!-- bounded comment -->" ROOT_CLOSE, S_OK );
    check_xml( ROOT_OPEN "<!-- <!DOCTYPE BlockMap SYSTEM=\"file:///etc/passwd\"> -->"
               ROOT_CLOSE, S_OK );
    check_xml( ROOT_OPEN "<![CDATA[ ]]>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<xi:include xmlns:xi=\"http://www.w3.org/2001/XInclude\""
               " href=\"file:///etc/passwd\"/>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<Unknown/>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"0\" LfhSize=\"30\" Unknown=\"x\"/>"
               ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<Unknown/></File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
    check_xml( ROOT_OPEN "<File Name=\"a\" Size=\"0\" LfhSize=\"30\">"
               "<Block Hash=\"" ZERO_HASH "\"><Block Hash=\"" ZERO_HASH "\"/>"
               "</Block></File>" ROOT_CLOSE, APPX_E_INVALID_BLOCKMAP );
}

static void test_parser_limits(void)
{
    struct buffer buffer = {0};
    UINT32 i;

    append_string( &buffer, "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\""
                   SHA256_METHOD "\"" );
    for (i = 0; i < 7; i++)
    {
        char declaration[] = " xmlns:a=\"urn:a\"";
        declaration[7] = 'a' + i;
        declaration[14] = 'a' + i;
        append_string( &buffer, declaration );
    }
    append_string( &buffer, "/>" );
    check_xml_data( buffer.data, buffer.size, APPX_E_INVALID_BLOCKMAP );
    buffer.size = 0;

    append_string( &buffer, "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\""
                   SHA256_METHOD "\" xmlns:x=\"" );
    append_repeat( &buffer, 'a', APPX_BLOCK_MAP_MAX_XML_VALUE_BYTES + 1 );
    append_string( &buffer, "\"/>" );
    check_xml_data( buffer.data, buffer.size, APPX_E_INVALID_BLOCKMAP );
    buffer.size = 0;

    append_string( &buffer, "<BlockMap xmlns=\"" BLOCK_MAP_NS "\" HashMethod=\""
                   SHA256_METHOD "\" xmlns:" );
    append_repeat( &buffer, 'a', APPX_BLOCK_MAP_MAX_XML_NAME_BYTES + 1 );
    append_string( &buffer, "=\"urn:a\"/>" );
    check_xml_data( buffer.data, buffer.size, APPX_E_INVALID_BLOCKMAP );
    buffer.size = 0;

    append_string( &buffer, ROOT_OPEN );
    for (i = 0; i < APPX_BLOCK_MAP_MAX_DEPTH + 1; i++)
        append_string( &buffer, "<x>" );
    for (i = 0; i < APPX_BLOCK_MAP_MAX_DEPTH + 1; i++)
        append_string( &buffer, "</x>" );
    append_string( &buffer, ROOT_CLOSE );
    check_xml_data( buffer.data, buffer.size, APPX_E_INVALID_BLOCKMAP );
    free_buffer( &buffer );
}

static void test_invalid_utf8(void)
{
    static const BYTE invalid_name[] =
        ROOT_OPEN "<File Name=\"bad\xc0\xafname\" Size=\"0\" LfhSize=\"30\"/>" ROOT_CLOSE;
    static const BYTE invalid_text[] = ROOT_OPEN "\xed\xa0\x80" ROOT_CLOSE;
    static const BYTE truncated[] =
        ROOT_OPEN "<File Name=\"bad\xe2\x82\" Size=\"0\" LfhSize=\"30\"/>" ROOT_CLOSE;

    check_xml_data( invalid_name, sizeof(invalid_name) - 1, APPX_E_INVALID_BLOCKMAP );
    check_xml_data( invalid_text, sizeof(invalid_text) - 1, APPX_E_INVALID_BLOCKMAP );
    check_xml_data( truncated, sizeof(truncated) - 1, APPX_E_INVALID_BLOCKMAP );
}

START_TEST(blockmap)
{
    HMODULE module = LoadLibraryW( L"appxsvc.dll" );

    if (!module)
    {
        ok( 0, "appxsvc.dll is unavailable, error %lu.\n", GetLastError() );
        return;
    }
    p_appx_block_map_parse = (void *)GetProcAddress( module, "appx_block_map_parse" );
    p_appx_block_map_free = (void *)GetProcAddress( module, "appx_block_map_free" );
    p_appx_block_map_get_file_count =
        (void *)GetProcAddress( module, "appx_block_map_get_file_count" );
    p_appx_block_map_get_block_count =
        (void *)GetProcAddress( module, "appx_block_map_get_block_count" );
    p_appx_block_map_get_file = (void *)GetProcAddress( module, "appx_block_map_get_file" );
    p_appx_block_map_get_block = (void *)GetProcAddress( module, "appx_block_map_get_block" );
    p_wine_appx_validate_archive_path =
        (void *)GetProcAddress( module, "wine_appx_validate_archive_path" );
    if (!p_appx_block_map_parse || !p_appx_block_map_free ||
        !p_appx_block_map_get_file_count || !p_appx_block_map_get_block_count ||
        !p_appx_block_map_get_file || !p_appx_block_map_get_block ||
        !p_wine_appx_validate_archive_path)
    {
        win_skip( "AppX block map parser exports are unavailable.\n" );
        FreeLibrary( module );
        return;
    }

    test_normal();
    test_arguments_and_empty();
    test_namespaces_and_methods();
    test_file_hash_extension();
    test_hashes();
    test_block_counts();
    test_file_names();
    test_decimals();
    test_xml_security();
    test_parser_limits();
    test_invalid_utf8();
    FreeLibrary( module );
}
