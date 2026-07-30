/*
 * AppX block map parser
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

#include <string.h>

#include <libxml/xmlreader.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"

#include "wine/appxsvc.h"

#include "blockmap.h"

static const xmlChar block_map_namespace[] =
    "http://schemas.microsoft.com/appx/2010/blockmap";
static const xmlChar block_map_namespace_2015[] =
    "http://schemas.microsoft.com/appx/2015/blockmap";
static const xmlChar block_map_namespace_2017[] =
    "http://schemas.microsoft.com/appx/2017/blockmap";
static const xmlChar block_map_namespace_2021[] =
    "http://schemas.microsoft.com/appx/2021/blockmap";
static const xmlChar sha256_method[] =
    "http://www.w3.org/2001/04/xmlenc#sha256";
static const xmlChar sha384_method[] =
    "http://www.w3.org/2001/04/xmldsig-more#sha384";
static const xmlChar sha512_method[] =
    "http://www.w3.org/2001/04/xmlenc#sha512";

struct block_map_file
{
    APPX_BLOCK_MAP_FILE public;
    UINT32 first_block;
};

struct appx_block_map
{
    struct block_map_file *files;
    APPX_BLOCK_MAP_BLOCK *blocks;
    UINT64 total_file_size;
    UINT32 file_count;
    UINT32 file_capacity;
    UINT32 block_count;
    UINT32 block_capacity;
};

struct parser
{
    APPX_BLOCK_MAP *map;
    xmlTextReaderPtr reader;
    UINT64 total_name_bytes;
    UINT64 total_value_bytes;
    UINT32 node_count;
    UINT32 element_count;
    UINT32 attribute_count;
    UINT32 current_file;
    BOOL root_seen;
    BOOL root_ended;
    BOOL declaration_seen;
    BOOL file_hash_extension;
    BOOL inside_file;
    BOOL inside_block;
    BOOL inside_file_hash;
};

static HRESULT invalid_block_map(void)
{
    return APPX_E_INVALID_BLOCKMAP;
}

static BOOL add_uint64( UINT64 left, UINT64 right, UINT64 *result )
{
    if (~(UINT64)0 - left < right) return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL multiply_size( SIZE_T left, SIZE_T right, SIZE_T *result )
{
    if (left && ~(SIZE_T)0 / left < right) return FALSE;
    *result = left * right;
    return TRUE;
}

static BOOL xml_equal( const xmlChar *left, const xmlChar *right )
{
    return left && right && !xmlStrcmp( left, right );
}

static BOOL empty_namespace( const xmlChar *uri )
{
    return !uri || !*uri;
}

static HRESULT add_bounded_length( UINT64 *total, UINT32 length, UINT64 limit )
{
    UINT64 value;

    if (!add_uint64( *total, length, &value ) || value > limit)
        return invalid_block_map();
    *total = value;
    return S_OK;
}

static HRESULT inspect_string( struct parser *parser, const xmlChar *string,
                               UINT32 maximum, BOOL name, UINT32 *length )
{
    SIZE_T count;
    HRESULT hr;

    if (!string)
    {
        if (length) *length = 0;
        return S_OK;
    }

    count = xmlStrlen( string );
    if (count > maximum || count > MAXDWORD) return invalid_block_map();
    hr = add_bounded_length( name ? &parser->total_name_bytes : &parser->total_value_bytes,
                             (UINT32)count, name ? APPX_BLOCK_MAP_MAX_TOTAL_NAME_BYTES :
                                                   APPX_BLOCK_MAP_MAX_TOTAL_VALUE_BYTES );
    if (FAILED(hr)) return hr;
    if (length) *length = count;
    return S_OK;
}

static HRESULT inspect_node_strings( struct parser *parser )
{
    const xmlChar *name = xmlTextReaderConstName( parser->reader );
    const xmlChar *local = xmlTextReaderConstLocalName( parser->reader );
    const xmlChar *prefix = xmlTextReaderConstPrefix( parser->reader );
    const xmlChar *uri = xmlTextReaderConstNamespaceUri( parser->reader );
    const xmlChar *value = xmlTextReaderConstValue( parser->reader );
    HRESULT hr;

    if (FAILED(hr = inspect_string( parser, name, APPX_BLOCK_MAP_MAX_XML_NAME_BYTES,
                                    TRUE, NULL ))) return hr;
    if (FAILED(hr = inspect_string( parser, local, APPX_BLOCK_MAP_MAX_XML_NAME_BYTES,
                                    TRUE, NULL ))) return hr;
    if (FAILED(hr = inspect_string( parser, prefix, APPX_BLOCK_MAP_MAX_XML_NAME_BYTES,
                                    TRUE, NULL ))) return hr;
    if (FAILED(hr = inspect_string( parser, uri, APPX_BLOCK_MAP_MAX_XML_VALUE_BYTES,
                                    FALSE, NULL ))) return hr;
    return inspect_string( parser, value, APPX_BLOCK_MAP_MAX_XML_VALUE_BYTES,
                           FALSE, NULL );
}

static BOOL is_xml_whitespace( const xmlChar *value )
{
    if (!value) return TRUE;
    while (*value)
    {
        if (*value != ' ' && *value != '\t' && *value != '\r' && *value != '\n')
            return FALSE;
        value++;
    }
    return TRUE;
}

static SIZE_T skip_xml_section( const BYTE *document, SIZE_T size, SIZE_T offset,
                               const char *terminator )
{
    SIZE_T length = strlen( terminator );

    while (offset + length <= size)
    {
        if (!memcmp( document + offset, terminator, length ))
            return offset + length;
        offset++;
    }
    return size;
}

static BOOL contains_doctype_declaration( const BYTE *document, SIZE_T size )
{
    static const char comment[] = "<!--";
    static const char cdata[] = "<![CDATA[";
    static const char instruction[] = "<?";
    static const char doctype[] = "<!DOCTYPE";
    SIZE_T i = 0;

    while (i < size)
    {
        if (document[i] != '<')
        {
            i++;
            continue;
        }
        if (size - i >= sizeof(comment) - 1 &&
            !memcmp( document + i, comment, sizeof(comment) - 1 ))
        {
            i = skip_xml_section( document, size, i + sizeof(comment) - 1, "-->" );
            continue;
        }
        if (size - i >= sizeof(cdata) - 1 &&
            !memcmp( document + i, cdata, sizeof(cdata) - 1 ))
        {
            i = skip_xml_section( document, size, i + sizeof(cdata) - 1, "]]>" );
            continue;
        }
        if (size - i >= sizeof(instruction) - 1 &&
            !memcmp( document + i, instruction, sizeof(instruction) - 1 ))
        {
            i = skip_xml_section( document, size, i + sizeof(instruction) - 1, "?>" );
            continue;
        }
        if (size - i >= sizeof(doctype) - 1 &&
            !memcmp( document + i, doctype, sizeof(doctype) - 1 ))
            return TRUE;
        i++;
    }
    return FALSE;
}

static BOOL parse_uint64( const xmlChar *value, UINT64 maximum, UINT64 *result )
{
    UINT64 number = 0;

    if (!value || !*value) return FALSE;
    while (*value)
    {
        UINT32 digit;

        if (*value < '0' || *value > '9') return FALSE;
        digit = *value++ - '0';
        if (number > (maximum - digit) / 10) return FALSE;
        number = number * 10 + digit;
    }
    *result = number;
    return TRUE;
}

static int base64_value( xmlChar ch )
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static BOOL parse_sha256( const xmlChar *value, BYTE hash[APPX_BLOCK_MAP_HASH_SIZE] )
{
    UINT32 input, output = 0;

    if (!value || xmlStrlen( value ) != 44 || value[43] != '=')
        return FALSE;

    for (input = 0; input < 40; input += 4)
    {
        int a = base64_value( value[input] );
        int b = base64_value( value[input + 1] );
        int c = base64_value( value[input + 2] );
        int d = base64_value( value[input + 3] );

        if (a < 0 || b < 0 || c < 0 || d < 0) return FALSE;
        hash[output++] = (a << 2) | (b >> 4);
        hash[output++] = (b << 4) | (c >> 2);
        hash[output++] = (c << 6) | d;
    }

    {
        int a = base64_value( value[40] );
        int b = base64_value( value[41] );
        int c = base64_value( value[42] );

        if (a < 0 || b < 0 || c < 0 || (c & 3)) return FALSE;
        hash[output++] = (a << 2) | (b >> 4);
        hash[output++] = (b << 4) | (c >> 2);
    }
    return output == APPX_BLOCK_MAP_HASH_SIZE;
}

static HRESULT reserve_files( APPX_BLOCK_MAP *map, UINT32 required )
{
    struct block_map_file *files;
    SIZE_T size;
    UINT32 capacity;

    if (required > APPX_BLOCK_MAP_MAX_FILES) return invalid_block_map();
    if (required <= map->file_capacity) return S_OK;

    capacity = map->file_capacity ? map->file_capacity : 16;
    while (capacity < required)
    {
        if (capacity > APPX_BLOCK_MAP_MAX_FILES / 2)
        {
            capacity = APPX_BLOCK_MAP_MAX_FILES;
            break;
        }
        capacity *= 2;
    }
    if (!multiply_size( capacity, sizeof(*files), &size )) return E_OUTOFMEMORY;
    if (map->files)
        files = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, map->files, size );
    else
        files = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size );
    if (!files) return E_OUTOFMEMORY;
    map->files = files;
    map->file_capacity = capacity;
    return S_OK;
}

static HRESULT reserve_blocks( APPX_BLOCK_MAP *map, UINT32 required )
{
    APPX_BLOCK_MAP_BLOCK *blocks;
    SIZE_T size;
    UINT32 capacity;

    if (required > APPX_BLOCK_MAP_MAX_BLOCKS) return invalid_block_map();
    if (required <= map->block_capacity) return S_OK;

    capacity = map->block_capacity ? map->block_capacity : 64;
    while (capacity < required)
    {
        if (capacity > APPX_BLOCK_MAP_MAX_BLOCKS / 2)
        {
            capacity = APPX_BLOCK_MAP_MAX_BLOCKS;
            break;
        }
        capacity *= 2;
    }
    if (!multiply_size( capacity, sizeof(*blocks), &size )) return E_OUTOFMEMORY;
    if (map->blocks)
        blocks = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, map->blocks, size );
    else
        blocks = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size );
    if (!blocks) return E_OUTOFMEMORY;
    map->blocks = blocks;
    map->block_capacity = capacity;
    return S_OK;
}

static HRESULT canonicalize_file_name( const xmlChar *value, WCHAR **name, UINT32 *name_length )
{
    BYTE *archive_name;
    WCHAR *path;
    UINT32 bytes, capacity = 0, output = 0, required = 0, i;
    HRESULT hr;

    *name = NULL;
    *name_length = 0;
    if (!value || !(bytes = (UINT32)xmlStrlen( value )) ||
        bytes > APPX_BLOCK_MAP_MAX_XML_VALUE_BYTES)
        return invalid_block_map();

    if (!(archive_name = HeapAlloc( GetProcessHeap(), 0, bytes * 3 )))
        return E_OUTOFMEMORY;
    for (i = 0; i < bytes; i++)
    {
        if (value[i] == '/')
        {
            HeapFree( GetProcessHeap(), 0, archive_name );
            return invalid_block_map();
        }
        if (value[i] == '\\')
            archive_name[output++] = '/';
        else if (value[i] == '%')
        {
            /*
             * Block-map names are already decoded, whereas the archive path
             * validator decodes OPC percent escapes.  Escape a literal
             * percent once so both representations canonicalize identically.
             */
            archive_name[output++] = '%';
            archive_name[output++] = '2';
            archive_name[output++] = '5';
        }
        else
            archive_name[output++] = value[i];
    }

    hr = wine_appx_validate_archive_path( archive_name, output, 0, &required, NULL );
    if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
    {
        HeapFree( GetProcessHeap(), 0, archive_name );
        return FAILED(hr) && hr == E_OUTOFMEMORY ? hr : invalid_block_map();
    }
    if (!required || required - 1 > APPX_BLOCK_MAP_MAX_FILE_NAME_CHARS)
    {
        HeapFree( GetProcessHeap(), 0, archive_name );
        return invalid_block_map();
    }

    if (!(path = HeapAlloc( GetProcessHeap(), 0, required * sizeof(*path) )))
    {
        HeapFree( GetProcessHeap(), 0, archive_name );
        return E_OUTOFMEMORY;
    }
    capacity = required;
    hr = wine_appx_validate_archive_path( archive_name, output, 0, &capacity, path );
    HeapFree( GetProcessHeap(), 0, archive_name );
    if (FAILED(hr) || capacity != required)
    {
        HeapFree( GetProcessHeap(), 0, path );
        return hr == E_OUTOFMEMORY ? hr : invalid_block_map();
    }

    *name = path;
    *name_length = required - 1;
    return S_OK;
}

struct attributes
{
    const xmlChar *name;
    const xmlChar *size;
    const xmlChar *lfh_size;
    const xmlChar *hash;
    const xmlChar *hash_method;
    const xmlChar *ignorable_namespaces;
    BOOL has_size;
    BOOL has_b4_declaration;
};

enum element_kind
{
    ELEMENT_BLOCK_MAP,
    ELEMENT_FILE,
    ELEMENT_BLOCK,
    ELEMENT_FILE_HASH
};

static HRESULT read_attributes( struct parser *parser, enum element_kind kind,
                                struct attributes *attributes )
{
    int count, moved;
    HRESULT hr;

    memset( attributes, 0, sizeof(*attributes) );
    count = xmlTextReaderAttributeCount( parser->reader );
    if (count < 0 || count > APPX_BLOCK_MAP_MAX_ATTRIBUTES_PER_NODE ||
        parser->attribute_count > APPX_BLOCK_MAP_MAX_ATTRIBUTES - count)
        return invalid_block_map();
    parser->attribute_count += (UINT32)count;

    moved = xmlTextReaderMoveToFirstAttribute( parser->reader );
    while (moved == 1)
    {
        const xmlChar *local;
        const xmlChar *uri;
        const xmlChar *value;

        if (FAILED(hr = inspect_node_strings( parser ))) return hr;
        local = xmlTextReaderConstLocalName( parser->reader );
        uri = xmlTextReaderConstNamespaceUri( parser->reader );
        value = xmlTextReaderConstValue( parser->reader );
        if (xmlTextReaderIsNamespaceDecl( parser->reader ))
        {
            if (kind == ELEMENT_BLOCK_MAP)
            {
                if (xml_equal( value, block_map_namespace_2021 ))
                {
                    if (!xml_equal( local, BAD_CAST "b4" ) ||
                        attributes->has_b4_declaration)
                        return invalid_block_map();
                    attributes->has_b4_declaration = TRUE;
                }
                else if (xml_equal( local, BAD_CAST "b4" ))
                {
                    return invalid_block_map();
                }
            }
            moved = xmlTextReaderMoveToNextAttribute( parser->reader );
            continue;
        }
        if (!local || !empty_namespace( uri ) || !value)
            return invalid_block_map();

        if (kind == ELEMENT_BLOCK_MAP && xml_equal( local, BAD_CAST "HashMethod" ))
        {
            if (attributes->hash_method) return invalid_block_map();
            attributes->hash_method = value;
        }
        else if (kind == ELEMENT_BLOCK_MAP &&
                 xml_equal( local, BAD_CAST "IgnorableNamespaces" ))
        {
            if (attributes->ignorable_namespaces) return invalid_block_map();
            attributes->ignorable_namespaces = value;
        }
        else if (kind == ELEMENT_FILE && xml_equal( local, BAD_CAST "Name" ))
        {
            if (attributes->name) return invalid_block_map();
            attributes->name = value;
        }
        else if (kind == ELEMENT_FILE && xml_equal( local, BAD_CAST "Size" ))
        {
            if (attributes->has_size) return invalid_block_map();
            attributes->size = value;
            attributes->has_size = TRUE;
        }
        else if (kind == ELEMENT_FILE && xml_equal( local, BAD_CAST "LfhSize" ))
        {
            if (attributes->lfh_size) return invalid_block_map();
            attributes->lfh_size = value;
        }
        else if ((kind == ELEMENT_BLOCK || kind == ELEMENT_FILE_HASH) &&
                 xml_equal( local, BAD_CAST "Hash" ))
        {
            if (attributes->hash) return invalid_block_map();
            attributes->hash = value;
        }
        else if (kind == ELEMENT_BLOCK && xml_equal( local, BAD_CAST "Size" ))
        {
            if (attributes->has_size) return invalid_block_map();
            attributes->size = value;
            attributes->has_size = TRUE;
        }
        else
        {
            return invalid_block_map();
        }
        moved = xmlTextReaderMoveToNextAttribute( parser->reader );
    }
    if (moved < 0 || xmlTextReaderMoveToElement( parser->reader ) < 0)
        return invalid_block_map();
    return S_OK;
}

static HRESULT parse_root( struct parser *parser, BOOL empty )
{
    struct attributes attributes;
    const xmlChar *uri = xmlTextReaderConstNamespaceUri( parser->reader );
    HRESULT hr;

    if (parser->root_seen || parser->root_ended ||
        !xml_equal( xmlTextReaderConstLocalName( parser->reader ), BAD_CAST "BlockMap" ))
        return invalid_block_map();
    if (xml_equal( uri, block_map_namespace_2015 ) ||
        xml_equal( uri, block_map_namespace_2017 ))
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    if (!xml_equal( uri, block_map_namespace )) return invalid_block_map();
    if (FAILED(hr = read_attributes( parser, ELEMENT_BLOCK_MAP, &attributes ))) return hr;
    if (!attributes.hash_method) return invalid_block_map();
    if (xml_equal( attributes.hash_method, sha384_method ) ||
        xml_equal( attributes.hash_method, sha512_method ))
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    if (!xml_equal( attributes.hash_method, sha256_method ))
        return invalid_block_map();
    if (attributes.ignorable_namespaces)
    {
        if (!xml_equal( attributes.ignorable_namespaces, BAD_CAST "b4" ) ||
            !attributes.has_b4_declaration)
            return invalid_block_map();
        parser->file_hash_extension = TRUE;
    }
    else if (attributes.has_b4_declaration)
    {
        return invalid_block_map();
    }

    parser->root_seen = TRUE;
    parser->root_ended = empty;
    return S_OK;
}

static HRESULT parse_file( struct parser *parser, BOOL empty )
{
    struct block_map_file *file;
    struct attributes attributes;
    UINT64 size, lfh_size, total;
    HRESULT hr;

    if (!parser->root_seen || parser->root_ended || parser->inside_file ||
        parser->inside_block || parser->inside_file_hash) return invalid_block_map();
    if (FAILED(hr = read_attributes( parser, ELEMENT_FILE, &attributes ))) return hr;
    if (!attributes.name || !attributes.has_size || !attributes.lfh_size ||
        !parse_uint64( attributes.size, APPX_BLOCK_MAP_MAX_FILE_SIZE, &size ) ||
        !parse_uint64( attributes.lfh_size, 65536, &lfh_size ) ||
        lfh_size < 30)
        return invalid_block_map();
    if (!add_uint64( parser->map->total_file_size, size, &total ) ||
        total > APPX_BLOCK_MAP_MAX_TOTAL_FILE_SIZE)
        return invalid_block_map();
    if (parser->map->file_count == MAXDWORD) return invalid_block_map();
    if (FAILED(hr = reserve_files( parser->map, parser->map->file_count + 1 ))) return hr;

    file = &parser->map->files[parser->map->file_count];
    memset( file, 0, sizeof(*file) );
    if (FAILED(hr = canonicalize_file_name( attributes.name, (WCHAR **)&file->public.name,
                                            &file->public.name_length ))) return hr;
    file->public.size = size;
    file->public.local_file_header_size = (UINT32)lfh_size;
    file->first_block = parser->map->block_count;
    parser->current_file = parser->map->file_count++;
    parser->map->total_file_size = total;
    parser->inside_file = !empty;
    if (empty && size) return invalid_block_map();
    return S_OK;
}

static HRESULT parse_block( struct parser *parser, BOOL empty )
{
    APPX_BLOCK_MAP_BLOCK *block;
    struct block_map_file *file;
    struct attributes attributes;
    BYTE hash[APPX_BLOCK_MAP_HASH_SIZE];
    UINT64 offset, remaining, compressed_size = 0;
    UINT32 expected;
    HRESULT hr;

    if (!parser->inside_file || parser->inside_block || parser->inside_file_hash)
        return invalid_block_map();
    if (FAILED(hr = read_attributes( parser, ELEMENT_BLOCK, &attributes ))) return hr;
    if (!attributes.hash || !parse_sha256( attributes.hash, hash ))
        return invalid_block_map();
    if (attributes.has_size &&
        !parse_uint64( attributes.size, MAXDWORD, &compressed_size ))
        return invalid_block_map();

    file = &parser->map->files[parser->current_file];
    if (file->public.has_file_hash) return invalid_block_map();
    expected = (UINT32)(file->public.size / APPX_BLOCK_MAP_BLOCK_SIZE);
    if (file->public.size % APPX_BLOCK_MAP_BLOCK_SIZE) expected++;
    if (file->public.block_count >= expected || parser->map->block_count == MAXDWORD)
        return invalid_block_map();
    if (FAILED(hr = reserve_blocks( parser->map, parser->map->block_count + 1 ))) return hr;

    block = &parser->map->blocks[parser->map->block_count];
    memset( block, 0, sizeof(*block) );
    memcpy( block->hash, hash, sizeof(block->hash) );
    offset = (UINT64)file->public.block_count * APPX_BLOCK_MAP_BLOCK_SIZE;
    remaining = file->public.size - offset;
    block->logical_size = remaining > APPX_BLOCK_MAP_BLOCK_SIZE ?
                          APPX_BLOCK_MAP_BLOCK_SIZE : (UINT32)remaining;
    block->has_compressed_size = attributes.has_size;
    if (attributes.has_size) block->compressed_size = (UINT32)compressed_size;
    file->public.block_count++;
    parser->map->block_count++;
    parser->inside_block = !empty;
    return S_OK;
}

static HRESULT parse_file_hash( struct parser *parser, BOOL empty )
{
    struct block_map_file *file;
    struct attributes attributes;
    BYTE hash[APPX_BLOCK_MAP_HASH_SIZE];
    UINT32 expected;
    HRESULT hr;

    if (!parser->file_hash_extension || !parser->inside_file ||
        parser->inside_block || parser->inside_file_hash ||
        !xml_equal( xmlTextReaderConstPrefix( parser->reader ), BAD_CAST "b4" ))
        return invalid_block_map();
    file = &parser->map->files[parser->current_file];
    if (file->public.has_file_hash) return invalid_block_map();
    expected = (UINT32)(file->public.size / APPX_BLOCK_MAP_BLOCK_SIZE);
    if (file->public.size % APPX_BLOCK_MAP_BLOCK_SIZE) expected++;
    if (file->public.block_count != expected) return invalid_block_map();
    if (FAILED(hr = read_attributes( parser, ELEMENT_FILE_HASH, &attributes ))) return hr;
    if (!attributes.hash || !parse_sha256( attributes.hash, hash ))
        return invalid_block_map();

    memcpy( file->public.file_hash, hash, sizeof(file->public.file_hash) );
    file->public.has_file_hash = TRUE;
    parser->inside_file_hash = !empty;
    return S_OK;
}

static HRESULT parse_element( struct parser *parser )
{
    const xmlChar *local = xmlTextReaderConstLocalName( parser->reader );
    const xmlChar *uri = xmlTextReaderConstNamespaceUri( parser->reader );
    int depth = xmlTextReaderDepth( parser->reader );
    BOOL empty = xmlTextReaderIsEmptyElement( parser->reader );

    if (depth < 0 || depth >= APPX_BLOCK_MAP_MAX_DEPTH) return invalid_block_map();
    if (parser->element_count == APPX_BLOCK_MAP_MAX_ELEMENTS) return invalid_block_map();
    parser->element_count++;

    if (!depth) return parse_root( parser, empty );
    if (depth == 1 && xml_equal( uri, block_map_namespace ) &&
        xml_equal( local, BAD_CAST "File" ))
        return parse_file( parser, empty );
    if (depth == 2 && xml_equal( uri, block_map_namespace ) &&
        xml_equal( local, BAD_CAST "Block" ))
        return parse_block( parser, empty );
    if (depth == 2 && xml_equal( uri, block_map_namespace_2021 ) &&
        xml_equal( local, BAD_CAST "FileHash" ))
        return parse_file_hash( parser, empty );
    return invalid_block_map();
}

static HRESULT finish_file( struct parser *parser )
{
    const struct block_map_file *file = &parser->map->files[parser->current_file];
    UINT32 expected = (UINT32)(file->public.size / APPX_BLOCK_MAP_BLOCK_SIZE);

    if (file->public.size % APPX_BLOCK_MAP_BLOCK_SIZE) expected++;
    if (file->public.block_count != expected) return invalid_block_map();
    parser->inside_file = FALSE;
    return S_OK;
}

static HRESULT parse_end_element( struct parser *parser )
{
    const xmlChar *local = xmlTextReaderConstLocalName( parser->reader );
    const xmlChar *uri = xmlTextReaderConstNamespaceUri( parser->reader );
    int depth = xmlTextReaderDepth( parser->reader );

    if (depth < 0 || depth >= APPX_BLOCK_MAP_MAX_DEPTH)
        return invalid_block_map();
    if (depth == 2 && xml_equal( uri, block_map_namespace ) &&
        xml_equal( local, BAD_CAST "Block" ) && parser->inside_block)
    {
        parser->inside_block = FALSE;
        return S_OK;
    }
    if (depth == 2 && xml_equal( uri, block_map_namespace_2021 ) &&
        xml_equal( local, BAD_CAST "FileHash" ) && parser->inside_file_hash)
    {
        parser->inside_file_hash = FALSE;
        return S_OK;
    }
    if (depth == 1 && xml_equal( uri, block_map_namespace ) &&
        xml_equal( local, BAD_CAST "File" ) && parser->inside_file &&
        !parser->inside_block && !parser->inside_file_hash)
        return finish_file( parser );
    if (!depth && xml_equal( uri, block_map_namespace ) &&
        xml_equal( local, BAD_CAST "BlockMap" ) &&
        parser->root_seen && !parser->root_ended && !parser->inside_file &&
        !parser->inside_block && !parser->inside_file_hash)
    {
        parser->root_ended = TRUE;
        return S_OK;
    }
    return invalid_block_map();
}

static int compare_file_names( const APPX_BLOCK_MAP *map, UINT32 left, UINT32 right )
{
    const APPX_BLOCK_MAP_FILE *a = &map->files[left].public;
    const APPX_BLOCK_MAP_FILE *b = &map->files[right].public;
    int result = CompareStringOrdinal( a->name, (int)a->name_length,
                                       b->name, (int)b->name_length, TRUE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static void merge_name_indexes( const APPX_BLOCK_MAP *map, UINT32 *indexes, UINT32 *temporary,
                                UINT32 begin, UINT32 middle, UINT32 end )
{
    UINT32 left = begin, right = middle, output = begin, i;

    while (left < middle && right < end)
    {
        if (compare_file_names( map, indexes[left], indexes[right] ) <= 0)
            temporary[output++] = indexes[left++];
        else
            temporary[output++] = indexes[right++];
    }
    while (left < middle) temporary[output++] = indexes[left++];
    while (right < end) temporary[output++] = indexes[right++];
    for (i = begin; i < end; i++) indexes[i] = temporary[i];
}

static void sort_name_indexes( const APPX_BLOCK_MAP *map, UINT32 *indexes, UINT32 *temporary,
                               UINT32 begin, UINT32 end )
{
    UINT32 middle;

    if (end - begin < 2) return;
    middle = begin + (end - begin) / 2;
    sort_name_indexes( map, indexes, temporary, begin, middle );
    sort_name_indexes( map, indexes, temporary, middle, end );
    merge_name_indexes( map, indexes, temporary, begin, middle, end );
}

static HRESULT reject_duplicate_names( const APPX_BLOCK_MAP *map )
{
    UINT32 *allocation, *indexes, *temporary, i;
    SIZE_T count, size;
    HRESULT hr = S_OK;

    if (map->file_count < 2) return S_OK;
    if (!multiply_size( map->file_count, 2, &count ) ||
        !multiply_size( count, sizeof(*allocation), &size ))
        return E_OUTOFMEMORY;
    if (!(allocation = HeapAlloc( GetProcessHeap(), 0, size ))) return E_OUTOFMEMORY;
    indexes = allocation;
    temporary = allocation + map->file_count;
    for (i = 0; i < map->file_count; i++) indexes[i] = i;
    sort_name_indexes( map, indexes, temporary, 0, map->file_count );
    for (i = 1; i < map->file_count; i++)
    {
        if (!compare_file_names( map, indexes[i - 1], indexes[i] ))
        {
            hr = invalid_block_map();
            break;
        }
    }
    HeapFree( GetProcessHeap(), 0, allocation );
    return hr;
}

static void xml_reader_error( void *argument, const char *message,
                              xmlParserSeverities severity, xmlTextReaderLocatorPtr locator )
{
    /* Malformed input is reported through APPX_E_INVALID_BLOCKMAP. */
    (void)argument;
    (void)message;
    (void)severity;
    (void)locator;
}

static HRESULT xml_failure(void)
{
    const xmlError *error = xmlGetLastError();

    if (error && error->code == XML_ERR_NO_MEMORY) return E_OUTOFMEMORY;
    return invalid_block_map();
}

HRESULT WINAPI appx_block_map_parse( const BYTE *document, UINT32 size, APPX_BLOCK_MAP **result )
{
    struct parser parser = {0};
    const xmlChar *encoding, *version;
    HRESULT hr = S_OK;
    int status;

    if (!result) return E_INVALIDARG;
    *result = NULL;
    if (!document) return E_INVALIDARG;
    if (!size || size > APPX_BLOCK_MAP_MAX_DOCUMENT_SIZE ||
        contains_doctype_declaration( document, size ))
        return invalid_block_map();

    if (!(parser.map = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*parser.map) )))
        return E_OUTOFMEMORY;
    parser.current_file = MAXDWORD;
    xmlResetLastError();
    if (!(parser.reader = xmlReaderForMemory( (const char *)document, (int)size, NULL, NULL,
                                              XML_PARSE_NONET )))
    {
        appx_block_map_free( parser.map );
        return xml_failure();
    }
    xmlTextReaderSetErrorHandler( parser.reader, xml_reader_error, NULL );
    if (xmlTextReaderSetParserProp( parser.reader, XML_PARSER_LOADDTD, 0 ) < 0 ||
        xmlTextReaderSetParserProp( parser.reader, XML_PARSER_DEFAULTATTRS, 0 ) < 0 ||
        xmlTextReaderSetParserProp( parser.reader, XML_PARSER_VALIDATE, 0 ) < 0 ||
        xmlTextReaderSetParserProp( parser.reader, XML_PARSER_SUBST_ENTITIES, 0 ) < 0)
    {
        xmlFreeTextReader( parser.reader );
        appx_block_map_free( parser.map );
        return invalid_block_map();
    }

    while ((status = xmlTextReaderRead( parser.reader )) == 1)
    {
        int depth, type;

        if (parser.node_count == APPX_BLOCK_MAP_MAX_NODES)
        {
            hr = invalid_block_map();
            break;
        }
        parser.node_count++;
        if (FAILED(hr = inspect_node_strings( &parser ))) break;
        depth = xmlTextReaderDepth( parser.reader );
        if (depth < 0 || depth >= APPX_BLOCK_MAP_MAX_DEPTH)
        {
            hr = invalid_block_map();
            break;
        }

        encoding = xmlTextReaderConstEncoding( parser.reader );
        if (encoding && xmlStrcasecmp( encoding, BAD_CAST "UTF-8" ))
        {
            hr = invalid_block_map();
            break;
        }
        version = xmlTextReaderConstXmlVersion( parser.reader );
        if (version && xmlStrcmp( version, BAD_CAST "1.0" ))
        {
            hr = invalid_block_map();
            break;
        }

        type = xmlTextReaderNodeType( parser.reader );
        if (type == XML_READER_TYPE_ELEMENT)
            hr = parse_element( &parser );
        else if (type == XML_READER_TYPE_END_ELEMENT)
            hr = parse_end_element( &parser );
        else if (type == XML_READER_TYPE_XML_DECLARATION)
        {
            if (parser.declaration_seen || parser.root_seen)
                hr = invalid_block_map();
            else
                parser.declaration_seen = TRUE;
        }
        else if (type == XML_READER_TYPE_WHITESPACE ||
                 type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
                 type == XML_READER_TYPE_TEXT)
        {
            if (!is_xml_whitespace( xmlTextReaderConstValue( parser.reader ) ))
                hr = invalid_block_map();
        }
        else if (type == XML_READER_TYPE_COMMENT)
        {
            /* Comments carry no block-map data and remain subject to node/value limits. */
        }
        else
        {
            /* This also rejects DTD, entity, entity-reference, PI and CDATA. */
            hr = invalid_block_map();
        }
        if (FAILED(hr)) break;
    }
    if (SUCCEEDED(hr) && status < 0) hr = xml_failure();
    if (SUCCEEDED(hr) && (!parser.root_seen || !parser.root_ended ||
                          parser.inside_file || parser.inside_block ||
                          parser.inside_file_hash))
        hr = invalid_block_map();
    if (SUCCEEDED(hr)) hr = reject_duplicate_names( parser.map );

    xmlFreeTextReader( parser.reader );
    if (FAILED(hr))
    {
        appx_block_map_free( parser.map );
        return hr;
    }
    *result = parser.map;
    return S_OK;
}

void WINAPI appx_block_map_free( APPX_BLOCK_MAP *map )
{
    UINT32 i;

    if (!map) return;
    for (i = 0; i < map->file_count; i++)
        HeapFree( GetProcessHeap(), 0, (void *)map->files[i].public.name );
    if (map->blocks) HeapFree( GetProcessHeap(), 0, map->blocks );
    if (map->files) HeapFree( GetProcessHeap(), 0, map->files );
    HeapFree( GetProcessHeap(), 0, map );
}

UINT32 WINAPI appx_block_map_get_file_count( const APPX_BLOCK_MAP *map )
{
    return map ? map->file_count : 0;
}

UINT32 WINAPI appx_block_map_get_block_count( const APPX_BLOCK_MAP *map )
{
    return map ? map->block_count : 0;
}

const APPX_BLOCK_MAP_FILE *WINAPI appx_block_map_get_file( const APPX_BLOCK_MAP *map,
                                                          UINT32 index )
{
    if (!map || index >= map->file_count) return NULL;
    return &map->files[index].public;
}

const APPX_BLOCK_MAP_BLOCK *WINAPI appx_block_map_get_block( const APPX_BLOCK_MAP *map,
                                                            UINT32 file_index,
                                                            UINT32 block_index )
{
    const struct block_map_file *file;

    if (!map || file_index >= map->file_count) return NULL;
    file = &map->files[file_index];
    if (block_index >= file->public.block_count) return NULL;
    return &map->blocks[file->first_block + block_index];
}
