/*
 * AppX manifest parser
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

#include <libxml/xmlreader.h>

#include "manifest.h"

#define XML_MAX_DEPTH                    32
#define XML_MAX_NODES                    65536
#define XML_MAX_ATTRIBUTES_PER_ELEMENT   64
#define XML_MAX_ATTRIBUTES               262144
#define XML_MAX_NAMESPACE_DECLARATIONS   128
#define XML_MAX_IGNORABLE_NAMESPACES     64
#define XML_MAX_LOCAL_NAME_BYTES         256
#define XML_MAX_PREFIX_BYTES             128
#define XML_MAX_NAMESPACE_BYTES          1024
#define XML_MAX_VALUE_BYTES              65536
#define XML_MAX_INPROC_SERVERS           128
#define XML_MAX_CLASSES_PER_SERVER       256
#define XML_MAX_RESOURCES                200

#define RESOURCE_QUALIFIER_LANGUAGE      0x00000001
#define RESOURCE_QUALIFIER_SCALE         0x00000002
#define RESOURCE_QUALIFIER_DX_FEATURE    0x00000004

static const xmlChar foundation_namespace[] =
    "http://schemas.microsoft.com/appx/manifest/foundation/windows10";
static const xmlChar windows8_namespace[] =
    "http://schemas.microsoft.com/appx/2010/manifest";
static const xmlChar uap_namespace[] =
    "http://schemas.microsoft.com/appx/manifest/uap/windows10";
static const xmlChar uap6_namespace[] =
    "http://schemas.microsoft.com/appx/manifest/uap/windows10/6";
static const xmlChar uap10_namespace[] =
    "http://schemas.microsoft.com/appx/manifest/uap/windows10/10";
static const xmlChar uap11_namespace[] =
    "http://schemas.microsoft.com/appx/manifest/uap/windows10/11";
static const xmlChar rescap_namespace[] =
    "http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities";
static const xmlChar xmlns_namespace[] = "http://www.w3.org/2000/xmlns/";
static const xmlChar xinclude_namespace[] = "http://www.w3.org/2001/XInclude";

struct appx_manifest
{
    struct appx_manifest_identity identity;
    struct appx_manifest_application *applications;
    struct appx_manifest_dependency *dependencies;
    struct appx_manifest_target_family *target_families;
    struct appx_manifest_inproc_class *classes;
    enum appx_manifest_unsupported_reason *unsupported_reasons;
    UINT32 application_count;
    UINT32 dependency_count;
    UINT32 target_family_count;
    UINT32 class_count;
    UINT32 unsupported_reason_count;
    BOOL framework;
    BOOL resource_package;
    BOOL run_full_trust;
};

struct ignorable_namespace
{
    xmlChar *prefix;
    xmlChar *uri;
};

struct parser
{
    xmlTextReaderPtr reader;
    APPX_MANIFEST *manifest;
    const xmlChar *foundation_uri;
    struct ignorable_namespace ignorable[XML_MAX_IGNORABLE_NAMESPACES];
    UINT32 ignorable_count;
    UINT32 node_count;
    UINT32 attribute_count;
    UINT32 namespace_count;
    UINT32 inproc_server_count;
    BOOL identity_seen;
    BOOL properties_seen;
    BOOL resources_seen;
    BOOL prerequisites_seen;
    BOOL os_min_version_seen;
    BOOL os_max_version_seen;
    BOOL dependencies_seen;
    BOOL applications_seen;
    BOOL capabilities_seen;
    BOOL extensions_seen;
    BOOL framework_seen;
    BOOL resource_package_seen;
    BOOL display_name_seen;
    BOOL publisher_display_name_seen;
    BOOL logo_seen;
    BOOL windows8_schema;
    BOOL architecture_seen;
    BOOL executable_code_seen;
    BOOL mixed_resource_qualifier;
    UINT32 resource_count;
    UINT32 resource_qualifier_types;
    UINT16 os_min_version[3];
    UINT16 os_max_version[3];
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

static BOOL foundation_node_is( struct parser *parser, const char *local )
{
    return node_is( parser, parser->foundation_uri, local );
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

static BOOL is_forbidden_node_type( int type )
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
 * This is the only function which advances the reader.  Consequently skipped
 * extension trees are subject to the same depth, node, attribute, namespace,
 * name and value bounds as recognized manifest data.
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
    if (is_forbidden_node_type( type ))
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
        const xmlChar *uri, *prefix;

        if (xmlTextReaderMoveToAttributeNo( parser->reader, i ) != 1)
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            break;
        }
        uri = xmlTextReaderConstNamespaceUri( parser->reader );
        prefix = xmlTextReaderConstPrefix( parser->reader );
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

static BOOL current_text_is_whitespace( struct parser *parser )
{
    int type = xmlTextReaderNodeType( parser->reader );

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
        const xmlChar *local, *uri, *prefix, *value;
        UINT32 j;

        if (xmlTextReaderMoveToAttributeNo( parser->reader, i ) != 1)
            goto invalid;
        local = empty_xml_string( xmlTextReaderConstLocalName( parser->reader ) );
        uri = empty_xml_string( xmlTextReaderConstNamespaceUri( parser->reader ) );
        prefix = empty_xml_string( xmlTextReaderConstPrefix( parser->reader ) );
        value = empty_xml_string( xmlTextReaderConstValue( parser->reader ) );

        for (j = 0; j < attributes->count; j++)
        {
            if (xml_equal( attributes->item[j].local, local ) &&
                xml_equal( attributes->item[j].uri, uri ))
                goto invalid;
        }

        if (!(attribute->local = xmlStrdup( local )) ||
            !(attribute->uri = xmlStrdup( uri )) ||
            !(attribute->prefix = xmlStrdup( prefix )) ||
            !(attribute->value = xmlStrdup( value )))
        {
            xmlFree( attribute->local );
            xmlFree( attribute->uri );
            xmlFree( attribute->prefix );
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
                                             const xmlChar *uri, const char *local )
{
    UINT32 i;

    for (i = 0; i < attributes->count; i++)
    {
        struct xml_attribute *attribute = attributes->item + i;

        if (!attribute->used && xml_equal( attribute->uri, uri ) &&
            xml_equal( attribute->local, BAD_CAST local ))
        {
            attribute->used = TRUE;
            return attribute;
        }
    }
    return NULL;
}

static BOOL prefix_is_ignorable( const struct parser *parser, const xmlChar *prefix,
                                 const xmlChar *uri )
{
    UINT32 i;

    if (!prefix || !*prefix) return FALSE;
    for (i = 0; i < parser->ignorable_count; i++)
        if (xml_equal( parser->ignorable[i].prefix, prefix ) &&
            xml_equal( parser->ignorable[i].uri, uri ))
            return TRUE;
    return FALSE;
}

static HRESULT add_unsupported_reason( struct parser *parser,
                                       enum appx_manifest_unsupported_reason reason )
{
    APPX_MANIFEST *manifest = parser->manifest;
    enum appx_manifest_unsupported_reason *reasons;
    UINT32 i, count;

    for (i = 0; i < manifest->unsupported_reason_count; i++)
        if (manifest->unsupported_reasons[i] == reason) return S_OK;

    count = manifest->unsupported_reason_count + 1;
    if (manifest->unsupported_reasons)
        reasons = HeapReAlloc( GetProcessHeap(), 0, manifest->unsupported_reasons,
                               count * sizeof(*reasons) );
    else
        reasons = HeapAlloc( GetProcessHeap(), 0, count * sizeof(*reasons) );
    if (!reasons) return parser_fail( parser, E_OUTOFMEMORY );
    manifest->unsupported_reasons = reasons;
    reasons[manifest->unsupported_reason_count++] = reason;
    return S_OK;
}

static HRESULT finish_attributes( struct parser *parser,
                                  struct xml_attributes *attributes, BOOL semantic )
{
    UINT32 i;

    for (i = 0; i < attributes->count; i++)
    {
        struct xml_attribute *attribute = attributes->item + i;

        if (attribute->used) continue;
        if (!*attribute->uri)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (prefix_is_ignorable( parser, attribute->prefix, attribute->uri ))
        {
            if (semantic && FAILED(add_unsupported_reason(
                    parser, APPX_MANIFEST_UNSUPPORTED_IGNORABLE_CONTENT )))
                return parser->hr;
        }
        else if (FAILED(add_unsupported_reason(
                     parser, APPX_MANIFEST_UNSUPPORTED_UNKNOWN_NAMESPACE )))
            return parser->hr;
        attribute->used = TRUE;
    }
    return S_OK;
}

static WCHAR *duplicate_wstring( const WCHAR *source )
{
    SIZE_T size;
    WCHAR *result;

    if (!source) return NULL;
    size = (lstrlenW( source ) + 1) * sizeof(*source);
    if ((result = HeapAlloc( GetProcessHeap(), 0, size )))
        memcpy( result, source, size );
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
    count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)value,
                                 bytes, NULL, 0 );
    if ((!count && bytes) || count < 0 || (UINT32)count < minimum ||
        (UINT32)count > maximum)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (!(string = HeapAlloc( GetProcessHeap(), 0, (count + 1) * sizeof(*string) )))
        return parser_fail( parser, E_OUTOFMEMORY );
    if (count && MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS,
                                      (const char *)value, bytes, string, count ) != count)
    {
        HeapFree( GetProcessHeap(), 0, string );
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    string[count] = 0;
    *result = string;
    return S_OK;
}

static HRESULT require_attribute( struct parser *parser, struct xml_attributes *attributes,
                                  const xmlChar *uri, const char *local,
                                  struct xml_attribute **attribute )
{
    if (!(*attribute = take_attribute( attributes, uri, local )))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return S_OK;
}

static HRESULT parse_version( struct parser *parser, const xmlChar *value,
                              struct appx_manifest_version *version )
{
    UINT16 parts[4];
    const xmlChar *cursor = value;
    UINT32 i;

    memset( version, 0, sizeof(*version) );
    memset( parts, 0, sizeof(parts) );
    if (!value || !*value) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    for (i = 0; i < 4; i++)
    {
        UINT32 number = 0, digits = 0;

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

static HRESULT parse_uint16( struct parser *parser, const xmlChar *value, UINT16 *result )
{
    UINT32 number = 0, digits = 0;

    if (!value || !*value) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    while (*value >= '0' && *value <= '9')
    {
        number = number * 10 + (*value++ - '0');
        if (++digits > 5 || number > 0xffff)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    if (*value || !digits) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    *result = number;
    return S_OK;
}

static HRESULT parse_boolean_value( struct parser *parser, const WCHAR *value, BOOL *result )
{
    if (!lstrcmpW( value, L"true" ) || !lstrcmpW( value, L"1" ))
        *result = TRUE;
    else if (!lstrcmpW( value, L"false" ) || !lstrcmpW( value, L"0" ))
        *result = FALSE;
    else
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return S_OK;
}

static BOOL is_ascii_identity_character( WCHAR character )
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           character == '.' || character == '-';
}

static BOOL reserved_dos_component( const WCHAR *component, UINT32 length );

static BOOL validate_identity_name( const WCHAR *name, UINT32 minimum, UINT32 maximum,
                                    BOOL allow_empty )
{
    UINT32 i, length = lstrlenW( name );

    if (!length) return allow_empty;
    if (length < minimum || length > maximum) return FALSE;
    for (i = 0; i < length; i++)
        if (!is_ascii_identity_character( name[i] )) return FALSE;
    if (name[length - 1] == '.' ||
        reserved_dos_component( name, length ) ||
        (length >= 4 &&
         CompareStringOrdinal( name, 4, L"xn--", 4, TRUE ) == CSTR_EQUAL))
        return FALSE;
    for (i = 0; i + 5 <= length; i++)
    {
        if (name[i] == '.' &&
            CompareStringOrdinal( name + i + 1, 4, L"xn--", 4,
                                  TRUE ) == CSTR_EQUAL)
            return FALSE;
    }
    return TRUE;
}

static BOOL wstring_equal_ascii_ci( const WCHAR *string, const WCHAR *expected )
{
    return string && !lstrcmpiW( string, expected );
}

static BOOL component_equal( const WCHAR *component, UINT32 length,
                             const WCHAR *expected )
{
    UINT32 expected_length = lstrlenW( expected );

    return length == expected_length &&
           CompareStringOrdinal( component, length, expected, expected_length,
                                 TRUE ) == CSTR_EQUAL;
}

static BOOL reserved_dos_component( const WCHAR *component, UINT32 length )
{
    UINT32 base_length = 0;
    WCHAR digit;

    while (base_length < length && component[base_length] != '.')
        base_length++;
    while (base_length && component[base_length - 1] == ' ')
        base_length--;
    if (component_equal( component, base_length, L"CON" ) ||
        component_equal( component, base_length, L"PRN" ) ||
        component_equal( component, base_length, L"AUX" ) ||
        component_equal( component, base_length, L"NUL" ) ||
        component_equal( component, base_length, L"CLOCK$" ) ||
        component_equal( component, base_length, L"CONIN$" ) ||
        component_equal( component, base_length, L"CONOUT$" ))
        return TRUE;
    if (base_length != 4 ||
        (!component_equal( component, 3, L"COM" ) &&
         !component_equal( component, 3, L"LPT" )))
        return FALSE;
    digit = component[3];
    return (digit >= '1' && digit <= '9') ||
           digit == 0x00b9 || digit == 0x00b2 || digit == 0x00b3;
}

static BOOL validate_package_relative_path( const WCHAR *path, UINT32 maximum,
                                            const WCHAR *extension )
{
    UINT32 component_start = 0, i, length;

    if (!path || !(length = lstrlenW( path )) || length > maximum ||
        path[0] == '\\' || path[0] == '/' ||
        path[length - 1] == '\\' || path[length - 1] == '/')
        return FALSE;
    if (!IsNormalizedString( NormalizationC, path, length )) return FALSE;

    for (i = 0; i <= length; i++)
    {
        WCHAR ch = i < length ? path[i] : '\\';
        UINT32 component_length;

        if (ch == '/')
            return FALSE;
        if (ch == '\\')
        {
            component_length = i - component_start;
            if (!component_length || component_length > 255 ||
                (component_length == 1 && path[component_start] == '.') ||
                (component_length == 2 && path[component_start] == '.' &&
                 path[component_start + 1] == '.') ||
                path[i - 1] == '.' || path[i - 1] == ' ' ||
                reserved_dos_component( path + component_start, component_length ) ||
                (component_length >= 4 &&
                 CompareStringOrdinal( path + component_start, 4, L"xn--", 4,
                                       TRUE ) == CSTR_EQUAL))
                return FALSE;
            component_start = i + 1;
            continue;
        }
        if (ch < 0x20 || ch == 0xfffe || ch == 0xffff ||
            ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '|' || ch == '%')
            return FALSE;
    }

    if (extension)
    {
        UINT32 extension_length = lstrlenW( extension );
        if (length < extension_length ||
            lstrcmpiW( path + length - extension_length, extension ))
            return FALSE;
    }
    return TRUE;
}

static const WCHAR *architecture_name( enum appx_manifest_architecture architecture )
{
    switch (architecture)
    {
    case APPX_MANIFEST_ARCHITECTURE_NEUTRAL: return L"neutral";
    case APPX_MANIFEST_ARCHITECTURE_X86: return L"x86";
    case APPX_MANIFEST_ARCHITECTURE_X64: return L"x64";
    case APPX_MANIFEST_ARCHITECTURE_ARM: return L"arm";
    case APPX_MANIFEST_ARCHITECTURE_ARM64: return L"arm64";
    case APPX_MANIFEST_ARCHITECTURE_X86A64: return L"x86a64";
    }
    return NULL;
}

static HRESULT parse_architecture( struct parser *parser, const xmlChar *value,
                                   enum appx_manifest_architecture *architecture )
{
    if (xml_equal( value, BAD_CAST "neutral" ))
        *architecture = APPX_MANIFEST_ARCHITECTURE_NEUTRAL;
    else if (xml_equal( value, BAD_CAST "x86" ))
        *architecture = APPX_MANIFEST_ARCHITECTURE_X86;
    else if (xml_equal( value, BAD_CAST "x64" ))
        *architecture = APPX_MANIFEST_ARCHITECTURE_X64;
    else if (xml_equal( value, BAD_CAST "arm" ))
        *architecture = APPX_MANIFEST_ARCHITECTURE_ARM;
    else if (xml_equal( value, BAD_CAST "arm64" ))
        *architecture = APPX_MANIFEST_ARCHITECTURE_ARM64;
    else if (xml_equal( value, BAD_CAST "x86a64" ))
        *architecture = APPX_MANIFEST_ARCHITECTURE_X86A64;
    else
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return S_OK;
}

static HRESULT derive_publisher_id( struct parser *parser, const WCHAR *publisher,
                                    WCHAR **result )
{
    static const WCHAR alphabet[] = L"0123456789abcdefghjkmnpqrstvwxyz";
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BYTE digest[32], *utf16le;
    WCHAR *publisher_id;
    NTSTATUS status;
    UINT32 i, j, length = lstrlenW( publisher );

    *result = NULL;
    if (!(utf16le = HeapAlloc( GetProcessHeap(), 0, length * 2 )))
        return parser_fail( parser, E_OUTOFMEMORY );
    for (i = 0; i < length; i++)
    {
        utf16le[i * 2] = publisher[i];
        utf16le[i * 2 + 1] = publisher[i] >> 8;
    }

    status = BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0 );
    if (!status)
        status = BCryptHash( algorithm, NULL, 0, utf16le, length * 2,
                             digest, sizeof(digest) );
    if (algorithm) BCryptCloseAlgorithmProvider( algorithm, 0 );
    HeapFree( GetProcessHeap(), 0, utf16le );
    if (status) return parser_fail( parser, HRESULT_FROM_NT( status ) );

    if (!(publisher_id = HeapAlloc( GetProcessHeap(), 0, 14 * sizeof(*publisher_id) )))
        return parser_fail( parser, E_OUTOFMEMORY );
    for (i = 0; i < 13; i++)
    {
        UINT32 value = 0;

        for (j = 0; j < 5; j++)
        {
            UINT32 bit = i * 5 + j;
            value <<= 1;
            if (bit < 64)
                value |= (digest[bit / 8] >> (7 - bit % 8)) & 1;
        }
        publisher_id[i] = alphabet[value];
    }
    publisher_id[13] = 0;
    *result = publisher_id;
    return S_OK;
}

static BOOL append_name( WCHAR *buffer, UINT32 capacity, UINT32 *length,
                         const WCHAR *value )
{
    UINT32 value_length = lstrlenW( value );

    if (value_length >= capacity - *length) return FALSE;
    memcpy( buffer + *length, value, value_length * sizeof(*buffer) );
    *length += value_length;
    buffer[*length] = 0;
    return TRUE;
}

static BOOL append_character( WCHAR *buffer, UINT32 capacity, UINT32 *length,
                              WCHAR value )
{
    if (*length + 1 >= capacity) return FALSE;
    buffer[(*length)++] = value;
    buffer[*length] = 0;
    return TRUE;
}

static BOOL append_uint16( WCHAR *buffer, UINT32 capacity, UINT32 *length, UINT16 value )
{
    WCHAR digits[5];
    UINT32 count = 0;

    do
    {
        digits[count++] = '0' + value % 10;
        value /= 10;
    } while (value);
    if (count >= capacity - *length) return FALSE;
    while (count) buffer[(*length)++] = digits[--count];
    buffer[*length] = 0;
    return TRUE;
}

static HRESULT make_identity_names( struct parser *parser )
{
    struct appx_manifest_identity *identity = &parser->manifest->identity;
    WCHAR full_name[128] = {0}, family_name[65] = {0};
    const WCHAR *architecture = architecture_name( identity->architecture );
    UINT32 length = 0;

    if (!append_name( family_name, ARRAY_SIZE(family_name), &length, identity->name ) ||
        !append_character( family_name, ARRAY_SIZE(family_name), &length, '_' ) ||
        !append_name( family_name, ARRAY_SIZE(family_name), &length, identity->publisher_id ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (!(identity->family_name = duplicate_wstring( family_name )))
        return parser_fail( parser, E_OUTOFMEMORY );

    length = 0;
    if (!append_name( full_name, ARRAY_SIZE(full_name), &length, identity->name ) ||
        !append_character( full_name, ARRAY_SIZE(full_name), &length, '_' ) ||
        !append_uint16( full_name, ARRAY_SIZE(full_name), &length, identity->version.major ) ||
        !append_character( full_name, ARRAY_SIZE(full_name), &length, '.' ) ||
        !append_uint16( full_name, ARRAY_SIZE(full_name), &length, identity->version.minor ) ||
        !append_character( full_name, ARRAY_SIZE(full_name), &length, '.' ) ||
        !append_uint16( full_name, ARRAY_SIZE(full_name), &length, identity->version.build ) ||
        !append_character( full_name, ARRAY_SIZE(full_name), &length, '.' ) ||
        !append_uint16( full_name, ARRAY_SIZE(full_name), &length, identity->version.revision ) ||
        !append_character( full_name, ARRAY_SIZE(full_name), &length, '_' ) ||
        !append_name( full_name, ARRAY_SIZE(full_name), &length, architecture ) ||
        !append_character( full_name, ARRAY_SIZE(full_name), &length, '_' ) ||
        !append_name( full_name, ARRAY_SIZE(full_name), &length, identity->resource_id ) ||
        !append_character( full_name, ARRAY_SIZE(full_name), &length, '_' ) ||
        !append_name( full_name, ARRAY_SIZE(full_name), &length, identity->publisher_id ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (!(identity->full_name = duplicate_wstring( full_name )))
        return parser_fail( parser, E_OUTOFMEMORY );
    return S_OK;
}

static HRESULT consume_empty_element( struct parser *parser )
{
    int depth = xmlTextReaderDepth( parser->reader ), result, type;

    if (xmlTextReaderIsEmptyElement( parser->reader )) return S_OK;
    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            return S_OK;
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );
}

static HRESULT parse_simple_text( struct parser *parser, UINT32 maximum, WCHAR **result )
{
    BYTE *buffer = NULL, *resized;
    UINT32 capacity = 0, length = 0;
    int depth = xmlTextReaderDepth( parser->reader ), read_result, type;
    HRESULT hr = S_OK;

    *result = NULL;
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return xml_to_wstring( parser, BAD_CAST "", 0, maximum, result );

    while ((read_result = read_node( parser )) == 1)
    {
        const xmlChar *value;
        UINT32 bytes;

        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            break;
        if (type == XML_READER_TYPE_COMMENT) continue;
        if (type != XML_READER_TYPE_TEXT && type != XML_READER_TYPE_WHITESPACE &&
            type != XML_READER_TYPE_SIGNIFICANT_WHITESPACE)
        {
            hr = parser_fail( parser, APPX_E_INVALID_MANIFEST );
            goto done;
        }
        value = empty_xml_string( xmlTextReaderConstValue( parser->reader ) );
        bytes = xmlStrlen( value );
        if (bytes > XML_MAX_VALUE_BYTES - length)
        {
            hr = parser_fail( parser, APPX_E_INVALID_MANIFEST );
            goto done;
        }
        if (length + bytes + 1 > capacity)
        {
            UINT32 new_capacity = capacity ? capacity : 64;
            while (new_capacity < length + bytes + 1)
            {
                if (new_capacity > XML_MAX_VALUE_BYTES / 2)
                {
                    new_capacity = XML_MAX_VALUE_BYTES + 1;
                    break;
                }
                new_capacity *= 2;
            }
            if (buffer)
                resized = HeapReAlloc( GetProcessHeap(), 0, buffer, new_capacity );
            else
                resized = HeapAlloc( GetProcessHeap(), 0, new_capacity );
            if (!resized)
            {
                hr = parser_fail( parser, E_OUTOFMEMORY );
                goto done;
            }
            buffer = resized;
            capacity = new_capacity;
        }
        memcpy( buffer + length, value, bytes );
        length += bytes;
    }
    if (read_result != 1)
    {
        hr = FAILED(parser->hr) ? parser->hr :
             parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (!buffer)
    {
        if (!(buffer = HeapAlloc( GetProcessHeap(), 0, 1 )))
        {
            hr = parser_fail( parser, E_OUTOFMEMORY );
            goto done;
        }
    }
    buffer[length] = 0;
    hr = xml_to_wstring( parser, buffer, 0, maximum, result );

done:
    HeapFree( GetProcessHeap(), 0, buffer );
    return hr;
}

static HRESULT skip_current_element( struct parser *parser )
{
    int depth = xmlTextReaderDepth( parser->reader ), result;

    if (xmlTextReaderIsEmptyElement( parser->reader )) return S_OK;
    while ((result = read_node( parser )) == 1)
    {
        if (xmlTextReaderNodeType( parser->reader ) == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            return S_OK;
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );
}

static HRESULT handle_unknown_element( struct parser *parser, BOOL semantic )
{
    const xmlChar *uri = empty_xml_string(
        xmlTextReaderConstNamespaceUri( parser->reader ) );
    const xmlChar *prefix = empty_xml_string(
        xmlTextReaderConstPrefix( parser->reader ) );

    if (xml_equal( uri, xinclude_namespace ) ||
        !*uri || xml_equal( uri, parser->foundation_uri ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (prefix_is_ignorable( parser, prefix, uri ))
    {
        if (semantic && FAILED(add_unsupported_reason(
                parser, APPX_MANIFEST_UNSUPPORTED_IGNORABLE_CONTENT )))
            return parser->hr;
    }
    else if (FAILED(add_unsupported_reason(
                 parser, APPX_MANIFEST_UNSUPPORTED_UNKNOWN_NAMESPACE )))
        return parser->hr;
    return skip_current_element( parser );
}

static BOOL valid_namespace_prefix( const xmlChar *prefix, UINT32 length )
{
    UINT32 i;

    if (!length || !((prefix[0] >= 'A' && prefix[0] <= 'Z') ||
                     (prefix[0] >= 'a' && prefix[0] <= 'z') ||
                     prefix[0] == '_'))
        return FALSE;
    for (i = 1; i < length; i++)
    {
        xmlChar ch = prefix[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
            return FALSE;
    }
    return TRUE;
}

static struct xml_attribute *find_namespace_declaration(
    struct xml_attributes *attributes, const xmlChar *prefix, UINT32 prefix_length )
{
    UINT32 i;

    for (i = 0; i < attributes->count; i++)
    {
        struct xml_attribute *attribute = attributes->item + i;

        if (!attribute->namespace_declaration ||
            xmlStrlen( attribute->local ) != prefix_length)
            continue;
        if (!memcmp( attribute->local, prefix, prefix_length ))
            return attribute;
    }
    return NULL;
}

static HRESULT parse_ignorable_namespaces( struct parser *parser,
                                           struct xml_attributes *attributes,
                                           const xmlChar *value )
{
    const xmlChar *cursor = value;

    while (*cursor)
    {
        const xmlChar *start;
        struct xml_attribute *declaration;
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
        if (length > XML_MAX_PREFIX_BYTES ||
            !valid_namespace_prefix( start, length ) ||
            parser->ignorable_count >= XML_MAX_IGNORABLE_NAMESPACES ||
            !(declaration = find_namespace_declaration(
                  attributes, start, length )) ||
            !*declaration->value ||
            xml_equal( declaration->value, parser->foundation_uri ) ||
            xml_equal( declaration->value, xmlns_namespace ))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );

        for (i = 0; i < parser->ignorable_count; i++)
        {
            if ((xmlStrlen( parser->ignorable[i].prefix ) == length &&
                 !memcmp( parser->ignorable[i].prefix, start, length )) ||
                xml_equal( parser->ignorable[i].uri, declaration->value ))
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        }

        parser->ignorable[parser->ignorable_count].prefix =
            xmlStrndup( start, length );
        parser->ignorable[parser->ignorable_count].uri =
            xmlStrdup( declaration->value );
        if (!parser->ignorable[parser->ignorable_count].prefix ||
            !parser->ignorable[parser->ignorable_count].uri)
        {
            xmlFree( parser->ignorable[parser->ignorable_count].prefix );
            xmlFree( parser->ignorable[parser->ignorable_count].uri );
            memset( parser->ignorable + parser->ignorable_count, 0,
                    sizeof(parser->ignorable[parser->ignorable_count]) );
            return parser_fail( parser, E_OUTOFMEMORY );
        }
        parser->ignorable_count++;
    }
    return S_OK;
}

static HRESULT parse_identity_element( struct parser *parser )
{
    struct appx_manifest_identity *identity = &parser->manifest->identity;
    struct xml_attributes attributes;
    struct xml_attribute *name, *publisher, *version, *architecture, *resource_id;
    HRESULT hr;

    if (parser->identity_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    parser->identity_seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    name = publisher = version = architecture = resource_id = NULL;
    if (FAILED(require_attribute( parser, &attributes, BAD_CAST "", "Name", &name )) ||
        FAILED(require_attribute( parser, &attributes, BAD_CAST "", "Publisher",
                                  &publisher )) ||
        FAILED(require_attribute( parser, &attributes, BAD_CAST "", "Version",
                                  &version )))
        goto done;
    architecture = take_attribute( &attributes, BAD_CAST "",
                                   "ProcessorArchitecture" );
    resource_id = take_attribute( &attributes, BAD_CAST "", "ResourceId" );
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;

    if (FAILED(xml_to_wstring( parser, name->value, 3, 50,
                               (WCHAR **)&identity->name )) ||
        !validate_identity_name( identity->name, 3, 50, FALSE ))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (FAILED(xml_to_wstring( parser, publisher->value, 1, 8192,
                               (WCHAR **)&identity->publisher )))
        goto done;
    if (resource_id)
    {
        if (FAILED(xml_to_wstring( parser, resource_id->value,
                                   parser->windows8_schema ? 1 : 0, 30,
                                   (WCHAR **)&identity->resource_id )) ||
            !validate_identity_name( identity->resource_id, 1, 30,
                                     !parser->windows8_schema ))
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            goto done;
        }
    }
    else if (!(identity->resource_id = duplicate_wstring( L"" )))
    {
        parser_fail( parser, E_OUTOFMEMORY );
        goto done;
    }
    identity->architecture = APPX_MANIFEST_ARCHITECTURE_NEUTRAL;
    parser->architecture_seen = architecture != NULL;
    if (FAILED(parse_version( parser, version->value, &identity->version )) ||
        (architecture &&
         FAILED(parse_architecture( parser, architecture->value,
                                    &identity->architecture ))))
        goto done;
    if (parser->windows8_schema &&
        (identity->architecture == APPX_MANIFEST_ARCHITECTURE_ARM64 ||
         identity->architecture == APPX_MANIFEST_ARCHITECTURE_X86A64))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (FAILED(derive_publisher_id( parser, identity->publisher,
                                    (WCHAR **)&identity->publisher_id )) ||
        FAILED(make_identity_names( parser )) ||
        FAILED(consume_empty_element( parser )))
        goto done;

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_simple_boolean_element( struct parser *parser, BOOL *result )
{
    struct xml_attributes attributes;
    WCHAR *text = NULL;
    HRESULT hr;

    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    if (FAILED(parse_simple_text( parser, 5, &text ))) goto done;
    parse_boolean_value( parser, text, result );

done:
    HeapFree( GetProcessHeap(), 0, text );
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_required_property( struct parser *parser, BOOL *seen,
                                        BOOL package_path )
{
    struct xml_attributes attributes;
    WCHAR *text = NULL;
    HRESULT hr;

    if (*seen) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    *seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    if (FAILED(parse_simple_text( parser, 256, &text ))) goto done;
    if (!text[0] ||
        (package_path && !validate_package_relative_path( text, 256, NULL )))
        parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    HeapFree( GetProcessHeap(), 0, text );
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT finish_properties( struct parser *parser )
{
    if (!parser->display_name_seen || !parser->publisher_display_name_seen ||
        !parser->logo_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    return S_OK;
}

static HRESULT parse_properties_element( struct parser *parser )
{
    struct xml_attributes attributes;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    HRESULT hr;

    if (parser->properties_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    parser->properties_seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            return finish_properties( parser );
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );

        if (foundation_node_is( parser, "DisplayName" ))
        {
            if (FAILED(parse_required_property(
                    parser, &parser->display_name_seen, FALSE )))
                return parser->hr;
        }
        else if (foundation_node_is( parser, "PublisherDisplayName" ))
        {
            if (FAILED(parse_required_property(
                    parser, &parser->publisher_display_name_seen, FALSE )))
                return parser->hr;
        }
        else if (foundation_node_is( parser, "Logo" ))
        {
            if (FAILED(parse_required_property(
                    parser, &parser->logo_seen, TRUE )))
                return parser->hr;
        }
        else if (foundation_node_is( parser, "Framework" ))
        {
            if (parser->framework_seen)
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
            parser->framework_seen = TRUE;
            if (FAILED(parse_simple_boolean_element(
                    parser, &parser->manifest->framework )))
                return parser->hr;
        }
        else if (!parser->windows8_schema &&
                 foundation_node_is( parser, "ResourcePackage" ))
        {
            if (parser->resource_package_seen)
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
            parser->resource_package_seen = TRUE;
            if (FAILED(parse_simple_boolean_element(
                    parser, &parser->manifest->resource_package )))
                return parser->hr;
        }
        else if (foundation_node_is( parser, "Description" ))
        {
            if (FAILED(skip_current_element( parser ))) return parser->hr;
        }
        else if (FAILED(handle_unknown_element( parser, TRUE )))
            return parser->hr;
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_resource_element( struct parser *parser )
{
    struct xml_attributes attributes;
    struct xml_attribute *language, *scale = NULL, *dx_feature = NULL;
    WCHAR *value = NULL;
    HRESULT hr;

    if (parser->resource_count >= XML_MAX_RESOURCES)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    language = take_attribute( &attributes, BAD_CAST "", "Language" );
    if (!parser->windows8_schema)
    {
        scale = take_attribute( &attributes, BAD_CAST "", "Scale" );
        dx_feature = take_attribute( &attributes, BAD_CAST "",
                                     "DXFeatureLevel" );
    }
    if ((!language && !scale && !dx_feature) ||
        (parser->windows8_schema && !language))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (FAILED(finish_attributes( parser, &attributes, FALSE )))
        goto done;
    if (language)
    {
        parser->resource_qualifier_types |= RESOURCE_QUALIFIER_LANGUAGE;
        if (FAILED(xml_to_wstring( parser, language->value, 1, 85, &value )))
            goto done;
        HeapFree( GetProcessHeap(), 0, value );
        value = NULL;
    }
    if (scale)
    {
        parser->resource_qualifier_types |= RESOURCE_QUALIFIER_SCALE;
        if (FAILED(xml_to_wstring( parser, scale->value, 1, 16, &value )))
            goto done;
        HeapFree( GetProcessHeap(), 0, value );
        value = NULL;
    }
    if (dx_feature)
    {
        parser->resource_qualifier_types |= RESOURCE_QUALIFIER_DX_FEATURE;
        if (FAILED(xml_to_wstring( parser, dx_feature->value, 1, 32, &value )))
            goto done;
    }
    if (!!language + !!scale + !!dx_feature != 1)
        parser->mixed_resource_qualifier = TRUE;
    if (FAILED(consume_empty_element( parser ))) goto done;
    parser->resource_count++;

done:
    HeapFree( GetProcessHeap(), 0, value );
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_resources_element( struct parser *parser )
{
    struct xml_attributes attributes;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    HRESULT hr;

    if (parser->resources_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    parser->resources_seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, FALSE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser->windows8_schema ?
               parser_fail( parser, APPX_E_INVALID_MANIFEST ) : S_OK;

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            return !parser->windows8_schema || parser->resource_count ?
                   S_OK : parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (foundation_node_is( parser, "Resource" ))
        {
            if (FAILED(parse_resource_element( parser ))) return parser->hr;
        }
        else if (FAILED(handle_unknown_element( parser, FALSE )))
            return parser->hr;
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_os_version( struct parser *parser, const WCHAR *text,
                                 UINT16 version[3] )
{
    const WCHAR *cursor = text;
    UINT32 part;

    memset( version, 0, 3 * sizeof(*version) );
    for (part = 0; part < 3; part++)
    {
        UINT32 digits = 0, value = 0;

        if (*cursor < '0' || *cursor > '9')
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9')
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        while (*cursor >= '0' && *cursor <= '9')
        {
            value = value * 10 + (*cursor++ - '0');
            if (++digits > 5 || value > 0xffff)
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        }
        version[part] = value;
        if (!*cursor)
            return part >= 1 ? S_OK :
                   parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (*cursor++ != '.' || part == 2)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    return parser_fail( parser, APPX_E_INVALID_MANIFEST );
}

static HRESULT parse_os_version_element( struct parser *parser, BOOL *seen,
                                         UINT16 version[3] )
{
    struct xml_attributes attributes;
    WCHAR *text = NULL;
    HRESULT hr;

    if (*seen) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    *seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    if (FAILED(parse_simple_text( parser, 17, &text ))) goto done;
    parse_os_version( parser, text, version );

done:
    HeapFree( GetProcessHeap(), 0, text );
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_prerequisites_element( struct parser *parser )
{
    struct xml_attributes attributes;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    UINT32 i;
    HRESULT hr;

    if (parser->prerequisites_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    parser->prerequisites_seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
        {
            if (!parser->os_min_version_seen ||
                !parser->os_max_version_seen)
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
            for (i = 0; i < 3; i++)
            {
                if (parser->os_min_version[i] <
                    parser->os_max_version[i])
                    return S_OK;
                if (parser->os_min_version[i] >
                    parser->os_max_version[i])
                    return parser_fail( parser, APPX_E_INVALID_MANIFEST );
            }
            return S_OK;
        }
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (foundation_node_is( parser, "OSMinVersion" ))
        {
            if (FAILED(parse_os_version_element(
                    parser, &parser->os_min_version_seen,
                    parser->os_min_version )))
                return parser->hr;
        }
        else if (foundation_node_is( parser, "OSMaxVersionTested" ))
        {
            if (FAILED(parse_os_version_element(
                    parser, &parser->os_max_version_seen,
                    parser->os_max_version )))
                return parser->hr;
        }
        else
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static struct appx_manifest_dependency *append_dependency( struct parser *parser )
{
    APPX_MANIFEST *manifest = parser->manifest;
    struct appx_manifest_dependency *dependencies;
    UINT32 count;

    if (manifest->dependency_count >= APPX_MANIFEST_MAX_DEPENDENCIES)
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return NULL;
    }
    count = manifest->dependency_count + 1;
    if (manifest->dependencies)
        dependencies = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    manifest->dependencies,
                                    count * sizeof(*dependencies) );
    else
        dependencies = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  count * sizeof(*dependencies) );
    if (!dependencies)
    {
        parser_fail( parser, E_OUTOFMEMORY );
        return NULL;
    }
    manifest->dependencies = dependencies;
    return dependencies + manifest->dependency_count++;
}

static BOOL dependency_is_duplicate( const APPX_MANIFEST *manifest,
                                     const struct appx_manifest_dependency *dependency )
{
    UINT32 i;

    for (i = 0; i + 1 < manifest->dependency_count; i++)
    {
        const struct appx_manifest_dependency *other = manifest->dependencies + i;

        if (!lstrcmpiW( other->name, dependency->name ))
            return TRUE;
    }
    return FALSE;
}

static HRESULT parse_package_dependency_element( struct parser *parser )
{
    struct appx_manifest_dependency *dependency;
    struct xml_attributes attributes;
    struct xml_attribute *name, *publisher, *min_version, *max_major, *optional;
    HRESULT hr;

    if (!(dependency = append_dependency( parser ))) return parser->hr;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    name = publisher = min_version = max_major = optional = NULL;
    if (FAILED(require_attribute( parser, &attributes, BAD_CAST "", "Name", &name )))
        goto done;
    min_version = take_attribute( &attributes, BAD_CAST "", "MinVersion" );
    if (!min_version && !parser->windows8_schema)
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    publisher = take_attribute( &attributes, BAD_CAST "", "Publisher" );
    if (!parser->windows8_schema)
        max_major = take_attribute( &attributes, BAD_CAST "",
                                    "MaxMajorVersionTested" );
    if (!parser->windows8_schema)
        optional = take_attribute( &attributes, uap6_namespace, "Optional" );
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;

    if (FAILED(xml_to_wstring( parser, name->value, 3, 50,
                               (WCHAR **)&dependency->name )) ||
        !validate_identity_name( dependency->name, 3, 50, FALSE ) ||
        (min_version &&
         FAILED(parse_version( parser, min_version->value,
                               &dependency->min_version ))))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (publisher)
    {
        if (FAILED(xml_to_wstring( parser, publisher->value, 1, 8192,
                                   (WCHAR **)&dependency->publisher )))
            goto done;
    }
    else
    {
        if (!(dependency->publisher = duplicate_wstring( L"" )))
        {
            parser_fail( parser, E_OUTOFMEMORY );
            goto done;
        }
        if (FAILED(add_unsupported_reason(
                parser, APPX_MANIFEST_UNSUPPORTED_UNSIGNED_DEPENDENCY )))
            goto done;
    }
    if (max_major)
    {
        if (FAILED(parse_uint16( parser, max_major->value,
                                 &dependency->max_major_version_tested )))
            goto done;
        dependency->has_max_major_version_tested = TRUE;
    }
    if (optional)
    {
        WCHAR *value = NULL;

        if (FAILED(xml_to_wstring( parser, optional->value, 1, 5, &value )))
            goto done;
        hr = parse_boolean_value( parser, value, &dependency->optional );
        HeapFree( GetProcessHeap(), 0, value );
        if (FAILED(hr)) goto done;
        if (dependency->optional &&
            FAILED(add_unsupported_reason(
                parser, APPX_MANIFEST_UNSUPPORTED_OPTIONAL_DEPENDENCY )))
            goto done;
    }
    if (dependency_is_duplicate( parser->manifest, dependency ))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    consume_empty_element( parser );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static struct appx_manifest_target_family *append_target_family( struct parser *parser )
{
    APPX_MANIFEST *manifest = parser->manifest;
    struct appx_manifest_target_family *families;
    UINT32 count;

    if (manifest->target_family_count >= APPX_MANIFEST_MAX_TARGET_FAMILIES)
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return NULL;
    }
    count = manifest->target_family_count + 1;
    if (manifest->target_families)
        families = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                manifest->target_families,
                                count * sizeof(*families) );
    else
        families = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                              count * sizeof(*families) );
    if (!families)
    {
        parser_fail( parser, E_OUTOFMEMORY );
        return NULL;
    }
    manifest->target_families = families;
    return families + manifest->target_family_count++;
}

static BOOL target_family_is_duplicate( const APPX_MANIFEST *manifest,
                                        const struct appx_manifest_target_family *family )
{
    UINT32 i;

    for (i = 0; i + 1 < manifest->target_family_count; i++)
        if (!lstrcmpiW( manifest->target_families[i].name, family->name ))
            return TRUE;
    return FALSE;
}

static HRESULT parse_target_family_element( struct parser *parser )
{
    struct appx_manifest_target_family *family;
    struct xml_attributes attributes;
    struct xml_attribute *name, *min_version, *max_version;
    HRESULT hr;

    if (!(family = append_target_family( parser ))) return parser->hr;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    name = min_version = max_version = NULL;
    if (FAILED(require_attribute( parser, &attributes, BAD_CAST "", "Name", &name )) ||
        FAILED(require_attribute( parser, &attributes, BAD_CAST "", "MinVersion",
                                  &min_version )))
        goto done;
    if (FAILED(require_attribute( parser, &attributes, BAD_CAST "",
                                  "MaxVersionTested", &max_version )))
        goto done;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    if (FAILED(xml_to_wstring( parser, name->value, 1, 256,
                               (WCHAR **)&family->name )) ||
        FAILED(parse_version( parser, min_version->value, &family->min_version )))
        goto done;
    if (FAILED(parse_version( parser, max_version->value,
                              &family->max_version_tested )))
        goto done;
    family->has_max_version_tested = TRUE;
    if (target_family_is_duplicate( parser->manifest, family ))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (lstrcmpiW( family->name, L"Windows.Desktop" ) &&
        lstrcmpiW( family->name, L"Windows.Universal" ))
    {
        if (FAILED(add_unsupported_reason(
                parser, APPX_MANIFEST_UNSUPPORTED_TARGET_DEVICE_FAMILY )))
            goto done;
    }
    consume_empty_element( parser );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_dependencies_element( struct parser *parser )
{
    struct xml_attributes attributes;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    HRESULT hr;

    if (parser->dependencies_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    parser->dependencies_seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            return (parser->windows8_schema ?
                    parser->manifest->dependency_count :
                    parser->manifest->target_family_count) ? S_OK :
                   parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );

        if (foundation_node_is( parser, "PackageDependency" ))
        {
            if (FAILED(parse_package_dependency_element( parser )))
                return parser->hr;
        }
        else if (!parser->windows8_schema &&
                 foundation_node_is( parser, "TargetDeviceFamily" ))
        {
            if (FAILED(parse_target_family_element( parser )))
                return parser->hr;
        }
        else if (FAILED(handle_unknown_element( parser, TRUE )))
            return parser->hr;
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_capability_element( struct parser *parser, BOOL restricted )
{
    struct xml_attributes attributes;
    struct xml_attribute *name;
    HRESULT hr;

    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    name = NULL;
    if (FAILED(require_attribute( parser, &attributes, BAD_CAST "", "Name", &name )))
        goto done;
    /*
     * Capabilities are permissions.  In particular runFullTrust is not an
     * activation selector; application attributes determine activation.
     */
    if (restricted && xml_equal( name->value, BAD_CAST "runFullTrust" ))
    {
        if (parser->manifest->run_full_trust)
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            goto done;
        }
        parser->manifest->run_full_trust = TRUE;
    }
    if (FAILED(finish_attributes( parser, &attributes, FALSE ))) goto done;
    consume_empty_element( parser );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_capabilities_element( struct parser *parser )
{
    struct xml_attributes attributes;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    HRESULT hr;

    if (parser->capabilities_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    parser->capabilities_seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, FALSE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader )) return S_OK;

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            return S_OK;
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );

        if (foundation_node_is( parser, "Capability" ) ||
            foundation_node_is( parser, "DeviceCapability" ))
        {
            if (FAILED(parse_capability_element( parser, FALSE )))
                return parser->hr;
        }
        else if (!parser->windows8_schema &&
                 node_is( parser, rescap_namespace, "Capability" ))
        {
            if (FAILED(parse_capability_element( parser, TRUE )))
                return parser->hr;
        }
        else if (FAILED(handle_unknown_element( parser, TRUE )))
            return parser->hr;
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static struct appx_manifest_inproc_class *append_inproc_class( struct parser *parser )
{
    APPX_MANIFEST *manifest = parser->manifest;
    struct appx_manifest_inproc_class *classes;
    UINT32 count;

    if (manifest->class_count >= APPX_MANIFEST_MAX_INPROC_CLASSES)
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return NULL;
    }
    count = manifest->class_count + 1;
    if (manifest->classes)
        classes = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                               manifest->classes, count * sizeof(*classes) );
    else
        classes = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                             count * sizeof(*classes) );
    if (!classes)
    {
        parser_fail( parser, E_OUTOFMEMORY );
        return NULL;
    }
    manifest->classes = classes;
    return classes + manifest->class_count++;
}

static BOOL class_id_is_duplicate( const APPX_MANIFEST *manifest,
                                   const struct appx_manifest_inproc_class *class )
{
    UINT32 i;

    for (i = 0; i + 1 < manifest->class_count; i++)
        if (!lstrcmpW( manifest->classes[i].activatable_class_id,
                       class->activatable_class_id ))
            return TRUE;
    return FALSE;
}

static HRESULT parse_activatable_class_element( struct parser *parser )
{
    struct appx_manifest_inproc_class *class;
    struct xml_attributes attributes;
    struct xml_attribute *class_id, *threading_model;
    HRESULT hr;

    if (!(class = append_inproc_class( parser ))) return parser->hr;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    class_id = threading_model = NULL;
    if (FAILED(require_attribute( parser, &attributes, BAD_CAST "",
                                  "ActivatableClassId", &class_id )) ||
        FAILED(require_attribute( parser, &attributes, BAD_CAST "",
                                  "ThreadingModel", &threading_model )) ||
        FAILED(finish_attributes( parser, &attributes, TRUE )))
        goto done;
    if (FAILED(xml_to_wstring( parser, class_id->value, 1, 255,
                               (WCHAR **)&class->activatable_class_id )))
        goto done;

    if (xml_equal( threading_model->value, BAD_CAST "both" ))
        class->threading_model = APPX_MANIFEST_THREADING_BOTH;
    else if (xml_equal( threading_model->value, BAD_CAST "STA" ))
        class->threading_model = APPX_MANIFEST_THREADING_STA;
    else if (xml_equal( threading_model->value, BAD_CAST "MTA" ))
        class->threading_model = APPX_MANIFEST_THREADING_MTA;
    else
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (class_id_is_duplicate( parser->manifest, class ))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    consume_empty_element( parser );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_path_element( struct parser *parser, WCHAR **path )
{
    struct xml_attributes attributes;
    HRESULT hr;

    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    if (FAILED(parse_simple_text( parser, 256, path ))) goto done;
    if (!validate_package_relative_path( *path, 256, L".dll" ))
        parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_inproc_server_element( struct parser *parser )
{
    struct xml_attributes attributes;
    APPX_MANIFEST *manifest = parser->manifest;
    WCHAR *path = NULL;
    UINT32 start_class = manifest->class_count, i;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    BOOL path_seen = FALSE;
    HRESULT hr;

    if (++parser->inproc_server_count > XML_MAX_INPROC_SERVERS)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            break;
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            goto done_no_attrs;
        }

        if (foundation_node_is( parser, "Path" ))
        {
            if (path_seen)
            {
                parser_fail( parser, APPX_E_INVALID_MANIFEST );
                goto done_no_attrs;
            }
            path_seen = TRUE;
            if (FAILED(parse_path_element( parser, &path ))) goto done_no_attrs;
        }
        else if (foundation_node_is( parser, "ActivatableClass" ))
        {
            if (manifest->class_count - start_class >= XML_MAX_CLASSES_PER_SERVER)
            {
                parser_fail( parser, APPX_E_INVALID_MANIFEST );
                goto done_no_attrs;
            }
            if (FAILED(parse_activatable_class_element( parser ))) goto done_no_attrs;
        }
        else if (FAILED(handle_unknown_element( parser, TRUE )))
            goto done_no_attrs;
    }
    if (result != 1 || !path_seen || manifest->class_count == start_class)
    {
        if (SUCCEEDED(parser->hr)) parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done_no_attrs;
    }
    parser->executable_code_seen = TRUE;
    for (i = start_class; i < manifest->class_count; i++)
    {
        if (!(manifest->classes[i].path = duplicate_wstring( path )))
        {
            parser_fail( parser, E_OUTOFMEMORY );
            goto done_no_attrs;
        }
    }
    HeapFree( GetProcessHeap(), 0, path );
    return S_OK;

done:
    free_attributes( &attributes );
done_no_attrs:
    HeapFree( GetProcessHeap(), 0, path );
    return parser->hr;
}

static HRESULT parse_extension_element( struct parser *parser )
{
    struct xml_attributes attributes;
    struct xml_attribute *category;
    HRESULT hr;

    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    category = NULL;
    if (FAILED(require_attribute( parser, &attributes, BAD_CAST "", "Category",
                                  &category )))
        goto done;

    if (xml_equal( category->value,
                   BAD_CAST "windows.activatableClass.inProcessServer" ))
    {
        if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
        free_attributes( &attributes );

        if (xmlTextReaderIsEmptyElement( parser->reader ))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        {
            int depth = xmlTextReaderDepth( parser->reader ), result, type;
            BOOL server_seen = FALSE;

            while ((result = read_node( parser )) == 1)
            {
                type = xmlTextReaderNodeType( parser->reader );
                if (type == XML_READER_TYPE_END_ELEMENT &&
                    xmlTextReaderDepth( parser->reader ) == depth)
                    break;
                if (type == XML_READER_TYPE_COMMENT ||
                    current_text_is_whitespace( parser ))
                    continue;
                if (type != XML_READER_TYPE_ELEMENT ||
                    xmlTextReaderDepth( parser->reader ) != depth + 1)
                    return parser_fail( parser, APPX_E_INVALID_MANIFEST );
                if (foundation_node_is( parser, "InProcessServer" ))
                {
                    if (server_seen)
                        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
                    server_seen = TRUE;
                    if (FAILED(parse_inproc_server_element( parser )))
                        return parser->hr;
                }
                else if (FAILED(handle_unknown_element( parser, TRUE )))
                    return parser->hr;
            }
            if (result != 1 || !server_seen)
                return FAILED(parser->hr) ? parser->hr :
                       parser_fail( parser, APPX_E_INVALID_MANIFEST );
        }
        return S_OK;
    }

    if (xml_equal( category->value,
                   BAD_CAST "windows.activatableClass.outOfProcessServer" ))
        hr = add_unsupported_reason(
            parser, APPX_MANIFEST_UNSUPPORTED_OUT_OF_PROCESS_SERVER );
    else
        hr = add_unsupported_reason( parser, APPX_MANIFEST_UNSUPPORTED_EXTENSION );
    if (SUCCEEDED(hr)) hr = skip_current_element( parser );

done:
    free_attributes( &attributes );
    return FAILED(parser->hr) ? parser->hr : hr;
}

static HRESULT parse_extensions_element( struct parser *parser, BOOL package_level )
{
    struct xml_attributes attributes;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    HRESULT hr;

    if (package_level)
    {
        if (parser->extensions_seen)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        parser->extensions_seen = TRUE;
    }
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader )) return S_OK;

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            return S_OK;
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (foundation_node_is( parser, "Extension" ))
        {
            if (FAILED(parse_extension_element( parser ))) return parser->hr;
        }
        else if (FAILED(handle_unknown_element( parser, TRUE )))
            return parser->hr;
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static struct appx_manifest_application *append_application( struct parser *parser )
{
    APPX_MANIFEST *manifest = parser->manifest;
    struct appx_manifest_application *applications;
    UINT32 count;

    if (manifest->application_count >= APPX_MANIFEST_MAX_APPLICATIONS)
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return NULL;
    }
    count = manifest->application_count + 1;
    if (manifest->applications)
        applications = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    manifest->applications,
                                    count * sizeof(*applications) );
    else
        applications = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  count * sizeof(*applications) );
    if (!applications)
    {
        parser_fail( parser, E_OUTOFMEMORY );
        return NULL;
    }
    manifest->applications = applications;
    return applications + manifest->application_count++;
}

static BOOL application_id_is_duplicate( const APPX_MANIFEST *manifest,
                                         const struct appx_manifest_application *application )
{
    UINT32 i;

    for (i = 0; i + 1 < manifest->application_count; i++)
        if (!lstrcmpiW( manifest->applications[i].id, application->id ))
            return TRUE;
    return FALSE;
}

static BOOL valid_application_id( const WCHAR *id )
{
    UINT32 i, length = lstrlenW( id );

    if (!length || length > 64) return FALSE;
    for (i = 0; i < length; i++)
    {
        WCHAR ch = id[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-'))
            return FALSE;
    }
    return TRUE;
}

static BOOL valid_windows8_application_id( const WCHAR *id )
{
    UINT32 component_start = 0, i, length = lstrlenW( id );

    if (!length || length > 64) return FALSE;
    for (i = 0; i <= length; i++)
    {
        WCHAR ch = i < length ? id[i] : '.';
        UINT32 component_length;

        if (ch == '.')
        {
            component_length = i - component_start;
            if (!component_length ||
                reserved_dos_component( id + component_start,
                                        component_length ))
                return FALSE;
            component_start = i + 1;
            continue;
        }
        if (i == component_start)
        {
            if (!((ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z')))
                return FALSE;
        }
        else if (!((ch >= 'A' && ch <= 'Z') ||
                   (ch >= 'a' && ch <= 'z') ||
                   (ch >= '0' && ch <= '9')))
            return FALSE;
    }
    return TRUE;
}

static HRESULT determine_application_activation(
    struct parser *parser, struct appx_manifest_application *application )
{
    BOOL legacy = wstring_equal_ascii_ci(
        application->entry_point, L"windows.fullTrustApplication" );
    BOOL runtime = application->runtime_behavior != NULL;
    BOOL trust = application->trust_level != NULL;

    /*
     * The Windows 8 schema describes AppContainer applications only.  A
     * future namespace or an entry-point spelling must never promote one to
     * the packaged-desktop activation path.
     */
    if (parser->windows8_schema)
    {
        application->activation_kind = APPX_MANIFEST_ACTIVATION_UNSUPPORTED;
        return add_unsupported_reason(
            parser, APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION );
    }

    if (application->entry_point)
    {
        if (legacy)
        {
            if (runtime || trust)
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
            application->activation_kind = APPX_MANIFEST_ACTIVATION_FULL_TRUST;
            return S_OK;
        }
        if (runtime)
        {
            if (lstrcmpW( application->runtime_behavior, L"windowsApp" ) ||
                (trust && lstrcmpW( application->trust_level, L"appContainer" )))
                return parser_fail( parser, APPX_E_INVALID_MANIFEST );
            application->activation_kind = APPX_MANIFEST_ACTIVATION_UNSUPPORTED;
            if (FAILED(add_unsupported_reason(
                    parser, APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION )))
                return parser->hr;
            return add_unsupported_reason(
                parser, APPX_MANIFEST_UNSUPPORTED_APPCONTAINER );
        }
        if (trust) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        application->activation_kind = APPX_MANIFEST_ACTIVATION_UNSUPPORTED;
        return add_unsupported_reason(
            parser, APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION );
    }

    if (!runtime)
    {
        if (trust) return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        application->activation_kind = APPX_MANIFEST_ACTIVATION_UNSUPPORTED;
        return add_unsupported_reason(
            parser, APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION );
    }
    if (!trust) return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    if (!lstrcmpW( application->trust_level, L"appContainer" ))
    {
        application->activation_kind = APPX_MANIFEST_ACTIVATION_UNSUPPORTED;
        if (lstrcmpW( application->runtime_behavior, L"windowsApp" ) &&
            lstrcmpW( application->runtime_behavior, L"appContainer" ))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        return add_unsupported_reason(
            parser, APPX_MANIFEST_UNSUPPORTED_APPCONTAINER );
    }
    if (lstrcmpW( application->trust_level, L"mediumIL" ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (!lstrcmpW( application->runtime_behavior, L"packagedClassicApp" ))
        application->activation_kind =
            APPX_MANIFEST_ACTIVATION_PACKAGED_CLASSIC;
    else if (!lstrcmpW( application->runtime_behavior, L"win32App" ))
        application->activation_kind = APPX_MANIFEST_ACTIVATION_WIN32;
    else
    {
        application->activation_kind = APPX_MANIFEST_ACTIVATION_UNSUPPORTED;
        if (!lstrcmpW( application->runtime_behavior, L"windowsApp" ) ||
            !lstrcmpW( application->runtime_behavior, L"appContainer" ))
            return add_unsupported_reason(
                parser, APPX_MANIFEST_UNSUPPORTED_APPCONTAINER );
        return add_unsupported_reason(
            parser, APPX_MANIFEST_UNSUPPORTED_RUNTIME_BEHAVIOR );
    }
    return S_OK;
}

static HRESULT parse_application_element( struct parser *parser )
{
    struct appx_manifest_application *application;
    struct xml_attributes attributes;
    struct xml_attribute *id, *executable, *entry_point, *runtime_behavior;
    struct xml_attribute *trust_level, *parameters, *current_directory, *start_page;
    WCHAR *start_page_path = NULL;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    BOOL extensions_seen = FALSE, visual_elements_seen = FALSE;
    HRESULT hr;

    if (!(application = append_application( parser ))) return parser->hr;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    id = executable = entry_point = runtime_behavior = trust_level = NULL;
    parameters = current_directory = start_page = NULL;
    if (FAILED(require_attribute( parser, &attributes, BAD_CAST "", "Id", &id )))
        goto done;
    executable = take_attribute( &attributes, BAD_CAST "", "Executable" );
    entry_point = take_attribute( &attributes, BAD_CAST "", "EntryPoint" );
    if (!parser->windows8_schema)
    {
        runtime_behavior = take_attribute( &attributes, uap10_namespace,
                                           "RuntimeBehavior" );
        trust_level = take_attribute( &attributes, uap10_namespace,
                                      "TrustLevel" );
        parameters = take_attribute( &attributes, uap11_namespace,
                                     "Parameters" );
        current_directory = take_attribute( &attributes, uap11_namespace,
                                            "CurrentDirectoryPath" );
    }
    start_page = take_attribute( &attributes, BAD_CAST "", "StartPage" );
    if ((start_page &&
         (executable || entry_point || runtime_behavior || trust_level)) ||
        (!start_page && !executable) ||
        (parser->windows8_schema && !start_page && !entry_point))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;

    if (FAILED(xml_to_wstring( parser, id->value, 1, 64,
                               (WCHAR **)&application->id )) ||
        !(parser->windows8_schema ?
          valid_windows8_application_id( application->id ) :
          valid_application_id( application->id )))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (executable &&
        (FAILED(xml_to_wstring( parser, executable->value, 1, 256,
                                (WCHAR **)&application->executable )) ||
         !validate_package_relative_path(
             application->executable, 256, L".exe" )))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (executable) parser->executable_code_seen = TRUE;
    if (start_page &&
        (FAILED(xml_to_wstring( parser, start_page->value, 1, 256,
                                &start_page_path )) ||
         !validate_package_relative_path( start_page_path, 256, NULL )))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (entry_point &&
        FAILED(xml_to_wstring( parser, entry_point->value, 1, 256,
                               (WCHAR **)&application->entry_point )))
        goto done;
    if (runtime_behavior &&
        FAILED(xml_to_wstring( parser, runtime_behavior->value, 1, 64,
                               (WCHAR **)&application->runtime_behavior )))
        goto done;
    if (trust_level &&
        FAILED(xml_to_wstring( parser, trust_level->value, 1, 64,
                               (WCHAR **)&application->trust_level )))
        goto done;
    if (parameters)
    {
        if (FAILED(xml_to_wstring( parser, parameters->value, 0, 1024,
                                   (WCHAR **)&application->parameters )) ||
            FAILED(add_unsupported_reason(
                parser, APPX_MANIFEST_UNSUPPORTED_APPLICATION_PARAMETERS )))
            goto done;
    }
    if (current_directory)
    {
        if (FAILED(xml_to_wstring( parser, current_directory->value, 0, 256,
                                   (WCHAR **)&application->current_directory_path )) ||
            FAILED(add_unsupported_reason(
                parser, APPX_MANIFEST_UNSUPPORTED_CURRENT_DIRECTORY )))
            goto done;
    }
    if (application_id_is_duplicate( parser->manifest, application ))
    {
        parser_fail( parser, APPX_E_INVALID_MANIFEST );
        goto done;
    }
    if (FAILED(determine_application_activation( parser, application ))) goto done;
    free_attributes( &attributes );

    if (xmlTextReaderIsEmptyElement( parser->reader ))
    {
        hr = parser->windows8_schema ?
             parser_fail( parser, APPX_E_INVALID_MANIFEST ) : S_OK;
        goto done_no_attrs;
    }
    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
        {
            hr = !parser->windows8_schema || visual_elements_seen ?
                 S_OK : parser_fail( parser, APPX_E_INVALID_MANIFEST );
            goto done_no_attrs;
        }
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
        {
            parser_fail( parser, APPX_E_INVALID_MANIFEST );
            goto done_no_attrs;
        }

        if (foundation_node_is( parser, "Extensions" ))
        {
            if (extensions_seen)
            {
                parser_fail( parser, APPX_E_INVALID_MANIFEST );
                goto done_no_attrs;
            }
            extensions_seen = TRUE;
            if (FAILED(parse_extensions_element( parser, FALSE )))
                goto done_no_attrs;
        }
        else if ((!parser->windows8_schema &&
                  node_is( parser, uap_namespace, "VisualElements" )) ||
                 (parser->windows8_schema &&
                  foundation_node_is( parser, "VisualElements" )))
        {
            if (visual_elements_seen)
            {
                parser_fail( parser, APPX_E_INVALID_MANIFEST );
                goto done_no_attrs;
            }
            visual_elements_seen = TRUE;
            /* This subtree changes presentation, not execution semantics. */
            if (FAILED(skip_current_element( parser ))) goto done_no_attrs;
        }
        else if (parser->windows8_schema &&
                 foundation_node_is( parser, "ApplicationContentUriRules" ))
        {
            if (FAILED(skip_current_element( parser ))) goto done_no_attrs;
        }
        else if (FAILED(handle_unknown_element( parser, TRUE )))
            goto done_no_attrs;
    }
    hr = FAILED(parser->hr) ? parser->hr :
         parser_fail( parser, APPX_E_INVALID_MANIFEST );
    goto done_no_attrs;

done:
    hr = parser->hr;
    free_attributes( &attributes );
done_no_attrs:
    HeapFree( GetProcessHeap(), 0, start_page_path );
    return hr;
}

static HRESULT parse_applications_element( struct parser *parser )
{
    struct xml_attributes attributes;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    HRESULT hr;

    if (parser->applications_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    parser->applications_seen = TRUE;
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            return parser->manifest->application_count ? S_OK :
                   parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (foundation_node_is( parser, "Application" ))
        {
            if (FAILED(parse_application_element( parser ))) return parser->hr;
        }
        else if (FAILED(handle_unknown_element( parser, TRUE )))
            return parser->hr;
    }
    return FAILED(parser->hr) ? parser->hr :
           parser_fail( parser, APPX_E_INVALID_MANIFEST );

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static HRESULT parse_package_element( struct parser *parser )
{
    struct xml_attributes attributes;
    struct xml_attribute *ignorable;
    const xmlChar *namespace_uri;
    int depth = xmlTextReaderDepth( parser->reader ), result, type;
    HRESULT hr;

    namespace_uri = empty_xml_string(
        xmlTextReaderConstNamespaceUri( parser->reader ) );
    if (!xml_equal( xmlTextReaderConstLocalName( parser->reader ),
                    BAD_CAST "Package" ) ||
        depth != 0)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (xml_equal( namespace_uri, foundation_namespace ))
        parser->foundation_uri = foundation_namespace;
    else if (xml_equal( namespace_uri, windows8_namespace ))
    {
        parser->foundation_uri = windows8_namespace;
        parser->windows8_schema = TRUE;
    }
    else
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (FAILED(hr = get_attributes( parser, &attributes ))) return hr;
    ignorable = take_attribute( &attributes, BAD_CAST "", "IgnorableNamespaces" );
    if (ignorable && FAILED(parse_ignorable_namespaces(
            parser, &attributes, ignorable->value )))
        goto done;
    if (FAILED(finish_attributes( parser, &attributes, TRUE ))) goto done;
    free_attributes( &attributes );
    if (xmlTextReaderIsEmptyElement( parser->reader ))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );

    while ((result = read_node( parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser->reader );
        if (type == XML_READER_TYPE_END_ELEMENT &&
            xmlTextReaderDepth( parser->reader ) == depth)
            break;
        if (type == XML_READER_TYPE_COMMENT || current_text_is_whitespace( parser ))
            continue;
        if (type != XML_READER_TYPE_ELEMENT ||
            xmlTextReaderDepth( parser->reader ) != depth + 1)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );

        if (foundation_node_is( parser, "Identity" ))
        {
            if (FAILED(parse_identity_element( parser ))) return parser->hr;
        }
        else if (foundation_node_is( parser, "Properties" ))
        {
            if (FAILED(parse_properties_element( parser ))) return parser->hr;
        }
        else if (foundation_node_is( parser, "Resources" ))
        {
            if (FAILED(parse_resources_element( parser ))) return parser->hr;
        }
        else if (parser->windows8_schema &&
                 foundation_node_is( parser, "Prerequisites" ))
        {
            if (FAILED(parse_prerequisites_element( parser )))
                return parser->hr;
        }
        else if (foundation_node_is( parser, "Dependencies" ))
        {
            if (FAILED(parse_dependencies_element( parser ))) return parser->hr;
        }
        else if (foundation_node_is( parser, "Capabilities" ))
        {
            if (FAILED(parse_capabilities_element( parser ))) return parser->hr;
        }
        else if (foundation_node_is( parser, "Applications" ))
        {
            if (FAILED(parse_applications_element( parser ))) return parser->hr;
        }
        else if (foundation_node_is( parser, "Extensions" ))
        {
            if (FAILED(parse_extensions_element( parser, TRUE ))) return parser->hr;
        }
        else if (FAILED(handle_unknown_element( parser, TRUE )))
            return parser->hr;
    }
    if (result != 1 || !parser->identity_seen || !parser->properties_seen)
        return FAILED(parser->hr) ? parser->hr :
               parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (parser->windows8_schema)
    {
        if (!parser->resources_seen || !parser->resource_count ||
            !parser->prerequisites_seen)
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
        if (parser->manifest->framework &&
            (parser->dependencies_seen || parser->capabilities_seen ||
             parser->extensions_seen || parser->applications_seen))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    else if (parser->manifest->resource_package)
    {
        if (!parser->resources_seen || parser->architecture_seen ||
            parser->dependencies_seen || parser->capabilities_seen ||
            parser->extensions_seen || parser->applications_seen ||
            parser->manifest->framework ||
            parser->mixed_resource_qualifier ||
            !parser->resource_qualifier_types ||
            (parser->resource_qualifier_types &
             (parser->resource_qualifier_types - 1)))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    else
    {
        if (!parser->resources_seen ||
            !parser->dependencies_seen ||
            !parser->manifest->target_family_count ||
            (!parser->manifest->application_count &&
             !parser->manifest->framework))
            return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    }
    if (parser->executable_code_seen && !parser->architecture_seen)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (!parser->windows8_schema && parser->manifest->framework &&
        (parser->manifest->dependency_count || parser->capabilities_seen ||
         parser->applications_seen))
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (parser->manifest->framework && parser->manifest->resource_package)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if ((parser->manifest->framework || parser->manifest->resource_package) &&
        parser->manifest->application_count)
        return parser_fail( parser, APPX_E_INVALID_MANIFEST );
    if (parser->manifest->resource_package &&
        FAILED(add_unsupported_reason(
            parser, APPX_MANIFEST_UNSUPPORTED_RESOURCE_PACKAGE )))
        return parser->hr;

    if (!parser->manifest->run_full_trust)
    {
        UINT32 i;

        for (i = 0; i < parser->manifest->application_count; i++)
        {
            enum appx_manifest_activation_kind kind =
                parser->manifest->applications[i].activation_kind;

            if (kind == APPX_MANIFEST_ACTIVATION_FULL_TRUST ||
                kind == APPX_MANIFEST_ACTIVATION_PACKAGED_CLASSIC ||
                kind == APPX_MANIFEST_ACTIVATION_WIN32)
                return add_unsupported_reason(
                    parser, APPX_MANIFEST_UNSUPPORTED_MISSING_RUN_FULL_TRUST );
        }
    }
    return S_OK;

done:
    hr = parser->hr;
    free_attributes( &attributes );
    return hr;
}

static void free_ignorable_namespaces( struct parser *parser )
{
    UINT32 i;

    for (i = 0; i < parser->ignorable_count; i++)
    {
        xmlFree( parser->ignorable[i].prefix );
        xmlFree( parser->ignorable[i].uri );
    }
    parser->ignorable_count = 0;
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
    static const char comment[] = "<!--";
    static const char cdata[] = "<![CDATA[";
    static const char instruction[] = "<?";
    static const char doctype[] = "<!DOCTYPE";
    SIZE_T i = 0;

    while (i < size)
    {
        if (data[i] != '<')
        {
            i++;
            continue;
        }
        if (size - i >= sizeof(comment) - 1 &&
            !memcmp( data + i, comment, sizeof(comment) - 1 ))
        {
            i = skip_xml_section( data, size, i + sizeof(comment) - 1, "-->" );
            continue;
        }
        if (size - i >= sizeof(cdata) - 1 &&
            !memcmp( data + i, cdata, sizeof(cdata) - 1 ))
        {
            i = skip_xml_section( data, size, i + sizeof(cdata) - 1, "]]>" );
            continue;
        }
        if (size - i >= sizeof(instruction) - 1 &&
            !memcmp( data + i, instruction, sizeof(instruction) - 1 ))
        {
            i = skip_xml_section( data, size, i + sizeof(instruction) - 1, "?>" );
            continue;
        }
        if (size - i >= sizeof(doctype) - 1 &&
            !memcmp( data + i, doctype, sizeof(doctype) - 1 ))
            return TRUE;
        i++;
    }
    return FALSE;
}

void WINAPI appx_manifest_free( APPX_MANIFEST *manifest )
{
    UINT32 i;

    if (!manifest) return;
    HeapFree( GetProcessHeap(), 0, (void *)manifest->identity.name );
    HeapFree( GetProcessHeap(), 0, (void *)manifest->identity.publisher );
    HeapFree( GetProcessHeap(), 0, (void *)manifest->identity.resource_id );
    HeapFree( GetProcessHeap(), 0, (void *)manifest->identity.publisher_id );
    HeapFree( GetProcessHeap(), 0, (void *)manifest->identity.full_name );
    HeapFree( GetProcessHeap(), 0, (void *)manifest->identity.family_name );
    for (i = 0; i < manifest->application_count; i++)
    {
        struct appx_manifest_application *application = manifest->applications + i;

        HeapFree( GetProcessHeap(), 0, (void *)application->id );
        HeapFree( GetProcessHeap(), 0, (void *)application->executable );
        HeapFree( GetProcessHeap(), 0, (void *)application->entry_point );
        HeapFree( GetProcessHeap(), 0, (void *)application->runtime_behavior );
        HeapFree( GetProcessHeap(), 0, (void *)application->trust_level );
        HeapFree( GetProcessHeap(), 0, (void *)application->parameters );
        HeapFree( GetProcessHeap(), 0, (void *)application->current_directory_path );
    }
    for (i = 0; i < manifest->dependency_count; i++)
    {
        HeapFree( GetProcessHeap(), 0, (void *)manifest->dependencies[i].name );
        HeapFree( GetProcessHeap(), 0, (void *)manifest->dependencies[i].publisher );
    }
    for (i = 0; i < manifest->target_family_count; i++)
        HeapFree( GetProcessHeap(), 0, (void *)manifest->target_families[i].name );
    for (i = 0; i < manifest->class_count; i++)
    {
        HeapFree( GetProcessHeap(), 0, (void *)manifest->classes[i].path );
        HeapFree( GetProcessHeap(), 0,
                  (void *)manifest->classes[i].activatable_class_id );
    }
    HeapFree( GetProcessHeap(), 0, manifest->applications );
    HeapFree( GetProcessHeap(), 0, manifest->dependencies );
    HeapFree( GetProcessHeap(), 0, manifest->target_families );
    HeapFree( GetProcessHeap(), 0, manifest->classes );
    HeapFree( GetProcessHeap(), 0, manifest->unsupported_reasons );
    HeapFree( GetProcessHeap(), 0, manifest );
}

HRESULT WINAPI appx_manifest_parse( const BYTE *data, SIZE_T size,
                                    APPX_MANIFEST **manifest )
{
    struct parser parser = {0};
    const xmlChar *encoding;
    APPX_MANIFEST *result;
    int read_result, type;
    SIZE_T i;
    HRESULT hr;

    if (!manifest) return E_INVALIDARG;
    *manifest = NULL;
    if (!data || !size)
        return E_INVALIDARG;
    if (size > APPX_MANIFEST_MAX_SIZE) return APPX_E_INVALID_MANIFEST;
    for (i = 0; i < size; i++)
        if (!data[i]) return APPX_E_INVALID_MANIFEST;
    if (size >= 2 &&
        ((data[0] == 0xff && data[1] == 0xfe) ||
         (data[0] == 0xfe && data[1] == 0xff)))
        return APPX_E_INVALID_MANIFEST;
    if (!MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS,
                              (const char *)data, (int)size, NULL, 0 ))
        return APPX_E_INVALID_MANIFEST;
    /*
     * Reject the declaration before libxml sees it.  This is stronger than
     * relying on DTDLOAD being disabled: even a hostile external parameter
     * entity cannot cause a local file open during reader construction.
     */
    if (contains_doctype_declaration( data, size ))
        return APPX_E_INVALID_MANIFEST;

    if (!(result = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                              sizeof(*result) )))
        return E_OUTOFMEMORY;
    parser.manifest = result;
    parser.hr = S_OK;
    parser.reader = xmlReaderForMemory( (const char *)data, (int)size, NULL, NULL,
                                        XML_PARSE_NONET );
    if (!parser.reader)
    {
        appx_manifest_free( result );
        return APPX_E_INVALID_MANIFEST;
    }
    xmlTextReaderSetErrorHandler( parser.reader, silent_xml_error, &parser );
    xmlTextReaderSetParserProp( parser.reader, XML_PARSER_LOADDTD, 0 );
    xmlTextReaderSetParserProp( parser.reader, XML_PARSER_DEFAULTATTRS, 0 );
    xmlTextReaderSetParserProp( parser.reader, XML_PARSER_VALIDATE, 0 );
    xmlTextReaderSetParserProp( parser.reader, XML_PARSER_SUBST_ENTITIES, 0 );

    while ((read_result = read_node( &parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser.reader );
        if (type == XML_READER_TYPE_ELEMENT) break;
        if (type == XML_READER_TYPE_COMMENT ||
            type == XML_READER_TYPE_DOCUMENT ||
            type == XML_READER_TYPE_XML_DECLARATION ||
            current_text_is_whitespace( &parser ))
            continue;
        parser_fail( &parser, APPX_E_INVALID_MANIFEST );
        break;
    }
    encoding = xmlTextReaderConstEncoding( parser.reader );
    if (encoding && !xml_equal( encoding, BAD_CAST "UTF-8" ) &&
        !xml_equal( encoding, BAD_CAST "utf-8" ))
        parser_fail( &parser, APPX_E_INVALID_MANIFEST );
    if (read_result != 1 || FAILED(parser.hr) ||
        FAILED(parse_package_element( &parser )))
        goto failed;

    while ((read_result = read_node( &parser )) == 1)
    {
        type = xmlTextReaderNodeType( parser.reader );
        if (type == XML_READER_TYPE_COMMENT ||
            type == XML_READER_TYPE_DOCUMENT ||
            current_text_is_whitespace( &parser ))
            continue;
        parser_fail( &parser, APPX_E_INVALID_MANIFEST );
        break;
    }
    if (read_result < 0 || FAILED(parser.hr)) goto failed;

    xmlFreeTextReader( parser.reader );
    free_ignorable_namespaces( &parser );
    *manifest = result;
    return S_OK;

failed:
    hr = FAILED(parser.hr) ? parser.hr : APPX_E_INVALID_MANIFEST;
    xmlFreeTextReader( parser.reader );
    free_ignorable_namespaces( &parser );
    appx_manifest_free( result );
    return hr;
}

const struct appx_manifest_identity * WINAPI appx_manifest_get_identity(
    const APPX_MANIFEST *manifest )
{
    return manifest ? &manifest->identity : NULL;
}

BOOL WINAPI appx_manifest_is_supported( const APPX_MANIFEST *manifest )
{
    return manifest && !manifest->unsupported_reason_count;
}

BOOL WINAPI appx_manifest_is_framework( const APPX_MANIFEST *manifest )
{
    return manifest && manifest->framework;
}

BOOL WINAPI appx_manifest_is_resource_package( const APPX_MANIFEST *manifest )
{
    return manifest && manifest->resource_package;
}

BOOL WINAPI appx_manifest_has_run_full_trust( const APPX_MANIFEST *manifest )
{
    return manifest && manifest->run_full_trust;
}

UINT32 WINAPI appx_manifest_get_application_count( const APPX_MANIFEST *manifest )
{
    return manifest ? manifest->application_count : 0;
}

const struct appx_manifest_application * WINAPI appx_manifest_get_application(
    const APPX_MANIFEST *manifest, UINT32 index )
{
    if (!manifest || index >= manifest->application_count) return NULL;
    return manifest->applications + index;
}

UINT32 WINAPI appx_manifest_get_dependency_count( const APPX_MANIFEST *manifest )
{
    return manifest ? manifest->dependency_count : 0;
}

const struct appx_manifest_dependency * WINAPI appx_manifest_get_dependency(
    const APPX_MANIFEST *manifest, UINT32 index )
{
    if (!manifest || index >= manifest->dependency_count) return NULL;
    return manifest->dependencies + index;
}

UINT32 WINAPI appx_manifest_get_target_family_count( const APPX_MANIFEST *manifest )
{
    return manifest ? manifest->target_family_count : 0;
}

const struct appx_manifest_target_family * WINAPI appx_manifest_get_target_family(
    const APPX_MANIFEST *manifest, UINT32 index )
{
    if (!manifest || index >= manifest->target_family_count) return NULL;
    return manifest->target_families + index;
}

UINT32 WINAPI appx_manifest_get_inproc_class_count( const APPX_MANIFEST *manifest )
{
    return manifest ? manifest->class_count : 0;
}

const struct appx_manifest_inproc_class * WINAPI appx_manifest_get_inproc_class(
    const APPX_MANIFEST *manifest, UINT32 index )
{
    if (!manifest || index >= manifest->class_count) return NULL;
    return manifest->classes + index;
}

UINT32 WINAPI appx_manifest_get_unsupported_reason_count(
    const APPX_MANIFEST *manifest )
{
    return manifest ? manifest->unsupported_reason_count : 0;
}

enum appx_manifest_unsupported_reason WINAPI appx_manifest_get_unsupported_reason(
    const APPX_MANIFEST *manifest, UINT32 index )
{
    if (!manifest || index >= manifest->unsupported_reason_count) return 0;
    return manifest->unsupported_reasons[index];
}
