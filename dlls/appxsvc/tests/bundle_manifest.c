/*
 * AppX bundle manifest parser tests
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
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "../bundle_manifest.h"
#include "wine/test.h"

static HRESULT (WINAPI *p_appx_bundle_manifest_parse)(
    const BYTE *, SIZE_T, APPX_BUNDLE_MANIFEST ** );
static void (WINAPI *p_appx_bundle_manifest_free)( APPX_BUNDLE_MANIFEST * );
static const struct appx_bundle_identity *
    (WINAPI *p_appx_bundle_manifest_get_identity)(
        const APPX_BUNDLE_MANIFEST * );
static UINT32 (WINAPI *p_appx_bundle_manifest_get_package_count)(
    const APPX_BUNDLE_MANIFEST * );
static const struct appx_bundle_package *
    (WINAPI *p_appx_bundle_manifest_get_package)(
        const APPX_BUNDLE_MANIFEST *, UINT32 );
static HRESULT (WINAPI *p_appx_bundle_manifest_select)(
    const APPX_BUNDLE_MANIFEST *,
    const struct appx_bundle_selection_policy *,
    struct appx_bundle_selection * );

#define BUNDLE_OPEN \
    "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\"" \
    " SchemaVersion=\"1.0\">"
#define BUNDLE_IDENTITY \
    "<Identity Name=\"Wine.Bundle\" Publisher=\"CN=Wine &amp; Project\"" \
    " Version=\"1.2.3.4\"/>"
#define NEUTRAL_RESOURCES "<Resources><Resource/></Resources>"
#define BUNDLE_CLOSE "</Bundle>"

static HRESULT parse_text( const char *text, APPX_BUNDLE_MANIFEST **manifest )
{
    return p_appx_bundle_manifest_parse( (const BYTE *)text,
                                         strlen( text ), manifest );
}

static struct appx_bundle_selection_policy exact_policy(
    enum appx_bundle_architecture architecture,
    const WCHAR *language, UINT32 scale )
{
    struct appx_bundle_selection_policy policy;

    memset( &policy, 0, sizeof(policy) );
    policy.size = sizeof(policy);
    policy.host_architecture = architecture;
    policy.language = language;
    policy.scale = scale;
    return policy;
}

static struct appx_bundle_selection_policy neutral_policy(
    enum appx_bundle_architecture architecture )
{
    struct appx_bundle_selection_policy policy =
        exact_policy( architecture, NULL, 0 );

    policy.language_neutral_only = TRUE;
    policy.scale_neutral_only = TRUE;
    return policy;
}

static void init_selection( struct appx_bundle_selection *selection )
{
    memset( selection, 0, sizeof(*selection) );
    selection->size = sizeof(*selection);
}

static void test_arguments(void)
{
    struct appx_bundle_selection selection;
    struct appx_bundle_selection_policy policy;
    APPX_BUNDLE_MANIFEST *manifest = (APPX_BUNDLE_MANIFEST *)0xdeadbeef;
    BYTE byte = '<';
    HRESULT hr;

    hr = p_appx_bundle_manifest_parse( NULL, 1, &manifest );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( !manifest, "got manifest %p.\n", manifest );
    hr = p_appx_bundle_manifest_parse( &byte, 0, &manifest );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_appx_bundle_manifest_parse( &byte, 1, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_appx_bundle_manifest_parse(
        &byte, APPX_BUNDLE_MANIFEST_MAX_SIZE + 1, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );

    p_appx_bundle_manifest_free( NULL );
    ok( !p_appx_bundle_manifest_get_identity( NULL ), "got identity.\n" );
    ok( !p_appx_bundle_manifest_get_package_count( NULL ),
        "got packages.\n" );
    ok( !p_appx_bundle_manifest_get_package( NULL, 0 ), "got package.\n" );

    memset( &selection, 0xcc, sizeof(selection) );
    selection.size = sizeof(selection);
    policy = neutral_policy( APPX_BUNDLE_ARCHITECTURE_X64 );
    hr = p_appx_bundle_manifest_select( NULL, &policy, &selection );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( selection.size == sizeof(selection), "got size %u.\n",
        selection.size );
    hr = p_appx_bundle_manifest_select( NULL, &policy, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
}

static void test_valid_manifest(void)
{
    static const char manifest_text[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages>"
        "<Package Type=\"application\" Version=\"1.2.3.4\""
        " Architecture=\"x64\""
        " FileName=\"payload/app.msix\" Offset=\"0\" Size=\"4096\">"
        "<Resources><Resource Language=\"en-US\"/></Resources></Package>"
        "<Package Type=\"resource\" Version=\"1.2.3.4\""
        " Architecture=\"neutral\" ResourceId=\"resources.en-us\""
        " FileName=\"payload/resources.msix\" Offset=\"4096\" Size=\"1024\">"
        "<Resources><Resource Language=\"en-US\" Scale=\"225\"/></Resources>"
        "</Package>"
        "</Packages>" BUNDLE_CLOSE;
    const struct appx_bundle_identity *identity;
    const struct appx_bundle_package *package;
    struct appx_bundle_selection_policy policy;
    struct appx_bundle_selection selection;
    APPX_BUNDLE_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( manifest_text, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;

    identity = p_appx_bundle_manifest_get_identity( manifest );
    ok( !!identity, "identity is missing.\n" );
    ok( !lstrcmpW( identity->name, L"Wine.Bundle" ), "got name %s.\n",
        wine_dbgstr_w(identity->name) );
    ok( !lstrcmpW( identity->publisher, L"CN=Wine & Project" ),
        "got publisher %s.\n", wine_dbgstr_w(identity->publisher) );
    ok( identity->version.major == 1 && identity->version.minor == 2 &&
        identity->version.build == 3 && identity->version.revision == 4,
        "got version %u.%u.%u.%u.\n", identity->version.major,
        identity->version.minor, identity->version.build,
        identity->version.revision );

    ok( p_appx_bundle_manifest_get_package_count( manifest ) == 2,
        "got %u packages.\n",
        p_appx_bundle_manifest_get_package_count( manifest ) );
    package = p_appx_bundle_manifest_get_package( manifest, 0 );
    ok( !!package, "package is missing.\n" );
    ok( package->type == APPX_BUNDLE_PACKAGE_APPLICATION,
        "got type %u.\n", package->type );
    ok( package->architecture == APPX_BUNDLE_ARCHITECTURE_X64,
        "got architecture %u.\n", package->architecture );
    ok( !lstrcmpW( package->file_name, L"payload\\app.msix" ),
        "got file name %s.\n", wine_dbgstr_w(package->file_name) );
    ok( package->flags == APPX_BUNDLE_PACKAGE_HAS_RANGE,
        "got flags %#x.\n", package->flags );
    ok( package->resource_count == 1, "got %u application resources.\n",
        package->resource_count );
    ok( package->offset == 0 && package->size == 4096,
        "got range %s+%s.\n", wine_dbgstr_longlong(package->offset),
        wine_dbgstr_longlong(package->size) );

    package = p_appx_bundle_manifest_get_package( manifest, 1 );
    ok( package->type == APPX_BUNDLE_PACKAGE_RESOURCE,
        "got type %u.\n", package->type );
    ok( package->resource_count == 1, "got %u resources.\n",
        package->resource_count );
    ok( !lstrcmpW( package->resources[0].language, L"en-US" ),
        "got language %s.\n",
        wine_dbgstr_w(package->resources[0].language) );
    ok( package->resources[0].has_scale &&
        package->resources[0].scale == 225, "got scale %u.\n",
        package->resources[0].scale );
    ok( !p_appx_bundle_manifest_get_package( manifest, 2 ),
        "got out-of-range package.\n" );

    policy = exact_policy( APPX_BUNDLE_ARCHITECTURE_X64, L"en-us", 225 );
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( selection.payload ==
        p_appx_bundle_manifest_get_package( manifest, 0 ),
        "got payload %p.\n", selection.payload );
    ok( selection.package_index == 0, "got index %u.\n",
        selection.package_index );
    ok( selection.matching_resource_count == 1,
        "got %u matching resources.\n",
        selection.matching_resource_count );
    ok( (selection.issues &
         (APPX_BUNDLE_SELECTION_RESOURCE_PAYLOAD |
          APPX_BUNDLE_SELECTION_MATCHING_RESOURCE_PAYLOAD)) ==
        (APPX_BUNDLE_SELECTION_RESOURCE_PAYLOAD |
         APPX_BUNDLE_SELECTION_MATCHING_RESOURCE_PAYLOAD),
        "got issues %#x.\n", selection.issues );

    policy = neutral_policy( APPX_BUNDLE_ARCHITECTURE_X64 );
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( !selection.matching_resource_count,
        "got %u matching resources.\n",
        selection.matching_resource_count );
    p_appx_bundle_manifest_free( manifest );
}

static void test_schema_defaults_and_uniqueness(void)
{
    static const char defaults_and_reverse_order[] =
        BUNDLE_OPEN
        "<Packages><Package Version=\"1.0.0.0\" FileName=\"default.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES
        "</Package></Packages>"
        BUNDLE_IDENTITY BUNDLE_CLOSE;
    static const char *duplicates[] =
    {
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
        " FileName=\"language.msix\" Offset=\"0\" Size=\"1\"><Resources>"
        "<Resource Language=\"en-US\" Scale=\"100\"/>"
        "<Resource Language=\"en-US\" Scale=\"225\"/>"
        "</Resources></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
        " FileName=\"scale.msix\" Offset=\"0\" Size=\"1\"><Resources>"
        "<Resource Language=\"en-US\" Scale=\"100\"/>"
        "<Resource Language=\"de-DE\" Scale=\"100\"/>"
        "</Resources></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
        " FileName=\"dx.msix\" Offset=\"0\" Size=\"1\"><Resources>"
        "<Resource Language=\"en-US\" Scale=\"100\""
        " DXFeatureLevel=\"dx11\"/>"
        "<Resource Language=\"de-DE\" Scale=\"225\""
        " DXFeatureLevel=\"dx11\"/>"
        "</Resources></Package></Packages>" BUNDLE_CLOSE
    };
    const struct appx_bundle_package *package;
    struct appx_bundle_selection_policy policy;
    struct appx_bundle_selection selection;
    APPX_BUNDLE_MANIFEST *manifest = NULL;
    HRESULT hr;
    UINT32 i;

    hr = parse_text( defaults_and_reverse_order, &manifest );
    ok( hr == S_OK, "defaults got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        package = p_appx_bundle_manifest_get_package( manifest, 0 );
        ok( package->type == APPX_BUNDLE_PACKAGE_RESOURCE,
            "got default type %u.\n", package->type );
        ok( !lstrcmpW( package->type_name, L"resource" ),
            "got type name %s.\n", wine_dbgstr_w(package->type_name) );
        ok( package->architecture == APPX_BUNDLE_ARCHITECTURE_NEUTRAL,
            "got default architecture %u.\n", package->architecture );
        ok( !lstrcmpW( package->architecture_name, L"neutral" ),
            "got architecture name %s.\n",
            wine_dbgstr_w(package->architecture_name) );
        ok( package->resource_count == 1, "got %u resources.\n",
            package->resource_count );
        ok( !package->resources[0].language &&
            !package->resources[0].has_scale &&
            !package->resources[0].dx_feature_level,
            "neutral resource unexpectedly has a qualifier.\n" );
        policy = exact_policy( APPX_BUNDLE_ARCHITECTURE_X64, L"ja-JP", 225 );
        init_selection( &selection );
        hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
        ok( hr == HRESULT_FROM_WIN32( ERROR_NOT_FOUND ),
            "neutral resource select got hr %#lx.\n", hr );
        ok( selection.matching_resource_count == 1,
            "got %u matching neutral resources.\n",
            selection.matching_resource_count );
        p_appx_bundle_manifest_free( manifest );
    }

    for (i = 0; i < ARRAY_SIZE(duplicates); i++)
    {
        manifest = NULL;
        hr = parse_text( duplicates[i], &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "duplicate test %u got hr %#lx and manifest %p.\n",
            i, hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
}

static void test_package_schema_values(void)
{
    static const char resource_format[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
        " ResourceId=\"r\" FileName=\"r.msix\" Offset=\"0\" Size=\"1\">"
        "<Resources><Resource %s=\"%s\"/></Resources>"
        "</Package></Packages>" BUNDLE_CLOSE;
    static const char resource_v2_format[] =
        "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\""
        " SchemaVersion=\"2.0\">" BUNDLE_IDENTITY
        "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
        " ResourceId=\"r\" FileName=\"r.msix\" Offset=\"0\" Size=\"1\">"
        "<Resources><Resource %s=\"%s\"/></Resources>"
        "</Package></Packages>" BUNDLE_CLOSE;
    static const char file_name_format[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"%s\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "</Package></Packages>" BUNDLE_CLOSE;
    static const char *valid_scales_2013[] =
    {
        "100", "120", "140", "150", "160", "180", "225"
    };
    static const char *valid_scales_v2[] =
    {
        "100", "120", "125", "140", "150", "160", "180", "200",
        "220", "225", "240", "250", "300", "400", "500"
    };
    static const char *v2_only_scales[] =
    {
        "125", "200", "220", "240", "250", "300", "400", "500"
    };
    static const char *invalid_scales[] =
    {
        "0", "99", "130", "175", "226", "450", "1000", "0100", " 100"
    };
    static const char *valid_dx[] = {"dx9", "dx10", "dx11"};
    static const char *invalid_dx[] = {"", "DX9", "dx8", "dx12", "dx_9"};
    static const char *invalid_packages[] =
    {
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"framework\" Version=\"1.0.0.0\""
        " FileName=\"f.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"optional\" Version=\"1.0.0.0\""
        " FileName=\"o.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"future\" Version=\"1.0.0.0\""
        " FileName=\"f.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " ResourceId=\"\" FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
        " ResourceId=\"bad_id\" FileName=\"r.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
        " ResourceId=\"bad/id\" FileName=\"r.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\" Encrypted=\"true\">"
        NEUTRAL_RESOURCES "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\""
        " xmlns:b=\"http://schemas.microsoft.com/appx/2013/bundle\""
        " b:Unknown=\"x\">" NEUTRAL_RESOURCES
        "</Package></Packages>" BUNDLE_CLOSE
    };
    APPX_BUNDLE_MANIFEST *manifest;
    char file_name[258], text[2048];
    HRESULT hr;
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(valid_scales_2013); i++)
    {
        sprintf( text, resource_format, "Scale", valid_scales_2013[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == S_OK, "valid Scale %s got hr %#lx.\n",
            valid_scales_2013[i], hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
    for (i = 0; i < ARRAY_SIZE(v2_only_scales); i++)
    {
        sprintf( text, resource_format, "Scale", v2_only_scales[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "v1-only invalid Scale %s got hr %#lx and manifest %p.\n",
            v2_only_scales[i], hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
    for (i = 0; i < ARRAY_SIZE(valid_scales_v2); i++)
    {
        sprintf( text, resource_v2_format, "Scale", valid_scales_v2[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == S_OK, "valid v2 Scale %s got hr %#lx.\n",
            valid_scales_v2[i], hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
    for (i = 0; i < ARRAY_SIZE(invalid_scales); i++)
    {
        sprintf( text, resource_format, "Scale", invalid_scales[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "invalid Scale %s got hr %#lx and manifest %p.\n",
            invalid_scales[i], hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
    for (i = 0; i < ARRAY_SIZE(valid_dx); i++)
    {
        sprintf( text, resource_format, "DXFeatureLevel", valid_dx[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == S_OK, "valid DXFeatureLevel %s got hr %#lx.\n",
            valid_dx[i], hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
    for (i = 0; i < ARRAY_SIZE(invalid_dx); i++)
    {
        sprintf( text, resource_format, "DXFeatureLevel", invalid_dx[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "invalid DXFeatureLevel %s got hr %#lx and manifest %p.\n",
            invalid_dx[i], hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
    for (i = 0; i < ARRAY_SIZE(invalid_packages); i++)
    {
        manifest = NULL;
        hr = parse_text( invalid_packages[i], &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "invalid package test %u got hr %#lx and manifest %p.\n",
            i, hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }

    memset( file_name, 'a', 249 );
    strcpy( file_name + 249, "/a.msix" );
    ok( strlen(file_name) == 256, "got FileName length %u.\n",
        (UINT32)strlen(file_name) );
    sprintf( text, file_name_format, file_name );
    manifest = NULL;
    hr = parse_text( text, &manifest );
    ok( hr == S_OK, "256-character FileName got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );

    memset( file_name, 'a', 250 );
    strcpy( file_name + 250, "/a.msix" );
    ok( strlen(file_name) == 257, "got FileName length %u.\n",
        (UINT32)strlen(file_name) );
    sprintf( text, file_name_format, file_name );
    manifest = NULL;
    hr = parse_text( text, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "257-character FileName got hr %#lx and manifest %p.\n",
        hr, manifest );
    if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
}

static void test_stub_and_dependencies(void)
{
    static const char stub_format[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"a.msix\" Offset=\"0\" Size=\"1\""
        " IsStub=\"%s\">" NEUTRAL_RESOURCES
        "</Package></Packages>" BUNDLE_CLOSE;
    static const char dependencies[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        "<Dependencies>"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.19041.0\" MaxVersionTested=\"10.0.26100.0\"/>"
        "<TargetDeviceFamily Name=\"Windows.Universal\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.26100.0\"/>"
        "</Dependencies>" NEUTRAL_RESOURCES
        "</Package></Packages>" BUNDLE_CLOSE;
    static const char *invalid_booleans[] =
    {
        "", "TRUE", "False", "yes", "-1", "2"
    };
    static const char *invalid_dependencies[] =
    {
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies Unknown=\"x\">"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies><Other/></Dependencies>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies>text"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies>"
        "<TargetDeviceFamily MinVersion=\"10.0.0.0\""
        " MaxVersionTested=\"10.0.0.0\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies>"
        "<TargetDeviceFamily Name=\"Windows_Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies>"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0\" MaxVersionTested=\"10.0.0.0\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies>"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies>"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\""
        " Unknown=\"x\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies>"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\">"
        "<Other/></TargetDeviceFamily>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "<Dependencies>"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\"/>"
        "<TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES
        "<Dependencies><TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\"/>"
        "</Dependencies>"
        "<Dependencies><TargetDeviceFamily Name=\"Windows.Universal\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.0.0\"/>"
        "</Dependencies></Package></Packages>" BUNDLE_CLOSE
    };
    const struct appx_bundle_package *package;
    struct appx_bundle_selection_policy policy;
    struct appx_bundle_selection selection;
    APPX_BUNDLE_MANIFEST *manifest;
    char text[2048];
    HRESULT hr;
    UINT32 i;

    policy = neutral_policy( APPX_BUNDLE_ARCHITECTURE_X64 );
    for (i = 0; i < 2; i++)
    {
        sprintf( text, stub_format, i ? "0" : "false" );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == S_OK, "IsStub=%s got hr %#lx.\n",
            i ? "0" : "false", hr );
        if (SUCCEEDED(hr))
        {
            package = p_appx_bundle_manifest_get_package( manifest, 0 );
            ok( !(package->flags & APPX_BUNDLE_PACKAGE_STUB),
                "IsStub=%s got flags %#x.\n", i ? "0" : "false",
                package->flags );
            init_selection( &selection );
            hr = p_appx_bundle_manifest_select( manifest, &policy,
                                                &selection );
            ok( hr == S_OK, "IsStub=%s select got hr %#lx.\n",
                i ? "0" : "false", hr );
            p_appx_bundle_manifest_free( manifest );
        }
    }
    for (i = 0; i < 2; i++)
    {
        sprintf( text, stub_format, i ? "1" : "true" );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == S_OK, "IsStub=%s got hr %#lx.\n",
            i ? "1" : "true", hr );
        if (SUCCEEDED(hr))
        {
            package = p_appx_bundle_manifest_get_package( manifest, 0 );
            ok( package->flags & APPX_BUNDLE_PACKAGE_STUB,
                "IsStub=%s got flags %#x.\n", i ? "1" : "true",
                package->flags );
            init_selection( &selection );
            hr = p_appx_bundle_manifest_select( manifest, &policy,
                                                &selection );
            ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                "IsStub=%s select got hr %#lx.\n",
                i ? "1" : "true", hr );
            ok( !selection.payload &&
                (selection.issues & APPX_BUNDLE_SELECTION_STUB_PAYLOAD),
                "IsStub=%s got payload %p and issues %#x.\n",
                i ? "1" : "true", selection.payload, selection.issues );
            p_appx_bundle_manifest_free( manifest );
        }
    }
    for (i = 0; i < ARRAY_SIZE(invalid_booleans); i++)
    {
        sprintf( text, stub_format, invalid_booleans[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "invalid IsStub=%s got hr %#lx and manifest %p.\n",
            invalid_booleans[i], hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }

    manifest = NULL;
    hr = parse_text( dependencies, &manifest );
    ok( hr == S_OK, "Dependencies got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        package = p_appx_bundle_manifest_get_package( manifest, 0 );
        ok( package->flags & APPX_BUNDLE_PACKAGE_HAS_DEPENDENCIES,
            "Dependencies got flags %#x.\n", package->flags );
        init_selection( &selection );
        hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
        ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            "Dependencies select got hr %#lx.\n", hr );
        ok( !selection.payload &&
            (selection.issues &
             APPX_BUNDLE_SELECTION_DEPENDENCY_PAYLOAD),
            "Dependencies got payload %p and issues %#x.\n",
            selection.payload, selection.issues );
        p_appx_bundle_manifest_free( manifest );
    }

    for (i = 0; i < ARRAY_SIZE(invalid_dependencies); i++)
    {
        manifest = NULL;
        hr = parse_text( invalid_dependencies[i], &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "invalid Dependencies test %u got hr %#lx and manifest %p.\n",
            i, hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
}

static void test_schema_versions(void)
{
    static const char manifest_format[] =
        "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\""
        " SchemaVersion=\"%s\">" BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        NEUTRAL_RESOURCES "</Package></Packages>" BUNDLE_CLOSE;
    static const char *valid[] =
    {
        "0.0", "1.0", "1.2.3", "65535.65535",
        "65535.0.65535"
    };
    static const char *invalid[] =
    {
        "", "1", "1.", ".1", "1.2.", "1..2", "1.2.3.4",
        "00.0", "01.2", "1.00", "1.2.03",
        "65536.0", "0.65536", "0.0.65536",
        "+1.0", "1.-1", "1.0x"
    };
    APPX_BUNDLE_MANIFEST *manifest;
    char text[1024];
    HRESULT hr;
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(valid); i++)
    {
        sprintf( text, manifest_format, valid[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == S_OK, "valid version %s got hr %#lx.\n", valid[i], hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
    for (i = 0; i < ARRAY_SIZE(invalid); i++)
    {
        sprintf( text, manifest_format, invalid[i] );
        manifest = NULL;
        hr = parse_text( text, &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "invalid version %s got hr %#lx and manifest %p.\n",
            invalid[i], hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
}

static void test_required_grammar(void)
{
    static const char *tests[] =
    {
        "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\">"
        BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>"
        BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY BUNDLE_CLOSE,
        BUNDLE_OPEN "<Packages/>" BUNDLE_IDENTITY BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY "<Packages/>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Offset=\"0\" Size=\"1\"><Resources/></Package></Packages>"
        BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Size=\"1\"><Resources/></Package></Packages>"
        BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\"><Resources/></Package></Packages>"
        BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"/></Packages>"
        BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Other/></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        "<Other/><Resources/></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
        " FileName=\"r.msix\" Offset=\"0\" Size=\"1\">"
        "<Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\" Unknown=\"x\">"
        "<Resources/></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Unexpected/><Packages><Package Type=\"application\""
        " Version=\"1.0.0.0\" FileName=\"a.msix\" Offset=\"0\""
        " Size=\"1\"><Resources/></Package></Packages>"
        BUNDLE_CLOSE
    };
    APPX_BUNDLE_MANIFEST *manifest;
    HRESULT hr;
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        manifest = NULL;
        hr = parse_text( tests[i], &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "test %u got hr %#lx and manifest %p.\n", i, hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
}

static void test_versions_ranges_and_duplicates(void)
{
    static const char *tests[] =
    {
        BUNDLE_OPEN
        "<Identity Name=\"Wine.Bundle\" Publisher=\"CN=Wine\""
        " Version=\"1.2.3\"/>"
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"65536.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0x\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\"/></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"0\"><Resources/>"
        "</Package></Packages>"
        BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"18446744073709551615\""
        " Size=\"1\"><Resources/></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"100\"><Resources/>"
        "</Package>"
        "<Package Type=\"resource\" Version=\"1.0.0.0\""
        " ResourceId=\"r\" FileName=\"r.msix\" Offset=\"99\" Size=\"1\">"
        "<Resources/></Package>"
        "</Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
        "<Resources/></Package>"
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"b.msix\" Offset=\"1\" Size=\"1\">"
        "<Resources/></Package>"
        "</Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"Path/App.msix\""
        " Offset=\"0\" Size=\"1\"><Resources/></Package>"
        "<Package Type=\"resource\" Version=\"1.0.0.0\" ResourceId=\"r\""
        " FileName=\"path/app.MSIX\" Offset=\"1\" Size=\"1\">"
        "<Resources/></Package>"
        "</Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"payload/%41pp.msix\""
        " Offset=\"0\" Size=\"1\"><Resources/></Package>"
        "<Package Type=\"resource\" Version=\"1.0.0.0\" ResourceId=\"r\""
        " FileName=\"payload/app.msix\" Offset=\"1\" Size=\"1\">"
        "<Resources/></Package>"
        "</Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"payload%2fapp.msix\""
        " Offset=\"0\" Size=\"1\"><Resources/></Package>"
        "</Packages>" BUNDLE_CLOSE
    };
    static const char boundary[] =
        BUNDLE_OPEN
        "<Identity Name=\"Wine.Bundle\" Publisher=\"CN=Wine\""
        " Version=\"65535.65535.65535.65535\"/>"
        "<Packages><Package Type=\"application\""
        " Version=\"65535.65535.65535.65535\""
        " Architecture=\"x64\" FileName=\"a.msix\""
        " Offset=\"18446744073709551614\" Size=\"1\">"
        NEUTRAL_RESOURCES
        "</Package>"
        "</Packages>" BUNDLE_CLOSE;
    APPX_BUNDLE_MANIFEST *manifest;
    HRESULT hr;
    UINT32 i;

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        manifest = NULL;
        hr = parse_text( tests[i], &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "test %u got hr %#lx and manifest %p.\n", i, hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
    hr = parse_text( boundary, &manifest );
    ok( hr == S_OK, "boundary got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
}

static void test_security(void)
{
    static const BYTE embedded_nul[] =
        BUNDLE_OPEN "\0" BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE;
    static const BYTE invalid_utf8[] =
        BUNDLE_OPEN "\xc0\xaf" BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE;
    static const char *tests[] =
    {
        "<!DOCTYPE Bundle [<!ENTITY x SYSTEM \"file:///etc/passwd\">]>"
        BUNDLE_OPEN
        "<Identity Name=\"Wine.Bundle\" Publisher=\"&x;\" Version=\"1.0.0.0\"/>"
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN "<?attack value?>" BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN "<![CDATA[ ]]>" BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\""
        " xmlns:xi=\"http://www.w3.org/2001/XInclude\" SchemaVersion=\"1.0\">"
        BUNDLE_IDENTITY "<xi:include href=\"file:///etc/passwd\"/>"
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"../escape.msix\" Offset=\"0\" Size=\"1\">"
        "<Resources/></Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"NUL.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE,
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " FileName=\"a%00.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
        "</Package></Packages>" BUNDLE_CLOSE
    };
    APPX_BUNDLE_MANIFEST *manifest;
    HRESULT hr;
    UINT32 i;

    hr = p_appx_bundle_manifest_parse( embedded_nul,
                                      sizeof(embedded_nul) - 1, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "NUL got hr %#lx.\n", hr );
    hr = p_appx_bundle_manifest_parse( invalid_utf8,
                                      sizeof(invalid_utf8) - 1, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "UTF-8 got hr %#lx.\n", hr );
    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        manifest = NULL;
        hr = parse_text( tests[i], &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "test %u got hr %#lx and manifest %p.\n", i, hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
    }
}

static void test_limits(void)
{
    APPX_BUNDLE_MANIFEST *manifest = NULL;
    char *buffer, *cursor;
    HRESULT hr;
    UINT32 i;

    buffer = HeapAlloc( GetProcessHeap(), 0, 16384 );
    ok( !!buffer, "failed to allocate depth buffer.\n" );
    if (buffer)
    {
        cursor = buffer;
        cursor += sprintf( cursor,
            "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\""
            " xmlns:f=\"urn:wine:future\" IgnorableNamespaces=\"f\""
            " SchemaVersion=\"1.0\">"
            BUNDLE_IDENTITY );
        for (i = 0; i < 33; i++) cursor += sprintf( cursor, "<f:N>" );
        for (i = 0; i < 33; i++) cursor += sprintf( cursor, "</f:N>" );
        cursor += sprintf( cursor,
            "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
            " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
            "</Package></Packages>" BUNDLE_CLOSE );
        hr = p_appx_bundle_manifest_parse( (BYTE *)buffer, cursor - buffer,
                                           &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "deep tree got hr %#lx.\n", hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );

        cursor = buffer;
        cursor += sprintf( cursor,
            "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\""
            " xmlns:f=\"urn:wine:future\" IgnorableNamespaces=\"f\""
            " SchemaVersion=\"1.0\">"
            BUNDLE_IDENTITY "<f:N" );
        for (i = 0; i < 65; i++)
            cursor += sprintf( cursor, " a%u=\"x\"", i );
        cursor += sprintf( cursor,
            "/><Packages><Package Type=\"application\" Version=\"1.0.0.0\""
            " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
            "</Package></Packages>" BUNDLE_CLOSE );
        hr = p_appx_bundle_manifest_parse( (BYTE *)buffer, cursor - buffer,
                                           &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "attribute limit got hr %#lx.\n", hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
        HeapFree( GetProcessHeap(), 0, buffer );
    }

    buffer = HeapAlloc( GetProcessHeap(), 0, 70000 );
    ok( !!buffer, "failed to allocate value buffer.\n" );
    if (buffer)
    {
        cursor = buffer;
        cursor += sprintf( cursor, "<!--" );
        memset( cursor, 'x', 65537 );
        cursor += 65537;
        cursor += sprintf( cursor, "-->" BUNDLE_OPEN BUNDLE_IDENTITY
            "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
            " FileName=\"a.msix\" Offset=\"0\" Size=\"1\"><Resources/>"
            "</Package></Packages>" BUNDLE_CLOSE );
        hr = p_appx_bundle_manifest_parse( (BYTE *)buffer, cursor - buffer,
                                           &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "value limit got hr %#lx.\n", hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
        HeapFree( GetProcessHeap(), 0, buffer );
    }

    buffer = HeapAlloc( GetProcessHeap(), 0, 1024 * 1024 );
    ok( !!buffer, "failed to allocate package buffer.\n" );
    if (buffer)
    {
        cursor = buffer;
        cursor += sprintf( cursor, BUNDLE_OPEN BUNDLE_IDENTITY "<Packages>" );
        for (i = 0; i <= APPX_BUNDLE_MANIFEST_MAX_PACKAGES; i++)
            cursor += sprintf( cursor,
                "<Package Type=\"resource\" Version=\"1.0.0.0\""
                " ResourceId=\"r%u\" FileName=\"p%u.msix\""
                " Offset=\"%u\" Size=\"1\">" NEUTRAL_RESOURCES
                "</Package>",
                i, i, i );
        cursor += sprintf( cursor, "</Packages>" BUNDLE_CLOSE );
        ok( cursor - buffer < 1024 * 1024, "package buffer overflowed.\n" );
        hr = p_appx_bundle_manifest_parse( (BYTE *)buffer, cursor - buffer,
                                           &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "package limit got hr %#lx.\n", hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
        HeapFree( GetProcessHeap(), 0, buffer );
    }

    buffer = HeapAlloc( GetProcessHeap(), 0, 16384 );
    ok( !!buffer, "failed to allocate resource buffer.\n" );
    if (buffer)
    {
        cursor = buffer;
        cursor += sprintf( cursor, BUNDLE_OPEN BUNDLE_IDENTITY
            "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
            " ResourceId=\"r\" FileName=\"r.msix\" Offset=\"0\" Size=\"1\">"
            "<Resources>" );
        for (i = 0; i < 200; i++)
            cursor += sprintf( cursor, "<Resource/>" );
        cursor += sprintf( cursor, "</Resources></Package></Packages>"
                           BUNDLE_CLOSE );
        hr = p_appx_bundle_manifest_parse( (BYTE *)buffer, cursor - buffer,
                                           &manifest );
        ok( hr == S_OK, "200 resources got hr %#lx.\n", hr );
        if (SUCCEEDED(hr))
        {
            ok( p_appx_bundle_manifest_get_package( manifest, 0 )
                ->resource_count == 200, "got %u resources.\n",
                p_appx_bundle_manifest_get_package( manifest, 0 )
                ->resource_count );
            p_appx_bundle_manifest_free( manifest );
        }

        cursor = buffer;
        cursor += sprintf( cursor, BUNDLE_OPEN BUNDLE_IDENTITY
            "<Packages><Package Type=\"resource\" Version=\"1.0.0.0\""
            " ResourceId=\"r\" FileName=\"r.msix\" Offset=\"0\" Size=\"1\">"
            "<Resources>" );
        for (i = 0; i < 201; i++)
            cursor += sprintf( cursor, "<Resource/>" );
        cursor += sprintf( cursor, "</Resources></Package></Packages>"
                           BUNDLE_CLOSE );
        manifest = NULL;
        hr = p_appx_bundle_manifest_parse( (BYTE *)buffer, cursor - buffer,
                                           &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "201 resources got hr %#lx and manifest %p.\n", hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
        HeapFree( GetProcessHeap(), 0, buffer );
    }

    buffer = HeapAlloc( GetProcessHeap(), 0, 65536 );
    ok( !!buffer, "failed to allocate dependency buffer.\n" );
    if (buffer)
    {
        cursor = buffer;
        cursor += sprintf( cursor, BUNDLE_OPEN BUNDLE_IDENTITY
            "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
            " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
            NEUTRAL_RESOURCES "<Dependencies>" );
        for (i = 0; i < 128; i++)
            cursor += sprintf( cursor,
                "<TargetDeviceFamily Name=\"Windows.Family%u\""
                " MinVersion=\"10.0.0.0\""
                " MaxVersionTested=\"10.0.26100.0\"/>", i );
        cursor += sprintf( cursor,
            "</Dependencies></Package></Packages>" BUNDLE_CLOSE );
        ok( cursor - buffer < 65536, "dependency buffer overflowed.\n" );
        manifest = NULL;
        hr = p_appx_bundle_manifest_parse( (BYTE *)buffer, cursor - buffer,
                                           &manifest );
        ok( hr == S_OK, "128 dependencies got hr %#lx.\n", hr );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );

        cursor = buffer;
        cursor += sprintf( cursor, BUNDLE_OPEN BUNDLE_IDENTITY
            "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
            " FileName=\"a.msix\" Offset=\"0\" Size=\"1\">"
            NEUTRAL_RESOURCES "<Dependencies>" );
        for (i = 0; i < 129; i++)
            cursor += sprintf( cursor,
                "<TargetDeviceFamily Name=\"Windows.Family%u\""
                " MinVersion=\"10.0.0.0\""
                " MaxVersionTested=\"10.0.26100.0\"/>", i );
        cursor += sprintf( cursor,
            "</Dependencies></Package></Packages>" BUNDLE_CLOSE );
        ok( cursor - buffer < 65536, "dependency buffer overflowed.\n" );
        manifest = NULL;
        hr = p_appx_bundle_manifest_parse( (BYTE *)buffer, cursor - buffer,
                                           &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "129 dependencies got hr %#lx and manifest %p.\n",
            hr, manifest );
        if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
        HeapFree( GetProcessHeap(), 0, buffer );
    }
}

static void test_unsupported_payloads(void)
{
    static const char manifest_text[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages>"
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"app.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"resource\" Version=\"1.0.0.0\" ResourceId=\"r\""
        " Architecture=\"neutral\" FileName=\"resource.msix\""
        " Offset=\"1\" Size=\"1\">"
        "<Resources><Resource DXFeatureLevel=\"dx11\"/></Resources>"
        "</Package>"
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"riscv64\" FileName=\"future.msix\""
        " Offset=\"2\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "</Packages>" BUNDLE_CLOSE;
    const struct appx_bundle_package *package;
    struct appx_bundle_selection_policy policy;
    struct appx_bundle_selection selection;
    APPX_BUNDLE_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( manifest_text, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    package = p_appx_bundle_manifest_get_package( manifest, 1 );
    ok( package->flags & APPX_BUNDLE_PACKAGE_UNSUPPORTED_QUALIFIER,
        "got flags %#x.\n", package->flags );
    package = p_appx_bundle_manifest_get_package( manifest, 2 );
    ok( package->type == APPX_BUNDLE_PACKAGE_APPLICATION,
        "got type %u.\n", package->type );
    ok( package->architecture == APPX_BUNDLE_ARCHITECTURE_UNSUPPORTED,
        "got architecture %u.\n", package->architecture );
    ok( package->flags & APPX_BUNDLE_PACKAGE_UNSUPPORTED_ARCHITECTURE,
        "got flags %#x.\n", package->flags );

    policy = neutral_policy( APPX_BUNDLE_ARCHITECTURE_X64 );
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ok( selection.payload ==
        p_appx_bundle_manifest_get_package( manifest, 0 ),
        "got payload %p.\n", selection.payload );
    ok( (selection.issues &
         (APPX_BUNDLE_SELECTION_RESOURCE_PAYLOAD |
          APPX_BUNDLE_SELECTION_UNSUPPORTED_ARCHITECTURE |
          APPX_BUNDLE_SELECTION_UNSUPPORTED_QUALIFIER)) ==
        (APPX_BUNDLE_SELECTION_RESOURCE_PAYLOAD |
         APPX_BUNDLE_SELECTION_UNSUPPORTED_ARCHITECTURE |
         APPX_BUNDLE_SELECTION_UNSUPPORTED_QUALIFIER),
        "got issues %#x.\n", selection.issues );
    p_appx_bundle_manifest_free( manifest );
}

static void test_selection(void)
{
    static const char ranked[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages>"
        "<Package Type=\"application\" Version=\"9.0.0.0\""
        " Architecture=\"neutral\" FileName=\"neutral.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"old.msix\""
        " Offset=\"1\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"new.msix\""
        " Offset=\"2\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"8.0.0.0\""
        " Architecture=\"arm64\" FileName=\"arm.msix\""
        " Offset=\"3\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "</Packages>" BUNDLE_CLOSE;
    static const char invalid_framework[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages>"
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"framework\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"framework.msix\""
        " Offset=\"1\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "</Packages>" BUNDLE_CLOSE;
    static const char incompatible[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"arm64\" FileName=\"arm.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES
        "</Package></Packages>"
        BUNDLE_CLOSE;
    static const char ignorable[] =
        "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\""
        " xmlns:f=\"urn:wine:future\" IgnorableNamespaces=\"f\""
        " SchemaVersion=\"1.0\">"
        BUNDLE_IDENTITY
        "<Packages><Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"app.msix\" Offset=\"0\""
        " Size=\"1\" f:Mode=\"future\">" NEUTRAL_RESOURCES "</Package>"
        "</Packages>" BUNDLE_CLOSE;
    struct appx_bundle_selection_policy policy;
    struct appx_bundle_selection selection;
    APPX_BUNDLE_MANIFEST *manifest;
    HRESULT hr;

    policy = neutral_policy( APPX_BUNDLE_ARCHITECTURE_X64 );
    hr = parse_text( ranked, &manifest );
    ok( hr == S_OK, "ranked parse got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        init_selection( &selection );
        hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
        ok( hr == S_OK, "ranked select got hr %#lx.\n", hr );
        ok( selection.package_index == 2, "got index %u.\n",
            selection.package_index );
        ok( !lstrcmpW( selection.payload->file_name, L"new.msix" ),
            "got file %s.\n", wine_dbgstr_w(selection.payload->file_name) );
        p_appx_bundle_manifest_free( manifest );
    }

    manifest = NULL;
    hr = parse_text( invalid_framework, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "framework parse got hr %#lx and manifest %p.\n", hr, manifest );
    if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );

    hr = parse_text( incompatible, &manifest );
    ok( hr == S_OK, "incompatible parse got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        init_selection( &selection );
        hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
        ok( hr == HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED ),
            "incompatible select got hr %#lx.\n", hr );
        ok( selection.issues &
            APPX_BUNDLE_SELECTION_INCOMPATIBLE_ARCHITECTURE,
            "got issues %#x.\n", selection.issues );
        p_appx_bundle_manifest_free( manifest );
    }

    hr = parse_text( ignorable, &manifest );
    ok( hr == S_OK, "ignorable parse got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        init_selection( &selection );
        hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
        ok( hr == HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED ),
            "ignorable select got hr %#lx.\n", hr );
        ok( selection.issues &
            APPX_BUNDLE_SELECTION_UNSUPPORTED_EXTENSION,
            "got issues %#x.\n", selection.issues );
        p_appx_bundle_manifest_free( manifest );
    }

    policy = exact_policy( APPX_BUNDLE_ARCHITECTURE_X64, NULL, 100 );
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( NULL, &policy, &selection );
    ok( hr == E_INVALIDARG, "invalid language got hr %#lx.\n", hr );
    policy = exact_policy( APPX_BUNDLE_ARCHITECTURE_X64, L"en-US", 0 );
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( NULL, &policy, &selection );
    ok( hr == E_INVALIDARG, "invalid scale got hr %#lx.\n", hr );
}

static void test_architecture_capability_policy(void)
{
    static const char ranked[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages>"
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"arm64\" FileName=\"native.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"9.0.0.0\""
        " Architecture=\"neutral\" FileName=\"neutral.msix\""
        " Offset=\"1\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"8.0.0.0\""
        " Architecture=\"x64\" FileName=\"x64.msix\""
        " Offset=\"2\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"8.0.0.0\""
        " Architecture=\"x86a64\" FileName=\"x86a64.msix\""
        " Offset=\"3\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"8.0.0.0\""
        " Architecture=\"x86\" FileName=\"x86.msix\""
        " Offset=\"4\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "</Packages>" BUNDLE_CLOSE;
    static const char guests[] =
        BUNDLE_OPEN BUNDLE_IDENTITY
        "<Packages>"
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x86\" FileName=\"x86.msix\""
        " Offset=\"0\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"99.0.0.0\""
        " Architecture=\"x86a64\" FileName=\"x86a64.msix\""
        " Offset=\"1\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"x64.msix\""
        " Offset=\"2\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"99.0.0.0\""
        " Architecture=\"arm\" FileName=\"arm.msix\""
        " Offset=\"3\" Size=\"1\">" NEUTRAL_RESOURCES "</Package>"
        "</Packages>" BUNDLE_CLOSE;
    const struct appx_bundle_package *package;
    struct appx_bundle_selection_policy policy;
    struct appx_bundle_selection selection;
    APPX_BUNDLE_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( ranked, &manifest );
    ok( hr == S_OK, "ranked parse got hr %#lx.\n", hr );
    if (FAILED(hr)) return;

    package = p_appx_bundle_manifest_get_package( manifest, 3 );
    ok( package->architecture == APPX_BUNDLE_ARCHITECTURE_X86A64,
        "x86a64 parsed as %u.\n", package->architecture );
    ok( !(package->flags & APPX_BUNDLE_PACKAGE_UNSUPPORTED_ARCHITECTURE),
        "x86a64 got flags %#x.\n", package->flags );

    policy = neutral_policy( APPX_BUNDLE_ARCHITECTURE_ARM64 );
    policy.architecture_policy.value.version =
        APPX_BUNDLE_ARCHITECTURE_POLICY_VERSION;
    policy.architecture_policy.value.supported_architectures =
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_ARM64) |
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X64) |
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_ARM) |
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X86A64) |
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X86);
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == S_OK, "capability select got hr %#lx.\n", hr );
    ok( selection.package_index == 0,
        "native preference selected index %u.\n", selection.package_index );

    policy.host_architecture = APPX_BUNDLE_ARCHITECTURE_X64;
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == S_OK, "x64 preference select got hr %#lx.\n", hr );
    ok( selection.package_index == 2,
        "x64 preference selected index %u.\n", selection.package_index );

    p_appx_bundle_manifest_free( manifest );
    manifest = NULL;

    hr = parse_text( guests, &manifest );
    ok( hr == S_OK, "guest parse got hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    policy = neutral_policy( APPX_BUNDLE_ARCHITECTURE_ARM64 );
    policy.architecture_policy.value.version =
        APPX_BUNDLE_ARCHITECTURE_POLICY_VERSION;
    policy.architecture_policy.value.supported_architectures =
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_ARM64) |
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X64) |
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_ARM) |
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X86A64) |
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X86);
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == S_OK, "guest select got hr %#lx.\n", hr );
    ok( selection.package_index == 2,
        "deterministic guest preference selected index %u.\n",
        selection.package_index );

    policy.architecture_policy.value.supported_architectures &=
        ~APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X64);
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == S_OK, "native ARM guest select got hr %#lx.\n", hr );
    ok( selection.package_index == 3,
        "ARM versus x86 guest preference selected index %u.\n",
        selection.package_index );

    memset( &policy.architecture_policy, 0xcc,
            sizeof(policy.architecture_policy) );
    policy.size = APPX_BUNDLE_SELECTION_POLICY_LEGACY_SIZE;
    policy.host_architecture = APPX_BUNDLE_ARCHITECTURE_X64;
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == S_OK && selection.package_index == 2,
        "legacy policy returned %#lx, index %u.\n",
        hr, selection.package_index );

    policy.size = APPX_BUNDLE_SELECTION_POLICY_LEGACY_SIZE - 1;
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == E_INVALIDARG, "truncated policy returned %#lx.\n", hr );
    policy.size = APPX_BUNDLE_SELECTION_POLICY_LEGACY_SIZE + 1;
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == E_INVALIDARG, "intermediate policy returned %#lx.\n", hr );
    policy.size = sizeof(policy) + 1;
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == E_INVALIDARG, "oversized policy returned %#lx.\n", hr );

    policy = neutral_policy( APPX_BUNDLE_ARCHITECTURE_X64 );
    policy.architecture_policy.value.version =
        APPX_BUNDLE_ARCHITECTURE_POLICY_VERSION + 1;
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == E_INVALIDARG, "unknown policy version returned %#lx.\n", hr );
    policy.architecture_policy.value.version =
        APPX_BUNDLE_ARCHITECTURE_POLICY_VERSION;
    policy.architecture_policy.value.supported_architectures =
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X64) |
        0x80000000u;
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == E_INVALIDARG, "unknown capability flag returned %#lx.\n", hr );
    policy.architecture_policy.value.supported_architectures =
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X86);
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == E_INVALIDARG, "missing preferred capability returned %#lx.\n",
        hr );
    policy.architecture_policy.value.version = 0;
    policy.architecture_policy.value.supported_architectures =
        APPX_BUNDLE_ARCHITECTURE_MASK(APPX_BUNDLE_ARCHITECTURE_X64);
    init_selection( &selection );
    hr = p_appx_bundle_manifest_select( manifest, &policy, &selection );
    ok( hr == E_INVALIDARG, "reserved extension returned %#lx.\n", hr );

    p_appx_bundle_manifest_free( manifest );
}

static BOOL load_functions(void)
{
    HMODULE module = LoadLibraryA( "appxsvc.dll" );

    ok( !!module, "failed to load appxsvc.dll, error %lu.\n", GetLastError() );
    if (!module) return FALSE;
#define LOAD(name) \
    do { \
        p_##name = (void *)GetProcAddress( module, #name ); \
        ok( !!p_##name, "failed to load %s, error %lu.\n", \
            #name, GetLastError() ); \
        if (!p_##name) return FALSE; \
    } while (0)
    LOAD(appx_bundle_manifest_parse);
    LOAD(appx_bundle_manifest_free);
    LOAD(appx_bundle_manifest_get_identity);
    LOAD(appx_bundle_manifest_get_package_count);
    LOAD(appx_bundle_manifest_get_package);
    LOAD(appx_bundle_manifest_select);
#undef LOAD
    return TRUE;
}

START_TEST(bundle_manifest)
{
    if (!load_functions())
    {
        win_skip( "AppX bundle manifest parser exports are unavailable.\n" );
        return;
    }
    test_arguments();
    test_valid_manifest();
    test_schema_defaults_and_uniqueness();
    test_package_schema_values();
    test_stub_and_dependencies();
    test_schema_versions();
    test_required_grammar();
    test_versions_ranges_and_duplicates();
    test_security();
    test_limits();
    test_unsupported_payloads();
    test_selection();
    test_architecture_capability_policy();
}
