/*
 * Test-local AppX deployment backend executor
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#define APPX_DEPLOYMENT_TESTING
#include "../bundle.h"
#include "../deployment.h"

struct deployment_test_backend_imports
{
    HRESULT (WINAPI *catalog_load_bounded)(
        const WCHAR *, DWORD, HANDLE, APPX_CATALOG_SNAPSHOT ** );
    HRESULT (WINAPI *catalog_publish_bounded)(
        const WCHAR *, UINT64, const APPX_CATALOG_SNAPSHOT *, DWORD, HANDLE );
    HRESULT (WINAPI *catalog_snapshot_create)(
        UINT64, const struct appx_catalog_package *, UINT32,
        APPX_CATALOG_SNAPSHOT ** );
    HRESULT (WINAPI *catalog_snapshot_deep_copy)(
        const APPX_CATALOG_SNAPSHOT *, APPX_CATALOG_SNAPSHOT ** );
    void (WINAPI *catalog_snapshot_free)( APPX_CATALOG_SNAPSHOT * );
    UINT64 (WINAPI *catalog_snapshot_get_epoch)(
        const APPX_CATALOG_SNAPSHOT * );
    UINT32 (WINAPI *catalog_snapshot_get_package_count)(
        const APPX_CATALOG_SNAPSHOT * );
    const struct appx_catalog_package *(WINAPI *catalog_snapshot_get_package)(
        const APPX_CATALOG_SNAPSHOT *, UINT32 );
    HRESULT (WINAPI *graph_create)(
        const APPX_CATALOG_SNAPSHOT *, const WCHAR *, const WCHAR *,
        const WCHAR *, enum appx_catalog_architecture, UINT64,
        const struct appx_graph_file_identity *,
        const struct appx_graph_loader_file *, UINT32,
        APPX_PACKAGE_GRAPH ** );
    HRESULT (WINAPI *graph_create_with_classes)(
        const APPX_CATALOG_SNAPSHOT *, const WCHAR *, const WCHAR *,
        const WCHAR *, enum appx_catalog_architecture, UINT64,
        const struct appx_graph_file_identity *,
        const struct appx_graph_loader_file *, UINT32,
        const struct appx_graph_inproc_class *, UINT32,
        APPX_PACKAGE_GRAPH ** );
    void (WINAPI *graph_free)( APPX_PACKAGE_GRAPH * );
    const BYTE *(WINAPI *graph_get_blob)(
        const APPX_PACKAGE_GRAPH *, UINT32 * );
    UINT32 (WINAPI *graph_get_package_count)(
        const APPX_PACKAGE_GRAPH * );
    HRESULT (WINAPI *graph_get_package)(
        const APPX_PACKAGE_GRAPH *, UINT32, struct appx_graph_package * );
};

static struct deployment_test_backend_imports imports;

void deployment_test_backend_set_imports(
    const struct deployment_test_backend_imports *input )
{
    imports = *input;
}

static HRESULT WINAPI test_catalog_load_bounded(
    const WCHAR *store_root, DWORD timeout_ms, HANDLE cancel_event,
    APPX_CATALOG_SNAPSHOT **snapshot )
{
    return imports.catalog_load_bounded(
        store_root, timeout_ms, cancel_event, snapshot );
}

static HRESULT WINAPI test_catalog_publish_bounded(
    const WCHAR *store_root, UINT64 expected_epoch,
    const APPX_CATALOG_SNAPSHOT *replacement,
    DWORD timeout_ms, HANDLE cancel_event )
{
    return imports.catalog_publish_bounded(
        store_root, expected_epoch, replacement, timeout_ms, cancel_event );
}

static HRESULT WINAPI test_catalog_snapshot_create(
    UINT64 epoch, const struct appx_catalog_package *packages, UINT32 count,
    APPX_CATALOG_SNAPSHOT **snapshot )
{
    return imports.catalog_snapshot_create( epoch, packages, count, snapshot );
}

static HRESULT WINAPI test_catalog_snapshot_deep_copy(
    const APPX_CATALOG_SNAPSHOT *source, APPX_CATALOG_SNAPSHOT **snapshot )
{
    return imports.catalog_snapshot_deep_copy( source, snapshot );
}

static void WINAPI test_catalog_snapshot_free(
    APPX_CATALOG_SNAPSHOT *snapshot )
{
    imports.catalog_snapshot_free( snapshot );
}

static UINT64 WINAPI test_catalog_snapshot_get_epoch(
    const APPX_CATALOG_SNAPSHOT *snapshot )
{
    return imports.catalog_snapshot_get_epoch( snapshot );
}

static UINT32 WINAPI test_catalog_snapshot_get_package_count(
    const APPX_CATALOG_SNAPSHOT *snapshot )
{
    return imports.catalog_snapshot_get_package_count( snapshot );
}

static const struct appx_catalog_package * WINAPI
test_catalog_snapshot_get_package(
    const APPX_CATALOG_SNAPSHOT *snapshot, UINT32 index )
{
    return imports.catalog_snapshot_get_package( snapshot, index );
}

/*
 * Production package-format probing is not exercised by this executor.  Its
 * caller supplies a fully prepared package through prepare_package_callback().
 * Keep the included deployment implementation link-contained so a future test
 * which accidentally reaches the production probe fails deterministically.
 */
static HRESULT WINAPI test_archive_open(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    WINE_APPX_ARCHIVE **archive )
{
    UNREFERENCED_PARAMETER( file );
    UNREFERENCED_PARAMETER( limits );
    UNREFERENCED_PARAMETER( flags );
    if (!archive) return E_INVALIDARG;
    *archive = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI test_archive_open_ex(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    HANDLE cancel_event, WINE_APPX_ARCHIVE **archive )
{
    UNREFERENCED_PARAMETER( cancel_event );
    return test_archive_open( file, limits, flags, archive );
}

static void WINAPI test_archive_close( WINE_APPX_ARCHIVE *archive )
{
    UNREFERENCED_PARAMETER( archive );
}

static HRESULT WINAPI test_archive_find_entry(
    WINE_APPX_ARCHIVE *archive, const WCHAR *path, UINT32 *index )
{
    UNREFERENCED_PARAMETER( archive );
    UNREFERENCED_PARAMETER( path );
    UNREFERENCED_PARAMETER( index );
    return E_NOTIMPL;
}

static HRESULT WINAPI test_archive_get_entry(
    WINE_APPX_ARCHIVE *archive, UINT32 index, WINE_APPX_ARCHIVE_ENTRY *entry,
    UINT32 *path_length, WCHAR *path )
{
    UNREFERENCED_PARAMETER( archive );
    UNREFERENCED_PARAMETER( index );
    UNREFERENCED_PARAMETER( entry );
    UNREFERENCED_PARAMETER( path_length );
    UNREFERENCED_PARAMETER( path );
    return E_NOTIMPL;
}

static HRESULT WINAPI test_graph_create_with_classes(
    const APPX_CATALOG_SNAPSHOT *catalog, const WCHAR *store_root,
    const WCHAR *full_name, const WCHAR *application_id,
    enum appx_catalog_architecture target_architecture, UINT64 revision,
    const struct appx_graph_file_identity *application_identity,
    const struct appx_graph_loader_file *loader_files, UINT32 loader_count,
    const struct appx_graph_inproc_class *classes, UINT32 class_count,
    APPX_PACKAGE_GRAPH **graph )
{
    return imports.graph_create_with_classes(
        catalog, store_root, full_name, application_id, target_architecture,
        revision, application_identity, loader_files, loader_count, classes,
        class_count, graph );
}

static HRESULT WINAPI test_graph_create(
    const APPX_CATALOG_SNAPSHOT *catalog, const WCHAR *store_root,
    const WCHAR *full_name, const WCHAR *application_id,
    enum appx_catalog_architecture target_architecture, UINT64 revision,
    const struct appx_graph_file_identity *application_identity,
    const struct appx_graph_loader_file *loader_files, UINT32 loader_count,
    APPX_PACKAGE_GRAPH **graph )
{
    return imports.graph_create(
        catalog, store_root, full_name, application_id, target_architecture,
        revision, application_identity, loader_files, loader_count, graph );
}

static void WINAPI test_graph_free( APPX_PACKAGE_GRAPH *graph )
{
    imports.graph_free( graph );
}

static const BYTE *WINAPI test_graph_get_blob(
    const APPX_PACKAGE_GRAPH *graph, UINT32 *size )
{
    return imports.graph_get_blob( graph, size );
}

static UINT32 WINAPI test_graph_get_package_count(
    const APPX_PACKAGE_GRAPH *graph )
{
    return imports.graph_get_package_count( graph );
}

static HRESULT WINAPI test_graph_get_package(
    const APPX_PACKAGE_GRAPH *graph, UINT32 index,
    struct appx_graph_package *package )
{
    return imports.graph_get_package( graph, index, package );
}

static HRESULT WINAPI test_package_inspect(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    APPX_PACKAGE_INSPECTION **inspection )
{
    UNREFERENCED_PARAMETER( file );
    UNREFERENCED_PARAMETER( limits );
    UNREFERENCED_PARAMETER( flags );
    *inspection = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI test_package_inspect_ex(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    HANDLE cancel_event, APPX_PACKAGE_INSPECTION **inspection )
{
    UNREFERENCED_PARAMETER( cancel_event );
    return test_package_inspect( file, limits, flags, inspection );
}

static HRESULT WINAPI test_bundle_inspect(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    const struct appx_bundle_selection_policy *policy,
    APPX_BUNDLE_INSPECTION **inspection )
{
    UNREFERENCED_PARAMETER( file );
    UNREFERENCED_PARAMETER( limits );
    UNREFERENCED_PARAMETER( flags );
    UNREFERENCED_PARAMETER( policy );
    *inspection = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI test_bundle_inspect_ex(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    const struct appx_bundle_selection_policy *policy,
    const APPX_BUNDLE_INSPECT_OPTIONS *options,
    APPX_BUNDLE_INSPECTION **inspection )
{
    UNREFERENCED_PARAMETER( options );
    return test_bundle_inspect(
        file, limits, flags, policy, inspection );
}

static void WINAPI test_bundle_inspection_free(
    APPX_BUNDLE_INSPECTION *inspection )
{
    UNREFERENCED_PARAMETER( inspection );
}

static const APPX_PACKAGE_INSPECTION *WINAPI
test_bundle_inspection_get_selected_package(
    const APPX_BUNDLE_INSPECTION *inspection )
{
    UNREFERENCED_PARAMETER( inspection );
    return NULL;
}

static void WINAPI test_package_inspection_free(
    APPX_PACKAGE_INSPECTION *inspection )
{
    UNREFERENCED_PARAMETER( inspection );
}

static const APPX_MANIFEST *WINAPI test_package_inspection_get_manifest(
    const APPX_PACKAGE_INSPECTION *inspection )
{
    UNREFERENCED_PARAMETER( inspection );
    return NULL;
}

static UINT32 WINAPI test_package_inspection_get_file_count(
    const APPX_PACKAGE_INSPECTION *inspection )
{
    UNREFERENCED_PARAMETER( inspection );
    return 0;
}

static const APPX_PACKAGE_FILE *WINAPI test_package_inspection_get_file(
    const APPX_PACKAGE_INSPECTION *inspection, UINT32 index )
{
    UNREFERENCED_PARAMETER( inspection );
    UNREFERENCED_PARAMETER( index );
    return NULL;
}

static HRESULT WINAPI test_package_inspection_get_content_id(
    const APPX_PACKAGE_INSPECTION *inspection, BYTE *content_id, UINT32 size )
{
    UNREFERENCED_PARAMETER( inspection );
    UNREFERENCED_PARAMETER( content_id );
    UNREFERENCED_PARAMETER( size );
    return E_NOTIMPL;
}

static HRESULT WINAPI test_package_extract(
    const APPX_PACKAGE_INSPECTION *inspection, HANDLE target,
    const APPX_EXTRACT_OPTIONS *options )
{
    UNREFERENCED_PARAMETER( inspection );
    UNREFERENCED_PARAMETER( target );
    UNREFERENCED_PARAMETER( options );
    return E_NOTIMPL;
}

static const struct appx_manifest_identity *WINAPI test_manifest_get_identity(
    const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return NULL;
}

static BOOL WINAPI test_manifest_is_supported( const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return FALSE;
}

static BOOL WINAPI test_manifest_is_framework( const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return FALSE;
}

static BOOL WINAPI test_manifest_is_resource_package(
    const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return FALSE;
}

static BOOL WINAPI test_manifest_has_run_full_trust(
    const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return FALSE;
}

static UINT32 WINAPI test_manifest_get_application_count(
    const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return 0;
}

static const struct appx_manifest_application *WINAPI
test_manifest_get_application( const APPX_MANIFEST *manifest, UINT32 index )
{
    UNREFERENCED_PARAMETER( manifest );
    UNREFERENCED_PARAMETER( index );
    return NULL;
}

static UINT32 WINAPI test_manifest_get_dependency_count(
    const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return 0;
}

static const struct appx_manifest_dependency *WINAPI
test_manifest_get_dependency( const APPX_MANIFEST *manifest, UINT32 index )
{
    UNREFERENCED_PARAMETER( manifest );
    UNREFERENCED_PARAMETER( index );
    return NULL;
}

static UINT32 WINAPI test_manifest_get_inproc_class_count(
    const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return 0;
}

static const struct appx_manifest_inproc_class *WINAPI
test_manifest_get_inproc_class( const APPX_MANIFEST *manifest, UINT32 index )
{
    UNREFERENCED_PARAMETER( manifest );
    UNREFERENCED_PARAMETER( index );
    return NULL;
}

static BOOL WINAPI test_manifest_has_loader_search_path_override(
    const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return FALSE;
}

static UINT32 WINAPI test_manifest_get_loader_search_path_count(
    const APPX_MANIFEST *manifest )
{
    UNREFERENCED_PARAMETER( manifest );
    return 0;
}

static const WCHAR *WINAPI test_manifest_get_loader_search_path(
    const APPX_MANIFEST *manifest, UINT32 index )
{
    UNREFERENCED_PARAMETER( manifest );
    UNREFERENCED_PARAMETER( index );
    return NULL;
}

#define appx_catalog_load_bounded test_catalog_load_bounded
#define appx_catalog_publish_bounded test_catalog_publish_bounded
#define appx_catalog_snapshot_create test_catalog_snapshot_create
#define appx_catalog_snapshot_deep_copy test_catalog_snapshot_deep_copy
#define appx_catalog_snapshot_free test_catalog_snapshot_free
#define appx_catalog_snapshot_get_epoch test_catalog_snapshot_get_epoch
#define appx_catalog_snapshot_get_package_count \
    test_catalog_snapshot_get_package_count
#define appx_catalog_snapshot_get_package test_catalog_snapshot_get_package
#define wine_appx_archive_open test_archive_open
#define wine_appx_archive_open_ex test_archive_open_ex
#define wine_appx_archive_close test_archive_close
#define wine_appx_archive_find_entry test_archive_find_entry
#define wine_appx_archive_get_entry test_archive_get_entry
#define appx_package_graph_create test_graph_create
#define appx_package_graph_create_with_classes test_graph_create_with_classes
#define appx_package_graph_free test_graph_free
#define appx_package_graph_get_blob test_graph_get_blob
#define appx_package_graph_get_package_count test_graph_get_package_count
#define appx_package_graph_get_package test_graph_get_package
#define appx_bundle_inspect test_bundle_inspect
#define appx_bundle_inspect_ex test_bundle_inspect_ex
#define appx_bundle_inspection_free test_bundle_inspection_free
#define appx_bundle_inspection_get_selected_package \
    test_bundle_inspection_get_selected_package
#define appx_package_inspect test_package_inspect
#define appx_package_inspect_ex test_package_inspect_ex
#define appx_package_inspection_free test_package_inspection_free
#define appx_package_inspection_get_manifest \
    test_package_inspection_get_manifest
#define appx_package_inspection_get_file_count \
    test_package_inspection_get_file_count
#define appx_package_inspection_get_file test_package_inspection_get_file
#define appx_package_inspection_get_content_id \
    test_package_inspection_get_content_id
#define appx_package_extract test_package_extract
#define appx_manifest_get_identity test_manifest_get_identity
#define appx_manifest_is_supported test_manifest_is_supported
#define appx_manifest_is_framework test_manifest_is_framework
#define appx_manifest_is_resource_package test_manifest_is_resource_package
#define appx_manifest_has_run_full_trust test_manifest_has_run_full_trust
#define appx_manifest_has_loader_search_path_override \
    test_manifest_has_loader_search_path_override
#define appx_manifest_get_loader_search_path_count \
    test_manifest_get_loader_search_path_count
#define appx_manifest_get_loader_search_path \
    test_manifest_get_loader_search_path
#define appx_manifest_get_application_count test_manifest_get_application_count
#define appx_manifest_get_application test_manifest_get_application
#define appx_manifest_get_dependency_count test_manifest_get_dependency_count
#define appx_manifest_get_dependency test_manifest_get_dependency
#define appx_manifest_get_inproc_class_count test_manifest_get_inproc_class_count
#define appx_manifest_get_inproc_class test_manifest_get_inproc_class

#include "../deployment.c"

void func_deployment_test_backend(void)
{
}
