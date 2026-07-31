/*
 * AppX package catalog persistence tests
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

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "bcrypt.h"

#include "../catalog.h"
#include "wine/test.h"

#define CATALOG_HEADER_VERSION_OFFSET               8
#define CATALOG_HEADER_FILE_SIZE_OFFSET             16
#define CATALOG_HEADER_PACKAGE_COUNT_OFFSET         32
#define CATALOG_HEADER_DIGEST_OFFSET                48
#define CATALOG_PACKAGE_RECORD_SIZE_OFFSET          0
#define CATALOG_PACKAGE_FLAGS_OFFSET                4
#define CATALOG_PACKAGE_ARCHITECTURE_OFFSET         8
#define CATALOG_PACKAGE_APPLICATION_COUNT_OFFSET    12
#define CATALOG_PACKAGE_APPLICATIONS_OFFSET         20
#define CATALOG_PACKAGE_STRING_REFS_OFFSET          76
#define CATALOG_PACKAGE_FIXED_SIZE                  132
#define CATALOG_APPLICATION_KIND_OFFSET             0

#define TEST_WRITER_COUNT                           4
#define TEST_WRITER_RETRIES                         16
#define TEST_THREAD_TIMEOUT                         10000

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

static NTSTATUS (WINAPI *p_BCryptOpenAlgorithmProvider)(
    BCRYPT_ALG_HANDLE *, const WCHAR *, const WCHAR *, ULONG );
static NTSTATUS (WINAPI *p_BCryptCreateHash)(
    BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, BYTE *, ULONG, BYTE *, ULONG, ULONG );
static NTSTATUS (WINAPI *p_BCryptHashData)( BCRYPT_HASH_HANDLE, BYTE *, ULONG, ULONG );
static NTSTATUS (WINAPI *p_BCryptFinishHash)( BCRYPT_HASH_HANDLE, BYTE *, ULONG, ULONG );
static NTSTATUS (WINAPI *p_BCryptDestroyHash)( BCRYPT_HASH_HANDLE );
static NTSTATUS (WINAPI *p_BCryptCloseAlgorithmProvider)( BCRYPT_ALG_HANDLE, ULONG );
static HRESULT (WINAPI *p_appx_catalog_snapshot_create)(
    UINT64, const struct appx_catalog_package *, UINT32, APPX_CATALOG_SNAPSHOT ** );
static HRESULT (WINAPI *p_appx_catalog_snapshot_deep_copy)(
    const APPX_CATALOG_SNAPSHOT *, APPX_CATALOG_SNAPSHOT ** );
static void (WINAPI *p_appx_catalog_snapshot_free)( APPX_CATALOG_SNAPSHOT * );
static UINT64 (WINAPI *p_appx_catalog_snapshot_get_epoch)(
    const APPX_CATALOG_SNAPSHOT * );
static UINT32 (WINAPI *p_appx_catalog_snapshot_get_package_count)(
    const APPX_CATALOG_SNAPSHOT * );
static const struct appx_catalog_package *(WINAPI
    *p_appx_catalog_snapshot_get_package)( const APPX_CATALOG_SNAPSHOT *, UINT32 );
static HRESULT (WINAPI *p_appx_catalog_load)(
    const WCHAR *, APPX_CATALOG_SNAPSHOT ** );
static HRESULT (WINAPI *p_appx_catalog_publish)(
    const WCHAR *, UINT64, const APPX_CATALOG_SNAPSHOT * );

#define appx_catalog_snapshot_create p_appx_catalog_snapshot_create
#define appx_catalog_snapshot_deep_copy p_appx_catalog_snapshot_deep_copy
#define appx_catalog_snapshot_free p_appx_catalog_snapshot_free
#define appx_catalog_snapshot_get_epoch p_appx_catalog_snapshot_get_epoch
#define appx_catalog_snapshot_get_package_count p_appx_catalog_snapshot_get_package_count
#define appx_catalog_snapshot_get_package p_appx_catalog_snapshot_get_package
#define appx_catalog_load p_appx_catalog_load
#define appx_catalog_publish p_appx_catalog_publish

struct catalog_bytes
{
    BYTE *data;
    DWORD size;
};

struct writer_context
{
    WCHAR root[MAX_PATH];
    LONG successes;
    LONG failure;
};

struct publish_context
{
    WCHAR root[MAX_PATH];
    APPX_CATALOG_SNAPSHOT *snapshot;
    HANDLE started;
    HRESULT result;
};

static void put_u32( BYTE *data, DWORD value )
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static DWORD read_u32( const BYTE *data )
{
    return data[0] | ((DWORD)data[1] << 8) | ((DWORD)data[2] << 16) |
           ((DWORD)data[3] << 24);
}

static BOOL append_name( WCHAR *path, const WCHAR *root, const WCHAR *name )
{
    lstrcpyW( path, root );
    if (path[lstrlenW(path) - 1] != '\\') lstrcatW( path, L"\\" );
    lstrcatW( path, name );
    return TRUE;
}

static BOOL create_test_directory( WCHAR *root )
{
    WCHAR temp[MAX_PATH];

    if (!GetTempPathW( ARRAY_SIZE(temp), temp )) return FALSE;
    if (!GetTempFileNameW( temp, L"cat", 0, root )) return FALSE;
    DeleteFileW( root );
    return CreateDirectoryW( root, NULL );
}

static void cleanup_test_directory( const WCHAR *root )
{
    WCHAR path[MAX_PATH];

    append_name( path, root, APPX_CATALOG_PENDING_FILE_NAME );
    DeleteFileW( path );
    RemoveDirectoryW( path );
    append_name( path, root, APPX_CATALOG_FILE_NAME );
    DeleteFileW( path );
    append_name( path, root, APPX_CATALOG_LOCK_FILE_NAME );
    DeleteFileW( path );
    RemoveDirectoryW( root );
}

static BOOL load_bcrypt( void )
{
    HMODULE bcrypt = LoadLibraryW( L"bcrypt.dll" );

    if (!bcrypt) return FALSE;
    p_BCryptOpenAlgorithmProvider =
        (void *)GetProcAddress( bcrypt, "BCryptOpenAlgorithmProvider" );
    p_BCryptCreateHash = (void *)GetProcAddress( bcrypt, "BCryptCreateHash" );
    p_BCryptHashData = (void *)GetProcAddress( bcrypt, "BCryptHashData" );
    p_BCryptFinishHash = (void *)GetProcAddress( bcrypt, "BCryptFinishHash" );
    p_BCryptDestroyHash = (void *)GetProcAddress( bcrypt, "BCryptDestroyHash" );
    p_BCryptCloseAlgorithmProvider =
        (void *)GetProcAddress( bcrypt, "BCryptCloseAlgorithmProvider" );
    return p_BCryptOpenAlgorithmProvider && p_BCryptCreateHash &&
           p_BCryptHashData && p_BCryptFinishHash && p_BCryptDestroyHash &&
           p_BCryptCloseAlgorithmProvider;
}

static BOOL hash_catalog_bytes( const BYTE *data, DWORD size, BYTE digest[32] )
{
    static const BYTE zero_digest[32] = {0};
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    NTSTATUS status;
    BOOL result = FALSE;

    if ((status = p_BCryptOpenAlgorithmProvider( &algorithm,
                                                 BCRYPT_SHA256_ALGORITHM,
                                                 NULL, 0 )))
        goto done;
    if ((status = p_BCryptCreateHash( algorithm, &hash, NULL, 0, NULL, 0, 0 )))
        goto done;
    if ((status = p_BCryptHashData( hash, (BYTE *)data,
                                    CATALOG_HEADER_DIGEST_OFFSET, 0 )))
        goto done;
    if ((status = p_BCryptHashData( hash, (BYTE *)zero_digest,
                                    sizeof(zero_digest), 0 )))
        goto done;
    if ((status = p_BCryptHashData( hash,
            (BYTE *)data + CATALOG_HEADER_DIGEST_OFFSET + sizeof(zero_digest),
            size - CATALOG_HEADER_DIGEST_OFFSET - sizeof(zero_digest), 0 )))
        goto done;
    if ((status = p_BCryptFinishHash( hash, digest, sizeof(zero_digest), 0 )))
        goto done;
    result = TRUE;

done:
    if (hash) p_BCryptDestroyHash( hash );
    if (algorithm) p_BCryptCloseAlgorithmProvider( algorithm, 0 );
    return result;
}

static void fix_catalog_digest( struct catalog_bytes *bytes )
{
    BYTE digest[32];

    memset( bytes->data + CATALOG_HEADER_DIGEST_OFFSET, 0, sizeof(digest) );
    ok( hash_catalog_bytes( bytes->data, bytes->size, digest ),
        "failed to hash catalog bytes.\n" );
    memcpy( bytes->data + CATALOG_HEADER_DIGEST_OFFSET, digest, sizeof(digest) );
}

static BOOL read_catalog_bytes( const WCHAR *root, struct catalog_bytes *bytes )
{
    WCHAR path[MAX_PATH];
    LARGE_INTEGER size;
    HANDLE file;
    DWORD read;

    memset( bytes, 0, sizeof(*bytes) );
    append_name( path, root, APPX_CATALOG_FILE_NAME );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to open catalog, error %lu.\n",
        GetLastError() );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok( GetFileSizeEx( file, &size ), "failed to get catalog size, error %lu.\n",
        GetLastError() );
    if (size.QuadPart > MAXDWORD)
    {
        CloseHandle( file );
        return FALSE;
    }
    bytes->size = size.u.LowPart;
    bytes->data = HeapAlloc( GetProcessHeap(), 0, bytes->size );
    ok( !!bytes->data, "failed to allocate catalog bytes.\n" );
    if (!bytes->data)
    {
        CloseHandle( file );
        return FALSE;
    }
    ok( ReadFile( file, bytes->data, bytes->size, &read, NULL ) &&
        read == bytes->size, "failed to read catalog, error %lu.\n",
        GetLastError() );
    CloseHandle( file );
    return TRUE;
}

static BOOL write_catalog_bytes( const WCHAR *root, const struct catalog_bytes *bytes )
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD written;
    BOOL ret;

    append_name( path, root, APPX_CATALOG_FILE_NAME );
    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to create catalog, error %lu.\n",
        GetLastError() );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ret = WriteFile( file, bytes->data, bytes->size, &written, NULL ) &&
          written == bytes->size;
    ok( ret, "failed to write catalog, error %lu.\n", GetLastError() );
    CloseHandle( file );
    return ret;
}

static void free_catalog_bytes( struct catalog_bytes *bytes )
{
    HeapFree( GetProcessHeap(), 0, bytes->data );
    memset( bytes, 0, sizeof(*bytes) );
}

static void init_content_id( BYTE content_id[APPX_CATALOG_CONTENT_ID_SIZE], BYTE seed )
{
    UINT32 i;

    for (i = 0; i < APPX_CATALOG_CONTENT_ID_SIZE; i++)
        content_id[i] = seed + i;
}

static void init_package( struct appx_catalog_package *package, const WCHAR *name,
                          const WCHAR *full_name, const WCHAR *family_name,
                          const WCHAR *payload_path, UINT32 flags )
{
    static const struct appx_catalog_application applications[] =
    {
        {L"App", L"VFS\\ProgramFilesX64\\App\\app.exe",
         L"Windows.FullTrustApplication", APPX_CATALOG_ACTIVATION_FULL_TRUST},
    };
    static const struct appx_catalog_dependency dependencies[] =
    {
        {L"Microsoft.WindowsAppRuntime.1.6",
         L"CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US",
         {6000, 1, 0, 0}},
    };

    memset( package, 0, sizeof(*package) );
    package->name = name;
    package->publisher = L"CN=Contoso";
    package->resource_id = L"";
    package->publisher_id = L"8wekyb3d8bbwe";
    package->full_name = full_name;
    package->family_name = family_name;
    package->payload_path = payload_path;
    package->version.major = 1;
    package->version.minor = 2;
    package->version.build = 3;
    package->version.revision = 4;
    package->architecture = APPX_CATALOG_ARCHITECTURE_X64;
    package->flags = flags;
    init_content_id( package->content_id, name[0] );
    package->application_count = ARRAY_SIZE(applications);
    package->applications = applications;
    package->dependency_count = ARRAY_SIZE(dependencies);
    package->dependencies = dependencies;
}

static APPX_CATALOG_SNAPSHOT *make_snapshot( UINT64 epoch, BOOL reversed )
{
    struct appx_catalog_package packages[2];
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    HRESULT hr;

    init_package( packages, L"Contoso.Alpha",
                  L"Contoso.Alpha_1.2.3.4_x64__8wekyb3d8bbwe",
                  L"Contoso.Alpha_8wekyb3d8bbwe",
                  L"Packages\\Contoso.Alpha", APPX_CATALOG_PACKAGE_ACTIVE |
                  APPX_CATALOG_PACKAGE_SIGNED );
    init_package( packages + 1, L"Contoso.Beta",
                  L"Contoso.Beta_1.2.3.4_x64__8wekyb3d8bbwe",
                  L"Contoso.Beta_8wekyb3d8bbwe",
                  L"Packages\\Contoso.Beta", APPX_CATALOG_PACKAGE_ACTIVE |
                  APPX_CATALOG_PACKAGE_SIGNED );
    if (reversed)
    {
        struct appx_catalog_package temporary = packages[0];

        packages[0] = packages[1];
        packages[1] = temporary;
    }
    hr = appx_catalog_snapshot_create( epoch, packages, ARRAY_SIZE(packages),
                                       &snapshot );
    ok( hr == S_OK, "failed to create snapshot, hr %#lx.\n", hr );
    return snapshot;
}

static void check_loaded_package( const struct appx_catalog_package *package,
                                  const WCHAR *full_name )
{
    ok( package != NULL, "missing package %s.\n", wine_dbgstr_w(full_name) );
    if (!package) return;
    ok( !lstrcmpW( package->full_name, full_name ), "got full name %s.\n",
        wine_dbgstr_w(package->full_name) );
    ok( package->application_count == 1, "got application count %u.\n",
        package->application_count );
    ok( package->dependency_count == 1, "got dependency count %u.\n",
        package->dependency_count );
    ok( package->flags == (APPX_CATALOG_PACKAGE_ACTIVE |
                           APPX_CATALOG_PACKAGE_SIGNED),
        "got flags %#x.\n", package->flags );
    ok( package->architecture == APPX_CATALOG_ARCHITECTURE_X64,
        "got architecture %u.\n", package->architecture );
}

static void test_roundtrip( void )
{
    APPX_CATALOG_SNAPSHOT *snapshot, *copy = NULL, *loaded = NULL;
    WCHAR root[MAX_PATH];
    HRESULT hr;

    ok( create_test_directory( root ), "failed to create test directory.\n" );
    snapshot = make_snapshot( 1, TRUE );
    hr = appx_catalog_snapshot_deep_copy( snapshot, &copy );
    ok( hr == S_OK, "deep copy returned %#lx.\n", hr );
    ok( appx_catalog_snapshot_get_epoch( copy ) == 1, "got copy epoch %s.\n",
        wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(copy)) );

    hr = appx_catalog_publish( root, 0, snapshot );
    ok( SUCCEEDED(hr), "publish returned %#lx.\n", hr );
    hr = appx_catalog_load( root, &loaded );
    ok( hr == S_OK, "load returned %#lx.\n", hr );
    ok( appx_catalog_snapshot_get_epoch( loaded ) == 1, "got epoch %s.\n",
        wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(loaded)) );
    ok( appx_catalog_snapshot_get_package_count( loaded ) == 2,
        "got package count %u.\n", appx_catalog_snapshot_get_package_count(loaded) );
    check_loaded_package( appx_catalog_snapshot_get_package( loaded, 0 ),
                          L"Contoso.Alpha_1.2.3.4_x64__8wekyb3d8bbwe" );
    check_loaded_package( appx_catalog_snapshot_get_package( loaded, 1 ),
                          L"Contoso.Beta_1.2.3.4_x64__8wekyb3d8bbwe" );
    ok( !appx_catalog_snapshot_get_package( loaded, 2 ),
        "out-of-range package lookup succeeded.\n" );

    appx_catalog_snapshot_free( loaded );
    appx_catalog_snapshot_free( copy );
    appx_catalog_snapshot_free( snapshot );
    cleanup_test_directory( root );
}

static void test_large_record_reallocation( void )
{
    struct appx_catalog_application application;
    struct appx_catalog_package package;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL, *loaded = NULL;
    const struct appx_catalog_package *loaded_package;
    WCHAR root[MAX_PATH], *entry_point;
    UINT32 i;
    HRESULT hr;

    entry_point = HeapAlloc( GetProcessHeap(), 0, 8193 * sizeof(*entry_point) );
    ok( !!entry_point, "failed to allocate large entry point.\n" );
    if (!entry_point) return;
    for (i = 0; i < 8192; i++) entry_point[i] = 'A' + i % 26;
    entry_point[8192] = 0;

    init_package( &package, L"Contoso.Large",
                  L"Contoso.Large_1.2.3.4_x64__8wekyb3d8bbwe",
                  L"Contoso.Large_8wekyb3d8bbwe",
                  L"Packages\\Contoso.Large",
                  APPX_CATALOG_PACKAGE_ACTIVE | APPX_CATALOG_PACKAGE_SIGNED );
    application = package.applications[0];
    application.entry_point = entry_point;
    package.applications = &application;

    if (!create_test_directory( root ))
    {
        ok( 0, "failed to create large-record directory.\n" );
        HeapFree( GetProcessHeap(), 0, entry_point );
        return;
    }
    hr = appx_catalog_snapshot_create( 1, &package, 1, &snapshot );
    ok( hr == S_OK, "large snapshot returned %#lx.\n", hr );
    if (SUCCEEDED(hr))
    {
        hr = appx_catalog_publish( root, 0, snapshot );
        ok( SUCCEEDED(hr), "large publish returned %#lx.\n", hr );
    }
    if (SUCCEEDED(hr))
    {
        hr = appx_catalog_load( root, &loaded );
        ok( hr == S_OK, "large load returned %#lx.\n", hr );
    }
    loaded_package = appx_catalog_snapshot_get_package( loaded, 0 );
    ok( loaded_package && loaded_package->application_count == 1,
        "large record application is missing.\n" );
    if (loaded_package && loaded_package->application_count == 1)
        ok( !lstrcmpW( loaded_package->applications[0].entry_point, entry_point ),
            "large entry point changed during serialization.\n" );

    appx_catalog_snapshot_free( loaded );
    appx_catalog_snapshot_free( snapshot );
    cleanup_test_directory( root );
    HeapFree( GetProcessHeap(), 0, entry_point );
}

static void test_missing_and_stale_pending( void )
{
    APPX_CATALOG_SNAPSHOT *snapshot = NULL, *loaded = NULL;
    WCHAR root[MAX_PATH], path[MAX_PATH];
    HANDLE file;
    DWORD written;
    HRESULT hr;

    ok( create_test_directory( root ), "failed to create test directory.\n" );
    append_name( path, root, APPX_CATALOG_PENDING_FILE_NAME );
    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to create stale pending file.\n" );
    if (file != INVALID_HANDLE_VALUE)
    {
        ok( WriteFile( file, "stale", 5, &written, NULL ), "pending write failed.\n" );
        CloseHandle( file );
    }
    hr = appx_catalog_load( root, &loaded );
    ok( hr == S_OK, "missing load returned %#lx.\n", hr );
    ok( appx_catalog_snapshot_get_epoch( loaded ) == 0, "got epoch %s.\n",
        wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(loaded)) );
    ok( appx_catalog_snapshot_get_package_count( loaded ) == 0,
        "got package count %u.\n", appx_catalog_snapshot_get_package_count(loaded) );
    ok( GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND, "stale pending was not removed.\n" );
    appx_catalog_snapshot_free( loaded );

    snapshot = make_snapshot( 1, FALSE );
    hr = appx_catalog_publish( root, 0, snapshot );
    ok( SUCCEEDED(hr), "publish returned %#lx.\n", hr );
    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to create second stale pending file.\n" );
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    loaded = NULL;
    hr = appx_catalog_load( root, &loaded );
    ok( hr == S_OK, "active load returned %#lx.\n", hr );
    ok( appx_catalog_snapshot_get_epoch( loaded ) == 1, "got epoch %s.\n",
        wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(loaded)) );

    appx_catalog_snapshot_free( loaded );
    appx_catalog_snapshot_free( snapshot );
    cleanup_test_directory( root );
}

static void expect_mutated_catalog( const struct catalog_bytes *source,
                                    DWORD offset, DWORD value, HRESULT expected )
{
    struct catalog_bytes bytes;
    APPX_CATALOG_SNAPSHOT *loaded = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;
    WCHAR root[MAX_PATH];
    HRESULT hr;

    ok( offset + sizeof(value) <= source->size, "invalid mutation offset %lu.\n",
        offset );
    if (offset + sizeof(value) > source->size) return;
    ok( create_test_directory( root ), "failed to create mutation directory.\n" );
    bytes.size = source->size;
    bytes.data = HeapAlloc( GetProcessHeap(), 0, bytes.size );
    ok( !!bytes.data, "failed to allocate mutated catalog.\n" );
    if (bytes.data)
    {
        memcpy( bytes.data, source->data, bytes.size );
        put_u32( bytes.data + offset, value );
        fix_catalog_digest( &bytes );
        ok( write_catalog_bytes( root, &bytes ), "failed to write mutated catalog.\n" );
        hr = appx_catalog_load( root, &loaded );
        ok( hr == expected, "mutated catalog returned %#lx, expected %#lx.\n",
            hr, expected );
        ok( !loaded, "mutated catalog returned snapshot %p.\n", loaded );
        free_catalog_bytes( &bytes );
    }
    cleanup_test_directory( root );
}

static void test_canonical_and_binary_boundaries( void )
{
    APPX_CATALOG_SNAPSHOT *snapshot_a, *snapshot_b;
    struct catalog_bytes bytes_a, bytes_b, mutated;
    DWORD record, app_record;
    WCHAR root_a[MAX_PATH], root_b[MAX_PATH];
    HRESULT hr;

    ok( create_test_directory( root_a ), "failed to create first directory.\n" );
    ok( create_test_directory( root_b ), "failed to create second directory.\n" );
    snapshot_a = make_snapshot( 1, FALSE );
    snapshot_b = make_snapshot( 1, TRUE );
    hr = appx_catalog_publish( root_a, 0, snapshot_a );
    ok( SUCCEEDED(hr), "first publish returned %#lx.\n", hr );
    hr = appx_catalog_publish( root_b, 0, snapshot_b );
    ok( SUCCEEDED(hr), "second publish returned %#lx.\n", hr );
    ok( read_catalog_bytes( root_a, &bytes_a ), "failed to read first catalog.\n" );
    ok( read_catalog_bytes( root_b, &bytes_b ), "failed to read second catalog.\n" );
    ok( bytes_a.size == bytes_b.size &&
        !memcmp( bytes_a.data, bytes_b.data, bytes_a.size ),
        "canonical catalog bytes differ for input order.\n" );

    record = APPX_CATALOG_HEADER_SIZE;
    app_record = record + read_u32( bytes_a.data + record +
                                    CATALOG_PACKAGE_APPLICATIONS_OFFSET );
    expect_mutated_catalog( &bytes_a, CATALOG_HEADER_PACKAGE_COUNT_OFFSET,
                            APPX_CATALOG_MAX_PACKAGES + 1,
                            APPX_E_INVALID_PACKAGING_LAYOUT );
    expect_mutated_catalog( &bytes_a, record + CATALOG_PACKAGE_RECORD_SIZE_OFFSET,
                            CATALOG_PACKAGE_FIXED_SIZE - 1,
                            APPX_E_INVALID_PACKAGING_LAYOUT );
    expect_mutated_catalog( &bytes_a, record + CATALOG_PACKAGE_RECORD_SIZE_OFFSET,
                            MAXDWORD, APPX_E_INVALID_PACKAGING_LAYOUT );
    expect_mutated_catalog( &bytes_a, record + CATALOG_PACKAGE_FLAGS_OFFSET,
                            0x80000000, APPX_E_INVALID_PACKAGING_LAYOUT );
    expect_mutated_catalog( &bytes_a, record + CATALOG_PACKAGE_ARCHITECTURE_OFFSET,
                            0x80000000, APPX_E_INVALID_PACKAGING_LAYOUT );
    expect_mutated_catalog( &bytes_a, record + CATALOG_PACKAGE_APPLICATION_COUNT_OFFSET,
                            APPX_CATALOG_MAX_APPLICATIONS_PER_PACKAGE + 1,
                            APPX_E_INVALID_PACKAGING_LAYOUT );
    expect_mutated_catalog( &bytes_a, app_record + CATALOG_APPLICATION_KIND_OFFSET,
                            0x80000000, APPX_E_INVALID_PACKAGING_LAYOUT );
    expect_mutated_catalog( &bytes_a, record + CATALOG_PACKAGE_STRING_REFS_OFFSET,
                            bytes_a.size, APPX_E_INVALID_PACKAGING_LAYOUT );
    expect_mutated_catalog( &bytes_a, record + CATALOG_PACKAGE_STRING_REFS_OFFSET + 4,
                            0, APPX_E_INVALID_PACKAGING_LAYOUT );

    mutated.size = bytes_a.size;
    mutated.data = HeapAlloc( GetProcessHeap(), 0, mutated.size );
    ok( !!mutated.data, "failed to allocate digest mutation.\n" );
    if (mutated.data)
    {
        APPX_CATALOG_SNAPSHOT *loaded = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;

        memcpy( mutated.data, bytes_a.data, mutated.size );
        mutated.data[mutated.size - 1] ^= 1;
        ok( write_catalog_bytes( root_a, &mutated ), "failed to write digest mutation.\n" );
        hr = appx_catalog_load( root_a, &loaded );
        ok( hr == APPX_E_DIGEST_MISMATCH, "digest mutation returned %#lx.\n", hr );
        ok( !loaded, "digest mutation returned snapshot %p.\n", loaded );
        free_catalog_bytes( &mutated );
    }

    mutated.size = bytes_b.size - 1;
    mutated.data = HeapAlloc( GetProcessHeap(), 0, mutated.size );
    ok( !!mutated.data, "failed to allocate truncated catalog.\n" );
    if (mutated.data)
    {
        APPX_CATALOG_SNAPSHOT *loaded = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;

        memcpy( mutated.data, bytes_b.data, mutated.size );
        ok( write_catalog_bytes( root_b, &mutated ), "failed to write truncation.\n" );
        hr = appx_catalog_load( root_b, &loaded );
        ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "truncation returned %#lx.\n", hr );
        ok( !loaded, "truncation returned snapshot %p.\n", loaded );
        free_catalog_bytes( &mutated );
    }

    mutated.size = bytes_b.size + 1;
    mutated.data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, mutated.size );
    ok( !!mutated.data, "failed to allocate trailing catalog.\n" );
    if (mutated.data)
    {
        APPX_CATALOG_SNAPSHOT *loaded = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;

        memcpy( mutated.data, bytes_b.data, bytes_b.size );
        ok( write_catalog_bytes( root_b, &mutated ), "failed to write trailing byte.\n" );
        hr = appx_catalog_load( root_b, &loaded );
        ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "trailing byte returned %#lx.\n", hr );
        ok( !loaded, "trailing byte returned snapshot %p.\n", loaded );
        free_catalog_bytes( &mutated );
    }

    expect_mutated_catalog( &bytes_b, CATALOG_HEADER_VERSION_OFFSET, 2,
                            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) );

    free_catalog_bytes( &bytes_a );
    free_catalog_bytes( &bytes_b );
    appx_catalog_snapshot_free( snapshot_a );
    appx_catalog_snapshot_free( snapshot_b );
    cleanup_test_directory( root_a );
    cleanup_test_directory( root_b );
}

static void test_snapshot_validation( void )
{
    struct appx_catalog_application application;
    struct appx_catalog_package packages[2];
    APPX_CATALOG_SNAPSHOT *snapshot = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;
    HRESULT hr;

    init_package( packages, L"Contoso.Alpha",
                  L"Contoso.Alpha_1.2.3.4_x64__8wekyb3d8bbwe",
                  L"Contoso.Alpha_8wekyb3d8bbwe",
                  L"Packages\\Contoso.Alpha", APPX_CATALOG_PACKAGE_ACTIVE );
    packages[0].payload_path = L"..\\escape";
    hr = appx_catalog_snapshot_create( 1, packages, 1, &snapshot );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "bad payload path returned %#lx.\n", hr );
    ok( !snapshot, "bad payload path returned snapshot %p.\n", snapshot );

    init_package( packages, L"Contoso.Alpha",
                  L"Contoso.Alpha_1.2.3.4_x64__8wekyb3d8bbwe",
                  L"Contoso.Alpha_8wekyb3d8bbwe",
                  L"Packages\\Contoso.Alpha", APPX_CATALOG_PACKAGE_ACTIVE );
    application = packages[0].applications[0];
    application.executable = L"..\\outside.exe";
    packages[0].applications = &application;
    snapshot = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;
    hr = appx_catalog_snapshot_create( 1, packages, 1, &snapshot );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "bad executable path returned %#lx.\n", hr );
    ok( !snapshot, "bad executable path returned snapshot %p.\n", snapshot );

    init_package( packages, L"Contoso.Alpha",
                  L"Contoso.Alpha_1.2.3.4_x64__8wekyb3d8bbwe",
                  L"Contoso.Alpha_8wekyb3d8bbwe",
                  L"Packages\\Contoso.Alpha", APPX_CATALOG_PACKAGE_ACTIVE );
    packages[1] = packages[0];
    packages[1].payload_path = L"Packages\\Contoso.Alpha2";
    hr = appx_catalog_snapshot_create( 1, packages, 2, &snapshot );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT, "duplicate full name returned %#lx.\n", hr );
    ok( !snapshot, "duplicate full name returned snapshot %p.\n", snapshot );

    init_package( packages, L"Contoso.Alpha",
                  L"Contoso.Alpha_1.2.3.4_x64__8wekyb3d8bbwe",
                  L"Contoso.Alpha_8wekyb3d8bbwe",
                  L"Packages\\Contoso.Alpha", APPX_CATALOG_PACKAGE_ACTIVE );
    init_package( packages + 1, L"Contoso.Alpha",
                  L"Contoso.Alpha_1.2.3.4_x86__8wekyb3d8bbwe",
                  L"Contoso.Alpha_8wekyb3d8bbwe",
                  L"Packages\\Contoso.Alpha.x86", APPX_CATALOG_PACKAGE_ACTIVE );
    packages[1].architecture = APPX_CATALOG_ARCHITECTURE_X86;
    hr = appx_catalog_snapshot_create( 1, packages, 2, &snapshot );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "family/version active ambiguity returned %#lx.\n", hr );
    ok( !snapshot, "active ambiguity returned snapshot %p.\n", snapshot );

    hr = appx_catalog_snapshot_create( 1, NULL, APPX_CATALOG_MAX_PACKAGES + 1,
                                       &snapshot );
    ok( hr == E_INVALIDARG, "too many packages returned %#lx.\n", hr );
    ok( !snapshot, "too many packages returned snapshot %p.\n", snapshot );
}

static void test_epoch_conflict_and_publish_failure( void )
{
    APPX_CATALOG_SNAPSHOT *snapshot, *replacement, *loaded = NULL;
    WCHAR root[MAX_PATH], pending[MAX_PATH], catalog[MAX_PATH];
    HANDLE file;
    DWORD attributes, error;
    HRESULT hr;

    ok( create_test_directory( root ), "failed to create test directory.\n" );
    snapshot = make_snapshot( 1, FALSE );
    replacement = make_snapshot( 2, FALSE );
    hr = appx_catalog_publish( root, 0, snapshot );
    ok( SUCCEEDED(hr), "initial publish returned %#lx.\n", hr );
    hr = appx_catalog_publish( root, 0, snapshot );
    ok( hr == APPX_CATALOG_E_EPOCH_CONFLICT, "epoch conflict returned %#lx.\n", hr );

    append_name( pending, root, APPX_CATALOG_PENDING_FILE_NAME );
    ok( CreateDirectoryW( pending, NULL ), "failed to create pending directory.\n" );
    hr = appx_catalog_publish( root, 1, replacement );
    ok( FAILED(hr), "publish with pending directory returned %#lx.\n", hr );
    RemoveDirectoryW( pending );
    hr = appx_catalog_load( root, &loaded );
    ok( hr == S_OK, "post-failure load returned %#lx.\n", hr );
    ok( appx_catalog_snapshot_get_epoch( loaded ) == 1,
        "publish failure did not preserve old catalog, epoch %s.\n",
        wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(loaded)) );
    appx_catalog_snapshot_free( loaded );
    loaded = NULL;

    append_name( catalog, root, APPX_CATALOG_FILE_NAME );
    file = CreateFileW( catalog, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to hold catalog open, error %lu.\n",
        GetLastError() );
    if (file != INVALID_HANDLE_VALUE)
    {
        hr = appx_catalog_publish( root, 1, replacement );
        ok( FAILED(hr), "publish with non-delete-shared catalog returned %#lx.\n", hr );
        CloseHandle( file );
        attributes = GetFileAttributesW( pending );
        error = GetLastError();
        ok( attributes == INVALID_FILE_ATTRIBUTES &&
            error == ERROR_FILE_NOT_FOUND,
            "failed rename left pending file, attributes %#lx, error %lu.\n",
            attributes, error );
        hr = appx_catalog_load( root, &loaded );
        ok( hr == S_OK, "load after rename failure returned %#lx.\n", hr );
        ok( appx_catalog_snapshot_get_epoch( loaded ) == 1,
            "rename failure changed catalog epoch to %s.\n",
            wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(loaded)) );
    }

    appx_catalog_snapshot_free( loaded );
    appx_catalog_snapshot_free( replacement );
    appx_catalog_snapshot_free( snapshot );
    cleanup_test_directory( root );
}

static DWORD WINAPI publish_thread( void *arg )
{
    struct publish_context *context = arg;

    SetEvent( context->started );
    context->result = appx_catalog_publish( context->root, 0,
                                            context->snapshot );
    return 0;
}

static void run_store_swap_test( BOOL ancestor )
{
    APPX_CATALOG_SNAPSHOT *snapshot = NULL, *loaded = NULL;
    struct publish_context *context = NULL;
    OVERLAPPED overlapped = {0};
    WCHAR container[MAX_PATH], moved_container[MAX_PATH];
    WCHAR root[MAX_PATH], moved_root[MAX_PATH], lock_path[MAX_PATH];
    HANDLE lock = INVALID_HANDLE_VALUE, thread = NULL;
    DWORD wait;
    HRESULT hr;

    if (ancestor)
    {
        ok( create_test_directory( container ),
            "failed to create ancestor swap directory.\n" );
        append_name( root, container, L"store" );
        ok( CreateDirectoryW( root, NULL ),
            "failed to create nested catalog root, error %lu.\n", GetLastError() );
        lstrcpyW( moved_container, container );
        lstrcatW( moved_container, L".moved" );
        append_name( moved_root, moved_container, L"store" );
    }
    else
    {
        ok( create_test_directory( root ),
            "failed to create root swap directory.\n" );
        lstrcpyW( moved_root, root );
        lstrcatW( moved_root, L".moved" );
        lstrcpyW( container, root );
        lstrcpyW( moved_container, moved_root );
    }

    snapshot = make_snapshot( 1, FALSE );
    context = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context) );
    ok( context != NULL, "failed to allocate swap context.\n" );
    append_name( lock_path, root, APPX_CATALOG_LOCK_FILE_NAME );
    lock = CreateFileW( lock_path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( lock != INVALID_HANDLE_VALUE, "failed to open swap lock, error %lu.\n",
        GetLastError() );
    if (!snapshot || !context || lock == INVALID_HANDLE_VALUE) goto done;
    if (!LockFileEx( lock, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped ))
    {
        ok( 0, "failed to lock swap store, error %lu.\n", GetLastError() );
        goto done;
    }

    lstrcpyW( context->root, root );
    context->snapshot = snapshot;
    context->result = E_PENDING;
    context->started = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( context->started != NULL, "failed to create swap event, error %lu.\n",
        GetLastError() );
    if (!context->started) goto unlock;
    thread = CreateThread( NULL, 0, publish_thread, context, 0, NULL );
    ok( thread != NULL, "failed to create swap thread, error %lu.\n",
        GetLastError() );
    if (!thread) goto unlock;
    wait = WaitForSingleObject( context->started, 2000 );
    ok( wait == WAIT_OBJECT_0, "swap start wait returned %#lx.\n", wait );
    Sleep( 200 );
    ok( WaitForSingleObject( thread, 0 ) == WAIT_TIMEOUT,
        "publisher did not block on the store lock.\n" );

    if (!MoveFileExW( container, moved_container, 0 ))
    {
        win_skip( "cannot rename an open catalog %s, error %lu.\n",
                  ancestor ? "ancestor" : "root", GetLastError() );
        goto unlock;
    }
    ok( CreateDirectoryW( container, NULL ),
        "failed to create replacement container, error %lu.\n", GetLastError() );
    if (ancestor)
        ok( CreateDirectoryW( root, NULL ),
            "failed to create replacement root, error %lu.\n", GetLastError() );

unlock:
    if (lock != INVALID_HANDLE_VALUE)
        UnlockFileEx( lock, 0, 1, 0, &overlapped );
    if (thread)
    {
        wait = WaitForSingleObject( thread, TEST_THREAD_TIMEOUT );
        ok( wait == WAIT_OBJECT_0, "swap publisher wait returned %#lx.\n", wait );
        if (wait != WAIT_OBJECT_0)
        {
            TerminateThread( thread, 0 );
            wait = WaitForSingleObject( thread, 2000 );
            if (wait != WAIT_OBJECT_0)
            {
                /*
                 * Do not free memory or remove files that a regressed worker
                 * might still access.  The test process owns and reclaims
                 * these bounded resources when the suite exits.
                 */
                CloseHandle( thread );
                if (context->started) CloseHandle( context->started );
                if (lock != INVALID_HANDLE_VALUE) CloseHandle( lock );
                return;
            }
        }
        else if (GetFileAttributesW( moved_root ) != INVALID_FILE_ATTRIBUTES)
        {
            ok( SUCCEEDED(context->result) ||
                context->result == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE),
                "swap publish returned %#lx.\n", context->result );
            hr = appx_catalog_load( moved_root, &loaded );
            ok( hr == S_OK, "moved-root load returned %#lx.\n", hr );
            ok( appx_catalog_snapshot_get_epoch( loaded ) ==
                    (SUCCEEDED(context->result) ? 1 : 0),
                "moved root has epoch %s after publish result %#lx.\n",
                wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(loaded)),
                context->result );
            appx_catalog_snapshot_free( loaded );
            loaded = NULL;
            hr = appx_catalog_load( root, &loaded );
            ok( hr == S_OK, "replacement-root load returned %#lx.\n", hr );
            ok( appx_catalog_snapshot_get_epoch( loaded ) == 0,
                "replacement root was modified, epoch %s.\n",
                wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(loaded)) );
        }
    }

done:
    appx_catalog_snapshot_free( loaded );
    if (thread) CloseHandle( thread );
    if (context && context->started) CloseHandle( context->started );
    if (lock != INVALID_HANDLE_VALUE) CloseHandle( lock );
    appx_catalog_snapshot_free( snapshot );
    HeapFree( GetProcessHeap(), 0, context );
    cleanup_test_directory( root );
    if (lstrcmpW( moved_root, root )) cleanup_test_directory( moved_root );
    if (ancestor)
    {
        RemoveDirectoryW( container );
        RemoveDirectoryW( moved_container );
    }
}

static void test_store_handle_stability( void )
{
    run_store_swap_test( FALSE );
    run_store_swap_test( TRUE );
}

static void test_reparse_points( void )
{
    APPX_CATALOG_SNAPSHOT *loaded = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;
    WCHAR target[MAX_PATH], link[MAX_PATH], root[MAX_PATH], pending[MAX_PATH];
    WCHAR outside[MAX_PATH], temp[MAX_PATH];
    HANDLE file;
    HRESULT hr;

    ok( create_test_directory( target ), "failed to create reparse target.\n" );
    lstrcpyW( link, target );
    lstrcatW( link, L".link" );
    if (!CreateSymbolicLinkW( link, target,
                              SYMBOLIC_LINK_FLAG_DIRECTORY |
                              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE ))
    {
        win_skip( "directory symbolic links are unavailable, error %lu.\n",
                  GetLastError() );
    }
    else
    {
        hr = appx_catalog_load( link, &loaded );
        ok( FAILED(hr), "reparse root load returned %#lx.\n", hr );
        ok( !loaded, "reparse root returned snapshot %p.\n", loaded );
        if (!RemoveDirectoryW( link )) DeleteFileW( link );
    }
    cleanup_test_directory( target );

    ok( create_test_directory( root ), "failed to create reparse child root.\n" );
    if (!GetTempPathW( ARRAY_SIZE(temp), temp ) ||
        !GetTempFileNameW( temp, L"cto", 0, outside ))
    {
        ok( 0, "failed to create outside target, error %lu.\n", GetLastError() );
        cleanup_test_directory( root );
        return;
    }
    append_name( pending, root, APPX_CATALOG_PENDING_FILE_NAME );
    if (!CreateSymbolicLinkW( pending, outside,
                              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE ))
    {
        win_skip( "file symbolic links are unavailable, error %lu.\n",
                  GetLastError() );
    }
    else
    {
        loaded = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;
        hr = appx_catalog_load( root, &loaded );
        ok( FAILED(hr), "reparse pending load returned %#lx.\n", hr );
        ok( !loaded, "reparse pending returned snapshot %p.\n", loaded );
        file = CreateFileW( outside, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
        ok( file != INVALID_HANDLE_VALUE,
            "pending cleanup followed/deleted outside target, error %lu.\n",
            GetLastError() );
        if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
        DeleteFileW( pending );
    }
    DeleteFileW( outside );
    cleanup_test_directory( root );
}

static void test_pending_hardlink( void )
{
    APPX_CATALOG_SNAPSHOT *loaded = NULL;
    WCHAR root[MAX_PATH], pending[MAX_PATH], outside[MAX_PATH], temp[MAX_PATH];
    LARGE_INTEGER size;
    HANDLE file;
    DWORD written, attributes, error;
    HRESULT hr;

    ok( create_test_directory( root ), "failed to create hardlink root.\n" );
    if (!GetTempPathW( ARRAY_SIZE(temp), temp ) ||
        !GetTempFileNameW( temp, L"cth", 0, outside ))
    {
        ok( 0, "failed to create hardlink target, error %lu.\n", GetLastError() );
        cleanup_test_directory( root );
        return;
    }
    file = CreateFileW( outside, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to open hardlink target.\n" );
    if (file != INVALID_HANDLE_VALUE)
    {
        ok( WriteFile( file, "safe", 4, &written, NULL ) && written == 4,
            "failed to initialize hardlink target, error %lu.\n", GetLastError() );
        CloseHandle( file );
    }
    append_name( pending, root, APPX_CATALOG_PENDING_FILE_NAME );
    if (!CreateHardLinkW( pending, outside, NULL ))
    {
        win_skip( "hard links are unavailable, error %lu.\n", GetLastError() );
    }
    else
    {
        hr = appx_catalog_load( root, &loaded );
        ok( hr == S_OK, "load with stale pending hardlink returned %#lx.\n", hr );
        attributes = GetFileAttributesW( pending );
        error = GetLastError();
        ok( attributes == INVALID_FILE_ATTRIBUTES &&
            error == ERROR_FILE_NOT_FOUND,
            "stale pending hardlink was not removed, attributes %#lx, error %lu.\n",
            attributes, error );
        file = CreateFileW( outside, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
        ok( file != INVALID_HANDLE_VALUE,
            "pending cleanup deleted outside hardlink target, error %lu.\n",
            GetLastError() );
        if (file != INVALID_HANDLE_VALUE)
        {
            size.QuadPart = -1;
            ok( GetFileSizeEx( file, &size ) && size.QuadPart == 4,
                "pending cleanup changed outside target size to %s.\n",
                wine_dbgstr_longlong(size.QuadPart) );
            CloseHandle( file );
        }
    }
    appx_catalog_snapshot_free( loaded );
    DeleteFileW( outside );
    cleanup_test_directory( root );
}

static DWORD WINAPI writer_thread( void *arg )
{
    struct writer_context *context = arg;
    UINT32 attempt;

    for (attempt = 0; attempt < TEST_WRITER_RETRIES; attempt++)
    {
        APPX_CATALOG_SNAPSHOT *loaded = NULL, *replacement = NULL;
        struct appx_catalog_package package;
        HRESULT hr;
        UINT64 epoch;

        hr = appx_catalog_load( context->root, &loaded );
        if (FAILED(hr))
        {
            InterlockedCompareExchange( &context->failure, hr, S_OK );
            return 0;
        }
        epoch = appx_catalog_snapshot_get_epoch( loaded );
        init_package( &package, L"Contoso.Alpha",
                      L"Contoso.Alpha_1.2.3.4_x64__8wekyb3d8bbwe",
                      L"Contoso.Alpha_8wekyb3d8bbwe",
                      L"Packages\\Contoso.Alpha", APPX_CATALOG_PACKAGE_ACTIVE );
        hr = appx_catalog_snapshot_create( epoch + 1, &package, 1, &replacement );
        appx_catalog_snapshot_free( loaded );
        if (FAILED(hr))
        {
            InterlockedCompareExchange( &context->failure, hr, S_OK );
            return 0;
        }
        hr = appx_catalog_publish( context->root, epoch, replacement );
        appx_catalog_snapshot_free( replacement );
        if (SUCCEEDED(hr))
        {
            InterlockedIncrement( &context->successes );
            return 0;
        }
        if (hr != APPX_CATALOG_E_EPOCH_CONFLICT)
        {
            InterlockedCompareExchange( &context->failure, hr, S_OK );
            return 0;
        }
    }
    InterlockedCompareExchange( &context->failure,
                                APPX_CATALOG_E_EPOCH_CONFLICT, S_OK );
    return 0;
}

static void test_concurrent_writers( void )
{
    struct writer_context *context;
    HANDLE threads[TEST_WRITER_COUNT] = {0};
    APPX_CATALOG_SNAPSHOT *loaded = NULL;
    WCHAR root[MAX_PATH];
    DWORD wait;
    UINT32 count = 0, i;
    HRESULT hr;

    ok( create_test_directory( root ), "failed to create writer directory.\n" );
    if (!(context = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*context) )))
    {
        ok( 0, "failed to allocate writer context.\n" );
        cleanup_test_directory( root );
        return;
    }
    lstrcpyW( context->root, root );
    for (i = 0; i < ARRAY_SIZE(threads); i++)
    {
        threads[count] = CreateThread( NULL, 0, writer_thread, context, 0, NULL );
        ok( threads[count] != NULL, "failed to create writer thread %u.\n", i );
        if (threads[count]) count++;
    }
    wait = count ? WaitForMultipleObjects( count, threads, TRUE, 30000 ) :
                   WAIT_OBJECT_0;
    ok( wait == WAIT_OBJECT_0, "writer wait returned %#lx.\n", wait );
    if (wait != WAIT_OBJECT_0)
    {
        for (i = 0; i < count; i++) TerminateThread( threads[i], 0 );
        wait = WaitForMultipleObjects( count, threads, TRUE, 2000 );
        for (i = 0; i < count; i++) CloseHandle( threads[i] );
        if (wait == WAIT_OBJECT_0)
        {
            HeapFree( GetProcessHeap(), 0, context );
            cleanup_test_directory( root );
        }
        /* Otherwise workers may still hold the context or store lock. */
        return;
    }
    for (i = 0; i < count; i++)
        if (threads[i]) CloseHandle( threads[i] );

    ok( context->successes > 0, "no writer succeeded.\n" );
    ok( context->failure == S_OK, "writer failed with %#lx.\n", context->failure );
    hr = appx_catalog_load( root, &loaded );
    ok( hr == S_OK, "load after writers returned %#lx.\n", hr );
    ok( appx_catalog_snapshot_get_epoch( loaded ) == context->successes,
        "got epoch %s after %ld successful writers.\n",
        wine_dbgstr_longlong(appx_catalog_snapshot_get_epoch(loaded)),
        context->successes );

    appx_catalog_snapshot_free( loaded );
    HeapFree( GetProcessHeap(), 0, context );
    cleanup_test_directory( root );
}

static void test_arguments( void )
{
    APPX_CATALOG_SNAPSHOT *snapshot = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;
    HRESULT hr;

    hr = appx_catalog_load( NULL, &snapshot );
    ok( hr == E_INVALIDARG, "NULL root load returned %#lx.\n", hr );
    ok( !snapshot, "NULL root load returned snapshot %p.\n", snapshot );
    hr = appx_catalog_load( L"", &snapshot );
    ok( hr == E_INVALIDARG, "empty root load returned %#lx.\n", hr );
    hr = appx_catalog_load( L"C:\\missing\\root", NULL );
    ok( hr == E_INVALIDARG, "NULL load output returned %#lx.\n", hr );
    ok( appx_catalog_snapshot_get_epoch( NULL ) == 0, "NULL epoch was nonzero.\n" );
    ok( appx_catalog_snapshot_get_package_count( NULL ) == 0,
        "NULL package count was nonzero.\n" );
    ok( !appx_catalog_snapshot_get_package( NULL, 0 ),
        "NULL package lookup returned a package.\n" );
    snapshot = (APPX_CATALOG_SNAPSHOT *)0xdeadbeef;
    ok( appx_catalog_snapshot_deep_copy( NULL, &snapshot ) == E_INVALIDARG,
        "deep-copy accepted NULL source.\n" );
    ok( !snapshot, "invalid deep-copy returned snapshot %p.\n", snapshot );
    appx_catalog_snapshot_free( NULL );
}

START_TEST(catalog)
{
    HMODULE module = LoadLibraryW( L"appxsvc.dll" );

    if (!module)
    {
        ok( 0, "appxsvc.dll is unavailable, error %lu.\n", GetLastError() );
        return;
    }
    p_appx_catalog_snapshot_create =
        (void *)GetProcAddress( module, "appx_catalog_snapshot_create" );
    p_appx_catalog_snapshot_deep_copy =
        (void *)GetProcAddress( module, "appx_catalog_snapshot_deep_copy" );
    p_appx_catalog_snapshot_free =
        (void *)GetProcAddress( module, "appx_catalog_snapshot_free" );
    p_appx_catalog_snapshot_get_epoch =
        (void *)GetProcAddress( module, "appx_catalog_snapshot_get_epoch" );
    p_appx_catalog_snapshot_get_package_count =
        (void *)GetProcAddress( module, "appx_catalog_snapshot_get_package_count" );
    p_appx_catalog_snapshot_get_package =
        (void *)GetProcAddress( module, "appx_catalog_snapshot_get_package" );
    p_appx_catalog_load =
        (void *)GetProcAddress( module, "appx_catalog_load" );
    p_appx_catalog_publish =
        (void *)GetProcAddress( module, "appx_catalog_publish" );
    if (!p_appx_catalog_snapshot_create ||
        !p_appx_catalog_snapshot_deep_copy ||
        !p_appx_catalog_snapshot_free ||
        !p_appx_catalog_snapshot_get_epoch ||
        !p_appx_catalog_snapshot_get_package_count ||
        !p_appx_catalog_snapshot_get_package ||
        !p_appx_catalog_load || !p_appx_catalog_publish)
    {
        ok( 0, "AppX catalog exports are unavailable.\n" );
        FreeLibrary( module );
        return;
    }
    if (!load_bcrypt())
    {
        ok( 0, "bcrypt.dll exports are not available.\n" );
        FreeLibrary( module );
        return;
    }

    test_roundtrip();
    test_large_record_reallocation();
    test_missing_and_stale_pending();
    test_canonical_and_binary_boundaries();
    test_snapshot_validation();
    test_epoch_conflict_and_publish_failure();
    test_store_handle_stability();
    test_reparse_points();
    test_pending_hardlink();
    test_concurrent_writers();
    test_arguments();
    FreeLibrary( module );
}
