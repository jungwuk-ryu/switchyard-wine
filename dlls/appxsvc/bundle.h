/*
 * AppX bundle inspection interfaces
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

#ifndef __WINE_APPXSVC_BUNDLE_H
#define __WINE_APPXSVC_BUNDLE_H

#include "windef.h"
#include "wine/appxsvc.h"

#include "bundle_manifest.h"
#include "manifest.h"
#include "package.h"

#define APPX_BUNDLE_INSPECT_ALLOW_UNTRUSTED_CHAIN 0x00000001
/* Deployment validates the selected inner package while extracting it. */
#define APPX_BUNDLE_INSPECT_DEFER_SELECTED_PAYLOAD_VALIDATION 0x00000002
#define APPX_BUNDLE_SIGNER_ID_SIZE                APPX_PACKAGE_SIGNER_ID_SIZE
#define APPX_BUNDLE_MAX_CODE_INTEGRITY_SIZE       APPX_PACKAGE_MAX_CODE_INTEGRITY_SIZE
#define APPX_BUNDLE_MATERIALIZE_BUFFER_SIZE       (1024 * 1024)
#define APPX_BUNDLE_INSPECT_OPTIONS_VERSION       1
#define APPX_BUNDLE_DEFAULT_SPACE_FLOOR           \
    (2ULL * 1024 * 1024 * 1024)
#define APPX_BUNDLE_DEFAULT_LOCK_TIMEOUT_MS       (5 * 60 * 1000)

typedef struct appx_bundle_inspection APPX_BUNDLE_INSPECTION;

/*
 * temporary_directory is a borrowed, non-reparse directory handle.  Supplying
 * it keeps private inner-package materialization on a caller-selected volume;
 * deployment uses its already-validated staging directory.  A NULL handle
 * selects the current-user temporary directory.  The per-user serialization
 * lock and repeated volume checks keep this implementation's concurrent
 * writers above free_space_floor_bytes.
 */
typedef struct
{
    UINT32 size;
    UINT32 version;
    HANDLE temporary_directory;
    HANDLE cancel_event;
    UINT64 free_space_floor_bytes;
    UINT32 lock_timeout_ms;
    UINT32 reserved;
} APPX_BUNDLE_INSPECT_OPTIONS;

/*
 * Inspecting a bundle validates the signed outer container and every embedded
 * package before applying the selection policy.  Deployment may defer the
 * selected package's bulk payload hashes until extraction; all other inner
 * packages are fully validated during inspection.  The returned selected
 * package is backed by a delete-pending private temporary file and remains
 * immutable until appx_bundle_inspection_free().
 */
HRESULT WINAPI appx_bundle_inspect(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    const struct appx_bundle_selection_policy *policy,
    APPX_BUNDLE_INSPECTION **result );
HRESULT WINAPI appx_bundle_inspect_ex(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    const struct appx_bundle_selection_policy *policy,
    const APPX_BUNDLE_INSPECT_OPTIONS *options,
    APPX_BUNDLE_INSPECTION **result );
void WINAPI appx_bundle_inspection_free( APPX_BUNDLE_INSPECTION *inspection );

const APPX_BUNDLE_MANIFEST *WINAPI appx_bundle_inspection_get_manifest(
    const APPX_BUNDLE_INSPECTION *inspection );
const struct appx_bundle_selection *WINAPI appx_bundle_inspection_get_selection(
    const APPX_BUNDLE_INSPECTION *inspection );
const APPX_PACKAGE_INSPECTION *WINAPI appx_bundle_inspection_get_selected_package(
    const APPX_BUNDLE_INSPECTION *inspection );
HRESULT WINAPI appx_bundle_inspection_get_signer_id(
    const APPX_BUNDLE_INSPECTION *inspection, BYTE *signer_id, UINT32 size );

/*
 * The source interface is a deliberately narrow test seam around the layout
 * and identity-binding core.  Production builds use the same validator after
 * signature, content-types, and block-map validation.  Returned strings remain
 * owned by the callback for the duration of the call.
 */
struct appx_bundle_source_entry
{
    UINT32 size;
    const WCHAR *path;
    const WCHAR *content_type;
    UINT32 flags;
    UINT16 compression_method;
    UINT16 reserved;
    UINT64 compressed_size;
    UINT64 uncompressed_size;
    UINT64 data_offset;
};

struct appx_bundle_source_package
{
    UINT32 size;
    const WCHAR *name;
    const WCHAR *publisher;
    const WCHAR *resource_id;
    struct appx_manifest_version version;
    enum appx_manifest_architecture architecture;
    BOOL framework;
    BOOL resource;
    BYTE signer_id[APPX_BUNDLE_SIGNER_ID_SIZE];
    UINT64 expanded_size;
};

struct appx_bundle_test_source
{
    UINT32 size;
    void *context;
    UINT32 entry_count;
    UINT64 max_total_expanded_size;
    HRESULT (WINAPI *get_entry)(
        void *context, UINT32 index, struct appx_bundle_source_entry *entry );
    HRESULT (WINAPI *inspect_package)(
        void *context, UINT32 manifest_index, UINT32 archive_index,
        UINT64 remaining_expanded_size,
        struct appx_bundle_source_package *package );
};

HRESULT WINAPI appx_bundle_validate_with_test_source(
    const APPX_BUNDLE_MANIFEST *manifest,
    const BYTE outer_signer_id[APPX_BUNDLE_SIGNER_ID_SIZE],
    const struct appx_bundle_selection_policy *policy,
    const struct appx_bundle_test_source *source,
    struct appx_bundle_selection *selection );

/*
 * Exercise the exact production private-file streaming path without requiring
 * a signed fixture.  inspect() receives a read-only, delete-pending file at
 * position zero.  S_FALSE is the only accepted terminal read result.
 */
struct appx_bundle_materialize_test_source
{
    UINT32 size;
    void *context;
    HRESULT (WINAPI *read)( void *context, void *buffer, UINT32 capacity,
                            UINT32 *read );
    HRESULT (WINAPI *inspect)( void *context, HANDLE file );
    const APPX_BUNDLE_INSPECT_OPTIONS *options;
};

HRESULT WINAPI appx_bundle_materialize_with_test_source(
    UINT64 expected_size,
    const struct appx_bundle_materialize_test_source *source );

#endif /* __WINE_APPXSVC_BUNDLE_H */
