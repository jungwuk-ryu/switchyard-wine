/*
 * AppX OPC content-types parser tests
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

#include "../content_types.h"

#define CONTENT_TYPES_NS \
    "http://schemas.openxmlformats.org/package/2006/content-types"
#define TYPES_OPEN "<Types xmlns=\"" CONTENT_TYPES_NS "\">"
#define TYPES_CLOSE "</Types>"
#define PACKAGE_REQUIRED \
    "<Override PartName=\"/AppxManifest.xml\"" \
    " ContentType=\"application/vnd.ms-appx.manifest+xml\"/>" \
    "<Override PartName=\"/AppxBlockMap.xml\"" \
    " ContentType=\"application/vnd.ms-appx.blockmap+xml\"/>" \
    "<Override PartName=\"/AppxSignature.p7x\"" \
    " ContentType=\"application/vnd.ms-appx.signature\"/>"
#define BUNDLE_REQUIRED \
    "<Override PartName=\"/AppxMetadata/AppxBundleManifest.xml\"" \
    " ContentType=\"application/vnd.ms-appx.bundlemanifest+xml\"/>" \
    "<Override PartName=\"/AppxBlockMap.xml\"" \
    " ContentType=\"application/vnd.ms-appx.blockmap+xml\"/>" \
    "<Override PartName=\"/AppxSignature.p7x\"" \
    " ContentType=\"application/vnd.ms-appx.signature\"/>"

static HRESULT (WINAPI *p_appx_content_types_parse)(
    const BYTE *, UINT32, enum appx_content_types_mode, APPX_CONTENT_TYPES ** );
static void (WINAPI *p_appx_content_types_free)( APPX_CONTENT_TYPES * );
static enum appx_content_types_mode (WINAPI *p_appx_content_types_get_mode)(
    const APPX_CONTENT_TYPES * );
static UINT32 (WINAPI *p_appx_content_types_get_default_count)(
    const APPX_CONTENT_TYPES * );
static const APPX_CONTENT_TYPE_DEFAULT *(WINAPI *p_appx_content_types_get_default)(
    const APPX_CONTENT_TYPES *, UINT32 );
static UINT32 (WINAPI *p_appx_content_types_get_override_count)(
    const APPX_CONTENT_TYPES * );
static const APPX_CONTENT_TYPE_OVERRIDE *(WINAPI *p_appx_content_types_get_override)(
    const APPX_CONTENT_TYPES *, UINT32 );
static const WCHAR *(WINAPI *p_appx_content_types_get_content_type)(
    const APPX_CONTENT_TYPES *, const WCHAR * );

static const char valid_package[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    TYPES_OPEN
    "<Default Extension=\"png\" ContentType=\"image/png\"/>"
    "<Default Extension=\"exe\" ContentType=\"application/x-msdownload\"/>"
    PACKAGE_REQUIRED
    "<Override PartName=\"/Assets/%41%25icon.png\""
    " ContentType=\"application/vnd.wine.icon\"/>"
    "<Override PartName=\"/Assets/a&amp;b.png\""
    " ContentType=\"application/a&amp;b\"/>"
    TYPES_CLOSE;

static const char valid_bundle[] =
    TYPES_OPEN
    "<Default Extension=\"msix\" ContentType=\"application/vnd.ms-appx.package\"/>"
    BUNDLE_REQUIRED
    TYPES_CLOSE;

static HRESULT parse_text( const char *text, enum appx_content_types_mode mode,
                           APPX_CONTENT_TYPES **types )
{
    return p_appx_content_types_parse( (const BYTE *)text, strlen( text ),
                                       mode, types );
}

static void check_data_( const void *document, SIZE_T size,
                         enum appx_content_types_mode mode, HRESULT expected,
                         const char *file, int line )
{
    APPX_CONTENT_TYPES *types = (APPX_CONTENT_TYPES *)0xdeadbeef;
    HRESULT hr;

    if (size > MAXDWORD)
    {
        ok_(file, line)( 0, "test document is too large.\n" );
        return;
    }
    hr = p_appx_content_types_parse( document, size, mode, &types );
    ok_(file, line)( hr == expected, "got hr %#lx, expected %#lx.\n",
                     hr, expected );
    if (SUCCEEDED(hr))
    {
        ok_(file, line)( types != NULL, "successful parse returned no table.\n" );
        p_appx_content_types_free( types );
    }
    else
        ok_(file, line)( types == NULL, "failed parse returned table %p.\n", types );
}

#define check_data(document, size, mode, expected) \
    check_data_( document, size, mode, expected, __FILE__, __LINE__ )
#define check_text(document, mode, expected) \
    check_data_( document, strlen(document), mode, expected, __FILE__, __LINE__ )
#define check_package(document, expected) \
    check_text( document, APPX_CONTENT_TYPES_MODE_PACKAGE, expected )
#define check_bundle(document, expected) \
    check_text( document, APPX_CONTENT_TYPES_MODE_BUNDLE, expected )

static char *make_extra_entry( const char *element, const char *key_name,
                               const char *key, const char *content_type,
                               SIZE_T *size )
{
    static const char prefix[] = TYPES_OPEN PACKAGE_REQUIRED "<";
    static const char middle1[] = " ";
    static const char middle2[] = "=\"";
    static const char middle3[] = "\" ContentType=\"";
    static const char suffix[] = "\"/>" TYPES_CLOSE;
    SIZE_T length = sizeof(prefix) - 1 + strlen(element) +
                    sizeof(middle1) - 1 + strlen(key_name) +
                    sizeof(middle2) - 1 + strlen(key) +
                    sizeof(middle3) - 1 + strlen(content_type) +
                    sizeof(suffix) - 1;
    char *document, *cursor;

    if (!(document = HeapAlloc( GetProcessHeap(), 0, length + 1 ))) return NULL;
    cursor = document;
#define APPEND_LITERAL(value) \
    do { memcpy( cursor, value, sizeof(value) - 1 ); cursor += sizeof(value) - 1; } while (0)
#define APPEND_STRING(value) \
    do { SIZE_T len = strlen(value); memcpy( cursor, value, len ); cursor += len; } while (0)
    APPEND_LITERAL(prefix);
    APPEND_STRING(element);
    APPEND_LITERAL(middle1);
    APPEND_STRING(key_name);
    APPEND_LITERAL(middle2);
    APPEND_STRING(key);
    APPEND_LITERAL(middle3);
    APPEND_STRING(content_type);
    APPEND_LITERAL(suffix);
#undef APPEND_STRING
#undef APPEND_LITERAL
    *cursor = 0;
    *size = cursor - document;
    return document;
}

static void check_extra_entry( const char *element, const char *key_name,
                               const char *key, const char *content_type,
                               HRESULT expected )
{
    char *document;
    SIZE_T size;

    document = make_extra_entry( element, key_name, key, content_type, &size );
    ok( document != NULL, "failed to allocate test document.\n" );
    if (!document) return;
    check_data( document, size, APPX_CONTENT_TYPES_MODE_PACKAGE, expected );
    HeapFree( GetProcessHeap(), 0, document );
}

static const APPX_CONTENT_TYPE_OVERRIDE *find_override(
    const APPX_CONTENT_TYPES *types, const WCHAR *name )
{
    UINT32 count = p_appx_content_types_get_override_count( types ), i;

    for (i = 0; i < count; i++)
    {
        const APPX_CONTENT_TYPE_OVERRIDE *entry =
            p_appx_content_types_get_override( types, i );
        if (entry && !lstrcmpW( entry->part_name, name )) return entry;
    }
    return NULL;
}

static void test_arguments(void)
{
    APPX_CONTENT_TYPES *types = (APPX_CONTENT_TYPES *)0xdeadbeef;
    BYTE byte = '<';
    HRESULT hr;

    hr = p_appx_content_types_parse( NULL, 1, APPX_CONTENT_TYPES_MODE_PACKAGE,
                                     &types );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( !types, "got table %p.\n", types );
    hr = p_appx_content_types_parse( &byte, 0, APPX_CONTENT_TYPES_MODE_PACKAGE,
                                     &types );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_appx_content_types_parse( &byte, 1, 0, &types );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_appx_content_types_parse( &byte, 1, 3, &types );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_appx_content_types_parse( &byte, 1, APPX_CONTENT_TYPES_MODE_PACKAGE,
                                     NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_appx_content_types_parse( &byte,
                                     APPX_CONTENT_TYPES_MAX_DOCUMENT_SIZE + 1,
                                     APPX_CONTENT_TYPES_MODE_PACKAGE, &types );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "got hr %#lx.\n", hr );

    p_appx_content_types_free( NULL );
    ok( !p_appx_content_types_get_mode( NULL ), "got a mode.\n" );
    ok( !p_appx_content_types_get_default_count( NULL ), "got defaults.\n" );
    ok( !p_appx_content_types_get_default( NULL, 0 ), "got a default.\n" );
    ok( !p_appx_content_types_get_override_count( NULL ), "got overrides.\n" );
    ok( !p_appx_content_types_get_override( NULL, 0 ), "got an override.\n" );
    ok( !p_appx_content_types_get_content_type( NULL, L"/a.xml" ),
        "got a content type.\n" );
}

static void test_package(void)
{
    const APPX_CONTENT_TYPE_OVERRIDE *override;
    const WCHAR *content_type;
    APPX_CONTENT_TYPES *types;
    HRESULT hr;

    hr = parse_text( valid_package, APPX_CONTENT_TYPES_MODE_PACKAGE, &types );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;

    ok( p_appx_content_types_get_mode( types ) == APPX_CONTENT_TYPES_MODE_PACKAGE,
        "got mode %u.\n", p_appx_content_types_get_mode( types ) );
    ok( p_appx_content_types_get_default_count( types ) == 2,
        "got %u defaults.\n", p_appx_content_types_get_default_count( types ) );
    ok( p_appx_content_types_get_override_count( types ) == 5,
        "got %u overrides.\n", p_appx_content_types_get_override_count( types ) );
    ok( !p_appx_content_types_get_default( types, 2 ),
        "got an out-of-range default.\n" );
    ok( !p_appx_content_types_get_override( types, 5 ),
        "got an out-of-range override.\n" );

    content_type = p_appx_content_types_get_content_type(
        types, L"/APPXMANIFEST.XML" );
    ok( content_type && !lstrcmpW( content_type, APPX_CONTENT_TYPE_MANIFEST ),
        "got content type %s.\n", debugstr_w(content_type) );
    content_type = p_appx_content_types_get_content_type(
        types, L"/bin/WINE.EXE" );
    ok( content_type && !lstrcmpW( content_type, L"application/x-msdownload" ),
        "got content type %s.\n", debugstr_w(content_type) );
    content_type = p_appx_content_types_get_content_type(
        types, L"/Assets/A%icon.png" );
    ok( content_type && !lstrcmpW( content_type, L"application/vnd.wine.icon" ),
        "override did not take precedence, got %s.\n", debugstr_w(content_type) );
    content_type = p_appx_content_types_get_content_type(
        types, L"/Assets/a&b.png" );
    ok( content_type && !lstrcmpW( content_type, L"application/a&b" ),
        "predefined entity produced %s.\n", debugstr_w(content_type) );
    ok( !p_appx_content_types_get_content_type( types, L"/no-extension" ),
        "extensionless part resolved.\n" );
    ok( !p_appx_content_types_get_content_type( types, L"relative.exe" ),
        "relative part resolved.\n" );
    ok( !p_appx_content_types_get_content_type( types, L"/bad\\name.exe" ),
        "backslash part resolved.\n" );
    ok( !p_appx_content_types_get_content_type( types, L"/NUL.exe" ),
        "reserved-name part resolved.\n" );
    ok( !p_appx_content_types_get_content_type( types, L"/a/../b.exe" ),
        "traversal part resolved.\n" );
    ok( !p_appx_content_types_get_content_type( types, L"/bad?.exe" ),
        "forbidden-character part resolved.\n" );
    ok( !p_appx_content_types_get_content_type( types, L"/bad./file.exe" ),
        "trailing-dot component resolved.\n" );
    ok( !p_appx_content_types_get_content_type( types, L"/bad /file.exe" ),
        "trailing-space component resolved.\n" );
    ok( !p_appx_content_types_get_content_type(
            types, L"/cafe\x0301/file.exe" ),
        "non-NFC part resolved.\n" );

    override = find_override( types, L"/Assets/A%icon.png" );
    ok( override != NULL, "canonical percent-decoded override is missing.\n" );
    if (override)
    {
        ok( override->part_name_length == lstrlenW( override->part_name ),
            "got part-name length %u.\n", override->part_name_length );
        ok( override->content_type_length == lstrlenW( override->content_type ),
            "got content-type length %u.\n", override->content_type_length );
    }
    p_appx_content_types_free( types );
}

static void test_bundle_and_prefixed_namespace(void)
{
    static const char prefixed[] =
        "<ct:Types xmlns:ct=\"" CONTENT_TYPES_NS "\" xmlns:u=\"urn:unused\">"
        "<ct:Override PartName=\"/AppxManifest.xml\""
        " ContentType=\"application/vnd.ms-appx.manifest+xml\"></ct:Override>"
        "<ct:Override PartName=\"/AppxBlockMap.xml\""
        " ContentType=\"application/vnd.ms-appx.blockmap+xml\">"
        "<!-- empty schema content --></ct:Override>"
        "<ct:Override PartName=\"/AppxSignature.p7x\""
        " ContentType=\"application/vnd.ms-appx.signature\"/>"
        "</ct:Types>";
    APPX_CONTENT_TYPES *types;
    HRESULT hr;

    hr = parse_text( valid_bundle, APPX_CONTENT_TYPES_MODE_BUNDLE, &types );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( p_appx_content_types_get_mode( types ) ==
            APPX_CONTENT_TYPES_MODE_BUNDLE, "got mode %u.\n",
            p_appx_content_types_get_mode( types ) );
        ok( p_appx_content_types_get_content_type(
                types, L"/AppxMetadata/AppxBundleManifest.xml" ) != NULL,
            "bundle manifest mapping is missing.\n" );
        p_appx_content_types_free( types );
    }
    check_package( prefixed, S_OK );
}

static void test_required_mappings_and_precedence(void)
{
    static const char missing_manifest[] =
        TYPES_OPEN
        "<Override PartName=\"/AppxBlockMap.xml\""
        " ContentType=\"application/vnd.ms-appx.blockmap+xml\"/>"
        "<Override PartName=\"/AppxSignature.p7x\""
        " ContentType=\"application/vnd.ms-appx.signature\"/>"
        TYPES_CLOSE;
    static const char wrong_manifest_case[] =
        TYPES_OPEN
        "<Override PartName=\"/AppxManifest.xml\""
        " ContentType=\"Application/Vnd.Ms-Appx.Manifest+Xml\"/>"
        "<Override PartName=\"/AppxBlockMap.xml\""
        " ContentType=\"application/vnd.ms-appx.blockmap+xml\"/>"
        "<Override PartName=\"/AppxSignature.p7x\""
        " ContentType=\"application/vnd.ms-appx.signature\"/>"
        TYPES_CLOSE;
    static const char correct_override_wins[] =
        TYPES_OPEN
        "<Default Extension=\"xml\" ContentType=\"application/not-appx\"/>"
        PACKAGE_REQUIRED
        TYPES_CLOSE;
    static const char required_default[] =
        TYPES_OPEN
        "<Default Extension=\"p7x\""
        " ContentType=\"application/vnd.ms-appx.signature\"/>"
        "<Override PartName=\"/AppxManifest.xml\""
        " ContentType=\"application/vnd.ms-appx.manifest+xml\"/>"
        "<Override PartName=\"/AppxBlockMap.xml\""
        " ContentType=\"application/vnd.ms-appx.blockmap+xml\"/>"
        TYPES_CLOSE;
    static const char wrong_override_wins[] =
        TYPES_OPEN
        "<Default Extension=\"xml\""
        " ContentType=\"application/vnd.ms-appx.manifest+xml\"/>"
        "<Override PartName=\"/AppxManifest.xml\" ContentType=\"application/wrong\"/>"
        "<Override PartName=\"/AppxBlockMap.xml\""
        " ContentType=\"application/vnd.ms-appx.blockmap+xml\"/>"
        "<Override PartName=\"/AppxSignature.p7x\""
        " ContentType=\"application/vnd.ms-appx.signature\"/>"
        TYPES_CLOSE;
    static const char package_as_bundle[] = TYPES_OPEN PACKAGE_REQUIRED TYPES_CLOSE;
    static const char bundle_as_package[] = TYPES_OPEN BUNDLE_REQUIRED TYPES_CLOSE;

    check_package( missing_manifest, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( wrong_manifest_case, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( correct_override_wins, S_OK );
    check_package( required_default, S_OK );
    check_package( wrong_override_wins, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_bundle( package_as_bundle, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( bundle_as_package, APPX_E_INVALID_PACKAGING_LAYOUT );
}

static void test_duplicates_and_single_decode(void)
{
    static const char duplicate_default[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Default Extension=\"XML\" ContentType=\"application/one\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/two\"/>"
        TYPES_CLOSE;
    static const char duplicate_override_case[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Override PartName=\"/Assets/Icon.PNG\" ContentType=\"image/one\"/>"
        "<Override PartName=\"/assets/icon.png\" ContentType=\"image/two\"/>"
        TYPES_CLOSE;
    static const char duplicate_override_escape[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Override PartName=\"/Assets/%49con.png\" ContentType=\"image/one\"/>"
        "<Override PartName=\"/assets/icon.png\" ContentType=\"image/two\"/>"
        TYPES_CLOSE;
    static const char duplicate_percent[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Override PartName=\"/a%25B.dat\" ContentType=\"application/one\"/>"
        "<Override PartName=\"/A%25b.dat\" ContentType=\"application/two\"/>"
        TYPES_CLOSE;
    static const char single_decode[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Override PartName=\"/a%2520b.dat\" ContentType=\"application/first\"/>"
        "<Override PartName=\"/a%20b.dat\" ContentType=\"application/second\"/>"
        TYPES_CLOSE;
    const WCHAR *content_type;
    APPX_CONTENT_TYPES *types;
    HRESULT hr;

    check_package( duplicate_default, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( duplicate_override_case, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( duplicate_override_escape, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( duplicate_percent, APPX_E_INVALID_PACKAGING_LAYOUT );

    hr = parse_text( single_decode, APPX_CONTENT_TYPES_MODE_PACKAGE, &types );
    ok( hr == S_OK, "single percent decode got hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    content_type = p_appx_content_types_get_content_type( types, L"/a%20b.dat" );
    ok( content_type && !lstrcmpW( content_type, L"application/first" ),
        "got content type %s.\n", debugstr_w(content_type) );
    content_type = p_appx_content_types_get_content_type( types, L"/a b.dat" );
    ok( content_type && !lstrcmpW( content_type, L"application/second" ),
        "got content type %s.\n", debugstr_w(content_type) );
    p_appx_content_types_free( types );
}

static void test_xml_security_and_grammar(void)
{
    static const char doctype_in_comment[] =
        "<!-- <!DOCTYPE Types SYSTEM=\"file:///etc/passwd\"> -->"
        TYPES_OPEN PACKAGE_REQUIRED TYPES_CLOSE;
    static const char real_doctype[] =
        "<!DOCTYPE Types SYSTEM=\"file:///etc/passwd\">"
        TYPES_OPEN PACKAGE_REQUIRED TYPES_CLOSE;
    static const char entity[] =
        "<!DOCTYPE Types [<!ENTITY x SYSTEM \"file:///etc/passwd\">]>"
        TYPES_OPEN PACKAGE_REQUIRED TYPES_CLOSE;
    static const char undefined_entity[] =
        TYPES_OPEN
        "<Default Extension=\"dat\" ContentType=\"application/&undefined;\"/>"
        PACKAGE_REQUIRED TYPES_CLOSE;
    static const char instruction[] =
        TYPES_OPEN "<?hostile data?>" PACKAGE_REQUIRED TYPES_CLOSE;
    static const char cdata[] =
        TYPES_OPEN "<![CDATA[ignored]]>" PACKAGE_REQUIRED TYPES_CLOSE;
    static const char xinclude[] =
        TYPES_OPEN "<xi:include xmlns:xi=\"http://www.w3.org/2001/XInclude\""
        " href=\"file:///etc/passwd\"/>" PACKAGE_REQUIRED TYPES_CLOSE;
    static const char unused_xinclude[] =
        "<Types xmlns=\"" CONTENT_TYPES_NS "\""
        " xmlns:xi=\"http://www.w3.org/2001/XInclude\">"
        PACKAGE_REQUIRED TYPES_CLOSE;
    static const char unknown_root_attribute[] =
        "<Types xmlns=\"" CONTENT_TYPES_NS "\" Unknown=\"1\">"
        PACKAGE_REQUIRED TYPES_CLOSE;
    static const char qualified_attribute[] =
        "<Types xmlns=\"" CONTENT_TYPES_NS "\" xmlns:x=\"urn:test\">"
        "<Override x:PartName=\"/AppxManifest.xml\""
        " ContentType=\"application/vnd.ms-appx.manifest+xml\"/>"
        "<Override PartName=\"/AppxBlockMap.xml\""
        " ContentType=\"application/vnd.ms-appx.blockmap+xml\"/>"
        "<Override PartName=\"/AppxSignature.p7x\""
        " ContentType=\"application/vnd.ms-appx.signature\"/>"
        TYPES_CLOSE;
    static const char unknown_child[] =
        TYPES_OPEN "<Future/>" PACKAGE_REQUIRED TYPES_CLOSE;
    static const char nested_child[] =
        TYPES_OPEN
        "<Override PartName=\"/extra.dat\" ContentType=\"application/test\">"
        "<Default Extension=\"dat\" ContentType=\"application/test\"/>"
        "</Override>" PACKAGE_REQUIRED TYPES_CLOSE;
    static const char non_whitespace_text[] =
        TYPES_OPEN "payload" PACKAGE_REQUIRED TYPES_CLOSE;
    static const char trailing_root[] =
        TYPES_OPEN PACKAGE_REQUIRED TYPES_CLOSE
        TYPES_OPEN PACKAGE_REQUIRED TYPES_CLOSE;
    static const char missing_attribute[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Default Extension=\"dat\"/>"
        TYPES_CLOSE;
    static const char unknown_attribute[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Default Extension=\"dat\" ContentType=\"application/test\" X=\"1\"/>"
        TYPES_CLOSE;
    static const char invalid_media_type[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Default Extension=\"dat\" ContentType=\"application\"/>"
        TYPES_CLOSE;
    static const char media_type_parameter[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Default Extension=\"dat\" ContentType=\"text/plain;charset=utf-8\"/>"
        TYPES_CLOSE;
    static const char invalid_extension[] =
        TYPES_OPEN PACKAGE_REQUIRED
        "<Default Extension=\".dat\" ContentType=\"application/test\"/>"
        TYPES_CLOSE;
    static const char version_11[] =
        "<?xml version=\"1.1\"?>" TYPES_OPEN PACKAGE_REQUIRED TYPES_CLOSE;
    static const char encoding_utf16[] =
        "<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        TYPES_OPEN PACKAGE_REQUIRED TYPES_CLOSE;
    static const BYTE embedded_nul[] =
        TYPES_OPEN PACKAGE_REQUIRED "\0" TYPES_CLOSE;
    static const BYTE invalid_utf8[] =
        TYPES_OPEN PACKAGE_REQUIRED "\xc0\xaf" TYPES_CLOSE;
    static const BYTE utf16_bom[] = {0xff, 0xfe, '<'};

    check_package( doctype_in_comment, S_OK );
    check_package( real_doctype, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( entity, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( undefined_entity, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( instruction, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( cdata, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( xinclude, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( unused_xinclude, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( unknown_root_attribute, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( qualified_attribute, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( unknown_child, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( nested_child, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( non_whitespace_text, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( trailing_root, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( missing_attribute, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( unknown_attribute, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( invalid_media_type, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( media_type_parameter, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( invalid_extension, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( version_11, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_package( encoding_utf16, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_data( embedded_nul, sizeof(embedded_nul) - 1,
                APPX_CONTENT_TYPES_MODE_PACKAGE, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_data( invalid_utf8, sizeof(invalid_utf8) - 1,
                APPX_CONTENT_TYPES_MODE_PACKAGE, APPX_E_INVALID_PACKAGING_LAYOUT );
    check_data( utf16_bom, sizeof(utf16_bom),
                APPX_CONTENT_TYPES_MODE_PACKAGE, APPX_E_INVALID_PACKAGING_LAYOUT );
}

static void test_part_names(void)
{
    static const char *invalid[] =
    {
        "relative.dat",
        "/",
        "//a.dat",
        "/a/",
        "/a//b.dat",
        "/a/./b.dat",
        "/a/../b.dat",
        "/a b.dat",
        "/a%2fb.dat",
        "/a%5cb.dat",
        "/a%.dat",
        "/a%2.dat",
        "/a%xx.dat",
        "/a?b.dat",
        "/a#b.dat",
        "/a\\b.dat",
        "/C:a.dat",
        "/NUL.dat",
        "/a[0].dat",
        "/caf\xc3\xa9.dat"
    };
    static const char *valid[] =
    {
        "/a%20b.dat",
        "/a%23b.dat",
        "/a%25b.dat",
        "/caf%C3%A9.dat",
        "/symbols!$&amp;'()+,;=@.dat"
    };
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(invalid); i++)
        check_extra_entry( "Override", "PartName", invalid[i],
                           "application/test", APPX_E_INVALID_PACKAGING_LAYOUT );
    for (i = 0; i < ARRAY_SIZE(valid); i++)
        check_extra_entry( "Override", "PartName", valid[i],
                           "application/test", S_OK );
}

static void test_boundaries(void)
{
    static const char root_eight_attributes[] =
        "<Types xmlns=\"" CONTENT_TYPES_NS "\""
        " xmlns:a=\"urn:a\" xmlns:b=\"urn:b\" xmlns:c=\"urn:c\""
        " xmlns:d=\"urn:d\" xmlns:e=\"urn:e\" xmlns:f=\"urn:f\""
        " xmlns:g=\"urn:g\">"
        PACKAGE_REQUIRED TYPES_CLOSE;
    static const char root_nine_attributes[] =
        "<Types xmlns=\"" CONTENT_TYPES_NS "\""
        " xmlns:a=\"urn:a\" xmlns:b=\"urn:b\" xmlns:c=\"urn:c\""
        " xmlns:d=\"urn:d\" xmlns:e=\"urn:e\" xmlns:f=\"urn:f\""
        " xmlns:g=\"urn:g\" xmlns:h=\"urn:h\">"
        PACKAGE_REQUIRED TYPES_CLOSE;
    char component[258], extension[APPX_CONTENT_TYPES_MAX_EXTENSION_CHARS + 2];
    char content_type[APPX_CONTENT_TYPES_MAX_CONTENT_TYPE_CHARS + 2];
    UINT32 prefix_length = strlen( "application/" );

    component[0] = '/';
    memset( component + 1, 'a', 251 );
    memcpy( component + 252, ".dat", 5 );
    check_extra_entry( "Override", "PartName", component,
                       "application/test", S_OK );
    component[252] = 'a';
    memcpy( component + 253, ".dat", 5 );
    check_extra_entry( "Override", "PartName", component,
                       "application/test", APPX_E_INVALID_PACKAGING_LAYOUT );

    memset( extension, 'a', APPX_CONTENT_TYPES_MAX_EXTENSION_CHARS );
    extension[APPX_CONTENT_TYPES_MAX_EXTENSION_CHARS] = 0;
    check_extra_entry( "Default", "Extension", extension,
                       "application/test", S_OK );
    extension[APPX_CONTENT_TYPES_MAX_EXTENSION_CHARS] = 'a';
    extension[APPX_CONTENT_TYPES_MAX_EXTENSION_CHARS + 1] = 0;
    check_extra_entry( "Default", "Extension", extension,
                       "application/test", APPX_E_INVALID_PACKAGING_LAYOUT );

    memcpy( content_type, "application/", prefix_length );
    memset( content_type + prefix_length, 'a',
            APPX_CONTENT_TYPES_MAX_CONTENT_TYPE_CHARS - prefix_length );
    content_type[APPX_CONTENT_TYPES_MAX_CONTENT_TYPE_CHARS] = 0;
    check_extra_entry( "Default", "Extension", "dat", content_type, S_OK );
    content_type[APPX_CONTENT_TYPES_MAX_CONTENT_TYPE_CHARS] = 'a';
    content_type[APPX_CONTENT_TYPES_MAX_CONTENT_TYPE_CHARS + 1] = 0;
    check_extra_entry( "Default", "Extension", "dat", content_type,
                       APPX_E_INVALID_PACKAGING_LAYOUT );

    check_package( root_eight_attributes, S_OK );
    check_package( root_nine_attributes, APPX_E_INVALID_PACKAGING_LAYOUT );
}

static BOOL load_functions(void)
{
    HMODULE module = LoadLibraryW( L"appxsvc.dll" );

    ok( module != NULL, "failed to load appxsvc.dll, error %lu.\n",
        GetLastError() );
    if (!module) return FALSE;
#define LOAD(name) \
    do { \
        p_##name = (void *)GetProcAddress( module, #name ); \
        ok( p_##name != NULL, "failed to load %s, error %lu.\n", \
            #name, GetLastError() ); \
        if (!p_##name) return FALSE; \
    } while (0)
    LOAD(appx_content_types_parse);
    LOAD(appx_content_types_free);
    LOAD(appx_content_types_get_mode);
    LOAD(appx_content_types_get_default_count);
    LOAD(appx_content_types_get_default);
    LOAD(appx_content_types_get_override_count);
    LOAD(appx_content_types_get_override);
    LOAD(appx_content_types_get_content_type);
#undef LOAD
    return TRUE;
}

START_TEST(content_types)
{
    if (!load_functions())
    {
        win_skip( "AppX content-types parser exports are unavailable.\n" );
        return;
    }
    test_arguments();
    test_package();
    test_bundle_and_prefixed_namespace();
    test_required_mappings_and_precedence();
    test_duplicates_and_single_decode();
    test_xml_security_and_grammar();
    test_part_names();
    test_boundaries();
}
