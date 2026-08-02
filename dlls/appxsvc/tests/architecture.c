/*
 * AppX package architecture policy tests
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

#include "../architecture.c"

#include "wine/test.h"

#define TEST_IMAGE_SIZE 1024
#define TEST_NT_OFFSET  0x80

static HRESULT wrong_processor_architecture_result(void)
{
    return HRESULT_FROM_WIN32(
        ERROR_INSTALL_WRONG_PROCESSOR_ARCHITECTURE );
}

static UINT32 build_test_image( BYTE image[TEST_IMAGE_SIZE],
                                USHORT machine, USHORT optional_magic )
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    IMAGE_FILE_HEADER *file;
    BYTE *nt, *optional;
    UINT32 optional_size;

    memset( image, 0, TEST_IMAGE_SIZE );
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = TEST_NT_OFFSET;
    nt = image + TEST_NT_OFFSET;
    *(DWORD *)nt = IMAGE_NT_SIGNATURE;
    file = (IMAGE_FILE_HEADER *)(nt + sizeof(DWORD));
    file->Machine = machine;
    file->NumberOfSections = 1;
    file->Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
    optional_size =
        optional_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ?
        FIELD_OFFSET(IMAGE_OPTIONAL_HEADER64, DataDirectory) :
        FIELD_OFFSET(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    file->SizeOfOptionalHeader = optional_size;
    optional = (BYTE *)(file + 1);
    *(USHORT *)optional = optional_magic;
    return (optional - image) + optional_size + sizeof(IMAGE_SECTION_HEADER);
}

static BOOL write_test_file( HANDLE file, const void *data, UINT32 size )
{
    LARGE_INTEGER zero;
    DWORD written;

    zero.QuadPart = 0;
    if (!SetFilePointerEx( file, zero, NULL, FILE_BEGIN ) ||
        !SetEndOfFile( file ) ||
        !WriteFile( file, data, size, &written, NULL ) ||
        written != size)
        return FALSE;
    return TRUE;
}

static HANDLE create_test_file(void)
{
    WCHAR directory[MAX_PATH], path[MAX_PATH];
    HANDLE file;

    if (!GetTempPathW( ARRAY_SIZE(directory), directory ) ||
        !GetTempFileNameW( directory, L"swa", 0, path ))
        return INVALID_HANDLE_VALUE;
    file = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL );
    if (file == INVALID_HANDLE_VALUE) DeleteFileW( path );
    return file;
}

static LONGLONG get_file_position( HANDLE file )
{
    LARGE_INTEGER zero, position;

    zero.QuadPart = 0;
    if (!SetFilePointerEx( file, zero, &position, FILE_CURRENT ))
        return -1;
    return position.QuadPart;
}

static void set_file_position( HANDLE file, LONGLONG value )
{
    LARGE_INTEGER position;

    position.QuadPart = value;
    ok( SetFilePointerEx( file, position, NULL, FILE_BEGIN ),
        "failed to set file position, error %lu.\n", GetLastError() );
}

static void test_policy(void)
{
    const MACHINE_ATTRIBUTES user =
        (MACHINE_ATTRIBUTES)UserEnabled;
    const MACHINE_ATTRIBUTES native =
        (MACHINE_ATTRIBUTES)(UserEnabled | KernelEnabled);
    const MACHINE_ATTRIBUTES none = (MACHINE_ATTRIBUTES)0;
    struct appx_architecture_policy policy;
    HRESULT hr;

    hr = appx_architecture_policy_init(
        APPX_CATALOG_ARCHITECTURE_X64,
        user, native, none, none, &policy );
    ok( hr == S_OK, "x64 policy returned %#lx.\n", hr );
    ok( policy.size == sizeof(policy) &&
        policy.version == APPX_ARCHITECTURE_POLICY_VERSION,
        "got policy size %u, version %u.\n",
        policy.size, policy.version );
    ok( appx_architecture_policy_supports(
            &policy, APPX_CATALOG_ARCHITECTURE_NEUTRAL ),
        "neutral is not supported.\n" );
    ok( appx_architecture_policy_supports(
            &policy, APPX_CATALOG_ARCHITECTURE_X86 ),
        "x86 guest is not supported.\n" );
    ok( appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_X64 ) == 7,
        "x64 preferred rank is %u.\n",
        appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_X64 ) );
    ok( appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_NEUTRAL ) == 6,
        "neutral rank is %u.\n",
        appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_NEUTRAL ) );
    ok( appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_X86 ) == 2,
        "x86 guest rank is %u.\n",
        appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_X86 ) );
    ok( !appx_architecture_policy_supports(
            &policy, APPX_CATALOG_ARCHITECTURE_X86A64 ),
        "x86a64 was enabled without an ARM64 kernel.\n" );

    hr = appx_architecture_policy_init(
        APPX_CATALOG_ARCHITECTURE_NEUTRAL,
        user, user, user, native, &policy );
    ok( hr == S_OK, "ARM64 policy returned %#lx.\n", hr );
    ok( policy.preferred == APPX_CATALOG_ARCHITECTURE_ARM64,
        "got preferred architecture %u.\n", policy.preferred );
    ok( appx_architecture_policy_supports(
            &policy, APPX_CATALOG_ARCHITECTURE_X86A64 ),
        "ARM64 x86a64 capability is missing.\n" );
    ok( appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_X64 ) >
        appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_ARM ) &&
        appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_ARM ) >
        appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_X86A64 ) &&
        appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_X86A64 ) >
        appx_architecture_policy_rank(
            &policy, APPX_CATALOG_ARCHITECTURE_X86 ),
        "guest ranks are not deterministic.\n" );

    hr = appx_architecture_policy_init(
        APPX_CATALOG_ARCHITECTURE_X86A64,
        user, native, none, none, &policy );
    ok( hr == wrong_processor_architecture_result(),
        "x64-host x86a64 policy returned %#lx.\n", hr );
    hr = appx_architecture_policy_init(
        (enum appx_catalog_architecture)99,
        user, native, none, none, &policy );
    ok( hr == E_INVALIDARG, "invalid preferred returned %#lx.\n", hr );

    policy.size = sizeof(policy) - 1;
    ok( !appx_architecture_policy_supports(
            &policy, APPX_CATALOG_ARCHITECTURE_X64 ),
        "truncated policy was accepted.\n" );
    policy.size = sizeof(policy);
    policy.version++;
    ok( !appx_architecture_policy_supports(
            &policy, APPX_CATALOG_ARCHITECTURE_X64 ),
        "unknown policy version was accepted.\n" );
}

static void test_compatibility_and_machine_mapping(void)
{
    static const struct
    {
        USHORT machine;
        enum appx_catalog_architecture architecture;
    } mappings[] =
    {
        {IMAGE_FILE_MACHINE_I386, APPX_CATALOG_ARCHITECTURE_X86},
        {IMAGE_FILE_MACHINE_AMD64, APPX_CATALOG_ARCHITECTURE_X64},
        {IMAGE_FILE_MACHINE_ARMNT, APPX_CATALOG_ARCHITECTURE_ARM},
        {IMAGE_FILE_MACHINE_ARM64, APPX_CATALOG_ARCHITECTURE_ARM64},
        {IMAGE_FILE_MACHINE_CHPE_X86, APPX_CATALOG_ARCHITECTURE_X86A64},
        /*
         * ARM64EC is an x64 ABI/dependency domain.  It is deliberately not
         * conflated with the x86-on-ARM64 package architecture.
         */
        {IMAGE_FILE_MACHINE_ARM64EC, APPX_CATALOG_ARCHITECTURE_X64},
    };
    enum appx_catalog_architecture architecture;
    UINT32 i;
    HRESULT hr;

    ok( appx_architecture_is_valid(
            APPX_CATALOG_ARCHITECTURE_NEUTRAL ),
        "neutral is invalid.\n" );
    ok( !appx_architecture_is_concrete(
            APPX_CATALOG_ARCHITECTURE_NEUTRAL ),
        "neutral is concrete.\n" );
    ok( appx_architecture_is_concrete(
            APPX_CATALOG_ARCHITECTURE_X86A64 ),
        "x86a64 is not concrete.\n" );
    ok( !appx_architecture_is_valid(
            (enum appx_catalog_architecture)99 ),
        "unknown architecture is valid.\n" );
    ok( appx_architecture_is_compatible(
            APPX_CATALOG_ARCHITECTURE_NEUTRAL,
            APPX_CATALOG_ARCHITECTURE_X64 ),
        "neutral package is incompatible with x64.\n" );
    ok( appx_architecture_is_compatible(
            APPX_CATALOG_ARCHITECTURE_X86,
            APPX_CATALOG_ARCHITECTURE_X86 ),
        "x86 package is incompatible with x86.\n" );
    ok( !appx_architecture_is_compatible(
            APPX_CATALOG_ARCHITECTURE_X64,
            APPX_CATALOG_ARCHITECTURE_X86 ),
        "x64 package is compatible with x86.\n" );
    ok( !appx_architecture_is_compatible(
            APPX_CATALOG_ARCHITECTURE_NEUTRAL,
            APPX_CATALOG_ARCHITECTURE_NEUTRAL ),
        "neutral process target was accepted.\n" );

    for (i = 0; i < ARRAY_SIZE(mappings); i++)
    {
        architecture = APPX_CATALOG_ARCHITECTURE_NEUTRAL;
        hr = appx_architecture_map_machine(
            mappings[i].machine, &architecture );
        ok( hr == S_OK && architecture == mappings[i].architecture,
            "machine %#x returned %#lx, architecture %u.\n",
            mappings[i].machine, hr, architecture );
    }
    hr = appx_architecture_map_machine(
        IMAGE_FILE_MACHINE_ARM64X, &architecture );
    ok( hr == wrong_processor_architecture_result(),
        "ARM64X returned %#lx.\n", hr );
    hr = appx_architecture_map_machine(
        IMAGE_FILE_MACHINE_IA64, &architecture );
    ok( hr == wrong_processor_architecture_result(),
        "unknown machine returned %#lx.\n", hr );
}

static void test_pe_reader_and_executable_validation(void)
{
    const MACHINE_ATTRIBUTES user =
        (MACHINE_ATTRIBUTES)UserEnabled;
    const MACHINE_ATTRIBUTES native =
        (MACHINE_ATTRIBUTES)(UserEnabled | KernelEnabled);
    const MACHINE_ATTRIBUTES none = (MACHINE_ATTRIBUTES)0;
    struct appx_architecture_policy policy;
    enum appx_catalog_architecture target;
    BYTE image[TEST_IMAGE_SIZE];
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    IMAGE_FILE_HEADER *file;
    HANDLE handle;
    UINT32 size;
    USHORT machine;
    HRESULT hr;

    handle = create_test_file();
    ok( handle != INVALID_HANDLE_VALUE,
        "failed to create test file, error %lu.\n", GetLastError() );
    if (handle == INVALID_HANDLE_VALUE) return;

    size = build_test_image(
        image, IMAGE_FILE_MACHINE_AMD64,
        IMAGE_NT_OPTIONAL_HDR64_MAGIC );
    ok( write_test_file( handle, image, size ),
        "failed to write PE fixture, error %lu.\n", GetLastError() );
    set_file_position( handle, 17 );
    machine = IMAGE_FILE_MACHINE_UNKNOWN;
    hr = appx_architecture_read_pe_machine( handle, &machine );
    ok( hr == S_OK && machine == IMAGE_FILE_MACHINE_AMD64,
        "PE reader returned %#lx, machine %#x.\n", hr, machine );
    ok( get_file_position( handle ) == 17,
        "PE reader changed the synchronous file position to %s.\n",
        wine_dbgstr_longlong(get_file_position(handle)) );

    file = (IMAGE_FILE_HEADER *)(image + TEST_NT_OFFSET + sizeof(DWORD));
    file->NumberOfSections = APPX_ARCHITECTURE_MAX_SECTIONS + 1;
    ok( write_test_file( handle, image, size ),
        "failed to write section fixture.\n" );
    set_file_position( handle, 23 );
    hr = appx_architecture_read_pe_machine( handle, &machine );
    ok( hr == S_FALSE && machine == IMAGE_FILE_MACHINE_UNKNOWN,
        "oversized section count returned %#lx, machine %#x.\n",
        hr, machine );
    ok( get_file_position( handle ) == 23,
        "malformed PE changed the file position to %s.\n",
        wine_dbgstr_longlong(get_file_position(handle)) );

    size = build_test_image(
        image, IMAGE_FILE_MACHINE_AMD64,
        IMAGE_NT_OPTIONAL_HDR32_MAGIC );
    ok( write_test_file( handle, image, size ),
        "failed to write mismatched-magic fixture.\n" );
    hr = appx_architecture_read_pe_machine( handle, &machine );
    ok( hr == S_FALSE,
        "machine/magic contradiction returned %#lx.\n", hr );

    size = build_test_image(
        image, IMAGE_FILE_MACHINE_AMD64,
        IMAGE_NT_OPTIONAL_HDR64_MAGIC );
    dos->e_lfanew = APPX_ARCHITECTURE_MAX_PE_HEADER_OFFSET + 1;
    ok( write_test_file( handle, image, size ),
        "failed to write bounded-offset fixture.\n" );
    hr = appx_architecture_read_pe_machine( handle, &machine );
    ok( hr == S_FALSE, "oversized PE offset returned %#lx.\n", hr );

    size = build_test_image(
        image, IMAGE_FILE_MACHINE_IA64,
        IMAGE_NT_OPTIONAL_HDR64_MAGIC );
    ok( write_test_file( handle, image, size ),
        "failed to write future-machine fixture.\n" );
    hr = appx_architecture_read_pe_machine( handle, &machine );
    ok( hr == S_OK && machine == IMAGE_FILE_MACHINE_IA64,
        "future PE returned %#lx, machine %#x.\n", hr, machine );

    hr = appx_architecture_policy_init(
        APPX_CATALOG_ARCHITECTURE_NEUTRAL,
        user, user, user, native, &policy );
    ok( hr == S_OK, "validation policy returned %#lx.\n", hr );

    size = build_test_image(
        image, IMAGE_FILE_MACHINE_ARM64EC,
        IMAGE_NT_OPTIONAL_HDR64_MAGIC );
    ok( write_test_file( handle, image, size ),
        "failed to write ARM64EC fixture.\n" );
    hr = appx_architecture_validate_executable(
        handle, APPX_CATALOG_ARCHITECTURE_X64, &policy, &target );
    ok( hr == S_OK && target == APPX_CATALOG_ARCHITECTURE_X64,
        "ARM64EC validation returned %#lx, target %u.\n", hr, target );

    size = build_test_image(
        image, IMAGE_FILE_MACHINE_CHPE_X86,
        IMAGE_NT_OPTIONAL_HDR32_MAGIC );
    ok( write_test_file( handle, image, size ),
        "failed to write CHPE fixture.\n" );
    hr = appx_architecture_validate_executable(
        handle, APPX_CATALOG_ARCHITECTURE_X86A64, &policy, &target );
    ok( hr == S_OK && target == APPX_CATALOG_ARCHITECTURE_X86A64,
        "CHPE validation returned %#lx, target %u.\n", hr, target );

    size = build_test_image(
        image, IMAGE_FILE_MACHINE_AMD64,
        IMAGE_NT_OPTIONAL_HDR64_MAGIC );
    ok( write_test_file( handle, image, size ),
        "failed to write manifest-mismatch fixture.\n" );
    hr = appx_architecture_validate_executable(
        handle, APPX_CATALOG_ARCHITECTURE_X86, &policy, &target );
    ok( hr == wrong_processor_architecture_result(),
        "non-neutral manifest contradiction returned %#lx.\n", hr );
    hr = appx_architecture_validate_executable(
        handle, APPX_CATALOG_ARCHITECTURE_NEUTRAL, &policy, &target );
    ok( hr == S_OK && target == APPX_CATALOG_ARCHITECTURE_X64,
        "neutral manifest returned %#lx, target %u.\n", hr, target );

    size = build_test_image(
        image, IMAGE_FILE_MACHINE_ARM64X,
        IMAGE_NT_OPTIONAL_HDR64_MAGIC );
    ok( write_test_file( handle, image, size ),
        "failed to write ARM64X fixture.\n" );
    hr = appx_architecture_validate_executable(
        handle, APPX_CATALOG_ARCHITECTURE_NEUTRAL, &policy, &target );
    ok( hr == wrong_processor_architecture_result(),
        "ARM64X validation returned %#lx.\n", hr );

    hr = appx_architecture_policy_init(
        APPX_CATALOG_ARCHITECTURE_X64,
        user, native, none, none, &policy );
    ok( hr == S_OK, "x64-only policy returned %#lx.\n", hr );
    size = build_test_image(
        image, IMAGE_FILE_MACHINE_ARM64,
        IMAGE_NT_OPTIONAL_HDR64_MAGIC );
    ok( write_test_file( handle, image, size ),
        "failed to write unsupported ARM64 fixture.\n" );
    hr = appx_architecture_validate_executable(
        handle, APPX_CATALOG_ARCHITECTURE_NEUTRAL, &policy, &target );
    ok( hr == wrong_processor_architecture_result(),
        "host-disabled executable returned %#lx.\n", hr );

    memset( image, 0, sizeof(IMAGE_DOS_HEADER) );
    ok( write_test_file( handle, image, sizeof(IMAGE_DOS_HEADER) ),
        "failed to write non-PE fixture.\n" );
    hr = appx_architecture_validate_executable(
        handle, APPX_CATALOG_ARCHITECTURE_NEUTRAL, &policy, &target );
    ok( hr == APPX_E_INVALID_PACKAGING_LAYOUT,
        "malformed executable returned %#lx.\n", hr );

    CloseHandle( handle );
}

START_TEST(architecture)
{
    test_policy();
    test_compatibility_and_machine_mapping();
    test_pe_reader_and_executable_validation();
}
