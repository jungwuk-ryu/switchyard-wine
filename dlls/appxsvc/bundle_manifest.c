/*
 * AppX bundle manifest parser
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

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"

#include <libxml/xmlreader.h>

#include "wine/appxsvc.h"

#include "bundle_manifest.h"

#define XML_MAX_DEPTH                    32
#define XML_MAX_NODES                    131072
#define XML_MAX_ATTRIBUTES_PER_ELEMENT   64
#define XML_MAX_ATTRIBUTES               262144
#define XML_MAX_NAMESPACE_DECLARATIONS   128
#define XML_MAX_IGNORABLE_NAMESPACES     32
#define XML_MAX_LOCAL_NAME_BYTES         256
#define XML_MAX_PREFIX_BYTES             128
#define XML_MAX_NAMESPACE_BYTES          1024
#define XML_MAX_VALUE_BYTES              65536
#define MAX_RESOURCES_PER_PACKAGE        200

static const xmlChar bundle_namespace[] =
    "http://schemas.microsoft.com/appx/2013/bundle";
static const xmlChar xmlns_namespace[] = "http://www.w3.org/2000/xmlns/";
static const xmlChar xinclude_namespace[] = "http://www.w3.org/2001/XInclude";

struct bundle_package
{
    struct appx_bundle_package public;
    struct appx_bundle_resource *resources;
    UINT32 resource_capacity;
};

struct appx_bundle_manifest
{
    struct appx_bundle_identity identity;
    struct bundle_package *packages;
    UINT32 package_count;
    UINT32 package_capacity;
    UINT32 resource_count;
    BOOL unsupported_extensions;
};

struct ignorable_namespace
{
    xmlChar *prefix;
    xmlChar *uri;
};

struct parser
{
    xmlTextReaderPtr reader;
    APPX_BUNDLE_MANIFEST *manifest;
    struct ignorable_namespace ignorable[XML_MAX_IGNORABLE_NAMESPACES];
    UINT32 ignorable_count;
    UINT32 node_count;
    UINT32 attribute_count;
    UINT32 namespace_count;
    HRESULT hr;
};

struct xml_attribute
{
    xmlChar *local;
    xmlChar *uri;
    xmlChar *prefix;
    xmlChar *value;
    BOOL namespace_declaration;
    BOOL used;
};

struct xml_attributes
{
    struct xml_attribute item[XML_MAX_ATTRIBUTES_PER_ELEMENT];
    UINT32 count;
};

struct package_pointer
{
    const struct bundle_package *package;
};

static const xmlChar *empty_xml_string( const xmlChar *string )
{
    return string ? string : BAD_CAST "";
}

static BOOL xml_equal( const xmlChar *left, const xmlChar *right )
{
    return xmlStrEqual( empty_xml_string( left ), empty_xml_string( right ) );
}

static BOOL node_is( struct parser *parser, const xmlChar *uri, const char *local )
{
    return xml_equal( xmlTextReaderConstNamespaceUri( parser->reader ), uri ) &&
           xml_equal( xmlTextReaderConstLocalName( parser->reader ), BAD_CAST local );
}

static HRESULT parser_fail( struct parser *parser, HRESULT hr )
{
    if (SUCCEEDED(parser->hr)) parser->hr = hr;
    return parser->hr;
}

static void silent_xml_error( void *arg, const char *message,
                              xmlParserSeverities severity,
                              xmlTextReaderLocatorPtr locator )
{
    /* Parser diagnostics can contain hostile document contents. */
    (void)arg;
    (void)message;
    (void)severity;
    (void)locator;
}

static BOOL xml_string_within( const xmlChar *string, UINT32 limit )
{
    int length;

    if (!string) return TRUE;
    length = xmlStrlen( string );
    return length >= 0 && (UINT32)length <= limit;
}

static BOOL forbidden_node_type( int type )
{
    return type == XML_READER_TYPE_CDATA ||
           type == XML_READER_TYPE_ENTITY_REFERENCE ||
           type == XML_READER_TYPE_ENTITY ||
           type == XML_READER_TYPE_PROCESSING_INSTRUCTION ||
           type == XML_READER_TYPE_DOCUMENT_TYPE ||
           type == XML_READER_TYPE_NOTATION ||
           type == XML_READER_TYPE_END_ENTITY;
}

/*
 * This is the only reader-advancing function.  Ignored extension subtrees are
 * therefore subject to exactly the same resource bounds as recognized data.
 */
static int read_node( struct parser *parser )
{
    int attributes, depth, i, result, type;

    if (FAILED(parser->hr)) return -1;
    result = xmlTextReaderRead( parser->reader );
    if (result <= 0)
    {
        if (result < 0) parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return result;
    }

    if (++parser->node_count > XML_MAX_NODES)
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return -1;
    }

    type = xmlTextReaderNodeType( parser->reader );
    if (forbidden_node_type( type ))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return -1;
    }
    depth = xmlTextReaderDepth( parser->reader );
    if (depth < 0 || depth >= XML_MAX_DEPTH)
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return -1;
    }

    if (type == XML_READER_TYPE_ELEMENT || type == XML_READER_TYPE_END_ELEMENT)
    {
        if (!xml_string_within( xmlTextReaderConstLocalName( parser->reader ),
                                XML_MAX_LOCAL_NAME_BYTES ) ||
            !xml_string_within( xmlTextReaderConstPrefix( parser->reader ),
                                XML_MAX_PREFIX_BYTES ) ||
            !xml_string_within( xmlTextReaderConstNamespaceUri( parser->reader ),
                                XML_MAX_NAMESPACE_BYTES ))
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            return -1;
        }
        if (type == XML_READER_TYPE_ELEMENT &&
            xml_equal( xmlTextReaderConstNamespaceUri( parser->reader ),
                       xinclude_namespace ))
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            return -1;
        }
    }

    if (type != XML_READER_TYPE_ELEMENT)
    {
        if (!xml_string_within( xmlTextReaderConstValue( parser->reader ),
                                XML_MAX_VALUE_BYTES ))
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            return -1;
        }
        return 1;
    }

    attributes = xmlTextReaderAttributeCount( parser->reader );
    if (attributes < 0 || attributes > XML_MAX_ATTRIBUTES_PER_ELEMENT ||
        parser->attribute_count > XML_MAX_ATTRIBUTES - attributes)
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return -1;
    }
    parser->attribute_count += attributes;

    for (i = 0; i < attributes; i++)
    {
        const xmlChar *prefix, *uri;

        if (xmlTextReaderMoveToAttributeNo( parser->reader, i ) != 1)
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            break;
        }
        prefix = xmlTextReaderConstPrefix( parser->reader );
        uri = xmlTextReaderConstNamespaceUri( parser->reader );
        if (!xml_string_within( xmlTextReaderConstLocalName( parser->reader ),
                                XML_MAX_LOCAL_NAME_BYTES ) ||
            !xml_string_within( prefix, XML_MAX_PREFIX_BYTES ) ||
            !xml_string_within( uri, XML_MAX_NAMESPACE_BYTES ) ||
            !xml_string_within( xmlTextReaderConstValue( parser->reader ),
                                XML_MAX_VALUE_BYTES ))
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            break;
        }
        if (xml_equal( uri, xmlns_namespace ) ||
            xml_equal( prefix, BAD_CAST "xmlns" ) ||
            (!prefix && xml_equal( xmlTextReaderConstLocalName( parser->reader ),
                                   BAD_CAST "xmlns" )))
        {
            if (++parser->namespace_count > XML_MAX_NAMESPACE_DECLARATIONS)
            {
                parser_fail( parser, APPX_E_INVALID_MANIFEST );
                break;
            }
        }
    }
    xmlTextReaderMoveToElement( parser->reader );
    return FAILED(parser->hr) ? -1 : 1;
}

static BOOL text_is_whitespace( const xmlChar *text )
{
    if (!text) return TRUE;
    while (*text)
    {
        if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n')
            return FALSE;
        text++;
    }
    return TRUE;
}

static BOOL current_node_is_ignorable_text( struct parser *parser )
{
    int type = xmlTextReaderNodeType( parser->reader );

    if (type == XML_READER_TYPE_COMMENT ||
        type == XML_READER_TYPE_DOCUMENT ||
        type == XML_READER_TYPE_XML_DECLARATION)
        return TRUE;
    return (type == XML_READER_TYPE_TEXT ||
            type == XML_READER_TYPE_WHITESPACE ||
            type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE) &&
           text_is_whitespace( xmlTextReaderConstValue( parser->reader ) );
}

static void free_attributes( struct xml_attributes *attributes )
{
    UINT32 i;

    for (i = 0; i < attributes->count; i++)
    {
        xmlFree( attributes->item[i].local );
        xmlFree( attributes->item[i].uri );
        xmlFree( attributes->item[i].prefix );
        xmlFree( attributes->item[i].value );
    }
    memset( attributes, 0, sizeof(*attributes) );
}

static HRESULT get_attributes( struct parser *parser, struct xml_attributes *attributes )
{
    int count, i;

    memset( attributes, 0, sizeof(*attributes) );
    count = xmlTextReaderAttributeCount( parser->reader );
    if (count < 0 || count > XML_MAX_ATTRIBUTES_PER_ELEMENT)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    for (i = 0; i < count; i++)
    {
        struct xml_attribute *attribute = attributes->item + attributes->count;
        const xmlChar *local, *prefix, *uri, *value;
        UINT32 j;

        if (xmlTextReaderMoveToAttributeNo( parser->reader, i ) != 1)
            goto invalid;
        local = empty_xml_string( xmlTextReaderConstLocalName( parser->reader ) );
        prefix = empty_xml_string( xmlTextReaderConstPrefix( parser->reader ) );
        uri = empty_xml_string( xmlTextReaderConstNamespaceUri( parser->reader ) );
        value = empty_xml_string( xmlTextReaderConstValue( parser->reader ) );

        for (j = 0; j < attributes->count; j++)
            if (xml_equal( attributes->item[j].local, local ) &&
                xml_equal( attributes->item[j].uri, uri ))
                goto invalid;

        if (!(attribute->local = xmlStrdup( local )) ||
            !(attribute->prefix = xmlStrdup( prefix )) ||
            !(attribute->uri = xmlStrdup( uri )) ||
            !(attribute->value = xmlStrdup( value )))
        {
            xmlFree( attribute->local );
            xmlFree( attribute->prefix );
            xmlFree( attribute->uri );
            xmlFree( attribute->value );
            memset( attribute, 0, sizeof(*attribute) );
            parser_fail( parser, E_OUTOFMEMORY );
            goto done;
        }
        attribute->namespace_declaration =
            xml_equal( uri, xmlns_namespace ) ||
            xml_equal( prefix, BAD_CAST "xmlns" ) ||
            (!*prefix && xml_equal( local, BAD_CAST "xmlns" ));
        attribute->used = attribute->namespace_declaration;
        attributes->count++;
    }
    xmlTextReaderMoveToElement( parser->reader );
    return S_OK;

invalid:
    parser_fail( parser, APPX_E_INVALID_MANIFEST );
done:
    xmlTextReaderMoveToElement( parser->reader );
    free_attributes( attributes );
    return parser->hr;
}

static struct xml_attribute *take_attribute( struct xml_attributes *attributes,
                                             const char *local )
{
    UINT32 i;

    for (i = 0; i < attributes->count; i++)
    {
        struct xml_attribute *attribute = attributes->item + i;

        if (!attribute->used && !*attribute->uri &&
            xml_equal( attribute->local, BAD_CAST local ))
        {
            attribute->used = TRUE;
            return attribute;
        }
    }
    return NULL;
}

static BOOL uri_is_ignorable( const struct parser *parser,
                              const xmlChar *uri );

static struct xml_attribute *take_ignorable_attribute(
    const struct parser *parser, struct xml_attributes *attributes,
    const char *local )
{
    UINT32 i;

    for (i = 0; i < attributes->count; i++)
    {
        struct xml_attribute *attribute = attributes->item + i;

        if (!attribute->used &&
            xml_equal( attribute->local, BAD_CAST local ) &&
            uri_is_ignorable( parser, attribute->uri ))
        {
            attribute->used = TRUE;
            return attribute;
        }
    }
    return NULL;
}

static HRESULT require_attribute( struct parser *parser,
                                  struct xml_attributes *attributes,
                                  const char *local,
                                  struct xml_attribute **attribute )
{
    if (!(*attribute = take_attribute( attributes, local )))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return S_OK;
}

static BOOL uri_is_ignorable( const struct parser *parser, const xmlChar *uri )
{
    UINT32 i;

    if (!uri || !*uri) return FALSE;
    for (i = 0; i < parser->ignorable_count; i++)
        if (xml_equal( parser->ignorable[i].uri, uri )) return TRUE;
    return FALSE;
}

static HRESULT finish_attributes( struct parser *parser,
                                  struct xml_attributes *attributes,
                                  UINT32 *package_flags )
{
    UINT32 i;

    for (i = 0; i < attributes->count; i++)
    {
        struct xml_attribute *attribute = attributes->item + i;

        if (attribute->used) continue;
        if (!uri_is_ignorable( parser, attribute->uri ))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        parser->manifest->unsupported_extensions = TRUE;
        if (package_flags)
            *package_flags |= APPX_BUNDLE_PACKAGE_UNSUPPORTED_EXTENSION;
        attribute->used = TRUE;
    }
    return S_OK;
}

static HRESULT parse_ignorable_namespaces( struct parser *parser,
                                           const xmlChar *value )
{
    const xmlChar *cursor = value;

    while (cursor && *cursor)
    {
        const xmlChar *start;
        xmlChar *prefix, *uri;
        UINT32 length, i;

        while (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n')
            cursor++;
        if (!*cursor) break;
        start = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\r' && *cursor != '\n')
            cursor++;
        length = cursor - start;
        if (!length || length > XML_MAX_PREFIX_BYTES ||
            parser->ignorable_count >= XML_MAX_IGNORABLE_NAMESPACES)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        for (i = 0; i < length; i++)
            if (!(start[i] == '_' || start[i] == '-' || start[i] == '.' ||
                  (start[i] >= '0' && start[i] <= '9') ||
                  (start[i] >= 'A' && start[i] <= 'Z') ||
                  (start[i] >= 'a' && start[i] <= 'z')))
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );

        if (!(prefix = xmlStrndup( start, length )))
            return parser_fail( parser, E_OUTOFMEMORY );
        if (!(uri = xmlTextReaderLookupNamespace( parser->reader, prefix )))
        {
            xmlFree( prefix );
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        }
        if (!*uri || xml_equal( uri, bundle_namespace ) ||
            xml_equal( uri, xmlns_namespace ) ||
            xml_equal( uri, xinclude_namespace ))
        {
            xmlFree( prefix );
            xmlFree( uri );
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        }
        for (i = 0; i < parser->ignorable_count; i++)
        {
            if (xml_equal( parser->ignorable[i].prefix, prefix ) ||
                xml_equal( parser->ignorable[i].uri, uri ))
            {
                xmlFree( prefix );
                xmlFree( uri );
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
            }
        }
        parser->ignorable[parser->ignorable_count].prefix = prefix;
        parser->ignorable[parser->ignorable_count].uri = uri;
        parser->ignorable_count++;
    }
    return S_OK;
}

static HRESULT skip_ignorable_element( struct parser *parser, UINT32 *package_flags )
{
    int start_depth;

    if (!uri_is_ignorable( parser,
                           xmlTextReaderConstNamespaceUri( parser->reader ) ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    parser->manifest->unsupported_extensions = TRUE;
    if (package_flags)
        *package_flags |= APPX_BUNDLE_PACKAGE_UNSUPPORTED_EXTENSION;
    if (xmlTextReaderIsEmptyElement( parser->reader )) return S_OK;

    start_depth = xmlTextReaderDepth( parser->reader );
    while (read_node( parser ) > 0)
        if (xmlTextReaderNodeType( parser->reader ) == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == start_depth)
            return S_OK;
    return parser_fail( parser, APPX_E_INVALID_MANIFEST );
}

static WCHAR *duplicate_wstring( const WCHAR *source )
{
    SIZE_T bytes = (lstrlenW( source ) + 1) * sizeof(*source);
    WCHAR *result;

    if ((result = HeapAlloc( GetProcessHeap(), 0, bytes )))
        memcpy( result, source, bytes );
    return result;
}

static HRESULT xml_to_wstring( struct parser *parser, const xmlChar *value,
                               UINT32 minimum, UINT32 maximum, WCHAR **result )
{
    WCHAR *string;
    int bytes, count;

    *result = NULL;
    if (!value) return minimum ? parser_fail( parser, APPX_E_INVALID_MANIFEST ) : S_OK;
    bytes = xmlStrlen( value );
    if (bytes < 0 || (UINT32)bytes > XML_MAX_VALUE_BYTES)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS,
                                 (const char *)value, bytes, NULL, 0 );
    if ((!count && bytes) || count < 0 || (UINT32)count < minimum ||
        (UINT32)count > maximum)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (!(string = HeapAlloc( GetProcessHeap(), 0,
                              (count + 1) * sizeof(*string) )))
        return parser_fail( parser, E_OUTOFMEMORY );
    if (count && MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS,
                                      (const char *)value, bytes,
                                      string, count ) != count)
    {
        HeapFree( GetProcessHeap(), 0, string );
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    string[count] = 0;
    *result = string;
    return S_OK;
}

static HRESULT parse_version( struct parser *parser, const xmlChar *value,
                              struct appx_bundle_version *version )
{
    const xmlChar *cursor = value;
    UINT16 parts[4];
    UINT32 i;

    memset( version, 0, sizeof(*version) );
    if (!value || !*value) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    for (i = 0; i < 4; i++)
    {
        UINT32 digits = 0, number = 0;

        while (*cursor >= '0' && *cursor <= '9')
        {
            number = number * 10 + (*cursor++ - '0');
            if (++digits > 5 || number > 0xffff)
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        }
        if (!digits || (i < 3 ? *cursor != '.' : *cursor != 0))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        parts[i] = number;
        if (i < 3) cursor++;
    }
    version->major = parts[0];
    version->minor = parts[1];
    version->build = parts[2];
    version->revision = parts[3];
    return S_OK;
}

static HRESULT parse_uint64( struct parser *parser, const xmlChar *value,
                             UINT64 *result )
{
    UINT64 number = 0;
    UINT32 digits = 0;

    if (!value || !*value) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    while (*value >= '0' && *value <= '9')
    {
        UINT32 digit = *value++ - '0';

        if (number > (~(UINT64)0 - digit) / 10)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        number = number * 10 + digit;
        if (++digits > 20)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    if (*value || !digits)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    *result = number;
    return S_OK;
}

static HRESULT parse_uint32( struct parser *parser, const xmlChar *value,
                             UINT32 minimum, UINT32 maximum, UINT32 *result )
{
    UINT64 number;
    HRESULT hr;

    if (FAILED(hr = parse_uint64( parser, value, &number ))) return hr;
    if (number < minimum || number > maximum)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    *result = number;
    return S_OK;
}

static HRESULT parse_boolean( struct parser *parser, const xmlChar *value,
                              BOOL *result )
{
    if (xml_equal( value, BAD_CAST "true" ) ||
        xml_equal( value, BAD_CAST "1" ))
        *result = TRUE;
    else if (xml_equal( value, BAD_CAST "false" ) ||
             xml_equal( value, BAD_CAST "0" ))
        *result = FALSE;
    else
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return S_OK;
}

static BOOL validate_identity_name( const WCHAR *name )
{
    UINT32 i, length = lstrlenW( name );

    if (length < 3 || length > 50 || name[0] == '.' ||
        name[length - 1] == '.')
        return FALSE;
    for (i = 0; i < length; i++)
        if (!((name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') ||
              name[i] == '.' || name[i] == '-'))
            return FALSE;
    return TRUE;
}

static BOOL validate_normalized_string( const WCHAR *string, BOOL allow_empty )
{
    UINT32 i, length = lstrlenW( string );

    if (!length) return allow_empty;
    if (!IsNormalizedString( NormalizationC, string, length )) return FALSE;
    for (i = 0; i < length; i++)
        if (string[i] < 0x20 || string[i] == 0x7f ||
            string[i] == 0xfffe || string[i] == 0xffff)
            return FALSE;
    return TRUE;
}

static BOOL validate_language( const WCHAR *language )
{
    UINT32 component_length = 0, i, length;

    if (!language || !(length = lstrlenW( language )) || length > 85)
        return FALSE;
    for (i = 0; i < length; i++)
    {
        WCHAR ch = language[i];

        if (ch == '-')
        {
            if (!component_length || component_length > 8) return FALSE;
            component_length = 0;
        }
        else
        {
            if (!((ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9')))
                return FALSE;
            component_length++;
        }
    }
    return component_length && component_length <= 8;
}

static BOOL validate_simple_ascii_token( const WCHAR *token, UINT32 maximum )
{
    UINT32 i, length;

    if (!token || !(length = lstrlenW( token )) || length > maximum)
        return FALSE;
    for (i = 0; i < length; i++)
        if (!((token[i] >= 'A' && token[i] <= 'Z') ||
              (token[i] >= 'a' && token[i] <= 'z') ||
              (token[i] >= '0' && token[i] <= '9') ||
              token[i] == '.' || token[i] == '-' || token[i] == '_'))
            return FALSE;
    return TRUE;
}

static HRESULT parse_file_name( struct parser *parser, const xmlChar *value,
                                WCHAR **file_name )
{
    UINT32 capacity, length = 0;
    HRESULT hr;
    WCHAR *path;
    int bytes;

    *file_name = NULL;
    if (!value || (bytes = xmlStrlen( value )) <= 0 ||
        bytes > WINE_APPX_MAX_ENTRY_NAME_BYTES)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    hr = wine_appx_validate_archive_path( value, bytes, 0, &length, NULL );
    if (hr != HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER ) ||
        !length || length > WINE_APPX_MAX_PATH_CHARS)
        return parser_fail( parser, hr == E_OUTOFMEMORY ? hr :
                            APPX_E_INVALID_MANIFEST );
    capacity = length;
    if (!(path = HeapAlloc( GetProcessHeap(), 0, capacity * sizeof(*path) )))
        return parser_fail( parser, E_OUTOFMEMORY );
    hr = wine_appx_validate_archive_path( value, bytes, 0,
                                          &capacity, path );
    if (FAILED(hr) || capacity != length)
    {
        HeapFree( GetProcessHeap(), 0, path );
        return parser_fail( parser, hr == E_OUTOFMEMORY ? hr :
                            APPX_E_INVALID_MANIFEST );
    }
    *file_name = path;
    return S_OK;
}

static enum appx_bundle_package_type parse_package_type( const WCHAR *name )
{
    if (!lstrcmpW( name, L"application" ))
        return APPX_BUNDLE_PACKAGE_APPLICATION;
    if (!lstrcmpW( name, L"framework" ))
        return APPX_BUNDLE_PACKAGE_FRAMEWORK;
    if (!lstrcmpW( name, L"resource" ))
        return APPX_BUNDLE_PACKAGE_RESOURCE;
    if (!lstrcmpW( name, L"optional" ))
        return APPX_BUNDLE_PACKAGE_OPTIONAL;
    return APPX_BUNDLE_PACKAGE_UNSUPPORTED;
}

static enum appx_bundle_architecture parse_architecture_name( const WCHAR *name )
{
    if (!lstrcmpW( name, L"neutral" ))
        return APPX_BUNDLE_ARCHITECTURE_NEUTRAL;
    if (!lstrcmpW( name, L"x86" ))
        return APPX_BUNDLE_ARCHITECTURE_X86;
    if (!lstrcmpW( name, L"x64" ))
        return APPX_BUNDLE_ARCHITECTURE_X64;
    if (!lstrcmpW( name, L"arm" ))
        return APPX_BUNDLE_ARCHITECTURE_ARM;
    if (!lstrcmpW( name, L"arm64" ))
        return APPX_BUNDLE_ARCHITECTURE_ARM64;
    if (!lstrcmpW( name, L"x86a64" ))
        return APPX_BUNDLE_ARCHITECTURE_X86A64;
    return APPX_BUNDLE_ARCHITECTURE_UNSUPPORTED;
}

static void free_resource( struct appx_bundle_resource *resource )
{
    HeapFree( GetProcessHeap(), 0, (WCHAR *)resource->language );
    HeapFree( GetProcessHeap(), 0, (WCHAR *)resource->dx_feature_level );
    memset( resource, 0, sizeof(*resource) );
}

static void free_package( struct bundle_package *package )
{
    UINT32 i;

    HeapFree( GetProcessHeap(), 0, (WCHAR *)package->public.type_name );
    HeapFree( GetProcessHeap(), 0, (WCHAR *)package->public.architecture_name );
    HeapFree( GetProcessHeap(), 0, (WCHAR *)package->public.resource_id );
    HeapFree( GetProcessHeap(), 0, (WCHAR *)package->public.file_name );
    for (i = 0; i < package->public.resource_count; i++)
        free_resource( package->resources + i );
    HeapFree( GetProcessHeap(), 0, package->resources );
    memset( package, 0, sizeof(*package) );
}

static HRESULT append_resource( struct parser *parser,
                                struct bundle_package *package,
                                struct appx_bundle_resource *resource )
{
    struct appx_bundle_resource *resources;
    UINT32 capacity, i;

    if (package->public.resource_count >= MAX_RESOURCES_PER_PACKAGE ||
        parser->manifest->resource_count >= APPX_BUNDLE_MANIFEST_MAX_RESOURCES)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    for (i = 0; i < package->public.resource_count; i++)
    {
        const struct appx_bundle_resource *other = package->resources + i;

        /*
         * BundleManifestSchema2013.xsd declares three independent xs:unique
         * constraints.  A missing field does not participate in its set.
         */
        if (other->language && resource->language &&
            !lstrcmpW( other->language, resource->language ))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (other->has_scale && resource->has_scale &&
            other->scale == resource->scale)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (other->dx_feature_level && resource->dx_feature_level &&
            !lstrcmpW( other->dx_feature_level,
                       resource->dx_feature_level ))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }

    if (package->public.resource_count == package->resource_capacity)
    {
        capacity = package->resource_capacity ? package->resource_capacity * 2 : 4;
        if (capacity > MAX_RESOURCES_PER_PACKAGE)
            capacity = MAX_RESOURCES_PER_PACKAGE;
        if (package->resources)
            resources = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     package->resources,
                                     capacity * sizeof(*resources) );
        else
            resources = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   capacity * sizeof(*resources) );
        if (!resources) return parser_fail( parser, E_OUTOFMEMORY );
        package->resources = resources;
        package->resource_capacity = capacity;
        package->public.resources = resources;
    }
    package->resources[package->public.resource_count++] = *resource;
    parser->manifest->resource_count++;
    memset( resource, 0, sizeof(*resource) );
    return S_OK;
}

static HRESULT parse_resource( struct parser *parser,
                               struct bundle_package *package )
{
    struct xml_attribute *language, *scale, *dx;
    struct appx_bundle_resource resource;
    struct xml_attributes attributes;
    int depth, result;

    memset( &resource, 0, sizeof(resource) );
    if (FAILED(get_attributes( parser, &attributes ))) return parser->hr;
    language = take_attribute( &attributes, "Language" );
    scale = take_attribute( &attributes, "Scale" );
    dx = take_attribute( &attributes, "DXFeatureLevel" );
    if (SUCCEEDED(parser->hr) && language &&
        (FAILED(xml_to_wstring( parser, language->value, 1, 85,
                                (WCHAR **)&resource.language )) ||
         !validate_language( resource.language )))
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr) && scale)
    {
        resource.has_scale = TRUE;
        parse_uint32( parser, scale->value, 1, 1000, &resource.scale );
    }
    if (SUCCEEDED(parser->hr) && dx &&
        (FAILED(xml_to_wstring( parser, dx->value, 1, 32,
                                (WCHAR **)&resource.dx_feature_level )) ||
         !validate_simple_ascii_token( resource.dx_feature_level, 32 )))
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (dx)
        package->public.flags |= APPX_BUNDLE_PACKAGE_UNSUPPORTED_QUALIFIER;
    if (SUCCEEDED(parser->hr))
        finish_attributes( parser, &attributes, &package->public.flags );
    free_attributes( &attributes );
    if (FAILED(parser->hr)) goto done;

    if (!xmlTextReaderIsEmptyElement( parser->reader ))
    {
        depth = xmlTextReaderDepth( parser->reader );
        while ((result = read_node( parser )) > 0)
        {
            int type = xmlTextReaderNodeType( parser->reader );

            if (type == XML_READER_TYPE_END_ELEMENT &&
                xmlTextReaderDepth( parser->reader ) == depth)
                break;
            if (!current_node_is_ignorable_text( parser ))
            {
                parser_fail( parser, APPX_E_INVALID_MANIFEST );
                break;
            }
        }
        if (result <= 0) parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    if (SUCCEEDED(parser->hr))
        append_resource( parser, package, &resource );

done:
    free_resource( &resource );
    return parser->hr;
}

static HRESULT parse_resources( struct parser *parser,
                                struct bundle_package *package )
{
    struct xml_attributes attributes;
    UINT32 initial_count = package->public.resource_count;
    int depth, result;

    if (FAILED(get_attributes( parser, &attributes ))) return parser->hr;
    finish_attributes( parser, &attributes, &package->public.flags );
    free_attributes( &attributes );
    if (FAILED(parser->hr)) return parser->hr;
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    depth = xmlTextReaderDepth( parser->reader );
    while ((result = read_node( parser )) > 0)
    {
        int type = xmlTextReaderNodeType( parser->reader );

        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            break;
        if (type == XML_READER_TYPE_ELEMENT)
        {
            if (node_is( parser, bundle_namespace, "Resource" ))
                parse_resource( parser, package );
            else
                skip_ignorable_element( parser, &package->public.flags );
        }
        else if (!current_node_is_ignorable_text( parser ))
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (FAILED(parser->hr)) break;
    }
    if (result <= 0) parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr) &&
        package->public.resource_count == initial_count)
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return parser->hr;
}

static HRESULT append_package( struct parser *parser,
                               struct bundle_package *package )
{
    struct bundle_package *packages;
    UINT32 capacity;

    if (parser->manifest->package_count >= APPX_BUNDLE_MANIFEST_MAX_PACKAGES)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (parser->manifest->package_count == parser->manifest->package_capacity)
    {
        capacity = parser->manifest->package_capacity ?
                   parser->manifest->package_capacity * 2 : 16;
        if (capacity > APPX_BUNDLE_MANIFEST_MAX_PACKAGES)
            capacity = APPX_BUNDLE_MANIFEST_MAX_PACKAGES;
        if (parser->manifest->packages)
            packages = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    parser->manifest->packages,
                                    capacity * sizeof(*packages) );
        else
            packages = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  capacity * sizeof(*packages) );
        if (!packages) return parser_fail( parser, E_OUTOFMEMORY );
        parser->manifest->packages = packages;
        parser->manifest->package_capacity = capacity;
    }
    parser->manifest->packages[parser->manifest->package_count++] = *package;
    memset( package, 0, sizeof(*package) );
    return S_OK;
}

static HRESULT parse_package( struct parser *parser )
{
    struct xml_attribute *type = NULL, *version = NULL;
    struct xml_attribute *architecture, *resource_id;
    struct xml_attribute *file_name = NULL, *offset = NULL, *size = NULL;
    struct xml_attribute *encrypted, *extension_encrypted;
    struct bundle_package package;
    struct xml_attributes attributes;
    BOOL resources_seen = FALSE;
    int depth, result;

    memset( &package, 0, sizeof(package) );
    if (FAILED(get_attributes( parser, &attributes ))) return parser->hr;
    type = take_attribute( &attributes, "Type" );
    require_attribute( parser, &attributes, "Version", &version );
    require_attribute( parser, &attributes, "FileName", &file_name );
    architecture = take_attribute( &attributes, "Architecture" );
    resource_id = take_attribute( &attributes, "ResourceId" );
    require_attribute( parser, &attributes, "Offset", &offset );
    require_attribute( parser, &attributes, "Size", &size );
    encrypted = take_attribute( &attributes, "Encrypted" );
    if (!encrypted)
        encrypted = take_attribute( &attributes, "IsEncrypted" );
    extension_encrypted = take_ignorable_attribute( parser, &attributes,
                                                     "Encrypted" );
    if (!extension_encrypted)
        extension_encrypted = take_ignorable_attribute( parser, &attributes,
                                                        "IsEncrypted" );
    if (encrypted && extension_encrypted)
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr) && type)
    {
        if (FAILED(xml_to_wstring( parser, type->value, 1, 64,
                                   (WCHAR **)&package.public.type_name )) ||
            !validate_simple_ascii_token( package.public.type_name, 64 ))
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    else if (SUCCEEDED(parser->hr) &&
             !(package.public.type_name = duplicate_wstring( L"resource" )))
        parser_fail( parser, E_OUTOFMEMORY );
    if (SUCCEEDED(parser->hr))
    {
        package.public.type = parse_package_type( package.public.type_name );
        if (package.public.type == APPX_BUNDLE_PACKAGE_UNSUPPORTED)
            package.public.flags |= APPX_BUNDLE_PACKAGE_UNSUPPORTED_TYPE;
        parse_version( parser, version->value, &package.public.version );
    }
    if (SUCCEEDED(parser->hr) && architecture)
        xml_to_wstring( parser, architecture->value, 1, 32,
                        (WCHAR **)&package.public.architecture_name );
    else if (SUCCEEDED(parser->hr) &&
             !(package.public.architecture_name =
               duplicate_wstring( L"neutral" )))
        parser_fail( parser, E_OUTOFMEMORY );
    if (SUCCEEDED(parser->hr) &&
        !validate_simple_ascii_token( package.public.architecture_name, 32 ))
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr))
    {
        package.public.architecture =
            parse_architecture_name( package.public.architecture_name );
        if (package.public.architecture == APPX_BUNDLE_ARCHITECTURE_X86A64 ||
            package.public.architecture == APPX_BUNDLE_ARCHITECTURE_UNSUPPORTED)
            package.public.flags |=
                APPX_BUNDLE_PACKAGE_UNSUPPORTED_ARCHITECTURE;
    }
    if (SUCCEEDED(parser->hr) && resource_id)
        xml_to_wstring( parser, resource_id->value, 0, 30,
                        (WCHAR **)&package.public.resource_id );
    else if (SUCCEEDED(parser->hr) &&
             !(package.public.resource_id = duplicate_wstring( L"" )))
        parser_fail( parser, E_OUTOFMEMORY );
    if (SUCCEEDED(parser->hr) &&
        (!validate_normalized_string( package.public.resource_id, TRUE ) ||
         (*package.public.resource_id &&
          !validate_simple_ascii_token( package.public.resource_id, 30 ))))
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr))
        parse_file_name( parser, file_name->value,
                         (WCHAR **)&package.public.file_name );
    if (SUCCEEDED(parser->hr))
    {
        parse_uint64( parser, offset->value, &package.public.offset );
        parse_uint64( parser, size->value, &package.public.size );
        if (SUCCEEDED(parser->hr) &&
            (!package.public.size ||
             package.public.offset >
             ~(UINT64)0 - package.public.size))
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
        package.public.flags |= APPX_BUNDLE_PACKAGE_HAS_RANGE;
    }
    if (SUCCEEDED(parser->hr) && encrypted)
    {
        BOOL value;

        if (SUCCEEDED(parse_boolean( parser, encrypted->value, &value )) &&
            value)
            package.public.flags |= APPX_BUNDLE_PACKAGE_ENCRYPTED;
    }
    if (SUCCEEDED(parser->hr) && extension_encrypted)
    {
        BOOL value;

        package.public.flags |= APPX_BUNDLE_PACKAGE_UNSUPPORTED_EXTENSION;
        parser->manifest->unsupported_extensions = TRUE;
        if (SUCCEEDED(parse_boolean( parser, extension_encrypted->value,
                                     &value )) && value)
            package.public.flags |= APPX_BUNDLE_PACKAGE_ENCRYPTED;
    }
    if (SUCCEEDED(parser->hr))
        finish_attributes( parser, &attributes, &package.public.flags );
    free_attributes( &attributes );
    if (FAILED(parser->hr)) goto done;

    if (!xmlTextReaderIsEmptyElement( parser->reader ))
    {
        depth = xmlTextReaderDepth( parser->reader );
        while ((result = read_node( parser )) > 0)
        {
            int node_type = xmlTextReaderNodeType( parser->reader );

            if (node_type == XML_READER_TYPE_END_ELEMENT &&
                xmlTextReaderDepth( parser->reader ) == depth)
                break;
            if (node_type == XML_READER_TYPE_ELEMENT)
            {
                if (node_is( parser, bundle_namespace, "Resources" ))
                {
                    if (resources_seen)
                        parser_fail( parser, APPX_E_INVALID_MANIFEST );
                    else
                    {
                        resources_seen = TRUE;
                        parse_resources( parser, &package );
                    }
                }
                else
                    skip_ignorable_element( parser, &package.public.flags );
            }
            else if (!current_node_is_ignorable_text( parser ))
                parser_fail( parser, APPX_E_INVALID_MANIFEST );
            if (FAILED(parser->hr)) break;
        }
        if (result <= 0) parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    if (SUCCEEDED(parser->hr) && !resources_seen)
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr) &&
        (package.public.type == APPX_BUNDLE_PACKAGE_APPLICATION ||
         package.public.type == APPX_BUNDLE_PACKAGE_FRAMEWORK) &&
        *package.public.resource_id)
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr))
        append_package( parser, &package );

done:
    if (FAILED(parser->hr))
        parser->manifest->resource_count -= package.public.resource_count;
    free_package( &package );
    return parser->hr;
}

static HRESULT parse_identity( struct parser *parser )
{
    struct appx_bundle_identity *identity = &parser->manifest->identity;
    struct xml_attribute *name, *publisher, *version;
    struct xml_attributes attributes;

    if (FAILED(get_attributes( parser, &attributes ))) return parser->hr;
    require_attribute( parser, &attributes, "Name", &name );
    require_attribute( parser, &attributes, "Publisher", &publisher );
    require_attribute( parser, &attributes, "Version", &version );
    if (SUCCEEDED(parser->hr))
        xml_to_wstring( parser, name->value, 3, 50,
                        (WCHAR **)&identity->name );
    if (SUCCEEDED(parser->hr) && !validate_identity_name( identity->name ))
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr))
        xml_to_wstring( parser, publisher->value, 1, 8192,
                        (WCHAR **)&identity->publisher );
    if (SUCCEEDED(parser->hr) &&
        !validate_normalized_string( identity->publisher, FALSE ))
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr))
        parse_version( parser, version->value, &identity->version );
    if (SUCCEEDED(parser->hr))
        finish_attributes( parser, &attributes, NULL );
    free_attributes( &attributes );
    if (FAILED(parser->hr)) return parser->hr;
    if (!xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return S_OK;
}

static HRESULT parse_packages( struct parser *parser )
{
    struct xml_attributes attributes;
    int depth, result;

    if (FAILED(get_attributes( parser, &attributes ))) return parser->hr;
    finish_attributes( parser, &attributes, NULL );
    free_attributes( &attributes );
    if (FAILED(parser->hr)) return parser->hr;
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    depth = xmlTextReaderDepth( parser->reader );
    while ((result = read_node( parser )) > 0)
    {
        int type = xmlTextReaderNodeType( parser->reader );

        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            break;
        if (type == XML_READER_TYPE_ELEMENT)
        {
            if (node_is( parser, bundle_namespace, "Package" ))
                parse_package( parser );
            else
                skip_ignorable_element( parser, NULL );
        }
        else if (!current_node_is_ignorable_text( parser ))
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (FAILED(parser->hr)) break;
    }
    if (result <= 0) parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (!parser->manifest->package_count)
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return parser->hr;
}

static HRESULT parse_schema_version( struct parser *parser,
                                     const xmlChar *value )
{
    const xmlChar *cursor = value;
    int length;
    UINT32 parts = 0;

    /* Three unsigned 16-bit decimal components require at most 17 bytes. */
    if (!value || !*value || (length = xmlStrlen( value )) <= 0 ||
        length > 17)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    while (*cursor)
    {
        UINT32 digits = 0, number = 0;

        if (++parts > 3)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9')
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        while (*cursor >= '0' && *cursor <= '9')
        {
            number = number * 10 + (*cursor++ - '0');
            if (++digits > 5 || number > 0xffff)
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        }
        if (!digits || (*cursor && *cursor != '.'))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (*cursor)
        {
            if (!cursor[1])
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
            cursor++;
        }
    }
    if (parts < 2)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return S_OK;
}

static int compare_wstring_ci( const WCHAR *left, const WCHAR *right )
{
    int result = CompareStringOrdinal( left, -1, right, -1, TRUE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static int compare_file_pointer( const void *left, const void *right )
{
    const struct package_pointer *a = left, *b = right;

    return compare_wstring_ci( a->package->public.file_name,
                               b->package->public.file_name );
}

static int compare_version_values( const struct appx_bundle_version *left,
                                   const struct appx_bundle_version *right )
{
    if (left->major != right->major)
        return left->major < right->major ? -1 : 1;
    if (left->minor != right->minor)
        return left->minor < right->minor ? -1 : 1;
    if (left->build != right->build)
        return left->build < right->build ? -1 : 1;
    if (left->revision != right->revision)
        return left->revision < right->revision ? -1 : 1;
    return 0;
}

static int compare_identity_pointer( const void *left, const void *right )
{
    const struct appx_bundle_package *a =
        &((const struct package_pointer *)left)->package->public;
    const struct appx_bundle_package *b =
        &((const struct package_pointer *)right)->package->public;
    int result;

    if ((result = compare_wstring_ci( a->type_name, b->type_name ))) return result;
    if ((result = compare_wstring_ci( a->architecture_name,
                                      b->architecture_name )))
        return result;
    if ((result = compare_wstring_ci( a->resource_id, b->resource_id )))
        return result;
    return compare_version_values( &a->version, &b->version );
}

static int compare_range_pointer( const void *left, const void *right )
{
    const struct appx_bundle_package *a =
        &((const struct package_pointer *)left)->package->public;
    const struct appx_bundle_package *b =
        &((const struct package_pointer *)right)->package->public;

    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    if (a->size != b->size) return a->size < b->size ? -1 : 1;
    return compare_wstring_ci( a->file_name, b->file_name );
}

static HRESULT validate_packages( struct parser *parser )
{
    struct package_pointer *pointers;
    UINT32 i, range_count = 0;

    if (!(pointers = HeapAlloc( GetProcessHeap(), 0,
                                parser->manifest->package_count *
                                sizeof(*pointers) )))
        return parser_fail( parser, E_OUTOFMEMORY );
    for (i = 0; i < parser->manifest->package_count; i++)
        pointers[i].package = parser->manifest->packages + i;

    qsort( pointers, parser->manifest->package_count, sizeof(*pointers),
           compare_file_pointer );
    for (i = 1; i < parser->manifest->package_count; i++)
        if (!compare_file_pointer( pointers + i - 1, pointers + i ))
            goto invalid;

    qsort( pointers, parser->manifest->package_count, sizeof(*pointers),
           compare_identity_pointer );
    for (i = 1; i < parser->manifest->package_count; i++)
        if (!compare_identity_pointer( pointers + i - 1, pointers + i ))
            goto invalid;

    for (i = 0; i < parser->manifest->package_count; i++)
        if (parser->manifest->packages[i].public.flags &
            APPX_BUNDLE_PACKAGE_HAS_RANGE)
            pointers[range_count++].package = parser->manifest->packages + i;
    qsort( pointers, range_count, sizeof(*pointers), compare_range_pointer );
    for (i = 1; i < range_count; i++)
    {
        const struct appx_bundle_package *previous =
            &pointers[i - 1].package->public;
        const struct appx_bundle_package *current =
            &pointers[i].package->public;

        if (current->offset < previous->offset + previous->size)
            goto invalid;
    }

    HeapFree( GetProcessHeap(), 0, pointers );
    return S_OK;

invalid:
    HeapFree( GetProcessHeap(), 0, pointers );
    return parser_fail( parser, APPX_E_INVALID_MANIFEST );
}

static HRESULT parse_bundle( struct parser *parser )
{
    struct xml_attribute *ignorable, *schema_version = NULL;
    struct xml_attributes attributes;
    BOOL identity_seen = FALSE, packages_seen = FALSE;
    int depth, result;

    if (!node_is( parser, bundle_namespace, "Bundle" ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (FAILED(get_attributes( parser, &attributes ))) return parser->hr;
    ignorable = take_attribute( &attributes, "IgnorableNamespaces" );
    require_attribute( parser, &attributes, "SchemaVersion",
                       &schema_version );
    if (ignorable)
        parse_ignorable_namespaces( parser, ignorable->value );
    if (SUCCEEDED(parser->hr) && schema_version)
        parse_schema_version( parser, schema_version->value );
    if (SUCCEEDED(parser->hr))
        finish_attributes( parser, &attributes, NULL );
    free_attributes( &attributes );
    if (FAILED(parser->hr)) return parser->hr;
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    depth = xmlTextReaderDepth( parser->reader );
    while ((result = read_node( parser )) > 0)
    {
        int type = xmlTextReaderNodeType( parser->reader );

        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            break;
        if (type == XML_READER_TYPE_ELEMENT)
        {
            if (node_is( parser, bundle_namespace, "Identity" ))
            {
                if (identity_seen)
                    parser_fail( parser, APPX_E_INVALID_MANIFEST );
                else
                {
                    identity_seen = TRUE;
                    parse_identity( parser );
                }
            }
            else if (node_is( parser, bundle_namespace, "Packages" ))
            {
                if (packages_seen)
                    parser_fail( parser, APPX_E_INVALID_MANIFEST );
                else
                {
                    packages_seen = TRUE;
                    parse_packages( parser );
                }
            }
            else
                skip_ignorable_element( parser, NULL );
        }
        else if (!current_node_is_ignorable_text( parser ))
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (FAILED(parser->hr)) break;
    }
    if (result <= 0 || !identity_seen || !packages_seen)
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (SUCCEEDED(parser->hr)) validate_packages( parser );
    return parser->hr;
}

static BOOL validate_utf8_input( const BYTE *data, SIZE_T size )
{
    if (memchr( data, 0, size )) return FALSE;
    return MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS,
                                (const char *)data, (int)size, NULL, 0 ) != 0;
}

static SIZE_T skip_xml_section( const BYTE *data, SIZE_T size, SIZE_T offset,
                                const char *terminator )
{
    SIZE_T length = strlen( terminator );

    while (offset + length <= size)
    {
        if (!memcmp( data + offset, terminator, length ))
            return offset + length;
        offset++;
    }
    return size;
}

static BOOL contains_doctype_declaration( const BYTE *data, SIZE_T size )
{
    static const char instruction[] = "<?";
    static const char comment[] = "<!--";
    static const char cdata[] = "<![CDATA[";
    static const char doctype[] = "<!DOCTYPE";
    SIZE_T offset = 0;

    while (offset < size)
    {
        if (data[offset] != '<')
        {
            offset++;
            continue;
        }
        if (size - offset >= sizeof(comment) - 1 &&
            !memcmp( data + offset, comment, sizeof(comment) - 1 ))
        {
            offset = skip_xml_section( data, size,
                                       offset + sizeof(comment) - 1, "-->" );
            continue;
        }
        if (size - offset >= sizeof(cdata) - 1 &&
            !memcmp( data + offset, cdata, sizeof(cdata) - 1 ))
        {
            offset = skip_xml_section( data, size,
                                       offset + sizeof(cdata) - 1, "]]>" );
            continue;
        }
        if (size - offset >= sizeof(instruction) - 1 &&
            !memcmp( data + offset, instruction,
                     sizeof(instruction) - 1 ))
        {
            offset = skip_xml_section( data, size,
                                       offset + sizeof(instruction) - 1, "?>" );
            continue;
        }
        if (size - offset >= sizeof(doctype) - 1 &&
            !memcmp( data + offset, doctype, sizeof(doctype) - 1 ))
            return TRUE;
        offset++;
    }
    return FALSE;
}

HRESULT WINAPI appx_bundle_manifest_parse( const BYTE *data, SIZE_T size,
                                           APPX_BUNDLE_MANIFEST **result )
{
    APPX_BUNDLE_MANIFEST *manifest = NULL;
    struct parser parser;
    const xmlChar *encoding;
    int read_result;
    HRESULT hr;
    UINT32 i;

    if (result) *result = NULL;
    if (!data || !size || !result) return E_INVALIDARG;
    if (size > APPX_BUNDLE_MANIFEST_MAX_SIZE || size > INT_MAX ||
        !validate_utf8_input( data, size ) ||
        contains_doctype_declaration( data, size ))
        return APPX_E_INVALID_MANIFEST;
    if (!(manifest = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                sizeof(*manifest) )))
        return E_OUTOFMEMORY;

    memset( &parser, 0, sizeof(parser) );
    parser.manifest = manifest;
    parser.hr = S_OK;
    parser.reader = xmlReaderForMemory( (const char *)data, (int)size, NULL, NULL,
                                        XML_PARSE_NONET | XML_PARSE_COMPACT |
                                        XML_PARSE_NOERROR | XML_PARSE_NOWARNING );
    if (!parser.reader)
    {
        hr = APPX_E_INVALID_MANIFEST;
        goto done;
    }
    xmlTextReaderSetErrorHandler( parser.reader, silent_xml_error, &parser );
    xmlTextReaderSetParserProp( parser.reader, XML_PARSER_LOADDTD, 0 );
    xmlTextReaderSetParserProp( parser.reader, XML_PARSER_DEFAULTATTRS, 0 );
    xmlTextReaderSetParserProp( parser.reader, XML_PARSER_VALIDATE, 0 );
    xmlTextReaderSetParserProp( parser.reader, XML_PARSER_SUBST_ENTITIES, 0 );

    while ((read_result = read_node( &parser )) > 0)
    {
        int type = xmlTextReaderNodeType( parser.reader );

        if (type == XML_READER_TYPE_ELEMENT) break;
        if (!current_node_is_ignorable_text( &parser ))
        {
            parser_fail( &parser, APPX_E_INVALID_MANIFEST );
            break;
        }
    }
    if (read_result <= 0 || FAILED(parser.hr))
        parser_fail( &parser, APPX_E_INVALID_MANIFEST );
    else
        parse_bundle( &parser );
    encoding = xmlTextReaderConstEncoding( parser.reader );
    if (encoding && !xml_equal( encoding, BAD_CAST "UTF-8" ) &&
        !xml_equal( encoding, BAD_CAST "utf-8" ))
        parser_fail( &parser, APPX_E_INVALID_MANIFEST );

    if (SUCCEEDED(parser.hr))
    {
        while ((read_result = read_node( &parser )) > 0)
            if (!current_node_is_ignorable_text( &parser ))
            {
                parser_fail( &parser, APPX_E_INVALID_MANIFEST );
                break;
            }
        if (read_result < 0) parser_fail( &parser, APPX_E_INVALID_MANIFEST );
    }
    hr = parser.hr;

done:
    if (parser.reader) xmlFreeTextReader( parser.reader );
    for (i = 0; i < parser.ignorable_count; i++)
    {
        xmlFree( parser.ignorable[i].prefix );
        xmlFree( parser.ignorable[i].uri );
    }
    if (FAILED(hr))
        appx_bundle_manifest_free( manifest );
    else
        *result = manifest;
    return hr;
}

void WINAPI appx_bundle_manifest_free( APPX_BUNDLE_MANIFEST *manifest )
{
    UINT32 i;

    if (!manifest) return;
    HeapFree( GetProcessHeap(), 0, (WCHAR *)manifest->identity.name );
    HeapFree( GetProcessHeap(), 0, (WCHAR *)manifest->identity.publisher );
    for (i = 0; i < manifest->package_count; i++)
        free_package( manifest->packages + i );
    HeapFree( GetProcessHeap(), 0, manifest->packages );
    HeapFree( GetProcessHeap(), 0, manifest );
}

const struct appx_bundle_identity * WINAPI appx_bundle_manifest_get_identity(
    const APPX_BUNDLE_MANIFEST *manifest )
{
    return manifest ? &manifest->identity : NULL;
}

UINT32 WINAPI appx_bundle_manifest_get_package_count(
    const APPX_BUNDLE_MANIFEST *manifest )
{
    return manifest ? manifest->package_count : 0;
}

const struct appx_bundle_package * WINAPI appx_bundle_manifest_get_package(
    const APPX_BUNDLE_MANIFEST *manifest, UINT32 index )
{
    if (!manifest || index >= manifest->package_count) return NULL;
    return &manifest->packages[index].public;
}

static BOOL valid_host_architecture( enum appx_bundle_architecture architecture )
{
    return architecture == APPX_BUNDLE_ARCHITECTURE_X86 ||
           architecture == APPX_BUNDLE_ARCHITECTURE_X64 ||
           architecture == APPX_BUNDLE_ARCHITECTURE_ARM ||
           architecture == APPX_BUNDLE_ARCHITECTURE_ARM64;
}

static UINT32 architecture_rank( enum appx_bundle_architecture package,
                                 enum appx_bundle_architecture host )
{
    if (package == host) return 2;
    if (package == APPX_BUNDLE_ARCHITECTURE_NEUTRAL) return 1;
    return 0;
}

static BOOL resource_declaration_matches(
    const struct appx_bundle_resource *resource,
    const struct appx_bundle_selection_policy *policy )
{
    if (resource->dx_feature_level) return FALSE;
    if (resource->language)
    {
        if (policy->language_neutral_only ||
            lstrcmpiW( resource->language, policy->language ))
            return FALSE;
    }
    if (resource->has_scale)
    {
        if (policy->scale_neutral_only || resource->scale != policy->scale)
            return FALSE;
    }
    return TRUE;
}

static BOOL resource_package_matches(
    const struct appx_bundle_package *package,
    const struct appx_bundle_selection_policy *policy )
{
    UINT32 i;

    if (!architecture_rank( package->architecture, policy->host_architecture ))
        return FALSE;
    if (!package->resource_count) return TRUE;
    for (i = 0; i < package->resource_count; i++)
        if (resource_declaration_matches( package->resources + i, policy ))
            return TRUE;
    return FALSE;
}

HRESULT WINAPI appx_bundle_manifest_select(
    const APPX_BUNDLE_MANIFEST *manifest,
    const struct appx_bundle_selection_policy *policy,
    struct appx_bundle_selection *selection )
{
    const struct appx_bundle_package *best = NULL;
    UINT32 best_index = 0, best_rank = 0, i;
    BOOL ambiguous = FALSE, payload_seen = FALSE;

    if (!selection || selection->size != sizeof(*selection))
        return E_INVALIDARG;
    memset( selection, 0, sizeof(*selection) );
    selection->size = sizeof(*selection);
    if (!manifest || !policy ||
        policy->size != sizeof(*policy) ||
        !valid_host_architecture( policy->host_architecture ))
        return E_INVALIDARG;
    if ((!policy->language_neutral_only &&
         (!policy->language || !validate_language( policy->language ))) ||
        (!policy->scale_neutral_only &&
         (policy->scale < 1 || policy->scale > 1000)))
        return E_INVALIDARG;

    if (manifest->unsupported_extensions)
        selection->issues |= APPX_BUNDLE_SELECTION_UNSUPPORTED_EXTENSION;

    for (i = 0; i < manifest->package_count; i++)
    {
        const struct appx_bundle_package *package =
            &manifest->packages[i].public;
        UINT32 rank;

        if (package->flags & APPX_BUNDLE_PACKAGE_ENCRYPTED)
            selection->issues |= APPX_BUNDLE_SELECTION_ENCRYPTED_PAYLOAD;
        if (package->flags & APPX_BUNDLE_PACKAGE_UNSUPPORTED_TYPE)
            selection->issues |= APPX_BUNDLE_SELECTION_UNSUPPORTED_TYPE;
        if (package->flags & APPX_BUNDLE_PACKAGE_UNSUPPORTED_ARCHITECTURE)
            selection->issues |=
                APPX_BUNDLE_SELECTION_UNSUPPORTED_ARCHITECTURE;
        if (package->flags & APPX_BUNDLE_PACKAGE_UNSUPPORTED_EXTENSION)
            selection->issues |= APPX_BUNDLE_SELECTION_UNSUPPORTED_EXTENSION;
        if (package->flags & APPX_BUNDLE_PACKAGE_UNSUPPORTED_QUALIFIER)
            selection->issues |= APPX_BUNDLE_SELECTION_UNSUPPORTED_QUALIFIER;

        if (package->type == APPX_BUNDLE_PACKAGE_RESOURCE)
        {
            selection->issues |= APPX_BUNDLE_SELECTION_RESOURCE_PAYLOAD;
            if (resource_package_matches( package, policy ))
            {
                selection->issues |=
                    APPX_BUNDLE_SELECTION_MATCHING_RESOURCE_PAYLOAD;
                selection->matching_resource_count++;
            }
            continue;
        }
        if (package->type == APPX_BUNDLE_PACKAGE_OPTIONAL)
        {
            selection->issues |= APPX_BUNDLE_SELECTION_OPTIONAL_PAYLOAD;
            continue;
        }
        if (package->type != APPX_BUNDLE_PACKAGE_APPLICATION &&
            package->type != APPX_BUNDLE_PACKAGE_FRAMEWORK)
            continue;
        payload_seen = TRUE;
        rank = architecture_rank( package->architecture,
                                  policy->host_architecture );
        if (!rank)
        {
            selection->issues |=
                APPX_BUNDLE_SELECTION_INCOMPATIBLE_ARCHITECTURE;
            continue;
        }
        if ((package->flags & (APPX_BUNDLE_PACKAGE_ENCRYPTED |
                               APPX_BUNDLE_PACKAGE_UNSUPPORTED_ARCHITECTURE |
                               APPX_BUNDLE_PACKAGE_UNSUPPORTED_EXTENSION |
                               APPX_BUNDLE_PACKAGE_UNSUPPORTED_QUALIFIER)) ||
            manifest->unsupported_extensions)
            continue;
        if (!best || rank > best_rank ||
            (rank == best_rank &&
             compare_version_values( &package->version,
                                     &best->version ) > 0))
        {
            best = package;
            best_index = i;
            best_rank = rank;
            ambiguous = FALSE;
        }
        else if (rank == best_rank &&
                 !compare_version_values( &package->version,
                                          &best->version ))
            ambiguous = TRUE;
    }

    if (ambiguous)
    {
        selection->issues |= APPX_BUNDLE_SELECTION_AMBIGUOUS_PAYLOAD |
                             APPX_BUNDLE_SELECTION_NO_PAYLOAD;
        return HRESULT_FROM_WIN32( ERROR_DUP_NAME );
    }
    if (!best)
    {
        selection->issues |= APPX_BUNDLE_SELECTION_NO_PAYLOAD;
        if (payload_seen)
            return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
        return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    }
    selection->payload = best;
    selection->package_index = best_index;
    return S_OK;
}
