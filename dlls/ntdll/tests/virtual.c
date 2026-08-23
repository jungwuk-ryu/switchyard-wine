/*
 * Unit test suite for the virtual memory APIs.
 *
 * Copyright 2019 Remi Bernon for CodeWeavers
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

#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/low_va.h"
#include "wine/test.h"
#include "ddk/wdm.h"

static unsigned int page_size;

static DWORD64 (WINAPI *pGetEnabledXStateFeatures)(void);
static NTSTATUS (WINAPI *pRtlCreateUserStack)(SIZE_T, SIZE_T, ULONG, SIZE_T, SIZE_T, INITIAL_TEB *);
static NTSTATUS (WINAPI *pRtlCreateUserThread)(HANDLE, SECURITY_DESCRIPTOR*, BOOLEAN, ULONG, SIZE_T,
                                               SIZE_T, PRTL_THREAD_START_ROUTINE, void*, HANDLE*, CLIENT_ID* );
static ULONG64 (WINAPI *pRtlGetEnabledExtendedFeatures)(ULONG64);
static NTSTATUS (WINAPI *pRtlFreeUserStack)(void *);
static void * (WINAPI *pRtlFindExportedRoutineByName)(HMODULE,const char*);
static BOOL (WINAPI *pIsWow64Process)(HANDLE, PBOOL);
static NTSTATUS (WINAPI *pNtAllocateVirtualMemoryEx)(HANDLE, PVOID *, SIZE_T *, ULONG, ULONG,
                                                     MEM_EXTENDED_PARAMETER *, ULONG);
static NTSTATUS (WINAPI *pNtMapViewOfSectionEx)(HANDLE, HANDLE, PVOID *, const LARGE_INTEGER *, SIZE_T *,
        ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
static NTSTATUS (WINAPI *pNtCreateSectionEx)(HANDLE *, ACCESS_MASK, const OBJECT_ATTRIBUTES *,
        const LARGE_INTEGER *, ULONG, ULONG, HANDLE, MEM_EXTENDED_PARAMETER *, ULONG);
static NTSTATUS (WINAPI *pNtSetInformationVirtualMemory)(HANDLE, VIRTUAL_MEMORY_INFORMATION_CLASS,
                                                         ULONG_PTR, PMEMORY_RANGE_ENTRY,
                                                         PVOID, ULONG);

#ifndef __aarch64__
static NTSTATUS (WINAPI *pRtlGetNativeSystemInformation)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
#endif

#ifdef __x86_64__
static BOOLEAN (WINAPI *pRtlIsEcCode)(const void *);
#endif

static const BOOL is_win64 = sizeof(void*) != sizeof(int);
static BOOL is_wow64;

static SYSTEM_BASIC_INFORMATION sbi;

static HANDLE create_target_process(const char *arg);

#ifndef _WIN64
static LONG wow64_guard_exception_count;
static ULONG_PTR wow64_guard_exception_access;
static ULONG_PTR wow64_guard_exception_address;

static LONG WINAPI wow64_guard_exception_handler( EXCEPTION_POINTERS *ptrs )
{
    EXCEPTION_RECORD *record = ptrs->ExceptionRecord;

    if (record->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement( &wow64_guard_exception_count );
    if (record->NumberParameters >= 2)
    {
        wow64_guard_exception_access = record->ExceptionInformation[0];
        wow64_guard_exception_address = record->ExceptionInformation[1];
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void test_wow64_translated_guard_resolution(void)
{
    MEMORY_BASIC_INFORMATION info;
    ULONG old_protect;
    volatile BYTE *page;
    void *address;
    void *base;
    void *protect_page;
    void *handler;
    SIZE_T size;
    NTSTATUS status;
    BYTE value;

    if (!is_wow64) return;

    address = NULL;
    size = 0x10000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "WoW64 guard allocation failed %#lx\n", status );
    if (status) return;
    base = address;

    page = (BYTE *)address + page_size;
    *page = 0x6d;
    protect_page = (void *)page;
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &size,
                                     PAGE_READWRITE | PAGE_GUARD, &old_protect );
    ok( !status, "WoW64 guard protect failed %#lx\n", status );
    ok( !status && old_protect == PAGE_READWRITE,
        "WoW64 guard old protection was %#lx\n", old_protect );
    if (status) goto done;
    page = protect_page;

    wow64_guard_exception_count = 0;
    wow64_guard_exception_access = ~(ULONG_PTR)0;
    wow64_guard_exception_address = 0;
    handler = RtlAddVectoredExceptionHandler( TRUE, wow64_guard_exception_handler );
    ok( !!handler, "RtlAddVectoredExceptionHandler failed\n" );
    if (!handler) goto done;
    value = *page;
    RtlRemoveVectoredExceptionHandler( handler );

    ok( value == 0x6d, "guarded read returned %#x\n", value );
    ok( wow64_guard_exception_count == 1, "got %ld guard exceptions\n",
        wow64_guard_exception_count );
    ok( wow64_guard_exception_access == EXCEPTION_READ_FAULT,
        "got guard access %Ix\n", wow64_guard_exception_access );
    ok( wow64_guard_exception_address == (ULONG_PTR)page,
        "got guard address %p, expected %p\n",
        (void *)wow64_guard_exception_address, (void *)page );

    status = NtQueryVirtualMemory( NtCurrentProcess(), (void *)page,
                                   MemoryBasicInformation, &info, sizeof(info), NULL );
    ok( !status, "guard query failed %#lx\n", status );
    ok( !(info.Protect & PAGE_GUARD), "guard remained set in protection %#lx\n",
        info.Protect );
    value = *page;
    ok( value == 0x6d && wow64_guard_exception_count == 1,
        "second read returned %#x after %ld guard exceptions\n",
        value, wow64_guard_exception_count );

done:
    address = base;
    size = 0;
    status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
    ok( !status, "WoW64 guard release failed %#lx\n", status );
}

static DWORD run_wow64_translated_writewatch_child(void)
{
    void *addresses[1] = {NULL};
    volatile BYTE *memory;
    DWORD old_protect;
    ULONG granularity;
    ULONG_PTR count;

    memory = VirtualAlloc( NULL, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                           PAGE_READWRITE );
    if (!memory) return 1;

    /* The first fault makes this logical page writable.  On a 16K translated
     * host page it also dirties its three siblings. */
    memory[0] = 0x11;

    /* PAGE_EXECUTE remains readable on Windows.  The translated fault bridge
     * must use the effective Unix access instead of requiring VPROT_READ. */
    memory[0x2000] = 0x33;
    if (!VirtualProtect( (BYTE *)memory + 0x2000, 0x1000, PAGE_EXECUTE, &old_protect ))
    {
        VirtualFree( (void *)memory, 0, MEM_RELEASE );
        return 2;
    }
    if (memory[0x2000] != 0x33)
    {
        VirtualFree( (void *)memory, 0, MEM_RELEASE );
        return 3;
    }

    /* Make the watched sibling executable.  Its executable-write policy must
     * not be inherited by the ordinary writable faulting lane. */
    if (!VirtualProtect( (BYTE *)memory + 0x1000, 0x1000,
                         PAGE_EXECUTE_READWRITE, &old_protect ))
    {
        VirtualFree( (void *)memory, 0, MEM_RELEASE );
        return 4;
    }

    /* Re-arm only the sibling.  The target page is now logically writable but
     * shares a physically read-only host page with a watched 4K lane. */
    count = ARRAY_SIZE(addresses);
    if (GetWriteWatch( WRITE_WATCH_FLAG_RESET, (BYTE *)memory + 0x1000, 0x1000,
                       addresses, &count, &granularity ))
    {
        VirtualFree( (void *)memory, 0, MEM_RELEASE );
        return 5;
    }

    memory[1] = 0x22;
    if (memory[1] != 0x22)
    {
        VirtualFree( (void *)memory, 0, MEM_RELEASE );
        return 6;
    }
    VirtualFree( (void *)memory, 0, MEM_RELEASE );
    return 0;
}

static void test_wow64_translated_writewatch_resolution(void)
{
    DWORD exit_code = ~0u, wait;
    HANDLE process;

    if (!is_wow64) return;
    process = create_target_process( "wow64_writewatch" );
    if (!process) return;
    wait = WaitForSingleObject( process, 5000 );
    ok( wait == WAIT_OBJECT_0, "WoW64 mixed write-watch child wait returned %#lx\n", wait );
    if (wait != WAIT_OBJECT_0) TerminateProcess( process, 0xdead );
    else
    {
        ok( GetExitCodeProcess( process, &exit_code ),
            "GetExitCodeProcess failed, error %lu\n", GetLastError() );
        ok( exit_code == 0, "WoW64 mixed write-watch child exited %#lx\n", exit_code );
    }
    CloseHandle( process );
}

static void test_wow64_translated_copy_protection(void)
{
    MEMORY_BASIC_INFORMATION info;
    volatile BYTE *memory;
    void *address, *page;
    SIZE_T size, transferred;
    ULONG old_protect, translated = 0;
    BYTE value, write_value;
    NTSTATUS status;

    if (!is_wow64) return;

    address = NULL;
    size = 0x10000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "WoW64 copy-protection allocation failed %#lx\n", status );
    if (status) return;
    memory = address;

    status = NtQueryVirtualMemory( NtCurrentProcess(), address,
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), NULL );
    if (status || !translated)
    {
        win_skip( "not running with translated WoW64 memory\n" );
        goto done;
    }

    memory[0] = 0x11;
    memory[page_size] = 0x22;
    memory[2 * page_size] = 0x33;

    page = (void *)(memory + page_size);
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &size,
                                     PAGE_NOACCESS, &old_protect );
    ok( !status && old_protect == PAGE_READWRITE,
        "WoW64 no-access protect returned %#lx, old protection %#lx\n",
        status, old_protect );
    if (status) goto done;

    page = (void *)(memory + 2 * page_size);
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &size,
                                     PAGE_READWRITE | PAGE_GUARD, &old_protect );
    ok( !status && old_protect == PAGE_READWRITE,
        "WoW64 guard protect returned %#lx, old protection %#lx\n",
        status, old_protect );
    if (status) goto restore_noaccess;

    value = 0;
    transferred = ~(SIZE_T)0;
    status = NtReadVirtualMemory( NtCurrentProcess(), (const void *)memory,
                                  &value, sizeof(value), &transferred );
    ok( !status && transferred == sizeof(value) && value == 0x11,
        "adjacent readable page returned %#lx, size %Iu, value %#x\n",
        status, transferred, value );

    value = 0xcc;
    transferred = ~(SIZE_T)0;
    status = NtReadVirtualMemory( NtCurrentProcess(), (const void *)(memory + page_size),
                                  &value, sizeof(value), &transferred );
    ok( status == STATUS_PARTIAL_COPY && !transferred && value == 0xcc,
        "no-access read returned %#lx, size %Iu, value %#x\n",
        status, transferred, value );

    value = 0xcc;
    transferred = ~(SIZE_T)0;
    status = NtReadVirtualMemory( NtCurrentProcess(), (const void *)(memory + 2 * page_size),
                                  &value, sizeof(value), &transferred );
    ok( status == STATUS_PARTIAL_COPY && !transferred && value == 0xcc,
        "guard read returned %#lx, size %Iu, value %#x\n",
        status, transferred, value );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (const void *)(memory + 2 * page_size),
                                   MemoryBasicInformation, &info, sizeof(info), NULL );
    ok( !status && (info.Protect & PAGE_GUARD),
        "guard query returned %#lx, protection %#lx\n", status, info.Protect );

    write_value = 0x44;
    transferred = ~(SIZE_T)0;
    status = NtWriteVirtualMemory( NtCurrentProcess(), (void *)memory,
                                   &write_value, sizeof(write_value), &transferred );
    ok( !status && transferred == sizeof(write_value) && memory[0] == write_value,
        "adjacent writable page returned %#lx, size %Iu, value %#x\n",
        status, transferred, memory[0] );

    transferred = ~(SIZE_T)0;
    status = NtWriteVirtualMemory( NtCurrentProcess(), (void *)(memory + page_size),
                                   &write_value, sizeof(write_value), &transferred );
    ok( status == STATUS_PARTIAL_COPY && !transferred,
        "no-access write returned %#lx, size %Iu\n", status, transferred );

    transferred = ~(SIZE_T)0;
    status = NtWriteVirtualMemory( NtCurrentProcess(), (void *)(memory + 2 * page_size),
                                   &write_value, sizeof(write_value), &transferred );
    ok( status == STATUS_PARTIAL_COPY && !transferred,
        "guard write returned %#lx, size %Iu\n", status, transferred );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (const void *)(memory + 2 * page_size),
                                   MemoryBasicInformation, &info, sizeof(info), NULL );
    ok( !status && (info.Protect & PAGE_GUARD),
        "guard query after write returned %#lx, protection %#lx\n", status, info.Protect );

    page = (void *)(memory + 2 * page_size);
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &size,
                                     PAGE_READWRITE, &old_protect );
    ok( !status, "WoW64 guard restore failed %#lx\n", status );
    if (!status)
        ok( memory[2 * page_size] == 0x33,
            "failed guard write changed value to %#x\n", memory[2 * page_size] );

restore_noaccess:
    page = (void *)(memory + page_size);
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &size,
                                     PAGE_READWRITE, &old_protect );
    ok( !status, "WoW64 no-access restore failed %#lx\n", status );
    if (!status)
        ok( memory[page_size] == 0x22,
            "failed no-access write changed value to %#x\n", memory[page_size] );

done:
    address = (void *)memory;
    size = 0;
    status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
    ok( !status, "WoW64 copy-protection release failed %#lx\n", status );
}

static void test_wow64_unowned_shadow_access(void)
{
    LARGE_INTEGER section_size;
    volatile BYTE *reference = NULL;
    void *short_view = NULL;
    SIZE_T reference_size = 0, short_size, transferred;
    BYTE value, write_value = 0x44;
    HANDLE section;
    NTSTATUS status;

    if (!is_wow64) return;

    section_size.QuadPart = 0x10000;
    status = NtCreateSection( &section, SECTION_ALL_ACCESS, NULL, &section_size,
                              PAGE_READWRITE, SEC_COMMIT, NULL );
    ok( !status, "unowned-shadow section creation failed %#lx\n", status );
    if (status) return;

    status = NtMapViewOfSection( section, NtCurrentProcess(), (void **)&reference, 0, 0,
                                 NULL, &reference_size, ViewShare, 0, PAGE_READWRITE );
    ok( !status, "reference section map failed %#lx\n", status );
    if (status) goto done;
    reference[page_size] = 0x22;

    short_size = page_size;
    status = NtMapViewOfSection( section, NtCurrentProcess(), &short_view, 0, 0,
                                 NULL, &short_size, ViewShare, 0, PAGE_READWRITE );
    ok( !status && short_size == page_size,
        "short section map returned %#lx, size %Iu\n", status, short_size );
    if (status) goto done;

    /* Darwin maps the containing 16K host page, but the adjacent 4K lane is
     * outside the tagged view and must remain logically inaccessible. */
    value = 0xcc;
    transferred = ~(SIZE_T)0;
    status = NtReadVirtualMemory( NtCurrentProcess(), (BYTE *)short_view + page_size,
                                  &value, sizeof(value), &transferred );
    ok( status == STATUS_PARTIAL_COPY && !transferred && value == 0xcc,
        "unowned shadow read returned %#lx, size %Iu, value %#x\n",
        status, transferred, value );

    transferred = ~(SIZE_T)0;
    status = NtWriteVirtualMemory( NtCurrentProcess(), (BYTE *)short_view + page_size,
                                   &write_value, sizeof(write_value), &transferred );
    ok( status == STATUS_PARTIAL_COPY && !transferred,
        "unowned shadow write returned %#lx, size %Iu\n", status, transferred );
    ok( reference[page_size] == 0x22,
        "failed unowned shadow write changed backing to %#x\n", reference[page_size] );

done:
    if (short_view)
    {
        status = NtUnmapViewOfSection( NtCurrentProcess(), short_view );
        ok( !status, "short section unmap failed %#lx\n", status );
    }
    if (reference)
    {
        status = NtUnmapViewOfSection( NtCurrentProcess(), (void *)reference );
        ok( !status, "reference section unmap failed %#lx\n", status );
    }
    NtClose( section );
}

static void test_wow64_virtual_guest_marshalling(void)
{
    MEM_ADDRESS_REQUIREMENTS *requirements;
    MEM_EXTENDED_PARAMETER *parameter;
    MEMORY_WORKING_SET_EX_INFORMATION *working_set;
    MEMORY_RANGE_ENTRY *ranges;
    MEMORY_BASIC_INFORMATION *query_info;
    WINE_PROCESS_VM_INFORMATION *machine_info;
    LARGE_INTEGER section_size;
    void **address_cell, **watch_addresses;
    SIZE_T *size_cell, size;
    ULONG *old_protect_cell;
    ULONG old_protect, translated = 0, granularity, prefetch_flags = 0;
    ULONG_PTR watch_count;
    BYTE *cells, *watch_memory;
    void *target = NULL, *page, *candidate, *view;
    HANDLE section = NULL, restricted = NULL, query_handle = NULL;
    NTSTATUS status;
    DWORD handles_before = 0, handles_after = 0;
    BOOL have_handle_count;
    unsigned int i;

    if (!is_wow64 || !winetest_platform_is_wine || page_size != 0x1000) return;

    cells = VirtualAlloc( NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    ok( !!cells, "guest-marshalling cell allocation failed %lu\n", GetLastError() );
    if (!cells) return;
    status = NtQueryVirtualMemory( NtCurrentProcess(), cells,
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), NULL );
    if (status || !translated)
    {
        win_skip( "not running with translated WoW64 memory\n" );
        VirtualFree( cells, 0, MEM_RELEASE );
        return;
    }

    address_cell = (void **)(cells + 0x100);
    size_cell = (SIZE_T *)(cells + 0x1100);
    old_protect_cell = (ULONG *)(cells + 0x2100);

    size = 0x10000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &target, 0, &size,
                                      MEM_RESERVE, PAGE_READWRITE );
    ok( !status, "target reserve failed %#lx\n", status );
    if (status) goto done;

    *address_cell = (BYTE *)target + page_size;
    *size_cell = page_size;
    ok( VirtualProtect( cells + 0x1000, page_size, PAGE_READONLY, &old_protect ),
        "size-cell read-only protect failed %lu\n", GetLastError() );
    status = NtAllocateVirtualMemory( NtCurrentProcess(), address_cell, 0, size_cell,
                                      MEM_COMMIT, PAGE_READWRITE );
    ok( status == STATUS_ACCESS_VIOLATION,
        "protected commit output returned %#lx\n", status );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (BYTE *)target + page_size,
                                   MemoryBasicInformation, &size, sizeof(size), NULL );
    ok( status == STATUS_INFO_LENGTH_MISMATCH,
        "short reserve-state query returned %#lx\n", status );
    {
        MEMORY_BASIC_INFORMATION info;

        status = NtQueryVirtualMemory( NtCurrentProcess(), (BYTE *)target + page_size,
                                       MemoryBasicInformation, &info, sizeof(info), NULL );
        ok( !status && info.State == MEM_RESERVE,
            "failed commit changed state, status %#lx state %#lx\n", status, info.State );
    }
    ok( VirtualProtect( cells + 0x1000, page_size, PAGE_READWRITE, &old_protect ),
        "size-cell restore failed %lu\n", GetLastError() );
    status = NtAllocateVirtualMemory( NtCurrentProcess(), address_cell, 0, size_cell,
                                      MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "valid commit follow-up returned %#lx\n", status );

    *address_cell = (BYTE *)target + page_size;
    *size_cell = page_size;
    *old_protect_cell = 0xdeadbeef;
    ok( VirtualProtect( cells + 0x2000, page_size, PAGE_READWRITE | PAGE_GUARD,
                        &old_protect ), "old-protection guard failed %lu\n", GetLastError() );
    status = NtProtectVirtualMemory( NtCurrentProcess(), address_cell, size_cell,
                                     PAGE_READONLY, old_protect_cell );
    ok( status == STATUS_GUARD_PAGE_VIOLATION,
        "guarded old-protection output returned %#lx\n", status );
    {
        MEMORY_BASIC_INFORMATION info;

        status = NtQueryVirtualMemory( NtCurrentProcess(), (BYTE *)target + page_size,
                                       MemoryBasicInformation, &info, sizeof(info), NULL );
        ok( !status && info.Protect == PAGE_READWRITE,
            "failed protect changed target, status %#lx protection %#lx\n",
            status, info.Protect );
        status = NtQueryVirtualMemory( NtCurrentProcess(), cells + 0x2000,
                                       MemoryBasicInformation, &info, sizeof(info), NULL );
        ok( !status && (info.Protect & PAGE_GUARD),
            "output probe consumed guard, status %#lx protection %#lx\n",
            status, info.Protect );
    }
    ok( VirtualProtect( cells + 0x2000, page_size, PAGE_READWRITE, &old_protect ),
        "old-protection guard restore failed %lu\n", GetLastError() );
    status = NtProtectVirtualMemory( NtCurrentProcess(), address_cell, size_cell,
                                     PAGE_READONLY, old_protect_cell );
    ok( !status && *old_protect_cell == PAGE_READWRITE,
        "valid protect follow-up returned %#lx old %#lx\n", status, *old_protect_cell );
    status = NtProtectVirtualMemory( NtCurrentProcess(), address_cell, size_cell,
                                     PAGE_READWRITE, old_protect_cell );
    ok( !status, "target protection restore returned %#lx\n", status );

    *address_cell = (BYTE *)target + page_size;
    *size_cell = page_size;
    ok( VirtualProtect( cells + 0x1000, page_size, PAGE_READONLY, &old_protect ),
        "free size-cell protect failed %lu\n", GetLastError() );
    status = NtFreeVirtualMemory( NtCurrentProcess(), address_cell, size_cell, MEM_DECOMMIT );
    ok( status == STATUS_ACCESS_VIOLATION,
        "protected decommit output returned %#lx\n", status );
    {
        MEMORY_BASIC_INFORMATION info;

        status = NtQueryVirtualMemory( NtCurrentProcess(), (BYTE *)target + page_size,
                                       MemoryBasicInformation, &info, sizeof(info), NULL );
        ok( !status && info.State == MEM_COMMIT,
            "failed decommit changed state, status %#lx state %#lx\n", status, info.State );
    }
    ok( VirtualProtect( cells + 0x1000, page_size, PAGE_READWRITE, &old_protect ),
        "free size-cell restore failed %lu\n", GetLastError() );

    query_info = (MEMORY_BASIC_INFORMATION *)(cells + 0x4000 - sizeof(*query_info) / 2);
    memset( query_info, 0xcc, sizeof(*query_info) );
    ok( VirtualProtect( cells + 0x4000, page_size, PAGE_NOACCESS, &old_protect ),
        "query output no-access protect failed %lu\n", GetLastError() );
    status = NtQueryVirtualMemory( NtCurrentProcess(), target, MemoryBasicInformation,
                                   query_info, sizeof(*query_info), NULL );
    ok( status == STATUS_ACCESS_VIOLATION,
        "cross-page query output returned %#lx\n", status );
    ok( *(BYTE *)query_info == 0xcc, "failed query partially changed output\n" );
    ok( VirtualProtect( cells + 0x4000, page_size, PAGE_READWRITE, &old_protect ),
        "query output restore failed %lu\n", GetLastError() );
    status = NtQueryVirtualMemory( NtCurrentProcess(), target, MemoryBasicInformation,
                                   query_info, sizeof(*query_info), NULL );
    ok( !status && query_info->State == MEM_RESERVE,
        "valid query follow-up returned %#lx state %#lx\n", status, query_info->State );

    working_set = (MEMORY_WORKING_SET_EX_INFORMATION *)(
        cells + 0x5000 - sizeof(*working_set) );
    working_set[0].VirtualAddress = target;
    working_set[1].VirtualAddress = (BYTE *)target + page_size;
    ok( VirtualProtect( cells + 0x5000, page_size, PAGE_NOACCESS, &old_protect ),
        "working-set lane protect failed %lu\n", GetLastError() );
    status = NtQueryVirtualMemory( NtCurrentProcess(), NULL,
                                   MemoryWorkingSetExInformation, working_set,
                                   2 * sizeof(*working_set), NULL );
    ok( status == STATUS_ACCESS_VIOLATION,
        "cross-page WorkingSetEx input returned %#lx\n", status );
    ok( VirtualProtect( cells + 0x5000, page_size, PAGE_READWRITE, &old_protect ),
        "working-set lane restore failed %lu\n", GetLastError() );
    status = NtDuplicateObject( NtCurrentProcess(), NtCurrentProcess(), NtCurrentProcess(),
                                &restricted, SYNCHRONIZE, 0, 0 );
    ok( !status, "restricted self duplicate failed %#lx\n", status );
    if (!status)
    {
        status = NtQueryVirtualMemory( restricted, NULL, MemoryWorkingSetExInformation,
                                       working_set, sizeof(*working_set), NULL );
        ok( status == STATUS_ACCESS_DENIED,
            "restricted WorkingSetEx query returned %#lx\n", status );
        NtClose( restricted );
        restricted = NULL;
    }
    status = NtDuplicateObject( NtCurrentProcess(), NtCurrentProcess(), NtCurrentProcess(),
                                &query_handle, PROCESS_QUERY_INFORMATION, 0, 0 );
    ok( !status, "query self duplicate failed %#lx\n", status );
    if (!status)
    {
        status = NtQueryVirtualMemory( query_handle, NULL, MemoryWorkingSetExInformation,
                                       working_set, sizeof(*working_set), NULL );
        ok( !status, "valid WorkingSetEx follow-up returned %#lx\n", status );
        status = NtQueryVirtualMemory( query_handle, target,
                                       MemoryWineWow64TranslatedInformation,
                                       &translated, sizeof(translated), NULL );
        ok( status == STATUS_INVALID_HANDLE,
            "duplicated-self translated-info query returned %#lx\n", status );
        NtClose( query_handle );
        query_handle = NULL;
    }

    watch_memory = VirtualAlloc( NULL, 0x10000,
                                 MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                                 PAGE_READWRITE );
    ok( !!watch_memory, "write-watch allocation failed %lu\n", GetLastError() );
    if (watch_memory)
    {
        BOOL found = FALSE;

        watch_memory[0] = 0x5a;
        watch_memory[page_size] = 0xa5;
        watch_addresses = (void **)(cells + 0x6000 - sizeof(*watch_addresses));
        watch_count = 2;
        ok( VirtualProtect( cells + 0x6000, page_size, PAGE_NOACCESS, &old_protect ),
            "write-watch output lane protect failed %lu\n", GetLastError() );
        status = NtGetWriteWatch( NtCurrentProcess(), WRITE_WATCH_FLAG_RESET,
                                  watch_memory, 2 * page_size, watch_addresses,
                                  &watch_count, &granularity );
        ok( status == STATUS_ACCESS_VIOLATION,
            "protected write-watch reset returned %#lx\n", status );
        ok( VirtualProtect( cells + 0x6000, page_size, PAGE_READWRITE, &old_protect ),
            "write-watch output lane restore failed %lu\n", GetLastError() );
        watch_count = 2;
        status = NtGetWriteWatch( NtCurrentProcess(), 0, watch_memory, page_size,
                                  watch_addresses, &watch_count, &granularity );
        ok( !status, "post-failure write-watch query returned %#lx\n", status );
        for (i = 0; !status && i < watch_count; i++)
            if (watch_addresses[i] == watch_memory) found = TRUE;
        ok( found, "failed reset discarded the write watch\n" );
        watch_count = 2;
        status = NtGetWriteWatch( NtCurrentProcess(), WRITE_WATCH_FLAG_RESET,
                                  watch_memory, 2 * page_size, watch_addresses,
                                  &watch_count, &granularity );
        ok( !status, "valid write-watch reset returned %#lx\n", status );
        VirtualFree( watch_memory, 0, MEM_RELEASE );
    }

    machine_info = (WINE_PROCESS_VM_INFORMATION *)(cells + 0x7000);
    ok( VirtualProtect( cells + 0x7000, page_size, PAGE_READONLY, &old_protect ),
        "machine-info output protect failed %lu\n", GetLastError() );
    status = NtQueryVirtualMemory( NtCurrentProcess(), NULL,
                                   MemoryWineProcessVmMachineInformation,
                                   machine_info, sizeof(*machine_info), NULL );
    ok( status == STATUS_ACCESS_VIOLATION,
        "protected VM-machine output returned %#lx\n", status );
    ok( VirtualProtect( cells + 0x7000, page_size, PAGE_READWRITE, &old_protect ),
        "machine-info output restore failed %lu\n", GetLastError() );
    status = NtQueryVirtualMemory( NtCurrentProcess(), NULL,
                                   MemoryWineProcessVmMachineInformation,
                                   machine_info, sizeof(*machine_info), NULL );
    ok( !status && machine_info->Machine == IMAGE_FILE_MACHINE_I386,
        "valid VM-machine follow-up returned %#lx machine %#x\n",
        status, machine_info->Machine );

    {
        static const WCHAR missing_unixlib[] = L"__wine_missing_marshalling_test";
        UNICODE_STRING *unix_name = (UNICODE_STRING *)(
            cells + 0xc000 - sizeof(*unix_name) / 2 );
        UINT64 unix_result[2] = {0};
        SIZE_T unix_retlen = ~(SIZE_T)0;

        memcpy( cells + 0xd000, missing_unixlib, sizeof(missing_unixlib) );
        unix_name->Length = sizeof(missing_unixlib) - sizeof(WCHAR);
        unix_name->MaximumLength = sizeof(missing_unixlib);
        unix_name->Buffer = (WCHAR *)(cells + 0xd000);
        ok( VirtualProtect( cells + 0xc000, page_size, PAGE_NOACCESS, &old_protect ),
            "Unix-name header lane protect failed %lu\n", GetLastError() );
        status = NtQueryVirtualMemory( NtCurrentProcess(), unix_name,
                                       MemoryWineLoadUnixLibByName, unix_result,
                                       sizeof(unix_result), &unix_retlen );
        ok( status == STATUS_ACCESS_VIOLATION,
            "cross-page Unix-name header returned %#lx\n", status );
        ok( VirtualProtect( cells + 0xc000, page_size, PAGE_READWRITE, &old_protect ),
            "Unix-name header lane restore failed %lu\n", GetLastError() );
        ok( VirtualProtect( cells + 0xd000, page_size, PAGE_NOACCESS, &old_protect ),
            "Unix-name buffer lane protect failed %lu\n", GetLastError() );
        status = NtQueryVirtualMemory( NtCurrentProcess(), unix_name,
                                       MemoryWineLoadUnixLibByName, unix_result,
                                       sizeof(unix_result), &unix_retlen );
        ok( status == STATUS_ACCESS_VIOLATION,
            "protected Unix-name buffer returned %#lx\n", status );
        ok( VirtualProtect( cells + 0xd000, page_size, PAGE_READWRITE, &old_protect ),
            "Unix-name buffer lane restore failed %lu\n", GetLastError() );
        status = NtQueryVirtualMemory( NtCurrentProcess(), unix_name,
                                       MemoryWineLoadUnixLibByName, unix_result,
                                       sizeof(unix_result), &unix_retlen );
        ok( status == STATUS_DLL_NOT_FOUND,
            "valid missing Unix-name follow-up returned %#lx\n", status );
    }

    if (pNtSetInformationVirtualMemory)
    {
        ranges = (MEMORY_RANGE_ENTRY *)(cells + 0x8000 - sizeof(*ranges));
        ranges[0].VirtualAddress = target;
        ranges[0].NumberOfBytes = page_size;
        ranges[1].VirtualAddress = (BYTE *)target + page_size;
        ranges[1].NumberOfBytes = page_size;
        ok( VirtualProtect( cells + 0x8000, page_size, PAGE_NOACCESS, &old_protect ),
            "range input lane protect failed %lu\n", GetLastError() );
        status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                                 2, ranges, &prefetch_flags,
                                                 sizeof(prefetch_flags) );
        ok( status == STATUS_ACCESS_VIOLATION,
            "cross-page range input returned %#lx\n", status );
        ok( VirtualProtect( cells + 0x8000, page_size, PAGE_READWRITE, &old_protect ),
            "range input lane restore failed %lu\n", GetLastError() );
        status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                                 2, ranges, &prefetch_flags,
                                                 sizeof(prefetch_flags) );
        ok( !status, "valid range follow-up returned %#lx\n", status );
        status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                                 ~(ULONG_PTR)0, ranges, &prefetch_flags,
                                                 sizeof(prefetch_flags) );
        ok( status == STATUS_INTEGER_OVERFLOW,
            "range-count overflow returned %#lx\n", status );
    }

    if (pNtAllocateVirtualMemoryEx)
    {
        parameter = (MEM_EXTENDED_PARAMETER *)(cells + 0x9000);
        requirements = (MEM_ADDRESS_REQUIREMENTS *)(cells + 0xa000);
        memset( parameter, 0, sizeof(*parameter) );
        memset( requirements, 0, sizeof(*requirements) );
        parameter->Type = MemExtendedParameterAddressRequirements;
        parameter->Pointer = requirements;
        *address_cell = NULL;
        *size_cell = 0x10000;
        ok( VirtualProtect( cells + 0xa000, page_size, PAGE_NOACCESS, &old_protect ),
            "nested requirements lane protect failed %lu\n", GetLastError() );
        status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), address_cell, size_cell,
                                             MEM_RESERVE, PAGE_READWRITE, parameter, 1 );
        ok( status == STATUS_ACCESS_VIOLATION,
            "protected address requirements returned %#lx\n", status );
        ok( !*address_cell, "failed extended allocation changed address to %p\n",
            *address_cell );
        ok( VirtualProtect( cells + 0xa000, page_size, PAGE_READWRITE, &old_protect ),
            "nested requirements lane restore failed %lu\n", GetLastError() );
        status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), address_cell, size_cell,
                                             MEM_RESERVE, PAGE_READWRITE, parameter, 1 );
        ok( !status, "valid extended allocation follow-up returned %#lx\n", status );
        if (!status)
        {
            page = *address_cell;
            size = 0;
            NtFreeVirtualMemory( NtCurrentProcess(), &page, &size, MEM_RELEASE );
        }
        *address_cell = NULL;
        *size_cell = 0x10000;
        status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), address_cell, size_cell,
                                             MEM_RESERVE, PAGE_READWRITE, parameter, ~0u );
        ok( status == STATUS_INVALID_PARAMETER,
            "extended-parameter count overflow returned %#lx\n", status );
    }

    if (pNtCreateSectionEx)
    {
        HANDLE *handle_cell = (HANDLE *)address_cell;

        section_size.QuadPart = page_size;
        status = NtQueryInformationProcess( NtCurrentProcess(), ProcessHandleCount,
                                            &handles_before, sizeof(handles_before), NULL );
        have_handle_count = !status;
        ok( VirtualProtect( cells, page_size, PAGE_READONLY, &old_protect ),
            "section handle output protect failed %lu\n", GetLastError() );
        status = pNtCreateSectionEx( handle_cell, SECTION_ALL_ACCESS, NULL, &section_size,
                                     PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0 );
        ok( status == STATUS_ACCESS_VIOLATION,
            "protected section handle output returned %#lx\n", status );
        ok( VirtualProtect( cells, page_size, PAGE_READWRITE, &old_protect ),
            "section handle output restore failed %lu\n", GetLastError() );
        if (have_handle_count)
        {
            status = NtQueryInformationProcess( NtCurrentProcess(), ProcessHandleCount,
                                                &handles_after, sizeof(handles_after), NULL );
            ok( !status, "post-failure handle-count query returned %#lx\n", status );
            if (!status)
                ok( handles_after == handles_before,
                    "failed section publish leaked handles (%lu -> %lu)\n",
                    handles_before, handles_after );
        }
        status = pNtCreateSectionEx( handle_cell, SECTION_ALL_ACCESS, NULL, &section_size,
                                     PAGE_READWRITE, SEC_COMMIT, NULL, NULL, 0 );
        ok( !status, "valid section create follow-up returned %#lx\n", status );
        if (!status) NtClose( *handle_cell );
    }

    section_size.QuadPart = page_size;
    status = NtCreateSection( &section, SECTION_ALL_ACCESS, NULL, &section_size,
                              PAGE_READWRITE, SEC_COMMIT, NULL );
    ok( !status, "map test section creation failed %#lx\n", status );
    if (!status)
    {
        candidate = NULL;
        size = 0x10000;
        status = NtAllocateVirtualMemory( NtCurrentProcess(), &candidate, 0, &size,
                                          MEM_RESERVE, PAGE_NOACCESS );
        ok( !status, "map candidate reserve failed %#lx\n", status );
        if (!status)
        {
            view = candidate;
            size = 0;
            status = NtFreeVirtualMemory( NtCurrentProcess(), &view, &size, MEM_RELEASE );
            ok( !status, "map candidate release failed %#lx\n", status );
            *address_cell = candidate;
            *size_cell = page_size;
            ok( VirtualProtect( cells + 0x1000, page_size, PAGE_READONLY, &old_protect ),
                "map size-cell protect failed %lu\n", GetLastError() );
            status = NtMapViewOfSection( section, NtCurrentProcess(), address_cell, 0, 0,
                                         NULL, size_cell, ViewShare, 0, PAGE_READWRITE );
            ok( status == STATUS_ACCESS_VIOLATION,
                "protected map output returned %#lx\n", status );
            ok( VirtualProtect( cells + 0x1000, page_size, PAGE_READWRITE, &old_protect ),
                "map size-cell restore failed %lu\n", GetLastError() );
            *address_cell = candidate;
            *size_cell = page_size;
            status = NtMapViewOfSection( section, NtCurrentProcess(), address_cell, 0, 0,
                                         NULL, size_cell, ViewShare, 0, PAGE_READWRITE );
            ok( !status, "valid map follow-up returned %#lx\n", status );
            if (!status) NtUnmapViewOfSection( NtCurrentProcess(), *address_cell );
        }
        NtClose( section );
        section = NULL;
    }

done:
    if (restricted) NtClose( restricted );
    if (query_handle) NtClose( query_handle );
    if (section) NtClose( section );
    if (target)
    {
        page = target;
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &page, &size, MEM_RELEASE );
    }
    for (i = 0; i < 0xe; i++)
        VirtualProtect( cells + i * page_size, page_size, PAGE_READWRITE, &old_protect );
    VirtualFree( cells, 0, MEM_RELEASE );
}
#else
static void test_wow64_translated_guard_resolution(void)
{
}

static void test_wow64_translated_writewatch_resolution(void)
{
}

static void test_wow64_translated_copy_protection(void)
{
}

static void test_wow64_unowned_shadow_access(void)
{
}

static void test_wow64_virtual_guest_marshalling(void)
{
}
#endif

static void test_wow64_process_vm_machine_information(void)
{
#ifndef _WIN64
    static const ACCESS_MASK allowed_access[] =
    {
        PROCESS_VM_READ,
        PROCESS_VM_WRITE,
        PROCESS_VM_OPERATION,
        PROCESS_QUERY_INFORMATION,
        PROCESS_QUERY_LIMITED_INFORMATION,
        SYNCHRONIZE,
    };
    HANDLE process;
    WINE_PROCESS_VM_INFORMATION info;
    SIZE_T ret_len;
    NTSTATUS status;
    unsigned int i;

    if (!is_wow64 || !winetest_platform_is_wine) return;

    for (i = 0; i < ARRAY_SIZE(allowed_access); i++)
    {
        process = NULL;
        status = NtDuplicateObject( NtCurrentProcess(), NtCurrentProcess(), NtCurrentProcess(),
                                    &process, allowed_access[i], 0, 0 );
        ok( !status, "NtDuplicateObject(%#lx) failed %#lx\n", allowed_access[i], status );
        if (status) continue;

        memset( &info, 0, sizeof(info) );
        ret_len = ~(SIZE_T)0;
        status = NtQueryVirtualMemory( process, NULL, MemoryWineProcessVmMachineInformation,
                                       &info, sizeof(info), &ret_len );
        ok( !status, "VM machine query (%#lx) failed %#lx\n", allowed_access[i], status );
        ok( info.Version == WINE_PROCESS_VM_INFORMATION_VERSION, "got version %lu\n", info.Version );
        ok( info.Size == sizeof(info), "got structure size %lu\n", info.Size );
        ok( info.Machine != 0, "got machine %#x\n", info.Machine );
        ok( !(info.Flags & ~WINE_PROCESS_VM_FLAG_WOW64_TRANSLATED), "got flags %#x\n", info.Flags );
        ok( !info.Reserved, "got reserved %#lx\n", info.Reserved );
        ok( ret_len == sizeof(info), "got size %Iu\n", ret_len );
        NtClose( process );
    }

    memset( &info, 0, sizeof(info) );
    ret_len = ~(SIZE_T)0;
    status = NtQueryVirtualMemory( NtCurrentProcess(), (void *)(ULONG_PTR)~(ULONG)0,
                                   MemoryWineProcessVmMachineInformation,
                                   &info, sizeof(info), &ret_len );
    ok( !status, "maximum-address query failed %#lx\n", status );
    ok( info.Version == WINE_PROCESS_VM_INFORMATION_VERSION, "got version %lu\n", info.Version );
    ok( info.Size == sizeof(info), "got structure size %lu\n", info.Size );
    ok( info.Machine != 0, "got machine %#x\n", info.Machine );
    ok( !(info.Flags & ~WINE_PROCESS_VM_FLAG_WOW64_TRANSLATED), "got flags %#x\n", info.Flags );
    ok( !info.Reserved, "got reserved %#lx\n", info.Reserved );
    ok( ret_len == sizeof(info), "got size %Iu\n", ret_len );

    memset( &info, 0xdd, sizeof(info) );
    ret_len = 0xdeadbeef;
    status = NtQueryVirtualMemory( NtCurrentProcess(), NULL,
                                   MemoryWineProcessVmMachineInformation,
                                   &info, sizeof(info) - 1, &ret_len );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "short query returned %#lx\n", status );
    ok( info.Version == 0xdddddddd, "short query changed output to %#lx\n", info.Version );
    ok( ret_len == 0xdeadbeef, "short query changed size to %Iu\n", ret_len );

    status = NtQueryVirtualMemory( NtCurrentProcess(), NULL,
                                   MemoryWineProcessVmMachineInformation,
                                   &info, sizeof(info) + 1, &ret_len );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "long query returned %#lx\n", status );
    ok( info.Version == 0xdddddddd, "long query changed output to %#lx\n", info.Version );
    ok( ret_len == 0xdeadbeef, "long query changed size to %Iu\n", ret_len );

    status = NtQueryVirtualMemory( NtCurrentProcess(), NULL,
                                   MemoryWineProcessVmMachineInformation,
                                   NULL, sizeof(info), &ret_len );
    ok( status == STATUS_ACCESS_VIOLATION, "null-buffer query returned %#lx\n", status );
    ok( ret_len == 0xdeadbeef, "null-buffer query changed size to %Iu\n", ret_len );

    status = NtQueryVirtualMemory( (HANDLE)(ULONG_PTR)0xdeadbeef, NULL,
                                   MemoryWineProcessVmMachineInformation,
                                   &info, sizeof(info), &ret_len );
    ok( status == STATUS_INVALID_HANDLE, "invalid-handle query returned %#lx\n", status );
    status = NtQueryVirtualMemory( (HANDLE)(ULONG_PTR)0xdeadbeef, NULL,
                                   MemoryWineProcessVmMachineInformation,
                                   &info, sizeof(info) - 1, &ret_len );
    ok( status == STATUS_INFO_LENGTH_MISMATCH,
        "short invalid-handle query returned %#lx\n", status );
#endif
}

static void test_wow64_translated_view_contract(void)
{
#ifdef _WIN64
    static const ACCESS_MASK vm_access[] =
    {
        PROCESS_VM_READ,
        PROCESS_VM_WRITE,
        PROCESS_VM_OPERATION,
    };
    const ULONG_PTR shadow_start = WINE_LOW_VA_SHADOW_BASE;
    const ULONG_PTR shadow_end = WINE_LOW_VA_SHADOW_BASE + WINE_LOW_VA_SHADOW_SIZE;
    const ULONG_PTR test_margin = 0x1000000;
    MEM_EXTENDED_PARAMETER translated_params[2] = {0};
    MEM_EXTENDED_PARAMETER ext = {0};
    MEM_ADDRESS_REQUIREMENTS requirements = {0};
    MEMORY_BASIC_INFORMATION info;
    ULONG translated = 2;
    ULONG_PTR shared_data = (ULONG_PTR)NtCurrentTeb()->Peb->SharedData;
    LARGE_INTEGER section_size;
    HANDLE process;
    HANDLE section;
    void *address, *page, *ordinary, *translated_view;
    SIZE_T size, ret_len = 0;
    NTSTATUS status;
    ULONG old_protect;
    WINE_PROCESS_VM_INFORMATION vm_info;
    unsigned int i;
    BOOL ordinary_reserved, translated_reserved;

    if (!is_wow64) return;
    if (shared_data < WINE_LOW_VA_SHADOW_BASE ||
        shared_data - WINE_LOW_VA_SHADOW_BASE >= WINE_LOW_VA_SHADOW_SIZE)
        return;

    for (i = 0; i < ARRAY_SIZE(vm_access); i++)
    {
        process = NULL;
        status = NtDuplicateObject( NtCurrentProcess(), NtCurrentProcess(), NtCurrentProcess(),
                                    &process, vm_access[i], 0, 0 );
        ok( !status, "NtDuplicateObject(%#lx) failed %#lx\n", vm_access[i], status );
        if (!status)
        {
            memset( &vm_info, 0, sizeof(vm_info) );
            ret_len = 0;
            status = NtQueryVirtualMemory( process, NULL, MemoryWineProcessVmMachineInformation,
                                           &vm_info, sizeof(vm_info), &ret_len );
            ok( !status, "VM machine query (%#lx) failed %#lx\n", vm_access[i], status );
            ok( vm_info.Version == WINE_PROCESS_VM_INFORMATION_VERSION,
                "got version %lu\n", vm_info.Version );
            ok( vm_info.Size == sizeof(vm_info), "got structure size %lu\n", vm_info.Size );
            ok( vm_info.Machine == IMAGE_FILE_MACHINE_I386, "got machine %#x\n", vm_info.Machine );
            ok( vm_info.Flags == WINE_PROCESS_VM_FLAG_WOW64_TRANSLATED,
                "got flags %#x\n", vm_info.Flags );
            ok( !vm_info.Reserved, "got reserved %#lx\n", vm_info.Reserved );
            ok( ret_len == sizeof(vm_info), "got size %Iu\n", ret_len );
            NtClose( process );
        }
    }

    process = NULL;
    status = NtDuplicateObject( NtCurrentProcess(), NtCurrentProcess(), NtCurrentProcess(),
                                &process, SYNCHRONIZE, 0, 0 );
    ok( !status, "NtDuplicateObject(SYNCHRONIZE) failed %#lx\n", status );
    if (!status)
    {
        status = NtQueryVirtualMemory( process, NULL, MemoryWineProcessVmMachineInformation,
                                       &vm_info, sizeof(vm_info), NULL );
        ok( status == STATUS_ACCESS_DENIED, "no-VM-rights query returned %#lx\n", status );
        NtClose( process );
    }
    status = NtQueryVirtualMemory( (HANDLE)(ULONG_PTR)0xdeadbeef, NULL,
                                   MemoryWineProcessVmMachineInformation,
                                   &vm_info, sizeof(vm_info), NULL );
    ok( status == STATUS_INVALID_HANDLE, "invalid-handle query returned %#lx\n", status );

    status = NtQueryVirtualMemory( NtCurrentProcess(), (void *)shared_data,
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated) - 1, NULL );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "got %#lx\n", status );
    status = NtQueryVirtualMemory( 0, (void *)shared_data,
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), NULL );
    ok( status == STATUS_INVALID_HANDLE, "got %#lx\n", status );

    translated = 0;
    status = NtQueryVirtualMemory( NtCurrentProcess(), (void *)shared_data,
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), &ret_len );
    ok( !status, "got %#lx\n", status );
    ok( translated == 1, "got %lu\n", translated );
    ok( ret_len == sizeof(translated), "got %Iu\n", ret_len );

    translated = 1;
    status = NtQueryVirtualMemory( NtCurrentProcess(), test_wow64_translated_view_contract,
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), NULL );
    ok( !status, "got %#lx\n", status );
    ok( translated == 0, "got %lu\n", translated );

    address = (void *)(WINE_LOW_VA_SHADOW_BASE + 0x50000000);
    size = 0x10000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &size,
                                      MEM_RESERVE, PAGE_READWRITE );
    ok( status == STATUS_CONFLICTING_ADDRESSES, "got %#lx, address %p\n", status, address );

    address = NULL;
    size = 0x10000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &size,
                                      MEM_RESERVE, PAGE_READWRITE );
    ok( !status, "got %#lx\n", status );
    ok( (ULONG_PTR)address < WINE_LOW_VA_SHADOW_BASE ||
        (ULONG_PTR)address - WINE_LOW_VA_SHADOW_BASE >= WINE_LOW_VA_SHADOW_SIZE,
        "ordinary allocation entered the translated shadow at %p\n", address );
    if (!status)
    {
        size = 0;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
        ok( !status, "got %#lx\n", status );
    }

    if (!pNtAllocateVirtualMemoryEx)
    {
        win_skip( "NtAllocateVirtualMemoryEx() is missing\n" );
        return;
    }

    ext.Type = MemExtendedParameterAddressRequirements;
    ext.Pointer = &requirements;
    requirements.LowestStartingAddress = (void *)(shadow_start - test_margin);
    requirements.HighestEndingAddress = (void *)(shadow_end + test_margin - 1);

    address = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &address, &size, MEM_RESERVE,
                                         PAGE_READWRITE, &ext, 1 );
    ok( !status, "bottom-up allocation failed, status %#lx\n", status );
    if (!status)
    {
        ok( (ULONG_PTR)address >= shadow_start - test_margin &&
            (ULONG_PTR)address + size <= shadow_start,
            "bottom-up allocation did not use the range below the shadow: %p size %Iu\n",
            address, size );
        size = 0;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
        ok( !status, "got %#lx\n", status );
    }

    address = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &address, &size,
                                         MEM_RESERVE | MEM_TOP_DOWN,
                                         PAGE_READWRITE, &ext, 1 );
    ok( !status, "top-down allocation failed, status %#lx\n", status );
    if (!status)
    {
        ok( (ULONG_PTR)address >= shadow_end &&
            (ULONG_PTR)address + size - 1 <= shadow_end + test_margin - 1,
            "top-down allocation did not use the range above the shadow: %p size %Iu\n",
            address, size );
        size = 0;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
        ok( !status, "got %#lx\n", status );
    }

    /* Placeholder coalescing must not erase the type-31 ownership boundary,
     * regardless of which side of the requested range is translated. */
    ordinary_reserved = translated_reserved = FALSE;
    ordinary = (void *)(shadow_start - 0x10000);
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &ordinary, &size,
                                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                         PAGE_NOACCESS, NULL, 0 );
    ok( !status, "lower ordinary placeholder reserve failed %#lx\n", status );
    if (!status) ordinary_reserved = TRUE;
    translated_view = (void *)shadow_start;
    requirements.LowestStartingAddress = translated_view;
    requirements.HighestEndingAddress = (BYTE *)translated_view + 0xffff;
    translated_params[0].Type = MemExtendedParameterAddressRequirements;
    translated_params[0].Pointer = &requirements;
    translated_params[1].Type = WINE_MEM_EXTENDED_PARAMETER_WOW64_TRANSLATED;
    size = 0x10000;
    if (!status)
    {
        status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &translated_view, &size,
                                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                             PAGE_NOACCESS, translated_params,
                                             ARRAY_SIZE(translated_params) );
        ok( !status, "lower translated placeholder reserve failed %#lx\n", status );
        if (!status) translated_reserved = TRUE;
    }
    if (!status)
    {
        address = ordinary;
        size = 0x20000;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size,
                                      MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS );
        ok( status == STATUS_CONFLICTING_ADDRESSES,
            "ordinary-to-translated coalesce returned %#lx\n", status );
        translated = 0;
        status = NtQueryVirtualMemory( NtCurrentProcess(), translated_view,
                                       MemoryWineWow64TranslatedInformation,
                                       &translated, sizeof(translated), NULL );
        ok( !status && translated == 1,
            "lower translated placeholder changed, status %#lx translated %lu\n",
            status, translated );
    }
    if (translated_reserved)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &translated_view, &size, MEM_RELEASE );
    }
    if (ordinary_reserved)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &ordinary, &size, MEM_RELEASE );
    }

    ordinary_reserved = translated_reserved = FALSE;
    translated_view = (void *)(shadow_end - 0x10000);
    requirements.LowestStartingAddress = translated_view;
    requirements.HighestEndingAddress = (BYTE *)translated_view + 0xffff;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &translated_view, &size,
                                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                         PAGE_NOACCESS, translated_params,
                                         ARRAY_SIZE(translated_params) );
    ok( !status, "upper translated placeholder reserve failed %#lx\n", status );
    if (!status) translated_reserved = TRUE;
    ordinary = (void *)shadow_end;
    size = 0x10000;
    if (!status)
    {
        status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &ordinary, &size,
                                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                             PAGE_NOACCESS, NULL, 0 );
        ok( !status, "upper ordinary placeholder reserve failed %#lx\n", status );
        if (!status) ordinary_reserved = TRUE;
    }
    if (!status)
    {
        address = translated_view;
        size = 0x20000;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size,
                                      MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS );
        ok( status == STATUS_CONFLICTING_ADDRESSES,
            "translated-to-ordinary coalesce returned %#lx\n", status );
        translated = 0;
        status = NtQueryVirtualMemory( NtCurrentProcess(), translated_view,
                                       MemoryWineWow64TranslatedInformation,
                                       &translated, sizeof(translated), NULL );
        ok( !status && translated == 1,
            "upper translated placeholder changed, status %#lx translated %lu\n",
            status, translated );
    }
    if (ordinary_reserved)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &ordinary, &size, MEM_RELEASE );
    }
    if (translated_reserved)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &translated_view, &size, MEM_RELEASE );
    }

    /* Exercise every translated mutation through the registered provider.  The
     * observer is intentionally not replaceable by a late test double: these
     * state checks cover the real producer/consumer path and the provider's
     * native harness validates the exact event snapshots. */
    requirements.LowestStartingAddress = (void *)(shadow_start + 0x60000000);
    requirements.HighestEndingAddress = (void *)(shadow_start + 0x6fffffff);
    translated_params[0].Type = MemExtendedParameterAddressRequirements;
    translated_params[0].Pointer = &requirements;
    translated_params[1].Type = WINE_MEM_EXTENDED_PARAMETER_WOW64_TRANSLATED;

    address = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &address, &size, MEM_RESERVE,
                                         PAGE_READWRITE, translated_params,
                                         ARRAY_SIZE(translated_params) );
    ok( !status, "translated reserve failed %#lx\n", status );
    if (status) return;

    translated = 0;
    status = NtQueryVirtualMemory( NtCurrentProcess(), address,
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), NULL );
    ok( !status && translated == 1, "translated query returned %#lx, %lu\n",
        status, translated );
    status = NtQueryVirtualMemory( NtCurrentProcess(), address, MemoryBasicInformation,
                                   &info, sizeof(info), NULL );
    ok( !status && info.State == MEM_RESERVE,
        "reserve query returned %#lx, state %#lx\n", status, info.State );

    page = (char *)address + 0x4000;
    size = 3 * page_size;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &page, 0, &size,
                                      MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "translated commit failed %#lx\n", status );
    if (!status)
    {
        memset( page, 0x5a, size );
        ok( *(BYTE *)page == 0x5a && *((BYTE *)page + size - 1) == 0x5a,
            "translated committed memory is not writable\n" );
    }

    page = (char *)address + 0x5000;
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &size,
                                     PAGE_READONLY, &old_protect );
    ok( !status && old_protect == PAGE_READWRITE,
        "translated protect returned %#lx, old protection %#lx\n", status, old_protect );
    status = NtQueryVirtualMemory( NtCurrentProcess(), page, MemoryBasicInformation,
                                   &info, sizeof(info), NULL );
    ok( !status && info.State == MEM_COMMIT && info.Protect == PAGE_READONLY,
        "protect query returned %#lx, state %#lx protection %#lx\n",
        status, info.State, info.Protect );

    page = (char *)address + 0x6000;
    size = page_size;
    status = NtFreeVirtualMemory( NtCurrentProcess(), &page, &size, MEM_DECOMMIT );
    ok( !status, "translated decommit failed %#lx\n", status );
    status = NtQueryVirtualMemory( NtCurrentProcess(), page, MemoryBasicInformation,
                                   &info, sizeof(info), NULL );
    ok( !status && info.State == MEM_RESERVE,
        "decommit query returned %#lx, state %#lx\n", status, info.State );

    page = (char *)address + 0x6000;
    size = page_size;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &page, 0, &size,
                                      MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "translated recommit failed %#lx\n", status );
    if (!status)
    {
        *(BYTE *)page = 0xa5;
        ok( *(BYTE *)page == 0xa5, "translated recommitted memory is not writable\n" );
    }

    /* A failed mutation must complete and release the provider gate before the
     * immediately following successful mutation. */
    page = (char *)address + 0x7000;
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &size,
                                     PAGE_READONLY, &old_protect );
    ok( status == STATUS_NOT_COMMITTED, "reserved-page protect returned %#lx\n", status );
    page = (char *)address + 0x4000;
    size = page_size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &size,
                                     PAGE_READONLY, &old_protect );
    ok( !status, "post-failure translated protect failed %#lx\n", status );

    page = address;
    size = 0;
    status = NtFreeVirtualMemory( NtCurrentProcess(), &page, &size, MEM_RELEASE );
    ok( !status, "translated release failed %#lx\n", status );
    translated = 1;
    status = NtQueryVirtualMemory( NtCurrentProcess(), address,
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), NULL );
    ok( status == STATUS_NOT_MAPPED_VIEW,
        "released translated query returned %#lx, %lu\n", status, translated );

    if (!pNtMapViewOfSectionEx)
    {
        win_skip( "NtMapViewOfSectionEx() is missing\n" );
        return;
    }
    section_size.QuadPart = 0x10000;
    status = NtCreateSection( &section, SECTION_ALL_ACCESS, NULL, &section_size,
                              PAGE_READWRITE, SEC_COMMIT, NULL );
    ok( !status, "NtCreateSection failed %#lx\n", status );
    if (status) return;
    address = NULL;
    size = 0;
    status = pNtMapViewOfSectionEx( section, NtCurrentProcess(), &address, NULL, &size,
                                    0, PAGE_READWRITE, translated_params,
                                    ARRAY_SIZE(translated_params) );
    ok( !status, "translated section map failed %#lx\n", status );
    if (!status)
    {
        translated = 0;
        status = NtQueryVirtualMemory( NtCurrentProcess(), address,
                                       MemoryWineWow64TranslatedInformation,
                                       &translated, sizeof(translated), NULL );
        ok( !status && translated == 1, "mapped translated query returned %#lx, %lu\n",
            status, translated );
        *(BYTE *)address = 0x3c;
        ok( *(BYTE *)address == 0x3c, "translated section memory is not writable\n" );
        status = NtUnmapViewOfSection( NtCurrentProcess(), address );
        ok( !status, "translated section unmap failed %#lx\n", status );
        translated = 1;
        status = NtQueryVirtualMemory( NtCurrentProcess(), address,
                                       MemoryWineWow64TranslatedInformation,
                                       &translated, sizeof(translated), NULL );
        ok( status == STATUS_NOT_MAPPED_VIEW,
            "unmapped translated query returned %#lx, %lu\n", status, translated );
    }

    /* A legacy section replacement inherits ownership from a type-31
     * placeholder.  First force a size mismatch: its transaction must still
     * complete and leave the placeholder intact before the successful retry. */
    address = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &address, &size,
                                         MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                         PAGE_NOACCESS, translated_params,
                                         ARRAY_SIZE(translated_params) );
    ok( !status, "translated placeholder reserve failed %#lx\n", status );
    if (!status)
    {
        page = address;
        size = page_size;
        status = pNtMapViewOfSectionEx( section, NtCurrentProcess(), &page, NULL, &size,
                                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0 );
        ok( status == STATUS_CONFLICTING_ADDRESSES,
            "mismatched placeholder replacement returned %#lx\n", status );

        page = address;
        size = 0x10000;
        status = pNtMapViewOfSectionEx( section, NtCurrentProcess(), &page, NULL, &size,
                                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0 );
        ok( !status, "legacy placeholder replacement failed %#lx\n", status );
        if (!status)
        {
            translated = 0;
            status = NtQueryVirtualMemory( NtCurrentProcess(), page,
                                           MemoryWineWow64TranslatedInformation,
                                           &translated, sizeof(translated), NULL );
            ok( !status && translated == 1,
                "replacement translated query returned %#lx, %lu\n", status, translated );
            *(BYTE *)page = 0x7b;
            ok( *(BYTE *)page == 0x7b, "replacement mapping is not writable\n" );
            status = NtUnmapViewOfSection( NtCurrentProcess(), page );
            ok( !status, "replacement section unmap failed %#lx\n", status );
        }
        else
        {
            page = address;
            size = 0;
            status = NtFreeVirtualMemory( NtCurrentProcess(), &page, &size, MEM_RELEASE );
            ok( !status, "translated placeholder release failed %#lx\n", status );
        }
    }
    NtClose( section );

    /* Remote calls execute these mutations in the APC target process; using
     * the translated reserve followed by legacy commit/protect/free exercises
     * both the private transport flag and tag-derived ownership in the target. */
    process = create_target_process( "sleep" );
    ok( !!process, "failed to create remote translated-memory target\n" );
    if (!process) return;
    address = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( process, &address, &size, MEM_RESERVE,
                                         PAGE_READWRITE, translated_params,
                                         ARRAY_SIZE(translated_params) );
    ok( !status, "remote translated reserve failed %#lx\n", status );
    if (!status)
    {
        ok( (ULONG_PTR)address >= shadow_start && (ULONG_PTR)address + size <= shadow_end,
            "remote translated reserve escaped shadow: %p size %Iu\n", address, size );
        page = (char *)address + 0x4000;
        size = page_size;
        status = NtAllocateVirtualMemory( process, &page, 0, &size,
                                          MEM_COMMIT, PAGE_READWRITE );
        ok( !status, "remote translated commit failed %#lx\n", status );
        status = NtQueryVirtualMemory( process, page, MemoryBasicInformation,
                                       &info, sizeof(info), NULL );
        ok( !status && info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE,
            "remote commit query returned %#lx, state %#lx protection %#lx\n",
            status, info.State, info.Protect );
        size = page_size;
        status = NtProtectVirtualMemory( process, &page, &size,
                                         PAGE_READONLY, &old_protect );
        ok( !status, "remote translated protect failed %#lx\n", status );
        size = page_size;
        status = NtFreeVirtualMemory( process, &page, &size, MEM_DECOMMIT );
        ok( !status, "remote translated decommit failed %#lx\n", status );
        page = address;
        size = 0;
        status = NtFreeVirtualMemory( process, &page, &size, MEM_RELEASE );
        ok( !status, "remote translated release failed %#lx\n", status );
    }
    NtTerminateProcess( process, 0 );
    NtWaitForSingleObject( process, FALSE, NULL );
    NtClose( process );
#endif
}

static inline void *get_rva( HMODULE module, DWORD va )
{
    return (void *)((char *)module + va);
}

static HANDLE create_target_process(const char *arg)
{
    char **argv;
    char cmdline[MAX_PATH];
    PROCESS_INFORMATION pi;
    BOOL ret;
    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);

    winetest_get_mainargs(&argv);
    sprintf(cmdline, "%s %s %s", argv[0], argv[1], arg);
    ret = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    ok(ret, "error: %lu\n", GetLastError());
    ret = CloseHandle(pi.hThread);
    ok(ret, "error %lu\n", GetLastError());
    return pi.hProcess;
}

static UINT_PTR get_zero_bits(UINT_PTR p)
{
    UINT_PTR z = 0;

#ifdef _WIN64
    if (p >= 0xffffffff)
        return (~(UINT_PTR)0) >> get_zero_bits(p >> 32);
#endif

    if (p == 0) return 0;
    while ((p >> (31 - z)) != 1) z++;
    return z;
}

static UINT_PTR get_zero_bits_mask(ULONG_PTR z)
{
    if (z >= 32)
    {
        z = get_zero_bits(z);
#ifdef _WIN64
        if (z >= 32) return z;
#endif
    }
    return (~(UINT32)0) >> z;
}

static void test_NtAllocateVirtualMemory(void)
{
    MEMORY_BASIC_INFORMATION info;
    void *addr1, *addr2, *allocation_base, *protect_base;
    NTSTATUS status;
    SIZE_T query_size, size;
    ULONG old_protect;
    ULONG_PTR zero_bits;

    /* simple allocation should success */
    size = 0x1000;
    addr1 = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr1, 0, &size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_SUCCESS, "NtAllocateVirtualMemory returned %08lx\n", status);

    /* logical executable protection must survive a separate reserve and commit */
    size = 0x10000;
    addr2 = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, 0, &size,
                                     MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_SUCCESS, "executable reserve returned %08lx\n", status);
    if (status == STATUS_SUCCESS)
    {
        allocation_base = addr2;
        query_size = 0;
        status = NtQueryVirtualMemory( NtCurrentProcess(), addr2,
                                       MemoryBasicInformation, &info,
                                       sizeof(info), &query_size );
        ok(status == STATUS_SUCCESS, "executable reserve query returned %08lx\n", status);
        if (status == STATUS_SUCCESS)
        {
            ok(info.BaseAddress == allocation_base, "executable reserve base is %p\n",
               info.BaseAddress);
            ok(info.AllocationBase == addr2, "executable reserve allocation base is %p\n",
               info.AllocationBase);
            ok(info.AllocationProtect == PAGE_EXECUTE_READWRITE,
               "executable reserve allocation protection is %#lx\n", info.AllocationProtect);
            ok(info.RegionSize == 0x10000, "executable reserve region size is %Ix\n",
               info.RegionSize);
            ok(info.State == MEM_RESERVE, "executable reserve state is %#lx\n", info.State);
            ok(!info.Protect, "executable reserve protection is %#lx\n", info.Protect);
            ok(info.Type == MEM_PRIVATE, "executable reserve type is %#lx\n", info.Type);
            ok(query_size == sizeof(info), "executable reserve query size is %Ix\n", query_size);
        }

        size = 0x10000;
        status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, 0, &size,
                                         MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        ok(status == STATUS_SUCCESS, "executable commit returned %08lx\n", status);
        if (status == STATUS_SUCCESS)
        {
            ok(addr2 == allocation_base, "executable commit base is %p\n", addr2);
            ok(size == 0x10000, "executable commit size is %Ix\n", size);
            query_size = 0;
            status = NtQueryVirtualMemory( NtCurrentProcess(), addr2,
                                           MemoryBasicInformation, &info,
                                           sizeof(info), &query_size );
            ok(status == STATUS_SUCCESS, "executable query returned %08lx\n", status);
            if (status == STATUS_SUCCESS)
            {
                ok(info.BaseAddress == allocation_base, "executable allocation base address is %p\n",
                   info.BaseAddress);
                ok(info.AllocationBase == addr2, "executable allocation base is %p\n",
                   info.AllocationBase);
                ok(info.AllocationProtect == PAGE_EXECUTE_READWRITE,
                   "executable allocation protection is %#lx\n", info.AllocationProtect);
                ok(info.RegionSize == 0x10000, "executable allocation region size is %Ix\n",
                   info.RegionSize);
                ok(info.State == MEM_COMMIT, "executable allocation state is %#lx\n", info.State);
                ok(info.Protect == PAGE_EXECUTE_READWRITE,
                   "executable allocation protection is %#lx\n", info.Protect);
                ok(info.Type == MEM_PRIVATE, "executable allocation type is %#lx\n", info.Type);
                ok(query_size == sizeof(info), "executable allocation query size is %Ix\n",
                   query_size);
            }
            *(volatile BYTE *)addr2 = 0x5a;
            ok(*(volatile BYTE *)addr2 == 0x5a,
               "executable allocation is not readable and writable\n");

            /* On hosts with larger physical pages this currently exposes an RX+RWX
             * protection union.  The transition is required to succeed on Windows;
             * if Wine cannot represent it, the failed change must be atomic. */
            protect_base = addr2;
            size = 0x1000;
            old_protect = 0xdeadbeef;
            status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_base, &size,
                                             PAGE_EXECUTE_READ, &old_protect );
            todo_wine_if(status == STATUS_ACCESS_DENIED)
                ok(status == STATUS_SUCCESS, "executable read protect returned %08lx\n", status);
            if (status == STATUS_SUCCESS)
            {
                ok(old_protect == PAGE_EXECUTE_READWRITE,
                   "executable read old protection is %#lx\n", old_protect);
                query_size = 0;
                status = NtQueryVirtualMemory( NtCurrentProcess(), addr2,
                                               MemoryBasicInformation, &info,
                                               sizeof(info), &query_size );
                ok(status == STATUS_SUCCESS, "executable read query returned %08lx\n", status);
                if (status == STATUS_SUCCESS)
                {
                    ok(info.State == MEM_COMMIT, "executable read state is %#lx\n", info.State);
                    ok(info.Protect == PAGE_EXECUTE_READ,
                       "executable read protection is %#lx\n", info.Protect);
                }
            }
            else if (status == STATUS_ACCESS_DENIED)
            {
                query_size = 0;
                status = NtQueryVirtualMemory( NtCurrentProcess(), addr2,
                                               MemoryBasicInformation, &info,
                                               sizeof(info), &query_size );
                ok(status == STATUS_SUCCESS, "failed protect query returned %08lx\n", status);
                if (status == STATUS_SUCCESS)
                {
                    ok(info.State == MEM_COMMIT, "failed protect state is %#lx\n", info.State);
                    ok(info.Protect == PAGE_EXECUTE_READWRITE,
                       "failed protect changed protection to %#lx\n", info.Protect);
                }
                *(volatile BYTE *)addr2 = 0xa5;
                ok(*(volatile BYTE *)addr2 == 0xa5,
                   "failed protect did not restore readable and writable access\n");
            }
        }

        size = 0;
        status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_RELEASE);
        ok(status == STATUS_SUCCESS, "executable allocation release returned %08lx\n", status);
    }

    /* allocation conflicts because of 64k align */
    size = 0x1000;
    addr2 = (char *)addr1 + 0x1000;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, 0, &size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "NtAllocateVirtualMemory returned %08lx\n", status);

    /* it should conflict, even when zero_bits is explicitly set */
    size = 0x1000;
    addr2 = (char *)addr1 + 0x1000;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, 12, &size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "NtAllocateVirtualMemory returned %08lx\n", status);

    /* 1 zero bits should zero 63-31 upper bits */
    size = 0x1000;
    addr2 = NULL;
    zero_bits = 1;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, zero_bits, &size,
                                     MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                                     PAGE_READWRITE);
    ok(status == STATUS_SUCCESS || status == STATUS_NO_MEMORY ||
       broken(status == STATUS_INVALID_PARAMETER_3) /* winxp */,
       "NtAllocateVirtualMemory returned %08lx\n", status);
    if (status == STATUS_SUCCESS)
    {
        ok(((UINT_PTR)addr2 >> (32 - zero_bits)) == 0,
           "NtAllocateVirtualMemory returned address: %p\n", addr2);

        size = 0;
        status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_RELEASE);
        ok(status == STATUS_SUCCESS, "NtFreeVirtualMemory return %08lx, addr2: %p\n", status, addr2);
    }

    for (zero_bits = 2; zero_bits <= 20; zero_bits++)
    {
        size = 0x1000;
        addr2 = NULL;
        status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, zero_bits, &size,
                                         MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                                         PAGE_READWRITE);
        ok(status == STATUS_SUCCESS || status == STATUS_NO_MEMORY ||
           broken(zero_bits == 20 && status == STATUS_CONFLICTING_ADDRESSES) /* w1064v1809 */,
           "NtAllocateVirtualMemory with %d zero_bits returned %08lx\n", (int)zero_bits, status);
        if (status == STATUS_SUCCESS)
        {
            ok(((UINT_PTR)addr2 >> (32 - zero_bits)) == 0,
               "NtAllocateVirtualMemory with %d zero_bits returned address %p\n", (int)zero_bits, addr2);

            size = 0;
            status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_RELEASE);
            ok(status == STATUS_SUCCESS, "NtFreeVirtualMemory return %08lx, addr2: %p\n", status, addr2);
        }
    }

    /* 21 zero bits never succeeds */
    size = 0x1000;
    addr2 = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, 21, &size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_NO_MEMORY || status == STATUS_INVALID_PARAMETER,
       "NtAllocateVirtualMemory returned %08lx\n", status);
    if (status == STATUS_SUCCESS)
    {
        size = 0;
        status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_RELEASE);
        ok(status == STATUS_SUCCESS, "NtFreeVirtualMemory return %08lx, addr2: %p\n", status, addr2);
    }

    /* 22 zero bits is invalid */
    size = 0x1000;
    addr2 = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, 22, &size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_INVALID_PARAMETER_3 || status == STATUS_INVALID_PARAMETER,
       "NtAllocateVirtualMemory returned %08lx\n", status);

    /* zero bits > 31 should be considered as a leading zeroes bitmask on 64bit and WoW64 */
    size = 0x1000;
    addr2 = NULL;
    zero_bits = 0x1aaaaaaa;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, zero_bits, &size,
                                      MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                                      PAGE_READWRITE);

    if (!is_win64 && !is_wow64)
    {
        ok(status == STATUS_INVALID_PARAMETER_3, "NtAllocateVirtualMemory returned %08lx\n", status);
    }
    else
    {
        ok(status == STATUS_SUCCESS || status == STATUS_NO_MEMORY,
           "NtAllocateVirtualMemory returned %08lx\n", status);
        if (status == STATUS_SUCCESS)
        {
            ok(((UINT_PTR)addr2 & ~get_zero_bits_mask(zero_bits)) == 0 &&
               ((UINT_PTR)addr2 & ~zero_bits) != 0, /* only the leading zeroes matter */
               "NtAllocateVirtualMemory returned address %p\n", addr2);

            size = 0;
            status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_RELEASE);
            ok(status == STATUS_SUCCESS, "NtFreeVirtualMemory return %08lx, addr2: %p\n", status, addr2);
        }
    }

    /* AT_ROUND_TO_PAGE flag is not supported for NtAllocateVirtualMemory */
    size = 0x1000;
    addr2 = (char *)addr1 + 0x1000;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr2, 0, &size,
                                     MEM_RESERVE | MEM_COMMIT | AT_ROUND_TO_PAGE, PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_INVALID_PARAMETER_5 || status == STATUS_INVALID_PARAMETER,
       "NtAllocateVirtualMemory returned %08lx\n", status);

    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr1, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "NtFreeVirtualMemory failed\n");

    /* NtFreeVirtualMemory tests */

    size = 0x10000;
    addr1 = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr1, 0, &size,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_SUCCESS, "NtAllocateVirtualMemory returned %08lx\n", status);

    size = 2;
    addr2 = (char *)addr1 + 0x1fff;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_DECOMMIT);
    ok(status == STATUS_SUCCESS, "NtFreeVirtualMemory failed %lx\n", status);
    ok( size == 0x2000, "wrong size %Ix\n", size );
    ok( addr2 == (char *)addr1 + 0x1000, "wrong addr %p\n", addr2 );

    size = 0;
    addr2 = (char *)addr1 + 0x1001;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_DECOMMIT);
    ok(status == STATUS_FREE_VM_NOT_AT_BASE, "NtFreeVirtualMemory failed %lx\n", status);
    ok( size == 0, "wrong size %Ix\n", size );
    ok( addr2 == (char *)addr1 + 0x1001, "wrong addr %p\n", addr2 );

    size = 0;
    addr2 = (char *)addr1 + 0xffe;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_DECOMMIT);
    ok(status == STATUS_SUCCESS, "NtFreeVirtualMemory failed %lx\n", status);
    ok( size == 0 || broken(size == 0x10000) /* <= win10 1709 */, "wrong size %Ix\n", size );
    ok( addr2 == addr1, "wrong addr %p\n", addr2 );

    size = 0;
    addr2 = (char *)addr1 + 0x1001;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_RELEASE);
    ok(status == STATUS_FREE_VM_NOT_AT_BASE, "NtFreeVirtualMemory failed %lx\n", status);
    ok( size == 0, "wrong size %Ix\n", size );
    ok( addr2 == (char *)addr1 + 0x1001, "wrong addr %p\n", addr2 );

    size = 0;
    addr2 = (char *)addr1 + 0xfff;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr2, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "NtFreeVirtualMemory failed %lx\n", status);
    ok( size == 0x10000, "wrong size %Ix\n", size );
    ok( addr2 == addr1, "wrong addr %p\n", addr2 );

    /* Placeholder functionality */
    size = 0x10000;
    addr1 = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr1, 0, &size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS);
    ok(!!status, "Unexpected status %08lx.\n", status);
}

#define check_region_size(p, s) check_region_size_(p, s, __LINE__)
static void check_region_size_(void *p, SIZE_T s, unsigned int line)
{
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS status;
    SIZE_T size;

    memset(&mbi, 0, sizeof(mbi));
    status = NtQueryVirtualMemory( NtCurrentProcess(), p, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok_(__FILE__,line)( !status, "Unexpected return value %08lx\n", status );
    ok_(__FILE__,line)( size == sizeof(mbi), "Unexpected return value.\n");
    ok_(__FILE__,line)( mbi.RegionSize == s, "Unexpected size %Iu, expected %Iu.\n", mbi.RegionSize, s);
}

static void test_NtAllocateVirtualMemoryEx(void)
{
    MEMORY_REGION_INFORMATION mri;
    MEMORY_BASIC_INFORMATION mbi;
    MEM_EXTENDED_PARAMETER ext[2];
    char *p, *p1, *p2, *p3;
    void *addresses[16];
    SIZE_T size, size2;
    ULONG granularity;
    NTSTATUS status;
    ULONG_PTR count;
    void *addr1;

    if (!pNtAllocateVirtualMemoryEx)
    {
        win_skip("NtAllocateVirtualMemoryEx() is missing\n");
        return;
    }

    size = 0x1000;
    addr1 = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE, NULL, 0);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr1, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    /* specifying a count of >0 with NULL parameters should fail */
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE, NULL, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    /* NULL process handle */
    size = 0x1000;
    addr1 = NULL;
    status = pNtAllocateVirtualMemoryEx(NULL, &addr1, &size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, NULL, 0);
    ok(status == STATUS_INVALID_HANDLE, "Unexpected status %08lx.\n", status);

    /* Placeholder functionality */
    size = 0x10000;
    addr1 = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr1, 0, &size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS, NULL, 0);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE, NULL, 0);
    ok(!status, "Unexpected status %08lx.\n", status);

    memset(addr1, 0xcc, size);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&addr1, &size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(!status, "Unexpected status %08lx.\n", status);

    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
            PAGE_READONLY, NULL, 0);
    ok(!status, "Unexpected status %08lx.\n", status);

    ok(!*(unsigned int *)addr1, "Got %#x.\n", *(unsigned int *)addr1);

    status = NtQueryVirtualMemory( NtCurrentProcess(), addr1, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok(!status, "Unexpected status %08lx.\n", status);
    ok(mbi.AllocationProtect == PAGE_READONLY, "Unexpected protection %#lx.\n", mbi.AllocationProtect);
    ok(mbi.State == MEM_COMMIT, "Unexpected state %#lx.\n", mbi.State);
    ok(mbi.Type == MEM_PRIVATE, "Unexpected type %#lx.\n", mbi.Type);
    ok(mbi.RegionSize == 0x10000, "Unexpected size.\n");

    size = 0x10000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&addr1, &size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(!status, "Unexpected status %08lx.\n", status);

    status = NtQueryVirtualMemory( NtCurrentProcess(), addr1, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok(!status, "Unexpected status %08lx.\n", status);
    ok(mbi.AllocationProtect == PAGE_NOACCESS, "Unexpected protection %#lx.\n", mbi.AllocationProtect);
    ok(mbi.State == MEM_RESERVE, "Unexpected state %#lx.\n", mbi.State);
    ok(mbi.Type == MEM_PRIVATE, "Unexpected type %#lx.\n", mbi.Type);
    ok(mbi.RegionSize == 0x10000, "Unexpected size.\n");

    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS, NULL, 0);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE,
            PAGE_NOACCESS, NULL, 0);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    size = 0x1000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
            PAGE_NOACCESS, NULL, 0);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_COMMIT, PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size,
            MEM_WRITE_WATCH | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
            PAGE_READONLY, NULL, 0);
    ok(!status || broken(status == STATUS_INVALID_PARAMETER) /* Win10 1809, the version where
            NtAllocateVirtualMemoryEx is introduced */, "Unexpected status %08lx.\n", status);

    if (!status)
    {
        size = 0x10000;
        status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_COMMIT, PAGE_READWRITE, NULL, 0);
        ok(!status, "Unexpected status %08lx.\n", status);

        status = NtQueryVirtualMemory( NtCurrentProcess(), addr1, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
        ok(!status, "Unexpected status %08lx.\n", status);
        ok(mbi.AllocationProtect == PAGE_READONLY, "Unexpected protection %#lx.\n", mbi.AllocationProtect);
        ok(mbi.State == MEM_COMMIT, "Unexpected state %#lx.\n", mbi.State);
        ok(mbi.Type == MEM_PRIVATE, "Unexpected type %#lx.\n", mbi.Type);
        ok(mbi.RegionSize == 0x10000, "Unexpected size.\n");

        size = 0x10000;
        count = ARRAY_SIZE(addresses);
        status = NtGetWriteWatch( NtCurrentProcess(), WRITE_WATCH_FLAG_RESET, addr1, size,
                                  addresses, &count, &granularity );
        ok(!status, "Unexpected status %08lx.\n", status);
        ok(!count, "Unexpected count %u.\n", (unsigned int)count);
        *((char *)addr1 + 0x1000) = 1;
        count = ARRAY_SIZE(addresses);
        status = NtGetWriteWatch( NtCurrentProcess(), WRITE_WATCH_FLAG_RESET, addr1, size,
                                  addresses, &count, &granularity );
        ok(!status, "Unexpected status %08lx.\n", status);
        ok(count == 1, "Unexpected count %u.\n", (unsigned int)count);
        ok(addresses[0] == (char *)addr1 + 0x1000, "Unexpected address %p.\n", addresses[0]);

        size = 0;
        status = NtFreeVirtualMemory(NtCurrentProcess(), &addr1, &size, MEM_RELEASE);
        ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    }

    /* Placeholder region splitting. */
    addr1 = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE,
            PAGE_NOACCESS, NULL, 0);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    p = addr1;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p, &size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);
    ok(size == 0x10000, "Unexpected size %#Ix.\n", size);
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size == 0x10000, "Unexpected size %#Ix.\n", size);
    ok(p == addr1, "Unexpected addr %p, expected %p.\n", p, addr1);


    /* Split in three regions. */
    addr1 = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS, NULL, 0);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&addr1, &size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    p = addr1;
    p1 = p + size / 2;
    p2 = p1 + size / 4;
    size2 = size / 4;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x4000, "Unexpected size %#Ix.\n", size2);
    ok(p1 == p + size / 2, "Unexpected addr %p, expected %p.\n", p, p + size / 2);

    check_region_size(p, size / 2);
    check_region_size(p1, size / 4);
    check_region_size(p2, size - size / 2 - size / 4);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p, &size2, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x4000, "Unexpected size %#Ix.\n", size2);
    ok(p == addr1, "Unexpected addr %p, expected %p.\n", p, addr1);
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x4000, "Unexpected size %#Ix.\n", size2);
    ok(p1 == p + size / 2, "Unexpected addr %p, expected %p.\n", p1, p + size / 2);
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p2, &size2, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x4000, "Unexpected size %#Ix.\n", size2);
    ok(p2 == p1 + size / 4, "Unexpected addr %p, expected %p.\n", p2, p1 + size / 4);

    /* Split in two regions, specifying lower part. */
    addr1 = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS, NULL, 0);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    size2 = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&addr1, &size2, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(status == STATUS_INVALID_PARAMETER_3, "Unexpected status %08lx.\n", status);
    ok(!size2, "Unexpected size %#Ix.\n", size2);

    p1 = addr1;
    p2 = p1 + size / 4;
    p3 = p2 + size / 4;
    size2 = size / 4;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(p1 == addr1, "Unexpected address.\n");
    ok(size2 == 0x4000, "Unexpected size %#Ix.\n", size2);
    ok(p1 == addr1, "Unexpected addr %p, expected %p.\n", p1, addr1);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p2, &size2, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    check_region_size(p1, p2 - p1);
    check_region_size(p2, p3 - p2);
    check_region_size(p3, size - (p3 - p1));

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size, MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_INVALID_PARAMETER_4, "Unexpected status %08lx.\n", status);

    size2 = size + 0x1000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    size2 = size - 0x1000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    p1 = (char *)addr1 + 0x1000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);
    p1 = addr1;

    size2 = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_INVALID_PARAMETER_3, "Unexpected status %08lx.\n", status);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size, MEM_RELEASE);
    ok(status == STATUS_UNABLE_TO_FREE_VM, "Unexpected status %08lx.\n", status);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size == 0x10000, "Unexpected size %#Ix.\n", size);
    ok(p1 == addr1, "Unexpected addr %p, expected %p.\n", p1, addr1);
    check_region_size(p1, size);

    size2 = size / 4;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x4000, "Unexpected size %#Ix.\n", size2);
    ok(p1 == addr1, "Unexpected addr %p, expected %p.\n", p1, addr1);
    check_region_size(p1, size / 4);
    check_region_size(p2, size - size / 4);

    size2 = size - size / 4;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), (void **)&p2, &size2, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    size2 = size - size / 4;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p2, &size2, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0xc000, "Unexpected size %#Ix.\n", size2);
    ok(p2 == p1 + size / 4, "Unexpected addr %p, expected %p.\n", p2, p1 + size / 4);

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);

    size2 = size / 4;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x4000, "Unexpected size %#Ix.\n", size2);
    ok(p1 == addr1, "Unexpected addr %p, expected %p.\n", p1, addr1);

    size2 = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p3, &size2, MEM_RELEASE);
    ok(status == STATUS_MEMORY_NOT_ALLOCATED, "Unexpected status %08lx.\n", status);

    /* Split in two regions, specifying second half. */
    addr1 = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr1, &size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS, NULL, 0);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size == 0x10000, "Unexpected size %#Ix.\n", size);

    p1 = addr1;
    p2 = p1 + size / 2;

    size2 = size / 2;
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p2, &size2, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x8000, "Unexpected size %#Ix.\n", size2);
    ok(p2 == p1 + size / 2, "Unexpected addr %p, expected %p.\n", p2, p1 + size / 2);
    check_region_size(p1, size / 2);
    check_region_size(p2, size / 2);

    status = NtQueryVirtualMemory( NtCurrentProcess(), p1, MemoryBasicInformation, &mbi, sizeof(mbi), NULL );
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status );
    ok( mbi.AllocationBase == p1, "got %p.\n", mbi.AllocationBase );
    ok( mbi.Type == MEM_PRIVATE, "got %#lx.\n", mbi.Type );
    ok( mbi.State == MEM_RESERVE, "got %#lx.\n", mbi.State );
    ok( mbi.RegionSize == size / 2, "Unexpected size %Iu, expected %Iu.\n", mbi.RegionSize, size / 2 );
    ok( mbi.AllocationProtect == PAGE_NOACCESS, "got %#lx.\n", mbi.AllocationProtect );
    status = NtQueryVirtualMemory( NtCurrentProcess(), p1, MemoryRegionInformation, &mri, sizeof(mri), NULL );
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status );
    ok( mri.AllocationBase == p1, "got %p.\n", mri.AllocationBase );
    ok( mri.RegionSize == size / 2, "Unexpected size %Iu, expected %Iu.\n", mri.RegionSize, size / 2 );
    ok( !mri.CommitSize, "Unexpected size %Iu.\n", mri.CommitSize );
    ok( mri.AllocationProtect == PAGE_NOACCESS, "got %#lx.\n", mri.AllocationProtect );

    status = NtQueryVirtualMemory( NtCurrentProcess(), p2, MemoryBasicInformation, &mbi, sizeof(mbi), NULL );
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status );
    ok( mbi.AllocationBase == p2, "got %p.\n", mbi.AllocationBase );
    ok( mbi.Type == MEM_PRIVATE, "got %#lx.\n", mbi.Type );
    ok( mbi.State == MEM_RESERVE, "got %#lx.\n", mbi.State );
    ok( mbi.RegionSize == size / 2, "Unexpected size %Iu, expected %Iu.\n", mbi.RegionSize, size / 2 );
    ok( mbi.AllocationProtect == PAGE_NOACCESS, "got %#lx.\n", mbi.AllocationProtect );
    status = NtQueryVirtualMemory( NtCurrentProcess(), p2, MemoryRegionInformation, &mri, sizeof(mri), NULL );
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status );
    ok( mri.AllocationBase == p2, "got %p.\n", mri.AllocationBase );
    ok( mri.RegionSize == size / 2, "Unexpected size %Iu, expected %Iu.\n", mri.RegionSize, size / 2 );
    ok( !mri.CommitSize, "Unexpected size %Iu.\n", mri.CommitSize );
    ok( mri.AllocationProtect == PAGE_NOACCESS, "got %#lx.\n", mri.AllocationProtect );

    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p1, &size2, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x8000, "Unexpected size %#Ix.\n", size2);
    ok(p1 == addr1, "Unexpected addr %p, expected %p.\n", p1, addr1);
    status = NtFreeVirtualMemory(NtCurrentProcess(), (void **)&p2, &size2, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(size2 == 0x8000, "Unexpected size %#Ix.\n", size2);
    ok(p2 == p1 + size / 2, "Unexpected addr %p, expected %p.\n", p2, p1 + size / 2);

    memset( ext, 0, sizeof(ext) );
    ext[0].Type = MemExtendedParameterAttributeFlags;
    ext[0].ULong = 0;
    ext[1].Type = MemExtendedParameterAttributeFlags;
    ext[1].ULong = 0;
    size = 0x10000;
    addr1 = NULL;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr1, &size, MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE, ext, 1 );
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    NtFreeVirtualMemory( NtCurrentProcess(), &addr1, &size, MEM_DECOMMIT );
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr1, &size, MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE, ext, 2 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    memset( ext, 0, sizeof(ext) );
    ext[0].Type = MemExtendedParameterAttributeFlags;
    ext[0].ULong = MEM_EXTENDED_PARAMETER_EC_CODE;
    size = 0x10000;
    addr1 = NULL;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr1, &size, MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE, ext, 1 );
#ifdef __x86_64__
    if (pRtlGetNativeSystemInformation)
    {
        SYSTEM_CPU_INFORMATION cpu_info;

        pRtlGetNativeSystemInformation( SystemCpuInformation, &cpu_info, sizeof(cpu_info), NULL );
        if (cpu_info.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
        {
            if (pRtlIsEcCode)
            {
                const void *ptr = (const void *)~(ULONG_PTR)0;

                ok( !pRtlIsEcCode( ptr ), "EC code %p\n", ptr );
                ptr = (const void *)(ULONG_PTR)0x0000800000000000ULL;
                ok( !pRtlIsEcCode( ptr ), "EC code %p\n", ptr );
                ptr = (const void *)(ULONG_PTR)0xffff800000000000ULL;
                ok( !pRtlIsEcCode( ptr ), "EC code %p\n", ptr );
            }
            ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
            if (pRtlIsEcCode) ok( pRtlIsEcCode( addr1 ), "not EC code %p\n", addr1 );
            size = 0;
            NtFreeVirtualMemory( NtCurrentProcess(), &addr1, &size, MEM_RELEASE );

            size = 0x10000;
            addr1 = NULL;
            status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr1, &size, MEM_RESERVE,
                                                 PAGE_EXECUTE_READWRITE, NULL, 0 );
            ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
            if (pRtlIsEcCode) ok( !pRtlIsEcCode( addr1 ), "EC code %p\n", addr1 );
            size = 0x1000;
            status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr1, &size, MEM_COMMIT,
                                                 PAGE_EXECUTE_READWRITE, ext, 1 );
            ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
            if (pRtlIsEcCode)
            {
                ok( pRtlIsEcCode( addr1 ), "not EC code %p\n", addr1 );
                ok( !pRtlIsEcCode( (char *)addr1 + 0x1000 ), "EC code %p\n", (char *)addr1 + 0x1000 );
            }
            size = 0x2000;
            status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr1, &size, MEM_COMMIT,
                                                 PAGE_EXECUTE_READWRITE, NULL, 0 );
            ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
            if (pRtlIsEcCode)
            {
                ok( pRtlIsEcCode( addr1 ), "not EC code %p\n", addr1 );
                ok( !pRtlIsEcCode( (char *)addr1 + 0x1000 ), "EC code %p\n", (char *)addr1 + 0x1000 );
            }

            NtFreeVirtualMemory( NtCurrentProcess(), &addr1, &size, MEM_DECOMMIT );
            if (pRtlIsEcCode) ok( pRtlIsEcCode( addr1 ), "not EC code %p\n", addr1 );

            size = 0x2000;
            ext[0].ULong = 0;
            status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr1, &size, MEM_COMMIT,
                                                 PAGE_EXECUTE_READWRITE, ext, 1 );
            ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
            if (pRtlIsEcCode)
            {
                ok( pRtlIsEcCode( addr1 ), "not EC code %p\n", addr1 );
                ok( !pRtlIsEcCode( (char *)addr1 + 0x1000 ), "EC code %p\n", (char *)addr1 + 0x1000 );
            }

            size = 0;
            NtFreeVirtualMemory( NtCurrentProcess(), &addr1, &size, MEM_RELEASE );
            return;
        }
    }
#endif
    ok(status == STATUS_INVALID_PARAMETER || status == STATUS_NOT_SUPPORTED,
       "Unexpected status %08lx.\n", status);
}

static void test_NtAllocateVirtualMemoryEx_address_requirements(void)
{
    MEM_EXTENDED_PARAMETER ext[2];
    MEM_ADDRESS_REQUIREMENTS a;
    NTSTATUS status;
    SYSTEM_INFO si;
    SIZE_T size;
    void *addr;

    if (!pNtAllocateVirtualMemoryEx)
    {
        win_skip("NtAllocateVirtualMemoryEx() is missing\n");
        return;
    }

    GetSystemInfo(&si);

    memset(&ext, 0, sizeof(ext));
    ext[0].Type = 0;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    memset(&ext, 0, sizeof(ext));
    ext[0].Type = MemExtendedParameterMax;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    memset(&a, 0, sizeof(a));
    ext[0].Type = MemExtendedParameterAddressRequirements;
    ext[0].Pointer = &a;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(!status, "Unexpected status %08lx.\n", status);
    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(!status, "Unexpected status %08lx.\n", status);

    ext[1] = ext[0];
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE, ext, 2);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = NULL;
    a.Alignment = 0;

    a.HighestEndingAddress = (void *)(0x20001000 + 1);
    size = 0x10000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (void *)(0x20001000 - 2);
    size = 0x10000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (void *)(0x20000800 - 1);
    size = 0x10000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (char *)si.lpMaximumApplicationAddress + 0x1000;
    size = 0x10000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (char *)si.lpMaximumApplicationAddress;
    size = 0x10000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(!status, "Unexpected status %08lx.\n", status);
    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(!status, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (void *)(0x20001000 - 1);
    size = 0x40000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(!status, "Unexpected status %08lx.\n", status);
    ok(!((ULONG_PTR)addr & 0xffff), "Unexpected addr %p.\n", addr);
    ok((ULONG_PTR)addr + size <= 0x20001000, "Unexpected addr %p.\n", addr);

    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(!status, "Unexpected status %08lx.\n", status);


    size = 0x40000;
    a.HighestEndingAddress = (void *)(0x20001000 - 1);
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr, 24, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    ok(status == STATUS_INVALID_PARAMETER_3 || status == STATUS_INVALID_PARAMETER,
            "Unexpected status %08lx.\n", status);

    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr, 0xffffffff, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (is_win64 || is_wow64)
        ok(!status || status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);
    else
        ok(status == STATUS_INVALID_PARAMETER_3 || status == STATUS_INVALID_PARAMETER,
                "Unexpected status %08lx.\n", status);

    if (!status)
    {
        size = 0;
        status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
        ok(!status, "Unexpected status %08lx.\n", status);
    }

    a.HighestEndingAddress = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(!status || status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx.\n", status);
    if (!status)
    {
        size = 0;
        status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
        ok(!status, "Unexpected status %08lx.\n", status);
    }


    a.HighestEndingAddress = (void *)(0x20001000 - 1);
    a.Alignment = 0x10000;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(!status, "Unexpected status %08lx.\n", status);
    ok(!((ULONG_PTR)addr & 0xffff), "Unexpected addr %p.\n", addr);
    ok((ULONG_PTR)addr + size < 0x20001000, "Unexpected addr %p.\n", addr);
    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(!status, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (void *)(0x20001000 - 1);
    a.Alignment = 0x20000000;
    size = 0x2000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_NO_MEMORY, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = NULL;
    a.Alignment = 0x8000;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.Alignment = 0x30000;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.Alignment = 0x40000;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(!status, "Unexpected status %08lx.\n", status);
    ok(!((ULONG_PTR)addr & 0x3ffff), "Unexpected addr %p.\n", addr);
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(!status, "Unexpected status %08lx.\n", status);

    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = (void *)0x20001000;
    a.Alignment = 0;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = (void *)(0x20001000 - 1);
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = (void *)(0x20001000 + 1);
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = (void *)0x30000000;
    a.HighestEndingAddress = (void *)0x20000000;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = (void *)0x20000000;
    a.HighestEndingAddress = 0;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(!status, "Unexpected status %08lx.\n", status);
    ok(addr >= (void *)0x20000000, "Unexpected addr %p.\n", addr);
    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(!status, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = (void *)0x20000000;
    a.HighestEndingAddress = (void *)0x2fffffff;
    size = 0x1000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(!status, "Unexpected status %08lx.\n", status);
    ok(addr >= (void *)0x20000000 && addr < (void *)0x30000000, "Unexpected addr %p.\n", addr);
    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(!status, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = (char *)si.lpMaximumApplicationAddress + 1;
    a.HighestEndingAddress = 0;
    size = 0x10000;
    addr = NULL;
    status = pNtAllocateVirtualMemoryEx(NtCurrentProcess(), &addr, &size, MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE, ext, 1);
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);
}

struct test_stack_size_thread_args
{
    DWORD expect_committed;
    DWORD expect_reserved;
};

static void DECLSPEC_NOINLINE force_stack_grow(void)
{
    volatile int buffer[0x2000];
    int i;

    for (i = 0; i < ARRAY_SIZE(buffer); i++) buffer[i] = 0xdeadbeef;
    (void)buffer[0];
}

static void DECLSPEC_NOINLINE force_stack_grow_small(void)
{
    volatile int buffer[0x400];
    int i;

    for (i = 0; i < ARRAY_SIZE(buffer); i++) buffer[i] = 0xdeadbeef;
    (void)buffer[0];
}

static DWORD WINAPI test_stack_size_thread(void *ptr)
{
    struct test_stack_size_thread_args *args = ptr;
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS status;
    SIZE_T size, guard_size;
    DWORD committed, reserved;
    void *addr;

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    reserved = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->DeallocationStack;
    todo_wine ok( committed == args->expect_committed || broken(committed == 0x1000), "unexpected stack committed size %lx, expected %lx\n", committed, args->expect_committed );
    ok( reserved == args->expect_reserved, "unexpected stack reserved size %lx, expected %lx\n", reserved, args->expect_reserved );

    addr = (char *)NtCurrentTeb()->DeallocationStack;
    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.AllocationBase == NtCurrentTeb()->DeallocationStack, "unexpected AllocationBase %p, expected %p\n", mbi.AllocationBase, NtCurrentTeb()->DeallocationStack );
    ok( mbi.AllocationProtect == PAGE_READWRITE, "unexpected AllocationProtect %#lx, expected %#x\n", mbi.AllocationProtect, PAGE_READWRITE );
    ok( mbi.BaseAddress == addr, "unexpected BaseAddress %p, expected %p\n", mbi.BaseAddress, addr );
    ok( mbi.State == MEM_RESERVE, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_RESERVE );
    ok( mbi.Protect == 0, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, 0 );
    ok( mbi.Type == MEM_PRIVATE, "unexpected Type %#lx, expected %#x\n", mbi.Type, MEM_PRIVATE );


    force_stack_grow();

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    reserved = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->DeallocationStack;
    todo_wine ok( committed == 0x9000, "unexpected stack committed size %lx, expected 9000\n", committed );
    ok( reserved == args->expect_reserved, "unexpected stack reserved size %lx, expected %lx\n", reserved, args->expect_reserved );


    /* reserved area shrinks whenever stack grows */

    addr = (char *)NtCurrentTeb()->DeallocationStack;
    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.AllocationBase == NtCurrentTeb()->DeallocationStack, "unexpected AllocationBase %p, expected %p\n", mbi.AllocationBase, NtCurrentTeb()->DeallocationStack );
    ok( mbi.AllocationProtect == PAGE_READWRITE, "unexpected AllocationProtect %#lx, expected %#x\n", mbi.AllocationProtect, PAGE_READWRITE );
    ok( mbi.BaseAddress == addr, "unexpected BaseAddress %p, expected %p\n", mbi.BaseAddress, addr );
    ok( mbi.State == MEM_RESERVE, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_RESERVE );
    ok( mbi.Protect == 0, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, 0 );
    ok( mbi.Type == MEM_PRIVATE, "unexpected Type %#lx, expected %#x\n", mbi.Type, MEM_PRIVATE );

    guard_size = reserved - committed - mbi.RegionSize;
    ok( guard_size == 0x1000 || guard_size == 0x2000 || guard_size == 0x3000, "unexpected guard_size %I64x, expected 1000, 2000 or 3000\n", (UINT64)guard_size );

    /* the commit area is initially preceded by guard pages */

    addr = (char *)NtCurrentTeb()->DeallocationStack + mbi.RegionSize;
    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.AllocationBase == NtCurrentTeb()->DeallocationStack, "unexpected AllocationBase %p, expected %p\n", mbi.AllocationBase, NtCurrentTeb()->DeallocationStack );
    ok( mbi.AllocationProtect == PAGE_READWRITE, "unexpected AllocationProtect %#lx, expected %#x\n", mbi.AllocationProtect, PAGE_READWRITE );
    ok( mbi.BaseAddress == addr, "unexpected BaseAddress %p, expected %p\n", mbi.BaseAddress, addr );
    ok( mbi.RegionSize == guard_size, "unexpected RegionSize %I64x, expected 3000\n", (UINT64)mbi.RegionSize );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    ok( mbi.Protect == (PAGE_READWRITE|PAGE_GUARD), "unexpected Protect %#lx, expected %#x\n", mbi.Protect, PAGE_READWRITE|PAGE_GUARD );
    ok( mbi.Type == MEM_PRIVATE, "unexpected Type %#lx, expected %#x\n", mbi.Type, MEM_PRIVATE );

    addr = (char *)NtCurrentTeb()->Tib.StackLimit;
    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.AllocationBase == NtCurrentTeb()->DeallocationStack, "unexpected AllocationBase %p, expected %p\n", mbi.AllocationBase, NtCurrentTeb()->DeallocationStack );
    ok( mbi.AllocationProtect == PAGE_READWRITE, "unexpected AllocationProtect %#lx, expected %#x\n", mbi.AllocationProtect, PAGE_READWRITE );
    ok( mbi.BaseAddress == addr, "unexpected BaseAddress %p, expected %p\n", mbi.BaseAddress, addr );
    ok( mbi.RegionSize == committed, "unexpected RegionSize %I64x, expected %I64x\n", (UINT64)mbi.RegionSize, (UINT64)committed );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    ok( mbi.Protect == PAGE_READWRITE, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, PAGE_READWRITE );
    ok( mbi.Type == MEM_PRIVATE, "unexpected Type %#lx, expected %#x\n", mbi.Type, MEM_PRIVATE );

    return 0;
}

static DWORD WINAPI test_stack_growth_thread(void *ptr)
{
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS status;
    SIZE_T size, guard_size;
    DWORD committed;
    void *addr;
    DWORD prot;
    void *tmp;

    test_stack_size_thread( ptr );
    if (!is_win64) return 0;

    addr = (char *)NtCurrentTeb()->DeallocationStack;
    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );

    guard_size = (char *)NtCurrentTeb()->Tib.StackLimit - (char *)NtCurrentTeb()->DeallocationStack - mbi.RegionSize;
    ok( guard_size == 0x1000 || guard_size == 0x2000 || guard_size == 0x3000, "unexpected guard_size %I64x, expected 1000, 2000 or 3000\n", (UINT64)guard_size );

    /* setting a guard page shrinks stack automatically */

    addr = (char *)NtCurrentTeb()->Tib.StackLimit + 0x2000;
    size = 0x1000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD );
    ok( !status, "NtAllocateVirtualMemory returned %08lx\n", status );

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    todo_wine ok( committed == 0x6000, "unexpected stack committed size %lx, expected 6000\n", committed );

    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)addr - 0x2000, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.RegionSize == 0x2000, "unexpected RegionSize %I64x, expected 2000\n", (UINT64)mbi.RegionSize );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    ok( mbi.Protect == PAGE_READWRITE, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, PAGE_READWRITE );

    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.RegionSize == 0x1000, "unexpected RegionSize %I64x, expected 1000\n", (UINT64)mbi.RegionSize );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    ok( mbi.Protect == (PAGE_READWRITE|PAGE_GUARD), "unexpected Protect %#lx, expected %#x\n", mbi.Protect, (PAGE_READWRITE|PAGE_GUARD) );

    addr = (char *)NtCurrentTeb()->Tib.StackLimit;
    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    todo_wine ok( mbi.RegionSize == 0x6000, "unexpected RegionSize %I64x, expected 6000\n", (UINT64)mbi.RegionSize );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    ok( mbi.Protect == PAGE_READWRITE, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, PAGE_READWRITE );


    /* guard pages are restored as the stack grows back */

    addr = (char *)NtCurrentTeb()->Tib.StackLimit + 0x4000;
    tmp = (char *)addr - guard_size - 0x1000;
    size = 0x1000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD );
    ok( !status, "NtAllocateVirtualMemory returned %08lx\n", status );

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    todo_wine ok( committed == 0x1000, "unexpected stack committed size %lx, expected 1000\n", committed );

    status = NtQueryVirtualMemory( NtCurrentProcess(), tmp, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    todo_wine ok( mbi.RegionSize == guard_size + 0x1000, "unexpected RegionSize %I64x, expected %I64x\n", (UINT64)mbi.RegionSize, (UINT64)(guard_size + 0x1000) );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    todo_wine ok( mbi.Protect == PAGE_READWRITE, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, PAGE_READWRITE );

    force_stack_grow_small();

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    todo_wine ok( committed == 0x2000, "unexpected stack committed size %lx, expected 2000\n", committed );

    status = NtQueryVirtualMemory( NtCurrentProcess(), tmp, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.RegionSize == 0x1000, "unexpected RegionSize %I64x, expected 1000\n", (UINT64)mbi.RegionSize );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    todo_wine ok( mbi.Protect == PAGE_READWRITE, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, PAGE_READWRITE );

    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)tmp + 0x1000, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.RegionSize == guard_size, "unexpected RegionSize %I64x, expected %I64x\n", (UINT64)mbi.RegionSize, (UINT64)guard_size );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    todo_wine ok( mbi.Protect == (PAGE_READWRITE|PAGE_GUARD), "unexpected Protect %#lx, expected %#x\n", mbi.Protect, (PAGE_READWRITE|PAGE_GUARD) );


    /* forcing stack limit over guard pages still shrinks the stack on page fault */

    addr = (char *)tmp + guard_size + 0x1000;
    size = 0x1000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD );
    ok( !status, "NtAllocateVirtualMemory returned %08lx\n", status );

    NtCurrentTeb()->Tib.StackLimit = (char *)tmp;

    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)tmp + 0x1000, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    todo_wine ok( mbi.RegionSize == guard_size + 0x1000, "unexpected RegionSize %I64x, expected %I64x\n", (UINT64)mbi.RegionSize, (UINT64)(guard_size + 0x1000) );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    todo_wine ok( mbi.Protect == (PAGE_READWRITE|PAGE_GUARD), "unexpected Protect %#lx, expected %#x\n", mbi.Protect, (PAGE_READWRITE|PAGE_GUARD) );

    force_stack_grow_small();

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    todo_wine ok( committed == 0x2000, "unexpected stack committed size %lx, expected 2000\n", committed );


    /* it works with NtProtectVirtualMemory as well */

    force_stack_grow();

    addr = (char *)NtCurrentTeb()->Tib.StackLimit + 0x2000;
    size = 0x1000;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_READWRITE | PAGE_GUARD, &prot );
    ok( !status, "NtProtectVirtualMemory returned %08lx\n", status );
    todo_wine ok( prot == PAGE_READWRITE, "unexpected prot %#lx, expected %#x\n", prot, PAGE_READWRITE );

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    todo_wine ok( committed == 0x6000, "unexpected stack committed size %lx, expected 6000\n", committed );

    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)addr - 0x2000, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    todo_wine ok( mbi.RegionSize == 0x2000, "unexpected RegionSize %I64x, expected 2000\n", (UINT64)mbi.RegionSize );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    todo_wine ok( mbi.Protect == PAGE_READWRITE, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, PAGE_READWRITE );

    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    ok( mbi.RegionSize == 0x1000, "unexpected RegionSize %I64x, expected 1000\n", (UINT64)mbi.RegionSize );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    ok( mbi.Protect == (PAGE_READWRITE|PAGE_GUARD), "unexpected Protect %#lx, expected %#x\n", mbi.Protect, (PAGE_READWRITE|PAGE_GUARD) );

    addr = (char *)NtCurrentTeb()->Tib.StackLimit;
    status = NtQueryVirtualMemory( NtCurrentProcess(), addr, MemoryBasicInformation, &mbi, sizeof(mbi), &size );
    ok( !status, "NtQueryVirtualMemory returned %08lx\n", status );
    todo_wine ok( mbi.RegionSize == 0x6000, "unexpected RegionSize %I64x, expected 6000\n", (UINT64)mbi.RegionSize );
    ok( mbi.State == MEM_COMMIT, "unexpected State %#lx, expected %#x\n", mbi.State, MEM_COMMIT );
    todo_wine ok( mbi.Protect == PAGE_READWRITE, "unexpected Protect %#lx, expected %#x\n", mbi.Protect, PAGE_READWRITE );


    /* clearing the guard pages doesn't change StackLimit back */

    force_stack_grow();

    addr = (char *)NtCurrentTeb()->Tib.StackLimit + 0x2000;
    size = 0x1000;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_READWRITE | PAGE_GUARD, &prot );
    ok( !status, "NtProtectVirtualMemory returned %08lx\n", status );
    todo_wine ok( prot == PAGE_READWRITE, "unexpected prot %#lx, expected %#x\n", prot, PAGE_READWRITE );

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    todo_wine ok( committed == 0x6000, "unexpected stack committed size %lx, expected 6000\n", committed );

    status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size, PAGE_READWRITE, &prot );
    ok( !status, "NtProtectVirtualMemory returned %08lx\n", status );
    ok( prot == (PAGE_READWRITE | PAGE_GUARD), "unexpected prot %#lx, expected %#x\n", prot, (PAGE_READWRITE | PAGE_GUARD) );

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    todo_wine ok( committed == 0x6000, "unexpected stack committed size %lx, expected 6000\n", committed );

    /* and as we messed with it and it now doesn't fault, it doesn't grow back either */

    force_stack_grow();

    committed = (char *)NtCurrentTeb()->Tib.StackBase - (char *)NtCurrentTeb()->Tib.StackLimit;
    todo_wine ok( committed == 0x6000, "unexpected stack committed size %lx, expected 6000\n", committed );

    ExitThread(0);
}

static DWORD WINAPI test_stack_size_dummy_thread(void *ptr)
{
    return 0;
}

static void test_RtlCreateUserStack(void)
{
    IMAGE_NT_HEADERS *nt = RtlImageNtHeader( NtCurrentTeb()->Peb->ImageBaseAddress );
    struct test_stack_size_thread_args args;
    SIZE_T default_commit = nt->OptionalHeader.SizeOfStackCommit;
    SIZE_T default_reserve = nt->OptionalHeader.SizeOfStackReserve;
    MEMORY_BASIC_INFORMATION mbi;
    INITIAL_TEB stack = {0};
    unsigned int i;
    NTSTATUS ret;
    HANDLE thread;
    CLIENT_ID id;
    SIZE_T szret;

    struct
    {
        SIZE_T commit, reserve, commit_align, reserve_align, expect_commit, expect_reserve;
    }
    tests[] =
    {
        {       0,        0,      1,        1, default_commit, default_reserve},
        {  0x2000,        0,      1,        1,         0x2000, default_reserve},
        {  0x4000,        0,      1,        1,         0x4000, default_reserve},
        {       0, 0x200000,      1,        1, default_commit, 0x200000},
        {  0x4000, 0x200000,      1,        1,         0x4000, 0x200000},
        {0x100000, 0x100000,      1,        1,       0x100000, 0x100000},
        { 0xff000, 0x100000,      1,        1,        0xff000, 0x100000},
        { 0x20000,  0x20000,      1,        1,        0x20000, 0x100000},

        {       0, 0x110000,      1,        1, default_commit, 0x110000},
        {       0, 0x110000,      1,  0x40000, default_commit, 0x140000},
        {       0, 0x140000,      1,  0x40000, default_commit, 0x140000},
        { 0x11000, 0x140000,      1,  0x40000,        0x11000, 0x140000},
        { 0x11000, 0x140000, 0x4000,  0x40000,        0x14000, 0x140000},
        {       0,        0, 0x4000, 0x400000,
                (default_commit + 0x3fff) & ~0x3fff,
                (default_reserve + 0x3fffff) & ~0x3fffff},
    };

    if (!pRtlCreateUserStack)
    {
        win_skip("RtlCreateUserStack() is missing\n");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        memset(&stack, 0xcc, sizeof(stack));
        ret = pRtlCreateUserStack(tests[i].commit, tests[i].reserve, 0,
                tests[i].commit_align, tests[i].reserve_align, &stack);
        ok(!ret, "%u: got status %#lx\n", i, ret);
        ok(!stack.OldStackBase, "%u: got OldStackBase %p\n", i, stack.OldStackBase);
        ok(!stack.OldStackLimit, "%u: got OldStackLimit %p\n", i, stack.OldStackLimit);
        ok(!((ULONG_PTR)stack.DeallocationStack & (page_size - 1)),
                "%u: got unaligned memory %p\n", i, stack.DeallocationStack);
        ok((ULONG_PTR)stack.StackBase - (ULONG_PTR)stack.DeallocationStack == tests[i].expect_reserve,
                "%u: got reserve %#Ix\n", i, (ULONG_PTR)stack.StackBase - (ULONG_PTR)stack.DeallocationStack);
        todo_wine ok((ULONG_PTR)stack.StackBase - (ULONG_PTR)stack.StackLimit == tests[i].expect_commit,
                "%u: got commit %#Ix\n", i, (ULONG_PTR)stack.StackBase - (ULONG_PTR)stack.StackLimit);
        szret = VirtualQuery(stack.DeallocationStack, &mbi, sizeof(mbi));
        ok(szret == sizeof(mbi), "got %Iu.\n", szret);
        ok(mbi.AllocationBase == stack.DeallocationStack, "got %p, %p.\n", mbi.AllocationBase, stack.DeallocationStack);
        if (tests[i].commit + 2 * page_size <= max( tests[i].reserve, 0x100000))
        {
            ok(mbi.State == MEM_RESERVE, "%u: got %#lx.\n", i, mbi.State);
            ok(!mbi.Protect, "%u: got %#lx.\n", i, mbi.Protect);
        }
        else if (tests[i].commit + page_size <= max( tests[i].reserve, 0x100000))
        {
            todo_wine ok(mbi.State == MEM_COMMIT, "%u: got %#lx.\n", i, mbi.State);
            todo_wine ok(mbi.Protect == (PAGE_READWRITE | PAGE_GUARD), "%u: got %#lx.\n", i, mbi.Protect);
        }
        else
        {
            todo_wine ok(mbi.State == MEM_COMMIT, "%u: got %#lx.\n", i, mbi.State);
            todo_wine ok(mbi.Protect == PAGE_READWRITE, "%u: got %#lx.\n", i, mbi.Protect);
        }
        pRtlFreeUserStack(stack.DeallocationStack);
    }

    ret = pRtlCreateUserStack(0x11000, 0x110000, 0, 1, 0, &stack);
    ok(ret == STATUS_INVALID_PARAMETER, "got %#lx\n", ret);

    ret = pRtlCreateUserStack(0x11000, 0x110000, 0, 0, 1, &stack);
    ok(ret == STATUS_INVALID_PARAMETER, "got %#lx\n", ret);

    args.expect_committed = 0x4000;
    args.expect_reserved = default_reserve;
    thread = CreateThread(NULL, 0x3f00, test_stack_growth_thread, &args, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    args.expect_committed = default_commit < 0x2000 ? 0x2000 : default_commit;
    args.expect_reserved = 0x400000;
    thread = CreateThread(NULL, 0x3ff000, test_stack_growth_thread, &args, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    if (is_win64)
    {
        thread = CreateThread(NULL, 0x80000000, test_stack_size_dummy_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        ok(thread != NULL, "CreateThread with huge stack failed\n");
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }

    args.expect_committed = default_commit < 0x2000 ? 0x2000 : default_commit;
    args.expect_reserved = 0x100000;
    for (i = 0; i < 32; i++)
    {
        ULONG mask = ~0u >> i;
        NTSTATUS expect_ret = STATUS_SUCCESS;

        if (i == 12) expect_ret = STATUS_CONFLICTING_ADDRESSES;
        else if (i >= 13) expect_ret = STATUS_INVALID_PARAMETER;
        ret = pRtlCreateUserStack( args.expect_committed, args.expect_reserved, i, 0x1000, 0x1000, &stack );
        ok( ret == expect_ret || ret == STATUS_NO_MEMORY ||
            (ret == STATUS_INVALID_PARAMETER_3 && expect_ret == STATUS_INVALID_PARAMETER) ||
            broken( i == 1 && ret == STATUS_INVALID_PARAMETER_3 ), /* win7 */
            "%u: got %lx / %lx\n", i, ret, expect_ret );
        if (!ret) pRtlFreeUserStack( stack.DeallocationStack );
        ret = pRtlCreateUserThread( GetCurrentProcess(), NULL, FALSE, i,
                                    args.expect_reserved, args.expect_committed,
                                    (void *)test_stack_size_thread, &args, &thread, &id );
        ok( ret == expect_ret || ret == STATUS_NO_MEMORY ||
            (ret == STATUS_INVALID_PARAMETER_3 && expect_ret == STATUS_INVALID_PARAMETER) ||
            broken( i == 1 && ret == STATUS_INVALID_PARAMETER_3 ), /* win7 */
            "%u: got %lx / %lx\n", i, ret, expect_ret );
        if (!ret)
        {
            WaitForSingleObject( thread, INFINITE );
            CloseHandle( thread );
        }

        if (mask <= 31) continue;
        if (!is_win64 && !is_wow64) expect_ret = STATUS_INVALID_PARAMETER_3;
        ret = pRtlCreateUserStack( args.expect_committed, args.expect_reserved, mask, 0x1000, 0x1000, &stack );
        ok( ret == expect_ret || ret == STATUS_NO_MEMORY ||
            (ret == STATUS_INVALID_PARAMETER_3 && expect_ret == STATUS_INVALID_PARAMETER),
            "%08lx: got %lx / %lx\n", mask, ret, expect_ret );
        if (!ret) pRtlFreeUserStack( stack.DeallocationStack );
        ret = pRtlCreateUserThread( GetCurrentProcess(), NULL, FALSE, mask,
                                    args.expect_reserved, args.expect_committed,
                                    (void *)test_stack_size_thread, &args, &thread, &id );
        ok( ret == expect_ret || ret == STATUS_NO_MEMORY ||
            (ret == STATUS_INVALID_PARAMETER_3 && expect_ret == STATUS_INVALID_PARAMETER),
            "%08lx: got %lx / %lx\n", mask, ret, expect_ret );
        if (!ret)
        {
            WaitForSingleObject( thread, INFINITE );
            CloseHandle( thread );
        }
    }
}

static void check_section_commit_size( HANDLE process )
{
    const SIZE_T commit_size = page_size + 1;
    const SIZE_T rounded_commit = 2 * page_size;
    MEMORY_BASIC_INFORMATION mbi;
    LARGE_INTEGER offset, section_size;
    HANDLE mapping = NULL;
    SIZE_T size, ret_size;
    void *ptr;
    NTSTATUS status;

    section_size.QuadPart = 0x10000;
    status = NtCreateSection( &mapping, SECTION_ALL_ACCESS, NULL, &section_size,
                              PAGE_READWRITE, SEC_RESERVE, NULL );
    ok( status == STATUS_SUCCESS, "NtCreateSection returned %08lx\n", status );
    if (status) return;

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection( mapping, process, &ptr, 0, commit_size, &offset, &size,
                                 ViewUnmap, 0, PAGE_READWRITE );
    ok( status == STATUS_SUCCESS, "NtMapViewOfSection returned %08lx\n", status );
    if (status)
    {
        NtClose( mapping );
        return;
    }

    status = NtQueryVirtualMemory( process, ptr, MemoryBasicInformation, &mbi, sizeof(mbi),
                                   &ret_size );
    ok( status == STATUS_SUCCESS, "NtQueryVirtualMemory returned %08lx\n", status );
    if (!status)
    {
        ok( mbi.BaseAddress == ptr, "got base %p, expected %p\n", mbi.BaseAddress, ptr );
        ok( mbi.State == MEM_COMMIT, "got state %#lx\n", mbi.State );
        ok( mbi.RegionSize == rounded_commit, "got region size %Iu, expected %Iu\n",
            mbi.RegionSize, rounded_commit );
    }

    status = NtQueryVirtualMemory( process, (char *)ptr + rounded_commit,
                                   MemoryBasicInformation, &mbi, sizeof(mbi), &ret_size );
    ok( status == STATUS_SUCCESS, "NtQueryVirtualMemory returned %08lx\n", status );
    if (!status)
    {
        ok( mbi.BaseAddress == (char *)ptr + rounded_commit,
            "got base %p, expected %p\n", mbi.BaseAddress, (char *)ptr + rounded_commit );
        ok( mbi.State == MEM_RESERVE, "got state %#lx\n", mbi.State );
    }

    status = NtUnmapViewOfSection( process, ptr );
    ok( status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status );
    NtClose( mapping );
}

static void test_wow64_section_commit_size( HANDLE process )
{
    if (!is_wow64) return;

    check_section_commit_size( NtCurrentProcess() );
    check_section_commit_size( process );
}

static void test_wow64_sec_reserve_alias_commit(void)
{
#ifndef _WIN64
    LARGE_INTEGER section_size;
    SIZE_T size, transferred;
    HANDLE mapping = NULL;
    void *first = NULL, *second = NULL, *commit;
    BYTE input, output;
    NTSTATUS status;

    if (!is_wow64) return;

    section_size.QuadPart = 0x10000;
    status = NtCreateSection( &mapping, SECTION_ALL_ACCESS, NULL, &section_size,
                              PAGE_READWRITE, SEC_RESERVE, NULL );
    ok( !status, "NtCreateSection returned %#lx\n", status );
    if (status) return;

    size = 0;
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &first, 0, 0, NULL, &size,
                                 ViewUnmap, 0, PAGE_READWRITE );
    ok( !status, "first SEC_RESERVE map returned %#lx\n", status );
    size = 0;
    if (!status)
    {
        status = NtMapViewOfSection( mapping, NtCurrentProcess(), &second, 0, 0, NULL, &size,
                                     ViewUnmap, 0, PAGE_READWRITE );
        ok( !status, "second SEC_RESERVE map returned %#lx\n", status );
    }
    if (!first || !second) goto done;

    commit = first;
    size = page_size;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &commit, 0, &size,
                                      MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "SEC_RESERVE alias commit returned %#lx\n", status );
    if (status) goto done;

    input = 0x4d;
    transferred = 0xdeadbeef;
    status = NtWriteVirtualMemory( NtCurrentProcess(), first, &input, sizeof(input),
                                   &transferred );
    ok( !status && transferred == sizeof(input),
        "first alias write returned %#lx, %Iu\n", status, transferred );
    output = 0;
    transferred = 0xdeadbeef;
    status = NtReadVirtualMemory( NtCurrentProcess(), second, &output, sizeof(output),
                                  &transferred );
    ok( !status && transferred == sizeof(output) && output == input,
        "unfaulted alias read returned %#lx, %Iu, %#x\n", status, transferred, output );

    input = 0xa6;
    transferred = 0xdeadbeef;
    status = NtWriteVirtualMemory( NtCurrentProcess(), second, &input, sizeof(input),
                                   &transferred );
    ok( !status && transferred == sizeof(input),
        "unfaulted alias write returned %#lx, %Iu\n", status, transferred );
    output = 0;
    transferred = 0xdeadbeef;
    status = NtReadVirtualMemory( NtCurrentProcess(), first, &output, sizeof(output),
                                  &transferred );
    ok( !status && transferred == sizeof(output) && output == input,
        "first alias read returned %#lx, %Iu, %#x\n", status, transferred, output );

done:
    if (second) NtUnmapViewOfSection( NtCurrentProcess(), second );
    if (first) NtUnmapViewOfSection( NtCurrentProcess(), first );
    NtClose( mapping );
#endif
}

static void test_NtMapViewOfSection(void)
{
    static const char testfile[] = "testfile.xxx";
    static const char data[] = "test data for NtMapViewOfSection";
    char buffer[sizeof(data)];
    HANDLE file, mapping, process;
    void *ptr, *ptr2, *mem, *mem2;
    BOOL ret;
    DWORD status, written;
    SIZE_T size, size2, result;
    LARGE_INTEGER offset;
    ULONG_PTR zero_bits;
    SYSTEM_INFO si;

    if (!pIsWow64Process || !pIsWow64Process(NtCurrentProcess(), &is_wow64)) is_wow64 = FALSE;

    file = CreateFileA(testfile, GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, 0);
    ok(file != INVALID_HANDLE_VALUE, "Failed to create test file\n");
    WriteFile(file, data, sizeof(data), &written, NULL);
    SetFilePointer(file, 4096, NULL, FILE_BEGIN);
    SetEndOfFile(file);

    /* read/write mapping */

    mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, 4096, NULL);
    ok(mapping != 0, "CreateFileMapping failed\n");

    process = create_target_process("sleep");
    ok(process != NULL, "Can't start process\n");

    test_wow64_section_commit_size( process );
    test_wow64_sec_reserve_alias_commit();

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, NULL, &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_INVALID_HANDLE, "NtMapViewOfSection returned %08lx\n", status);

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "NtMapViewOfSection returned %08lx\n", status);
    ok(!((ULONG_PTR)ptr & 0xffff), "returned memory %p is not aligned to 64k\n", ptr);

    ret = ReadProcessMemory(process, ptr, buffer, sizeof(buffer), &result);
    ok(ret, "ReadProcessMemory failed\n");
    ok(result == sizeof(buffer), "ReadProcessMemory didn't read all data (%Ix)\n", result);
    ok(!memcmp(buffer, data, sizeof(buffer)), "Wrong data read\n");

    /* 1 zero bits should zero 63-31 upper bits */
    ptr2 = NULL;
    size = 0;
    zero_bits = 1;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr2, zero_bits, 0, &offset, &size, 1, MEM_TOP_DOWN, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS || status == STATUS_NO_MEMORY,
       "NtMapViewOfSection returned %08lx\n", status);
    if (status == STATUS_SUCCESS)
    {
        ok(((UINT_PTR)ptr2 >> (32 - zero_bits)) == 0,
           "NtMapViewOfSection returned address: %p\n", ptr2);

        status = NtUnmapViewOfSection(process, ptr2);
        ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);
    }

    for (zero_bits = 2; zero_bits <= 20; zero_bits++)
    {
        ptr2 = NULL;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection(mapping, process, &ptr2, zero_bits, 0, &offset, &size, 1, MEM_TOP_DOWN, PAGE_READWRITE);
        ok(status == STATUS_SUCCESS || status == STATUS_NO_MEMORY,
           "NtMapViewOfSection with %d zero_bits returned %08lx\n", (int)zero_bits, status);
        if (status == STATUS_SUCCESS)
        {
            ok(((UINT_PTR)ptr2 >> (32 - zero_bits)) == 0,
               "NtMapViewOfSection with %d zero_bits returned address %p\n", (int)zero_bits, ptr2);

            status = NtUnmapViewOfSection(process, ptr2);
            ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);
        }
    }

    /* 21 zero bits never succeeds */
    ptr2 = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr2, 21, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_NO_MEMORY || status == STATUS_INVALID_PARAMETER,
       "NtMapViewOfSection returned %08lx\n", status);

    /* 22 zero bits is invalid */
    ptr2 = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr2, 22, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_INVALID_PARAMETER_4 || status == STATUS_INVALID_PARAMETER,
       "NtMapViewOfSection returned %08lx\n", status);

    /* zero bits > 31 should be considered as a leading zeroes bitmask on 64bit and WoW64 */
    ptr2 = NULL;
    size = 0;
    zero_bits = 0x1aaaaaaa;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr2, zero_bits, 0, &offset, &size, 1, MEM_TOP_DOWN, PAGE_READWRITE);

    if (!is_win64 && !is_wow64)
    {
        ok(status == STATUS_INVALID_PARAMETER_4, "NtMapViewOfSection returned %08lx\n", status);
    }
    else
    {
        ok(status == STATUS_SUCCESS || status == STATUS_NO_MEMORY,
           "NtMapViewOfSection returned %08lx\n", status);
        if (status == STATUS_SUCCESS)
        {
            ok(((UINT_PTR)ptr2 & ~get_zero_bits_mask(zero_bits)) == 0 &&
               ((UINT_PTR)ptr2 & ~zero_bits) != 0, /* only the leading zeroes matter */
               "NtMapViewOfSection returned address %p\n", ptr2);

            status = NtUnmapViewOfSection(process, ptr2);
            ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);
        }
    }

    /* mapping at the same page conflicts */
    ptr2 = ptr;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "NtMapViewOfSection returned %08lx\n", status);

    /* offset has to be aligned */
    ptr2 = ptr;
    size = 0;
    offset.QuadPart = 1;
    status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_MAPPED_ALIGNMENT, "NtMapViewOfSection returned %08lx\n", status);

    /* ptr has to be aligned */
    ptr2 = (char *)ptr + 42;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_MAPPED_ALIGNMENT, "NtMapViewOfSection returned %08lx\n", status);

    /* still not 64k aligned */
    ptr2 = (char *)ptr + 0x1000;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_MAPPED_ALIGNMENT, "NtMapViewOfSection returned %08lx\n", status);

    /* when an address is passed, it has to satisfy the provided number of zero bits */
    ptr2 = (char *)ptr + 0x1000;
    size = 0;
    offset.QuadPart = 0;
    zero_bits = get_zero_bits(((UINT_PTR)ptr2) >> 1);
    status = NtMapViewOfSection(mapping, process, &ptr2, zero_bits, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_INVALID_PARAMETER_4 || status == STATUS_INVALID_PARAMETER,
       "NtMapViewOfSection returned %08lx\n", status);

    ptr2 = (char *)ptr + 0x1000;
    size = 0;
    offset.QuadPart = 0;
    zero_bits = get_zero_bits((UINT_PTR)ptr2);
    status = NtMapViewOfSection(mapping, process, &ptr2, zero_bits, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_MAPPED_ALIGNMENT, "NtMapViewOfSection returned %08lx\n", status);

    if (!is_win64 && !is_wow64)
    {
        /* new memory region conflicts with previous mapping */
        ptr2 = ptr;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset,
                                    &size, 1, AT_ROUND_TO_PAGE, PAGE_READWRITE);
        ok(status == STATUS_CONFLICTING_ADDRESSES, "NtMapViewOfSection returned %08lx\n", status);

        ptr2 = (char *)ptr + 42;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset,
                                    &size, 1, AT_ROUND_TO_PAGE, PAGE_READWRITE);
        ok(status == STATUS_CONFLICTING_ADDRESSES, "NtMapViewOfSection returned %08lx\n", status);

        /* in contrary to regular NtMapViewOfSection, only 4kb align is enforced */
        ptr2 = (char *)ptr + 0x1000;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset,
                                    &size, 1, AT_ROUND_TO_PAGE, PAGE_READWRITE);
        ok(status == STATUS_SUCCESS, "NtMapViewOfSection returned %08lx\n", status);
        ok((char *)ptr2 == (char *)ptr + 0x1000,
           "expected address %p, got %p\n", (char *)ptr + 0x1000, ptr2);
        status = NtUnmapViewOfSection(process, ptr2);
        ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

        /* the address is rounded down if not on a page boundary */
        ptr2 = (char *)ptr + 0x1001;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset,
                                    &size, 1, AT_ROUND_TO_PAGE, PAGE_READWRITE);
        ok(status == STATUS_SUCCESS, "NtMapViewOfSection returned %08lx\n", status);
        ok((char *)ptr2 == (char *)ptr + 0x1000,
           "expected address %p, got %p\n", (char *)ptr + 0x1000, ptr2);
        status = NtUnmapViewOfSection(process, ptr2);
        ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

        ptr2 = (char *)ptr + 0x2000;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset,
                                    &size, 1, AT_ROUND_TO_PAGE, PAGE_READWRITE);
        ok(status == STATUS_SUCCESS, "NtMapViewOfSection returned %08lx\n", status);
        ok((char *)ptr2 == (char *)ptr + 0x2000,
           "expected address %p, got %p\n", (char *)ptr + 0x2000, ptr2);
        status = NtUnmapViewOfSection(process, ptr2);
        ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);
    }
    else
    {
        ptr2 = (char *)ptr + 0x1000;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset,
                                    &size, 1, AT_ROUND_TO_PAGE, PAGE_READWRITE);
        ok(status == STATUS_INVALID_PARAMETER_9 || status == STATUS_INVALID_PARAMETER,
           "NtMapViewOfSection returned %08lx\n", status);
    }

    status = NtUnmapViewOfSection(process, ptr);
    ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

    NtClose(mapping);

    CloseHandle(file);
    DeleteFileA(testfile);

    /* test zero_bits > 31 with a 64-bit DLL file image mapping */
    if (is_win64)
    {
        file = CreateFileA("c:\\windows\\system32\\version.dll", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, 0);
        ok(file != INVALID_HANDLE_VALUE, "Failed to open version.dll\n");

        mapping = CreateFileMappingA(file, NULL, PAGE_READONLY|SEC_IMAGE, 0, 0, NULL);
        ok(mapping != 0, "CreateFileMapping failed\n");

        ptr = NULL;
        size = 0;
        offset.QuadPart = 0;
        zero_bits = 0x7fffffff;
        status = NtMapViewOfSection(mapping, process, &ptr, zero_bits, 0, &offset, &size, 1, 0, PAGE_READONLY);

        ok(status == STATUS_SUCCESS || status == STATUS_IMAGE_NOT_AT_BASE, "NtMapViewOfSection returned %08lx\n", status);
        ok(!((ULONG_PTR)ptr & 0xffff), "returned memory %p is not aligned to 64k\n", ptr);
        ok(((UINT_PTR)ptr & ~get_zero_bits_mask(zero_bits)) == 0, "NtMapViewOfSection returned address %p\n", ptr);

        status = NtUnmapViewOfSection(process, ptr);
        ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

        NtClose(mapping);
        CloseHandle(file);
    }

    /* image offset */

    GetSystemInfo(&si);

    file = CreateFileA("c:\\windows\\system32\\ntdll.dll", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, 0);
    ok(file != INVALID_HANDLE_VALUE, "Failed to open ntdll.dll\n");

    mapping = CreateFileMappingA(file, NULL, PAGE_READONLY|SEC_IMAGE, 0, 0, NULL);
    ok(mapping != 0, "CreateFileMapping failed\n");

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, process, &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READONLY);
    ok(status == STATUS_IMAGE_NOT_AT_BASE, "NtMapViewOfSection returned %08lx\n", status);

    ptr2 = NULL;
    size2 = 0;
    offset.QuadPart = si.dwAllocationGranularity;
    status = NtMapViewOfSection(mapping, process, &ptr2, 0, 0, &offset, &size2, 1, 0, PAGE_READONLY);
    ok(status == STATUS_IMAGE_NOT_AT_BASE, "NtMapViewOfSection returned %08lx\n", status);

    ok(size2 == size - si.dwAllocationGranularity, "got unexpected sizes %Ix, %Ix\n", size, size2);
    size2 = size - si.dwAllocationGranularity;

    mem = malloc(size2);
    ret = ReadProcessMemory(process, (char*)ptr + si.dwAllocationGranularity, mem, size2, &result);
    ok(ret, "ReadProcessMemory failed\n");
    ok(size2 == result, "ReadProcessMemory didn't read all data (%Ix)\n", result);

    mem2 = malloc(size2);
    ret = ReadProcessMemory(process, ptr2, mem2, size2, &result);
    ok(ret, "ReadProcessMemory failed\n");
    ok(size2 == result, "ReadProcessMemory didn't read all data (%Ix)\n", result);

    ok(memcmp(mem, mem2, size2) == 0, "memory does not match\n");

    free(mem);
    free(mem2);

    status = NtUnmapViewOfSection(process, ptr);
    ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

    status = NtUnmapViewOfSection(process, ptr2);
    ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

    NtClose(mapping);
    CloseHandle(file);

    TerminateProcess(process, 0);
    CloseHandle(process);
}

static void test_NtMapViewOfSectionEx(void)
{
    static const char testfile[] = "testfile.xxx";
    static const char data[] = "test data for NtMapViewOfSectionEx";
    char buffer[sizeof(data)];
    MEM_EXTENDED_PARAMETER ext[2];
    MEM_ADDRESS_REQUIREMENTS a;
    SYSTEM_INFO si;
    HANDLE file, mapping, process;
    DWORD status, written;
    SIZE_T size, result;
    LARGE_INTEGER offset;
    void *ptr, *ptr2;
    BOOL ret;

    if (!pNtMapViewOfSectionEx)
    {
        win_skip("NtMapViewOfSectionEx() is not supported.\n");
        return;
    }

    if (!pIsWow64Process || !pIsWow64Process(NtCurrentProcess(), &is_wow64)) is_wow64 = FALSE;
    GetSystemInfo(&si);

    file = CreateFileA(testfile, GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, 0);
    ok(file != INVALID_HANDLE_VALUE, "Failed to create test file\n");
    WriteFile(file, data, sizeof(data), &written, NULL);
    SetFilePointer(file, 0x40000, NULL, FILE_BEGIN);
    SetEndOfFile(file);

    /* read/write mapping */

    mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, 0x40000, NULL);
    ok(mapping != 0, "CreateFileMapping failed\n");

    process = create_target_process("sleep");
    ok(process != NULL, "Can't start process\n");

    ptr = NULL;
    size = 0x1000;
    offset.QuadPart = 0;
    status = pNtMapViewOfSectionEx(mapping, NULL, &ptr, &offset, &size, 0, PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_INVALID_HANDLE, "Unexpected status %08lx\n", status);

    ptr = NULL;
    size = 0x1000;
    offset.QuadPart = 0;
    status = pNtMapViewOfSectionEx(mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx\n", status);
    ok(!((ULONG_PTR)ptr & 0xffff), "returned memory %p is not aligned to 64k\n", ptr);

    ret = ReadProcessMemory(process, ptr, buffer, sizeof(buffer), &result);
    ok(ret, "ReadProcessMemory failed\n");
    ok(result == sizeof(buffer), "ReadProcessMemory didn't read all data (%Ix)\n", result);
    ok(!memcmp(buffer, data, sizeof(buffer)), "Wrong data read\n");

    /* mapping at the same page conflicts */
    ptr2 = ptr;
    size = 0;
    offset.QuadPart = 0;
    status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, 0, PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx\n", status);

    /* offset has to be aligned */
    ptr2 = ptr;
    size = 0;
    offset.QuadPart = 1;
    status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, 0, PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_MAPPED_ALIGNMENT, "Unexpected status %08lx\n", status);

    /* ptr has to be aligned */
    ptr2 = (char *)ptr + 42;
    size = 0;
    offset.QuadPart = 0;
    status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, 0, PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_MAPPED_ALIGNMENT, "Unexpected status %08lx\n", status);

    /* still not 64k aligned */
    ptr2 = (char *)ptr + 0x1000;
    size = 0;
    offset.QuadPart = 0;
    status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, 0, PAGE_READWRITE, NULL, 0);
    ok(status == STATUS_MAPPED_ALIGNMENT, "Unexpected status %08lx\n", status);

    if (!is_win64 && !is_wow64)
    {
        /* new memory region conflicts with previous mapping */
        ptr2 = ptr;
        size = 0x1000;
        offset.QuadPart = 0;
        status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, AT_ROUND_TO_PAGE, PAGE_READWRITE, NULL, 0);
        ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx\n", status);

        ptr2 = (char *)ptr + 42;
        size = 0x1000;
        offset.QuadPart = 0;
        status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, AT_ROUND_TO_PAGE, PAGE_READWRITE, NULL, 0);
        ok(status == STATUS_CONFLICTING_ADDRESSES, "Unexpected status %08lx\n", status);

        /* in contrary to regular NtMapViewOfSection, only 4kb align is enforced */
        ptr2 = (char *)ptr + 0x1000;
        size = 0x1000;
        offset.QuadPart = 0;
        status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, AT_ROUND_TO_PAGE, PAGE_READWRITE, NULL, 0);
        ok(status == STATUS_SUCCESS, "Unexpected status %08lx\n", status);
        ok((char *)ptr2 == (char *)ptr + 0x1000,
           "expected address %p, got %p\n", (char *)ptr + 0x1000, ptr2);
        status = NtUnmapViewOfSection(process, ptr2);
        ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

        /* the address is rounded down if not on a page boundary */
        ptr2 = (char *)ptr + 0x1001;
        size = 0x1000;
        offset.QuadPart = 0;
        status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, AT_ROUND_TO_PAGE, PAGE_READWRITE, NULL, 0);
        ok(status == STATUS_SUCCESS, "Unexpected status %08lx\n", status);
        ok((char *)ptr2 == (char *)ptr + 0x1000,
           "expected address %p, got %p\n", (char *)ptr + 0x1000, ptr2);
        status = NtUnmapViewOfSection(process, ptr2);
        ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

        ptr2 = (char *)ptr + 0x2000;
        size = 0x1000;
        offset.QuadPart = 0;
        status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, AT_ROUND_TO_PAGE, PAGE_READWRITE, NULL, 0);
        ok(status == STATUS_SUCCESS, "Unexpected status %08lx\n", status);
        ok((char *)ptr2 == (char *)ptr + 0x2000,
           "expected address %p, got %p\n", (char *)ptr + 0x2000, ptr2);
        status = NtUnmapViewOfSection(process, ptr2);
        ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);
    }
    else
    {
        ptr2 = (char *)ptr + 0x1000;
        size = 0;
        offset.QuadPart = 0;
        status = pNtMapViewOfSectionEx(mapping, process, &ptr2, &offset, &size, AT_ROUND_TO_PAGE, PAGE_READWRITE, NULL, 0);
        ok(status == STATUS_INVALID_PARAMETER, "NtMapViewOfSectionEx returned %08lx\n", status);
    }

    status = NtUnmapViewOfSection(process, ptr);
    ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

    /* extended parameters */

    memset(&ext, 0, sizeof(ext));
    ext[0].Type = 0;
    size = 0x1000;
    ptr = NULL;
    offset.QuadPart = 0;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    memset(&ext, 0, sizeof(ext));
    ext[0].Type = MemExtendedParameterMax;
    size = 0x1000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    memset(&a, 0, sizeof(a));
    ext[0].Type = MemExtendedParameterAddressRequirements;
    ext[0].Pointer = &a;
    size = 0x1000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(!status, "Unexpected status %08lx.\n", status);
    status = NtUnmapViewOfSection(process, ptr);
    ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

    ext[1] = ext[0];
    size = 0x1000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 2 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.LowestStartingAddress = NULL;
    a.Alignment = 0;
    a.HighestEndingAddress = (void *)(0x20001000 + 1);
    size = 0x10000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (void *)(0x20001000 - 2);
    size = 0x10000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (void *)(0x20000800 - 1);
    size = 0x10000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (char *)si.lpMaximumApplicationAddress + 0x1000;
    size = 0x10000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = (char *)si.lpMaximumApplicationAddress;
    size = 0x10000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(!status, "Unexpected status %08lx.\n", status);
    status = NtUnmapViewOfSection(process, ptr);
    ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

    a.HighestEndingAddress = (void *)(0x20001000 - 1);
    size = 0x40000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(!status, "Unexpected status %08lx.\n", status);
    ok(!((ULONG_PTR)ptr & 0xffff), "Unexpected addr %p.\n", ptr);
    ok((ULONG_PTR)ptr + size <= 0x20001000, "Unexpected addr %p.\n", ptr);
    status = NtUnmapViewOfSection(process, ptr);
    ok(status == STATUS_SUCCESS, "NtUnmapViewOfSection returned %08lx\n", status);

    size = 0x40000;
    a.HighestEndingAddress = (void *)(0x20001000 - 1);
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    a.HighestEndingAddress = NULL;
    a.Alignment = 0x30000;
    size = 0x1000;
    ptr = NULL;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
    ok(status == STATUS_INVALID_PARAMETER, "Unexpected status %08lx.\n", status);

    for (a.Alignment = 1; a.Alignment; a.Alignment *= 2)
    {
        size = 0x1000;
        ptr = NULL;
        status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READWRITE, ext, 1 );
        ok(status == STATUS_INVALID_PARAMETER, "Align %Ix unexpected status %08lx.\n", a.Alignment, status);
    }

    NtClose(mapping);

    CloseHandle(file);
    DeleteFileA(testfile);

    file = CreateFileA( "c:\\windows\\system32\\version.dll", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, 0 );
    ok( file != INVALID_HANDLE_VALUE, "Failed to open version.dll\n" );
    mapping = CreateFileMappingA( file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );

    memset(&ext, 0, sizeof(ext));
    ext[0].Type = MemExtendedParameterImageMachine;
    ext[0].ULong = 0;
    ptr = NULL;
    size = 0;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, ext, 1 );
    if (status != STATUS_INVALID_PARAMETER)
    {
        ok(status == STATUS_SUCCESS || status == STATUS_IMAGE_NOT_AT_BASE, "NtMapViewOfSection returned %08lx\n", status);
        NtUnmapViewOfSection(process, ptr);

        ext[1].Type = MemExtendedParameterImageMachine;
        ext[1].ULong = 0;
        ptr = NULL;
        size = 0;
        status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, ext, 2 );
        ok(status == STATUS_INVALID_PARAMETER, "NtMapViewOfSection returned %08lx\n", status);

        ext[0].ULong = IMAGE_FILE_MACHINE_R3000;
        ext[1].ULong = IMAGE_FILE_MACHINE_R4000;
        ptr = NULL;
        size = 0;
        status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, ext, 2 );
        ok(status == STATUS_INVALID_PARAMETER, "NtMapViewOfSection returned %08lx\n", status);

        ptr = NULL;
        size = 0;
        status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, ext, 1 );
        ok(status == STATUS_NOT_SUPPORTED, "NtMapViewOfSection returned %08lx\n", status);
    }
    else win_skip( "MemExtendedParameterImageMachine not supported\n" );

    NtClose(mapping);
    CloseHandle(file);

    TerminateProcess(process, 0);
    CloseHandle(process);
}

static void test_user_shared_data(void)
{
    struct old_xstate_configuration
    {
        ULONG64 EnabledFeatures;
        ULONG Size;
        ULONG OptimizedSave:1;
        ULONG CompactionEnabled:1;
        XSTATE_FEATURE Features[MAXIMUM_XSTATE_FEATURES];
    };

    ULONG feature_offsets[] =
    {
            0,
            160, /*offsetof(XMM_SAVE_AREA32, XmmRegisters)*/
            512  /* sizeof(XMM_SAVE_AREA32) */ + offsetof(XSTATE, YmmContext),
    };
    ULONG feature_sizes[] =
    {
            160,
            256, /*sizeof(M128A) * 16 */
            sizeof(YMMCONTEXT),
    };
    const KUSER_SHARED_DATA *user_shared_data = (void *)0x7ffe0000;
    XSTATE_CONFIGURATION xstate = user_shared_data->XState;
    ULONG64 feature_mask;
    ULONG64 supported_xstate_features = (1 << XSTATE_LEGACY_FLOATING_POINT) | (1 << XSTATE_LEGACY_SSE) | (1 << XSTATE_AVX);
    ULONG xstate_part_size = sizeof(XSTATE);
    unsigned int i;

    ok(user_shared_data->NumberOfPhysicalPages == sbi.MmNumberOfPhysicalPages,
            "Got number of physical pages %#lx, expected %#lx.\n",
            user_shared_data->NumberOfPhysicalPages, sbi.MmNumberOfPhysicalPages);

#if defined(__i386__) || defined(__x86_64__)
    ok(user_shared_data->ProcessorFeatures[PF_RDTSC_INSTRUCTION_AVAILABLE] /* Supported since Pentium CPUs. */,
            "_RDTSC not available.\n");
#endif
    ok(user_shared_data->ActiveProcessorCount == NtCurrentTeb()->Peb->NumberOfProcessors
            || broken(!user_shared_data->ActiveProcessorCount) /* before Win7 */,
            "Got unexpected ActiveProcessorCount %lu.\n", user_shared_data->ActiveProcessorCount);
    ok(user_shared_data->ActiveGroupCount == 1
            || broken(!user_shared_data->ActiveGroupCount) /* before Win7 */,
            "Got unexpected ActiveGroupCount %u.\n", user_shared_data->ActiveGroupCount);

    if (!pRtlGetEnabledExtendedFeatures)
    {
        win_skip("RtlGetEnabledExtendedFeatures is not available.\n");
        return;
    }

    feature_mask = pRtlGetEnabledExtendedFeatures(~(ULONG64)0);
    if (!feature_mask)
    {
        skip("XState features are not available.\n");
        return;
    }

    if (!xstate.EnabledFeatures)
    {
        struct old_xstate_configuration *xs_old
                = (struct old_xstate_configuration *)((char *)user_shared_data + 0x3e0);

        ok(feature_mask == xs_old->EnabledFeatures, "Got unexpected xs_old->EnabledFeatures %s.\n",
                wine_dbgstr_longlong(xs_old->EnabledFeatures));
        win_skip("Old structure layout.\n");
        return;
    }

    if (!(xstate.EnabledFeatures & (1 << XSTATE_AVX)))
    {
        trace("AVX not present\n");
        feature_offsets[2] = 0;
        feature_sizes[2] = 0;
        xstate_part_size = offsetof( XSTATE, YmmContext );
        supported_xstate_features = (1 << XSTATE_LEGACY_FLOATING_POINT) | (1 << XSTATE_LEGACY_SSE);
    }

    trace("XState EnabledFeatures %#I64x, EnabledSupervisorFeatures %#I64x, EnabledVolatileFeatures %I64x.\n",
            xstate.EnabledFeatures, xstate.EnabledSupervisorFeatures, xstate.EnabledVolatileFeatures);
    feature_mask = pRtlGetEnabledExtendedFeatures(0);
    ok(!feature_mask, "Got unexpected feature_mask %s.\n", wine_dbgstr_longlong(feature_mask));
    feature_mask = pRtlGetEnabledExtendedFeatures(~(ULONG64)0);
    ok(feature_mask == (xstate.EnabledFeatures | xstate.EnabledSupervisorFeatures), "Got unexpected feature_mask %s.\n",
            wine_dbgstr_longlong(feature_mask));
    feature_mask = pGetEnabledXStateFeatures();
    ok(feature_mask == (xstate.EnabledFeatures | xstate.EnabledSupervisorFeatures), "Got unexpected feature_mask %s.\n",
            wine_dbgstr_longlong(feature_mask));
    ok((xstate.EnabledFeatures & supported_xstate_features) == supported_xstate_features,
            "Got unexpected EnabledFeatures %s.\n", wine_dbgstr_longlong(xstate.EnabledFeatures));
    ok((xstate.EnabledVolatileFeatures & supported_xstate_features) == (xstate.EnabledFeatures & supported_xstate_features),
            "Got unexpected EnabledVolatileFeatures %s.\n", wine_dbgstr_longlong(xstate.EnabledVolatileFeatures));
    ok(xstate.Size >= 512 + xstate_part_size,
            "Got unexpected Size %lu, expected %lu.\n", xstate.Size, (ULONG)(512 + xstate_part_size));
    if (xstate.CompactionEnabled)
        ok(xstate.OptimizedSave, "Got zero OptimizedSave with compaction enabled.\n");
    ok(!xstate.AlignedFeatures, "Got unexpected AlignedFeatures %s.\n",
            wine_dbgstr_longlong(xstate.AlignedFeatures));
    ok(xstate.AllFeatureSize >= 512 + xstate_part_size
            || !xstate.AllFeatureSize /* win8 on CPUs without XSAVEC */,
            "Got unexpected AllFeatureSize %lu.\n", xstate.AllFeatureSize);

    for (i = 0; i < ARRAY_SIZE(feature_sizes); ++i)
    {
        ok(xstate.AllFeatures[i] == feature_sizes[i]
                || !xstate.AllFeatures[i] /* win8+ on CPUs without XSAVEC */,
                "Got unexpected AllFeatures[%u] %lu, expected %lu.\n", i,
                xstate.AllFeatures[i], feature_sizes[i]);
        ok(xstate.Features[i].Size == feature_sizes[i], "Got unexpected Features[%u].Size %lu, expected %lu.\n", i,
                xstate.Features[i].Size, feature_sizes[i]);
        ok(xstate.Features[i].Offset == feature_offsets[i], "Got unexpected Features[%u].Offset %lu, expected %lu.\n",
                i, xstate.Features[i].Offset, feature_offsets[i]);
    }
}

static void perform_relocations( void *module, INT_PTR delta )
{
    IMAGE_NT_HEADERS *nt;
    IMAGE_BASE_RELOCATION *rel, *end;
    const IMAGE_DATA_DIRECTORY *relocs;
    const IMAGE_SECTION_HEADER *sec;
    ULONG protect_old[96], i;

    nt = RtlImageNtHeader( module );
    relocs = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!relocs->VirtualAddress || !relocs->Size) return;
    sec = IMAGE_FIRST_SECTION( nt );
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        void *addr = (char *)module + sec[i].VirtualAddress;
        SIZE_T size = sec[i].SizeOfRawData;
        NtProtectVirtualMemory( NtCurrentProcess(), &addr,
                                &size, PAGE_READWRITE, &protect_old[i] );
    }
    rel = (IMAGE_BASE_RELOCATION *)((char *)module + relocs->VirtualAddress);
    end = (IMAGE_BASE_RELOCATION *)((char *)rel + relocs->Size);
    while (rel && rel < end - 1 && rel->SizeOfBlock)
        rel = LdrProcessRelocationBlock( (char *)module + rel->VirtualAddress,
                                         (rel->SizeOfBlock - sizeof(*rel)) / sizeof(USHORT),
                                         (USHORT *)(rel + 1), delta );
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        void *addr = (char *)module + sec[i].VirtualAddress;
        SIZE_T size = sec[i].SizeOfRawData;
        NtProtectVirtualMemory( NtCurrentProcess(), &addr,
                                &size, protect_old[i], &protect_old[i] );
    }
}


static void test_syscalls(void)
{
    HMODULE module = GetModuleHandleW( L"ntdll.dll" );
    HANDLE handle, self;
    NTSTATUS status;
    NTSTATUS (WINAPI *pNtClose)(HANDLE);
    WCHAR path[MAX_PATH];
    HANDLE file, mapping;
    SIZE_T protect_size;
    ULONG old_protect;
    INT_PTR delta;
    void *ptr, *protect_ptr;

    /* initial image */
    pNtClose = (void *)GetProcAddress( module, "NtClose" );
    handle = CreateEventW( NULL, FALSE, FALSE, NULL );
    ok( handle != 0, "CreateEventWfailed %lu\n", GetLastError() );
    status = pNtClose( handle );
    ok( !status, "NtClose failed %lx\n", status );
    status = pNtClose( handle );
    ok( status == STATUS_INVALID_HANDLE, "NtClose failed %lx\n", status );

    status = NtFlushInstructionCache( GetCurrentProcess(), NULL, 0x1234 );
    ok( !status, "NtFlushInstructionCache(NULL) failed %lx\n", status );
    status = NtFlushInstructionCache( GetCurrentProcess(), pNtClose, 0 );
    ok( !status, "zero-size NtFlushInstructionCache failed %lx\n", status );

    /* syscall thunk copy */
    ptr = VirtualAlloc( NULL, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE );
    ok( ptr != NULL, "VirtualAlloc failed\n" );
    memcpy( ptr, pNtClose, 32 );
    protect_ptr = ptr;
    protect_size = 0x1000;
    status = NtProtectVirtualMemory( GetCurrentProcess(), &protect_ptr, &protect_size,
                                     PAGE_EXECUTE_READ, &old_protect );
    ok( !status, "NtProtectVirtualMemory failed %lx\n", status );
    ok( old_protect == PAGE_EXECUTE_READWRITE, "old protection is %#lx\n", old_protect );
    status = NtFlushInstructionCache( GetCurrentProcess(), ptr, 32 );
    ok( !status, "NtFlushInstructionCache failed %lx\n", status );
    self = OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                        GetCurrentProcessId() );
    ok( !!self, "OpenProcess failed %lu\n", GetLastError() );
    if (self)
    {
        status = NtFlushInstructionCache( self, ptr, 32 );
        ok( !status, "real-self NtFlushInstructionCache failed %lx\n", status );
        NtClose( self );
    }
    pNtClose = ptr;
    handle = CreateEventW( NULL, FALSE, FALSE, NULL );
    ok( handle != 0, "CreateEventWfailed %lu\n", GetLastError() );
    status = pNtClose( handle );
    ok( !status, "NtClose failed %lx\n", status );
    status = pNtClose( handle );
    ok( status == STATUS_INVALID_HANDLE, "NtClose failed %lx\n", status );
    VirtualFree( ptr, 0, MEM_FREE );

    /* new mapping */
    GetModuleFileNameW( module, path, MAX_PATH );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0 );
    ok( file != INVALID_HANDLE_VALUE, "can't open %s: %lu\n", wine_dbgstr_w(path), GetLastError() );
    mapping = CreateFileMappingW( file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0, NULL );
    ok( mapping != NULL, "CreateFileMappingW failed err %lu\n", GetLastError() );
    ptr = MapViewOfFile( mapping, FILE_MAP_READ, 0, 0, 0 );
    ok( ptr != NULL, "MapViewOfFile failed err %lu\n", GetLastError() );
    CloseHandle( mapping );
    delta = (char *)ptr - (char *)module;

    if (memcmp( ptr, module, 0x1000 ))
    {
        skip( "modules are not identical (non-PE build?)\n" );
        UnmapViewOfFile( ptr );
        CloseHandle( file );
        return;
    }
    perform_relocations( ptr, delta );
    pNtClose = (void *)GetProcAddress( module, "NtClose" );

    if (pRtlFindExportedRoutineByName)
    {
        void *func = pRtlFindExportedRoutineByName( module, "NtClose" );
        ok( func == (void *)pNtClose, "wrong ptr %p / %p\n", func, pNtClose );
        func = pRtlFindExportedRoutineByName( ptr, "NtClose" );
        ok( (char *)func - (char *)pNtClose == delta, "wrong ptr %p / %p\n", func, pNtClose );
    }
    else win_skip( "RtlFindExportedRoutineByName not supported\n" );

    if (!memcmp( pNtClose, (char *)pNtClose + delta, 32 ))
    {
        pNtClose = (void *)((char *)pNtClose + delta);
        handle = CreateEventW( NULL, FALSE, FALSE, NULL );
        ok( handle != 0, "CreateEventWfailed %lu\n", GetLastError() );
        status = pNtClose( handle );
        ok( !status, "NtClose failed %lx\n", status );
        status = pNtClose( handle );
        ok( status == STATUS_INVALID_HANDLE, "NtClose failed %lx\n", status );
    }
    else
    {
#ifdef __i386__
        NTSTATUS (WINAPI *pNtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, void *, ULONG, ULONG *);
        PROCESS_BASIC_INFORMATION pbi;
        void *exec_mem, *va_ptr;
        ULONG size;
        BOOL ret;

        exec_mem = VirtualAlloc( NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
        ok( !!exec_mem, "got NULL.\n" );

        /* NtQueryInformationProcess is special. */
        pNtQueryInformationProcess = (void *)GetProcAddress( module, "NtQueryInformationProcess" );
        va_ptr = RtlImageRvaToVa( RtlImageNtHeader(module), module,
                                  (char *)pNtQueryInformationProcess - (char *)module, NULL );
        ok( !!va_ptr, "offset not found %p / %p\n", pNtQueryInformationProcess, module );
        ret = SetFilePointer( file, (char *)va_ptr - (char *)module, NULL, FILE_BEGIN );
        ok( ret, "got %d, err %lu.\n", ret, GetLastError() );
        ret = ReadFile( file, exec_mem, 32, NULL, NULL );
        ok( ret, "got %d, err %lu.\n", ret, GetLastError() );
        if (!memcmp( exec_mem, pNtQueryInformationProcess, 5 ))
        {
            pNtQueryInformationProcess = exec_mem;
            /* The thunk still works without relocation. */
            status = pNtQueryInformationProcess( GetCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi), &size );
            ok( !status, "got %#lx.\n", status );
            ok( size == sizeof(pbi), "got %lu.\n", size );
            ok( pbi.PebBaseAddress == NtCurrentTeb()->Peb, "got %p, %p.\n", pbi.PebBaseAddress, NtCurrentTeb()->Peb );
        }
        else
            ok( 0, "file on disk doesn't match syscall %x / %x\n",
                *(UINT *)pNtQueryInformationProcess, *(UINT *)exec_mem );

        VirtualFree( exec_mem, 0, MEM_RELEASE );
#elif defined __x86_64__
        ok( 0, "syscall thunk relocated\n" );
#else
        skip( "syscall thunk relocated\n" );
#endif
    }
    CloseHandle( file );
    UnmapViewOfFile( ptr );
}

static void test_invalid_syscalls(void)
{
    HMODULE module = GetModuleHandleW( L"ntdll.dll" );
    NTSTATUS (WINAPI *pNtImpersonateAnonymousToken)( HANDLE thread );
    NTSTATUS status;
    DWORD prot, i;
    LONG old_id, new_id, *id;

    /* grab a syscall that's unlikely to be used while we are testing */
    pNtImpersonateAnonymousToken = (void *)GetProcAddress( module, "NtImpersonateAnonymousToken" );
    if (!pNtImpersonateAnonymousToken)
    {
        win_skip( "NtImpersonateAnonymousToken not supported\n" );
        return;
    }
    status = pNtImpersonateAnonymousToken( 0 );
    ok( status == STATUS_INVALID_HANDLE || status == STATUS_NOT_IMPLEMENTED, "wrong status %lx\n", status );
    VirtualProtect( pNtImpersonateAnonymousToken, 32, PAGE_EXECUTE_READWRITE, &prot );
    for (i = 0; i < 4; i++)
    {
        new_id = 0x666 | (i << 12);
        winetest_push_context( "%04lx", new_id );
#ifdef __i386__
        id = (LONG *)((BYTE *)pNtImpersonateAnonymousToken + 1);
        new_id = (*id & ~0xffff) | new_id;
#elif defined __x86_64__
        id = (LONG *)pNtImpersonateAnonymousToken + 1;
        new_id = (*id & ~0xffff) | new_id;
#elif defined __aarch64__
        id = (LONG *)pNtImpersonateAnonymousToken;
        new_id = (*id & ~(0xffff << 5)) | (new_id << 5);
#elif defined __arm__
        id = (LONG *)(((ULONG_PTR)pNtImpersonateAnonymousToken & ~1) + 2);
        new_id = 0x0c00f240 | ((new_id & 0xff) << 16) | ((new_id & 0xf00) << 20) | (new_id >> 12); /* movw ip, #0xnnn */
#endif
        old_id = *id;
        *id = new_id;
        NtFlushInstructionCache( GetCurrentProcess(), pNtImpersonateAnonymousToken, 32 );
        status = pNtImpersonateAnonymousToken( 0 );
        ok( status == STATUS_INVALID_SYSTEM_SERVICE, "wrong status %lx\n", status );
        *id = old_id;
        NtFlushInstructionCache( GetCurrentProcess(), pNtImpersonateAnonymousToken, 32 );
        winetest_pop_context();
    }
    VirtualProtect( pNtImpersonateAnonymousToken, 32, prot, &prot );
}

struct syscall_export
{
    UINT        rva;
    int         id;
    const char *name;
};

static int CDECL sort_syscalls( const void *a, const void *b )
{
    const struct syscall_export *exp_a = a;
    const struct syscall_export *exp_b = b;
    int ret = exp_a->rva - exp_b->rva;
    if (!ret) ret = strcmp( exp_a->name, exp_b->name );
    return ret;
}

static int get_syscall_id( void *code )
{
#ifdef __i386__
    static const BYTE patterns[][18] =
    {
        { 0xb8, 0, 0, 0, 0, 0xba, 0, 0, 0, 0, 0xff, 0xd2 }, /* >= win10 */
        { 0xb8, 0, 0, 0, 0, 0xba, 0, 0, 0, 0, 0xff, 0x12 }, /* winxp */
        { 0xb8, 0, 0, 0, 0, 0x64, 0xff, 0x15, 0xc0, 0, 0, 0 },  /* nt */
        { 0xb8, 0, 0, 0, 0, 0x8d, 0x54, 0x24, 0x04, 0xcd, 0x2e }, /* nt */
        { 0xb8, 0, 0, 0, 0, 0xb9, 0, 0, 0, 0, 0x8d, 0x54, 0x24, 0x04, 0x64, 0xff, 0x15, 0xc0 }, /* vista */
        { 0xb8, 0, 0, 0, 0, 0x33, 0xc9, 0x8d, 0x54, 0x24, 0x04, 0x64, 0xff, 0x15, 0xc0 }, /* vista */
        { 0xb8, 0, 0, 0, 0, 0xe8, 0, 0, 0, 0, 0x8d, 0x54, 0x24, 0x04, 0x64, 0xff, 0x15, 0xc0 }, /* win8 */
        { 0xb8, 0, 0, 0, 0, 0xe8, 0x01, 0, 0, 0, 0xc3, 0x8b, 0xd4, 0x0f, 0x34, 0xc3 },  /* win8 */
        { 0xb8, 0, 0, 0, 0, 0xe8, 0x03, 0, 0, 0, 0xc2, 0, 0, 0x8b, 0xd4, 0x0f, 0x34, 0xc3 }, /* win8 */
    };
    const BYTE *instr = code;
    UINT i, j;

    for (i = 0; i < ARRAY_SIZE(patterns); i++)
    {
        for (j = 0; j < ARRAY_SIZE(patterns[0]); j++)
            if (patterns[i][j] && patterns[i][j] != instr[j]) break;
        if (j == ARRAY_SIZE(patterns[0]))
            return *(UINT *)(instr + 1);
    }
#elif defined __x86_64__
    static const BYTE patterns[][20] =
    {
        { 0x4c, 0x8b, 0xd1, 0xb8, 0, 0, 0, 0, 0xf6, 0x04, 0x25, 0x08, 0x03, 0xfe,
          0x7f, 0x01, 0x75, 0x03, 0x0f, 0x05 },  /* >= win10 */
        { 0x4c, 0x8b, 0xd1, 0xb8, 0, 0, 0, 0, 0x0f, 0x05, 0xc3 }, /* < win10 */
    };
    const BYTE *instr = code;
    UINT i, j;

    for (i = 0; i < ARRAY_SIZE(patterns); i++)
    {
        for (j = 0; j < ARRAY_SIZE(patterns[0]); j++)
            if (patterns[i][j] && patterns[i][j] != instr[j]) break;
        if (j == ARRAY_SIZE(patterns[0]))
            return ((UINT *)instr)[1];
    }
#elif defined __aarch64__
    const UINT *instr = code;

    if ((instr[0] & 0xffe0001f) == 0xd4000001 && instr[1] == 0xd65f03c0)  /* windows */
        return (instr[0] >> 5) & 0xffff;
    if ((instr[0] & 0xffe0001f) == 0xd2800008 && instr[1] == 0xaa1e03e9 &&
        instr[3] == 0xf9400210 && instr[4] == 0xd63f0200 && instr[5] == 0xd65f03c0) /* wine */
        return (instr[0] >> 5) & 0xffff;
#elif defined __arm__
    const USHORT *instr = code;

    if (instr[0] == 0xb40f && (instr[2] & 0x0f00) == 0x0c00 &&
        ((instr[3] == 0xdef8 && instr[4] == 0xb004 && instr[5] == 0x4770) ||  /* windows */
         (instr[3] == 0x4673 && instr[6] == 0xb004 && instr[7] == 0x4770)))  /* wine */
    {
        USHORT imm = ((instr[1] & 0x400) << 1) | (instr[2] & 0xff) | ((instr[2] >> 4) & 0x0700);
        if ((instr[1] & 0xfbf0) == 0xf240)  /* T3 */
        {
            return imm | (instr[1] & 0x0f) << 12;
        }
        else if ((instr[1] & 0xfbf0) == 0xf040)  /* T2 */
        {
            switch (imm >> 8)
            {
            case 0: return imm;
            case 1: return (imm & 0xff);
            case 2: return (imm & 0xff) << 8;
            case 3: return (imm & 0xff) | ((imm & 0xff) << 8);
            default: return (0x80 | (imm & 0x7f)) << (32 - (imm >> 7));
            }
        }
    }
#endif
    return -1;
}

static void test_syscall_numbers(void)
{
    struct syscall_export syscalls[4096];
    HMODULE module = GetModuleHandleA( "ntdll.dll" );
    IMAGE_EXPORT_DIRECTORY *exports;
    ULONG size;
    int pos;
    const WORD *ordinals;
    const DWORD *names, *functions;
    static const char *prefix[] = { "Nt", "Zw" };

    exports = RtlImageDirectoryEntryToData( module, TRUE, IMAGE_DIRECTORY_ENTRY_EXPORT, &size );
    names = get_rva( module, exports->AddressOfNames );
    ordinals = get_rva( module, exports->AddressOfNameOrdinals );
    functions = get_rva( module, exports->AddressOfFunctions );

    for (unsigned int test = 0; test < ARRAY_SIZE(prefix); test++)
    {
        for (int i = pos = 0; i < exports->NumberOfNames; i++)
        {
            char *name = get_rva( module, names[i] );
            if (strncmp( name, prefix[test], strlen(prefix[test]) )) continue;
            if (!strcmp( name, "NtGetTickCount" ) ||
                !strcmp( name, "NtCurrentTeb" ) ||
                !strncmp( name, "NtdllDialogWndProc", 18 ) ||
                !strncmp( name, "NtdllDefWindowProc", 18 ))
                continue;  /* these are special */
            syscalls[pos].rva  = functions[ordinals[i]];
#ifdef __arm__
            syscalls[pos].rva &= ~1; /* thumb */
#endif
            syscalls[pos].id   = get_syscall_id( get_rva( module, syscalls[pos].rva ));
            syscalls[pos].name = name;
            pos++;
        }
        ok( pos, "no syscalls found\n" );
        qsort( syscalls, pos, sizeof(*syscalls), sort_syscalls );
        for (int i = 0, expect = 0; i < pos; i++, expect++)
        {
            if (syscalls[i].id == -1)
            {
                /* these may not be real syscalls */
                ok( !strcmp( syscalls[i].name + strlen(prefix[test]), "QuerySystemTime" ) ||
                    !strcmp( syscalls[i].name + strlen(prefix[test]), "QueryInformationProcess" ),
                    "not a syscall %04x %s\n", i, syscalls[i].name );
            }
            else if (LOWORD(syscalls[i].id) > expect)
            {
                ok( 0, "missing syscall %04x\n", expect );
                i--;
            }
            else
            {
                ok( LOWORD(syscalls[i].id) == expect, "wrong id %04x / %04x for %s\n",
                    syscalls[i].id, expect, syscalls[i].name );
            }
        }
    }
}

static void test_syscall_abi(void)
{
    LCID user_lcid, system_lcid, prev_user_lcid = 0, lcid;
    NTSTATUS (WINAPI *pNtQueryDefaultLocale)( ULONG user, LCID *lcid );

    /* test that BOOLEAN values are correctly extended */

    pNtQueryDefaultLocale = (void *)GetProcAddress( GetModuleHandleA("ntdll.dll"), "NtQueryDefaultLocale" );
    pNtQueryDefaultLocale( 0, &system_lcid );
    pNtQueryDefaultLocale( 1, &user_lcid );
    if (system_lcid == user_lcid)
    {
        prev_user_lcid = user_lcid;
        user_lcid += 0x400;
        NtSetDefaultLocale( TRUE, user_lcid );
    }
    pNtQueryDefaultLocale( 0, &lcid );
    ok( lcid == system_lcid, "got %04lx / %04lx\n", lcid, system_lcid );
    for (ULONG i = 1; i < 0x10000; i <<= 1)
    {
        LCID expect = LOBYTE(i) ? user_lcid : system_lcid;
        pNtQueryDefaultLocale( i, &lcid );
        ok( lcid == expect, "%lx: got %04lx / %04lx\n", i, lcid, expect );
    }
    if (prev_user_lcid) NtSetDefaultLocale( TRUE, prev_user_lcid );
}

static void test_NtFreeVirtualMemory(void)
{
    void *addr1, *addr;
    NTSTATUS status;
    SIZE_T size;

    size = 0x10000;
    addr1 = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr1, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    size = 0;
    status = NtFreeVirtualMemory(NULL, &addr1, &size, MEM_RELEASE);
    ok(status == STATUS_INVALID_HANDLE, "Unexpected status %08lx.\n", status);

    addr = (char *)addr1 + 0x1000;
    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(status == STATUS_FREE_VM_NOT_AT_BASE, "Unexpected status %08lx.\n", status);

    size = 0x11000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr1, &size, MEM_RELEASE);
    ok(status == STATUS_UNABLE_TO_FREE_VM, "Unexpected status %08lx.\n", status);

    addr = (char *)addr1 + 0x1001;
    size = 0xffff;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(status == STATUS_UNABLE_TO_FREE_VM, "Unexpected status %08lx.\n", status);
    ok(size == 0xffff, "Unexpected size %p.\n", (void *)size);
    ok(addr == (char *)addr1 + 0x1001, "Got addr %p, addr1 %p.\n", addr, addr1);

    size = 0xfff;
    addr = (char *)addr1 + 0x1001;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    *(volatile char *)addr1 = 1;
    *((volatile char *)addr1 + 0x2000) = 1;
    ok(size == 0x1000, "Unexpected size %p.\n", (void *)size);
    ok(addr == (char *)addr1 + 0x1000, "Got addr %p, addr1 %p.\n", addr, addr1);

    size = 0xfff;
    addr = (char *)addr1 + 1;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    *((volatile char *)addr1 + 0x2000) = 1;
    ok(size == 0x1000, "Unexpected size %p.\n", (void *)size);
    ok(addr == addr1, "Got addr %p, addr1 %p.\n", addr, addr1);

    size = 0x1000;
    addr = addr1;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(addr == addr1, "Unexpected addr %p, addr1 %p.\n", addr, addr1);
    ok(size == 0x1000, "Unexpected size %p.\n", (void *)size);

    size = 0x10000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr1, &size, MEM_DECOMMIT);
    ok(status == STATUS_UNABLE_TO_FREE_VM, "Unexpected status %08lx.\n", status);

    size = 0x10000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr1, &size, MEM_RELEASE);
    ok(status == STATUS_UNABLE_TO_FREE_VM, "Unexpected status %08lx.\n", status);

    size = 0;
    addr = (char *)addr1 + 0x1000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(status == STATUS_MEMORY_NOT_ALLOCATED, "Unexpected status %08lx.\n", status);

    size = 0x1000;
    addr = (char *)addr1 + 0x1000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_DECOMMIT);
    ok(status == STATUS_MEMORY_NOT_ALLOCATED, "Unexpected status %08lx.\n", status);

    size = 0;
    addr = (char *)addr1 + 0x2000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    size = 0x1000;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr1, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
}

static void test_NtProtectVirtualMemory(void)
{
    void *addr, *addr2;
    NTSTATUS status;
    SIZE_T size;
    DWORD old_prot;

    size = page_size * 16;
    addr = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    old_prot = 0;
    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_READONLY, &old_prot);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(old_prot == PAGE_READWRITE, "Unexpected old_prot %lx.\n", old_prot);

    status = NtProtectVirtualMemory(NULL, &addr, &size, PAGE_READONLY, &old_prot);
    ok(status == STATUS_INVALID_HANDLE, "Unexpected status %08lx.\n", status);

    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_READONLY, NULL);
    ok(status == STATUS_ACCESS_VIOLATION, "Unexpected status %08lx.\n", status);

    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, 0, &old_prot);
    ok(status == STATUS_INVALID_PAGE_PROTECTION, "Unexpected status %08lx.\n", status);

    size = page_size * 8;
    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_READWRITE, &old_prot);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(old_prot == PAGE_READONLY, "Unexpected old_prot %lx.\n", old_prot);
    addr = (char *)addr + page_size * 8;
    old_prot = 0;
    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_EXECUTE_READWRITE, &old_prot);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(old_prot == PAGE_READONLY, "Unexpected old_prot %lx.\n", old_prot);
    addr = (char *)addr - page_size * 8;
    size = page_size * 16;
    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_EXECUTE_READ, &old_prot);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(old_prot == PAGE_READWRITE, "Unexpected old_prot %lx.\n", old_prot);

    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    addr = NULL;
    size = page_size;
    old_prot = 0;
    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_READONLY, &old_prot);
    /* todo: STATUS_INVALID_PARAMETER in wine, STATUS_CONFLICTING_ADDRESSES in win64 */
    ok(status, "Unexpected status %08lx.\n", status);
    ok(old_prot == PAGE_NOACCESS || broken(old_prot) /* win7 */, "Unexpected old_prot %lx.\n", old_prot);

    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    old_prot = 0;
    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_READONLY, &old_prot);
    ok(status == STATUS_NOT_COMMITTED, "Unexpected status %08lx.\n", status);
    ok(old_prot == PAGE_NOACCESS || broken(old_prot) /* win7 */, "Unexpected old_prot %lx.\n", old_prot);
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    addr2 = addr;
    addr = (char *)addr + 1;
    size = 1;
    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_READONLY, &old_prot);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(old_prot == PAGE_READWRITE, "Unexpected old_prot %lx.\n", old_prot);
    ok(size == page_size, "Unexpected size %p.\n",  (void *)size);
    ok(addr == addr2, "Got addr %p, addr2 %p.\n", addr, addr2);

    status = NtFreeVirtualMemory(NtCurrentProcess(), &addr, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
}

static void test_prefetch(void)
{
    NTSTATUS status;
    MEMORY_RANGE_ENTRY entries[2] = {{ 0 }};
    ULONG reservedarg = 0;
    char stackmem[] = "Test stack mem";
    static char testmem[] = "Test memory range data";

    if (!pNtSetInformationVirtualMemory)
    {
        skip("no NtSetInformationVirtualMemory in ntdll\n");
        return;
    }

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), -1, 1, entries, NULL, 32);
    ok( status == STATUS_INVALID_PARAMETER_2,
        "NtSetInformationVirtualMemory unexpected status on invalid info class (1): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), -1, 0, NULL, NULL, 0);
    ok( status == STATUS_INVALID_PARAMETER_2 || (is_wow64 && status == STATUS_INVALID_PARAMETER_3),
        "NtSetInformationVirtualMemory unexpected status on invalid info class (2): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), -1, 1, NULL, NULL, 32);
    ok( status == STATUS_INVALID_PARAMETER_2 || (is_wow64 && status == STATUS_ACCESS_VIOLATION),
        "NtSetInformationVirtualMemory unexpected status on invalid info class (3): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, entries, NULL, 0 );
    ok( status == STATUS_INVALID_PARAMETER_5 ||
        broken( is_wow64 && status == STATUS_INVALID_PARAMETER_6 ) /* win10 1507 */,
        "NtSetInformationVirtualMemory unexpected status on NULL info data (1): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, NULL, NULL, 0 );
    ok( status == STATUS_INVALID_PARAMETER_5 || (is_wow64 && status == STATUS_ACCESS_VIOLATION),
        "NtSetInformationVirtualMemory unexpected status on NULL info data (2): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             0, NULL, NULL, 0 );
    ok( status == STATUS_INVALID_PARAMETER_5 || (is_wow64 && status == STATUS_INVALID_PARAMETER_3),
        "NtSetInformationVirtualMemory unexpected status on NULL info data (3): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, entries, &reservedarg, sizeof(reservedarg) * 2 );
    ok( status == STATUS_INVALID_PARAMETER_6,
        "NtSetInformationVirtualMemory unexpected status on extended info data (1): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             0, NULL, &reservedarg, sizeof(reservedarg) * 2 );
    ok( status == STATUS_INVALID_PARAMETER_6 || (is_wow64 && status == STATUS_INVALID_PARAMETER_3),
        "NtSetInformationVirtualMemory unexpected status on extended info data (2): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, entries, &reservedarg, sizeof(reservedarg) / 2 );
    ok( status == STATUS_INVALID_PARAMETER_6,
        "NtSetInformationVirtualMemory unexpected status on shrunk info data (1): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             0, NULL, &reservedarg, sizeof(reservedarg) / 2 );
    ok( status == STATUS_INVALID_PARAMETER_6 || (is_wow64 && status == STATUS_INVALID_PARAMETER_3),
        "NtSetInformationVirtualMemory unexpected status on shrunk info data (2): %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             0, NULL, &reservedarg, sizeof(reservedarg) );
    ok( status == STATUS_INVALID_PARAMETER_3,
        "NtSetInformationVirtualMemory unexpected status on 0 entries: %08lx\n", status);

    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, NULL, &reservedarg, sizeof(reservedarg) );
    ok( status == STATUS_ACCESS_VIOLATION,
        "NtSetInformationVirtualMemory unexpected status on NULL entries: %08lx\n", status);

    entries[0].VirtualAddress = NULL;
    entries[0].NumberOfBytes = 0;
    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, entries, &reservedarg, sizeof(reservedarg) );
    ok( status == STATUS_INVALID_PARAMETER_4 ||
        broken( is_wow64 && status == STATUS_INVALID_PARAMETER_6 ) /* win10 1507 */,
        "NtSetInformationVirtualMemory unexpected status on 1 empty entry: %08lx\n", status);

    entries[0].VirtualAddress = NULL;
    entries[0].NumberOfBytes = page_size;
    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, entries, &reservedarg, sizeof(reservedarg) );
    ok( status == STATUS_SUCCESS ||
        broken( is_wow64 && status == STATUS_INVALID_PARAMETER_6 ) /* win10 1507 */,
        "NtSetInformationVirtualMemory unexpected status on 1 NULL address entry: %08lx\n", status);

    entries[0].VirtualAddress = (void *)((ULONG_PTR)testmem & -(ULONG_PTR)page_size);
    entries[0].NumberOfBytes = page_size;
    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, entries, &reservedarg, sizeof(reservedarg) );
    ok( status == STATUS_SUCCESS ||
        broken( is_wow64 && status == STATUS_INVALID_PARAMETER_6 ) /* win10 1507 */,
        "NtSetInformationVirtualMemory unexpected status on 1 page-aligned entry: %08lx\n", status);

    entries[0].VirtualAddress = testmem;
    entries[0].NumberOfBytes = sizeof(testmem);
    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, entries, &reservedarg, sizeof(reservedarg) );
    ok( status == STATUS_SUCCESS ||
        broken( is_wow64 && status == STATUS_INVALID_PARAMETER_6 ) /* win10 1507 */,
        "NtSetInformationVirtualMemory unexpected status on 1 entry: %08lx\n", status);

    entries[0].VirtualAddress = NULL;
    entries[0].NumberOfBytes = page_size;
    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             1, entries, &reservedarg, sizeof(reservedarg) );
    ok( status == STATUS_SUCCESS ||
        broken( is_wow64 && status == STATUS_INVALID_PARAMETER_6 ) /* win10 1507 */,
        "NtSetInformationVirtualMemory unexpected status on 1 unmapped entry: %08lx\n", status);

    entries[0].VirtualAddress = (void *)((ULONG_PTR)testmem & -(ULONG_PTR)page_size);
    entries[0].NumberOfBytes = page_size;
    entries[1].VirtualAddress = (void *)((ULONG_PTR)stackmem & -(ULONG_PTR)page_size);
    entries[1].NumberOfBytes = page_size;
    status = pNtSetInformationVirtualMemory( NtCurrentProcess(), VmPrefetchInformation,
                                             2, entries, &reservedarg, sizeof(reservedarg) );
    ok( status == STATUS_SUCCESS ||
        broken( is_wow64 && status == STATUS_INVALID_PARAMETER_6 ) /* win10 1507 */,
        "NtSetInformationVirtualMemory unexpected status on 2 page-aligned entries: %08lx\n", status);
}

static void test_query_region_information(void)
{
    MEMORY_REGION_INFORMATION info;
    LARGE_INTEGER offset;
    SIZE_T len, size;
    NTSTATUS status;
    HANDLE mapping;
    void *ptr, *addr;
    ULONG old;

    size = 0x10000;
    ptr = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &ptr, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

#ifdef _WIN64
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info,
            FIELD_OFFSET(MEMORY_REGION_INFORMATION, PartitionId), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info,
            FIELD_OFFSET(MEMORY_REGION_INFORMATION, CommitSize), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info,
            FIELD_OFFSET(MEMORY_REGION_INFORMATION, RegionSize), &len);
    ok(status == STATUS_INFO_LENGTH_MISMATCH, "Unexpected status %08lx.\n", status);
#endif

    len = 0;
    memset(&info, 0x11, sizeof(info));
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(len == sizeof(info) ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId)) /* <= Win10-1909 */ ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, CommitSize)) /* Win7 */,
       "Unexpected len %Ix\n", len);
    ok(info.AllocationBase == ptr, "Unexpected base %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == PAGE_READWRITE, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(!info.Private, "Unexpected flag %d.\n", info.Private);
    ok(!info.MappedDataFile, "Unexpected flag %d.\n", info.MappedDataFile);
    ok(!info.MappedImage, "Unexpected flag %d.\n", info.MappedImage);
    ok(!info.MappedPageFile, "Unexpected flag %d.\n", info.MappedPageFile);
    ok(!info.MappedPhysical, "Unexpected flag %d.\n", info.MappedPhysical);
    ok(!info.DirectMapped, "Unexpected flag %d.\n", info.DirectMapped);
    ok(info.RegionSize == size, "Unexpected region size.\n");
    if (len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId))
        ok(!info.CommitSize, "Unexpected commit size %#Ix.\n", info.CommitSize);

    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &ptr, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    /* Committed size */
    size = 0x10000;
    ptr = NULL;
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &ptr, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    len = 0;
    memset(&info, 0x11, sizeof(info));
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(len == sizeof(info) ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId)) /* <= Win10-1909 */ ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, CommitSize)) /* Win7 */,
       "Unexpected len %Ix\n", len);
    ok(info.AllocationBase == ptr, "Unexpected base %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == PAGE_READWRITE, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(!info.Private, "Unexpected flag %d.\n", info.Private);
    ok(!info.MappedDataFile, "Unexpected flag %d.\n", info.MappedDataFile);
    ok(!info.MappedImage, "Unexpected flag %d.\n", info.MappedImage);
    ok(!info.MappedPageFile, "Unexpected flag %d.\n", info.MappedPageFile);
    ok(!info.MappedPhysical, "Unexpected flag %d.\n", info.MappedPhysical);
    ok(!info.DirectMapped, "Unexpected flag %d.\n", info.DirectMapped);
    ok(info.RegionSize == size, "Unexpected region size.\n");
    if (len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId))
        ok(info.CommitSize == size, "Unexpected commit size %#Ix.\n", info.CommitSize);

    len = 0;
    addr = (char *)ptr + 0x1000;
    size = 0x1000;
    status = NtProtectVirtualMemory(NtCurrentProcess(), &addr, &size, PAGE_NOACCESS, &old );
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(len == sizeof(info) ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId)) /* <= Win10-1909 */ ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, CommitSize)) /* Win7 */,
       "Unexpected len %Ix\n", len);
    ok(info.AllocationBase == ptr, "Unexpected base %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == PAGE_READWRITE, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(!info.Private, "Unexpected flag %d.\n", info.Private);
    ok(!info.MappedDataFile, "Unexpected flag %d.\n", info.MappedDataFile);
    ok(!info.MappedImage, "Unexpected flag %d.\n", info.MappedImage);
    ok(!info.MappedPageFile, "Unexpected flag %d.\n", info.MappedPageFile);
    ok(!info.MappedPhysical, "Unexpected flag %d.\n", info.MappedPhysical);
    ok(!info.DirectMapped, "Unexpected flag %d.\n", info.DirectMapped);
    ok(info.RegionSize == 0x10000, "Unexpected region size %#Ix.\n", info.RegionSize);
    if (len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId))
        ok(info.CommitSize == 0x10000, "Unexpected commit size %#Ix.\n", info.CommitSize);

    len = 0;
    status = NtQueryVirtualMemory(NtCurrentProcess(), (char *)ptr + 0x1000, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(len == sizeof(info) ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId)) /* <= Win10-1909 */ ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, CommitSize)) /* Win7 */,
       "Unexpected len %Ix\n", len);
    ok(info.AllocationBase == ptr, "Unexpected base %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == PAGE_READWRITE, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(!info.Private, "Unexpected flag %d.\n", info.Private);
    ok(!info.MappedDataFile, "Unexpected flag %d.\n", info.MappedDataFile);
    ok(!info.MappedImage, "Unexpected flag %d.\n", info.MappedImage);
    ok(!info.MappedPageFile, "Unexpected flag %d.\n", info.MappedPageFile);
    ok(!info.MappedPhysical, "Unexpected flag %d.\n", info.MappedPhysical);
    ok(!info.DirectMapped, "Unexpected flag %d.\n", info.DirectMapped);
    ok(info.RegionSize == 0x10000, "Unexpected region size %#Ix.\n", info.RegionSize);
    if (len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId))
        ok(info.CommitSize == 0x10000, "Unexpected commit size %#Ix.\n", info.CommitSize);

    size = 0;
    status = NtFreeVirtualMemory(NtCurrentProcess(), &ptr, &size, MEM_RELEASE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    memset(&info, 0xcc, sizeof(info));
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_INVALID_ADDRESS, "Unexpected status %08lx.\n", status);
    ok(info.AllocationBase == (void *)(ULONG_PTR)0xcccccccccccccccc, "got %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == 0xcccccccc, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(info.RegionType == 0xcccccccc, "got %#lx.\n", info.RegionType);

    /* Pagefile mapping */
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT, 0, 4096, NULL);
    ok(mapping != 0, "CreateFileMapping failed\n");

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, NtCurrentProcess(), &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READONLY);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    len = 0;
    memset(&info, 0x11, sizeof(info));
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(len == sizeof(info) ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId)) /* <= Win10-1909 */ ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, CommitSize)) /* Win7 */,
       "Unexpected len %Ix\n", len);
    ok(info.AllocationBase == ptr, "Unexpected base %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == PAGE_READONLY, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(!info.Private, "Unexpected flag %d.\n", info.Private);
    ok(!info.MappedDataFile, "Unexpected flag %d.\n", info.MappedDataFile);
    ok(!info.MappedImage, "Unexpected flag %d.\n", info.MappedImage);
    ok(!info.MappedPageFile, "Unexpected flag %d.\n", info.MappedPageFile);
    ok(!info.MappedPhysical, "Unexpected flag %d.\n", info.MappedPhysical);
    ok(!info.DirectMapped, "Unexpected flag %d.\n", info.DirectMapped);
    ok(info.RegionSize == 4096, "Unexpected region size.\n");
    if (len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId))
        ok(!info.CommitSize, "Unexpected commit size %#Ix.\n", info.CommitSize);

    status = NtUnmapViewOfSection(NtCurrentProcess(), ptr);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, NtCurrentProcess(), &ptr, 0, 0, &offset, &size, 1, 0, PAGE_WRITECOPY);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    len = 0;
    memset(&info, 0x11, sizeof(info));
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(len == sizeof(info) ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId)) /* <= Win10-1909 */ ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, CommitSize)) /* Win7 */,
       "Unexpected len %Ix\n", len);
    ok(info.AllocationBase == ptr, "Unexpected base %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == PAGE_WRITECOPY, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(!info.Private, "Unexpected flag %d.\n", info.Private);
    ok(!info.MappedDataFile, "Unexpected flag %d.\n", info.MappedDataFile);
    ok(!info.MappedImage, "Unexpected flag %d.\n", info.MappedImage);
    ok(!info.MappedPageFile, "Unexpected flag %d.\n", info.MappedPageFile);
    ok(!info.MappedPhysical, "Unexpected flag %d.\n", info.MappedPhysical);
    ok(!info.DirectMapped, "Unexpected flag %d.\n", info.DirectMapped);
    ok(info.RegionSize == 4096, "Unexpected region size.\n");
    if (len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId))
        ok(info.CommitSize == 4096, "Unexpected commit size %#Ix.\n", info.CommitSize);

    len = 0;
    *(volatile int *)ptr = 1;
    memset(&info, 0x11, sizeof(info));
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(len == sizeof(info) ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId)) /* <= Win10-1909 */ ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, CommitSize)) /* Win7 */,
       "Unexpected len %Ix\n", len);
    ok(info.AllocationBase == ptr, "Unexpected base %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == PAGE_WRITECOPY, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(!info.Private, "Unexpected flag %d.\n", info.Private);
    ok(!info.MappedDataFile, "Unexpected flag %d.\n", info.MappedDataFile);
    ok(!info.MappedImage, "Unexpected flag %d.\n", info.MappedImage);
    ok(!info.MappedPageFile, "Unexpected flag %d.\n", info.MappedPageFile);
    ok(!info.MappedPhysical, "Unexpected flag %d.\n", info.MappedPhysical);
    ok(!info.DirectMapped, "Unexpected flag %d.\n", info.DirectMapped);
    ok(info.RegionSize == 4096, "Unexpected region size.\n");
    if (len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId))
        ok(info.CommitSize == 4096, "Unexpected commit size %#Ix.\n", info.CommitSize);

    status = NtUnmapViewOfSection(NtCurrentProcess(), ptr);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection(mapping, NtCurrentProcess(), &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READWRITE);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    *(volatile int *)ptr = 1;

    len = 0;
    memset(&info, 0x11, sizeof(info));
    status = NtQueryVirtualMemory(NtCurrentProcess(), ptr, MemoryRegionInformation, &info, sizeof(info), &len);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);
    ok(len == sizeof(info) ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId)) /* <= Win10-1909 */ ||
       broken(len >= offsetof(MEMORY_REGION_INFORMATION, CommitSize)) /* Win7 */,
       "Unexpected len %Ix\n", len);
    ok(info.AllocationBase == ptr, "Unexpected base %p.\n", info.AllocationBase);
    ok(info.AllocationProtect == PAGE_READWRITE, "Unexpected protection %lu.\n", info.AllocationProtect);
    ok(!info.Private, "Unexpected flag %d.\n", info.Private);
    ok(!info.MappedDataFile, "Unexpected flag %d.\n", info.MappedDataFile);
    ok(!info.MappedImage, "Unexpected flag %d.\n", info.MappedImage);
    ok(!info.MappedPageFile, "Unexpected flag %d.\n", info.MappedPageFile);
    ok(!info.MappedPhysical, "Unexpected flag %d.\n", info.MappedPhysical);
    ok(!info.DirectMapped, "Unexpected flag %d.\n", info.DirectMapped);
    ok(info.RegionSize == 4096, "Unexpected region size.\n");
    if (len >= offsetof(MEMORY_REGION_INFORMATION, PartitionId))
        ok(!info.CommitSize, "Unexpected commit size %#Ix.\n", info.CommitSize);

    status = NtUnmapViewOfSection(NtCurrentProcess(), ptr);
    ok(status == STATUS_SUCCESS, "Unexpected status %08lx.\n", status);

    NtClose(mapping);
}

static void test_query_image_information(void)
{
    MEMORY_IMAGE_INFORMATION info;
    IMAGE_NT_HEADERS *nt;
    LARGE_INTEGER offset;
    SIZE_T len, size;
    NTSTATUS status;
    HANDLE mapping, file;
    void *ptr;

    /* virtual allocation */

    size = 0x8000;
    ptr = NULL;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &ptr, 0, &size, MEM_RESERVE, PAGE_READWRITE );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );

    len = 0xdead;
    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), ptr, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    if (status == STATUS_INVALID_INFO_CLASS)
    {
        win_skip( "MemoryImageInformation not supported\n" );
        NtUnmapViewOfSection( NtCurrentProcess(), ptr );
        return;
    }
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( len == sizeof(info), "wrong len %Ix\n", len );
    ok( !info.ImageBase, "wrong image base %p/%p\n", info.ImageBase, ptr );
    ok( !info.SizeOfImage, "wrong size %Ix/%Ix\n", info.SizeOfImage, size );
    ok( !info.ImageFlags, "wrong flags %lx\n", info.ImageFlags );

    len = 0xdead;
    status = NtQueryVirtualMemory( NtCurrentProcess(), ptr, MemoryImageInformation,
                                   &info, sizeof(info) + 2, &len );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( len == sizeof(info), "wrong len %Ix\n", len );

    len = 0xdead;
    status = NtQueryVirtualMemory( NtCurrentProcess(), ptr, MemoryImageInformation,
                                   &info, sizeof(info) - 1, &len );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "Unexpected status %08lx\n", status );
    ok( len == 0xdead, "wrong len %Ix\n", len );

    len = 0xdead;
    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)ptr + size, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    ok( status == STATUS_INVALID_ADDRESS, "Unexpected status %08lx\n", status );
    ok( len == 0xdead || broken(len == sizeof(info)), "wrong len %Ix\n", len );

    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)ptr + 0x1234, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( !info.ImageBase, "wrong image base %p/%p\n", info.ImageBase, ptr );
    ok( !info.SizeOfImage, "wrong size %Ix/%Ix\n", info.SizeOfImage, size );
    ok( !info.ImageFlags, "wrong flags %lx\n", info.ImageFlags );

    size = 0;
    NtFreeVirtualMemory( NtCurrentProcess(), &ptr, &size, MEM_RELEASE );

    /* mapped dll */

    ptr = GetModuleHandleA( "ntdll.dll" );
    nt = RtlImageNtHeader( ptr );
    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)ptr + 0x1234, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( info.ImageBase == ptr, "wrong image base %p/%p\n", info.ImageBase, ptr );
    ok( info.SizeOfImage == nt->OptionalHeader.SizeOfImage, "wrong size %Ix/%x\n",
        info.SizeOfImage, (UINT)nt->OptionalHeader.SizeOfImage );
    ok( !info.ImagePartialMap, "wrong partial map\n" );
    ok( !info.ImageNotExecutable, "wrong not executable\n" );
    ok( info.ImageSigningLevel == 0 || info.ImageSigningLevel == 12,
        "wrong signing level %u\n", info.ImageSigningLevel );

    /* image mapping */

    file = CreateFileA( "c:\\windows\\system32\\kernel32.dll", GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, 0 );
    mapping = CreateFileMappingA( file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READONLY );
    ok( status == STATUS_IMAGE_NOT_AT_BASE, "Unexpected status %08lx\n", status );
    NtClose( mapping );

    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)ptr + 0x1234, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( info.ImageBase == ptr, "wrong image base %p/%p\n", info.ImageBase, ptr );
    ok( info.SizeOfImage == size, "wrong size %Ix/%Ix\n", info.SizeOfImage, size );
    ok( !info.ImagePartialMap, "wrong partial map\n" );
    ok( !info.ImageNotExecutable, "wrong not executable\n" );
    ok( info.ImageSigningLevel == 0 || info.ImageSigningLevel == 12,
        "wrong signing level %u\n", info.ImageSigningLevel );

    NtUnmapViewOfSection( NtCurrentProcess(), ptr );

    /* partial image mapping */

    file = CreateFileA( "c:\\windows\\system32\\kernel32.dll", GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, 0 );
    mapping = CreateFileMappingA( file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0x4000, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READONLY );
    ok( status == STATUS_IMAGE_NOT_AT_BASE, "Unexpected status %08lx\n", status );
    todo_wine
    ok( size == 0x4000, "wrong size %Ix\n", size );
    NtClose( mapping );

    nt = RtlImageNtHeader( ptr );
    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)ptr + 0x1234, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( info.ImageBase == ptr, "wrong image base %p/%p\n", info.ImageBase, ptr );
    ok( info.SizeOfImage == nt->OptionalHeader.SizeOfImage, "wrong size %Ix/%x\n",
        info.SizeOfImage, (UINT)nt->OptionalHeader.SizeOfImage );
    todo_wine
    ok( info.ImagePartialMap, "wrong partial map\n" );
    ok( !info.ImageNotExecutable, "wrong not executable\n" );
    ok( info.ImageSigningLevel == 0 || info.ImageSigningLevel == 12,
        "wrong signing level %u\n", info.ImageSigningLevel );

    NtUnmapViewOfSection( NtCurrentProcess(), ptr );

    file = CreateFileA( "c:\\windows\\system32\\kernel32.dll", GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, 0, 0 );
    mapping = CreateFileMappingA( file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );

    ptr = NULL;
    size = 0x5000;
    offset.QuadPart = 0;
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READONLY );
    ok( status == STATUS_IMAGE_NOT_AT_BASE, "Unexpected status %08lx\n", status );
    todo_wine
    ok( size == 0x5000, "wrong size %Ix\n", size );
    NtClose( mapping );

    nt = RtlImageNtHeader( ptr );
    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)ptr + 0x1234, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( info.ImageBase == ptr, "wrong image base %p/%p\n", info.ImageBase, ptr );
    ok( info.SizeOfImage == nt->OptionalHeader.SizeOfImage, "wrong size %Ix/%x\n",
        info.SizeOfImage, (UINT)nt->OptionalHeader.SizeOfImage );
    todo_wine
    ok( info.ImagePartialMap, "wrong partial map\n" );
    ok( !info.ImageNotExecutable, "wrong not executable\n" );
    ok( info.ImageSigningLevel == 0 || info.ImageSigningLevel == 12,
        "wrong signing level %u\n", info.ImageSigningLevel );

    NtUnmapViewOfSection( NtCurrentProcess(), ptr );

    /* non-image mapping */

    mapping = CreateFileMappingA( file, NULL, PAGE_READONLY, 0, 0x10000, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READONLY );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    NtClose( mapping );

    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)ptr + 0x1234, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( !info.ImageBase, "wrong image base %p/%p\n", info.ImageBase, ptr );
    ok( !info.SizeOfImage, "wrong size %Ix/%Ix\n", info.SizeOfImage, size );
    ok( !info.ImageFlags, "wrong flags %lx\n", info.ImageFlags );

    NtUnmapViewOfSection( NtCurrentProcess(), ptr );

    /* pagefile mapping */

    mapping = CreateFileMappingA( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x10000, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );

    ptr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection( mapping, NtCurrentProcess(), &ptr, 0, 0, &offset, &size, 1, 0, PAGE_READONLY );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    NtClose( mapping );

    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (char *)ptr + 0x1234, MemoryImageInformation,
                                   &info, sizeof(info), &len );
    ok( status == STATUS_SUCCESS, "Unexpected status %08lx\n", status );
    ok( !info.ImageBase, "wrong image base %p/%p\n", info.ImageBase, ptr );
    ok( !info.SizeOfImage, "wrong size %Ix/%Ix\n", info.SizeOfImage, size );
    ok( !info.ImageFlags, "wrong flags %lx\n", info.ImageFlags );

    NtUnmapViewOfSection( NtCurrentProcess(), ptr );
    NtClose( file );
}

static int *write_addr;
static int got_exception;

static LONG CALLBACK exec_write_handler( EXCEPTION_POINTERS *ptrs )
{
    MANAGE_WRITES_TO_EXECUTABLE_MEMORY mem = { .Version = 2, .ThreadAllowWrites = 1 };
    EXCEPTION_RECORD *rec = ptrs->ExceptionRecord;
    NTSTATUS status;

    got_exception++;
    ok( rec->ExceptionCode == STATUS_IN_PAGE_ERROR, "wrong exception %lx\n", rec->ExceptionCode );
    ok( rec->NumberParameters == 3, "wrong params %lx\n", rec->NumberParameters );
    ok( rec->ExceptionInformation[0] == 1, "not write access %Ix\n", rec->ExceptionInformation[0] );
    ok( (int *)rec->ExceptionInformation[1] == write_addr,
        "wrong address %p / %p\n", (void *)rec->ExceptionInformation[1], write_addr );
    ok( rec->ExceptionInformation[2] == STATUS_EXECUTABLE_MEMORY_WRITE, "wrong status %Ix\n",
        rec->ExceptionInformation[2] );

    status = NtSetInformationThread( GetCurrentThread(), ThreadManageWritesToExecutableMemory,
                                     &mem, sizeof(mem) );
    ok( !status, "NtSetInformationThread failed %lx\n", status );
    *write_addr = 0;  /* make the page dirty to prevent further exceptions */
    mem.ThreadAllowWrites = 0;
    status = NtSetInformationThread( GetCurrentThread(), ThreadManageWritesToExecutableMemory,
                                     &mem, sizeof(mem) );
    ok( !status, "NtSetInformationThread failed %lx\n", status );
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void test_exec_memory_writes(void)
{
    NTSTATUS status;
    void *ptr, *handler;
    MANAGE_WRITES_TO_EXECUTABLE_MEMORY mem = { .Version = 2 };
    MEMORY_RANGE_ENTRY range;
    ULONG flag, len, granularity;
    ULONG_PTR count;
    void *addresses[4];
    DWORD old_prot;
    WCHAR path[MAX_PATH];
    HANDLE file;
    IO_STATUS_BLOCK io;

    status = NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                      &mem, sizeof(mem) );
#ifdef __aarch64__
    ok( !status, "NtSetInformationProcess failed %lx\n", status );
#else
    if (!status)
    {
        SYSTEM_CPU_INFORMATION info;
        ULONG len;

        pRtlGetNativeSystemInformation( SystemCpuInformation, &info, sizeof(info), &len );
        ok (info.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64, "succeeded on non-ARM64\n" );
        mem.ProcessEnableWriteExceptions = 1;
        NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                 &mem, sizeof(mem) );
        skip( "skipping test on ARM64EC\n" );
        return;
    }
    ok( status == STATUS_INVALID_INFO_CLASS || status == STATUS_NOT_SUPPORTED,
        "NtSetInformationProcess failed %lx\n", status );
#endif
    if (status) return;
    handler = RtlAddVectoredExceptionHandler( TRUE, exec_write_handler );

    /* test anon mapping */

    ptr = VirtualAlloc( NULL, page_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    write_addr = (int *)ptr + 3;

    mem.ProcessEnableWriteExceptions = 1;
    status = NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                      &mem, sizeof(mem) );
    ok( !status, "NtSetInformationProcess failed %lx\n", status );

    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 0, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    VirtualProtect( ptr, page_size, PAGE_EXECUTE_READWRITE, &old_prot );
    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    /* no longer failing on dirty page */
    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 0, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    /* setting permissions resets protection */
    VirtualProtect( ptr, page_size, PAGE_EXECUTE_READWRITE, &old_prot );
    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    /* clearing dirty state also resets protection */
    range.VirtualAddress = ptr;
    range.NumberOfBytes = 1;
    flag = 0;
    status = pNtSetInformationVirtualMemory( GetCurrentProcess(), VmPageDirtyStateInformation,
                                             1, &range, &flag, sizeof(flag) );
    ok( !status, "NtSetInformationVirtualMemory failed %lx\n", status );

    /* making page dirty is not allowed */
    flag = 1;
    status = pNtSetInformationVirtualMemory( GetCurrentProcess(), VmPageDirtyStateInformation,
                                             1, &range, &flag, sizeof(flag) );
    ok( status == STATUS_INVALID_PARAMETER_5, "NtSetInformationVirtualMemory failed %lx\n", status );

    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    GetModuleFileNameW( 0, path, MAX_PATH );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0 );
    ok( file != INVALID_HANDLE_VALUE, "can't open %s: %lu\n", debugstr_w(path), GetLastError() );
    /* reading into protected page crashes on Windows */
    if (0) VirtualProtect( ptr, page_size, PAGE_EXECUTE_READWRITE, &old_prot );
    status = NtReadFile( file, 0, NULL, NULL, &io, write_addr, 8, NULL, NULL );
    ok( !status, "NtReadFile failed %lx\n", status );
    CloseHandle( file );

    VirtualFree( ptr, 0, MEM_RELEASE );

    /* test PE mapping */

    ptr = GetModuleHandleA( NULL );
    write_addr = (int *)ptr + 3;
    VirtualProtect( ptr, page_size, PAGE_EXECUTE_WRITECOPY, &old_prot );

    got_exception = 0;
    *write_addr = 0;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    got_exception = 0;
    *write_addr = 0;
    ok( got_exception == 0, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    VirtualProtect( ptr, page_size, PAGE_EXECUTE_WRITECOPY, &old_prot );
    got_exception = 0;
    *write_addr = 0;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    range.VirtualAddress = write_addr;
    range.NumberOfBytes = 1;
    flag = 0;
    status = pNtSetInformationVirtualMemory( GetCurrentProcess(), VmPageDirtyStateInformation,
                                             1, &range, &flag, sizeof(flag) );
    ok( !status, "NtSetInformationVirtualMemory failed %lx\n", status );
    got_exception = 0;
    *write_addr = 0;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    /* test interactions with write watches */

    ptr = VirtualAlloc( NULL, page_size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE );
    write_addr = (int *)ptr + 3;

    VirtualProtect( ptr, page_size, PAGE_EXECUTE_READWRITE, &old_prot );
    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    count = ARRAY_SIZE(addresses);
    status = NtGetWriteWatch( GetCurrentProcess(), 0, ptr, page_size, addresses, &count, &granularity );
    ok( !status, "NtGetWriteWatch failed %lx\n", status );
    ok( count == 1, "got count %Iu\n", count );
    ok( addresses[0] == ptr, "wrong ptr %p / %p\n", addresses[0], ptr );

    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 0, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    count = ARRAY_SIZE(addresses);
    status = NtGetWriteWatch( GetCurrentProcess(), WRITE_WATCH_FLAG_RESET,
                              ptr, page_size, addresses, &count, &granularity );
    ok( !status, "NtGetWriteWatch failed %lx\n", status );
    ok( count == 1, "got count %Iu\n", count );
    ok( addresses[0] == ptr, "wrong ptr %p / %p\n", addresses[0], ptr );

    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    count = ARRAY_SIZE(addresses);
    status = NtGetWriteWatch( GetCurrentProcess(), 0, ptr, page_size, addresses, &count, &granularity );
    ok( !status, "NtGetWriteWatch failed %lx\n", status );
    ok( count == 1, "got count %Iu\n", count );
    ok( addresses[0] == ptr, "wrong ptr %p / %p\n", addresses[0], ptr );

    range.VirtualAddress = ptr;
    range.NumberOfBytes = 1;
    flag = 0;
    status = pNtSetInformationVirtualMemory( GetCurrentProcess(), VmPageDirtyStateInformation,
                                             1, &range, &flag, sizeof(flag) );
    ok( !status, "NtSetInformationVirtualMemory failed %lx\n", status );

    count = ARRAY_SIZE(addresses);
    status = NtGetWriteWatch( GetCurrentProcess(), 0, ptr, page_size, addresses, &count, &granularity );
    ok( !status, "NtGetWriteWatch failed %lx\n", status );
    ok( count == 0, "got count %Iu\n", count );

    got_exception = 0;
    *write_addr = 0x123456;
    ok( got_exception == 1, "wrong number of exceptions %u\n", got_exception );
    write_addr++;

    /* test some invalid calls */

    VirtualFree( ptr, 0, MEM_RELEASE );
    flag = 0;
    status = pNtSetInformationVirtualMemory( GetCurrentProcess(), VmPageDirtyStateInformation,
                                             1, &range, &flag, sizeof(flag) );
    ok( status == STATUS_MEMORY_NOT_ALLOCATED, "NtSetInformationVirtualMemory failed %lx\n", status );

    mem.ProcessEnableWriteExceptions = 0;
    NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                             &mem, sizeof(mem) );

    status = pNtSetInformationVirtualMemory( GetCurrentProcess(), VmPageDirtyStateInformation,
                                             1, &range, &flag, sizeof(flag) );
    ok( status == STATUS_NOT_SUPPORTED, "NtSetInformationVirtualMemory failed %lx\n", status );
    status = NtQueryInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                        &mem, sizeof(mem), &len );
    ok( status == STATUS_INVALID_INFO_CLASS, "NtQueryInformationProcess failed %lx\n", status );

    mem.ProcessEnableWriteExceptions = 1;
    mem.ThreadAllowWrites = 1;
    status = NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                      &mem, sizeof(mem) );
    ok( status == STATUS_INVALID_PARAMETER, "NtSetInformationProcess failed %lx\n", status );
    status = NtSetInformationThread( GetCurrentThread(), ThreadManageWritesToExecutableMemory,
                                     &mem, sizeof(mem) );
    ok( status == STATUS_INVALID_PARAMETER, "NtSetInformationThread failed %lx\n", status );
    mem.ProcessEnableWriteExceptions = 0;
    mem.ThreadAllowWrites = 0;
    mem.Version = 3;
    status = NtSetInformationThread( GetCurrentThread(), ThreadManageWritesToExecutableMemory,
                                     &mem, sizeof(mem) );
    ok( status == STATUS_REVISION_MISMATCH, "NtSetInformationThread failed %lx\n", status );
    status = NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                      &mem, sizeof(mem) );
    ok( status == STATUS_REVISION_MISMATCH, "NtSetInformationProcess failed %lx\n", status );
    mem.Version = 2;
    status = NtSetInformationThread( GetCurrentThread(), ThreadManageWritesToExecutableMemory,
                                     &mem, sizeof(mem) - 1 );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "NtSetInformationThread failed %lx\n", status );
    status = NtSetInformationThread( GetCurrentThread(), ThreadManageWritesToExecutableMemory,
                                     &mem, sizeof(mem) + 1 );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "NtSetInformationThread failed %lx\n", status );
    status = NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                      &mem, sizeof(mem) - 1 );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "NtSetInformationProcess failed %lx\n", status );
    status = NtSetInformationProcess( GetCurrentProcess(), ProcessManageWritesToExecutableMemory,
                                      &mem, sizeof(mem) + 1 );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "NtSetInformationProcess failed %lx\n", status );

    RtlRemoveVectoredExceptionHandler( handler );
}

START_TEST(virtual)
{
    HMODULE mod;

    int argc;
    char **argv;
    argc = winetest_get_mainargs(&argv);

    if (argc >= 3)
    {
        if (!strcmp(argv[2], "sleep"))
        {
            Sleep(5000); /* spawned process runs for at most 5 seconds */
            return;
        }
#ifndef _WIN64
        if (!strcmp(argv[2], "wow64_writewatch"))
            ExitProcess( run_wow64_translated_writewatch_child() );
#endif
        return;
    }

    mod = GetModuleHandleA("kernel32.dll");
    pIsWow64Process = (void *)GetProcAddress(mod, "IsWow64Process");
    pGetEnabledXStateFeatures = (void *)GetProcAddress(mod, "GetEnabledXStateFeatures");
    mod = GetModuleHandleA("ntdll.dll");
    pRtlCreateUserStack = (void *)GetProcAddress(mod, "RtlCreateUserStack");
    pRtlCreateUserThread = (void *)GetProcAddress(mod, "RtlCreateUserThread");
    pRtlFreeUserStack = (void *)GetProcAddress(mod, "RtlFreeUserStack");
    pRtlFindExportedRoutineByName = (void *)GetProcAddress(mod, "RtlFindExportedRoutineByName");
    pRtlGetEnabledExtendedFeatures = (void *)GetProcAddress(mod, "RtlGetEnabledExtendedFeatures");
    pNtAllocateVirtualMemoryEx = (void *)GetProcAddress(mod, "NtAllocateVirtualMemoryEx");
    pNtMapViewOfSectionEx = (void *)GetProcAddress(mod, "NtMapViewOfSectionEx");
    pNtCreateSectionEx = (void *)GetProcAddress(mod, "NtCreateSectionEx");
    pNtSetInformationVirtualMemory = (void *)GetProcAddress(mod, "NtSetInformationVirtualMemory");

#ifndef __aarch64__
    pRtlGetNativeSystemInformation = (void *)GetProcAddress(mod, "RtlGetNativeSystemInformation");
#endif
#ifdef __x86_64__
    pRtlIsEcCode = (void *)GetProcAddress(mod, "RtlIsEcCode");
#endif

    NtQuerySystemInformation(SystemBasicInformation, &sbi, sizeof(sbi), NULL);
    trace("system page size %#lx\n", sbi.PageSize);
    page_size = sbi.PageSize;
    if (!pIsWow64Process || !pIsWow64Process(NtCurrentProcess(), &is_wow64)) is_wow64 = FALSE;

    test_NtAllocateVirtualMemory();
    test_NtAllocateVirtualMemoryEx();
    test_NtAllocateVirtualMemoryEx_address_requirements();
    test_NtFreeVirtualMemory();
    test_NtProtectVirtualMemory();
    test_RtlCreateUserStack();
    test_NtMapViewOfSection();
    test_NtMapViewOfSectionEx();
    test_prefetch();
    test_user_shared_data();
    test_wow64_translated_guard_resolution();
    test_wow64_translated_writewatch_resolution();
    test_wow64_translated_copy_protection();
    test_wow64_unowned_shadow_access();
    test_wow64_virtual_guest_marshalling();
    test_wow64_process_vm_machine_information();
    test_wow64_translated_view_contract();
    test_syscalls();
    test_invalid_syscalls();
    test_syscall_numbers();
    test_syscall_abi();
    test_query_region_information();
    test_query_image_information();
    test_exec_memory_writes();
}
