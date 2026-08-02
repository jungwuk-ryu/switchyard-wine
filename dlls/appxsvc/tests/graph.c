/*
 * AppX packaged process graph tests
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

#include "../catalog.h"
#include "../graph.h"
#include "wine/test.h"

#define GRAPH_HEADER_TOTAL_SIZE_OFFSET             16
#define GRAPH_HEADER_TARGET_ARCHITECTURE_OFFSET    40
#define GRAPH_HEADER_PACKAGE_COUNT_OFFSET          44
#define GRAPH_HEADER_PACKAGES_OFFSET               48
#define GRAPH_HEADER_LOADER_COUNT_OFFSET           52
#define GRAPH_HEADER_LOADERS_OFFSET                56
#define GRAPH_HEADER_STRINGS_OFFSET                60
#define GRAPH_HEADER_STRINGS_SIZE_OFFSET           64
#define GRAPH_HEADER_APPLICATION_ID_REF_OFFSET     72
#define GRAPH_HEADER_AUMID_REF_OFFSET              80
#define GRAPH_HEADER_CLASS_COUNT_OFFSET            104
#define GRAPH_HEADER_CLASSES_OFFSET                108
#define GRAPH_HEADER_VOLUME_SERIAL_OFFSET          112
#define GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET        116
#define GRAPH_HEADER_FILE_INDEX_LOW_OFFSET         120
#define GRAPH_HEADER_OBJECT_ID_OFFSET              124
#define GRAPH_HEADER_RESERVED_OFFSET               140
#define GRAPH_PACKAGE_ARCHITECTURE_OFFSET          8
#define GRAPH_PACKAGE_FLAGS_OFFSET                 12
#define GRAPH_PACKAGE_RANK_OFFSET                  16
#define GRAPH_PACKAGE_ROOT_REF_OFFSET              104
#define GRAPH_LOADER_PACKAGE_INDEX_OFFSET          0
#define GRAPH_LOADER_SEARCH_RANK_OFFSET             4
#define GRAPH_LOADER_BASENAME_REF_OFFSET           8
#define GRAPH_LOADER_PATH_REF_OFFSET               16
#define GRAPH_LOADER_VOLUME_SERIAL_OFFSET          24
#define GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET        28
#define GRAPH_LOADER_FILE_INDEX_LOW_OFFSET         32
#define GRAPH_LOADER_RESERVED_OFFSET               36
#define GRAPH_LOADER_CHANGE_TIME_OFFSET             40
#define GRAPH_LOADER_FILE_SIZE_OFFSET               48
#define GRAPH_LOADER_OBJECT_ID_OFFSET               56
#define GRAPH_CLASS_PACKAGE_INDEX_OFFSET           0
#define GRAPH_CLASS_THREADING_MODEL_OFFSET         4
#define GRAPH_CLASS_ID_REF_OFFSET                  8
#define GRAPH_CLASS_PATH_REF_OFFSET                16
#define GRAPH_CLASS_VOLUME_SERIAL_OFFSET           24
#define GRAPH_CLASS_FILE_INDEX_HIGH_OFFSET         28
#define GRAPH_CLASS_FILE_INDEX_LOW_OFFSET          32
#define GRAPH_CLASS_LOADER_INDEX_OFFSET             36
#define GRAPH_CLASS_CHANGE_TIME_OFFSET              40
#define GRAPH_CLASS_FILE_SIZE_OFFSET                48

static HRESULT (WINAPI *p_appx_catalog_snapshot_create)(
    UINT64, const struct appx_catalog_package *, UINT32, APPX_CATALOG_SNAPSHOT ** );
static void (WINAPI *p_appx_catalog_snapshot_free)( APPX_CATALOG_SNAPSHOT * );
static const struct appx_catalog_package *(WINAPI
    *p_appx_catalog_snapshot_get_package)( const APPX_CATALOG_SNAPSHOT *, UINT32 );
static HRESULT (WINAPI *p_appx_package_graph_create)(
    const APPX_CATALOG_SNAPSHOT *, const WCHAR *, const WCHAR *, const WCHAR *,
    enum appx_catalog_architecture, UINT64,
    const struct appx_graph_file_identity *,
    const struct appx_graph_loader_file *, UINT32, APPX_PACKAGE_GRAPH ** );
static HRESULT (WINAPI *p_appx_package_graph_create_with_classes)(
    const APPX_CATALOG_SNAPSHOT *, const WCHAR *, const WCHAR *, const WCHAR *,
    enum appx_catalog_architecture, UINT64,
    const struct appx_graph_file_identity *,
    const struct appx_graph_loader_file *, UINT32,
    const struct appx_graph_inproc_class *, UINT32, APPX_PACKAGE_GRAPH ** );
static HRESULT (WINAPI *p_appx_package_graph_resolve_direct_dependencies)(
    const APPX_CATALOG_SNAPSHOT *, const WCHAR *,
    enum appx_catalog_architecture, UINT32, UINT32 *, UINT32 * );
static HRESULT (WINAPI *p_appx_package_graph_from_blob)(
    const void *, SIZE_T, APPX_PACKAGE_GRAPH ** );
static HRESULT (WINAPI *p_appx_package_graph_clone)(
    const APPX_PACKAGE_GRAPH *, APPX_PACKAGE_GRAPH ** );
static HRESULT (WINAPI *p_appx_package_graph_validate_blob)( const void *, SIZE_T );
static void (WINAPI *p_appx_package_graph_free)( APPX_PACKAGE_GRAPH * );
static const BYTE *(WINAPI *p_appx_package_graph_get_blob)(
    const APPX_PACKAGE_GRAPH *, UINT32 * );
static UINT64 (WINAPI *p_appx_package_graph_get_epoch)(
    const APPX_PACKAGE_GRAPH * );
static UINT64 (WINAPI *p_appx_package_graph_get_revision)(
    const APPX_PACKAGE_GRAPH * );
static enum appx_catalog_architecture (WINAPI
    *p_appx_package_graph_get_target_architecture)( const APPX_PACKAGE_GRAPH * );
static UINT32 (WINAPI *p_appx_package_graph_get_package_count)(
    const APPX_PACKAGE_GRAPH * );
static HRESULT (WINAPI *p_appx_package_graph_get_package)(
    const APPX_PACKAGE_GRAPH *, UINT32, struct appx_graph_package * );
static HRESULT (WINAPI *p_appx_package_graph_get_application)(
    const APPX_PACKAGE_GRAPH *, struct appx_graph_application * );
static HRESULT (WINAPI *p_appx_package_graph_lookup_basename)(
    const APPX_PACKAGE_GRAPH *, const WCHAR *, struct appx_graph_loader_match * );
static UINT32 (WINAPI *p_appx_package_graph_get_inproc_class_count)(
    const APPX_PACKAGE_GRAPH * );
static HRESULT (WINAPI *p_appx_package_graph_get_inproc_class)(
    const APPX_PACKAGE_GRAPH *, UINT32, struct appx_graph_class_match * );
static HRESULT (WINAPI *p_appx_package_graph_lookup_inproc_class)(
    const APPX_PACKAGE_GRAPH *, const WCHAR *, struct appx_graph_class_match * );

#define appx_catalog_snapshot_create p_appx_catalog_snapshot_create
#define appx_catalog_snapshot_free p_appx_catalog_snapshot_free
#define appx_catalog_snapshot_get_package p_appx_catalog_snapshot_get_package
#define appx_package_graph_create(snapshot, package, application, architecture, \
                                  revision, ...) \
    p_appx_package_graph_create((snapshot), store_root, (package), \
        (application), (architecture), (revision), &test_application_identity, \
        __VA_ARGS__)
#define appx_package_graph_create_with_classes(snapshot, package, application, \
                                               architecture, revision, ...) \
    p_appx_package_graph_create_with_classes((snapshot), store_root, (package), \
        (application), (architecture), (revision), &test_application_identity, \
        __VA_ARGS__)
#define appx_package_graph_from_blob p_appx_package_graph_from_blob
#define appx_package_graph_resolve_direct_dependencies \
    p_appx_package_graph_resolve_direct_dependencies
#define appx_package_graph_clone p_appx_package_graph_clone
#define appx_package_graph_validate_blob p_appx_package_graph_validate_blob
#define appx_package_graph_free p_appx_package_graph_free
#define appx_package_graph_get_blob p_appx_package_graph_get_blob
#define appx_package_graph_get_epoch p_appx_package_graph_get_epoch
#define appx_package_graph_get_revision p_appx_package_graph_get_revision
#define appx_package_graph_get_target_architecture \
    p_appx_package_graph_get_target_architecture
#define appx_package_graph_get_package_count p_appx_package_graph_get_package_count
#define appx_package_graph_get_package p_appx_package_graph_get_package
#define appx_package_graph_get_application p_appx_package_graph_get_application
#define appx_package_graph_lookup_basename p_appx_package_graph_lookup_basename
#define appx_package_graph_get_inproc_class_count \
    p_appx_package_graph_get_inproc_class_count
#define appx_package_graph_get_inproc_class \
    p_appx_package_graph_get_inproc_class
#define appx_package_graph_lookup_inproc_class \
    p_appx_package_graph_lookup_inproc_class

static const WCHAR publisher[] = L"CN=Contoso";
static const WCHAR publisher_id[] = L"contosoid";
static const WCHAR store_root[] =
    L"C:\\Program Files\\WindowsApps\\.wine-msix-store";
#define TEST_OBJECT_ID(index) \
    {0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, \
     (index) & 0xff, ((index) >> 8) & 0xff, \
     ((index) >> 16) & 0xff, ((index) >> 24) & 0xff, 0, 0, 0, 0}
#define TEST_FILE_OBJECT_ID TEST_OBJECT_ID(0x50607080)
static const BYTE test_file_object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE] =
    TEST_FILE_OBJECT_ID;

static BOOL object_id_matches(
    const BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE], UINT32 index )
{
    const BYTE expected[WINE_APPX_GRAPH_OBJECT_ID_SIZE] =
        TEST_OBJECT_ID(index);

    return !memcmp( object_id, expected, sizeof(expected) );
}

static const struct appx_graph_file_identity test_application_identity =
    {0x10203040, 0, 0x50607080, TEST_FILE_OBJECT_ID};

#define TEST_FILE_IDENTITY 0x10203040, 0, 0x50607080
#define TEST_FILE_STAMP 0x01d0000000000000ULL, 4096
static const struct appx_catalog_application main_application =
{
    L"App", L"bin\\main.exe", L"Windows.FullTrustApplication",
    L"", L"", APPX_CATALOG_ACTIVATION_FULL_TRUST
};

static void init_content_id( BYTE content_id[APPX_CATALOG_CONTENT_ID_SIZE],
                             BYTE seed )
{
    UINT32 i;

    for (i = 0; i < APPX_CATALOG_CONTENT_ID_SIZE; i++)
        content_id[i] = seed + i;
}

static void init_package( struct appx_catalog_package *package,
                          const WCHAR *name, const WCHAR *full_name,
                          const WCHAR *family_name, const WCHAR *root,
                          UINT16 version,
                          enum appx_catalog_architecture architecture,
                          UINT32 flags )
{
    memset( package, 0, sizeof(*package) );
    package->name = name;
    package->publisher = publisher;
    package->resource_id = L"";
    package->publisher_id = publisher_id;
    package->full_name = full_name;
    package->family_name = family_name;
    package->payload_path = root;
    package->version.major = version;
    package->architecture = architecture;
    package->flags = flags;
    init_content_id( package->content_id, name[0] );
}

static void make_main( struct appx_catalog_package *package,
                       const struct appx_catalog_dependency *dependencies,
                       UINT32 dependency_count )
{
    init_package( package, L"Contoso.Main",
                  L"Contoso.Main_1.0.0.0_x64__contosoid",
                  L"Contoso.Main_contosoid", L"payloads\\main\\content",
                  1, APPX_CATALOG_ARCHITECTURE_X64,
                  APPX_CATALOG_PACKAGE_ACTIVE | APPX_CATALOG_PACKAGE_SIGNED );
    package->application_count = 1;
    package->applications = &main_application;
    package->dependency_count = dependency_count;
    package->dependencies = dependencies;
}

static void make_framework( struct appx_catalog_package *package,
                            const WCHAR *name, const WCHAR *full_name,
                            const WCHAR *family_name, const WCHAR *root,
                            UINT16 version,
                            enum appx_catalog_architecture architecture )
{
    init_package( package, name, full_name, family_name, root, version,
                  architecture, APPX_CATALOG_PACKAGE_ACTIVE |
                  APPX_CATALOG_PACKAGE_SIGNED |
                  APPX_CATALOG_PACKAGE_FRAMEWORK );
}

static APPX_CATALOG_SNAPSHOT *create_snapshot(
    UINT64 epoch, const struct appx_catalog_package *packages, UINT32 count )
{
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    HRESULT hr = appx_catalog_snapshot_create( epoch, packages, count, &snapshot );

    ok( hr == S_OK, "snapshot creation returned %#lx.\n", hr );
    return snapshot;
}

static void check_package( const APPX_PACKAGE_GRAPH *graph, UINT32 index,
                           const WCHAR *full_name, UINT32 rank )
{
    struct appx_graph_package package;
    HRESULT hr = appx_package_graph_get_package( graph, index, &package );

    ok( hr == S_OK, "package %u returned %#lx.\n", index, hr );
    if (FAILED(hr)) return;
    ok( !lstrcmpW( package.full_name, full_name ),
        "package %u got full name %s.\n", index, wine_dbgstr_w(package.full_name) );
    ok( package.rank == rank, "package %u got rank %u.\n", index, package.rank );
    ok( package.root && package.root[0],
        "package %u has no immutable root.\n", index );
}

static void test_basic_graph( void )
{
    static const struct appx_catalog_dependency dependencies[] =
    {
        {L"Contoso.FrameworkA", publisher, {2, 0, 0, 0}},
        {L"Contoso.FrameworkB", publisher, {1, 0, 0, 0}},
    };
    static const struct appx_graph_loader_file loader_files[] =
    {
        {L"Contoso.Main_1.0.0.0_x64__contosoid", L"bin\\component.dll",
         TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
        {L"Contoso.Main_1.0.0.0_x64__contosoid", L"bin\\Shared.dll",
         TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
        {L"Contoso.FrameworkA_3.0.0.0_x64__contosoid",
         L"lib\\framework.dll", TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP,
         TEST_FILE_OBJECT_ID},
        {L"Contoso.FrameworkA_3.0.0.0_x64__contosoid", L"lib\\shared.DLL",
         TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
        {L"Contoso.FrameworkB_1.0.0.0_neutral__contosoid", L"Other.dll",
         TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
    };
    static const struct appx_graph_inproc_class classes[] =
    {
        {L"Contoso.Main_1.0.0.0_x64__contosoid",
         L"Wine.Main.Component", L"BIN\\COMPONENT.DLL", 2,
         TEST_FILE_IDENTITY, TEST_FILE_STAMP},
        {L"Contoso.FrameworkA_3.0.0.0_x64__contosoid",
         L"Wine.Framework.Component", L"lib\\framework.dll", 0,
         TEST_FILE_IDENTITY, TEST_FILE_STAMP},
    };
    struct appx_catalog_package packages[4];
    struct appx_graph_application application;
    struct appx_graph_class_match class_match;
    struct appx_graph_loader_match match;
    struct appx_graph_package package;
    APPX_CATALOG_SNAPSHOT *snapshot;
    APPX_PACKAGE_GRAPH *graph = NULL, *copy = NULL, *clone = NULL;
    const BYTE *blob, *copy_blob;
    UINT32 size, copy_size;
    HRESULT hr;

    make_main( packages, dependencies, ARRAY_SIZE(dependencies) );
    make_framework( packages + 1, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_1.0.0.0_x64__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a1\\content", 1,
                    APPX_CATALOG_ARCHITECTURE_X64 );
    make_framework( packages + 2, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_3.0.0.0_x64__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a3\\content", 3,
                    APPX_CATALOG_ARCHITECTURE_X64 );
    make_framework( packages + 3, L"Contoso.FrameworkB",
                    L"Contoso.FrameworkB_1.0.0.0_neutral__contosoid",
                    L"Contoso.FrameworkB_contosoid",
                    L"payloads\\b1\\content", 1,
                    APPX_CATALOG_ARCHITECTURE_NEUTRAL );
    snapshot = create_snapshot( 0x123456789ULL, packages,
                                ARRAY_SIZE(packages) );
    if (!snapshot) return;

    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 0xabcdef0123ULL,
        loader_files, ARRAY_SIZE(loader_files),
        classes, ARRAY_SIZE(classes), &graph );
    ok( hr == S_OK, "graph creation returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );
    if (FAILED(hr)) return;

    ok( appx_package_graph_get_epoch( graph ) == 0x123456789ULL,
        "got epoch %s.\n",
        wine_dbgstr_longlong(appx_package_graph_get_epoch(graph)) );
    ok( appx_package_graph_get_revision( graph ) == 0xabcdef0123ULL,
        "got revision %s.\n",
        wine_dbgstr_longlong(appx_package_graph_get_revision(graph)) );
    ok( appx_package_graph_get_target_architecture( graph ) ==
        APPX_CATALOG_ARCHITECTURE_X64, "got architecture %u.\n",
        appx_package_graph_get_target_architecture(graph) );
    ok( appx_package_graph_get_package_count( graph ) == 3,
        "got package count %u.\n",
        appx_package_graph_get_package_count(graph) );
    check_package( graph, 0, packages[0].full_name, 0 );
    check_package( graph, 1, packages[2].full_name, 1 );
    check_package( graph, 2, packages[3].full_name, 2 );
    hr = appx_package_graph_get_package( graph, 0, &package );
    ok( hr == S_OK, "main package lookup returned %#lx.\n", hr );
    ok( !lstrcmpW( package.root,
                   L"C:\\Program Files\\WindowsApps\\.wine-msix-store\\"
                   L"payloads\\main\\content" ),
        "got absolute main root %s.\n", wine_dbgstr_w(package.root) );
    ok( !(package.flags & APPX_GRAPH_PACKAGE_DIRECT),
        "main package incorrectly marked direct %#x.\n", package.flags );
    hr = appx_package_graph_get_package( graph, 1, &package );
    ok( hr == S_OK && (package.flags & APPX_GRAPH_PACKAGE_DIRECT),
        "first dependency flags %#x, hr %#lx.\n", package.flags, hr );
    hr = appx_package_graph_get_package( graph, 2, &package );
    ok( hr == S_OK && (package.flags & APPX_GRAPH_PACKAGE_DIRECT),
        "second dependency flags %#x, hr %#lx.\n", package.flags, hr );
    hr = appx_package_graph_get_package( graph, 3, &package );
    ok( hr == E_INVALIDARG, "out-of-range package returned %#lx.\n", hr );

    hr = appx_package_graph_get_application( graph, &application );
    ok( hr == S_OK, "application lookup returned %#lx.\n", hr );
    ok( !lstrcmpW( application.id, L"App" ), "got id %s.\n",
        wine_dbgstr_w(application.id) );
    ok( !lstrcmpW( application.aumid, L"Contoso.Main_contosoid!App" ),
        "got AUMID %s.\n", wine_dbgstr_w(application.aumid) );
    ok( !lstrcmpW( application.executable, L"bin\\main.exe" ),
        "got executable %s.\n", wine_dbgstr_w(application.executable) );
    ok( application.activation_kind == APPX_CATALOG_ACTIVATION_FULL_TRUST,
        "got activation kind %u.\n", application.activation_kind );
    ok( application.volume_serial == test_application_identity.volume_serial &&
        application.file_index_high ==
            test_application_identity.file_index_high &&
        application.file_index_low == test_application_identity.file_index_low &&
        !memcmp( application.object_id, test_file_object_id,
                 sizeof(application.object_id) ),
        "got application identity %08x:%08x%08x.\n",
        application.volume_serial, application.file_index_high,
        application.file_index_low );

    hr = appx_package_graph_lookup_basename( graph, L"SHARED.DLL", &match );
    ok( hr == S_OK, "shared lookup returned %#lx.\n", hr );
    ok( match.package_index == 0, "shared lookup got package %u.\n",
        match.package_index );
    ok( !lstrcmpW( match.relative_path, L"bin\\Shared.dll" ),
        "shared lookup got path %s.\n", wine_dbgstr_w(match.relative_path) );
    ok( match.volume_serial == 0x10203040 &&
        !match.file_index_high && match.file_index_low == 0x50607080,
        "shared lookup got identity %08x:%08x%08x.\n",
        match.volume_serial, match.file_index_high, match.file_index_low );
    ok( match.change_time == 0x01d0000000000000ULL &&
        match.file_size == 4096 &&
        !memcmp( match.object_id, test_file_object_id,
                 sizeof(match.object_id) ),
        "shared lookup got stamp %s:%s.\n",
        wine_dbgstr_longlong(match.change_time),
        wine_dbgstr_longlong(match.file_size) );
    hr = appx_package_graph_lookup_basename( graph, L"other.dll", &match );
    ok( hr == S_OK, "other lookup returned %#lx.\n", hr );
    ok( match.package_index == 2, "other lookup got package %u.\n",
        match.package_index );
    hr = appx_package_graph_lookup_basename( graph, L"missing.dll", &match );
    ok( hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND),
        "missing lookup returned %#lx.\n", hr );
    hr = appx_package_graph_lookup_basename( graph, L"..\\bad.dll", &match );
    ok( hr == E_INVALIDARG, "invalid basename returned %#lx.\n", hr );

    ok( appx_package_graph_get_inproc_class_count(graph) == 2,
        "got class count %u.\n",
        appx_package_graph_get_inproc_class_count(graph) );
    hr = appx_package_graph_get_inproc_class( graph, 0, &class_match );
    ok( hr == S_OK, "class 0 returned %#lx.\n", hr );
    ok( !lstrcmpW( class_match.activatable_class_id,
                    L"Wine.Framework.Component" ),
        "got class id %s.\n",
        wine_dbgstr_w(class_match.activatable_class_id) );
    ok( class_match.package_index == 1 && class_match.threading_model == 0,
        "got class package %u, threading %u.\n",
        class_match.package_index, class_match.threading_model );
    ok( !lstrcmpW( class_match.path, L"lib\\framework.dll" ),
        "got class path %s.\n", wine_dbgstr_w(class_match.path) );
    ok( class_match.volume_serial == 0x10203040 &&
        !class_match.file_index_high &&
        class_match.file_index_low == 0x50607080,
        "got class identity %08x:%08x%08x.\n",
        class_match.volume_serial, class_match.file_index_high,
        class_match.file_index_low );
    ok( class_match.change_time == 0x01d0000000000000ULL &&
        class_match.file_size == 4096 &&
        !memcmp( class_match.object_id, test_file_object_id,
                 sizeof(class_match.object_id) ),
        "got class stamp %s:%s.\n",
        wine_dbgstr_longlong(class_match.change_time),
        wine_dbgstr_longlong(class_match.file_size) );
    hr = appx_package_graph_lookup_inproc_class(
        graph, L"Wine.Main.Component", &class_match );
    ok( hr == S_OK, "main class lookup returned %#lx.\n", hr );
    ok( class_match.package_index == 0 && class_match.threading_model == 2,
        "got main class package %u, threading %u.\n",
        class_match.package_index, class_match.threading_model );
    ok( !lstrcmpW( class_match.path, L"bin\\component.dll" ),
        "got canonical main class path %s.\n",
        wine_dbgstr_w(class_match.path) );
    hr = appx_package_graph_lookup_inproc_class(
        graph, L"wine.main.component", &class_match );
    ok( hr == S_OK && class_match.package_index == 0,
        "ASCII case-folded class lookup returned %#lx, package %u.\n",
        hr, class_match.package_index );
    hr = appx_package_graph_get_inproc_class( graph, 2, &class_match );
    ok( hr == E_INVALIDARG, "out-of-range class returned %#lx.\n", hr );

    blob = appx_package_graph_get_blob( graph, &size );
    ok( !!blob && size >= APPX_GRAPH_BLOB_HEADER_SIZE,
        "got blob %p, size %u.\n", blob, size );
    hr = appx_package_graph_validate_blob( blob, size );
    ok( hr == S_OK, "blob validation returned %#lx.\n", hr );
    hr = appx_package_graph_from_blob( blob, size, &copy );
    ok( hr == S_OK, "blob clone returned %#lx.\n", hr );
    hr = appx_package_graph_clone( graph, &clone );
    ok( hr == S_OK, "graph clone returned %#lx.\n", hr );
    copy_blob = appx_package_graph_get_blob( copy, &copy_size );
    ok( copy_size == size && !memcmp( copy_blob, blob, size ),
        "blob clone differs from source.\n" );
    ok( copy_blob != blob, "blob clone aliases source storage.\n" );

    appx_package_graph_free( clone );
    appx_package_graph_free( copy );
    appx_package_graph_free( graph );
}

static void test_loader_search_order( void )
{
    static const struct appx_graph_loader_file loader_files[] =
    {
        {L"Contoso.Main_1.0.0.0_x64__contosoid", L"later\\same.dll",
         TEST_FILE_IDENTITY, 1, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
        {L"Contoso.Main_1.0.0.0_x64__contosoid", L"first\\SAME.dll",
         TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
        {L"Contoso.Main_1.0.0.0_x64__contosoid", L"hidden\\only.dll",
         TEST_FILE_IDENTITY, WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY,
         TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
        {L"Contoso.Main_1.0.0.0_x64__contosoid", L"tools\\runner.exe",
         TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
    };
    struct appx_catalog_package package;
    struct appx_graph_loader_match match;
    APPX_CATALOG_SNAPSHOT *snapshot;
    APPX_PACKAGE_GRAPH *graph = NULL;
    HRESULT hr;

    make_main( &package, NULL, 0 );
    snapshot = create_snapshot( 1, &package, 1 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, package.full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files,
        ARRAY_SIZE(loader_files), &graph );
    ok( hr == S_OK, "search-order graph returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        hr = appx_package_graph_lookup_basename(
            graph, L"same.dll", &match );
        ok( hr == S_OK && !lstrcmpW( match.relative_path,
                                     L"first\\SAME.dll" ),
            "ranked lookup returned %#lx, path %s.\n",
            hr, wine_dbgstr_w(match.relative_path) );
        hr = appx_package_graph_lookup_basename(
            graph, L"only.dll", &match );
        ok( hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND),
            "explicit-only lookup returned %#lx.\n", hr );
        hr = appx_package_graph_lookup_basename(
            graph, L"runner.exe", &match );
        ok( hr == S_OK && !lstrcmpW( match.relative_path,
                                     L"tools\\runner.exe" ),
            "executable lookup returned %#lx, path %s.\n",
            hr, wine_dbgstr_w(match.relative_path) );
    }
    appx_package_graph_free( graph );
    appx_catalog_snapshot_free( snapshot );
}

static void test_class_loader_search_boundaries( void )
{
    static const struct appx_catalog_dependency dependencies[] =
    {
        {L"Framework.A", publisher, {1, 0, 0, 0}},
        {L"Framework.B", publisher, {1, 0, 0, 0}},
    };
    static const struct appx_graph_loader_file loaders[] =
    {
        {L"Contoso.Main_1.0.0.0_x64__contosoid", L"main\\shared.dll",
         0x10203040, 0, 0x50607081, 0, TEST_FILE_STAMP,
         TEST_OBJECT_ID(0x50607081)},
        {L"Framework.A_1.0.0.0_x64__contosoid", L"a\\SHARED.DLL",
         0x10203040, 0, 0x50607082, 0, TEST_FILE_STAMP,
         TEST_OBJECT_ID(0x50607082)},
        {L"Framework.B_1.0.0.0_x64__contosoid", L"b\\Shared.dll",
         0x10203040, 0, 0x50607083, 0, TEST_FILE_STAMP,
         TEST_OBJECT_ID(0x50607083)},
    };
    static const struct appx_graph_inproc_class classes[] =
    {
        {L"Contoso.Main_1.0.0.0_x64__contosoid",
         L"Wine.Boundary.First", L"MAIN\\SHARED.DLL", 0,
         0x10203040, 0, 0x50607081, TEST_FILE_STAMP},
        {L"Framework.A_1.0.0.0_x64__contosoid",
         L"Wine.Boundary.Middle", L"A\\shared.dll", 0,
         0x10203040, 0, 0x50607082, TEST_FILE_STAMP},
        {L"Framework.B_1.0.0.0_x64__contosoid",
         L"Wine.Boundary.Last", L"B\\SHARED.DLL", 0,
         0x10203040, 0, 0x50607083, TEST_FILE_STAMP},
    };
    struct appx_graph_class_match match;
    struct appx_catalog_package packages[3];
    APPX_CATALOG_SNAPSHOT *snapshot;
    APPX_PACKAGE_GRAPH *graph = NULL;
    HRESULT hr;

    make_main( packages, dependencies, ARRAY_SIZE(dependencies) );
    make_framework( packages + 1, L"Framework.A",
                    L"Framework.A_1.0.0.0_x64__contosoid",
                    L"Framework.A_contosoid", L"payloads\\a\\content",
                    1, APPX_CATALOG_ARCHITECTURE_X64 );
    make_framework( packages + 2, L"Framework.B",
                    L"Framework.B_1.0.0.0_x64__contosoid",
                    L"Framework.B_contosoid", L"payloads\\b\\content",
                    1, APPX_CATALOG_ARCHITECTURE_X64 );
    snapshot = create_snapshot( 1, packages, ARRAY_SIZE(packages) );
    if (!snapshot) return;

    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders, ARRAY_SIZE(loaders),
        classes, ARRAY_SIZE(classes), &graph );
    ok( hr == S_OK, "boundary graph creation returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        hr = appx_package_graph_lookup_inproc_class(
            graph, L"Wine.Boundary.First", &match );
        ok( hr == S_OK && match.package_index == 0 &&
            !lstrcmpW( match.path, L"main\\shared.dll" ) &&
            object_id_matches( match.object_id, 0x50607081 ),
            "first boundary lookup returned %#lx, package %u, path %s.\n",
            hr, match.package_index, wine_dbgstr_w(match.path) );
        hr = appx_package_graph_lookup_inproc_class(
            graph, L"Wine.Boundary.Middle", &match );
        ok( hr == S_OK && match.package_index == 1 &&
            !lstrcmpW( match.path, L"a\\SHARED.DLL" ) &&
            object_id_matches( match.object_id, 0x50607082 ),
            "middle boundary lookup returned %#lx, package %u, path %s.\n",
            hr, match.package_index, wine_dbgstr_w(match.path) );
        hr = appx_package_graph_lookup_inproc_class(
            graph, L"Wine.Boundary.Last", &match );
        ok( hr == S_OK && match.package_index == 2 &&
            !lstrcmpW( match.path, L"b\\Shared.dll" ) &&
            object_id_matches( match.object_id, 0x50607083 ),
            "last boundary lookup returned %#lx, package %u, path %s.\n",
            hr, match.package_index, wine_dbgstr_w(match.path) );
    }

    appx_package_graph_free( graph );
    graph = NULL;
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, classes, 1, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "zero-loader class returned %#lx.\n", hr );

    appx_catalog_snapshot_free( snapshot );
}

static void test_resolution_and_architecture( void )
{
    static const struct appx_catalog_dependency dependency =
        {L"Contoso.FrameworkA", publisher, {1, 0, 0, 0}};
    struct appx_catalog_dependency changed_dependency = dependency;
    struct appx_catalog_package packages[5];
    APPX_CATALOG_SNAPSHOT *snapshot;
    APPX_PACKAGE_GRAPH *graph = NULL;
    HRESULT hr;

    make_main( packages, &dependency, 1 );
    make_framework( packages + 1, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_9.0.0.0_x86__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a9x86\\content", 9,
                    APPX_CATALOG_ARCHITECTURE_X86 );
    make_framework( packages + 2, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_4.0.0.0_neutral__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a4neutral\\content", 4,
                    APPX_CATALOG_ARCHITECTURE_NEUTRAL );
    make_framework( packages + 3, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_5.0.0.0_x64__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a5x64\\content", 5,
                    APPX_CATALOG_ARCHITECTURE_X64 );

    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == S_OK, "architecture graph returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
        check_package( graph, 1, packages[3].full_name, 1 );
    appx_package_graph_free( graph );
    graph = NULL;
    appx_catalog_snapshot_free( snapshot );

    snapshot = create_snapshot( 1, packages, 3 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == S_OK, "neutral fallback returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
        check_package( graph, 1, packages[2].full_name, 1 );
    appx_package_graph_free( graph );
    graph = NULL;

    packages[0].architecture = APPX_CATALOG_ARCHITECTURE_X86;
    appx_catalog_snapshot_free( snapshot );
    snapshot = create_snapshot( 1, packages, 3 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X86, 1, NULL, 0, &graph );
    ok( hr == S_OK, "x86 process graph returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
        check_package( graph, 1, packages[1].full_name, 1 );
    appx_package_graph_free( graph );
    graph = NULL;

    packages[0].architecture = APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    appx_catalog_snapshot_free( snapshot );
    snapshot = create_snapshot( 1, packages, 3 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X86, 1, NULL, 0, &graph );
    ok( hr == S_OK, "neutral package x86 process returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
        check_package( graph, 1, packages[1].full_name, 1 );
    appx_package_graph_free( graph );
    graph = NULL;

    packages[0].architecture = APPX_CATALOG_ARCHITECTURE_X64;
    appx_catalog_snapshot_free( snapshot );
    snapshot = create_snapshot( 1, packages, 3 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X86, 1, NULL, 0, &graph );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE),
        "wrong main architecture returned %#lx.\n", hr );

    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_NEUTRAL, 1, NULL, 0, &graph );
    ok( hr == E_INVALIDARG,
        "neutral process target returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    changed_dependency.publisher = L"CN=Other";
    make_main( packages, &changed_dependency, 1 );
    snapshot = create_snapshot( 1, packages, 3 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "publisher mismatch returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    changed_dependency = dependency;
    changed_dependency.min_version.major = 10;
    make_main( packages, &changed_dependency, 1 );
    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "minimum-version failure returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    make_main( packages, &dependency, 1 );
    packages[3].flags &= ~APPX_CATALOG_PACKAGE_SIGNED;
    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "unsigned highest version returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    make_main( packages, &dependency, 1 );
    packages[3].flags = APPX_CATALOG_PACKAGE_ACTIVE |
                        APPX_CATALOG_PACKAGE_SIGNED;
    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "ordinary dependency returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    make_main( packages, &dependency, 1 );
    packages[3].flags = APPX_CATALOG_PACKAGE_ACTIVE |
                        APPX_CATALOG_PACKAGE_RESOURCE |
                        APPX_CATALOG_PACKAGE_SIGNED;
    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "resource dependency returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    make_main( packages, &dependency, 1 );
    make_framework( packages + 3, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_5.0.0.0_x64__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a5x64\\content", 5,
                    APPX_CATALOG_ARCHITECTURE_X64 );
    make_framework( packages + 4, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_5.0.0.0_x64__otherid",
                    L"Contoso.FrameworkA_otherid",
                    L"payloads\\ambiguous\\content", 5,
                    APPX_CATALOG_ARCHITECTURE_X64 );
    snapshot = create_snapshot( 1, packages, 5 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "ambiguous highest version returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );
}

static void check_direct_dependency(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *main_full_name,
    enum appx_catalog_architecture architecture,
    const WCHAR *expected_full_name )
{
    const struct appx_catalog_package *resolved;
    UINT32 package_index = ~0u, count = 0;
    HRESULT hr;

    hr = appx_package_graph_resolve_direct_dependencies(
        snapshot, main_full_name, architecture, 1, &package_index, &count );
    ok( hr == S_OK, "direct resolution returned %#lx.\n", hr );
    ok( count == 1, "direct resolution returned count %u.\n", count );
    if (FAILED(hr) || count != 1) return;

    resolved = appx_catalog_snapshot_get_package( snapshot, package_index );
    ok( !!resolved, "direct resolution returned invalid index %u.\n",
        package_index );
    if (resolved)
        ok( !lstrcmpW( resolved->full_name, expected_full_name ),
            "direct resolution returned %s, expected %s.\n",
            wine_dbgstr_w(resolved->full_name),
            wine_dbgstr_w(expected_full_name) );
}

static void test_direct_dependency_resolution( void )
{
    static const struct appx_catalog_dependency dependency =
        {L"Contoso.FrameworkA", publisher, {1, 0, 0, 0}};
    struct appx_catalog_package packages[5];
    APPX_CATALOG_SNAPSHOT *snapshot;
    UINT32 package_index = ~0u, count;
    HRESULT hr;

    make_main( packages, &dependency, 1 );
    packages[0].architecture = APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    make_framework( packages + 1, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_9.0.0.0_x86__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a9x86\\content", 9,
                    APPX_CATALOG_ARCHITECTURE_X86 );
    make_framework( packages + 2, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_4.0.0.0_neutral__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a4neutral\\content", 4,
                    APPX_CATALOG_ARCHITECTURE_NEUTRAL );
    make_framework( packages + 3, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_5.0.0.0_x64__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a5x64\\content", 5,
                    APPX_CATALOG_ARCHITECTURE_X64 );

    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    count = 0;
    hr = appx_package_graph_resolve_direct_dependencies(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_X64,
        0, NULL, &count );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
        "direct size query returned %#lx.\n", hr );
    ok( count == 1, "direct size query returned count %u.\n", count );

    count = 123;
    hr = appx_package_graph_resolve_direct_dependencies(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_X64,
        1, NULL, &count );
    ok( hr == E_INVALIDARG, "NULL direct output returned %#lx.\n", hr );
    ok( count == 0, "NULL direct output returned count %u.\n", count );

    check_direct_dependency(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_X64,
        packages[3].full_name );
    check_direct_dependency(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_X86,
        packages[1].full_name );

    count = 123;
    hr = appx_package_graph_resolve_direct_dependencies(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_NEUTRAL,
        1, &package_index, &count );
    ok( hr == E_INVALIDARG, "neutral direct target returned %#lx.\n", hr );
    ok( count == 0, "neutral direct target returned count %u.\n", count );
    appx_catalog_snapshot_free( snapshot );

    packages[3].flags &= ~APPX_CATALOG_PACKAGE_SIGNED;
    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    count = 123;
    hr = appx_package_graph_resolve_direct_dependencies(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_X64,
        1, &package_index, &count );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "unsigned direct dependency returned %#lx.\n", hr );
    ok( count == 0, "unsigned direct dependency returned count %u.\n", count );
    appx_catalog_snapshot_free( snapshot );

    packages[3].flags = APPX_CATALOG_PACKAGE_ACTIVE |
                        APPX_CATALOG_PACKAGE_SIGNED;
    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    count = 123;
    hr = appx_package_graph_resolve_direct_dependencies(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_X64,
        1, &package_index, &count );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "ordinary direct dependency returned %#lx.\n", hr );
    ok( count == 0, "ordinary direct dependency returned count %u.\n", count );
    appx_catalog_snapshot_free( snapshot );

    packages[3].flags = APPX_CATALOG_PACKAGE_ACTIVE |
                        APPX_CATALOG_PACKAGE_RESOURCE |
                        APPX_CATALOG_PACKAGE_SIGNED;
    snapshot = create_snapshot( 1, packages, 4 );
    if (!snapshot) return;
    count = 123;
    hr = appx_package_graph_resolve_direct_dependencies(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_X64,
        1, &package_index, &count );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "resource direct dependency returned %#lx.\n", hr );
    ok( count == 0, "resource direct dependency returned count %u.\n", count );
    appx_catalog_snapshot_free( snapshot );

    make_framework( packages + 3, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_5.0.0.0_x64__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a5x64\\content", 5,
                    APPX_CATALOG_ARCHITECTURE_X64 );
    make_framework( packages + 4, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_5.0.0.0_x64__otherid",
                    L"Contoso.FrameworkA_otherid",
                    L"payloads\\ambiguous\\content", 5,
                    APPX_CATALOG_ARCHITECTURE_X64 );
    snapshot = create_snapshot( 1, packages, 5 );
    if (!snapshot) return;
    count = 123;
    hr = appx_package_graph_resolve_direct_dependencies(
        snapshot, packages[0].full_name, APPX_CATALOG_ARCHITECTURE_X64,
        1, &package_index, &count );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "ambiguous direct dependency returned %#lx.\n", hr );
    ok( count == 0, "ambiguous direct dependency returned count %u.\n", count );
    appx_catalog_snapshot_free( snapshot );
}

static void test_missing_duplicates_and_loader_input( void )
{
    static const struct appx_catalog_dependency duplicate_dependencies[] =
    {
        {L"Contoso.FrameworkA", publisher, {1, 0, 0, 0}},
        {L"contoso.frameworka", publisher, {2, 0, 0, 0}},
    };
    static const struct appx_catalog_dependency dependency =
        {L"Contoso.FrameworkA", publisher, {1, 0, 0, 0}};
    static const struct appx_catalog_application duplicate_apps[] =
    {
        {L"App", L"bin\\one.exe", L"", L"", L"",
         APPX_CATALOG_ACTIVATION_FULL_TRUST},
        {L"App", L"bin\\two.exe", L"", L"", L"",
         APPX_CATALOG_ACTIVATION_WIN32},
    };
    struct appx_graph_loader_file loader_files[2] = {{0}};
    struct appx_graph_file_identity invalid_identity;
    struct appx_catalog_package packages[2];
    APPX_CATALOG_SNAPSHOT *snapshot;
    APPX_PACKAGE_GRAPH *graph = NULL;
    WCHAR *long_store = NULL;
    UINT32 i;
    HRESULT hr;

    make_main( packages, &dependency, 1 );
    make_framework( packages + 1, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_1.0.0.0_x64__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a\\content", 1,
                    APPX_CATALOG_ARCHITECTURE_X64 );
    snapshot = create_snapshot( 1, packages, 2 );
    if (!snapshot) return;
    for (i = 0; i < ARRAY_SIZE(loader_files); i++)
    {
        loader_files[i].volume_serial = test_application_identity.volume_serial;
        loader_files[i].file_index_high =
            test_application_identity.file_index_high;
        loader_files[i].file_index_low =
            test_application_identity.file_index_low;
        loader_files[i].change_time = 0x01d0000000000000ULL;
        loader_files[i].file_size = 4096;
        memcpy( loader_files[i].object_id,
                test_application_identity.object_id,
                sizeof(loader_files[i].object_id) );
    }

    hr = p_appx_package_graph_create(
        snapshot, store_root, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, NULL, 0, &graph );
    ok( hr == E_INVALIDARG,
        "NULL application identity returned %#lx.\n", hr );
    memset( &invalid_identity, 0, sizeof(invalid_identity) );
    hr = p_appx_package_graph_create(
        snapshot, store_root, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &invalid_identity,
        NULL, 0, &graph );
    ok( hr == E_INVALIDARG,
        "zero application identity returned %#lx.\n", hr );
    invalid_identity = test_application_identity;
    memset( invalid_identity.object_id, 0, sizeof(invalid_identity.object_id) );
    hr = p_appx_package_graph_create(
        snapshot, store_root, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &invalid_identity,
        NULL, 0, &graph );
    ok( hr == E_INVALIDARG,
        "zero application object token returned %#lx.\n", hr );

    hr = p_appx_package_graph_create(
        snapshot, L"relative\\store", packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &test_application_identity,
        NULL, 0, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "relative store root returned %#lx.\n", hr );
    hr = p_appx_package_graph_create(
        snapshot, L"\\\\server\\share", packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &test_application_identity,
        NULL, 0, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "UNC store root returned %#lx.\n", hr );
    hr = p_appx_package_graph_create(
        snapshot, L"C:\\store\\..\\escape", packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &test_application_identity,
        NULL, 0, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "traversing store root returned %#lx.\n", hr );
    hr = p_appx_package_graph_create(
        snapshot, L"C:/store", packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &test_application_identity,
        NULL, 0, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "forward-slash store root returned %#lx.\n", hr );
    hr = p_appx_package_graph_create(
        snapshot, L"C:\\store\\bad. ", packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &test_application_identity,
        NULL, 0, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "noncanonical store root returned %#lx.\n", hr );
    long_store = HeapAlloc( GetProcessHeap(), 0, 32761 * sizeof(*long_store) );
    ok( !!long_store, "failed to allocate long store root.\n" );
    if (long_store)
    {
        long_store[0] = 'C';
        long_store[1] = ':';
        long_store[2] = '\\';
        for (i = 3; i < 32760; i++)
            long_store[i] = (i - 3) & 1 ? '\\' : 'a';
        long_store[32760] = 0;
        hr = p_appx_package_graph_create(
            snapshot, long_store, packages[0].full_name, L"App",
            APPX_CATALOG_ARCHITECTURE_X64, 1, &test_application_identity,
            NULL, 0, &graph );
        ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
            "combined root overflow returned %#lx.\n", hr );
        HeapFree( GetProcessHeap(), 0, long_store );
    }
    hr = p_appx_package_graph_create(
        snapshot, L"C:\\", packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &test_application_identity,
        NULL, 0, &graph );
    ok( hr == S_OK, "drive store root returned %#lx.\n", hr );
    appx_package_graph_free( graph );
    graph = NULL;

    hr = appx_package_graph_create(
        snapshot, L"Missing_1.0.0.0_x64__id", L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_NOT_FOUND),
        "missing main returned %#lx.\n", hr );
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"Missing",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_NOT_FOUND),
        "missing application returned %#lx.\n", hr );

    loader_files[0].package_full_name = packages[0].full_name;
    loader_files[0].relative_path = L"test.dll";
    loader_files[0].volume_serial = 0;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 1, &graph );
    ok( hr == S_OK, "zero loader volume serial returned %#lx.\n", hr );
    appx_package_graph_free( graph );
    graph = NULL;
    loader_files[0].volume_serial = test_application_identity.volume_serial;
    loader_files[0].file_index_low = 0;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 1, &graph );
    ok( hr == E_INVALIDARG,
        "zero loader file identity returned %#lx.\n", hr );
    loader_files[0].file_index_low =
        test_application_identity.file_index_low;
    memset( loader_files[0].object_id, 0,
            sizeof(loader_files[0].object_id) );
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 1, &graph );
    ok( hr == E_INVALIDARG,
        "zero loader object token returned %#lx.\n", hr );
    memcpy( loader_files[0].object_id,
            test_application_identity.object_id,
            sizeof(loader_files[0].object_id) );
    loader_files[0].change_time = 0;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 1, &graph );
    ok( hr == E_INVALIDARG,
        "zero loader change time returned %#lx.\n", hr );
    loader_files[0].change_time = 0x01d0000000000000ULL;
    loader_files[0].file_size = 0;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 1, &graph );
    ok( hr == E_INVALIDARG, "zero loader file size returned %#lx.\n", hr );
    loader_files[0].file_size = 4096;

    loader_files[0].package_full_name = L"Missing_1.0.0.0_x64__id";
    loader_files[0].relative_path = L"test.dll";
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 1, &graph );
    ok( hr == E_INVALIDARG, "unknown loader package returned %#lx.\n", hr );
    loader_files[0].package_full_name = packages[0].full_name;
    loader_files[0].relative_path = L"..\\test.dll";
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 1, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "traversal loader path returned %#lx.\n", hr );
    loader_files[0].relative_path = L"one\\same.dll";
    loader_files[1].package_full_name = packages[0].full_name;
    loader_files[1].relative_path = L"two\\SAME.DLL";
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 2, &graph );
    ok( hr == S_OK, "duplicate package basename returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        struct appx_graph_loader_match match;

        hr = appx_package_graph_lookup_basename(
            graph, L"same.dll", &match );
        ok( hr == S_OK && !lstrcmpW( match.relative_path, L"one\\same.dll" ),
            "ordered duplicate lookup returned %#lx, path %s.\n",
            hr, wine_dbgstr_w(match.relative_path) );
        appx_package_graph_free( graph );
        graph = NULL;
    }
    loader_files[1].relative_path = L"ONE\\same.dll";
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 2, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "duplicate package path returned %#lx.\n", hr );
    loader_files[1].relative_path = L"two\\SAME.DLL";
    loader_files[1].search_rank =
        WINE_APPX_GRAPH_MAX_LOADER_SEARCH_PATHS;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loader_files, 2, &graph );
    ok( hr == E_INVALIDARG, "invalid loader rank returned %#lx.\n", hr );
    loader_files[1].search_rank = 0;
    appx_catalog_snapshot_free( snapshot );

    make_main( packages, duplicate_dependencies,
               ARRAY_SIZE(duplicate_dependencies) );
    snapshot = create_snapshot( 1, packages, 2 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "duplicate dependencies returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    make_main( packages, &dependency, 1 );
    packages[0].application_count = ARRAY_SIZE(duplicate_apps);
    packages[0].applications = duplicate_apps;
    snapshot = create_snapshot( 1, packages, 2 );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "duplicate applications returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    hr = appx_package_graph_create(
        NULL, L"x", L"App", APPX_CATALOG_ARCHITECTURE_X64,
        1, NULL, 0, &graph );
    ok( hr == E_INVALIDARG, "null snapshot returned %#lx.\n", hr );
    hr = appx_package_graph_create(
        (void *)1, L"x", L"App", 99, 1, NULL, 0, &graph );
    ok( hr == E_INVALIDARG, "invalid architecture returned %#lx.\n", hr );
    hr = appx_package_graph_create(
        (void *)1, L"x", L"App", APPX_CATALOG_ARCHITECTURE_X64,
        1, NULL, APPX_GRAPH_MAX_LOADER_FILES + 1, &graph );
    ok( hr == E_INVALIDARG, "loader count limit returned %#lx.\n", hr );
}

static void test_cycles_diamond_and_order( void )
{
    static const struct appx_catalog_dependency main_dependencies[] =
    {
        {L"Framework.B", publisher, {1, 0, 0, 0}},
        {L"Framework.A", publisher, {1, 0, 0, 0}},
    };
    static const struct appx_graph_loader_file collision_loaders[] =
    {
        {L"Framework.A_1.0.0.0_x64__contosoid", L"lib\\collision.dll",
         0x10203040, 0, 0x50607081, 0, TEST_FILE_STAMP,
         TEST_OBJECT_ID(0x50607081)},
        {L"Framework.B_1.0.0.0_x64__contosoid", L"collision.dll",
         0x10203040, 0, 0x50607082, 0, TEST_FILE_STAMP,
         TEST_OBJECT_ID(0x50607082)},
    };
    static const struct appx_catalog_dependency dependency_c =
        {L"Framework.C", publisher, {1, 0, 0, 0}};
    static const struct appx_catalog_dependency dependency_a =
        {L"Framework.A", publisher, {1, 0, 0, 0}};
    static const struct appx_catalog_dependency dependency_b =
        {L"Framework.B", publisher, {1, 0, 0, 0}};
    struct appx_catalog_package packages[4];
    APPX_CATALOG_SNAPSHOT *snapshot;
    struct appx_graph_loader_match match;
    APPX_PACKAGE_GRAPH *graph = NULL;
    HRESULT hr;

    make_main( packages, main_dependencies, ARRAY_SIZE(main_dependencies) );
    make_framework( packages + 1, L"Framework.A",
                    L"Framework.A_1.0.0.0_x64__contosoid",
                    L"Framework.A_contosoid", L"payloads\\a\\content",
                    1, APPX_CATALOG_ARCHITECTURE_X64 );
    make_framework( packages + 2, L"Framework.B",
                    L"Framework.B_1.0.0.0_x64__contosoid",
                    L"Framework.B_contosoid", L"payloads\\b\\content",
                    1, APPX_CATALOG_ARCHITECTURE_X64 );
    make_framework( packages + 3, L"Framework.C",
                    L"Framework.C_1.0.0.0_x64__contosoid",
                    L"Framework.C_contosoid", L"payloads\\c\\content",
                    1, APPX_CATALOG_ARCHITECTURE_X64 );
    packages[1].dependency_count = 1;
    packages[1].dependencies = &dependency_c;
    packages[2].dependency_count = 1;
    packages[2].dependencies = &dependency_c;

    snapshot = create_snapshot( 1, packages, ARRAY_SIZE(packages) );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, collision_loaders,
        ARRAY_SIZE(collision_loaders), &graph );
    ok( hr == S_OK, "diamond graph returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        struct appx_graph_package package;

        ok( appx_package_graph_get_package_count( graph ) == 4,
            "diamond graph got %u packages.\n",
            appx_package_graph_get_package_count(graph) );
        check_package( graph, 0, packages[0].full_name, 0 );
        check_package( graph, 1, packages[2].full_name, 1 );
        check_package( graph, 2, packages[1].full_name, 2 );
        check_package( graph, 3, packages[3].full_name, 3 );
        hr = appx_package_graph_lookup_basename(
            graph, L"collision.dll", &match );
        ok( hr == S_OK && match.package_index == 1 &&
            !lstrcmpW( match.relative_path, L"collision.dll" ),
            "declaration-order collision returned %#lx, package %u, path %s.\n",
            hr, match.package_index, wine_dbgstr_w(match.relative_path) );
        hr = appx_package_graph_get_package( graph, 1, &package );
        ok( hr == S_OK && (package.flags & APPX_GRAPH_PACKAGE_DIRECT),
            "direct package B flags %#x, hr %#lx.\n", package.flags, hr );
        hr = appx_package_graph_get_package( graph, 2, &package );
        ok( hr == S_OK && (package.flags & APPX_GRAPH_PACKAGE_DIRECT),
            "direct package A flags %#x, hr %#lx.\n", package.flags, hr );
        hr = appx_package_graph_get_package( graph, 3, &package );
        ok( hr == S_OK && !(package.flags & APPX_GRAPH_PACKAGE_DIRECT),
            "transitive package C flags %#x, hr %#lx.\n", package.flags, hr );
    }
    appx_package_graph_free( graph );
    graph = NULL;
    appx_catalog_snapshot_free( snapshot );

    packages[3].dependency_count = 1;
    packages[3].dependencies = &dependency_a;
    snapshot = create_snapshot( 1, packages, ARRAY_SIZE(packages) );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "indirect cycle returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    packages[1].dependency_count = 1;
    packages[1].dependencies = &dependency_a;
    packages[2].dependency_count = 0;
    packages[2].dependencies = NULL;
    packages[3].dependency_count = 0;
    packages[3].dependencies = NULL;
    snapshot = create_snapshot( 1, packages, ARRAY_SIZE(packages) );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "self cycle returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );

    packages[1].dependency_count = 1;
    packages[1].dependencies = &dependency_b;
    packages[2].dependency_count = 1;
    packages[2].dependencies = &dependency_a;
    snapshot = create_snapshot( 1, packages, ARRAY_SIZE(packages) );
    if (!snapshot) return;
    hr = appx_package_graph_create(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
    ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
        "two-node cycle returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );
}

struct limit_package_storage
{
    WCHAR name[32];
    WCHAR full_name[96];
    WCHAR family_name[64];
    WCHAR root[64];
};

static void test_limits_and_overflow( void )
{
    struct limit_package_storage *storage;
    struct appx_catalog_dependency *dependencies;
    struct appx_catalog_package *packages;
    struct appx_catalog_application application = main_application;
    APPX_CATALOG_SNAPSHOT *snapshot;
    APPX_PACKAGE_GRAPH *graph = NULL;
    WCHAR *long_family = NULL, *long_id = NULL;
    UINT32 i;
    HRESULT hr;

    storage = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                         257 * sizeof(*storage) );
    dependencies = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                              256 * sizeof(*dependencies) );
    packages = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                          257 * sizeof(*packages) );
    ok( !!storage && !!dependencies && !!packages,
        "failed to allocate limit fixtures.\n" );
    if (!storage || !dependencies || !packages) goto done;

    make_main( packages, dependencies, 255 );
    for (i = 0; i < 256; i++)
    {
        _snwprintf( storage[i + 1].name,
                    ARRAY_SIZE(storage[i + 1].name),
                    L"Framework.%03u", i );
        _snwprintf( storage[i + 1].full_name,
                    ARRAY_SIZE(storage[i + 1].full_name),
                    L"Framework.%03u_1.0.0.0_x64__contosoid", i );
        _snwprintf( storage[i + 1].family_name,
                    ARRAY_SIZE(storage[i + 1].family_name),
                    L"Framework.%03u_contosoid", i );
        _snwprintf( storage[i + 1].root,
                    ARRAY_SIZE(storage[i + 1].root),
                    L"payloads\\f%03u\\content", i );
        dependencies[i].name = storage[i + 1].name;
        dependencies[i].publisher = publisher;
        dependencies[i].min_version.major = 1;
        make_framework( packages + i + 1, storage[i + 1].name,
                        storage[i + 1].full_name,
                        storage[i + 1].family_name, storage[i + 1].root,
                        1, APPX_CATALOG_ARCHITECTURE_X64 );
    }

    snapshot = create_snapshot( 1, packages, 256 );
    if (snapshot)
    {
        hr = appx_package_graph_create(
            snapshot, packages[0].full_name, L"App",
            APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
        ok( hr == S_OK, "maximum-size graph returned %#lx.\n", hr );
        if (SUCCEEDED(hr))
            ok( appx_package_graph_get_package_count( graph ) == 256,
                "maximum-size graph got %u packages.\n",
                appx_package_graph_get_package_count(graph) );
        appx_package_graph_free( graph );
        graph = NULL;
        appx_catalog_snapshot_free( snapshot );
    }

    packages[0].dependency_count = 256;
    snapshot = create_snapshot( 1, packages, 257 );
    if (snapshot)
    {
        hr = appx_package_graph_create(
            snapshot, packages[0].full_name, L"App",
            APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
        ok( hr == APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED,
            "package-count overflow returned %#lx.\n", hr );
        appx_catalog_snapshot_free( snapshot );
    }

    long_family = HeapAlloc( GetProcessHeap(), 0, 20001 * sizeof(*long_family) );
    long_id = HeapAlloc( GetProcessHeap(), 0, 20001 * sizeof(*long_id) );
    ok( !!long_family && !!long_id, "failed to allocate long identities.\n" );
    if (!long_family || !long_id) goto done;
    for (i = 0; i < 20000; i++)
    {
        long_family[i] = 'F';
        long_id[i] = 'A';
    }
    long_family[20000] = 0;
    long_id[20000] = 0;
    make_main( packages, NULL, 0 );
    packages[0].family_name = long_family;
    application.id = long_id;
    packages[0].applications = &application;
    snapshot = create_snapshot( 1, packages, 1 );
    if (snapshot)
    {
        hr = appx_package_graph_create(
            snapshot, packages[0].full_name, long_id,
            APPX_CATALOG_ARCHITECTURE_X64, 1, NULL, 0, &graph );
        ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
            "AUMID overflow returned %#lx.\n", hr );
        appx_catalog_snapshot_free( snapshot );
    }

done:
    HeapFree( GetProcessHeap(), 0, long_id );
    HeapFree( GetProcessHeap(), 0, long_family );
    HeapFree( GetProcessHeap(), 0, packages );
    HeapFree( GetProcessHeap(), 0, dependencies );
    HeapFree( GetProcessHeap(), 0, storage );
}

static UINT32 read_u32( const BYTE *data )
{
    return data[0] | ((UINT32)data[1] << 8) |
           ((UINT32)data[2] << 16) | ((UINT32)data[3] << 24);
}

static void put_u16( BYTE *data, UINT16 value )
{
    data[0] = value;
    data[1] = value >> 8;
}

static void put_u32( BYTE *data, UINT32 value )
{
    put_u16( data, value );
    put_u16( data + 2, value >> 16 );
}

static void put_u64( BYTE *data, UINT64 value )
{
    put_u32( data, value );
    put_u32( data + 4, value >> 32 );
}

static void expect_corrupt( BYTE *copy, const BYTE *valid, UINT32 size,
                            const char *description )
{
    APPX_PACKAGE_GRAPH *graph = NULL;
    HRESULT hr = appx_package_graph_validate_blob( copy, size );

    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "%s validation returned %#lx.\n", description, hr );
    hr = appx_package_graph_from_blob( copy, size, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "%s import returned %#lx.\n", description, hr );
    ok( !graph, "%s returned graph %p.\n", description, graph );
    memcpy( copy, valid, size );
}

static void test_corrupt_blob( void )
{
    static const struct appx_catalog_dependency dependency =
        {L"Contoso.FrameworkA", publisher, {1, 0, 0, 0}};
    static const struct appx_graph_loader_file loaders[] =
    {
        {L"Contoso.Main_1.0.0.0_x64__contosoid", L"bin\\a.dll",
         TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
        {L"Contoso.FrameworkA_1.0.0.0_x64__contosoid", L"lib\\b.dll",
         TEST_FILE_IDENTITY, 0, TEST_FILE_STAMP, TEST_FILE_OBJECT_ID},
    };
    static const struct appx_graph_inproc_class classes[] =
    {
        {L"Contoso.Main_1.0.0.0_x64__contosoid",
         L"Wine.Class.A", L"bin\\a.dll", 2, TEST_FILE_IDENTITY,
         TEST_FILE_STAMP},
        {L"Contoso.FrameworkA_1.0.0.0_x64__contosoid",
         L"Wine.Class.B", L"lib\\b.dll", 0, TEST_FILE_IDENTITY,
         TEST_FILE_STAMP},
    };
    static const struct appx_graph_inproc_class duplicate_classes[] =
    {
        {L"Contoso.Main_1.0.0.0_x64__contosoid",
         L"Wine.Class.A", L"bin\\a.dll", 2, TEST_FILE_IDENTITY,
         TEST_FILE_STAMP},
        {L"Contoso.FrameworkA_1.0.0.0_x64__contosoid",
         L"wine.class.a", L"lib\\b.dll", 0, TEST_FILE_IDENTITY,
         TEST_FILE_STAMP},
    };
    struct appx_graph_inproc_class invalid_class;
    struct appx_graph_loader_file zero_serial_loader;
    struct appx_catalog_package packages[2];
    APPX_CATALOG_SNAPSHOT *snapshot;
    APPX_PACKAGE_GRAPH *graph = NULL;
    const BYTE *valid;
    BYTE *copy = NULL, *extended = NULL;
    UINT32 size, strings_offset, loader_offset, class_offset, package_count;
    UINT32 string_ref, second_string_ref, original;
    HRESULT hr;

    make_main( packages, &dependency, 1 );
    make_framework( packages + 1, L"Contoso.FrameworkA",
                    L"Contoso.FrameworkA_1.0.0.0_x64__contosoid",
                    L"Contoso.FrameworkA_contosoid",
                    L"payloads\\a\\content", 1,
                    APPX_CATALOG_ARCHITECTURE_X64 );
    snapshot = create_snapshot( 1, packages, 2 );
    if (!snapshot) return;
    invalid_class = classes[0];
    invalid_class.volume_serial = 0;
    zero_serial_loader = loaders[0];
    zero_serial_loader.volume_serial = 0;
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, &zero_serial_loader,
        1, &invalid_class, 1, &graph );
    ok( hr == S_OK, "zero class volume serial returned %#lx.\n", hr );
    appx_package_graph_free( graph );
    graph = NULL;
    invalid_class = classes[0];
    invalid_class.file_index_low = 0;
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), &invalid_class, 1, &graph );
    ok( hr == E_INVALIDARG,
        "zero class file identity returned %#lx.\n", hr );
    invalid_class = classes[0];
    invalid_class.change_time = 0;
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), &invalid_class, 1, &graph );
    ok( hr == E_INVALIDARG, "zero class change time returned %#lx.\n", hr );
    invalid_class = classes[0];
    invalid_class.file_size = 0;
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), &invalid_class, 1, &graph );
    ok( hr == E_INVALIDARG, "zero class file size returned %#lx.\n", hr );
    invalid_class = classes[0];
    invalid_class.change_time++;
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), &invalid_class, 1, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "mismatched class stamp returned %#lx.\n", hr );
    invalid_class = classes[0];
    invalid_class.threading_model = 3;
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), &invalid_class, 1, &graph );
    ok( hr == E_INVALIDARG, "invalid class threading returned %#lx.\n", hr );
    invalid_class = classes[0];
    invalid_class.package_full_name = L"Missing_1.0.0.0_x64__contosoid";
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), &invalid_class, 1, &graph );
    ok( hr == E_INVALIDARG, "unknown class package returned %#lx.\n", hr );
    invalid_class = classes[0];
    invalid_class.path = L"..\\escape.dll";
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), &invalid_class, 1, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "invalid class path returned %#lx.\n", hr );
    invalid_class = classes[0];
    invalid_class.path = L"bin\\missing.dll";
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), &invalid_class, 1, &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "class outside loader inventory returned %#lx.\n", hr );
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), duplicate_classes,
        ARRAY_SIZE(duplicate_classes), &graph );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "ASCII case-fold duplicate classes returned %#lx.\n", hr );
    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), (void *)1, APPX_GRAPH_MAX_CLASSES + 1,
        &graph );
    ok( hr == E_INVALIDARG, "class count limit returned %#lx.\n", hr );

    hr = appx_package_graph_create_with_classes(
        snapshot, packages[0].full_name, L"App",
        APPX_CATALOG_ARCHITECTURE_X64, 1, loaders,
        ARRAY_SIZE(loaders), classes, ARRAY_SIZE(classes), &graph );
    ok( hr == S_OK, "corruption fixture returned %#lx.\n", hr );
    appx_catalog_snapshot_free( snapshot );
    if (FAILED(hr)) return;
    valid = appx_package_graph_get_blob( graph, &size );
    copy = HeapAlloc( GetProcessHeap(), 0, size );
    ok( !!copy, "failed to allocate corrupt blob copy.\n" );
    if (!copy) goto done;
    memcpy( copy, valid, size );

    hr = appx_package_graph_validate_blob( NULL, size );
    ok( hr == E_INVALIDARG, "null blob returned %#lx.\n", hr );
    hr = appx_package_graph_validate_blob( valid, size - 1 );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "truncated blob returned %#lx.\n", hr );

    copy[0] ^= 0xff;
    expect_corrupt( copy, valid, size, "magic" );
    put_u32( copy + GRAPH_HEADER_TOTAL_SIZE_OFFSET, size + 1 );
    expect_corrupt( copy, valid, size, "total size" );
    put_u32( copy + GRAPH_HEADER_PACKAGE_COUNT_OFFSET,
             APPX_GRAPH_MAX_PACKAGES + 1 );
    expect_corrupt( copy, valid, size, "package count" );
    put_u32( copy + GRAPH_HEADER_PACKAGES_OFFSET,
             APPX_GRAPH_BLOB_HEADER_SIZE + 2 );
    expect_corrupt( copy, valid, size, "package array offset" );
    put_u32( copy + GRAPH_HEADER_TARGET_ARCHITECTURE_OFFSET,
             APPX_CATALOG_ARCHITECTURE_NEUTRAL );
    expect_corrupt( copy, valid, size, "neutral process target" );
    put_u32( copy + GRAPH_HEADER_VOLUME_SERIAL_OFFSET, 0 );
    hr = appx_package_graph_validate_blob( copy, size );
    ok( hr == S_OK, "zero application volume serial returned %#lx.\n", hr );
    memcpy( copy, valid, size );
    put_u32( copy + GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET, 0 );
    put_u32( copy + GRAPH_HEADER_FILE_INDEX_LOW_OFFSET, 0 );
    expect_corrupt( copy, valid, size, "zero application file identity" );
    memset( copy + GRAPH_HEADER_OBJECT_ID_OFFSET, 0,
            WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    expect_corrupt( copy, valid, size, "zero application object token" );
    copy[GRAPH_HEADER_RESERVED_OFFSET] = 1;
    expect_corrupt( copy, valid, size, "reserved header" );

    strings_offset = read_u32( valid + GRAPH_HEADER_STRINGS_OFFSET );
    put_u32( copy + GRAPH_HEADER_APPLICATION_ID_REF_OFFSET,
             strings_offset + 2 );
    expect_corrupt( copy, valid, size, "out-of-order application string" );
    put_u32( copy + GRAPH_HEADER_APPLICATION_ID_REF_OFFSET + 4, 0 );
    expect_corrupt( copy, valid, size, "zero application length" );
    string_ref = read_u32( valid + GRAPH_HEADER_AUMID_REF_OFFSET );
    copy[string_ref] ^= 1;
    expect_corrupt( copy, valid, size, "mismatched AUMID" );

    put_u32( copy + APPX_GRAPH_BLOB_HEADER_SIZE +
             GRAPH_PACKAGE_RANK_OFFSET, 1 );
    expect_corrupt( copy, valid, size, "package rank" );
    put_u32( copy + APPX_GRAPH_BLOB_HEADER_SIZE +
             GRAPH_PACKAGE_ARCHITECTURE_OFFSET, 99 );
    expect_corrupt( copy, valid, size, "package architecture" );
    original = read_u32( valid + APPX_GRAPH_BLOB_HEADER_SIZE +
                         GRAPH_PACKAGE_FLAGS_OFFSET );
    put_u32( copy + APPX_GRAPH_BLOB_HEADER_SIZE +
             GRAPH_PACKAGE_FLAGS_OFFSET,
             original & ~APPX_CATALOG_PACKAGE_SIGNED );
    expect_corrupt( copy, valid, size, "unsigned package" );
    put_u32( copy + APPX_GRAPH_BLOB_HEADER_SIZE +
             GRAPH_PACKAGE_FLAGS_OFFSET,
             original | APPX_GRAPH_PACKAGE_DIRECT );
    expect_corrupt( copy, valid, size, "direct main package" );
    string_ref = read_u32( valid + APPX_GRAPH_BLOB_HEADER_SIZE +
                           GRAPH_PACKAGE_ROOT_REF_OFFSET );
    put_u16( copy + string_ref, '\\' );
    expect_corrupt( copy, valid, size, "absolute package root" );

    package_count = read_u32( valid + GRAPH_HEADER_PACKAGE_COUNT_OFFSET );
    loader_offset = read_u32( valid + GRAPH_HEADER_LOADERS_OFFSET );
    put_u32( copy + loader_offset + GRAPH_LOADER_PACKAGE_INDEX_OFFSET,
             package_count );
    expect_corrupt( copy, valid, size, "loader package index" );
    put_u32( copy + loader_offset + GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET, 0 );
    put_u32( copy + loader_offset + GRAPH_LOADER_FILE_INDEX_LOW_OFFSET, 0 );
    expect_corrupt( copy, valid, size, "zero loader file identity" );
    memset( copy + loader_offset + GRAPH_LOADER_OBJECT_ID_OFFSET, 0,
            WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    expect_corrupt( copy, valid, size, "zero loader object token" );
    put_u32( copy + loader_offset + GRAPH_LOADER_RESERVED_OFFSET, 1 );
    expect_corrupt( copy, valid, size, "loader reserved data" );
    put_u64( copy + loader_offset + GRAPH_LOADER_CHANGE_TIME_OFFSET, 0 );
    expect_corrupt( copy, valid, size, "zero loader change time" );
    put_u64( copy + loader_offset + GRAPH_LOADER_CHANGE_TIME_OFFSET,
             1ULL << 63 );
    expect_corrupt( copy, valid, size, "negative loader change time" );
    put_u64( copy + loader_offset + GRAPH_LOADER_FILE_SIZE_OFFSET, 0 );
    expect_corrupt( copy, valid, size, "zero loader file size" );
    string_ref = read_u32(
        valid + loader_offset + APPX_GRAPH_BLOB_LOADER_RECORD_SIZE +
        GRAPH_LOADER_BASENAME_REF_OFFSET );
    put_u16( copy + string_ref, 'a' );
    string_ref = read_u32(
        valid + loader_offset + APPX_GRAPH_BLOB_LOADER_RECORD_SIZE +
        GRAPH_LOADER_PATH_REF_OFFSET );
    put_u16( copy + string_ref +
             (read_u32(valid + loader_offset +
                       APPX_GRAPH_BLOB_LOADER_RECORD_SIZE +
                       GRAPH_LOADER_PATH_REF_OFFSET + 4) - 6) *
             sizeof(WCHAR), 'a' );
    put_u32( copy + loader_offset + APPX_GRAPH_BLOB_LOADER_RECORD_SIZE +
             GRAPH_LOADER_PACKAGE_INDEX_OFFSET, 0 );
    expect_corrupt( copy, valid, size, "duplicate loader basename" );

    class_offset = read_u32( valid + GRAPH_HEADER_CLASSES_OFFSET );
    put_u32( copy + class_offset + GRAPH_CLASS_PACKAGE_INDEX_OFFSET,
             package_count );
    expect_corrupt( copy, valid, size, "class package index" );
    put_u32( copy + class_offset + GRAPH_CLASS_THREADING_MODEL_OFFSET, 3 );
    expect_corrupt( copy, valid, size, "class threading model" );
    put_u32( copy + class_offset + GRAPH_CLASS_FILE_INDEX_HIGH_OFFSET, 0 );
    put_u32( copy + class_offset + GRAPH_CLASS_FILE_INDEX_LOW_OFFSET, 0 );
    expect_corrupt( copy, valid, size, "zero class file identity" );
    put_u32( copy + class_offset + GRAPH_CLASS_LOADER_INDEX_OFFSET,
             read_u32(valid + GRAPH_HEADER_LOADER_COUNT_OFFSET) );
    expect_corrupt( copy, valid, size, "class loader index" );
    put_u64( copy + class_offset + GRAPH_CLASS_CHANGE_TIME_OFFSET, 0 );
    expect_corrupt( copy, valid, size, "zero class change time" );
    put_u64( copy + class_offset + GRAPH_CLASS_CHANGE_TIME_OFFSET,
             wine_appx_graph_read_u64(
                 valid + class_offset + GRAPH_CLASS_CHANGE_TIME_OFFSET ) + 1 );
    expect_corrupt( copy, valid, size, "mismatched class change time" );
    put_u64( copy + class_offset + GRAPH_CLASS_FILE_SIZE_OFFSET, 0 );
    expect_corrupt( copy, valid, size, "zero class file size" );
    put_u32( copy + GRAPH_HEADER_CLASS_COUNT_OFFSET,
             APPX_GRAPH_MAX_CLASSES + 1 );
    expect_corrupt( copy, valid, size, "class count" );
    put_u32( copy + GRAPH_HEADER_CLASSES_OFFSET, class_offset + 2 );
    expect_corrupt( copy, valid, size, "class array offset" );
    string_ref = read_u32( valid + class_offset + GRAPH_CLASS_ID_REF_OFFSET );
    second_string_ref = read_u32(
        valid + class_offset + APPX_GRAPH_BLOB_CLASS_RECORD_SIZE +
        GRAPH_CLASS_ID_REF_OFFSET );
    put_u16( copy + string_ref +
             (read_u32(valid + class_offset + GRAPH_CLASS_ID_REF_OFFSET + 4) -
              2) * sizeof(WCHAR), 'C' );
    expect_corrupt( copy, valid, size, "descending class identifier" );
    put_u16( copy + string_ref, 'w' );
    memcpy( copy + string_ref + sizeof(WCHAR),
            valid + second_string_ref + sizeof(WCHAR),
            (read_u32(valid + class_offset + GRAPH_CLASS_ID_REF_OFFSET + 4) -
             2) * sizeof(WCHAR) );
    expect_corrupt( copy, valid, size, "case-fold duplicate class identifier" );
    string_ref = read_u32( valid + class_offset + GRAPH_CLASS_PATH_REF_OFFSET );
    put_u16( copy + string_ref, '\\' );
    expect_corrupt( copy, valid, size, "absolute class path" );

    extended = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2 );
    ok( !!extended, "failed to allocate extended blob.\n" );
    if (extended)
    {
        memcpy( extended, valid, size );
        put_u32( extended + GRAPH_HEADER_TOTAL_SIZE_OFFSET, size + 2 );
        put_u32( extended + GRAPH_HEADER_STRINGS_SIZE_OFFSET,
                 read_u32(valid + GRAPH_HEADER_STRINGS_SIZE_OFFSET) + 2 );
        hr = appx_package_graph_validate_blob( extended, size + 2 );
        ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
            "hidden trailing bytes returned %#lx.\n", hr );
    }

done:
    HeapFree( GetProcessHeap(), 0, extended );
    HeapFree( GetProcessHeap(), 0, copy );
    appx_package_graph_free( graph );
}

static BOOL load_functions( void )
{
    HMODULE module = LoadLibraryA( "appxsvc.dll" );

    ok( !!module, "failed to load appxsvc.dll, error %lu.\n", GetLastError() );
    if (!module) return FALSE;
#define LOAD(name) do \
    { \
        p_##name = (void *)GetProcAddress( module, #name ); \
        ok( !!p_##name, "missing appxsvc export %s, error %lu.\n", \
            #name, GetLastError() ); \
    } while (0)
    LOAD(appx_catalog_snapshot_create);
    LOAD(appx_catalog_snapshot_free);
    LOAD(appx_catalog_snapshot_get_package);
    LOAD(appx_package_graph_create);
    LOAD(appx_package_graph_create_with_classes);
    LOAD(appx_package_graph_resolve_direct_dependencies);
    LOAD(appx_package_graph_from_blob);
    LOAD(appx_package_graph_clone);
    LOAD(appx_package_graph_validate_blob);
    LOAD(appx_package_graph_free);
    LOAD(appx_package_graph_get_blob);
    LOAD(appx_package_graph_get_epoch);
    LOAD(appx_package_graph_get_revision);
    LOAD(appx_package_graph_get_target_architecture);
    LOAD(appx_package_graph_get_package_count);
    LOAD(appx_package_graph_get_package);
    LOAD(appx_package_graph_get_application);
    LOAD(appx_package_graph_lookup_basename);
    LOAD(appx_package_graph_get_inproc_class_count);
    LOAD(appx_package_graph_get_inproc_class);
    LOAD(appx_package_graph_lookup_inproc_class);
#undef LOAD
    return p_appx_catalog_snapshot_create && p_appx_catalog_snapshot_free &&
           p_appx_catalog_snapshot_get_package &&
           p_appx_package_graph_create &&
           p_appx_package_graph_create_with_classes &&
           p_appx_package_graph_resolve_direct_dependencies &&
           p_appx_package_graph_from_blob &&
           p_appx_package_graph_clone && p_appx_package_graph_validate_blob &&
           p_appx_package_graph_free && p_appx_package_graph_get_blob &&
           p_appx_package_graph_get_epoch &&
           p_appx_package_graph_get_revision &&
           p_appx_package_graph_get_target_architecture &&
           p_appx_package_graph_get_package_count &&
           p_appx_package_graph_get_package &&
           p_appx_package_graph_get_application &&
           p_appx_package_graph_lookup_basename &&
           p_appx_package_graph_get_inproc_class_count &&
           p_appx_package_graph_get_inproc_class &&
           p_appx_package_graph_lookup_inproc_class;
}

START_TEST(graph)
{
    if (!load_functions())
    {
        win_skip( "packaged process graph exports are unavailable.\n" );
        return;
    }
    test_basic_graph();
    test_loader_search_order();
    test_class_loader_search_boundaries();
    test_resolution_and_architecture();
    test_direct_dependency_resolution();
    test_missing_duplicates_and_loader_input();
    test_cycles_diamond_and_order();
    test_limits_and_overflow();
    test_corrupt_blob();
}
