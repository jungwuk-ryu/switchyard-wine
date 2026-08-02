/*
 * AppX package catalog persistence interfaces
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

#ifndef __WINE_APPXSVC_CATALOG_H
#define __WINE_APPXSVC_CATALOG_H

#include "windef.h"
#include "winerror.h"

#define APPX_CATALOG_FILE_NAME                    L"catalog.bin"
#define APPX_CATALOG_PENDING_FILE_NAME            L"catalog.bin.pending"
#define APPX_CATALOG_LOCK_FILE_NAME               L"store.lock"

#define APPX_CATALOG_VERSION                      2
#define APPX_CATALOG_HEADER_SIZE                  96
#define APPX_CATALOG_MAX_FILE_SIZE                (16u * 1024 * 1024)
#define APPX_CATALOG_MAX_PACKAGES                 4096
#define APPX_CATALOG_MAX_APPLICATIONS_PER_PACKAGE 256
#define APPX_CATALOG_MAX_DEPENDENCIES_PER_PACKAGE 256
#define APPX_CATALOG_MAX_STRING_CHARS             32767
#define APPX_CATALOG_CONTENT_ID_SIZE              32

#ifndef ERROR_RETRY
#define ERROR_RETRY 1237
#endif
#define APPX_CATALOG_E_EPOCH_CONFLICT             HRESULT_FROM_WIN32(ERROR_RETRY)

/*
 * A publish returning S_OK is atomically visible and both the catalog contents
 * and containing-directory rename metadata were flushed.  This success code
 * means the catalog is atomically visible, but the host did not provide a
 * working directory flush; the last rename may therefore be lost after a
 * power failure or kernel crash.
 */
#define APPX_CATALOG_S_WEAK_DURABILITY             S_FALSE

typedef struct appx_catalog_snapshot APPX_CATALOG_SNAPSHOT;

enum appx_catalog_architecture
{
    APPX_CATALOG_ARCHITECTURE_NEUTRAL,
    APPX_CATALOG_ARCHITECTURE_X86,
    APPX_CATALOG_ARCHITECTURE_X64,
    APPX_CATALOG_ARCHITECTURE_ARM,
    APPX_CATALOG_ARCHITECTURE_ARM64,
    APPX_CATALOG_ARCHITECTURE_X86A64
};

enum appx_catalog_activation_kind
{
    APPX_CATALOG_ACTIVATION_UNSUPPORTED,
    APPX_CATALOG_ACTIVATION_FULL_TRUST,
    APPX_CATALOG_ACTIVATION_PACKAGED_CLASSIC,
    APPX_CATALOG_ACTIVATION_WIN32
};

#define APPX_CATALOG_PACKAGE_ACTIVE               0x00000001
#define APPX_CATALOG_PACKAGE_FRAMEWORK            0x00000002
#define APPX_CATALOG_PACKAGE_RESOURCE             0x00000004
#define APPX_CATALOG_PACKAGE_SIGNED               0x00000008
#define APPX_CATALOG_PACKAGE_KNOWN_FLAGS          \
    (APPX_CATALOG_PACKAGE_ACTIVE |                \
     APPX_CATALOG_PACKAGE_FRAMEWORK |             \
     APPX_CATALOG_PACKAGE_RESOURCE |              \
     APPX_CATALOG_PACKAGE_SIGNED)

struct appx_catalog_version
{
    UINT16 major;
    UINT16 minor;
    UINT16 build;
    UINT16 revision;
};

struct appx_catalog_application
{
    const WCHAR *id;
    const WCHAR *executable;
    const WCHAR *entry_point;
    const WCHAR *parameters;
    const WCHAR *current_directory_path;
    enum appx_catalog_activation_kind activation_kind;
};

struct appx_catalog_dependency
{
    const WCHAR *name;
    const WCHAR *publisher;
    struct appx_catalog_version min_version;
};

struct appx_catalog_package
{
    const WCHAR *name;
    const WCHAR *publisher;
    const WCHAR *resource_id;
    const WCHAR *publisher_id;
    const WCHAR *full_name;
    const WCHAR *family_name;
    const WCHAR *payload_path;
    struct appx_catalog_version version;
    enum appx_catalog_architecture architecture;
    UINT32 flags;
    BYTE content_id[APPX_CATALOG_CONTENT_ID_SIZE];
    UINT32 application_count;
    const struct appx_catalog_application *applications;
    UINT32 dependency_count;
    const struct appx_catalog_dependency *dependencies;
};

HRESULT WINAPI appx_catalog_snapshot_create(
    UINT64 epoch, const struct appx_catalog_package *packages, UINT32 count,
    APPX_CATALOG_SNAPSHOT **snapshot );
HRESULT WINAPI appx_catalog_snapshot_deep_copy(
    const APPX_CATALOG_SNAPSHOT *source, APPX_CATALOG_SNAPSHOT **snapshot );
void WINAPI appx_catalog_snapshot_free( APPX_CATALOG_SNAPSHOT *snapshot );

UINT64 WINAPI appx_catalog_snapshot_get_epoch( const APPX_CATALOG_SNAPSHOT *snapshot );
UINT32 WINAPI appx_catalog_snapshot_get_package_count( const APPX_CATALOG_SNAPSHOT *snapshot );
const struct appx_catalog_package * WINAPI appx_catalog_snapshot_get_package(
    const APPX_CATALOG_SNAPSHOT *snapshot, UINT32 index );

HRESULT WINAPI appx_catalog_load( const WCHAR *store_root,
                                  APPX_CATALOG_SNAPSHOT **snapshot );
HRESULT WINAPI appx_catalog_publish( const WCHAR *store_root, UINT64 expected_epoch,
                                     const APPX_CATALOG_SNAPSHOT *replacement );
HRESULT WINAPI appx_catalog_load_bounded( const WCHAR *store_root,
                                          DWORD timeout_ms, HANDLE cancel_event,
                                          APPX_CATALOG_SNAPSHOT **snapshot );
HRESULT WINAPI appx_catalog_publish_bounded( const WCHAR *store_root,
                                             UINT64 expected_epoch,
                                             const APPX_CATALOG_SNAPSHOT *replacement,
                                             DWORD timeout_ms,
                                             HANDLE cancel_event );

#endif /* __WINE_APPXSVC_CATALOG_H */
