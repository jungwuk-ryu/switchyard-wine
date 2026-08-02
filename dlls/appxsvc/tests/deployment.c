/*
 * AppX package deployment transaction tests
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winioctl.h"
#include "winerror.h"
#include "winternl.h"

#define APPX_DEPLOYMENT_TESTING
#include "../deployment.h"
#include "wine/test.h"

#define TEST_TIMEOUT 10000
#define TEST_DEPLOYMENT_GENERATION_CHARS 64
#define TEST_DEPLOYMENT_MARKER_VERSION 1
#define TEST_DEPLOYMENT_MARKER_HEADER_SIZE 40
#define TEST_MARKER_VERSION_OFFSET 4
#define TEST_MARKER_CONTENT_ID_OFFSET 8
#define TEST_MARKER_FULL_NAME_OFFSET 40
#define TEST_PE_IMAGE_SIZE 1024
#define TEST_PE_NT_OFFSET  0x80

static HRESULT (WINAPI *p_appx_deployment_initialize)(
    const APPX_DEPLOYMENT_OPTIONS *, APPX_DEPLOYMENT_RESULT ** );
static HRESULT (WINAPI *p_appx_deployment_query)(
    const WCHAR *, const APPX_DEPLOYMENT_OPTIONS *, APPX_CATALOG_SNAPSHOT ** );
static HRESULT (WINAPI *p_appx_deployment_record_load)(
    const struct appx_catalog_package *, const APPX_DEPLOYMENT_OPTIONS *,
    APPX_DEPLOYMENT_RECORD ** );
static void (WINAPI *p_appx_deployment_record_free)(
    APPX_DEPLOYMENT_RECORD * );
static UINT32 (WINAPI *p_appx_deployment_record_get_loader_file_count)(
    const APPX_DEPLOYMENT_RECORD * );
static const struct appx_deployment_loader_file *(WINAPI
    *p_appx_deployment_record_get_loader_file)(
    const APPX_DEPLOYMENT_RECORD *, UINT32 );
static UINT32 (WINAPI *p_appx_deployment_record_get_inproc_class_count)(
    const APPX_DEPLOYMENT_RECORD * );
static const struct appx_deployment_inproc_class *(WINAPI
    *p_appx_deployment_record_get_inproc_class)(
    const APPX_DEPLOYMENT_RECORD *, UINT32 );
static UINT32 (WINAPI *p_appx_deployment_record_get_application_file_count)(
    const APPX_DEPLOYMENT_RECORD * );
static const struct appx_deployment_application_file *(WINAPI
    *p_appx_deployment_record_get_application_file)(
    const APPX_DEPLOYMENT_RECORD *, UINT32 );
static void (WINAPI *p_appx_deployment_result_free)(
    APPX_DEPLOYMENT_RESULT * );
static UINT32 (WINAPI *p_appx_deployment_result_get_flags)(
    const APPX_DEPLOYMENT_RESULT * );
static UINT64 (WINAPI *p_appx_deployment_result_get_catalog_epoch)(
    const APPX_DEPLOYMENT_RESULT * );
static HRESULT (WINAPI *p_appx_catalog_snapshot_create)(
    UINT64, const struct appx_catalog_package *, UINT32,
    APPX_CATALOG_SNAPSHOT ** );
static HRESULT (WINAPI *p_appx_catalog_snapshot_deep_copy)(
    const APPX_CATALOG_SNAPSHOT *, APPX_CATALOG_SNAPSHOT ** );
static void (WINAPI *p_appx_catalog_snapshot_free)(
    APPX_CATALOG_SNAPSHOT * );
static UINT64 (WINAPI *p_appx_catalog_snapshot_get_epoch)(
    const APPX_CATALOG_SNAPSHOT * );
static UINT32 (WINAPI *p_appx_catalog_snapshot_get_package_count)(
    const APPX_CATALOG_SNAPSHOT * );
static const struct appx_catalog_package *(WINAPI
    *p_appx_catalog_snapshot_get_package)(
    const APPX_CATALOG_SNAPSHOT *, UINT32 );
static HRESULT (WINAPI *p_appx_catalog_load)(
    const WCHAR *, APPX_CATALOG_SNAPSHOT ** );
static HRESULT (WINAPI *p_appx_catalog_publish)(
    const WCHAR *, UINT64, const APPX_CATALOG_SNAPSHOT * );
static HRESULT (WINAPI *p_appx_catalog_load_bounded)(
    const WCHAR *, DWORD, HANDLE, APPX_CATALOG_SNAPSHOT ** );
static HRESULT (WINAPI *p_appx_catalog_publish_bounded)(
    const WCHAR *, UINT64, const APPX_CATALOG_SNAPSHOT *, DWORD, HANDLE );
static HRESULT (WINAPI *p_appx_package_graph_create_with_classes)(
    const APPX_CATALOG_SNAPSHOT *, const WCHAR *, const WCHAR *,
    const WCHAR *, enum appx_catalog_architecture, UINT64,
    const struct appx_graph_file_identity *,
    const struct appx_graph_loader_file *, UINT32,
    const struct appx_graph_inproc_class *, UINT32, APPX_PACKAGE_GRAPH ** );
static HRESULT (WINAPI *p_appx_package_graph_create)(
    const APPX_CATALOG_SNAPSHOT *, const WCHAR *, const WCHAR *,
    const WCHAR *, enum appx_catalog_architecture, UINT64,
    const struct appx_graph_file_identity *,
    const struct appx_graph_loader_file *, UINT32, APPX_PACKAGE_GRAPH ** );
static void (WINAPI *p_appx_package_graph_free)( APPX_PACKAGE_GRAPH * );
static const BYTE *(WINAPI *p_appx_package_graph_get_blob)(
    const APPX_PACKAGE_GRAPH *, UINT32 * );
static UINT32 (WINAPI *p_appx_package_graph_get_package_count)(
    const APPX_PACKAGE_GRAPH * );
static HRESULT (WINAPI *p_appx_package_graph_get_package)(
    const APPX_PACKAGE_GRAPH *, UINT32, struct appx_graph_package * );
static HRESULT (WINAPI *p_appx_deployment_runtime_acquire)(
    const WCHAR *, const WCHAR *, const APPX_DEPLOYMENT_OPTIONS *,
    APPX_DEPLOYMENT_RUNTIME ** );
static void (WINAPI *p_appx_deployment_runtime_free)(
    APPX_DEPLOYMENT_RUNTIME * );
static const APPX_PACKAGE_GRAPH *(WINAPI
    *p_appx_deployment_runtime_get_graph)(
    const APPX_DEPLOYMENT_RUNTIME * );
static UINT32 (WINAPI *p_appx_deployment_runtime_get_lease_count)(
    const APPX_DEPLOYMENT_RUNTIME * );
static const HANDLE *(WINAPI *p_appx_deployment_runtime_get_leases)(
    const APPX_DEPLOYMENT_RUNTIME * );
static const struct wine_appx_graph_attach *(WINAPI
    *p_appx_deployment_runtime_get_attach)(
    const APPX_DEPLOYMENT_RUNTIME * );
static HANDLE (WINAPI *p_appx_deployment_runtime_get_executable_handle)(
    const APPX_DEPLOYMENT_RUNTIME * );
static const WCHAR *(WINAPI *p_appx_deployment_runtime_get_executable_path)(
    const APPX_DEPLOYMENT_RUNTIME * );
static const WCHAR *(WINAPI *p_appx_deployment_runtime_get_parameters)(
    const APPX_DEPLOYMENT_RUNTIME * );
static const WCHAR *(WINAPI *p_appx_deployment_runtime_get_current_directory)(
    const APPX_DEPLOYMENT_RUNTIME * );

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

void deployment_test_backend_set_imports(
    const struct deployment_test_backend_imports *imports );

#define appx_deployment_initialize p_appx_deployment_initialize
#define appx_deployment_query p_appx_deployment_query
#define appx_deployment_record_load p_appx_deployment_record_load
#define appx_deployment_record_free p_appx_deployment_record_free
#define appx_deployment_record_get_loader_file_count \
    p_appx_deployment_record_get_loader_file_count
#define appx_deployment_record_get_loader_file \
    p_appx_deployment_record_get_loader_file
#define appx_deployment_record_get_inproc_class_count \
    p_appx_deployment_record_get_inproc_class_count
#define appx_deployment_record_get_inproc_class \
    p_appx_deployment_record_get_inproc_class
#define appx_deployment_record_get_application_file_count \
    p_appx_deployment_record_get_application_file_count
#define appx_deployment_record_get_application_file \
    p_appx_deployment_record_get_application_file
#define appx_deployment_result_free p_appx_deployment_result_free
#define appx_deployment_result_get_flags p_appx_deployment_result_get_flags
#define appx_deployment_result_get_catalog_epoch \
    p_appx_deployment_result_get_catalog_epoch
#define appx_catalog_snapshot_create p_appx_catalog_snapshot_create
#define appx_catalog_snapshot_deep_copy p_appx_catalog_snapshot_deep_copy
#define appx_catalog_snapshot_free p_appx_catalog_snapshot_free
#define appx_catalog_snapshot_get_epoch p_appx_catalog_snapshot_get_epoch
#define appx_catalog_snapshot_get_package_count \
    p_appx_catalog_snapshot_get_package_count
#define appx_catalog_snapshot_get_package \
    p_appx_catalog_snapshot_get_package
#define appx_catalog_load p_appx_catalog_load
#define appx_catalog_publish p_appx_catalog_publish
#define appx_catalog_load_bounded p_appx_catalog_load_bounded
#define appx_catalog_publish_bounded p_appx_catalog_publish_bounded
#define appx_package_graph_create_with_classes \
    p_appx_package_graph_create_with_classes
#define appx_package_graph_create p_appx_package_graph_create
#define appx_package_graph_free p_appx_package_graph_free
#define appx_package_graph_get_blob p_appx_package_graph_get_blob
#define appx_package_graph_get_package_count \
    p_appx_package_graph_get_package_count
#define appx_package_graph_get_package p_appx_package_graph_get_package
#define appx_deployment_runtime_acquire p_appx_deployment_runtime_acquire
#define appx_deployment_runtime_free p_appx_deployment_runtime_free
#define appx_deployment_runtime_get_graph \
    p_appx_deployment_runtime_get_graph
#define appx_deployment_runtime_get_lease_count \
    p_appx_deployment_runtime_get_lease_count
#define appx_deployment_runtime_get_leases \
    p_appx_deployment_runtime_get_leases
#define appx_deployment_runtime_get_attach \
    p_appx_deployment_runtime_get_attach
#define appx_deployment_runtime_get_executable_handle \
    p_appx_deployment_runtime_get_executable_handle
#define appx_deployment_runtime_get_executable_path \
    p_appx_deployment_runtime_get_executable_path
#define appx_deployment_runtime_get_parameters \
    p_appx_deployment_runtime_get_parameters
#define appx_deployment_runtime_get_current_directory \
    p_appx_deployment_runtime_get_current_directory

struct fake_package
{
    const WCHAR *name;
    const WCHAR *full_name;
    const WCHAR *family_name;
    struct appx_catalog_version version;
    enum appx_catalog_architecture architecture;
    UINT32 package_flags;
    BYTE content_seed;
    const struct appx_catalog_dependency *dependencies;
    UINT32 dependency_count;
};

struct test_context
{
    struct fake_package package;
    enum appx_deployment_test_checkpoint fail_checkpoint;
    enum appx_deployment_test_checkpoint signal_checkpoint;
    HRESULT checkpoint_error;
    LONG checkpoint_hits[APPX_DEPLOYMENT_CHECKPOINT_CLEANED + 1];
    BOOL partial_writes;
    BOOL disk_full;
    BOOL weak_directory_flush;
    BOOL fail_directory_flush;
    BOOL generation_in_use;
    BOOL create_data_directory;
    BYTE random_seed;
    HANDLE cancel_event;
    HANDLE cancel_wait_start;
    HANDLE external_publish_start;
    HANDLE external_publish_done;
    const WCHAR *external_store_root;
    LONG external_publish_triggered;
    HRESULT external_publish_result;
    const struct appx_catalog_application *applications;
    UINT32 application_count;
};

static const struct appx_catalog_application test_applications[] =
{
    {L"App", L"bin\\App.exe", L"Windows.FullTrustApplication",
     L"", L"", APPX_CATALOG_ACTIVATION_FULL_TRUST},
};

static const struct appx_deployment_loader_file test_loaders[] =
{
    {L"bin\\App.dll"},
};

static const struct appx_deployment_inproc_class test_classes[] =
{
    {L"bin\\App.dll", L"Contoso.Component",
     APPX_MANIFEST_THREADING_BOTH},
};

static void init_fake_package( struct fake_package *package, UINT16 major,
                               BYTE seed )
{
    memset( package, 0, sizeof(*package) );
    package->name = L"Contoso.App";
    package->full_name = major == 1 ?
        L"Contoso.App_1.0.0.0_x64__8wekyb3d8bbwe" :
        L"Contoso.App_2.0.0.0_x64__8wekyb3d8bbwe";
    package->family_name = L"Contoso.App_8wekyb3d8bbwe";
    package->version.major = major;
    package->architecture = APPX_CATALOG_ARCHITECTURE_X64;
    package->package_flags = APPX_CATALOG_PACKAGE_ACTIVE |
                             APPX_CATALOG_PACKAGE_SIGNED;
    package->content_seed = seed;
}

static void init_options( APPX_DEPLOYMENT_OPTIONS *options,
                          const WCHAR *root )
{
    memset( options, 0, sizeof(*options) );
    options->size = sizeof(*options);
    options->version = APPX_DEPLOYMENT_OPTIONS_VERSION;
    options->store_root = root;
    options->flags = APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY;
    options->target_architecture = APPX_CATALOG_ARCHITECTURE_X64;
    options->writer_timeout_ms = 2000;
    options->max_epoch_retries = 4;
    options->max_gc_entries = 1024;
    options->max_gc_bytes = 16 * 1024 * 1024;
}

static BOOL make_test_root( WCHAR *root )
{
    WCHAR temp[MAX_PATH];

    if (!GetTempPathW( ARRAY_SIZE(temp), temp ) ||
        !GetTempFileNameW( temp, L"dpl", 0, root ))
        return FALSE;
    return DeleteFileW( root );
}

static BOOL append_path( WCHAR *output, SIZE_T capacity, const WCHAR *root,
                         const WCHAR *relative )
{
    SIZE_T root_length = lstrlenW( root );
    SIZE_T relative_length = lstrlenW( relative );
    BOOL separator = root_length && root[root_length - 1] != '\\';

    if (!capacity || !root_length ||
        root_length + separator + relative_length >= capacity)
    {
        if (capacity) output[0] = 0;
        return FALSE;
    }
    memcpy( output, root, root_length * sizeof(*output) );
    if (separator) output[root_length++] = '\\';
    memcpy( output + root_length, relative,
            (relative_length + 1) * sizeof(*output) );
    return TRUE;
}

static BOOL get_lease_generation_path(
    const WCHAR *root, const struct appx_catalog_package *package,
    WCHAR *path );
static BOOL verify_generation_marker_file( const WCHAR *path,
                                           const struct appx_catalog_package *package );

static void remove_tree( const WCHAR *path )
{
    WIN32_FIND_DATAW data;
    WCHAR pattern[MAX_PATH * 2], child[MAX_PATH * 2];
    HANDLE find;
    DWORD attributes = GetFileAttributesW( path );

    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        SetFileAttributesW( path, FILE_ATTRIBUTE_NORMAL );
        DeleteFileW( path );
        return;
    }
    if (!append_path( pattern, ARRAY_SIZE(pattern), path, L"*" )) return;
    find = FindFirstFileW( pattern, &data );
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!lstrcmpW( data.cFileName, L"." ) ||
                !lstrcmpW( data.cFileName, L".." ))
                continue;
            if (!append_path( child, ARRAY_SIZE(child), path,
                              data.cFileName ))
                continue;
            remove_tree( child );
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    RemoveDirectoryW( path );
}

static HRESULT create_child( HANDLE root, const WCHAR *name, BOOL directory,
                             HANDLE *handle )
{
    UNICODE_STRING string;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    *handle = INVALID_HANDLE_VALUE;
    RtlInitUnicodeString( &string, name );
    InitializeObjectAttributes( &attributes, &string, OBJ_CASE_INSENSITIVE,
                                root, NULL );
    status = NtCreateFile(
        handle, GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE,
        &attributes, &io, NULL,
        directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_CREATE, (directory ? FILE_DIRECTORY_FILE :
                                  FILE_NON_DIRECTORY_FILE) |
        FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL, 0 );
    return status ? HRESULT_FROM_WIN32( RtlNtStatusToDosError(status) ) : S_OK;
}

static HRESULT write_test_pe( HANDLE file, BOOL dll )
{
    BYTE image[TEST_PE_IMAGE_SIZE];
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    IMAGE_FILE_HEADER *file_header;
    IMAGE_OPTIONAL_HEADER64 *optional;
    BYTE *nt;
    DWORD written;
    UINT32 size;

    memset( image, 0, sizeof(image) );
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = TEST_PE_NT_OFFSET;
    nt = image + TEST_PE_NT_OFFSET;
    *(DWORD *)nt = IMAGE_NT_SIGNATURE;
    file_header = (IMAGE_FILE_HEADER *)(nt + sizeof(DWORD));
    file_header->Machine = IMAGE_FILE_MACHINE_AMD64;
    file_header->NumberOfSections = 1;
    file_header->SizeOfOptionalHeader =
        FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64, DataDirectory);
    file_header->Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE |
        (dll ? IMAGE_FILE_DLL : 0);
    optional = (IMAGE_OPTIONAL_HEADER64 *)(file_header + 1);
    optional->Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    size = (BYTE *)optional - image + file_header->SizeOfOptionalHeader +
        sizeof(IMAGE_SECTION_HEADER);

    if (!WriteFile( file, image, size, &written, NULL ) || written != size)
        return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

static HRESULT WINAPI prepare_package_callback(
    void *opaque, HANDLE file,
    enum appx_catalog_architecture architecture,
    APPX_DEPLOYMENT_TEST_PACKAGE *output )
{
    struct test_context *context = opaque;
    struct appx_catalog_package *package = &output->package;
    UINT32 i;

    UNREFERENCED_PARAMETER( file );
    UNREFERENCED_PARAMETER( architecture );
    memset( output, 0, sizeof(*output) );
    output->size = sizeof(*output);
    output->version = APPX_DEPLOYMENT_TEST_PACKAGE_VERSION;
    package->name = context->package.name;
    package->publisher = L"CN=Contoso";
    package->resource_id = L"";
    package->publisher_id = L"8wekyb3d8bbwe";
    package->full_name = context->package.full_name;
    package->family_name = context->package.family_name;
    package->version = context->package.version;
    package->architecture = context->package.architecture;
    package->flags = context->package.package_flags;
    for (i = 0; i < ARRAY_SIZE(package->content_id); i++)
        package->content_id[i] = context->package.content_seed + i;
    if (!(package->flags & APPX_CATALOG_PACKAGE_FRAMEWORK))
    {
        package->application_count = context->application_count ?
            context->application_count : ARRAY_SIZE(test_applications);
        package->applications = context->applications ?
            context->applications : test_applications;
    }
    package->dependency_count = context->package.dependency_count;
    package->dependencies = context->package.dependencies;
    output->loader_file_count = ARRAY_SIZE(test_loaders);
    output->loader_files = test_loaders;
    output->inproc_class_count = ARRAY_SIZE(test_classes);
    output->inproc_classes = test_classes;
    return S_OK;
}

static HRESULT WINAPI extract_callback(
    void *opaque, HANDLE staging, const APPX_EXTRACT_OPTIONS *options )
{
    struct test_context *context = opaque;
    HANDLE bin = INVALID_HANDLE_VALUE, data = INVALID_HANDLE_VALUE;
    HANDLE file = INVALID_HANDLE_VALUE;
    HRESULT hr;

    UNREFERENCED_PARAMETER( options );
    if (context->cancel_event &&
        WaitForSingleObject( context->cancel_event, 0 ) == WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    if (FAILED(hr = create_child( staging, L"bin", TRUE, &bin )) ||
        FAILED(hr = create_child( bin, L"App.dll", FALSE, &file )))
        goto done;
    if (FAILED(hr = write_test_pe( file, TRUE ))) goto done;
    CloseHandle( file );
    file = INVALID_HANDLE_VALUE;
    if (FAILED(hr = create_child( bin, L"App.exe", FALSE, &file )))
        goto done;
    hr = write_test_pe( file, FALSE );
    if (FAILED(hr)) goto done;
    if (context->create_data_directory)
        hr = create_child( staging, L"data", TRUE, &data );

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    if (data != INVALID_HANDLE_VALUE) CloseHandle( data );
    if (bin != INVALID_HANDLE_VALUE) CloseHandle( bin );
    return hr;
}

static HRESULT WINAPI checkpoint_callback(
    void *opaque, enum appx_deployment_test_checkpoint checkpoint )
{
    struct test_context *context = opaque;

    if (checkpoint <= APPX_DEPLOYMENT_CHECKPOINT_CLEANED)
        InterlockedIncrement( context->checkpoint_hits + checkpoint );
    if (checkpoint == (context->signal_checkpoint ?
                       context->signal_checkpoint :
                       APPX_DEPLOYMENT_CHECKPOINT_EXTRACTED) &&
        context->cancel_wait_start)
        SetEvent( context->cancel_wait_start );
    if (checkpoint == APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PREPARED &&
        context->external_publish_start &&
        !InterlockedCompareExchange(
            &context->external_publish_triggered, 1, 0 ))
    {
        DWORD wait;

        if (!SetEvent( context->external_publish_start ))
            return HRESULT_FROM_WIN32( GetLastError() );
        wait = WaitForSingleObject(
            context->external_publish_done, TEST_TIMEOUT );
        if (wait == WAIT_TIMEOUT) return HRESULT_FROM_WIN32( ERROR_TIMEOUT );
        if (wait != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32( GetLastError() );
        if (FAILED(context->external_publish_result))
            return context->external_publish_result;
    }
    if (checkpoint == context->fail_checkpoint)
        return context->checkpoint_error;
    return S_OK;
}

static BOOL WINAPI write_callback( void *opaque, HANDLE file,
                                   const void *data, DWORD size,
                                   DWORD *written )
{
    struct test_context *context = opaque;

    if (context->disk_full)
    {
        *written = 0;
        SetLastError( ERROR_DISK_FULL );
        return FALSE;
    }
    if (context->partial_writes && size > 3) size = 3;
    return WriteFile( file, data, size, written, NULL );
}

static HRESULT WINAPI flush_callback( void *opaque, HANDLE handle,
                                      BOOL directory )
{
    struct test_context *context = opaque;

    UNREFERENCED_PARAMETER( handle );
    if (directory && context->fail_directory_flush)
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    if (directory && context->weak_directory_flush) return S_FALSE;
    return S_OK;
}

static HRESULT WINAPI in_use_callback( void *opaque,
                                       const WCHAR *generation,
                                       BOOL *in_use )
{
    struct test_context *context = opaque;

    UNREFERENCED_PARAMETER( generation );
    *in_use = context->generation_in_use;
    return S_OK;
}

static HRESULT WINAPI random_callback( void *opaque, BYTE *data, UINT32 size )
{
    struct test_context *context = opaque;
    UINT32 i;

    for (i = 0; i < size; i++) data[i] = ++context->random_seed;
    return S_OK;
}

static void init_backend( APPX_DEPLOYMENT_TEST_BACKEND *backend,
                          struct test_context *context )
{
    memset( backend, 0, sizeof(*backend) );
    backend->size = sizeof(*backend);
    backend->version = APPX_DEPLOYMENT_TEST_BACKEND_VERSION;
    backend->context = context;
    backend->prepare_package = prepare_package_callback;
    backend->extract = extract_callback;
    backend->checkpoint = checkpoint_callback;
    backend->write = write_callback;
    backend->flush = flush_callback;
    backend->generation_in_use = in_use_callback;
    backend->random = random_callback;
}

static HANDLE create_dummy_package_file( WCHAR *path )
{
    WCHAR temp[MAX_PATH];

    if (!GetTempPathW( ARRAY_SIZE(temp), temp ) ||
        !GetTempFileNameW( temp, L"pkg", 0, path ))
        return INVALID_HANDLE_VALUE;
    return CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
}

static HRESULT execute_package(
    enum appx_deployment_operation operation, HANDLE package_file,
    const WCHAR *full_name, APPX_DEPLOYMENT_OPTIONS *options,
    struct test_context *context, APPX_DEPLOYMENT_RESULT **result )
{
    APPX_DEPLOYMENT_TEST_BACKEND backend;

    init_backend( &backend, context );
    return appx_deployment_execute_with_test_backend(
        operation, package_file, full_name, options, &backend, result );
}

static HRESULT execute_package_with_production_lease_check(
    enum appx_deployment_operation operation, HANDLE package_file,
    const WCHAR *full_name, APPX_DEPLOYMENT_OPTIONS *options,
    struct test_context *context, APPX_DEPLOYMENT_RESULT **result )
{
    APPX_DEPLOYMENT_TEST_BACKEND backend;

    init_backend( &backend, context );
    backend.generation_in_use = NULL;
    return appx_deployment_execute_with_test_backend(
        operation, package_file, full_name, options, &backend, result );
}

static DWORD WINAPI external_catalog_publish_thread( void *opaque )
{
    struct test_context *context = opaque;
    APPX_CATALOG_SNAPSHOT *current = NULL, *replacement = NULL;
    struct appx_catalog_package *packages = NULL;
    UINT64 epoch;
    UINT32 count = 0, i;
    DWORD wait;
    HRESULT hr;

    wait = WaitForSingleObject( context->external_publish_start,
                                TEST_TIMEOUT );
    if (wait == WAIT_TIMEOUT)
    {
        hr = HRESULT_FROM_WIN32( ERROR_TIMEOUT );
        goto done;
    }
    if (wait != WAIT_OBJECT_0)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (FAILED(hr = appx_catalog_load(
            context->external_store_root, &current )))
        goto done;
    epoch = appx_catalog_snapshot_get_epoch( current );
    count = appx_catalog_snapshot_get_package_count( current );
    if (count &&
        !(packages = HeapAlloc(
            GetProcessHeap(), 0, count * sizeof(*packages) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    for (i = 0; i < count; i++)
    {
        const struct appx_catalog_package *package =
            appx_catalog_snapshot_get_package( current, i );

        if (!package)
        {
            hr = APPX_DEPLOYMENT_E_CORRUPT_STORE;
            goto done;
        }
        packages[i] = *package;
    }
    if (FAILED(hr = appx_catalog_snapshot_create(
            epoch + 1, packages, count, &replacement )))
        goto done;
    hr = appx_catalog_publish(
        context->external_store_root, epoch, replacement );

done:
    context->external_publish_result = hr;
    appx_catalog_snapshot_free( replacement );
    appx_catalog_snapshot_free( current );
    HeapFree( GetProcessHeap(), 0, packages );
    SetEvent( context->external_publish_done );
    return 0;
}

struct cancel_signal_context
{
    HANDLE start;
    HANDLE cancel;
};

static DWORD WINAPI cancel_signal_thread( void *opaque )
{
    struct cancel_signal_context *context = opaque;

    if (WaitForSingleObject( context->start, TEST_TIMEOUT ) == WAIT_OBJECT_0)
    {
        Sleep( 30 );
        SetEvent( context->cancel );
    }
    return 0;
}

static HANDLE lock_store_byte( const WCHAR *root, DWORD offset,
                               OVERLAPPED *overlapped )
{
    WCHAR path[MAX_PATH * 2];
    HANDLE file;

    if (!append_path( path, ARRAY_SIZE(path), root, L"store.lock" ))
        return INVALID_HANDLE_VALUE;
    file = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return file;
    memset( overlapped, 0, sizeof(*overlapped) );
    overlapped->Offset = offset;
    if (!LockFileEx( file, LOCKFILE_EXCLUSIVE_LOCK |
                     LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, overlapped ))
    {
        CloseHandle( file );
        return INVALID_HANDLE_VALUE;
    }
    return file;
}

static HANDLE lock_deployment_writer( const WCHAR *root,
                                      OVERLAPPED *overlapped )
{
    return lock_store_byte( root, 1, overlapped );
}

static HANDLE lock_catalog_writer( const WCHAR *root,
                                   OVERLAPPED *overlapped )
{
    return lock_store_byte( root, 0, overlapped );
}

static void test_arguments_and_durability( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = (void *)0xdeadbeef;
    APPX_DEPLOYMENT_TEST_BACKEND backend;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    HANDLE package_file;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x10 );
    ok( make_test_root( root ), "failed to make root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    ok( package_file != INVALID_HANDLE_VALUE, "failed to create package file.\n" );

    options.size--;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == E_INVALIDARG, "invalid size returned %#lx.\n", hr );
    ok( !result, "result was not cleared.\n" );
    options.size = sizeof(options);
    options.store_root = L"C:\\safe\\..\\escape";
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == E_INVALIDARG, "traversal root returned %#lx.\n", hr );

    options.store_root = root;
    options.flags = 0;
    context.weak_directory_flush = TRUE;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        "non-opt-in weak flush returned %#lx.\n", hr );
    ok( !result, "weak failure returned a result.\n" );
    remove_tree( root );

    ok( make_test_root( root ), "failed to make second root.\n" );
    options.store_root = root;
    options.flags = APPX_DEPLOYMENT_ACCEPT_WEAK_DURABILITY;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == S_FALSE, "opt-in weak flush returned %#lx.\n", hr );
    ok( result && (appx_deployment_result_get_flags(result) &
                   APPX_DEPLOYMENT_RESULT_WEAK_DURABILITY),
        "weak result flag is missing.\n" );
    appx_deployment_result_free( result );
    result = NULL;
    remove_tree( root );

    ok( make_test_root( root ), "failed to make third root.\n" );
    init_options( &options, root );
    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x10 );
    context.fail_directory_flush = TRUE;
    init_backend( &backend, &context );
    hr = appx_deployment_execute_with_test_backend(
        APPX_DEPLOYMENT_OPERATION_INSTALL, package_file, NULL,
        &options, &backend, &result );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL),
        "unexpected flush failure returned %#lx.\n", hr );
    ok( !result, "flush failure returned a result.\n" );

    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_install_update_remove_and_record( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    APPX_DEPLOYMENT_RECORD *record = NULL;
    const struct appx_catalog_package *package;
    const struct appx_deployment_loader_file *loader;
    const struct appx_deployment_inproc_class *class;
    const struct appx_deployment_application_file *application;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH], lease_path[MAX_PATH * 2];
    HANDLE package_file;
    UINT32 extracted;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x20 );
    ok( make_test_root( root ), "failed to make lifecycle root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    ok( package_file != INVALID_HANDLE_VALUE, "failed to create package file.\n" );

    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "pristine-store query returned %#lx.\n", hr );
    ok( snapshot && !appx_catalog_snapshot_get_package_count(snapshot),
        "pristine-store query returned installed packages.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;

    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "install returned %#lx.\n", hr );
    ok( result && appx_deployment_result_get_catalog_epoch(result) == 1,
        "install epoch is wrong.\n" );
    ok( result && (appx_deployment_result_get_flags(result) &
                   APPX_DEPLOYMENT_RESULT_CATALOG_CHANGED),
        "install did not report catalog publication.\n" );
    appx_deployment_result_free( result );
    result = NULL;

    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "query returned %#lx.\n", hr );
    ok( snapshot && appx_catalog_snapshot_get_package_count(snapshot) == 1,
        "query package count is wrong.\n" );
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    ok( package && !lstrcmpW( package->full_name,
                              context.package.full_name ),
        "installed identity is wrong.\n" );
    ok( package && package->application_count == 1,
        "catalog application count is wrong.\n" );
    ok( package && package->application_count == 1 &&
        !lstrcmpW( package->applications[0].entry_point,
                   L"Windows.FullTrustApplication" ),
        "catalog entry point is wrong.\n" );
    ok( package && package->application_count == 1 &&
        !lstrcmpW( package->applications[0].parameters, L"" ),
        "catalog parameters are wrong.\n" );
    ok( package && package->application_count == 1 && !lstrcmpW(
            package->applications[0].current_directory_path, L"" ),
        "catalog current directory is wrong.\n" );
    ok( package && package->application_count == 1 &&
        package->applications[0].activation_kind ==
            APPX_CATALOG_ACTIVATION_FULL_TRUST,
        "catalog activation kind is wrong.\n" );
    ok( package && get_lease_generation_path( root, package, lease_path ) &&
        verify_generation_marker_file( lease_path, package ),
        "generation marker is missing or invalid.\n" );
    hr = appx_deployment_record_load( package, &options, &record );
    ok( hr == S_OK, "record load returned %#lx.\n", hr );
    ok( appx_deployment_record_get_loader_file_count(record) == 1,
        "loader count is wrong.\n" );
    loader = appx_deployment_record_get_loader_file( record, 0 );
    ok( loader && !lstrcmpW( loader->relative_path, L"bin\\App.dll" ),
        "loader path is wrong.\n" );
    ok( loader && (loader->identity.file_index_high ||
                   loader->identity.file_index_low),
        "loader identity is missing.\n" );
    ok( appx_deployment_record_get_inproc_class_count(record) == 1,
        "class count is wrong.\n" );
    class = appx_deployment_record_get_inproc_class( record, 0 );
    ok( class && !lstrcmpW( class->activatable_class_id,
                            L"Contoso.Component" ),
        "class id is wrong.\n" );
    ok( class && (class->identity.file_index_high ||
                  class->identity.file_index_low),
        "class identity is missing.\n" );
    ok( appx_deployment_record_get_application_file_count(record) == 1,
        "application file count is wrong.\n" );
    application = appx_deployment_record_get_application_file( record, 0 );
    ok( application && !lstrcmpW( application->id, L"App" ),
        "application id is wrong.\n" );
    ok( application && !lstrcmpW( application->executable,
                                  L"bin\\App.exe" ),
        "application executable is wrong.\n" );
    ok( application && !lstrcmpW( application->entry_point,
                                  L"Windows.FullTrustApplication" ),
        "application entry point is wrong.\n" );
    ok( application && !lstrcmpW( application->parameters, L"" ),
        "application parameters are wrong.\n" );
    ok( application && !lstrcmpW(
            application->current_directory_path, L"" ),
        "application current directory is wrong.\n" );
    ok( application && application->activation_kind ==
            APPX_CATALOG_ACTIVATION_FULL_TRUST,
        "application activation kind is wrong.\n" );
    ok( application && (application->identity.file_index_high ||
                        application->identity.file_index_low),
        "application identity is missing.\n" );
    appx_deployment_record_free( record );
    record = NULL;
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;

    extracted = context.checkpoint_hits[
        APPX_DEPLOYMENT_CHECKPOINT_EXTRACTED];
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "idempotent install returned %#lx.\n", hr );
    ok( result && (appx_deployment_result_get_flags(result) &
                   APPX_DEPLOYMENT_RESULT_PAYLOAD_REUSED),
        "idempotent install did not report reuse.\n" );
    ok( context.checkpoint_hits[APPX_DEPLOYMENT_CHECKPOINT_EXTRACTED] ==
        extracted, "idempotent install extracted again.\n" );
    appx_deployment_result_free( result );
    result = NULL;

    context.package.content_seed++;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == APPX_DEPLOYMENT_E_CONTENT_CONFLICT,
        "same-name different-content returned %#lx.\n", hr );
    ok( !result, "content conflict returned a result.\n" );
    context.package.content_seed--;

    init_fake_package( &context.package, 2, 0x40 );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_UPDATE, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "update returned %#lx.\n", hr );
    ok( result && appx_deployment_result_get_catalog_epoch(result) == 2,
        "update epoch is wrong.\n" );
    appx_deployment_result_free( result );
    result = NULL;

    init_fake_package( &context.package, 1, 0x20 );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_UPDATE, package_file,
                          NULL, &options, &context, &result );
    ok( hr == APPX_DEPLOYMENT_E_DOWNGRADE,
        "downgrade returned %#lx.\n", hr );
    ok( !result, "downgrade returned a result.\n" );

    context.generation_in_use = TRUE;
    hr = execute_package(
        APPX_DEPLOYMENT_OPERATION_REMOVE, INVALID_HANDLE_VALUE,
        L"Contoso.App_2.0.0.0_x64__8wekyb3d8bbwe",
        &options, &context, &result );
    ok( SUCCEEDED(hr), "remove returned %#lx.\n", hr );
    ok( result && (appx_deployment_result_get_flags(result) &
                   APPX_DEPLOYMENT_RESULT_GC_DEFERRED),
        "in-use generation was not deferred.\n" );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "post-remove query returned %#lx.\n", hr );
    ok( snapshot && !appx_catalog_snapshot_get_package_count(snapshot),
        "removed package remains active.\n" );

    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    context.generation_in_use = FALSE;
    hr = execute_package(
        APPX_DEPLOYMENT_OPERATION_GARBAGE_COLLECT, INVALID_HANDLE_VALUE,
        NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "deferred-generation collection returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;

    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static BOOL get_record_path( const WCHAR *root,
                             const struct appx_catalog_package *package,
                             WCHAR *path )
{
    const WCHAR *generation = wcsrchr( package->payload_path, '\\' );
    WCHAR relative[96];
    SIZE_T generation_length;

    if (!generation) return FALSE;
    generation_length = lstrlenW( generation + 1 );
    if (generation_length + 13 > ARRAY_SIZE(relative)) return FALSE;
    memcpy( relative, L"records\\", 8 * sizeof(*relative) );
    memcpy( relative + 8, generation + 1,
            generation_length * sizeof(*relative) );
    memcpy( relative + 8 + generation_length, L".bin",
            5 * sizeof(*relative) );
    return append_path( path, MAX_PATH * 2, root, relative );
}

static const WCHAR *payload_generation_name(
    const struct appx_catalog_package *package )
{
    const WCHAR *generation = wcsrchr( package->payload_path, '\\' );

    return generation ? generation + 1 : NULL;
}

static BOOL get_payload_file_path( const WCHAR *root,
                                   const struct appx_catalog_package *package,
                                   const WCHAR *relative, WCHAR *path )
{
    WCHAR payload[MAX_PATH * 2];

    return append_path( payload, ARRAY_SIZE(payload), root,
                        package->payload_path ) &&
           append_path( path, MAX_PATH * 2, payload, relative );
}

static BOOL get_payload_directory_path(
    const WCHAR *root, const struct appx_catalog_package *package,
    WCHAR *path )
{
    return append_path( path, MAX_PATH * 2, root, package->payload_path );
}

static BOOL get_lease_generation_path(
    const WCHAR *root, const struct appx_catalog_package *package,
    WCHAR *path )
{
    const WCHAR *generation = payload_generation_name( package );
    WCHAR relative[ARRAY_SIZE(L"leases\\generations\\") +
                   TEST_DEPLOYMENT_GENERATION_CHARS];

    if (!generation ||
        lstrlenW( generation ) != TEST_DEPLOYMENT_GENERATION_CHARS)
        return FALSE;
    lstrcpyW( relative, L"leases\\generations\\" );
    lstrcatW( relative, generation );
    return append_path( path, MAX_PATH * 2, root, relative );
}

static BOOL verify_generation_marker_file(
    const WCHAR *path, const struct appx_catalog_package *package )
{
    static const BYTE magic[4] = {'S','W','L','M'};
    BYTE header[TEST_DEPLOYMENT_MARKER_HEADER_SIZE];
    LARGE_INTEGER size;
    WCHAR *full_name = NULL;
    HANDLE file;
    UINT32 version, full_name_bytes;
    DWORD read;
    BOOL ret = FALSE;

    file = CreateFileW( path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                        FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE,
        "failed to open generation marker %s, error %lu.\n",
        wine_dbgstr_w(path), GetLastError() );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    full_name_bytes = (lstrlenW( package->full_name ) + 1) *
                      sizeof(*package->full_name);
    if (!(full_name = HeapAlloc( GetProcessHeap(), 0, full_name_bytes )))
        goto done;
    ret = GetFileSizeEx( file, &size ) &&
          size.QuadPart == sizeof(header) + full_name_bytes &&
          ReadFile( file, header, sizeof(header), &read, NULL ) &&
          read == sizeof(header) &&
          ReadFile( file, full_name, full_name_bytes, &read, NULL ) &&
          read == full_name_bytes;
    version = header[TEST_MARKER_VERSION_OFFSET] |
              (header[TEST_MARKER_VERSION_OFFSET + 1] << 8) |
              (header[TEST_MARKER_VERSION_OFFSET + 2] << 16) |
              (header[TEST_MARKER_VERSION_OFFSET + 3] << 24);
    ret = ret && !memcmp( header, magic, sizeof(magic) ) &&
          version == TEST_DEPLOYMENT_MARKER_VERSION &&
          !memcmp( header + TEST_MARKER_CONTENT_ID_OFFSET,
                   package->content_id, sizeof(package->content_id) ) &&
          !memcmp( full_name, package->full_name, full_name_bytes );
    ok( ret, "generation marker contents are wrong.\n" );
done:
    HeapFree( GetProcessHeap(), 0, full_name );
    CloseHandle( file );
    return ret;
}

static BOOL replace_record_string( const WCHAR *path, const WCHAR *needle,
                                   const WCHAR *replacement )
{
    UINT32 needle_size = (lstrlenW( needle ) + 1) * sizeof(WCHAR);
    BYTE *data = NULL;
    HANDLE file;
    DWORD size, read, written, i;
    BOOL ret = FALSE;

    ok( lstrlenW( needle ) == lstrlenW( replacement ),
        "replacement string length differs.\n" );
    file = CreateFileW( path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to open record %s.\n",
        wine_dbgstr_w(path) );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize( file, NULL );
    if (size == INVALID_FILE_SIZE ||
        !(data = HeapAlloc( GetProcessHeap(), 0, size )))
        goto done;
    if (!ReadFile( file, data, size, &read, NULL ) || read != size)
        goto done;
    for (i = 0; i + needle_size <= size; i++)
    {
        if (memcmp( data + i, needle, needle_size )) continue;
        SetFilePointer( file, i, NULL, FILE_BEGIN );
        ret = WriteFile( file, replacement, needle_size, &written, NULL ) &&
              written == needle_size;
        break;
    }
    ok( ret, "failed to replace record string %s.\n",
        wine_dbgstr_w(needle) );

done:
    HeapFree( GetProcessHeap(), 0, data );
    CloseHandle( file );
    return ret;
}

static BOOL path_ends_with( const WCHAR *path, const WCHAR *suffix )
{
    SIZE_T path_length, suffix_length;

    if (!path || !suffix) return FALSE;
    path_length = lstrlenW( path );
    suffix_length = lstrlenW( suffix );
    return path_length >= suffix_length &&
           !lstrcmpW( path + path_length - suffix_length, suffix );
}

static BOOL object_id_is_nonzero( const BYTE *object_id )
{
    UINT32 i;

    for (i = 0; i < WINE_APPX_GRAPH_OBJECT_ID_SIZE; i++)
        if (object_id[i]) return TRUE;
    return FALSE;
}

static void check_runtime_graph_object_ids(
    const APPX_DEPLOYMENT_RUNTIME *runtime )
{
    FILE_OBJECTID_BUFFER executable_id;
    const APPX_PACKAGE_GRAPH *graph;
    const BYTE *blob;
    IO_STATUS_BLOCK io;
    HANDLE executable;
    NTSTATUS status;
    UINT32 blob_size, loader_count, loaders_offset, i;

    if (!runtime) return;
    graph = appx_deployment_runtime_get_graph( runtime );
    blob = appx_package_graph_get_blob( graph, &blob_size );
    executable = appx_deployment_runtime_get_executable_handle( runtime );
    ok( blob && blob_size >= WINE_APPX_GRAPH_BLOB_HEADER_SIZE,
        "runtime graph blob is missing or truncated.\n" );
    if (!blob || blob_size < WINE_APPX_GRAPH_BLOB_HEADER_SIZE) return;

    memset( &executable_id, 0, sizeof(executable_id) );
    status = NtFsControlFile(
        executable, 0, NULL, NULL, &io, FSCTL_GET_OBJECT_ID, NULL, 0,
        &executable_id, sizeof(executable_id) );
    ok( !status, "executable object-id query returned %#lx.\n", status );
    if (!status)
        ok( !memcmp(
                blob + WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET,
                executable_id.ObjectId, WINE_APPX_GRAPH_OBJECT_ID_SIZE ),
            "runtime graph executable object id does not match its handle.\n" );

    loader_count = wine_appx_graph_read_u32( blob + 52 );
    loaders_offset = wine_appx_graph_read_u32( blob + 56 );
    ok( loaders_offset <= blob_size &&
        loader_count <=
            (blob_size - loaders_offset) /
                WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE,
        "runtime graph loader range is invalid.\n" );
    if (loaders_offset > blob_size ||
        loader_count >
            (blob_size - loaders_offset) /
                WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE)
        return;
    for (i = 0; i < loader_count; i++)
        ok( object_id_is_nonzero(
                blob + loaders_offset +
                i * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE +
                WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET ),
            "runtime loader %u has no object id.\n", i );
}

static BOOL prepare_orphaned_generation(
    APPX_DEPLOYMENT_OPTIONS *options, HANDLE package_file,
    struct test_context *context, WCHAR *payload_path, WCHAR *lease_path )
{
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    const struct appx_catalog_package *package;
    UINT32 saved_flags = options->flags;
    HRESULT hr;
    BOOL ret;

    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, options, context, &result );
    ok( SUCCEEDED(hr), "orphan setup install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, options, &snapshot );
    ok( SUCCEEDED(hr), "orphan setup query returned %#lx.\n", hr );
    package = snapshot ? appx_catalog_snapshot_get_package( snapshot, 0 ) : NULL;
    ret = package &&
          get_payload_directory_path( options->store_root, package,
                                      payload_path ) &&
          get_lease_generation_path( options->store_root, package, lease_path );
    ok( ret, "failed to derive orphan paths.\n" );
    appx_catalog_snapshot_free( snapshot );
    if (!ret) return FALSE;

    options->flags |= APPX_DEPLOYMENT_SKIP_GARBAGE_COLLECTION;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_REMOVE,
                          INVALID_HANDLE_VALUE,
                          context->package.full_name, options,
                          context, &result );
    options->flags = saved_flags;
    ok( SUCCEEDED(hr), "orphan setup removal returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    return SUCCEEDED(hr);
}

static void check_rejected_application_metadata(
    const WCHAR *name, const WCHAR *parameters,
    const WCHAR *current_directory_path )
{
    struct appx_catalog_application application =
    {
        L"App", L"bin\\App.exe", L"Windows.FullTrustApplication",
        parameters, current_directory_path,
        APPX_CATALOG_ACTIVATION_FULL_TRUST
    };
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = (void *)0xdeadbeef;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    HANDLE package_file;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x55 );
    context.applications = &application;
    context.application_count = 1;
    context.create_data_directory = TRUE;
    ok( make_test_root( root ), "failed to make rejected metadata root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        "%s metadata returned %#lx.\n", wine_dbgstr_w(name), hr );
    ok( !result, "%s metadata returned a result.\n", wine_dbgstr_w(name) );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_application_launch_metadata( void )
{
    static const struct appx_catalog_application metadata_application =
    {
        L"App", L"bin\\App.exe", L"Windows.FullTrustApplication",
        L"--safe", L"data", APPX_CATALOG_ACTIVATION_FULL_TRUST
    };
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL, *failed = (void *)0xdeadbeef;
    APPX_DEPLOYMENT_RECORD *record = NULL;
    APPX_DEPLOYMENT_RUNTIME *runtime = NULL;
    const struct appx_catalog_package *package;
    const struct appx_deployment_application_file *application;
    const struct wine_appx_graph_attach *attach;
    const BYTE *blob;
    const HANDLE *leases;
    const UINT64 *lease_values;
    const WCHAR *path, *parameters, *current_directory;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH], record_path[MAX_PATH * 2];
    WCHAR long_parameters[1026], long_current_dir[258];
    HANDLE package_file;
    UINT32 blob_size, i;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x56 );
    context.applications = &metadata_application;
    context.application_count = 1;
    context.create_data_directory = TRUE;
    ok( make_test_root( root ), "failed to make metadata root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "metadata install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "metadata query returned %#lx.\n", hr );
    package = snapshot ? appx_catalog_snapshot_get_package( snapshot, 0 ) : NULL;
    ok( package && package->application_count == 1,
        "metadata catalog application is missing.\n" );
    ok( package && package->application_count == 1 &&
        !lstrcmpW( package->applications[0].entry_point,
                   L"Windows.FullTrustApplication" ),
        "metadata catalog entry point is wrong.\n" );
    ok( package && package->application_count == 1 &&
        !lstrcmpW( package->applications[0].parameters, L"--safe" ),
        "metadata catalog parameters are wrong.\n" );
    ok( package && package->application_count == 1 &&
        !lstrcmpW( package->applications[0].current_directory_path, L"data" ),
        "metadata catalog current directory is wrong.\n" );
    hr = appx_deployment_record_load( package, &options, &record );
    ok( hr == S_OK, "metadata record load returned %#lx.\n", hr );
    ok( appx_deployment_record_get_application_file_count(record) == 1,
        "metadata record application count is wrong.\n" );
    application = appx_deployment_record_get_application_file( record, 0 );
    ok( application && !lstrcmpW( application->entry_point,
                                  L"Windows.FullTrustApplication" ),
        "metadata record entry point is wrong.\n" );
    ok( application && !lstrcmpW( application->parameters, L"--safe" ),
        "metadata record parameters are wrong.\n" );
    ok( application && !lstrcmpW(
            application->current_directory_path, L"data" ),
        "metadata record current directory is wrong.\n" );
    ok( application && application->activation_kind ==
            APPX_CATALOG_ACTIVATION_FULL_TRUST,
        "metadata record activation kind is wrong.\n" );
    hr = appx_deployment_runtime_acquire(
        context.package.full_name, L"App", &options, &runtime );
    ok( SUCCEEDED(hr), "metadata runtime acquire returned %#lx.\n", hr );
    ok( runtime != NULL, "metadata runtime was not returned.\n" );
    ok( appx_deployment_runtime_get_graph(runtime) != NULL,
        "metadata runtime graph is missing.\n" );
    check_runtime_graph_object_ids( runtime );
    ok( appx_deployment_runtime_get_lease_count(runtime) == 1,
        "metadata runtime lease count is wrong.\n" );
    leases = appx_deployment_runtime_get_leases( runtime );
    ok( leases && leases[0] != INVALID_HANDLE_VALUE,
        "metadata runtime lease is missing.\n" );
    blob = appx_package_graph_get_blob(
        appx_deployment_runtime_get_graph(runtime), &blob_size );
    attach = appx_deployment_runtime_get_attach( runtime );
    ok( attach && attach->size == blob_size,
        "attach size is %u, expected graph byte size %u.\n",
        attach ? attach->size : 0, blob_size );
    ok( attach && (const BYTE *)(ULONG_PTR)attach->blob == blob,
        "attach graph pointer does not reference the runtime graph.\n" );
    ok( attach && attach->lease_count == 1,
        "attach lease count is %u, expected 1.\n",
        attach ? attach->lease_count : 0 );
    lease_values = attach
        ? (const UINT64 *)(ULONG_PTR)attach->leases : NULL;
    ok( lease_values &&
        lease_values[0] == (UINT64)(ULONG_PTR)leases[0],
        "attach lease value is %s, expected %s.\n",
        wine_dbgstr_longlong(lease_values ? lease_values[0] : 0),
        wine_dbgstr_longlong(leases ? (UINT64)(ULONG_PTR)leases[0] : 0) );
    ok( sizeof(*lease_values) == sizeof(UINT64),
        "attach lease element has size %Iu, expected %Iu.\n",
        sizeof(*lease_values), sizeof(UINT64) );
    ok( appx_deployment_runtime_get_executable_handle(runtime) !=
            INVALID_HANDLE_VALUE,
        "metadata runtime executable handle is missing.\n" );
    path = appx_deployment_runtime_get_executable_path( runtime );
    ok( path_ends_with( path, L"\\bin\\App.exe" ),
        "metadata runtime executable path is %s.\n", wine_dbgstr_w(path) );
    parameters = appx_deployment_runtime_get_parameters( runtime );
    ok( parameters && !lstrcmpW( parameters, L"--safe" ),
        "metadata runtime parameters are %s.\n",
        wine_dbgstr_w(parameters) );
    current_directory =
        appx_deployment_runtime_get_current_directory( runtime );
    ok( path_ends_with( current_directory, L"\\data" ),
        "metadata runtime current directory is %s.\n",
        wine_dbgstr_w(current_directory) );
    appx_deployment_runtime_free( runtime );
    runtime = NULL;
    ok( package && get_record_path( root, package, record_path ),
        "failed to derive metadata record path.\n" );
    appx_deployment_record_free( record );
    record = NULL;
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;

    if (replace_record_string( record_path, L"data", L"..\\x" ))
    {
        hr = appx_deployment_query( NULL, &options, &failed );
        ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
            "damaged current-directory query returned %#lx.\n", hr );
        ok( !failed, "damaged current-directory query returned a snapshot.\n" );
    }
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    for (i = 0; i < ARRAY_SIZE(long_parameters) - 1; i++)
        long_parameters[i] = 'p';
    long_parameters[ARRAY_SIZE(long_parameters) - 1] = 0;
    for (i = 0; i < ARRAY_SIZE(long_current_dir) - 1; i++)
        long_current_dir[i] = 'd';
    long_current_dir[ARRAY_SIZE(long_current_dir) - 1] = 0;
    check_rejected_application_metadata(
        L"overlength parameters", long_parameters, L"" );
    check_rejected_application_metadata(
        L"overlength current directory", L"", long_current_dir );
    check_rejected_application_metadata(
        L"absolute current directory", L"", L"\\data" );
    check_rejected_application_metadata(
        L"up-level current directory", L"", L"..\\data" );
}

static void test_runtime_acquisition_and_lease( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    APPX_DEPLOYMENT_RUNTIME *runtime = NULL;
    const struct appx_catalog_package *package;
    const WCHAR *path, *parameters, *current_directory;
    const HANDLE *leases;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    WCHAR payload_path[MAX_PATH * 2] = {0}, lease_path[MAX_PATH * 2] = {0};
    HANDLE package_file, writer;
    DWORD attributes;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x57 );
    ok( make_test_root( root ), "failed to make runtime root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "runtime install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;

    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "runtime baseline query returned %#lx.\n", hr );
    package = snapshot ? appx_catalog_snapshot_get_package( snapshot, 0 ) : NULL;
    ok( package && get_payload_directory_path(
            root, package, payload_path ) &&
        get_lease_generation_path( root, package, lease_path ),
        "failed to derive runtime generation paths.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;

    hr = appx_deployment_runtime_acquire(
        context.package.full_name, L"App", &options, &runtime );
    ok( SUCCEEDED(hr), "runtime acquire returned %#lx.\n", hr );
    ok( runtime != NULL, "runtime was not returned.\n" );
    ok( appx_deployment_runtime_get_graph(runtime) != NULL,
        "runtime graph is missing.\n" );
    check_runtime_graph_object_ids( runtime );
    ok( appx_deployment_runtime_get_lease_count(runtime) == 1,
        "runtime lease count is wrong.\n" );
    leases = appx_deployment_runtime_get_leases( runtime );
    ok( leases && leases[0] != INVALID_HANDLE_VALUE,
        "runtime lease is missing.\n" );
    SetLastError( 0xdeadbeef );
    writer = CreateFileW(
        lease_path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( writer == INVALID_HANDLE_VALUE &&
        GetLastError() == ERROR_SHARING_VIOLATION,
        "live runtime marker write returned %p, error %lu.\n",
        writer, GetLastError() );
    if (writer != INVALID_HANDLE_VALUE) CloseHandle( writer );
    ok( appx_deployment_runtime_get_executable_handle(runtime) !=
            INVALID_HANDLE_VALUE,
        "runtime executable handle is missing.\n" );
    path = appx_deployment_runtime_get_executable_path( runtime );
    ok( path_ends_with( path, L"\\bin\\App.exe" ),
        "runtime executable path is %s.\n", wine_dbgstr_w(path) );
    parameters = appx_deployment_runtime_get_parameters( runtime );
    ok( parameters && !parameters[0], "runtime parameters are %s.\n",
        wine_dbgstr_w(parameters) );
    current_directory =
        appx_deployment_runtime_get_current_directory( runtime );
    ok( current_directory && !lstrcmpW( current_directory, payload_path ),
        "runtime current directory is %s, expected %s.\n",
        wine_dbgstr_w(current_directory), wine_dbgstr_w(payload_path) );

    hr = execute_package_with_production_lease_check(
        APPX_DEPLOYMENT_OPERATION_REMOVE, INVALID_HANDLE_VALUE,
        context.package.full_name, &options, &context, &result );
    ok( SUCCEEDED(hr), "runtime remove returned %#lx.\n", hr );
    ok( result && (appx_deployment_result_get_flags(result) &
                   APPX_DEPLOYMENT_RESULT_GC_DEFERRED),
        "runtime lease did not defer GC.\n" );
    attributes = GetFileAttributesW( payload_path );
    ok( attributes != INVALID_FILE_ATTRIBUTES,
        "runtime payload was reclaimed while lease was held.\n" );
    appx_deployment_result_free( result );
    result = NULL;
    appx_deployment_runtime_free( runtime );
    runtime = NULL;
    writer = CreateFileW(
        lease_path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( writer != INVALID_HANDLE_VALUE,
        "released runtime retained marker write exclusion, error %lu.\n",
        GetLastError() );
    if (writer != INVALID_HANDLE_VALUE) CloseHandle( writer );

    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_runtime_ignores_unrelated_packages( void )
{
    static const WCHAR unrelated_full_name[] =
        L"Fabrikam.Unrelated_1.0.0.0_x64__8wekyb3d8bbwe";
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    APPX_DEPLOYMENT_RUNTIME *runtime = NULL;
    const struct appx_catalog_package *package = NULL;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH], payload_path[MAX_PATH * 2];
    HANDLE package_file, file;
    DWORD written;
    UINT32 i;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    ok( make_test_root( root ),
        "failed to make unrelated-package runtime root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );

    init_fake_package( &context.package, 1, 0x58 );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "main-package install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;

    init_fake_package( &context.package, 1, 0x59 );
    context.package.name = L"Fabrikam.Unrelated";
    context.package.full_name = unrelated_full_name;
    context.package.family_name = L"Fabrikam.Unrelated_8wekyb3d8bbwe";
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "unrelated-package install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;

    payload_path[0] = 0;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "unrelated-package baseline query returned %#lx.\n", hr );
    if (snapshot)
    {
        for (i = 0; i < appx_catalog_snapshot_get_package_count(snapshot); i++)
        {
            const struct appx_catalog_package *candidate =
                appx_catalog_snapshot_get_package( snapshot, i );

            if (candidate && !lstrcmpW( candidate->full_name,
                                       unrelated_full_name ))
            {
                package = candidate;
                break;
            }
        }
    }
    ok( package && get_payload_file_path(
            root, package, L"bin\\App.dll", payload_path ),
        "failed to derive unrelated package payload path.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    file = payload_path[0] ? CreateFileW(
        payload_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) :
        INVALID_HANDLE_VALUE;
    ok( file != INVALID_HANDLE_VALUE,
        "failed to open unrelated package payload for tampering.\n" );
    if (file != INVALID_HANDLE_VALUE)
    {
        BYTE byte = 'U';

        SetFilePointer( file, 3, NULL, FILE_BEGIN );
        ok( WriteFile( file, &byte, 1, &written, NULL ) && written == 1,
            "failed to tamper unrelated package payload.\n" );
        CloseHandle( file );
    }

    hr = appx_deployment_runtime_acquire(
        L"Contoso.App_1.0.0.0_x64__8wekyb3d8bbwe",
        L"App", &options, &runtime );
    ok( SUCCEEDED(hr),
        "runtime acquire with unrelated installed package returned %#lx.\n",
        hr );
    ok( runtime && appx_package_graph_get_package_count(
            appx_deployment_runtime_get_graph(runtime)) == 1,
        "runtime graph included an unrelated package.\n" );
    ok( runtime && appx_deployment_runtime_get_lease_count(runtime) == 1,
        "runtime acquired a lease for an unrelated package.\n" );

    appx_deployment_runtime_free( runtime );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_generation_marker_fail_closed( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL, *failed = (void *)0xdeadbeef;
    const struct appx_catalog_package *package;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    WCHAR lease_path[MAX_PATH * 2], link_path[MAX_PATH * 2];
    HANDLE package_file, marker;
    BYTE bad_content_id[APPX_CATALOG_CONTENT_ID_SIZE];
    DWORD written;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x57 );
    ok( make_test_root( root ), "failed to make missing-marker root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "missing-marker install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "missing-marker baseline query returned %#lx.\n", hr );
    package = snapshot ? appx_catalog_snapshot_get_package( snapshot, 0 ) : NULL;
    ok( package && get_lease_generation_path( root, package, lease_path ) &&
        verify_generation_marker_file( lease_path, package ),
        "failed to verify missing-marker baseline.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    ok( DeleteFileW( lease_path ), "failed to delete generation marker.\n" );
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "missing-marker query returned %#lx.\n", hr );
    ok( !failed, "missing-marker query returned a snapshot.\n" );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    failed = (void *)0xdeadbeef;
    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x58 );
    ok( make_test_root( root ), "failed to make tampered-marker root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "tampered-marker install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "tampered-marker baseline query returned %#lx.\n", hr );
    package = snapshot ? appx_catalog_snapshot_get_package( snapshot, 0 ) : NULL;
    ok( package && get_lease_generation_path( root, package, lease_path ),
        "failed to derive tampered marker path.\n" );
    if (package)
    {
        memcpy( bad_content_id, package->content_id, sizeof(bad_content_id) );
        bad_content_id[0] ^= 0xff;
    }
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    marker = CreateFileW( lease_path, GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( marker != INVALID_HANDLE_VALUE,
        "failed to open generation marker for tampering.\n" );
    if (marker != INVALID_HANDLE_VALUE)
    {
        ok( WriteFile( marker, bad_content_id, sizeof(bad_content_id),
                       &written, NULL ) &&
            written == sizeof(bad_content_id),
            "failed to tamper generation marker.\n" );
        CloseHandle( marker );
    }
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "tampered-marker query returned %#lx.\n", hr );
    ok( !failed, "tampered-marker query returned a snapshot.\n" );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    failed = (void *)0xdeadbeef;
    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x59 );
    ok( make_test_root( root ), "failed to make hardlink-marker root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "hardlink-marker install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "hardlink-marker baseline query returned %#lx.\n", hr );
    package = snapshot ? appx_catalog_snapshot_get_package( snapshot, 0 ) : NULL;
    ok( package && get_lease_generation_path( root, package, lease_path ),
        "failed to derive hardlink marker path.\n" );
    ok( append_path( link_path, ARRAY_SIZE(link_path), root,
                     L"marker-link.bin" ),
        "failed to derive marker link path.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    if (CreateHardLinkW( link_path, lease_path, NULL ))
    {
        hr = appx_deployment_query( NULL, &options, &failed );
        ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
            "hardlinked-marker query returned %#lx.\n", hr );
        ok( !failed, "hardlinked-marker query returned a snapshot.\n" );
        DeleteFileW( link_path );
    }
    else
        win_skip( "marker hardlinks are unavailable, error %lu.\n",
                  GetLastError() );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_sidecar_fail_closed( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL, *failed = (void *)0xdeadbeef;
    const struct appx_catalog_package *package;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH], record_path[MAX_PATH * 2];
    HANDLE package_file, record;
    BYTE byte;
    DWORD read, written;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x51 );
    ok( make_test_root( root ), "failed to make sidecar root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "sidecar install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "sidecar baseline query returned %#lx.\n", hr );
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    ok( package && get_record_path( root, package, record_path ),
        "failed to derive record path.\n" );

    record = CreateFileW( record_path, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL );
    ok( record != INVALID_HANDLE_VALUE, "failed to open sidecar.\n" );
    if (record != INVALID_HANDLE_VALUE)
    {
        ok( ReadFile( record, &byte, 1, &read, NULL ) && read == 1,
            "failed to read sidecar byte.\n" );
        byte ^= 0x80;
        SetFilePointer( record, 0, NULL, FILE_BEGIN );
        ok( WriteFile( record, &byte, 1, &written, NULL ) && written == 1,
            "failed to corrupt sidecar.\n" );
        CloseHandle( record );
    }
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "digest-bad sidecar query returned %#lx.\n", hr );
    ok( !failed, "corrupt query returned a snapshot.\n" );
    ok( GetFileAttributesW( record_path ) != INVALID_FILE_ATTRIBUTES,
        "corrupt sidecar evidence was deleted.\n" );

    record = CreateFileW( record_path, GENERIC_WRITE, 0, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( record != INVALID_HANDLE_VALUE, "failed to open sidecar for truncation.\n" );
    if (record != INVALID_HANDLE_VALUE)
    {
        SetFilePointer( record, 16, NULL, FILE_BEGIN );
        ok( SetEndOfFile( record ), "failed to truncate sidecar.\n" );
        CloseHandle( record );
    }
    failed = (void *)0xdeadbeef;
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "truncated sidecar query returned %#lx.\n", hr );
    ok( !failed, "truncated query returned a snapshot.\n" );
    ok( DeleteFileW( record_path ), "failed to remove sidecar.\n" );
    failed = (void *)0xdeadbeef;
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "missing sidecar query returned %#lx.\n", hr );
    ok( !failed, "missing query returned a snapshot.\n" );

    appx_catalog_snapshot_free( snapshot );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_payload_identity_fail_closed( void )
{
    static const BYTE replacement_dll[] = "MZ replacement dll";
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL, *failed = (void *)0xdeadbeef;
    APPX_DEPLOYMENT_RUNTIME *runtime = (void *)0xdeadbeef;
    const struct appx_catalog_package *package;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH], payload_path[MAX_PATH * 2];
    WCHAR link_path[MAX_PATH * 2];
    HANDLE package_file, file;
    DWORD written;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x52 );
    ok( make_test_root( root ), "failed to make executable-missing root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "executable-missing install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "executable-missing baseline query returned %#lx.\n", hr );
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    ok( package && get_payload_file_path(
            root, package, L"bin\\App.exe", payload_path ),
        "failed to derive executable path.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    ok( DeleteFileW( payload_path ), "failed to delete executable.\n" );
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "missing executable query returned %#lx.\n", hr );
    ok( !failed, "missing executable returned a snapshot.\n" );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    failed = (void *)0xdeadbeef;
    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x55 );
    ok( make_test_root( root ), "failed to make executable-tampered root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "executable-tampered install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "executable-tampered baseline query returned %#lx.\n", hr );
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    ok( package && get_payload_file_path(
            root, package, L"bin\\App.exe", payload_path ),
        "failed to derive tampered executable path.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    file = CreateFileW( payload_path, GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE,
        "failed to open executable for in-place tampering.\n" );
    if (file != INVALID_HANDLE_VALUE)
    {
        BYTE byte = 'X';

        SetFilePointer( file, 3, NULL, FILE_BEGIN );
        ok( WriteFile( file, &byte, 1, &written, NULL ) && written == 1,
            "failed to tamper executable in place.\n" );
        CloseHandle( file );
    }
    hr = appx_deployment_runtime_acquire(
        context.package.full_name, L"App", &options, &runtime );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "same-id executable tamper runtime acquire returned %#lx.\n", hr );
    ok( !runtime, "same-id executable tamper returned a runtime.\n" );
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "same-id executable tamper query returned %#lx.\n", hr );
    ok( !failed, "same-id executable tamper returned a snapshot.\n" );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    failed = (void *)0xdeadbeef;
    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x56 );
    ok( make_test_root( root ), "failed to make dll-tampered root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "dll-tampered install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "dll-tampered baseline query returned %#lx.\n", hr );
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    ok( package && get_payload_file_path(
            root, package, L"bin\\App.dll", payload_path ),
        "failed to derive tampered dll path.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    file = CreateFileW( payload_path, GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE,
        "failed to open dll for in-place tampering.\n" );
    if (file != INVALID_HANDLE_VALUE)
    {
        BYTE byte = 'Y';

        SetFilePointer( file, 3, NULL, FILE_BEGIN );
        ok( WriteFile( file, &byte, 1, &written, NULL ) && written == 1,
            "failed to tamper dll in place.\n" );
        CloseHandle( file );
    }
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "same-id dll tamper query returned %#lx.\n", hr );
    ok( !failed, "same-id dll tamper returned a snapshot.\n" );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    failed = (void *)0xdeadbeef;
    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x53 );
    ok( make_test_root( root ), "failed to make dll-replaced root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "dll-replaced install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "dll-replaced baseline query returned %#lx.\n", hr );
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    ok( package && get_payload_file_path(
            root, package, L"bin\\App.dll", payload_path ),
        "failed to derive dll path.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    ok( DeleteFileW( payload_path ), "failed to delete dll.\n" );
    file = CreateFileW( payload_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL );
    ok( file != INVALID_HANDLE_VALUE, "failed to recreate dll.\n" );
    if (file != INVALID_HANDLE_VALUE)
    {
        ok( WriteFile( file, replacement_dll, sizeof(replacement_dll),
                       &written, NULL ) &&
            written == sizeof(replacement_dll),
            "failed to write replacement dll.\n" );
        CloseHandle( file );
    }
    hr = appx_deployment_query( NULL, &options, &failed );
    ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
        "substituted dll query returned %#lx.\n", hr );
    ok( !failed, "substituted dll returned a snapshot.\n" );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    failed = (void *)0xdeadbeef;
    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x54 );
    ok( make_test_root( root ), "failed to make executable-hardlink root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "executable-hardlink install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "executable-hardlink baseline query returned %#lx.\n", hr );
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    ok( package && get_payload_file_path(
            root, package, L"bin\\App.exe", payload_path ),
        "failed to derive executable path.\n" );
    ok( append_path( link_path, ARRAY_SIZE(link_path), root,
                     L"executable-link.bin" ),
        "failed to derive executable link path.\n" );
    if (CreateHardLinkW( link_path, payload_path, NULL ))
    {
        hr = appx_deployment_query( NULL, &options, &failed );
        ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
            "hardlinked executable query returned %#lx.\n", hr );
        ok( !failed, "hardlinked executable returned a snapshot.\n" );
        DeleteFileW( link_path );
    }
    else
        win_skip( "payload hardlinks are unavailable, error %lu.\n",
                  GetLastError() );
    appx_catalog_snapshot_free( snapshot );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_dependencies_and_architecture( void )
{
    static const struct appx_catalog_dependency dependency =
    {
        L"Contoso.Framework", L"CN=Contoso", {1, 0, 0, 0}
    };
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    HANDLE package_file;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    ok( make_test_root( root ), "failed to make dependency root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );

    init_fake_package( &context.package, 1, 0x61 );
    context.package.name = L"Contoso.Framework";
    context.package.full_name =
        L"Contoso.Framework_1.0.0.0_neutral__8wekyb3d8bbwe";
    context.package.family_name = L"Contoso.Framework_8wekyb3d8bbwe";
    context.package.architecture = APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    context.package.package_flags |= APPX_CATALOG_PACKAGE_FRAMEWORK;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "framework install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;

    init_fake_package( &context.package, 1, 0x62 );
    context.package.dependencies = &dependency;
    context.package.dependency_count = 1;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "dependent install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;

    hr = execute_package(
        APPX_DEPLOYMENT_OPERATION_REMOVE, INVALID_HANDLE_VALUE,
        L"Contoso.Framework_1.0.0.0_neutral__8wekyb3d8bbwe",
        &options, &context, &result );
    ok( hr == APPX_DEPLOYMENT_E_PACKAGE_IN_USE,
        "active-dependent removal returned %#lx.\n", hr );
    ok( !result, "blocked removal returned a result.\n" );

    hr = execute_package(
        APPX_DEPLOYMENT_OPERATION_REMOVE, INVALID_HANDLE_VALUE,
        L"Contoso.App_1.0.0.0_x64__8wekyb3d8bbwe",
        &options, &context, &result );
    ok( SUCCEEDED(hr), "dependent removal returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = execute_package(
        APPX_DEPLOYMENT_OPERATION_REMOVE, INVALID_HANDLE_VALUE,
        L"Contoso.Framework_1.0.0.0_neutral__8wekyb3d8bbwe",
        &options, &context, &result );
    ok( SUCCEEDED(hr), "framework removal returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;

    remove_tree( root );
    ok( make_test_root( root ), "failed to make architecture root.\n" );
    options.store_root = root;
    init_fake_package( &context.package, 1, 0x63 );
    context.package.full_name =
        L"Contoso.App_1.0.0.0_x86__8wekyb3d8bbwe";
    context.package.architecture = APPX_CATALOG_ARCHITECTURE_X86;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "supported x86 guest install returned %#lx.\n", hr );
    ok( result != NULL, "supported x86 guest install returned no result.\n" );
    appx_deployment_result_free( result );
    result = NULL;
    remove_tree( root );

    ok( make_test_root( root ),
        "failed to make unsupported architecture root.\n" );
    options.store_root = root;
    init_fake_package( &context.package, 1, 0x64 );
    context.package.full_name =
        L"Contoso.App_1.0.0.0_arm64__8wekyb3d8bbwe";
    context.package.architecture = APPX_CATALOG_ARCHITECTURE_ARM64;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == HRESULT_FROM_WIN32(
            ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE),
        "architecture mismatch returned %#lx.\n", hr );
    ok( !result, "architecture mismatch returned a result.\n" );

    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void run_recovery_checkpoint(
    enum appx_deployment_test_checkpoint point, BOOL committed )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    HANDLE package_file;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x71 + point );
    context.fail_checkpoint = point;
    context.checkpoint_error = E_ABORT;
    ok( make_test_root( root ), "failed to make recovery root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == E_ABORT, "checkpoint %u returned %#lx.\n", point, hr );
    ok( !result, "checkpoint failure returned a result.\n" );

    context.fail_checkpoint = 0;
    hr = execute_package(
        APPX_DEPLOYMENT_OPERATION_RECOVER, INVALID_HANDLE_VALUE,
        NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "recovery after checkpoint %u returned %#lx.\n",
        point, hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "recovery query returned %#lx.\n", hr );
    ok( snapshot &&
        appx_catalog_snapshot_get_package_count(snapshot) == committed,
        "checkpoint %u committed state is wrong.\n", point );

    appx_catalog_snapshot_free( snapshot );
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void check_recovery_path( const char *label, const WCHAR *path,
                                 BOOL expected )
{
    DWORD attributes = GetFileAttributesW( path );

    if (expected)
        ok( attributes != INVALID_FILE_ATTRIBUTES,
            "%s path %s is missing, error %lu.\n",
            label, wine_dbgstr_w(path), GetLastError() );
    else
        ok( attributes == INVALID_FILE_ATTRIBUTES,
            "%s path %s remains, attributes %#lx.\n",
            label, wine_dbgstr_w(path), attributes );
}

static void check_remove_recovery_state(
    const WCHAR *full_name, APPX_DEPLOYMENT_OPTIONS *options,
    const WCHAR *payload_path, const WCHAR *record_path,
    const WCHAR *lease_path, BOOL active, UINT32 pass )
{
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    const struct appx_catalog_package *package;
    UINT32 count = ~0u, expected = active ? 1 : 0;
    HRESULT hr;

    hr = appx_deployment_query( NULL, options, &snapshot );
    ok( SUCCEEDED(hr), "remove recovery pass %u query returned %#lx.\n",
        pass, hr );
    if (snapshot) count = appx_catalog_snapshot_get_package_count( snapshot );
    ok( snapshot && count == expected,
        "remove recovery pass %u package count is %u, expected %u.\n",
        pass, count, expected );
    if (snapshot && active && count)
    {
        package = appx_catalog_snapshot_get_package( snapshot, 0 );
        ok( package && !lstrcmpW( package->full_name, full_name ),
            "remove recovery pass %u active package is %s.\n",
            pass, package ? wine_dbgstr_w(package->full_name) : "<null>" );
    }
    appx_catalog_snapshot_free( snapshot );

    check_recovery_path( "payload", payload_path, active );
    check_recovery_path( "record", record_path, active );
    check_recovery_path( "lease marker", lease_path, active );
}

static void run_remove_recovery_checkpoint(
    enum appx_deployment_test_checkpoint point, BOOL active_after_recovery )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    const struct appx_catalog_package *package;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    WCHAR payload_path[MAX_PATH * 2], record_path[MAX_PATH * 2];
    WCHAR lease_path[MAX_PATH * 2];
    HANDLE package_file;
    UINT32 i, count = 0;
    HRESULT hr;
    BOOL ready;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0xa0 + point );
    ok( make_test_root( root ), "failed to make remove recovery root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    ok( package_file != INVALID_HANDLE_VALUE,
        "failed to create remove recovery package file.\n" );

    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "remove recovery setup install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;

    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "remove recovery setup query returned %#lx.\n", hr );
    if (snapshot) count = appx_catalog_snapshot_get_package_count( snapshot );
    ok( snapshot && count == 1,
        "remove recovery setup package count is %u.\n", count );
    package = snapshot && count ?
        appx_catalog_snapshot_get_package( snapshot, 0 ) : NULL;
    ready = package &&
            get_payload_directory_path( root, package, payload_path ) &&
            get_record_path( root, package, record_path ) &&
            get_lease_generation_path( root, package, lease_path );
    ok( ready, "failed to derive remove recovery generation paths.\n" );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;
    if (!ready) goto done;

    context.fail_checkpoint = point;
    context.checkpoint_error = E_ABORT;
    hr = execute_package(
        APPX_DEPLOYMENT_OPERATION_REMOVE, INVALID_HANDLE_VALUE,
        context.package.full_name, &options, &context, &result );
    ok( hr == E_ABORT, "remove checkpoint %u returned %#lx.\n", point, hr );
    ok( !result, "remove checkpoint failure returned a result.\n" );
    context.fail_checkpoint = 0;

    for (i = 0; i < 2; i++)
    {
        hr = execute_package(
            APPX_DEPLOYMENT_OPERATION_RECOVER, INVALID_HANDLE_VALUE,
            NULL, &options, &context, &result );
        ok( SUCCEEDED(hr),
            "remove recovery checkpoint %u pass %u returned %#lx.\n",
            point, i + 1, hr );
        appx_deployment_result_free( result );
        result = NULL;
        check_remove_recovery_state(
            context.package.full_name, &options, payload_path, record_path,
            lease_path, active_after_recovery, i + 1 );
    }

done:
    appx_deployment_result_free( result );
    if (package_file != INVALID_HANDLE_VALUE) CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_recovery_boundaries( void )
{
    run_recovery_checkpoint(
        APPX_DEPLOYMENT_CHECKPOINT_JOURNAL_CREATED, FALSE );
    run_recovery_checkpoint(
        APPX_DEPLOYMENT_CHECKPOINT_EXTRACTED, FALSE );
    run_recovery_checkpoint(
        APPX_DEPLOYMENT_CHECKPOINT_PAYLOAD_RENAMED, FALSE );
    run_recovery_checkpoint(
        APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PREPARED, FALSE );
    run_recovery_checkpoint(
        APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PUBLISHED, TRUE );
    run_remove_recovery_checkpoint(
        APPX_DEPLOYMENT_CHECKPOINT_CATALOG_PREPARED, TRUE );
    run_remove_recovery_checkpoint(
        APPX_DEPLOYMENT_CHECKPOINT_CATALOG_UNPUBLISHED, FALSE );
}

static void test_partial_write_disk_full_and_cancel( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    HANDLE package_file;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x81 );
    context.partial_writes = TRUE;
    ok( make_test_root( root ), "failed to make partial-write root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "partial-write install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    remove_tree( root );

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x82 );
    context.disk_full = TRUE;
    ok( make_test_root( root ), "failed to make disk-full root.\n" );
    options.store_root = root;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL),
        "disk-full write returned %#lx.\n", hr );
    ok( !result, "disk-full write returned a result.\n" );
    remove_tree( root );

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x83 );
    context.cancel_event = CreateEventW( NULL, TRUE, TRUE, NULL );
    ok( make_test_root( root ), "failed to make cancellation root.\n" );
    options.store_root = root;
    options.cancel_event = context.cancel_event;
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
        "cancellation returned %#lx.\n", hr );
    ok( !result, "cancellation returned a result.\n" );
    CloseHandle( context.cancel_event );

    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_lock_order_epoch_conflict_and_waits( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL;
    struct cancel_signal_context cancel_signal;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    HANDLE package_file, lock_file = INVALID_HANDLE_VALUE;
    HANDLE thread = NULL;
    OVERLAPPED overlapped;
    ULONGLONG start, elapsed;
    DWORD wait;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x89 );
    ok( make_test_root( root ), "failed to make epoch-conflict root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    ok( package_file != INVALID_HANDLE_VALUE,
        "failed to create concurrency package file.\n" );
    context.external_store_root = root;
    context.external_publish_start =
        CreateEventW( NULL, TRUE, FALSE, NULL );
    context.external_publish_done =
        CreateEventW( NULL, TRUE, FALSE, NULL );
    context.external_publish_result = E_PENDING;
    ok( context.external_publish_start && context.external_publish_done,
        "failed to create external-publish events.\n" );
    if (context.external_publish_start && context.external_publish_done)
        thread = CreateThread( NULL, 0, external_catalog_publish_thread,
                               &context, 0, NULL );
    ok( !!thread, "failed to create external-publish thread.\n" );
    if (thread)
    {
        hr = execute_package(
            APPX_DEPLOYMENT_OPERATION_INSTALL, package_file, NULL,
            &options, &context, &result );
        ok( SUCCEEDED(hr), "epoch-conflict install returned %#lx.\n", hr );
        wait = WaitForSingleObject( thread, TEST_TIMEOUT );
        ok( wait == WAIT_OBJECT_0,
            "external-publish thread wait returned %lu.\n", wait );
        ok( SUCCEEDED(context.external_publish_result),
            "external catalog publish returned %#lx.\n",
            context.external_publish_result );
        ok( context.external_publish_triggered == 1,
            "external catalog publish was triggered %ld times.\n",
            context.external_publish_triggered );
        ok( result &&
            appx_deployment_result_get_catalog_epoch(result) == 2,
            "retried deployment epoch is wrong.\n" );
        appx_deployment_result_free( result );
        result = NULL;
        hr = appx_deployment_query( NULL, &options, &snapshot );
        ok( SUCCEEDED(hr), "post-conflict query returned %#lx.\n", hr );
        ok( snapshot &&
            appx_catalog_snapshot_get_package_count(snapshot) == 1,
            "post-conflict package count is wrong.\n" );
        appx_catalog_snapshot_free( snapshot );
        snapshot = NULL;
        CloseHandle( thread );
        thread = NULL;
    }
    if (context.external_publish_done)
        CloseHandle( context.external_publish_done );
    if (context.external_publish_start)
        CloseHandle( context.external_publish_start );
    remove_tree( root );

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x8a );
    ok( make_test_root( root ), "failed to make lock-timeout root.\n" );
    init_options( &options, root );
    options.writer_timeout_ms = 80;
    hr = appx_deployment_initialize( &options, &result );
    ok( SUCCEEDED(hr), "lock-timeout initialization returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    lock_file = lock_deployment_writer( root, &overlapped );
    ok( lock_file != INVALID_HANDLE_VALUE,
        "failed to hold deployment writer lock, error %lu.\n",
        GetLastError() );
    if (lock_file != INVALID_HANDLE_VALUE)
    {
        start = GetTickCount64();
        hr = execute_package(
            APPX_DEPLOYMENT_OPERATION_INSTALL, package_file, NULL,
            &options, &context, &result );
        elapsed = GetTickCount64() - start;
        ok( hr == APPX_DEPLOYMENT_E_LOCK_TIMEOUT,
            "writer-lock timeout returned %#lx.\n", hr );
        ok( !result, "writer-lock timeout returned a result.\n" );
        ok( elapsed < 2000,
            "writer-lock timeout was not bounded, elapsed %s ms.\n",
            wine_dbgstr_longlong(elapsed) );
        UnlockFileEx( lock_file, 0, 1, 0, &overlapped );
        CloseHandle( lock_file );
        lock_file = INVALID_HANDLE_VALUE;
    }
    remove_tree( root );

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x8c );
    ok( make_test_root( root ), "failed to make catalog-lock-timeout root.\n" );
    init_options( &options, root );
    options.writer_timeout_ms = 80;
    hr = appx_deployment_initialize( &options, &result );
    ok( SUCCEEDED(hr), "catalog-lock timeout initialization returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    lock_file = lock_catalog_writer( root, &overlapped );
    ok( lock_file != INVALID_HANDLE_VALUE,
        "failed to hold catalog lock, error %lu.\n", GetLastError() );
    if (lock_file != INVALID_HANDLE_VALUE)
    {
        start = GetTickCount64();
        hr = execute_package(
            APPX_DEPLOYMENT_OPERATION_INSTALL, package_file, NULL,
            &options, &context, &result );
        elapsed = GetTickCount64() - start;
        ok( hr == APPX_DEPLOYMENT_E_LOCK_TIMEOUT,
            "catalog-lock timeout returned %#lx.\n", hr );
        ok( !result, "catalog-lock timeout returned a result.\n" );
        ok( elapsed < 2000,
            "catalog-lock timeout was not bounded, elapsed %s ms.\n",
            wine_dbgstr_longlong(elapsed) );
        UnlockFileEx( lock_file, 0, 1, 0, &overlapped );
        CloseHandle( lock_file );
        lock_file = INVALID_HANDLE_VALUE;
    }
    remove_tree( root );

    memset( &context, 0, sizeof(context) );
    memset( &cancel_signal, 0, sizeof(cancel_signal) );
    init_fake_package( &context.package, 1, 0x8d );
    ok( make_test_root( root ), "failed to make catalog-lock-cancel root.\n" );
    init_options( &options, root );
    options.writer_timeout_ms = 2000;
    hr = appx_deployment_initialize( &options, &result );
    ok( SUCCEEDED(hr), "catalog-lock cancel initialization returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    lock_file = lock_catalog_writer( root, &overlapped );
    ok( lock_file != INVALID_HANDLE_VALUE,
        "failed to hold cancellable catalog lock, error %lu.\n",
        GetLastError() );
    context.signal_checkpoint = APPX_DEPLOYMENT_CHECKPOINT_INSPECTED;
    context.cancel_wait_start = CreateEventW( NULL, TRUE, FALSE, NULL );
    context.cancel_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    options.cancel_event = context.cancel_event;
    cancel_signal.start = context.cancel_wait_start;
    cancel_signal.cancel = context.cancel_event;
    if (context.cancel_wait_start && context.cancel_event)
        thread = CreateThread( NULL, 0, cancel_signal_thread,
                               &cancel_signal, 0, NULL );
    ok( !!thread, "failed to create catalog-lock-cancel thread.\n" );
    if (lock_file != INVALID_HANDLE_VALUE && thread)
    {
        start = GetTickCount64();
        hr = execute_package(
            APPX_DEPLOYMENT_OPERATION_INSTALL, package_file, NULL,
            &options, &context, &result );
        elapsed = GetTickCount64() - start;
        ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
            "catalog-lock cancellation returned %#lx.\n", hr );
        ok( !result, "catalog-lock cancellation returned a result.\n" );
        ok( elapsed < options.writer_timeout_ms,
            "catalog-lock cancellation took %s ms.\n",
            wine_dbgstr_longlong(elapsed) );
        wait = WaitForSingleObject( thread, TEST_TIMEOUT );
        ok( wait == WAIT_OBJECT_0,
            "catalog-lock-cancel thread wait returned %lu.\n", wait );
    }
    if (thread) CloseHandle( thread );
    thread = NULL;
    if (context.cancel_event) CloseHandle( context.cancel_event );
    if (context.cancel_wait_start) CloseHandle( context.cancel_wait_start );
    options.cancel_event = NULL;
    if (lock_file != INVALID_HANDLE_VALUE)
    {
        UnlockFileEx( lock_file, 0, 1, 0, &overlapped );
        CloseHandle( lock_file );
        lock_file = INVALID_HANDLE_VALUE;
    }
    remove_tree( root );

    memset( &context, 0, sizeof(context) );
    memset( &cancel_signal, 0, sizeof(cancel_signal) );
    init_fake_package( &context.package, 1, 0x8b );
    ok( make_test_root( root ), "failed to make lock-cancel root.\n" );
    init_options( &options, root );
    options.writer_timeout_ms = 2000;
    hr = appx_deployment_initialize( &options, &result );
    ok( SUCCEEDED(hr), "lock-cancel initialization returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    lock_file = lock_deployment_writer( root, &overlapped );
    ok( lock_file != INVALID_HANDLE_VALUE,
        "failed to hold cancellable writer lock, error %lu.\n",
        GetLastError() );
    context.cancel_wait_start = CreateEventW( NULL, TRUE, FALSE, NULL );
    context.cancel_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    options.cancel_event = context.cancel_event;
    cancel_signal.start = context.cancel_wait_start;
    cancel_signal.cancel = context.cancel_event;
    if (context.cancel_wait_start && context.cancel_event)
        thread = CreateThread( NULL, 0, cancel_signal_thread,
                               &cancel_signal, 0, NULL );
    ok( !!thread, "failed to create lock-cancel thread.\n" );
    if (lock_file != INVALID_HANDLE_VALUE && thread)
    {
        start = GetTickCount64();
        hr = execute_package(
            APPX_DEPLOYMENT_OPERATION_INSTALL, package_file, NULL,
            &options, &context, &result );
        elapsed = GetTickCount64() - start;
        ok( hr == HRESULT_FROM_WIN32(ERROR_CANCELLED),
            "writer-lock cancellation returned %#lx.\n", hr );
        ok( !result, "writer-lock cancellation returned a result.\n" );
        ok( elapsed < options.writer_timeout_ms,
            "writer-lock cancellation took %s ms.\n",
            wine_dbgstr_longlong(elapsed) );
        wait = WaitForSingleObject( thread, TEST_TIMEOUT );
        ok( wait == WAIT_OBJECT_0,
            "lock-cancel thread wait returned %lu.\n", wait );
    }
    if (thread) CloseHandle( thread );
    if (context.cancel_event) CloseHandle( context.cancel_event );
    if (context.cancel_wait_start) CloseHandle( context.cancel_wait_start );
    if (lock_file != INVALID_HANDLE_VALUE)
    {
        UnlockFileEx( lock_file, 0, 1, 0, &overlapped );
        CloseHandle( lock_file );
    }

    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_hardlink_rejection( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    APPX_CATALOG_SNAPSHOT *snapshot = NULL, *failed = NULL;
    const struct appx_catalog_package *package;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH], record_path[MAX_PATH * 2];
    WCHAR link_path[MAX_PATH * 2], lock_path[MAX_PATH * 2];
    HANDLE package_file;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x91 );
    ok( make_test_root( root ), "failed to make hardlink root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                          NULL, &options, &context, &result );
    ok( SUCCEEDED(hr), "hardlink install returned %#lx.\n", hr );
    appx_deployment_result_free( result );
    result = NULL;
    hr = appx_deployment_query( NULL, &options, &snapshot );
    ok( SUCCEEDED(hr), "hardlink baseline query returned %#lx.\n", hr );
    package = appx_catalog_snapshot_get_package( snapshot, 0 );
    ok( package && get_record_path( root, package, record_path ),
        "failed to derive hardlink record path.\n" );
    ok( append_path( link_path, ARRAY_SIZE(link_path), root,
                     L"record-link.bin" ),
        "failed to derive hardlink path.\n" );
    if (CreateHardLinkW( link_path, record_path, NULL ))
    {
        hr = appx_deployment_query( NULL, &options, &failed );
        ok( hr == APPX_DEPLOYMENT_E_CORRUPT_STORE,
            "hardlinked record query returned %#lx.\n", hr );
        ok( !failed, "hardlinked record returned a snapshot.\n" );
        DeleteFileW( link_path );
    }
    else
        win_skip( "hardlinks are unavailable, error %lu.\n", GetLastError() );
    appx_catalog_snapshot_free( snapshot );
    snapshot = NULL;

    ok( append_path( lock_path, ARRAY_SIZE(lock_path), root, L"store.lock" ),
        "failed to derive lock path.\n" );
    ok( append_path( link_path, ARRAY_SIZE(link_path), root,
                     L"lock-link.bin" ),
        "failed to derive lock hardlink path.\n" );
    if (CreateHardLinkW( link_path, lock_path, NULL ))
    {
        hr = execute_package( APPX_DEPLOYMENT_OPERATION_INSTALL, package_file,
                              NULL, &options, &context, &result );
        ok( hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
            "hardlinked lock returned %#lx.\n", hr );
        ok( !result, "hardlinked lock returned a result.\n" );
        DeleteFileW( link_path );
    }
    else
        win_skip( "lock hardlink is unavailable, error %lu.\n", GetLastError() );

    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static void test_generation_lease_gc_semantics( void )
{
    APPX_DEPLOYMENT_OPTIONS options;
    APPX_DEPLOYMENT_RESULT *result = NULL;
    struct test_context context;
    WCHAR root[MAX_PATH], package_path[MAX_PATH];
    WCHAR payload_path[MAX_PATH * 2], lease_path[MAX_PATH * 2];
    HANDLE package_file, lease_file;
    DWORD attributes;
    HRESULT hr;

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x92 );
    ok( make_test_root( root ), "failed to make unused-lease root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    ok( package_file != INVALID_HANDLE_VALUE,
        "failed to create lease package file.\n" );
    if (prepare_orphaned_generation(
            &options, package_file, &context, payload_path, lease_path ))
    {
        lease_file = CreateFileW(
            lease_path, GENERIC_READ | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
        ok( lease_file != INVALID_HANDLE_VALUE,
            "failed to open unused lease file.\n" );
        if (lease_file != INVALID_HANDLE_VALUE) CloseHandle( lease_file );
        hr = execute_package_with_production_lease_check(
            APPX_DEPLOYMENT_OPERATION_GARBAGE_COLLECT,
            INVALID_HANDLE_VALUE, NULL, &options, &context, &result );
        ok( SUCCEEDED(hr), "unused-lease GC returned %#lx.\n", hr );
        ok( result && !(appx_deployment_result_get_flags(result) &
                        APPX_DEPLOYMENT_RESULT_GC_DEFERRED),
            "unused lease deferred GC.\n" );
        attributes = GetFileAttributesW( payload_path );
        ok( attributes == INVALID_FILE_ATTRIBUTES,
            "unused-lease payload remains, attributes %#lx.\n",
            attributes );
        attributes = GetFileAttributesW( lease_path );
        ok( attributes == INVALID_FILE_ATTRIBUTES,
            "unused-lease marker remains, attributes %#lx.\n",
            attributes );
        appx_deployment_result_free( result );
        result = NULL;
    }
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x93 );
    ok( make_test_root( root ), "failed to make shared-lease root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    if (prepare_orphaned_generation(
            &options, package_file, &context, payload_path, lease_path ))
    {
        lease_file = CreateFileW(
            lease_path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL );
        ok( lease_file != INVALID_HANDLE_VALUE,
            "failed to open shared lease file.\n" );
        hr = execute_package_with_production_lease_check(
            APPX_DEPLOYMENT_OPERATION_GARBAGE_COLLECT,
            INVALID_HANDLE_VALUE, NULL, &options, &context, &result );
        ok( SUCCEEDED(hr), "shared-lease GC returned %#lx.\n", hr );
        ok( result && (appx_deployment_result_get_flags(result) &
                       APPX_DEPLOYMENT_RESULT_GC_DEFERRED),
            "shared lease did not defer GC.\n" );
        attributes = GetFileAttributesW( payload_path );
        ok( attributes != INVALID_FILE_ATTRIBUTES,
            "shared-lease payload was reclaimed.\n" );
        appx_deployment_result_free( result );
        result = NULL;
        if (lease_file != INVALID_HANDLE_VALUE) CloseHandle( lease_file );
    }
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );

    memset( &context, 0, sizeof(context) );
    init_fake_package( &context.package, 1, 0x94 );
    ok( make_test_root( root ), "failed to make malformed-lease root.\n" );
    init_options( &options, root );
    package_file = create_dummy_package_file( package_path );
    if (prepare_orphaned_generation(
            &options, package_file, &context, payload_path, lease_path ))
    {
        ok( DeleteFileW( lease_path ),
            "failed to remove lease marker before malformed test, error %lu.\n",
            GetLastError() );
        ok( CreateDirectoryW( lease_path, NULL ),
            "failed to create malformed lease directory, error %lu.\n",
            GetLastError() );
        hr = execute_package_with_production_lease_check(
            APPX_DEPLOYMENT_OPERATION_GARBAGE_COLLECT,
            INVALID_HANDLE_VALUE, NULL, &options, &context, &result );
        ok( SUCCEEDED(hr), "malformed-lease GC returned %#lx.\n", hr );
        ok( result && (appx_deployment_result_get_flags(result) &
                       APPX_DEPLOYMENT_RESULT_GC_DEFERRED),
            "malformed lease did not defer GC.\n" );
        attributes = GetFileAttributesW( payload_path );
        ok( attributes != INVALID_FILE_ATTRIBUTES,
            "malformed-lease payload was reclaimed.\n" );
        appx_deployment_result_free( result );
        result = NULL;
    }
    CloseHandle( package_file );
    DeleteFileW( package_path );
    remove_tree( root );
}

static BOOL load_functions( void )
{
    struct deployment_test_backend_imports imports;
    HMODULE module = LoadLibraryW( L"appxsvc.dll" );

    if (!module) return FALSE;
#define LOAD_FUNC(name) \
    p_##name = (void *)GetProcAddress( module, #name )
    LOAD_FUNC( appx_deployment_initialize );
    ok( !GetProcAddress( module, "appx_deployment_execute_with_test_backend" ),
        "test backend export leaked into appxsvc.dll.\n" );
    LOAD_FUNC( appx_deployment_query );
    LOAD_FUNC( appx_deployment_record_load );
    LOAD_FUNC( appx_deployment_record_free );
    LOAD_FUNC( appx_deployment_record_get_loader_file_count );
    LOAD_FUNC( appx_deployment_record_get_loader_file );
    LOAD_FUNC( appx_deployment_record_get_inproc_class_count );
    LOAD_FUNC( appx_deployment_record_get_inproc_class );
    LOAD_FUNC( appx_deployment_record_get_application_file_count );
    LOAD_FUNC( appx_deployment_record_get_application_file );
    LOAD_FUNC( appx_deployment_result_free );
    LOAD_FUNC( appx_deployment_result_get_flags );
    LOAD_FUNC( appx_deployment_result_get_catalog_epoch );
    LOAD_FUNC( appx_catalog_snapshot_create );
    LOAD_FUNC( appx_catalog_snapshot_deep_copy );
    LOAD_FUNC( appx_catalog_snapshot_free );
    LOAD_FUNC( appx_catalog_snapshot_get_epoch );
    LOAD_FUNC( appx_catalog_snapshot_get_package_count );
    LOAD_FUNC( appx_catalog_snapshot_get_package );
    LOAD_FUNC( appx_catalog_load );
    LOAD_FUNC( appx_catalog_publish );
    LOAD_FUNC( appx_catalog_load_bounded );
    LOAD_FUNC( appx_catalog_publish_bounded );
    LOAD_FUNC( appx_package_graph_create_with_classes );
    LOAD_FUNC( appx_package_graph_create );
    LOAD_FUNC( appx_package_graph_free );
    LOAD_FUNC( appx_package_graph_get_blob );
    LOAD_FUNC( appx_package_graph_get_package_count );
    LOAD_FUNC( appx_package_graph_get_package );
    LOAD_FUNC( appx_deployment_runtime_acquire );
    LOAD_FUNC( appx_deployment_runtime_free );
    LOAD_FUNC( appx_deployment_runtime_get_graph );
    LOAD_FUNC( appx_deployment_runtime_get_lease_count );
    LOAD_FUNC( appx_deployment_runtime_get_leases );
    LOAD_FUNC( appx_deployment_runtime_get_attach );
    LOAD_FUNC( appx_deployment_runtime_get_executable_handle );
    LOAD_FUNC( appx_deployment_runtime_get_executable_path );
    LOAD_FUNC( appx_deployment_runtime_get_parameters );
    LOAD_FUNC( appx_deployment_runtime_get_current_directory );
#undef LOAD_FUNC
    memset( &imports, 0, sizeof(imports) );
    imports.catalog_load_bounded = p_appx_catalog_load_bounded;
    imports.catalog_publish_bounded = p_appx_catalog_publish_bounded;
    imports.catalog_snapshot_create = p_appx_catalog_snapshot_create;
    imports.catalog_snapshot_deep_copy = p_appx_catalog_snapshot_deep_copy;
    imports.catalog_snapshot_free = p_appx_catalog_snapshot_free;
    imports.catalog_snapshot_get_epoch = p_appx_catalog_snapshot_get_epoch;
    imports.catalog_snapshot_get_package_count =
        p_appx_catalog_snapshot_get_package_count;
    imports.catalog_snapshot_get_package = p_appx_catalog_snapshot_get_package;
    imports.graph_create_with_classes =
        p_appx_package_graph_create_with_classes;
    imports.graph_create = p_appx_package_graph_create;
    imports.graph_free = p_appx_package_graph_free;
    imports.graph_get_blob = p_appx_package_graph_get_blob;
    imports.graph_get_package_count = p_appx_package_graph_get_package_count;
    imports.graph_get_package = p_appx_package_graph_get_package;
    deployment_test_backend_set_imports( &imports );
    return p_appx_deployment_initialize &&
           p_appx_deployment_query && p_appx_deployment_record_load &&
           p_appx_deployment_record_free &&
           p_appx_deployment_record_get_loader_file_count &&
           p_appx_deployment_record_get_loader_file &&
           p_appx_deployment_record_get_inproc_class_count &&
           p_appx_deployment_record_get_inproc_class &&
           p_appx_deployment_record_get_application_file_count &&
           p_appx_deployment_record_get_application_file &&
           p_appx_deployment_result_free &&
           p_appx_deployment_result_get_flags &&
           p_appx_deployment_result_get_catalog_epoch &&
           p_appx_catalog_snapshot_create &&
           p_appx_catalog_snapshot_deep_copy &&
           p_appx_catalog_snapshot_free &&
           p_appx_catalog_snapshot_get_epoch &&
           p_appx_catalog_snapshot_get_package_count &&
           p_appx_catalog_snapshot_get_package &&
           p_appx_catalog_load && p_appx_catalog_publish &&
           p_appx_catalog_load_bounded && p_appx_catalog_publish_bounded &&
           p_appx_package_graph_create_with_classes &&
           p_appx_package_graph_create &&
           p_appx_package_graph_free &&
           p_appx_package_graph_get_blob &&
           p_appx_package_graph_get_package_count &&
           p_appx_package_graph_get_package &&
           p_appx_deployment_runtime_acquire &&
           p_appx_deployment_runtime_free &&
           p_appx_deployment_runtime_get_graph &&
           p_appx_deployment_runtime_get_lease_count &&
           p_appx_deployment_runtime_get_leases &&
           p_appx_deployment_runtime_get_attach &&
           p_appx_deployment_runtime_get_executable_handle &&
           p_appx_deployment_runtime_get_executable_path &&
           p_appx_deployment_runtime_get_parameters &&
           p_appx_deployment_runtime_get_current_directory;
}

START_TEST(deployment)
{
    if (!load_functions())
    {
        win_skip( "deployment entry points are unavailable.\n" );
        return;
    }
    test_arguments_and_durability();
    test_install_update_remove_and_record();
    test_application_launch_metadata();
    test_runtime_acquisition_and_lease();
    test_runtime_ignores_unrelated_packages();
    test_sidecar_fail_closed();
    test_generation_marker_fail_closed();
    test_payload_identity_fail_closed();
    test_dependencies_and_architecture();
    test_recovery_boundaries();
    test_partial_write_disk_full_and_cancel();
    test_lock_order_epoch_conflict_and_waits();
    test_hardlink_rejection();
    test_generation_lease_gc_semantics();
}
