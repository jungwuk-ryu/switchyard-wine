/* ARM64EC EC-code bitmap tests
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
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/low_va.h"
#include "wine/test.h"

#ifdef __x86_64__

static BOOLEAN (WINAPI *pRtlIsEcCode)(ULONG_PTR);
static NTSTATUS (WINAPI *pRtlGetNativeSystemInformation)(SYSTEM_INFORMATION_CLASS, void *, ULONG,
                                                        ULONG *);
static NTSTATUS (WINAPI *pNtAllocateVirtualMemoryEx)(HANDLE, void **, SIZE_T *, ULONG, ULONG,
                                                     MEM_EXTENDED_PARAMETER *, ULONG);

static void check_non_ec_pointer( ULONGLONG value )
{
    ok( !pRtlIsEcCode( value ), "EC code pointer %#I64x\n", value );
}

static void test_bitmap_boundaries(void)
{
    MEM_EXTENDED_PARAMETER ext = {0};
    SYSTEM_CPU_INFORMATION cpu_info;
    HMODULE module = GetModuleHandleW( L"ntdll.dll" );
    ULONG return_length = 0;
    SIZE_T size;
    NTSTATUS status;
    void *address;

    pRtlIsEcCode = (void *)GetProcAddress( module, "RtlIsEcCode" );
    pRtlGetNativeSystemInformation =
        (void *)GetProcAddress( module, "RtlGetNativeSystemInformation" );
    pNtAllocateVirtualMemoryEx = (void *)GetProcAddress( module, "NtAllocateVirtualMemoryEx" );
    if (!pRtlIsEcCode || !pRtlGetNativeSystemInformation || !pNtAllocateVirtualMemoryEx)
    {
        win_skip( "ARM64EC virtual-memory exports are unavailable\n" );
        return;
    }

    status = pRtlGetNativeSystemInformation( SystemCpuInformation, &cpu_info,
                                             sizeof(cpu_info), &return_length );
    ok( !status, "RtlGetNativeSystemInformation failed %#lx\n", status );
    if (status || cpu_info.ProcessorArchitecture != PROCESSOR_ARCHITECTURE_ARM64)
    {
        win_skip( "not running an x64 process on ARM64\n" );
        return;
    }

    check_non_ec_pointer( WINE_ARM64EC_CODE_POINTER_LIMIT - 1 );
    check_non_ec_pointer( WINE_ARM64EC_CODE_POINTER_LIMIT );
    check_non_ec_pointer( ~0ull );
    check_non_ec_pointer( 0xffff800000000000ull );
    check_non_ec_pointer( 0x6f646e69775c3a43ull );
    check_non_ec_pointer( WINE_LOW_VA_SHADOW_BASE );
    check_non_ec_pointer( WINE_LOW_VA_SHADOW_BASE + 0x00401000 );

    ext.Type = MemExtendedParameterAttributeFlags;
    ext.ULong64 = MEM_EXTENDED_PARAMETER_EC_CODE;
    address = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &address, &size, MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE, &ext, 1 );
    ok( !status, "EC-code allocation failed %#lx\n", status );
    if (!status)
    {
        ok( pRtlIsEcCode( (ULONG_PTR)address ), "EC-code allocation %p is unmarked\n", address );
        size = 0;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
        ok( !status, "EC-code allocation release failed %#lx\n", status );
    }

    address = NULL;
    size = 0x10000;
    status = pNtAllocateVirtualMemoryEx( NtCurrentProcess(), &address, &size, MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE, NULL, 0 );
    ok( !status, "ordinary allocation failed %#lx\n", status );
    if (!status)
    {
        ok( !pRtlIsEcCode( (ULONG_PTR)address ), "ordinary allocation %p is EC code\n", address );
        size = 0;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
        ok( !status, "ordinary allocation release failed %#lx\n", status );
    }
}

#endif

START_TEST(arm64ec_ec_code)
{
#ifdef __x86_64__
    test_bitmap_boundaries();
#else
    win_skip( "RtlIsEcCode is an x64 export\n" );
#endif
}
