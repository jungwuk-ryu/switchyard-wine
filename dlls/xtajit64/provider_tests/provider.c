/*
 * ARM64EC x64 provider contract tests
 *
 * Copyright 2026 Switchyard contributors
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
#include "winternl.h"
#include "wine/test.h"

#ifdef __arm64ec__

static DWORD CALLBACK simulation_thread( void *arg )
{
    BYTE code[] =
    {
        0x65, 0x48, 0x8b, 0x04, 0x25, 0x30, 0, 0, 0, /* mov gs:[0x30],%rax */
        0x48, 0x8b, 0x40, 0x30,                       /* mov 0x30(%rax),%rax */
        0x48, 0xba, 0, 0, 0, 0, 0, 0, 0, 0,          /* movabs $TEB,%rdx */
        0x48, 0x39, 0xd0,                              /* cmp %rdx,%rax */
        0x75, 0x23,                                    /* jne failed */
        0x65, 0x48, 0x8b, 0x04, 0x25, 0x60, 0, 0, 0, /* mov gs:[0x60],%rax */
        0x48, 0x8b, 0x40, 0x30,                       /* mov PEB.ProcessHeap,%rax */
        0x48, 0xba, 0, 0, 0, 0, 0, 0, 0, 0,          /* movabs $ProcessHeap,%rdx */
        0x48, 0x39, 0xd0,                              /* cmp %rdx,%rax */
        0x75, 0x07,                                    /* jne failed */
        0xb9, 0x34, 0x12, 0, 0,                        /* mov $0x1234,%ecx */
        0xeb, 0x05,                                    /* jmp exit */
        0xb9, 0x01, 0xe0, 0, 0,                        /* failed: mov $0xe001,%ecx */
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,          /* exit: movabs $RtlExitUserThread,%rax */
        0xff, 0xd0,                                    /* call *%rax */
        0xc3,                                          /* ret */
    };
    void (WINAPI *begin_simulation)(void) = arg;
    CONTEXT *context;
    DWORD old_protect;
    void *address, *heap, *target, *teb;

    if (!(address = VirtualAlloc( NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE ))) return 0xe001;
    if (!(target = GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "RtlExitUserThread" ))) return 0xe002;
    teb = NtCurrentTeb();
    heap = NtCurrentTeb()->Peb->ProcessHeap;
    memcpy( code + 15, &teb, sizeof(teb) );
    memcpy( code + 43, &heap, sizeof(heap) );
    memcpy( code + 70, &target, sizeof(target) );
    memcpy( address, code, sizeof(code) );
    if (!VirtualProtect( address, 0x1000, PAGE_EXECUTE_READ, &old_protect )) return 0xe003;

    context = &NtCurrentTeb()->ChpeV2CpuAreaInfo->ContextAmd64->AMD64_Context;
    context->Rsp = (ULONG_PTR)&context - 0x800;
    context->Rip = (ULONG_PTR)address;
    NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation = 1;
    begin_simulation();
    return 0xe004;
}

#define MIXED_IMAGE_SECTION_SIZE 0x1000  /* x64 logical page size */
#define MIXED_IMAGE_RAW_SIZE     0x0200
#define MIXED_IMAGE_TEXT_RVA     0x1000
#define MIXED_IMAGE_DATA_RVA     0x2000
#define MIXED_IMAGE_RDATA_RVA    0x3000
#define MIXED_IMAGE_TAIL_RVA     0x4000
#define MIXED_IMAGE_HOST_PAGE    0x4000  /* Darwin arm64 host page size */
#define MIXED_IMAGE_SIZE         0x5000

struct mixed_image_thread_args
{
    void (WINAPI *begin_simulation)(void);
    void *entry;
    void *exit_thread;
};

static BOOL write_image_part( HANDLE file, DWORD offset, const void *buffer, DWORD size )
{
    LARGE_INTEGER position;
    DWORD written;

    position.QuadPart = offset;
    return SetFilePointerEx( file, position, NULL, FILE_BEGIN ) &&
           WriteFile( file, buffer, size, &written, NULL ) && written == size;
}

static HANDLE create_mixed_protection_image( char path[MAX_PATH] )
{
    static const DWORD written_value = 0x13572468;
    static const DWORD readonly_value = 0x6a09beef;
    IMAGE_SECTION_HEADER sections[4];
    IMAGE_NT_HEADERS64 nt;
    IMAGE_DOS_HEADER dos;
    BYTE text[MIXED_IMAGE_RAW_SIZE] = {0};
    BYTE data[MIXED_IMAGE_RAW_SIZE] = {0};
    BYTE rdata[MIXED_IMAGE_RAW_SIZE] = {0};
    BYTE tail[MIXED_IMAGE_RAW_SIZE] = {0};
    char temp_path[MAX_PATH];
    SIZE_T offset = 0, displacement_offset, branch_offset, failure_offset, exit_offset;
    void *preferred;
    DWORD value;
    LONG displacement;
    HANDLE file;
    DWORD length;
    BOOL ret;

    preferred = VirtualAlloc( NULL, MIXED_IMAGE_SIZE, MEM_RESERVE, PAGE_NOACCESS );
    if (!preferred) return INVALID_HANDLE_VALUE;
    if (!VirtualFree( preferred, 0, MEM_RELEASE )) return INVALID_HANDLE_VALUE;

    memset( &dos, 0, sizeof(dos) );
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = sizeof(dos);

    memset( &nt, 0, sizeof(nt) );
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = ARRAY_SIZE(sections);
    nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
    nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE |
                                    IMAGE_FILE_DLL;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.MajorLinkerVersion = 1;
    nt.OptionalHeader.SizeOfCode = MIXED_IMAGE_RAW_SIZE;
    nt.OptionalHeader.SizeOfInitializedData = 3 * MIXED_IMAGE_RAW_SIZE;
    nt.OptionalHeader.AddressOfEntryPoint = MIXED_IMAGE_TEXT_RVA;
    nt.OptionalHeader.BaseOfCode = MIXED_IMAGE_TEXT_RVA;
    nt.OptionalHeader.ImageBase = (ULONG_PTR)preferred;
    nt.OptionalHeader.SectionAlignment = MIXED_IMAGE_SECTION_SIZE;
    nt.OptionalHeader.FileAlignment = MIXED_IMAGE_RAW_SIZE;
    nt.OptionalHeader.MajorOperatingSystemVersion = 6;
    nt.OptionalHeader.MajorSubsystemVersion = 6;
    nt.OptionalHeader.SizeOfImage = MIXED_IMAGE_SIZE;
    nt.OptionalHeader.SizeOfHeaders = MIXED_IMAGE_RAW_SIZE;
    nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    nt.OptionalHeader.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
    nt.OptionalHeader.SizeOfStackReserve = 0x100000;
    nt.OptionalHeader.SizeOfStackCommit = 0x1000;
    nt.OptionalHeader.SizeOfHeapReserve = 0x100000;
    nt.OptionalHeader.SizeOfHeapCommit = 0x1000;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

    memset( sections, 0, sizeof(sections) );
    memcpy( sections[0].Name, ".text", sizeof(".text") );
    sections[0].Misc.VirtualSize = MIXED_IMAGE_SECTION_SIZE;
    sections[0].VirtualAddress = MIXED_IMAGE_TEXT_RVA;
    sections[0].SizeOfRawData = MIXED_IMAGE_RAW_SIZE;
    sections[0].PointerToRawData = MIXED_IMAGE_RAW_SIZE;
    sections[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_READ |
                                  IMAGE_SCN_MEM_EXECUTE;
    memcpy( sections[1].Name, ".data", sizeof(".data") );
    sections[1].Misc.VirtualSize = MIXED_IMAGE_SECTION_SIZE;
    sections[1].VirtualAddress = MIXED_IMAGE_DATA_RVA;
    sections[1].SizeOfRawData = MIXED_IMAGE_RAW_SIZE;
    sections[1].PointerToRawData = 2 * MIXED_IMAGE_RAW_SIZE;
    sections[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                                  IMAGE_SCN_MEM_WRITE;
    memcpy( sections[2].Name, ".rdata", sizeof(".rdata") );
    sections[2].Misc.VirtualSize = MIXED_IMAGE_SECTION_SIZE;
    sections[2].VirtualAddress = MIXED_IMAGE_RDATA_RVA;
    sections[2].SizeOfRawData = MIXED_IMAGE_RAW_SIZE;
    sections[2].PointerToRawData = 3 * MIXED_IMAGE_RAW_SIZE;
    sections[2].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
    memcpy( sections[3].Name, ".tail", sizeof(".tail") );
    sections[3].Misc.VirtualSize = MIXED_IMAGE_SECTION_SIZE;
    sections[3].VirtualAddress = MIXED_IMAGE_TAIL_RVA;
    sections[3].SizeOfRawData = MIXED_IMAGE_RAW_SIZE;
    sections[3].PointerToRawData = 4 * MIXED_IMAGE_RAW_SIZE;
    sections[3].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                                  IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE;

    /* movl $written_value, data(%rip) */
    text[offset++] = 0xc7;
    text[offset++] = 0x05;
    displacement_offset = offset;
    offset += sizeof(displacement);
    memcpy( text + offset, &written_value, sizeof(written_value) );
    offset += sizeof(written_value);
    displacement = MIXED_IMAGE_DATA_RVA - (MIXED_IMAGE_TEXT_RVA + offset);
    memcpy( text + displacement_offset, &displacement, sizeof(displacement) );

    /* cmpl $readonly_value, rdata(%rip) */
    text[offset++] = 0x81;
    text[offset++] = 0x3d;
    displacement_offset = offset;
    offset += sizeof(displacement);
    memcpy( text + offset, &readonly_value, sizeof(readonly_value) );
    offset += sizeof(readonly_value);
    displacement = MIXED_IMAGE_RDATA_RVA - (MIXED_IMAGE_TEXT_RVA + offset);
    memcpy( text + displacement_offset, &displacement, sizeof(displacement) );

    text[offset++] = 0x75;  /* jne failure */
    branch_offset = offset++;
    text[offset++] = 0xb9;  /* mov $0x6500,%ecx */
    value = 0x6500;
    memcpy( text + offset, &value, sizeof(value) );
    offset += sizeof(value);
    text[offset++] = 0xeb;  /* jmp exit */
    displacement_offset = offset++;
    failure_offset = offset;
    text[offset++] = 0xb9;  /* mov $0xe201,%ecx */
    value = 0xe201;
    memcpy( text + offset, &value, sizeof(value) );
    offset += sizeof(value);
    exit_offset = offset;
    text[branch_offset] = failure_offset - branch_offset - 1;
    text[displacement_offset] = exit_offset - displacement_offset - 1;

    text[offset++] = 0xff;  /* call *%rax (RtlExitUserThread) */
    text[offset++] = 0xd0;
    text[offset++] = 0xc3;

    memcpy( rdata, &readonly_value, sizeof(readonly_value) );

    length = GetTempPathA( ARRAY_SIZE(temp_path), temp_path );
    if (!length || length >= ARRAY_SIZE(temp_path) ||
        !GetTempFileNameA( temp_path, "x64", 0, path ))
        return INVALID_HANDLE_VALUE;
    file = CreateFileA( path, GENERIC_WRITE,
                        FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        DeleteFileA( path );
        return file;
    }
    ret = write_image_part( file, 0, &dos, sizeof(dos) ) &&
          write_image_part( file, dos.e_lfanew, &nt, sizeof(nt) ) &&
          write_image_part( file, dos.e_lfanew + sizeof(nt), sections, sizeof(sections) ) &&
          write_image_part( file, sections[0].PointerToRawData, text, sizeof(text) ) &&
          write_image_part( file, sections[1].PointerToRawData, data, sizeof(data) ) &&
          write_image_part( file, sections[2].PointerToRawData, rdata, sizeof(rdata) ) &&
          write_image_part( file, sections[3].PointerToRawData, tail, sizeof(tail) );
    CloseHandle( file );
    if (ret)
    {
        file = CreateFileA( path, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
        if (file != INVALID_HANDLE_VALUE) return file;
    }
    DeleteFileA( path );
    return INVALID_HANDLE_VALUE;
}

static DWORD CALLBACK mixed_image_thread( void *arg )
{
    struct mixed_image_thread_args *args = arg;
    CONTEXT *context;

    context = &NtCurrentTeb()->ChpeV2CpuAreaInfo->ContextAmd64->AMD64_Context;
    context->Rax = (ULONG_PTR)args->exit_thread;
    context->Rsp = (ULONG_PTR)&context - 0x800;
    context->Rip = (ULONG_PTR)args->entry;
    NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation = 1;
    args->begin_simulation();
    return 0xe202;
}

static void check_image_protection( const void *address, DWORD expected, const char *section )
{
    MEMORY_BASIC_INFORMATION info;
    SIZE_T ret;

    memset( &info, 0, sizeof(info) );
    ret = VirtualQuery( address, &info, sizeof(info) );
    ok( ret == sizeof(info), "%s VirtualQuery returned %Iu, error %lu\n",
        section, ret, GetLastError() );
    if (ret == sizeof(info))
    {
        ok( (info.Protect & 0xff) == expected, "%s protection %#lx, expected %#lx\n",
            section, info.Protect, expected );
        ok( info.State == MEM_COMMIT, "%s state %#lx\n", section, info.State );
        ok( info.Type == MEM_IMAGE, "%s type %#lx\n", section, info.Type );
    }
}

static BOOL check_identity_image_region( const void *address, const void *allocation,
                                         DWORD expected, const char *section )
{
    WINE_TRANSLATED_VIEW_INFORMATION info = {0};
    NTSTATUS status;

    status = NtQueryVirtualMemory( GetCurrentProcess(), address,
                                   MemoryWineTranslatedViewInformation,
                                   &info, sizeof(info), NULL );
    ok( !status, "%s translated-view query returned %#lx\n", section, status );
    if (status) return FALSE;
    ok( info.Version == WINE_TRANSLATED_VIEW_INFORMATION_VERSION,
        "%s translated-view version %lu\n", section, info.Version );
    ok( !info.Flags, "%s translated-view flags %#lx, expected identity\n",
        section, info.Flags );
    ok( info.GuestBase == address, "%s guest base %p, expected %p\n",
        section, info.GuestBase, address );
    ok( info.HostBase == address, "%s host base %p, expected %p\n",
        section, info.HostBase, address );
    ok( info.AllocationBase == allocation, "%s allocation base %p, expected %p\n",
        section, info.AllocationBase, allocation );
    ok( info.RegionSize == MIXED_IMAGE_SECTION_SIZE,
        "%s region size %#Ix, expected %#x\n", section, info.RegionSize,
        MIXED_IMAGE_SECTION_SIZE );
    ok( (info.Protect & 0xff) == expected, "%s translated-view protection %#lx, expected %#lx\n",
        section, info.Protect, expected );
    ok( !info.Reserved, "%s translated-view reserved field %#lx\n",
        section, info.Reserved );
    return info.Version == WINE_TRANSLATED_VIEW_INFORMATION_VERSION && !info.Flags &&
           info.GuestBase == address && info.HostBase == address &&
           info.AllocationBase == allocation && info.RegionSize == MIXED_IMAGE_SECTION_SIZE &&
           (info.Protect & 0xff) == expected && !info.Reserved;
}

/* Force the shared host page through a non-writable state before restoring the
 * image's writable logical lane.  On a 16K Darwin host the untagged path then
 * requests RWX and fails with EACCES.  Ignoring that failure cannot make this
 * regression pass: the native write below would still fault on the read-only
 * host page before the provider gets a chance to execute the x64 write. */
static BOOL restore_mixed_image_writable_lane( void *base )
{
    DWORD old_protect, restore_old;
    void *data = (BYTE *)base + MIXED_IMAGE_DATA_RVA;
    volatile DWORD *value = data;
    BOOL ret;

    ret = VirtualProtect( data, MIXED_IMAGE_SECTION_SIZE, PAGE_READONLY, &old_protect );
    ok( ret, "mixed image writable-to-read-only transition failed, error %lu\n",
        GetLastError() );
    if (!ret) return FALSE;
    /* VirtualProtect reports writable image copy-on-write pages as read/write
     * when the requested replacement protection is non-executable. */
    ok( (old_protect & 0xff) == PAGE_READWRITE,
        "mixed image writable lane old protection %#lx\n", old_protect );

    ret = VirtualProtect( data, MIXED_IMAGE_SECTION_SIZE, PAGE_WRITECOPY, &restore_old );
    ok( ret, "mixed RX/RW host-page restoration failed, error %lu\n", GetLastError() );
    if (!ret) return FALSE;
    ok( (restore_old & 0xff) == PAGE_READONLY,
        "mixed image restored lane old protection %#lx\n", restore_old );

    *value = 0x24681357;
    ok( *value == 0x24681357, "mixed image native writable-lane probe did not persist\n" );
    return TRUE;
}

/* Keep the final logical image page deliberately short of its 16K host page.
 * The native store must fault once for executable-write tracking, retain the
 * identity-image owner while reprotecting the rounded host page, and retry. */
static BOOL write_mixed_image_tail( void *base )
{
    MANAGE_WRITES_TO_EXECUTABLE_MEMORY process =
        { .Version = 2, .ProcessEnableWriteExceptions = 1 };
    MANAGE_WRITES_TO_EXECUTABLE_MEMORY thread = { .Version = 2, .ThreadAllowWrites = 1 };
    volatile DWORD *value = (DWORD *)((BYTE *)base + MIXED_IMAGE_TAIL_RVA);
    NTSTATUS status, cleanup_status;
    BOOL ret = FALSE;

    status = NtSetInformationThread( GetCurrentThread(), ThreadManageWritesToExecutableMemory,
                                     &thread, sizeof(thread) );
    ok( !status, "tail write allowance failed %#lx\n", status );
    if (status) return FALSE;

    status = NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                      &process, sizeof(process) );
    ok( !status, "tail executable-write tracking failed %#lx\n", status );
    if (!status)
    {
        *value = 0x31415926;
        ok( *value == 0x31415926, "tail executable-write retry did not persist\n" );
        ret = *value == 0x31415926;

        process.ProcessEnableWriteExceptions = 0;
        cleanup_status = NtSetInformationProcess( GetCurrentProcess(),
                                                  ProcessManageWritesToExecutableMemory,
                                                  &process, sizeof(process) );
        ok( !cleanup_status, "tail executable-write tracking cleanup failed %#lx\n",
            cleanup_status );
        if (cleanup_status) ret = FALSE;
    }

    thread.ThreadAllowWrites = 0;
    cleanup_status = NtSetInformationThread( GetCurrentThread(),
                                             ThreadManageWritesToExecutableMemory,
                                             &thread, sizeof(thread) );
    ok( !cleanup_status, "tail write allowance cleanup failed %#lx\n", cleanup_status );
    return ret && !cleanup_status;
}

static void test_mixed_image_mapping( void (WINAPI *begin_simulation)(void) )
{
    static WCHAR image_name[] = L"x64-provider-mixed-protection-image";
    struct mixed_image_thread_args args = { begin_simulation };
    WCHAR *arbitrary_user_pointer;
    MEMORY_BASIC_INFORMATION info;
    char path[MAX_PATH] = {0};
    LARGE_INTEGER offset = {0};
    SIZE_T size = 0;
    NTSTATUS status;
    DWORD exit_code, ret;
    HANDLE file, mapping = NULL, thread;
    void *base = NULL, *exit_thread;

    exit_thread = GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "RtlExitUserThread" );
    ok( !!exit_thread, "RtlExitUserThread is missing\n" );
    if (!exit_thread) return;
    file = create_mixed_protection_image( path );
    ok( file != INVALID_HANDLE_VALUE, "could not create mixed-protection image, error %lu\n",
        GetLastError() );
    if (file == INVALID_HANDLE_VALUE) return;
    mapping = CreateFileMappingA( file, NULL, PAGE_EXECUTE_READ | SEC_IMAGE, 0, 0, NULL );
    ok( !!mapping, "could not create mixed-protection image section, error %lu\n",
        GetLastError() );
    if (!mapping) goto done;

    arbitrary_user_pointer = NtCurrentTeb()->Tib.ArbitraryUserPointer;
    NtCurrentTeb()->Tib.ArbitraryUserPointer = image_name;
    /* The loader's view-level RX request cannot describe the writable and
     * read-only sections that the image mapper has already finalized. */
    status = NtMapViewOfSection( mapping, GetCurrentProcess(), &base, 0, 0, &offset,
                                 &size, ViewShare, 0, PAGE_EXECUTE_READ );
    NtCurrentTeb()->Tib.ArbitraryUserPointer = arbitrary_user_pointer;
    ok( NT_SUCCESS(status), "mixed-protection NtMapViewOfSection failed %#lx\n", status );
    if (!NT_SUCCESS(status)) goto done;
    ok( size == MIXED_IMAGE_SIZE, "mixed-protection image size %#Ix\n", size );
    ok( !((ULONG_PTR)base & (MIXED_IMAGE_HOST_PAGE - 1)),
        "mixed-protection image base %p is not 16K aligned\n", base );

    check_image_protection( (BYTE *)base + MIXED_IMAGE_TEXT_RVA,
                            PAGE_EXECUTE_READ, "RX section" );
    check_image_protection( (BYTE *)base + MIXED_IMAGE_DATA_RVA,
                            PAGE_WRITECOPY, "writable section" );
    check_image_protection( (BYTE *)base + MIXED_IMAGE_RDATA_RVA,
                            PAGE_READONLY, "read-only section" );
    check_image_protection( (BYTE *)base + MIXED_IMAGE_TAIL_RVA,
                            PAGE_EXECUTE_WRITECOPY, "executable-write tail section" );
    check_identity_image_region( base, base, PAGE_READONLY, "image header" );
    check_identity_image_region( (BYTE *)base + MIXED_IMAGE_TEXT_RVA, base,
                                 PAGE_EXECUTE_READ, "RX section" );
    check_identity_image_region( (BYTE *)base + MIXED_IMAGE_DATA_RVA, base,
                                 PAGE_WRITECOPY, "writable section" );
    check_identity_image_region( (BYTE *)base + MIXED_IMAGE_RDATA_RVA, base,
                                 PAGE_READONLY, "read-only section" );
    check_identity_image_region( (BYTE *)base + MIXED_IMAGE_TAIL_RVA, base,
                                 PAGE_EXECUTE_WRITECOPY, "executable-write tail section" );
    ret = VirtualQuery( (BYTE *)base + MIXED_IMAGE_DATA_RVA, &info, sizeof(info) );
    if (ret != sizeof(info) || (info.Protect & 0xff) != PAGE_WRITECOPY) goto done;
    if (!restore_mixed_image_writable_lane( base )) goto done;
    check_image_protection( (BYTE *)base + MIXED_IMAGE_TEXT_RVA,
                            PAGE_EXECUTE_READ, "RX section after writable restore" );
    check_identity_image_region( (BYTE *)base + MIXED_IMAGE_DATA_RVA, base,
                                 PAGE_WRITECOPY, "restored writable section" );

    args.entry = (BYTE *)base + MIXED_IMAGE_TEXT_RVA;
    args.exit_thread = exit_thread;
    thread = CreateThread( NULL, 0, mixed_image_thread, &args, 0, NULL );
    ok( !!thread, "mixed-protection simulation thread creation failed, error %lu\n",
        GetLastError() );
    if (!thread) goto done;
    ret = WaitForSingleObject( thread, 10000 );
    ok( ret == WAIT_OBJECT_0, "mixed-protection simulation wait returned %#lx\n", ret );
    if (ret == WAIT_OBJECT_0)
    {
        ret = GetExitCodeThread( thread, &exit_code );
        ok( ret, "mixed-protection GetExitCodeThread failed, error %lu\n", GetLastError() );
        if (ret) ok( exit_code == 0x6500, "mixed-protection exit code %#lx\n", exit_code );
        ok( *(DWORD *)((BYTE *)base + MIXED_IMAGE_DATA_RVA) == 0x13572468,
            "mixed-protection writable section was not updated\n" );
    }
    CloseHandle( thread );
    if (write_mixed_image_tail( base ))
    {
        check_image_protection( (BYTE *)base + MIXED_IMAGE_TAIL_RVA,
                                PAGE_EXECUTE_WRITECOPY,
                                "executable-write tail section after retry" );
        check_identity_image_region( (BYTE *)base + MIXED_IMAGE_TAIL_RVA, base,
                                     PAGE_EXECUTE_WRITECOPY,
                                     "executable-write tail section after retry" );
    }

done:
    if (base) NtUnmapViewOfSection( GetCurrentProcess(), base );
    if (mapping) CloseHandle( mapping );
    CloseHandle( file );
    DeleteFileA( path );
}

struct concurrent_simulation_args
{
    void (WINAPI *begin_simulation)(void);
    volatile LONG *ready;
    UINT self;
};

static DWORD CALLBACK concurrent_simulation_thread( void *arg )
{
    struct concurrent_simulation_args *args = arg;
    BYTE code[] =
    {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, /* movabs $ready,%rax */
        0xc7, 0x40, 0, 1, 0, 0, 0,             /* movl $1,self*4(%rax) */
        0x83, 0x78, 0, 1,                       /* cmpl $1,peer*4(%rax) */
        0x72, 0xfa,                             /* jb peer wait */
        0xb9, 0, 0, 0, 0,                      /* mov $exit_code,%ecx */
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, /* movabs $RtlExitUserThread,%rax */
        0xff, 0xd0,                             /* call *%rax */
        0xc3,
    };
    CONTEXT *context;
    DWORD old_protect, exit_code = 0x6400 + args->self;
    void *address, *target;

    if (!(address = VirtualAlloc( NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE ))) return 0xe101;
    if (!(target = GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "RtlExitUserThread" ))) return 0xe102;
    *(void **)(code + 2) = (void *)args->ready;
    code[12] = args->self * sizeof(*args->ready);
    code[19] = (1 - args->self) * sizeof(*args->ready);
    memcpy( code + 24, &exit_code, sizeof(exit_code) );
    *(void **)(code + 30) = target;
    memcpy( address, code, sizeof(code) );
    if (!VirtualProtect( address, 0x1000, PAGE_EXECUTE_READ, &old_protect )) return 0xe103;

    context = &NtCurrentTeb()->ChpeV2CpuAreaInfo->ContextAmd64->AMD64_Context;
    context->Rsp = (ULONG_PTR)&context - 0x800;
    context->Rip = (ULONG_PTR)address;
    NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation = 1;
    args->begin_simulation();
    return 0xe104;
}

static void test_concurrent_provider( void (WINAPI *begin_simulation)(void) )
{
    struct concurrent_simulation_args args[2];
    volatile LONG ready[2] = {0};
    DWORD exit_code, ret;
    HANDLE threads[2];
    UINT i;

    memset( args, 0, sizeof(args) );
    for (i = 0; i < ARRAY_SIZE(args); ++i)
    {
        args[i].begin_simulation = begin_simulation;
        args[i].ready = ready;
        args[i].self = i;
        threads[i] = CreateThread( NULL, 0, concurrent_simulation_thread, &args[i], 0, NULL );
        ok( !!threads[i], "concurrent thread %u creation failed, error %lu\n",
            i, GetLastError() );
        if (!threads[i])
        {
            InterlockedExchange( ready, 1 );
            InterlockedExchange( ready + 1, 1 );
            if (i) WaitForMultipleObjects( i, threads, TRUE, 10000 );
            while (i) CloseHandle( threads[--i] );
            return;
        }
    }

    ret = WaitForMultipleObjects( ARRAY_SIZE(threads), threads, TRUE, 10000 );
    ok( ret == WAIT_OBJECT_0,
        "mutually dependent x64 guest loops did not run concurrently, wait %#lx\n", ret );
    if (ret != WAIT_OBJECT_0)
    {
        /* Release a serialized implementation so the regression reports its
         * failure without stranding provider threads in the test process. */
        InterlockedExchange( ready, 1 );
        InterlockedExchange( ready + 1, 1 );
        WaitForMultipleObjects( ARRAY_SIZE(threads), threads, TRUE, 10000 );
    }
    for (i = 0; i < ARRAY_SIZE(threads); ++i)
    {
        ret = GetExitCodeThread( threads[i], &exit_code );
        ok( ret, "GetExitCodeThread(%u) failed, error %lu\n", i, GetLastError() );
        if (ret) ok( exit_code == 0x6400 + i, "thread %u exit code %#lx\n", i, exit_code );
        CloseHandle( threads[i] );
    }
}

struct x64_memory_fault_args
{
    void (WINAPI *begin_simulation)(void);
    void *entry;
};

static LONG x64_memory_fault_handler_calls;
static void *x64_memory_fault_instruction;
static void *x64_memory_fault_resume;
static void *x64_memory_fault_address;
static ULONG_PTR x64_memory_fault_access;
static const char *x64_memory_fault_name;

static LONG WINAPI x64_memory_fault_handler( EXCEPTION_POINTERS *ptrs )
{
    EXCEPTION_RECORD *rec = ptrs->ExceptionRecord;
    CONTEXT *context = ptrs->ContextRecord;

    if (rec->ExceptionCode != STATUS_ACCESS_VIOLATION ||
        rec->ExceptionAddress != x64_memory_fault_instruction)
        return EXCEPTION_CONTINUE_SEARCH;

    InterlockedIncrement( &x64_memory_fault_handler_calls );
    ok( rec->NumberParameters == 2, "%s x64 memory fault parameter count %lu\n",
        x64_memory_fault_name, rec->NumberParameters );
    ok( rec->ExceptionInformation[0] == x64_memory_fault_access,
        "%s x64 memory fault access %#Ix, expected %#Ix\n",
        x64_memory_fault_name, rec->ExceptionInformation[0],
        x64_memory_fault_access );
    ok( rec->ExceptionInformation[1] == (ULONG_PTR)x64_memory_fault_address,
        "%s x64 memory fault address %p, expected %p\n",
        x64_memory_fault_name, (void *)rec->ExceptionInformation[1],
        x64_memory_fault_address );
    ok( context->Rip == (ULONG_PTR)x64_memory_fault_instruction,
        "%s x64 memory fault RIP %p, expected %p\n",
        x64_memory_fault_name, (void *)(ULONG_PTR)context->Rip,
        x64_memory_fault_instruction );
    context->Rip = (ULONG_PTR)x64_memory_fault_resume;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD CALLBACK x64_memory_fault_thread( void *arg )
{
    struct x64_memory_fault_args *args = arg;
    CONTEXT *context;

    context = &NtCurrentTeb()->ChpeV2CpuAreaInfo->ContextAmd64->AMD64_Context;
    context->Rsp = (ULONG_PTR)&context - 0x800;
    context->Rip = (ULONG_PTR)args->entry;
    NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation = 1;
    args->begin_simulation();
    return 0xe301;
}

struct x64_memory_fault_case
{
    const char *name;
    ULONG_PTR access;
};

static void run_x64_memory_fault_case( void (WINAPI *begin_simulation)(void),
                                       const struct x64_memory_fault_case *fault_case,
                                       unsigned int index )
{
    BYTE code[64] = {0};
    struct x64_memory_fault_args args = { begin_simulation };
    void *address = NULL, *fault = NULL, *target, *handler = NULL;
    SIZE_T offset = 0, instruction_offset, branch_offset, resume_offset, exit_offset;
    DWORD old_protect, exit_code = 0, ret;
    DWORD failure_code = 0xe310 + index, success_code = 0x6600 + index;
    HANDLE thread = NULL;

    target = GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "RtlExitUserThread" );
    ok( !!target, "%s RtlExitUserThread is missing\n", fault_case->name );
    if (!target) return;
    address = VirtualAlloc( NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE );
    ok( !!address, "%s x64 memory fault code allocation failed, error %lu\n",
        fault_case->name, GetLastError() );
    if (!address) goto done;
    fault = VirtualAlloc( NULL, 0x1000, MEM_RESERVE, PAGE_NOACCESS );
    ok( !!fault, "%s x64 memory fault reserve allocation failed, error %lu\n",
        fault_case->name, GetLastError() );
    if (!fault) goto done;

    code[offset++] = 0x48;
    code[offset++] = 0xb8; /* movabs $fault,%rax */
    memcpy( code + offset, &fault, sizeof(fault) );
    offset += sizeof(fault);
    instruction_offset = offset;
    switch (fault_case->access)
    {
    case EXCEPTION_READ_FAULT:
        code[offset++] = 0x8b;
        code[offset++] = 0x08; /* movl (%rax),%ecx */
        break;
    case EXCEPTION_WRITE_FAULT:
        code[offset++] = 0xc7;
        code[offset++] = 0x00;
        code[offset++] = 0x78;
        code[offset++] = 0x56;
        code[offset++] = 0x34;
        code[offset++] = 0x12; /* movl $0x12345678,(%rax) */
        break;
    case EXCEPTION_EXECUTE_FAULT:
        code[offset++] = 0xff;
        code[offset++] = 0xe0; /* jmp *%rax */
        break;
    default:
        ok( 0, "unsupported x64 memory fault access %#Ix\n", fault_case->access );
        goto done;
    }
    code[offset++] = 0xb9; /* mov $failure_code,%ecx */
    memcpy( code + offset, &failure_code, sizeof(failure_code) );
    offset += sizeof(failure_code);
    code[offset++] = 0xeb; /* jmp exit */
    branch_offset = offset++;
    resume_offset = offset;
    code[offset++] = 0xb9; /* mov $success_code,%ecx */
    memcpy( code + offset, &success_code, sizeof(success_code) );
    offset += sizeof(success_code);
    exit_offset = offset;
    code[branch_offset] = exit_offset - branch_offset - 1;
    code[offset++] = 0x48;
    code[offset++] = 0xb8; /* movabs $RtlExitUserThread,%rax */
    memcpy( code + offset, &target, sizeof(target) );
    offset += sizeof(target);
    code[offset++] = 0xff;
    code[offset++] = 0xd0; /* call *%rax */
    code[offset++] = 0xc3;
    ok( offset <= sizeof(code), "%s x64 memory fault code overflowed\n",
        fault_case->name );
    if (offset > sizeof(code)) goto done;

    memcpy( address, code, offset );
    ret = VirtualProtect( address, 0x1000, PAGE_EXECUTE_READ, &old_protect );
    ok( ret, "%s x64 memory fault code protect failed, error %lu\n",
        fault_case->name, GetLastError() );
    if (!ret) goto done;

    x64_memory_fault_handler_calls = 0;
    x64_memory_fault_name = fault_case->name;
    x64_memory_fault_access = fault_case->access;
    x64_memory_fault_address = fault;
    x64_memory_fault_instruction = fault_case->access == EXCEPTION_EXECUTE_FAULT ?
                                   fault : (BYTE *)address + instruction_offset;
    x64_memory_fault_resume = (BYTE *)address + resume_offset;
    handler = AddVectoredExceptionHandler( TRUE, x64_memory_fault_handler );
    ok( !!handler, "%s x64 memory fault handler registration failed\n",
        fault_case->name );
    if (!handler) goto done;

    args.entry = address;
    thread = CreateThread( NULL, 0, x64_memory_fault_thread, &args, 0, NULL );
    ok( !!thread, "%s x64 memory fault thread creation failed, error %lu\n",
        fault_case->name, GetLastError() );
    if (!thread) goto done;
    ret = WaitForSingleObject( thread, 10000 );
    ok( ret == WAIT_OBJECT_0, "%s x64 memory fault wait returned %#lx\n",
        fault_case->name, ret );
    if (ret == WAIT_OBJECT_0)
    {
        ret = GetExitCodeThread( thread, &exit_code );
        ok( ret, "%s x64 memory fault GetExitCodeThread failed, error %lu\n",
            fault_case->name, GetLastError() );
        if (ret)
            ok( exit_code == success_code,
                "%s x64 memory fault exit code %#lx, expected %#lx\n",
                fault_case->name, exit_code, success_code );
        ok( x64_memory_fault_handler_calls == 1,
            "%s x64 memory fault handler called %ld times\n",
            fault_case->name, x64_memory_fault_handler_calls );
        trace( "XTAJIT64_MEMORY_FAULT_SEH access=%s/%#Ix address=%p exit=%#lx handlers=%ld\n",
               fault_case->name, fault_case->access, fault, exit_code,
               x64_memory_fault_handler_calls );
    }
    else
    {
        TerminateThread( thread, STATUS_TIMEOUT );
        WaitForSingleObject( thread, 10000 );
    }

done:
    if (thread) CloseHandle( thread );
    if (handler) RemoveVectoredExceptionHandler( handler );
    x64_memory_fault_name = NULL;
    x64_memory_fault_access = 0;
    x64_memory_fault_address = NULL;
    x64_memory_fault_instruction = NULL;
    x64_memory_fault_resume = NULL;
    if (fault) VirtualFree( fault, 0, MEM_RELEASE );
    if (address) VirtualFree( address, 0, MEM_RELEASE );
}

static void test_x64_memory_fault_exception( void (WINAPI *begin_simulation)(void) )
{
    static const struct x64_memory_fault_case cases[] =
    {
        { "read", EXCEPTION_READ_FAULT },
        { "write", EXCEPTION_WRITE_FAULT },
        { "execute", EXCEPTION_EXECUTE_FAULT },
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(cases); ++i)
        run_x64_memory_fault_case( begin_simulation, &cases[i], i );
}

static void test_reset_to_consistent_state( HMODULE module )
{
    void (WINAPI *reset_to_consistent_state)(EXCEPTION_RECORD *, CONTEXT *, ARM64_NT_CONTEXT *);
    ARM64_NT_CONTEXT arm_context, original_arm_context;
    CONTEXT context, original_context;
    EXCEPTION_RECORD record;

    reset_to_consistent_state = (void *)GetProcAddress( module, "ResetToConsistentState" );
    ok( !!reset_to_consistent_state, "ResetToConsistentState is missing\n" );
    if (!reset_to_consistent_state) return;

    memset( &record, 0, sizeof(record) );
    record.ExceptionCode = STATUS_ACCESS_VIOLATION;
    record.ExceptionAddress = (void *)0x12345000;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = EXCEPTION_EXECUTE_FAULT;
    record.ExceptionInformation[1] = (ULONG_PTR)record.ExceptionAddress;
    memset( &context, 0, sizeof(context) );
    context.ContextFlags = CONTEXT_AMD64_FULL;
    context.Rip = (ULONG_PTR)record.ExceptionAddress;
    context.Rsp = (ULONG_PTR)&context;
    memset( &arm_context, 0, sizeof(arm_context) );
    arm_context.ContextFlags = CONTEXT_ARM64_FULL;
    arm_context.Pc = (ULONG_PTR)record.ExceptionAddress;
    arm_context.Sp = (ULONG_PTR)&arm_context;
    original_context = context;
    original_arm_context = arm_context;

    NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation = 0;
    reset_to_consistent_state( &record, &context, &arm_context );
    ok( !memcmp( &context, &original_context, sizeof(context) ),
        "native exception reset changed the AMD64 context\n" );
    ok( !memcmp( &arm_context, &original_arm_context, sizeof(arm_context) ),
        "native exception reset changed the ARM64 context\n" );
    ok( !NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation,
        "native exception reset entered simulation\n" );
}

static void test_provider_contract(void)
{
    static const ULONGLONG required_features =
        (1ull << PF_COMPARE_EXCHANGE_DOUBLE) |
        (1ull << PF_MMX_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_XMMI_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_RDTSC_INSTRUCTION_AVAILABLE) |
        (1ull << PF_XMMI64_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_NX_ENABLED) |
        (1ull << PF_SSE3_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_COMPARE_EXCHANGE128) |
        (1ull << PF_FASTFAIL_AVAILABLE) |
        (1ull << PF_RDTSCP_INSTRUCTION_AVAILABLE) |
        (1ull << PF_SSSE3_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_SSE4_1_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_SSE4_2_INSTRUCTIONS_AVAILABLE);
    BOOLEAN (WINAPI *is_feature_present)(UINT);
    void (WINAPI *update_processor_information)(SYSTEM_CPU_INFORMATION *);
    void (WINAPI *begin_simulation)(void);
    static const char marker[] = "SWITCHYARD_X64_OK\r\n";
    SYSTEM_CPU_INFORMATION info;
    PROCESS_MACHINE_INFORMATION machine_info;
    char image[MAX_PATH], provider_image[MAX_PATH];
    DWORD exit_code, ret, written;
    BOOL simulation_ok = FALSE;
    HMODULE module;
    HANDLE thread;
    UINT i;

    memset( &machine_info, 0, sizeof(machine_info) );
    ret = GetProcessInformation( GetCurrentProcess(), ProcessMachineTypeInfo,
                                 &machine_info, sizeof(machine_info) );
    ok( ret, "GetProcessInformation(ProcessMachineTypeInfo) failed, error %lu\n",
        GetLastError() );
    if (!ret) return;
    ok( machine_info.ProcessMachine == IMAGE_FILE_MACHINE_AMD64,
        "provider child machine %#x, expected AMD64\n", machine_info.ProcessMachine );
    if (machine_info.ProcessMachine != IMAGE_FILE_MACHINE_AMD64) return;
    ret = GetModuleFileNameA( NULL, image, ARRAY_SIZE(image) );
    ok( ret && ret < ARRAY_SIZE(image), "GetModuleFileNameA failed, error %lu\n", GetLastError() );
    if (!ret || ret >= ARRAY_SIZE(image)) return;
    trace( "XTAJIT64_CHILD pid=%lu image=\"%s\" process_machine=%#x\n",
           GetCurrentProcessId(), image, machine_info.ProcessMachine );

    module = GetModuleHandleA( "xtajit64.dll" );
    ok( !!module, "xtajit64.dll is not loaded\n" );
    if (!module) return;
    ret = GetModuleFileNameA( module, provider_image, ARRAY_SIZE(provider_image) );
    ok( ret && ret < ARRAY_SIZE(provider_image),
        "GetModuleFileNameA(xtajit64.dll) failed, error %lu\n", GetLastError() );
    if (!ret || ret >= ARRAY_SIZE(provider_image)) return;
    trace( "XTAJIT64_PROVIDER child_pid=%lu module=%p image=\"%s\"\n",
           GetCurrentProcessId(), module, provider_image );

    begin_simulation = (void *)GetProcAddress( module, "BeginSimulation" );
    is_feature_present = (void *)GetProcAddress( module, "BTCpu64IsProcessorFeaturePresent" );
    update_processor_information = (void *)GetProcAddress( module, "UpdateProcessorInformation" );
    ok( !!begin_simulation, "BeginSimulation is missing\n" );
    ok( !!is_feature_present, "BTCpu64IsProcessorFeaturePresent is missing\n" );
    ok( !!update_processor_information, "UpdateProcessorInformation is missing\n" );
    if (!begin_simulation || !is_feature_present || !update_processor_information) return;

    test_reset_to_consistent_state( module );

    for (i = 0; i < 64; i++)
        if (required_features & (1ull << i))
            ok( is_feature_present( i ), "missing required processor feature %u\n", i );

    memset( &info, 0xcc, sizeof(info) );
    info.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
    update_processor_information( &info );
    ok( info.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64,
        "wrong architecture %u\n", info.ProcessorArchitecture );
    ok( info.ProcessorLevel == 21, "wrong level %u\n", info.ProcessorLevel );
    ok( info.ProcessorRevision == 1, "wrong revision %u\n", info.ProcessorRevision );
    ok( info.MaximumProcessors == 0xcccc, "wrong max processors %u\n", info.MaximumProcessors );
    ok( info.ProcessorFeatureBits == 0xcccccccc, "wrong feature bits %#lx\n",
        info.ProcessorFeatureBits );

    thread = CreateThread( NULL, 0, simulation_thread, begin_simulation, 0, NULL );
    ok( !!thread, "thread creation failed, error %lu\n", GetLastError() );
    if (!thread) return;
    ret = WaitForSingleObject( thread, 10000 );
    ok( ret == WAIT_OBJECT_0, "wait returned %#lx\n", ret );
    if (ret == WAIT_OBJECT_0)
    {
        ret = GetExitCodeThread( thread, &exit_code );
        ok( ret, "GetExitCodeThread failed, error %lu\n", GetLastError() );
        if (ret)
        {
            simulation_ok = exit_code == 0x1234;
            ok( simulation_ok, "wrong simulation exit code %#lx\n", exit_code );
        }
    }
    CloseHandle( thread );

    test_x64_memory_fault_exception( begin_simulation );
    test_concurrent_provider( begin_simulation );
    /* This enables process-wide executable-write tracking; keep it last so
     * every earlier concurrency test has completed before policy changes. */
    test_mixed_image_mapping( begin_simulation );

    if (simulation_ok)
    {
        written = 0;
        ret = WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), marker, sizeof(marker) - 1, &written, NULL );
        ok( ret && written == sizeof(marker) - 1, "success marker write failed, error %lu\n", GetLastError() );
    }
}

#endif

#ifdef __aarch64__

static void run_arm64ec_child(void)
{
    static const char child_arg[] = " --x64-provider-child";
    struct _PROC_THREAD_ATTRIBUTE_LIST *list;
    STARTUPINFOEXA startup = {{ sizeof(startup) }};
    PROCESS_INFORMATION process;
    PROCESS_MACHINE_INFORMATION machine_info;
    ULONG_PTR list_buffer[128];
    char command[MAX_PATH], image[MAX_PATH];
    SIZE_T size = sizeof(list_buffer);
    USHORT machine = IMAGE_FILE_MACHINE_AMD64;
    DWORD exit_code, ret;
    int command_length;
    char **argv;

    winetest_get_mainargs( &argv );
    if (!argv || !argv[0] || !argv[1])
    {
        ok( 0, "provider child command line is missing\n" );
        return;
    }
    command_length = snprintf( command, sizeof(command), "\"%s\" %s%s",
                               argv[0], argv[1], child_arg );
    if (command_length < 0 || (SIZE_T)command_length >= sizeof(command))
    {
        ok( 0, "provider child command line is too long\n" );
        return;
    }
    memset( &machine_info, 0, sizeof(machine_info) );
    ret = GetProcessInformation( GetCurrentProcess(), ProcessMachineTypeInfo,
                                 &machine_info, sizeof(machine_info) );
    ok( ret, "GetProcessInformation(ProcessMachineTypeInfo) failed, error %lu\n",
        GetLastError() );
    if (!ret) return;
    ok( machine_info.ProcessMachine == IMAGE_FILE_MACHINE_ARM64,
        "provider parent machine %#x, expected ARM64\n", machine_info.ProcessMachine );
    if (machine_info.ProcessMachine != IMAGE_FILE_MACHINE_ARM64) return;
    ret = GetModuleFileNameA( NULL, image, ARRAY_SIZE(image) );
    ok( ret && ret < ARRAY_SIZE(image), "GetModuleFileNameA failed, error %lu\n", GetLastError() );
    if (!ret || ret >= ARRAY_SIZE(image)) return;
    trace( "XTAJIT64_PARENT pid=%lu image=\"%s\" process_machine=%#x child_command=\"%s\"\n",
           GetCurrentProcessId(), image, machine_info.ProcessMachine, command );
    startup.lpAttributeList = list = (void *)list_buffer;
    ret = InitializeProcThreadAttributeList( list, 1, 0, &size );
    ok( ret, "InitializeProcThreadAttributeList failed, error %lu\n", GetLastError() );
    if (!ret) return;
    ret = UpdateProcThreadAttribute( list, 0, PROC_THREAD_ATTRIBUTE_MACHINE_TYPE,
                                     &machine, sizeof(machine), NULL, NULL );
    ok( ret, "UpdateProcThreadAttribute failed, error %lu\n", GetLastError() );
    if (!ret)
    {
        DeleteProcThreadAttributeList( list );
        return;
    }
    ret = CreateProcessA( NULL, command, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                          NULL, NULL, &startup.StartupInfo, &process );
    ok( ret, "could not start ARM64EC provider test, error %lu\n", GetLastError() );
    DeleteProcThreadAttributeList( list );
    if (!ret) return;
    trace( "XTAJIT64_CHILD_CREATE parent_pid=%lu child_pid=%lu requested_machine=%#x image=\"%s\"\n",
           GetCurrentProcessId(), process.dwProcessId, machine, image );

    ret = WaitForSingleObject( process.hProcess, 15000 );
    ok( ret == WAIT_OBJECT_0, "ARM64EC provider test wait returned %#lx\n", ret );
    if (ret == WAIT_OBJECT_0)
    {
        ret = GetExitCodeProcess( process.hProcess, &exit_code );
        ok( ret, "GetExitCodeProcess failed, error %lu\n", GetLastError() );
        if (ret)
        {
            trace( "XTAJIT64_CHILD_EXIT child_pid=%lu exit_code=%#lx\n",
                   process.dwProcessId, exit_code );
            ok( !exit_code, "ARM64EC provider test exited %#lx\n", exit_code );
        }
    }
    else
    {
        TerminateProcess( process.hProcess, STATUS_TIMEOUT );
        WaitForSingleObject( process.hProcess, 10000 );
    }
    CloseHandle( process.hThread );
    CloseHandle( process.hProcess );
}

#endif

START_TEST(provider)
{
#ifdef __arm64ec__
    char **argv;

    winetest_get_mainargs( &argv );
    if (!argv || !argv[2] || strcmp( argv[2], "--x64-provider-child" ))
        win_skip( "provider contract runs only in the AMD64 child\n" );
    else
        test_provider_contract();
#elif defined __aarch64__
    run_arm64ec_child();
#else
    win_skip( "ARM64EC provider contract only\n" );
#endif
}
