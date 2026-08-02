/*
 * Wine AppX/MSIX command-line client
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

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "../../dlls/appxsvc/bundle.h"
#include "../../dlls/appxsvc/deployment.h"

#define DEFAULT_STORE_ROOT L"C:\\Program Files\\WindowsApps\\.wine-msix-store"

#define DEFAULT_MAX_ARCHIVE_BYTES  (32ULL * 1024 * 1024 * 1024)
#define DEFAULT_MAX_EXPANDED_BYTES (16ULL * 1024 * 1024 * 1024)
#define DEFAULT_FREE_SPACE_FLOOR   (2ULL * 1024 * 1024 * 1024)
#define MAX_ARCHIVE_BYTES          (128ULL * 1024 * 1024 * 1024)
#define MAX_EXPANDED_BYTES         (64ULL * 1024 * 1024 * 1024)
#define MAX_ENTRY_BYTES            (16ULL * 1024 * 1024 * 1024)

#define EXIT_OPERATION_FAILED 1
#define EXIT_USAGE_ERROR      2
#define EXIT_API_UNAVAILABLE  3

struct appxsvc_api
{
    HMODULE module;

    HRESULT (WINAPI *package_inspect)(
        HANDLE, const WINE_APPX_ARCHIVE_LIMITS *, UINT32,
        APPX_PACKAGE_INSPECTION ** );
    void (WINAPI *package_inspection_free)( APPX_PACKAGE_INSPECTION * );
    const APPX_MANIFEST *(WINAPI *package_inspection_get_manifest)(
        const APPX_PACKAGE_INSPECTION * );
    UINT32 (WINAPI *package_inspection_get_file_count)(
        const APPX_PACKAGE_INSPECTION * );
    UINT64 (WINAPI *package_inspection_get_expanded_size)(
        const APPX_PACKAGE_INSPECTION * );
    HRESULT (WINAPI *package_inspection_get_content_id)(
        const APPX_PACKAGE_INSPECTION *, BYTE *, UINT32 );
    HRESULT (WINAPI *package_inspection_get_signer_id)(
        const APPX_PACKAGE_INSPECTION *, BYTE *, UINT32 );
    HRESULT (WINAPI *package_extract)(
        const APPX_PACKAGE_INSPECTION *, HANDLE,
        const APPX_EXTRACT_OPTIONS * );

    const struct appx_manifest_identity *(WINAPI *manifest_get_identity)(
        const APPX_MANIFEST * );
    BOOL (WINAPI *manifest_is_supported)( const APPX_MANIFEST * );
    BOOL (WINAPI *manifest_is_framework)( const APPX_MANIFEST * );
    BOOL (WINAPI *manifest_is_resource_package)( const APPX_MANIFEST * );
    BOOL (WINAPI *manifest_has_run_full_trust)( const APPX_MANIFEST * );
    UINT32 (WINAPI *manifest_get_application_count)( const APPX_MANIFEST * );
    const struct appx_manifest_application *(WINAPI
        *manifest_get_application)( const APPX_MANIFEST *, UINT32 );
    UINT32 (WINAPI *manifest_get_dependency_count)( const APPX_MANIFEST * );
    const struct appx_manifest_dependency *(WINAPI
        *manifest_get_dependency)( const APPX_MANIFEST *, UINT32 );
    UINT32 (WINAPI *manifest_get_unsupported_reason_count)(
        const APPX_MANIFEST * );
    enum appx_manifest_unsupported_reason (WINAPI
        *manifest_get_unsupported_reason)( const APPX_MANIFEST *, UINT32 );

    HRESULT (WINAPI *bundle_inspect)(
        HANDLE, const WINE_APPX_ARCHIVE_LIMITS *, UINT32,
        const struct appx_bundle_selection_policy *,
        APPX_BUNDLE_INSPECTION ** );
    void (WINAPI *bundle_inspection_free)( APPX_BUNDLE_INSPECTION * );
    const APPX_BUNDLE_MANIFEST *(WINAPI *bundle_inspection_get_manifest)(
        const APPX_BUNDLE_INSPECTION * );
    const struct appx_bundle_selection *(WINAPI
        *bundle_inspection_get_selection)( const APPX_BUNDLE_INSPECTION * );
    const APPX_PACKAGE_INSPECTION *(WINAPI
        *bundle_inspection_get_selected_package)(
            const APPX_BUNDLE_INSPECTION * );
    HRESULT (WINAPI *bundle_inspection_get_signer_id)(
        const APPX_BUNDLE_INSPECTION *, BYTE *, UINT32 );
    const struct appx_bundle_identity *(WINAPI *bundle_manifest_get_identity)(
        const APPX_BUNDLE_MANIFEST * );

    HRESULT (WINAPI *deployment_initialize)(
        const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT ** );
    HRESULT (WINAPI *deployment_install)(
        HANDLE, const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT ** );
    HRESULT (WINAPI *deployment_update)(
        HANDLE, const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT ** );
    HRESULT (WINAPI *deployment_remove)(
        const WCHAR *, const APPX_DEPLOYMENT_OPTIONS *,
        APPX_DEPLOYMENT_RESULT ** );
    HRESULT (WINAPI *deployment_query)(
        const WCHAR *, const APPX_DEPLOYMENT_OPTIONS *,
        APPX_CATALOG_SNAPSHOT ** );
    HRESULT (WINAPI *deployment_launch)(
        const WCHAR *, const WCHAR *, const APPX_DEPLOYMENT_OPTIONS *,
        const STARTUPINFOW *, PROCESS_INFORMATION * );
    HRESULT (WINAPI *deployment_recover)(
        const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT ** );
    HRESULT (WINAPI *deployment_collect_garbage)(
        const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT ** );
    void (WINAPI *deployment_result_free)( APPX_DEPLOYMENT_RESULT * );
    UINT32 (WINAPI *deployment_result_get_flags)(
        const APPX_DEPLOYMENT_RESULT * );
    UINT64 (WINAPI *deployment_result_get_catalog_epoch)(
        const APPX_DEPLOYMENT_RESULT * );
    UINT64 (WINAPI *deployment_result_get_reclaimed_bytes)(
        const APPX_DEPLOYMENT_RESULT * );
    UINT32 (WINAPI *deployment_result_get_reclaimed_entries)(
        const APPX_DEPLOYMENT_RESULT * );
    HRESULT (WINAPI *deployment_result_get_package_full_name)(
        const APPX_DEPLOYMENT_RESULT *, UINT32 *, WCHAR * );

    void (WINAPI *catalog_snapshot_free)( APPX_CATALOG_SNAPSHOT * );
    UINT64 (WINAPI *catalog_snapshot_get_epoch)(
        const APPX_CATALOG_SNAPSHOT * );
    UINT32 (WINAPI *catalog_snapshot_get_package_count)(
        const APPX_CATALOG_SNAPSHOT * );
    const struct appx_catalog_package *(WINAPI
        *catalog_snapshot_get_package)(
            const APPX_CATALOG_SNAPSHOT *, UINT32 );
};

struct command_options
{
    const WCHAR *store_root;
    enum appx_catalog_architecture architecture;
    UINT32 deployment_flags;
    UINT64 max_archive_bytes;
    UINT64 max_expanded_bytes;
    UINT64 free_space_floor_bytes;
    BOOL wait;
};

struct inspected_source
{
    APPX_PACKAGE_INSPECTION *package;
    APPX_BUNDLE_INSPECTION *bundle;
    HRESULT package_error;
    HRESULT bundle_error;
};

static struct appxsvc_api api;

static void print_usage( FILE *stream )
{
    fwprintf( stream,
        L"Usage: wineappx [OPTIONS] COMMAND [ARGUMENTS]\n"
        L"\n"
        L"Verified package operations:\n"
        L"  inspect PACKAGE                    Verify and describe an MSIX/AppX package or bundle\n"
        L"  unpack PACKAGE DESTINATION         Verify and extract into a new directory\n"
        L"\n"
        L"Deployment store operations:\n"
        L"  initialize                         Create or validate the deployment store\n"
        L"  install PACKAGE                    Install a new package family generation\n"
        L"  update PACKAGE                     Update an installed package family\n"
        L"  remove PACKAGE_FULL_NAME           Remove one exact installed package\n"
        L"  query PACKAGE_FULL_NAME            Query one exact installed package\n"
        L"  list                               List all installed packages\n"
        L"  launch PACKAGE_FULL_NAME APP_ID    Launch one full-trust package application\n"
        L"  recover                            Recover interrupted transactions\n"
        L"  gc                                 Collect unpublished package generations\n"
        L"\n"
        L"Options (must precede COMMAND):\n"
        L"  --store PATH                       Drive-absolute deployment store path\n"
        L"  --arch ARCH                        neutral, x86, x64, arm, arm64, or x86a64\n"
        L"  --allow-downgrade                  Allow a lower-version update\n"
        L"  --accept-weak-durability           Accept hosts without directory flush support\n"
        L"  --max-archive BYTES                Input cap (default 32G, maximum 128G)\n"
        L"  --max-expanded BYTES               Expanded cap (default 16G, maximum 64G)\n"
        L"  --free-space-floor BYTES           Bytes to keep free (default 2G)\n"
        L"  --wait                             Wait for a launched process and return its status\n"
        L"  -h, --help                         Show this help\n"
        L"\n"
        L"Byte limits accept an optional K, M, G, or T binary suffix.\n"
        L"\n"
        L"inspect/unpack accept signed MSIX/AppX packages and bundles. They\n"
        L"verify signatures, signed digests, block maps, hashes, CRCs, "
        L"manifests,\n"
        L"and layout before extraction. Bundle selection rejects resource,\n"
        L"optional, encrypted, unsupported-type/architecture, and "
        L"unsupported-\n"
        L"qualifier payloads, plus missing or ambiguous application payloads.\n"
        L"install/update accept signed packages and supported app-only "
        L"bundles.\n"
        L"\n"
        L"Successful stdout is stable key=value data. Indexed keys are "
        L"zero-based;\n"
        L"digests are lowercase hex; string backslash/control characters "
        L"use\n"
        L"\\\\, \\t, \\r, \\n, or \\uNNNN escapes. Diagnostics go to stderr.\n"
        L"Exit codes: 0 success, 1 operation/verification failure, 2 usage "
        L"error,\n"
        L"3 private-API mismatch. launch --wait instead returns the child "
        L"status.\n" );
}

static enum appx_catalog_architecture native_architecture(void)
{
    SYSTEM_INFO info;

    GetNativeSystemInfo( &info );
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_INTEL:
        return APPX_CATALOG_ARCHITECTURE_X86;
    case PROCESSOR_ARCHITECTURE_AMD64:
        return APPX_CATALOG_ARCHITECTURE_X64;
    case PROCESSOR_ARCHITECTURE_ARM:
        return APPX_CATALOG_ARCHITECTURE_ARM;
    case PROCESSOR_ARCHITECTURE_ARM64:
        return APPX_CATALOG_ARCHITECTURE_ARM64;
    default:
        return APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    }
}

static const WCHAR *option_inline_value( const WCHAR *argument,
                                         const WCHAR *name )
{
    SIZE_T length = lstrlenW( name );

    if (wcsncmp( argument, name, length ) || argument[length] != '=')
        return NULL;
    return argument + length + 1;
}

static BOOL parse_size( const WCHAR *string, UINT64 *value )
{
    const WCHAR *cursor = string;
    UINT64 number = 0, multiplier = 1;

    if (!cursor || *cursor < '0' || *cursor > '9') return FALSE;
    while (*cursor >= '0' && *cursor <= '9')
    {
        UINT32 digit = *cursor++ - '0';

        if (number > (UINT64_MAX - digit) / 10) return FALSE;
        number = number * 10 + digit;
    }
    if (*cursor)
    {
        switch (*cursor++)
        {
        case 'k':
        case 'K':
            multiplier = 1024ULL;
            break;
        case 'm':
        case 'M':
            multiplier = 1024ULL * 1024;
            break;
        case 'g':
        case 'G':
            multiplier = 1024ULL * 1024 * 1024;
            break;
        case 't':
        case 'T':
            multiplier = 1024ULL * 1024 * 1024 * 1024;
            break;
        default:
            return FALSE;
        }
        if (*cursor == 'i' || *cursor == 'I') cursor++;
        if (*cursor == 'b' || *cursor == 'B') cursor++;
        if (*cursor) return FALSE;
    }
    if (!number || number > UINT64_MAX / multiplier) return FALSE;
    *value = number * multiplier;
    return TRUE;
}

static BOOL parse_architecture( const WCHAR *value,
                                enum appx_catalog_architecture *architecture )
{
    if (!lstrcmpiW( value, L"neutral" ))
        *architecture = APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    else if (!lstrcmpiW( value, L"x86" ))
        *architecture = APPX_CATALOG_ARCHITECTURE_X86;
    else if (!lstrcmpiW( value, L"x64" ))
        *architecture = APPX_CATALOG_ARCHITECTURE_X64;
    else if (!lstrcmpiW( value, L"arm" ))
        *architecture = APPX_CATALOG_ARCHITECTURE_ARM;
    else if (!lstrcmpiW( value, L"arm64" ))
        *architecture = APPX_CATALOG_ARCHITECTURE_ARM64;
    else if (!lstrcmpiW( value, L"x86a64" ))
        *architecture = APPX_CATALOG_ARCHITECTURE_X86A64;
    else
        return FALSE;
    return TRUE;
}

static BOOL take_option_value( int argc, WCHAR **argv, int *index,
                               const WCHAR *name, const WCHAR **value )
{
    const WCHAR *inline_value;

    if (!lstrcmpW( argv[*index], name ))
    {
        if (++*index >= argc) return FALSE;
        *value = argv[*index];
        return **value != 0;
    }
    if ((inline_value = option_inline_value( argv[*index], name )) &&
        *inline_value)
    {
        *value = inline_value;
        return TRUE;
    }
    return FALSE;
}

/*
 * Return the command index, zero for --help, or -1 for a usage error.
 * Keeping all options before the command makes package names beginning with
 * '-' unambiguous when the caller uses "--".
 */
static int parse_options( int argc, WCHAR **argv,
                          struct command_options *options )
{
    int i;

    memset( options, 0, sizeof(*options) );
    options->store_root = DEFAULT_STORE_ROOT;
    options->architecture = native_architecture();
    options->max_archive_bytes = DEFAULT_MAX_ARCHIVE_BYTES;
    options->max_expanded_bytes = DEFAULT_MAX_EXPANDED_BYTES;
    options->free_space_floor_bytes = DEFAULT_FREE_SPACE_FLOOR;

    for (i = 1; i < argc; i++)
    {
        const WCHAR *value = NULL;
        UINT64 size;

        if (!lstrcmpW( argv[i], L"--" ))
            return ++i < argc ? i : -1;
        if (argv[i][0] != '-') return i;
        if (!lstrcmpW( argv[i], L"-h" ) ||
            !lstrcmpW( argv[i], L"--help" ))
            return 0;
        if (!lstrcmpW( argv[i], L"--allow-downgrade" ))
        {
            options->deployment_flags |= APPX_DEPLOYMENT_ALLOW_DOWNGRADE;
            continue;
        }
        if (!lstrcmpW( argv[i], L"--accept-weak-durability" ))
        {
            options->deployment_flags |=
                APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY;
            continue;
        }
        if (!lstrcmpW( argv[i], L"--wait" ))
        {
            options->wait = TRUE;
            continue;
        }
        if (!wcsncmp( argv[i], L"--store", 7 ))
        {
            if (!take_option_value(
                    argc, argv, &i, L"--store", &value ))
                return -1;
            options->store_root = value;
            continue;
        }
        if (!wcsncmp( argv[i], L"--arch", 6 ))
        {
            if (!take_option_value(
                    argc, argv, &i, L"--arch", &value ) ||
                !parse_architecture( value, &options->architecture ))
                return -1;
            continue;
        }
        if (!wcsncmp( argv[i], L"--max-archive", 13 ))
        {
            if (!take_option_value(
                    argc, argv, &i, L"--max-archive", &value ) ||
                !parse_size( value, &size ) || size > MAX_ARCHIVE_BYTES)
                return -1;
            options->max_archive_bytes = size;
            continue;
        }
        if (!wcsncmp( argv[i], L"--max-expanded", 14 ))
        {
            if (!take_option_value(
                    argc, argv, &i, L"--max-expanded", &value ) ||
                !parse_size( value, &size ) || size > MAX_EXPANDED_BYTES)
                return -1;
            options->max_expanded_bytes = size;
            continue;
        }
        if (!wcsncmp( argv[i], L"--free-space-floor", 18 ))
        {
            if (!take_option_value(
                    argc, argv, &i, L"--free-space-floor", &value ) ||
                !parse_size( value, &size ))
                return -1;
            options->free_space_floor_bytes = size;
            continue;
        }
        return -1;
    }
    return -1;
}

static HRESULT load_appxsvc(void)
{
    FARPROC proc;

    if (!(api.module = LoadLibraryExW(
            L"appxsvc.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 )))
        return HRESULT_FROM_WIN32( GetLastError() );

#define LOAD_API(member, name)                                             \
    do                                                                     \
    {                                                                      \
        if (!(proc = GetProcAddress( api.module, #name )))                 \
        {                                                                  \
            fwprintf( stderr, L"wineappx: appxsvc.dll is missing %hs.\n",   \
                      #name );                                              \
            return HRESULT_FROM_WIN32( ERROR_PROC_NOT_FOUND );             \
        }                                                                  \
        memcpy( &api.member, &proc, sizeof(proc) );                         \
    } while (0)

    LOAD_API( package_inspect, appx_package_inspect );
    LOAD_API( package_inspection_free, appx_package_inspection_free );
    LOAD_API( package_inspection_get_manifest,
              appx_package_inspection_get_manifest );
    LOAD_API( package_inspection_get_file_count,
              appx_package_inspection_get_file_count );
    LOAD_API( package_inspection_get_expanded_size,
              appx_package_inspection_get_expanded_size );
    LOAD_API( package_inspection_get_content_id,
              appx_package_inspection_get_content_id );
    LOAD_API( package_inspection_get_signer_id,
              appx_package_inspection_get_signer_id );
    LOAD_API( package_extract, appx_package_extract );

    LOAD_API( manifest_get_identity, appx_manifest_get_identity );
    LOAD_API( manifest_is_supported, appx_manifest_is_supported );
    LOAD_API( manifest_is_framework, appx_manifest_is_framework );
    LOAD_API( manifest_is_resource_package,
              appx_manifest_is_resource_package );
    LOAD_API( manifest_has_run_full_trust,
              appx_manifest_has_run_full_trust );
    LOAD_API( manifest_get_application_count,
              appx_manifest_get_application_count );
    LOAD_API( manifest_get_application, appx_manifest_get_application );
    LOAD_API( manifest_get_dependency_count,
              appx_manifest_get_dependency_count );
    LOAD_API( manifest_get_dependency, appx_manifest_get_dependency );
    LOAD_API( manifest_get_unsupported_reason_count,
              appx_manifest_get_unsupported_reason_count );
    LOAD_API( manifest_get_unsupported_reason,
              appx_manifest_get_unsupported_reason );

    LOAD_API( bundle_inspect, appx_bundle_inspect );
    LOAD_API( bundle_inspection_free, appx_bundle_inspection_free );
    LOAD_API( bundle_inspection_get_manifest,
              appx_bundle_inspection_get_manifest );
    LOAD_API( bundle_inspection_get_selection,
              appx_bundle_inspection_get_selection );
    LOAD_API( bundle_inspection_get_selected_package,
              appx_bundle_inspection_get_selected_package );
    LOAD_API( bundle_inspection_get_signer_id,
              appx_bundle_inspection_get_signer_id );
    LOAD_API( bundle_manifest_get_identity,
              appx_bundle_manifest_get_identity );

    LOAD_API( deployment_initialize, appx_deployment_initialize );
    LOAD_API( deployment_install, appx_deployment_install );
    LOAD_API( deployment_update, appx_deployment_update );
    LOAD_API( deployment_remove, appx_deployment_remove );
    LOAD_API( deployment_query, appx_deployment_query );
    LOAD_API( deployment_launch, appx_deployment_launch );
    LOAD_API( deployment_recover, appx_deployment_recover );
    LOAD_API( deployment_collect_garbage,
              appx_deployment_collect_garbage );
    LOAD_API( deployment_result_free, appx_deployment_result_free );
    LOAD_API( deployment_result_get_flags,
              appx_deployment_result_get_flags );
    LOAD_API( deployment_result_get_catalog_epoch,
              appx_deployment_result_get_catalog_epoch );
    LOAD_API( deployment_result_get_reclaimed_bytes,
              appx_deployment_result_get_reclaimed_bytes );
    LOAD_API( deployment_result_get_reclaimed_entries,
              appx_deployment_result_get_reclaimed_entries );
    LOAD_API( deployment_result_get_package_full_name,
              appx_deployment_result_get_package_full_name );

    LOAD_API( catalog_snapshot_free, appx_catalog_snapshot_free );
    LOAD_API( catalog_snapshot_get_epoch, appx_catalog_snapshot_get_epoch );
    LOAD_API( catalog_snapshot_get_package_count,
              appx_catalog_snapshot_get_package_count );
    LOAD_API( catalog_snapshot_get_package,
              appx_catalog_snapshot_get_package );
#undef LOAD_API
    return S_OK;
}

static void trim_message( WCHAR *message )
{
    SIZE_T length = lstrlenW( message );

    while (length && (message[length - 1] == '\r' ||
                      message[length - 1] == '\n' ||
                      message[length - 1] == ' '))
        message[--length] = 0;
}

static void report_failure( const WCHAR *operation, HRESULT hr )
{
    WCHAR message[512];
    DWORD length;

    length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, hr, 0, message, ARRAY_SIZE(message), NULL );
    if (!length && HRESULT_FACILITY( hr ) == FACILITY_WIN32)
        length = FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, HRESULT_CODE( hr ), 0, message, ARRAY_SIZE(message), NULL );
    if (length)
    {
        trim_message( message );
        fwprintf( stderr, L"wineappx: %s failed (0x%08lx): %s\n",
                  operation, (unsigned long)hr, message );
    }
    else
        fwprintf( stderr, L"wineappx: %s failed (0x%08lx).\n",
                  operation, (unsigned long)hr );
}

static UINT64 min_uint64( UINT64 left, UINT64 right )
{
    return left < right ? left : right;
}

static void initialize_archive_limits(
    const struct command_options *command,
    WINE_APPX_ARCHIVE_LIMITS *limits )
{
    memset( limits, 0, sizeof(*limits) );
    limits->size = sizeof(*limits);
    limits->max_entries = 65536;
    limits->max_archive_size = command->max_archive_bytes;
    limits->max_central_directory_size =
        min_uint64( command->max_archive_bytes, 64ULL * 1024 * 1024 );
    limits->max_entry_compressed_size =
        min_uint64( command->max_archive_bytes, MAX_ENTRY_BYTES );
    limits->max_entry_uncompressed_size =
        min_uint64( command->max_expanded_bytes, MAX_ENTRY_BYTES );
    limits->max_total_uncompressed_size = command->max_expanded_bytes;
    limits->max_compression_ratio = 1000;
    limits->compression_ratio_slack = 1024 * 1024;
}

static void initialize_deployment_options(
    const struct command_options *command,
    WINE_APPX_ARCHIVE_LIMITS *limits,
    APPX_DEPLOYMENT_OPTIONS *options )
{
    initialize_archive_limits( command, limits );
    memset( options, 0, sizeof(*options) );
    options->size = sizeof(*options);
    options->version = APPX_DEPLOYMENT_OPTIONS_VERSION;
    options->store_root = command->store_root;
    options->flags = command->deployment_flags;
    options->target_architecture = command->architecture;
    options->package_quota_bytes = command->max_expanded_bytes;
    options->free_space_floor_bytes = command->free_space_floor_bytes;
    options->archive_limits = limits;
}

static HRESULT open_input_file( const WCHAR *path, HANDLE *file )
{
    FILE_ATTRIBUTE_TAG_INFO tag;
    DWORD attributes;

    *file = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
        FILE_FLAG_SEQUENTIAL_SCAN, NULL );
    if (*file == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32( GetLastError() );
    if (GetFileType( *file ) != FILE_TYPE_DISK)
    {
        HRESULT hr = HRESULT_FROM_WIN32(
            GetLastError() ? GetLastError() : ERROR_INVALID_HANDLE );

        CloseHandle( *file );
        *file = INVALID_HANDLE_VALUE;
        return hr;
    }
    if (GetFileInformationByHandleEx(
            *file, FileAttributeTagInfo, &tag, sizeof(tag) ))
        attributes = tag.FileAttributes;
    else
    {
        BY_HANDLE_FILE_INFORMATION info;

        if (!GetFileInformationByHandle( *file, &info ))
        {
            HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );

            CloseHandle( *file );
            *file = INVALID_HANDLE_VALUE;
            return hr;
        }
        attributes = info.dwFileAttributes;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        CloseHandle( *file );
        *file = INVALID_HANDLE_VALUE;
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    }
    return S_OK;
}

static enum appx_bundle_architecture bundle_architecture(
    enum appx_catalog_architecture architecture )
{
    switch (architecture)
    {
    case APPX_CATALOG_ARCHITECTURE_X86:
        return APPX_BUNDLE_ARCHITECTURE_X86;
    case APPX_CATALOG_ARCHITECTURE_X64:
        return APPX_BUNDLE_ARCHITECTURE_X64;
    case APPX_CATALOG_ARCHITECTURE_ARM:
        return APPX_BUNDLE_ARCHITECTURE_ARM;
    case APPX_CATALOG_ARCHITECTURE_ARM64:
        return APPX_BUNDLE_ARCHITECTURE_ARM64;
    case APPX_CATALOG_ARCHITECTURE_X86A64:
        return APPX_BUNDLE_ARCHITECTURE_X86A64;
    default:
        return APPX_BUNDLE_ARCHITECTURE_NEUTRAL;
    }
}

static HRESULT inspect_source(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits,
    enum appx_catalog_architecture architecture,
    struct inspected_source *source )
{
    struct appx_bundle_selection_policy policy;
    HRESULT hr;

    memset( source, 0, sizeof(*source) );
    source->package_error = api.package_inspect(
        file, limits, 0, &source->package );
    if (SUCCEEDED(source->package_error) && source->package) return S_OK;
    if (SUCCEEDED(source->package_error))
        source->package_error = E_UNEXPECTED;
    api.package_inspection_free( source->package );
    source->package = NULL;

    memset( &policy, 0, sizeof(policy) );
    policy.size = sizeof(policy);
    policy.host_architecture = bundle_architecture( architecture );
    policy.language_neutral_only = TRUE;
    policy.scale_neutral_only = TRUE;
    source->bundle_error = api.bundle_inspect(
        file, limits, 0, &policy, &source->bundle );
    if (SUCCEEDED(source->bundle_error))
    {
        source->package = (APPX_PACKAGE_INSPECTION *)
            api.bundle_inspection_get_selected_package( source->bundle );
        if (!source->package)
        {
            api.bundle_inspection_free( source->bundle );
            source->bundle = NULL;
            return E_UNEXPECTED;
        }
        return S_OK;
    }

    api.bundle_inspection_free( source->bundle );
    source->bundle = NULL;
    hr = source->bundle_error;
    report_failure( L"package verification", source->package_error );
    report_failure( L"bundle verification", source->bundle_error );
    return hr;
}

static void free_inspected_source( struct inspected_source *source )
{
    if (source->bundle)
        api.bundle_inspection_free( source->bundle );
    else if (source->package)
        api.package_inspection_free( source->package );
    memset( source, 0, sizeof(*source) );
}

static const WCHAR *manifest_architecture_name(
    enum appx_manifest_architecture architecture )
{
    switch (architecture)
    {
    case APPX_MANIFEST_ARCHITECTURE_NEUTRAL:
        return L"neutral";
    case APPX_MANIFEST_ARCHITECTURE_X86:
        return L"x86";
    case APPX_MANIFEST_ARCHITECTURE_X64:
        return L"x64";
    case APPX_MANIFEST_ARCHITECTURE_ARM:
        return L"arm";
    case APPX_MANIFEST_ARCHITECTURE_ARM64:
        return L"arm64";
    case APPX_MANIFEST_ARCHITECTURE_X86A64:
        return L"x86a64";
    default:
        return L"unknown";
    }
}

static const WCHAR *catalog_architecture_name(
    enum appx_catalog_architecture architecture )
{
    switch (architecture)
    {
    case APPX_CATALOG_ARCHITECTURE_NEUTRAL:
        return L"neutral";
    case APPX_CATALOG_ARCHITECTURE_X86:
        return L"x86";
    case APPX_CATALOG_ARCHITECTURE_X64:
        return L"x64";
    case APPX_CATALOG_ARCHITECTURE_ARM:
        return L"arm";
    case APPX_CATALOG_ARCHITECTURE_ARM64:
        return L"arm64";
    case APPX_CATALOG_ARCHITECTURE_X86A64:
        return L"x86a64";
    default:
        return L"unknown";
    }
}

static const WCHAR *manifest_activation_name(
    enum appx_manifest_activation_kind activation )
{
    switch (activation)
    {
    case APPX_MANIFEST_ACTIVATION_FULL_TRUST:
        return L"full-trust";
    case APPX_MANIFEST_ACTIVATION_PACKAGED_CLASSIC:
        return L"packaged-classic";
    case APPX_MANIFEST_ACTIVATION_WIN32:
        return L"win32";
    default:
        return L"unsupported";
    }
}

static const WCHAR *catalog_activation_name(
    enum appx_catalog_activation_kind activation )
{
    switch (activation)
    {
    case APPX_CATALOG_ACTIVATION_FULL_TRUST:
        return L"full-trust";
    case APPX_CATALOG_ACTIVATION_PACKAGED_CLASSIC:
        return L"packaged-classic";
    case APPX_CATALOG_ACTIVATION_WIN32:
        return L"win32";
    default:
        return L"unsupported";
    }
}

static const WCHAR *unsupported_reason_name(
    enum appx_manifest_unsupported_reason reason )
{
    switch (reason)
    {
    case APPX_MANIFEST_UNSUPPORTED_UNKNOWN_NAMESPACE:
        return L"unknown-namespace";
    case APPX_MANIFEST_UNSUPPORTED_IGNORABLE_CONTENT:
        return L"unsupported-ignorable-content";
    case APPX_MANIFEST_UNSUPPORTED_UWP_APPLICATION:
        return L"uwp-application";
    case APPX_MANIFEST_UNSUPPORTED_APPCONTAINER:
        return L"app-container";
    case APPX_MANIFEST_UNSUPPORTED_RUNTIME_BEHAVIOR:
        return L"runtime-behavior";
    case APPX_MANIFEST_UNSUPPORTED_OUT_OF_PROCESS_SERVER:
        return L"out-of-process-server";
    case APPX_MANIFEST_UNSUPPORTED_OPTIONAL_DEPENDENCY:
        return L"optional-dependency";
    case APPX_MANIFEST_UNSUPPORTED_APPLICATION_PARAMETERS:
        return L"application-parameters";
    case APPX_MANIFEST_UNSUPPORTED_CURRENT_DIRECTORY:
        return L"current-directory";
    case APPX_MANIFEST_UNSUPPORTED_RESOURCE_PACKAGE:
        return L"resource-package";
    case APPX_MANIFEST_UNSUPPORTED_EXTENSION:
        return L"extension";
    case APPX_MANIFEST_UNSUPPORTED_MISSING_RUN_FULL_TRUST:
        return L"missing-run-full-trust";
    case APPX_MANIFEST_UNSUPPORTED_TARGET_DEVICE_FAMILY:
        return L"target-device-family";
    case APPX_MANIFEST_UNSUPPORTED_UNSIGNED_DEPENDENCY:
        return L"unsigned-dependency";
    default:
        return L"unknown";
    }
}

static const WCHAR *safe_string( const WCHAR *string )
{
    return string ? string : L"";
}

static void print_string_record( const WCHAR *key, const WCHAR *value )
{
    const WCHAR *cursor = safe_string( value );

    wprintf( L"%s=", key );
    while (*cursor)
    {
        WCHAR ch = *cursor++;

        switch (ch)
        {
        case '\\':
            wprintf( L"\\\\" );
            break;
        case '\t':
            wprintf( L"\\t" );
            break;
        case '\r':
            wprintf( L"\\r" );
            break;
        case '\n':
            wprintf( L"\\n" );
            break;
        default:
            if (ch < 0x20 || ch == 0x7f)
                wprintf( L"\\u%04x", ch );
            else
                putwchar( ch );
            break;
        }
    }
    putwchar( '\n' );
}

static void print_digest( const WCHAR *label, const BYTE *digest, UINT32 size )
{
    UINT32 i;

    wprintf( L"%s=", label );
    for (i = 0; i < size; i++) wprintf( L"%02x", digest[i] );
    wprintf( L"\n" );
}

static void print_manifest( const APPX_PACKAGE_INSPECTION *inspection )
{
    const struct appx_manifest_identity *identity;
    const APPX_MANIFEST *manifest;
    BYTE content_id[APPX_PACKAGE_CONTENT_ID_SIZE];
    BYTE signer_id[APPX_PACKAGE_SIGNER_ID_SIZE];
    WCHAR key[128];
    UINT32 i, count;

    manifest = api.package_inspection_get_manifest( inspection );
    identity = manifest ? api.manifest_get_identity( manifest ) : NULL;
    if (!identity)
    {
        wprintf( L"package_manifest=missing\n" );
        return;
    }

    print_string_record( L"package_full_name", identity->full_name );
    print_string_record( L"package_family_name", identity->family_name );
    print_string_record( L"name", identity->name );
    print_string_record( L"publisher", identity->publisher );
    print_string_record( L"resource_id", identity->resource_id );
    wprintf( L"version=%u.%u.%u.%u\n",
             identity->version.major, identity->version.minor,
             identity->version.build, identity->version.revision );
    wprintf( L"architecture=%s\n",
             manifest_architecture_name( identity->architecture ) );
    wprintf( L"supported=%s\n",
             api.manifest_is_supported( manifest ) ? L"yes" : L"no" );
    wprintf( L"framework=%s\n",
             api.manifest_is_framework( manifest ) ? L"yes" : L"no" );
    wprintf( L"resource_package=%s\n",
             api.manifest_is_resource_package( manifest ) ? L"yes" : L"no" );
    wprintf( L"run_full_trust=%s\n",
             api.manifest_has_run_full_trust( manifest ) ? L"yes" : L"no" );
    wprintf( L"file_count=%u\n",
             api.package_inspection_get_file_count( inspection ) );
    wprintf( L"expanded_bytes=%llu\n",
             (unsigned long long)
             api.package_inspection_get_expanded_size( inspection ) );

    if (SUCCEEDED(api.package_inspection_get_content_id(
            inspection, content_id, sizeof(content_id) )))
        print_digest( L"content_id", content_id, sizeof(content_id) );
    if (SUCCEEDED(api.package_inspection_get_signer_id(
            inspection, signer_id, sizeof(signer_id) )))
        print_digest( L"signer_id", signer_id, sizeof(signer_id) );

    count = api.manifest_get_application_count( manifest );
    wprintf( L"application_count=%u\n", count );
    for (i = 0; i < count; i++)
    {
        const struct appx_manifest_application *application =
            api.manifest_get_application( manifest, i );

        if (!application) continue;
        swprintf( key, ARRAY_SIZE(key), L"application[%u].id", i );
        print_string_record( key, application->id );
        swprintf( key, ARRAY_SIZE(key), L"application[%u].executable", i );
        print_string_record( key, application->executable );
        wprintf( L"application[%u].activation=%s\n", i,
                 manifest_activation_name( application->activation_kind ) );
    }

    count = api.manifest_get_dependency_count( manifest );
    wprintf( L"dependency_count=%u\n", count );
    for (i = 0; i < count; i++)
    {
        const struct appx_manifest_dependency *dependency =
            api.manifest_get_dependency( manifest, i );

        if (!dependency) continue;
        swprintf( key, ARRAY_SIZE(key), L"dependency[%u].name", i );
        print_string_record( key, dependency->name );
        wprintf( L"dependency[%u].min_version=%u.%u.%u.%u\n", i,
                 dependency->min_version.major, dependency->min_version.minor,
                 dependency->min_version.build,
                 dependency->min_version.revision );
    }

    count = api.manifest_get_unsupported_reason_count( manifest );
    wprintf( L"unsupported_reason_count=%u\n", count );
    for (i = 0; i < count; i++)
        wprintf( L"unsupported_reason[%u]=%s\n", i,
                 unsupported_reason_name(
                     api.manifest_get_unsupported_reason( manifest, i ) ) );
}

static int command_inspect( const WCHAR *path,
                            const struct command_options *command )
{
    WINE_APPX_ARCHIVE_LIMITS limits;
    struct inspected_source source;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    initialize_archive_limits( command, &limits );
    if (FAILED(hr = open_input_file( path, &file )))
    {
        report_failure( L"opening input package", hr );
        return EXIT_OPERATION_FAILED;
    }
    hr = inspect_source( file, &limits, command->architecture, &source );
    CloseHandle( file );
    if (FAILED(hr)) return EXIT_OPERATION_FAILED;

    if (source.bundle)
    {
        const APPX_BUNDLE_MANIFEST *manifest =
            api.bundle_inspection_get_manifest( source.bundle );
        const struct appx_bundle_identity *identity =
            manifest ? api.bundle_manifest_get_identity( manifest ) : NULL;
        const struct appx_bundle_selection *selection =
            api.bundle_inspection_get_selection( source.bundle );
        BYTE signer_id[APPX_BUNDLE_SIGNER_ID_SIZE];

        wprintf( L"container=bundle\n" );
        if (identity)
        {
            print_string_record( L"bundle_name", identity->name );
            print_string_record( L"bundle_publisher", identity->publisher );
            wprintf( L"bundle_version=%u.%u.%u.%u\n",
                     identity->version.major, identity->version.minor,
                     identity->version.build, identity->version.revision );
        }
        if (selection)
        {
            wprintf( L"bundle_selected_index=%u\n",
                     selection->package_index );
            if (selection->payload)
                print_string_record( L"bundle_selected_file",
                                     selection->payload->file_name );
            wprintf( L"bundle_selection_issues=0x%08x\n",
                     selection->issues );
        }
        if (SUCCEEDED(api.bundle_inspection_get_signer_id(
                source.bundle, signer_id, sizeof(signer_id) )))
            print_digest(
                L"bundle_signer_id", signer_id, sizeof(signer_id) );
    }
    else
        wprintf( L"container=package\n" );
    print_manifest( source.package );
    free_inspected_source( &source );
    return 0;
}

static HRESULT canonical_destination( const WCHAR *input, WCHAR **output )
{
    WCHAR *path;
    DWORD length, i;

    *output = NULL;
    length = GetFullPathNameW( input, 0, NULL, NULL );
    if (!length || length > WINE_APPX_MAX_PATH_CHARS)
        return HRESULT_FROM_WIN32(
            GetLastError() ? GetLastError() : ERROR_FILENAME_EXCED_RANGE );
    if (!(path = HeapAlloc(
            GetProcessHeap(), 0, (length + 1) * sizeof(*path) )))
        return E_OUTOFMEMORY;
    if (!GetFullPathNameW( input, length + 1, path, NULL ))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );

        HeapFree( GetProcessHeap(), 0, path );
        return hr;
    }
    for (i = 0; path[i]; i++)
        if (path[i] == '/') path[i] = '\\';
    length = lstrlenW( path );
    if (length <= 3 || path[1] != ':' || path[2] != '\\' ||
        path[length - 1] == '\\')
    {
        HeapFree( GetProcessHeap(), 0, path );
        return E_INVALIDARG;
    }
    *output = path;
    return S_OK;
}

static HRESULT validate_directory_handle( HANDLE directory )
{
    FILE_ATTRIBUTE_TAG_INFO tag;
    DWORD attributes;

    if (GetFileInformationByHandleEx(
            directory, FileAttributeTagInfo, &tag, sizeof(tag) ))
        attributes = tag.FileAttributes;
    else
    {
        BY_HANDLE_FILE_INFORMATION info;

        if (!GetFileInformationByHandle( directory, &info ))
            return HRESULT_FROM_WIN32( GetLastError() );
        attributes = info.dwFileAttributes;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return HRESULT_FROM_WIN32( ERROR_DIRECTORY );
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    return S_OK;
}

static HRESULT open_held_parent( const WCHAR *path, HANDLE *handle )
{
    HRESULT hr;

    *handle = CreateFileW(
        path, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL );
    if (*handle == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32( GetLastError() );
    if (FAILED(hr = validate_directory_handle( *handle )))
    {
        CloseHandle( *handle );
        *handle = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static HRESULT create_private_destination(
    const WCHAR *input, WCHAR **output_path, HANDLE *output_handle )
{
    HANDLE *parents = NULL;
    WCHAR *path = NULL, saved;
    UINT32 parent_count = 0, capacity, i;
    SIZE_T length;
    HRESULT hr;

    *output_path = NULL;
    *output_handle = INVALID_HANDLE_VALUE;
    if (FAILED(hr = canonical_destination( input, &path ))) return hr;
    length = lstrlenW( path );
    capacity = (UINT32)(length / 2 + 2);
    if (!(parents = HeapAlloc(
            GetProcessHeap(), 0, capacity * sizeof(*parents) )))
    {
        HeapFree( GetProcessHeap(), 0, path );
        return E_OUTOFMEMORY;
    }

    saved = path[3];
    path[3] = 0;
    hr = open_held_parent( path, parents + parent_count );
    path[3] = saved;
    if (FAILED(hr)) goto done;
    parent_count++;

    for (i = 3; path[i]; i++)
    {
        if (path[i] != '\\') continue;
        saved = path[i];
        path[i] = 0;
        hr = open_held_parent( path, parents + parent_count );
        path[i] = saved;
        if (FAILED(hr)) goto done;
        parent_count++;
    }

    if (!CreateDirectoryW( path, NULL ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    *output_handle = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL );
    if (*output_handle == INVALID_HANDLE_VALUE)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        if (!RemoveDirectoryW( path ))
            fwprintf( stderr,
                L"wineappx: the newly created destination %s could not be "
                L"removed after an open failure.\n", path );
        goto done;
    }
    if (FAILED(hr = validate_directory_handle( *output_handle )))
    {
        CloseHandle( *output_handle );
        *output_handle = INVALID_HANDLE_VALUE;
        if (!RemoveDirectoryW( path ))
            fwprintf( stderr,
                L"wineappx: the newly created destination %s could not be "
                L"removed after validation failed.\n", path );
        goto done;
    }
    *output_path = path;
    path = NULL;
    hr = S_OK;

done:
    for (i = 0; i < parent_count; i++) CloseHandle( parents[i] );
    HeapFree( GetProcessHeap(), 0, parents );
    HeapFree( GetProcessHeap(), 0, path );
    return hr;
}

static int command_unpack( const WCHAR *package_path,
                           const WCHAR *destination,
                           const struct command_options *command )
{
    WINE_APPX_ARCHIVE_LIMITS limits;
    APPX_EXTRACT_OPTIONS extract_options;
    struct inspected_source source;
    HANDLE file = INVALID_HANDLE_VALUE, directory = INVALID_HANDLE_VALUE;
    WCHAR *destination_path = NULL;
    HRESULT hr;

    initialize_archive_limits( command, &limits );
    if (FAILED(hr = open_input_file( package_path, &file )))
    {
        report_failure( L"opening input package", hr );
        return EXIT_OPERATION_FAILED;
    }
    hr = inspect_source( file, &limits, command->architecture, &source );
    CloseHandle( file );
    if (FAILED(hr)) return EXIT_OPERATION_FAILED;
    if (FAILED(hr = create_private_destination(
            destination, &destination_path, &directory )))
    {
        report_failure( L"creating extraction destination", hr );
        free_inspected_source( &source );
        return EXIT_OPERATION_FAILED;
    }

    memset( &extract_options, 0, sizeof(extract_options) );
    extract_options.size = sizeof(extract_options);
    extract_options.max_expanded_bytes = command->max_expanded_bytes;
    extract_options.free_space_floor_bytes =
        command->free_space_floor_bytes;
    hr = api.package_extract(
        source.package, directory, &extract_options );
    CloseHandle( directory );
    free_inspected_source( &source );
    if (FAILED(hr))
    {
        report_failure( L"verified extraction", hr );
        if (!RemoveDirectoryW( destination_path ))
            fwprintf( stderr,
                L"wineappx: partial extraction remains at %s; discard that "
                L"new directory before retrying.\n", destination_path );
        HeapFree( GetProcessHeap(), 0, destination_path );
        return EXIT_OPERATION_FAILED;
    }
    print_string_record( L"extracted", destination_path );
    wprintf( L"durability=%s\n",
             hr == APPX_EXTRACT_S_WEAK_DURABILITY ?
             L"weak" : L"full" );
    if (hr == APPX_EXTRACT_S_WEAK_DURABILITY)
        fwprintf( stderr,
            L"wineappx: warning: extraction is verified but the host could "
            L"not flush directory metadata.\n" );
    HeapFree( GetProcessHeap(), 0, destination_path );
    return 0;
}

static WCHAR *deployment_result_package_name(
    const APPX_DEPLOYMENT_RESULT *result )
{
    WCHAR *name;
    UINT32 chars = 0, capacity;

    if (FAILED(api.deployment_result_get_package_full_name(
            result, &chars, NULL )) || !chars)
        return NULL;
    if (!(name = HeapAlloc(
            GetProcessHeap(), 0, chars * sizeof(*name) )))
        return NULL;
    capacity = chars;
    if (FAILED(api.deployment_result_get_package_full_name(
            result, &capacity, name )))
    {
        HeapFree( GetProcessHeap(), 0, name );
        return NULL;
    }
    return name;
}

static void print_deployment_result( const APPX_DEPLOYMENT_RESULT *result,
                                     HRESULT hr )
{
    WCHAR *name = deployment_result_package_name( result );

    if (name)
    {
        print_string_record( L"package_full_name", name );
        HeapFree( GetProcessHeap(), 0, name );
    }
    wprintf( L"catalog_epoch=%llu\n",
             (unsigned long long)
             api.deployment_result_get_catalog_epoch( result ) );
    wprintf( L"result_flags=0x%08x\n",
             api.deployment_result_get_flags( result ) );
    wprintf( L"reclaimed_entries=%u\n",
             api.deployment_result_get_reclaimed_entries( result ) );
    wprintf( L"reclaimed_bytes=%llu\n",
             (unsigned long long)
             api.deployment_result_get_reclaimed_bytes( result ) );
    wprintf( L"durability=%s\n", hr == APPX_DEPLOYMENT_S_WEAK_DURABILITY ?
             L"weak" : L"full" );
}

enum deployment_command
{
    DEPLOYMENT_INITIALIZE,
    DEPLOYMENT_INSTALL,
    DEPLOYMENT_UPDATE,
    DEPLOYMENT_REMOVE,
    DEPLOYMENT_RECOVER,
    DEPLOYMENT_GC
};

static int command_deployment(
    enum deployment_command operation, const WCHAR *argument,
    const struct command_options *command )
{
    APPX_DEPLOYMENT_RESULT *result = NULL;
    WINE_APPX_ARCHIVE_LIMITS limits;
    APPX_DEPLOYMENT_OPTIONS options;
    HANDLE file = INVALID_HANDLE_VALUE;
    const WCHAR *description;
    HRESULT hr;

    initialize_deployment_options( command, &limits, &options );
    switch (operation)
    {
    case DEPLOYMENT_INSTALL:
    case DEPLOYMENT_UPDATE:
        if (FAILED(hr = open_input_file( argument, &file )))
        {
            report_failure( L"opening input package", hr );
            return EXIT_OPERATION_FAILED;
        }
        if (operation == DEPLOYMENT_INSTALL)
        {
            description = L"package installation";
            hr = api.deployment_install( file, &options, &result );
        }
        else
        {
            description = L"package update";
            hr = api.deployment_update( file, &options, &result );
        }
        CloseHandle( file );
        break;
    case DEPLOYMENT_REMOVE:
        description = L"package removal";
        hr = api.deployment_remove( argument, &options, &result );
        break;
    case DEPLOYMENT_RECOVER:
        description = L"deployment recovery";
        hr = api.deployment_recover( &options, &result );
        break;
    case DEPLOYMENT_GC:
        description = L"garbage collection";
        hr = api.deployment_collect_garbage( &options, &result );
        break;
    default:
        description = L"store initialization";
        hr = api.deployment_initialize( &options, &result );
        break;
    }
    if (FAILED(hr))
    {
        report_failure( description, hr );
        api.deployment_result_free( result );
        return EXIT_OPERATION_FAILED;
    }
    if (!result)
    {
        report_failure( description, E_UNEXPECTED );
        return EXIT_OPERATION_FAILED;
    }
    print_deployment_result( result, hr );
    api.deployment_result_free( result );
    return 0;
}

static void print_catalog_flags( UINT32 flags )
{
    BOOL separator = FALSE;

    if (flags & APPX_CATALOG_PACKAGE_ACTIVE)
    {
        wprintf( L"active" );
        separator = TRUE;
    }
    if (flags & APPX_CATALOG_PACKAGE_FRAMEWORK)
    {
        wprintf( L"%sframework", separator ? L"," : L"" );
        separator = TRUE;
    }
    if (flags & APPX_CATALOG_PACKAGE_RESOURCE)
    {
        wprintf( L"%sresource", separator ? L"," : L"" );
        separator = TRUE;
    }
    if (flags & APPX_CATALOG_PACKAGE_SIGNED)
        wprintf( L"%ssigned", separator ? L"," : L"" );
}

static void print_catalog( const APPX_CATALOG_SNAPSHOT *catalog )
{
    UINT32 count = api.catalog_snapshot_get_package_count( catalog );
    WCHAR key[128];
    UINT32 i, j;

    wprintf( L"catalog_epoch=%llu\n",
             (unsigned long long)
             api.catalog_snapshot_get_epoch( catalog ) );
    wprintf( L"package_count=%u\n", count );
    for (i = 0; i < count; i++)
    {
        const struct appx_catalog_package *package =
            api.catalog_snapshot_get_package( catalog, i );

        if (!package) continue;
        swprintf( key, ARRAY_SIZE(key), L"package[%u].full_name", i );
        print_string_record( key, package->full_name );
        swprintf( key, ARRAY_SIZE(key), L"package[%u].family_name", i );
        print_string_record( key, package->family_name );
        wprintf( L"package[%u].version=%u.%u.%u.%u\n", i,
                 package->version.major, package->version.minor,
                 package->version.build, package->version.revision );
        wprintf( L"package[%u].architecture=%s\n", i,
                 catalog_architecture_name( package->architecture ) );
        wprintf( L"package[%u].flags=", i );
        print_catalog_flags( package->flags );
        wprintf( L"\n" );
        wprintf( L"package[%u].application_count=%u\n", i,
                 package->application_count );
        for (j = 0; j < package->application_count; j++)
        {
            const struct appx_catalog_application *application =
                package->applications + j;

            swprintf( key, ARRAY_SIZE(key),
                      L"package[%u].application[%u].id", i, j );
            print_string_record( key, application->id );
            swprintf( key, ARRAY_SIZE(key),
                      L"package[%u].application[%u].executable", i, j );
            print_string_record( key, application->executable );
            wprintf( L"package[%u].application[%u].activation=%s\n",
                     i, j,
                     catalog_activation_name(
                         application->activation_kind ) );
        }
    }
}

static int command_query( const WCHAR *full_name,
                          const struct command_options *command )
{
    APPX_CATALOG_SNAPSHOT *catalog = NULL;
    WINE_APPX_ARCHIVE_LIMITS limits;
    APPX_DEPLOYMENT_OPTIONS options;
    HRESULT hr;

    initialize_deployment_options( command, &limits, &options );
    hr = api.deployment_query( full_name, &options, &catalog );
    if (FAILED(hr))
    {
        report_failure(
            full_name ? L"package query" : L"package listing", hr );
        api.catalog_snapshot_free( catalog );
        return EXIT_OPERATION_FAILED;
    }
    if (!catalog)
    {
        report_failure(
            full_name ? L"package query" : L"package listing", E_UNEXPECTED );
        return EXIT_OPERATION_FAILED;
    }
    print_catalog( catalog );
    api.catalog_snapshot_free( catalog );
    return 0;
}

static int command_launch( const WCHAR *full_name, const WCHAR *application_id,
                           const struct command_options *command )
{
    WINE_APPX_ARCHIVE_LIMITS limits;
    APPX_DEPLOYMENT_OPTIONS options;
    PROCESS_INFORMATION process;
    STARTUPINFOW startup;
    DWORD wait, exit_code;
    HRESULT hr;

    initialize_deployment_options( command, &limits, &options );
    memset( &startup, 0, sizeof(startup) );
    startup.cb = sizeof(startup);
    hr = api.deployment_launch(
        full_name, application_id, &options, &startup, &process );
    if (FAILED(hr))
    {
        report_failure( L"full-trust launch", hr );
        return EXIT_OPERATION_FAILED;
    }
    wprintf( L"process_id=%lu\n", process.dwProcessId );
    CloseHandle( process.hThread );
    if (!command->wait)
    {
        CloseHandle( process.hProcess );
        return 0;
    }
    wait = WaitForSingleObject( process.hProcess, INFINITE );
    if (wait != WAIT_OBJECT_0 ||
        !GetExitCodeProcess( process.hProcess, &exit_code ))
    {
        hr = HRESULT_FROM_WIN32(
            wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE );
        CloseHandle( process.hProcess );
        report_failure( L"waiting for launched process", hr );
        return EXIT_OPERATION_FAILED;
    }
    CloseHandle( process.hProcess );
    wprintf( L"process_exit_code=%lu\n", exit_code );
    return (int)exit_code;
}

static BOOL command_equals( const WCHAR *command, const WCHAR *expected )
{
    return !lstrcmpiW( command, expected );
}

int __cdecl wmain( int argc, WCHAR **argv )
{
    struct command_options options;
    const WCHAR *command;
    HRESULT hr;
    int index, operands;

    index = parse_options( argc, argv, &options );
    if (!index)
    {
        print_usage( stdout );
        return 0;
    }
    if (index < 0)
    {
        fwprintf( stderr, L"wineappx: invalid or missing option.\n" );
        print_usage( stderr );
        return EXIT_USAGE_ERROR;
    }
    command = argv[index];
    operands = argc - index - 1;

    if ((command_equals( command, L"inspect" ) && operands != 1) ||
        (command_equals( command, L"unpack" ) && operands != 2) ||
        (command_equals( command, L"initialize" ) && operands) ||
        (command_equals( command, L"install" ) && operands != 1) ||
        (command_equals( command, L"update" ) && operands != 1) ||
        (command_equals( command, L"remove" ) && operands != 1) ||
        (command_equals( command, L"query" ) && operands != 1) ||
        (command_equals( command, L"list" ) && operands) ||
        (command_equals( command, L"launch" ) && operands != 2) ||
        (command_equals( command, L"recover" ) && operands) ||
        (command_equals( command, L"gc" ) && operands))
    {
        fwprintf( stderr, L"wineappx: wrong number of arguments for %s.\n",
                  command );
        print_usage( stderr );
        return EXIT_USAGE_ERROR;
    }
    if (!command_equals( command, L"inspect" ) &&
        !command_equals( command, L"unpack" ) &&
        !command_equals( command, L"initialize" ) &&
        !command_equals( command, L"install" ) &&
        !command_equals( command, L"update" ) &&
        !command_equals( command, L"remove" ) &&
        !command_equals( command, L"query" ) &&
        !command_equals( command, L"list" ) &&
        !command_equals( command, L"launch" ) &&
        !command_equals( command, L"recover" ) &&
        !command_equals( command, L"gc" ))
    {
        fwprintf( stderr, L"wineappx: unknown command %s.\n", command );
        print_usage( stderr );
        return EXIT_USAGE_ERROR;
    }
    if (options.wait && !command_equals( command, L"launch" ))
    {
        fwprintf( stderr,
                  L"wineappx: --wait is valid only with launch.\n" );
        return EXIT_USAGE_ERROR;
    }
    if ((options.deployment_flags & APPX_DEPLOYMENT_ALLOW_DOWNGRADE) &&
        !command_equals( command, L"update" ))
    {
        fwprintf( stderr,
                  L"wineappx: --allow-downgrade is valid only with update.\n" );
        return EXIT_USAGE_ERROR;
    }

    if (FAILED(hr = load_appxsvc()))
    {
        report_failure( L"loading the appxsvc private API", hr );
        if (api.module) FreeLibrary( api.module );
        return EXIT_API_UNAVAILABLE;
    }

    if (command_equals( command, L"inspect" ))
        index = command_inspect( argv[index + 1], &options );
    else if (command_equals( command, L"unpack" ))
        index = command_unpack(
            argv[index + 1], argv[index + 2], &options );
    else if (command_equals( command, L"initialize" ))
        index = command_deployment(
            DEPLOYMENT_INITIALIZE, NULL, &options );
    else if (command_equals( command, L"install" ))
        index = command_deployment(
            DEPLOYMENT_INSTALL, argv[index + 1], &options );
    else if (command_equals( command, L"update" ))
        index = command_deployment(
            DEPLOYMENT_UPDATE, argv[index + 1], &options );
    else if (command_equals( command, L"remove" ))
        index = command_deployment(
            DEPLOYMENT_REMOVE, argv[index + 1], &options );
    else if (command_equals( command, L"query" ))
        index = command_query( argv[index + 1], &options );
    else if (command_equals( command, L"list" ))
        index = command_query( NULL, &options );
    else if (command_equals( command, L"launch" ))
        index = command_launch(
            argv[index + 1], argv[index + 2], &options );
    else if (command_equals( command, L"recover" ))
        index = command_deployment(
            DEPLOYMENT_RECOVER, NULL, &options );
    else
        index = command_deployment( DEPLOYMENT_GC, NULL, &options );

    if (!index && (ferror( stdout ) || fflush( stdout ) == EOF))
    {
        fwprintf( stderr, L"wineappx: writing command output failed.\n" );
        index = EXIT_OPERATION_FAILED;
    }
    FreeLibrary( api.module );
    return index;
}
