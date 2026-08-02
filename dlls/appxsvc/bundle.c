/*
 * AppX bundle inspection and embedded-package trust binding
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
#include "winerror.h"
#include "winnt.h"
#include "winternl.h"
#include "winioctl.h"
#include "winnls.h"
#include "bcrypt.h"
#include "aclapi.h"
#include "sddl.h"

#include "blockmap.h"
#include "bundle.h"
#include "content_types.h"
#include "signature.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(appxsvc);

#define ZIP_METHOD_STORE 0
#define ZIP_METHOD_DEFLATE 8
#define PRIVATE_TEMP_RANDOM_SIZE 16
#define PRIVATE_TEMP_RETRIES 32
#define DEFAULT_MAX_TOTAL_INNER_EXPANDED_SIZE (64ULL * 1024 * 1024 * 1024)

static const WCHAR bundle_manifest_path[] =
    L"AppxMetadata\\AppxBundleManifest.xml";
static const WCHAR block_map_path[] = L"AppxBlockMap.xml";
static const WCHAR content_types_path[] = L"[Content_Types].xml";
static const WCHAR signature_path[] = L"AppxSignature.p7x";
static const WCHAR code_integrity_path[] =
    L"AppxMetadata\\CodeIntegrity.cat";
static const WCHAR embedded_package_type[] = L"application/vnd.ms-appx";
static const WCHAR embedded_package_type_alt[] =
    L"application/vnd.ms-appx.package";

static HRESULT check_cancelled( HANDLE event );

struct appx_bundle_inspection
{
    APPX_BUNDLE_MANIFEST *manifest;
    APPX_PACKAGE_INSPECTION *selected_package;
    struct appx_bundle_selection selection;
    BYTE signer_id[APPX_BUNDLE_SIGNER_ID_SIZE];
};

struct private_security
{
    HANDLE token;
    TOKEN_USER *user;
    ACL *acl;
    SECURITY_DESCRIPTOR descriptor;
    SECURITY_ATTRIBUTES attributes;
};

struct materialized_package
{
    APPX_PACKAGE_INSPECTION *inspection;
};

struct archive_source
{
    WINE_APPX_ARCHIVE *archive;
    const WINE_APPX_ARCHIVE_LIMITS *limits;
    APPX_BUNDLE_INSPECT_OPTIONS options;
    UINT32 package_flags;
    APPX_PACKAGE_INSPECTION *current;
    struct materialized_package retained;
    UINT32 retained_index;
};

static BOOL exact_path( const WCHAR *left, const WCHAR *right )
{
    return CompareStringOrdinal( left, -1, right, -1, FALSE ) == CSTR_EQUAL;
}

static BOOL constant_equal( const BYTE *left, const BYTE *right, UINT32 size )
{
    BYTE difference = 0;
    UINT32 i;

    for (i = 0; i < size; i++) difference |= left[i] ^ right[i];
    return !difference;
}

static BOOL versions_equal( const struct appx_bundle_version *bundle,
                            const struct appx_manifest_version *package )
{
    return bundle->major == package->major &&
           bundle->minor == package->minor &&
           bundle->build == package->build &&
           bundle->revision == package->revision;
}

static BOOL architecture_equal( enum appx_bundle_architecture bundle,
                                enum appx_manifest_architecture package )
{
    switch (bundle)
    {
    case APPX_BUNDLE_ARCHITECTURE_NEUTRAL:
        return package == APPX_MANIFEST_ARCHITECTURE_NEUTRAL;
    case APPX_BUNDLE_ARCHITECTURE_X86:
        return package == APPX_MANIFEST_ARCHITECTURE_X86;
    case APPX_BUNDLE_ARCHITECTURE_X64:
        return package == APPX_MANIFEST_ARCHITECTURE_X64;
    case APPX_BUNDLE_ARCHITECTURE_ARM:
        return package == APPX_MANIFEST_ARCHITECTURE_ARM;
    case APPX_BUNDLE_ARCHITECTURE_ARM64:
        return package == APPX_MANIFEST_ARCHITECTURE_ARM64;
    case APPX_BUNDLE_ARCHITECTURE_X86A64:
        return package == APPX_MANIFEST_ARCHITECTURE_X86A64;
    default:
        return FALSE;
    }
}

static HRESULT get_entry_path( WINE_APPX_ARCHIVE *archive, UINT32 index,
                               WINE_APPX_ARCHIVE_ENTRY *entry, WCHAR **path )
{
    UINT32 capacity, length = 0;
    HRESULT hr;

    *path = NULL;
    memset( entry, 0, sizeof(*entry) );
    entry->size = sizeof(*entry);
    hr = wine_appx_archive_get_entry( archive, index, entry, &length, NULL );
    if (hr != HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER ) ||
        !length || length > WINE_APPX_MAX_PATH_CHARS)
        return FAILED(hr) ? hr : APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!( *path = HeapAlloc( GetProcessHeap(), 0,
                              (SIZE_T)length * sizeof(**path) ) ))
        return E_OUTOFMEMORY;
    capacity = length;
    entry->size = sizeof(*entry);
    hr = wine_appx_archive_get_entry( archive, index, entry, &capacity, *path );
    if (FAILED(hr) || capacity != length || (*path)[length - 1])
    {
        HeapFree( GetProcessHeap(), 0, *path );
        *path = NULL;
        return FAILED(hr) ? hr : APPX_E_INVALID_PACKAGING_LAYOUT;
    }
    return S_OK;
}

static HRESULT get_named_entry( WINE_APPX_ARCHIVE *archive, const WCHAR *path,
                                UINT32 limit, BOOL required, UINT32 *index,
                                WINE_APPX_ARCHIVE_ENTRY *entry )
{
    WCHAR *actual = NULL;
    HRESULT hr;

    *index = 0;
    memset( entry, 0, sizeof(*entry) );
    entry->size = sizeof(*entry);
    if (FAILED(hr = wine_appx_archive_find_entry( archive, path, index )))
    {
        if (hr == HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ))
            return required ? APPX_E_MISSING_REQUIRED_FILE : S_FALSE;
        return hr;
    }
    if (FAILED(hr = get_entry_path( archive, *index, entry, &actual )))
        return hr;
    if (!exact_path( actual, path ) ||
        (entry->flags & WINE_APPX_ENTRY_DIRECTORY) ||
        !entry->uncompressed_size || entry->uncompressed_size > limit ||
        entry->uncompressed_size > MAXDWORD)
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
    HeapFree( GetProcessHeap(), 0, actual );
    return hr;
}

static HRESULT read_entry( WINE_APPX_ARCHIVE *archive, const WCHAR *path,
                           UINT32 limit, HANDLE cancel_event,
                           BYTE **data, UINT32 *size )
{
    WINE_APPX_ARCHIVE_ENTRY entry;
    WINE_APPX_ARCHIVE_STREAM *stream = NULL;
    BYTE terminal, *buffer = NULL;
    UINT64 offset = 0;
    UINT32 index;
    HRESULT hr;

    *data = NULL;
    *size = 0;
    if (FAILED(hr = check_cancelled( cancel_event )) ||
        FAILED(hr = get_named_entry( archive, path, limit, TRUE,
                                     &index, &entry )))
        return hr;
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0,
                              (SIZE_T)entry.uncompressed_size )))
        return E_OUTOFMEMORY;
    if (FAILED(hr = wine_appx_archive_stream_open( archive, index, &stream )))
        goto done;

    while (offset < entry.uncompressed_size)
    {
        UINT64 remaining = entry.uncompressed_size - offset;
        UINT32 capacity = remaining < APPX_BUNDLE_MATERIALIZE_BUFFER_SIZE ?
                          (UINT32)remaining :
                          APPX_BUNDLE_MATERIALIZE_BUFFER_SIZE;
        UINT32 read = 0;

        if (FAILED(hr = check_cancelled( cancel_event )))
        {
            wine_appx_archive_stream_cancel( stream );
            goto done;
        }
        hr = wine_appx_archive_stream_read( stream, buffer + offset,
                                            capacity, &read );
        if (hr == S_FALSE || FAILED(hr) || !read || read > capacity)
        {
            if (hr == S_FALSE || SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
        offset += read;
    }
    {
        UINT32 read = 0;

        if (FAILED(hr = check_cancelled( cancel_event )))
        {
            wine_appx_archive_stream_cancel( stream );
            goto done;
        }
        hr = wine_appx_archive_stream_read( stream, &terminal, 1, &read );
        if (hr != S_FALSE || read)
        {
            if (SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
    }

    *data = buffer;
    *size = (UINT32)entry.uncompressed_size;
    buffer = NULL;
    hr = S_OK;

done:
    if (stream) wine_appx_archive_stream_close( stream );
    HeapFree( GetProcessHeap(), 0, buffer );
    return hr;
}

static HRESULT init_private_security( struct private_security *security )
{
    DWORD acl_size, error, sid_size, token_size = 0;
    PSID sid;

    memset( security, 0, sizeof(*security) );
    if (!OpenThreadToken( GetCurrentThread(), TOKEN_QUERY, TRUE,
                          &security->token ))
    {
        error = GetLastError();
        if (error != ERROR_NO_TOKEN ||
            !OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY,
                               &security->token ))
            return HRESULT_FROM_WIN32( error == ERROR_NO_TOKEN ?
                                       GetLastError() : error );
    }
    if (GetTokenInformation( security->token, TokenUser, NULL, 0,
                             &token_size ))
        return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER ||
        token_size < sizeof(TOKEN_USER))
        return HRESULT_FROM_WIN32( error );
    if (!(security->user = HeapAlloc( GetProcessHeap(), 0, token_size )))
        return E_OUTOFMEMORY;
    if (!GetTokenInformation( security->token, TokenUser, security->user,
                              token_size, &token_size ))
    {
        error = GetLastError();
        return HRESULT_FROM_WIN32( error );
    }
    sid = security->user->User.Sid;
    if (!IsValidSid( sid ) ||
        !(sid_size = GetLengthSid( sid )) ||
        sid_size > SECURITY_MAX_SID_SIZE ||
        sid_size > MAXDWORD - sizeof(ACL) -
                   (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)))
        return HRESULT_FROM_WIN32( ERROR_INVALID_SID );
    acl_size = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
               sid_size;
    if (!(security->acl = HeapAlloc( GetProcessHeap(), 0, acl_size )))
        return E_OUTOFMEMORY;
    if (!InitializeAcl( security->acl, acl_size, ACL_REVISION ) ||
        !AddAccessAllowedAceEx( security->acl, ACL_REVISION, 0,
                                GENERIC_ALL, sid ) ||
        !InitializeSecurityDescriptor( &security->descriptor,
                                       SECURITY_DESCRIPTOR_REVISION ) ||
        !SetSecurityDescriptorOwner( &security->descriptor, sid, FALSE ) ||
        !SetSecurityDescriptorGroup( &security->descriptor, sid, FALSE ) ||
        !SetSecurityDescriptorDacl( &security->descriptor, TRUE,
                                    security->acl, FALSE ) ||
        !SetSecurityDescriptorControl( &security->descriptor,
                                       SE_DACL_PROTECTED,
                                       SE_DACL_PROTECTED ))
        return HRESULT_FROM_WIN32( GetLastError() );

    security->attributes.nLength = sizeof(security->attributes);
    security->attributes.lpSecurityDescriptor = &security->descriptor;
    security->attributes.bInheritHandle = FALSE;
    return S_OK;
}

static void free_private_security( struct private_security *security )
{
    HeapFree( GetProcessHeap(), 0, security->acl );
    HeapFree( GetProcessHeap(), 0, security->user );
    if (security->token) CloseHandle( security->token );
    memset( security, 0, sizeof(*security) );
}

static HRESULT verify_private_file_security(
    HANDLE file, const struct private_security *security )
{
    static const SID local_system_sid = {
        SID_REVISION, 1, {SECURITY_NT_AUTHORITY},
        {SECURITY_LOCAL_SYSTEM_RID}
    };
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SECURITY_DESCRIPTOR_CONTROL control;
    ACL_SIZE_INFORMATION information;
    ACCESS_ALLOWED_ACE *ace;
    PSID owner = NULL, group = NULL;
    PACL dacl = NULL;
    DWORD error, revision, i, system_aces = 0, user_aces = 0;
    BOOL control_ok, acl_ok, valid = TRUE;
    HRESULT hr = S_OK;

    if (!file || file == INVALID_HANDLE_VALUE || !security ||
        !security->user || !IsValidSid( security->user->User.Sid ))
        return E_INVALIDARG;
    error = GetSecurityInfo( file, SE_FILE_OBJECT,
                             OWNER_SECURITY_INFORMATION |
                             GROUP_SECURITY_INFORMATION |
                             DACL_SECURITY_INFORMATION,
                             &owner, &group, &dacl, NULL,
                             &descriptor );
    if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32( error );
    control_ok = descriptor &&
                 GetSecurityDescriptorControl(
                     descriptor, &control, &revision );
    acl_ok = dacl &&
             GetAclInformation( dacl, &information, sizeof(information),
                                AclSizeInformation );
    if (!descriptor || !owner || !group || !dacl ||
        !IsValidSid( owner ) || !IsValidSid( group ) ||
        !EqualSid( owner, security->user->User.Sid ) ||
        !control_ok || !(control & SE_DACL_PRESENT) ||
        (control & (SE_DACL_DEFAULTED | SE_DACL_AUTO_INHERIT_REQ |
                    SE_DACL_AUTO_INHERITED)) || !acl_ok ||
        (dacl->AclRevision != ACL_REVISION &&
         dacl->AclRevision != ACL_REVISION4) ||
        !information.AceCount || information.AceCount > 2)
        valid = FALSE;
    if (valid)
    {
        for (i = 0; i < information.AceCount; i++)
        {
            DWORD sid_size;

            if (!GetAce( dacl, i, (void **)&ace ))
            {
                valid = FALSE;
                break;
            }
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
                ace->Header.AceFlags ||
                ace->Header.AceSize <
                FIELD_OFFSET( ACCESS_ALLOWED_ACE, SidStart ) +
                FIELD_OFFSET( SID, SubAuthority ) ||
                (ace->Mask != GENERIC_ALL &&
                 ace->Mask != FILE_ALL_ACCESS) ||
                !IsValidSid( &ace->SidStart ))
            {
                valid = FALSE;
                break;
            }
            sid_size = GetLengthSid( &ace->SidStart );
            if (!sid_size ||
                ace->Header.AceSize !=
                FIELD_OFFSET( ACCESS_ALLOWED_ACE, SidStart ) + sid_size)
            {
                valid = FALSE;
                break;
            }
            if (EqualSid( &ace->SidStart, security->user->User.Sid ))
                user_aces++;
            else if (EqualSid( &ace->SidStart, (PSID)&local_system_sid ))
                system_aces++;
            else
            {
                valid = FALSE;
                break;
            }
        }
    }
    if (!valid || user_aces != 1 || system_aces > 1 ||
        user_aces + system_aces != information.AceCount)
        hr = HRESULT_FROM_WIN32( ERROR_INVALID_SECURITY_DESCR );
    TRACE( "private file security verification %#lx, ACEs user %lu, "
           "local-system %lu.\n", hr, user_aces, system_aces );
    if (descriptor) LocalFree( descriptor );
    return hr;
}

static HRESULT ntstatus_error( NTSTATUS status )
{
    DWORD error = RtlNtStatusToDosError( status );

    return error ? HRESULT_FROM_WIN32( error ) : E_FAIL;
}

static HRESULT complete_nt_io( HANDLE handle, IO_STATUS_BLOCK *io,
                               NTSTATUS status, NTSTATUS *completed )
{
    if (status == STATUS_PENDING)
    {
        DWORD wait = WaitForSingleObject( handle, INFINITE );

        if (wait != WAIT_OBJECT_0)
        {
            if (completed) *completed = STATUS_PENDING;
            return wait == WAIT_FAILED ?
                   HRESULT_FROM_WIN32( GetLastError() ) : E_FAIL;
        }
        status = io->Status;
    }
    if (completed) *completed = status;
    return status ? ntstatus_error( status ) : S_OK;
}

static HRESULT check_cancelled( HANDLE event )
{
    DWORD wait;

    if (!event) return S_OK;
    wait = WaitForSingleObject( event, 0 );
    if (wait == WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    if (wait == WAIT_TIMEOUT) return S_OK;
    return HRESULT_FROM_WIN32( wait == WAIT_FAILED ?
                               GetLastError() : ERROR_GEN_FAILURE );
}

static HRESULT check_directory_handle( HANDLE directory )
{
    FILE_ATTRIBUTE_TAG_INFO tag;
    BY_HANDLE_FILE_INFORMATION info;
    DWORD attributes;

    if (!directory || directory == INVALID_HANDLE_VALUE)
        return E_INVALIDARG;
    if (GetFileInformationByHandleEx( directory, FileAttributeTagInfo,
                                      &tag, sizeof(tag) ))
        attributes = tag.FileAttributes;
    else
    {
        if (!GetFileInformationByHandle( directory, &info ))
            return HRESULT_FROM_WIN32( GetLastError() );
        attributes = info.dwFileAttributes;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return HRESULT_FROM_WIN32( ERROR_DIRECTORY );
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                      FILE_ATTRIBUTE_COMPRESSED |
                      FILE_ATTRIBUTE_ENCRYPTED))
        return HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED );
    return S_OK;
}

static HRESULT acquire_materialization_directory(
    const APPX_BUNDLE_INSPECT_OPTIONS *options, HANDLE *directory )
{
    WCHAR path[MAX_PATH];
    DWORD length;
    HRESULT hr;

    *directory = INVALID_HANDLE_VALUE;
    if (options->temporary_directory)
    {
        if (options->temporary_directory == INVALID_HANDLE_VALUE)
            return E_INVALIDARG;
        if (!DuplicateHandle(
                GetCurrentProcess(), options->temporary_directory,
                GetCurrentProcess(), directory, 0, FALSE,
                DUPLICATE_SAME_ACCESS ))
            return HRESULT_FROM_WIN32( GetLastError() );
    }
    else
    {
        length = GetTempPathW( ARRAY_SIZE(path), path );
        if (!length) return HRESULT_FROM_WIN32( GetLastError() );
        if (length >= ARRAY_SIZE(path))
            return HRESULT_FROM_WIN32( ERROR_FILENAME_EXCED_RANGE );
        *directory = CreateFileW(
            path, FILE_LIST_DIRECTORY | FILE_ADD_FILE |
                  FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                                 FILE_FLAG_OPEN_REPARSE_POINT, NULL );
        if (*directory == INVALID_HANDLE_VALUE)
            return HRESULT_FROM_WIN32( GetLastError() );
    }
    if (FAILED(hr = check_directory_handle( *directory )))
    {
        CloseHandle( *directory );
        *directory = INVALID_HANDLE_VALUE;
    }
    return hr;
}

static HRESULT query_available_bytes( HANDLE directory, UINT64 *available )
{
    FILE_FS_FULL_SIZE_INFORMATION full;
    FILE_FS_SIZE_INFORMATION size;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER units;
    UINT64 bytes_per_unit;
    ULONG sectors, bytes;
    NTSTATUS status;
    HRESULT hr;

    *available = 0;
    status = NtQueryVolumeInformationFile(
        directory, &io, &full, sizeof(full),
        FileFsFullSizeInformation );
    hr = complete_nt_io( directory, &io, status, &status );
    if (SUCCEEDED(hr))
    {
        units = full.CallerAvailableAllocationUnits;
        sectors = full.SectorsPerAllocationUnit;
        bytes = full.BytesPerSector;
    }
    else
    {
        if (status != STATUS_INVALID_DEVICE_REQUEST &&
            status != STATUS_INVALID_INFO_CLASS &&
            status != STATUS_NOT_IMPLEMENTED &&
            status != STATUS_NOT_SUPPORTED)
            return hr;
        status = NtQueryVolumeInformationFile(
            directory, &io, &size, sizeof(size),
            FileFsSizeInformation );
        if (FAILED(hr = complete_nt_io(
              directory, &io, status, &status )))
            return hr;
        units = size.AvailableAllocationUnits;
        sectors = size.SectorsPerAllocationUnit;
        bytes = size.BytesPerSector;
    }
    if (units.QuadPart < 0 || !sectors || !bytes ||
        sectors > ~(UINT64)0 / bytes)
        return E_FAIL;
    bytes_per_unit = (UINT64)sectors * bytes;
    if ((UINT64)units.QuadPart > ~(UINT64)0 / bytes_per_unit)
        *available = ~(UINT64)0;
    else
        *available = (UINT64)units.QuadPart * bytes_per_unit;
    return S_OK;
}

static HRESULT check_materialization_space(
    HANDLE directory, UINT64 write_size, UINT64 floor )
{
    UINT64 required, available;
    HRESULT hr;

    if (write_size > ~(UINT64)0 - floor)
        return HRESULT_FROM_WIN32( ERROR_DISK_FULL );
    required = write_size + floor;
    if (FAILED(hr = query_available_bytes( directory, &available )))
        return hr;
    return available < required ?
           HRESULT_FROM_WIN32( ERROR_DISK_FULL ) : S_OK;
}

static HRESULT create_materialization_mutex(
    const struct private_security *security, HANDLE *mutex )
{
    static const WCHAR prefix[] =
        L"Local\\Wine.AppxSvc.Bundle.Materialize.";
    WCHAR *name = NULL, *sid = NULL;
    SIZE_T chars;
    HRESULT hr;

    *mutex = NULL;
    if (!ConvertSidToStringSidW( security->user->User.Sid, &sid ))
        return HRESULT_FROM_WIN32( GetLastError() );
    chars = ARRAY_SIZE(prefix) + lstrlenW( sid );
    if (chars > ~(SIZE_T)0 / sizeof(*name) ||
        !(name = HeapAlloc( GetProcessHeap(), 0,
                            chars * sizeof(*name) )))
    {
        LocalFree( sid );
        return E_OUTOFMEMORY;
    }
    lstrcpyW( name, prefix );
    lstrcatW( name, sid );
    *mutex = CreateMutexW(
        (SECURITY_ATTRIBUTES *)&security->attributes, FALSE, name );
    hr = *mutex ? S_OK : HRESULT_FROM_WIN32( GetLastError() );
    HeapFree( GetProcessHeap(), 0, name );
    LocalFree( sid );
    return hr;
}

static HRESULT acquire_materialization_mutex(
    HANDLE mutex, HANDLE cancel_event, DWORD timeout, BOOL *owned )
{
    HANDLE handles[2];
    DWORD wait;

    *owned = FALSE;
    if (FAILED(check_cancelled( cancel_event )))
        return HRESULT_FROM_WIN32( ERROR_CANCELLED );
    if (cancel_event)
    {
        handles[0] = cancel_event;
        handles[1] = mutex;
        wait = WaitForMultipleObjects(
            ARRAY_SIZE(handles), handles, FALSE, timeout );
        if (wait == WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32( ERROR_CANCELLED );
        if (wait == WAIT_OBJECT_0 + 1 ||
            wait == WAIT_ABANDONED_0 + 1)
            *owned = TRUE;
    }
    else
    {
        wait = WaitForSingleObject( mutex, timeout );
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)
            *owned = TRUE;
    }
    if (*owned) return S_OK;
    if (wait == WAIT_TIMEOUT)
        return HRESULT_FROM_WIN32( ERROR_TIMEOUT );
    return HRESULT_FROM_WIN32( wait == WAIT_FAILED ?
                               GetLastError() : ERROR_GEN_FAILURE );
}

static HRESULT create_private_temp_file(
    HANDLE directory, const struct private_security *security, HANDLE *file )
{
    static const WCHAR hex[] = L"0123456789abcdef";
    WCHAR name[ARRAY_SIZE(L"appxsvc-bundle-") +
               PRIVATE_TEMP_RANDOM_SIZE * 2 + ARRAY_SIZE(L".msix")];
    BYTE random[PRIVATE_TEMP_RANDOM_SIZE];
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING name_string;
    IO_STATUS_BLOCK io;
    UINT32 attempt, i;
    NTSTATUS status;
    HRESULT hr = HRESULT_FROM_WIN32( ERROR_ALREADY_EXISTS );

    *file = INVALID_HANDLE_VALUE;
    for (attempt = 0; attempt < PRIVATE_TEMP_RETRIES; attempt++)
    {
        status = BCryptGenRandom( NULL, random, sizeof(random),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG );
        if (status) return HRESULT_FROM_NT( status );
        lstrcpyW( name, L"appxsvc-bundle-" );
        for (i = 0; i < ARRAY_SIZE(random); i++)
        {
            name[15 + i * 2] = hex[random[i] >> 4];
            name[16 + i * 2] = hex[random[i] & 15];
        }
        lstrcpyW( name + 15 + ARRAY_SIZE(random) * 2, L".msix" );
        RtlInitUnicodeString( &name_string, name );
        InitializeObjectAttributes(
            &attributes, &name_string, OBJ_CASE_INSENSITIVE,
            directory, (void *)&security->descriptor );
        status = NtCreateFile(
            file, GENERIC_READ | GENERIC_WRITE | DELETE |
                  FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            &attributes, &io, NULL, FILE_ATTRIBUTE_TEMPORARY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE |
                         FILE_SEQUENTIAL_ONLY | FILE_SYNCHRONOUS_IO_NONALERT |
                         FILE_OPEN_REPARSE_POINT, NULL, 0 );
        if (!status)
        {
            if (SUCCEEDED(hr = verify_private_file_security(
                    *file, security )))
                return S_OK;
            CloseHandle( *file );
            *file = INVALID_HANDLE_VALUE;
            return hr;
        }
        if (status != STATUS_OBJECT_NAME_COLLISION)
            return ntstatus_error( status );
    }
    return hr;
}

typedef HRESULT (WINAPI *materialize_read_callback)(
    void *context, void *buffer, UINT32 capacity, UINT32 *read );
typedef HRESULT (WINAPI *materialize_inspect_callback)(
    void *context, HANDLE file );

static HRESULT normalize_inspect_options(
    const APPX_BUNDLE_INSPECT_OPTIONS *input,
    APPX_BUNDLE_INSPECT_OPTIONS *options )
{
    memset( options, 0, sizeof(*options) );
    options->size = sizeof(*options);
    options->version = APPX_BUNDLE_INSPECT_OPTIONS_VERSION;
    options->free_space_floor_bytes =
        APPX_BUNDLE_DEFAULT_SPACE_FLOOR;
    options->lock_timeout_ms =
        APPX_BUNDLE_DEFAULT_LOCK_TIMEOUT_MS;
    if (!input) return S_OK;
    if (input->size != sizeof(*input) ||
        input->version != APPX_BUNDLE_INSPECT_OPTIONS_VERSION ||
        input->reserved ||
        input->temporary_directory == INVALID_HANDLE_VALUE)
        return E_INVALIDARG;
    *options = *input;
    if (!options->lock_timeout_ms)
        options->lock_timeout_ms =
            APPX_BUNDLE_DEFAULT_LOCK_TIMEOUT_MS;
    return S_OK;
}

static HRESULT materialize_private_file(
    UINT64 expected_size, const APPX_BUNDLE_INSPECT_OPTIONS *input_options,
    void *context,
    materialize_read_callback read_callback,
    materialize_inspect_callback inspect_callback )
{
    struct private_security security;
    APPX_BUNDLE_INSPECT_OPTIONS options;
    HANDLE file = INVALID_HANDLE_VALUE, readonly = INVALID_HANDLE_VALUE;
    HANDLE restricted = INVALID_HANDLE_VALUE, directory = INVALID_HANDLE_VALUE;
    HANDLE mutex = NULL;
    BY_HANDLE_FILE_INFORMATION before, after, final;
    FILE_ATTRIBUTE_TAG_INFO tag;
    FILE_DISPOSITION_INFORMATION_EX disposition;
    FILE_STANDARD_INFO standard;
    IO_STATUS_BLOCK disposition_io;
    LARGE_INTEGER position, size;
    NTSTATUS disposition_status;
    BYTE *buffer = NULL;
    UINT64 offset = 0;
    BOOL mutex_owned = FALSE;
    HRESULT hr;

    if (!read_callback || !inspect_callback)
        return E_INVALIDARG;
    if (!expected_size) return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (FAILED(hr = normalize_inspect_options(
            input_options, &options )))
        return hr;
    if (FAILED(hr = init_private_security( &security ))) goto done;
    if (FAILED(hr = acquire_materialization_directory(
            &options, &directory )) ||
        FAILED(hr = create_materialization_mutex(
            &security, &mutex )) ||
        FAILED(hr = acquire_materialization_mutex(
            mutex, options.cancel_event, options.lock_timeout_ms,
            &mutex_owned )) ||
        FAILED(hr = check_materialization_space(
            directory, expected_size,
            options.free_space_floor_bytes )) ||
        FAILED(hr = check_cancelled( options.cancel_event )) ||
        FAILED(hr = create_private_temp_file(
            directory, &security, &file )))
        goto done;
    if (!(buffer = HeapAlloc( GetProcessHeap(), 0,
                              APPX_BUNDLE_MATERIALIZE_BUFFER_SIZE )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    while (offset < expected_size)
    {
        UINT64 remaining = expected_size - offset;
        UINT32 capacity = remaining < APPX_BUNDLE_MATERIALIZE_BUFFER_SIZE ?
                          (UINT32)remaining :
                          APPX_BUNDLE_MATERIALIZE_BUFFER_SIZE;
        UINT32 read = 0, written_offset = 0;

        if (FAILED(hr = check_cancelled( options.cancel_event )) ||
            FAILED(hr = check_materialization_space(
                directory, capacity,
                options.free_space_floor_bytes )))
            goto done;
        hr = read_callback( context, buffer, capacity, &read );
        if (hr == S_FALSE || FAILED(hr) || !read || read > capacity)
        {
            if (hr == S_FALSE || SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
        if (FAILED(hr = check_cancelled( options.cancel_event )) ||
            FAILED(hr = check_materialization_space(
                directory, read, options.free_space_floor_bytes )))
            goto done;
        while (written_offset < read)
        {
            DWORD written = 0;

            if (!WriteFile( file, buffer + written_offset,
                             read - written_offset, &written, NULL ))
            {
                hr = HRESULT_FROM_WIN32( GetLastError() );
                goto done;
            }
            if (!written || written > read - written_offset)
            {
                hr = HRESULT_FROM_WIN32( ERROR_WRITE_FAULT );
                goto done;
            }
            written_offset += written;
            if (FAILED(hr = check_cancelled(
                    options.cancel_event )))
                goto done;
        }
        offset += read;
    }
    {
        BYTE terminal;
        UINT32 read = 0;

        if (FAILED(hr = check_cancelled( options.cancel_event )))
            goto done;
        hr = read_callback( context, &terminal, 1, &read );
        if (hr != S_FALSE || read)
        {
            if (SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
    }
    if (!FlushFileBuffers( file ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (FAILED(hr = check_materialization_space(
            directory, 0, options.free_space_floor_bytes )) ||
        FAILED(hr = check_cancelled( options.cancel_event )))
        goto done;
    if (!GetFileSizeEx( file, &size ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (size.QuadPart < 0 || (UINT64)size.QuadPart != expected_size)
    {
        hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        goto done;
    }
    if (!GetFileInformationByHandleEx( file, FileStandardInfo, &standard,
                                       sizeof(standard) ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (standard.Directory || standard.NumberOfLinks != 1)
    {
        hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        goto done;
    }
    if (!GetFileInformationByHandleEx( file, FileAttributeTagInfo, &tag,
                                       sizeof(tag) ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        goto done;
    }
    if (!GetFileInformationByHandle( file, &before ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    readonly = ReOpenFile( file, GENERIC_READ | DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                           FILE_SHARE_DELETE, FILE_FLAG_SEQUENTIAL_SCAN );
    if (readonly == INVALID_HANDLE_VALUE)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (!GetFileInformationByHandle( readonly, &after ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
        before.nFileIndexHigh != after.nFileIndexHigh ||
        before.nFileIndexLow != after.nFileIndexLow)
    {
        hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        goto done;
    }
    CloseHandle( file );
    file = INVALID_HANDLE_VALUE;

    /*
     * The writer must share writes while it exists so the first read-only
     * reopen can coexist with it.  Once the writer is closed, establish a
     * second handle that denies new writers.  If another writer won the
     * narrow transition race, this reopen fails closed.
     */
    restricted = ReOpenFile( readonly, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             FILE_FLAG_SEQUENTIAL_SCAN );
    if (restricted == INVALID_HANDLE_VALUE)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (!GetFileInformationByHandle( restricted, &final ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (before.dwVolumeSerialNumber != final.dwVolumeSerialNumber ||
        before.nFileIndexHigh != final.nFileIndexHigh ||
        before.nFileIndexLow != final.nFileIndexLow)
    {
        hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        goto done;
    }
    if (!GetFileSizeEx( restricted, &size ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (size.QuadPart < 0 || (UINT64)size.QuadPart != expected_size)
    {
        hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        goto done;
    }
    position.QuadPart = 0;
    if (!SetFilePointerEx( restricted, position, NULL, FILE_BEGIN ))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        goto done;
    }
    if (SUCCEEDED(hr = check_cancelled( options.cancel_event )))
        hr = inspect_callback( context, restricted );
    memset( &disposition, 0, sizeof(disposition) );
    disposition.Flags = FILE_DISPOSITION_DELETE |
                        FILE_DISPOSITION_POSIX_SEMANTICS;
    disposition_status = NtSetInformationFile(
        readonly, &disposition_io, &disposition, sizeof(disposition),
        FileDispositionInformationEx );
    if (disposition_status != STATUS_SUCCESS &&
        disposition_status != STATUS_INVALID_INFO_CLASS &&
        SUCCEEDED(hr))
        hr = ntstatus_error( disposition_status );
    CloseHandle( readonly );
    readonly = INVALID_HANDLE_VALUE;

done:
    if (restricted != INVALID_HANDLE_VALUE) CloseHandle( restricted );
    if (readonly != INVALID_HANDLE_VALUE) CloseHandle( readonly );
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
    HeapFree( GetProcessHeap(), 0, buffer );
    if (mutex_owned) ReleaseMutex( mutex );
    if (mutex) CloseHandle( mutex );
    if (directory != INVALID_HANDLE_VALUE) CloseHandle( directory );
    free_private_security( &security );
    return hr;
}

HRESULT WINAPI appx_bundle_materialize_with_test_source(
    UINT64 expected_size,
    const struct appx_bundle_materialize_test_source *source )
{
    if (!source || source->size != sizeof(*source) ||
        !source->read || !source->inspect)
        return E_INVALIDARG;
    return materialize_private_file(
                                     expected_size, source->options,
                                     source->context,
                                     source->read, source->inspect );
}

struct hash_engine
{
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE block;
    BCRYPT_HASH_HANDLE file;
};

static HRESULT hash_engine_open( struct hash_engine *engine )
{
    NTSTATUS status;

    memset( engine, 0, sizeof(*engine) );
    if ((status = BCryptOpenAlgorithmProvider( &engine->algorithm,
                                               BCRYPT_SHA256_ALGORITHM, NULL,
                                               BCRYPT_HASH_REUSABLE_FLAG )))
        return HRESULT_FROM_NT( status );
    if ((status = BCryptCreateHash( engine->algorithm, &engine->block,
                                    NULL, 0, NULL, 0,
                                    BCRYPT_HASH_REUSABLE_FLAG )) ||
        (status = BCryptCreateHash( engine->algorithm, &engine->file,
                                    NULL, 0, NULL, 0,
                                    BCRYPT_HASH_REUSABLE_FLAG )))
    {
        if (engine->block) BCryptDestroyHash( engine->block );
        BCryptCloseAlgorithmProvider( engine->algorithm, 0 );
        memset( engine, 0, sizeof(*engine) );
        return HRESULT_FROM_NT( status );
    }
    return S_OK;
}

static void hash_engine_close( struct hash_engine *engine )
{
    if (engine->file) BCryptDestroyHash( engine->file );
    if (engine->block) BCryptDestroyHash( engine->block );
    if (engine->algorithm) BCryptCloseAlgorithmProvider( engine->algorithm, 0 );
}

static HRESULT hash_data( BCRYPT_HASH_HANDLE hash, const BYTE *data,
                          UINT32 size )
{
    NTSTATUS status = BCryptHashData( hash, (BYTE *)data, size, 0 );

    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

static HRESULT finish_hash( BCRYPT_HASH_HANDLE hash,
                            BYTE digest[APPX_BLOCK_MAP_HASH_SIZE] )
{
    NTSTATUS status = BCryptFinishHash( hash, digest,
                                        APPX_BLOCK_MAP_HASH_SIZE, 0 );

    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

static HRESULT reconcile_manifest_entry(
    const APPX_BLOCK_MAP *map, const APPX_BLOCK_MAP_FILE *mapped,
    const WINE_APPX_ARCHIVE_ENTRY *entry )
{
    UINT64 compressed_blocks = 0;
    UINT32 i;

    if (entry->flags & WINE_APPX_ENTRY_DIRECTORY ||
        entry->uncompressed_size != mapped->size ||
        entry->data_offset < entry->local_header_offset ||
        entry->data_offset - entry->local_header_offset !=
        mapped->local_file_header_size)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (entry->compression_method == ZIP_METHOD_STORE)
    {
        if (entry->compressed_size != entry->uncompressed_size)
            return APPX_E_FILE_COMPRESSION_MISMATCH;
        for (i = 0; i < mapped->block_count; i++)
        {
            const APPX_BLOCK_MAP_BLOCK *block =
                appx_block_map_get_block( map, 0, i );

            if (!block || block->has_compressed_size)
                return APPX_E_FILE_COMPRESSION_MISMATCH;
        }
        return S_OK;
    }
    if (entry->compression_method != ZIP_METHOD_DEFLATE)
        return APPX_E_FILE_COMPRESSION_MISMATCH;
    for (i = 0; i < mapped->block_count; i++)
    {
        const APPX_BLOCK_MAP_BLOCK *block =
            appx_block_map_get_block( map, 0, i );

        if (!block || !block->has_compressed_size ||
            compressed_blocks > ~(UINT64)0 - block->compressed_size)
            return APPX_E_FILE_COMPRESSION_MISMATCH;
        compressed_blocks += block->compressed_size;
    }
    if (compressed_blocks != entry->compressed_size &&
        (compressed_blocks > ~(UINT64)0 - 2 ||
         compressed_blocks + 2 != entry->compressed_size))
        return APPX_E_FILE_COMPRESSION_MISMATCH;
    return S_OK;
}

static HRESULT verify_manifest_stream( WINE_APPX_ARCHIVE *archive,
                                       const APPX_BLOCK_MAP *map,
                                       UINT32 archive_index,
                                       const APPX_BLOCK_MAP_FILE *mapped,
                                       HANDLE cancel_event )
{
    WINE_APPX_ARCHIVE_STREAM *stream = NULL;
    BYTE buffer[APPX_BLOCK_MAP_BLOCK_SIZE];
    BYTE digest[APPX_BLOCK_MAP_HASH_SIZE], terminal;
    struct hash_engine engine;
    UINT32 i;
    HRESULT hr;

    if (FAILED(hr = hash_engine_open( &engine ))) return hr;
    if (FAILED(hr = wine_appx_archive_stream_open( archive, archive_index,
                                                   &stream )))
        goto done;
    for (i = 0; i < mapped->block_count; i++)
    {
        const APPX_BLOCK_MAP_BLOCK *block =
            appx_block_map_get_block( map, 0, i );
        UINT32 offset = 0;

        if (!block)
        {
            hr = APPX_E_INVALID_BLOCKMAP;
            goto done;
        }
        while (offset < block->logical_size)
        {
            UINT32 read = 0, remaining = block->logical_size - offset;

            if (FAILED(hr = check_cancelled( cancel_event )))
            {
                wine_appx_archive_stream_cancel( stream );
                goto done;
            }
            hr = wine_appx_archive_stream_read( stream, buffer + offset,
                                                remaining, &read );
            if (hr == S_FALSE || FAILED(hr) || !read || read > remaining)
            {
                if (hr == S_FALSE || SUCCEEDED(hr))
                    hr = APPX_E_CORRUPT_CONTENT;
                goto done;
            }
            if (FAILED(hr = hash_data( engine.block, buffer + offset, read )))
                goto done;
            if (mapped->has_file_hash &&
                FAILED(hr = hash_data( engine.file, buffer + offset, read )))
                goto done;
            offset += read;
        }
        if (FAILED(hr = finish_hash( engine.block, digest ))) goto done;
        if (!constant_equal( digest, block->hash, sizeof(digest) ))
        {
            hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
    }
    {
        UINT32 read = 0;

        if (FAILED(hr = check_cancelled( cancel_event )))
        {
            wine_appx_archive_stream_cancel( stream );
            goto done;
        }
        hr = wine_appx_archive_stream_read( stream, &terminal, 1, &read );
        if (hr != S_FALSE || read)
        {
            if (SUCCEEDED(hr)) hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
    }
    if (mapped->has_file_hash)
    {
        if (FAILED(hr = finish_hash( engine.file, digest ))) goto done;
        if (!constant_equal( digest, mapped->file_hash, sizeof(digest) ))
        {
            hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
    }
    hr = S_OK;

done:
    if (stream) wine_appx_archive_stream_close( stream );
    hash_engine_close( &engine );
    return hr;
}

static HRESULT validate_outer_block_map( WINE_APPX_ARCHIVE *archive,
                                         const APPX_BLOCK_MAP *map,
                                         HANDLE cancel_event )
{
    const APPX_BLOCK_MAP_FILE *mapped;
    WINE_APPX_ARCHIVE_ENTRY entry;
    WCHAR *path = NULL;
    UINT32 index;
    HRESULT hr;

    if (appx_block_map_get_file_count( map ) != 1 ||
        !(mapped = appx_block_map_get_file( map, 0 )) ||
        !exact_path( mapped->name, bundle_manifest_path ))
        return APPX_E_INVALID_BLOCKMAP;
    if (FAILED(hr = wine_appx_archive_find_entry( archive,
                                                  bundle_manifest_path,
                                                  &index )))
        return APPX_E_INVALID_BLOCKMAP;
    if (FAILED(hr = get_entry_path( archive, index, &entry, &path )))
        return hr;
    if (!exact_path( path, bundle_manifest_path ))
        hr = APPX_E_INVALID_PACKAGING_LAYOUT;
    else if (SUCCEEDED(hr = reconcile_manifest_entry( map, mapped, &entry )))
        hr = verify_manifest_stream(
            archive, map, index, mapped, cancel_event );
    HeapFree( GetProcessHeap(), 0, path );
    return hr;
}

static BOOL is_outer_footprint( const WCHAR *path )
{
    return exact_path( path, bundle_manifest_path ) ||
           exact_path( path, block_map_path ) ||
           exact_path( path, content_types_path ) ||
           exact_path( path, signature_path ) ||
           exact_path( path, code_integrity_path );
}

static BOOL valid_entry_content_type(
    const struct appx_bundle_source_entry *entry, BOOL embedded_package )
{
    if (exact_path( entry->path, content_types_path ))
        return TRUE;
    if (!entry->content_type) return FALSE;
    if (exact_path( entry->path, bundle_manifest_path ))
        return !lstrcmpW( entry->content_type,
                          APPX_CONTENT_TYPE_BUNDLE_MANIFEST );
    if (exact_path( entry->path, block_map_path ))
        return !lstrcmpW( entry->content_type,
                          APPX_CONTENT_TYPE_BLOCK_MAP );
    if (exact_path( entry->path, signature_path ))
        return !lstrcmpW( entry->content_type,
                          APPX_CONTENT_TYPE_SIGNATURE );
    if (embedded_package)
        return !lstrcmpW( entry->content_type, embedded_package_type ) ||
               !lstrcmpW( entry->content_type,
                           embedded_package_type_alt );
    /* CodeIntegrity.cat has no mode-specific required MIME value. */
    return exact_path( entry->path, code_integrity_path );
}

struct declared_package_lookup
{
    const struct appx_bundle_package *package;
    UINT32 manifest_index;
};

static int compare_wstring_ci( const WCHAR *left, const WCHAR *right )
{
    int result = CompareStringOrdinal( left, -1, right, -1, TRUE );

    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return 0;
}

static int compare_declared_package_path( const void *left, const void *right )
{
    const struct declared_package_lookup *a = left, *b = right;

    return compare_wstring_ci( a->package->file_name,
                               b->package->file_name );
}

static int compare_declared_package_range( const void *left,
                                           const void *right )
{
    const struct declared_package_lookup *a = left, *b = right;

    if (a->package->offset != b->package->offset)
        return a->package->offset < b->package->offset ? -1 : 1;
    if (a->package->size != b->package->size)
        return a->package->size < b->package->size ? -1 : 1;
    return compare_declared_package_path( left, right );
}

static const struct appx_bundle_package *find_declared_package(
    const struct declared_package_lookup *lookup, UINT32 count,
    const WCHAR *path, UINT32 *package_index )
{
    UINT32 low = 0, high = count;

    while (low < high)
    {
        UINT32 middle = low + (high - low) / 2;
        int result = compare_wstring_ci(
            path, lookup[middle].package->file_name );

        if (result < 0)
            high = middle;
        else if (result > 0)
            low = middle + 1;
        else
        {
            /* The archive path must also preserve the manifest's exact case. */
            if (!exact_path( path, lookup[middle].package->file_name ))
                return NULL;
            if (package_index)
                *package_index = lookup[middle].manifest_index;
            return lookup[middle].package;
        }
    }
    return NULL;
}

static HRESULT validate_source_layout(
    const APPX_BUNDLE_MANIFEST *manifest,
    const struct appx_bundle_test_source *source,
    UINT32 **package_entries )
{
    UINT32 package_count = appx_bundle_manifest_get_package_count( manifest );
    UINT32 footprint_mask = 0;
    struct declared_package_lookup *lookup = NULL;
    UINT32 *indices = NULL, i;
    HRESULT hr;

    *package_entries = NULL;
    if (!package_count || package_count > APPX_BUNDLE_MANIFEST_MAX_PACKAGES ||
        source->entry_count < package_count + 4)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!(indices = HeapAlloc( GetProcessHeap(), 0,
                               (SIZE_T)package_count * sizeof(*indices) )))
        return E_OUTOFMEMORY;
    if (!(lookup = HeapAlloc( GetProcessHeap(), 0,
                              (SIZE_T)package_count * sizeof(*lookup) )))
    {
        HeapFree( GetProcessHeap(), 0, indices );
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < package_count; i++)
    {
        indices[i] = MAXDWORD;
        lookup[i].package = appx_bundle_manifest_get_package( manifest, i );
        lookup[i].manifest_index = i;
        if (!lookup[i].package)
        {
            hr = APPX_E_INVALID_MANIFEST;
            goto failed;
        }
    }
    qsort( lookup, package_count, sizeof(*lookup),
           compare_declared_package_path );
    for (i = 1; i < package_count; i++)
    {
        if (!compare_declared_package_path( lookup + i - 1, lookup + i ))
        {
            hr = APPX_E_INVALID_MANIFEST;
            goto failed;
        }
    }

    for (i = 0; i < source->entry_count; i++)
    {
        struct appx_bundle_source_entry entry = {sizeof(entry)};
        const struct appx_bundle_package *package;
        UINT32 package_index;

        if (FAILED(hr = source->get_entry( source->context, i, &entry )))
            goto failed;
        if (entry.size != sizeof(entry) || !entry.path || entry.reserved ||
            (entry.flags & ~(WINE_APPX_ENTRY_DIRECTORY |
                             WINE_APPX_ENTRY_DATA_DESCRIPTOR)))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
        if (entry.flags & WINE_APPX_ENTRY_DIRECTORY)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
        if (is_outer_footprint( entry.path ))
        {
            UINT32 bit = exact_path( entry.path, bundle_manifest_path ) ? 0x01 :
                         exact_path( entry.path, block_map_path ) ? 0x02 :
                         exact_path( entry.path, content_types_path ) ? 0x04 :
                         exact_path( entry.path, signature_path ) ? 0x08 : 0x10;

            if ((footprint_mask & bit) ||
                !valid_entry_content_type( &entry, FALSE ))
            {
                hr = APPX_E_INVALID_PACKAGING_LAYOUT;
                goto failed;
            }
            footprint_mask |= bit;
            continue;
        }
        if (!(package = find_declared_package(
                lookup, package_count, entry.path, &package_index )))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
        if (!valid_entry_content_type( &entry, TRUE ))
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
        if (indices[package_index] != MAXDWORD ||
            !(package->flags & APPX_BUNDLE_PACKAGE_HAS_RANGE))
        {
            hr = !(package->flags & APPX_BUNDLE_PACKAGE_HAS_RANGE) ?
                 HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED ) :
                 APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
        if (entry.compression_method != ZIP_METHOD_STORE ||
            entry.compressed_size != entry.uncompressed_size ||
            entry.uncompressed_size != package->size ||
            entry.data_offset != package->offset)
        {
            hr = entry.compression_method != ZIP_METHOD_STORE ?
                 HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED ) :
                 APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
        indices[package_index] = i;
    }

    if ((footprint_mask & 0x0f) != 0x0f)
    {
        hr = APPX_E_MISSING_REQUIRED_FILE;
        goto failed;
    }
    for (i = 0; i < package_count; i++)
    {
        const struct appx_bundle_package *package =
            appx_bundle_manifest_get_package( manifest, i );

        if (!(package->flags & APPX_BUNDLE_PACKAGE_HAS_RANGE))
        {
            hr = HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );
            goto failed;
        }
        if (indices[i] == MAXDWORD)
        {
            hr = APPX_E_MISSING_REQUIRED_FILE;
            goto failed;
        }
        if (package->offset > ~(UINT64)0 - package->size)
        {
            hr = APPX_E_INVALID_MANIFEST;
            goto failed;
        }
    }
    qsort( lookup, package_count, sizeof(*lookup),
           compare_declared_package_range );
    for (i = 1; i < package_count; i++)
    {
        const struct appx_bundle_package *previous = lookup[i - 1].package;
        const struct appx_bundle_package *current = lookup[i].package;

        if (current->offset < previous->offset + previous->size)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto failed;
        }
    }
    HeapFree( GetProcessHeap(), 0, lookup );
    *package_entries = indices;
    return S_OK;

failed:
    HeapFree( GetProcessHeap(), 0, lookup );
    HeapFree( GetProcessHeap(), 0, indices );
    return hr;
}

static HRESULT validate_inner_identity(
    const struct appx_bundle_identity *outer,
    const struct appx_bundle_package *declared,
    const BYTE outer_signer_id[APPX_BUNDLE_SIGNER_ID_SIZE],
    const struct appx_bundle_source_package *inner )
{
    BOOL expected_framework, expected_resource;

    if (!outer || !declared || !inner ||
        inner->size != sizeof(*inner) || !inner->name ||
        !inner->publisher || !inner->resource_id)
        return APPX_E_INVALID_MANIFEST;
    if (!constant_equal( outer_signer_id, inner->signer_id,
                         APPX_BUNDLE_SIGNER_ID_SIZE ))
        return APPX_E_DIGEST_MISMATCH;
    expected_framework =
        declared->type == APPX_BUNDLE_PACKAGE_FRAMEWORK;
    expected_resource =
        declared->type == APPX_BUNDLE_PACKAGE_RESOURCE;
    if (lstrcmpW( outer->name, inner->name ) ||
        lstrcmpW( outer->publisher, inner->publisher ) ||
        lstrcmpW( declared->resource_id, inner->resource_id ) ||
        !versions_equal( &declared->version, &inner->version ) ||
        !architecture_equal( declared->architecture,
                             inner->architecture ) ||
        !!inner->framework != expected_framework ||
        !!inner->resource != expected_resource ||
        (inner->framework && inner->resource))
        return APPX_E_INVALID_MANIFEST;
    if (declared->type == APPX_BUNDLE_PACKAGE_APPLICATION &&
        (inner->framework || inner->resource))
        return APPX_E_INVALID_MANIFEST;
    return S_OK;
}

HRESULT WINAPI appx_bundle_validate_with_test_source(
    const APPX_BUNDLE_MANIFEST *manifest,
    const BYTE outer_signer_id[APPX_BUNDLE_SIGNER_ID_SIZE],
    const struct appx_bundle_selection_policy *policy,
    const struct appx_bundle_test_source *source,
    struct appx_bundle_selection *selection )
{
    const struct appx_bundle_identity *identity;
    UINT64 total_expanded = 0;
    UINT32 *package_entries = NULL, package_count, i;
    HRESULT hr;

    if (!selection || selection->size != sizeof(*selection))
        return E_INVALIDARG;
    memset( selection, 0, sizeof(*selection) );
    selection->size = sizeof(*selection);
    if (!manifest || !outer_signer_id ||
        FAILED(appx_bundle_selection_policy_validate_architecture( policy )) ||
        !source || source->size != sizeof(*source) ||
        !source->max_total_expanded_size ||
        !source->get_entry || !source->inspect_package)
        return E_INVALIDARG;
    if (!(identity = appx_bundle_manifest_get_identity( manifest )))
        return APPX_E_INVALID_MANIFEST;
    if (FAILED(hr = validate_source_layout( manifest, source,
                                            &package_entries )))
        return hr;

    package_count = appx_bundle_manifest_get_package_count( manifest );
    for (i = 0; i < package_count; i++)
    {
        const struct appx_bundle_package *declared =
            appx_bundle_manifest_get_package( manifest, i );
        struct appx_bundle_source_package inner = {sizeof(inner)};
        UINT64 remaining =
            source->max_total_expanded_size - total_expanded;

        if (!remaining)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
        if (FAILED(hr = source->inspect_package(
            source->context, i, package_entries[i], remaining, &inner )))
            goto done;
        if (FAILED(hr = validate_inner_identity( identity, declared,
                                                 outer_signer_id, &inner )))
            goto done;
        if (inner.expanded_size > remaining)
        {
            hr = APPX_E_INVALID_PACKAGING_LAYOUT;
            goto done;
        }
        total_expanded += inner.expanded_size;
    }

    hr = appx_bundle_manifest_select( manifest, policy, selection );
    if (SUCCEEDED(hr) && (selection->issues &
        (APPX_BUNDLE_SELECTION_RESOURCE_PAYLOAD |
         APPX_BUNDLE_SELECTION_MATCHING_RESOURCE_PAYLOAD |
         APPX_BUNDLE_SELECTION_OPTIONAL_PAYLOAD |
         APPX_BUNDLE_SELECTION_ENCRYPTED_PAYLOAD |
         APPX_BUNDLE_SELECTION_UNSUPPORTED_TYPE |
         APPX_BUNDLE_SELECTION_UNSUPPORTED_ARCHITECTURE |
         APPX_BUNDLE_SELECTION_UNSUPPORTED_EXTENSION |
         APPX_BUNDLE_SELECTION_UNSUPPORTED_QUALIFIER)))
        hr = HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED );

done:
    HeapFree( GetProcessHeap(), 0, package_entries );
    return hr;
}

struct stable_archive_entry
{
    struct appx_bundle_source_entry entry;
    WCHAR *path;
};

struct stable_archive_source
{
    struct archive_source production;
    const APPX_BUNDLE_MANIFEST *manifest;
    struct stable_archive_entry *entries;
    UINT32 count;
};

static HRESULT WINAPI stable_get_entry_callback(
    void *context, UINT32 index, struct appx_bundle_source_entry *entry )
{
    struct stable_archive_source *source = context;

    if (!entry || entry->size != sizeof(*entry) || index >= source->count)
        return E_INVALIDARG;
    *entry = source->entries[index].entry;
    return S_OK;
}

struct archive_stream_context
{
    WINE_APPX_ARCHIVE_STREAM *stream;
    struct archive_source *source;
    APPX_PACKAGE_INSPECTION *inspection;
};

static HRESULT WINAPI archive_stream_read_callback(
    void *context, void *buffer, UINT32 capacity, UINT32 *read )
{
    struct archive_stream_context *stream = context;
    HRESULT hr;

    if (FAILED(hr = check_cancelled(
            stream->source->options.cancel_event )))
    {
        wine_appx_archive_stream_cancel( stream->stream );
        *read = 0;
        return hr;
    }
    return wine_appx_archive_stream_read( stream->stream, buffer,
                                          capacity, read );
}

static HRESULT WINAPI archive_package_inspect_callback(
    void *context, HANDLE file )
{
    struct archive_stream_context *stream = context;

    return appx_package_inspect_ex(
        file, stream->source->limits, stream->source->package_flags,
        stream->source->options.cancel_event, &stream->inspection );
}

static void free_materialized_package( struct materialized_package *package )
{
    if (!package) return;
    appx_package_inspection_free( package->inspection );
    memset( package, 0, sizeof(*package) );
}

static HRESULT materialize_archive_package(
    struct archive_source *source, UINT32 archive_index, UINT64 expected_size,
    struct materialized_package *package )
{
    struct archive_stream_context context;
    HRESULT hr;

    memset( package, 0, sizeof(*package) );
    memset( &context, 0, sizeof(context) );
    context.source = source;
    if (FAILED(hr = wine_appx_archive_stream_open( source->archive,
                                                   archive_index,
                                                   &context.stream )))
        return hr;
    hr = materialize_private_file( expected_size, &source->options, &context,
                                   archive_stream_read_callback,
                                   archive_package_inspect_callback );
    wine_appx_archive_stream_close( context.stream );
    if (SUCCEEDED(hr))
        package->inspection = context.inspection;
    else
        appx_package_inspection_free( context.inspection );
    return hr;
}

static HRESULT WINAPI archive_inspect_package_callback(
    void *context, UINT32 manifest_index, UINT32 archive_index,
    UINT64 remaining_expanded_size,
    struct appx_bundle_source_package *package )
{
    struct stable_archive_source *stable = context;
    struct archive_source *source = &stable->production;
    const APPX_MANIFEST *manifest;
    const struct appx_manifest_identity *identity;
    const WINE_APPX_ARCHIVE_LIMITS *original_limits = source->limits;
    APPX_PACKAGE_INSPECTION *current_inspection;
    struct materialized_package materialized;
    WINE_APPX_ARCHIVE_LIMITS limited;
    HRESULT hr;

    appx_package_inspection_free( source->current );
    source->current = NULL;
    if (!appx_bundle_manifest_get_package( stable->manifest, manifest_index ))
        return APPX_E_INVALID_MANIFEST;
    if (archive_index >= stable->count)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!remaining_expanded_size)
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (original_limits)
        limited = *original_limits;
    else
    {
        memset( &limited, 0, sizeof(limited) );
        limited.size = sizeof(limited);
        limited.max_entries = 65536;
        limited.max_archive_size = 128ULL * 1024 * 1024 * 1024;
        limited.max_central_directory_size = 64ULL * 1024 * 1024;
        limited.max_entry_compressed_size = 16ULL * 1024 * 1024 * 1024;
        limited.max_entry_uncompressed_size =
            16ULL * 1024 * 1024 * 1024;
        limited.max_total_uncompressed_size =
            DEFAULT_MAX_TOTAL_INNER_EXPANDED_SIZE;
        limited.max_compression_ratio = 1000;
        limited.compression_ratio_slack = 1024 * 1024;
    }
    if (limited.max_total_uncompressed_size >
        remaining_expanded_size)
        limited.max_total_uncompressed_size =
            remaining_expanded_size;
    source->limits = &limited;
    if (FAILED(hr = materialize_archive_package(
        source, archive_index,
        stable->entries[archive_index].entry.uncompressed_size,
        &materialized )))
    {
        source->limits = original_limits;
        return hr;
    }
    source->limits = original_limits;
    if (manifest_index == source->retained_index)
    {
        if (source->retained.inspection)
        {
            free_materialized_package( &materialized );
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        }
        source->retained = materialized;
        memset( &materialized, 0, sizeof(materialized) );
        current_inspection = source->retained.inspection;
    }
    else
    {
        source->current = materialized.inspection;
        memset( &materialized, 0, sizeof(materialized) );
        current_inspection = source->current;
    }
    if (!(manifest = appx_package_inspection_get_manifest(
        current_inspection )) ||
        !(identity = appx_manifest_get_identity( manifest )))
        return APPX_E_INVALID_MANIFEST;
    package->name = identity->name;
    package->publisher = identity->publisher;
    package->resource_id = identity->resource_id;
    package->version = identity->version;
    package->architecture = identity->architecture;
    package->framework = appx_manifest_is_framework( manifest );
    package->resource = appx_manifest_is_resource_package( manifest );
    package->expanded_size =
        appx_package_inspection_get_archive_expanded_size(
            current_inspection );
    return appx_package_inspection_get_signer_id(
        current_inspection, package->signer_id, sizeof(package->signer_id) );
}

static void free_stable_archive_source( struct stable_archive_source *source )
{
    UINT32 i;

    appx_package_inspection_free( source->production.current );
    free_materialized_package( &source->production.retained );
    for (i = 0; i < source->count; i++)
        HeapFree( GetProcessHeap(), 0, source->entries[i].path );
    HeapFree( GetProcessHeap(), 0, source->entries );
    memset( source, 0, sizeof(*source) );
}

static HRESULT build_stable_archive_source(
    WINE_APPX_ARCHIVE *archive, const WINE_APPX_ARCHIVE_LIMITS *limits,
    UINT32 package_flags,
    const APPX_BUNDLE_INSPECT_OPTIONS *options,
    struct stable_archive_source *source )
{
    WINE_APPX_ARCHIVE_ENTRY archive_entry;
    UINT32 count, i;
    HRESULT hr;

    memset( source, 0, sizeof(*source) );
    source->production.archive = archive;
    source->production.limits = limits;
    source->production.options = *options;
    source->production.package_flags = package_flags;
    source->production.retained_index = MAXDWORD;
    if (FAILED(hr = wine_appx_archive_get_count( archive, &count )))
        return hr;
    if (!count || count > 65536 ||
        !(source->entries = HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            (SIZE_T)count * sizeof(*source->entries) )))
        return !count || count > 65536 ?
               APPX_E_INVALID_PACKAGING_LAYOUT : E_OUTOFMEMORY;
    source->count = count;
    for (i = 0; i < count; i++)
    {
        struct stable_archive_entry *stable = source->entries + i;

        if (FAILED(hr = check_cancelled(
                source->production.options.cancel_event )))
        {
            free_stable_archive_source( source );
            return hr;
        }
        if (FAILED(hr = get_entry_path( archive, i, &archive_entry,
                                        &stable->path )))
        {
            free_stable_archive_source( source );
            return hr;
        }
        stable->entry.size = sizeof(stable->entry);
        stable->entry.path = stable->path;
        stable->entry.flags = archive_entry.flags;
        stable->entry.compression_method = archive_entry.compression_method;
        stable->entry.reserved = archive_entry.reserved;
        stable->entry.compressed_size = archive_entry.compressed_size;
        stable->entry.uncompressed_size = archive_entry.uncompressed_size;
        stable->entry.data_offset = archive_entry.data_offset;
    }
    return S_OK;
}

static HRESULT validate_outer_content_types(
    const APPX_CONTENT_TYPES *types,
    struct stable_archive_source *source )
{
    UINT32 i, j;

    for (i = 0; i < source->count; i++)
    {
        const WCHAR *path = source->entries[i].entry.path, *content_type;
        WCHAR *part;
        UINT32 length;
        HRESULT hr;

        if (FAILED(hr = check_cancelled(
                source->production.options.cancel_event )))
            return hr;
        if (source->entries[i].entry.flags & WINE_APPX_ENTRY_DIRECTORY)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        if (exact_path( path, content_types_path )) continue;
        length = lstrlenW( path );
        if (length >= WINE_APPX_MAX_PATH_CHARS - 1)
            return APPX_E_INVALID_PACKAGING_LAYOUT;
        if (!(part = HeapAlloc( GetProcessHeap(), 0,
                                (SIZE_T)(length + 2) * sizeof(*part) )))
            return E_OUTOFMEMORY;
        part[0] = '/';
        for (j = 0; j < length; j++)
            part[j + 1] = path[j] == '\\' ? '/' : path[j];
        part[length + 1] = 0;
        content_type = appx_content_types_get_content_type( types, part );
        HeapFree( GetProcessHeap(), 0, part );
        if (!content_type) return APPX_E_INVALID_PACKAGING_LAYOUT;
        source->entries[i].entry.content_type = content_type;
    }
    return S_OK;
}

HRESULT WINAPI appx_bundle_inspect_ex(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    const struct appx_bundle_selection_policy *policy,
    const APPX_BUNDLE_INSPECT_OPTIONS *input_options,
    APPX_BUNDLE_INSPECTION **result )
{
    struct appx_signature_digest_set digests;
    struct stable_archive_source stable;
    struct appx_bundle_test_source test_source;
    APPX_BUNDLE_INSPECTION *inspection = NULL;
    APPX_BUNDLE_MANIFEST *manifest = NULL;
    APPX_CONTENT_TYPES *types = NULL;
    APPX_BLOCK_MAP *block_map = NULL;
    APPX_SIGNATURE *signature = NULL;
    WINE_APPX_ARCHIVE *archive = NULL;
    WINE_APPX_ARCHIVE_ENTRY metadata_entry;
    APPX_BUNDLE_INSPECT_OPTIONS options;
    BYTE signer_id[APPX_BUNDLE_SIGNER_ID_SIZE];
    BYTE *document = NULL;
    UINT32 metadata_index, size;
    HRESULT hr;

    TRACE( "file %p, limits %p, flags %#x, policy %p, options %p, result %p.\n",
           file, limits, flags, policy, input_options, result );

    memset( &stable, 0, sizeof(stable) );
    if (!result) return E_INVALIDARG;
    *result = NULL;
    if (!file || file == INVALID_HANDLE_VALUE ||
        FAILED(appx_bundle_selection_policy_validate_architecture( policy )) ||
        (flags & ~APPX_BUNDLE_INSPECT_ALLOW_UNTRUSTED_CHAIN))
        return E_INVALIDARG;
    if (FAILED(hr = normalize_inspect_options(
            input_options, &options )) ||
        FAILED(hr = check_cancelled( options.cancel_event )))
        return hr;
    if (FAILED(hr = wine_appx_archive_open_ex(
        file, limits, WINE_APPX_ARCHIVE_OPEN_BUNDLE, options.cancel_event,
        &archive )))
    {
        TRACE( "archive validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = read_entry( archive, signature_path,
                                APPX_SIGNATURE_MAX_SIZE,
                                options.cancel_event, &document, &size )))
    {
        TRACE( "signature read failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = appx_signature_parse_and_verify(
        document, size,
        APPX_SIGNATURE_VERIFY_BUNDLE |
        (flags & APPX_BUNDLE_INSPECT_ALLOW_UNTRUSTED_CHAIN ?
         APPX_SIGNATURE_VERIFY_ALLOW_UNTRUSTED_CHAIN : 0),
        &signature )))
    {
        TRACE( "CMS validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancelled( options.cancel_event )))
        goto done;
    HeapFree( GetProcessHeap(), 0, document );
    document = NULL;

    if (FAILED(hr = get_named_entry( archive, content_types_path,
                                     APPX_CONTENT_TYPES_MAX_DOCUMENT_SIZE,
                                     TRUE, &metadata_index,
                                     &metadata_entry )))
    {
        TRACE( "content-types metadata validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = get_named_entry( archive, block_map_path,
                                     APPX_BLOCK_MAP_MAX_DOCUMENT_SIZE,
                                     TRUE, &metadata_index,
                                     &metadata_entry )))
    {
        TRACE( "block-map metadata validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = get_named_entry( archive, bundle_manifest_path,
                                     APPX_BUNDLE_MANIFEST_MAX_SIZE,
                                     TRUE, &metadata_index,
                                     &metadata_entry )))
    {
        TRACE( "manifest metadata validation failed, hr %#lx.\n", hr );
        goto done;
    }
    hr = get_named_entry( archive, code_integrity_path,
                          APPX_BUNDLE_MAX_CODE_INTEGRITY_SIZE, FALSE,
                          &metadata_index, &metadata_entry );
    if (FAILED(hr))
    {
        TRACE( "code-integrity metadata validation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = appx_archive_calculate_digest_set_ex(
            archive, options.cancel_event, &digests )))
    {
        TRACE( "signed bundle digest calculation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = appx_signature_verify_digest_set( signature, &digests )))
    {
        TRACE( "signed bundle digest comparison failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancelled( options.cancel_event )))
        goto done;

    if (FAILED(hr = read_entry( archive, content_types_path,
                                APPX_CONTENT_TYPES_MAX_DOCUMENT_SIZE,
                                options.cancel_event, &document, &size )))
    {
        TRACE( "content-types read failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = appx_content_types_parse(
            document, size, APPX_CONTENT_TYPES_MODE_BUNDLE, &types )))
    {
        TRACE( "content-types parse failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancelled( options.cancel_event )))
        goto done;
    HeapFree( GetProcessHeap(), 0, document );
    document = NULL;
    if (FAILED(hr = read_entry( archive, block_map_path,
                                APPX_BLOCK_MAP_MAX_DOCUMENT_SIZE,
                                options.cancel_event, &document, &size )))
    {
        TRACE( "block-map read failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = appx_block_map_parse( document, size, &block_map )))
    {
        TRACE( "block-map parse failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancelled( options.cancel_event )))
        goto done;
    HeapFree( GetProcessHeap(), 0, document );
    document = NULL;
    if (FAILED(hr = read_entry( archive, bundle_manifest_path,
                                APPX_BUNDLE_MANIFEST_MAX_SIZE,
                                options.cancel_event, &document, &size )))
    {
        TRACE( "manifest read failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = appx_bundle_manifest_parse( document, size, &manifest )))
    {
        TRACE( "manifest parse failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = check_cancelled( options.cancel_event )))
        goto done;
    HeapFree( GetProcessHeap(), 0, document );
    document = NULL;
    if (!appx_bundle_manifest_get_identity( manifest ))
    {
        hr = APPX_E_INVALID_MANIFEST;
        goto done;
    }
    if (FAILED(hr = appx_signature_check_publisher(
        signature, appx_bundle_manifest_get_identity( manifest )->publisher )))
    {
        TRACE( "publisher binding failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = appx_signature_get_signer_certificate_id(
            signature, signer_id, sizeof(signer_id) )))
    {
        TRACE( "signer identity calculation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = validate_outer_block_map(
            archive, block_map, options.cancel_event )))
    {
        TRACE( "outer block-map reconciliation failed, hr %#lx.\n", hr );
        goto done;
    }
    if (FAILED(hr = build_stable_archive_source(
        archive, limits,
        flags & APPX_BUNDLE_INSPECT_ALLOW_UNTRUSTED_CHAIN ?
        APPX_PACKAGE_INSPECT_ALLOW_UNTRUSTED_CHAIN : 0,
        &options, &stable )))
    {
        TRACE( "inner archive preparation failed, hr %#lx.\n", hr );
        goto done;
    }
    stable.manifest = manifest;
    if (FAILED(hr = validate_outer_content_types( types, &stable )))
    {
        TRACE( "outer content-types reconciliation failed, hr %#lx.\n", hr );
        goto done;
    }

    memset( &test_source, 0, sizeof(test_source) );
    test_source.size = sizeof(test_source);
    test_source.context = &stable;
    test_source.entry_count = stable.count;
    test_source.max_total_expanded_size =
        limits ? limits->max_total_uncompressed_size :
        DEFAULT_MAX_TOTAL_INNER_EXPANDED_SIZE;
    test_source.get_entry = stable_get_entry_callback;
    test_source.inspect_package = archive_inspect_package_callback;
    {
        struct appx_bundle_selection retention_hint =
            {sizeof(retention_hint)};

        /*
         * This hint controls only which already-required validation file is
         * retained.  It is never exposed or accepted as a selection result;
         * the shared validator repeats selection after every package passes.
         */
        if (SUCCEEDED(appx_bundle_manifest_select(
            manifest, policy, &retention_hint )))
            stable.production.retained_index =
                retention_hint.package_index;
    }
    /*
     * Every inner package is materialized and fully inspected by the shared
     * validator.  Selection occurs only after the last successful callback.
     */
    {
        struct appx_bundle_selection selection = {sizeof(selection)};

        if (FAILED(hr = appx_bundle_validate_with_test_source(
            manifest, signer_id, policy, &test_source, &selection )))
        {
            TRACE( "inner package validation or selection failed, hr %#lx.\n",
                   hr );
            goto done;
        }
        if (selection.package_index >=
            appx_bundle_manifest_get_package_count( manifest ))
        {
            hr = APPX_E_INVALID_MANIFEST;
            goto done;
        }
        if (stable.production.retained_index != selection.package_index ||
            !stable.production.retained.inspection)
        {
            hr = APPX_E_CORRUPT_CONTENT;
            goto done;
        }
        if (!(inspection = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      sizeof(*inspection) )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        inspection->selection = selection;
    }
    inspection->manifest = manifest;
    inspection->selected_package =
        stable.production.retained.inspection;
    memset( &stable.production.retained, 0,
            sizeof(stable.production.retained) );
    memcpy( inspection->signer_id, signer_id, sizeof(signer_id) );
    manifest = NULL;
    *result = inspection;
    inspection = NULL;
    hr = S_OK;

done:
    HeapFree( GetProcessHeap(), 0, document );
    appx_bundle_inspection_free( inspection );
    free_stable_archive_source( &stable );
    appx_bundle_manifest_free( manifest );
    appx_signature_free( signature );
    appx_block_map_free( block_map );
    appx_content_types_free( types );
    wine_appx_archive_close( archive );
    return hr;
}

HRESULT WINAPI appx_bundle_inspect(
    HANDLE file, const WINE_APPX_ARCHIVE_LIMITS *limits, UINT32 flags,
    const struct appx_bundle_selection_policy *policy,
    APPX_BUNDLE_INSPECTION **result )
{
    return appx_bundle_inspect_ex(
        file, limits, flags, policy, NULL, result );
}

void WINAPI appx_bundle_inspection_free( APPX_BUNDLE_INSPECTION *inspection )
{
    if (!inspection) return;
    appx_package_inspection_free( inspection->selected_package );
    appx_bundle_manifest_free( inspection->manifest );
    HeapFree( GetProcessHeap(), 0, inspection );
}

const APPX_BUNDLE_MANIFEST *WINAPI appx_bundle_inspection_get_manifest(
    const APPX_BUNDLE_INSPECTION *inspection )
{
    return inspection ? inspection->manifest : NULL;
}

const struct appx_bundle_selection *WINAPI
appx_bundle_inspection_get_selection(
    const APPX_BUNDLE_INSPECTION *inspection )
{
    return inspection ? &inspection->selection : NULL;
}

const APPX_PACKAGE_INSPECTION *WINAPI
appx_bundle_inspection_get_selected_package(
    const APPX_BUNDLE_INSPECTION *inspection )
{
    return inspection ? inspection->selected_package : NULL;
}

HRESULT WINAPI appx_bundle_inspection_get_signer_id(
    const APPX_BUNDLE_INSPECTION *inspection, BYTE *signer_id, UINT32 size )
{
    if (!inspection || !signer_id || size != APPX_BUNDLE_SIGNER_ID_SIZE)
        return E_INVALIDARG;
    memcpy( signer_id, inspection->signer_id, size );
    return S_OK;
}
