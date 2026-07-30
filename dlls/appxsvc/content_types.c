/*
 * AppX OPC content-types parser
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

#include <stdlib.h>
#include <string.h>

#include <libxml/xmlreader.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"

#include "wine/appxsvc.h"

#include "content_types.h"

static const xmlChar content_types_namespace[] =
    "http://schemas.openxmlformats.org/package/2006/content-types";
static const xmlChar xmlns_namespace[] = "http://www.w3.org/2000/xmlns/";
static const xmlChar xinclude_namespace[] = "http://www.w3.org/2001/XInclude";

#define XML_MAX_NAMESPACE_BYTES 1024

enum child_kind
{
    CHILD_NONE,
    CHILD_DEFAULT,
    CHILD_OVERRIDE
};

struct appx_content_types
{
    APPX_CONTENT_TYPE_DEFAULT *defaults;
    APPX_CONTENT_TYPE_OVERRIDE *overrides;
    UINT32 default_count;
    UINT32 default_capacity;
    UINT32 override_count;
    UINT32 override_capacity;
    enum appx_content_types_mode mode;
};

struct parser
{
    APPX_CONTENT_TYPES *types;
    xmlTextReaderPtr reader;
    UINT32 node_count;
    UINT32 element_count;
    UINT32 attribute_count;
    UINT32 namespace_count;
    enum child_kind child;
    BOOL declaration_seen;
    BOOL root_seen;
    BOOL root_ended;
};

static HRESULT invalid_content_types(void)
{
    return APPX_E_INVALID_PACKAGING_LAYOUT;
}

static BOOL xml_equal( const xmlChar *left, const xmlChar *right )
{
    if (!left) left = BAD_CAST "";
    if (!right) right = BAD_CAST "";
    return !xmlStrcmp( left, right );
}

static BOOL empty_namespace( const xmlChar *uri )
{
    return !uri || !*uri;
}

static BOOL is_namespace_declaration( const xmlChar *local, const xmlChar *prefix,
                                      const xmlChar *uri )
{
    return xml_equal( uri, xmlns_namespace ) ||
           xml_equal( prefix, BAD_CAST "xmlns" ) ||
           (empty_namespace( prefix ) && xml_equal( local, BAD_CAST "xmlns" ));
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

/*
 * Reject a DTD before constructing a reader.  Disabling DTD loading is not
 * enough because reader construction must not resolve a hostile parameter
 * entity.  Markup-looking text inside comments, CDATA, and instructions is not
 * a declaration; CDATA and non-declaration instructions are rejected later.
 */
static BOOL contains_doctype_declaration( const BYTE *document, SIZE_T size )
{
    static const char comment[] = "<!--";
    static const char cdata[] = "<![CDATA[";
    static const char instruction[] = "<?";
    static const char doctype[] = "<!DOCTYPE";
    SIZE_T offset = 0;

    while (offset < size)
    {
        if (document[offset] != '<')
        {
            offset++;
            continue;
        }
        if (size - offset >= sizeof(comment) - 1 &&
            !memcmp( document + offset, comment, sizeof(comment) - 1 ))
        {
            offset = skip_xml_section( document, size,
                                       offset + sizeof(comment) - 1, "-->" );
            continue;
        }
        if (size - offset >= sizeof(cdata) - 1 &&
            !memcmp( document + offset, cdata, sizeof(cdata) - 1 ))
        {
            offset = skip_xml_section( document, size,
                                       offset + sizeof(cdata) - 1, "]]>" );
            continue;
        }
        if (size - offset >= sizeof(instruction) - 1 &&
            !memcmp( document + offset, instruction, sizeof(instruction) - 1 ))
        {
            offset = skip_xml_section( document, size,
                                       offset + sizeof(instruction) - 1, "?>" );
            continue;
        }
        if (size - offset >= sizeof(doctype) - 1 &&
            !memcmp( document + offset, doctype, sizeof(doctype) - 1 ))
            return TRUE;
        offset++;
    }
    return FALSE;
}

static HRESULT inspect_string( const xmlChar *value, UINT32 maximum )
{
    int length;

    if (!value) return S_OK;
    length = xmlStrlen( value );
    if (length < 0 || (UINT32)length > maximum)
        return invalid_content_types();
    return S_OK;
}

static HRESULT inspect_current_node( struct parser *parser )
{
    const xmlChar *local, *prefix, *uri, *value;
    int attributes, depth, i, type;
    HRESULT hr;

    if (parser->node_count == APPX_CONTENT_TYPES_MAX_NODES)
        return invalid_content_types();
    parser->node_count++;

    depth = xmlTextReaderDepth( parser->reader );
    if (depth < 0 || depth >= APPX_CONTENT_TYPES_MAX_DEPTH)
        return invalid_content_types();

    local = xmlTextReaderConstLocalName( parser->reader );
    prefix = xmlTextReaderConstPrefix( parser->reader );
    uri = xmlTextReaderConstNamespaceUri( parser->reader );
    value = xmlTextReaderConstValue( parser->reader );
    if (FAILED(hr = inspect_string( local, APPX_CONTENT_TYPES_MAX_XML_NAME_BYTES )) ||
        FAILED(hr = inspect_string( prefix, APPX_CONTENT_TYPES_MAX_XML_NAME_BYTES )) ||
        FAILED(hr = inspect_string( uri, XML_MAX_NAMESPACE_BYTES )) ||
        FAILED(hr = inspect_string( value, APPX_CONTENT_TYPES_MAX_XML_VALUE_BYTES )))
        return hr;

    type = xmlTextReaderNodeType( parser->reader );
    if ((type == XML_READER_TYPE_ELEMENT || type == XML_READER_TYPE_END_ELEMENT) &&
        xml_equal( uri, xinclude_namespace ))
        return invalid_content_types();
    if (type != XML_READER_TYPE_ELEMENT) return S_OK;

    if (parser->element_count == APPX_CONTENT_TYPES_MAX_ENTRIES + 1)
        return invalid_content_types();
    parser->element_count++;

    attributes = xmlTextReaderAttributeCount( parser->reader );
    if (attributes < 0 || attributes > APPX_CONTENT_TYPES_MAX_ATTRIBUTES_PER_NODE ||
        parser->attribute_count > APPX_CONTENT_TYPES_MAX_ATTRIBUTES - attributes)
        return invalid_content_types();
    parser->attribute_count += attributes;

    for (i = 0; i < attributes; i++)
    {
        if (xmlTextReaderMoveToAttributeNo( parser->reader, i ) != 1)
            return invalid_content_types();
        local = xmlTextReaderConstLocalName( parser->reader );
        prefix = xmlTextReaderConstPrefix( parser->reader );
        uri = xmlTextReaderConstNamespaceUri( parser->reader );
        value = xmlTextReaderConstValue( parser->reader );
        if (FAILED(hr = inspect_string( local, APPX_CONTENT_TYPES_MAX_XML_NAME_BYTES )) ||
            FAILED(hr = inspect_string( prefix, APPX_CONTENT_TYPES_MAX_XML_NAME_BYTES )) ||
            FAILED(hr = inspect_string( uri, XML_MAX_NAMESPACE_BYTES )) ||
            FAILED(hr = inspect_string( value, APPX_CONTENT_TYPES_MAX_XML_VALUE_BYTES )))
        {
            xmlTextReaderMoveToElement( parser->reader );
            return hr;
        }
        if (xml_equal( uri, xinclude_namespace ))
        {
            xmlTextReaderMoveToElement( parser->reader );
            return invalid_content_types();
        }
        if (is_namespace_declaration( local, prefix, uri ))
        {
            if (parser->namespace_count == APPX_CONTENT_TYPES_MAX_NAMESPACES ||
                FAILED(inspect_string( value, XML_MAX_NAMESPACE_BYTES )) ||
                xml_equal( value, xinclude_namespace ))
            {
                xmlTextReaderMoveToElement( parser->reader );
                return invalid_content_types();
            }
            parser->namespace_count++;
        }
    }
    if (xmlTextReaderMoveToElement( parser->reader ) != 1)
        return invalid_content_types();
    return S_OK;
}

static BOOL is_extension_character( xmlChar ch )
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '~';
}

static BOOL is_media_token_character( xmlChar ch )
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '!' || ch == '#' || ch == '$' || ch == '%' ||
           ch == '&' || ch == '\'' || ch == '*' || ch == '+' ||
           ch == '-' || ch == '.' || ch == '^' || ch == '_' ||
           ch == '`' || ch == '|' || ch == '~';
}

static BOOL valid_extension( const xmlChar *value )
{
    UINT32 i, length;

    if (!value || !(length = xmlStrlen( value )) ||
        length > APPX_CONTENT_TYPES_MAX_EXTENSION_CHARS)
        return FALSE;
    for (i = 0; i < length; i++)
        if (!is_extension_character( value[i] )) return FALSE;
    return TRUE;
}

static BOOL valid_content_type( const xmlChar *value )
{
    UINT32 i, length, slash = MAXDWORD;

    if (!value || !(length = xmlStrlen( value )) ||
        length > APPX_CONTENT_TYPES_MAX_CONTENT_TYPE_CHARS)
        return FALSE;
    for (i = 0; i < length; i++)
    {
        if (value[i] == '/')
        {
            if (slash != MAXDWORD) return FALSE;
            slash = i;
        }
        else if (!is_media_token_character( value[i] ))
            return FALSE;
    }
    return slash && slash != MAXDWORD && slash + 1 < length;
}

static BOOL is_hexadecimal( BYTE ch )
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static BOOL is_uri_path_character( BYTE ch )
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '.' || ch == '_' || ch == '~' ||
           ch == '!' || ch == '$' || ch == '&' || ch == '\'' ||
           ch == '(' || ch == ')' || ch == '*' || ch == '+' ||
           ch == ',' || ch == ';' || ch == '=' || ch == ':' || ch == '@';
}

static HRESULT canonicalize_part_name( const xmlChar *value, WCHAR **result,
                                       UINT32 *result_length )
{
    WCHAR *path;
    UINT32 bytes, capacity = 0, i, required;
    HRESULT hr;

    *result = NULL;
    *result_length = 0;
    if (!value || !(bytes = xmlStrlen( value )) || bytes <= 1 ||
        bytes > APPX_CONTENT_TYPES_MAX_XML_VALUE_BYTES ||
        value[0] != '/' || value[bytes - 1] == '/')
        return invalid_content_types();

    for (i = 1; i < bytes; i++)
    {
        BYTE ch = value[i];

        if (ch == '/') continue;
        if (ch == '%')
        {
            if (bytes - i < 3 ||
                !is_hexadecimal( value[i + 1] ) ||
                !is_hexadecimal( value[i + 2] ))
                return invalid_content_types();
            i += 2;
            continue;
        }
        if (!is_uri_path_character( ch ))
            return invalid_content_types();
    }

    hr = wine_appx_validate_archive_path( value + 1, bytes - 1, 0,
                                          &capacity, NULL );
    if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
        return hr == E_OUTOFMEMORY ? hr : invalid_content_types();
    required = capacity;
    if (!required || required > WINE_APPX_MAX_PATH_CHARS)
        return invalid_content_types();
    if (!(path = HeapAlloc( GetProcessHeap(), 0,
                            (required + 1) * sizeof(*path) )))
        return E_OUTOFMEMORY;

    path[0] = '/';
    capacity = required;
    hr = wine_appx_validate_archive_path( value + 1, bytes - 1, 0,
                                          &capacity, path + 1 );
    if (FAILED(hr) || capacity != required)
    {
        HeapFree( GetProcessHeap(), 0, path );
        return hr == E_OUTOFMEMORY ? hr : invalid_content_types();
    }
    for (i = 1; i < required; i++)
        if (path[i] == '\\') path[i] = '/';
    path[required] = 0;
    *result = path;
    *result_length = required;
    return S_OK;
}

static HRESULT copy_ascii_string( const xmlChar *value, WCHAR **result,
                                  UINT32 *result_length )
{
    WCHAR *string;
    UINT32 i, length = xmlStrlen( value );

    if (!(string = HeapAlloc( GetProcessHeap(), 0,
                              (length + 1) * sizeof(*string) )))
        return E_OUTOFMEMORY;
    for (i = 0; i < length; i++) string[i] = value[i];
    string[length] = 0;
    *result = string;
    *result_length = length;
    return S_OK;
}

static HRESULT reserve_defaults( APPX_CONTENT_TYPES *types, UINT32 required )
{
    APPX_CONTENT_TYPE_DEFAULT *entries;
    UINT32 capacity;

    if (required > APPX_CONTENT_TYPES_MAX_ENTRIES ||
        types->override_count > APPX_CONTENT_TYPES_MAX_ENTRIES - required)
        return invalid_content_types();
    if (required <= types->default_capacity) return S_OK;

    capacity = types->default_capacity ? types->default_capacity : 16;
    while (capacity < required)
    {
        if (capacity > APPX_CONTENT_TYPES_MAX_ENTRIES / 2)
        {
            capacity = APPX_CONTENT_TYPES_MAX_ENTRIES;
            break;
        }
        capacity *= 2;
    }
    if (types->defaults)
        entries = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, types->defaults,
                               capacity * sizeof(*entries) );
    else
        entries = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                             capacity * sizeof(*entries) );
    if (!entries) return E_OUTOFMEMORY;
    types->defaults = entries;
    types->default_capacity = capacity;
    return S_OK;
}

static HRESULT reserve_overrides( APPX_CONTENT_TYPES *types, UINT32 required )
{
    APPX_CONTENT_TYPE_OVERRIDE *entries;
    UINT32 capacity;

    if (required > APPX_CONTENT_TYPES_MAX_ENTRIES ||
        types->default_count > APPX_CONTENT_TYPES_MAX_ENTRIES - required)
        return invalid_content_types();
    if (required <= types->override_capacity) return S_OK;

    capacity = types->override_capacity ? types->override_capacity : 16;
    while (capacity < required)
    {
        if (capacity > APPX_CONTENT_TYPES_MAX_ENTRIES / 2)
        {
            capacity = APPX_CONTENT_TYPES_MAX_ENTRIES;
            break;
        }
        capacity *= 2;
    }
    if (types->overrides)
        entries = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, types->overrides,
                               capacity * sizeof(*entries) );
    else
        entries = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                             capacity * sizeof(*entries) );
    if (!entries) return E_OUTOFMEMORY;
    types->overrides = entries;
    types->override_capacity = capacity;
    return S_OK;
}

static HRESULT add_default( APPX_CONTENT_TYPES *types, const xmlChar *extension,
                            const xmlChar *content_type )
{
    APPX_CONTENT_TYPE_DEFAULT *entry;
    HRESULT hr;

    if (!valid_extension( extension ) || !valid_content_type( content_type ))
        return invalid_content_types();
    if (FAILED(hr = reserve_defaults( types, types->default_count + 1 )))
        return hr;
    entry = types->defaults + types->default_count;
    if (FAILED(hr = copy_ascii_string( extension, (WCHAR **)&entry->extension,
                                       &entry->extension_length )))
        return hr;
    if (FAILED(hr = copy_ascii_string( content_type, (WCHAR **)&entry->content_type,
                                       &entry->content_type_length )))
    {
        HeapFree( GetProcessHeap(), 0, (void *)entry->extension );
        memset( entry, 0, sizeof(*entry) );
        return hr;
    }
    types->default_count++;
    return S_OK;
}

static HRESULT add_override( APPX_CONTENT_TYPES *types, const xmlChar *part_name,
                             const xmlChar *content_type )
{
    APPX_CONTENT_TYPE_OVERRIDE *entry;
    HRESULT hr;

    if (!valid_content_type( content_type ))
        return invalid_content_types();
    if (FAILED(hr = reserve_overrides( types, types->override_count + 1 )))
        return hr;
    entry = types->overrides + types->override_count;
    if (FAILED(hr = canonicalize_part_name( part_name, (WCHAR **)&entry->part_name,
                                            &entry->part_name_length )))
        return hr;
    if (FAILED(hr = copy_ascii_string( content_type, (WCHAR **)&entry->content_type,
                                       &entry->content_type_length )))
    {
        HeapFree( GetProcessHeap(), 0, (void *)entry->part_name );
        memset( entry, 0, sizeof(*entry) );
        return hr;
    }
    types->override_count++;
    return S_OK;
}

static HRESULT parse_root_attributes( struct parser *parser )
{
    int count = xmlTextReaderAttributeCount( parser->reader ), i;

    for (i = 0; i < count; i++)
    {
        const xmlChar *local, *prefix, *uri;

        if (xmlTextReaderMoveToAttributeNo( parser->reader, i ) != 1)
            return invalid_content_types();
        local = xmlTextReaderConstLocalName( parser->reader );
        prefix = xmlTextReaderConstPrefix( parser->reader );
        uri = xmlTextReaderConstNamespaceUri( parser->reader );
        if (!is_namespace_declaration( local, prefix, uri ))
        {
            xmlTextReaderMoveToElement( parser->reader );
            return invalid_content_types();
        }
    }
    if (xmlTextReaderMoveToElement( parser->reader ) != 1)
        return invalid_content_types();
    return S_OK;
}

static HRESULT parse_child_attributes( struct parser *parser, enum child_kind kind )
{
    const xmlChar *extension = NULL, *part_name = NULL, *content_type = NULL;
    BOOL has_extension = FALSE, has_part_name = FALSE, has_content_type = FALSE;
    int count = xmlTextReaderAttributeCount( parser->reader ), i;

    for (i = 0; i < count; i++)
    {
        const xmlChar *local, *prefix, *uri, *value;

        if (xmlTextReaderMoveToAttributeNo( parser->reader, i ) != 1)
            return invalid_content_types();
        local = xmlTextReaderConstLocalName( parser->reader );
        prefix = xmlTextReaderConstPrefix( parser->reader );
        uri = xmlTextReaderConstNamespaceUri( parser->reader );
        value = xmlTextReaderConstValue( parser->reader );
        if (is_namespace_declaration( local, prefix, uri )) continue;
        if (!empty_namespace( uri ))
        {
            xmlTextReaderMoveToElement( parser->reader );
            return invalid_content_types();
        }
        if (kind == CHILD_DEFAULT && xml_equal( local, BAD_CAST "Extension" ) &&
            !has_extension)
        {
            extension = value;
            has_extension = TRUE;
        }
        else if (kind == CHILD_OVERRIDE &&
                 xml_equal( local, BAD_CAST "PartName" ) && !has_part_name)
        {
            part_name = value;
            has_part_name = TRUE;
        }
        else if (xml_equal( local, BAD_CAST "ContentType" ) && !has_content_type)
        {
            content_type = value;
            has_content_type = TRUE;
        }
        else
        {
            xmlTextReaderMoveToElement( parser->reader );
            return invalid_content_types();
        }
    }
    if (xmlTextReaderMoveToElement( parser->reader ) != 1)
        return invalid_content_types();
    if (!has_content_type ||
        (kind == CHILD_DEFAULT && !has_extension) ||
        (kind == CHILD_OVERRIDE && !has_part_name))
        return invalid_content_types();

    if (kind == CHILD_DEFAULT)
        return add_default( parser->types, extension, content_type );
    return add_override( parser->types, part_name, content_type );
}

static HRESULT parse_element( struct parser *parser )
{
    const xmlChar *local = xmlTextReaderConstLocalName( parser->reader );
    const xmlChar *uri = xmlTextReaderConstNamespaceUri( parser->reader );
    int depth = xmlTextReaderDepth( parser->reader );
    BOOL empty = xmlTextReaderIsEmptyElement( parser->reader );
    enum child_kind kind;
    HRESULT hr;

    if (!parser->root_seen)
    {
        if (depth || parser->root_ended ||
            !xml_equal( local, BAD_CAST "Types" ) ||
            !xml_equal( uri, content_types_namespace ))
            return invalid_content_types();
        if (FAILED(hr = parse_root_attributes( parser ))) return hr;
        parser->root_seen = TRUE;
        if (empty) parser->root_ended = TRUE;
        return S_OK;
    }

    if (parser->root_ended || parser->child != CHILD_NONE || depth != 1 ||
        !xml_equal( uri, content_types_namespace ))
        return invalid_content_types();
    if (xml_equal( local, BAD_CAST "Default" ))
        kind = CHILD_DEFAULT;
    else if (xml_equal( local, BAD_CAST "Override" ))
        kind = CHILD_OVERRIDE;
    else
        return invalid_content_types();

    if (FAILED(hr = parse_child_attributes( parser, kind ))) return hr;
    if (!empty) parser->child = kind;
    return S_OK;
}

static HRESULT parse_end_element( struct parser *parser )
{
    const xmlChar *local = xmlTextReaderConstLocalName( parser->reader );
    const xmlChar *uri = xmlTextReaderConstNamespaceUri( parser->reader );
    int depth = xmlTextReaderDepth( parser->reader );

    if (!xml_equal( uri, content_types_namespace ))
        return invalid_content_types();
    if (depth == 1 && parser->child != CHILD_NONE)
    {
        if ((parser->child == CHILD_DEFAULT &&
             !xml_equal( local, BAD_CAST "Default" )) ||
            (parser->child == CHILD_OVERRIDE &&
             !xml_equal( local, BAD_CAST "Override" )))
            return invalid_content_types();
        parser->child = CHILD_NONE;
        return S_OK;
    }
    if (!depth && parser->root_seen && !parser->root_ended &&
        parser->child == CHILD_NONE && xml_equal( local, BAD_CAST "Types" ))
    {
        parser->root_ended = TRUE;
        return S_OK;
    }
    return invalid_content_types();
}

static int compare_wide_strings( const WCHAR *left, UINT32 left_length,
                                 const WCHAR *right, UINT32 right_length )
{
    int result = CompareStringOrdinal( left, left_length, right, right_length, TRUE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static int compare_defaults( const void *left, const void *right )
{
    const APPX_CONTENT_TYPE_DEFAULT *a = left, *b = right;

    return compare_wide_strings( a->extension, a->extension_length,
                                 b->extension, b->extension_length );
}

static int compare_overrides( const void *left, const void *right )
{
    const APPX_CONTENT_TYPE_OVERRIDE *a = left, *b = right;

    return compare_wide_strings( a->part_name, a->part_name_length,
                                 b->part_name, b->part_name_length );
}

static HRESULT sort_and_validate_unique_entries( APPX_CONTENT_TYPES *types )
{
    UINT32 i;

    if (types->default_count > 1)
        qsort( types->defaults, types->default_count, sizeof(*types->defaults),
               compare_defaults );
    for (i = 1; i < types->default_count; i++)
        if (!compare_defaults( types->defaults + i - 1, types->defaults + i ))
            return invalid_content_types();

    if (types->override_count > 1)
        qsort( types->overrides, types->override_count, sizeof(*types->overrides),
               compare_overrides );
    for (i = 1; i < types->override_count; i++)
        if (!compare_overrides( types->overrides + i - 1, types->overrides + i ))
            return invalid_content_types();
    return S_OK;
}

static const APPX_CONTENT_TYPE_OVERRIDE *find_override(
    const APPX_CONTENT_TYPES *types, const WCHAR *part_name, UINT32 length )
{
    UINT32 low = 0, high = types->override_count;

    while (low < high)
    {
        const APPX_CONTENT_TYPE_OVERRIDE *entry;
        UINT32 middle = low + (high - low) / 2;
        int comparison;

        entry = types->overrides + middle;
        comparison = compare_wide_strings( part_name, length, entry->part_name,
                                           entry->part_name_length );
        if (comparison < 0)
            high = middle;
        else if (comparison > 0)
            low = middle + 1;
        else
            return entry;
    }
    return NULL;
}

static const APPX_CONTENT_TYPE_DEFAULT *find_default(
    const APPX_CONTENT_TYPES *types, const WCHAR *extension, UINT32 length )
{
    UINT32 low = 0, high = types->default_count;

    while (low < high)
    {
        const APPX_CONTENT_TYPE_DEFAULT *entry;
        UINT32 middle = low + (high - low) / 2;
        int comparison;

        entry = types->defaults + middle;
        comparison = compare_wide_strings( extension, length, entry->extension,
                                           entry->extension_length );
        if (comparison < 0)
            high = middle;
        else if (comparison > 0)
            low = middle + 1;
        else
            return entry;
    }
    return NULL;
}

/*
 * Lookup accepts the decoded absolute form returned by this parser.  Escape a
 * literal percent before reusing the archive path validator so it cannot be
 * interpreted as a second round of OPC percent decoding.
 */
static BOOL valid_decoded_part_name( const WCHAR *part_name, UINT32 length )
{
    BYTE *utf8 = NULL, *escaped = NULL;
    UINT32 capacity = 0, escaped_size, i, j, percent_count = 0;
    int utf8_size;
    HRESULT hr;
    BOOL valid = FALSE;

    if (length < 2 || length >= WINE_APPX_MAX_PATH_CHARS)
        return FALSE;
    utf8_size = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS,
                                    part_name + 1, length - 1, NULL, 0,
                                    NULL, NULL );
    if (!utf8_size || utf8_size > WINE_APPX_MAX_ENTRY_NAME_BYTES)
        return FALSE;
    if (!(utf8 = HeapAlloc( GetProcessHeap(), 0, utf8_size )))
        return FALSE;
    if (WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, part_name + 1,
                             length - 1, (char *)utf8, utf8_size,
                             NULL, NULL ) != utf8_size)
        goto done;

    for (i = 0; i < utf8_size; i++)
        if (utf8[i] == '%') percent_count++;
    if (percent_count > (WINE_APPX_MAX_ENTRY_NAME_BYTES - utf8_size) / 2)
        goto done;
    escaped_size = utf8_size + percent_count * 2;
    if (!(escaped = HeapAlloc( GetProcessHeap(), 0, escaped_size )))
        goto done;
    for (i = j = 0; i < utf8_size; i++)
    {
        if (utf8[i] == '%')
        {
            escaped[j++] = '%';
            escaped[j++] = '2';
            escaped[j++] = '5';
        }
        else
            escaped[j++] = utf8[i];
    }

    hr = wine_appx_validate_archive_path( escaped, escaped_size, 0,
                                          &capacity, NULL );
    valid = hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) &&
            capacity == length;

done:
    HeapFree( GetProcessHeap(), 0, escaped );
    HeapFree( GetProcessHeap(), 0, utf8 );
    return valid;
}

const WCHAR *WINAPI appx_content_types_get_content_type(
    const APPX_CONTENT_TYPES *types, const WCHAR *part_name )
{
    const APPX_CONTENT_TYPE_OVERRIDE *override;
    const APPX_CONTENT_TYPE_DEFAULT *def;
    UINT32 component = 0, dot = MAXDWORD, i, length;

    if (!types || !part_name || part_name[0] != '/' ||
        !(length = lstrlenW( part_name )) || part_name[length - 1] == '/' ||
        !valid_decoded_part_name( part_name, length ))
        return NULL;
    for (i = 1; i < length; i++)
    {
        if (part_name[i] == '\\') return NULL;
        if (part_name[i] == '/')
        {
            if (i == component + 1) return NULL;
            component = i;
            dot = MAXDWORD;
        }
        else if (part_name[i] == '.')
            dot = i;
    }

    if ((override = find_override( types, part_name, length )))
        return override->content_type;
    if (dot == MAXDWORD || dot + 1 == length) return NULL;
    if (!(def = find_default( types, part_name + dot + 1, length - dot - 1 )))
        return NULL;
    return def->content_type;
}

static BOOL has_exact_mapping( const APPX_CONTENT_TYPES *types,
                               const WCHAR *part_name, const WCHAR *expected )
{
    const WCHAR *actual = appx_content_types_get_content_type( types, part_name );

    return actual && !lstrcmpW( actual, expected );
}

static HRESULT validate_required_mappings( const APPX_CONTENT_TYPES *types )
{
    if (types->mode == APPX_CONTENT_TYPES_MODE_PACKAGE)
    {
        if (!has_exact_mapping( types, L"/AppxManifest.xml",
                                APPX_CONTENT_TYPE_MANIFEST ))
            return invalid_content_types();
    }
    else
    {
        if (!has_exact_mapping( types, L"/AppxMetadata/AppxBundleManifest.xml",
                                APPX_CONTENT_TYPE_BUNDLE_MANIFEST ))
            return invalid_content_types();
    }
    if (!has_exact_mapping( types, L"/AppxBlockMap.xml",
                            APPX_CONTENT_TYPE_BLOCK_MAP ) ||
        !has_exact_mapping( types, L"/AppxSignature.p7x",
                            APPX_CONTENT_TYPE_SIGNATURE ))
        return invalid_content_types();
    return S_OK;
}

static void xml_reader_error( void *argument, const char *message,
                              xmlParserSeverities severity,
                              xmlTextReaderLocatorPtr locator )
{
    /* Parser diagnostics can contain hostile document contents. */
    (void)argument;
    (void)message;
    (void)severity;
    (void)locator;
}

static HRESULT xml_failure(void)
{
    const xmlError *error = xmlGetLastError();

    if (error && error->code == XML_ERR_NO_MEMORY) return E_OUTOFMEMORY;
    return invalid_content_types();
}

void WINAPI appx_content_types_free( APPX_CONTENT_TYPES *types )
{
    UINT32 i;

    if (!types) return;
    for (i = 0; i < types->default_count; i++)
    {
        HeapFree( GetProcessHeap(), 0, (void *)types->defaults[i].extension );
        HeapFree( GetProcessHeap(), 0, (void *)types->defaults[i].content_type );
    }
    for (i = 0; i < types->override_count; i++)
    {
        HeapFree( GetProcessHeap(), 0, (void *)types->overrides[i].part_name );
        HeapFree( GetProcessHeap(), 0, (void *)types->overrides[i].content_type );
    }
    HeapFree( GetProcessHeap(), 0, types->defaults );
    HeapFree( GetProcessHeap(), 0, types->overrides );
    HeapFree( GetProcessHeap(), 0, types );
}

HRESULT WINAPI appx_content_types_parse( const BYTE *document, UINT32 size,
                                         enum appx_content_types_mode mode,
                                         APPX_CONTENT_TYPES **result )
{
    struct parser parser = {0};
    const xmlChar *encoding, *version;
    APPX_CONTENT_TYPES *types;
    HRESULT hr = S_OK;
    UINT32 i;
    int status;

    if (!result) return E_INVALIDARG;
    *result = NULL;
    if (!document || !size) return E_INVALIDARG;
    if (mode != APPX_CONTENT_TYPES_MODE_PACKAGE &&
        mode != APPX_CONTENT_TYPES_MODE_BUNDLE)
        return E_INVALIDARG;
    if (size > APPX_CONTENT_TYPES_MAX_DOCUMENT_SIZE)
        return invalid_content_types();
    for (i = 0; i < size; i++)
        if (!document[i]) return invalid_content_types();
    if (!MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS,
                              (const char *)document, size, NULL, 0 ))
        return invalid_content_types();
    if (contains_doctype_declaration( document, size ))
        return invalid_content_types();

    if (!(types = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*types) )))
        return E_OUTOFMEMORY;
    types->mode = mode;
    parser.types = types;

    xmlResetLastError();
    if (!(parser.reader = xmlReaderForMemory( (const char *)document, size,
                                              NULL, NULL, XML_PARSE_NONET )))
    {
        appx_content_types_free( types );
        return xml_failure();
    }
    xmlTextReaderSetErrorHandler( parser.reader, xml_reader_error, NULL );
    if (xmlTextReaderSetParserProp( parser.reader, XML_PARSER_LOADDTD, 0 ) < 0 ||
        xmlTextReaderSetParserProp( parser.reader, XML_PARSER_DEFAULTATTRS, 0 ) < 0 ||
        xmlTextReaderSetParserProp( parser.reader, XML_PARSER_VALIDATE, 0 ) < 0 ||
        xmlTextReaderSetParserProp( parser.reader, XML_PARSER_SUBST_ENTITIES, 0 ) < 0)
    {
        xmlFreeTextReader( parser.reader );
        appx_content_types_free( types );
        return invalid_content_types();
    }

    while ((status = xmlTextReaderRead( parser.reader )) == 1)
    {
        int type;

        if (FAILED(hr = inspect_current_node( &parser ))) break;
        encoding = xmlTextReaderConstEncoding( parser.reader );
        version = xmlTextReaderConstXmlVersion( parser.reader );
        if ((encoding && xmlStrcasecmp( encoding, BAD_CAST "UTF-8" )) ||
            (version && xmlStrcmp( version, BAD_CAST "1.0" )))
        {
            hr = invalid_content_types();
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
                hr = invalid_content_types();
            else
                parser.declaration_seen = TRUE;
        }
        else if (type == XML_READER_TYPE_TEXT ||
                 type == XML_READER_TYPE_WHITESPACE ||
                 type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE)
        {
            if (!is_xml_whitespace( xmlTextReaderConstValue( parser.reader ) ))
                hr = invalid_content_types();
        }
        else if (type != XML_READER_TYPE_COMMENT &&
                 type != XML_READER_TYPE_DOCUMENT)
            hr = invalid_content_types();
        if (FAILED(hr)) break;
    }

    if (SUCCEEDED(hr) && status < 0) hr = xml_failure();
    if (SUCCEEDED(hr) &&
        (!parser.root_seen || !parser.root_ended || parser.child != CHILD_NONE))
        hr = invalid_content_types();
    xmlFreeTextReader( parser.reader );

    if (SUCCEEDED(hr)) hr = sort_and_validate_unique_entries( types );
    if (SUCCEEDED(hr)) hr = validate_required_mappings( types );
    if (FAILED(hr))
    {
        appx_content_types_free( types );
        return hr;
    }
    *result = types;
    return S_OK;
}

enum appx_content_types_mode WINAPI appx_content_types_get_mode(
    const APPX_CONTENT_TYPES *types )
{
    return types ? types->mode : 0;
}

UINT32 WINAPI appx_content_types_get_default_count( const APPX_CONTENT_TYPES *types )
{
    return types ? types->default_count : 0;
}

const APPX_CONTENT_TYPE_DEFAULT *WINAPI appx_content_types_get_default(
    const APPX_CONTENT_TYPES *types, UINT32 index )
{
    if (!types || index >= types->default_count) return NULL;
    return types->defaults + index;
}

UINT32 WINAPI appx_content_types_get_override_count( const APPX_CONTENT_TYPES *types )
{
    return types ? types->override_count : 0;
}

const APPX_CONTENT_TYPE_OVERRIDE *WINAPI appx_content_types_get_override(
    const APPX_CONTENT_TYPES *types, UINT32 index )
{
    if (!types || index >= types->override_count) return NULL;
    return types->overrides + index;
}
