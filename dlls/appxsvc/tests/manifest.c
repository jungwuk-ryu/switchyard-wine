/*
 * AppX manifest parser tests
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

#include "../manifest.h"
#include "wine/test.h"

static HRESULT (WINAPI *p_appx_manifest_parse)( const BYTE *, SIZE_T,
                                                 APPX_MANIFEST ** );
static void (WINAPI *p_appx_manifest_free)( APPX_MANIFEST * );
static const struct appx_manifest_identity * (WINAPI *p_appx_manifest_get_identity)(
    const APPX_MANIFEST * );
static BOOL (WINAPI *p_appx_manifest_is_supported)( const APPX_MANIFEST * );
static BOOL (WINAPI *p_appx_manifest_is_framework)( const APPX_MANIFEST * );
static BOOL (WINAPI *p_appx_manifest_is_resource_package)( const APPX_MANIFEST * );
static BOOL (WINAPI *p_appx_manifest_has_run_full_trust)( const APPX_MANIFEST * );
static UINT32 (WINAPI *p_appx_manifest_get_application_count)(
    const APPX_MANIFEST * );
static const struct appx_manifest_application *
    (WINAPI *p_appx_manifest_get_application)( const APPX_MANIFEST *, UINT32 );
static UINT32 (WINAPI *p_appx_manifest_get_dependency_count)(
    const APPX_MANIFEST * );
static const struct appx_manifest_dependency *
    (WINAPI *p_appx_manifest_get_dependency)( const APPX_MANIFEST *, UINT32 );
static UINT32 (WINAPI *p_appx_manifest_get_target_family_count)(
    const APPX_MANIFEST * );
static const struct appx_manifest_target_family *
    (WINAPI *p_appx_manifest_get_target_family)( const APPX_MANIFEST *, UINT32 );
static UINT32 (WINAPI *p_appx_manifest_get_inproc_class_count)(
    const APPX_MANIFEST * );
static const struct appx_manifest_inproc_class *
    (WINAPI *p_appx_manifest_get_inproc_class)( const APPX_MANIFEST *, UINT32 );
static UINT32 (WINAPI *p_appx_manifest_get_unsupported_reason_count)(
    const APPX_MANIFEST * );
static enum appx_manifest_unsupported_reason
    (WINAPI *p_appx_manifest_get_unsupported_reason)(
        const APPX_MANIFEST *, UINT32 );

#define REQUIRED_PROPERTY_CHILDREN \
    "<DisplayName>Wine</DisplayName>" \
    "<PublisherDisplayName>Wine</PublisherDisplayName>" \
    "<Logo>logo.png</Logo>"
#define REQUIRED_PROPERTIES \
    "<Properties>" REQUIRED_PROPERTY_CHILDREN "</Properties>"
#define FRAMEWORK_PROPERTIES \
    "<Properties><Framework>true</Framework>" \
    REQUIRED_PROPERTY_CHILDREN "</Properties>"
#define RESOURCE_PROPERTIES \
    "<Properties><ResourcePackage>true</ResourcePackage>" \
    REQUIRED_PROPERTY_CHILDREN "</Properties>"
#define TARGET_FAMILY \
    "<TargetDeviceFamily Name=\"Windows.Desktop\" MinVersion=\"10.0.0.0\"" \
    " MaxVersionTested=\"10.0.65535.0\"/>"
#define REQUIRED_DEPENDENCIES \
    "<Dependencies>" TARGET_FAMILY "</Dependencies>"
#define REQUIRED_RESOURCES "<Resources/>"
#define REQUIRED_STRUCTURE \
    REQUIRED_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
#define WINDOWS8_NAMESPACE \
    "http://schemas.microsoft.com/appx/2010/manifest"
#define WINDOWS8_IDENTITY \
    "<Identity Name=\"Wine.Legacy\" Publisher=\"CN=Wine\"" \
    " Version=\"1.2.3.4\"/>"
#define WINDOWS8_RESOURCES \
    "<Resources><Resource Language=\"en-us\"/></Resources>"
#define WINDOWS8_PREREQUISITES \
    "<Prerequisites><OSMinVersion>6.2</OSMinVersion>" \
    "<OSMaxVersionTested>6.3.1</OSMaxVersionTested></Prerequisites>"

static const char valid_manifest[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
    " xmlns:uap=\"http://schemas.microsoft.com/appx/manifest/uap/windows10\""
    " xmlns:rescap=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\">"
    "<Identity Name=\"Microsoft.WindowsStore\""
    " Publisher=\"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US\""
    " Version=\"1.2.3.4\" ProcessorArchitecture=\"x64\"/>"
    "<Properties><DisplayName>Store</DisplayName><PublisherDisplayName>Microsoft</PublisherDisplayName>"
    "<Logo>Assets\\Logo.png</Logo></Properties>"
    "<Resources/>"
    "<Dependencies><TargetDeviceFamily Name=\"Windows.Desktop\" MinVersion=\"10.0.0.0\""
    " MaxVersionTested=\"10.0.65535.0\"/></Dependencies>"
    "<Applications><Application Id=\"Store\" Executable=\"Store.exe\""
    " EntryPoint=\"Windows.FullTrustApplication\">"
    "<uap:VisualElements DisplayName=\"Store\" Square150x150Logo=\"Assets\\Logo.png\""
    " Square44x44Logo=\"Assets\\Small.png\" Description=\"Store\""
    " BackgroundColor=\"transparent\"/>"
    "</Application></Applications>"
    "<Capabilities><rescap:Capability Name=\"runFullTrust\"/></Capabilities>"
    "</Package>";

static HRESULT parse_text( const char *text, APPX_MANIFEST **manifest )
{
    return p_appx_manifest_parse( (const BYTE *)text, strlen( text ), manifest );
}

static BOOL has_reason( const APPX_MANIFEST *manifest,
                        enum appx_manifest_unsupported_reason expected )
{
    UINT32 count = p_appx_manifest_get_unsupported_reason_count( manifest ), i;

    for (i = 0; i < count; i++)
        if (p_appx_manifest_get_unsupported_reason( manifest, i ) == expected)
            return TRUE;
    return FALSE;
}

static void test_arguments(void)
{
    APPX_MANIFEST *manifest = (APPX_MANIFEST *)0xdeadbeef;
    BYTE byte = '<';
    HRESULT hr;

    hr = p_appx_manifest_parse( NULL, 1, &manifest );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    ok( !manifest, "got manifest %p.\n", manifest );
    hr = p_appx_manifest_parse( &byte, 0, &manifest );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = p_appx_manifest_parse( &byte, APPX_MANIFEST_MAX_SIZE + 1, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = p_appx_manifest_parse( &byte, 1, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );

    p_appx_manifest_free( NULL );
    ok( !p_appx_manifest_get_identity( NULL ), "got an identity.\n" );
    ok( !p_appx_manifest_is_supported( NULL ), "NULL manifest is supported.\n" );
    ok( !p_appx_manifest_get_application_count( NULL ), "got applications.\n" );
    ok( !p_appx_manifest_get_application( NULL, 0 ), "got an application.\n" );
    ok( !p_appx_manifest_get_dependency( NULL, 0 ), "got a dependency.\n" );
    ok( !p_appx_manifest_get_target_family( NULL, 0 ), "got a target family.\n" );
    ok( !p_appx_manifest_get_inproc_class( NULL, 0 ), "got a class.\n" );
}

static void test_identity(void)
{
    const struct appx_manifest_application *application;
    const struct appx_manifest_target_family *family;
    const struct appx_manifest_identity *identity;
    APPX_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( valid_manifest, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    ok( p_appx_manifest_is_supported( manifest ), "manifest is unsupported.\n" );
    ok( p_appx_manifest_has_run_full_trust( manifest ), "capability is missing.\n" );
    ok( !p_appx_manifest_is_framework( manifest ), "unexpected framework.\n" );
    ok( !p_appx_manifest_is_resource_package( manifest ),
        "unexpected resource package.\n" );

    identity = p_appx_manifest_get_identity( manifest );
    ok( !!identity, "identity is missing.\n" );
    ok( !lstrcmpW( identity->name, L"Microsoft.WindowsStore" ),
        "got name %s.\n", wine_dbgstr_w(identity->name) );
    ok( !lstrcmpW( identity->publisher_id, L"8wekyb3d8bbwe" ),
        "got publisher id %s.\n", wine_dbgstr_w(identity->publisher_id) );
    ok( !lstrcmpW( identity->family_name,
                    L"Microsoft.WindowsStore_8wekyb3d8bbwe" ),
        "got family name %s.\n", wine_dbgstr_w(identity->family_name) );
    ok( !lstrcmpW( identity->full_name,
                    L"Microsoft.WindowsStore_1.2.3.4_x64__8wekyb3d8bbwe" ),
        "got full name %s.\n", wine_dbgstr_w(identity->full_name) );
    ok( identity->version.major == 1 && identity->version.minor == 2 &&
        identity->version.build == 3 && identity->version.revision == 4,
        "got version %u.%u.%u.%u.\n", identity->version.major,
        identity->version.minor, identity->version.build,
        identity->version.revision );
    ok( identity->architecture == APPX_MANIFEST_ARCHITECTURE_X64,
        "got architecture %u.\n", identity->architecture );

    ok( p_appx_manifest_get_application_count( manifest ) == 1,
        "got %u applications.\n",
        p_appx_manifest_get_application_count( manifest ) );
    application = p_appx_manifest_get_application( manifest, 0 );
    ok( !!application, "application is missing.\n" );
    ok( !lstrcmpW( application->id, L"Store" ), "got id %s.\n",
        wine_dbgstr_w(application->id) );
    ok( !lstrcmpW( application->executable, L"Store.exe" ),
        "got executable %s.\n", wine_dbgstr_w(application->executable) );
    ok( application->activation_kind == APPX_MANIFEST_ACTIVATION_FULL_TRUST,
        "got activation kind %u.\n", application->activation_kind );
    ok( !p_appx_manifest_get_application( manifest, 1 ),
        "out-of-range application was returned.\n" );

    ok( p_appx_manifest_get_target_family_count( manifest ) == 1,
        "got %u target families.\n",
        p_appx_manifest_get_target_family_count( manifest ) );
    family = p_appx_manifest_get_target_family( manifest, 0 );
    ok( !lstrcmpW( family->name, L"Windows.Desktop" ), "got family %s.\n",
        wine_dbgstr_w(family->name) );
    ok( family->has_max_version_tested, "max version is missing.\n" );
    ok( family->max_version_tested.build == 65535, "got build %u.\n",
        family->max_version_tested.build );
    p_appx_manifest_free( manifest );
}

static void test_windows8_manifest(void)
{
    static const char valid[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        "<Prerequisites><OSMaxVersionTested>6.3.1</OSMaxVersionTested>"
        "<OSMinVersion>6.2</OSMinVersion></Prerequisites>"
        "<Dependencies><PackageDependency Name=\"Wine.Framework\""
        " Publisher=\"CN=Wine\"/></Dependencies></Package>";
    static const char start_page[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Applications><Application Id=\"App\" StartPage=\"default.html\">"
        "<VisualElements DisplayName=\"App\" Logo=\"logo.png\"/>"
        "</Application></Applications></Package>";
    static const char missing_resources[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_PREREQUISITES
        "</Package>";
    static const char empty_resources[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES "<Resources/>"
        WINDOWS8_PREREQUISITES "</Package>";
    static const char missing_maximum[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        "<Prerequisites><OSMinVersion>6.2</OSMinVersion></Prerequisites>"
        "</Package>";
    static const char leading_zero[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        "<Prerequisites><OSMinVersion>06.2</OSMinVersion>"
        "<OSMaxVersionTested>6.3</OSMaxVersionTested></Prerequisites>"
        "</Package>";
    static const char reversed_versions[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        "<Prerequisites><OSMinVersion>6.3.1</OSMinVersion>"
        "<OSMaxVersionTested>6.3</OSMaxVersionTested></Prerequisites>"
        "</Package>";
    static const char invalid_resource[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES
        "<Resources><Resource Language=\"en-us\" Scale=\"100\"/></Resources>"
        WINDOWS8_PREREQUISITES "</Package>";
    static const char invalid_then_valid_resource[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES
        "<Resources><Resource Scale=\"100\"/>"
        "<Resource Language=\"en-us\"/></Resources>"
        WINDOWS8_PREREQUISITES "</Package>";
    static const char future_restricted_capability[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\""
        " xmlns:rescap=\"http://schemas.microsoft.com/appx/manifest/"
        "foundation/windows10/restrictedcapabilities\""
        " IgnorableNamespaces=\"rescap\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Capabilities><rescap:Capability Name=\"runFullTrust\"/>"
        "</Capabilities></Package>";
    static const char executable_without_architecture[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"App.Main\"><VisualElements/>"
        "</Application></Applications></Package>";
    static const char unsupported_architecture[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        "<Identity Name=\"Wine.Legacy\" Publisher=\"CN=Wine\""
        " Version=\"1.2.3.4\" ProcessorArchitecture=\"arm64\"/>"
        REQUIRED_PROPERTIES WINDOWS8_RESOURCES WINDOWS8_PREREQUISITES
        "</Package>";
    static const char resource_package_property[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY
        "<Properties><ResourcePackage>true</ResourcePackage>"
        REQUIRED_PROPERTY_CHILDREN "</Properties>"
        WINDOWS8_RESOURCES WINDOWS8_PREREQUISITES "</Package>";
    static const char future_dependency_attribute[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Dependencies><PackageDependency Name=\"Wine.Framework\""
        " MaxMajorVersionTested=\"2\"/></Dependencies></Package>";
    static const char missing_visual_elements[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Applications><Application Id=\"App\" StartPage=\"default.html\"/>"
        "</Applications></Package>";
    static const char invalid_application_id[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Applications><Application Id=\"9App\" StartPage=\"default.html\">"
        "<VisualElements/></Application></Applications></Package>";
    static const char unsafe_start_page[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY REQUIRED_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Applications><Application Id=\"App\" StartPage=\"..\\default.html\">"
        "<VisualElements/></Application></Applications></Package>";
    static const char executable_uwp[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        "<Identity Name=\"Wine.Legacy\" Publisher=\"CN=Wine\""
        " Version=\"1.2.3.4\" ProcessorArchitecture=\"x86\"/>"
        REQUIRED_PROPERTIES WINDOWS8_RESOURCES WINDOWS8_PREREQUISITES
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\"><VisualElements/>"
        "</Application></Applications></Package>";
    static const char framework[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY FRAMEWORK_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES "</Package>";
    static const char framework_with_dependency[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY FRAMEWORK_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Dependencies><PackageDependency Name=\"Wine.Framework\"/>"
        "</Dependencies></Package>";
    static const char framework_with_capability[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY FRAMEWORK_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES
        "<Capabilities><Capability Name=\"internetClient\"/>"
        "</Capabilities></Package>";
    static const char framework_with_extensions[] =
        "<Package xmlns=\"" WINDOWS8_NAMESPACE "\">"
        WINDOWS8_IDENTITY FRAMEWORK_PROPERTIES WINDOWS8_RESOURCES
        WINDOWS8_PREREQUISITES "<Extensions/></Package>";
    const struct appx_manifest_application *application;
    const struct appx_manifest_dependency *dependency;
    const struct appx_manifest_identity *identity;
    APPX_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( valid, &manifest );
    ok( hr == S_OK, "Windows 8 manifest returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        identity = p_appx_manifest_get_identity( manifest );
        ok( identity->architecture == APPX_MANIFEST_ARCHITECTURE_NEUTRAL,
            "got architecture %u.\n", identity->architecture );
        ok( !lstrcmpW( identity->full_name,
                        L"Wine.Legacy_1.2.3.4_neutral__cb087t8t9s0q4" ),
            "got full name %s.\n", wine_dbgstr_w(identity->full_name) );
        ok( p_appx_manifest_get_dependency_count( manifest ) == 1,
            "got %u dependencies.\n",
            p_appx_manifest_get_dependency_count( manifest ) );
        dependency = p_appx_manifest_get_dependency( manifest, 0 );
        ok( !dependency->min_version.major &&
            !dependency->min_version.minor &&
            !dependency->min_version.build &&
            !dependency->min_version.revision,
            "got default minimum version %u.%u.%u.%u.\n",
            dependency->min_version.major, dependency->min_version.minor,
            dependency->min_version.build, dependency->min_version.revision );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( start_page, &manifest );
    ok( hr == S_OK, "Windows 8 StartPage manifest returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        application = p_appx_manifest_get_application( manifest, 0 );
        ok( !!application, "StartPage application is missing.\n" );
        ok( application && !application->executable,
            "StartPage application unexpectedly has an executable.\n" );
        ok( has_reason( manifest,
                        APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION ),
            "StartPage unsupported reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( missing_resources, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "missing resources returned %#lx.\n", hr );
    hr = parse_text( empty_resources, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "empty resources returned %#lx.\n", hr );
    hr = parse_text( missing_maximum, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "missing maximum returned %#lx.\n", hr );
    hr = parse_text( leading_zero, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "leading-zero version returned %#lx.\n", hr );
    hr = parse_text( reversed_versions, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "reversed versions returned %#lx.\n", hr );
    hr = parse_text( invalid_resource, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "invalid resource returned %#lx.\n", hr );
    hr = parse_text( invalid_then_valid_resource, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "invalid then valid resource returned %#lx.\n", hr );

    hr = parse_text( future_restricted_capability, &manifest );
    ok( hr == S_OK, "future restricted capability returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( !p_appx_manifest_has_run_full_trust( manifest ),
            "Windows 10 capability changed Windows 8 trust semantics.\n" );
        ok( has_reason( manifest,
                        APPX_MANIFEST_UNSUPPORTED_IGNORABLE_CONTENT ),
            "ignored-content reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( executable_without_architecture, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "executable without architecture returned %#lx.\n", hr );
    hr = parse_text( unsupported_architecture, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "unsupported architecture returned %#lx.\n", hr );
    hr = parse_text( resource_package_property, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "legacy ResourcePackage returned %#lx.\n", hr );
    hr = parse_text( future_dependency_attribute, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "future dependency attribute returned %#lx.\n", hr );
    hr = parse_text( missing_visual_elements, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "missing VisualElements returned %#lx.\n", hr );
    hr = parse_text( invalid_application_id, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "invalid application id returned %#lx.\n", hr );
    hr = parse_text( unsafe_start_page, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "unsafe StartPage returned %#lx.\n", hr );

    hr = parse_text( executable_uwp, &manifest );
    ok( hr == S_OK, "legacy executable UWP returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        application = p_appx_manifest_get_application( manifest, 0 );
        ok( application && application->activation_kind ==
                APPX_MANIFEST_ACTIVATION_UNSUPPORTED,
            "legacy executable has activation kind %u.\n",
            application ? application->activation_kind : ~0u );
        ok( !p_appx_manifest_has_run_full_trust( manifest ),
            "legacy executable gained runFullTrust.\n" );
        ok( has_reason( manifest,
                        APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION ),
            "legacy executable UWP reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( framework, &manifest );
    ok( hr == S_OK, "legacy framework returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( p_appx_manifest_is_framework( manifest ),
            "legacy framework flag is missing.\n" );
        p_appx_manifest_free( manifest );
    }
    hr = parse_text( framework_with_dependency, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "legacy framework with dependency returned %#lx.\n", hr );
    hr = parse_text( framework_with_capability, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "legacy framework with capability returned %#lx.\n", hr );
    hr = parse_text( framework_with_extensions, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "legacy framework with extensions returned %#lx.\n", hr );
}

static void test_namespaces(void)
{
    static const char prefixed_root[] =
        "<f:Package xmlns:f=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:r=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\">"
        "<f:Identity Name=\"Wine.Test\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<f:Properties><f:DisplayName>Wine</f:DisplayName>"
        "<f:PublisherDisplayName>Wine</f:PublisherDisplayName>"
        "<f:Logo>logo.png</f:Logo></f:Properties>"
        "<f:Resources/>"
        "<f:Dependencies><f:TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.65535.0\"/>"
        "</f:Dependencies>"
        "<f:Applications><f:Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\"/></f:Applications>"
        "<f:Capabilities><r:Capability Name=\"runFullTrust\"/></f:Capabilities>"
        "</f:Package>";
    static const char duplicate_ignorable[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:u=\"urn:test\" IgnorableNamespaces=\"u u\">"
        "<Identity Name=\"Wine.Test\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    static const char undeclared_ignorable[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " IgnorableNamespaces=\"missing\">"
        "<Identity Name=\"Wine.Test\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    static const char ignored_semantic[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:x=\"urn:future\" IgnorableNamespaces=\"x\">"
        "<Identity Name=\"Wine.Test\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "<x:ActivationOverride Mode=\"hosted\"/></Package>";
    static const char unknown_required[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:x=\"urn:future\">"
        "<Identity Name=\"Wine.Test\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "<x:FuturePackageKind/></Package>";
    static const char rebound_ignorable[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:x=\"urn:declared\" IgnorableNamespaces=\"x\">"
        "<Identity Name=\"Wine.Test\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "<x:FuturePackageKind xmlns:x=\"urn:rebound\"/></Package>";
    static const char wrong_root[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows8\"/>";
    static const char interleaved_root[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        REQUIRED_DEPENDENCIES
        "<Identity Name=\"Wine.Interleaved\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES
        "</Package>";
    APPX_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( prefixed_root, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( p_appx_manifest_is_supported( manifest ), "manifest is unsupported.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( duplicate_ignorable, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( undeclared_ignorable, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );

    hr = parse_text( ignored_semantic, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( !p_appx_manifest_is_supported( manifest ), "manifest is supported.\n" );
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_IGNORABLE_CONTENT ),
            "ignored-content reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( unknown_required, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_UNKNOWN_NAMESPACE ),
            "unknown-namespace reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }
    hr = parse_text( rebound_ignorable, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_UNKNOWN_NAMESPACE ),
            "rebound namespace was treated as ignorable.\n" );
        ok( !has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_IGNORABLE_CONTENT ),
            "rebound namespace retained ignorable status.\n" );
        p_appx_manifest_free( manifest );
    }
    hr = parse_text( wrong_root, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( interleaved_root, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_appx_manifest_free( manifest );
}

static void test_required_structure(void)
{
    static const char missing_properties[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.MissingProperties\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    static const char missing_dependencies[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.MissingDependencies\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES
        "</Package>";
    static const char missing_resources[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.MissingResources\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_DEPENDENCIES
        "</Package>";
    static const char empty_dependencies[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.EmptyDependencies\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES
        "<Dependencies/>"
        "</Package>";
    static const char missing_display_name[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.MissingDisplayName\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework>"
        "<PublisherDisplayName>Wine</PublisherDisplayName><Logo>logo.png</Logo>"
        "</Properties>"
        REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    static const char missing_publisher_display_name[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.MissingPublisherDisplayName\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework>"
        "<DisplayName>Wine</DisplayName><Logo>logo.png</Logo></Properties>"
        REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    static const char missing_logo[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.MissingLogo\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework>"
        "<DisplayName>Wine</DisplayName>"
        "<PublisherDisplayName>Wine</PublisherDisplayName></Properties>"
        REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    static const char missing_max_version[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.MissingMaxVersion\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES
        "<Dependencies><TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\"/></Dependencies>"
        "</Package>";
    const char *invalid[] =
    {
        missing_properties,
        missing_dependencies,
        missing_resources,
        empty_dependencies,
        missing_display_name,
        missing_publisher_display_name,
        missing_logo,
        missing_max_version,
    };
    APPX_MANIFEST *manifest = NULL;
    UINT32 i;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(invalid); i++)
    {
        hr = parse_text( invalid[i], &manifest );
        ok( hr == APPX_E_INVALID_MANIFEST,
            "case %u got hr %#lx.\n", i, hr );
    }
}

static void test_activation(void)
{
    static const char modern[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:uap10=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/10\""
        " xmlns:r=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\""
        " IgnorableNamespaces=\"uap10\">"
        "<Identity Name=\"Wine.Modern\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x86a64\" ResourceId=\"en-US\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"bin\\app.exe\""
        " uap10:RuntimeBehavior=\"packagedClassicApp\" uap10:TrustLevel=\"mediumIL\"/>"
        "</Applications><Capabilities><r:Capability Name=\"runFullTrust\"/></Capabilities>"
        "</Package>";
    static const char contradiction[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:uap10=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/10\">"
        "<Identity Name=\"Wine.Bad\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\""
        " uap10:RuntimeBehavior=\"win32App\" uap10:TrustLevel=\"mediumIL\"/>"
        "</Applications></Package>";
    static const char uwp[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:r=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\">"
        "<Identity Name=\"Wine.Uwp\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"Wine.App\"/></Applications>"
        "<Capabilities><r:Capability Name=\"runFullTrust\"/></Capabilities></Package>";
    static const char permission_only[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:r=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\">"
        "<Identity Name=\"Wine.Permission\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\"/></Applications>"
        "<Capabilities><r:Capability Name=\"runFullTrust\"/></Capabilities></Package>";
    static const char windows_app[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:uap10=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/10\">"
        "<Identity Name=\"Wine.WindowsApp\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"Wine.App\" uap10:RuntimeBehavior=\"windowsApp\"/>"
        "</Applications></Package>";
    static const char windows_app_container[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:uap10=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/10\">"
        "<Identity Name=\"Wine.WindowsAppContainer\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"Wine.App\" uap10:RuntimeBehavior=\"windowsApp\""
        " uap10:TrustLevel=\"appContainer\"/></Applications></Package>";
    static const char windows_app_medium[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:uap10=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/10\">"
        "<Identity Name=\"Wine.WindowsAppMedium\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"Wine.App\" uap10:RuntimeBehavior=\"windowsApp\""
        " uap10:TrustLevel=\"mediumIL\"/></Applications></Package>";
    static const char invalid_trust[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:uap10=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/10\">"
        "<Identity Name=\"Wine.InvalidTrust\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " uap10:RuntimeBehavior=\"win32App\" uap10:TrustLevel=\"arbitrary\"/>"
        "</Applications></Package>";
    static const char unqualified_runtime[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.BadRuntime\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " RuntimeBehavior=\"win32App\" TrustLevel=\"mediumIL\"/>"
        "</Applications></Package>";
    static const char missing_permission[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.NoPermission\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\"/></Applications></Package>";
    static const char parameters[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:r=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities\""
        " xmlns:uap11=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/11\""
        " IgnorableNamespaces=\"uap11\">"
        "<Identity Name=\"Wine.Parameters\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\" uap11:Parameters=\"--safe\""
        " uap11:CurrentDirectoryPath=\"data\"/></Applications>"
        "<Capabilities><r:Capability Name=\"runFullTrust\"/></Capabilities></Package>";
    static const char traversal[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Path\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>"
        REQUIRED_STRUCTURE
        "<Applications><Application Id=\"App\" Executable=\"..\\app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\"/></Applications></Package>";
    const struct appx_manifest_application *application;
    const struct appx_manifest_identity *identity;
    APPX_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( modern, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( p_appx_manifest_is_supported( manifest ), "manifest is unsupported.\n" );
        identity = p_appx_manifest_get_identity( manifest );
        ok( identity->architecture == APPX_MANIFEST_ARCHITECTURE_X86A64,
            "got architecture %u.\n", identity->architecture );
        ok( !lstrcmpW( identity->resource_id, L"en-US" ), "got resource id %s.\n",
            wine_dbgstr_w(identity->resource_id) );
        application = p_appx_manifest_get_application( manifest, 0 );
        ok( application->activation_kind ==
                APPX_MANIFEST_ACTIVATION_PACKAGED_CLASSIC,
            "got activation kind %u.\n", application->activation_kind );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( contradiction, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );

    hr = parse_text( uwp, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION ),
            "UWP reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( permission_only, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION ),
            "UWP reason is missing.\n" );
        ok( p_appx_manifest_has_run_full_trust( manifest ),
            "runFullTrust was not recorded.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( windows_app, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION ),
            "UWP reason is missing.\n" );
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_APPCONTAINER ),
            "AppContainer reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( windows_app_container, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION ),
            "UWP reason is missing.\n" );
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_APPCONTAINER ),
            "AppContainer reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }
    hr = parse_text( windows_app_medium, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( invalid_trust, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );

    hr = parse_text( unqualified_runtime, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );

    hr = parse_text( missing_permission, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest,
                        APPX_MANIFEST_UNSUPPORTED_MISSING_RUN_FULL_TRUST ),
            "missing-permission reason is absent.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( parameters, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        application = p_appx_manifest_get_application( manifest, 0 );
        ok( !lstrcmpW( application->parameters, L"--safe" ), "got parameters %s.\n",
            wine_dbgstr_w(application->parameters) );
        ok( !lstrcmpW( application->current_directory_path, L"data" ),
            "got current directory %s.\n",
            wine_dbgstr_w(application->current_directory_path) );
        ok( has_reason( manifest,
                        APPX_MANIFEST_UNSUPPORTED_APPLICATION_PARAMETERS ),
            "parameters reason is missing.\n" );
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_CURRENT_DIRECTORY ),
            "current-directory reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( traversal, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
}

static void test_dependencies_and_classes(void)
{
    static const char manifest_text[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:uap6=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/6\""
        " xmlns:rescap=\"http://schemas.microsoft.com/appx/manifest/"
        "foundation/windows10/restrictedcapabilities\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"2.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        REQUIRED_PROPERTIES REQUIRED_RESOURCES
        "<Dependencies>"
        "<TargetDeviceFamily Name=\"Windows.Universal\" MinVersion=\"10.0.17763.0\""
        " MaxVersionTested=\"10.0.22621.0\"/>"
        "<PackageDependency Name=\"Microsoft.WindowsAppRuntime.1.8\""
        " Publisher=\"CN=Microsoft Corporation\" MinVersion=\"8000.0.0.0\""
        " MaxMajorVersionTested=\"8000\" uap6:Optional=\"false\"/>"
        "</Dependencies>"
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\"/></Applications>"
        "<Capabilities><rescap:Capability Name=\"runFullTrust\"/>"
        "</Capabilities>"
        "<Extensions><Extension Category=\"windows.activatableClass.inProcessServer\">"
        "<InProcessServer><Path>runtime\\server.dll</Path>"
        "<ActivatableClass ActivatableClassId=\"Wine.Runtime.Class\" ThreadingModel=\"both\"/>"
        "<ActivatableClass ActivatableClassId=\"Wine.Runtime.Sta\" ThreadingModel=\"STA\"/>"
        "</InProcessServer></Extension></Extensions>"
        "</Package>";
    static const char optional_dependency[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:uap6=\"http://schemas.microsoft.com/appx/manifest/uap/windows10/6\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        REQUIRED_PROPERTIES REQUIRED_RESOURCES
        "<Dependencies>" TARGET_FAMILY
        "<PackageDependency Name=\"Wine.Dependency\" Publisher=\"CN=Wine\""
        " MinVersion=\"1.0.0.0\" uap6:Optional=\"true\"/></Dependencies>"
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\"/></Applications></Package>";
    static const char duplicate_dependency[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES "<Dependencies>" TARGET_FAMILY
        "<PackageDependency Name=\"Wine.Dependency\" Publisher=\"CN=Wine\" MinVersion=\"1.0.0.0\"/>"
        "<PackageDependency Name=\"wine.dependency\" Publisher=\"cn=wine\" MinVersion=\"2.0.0.0\"/>"
        "</Dependencies></Package>";
    static const char duplicate_name_different_publisher[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES "<Dependencies>" TARGET_FAMILY
        "<PackageDependency Name=\"Wine.Dependency\" Publisher=\"CN=First\""
        " MinVersion=\"1.0.0.0\"/>"
        "<PackageDependency Name=\"wine.dependency\" Publisher=\"CN=Second\""
        " MinVersion=\"2.0.0.0\"/>"
        "</Dependencies></Package>";
    static const char unsigned_dependency[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        REQUIRED_PROPERTIES REQUIRED_RESOURCES "<Dependencies>" TARGET_FAMILY
        "<PackageDependency Name=\"Wine.Unsigned\" MinVersion=\"1.0.0.0\"/>"
        "</Dependencies><Applications><Application Id=\"App\""
        " Executable=\"app.exe\" EntryPoint=\"windows.fullTrustApplication\"/>"
        "</Applications></Package>";
    static const char outproc[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "<Extensions>"
        "<Extension Category=\"windows.activatableClass.outOfProcessServer\">"
        "<OutOfProcessServer/></Extension></Extensions></Package>";
    static const char framework_dependency[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES "<Dependencies>" TARGET_FAMILY
        "<PackageDependency Name=\"Wine.Dependency\" Publisher=\"CN=Wine\""
        " MinVersion=\"1.0.0.0\"/></Dependencies></Package>";
    static const char framework_capability[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "<Capabilities><Capability Name=\"internetClient\"/>"
        "</Capabilities></Package>";
    static const char framework_application[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "<Applications><Application Id=\"App\" Executable=\"app.exe\""
        " EntryPoint=\"windows.fullTrustApplication\"/></Applications></Package>";
    static const char inproc_without_architecture[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "<Extensions><Extension Category=\"windows.activatableClass.inProcessServer\">"
        "<InProcessServer><Path>runtime\\server.dll</Path>"
        "<ActivatableClass ActivatableClassId=\"Wine.Runtime.Class\""
        " ThreadingModel=\"both\"/></InProcessServer></Extension>"
        "</Extensions></Package>";
    const struct appx_manifest_inproc_class *class;
    const struct appx_manifest_dependency *dependency;
    APPX_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( manifest_text, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( p_appx_manifest_is_supported( manifest ), "manifest is unsupported.\n" );
        ok( !p_appx_manifest_is_framework( manifest ),
            "unexpected framework flag.\n" );
        ok( p_appx_manifest_get_dependency_count( manifest ) == 1,
            "got %u dependencies.\n",
            p_appx_manifest_get_dependency_count( manifest ) );
        dependency = p_appx_manifest_get_dependency( manifest, 0 );
        ok( !lstrcmpW( dependency->name, L"Microsoft.WindowsAppRuntime.1.8" ),
            "got dependency %s.\n", wine_dbgstr_w(dependency->name) );
        ok( dependency->has_max_major_version_tested &&
            dependency->max_major_version_tested == 8000,
            "got max major %u, present %u.\n",
            dependency->max_major_version_tested,
            dependency->has_max_major_version_tested );
        ok( !dependency->optional, "dependency is optional.\n" );
        ok( p_appx_manifest_get_inproc_class_count( manifest ) == 2,
            "got %u classes.\n",
            p_appx_manifest_get_inproc_class_count( manifest ) );
        class = p_appx_manifest_get_inproc_class( manifest, 0 );
        ok( !lstrcmpW( class->path, L"runtime\\server.dll" ), "got path %s.\n",
            wine_dbgstr_w(class->path) );
        ok( !lstrcmpW( class->activatable_class_id, L"Wine.Runtime.Class" ),
            "got class %s.\n", wine_dbgstr_w(class->activatable_class_id) );
        ok( class->threading_model == APPX_MANIFEST_THREADING_BOTH,
            "got threading model %u.\n", class->threading_model );
        class = p_appx_manifest_get_inproc_class( manifest, 1 );
        ok( class->threading_model == APPX_MANIFEST_THREADING_STA,
            "got threading model %u.\n", class->threading_model );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( optional_dependency, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest,
                        APPX_MANIFEST_UNSUPPORTED_OPTIONAL_DEPENDENCY ),
            "optional-dependency reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( duplicate_dependency, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( duplicate_name_different_publisher, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( framework_dependency, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "framework dependency returned %#lx.\n", hr );
    hr = parse_text( framework_capability, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "framework capability returned %#lx.\n", hr );
    hr = parse_text( framework_application, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "framework application returned %#lx.\n", hr );
    hr = parse_text( inproc_without_architecture, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "in-process server without architecture returned %#lx.\n", hr );

    hr = parse_text( unsigned_dependency, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        dependency = p_appx_manifest_get_dependency( manifest, 0 );
        ok( !dependency->publisher[0], "got publisher %s.\n",
            wine_dbgstr_w(dependency->publisher) );
        ok( has_reason( manifest,
                        APPX_MANIFEST_UNSUPPORTED_UNSIGNED_DEPENDENCY ),
            "unsigned-dependency reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }

    hr = parse_text( outproc, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( has_reason( manifest,
                        APPX_MANIFEST_UNSUPPORTED_OUT_OF_PROCESS_SERVER ),
            "out-of-process reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }
}

static void test_security_and_limits(void)
{
    static const char doctype[] =
        "<!DOCTYPE Package [<!ENTITY publisher \"CN=Wine\">]>"
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"&publisher;\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    static const char processing_instruction[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<?install href=\"file:///tmp/override\"?>"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    static const char xinclude[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\""
        " xmlns:xi=\"http://www.w3.org/2001/XInclude\" IgnorableNamespaces=\"xi\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties>"
        "<xi:include href=\"file:///etc/passwd\" parse=\"text\"/></Package>";
    static const char non_utf8_encoding[] =
        "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>"
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    static const char duplicate_identity[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Identity Name=\"Wine.Framework2\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    static const char duplicate_attribute[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Name=\"Wine.Other\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    static const char overflow_version[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine\" Version=\"65536.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    static const char reserved_identity[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"CON.package\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    static const char trailing_dot_identity[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    static const char idn_identity[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.xn--bad\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    static const char resource_package[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Resources\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ResourceId=\"en-US\"/>"
        RESOURCE_PROPERTIES
        "<Resources><Resource Language=\"en-US\"/></Resources></Package>";
    static const char resource_package_with_architecture[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Resources\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\" ResourceId=\"en-US\"/>"
        RESOURCE_PROPERTIES
        "<Resources><Resource Language=\"en-US\"/></Resources></Package>";
    static const char resource_package_with_dependencies[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Resources\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ResourceId=\"en-US\"/>"
        RESOURCE_PROPERTIES
        "<Resources><Resource Language=\"en-US\"/></Resources>"
        REQUIRED_DEPENDENCIES "</Package>";
    static const char resource_package_mixed_element[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Resources\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ResourceId=\"en-US\"/>"
        RESOURCE_PROPERTIES
        "<Resources><Resource Language=\"en-US\" Scale=\"100\"/>"
        "</Resources></Package>";
    static const char resource_package_mixed_types[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Resources\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ResourceId=\"en-US\"/>"
        RESOURCE_PROPERTIES
        "<Resources><Resource Language=\"en-US\"/><Resource Scale=\"100\"/>"
        "</Resources></Package>";
    static const BYTE invalid_utf8[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=\xc0\xaf\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"neutral\"/>"
        "<Properties><Framework>true</Framework></Properties></Package>";
    APPX_MANIFEST *manifest = NULL;
    char *many_applications, *cursor;
    UINT32 i;
    HRESULT hr;

    hr = parse_text( doctype, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( processing_instruction, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( xinclude, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( non_utf8_encoding, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = p_appx_manifest_parse( invalid_utf8, sizeof(invalid_utf8) - 1, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( duplicate_identity, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( duplicate_attribute, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( overflow_version, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( reserved_identity, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "reserved identity returned %#lx.\n", hr );
    hr = parse_text( trailing_dot_identity, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "trailing-dot identity returned %#lx.\n", hr );
    hr = parse_text( idn_identity, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "IDN identity returned %#lx.\n", hr );

    hr = parse_text( resource_package, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        ok( p_appx_manifest_is_resource_package( manifest ),
            "resource-package flag is missing.\n" );
        ok( has_reason( manifest, APPX_MANIFEST_UNSUPPORTED_RESOURCE_PACKAGE ),
            "resource-package reason is missing.\n" );
        p_appx_manifest_free( manifest );
    }
    hr = parse_text( resource_package_with_architecture, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "resource package with architecture returned %#lx.\n", hr );
    hr = parse_text( resource_package_with_dependencies, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "resource package with dependencies returned %#lx.\n", hr );
    hr = parse_text( resource_package_mixed_element, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "resource package with mixed element returned %#lx.\n", hr );
    hr = parse_text( resource_package_mixed_types, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "resource package with mixed qualifier types returned %#lx.\n", hr );

    many_applications = HeapAlloc( GetProcessHeap(), 0, 32768 );
    ok( !!many_applications, "allocation failed.\n" );
    if (!many_applications) return;
    cursor = many_applications;
    cursor += sprintf( cursor,
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.ManyApps\" Publisher=\"CN=Wine\" Version=\"1.0.0.0\""
        " ProcessorArchitecture=\"x64\"/>" REQUIRED_STRUCTURE "<Applications>" );
    for (i = 0; i < APPX_MANIFEST_MAX_APPLICATIONS + 1; i++)
        cursor += sprintf( cursor, "<Application Id=\"App%u\" Executable=\"app%u.exe\""
                           " EntryPoint=\"windows.fullTrustApplication\"/>", i, i );
    cursor += sprintf( cursor, "</Applications></Package>" );
    hr = p_appx_manifest_parse( (BYTE *)many_applications,
                                cursor - many_applications, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    HeapFree( GetProcessHeap(), 0, many_applications );
}

static void test_embedded_nul_and_predefined_entity(void)
{
    static const char entity[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Framework\" Publisher=\"CN=Wine &amp; Project\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    static const BYTE embedded_nul[] =
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "\0</Package>";
    static const char doctype_in_comment[] =
        "<!-- <!DOCTYPE Package SYSTEM=\"file:///etc/passwd\"> -->"
        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">"
        "<Identity Name=\"Wine.Comment\" Publisher=\"CN=Wine\""
        " Version=\"1.0.0.0\" ProcessorArchitecture=\"neutral\"/>"
        FRAMEWORK_PROPERTIES REQUIRED_RESOURCES REQUIRED_DEPENDENCIES
        "</Package>";
    const struct appx_manifest_identity *identity;
    APPX_MANIFEST *manifest = NULL;
    HRESULT hr;

    hr = parse_text( entity, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        identity = p_appx_manifest_get_identity( manifest );
        ok( !lstrcmpW( identity->publisher, L"CN=Wine & Project" ),
            "got publisher %s.\n", wine_dbgstr_w(identity->publisher) );
        p_appx_manifest_free( manifest );
    }
    hr = p_appx_manifest_parse( embedded_nul, sizeof(embedded_nul) - 1, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST, "got hr %#lx.\n", hr );
    hr = parse_text( doctype_in_comment, &manifest );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    if (SUCCEEDED(hr)) p_appx_manifest_free( manifest );
}

static BOOL load_functions(void)
{
    HMODULE module = LoadLibraryA( "appxsvc.dll" );

    ok( !!module, "failed to load appxsvc.dll, error %lu.\n", GetLastError() );
    if (!module) return FALSE;
#define LOAD(name) \
    do { \
        p_##name = (void *)GetProcAddress( module, #name ); \
        ok( !!p_##name, "failed to load %s, error %lu.\n", #name, GetLastError() ); \
        if (!p_##name) return FALSE; \
    } while (0)
    LOAD(appx_manifest_parse);
    LOAD(appx_manifest_free);
    LOAD(appx_manifest_get_identity);
    LOAD(appx_manifest_is_supported);
    LOAD(appx_manifest_is_framework);
    LOAD(appx_manifest_is_resource_package);
    LOAD(appx_manifest_has_run_full_trust);
    LOAD(appx_manifest_get_application_count);
    LOAD(appx_manifest_get_application);
    LOAD(appx_manifest_get_dependency_count);
    LOAD(appx_manifest_get_dependency);
    LOAD(appx_manifest_get_target_family_count);
    LOAD(appx_manifest_get_target_family);
    LOAD(appx_manifest_get_inproc_class_count);
    LOAD(appx_manifest_get_inproc_class);
    LOAD(appx_manifest_get_unsupported_reason_count);
    LOAD(appx_manifest_get_unsupported_reason);
#undef LOAD
    return TRUE;
}

START_TEST(manifest)
{
    if (!load_functions())
    {
        win_skip( "AppX manifest parser exports are unavailable.\n" );
        return;
    }
    test_arguments();
    test_identity();
    test_windows8_manifest();
    test_namespaces();
    test_required_structure();
    test_activation();
    test_dependencies_and_classes();
    test_security_and_limits();
    test_embedded_nul_and_predefined_entity();
}
