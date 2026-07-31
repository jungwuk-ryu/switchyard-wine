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

/*
 * The blob format is little-endian and contains offsets rather than pointers.
 * Its fixed-width layout is therefore identical in native and WOW64 processes.
 * A receiver must call appx_package_graph_validate_blob() before retaining or
 * interpreting bytes received across a trust boundary.
 */
#define APPX_GRAPH_BLOB_VERSION                    1
#define APPX_GRAPH_BLOB_HEADER_SIZE                128
#define APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE        112
#define APPX_GRAPH_BLOB_LOADER_RECORD_SIZE         24
#define APPX_GRAPH_MAX_BLOB_SIZE                   (16u * 1024 * 1024)
#define APPX_GRAPH_MAX_PACKAGES                    256
#define APPX_GRAPH_MAX_LOADER_FILES                16384
#define APPX_GRAPH_MAX_STRING_CHARS                32767

#define APPX_GRAPH_E_RESOLVE_DEPENDENCY_FAILED \
    HRESULT_FROM_WIN32(ERROR_INSTALL_RESOLVE_DEPENDENCY_FAILED)

typedef struct appx_package_graph APPX_PACKAGE_GRAPH;

/*
 * Loader-file inventory is supplied by the already-validated deployment
 * record.  Graph construction never enumerates a directory.  A relative path
 * is indexed by its final component; duplicate basenames within one package
 * are rejected because basename-only loader resolution would be ambiguous.
 */
struct appx_graph_loader_file
{
    const WCHAR *package_full_name;
    const WCHAR *relative_path;
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
};

struct appx_graph_loader_match
{
    UINT32 package_index;
    const WCHAR *package_root;
    const WCHAR *relative_path;
};

/*
 * store_root is the canonical drive-absolute root of the immutable package
 * store.  Catalog payload paths are store-relative; construction checks and
 * joins both parts so every packed package record is independently usable by
 * the loader without consulting the catalog or filesystem.
 */
HRESULT WINAPI appx_package_graph_create(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *store_root,
    const WCHAR *package_full_name, const WCHAR *application_id,
    enum appx_catalog_architecture target_architecture, UINT64 revision,
    const struct appx_graph_loader_file *loader_files, UINT32 loader_file_count,
    APPX_PACKAGE_GRAPH **graph );
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

#endif /* __WINE_APPXSVC_GRAPH_H */
