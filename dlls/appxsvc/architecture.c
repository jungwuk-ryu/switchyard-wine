/*
 * AppX package architecture policy
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
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnt.h"
#include "winternl.h"
#undef WIN32_NO_STATUS

#include "architecture.h"

static HRESULT wrong_processor_architecture( void )
{
    return HRESULT_FROM_WIN32( ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
}

BOOL appx_architecture_is_valid(
    enum appx_catalog_architecture architecture )
{
    return (UINT32)architecture <= APPX_CATALOG_ARCHITECTURE_X86A64;
}

BOOL appx_architecture_is_concrete(
    enum appx_catalog_architecture architecture )
{
    return architecture >= APPX_CATALOG_ARCHITECTURE_X86 &&
           architecture <= APPX_CATALOG_ARCHITECTURE_X86A64;
}

BOOL appx_architecture_is_compatible(
    enum appx_catalog_architecture package_architecture,
    enum appx_catalog_architecture target_architecture )
{
    if (!appx_architecture_is_valid( package_architecture ) ||
        !appx_architecture_is_concrete( target_architecture ))
        return FALSE;
    return package_architecture == APPX_CATALOG_ARCHITECTURE_NEUTRAL ||
           package_architecture == target_architecture;
}

static void add_user_enabled(
    UINT32 *mask, enum appx_catalog_architecture architecture,
    MACHINE_ATTRIBUTES attributes )
{
    if (attributes & UserEnabled)
        *mask |= APPX_ARCHITECTURE_MASK( architecture );
}

static enum appx_catalog_architecture native_architecture(
    MACHINE_ATTRIBUTES i386_attributes,
    MACHINE_ATTRIBUTES amd64_attributes,
    MACHINE_ATTRIBUTES armnt_attributes,
    MACHINE_ATTRIBUTES arm64_attributes )
{
    /*
     * GetMachineTypeAttributes exposes one native KernelEnabled machine.
     * Keep an explicit order so malformed/injected multi-native inputs are
     * deterministic.
     */
    if (arm64_attributes & KernelEnabled)
        return APPX_CATALOG_ARCHITECTURE_ARM64;
    if (amd64_attributes & KernelEnabled)
        return APPX_CATALOG_ARCHITECTURE_X64;
    if (armnt_attributes & KernelEnabled)
        return APPX_CATALOG_ARCHITECTURE_ARM;
    if (i386_attributes & KernelEnabled)
        return APPX_CATALOG_ARCHITECTURE_X86;
    return APPX_CATALOG_ARCHITECTURE_NEUTRAL;
}

HRESULT appx_architecture_policy_init(
    enum appx_catalog_architecture preferred,
    MACHINE_ATTRIBUTES i386_attributes,
    MACHINE_ATTRIBUTES amd64_attributes,
    MACHINE_ATTRIBUTES armnt_attributes,
    MACHINE_ATTRIBUTES arm64_attributes,
    struct appx_architecture_policy *policy )
{
    enum appx_catalog_architecture selected = preferred;
    UINT32 mask = APPX_ARCHITECTURE_MASK(
        APPX_CATALOG_ARCHITECTURE_NEUTRAL );

    if (!policy) return E_INVALIDARG;
    memset( policy, 0, sizeof(*policy) );
    policy->size = sizeof(*policy);
    policy->version = APPX_ARCHITECTURE_POLICY_VERSION;

    if (!appx_architecture_is_valid( preferred ))
        return E_INVALIDARG;

    add_user_enabled( &mask, APPX_CATALOG_ARCHITECTURE_X86,
                      i386_attributes );
    add_user_enabled( &mask, APPX_CATALOG_ARCHITECTURE_X64,
                      amd64_attributes );
    add_user_enabled( &mask, APPX_CATALOG_ARCHITECTURE_ARM,
                      armnt_attributes );
    add_user_enabled( &mask, APPX_CATALOG_ARCHITECTURE_ARM64,
                      arm64_attributes );

    /*
     * X86A64 is the package identity for x86-on-ARM64.  It is intentionally
     * not inferred from IMAGE_FILE_MACHINE_ARM64EC, whose dependency domain
     * is X64.
     */
    if ((arm64_attributes & KernelEnabled) &&
        (i386_attributes & UserEnabled))
        mask |= APPX_ARCHITECTURE_MASK(
            APPX_CATALOG_ARCHITECTURE_X86A64 );

    policy->supported_mask = mask;
    if (preferred == APPX_CATALOG_ARCHITECTURE_NEUTRAL)
    {
        selected = native_architecture(
            i386_attributes, amd64_attributes,
            armnt_attributes, arm64_attributes );
        if (!appx_architecture_is_concrete( selected ) ||
            !(mask & APPX_ARCHITECTURE_MASK( selected )))
            return wrong_processor_architecture();
    }
    else if (!(mask & APPX_ARCHITECTURE_MASK( preferred )))
    {
        return wrong_processor_architecture();
    }

    policy->preferred = selected;
    return S_OK;
}

HRESULT appx_architecture_query_host_policy(
    enum appx_catalog_architecture preferred,
    struct appx_architecture_policy *policy )
{
    MACHINE_ATTRIBUTES i386_attributes, amd64_attributes;
    MACHINE_ATTRIBUTES armnt_attributes, arm64_attributes;
    HRESULT hr;

    if (!policy) return E_INVALIDARG;
    memset( policy, 0, sizeof(*policy) );

    if (FAILED(hr = GetMachineTypeAttributes(
            IMAGE_FILE_MACHINE_I386, &i386_attributes )))
        return hr;
    if (FAILED(hr = GetMachineTypeAttributes(
            IMAGE_FILE_MACHINE_AMD64, &amd64_attributes )))
        return hr;
    if (FAILED(hr = GetMachineTypeAttributes(
            IMAGE_FILE_MACHINE_ARMNT, &armnt_attributes )))
        return hr;
    if (FAILED(hr = GetMachineTypeAttributes(
            IMAGE_FILE_MACHINE_ARM64, &arm64_attributes )))
        return hr;

    return appx_architecture_policy_init(
        preferred, i386_attributes, amd64_attributes,
        armnt_attributes, arm64_attributes, policy );
}

static BOOL valid_policy( const struct appx_architecture_policy *policy )
{
    if (!policy || policy->size < sizeof(*policy) ||
        policy->version != APPX_ARCHITECTURE_POLICY_VERSION ||
        policy->supported_mask & ~APPX_ARCHITECTURE_KNOWN_MASK ||
        !(policy->supported_mask & APPX_ARCHITECTURE_MASK(
            APPX_CATALOG_ARCHITECTURE_NEUTRAL )) ||
        !appx_architecture_is_concrete( policy->preferred ))
        return FALSE;
    return !!(policy->supported_mask &
              APPX_ARCHITECTURE_MASK( policy->preferred ));
}

BOOL appx_architecture_policy_supports(
    const struct appx_architecture_policy *policy,
    enum appx_catalog_architecture architecture )
{
    return valid_policy( policy ) &&
           appx_architecture_is_valid( architecture ) &&
           !!(policy->supported_mask & APPX_ARCHITECTURE_MASK( architecture ));
}

UINT32 appx_architecture_policy_rank(
    const struct appx_architecture_policy *policy,
    enum appx_catalog_architecture architecture )
{
    if (!appx_architecture_policy_supports( policy, architecture ))
        return 0;
    if (architecture == policy->preferred) return 7;
    if (architecture == APPX_CATALOG_ARCHITECTURE_NEUTRAL) return 6;

    /*
     * Guest order is fixed to make every selection deterministic.  On an
     * ARM64 host this gives ARM64, neutral, X64, ARM, X86A64, then X86.
     * Microsoft documents that Windows on Arm selects native ARM32 over x86
     * when those are the only submitted variants; keep that proven relation
     * ahead of either x86 representation.  ARM64 is last only when it is not
     * itself preferred.
     */
    switch (architecture)
    {
    case APPX_CATALOG_ARCHITECTURE_X64:
        return 5;
    case APPX_CATALOG_ARCHITECTURE_ARM:
        return 4;
    case APPX_CATALOG_ARCHITECTURE_X86A64:
        return 3;
    case APPX_CATALOG_ARCHITECTURE_X86:
        return 2;
    case APPX_CATALOG_ARCHITECTURE_ARM64:
        return 1;
    default:
        return 0;
    }
}

static HRESULT complete_nt_io(
    HANDLE file, IO_STATUS_BLOCK *io, NTSTATUS status, NTSTATUS *completed )
{
    if (status == STATUS_PENDING)
    {
        DWORD wait = WaitForSingleObject( file, INFINITE );

        if (wait != WAIT_OBJECT_0)
        {
            if (completed) *completed = STATUS_PENDING;
            return wait == WAIT_FAILED ?
                   HRESULT_FROM_WIN32( GetLastError() ) : E_FAIL;
        }
        status = io->Status;
    }
    if (completed) *completed = status;
    return status ? HRESULT_FROM_NT( status ) : S_OK;
}

static HRESULT query_file_position(
    HANDLE file, FILE_POSITION_INFORMATION *position )
{
    IO_STATUS_BLOCK io;
    NTSTATUS completed, status;
    HRESULT hr;

    status = NtQueryInformationFile(
        file, &io, position, sizeof(*position), FilePositionInformation );
    hr = complete_nt_io( file, &io, status, &completed );
    if (FAILED(hr)) return hr;
    return io.Information == sizeof(*position) ? S_OK : E_FAIL;
}

static HRESULT restore_file_position(
    HANDLE file, const FILE_POSITION_INFORMATION *position )
{
    FILE_POSITION_INFORMATION copy = *position;
    IO_STATUS_BLOCK io;
    NTSTATUS status;

    status = NtSetInformationFile(
        file, &io, &copy, sizeof(copy), FilePositionInformation );
    return complete_nt_io( file, &io, status, NULL );
}

static HRESULT query_file_size( HANDLE file, UINT64 *size )
{
    FILE_STANDARD_INFORMATION information;
    IO_STATUS_BLOCK io;
    NTSTATUS completed, status;
    HRESULT hr;

    status = NtQueryInformationFile(
        file, &io, &information, sizeof(information),
        FileStandardInformation );
    hr = complete_nt_io( file, &io, status, &completed );
    if (FAILED(hr)) return hr;
    if (io.Information != sizeof(information)) return E_FAIL;
    if (information.Directory || information.EndOfFile.QuadPart < 0)
        return S_FALSE;
    *size = information.EndOfFile.QuadPart;
    return S_OK;
}

static HRESULT read_at(
    HANDLE file, UINT64 offset, void *buffer, ULONG size )
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER position;
    NTSTATUS completed, status;
    HRESULT hr;

    position.QuadPart = offset;
    status = NtReadFile(
        file, NULL, NULL, NULL, &io, buffer, size, &position, NULL );
    hr = complete_nt_io( file, &io, status, &completed );
    if (completed == STATUS_END_OF_FILE) return S_FALSE;
    if (FAILED(hr)) return hr;
    return io.Information == size ? S_OK : S_FALSE;
}

static BOOL machine_matches_optional_magic( USHORT machine, USHORT magic )
{
    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:
    case IMAGE_FILE_MACHINE_ARMNT:
    case IMAGE_FILE_MACHINE_CHPE_X86:
        return magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    case IMAGE_FILE_MACHINE_AMD64:
    case IMAGE_FILE_MACHINE_ARM64:
    case IMAGE_FILE_MACHINE_ARM64EC:
    case IMAGE_FILE_MACHINE_ARM64X:
        return magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    default:
        /* Preserve every otherwise-valid future PE in loader provenance. */
        return TRUE;
    }
}

static HRESULT read_pe_machine(
    HANDLE file, USHORT *machine )
{
    UINT32 minimum_optional_size;
    UINT64 file_size, optional_offset, section_table_end;
    IMAGE_DOS_HEADER dos;
    struct
    {
        DWORD signature;
        IMAGE_FILE_HEADER file;
    } nt;
    USHORT optional_magic;
    HRESULT hr;

    if ((hr = query_file_size( file, &file_size )) != S_OK)
        return hr;
    if (file_size < sizeof(dos)) return S_FALSE;
    if ((hr = read_at( file, 0, &dos, sizeof(dos) )) != S_OK)
        return hr;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE ||
        (UINT32)dos.e_lfanew < sizeof(dos) ||
        (UINT32)dos.e_lfanew >
            APPX_ARCHITECTURE_MAX_PE_HEADER_OFFSET - sizeof(nt))
        return S_FALSE;
    if ((UINT64)dos.e_lfanew + sizeof(nt) > file_size)
        return S_FALSE;

    if ((hr = read_at( file, dos.e_lfanew, &nt, sizeof(nt) )) != S_OK)
        return hr;
    if (nt.signature != IMAGE_NT_SIGNATURE ||
        !nt.file.NumberOfSections ||
        nt.file.NumberOfSections > APPX_ARCHITECTURE_MAX_SECTIONS ||
        !(nt.file.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        nt.file.SizeOfOptionalHeader < sizeof(optional_magic) ||
        nt.file.SizeOfOptionalHeader >
            APPX_ARCHITECTURE_MAX_OPTIONAL_HEADER_SIZE)
        return S_FALSE;

    optional_offset = (UINT32)dos.e_lfanew + sizeof(nt);
    if (optional_offset + nt.file.SizeOfOptionalHeader > file_size)
        return S_FALSE;
    if ((hr = read_at(
            file, optional_offset, &optional_magic,
            sizeof(optional_magic) )) != S_OK)
        return hr;
    if (optional_magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        minimum_optional_size = FIELD_OFFSET(
            IMAGE_OPTIONAL_HEADER32, DataDirectory );
    else if (optional_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        minimum_optional_size = FIELD_OFFSET(
            IMAGE_OPTIONAL_HEADER64, DataDirectory );
    else
        return S_FALSE;
    if (nt.file.SizeOfOptionalHeader < minimum_optional_size ||
        !machine_matches_optional_magic( nt.file.Machine, optional_magic ))
        return S_FALSE;

    section_table_end =
        optional_offset + nt.file.SizeOfOptionalHeader +
        (UINT64)nt.file.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (section_table_end > file_size) return S_FALSE;

    *machine = nt.file.Machine;
    return S_OK;
}

HRESULT appx_architecture_read_pe_machine( HANDLE file, USHORT *machine )
{
    FILE_POSITION_INFORMATION original;
    HRESULT hr, restore_hr;

    if (!machine) return E_INVALIDARG;
    *machine = IMAGE_FILE_MACHINE_UNKNOWN;

    if (FAILED(hr = query_file_position( file, &original ))) return hr;
    hr = read_pe_machine( file, machine );
    /*
     * Wine's synchronous NtReadFile path advances the shared file position
     * even for an explicit ByteOffset, so restore the private held handle on
     * every successful, malformed, or failing read path.
     */
    restore_hr = restore_file_position( file, &original );
    if (FAILED(restore_hr) && SUCCEEDED(hr)) hr = restore_hr;
    if (hr != S_OK) *machine = IMAGE_FILE_MACHINE_UNKNOWN;
    return hr;
}

HRESULT appx_architecture_map_machine(
    USHORT machine, enum appx_catalog_architecture *architecture )
{
    if (!architecture) return E_INVALIDARG;
    *architecture = APPX_CATALOG_ARCHITECTURE_NEUTRAL;

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:
        *architecture = APPX_CATALOG_ARCHITECTURE_X86;
        return S_OK;
    case IMAGE_FILE_MACHINE_AMD64:
        *architecture = APPX_CATALOG_ARCHITECTURE_X64;
        return S_OK;
    case IMAGE_FILE_MACHINE_ARMNT:
        *architecture = APPX_CATALOG_ARCHITECTURE_ARM;
        return S_OK;
    case IMAGE_FILE_MACHINE_ARM64:
        *architecture = APPX_CATALOG_ARCHITECTURE_ARM64;
        return S_OK;
    case IMAGE_FILE_MACHINE_CHPE_X86:
        *architecture = APPX_CATALOG_ARCHITECTURE_X86A64;
        return S_OK;
    case IMAGE_FILE_MACHINE_ARM64EC:
        *architecture = APPX_CATALOG_ARCHITECTURE_X64;
        return S_OK;
    case IMAGE_FILE_MACHINE_ARM64X:
        return wrong_processor_architecture();
    default:
        return wrong_processor_architecture();
    }
}

HRESULT appx_architecture_validate_executable(
    HANDLE file, enum appx_catalog_architecture manifest_architecture,
    const struct appx_architecture_policy *policy,
    enum appx_catalog_architecture *target_architecture )
{
    enum appx_catalog_architecture target;
    USHORT machine;
    HRESULT hr;

    if (!target_architecture) return E_INVALIDARG;
    *target_architecture = APPX_CATALOG_ARCHITECTURE_NEUTRAL;
    if (!appx_architecture_is_valid( manifest_architecture ))
        return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (!valid_policy( policy )) return E_INVALIDARG;

    hr = appx_architecture_read_pe_machine( file, &machine );
    if (hr == S_FALSE) return APPX_E_INVALID_PACKAGING_LAYOUT;
    if (FAILED(hr)) return hr;
    if (FAILED(hr = appx_architecture_map_machine( machine, &target )))
        return hr;
    if (!appx_architecture_policy_supports( policy, target ) ||
        !appx_architecture_is_compatible(
            manifest_architecture, target ))
        return wrong_processor_architecture();

    *target_architecture = target;
    return S_OK;
}
