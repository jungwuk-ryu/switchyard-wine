/*
 * AppX package deployment transaction interfaces
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

#ifndef __WINE_APPXSVC_DEPLOYMENT_H
#define __WINE_APPXSVC_DEPLOYMENT_H

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "catalog.h"
#include "package.h"
#include "extract.h"
#include "graph.h"

#define APPX_DEPLOYMENT_OPTIONS_VERSION              1
#define APPX_DEPLOYMENT_RESULT_VERSION               1
#define APPX_DEPLOYMENT_TEST_BACKEND_VERSION         1
#define APPX_DEPLOYMENT_TEST_PACKAGE_VERSION         1

#define APPX_DEPLOYMENT_DEFAULT_LOCK_TIMEOUT_MS      30000
#define APPX_DEPLOYMENT_MAX_LOCK_TIMEOUT_MS          300000
#define APPX_DEPLOYMENT_DEFAULT_EPOCH_RETRIES        8
#define APPX_DEPLOYMENT_MAX_EPOCH_RETRIES            64
#define APPX_DEPLOYMENT_DEFAULT_GC_ENTRIES           4096
#define APPX_DEPLOYMENT_MAX_GC_ENTRIES               65536
#define APPX_DEPLOYMENT_DEFAULT_GC_BYTES              \
    ((UINT64)4 * 1024 * 1024 * 1024)
#define APPX_DEPLOYMENT_MAX_JOURNAL_SIZE             (64u * 1024)
#define APPX_DEPLOYMENT_MAX_RECORD_SIZE              (16u * 1024 * 1024)
#define APPX_DEPLOYMENT_MAX_LOADER_FILES             16384
#define APPX_DEPLOYMENT_MAX_INPROC_CLASSES           1024
#define APPX_DEPLOYMENT_MAX_LOADER_SEARCH_PATHS      5
#define APPX_DEPLOYMENT_RECORD_VERSION               4
#define APPX_DEPLOYMENT_JOURNAL_VERSION              1
#define APPX_DEPLOYMENT_CONTENT_ID_SIZE              32
#define APPX_DEPLOYMENT_TRANSACTION_ID_SIZE          16
#define APPX_DEPLOYMENT_FILE_DIGEST_SIZE             32

#define APPX_DEPLOYMENT_ALLOW_DOWNGRADE              0x00000001
#define APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY       0x00000002
#define APPX_DEPLOYMENT_SKIP_GARBAGE_COLLECTION      0x00000004
#define APPX_DEPLOYMENT_KNOWN_FLAGS                  \
    (APPX_DEPLOYMENT_ALLOW_DOWNGRADE |              \
     APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY |       \
     APPX_DEPLOYMENT_SKIP_GARBAGE_COLLECTION)

#define APPX_DEPLOYMENT_RESULT_CATALOG_CHANGED       0x00000001
#define APPX_DEPLOYMENT_RESULT_WEAK_DURABILITY       0x00000002
#define APPX_DEPLOYMENT_RESULT_PAYLOAD_REUSED        0x00000004
#define APPX_DEPLOYMENT_RESULT_GC_DEFERRED            0x00000008

#ifndef ERROR_INSTALL_PACKAGE_ALREADY_EXISTS
#define ERROR_INSTALL_PACKAGE_ALREADY_EXISTS         15611
#endif
#ifndef ERROR_INSTALL_PACKAGE_DOWNGRADE
#define ERROR_INSTALL_PACKAGE_DOWNGRADE              15622
#endif
#ifndef ERROR_INSTALL_DEPENDENCY_FAILURE
#define ERROR_INSTALL_DEPENDENCY_FAILURE             15618
#endif
#ifndef ERROR_INSTALL_PACKAGE_IN_USE
#define ERROR_INSTALL_PACKAGE_IN_USE                 15616
#endif
#ifndef ERROR_TIMEOUT
#define ERROR_TIMEOUT                                1460
#endif

#define APPX_DEPLOYMENT_E_CONTENT_CONFLICT \
    HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_ALREADY_EXISTS)
#define APPX_DEPLOYMENT_E_DOWNGRADE \
    HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_DOWNGRADE)
#define APPX_DEPLOYMENT_E_DEPENDENCY \
    HRESULT_FROM_WIN32(ERROR_INSTALL_DEPENDENCY_FAILURE)
#define APPX_DEPLOYMENT_E_PACKAGE_IN_USE \
    HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_IN_USE)
#define APPX_DEPLOYMENT_E_LOCK_TIMEOUT \
    HRESULT_FROM_WIN32(ERROR_TIMEOUT)
#define APPX_DEPLOYMENT_E_CORRUPT_STORE APPX_E_INVALID_PACKAGING_LAYOUT
#define APPX_DEPLOYMENT_S_WEAK_DURABILITY S_FALSE

typedef struct appx_deployment_result APPX_DEPLOYMENT_RESULT;
typedef struct appx_deployment_record APPX_DEPLOYMENT_RECORD;
typedef struct appx_deployment_runtime APPX_DEPLOYMENT_RUNTIME;

struct appx_deployment_file_identity
{
    UINT32 volume_serial;
    UINT32 file_index_high;
    UINT32 file_index_low;
};

struct appx_deployment_file_integrity
{
    UINT64 size;
    BYTE digest[APPX_DEPLOYMENT_FILE_DIGEST_SIZE];
};

struct appx_deployment_loader_file
{
    const WCHAR *relative_path;
    struct appx_deployment_file_identity identity;
    struct appx_deployment_file_integrity integrity;
};

struct appx_deployment_inproc_class
{
    const WCHAR *path;
    const WCHAR *activatable_class_id;
    enum appx_manifest_threading_model threading_model;
    struct appx_deployment_file_identity identity;
    struct appx_deployment_file_integrity integrity;
};

struct appx_deployment_application_file
{
    const WCHAR *id;
    const WCHAR *executable;
    const WCHAR *entry_point;
    const WCHAR *parameters;
    const WCHAR *current_directory_path;
    enum appx_catalog_activation_kind activation_kind;
    const WCHAR * const *loader_search_paths;
    UINT32 loader_search_path_count;
    BOOL has_loader_search_path_override;
    struct appx_deployment_file_identity identity;
    struct appx_deployment_file_integrity integrity;
};

/*
 * Every field is read only when size covers it.  Version 1 callers must set
 * size to sizeof(APPX_DEPLOYMENT_OPTIONS) and version to
 * APPX_DEPLOYMENT_OPTIONS_VERSION.  store_root is borrowed for the duration of
 * the call and must be a canonical drive-absolute path without a trailing
 * separator (except "X:\").  The deployment layer derives all child names.
 */
typedef struct
{
    UINT32 size;
    UINT32 version;
    const WCHAR *store_root;
    UINT32 flags;
    enum appx_catalog_architecture target_architecture;
    UINT32 writer_timeout_ms;
    UINT32 max_epoch_retries;
    UINT32 max_gc_entries;
    UINT64 max_gc_bytes;
    UINT64 package_quota_bytes;
    UINT64 free_space_floor_bytes;
    HANDLE cancel_event;
    const WINE_APPX_ARCHIVE_LIMITS *archive_limits;
} APPX_DEPLOYMENT_OPTIONS;

/*
 * Initialize or validate the private store tree.  Existing nodes are opened
 * without following reparse points and checked for directory type and stable
 * identity.  A successful call does not imply that any package is installed.
 */
HRESULT WINAPI appx_deployment_initialize(
    const APPX_DEPLOYMENT_OPTIONS *options, APPX_DEPLOYMENT_RESULT **result );

/*
 * The package handle is borrowed until the call returns.  It must identify a
 * regular, seekable, non-reparse package or bundle file and allow the
 * inspectors to reopen it for shared read access.  A bundle is accepted only
 * after its outer container and every embedded package have been verified; the
 * neutral or exact target-architecture application/framework payload is then
 * deployed.  Resource, optional, encrypted, ambiguous, or otherwise
 * unsupported bundle payloads fail closed.  Install requires that the family
 * has no active generation.  Update requires an active generation in the same
 * family.
 */
HRESULT WINAPI appx_deployment_install(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result );
HRESULT WINAPI appx_deployment_update(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result );

/*
 * full_name is borrowed for the duration of the call and must match one active
 * catalog record.  Removal publishes a catalog without that record before any
 * payload bytes are reclaimed.  Active dependents prevent removal.
 */
HRESULT WINAPI appx_deployment_remove(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result );

/*
 * Returns a caller-owned immutable snapshot.  Pass NULL for full_name to query
 * the complete catalog.  A non-NULL name filters to one exact
 * case-insensitive full-name match.  Every referenced payload is verified as
 * a pinned, non-reparse directory before success.  Free the result with
 * appx_catalog_snapshot_free().
 */
HRESULT WINAPI appx_deployment_query(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_CATALOG_SNAPSHOT **snapshot );

/*
 * Load the immutable deployment sidecar bound to an already validated catalog
 * record.  The returned pointer-free snapshot owns all strings exposed by the
 * accessors; views become invalid only when appx_deployment_record_free() is
 * called.  Missing, substituted, truncated, digest-invalid, or identity-
 * mismatched records fail closed.
 */
HRESULT WINAPI appx_deployment_record_load(
    const struct appx_catalog_package *package,
    const APPX_DEPLOYMENT_OPTIONS *options, APPX_DEPLOYMENT_RECORD **record );
void WINAPI appx_deployment_record_free( APPX_DEPLOYMENT_RECORD *record );
UINT32 WINAPI appx_deployment_record_get_loader_file_count(
    const APPX_DEPLOYMENT_RECORD *record );
const struct appx_deployment_loader_file *WINAPI
    appx_deployment_record_get_loader_file(
        const APPX_DEPLOYMENT_RECORD *record, UINT32 index );
UINT32 WINAPI appx_deployment_record_get_inproc_class_count(
    const APPX_DEPLOYMENT_RECORD *record );
const struct appx_deployment_inproc_class *WINAPI
    appx_deployment_record_get_inproc_class(
        const APPX_DEPLOYMENT_RECORD *record, UINT32 index );
UINT32 WINAPI appx_deployment_record_get_application_file_count(
    const APPX_DEPLOYMENT_RECORD *record );
const struct appx_deployment_application_file *WINAPI
    appx_deployment_record_get_application_file(
        const APPX_DEPLOYMENT_RECORD *record, UINT32 index );
BOOL WINAPI appx_deployment_record_has_loader_search_path_override(
    const APPX_DEPLOYMENT_RECORD *record );
UINT32 WINAPI appx_deployment_record_get_loader_search_path_count(
    const APPX_DEPLOYMENT_RECORD *record );
const WCHAR * WINAPI appx_deployment_record_get_loader_search_path(
    const APPX_DEPLOYMENT_RECORD *record, UINT32 index );

/*
 * Acquire a launch-time runtime binding for one full-trust application.  The
 * call loads and validates one catalog snapshot, builds one exact package
 * graph from that snapshot, opens a non-write/non-delete executable handle,
 * and holds one validated generation marker lease per graph package until
 * appx_deployment_runtime_free().  The returned strings and handles are
 * borrowed from the runtime object.
 */
HRESULT WINAPI appx_deployment_runtime_acquire(
    const WCHAR *full_name, const WCHAR *application_id,
    const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RUNTIME **runtime );
void WINAPI appx_deployment_runtime_free(
    APPX_DEPLOYMENT_RUNTIME *runtime );
const APPX_PACKAGE_GRAPH *WINAPI appx_deployment_runtime_get_graph(
    const APPX_DEPLOYMENT_RUNTIME *runtime );
UINT32 WINAPI appx_deployment_runtime_get_lease_count(
    const APPX_DEPLOYMENT_RUNTIME *runtime );
const HANDLE *WINAPI appx_deployment_runtime_get_leases(
    const APPX_DEPLOYMENT_RUNTIME *runtime );
const struct wine_appx_graph_attach *WINAPI
    appx_deployment_runtime_get_attach(
        const APPX_DEPLOYMENT_RUNTIME *runtime );
HANDLE WINAPI appx_deployment_runtime_get_executable_handle(
    const APPX_DEPLOYMENT_RUNTIME *runtime );
const WCHAR *WINAPI appx_deployment_runtime_get_executable_path(
    const APPX_DEPLOYMENT_RUNTIME *runtime );
const WCHAR *WINAPI appx_deployment_runtime_get_parameters(
    const APPX_DEPLOYMENT_RUNTIME *runtime );
const WCHAR *WINAPI appx_deployment_runtime_get_current_directory(
    const APPX_DEPLOYMENT_RUNTIME *runtime );
HRESULT WINAPI appx_deployment_launch(
    const WCHAR *full_name, const WCHAR *application_id,
    const APPX_DEPLOYMENT_OPTIONS *options, const STARTUPINFOW *startup,
    PROCESS_INFORMATION *process_information );

/*
 * Recovery is bounded by max_gc_entries/max_gc_bytes.  It never removes an
 * unknown, truncated, digest-invalid, or future-version journal.  Such input
 * fails closed and remains in place as evidence.
 */
HRESULT WINAPI appx_deployment_recover(
    const APPX_DEPLOYMENT_OPTIONS *options, APPX_DEPLOYMENT_RESULT **result );
HRESULT WINAPI appx_deployment_collect_garbage(
    const APPX_DEPLOYMENT_OPTIONS *options, APPX_DEPLOYMENT_RESULT **result );

/*
 * Result objects are immutable and owned by the caller.  The two-call package
 * name accessor reports the required WCHAR count including the terminator.
 * Initialize *chars to the supplied capacity; passing NULL data obtains the
 * required count.  On ERROR_INSUFFICIENT_BUFFER no partial string is written.
 */
void WINAPI appx_deployment_result_free( APPX_DEPLOYMENT_RESULT *result );
UINT32 WINAPI appx_deployment_result_get_flags(
    const APPX_DEPLOYMENT_RESULT *result );
UINT64 WINAPI appx_deployment_result_get_catalog_epoch(
    const APPX_DEPLOYMENT_RESULT *result );
UINT64 WINAPI appx_deployment_result_get_reclaimed_bytes(
    const APPX_DEPLOYMENT_RESULT *result );
UINT32 WINAPI appx_deployment_result_get_reclaimed_entries(
    const APPX_DEPLOYMENT_RESULT *result );
HRESULT WINAPI appx_deployment_result_get_package_full_name(
    const APPX_DEPLOYMENT_RESULT *result, UINT32 *chars, WCHAR *name );

enum appx_deployment_operation
{
    APPX_DEPLOYMENT_OPERATION_INSTALL = 1,
    APPX_DEPLOYMENT_OPERATION_UPDATE,
    APPX_DEPLOYMENT_OPERATION_REMOVE,
    APPX_DEPLOYMENT_OPERATION_RECOVER,
    APPX_DEPLOYMENT_OPERATION_GARBAGE_COLLECT
};

enum appx_deployment_journal_state
{
    APPX_DEPLOYMENT_JOURNAL_CREATED = 1,
    APPX_DEPLOYMENT_JOURNAL_INSPECTED,
    APPX_DEPLOYMENT_JOURNAL_STAGED,
    APPX_DEPLOYMENT_JOURNAL_PAYLOAD_COMPLETE,
    APPX_DEPLOYMENT_JOURNAL_DEPENDENCY_CHECKED,
    APPX_DEPLOYMENT_JOURNAL_CATALOG_PREPARED,
    APPX_DEPLOYMENT_JOURNAL_PUBLISHED,
    APPX_DEPLOYMENT_JOURNAL_GC_PENDING,
    APPX_DEPLOYMENT_JOURNAL_CLEANED
};

enum appx_deployment_test_checkpoint
{
    APPX_DEPLOYMENT_CHECKPOINT_STORE_READY = 1,
    APPX_DEPLOYMENT_CHECKPOINT_JOURNAL_CREATED,
    APPX_DEPLOYMENT_CHECKPOINT_INSPECTED,
    APPX_DEPLOYMENT_CHECKPOINT_STAGING_CREATED,
    APPX_DEPLOYMENT_CHECKPOINT_EXTRACTED,
    APPX_DEPLOYMENT_CHECKPOINT_PAYLOAD_RENAMED,
    APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PREPARED,
    APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PUBLISHED,
    APPX_DEPLOYMENT_CHECKPOINT_CATALOG_UNPUBLISHED,
    APPX_DEPLOYMENT_CHECKPOINT_GC_STARTED,
    APPX_DEPLOYMENT_CHECKPOINT_GC_FINISHED,
    APPX_DEPLOYMENT_CHECKPOINT_CLEANED
};

/*
 * Private deterministic test seam.  The catalog package and its child arrays
 * are borrowed from prepare_package until release_package.  Production input
 * inspection is used when prepare_package is NULL.  Tests may replace only
 * the callbacks they need; every NULL callback selects the production
 * implementation.  Callbacks must not retain borrowed handles or pointers.
 */
typedef struct
{
    UINT32 size;
    UINT32 version;
    UINT32 flags;
    struct appx_catalog_package package;
    UINT32 loader_file_count;
    const struct appx_deployment_loader_file *loader_files;
    UINT32 inproc_class_count;
    const struct appx_deployment_inproc_class *inproc_classes;
} APPX_DEPLOYMENT_TEST_PACKAGE;

#define APPX_DEPLOYMENT_TEST_PACKAGE_UNSUPPORTED 0x00000001
#define APPX_DEPLOYMENT_TEST_PACKAGE_RESOURCE    0x00000002
#define APPX_DEPLOYMENT_TEST_PACKAGE_KNOWN_FLAGS \
    (APPX_DEPLOYMENT_TEST_PACKAGE_UNSUPPORTED | \
     APPX_DEPLOYMENT_TEST_PACKAGE_RESOURCE)

typedef HRESULT (WINAPI *APPX_DEPLOYMENT_PREPARE_PACKAGE_CALLBACK)(
    void *context, HANDLE package_file,
    enum appx_catalog_architecture target_architecture,
    APPX_DEPLOYMENT_TEST_PACKAGE *package );
typedef void (WINAPI *APPX_DEPLOYMENT_RELEASE_PACKAGE_CALLBACK)(
    void *context, APPX_DEPLOYMENT_TEST_PACKAGE *package );
typedef HRESULT (WINAPI *APPX_DEPLOYMENT_EXTRACT_CALLBACK)(
    void *context, HANDLE staging_root, const APPX_EXTRACT_OPTIONS *options );
typedef HRESULT (WINAPI *APPX_DEPLOYMENT_CHECKPOINT_CALLBACK)(
    void *context, enum appx_deployment_test_checkpoint checkpoint );
typedef BOOL (WINAPI *APPX_DEPLOYMENT_WRITE_CALLBACK)(
    void *context, HANDLE file, const void *data, DWORD size, DWORD *written );
typedef HRESULT (WINAPI *APPX_DEPLOYMENT_FLUSH_CALLBACK)(
    void *context, HANDLE handle, BOOL directory );
typedef HRESULT (WINAPI *APPX_DEPLOYMENT_RENAME_CALLBACK)(
    void *context, HANDLE object, HANDLE destination_root,
    const WCHAR *destination, BOOL replace );
typedef HRESULT (WINAPI *APPX_DEPLOYMENT_CATALOG_LOAD_CALLBACK)(
    void *context, const WCHAR *store_root, APPX_CATALOG_SNAPSHOT **snapshot );
typedef HRESULT (WINAPI *APPX_DEPLOYMENT_CATALOG_PUBLISH_CALLBACK)(
    void *context, const WCHAR *store_root, UINT64 expected_epoch,
    const APPX_CATALOG_SNAPSHOT *replacement );
typedef HRESULT (WINAPI *APPX_DEPLOYMENT_GENERATION_IN_USE_CALLBACK)(
    void *context, const WCHAR *payload_path, BOOL *in_use );
typedef HRESULT (WINAPI *APPX_DEPLOYMENT_RANDOM_CALLBACK)(
    void *context, BYTE *data, UINT32 size );

typedef struct
{
    UINT32 size;
    UINT32 version;
    void *context;
    APPX_DEPLOYMENT_PREPARE_PACKAGE_CALLBACK prepare_package;
    APPX_DEPLOYMENT_RELEASE_PACKAGE_CALLBACK release_package;
    APPX_DEPLOYMENT_EXTRACT_CALLBACK extract;
    APPX_DEPLOYMENT_CHECKPOINT_CALLBACK checkpoint;
    APPX_DEPLOYMENT_WRITE_CALLBACK write;
    APPX_DEPLOYMENT_FLUSH_CALLBACK flush;
    APPX_DEPLOYMENT_RENAME_CALLBACK rename;
    APPX_DEPLOYMENT_CATALOG_LOAD_CALLBACK catalog_load;
    APPX_DEPLOYMENT_CATALOG_PUBLISH_CALLBACK catalog_publish;
    APPX_DEPLOYMENT_GENERATION_IN_USE_CALLBACK generation_in_use;
    APPX_DEPLOYMENT_RANDOM_CALLBACK random;
} APPX_DEPLOYMENT_TEST_BACKEND;

#ifdef APPX_DEPLOYMENT_TESTING
HRESULT WINAPI appx_deployment_execute_with_test_backend(
    enum appx_deployment_operation operation, HANDLE package_file,
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    const APPX_DEPLOYMENT_TEST_BACKEND *backend,
    APPX_DEPLOYMENT_RESULT **result );
#endif

#endif /* __WINE_APPXSVC_DEPLOYMENT_H */
