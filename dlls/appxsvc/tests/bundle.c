/*
 * AppX bundle inspection tests
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
#include "aclapi.h"

#include "wine/test.h"

#include "../bundle.h"
#include "../content_types.h"

#define BUNDLE_OPEN \
    "<Bundle xmlns=\"http://schemas.microsoft.com/appx/2013/bundle\"" \
    " SchemaVersion=\"1.0\">" \
    "<Identity Name=\"Wine.Bundle\" Publisher=\"CN=Wine\"" \
    " Version=\"2.0.0.0\"/><Packages>"
#define BUNDLE_CLOSE "</Packages></Bundle>"
#define PACKAGE_RESOURCES "<Resources><Resource/></Resources>"

static HRESULT (WINAPI *p_appx_bundle_manifest_parse)(
    const BYTE *, SIZE_T, APPX_BUNDLE_MANIFEST ** );
static void (WINAPI *p_appx_bundle_manifest_free)(
    APPX_BUNDLE_MANIFEST * );
static const struct appx_bundle_package *(WINAPI
    *p_appx_bundle_manifest_get_package)(
    const APPX_BUNDLE_MANIFEST *, UINT32 );
static HRESULT (WINAPI *p_appx_bundle_validate_with_test_source)(
    const APPX_BUNDLE_MANIFEST *, const BYTE *,
    const struct appx_bundle_selection_policy *,
    const struct appx_bundle_test_source *,
    struct appx_bundle_selection * );
static HRESULT (WINAPI *p_appx_bundle_materialize_with_test_source)(
    UINT64, const struct appx_bundle_materialize_test_source * );

static const WCHAR bundle_manifest_path[] =
    L"AppxMetadata\\AppxBundleManifest.xml";
static const WCHAR block_map_path[] = L"AppxBlockMap.xml";
static const WCHAR content_types_path[] = L"[Content_Types].xml";
static const WCHAR signature_path[] = L"AppxSignature.p7x";
static const WCHAR code_integrity_path[] =
    L"AppxMetadata\\CodeIntegrity.cat";
static const WCHAR x86_path[] = L"app-x86.msix";
static const WCHAR x64_path[] = L"app-x64.msix";
static const WCHAR extra_path[] = L"unexpected.bin";

struct source_context
{
    struct appx_bundle_source_entry entries[12];
    struct appx_bundle_source_package packages[8];
    HRESULT entry_hr;
    HRESULT package_hr;
    UINT32 package_fail_index;
    UINT32 entry_count;
    UINT32 inspect_count;
    UINT64 max_total_expanded_size;
};

struct materialize_context
{
    BYTE data[64];
    UINT32 data_size;
    UINT32 offset;
    UINT32 read_limit;
    UINT32 fail_after;
    LONG read_count;
    HRESULT read_failure;
    HRESULT inspect_result;
    UINT32 inspect_count;
    BOOL retain_file;
    HANDLE retained_file;
    HANDLE inspect_enter_event;
    HANDLE inspect_release_event;
    WCHAR file_path[MAX_PATH];
};

static HRESULT parse_manifest( const char *text, APPX_BUNDLE_MANIFEST **manifest )
{
    *manifest = NULL;
    return p_appx_bundle_manifest_parse( (const BYTE *)text, strlen(text),
                                         manifest );
}

static HRESULT WINAPI get_entry_callback(
    void *opaque, UINT32 index, struct appx_bundle_source_entry *entry )
{
    struct source_context *context = opaque;

    if (FAILED(context->entry_hr)) return context->entry_hr;
    if (index >= context->entry_count || !entry ||
        entry->size != sizeof(*entry))
        return E_INVALIDARG;
    *entry = context->entries[index];
    return S_OK;
}

static HRESULT WINAPI inspect_package_callback(
    void *opaque, UINT32 manifest_index, UINT32 archive_index,
    UINT64 remaining_expanded_size,
    struct appx_bundle_source_package *package )
{
    struct source_context *context = opaque;

    (void)archive_index;
    context->inspect_count++;
    if (manifest_index == context->package_fail_index &&
        FAILED(context->package_hr))
        return context->package_hr;
    if (manifest_index >= ARRAY_SIZE(context->packages) || !package ||
        package->size != sizeof(*package))
        return E_INVALIDARG;
    if (context->packages[manifest_index].expanded_size >
        remaining_expanded_size)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    *package = context->packages[manifest_index];
    return S_OK;
}

static void init_entry( struct appx_bundle_source_entry *entry,
                        const WCHAR *path, UINT64 offset, UINT64 size )
{
    memset( entry, 0, sizeof(*entry) );
    entry->size = sizeof(*entry);
    entry->path = path;
    if (!lstrcmpW( path, bundle_manifest_path ))
        entry->content_type = APPX_CONTENT_TYPE_BUNDLE_MANIFEST;
    else if (!lstrcmpW( path, block_map_path ))
        entry->content_type = APPX_CONTENT_TYPE_BLOCK_MAP;
    else if (!lstrcmpW( path, signature_path ))
        entry->content_type = APPX_CONTENT_TYPE_SIGNATURE;
    else if (lstrcmpW( path, content_types_path ))
        entry->content_type = L"application/vnd.ms-appx";
    entry->compression_method = 0;
    entry->compressed_size = size;
    entry->uncompressed_size = size;
    entry->data_offset = offset;
}

static void init_package( struct appx_bundle_source_package *package,
                          enum appx_manifest_architecture architecture,
                          UINT16 major, BOOL framework, BOOL resource )
{
    memset( package, 0, sizeof(*package) );
    package->size = sizeof(*package);
    package->name = L"Wine.Bundle";
    package->publisher = L"CN=Wine";
    package->resource_id = resource ? L"resources" : L"";
    package->version.major = major;
    package->architecture = architecture;
    package->framework = framework;
    package->resource = resource;
    package->expanded_size = 64;
    memset( package->signer_id, 0x5a, sizeof(package->signer_id) );
}

static void init_source( struct source_context *context )
{
    memset( context, 0, sizeof(*context) );
    context->package_fail_index = MAXDWORD;
    context->max_total_expanded_size = ~(UINT64)0;
    init_entry( context->entries + 0, bundle_manifest_path, 10, 5 );
    init_entry( context->entries + 1, block_map_path, 20, 5 );
    init_entry( context->entries + 2, content_types_path, 30, 5 );
    init_entry( context->entries + 3, signature_path, 40, 5 );
    init_entry( context->entries + 4, x86_path, 100, 10 );
    init_entry( context->entries + 5, x64_path, 200, 12 );
    context->entry_count = 6;
    init_package( context->packages + 0, APPX_MANIFEST_ARCHITECTURE_X86,
                  1, FALSE, FALSE );
    init_package( context->packages + 1, APPX_MANIFEST_ARCHITECTURE_X64,
                  2, FALSE, FALSE );
}

static void init_policy( struct appx_bundle_selection_policy *policy,
                         enum appx_bundle_architecture architecture )
{
    memset( policy, 0, sizeof(*policy) );
    policy->size = sizeof(*policy);
    policy->host_architecture = architecture;
    policy->language_neutral_only = TRUE;
    policy->scale_neutral_only = TRUE;
}

static HRESULT validate( APPX_BUNDLE_MANIFEST *manifest,
                         struct source_context *context,
                         struct appx_bundle_selection_policy *policy,
                         struct appx_bundle_selection *selection )
{
    struct appx_bundle_test_source source;
    BYTE signer[APPX_BUNDLE_SIGNER_ID_SIZE];

    memset( signer, 0x5a, sizeof(signer) );
    memset( &source, 0, sizeof(source) );
    source.size = sizeof(source);
    source.context = context;
    source.entry_count = context->entry_count;
    source.max_total_expanded_size = context->max_total_expanded_size;
    source.get_entry = get_entry_callback;
    source.inspect_package = inspect_package_callback;
    memset( selection, 0, sizeof(*selection) );
    selection->size = sizeof(*selection);
    return p_appx_bundle_validate_with_test_source(
        manifest, signer, policy, &source, selection );
}

static void test_layout_and_identity(void)
{
    static const char valid[] =
        BUNDLE_OPEN
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x86\" FileName=\"app-x86.msix\""
        " Offset=\"100\" Size=\"10\">" PACKAGE_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\""
        " Offset=\"200\" Size=\"12\">" PACKAGE_RESOURCES "</Package>"
        BUNDLE_CLOSE;
    struct appx_bundle_selection_policy policy;
    struct appx_bundle_selection selection;
    struct source_context context;
    APPX_BUNDLE_MANIFEST *manifest;
    HRESULT hr;

    hr = parse_manifest( valid, &manifest );
    ok( hr == S_OK, "valid manifest returned %#lx.\n", hr );
    if (FAILED(hr)) return;
    init_policy( &policy, APPX_BUNDLE_ARCHITECTURE_X64 );
    init_source( &context );
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == S_OK, "valid source returned %#lx.\n", hr );
    ok( selection.package_index == 1, "selected package %u.\n",
        selection.package_index );
    ok( context.inspect_count == 2, "inspected %u packages.\n",
        context.inspect_count );

    init_source( &context );
    context.packages[0].expanded_size = 10;
    context.packages[1].expanded_size = 12;
    context.max_total_expanded_size = 22;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == S_OK, "exact expanded budget returned %#lx.\n", hr );
    ok( context.inspect_count == 2, "inspected %u budgeted packages.\n",
        context.inspect_count );

    init_source( &context );
    context.packages[0].expanded_size = 10;
    context.packages[1].expanded_size = 12;
    context.max_total_expanded_size = 21;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "excess expanded budget returned %#lx.\n", hr );
    ok( context.inspect_count == 2, "inspected %u over-budget packages.\n",
        context.inspect_count );
    ok( !selection.payload, "selected payload before budget validation.\n" );

    init_source( &context );
    context.packages[0].expanded_size = ~(UINT64)0;
    context.packages[1].expanded_size = 1;
    context.max_total_expanded_size = ~(UINT64)0;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "overflowing expanded budget returned %#lx.\n", hr );
    ok( context.inspect_count == 1, "inspected %u overflowing packages.\n",
        context.inspect_count );
    ok( !selection.payload, "selected payload after budget overflow.\n" );

    init_source( &context );
    context.entries[5].path = L"APP-X64.MSIX";
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "case mismatch returned %#lx.\n", hr );
    ok( context.inspect_count == 0, "inspected %u malformed packages.\n",
        context.inspect_count );

    init_source( &context );
    context.entries[5].compression_method = 8;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        "compressed inner returned %#lx.\n", hr );

    init_source( &context );
    context.entries[5].content_type = L"application/octet-stream";
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "wrong inner content type returned %#lx.\n", hr );

    init_source( &context );
    context.entries[0].content_type = APPX_CONTENT_TYPE_MANIFEST;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "wrong bundle-manifest content type returned %#lx.\n", hr );

    init_source( &context );
    context.entries[5].data_offset++;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "wrong range returned %#lx.\n", hr );

    init_source( &context );
    init_entry( context.entries + context.entry_count++, extra_path, 300, 1 );
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "unexpected entry returned %#lx.\n", hr );

    init_source( &context );
    context.entries[3] = context.entries[2];
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "duplicate footprint returned %#lx.\n", hr );

    init_source( &context );
    context.entries[3].path = code_integrity_path;
    context.entries[3].content_type = L"application/vnd.ms-appx.cat";
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_MISSING_REQUIRED_FILE,
        "missing signature footprint returned %#lx.\n", hr );

    init_source( &context );
    context.entry_count--;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "missing inner returned %#lx.\n", hr );

    init_source( &context );
    context.entries[5].flags = WINE_APPX_ENTRY_DIRECTORY;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "directory inner returned %#lx.\n", hr );

    init_source( &context );
    context.entry_hr = HRESULT_FROM_WIN32( ERROR_READ_FAULT );
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == HRESULT_FROM_WIN32(ERROR_READ_FAULT),
        "entry failure returned %#lx.\n", hr );

#define CHECK_INNER_FAILURE(member, value, expected, label) do               \
    {                                                                         \
        init_source( &context );                                               \
        context.packages[1].member = value;                                    \
        hr = validate( manifest, &context, &policy, &selection );              \
        ok( hr == expected, label " returned %#lx.\\n", hr );                 \
        ok( context.inspect_count == 2, label " inspected %u packages.\\n",   \
            context.inspect_count );                                           \
    } while (0)

    CHECK_INNER_FAILURE( name, L"Wine.Other", APPX_E_INVALID_MANIFEST,
                         "name mismatch" );
    CHECK_INNER_FAILURE( publisher, L"CN=Other", APPX_E_INVALID_MANIFEST,
                         "publisher mismatch" );
    CHECK_INNER_FAILURE( resource_id, L"bad", APPX_E_INVALID_MANIFEST,
                         "resource mismatch" );
    CHECK_INNER_FAILURE( architecture, APPX_MANIFEST_ARCHITECTURE_ARM64,
                         APPX_E_INVALID_MANIFEST, "architecture mismatch" );
    CHECK_INNER_FAILURE( framework, TRUE, APPX_E_INVALID_MANIFEST,
                         "type mismatch" );
#undef CHECK_INNER_FAILURE

    init_source( &context );
    context.packages[1].version.major++;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "version mismatch returned %#lx.\n", hr );

    init_source( &context );
    context.packages[1].signer_id[7] ^= 1;
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == APPX_E_DIGEST_MISMATCH,
        "signer mismatch returned %#lx.\n", hr );

    init_source( &context );
    context.package_fail_index = 1;
    context.package_hr = HRESULT_FROM_WIN32( ERROR_CANCELLED );
    hr = validate( manifest, &context, &policy, &selection );
    ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "inner cancellation returned %#lx.\n", hr );
    ok( context.inspect_count == 2, "inspected %u packages before cancel.\n",
        context.inspect_count );
    ok( !selection.payload, "selected payload before validation completed.\n" );

    p_appx_bundle_manifest_free( manifest );
}

static void test_flat_and_unsupported(void)
{
    static const char flat[] =
        BUNDLE_OPEN
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\">"
        PACKAGE_RESOURCES "</Package>"
        BUNDLE_CLOSE;
    static const char resource[] =
        BUNDLE_OPEN
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\""
        " Offset=\"200\" Size=\"12\">" PACKAGE_RESOURCES "</Package>"
        "<Package Type=\"resource\" Version=\"2.0.0.0\""
        " Architecture=\"neutral\" ResourceId=\"resources\""
        " FileName=\"resources.msix\" Offset=\"300\" Size=\"8\">"
        "<Resources><Resource Language=\"en-US\"/></Resources></Package>"
        BUNDLE_CLOSE;
    static const char framework[] =
        BUNDLE_OPEN
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\""
        " Offset=\"200\" Size=\"12\">" PACKAGE_RESOURCES "</Package>"
        "<Package Type=\"framework\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"framework.msix\""
        " Offset=\"300\" Size=\"8\">" PACKAGE_RESOURCES "</Package>"
        BUNDLE_CLOSE;
    static const char encrypted[] =
        BUNDLE_OPEN
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\""
        " Offset=\"200\" Size=\"12\" Encrypted=\"true\">"
        PACKAGE_RESOURCES "</Package>"
        BUNDLE_CLOSE;
    static const char optional[] =
        BUNDLE_OPEN
        "<Package Type=\"optional\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\""
        " Offset=\"200\" Size=\"12\">"
        PACKAGE_RESOURCES "</Package>"
        BUNDLE_CLOSE;
    static const char stub[] =
        BUNDLE_OPEN
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\""
        " Offset=\"200\" Size=\"12\" IsStub=\"true\">"
        PACKAGE_RESOURCES "</Package>"
        BUNDLE_CLOSE;
    static const char dependencies[] =
        BUNDLE_OPEN
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\""
        " Offset=\"200\" Size=\"12\">" PACKAGE_RESOURCES
        "<Dependencies><TargetDeviceFamily Name=\"Windows.Desktop\""
        " MinVersion=\"10.0.0.0\" MaxVersionTested=\"10.0.26100.0\"/>"
        "</Dependencies></Package>"
        BUNDLE_CLOSE;
    static const char overlap[] =
        BUNDLE_OPEN
        "<Package Type=\"application\" Version=\"1.0.0.0\""
        " Architecture=\"x86\" FileName=\"app-x86.msix\""
        " Offset=\"100\" Size=\"10\">" PACKAGE_RESOURCES "</Package>"
        "<Package Type=\"application\" Version=\"2.0.0.0\""
        " Architecture=\"x64\" FileName=\"app-x64.msix\""
        " Offset=\"109\" Size=\"12\">" PACKAGE_RESOURCES "</Package>"
        BUNDLE_CLOSE;
    struct appx_bundle_selection_policy policy;
    struct appx_bundle_selection selection;
    struct source_context context;
    const struct appx_bundle_package *package;
    APPX_BUNDLE_MANIFEST *manifest;
    HRESULT hr;

    init_policy( &policy, APPX_BUNDLE_ARCHITECTURE_X64 );
    hr = parse_manifest( flat, &manifest );
    ok( hr == S_OK, "flat manifest returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        package = p_appx_bundle_manifest_get_package( manifest, 0 );
        ok( package && !(package->flags & APPX_BUNDLE_PACKAGE_HAS_RANGE),
            "flat package unexpectedly has a range.\n" );
        init_source( &context );
        context.entries[4] = context.entries[5];
        context.entry_count = 5;
        context.packages[0] = context.packages[1];
        hr = validate( manifest, &context, &policy, &selection );
        ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            "flat bundle returned %#lx.\n", hr );
        p_appx_bundle_manifest_free( manifest );
    }

    hr = parse_manifest( resource, &manifest );
    ok( hr == S_OK, "resource manifest returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        init_source( &context );
        context.entries[4] = context.entries[5];
        init_entry( context.entries + 5, L"resources.msix", 300, 8 );
        context.entry_count = 6;
        context.packages[0] = context.packages[1];
        init_package( context.packages + 1,
                      APPX_MANIFEST_ARCHITECTURE_NEUTRAL, 2,
                      FALSE, TRUE );
        hr = validate( manifest, &context, &policy, &selection );
        ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            "resource bundle returned %#lx.\n", hr );
        ok( context.inspect_count == 2,
            "resource bundle inspected %u packages.\n",
            context.inspect_count );
        p_appx_bundle_manifest_free( manifest );
    }

    hr = parse_manifest( framework, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "framework manifest returned %#lx and manifest %p.\n",
        hr, manifest );
    if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );

    hr = parse_manifest( encrypted, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "encrypted manifest returned %#lx and manifest %p.\n",
        hr, manifest );
    if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );

    hr = parse_manifest( optional, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "optional manifest returned %#lx and manifest %p.\n",
        hr, manifest );
    if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );

    hr = parse_manifest( stub, &manifest );
    ok( hr == S_OK, "stub manifest returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        init_source( &context );
        context.entries[4] = context.entries[5];
        context.entry_count = 5;
        context.packages[0] = context.packages[1];
        hr = validate( manifest, &context, &policy, &selection );
        ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            "stub bundle returned %#lx.\n", hr );
        ok( context.inspect_count == 1,
            "stub bundle inspected %u packages.\n",
            context.inspect_count );
        p_appx_bundle_manifest_free( manifest );
    }

    hr = parse_manifest( dependencies, &manifest );
    ok( hr == S_OK, "dependency manifest returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        init_source( &context );
        context.entries[4] = context.entries[5];
        context.entry_count = 5;
        context.packages[0] = context.packages[1];
        hr = validate( manifest, &context, &policy, &selection );
        ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            "dependency bundle returned %#lx.\n", hr );
        ok( context.inspect_count == 1,
            "dependency bundle inspected %u packages.\n",
            context.inspect_count );
        p_appx_bundle_manifest_free( manifest );
    }

    hr = parse_manifest( overlap, &manifest );
    ok( hr == APPX_E_INVALID_MANIFEST,
        "overlapping ranges returned %#lx and manifest %p.\n",
        hr, manifest );
    if (SUCCEEDED(hr)) p_appx_bundle_manifest_free( manifest );
}

static HRESULT WINAPI materialize_read_callback(
    void *opaque, void *buffer, UINT32 capacity, UINT32 *read )
{
    struct materialize_context *context = opaque;
    UINT32 remaining, amount;

    InterlockedIncrement( &context->read_count );
    *read = 0;
    if (context->fail_after != MAXDWORD &&
        context->offset >= context->fail_after)
        return context->read_failure;
    if (context->offset == context->data_size) return S_FALSE;
    remaining = context->data_size - context->offset;
    amount = remaining < capacity ? remaining : capacity;
    if (context->read_limit && amount > context->read_limit)
        amount = context->read_limit;
    memcpy( buffer, context->data + context->offset, amount );
    context->offset += amount;
    *read = amount;
    return S_OK;
}

static BOOL get_handle_path( HANDLE file, WCHAR path[MAX_PATH] )
{
    DWORD length = GetFinalPathNameByHandleW( file, path, MAX_PATH,
                                               FILE_NAME_NORMALIZED |
                                               VOLUME_NAME_DOS );
    WCHAR *source;

    if (!length || length >= MAX_PATH) return FALSE;
    source = path;
    if (!wcsncmp( source, L"\\\\?\\", 4 )) source += 4;
    if (source != path)
        memmove( path, source, (lstrlenW(source) + 1) * sizeof(*path) );
    return TRUE;
}

static void check_private_file_security( HANDLE file )
{
    static const SID local_system_sid = {
        SID_REVISION, 1, {SECURITY_NT_AUTHORITY},
        {SECURITY_LOCAL_SYSTEM_RID}
    };
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    ACL_SIZE_INFORMATION information = {0};
    ACCESS_ALLOWED_ACE *ace = NULL;
    TOKEN_USER *user = NULL;
    PSID owner = NULL, group = NULL;
    PACL dacl = NULL;
    HANDLE token = NULL;
    DWORD error, i, revision = 0, size = 0;
    UINT32 system_aces = 0, user_aces = 0;

    ok( OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &token ),
        "failed to open current token, error %lu.\n", GetLastError() );
    if (!token) return;
    GetTokenInformation( token, TokenUser, NULL, 0, &size );
    ok( GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
        size >= sizeof(TOKEN_USER),
        "unexpected token-user probe, error %lu, size %lu.\n",
        GetLastError(), size );
    if (size >= sizeof(TOKEN_USER))
        user = HeapAlloc( GetProcessHeap(), 0, size );
    ok( !!user && GetTokenInformation( token, TokenUser, user, size, &size ),
        "failed to read token user, error %lu.\n", GetLastError() );

    error = GetSecurityInfo( file, SE_FILE_OBJECT,
                             OWNER_SECURITY_INFORMATION |
                             GROUP_SECURITY_INFORMATION |
                             DACL_SECURITY_INFORMATION,
                             &owner, &group, &dacl, NULL, &descriptor );
    ok( error == ERROR_SUCCESS, "GetSecurityInfo returned %lu.\n", error );
    if (error == ERROR_SUCCESS && descriptor && user)
    {
        ok( owner && EqualSid( owner, user->User.Sid ),
            "file owner is not the current user.\n" );
        ok( group && IsValidSid( group ),
            "file group is absent or invalid.\n" );
        ok( dacl != NULL, "file has a NULL DACL.\n" );
        ok( GetSecurityDescriptorControl( descriptor, &control, &revision ) &&
            (control & SE_DACL_PRESENT) &&
            !(control & (SE_DACL_DEFAULTED | SE_DACL_AUTO_INHERIT_REQ |
                         SE_DACL_AUTO_INHERITED)),
            "DACL control is unsafe, control %#x.\n", control );
        ok( dacl && GetAclInformation( dacl, &information,
                                       sizeof(information),
                                       AclSizeInformation ) &&
            information.AceCount >= 1 &&
            information.AceCount <= 2,
            "DACL contains %lu ACEs.\n", information.AceCount );
        for (i = 0; dacl && i < information.AceCount; i++)
        {
            DWORD sid_size;

            ace = NULL;
            ok( GetAce( dacl, i, (void **)&ace ),
                "failed to retrieve DACL ACE %lu.\n", i );
            if (!ace) continue;
            ok( ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE,
                "ACE %lu has type %u.\n", i, ace->Header.AceType );
            ok( !ace->Header.AceFlags,
                "ACE %lu has flags %#x.\n", i,
                ace->Header.AceFlags );
            ok( ace->Mask == GENERIC_ALL || ace->Mask == FILE_ALL_ACCESS,
                "ACE %lu has access mask %#lx.\n", i, ace->Mask );
            ok( IsValidSid( &ace->SidStart ),
                "ACE %lu has an invalid SID.\n", i );
            if (!IsValidSid( &ace->SidStart )) continue;
            sid_size = GetLengthSid( &ace->SidStart );
            ok( ace->Header.AceSize ==
                FIELD_OFFSET( ACCESS_ALLOWED_ACE, SidStart ) + sid_size,
                "ACE %lu has size %u for SID size %lu.\n", i,
                ace->Header.AceSize, sid_size );
            if (EqualSid( &ace->SidStart, user->User.Sid ))
                user_aces++;
            else if (EqualSid( &ace->SidStart,
                               (PSID)&local_system_sid ))
                system_aces++;
            else
                ok( 0, "ACE %lu belongs to another SID.\n", i );
        }
        ok( user_aces == 1, "DACL contains %u user ACEs.\n", user_aces );
        ok( system_aces <= 1, "DACL contains %u SYSTEM ACEs.\n",
            system_aces );
        ok( user_aces + system_aces == information.AceCount,
            "DACL contains unaccounted ACEs.\n" );
    }
    if (descriptor) LocalFree( descriptor );
    HeapFree( GetProcessHeap(), 0, user );
    CloseHandle( token );
}

static HRESULT WINAPI materialize_inspect_callback( void *opaque, HANDLE file )
{
    struct materialize_context *context = opaque;
    BYTE buffer[64];
    HANDLE writable;
    UINT32 path_length;
    DWORD read = 0, written = 0;

    context->inspect_count++;
    ok( GetFileType( file ) == FILE_TYPE_DISK,
        "materialized handle is not a disk file.\n" );
    ok( get_handle_path( file, context->file_path ),
        "failed to get materialized path, error %lu.\n", GetLastError() );
    path_length = lstrlenW( context->file_path );
    ok( path_length >= 5 &&
        wcsstr( context->file_path, L"appxsvc-bundle-" ) != NULL &&
        !lstrcmpiW( context->file_path + path_length - 5, L".msix" ),
        "unexpected private file name %s.\n",
        wine_dbgstr_w(context->file_path) );
    check_private_file_security( file );
    SetLastError( 0xdeadbeef );
    ok( !WriteFile( file, "x", 1, &written, NULL ) &&
        GetLastError() == ERROR_ACCESS_DENIED,
        "read-only handle accepted write, error %lu.\n", GetLastError() );
    SetLastError( 0xdeadbeef );
    writable = ReOpenFile(
        file, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_FLAG_SEQUENTIAL_SCAN );
    ok( writable == INVALID_HANDLE_VALUE &&
        GetLastError() == ERROR_SHARING_VIOLATION,
        "materialized file allowed a writer, handle %p, error %lu.\n",
        writable, GetLastError() );
    if (writable != INVALID_HANDLE_VALUE) CloseHandle( writable );
    ok( ReadFile( file, buffer, sizeof(buffer), &read, NULL ),
        "failed to read materialized file, error %lu.\n", GetLastError() );
    ok( read == context->data_size, "read %lu bytes.\n", read );
    ok( !memcmp( buffer, context->data, read ),
        "materialized data differs.\n" );
    if (context->retain_file)
    {
        context->retained_file = ReOpenFile(
            file, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_FLAG_SEQUENTIAL_SCAN );
        ok( context->retained_file != INVALID_HANDLE_VALUE,
            "failed to retain delete-pending file, error %lu.\n",
            GetLastError() );
    }
    if (context->inspect_enter_event)
        SetEvent( context->inspect_enter_event );
    if (context->inspect_release_event &&
        WaitForSingleObject( context->inspect_release_event, 5000 ) !=
        WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32( ERROR_TIMEOUT );
    return context->inspect_result;
}

static UINT32 count_private_temp_files(void)
{
    WIN32_FIND_DATAW data;
    WCHAR path[MAX_PATH];
    HANDLE find;
    DWORD length;
    UINT32 count = 0;

    length = GetTempPathW( MAX_PATH, path );
    if (!length || length >= MAX_PATH - 18) return MAXDWORD;
    lstrcatW( path, L"appxsvc-bundle-*" );
    find = FindFirstFileW( path, &data );
    if (find == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : MAXDWORD;
    do
    {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) count++;
    } while (FindNextFileW( find, &data ));
    FindClose( find );
    return count;
}

static void init_materialize( struct materialize_context *context )
{
    UINT32 i;

    memset( context, 0, sizeof(*context) );
    for (i = 0; i < 32; i++) context->data[i] = i ^ 0xa5;
    context->data_size = 32;
    context->read_limit = 7;
    context->fail_after = MAXDWORD;
    context->inspect_result = S_OK;
    context->retained_file = INVALID_HANDLE_VALUE;
}

static HRESULT materialize_with_options(
    struct materialize_context *context, UINT64 expected,
    const APPX_BUNDLE_INSPECT_OPTIONS *options )
{
    struct appx_bundle_materialize_test_source source;

    memset( &source, 0, sizeof(source) );
    source.size = sizeof(source);
    source.context = context;
    source.read = materialize_read_callback;
    source.inspect = materialize_inspect_callback;
    source.options = options;
    return p_appx_bundle_materialize_with_test_source( expected, &source );
}

static HRESULT materialize( struct materialize_context *context,
                            UINT64 expected )
{
    return materialize_with_options( context, expected, NULL );
}

static void init_inspect_options( APPX_BUNDLE_INSPECT_OPTIONS *options )
{
    memset( options, 0, sizeof(*options) );
    options->size = sizeof(*options);
    options->version = APPX_BUNDLE_INSPECT_OPTIONS_VERSION;
    options->lock_timeout_ms = 5000;
}

static BOOL create_materialize_directory( WCHAR path[MAX_PATH],
                                          HANDLE *directory )
{
    WCHAR temp[MAX_PATH];
    DWORD length;

    *directory = INVALID_HANDLE_VALUE;
    length = GetTempPathW( ARRAY_SIZE(temp), temp );
    if (!length || length >= ARRAY_SIZE(temp) ||
        !GetTempFileNameW( temp, L"abm", 0, path ))
        return FALSE;
    if (!DeleteFileW( path ) || !CreateDirectoryW( path, NULL ))
        return FALSE;
    *directory = CreateFileW(
        path, FILE_LIST_DIRECTORY | FILE_ADD_FILE |
              FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                             FILE_FLAG_OPEN_REPARSE_POINT, NULL );
    if (*directory == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW( path );
        return FALSE;
    }
    return TRUE;
}

struct materialize_thread
{
    struct materialize_context *context;
    const APPX_BUNDLE_INSPECT_OPTIONS *options;
    HRESULT result;
};

static DWORD WINAPI materialize_thread_proc( void *opaque )
{
    struct materialize_thread *thread = opaque;

    thread->result = materialize_with_options(
        thread->context, thread->context->data_size, thread->options );
    return 0;
}

static void test_materialization_options(void)
{
    struct materialize_context first, second, context;
    APPX_BUNDLE_INSPECT_OPTIONS options;
    struct materialize_thread first_call, second_call;
    HANDLE first_thread = NULL, second_thread = NULL;
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE entered = NULL, release = NULL, cancel = NULL;
    WCHAR path[MAX_PATH];
    UINT32 length;
    DWORD wait;
    HRESULT hr;

    init_inspect_options( &options );
    options.free_space_floor_bytes = ~(UINT64)0;
    init_materialize( &context );
    hr = materialize_with_options(
        &context, context.data_size, &options );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL),
        "custom free-space floor returned %#lx.\n", hr );
    ok( !context.read_count && !context.inspect_count,
        "space rejection read %ld times and inspected %u times.\n",
        context.read_count, context.inspect_count );

    init_inspect_options( &options );
    cancel = CreateEventW( NULL, TRUE, TRUE, NULL );
    ok( !!cancel, "failed to create cancellation event, error %lu.\n",
        GetLastError() );
    options.cancel_event = cancel;
    init_materialize( &context );
    hr = materialize_with_options(
        &context, context.data_size, &options );
    ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "pre-cancelled materialization returned %#lx.\n", hr );
    ok( !context.read_count && !context.inspect_count,
        "cancelled materialization read %ld times and inspected %u times.\n",
        context.read_count, context.inspect_count );
    if (cancel) CloseHandle( cancel );

    init_inspect_options( &options );
    ok( create_materialize_directory( path, &directory ),
        "failed to create private test directory, error %lu.\n",
        GetLastError() );
    if (directory != INVALID_HANDLE_VALUE)
    {
        options.temporary_directory = directory;
        init_materialize( &context );
        hr = materialize_with_options(
            &context, context.data_size, &options );
        ok( hr == S_OK, "custom-directory materialization returned %#lx.\n",
            hr );
        length = lstrlenW( path );
        ok( lstrlenW( context.file_path ) > length &&
            CompareStringOrdinal( context.file_path, length,
                                  path, length, TRUE ) == CSTR_EQUAL &&
            context.file_path[length] == '\\',
            "materialized outside requested directory: %s, expected %s.\n",
            wine_dbgstr_w(context.file_path), wine_dbgstr_w(path) );
        CloseHandle( directory );
        ok( RemoveDirectoryW( path ),
            "failed to remove test directory, error %lu.\n", GetLastError() );
    }

    init_inspect_options( &options );
    options.reserved = 1;
    init_materialize( &context );
    hr = materialize_with_options(
        &context, context.data_size, &options );
    ok( hr == E_INVALIDARG, "reserved options returned %#lx.\n", hr );
    options.reserved = 0;
    options.temporary_directory = INVALID_HANDLE_VALUE;
    hr = materialize_with_options(
        &context, context.data_size, &options );
    ok( hr == E_INVALIDARG, "invalid directory returned %#lx.\n", hr );

    init_inspect_options( &options );
    entered = CreateEventW( NULL, TRUE, FALSE, NULL );
    release = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!entered && !!release,
        "failed to create serialization events, error %lu.\n",
        GetLastError() );
    if (entered && release)
    {
        init_materialize( &first );
        init_materialize( &second );
        first.inspect_enter_event = entered;
        first.inspect_release_event = release;
        memset( &first_call, 0, sizeof(first_call) );
        memset( &second_call, 0, sizeof(second_call) );
        first_call.context = &first;
        first_call.options = &options;
        second_call.context = &second;
        second_call.options = &options;
        first_thread = CreateThread(
            NULL, 0, materialize_thread_proc, &first_call, 0, NULL );
        ok( !!first_thread, "failed to create first thread, error %lu.\n",
            GetLastError() );
        wait = first_thread ?
               WaitForSingleObject( entered, 5000 ) : WAIT_FAILED;
        ok( wait == WAIT_OBJECT_0,
            "first materialization did not enter inspect, wait %#lx.\n",
            wait );
        if (wait == WAIT_OBJECT_0)
        {
            second_thread = CreateThread(
                NULL, 0, materialize_thread_proc, &second_call, 0, NULL );
            ok( !!second_thread,
                "failed to create second thread, error %lu.\n",
                GetLastError() );
            if (second_thread)
            {
                wait = WaitForSingleObject( second_thread, 100 );
                ok( wait == WAIT_TIMEOUT,
                    "second materialization was not serialized, wait %#lx.\n",
                    wait );
                ok( !second.read_count,
                    "second materialization read %ld times before release.\n",
                    second.read_count );
            }
        }
        SetEvent( release );
        if (first_thread)
        {
            wait = WaitForSingleObject( first_thread, 5000 );
            ok( wait == WAIT_OBJECT_0,
                "first materialization did not finish, wait %#lx.\n", wait );
            CloseHandle( first_thread );
        }
        if (second_thread)
        {
            wait = WaitForSingleObject( second_thread, 5000 );
            ok( wait == WAIT_OBJECT_0,
                "second materialization did not finish, wait %#lx.\n", wait );
            CloseHandle( second_thread );
        }
        ok( first_call.result == S_OK,
            "first materialization returned %#lx.\n", first_call.result );
        ok( second_call.result == S_OK,
            "second materialization returned %#lx.\n", second_call.result );
    }
    if (release) CloseHandle( release );
    if (entered) CloseHandle( entered );
}

static void test_materialization(void)
{
    struct appx_bundle_materialize_test_source invalid = {0};
    struct materialize_context context;
    UINT32 before, after;
    HRESULT hr;

    before = count_private_temp_files();
    init_materialize( &context );
    hr = materialize( &context, context.data_size );
    ok( hr == S_OK, "materialization returned %#lx.\n", hr );
    ok( context.inspect_count == 1, "inspect called %u times.\n",
        context.inspect_count );
    ok( GetFileAttributesW( context.file_path ) == INVALID_FILE_ATTRIBUTES,
        "delete-pending file remains at %s.\n",
        wine_dbgstr_w(context.file_path) );
    after = count_private_temp_files();
    ok( before == MAXDWORD || after == MAXDWORD || before == after,
        "private file count changed from %u to %u.\n", before, after );

    init_materialize( &context );
    context.retain_file = TRUE;
    hr = materialize( &context, context.data_size );
    ok( hr == S_OK, "retained materialization returned %#lx.\n", hr );
    ok( context.retained_file != INVALID_HANDLE_VALUE,
        "materialized file was not retained.\n" );
    ok( GetFileAttributesW( context.file_path ) == INVALID_FILE_ATTRIBUTES,
        "delete-pending path remains visible at %s.\n",
        wine_dbgstr_w(context.file_path) );
    if (context.retained_file != INVALID_HANDLE_VALUE)
    {
        BYTE retained[64];
        LARGE_INTEGER zero;
        DWORD read = 0;

        zero.QuadPart = 0;
        ok( SetFilePointerEx( context.retained_file, zero, NULL, FILE_BEGIN ) &&
            ReadFile( context.retained_file, retained, sizeof(retained),
                      &read, NULL ),
            "failed to read retained file, error %lu.\n", GetLastError() );
        ok( read == context.data_size &&
            !memcmp( retained, context.data, read ),
            "retained file contents differ (%lu bytes).\n", read );
        CloseHandle( context.retained_file );
    }
    ok( GetFileAttributesW( context.file_path ) == INVALID_FILE_ATTRIBUTES,
        "retained file remains after final close.\n" );

    init_materialize( &context );
    context.fail_after = 14;
    context.read_failure = HRESULT_FROM_WIN32( ERROR_CANCELLED );
    before = count_private_temp_files();
    hr = materialize( &context, context.data_size );
    ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "read cancellation returned %#lx.\n", hr );
    ok( context.inspect_count == 0, "inspect called after cancellation.\n" );
    after = count_private_temp_files();
    ok( before == MAXDWORD || after == MAXDWORD || before == after,
        "cancel left a private file (%u -> %u).\n", before, after );

    init_materialize( &context );
    context.inspect_result = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    hr = materialize( &context, context.data_size );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
        "inspect failure returned %#lx.\n", hr );
    ok( GetFileAttributesW( context.file_path ) == INVALID_FILE_ATTRIBUTES,
        "inspect failure left %s.\n", wine_dbgstr_w(context.file_path) );

    init_materialize( &context );
    hr = materialize( &context, context.data_size + 1 );
    ok( hr == APPX_E_CORRUPT_CONTENT,
        "early terminal returned %#lx.\n", hr );

    init_materialize( &context );
    context.data_size++;
    hr = materialize( &context, context.data_size - 1 );
    ok( hr == APPX_E_CORRUPT_CONTENT,
        "trailing byte returned %#lx.\n", hr );

    init_materialize( &context );
    before = count_private_temp_files();
    hr = materialize( &context, ~(UINT64)0 );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL),
        "unreservable materialization returned %#lx.\n", hr );
    ok( context.inspect_count == 0,
        "unreservable materialization called inspect.\n" );
    after = count_private_temp_files();
    ok( before == MAXDWORD || after == MAXDWORD || before == after,
        "disk-space rejection left a private file (%u -> %u).\n",
        before, after );

    ok( p_appx_bundle_materialize_with_test_source( 0, &invalid ) ==
        E_INVALIDARG, "invalid source was accepted.\n" );
    invalid.size = sizeof(invalid);
    invalid.read = materialize_read_callback;
    ok( p_appx_bundle_materialize_with_test_source( 1, &invalid ) ==
        E_INVALIDARG, "missing inspect callback was accepted.\n" );
}

START_TEST(bundle)
{
    HMODULE module = LoadLibraryA( "appxsvc.dll" );

    ok( !!module, "failed to load appxsvc.dll, error %lu.\n",
        GetLastError() );
    if (!module) return;

    p_appx_bundle_manifest_parse = (void *)GetProcAddress(
        module, "appx_bundle_manifest_parse" );
    p_appx_bundle_manifest_free = (void *)GetProcAddress(
        module, "appx_bundle_manifest_free" );
    p_appx_bundle_manifest_get_package = (void *)GetProcAddress(
        module, "appx_bundle_manifest_get_package" );
    p_appx_bundle_validate_with_test_source = (void *)GetProcAddress(
        module, "appx_bundle_validate_with_test_source" );
    p_appx_bundle_materialize_with_test_source = (void *)GetProcAddress(
        module, "appx_bundle_materialize_with_test_source" );
    if (!p_appx_bundle_manifest_parse || !p_appx_bundle_manifest_free ||
        !p_appx_bundle_manifest_get_package ||
        !p_appx_bundle_validate_with_test_source ||
        !p_appx_bundle_materialize_with_test_source)
    {
        win_skip( "bundle inspection exports are unavailable: module %p,"
                  " parse %p, free %p, package %p, validate %p,"
                  " materialize %p.\n", module,
                  p_appx_bundle_manifest_parse,
                  p_appx_bundle_manifest_free,
                  p_appx_bundle_manifest_get_package,
                  p_appx_bundle_validate_with_test_source,
                  p_appx_bundle_materialize_with_test_source );
        FreeLibrary( module );
        return;
    }

    test_layout_and_identity();
    test_flat_and_unsupported();
    test_materialization();
    test_materialization_options();
    FreeLibrary( module );
}
