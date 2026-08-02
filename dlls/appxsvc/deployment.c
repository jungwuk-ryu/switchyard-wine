/*
 * AppX package deployment transactions
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
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winioctl.h"
#include "winnls.h"
#include "winerror.h"
#include "winternl.h"
#include "bcrypt.h"

#include "architecture.h"
#include "bundle.h"
#include "deployment.h"
#include "graph.h"
#include "wine/appx_package_graph.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define DEPLOYMENT_STORE_LOCK_NAME          L"store.lock"
#define DEPLOYMENT_TRANSACTIONS_NAME        L"transactions"
#define DEPLOYMENT_STAGING_NAME             L"staging"
#define DEPLOYMENT_PAYLOADS_NAME            L"payloads"
#define DEPLOYMENT_QUARANTINE_NAME           L"quarantine"
#define DEPLOYMENT_RECORDS_NAME              L"records"
#define DEPLOYMENT_RECORD_STAGING_NAME       L"record-staging"
#define DEPLOYMENT_RECORD_QUARANTINE_NAME    L"record-quarantine"
#define DEPLOYMENT_LEASES_NAME              L"leases"
#define DEPLOYMENT_LEASE_PENDING_NAME       L"pending"
#define DEPLOYMENT_LEASE_GENERATIONS_NAME   L"generations"

#define DEPLOYMENT_JOURNAL_HEADER_SIZE      256
#define JOURNAL_VERSION_OFFSET              8
#define JOURNAL_HEADER_SIZE_OFFSET          12
#define JOURNAL_TOTAL_SIZE_OFFSET           16
#define JOURNAL_OPERATION_OFFSET            20
#define JOURNAL_STATE_OFFSET                24
#define JOURNAL_RESERVED0_OFFSET            28
#define JOURNAL_EXPECTED_EPOCH_OFFSET       32
#define JOURNAL_TRANSACTION_ID_OFFSET       40
#define JOURNAL_CONTENT_ID_OFFSET           56
#define JOURNAL_DIGEST_OFFSET               88
#define JOURNAL_FULL_NAME_REF_OFFSET        120
#define JOURNAL_FAMILY_NAME_REF_OFFSET      128
#define JOURNAL_PACKAGE_NAME_REF_OFFSET     136
#define JOURNAL_PUBLISHER_REF_OFFSET        144
#define JOURNAL_PAYLOAD_PATH_REF_OFFSET     152
#define JOURNAL_STAGING_NAME_REF_OFFSET     160
#define JOURNAL_RESERVED_OFFSET             168
#define JOURNAL_RESERVED_SIZE               \
    (DEPLOYMENT_JOURNAL_HEADER_SIZE - JOURNAL_RESERVED_OFFSET)

#define DEPLOYMENT_DIGEST_SIZE              32
#define DEPLOYMENT_PATH_HASH_SIZE           32
#define DEPLOYMENT_GENERATION_CHARS         (DEPLOYMENT_PATH_HASH_SIZE * 2)
#define DEPLOYMENT_TRANSACTION_CHARS        \
    (APPX_DEPLOYMENT_TRANSACTION_ID_SIZE * 2)
#define DEPLOYMENT_JOURNAL_SUFFIX           L".bin"
#define DEPLOYMENT_ENUM_BUFFER_SIZE         (64u * 1024)
#define DEPLOYMENT_LOCK_POLL_MS             20
#define DEPLOYMENT_LOCK_BYTE_OFFSET         1

#define DEPLOYMENT_MARKER_VERSION           1
#define DEPLOYMENT_MARKER_HEADER_SIZE       40
#define MARKER_MAGIC_OFFSET                 0
#define MARKER_VERSION_OFFSET               4
#define MARKER_CONTENT_ID_OFFSET            8
#define MARKER_FULL_NAME_OFFSET             40

#define DEPLOYMENT_RECORD_HEADER_SIZE        160
#define RECORD_VERSION_OFFSET                8
#define RECORD_HEADER_SIZE_OFFSET            12
#define RECORD_TOTAL_SIZE_OFFSET             16
#define RECORD_LOADER_COUNT_OFFSET           20
#define RECORD_CLASS_COUNT_OFFSET            24
#define RECORD_APPLICATION_COUNT_OFFSET       28
#define RECORD_LOADERS_OFFSET                32
#define RECORD_CLASSES_OFFSET                36
#define RECORD_APPLICATIONS_OFFSET           40
#define RECORD_STRINGS_OFFSET                44
#define RECORD_STRINGS_SIZE_OFFSET           48
#define RECORD_RESERVED0_OFFSET              52
#define RECORD_CONTENT_ID_OFFSET             56
#define RECORD_DIGEST_OFFSET                 88
#define RECORD_FULL_NAME_REF_OFFSET          120
#define RECORD_PAYLOAD_PATH_REF_OFFSET       128
#define RECORD_PACKAGE_SEARCH_COUNT_OFFSET   136
#define RECORD_PACKAGE_SEARCH_FLAGS_OFFSET   140
#define RECORD_SEARCH_PATH_COUNT_OFFSET      144
#define RECORD_SEARCH_PATHS_OFFSET           148
#define RECORD_RESERVED_OFFSET               152
#define RECORD_RESERVED_SIZE                 \
    (DEPLOYMENT_RECORD_HEADER_SIZE - RECORD_RESERVED_OFFSET)
#define RECORD_SEARCH_OVERRIDE_PRESENT       0x00000001
#define RECORD_SEARCH_KNOWN_FLAGS            RECORD_SEARCH_OVERRIDE_PRESENT
#define DEPLOYMENT_RECORD_SEARCH_PATH_SIZE   8
#define DEPLOYMENT_RECORD_LOADER_SIZE        60
#define RECORD_LOADER_PATH_REF_OFFSET        0
#define RECORD_LOADER_VOLUME_SERIAL_OFFSET   8
#define RECORD_LOADER_FILE_INDEX_HIGH_OFFSET 12
#define RECORD_LOADER_FILE_INDEX_LOW_OFFSET  16
#define RECORD_LOADER_SIZE_OFFSET            20
#define RECORD_LOADER_DIGEST_OFFSET          28
#define DEPLOYMENT_RECORD_CLASS_SIZE         72
#define RECORD_CLASS_PATH_REF_OFFSET         0
#define RECORD_CLASS_ID_REF_OFFSET           8
#define RECORD_CLASS_THREADING_OFFSET        16
#define RECORD_CLASS_VOLUME_SERIAL_OFFSET    20
#define RECORD_CLASS_FILE_INDEX_HIGH_OFFSET  24
#define RECORD_CLASS_FILE_INDEX_LOW_OFFSET   28
#define RECORD_CLASS_SIZE_OFFSET             32
#define RECORD_CLASS_DIGEST_OFFSET           40
#define DEPLOYMENT_RECORD_APPLICATION_SIZE   108
#define RECORD_APPLICATION_ID_REF_OFFSET     0
#define RECORD_APPLICATION_EXEC_REF_OFFSET   8
#define RECORD_APPLICATION_ENTRY_REF_OFFSET  16
#define RECORD_APPLICATION_PARAMETERS_REF_OFFSET 24
#define RECORD_APPLICATION_CURRENT_DIR_REF_OFFSET 32
#define RECORD_APPLICATION_KIND_OFFSET       40
#define RECORD_APPLICATION_VOLUME_SERIAL_OFFSET 44
#define RECORD_APPLICATION_FILE_INDEX_HIGH_OFFSET 48
#define RECORD_APPLICATION_FILE_INDEX_LOW_OFFSET  52
#define RECORD_APPLICATION_SIZE_OFFSET       56
#define RECORD_APPLICATION_DIGEST_OFFSET     64
#define RECORD_APPLICATION_SEARCH_START_OFFSET 96
#define RECORD_APPLICATION_SEARCH_COUNT_OFFSET 100
#define RECORD_APPLICATION_SEARCH_FLAGS_OFFSET 104

static const BYTE deployment_record_magic[8] =
    {'S','W','X','D','E','P','L','Y'};

static const BYTE deployment_journal_magic[8] =
    {'S','W','X','T','R','A','N','S'};

struct object_identity
{
    DWORD volume_serial;
    DWORD file_index_high;
    DWORD file_index_low;
    DWORD attributes;
};

struct deployment_store
{
    WCHAR *path;
    HANDLE root;
    HANDLE transactions;
    HANDLE staging;
    HANDLE payloads;
    HANDLE quarantine;
    HANDLE records;
    HANDLE record_staging;
    HANDLE record_quarantine;
    HANDLE leases;
    HANDLE lease_pending;
    HANDLE lease_generations;
    HANDLE lock_file;
    struct object_identity root_identity;
    struct object_identity transactions_identity;
    struct object_identity staging_identity;
    struct object_identity payloads_identity;
    struct object_identity quarantine_identity;
    struct object_identity records_identity;
    struct object_identity record_staging_identity;
    struct object_identity record_quarantine_identity;
    struct object_identity leases_identity;
    struct object_identity lease_pending_identity;
    struct object_identity lease_generations_identity;
};

struct deployment_lock
{
    HANDLE file;
    OVERLAPPED overlapped;
    BOOL locked;
};

struct appx_deployment_result
{
    UINT32 version;
    UINT32 flags;
    UINT64 catalog_epoch;
    UINT64 reclaimed_bytes;
    UINT32 reclaimed_entries;
    WCHAR *package_full_name;
};

struct appx_deployment_record
{
    UINT32 size;
    UINT32 loader_count;
    UINT32 class_count;
    UINT32 application_count;
    UINT32 loader_search_path_count;
    UINT32 package_loader_search_path_count;
    BOOL has_package_loader_search_path_override;
    struct appx_deployment_loader_file *loaders;
    struct appx_deployment_inproc_class *classes;
    struct appx_deployment_application_file *applications;
    const WCHAR **loader_search_paths;
    BYTE data[1];
};

struct runtime_record
{
    const struct appx_catalog_package *package;
    APPX_DEPLOYMENT_RECORD *record;
};

struct appx_deployment_runtime
{
    APPX_CATALOG_SNAPSHOT *catalog;
    APPX_PACKAGE_GRAPH *graph;
    HANDLE *leases;
    UINT64 *lease_values;
    UINT32 lease_count;
    HANDLE executable;
    WCHAR *executable_path;
    WCHAR *parameters;
    WCHAR *current_directory;
    struct wine_appx_graph_attach attach;
};

struct deployment_backend
{
    const APPX_DEPLOYMENT_TEST_BACKEND *test;
    BOOL accept_weak_durability;
};

struct prepared_package
{
    APPX_DEPLOYMENT_TEST_PACKAGE test_view;
    APPX_PACKAGE_INSPECTION *package_inspection;
    APPX_BUNDLE_INSPECTION *bundle_inspection;
    const APPX_PACKAGE_INSPECTION *inspection;
    struct appx_catalog_application *applications;
    struct appx_catalog_dependency *dependencies;
    struct appx_deployment_loader_file *loader_files;
    struct appx_deployment_inproc_class *inproc_classes;
    const WCHAR *package_loader_search_paths[
        APPX_DEPLOYMENT_MAX_LOADER_SEARCH_PATHS];
    struct prepared_loader_search *application_loader_search;
    UINT32 package_loader_search_path_count;
    BOOL has_package_loader_search_path_override;
    WCHAR *payload_path;
    BOOL test_owned;
};

struct prepared_loader_search
{
    const WCHAR *paths[APPX_DEPLOYMENT_MAX_LOADER_SEARCH_PATHS];
    UINT32 count;
    BOOL present;
};

struct deployment_journal
{
    BYTE transaction_id[APPX_DEPLOYMENT_TRANSACTION_ID_SIZE];
    BYTE content_id[APPX_DEPLOYMENT_CONTENT_ID_SIZE];
    enum appx_deployment_operation operation;
    enum appx_deployment_journal_state state;
    UINT64 expected_epoch;
    WCHAR transaction_name[DEPLOYMENT_TRANSACTION_CHARS + 1];
    WCHAR journal_name[DEPLOYMENT_TRANSACTION_CHARS +
                       ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
    WCHAR *full_name;
    WCHAR *family_name;
    WCHAR *package_name;
    WCHAR *publisher;
    WCHAR *payload_path;
    WCHAR *staging_name;
};

struct delete_budget
{
    UINT64 max_bytes;
    UINT64 bytes;
    UINT32 max_entries;
    UINT32 entries;
};

struct gc_context
{
    const APPX_DEPLOYMENT_OPTIONS *options;
    struct deployment_backend *backend;
    struct deployment_store *store;
    const APPX_CATALOG_SNAPSHOT *catalog;
    struct delete_budget budget;
    UINT32 deferred;
    BOOL weak;
};

static HRESULT load_record_from_store(
    struct deployment_store *store,
    const struct appx_catalog_package *package,
    APPX_DEPLOYMENT_RECORD **record );
static HRESULT verify_record_inventory(
    struct deployment_store *store,
    const struct appx_catalog_package *package,
    const APPX_DEPLOYMENT_RECORD *record );
static HRESULT validate_store_snapshot(
    struct deployment_store *store,
    const APPX_CATALOG_SNAPSHOT *catalog );

static UINT16 read_uint16( const BYTE *data )
{
    return data[0] | ((UINT16)data[1] << 8);
}

static UINT32 read_uint32( const BYTE *data )
{
    return read_uint16( data ) | ((UINT32)read_uint16( data + 2 ) << 16);
}

static UINT64 read_uint64( const BYTE *data )
{
    return read_uint32( data ) | ((UINT64)read_uint32( data + 4 ) << 32);
}

static void write_uint16( BYTE *data, UINT16 value )
{
    data[0] = value;
    data[1] = value >> 8;
}

static void write_uint32( BYTE *data, UINT32 value )
{
    write_uint16( data, value );
    write_uint16( data + 2, value >> 16 );
}

static void write_uint64( BYTE *data, UINT64 value )
{
    write_uint32( data, value );
    write_uint32( data + 4, value >> 32 );
}

static BOOL add_uint32( UINT32 left, UINT32 right, UINT32 *result )
{
    if (MAXDWORD - left < right) return FALSE;
    *result = left + right;
    return TRUE;
}

static BOOL multiply_uint32( UINT32 left, UINT32 right, UINT32 *result )
{
    if (left && right > MAXDWORD / left) return FALSE;
    *result = left * right;
    return TRUE;
}

static HRESULT win32_error( DWORD error )
{
    return HRESULT_FROM_WIN32( error ? error : ERROR_GEN_FAILURE );
}

static HRESULT ntstatus_error( NTSTATUS status )
{
    return win32_error( RtlNtStatusToDosError( status ) );
}

static BOOL is_not_found( HRESULT hr )
{
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

static BOOL is_sharing_failure( HRESULT hr )
{
    return hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
           hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
           hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
           hr == APPX_DEPLOYMENT_E_PACKAGE_IN_USE;
}

static INT compare_string_ci( const WCHAR *left, const WCHAR *right )
{
    INT result = CompareStringOrdinal( left, -1, right, -1, TRUE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static INT compare_version( const struct appx_catalog_version *left,
                            const struct appx_catalog_version *right )
{
    if (left->major != right->major)
        return left->major < right->major ? -1 : 1;
    if (left->minor != right->minor)
        return left->minor < right->minor ? -1 : 1;
    if (left->build != right->build)
        return left->build < right->build ? -1 : 1;
    if (left->revision != right->revision)
        return left->revision < right->revision ? -1 : 1;
    return 0;
}

static BOOL equal_content_id( const BYTE *left, const BYTE *right )
{
    BYTE difference = 0;
    UINT32 i;

    for (i = 0; i < APPX_DEPLOYMENT_CONTENT_ID_SIZE; i++)
        difference |= left[i] ^ right[i];
    return !difference;
}

static INT compare_string_exact( const WCHAR *left, const WCHAR *right )
{
    INT result = CompareStringOrdinal( left, -1, right, -1, FALSE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static const WCHAR empty_stringW[] = L"";
static const BYTE generation_marker_magic[4] = {'S','W','L','M'};

static const WCHAR *optional_string( const WCHAR *string )
{
    return string ? string : empty_stringW;
}

static BOOL deployment_file_identity_is_valid(
    const struct appx_deployment_file_identity *identity )
{
    /*
     * Windows does not reserve zero as an invalid volume serial.  Wine also
     * reports zero while mountmgr is unavailable, even though the file index
     * remains stable.  Treat the index as the presence discriminator; every
     * security-sensitive consumer additionally checks the serial (including
     * zero), integrity metadata, and the object opened below the trusted root.
     */
    return identity->file_index_high || identity->file_index_low;
}

static void deployment_file_identity_from_object(
    struct appx_deployment_file_identity *output,
    const struct object_identity *identity )
{
    output->volume_serial = identity->volume_serial;
    output->file_index_high = identity->file_index_high;
    output->file_index_low = identity->file_index_low;
}

static BOOL object_identity_matches( const struct object_identity *expected,
                                     const struct object_identity *current )
{
    return expected->volume_serial == current->volume_serial &&
           expected->file_index_high == current->file_index_high &&
           expected->file_index_low == current->file_index_low;
}

static BOOL deployment_file_identity_matches_object(
    const struct appx_deployment_file_identity *expected,
    const struct object_identity *current )
{
    return expected->volume_serial == current->volume_serial &&
           expected->file_index_high == current->file_index_high &&
           expected->file_index_low == current->file_index_low;
}

static void write_file_identity(
    BYTE *data, UINT32 volume_offset, UINT32 high_offset, UINT32 low_offset,
    const struct appx_deployment_file_identity *identity )
{
    write_uint32( data + volume_offset, identity->volume_serial );
    write_uint32( data + high_offset, identity->file_index_high );
    write_uint32( data + low_offset, identity->file_index_low );
}

static HRESULT read_file_identity(
    const BYTE *data, UINT32 volume_offset, UINT32 high_offset,
    UINT32 low_offset, struct appx_deployment_file_identity *identity )
{
    identity->volume_serial = read_uint32( data + volume_offset );
    identity->file_index_high = read_uint32( data + high_offset );
    identity->file_index_low = read_uint32( data + low_offset );
    return deployment_file_identity_is_valid( identity ) ?
           S_OK : APPX_DEPLOYMENT_E_CORRUPT_STORE;
}

static void write_file_integrity(
    BYTE *data, UINT32 size_offset, UINT32 digest_offset,
    const struct appx_deployment_file_integrity *integrity )
{
    write_uint64( data + size_offset, integrity->size );
    memcpy( data + digest_offset, integrity->digest,
            sizeof(integrity->digest) );
}

static void read_file_integrity(
    const BYTE *data, UINT32 size_offset, UINT32 digest_offset,
    struct appx_deployment_file_integrity *integrity )
{
    integrity->size = read_uint64( data + size_offset );
    memcpy( integrity->digest, data + digest_offset,
            sizeof(integrity->digest) );
}

static BOOL deployment_file_integrity_matches(
    const struct appx_deployment_file_integrity *expected,
    const struct appx_deployment_file_integrity *current )
{
    return expected->size == current->size &&
           equal_content_id( expected->digest, current->digest );
}

static WCHAR ascii_upper( WCHAR ch )
{
    if (ch >= 'a' && ch <= 'z') return ch - ('a' - 'A');
    return ch;
}

static BOOL reserved_component_name( const WCHAR *string, UINT32 start,
                                     UINT32 end )
{
    WCHAR name[5] = {0};
    UINT32 i, base_end = start;

    while (base_end < end && string[base_end] != '.') base_end++;
    if (base_end - start < 3 || base_end - start > 4) return FALSE;
    for (i = start; i < base_end; i++)
        name[i - start] = ascii_upper( string[i] );
    if (base_end - start == 3)
        return (!memcmp( name, L"CON", 3 * sizeof(WCHAR) ) ||
                !memcmp( name, L"PRN", 3 * sizeof(WCHAR) ) ||
                !memcmp( name, L"AUX", 3 * sizeof(WCHAR) ) ||
                !memcmp( name, L"NUL", 3 * sizeof(WCHAR) ));
    return ((!memcmp( name, L"COM", 3 * sizeof(WCHAR) ) ||
             !memcmp( name, L"LPT", 3 * sizeof(WCHAR) )) &&
            name[3] >= '1' && name[3] <= '9');
}

static HRESULT bounded_string_length( const WCHAR *string, BOOL allow_empty,
                                      UINT32 *chars )
{
    UINT32 i;

    if (!string) return E_INVALIDARG;
    for (i = 0; i <= WINE_APPX_MAX_PATH_CHARS; i++)
    {
        WCHAR ch = string[i];

        if (!ch)
        {
            if (!allow_empty && !i) return APPX_E_INVALID_PACKAGING_LAYOUT;
            *chars = i + 1;
            return S_OK;
        }
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            if (i == WINE_APPX_MAX_PATH_CHARS ||
                string[++i] < 0xdc00 || string[i] > 0xdfff)
                return APPX_E_INVALID_PACKAGING_LAYOUT;
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    }
    return APPX_E_INVALID_PACKAGING_LAYOUT;
}

static HRESULT validate_relative_path( const WCHAR *path, BOOL basename_only )
{
    UINT32 chars, length, component_start = 0, i;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length( path, FALSE, &chars ))) return hr;
    length = chars - 1;
    if (path[0] == '\\' || path[0] == '/') return APPX_E_INVALID_PACKAGING_LAYOUT;
    for (i = 0; i <= length; i++)
    {
        WCHAR ch = i == length ? '\\' : path[i];

        if (ch == '/') return APPX_E_INVALID_PACKAGING_LAYOUT;
        if (ch != '\\')
        {
            if (ch < 0x20 || ch == ':' || ch == '"' || ch == '<' || ch == '>' ||
                ch == '|' || ch == '?' || ch == '*')
                return APPX_E_INVALID_PACKAGING_LAYOUT;
            continue;
        }
        if ((basename_only && i != length) || i == component_start ||
            path[i - 1] == ' ' || path[i - 1] == '.' ||
            i - component_start > WINE_APPX_MAX_COMPONENT_CHARS ||
            (i - component_start == 1 && path[component_start] == '.') ||
            (i - component_start == 2 && path[component_start] == '.' &&
             path[component_start + 1] == '.') ||
            reserved_component_name( path, component_start, i ))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        component_start = i + 1;
    }
    return S_OK;
}

static HRESULT validate_optional_string_max( const WCHAR *string,
                                             UINT32 maximum )
{
    UINT32 chars;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length(
            optional_string( string ), TRUE, &chars )))
        return hr;
    return chars <= maximum + 1 ? S_OK : APPX_E_INVALID_PACKAGING_LAYOUT;
}

static HRESULT validate_optional_relative_path( const WCHAR *path,
                                                UINT32 maximum )
{
    const WCHAR *value = optional_string( path );
    HRESULT hr;

    if (FAILED(hr = validate_optional_string_max( value, maximum )))
        return hr;
    return value[0] ? validate_relative_path( value, FALSE ) : S_OK;
}

static HRESULT validate_store_root( const WCHAR *path )
{
    UINT32 chars, length, component_start, i;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length( path, FALSE, &chars ))) return hr;
    length = chars - 1;
    if (length < 3 ||
        !((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) ||
        path[1] != ':' || path[2] != '\\' ||
        (length > 3 && path[length - 1] == '\\'))
        return E_INVALIDARG;
    if (length == 3) return S_OK;
    component_start = 3;
    for (i = 3; i <= length; i++)
    {
        WCHAR ch = i == length ? '\\' : path[i];

        if (ch == '/') return E_INVALIDARG;
        if (ch != '\\')
        {
            if (ch < 0x20 || ch == ':' || ch == '"' || ch == '<' || ch == '>' ||
                ch == '|' || ch == '?' || ch == '*')
                return E_INVALIDARG;
            continue;
        }
        if (i == component_start || path[i - 1] == ' ' || path[i - 1] == '.' ||
            i - component_start > WINE_APPX_MAX_COMPONENT_CHARS ||
            (i - component_start == 1 && path[component_start] == '.') ||
            (i - component_start == 2 && path[component_start] == '.' &&
             path[component_start + 1] == '.') ||
            reserved_component_name( path, component_start, i ))
            return E_INVALIDARG;
        component_start = i + 1;
    }
    return S_OK;
}

static HRESULT validate_options( const APPX_DEPLOYMENT_OPTIONS *input,
                                 APPX_DEPLOYMENT_OPTIONS *options )
{
    if (!input || input->size != sizeof(*input) ||
        input->version != APPX_DEPLOYMENT_OPTIONS_VERSION ||
        !input->store_root || (input->flags & ~APPX_DEPLOYMENT_KNOWN_FLAGS) ||
        !appx_architecture_is_valid( input->target_architecture ))
        return E_INVALIDARG;
    if (FAILED(validate_store_root( input->store_root ))) return E_INVALIDARG;
    if (input->writer_timeout_ms > APPX_DEPLOYMENT_MAX_LOCK_TIMEOUT_MS ||
        input->max_epoch_retries > APPX_DEPLOYMENT_MAX_EPOCH_RETRIES ||
        input->max_gc_entries > APPX_DEPLOYMENT_MAX_GC_ENTRIES)
        return E_INVALIDARG;
    if (input->archive_limits &&
        input->archive_limits->size != sizeof(*input->archive_limits))
        return E_INVALIDARG;

    *options = *input;
    if (!options->writer_timeout_ms)
        options->writer_timeout_ms = APPX_DEPLOYMENT_DEFAULT_LOCK_TIMEOUT_MS;
    if (!options->max_epoch_retries)
        options->max_epoch_retries = APPX_DEPLOYMENT_DEFAULT_EPOCH_RETRIES;
    if (!options->max_gc_entries)
        options->max_gc_entries = APPX_DEPLOYMENT_DEFAULT_GC_ENTRIES;
    if (!options->max_gc_bytes)
        options->max_gc_bytes = APPX_DEPLOYMENT_DEFAULT_GC_BYTES;
    return S_OK;
}

static HRESULT validate_backend( const APPX_DEPLOYMENT_TEST_BACKEND *backend )
{
    if (!backend) return S_OK;
    if (backend->size != sizeof(*backend) ||
        backend->version != APPX_DEPLOYMENT_TEST_BACKEND_VERSION)
        return E_INVALIDARG;
    return S_OK;
}

static HRESULT check_cancelled( const APPX_DEPLOYMENT_OPTIONS *options )
{
    DWORD wait;

    if (!options->cancel_event) return S_OK;
    wait = WaitForSingleObject( options->cancel_event, 0 );
    if (wait == WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    if (wait == WAIT_TIMEOUT) return S_OK;
    return win32_error( GetLastError() );
}

static HRESULT duplicate_string( const WCHAR *source, BOOL allow_empty,
                                 WCHAR **destination )
{
    UINT32 chars;
    HRESULT hr;

    *destination = NULL;
    if (FAILED(hr = bounded_string_length( source, allow_empty, &chars )))
        return hr;
    if (!multiply_uint32( chars, sizeof(WCHAR), &chars ))
        return E_OUTOFMEMORY;
    if (!(*destination = HeapAlloc( GetProcessHeap(), 0, chars )))
        return E_OUTOFMEMORY;
    memcpy( *destination, source, chars );
    return S_OK;
}

static HRESULT get_identity( HANDLE handle, BOOL directory,
                             struct object_identity *identity,
                             DWORD *links, UINT64 *size )
{
    BY_HANDLE_FILE_INFORMATION info;

    if (!GetFileInformationByHandle( handle, &info ))
    {
        TRACE( "failed to query identity for handle %p, error %lu.\n",
               handle, GetLastError() );
        return win32_error( GetLastError() );
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        TRACE( "rejected reparse-point handle %p, attributes %#lx.\n",
               handle, info.dwFileAttributes );
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    }
    if (!!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory)
    {
        TRACE( "handle %p has attributes %#lx, expected directory %u.\n",
               handle, info.dwFileAttributes, directory );
        return HRESULT_FROM_WIN32( directory ? ERROR_DIRECTORY :
                                  ERROR_ACCESS_DENIED );
    }
    if (identity)
    {
        identity->volume_serial = info.dwVolumeSerialNumber;
        identity->file_index_high = info.nFileIndexHigh;
        identity->file_index_low = info.nFileIndexLow;
        identity->attributes = info.dwFileAttributes;
    }
    if (links) *links = info.nNumberOfLinks;
    if (size)
        *size = info.nFileSizeLow | ((UINT64)info.nFileSizeHigh << 32);
    return S_OK;
}

static HRESULT verify_identity( HANDLE handle, BOOL directory,
                                const struct object_identity *expected )
{
    struct object_identity current;
    HRESULT hr;

    if (FAILED(hr = get_identity( handle, directory, &current, NULL, NULL )))
        return hr;
    if (current.volume_serial != expected->volume_serial ||
        current.file_index_high != expected->file_index_high ||
        current.file_index_low != expected->file_index_low)
    {
        TRACE( "object identity changed: expected volume %#lx "
               "index %08lx:%08lx, current volume %#lx "
               "index %08lx:%08lx.\n",
               expected->volume_serial, expected->file_index_high,
               expected->file_index_low, current.volume_serial,
               current.file_index_high, current.file_index_low );
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    }
    return S_OK;
}

static HRESULT require_single_link_file( HANDLE file )
{
    DWORD links;
    HRESULT hr;

    if (FAILED(hr = get_identity(
            file, FALSE, NULL, &links, NULL )))
        return hr;
    return links == 1 ? S_OK :
           HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
}

static void close_handle( HANDLE *handle )
{
    if (*handle && *handle != INVALID_HANDLE_VALUE) CloseHandle( *handle );
    *handle = INVALID_HANDLE_VALUE;
}

static void close_store( struct deployment_store *store )
{
    close_handle( &store->lock_file );
    close_handle( &store->lease_generations );
    close_handle( &store->lease_pending );
    close_handle( &store->leases );
    close_handle( &store->quarantine );
    close_handle( &store->record_quarantine );
    close_handle( &store->record_staging );
    close_handle( &store->records );
    close_handle( &store->payloads );
    close_handle( &store->staging );
    close_handle( &store->transactions );
    close_handle( &store->root );
    HeapFree( GetProcessHeap(), 0, store->path );
    memset( store, 0, sizeof(*store) );
    store->root = store->transactions = store->staging = store->payloads =
        store->quarantine = store->records = store->record_staging =
        store->record_quarantine = store->leases = store->lease_pending =
        store->lease_generations = store->lock_file = INVALID_HANDLE_VALUE;
}

static HRESULT open_child( HANDLE root, const WCHAR *name, ACCESS_MASK access,
                           ULONG sharing, ULONG disposition, BOOL directory,
                           HANDLE *handle, struct object_identity *identity )
{
    UNICODE_STRING child_name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    HRESULT hr;

    *handle = INVALID_HANDLE_VALUE;
    if (FAILED(hr = validate_relative_path( name, TRUE ))) return hr;
    RtlInitUnicodeString( &child_name, name );
    InitializeObjectAttributes( &attributes, &child_name, OBJ_CASE_INSENSITIVE,
                                root, NULL );
    status = NtCreateFile( handle, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           &attributes, &io, NULL,
                           directory ? FILE_ATTRIBUTE_DIRECTORY :
                                       FILE_ATTRIBUTE_NORMAL,
                           sharing, disposition,
                           (directory ? FILE_DIRECTORY_FILE :
                                        FILE_NON_DIRECTORY_FILE) |
                           FILE_OPEN_REPARSE_POINT |
                           FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0 );
    if (status)
    {
        if (status != STATUS_OBJECT_NAME_NOT_FOUND &&
            status != STATUS_OBJECT_PATH_NOT_FOUND &&
            status != STATUS_NO_SUCH_FILE)
            TRACE( "failed to open child %s, access %#lx, sharing %#lx, "
                   "disposition %lu, directory %u, status %#lx.\n",
                   debugstr_w(name), access, sharing, disposition, directory,
                   status );
        return ntstatus_error( status );
    }
    if (FAILED(hr = get_identity( *handle, directory, identity, NULL, NULL )))
    {
        CloseHandle( *handle );
        *handle = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static HRESULT open_path_component( HANDLE root, const WCHAR *name,
                                    BOOL create, HANDLE *handle,
                                    struct object_identity *identity );
static const WCHAR *payload_generation_name( const WCHAR *payload_path );
static HRESULT calculate_file_integrity(
    HANDLE file, const struct object_identity *expected_identity,
    struct appx_deployment_file_integrity *integrity );
static HRESULT delete_named_file(
    HANDLE parent, const WCHAR *name, struct delete_budget *budget,
    struct deployment_backend *backend, BOOL *weak );

static HRESULT open_payload_file( HANDLE generation, const WCHAR *path,
                                  HANDLE *file,
                                  struct object_identity *file_identity )
{
    WCHAR component[WINE_APPX_MAX_COMPONENT_CHARS + 1];
    struct object_identity identity;
    HANDLE current = INVALID_HANDLE_VALUE, child = INVALID_HANDLE_VALUE;
    UINT32 start = 0, end, length = lstrlenW( path );
    HRESULT hr;

    *file = INVALID_HANDLE_VALUE;
    if (FAILED(hr = validate_relative_path( path, FALSE ))) return hr;
    if (!DuplicateHandle( GetCurrentProcess(), generation,
                          GetCurrentProcess(), &current, 0, FALSE,
                          DUPLICATE_SAME_ACCESS ))
        return win32_error( GetLastError() );
    while (start < length)
    {
        for (end = start; end < length && path[end] != '\\'; end++);
        memcpy( component, path + start, (end - start) * sizeof(WCHAR) );
        component[end - start] = 0;
        if (end == length)
        {
            hr = open_child(
                current, component, FILE_READ_DATA,
                FILE_SHARE_READ,
                FILE_OPEN, FALSE, &child, &identity );
            if (SUCCEEDED(hr))
                hr = require_single_link_file( child );
            if (SUCCEEDED(hr) && FAILED(hr = verify_identity(
                    child, FALSE, &identity )))
                hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
            if (SUCCEEDED(hr) && file_identity)
                *file_identity = identity;
        }
        else
            hr = open_path_component( current, component, FALSE,
                                      &child, &identity );
        if (FAILED(hr)) goto done;
        close_handle( &current );
        current = child;
        child = INVALID_HANDLE_VALUE;
        start = end + 1;
    }
    *file = current;
    current = INVALID_HANDLE_VALUE;
    hr = S_OK;

done:
    close_handle( &child );
    close_handle( &current );
    return hr;
}

static HRESULT verify_payload_directory( HANDLE generation, const WCHAR *path )
{
    WCHAR component[WINE_APPX_MAX_COMPONENT_CHARS + 1];
    struct object_identity identity;
    HANDLE current = INVALID_HANDLE_VALUE, child = INVALID_HANDLE_VALUE;
    UINT32 start = 0, end, length;
    HRESULT hr;

    path = optional_string( path );
    if (!path[0]) return S_OK;
    if (FAILED(hr = validate_relative_path( path, FALSE ))) return hr;
    length = lstrlenW( path );
    if (!DuplicateHandle( GetCurrentProcess(), generation,
                          GetCurrentProcess(), &current, 0, FALSE,
                          DUPLICATE_SAME_ACCESS ))
        return win32_error( GetLastError() );
    while (start < length)
    {
        for (end = start; end < length && path[end] != '\\'; end++);
        memcpy( component, path + start, (end - start) * sizeof(WCHAR) );
        component[end - start] = 0;
        if (FAILED(hr = open_path_component( current, component, FALSE,
                                             &child, &identity )))
            goto done;
        close_handle( &current );
        current = child;
        child = INVALID_HANDLE_VALUE;
        start = end + 1;
    }
    hr = verify_identity( current, TRUE, &identity );

done:
    close_handle( &child );
    close_handle( &current );
    return hr;
}

static HRESULT capture_payload_file_state(
    HANDLE generation, const WCHAR *path,
    struct appx_deployment_file_identity *identity,
    struct appx_deployment_file_integrity *integrity )
{
    struct object_identity current;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    if (FAILED(hr = open_payload_file(
            generation, path, &file, &current )))
        return hr;
    hr = calculate_file_integrity( file, &current, integrity );
    close_handle( &file );
    if (FAILED(hr)) return hr;
    deployment_file_identity_from_object( identity, &current );
    if (!deployment_file_identity_is_valid( identity ))
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    return S_OK;
}

static HRESULT verify_payload_file_state(
    HANDLE generation, const WCHAR *path,
    const struct appx_deployment_file_identity *expected_identity,
    const struct appx_deployment_file_integrity *expected_integrity )
{
    struct appx_deployment_file_integrity current_integrity;
    struct object_identity current;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    if (!deployment_file_identity_is_valid( expected_identity ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = open_payload_file(
            generation, path, &file, &current )))
        goto done;
    if (!deployment_file_identity_matches_object( expected_identity,
                                                  &current ))
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    if (SUCCEEDED(hr))
        hr = calculate_file_integrity( file, &current, &current_integrity );
    if (SUCCEEDED(hr) && !deployment_file_integrity_matches(
            expected_integrity, &current_integrity ))
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );

done:
    close_handle( &file );
    return hr;
}

static HRESULT verify_record_inventory(
    struct deployment_store *store,
    const struct appx_catalog_package *package,
    const APPX_DEPLOYMENT_RECORD *record )
{
    const WCHAR *generation = payload_generation_name( package->payload_path );
    struct object_identity identity;
    HANDLE directory = INVALID_HANDLE_VALUE;
    UINT32 i, j;
    HRESULT hr;

    if (!generation) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = open_path_component(
            store->payloads, generation, FALSE,
            &directory, &identity )))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    for (i = 0; i < record->loader_count; i++)
    {
        hr = verify_payload_file_state(
            directory, record->loaders[i].relative_path,
            &record->loaders[i].identity,
            &record->loaders[i].integrity );
        if (FAILED(hr))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
    }
    for (i = 0; i < record->class_count; i++)
    {
        hr = verify_payload_file_state(
            directory, record->classes[i].path,
            &record->classes[i].identity,
            &record->classes[i].integrity );
        if (FAILED(hr))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
    }
    if (record->application_count != package->application_count)
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    for (i = 0; i < package->application_count; i++)
    {
        const struct appx_catalog_application *application =
            package->applications + i;
        const struct appx_deployment_application_file *file = NULL;

        for (j = 0; j < record->application_count; j++)
        {
            if (compare_string_exact( application->id,
                                      record->applications[j].id ) < 0)
                break;
            if (!compare_string_exact( application->id,
                                       record->applications[j].id ))
            {
                file = record->applications + j;
                break;
            }
        }
        if (!file ||
            lstrcmpW( file->executable, application->executable ) ||
            lstrcmpW( file->entry_point,
                      optional_string( application->entry_point ) ) ||
            lstrcmpW( file->parameters,
                      optional_string( application->parameters ) ) ||
            lstrcmpW( file->current_directory_path,
                      optional_string(
                          application->current_directory_path ) ) ||
            file->activation_kind != application->activation_kind)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
        hr = verify_payload_file_state(
            directory, file->executable, &file->identity,
            &file->integrity );
        if (FAILED(hr))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
        hr = verify_payload_directory(
            directory, file->current_directory_path );
        if (FAILED(hr))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
    }
    hr = verify_identity( directory, TRUE, &identity );

done:
    close_handle( &directory );
    return hr;
}

static HRESULT open_path_component( HANDLE root, const WCHAR *name,
                                    BOOL create, HANDLE *handle,
                                    struct object_identity *identity )
{
    return open_child( root, name,
                       FILE_LIST_DIRECTORY | FILE_ADD_FILE |
                       FILE_ADD_SUBDIRECTORY | FILE_TRAVERSE | DELETE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       create ? FILE_OPEN_IF : FILE_OPEN, TRUE, handle, identity );
}

static HRESULT open_absolute_store_root( const WCHAR *path, BOOL create,
                                         HANDLE *root,
                                         struct object_identity *identity )
{
    WCHAR drive[4] = {path[0], ':', '\\', 0};
    WCHAR component[WINE_APPX_MAX_COMPONENT_CHARS + 1];
    UNICODE_STRING nt_path;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    struct object_identity current_identity;
    HANDLE current = INVALID_HANDLE_VALUE, child = INVALID_HANDLE_VALUE;
    UINT32 start, end, length = lstrlenW( path );
    HRESULT hr;

    *root = INVALID_HANDLE_VALUE;
    status = RtlDosPathNameToNtPathName_U_WithStatus( drive, &nt_path,
                                                      NULL, NULL );
    if (status) return ntstatus_error( status );
    InitializeObjectAttributes( &attributes, &nt_path, OBJ_CASE_INSENSITIVE,
                                NULL, NULL );
    status = NtCreateFile( &current,
                           FILE_LIST_DIRECTORY | FILE_ADD_SUBDIRECTORY |
                           FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           &attributes, &io, NULL, FILE_ATTRIBUTE_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_OPEN, FILE_DIRECTORY_FILE |
                           FILE_OPEN_REPARSE_POINT |
                           FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0 );
    RtlFreeUnicodeString( &nt_path );
    if (status) return ntstatus_error( status );
    if (FAILED(hr = get_identity( current, TRUE, &current_identity, NULL, NULL )))
        goto failed;
    if (length == 3)
    {
        *root = current;
        *identity = current_identity;
        return S_OK;
    }

    for (start = 3; start < length; start = end + 1)
    {
        for (end = start; end < length && path[end] != '\\'; end++);
        memcpy( component, path + start, (end - start) * sizeof(WCHAR) );
        component[end - start] = 0;
        if (FAILED(hr = open_path_component( current, component, create,
                                             &child, &current_identity )))
            goto failed;
        CloseHandle( current );
        current = child;
        child = INVALID_HANDLE_VALUE;
    }
    *root = current;
    *identity = current_identity;
    return S_OK;

failed:
    close_handle( &child );
    close_handle( &current );
    return hr;
}

static HRESULT backend_flush( struct deployment_backend *backend,
                              HANDLE handle, BOOL directory )
{
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    if (backend->test && backend->test->flush)
        return backend->test->flush( backend->test->context, handle,
                                     directory );
    if (!directory)
        return FlushFileBuffers( handle ) ? S_OK : win32_error( GetLastError() );
    status = NtFlushBuffersFile( handle, &io );
    if (!status) return S_OK;
    if (status == STATUS_INVALID_DEVICE_REQUEST ||
        status == STATUS_NOT_IMPLEMENTED || status == STATUS_NOT_SUPPORTED)
        return APPX_DEPLOYMENT_S_WEAK_DURABILITY;
    WARN( "Directory flush failed, status %#lx.\n", status );
    return ntstatus_error( status );
}

static HRESULT mark_durability( struct deployment_backend *backend,
                                HRESULT hr, BOOL *weak )
{
    if (hr == S_FALSE)
    {
        if (!backend->accept_weak_durability)
            return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
        *weak = TRUE;
        return S_OK;
    }
    return hr;
}

static HRESULT create_store_subdirectory(
    struct deployment_backend *backend, HANDLE parent, const WCHAR *name,
    BOOL create, HANDLE *handle, struct object_identity *identity, BOOL *weak )
{
    HRESULT hr;

    if (FAILED(hr = open_path_component( parent, name, create, handle,
                                         identity )))
        return hr;
    if (create &&
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, parent, TRUE ), weak )))
    {
        close_handle( handle );
        return hr;
    }
    return S_OK;
}

static HRESULT open_store( const APPX_DEPLOYMENT_OPTIONS *options, BOOL create,
                           struct deployment_backend *backend,
                           struct deployment_store *store, BOOL *weak )
{
    HRESULT hr;

    memset( store, 0, sizeof(*store) );
    store->root = store->transactions = store->staging = store->payloads =
        store->quarantine = store->records = store->record_staging =
        store->record_quarantine = store->leases = store->lease_pending =
        store->lease_generations = store->lock_file = INVALID_HANDLE_VALUE;
    if (FAILED(hr = duplicate_string( options->store_root, FALSE,
                                      &store->path )))
        return hr;
    if (FAILED(hr = open_absolute_store_root( options->store_root, create,
                                              &store->root,
                                              &store->root_identity )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->root, DEPLOYMENT_TRANSACTIONS_NAME, create,
            &store->transactions, &store->transactions_identity, weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->root, DEPLOYMENT_STAGING_NAME, create,
            &store->staging, &store->staging_identity, weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->root, DEPLOYMENT_PAYLOADS_NAME, create,
            &store->payloads, &store->payloads_identity, weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->root, DEPLOYMENT_QUARANTINE_NAME, create,
            &store->quarantine, &store->quarantine_identity, weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->root, DEPLOYMENT_RECORDS_NAME, create,
            &store->records, &store->records_identity, weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->root, DEPLOYMENT_RECORD_STAGING_NAME, create,
            &store->record_staging, &store->record_staging_identity, weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->root, DEPLOYMENT_RECORD_QUARANTINE_NAME, create,
            &store->record_quarantine, &store->record_quarantine_identity,
            weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->root, DEPLOYMENT_LEASES_NAME, create,
            &store->leases, &store->leases_identity, weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->leases, DEPLOYMENT_LEASE_PENDING_NAME, create,
            &store->lease_pending, &store->lease_pending_identity, weak )) ||
        FAILED(hr = create_store_subdirectory(
            backend, store->leases, DEPLOYMENT_LEASE_GENERATIONS_NAME, create,
            &store->lease_generations, &store->lease_generations_identity,
            weak )))
        goto failed;
    if (FAILED(hr = open_child(
            store->root, DEPLOYMENT_STORE_LOCK_NAME,
            FILE_READ_DATA | FILE_WRITE_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            create ? FILE_OPEN_IF : FILE_OPEN, FALSE, &store->lock_file, NULL )))
        goto failed;
    if (FAILED(hr = require_single_link_file( store->lock_file )))
        goto failed;
    if (FAILED(hr = verify_identity( store->root, TRUE,
                                     &store->root_identity )) ||
        FAILED(hr = verify_identity( store->transactions, TRUE,
                                     &store->transactions_identity )) ||
        FAILED(hr = verify_identity( store->staging, TRUE,
                                     &store->staging_identity )) ||
        FAILED(hr = verify_identity( store->payloads, TRUE,
                                     &store->payloads_identity )) ||
        FAILED(hr = verify_identity( store->quarantine, TRUE,
                                     &store->quarantine_identity )) ||
        FAILED(hr = verify_identity( store->records, TRUE,
                                     &store->records_identity )) ||
        FAILED(hr = verify_identity( store->record_staging, TRUE,
                                     &store->record_staging_identity )) ||
        FAILED(hr = verify_identity( store->record_quarantine, TRUE,
                                     &store->record_quarantine_identity )) ||
        FAILED(hr = verify_identity( store->leases, TRUE,
                                     &store->leases_identity )) ||
        FAILED(hr = verify_identity( store->lease_pending, TRUE,
                                     &store->lease_pending_identity )) ||
        FAILED(hr = verify_identity( store->lease_generations, TRUE,
                                     &store->lease_generations_identity )))
        goto failed;
    if (FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->root, TRUE ), weak )))
        goto failed;
    return S_OK;

failed:
    close_store( store );
    return hr;
}

static HRESULT checkpoint( struct deployment_backend *backend,
                           enum appx_deployment_test_checkpoint point )
{
    if (backend->test && backend->test->checkpoint)
        return backend->test->checkpoint( backend->test->context, point );
    return S_OK;
}

static HRESULT acquire_lock_byte( struct deployment_store *store,
                                  const APPX_DEPLOYMENT_OPTIONS *options,
                                  DWORD offset,
                                  struct deployment_lock *lock )
{
    ULONGLONG start = GetTickCount64(), elapsed;
    DWORD error, wait;

    memset( lock, 0, sizeof(*lock) );
    lock->file = store->lock_file;
    lock->overlapped.Offset = offset;
    for (;;)
    {
        if (LockFileEx( lock->file, LOCKFILE_EXCLUSIVE_LOCK |
                       LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                       &lock->overlapped ))
        {
            lock->locked = TRUE;
            return S_OK;
        }
        error = GetLastError();
        if (error != ERROR_LOCK_VIOLATION && error != ERROR_IO_PENDING)
            return win32_error( error );
        if (FAILED(check_cancelled( options )))
            return HRESULT_FROM_WIN32( ERROR_CANCELLED );
        elapsed = GetTickCount64() - start;
        if (elapsed >= options->writer_timeout_ms)
            return APPX_DEPLOYMENT_E_LOCK_TIMEOUT;
        wait = min( DEPLOYMENT_LOCK_POLL_MS,
                    options->writer_timeout_ms - (DWORD)elapsed );
        if (options->cancel_event)
        {
            DWORD status = WaitForSingleObject( options->cancel_event, wait );

            if (status == WAIT_OBJECT_0)
                return HRESULT_FROM_WIN32( ERROR_CANCELLED );
            if (status == WAIT_FAILED)
                return win32_error( GetLastError() );
        }
        else
            Sleep( wait );
    }
}

static HRESULT acquire_writer_lock( struct deployment_store *store,
                                    const APPX_DEPLOYMENT_OPTIONS *options,
                                    struct deployment_lock *lock )
{
    /*
     * Byte zero belongs to catalog.c.  Its load and publish entry points take
     * that lock internally, so deployment writers use the adjacent byte and
     * rely on the catalog epoch compare-and-swap for cross-API conflicts.
     */
    return acquire_lock_byte( store, options, DEPLOYMENT_LOCK_BYTE_OFFSET,
                              lock );
}

static void release_store_lock( struct deployment_lock *lock )
{
    if (lock->locked)
        UnlockFileEx( lock->file, 0, 1, 0, &lock->overlapped );
    memset( lock, 0, sizeof(*lock) );
}

struct sha256_context
{
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
};

static HRESULT sha256_init( struct sha256_context *context )
{
    NTSTATUS status;

    memset( context, 0, sizeof(*context) );
    status = BCryptOpenAlgorithmProvider( &context->algorithm,
                                          BCRYPT_SHA256_ALGORITHM, NULL, 0 );
    if (status) return HRESULT_FROM_NT( status );
    status = BCryptCreateHash( context->algorithm, &context->hash,
                               NULL, 0, NULL, 0, 0 );
    if (status)
    {
        BCryptCloseAlgorithmProvider( context->algorithm, 0 );
        context->algorithm = NULL;
        return HRESULT_FROM_NT( status );
    }
    return S_OK;
}

static void sha256_destroy( struct sha256_context *context )
{
    if (context->hash) BCryptDestroyHash( context->hash );
    if (context->algorithm) BCryptCloseAlgorithmProvider( context->algorithm, 0 );
    memset( context, 0, sizeof(*context) );
}

static HRESULT sha256_update( struct sha256_context *context,
                              const void *data, UINT32 size )
{
    NTSTATUS status;

    if (size && (status = BCryptHashData( context->hash, (BYTE *)data,
                                         size, 0 )))
        return HRESULT_FROM_NT( status );
    return S_OK;
}

static HRESULT sha256_finish( struct sha256_context *context,
                              BYTE digest[DEPLOYMENT_DIGEST_SIZE] )
{
    NTSTATUS status = BCryptFinishHash( context->hash, digest,
                                        DEPLOYMENT_DIGEST_SIZE, 0 );

    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

static HRESULT hash_with_zero_range( const BYTE *data, UINT32 size,
                                     UINT32 zero_offset, UINT32 zero_size,
                                     BYTE digest[DEPLOYMENT_DIGEST_SIZE] )
{
    static const BYTE zeros[DEPLOYMENT_DIGEST_SIZE] = {0};
    struct sha256_context hash;
    HRESULT hr;

    if (zero_size > sizeof(zeros) || zero_offset > size ||
        zero_size > size - zero_offset)
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = sha256_init( &hash ))) return hr;
    if (SUCCEEDED(hr))
        hr = sha256_update( &hash, data, zero_offset );
    if (SUCCEEDED(hr))
        hr = sha256_update( &hash, zeros, zero_size );
    if (SUCCEEDED(hr))
        hr = sha256_update( &hash, data + zero_offset + zero_size,
                            size - zero_offset - zero_size );
    if (SUCCEEDED(hr))
        hr = sha256_finish( &hash, digest );
    sha256_destroy( &hash );
    return hr;
}

static HRESULT calculate_file_integrity(
    HANDLE file, const struct object_identity *expected_identity,
    struct appx_deployment_file_integrity *integrity )
{
    BYTE buffer[DEPLOYMENT_ENUM_BUFFER_SIZE];
    struct object_identity identity;
    struct sha256_context hash;
    LARGE_INTEGER zero;
    DWORD links, read;
    UINT64 size;
    HRESULT hr;

    if (FAILED(hr = get_identity( file, FALSE, &identity, &links, &size )) ||
        links != 1)
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (expected_identity && !object_identity_matches(
            expected_identity, &identity ))
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    zero.QuadPart = 0;
    if (!SetFilePointerEx( file, zero, NULL, FILE_BEGIN ))
        return win32_error( GetLastError() );
    if (FAILED(hr = sha256_init( &hash ))) return hr;
    for (;;)
    {
        if (!ReadFile( file, buffer, sizeof(buffer), &read, NULL ))
        {
            hr = win32_error( GetLastError() );
            break;
        }
        if (!read) break;
        if (FAILED(hr = sha256_update( &hash, buffer, read ))) break;
    }
    if (SUCCEEDED(hr))
    {
        integrity->size = size;
        hr = sha256_finish( &hash, integrity->digest );
    }
    sha256_destroy( &hash );
    if (SUCCEEDED(hr) &&
        FAILED(hr = verify_identity( file, FALSE, &identity )))
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    return hr;
}

static HRESULT backend_random( struct deployment_backend *backend,
                               BYTE *data, UINT32 size )
{
    NTSTATUS status;

    if (backend->test && backend->test->random)
        return backend->test->random( backend->test->context, data, size );
    status = BCryptGenRandom( NULL, data, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG );
    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

static void bytes_to_hex( const BYTE *bytes, UINT32 count, WCHAR *hex )
{
    static const WCHAR digits[] = L"0123456789abcdef";
    UINT32 i;

    for (i = 0; i < count; i++)
    {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    hex[count * 2] = 0;
}

static BOOL is_lower_hex_name( const WCHAR *name, UINT32 chars )
{
    UINT32 i;

    if (lstrlenW( name ) != chars) return FALSE;
    for (i = 0; i < chars; i++)
        if (!((name[i] >= '0' && name[i] <= '9') ||
              (name[i] >= 'a' && name[i] <= 'f')))
            return FALSE;
    return TRUE;
}

static HRESULT derive_payload_path( const WCHAR *full_name,
                                    const BYTE content_id[
                                        APPX_DEPLOYMENT_CONTENT_ID_SIZE],
                                    WCHAR **payload_path )
{
    static const WCHAR prefix[] = L"payloads\\";
    struct sha256_context hash;
    BYTE digest[DEPLOYMENT_PATH_HASH_SIZE];
    WCHAR *folded = NULL, *result = NULL;
    UINT32 chars, bytes, result_chars;
    HRESULT hr;

    *payload_path = NULL;
    if (FAILED(hr = bounded_string_length( full_name, FALSE, &chars )))
        return hr;
    if (!multiply_uint32( chars - 1, sizeof(WCHAR), &bytes ))
        return E_OUTOFMEMORY;
    if (!(folded = HeapAlloc( GetProcessHeap(), 0,
                              chars * sizeof(*folded) )))
        return E_OUTOFMEMORY;
    if (!LCMapStringEx( LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
                        full_name, chars, folded, chars,
                        NULL, NULL, 0 ))
    {
        hr = win32_error( GetLastError() );
        goto done;
    }
    if (FAILED(hr = sha256_init( &hash ))) goto done;
    if (SUCCEEDED(hr))
        hr = sha256_update( &hash, folded, bytes );
    if (SUCCEEDED(hr))
        hr = sha256_update( &hash, content_id,
                            APPX_DEPLOYMENT_CONTENT_ID_SIZE );
    if (SUCCEEDED(hr))
        hr = sha256_finish( &hash, digest );
    sha256_destroy( &hash );
    if (FAILED(hr)) goto done;

    result_chars = ARRAY_SIZE(prefix) - 1 + DEPLOYMENT_GENERATION_CHARS + 1;
    if (!(result = HeapAlloc( GetProcessHeap(), 0,
                              result_chars * sizeof(*result) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    memcpy( result, prefix, (ARRAY_SIZE(prefix) - 1) * sizeof(*result) );
    bytes_to_hex( digest, sizeof(digest),
                  result + ARRAY_SIZE(prefix) - 1 );
    *payload_path = result;
    result = NULL;
    hr = S_OK;

done:
    HeapFree( GetProcessHeap(), 0, result );
    HeapFree( GetProcessHeap(), 0, folded );
    return hr;
}

static const WCHAR *payload_generation_name( const WCHAR *payload_path )
{
    static const WCHAR prefix[] = L"payloads\\";

    if (wcsncmp( payload_path, prefix, ARRAY_SIZE(prefix) - 1 ))
        return NULL;
    payload_path += ARRAY_SIZE(prefix) - 1;
    return is_lower_hex_name( payload_path, DEPLOYMENT_GENERATION_CHARS ) ?
           payload_path : NULL;
}

static HRESULT backend_write_all( struct deployment_backend *backend,
                                  HANDLE file, const BYTE *data, UINT32 size )
{
    UINT32 offset = 0;

    while (offset < size)
    {
        DWORD written = 0;
        BOOL success;

        if (backend->test && backend->test->write)
            success = backend->test->write( backend->test->context, file,
                                             data + offset, size - offset,
                                             &written );
        else
            success = WriteFile( file, data + offset, size - offset,
                                 &written, NULL );
        if (!success) return win32_error( GetLastError() );
        if (!written || written > size - offset)
            return HRESULT_FROM_WIN32( ERROR_WRITE_FAULT );
        offset += written;
    }
    return S_OK;
}

static HRESULT backend_rename( struct deployment_backend *backend,
                               HANDLE object, HANDLE destination_root,
                               const WCHAR *destination, BOOL replace )
{
    FILE_RENAME_INFORMATION *rename;
    IO_STATUS_BLOCK io;
    UINT32 chars, name_bytes, size;
    NTSTATUS status;
    HRESULT hr;

    if (FAILED(hr = validate_relative_path( destination, TRUE ))) return hr;
    if (backend->test && backend->test->rename)
        return backend->test->rename( backend->test->context, object,
                                      destination_root, destination, replace );
    chars = lstrlenW( destination );
    if (!multiply_uint32( chars, sizeof(WCHAR), &name_bytes ) ||
        !add_uint32( offsetof(FILE_RENAME_INFORMATION, FileName),
                     name_bytes, &size ))
        return E_OUTOFMEMORY;
    if (!(rename = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, size )))
        return E_OUTOFMEMORY;
    rename->ReplaceIfExists = replace;
    rename->RootDirectory = destination_root;
    rename->FileNameLength = name_bytes;
    memcpy( rename->FileName, destination, name_bytes );
    status = NtSetInformationFile( object, &io, rename, size,
                                   FileRenameInformation );
    HeapFree( GetProcessHeap(), 0, rename );
    return status ? ntstatus_error( status ) : S_OK;
}

static HRESULT delete_open_object( HANDLE object )
{
    FILE_DISPOSITION_INFORMATION disposition = {TRUE};
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    status = NtSetInformationFile( object, &io, &disposition,
                                   sizeof(disposition),
                                   FileDispositionInformation );
    return status ? ntstatus_error( status ) : S_OK;
}

static void journal_clear_strings( struct deployment_journal *journal )
{
    HeapFree( GetProcessHeap(), 0, journal->staging_name );
    HeapFree( GetProcessHeap(), 0, journal->payload_path );
    HeapFree( GetProcessHeap(), 0, journal->publisher );
    HeapFree( GetProcessHeap(), 0, journal->package_name );
    HeapFree( GetProcessHeap(), 0, journal->family_name );
    HeapFree( GetProcessHeap(), 0, journal->full_name );
    journal->staging_name = journal->payload_path = journal->publisher =
        journal->package_name = journal->family_name = journal->full_name = NULL;
}

static void journal_destroy( struct deployment_journal *journal )
{
    journal_clear_strings( journal );
    memset( journal, 0, sizeof(*journal) );
}

static HRESULT journal_initialize( struct deployment_backend *backend,
                                   enum appx_deployment_operation operation,
                                   struct deployment_journal *journal )
{
    HRESULT hr;

    memset( journal, 0, sizeof(*journal) );
    journal->operation = operation;
    journal->state = APPX_DEPLOYMENT_JOURNAL_CREATED;
    if (FAILED(hr = backend_random( backend, journal->transaction_id,
                                    sizeof(journal->transaction_id) )))
        return hr;
    bytes_to_hex( journal->transaction_id, sizeof(journal->transaction_id),
                  journal->transaction_name );
    lstrcpyW( journal->journal_name, journal->transaction_name );
    lstrcatW( journal->journal_name, DEPLOYMENT_JOURNAL_SUFFIX );
    return duplicate_string( journal->transaction_name, FALSE,
                             &journal->staging_name );
}

static HRESULT journal_set_package(
    struct deployment_journal *journal,
    const struct appx_catalog_package *package, const WCHAR *payload_path )
{
    WCHAR *full_name = NULL, *family_name = NULL, *package_name = NULL;
    WCHAR *publisher = NULL, *payload = NULL;
    HRESULT hr;

    if (!package || !payload_path) return E_INVALIDARG;
    if (FAILED(hr = duplicate_string( package->full_name, FALSE,
                                      &full_name )) ||
        FAILED(hr = duplicate_string( package->family_name, FALSE,
                                      &family_name )) ||
        FAILED(hr = duplicate_string( package->name, FALSE,
                                      &package_name )) ||
        FAILED(hr = duplicate_string( package->publisher, FALSE,
                                      &publisher )) ||
        FAILED(hr = duplicate_string( payload_path, FALSE, &payload )))
        goto failed;
    journal_clear_strings( journal );
    journal->full_name = full_name;
    journal->family_name = family_name;
    journal->package_name = package_name;
    journal->publisher = publisher;
    journal->payload_path = payload;
    if (FAILED(hr = duplicate_string( journal->transaction_name, FALSE,
                                      &journal->staging_name )))
        goto failed_after_assign;
    memcpy( journal->content_id, package->content_id,
            sizeof(journal->content_id) );
    return S_OK;

failed_after_assign:
    journal_clear_strings( journal );
    return hr;
failed:
    HeapFree( GetProcessHeap(), 0, payload );
    HeapFree( GetProcessHeap(), 0, publisher );
    HeapFree( GetProcessHeap(), 0, package_name );
    HeapFree( GetProcessHeap(), 0, family_name );
    HeapFree( GetProcessHeap(), 0, full_name );
    return hr;
}

static HRESULT append_journal_string( BYTE *data, UINT32 capacity,
                                      UINT32 *offset, UINT32 reference_offset,
                                      const WCHAR *string, BOOL allow_empty )
{
    UINT32 chars, bytes, next;
    HRESULT hr;

    if (!string) string = L"";
    if (FAILED(hr = bounded_string_length( string, allow_empty, &chars )))
        return hr;
    if (!multiply_uint32( chars, sizeof(WCHAR), &bytes ) ||
        !add_uint32( *offset, bytes, &next ) || next > capacity)
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    write_uint32( data + reference_offset, *offset );
    write_uint32( data + reference_offset + 4, chars );
    memcpy( data + *offset, string, bytes );
    *offset = next;
    return S_OK;
}

static HRESULT serialize_journal( const struct deployment_journal *journal,
                                  BYTE **output, UINT32 *output_size )
{
    BYTE *data, digest[DEPLOYMENT_DIGEST_SIZE];
    UINT32 offset = DEPLOYMENT_JOURNAL_HEADER_SIZE;
    HRESULT hr;

    *output = NULL;
    *output_size = 0;
    if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                            APPX_DEPLOYMENT_MAX_JOURNAL_SIZE )))
        return E_OUTOFMEMORY;
    memcpy( data, deployment_journal_magic, sizeof(deployment_journal_magic) );
    write_uint32( data + JOURNAL_VERSION_OFFSET,
                  APPX_DEPLOYMENT_JOURNAL_VERSION );
    write_uint32( data + JOURNAL_HEADER_SIZE_OFFSET,
                  DEPLOYMENT_JOURNAL_HEADER_SIZE );
    write_uint32( data + JOURNAL_OPERATION_OFFSET, journal->operation );
    write_uint32( data + JOURNAL_STATE_OFFSET, journal->state );
    write_uint64( data + JOURNAL_EXPECTED_EPOCH_OFFSET,
                  journal->expected_epoch );
    memcpy( data + JOURNAL_TRANSACTION_ID_OFFSET, journal->transaction_id,
            sizeof(journal->transaction_id) );
    memcpy( data + JOURNAL_CONTENT_ID_OFFSET, journal->content_id,
            sizeof(journal->content_id) );
    if (FAILED(hr = append_journal_string(
            data, APPX_DEPLOYMENT_MAX_JOURNAL_SIZE, &offset,
            JOURNAL_FULL_NAME_REF_OFFSET, journal->full_name, TRUE )) ||
        FAILED(hr = append_journal_string(
            data, APPX_DEPLOYMENT_MAX_JOURNAL_SIZE, &offset,
            JOURNAL_FAMILY_NAME_REF_OFFSET, journal->family_name, TRUE )) ||
        FAILED(hr = append_journal_string(
            data, APPX_DEPLOYMENT_MAX_JOURNAL_SIZE, &offset,
            JOURNAL_PACKAGE_NAME_REF_OFFSET, journal->package_name, TRUE )) ||
        FAILED(hr = append_journal_string(
            data, APPX_DEPLOYMENT_MAX_JOURNAL_SIZE, &offset,
            JOURNAL_PUBLISHER_REF_OFFSET, journal->publisher, TRUE )) ||
        FAILED(hr = append_journal_string(
            data, APPX_DEPLOYMENT_MAX_JOURNAL_SIZE, &offset,
            JOURNAL_PAYLOAD_PATH_REF_OFFSET, journal->payload_path, TRUE )) ||
        FAILED(hr = append_journal_string(
            data, APPX_DEPLOYMENT_MAX_JOURNAL_SIZE, &offset,
            JOURNAL_STAGING_NAME_REF_OFFSET, journal->staging_name, TRUE )))
        goto failed;
    write_uint32( data + JOURNAL_TOTAL_SIZE_OFFSET, offset );
    if (FAILED(hr = hash_with_zero_range(
            data, offset, JOURNAL_DIGEST_OFFSET, DEPLOYMENT_DIGEST_SIZE,
            digest )))
        goto failed;
    memcpy( data + JOURNAL_DIGEST_OFFSET, digest, sizeof(digest) );
    *output = data;
    *output_size = offset;
    return S_OK;

failed:
    HeapFree( GetProcessHeap(), 0, data );
    return hr;
}

static HRESULT write_journal( struct deployment_store *store,
                              struct deployment_backend *backend,
                              struct deployment_journal *journal,
                              enum appx_deployment_journal_state state,
                              BOOL *weak )
{
    BYTE *data = NULL;
    UINT32 size;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    if (state < journal->state) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    journal->state = state;
    if (FAILED(hr = serialize_journal( journal, &data, &size )))
        return hr;
    if (FAILED(hr = open_child(
            store->transactions, journal->journal_name,
            FILE_WRITE_DATA | DELETE,
            FILE_SHARE_READ, FILE_OVERWRITE_IF, FALSE, &file, NULL )))
        goto done;
    if (FAILED(hr = require_single_link_file( file ))) goto done;
    if (FAILED(hr = backend_write_all( backend, file, data, size )) ||
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, file, FALSE ), weak )) ||
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->transactions, TRUE ),
            weak )))
        goto done;

done:
    close_handle( &file );
    HeapFree( GetProcessHeap(), 0, data );
    return hr;
}

static HRESULT advance_journal(
    struct deployment_store *store, struct deployment_backend *backend,
    struct deployment_journal *journal,
    enum appx_deployment_journal_state state, BOOL *weak )
{
    if (journal->state > state) return S_OK;
    return write_journal( store, backend, journal, state, weak );
}

static HRESULT remove_journal( struct deployment_store *store,
                               struct deployment_backend *backend,
                               const struct deployment_journal *journal,
                               BOOL *weak )
{
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    hr = open_child( store->transactions, journal->journal_name, DELETE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     FILE_OPEN, FALSE, &file, NULL );
    if (is_not_found( hr )) return S_OK;
    if (FAILED(hr)) return hr;
    if (FAILED(hr = require_single_link_file( file )))
    {
        close_handle( &file );
        return hr;
    }
    if (SUCCEEDED(hr = delete_open_object( file )))
        hr = mark_durability(
            backend, backend_flush( backend, store->transactions, TRUE ),
            weak );
    close_handle( &file );
    return hr;
}

static enum appx_catalog_architecture map_manifest_architecture(
    enum appx_manifest_architecture architecture )
{
    switch (architecture)
    {
    case APPX_MANIFEST_ARCHITECTURE_NEUTRAL:
        return APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    case APPX_MANIFEST_ARCHITECTURE_X86:
        return APPX_CATALOG_ARCHITECTURE_X86;
    case APPX_MANIFEST_ARCHITECTURE_X64:
        return APPX_CATALOG_ARCHITECTURE_X64;
    case APPX_MANIFEST_ARCHITECTURE_ARM:
        return APPX_CATALOG_ARCHITECTURE_ARM;
    case APPX_MANIFEST_ARCHITECTURE_ARM64:
        return APPX_CATALOG_ARCHITECTURE_ARM64;
    case APPX_MANIFEST_ARCHITECTURE_X86A64:
        return APPX_CATALOG_ARCHITECTURE_X86A64;
    }
    return ~0u;
}

static HRESULT map_bundle_host_architecture(
    enum appx_catalog_architecture architecture,
    enum appx_bundle_architecture *bundle_architecture )
{
    if (!bundle_architecture) return E_POINTER;
    switch (architecture)
    {
    case APPX_CATALOG_ARCHITECTURE_X86:
        *bundle_architecture = APPX_BUNDLE_ARCHITECTURE_X86;
        return S_OK;
    case APPX_CATALOG_ARCHITECTURE_X64:
        *bundle_architecture = APPX_BUNDLE_ARCHITECTURE_X64;
        return S_OK;
    case APPX_CATALOG_ARCHITECTURE_ARM:
        *bundle_architecture = APPX_BUNDLE_ARCHITECTURE_ARM;
        return S_OK;
    case APPX_CATALOG_ARCHITECTURE_ARM64:
        *bundle_architecture = APPX_BUNDLE_ARCHITECTURE_ARM64;
        return S_OK;
    case APPX_CATALOG_ARCHITECTURE_X86A64:
        *bundle_architecture = APPX_BUNDLE_ARCHITECTURE_X86A64;
        return S_OK;
    case APPX_CATALOG_ARCHITECTURE_NEUTRAL:
        break;
    }
    return HRESULT_FROM_WIN32(
        ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
}

static UINT32 map_bundle_supported_architectures(
    const struct appx_architecture_policy *policy )
{
    enum appx_catalog_architecture architecture;
    UINT32 mask = 0;

    for (architecture = APPX_CATALOG_ARCHITECTURE_X86;
         architecture <= APPX_CATALOG_ARCHITECTURE_X86A64;
         architecture++)
    {
        enum appx_bundle_architecture bundle_architecture;

        if (!appx_architecture_policy_supports( policy, architecture ))
            continue;
        if (SUCCEEDED(map_bundle_host_architecture(
                architecture, &bundle_architecture )))
            mask |= APPX_BUNDLE_ARCHITECTURE_MASK(bundle_architecture);
    }
    return mask;
}

static enum appx_catalog_activation_kind map_manifest_activation(
    enum appx_manifest_activation_kind activation )
{
    switch (activation)
    {
    case APPX_MANIFEST_ACTIVATION_FULL_TRUST:
        return APPX_CATALOG_ACTIVATION_FULL_TRUST;
    case APPX_MANIFEST_ACTIVATION_PACKAGED_CLASSIC:
        return APPX_CATALOG_ACTIVATION_PACKAGED_CLASSIC;
    case APPX_MANIFEST_ACTIVATION_WIN32:
        return APPX_CATALOG_ACTIVATION_WIN32;
    case APPX_MANIFEST_ACTIVATION_UNSUPPORTED:
        break;
    }
    return APPX_CATALOG_ACTIVATION_UNSUPPORTED;
}

static void release_prepared_package( struct deployment_backend *backend,
                                      struct prepared_package *prepared )
{
    if (prepared->test_owned && backend->test &&
        backend->test->release_package)
        backend->test->release_package( backend->test->context,
                                        &prepared->test_view );
    appx_bundle_inspection_free( prepared->bundle_inspection );
    appx_package_inspection_free( prepared->package_inspection );
    HeapFree( GetProcessHeap(), 0, prepared->payload_path );
    HeapFree( GetProcessHeap(), 0, prepared->application_loader_search );
    HeapFree( GetProcessHeap(), 0, prepared->inproc_classes );
    HeapFree( GetProcessHeap(), 0, prepared->loader_files );
    HeapFree( GetProcessHeap(), 0, prepared->dependencies );
    HeapFree( GetProcessHeap(), 0, prepared->applications );
    memset( prepared, 0, sizeof(*prepared) );
}

/*
 * Preserve the deterministic test backend's original loader-list contract.
 * Production inventory is populated from bounded PE header inspection after
 * extraction and never uses this suffix filter.
 */
static BOOL test_loader_path_has_legacy_suffix( const WCHAR *path )
{
    UINT32 length = lstrlenW( path );

    return length >= 4 &&
           (CompareStringOrdinal( path + length - 4, 4, L".dll", 4,
                                  TRUE ) == CSTR_EQUAL ||
            CompareStringOrdinal( path + length - 4, 4, L".exe", 4,
                                  TRUE ) == CSTR_EQUAL);
}

static HRESULT validate_loader_search_paths(
    BOOL present, UINT32 count, const WCHAR * const *paths )
{
    UINT32 i, j;

    if ((!present && count) ||
        count > APPX_DEPLOYMENT_MAX_LOADER_SEARCH_PATHS ||
        (count && !paths))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    for (i = 0; i < count; i++)
    {
        if (!paths[i] ||
            FAILED(validate_optional_relative_path( paths[i], 256 )))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        for (j = 0; j < i; j++)
            if (CompareStringOrdinal( paths[j], -1, paths[i], -1,
                                      TRUE ) == CSTR_EQUAL)
                return APPX_E_INVALID_PACKAGING_LAYOUT;
    }
    return S_OK;
}

static HRESULT validate_prepared_package(
    struct prepared_package *prepared,
    enum appx_catalog_architecture target_architecture )
{
    const struct appx_catalog_package *package = &prepared->test_view.package;
    struct appx_architecture_policy policy;
    UINT32 i, count;
    HRESULT hr;

    if (prepared->test_view.flags &
        ~APPX_DEPLOYMENT_TEST_PACKAGE_KNOWN_FLAGS)
        return E_INVALIDARG;
    if (prepared->test_view.flags &
        (APPX_DEPLOYMENT_TEST_PACKAGE_UNSUPPORTED |
         APPX_DEPLOYMENT_TEST_PACKAGE_RESOURCE))
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    if (!package->name || !package->publisher || !package->resource_id ||
        !package->publisher_id || !package->full_name ||
        !package->family_name ||
        (package->flags & ~APPX_CATALOG_PACKAGE_KNOWN_FLAGS) ||
        !(package->flags & APPX_CATALOG_PACKAGE_SIGNED) ||
        !appx_architecture_is_valid( package->architecture ) ||
        package->application_count >
            APPX_CATALOG_MAX_APPLICATIONS_PER_PACKAGE ||
        package->dependency_count >
            APPX_CATALOG_MAX_DEPENDENCIES_PER_PACKAGE ||
        (package->application_count && !package->applications) ||
        (package->dependency_count && !package->dependencies))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (FAILED(hr = appx_architecture_query_host_policy(
            target_architecture, &policy )))
        return hr;
    if (!appx_architecture_policy_supports(
            &policy, package->architecture ))
        return HRESULT_FROM_WIN32(
            ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
    if (prepared->test_view.loader_file_count >
            APPX_DEPLOYMENT_MAX_LOADER_FILES ||
        prepared->test_view.inproc_class_count >
            APPX_DEPLOYMENT_MAX_INPROC_CLASSES ||
        (prepared->test_view.loader_file_count &&
         !prepared->test_view.loader_files) ||
        (prepared->test_view.inproc_class_count &&
         !prepared->test_view.inproc_classes))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (FAILED(validate_loader_search_paths(
            prepared->has_package_loader_search_path_override,
            prepared->package_loader_search_path_count,
            prepared->package_loader_search_paths )))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!(package->flags & APPX_CATALOG_PACKAGE_FRAMEWORK) &&
        !package->application_count)
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    for (i = 0; i < package->application_count; i++)
    {
        enum appx_catalog_activation_kind kind =
            package->applications[i].activation_kind;

        if (!package->applications[i].id ||
            !package->applications[i].executable ||
            !package->applications[i].entry_point ||
            FAILED(validate_optional_string_max(
                package->applications[i].parameters, 1024 )) ||
            FAILED(validate_optional_relative_path(
                package->applications[i].current_directory_path, 256 )) ||
            (kind != APPX_CATALOG_ACTIVATION_FULL_TRUST &&
             kind != APPX_CATALOG_ACTIVATION_PACKAGED_CLASSIC &&
             kind != APPX_CATALOG_ACTIVATION_WIN32))
            return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
        if (prepared->application_loader_search &&
            FAILED(validate_loader_search_paths(
                prepared->application_loader_search[i].present,
                prepared->application_loader_search[i].count,
                prepared->application_loader_search[i].paths )))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    }
    for (i = 0; i < package->dependency_count; i++)
        if (!package->dependencies[i].name ||
            !package->dependencies[i].publisher)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    for (i = 0; i < prepared->test_view.loader_file_count; i++)
        if (!prepared->test_view.loader_files[i].relative_path ||
            FAILED(validate_relative_path(
                prepared->test_view.loader_files[i].relative_path, FALSE )) ||
            (prepared->test_owned && !test_loader_path_has_legacy_suffix(
                prepared->test_view.loader_files[i].relative_path )))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    for (i = 0; i < prepared->test_view.inproc_class_count; i++)
    {
        const struct appx_deployment_inproc_class *class =
            prepared->test_view.inproc_classes + i;

        if (!class->path || !class->activatable_class_id ||
            FAILED(validate_relative_path( class->path, FALSE )) ||
            class->threading_model > APPX_MANIFEST_THREADING_MTA)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        if (FAILED(bounded_string_length(
                class->activatable_class_id, FALSE, &count )))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    }
    return S_OK;
}

enum appx_container_kind
{
    APPX_CONTAINER_PACKAGE,
    APPX_CONTAINER_BUNDLE,
};

static HRESULT archive_has_exact_entry( WINE_APPX_ARCHIVE *archive,
                                        const WCHAR *expected,
                                        BOOL *present )
{
    WINE_APPX_ARCHIVE_ENTRY entry = {sizeof(entry)};
    WCHAR *path = NULL;
    UINT32 index, length = 0;
    HRESULT hr;

    *present = FALSE;
    hr = wine_appx_archive_find_entry( archive, expected, &index );
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return S_OK;
    if (FAILED(hr)) return hr;
    hr = wine_appx_archive_get_entry(
        archive, index, &entry, &length, NULL );
    if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) ||
        !length || length > WINE_APPX_MAX_PATH_CHARS)
        return FAILED(hr) ? hr : APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!(path = HeapAlloc( GetProcessHeap(), 0,
                            (SIZE_T)length * sizeof(*path) )))
        return E_OUTOFMEMORY;
    entry.size = sizeof(entry);
    hr = wine_appx_archive_get_entry(
        archive, index, &entry, &length, path );
    if (SUCCEEDED(hr) &&
        (lstrcmpW( path, expected ) ||
         (entry.flags & WINE_APPX_ENTRY_DIRECTORY)))
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
    HeapFree( GetProcessHeap(), 0, path );
    if (SUCCEEDED(hr)) *present = TRUE;
    return hr;
}

static HRESULT detect_container_kind(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits,
    const APPX_DEPLOYMENT_OPTIONS *options,
    enum appx_container_kind *kind )
{
    static const WCHAR package_manifest[] = L"AppxManifest.xml";
    static const WCHAR bundle_manifest[] =
        L"AppxMetadata\\AppxBundleManifest.xml";
    WINE_APPX_ARCHIVE *archive = NULL;
    BOOL package, bundle;
    HRESULT hr;

    if (FAILED(hr = check_cancelled( options )) ||
        FAILED(hr = wine_appx_archive_open_ex(
            file, limits, 0, options->cancel_event, &archive )) ||
        FAILED(hr = archive_has_exact_entry(
            archive, package_manifest, &package )) ||
        FAILED(hr = check_cancelled( options )) ||
        FAILED(hr = archive_has_exact_entry(
            archive, bundle_manifest, &bundle )) ||
        FAILED(hr = check_cancelled( options )))
        goto done;
    if (package == bundle)
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
    else
    {
        *kind = package ? APPX_CONTAINER_PACKAGE :
                          APPX_CONTAINER_BUNDLE;
        hr = S_OK;
    }

done:
    wine_appx_archive_close( archive );
    return hr;
}

static HRESULT prepare_production_package(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    HANDLE temporary_directory,
    struct prepared_package *prepared )
{
    const struct appx_manifest_identity *identity;
    const APPX_MANIFEST *manifest;
    struct appx_architecture_policy architecture_policy;
    struct appx_bundle_selection_policy bundle_policy = {
        .size = sizeof(bundle_policy),
        .language_neutral_only = TRUE,
        .scale_neutral_only = TRUE,
    };
    APPX_BUNDLE_INSPECT_OPTIONS bundle_options = {
        .size = sizeof(bundle_options),
        .version = APPX_BUNDLE_INSPECT_OPTIONS_VERSION,
        .temporary_directory = temporary_directory,
        .cancel_event = options->cancel_event,
        .free_space_floor_bytes = options->free_space_floor_bytes,
        .lock_timeout_ms = options->writer_timeout_ms,
    };
    struct appx_catalog_package *package = &prepared->test_view.package;
    enum appx_container_kind container;
    UINT32 i, count;
    HRESULT hr;

    if (!package_file || package_file == INVALID_HANDLE_VALUE)
        return E_INVALIDARG;
    if (FAILED(hr = get_identity( package_file, FALSE, NULL, NULL, NULL )))
        return hr;
    if (FAILED(hr = detect_container_kind(
            package_file, options->archive_limits, options, &container )))
        return hr;
    if (container == APPX_CONTAINER_PACKAGE)
    {
        if (FAILED(hr = appx_package_inspect_ex(
                package_file, options->archive_limits, 0,
                options->cancel_event, &prepared->package_inspection )))
            return hr;
        prepared->inspection = prepared->package_inspection;
    }
    else
    {
        if (FAILED(hr = appx_architecture_query_host_policy(
                options->target_architecture, &architecture_policy )) ||
            FAILED(hr = map_bundle_host_architecture(
                architecture_policy.preferred,
                &bundle_policy.host_architecture )))
            return hr;
        bundle_policy.architecture_policy.value.version =
            APPX_BUNDLE_ARCHITECTURE_POLICY_VERSION;
        bundle_policy.architecture_policy.value.supported_architectures =
            map_bundle_supported_architectures( &architecture_policy );
        hr = appx_bundle_inspect_ex(
            package_file, options->archive_limits, 0, &bundle_policy,
            &bundle_options,
            &prepared->bundle_inspection );
        if (FAILED(hr)) return hr;
        prepared->inspection =
            appx_bundle_inspection_get_selected_package(
                prepared->bundle_inspection );
        if (!prepared->inspection)
            return APPX_E_CORRUPT_CONTENT;
    }
    manifest = appx_package_inspection_get_manifest( prepared->inspection );
    if (!manifest || !(identity = appx_manifest_get_identity( manifest )))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!appx_manifest_is_supported( manifest ))
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    if (appx_manifest_is_resource_package( manifest ))
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
    if (!appx_manifest_is_framework( manifest ) &&
        !appx_manifest_has_run_full_trust( manifest ))
        return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

    prepared->test_view.size = sizeof(prepared->test_view);
    prepared->test_view.version = APPX_DEPLOYMENT_TEST_PACKAGE_VERSION;
    package->name = identity->name;
    package->publisher = identity->publisher;
    package->resource_id = identity->resource_id;
    package->publisher_id = identity->publisher_id;
    package->full_name = identity->full_name;
    package->family_name = identity->family_name;
    package->version.major = identity->version.major;
    package->version.minor = identity->version.minor;
    package->version.build = identity->version.build;
    package->version.revision = identity->version.revision;
    package->architecture =
        map_manifest_architecture( identity->architecture );
    package->flags = APPX_CATALOG_PACKAGE_ACTIVE |
                     APPX_CATALOG_PACKAGE_SIGNED;
    if (appx_manifest_is_framework( manifest ))
        package->flags |= APPX_CATALOG_PACKAGE_FRAMEWORK;
    if (FAILED(hr = appx_package_inspection_get_content_id(
            prepared->inspection, package->content_id,
            sizeof(package->content_id) )))
        return hr;

    count = appx_manifest_get_application_count( manifest );
    if (count)
    {
        if (!(prepared->applications = HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY,
                count * sizeof(*prepared->applications) )))
            return E_OUTOFMEMORY;
        package->applications = prepared->applications;
        package->application_count = count;
        if (!(prepared->application_loader_search = HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY,
                count * sizeof(*prepared->application_loader_search) )))
            return E_OUTOFMEMORY;
        for (i = 0; i < count; i++)
        {
            const struct appx_manifest_application *source =
                appx_manifest_get_application( manifest, i );

            if (!source) return APPX_E_INVALID_PACKAGING_LAYOUT;
            prepared->applications[i].id = source->id;
            prepared->applications[i].executable = source->executable;
            prepared->applications[i].entry_point = source->entry_point;
            prepared->applications[i].parameters = source->parameters;
            prepared->applications[i].current_directory_path =
                source->current_directory_path;
            prepared->applications[i].activation_kind =
                map_manifest_activation( source->activation_kind );
            prepared->application_loader_search[i].present =
                source->has_loader_search_path_override;
            prepared->application_loader_search[i].count =
                source->loader_search_path_count;
            memcpy( prepared->application_loader_search[i].paths,
                    source->loader_search_paths,
                    source->loader_search_path_count *
                    sizeof(*source->loader_search_paths) );
        }
    }

    prepared->has_package_loader_search_path_override =
        appx_manifest_has_loader_search_path_override( manifest );
    prepared->package_loader_search_path_count =
        appx_manifest_get_loader_search_path_count( manifest );
    for (i = 0; i < prepared->package_loader_search_path_count; i++)
        prepared->package_loader_search_paths[i] =
            appx_manifest_get_loader_search_path( manifest, i );

    count = appx_manifest_get_dependency_count( manifest );
    if (count)
    {
        if (!(prepared->dependencies = HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY,
                count * sizeof(*prepared->dependencies) )))
            return E_OUTOFMEMORY;
        package->dependencies = prepared->dependencies;
        package->dependency_count = count;
        for (i = 0; i < count; i++)
        {
            const struct appx_manifest_dependency *source =
                appx_manifest_get_dependency( manifest, i );

            if (!source || source->optional)
                return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
            prepared->dependencies[i].name = source->name;
            prepared->dependencies[i].publisher = source->publisher;
            prepared->dependencies[i].min_version.major =
                source->min_version.major;
            prepared->dependencies[i].min_version.minor =
                source->min_version.minor;
            prepared->dependencies[i].min_version.build =
                source->min_version.build;
            prepared->dependencies[i].min_version.revision =
                source->min_version.revision;
        }
    }

    count = appx_manifest_get_inproc_class_count( manifest );
    if (count)
    {
        if (count > APPX_DEPLOYMENT_MAX_INPROC_CLASSES)
            return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
        if (!(prepared->inproc_classes = HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY,
                count * sizeof(*prepared->inproc_classes) )))
            return E_OUTOFMEMORY;
        for (i = 0; i < count; i++)
        {
            const struct appx_manifest_inproc_class *source =
                appx_manifest_get_inproc_class( manifest, i );

            if (!source) return APPX_E_INVALID_PACKAGING_LAYOUT;
            prepared->inproc_classes[i].path = source->path;
            prepared->inproc_classes[i].activatable_class_id =
                source->activatable_class_id;
            prepared->inproc_classes[i].threading_model =
                source->threading_model;
        }
        prepared->test_view.inproc_class_count = count;
        prepared->test_view.inproc_classes = prepared->inproc_classes;
    }
    return S_OK;
}

static HRESULT prepare_package(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend, HANDLE temporary_directory,
    struct prepared_package *prepared )
{
    struct appx_catalog_package *package;
    HRESULT hr;

    memset( prepared, 0, sizeof(*prepared) );
    if (backend->test && backend->test->prepare_package)
    {
        prepared->test_view.size = sizeof(prepared->test_view);
        prepared->test_view.version = APPX_DEPLOYMENT_TEST_PACKAGE_VERSION;
        hr = backend->test->prepare_package(
            backend->test->context, package_file,
            options->target_architecture, &prepared->test_view );
        if (FAILED(hr)) return hr;
        prepared->test_owned = TRUE;
        if (prepared->test_view.size != sizeof(prepared->test_view) ||
            prepared->test_view.version !=
                APPX_DEPLOYMENT_TEST_PACKAGE_VERSION)
            return E_INVALIDARG;
    }
    else if (FAILED(hr = prepare_production_package(
                 package_file, options, temporary_directory, prepared )))
        return hr;

    if (FAILED(hr = validate_prepared_package(
            prepared, options->target_architecture )))
        return hr;
    package = &prepared->test_view.package;
    if (FAILED(hr = derive_payload_path(
            package->full_name, package->content_id,
            &prepared->payload_path )))
        return hr;
    package->payload_path = prepared->payload_path;
    return S_OK;
}

static HRESULT extract_prepared_package(
    struct deployment_backend *backend,
    const struct prepared_package *prepared, HANDLE staging,
    const APPX_DEPLOYMENT_OPTIONS *options, BOOL *weak )
{
    APPX_EXTRACT_OPTIONS extract_options;
    HRESULT hr;

    memset( &extract_options, 0, sizeof(extract_options) );
    extract_options.size = sizeof(extract_options);
    extract_options.max_expanded_bytes = options->package_quota_bytes;
    extract_options.free_space_floor_bytes = options->free_space_floor_bytes;
    extract_options.cancel_event = options->cancel_event;
    if (backend->test && backend->test->extract)
        hr = backend->test->extract( backend->test->context, staging,
                                     &extract_options );
    else
        hr = appx_package_extract( prepared->inspection, staging,
                                   &extract_options );
    if (hr == APPX_EXTRACT_S_WEAK_DURABILITY)
    {
        if (!backend->accept_weak_durability)
            return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
        *weak = TRUE;
        return S_OK;
    }
    return hr;
}

static INT compare_loader_inventory_file(
    const void *left_pointer, const void *right_pointer )
{
    const struct appx_deployment_loader_file *left = left_pointer;
    const struct appx_deployment_loader_file *right = right_pointer;
    INT result = compare_string_ci(
        left->relative_path, right->relative_path );

    if (result) return result;
    return compare_string_exact( left->relative_path, right->relative_path );
}

static BOOL loader_inventory_contains_sorted(
    const struct appx_deployment_loader_file *files, UINT32 count,
    const WCHAR *path )
{
    UINT32 low = 0, high = count;

    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;
        INT comparison = compare_string_ci(
            files[middle].relative_path, path );

        if (comparison < 0) low = middle + 1;
        else high = middle;
    }
    return low < count &&
           !compare_string_ci( files[low].relative_path, path );
}

static HRESULT collect_production_loader_inventory(
    struct prepared_package *prepared, HANDLE staging )
{
    struct appx_deployment_loader_file *files = NULL, *replacement;
    const struct appx_catalog_package *package = &prepared->test_view.package;
    UINT32 file_count, count = 0, capacity = 0, i;
    HRESULT hr = S_OK;

    if (prepared->test_owned) return S_OK;
    if (!prepared->inspection || prepared->loader_files ||
        prepared->test_view.loader_file_count ||
        prepared->test_view.loader_files)
        return APPX_E_INVALID_PACKAGING_LAYOUT;

    file_count =
        appx_package_inspection_get_file_count( prepared->inspection );
    for (i = 0; i < file_count; i++)
    {
        const APPX_PACKAGE_FILE *source =
            appx_package_inspection_get_file( prepared->inspection, i );
        HANDLE file = INVALID_HANDLE_VALUE;
        USHORT machine;

        if (!source || !source->path)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
        if (FAILED(hr = open_payload_file(
                staging, source->path, &file, NULL )))
            goto failed;
        hr = appx_architecture_read_pe_machine( file, &machine );
        close_handle( &file );
        if (FAILED(hr)) goto failed;
        if (hr == S_FALSE) continue;
        if (count == APPX_DEPLOYMENT_MAX_LOADER_FILES)
        {
            hr = HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
            goto failed;
        }
        if (count == capacity)
        {
            UINT32 new_capacity = capacity ? capacity * 2 : 64;

            if (new_capacity > APPX_DEPLOYMENT_MAX_LOADER_FILES)
                new_capacity = APPX_DEPLOYMENT_MAX_LOADER_FILES;
            if (files)
                replacement = HeapReAlloc(
                    GetProcessHeap(), HEAP_ZERO_MEMORY, files,
                    new_capacity * sizeof(*files) );
            else
                replacement = HeapAlloc(
                    GetProcessHeap(), HEAP_ZERO_MEMORY,
                    new_capacity * sizeof(*files) );
            if (!replacement)
            {
                hr = E_OUTOFMEMORY;
                goto failed;
            }
            files = replacement;
            capacity = new_capacity;
        }
        files[count++].relative_path = source->path;
    }

    /*
     * Sorting once keeps hostile maximum-size inventories bounded by
     * O(n log n); application lookups below are O(log n), rather than a
     * repeated linear scan over as many as 16K PE payloads.
     */
    if (count > 1)
        qsort( files, count, sizeof(*files),
               compare_loader_inventory_file );
    for (i = 1; i < count; i++)
        if (!compare_string_ci(
                files[i - 1].relative_path, files[i].relative_path ))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }

    /*
     * Application executables are part of the same provenance inventory as
     * extensionless images and DLL-style payloads.  Requiring the scanned PE
     * entry here both avoids a suffix escape and keeps a shared executable
     * represented exactly once.
     */
    for (i = 0; i < package->application_count; i++)
    {
        const WCHAR *executable = package->applications[i].executable;

        if (!loader_inventory_contains_sorted(
                files, count, executable ))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
    }

    prepared->loader_files = files;
    prepared->test_view.loader_file_count = count;
    prepared->test_view.loader_files = files;
    return S_OK;

failed:
    HeapFree( GetProcessHeap(), 0, files );
    return hr;
}

static HRESULT backend_catalog_load( struct deployment_backend *backend,
                                     const APPX_DEPLOYMENT_OPTIONS *options,
                                     const WCHAR *store_root,
                                     APPX_CATALOG_SNAPSHOT **snapshot )
{
    if (backend->test && backend->test->catalog_load)
        return backend->test->catalog_load( backend->test->context,
                                            store_root, snapshot );
    return appx_catalog_load_bounded(
        store_root, options->writer_timeout_ms, options->cancel_event,
        snapshot );
}

static HRESULT backend_catalog_publish(
    struct deployment_backend *backend, const APPX_DEPLOYMENT_OPTIONS *options,
    const WCHAR *store_root,
    UINT64 expected_epoch, const APPX_CATALOG_SNAPSHOT *replacement,
    BOOL *weak )
{
    HRESULT hr;

    if (backend->test && backend->test->catalog_publish)
        hr = backend->test->catalog_publish(
            backend->test->context, store_root, expected_epoch, replacement );
    else
        hr = appx_catalog_publish_bounded(
            store_root, expected_epoch, replacement,
            options->writer_timeout_ms, options->cancel_event );
    if (hr == APPX_CATALOG_S_WEAK_DURABILITY)
    {
        if (!backend->accept_weak_durability)
            return HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
        *weak = TRUE;
        return S_OK;
    }
    return hr;
}

struct dependency_index
{
    const struct appx_catalog_package *package;
    UINT32 package_index;
};

static INT compare_dependency_index( const void *left, const void *right )
{
    const struct dependency_index *left_index = left;
    const struct dependency_index *right_index = right;
    INT result;

    if ((result = compare_string_ci( left_index->package->name,
                                     right_index->package->name )))
        return result;
    if ((result = compare_string_ci( left_index->package->publisher,
                                     right_index->package->publisher )))
        return result;
    result = compare_version( &left_index->package->version,
                              &right_index->package->version );
    if (result) return -result;
    if (left_index->package_index < right_index->package_index) return -1;
    if (left_index->package_index > right_index->package_index) return 1;
    return 0;
}

static INT compare_dependency_key(
    const struct dependency_index *index,
    const struct appx_catalog_dependency *dependency )
{
    INT result;

    if ((result = compare_string_ci( index->package->name,
                                     dependency->name )))
        return result;
    return compare_string_ci( index->package->publisher,
                              dependency->publisher );
}

static const struct dependency_index *resolve_dependency(
    const struct dependency_index *index, UINT32 count,
    const struct appx_catalog_dependency *dependency,
    enum appx_catalog_architecture target_architecture )
{
    UINT32 low = 0, high = count, first, i;

    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;

        if (compare_dependency_key( index + middle, dependency ) < 0)
            low = middle + 1;
        else
            high = middle;
    }
    first = low;
    for (i = first; i < count &&
         !compare_dependency_key( index + i, dependency ); i++)
    {
        const struct appx_catalog_package *candidate = index[i].package;

        if (!(candidate->flags & APPX_CATALOG_PACKAGE_ACTIVE) ||
            !(candidate->flags & APPX_CATALOG_PACKAGE_FRAMEWORK) ||
            compare_version( &candidate->version,
                             &dependency->min_version ) < 0 ||
            !appx_architecture_is_compatible(
                candidate->architecture, target_architecture ))
            continue;
        return index + i;
    }
    return NULL;
}

static HRESULT visit_dependency_graph(
    const struct appx_catalog_package *packages, UINT32 count,
    const struct dependency_index *index, UINT32 index_count,
    enum appx_catalog_architecture target_architecture, BYTE *colors,
    UINT32 package_index )
{
    const struct appx_catalog_package *package = packages + package_index;
    UINT32 i;
    HRESULT hr;

    if (colors[package_index] == 2) return S_OK;
    if (colors[package_index] == 1)
        return APPX_DEPLOYMENT_E_DEPENDENCY;
    colors[package_index] = 1;
    for (i = 0; i < package->dependency_count; i++)
    {
        const struct dependency_index *resolved =
            resolve_dependency( index, index_count, package->dependencies + i,
                                target_architecture );

        if (!resolved) return APPX_DEPLOYMENT_E_DEPENDENCY;
        if (FAILED(hr = visit_dependency_graph(
                packages, count, index, index_count, target_architecture,
                colors, resolved->package_index )))
            return hr;
    }
    colors[package_index] = 2;
    return S_OK;
}

static HRESULT validate_dependency_graph(
    const struct appx_catalog_package *packages, UINT32 count )
{
    struct dependency_index *index = NULL;
    BYTE *colors = NULL;
    UINT32 index_count = 0, i, target;
    HRESULT hr = S_OK;

    if (count)
    {
        if (!(index = HeapAlloc( GetProcessHeap(), 0,
                                  count * sizeof(*index) )) ||
            !(colors = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   count )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
    }
    for (i = 0; i < count; i++)
    {
        if (!(packages[i].flags & APPX_CATALOG_PACKAGE_ACTIVE)) continue;
        if (!appx_architecture_is_valid( packages[i].architecture ))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
        index[index_count].package = packages + i;
        index[index_count].package_index = i;
        index_count++;
    }
    if (index_count > 1)
        qsort( index, index_count, sizeof(*index),
               compare_dependency_index );
    for (i = 0; i < count; i++)
    {
        if (!(packages[i].flags & APPX_CATALOG_PACKAGE_ACTIVE)) continue;
        if (packages[i].architecture != APPX_CATALOG_ARCHITECTURE_NEUTRAL)
        {
            memset( colors, 0, count );
            if (FAILED(hr = visit_dependency_graph(
                    packages, count, index, index_count,
                    packages[i].architecture, colors, i )))
                goto done;
            continue;
        }

        /*
         * A neutral package does not persist one process architecture in the
         * catalog.  Validate that at least one concrete dependency closure is
         * structurally usable; activation revalidates the selected
         * executable's actual machine and resolves its exact closure.
         */
        hr = APPX_DEPLOYMENT_E_DEPENDENCY;
        for (target = APPX_CATALOG_ARCHITECTURE_X86;
             target <= APPX_CATALOG_ARCHITECTURE_X86A64; target++)
        {
            memset( colors, 0, count );
            hr = visit_dependency_graph(
                packages, count, index, index_count, target, colors, i );
            if (SUCCEEDED(hr)) break;
            if (hr != APPX_DEPLOYMENT_E_DEPENDENCY) goto done;
        }
        if (FAILED(hr)) goto done;
    }

done:
    HeapFree( GetProcessHeap(), 0, colors );
    HeapFree( GetProcessHeap(), 0, index );
    return hr;
}

static HRESULT find_catalog_package(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *full_name,
    const struct appx_catalog_package **result, UINT32 *index )
{
    UINT32 low = 0, high = appx_catalog_snapshot_get_package_count( snapshot );

    *result = NULL;
    if (index) *index = MAXDWORD;
    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( snapshot, middle );
        INT comparison;

        if (!package) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        comparison = compare_string_ci( package->full_name, full_name );
        if (comparison < 0)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < appx_catalog_snapshot_get_package_count( snapshot ))
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( snapshot, low );

        if (!package) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        if (!compare_string_ci( package->full_name, full_name ))
        {
            *result = package;
            if (index) *index = low;
        }
    }
    return S_OK;
}

static HRESULT find_active_family(
    const APPX_CATALOG_SNAPSHOT *snapshot, const WCHAR *family_name,
    const struct appx_catalog_package **result, UINT32 *result_index )
{
    UINT32 count = appx_catalog_snapshot_get_package_count( snapshot ), i;

    *result = NULL;
    if (result_index) *result_index = MAXDWORD;
    for (i = 0; i < count; i++)
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( snapshot, i );

        if (!package) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        if (!(package->flags & APPX_CATALOG_PACKAGE_ACTIVE) ||
            compare_string_ci( package->family_name, family_name ))
            continue;
        if (*result) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        *result = package;
        if (result_index) *result_index = i;
    }
    return S_OK;
}

static HRESULT build_add_replacement(
    enum appx_deployment_operation operation,
    const APPX_CATALOG_SNAPSHOT *current,
    struct prepared_package *prepared,
    const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_CATALOG_SNAPSHOT **replacement, BOOL *no_change )
{
    const struct appx_catalog_package *exact = NULL, *active = NULL;
    struct appx_catalog_package *packages = NULL;
    struct appx_catalog_package package = prepared->test_view.package;
    UINT32 count, exact_index, active_index, output_count, i, output = 0;
    UINT64 epoch = appx_catalog_snapshot_get_epoch( current );
    HRESULT hr;

    *replacement = NULL;
    *no_change = FALSE;
    if (epoch == ~(UINT64)0) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = find_catalog_package( current, package.full_name,
                                          &exact, &exact_index )) ||
        FAILED(hr = find_active_family( current, package.family_name,
                                        &active, &active_index )))
        return hr;
    if (exact && !equal_content_id( exact->content_id, package.content_id ))
        return APPX_DEPLOYMENT_E_CONTENT_CONFLICT;
    if (exact && (exact->flags & APPX_CATALOG_PACKAGE_ACTIVE))
    {
        *no_change = TRUE;
        return S_OK;
    }
    if (operation == APPX_DEPLOYMENT_OPERATION_INSTALL)
    {
        if (active) return APPX_DEPLOYMENT_E_CONTENT_CONFLICT;
    }
    else
    {
        if (!active)
            return HRESULT_FROM_WIN32( ERROR_INSTALL_PACKAGE_NOT_FOUND );
        if (compare_string_ci( active->publisher, package.publisher ) ||
            compare_string_ci( active->family_name, package.family_name ))
            return APPX_DEPLOYMENT_E_CONTENT_CONFLICT;
        if (!(options->flags & APPX_DEPLOYMENT_ALLOW_DOWNGRADE) &&
            compare_version( &package.version, &active->version ) <= 0)
            return APPX_DEPLOYMENT_E_DOWNGRADE;
    }

    package.flags |= APPX_CATALOG_PACKAGE_ACTIVE;
    count = appx_catalog_snapshot_get_package_count( current );
    output_count = count + 1;
    if (exact) output_count--;
    if (active && active != exact) output_count--;
    if (output_count > APPX_CATALOG_MAX_PACKAGES)
        return HRESULT_FROM_WIN32( ERROR_INSTALL_OUT_OF_DISK_SPACE );
    if (!(packages = HeapAlloc( GetProcessHeap(), 0,
                                output_count * sizeof(*packages) )))
        return output_count ? E_OUTOFMEMORY :
               APPX_DEPLOYMENT_E_CORRUPT_STORE;
    for (i = 0; i < count; i++)
    {
        const struct appx_catalog_package *source =
            appx_catalog_snapshot_get_package( current, i );

        if (!source)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
        if ((exact && i == exact_index) ||
            (active && i == active_index))
            continue;
        packages[output++] = *source;
    }
    packages[output++] = package;
    if (output != output_count)
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (FAILED(hr = validate_dependency_graph( packages, output_count )))
        goto done;
    hr = appx_catalog_snapshot_create( epoch + 1, packages, output_count,
                                       replacement );

done:
    HeapFree( GetProcessHeap(), 0, packages );
    return hr;
}

static HRESULT build_remove_replacement(
    const APPX_CATALOG_SNAPSHOT *current, const WCHAR *full_name,
    const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_CATALOG_SNAPSHOT **replacement,
    const struct appx_catalog_package **removed )
{
    struct appx_catalog_package *packages = NULL;
    const struct appx_catalog_package *target = NULL;
    UINT32 count, target_index, i, output = 0;
    UINT64 epoch = appx_catalog_snapshot_get_epoch( current );
    HRESULT hr;

    *replacement = NULL;
    *removed = NULL;
    if (epoch == ~(UINT64)0) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = find_catalog_package( current, full_name,
                                          &target, &target_index )))
        return hr;
    if (!target || !(target->flags & APPX_CATALOG_PACKAGE_ACTIVE))
        return HRESULT_FROM_WIN32( ERROR_INSTALL_PACKAGE_NOT_FOUND );
    count = appx_catalog_snapshot_get_package_count( current );
    if (count > 1 &&
        !(packages = HeapAlloc( GetProcessHeap(), 0,
                                (count - 1) * sizeof(*packages) )))
        return E_OUTOFMEMORY;
    for (i = 0; i < count; i++)
    {
        const struct appx_catalog_package *source =
            appx_catalog_snapshot_get_package( current, i );

        if (!source)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
        if (i != target_index) packages[output++] = *source;
    }
    hr = validate_dependency_graph( packages, output );
    if (hr == APPX_DEPLOYMENT_E_DEPENDENCY)
        hr = APPX_DEPLOYMENT_E_PACKAGE_IN_USE;
    if (FAILED(hr)) goto done;
    if (FAILED(hr = appx_catalog_snapshot_create(
            epoch + 1, packages, output, replacement )))
        goto done;
    *removed = target;

done:
    HeapFree( GetProcessHeap(), 0, packages );
    return hr;
}

static HRESULT create_staging_directory(
    struct deployment_store *store,
    const struct deployment_journal *journal, HANDLE *staging,
    struct object_identity *identity )
{
    return open_child( store->staging, journal->staging_name,
                       FILE_LIST_DIRECTORY | FILE_ADD_FILE |
                       FILE_ADD_SUBDIRECTORY | FILE_TRAVERSE | DELETE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       FILE_CREATE, TRUE, staging, identity );
}

static HRESULT publish_staging_directory(
    struct deployment_store *store, struct deployment_backend *backend,
    HANDLE staging, const struct object_identity *staging_identity,
    const WCHAR *payload_path, BOOL *reused, BOOL *weak )
{
    const WCHAR *generation = payload_generation_name( payload_path );
    struct object_identity destination_identity;
    HANDLE destination = INVALID_HANDLE_VALUE;
    HRESULT hr;

    *reused = FALSE;
    if (!generation) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    hr = open_path_component( store->payloads, generation, FALSE,
                              &destination, &destination_identity );
    if (SUCCEEDED(hr))
    {
        if (destination_identity.volume_serial !=
                staging_identity->volume_serial ||
            destination_identity.file_index_high !=
                staging_identity->file_index_high ||
            destination_identity.file_index_low !=
                staging_identity->file_index_low)
            hr = APPX_DEPLOYMENT_E_CONTENT_CONFLICT;
        else
            *reused = TRUE;
        close_handle( &destination );
        return hr;
    }
    if (!is_not_found( hr )) return hr;
    if (FAILED(hr = verify_identity( staging, TRUE, staging_identity )))
        return hr;
    if (FAILED(hr = backend_rename( backend, staging, store->payloads,
                                    generation, FALSE )))
        return hr;
    if (FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->staging, TRUE ),
            weak )) ||
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->payloads, TRUE ),
            weak )))
        return hr;
    if (FAILED(hr = open_path_component(
            store->payloads, generation, FALSE, &destination,
            &destination_identity )))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (destination_identity.volume_serial != staging_identity->volume_serial ||
        destination_identity.file_index_high !=
            staging_identity->file_index_high ||
        destination_identity.file_index_low != staging_identity->file_index_low)
    {
        TRACE( "published staging identity changed: source volume %#lx "
               "index %08lx:%08lx, destination volume %#lx "
               "index %08lx:%08lx.\n",
               staging_identity->volume_serial,
               staging_identity->file_index_high,
               staging_identity->file_index_low,
               destination_identity.volume_serial,
               destination_identity.file_index_high,
               destination_identity.file_index_low );
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    }
    close_handle( &destination );
    return hr;
}

static HRESULT write_generation_marker(
    struct deployment_backend *backend, HANDLE marker,
    const struct appx_catalog_package *package )
{
    BYTE header[DEPLOYMENT_MARKER_HEADER_SIZE];
    UINT32 full_name_chars;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length(
            package->full_name, FALSE, &full_name_chars )))
        return hr;
    memset( header, 0, sizeof(header) );
    memcpy( header + MARKER_MAGIC_OFFSET, generation_marker_magic,
            sizeof(generation_marker_magic) );
    write_uint32( header + MARKER_VERSION_OFFSET,
                  DEPLOYMENT_MARKER_VERSION );
    memcpy( header + MARKER_CONTENT_ID_OFFSET, package->content_id,
            sizeof(package->content_id) );
    if (FAILED(hr = backend_write_all(
            backend, marker, header, sizeof(header) )))
        return hr;
    return backend_write_all(
        backend, marker, (const BYTE *)package->full_name,
        full_name_chars * sizeof(*package->full_name) );
}

static HRESULT validate_generation_marker(
    HANDLE marker, const struct appx_catalog_package *package,
    struct object_identity *identity )
{
    LARGE_INTEGER zero;
    struct object_identity current;
    BYTE header[DEPLOYMENT_MARKER_HEADER_SIZE];
    WCHAR *full_name = NULL;
    UINT32 full_name_chars;
    UINT64 expected_size;
    DWORD links, read;
    UINT64 size;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length(
            package->full_name, FALSE, &full_name_chars )))
        return hr;
    expected_size = sizeof(header) +
        (UINT64)full_name_chars * sizeof(*full_name);
    if (FAILED(hr = get_identity( marker, FALSE, &current, &links, &size )) ||
        links != 1 || size != expected_size)
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (!(full_name = HeapAlloc(
            GetProcessHeap(), 0, full_name_chars * sizeof(*full_name) )))
        return E_OUTOFMEMORY;
    zero.QuadPart = 0;
    if (!SetFilePointerEx( marker, zero, NULL, FILE_BEGIN ) ||
        !ReadFile( marker, header, sizeof(header), &read, NULL ) ||
        read != sizeof(header) ||
        !ReadFile( marker, full_name,
                   full_name_chars * sizeof(*full_name), &read, NULL ) ||
        read != full_name_chars * sizeof(*full_name) ||
        memcmp( header + MARKER_MAGIC_OFFSET, generation_marker_magic,
                sizeof(generation_marker_magic) ) ||
        read_uint32( header + MARKER_VERSION_OFFSET ) !=
            DEPLOYMENT_MARKER_VERSION ||
        !equal_content_id( header + MARKER_CONTENT_ID_OFFSET,
                           package->content_id ) ||
        memcmp( full_name, package->full_name,
                full_name_chars * sizeof(*full_name) ) ||
        FAILED(verify_identity( marker, FALSE, &current )))
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
    else
    {
        if (identity) *identity = current;
        hr = S_OK;
    }
    HeapFree( GetProcessHeap(), 0, full_name );
    return hr;
}

static HRESULT create_pending_generation_marker(
    struct deployment_store *store, struct deployment_backend *backend,
    const struct deployment_journal *journal,
    const struct appx_catalog_package *package,
    HANDLE *marker, struct object_identity *identity, BOOL *weak )
{
    HRESULT hr;

    *marker = INVALID_HANDLE_VALUE;
    if (FAILED(hr = open_child(
            store->lease_pending, journal->transaction_name,
            FILE_READ_DATA | FILE_WRITE_DATA | DELETE, 0, FILE_CREATE,
            FALSE, marker, identity )) ||
        FAILED(hr = require_single_link_file( *marker )) ||
        FAILED(hr = write_generation_marker( backend, *marker, package )) ||
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, *marker, FALSE ), weak )) ||
        FAILED(hr = validate_generation_marker(
            *marker, package, identity )) ||
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->lease_pending, TRUE ),
            weak )))
    {
        close_handle( marker );
        return hr;
    }
    return S_OK;
}

static HRESULT publish_generation_marker(
    struct deployment_store *store, struct deployment_backend *backend,
    HANDLE *pending, const struct object_identity *pending_identity,
    const struct appx_catalog_package *package, BOOL *reused, BOOL *weak )
{
    const WCHAR *generation = payload_generation_name( package->payload_path );
    struct object_identity destination_identity;
    HANDLE destination = INVALID_HANDLE_VALUE;
    HRESULT hr;

    *reused = FALSE;
    if (!generation) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    hr = open_child( store->lease_generations, generation,
                     FILE_READ_DATA | DELETE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     FILE_OPEN, FALSE, &destination,
                     &destination_identity );
    if (SUCCEEDED(hr))
    {
        hr = validate_generation_marker(
            destination, package, &destination_identity );
        close_handle( &destination );
        if (FAILED(hr)) return hr;
        close_handle( pending );
        *reused = TRUE;
        return S_OK;
    }
    if (!is_not_found( hr )) return hr;
    if (FAILED(hr = validate_generation_marker(
            *pending, package, NULL )) ||
        FAILED(hr = verify_identity( *pending, FALSE, pending_identity )))
        return hr;
    if (FAILED(hr = backend_rename(
            backend, *pending, store->lease_generations, generation,
            FALSE )))
        return hr;
    if (FAILED(hr = mark_durability(
            backend,
            backend_flush( backend, store->lease_pending, TRUE ), weak )) ||
        FAILED(hr = mark_durability(
            backend,
            backend_flush( backend, store->lease_generations, TRUE ), weak )))
        return hr;
    close_handle( pending );
    if (FAILED(hr = open_child(
            store->lease_generations, generation, FILE_READ_DATA | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_OPEN, FALSE, &destination, &destination_identity )))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = validate_generation_marker(
            destination, package, &destination_identity )))
        goto done;
    if (destination_identity.volume_serial != pending_identity->volume_serial ||
        destination_identity.file_index_high !=
            pending_identity->file_index_high ||
        destination_identity.file_index_low != pending_identity->file_index_low)
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );

done:
    close_handle( &destination );
    return hr;
}

static HRESULT delete_generation_marker(
    struct deployment_store *store, struct deployment_backend *backend,
    const WCHAR *generation, struct delete_budget *budget, BOOL *weak )
{
    return delete_named_file( store->lease_generations, generation, budget,
                              backend, weak );
}

static HRESULT open_directory_entry(
    HANDLE parent, const FILE_ID_BOTH_DIRECTORY_INFORMATION *entry, HANDLE *handle,
    struct object_identity *identity, DWORD *links, UINT64 *size )
{
    WCHAR name[WINE_APPX_MAX_COMPONENT_CHARS + 1];
    UINT32 chars = entry->FileNameLength / sizeof(WCHAR);
    BOOL directory = !!(entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    UINT64 file_id;
    HRESULT hr;

    *handle = INVALID_HANDLE_VALUE;
    if (entry->FileNameLength % sizeof(WCHAR) ||
        !chars || chars > WINE_APPX_MAX_COMPONENT_CHARS)
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    memcpy( name, entry->FileName, entry->FileNameLength );
    name[chars] = 0;
    if ((chars == 1 && name[0] == '.') ||
        (chars == 2 && name[0] == '.' && name[1] == '.'))
        return S_FALSE;
    if (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    if (FAILED(hr = open_child(
            parent, name, DELETE |
            (directory ? FILE_LIST_DIRECTORY | FILE_TRAVERSE :
                         FILE_READ_DATA),
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_OPEN, directory, handle, identity )))
        return hr;
    if (FAILED(hr = get_identity( *handle, directory, identity, links, size )))
        goto failed;
    file_id = identity->file_index_low |
              ((UINT64)identity->file_index_high << 32);
    if (entry->FileId.QuadPart && file_id != entry->FileId.QuadPart)
    {
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
        goto failed;
    }
    return S_OK;

failed:
    close_handle( handle );
    return hr;
}

static HRESULT delete_directory_contents( HANDLE directory,
                                          struct delete_budget *budget,
                                          UINT32 depth )
{
    BYTE *buffer = NULL;
    IO_STATUS_BLOCK io;
    BOOL restart, deleted_any;
    HRESULT hr = S_OK;
    NTSTATUS status;

    if (depth > 1024)
        return HRESULT_FROM_WIN32( ERROR_MORE_DATA );
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0,
                              DEPLOYMENT_ENUM_BUFFER_SIZE )))
        return E_OUTOFMEMORY;
    do
    {
        restart = TRUE;
        deleted_any = FALSE;
        for (;;)
        {
            FILE_ID_BOTH_DIRECTORY_INFORMATION *entry;
            ULONG offset = 0;

            status = NtQueryDirectoryFile(
                directory, NULL, NULL, NULL, &io, buffer,
                DEPLOYMENT_ENUM_BUFFER_SIZE, FileIdBothDirectoryInformation,
                FALSE, NULL, restart );
            restart = FALSE;
            if (status == STATUS_NO_MORE_FILES) break;
            if (status == STATUS_PENDING)
            {
                hr = HRESULT_FROM_WIN32( ERROR_TIMEOUT );
                goto done;
            }
            if (status)
            {
                hr = ntstatus_error( status );
                goto done;
            }
            for (;;)
            {
                struct object_identity identity;
                HANDLE child = INVALID_HANDLE_VALUE;
                DWORD links = 0;
                UINT64 size = 0;
                BOOL is_directory;

                entry = (FILE_ID_BOTH_DIRECTORY_INFORMATION *)(buffer + offset);
                hr = open_directory_entry( directory, entry, &child,
                                           &identity, &links, &size );
                if (hr == S_FALSE)
                {
                    hr = S_OK;
                    goto next_entry;
                }
                if (FAILED(hr)) goto done;
                if (budget->entries >= budget->max_entries)
                {
                    close_handle( &child );
                    hr = HRESULT_FROM_WIN32( ERROR_MORE_DATA );
                    goto done;
                }
                budget->entries++;
                is_directory =
                    !!(identity.attributes & FILE_ATTRIBUTE_DIRECTORY);
                if (!is_directory)
                {
                    if (links != 1)
                    {
                        close_handle( &child );
                        hr = HRESULT_FROM_WIN32( ERROR_SHARING_VIOLATION );
                        goto done;
                    }
                    if (size > budget->max_bytes - budget->bytes)
                    {
                        close_handle( &child );
                        hr = HRESULT_FROM_WIN32( ERROR_MORE_DATA );
                        goto done;
                    }
                    budget->bytes += size;
                }
                else if (FAILED(hr = delete_directory_contents(
                                    child, budget, depth + 1 )))
                {
                    close_handle( &child );
                    goto done;
                }
                if (FAILED(hr = verify_identity( child, is_directory,
                                                 &identity )) ||
                    FAILED(hr = delete_open_object( child )))
                {
                    close_handle( &child );
                    goto done;
                }
                close_handle( &child );
                deleted_any = TRUE;

next_entry:
                if (!entry->NextEntryOffset) break;
                if (entry->NextEntryOffset >
                    DEPLOYMENT_ENUM_BUFFER_SIZE - offset)
                {
                    hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                    goto done;
                }
                offset += entry->NextEntryOffset;
            }
        }
        /*
         * Deleting while enumerating may cause a filesystem cursor to skip a
         * name.  Restart once more until a complete pass deletes nothing.
         * The global entry budget makes the loop finite under hostile churn.
         */
    } while (deleted_any);

done:
    HeapFree( GetProcessHeap(), 0, buffer );
    return hr;
}

static HRESULT delete_named_tree(
    HANDLE parent, const WCHAR *name, struct delete_budget *budget,
    struct deployment_backend *backend, BOOL *weak )
{
    struct object_identity identity;
    HANDLE directory = INVALID_HANDLE_VALUE;
    HRESULT hr;

    hr = open_path_component( parent, name, FALSE, &directory, &identity );
    if (is_not_found( hr )) return S_OK;
    if (FAILED(hr)) return hr;
    if (FAILED(hr = delete_directory_contents( directory, budget, 0 )) ||
        FAILED(hr = verify_identity( directory, TRUE, &identity )) ||
        FAILED(hr = delete_open_object( directory )))
        goto done;
    hr = mark_durability(
        backend, backend_flush( backend, parent, TRUE ), weak );

done:
    close_handle( &directory );
    return hr;
}

static HRESULT move_generation_to_quarantine(
    struct deployment_store *store, struct deployment_backend *backend,
    const WCHAR *generation, BOOL *weak )
{
    struct object_identity identity, destination_identity;
    HANDLE source = INVALID_HANDLE_VALUE, destination = INVALID_HANDLE_VALUE;
    HRESULT hr;

    hr = open_path_component( store->payloads, generation, FALSE,
                              &source, &identity );
    if (is_not_found( hr )) return S_OK;
    if (FAILED(hr)) return hr;
    hr = open_path_component( store->quarantine, generation, FALSE,
                              &destination, &destination_identity );
    if (SUCCEEDED(hr))
    {
        /*
         * Never merge or replace quarantined evidence.  The caller may retry
         * bounded collection after an operator resolves the collision.
         */
        close_handle( &destination );
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (!is_not_found( hr )) goto done;
    if (FAILED(hr = backend_rename( backend, source, store->quarantine,
                                    generation, FALSE )))
        goto done;
    if (FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->payloads, TRUE ),
            weak )) ||
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->quarantine, TRUE ),
            weak )))
        goto done;
    if (FAILED(hr = open_path_component(
            store->quarantine, generation, FALSE, &destination,
            &destination_identity )))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (destination_identity.volume_serial != identity.volume_serial ||
        destination_identity.file_index_high != identity.file_index_high ||
        destination_identity.file_index_low != identity.file_index_low)
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );

done:
    close_handle( &destination );
    close_handle( &source );
    return hr;
}

static HRESULT move_record_to_quarantine(
    struct deployment_store *store, struct deployment_backend *backend,
    const WCHAR *generation, BOOL *weak )
{
    WCHAR name[DEPLOYMENT_GENERATION_CHARS +
               ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
    struct object_identity identity, destination_identity;
    HANDLE source = INVALID_HANDLE_VALUE, destination = INVALID_HANDLE_VALUE;
    HRESULT hr;

    lstrcpyW( name, generation );
    lstrcatW( name, DEPLOYMENT_JOURNAL_SUFFIX );
    hr = open_child( store->records, name, FILE_READ_DATA | DELETE,
                     FILE_SHARE_READ | FILE_SHARE_DELETE,
                     FILE_OPEN, FALSE, &source, &identity );
    if (is_not_found( hr )) return S_OK;
    if (FAILED(hr)) return hr;
    hr = open_child( store->record_quarantine, name, FILE_READ_DATA,
                     FILE_SHARE_READ | FILE_SHARE_DELETE,
                     FILE_OPEN, FALSE, &destination,
                     &destination_identity );
    if (SUCCEEDED(hr))
    {
        close_handle( &destination );
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (!is_not_found( hr )) goto done;
    if (FAILED(hr = backend_rename(
            backend, source, store->record_quarantine, name, FALSE )) ||
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->records, TRUE ),
            weak )) ||
        FAILED(hr = mark_durability(
            backend,
            backend_flush( backend, store->record_quarantine, TRUE ),
            weak )))
        goto done;
    if (FAILED(hr = open_child(
            store->record_quarantine, name, FILE_READ_DATA,
            FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN, FALSE,
            &destination, &destination_identity )))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (destination_identity.volume_serial != identity.volume_serial ||
        destination_identity.file_index_high != identity.file_index_high ||
        destination_identity.file_index_low != identity.file_index_low)
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );

done:
    close_handle( &destination );
    close_handle( &source );
    return hr;
}

static HRESULT delete_named_file(
    HANDLE parent, const WCHAR *name, struct delete_budget *budget,
    struct deployment_backend *backend, BOOL *weak )
{
    struct object_identity identity;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD links;
    UINT64 size;
    HRESULT hr;

    hr = open_child( parent, name, FILE_READ_DATA | DELETE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     FILE_OPEN, FALSE, &file, &identity );
    if (is_not_found( hr )) return S_OK;
    if (FAILED(hr)) return hr;
    if (FAILED(hr = get_identity(
            file, FALSE, &identity, &links, &size )))
        goto done;
    if (links != 1)
    {
        hr = HRESULT_FROM_WIN32( ERROR_SHARING_VIOLATION );
        goto done;
    }
    if (budget->entries >= budget->max_entries ||
        size > budget->max_bytes - budget->bytes)
    {
        hr = HRESULT_FROM_WIN32( ERROR_MORE_DATA );
        goto done;
    }
    if (FAILED(hr = verify_identity( file, FALSE, &identity )) ||
        FAILED(hr = delete_open_object( file )))
        goto done;
    budget->entries++;
    budget->bytes += size;
    hr = mark_durability(
        backend, backend_flush( backend, parent, TRUE ), weak );

done:
    close_handle( &file );
    return hr;
}

static BOOL catalog_references_payload( const APPX_CATALOG_SNAPSHOT *catalog,
                                        const WCHAR *payload_path )
{
    UINT32 count = appx_catalog_snapshot_get_package_count( catalog ), i;

    for (i = 0; i < count; i++)
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( catalog, i );

        if (package && (package->flags & APPX_CATALOG_PACKAGE_ACTIVE) &&
            !compare_string_ci( package->payload_path, payload_path ))
            return TRUE;
    }
    return FALSE;
}

static HRESULT pin_catalog_file( struct deployment_store *store, HANDLE *file,
                                 struct object_identity *identity,
                                 BOOL *present )
{
    HRESULT hr;

    *file = INVALID_HANDLE_VALUE;
    *present = FALSE;
    hr = open_child( store->root, APPX_CATALOG_FILE_NAME, FILE_READ_DATA,
                     FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN, FALSE,
                     file, identity );
    if (is_not_found( hr )) return S_OK;
    if (FAILED(hr)) return hr;
    if (FAILED(hr = require_single_link_file( *file )))
    {
        close_handle( file );
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }
    *present = TRUE;
    return S_OK;
}

static BOOL same_catalog_file(
    BOOL left_present, const struct object_identity *left,
    BOOL right_present, const struct object_identity *right )
{
    if (left_present != right_present) return FALSE;
    if (!left_present) return TRUE;
    return left->volume_serial == right->volume_serial &&
           left->file_index_high == right->file_index_high &&
           left->file_index_low == right->file_index_low;
}

static HRESULT generation_in_use( struct gc_context *context,
                                  const WCHAR *generation, BOOL *in_use )
{
    HANDLE lease = INVALID_HANDLE_VALUE;
    HRESULT hr;

    *in_use = FALSE;
    if (context->backend->test &&
        context->backend->test->generation_in_use)
        return context->backend->test->generation_in_use(
            context->backend->test->context, generation, in_use );
    hr = open_child( context->store->lease_generations, generation,
                     DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE |
                     FILE_SHARE_DELETE, FILE_OPEN, FALSE, &lease, NULL );
    if (is_not_found( hr )) return S_OK;
    if (hr == HRESULT_FROM_WIN32( ERROR_SHARING_VIOLATION ))
    {
        *in_use = TRUE;
        return S_OK;
    }
    if (FAILED(hr))
    {
        *in_use = TRUE;
        return S_OK;
    }
    close_handle( &lease );
    return S_OK;
}

struct generation_name
{
    WCHAR name[DEPLOYMENT_GENERATION_CHARS + 1];
};

static HRESULT quarantine_generation_if_unreferenced(
    struct gc_context *context, const WCHAR *generation, BOOL *moved )
{
    APPX_CATALOG_SNAPSHOT *fresh = NULL;
    struct deployment_lock catalog_lock, writer_lock;
    struct object_identity before_identity, after_identity, locked_identity;
    HANDLE before = INVALID_HANDLE_VALUE, after = INVALID_HANDLE_VALUE;
    HANDLE locked = INVALID_HANDLE_VALUE;
    WCHAR payload_path[ARRAY_SIZE(L"payloads\\") +
                       DEPLOYMENT_GENERATION_CHARS];
    BOOL before_present = FALSE, after_present = FALSE;
    BOOL locked_present = FALSE;
    UINT32 attempt;
    HRESULT hr;

    *moved = FALSE;
    memset( &catalog_lock, 0, sizeof(catalog_lock) );
    memset( &writer_lock, 0, sizeof(writer_lock) );
    lstrcpyW( payload_path, L"payloads\\" );
    lstrcatW( payload_path, generation );
    if (FAILED(hr = acquire_writer_lock(
            context->store, context->options, &writer_lock )))
        goto done;
    for (attempt = 0; attempt < context->options->max_epoch_retries;
         attempt++)
    {
        release_store_lock( &catalog_lock );
        close_handle( &locked );
        close_handle( &after );
        close_handle( &before );
        appx_catalog_snapshot_free( fresh );
        fresh = NULL;
        before_present = after_present = locked_present = FALSE;

        if (FAILED(hr = pin_catalog_file(
                context->store, &before, &before_identity,
                &before_present )) ||
            FAILED(hr = backend_catalog_load(
                context->backend, context->options, context->store->path,
                &fresh )) ||
            FAILED(hr = pin_catalog_file(
                context->store, &after, &after_identity,
                &after_present )))
            goto done;
        if (!same_catalog_file(
                before_present, &before_identity,
                after_present, &after_identity ))
            continue;
        if (FAILED(hr = acquire_lock_byte(
                context->store, context->options, 0, &catalog_lock )) ||
            FAILED(hr = pin_catalog_file(
                context->store, &locked, &locked_identity,
                &locked_present )))
            goto done;
        if (!same_catalog_file(
                after_present, &after_identity,
                locked_present, &locked_identity ))
            continue;
        if (FAILED(hr = validate_store_snapshot(
                context->store, fresh )))
            goto done;
        if (catalog_references_payload( fresh, payload_path ))
        {
            hr = S_OK;
            goto done;
        }
        hr = delete_generation_marker(
            context->store, context->backend, generation,
            &context->budget, &context->weak );
        if (is_sharing_failure( hr ) ||
            hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA))
        {
            context->deferred++;
            hr = S_OK;
            goto done;
        }
        if (FAILED(hr)) goto done;
        if (FAILED(hr = move_generation_to_quarantine(
                context->store, context->backend, generation,
                &context->weak )) ||
            FAILED(hr = move_record_to_quarantine(
                context->store, context->backend, generation,
                &context->weak )))
            goto done;
        *moved = TRUE;
        hr = S_OK;
        goto done;
    }
    hr = APPX_CATALOG_E_EPOCH_CONFLICT;

done:
    release_store_lock( &catalog_lock );
    close_handle( &locked );
    close_handle( &after );
    close_handle( &before );
    appx_catalog_snapshot_free( fresh );
    release_store_lock( &writer_lock );
    return hr;
}

static BOOL valid_record_file_name( const WCHAR *name, UINT32 chars )
{
    UINT32 suffix_chars = ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX) - 1;
    UINT32 i;

    if (chars != DEPLOYMENT_GENERATION_CHARS + suffix_chars ||
        memcmp( name + DEPLOYMENT_GENERATION_CHARS,
                DEPLOYMENT_JOURNAL_SUFFIX,
                suffix_chars * sizeof(WCHAR) ))
        return FALSE;
    for (i = 0; i < DEPLOYMENT_GENERATION_CHARS; i++)
        if (!((name[i] >= '0' && name[i] <= '9') ||
              (name[i] >= 'a' && name[i] <= 'f')))
            return FALSE;
    return TRUE;
}

static HRESULT enumerate_record_generations(
    HANDLE directory, UINT32 maximum, struct generation_name **names,
    UINT32 *count, BOOL *truncated )
{
    BYTE *buffer = NULL;
    struct generation_name *array = NULL;
    UINT32 capacity = 0;
    IO_STATUS_BLOCK io;
    BOOL restart = TRUE;
    HRESULT hr = S_OK;
    NTSTATUS status;

    *names = NULL;
    *count = 0;
    *truncated = FALSE;
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0,
                              DEPLOYMENT_ENUM_BUFFER_SIZE )))
        return E_OUTOFMEMORY;
    for (;;)
    {
        FILE_ID_BOTH_DIRECTORY_INFORMATION *entry;
        ULONG offset = 0;

        status = NtQueryDirectoryFile(
            directory, NULL, NULL, NULL, &io, buffer,
            DEPLOYMENT_ENUM_BUFFER_SIZE, FileIdBothDirectoryInformation,
            FALSE, NULL, restart );
        restart = FALSE;
        if (status == STATUS_NO_MORE_FILES) break;
        if (status == STATUS_PENDING)
        {
            hr = HRESULT_FROM_WIN32( ERROR_TIMEOUT );
            goto done;
        }
        if (status)
        {
            hr = ntstatus_error( status );
            goto done;
        }
        for (;;)
        {
            UINT32 chars;

            entry = (FILE_ID_BOTH_DIRECTORY_INFORMATION *)(buffer + offset);
            chars = entry->FileNameLength / sizeof(WCHAR);
            if ((chars == 1 && entry->FileName[0] == '.') ||
                (chars == 2 && entry->FileName[0] == '.' &&
                 entry->FileName[1] == '.'))
                goto next_entry;
            if (entry->FileNameLength % sizeof(WCHAR) ||
                (entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                          FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !valid_record_file_name( entry->FileName, chars ))
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            if (*count >= maximum)
            {
                *truncated = TRUE;
                goto done;
            }
            if (*count == capacity)
            {
                struct generation_name *new_array;
                UINT32 new_capacity = capacity ? capacity * 2 : 32;

                if (new_capacity > maximum) new_capacity = maximum;
                if (!(new_array = HeapReAlloc(
                        GetProcessHeap(), 0, array,
                        new_capacity * sizeof(*new_array) )))
                {
                    if (array)
                    {
                        hr = E_OUTOFMEMORY;
                        goto done;
                    }
                    if (!(new_array = HeapAlloc(
                            GetProcessHeap(), 0,
                            new_capacity * sizeof(*new_array) )))
                    {
                        hr = E_OUTOFMEMORY;
                        goto done;
                    }
                }
                array = new_array;
                capacity = new_capacity;
            }
            memcpy( array[*count].name, entry->FileName,
                    DEPLOYMENT_GENERATION_CHARS * sizeof(WCHAR) );
            array[*count].name[DEPLOYMENT_GENERATION_CHARS] = 0;
            (*count)++;

next_entry:
            if (!entry->NextEntryOffset) break;
            if (entry->NextEntryOffset >
                DEPLOYMENT_ENUM_BUFFER_SIZE - offset)
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            offset += entry->NextEntryOffset;
        }
    }

done:
    HeapFree( GetProcessHeap(), 0, buffer );
    if (FAILED(hr))
    {
        HeapFree( GetProcessHeap(), 0, array );
        array = NULL;
        *count = 0;
    }
    *names = array;
    return hr;
}

static HRESULT enumerate_generation_names(
    HANDLE directory, UINT32 maximum, BOOL directory_entries,
    struct generation_name **names, UINT32 *count, BOOL *truncated )
{
    BYTE *buffer = NULL;
    struct generation_name *array = NULL;
    UINT32 capacity = 0;
    IO_STATUS_BLOCK io;
    BOOL restart = TRUE;
    HRESULT hr = S_OK;
    NTSTATUS status;

    *names = NULL;
    *count = 0;
    *truncated = FALSE;
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0,
                              DEPLOYMENT_ENUM_BUFFER_SIZE )))
        return E_OUTOFMEMORY;
    for (;;)
    {
        FILE_ID_BOTH_DIRECTORY_INFORMATION *entry;
        ULONG offset = 0;

        status = NtQueryDirectoryFile(
            directory, NULL, NULL, NULL, &io, buffer,
            DEPLOYMENT_ENUM_BUFFER_SIZE, FileIdBothDirectoryInformation,
            FALSE, NULL, restart );
        restart = FALSE;
        if (status == STATUS_NO_MORE_FILES) break;
        if (status == STATUS_PENDING)
        {
            hr = HRESULT_FROM_WIN32( ERROR_TIMEOUT );
            goto done;
        }
        if (status)
        {
            hr = ntstatus_error( status );
            goto done;
        }
        for (;;)
        {
            UINT32 chars;

            entry = (FILE_ID_BOTH_DIRECTORY_INFORMATION *)(buffer + offset);
            chars = entry->FileNameLength / sizeof(WCHAR);
            if ((chars == 1 && entry->FileName[0] == '.') ||
                (chars == 2 && entry->FileName[0] == '.' &&
                 entry->FileName[1] == '.'))
                goto next_entry;
            if (entry->FileNameLength % sizeof(WCHAR) ||
                chars != DEPLOYMENT_GENERATION_CHARS)
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            if (!!(entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) !=
                    directory_entries ||
                (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                if (!directory_entries) goto next_entry;
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            if (*count >= maximum)
            {
                *truncated = TRUE;
                goto done;
            }
            if (*count == capacity)
            {
                struct generation_name *new_array;
                UINT32 new_capacity = capacity ? capacity * 2 : 32;

                if (new_capacity > maximum) new_capacity = maximum;
                if (!(new_array = HeapReAlloc(
                        GetProcessHeap(), 0, array,
                        new_capacity * sizeof(*new_array) )))
                {
                    if (array)
                    {
                        hr = E_OUTOFMEMORY;
                        goto done;
                    }
                    if (!(new_array = HeapAlloc(
                            GetProcessHeap(), 0,
                            new_capacity * sizeof(*new_array) )))
                    {
                        hr = E_OUTOFMEMORY;
                        goto done;
                    }
                }
                array = new_array;
                capacity = new_capacity;
            }
            memcpy( array[*count].name, entry->FileName,
                    entry->FileNameLength );
            array[*count].name[chars] = 0;
            if (!is_lower_hex_name( array[*count].name,
                                    DEPLOYMENT_GENERATION_CHARS ))
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            (*count)++;

next_entry:
            if (!entry->NextEntryOffset) break;
            if (entry->NextEntryOffset >
                DEPLOYMENT_ENUM_BUFFER_SIZE - offset)
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            offset += entry->NextEntryOffset;
        }
    }

done:
    HeapFree( GetProcessHeap(), 0, buffer );
    if (FAILED(hr))
    {
        HeapFree( GetProcessHeap(), 0, array );
        array = NULL;
        *count = 0;
    }
    *names = array;
    return hr;
}

static HRESULT verify_catalog_payloads(
    struct deployment_store *store,
    const APPX_CATALOG_SNAPSHOT *catalog )
{
    UINT32 count = appx_catalog_snapshot_get_package_count( catalog ), i;

    for (i = 0; i < count; i++)
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( catalog, i );
        struct object_identity identity;
        WCHAR *expected = NULL;
        const WCHAR *generation;
        HANDLE directory = INVALID_HANDLE_VALUE;
        HANDLE marker = INVALID_HANDLE_VALUE;
        HRESULT hr;

        if (!package) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        if (FAILED(hr = derive_payload_path(
                package->full_name, package->content_id, &expected )))
            return hr;
        if (lstrcmpW( expected, package->payload_path ) ||
            !(generation = payload_generation_name( package->payload_path )))
        {
            HeapFree( GetProcessHeap(), 0, expected );
            return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        }
        HeapFree( GetProcessHeap(), 0, expected );
        if (FAILED(hr = open_path_component(
                store->payloads, generation, FALSE, &directory, &identity )))
            return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        hr = verify_identity( directory, TRUE, &identity );
        close_handle( &directory );
        if (FAILED(hr)) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        if (FAILED(hr = open_child(
                store->lease_generations, generation, FILE_READ_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                FILE_OPEN, FALSE, &marker, &identity )))
            return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        hr = validate_generation_marker( marker, package, &identity );
        close_handle( &marker );
        if (FAILED(hr)) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }
    return S_OK;
}

static HRESULT verify_catalog_records(
    struct deployment_store *store,
    const APPX_CATALOG_SNAPSHOT *catalog )
{
    UINT32 count = appx_catalog_snapshot_get_package_count( catalog ), i;

    for (i = 0; i < count; i++)
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( catalog, i );
        APPX_DEPLOYMENT_RECORD *record = NULL;
        HRESULT hr;

        if (!package) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        hr = load_record_from_store( store, package, &record );
        if (SUCCEEDED(hr))
            hr = verify_record_inventory( store, package, record );
        appx_deployment_record_free( record );
        if (FAILED(hr)) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }
    return S_OK;
}

static HRESULT collect_generation_list(
    struct gc_context *context, HANDLE directory, BOOL payload_directory )
{
    struct generation_name *names = NULL;
    UINT32 count = 0, i;
    BOOL truncated;
    HRESULT hr;

    if (FAILED(hr = enumerate_generation_names(
            directory, context->options->max_gc_entries,
            TRUE, &names, &count, &truncated )))
        return hr;
    if (truncated)
    {
        context->deferred++;
        if (!count)
        {
            HeapFree( GetProcessHeap(), 0, names );
            return S_OK;
        }
    }
    for (i = 0; i < count; i++)
    {
        WCHAR payload_path[ARRAY_SIZE(L"payloads\\") +
                           DEPLOYMENT_GENERATION_CHARS];
        BOOL in_use;

        if (FAILED(hr = check_cancelled( context->options ))) break;
        lstrcpyW( payload_path, L"payloads\\" );
        lstrcatW( payload_path, names[i].name );
        if (payload_directory &&
            catalog_references_payload( context->catalog, payload_path ))
            continue;
        if (FAILED(hr = generation_in_use(
                context, names[i].name, &in_use )))
        {
            /*
             * Uncertain lease state always defers reclamation.  Access and
             * sharing failures are not proof that a generation is unused.
             */
            context->deferred++;
            hr = S_OK;
            continue;
        }
        if (in_use)
        {
            context->deferred++;
            continue;
        }
        if (payload_directory)
        {
            BOOL moved;

            hr = quarantine_generation_if_unreferenced(
                context, names[i].name, &moved );
            if (is_sharing_failure( hr ))
            {
                context->deferred++;
                hr = S_OK;
                continue;
            }
            if (FAILED(hr)) break;
            if (!moved) continue;
        }
        hr = delete_generation_marker(
            context->store, context->backend, names[i].name,
            &context->budget, &context->weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            context->deferred++;
            hr = S_OK;
            continue;
        }
        if (FAILED(hr)) break;
        hr = delete_named_tree(
            context->store->quarantine, names[i].name, &context->budget,
            context->backend, &context->weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            context->deferred++;
            hr = S_OK;
            continue;
        }
        if (FAILED(hr)) break;
        {
            WCHAR record_name[DEPLOYMENT_GENERATION_CHARS +
                              ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];

            lstrcpyW( record_name, names[i].name );
            lstrcatW( record_name, DEPLOYMENT_JOURNAL_SUFFIX );
            hr = delete_named_file(
                context->store->record_quarantine, record_name,
                &context->budget, context->backend, &context->weak );
            if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
                is_sharing_failure( hr ))
            {
                context->deferred++;
                hr = S_OK;
                continue;
            }
            if (FAILED(hr)) break;
        }
    }
    HeapFree( GetProcessHeap(), 0, names );
    return hr;
}

static HRESULT directory_has_generation( HANDLE directory,
                                         const WCHAR *generation,
                                         BOOL *present )
{
    struct object_identity identity;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HRESULT hr;

    *present = FALSE;
    hr = open_path_component( directory, generation, FALSE,
                              &handle, &identity );
    if (is_not_found( hr )) return S_OK;
    if (FAILED(hr)) return hr;
    if (FAILED(hr = verify_identity( handle, TRUE, &identity )))
    {
        close_handle( &handle );
        return hr;
    }
    *present = TRUE;
    close_handle( &handle );
    return S_OK;
}

static HRESULT collect_record_list( struct gc_context *context,
                                    BOOL published )
{
    struct generation_name *names = NULL;
    HANDLE directory = published ? context->store->records :
                                   context->store->record_quarantine;
    UINT32 count = 0, i;
    BOOL truncated;
    HRESULT hr;

    if (FAILED(hr = enumerate_record_generations(
            directory, context->options->max_gc_entries,
            &names, &count, &truncated )))
        return hr;
    if (truncated) context->deferred++;
    for (i = 0; i < count; i++)
    {
        WCHAR payload_path[ARRAY_SIZE(L"payloads\\") +
                           DEPLOYMENT_GENERATION_CHARS];
        WCHAR record_name[DEPLOYMENT_GENERATION_CHARS +
                          ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
        BOOL paired, in_use;

        if (FAILED(hr = check_cancelled( context->options ))) break;
        lstrcpyW( payload_path, L"payloads\\" );
        lstrcatW( payload_path, names[i].name );
        if (published &&
            catalog_references_payload( context->catalog, payload_path ))
            continue;
        if (FAILED(hr = generation_in_use(
                context, names[i].name, &in_use )))
        {
            context->deferred++;
            hr = S_OK;
            continue;
        }
        if (in_use)
        {
            context->deferred++;
            continue;
        }
        if (published)
        {
            BOOL moved;

            hr = quarantine_generation_if_unreferenced(
                context, names[i].name, &moved );
            if (is_sharing_failure( hr ))
            {
                context->deferred++;
                hr = S_OK;
                continue;
            }
            if (FAILED(hr)) break;
            if (!moved) continue;
            continue;
        }
        if (FAILED(hr = directory_has_generation(
                context->store->quarantine, names[i].name, &paired )))
            break;
        if (paired) continue;
        hr = delete_generation_marker(
            context->store, context->backend, names[i].name,
            &context->budget, &context->weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            context->deferred++;
            hr = S_OK;
            continue;
        }
        if (FAILED(hr)) break;
        lstrcpyW( record_name, names[i].name );
        lstrcatW( record_name, DEPLOYMENT_JOURNAL_SUFFIX );
        hr = delete_named_file(
            context->store->record_quarantine, record_name,
            &context->budget, context->backend, &context->weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            context->deferred++;
            hr = S_OK;
            continue;
        }
        if (FAILED(hr)) break;
    }
    HeapFree( GetProcessHeap(), 0, names );
    return hr;
}

static HRESULT collect_marker_list( struct gc_context *context )
{
    struct generation_name *names = NULL;
    UINT32 count = 0, i;
    BOOL truncated;
    HRESULT hr;

    if (FAILED(hr = enumerate_generation_names(
            context->store->lease_generations,
            context->options->max_gc_entries, FALSE,
            &names, &count, &truncated )))
        return hr;
    if (truncated) context->deferred++;
    for (i = 0; i < count; i++)
    {
        WCHAR payload_path[ARRAY_SIZE(L"payloads\\") +
                           DEPLOYMENT_GENERATION_CHARS];
        BOOL in_use;

        if (FAILED(hr = check_cancelled( context->options ))) break;
        lstrcpyW( payload_path, L"payloads\\" );
        lstrcatW( payload_path, names[i].name );
        if (catalog_references_payload( context->catalog, payload_path ))
            continue;
        if (FAILED(hr = generation_in_use(
                context, names[i].name, &in_use )))
        {
            context->deferred++;
            hr = S_OK;
            continue;
        }
        if (in_use)
        {
            context->deferred++;
            continue;
        }
        hr = delete_generation_marker(
            context->store, context->backend, names[i].name,
            &context->budget, &context->weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            context->deferred++;
            hr = S_OK;
            continue;
        }
        if (FAILED(hr)) break;
    }
    HeapFree( GetProcessHeap(), 0, names );
    return hr;
}

static HRESULT collect_garbage_internal(
    const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend, struct deployment_store *store,
    const APPX_CATALOG_SNAPSHOT *catalog,
    APPX_DEPLOYMENT_RESULT *result, BOOL *weak )
{
    struct gc_context context;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    context.options = options;
    context.backend = backend;
    context.store = store;
    context.catalog = catalog;
    context.budget.max_entries = options->max_gc_entries;
    context.budget.max_bytes = options->max_gc_bytes;
    if (FAILED(hr = checkpoint( backend,
                                APPX_DEPLOYMENT_CHECKPOINT_GC_STARTED )))
        return hr;
    if (FAILED(hr = collect_generation_list(
            &context, store->payloads, TRUE )) ||
        FAILED(hr = collect_generation_list(
            &context, store->quarantine, FALSE )) ||
        FAILED(hr = collect_record_list( &context, TRUE )) ||
        FAILED(hr = collect_record_list( &context, FALSE )) ||
        FAILED(hr = collect_marker_list( &context )))
        return hr;
    if (FAILED(hr = checkpoint( backend,
                                APPX_DEPLOYMENT_CHECKPOINT_GC_FINISHED )))
        return hr;
    result->reclaimed_entries += context.budget.entries;
    result->reclaimed_bytes += context.budget.bytes;
    if (context.deferred)
        result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
    if (context.weak) *weak = TRUE;
    return S_OK;
}

static HRESULT read_journal_string(
    const BYTE *data, UINT32 size, UINT32 reference_offset, UINT32 *cursor,
    BOOL allow_empty, WCHAR **string )
{
    UINT32 offset = read_uint32( data + reference_offset );
    UINT32 chars = read_uint32( data + reference_offset + 4 );
    UINT32 bytes, end, i;

    *string = NULL;
    if (offset != *cursor || !chars ||
        !multiply_uint32( chars, sizeof(WCHAR), &bytes ) ||
        !add_uint32( offset, bytes, &end ) || end > size)
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (read_uint16( data + end - sizeof(WCHAR) ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (!allow_empty && chars == 1)
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    for (i = 0; i + 1 < chars; i++)
    {
        WCHAR ch = read_uint16( data + offset + i * sizeof(WCHAR) );

        if (!ch) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            WCHAR low;

            if (++i + 1 >= chars)
                return APPX_DEPLOYMENT_E_CORRUPT_STORE;
            low = read_uint16( data + offset + i * sizeof(WCHAR) );
            if (low < 0xdc00 || low > 0xdfff)
                return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff)
            return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }
    if (!(*string = HeapAlloc( GetProcessHeap(), 0, bytes )))
        return E_OUTOFMEMORY;
    for (i = 0; i < chars; i++)
        (*string)[i] = read_uint16( data + offset + i * sizeof(WCHAR) );
    *cursor = end;
    return S_OK;
}

static BOOL valid_journal_state( enum appx_deployment_operation operation,
                                 enum appx_deployment_journal_state state )
{
    if (operation == APPX_DEPLOYMENT_OPERATION_INSTALL ||
        operation == APPX_DEPLOYMENT_OPERATION_UPDATE)
        return state == APPX_DEPLOYMENT_JOURNAL_CREATED ||
               state == APPX_DEPLOYMENT_JOURNAL_INSPECTED ||
               state == APPX_DEPLOYMENT_JOURNAL_STAGED ||
               state == APPX_DEPLOYMENT_JOURNAL_PAYLOAD_COMPLETE ||
               state == APPX_DEPLOYMENT_JOURNAL_CATALOG_PREPARED ||
               state == APPX_DEPLOYMENT_JOURNAL_PUBLISHED ||
               state == APPX_DEPLOYMENT_JOURNAL_CLEANED;
    if (operation == APPX_DEPLOYMENT_OPERATION_REMOVE)
        return state == APPX_DEPLOYMENT_JOURNAL_CREATED ||
               state == APPX_DEPLOYMENT_JOURNAL_DEPENDENCY_CHECKED ||
               state == APPX_DEPLOYMENT_JOURNAL_CATALOG_PREPARED ||
               state == APPX_DEPLOYMENT_JOURNAL_PUBLISHED ||
               state == APPX_DEPLOYMENT_JOURNAL_GC_PENDING ||
               state == APPX_DEPLOYMENT_JOURNAL_CLEANED;
    return FALSE;
}

static HRESULT parse_journal( const BYTE *data, UINT32 size,
                              const WCHAR *file_name,
                              struct deployment_journal *journal )
{
    BYTE digest[DEPLOYMENT_DIGEST_SIZE];
    WCHAR expected_file_name[DEPLOYMENT_TRANSACTION_CHARS +
                             ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
    WCHAR *derived_path = NULL;
    UINT32 cursor = DEPLOYMENT_JOURNAL_HEADER_SIZE, i;
    BOOL has_package;
    HRESULT hr;

    memset( journal, 0, sizeof(*journal) );
    if (size < DEPLOYMENT_JOURNAL_HEADER_SIZE ||
        size > APPX_DEPLOYMENT_MAX_JOURNAL_SIZE ||
        memcmp( data, deployment_journal_magic,
                sizeof(deployment_journal_magic) ) ||
        read_uint32( data + JOURNAL_VERSION_OFFSET ) !=
            APPX_DEPLOYMENT_JOURNAL_VERSION ||
        read_uint32( data + JOURNAL_HEADER_SIZE_OFFSET ) !=
            DEPLOYMENT_JOURNAL_HEADER_SIZE ||
        read_uint32( data + JOURNAL_TOTAL_SIZE_OFFSET ) != size ||
        read_uint32( data + JOURNAL_RESERVED0_OFFSET ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    for (i = 0; i < JOURNAL_RESERVED_SIZE; i++)
        if (data[JOURNAL_RESERVED_OFFSET + i])
            return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = hash_with_zero_range(
            data, size, JOURNAL_DIGEST_OFFSET, DEPLOYMENT_DIGEST_SIZE,
            digest )))
        return hr;
    if (!equal_content_id( digest, data + JOURNAL_DIGEST_OFFSET ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;

    journal->operation = read_uint32( data + JOURNAL_OPERATION_OFFSET );
    journal->state = read_uint32( data + JOURNAL_STATE_OFFSET );
    journal->expected_epoch =
        read_uint64( data + JOURNAL_EXPECTED_EPOCH_OFFSET );
    if (!valid_journal_state( journal->operation, journal->state ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    memcpy( journal->transaction_id,
            data + JOURNAL_TRANSACTION_ID_OFFSET,
            sizeof(journal->transaction_id) );
    memcpy( journal->content_id, data + JOURNAL_CONTENT_ID_OFFSET,
            sizeof(journal->content_id) );
    bytes_to_hex( journal->transaction_id, sizeof(journal->transaction_id),
                  journal->transaction_name );
    lstrcpyW( expected_file_name, journal->transaction_name );
    lstrcatW( expected_file_name, DEPLOYMENT_JOURNAL_SUFFIX );
    if (lstrcmpW( file_name, expected_file_name ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    lstrcpyW( journal->journal_name, expected_file_name );

    if (FAILED(hr = read_journal_string(
            data, size, JOURNAL_FULL_NAME_REF_OFFSET, &cursor, TRUE,
            &journal->full_name )) ||
        FAILED(hr = read_journal_string(
            data, size, JOURNAL_FAMILY_NAME_REF_OFFSET, &cursor, TRUE,
            &journal->family_name )) ||
        FAILED(hr = read_journal_string(
            data, size, JOURNAL_PACKAGE_NAME_REF_OFFSET, &cursor, TRUE,
            &journal->package_name )) ||
        FAILED(hr = read_journal_string(
            data, size, JOURNAL_PUBLISHER_REF_OFFSET, &cursor, TRUE,
            &journal->publisher )) ||
        FAILED(hr = read_journal_string(
            data, size, JOURNAL_PAYLOAD_PATH_REF_OFFSET, &cursor, TRUE,
            &journal->payload_path )) ||
        FAILED(hr = read_journal_string(
            data, size, JOURNAL_STAGING_NAME_REF_OFFSET, &cursor, FALSE,
            &journal->staging_name )))
        goto failed;
    if (cursor != size ||
        lstrcmpW( journal->staging_name, journal->transaction_name ))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto failed;
    }
    has_package = journal->full_name[0] || journal->family_name[0] ||
                  journal->package_name[0] || journal->publisher[0] ||
                  journal->payload_path[0];
    if (journal->state == APPX_DEPLOYMENT_JOURNAL_CREATED)
    {
        if (has_package)
        {
            /*
             * A crash may occur after identity metadata is durably rewritten
             * but before the inspected-state rewrite.  Complete metadata is
             * accepted, partial metadata is not.
             */
            if (!journal->full_name[0] || !journal->family_name[0] ||
                !journal->package_name[0] || !journal->publisher[0] ||
                !journal->payload_path[0])
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto failed;
            }
        }
    }
    else if (!journal->full_name[0] || !journal->family_name[0] ||
             !journal->package_name[0] || !journal->publisher[0] ||
             !journal->payload_path[0])
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto failed;
    }
    if (has_package)
    {
        if (FAILED(hr = derive_payload_path(
                journal->full_name, journal->content_id, &derived_path )) ||
            lstrcmpW( derived_path, journal->payload_path ))
        {
            if (SUCCEEDED(hr)) hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
    }
    HeapFree( GetProcessHeap(), 0, derived_path );
    return S_OK;

failed:
    HeapFree( GetProcessHeap(), 0, derived_path );
    journal_destroy( journal );
    return hr;
}

static HRESULT read_journal_file( HANDLE transactions, const WCHAR *name,
                                  struct deployment_journal *journal )
{
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE *data = NULL;
    UINT64 file_size;
    DWORD links, read;
    HRESULT hr;

    if (FAILED(hr = open_child(
            transactions, name, FILE_READ_DATA,
            FILE_SHARE_READ, FILE_OPEN, FALSE, &file, NULL )))
        return hr;
    if (FAILED(hr = get_identity(
            file, FALSE, NULL, &links, &file_size )))
        goto done;
    if (links != 1 || file_size < DEPLOYMENT_JOURNAL_HEADER_SIZE ||
        file_size > APPX_DEPLOYMENT_MAX_JOURNAL_SIZE ||
        file_size > MAXDWORD)
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (!(data = HeapAlloc( GetProcessHeap(), 0, file_size )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!ReadFile( file, data, file_size, &read, NULL ))
    {
        hr = win32_error( GetLastError() );
        goto done;
    }
    if (read != file_size)
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    hr = parse_journal( data, file_size, name, journal );

done:
    HeapFree( GetProcessHeap(), 0, data );
    close_handle( &file );
    return hr;
}

struct journal_name
{
    WCHAR name[DEPLOYMENT_TRANSACTION_CHARS +
               ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
};

static BOOL valid_journal_file_name( const WCHAR *name, UINT32 chars )
{
    UINT32 suffix_chars = ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX) - 1;
    UINT32 i;

    if (chars != DEPLOYMENT_TRANSACTION_CHARS + suffix_chars ||
        memcmp( name + DEPLOYMENT_TRANSACTION_CHARS,
                DEPLOYMENT_JOURNAL_SUFFIX,
                suffix_chars * sizeof(WCHAR) ))
        return FALSE;
    for (i = 0; i < DEPLOYMENT_TRANSACTION_CHARS; i++)
        if (!((name[i] >= '0' && name[i] <= '9') ||
              (name[i] >= 'a' && name[i] <= 'f')))
            return FALSE;
    return TRUE;
}

static HRESULT enumerate_journal_names(
    HANDLE directory, UINT32 maximum, struct journal_name **names,
    UINT32 *count, BOOL *truncated )
{
    BYTE *buffer = NULL;
    struct journal_name *array = NULL;
    UINT32 capacity = 0;
    IO_STATUS_BLOCK io;
    BOOL restart = TRUE;
    NTSTATUS status;
    HRESULT hr = S_OK;

    *names = NULL;
    *count = 0;
    *truncated = FALSE;
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0,
                              DEPLOYMENT_ENUM_BUFFER_SIZE )))
        return E_OUTOFMEMORY;
    for (;;)
    {
        FILE_ID_BOTH_DIRECTORY_INFORMATION *entry;
        ULONG offset = 0;

        status = NtQueryDirectoryFile(
            directory, NULL, NULL, NULL, &io, buffer,
            DEPLOYMENT_ENUM_BUFFER_SIZE, FileIdBothDirectoryInformation,
            FALSE, NULL, restart );
        restart = FALSE;
        if (status == STATUS_NO_MORE_FILES) break;
        if (status == STATUS_PENDING)
        {
            hr = HRESULT_FROM_WIN32( ERROR_TIMEOUT );
            goto done;
        }
        if (status)
        {
            hr = ntstatus_error( status );
            goto done;
        }
        for (;;)
        {
            UINT32 chars;

            entry = (FILE_ID_BOTH_DIRECTORY_INFORMATION *)(buffer + offset);
            chars = entry->FileNameLength / sizeof(WCHAR);
            if ((chars == 1 && entry->FileName[0] == '.') ||
                (chars == 2 && entry->FileName[0] == '.' &&
                 entry->FileName[1] == '.'))
                goto next_entry;
            if (entry->FileNameLength % sizeof(WCHAR) ||
                (entry->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                          FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !valid_journal_file_name( entry->FileName, chars ))
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            if (*count >= maximum)
            {
                *truncated = TRUE;
                goto done;
            }
            if (*count == capacity)
            {
                struct journal_name *new_array;
                UINT32 new_capacity = capacity ? capacity * 2 : 32;

                if (new_capacity > maximum) new_capacity = maximum;
                if (!(new_array = HeapReAlloc(
                        GetProcessHeap(), 0, array,
                        new_capacity * sizeof(*new_array) )))
                {
                    if (array)
                    {
                        hr = E_OUTOFMEMORY;
                        goto done;
                    }
                    if (!(new_array = HeapAlloc(
                            GetProcessHeap(), 0,
                            new_capacity * sizeof(*new_array) )))
                    {
                        hr = E_OUTOFMEMORY;
                        goto done;
                    }
                }
                array = new_array;
                capacity = new_capacity;
            }
            memcpy( array[*count].name, entry->FileName,
                    entry->FileNameLength );
            array[*count].name[chars] = 0;
            (*count)++;

next_entry:
            if (!entry->NextEntryOffset) break;
            if (entry->NextEntryOffset >
                DEPLOYMENT_ENUM_BUFFER_SIZE - offset)
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            offset += entry->NextEntryOffset;
        }
    }

done:
    HeapFree( GetProcessHeap(), 0, buffer );
    if (FAILED(hr))
    {
        HeapFree( GetProcessHeap(), 0, array );
        array = NULL;
        *count = 0;
    }
    *names = array;
    return hr;
}

static BOOL catalog_matches_journal(
    const APPX_CATALOG_SNAPSHOT *catalog,
    const struct deployment_journal *journal )
{
    const struct appx_catalog_package *package = NULL;

    if (!journal->full_name || !journal->full_name[0]) return FALSE;
    if (FAILED(find_catalog_package(
            catalog, journal->full_name, &package, NULL )) || !package)
        return FALSE;
    return (package->flags & APPX_CATALOG_PACKAGE_ACTIVE) &&
           equal_content_id( package->content_id, journal->content_id ) &&
           !lstrcmpW( package->payload_path, journal->payload_path );
}

static HRESULT recover_one_journal(
    const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend, struct deployment_store *store,
    const APPX_CATALOG_SNAPSHOT *catalog, const WCHAR *name,
    APPX_DEPLOYMENT_RESULT *result, BOOL *weak )
{
    struct deployment_journal journal;
    struct delete_budget budget;
    const WCHAR *generation;
    BOOL committed;
    WCHAR pending_record_name[DEPLOYMENT_TRANSACTION_CHARS +
                              ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
    HRESULT hr;

    memset( &journal, 0, sizeof(journal) );
    if (FAILED(hr = read_journal_file(
            store->transactions, name, &journal )))
        return hr; /* Preserve malformed evidence. */
    committed = catalog_matches_journal( catalog, &journal );
    if (!committed && journal.payload_path && journal.payload_path[0] &&
        catalog_references_payload( catalog, journal.payload_path ))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    memset( &budget, 0, sizeof(budget) );
    budget.max_entries = options->max_gc_entries;
    budget.max_bytes = options->max_gc_bytes;
    lstrcpyW( pending_record_name, journal.transaction_name );
    lstrcatW( pending_record_name, DEPLOYMENT_JOURNAL_SUFFIX );

    if (journal.state < APPX_DEPLOYMENT_JOURNAL_PAYLOAD_COMPLETE)
    {
        hr = delete_named_tree( store->staging, journal.staging_name,
                                &budget, backend, weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
            hr = S_OK;
            goto done;
        }
        if (FAILED(hr)) goto done;
        hr = delete_named_file(
            store->record_staging, pending_record_name, &budget,
            backend, weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
            hr = S_OK;
            goto done;
        }
        if (FAILED(hr)) goto done;
    }
    else if (!committed)
    {
        if (!(generation = payload_generation_name( journal.payload_path )))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
        hr = delete_generation_marker( store, backend, generation,
                                       &budget, weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
            hr = S_OK;
            goto done;
        }
        if (FAILED(hr)) goto done;
        hr = move_generation_to_quarantine(
            store, backend, generation, weak );
        if (is_sharing_failure( hr ))
        {
            result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
            hr = S_OK;
            goto done;
        }
        if (FAILED(hr)) goto done;
        hr = move_record_to_quarantine(
            store, backend, generation, weak );
        if (is_sharing_failure( hr ))
        {
            result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
            hr = S_OK;
            goto done;
        }
        if (FAILED(hr)) goto done;
    }
    else
    {
        hr = delete_named_tree( store->staging, journal.staging_name,
                                &budget, backend, weak );
        if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
            is_sharing_failure( hr ))
        {
            result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
            hr = S_OK;
            goto done;
        }
        if (FAILED(hr)) goto done;
    }
    hr = delete_named_file(
        store->record_staging, pending_record_name, &budget, backend, weak );
    if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
        is_sharing_failure( hr ))
    {
        result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
        hr = S_OK;
        goto done;
    }
    if (FAILED(hr)) goto done;
    hr = delete_named_file(
        store->lease_pending, journal.transaction_name, &budget,
        backend, weak );
    if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
        is_sharing_failure( hr ))
    {
        result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
        hr = S_OK;
        goto done;
    }
    if (FAILED(hr)) goto done;
    if (SUCCEEDED(hr = remove_journal( store, backend, &journal, weak )))
    {
        result->reclaimed_entries += budget.entries;
        result->reclaimed_bytes += budget.bytes;
    }

done:
    journal_destroy( &journal );
    return hr;
}

static HRESULT recover_internal(
    const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend, struct deployment_store *store,
    const APPX_CATALOG_SNAPSHOT *catalog,
    APPX_DEPLOYMENT_RESULT *result, BOOL *weak )
{
    struct journal_name *names = NULL;
    UINT32 count = 0, i;
    BOOL truncated;
    HRESULT hr;

    if (FAILED(hr = enumerate_journal_names(
            store->transactions, options->max_gc_entries,
            &names, &count, &truncated )))
        return hr;
    if (truncated)
        result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
    for (i = 0; i < count; i++)
    {
        if (FAILED(hr = check_cancelled( options )) ||
            FAILED(hr = recover_one_journal(
                options, backend, store, catalog, names[i].name,
                result, weak )))
            break;
    }
    HeapFree( GetProcessHeap(), 0, names );
    return hr;
}

struct record_buffer
{
    BYTE *data;
    UINT32 size;
    UINT32 capacity;
};

static HRESULT record_buffer_reserve( struct record_buffer *buffer,
                                      UINT32 append )
{
    BYTE *data;
    UINT32 required, capacity;

    if (!add_uint32( buffer->size, append, &required ) ||
        required > APPX_DEPLOYMENT_MAX_RECORD_SIZE)
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    if (required <= buffer->capacity) return S_OK;
    capacity = buffer->capacity ? buffer->capacity : 4096;
    while (capacity < required)
    {
        if (capacity > APPX_DEPLOYMENT_MAX_RECORD_SIZE / 2)
        {
            capacity = APPX_DEPLOYMENT_MAX_RECORD_SIZE;
            break;
        }
        capacity *= 2;
    }
    if (!(data = HeapReAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                              buffer->data, capacity )))
    {
        if (buffer->data) return E_OUTOFMEMORY;
        if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                capacity )))
            return E_OUTOFMEMORY;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return S_OK;
}

static HRESULT record_buffer_append_zero( struct record_buffer *buffer,
                                          UINT32 size, UINT32 *offset )
{
    HRESULT hr;

    if (FAILED(hr = record_buffer_reserve( buffer, size ))) return hr;
    if (offset) *offset = buffer->size;
    memset( buffer->data + buffer->size, 0, size );
    buffer->size += size;
    return S_OK;
}

static HRESULT record_append_string( struct record_buffer *buffer,
                                     const WCHAR *string, BOOL allow_empty,
                                     UINT32 reference_offset )
{
    UINT32 chars, bytes, offset, i;
    HRESULT hr;

    if (FAILED(hr = bounded_string_length( string, allow_empty, &chars )))
        return hr;
    if (!multiply_uint32( chars, sizeof(WCHAR), &bytes ) ||
        FAILED(hr = record_buffer_append_zero(
            buffer, bytes, &offset )))
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    write_uint32( buffer->data + reference_offset, offset );
    write_uint32( buffer->data + reference_offset + 4, chars );
    for (i = 0; i < chars; i++)
        write_uint16( buffer->data + offset + i * sizeof(WCHAR),
                      string[i] );
    return S_OK;
}

struct loader_sort
{
    const WCHAR *path;
    struct appx_deployment_file_identity identity;
    struct appx_deployment_file_integrity integrity;
};

static INT compare_loader_sort( const void *left, const void *right )
{
    const struct loader_sort *left_loader = left;
    const struct loader_sort *right_loader = right;
    INT result = compare_string_ci( left_loader->path, right_loader->path );

    if (result) return result;
    result = CompareStringOrdinal( left_loader->path, -1,
                                   right_loader->path, -1, FALSE );
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static const struct loader_sort *find_loader_sort(
    const struct loader_sort *loaders, UINT32 count, const WCHAR *path )
{
    UINT32 low = 0, high = count;

    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;
        INT comparison = compare_string_ci( loaders[middle].path, path );

        if (comparison < 0) low = middle + 1;
        else high = middle;
    }
    if (low >= count || compare_string_ci( loaders[low].path, path ))
        return NULL;
    return loaders + low;
}

struct class_sort
{
    const struct appx_deployment_inproc_class *class;
    struct appx_deployment_file_identity identity;
    struct appx_deployment_file_integrity integrity;
};

static INT compare_class_sort( const void *left, const void *right )
{
    const struct class_sort *left_class = left;
    const struct class_sort *right_class = right;
    INT result = compare_string_ci(
        left_class->class->activatable_class_id,
        right_class->class->activatable_class_id );

    if (result) return result;
    return compare_string_ci( left_class->class->path,
                              right_class->class->path );
}

struct application_sort
{
    const struct appx_catalog_application *application;
    const struct prepared_loader_search *loader_search;
    struct appx_deployment_file_identity identity;
    struct appx_deployment_file_integrity integrity;
};

static INT compare_application_sort( const void *left, const void *right )
{
    const struct application_sort *left_application = left;
    const struct application_sort *right_application = right;
    INT result = compare_string_exact( left_application->application->id,
                                       right_application->application->id );

    if (result) return result;
    return compare_string_exact( left_application->application->executable,
                                 right_application->application->executable );
}

static HRESULT serialize_deployment_record(
    const struct prepared_package *prepared, HANDLE staging, BYTE **output,
    UINT32 *output_size )
{
    const struct appx_catalog_package *package =
        &prepared->test_view.package;
    struct loader_sort *loaders = NULL;
    struct class_sort *classes = NULL;
    struct application_sort *applications = NULL;
    struct record_buffer buffer = {0};
    BYTE digest[DEPLOYMENT_DIGEST_SIZE];
    UINT32 loaders_offset, classes_offset, applications_offset;
    UINT32 search_paths_offset, strings_offset;
    UINT32 loader_bytes, class_bytes, application_bytes, search_path_bytes;
    UINT32 search_path_count, search_index, i, j;
    HRESULT hr;

    *output = NULL;
    *output_size = 0;
    if (prepared->test_view.loader_file_count >
            APPX_DEPLOYMENT_MAX_LOADER_FILES ||
        prepared->test_view.inproc_class_count >
            APPX_DEPLOYMENT_MAX_INPROC_CLASSES ||
        !multiply_uint32( prepared->test_view.loader_file_count,
                          DEPLOYMENT_RECORD_LOADER_SIZE, &loader_bytes ) ||
        !multiply_uint32( prepared->test_view.inproc_class_count,
                          DEPLOYMENT_RECORD_CLASS_SIZE, &class_bytes ) ||
        !multiply_uint32( package->application_count,
                          DEPLOYMENT_RECORD_APPLICATION_SIZE,
                          &application_bytes ))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    search_path_count = prepared->package_loader_search_path_count;
    for (i = 0; i < package->application_count; i++)
        if (prepared->application_loader_search &&
            !add_uint32( search_path_count,
                         prepared->application_loader_search[i].count,
                         &search_path_count ))
            return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!multiply_uint32( search_path_count,
                          DEPLOYMENT_RECORD_SEARCH_PATH_SIZE,
                          &search_path_bytes ))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (prepared->test_view.loader_file_count &&
        !(loaders = HeapAlloc( GetProcessHeap(), 0,
                               prepared->test_view.loader_file_count *
                               sizeof(*loaders) )))
        return E_OUTOFMEMORY;
    if (prepared->test_view.inproc_class_count &&
        !(classes = HeapAlloc( GetProcessHeap(), 0,
                               prepared->test_view.inproc_class_count *
                               sizeof(*classes) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (package->application_count &&
        !(applications = HeapAlloc( GetProcessHeap(), 0,
                                    package->application_count *
                                    sizeof(*applications) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < prepared->test_view.loader_file_count; i++)
    {
        loaders[i].path =
            prepared->test_view.loader_files[i].relative_path;
        if (FAILED(hr = capture_payload_file_state(
                staging, loaders[i].path, &loaders[i].identity,
                &loaders[i].integrity )))
            goto done;
    }
    for (i = 0; i < prepared->test_view.inproc_class_count; i++)
        classes[i].class = prepared->test_view.inproc_classes + i;
    for (i = 0; i < package->application_count; i++)
    {
        applications[i].application = package->applications + i;
        applications[i].loader_search =
            prepared->application_loader_search ?
            prepared->application_loader_search + i : NULL;
        if (FAILED(hr = capture_payload_file_state(
                staging, applications[i].application->executable,
                &applications[i].identity,
                &applications[i].integrity )) ||
            FAILED(hr = validate_optional_string_max(
                applications[i].application->parameters, 1024 )) ||
            FAILED(hr = validate_optional_relative_path(
                applications[i].application->current_directory_path, 256 )) ||
            FAILED(hr = verify_payload_directory(
                staging,
                applications[i].application->current_directory_path )))
            goto done;
    }
    if (prepared->test_view.loader_file_count > 1)
        qsort( loaders, prepared->test_view.loader_file_count,
               sizeof(*loaders), compare_loader_sort );
    if (prepared->test_view.inproc_class_count > 1)
        qsort( classes, prepared->test_view.inproc_class_count,
               sizeof(*classes), compare_class_sort );
    if (package->application_count > 1)
        qsort( applications, package->application_count,
               sizeof(*applications), compare_application_sort );
    for (i = 1; i < prepared->test_view.loader_file_count; i++)
        if (!compare_string_ci( loaders[i - 1].path, loaders[i].path ))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
    for (i = 1; i < prepared->test_view.inproc_class_count; i++)
        if (!compare_string_ci(
                classes[i - 1].class->activatable_class_id,
                classes[i].class->activatable_class_id ))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
    for (i = 0; i < prepared->test_view.inproc_class_count; i++)
    {
        const struct loader_sort *loader = find_loader_sort(
            loaders, prepared->test_view.loader_file_count,
            classes[i].class->path );

        if (!loader)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
        classes[i].identity = loader->identity;
        classes[i].integrity = loader->integrity;
    }
    for (i = 1; i < package->application_count; i++)
        if (!compare_string_exact( applications[i - 1].application->id,
                                   applications[i].application->id ))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }

    if (FAILED(hr = record_buffer_append_zero(
            &buffer, DEPLOYMENT_RECORD_HEADER_SIZE, NULL )) ||
        FAILED(hr = record_buffer_append_zero(
            &buffer, loader_bytes, &loaders_offset )) ||
        FAILED(hr = record_buffer_append_zero(
            &buffer, class_bytes, &classes_offset )) ||
        FAILED(hr = record_buffer_append_zero(
            &buffer, application_bytes, &applications_offset )) ||
        FAILED(hr = record_buffer_append_zero(
            &buffer, search_path_bytes, &search_paths_offset )))
        goto done;
    strings_offset = buffer.size;
    if (FAILED(hr = record_append_string(
            &buffer, package->full_name, FALSE,
            RECORD_FULL_NAME_REF_OFFSET )) ||
        FAILED(hr = record_append_string(
            &buffer, package->payload_path, FALSE,
            RECORD_PAYLOAD_PATH_REF_OFFSET )))
        goto done;
    for (i = 0; i < prepared->test_view.loader_file_count; i++)
    {
        UINT32 offset = loaders_offset + i * DEPLOYMENT_RECORD_LOADER_SIZE;

        if (FAILED(hr = record_append_string(
                &buffer, loaders[i].path, FALSE,
                offset + RECORD_LOADER_PATH_REF_OFFSET )))
            goto done;
        write_file_identity(
            buffer.data + offset, RECORD_LOADER_VOLUME_SERIAL_OFFSET,
            RECORD_LOADER_FILE_INDEX_HIGH_OFFSET,
            RECORD_LOADER_FILE_INDEX_LOW_OFFSET, &loaders[i].identity );
        write_file_integrity(
            buffer.data + offset, RECORD_LOADER_SIZE_OFFSET,
            RECORD_LOADER_DIGEST_OFFSET, &loaders[i].integrity );
    }
    for (i = 0; i < prepared->test_view.inproc_class_count; i++)
    {
        UINT32 offset = classes_offset +
                        i * DEPLOYMENT_RECORD_CLASS_SIZE;

        if (FAILED(hr = record_append_string(
                &buffer, classes[i].class->path, FALSE,
                offset + RECORD_CLASS_PATH_REF_OFFSET )) ||
            FAILED(hr = record_append_string(
                &buffer, classes[i].class->activatable_class_id, FALSE,
                offset + RECORD_CLASS_ID_REF_OFFSET )))
            goto done;
        write_uint32( buffer.data + offset + RECORD_CLASS_THREADING_OFFSET,
                      classes[i].class->threading_model );
        write_file_identity(
            buffer.data + offset, RECORD_CLASS_VOLUME_SERIAL_OFFSET,
            RECORD_CLASS_FILE_INDEX_HIGH_OFFSET,
            RECORD_CLASS_FILE_INDEX_LOW_OFFSET, &classes[i].identity );
        write_file_integrity(
            buffer.data + offset, RECORD_CLASS_SIZE_OFFSET,
            RECORD_CLASS_DIGEST_OFFSET, &classes[i].integrity );
    }
    for (i = 0; i < package->application_count; i++)
    {
        UINT32 offset = applications_offset +
                        i * DEPLOYMENT_RECORD_APPLICATION_SIZE;

        if (FAILED(hr = record_append_string(
                &buffer, applications[i].application->id, FALSE,
                offset + RECORD_APPLICATION_ID_REF_OFFSET )) ||
            FAILED(hr = record_append_string(
                &buffer, applications[i].application->executable, FALSE,
                offset + RECORD_APPLICATION_EXEC_REF_OFFSET )) ||
            FAILED(hr = record_append_string(
                &buffer, optional_string(
                    applications[i].application->entry_point ), TRUE,
                offset + RECORD_APPLICATION_ENTRY_REF_OFFSET )) ||
            FAILED(hr = record_append_string(
                &buffer, optional_string(
                    applications[i].application->parameters ), TRUE,
                offset + RECORD_APPLICATION_PARAMETERS_REF_OFFSET )) ||
            FAILED(hr = record_append_string(
                &buffer, optional_string(
                    applications[i].application->current_directory_path ),
                TRUE, offset + RECORD_APPLICATION_CURRENT_DIR_REF_OFFSET )))
            goto done;
        write_uint32( buffer.data + offset + RECORD_APPLICATION_KIND_OFFSET,
                      applications[i].application->activation_kind );
        write_file_identity(
            buffer.data + offset, RECORD_APPLICATION_VOLUME_SERIAL_OFFSET,
            RECORD_APPLICATION_FILE_INDEX_HIGH_OFFSET,
            RECORD_APPLICATION_FILE_INDEX_LOW_OFFSET,
            &applications[i].identity );
        write_file_integrity(
            buffer.data + offset, RECORD_APPLICATION_SIZE_OFFSET,
            RECORD_APPLICATION_DIGEST_OFFSET,
            &applications[i].integrity );
    }
    search_index = 0;
    for (i = 0; i < prepared->package_loader_search_path_count; i++)
    {
        if (FAILED(hr = record_append_string(
                &buffer, prepared->package_loader_search_paths[i], TRUE,
                search_paths_offset +
                search_index * DEPLOYMENT_RECORD_SEARCH_PATH_SIZE )))
            goto done;
        search_index++;
    }
    for (i = 0; i < package->application_count; i++)
    {
        const struct prepared_loader_search *search =
            applications[i].loader_search;
        UINT32 offset = applications_offset +
                        i * DEPLOYMENT_RECORD_APPLICATION_SIZE;
        UINT32 count = search ? search->count : 0;

        write_uint32( buffer.data + offset +
                      RECORD_APPLICATION_SEARCH_START_OFFSET,
                      search_index );
        write_uint32( buffer.data + offset +
                      RECORD_APPLICATION_SEARCH_COUNT_OFFSET, count );
        write_uint32( buffer.data + offset +
                      RECORD_APPLICATION_SEARCH_FLAGS_OFFSET,
                      search && search->present ?
                      RECORD_SEARCH_OVERRIDE_PRESENT : 0 );
        for (j = 0; j < count; j++)
        {
            if (FAILED(hr = record_append_string(
                    &buffer, search->paths[j], TRUE,
                    search_paths_offset +
                    search_index * DEPLOYMENT_RECORD_SEARCH_PATH_SIZE )))
                goto done;
            search_index++;
        }
    }
    if (search_index != search_path_count)
    {
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
        goto done;
    }
    memcpy( buffer.data, deployment_record_magic,
            sizeof(deployment_record_magic) );
    write_uint32( buffer.data + RECORD_VERSION_OFFSET,
                  APPX_DEPLOYMENT_RECORD_VERSION );
    write_uint32( buffer.data + RECORD_HEADER_SIZE_OFFSET,
                  DEPLOYMENT_RECORD_HEADER_SIZE );
    write_uint32( buffer.data + RECORD_TOTAL_SIZE_OFFSET, buffer.size );
    write_uint32( buffer.data + RECORD_LOADER_COUNT_OFFSET,
                  prepared->test_view.loader_file_count );
    write_uint32( buffer.data + RECORD_CLASS_COUNT_OFFSET,
                  prepared->test_view.inproc_class_count );
    write_uint32( buffer.data + RECORD_APPLICATION_COUNT_OFFSET,
                  package->application_count );
    write_uint32( buffer.data + RECORD_LOADERS_OFFSET, loaders_offset );
    write_uint32( buffer.data + RECORD_CLASSES_OFFSET, classes_offset );
    write_uint32( buffer.data + RECORD_APPLICATIONS_OFFSET,
                  applications_offset );
    write_uint32( buffer.data + RECORD_STRINGS_OFFSET, strings_offset );
    write_uint32( buffer.data + RECORD_STRINGS_SIZE_OFFSET,
                  buffer.size - strings_offset );
    write_uint32( buffer.data + RECORD_PACKAGE_SEARCH_COUNT_OFFSET,
                  prepared->package_loader_search_path_count );
    write_uint32( buffer.data + RECORD_PACKAGE_SEARCH_FLAGS_OFFSET,
                  prepared->has_package_loader_search_path_override ?
                  RECORD_SEARCH_OVERRIDE_PRESENT : 0 );
    write_uint32( buffer.data + RECORD_SEARCH_PATH_COUNT_OFFSET,
                  search_path_count );
    write_uint32( buffer.data + RECORD_SEARCH_PATHS_OFFSET,
                  search_paths_offset );
    memcpy( buffer.data + RECORD_CONTENT_ID_OFFSET, package->content_id,
            sizeof(package->content_id) );
    if (FAILED(hr = hash_with_zero_range(
            buffer.data, buffer.size, RECORD_DIGEST_OFFSET,
            DEPLOYMENT_DIGEST_SIZE, digest )))
        goto done;
    memcpy( buffer.data + RECORD_DIGEST_OFFSET, digest, sizeof(digest) );
    *output = buffer.data;
    *output_size = buffer.size;
    buffer.data = NULL;
    hr = S_OK;

done:
    HeapFree( GetProcessHeap(), 0, buffer.data );
    HeapFree( GetProcessHeap(), 0, applications );
    HeapFree( GetProcessHeap(), 0, classes );
    HeapFree( GetProcessHeap(), 0, loaders );
    return hr;
}

static HRESULT make_record_file_name( const WCHAR *payload_path,
                                      WCHAR *name, UINT32 capacity )
{
    const WCHAR *generation = payload_generation_name( payload_path );
    UINT32 required = DEPLOYMENT_GENERATION_CHARS +
                      ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX);

    if (!generation) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (capacity < required) return E_INVALIDARG;
    lstrcpyW( name, generation );
    lstrcatW( name, DEPLOYMENT_JOURNAL_SUFFIX );
    return S_OK;
}

static HRESULT create_pending_record(
    struct deployment_store *store, struct deployment_backend *backend,
    const struct prepared_package *prepared, HANDLE staging,
    const struct deployment_journal *journal, HANDLE *record,
    struct object_identity *identity, BOOL *weak )
{
    BYTE *data = NULL;
    UINT32 size;
    WCHAR name[DEPLOYMENT_TRANSACTION_CHARS +
               ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
    HRESULT hr;

    *record = INVALID_HANDLE_VALUE;
    lstrcpyW( name, journal->transaction_name );
    lstrcatW( name, DEPLOYMENT_JOURNAL_SUFFIX );
    if (FAILED(hr = serialize_deployment_record(
            prepared, staging, &data, &size )) ||
        FAILED(hr = open_child(
            store->record_staging, name, FILE_WRITE_DATA | DELETE,
            0, FILE_CREATE, FALSE, record, identity )) ||
        FAILED(hr = require_single_link_file( *record )) ||
        FAILED(hr = backend_write_all( backend, *record, data, size )) ||
        FAILED(hr = mark_durability(
            backend, backend_flush( backend, *record, FALSE ), weak )) ||
        FAILED(hr = mark_durability(
            backend,
            backend_flush( backend, store->record_staging, TRUE ), weak )))
        goto done;

done:
    HeapFree( GetProcessHeap(), 0, data );
    if (FAILED(hr)) close_handle( record );
    return hr;
}

static HRESULT read_record_string(
    BYTE *data, UINT32 size, UINT32 reference_offset, UINT32 *cursor,
    BOOL path, WCHAR **string )
{
    UINT32 offset = read_uint32( data + reference_offset );
    UINT32 chars = read_uint32( data + reference_offset + 4 );
    UINT32 bytes, end, i;
    HRESULT hr;

    *string = NULL;
    if ((offset & (sizeof(WCHAR) - 1)) || offset != *cursor || !chars ||
        !multiply_uint32( chars, sizeof(WCHAR), &bytes ) ||
        !add_uint32( offset, bytes, &end ) || end > size ||
        read_uint16( data + end - sizeof(WCHAR) ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    for (i = 0; i + 1 < chars; i++)
    {
        WCHAR ch = read_uint16( data + offset + i * sizeof(WCHAR) );

        if (!ch) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            WCHAR low;

            if (++i + 1 >= chars)
                return APPX_DEPLOYMENT_E_CORRUPT_STORE;
            low = read_uint16( data + offset + i * sizeof(WCHAR) );
            if (low < 0xdc00 || low > 0xdfff)
                return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        }
        else if (ch >= 0xdc00 && ch <= 0xdfff)
            return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }
    *string = (WCHAR *)(data + offset);
    if (path && FAILED(hr = validate_relative_path( *string, FALSE )))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    *cursor = end;
    return S_OK;
}

static HRESULT parse_deployment_record(
    const BYTE *source, UINT32 size,
    const struct appx_catalog_package *package,
    APPX_DEPLOYMENT_RECORD **output )
{
    APPX_DEPLOYMENT_RECORD *record = NULL;
    BYTE digest[DEPLOYMENT_DIGEST_SIZE], *data;
    WCHAR *full_name, *payload_path;
    UINT32 loader_count, class_count, application_count, search_path_count;
    UINT32 package_search_count, package_search_flags;
    UINT32 loaders_offset, classes_offset, applications_offset;
    UINT32 search_paths_offset;
    UINT32 strings_offset, strings_size, loader_bytes, class_bytes;
    UINT32 application_bytes, search_path_bytes, pointer_bytes;
    UINT32 allocation, view_bytes, cursor, search_index, i, j;
    HRESULT hr;

    *output = NULL;
    if (size < DEPLOYMENT_RECORD_HEADER_SIZE ||
        size > APPX_DEPLOYMENT_MAX_RECORD_SIZE ||
        memcmp( source, deployment_record_magic,
                sizeof(deployment_record_magic) ) ||
        read_uint32( source + RECORD_VERSION_OFFSET ) !=
            APPX_DEPLOYMENT_RECORD_VERSION ||
        read_uint32( source + RECORD_HEADER_SIZE_OFFSET ) !=
            DEPLOYMENT_RECORD_HEADER_SIZE ||
        read_uint32( source + RECORD_TOTAL_SIZE_OFFSET ) != size ||
        read_uint32( source + RECORD_RESERVED0_OFFSET ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    for (i = 0; i < RECORD_RESERVED_SIZE; i++)
        if (source[RECORD_RESERVED_OFFSET + i])
            return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = hash_with_zero_range(
            source, size, RECORD_DIGEST_OFFSET,
            DEPLOYMENT_DIGEST_SIZE, digest )))
        return hr;
    if (!equal_content_id( digest, source + RECORD_DIGEST_OFFSET ) ||
        !equal_content_id( package->content_id,
                           source + RECORD_CONTENT_ID_OFFSET ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;

    loader_count = read_uint32( source + RECORD_LOADER_COUNT_OFFSET );
    class_count = read_uint32( source + RECORD_CLASS_COUNT_OFFSET );
    application_count = read_uint32( source + RECORD_APPLICATION_COUNT_OFFSET );
    loaders_offset = read_uint32( source + RECORD_LOADERS_OFFSET );
    classes_offset = read_uint32( source + RECORD_CLASSES_OFFSET );
    applications_offset = read_uint32( source + RECORD_APPLICATIONS_OFFSET );
    package_search_count = read_uint32(
        source + RECORD_PACKAGE_SEARCH_COUNT_OFFSET );
    package_search_flags = read_uint32(
        source + RECORD_PACKAGE_SEARCH_FLAGS_OFFSET );
    search_path_count = read_uint32(
        source + RECORD_SEARCH_PATH_COUNT_OFFSET );
    search_paths_offset = read_uint32(
        source + RECORD_SEARCH_PATHS_OFFSET );
    strings_offset = read_uint32( source + RECORD_STRINGS_OFFSET );
    strings_size = read_uint32( source + RECORD_STRINGS_SIZE_OFFSET );
    if (loader_count > APPX_DEPLOYMENT_MAX_LOADER_FILES ||
        class_count > APPX_DEPLOYMENT_MAX_INPROC_CLASSES ||
        application_count != package->application_count ||
        package_search_count > APPX_DEPLOYMENT_MAX_LOADER_SEARCH_PATHS ||
        (package_search_flags & ~RECORD_SEARCH_KNOWN_FLAGS) ||
        (!(package_search_flags & RECORD_SEARCH_OVERRIDE_PRESENT) &&
         package_search_count) ||
        search_path_count >
            APPX_DEPLOYMENT_MAX_LOADER_SEARCH_PATHS *
            (application_count + 1) ||
        !multiply_uint32( loader_count, DEPLOYMENT_RECORD_LOADER_SIZE,
                          &loader_bytes ) ||
        !multiply_uint32( class_count, DEPLOYMENT_RECORD_CLASS_SIZE,
                          &class_bytes ) ||
        !multiply_uint32( application_count,
                          DEPLOYMENT_RECORD_APPLICATION_SIZE,
                          &application_bytes ) ||
        !multiply_uint32( search_path_count,
                          DEPLOYMENT_RECORD_SEARCH_PATH_SIZE,
                          &search_path_bytes ) ||
        loaders_offset != DEPLOYMENT_RECORD_HEADER_SIZE ||
        classes_offset != loaders_offset + loader_bytes ||
        applications_offset != classes_offset + class_bytes ||
        search_paths_offset != applications_offset + application_bytes ||
        strings_offset != search_paths_offset + search_path_bytes ||
        strings_size != size - strings_offset)
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (!multiply_uint32( loader_count,
                          sizeof(struct appx_deployment_loader_file),
                          &view_bytes ) ||
        class_count > (MAXDWORD - view_bytes) /
            sizeof(struct appx_deployment_inproc_class) ||
        !add_uint32( view_bytes,
                     class_count *
                     sizeof(struct appx_deployment_inproc_class),
                     &view_bytes ) ||
        application_count > (MAXDWORD - view_bytes) /
            sizeof(struct appx_deployment_application_file) ||
        !add_uint32( view_bytes,
                     application_count *
                     sizeof(struct appx_deployment_application_file),
                     &view_bytes ) ||
        !multiply_uint32( search_path_count, sizeof(WCHAR *),
                          &pointer_bytes ) ||
        !add_uint32( view_bytes, pointer_bytes, &view_bytes ) ||
        !add_uint32( offsetof(APPX_DEPLOYMENT_RECORD, data),
                     view_bytes, &allocation ) ||
        !add_uint32( allocation, size, &allocation ))
        return E_OUTOFMEMORY;
    if (!(record = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                              allocation )))
        return E_OUTOFMEMORY;
    record->size = size;
    record->loader_count = loader_count;
    record->class_count = class_count;
    record->application_count = application_count;
    record->loader_search_path_count = search_path_count;
    record->package_loader_search_path_count = package_search_count;
    record->has_package_loader_search_path_override =
        !!(package_search_flags & RECORD_SEARCH_OVERRIDE_PRESENT);
    record->loaders = (struct appx_deployment_loader_file *)record->data;
    record->classes = (struct appx_deployment_inproc_class *)
        (record->data + loader_count * sizeof(*record->loaders));
    record->applications = (struct appx_deployment_application_file *)
        ((BYTE *)record->classes + class_count * sizeof(*record->classes));
    record->loader_search_paths = (const WCHAR **)
        (record->applications + application_count);
    data = record->data + view_bytes;
    memcpy( data, source, size );

    cursor = strings_offset;
    if (FAILED(hr = read_record_string(
            data, size, RECORD_FULL_NAME_REF_OFFSET, &cursor,
            FALSE, &full_name )) ||
        FAILED(hr = read_record_string(
            data, size, RECORD_PAYLOAD_PATH_REF_OFFSET, &cursor,
            TRUE, &payload_path )))
        goto failed;
    if (lstrcmpW( full_name, package->full_name ) ||
        lstrcmpW( payload_path, package->payload_path ))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto failed;
    }
    for (i = 0; i < loader_count; i++)
    {
        WCHAR *path;
        UINT32 offset = loaders_offset + i * DEPLOYMENT_RECORD_LOADER_SIZE;

        if (FAILED(hr = read_record_string(
                data, size, offset + RECORD_LOADER_PATH_REF_OFFSET,
                &cursor, TRUE, &path )) ||
            FAILED(hr = read_file_identity(
                data + offset, RECORD_LOADER_VOLUME_SERIAL_OFFSET,
                RECORD_LOADER_FILE_INDEX_HIGH_OFFSET,
                RECORD_LOADER_FILE_INDEX_LOW_OFFSET,
                &record->loaders[i].identity )))
        {
            if (SUCCEEDED(hr)) hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        read_file_integrity(
            data + offset, RECORD_LOADER_SIZE_OFFSET,
            RECORD_LOADER_DIGEST_OFFSET, &record->loaders[i].integrity );
        if (i && compare_string_ci(
                record->loaders[i - 1].relative_path, path ) >= 0)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        record->loaders[i].relative_path = path;
    }
    for (i = 0; i < class_count; i++)
    {
        UINT32 offset = classes_offset + i * DEPLOYMENT_RECORD_CLASS_SIZE;
        WCHAR *path, *class_id;
        UINT32 threading = read_uint32(
            data + offset + RECORD_CLASS_THREADING_OFFSET );

        if (threading > APPX_MANIFEST_THREADING_MTA ||
            FAILED(hr = read_record_string(
                data, size, offset + RECORD_CLASS_PATH_REF_OFFSET,
                &cursor, TRUE, &path )) ||
            FAILED(hr = read_record_string(
                data, size, offset + RECORD_CLASS_ID_REF_OFFSET,
                &cursor, FALSE, &class_id )) ||
            FAILED(hr = read_file_identity(
                data + offset, RECORD_CLASS_VOLUME_SERIAL_OFFSET,
                RECORD_CLASS_FILE_INDEX_HIGH_OFFSET,
                RECORD_CLASS_FILE_INDEX_LOW_OFFSET,
                &record->classes[i].identity )))
        {
            if (SUCCEEDED(hr)) hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        read_file_integrity(
            data + offset, RECORD_CLASS_SIZE_OFFSET,
            RECORD_CLASS_DIGEST_OFFSET, &record->classes[i].integrity );
        if (i && compare_string_ci(
                record->classes[i - 1].activatable_class_id,
                class_id ) >= 0)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        record->classes[i].path = path;
        record->classes[i].activatable_class_id = class_id;
        record->classes[i].threading_model = threading;
    }
    for (i = 0; i < application_count; i++)
    {
        UINT32 offset = applications_offset +
                        i * DEPLOYMENT_RECORD_APPLICATION_SIZE;
        UINT32 kind = read_uint32(
            data + offset + RECORD_APPLICATION_KIND_OFFSET );
        UINT32 search_start = read_uint32(
            data + offset + RECORD_APPLICATION_SEARCH_START_OFFSET );
        UINT32 search_count = read_uint32(
            data + offset + RECORD_APPLICATION_SEARCH_COUNT_OFFSET );
        UINT32 search_flags = read_uint32(
            data + offset + RECORD_APPLICATION_SEARCH_FLAGS_OFFSET );
        WCHAR *id, *executable, *entry_point, *parameters;
        WCHAR *current_directory_path;

        if (search_start > search_path_count ||
            search_count > APPX_DEPLOYMENT_MAX_LOADER_SEARCH_PATHS ||
            search_count > search_path_count - search_start ||
            (search_flags & ~RECORD_SEARCH_KNOWN_FLAGS) ||
            (!(search_flags & RECORD_SEARCH_OVERRIDE_PRESENT) && search_count) ||
            FAILED(hr = read_record_string(
                data, size, offset + RECORD_APPLICATION_ID_REF_OFFSET,
                &cursor, FALSE, &id )) ||
            FAILED(hr = read_record_string(
                data, size, offset + RECORD_APPLICATION_EXEC_REF_OFFSET,
                &cursor, TRUE, &executable )) ||
            FAILED(hr = read_record_string(
                data, size, offset + RECORD_APPLICATION_ENTRY_REF_OFFSET,
                &cursor, FALSE, &entry_point )) ||
            FAILED(hr = read_record_string(
                data, size, offset + RECORD_APPLICATION_PARAMETERS_REF_OFFSET,
                &cursor, FALSE, &parameters )) ||
            FAILED(hr = read_record_string(
                data, size, offset + RECORD_APPLICATION_CURRENT_DIR_REF_OFFSET,
                &cursor, FALSE, &current_directory_path )) ||
            FAILED(hr = validate_optional_string_max( entry_point, 256 )) ||
            FAILED(hr = validate_optional_string_max( parameters, 1024 )) ||
            FAILED(hr = validate_optional_relative_path(
                current_directory_path, 256 )) ||
            FAILED(hr = read_file_identity(
                data + offset, RECORD_APPLICATION_VOLUME_SERIAL_OFFSET,
                RECORD_APPLICATION_FILE_INDEX_HIGH_OFFSET,
                RECORD_APPLICATION_FILE_INDEX_LOW_OFFSET,
                &record->applications[i].identity )))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        read_file_integrity(
            data + offset, RECORD_APPLICATION_SIZE_OFFSET,
            RECORD_APPLICATION_DIGEST_OFFSET,
            &record->applications[i].integrity );
        if (i && compare_string_exact(
                record->applications[i - 1].id, id ) >= 0)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        if (kind != APPX_CATALOG_ACTIVATION_FULL_TRUST &&
            kind != APPX_CATALOG_ACTIVATION_PACKAGED_CLASSIC &&
            kind != APPX_CATALOG_ACTIVATION_WIN32)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        record->applications[i].id = id;
        record->applications[i].executable = executable;
        record->applications[i].entry_point = entry_point;
        record->applications[i].parameters = parameters;
        record->applications[i].current_directory_path =
            current_directory_path;
        record->applications[i].activation_kind = kind;
        record->applications[i].loader_search_paths =
            record->loader_search_paths + search_start;
        record->applications[i].loader_search_path_count = search_count;
        record->applications[i].has_loader_search_path_override =
            !!(search_flags & RECORD_SEARCH_OVERRIDE_PRESENT);
    }
    search_index = 0;
    for (i = 0; i < search_path_count; i++)
    {
        WCHAR *path;

        if (FAILED(hr = read_record_string(
                data, size,
                search_paths_offset +
                i * DEPLOYMENT_RECORD_SEARCH_PATH_SIZE,
                &cursor, FALSE, &path )) ||
            FAILED(validate_optional_relative_path( path, 256 )))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        record->loader_search_paths[i] = path;
    }
    for (i = 0; i < package_search_count; i++)
        for (j = 0; j < i; j++)
            if (!compare_string_ci( record->loader_search_paths[j],
                                    record->loader_search_paths[i] ))
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto failed;
            }
    search_index = package_search_count;
    for (i = 0; i < application_count; i++)
    {
        const struct appx_deployment_application_file *application =
            record->applications + i;
        UINT32 start = application->loader_search_paths -
                       record->loader_search_paths;

        if (start != search_index)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        for (j = 0; j < application->loader_search_path_count; j++)
        {
            UINT32 k;

            for (k = 0; k < j; k++)
                if (!compare_string_ci(
                        application->loader_search_paths[k],
                        application->loader_search_paths[j] ))
                {
                    hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                    goto failed;
                }
        }
        search_index += application->loader_search_path_count;
    }
    if (search_index != search_path_count)
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto failed;
    }
    if (cursor != size)
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto failed;
    }
    *output = record;
    return S_OK;

failed:
    HeapFree( GetProcessHeap(), 0, record );
    return hr;
}

static HRESULT load_record_from_store(
    struct deployment_store *store,
    const struct appx_catalog_package *package,
    APPX_DEPLOYMENT_RECORD **record )
{
    WCHAR name[DEPLOYMENT_GENERATION_CHARS +
               ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
    struct object_identity identity;
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE *data = NULL;
    DWORD links, read;
    UINT64 size;
    HRESULT hr;

    *record = NULL;
    if (FAILED(hr = make_record_file_name(
            package->payload_path, name, ARRAY_SIZE(name) )))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = open_child(
            store->records, name, FILE_READ_DATA,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            FILE_OPEN, FALSE, &file, &identity )))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = get_identity(
            file, FALSE, &identity, &links, &size )) ||
        links != 1 || size < DEPLOYMENT_RECORD_HEADER_SIZE ||
        size > APPX_DEPLOYMENT_MAX_RECORD_SIZE || size > MAXDWORD)
    {
        if (SUCCEEDED(hr)) hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (!(data = HeapAlloc( GetProcessHeap(), 0, size )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!ReadFile( file, data, size, &read, NULL ) || read != size)
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (FAILED(hr = verify_identity( file, FALSE, &identity )))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    hr = parse_deployment_record( data, size, package, record );

done:
    HeapFree( GetProcessHeap(), 0, data );
    close_handle( &file );
    return hr;
}

static HRESULT publish_pending_record(
    struct deployment_store *store, struct deployment_backend *backend,
    HANDLE *pending, const struct object_identity *pending_identity,
    const struct appx_catalog_package *package, BOOL *reused, BOOL *weak )
{
    WCHAR name[DEPLOYMENT_GENERATION_CHARS +
               ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
    struct object_identity destination_identity;
    APPX_DEPLOYMENT_RECORD *existing = NULL;
    HANDLE destination = INVALID_HANDLE_VALUE;
    HRESULT hr;

    *reused = FALSE;
    if (FAILED(hr = make_record_file_name(
            package->payload_path, name, ARRAY_SIZE(name) )))
        return hr;
    hr = open_child( store->records, name, FILE_READ_DATA,
                     FILE_SHARE_READ | FILE_SHARE_DELETE,
                     FILE_OPEN, FALSE, &destination, &destination_identity );
    if (SUCCEEDED(hr))
    {
        if (FAILED(hr = require_single_link_file( destination )))
        {
            close_handle( &destination );
            return APPX_DEPLOYMENT_E_CORRUPT_STORE;
        }
        if (destination_identity.volume_serial !=
                pending_identity->volume_serial ||
            destination_identity.file_index_high !=
                pending_identity->file_index_high ||
            destination_identity.file_index_low !=
                pending_identity->file_index_low)
        {
            close_handle( &destination );
            return APPX_DEPLOYMENT_E_CONTENT_CONFLICT;
        }
        close_handle( pending );
        close_handle( &destination );
        if (FAILED(hr = load_record_from_store(
                store, package, &existing )))
            return hr;
        appx_deployment_record_free( existing );
        *reused = TRUE;
        return S_OK;
    }
    if (!is_not_found( hr )) return hr;
    if (FAILED(hr = require_single_link_file( *pending )))
        return hr;
    if (FAILED(hr = verify_identity( *pending, FALSE, pending_identity )))
        return hr;
    if (FAILED(hr = backend_rename(
            backend, *pending, store->records, name, FALSE )))
        return hr;
    if (FAILED(hr = mark_durability(
            backend,
            backend_flush( backend, store->record_staging, TRUE ), weak )))
        return hr;
    if (FAILED(hr = mark_durability(
            backend, backend_flush( backend, store->records, TRUE ), weak )))
        return hr;
    close_handle( pending );
    if (FAILED(hr = open_child(
            store->records, name, FILE_READ_DATA,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            FILE_OPEN, FALSE, &destination, &destination_identity )))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = require_single_link_file( destination )))
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
    else if (destination_identity.volume_serial !=
                 pending_identity->volume_serial ||
        destination_identity.file_index_high !=
            pending_identity->file_index_high ||
        destination_identity.file_index_low != pending_identity->file_index_low)
        hr = HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    close_handle( &destination );
    return hr;
}

static HRESULT verify_published_record(
    struct deployment_store *store,
    const struct appx_catalog_package *package )
{
    APPX_DEPLOYMENT_RECORD *record = NULL;
    HRESULT hr;

    if (FAILED(hr = load_record_from_store( store, package, &record )))
        return hr;
    hr = verify_record_inventory( store, package, record );
    appx_deployment_record_free( record );
    return hr;
}

void WINAPI appx_deployment_record_free( APPX_DEPLOYMENT_RECORD *record )
{
    if (!record) return;
    memset( record, 0, offsetof(APPX_DEPLOYMENT_RECORD, data) );
    HeapFree( GetProcessHeap(), 0, record );
}

UINT32 WINAPI appx_deployment_record_get_loader_file_count(
    const APPX_DEPLOYMENT_RECORD *record )
{
    return record ? record->loader_count : 0;
}

const struct appx_deployment_loader_file *WINAPI
appx_deployment_record_get_loader_file(
    const APPX_DEPLOYMENT_RECORD *record, UINT32 index )
{
    if (!record || index >= record->loader_count) return NULL;
    return record->loaders + index;
}

UINT32 WINAPI appx_deployment_record_get_inproc_class_count(
    const APPX_DEPLOYMENT_RECORD *record )
{
    return record ? record->class_count : 0;
}

const struct appx_deployment_inproc_class *WINAPI
appx_deployment_record_get_inproc_class(
    const APPX_DEPLOYMENT_RECORD *record, UINT32 index )
{
    if (!record || index >= record->class_count) return NULL;
    return record->classes + index;
}

UINT32 WINAPI appx_deployment_record_get_application_file_count(
    const APPX_DEPLOYMENT_RECORD *record )
{
    return record ? record->application_count : 0;
}

const struct appx_deployment_application_file *WINAPI
appx_deployment_record_get_application_file(
    const APPX_DEPLOYMENT_RECORD *record, UINT32 index )
{
    if (!record || index >= record->application_count) return NULL;
    return record->applications + index;
}

BOOL WINAPI appx_deployment_record_has_loader_search_path_override(
    const APPX_DEPLOYMENT_RECORD *record )
{
    return record && record->has_package_loader_search_path_override;
}

UINT32 WINAPI appx_deployment_record_get_loader_search_path_count(
    const APPX_DEPLOYMENT_RECORD *record )
{
    return record ? record->package_loader_search_path_count : 0;
}

const WCHAR * WINAPI appx_deployment_record_get_loader_search_path(
    const APPX_DEPLOYMENT_RECORD *record, UINT32 index )
{
    if (!record || index >= record->package_loader_search_path_count) return NULL;
    return record->loader_search_paths[index];
}

static HRESULT allocate_result( const WCHAR *full_name,
                                APPX_DEPLOYMENT_RESULT **output )
{
    APPX_DEPLOYMENT_RESULT *result;
    HRESULT hr;

    *output = NULL;
    if (!(result = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                              sizeof(*result) )))
        return E_OUTOFMEMORY;
    result->version = APPX_DEPLOYMENT_RESULT_VERSION;
    if (full_name &&
        FAILED(hr = duplicate_string(
            full_name, FALSE, &result->package_full_name )))
    {
        HeapFree( GetProcessHeap(), 0, result );
        return hr;
    }
    *output = result;
    return S_OK;
}

static HRESULT finish_result( APPX_DEPLOYMENT_RESULT *result, BOOL weak )
{
    if (!weak) return S_OK;
    result->flags |= APPX_DEPLOYMENT_RESULT_WEAK_DURABILITY;
    return APPX_DEPLOYMENT_S_WEAK_DURABILITY;
}

void WINAPI appx_deployment_result_free( APPX_DEPLOYMENT_RESULT *result )
{
    if (!result) return;
    HeapFree( GetProcessHeap(), 0, result->package_full_name );
    memset( result, 0, sizeof(*result) );
    HeapFree( GetProcessHeap(), 0, result );
}

UINT32 WINAPI appx_deployment_result_get_flags(
    const APPX_DEPLOYMENT_RESULT *result )
{
    return result ? result->flags : 0;
}

UINT64 WINAPI appx_deployment_result_get_catalog_epoch(
    const APPX_DEPLOYMENT_RESULT *result )
{
    return result ? result->catalog_epoch : 0;
}

UINT64 WINAPI appx_deployment_result_get_reclaimed_bytes(
    const APPX_DEPLOYMENT_RESULT *result )
{
    return result ? result->reclaimed_bytes : 0;
}

UINT32 WINAPI appx_deployment_result_get_reclaimed_entries(
    const APPX_DEPLOYMENT_RESULT *result )
{
    return result ? result->reclaimed_entries : 0;
}

HRESULT WINAPI appx_deployment_result_get_package_full_name(
    const APPX_DEPLOYMENT_RESULT *result, UINT32 *chars, WCHAR *name )
{
    UINT32 required, capacity;

    if (!result || !chars) return E_INVALIDARG;
    required = result->package_full_name ?
               lstrlenW( result->package_full_name ) + 1 : 0;
    capacity = *chars;
    *chars = required;
    if (!name) return S_OK;
    if (capacity < required)
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    if (required)
        memcpy( name, result->package_full_name,
                required * sizeof(*name) );
    return S_OK;
}

static HRESULT validate_catalog_file( struct deployment_store *store )
{
    HANDLE catalog_file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    hr = open_child( store->root, APPX_CATALOG_FILE_NAME, FILE_READ_DATA,
                     FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN, FALSE,
                     &catalog_file, NULL );
    if (SUCCEEDED(hr))
    {
        hr = require_single_link_file( catalog_file );
        close_handle( &catalog_file );
        if (FAILED(hr)) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }
    else if (!is_not_found( hr ))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    return S_OK;
}

static HRESULT validate_store_snapshot(
    struct deployment_store *store,
    const APPX_CATALOG_SNAPSHOT *catalog )
{
    HRESULT hr;

    if (FAILED(hr = validate_catalog_file( store )))
        return hr;
    if (FAILED(hr = verify_catalog_payloads( store, catalog )) ||
        FAILED(hr = verify_catalog_records( store, catalog )))
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    return S_OK;
}

static HRESULT remove_staging_artifacts(
    const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend, struct deployment_store *store,
    const struct deployment_journal *journal,
    APPX_DEPLOYMENT_RESULT *result, BOOL *weak )
{
    struct delete_budget budget;
    WCHAR record_name[DEPLOYMENT_TRANSACTION_CHARS +
                      ARRAY_SIZE(DEPLOYMENT_JOURNAL_SUFFIX)];
    HRESULT hr;

    memset( &budget, 0, sizeof(budget) );
    budget.max_entries = options->max_gc_entries;
    budget.max_bytes = options->max_gc_bytes;
    hr = delete_named_tree( store->staging, journal->staging_name,
                            &budget, backend, weak );
    if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
        is_sharing_failure( hr ))
    {
        result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
        hr = S_OK;
    }
    if (FAILED(hr)) return hr;
    lstrcpyW( record_name, journal->transaction_name );
    lstrcatW( record_name, DEPLOYMENT_JOURNAL_SUFFIX );
    hr = delete_named_file( store->record_staging, record_name,
                            &budget, backend, weak );
    if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
        is_sharing_failure( hr ))
    {
        result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
        hr = S_OK;
    }
    if (FAILED(hr)) return hr;
    hr = delete_named_file( store->lease_pending, journal->transaction_name,
                            &budget, backend, weak );
    if (hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA) ||
        is_sharing_failure( hr ))
    {
        result->flags |= APPX_DEPLOYMENT_RESULT_GC_DEFERRED;
        hr = S_OK;
    }
    result->reclaimed_entries += budget.entries;
    result->reclaimed_bytes += budget.bytes;
    return hr;
}

static HRESULT load_and_build_add(
    enum appx_deployment_operation operation,
    const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend, struct deployment_store *store,
    struct prepared_package *prepared,
    APPX_CATALOG_SNAPSHOT **current,
    APPX_CATALOG_SNAPSHOT **replacement, BOOL *no_change )
{
    HRESULT hr;

    *current = NULL;
    *replacement = NULL;
    if (FAILED(hr = backend_catalog_load(
            backend, options, store->path, current )) ||
        FAILED(hr = validate_store_snapshot( store, *current )) ||
        FAILED(hr = build_add_replacement(
            operation, *current, prepared, options,
            replacement, no_change )))
        return hr;
    return S_OK;
}

static HRESULT execute_add(
    enum appx_deployment_operation operation, HANDLE package_file,
    const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend,
    APPX_DEPLOYMENT_RESULT **output )
{
    APPX_CATALOG_SNAPSHOT *current = NULL, *replacement = NULL;
    APPX_CATALOG_SNAPSHOT *revalidated = NULL;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    struct prepared_package prepared;
    struct deployment_journal journal;
    struct deployment_store store;
    struct deployment_lock lock;
    struct object_identity staging_identity, record_identity, marker_identity;
    HANDLE staging = INVALID_HANDLE_VALUE, pending_record = INVALID_HANDLE_VALUE;
    HANDLE pending_marker = INVALID_HANDLE_VALUE;
    BOOL weak = FALSE, no_change = FALSE;
    BOOL payload_reused, record_reused, marker_reused;
    BOOL staged = FALSE;
    UINT32 attempt;
    HRESULT hr;

    *output = NULL;
    memset( &prepared, 0, sizeof(prepared) );
    memset( &journal, 0, sizeof(journal) );
    memset( &store, 0, sizeof(store) );
    memset( &lock, 0, sizeof(lock) );
    if (FAILED(hr = open_store(
            options, operation == APPX_DEPLOYMENT_OPERATION_INSTALL,
            backend, &store, &weak )) ||
        FAILED(hr = checkpoint(
            backend, APPX_DEPLOYMENT_CHECKPOINT_STORE_READY )) ||
        FAILED(hr = journal_initialize(
            backend, operation, &journal )) ||
        FAILED(hr = write_journal(
            &store, backend, &journal,
            APPX_DEPLOYMENT_JOURNAL_CREATED, &weak )) ||
        FAILED(hr = checkpoint(
            backend, APPX_DEPLOYMENT_CHECKPOINT_JOURNAL_CREATED )) ||
        FAILED(hr = check_cancelled( options )) ||
        FAILED(hr = prepare_package(
            package_file, options, backend, store.staging, &prepared )) ||
        FAILED(hr = journal_set_package(
            &journal, &prepared.test_view.package,
            prepared.test_view.package.payload_path )) ||
        FAILED(hr = write_journal(
            &store, backend, &journal,
            APPX_DEPLOYMENT_JOURNAL_INSPECTED, &weak )) ||
        FAILED(hr = checkpoint(
            backend, APPX_DEPLOYMENT_CHECKPOINT_INSPECTED )) ||
        FAILED(hr = allocate_result(
            prepared.test_view.package.full_name, &result )) ||
        FAILED(hr = load_and_build_add(
            operation, options, backend, &store, &prepared,
            &current, &replacement, &no_change )))
        goto done;

    if (!no_change)
    {
        if (FAILED(hr = create_staging_directory(
                &store, &journal, &staging, &staging_identity )) ||
            FAILED(hr = mark_durability(
                backend, backend_flush( backend, store.staging, TRUE ),
                &weak )) ||
            FAILED(hr = write_journal(
                &store, backend, &journal,
                APPX_DEPLOYMENT_JOURNAL_STAGED, &weak )) ||
            FAILED(hr = checkpoint(
                backend, APPX_DEPLOYMENT_CHECKPOINT_STAGING_CREATED )) ||
            FAILED(hr = extract_prepared_package(
                backend, &prepared, staging, options, &weak )) ||
            FAILED(hr = collect_production_loader_inventory(
                &prepared, staging )) ||
            FAILED(hr = checkpoint(
                backend, APPX_DEPLOYMENT_CHECKPOINT_EXTRACTED )) ||
            FAILED(hr = create_pending_record(
                &store, backend, &prepared, staging, &journal,
                &pending_record,
                &record_identity, &weak )) ||
            FAILED(hr = create_pending_generation_marker(
                &store, backend, &journal, &prepared.test_view.package,
                &pending_marker, &marker_identity, &weak )))
            goto done;
        staged = TRUE;
    }

    for (attempt = 0; attempt < options->max_epoch_retries; attempt++)
    {
        UINT64 expected_epoch =
            appx_catalog_snapshot_get_epoch( current );

        if (FAILED(hr = check_cancelled( options )) ||
            FAILED(hr = acquire_writer_lock(
                &store, options, &lock )))
            goto done;
        if (FAILED(hr = backend_catalog_load(
                backend, options, store.path, &revalidated )) ||
            FAILED(hr = validate_store_snapshot(
                &store, revalidated )))
            goto unlock_done;
        if (appx_catalog_snapshot_get_epoch( revalidated ) != expected_epoch)
        {
            release_store_lock( &lock );
            appx_catalog_snapshot_free( revalidated );
            revalidated = NULL;
            appx_catalog_snapshot_free( replacement );
            replacement = NULL;
            appx_catalog_snapshot_free( current );
            current = NULL;
            if (FAILED(hr = load_and_build_add(
                    operation, options, backend, &store, &prepared,
                    &current, &replacement, &no_change )))
                goto done;
            if (!no_change && !staged)
            {
                if (FAILED(hr = create_staging_directory(
                        &store, &journal, &staging,
                        &staging_identity )) ||
                    FAILED(hr = mark_durability(
                        backend,
                        backend_flush( backend, store.staging, TRUE ),
                        &weak )) ||
                    FAILED(hr = write_journal(
                        &store, backend, &journal,
                        APPX_DEPLOYMENT_JOURNAL_STAGED, &weak )) ||
                    FAILED(hr = checkpoint(
                        backend,
                        APPX_DEPLOYMENT_CHECKPOINT_STAGING_CREATED )) ||
                    FAILED(hr = extract_prepared_package(
                        backend, &prepared, staging, options, &weak )) ||
                    FAILED(hr = collect_production_loader_inventory(
                        &prepared, staging )) ||
                    FAILED(hr = checkpoint(
                        backend,
                        APPX_DEPLOYMENT_CHECKPOINT_EXTRACTED )) ||
                    FAILED(hr = create_pending_record(
                        &store, backend, &prepared, staging, &journal,
                        &pending_record, &record_identity, &weak )) ||
                    FAILED(hr = create_pending_generation_marker(
                        &store, backend, &journal,
                        &prepared.test_view.package, &pending_marker,
                        &marker_identity, &weak )))
                    goto done;
                staged = TRUE;
            }
            continue;
        }
        if (no_change)
        {
            result->flags |= APPX_DEPLOYMENT_RESULT_PAYLOAD_REUSED;
            result->catalog_epoch = expected_epoch;
            hr = S_OK;
            goto unlock_done;
        }

        if (FAILED(hr = publish_staging_directory(
                &store, backend, staging, &staging_identity,
                prepared.test_view.package.payload_path,
                &payload_reused, &weak )))
            goto unlock_done;
        if (FAILED(hr = advance_journal(
                &store, backend, &journal,
                APPX_DEPLOYMENT_JOURNAL_PAYLOAD_COMPLETE, &weak )))
            goto unlock_done;
        if (FAILED(hr = checkpoint(
                backend, APPX_DEPLOYMENT_CHECKPOINT_PAYLOAD_RENAMED )))
            goto unlock_done;
        hr = publish_pending_record(
            &store, backend, &pending_record, &record_identity,
            &prepared.test_view.package, &record_reused, &weak );
        if (FAILED(hr))
            goto unlock_done;
        if (payload_reused != record_reused)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto unlock_done;
        }
        hr = publish_generation_marker(
            &store, backend, &pending_marker, &marker_identity,
            &prepared.test_view.package, &marker_reused, &weak );
        if (FAILED(hr))
            goto unlock_done;
        if (payload_reused != marker_reused)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto unlock_done;
        }
        if (FAILED(hr = verify_published_record(
                &store, &prepared.test_view.package )))
            goto unlock_done;
        if (payload_reused)
            result->flags |= APPX_DEPLOYMENT_RESULT_PAYLOAD_REUSED;
        journal.expected_epoch = expected_epoch;
        if (FAILED(hr = advance_journal(
                &store, backend, &journal,
                APPX_DEPLOYMENT_JOURNAL_CATALOG_PREPARED, &weak )) ||
            FAILED(hr = checkpoint(
                backend, APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PREPARED )) ||
            FAILED(hr = backend_catalog_publish(
                backend, options, store.path, expected_epoch, replacement,
                &weak )))
            goto unlock_done;
        result->flags |= APPX_DEPLOYMENT_RESULT_CATALOG_CHANGED;
        result->catalog_epoch = expected_epoch + 1;
        if (FAILED(hr = checkpoint(
                backend, APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PUBLISHED )) ||
            FAILED(hr = write_journal(
                &store, backend, &journal,
                APPX_DEPLOYMENT_JOURNAL_PUBLISHED, &weak )))
            goto unlock_done;
        hr = S_OK;

unlock_done:
        release_store_lock( &lock );
        appx_catalog_snapshot_free( revalidated );
        revalidated = NULL;
        if (hr == APPX_CATALOG_E_EPOCH_CONFLICT)
        {
            appx_catalog_snapshot_free( replacement );
            replacement = NULL;
            appx_catalog_snapshot_free( current );
            current = NULL;
            if (FAILED(hr = load_and_build_add(
                    operation, options, backend, &store, &prepared,
                    &current, &replacement, &no_change )))
                goto done;
            if (!no_change && !staged)
            {
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto done;
            }
            continue;
        }
        if (FAILED(hr)) goto done;
        break;
    }
    if (attempt == options->max_epoch_retries)
    {
        hr = APPX_CATALOG_E_EPOCH_CONFLICT;
        goto done;
    }

    close_handle( &pending_record );
    close_handle( &pending_marker );
    close_handle( &staging );
    if (FAILED(hr = remove_staging_artifacts(
            options, backend, &store, &journal, result, &weak )))
        goto done;
    if ((result->flags & APPX_DEPLOYMENT_RESULT_CATALOG_CHANGED) &&
        !(options->flags & APPX_DEPLOYMENT_SKIP_GARBAGE_COLLECTION))
    {
        APPX_CATALOG_SNAPSHOT *published = NULL;

        if (FAILED(hr = backend_catalog_load(
                backend, options, store.path, &published )) ||
            FAILED(hr = validate_store_snapshot(
                &store, published )) ||
            FAILED(hr = collect_garbage_internal(
                options, backend, &store, published, result, &weak )))
        {
            appx_catalog_snapshot_free( published );
            goto done;
        }
        appx_catalog_snapshot_free( published );
    }
    if (FAILED(hr = write_journal(
            &store, backend, &journal,
            APPX_DEPLOYMENT_JOURNAL_CLEANED, &weak )) ||
        FAILED(hr = checkpoint(
            backend, APPX_DEPLOYMENT_CHECKPOINT_CLEANED )) ||
        FAILED(hr = remove_journal(
            &store, backend, &journal, &weak )))
        goto done;
    hr = finish_result( result, weak );
    *output = result;
    result = NULL;

done:
    release_store_lock( &lock );
    close_handle( &pending_record );
    close_handle( &pending_marker );
    close_handle( &staging );
    appx_catalog_snapshot_free( revalidated );
    appx_catalog_snapshot_free( replacement );
    appx_catalog_snapshot_free( current );
    appx_deployment_result_free( result );
    release_prepared_package( backend, &prepared );
    journal_destroy( &journal );
    close_store( &store );
    return hr;
}

static HRESULT load_and_build_remove(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend, struct deployment_store *store,
    APPX_CATALOG_SNAPSHOT **current,
    APPX_CATALOG_SNAPSHOT **replacement,
    const struct appx_catalog_package **removed )
{
    HRESULT hr;

    *current = NULL;
    *replacement = NULL;
    *removed = NULL;
    if (FAILED(hr = backend_catalog_load(
            backend, options, store->path, current )) ||
        FAILED(hr = validate_store_snapshot( store, *current )) ||
        FAILED(hr = build_remove_replacement(
            *current, full_name, options, replacement, removed )))
        return hr;
    return S_OK;
}

static HRESULT execute_remove(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend,
    APPX_DEPLOYMENT_RESULT **output )
{
    APPX_CATALOG_SNAPSHOT *current = NULL, *replacement = NULL;
    APPX_CATALOG_SNAPSHOT *revalidated = NULL, *published = NULL;
    const struct appx_catalog_package *removed = NULL;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    struct deployment_journal journal;
    struct deployment_store store;
    struct deployment_lock lock;
    BOOL weak = FALSE;
    UINT32 attempt;
    HRESULT hr;

    *output = NULL;
    memset( &journal, 0, sizeof(journal) );
    memset( &store, 0, sizeof(store) );
    memset( &lock, 0, sizeof(lock) );
    if (FAILED(hr = validate_relative_path( full_name, TRUE )))
        return E_INVALIDARG;
    if (FAILED(hr = open_store(
            options, FALSE, backend, &store, &weak )) ||
        FAILED(hr = checkpoint(
            backend, APPX_DEPLOYMENT_CHECKPOINT_STORE_READY )) ||
        FAILED(hr = journal_initialize(
            backend, APPX_DEPLOYMENT_OPERATION_REMOVE, &journal )) ||
        FAILED(hr = write_journal(
            &store, backend, &journal,
            APPX_DEPLOYMENT_JOURNAL_CREATED, &weak )) ||
        FAILED(hr = checkpoint(
            backend, APPX_DEPLOYMENT_CHECKPOINT_JOURNAL_CREATED )) ||
        FAILED(hr = check_cancelled( options )) ||
        FAILED(hr = allocate_result( full_name, &result )) ||
        FAILED(hr = load_and_build_remove(
            full_name, options, backend, &store, &current,
            &replacement, &removed )) ||
        FAILED(hr = journal_set_package(
            &journal, removed, removed->payload_path )) ||
        FAILED(hr = write_journal(
            &store, backend, &journal,
            APPX_DEPLOYMENT_JOURNAL_DEPENDENCY_CHECKED, &weak )))
        goto done;

    for (attempt = 0; attempt < options->max_epoch_retries; attempt++)
    {
        UINT64 expected_epoch =
            appx_catalog_snapshot_get_epoch( current );

        if (FAILED(hr = check_cancelled( options )) ||
            FAILED(hr = acquire_writer_lock(
                &store, options, &lock )))
            goto done;
        if (FAILED(hr = backend_catalog_load(
                backend, options, store.path, &revalidated )) ||
            FAILED(hr = validate_store_snapshot(
                &store, revalidated )))
            goto unlock_done;
        if (appx_catalog_snapshot_get_epoch( revalidated ) != expected_epoch)
        {
            release_store_lock( &lock );
            appx_catalog_snapshot_free( revalidated );
            revalidated = NULL;
            appx_catalog_snapshot_free( replacement );
            replacement = NULL;
            appx_catalog_snapshot_free( current );
            current = NULL;
            if (FAILED(hr = load_and_build_remove(
                    full_name, options, backend, &store, &current,
                    &replacement, &removed )))
                goto done;
            journal.expected_epoch =
                appx_catalog_snapshot_get_epoch( current );
            if (FAILED(hr = journal_set_package(
                    &journal, removed, removed->payload_path )) ||
                FAILED(hr = advance_journal(
                    &store, backend, &journal,
                    APPX_DEPLOYMENT_JOURNAL_DEPENDENCY_CHECKED, &weak )))
                goto done;
            continue;
        }
        journal.expected_epoch = expected_epoch;
        if (FAILED(hr = advance_journal(
                &store, backend, &journal,
                APPX_DEPLOYMENT_JOURNAL_CATALOG_PREPARED, &weak )) ||
            FAILED(hr = checkpoint(
                backend, APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PREPARED )) ||
            FAILED(hr = backend_catalog_publish(
                backend, options, store.path, expected_epoch, replacement,
                &weak )))
            goto unlock_done;
        result->flags |= APPX_DEPLOYMENT_RESULT_CATALOG_CHANGED;
        result->catalog_epoch = expected_epoch + 1;
        if (FAILED(hr = checkpoint(
                backend, APPX_DEPLOYMENT_CHECKPOINT_CATALOG_UNPUBLISHED )) ||
            FAILED(hr = write_journal(
                &store, backend, &journal,
                APPX_DEPLOYMENT_JOURNAL_PUBLISHED, &weak )))
            goto unlock_done;
        hr = S_OK;

unlock_done:
        release_store_lock( &lock );
        appx_catalog_snapshot_free( revalidated );
        revalidated = NULL;
        if (hr == APPX_CATALOG_E_EPOCH_CONFLICT)
        {
            appx_catalog_snapshot_free( replacement );
            replacement = NULL;
            appx_catalog_snapshot_free( current );
            current = NULL;
            if (FAILED(hr = load_and_build_remove(
                    full_name, options, backend, &store, &current,
                    &replacement, &removed )))
                goto done;
            if (FAILED(hr = journal_set_package(
                    &journal, removed, removed->payload_path )) ||
                FAILED(hr = advance_journal(
                    &store, backend, &journal,
                    APPX_DEPLOYMENT_JOURNAL_DEPENDENCY_CHECKED, &weak )))
                goto done;
            continue;
        }
        if (FAILED(hr)) goto done;
        break;
    }
    if (attempt == options->max_epoch_retries)
    {
        hr = APPX_CATALOG_E_EPOCH_CONFLICT;
        goto done;
    }

    if (FAILED(hr = write_journal(
            &store, backend, &journal,
            APPX_DEPLOYMENT_JOURNAL_GC_PENDING, &weak )))
        goto done;
    if (!(options->flags & APPX_DEPLOYMENT_SKIP_GARBAGE_COLLECTION))
    {
        if (FAILED(hr = backend_catalog_load(
                backend, options, store.path, &published )) ||
            FAILED(hr = validate_store_snapshot(
                &store, published )) ||
            FAILED(hr = collect_garbage_internal(
                options, backend, &store, published, result, &weak )))
            goto done;
    }
    if (FAILED(hr = write_journal(
            &store, backend, &journal,
            APPX_DEPLOYMENT_JOURNAL_CLEANED, &weak )) ||
        FAILED(hr = checkpoint(
            backend, APPX_DEPLOYMENT_CHECKPOINT_CLEANED )) ||
        FAILED(hr = remove_journal(
            &store, backend, &journal, &weak )))
        goto done;
    hr = finish_result( result, weak );
    *output = result;
    result = NULL;

done:
    release_store_lock( &lock );
    appx_catalog_snapshot_free( published );
    appx_catalog_snapshot_free( revalidated );
    appx_catalog_snapshot_free( replacement );
    appx_catalog_snapshot_free( current );
    appx_deployment_result_free( result );
    journal_destroy( &journal );
    close_store( &store );
    return hr;
}

static HRESULT execute_recovery_or_gc(
    enum appx_deployment_operation operation,
    const APPX_DEPLOYMENT_OPTIONS *options,
    struct deployment_backend *backend,
    APPX_DEPLOYMENT_RESULT **output )
{
    APPX_CATALOG_SNAPSHOT *catalog = NULL;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    struct deployment_store store;
    struct deployment_lock lock;
    BOOL weak = FALSE;
    HRESULT hr;

    *output = NULL;
    memset( &store, 0, sizeof(store) );
    memset( &lock, 0, sizeof(lock) );
    if (FAILED(hr = open_store(
            options, FALSE, backend, &store, &weak )) ||
        FAILED(hr = checkpoint(
            backend, APPX_DEPLOYMENT_CHECKPOINT_STORE_READY )) ||
        FAILED(hr = allocate_result( NULL, &result )))
        goto done;
    if (operation == APPX_DEPLOYMENT_OPERATION_RECOVER &&
        FAILED(hr = acquire_writer_lock( &store, options, &lock )))
        goto done;
    if (FAILED(hr = backend_catalog_load(
            backend, options, store.path, &catalog )) ||
        FAILED(hr = validate_store_snapshot(
            &store, catalog )))
        goto done;
    result->catalog_epoch = appx_catalog_snapshot_get_epoch( catalog );
    if (operation == APPX_DEPLOYMENT_OPERATION_RECOVER &&
        FAILED(hr = recover_internal(
            options, backend, &store, catalog, result, &weak )))
        goto done;
    release_store_lock( &lock );
    if ((operation == APPX_DEPLOYMENT_OPERATION_GARBAGE_COLLECT ||
         !(options->flags & APPX_DEPLOYMENT_SKIP_GARBAGE_COLLECTION)) &&
        FAILED(hr = collect_garbage_internal(
            options, backend, &store, catalog, result, &weak )))
        goto done;
    hr = finish_result( result, weak );
    *output = result;
    result = NULL;

done:
    release_store_lock( &lock );
    appx_catalog_snapshot_free( catalog );
    appx_deployment_result_free( result );
    close_store( &store );
    return hr;
}

HRESULT WINAPI appx_deployment_initialize(
    const APPX_DEPLOYMENT_OPTIONS *input,
    APPX_DEPLOYMENT_RESULT **output )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_CATALOG_SNAPSHOT *catalog = NULL;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    struct deployment_backend backend = {0};
    struct deployment_store store;
    BOOL weak = FALSE;
    HRESULT hr;

    if (!output) return E_INVALIDARG;
    *output = NULL;
    memset( &store, 0, sizeof(store) );
    if (FAILED(hr = validate_options( input, &options )))
        goto done;
    backend.accept_weak_durability =
        !!(options.flags & APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY);
    if (FAILED(hr = open_store(
            &options, TRUE, &backend, &store, &weak )) ||
        FAILED(hr = checkpoint(
            &backend, APPX_DEPLOYMENT_CHECKPOINT_STORE_READY )) ||
        FAILED(hr = backend_catalog_load(
            &backend, &options, store.path, &catalog )) ||
        FAILED(hr = validate_store_snapshot(
            &store, catalog )) ||
        FAILED(hr = allocate_result( NULL, &result )))
        goto done;
    result->catalog_epoch = appx_catalog_snapshot_get_epoch( catalog );
    hr = finish_result( result, weak );
    *output = result;
    result = NULL;

done:
    appx_deployment_result_free( result );
    appx_catalog_snapshot_free( catalog );
    close_store( &store );
    return hr;
}

HRESULT WINAPI appx_deployment_query(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *input,
    APPX_CATALOG_SNAPSHOT **output )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_CATALOG_SNAPSHOT *catalog = NULL;
    struct deployment_backend backend = {0};
    struct deployment_store store;
    BOOL weak = FALSE;
    HRESULT hr;

    if (!output) return E_INVALIDARG;
    *output = NULL;
    memset( &store, 0, sizeof(store) );
    if (FAILED(hr = validate_options( input, &options ))) return hr;
    backend.accept_weak_durability =
        !!(options.flags & APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY);
    if (full_name &&
        FAILED(validate_relative_path( full_name, TRUE )))
        return E_INVALIDARG;
    if (FAILED(hr = open_store(
            &options, TRUE, &backend, &store, &weak )) ||
        FAILED(hr = backend_catalog_load(
            &backend, &options, store.path, &catalog )) ||
        FAILED(hr = validate_store_snapshot(
            &store, catalog )))
        goto done;
    if (full_name)
    {
        const struct appx_catalog_package *package = NULL;

        if (FAILED(hr = find_catalog_package(
                catalog, full_name, &package, NULL )))
            goto done;
        if (!package)
        {
            hr = HRESULT_FROM_WIN32( ERROR_INSTALL_PACKAGE_NOT_FOUND );
            goto done;
        }
        hr = appx_catalog_snapshot_create(
            appx_catalog_snapshot_get_epoch( catalog ), package, 1, output );
    }
    else
        hr = appx_catalog_snapshot_deep_copy( catalog, output );
    if (SUCCEEDED(hr) && weak) hr = APPX_DEPLOYMENT_S_WEAK_DURABILITY;

done:
    appx_catalog_snapshot_free( catalog );
    close_store( &store );
    return hr;
}

HRESULT WINAPI appx_deployment_record_load(
    const struct appx_catalog_package *package,
    const APPX_DEPLOYMENT_OPTIONS *input,
    APPX_DEPLOYMENT_RECORD **output )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_CATALOG_SNAPSHOT *catalog = NULL;
    const struct appx_catalog_package *installed = NULL;
    struct deployment_backend backend = {0};
    struct deployment_store store;
    BOOL weak = FALSE;
    HRESULT hr;

    if (!output) return E_INVALIDARG;
    *output = NULL;
    memset( &store, 0, sizeof(store) );
    if (!package || !package->full_name ||
        FAILED(hr = validate_options( input, &options )))
        return E_INVALIDARG;
    backend.accept_weak_durability =
        !!(options.flags & APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY);
    if (FAILED(hr = open_store(
            &options, FALSE, &backend, &store, &weak )) ||
        FAILED(hr = backend_catalog_load(
            &backend, &options, store.path, &catalog )) ||
        FAILED(hr = find_catalog_package(
            catalog, package->full_name, &installed, NULL )))
        goto done;
    if (!installed ||
        !equal_content_id( installed->content_id, package->content_id ) ||
        lstrcmpW( installed->payload_path, package->payload_path ))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (FAILED(hr = verify_catalog_payloads( &store, catalog )) ||
        FAILED(hr = load_record_from_store(
            &store, installed, output )) ||
        FAILED(hr = verify_record_inventory(
            &store, installed, *output )))
    {
        appx_deployment_record_free( *output );
        *output = NULL;
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }

done:
    appx_catalog_snapshot_free( catalog );
    close_store( &store );
    return hr;
}

static void free_runtime_records( struct runtime_record *records,
                                  UINT32 count )
{
    UINT32 i;

    if (!records) return;
    for (i = 0; i < count; i++)
        appx_deployment_record_free( records[i].record );
    HeapFree( GetProcessHeap(), 0, records );
}

static HRESULT allocate_joined_path( const WCHAR *root,
                                     const WCHAR *relative, WCHAR **path )
{
    UINT32 root_chars, relative_chars, total_chars;
    BOOL separator;

    *path = NULL;
    relative = optional_string( relative );
    if (!relative[0]) return duplicate_string( root, FALSE, path );
    if (FAILED(bounded_string_length( root, FALSE, &root_chars )) ||
        FAILED(bounded_string_length( relative, FALSE, &relative_chars )))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    separator = root[root_chars - 2] != '\\';
    if (!add_uint32( root_chars - 1, separator, &total_chars ) ||
        !add_uint32( total_chars, relative_chars, &total_chars ) ||
        !(*path = HeapAlloc(
            GetProcessHeap(), 0, total_chars * sizeof(**path) )))
        return E_OUTOFMEMORY;
    memcpy( *path, root, (root_chars - 1) * sizeof(**path) );
    total_chars = root_chars - 1;
    if (separator) (*path)[total_chars++] = '\\';
    memcpy( *path + total_chars, relative,
            relative_chars * sizeof(**path) );
    return S_OK;
}

static const struct appx_deployment_application_file *find_record_application(
    const APPX_DEPLOYMENT_RECORD *record, const WCHAR *application_id )
{
    UINT32 i;

    for (i = 0; i < record->application_count; i++)
        if (!compare_string_exact(
                record->applications[i].id, application_id ))
            return record->applications + i;
    return NULL;
}

static HRESULT open_payload_generation_directory(
    struct deployment_store *store, const struct appx_catalog_package *package,
    HANDLE *directory )
{
    const WCHAR *generation = payload_generation_name( package->payload_path );
    struct object_identity identity;

    *directory = INVALID_HANDLE_VALUE;
    if (!generation) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    return open_path_component( store->payloads, generation, FALSE,
                                directory, &identity );
}

static HRESULT open_generation_marker_lease(
    struct deployment_store *store, const struct appx_catalog_package *package,
    HANDLE *lease )
{
    const WCHAR *generation = payload_generation_name( package->payload_path );
    struct object_identity identity;
    HRESULT hr;

    *lease = INVALID_HANDLE_VALUE;
    if (!generation) return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    if (FAILED(hr = open_child(
            store->lease_generations, generation, FILE_READ_DATA,
            FILE_SHARE_READ, FILE_OPEN, FALSE, lease, &identity )))
        return hr;
    if (FAILED(hr = validate_generation_marker( *lease, package, &identity )))
        close_handle( lease );
    return hr;
}

static HRESULT query_runtime_object_id(
    HANDLE file, BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE] )
{
    FILE_OBJECTID_BUFFER information;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    UINT32 i;

    memset( object_id, 0, WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    memset( &information, 0, sizeof(information) );
    status = NtFsControlFile(
        file, 0, NULL, NULL, &io, FSCTL_GET_OBJECT_ID, NULL, 0,
        &information, sizeof(information) );
    if (status) return ntstatus_error( status );

    memcpy( object_id, information.ObjectId,
            WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    for (i = 0; i < WINE_APPX_GRAPH_OBJECT_ID_SIZE; i++)
        if (object_id[i]) return S_OK;

    memset( object_id, 0, WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    return APPX_DEPLOYMENT_E_CORRUPT_STORE;
}

static HRESULT open_runtime_executable(
    struct deployment_store *store, const struct appx_catalog_package *package,
    const struct appx_deployment_application_file *application,
    HANDLE *file, BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE] )
{
    struct appx_deployment_file_integrity integrity;
    struct object_identity identity;
    HANDLE directory = INVALID_HANDLE_VALUE;
    HRESULT hr;

    *file = INVALID_HANDLE_VALUE;
    memset( object_id, 0, WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    if (FAILED(hr = open_payload_generation_directory(
            store, package, &directory )))
        return hr;
    if (FAILED(hr = open_payload_file(
            directory, application->executable, file, &identity )))
        goto done;
    if (!deployment_file_identity_matches_object(
            &application->identity, &identity ) ||
        FAILED(hr = calculate_file_integrity(
            *file, &identity, &integrity )) ||
        !deployment_file_integrity_matches(
            &application->integrity, &integrity ))
    {
        close_handle( file );
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }
    else if (FAILED(hr = query_runtime_object_id( *file, object_id )))
        close_handle( file );

done:
    close_handle( &directory );
    return hr;
}

static BOOL deployment_file_identities_equal(
    const struct appx_deployment_file_identity *left,
    const struct appx_deployment_file_identity *right )
{
    return left->volume_serial == right->volume_serial &&
           left->file_index_high == right->file_index_high &&
           left->file_index_low == right->file_index_low;
}

static const struct appx_deployment_loader_file *find_runtime_loader(
    const APPX_DEPLOYMENT_RECORD *record, const WCHAR *path )
{
    UINT32 low = 0, high = record->loader_count;

    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;
        INT comparison = compare_string_ci(
            record->loaders[middle].relative_path, path );

        if (comparison < 0) low = middle + 1;
        else high = middle;
    }
    if (low >= record->loader_count ||
        compare_string_ci( record->loaders[low].relative_path, path ))
        return NULL;
    return record->loaders + low;
}

static HRESULT capture_runtime_file_stamp(
    HANDLE directory, const WCHAR *path,
    const struct appx_deployment_file_identity *expected_identity,
    const struct appx_deployment_file_integrity *expected_integrity,
    UINT64 *change_time, UINT64 *file_size,
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE] )
{
    struct appx_deployment_file_integrity integrity;
    FILE_STANDARD_INFORMATION standard;
    FILE_BASIC_INFORMATION basic;
    struct object_identity before, after;
    HANDLE file = INVALID_HANDLE_VALUE;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    HRESULT hr;

    *change_time = 0;
    *file_size = 0;
    memset( object_id, 0, WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    if (!deployment_file_identity_is_valid( expected_identity ) ||
        FAILED(hr = open_payload_file(
            directory, path, &file, &before )) ||
        !deployment_file_identity_matches_object(
            expected_identity, &before ) ||
        FAILED(hr = calculate_file_integrity(
            file, &before, &integrity )) ||
        !deployment_file_integrity_matches(
            expected_integrity, &integrity ))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (FAILED(hr = query_runtime_object_id( file, object_id )))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if ((status = NtQueryInformationFile(
             file, &io, &basic, sizeof(basic), FileBasicInformation )) ||
        (status = NtQueryInformationFile(
             file, &io, &standard, sizeof(standard),
             FileStandardInformation )) ||
        FAILED(hr = get_identity(
            file, FALSE, &after, NULL, NULL )))
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    if (!object_identity_matches( &before, &after ) ||
        basic.ChangeTime.QuadPart <= 0 ||
        standard.EndOfFile.QuadPart <= 0 ||
        standard.EndOfFile.QuadPart != integrity.size ||
        standard.AllocationSize.QuadPart < standard.EndOfFile.QuadPart ||
        standard.Directory)
    {
        hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
        goto done;
    }
    *change_time = basic.ChangeTime.QuadPart;
    *file_size = standard.EndOfFile.QuadPart;
    hr = S_OK;

done:
    close_handle( &file );
    return hr;
}

static UINT32 get_loader_search_rank(
    const WCHAR *relative_path, BOOL override_present,
    const WCHAR * const *search_paths, UINT32 search_path_count )
{
    UINT32 length = lstrlenW( relative_path ), parent_length = 0, i;

    for (i = 0; i < length; i++)
        if (relative_path[i] == '\\') parent_length = i;
    /*
     * A present override with no LoaderSearchPathEntry children retains the
     * package root as the schema-defined default.  Once at least one entry is
     * declared, only those directories participate (an empty FolderPath is
     * the explicit way to place the root among other entries).
     */
    if (!override_present || !search_path_count)
        return parent_length ? WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY : 0;
    for (i = 0; i < search_path_count; i++)
    {
        UINT32 search_length = lstrlenW( search_paths[i] );

        if (search_length == parent_length &&
            (!parent_length ||
             CompareStringOrdinal( relative_path, parent_length,
                                   search_paths[i], search_length,
                                   TRUE ) == CSTR_EQUAL))
            return i;
    }
    return WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY;
}

static HRESULT load_runtime_records(
    struct deployment_store *store, const APPX_CATALOG_SNAPSHOT *catalog,
    const APPX_PACKAGE_GRAPH *resolved_graph,
    APPX_DEPLOYMENT_RECORD *main_record, const WCHAR *application_id,
    struct runtime_record **records, UINT32 *record_count,
    struct appx_graph_loader_file **loader_files, UINT32 *loader_count,
    struct appx_graph_inproc_class **classes, UINT32 *class_count )
{
    UINT32 count = appx_package_graph_get_package_count( resolved_graph ), i, j;
    struct appx_graph_loader_file *loaders = NULL;
    struct appx_graph_inproc_class *class_array = NULL;
    struct runtime_record *loaded = NULL;
    APPX_DEPLOYMENT_RECORD *unconsumed_main = main_record;
    UINT32 loaded_count = 0, total_loaders = 0, total_classes = 0;
    HRESULT hr = S_OK;

    *records = NULL;
    *record_count = 0;
    *loader_files = NULL;
    *loader_count = 0;
    *classes = NULL;
    *class_count = 0;
    if (!count || !main_record)
    {
        appx_deployment_record_free( main_record );
        return APPX_DEPLOYMENT_E_CORRUPT_STORE;
    }
    if (count &&
        !(loaded = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                              count * sizeof(*loaded) )))
    {
        appx_deployment_record_free( unconsumed_main );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < count; i++)
    {
        struct appx_graph_package graph_package;
        const struct appx_catalog_package *package = NULL;
        APPX_DEPLOYMENT_RECORD *record = NULL;

        if (FAILED(hr = appx_package_graph_get_package(
                resolved_graph, i, &graph_package )) ||
            FAILED(hr = find_catalog_package(
                catalog, graph_package.full_name, &package, NULL )) ||
            !package || !(package->flags & APPX_CATALOG_PACKAGE_ACTIVE))
        {
            if (SUCCEEDED(hr)) hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        if (!i)
        {
            record = unconsumed_main;
            unconsumed_main = NULL;
        }
        else if (FAILED(hr = load_record_from_store(
                     store, package, &record )))
        {
            appx_deployment_record_free( record );
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        if (!add_uint32( total_loaders, record->loader_count,
                         &total_loaders ) ||
            !add_uint32( total_classes, record->class_count,
                         &total_classes ) ||
            total_loaders > APPX_GRAPH_MAX_LOADER_FILES ||
            total_classes > APPX_GRAPH_MAX_CLASSES)
        {
            appx_deployment_record_free( record );
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }
        loaded[loaded_count].package = package;
        loaded[loaded_count].record = record;
        loaded_count++;
    }
    if (total_loaders &&
        !(loaders = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                               total_loaders * sizeof(*loaders) )))
    {
        hr = E_OUTOFMEMORY;
        goto failed;
    }
    if (total_classes &&
        !(class_array = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   total_classes * sizeof(*class_array) )))
    {
        hr = E_OUTOFMEMORY;
        goto failed;
    }
    total_loaders = total_classes = 0;
    for (i = 0; i < loaded_count; i++)
    {
        const struct appx_catalog_package *package = loaded[i].package;
        const APPX_DEPLOYMENT_RECORD *record = loaded[i].record;
        const WCHAR * const *search_paths = record->loader_search_paths;
        UINT32 search_path_count = record->package_loader_search_path_count;
        UINT32 package_loader_start = total_loaders;
        BOOL override_present =
            record->has_package_loader_search_path_override;
        HANDLE directory = INVALID_HANDLE_VALUE;

        if (!i)
        {
            const struct appx_deployment_application_file *application =
                find_record_application( record, application_id );

            if (application &&
                application->has_loader_search_path_override)
            {
                search_paths = application->loader_search_paths;
                search_path_count = application->loader_search_path_count;
                override_present = TRUE;
            }
        }
        if (record->loader_count &&
            FAILED(hr = open_payload_generation_directory(
                store, package, &directory )))
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto failed;
        }

        for (j = 0; j < record->loader_count; j++)
        {
            loaders[total_loaders].package_full_name = package->full_name;
            loaders[total_loaders].relative_path =
                record->loaders[j].relative_path;
            loaders[total_loaders].search_rank = get_loader_search_rank(
                record->loaders[j].relative_path, override_present,
                search_paths, search_path_count );
            loaders[total_loaders].volume_serial =
                record->loaders[j].identity.volume_serial;
            loaders[total_loaders].file_index_high =
                record->loaders[j].identity.file_index_high;
            loaders[total_loaders].file_index_low =
                record->loaders[j].identity.file_index_low;
            if (FAILED(hr = capture_runtime_file_stamp(
                    directory, record->loaders[j].relative_path,
                    &record->loaders[j].identity,
                    &record->loaders[j].integrity,
                    &loaders[total_loaders].change_time,
                    &loaders[total_loaders].file_size,
                    loaders[total_loaders].object_id )))
            {
                close_handle( &directory );
                goto failed;
            }
            total_loaders++;
        }
        for (j = 0; j < record->class_count; j++)
        {
            const struct appx_deployment_loader_file *loader =
                find_runtime_loader( record, record->classes[j].path );
            UINT32 loader_index;

            if (!loader ||
                !deployment_file_identities_equal(
                    &loader->identity, &record->classes[j].identity ) ||
                !deployment_file_integrity_matches(
                    &loader->integrity, &record->classes[j].integrity ))
            {
                close_handle( &directory );
                hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
                goto failed;
            }
            loader_index = loader - record->loaders;
            class_array[total_classes].package_full_name =
                package->full_name;
            class_array[total_classes].activatable_class_id =
                record->classes[j].activatable_class_id;
            class_array[total_classes].path = record->classes[j].path;
            class_array[total_classes].threading_model =
                record->classes[j].threading_model;
            class_array[total_classes].volume_serial =
                record->classes[j].identity.volume_serial;
            class_array[total_classes].file_index_high =
                record->classes[j].identity.file_index_high;
            class_array[total_classes].file_index_low =
                record->classes[j].identity.file_index_low;
            class_array[total_classes].change_time =
                loaders[package_loader_start + loader_index].change_time;
            class_array[total_classes].file_size =
                loaders[package_loader_start + loader_index].file_size;
            total_classes++;
        }
        close_handle( &directory );
    }
    *records = loaded;
    *record_count = loaded_count;
    *loader_files = loaders;
    *loader_count = total_loaders;
    *classes = class_array;
    *class_count = total_classes;
    return S_OK;

failed:
    appx_deployment_record_free( unconsumed_main );
    HeapFree( GetProcessHeap(), 0, class_array );
    HeapFree( GetProcessHeap(), 0, loaders );
    free_runtime_records( loaded, loaded_count );
    return hr;
}

void WINAPI appx_deployment_runtime_free( APPX_DEPLOYMENT_RUNTIME *runtime )
{
    UINT32 i;

    if (!runtime) return;
    close_handle( &runtime->executable );
    for (i = 0; i < runtime->lease_count; i++)
        close_handle( runtime->leases + i );
    HeapFree( GetProcessHeap(), 0, runtime->lease_values );
    HeapFree( GetProcessHeap(), 0, runtime->leases );
    appx_package_graph_free( runtime->graph );
    appx_catalog_snapshot_free( runtime->catalog );
    HeapFree( GetProcessHeap(), 0, runtime->executable_path );
    HeapFree( GetProcessHeap(), 0, runtime->parameters );
    HeapFree( GetProcessHeap(), 0, runtime->current_directory );
    memset( runtime, 0, sizeof(*runtime) );
    HeapFree( GetProcessHeap(), 0, runtime );
}

HRESULT WINAPI appx_deployment_runtime_acquire(
    const WCHAR *full_name, const WCHAR *application_id,
    const APPX_DEPLOYMENT_OPTIONS *input,
    APPX_DEPLOYMENT_RUNTIME **output )
{
    const struct appx_deployment_application_file *application = NULL;
    struct appx_graph_loader_file *loader_files = NULL;
    struct appx_graph_inproc_class *classes = NULL;
    const struct appx_catalog_package *package = NULL;
    APPX_DEPLOYMENT_RECORD *main_record = NULL;
    APPX_PACKAGE_GRAPH *resolved_graph = NULL;
    struct appx_graph_file_identity app_identity;
    struct runtime_record *records = NULL;
    APPX_DEPLOYMENT_RUNTIME *runtime = NULL;
    APPX_DEPLOYMENT_OPTIONS options;
    struct appx_architecture_policy architecture_policy;
    struct deployment_backend backend = {0};
    struct deployment_store store;
    enum appx_catalog_architecture executable_architecture;
    WCHAR *payload_root = NULL;
    UINT32 record_count = 0, loader_count = 0, class_count = 0, i;
    BOOL weak = FALSE;
    HRESULT hr;

    if (!output) return E_INVALIDARG;
    *output = NULL;
    memset( &store, 0, sizeof(store) );
    if (!full_name || !application_id ||
        FAILED(validate_options( input, &options )) ||
        FAILED(bounded_string_length( full_name, FALSE, &i )) ||
        FAILED(bounded_string_length( application_id, FALSE, &i )))
        return E_INVALIDARG;
    backend.accept_weak_durability =
        !!(options.flags & APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY);
    if (!(runtime = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                               sizeof(*runtime) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    runtime->executable = INVALID_HANDLE_VALUE;
    memset( &app_identity, 0, sizeof(app_identity) );
    if (FAILED(hr = open_store(
            &options, FALSE, &backend, &store, &weak )) ||
        FAILED(hr = backend_catalog_load(
            &backend, &options, store.path, &runtime->catalog )) ||
        FAILED(hr = validate_catalog_file( &store )) ||
        FAILED(hr = find_catalog_package(
            runtime->catalog, full_name, &package, NULL )))
        goto done;
    if (!package || !(package->flags & APPX_CATALOG_PACKAGE_ACTIVE))
    {
        hr = HRESULT_FROM_WIN32( ERROR_INSTALL_PACKAGE_NOT_FOUND );
        goto done;
    }
    if (FAILED(hr = load_record_from_store(
            &store, package, &main_record )))
        goto done;
    application = find_record_application( main_record, application_id );
    if (!application)
    {
        hr = HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
        goto done;
    }
    if (FAILED(hr = open_runtime_executable(
            &store, package, application, &runtime->executable,
            app_identity.object_id )) ||
        FAILED(hr = appx_architecture_query_host_policy(
            APPX_CATALOG_ARCHITECTURE_NEUTRAL,
            &architecture_policy )) ||
        FAILED(hr = appx_architecture_validate_executable(
            runtime->executable, package->architecture,
            &architecture_policy, &executable_architecture )))
        goto done;
    app_identity.volume_serial = application->identity.volume_serial;
    app_identity.file_index_high = application->identity.file_index_high;
    app_identity.file_index_low = application->identity.file_index_low;
    if (FAILED(hr = appx_package_graph_create(
            runtime->catalog, store.path, full_name, application_id,
            executable_architecture,
            appx_catalog_snapshot_get_epoch( runtime->catalog ),
            &app_identity, NULL, 0, &resolved_graph )))
        goto done;
    hr = load_runtime_records(
        &store, runtime->catalog, resolved_graph, main_record, application_id,
        &records, &record_count, &loader_files, &loader_count,
        &classes, &class_count );
    main_record = NULL;
    if (FAILED(hr)) goto done;
    if (FAILED(hr = appx_package_graph_create_with_classes(
            runtime->catalog, store.path, full_name, application_id,
            executable_architecture,
            appx_catalog_snapshot_get_epoch( runtime->catalog ),
            &app_identity, loader_files, loader_count, classes, class_count,
            &runtime->graph )))
        goto done;
    runtime->lease_count =
        appx_package_graph_get_package_count( runtime->graph );
    if (runtime->lease_count &&
        !(runtime->leases = HeapAlloc(
              GetProcessHeap(), 0,
              runtime->lease_count * sizeof(*runtime->leases) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < runtime->lease_count; i++)
        runtime->leases[i] = INVALID_HANDLE_VALUE;
    if (runtime->lease_count &&
        !(runtime->lease_values = HeapAlloc(
              GetProcessHeap(), 0,
              runtime->lease_count * sizeof(*runtime->lease_values) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < runtime->lease_count; i++)
    {
        struct appx_graph_package graph_package;
        const struct appx_catalog_package *lease_package = NULL;

        if (FAILED(hr = appx_package_graph_get_package(
                runtime->graph, i, &graph_package )) ||
            FAILED(hr = find_catalog_package(
                runtime->catalog, graph_package.full_name,
                &lease_package, NULL )) ||
            !lease_package ||
            FAILED(hr = open_generation_marker_lease(
                &store, lease_package, runtime->leases + i )))
        {
            if (SUCCEEDED(hr)) hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
        runtime->lease_values[i] = (ULONG_PTR)runtime->leases[i];
    }
    if (FAILED(hr = allocate_joined_path(
            store.path, package->payload_path, &payload_root )) ||
        FAILED(hr = allocate_joined_path(
            payload_root, application->executable,
            &runtime->executable_path )) ||
        FAILED(hr = duplicate_string(
            application->parameters, TRUE, &runtime->parameters )) ||
        FAILED(hr = allocate_joined_path(
            payload_root, application->current_directory_path,
            &runtime->current_directory )))
        goto done;
    {
        UINT32 blob_size = 0;
        const BYTE *blob =
            appx_package_graph_get_blob( runtime->graph, &blob_size );

        runtime->attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
        runtime->attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
        runtime->attach.size = blob_size;
        runtime->attach.blob = (ULONG_PTR)blob;
        runtime->attach.leases = (ULONG_PTR)runtime->lease_values;
        runtime->attach.lease_count = runtime->lease_count;
    }
    *output = runtime;
    runtime = NULL;
    hr = S_OK;

done:
    appx_deployment_record_free( main_record );
    appx_package_graph_free( resolved_graph );
    HeapFree( GetProcessHeap(), 0, payload_root );
    HeapFree( GetProcessHeap(), 0, classes );
    HeapFree( GetProcessHeap(), 0, loader_files );
    free_runtime_records( records, record_count );
    appx_deployment_runtime_free( runtime );
    close_store( &store );
    return hr;
}

const APPX_PACKAGE_GRAPH *WINAPI appx_deployment_runtime_get_graph(
    const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    return runtime ? runtime->graph : NULL;
}

UINT32 WINAPI appx_deployment_runtime_get_lease_count(
    const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    return runtime ? runtime->lease_count : 0;
}

const HANDLE *WINAPI appx_deployment_runtime_get_leases(
    const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    return runtime ? runtime->leases : NULL;
}

const struct wine_appx_graph_attach *WINAPI
appx_deployment_runtime_get_attach( const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    return runtime ? &runtime->attach : NULL;
}

HANDLE WINAPI appx_deployment_runtime_get_executable_handle(
    const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    return runtime ? runtime->executable : INVALID_HANDLE_VALUE;
}

const WCHAR *WINAPI appx_deployment_runtime_get_executable_path(
    const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    return runtime ? runtime->executable_path : NULL;
}

const WCHAR *WINAPI appx_deployment_runtime_get_parameters(
    const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    return runtime ? runtime->parameters : NULL;
}

const WCHAR *WINAPI appx_deployment_runtime_get_current_directory(
    const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    return runtime ? runtime->current_directory : NULL;
}

static HRESULT build_launch_command_line(
    const APPX_DEPLOYMENT_RUNTIME *runtime, WCHAR **command_line )
{
    UINT32 exe_chars, parameter_chars, total_chars;

    *command_line = NULL;
    if (FAILED(bounded_string_length(
            runtime->executable_path, FALSE, &exe_chars )) ||
        FAILED(bounded_string_length(
            runtime->parameters, TRUE, &parameter_chars )) ||
        !add_uint32( exe_chars, parameter_chars, &total_chars ) ||
        !add_uint32( total_chars, 3, &total_chars ) ||
        !(*command_line = HeapAlloc(
            GetProcessHeap(), 0, total_chars * sizeof(**command_line) )))
        return E_OUTOFMEMORY;
    if (runtime->parameters[0])
        swprintf( *command_line, total_chars, L"\"%s\" %s",
                  runtime->executable_path, runtime->parameters );
    else
        swprintf( *command_line, total_chars, L"\"%s\"",
                  runtime->executable_path );
    return S_OK;
}

HRESULT WINAPI appx_deployment_launch(
    const WCHAR *full_name, const WCHAR *application_id,
    const APPX_DEPLOYMENT_OPTIONS *options, const STARTUPINFOW *startup,
    PROCESS_INFORMATION *process_information )
{
    APPX_DEPLOYMENT_RUNTIME *runtime = NULL;
    STARTUPINFOEXW startup_ex;
    SIZE_T attribute_size = 0;
    WCHAR *command_line = NULL;
    HRESULT hr;
    BOOL ret;

    if (!process_information) return E_INVALIDARG;
    memset( process_information, 0, sizeof(*process_information) );
    if (FAILED(hr = appx_deployment_runtime_acquire(
            full_name, application_id, options, &runtime )) ||
        FAILED(hr = build_launch_command_line( runtime, &command_line )))
        goto done;
    memset( &startup_ex, 0, sizeof(startup_ex) );
    if (startup) startup_ex.StartupInfo = *startup;
    startup_ex.StartupInfo.cb = sizeof(startup_ex);
    InitializeProcThreadAttributeList( NULL, 1, 0, &attribute_size );
    if (!(startup_ex.lpAttributeList = HeapAlloc(
            GetProcessHeap(), 0, attribute_size )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!InitializeProcThreadAttributeList(
            startup_ex.lpAttributeList, 1, 0, &attribute_size ) ||
        !UpdateProcThreadAttribute(
            startup_ex.lpAttributeList, 0,
            WINE_PROC_THREAD_ATTRIBUTE_PACKAGE_GRAPH, &runtime->attach,
            sizeof(runtime->attach), NULL, NULL ))
    {
        hr = win32_error( GetLastError() );
        DeleteProcThreadAttributeList( startup_ex.lpAttributeList );
        HeapFree( GetProcessHeap(), 0, startup_ex.lpAttributeList );
        goto done;
    }
    ret = CreateProcessW(
        runtime->executable_path, command_line, NULL, NULL, FALSE,
        EXTENDED_STARTUPINFO_PRESENT, NULL, runtime->current_directory,
        &startup_ex.StartupInfo, process_information );
    hr = ret ? S_OK : win32_error( GetLastError() );
    DeleteProcThreadAttributeList( startup_ex.lpAttributeList );
    HeapFree( GetProcessHeap(), 0, startup_ex.lpAttributeList );

done:
    HeapFree( GetProcessHeap(), 0, command_line );
    appx_deployment_runtime_free( runtime );
    return hr;
}

static HRESULT deployment_execute_with_backend(
    enum appx_deployment_operation operation, HANDLE package_file,
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *input,
    const APPX_DEPLOYMENT_TEST_BACKEND *test,
    APPX_DEPLOYMENT_RESULT **output )
{
    APPX_DEPLOYMENT_OPTIONS options;
    struct deployment_backend backend;
    HRESULT hr;

    if (!output) return E_INVALIDARG;
    *output = NULL;
    if (FAILED(hr = validate_options( input, &options )) ||
        FAILED(hr = validate_backend( test )))
        return hr;
    backend.test = test;
    backend.accept_weak_durability =
        !!(options.flags & APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY);
    switch (operation)
    {
    case APPX_DEPLOYMENT_OPERATION_INSTALL:
    case APPX_DEPLOYMENT_OPERATION_UPDATE:
        if (!package_file || package_file == INVALID_HANDLE_VALUE)
            return E_INVALIDARG;
        return execute_add( operation, package_file, &options,
                            &backend, output );
    case APPX_DEPLOYMENT_OPERATION_REMOVE:
        if (!full_name) return E_INVALIDARG;
        return execute_remove( full_name, &options, &backend, output );
    case APPX_DEPLOYMENT_OPERATION_RECOVER:
    case APPX_DEPLOYMENT_OPERATION_GARBAGE_COLLECT:
        return execute_recovery_or_gc( operation, &options,
                                       &backend, output );
    }
    return E_INVALIDARG;
}

#ifdef APPX_DEPLOYMENT_TESTING
HRESULT WINAPI appx_deployment_execute_with_test_backend(
    enum appx_deployment_operation operation, HANDLE package_file,
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *input,
    const APPX_DEPLOYMENT_TEST_BACKEND *test,
    APPX_DEPLOYMENT_RESULT **output )
{
    return deployment_execute_with_backend(
        operation, package_file, full_name, input, test, output );
}
#endif

HRESULT WINAPI appx_deployment_install(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    return deployment_execute_with_backend(
        APPX_DEPLOYMENT_OPERATION_INSTALL, package_file, NULL,
        options, NULL, result );
}

HRESULT WINAPI appx_deployment_update(
    HANDLE package_file, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    return deployment_execute_with_backend(
        APPX_DEPLOYMENT_OPERATION_UPDATE, package_file, NULL,
        options, NULL, result );
}

HRESULT WINAPI appx_deployment_remove(
    const WCHAR *full_name, const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    return deployment_execute_with_backend(
        APPX_DEPLOYMENT_OPERATION_REMOVE, INVALID_HANDLE_VALUE,
        full_name, options, NULL, result );
}

HRESULT WINAPI appx_deployment_recover(
    const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    return deployment_execute_with_backend(
        APPX_DEPLOYMENT_OPERATION_RECOVER, INVALID_HANDLE_VALUE,
        NULL, options, NULL, result );
}

HRESULT WINAPI appx_deployment_collect_garbage(
    const APPX_DEPLOYMENT_OPTIONS *options,
    APPX_DEPLOYMENT_RESULT **result )
{
    return deployment_execute_with_backend(
        APPX_DEPLOYMENT_OPERATION_GARBAGE_COLLECT, INVALID_HANDLE_VALUE,
        NULL, options, NULL, result );
}
