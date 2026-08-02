/*
 * AppX packaged process graph interfaces
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

#ifndef __WINE_APPXSVC_GRAPH_H
#define __WINE_APPXSVC_GRAPH_H

#include "catalog.h"
#include "wine/appx_package_graph.h"

/*
 * The blob format is little-endian and contains offsets rather than pointers.
 * Its fixed-width layout is therefore identical in native and WOW64 processes.
 * A receiver must call appx_package_graph_validate_blob() before retaining or
 * interpreting bytes received across a trust boundary.
 */
#define APPX_GRAPH_BLOB_VERSION                    WINE_APPX_GRAPH_BLOB_VERSION
#define APPX_GRAPH_BLOB_HEADER_SIZE                WINE_APPX_GRAPH_BLOB_HEADER_SIZE
#define APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE        \
    WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE
#define APPX_GRAPH_BLOB_LOADER_RECORD_SIZE         \
    WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE
#define APPX_GRAPH_BLOB_CLASS_RECORD_SIZE          \
    WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE
#define APPX_GRAPH_MAX_BLOB_SIZE                   WINE_APPX_GRAPH_MAX_BLOB_SIZE
#define APPX_GRAPH_MAX_PACKAGES                    WINE_APPX_GRAPH_MAX_PACKAGES
#define APPX_GRAPH_MAX_LOADER_FILES                \
    WINE_APPX_GRAPH_MAX_LOADER_FILES
#define APPX_GRAPH_MAX_CLASSES                     WINE_APPX_GRAPH_MAX_CLASSES
#define APPX_GRAPH_MAX_STRING_CHARS                \
    WINE_APPX_GRAPH_MAX_STRING_CHARS
#define APPX_GRAPH_PACKAGE_DIRECT                  WINE_APPX_GRAPH_PACKAGE_DIRECT

#define APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED \
    HRESULT_FROM_WIN32(ERROR_INSTALL_RESOLVE_DEPENDENCY_FAILED)

typedef struct appx_package_graph APPX_PACKAGE_GRAPH;

struct appx_graph_file_identity
{
    UINT32 volume_serial;
    UINT32 file_index_high;
    UINT32 file_index_low;
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
};

/*
 * Loader and class inventories are supplied by the already-validated
 * deployment record.  Their identities must come from final read handles
 * opened without following a substitutable path.  Graph construction never
 * enumerates a directory.  search_rank is the zero-based position of the
 * relative path's parent directory in the effective loader search override.
 * WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY keeps the file available to an exact
 * relative-path LoadPackagedLibrary call without exposing it to basename
 * search.  Duplicate basenames in distinct paths are valid.
 */
struct appx_graph_loader_file
{
    const WCHAR *package_full_name;
    const WCHAR *relative_path;
    UINT32 volume_serial;
    UINT32 file_index_high;
    UINT32 file_index_low;
    UINT32 search_rank;
    UINT64 change_time;
    UINT64 file_size;
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
};

struct appx_graph_inproc_class
{
    const WCHAR *package_full_name;
    const WCHAR *activatable_class_id;
    const WCHAR *path;
    UINT32 threading_model;
    UINT32 volume_serial;
    UINT32 file_index_high;
    UINT32 file_index_low;
    UINT64 change_time;
    UINT64 file_size;
};

struct appx_graph_package
{
    const WCHAR *name;
    const WCHAR *publisher;
    const WCHAR *resource_id;
    const WCHAR *publisher_id;
    const WCHAR *full_name;
    const WCHAR *family_name;
    const WCHAR *root;
    struct appx_catalog_version version;
    enum appx_catalog_architecture architecture;
    UINT32 flags;
    BYTE content_id[APPX_CATALOG_CONTENT_ID_SIZE];
    UINT32 rank;
};

struct appx_graph_application
{
    const WCHAR *id;
    const WCHAR *aumid;
    const WCHAR *executable;
    const WCHAR *entry_point;
    enum appx_catalog_activation_kind activation_kind;
    UINT32 volume_serial;
    UINT32 file_index_high;
    UINT32 file_index_low;
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
};

struct appx_graph_loader_match
{
    UINT32 package_index;
    const WCHAR *package_root;
    const WCHAR *relative_path;
    UINT32 volume_serial;
    UINT32 file_index_high;
    UINT32 file_index_low;
    UINT64 change_time;
    UINT64 file_size;
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
};

struct appx_graph_class_match
{
    UINT32 package_index;
    UINT32 threading_model;
    const WCHAR *package_root;
    const WCHAR *activatable_class_id;
    const WCHAR *path;
    UINT32 volume_serial;
    UINT32 file_index_high;
    UINT32 file_index_low;
    UINT64 change_time;
    UINT64 file_size;
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
};

/*
 * store_root is the canonical drive-absolute root of the immutable package
 * store.  Catalog payload paths are store-relative; construction checks and
 * joins both parts so every packed package record is independently usable by
 * the loader without consulting the catalog or filesystem.
 * application_identity must describe the final main-executable handle.
 * target_architecture is the concrete dependency domain derived from that
 * held executable's validated PE machine, not the caller or host preference.
 */
HRESULT WINAPI appx_package_graph_create(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *store_root,
    const WCHAR *package_full_name, const WCHAR *application_id,
    enum appx_catalog_architecture target_architecture, UINT64 revision,
    const struct appx_graph_file_identity *application_identity,
    const struct appx_graph_loader_file *loader_files, UINT32 loader_file_count,
    APPX_PACKAGE_GRAPH **graph );
HRESULT WINAPI appx_package_graph_create_with_classes(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *store_root,
    const WCHAR *package_full_name, const WCHAR *application_id,
    enum appx_catalog_architecture target_architecture, UINT64 revision,
    const struct appx_graph_file_identity *application_identity,
    const struct appx_graph_loader_file *loader_files, UINT32 loader_file_count,
    const struct appx_graph_inproc_class *classes, UINT32 class_count,
    APPX_PACKAGE_GRAPH **graph );
HRESULT WINAPI appx_package_graph_resolve_direct_dependencies(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *package_full_name,
    enum appx_catalog_architecture target_architecture, UINT32 capacity,
    UINT32 *package_indices, UINT32 *count );
HRESULT WINAPI appx_package_graph_from_blob(
    const void *data, SIZE_T size, APPX_PACKAGE_GRAPH **graph );
HRESULT WINAPI appx_package_graph_clone(
    const APPX_PACKAGE_GRAPH *source, APPX_PACKAGE_GRAPH **graph );
HRESULT WINAPI appx_package_graph_validate_blob( const void *data, SIZE_T size );
void WINAPI appx_package_graph_free( APPX_PACKAGE_GRAPH *graph );

const BYTE * WINAPI appx_package_graph_get_blob(
    const APPX_PACKAGE_GRAPH *graph, UINT32 *size );
UINT64 WINAPI appx_package_graph_get_epoch( const APPX_PACKAGE_GRAPH *graph );
UINT64 WINAPI appx_package_graph_get_revision( const APPX_PACKAGE_GRAPH *graph );
enum appx_catalog_architecture WINAPI appx_package_graph_get_target_architecture(
    const APPX_PACKAGE_GRAPH *graph );
UINT32 WINAPI appx_package_graph_get_package_count(
    const APPX_PACKAGE_GRAPH *graph );
HRESULT WINAPI appx_package_graph_get_package(
    const APPX_PACKAGE_GRAPH *graph, UINT32 index, struct appx_graph_package *package );
HRESULT WINAPI appx_package_graph_get_application(
    const APPX_PACKAGE_GRAPH *graph, struct appx_graph_application *application );
HRESULT WINAPI appx_package_graph_lookup_basename(
    const APPX_PACKAGE_GRAPH *graph, const WCHAR *basename,
    struct appx_graph_loader_match *match );
UINT32 WINAPI appx_package_graph_get_inproc_class_count(
    const APPX_PACKAGE_GRAPH *graph );
HRESULT WINAPI appx_package_graph_get_inproc_class(
    const APPX_PACKAGE_GRAPH *graph, UINT32 index,
    struct appx_graph_class_match *match );
HRESULT WINAPI appx_package_graph_lookup_inproc_class(
    const APPX_PACKAGE_GRAPH *graph, const WCHAR *activatable_class_id,
    struct appx_graph_class_match *match );

#endif /* __WINE_APPXSVC_GRAPH_H */
