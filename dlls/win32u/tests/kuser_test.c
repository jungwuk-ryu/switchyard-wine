/*
 * Native KUSER_SHARED_DATA backing test
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

#if 0
#pragma makedep testdll
#endif

#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntuser.h"

typedef DWORD (WINAPI *get_tick_count_func)(void);
typedef ULONGLONG (WINAPI *get_tick_count64_func)(void);
typedef DEP_SYSTEM_POLICY_TYPE (WINAPI *get_system_dep_policy_func)(void);
typedef void (WINAPI *query_interrupt_time_func)(ULONGLONG *);

static int fail( const char *message )
{
    fprintf( stderr, "KUSER_SHARED_DATA_TEST_FAIL: %s\n", message );
    return 1;
}

int main( int argc, char **argv )
{
    query_interrupt_time_func query_interrupt_time, query_unbiased_interrupt_time_precise;
    get_system_dep_policy_func get_system_dep_policy;
    get_tick_count_func kernelbase_get_tick_count;
    get_tick_count64_func kernelbase_get_tick_count64;
    const struct _KUSER_SHARED_DATA *shared_data = RtlGetCurrentPeb()->SharedData;
    const void *canonical_shared_data = ULongToPtr( 0x7ffe0000 );
    MEMORY_BASIC_INFORMATION mbi;
    DEP_SYSTEM_POLICY_TYPE dep_policy;
    BOOL alternate_backing;
    HMODULE kernel32, kernelbase;
    ULONGLONG tick64, kernelbase_tick64, interrupt_time, unbiased_time;
    SIZE_T ret_len;
    NTSTATUS status;
    DWORD tick, kernelbase_tick;

    (void)argc;
    (void)argv;

    if (!shared_data) return fail( "PEB.SharedData is null" );
    alternate_backing = shared_data != canonical_shared_data;
    status = NtQueryVirtualMemory( NtCurrentProcess(), shared_data, MemoryBasicInformation,
                                   &mbi, sizeof(mbi), &ret_len );
    if (status || ret_len != sizeof(mbi)) return fail( "shared-data query failed" );
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return fail( "shared-data backing is not readable committed memory" );
    if (alternate_backing)
    {
        status = NtQueryVirtualMemory( NtCurrentProcess(), canonical_shared_data,
                                       MemoryBasicInformation, &mbi, sizeof(mbi), &ret_len );
        if (!status && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return fail( "canonical low shared-data address is unexpectedly readable" );
    }

    tick = GetTickCount();
    tick64 = GetTickCount64();
    if (!(kernel32 = GetModuleHandleW( L"kernel32.dll" )) ||
        !(get_system_dep_policy =
          (get_system_dep_policy_func)GetProcAddress( kernel32, "GetSystemDEPPolicy" )))
        return fail( "missing kernel32 DEP export" );
    dep_policy = get_system_dep_policy();
    if (dep_policy > OptOut) return fail( "invalid DEP policy" );

    if (!(kernelbase = GetModuleHandleW( L"kernelbase.dll" )) &&
        !(kernelbase = LoadLibraryW( L"kernelbase.dll" )))
        return fail( "cannot load kernelbase" );
    kernelbase_get_tick_count = (get_tick_count_func)GetProcAddress( kernelbase, "GetTickCount" );
    kernelbase_get_tick_count64 =
        (get_tick_count64_func)GetProcAddress( kernelbase, "GetTickCount64" );
    query_interrupt_time =
        (query_interrupt_time_func)GetProcAddress( kernelbase, "QueryInterruptTime" );
    query_unbiased_interrupt_time_precise =
        (query_interrupt_time_func)GetProcAddress( kernelbase, "QueryUnbiasedInterruptTimePrecise" );
    if (!kernelbase_get_tick_count || !kernelbase_get_tick_count64 ||
        !query_interrupt_time || !query_unbiased_interrupt_time_precise)
        return fail( "missing kernelbase time export" );

    kernelbase_tick = kernelbase_get_tick_count();
    kernelbase_tick64 = kernelbase_get_tick_count64();
    query_interrupt_time( &interrupt_time );
    query_unbiased_interrupt_time_precise( &unbiased_time );

    /* The first call installs the queue masks; the second unconditionally
     * takes the matching-mask tick-count path without the PeekMessage throttle. */
    NtUserGetDesktopWindow();
    NtUserMsgWaitForMultipleObjectsEx( 0, NULL, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE );
    NtUserMsgWaitForMultipleObjectsEx( 0, NULL, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE );

    printf( "KUSER_SHARED_DATA_TEST_PASS alternate_backing=%u tick=%lu tick64=%llu "
            "kernelbase_tick=%lu kernelbase_tick64=%llu interrupt=%llu unbiased=%llu\n",
            alternate_backing, tick, tick64, kernelbase_tick, kernelbase_tick64,
            interrupt_time, unbiased_time );
    return 0;
}
