/*
 * Unit test suite Wow64 functions
 *
 * Copyright 2021 Alexandre Julliard
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
 *
 */

#include <stdarg.h>
#include <limits.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "winuser.h"
#include "winreg.h"
#include "ddk/wdm.h"
#include "wine/exception.h"
#include "wine/low_va.h"
#include "wine/server.h"
#include "wine/test.h"
#include "wine/unixlib.h"
#include "wine/wow64_user.h"
#include "x18_dispatch_test.h"

static NTSTATUS (WINAPI *pNtQuerySystemInformationEx)(SYSTEM_INFORMATION_CLASS,void*,ULONG,void*,ULONG,ULONG*);
static NTSTATUS (WINAPI *pRtlGetNativeSystemInformation)(SYSTEM_INFORMATION_CLASS,void*,ULONG,ULONG*);
static void     (WINAPI *pRtlOpenCrossProcessEmulatorWorkConnection)(HANDLE,HANDLE*,void**);
static void *   (WINAPI *pRtlFindExportedRoutineByName)(HMODULE,const char *);
static USHORT   (WINAPI *pRtlWow64GetCurrentMachine)(void);
static NTSTATUS (WINAPI *pRtlWow64GetProcessMachines)(HANDLE,WORD*,WORD*);
static NTSTATUS (WINAPI *pRtlWow64GetSharedInfoProcess)(HANDLE,BOOLEAN*,WOW64INFO*);
static NTSTATUS (WINAPI *pRtlWow64IsWowGuestMachineSupported)(USHORT,BOOLEAN*);
static NTSTATUS (WINAPI *pNtMapViewOfSectionEx)(HANDLE,HANDLE,PVOID*,const LARGE_INTEGER*,SIZE_T*,ULONG,ULONG,MEM_EXTENDED_PARAMETER*,ULONG);
#ifndef __arm__
static NTSTATUS (WINAPI *pNtSetLdtEntries)(ULONG,ULONG,ULONG,ULONG,ULONG,ULONG);
#endif
#ifdef __x86_64__
static NTSTATUS (WINAPI *pKiUserExceptionDispatcher)(EXCEPTION_RECORD*,CONTEXT*);
#endif
#ifdef _WIN64
static NTSTATUS (WINAPI *pRtlWow64GetCpuAreaInfo)(WOW64_CPURESERVED*,ULONG,WOW64_CPU_AREA_INFO*);
static NTSTATUS (WINAPI *pRtlWow64GetThreadContext)(HANDLE,WOW64_CONTEXT*);
static NTSTATUS (WINAPI *pRtlWow64GetThreadSelectorEntry)(HANDLE,THREAD_DESCRIPTOR_INFORMATION*,ULONG,ULONG*);
static CROSS_PROCESS_WORK_ENTRY * (WINAPI *pRtlWow64PopAllCrossProcessWorkFromWorkList)(CROSS_PROCESS_WORK_HDR*,BOOLEAN*);
static CROSS_PROCESS_WORK_ENTRY * (WINAPI *pRtlWow64PopCrossProcessWorkFromFreeList)(CROSS_PROCESS_WORK_HDR*);
static BOOLEAN (WINAPI *pRtlWow64PushCrossProcessWorkOntoFreeList)(CROSS_PROCESS_WORK_HDR*,CROSS_PROCESS_WORK_ENTRY*);
static BOOLEAN (WINAPI *pRtlWow64PushCrossProcessWorkOntoWorkList)(CROSS_PROCESS_WORK_HDR*,CROSS_PROCESS_WORK_ENTRY*,void**);
static BOOLEAN (WINAPI *pRtlWow64RequestCrossProcessHeavyFlush)(CROSS_PROCESS_WORK_HDR*);
static void (WINAPI *pProcessPendingCrossProcessEmulatorWork)(void);
#else
static NTSTATUS (WINAPI *pNtQuerySystemInformation)(SYSTEM_INFORMATION_CLASS,void*,ULONG,ULONG*);
static NTSTATUS (WINAPI *pNtWow64AllocateVirtualMemory64)(HANDLE,ULONG64*,ULONG64,ULONG64*,ULONG,ULONG);
static NTSTATUS (WINAPI *pNtWow64GetNativeSystemInformation)(SYSTEM_INFORMATION_CLASS,void*,ULONG,ULONG*);
static NTSTATUS (WINAPI *pNtWow64IsProcessorFeaturePresent)(ULONG);
static NTSTATUS (WINAPI *pNtWow64QueryInformationProcess64)(HANDLE,PROCESSINFOCLASS,void*,ULONG,ULONG*);
static NTSTATUS (WINAPI *pNtWow64ReadVirtualMemory64)(HANDLE,ULONG64,void*,ULONG64,ULONG64*);
static NTSTATUS (WINAPI *pNtWow64WriteVirtualMemory64)(HANDLE,ULONG64,const void *,ULONG64,ULONG64*);
static BOOL old_wow64;  /* Wine old-style wow64 */
static void *code_mem;
#endif

static BOOL is_win64 = sizeof(void *) > sizeof(int);
static BOOL is_wow64;

typedef NTSTATUS (WINAPI *unix_call_dispatcher_func)(unixlib_handle_t,
                                                      unsigned int, void *);

struct unixlib_dispatch_thread_params
{
    unix_call_dispatcher_func dispatcher;
    unixlib_handle_t handle;
    unsigned int code;
    void *args;
    NTSTATUS status;
};

static DWORD WINAPI unixlib_dispatch_thread( void *arg )
{
    struct unixlib_dispatch_thread_params *params = arg;

    params->status = params->dispatcher( params->handle, params->code, params->args );
    return 0;
}

static WCHAR *find_last_unixlib_path_separator( WCHAR *path )
{
    WCHAR *ret = NULL;

    for (; *path; path++) if (*path == '/' || *path == '\\') ret = path;
    return ret;
}

static NTSTATUS try_load_wow64_unixlib_lifecycle_helper(
    WCHAR *path, BOOL *found, unixlib_module_t *module, unixlib_handle_t *handle )
{
    UNICODE_STRING name;
    NTSTATUS status;
    DWORD attrs;

    if ((attrs = GetFileAttributesW( path )) == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return STATUS_DLL_NOT_FOUND;

    *found = TRUE;
    if ((status = RtlDosPathNameToNtPathName_U_WithStatus( path, &name, NULL, NULL )))
        return status;
    status = __wine_load_unix_lib( &name, module, handle );
    RtlFreeUnicodeString( &name );
    return status;
}

static NTSTATUS load_wow64_unixlib_lifecycle_helper(
    WCHAR *path, SIZE_T count, BOOL *found, unixlib_module_t *module,
    unixlib_handle_t *handle )
{
    static const WCHAR helper_name[] = L"ntdll-x18-test.so";
    UNICODE_STRING name;
    WCHAR *separator, *parent;
    NTSTATUS status;
    DWORD len;

    *found = FALSE;
    if (!(len = GetModuleFileNameW( NULL, path, count )) || len >= count)
        return STATUS_BUFFER_TOO_SMALL;
    if (!(separator = find_last_unixlib_path_separator( path )))
        return STATUS_OBJECT_PATH_INVALID;
    if (separator - path + ARRAY_SIZE(helper_name) >= count)
        return STATUS_BUFFER_TOO_SMALL;

    separator[1] = 0;
    lstrcatW( path, helper_name );
    status = try_load_wow64_unixlib_lifecycle_helper( path, found, module, handle );
    if (*found) return status;

    *separator = 0;
    if (!(parent = find_last_unixlib_path_separator( path ))) return STATUS_DLL_NOT_FOUND;
    if (parent - path + ARRAY_SIZE(helper_name) >= count) return STATUS_BUFFER_TOO_SMALL;
    parent[1] = 0;
    lstrcatW( path, helper_name );
    status = try_load_wow64_unixlib_lifecycle_helper( path, found, module, handle );
    if (*found) return status;

    RtlInitUnicodeString( &name, helper_name );
    status = __wine_load_unix_lib( &name, module, handle );
    if (!status)
    {
        lstrcpynW( path, helper_name, count );
        *found = TRUE;
    }
    return status;
}

static void test_unixlib_dispatch_bounds(void)
{
    enum
    {
        unix_get_current_teb_test = 3,
        ntdll_wow64_unix_call_count = 22,
        dispatch_max_slots = 1025,
    };
    NTSTATUS (WINAPI **dispatcher_ptr)(unixlib_handle_t, unsigned int, void *);
    NTSTATUS (WINAPI *dispatcher)(unixlib_handle_t, unsigned int, void *);
    struct
    {
        void *teb;
    } params = {0};
    unixlib_handle_t *handle_ptr, handle;
    ULONG translated = 0;
    NTSTATUS status;
    HMODULE ntdll;

    ntdll = GetModuleHandleA( "ntdll.dll" );
    handle_ptr = pRtlFindExportedRoutineByName( ntdll, "__wine_unixlib_handle" );
    dispatcher_ptr = pRtlFindExportedRoutineByName( ntdll, "__wine_unix_call_dispatcher" );
    if (!handle_ptr || !dispatcher_ptr || !*dispatcher_ptr)
    {
        skip( "ntdll Unix-call exports are unavailable\n" );
        return;
    }

    status = NtQueryVirtualMemory( GetCurrentProcess(), NtCurrentTeb(),
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), NULL );
    if (status) translated = 0;

    handle = *handle_ptr;
    dispatcher = *dispatcher_ptr;
    if (translated)
    {
        ok( !!(handle & WINE_UNIXLIB_DISPATCH_HANDLE_TAG),
            "expected a tagged WoW64 Unix-call handle, got %s\n", wine_dbgstr_longlong(handle) );

        status = dispatcher( handle, unix_get_current_teb_test, &params );
        ok( status == STATUS_SUCCESS, "valid dispatch returned %#lx\n", status );
        ok( params.teb == NtCurrentTeb(), "TEB %p, expected %p\n", params.teb, NtCurrentTeb() );

        status = dispatcher( handle, ntdll_wow64_unix_call_count, &params );
        ok( status == STATUS_INVALID_PARAMETER, "code==count returned %#lx\n", status );
        status = dispatcher( WINE_UNIXLIB_DISPATCH_HANDLE_TAG, unix_get_current_teb_test, &params );
        ok( status == STATUS_INVALID_PARAMETER, "zero tagged payload returned %#lx\n", status );
        status = dispatcher( WINE_UNIXLIB_DISPATCH_HANDLE_TAG | dispatch_max_slots,
                             unix_get_current_teb_test, &params );
        ok( status == STATUS_INVALID_PARAMETER, "empty tagged slot returned %#lx\n", status );
        status = dispatcher( WINE_UNIXLIB_DISPATCH_HANDLE_TAG | (dispatch_max_slots + 1),
                             unix_get_current_teb_test, &params );
        ok( status == STATUS_INVALID_PARAMETER, "out-of-range tagged slot returned %#lx\n", status );
    }
    else
    {
        ok( !(handle & WINE_UNIXLIB_DISPATCH_HANDLE_TAG),
            "trusted native Unix-call handle is unexpectedly tagged: %s\n",
            wine_dbgstr_longlong(handle) );
        status = dispatcher( handle, unix_get_current_teb_test, &params );
        ok( status == STATUS_SUCCESS, "native raw dispatch returned %#lx\n", status );
        ok( params.teb != NULL, "native raw dispatch returned no TEB\n" );
    }
}

static void test_unixlib_dispatch_lifecycle(void)
{
    static const SIZE_T result_lengths[] = {0, 1, 7, 8, 9, 15, 16, 17};
    unix_call_dispatcher_func *dispatcher_ptr, dispatcher;
    struct wow64_unixlib_context_params context_params;
    struct wow64_unixlib_context_result context_result;
    struct wow64_unixlib_checked_fault_params fault_params;
    struct wow64_unixlib_checked_fault_result fault_result = {0};
    struct wow64_unixlib_block_params block_params;
    struct wow64_unixlib_self_unload_params unload_params;
    struct unixlib_dispatch_thread_params thread_params;
    unixlib_handle_t handle, stale_handle;
    unixlib_module_t module;
    ULONG translated = 0;
    WCHAR helper_path[MAX_PATH];
    HMODULE ntdll;
    HANDLE thread, entered_event, release_event;
    NTSTATUS status;
    void *noaccess;
    BOOL found;
    UINT i;

    status = NtQueryVirtualMemory( GetCurrentProcess(), NtCurrentTeb(),
                                   MemoryWineWow64TranslatedInformation,
                                   &translated, sizeof(translated), NULL );
    if (status || !translated) return;

    ntdll = GetModuleHandleA( "ntdll.dll" );
    dispatcher_ptr = pRtlFindExportedRoutineByName( ntdll, "__wine_unix_call_dispatcher" );
    if (!dispatcher_ptr || !(dispatcher = *dispatcher_ptr))
    {
        skip( "the Wine Unix-call dispatcher is unavailable\n" );
        return;
    }

    status = load_wow64_unixlib_lifecycle_helper(
        helper_path, ARRAY_SIZE(helper_path), &found, &module, &handle );
    if (!found)
    {
        skip( "the WoW64 Unixlib lifecycle helper is unavailable\n" );
        return;
    }
    ok( !status, "failed to load %s, status %#lx\n",
        wine_dbgstr_w(helper_path), status );
    if (status) return;
    ok( wine_unixlib_decode_dispatch_handle( handle, NULL, NULL ),
        "expected generation-tagged handle, got %s\n", wine_dbgstr_longlong(handle) );

    status = __wine_unload_unix_lib( 0 );
    ok( status == STATUS_INVALID_HANDLE, "zero token unload returned %#lx\n", status );
    status = __wine_unload_unix_lib( 0x4000000000001234ull );
    ok( status == STATUS_INVALID_HANDLE, "unknown token unload returned %#lx\n", status );

    status = dispatcher( handle, wow64_unixlib_lifecycle_zero_args, NULL );
    ok( !status, "zero-argument dispatch returned %#lx\n", status );
    status = dispatcher( handle, wow64_unixlib_lifecycle_zero_args, &context_params );
    ok( !status, "zero-argument non-NULL dispatch returned %#lx\n", status );

    memset( &context_result, 0, sizeof(context_result) );
    context_params.result = PtrToUlong( &context_result );
    context_params.value = 0x12345678;
    status = dispatcher( handle, wow64_unixlib_lifecycle_context, &context_params );
    ok( !status, "context dispatch returned %#lx\n", status );
    ok( context_result.guest_args == PtrToUlong(&context_params),
        "context guest args %#x, expected %#lx\n", context_result.guest_args,
        PtrToUlong(&context_params) );
    ok( context_result.args_size == sizeof(context_params),
        "context args size %u, expected %Iu\n", context_result.args_size,
        sizeof(context_params) );
    ok( context_result.flags == (WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED |
                                 WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT),
        "context flags %#x\n", context_result.flags );
    ok( context_result.value == context_params.value, "context value %#x\n",
        context_result.value );

    noaccess = VirtualAlloc( NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS );
    fault_params.noaccess = PtrToUlong( noaccess );
    fault_params.result = PtrToUlong( &fault_result );
    ok( !!fault_params.noaccess, "failed to allocate no-access page, error %lu\n",
        GetLastError() );
    if (fault_params.noaccess)
    {
        memset( &fault_result, 0, sizeof(fault_result) );
        status = dispatcher( handle, wow64_unixlib_lifecycle_checked_fault,
                             &fault_params );
        ok( !status, "checked-fault dispatch returned %#lx\n", status );
        ok( fault_result.fault_status == STATUS_ACCESS_VIOLATION,
            "nested checked copy returned %#lx\n", fault_result.fault_status );
        ok( fault_result.guest_args_before == PtrToUlong(&fault_params) &&
            fault_result.guest_args_after == PtrToUlong(&fault_params),
            "nested context args %#x/%#x, expected %#lx\n",
            fault_result.guest_args_before, fault_result.guest_args_after,
            PtrToUlong(&fault_params) );
        ok( fault_result.context_preserved,
            "nested checked-copy longjmp discarded the outer context\n" );
        VirtualFree( noaccess, 0, MEM_RELEASE );
    }

    status = dispatcher( handle, wow64_unixlib_lifecycle_illegal_instruction, NULL );
    ok( status == STATUS_ILLEGAL_INSTRUCTION,
        "illegal-instruction dispatch returned %#lx\n", status );
    memset( &context_result, 0, sizeof(context_result) );
    status = dispatcher( handle, wow64_unixlib_lifecycle_context, &context_params );
    ok( !status && context_result.guest_args == PtrToUlong(&context_params),
        "post-signal context dispatch returned %#lx args %#x\n",
        status, context_result.guest_args );

    stale_handle = handle;
    status = __wine_unload_unix_lib( module );
    ok( !status, "initial unload returned %#lx\n", status );
    status = __wine_unload_unix_lib( module );
    ok( status == STATUS_INVALID_HANDLE, "replayed unload returned %#lx\n", status );
    status = dispatcher( stale_handle, wow64_unixlib_lifecycle_zero_args, NULL );
    ok( status == STATUS_INVALID_PARAMETER, "stale dispatch returned %#lx\n", status );

    status = load_wow64_unixlib_lifecycle_helper(
        helper_path, ARRAY_SIZE(helper_path), &found, &module, &handle );
    ok( found && !status, "reload returned %#lx, found %u\n", status, found );
    if (status || !found) return;
    ok( handle != stale_handle, "reload reused stale handle %s\n",
        wine_dbgstr_longlong(handle) );

    entered_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    release_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!entered_event && !!release_event,
        "failed to create lifecycle events, error %lu\n", GetLastError() );
    block_params.entered_event = (ULONG_PTR)entered_event;
    block_params.release_event = (ULONG_PTR)release_event;
    thread_params.dispatcher = dispatcher;
    thread_params.handle = handle;
    thread_params.code = wow64_unixlib_lifecycle_block;
    thread_params.args = &block_params;
    thread_params.status = STATUS_PENDING;
    thread = CreateThread( NULL, 0, unixlib_dispatch_thread, &thread_params, 0, NULL );
    ok( !!thread, "failed to create dispatch thread, error %lu\n", GetLastError() );
    if (thread)
    {
        ok( WaitForSingleObject( entered_event, 5000 ) == WAIT_OBJECT_0,
            "blocking dispatch did not enter\n" );
        stale_handle = handle;
        status = __wine_unload_unix_lib( module );
        ok( !status, "active unload returned %#lx\n", status );
        SetEvent( release_event );
        ok( WaitForSingleObject( thread, 5000 ) == WAIT_OBJECT_0,
            "blocking dispatch did not complete\n" );
        ok( !thread_params.status, "blocking dispatch returned %#lx\n",
            thread_params.status );
        CloseHandle( thread );
        status = dispatcher( stale_handle, wow64_unixlib_lifecycle_zero_args, NULL );
        ok( status == STATUS_INVALID_PARAMETER,
            "post-drain stale dispatch returned %#lx\n", status );
    }
    else
    {
        status = __wine_unload_unix_lib( module );
        ok( !status, "fallback unload returned %#lx\n", status );
    }
    if (entered_event) CloseHandle( entered_event );
    if (release_event) CloseHandle( release_event );

    status = load_wow64_unixlib_lifecycle_helper(
        helper_path, ARRAY_SIZE(helper_path), &found, &module, &handle );
    ok( found && !status, "self-unload reload returned %#lx, found %u\n", status, found );
    if (!status && found)
    {
        unload_params.module = module;
        stale_handle = handle;
        status = dispatcher( handle, wow64_unixlib_lifecycle_self_unload, &unload_params );
        ok( !status, "self-unload dispatch returned %#lx\n", status );
        status = __wine_unload_unix_lib( module );
        ok( status == STATUS_INVALID_HANDLE,
            "post-self-unload replay returned %#lx\n", status );
        status = dispatcher( stale_handle, wow64_unixlib_lifecycle_zero_args, NULL );
        ok( status == STATUS_INVALID_PARAMETER,
            "self-unloaded stale dispatch returned %#lx\n", status );
    }

    {
        UNICODE_STRING name;
        UINT64 result[3];

        if (!(status = RtlDosPathNameToNtPathName_U_WithStatus(
                  helper_path, &name, NULL, NULL )))
        {
            for (i = 0; i < ARRAY_SIZE(result_lengths); i++)
            {
                memset( result, 0x55, sizeof(result) );
                status = NtQueryVirtualMemory( GetCurrentProcess(), &name,
                                               MemoryWineLoadUnixLibByName,
                                               result, result_lengths[i], NULL );
                if (result_lengths[i] == 8 || result_lengths[i] == 16)
                {
                    ok( !status, "result length %Iu returned %#lx\n",
                        result_lengths[i], status );
                    if (!status)
                    {
                        unixlib_module_t token = result[0];
                        NTSTATUS unload_status = __wine_unload_unix_lib( token );

                        ok( !unload_status, "length %Iu unload returned %#lx\n",
                            result_lengths[i], unload_status );
                    }
                }
                else
                    ok( status == STATUS_INFO_LENGTH_MISMATCH,
                        "result length %Iu returned %#lx\n", result_lengths[i], status );
            }

            {
                unixlib_handle_t published_handle;
                unixlib_module_t published_module;
                DWORD old_protect;
                UINT64 *readonly_result;

                readonly_result = VirtualAlloc( NULL, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                                PAGE_READWRITE );
                ok( !!readonly_result, "failed to allocate protected result page, error %lu\n",
                    GetLastError() );
                if (readonly_result)
                {
                    readonly_result[0] = 0x1111111111111111ull;
                    readonly_result[1] = 0x2222222222222222ull;
                    ok( VirtualProtect( readonly_result, 0x1000, PAGE_READONLY,
                                        &old_protect ),
                        "failed to protect result page, error %lu\n", GetLastError() );
                    status = NtQueryVirtualMemory( GetCurrentProcess(), &name,
                                                   MemoryWineLoadUnixLibByName,
                                                   readonly_result, 16, NULL );
                    ok( status == STATUS_ACCESS_VIOLATION,
                        "read-only result returned %#lx\n", status );
                    ok( readonly_result[0] == 0x1111111111111111ull &&
                        readonly_result[1] == 0x2222222222222222ull,
                        "read-only result was modified: %s/%s\n",
                        wine_dbgstr_longlong(readonly_result[0]),
                        wine_dbgstr_longlong(readonly_result[1]) );
                    ok( VirtualProtect( readonly_result, 0x1000, old_protect,
                                        &old_protect ),
                        "failed to restore result page, error %lu\n", GetLastError() );

                    status = NtQueryVirtualMemory( GetCurrentProcess(), &name,
                                                   MemoryWineLoadUnixLibByName,
                                                   readonly_result, 16, NULL );
                    ok( !status, "valid retry returned %#lx\n", status );
                    if (!status)
                    {
                        published_module = readonly_result[0];
                        published_handle = readonly_result[1];
                        status = __wine_unload_unix_lib( published_module );
                        ok( !status, "valid retry unload returned %#lx\n", status );
                        status = dispatcher( published_handle,
                                             wow64_unixlib_lifecycle_zero_args, NULL );
                        ok( status == STATUS_INVALID_PARAMETER,
                            "post-retry stale dispatch returned %#lx\n", status );
                    }
                    VirtualFree( readonly_result, 0, MEM_RELEASE );
                }
            }

            {
                unixlib_handle_t reused_handle = 0;
                const UINT iterations = WINE_UNIXLIB_DISPATCH_MAX_SLOTS + 32;

                for (i = 0; i < iterations; i++)
                {
                    status = NtQueryVirtualMemory( GetCurrentProcess(), &name,
                                                   MemoryWineLoadUnixLibByName,
                                                   result, 16, NULL );
                    if (status)
                    {
                        ok( 0, "sequential load %u returned %#lx\n", i, status );
                        break;
                    }
                    reused_handle = result[1];
                    status = __wine_unload_unix_lib( result[0] );
                    if (status)
                    {
                        ok( 0, "sequential unload %u returned %#lx\n", i, status );
                        break;
                    }
                    status = dispatcher( reused_handle,
                                         wow64_unixlib_lifecycle_zero_args, NULL );
                    if (status != STATUS_INVALID_PARAMETER)
                    {
                        ok( 0, "sequential stale dispatch %u returned %#lx\n", i, status );
                        break;
                    }
                }
                ok( i == iterations, "completed only %u/%u sequential load cycles\n",
                    i, iterations );
            }
            RtlFreeUnicodeString( &name );
        }
        else ok( 0, "failed to convert helper path, status %#lx\n", status );
    }
}

static void test_unixlib_zero_args_requires_wow64_model(void)
{
#ifdef __aarch64__
    unix_call_dispatcher_func *dispatcher_ptr, dispatcher;
    struct wow64_unixlib_zero_count_result count = {0};
    unixlib_handle_t native_handle, tagged_handle;
    unixlib_module_t native_module, tagged_module;
    UINT64 result[2];
    UNICODE_STRING name;
    WCHAR helper_path[MAX_PATH];
    HMODULE ntdll;
    NTSTATUS status;
    BOOL found;

    if (is_wow64) return;
    status = load_wow64_unixlib_lifecycle_helper(
        helper_path, ARRAY_SIZE(helper_path), &found, &native_module, &native_handle );
    if (!found)
    {
        skip( "the native Unixlib lifecycle helper is unavailable\n" );
        return;
    }
    ok( !status, "failed to load native helper %s, status %#lx\n",
        wine_dbgstr_w(helper_path), status );
    if (status) return;

    ntdll = GetModuleHandleA( "ntdll.dll" );
    dispatcher_ptr = pRtlFindExportedRoutineByName( ntdll, "__wine_unix_call_dispatcher" );
    if (!dispatcher_ptr || !(dispatcher = *dispatcher_ptr))
    {
        skip( "the Wine Unix-call dispatcher is unavailable\n" );
        __wine_unload_unix_lib( native_module );
        return;
    }
    status = RtlDosPathNameToNtPathName_U_WithStatus( helper_path, &name, NULL, NULL );
    ok( !status, "failed to convert helper path, status %#lx\n", status );
    if (status)
    {
        __wine_unload_unix_lib( native_module );
        return;
    }
    status = NtQueryVirtualMemory( GetCurrentProcess(), &name,
                                   MemoryWineLoadUnixLibByNameWow64,
                                   result, sizeof(result), NULL );
    RtlFreeUnicodeString( &name );
    ok( !status, "failed to load tagged helper, status %#lx\n", status );
    if (status)
    {
        __wine_unload_unix_lib( native_module );
        return;
    }
    tagged_module = result[0];
    tagged_handle = result[1];
    ok( wine_unixlib_decode_dispatch_handle( tagged_handle, NULL, NULL ),
        "expected generation-tagged handle, got %s\n",
        wine_dbgstr_longlong(tagged_handle) );

    status = dispatcher( tagged_handle, wow64_unixlib_lifecycle_zero_args, NULL );
    ok( status == STATUS_INVALID_PARAMETER,
        "zero-argument dispatch without a paired WoW64 model returned %#lx\n", status );
    status = dispatcher( native_handle, x18_dispatch_get_zero_count, &count );
    ok( !status, "native zero-count dispatch returned %#lx\n", status );
    ok( !count.count, "zero-argument side effect ran %ld times without a WoW64 model\n",
        count.count );

    status = __wine_unload_unix_lib( tagged_module );
    ok( !status, "tagged helper unload returned %#lx\n", status );
    status = __wine_unload_unix_lib( native_module );
    ok( !status, "native helper unload returned %#lx\n", status );
#endif
}

#ifdef __i386__
static USHORT current_machine = IMAGE_FILE_MACHINE_I386;
static USHORT native_machine = IMAGE_FILE_MACHINE_I386;
#elif defined __x86_64__
static USHORT current_machine = IMAGE_FILE_MACHINE_AMD64;
static USHORT native_machine = IMAGE_FILE_MACHINE_AMD64;
#elif defined __arm__
static USHORT current_machine = IMAGE_FILE_MACHINE_ARMNT;
static USHORT native_machine = IMAGE_FILE_MACHINE_ARMNT;
#elif defined __aarch64__
static USHORT current_machine = IMAGE_FILE_MACHINE_ARM64;
static USHORT native_machine = IMAGE_FILE_MACHINE_ARM64;
#else
static USHORT current_machine;
static USHORT native_machine;
#endif

struct arm64ec_shared_info
{
    ULONG     Wow64ExecuteFlags;
    USHORT    NativeMachineType;
    USHORT    EmulatedMachineType;
    ULONGLONG SectionHandle;
    ULONGLONG CrossProcessWorkList;
    ULONGLONG unknown;
};

static BOOL is_machine_32bit( USHORT machine )
{
    return machine == IMAGE_FILE_MACHINE_I386 || machine == IMAGE_FILE_MACHINE_ARMNT;
}

static void init(void)
{
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );

    if (!IsWow64Process( GetCurrentProcess(), &is_wow64 )) is_wow64 = FALSE;

#ifndef _WIN64
    if (is_wow64)
    {
        TEB64 *teb64 = ULongToPtr( NtCurrentTeb()->GdiBatchCount );

        if (teb64)
        {
            PEB64 *peb64 = ULongToPtr(teb64->Peb);
            old_wow64 = !peb64->LdrData;
        }
    }
#endif

#define GET_PROC(func) p##func = (void *)GetProcAddress( ntdll, #func )
    GET_PROC( NtMapViewOfSectionEx );
    GET_PROC( NtQuerySystemInformationEx );
    GET_PROC( RtlGetNativeSystemInformation );
    GET_PROC( RtlOpenCrossProcessEmulatorWorkConnection );
    GET_PROC( RtlFindExportedRoutineByName );
    GET_PROC( RtlWow64GetCurrentMachine );
    GET_PROC( RtlWow64GetProcessMachines );
    GET_PROC( RtlWow64GetSharedInfoProcess );
    GET_PROC( RtlWow64IsWowGuestMachineSupported );
#ifndef __arm__
    GET_PROC( NtSetLdtEntries );
#endif
#ifdef __x86_64__
    GET_PROC( KiUserExceptionDispatcher );
#endif
#ifdef _WIN64
    GET_PROC( RtlWow64GetCpuAreaInfo );
    GET_PROC( RtlWow64GetThreadContext );
    GET_PROC( RtlWow64GetThreadSelectorEntry );
    GET_PROC( RtlWow64PopAllCrossProcessWorkFromWorkList );
    GET_PROC( RtlWow64PopCrossProcessWorkFromFreeList );
    GET_PROC( RtlWow64PushCrossProcessWorkOntoFreeList );
    GET_PROC( RtlWow64PushCrossProcessWorkOntoWorkList );
    GET_PROC( RtlWow64RequestCrossProcessHeavyFlush );
    GET_PROC( ProcessPendingCrossProcessEmulatorWork );
#else
    GET_PROC( NtQuerySystemInformation );
    GET_PROC( NtWow64AllocateVirtualMemory64 );
    GET_PROC( NtWow64GetNativeSystemInformation );
    GET_PROC( NtWow64IsProcessorFeaturePresent );
    GET_PROC( NtWow64QueryInformationProcess64 );
    GET_PROC( NtWow64ReadVirtualMemory64 );
    GET_PROC( NtWow64WriteVirtualMemory64 );
#endif
#undef GET_PROC

    if (pNtQuerySystemInformationEx)
    {
        SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION machines[8];
        HANDLE process = GetCurrentProcess();
        NTSTATUS status = pNtQuerySystemInformationEx( SystemSupportedProcessorArchitectures2, &process,
                                                       sizeof(process), machines, sizeof(machines), NULL );
        if (status)
            status = pNtQuerySystemInformationEx( SystemSupportedProcessorArchitectures, &process,
                                                  sizeof(process), machines, sizeof(machines), NULL );
        if (!status)
            for (int i = 0; machines[i].Machine; i++)
                trace( "machine %04x kernel %u user %u native %u process %u wow64 %u\n",
                       machines[i].Machine, machines[i].KernelMode, machines[i].UserMode,
                       machines[i].Native, machines[i].Process, machines[i].WoW64Container );
    }

    if (pRtlGetNativeSystemInformation)
    {
        SYSTEM_CPU_INFORMATION info;
        ULONG len;

        pRtlGetNativeSystemInformation( SystemCpuInformation, &info, sizeof(info), &len );
        switch (info.ProcessorArchitecture)
        {
        case PROCESSOR_ARCHITECTURE_ARM64:
            native_machine = IMAGE_FILE_MACHINE_ARM64;
            break;
        case PROCESSOR_ARCHITECTURE_AMD64:
            native_machine = IMAGE_FILE_MACHINE_AMD64;
            break;
        }
    }

    trace( "current %04x native %04x\n", current_machine, native_machine );

#ifndef _WIN64
    if (native_machine == IMAGE_FILE_MACHINE_AMD64)
        code_mem = VirtualAlloc( NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
#endif
}

static BOOL create_process_machine( char *cmdline, DWORD flags, USHORT machine, PROCESS_INFORMATION *pi )
{
    struct _PROC_THREAD_ATTRIBUTE_LIST *list;
    STARTUPINFOEXA si = {{ sizeof(si) }};
    SIZE_T size = 1024;
    BOOL ret;

    si.lpAttributeList = list = malloc( size );
    InitializeProcThreadAttributeList( list, 1, 0, &size );
    UpdateProcThreadAttribute( list, 0, PROC_THREAD_ATTRIBUTE_MACHINE_TYPE,
                               &machine, sizeof(machine), NULL, NULL );
    ret = CreateProcessA( NULL, cmdline, NULL, NULL, FALSE,
                          EXTENDED_STARTUPINFO_PRESENT | flags, NULL, NULL, &si.StartupInfo, pi );
    DeleteProcThreadAttributeList( list );
    free( list );
    return ret;
}

#ifndef _WIN64

static BOOL read_shadow_shared_data( HANDLE process, BYTE *value )
{
    ULONG64 size = 0;
    NTSTATUS status;

    status = pNtWow64ReadVirtualMemory64( process,
                                         WINE_LOW_VA_SHADOW_BASE + WINE_USER_SHARED_DATA_ADDRESS,
                                         value, sizeof(*value), &size );
    return !status && size == sizeof(*value);
}

static void check_remote_memory_minimal_rights( HANDLE process, void *address )
{
    static const BYTE value = 0xa5;
    HANDLE limited;
    SIZE_T size;
    void *base;
    ULONG old_protect;
    BYTE result = 0;
    NTSTATUS status;
    DWORD pid = GetProcessId( process );

    limited = OpenProcess( PROCESS_VM_READ, FALSE, pid );
    ok( !!limited, "OpenProcess(PROCESS_VM_READ) failed %lu\n", GetLastError() );
    if (limited)
    {
        size = 0;
        status = NtReadVirtualMemory( limited, address, &result, sizeof(result), &size );
        ok( !status && size == sizeof(result),
            "minimal-rights NtReadVirtualMemory failed %#lx, size %Iu\n", status, size );
        CloseHandle( limited );
    }

    limited = OpenProcess( PROCESS_VM_WRITE, FALSE, pid );
    ok( !!limited, "OpenProcess(PROCESS_VM_WRITE) failed %lu\n", GetLastError() );
    if (limited)
    {
        size = 0;
        status = NtWriteVirtualMemory( limited, address, &value, sizeof(value), &size );
        ok( !status && size == sizeof(value),
            "minimal-rights NtWriteVirtualMemory failed %#lx, size %Iu\n", status, size );
        CloseHandle( limited );
    }
    result = 0;
    size = 0;
    status = NtReadVirtualMemory( process, address, &result, sizeof(result), &size );
    ok( !status && size == sizeof(result) && result == value,
        "read after minimal-rights write failed %#lx, size %Iu, value %#x\n",
        status, size, result );

    limited = OpenProcess( PROCESS_VM_OPERATION, FALSE, pid );
    ok( !!limited, "OpenProcess(PROCESS_VM_OPERATION) failed %lu\n", GetLastError() );
    if (limited)
    {
        base = address;
        size = 1;
        status = NtProtectVirtualMemory( limited, &base, &size, PAGE_READONLY, &old_protect );
        ok( !status, "minimal-rights NtProtectVirtualMemory failed %#lx\n", status );
        if (!status)
        {
            base = address;
            size = 1;
            status = NtProtectVirtualMemory( limited, &base, &size, old_protect, &old_protect );
            ok( !status, "minimal-rights NtProtectVirtualMemory restore failed %#lx\n", status );
        }
        CloseHandle( limited );
    }

    limited = OpenProcess( SYNCHRONIZE, FALSE, pid );
    ok( !!limited, "OpenProcess(SYNCHRONIZE) failed %lu\n", GetLastError() );
    if (limited)
    {
        status = NtReadVirtualMemory( limited, address, &result, sizeof(result), &size );
        ok( status == STATUS_ACCESS_DENIED, "no-VM-rights read returned %#lx\n", status );
        CloseHandle( limited );
    }

    status = NtReadVirtualMemory( (HANDLE)(ULONG_PTR)0xdeadbeef, address,
                                  &result, sizeof(result), &size );
    ok( status == STATUS_INVALID_HANDLE, "invalid-handle read returned %#lx\n", status );
}

static void check_remote_memory_region( HANDLE process, BOOL check_minimal_rights )
{
    static const BYTE value = 0x5a;
    MEMORY_BASIC_INFORMATION info;
    LARGE_INTEGER section_size;
    SIZE_T size, ret_size = 0;
    void *address;
    HANDLE section;
    ULONG old_protect;
    BYTE result = 0;
    NTSTATUS status;

    address = NULL;
    size = 0x10000;
    status = NtAllocateVirtualMemory( process, &address, 0, &size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "NtAllocateVirtualMemory failed %#lx\n", status );
    if (!status)
    {
        status = NtWriteVirtualMemory( process, address, &value, sizeof(value), &ret_size );
        ok( !status && ret_size == sizeof(value), "NtWriteVirtualMemory failed %#lx, size %Iu\n",
            status, ret_size );
        status = NtReadVirtualMemory( process, address, &result, sizeof(result), &ret_size );
        ok( !status && ret_size == sizeof(result), "NtReadVirtualMemory failed %#lx, size %Iu\n",
            status, ret_size );
        ok( result == value, "read %#x, expected %#x\n", result, value );

        if (check_minimal_rights) check_remote_memory_minimal_rights( process, address );

        status = NtQueryVirtualMemory( process, address, MemoryBasicInformation,
                                       &info, sizeof(info), &ret_size );
        ok( !status, "NtQueryVirtualMemory failed %#lx\n", status );
        if (!status)
        {
            ok( info.BaseAddress == address, "got base %p, expected %p\n",
                info.BaseAddress, address );
            ok( info.State == MEM_COMMIT, "got state %#lx\n", info.State );
        }

        size = 1;
        status = NtProtectVirtualMemory( process, &address, &size, PAGE_READONLY, &old_protect );
        ok( !status, "NtProtectVirtualMemory failed %#lx\n", status );

        size = 0;
        status = NtFreeVirtualMemory( process, &address, &size, MEM_RELEASE );
        ok( !status, "NtFreeVirtualMemory failed %#lx\n", status );
    }

    section_size.QuadPart = 0x10000;
    status = NtCreateSection( &section, SECTION_ALL_ACCESS, NULL, &section_size,
                              PAGE_READWRITE, SEC_COMMIT, NULL );
    ok( !status, "NtCreateSection failed %#lx\n", status );
    if (status) return;

    address = NULL;
    size = 0;
    status = NtMapViewOfSection( section, process, &address, 0, 0, NULL, &size,
                                 ViewUnmap, 0, PAGE_READWRITE );
    ok( !status, "NtMapViewOfSection failed %#lx\n", status );
    if (!status)
    {
        status = NtQueryVirtualMemory( process, address, MemoryBasicInformation,
                                       &info, sizeof(info), &ret_size );
        ok( !status, "NtQueryVirtualMemory failed %#lx\n", status );
        if (!status) ok( info.State == MEM_COMMIT, "got state %#lx\n", info.State );
        status = NtUnmapViewOfSection( process, address );
        ok( !status, "NtUnmapViewOfSection failed %#lx\n", status );
    }
    NtClose( section );
}

static void check_native_low_address_is_raw( HANDLE process )
{
    const void *low_address = (void *)(ULONG_PTR)WINE_USER_SHARED_DATA_ADDRESS;
    MEMORY_BASIC_INFORMATION info;
    BYTE before, after;
    SIZE_T size, ret_size;
    void *address;
    ULONG old_protect;
    NTSTATUS status;

    if (!read_shadow_shared_data( process, &before ))
    {
        skip( "native target has no readable shadow shared-data mapping\n" );
        return;
    }

    status = NtQueryVirtualMemory( process, low_address, MemoryBasicInformation,
                                   &info, sizeof(info), &ret_size );
    ok( !status, "NtQueryVirtualMemory failed %#lx\n", status );
    if (!status) ok( info.State != MEM_COMMIT, "raw low address unexpectedly resolved to a mapping\n" );

    status = NtReadVirtualMemory( process, low_address, &after, sizeof(after), &ret_size );
    ok( status != STATUS_SUCCESS, "raw low NtReadVirtualMemory reached the shadow mapping\n" );

    status = NtWriteVirtualMemory( process, (void *)low_address, &before, sizeof(before), &ret_size );
    ok( status != STATUS_SUCCESS, "raw low NtWriteVirtualMemory reached the shadow mapping\n" );
    ok( read_shadow_shared_data( process, &after ) && after == before,
        "shadow shared data changed after raw low write\n" );

    address = (void *)low_address;
    size = 1;
    status = NtProtectVirtualMemory( process, &address, &size, PAGE_READONLY, &old_protect );
    ok( status != STATUS_SUCCESS, "raw low NtProtectVirtualMemory reached the shadow mapping\n" );
    ok( read_shadow_shared_data( process, &after ) && after == before,
        "shadow shared data became unreadable after raw low protect\n" );

    address = (void *)low_address;
    size = 0;
    status = NtFreeVirtualMemory( process, &address, &size, MEM_RELEASE );
    ok( status != STATUS_SUCCESS, "raw low NtFreeVirtualMemory reached the shadow mapping\n" );
    ok( read_shadow_shared_data( process, &after ) && after == before,
        "shadow shared data became unreadable after raw low free\n" );

    status = NtUnmapViewOfSection( process, (void *)low_address );
    ok( status != STATUS_SUCCESS, "raw low NtUnmapViewOfSection reached the shadow mapping\n" );
    ok( read_shadow_shared_data( process, &after ) && after == before,
        "shadow shared data became unreadable after raw low unmap\n" );
}

static void test_remote_memory_address_codec(void)
{
    char native_cmd[] = "C:\\windows\\sysnative\\cmd.exe /c ping -n 30 127.0.0.1 >nul";
    char wow64_cmd[] = "C:\\windows\\syswow64\\cmd.exe /c ping -n 30 127.0.0.1 >nul";
    char image_path[MAX_PATH], cmdline[MAX_PATH + 3];
    STARTUPINFOA startup = { sizeof(startup) };
    PROCESS_INFORMATION pi;
    WINE_PROCESS_VM_INFORMATION vm_info;
    USHORT process_machine = 0xffff, target_native_machine = 0xffff;
    DWORD len;
    BYTE value;
    BOOL ret;
    NTSTATUS status;

    if (current_machine != IMAGE_FILE_MACHINE_I386 ||
        native_machine != IMAGE_FILE_MACHINE_ARM64 ||
        !pRtlWow64GetProcessMachines ||
        !pNtWow64ReadVirtualMemory64 ||
        !read_shadow_shared_data( GetCurrentProcess(), &value ))
        return;

    len = GetModuleFileNameA( NULL, image_path, ARRAY_SIZE(image_path) );
    ok( len && len < ARRAY_SIZE(image_path), "GetModuleFileNameA failed %lu\n", GetLastError() );
    if (len && len < ARRAY_SIZE(image_path))
    {
        sprintf( cmdline, "\"%s\"", image_path );
        ret = CreateProcessA( NULL, cmdline, NULL, NULL, FALSE, CREATE_SUSPENDED,
                              NULL, NULL, &startup, &pi );
        ok( ret, "CreateProcessA with a non-redirected image name failed %lu\n", GetLastError() );
        if (ret)
        {
            TerminateProcess( pi.hProcess, 0 );
            CloseHandle( pi.hProcess );
            CloseHandle( pi.hThread );
        }
    }

    if (create_process_machine( wow64_cmd, 0, IMAGE_FILE_MACHINE_I386, &pi ))
    {
        winetest_push_context( "i386 target" );
        memset( &vm_info, 0, sizeof(vm_info) );
        status = NtQueryVirtualMemory( pi.hProcess, NULL,
                                       MemoryWineProcessVmMachineInformation,
                                       &vm_info, sizeof(vm_info), NULL );
        ok( !status, "VM metadata query failed %#lx\n", status );
        ok( vm_info.Version == WINE_PROCESS_VM_INFORMATION_VERSION,
            "got version %lu\n", vm_info.Version );
        ok( vm_info.Size == sizeof(vm_info), "got size %lu\n", vm_info.Size );
        ok( vm_info.Machine == IMAGE_FILE_MACHINE_I386, "got machine %#x\n", vm_info.Machine );
        ok( vm_info.Flags == WINE_PROCESS_VM_FLAG_WOW64_TRANSLATED,
            "got flags %#x\n", vm_info.Flags );
        ok( !vm_info.Reserved, "got reserved %#lx\n", vm_info.Reserved );
        status = pRtlWow64GetProcessMachines( pi.hProcess, &process_machine,
                                              &target_native_machine );
        ok( !status, "RtlWow64GetProcessMachines failed %#lx\n", status );
        ok( process_machine == IMAGE_FILE_MACHINE_I386, "got process machine %#x\n",
            process_machine );
        ok( target_native_machine == IMAGE_FILE_MACHINE_ARM64, "got native machine %#x\n",
            target_native_machine );
        if (!status && process_machine == IMAGE_FILE_MACHINE_I386)
            check_remote_memory_region( pi.hProcess, TRUE );
        TerminateProcess( pi.hProcess, 0 );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
        winetest_pop_context();
    }
    else win_skip( "could not start i386 target: %lu\n", GetLastError() );

    if (create_process_machine( native_cmd, 0, IMAGE_FILE_MACHINE_ARM64, &pi ))
    {
        winetest_push_context( "ARM64 target" );
        memset( &vm_info, 0, sizeof(vm_info) );
        status = NtQueryVirtualMemory( pi.hProcess, NULL,
                                       MemoryWineProcessVmMachineInformation,
                                       &vm_info, sizeof(vm_info), NULL );
        ok( !status, "VM metadata query failed %#lx\n", status );
        ok( vm_info.Version == WINE_PROCESS_VM_INFORMATION_VERSION,
            "got version %lu\n", vm_info.Version );
        ok( vm_info.Size == sizeof(vm_info), "got size %lu\n", vm_info.Size );
        ok( vm_info.Machine == IMAGE_FILE_MACHINE_ARM64, "got machine %#x\n", vm_info.Machine );
        ok( !vm_info.Flags, "got flags %#x\n", vm_info.Flags );
        ok( !vm_info.Reserved, "got reserved %#lx\n", vm_info.Reserved );
        status = pRtlWow64GetProcessMachines( pi.hProcess, &process_machine,
                                              &target_native_machine );
        ok( !status, "RtlWow64GetProcessMachines failed %#lx\n", status );
        ok( !process_machine, "got emulated process machine %#x\n", process_machine );
        ok( target_native_machine == IMAGE_FILE_MACHINE_ARM64, "got native machine %#x\n",
            target_native_machine );
        if (!status && !process_machine)
        {
            check_remote_memory_region( pi.hProcess, FALSE );
            check_native_low_address_is_raw( pi.hProcess );
        }
        TerminateProcess( pi.hProcess, 0 );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
        winetest_pop_context();
    }
    else win_skip( "could not start ARM64 target: %lu\n", GetLastError() );
}

static void test_remote_memory_codec_cleanup(void)
{
    MEMORY_BASIC_INFORMATION info;
    MEMORY_WORKING_SET_EX_INFORMATION working_set;
    HANDLE self = NULL;
    ULONG before, after, i;
    BYTE value;
    SIZE_T size;
    DWORD exception;
    NTSTATUS status;

    if (current_machine != IMAGE_FILE_MACHINE_I386 ||
        native_machine != IMAGE_FILE_MACHINE_ARM64 ||
        !read_shadow_shared_data( GetCurrentProcess(), &value ))
        return;

    /* Warm the exception path before taking the handle-count baseline. */
    exception = 0;
    __TRY
    {
        NtQueryVirtualMemory( GetCurrentProcess(), NtCurrentTeb(),
                              MemoryBasicInformation, NULL, sizeof(info), NULL );
    }
    __EXCEPT_ALL
    {
        exception = GetExceptionCode();
    }
    __ENDTRY
    ok( exception == STATUS_ACCESS_VIOLATION, "warm query raised %#lx\n", exception );

    status = NtDuplicateObject( GetCurrentProcess(), GetCurrentProcess(),
                                GetCurrentProcess(), &self, 0, 0, DUPLICATE_SAME_ACCESS );
    ok( !status, "NtDuplicateObject failed %#lx\n", status );
    if (status) return;

    memset( &working_set, 0, sizeof(working_set) );
    working_set.VirtualAddress = &working_set;
    status = NtQueryVirtualMemory( self, NULL, MemoryWorkingSetExInformation,
                                   &working_set, sizeof(working_set), NULL );
    ok( !status, "duplicated-self working-set query failed %#lx\n", status );

    status = NtQueryInformationProcess( GetCurrentProcess(), ProcessHandleCount,
                                        &before, sizeof(before), NULL );
    ok( !status, "ProcessHandleCount query failed %#lx\n", status );
    if (status)
    {
        NtClose( self );
        return;
    }

    for (i = 0; i < 32; i++)
    {
        size = 0x1000;
        exception = 0;
        __TRY
        {
            NtAllocateVirtualMemory( self, NULL, 0, &size, MEM_RESERVE,
                                     PAGE_READWRITE );
        }
        __EXCEPT_ALL
        {
            exception = GetExceptionCode();
        }
        __ENDTRY
        ok( exception == STATUS_ACCESS_VIOLATION,
            "allocate iteration %lu raised %#lx\n", i, exception );

        exception = 0;
        __TRY
        {
            NtQueryVirtualMemory( self, NtCurrentTeb(), MemoryBasicInformation,
                                  NULL, sizeof(info), NULL );
        }
        __EXCEPT_ALL
        {
            exception = GetExceptionCode();
        }
        __ENDTRY
        ok( exception == STATUS_ACCESS_VIOLATION,
            "query iteration %lu raised %#lx\n", i, exception );
    }

    status = NtQueryInformationProcess( GetCurrentProcess(), ProcessHandleCount,
                                        &after, sizeof(after), NULL );
    ok( !status, "ProcessHandleCount query failed %#lx\n", status );
    if (!status) ok( after == before, "handle count changed from %lu to %lu\n", before, after );

    status = NtQueryVirtualMemory( (HANDLE)(ULONG_PTR)0xdeadbeef, NULL,
                                   MemoryBasicInformation, &info,
                                   sizeof(info) - 1, NULL );
    ok( status == STATUS_INFO_LENGTH_MISMATCH,
        "invalid handle with short buffer returned %#lx\n", status );
    status = NtQueryVirtualMemory( (HANDLE)(ULONG_PTR)0xdeadbeef, NULL,
                                   (MEMORY_INFORMATION_CLASS)0xdead, &info,
                                   sizeof(info), NULL );
    ok( status == STATUS_INVALID_INFO_CLASS,
        "invalid handle with unsupported class returned %#lx\n", status );
    NtClose( self );
}

static void init_raw_create_event_request( struct __server_request_info *req,
                                           const struct object_attributes *objattr )
{
    memset( req, 0, sizeof(*req) );
    req->u.req.request_header.req = REQ_create_event;
    req->u.req.create_event_request.access = EVENT_ALL_ACCESS;
    req->u.req.create_event_request.manual_reset = FALSE;
    req->u.req.create_event_request.initial_state = FALSE;
    req->u.req.request_header.request_size = sizeof(*objattr);
    req->data_count = 1;
    req->data[0].ptr = objattr;
    req->data[0].size = sizeof(*objattr);
}

static NTSTATUS call_raw_create_event( unsigned int (CDECL *server_call)(void *),
                                       struct __server_request_info *req,
                                       const struct object_attributes *objattr )
{
    NTSTATUS status;

    init_raw_create_event_request( req, objattr );
    status = server_call( req );
    if (!status) NtClose( wine_server_ptr_handle( req->u.reply.create_event_reply.handle ) );
    return status;
}

static void init_raw_get_process_info_request( struct __server_request_info *req,
                                               void *reply_data, data_size_t reply_size )
{
    memset( req, 0, sizeof(*req) );
    req->u.req.request_header.req = REQ_get_process_info;
    req->u.req.get_process_info_request.handle = wine_server_obj_handle( NtCurrentProcess() );
    req->u.req.request_header.reply_size = reply_size;
    req->reply_data = reply_data;
}

static void test_wow64_raw_server_call_protection(void)
{
    unsigned int (CDECL *server_call)(void *);
    struct object_attributes objattr = {0};
    struct __server_request_info *req;
    void *base = NULL, *watch_base = NULL, *protect_page, *reply_page;
    void *written[8];
    SIZE_T region_size;
    ULONG before, after, old_protect, granularity;
    ULONG_PTR count;
    data_size_t reply_size;
    NTSTATUS status;
    UINT ret;
    BYTE probe;
    HMODULE ntdll;

    if (current_machine != IMAGE_FILE_MACHINE_I386 ||
        native_machine != IMAGE_FILE_MACHINE_ARM64 ||
        !read_shadow_shared_data( GetCurrentProcess(), &probe ))
        return;

    ntdll = GetModuleHandleA( "ntdll.dll" );
    server_call = (void *)pRtlFindExportedRoutineByName( ntdll, "wine_server_call" );
    if (!server_call)
    {
        win_skip( "wine_server_call is not exported\n" );
        return;
    }

    region_size = 0x20000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &base, 0, &region_size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "NtAllocateVirtualMemory failed %#lx\n", status );
    if (status) return;
    req = (struct __server_request_info *)((BYTE *)base + 0x1000);
    reply_page = (BYTE *)base + 0x10000;

    status = NtQueryInformationProcess( NtCurrentProcess(), ProcessHandleCount,
                                        &before, sizeof(before), NULL );
    ok( !status, "ProcessHandleCount query failed %#lx\n", status );
    init_raw_create_event_request( req, &objattr );
    protect_page = req;
    region_size = 0x1000;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &region_size,
                                     PAGE_READONLY, &old_protect );
    ok( !status, "fixed reply protect failed %#lx\n", status );
    if (!status)
    {
        status = server_call( req );
        ok( status == STATUS_ACCESS_VIOLATION,
            "read-only fixed reply returned %#lx\n", status );
        status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &region_size,
                                         PAGE_READWRITE, &old_protect );
        ok( !status, "fixed reply restore failed %#lx\n", status );
    }
    status = NtQueryInformationProcess( NtCurrentProcess(), ProcessHandleCount,
                                        &after, sizeof(after), NULL );
    ok( !status, "ProcessHandleCount query failed %#lx\n", status );
    if (!status) ok( after == before, "protected descriptor leaked a handle (%lu -> %lu)\n",
                     before, after );
    status = call_raw_create_event( server_call, req, &objattr );
    ok( !status, "valid call after fixed reply fault failed %#lx\n", status );

    init_raw_create_event_request( req, &objattr );
    req->data_count = __SERVER_MAX_DATA + 1;
    status = server_call( req );
    ok( status == STATUS_INVALID_PARAMETER, "oversized data count returned %#lx\n", status );
    status = call_raw_create_event( server_call, req, &objattr );
    ok( !status, "valid call after oversized count failed %#lx\n", status );

    init_raw_create_event_request( req, &objattr );
    req->u.req.request_header.request_size = 1;
    req->data[0].ptr = NULL;
    req->data[0].size = 1;
    status = server_call( req );
    ok( status == STATUS_ACCESS_VIOLATION, "NULL payload returned %#lx\n", status );
    status = call_raw_create_event( server_call, req, &objattr );
    ok( !status, "valid call after NULL payload failed %#lx\n", status );

    init_raw_create_event_request( req, &objattr );
    req->u.req.request_header.request_size = 4;
    req->data[0].ptr = (void *)(ULONG_PTR)0xfffffffe;
    req->data[0].size = 4;
    status = server_call( req );
    ok( status == STATUS_ACCESS_VIOLATION, "wrapping payload returned %#lx\n", status );
    status = call_raw_create_event( server_call, req, &objattr );
    ok( !status, "valid call after wrapping payload failed %#lx\n", status );

    init_raw_create_event_request( req, &objattr );
    req->u.req.request_header.request_size = ~(data_size_t)0;
    req->data[0].ptr = (void *)(ULONG_PTR)1;
    req->data[0].size = ~(data_size_t)0;
    status = server_call( req );
    ok( status == STATUS_INVALID_PARAMETER, "header-overflow payload returned %#lx\n", status );
    status = call_raw_create_event( server_call, req, &objattr );
    ok( !status, "valid call after header overflow failed %#lx\n", status );

    init_raw_get_process_info_request( req, reply_page, sizeof(struct pe_image_info) );
    protect_page = reply_page;
    region_size = 0x1000;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &region_size,
                                     PAGE_READONLY, &old_protect );
    ok( !status, "variable reply protect failed %#lx\n", status );
    if (!status)
    {
        status = server_call( req );
        ok( status == STATUS_ACCESS_VIOLATION,
            "read-only variable reply returned %#lx\n", status );
        status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &region_size,
                                         PAGE_READWRITE, &old_protect );
        ok( !status, "variable reply restore failed %#lx\n", status );
    }
    init_raw_get_process_info_request( req, reply_page, sizeof(struct pe_image_info) );
    status = server_call( req );
    ok( !status, "valid get_process_info after reply fault failed %#lx\n", status );

    region_size = 0x30000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &watch_base, 0, &region_size,
                                      MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                                      PAGE_READWRITE );
    ok( !status, "write-watch allocation failed %#lx\n", status );
    if (!status)
    {
        req = (struct __server_request_info *)watch_base;
        reply_page = (BYTE *)watch_base + 0x10000;
        init_raw_get_process_info_request( req, reply_page, 0x2000 );
        count = ARRAY_SIZE( written );
        ret = GetWriteWatch( WRITE_WATCH_FLAG_RESET, watch_base, 0x30000,
                             written, &count, &granularity );
        ok( !ret, "GetWriteWatch reset failed %u\n", ret );
        status = server_call( req );
        ok( !status, "write-watch get_process_info failed %#lx\n", status );
        reply_size = wine_server_reply_size( &req->u.reply );
        ok( reply_size && reply_size < 0x1000, "unexpected reply size %u\n", reply_size );
        count = ARRAY_SIZE( written );
        ret = GetWriteWatch( 0, watch_base, 0x30000, written, &count, &granularity );
        ok( !ret, "GetWriteWatch failed %u\n", ret );
        ok( count == 2, "expected two written pages, got %Iu\n", count );
        if (count == 2)
        {
            ok( written[0] == watch_base, "fixed reply page %p, expected %p\n",
                written[0], watch_base );
            ok( written[1] == reply_page, "variable reply page %p, expected %p\n",
                written[1], reply_page );
        }
    }

    if (watch_base)
    {
        region_size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &watch_base, &region_size, MEM_RELEASE );
    }
    region_size = 0;
    NtFreeVirtualMemory( NtCurrentProcess(), &base, &region_size, MEM_RELEASE );
}

static void test_wow64_handle_pair_publication(void)
{
    NTSTATUS (CDECL *publish_pair)(ULONG *,ULONG,ULONG *,ULONG);
    ULONG *first, *second, old_protect, granularity;
    void *base = NULL, *protect_page;
    void *written[4];
    SIZE_T region_size;
    ULONG_PTR count;
    NTSTATUS status;
    UINT ret;
    HMODULE ntdll;

    if (current_machine != IMAGE_FILE_MACHINE_I386) return;

    ntdll = GetModuleHandleA( "ntdll.dll" );
    publish_pair = (void *)pRtlFindExportedRoutineByName(
        ntdll, "__wine_wow64_publish_handle_pair" );
    if (!publish_pair)
    {
        win_skip( "__wine_wow64_publish_handle_pair is not exported\n" );
        return;
    }

    region_size = 0x30000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &base, 0, &region_size,
                                      MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                                      PAGE_READWRITE );
    ok( !status, "write-watch allocation failed %#lx\n", status );
    if (status) return;
    first = (ULONG *)((BYTE *)base + 0x1000);
    second = (ULONG *)((BYTE *)base + 0x10000);
    *first = 0x11111111;
    *second = 0x22222222;

    protect_page = second;
    region_size = 0x1000;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &region_size,
                                     PAGE_NOACCESS, &old_protect );
    ok( !status, "second lane protect failed %#lx\n", status );
    if (!status)
    {
        status = publish_pair( first, 0xaaaaaaaa, second, 0xbbbbbbbb );
        ok( status == STATUS_ACCESS_VIOLATION, "second-lane fault returned %#lx\n", status );
        ok( *first == 0x11111111, "first output changed to %#lx\n", *first );
        status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &region_size,
                                         PAGE_READWRITE, &old_protect );
        ok( !status, "second lane restore failed %#lx\n", status );
        ok( *second == 0x22222222, "second output changed to %#lx\n", *second );
    }

    protect_page = first;
    region_size = 0x1000;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &region_size,
                                     PAGE_NOACCESS, &old_protect );
    ok( !status, "first lane protect failed %#lx\n", status );
    if (!status)
    {
        status = publish_pair( first, 0xaaaaaaaa, second, 0xbbbbbbbb );
        ok( status == STATUS_ACCESS_VIOLATION, "first-lane fault returned %#lx\n", status );
        status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_page, &region_size,
                                         PAGE_READWRITE, &old_protect );
        ok( !status, "first lane restore failed %#lx\n", status );
        ok( *first == 0x11111111, "first output changed to %#lx\n", *first );
        ok( *second == 0x22222222, "second output changed to %#lx\n", *second );
    }

    status = publish_pair( first, 0x33333333, first, 0x44444444 );
    ok( !status, "same-address publication failed %#lx\n", status );
    ok( *first == 0x44444444, "same-address output is %#lx\n", *first );

    count = ARRAY_SIZE( written );
    ret = GetWriteWatch( WRITE_WATCH_FLAG_RESET, base, 0x30000,
                         written, &count, &granularity );
    ok( !ret, "GetWriteWatch reset failed %u\n", ret );
    status = publish_pair( first, 0x55555555, second, 0x66666666 );
    ok( !status, "valid publication failed %#lx\n", status );
    ok( *first == 0x55555555 && *second == 0x66666666,
        "outputs are %#lx/%#lx\n", *first, *second );
    count = ARRAY_SIZE( written );
    ret = GetWriteWatch( 0, base, 0x30000, written, &count, &granularity );
    ok( !ret, "GetWriteWatch failed %u\n", ret );
    ok( count == 2, "expected two written pages, got %Iu\n", count );
    if (count == 2)
    {
        ok( written[0] == first, "first dirty page %p, expected %p\n", written[0], first );
        ok( written[1] == second, "second dirty page %p, expected %p\n", written[1], second );
    }

    region_size = 0;
    NtFreeVirtualMemory( NtCurrentProcess(), &base, &region_size, MEM_RELEASE );
}

static ULONG query_memory_protect( const void *address )
{
    MEMORY_BASIC_INFORMATION info;
    NTSTATUS status;

    status = NtQueryVirtualMemory( NtCurrentProcess(), address, MemoryBasicInformation,
                                   &info, sizeof(info), NULL );
    ok( !status, "NtQueryVirtualMemory(%p) failed %#lx\n", address, status );
    return status ? 0 : info.Protect;
}

static void protect_stack_lane( void *address, ULONG protect )
{
    SIZE_T size = 0x1000;
    ULONG old_protect;
    void *base = address;
    NTSTATUS status;

    status = NtProtectVirtualMemory( NtCurrentProcess(), &base, &size, protect, &old_protect );
    ok( !status, "NtProtectVirtualMemory(%p, %#lx) failed %#lx\n",
        address, protect, status );
}

static void test_wow64_stack_guard_granularity(void)
{
    NTSTATUS (CDECL *copy_user)(void *,const void *,SIZE_T,ULONG);
    ULONG saved_protect[6], saved_guarantee, protect;
    BYTE *stack_start, *stack_base, *saved_limit;
    BYTE *block, *lanes[4], *bottom[2];
    BYTE value = 0xa5, probe;
    HMODULE ntdll;
    unsigned int i;
    NTSTATUS status;

    if (current_machine != IMAGE_FILE_MACHINE_I386 ||
        native_machine != IMAGE_FILE_MACHINE_ARM64 ||
        !read_shadow_shared_data( GetCurrentProcess(), &probe ))
        return;

    ntdll = GetModuleHandleA( "ntdll.dll" );
    copy_user = (void *)pRtlFindExportedRoutineByName( ntdll, "__wine_wow64_user_copy" );
    if (!copy_user)
    {
        win_skip( "__wine_wow64_user_copy is not exported\n" );
        return;
    }

    stack_start = NtCurrentTeb()->DeallocationStack;
    stack_base = NtCurrentTeb()->Tib.StackBase;
    saved_limit = NtCurrentTeb()->Tib.StackLimit;
    saved_guarantee = NtCurrentTeb()->GuaranteedStackBytes;
    block = (BYTE *)(((ULONG_PTR)saved_limit + 0x13fff) & ~0x3fffu);
    if (!stack_start || block < stack_start + 0x10000 || block + 0x4000 >= stack_base)
    {
        win_skip( "current WoW64 stack has no isolated 16K guard test range\n" );
        return;
    }

    for (i = 0; i < ARRAY_SIZE(lanes); i++)
    {
        lanes[i] = block + i * 0x1000;
        saved_protect[i] = query_memory_protect( lanes[i] );
        protect_stack_lane( lanes[i], PAGE_READWRITE );
    }
    protect_stack_lane( lanes[2], PAGE_READWRITE | PAGE_GUARD );
    protect_stack_lane( lanes[3], PAGE_READONLY );
    NtCurrentTeb()->GuaranteedStackBytes = 0;
    NtCurrentTeb()->Tib.StackLimit = lanes[3];

    status = copy_user( lanes[2], &value, sizeof(value), WOW64_USER_COPY_FAULTING_WRITE );
    ok( !status, "first logical stack growth failed %#lx\n", status );
    ok( NtCurrentTeb()->Tib.StackLimit == lanes[2],
        "first StackLimit is %p, expected %p\n", NtCurrentTeb()->Tib.StackLimit, lanes[2] );
    if (!status) ok( *lanes[2] == value, "first guard write stored %#x\n", *lanes[2] );
    protect = query_memory_protect( lanes[0] );
    ok( protect == PAGE_READWRITE, "lower sibling protect is %#lx\n", protect );
    protect = query_memory_protect( lanes[1] );
    ok( protect == (PAGE_READWRITE | PAGE_GUARD), "new guard protect is %#lx\n", protect );
    protect = query_memory_protect( lanes[2] );
    ok( protect == PAGE_READWRITE, "old guard protect is %#lx\n", protect );
    protect = query_memory_protect( lanes[3] );
    ok( protect == PAGE_READONLY, "upper sibling protect is %#lx\n", protect );

    status = copy_user( lanes[1], &value, sizeof(value), WOW64_USER_COPY_FAULTING_WRITE );
    ok( !status, "second logical stack growth failed %#lx\n", status );
    ok( NtCurrentTeb()->Tib.StackLimit == lanes[1],
        "second StackLimit is %p, expected %p\n", NtCurrentTeb()->Tib.StackLimit, lanes[1] );
    protect = query_memory_protect( lanes[0] );
    ok( protect == (PAGE_READWRITE | PAGE_GUARD), "second guard protect is %#lx\n", protect );
    protect = query_memory_protect( lanes[1] );
    ok( protect == PAGE_READWRITE, "second old guard protect is %#lx\n", protect );
    protect = query_memory_protect( lanes[2] );
    ok( protect == PAGE_READWRITE, "second upper sibling protect is %#lx\n", protect );
    protect = query_memory_protect( lanes[3] );
    ok( protect == PAGE_READONLY, "read-only sibling changed to %#lx\n", protect );

    bottom[0] = stack_start;
    bottom[1] = stack_start + 0x1000;
    saved_protect[4] = query_memory_protect( bottom[0] );
    saved_protect[5] = query_memory_protect( bottom[1] );
    protect_stack_lane( bottom[0], PAGE_NOACCESS );
    protect_stack_lane( bottom[1], PAGE_READWRITE );
    *bottom[1] = 0x5a;
    protect_stack_lane( bottom[1], PAGE_READWRITE | PAGE_GUARD );
    NtCurrentTeb()->Tib.StackLimit = bottom[1] + 0x1000;
    status = copy_user( bottom[1], &value, sizeof(value), WOW64_USER_COPY_FAULTING_WRITE );
    ok( status == STATUS_STACK_OVERFLOW, "minimum guarantee returned %#lx\n", status );
    ok( NtCurrentTeb()->Tib.StackLimit == bottom[1],
        "overflow StackLimit is %p, expected %p\n", NtCurrentTeb()->Tib.StackLimit, bottom[1] );
    protect = query_memory_protect( bottom[0] );
    ok( protect == PAGE_NOACCESS, "overflow no-access lane changed to %#lx\n", protect );
    protect = query_memory_protect( bottom[1] );
    ok( protect == PAGE_READWRITE, "overflow guarantee protect is %#lx\n", protect );
    if (protect == PAGE_READWRITE)
        ok( *bottom[1] == 0x5a, "overflow path published %#x\n", *bottom[1] );

    NtCurrentTeb()->Tib.StackLimit = saved_limit;
    NtCurrentTeb()->GuaranteedStackBytes = saved_guarantee;
    for (i = 0; i < ARRAY_SIZE(lanes); i++) protect_stack_lane( lanes[i], saved_protect[i] );
    protect_stack_lane( bottom[0], saved_protect[4] );
    protect_stack_lane( bottom[1], saved_protect[5] );
}

static void test_wow64_server_call_protection(void)
{
    static const WCHAR value_nameW[] = L"server-call-value";
    KEY_VALUE_PARTIAL_INFORMATION *info;
    UNICODE_STRING value_name;
    char key_name[128];
    DWORD disposition, size, type, value;
    DWORD initial = 0x11223344, replacement = 0x55667788;
    HANDLE key;
    BYTE probe;
    void *base = NULL, *page;
    SIZE_T region_size;
    ULONG old_protect, result_len;
    NTSTATUS status;
    LSTATUS error;

    if (current_machine != IMAGE_FILE_MACHINE_I386 ||
        native_machine != IMAGE_FILE_MACHINE_ARM64 ||
        !read_shadow_shared_data( GetCurrentProcess(), &probe ))
        return;

    sprintf( key_name, "Software\\Wine\\Tests\\wow64-server-call-%08lx", GetCurrentProcessId() );
    error = RegCreateKeyExA( HKEY_CURRENT_USER, key_name, 0, NULL, REG_OPTION_VOLATILE,
                             KEY_ALL_ACCESS, NULL, (HKEY *)&key, &disposition );
    ok( !error, "RegCreateKeyExA failed %ld\n", error );
    if (error) return;
    error = RegSetValueExW( key, value_nameW, 0, REG_DWORD,
                            (const BYTE *)&initial, sizeof(initial) );
    ok( !error, "initial RegSetValueExW failed %ld\n", error );
    if (error) goto done;

    region_size = 0x10000;
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &base, 0, &region_size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "NtAllocateVirtualMemory failed %#lx\n", status );
    if (status) goto done;
    page = (BYTE *)base + 0x1000;
    *(DWORD *)page = replacement;
    RtlInitUnicodeString( &value_name, value_nameW );

    region_size = 0x1000;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &region_size,
                                     PAGE_NOACCESS, &old_protect );
    ok( !status, "NtProtectVirtualMemory failed %#lx\n", status );
    if (status) goto free;
    status = NtSetValueKey( key, &value_name, 0, REG_DWORD, page, sizeof(DWORD) );
    ok( status == STATUS_ACCESS_VIOLATION, "protected request returned %#lx\n", status );
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &region_size,
                                     PAGE_READWRITE, &old_protect );
    ok( !status, "request page restore failed %#lx\n", status );

    value = 0;
    size = sizeof(value);
    error = RegQueryValueExW( key, value_nameW, NULL, &type, (BYTE *)&value, &size );
    ok( !error && type == REG_DWORD && size == sizeof(value) && value == initial,
        "protected request changed value, error %ld type %lu size %lu value %#lx\n",
        error, type, size, value );

    status = NtSetValueKey( key, &value_name, 0, REG_DWORD, NULL, sizeof(DWORD) );
    ok( status == STATUS_ACCESS_VIOLATION, "NULL request returned %#lx\n", status );
    status = NtSetValueKey( key, &value_name, 0, REG_DWORD,
                            (void *)(ULONG_PTR)0xfffffffe, sizeof(DWORD) );
    ok( status == STATUS_ACCESS_VIOLATION, "wrapping request returned %#lx\n", status );
    status = NtSetValueKey( key, &value_name, 0, REG_DWORD, page, sizeof(DWORD) );
    ok( !status, "valid request after rejected ranges failed %#lx\n", status );

    info = (KEY_VALUE_PARTIAL_INFORMATION *)
           ((BYTE *)page - FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data));
    *(DWORD *)page = 0xa5a5a5a5;
    region_size = 0x1000;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &region_size,
                                     PAGE_NOACCESS, &old_protect );
    ok( !status, "reply page protect failed %#lx\n", status );
    if (status) goto free;
    result_len = 0xdeadbeef;
    status = NtQueryValueKey( key, &value_name, KeyValuePartialInformation, info,
                              FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(DWORD),
                              &result_len );
    ok( status == STATUS_ACCESS_VIOLATION, "protected reply returned %#lx\n", status );
    status = NtProtectVirtualMemory( NtCurrentProcess(), &page, &region_size,
                                     PAGE_READWRITE, &old_protect );
    ok( !status, "reply page restore failed %#lx\n", status );
    ok( *(DWORD *)page == 0xa5a5a5a5, "protected reply changed data to %#lx\n", *(DWORD *)page );

    result_len = 0;
    status = NtQueryValueKey( key, &value_name, KeyValuePartialInformation, info,
                              FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(DWORD),
                              &result_len );
    ok( !status, "valid reply after protection failure failed %#lx\n", status );
    if (!status)
        ok( *(DWORD *)info->Data == replacement, "valid reply returned %#lx\n", *(DWORD *)info->Data );

free:
    if (base)
    {
        region_size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &base, &region_size, MEM_RELEASE );
    }
done:
    RegCloseKey( key );
    RegDeleteKeyA( HKEY_CURRENT_USER, key_name );
}

static void test_continue_boolean_argument(void)
{
    static LONG pass;
    CONTEXT context;
    BYTE value;
    LONG current;
    NTSTATUS status;

    if (current_machine != IMAGE_FILE_MACHINE_I386 ||
        native_machine != IMAGE_FILE_MACHINE_ARM64 ||
        !read_shadow_shared_data( GetCurrentProcess(), &value ))
        return;

    RtlCaptureContext( &context );
    current = InterlockedIncrement( &pass );
    if (current == 1)
    {
        status = NtContinue( &context, TRUE );
        ok( 0, "NtContinue returned %#lx\n", status );
    }
    ok( current == 2, "NtContinue resumed with pass %ld\n", current );
}

#else

static void test_remote_memory_address_codec(void)
{
}

static void test_remote_memory_codec_cleanup(void)
{
}

static void test_wow64_server_call_protection(void)
{
}

static void test_wow64_raw_server_call_protection(void)
{
}

static void test_wow64_handle_pair_publication(void)
{
}

static void test_wow64_stack_guard_granularity(void)
{
}

static void test_continue_boolean_argument(void)
{
}

#endif

static void test_complete_new_process_request_validation(void)
{
    static const int invalid_commits[] = { -1, INT_MIN, 2 };
    unsigned int (CDECL *server_call)(void *);
    struct __server_request_info req;
    NTSTATUS status;
    unsigned int i;
    HMODULE ntdll;

    ntdll = GetModuleHandleA( "ntdll.dll" );
    server_call = (void *)pRtlFindExportedRoutineByName( ntdll, "wine_server_call" );
    if (!server_call)
    {
        win_skip( "wine_server_call is not exported\n" );
        return;
    }

    for (i = 0; i < ARRAY_SIZE(invalid_commits); ++i)
    {
        memset( &req, 0, sizeof(req) );
        req.u.req.request_header.req = REQ_complete_new_process;
        req.u.req.complete_new_process_request.commit = invalid_commits[i];
        status = server_call( &req );
        ok( status == STATUS_INVALID_PARAMETER,
            "complete_new_process commit %d returned %#lx\n", invalid_commits[i], status );
    }
    for (i = 0; i <= 1; ++i)
    {
        memset( &req, 0, sizeof(req) );
        req.u.req.request_header.req = REQ_complete_new_process;
        req.u.req.complete_new_process_request.commit = i;
        status = server_call( &req );
        ok( status == STATUS_INVALID_HANDLE,
            "complete_new_process commit %u returned %#lx\n", i, status );
    }
}

static void test_process_architecture( SYSTEM_INFORMATION_CLASS class, HANDLE process, USHORT expect_machine, USHORT expect_native )
{
    SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION machines[8];
    NTSTATUS status;
    ULONG i, len;

    if (class == SystemSupportedProcessorArchitectures &&
        native_machine == IMAGE_FILE_MACHINE_ARM64 && expect_machine == IMAGE_FILE_MACHINE_AMD64)
        expect_machine = IMAGE_FILE_MACHINE_ARM64;

    len = 0xdead;
    status = pNtQuerySystemInformationEx( class, &process, sizeof(process),
                                          machines, sizeof(machines), &len );
    ok( !status, "failed %lx\n", status );
    ok( !(len & 3), "wrong len %lx\n", len );
    len /= sizeof(machines[0]);
    for (i = 0; i < len - 1; i++)
    {
        if (machines[i].Process)
            ok( machines[i].Machine == expect_machine, "wrong process machine %x\n", machines[i].Machine);
        else
            ok( machines[i].Machine != expect_machine, "wrong machine %x\n", machines[i].Machine);

        if (machines[i].Native)
            ok( machines[i].Machine == expect_native, "wrong native machine %x\n", machines[i].Machine);
        else
            ok( machines[i].Machine != expect_native, "wrong machine %x\n", machines[i].Machine);

        if (machines[i].WoW64Container)
            ok( is_machine_32bit( machines[i].Machine ) && !is_machine_32bit( native_machine ),
                "wrong wow64 %x\n", machines[i].Machine);

        if (class == SystemSupportedProcessorArchitectures && native_machine == IMAGE_FILE_MACHINE_ARM64)
            ok( machines[i].Machine != IMAGE_FILE_MACHINE_AMD64,
                "SystemSupportedProcessorArchitectures returned AMD64\n");
    }
    ok( !*(DWORD *)&machines[i], "missing terminating null\n" );

    len = i * sizeof(machines[0]);
    status = pNtQuerySystemInformationEx( class, &process, sizeof(process),
                                          machines, len, &len );
    ok( status == STATUS_BUFFER_TOO_SMALL, "failed %lx\n", status );
    ok( len == (i + 1) * sizeof(machines[0]), "wrong len %lu\n", len );

    if (pRtlWow64GetProcessMachines)
    {
        USHORT current = 0xdead, native = 0xbeef;
        status = pRtlWow64GetProcessMachines( process, &current, &native );
        ok( !status, "failed %lx\n", status );
        if (expect_machine != IMAGE_FILE_MACHINE_I386 &&
            expect_machine != IMAGE_FILE_MACHINE_ARMNT)
            ok( current == 0, "wrong current machine %x / %x\n", current, expect_machine );
        else
            ok( current == expect_machine, "wrong current machine %x / %x\n", current, expect_machine );
        ok( native == expect_native, "wrong native machine %x / %x\n", native, expect_native );
    }
}

static void test_process_machine( HANDLE process, HANDLE thread,
                                  USHORT expect_machine, USHORT expect_image )
{
    PROCESS_BASIC_INFORMATION basic;
    SECTION_IMAGE_INFORMATION image;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    PEB peb;
    ULONG len;
    SIZE_T size;
    NTSTATUS status;
    void *entry_point = NULL;
    void *win32_entry = NULL;

    status = NtQueryInformationProcess( process, ProcessBasicInformation, &basic, sizeof(basic), &len );
    ok( !status, "ProcessBasicInformation failed %lx\n", status );
    if (ReadProcessMemory( process, basic.PebBaseAddress, &peb, sizeof(peb), &size ) &&
        ReadProcessMemory( process, peb.ImageBaseAddress, &dos, sizeof(dos), &size ) &&
        ReadProcessMemory( process, (char *)peb.ImageBaseAddress + dos.e_lfanew, &nt, sizeof(nt), &size ))
    {
        ok( nt.FileHeader.Machine == expect_machine, "wrong nt machine %x / %x\n",
            nt.FileHeader.Machine, expect_machine );
        entry_point = (char *)peb.ImageBaseAddress + nt.OptionalHeader.AddressOfEntryPoint;
    }

    status = NtQueryInformationProcess( process, ProcessImageInformation, &image, sizeof(image), &len );
    ok( !status, "ProcessImageInformation failed %lx\n", status );
    ok( image.Machine == expect_image, "wrong image info %x / %x\n", image.Machine, expect_image );

    status = NtQueryInformationThread( thread, ThreadQuerySetWin32StartAddress,
                                       &win32_entry, sizeof(win32_entry), &len );
    ok( !status, "ThreadQuerySetWin32StartAddress failed %lx\n", status );

    if (!entry_point) return;

    if (image.Machine == expect_machine)
    {
        ok( image.TransferAddress == entry_point, "wrong entry %p / %p\n",
            image.TransferAddress, entry_point );
        ok( win32_entry == entry_point, "wrong win32 entry %p / %p\n",
            win32_entry, entry_point );
    }
    else
    {
        /* image.TransferAddress is the ARM64 entry, entry_point is the x86-64 one,
           win32_entry is the redirected x86-64 -> ARM64EC one */
        ok( image.TransferAddress != entry_point, "wrong entry %p\n", image.TransferAddress );
        ok( image.TransferAddress != win32_entry, "wrong entry %p\n", image.TransferAddress );
        ok( win32_entry != entry_point, "wrong win32 entry %p\n", win32_entry );
    }
}

static void test_query_architectures(SYSTEM_INFORMATION_CLASS class)
{
    static char cmd_sysnative[] = "C:\\windows\\sysnative\\cmd.exe /c exit";
    static char cmd_system32[] = "C:\\windows\\system32\\cmd.exe /c exit";
    static char cmd_syswow64[] = "C:\\windows\\syswow64\\cmd.exe /c exit";
    SYSTEM_SUPPORTED_PROCESSOR_ARCHITECTURES_INFORMATION machines[8];
    PROCESS_INFORMATION pi;
    STARTUPINFOA si = { sizeof(si) };
    NTSTATUS status;
    HANDLE process;
    ULONG i, len;
    USHORT machine;
#ifdef __arm64ec__
    BOOL is_arm64ec = TRUE;
#else
    BOOL is_arm64ec = FALSE;
#endif

    if (!pNtQuerySystemInformationEx) return;

    process = GetCurrentProcess();
    status = pNtQuerySystemInformationEx( class, &process, sizeof(process),
                                          machines, sizeof(machines), &len );
    if (status == STATUS_INVALID_INFO_CLASS)
    {
        win_skip( "SystemSupportedProcessorArchitectures%s not supported\n",
                  class == SystemSupportedProcessorArchitectures2 ? "2" : "" );
        return;
    }
    ok( !status, "failed %lx\n", status );

    process = (HANDLE)0xdeadbeef;
    status = pNtQuerySystemInformationEx( class, &process, sizeof(process),
                                          machines, sizeof(machines), &len );
    ok( status == STATUS_INVALID_HANDLE, "failed %lx\n", status );
    process = (HANDLE)0xdeadbeef;
    status = pNtQuerySystemInformationEx( class, &process, 3,
                                          machines, sizeof(machines), &len );
    ok( status == STATUS_INVALID_PARAMETER || broken(status == STATUS_INVALID_HANDLE),
        "failed %lx\n", status );
    process = GetCurrentProcess();
    status = pNtQuerySystemInformationEx( class, &process, 3,
                                          machines, sizeof(machines), &len );
    ok( status == STATUS_INVALID_PARAMETER || broken( status == STATUS_SUCCESS),
        "failed %lx\n", status );
    status = pNtQuerySystemInformationEx( class, NULL, 0,
                                          machines, sizeof(machines), &len );
    ok( status == STATUS_INVALID_PARAMETER, "failed %lx\n", status );

    winetest_push_context( "current" );
    test_process_architecture( class, GetCurrentProcess(), current_machine,
                               native_machine );
    test_process_machine( GetCurrentProcess(), GetCurrentThread(), current_machine,
                          is_arm64ec ? native_machine : current_machine );
    winetest_pop_context();

    winetest_push_context( "zero" );
    test_process_architecture( class, 0, 0, native_machine );
    winetest_pop_context();

    machine = (is_win64 && native_machine == IMAGE_FILE_MACHINE_ARM64) ? current_machine : IMAGE_FILE_MACHINE_AMD64;
    if (create_process_machine( is_win64 ? cmd_system32 : cmd_sysnative, CREATE_SUSPENDED, machine, &pi ))
    {
        winetest_push_context( "system32" );
        test_process_architecture( class, pi.hProcess, machine, native_machine );
        test_process_machine( pi.hProcess, pi.hThread,
                              is_win64 ? current_machine : native_machine, native_machine );
        TerminateProcess( pi.hProcess, 0 );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
        winetest_pop_context();
    }
    if (CreateProcessA( NULL, is_win64 ? cmd_syswow64 : cmd_system32, NULL, NULL,
                        FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi ))
    {
        winetest_push_context( "syswow64" );
        test_process_architecture( class, pi.hProcess, IMAGE_FILE_MACHINE_I386, native_machine );
        test_process_machine( pi.hProcess, pi.hThread, IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_I386 );
        TerminateProcess( pi.hProcess, 0 );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
        winetest_pop_context();
    }
    if (is_win64 && native_machine == IMAGE_FILE_MACHINE_ARM64)
    {
        USHORT machine = IMAGE_FILE_MACHINE_ARM64 + IMAGE_FILE_MACHINE_AMD64 - current_machine;

        if (create_process_machine( cmd_system32, CREATE_SUSPENDED, machine, &pi ))
        {
            winetest_push_context( "%04x", machine );
            test_process_architecture( class, pi.hProcess, machine, native_machine );
            test_process_machine( pi.hProcess, pi.hThread, machine, native_machine );
            TerminateProcess( pi.hProcess, 0 );
            CloseHandle( pi.hProcess );
            CloseHandle( pi.hThread );
            winetest_pop_context();
        }
    }

    if (pRtlWow64GetCurrentMachine)
    {
        USHORT machine = pRtlWow64GetCurrentMachine();
        ok( machine == current_machine, "wrong machine %x / %x\n", machine, current_machine );
    }
    if (pRtlWow64IsWowGuestMachineSupported)
    {
        static const WORD machines[] = { IMAGE_FILE_MACHINE_I386, IMAGE_FILE_MACHINE_ARMNT,
                                         IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_ARM64, 0xdead };

        for (i = 0; i < ARRAY_SIZE(machines); i++)
        {
            BOOLEAN ret = 0xcc;
            status = pRtlWow64IsWowGuestMachineSupported( machines[i], &ret );
            ok( !status, "failed %lx\n", status );
            if (is_machine_32bit( machines[i] ) && !is_machine_32bit( native_machine ))
                ok( ret || machines[i] == IMAGE_FILE_MACHINE_ARMNT ||
                    broken(current_machine == IMAGE_FILE_MACHINE_I386), /* win10-1607 wow64 */
                    "%04x: got %u\n", machines[i], ret );
            else
                ok( !ret, "%04x: got %u\n", machines[i], ret );
        }
    }
}

static void push_onto_free_list( CROSS_PROCESS_WORK_HDR *list, CROSS_PROCESS_WORK_ENTRY *entry )
{
#ifdef _WIN64
    pRtlWow64PushCrossProcessWorkOntoFreeList( list, entry );
#else
    entry->next = list->first;
    list->first = (char *)entry - (char *)list;
#endif
}

static void push_onto_work_list( CROSS_PROCESS_WORK_HDR *list, CROSS_PROCESS_WORK_ENTRY *entry )
{
#ifdef _WIN64
    void *ret;
    pRtlWow64PushCrossProcessWorkOntoWorkList( list, entry, &ret );
#else
    entry->next = list->first;
    list->first = (char *)entry - (char *)list;
#endif
}

static CROSS_PROCESS_WORK_ENTRY *pop_from_free_list( CROSS_PROCESS_WORK_HDR *list )
{
#ifdef _WIN64
    return pRtlWow64PopCrossProcessWorkFromFreeList( list );
#else
    CROSS_PROCESS_WORK_ENTRY *ret;

    if (!list->first) return NULL;
    ret = (CROSS_PROCESS_WORK_ENTRY *)((char *)list + list->first);
    list->first = ret->next;
    ret->next = 0;
    return ret;
#endif
}

static CROSS_PROCESS_WORK_ENTRY *pop_from_work_list( CROSS_PROCESS_WORK_HDR *list )
{
#ifdef _WIN64
    BOOLEAN flush;

    return pRtlWow64PopAllCrossProcessWorkFromWorkList( list, &flush );
#else
    UINT pos = list->first, prev_pos = 0;

    list->first = 0;
    if (!pos) return NULL;

    for (;;)  /* reverse the list */
    {
        CROSS_PROCESS_WORK_ENTRY *entry = CROSS_PROCESS_LIST_ENTRY( list, pos );
        UINT next = entry->next;
        entry->next = prev_pos;
        if (!next) return entry;
        prev_pos = pos;
        pos = next;
    }
#endif
}

static void request_cross_process_flush( CROSS_PROCESS_WORK_HDR *list )
{
#ifdef _WIN64
    pRtlWow64RequestCrossProcessHeavyFlush( list );
#else
    list->first |= CROSS_PROCESS_LIST_FLUSH;
#endif
}

#define expect_cross_work_entry(list,entry,id,addr,size,arg0,arg1,arg2,arg3) \
    expect_cross_work_entry_(list,entry,id,addr,size,arg0,arg1,arg2,arg3,__LINE__)
static BOOL cross_work_addresses_use_shadow;

static CROSS_PROCESS_WORK_ENTRY *expect_cross_work_entry_( CROSS_PROCESS_WORK_LIST *list,
                                                           CROSS_PROCESS_WORK_ENTRY *entry,
                                                           UINT id, void *addr, SIZE_T size,
                                                           UINT arg0, UINT arg1, UINT arg2, UINT arg3,
                                                           int line )
{
    CROSS_PROCESS_WORK_ENTRY *next;
    ULONG_PTR expected_addr = (ULONG_PTR)addr;

    if (cross_work_addresses_use_shadow && expected_addr)
        expected_addr += WINE_LOW_VA_SHADOW_BASE;

    ok_(__FILE__,line)( entry != NULL, "no more entries in list\n" );
    if (!entry) return NULL;
    ok_(__FILE__,line)( entry->id == id, "wrong type %u / %u\n", entry->id, id );
    ok_(__FILE__,line)( entry->addr == expected_addr, "wrong address %s / %s\n",
                        wine_dbgstr_longlong(entry->addr), wine_dbgstr_longlong(expected_addr) );
    ok_(__FILE__,line)( entry->size == size, "wrong size %s / %Ix\n",
                        wine_dbgstr_longlong(entry->size), size );
    ok_(__FILE__,line)( entry->args[0] == arg0, "wrong args[0] %x / %x\n", entry->args[0], arg0 );
    ok_(__FILE__,line)( entry->args[1] == arg1, "wrong args[1] %x / %x\n", entry->args[1], arg1 );
    ok_(__FILE__,line)( entry->args[2] == arg2, "wrong args[2] %x / %x\n", entry->args[2], arg2 );
    ok_(__FILE__,line)( entry->args[3] == arg3, "wrong args[3] %x / %x\n", entry->args[3], arg3 );
    next = entry->next ? CROSS_PROCESS_LIST_ENTRY( &list->work_list, entry->next ) : NULL;
    memset( entry, 0xcc, sizeof(*entry) );
    push_onto_free_list( &list->free_list, entry );
    return next;
}

static void test_cross_process_notifications( HANDLE process, ULONG_PTR section, ULONG_PTR ptr )
{
    CROSS_PROCESS_WORK_ENTRY *entry;
    CROSS_PROCESS_WORK_LIST *list;
    UINT pos;
    void *addr = NULL, *addr2;
    SIZE_T size = 0;
    DWORD old_prot;
    LARGE_INTEGER offset;
    HANDLE file, mapping;
    NTSTATUS status;
    BOOL ret;
    BYTE data[] = { 0xcc, 0xcc, 0xcc };

    cross_work_addresses_use_shadow = FALSE;
#ifndef _WIN64
    if (current_machine == IMAGE_FILE_MACHINE_I386 &&
        native_machine == IMAGE_FILE_MACHINE_ARM64 && pNtWow64ReadVirtualMemory64)
    {
        BYTE value;

        cross_work_addresses_use_shadow = read_shadow_shared_data( GetCurrentProcess(), &value );
    }
#endif

    ret = DuplicateHandle( process, (HANDLE)section, GetCurrentProcess(), &mapping,
                           0, FALSE, DUPLICATE_SAME_ACCESS );
    ok( ret, "DuplicateHandle failed %lu\n", GetLastError() );
    status = NtMapViewOfSection( mapping, GetCurrentProcess(), &addr, 0, 0, NULL,
                                 &size, ViewShare, 0, PAGE_READWRITE );
    ok( !status, "NtMapViewOfSection failed %lx\n", status );
    ok( size == 0x4000, "unexpected size %Ix\n", size );
    list = addr;
    addr2 = malloc( size );
    ret = ReadProcessMemory( process, (void *)ptr, addr2, size, &size );
    ok( ret, "ReadProcessMemory failed %lu\n", GetLastError() );
    ok( !memcmp( addr2, addr, size ), "wrong data\n" );
    free( addr2 );
    CloseHandle( mapping );

    if (pRtlOpenCrossProcessEmulatorWorkConnection)
    {
        pRtlOpenCrossProcessEmulatorWorkConnection( process, &mapping, &addr2 );
        ok( mapping != 0, "got 0 handle\n" );
        ok( addr2 != NULL, "got NULL data\n" );
        ok( !memcmp( addr2, addr, size ), "wrong data\n" );
        UnmapViewOfFile( addr2 );
        addr2 = NULL;
        size = 0;
        status = NtMapViewOfSection( mapping, GetCurrentProcess(), &addr2, 0, 0, NULL,
                                     &size, ViewShare, 0, PAGE_READWRITE );
        ok( !status, "NtMapViewOfSection failed %lx\n", status );
        ok( !memcmp( addr2, addr, size ), "wrong data\n" );
        ok( CloseHandle( mapping ), "invalid handle\n" );
        UnmapViewOfFile( addr2 );

        mapping = (HANDLE)0xdead;
        addr2 = (void *)0xdeadbeef;
        pRtlOpenCrossProcessEmulatorWorkConnection( GetCurrentProcess(), &mapping, &addr2 );
        ok( !mapping, "got handle %p\n", mapping );
        ok( !addr2, "got data %p\n", addr2 );
    }
    else skip( "RtlOpenCrossProcessEmulatorWorkConnection not supported\n" );

    NtSuspendProcess( process );

    /* set argument values in free list to detect changes */
    for (pos = list->free_list.first; pos; pos = entry->next )
    {
        entry = CROSS_PROCESS_LIST_ENTRY( &list->free_list, pos );
        memset( entry->args, 0xcc, sizeof(entry->args) );
    }

    addr = VirtualAllocEx( process, NULL, 0x1234, MEM_COMMIT, PAGE_READWRITE );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualAlloc, NULL, 0x1234,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, 0, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualAlloc, addr, 0x2000,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, 0, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    VirtualProtectEx( process, (char *)addr + 0x333, 17, PAGE_READONLY, &old_prot );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualProtect,
                                         (char *)addr + 0x333, 17,
                                         PAGE_READONLY, 0, 0xcccccccc, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualProtect, addr, 0x1000,
                                         PAGE_READONLY, 0, 0xcccccccc, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    VirtualFreeEx( process, addr, 0, MEM_RELEASE );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualFree, addr, 0,
                                         MEM_RELEASE, 0, 0xcccccccc, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualFree, addr, 0x2000,
                                         MEM_RELEASE, 0, 0xcccccccc, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    addr = (void *)0x123;
    size = 0x321;
    status = NtAllocateVirtualMemory( process, &addr, 0, &size, MEM_COMMIT, PAGE_EXECUTE_READ );
    ok( status == STATUS_CONFLICTING_ADDRESSES || status == STATUS_INVALID_PARAMETER,
        "NtAllocateVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualAlloc, addr, 0x321,
                                         MEM_COMMIT, PAGE_EXECUTE_READ, 0, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualAlloc, addr, 0x321,
                                         MEM_COMMIT, PAGE_EXECUTE_READ, status, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    addr = NULL;
    size = 0x321;
    status = NtAllocateVirtualMemory( process, &addr, 0, &size, 0, PAGE_EXECUTE_READ );
    ok( status == STATUS_INVALID_PARAMETER, "NtAllocateVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualAlloc, addr, 0x321,
                                         0, PAGE_EXECUTE_READ, 0, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualAlloc, addr, 0x321,
                                         0, PAGE_EXECUTE_READ, status, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    addr = NULL;
    size = 0x4321;
    status = NtAllocateVirtualMemory( process, &addr, 0, &size, MEM_RESERVE, PAGE_EXECUTE_READWRITE );
    ok( !status, "NtAllocateVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualAlloc, NULL, 0x4321,
                                         MEM_RESERVE, PAGE_EXECUTE_READWRITE, 0, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualAlloc, addr, 0x5000,
                                         MEM_RESERVE, PAGE_EXECUTE_READWRITE, 0, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    size = 0x4321;
    status = NtAllocateVirtualMemory( process, &addr, 0, &size, MEM_COMMIT, PAGE_READWRITE );
    ok( !status, "NtAllocateVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualAlloc, addr, 0x4321,
                                         MEM_COMMIT, PAGE_READWRITE, 0, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualAlloc, addr, 0x5000,
                                         MEM_COMMIT, PAGE_READWRITE, 0, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    addr2 = (char *)addr + 0x111;
    size = 23;
    status = NtProtectVirtualMemory( process, &addr2, &size, PAGE_EXECUTE_READWRITE, &old_prot );
    ok( !status, "NtProtectVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualProtect, (char *)addr + 0x111, 23,
                                         PAGE_EXECUTE_READWRITE, 0, 0xcccccccc, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualProtect, addr, 0x1000,
                                         PAGE_EXECUTE_READWRITE, 0, 0xcccccccc, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    addr2 = (char *)addr + 0x222;
    size = 34;
    status = NtProtectVirtualMemory( process, &addr2, &size, PAGE_EXECUTE_WRITECOPY, &old_prot );
    ok( status == STATUS_INVALID_PARAMETER_4 || status == STATUS_INVALID_PAGE_PROTECTION,
        "NtProtectVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualProtect,
                                         (char *)addr + 0x222, 34,
                                         PAGE_EXECUTE_WRITECOPY, 0, 0xcccccccc, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualProtect,
                                         (char *)addr + 0x222, 34,
                                         PAGE_EXECUTE_WRITECOPY, status, 0xcccccccc, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    status = NtWriteVirtualMemory( process, (char *)addr + 0x1111, data, sizeof(data), &size );
    ok( !status, "NtWriteVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    ok( !entry, "not at end of list\n" );

    addr2 = (char *)addr + 0x1234;
    size = 45;
    status = NtFreeVirtualMemory( process, &addr2, &size, MEM_DECOMMIT );
    ok( !status, "NtFreeVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualFree, (char *)addr + 0x1234, 45,
                                         MEM_DECOMMIT, 0, 0xcccccccc, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualFree, addr2, 0x1000,
                                         MEM_DECOMMIT, 0, 0xcccccccc, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    size = 0;
    status = NtFreeVirtualMemory( process, &addr, &size, MEM_RELEASE );
    ok( !status, "NtFreeVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualFree, addr, 0,
                                         MEM_RELEASE, 0, 0xcccccccc, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualFree, addr, 0x5000,
                                         MEM_RELEASE, 0, 0xcccccccc, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    addr = (void *)0x123;
    size = 0;
    status = NtFreeVirtualMemory( process, &addr, &size, MEM_RELEASE );
    ok( status == STATUS_MEMORY_NOT_ALLOCATED || status == STATUS_INVALID_PARAMETER,
        "NtFreeVirtualMemory failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualFree, addr, 0,
                                         MEM_RELEASE, 0, 0xcccccccc, 0xcccccccc );
        entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualFree, addr, 0,
                                         MEM_RELEASE, status, 0xcccccccc, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    file = CreateFileA( "c:\\windows\\syswow64\\version.dll", GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0 );
    ok( file != INVALID_HANDLE_VALUE, "Failed to open version.dll\n" );
    mapping = CreateFileMappingA( file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );
    addr = NULL;
    size = 0;
    offset.QuadPart = 0;
    status = NtMapViewOfSection( mapping, process, &addr, 0, 0, &offset, &size, ViewShare, 0, PAGE_READONLY );
    ok( NT_SUCCESS(status), "NtMapViewOfSection failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    ok( !entry, "list not empty\n" );

    FlushInstructionCache( process, addr, 0x1234 );
    entry = pop_from_work_list( &list->work_list );
    entry = expect_cross_work_entry( list, entry, CrossProcessFlushCache, addr, 0x1234,
                                     0xcccccccc, 0xcccccccc, 0xcccccccc, 0xcccccccc );
    ok( !entry, "not at end of list\n" );

    NtFlushInstructionCache( process, addr, 0x1234 );
    entry = pop_from_work_list( &list->work_list );
    if (current_machine != IMAGE_FILE_MACHINE_ARM64)
    {
        entry = expect_cross_work_entry( list, entry, CrossProcessFlushCache, addr, 0x1234,
                                         0xcccccccc, 0xcccccccc, 0xcccccccc, 0xcccccccc );
    }
    ok( !entry, "not at end of list\n" );

    WriteProcessMemory( process, (char *)addr + 0x1ffe, data, sizeof(data), &size );
    entry = pop_from_work_list( &list->work_list );
    entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualProtect,
                                     (char *)addr + 0x1000, 0x2000, 0x60000000 | PAGE_EXECUTE_WRITECOPY,
                                     (current_machine != IMAGE_FILE_MACHINE_ARM64) ? 0 : 0xcccccccc,
                                     0xcccccccc, 0xcccccccc );
    entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualProtect,
                                     (char *)addr + 0x1000, 0x2000,
                                     0x60000000 | PAGE_EXECUTE_WRITECOPY, 0, 0xcccccccc, 0xcccccccc );
    entry = expect_cross_work_entry( list, entry, CrossProcessFlushCache,
                                     (char *)addr + 0x1ffe, sizeof(data),
                                     0xcccccccc, 0xcccccccc, 0xcccccccc, 0xcccccccc );
    entry = expect_cross_work_entry( list, entry, CrossProcessPreVirtualProtect,
                                     (char *)addr + 0x1000, 0x2000, 0x60000000 | PAGE_EXECUTE_READ,
                                     (current_machine != IMAGE_FILE_MACHINE_ARM64) ? 0 : 0xcccccccc,
                                     0xcccccccc, 0xcccccccc );
    entry = expect_cross_work_entry( list, entry, CrossProcessPostVirtualProtect,
                                     (char *)addr + 0x1000, 0x2000,
                                     0x60000000 | PAGE_EXECUTE_READ, 0, 0xcccccccc, 0xcccccccc );
    ok( !entry, "not at end of list\n" );

    status = NtUnmapViewOfSection( process, addr );
    ok( !status, "NtUnmapViewOfSection failed %lx\n", status );
    entry = pop_from_work_list( &list->work_list );
    ok( !entry, "list not empty\n" );

    CloseHandle( mapping );
    CloseHandle( file );
    UnmapViewOfFile( list );
}

static void test_wow64_shared_info( HANDLE process )
{
    ULONG i, peb_data[0x200], buffer[16];
    WOW64INFO *info = (WOW64INFO *)buffer;
    ULONG_PTR peb_ptr;
    NTSTATUS status;
    SIZE_T res;
    BOOLEAN wow64 = 0xcc;

    NtQueryInformationProcess( process, ProcessWow64Information, &peb_ptr, sizeof(peb_ptr), NULL );
    memset( buffer, 0xcc, sizeof(buffer) );
    status = pRtlWow64GetSharedInfoProcess( process, &wow64, info );
    ok( !status, "RtlWow64GetSharedInfoProcess failed %lx\n", status );
    ok( wow64 == TRUE, "wrong wow64 %u\n", wow64 );
    todo_wine_if (!info->NativeSystemPageSize) /* not set in old wow64 */
    {
        ok( info->NativeSystemPageSize == 0x1000, "wrong page size %lx\n",
            info->NativeSystemPageSize );
        ok( info->CpuFlags == (native_machine == IMAGE_FILE_MACHINE_AMD64 ? WOW64_CPUFLAGS_MSFT64 : WOW64_CPUFLAGS_SOFTWARE),
            "wrong flags %lx\n", info->CpuFlags );
        ok( info->NativeMachineType == native_machine, "wrong machine %x / %x\n",
            info->NativeMachineType, native_machine );
        ok( info->EmulatedMachineType == IMAGE_FILE_MACHINE_I386, "wrong machine %x\n",
            info->EmulatedMachineType );
    }
    ok( buffer[sizeof(*info) / sizeof(ULONG)] == 0xcccccccc, "buffer set %lx\n",
        buffer[sizeof(*info) / sizeof(ULONG)] );
    if (ReadProcessMemory( process, (void *)peb_ptr, peb_data, sizeof(peb_data), &res ))
    {
        ULONG limit = (sizeof(peb_data) - sizeof(info)) / sizeof(ULONG);
        for (i = 0; i < limit; i++)
        {
            if (!memcmp( peb_data + i, info, sizeof(*info) ))
            {
                trace( "wow64info found at %lx\n", i * 4 );
                break;
            }
        }
        ok( i < limit, "wow64info not found in PEB\n" );
    }
    if (info->SectionHandle && info->CrossProcessWorkList)
        test_cross_process_notifications( process, info->SectionHandle, info->CrossProcessWorkList );
    else
        trace( "no WOW64INFO section handle\n" );
}

static void test_amd64_shared_info( HANDLE process )
{
    ULONG i, peb_data[0x200], buffer[16];
    PROCESS_BASIC_INFORMATION proc_info;
    NTSTATUS status;
    SIZE_T res;
    BOOLEAN wow64 = 0xcc;
    struct arm64ec_shared_info *info = NULL;

    NtQueryInformationProcess( process, ProcessBasicInformation, &proc_info, sizeof(proc_info), NULL );

    memset( buffer, 0xcc, sizeof(buffer) );
    status = pRtlWow64GetSharedInfoProcess( process, &wow64, (WOW64INFO *)buffer );
    ok( !status, "RtlWow64GetSharedInfoProcess failed %lx\n", status );
    ok( !wow64, "wrong wow64 %u\n", wow64 );
    ok( buffer[0] == 0xcccccccc, "buffer initialized %lx\n", buffer[0] );

    if (ReadProcessMemory( process, (void *)proc_info.PebBaseAddress, peb_data, sizeof(peb_data), &res ))
    {
        ULONG limit = (sizeof(peb_data) - sizeof(*info)) / sizeof(ULONG);
        for (i = 0; i < limit; i++)
        {
            info = (struct arm64ec_shared_info *)(peb_data + i);
             if (info->NativeMachineType == IMAGE_FILE_MACHINE_ARM64 &&
                 info->EmulatedMachineType == IMAGE_FILE_MACHINE_AMD64)
            {
                trace( "shared info found at %lx\n", i * 4 );
                break;
            }
        }
        ok( i < limit, "shared info not found in PEB\n" );
    }
    if (info && info->SectionHandle && info->CrossProcessWorkList)
        test_cross_process_notifications( process, info->SectionHandle, info->CrossProcessWorkList );
    else
        trace( "no shared info section handle\n" );
}

static void test_peb_teb(void)
{
    PROCESS_BASIC_INFORMATION proc_info;
    THREAD_BASIC_INFORMATION info;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si = {0};
    NTSTATUS status;
    void *redir;
    SIZE_T res;
    BOOL ret;
    TEB teb;
    PEB peb;
    TEB32 teb32;
    PEB32 peb32;
    RTL_USER_PROCESS_PARAMETERS params;
    RTL_USER_PROCESS_PARAMETERS32 params32;
    ULONG_PTR peb_ptr;
    ULONG buffer[16];
    WOW64INFO *wow64info = (WOW64INFO *)buffer;
    BOOLEAN wow64;

    Wow64DisableWow64FsRedirection( &redir );

    if (CreateProcessA( "C:\\windows\\syswow64\\msinfo32.exe", NULL, NULL, NULL,
                        FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi ))
    {
        memset( &info, 0xcc, sizeof(info) );
        status = NtQueryInformationThread( pi.hThread, ThreadBasicInformation, &info, sizeof(info), NULL );
        ok( !status, "ThreadBasicInformation failed %lx\n", status );
        if (!ReadProcessMemory( pi.hProcess, info.TebBaseAddress, &teb, sizeof(teb), &res )) res = 0;
        ok( res == sizeof(teb), "wrong len %Ix\n", res );
        ok( teb.Tib.Self == info.TebBaseAddress, "wrong teb %p / %p\n", teb.Tib.Self, info.TebBaseAddress );
        if (is_wow64)
        {
            ok( !!teb.GdiBatchCount, "GdiBatchCount not set\n" );
            ok( (char *)info.TebBaseAddress + teb.WowTebOffset == ULongToPtr(teb.GdiBatchCount) ||
                broken(!NtCurrentTeb()->WowTebOffset),  /* pre-win10 */
                "wrong teb offset %ld\n", teb.WowTebOffset );
        }
        else
        {
            ok( !teb.GdiBatchCount, "GdiBatchCount set\n" );
            ok( teb.WowTebOffset == 0x2000 ||
                broken( !teb.WowTebOffset || teb.WowTebOffset == 1 ),  /* pre-win10 */
                "wrong teb offset %ld\n", teb.WowTebOffset );
            ok( (char *)teb.Tib.ExceptionList == (char *)info.TebBaseAddress + 0x2000,
                "wrong Tib.ExceptionList %p / %p\n",
                (char *)teb.Tib.ExceptionList, (char *)info.TebBaseAddress + 0x2000 );
            if (!ReadProcessMemory( pi.hProcess, teb.Tib.ExceptionList, &teb32, sizeof(teb32), &res )) res = 0;
            ok( res == sizeof(teb32), "wrong len %Ix\n", res );
            ok( (char *)ULongToPtr(teb32.Peb) == (char *)teb.Peb + 0x1000 ||
                broken( ULongToPtr(teb32.Peb) != teb.Peb ), /* vista */
                "wrong peb %p / %p\n", ULongToPtr(teb32.Peb), teb.Peb );
        }

        status = NtQueryInformationProcess( pi.hProcess, ProcessBasicInformation,
                                            &proc_info, sizeof(proc_info), NULL );
        ok( !status, "ProcessBasicInformation failed %lx\n", status );
        ok( proc_info.PebBaseAddress == teb.Peb, "wrong peb %p / %p\n", proc_info.PebBaseAddress, teb.Peb );

        status = NtQueryInformationProcess( pi.hProcess, ProcessWow64Information,
                                            &peb_ptr, sizeof(peb_ptr), NULL );
        ok( !status, "ProcessWow64Information failed %lx\n", status );
        ok( (void *)peb_ptr == (is_wow64 ? teb.Peb : ULongToPtr(teb32.Peb)),
            "wrong peb %p\n", (void *)peb_ptr );

        if (!ReadProcessMemory( pi.hProcess, proc_info.PebBaseAddress, &peb, sizeof(peb), &res )) res = 0;
        ok( res == sizeof(peb), "wrong len %Ix\n", res );
        ok( !peb.BeingDebugged, "BeingDebugged is %u\n", peb.BeingDebugged );
        if (!is_wow64)
        {
            if (!ReadProcessMemory( pi.hProcess, ULongToPtr(teb32.Peb), &peb32, sizeof(peb32), &res )) res = 0;
            ok( res == sizeof(peb32), "wrong len %Ix\n", res );
            ok( !peb32.BeingDebugged, "BeingDebugged is %u\n", peb32.BeingDebugged );
        }

        if (!ReadProcessMemory( pi.hProcess, peb.ProcessParameters, &params, sizeof(params), &res )) res = 0;
        ok( res == sizeof(params), "wrong len %Ix\n", res );
#define CHECK_STR(name) \
        ok( (char *)params.name.Buffer >= (char *)peb.ProcessParameters && \
            (char *)params.name.Buffer < (char *)peb.ProcessParameters + params.Size, \
            "wrong " #name " ptr %p / %p-%p\n", params.name.Buffer, peb.ProcessParameters, \
            (char *)peb.ProcessParameters + params.Size )
        CHECK_STR( ImagePathName );
        CHECK_STR( CommandLine );
        CHECK_STR( WindowTitle );
        CHECK_STR( Desktop );
        CHECK_STR( ShellInfo );
#undef CHECK_STR
        if (!is_wow64)
        {
            ok( peb32.ProcessParameters && ULongToPtr(peb32.ProcessParameters) != peb.ProcessParameters,
                "wrong ptr32 %p / %p\n", ULongToPtr(peb32.ProcessParameters), peb.ProcessParameters );
            if (!ReadProcessMemory( pi.hProcess, ULongToPtr(peb32.ProcessParameters), &params32, sizeof(params32), &res )) res = 0;
            ok( res == sizeof(params32), "wrong len %Ix\n", res );
#define CHECK_STR(name) \
            ok( ULongToPtr(params32.name.Buffer) >= ULongToPtr(peb32.ProcessParameters) && \
                ULongToPtr(params32.name.Buffer) < ULongToPtr(peb32.ProcessParameters + params32.Size), \
                "wrong " #name " ptr %lx / %lx-%lx\n", params32.name.Buffer, peb32.ProcessParameters, \
                peb32.ProcessParameters + params.Size ); \
            ok( params32.name.Length == params.name.Length, "wrong " #name "len %u / %u\n", \
                params32.name.Length, params.name.Length )
            CHECK_STR( ImagePathName );
            CHECK_STR( CommandLine );
            CHECK_STR( WindowTitle );
            CHECK_STR( Desktop );
            CHECK_STR( ShellInfo );
#undef CHECK_STR
            ok( params32.EnvironmentSize == params.EnvironmentSize, "wrong size %lu / %Iu\n",
                params32.EnvironmentSize, params.EnvironmentSize );
        }

        ResumeThread( pi.hThread );
        WaitForInputIdle( pi.hProcess, 1000 );

        if (pRtlWow64GetSharedInfoProcess) test_wow64_shared_info( pi.hProcess );
        else win_skip( "RtlWow64GetSharedInfoProcess not supported\n" );

        ret = DebugActiveProcess( pi.dwProcessId );
        ok( ret, "debugging failed\n" );
        if (!ReadProcessMemory( pi.hProcess, proc_info.PebBaseAddress, &peb, sizeof(peb), &res )) res = 0;
        ok( res == sizeof(peb), "wrong len %Ix\n", res );
        ok( peb.BeingDebugged == !!ret, "BeingDebugged is %u\n", peb.BeingDebugged );
        if (!is_wow64)
        {
            if (!ReadProcessMemory( pi.hProcess, ULongToPtr(teb32.Peb), &peb32, sizeof(peb32), &res )) res = 0;
            ok( res == sizeof(peb32), "wrong len %Ix\n", res );
            ok( peb32.BeingDebugged == !!ret, "BeingDebugged is %u\n", peb32.BeingDebugged );
        }

        TerminateProcess( pi.hProcess, 0 );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
    }

    if (is_win64 && native_machine == IMAGE_FILE_MACHINE_ARM64 &&
        create_process_machine( (char *)"C:\\windows\\system32\\regsvr32.exe /?", CREATE_SUSPENDED,
                                IMAGE_FILE_MACHINE_AMD64, &pi ))
    {
        memset( &info, 0xcc, sizeof(info) );
        status = NtQueryInformationThread( pi.hThread, ThreadBasicInformation, &info, sizeof(info), NULL );
        ok( !status, "ThreadBasicInformation failed %lx\n", status );
        if (!ReadProcessMemory( pi.hProcess, info.TebBaseAddress, &teb, sizeof(teb), &res )) res = 0;
        ok( res == sizeof(teb), "wrong len %Ix\n", res );
        ok( teb.Tib.Self == info.TebBaseAddress, "wrong teb %p / %p\n", teb.Tib.Self, info.TebBaseAddress );
        ok( !teb.GdiBatchCount, "GdiBatchCount set\n" );
        ok( !teb.WowTebOffset, "wrong teb offset %ld\n", teb.WowTebOffset );
        ok( !teb.Tib.ExceptionList, "wrong Tib.ExceptionList %p\n", (char *)teb.Tib.ExceptionList );

        status = NtQueryInformationProcess( pi.hProcess, ProcessBasicInformation,
                                            &proc_info, sizeof(proc_info), NULL );
        ok( !status, "ProcessBasicInformation failed %lx\n", status );
        ok( proc_info.PebBaseAddress == teb.Peb, "wrong peb %p / %p\n", proc_info.PebBaseAddress, teb.Peb );

        status = NtQueryInformationProcess( pi.hProcess, ProcessWow64Information,
                                            &peb_ptr, sizeof(peb_ptr), NULL );
        ok( !status, "ProcessWow64Information failed %lx\n", status );
        ok( !peb_ptr, "wrong peb %p\n", (void *)peb_ptr );

        if (!ReadProcessMemory( pi.hProcess, proc_info.PebBaseAddress, &peb, sizeof(peb), &res )) res = 0;
        ok( res == sizeof(peb), "wrong len %Ix\n", res );
        ok( !peb.BeingDebugged, "BeingDebugged is %u\n", peb.BeingDebugged );

        ResumeThread( pi.hThread );
        WaitForInputIdle( pi.hProcess, 1000 );

        test_amd64_shared_info( pi.hProcess );

        TerminateProcess( pi.hProcess, 0 );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
    }

    if (CreateProcessA( "C:\\windows\\system32\\msinfo32.exe", NULL, NULL, NULL,
                        FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi ))
    {
        memset( &info, 0xcc, sizeof(info) );
        status = NtQueryInformationThread( pi.hThread, ThreadBasicInformation, &info, sizeof(info), NULL );
        ok( !status, "ThreadBasicInformation failed %lx\n", status );
        if (!is_wow64)
        {
            if (!ReadProcessMemory( pi.hProcess, info.TebBaseAddress, &teb, sizeof(teb), &res )) res = 0;
            ok( res == sizeof(teb), "wrong len %Ix\n", res );
            ok( teb.Tib.Self == info.TebBaseAddress, "wrong teb %p / %p\n",
                teb.Tib.Self, info.TebBaseAddress );
            ok( !teb.GdiBatchCount, "GdiBatchCount set\n" );
            ok( !teb.WowTebOffset || broken( teb.WowTebOffset == 1 ),  /* vista */
                "wrong teb offset %ld\n", teb.WowTebOffset );
        }
        else ok( !info.TebBaseAddress, "got teb %p\n", info.TebBaseAddress );

        status = NtQueryInformationProcess( pi.hProcess, ProcessBasicInformation,
                                            &proc_info, sizeof(proc_info), NULL );
        ok( !status, "ProcessBasicInformation failed %lx\n", status );
        if (is_wow64)
            ok( !proc_info.PebBaseAddress ||
                broken( (char *)proc_info.PebBaseAddress >= (char *)0x7f000000 ), /* vista */
                "wrong peb %p\n", proc_info.PebBaseAddress );
        else
            ok( proc_info.PebBaseAddress == teb.Peb, "wrong peb %p / %p\n",
                proc_info.PebBaseAddress, teb.Peb );

        ResumeThread( pi.hThread );
        WaitForInputIdle( pi.hProcess, 1000 );

        if (pRtlWow64GetSharedInfoProcess)
        {
            wow64 = 0xcc;
            memset( buffer, 0xcc, sizeof(buffer) );
            status = pRtlWow64GetSharedInfoProcess( pi.hProcess, &wow64, wow64info );
            ok( !status, "RtlWow64GetSharedInfoProcess failed %lx\n", status );
            ok( !wow64, "wrong wow64 %u\n", wow64 );
            ok( buffer[0] == 0xcccccccc, "buffer set %lx\n", buffer[0] );
        }

        TerminateProcess( pi.hProcess, 0 );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
    }

    Wow64RevertWow64FsRedirection( redir );

#ifndef _WIN64
    if (is_wow64)
    {
        PEB64 *peb64;
        TEB64 *teb64 = (TEB64 *)NtCurrentTeb()->GdiBatchCount;
        UINT64 nls_address_bias = 0;
        UINT64 expected_ansi, expected_oem, expected_case;
        ULONG translated = FALSE;

        status = NtQueryVirtualMemory( GetCurrentProcess(), NtCurrentTeb(),
                                       MemoryWineWow64TranslatedInformation,
                                       &translated, sizeof(translated), NULL );
        if (!status && translated) nls_address_bias = WINE_LOW_VA_SHADOW_BASE;

        ok( !!teb64, "GdiBatchCount not set\n" );
        ok( (char *)NtCurrentTeb() + NtCurrentTeb()->WowTebOffset == (char *)teb64 ||
            broken(!NtCurrentTeb()->WowTebOffset),  /* pre-win10 */
            "wrong WowTebOffset %lx (%p/%p)\n", NtCurrentTeb()->WowTebOffset, teb64, NtCurrentTeb() );
        ok( (char *)teb64 + 0x2000 == (char *)NtCurrentTeb(), "unexpected diff %p / %p\n",
            teb64, NtCurrentTeb() );
        ok( (char *)teb64 + teb64->WowTebOffset == (char *)NtCurrentTeb() ||
            broken( !teb64->WowTebOffset || teb64->WowTebOffset == 1 ),  /* pre-win10 */
            "wrong WowTebOffset %lx (%p/%p)\n", teb64->WowTebOffset, teb64, NtCurrentTeb() );
        ok( !teb64->GdiBatchCount, "GdiBatchCount set %lx\n", teb64->GdiBatchCount );
        ok( teb64->Tib.ExceptionList == PtrToUlong( NtCurrentTeb() ), "wrong Tib.ExceptionList %s / %p\n",
            wine_dbgstr_longlong(teb64->Tib.ExceptionList), NtCurrentTeb() );
        ok( teb64->Tib.Self == PtrToUlong( teb64 ), "wrong Tib.Self %s / %p\n",
            wine_dbgstr_longlong(teb64->Tib.Self), teb64 );
        ok( teb64->StaticUnicodeString.Buffer == PtrToUlong( teb64->StaticUnicodeBuffer ),
            "wrong StaticUnicodeString %s / %p\n",
            wine_dbgstr_longlong(teb64->StaticUnicodeString.Buffer), teb64->StaticUnicodeBuffer );
        ok( teb64->ClientId.UniqueProcess == GetCurrentProcessId(), "wrong pid %s / %lx\n",
            wine_dbgstr_longlong(teb64->ClientId.UniqueProcess), GetCurrentProcessId() );
        ok( teb64->ClientId.UniqueThread == GetCurrentThreadId(), "wrong tid %s / %lx\n",
            wine_dbgstr_longlong(teb64->ClientId.UniqueThread), GetCurrentThreadId() );
        peb64 = ULongToPtr( teb64->Peb );
        expected_ansi = PtrToUlong( NtCurrentTeb()->Peb->AnsiCodePageData );
        expected_oem = PtrToUlong( NtCurrentTeb()->Peb->OemCodePageData );
        expected_case = PtrToUlong( NtCurrentTeb()->Peb->UnicodeCaseTableData );
        if (nls_address_bias)
        {
            if (expected_ansi) expected_ansi += nls_address_bias;
            if (expected_oem) expected_oem += nls_address_bias;
            if (expected_case) expected_case += nls_address_bias;
        }
        ok( peb64->ImageBaseAddress == PtrToUlong( NtCurrentTeb()->Peb->ImageBaseAddress ),
            "wrong ImageBaseAddress %s / %p\n",
            wine_dbgstr_longlong(peb64->ImageBaseAddress), NtCurrentTeb()->Peb->ImageBaseAddress);
        ok( peb64->OSBuildNumber == NtCurrentTeb()->Peb->OSBuildNumber, "wrong OSBuildNumber %lx / %lx\n",
            peb64->OSBuildNumber, NtCurrentTeb()->Peb->OSBuildNumber );
        ok( peb64->OSPlatformId == NtCurrentTeb()->Peb->OSPlatformId, "wrong OSPlatformId %lx / %lx\n",
            peb64->OSPlatformId, NtCurrentTeb()->Peb->OSPlatformId );
        ok( peb64->AnsiCodePageData == expected_ansi,
            "wrong AnsiCodePageData %I64x / %p\n",
            peb64->AnsiCodePageData, NtCurrentTeb()->Peb->AnsiCodePageData );
        ok( peb64->OemCodePageData == expected_oem,
            "wrong OemCodePageData %I64x / %p\n",
            peb64->OemCodePageData, NtCurrentTeb()->Peb->OemCodePageData );
        ok( peb64->UnicodeCaseTableData == expected_case,
            "wrong UnicodeCaseTableData %I64x / %p\n",
            peb64->UnicodeCaseTableData, NtCurrentTeb()->Peb->UnicodeCaseTableData );
        return;
    }
#endif
    ok( !NtCurrentTeb()->GdiBatchCount, "GdiBatchCount set to %lx\n", NtCurrentTeb()->GdiBatchCount );
    ok( !NtCurrentTeb()->WowTebOffset || broken( NtCurrentTeb()->WowTebOffset == 1 ), /* vista */
        "WowTebOffset set to %lx\n", NtCurrentTeb()->WowTebOffset );
}

static void test_selectors(void)
{
#ifndef __arm__
    THREAD_DESCRIPTOR_INFORMATION info;
    NTSTATUS status;
    ULONG base, limit, sel, retlen;
    union { LDT_ENTRY entry; ULONG ul[2]; } ds_entry = { .ul[0] = 0 };
    I386_CONTEXT context = { CONTEXT_I386_CONTROL | CONTEXT_I386_SEGMENTS };

#ifdef _WIN64
    if (!pRtlWow64GetThreadSelectorEntry)
    {
        win_skip( "RtlWow64GetThreadSelectorEntry not supported\n" );
        return;
    }
    if (!pRtlWow64GetThreadContext || pRtlWow64GetThreadContext( GetCurrentThread(), &context ))
    {
        /* hardcoded values */
#ifdef __arm64ec__
        context.SegCs = 0x23;
        context.SegSs = 0x2b;
        context.SegFs = 0x53;
#elif defined __x86_64__
        context.SegCs = 0x23;
        __asm__( "movw %%fs,%0" : "=m" (context.SegFs) );
        __asm__( "movw %%ss,%0" : "=m" (context.SegSs) );
#else
        context.SegCs = 0x1b;
        context.SegSs = 0x23;
        context.SegFs = 0x3b;
#endif
    }
#define GET_ENTRY(info,size,ret) \
    pRtlWow64GetThreadSelectorEntry( GetCurrentThread(), info, size, ret )

#else
    GetThreadContext( GetCurrentThread(), &context );
#define GET_ENTRY(info,size,ret) \
    NtQueryInformationThread( GetCurrentThread(), ThreadDescriptorTableEntry, info, size, ret )
#endif

    trace( "cs %04lx ss %04lx fs %04lx\n", context.SegCs, context.SegSs, context.SegFs );
    retlen = 0xdeadbeef;
    info.Selector = 0;
    status = GET_ENTRY( &info, sizeof(info) - 1, &retlen );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "wrong status %lx\n", status );
    ok( retlen == 0xdeadbeef, "len set %lu\n", retlen );

    retlen = 0xdeadbeef;
    status = GET_ENTRY( &info, sizeof(info) + 1, &retlen );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "wrong status %lx\n", status );
    ok( retlen == 0xdeadbeef, "len set %lu\n", retlen );

    retlen = 0xdeadbeef;
    status = GET_ENTRY( NULL, 0, &retlen );
    ok( status == STATUS_INFO_LENGTH_MISMATCH, "wrong status %lx\n", status );
    ok( retlen == 0xdeadbeef, "len set %lu\n", retlen );

    status = GET_ENTRY( &info, sizeof(info), NULL );
    ok( !status, "wrong status %lx\n", status );

    for (info.Selector = 0; info.Selector < 0x100; info.Selector++)
    {
        retlen = 0xdeadbeef;
        status = GET_ENTRY( &info, sizeof(info), &retlen );
        base = (info.Entry.BaseLow |
                (info.Entry.HighWord.Bytes.BaseMid << 16) |
                (info.Entry.HighWord.Bytes.BaseHi << 24));
        limit = (info.Entry.LimitLow | info.Entry.HighWord.Bits.LimitHi << 16);
        sel = info.Selector | 3;

        if (sel == 0x03)  /* null selector */
        {
            ok( !status, "wrong status %lx\n", status );
            ok( retlen == sizeof(info.Entry), "len set %lu\n", retlen );
            ok( !base, "wrong base %lx\n", base );
            ok( !limit, "wrong limit %lx\n", limit );
            ok( !info.Entry.HighWord.Bytes.Flags1, "wrong flags1 %x\n", info.Entry.HighWord.Bytes.Flags1 );
            ok( !info.Entry.HighWord.Bytes.Flags2, "wrong flags2 %x\n", info.Entry.HighWord.Bytes.Flags2 );
        }
        else if (sel == context.SegCs)  /* 32-bit code selector */
        {
            ok( !status, "wrong status %lx\n", status );
            ok( retlen == sizeof(info.Entry), "len set %lu\n", retlen );
            ok( !base, "wrong base %lx\n", base );
            ok( limit == 0xfffff, "wrong limit %lx\n", limit );
            ok( info.Entry.HighWord.Bits.Type == 0x1b, "wrong type %x\n", info.Entry.HighWord.Bits.Type );
            ok( info.Entry.HighWord.Bits.Dpl == 3, "wrong dpl %x\n", info.Entry.HighWord.Bits.Dpl );
            ok( info.Entry.HighWord.Bits.Pres, "wrong pres\n" );
            ok( !info.Entry.HighWord.Bits.Sys, "wrong sys\n" );
            ok( info.Entry.HighWord.Bits.Default_Big, "wrong big\n" );
            ok( info.Entry.HighWord.Bits.Granularity, "wrong granularity\n" );
        }
        else if (sel == context.SegSs)  /* 32-bit data selector */
        {
            ok( !status, "wrong status %lx\n", status );
            ok( retlen == sizeof(info.Entry), "len set %lu\n", retlen );
            ok( !base, "wrong base %lx\n", base );
            ok( limit == 0xfffff, "wrong limit %lx\n", limit );
            ok( info.Entry.HighWord.Bits.Type == 0x13, "wrong type %x\n", info.Entry.HighWord.Bits.Type );
            ok( info.Entry.HighWord.Bits.Dpl == 3, "wrong dpl %x\n", info.Entry.HighWord.Bits.Dpl );
            ok( info.Entry.HighWord.Bits.Pres, "wrong pres\n" );
            ok( !info.Entry.HighWord.Bits.Sys, "wrong sys\n" );
            ok( info.Entry.HighWord.Bits.Default_Big, "wrong big\n" );
            ok( info.Entry.HighWord.Bits.Granularity, "wrong granularity\n" );
            ds_entry.entry = info.Entry;
        }
        else if (sel == context.SegFs)  /* TEB selector */
        {
            ok( !status, "wrong status %lx\n", status );
            ok( retlen == sizeof(info.Entry), "len set %lu\n", retlen );
#ifdef _WIN64
            if (NtCurrentTeb()->WowTebOffset == 0x2000)
                ok( base == (ULONG_PTR)NtCurrentTeb() + 0x2000, "wrong base %lx / %p\n",
                    base, NtCurrentTeb() );
#else
            ok( base == (ULONG_PTR)NtCurrentTeb(), "wrong base %lx / %p\n", base, NtCurrentTeb() );
#endif
            ok( limit == 0xfff || broken(limit == 0x4000),  /* <= win8 */
                "wrong limit %lx\n", limit );
            ok( info.Entry.HighWord.Bits.Type == 0x13, "wrong type %x\n", info.Entry.HighWord.Bits.Type );
            ok( info.Entry.HighWord.Bits.Dpl == 3, "wrong dpl %x\n", info.Entry.HighWord.Bits.Dpl );
            ok( info.Entry.HighWord.Bits.Pres, "wrong pres\n" );
            ok( !info.Entry.HighWord.Bits.Sys, "wrong sys\n" );
            ok( info.Entry.HighWord.Bits.Default_Big, "wrong big\n" );
            ok( !info.Entry.HighWord.Bits.Granularity, "wrong granularity\n" );
        }
        else if (!status)
        {
            ok( retlen == sizeof(info.Entry), "len set %lu\n", retlen );
            trace( "succeeded for %04lx base %lx limit %lx type %x\n",
                   sel, base, limit, info.Entry.HighWord.Bits.Type );
            ok( !(sel & 4), "succeeded for LDT selector %04lx\n", sel );
        }
        else
        {
            ok( status == STATUS_UNSUCCESSFUL ||
                ((sel & 4) && (status == STATUS_NO_LDT)) ||
                broken( status == STATUS_ACCESS_VIOLATION),  /* <= win8 */
                "%lx: wrong status %lx\n", info.Selector, status );
            ok( retlen == 0xdeadbeef, "len set %lu\n", retlen );
        }
    }

    status = pNtSetLdtEntries( 0, ds_entry.ul[0], ds_entry.ul[1], 0, ds_entry.ul[0], ds_entry.ul[1] );
    if (status != STATUS_NOT_IMPLEMENTED)
    {
        ok( !status, "NtSetLdtEntries failed: %08lx\n", status );

        status = pNtSetLdtEntries( 0x000f, ds_entry.ul[0], ds_entry.ul[1], 0x001f, ds_entry.ul[0], ds_entry.ul[1] );
        ok( !status, "NtSetLdtEntries failed: %08lx\n", status );

        info.Selector = 0x000f;
        memset(&info.Entry, 0x9a, sizeof(info.Entry));
        status = GET_ENTRY( &info, sizeof(info), NULL );
        ok(!status, "wrong status %lx\n", status);
        ok(!memcmp(&ds_entry, &info.Entry, sizeof(ds_entry)), "entries do not match\n");

        info.Selector = 0x001f;
        memset(&info.Entry, 0x9a, sizeof(info.Entry));
        status = GET_ENTRY( &info, sizeof(info), NULL );
        ok(!status, "wrong status %lx\n", status);
        ok(!memcmp(&ds_entry, &info.Entry, sizeof(ds_entry)), "entries do not match\n");
    }
    else skip( "NtSetLdtEntries not supported\n" );

#undef GET_ENTRY
#endif /* __arm__ */
}

static void test_image_mappings(void)
{
    MEM_EXTENDED_PARAMETER ext = { .Type = MemExtendedParameterImageMachine };
    HANDLE file, mapping, process = GetCurrentProcess();
    NTSTATUS status;
    SIZE_T size;
    LARGE_INTEGER offset;
    void *ptr;

    if (!pNtMapViewOfSectionEx)
    {
        win_skip( "NtMapViewOfSectionEx() not supported\n" );
        return;
    }

    offset.QuadPart = 0;
    file = CreateFileA( "c:\\windows\\system32\\version.dll", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0 );
    ok( file != INVALID_HANDLE_VALUE, "Failed to open version.dll\n" );
    mapping = CreateFileMappingA( file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );
    CloseHandle( file );

    ptr = NULL;
    size = 0;
    ext.ULong = IMAGE_FILE_MACHINE_AMD64;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
    if (status == STATUS_INVALID_PARAMETER)
    {
        win_skip( "MemExtendedParameterImageMachine not supported\n" );
        NtClose( mapping );
        return;
    }
    if (current_machine == IMAGE_FILE_MACHINE_AMD64)
    {
        ok( status == STATUS_SUCCESS || status == STATUS_IMAGE_NOT_AT_BASE,
            "NtMapViewOfSection returned %08lx\n", status );
        NtUnmapViewOfSection( process, ptr );
    }
    else if (current_machine == IMAGE_FILE_MACHINE_ARM64)
    {
        todo_wine
        ok( status == STATUS_IMAGE_MACHINE_TYPE_MISMATCH, "NtMapViewOfSection returned %08lx\n", status );
        NtUnmapViewOfSection( process, ptr );
    }
    else ok( status == STATUS_NOT_SUPPORTED, "NtMapViewOfSection returned %08lx\n", status );

    ptr = NULL;
    size = 0;
    ext.ULong = IMAGE_FILE_MACHINE_I386;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
    if (current_machine == IMAGE_FILE_MACHINE_I386)
    {
        ok( status == STATUS_SUCCESS || status == STATUS_IMAGE_NOT_AT_BASE,
            "NtMapViewOfSection returned %08lx\n", status );
        NtUnmapViewOfSection( process, ptr );
    }
    else ok( status == STATUS_NOT_SUPPORTED, "NtMapViewOfSection returned %08lx\n", status );

    ptr = NULL;
    size = 0;
    ext.ULong = IMAGE_FILE_MACHINE_ARM64;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
    if (native_machine == IMAGE_FILE_MACHINE_ARM64)
    {
        switch (current_machine)
        {
        case IMAGE_FILE_MACHINE_ARM64:
            ok( status == STATUS_SUCCESS || status == STATUS_IMAGE_NOT_AT_BASE,
                "NtMapViewOfSection returned %08lx\n", status );
            NtUnmapViewOfSection( process, ptr );
            break;
        case IMAGE_FILE_MACHINE_AMD64:
            ok( status == STATUS_IMAGE_MACHINE_TYPE_MISMATCH, "NtMapViewOfSection returned %08lx\n", status );
            NtUnmapViewOfSection( process, ptr );
            break;
        default:
            ok( status == STATUS_NOT_SUPPORTED, "NtMapViewOfSection returned %08lx\n", status );
            break;
        }
    }
    else ok( status == STATUS_NOT_SUPPORTED, "NtMapViewOfSection returned %08lx\n", status );

    ptr = NULL;
    size = 0;
    ext.ULong = IMAGE_FILE_MACHINE_R3000;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
    ok( status == STATUS_NOT_SUPPORTED, "NtMapViewOfSection returned %08lx\n", status );

    ptr = NULL;
    size = 0;
    ext.ULong = 0;
    status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
    ok( status == STATUS_SUCCESS || status == STATUS_IMAGE_NOT_AT_BASE,
        "NtMapViewOfSection returned %08lx\n", status );
    NtUnmapViewOfSection( process, ptr );

    NtClose( mapping );

    if (is_wow64)
    {
        file = CreateFileA( "c:\\windows\\sysnative\\version.dll", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, 0 );
        ok( file != INVALID_HANDLE_VALUE, "Failed to open version.dll\n" );

        mapping = CreateFileMappingA( file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL );
        ok( mapping != 0, "CreateFileMapping failed\n" );
        CloseHandle( file );

        ptr = NULL;
        size = 0;
        ext.ULong = native_machine;
        status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
        ok( status == STATUS_SUCCESS || status == STATUS_IMAGE_NOT_AT_BASE,
            "NtMapViewOfSection returned %08lx\n", status );
        NtUnmapViewOfSection( process, ptr );

        ptr = NULL;
        size = 0;
        ext.ULong = IMAGE_FILE_MACHINE_I386;
        status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
        ok( status == STATUS_NOT_SUPPORTED, "NtMapViewOfSection returned %08lx\n", status );
        NtClose( mapping );
    }
    else if (native_machine == IMAGE_FILE_MACHINE_AMD64 || native_machine == IMAGE_FILE_MACHINE_ARM64)
    {
        file = CreateFileA( "c:\\windows\\syswow64\\version.dll", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0 );
        ok( file != INVALID_HANDLE_VALUE, "Failed to open version.dll\n" );

        mapping = CreateFileMappingA( file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL );
        ok( mapping != 0, "CreateFileMapping failed\n" );
        CloseHandle( file );

        ptr = NULL;
        size = 0;
        ext.ULong = native_machine;
        status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
        ok( status == STATUS_NOT_SUPPORTED, "NtMapViewOfSection returned %08lx\n", status );

        ptr = NULL;
        size = 0;
        ext.ULong = IMAGE_FILE_MACHINE_I386;
        status = pNtMapViewOfSectionEx( mapping, process, &ptr, &offset, &size, 0, PAGE_READONLY, &ext, 1 );
        ok( status == STATUS_IMAGE_MACHINE_TYPE_MISMATCH, "NtMapViewOfSection returned %08lx\n", status );
        NtUnmapViewOfSection( process, ptr );
        NtClose( mapping );
    }
}

static DWORD hook_code[] =
{
    0x58000048, /* ldr x8, 1f */
    0xd61f0100, /* br x8 */
    0, 0        /* 1: .quad ptr */
};

static const DWORD log_params_code[] =
{
    0x10008009, /* adr x9, .+0x1000 */
    0xf940012a, /* ldr x10, [x9] */
    0xa8810540, /* stp x0, x1, [x10], #0x10 */
    0xa8810d42, /* stp x2, x3, [x10], #0x10 */
    0xa8811544, /* stp x4, x5, [x10], #0x10 */
    0xa8811d46, /* stp x6, x7, [x10], #0x10 */
    0xf900012a, /* str x10, [x9] */
    0xf9400520, /* ldr x0, [x9, #0x8] */
    0xd65f03c0, /* ret */
};

static void CALLBACK dummy_apc( ULONG_PTR arg )
{
}

struct expected_notification
{
    UINT    nb_args;
    ULONG64 args[6];
};

static void reset_results( ULONG64 *results )
{
    memset( results + 1, 0xcc, 0x1000 - sizeof(*results) );
    results[0] = (ULONG_PTR)(results + 2);
}

#define expect_notifications(results, count, expect, syscall) \
    expect_notifications_(results, count, expect, syscall, __LINE__)
static void expect_notifications_( ULONG64 *results, UINT count, const struct expected_notification *expect,
                                   BOOL syscall, int line )
{
    ULONG64 *regs = results + 2;
    UINT i, j, len = (results[0] - (ULONG_PTR)regs) / 8 / sizeof(*regs);

#ifdef _WIN64
    if (syscall)
    {
        CHPE_V2_CPU_AREA_INFO *cpu_area = NtCurrentTeb()->ChpeV2CpuAreaInfo;
        if (cpu_area && cpu_area->InSyscallCallback) count = 0;
    }
#endif

    ok_(__FILE__,line)( count == len, "wrong notification count %u / %u\n", len, count );
    for (i = 0; i < min( count, len ); i++, expect++, regs += 8)
        for (j = 0; j < expect->nb_args; j++)
            ok_(__FILE__,line)( regs[j] == expect->args[j], "%u: wrong args[%u] %I64x / %I64x\n",
                                i, j, regs[j], expect->args[j] );
    reset_results( results );
}

static void add_work_item( CROSS_PROCESS_WORK_LIST *list, UINT id, ULONG64 addr, ULONG64 size,
                           UINT arg0, UINT arg1, UINT arg2, UINT arg3 )
{
    CROSS_PROCESS_WORK_ENTRY *entry = pop_from_free_list( &list->free_list );

    entry->id = id;
    entry->addr = addr;
    entry->size = size;
    entry->args[0] = arg0;
    entry->args[1] = arg1;
    entry->args[2] = arg2;
    entry->args[3] = arg3;
    push_onto_work_list( &list->work_list, entry );
}

static void process_work_items(void)
{
#ifdef _WIN64
    if (pProcessPendingCrossProcessEmulatorWork)
    {
        pProcessPendingCrossProcessEmulatorWork();
        return;
    }
#endif
    QueueUserAPC( dummy_apc, GetCurrentThread(), 0 );
    SleepEx( 1, TRUE );
}

static BYTE old_code[sizeof(hook_code)];

static void *hook_notification_function( HMODULE module, const char *win32_name, const char *win64_name )
{
    BYTE *ptr;
    BOOL ret;

    if (current_machine == IMAGE_FILE_MACHINE_AMD64)
    {
        static const BYTE fast_forward[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9 };

        if (!(ptr = pRtlFindExportedRoutineByName( module, win64_name )))
        {
            skip( "%s not exported\n", win64_name  );
            return NULL;
        }
        if (memcmp( ptr, fast_forward, sizeof(fast_forward) ))
        {
            skip( "unrecognized x64 thunk for %s\n", win64_name  );
            return NULL;
        }
        ptr += sizeof(fast_forward);
        ptr += sizeof(LONG) + *(LONG *)ptr;
    }
    else if (!(ptr = pRtlFindExportedRoutineByName( module, win32_name )))
    {
        skip( "%s not exported\n", win32_name  );
        return NULL;
    }

    memcpy( old_code, ptr, sizeof(old_code) );
    ret = WriteProcessMemory( GetCurrentProcess(), ptr, hook_code, sizeof(hook_code), NULL );
    ok( ret, "hooking failed %p %lu\n", ptr, GetLastError() );
    return ptr;
}

static void test_notifications( HMODULE module, CROSS_PROCESS_WORK_LIST *list )
{
    void *code, *ptr, *addr = NULL;
    DWORD old_prot;
    SIZE_T size;
    ULONG64 *results;
    NTSTATUS status;
    HANDLE file, mapping;

    code = VirtualAlloc( NULL, 0x2000, MEM_COMMIT, PAGE_READWRITE );
    memcpy( code, log_params_code, sizeof(log_params_code) );
    VirtualProtect( code, 0x1000, PAGE_EXECUTE_READ, &old_prot );
    *(void **)&hook_code[2] = code;

    results = (ULONG64 *)((char *)code + 0x1000);
    reset_results( results );

    file = CreateFileA( "c:\\windows\\system32\\version.dll", GENERIC_READ | GENERIC_EXECUTE,
                        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0 );
    ok( file != INVALID_HANDLE_VALUE, "Failed to open version.dll\n" );
    mapping = CreateFileMappingA( file, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL );
    ok( mapping != 0, "CreateFileMapping failed\n" );

    if ((ptr = hook_notification_function( module, "BTCpuNotifyMemoryAlloc", "NotifyMemoryAlloc" )))
    {
        struct expected_notification expect_cross[2] =
        {
            { 6, { 0x1234567890, 0x6543210000, MEM_COMMIT, PAGE_EXECUTE_READ, 0, 0 } },
            { 6, { 0x1234567890, 0x6543210000, MEM_COMMIT, PAGE_EXECUTE_READ, 1, 0xdeadbeef } }
        };
        struct expected_notification expect_alloc[2] =
        {
            { 6, { 0, 0x123456, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0, 0 } },
            { 6, { 0, 0x124000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 1, 0 } }
        };

        add_work_item( list, CrossProcessPreVirtualAlloc, expect_cross[0].args[0], expect_cross[0].args[1],
                       expect_cross[0].args[2], expect_cross[0].args[3], 0, 0 );
        add_work_item( list, CrossProcessPostVirtualAlloc, expect_cross[1].args[0], expect_cross[1].args[1],
                       expect_cross[1].args[2], expect_cross[1].args[3], 0xdeadbeef, 0 );
        process_work_items();
        expect_notifications( results, 2, expect_cross, FALSE );
        ok( !list->work_list.first, "list not empty\n" );

        size = expect_alloc[0].args[1];
        status = NtAllocateVirtualMemory( GetCurrentProcess(), &addr, 0, &size, MEM_COMMIT, PAGE_READWRITE );
        ok( !status, "NtAllocateVirtualMemory failed %lx\n", status );
        expect_alloc[1].args[0] = (ULONG_PTR)addr;
        expect_notifications( results, 2, expect_alloc, TRUE );
        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuNotifyMemoryProtect", "NotifyMemoryProtect" )))
    {
        struct expected_notification expect_cross[2] =
        {
            { 5, { 0x1234567890, 0x6543210000, PAGE_READWRITE, 0, 0 } },
            { 5, { 0x1234567890, 0x6543210000, PAGE_READWRITE, 1, 0xdeadbeef } }
        };
        struct expected_notification expect_protect[2] =
        {
            { 5, { 0, 0x123456, PAGE_EXECUTE_READ, 0, 0 } },
            { 5, { 0, 0x124000, PAGE_EXECUTE_READ, 1, 0 } }
        };

        reset_results( results );
        add_work_item( list, CrossProcessPreVirtualProtect, expect_cross[0].args[0],
                       expect_cross[0].args[1], expect_cross[0].args[2], 0, 0, 0 );
        add_work_item( list, CrossProcessPostVirtualProtect, expect_cross[1].args[0],
                       expect_cross[1].args[1], expect_cross[1].args[2], 0xdeadbeef, 0, 0 );
        process_work_items();
        expect_notifications( results, 2, expect_cross, FALSE );
        ok( !list->work_list.first, "list not empty\n" );

        expect_protect[1].args[0] = (ULONG_PTR)addr;
        addr = (char *)addr + 0x123;
        expect_protect[0].args[0] = (ULONG_PTR)addr;
        size = expect_protect[0].args[1];
        status = NtProtectVirtualMemory( GetCurrentProcess(), &addr, &size, PAGE_EXECUTE_READ, &old_prot );
        ok( !status, "NtProtectVirtualMemory failed %lx\n", status );
        expect_notifications( results, 2, expect_protect, TRUE );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
        reset_results( results );
    }

    if ((ptr = hook_notification_function( module, "BTCpuNotifyMemoryFree", "NotifyMemoryFree" )))
    {
        struct expected_notification expect_cross[2] =
        {
            { 5, { 0x1234567890, 0x6543210000, MEM_RELEASE, 0, 0 } },
            { 5, { 0x1234567890, 0x6543210000, MEM_RELEASE, 1, 0xdeadbeef } }
        };
        struct expected_notification expect_free[2] =
        {
            { 5, { 0, 0x123456, MEM_RELEASE, 0, 0 } },
            { 5, { 0, 0x124000, MEM_RELEASE, 1, 0 } }
        };

        add_work_item( list, CrossProcessPreVirtualFree, expect_cross[0].args[0],
                       expect_cross[0].args[1], expect_cross[0].args[2], 0, 0, 0 );
        add_work_item( list, CrossProcessPostVirtualFree, expect_cross[1].args[0],
                       expect_cross[1].args[1], expect_cross[1].args[2], 0xdeadbeef, 0, 0 );
        process_work_items();
        expect_notifications( results, 2, expect_cross, FALSE );
        ok( !list->work_list.first, "list not empty\n" );

        expect_free[0].args[0] = (ULONG_PTR)addr;
        expect_free[1].args[0] = (ULONG_PTR)addr;
        size = expect_free[0].args[1];
        status = NtFreeVirtualMemory( GetCurrentProcess(), &addr, &size, MEM_RELEASE );
        ok( !status, "NtFreeVirtualMemory failed %lx\n", status );
        expect_notifications( results, 2, expect_free, TRUE );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuNotifyMemoryDirty", "BTCpu64NotifyMemoryDirty" )))
    {
        struct expected_notification expect = { 2, { 0x1234567890, 0x6543210000 } };

        add_work_item( list, CrossProcessMemoryWrite, expect.args[0], expect.args[1], 0, 0, 0, 0 );
        process_work_items();
        expect_notifications( results, 1, &expect, FALSE );
        ok( !list->work_list.first, "list not empty\n" );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuFlushInstructionCache2", "BTCpu64FlushInstructionCache" )))
    {
        struct expected_notification expect_cross = { 2, { 0x1234567890, 0x6543210000 } };
        struct expected_notification expect_flush = { 2, { 0, 0x1234 } };

        reset_results( results );
        add_work_item(list, CrossProcessFlushCache, expect_cross.args[0],
                      expect_cross.args[1], 0, 0, 0, 0 );
        process_work_items();
        expect_notifications( results, 1, &expect_cross, FALSE );
        ok( !list->work_list.first, "list not empty\n" );

        expect_flush.args[0] = (ULONG_PTR)ptr;
        NtFlushInstructionCache( GetCurrentProcess(), ptr, expect_flush.args[1] );
        expect_notifications( results, 1, &expect_flush, TRUE );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuFlushInstructionCacheHeavy", "FlushInstructionCacheHeavy" )))
    {
        struct expected_notification expect = { 2, { 0x1234567890, 0x6543210000 } };
        struct expected_notification expect2 = { 2 };

        reset_results( results );
        add_work_item( list, CrossProcessFlushCacheHeavy, expect.args[0], expect.args[1], 0, 0, 0, 0 );
        process_work_items();
        expect_notifications( results, 1, &expect, FALSE );
        ok( !list->work_list.first, "list not empty\n" );

        request_cross_process_flush( &list->work_list );
        process_work_items();
        expect_notifications( results, 1, &expect2, FALSE );
        ok( !list->work_list.first, "list not empty\n" );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuNotifyMapViewOfSection", "NotifyMapViewOfSection" )))
    {
        struct expected_notification expect = { 6 };
        LARGE_INTEGER offset;

        addr = NULL;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection( mapping, GetCurrentProcess(), &addr, 0, 0, &offset, &size,
                                     ViewShare, 0, PAGE_READONLY );
        ok( NT_SUCCESS(status), "NtMapViewOfSection failed %lx\n", status );
        expect_notifications( results, 0, NULL, TRUE );
        NtUnmapViewOfSection( GetCurrentProcess(), addr );

        /* only NtMapViewOfSection calls coming from the loader trigger a notification */
        NtCurrentTeb()->Tib.ArbitraryUserPointer = (WCHAR *)L"c:\\windows\\system32\\version.dll";
        addr = NULL;
        size = 0;
        results[1] = STATUS_SUCCESS;
        status = NtMapViewOfSection( mapping, GetCurrentProcess(), &addr, 0, 0, &offset, &size,
                                     ViewShare, 0, PAGE_READONLY );
        ok( NT_SUCCESS(status), "NtMapViewOfSection failed %lx\n", status );
        expect.args[0] = results[2];  /* FIXME: first parameter unknown */
        expect.args[1] = (ULONG_PTR)addr;
        expect.args[3] = size;
        expect.args[5] = PAGE_READONLY;
        expect_notifications( results, 1, &expect, TRUE );
        NtUnmapViewOfSection( GetCurrentProcess(), addr );

        results[1] = 0xdeadbeef;
        status = NtMapViewOfSection( mapping, GetCurrentProcess(), &addr, 0, 0, &offset, &size,
                                     ViewShare, 0, PAGE_READONLY );
#ifdef _WIN64
        if (NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback)
        {
            ok( status == STATUS_SUCCESS, "NtMapViewOfSection failed %lx\n", status );
            expect_notifications( results, 0, NULL, TRUE );
            NtUnmapViewOfSection( GetCurrentProcess(), addr );
        }
        else
#endif
        {
            ok( status == 0xdeadbeef, "NtMapViewOfSection failed %lx\n", status );
            expect.args[0] = results[2];  /* FIXME: first parameter unknown */
            expect.args[1] = (ULONG_PTR)addr;
            expect.args[3] = size;
            expect.args[5] = PAGE_READONLY;
            expect_notifications( results, 1, &expect, TRUE );
        }
        NtCurrentTeb()->Tib.ArbitraryUserPointer = NULL;
        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuNotifyUnmapViewOfSection", "NotifyUnmapViewOfSection" )))
    {
        struct expected_notification expect[2] = { { 3 }, { 3 } };
        LARGE_INTEGER offset;

        addr = NULL;
        size = 0;
        offset.QuadPart = 0;
        status = NtMapViewOfSection( mapping, GetCurrentProcess(), &addr, 0, 0, &offset, &size,
                                     ViewShare, 0, PAGE_READONLY );
        ok( NT_SUCCESS(status), "NtMapViewOfSection failed %lx\n", status );
        NtUnmapViewOfSection( GetCurrentProcess(), (char *)addr + 0x123 );
        expect[0].args[0] = expect[1].args[0] = (ULONG_PTR)addr + 0x123;
        expect[1].args[1] = 1;
        expect_notifications( results, 2, expect, TRUE );

        NtUnmapViewOfSection( GetCurrentProcess(), (char *)0x1234 );
        expect[0].args[0] = expect[1].args[0] = 0x1234;
        expect[1].args[1] = 1;
        expect[1].args[2] = (ULONG)STATUS_NOT_MAPPED_VIEW;
        expect_notifications( results, 2, expect, TRUE );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuNotifyReadFile", "BTCpu64NotifyReadFile" )))
    {
        char buffer[0x123];
        IO_STATUS_BLOCK io;
        struct expected_notification expect[2] =
        {
            { 5, { (ULONG_PTR)file, (ULONG_PTR)buffer, sizeof(buffer), 0, 0 } },
            { 5, { (ULONG_PTR)file, (ULONG_PTR)buffer, sizeof(buffer), 1, 0 } }
        };

        reset_results( results );
        status = NtReadFile( file, 0, NULL, NULL, &io, buffer, sizeof(buffer), NULL, NULL );
        ok( !status, "NtReadFile failed %lx\n", status );
        expect_notifications( results, 2, expect, TRUE );

        status = NtReadFile( (HANDLE)0xdead, 0, NULL, NULL, &io, buffer, sizeof(buffer), NULL, NULL );
        ok( status == STATUS_INVALID_HANDLE, "NtReadFile failed %lx\n", status );
        expect[0].args[0] = expect[1].args[0] = 0xdead;
        expect[1].args[4] = (ULONG)STATUS_INVALID_HANDLE;
        expect_notifications( results, 2, expect, TRUE );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuThreadTerm", "ThreadTerm" )))
    {
        struct expected_notification expect = { 2, { 0xdead, 0xbeef } };

        reset_results( results );
        status = NtTerminateThread( (HANDLE)0xdead, 0xbeef );
        ok( status == STATUS_INVALID_HANDLE, "NtTerminateThread failed %lx\n", status );
        expect_notifications( results, 1, &expect, TRUE );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    if ((ptr = hook_notification_function( module, "BTCpuProcessTerm", "ProcessTerm" )))
    {
        struct expected_notification expect[2] =
        {
            { 3, { 0, 0, 0 } },
            { 3, { 0, 1, 0 } }
        };

        reset_results( results );
        status = NtTerminateProcess( (HANDLE)0xdead, 0xbeef );
        ok( status == STATUS_INVALID_HANDLE, "NtTerminateProcess failed %lx\n", status );
        expect_notifications( results, 0, NULL, TRUE );

        status = NtTerminateProcess( 0, 0xbeef );
        ok( !status, "NtTerminateProcess failed %lx\n", status );
        expect_notifications( results, 2, expect, TRUE );

        WriteProcessMemory( GetCurrentProcess(), ptr, old_code, sizeof(old_code), NULL );
    }

    NtClose( mapping );
    NtClose( file );
    VirtualFree( code, 0, MEM_RELEASE );
}


#ifdef _WIN64

static void test_cross_process_work_list(void)
{
    UINT i, next, count = 10, size = offsetof( CROSS_PROCESS_WORK_LIST, entries[count] );
    BOOLEAN res, flush;
    CROSS_PROCESS_WORK_ENTRY *ptr, *ret;
    CROSS_PROCESS_WORK_LIST *list = calloc( size, 1 );

    if (!pRtlWow64PopAllCrossProcessWorkFromWorkList)
    {
        win_skip( "cross process list not supported\n" );
        return;
    }

    list = calloc( size, 1 );
    for (i = 0; i < count; i++)
    {
        res = pRtlWow64PushCrossProcessWorkOntoFreeList( &list->free_list, &list->entries[i] );
        ok( res == TRUE, "%u: RtlWow64PushCrossProcessWorkOntoFreeList failed\n", i );
    }

    ok( list->free_list.counter == count, "wrong counter %u\n", list->free_list.counter );
    ok( CROSS_PROCESS_LIST_ENTRY( &list->free_list, list->free_list.first ) == &list->entries[count - 1],
        "wrong offset %u\n", list->free_list.first );
    for (i = count; i > 1; i--)
        ok( CROSS_PROCESS_LIST_ENTRY( &list->free_list, list->entries[i - 1].next ) == &list->entries[i - 2],
            "%u: wrong offset %x / %x\n", i, list->entries[i - 1].next,
            (UINT)((char *)&list->entries[i - 2] - (char *)&list->free_list) );
    ok( !list->entries[0].next, "wrong last offset %x\n", list->entries[0].next );

    next = list->entries[count - 1].next;
    ptr = pRtlWow64PopCrossProcessWorkFromFreeList( &list->free_list );
    ok( ptr == (void *)&list->entries[count - 1], "wrong ptr %p (%p)\n", ptr, list );
    ok( !ptr->next, "next not reset %x\n", ptr->next );
    ok( list->free_list.first == next, "wrong offset %x / %x\n", list->free_list.first, next );
    ok( list->free_list.counter == count + 1, "wrong counter %u\n", list->free_list.counter );

    ptr->next = 0xdead;
    ptr->id = 3;
    ptr->addr = 0xdeadbeef;
    ptr->size = 0x1000;
    ptr->args[0] = 7;
    ret = (void *)0xdeadbeef;
    res = pRtlWow64PushCrossProcessWorkOntoWorkList( &list->work_list, ptr, (void **)&ret );
    ok( res == TRUE, "RtlWow64PushCrossProcessWorkOntoWorkList failed\n" );
    ok( !ret, "got ret ptr %p\n", ret );
    ok( list->work_list.counter == 1, "wrong counter %u\n", list->work_list.counter );
    ok( ptr == CROSS_PROCESS_LIST_ENTRY( &list->work_list, list->work_list.first), "wrong ptr %p / %p\n",
        ptr, CROSS_PROCESS_LIST_ENTRY( &list->work_list, list->work_list.first ));
    ok( !ptr->next, "got next %x\n", ptr->next );

    next = list->work_list.first;
    ptr = pRtlWow64PopCrossProcessWorkFromFreeList( &list->free_list );
    ok( list->free_list.counter == count + 2, "wrong counter %u\n", list->free_list.counter );
    ptr->id = 20;
    ptr->addr = 0x123456;
    ptr->size = 0x2345;
    res = pRtlWow64PushCrossProcessWorkOntoWorkList( &list->work_list, ptr, (void **)&ret );
    ok( res == TRUE, "RtlWow64PushCrossProcessWorkOntoWorkList failed\n" );
    ok( !ret, "got ret ptr %p\n", ret );
    ok( list->work_list.counter == 2, "wrong counter %u\n", list->work_list.counter );
    ok( list->work_list.first == (char *)ptr - (char *)&list->work_list, "wrong ptr %p / %p\n",
        ptr, (char *)list + list->work_list.first );
    ok( ptr->next == next, "got wrong next %x / %x\n", ptr->next, next );

    flush = 0xcc;
    ptr = pRtlWow64PopAllCrossProcessWorkFromWorkList( &list->work_list, &flush );
    ok( !flush, "RtlWow64PopAllCrossProcessWorkFromWorkList flush is TRUE\n" );
    ok( list->work_list.counter == 3, "wrong counter %u\n", list->work_list.counter );
    ok( !list->work_list.first, "list not empty %x\n", list->work_list.first );
    ok( ptr->addr == 0xdeadbeef, "wrong addr %s\n", wine_dbgstr_longlong(ptr->addr) );
    ok( ptr->size == 0x1000, "wrong size %s\n", wine_dbgstr_longlong(ptr->size) );
    ok( ptr->next, "next not set\n" );

    ptr = CROSS_PROCESS_LIST_ENTRY( &list->work_list, ptr->next );
    ok( ptr->addr == 0x123456, "wrong addr %s\n", wine_dbgstr_longlong(ptr->addr) );
    ok( ptr->size == 0x2345, "wrong size %s\n", wine_dbgstr_longlong(ptr->size) );
    ok( !ptr->next, "list not terminated\n" );

    res = pRtlWow64PushCrossProcessWorkOntoWorkList( &list->work_list, ptr, (void **)&ret );
    ok( res == TRUE, "RtlWow64PushCrossProcessWorkOntoWorkList failed\n" );
    ok( !ret, "got ret ptr %p\n", ret );
    ok( list->work_list.counter == 4, "wrong counter %u\n", list->work_list.counter );

    res = pRtlWow64RequestCrossProcessHeavyFlush( &list->work_list );
    ok( res == TRUE, "RtlWow64RequestCrossProcessHeavyFlush failed\n" );
    ok( list->work_list.counter == 5, "wrong counter %u\n", list->work_list.counter );
    ok( list->work_list.first & CROSS_PROCESS_LIST_FLUSH, "flush flag not set %x\n", list->work_list.first );
    ok( ptr == CROSS_PROCESS_LIST_ENTRY( &list->work_list, list->work_list.first), "wrong ptr %p / %p\n",
        ptr, CROSS_PROCESS_LIST_ENTRY( &list->work_list, list->work_list.first ));

    flush = 0xcc;
    ptr = pRtlWow64PopAllCrossProcessWorkFromWorkList( &list->work_list, &flush );
    ok( flush == TRUE, "RtlWow64PopAllCrossProcessWorkFromWorkList flush not set\n" );
    ok( list->work_list.counter == 6, "wrong counter %u\n", list->work_list.counter );
    ok( !list->work_list.first, "list not empty %x\n", list->work_list.first );
    ok( ptr->addr == 0x123456, "wrong addr %s\n", wine_dbgstr_longlong(ptr->addr) );
    ok( ptr->size == 0x2345, "wrong size %s\n", wine_dbgstr_longlong(ptr->size) );
    ok( !ptr->next, "next not set\n" );

    flush = 0xcc;
    ptr = pRtlWow64PopAllCrossProcessWorkFromWorkList( &list->work_list, &flush );
    ok( flush == FALSE, "RtlWow64PopAllCrossProcessWorkFromWorkList flush set\n" );
    ok( list->work_list.counter == 6, "wrong counter %u\n", list->work_list.counter );
    ok( !list->work_list.first, "list not empty %x\n", list->work_list.first );
    ok( !ptr, "got ptr %p\n", ptr );

    res = pRtlWow64RequestCrossProcessHeavyFlush( &list->work_list );
    ok( res == TRUE, "RtlWow64RequestCrossProcessHeavyFlush failed\n" );
    ok( list->work_list.counter == 7, "wrong counter %u\n", list->work_list.counter );
    ok( list->work_list.first & CROSS_PROCESS_LIST_FLUSH, "flush flag not set %x\n", list->work_list.first );

    res = pRtlWow64RequestCrossProcessHeavyFlush( &list->work_list );
    ok( res == TRUE, "RtlWow64RequestCrossProcessHeavyFlush failed\n" );
    ok( list->work_list.counter == 8, "wrong counter %u\n", list->work_list.counter );
    ok( list->work_list.first & CROSS_PROCESS_LIST_FLUSH, "flush flag not set %x\n", list->work_list.first );

    flush = 0xcc;
    ptr = pRtlWow64PopAllCrossProcessWorkFromWorkList( &list->work_list, &flush );
    ok( flush == TRUE, "RtlWow64PopAllCrossProcessWorkFromWorkList flush set\n" );
    ok( list->work_list.counter == 9, "wrong counter %u\n", list->work_list.counter );
    ok( !list->work_list.first, "list not empty %x\n", list->work_list.first );
    ok( !ptr, "got ptr %p\n", ptr );

    for (i = 0; i < count; i++)
    {
        ptr = pRtlWow64PopCrossProcessWorkFromFreeList( &list->free_list );
        if (!ptr) break;
        ok( list->free_list.counter == count + 3 + i, "wrong counter %u\n", list->free_list.counter );
    }
    ok( list->free_list.counter == count + 2 + i, "wrong counter %u\n", list->free_list.counter );
    ok( !list->free_list.first, "first still set %x\n", list->free_list.first );

    free( list );
}


static void test_cpu_area(void)
{
    if (pRtlWow64GetCpuAreaInfo)
    {
        static const struct
        {
            USHORT machine;
            NTSTATUS expect;
            ULONG_PTR align, size, offset, flag;
        } tests[] =
        {
            { IMAGE_FILE_MACHINE_I386,  0,  4, 0x2cc, 0x00, 0x00010000 },
            { IMAGE_FILE_MACHINE_AMD64, 0, 16, 0x4d0, 0x30, 0x00100000 },
            { IMAGE_FILE_MACHINE_ARMNT, 0,  8, 0x1a0, 0x00, 0x00200000 },
            { IMAGE_FILE_MACHINE_ARM64, 0, 16, 0x390, 0x00, 0x00400000 },
            { IMAGE_FILE_MACHINE_ARM,   STATUS_INVALID_PARAMETER },
            { IMAGE_FILE_MACHINE_THUMB, STATUS_INVALID_PARAMETER },
        };
        USHORT buffer[2048];
        WOW64_CPURESERVED *cpu;
        WOW64_CPU_AREA_INFO info;
        ULONG i, j;
        NTSTATUS status;
#define ALIGN(ptr,align) ((void *)(((ULONG_PTR)(ptr) + (align) - 1) & ~((align) - 1)))

        for (i = 0; i < ARRAY_SIZE(tests); i++)
        {
            for (j = 0; j < 8; j++)
            {
                cpu = (WOW64_CPURESERVED *)(buffer + j);
                cpu->Flags = 0;
                cpu->Machine = tests[i].machine;
                status = pRtlWow64GetCpuAreaInfo( cpu, 0, &info );
                ok( status == tests[i].expect, "%lu:%lu: failed %lx\n", i, j, status );
                if (status) continue;
                ok( info.Context == ALIGN( cpu + 1, tests[i].align ) ||
                    broken( (ULONG_PTR)info.Context == (ULONG)(ULONG_PTR)ALIGN( cpu + 1, tests[i].align ) ), /* win10 <= 1709 */
                    "%lu:%lu: wrong offset %Iu cpu %p context %p\n",
                    i, j, (ULONG_PTR)((char *)info.Context - (char *)cpu), cpu, info.Context );
                ok( info.ContextEx == ALIGN( (char *)info.Context + tests[i].size, sizeof(void*) ),
                    "%lu:%lu: wrong ex offset %lu\n", i, j, (ULONG)((char *)info.ContextEx - (char *)cpu) );
                ok( info.ContextFlagsLocation == (char *)info.Context + tests[i].offset,
                    "%lu:%lu: wrong flags offset %lu\n",
                    i, j, (ULONG)((char *)info.ContextFlagsLocation - (char *)info.Context) );
                ok( info.CpuReserved == cpu, "%lu:%lu: wrong cpu %p / %p\n", i, j, info.CpuReserved, cpu );
                ok( info.ContextFlag == tests[i].flag, "%lu:%lu: wrong flag %08lx\n", i, j, info.ContextFlag );
                ok( info.Machine == tests[i].machine, "%lu:%lu: wrong machine %x\n", i, j, info.Machine );
            }
        }
#undef ALIGN
    }
    else win_skip( "RtlWow64GetCpuAreaInfo not supported\n" );
}

static void test_exception_dispatcher(void)
{
#ifdef __x86_64__
    BYTE *code = (BYTE *)pKiUserExceptionDispatcher;
    void **hook;

    /* cld; mov xxx(%rip),%rax */
    ok( code[0] == 0xfc && code[1] == 0x48 && code[2] == 0x8b && code[3] == 0x05,
        "wrong opcodes %02x %02x %02x %02x\n", code[0], code[1], code[2], code[3] );
    hook = (void **)(code + 8 + *(int *)(code + 4));
    ok( !*hook, "hook %p set to %p\n", hook, *hook );
#endif
}

static void test_translated_view_information(void)
{
#ifdef _WIN64
    WINE_TRANSLATED_VIEW_INFORMATION info;
    SIZE_T ret_len = 0xdeadbeef;
    NTSTATUS status;
    void *page;

    C_ASSERT( sizeof(info) == 48 );

    page = VirtualAlloc( NULL, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    ok( !!page, "VirtualAlloc failed, error %lu\n", GetLastError() );
    if (!page) return;

    memset( &info, 0xcc, sizeof(info) );
    status = NtQueryVirtualMemory( GetCurrentProcess(), (char *)page + 0x321,
                                   MemoryWineTranslatedViewInformation,
                                   &info, sizeof(info) - 1, &ret_len );
    ok( status == STATUS_INFO_LENGTH_MISMATCH,
        "short translated-view query returned %#lx\n", status );
    ok( ret_len == 0xdeadbeef, "short query changed result length to %Iu\n", ret_len );

    status = NtQueryVirtualMemory( GetCurrentProcess(), page,
                                   MemoryWineTranslatedViewInformation,
                                   NULL, sizeof(info), &ret_len );
    ok( status == STATUS_ACCESS_VIOLATION,
        "NULL translated-view output returned %#lx\n", status );

    status = NtQueryVirtualMemory( (HANDLE)(ULONG_PTR)0xdeadbeef, page,
                                   MemoryWineTranslatedViewInformation,
                                   &info, sizeof(info), &ret_len );
    ok( status == STATUS_INVALID_HANDLE,
        "invalid-handle translated-view query returned %#lx\n", status );

    memset( &info, 0xcc, sizeof(info) );
    ret_len = 0xdeadbeef;
    status = NtQueryVirtualMemory( GetCurrentProcess(), (char *)page + 0x321,
                                   MemoryWineTranslatedViewInformation,
                                   &info, sizeof(info), &ret_len );
    ok( !status, "translated-view query returned %#lx\n", status );
    if (!status)
    {
        ok( info.Version == WINE_TRANSLATED_VIEW_INFORMATION_VERSION,
            "unexpected version %lu\n", info.Version );
        ok( !info.Flags, "unexpected identity-view flags %#lx\n", info.Flags );
        ok( !info.Reserved, "unexpected reserved value %#lx\n", info.Reserved );
        ok( info.GuestBase == info.HostBase,
            "identity guest base %p differs from host base %p\n",
            info.GuestBase, info.HostBase );
        ok( (char *)page + 0x321 >= (char *)info.GuestBase &&
            (char *)page + 0x321 < (char *)info.GuestBase + info.RegionSize,
            "query address is outside returned range %p+%Iu\n",
            info.GuestBase, info.RegionSize );
        ok( !!info.AllocationBase, "missing allocation base\n" );
        ok( info.Protect == PAGE_READWRITE, "unexpected protection %#lx\n", info.Protect );
        ok( ret_len == sizeof(info), "result length %Iu, expected %Iu\n",
            ret_len, sizeof(info) );
    }

    VirtualFree( page, 0, MEM_RELEASE );
#endif
}

#ifdef __arm64ec__
enum resync_hook_mode
{
    RESYNC_HOOK_RETURN,
    RESYNC_HOOK_BLOCK_FIRST,
    RESYNC_HOOK_MUTATE,
    RESYNC_HOOK_MUTATE_ALWAYS,
};

static LONG resync_hook_calls;
static LONG resync_hook_callback_state;
static volatile LONG resync_hook_mode;
static volatile NTSTATUS resync_hook_status;
static volatile NTSTATUS resync_hook_mutation_status;
static HANDLE resync_hook_entered_event;
static HANDLE resync_hook_release_event;
static void *resync_hook_mutation_addr;

static NTSTATUS WINAPI resync_test_hook(void)
{
    CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    LONG call = InterlockedIncrement( &resync_hook_calls );

    if (cpu && cpu->InSyscallCallback)
        InterlockedIncrement( &resync_hook_callback_state );
    if (resync_hook_mode == RESYNC_HOOK_BLOCK_FIRST && call == 1)
    {
        SetEvent( resync_hook_entered_event );
        if (WaitForSingleObject( resync_hook_release_event, 10000 ))
            resync_hook_mutation_status = STATUS_TIMEOUT;
    }
    else if ((resync_hook_mode == RESYNC_HOOK_MUTATE && call == 1) ||
             resync_hook_mode == RESYNC_HOOK_MUTATE_ALWAYS)
    {
        SIZE_T size = 0x1000;
        void *addr = NULL;
        NTSTATUS status;

        status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        if (!status && resync_hook_mode == RESYNC_HOOK_MUTATE) resync_hook_mutation_addr = addr;
        else if (!status)
        {
            size = 0;
            status = NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
        }
        if (status) resync_hook_mutation_status = status;
    }
    return resync_hook_status;
}

struct resync_gate_thread_params
{
    NTSTATUS (WINAPI *prepare)(void);
    HANDLE ready_event;
    HANDLE start_event;
};

static DWORD CALLBACK resync_gate_thread( void *arg )
{
    struct resync_gate_thread_params *params = arg;

    SetEvent( params->ready_event );
    if (WaitForSingleObject( params->start_event, 10000 )) return STATUS_TIMEOUT;
    return params->prepare();
}

static void test_x64_execution_gate(void)
{
    NTSTATUS (WINAPI *prepare_x64_execution)(void);
    NTSTATUS (WINAPI *get_x64_syscall_dispatcher)(ULONG_PTR *, ULONG *);
    struct resync_gate_thread_params thread_params;
    void *hook, *failure_addr, *reset_addr;
    void *addr = NULL, *addr_ex = NULL, *map = NULL, *map_ex = NULL;
    HANDLE thread = NULL, section = NULL;
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    HMODULE module = GetModuleHandleA( "xtajit64.dll" );
    LARGE_INTEGER section_size, offset;
    DWORD wait, exit_code;
    BOOL ret;
    SIZE_T size;
    ULONG old_protect;
    ULONG_PTR dispatcher = 0;
    ULONG count = 0;
    NTSTATUS failure_status[8];
    NTSTATUS status;

    prepare_x64_execution = pRtlFindExportedRoutineByName(
        ntdll, "__wine_arm64ec_prepare_x64_execution" );
    get_x64_syscall_dispatcher = pRtlFindExportedRoutineByName(
        ntdll, "__wine_arm64ec_get_x64_syscall_dispatcher" );
    ok( !!prepare_x64_execution, "x64 execution preparation gate is missing\n" );
    ok( !!get_x64_syscall_dispatcher, "x64 syscall dispatcher query is missing\n" );
    if (!prepare_x64_execution || !get_x64_syscall_dispatcher) return;

    status = prepare_x64_execution();
    ok( !status, "identity x64 execution preparation returned %#lx\n", status );
    status = prepare_x64_execution();
    ok( !status, "repeated identity x64 execution preparation returned %#lx\n", status );

    *(void **)&hook_code[2] = resync_test_hook;
    if (module && (hook = hook_notification_function( module,
                       "ResyncIdentityMemoryMappingsStatus",
                       "ResyncIdentityMemoryMappingsStatus" )))
    {
        resync_hook_mode = RESYNC_HOOK_RETURN;
        resync_hook_status = STATUS_SUCCESS;
        resync_hook_mutation_status = STATUS_SUCCESS;
        resync_hook_calls = resync_hook_callback_state = 0;

        size = 0x1000;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        if (!status)
            status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size,
                                             PAGE_READONLY, &old_protect );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "coalesced nested allocation/protection failed %#lx\n", status );
        status = prepare_x64_execution();
        ok( !status, "coalesced nested mutation resync failed %#lx\n", status );
        ok( resync_hook_calls == 1, "coalesced nested mutations caused %ld resyncs\n",
            resync_hook_calls );
        status = prepare_x64_execution();
        ok( !status, "clean execution gate failed %#lx\n", status );
        ok( resync_hook_calls == 1, "clean execution gate caused %ld resyncs\n", resync_hook_calls );

        size = 0x1000;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size,
                                         PAGE_READWRITE, &old_protect );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "nested protection failed %#lx\n", status );
        status = prepare_x64_execution();
        ok( !status, "nested protection resync failed %#lx\n", status );
        ok( resync_hook_calls == 2, "nested protection caused %ld total resyncs\n",
            resync_hook_calls );

        size = 0x1000;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtAllocateVirtualMemoryEx( NtCurrentProcess(), &addr_ex, &size,
                                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE,
                                            NULL, 0 );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "nested extended allocation failed %#lx\n", status );
        status = prepare_x64_execution();
        ok( !status, "nested extended allocation resync failed %#lx\n", status );
        ok( resync_hook_calls == 3, "nested extended allocation caused %ld total resyncs\n",
            resync_hook_calls );

        section_size.QuadPart = 0x10000;
        status = NtCreateSection( &section, SECTION_ALL_ACCESS, NULL, &section_size,
                                  PAGE_READWRITE, SEC_COMMIT, 0 );
        ok( !status, "NtCreateSection failed %#lx\n", status );
        if (!status)
        {
            size = 0;
            offset.QuadPart = 0;
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
            status = NtMapViewOfSection( section, NtCurrentProcess(), &map, 0, 0,
                                         &offset, &size, ViewShare, 0, PAGE_READWRITE );
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
            ok( !status, "nested map failed %#lx\n", status );
            status = prepare_x64_execution();
            ok( !status, "nested map resync failed %#lx\n", status );
            ok( resync_hook_calls == 4, "nested map caused %ld total resyncs\n",
                resync_hook_calls );

            size = 0;
            offset.QuadPart = 0;
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
            status = NtMapViewOfSectionEx( section, NtCurrentProcess(), &map_ex, &offset,
                                           &size, 0, PAGE_READWRITE, NULL, 0 );
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
            ok( !status, "nested extended map failed %#lx\n", status );
            status = prepare_x64_execution();
            ok( !status, "nested extended map resync failed %#lx\n", status );
            ok( resync_hook_calls == 5, "nested extended map caused %ld total resyncs\n",
                resync_hook_calls );

            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
            status = NtUnmapViewOfSection( NtCurrentProcess(), map );
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
            ok( !status, "nested unmap failed %#lx\n", status );
            if (!status) map = NULL;
            status = prepare_x64_execution();
            ok( !status, "nested unmap resync failed %#lx\n", status );
            ok( resync_hook_calls == 6, "nested unmap caused %ld total resyncs\n",
                resync_hook_calls );

            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
            status = NtUnmapViewOfSectionEx( NtCurrentProcess(), map_ex, 0 );
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
            ok( !status, "nested extended unmap failed %#lx\n", status );
            if (!status) map_ex = NULL;
            status = prepare_x64_execution();
            ok( !status, "nested extended unmap resync failed %#lx\n", status );
            ok( resync_hook_calls == 7, "nested extended unmap caused %ld total resyncs\n",
                resync_hook_calls );
        }

        size = 0;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &addr_ex, &size, MEM_RELEASE );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "nested extended allocation free failed %#lx\n", status );
        if (!status) addr_ex = NULL;
        status = prepare_x64_execution();
        ok( !status, "nested extended allocation free resync failed %#lx\n", status );
        ok( resync_hook_calls == 8, "nested extended allocation free caused %ld total resyncs\n",
            resync_hook_calls );

        resync_hook_status = STATUS_NO_MEMORY;
        size = 0x1000;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size,
                                         PAGE_READWRITE, &old_protect );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "nested failure-propagation protection failed %#lx\n", status );
        status = prepare_x64_execution();
        ok( status == STATUS_NO_MEMORY, "failed resync returned %#lx\n", status );
        ok( resync_hook_calls == 9, "failed resync caused %ld total calls\n", resync_hook_calls );
        resync_hook_status = STATUS_SUCCESS;
        status = prepare_x64_execution();
        ok( !status, "pending resync retry failed %#lx\n", status );
        ok( resync_hook_calls == 10, "pending resync retry caused %ld total calls\n",
            resync_hook_calls );

        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;

        failure_addr = NULL;
        size = 0;
        failure_status[0] = NtAllocateVirtualMemory( NtCurrentProcess(), &failure_addr,
                                                     0, &size, MEM_RESERVE, PAGE_READWRITE );

        failure_addr = NULL;
        size = 0;
        failure_status[1] = NtAllocateVirtualMemoryEx( NtCurrentProcess(), &failure_addr,
                                                       &size, MEM_RESERVE, PAGE_READWRITE,
                                                       NULL, 0 );

        failure_addr = (void *)0x1234;
        size = 0;
        failure_status[2] = NtFreeVirtualMemory( NtCurrentProcess(), &failure_addr,
                                                 &size, MEM_RELEASE );

        failure_addr = (void *)0x1234;
        size = 0x1000;
        failure_status[3] = NtProtectVirtualMemory( NtCurrentProcess(), &failure_addr,
                                                    &size, PAGE_READONLY, &old_protect );

        failure_addr = NULL;
        size = 0;
        offset.QuadPart = 0;
        failure_status[4] = NtMapViewOfSection( (HANDLE)0xdead, NtCurrentProcess(),
                                                &failure_addr, 0, 0, &offset, &size,
                                                ViewShare, 0, PAGE_READWRITE );

        failure_addr = NULL;
        size = 0;
        offset.QuadPart = 0;
        failure_status[5] = NtMapViewOfSectionEx( (HANDLE)0xdead, NtCurrentProcess(),
                                                  &failure_addr, &offset, &size, 0,
                                                  PAGE_READWRITE, NULL, 0 );

        failure_status[6] = NtUnmapViewOfSection( NtCurrentProcess(), (void *)0x1234 );
        failure_status[7] = NtUnmapViewOfSectionEx( NtCurrentProcess(), (void *)0x1234, 0 );

        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !NT_SUCCESS(failure_status[0]), "zero-size allocation returned %#lx\n",
            failure_status[0] );
        ok( !NT_SUCCESS(failure_status[1]), "zero-size extended allocation returned %#lx\n",
            failure_status[1] );
        ok( !NT_SUCCESS(failure_status[2]), "invalid free returned %#lx\n", failure_status[2] );
        ok( !NT_SUCCESS(failure_status[3]), "invalid protection returned %#lx\n",
            failure_status[3] );
        ok( !NT_SUCCESS(failure_status[4]), "invalid map returned %#lx\n", failure_status[4] );
        ok( !NT_SUCCESS(failure_status[5]), "invalid extended map returned %#lx\n",
            failure_status[5] );
        ok( !NT_SUCCESS(failure_status[6]), "invalid unmap returned %#lx\n",
            failure_status[6] );
        ok( !NT_SUCCESS(failure_status[7]), "invalid extended unmap returned %#lx\n",
            failure_status[7] );
        status = prepare_x64_execution();
        ok( !status, "gate after failed mutations returned %#lx\n", status );
        ok( resync_hook_calls == 10, "failed mutations caused %ld total resyncs\n",
            resync_hook_calls );

        resync_hook_mode = RESYNC_HOOK_MUTATE_ALWAYS;
        resync_hook_calls = resync_hook_callback_state = 0;
        resync_hook_mutation_status = STATUS_SUCCESS;
        size = 0x1000;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size,
                                         PAGE_READONLY, &old_protect );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "bounded-retry protection failed %#lx\n", status );
        status = prepare_x64_execution();
        ok( status == STATUS_RETRY, "bounded resync exhaustion returned %#lx\n", status );
        ok( resync_hook_calls == 8, "bounded resync exhaustion made %ld calls\n",
            resync_hook_calls );
        ok( !resync_hook_mutation_status, "bounded resync mutation failed %#lx\n",
            resync_hook_mutation_status );
        resync_hook_mode = RESYNC_HOOK_RETURN;
        status = prepare_x64_execution();
        ok( !status, "bounded resync retry failed %#lx\n", status );
        ok( resync_hook_calls == 9, "bounded resync retry made %ld calls\n",
            resync_hook_calls );

        resync_hook_mode = RESYNC_HOOK_MUTATE;
        resync_hook_calls = resync_hook_callback_state = 0;
        resync_hook_mutation_status = STATUS_SUCCESS;
        resync_hook_mutation_addr = NULL;
        size = 0x1000;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size,
                                         PAGE_READONLY, &old_protect );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "nested reentrant-resync protection failed %#lx\n", status );
        status = prepare_x64_execution();
        ok( !status, "self-mutating resync returned %#lx\n", status );
        ok( resync_hook_calls == 2, "self-mutating resync made %ld calls\n", resync_hook_calls );
        ok( resync_hook_callback_state == resync_hook_calls,
            "%ld/%ld resync hooks observed callback state\n",
            resync_hook_callback_state, resync_hook_calls );
        ok( !resync_hook_mutation_status, "hook mutation failed %#lx\n",
            resync_hook_mutation_status );
        ok( !!resync_hook_mutation_addr, "hook mutation did not retain allocation %p\n",
            resync_hook_mutation_addr );
        resync_hook_mode = RESYNC_HOOK_RETURN;
        if (resync_hook_mutation_addr)
        {
            failure_addr = resync_hook_mutation_addr;
            size = 0;
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
            status = NtFreeVirtualMemory( NtCurrentProcess(), &failure_addr, &size,
                                          MEM_RELEASE );
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
            ok( !status, "nested hook allocation cleanup failed %#lx\n", status );
            if (!status) resync_hook_mutation_addr = NULL;
            status = prepare_x64_execution();
            ok( !status, "hook allocation cleanup resync failed %#lx\n", status );
            ok( resync_hook_calls == 3, "hook allocation cleanup made %ld resync calls\n",
                resync_hook_calls );
        }

        resync_hook_entered_event = CreateEventW( NULL, TRUE, FALSE, NULL );
        resync_hook_release_event = CreateEventW( NULL, TRUE, FALSE, NULL );
        thread_params.ready_event = CreateEventW( NULL, TRUE, FALSE, NULL );
        thread_params.start_event = CreateEventW( NULL, TRUE, FALSE, NULL );
        thread_params.prepare = prepare_x64_execution;
        ok( !!resync_hook_entered_event && !!resync_hook_release_event &&
            !!thread_params.ready_event && !!thread_params.start_event,
            "failed to create resync gate events, error %lu\n", GetLastError() );
        if (resync_hook_entered_event && resync_hook_release_event &&
            thread_params.ready_event && thread_params.start_event)
            thread = CreateThread( NULL, 0, resync_gate_thread, &thread_params, 0, NULL );
        ok( !!thread, "CreateThread failed %lu\n", GetLastError() );
        if (thread)
        {
            wait = WaitForSingleObject( thread_params.ready_event, 10000 );
            ok( !wait, "gate thread did not become ready, wait %lu\n", wait );
            status = prepare_x64_execution();
            ok( !status, "gate thread setup resync failed %#lx\n", status );
            resync_hook_calls = resync_hook_callback_state = 0;
            resync_hook_mutation_status = STATUS_SUCCESS;
            resync_hook_mode = RESYNC_HOOK_BLOCK_FIRST;
            ResetEvent( resync_hook_entered_event );
            ResetEvent( resync_hook_release_event );
            size = 0x1000;
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
            status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size,
                                             PAGE_READWRITE, &old_protect );
            NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
            ok( !status, "nested concurrent protection failed %#lx\n", status );
            SetEvent( thread_params.start_event );
            wait = WaitForSingleObject( resync_hook_entered_event, 10000 );
            ok( !wait, "resync hook did not block, wait %lu\n", wait );
            if (!wait)
            {
                size = 0x1000;
                NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
                status = NtProtectVirtualMemory( NtCurrentProcess(), &addr, &size,
                                                 PAGE_READONLY, &old_protect );
                NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
                ok( !status, "mutation during resync failed %#lx\n", status );
            }
            SetEvent( resync_hook_release_event );
            wait = WaitForSingleObject( thread, 10000 );
            ok( !wait, "gate thread wait returned %lu\n", wait );
            GetExitCodeThread( thread, &exit_code );
            ok( !exit_code, "gate thread returned %#lx\n", exit_code );
            ok( resync_hook_calls == 2, "concurrent mutation caused %ld resync calls\n",
                resync_hook_calls );
            ok( resync_hook_callback_state == resync_hook_calls,
                "%ld/%ld concurrent hooks observed callback state\n",
                resync_hook_callback_state, resync_hook_calls );
            ok( !resync_hook_mutation_status, "blocking resync hook failed %#lx\n",
                resync_hook_mutation_status );
        }
        resync_hook_mode = RESYNC_HOOK_RETURN;

        if (addr)
        {
            resync_hook_calls = resync_hook_callback_state = 0;
            reset_addr = addr;
            size = 0x1000;
            status = NtAllocateVirtualMemory( NtCurrentProcess(), &reset_addr, 0, &size,
                                              MEM_RESET, PAGE_READONLY );
            ok( !status, "ordinary MEM_RESET failed %#lx\n", status );
            ok( resync_hook_calls == 1, "ordinary MEM_RESET caused %ld resyncs\n",
                resync_hook_calls );
            status = prepare_x64_execution();
            ok( !status, "clean gate after MEM_RESET failed %#lx\n", status );
            ok( resync_hook_calls == 1, "clean gate after MEM_RESET caused %ld resyncs\n",
                resync_hook_calls );
        }

        if (section)
        {
            resync_hook_calls = resync_hook_callback_state = 0;
            size = 0;
            offset.QuadPart = 0;
            status = NtMapViewOfSection( section, NtCurrentProcess(), &map, 0, 0,
                                         &offset, &size, ViewShare, 0, PAGE_READWRITE );
            ok( !status, "ordinary map failed %#lx\n", status );
            ok( resync_hook_calls == 1, "ordinary map caused %ld resyncs\n",
                resync_hook_calls );
            status = prepare_x64_execution();
            ok( !status, "clean gate after ordinary map failed %#lx\n", status );
            ok( resync_hook_calls == 1, "clean gate after ordinary map caused %ld resyncs\n",
                resync_hook_calls );
            if (map)
            {
                NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
                status = NtUnmapViewOfSection( NtCurrentProcess(), map );
                NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
                ok( !status, "ordinary map cleanup failed %#lx\n", status );
                if (!status) map = NULL;
                status = prepare_x64_execution();
                ok( !status, "ordinary map cleanup resync failed %#lx\n", status );
            }

            resync_hook_calls = resync_hook_callback_state = 0;
            size = 0;
            offset.QuadPart = 0;
            status = NtMapViewOfSectionEx( section, NtCurrentProcess(), &map_ex, &offset,
                                           &size, 0, PAGE_READWRITE, NULL, 0 );
            ok( !status, "ordinary extended map failed %#lx\n", status );
            ok( resync_hook_calls == 1, "ordinary extended map caused %ld resyncs\n",
                resync_hook_calls );
            status = prepare_x64_execution();
            ok( !status, "clean gate after ordinary extended map failed %#lx\n", status );
            ok( resync_hook_calls == 1,
                "clean gate after ordinary extended map caused %ld resyncs\n",
                resync_hook_calls );
            if (map_ex)
            {
                NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
                status = NtUnmapViewOfSectionEx( NtCurrentProcess(), map_ex, 0 );
                NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
                ok( !status, "ordinary extended map cleanup failed %#lx\n", status );
                if (!status) map_ex = NULL;
                status = prepare_x64_execution();
                ok( !status, "ordinary extended map cleanup resync failed %#lx\n", status );
            }
        }

        size = 0;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "nested allocation cleanup failed %#lx\n", status );
        if (!status) addr = NULL;
        status = prepare_x64_execution();
        ok( !status, "cleanup resync failed %#lx\n", status );

        ret = WriteProcessMemory( GetCurrentProcess(), hook, old_code, sizeof(old_code), NULL );
        ok( ret, "restoring resync hook failed, error %lu\n", GetLastError() );
        size = 0x1000;
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        if (!status)
        {
            size = 0;
            status = NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
        }
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "authoritative resync trigger failed %#lx\n", status );
        status = prepare_x64_execution();
        ok( !status, "authoritative provider resync failed %#lx\n", status );

        if (thread) CloseHandle( thread );
        if (thread_params.ready_event) CloseHandle( thread_params.ready_event );
        if (thread_params.start_event) CloseHandle( thread_params.start_event );
        if (resync_hook_entered_event) CloseHandle( resync_hook_entered_event );
        if (resync_hook_release_event) CloseHandle( resync_hook_release_event );
        if (map) NtUnmapViewOfSection( NtCurrentProcess(), map );
        if (map_ex) NtUnmapViewOfSection( NtCurrentProcess(), map_ex );
        if (addr_ex)
        {
            size = 0;
            NtFreeVirtualMemory( NtCurrentProcess(), &addr_ex, &size, MEM_RELEASE );
        }
        if (addr)
        {
            size = 0;
            NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
        }
        if (resync_hook_mutation_addr)
        {
            size = 0;
            NtFreeVirtualMemory( NtCurrentProcess(), &resync_hook_mutation_addr,
                                 &size, MEM_RELEASE );
        }
        if (section) NtClose( section );
    }

    *(void **)&hook_code[2] = resync_test_hook;
    if (module && (hook = hook_notification_function( module,
                       "ResyncIdentityMemoryMappingsStatus",
                       "ResyncIdentityMemoryMappingsStatus" )))
    {
        resync_hook_mode = RESYNC_HOOK_RETURN;
        resync_hook_status = STATUS_SUCCESS;
        resync_hook_calls = resync_hook_callback_state = 0;

        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtFlushInstructionCache( NtCurrentProcess(), hook, 0x10 );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !status, "nested instruction-cache flush failed %#lx\n", status );

        status = prepare_x64_execution();
        ok( !status, "deferred cache flush failed %#lx\n", status );
        ok( resync_hook_calls == 1, "deferred cache flush made %ld resync calls\n",
            resync_hook_calls );
        ok( resync_hook_callback_state == resync_hook_calls,
            "%ld/%ld cache resync hooks observed callback state\n",
            resync_hook_callback_state, resync_hook_calls );

        status = prepare_x64_execution();
        ok( !status, "clean cache gate failed %#lx\n", status );
        ok( resync_hook_calls == 1, "clean cache gate made %ld resync calls\n",
            resync_hook_calls );

        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        status = NtFlushInstructionCache( NtCurrentProcess(), (void *)1, 1 );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
        ok( !NT_SUCCESS(status), "invalid nested cache flush returned %#lx\n", status );
        status = prepare_x64_execution();
        ok( !status, "gate after failed cache flush returned %#lx\n", status );
        ok( resync_hook_calls == 1, "failed cache flush made %ld resync calls\n",
            resync_hook_calls );

        ret = WriteProcessMemory( NtCurrentProcess(), hook, old_code, sizeof(old_code), NULL );
        ok( ret, "restoring cache resync hook failed, error %lu\n", GetLastError() );
    }

    status = get_x64_syscall_dispatcher( NULL, &count );
    ok( status == STATUS_INVALID_PARAMETER, "NULL dispatcher returned %#lx\n", status );
    status = get_x64_syscall_dispatcher( &dispatcher, NULL );
    ok( status == STATUS_INVALID_PARAMETER, "NULL count returned %#lx\n", status );
    status = get_x64_syscall_dispatcher( &dispatcher, &count );
    ok( !status, "dispatcher query returned %#lx\n", status );
    ok( !!dispatcher, "dispatcher query returned NULL\n" );
    ok( !!count, "dispatcher query returned zero syscall count\n" );
}
#endif

#ifdef __arm64ec__
static DWORD CALLBACK simulation_thread( void *arg )
{
    BYTE code[] =
    {
        0x48, 0xc7, 0xc1, 0x34, 0x12, 0x00, 0x00, /* mov $0x1234,%rcx */
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,       /* movabs $RtlExitUserThread,%rax */
        0xff, 0xd0,                               /* call *%rax */
        0xc3,                                     /* ret */
    };
    DWORD old_prot;
    CONTEXT *context;
    void (WINAPI *pBeginSimulation)(void) = arg;
    void *addr = VirtualAlloc( NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE );

    *(void **)(code + 9) = GetProcAddress( GetModuleHandleA("ntdll.dll"), "RtlExitUserThread" );
    memcpy( addr, code, sizeof(code) );
    VirtualProtect( addr, 0x1000, PAGE_EXECUTE_READ, &old_prot );

    context = &NtCurrentTeb()->ChpeV2CpuAreaInfo->ContextAmd64->AMD64_Context;
    context->Rsp = (ULONG_PTR)&context - 0x800;
    context->Rip = (ULONG_PTR)addr;

    NtCurrentTeb()->ChpeV2CpuAreaInfo->InSimulation = 1;  /* otherwise it crashes on recent Windows */
    pBeginSimulation();
    return 0x5678;
}
#endif

static void test_xtajit64(void)
{
#ifdef __arm64ec__
    HMODULE module = GetModuleHandleA( "xtajit64.dll" );
    BOOLEAN (WINAPI *pBTCpu64IsProcessorFeaturePresent)( UINT feature );
    void (WINAPI *pUpdateProcessorInformation)( SYSTEM_CPU_INFORMATION *info );
    void (WINAPI *pBeginSimulation)(void);
    UINT i;

    if (!module)
    {
        win_skip( "xtaji64.dll not loaded\n" );
        return;
    }
#define GET_PROC(func) p##func = pRtlFindExportedRoutineByName( module, #func )
    GET_PROC( BTCpu64IsProcessorFeaturePresent );
    GET_PROC( BeginSimulation );
    GET_PROC( UpdateProcessorInformation );
#undef GET_PROC

    if (pBTCpu64IsProcessorFeaturePresent)
    {
        static const ULONGLONG expect_features =
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

        for (i = 0; i < 64; i++)
        {
            BOOLEAN ret = pBTCpu64IsProcessorFeaturePresent( i );
            if (expect_features & (1ull << i)) ok( ret, "missing feature %u\n", i );
            else if (ret) trace( "extra feature %u supported\n", i );
        }
    }
    else win_skip( "BTCpu64IsProcessorFeaturePresent missing\n" );

    if (pUpdateProcessorInformation)
    {
        SYSTEM_CPU_INFORMATION info;

        memset( &info, 0xcc, sizeof(info) );
        info.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
        pUpdateProcessorInformation( &info );

        ok( info.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64,
            "wrong architecture %u\n", info.ProcessorArchitecture );
        ok( info.ProcessorLevel == 21, "wrong level %u\n", info.ProcessorLevel );
        ok( info.ProcessorRevision == 1, "wrong revision %u\n", info.ProcessorRevision );
        ok( info.MaximumProcessors == 0xcccc, "wrong max proc %u\n", info.MaximumProcessors );
        ok( info.ProcessorFeatureBits == 0xcccccccc, "wrong features %lx\n", info.ProcessorFeatureBits );
    }
    else win_skip( "UpdateProcessorInformation missing\n" );

    if (pBeginSimulation)
    {
        DWORD ret, exit_code;
        HANDLE thread = CreateThread( NULL, 0, simulation_thread, pBeginSimulation, 0, NULL );

        ok( thread != 0, "thread creation failed\n" );
        ret = WaitForSingleObject( thread, 10000 );
        ok( !ret, "wait failed %lx\n", ret );
        GetExitCodeThread( thread, &exit_code );
        ok( exit_code == 0x1234, "wrong exit code %lx\n", exit_code );
        CloseHandle( thread );
    }
    else win_skip( "BeginSimulation missing\n" );
#endif
}


static void test_memory_notifications(void)
{
    HMODULE module;
    CHPEV2_PROCESS_INFO *info;

    if (current_machine == IMAGE_FILE_MACHINE_ARM64) return;
    if (!(module = GetModuleHandleA( "xtajit64.dll" ))) return;
    info = NtCurrentTeb()->Peb->ChpeV2ProcessInfo;
    if (info->NativeMachineType == native_machine &&
        info->EmulatedMachineType == IMAGE_FILE_MACHINE_AMD64)
    {
        test_notifications( module, (CROSS_PROCESS_WORK_LIST *)info->CrossProcessWorkList );

        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback++;
        test_notifications( module, (CROSS_PROCESS_WORK_LIST *)info->CrossProcessWorkList );
        NtCurrentTeb()->ChpeV2CpuAreaInfo->InSyscallCallback--;
    }
    skip( "arm64ec shared info not found\n" );
}


#else  /* _WIN64 */

static const BYTE call_func64_code[] =
{
    0x58,                               /* pop %eax */
    0x0e,                               /* push %cs */
    0x50,                               /* push %eax */
    0x6a, 0x33,                         /* push $0x33 */
    0xe8, 0x00, 0x00, 0x00, 0x00,       /* call 1f */
    0x83, 0x04, 0x24, 0x05,             /* 1: addl $0x5,(%esp) */
    0xcb,                               /* lret */
    /* in 64-bit mode: */
    0x4c, 0x87, 0xf4,                   /* xchg %r14,%rsp */
    0x55,                               /* push %rbp */
    0x48, 0x89, 0xe5,                   /* mov %rsp,%rbp */
    0x56,                               /* push %rsi */
    0x57,                               /* push %rdi */
    0x41, 0x8b, 0x4e, 0x10,             /* mov 0x10(%r14),%ecx */
    0x41, 0x8b, 0x76, 0x14,             /* mov 0x14(%r14),%esi */
    0x67, 0x8d, 0x04, 0xcd, 0, 0, 0, 0, /* lea 0x0(,%ecx,8),%eax */
    0x83, 0xf8, 0x20,                   /* cmp $0x20,%eax */
    0x7d, 0x05,                         /* jge 1f */
    0xb8, 0x20, 0x00, 0x00, 0x00,       /* mov $0x20,%eax */
    0x48, 0x29, 0xc4,                   /* 1: sub %rax,%rsp */
    0x48, 0x83, 0xe4, 0xf0,             /* and $~15,%rsp */
    0x48, 0x89, 0xe7,                   /* mov %rsp,%rdi */
    0xf3, 0x48, 0xa5,                   /* rep movsq */
    0x48, 0x8b, 0x0c, 0x24,             /* mov (%rsp),%rcx */
    0x48, 0x8b, 0x54, 0x24, 0x08,       /* mov 0x8(%rsp),%rdx */
    0x4c, 0x8b, 0x44, 0x24, 0x10,       /* mov 0x10(%rsp),%r8 */
    0x4c, 0x8b, 0x4c, 0x24, 0x18,       /* mov 0x18(%rsp),%r9 */
    0x41, 0xff, 0x56, 0x08,             /* callq *0x8(%r14) */
    0x48, 0x8d, 0x65, 0xf0,             /* lea -0x10(%rbp),%rsp */
    0x5f,                               /* pop %rdi */
    0x5e,                               /* pop %rsi */
    0x5d,                               /* pop %rbp */
    0x4c, 0x87, 0xf4,                   /* xchg %r14,%rsp */
    0xcb,                               /* lret */
};

static NTSTATUS call_func64( ULONG64 func64, int nb_args, ULONG64 *args )
{
    NTSTATUS (WINAPI *func)( ULONG64 func64, int nb_args, ULONG64 *args ) = code_mem;

    memcpy( code_mem, call_func64_code, sizeof(call_func64_code) );
    return func( func64, nb_args, args );
}

static struct
{
    ULONG64 main, ntdll, wow64, xtajit, wow64win, wow64base, wow64con, wow64cpu;
} modules;

static void enum_modules64( void (*func)(ULONG64,const WCHAR *) )
{
    typedef struct
    {
        LIST_ENTRY64     InLoadOrderLinks;
        LIST_ENTRY64     InMemoryOrderLinks;
        LIST_ENTRY64     InInitializationOrderLinks;
        ULONG64          DllBase;
        ULONG64          EntryPoint;
        ULONG            SizeOfImage;
        UNICODE_STRING64 FullDllName;
        UNICODE_STRING64 BaseDllName;
        /* etc. */
    } LDR_DATA_TABLE_ENTRY64;

    TEB64 *teb64 = (TEB64 *)NtCurrentTeb()->GdiBatchCount;
    PEB64 peb64;
    ULONG64 ptr;
    PEB_LDR_DATA64 ldr;
    LDR_DATA_TABLE_ENTRY64 entry;
    NTSTATUS status;
    HANDLE process;

    process = OpenProcess( PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId() );
    ok( process != 0, "failed to open current process %lu\n", GetLastError() );
    status = pNtWow64ReadVirtualMemory64( process, teb64->Peb, &peb64, sizeof(peb64), NULL );
    ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
    todo_wine_if( old_wow64 )
    ok( peb64.LdrData, "LdrData not initialized\n" );
    if (!peb64.LdrData) goto done;
    status = pNtWow64ReadVirtualMemory64( process, peb64.LdrData, &ldr, sizeof(ldr), NULL );
    ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
    ptr = ldr.InLoadOrderModuleList.Flink;
    for (;;)
    {
        WCHAR buffer[256];
        status = pNtWow64ReadVirtualMemory64( process, ptr, &entry, sizeof(entry), NULL );
        ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
        status = pNtWow64ReadVirtualMemory64( process, entry.BaseDllName.Buffer, buffer, sizeof(buffer), NULL );
        ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
        if (status) break;
        func( entry.DllBase, buffer );
        ptr = entry.InLoadOrderLinks.Flink;
        if (ptr == peb64.LdrData + offsetof( PEB_LDR_DATA64, InLoadOrderModuleList )) break;
    }
done:
    NtClose( process );
}

static ULONG64 get_proc_address64( ULONG64 module, const char *name )
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_EXPORT_DIRECTORY exports;
    ULONG i, *names, *funcs;
    USHORT *ordinals;
    NTSTATUS status;
    HANDLE process;
    ULONG64 ret = 0;
    char buffer[64];

    if (!module) return 0;
    process = OpenProcess( PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId() );
    ok( process != 0, "failed to open current process %lu\n", GetLastError() );
    status = pNtWow64ReadVirtualMemory64( process, module, &dos, sizeof(dos), NULL );
    ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
    status = pNtWow64ReadVirtualMemory64( process, module + dos.e_lfanew, &nt, sizeof(nt), NULL );
    ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
    status = pNtWow64ReadVirtualMemory64( process, module + nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress,
                                          &exports, sizeof(exports), NULL );
    ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
    names = calloc( exports.NumberOfNames, sizeof(*names) );
    ordinals = calloc( exports.NumberOfNames, sizeof(*ordinals) );
    funcs = calloc( exports.NumberOfFunctions, sizeof(*funcs) );
    status = pNtWow64ReadVirtualMemory64( process, module + exports.AddressOfNames,
                                          names, exports.NumberOfNames * sizeof(*names), NULL );
    ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
    status = pNtWow64ReadVirtualMemory64( process, module + exports.AddressOfNameOrdinals,
                                          ordinals, exports.NumberOfNames * sizeof(*ordinals), NULL );
    ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
    status = pNtWow64ReadVirtualMemory64( process, module + exports.AddressOfFunctions,
                                          funcs, exports.NumberOfFunctions * sizeof(*funcs), NULL );
    ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
    for (i = 0; i < exports.NumberOfNames && !ret; i++)
    {
        status = pNtWow64ReadVirtualMemory64( process, module + names[i], buffer, sizeof(buffer), NULL );
        ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
        if (!strcmp( buffer, name )) ret = module + funcs[ordinals[i]];
    }
    free( funcs );
    free( ordinals );
    free( names );
    NtClose( process );
    return ret;
}

static void check_module( ULONG64 base, const WCHAR *name )
{
    if (base == (ULONG_PTR)GetModuleHandleW(0))
    {
        WCHAR *p, module[MAX_PATH];

        GetModuleFileNameW( 0, module, MAX_PATH );
        if ((p = wcsrchr( module, '\\' ))) p++;
        else p = module;
        ok( !wcsicmp( name, p ), "wrong name %s / %s\n", debugstr_w(name), debugstr_w(module));
        modules.main = base;
        return;
    }
#define CHECK_MODULE(mod) do { if (!wcsicmp( name, L"" #mod ".dll" )) { modules.mod = base; return; } } while(0)
    CHECK_MODULE(ntdll);
    CHECK_MODULE(wow64);
    CHECK_MODULE(wow64base);
    CHECK_MODULE(wow64con);
    CHECK_MODULE(wow64win);
    if (native_machine == IMAGE_FILE_MACHINE_ARM64)
        CHECK_MODULE(xtajit);
    else
        CHECK_MODULE(wow64cpu);
#undef CHECK_MODULE
    todo_wine_if( !wcscmp( name, L"win32u.dll" ))
    ok( 0, "unknown module %s %s found\n", wine_dbgstr_longlong(base), wine_dbgstr_w(name));
}

static void test_modules(void)
{
    if (!is_wow64) return;
    if (!pNtWow64ReadVirtualMemory64) return;
    enum_modules64( check_module );
    todo_wine_if( old_wow64 )
    {
    ok( modules.main, "main module not found\n" );
    ok( modules.ntdll, "64-bit ntdll not found\n" );
    ok( modules.wow64, "wow64.dll not found\n" );
    if (native_machine == IMAGE_FILE_MACHINE_ARM64)
        ok( modules.xtajit, "xtajit.dll not found\n" );
    else
        ok( modules.wow64cpu, "wow64cpu.dll not found\n" );
    ok( modules.wow64win, "wow64win.dll not found\n" );
    }
}

static void test_nt_wow64(void)
{
    const char str[] = "hello wow64";
    char buffer[100];
    NTSTATUS status;
    ULONG64 res;
    HANDLE process = OpenProcess( PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId() );

    ok( process != 0, "failed to open current process %lu\n", GetLastError() );
    if (pNtWow64ReadVirtualMemory64)
    {
        status = pNtWow64ReadVirtualMemory64( process, (ULONG_PTR)str, buffer, sizeof(str), &res );
        ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
        ok( res == sizeof(str), "wrong size %s\n", wine_dbgstr_longlong(res) );
        ok( !strcmp( buffer, str ), "wrong data %s\n", debugstr_a(buffer) );
        status = pNtWow64WriteVirtualMemory64( process, (ULONG_PTR)buffer, " bye ", 5, &res );
        ok( !status, "NtWow64WriteVirtualMemory64 failed %lx\n", status );
        ok( res == 5, "wrong size %s\n", wine_dbgstr_longlong(res) );
        ok( !strcmp( buffer, " bye  wow64" ), "wrong data %s\n", debugstr_a(buffer) );
        /* current process pseudo-handle is broken on some Windows versions */
        status = pNtWow64ReadVirtualMemory64( GetCurrentProcess(), (ULONG_PTR)str, buffer, sizeof(str), &res );
        ok( !status || broken( status == STATUS_INVALID_HANDLE ),
            "NtWow64ReadVirtualMemory64 failed %lx\n", status );
        status = pNtWow64WriteVirtualMemory64( GetCurrentProcess(), (ULONG_PTR)buffer, " bye ", 5, &res );
        ok( !status || broken( status == STATUS_INVALID_HANDLE ),
            "NtWow64WriteVirtualMemory64 failed %lx\n", status );
    }
    else win_skip( "NtWow64ReadVirtualMemory64 not supported\n" );

    if (pNtWow64AllocateVirtualMemory64)
    {
        ULONG64 ptr = 0;
        ULONG64 size = 0x2345;

        status = pNtWow64AllocateVirtualMemory64( process, &ptr, 0, &size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        ok( !status, "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
        ok( ptr, "ptr not set\n" );
        ok( size == 0x3000, "size not set %s\n", wine_dbgstr_longlong(size) );
        ptr += 0x1000;
        status = pNtWow64AllocateVirtualMemory64( process, &ptr, 0, &size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READONLY );
        ok( status == STATUS_CONFLICTING_ADDRESSES, "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
        ptr = 0;
        size = 0;
        status = pNtWow64AllocateVirtualMemory64( process, &ptr, 0, &size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        ok( status == STATUS_INVALID_PARAMETER || status == STATUS_INVALID_PARAMETER_4,
            "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
        size = 0x1000;
        status = pNtWow64AllocateVirtualMemory64( process, &ptr, 22, &size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        ok( status == STATUS_INVALID_PARAMETER || status == STATUS_INVALID_PARAMETER_3,
            "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
        status = pNtWow64AllocateVirtualMemory64( process, &ptr, 33, &size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        ok( status == STATUS_INVALID_PARAMETER || status == STATUS_INVALID_PARAMETER_3,
            "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
        status = pNtWow64AllocateVirtualMemory64( process, &ptr, 0x3fffffff, &size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        todo_wine_if( !is_wow64 )
        ok( !status, "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
        ok( ptr < 0x40000000, "got wrong ptr %s\n", wine_dbgstr_longlong(ptr) );
        if (!status && pNtWow64WriteVirtualMemory64)
        {
            status = pNtWow64WriteVirtualMemory64( process, ptr, str, sizeof(str), &res );
            ok( !status, "NtWow64WriteVirtualMemory64 failed %lx\n", status );
            ok( res == sizeof(str), "wrong size %s\n", wine_dbgstr_longlong(res) );
            ok( !strcmp( (char *)(ULONG_PTR)ptr, str ), "wrong data %s\n",
                debugstr_a((char *)(ULONG_PTR)ptr) );
            ptr = 0;
            status = pNtWow64AllocateVirtualMemory64( process, &ptr, 0, &size,
                                                      MEM_RESERVE | MEM_COMMIT, PAGE_READONLY );
            ok( !status, "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
            status = pNtWow64WriteVirtualMemory64( process, ptr, str, sizeof(str), &res );
            todo_wine_if(status == STATUS_SUCCESS)
            ok( status == STATUS_PARTIAL_COPY || broken( status == STATUS_ACCESS_VIOLATION ),
                "NtWow64WriteVirtualMemory64 failed %lx\n", status );
            todo_wine_if(status == STATUS_SUCCESS)
            ok( !res || broken(res) /* win10 1709 */, "wrong size %s\n", wine_dbgstr_longlong(res) );
        }
        ptr = 0x9876543210ull;
        status = pNtWow64AllocateVirtualMemory64( process, &ptr, 0, &size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READONLY );
        todo_wine_if( !is_wow64 || old_wow64 )
        ok( !status || broken( status == STATUS_CONFLICTING_ADDRESSES ),
            "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
        if (!status) ok( ptr == 0x9876540000ull || broken(ptr == 0x76540000), /* win 8.1 */
                         "wrong ptr %s\n", wine_dbgstr_longlong(ptr) );
        ptr = 0;
        status = pNtWow64AllocateVirtualMemory64( GetCurrentProcess(), &ptr, 0, &size,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READONLY );
        ok( !status || broken( status == STATUS_INVALID_HANDLE ),
            "NtWow64AllocateVirtualMemory64 failed %lx\n", status );
    }
    else win_skip( "NtWow64AllocateVirtualMemory64 not supported\n" );

    if (pNtWow64GetNativeSystemInformation)
    {
        ULONG i, len;
        SYSTEM_BASIC_INFORMATION sbi, sbi2, sbi3;

        memset( &sbi, 0xcc, sizeof(sbi) );
        status = pNtQuerySystemInformation( SystemBasicInformation, &sbi, sizeof(sbi), &len );
        ok( status == STATUS_SUCCESS, "failed %lx\n", status );
        ok( len == sizeof(sbi), "wrong length %ld\n", len );

        memset( &sbi2, 0xcc, sizeof(sbi2) );
        status = pRtlGetNativeSystemInformation( SystemBasicInformation, &sbi2, sizeof(sbi2), &len );
        ok( status == STATUS_SUCCESS, "failed %lx\n", status );
        ok( len == sizeof(sbi2), "wrong length %ld\n", len );

        ok( sbi.HighestUserAddress == (void *)0x7ffeffff, "wrong limit %p\n", sbi.HighestUserAddress);
        todo_wine_if( old_wow64 )
        ok( sbi2.HighestUserAddress == (is_wow64 ? (void *)0xfffeffff : (void *)0x7ffeffff),
            "wrong limit %p\n", sbi.HighestUserAddress);

        memset( &sbi3, 0xcc, sizeof(sbi3) );
        status = pNtWow64GetNativeSystemInformation( SystemBasicInformation, &sbi3, sizeof(sbi3), &len );
        ok( status == STATUS_SUCCESS, "failed %lx\n", status );
        ok( len == sizeof(sbi3), "wrong length %ld\n", len );
        ok( !memcmp( &sbi2, &sbi3, offsetof(SYSTEM_BASIC_INFORMATION,NumberOfProcessors)+1 ),
            "info is different\n" );

        memset( &sbi3, 0xcc, sizeof(sbi3) );
        status = pNtWow64GetNativeSystemInformation( SystemEmulationBasicInformation, &sbi3, sizeof(sbi3), &len );
        ok( status == STATUS_SUCCESS, "failed %lx\n", status );
        ok( len == sizeof(sbi3), "wrong length %ld\n", len );
        ok( !memcmp( &sbi, &sbi3, offsetof(SYSTEM_BASIC_INFORMATION,NumberOfProcessors)+1 ),
            "info is different\n" );

        for (i = 0; i < 256; i++)
        {
            NTSTATUS expect = pNtQuerySystemInformation( i, NULL, 0, &len );
            status = pNtWow64GetNativeSystemInformation( i, NULL, 0, &len );
            switch (i)
            {
            case SystemNativeBasicInformation:
                ok( status == STATUS_INVALID_INFO_CLASS || status == STATUS_INFO_LENGTH_MISMATCH ||
                    broken(status == STATUS_NOT_IMPLEMENTED) /* vista */, "%lu: %lx / %lx\n", i, status, expect );
                break;
            case SystemBasicInformation:
            case SystemCpuInformation:
            case SystemEmulationBasicInformation:
            case SystemEmulationProcessorInformation:
                ok( status == expect, "%lu: %lx / %lx\n", i, status, expect );
                break;
            default:
                if (is_wow64)  /* only a few info classes are supported on Wow64 */
                    ok( status == STATUS_INVALID_INFO_CLASS ||
                        broken(status == STATUS_NOT_IMPLEMENTED), /* vista */
                        "%lu: %lx\n", i, status );
                else
                    ok( status == expect, "%lu: %lx / %lx\n", i, status, expect );
                break;
            }
        }
    }
    else win_skip( "NtWow64GetNativeSystemInformation not supported\n" );

    if (pNtWow64IsProcessorFeaturePresent)
    {
        ULONG i;

        for (i = 0; i < 64; i++)
            ok( pNtWow64IsProcessorFeaturePresent( i ) == IsProcessorFeaturePresent( i ),
                "mismatch %lu wow64 returned %lx\n", i, pNtWow64IsProcessorFeaturePresent( i ));

        if (native_machine == IMAGE_FILE_MACHINE_ARM64)
        {
            KUSER_SHARED_DATA *user_shared_data = ULongToPtr( 0x7ffe0000 );

            ok( user_shared_data->ProcessorFeatures[PF_ARM_V8_INSTRUCTIONS_AVAILABLE], "no ARM_V8\n" );
            ok( user_shared_data->ProcessorFeatures[PF_MMX_INSTRUCTIONS_AVAILABLE], "no MMX\n" );
            ok( !pNtWow64IsProcessorFeaturePresent( PF_ARM_V8_INSTRUCTIONS_AVAILABLE ), "ARM_V8 present\n" );
            ok( pNtWow64IsProcessorFeaturePresent( PF_MMX_INSTRUCTIONS_AVAILABLE ), "MMX not present\n" );
        }
    }
    else win_skip( "NtWow64IsProcessorFeaturePresent not supported\n" );

    if (pNtWow64QueryInformationProcess64)
    {
        PROCESS_BASIC_INFORMATION pbi32;
        PROCESS_BASIC_INFORMATION64 pbi64;
        ULONG expected_peb;
        ULONG class;

        for (class = 0; class <= MaxProcessInfoClass; class++)
        {
            winetest_push_context( "Process information class %lu", class );

            switch (class)
            {
            case ProcessBasicInformation:
                status = NtQueryInformationProcess( GetCurrentProcess(), ProcessBasicInformation, &pbi32, sizeof(pbi32), NULL );
                ok( !status, "NtQueryInformationProcess returned 0x%08lx\n", status );

                status = pNtWow64QueryInformationProcess64( GetCurrentProcess(), ProcessBasicInformation, &pbi64, sizeof(pbi64), NULL );
                ok( !status, "NtWow64QueryInformationProcess64 returned 0x%08lx\n", status );

                expected_peb = (ULONG)pbi32.PebBaseAddress;
                if (is_wow64) expected_peb -= 0x1000;

                ok( pbi64.ExitStatus == pbi32.ExitStatus,
                    "expected %lu got %lu\n", pbi32.ExitStatus, pbi64.ExitStatus );
                ok( pbi64.PebBaseAddress == expected_peb ||
                    /* The 64-bit PEB is usually, but not always, 4096 bytes below the 32-bit PEB */
                    broken( is_wow64 && llabs( (INT64)pbi64.PebBaseAddress - (INT64)expected_peb ) < 0x10000 ),
                    "expected 0x%lx got 0x%I64x\n", expected_peb, pbi64.PebBaseAddress );
                ok( pbi64.AffinityMask == pbi32.AffinityMask,
                    "expected 0x%Ix got 0x%I64x\n", pbi32.AffinityMask, pbi64.AffinityMask );
                ok( pbi64.UniqueProcessId == pbi32.UniqueProcessId,
                    "expected %Ix got %I64x\n", pbi32.UniqueProcessId, pbi64.UniqueProcessId );
                ok( pbi64.InheritedFromUniqueProcessId == pbi32.InheritedFromUniqueProcessId,
                    "expected %Ix got %I64x\n", pbi32.UniqueProcessId, pbi64.UniqueProcessId );
                break;
            default:
                status = pNtWow64QueryInformationProcess64( GetCurrentProcess(), class, NULL, 0, NULL );
                ok( status == STATUS_NOT_IMPLEMENTED, "NtWow64QueryInformationProcess64 returned 0x%08lx\n", status );
            }

            winetest_pop_context();
        }
    }
    else win_skip( "NtWow64QueryInformationProcess64 not supported\n" );

    NtClose( process );
}

static void test_init_block(void)
{
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    ULONG i, size = 0, *init_block;
    ULONG64 ptr64, *block64;
    void *ptr;

    if (!is_wow64) return;
    if ((ptr = GetProcAddress( ntdll, "LdrSystemDllInitBlock" )))
    {
        init_block = ptr;
        trace( "got init block %08lx\n", init_block[0] );
#define CHECK_FUNC(val,func) \
            ok( (val) == (ULONG_PTR)GetProcAddress( ntdll, func ), \
                "got %p for %s %p\n", (void *)(ULONG_PTR)(val), func, GetProcAddress( ntdll, func ))
        switch (init_block[0])
        {
        case 0x44:  /* vistau64 */
            CHECK_FUNC( init_block[1], "LdrInitializeThunk" );
            CHECK_FUNC( init_block[2], "KiUserExceptionDispatcher" );
            CHECK_FUNC( init_block[3], "KiUserApcDispatcher" );
            CHECK_FUNC( init_block[4], "KiUserCallbackDispatcher" );
            CHECK_FUNC( init_block[5], "LdrHotPatchRoutine" );
            CHECK_FUNC( init_block[6], "ExpInterlockedPopEntrySListFault" );
            CHECK_FUNC( init_block[7], "ExpInterlockedPopEntrySListResume" );
            CHECK_FUNC( init_block[8], "ExpInterlockedPopEntrySListEnd" );
            CHECK_FUNC( init_block[9], "RtlUserThreadStart" );
            CHECK_FUNC( init_block[10], "RtlpQueryProcessDebugInformationRemote" );
            CHECK_FUNC( init_block[11], "EtwpNotificationThread" );
            ok( init_block[12] == (ULONG_PTR)ntdll, "got %p for ntdll %p\n",
                (void *)(ULONG_PTR)init_block[12], ntdll );
            size = 13 * sizeof(*init_block);
            break;
        case 0x50:  /* win7 */
            CHECK_FUNC( init_block[4], "LdrInitializeThunk" );
            CHECK_FUNC( init_block[5], "KiUserExceptionDispatcher" );
            CHECK_FUNC( init_block[6], "KiUserApcDispatcher" );
            CHECK_FUNC( init_block[7], "KiUserCallbackDispatcher" );
            CHECK_FUNC( init_block[8], "LdrHotPatchRoutine" );
            CHECK_FUNC( init_block[9], "ExpInterlockedPopEntrySListFault" );
            CHECK_FUNC( init_block[10], "ExpInterlockedPopEntrySListResume" );
            CHECK_FUNC( init_block[11], "ExpInterlockedPopEntrySListEnd" );
            CHECK_FUNC( init_block[12], "RtlUserThreadStart" );
            CHECK_FUNC( init_block[13], "RtlpQueryProcessDebugInformationRemote" );
            CHECK_FUNC( init_block[14], "EtwpNotificationThread" );
            ok( init_block[15] == (ULONG_PTR)ntdll, "got %p for ntdll %p\n",
                (void *)(ULONG_PTR)init_block[15], ntdll );
            /* CHECK_FUNC( init_block[16], "LdrSystemDllInitBlock" ); not always present */
            size = 17 * sizeof(*init_block);
            break;
        case 0x70:  /* win8 */
            CHECK_FUNC( init_block[4], "LdrInitializeThunk" );
            CHECK_FUNC( init_block[5], "KiUserExceptionDispatcher" );
            CHECK_FUNC( init_block[6], "KiUserApcDispatcher" );
            CHECK_FUNC( init_block[7], "KiUserCallbackDispatcher" );
            CHECK_FUNC( init_block[8], "ExpInterlockedPopEntrySListFault" );
            CHECK_FUNC( init_block[9], "ExpInterlockedPopEntrySListResume" );
            CHECK_FUNC( init_block[10], "ExpInterlockedPopEntrySListEnd" );
            CHECK_FUNC( init_block[11], "RtlUserThreadStart" );
            CHECK_FUNC( init_block[12], "RtlpQueryProcessDebugInformationRemote" );
            ok( init_block[13] == (ULONG_PTR)ntdll, "got %p for ntdll %p\n",
                (void *)(ULONG_PTR)init_block[13], ntdll );
            CHECK_FUNC( init_block[14], "LdrSystemDllInitBlock" );
            size = 15 * sizeof(*init_block);
            break;
        case 0x80:  /* win10 1507 */
            CHECK_FUNC( init_block[4], "LdrInitializeThunk" );
            CHECK_FUNC( init_block[5], "KiUserExceptionDispatcher" );
            CHECK_FUNC( init_block[6], "KiUserApcDispatcher" );
            CHECK_FUNC( init_block[7], "KiUserCallbackDispatcher" );
            if (GetProcAddress( ntdll, "ExpInterlockedPopEntrySListFault" ))
            {
                CHECK_FUNC( init_block[8], "ExpInterlockedPopEntrySListFault" );
                CHECK_FUNC( init_block[9], "ExpInterlockedPopEntrySListResume" );
                CHECK_FUNC( init_block[10], "ExpInterlockedPopEntrySListEnd" );
                CHECK_FUNC( init_block[11], "RtlUserThreadStart" );
                CHECK_FUNC( init_block[12], "RtlpQueryProcessDebugInformationRemote" );
                ok( init_block[13] == (ULONG_PTR)ntdll, "got %p for ntdll %p\n",
                    (void *)(ULONG_PTR)init_block[13], ntdll );
                CHECK_FUNC( init_block[14], "LdrSystemDllInitBlock" );
                size = 15 * sizeof(*init_block);
            }
            else  /* win10 1607 */
            {
                CHECK_FUNC( init_block[8], "RtlUserThreadStart" );
                CHECK_FUNC( init_block[9], "RtlpQueryProcessDebugInformationRemote" );
                ok( init_block[10] == (ULONG_PTR)ntdll, "got %p for ntdll %p\n",
                    (void *)(ULONG_PTR)init_block[10], ntdll );
                CHECK_FUNC( init_block[11], "LdrSystemDllInitBlock" );
                size = 12 * sizeof(*init_block);
            }
            break;
        case 0xe0:  /* win10 1809 */
        case 0xf0:  /* win10 2004 */
        case 0x128: /* win11 24h2 */
            block64 = ptr;
            CHECK_FUNC( block64[3], "LdrInitializeThunk" );
            CHECK_FUNC( block64[4], "KiUserExceptionDispatcher" );
            CHECK_FUNC( block64[5], "KiUserApcDispatcher" );
            CHECK_FUNC( block64[6], "KiUserCallbackDispatcher" );
            CHECK_FUNC( block64[7], "RtlUserThreadStart" );
            CHECK_FUNC( block64[8], "RtlpQueryProcessDebugInformationRemote" );
            todo_wine_if( old_wow64 )
            ok( block64[9] == (ULONG_PTR)ntdll, "got %p for ntdll %p\n",
                (void *)(ULONG_PTR)block64[9], ntdll );
            CHECK_FUNC( block64[10], "LdrSystemDllInitBlock" );
            CHECK_FUNC( block64[11], "RtlpFreezeTimeBias" );
            size = 12 * sizeof(*block64);
            break;
        default:
            ok( 0, "unknown init block %08lx\n", init_block[0] );
            for (i = 0; i < init_block[0] / sizeof(ULONG); i++) trace("%04lx: %08lx\n", i, init_block[i]);
            break;
        }
#undef CHECK_FUNC

        if (size && (ptr64 = get_proc_address64( modules.ntdll, "LdrSystemDllInitBlock" )))
        {
            DWORD buffer[64];
            HANDLE process = OpenProcess( PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId() );
            NTSTATUS status = pNtWow64ReadVirtualMemory64( process, ptr64, buffer, size, NULL );
            ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
            ok( !memcmp( buffer, init_block, size ), "wrong 64-bit init block\n" );
            NtClose( process );
        }
    }
    else todo_wine win_skip( "LdrSystemDllInitBlock not supported\n" );
}


static void test_memory_notifications(void)
{
    HMODULE module = (HMODULE)(ULONG_PTR)modules.xtajit;
    WOW64INFO *info;
    DWORD i;

    if (!modules.xtajit)
    {
        skip( "xtajit.dll not loaded\n" );
        return;
    }
    if ((ULONG_PTR)module != modules.xtajit)
    {
        skip( "xtajit.dll loaded above 4G\n" );
        return;
    }

    for (i = 0x400; i < 0x800; i += sizeof(ULONG))
    {
        info = (WOW64INFO *)((char *)NtCurrentTeb()->Peb + i);
        if (info->NativeMachineType == native_machine &&
            info->EmulatedMachineType == IMAGE_FILE_MACHINE_I386)
        {
            if (info->CrossProcessWorkList >> 32)
                skip( "cross-process work list above 4G (%I64x)\n", info->CrossProcessWorkList );
            else
                test_notifications( module, ULongToPtr( info->CrossProcessWorkList ));
            return;
        }
    }
    skip( "WOW64INFO not found\n" );
}


static DWORD WINAPI iosb_delayed_write_thread(void *arg)
{
    HANDLE client = arg;
    DWORD size;
    BOOL ret;

    Sleep(100);

    ret = WriteFile( client, "data", sizeof("data"), &size, NULL );
    ok( ret == TRUE, "got error %lu\n", GetLastError() );

    return 0;
}


static void test_iosb(void)
{
    static const char pipe_name[] = "\\\\.\\pipe\\wow64iosbnamedpipe";
    HANDLE client, server, thread;
    NTSTATUS status;
    ULONG64 read_func, flush_func;
    IO_STATUS_BLOCK iosb32;
    char buffer[6];
    DWORD size;
    BOOL ret;
    struct
    {
        union
        {
            NTSTATUS Status;
            ULONG64 Pointer;
        };
        ULONG64 Information;
    } iosb64;
    ULONG64 args[] = { 0, 0, 0, 0, (ULONG_PTR)&iosb64, (ULONG_PTR)buffer, sizeof(buffer), 0, 0 };
    ULONG64 flush_args[] = { 0, (ULONG_PTR)&iosb64 };

    if (!is_wow64) return;
    if (!code_mem) return;
    if (!modules.ntdll) return;
    read_func = get_proc_address64( modules.ntdll, "NtReadFile" );
    flush_func = get_proc_address64( modules.ntdll, "NtFlushBuffersFile" );

    /* async calls set iosb32 but not iosb64 */

    server = CreateNamedPipeA( pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                               4, 1024, 1024, 1000, NULL );
    ok( server != INVALID_HANDLE_VALUE, "CreateNamedPipe failed: %lu\n", GetLastError() );

    client = CreateFileA( pipe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                          FILE_FLAG_NO_BUFFERING, NULL );
    ok( client != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError() );

    memset( buffer, 0xcc, sizeof(buffer) );
    memset( &iosb32, 0x55, sizeof(iosb32) );
    iosb64.Pointer = PtrToUlong( &iosb32 );
    iosb64.Information = 0xdeadbeef;

    args[0] = (LONG_PTR)server;
    status = call_func64( read_func, ARRAY_SIZE(args), args );
    ok( status == STATUS_PENDING, "NtReadFile returned %lx\n", status );
    ok( iosb32.Status == 0x55555555, "status changed to %lx\n", iosb32.Status );
    ok( iosb64.Pointer == PtrToUlong(&iosb32), "pointer changed to %I64x\n", iosb64.Pointer );
    ok( iosb64.Information == 0xdeadbeef, "info changed to %Ix\n", (ULONG_PTR)iosb64.Information );

    ret = WriteFile( client, "data", sizeof("data"), &size, NULL );
    ok( ret == TRUE, "got error %lu\n", GetLastError() );

    ok( iosb32.Status == 0, "Wrong iostatus %lx\n", iosb32.Status );
    ok( iosb32.Information == sizeof("data"), "Wrong information %Ix\n", iosb32.Information );
    ok( iosb64.Pointer == PtrToUlong(&iosb32), "pointer changed to %I64x\n", iosb64.Pointer );
    ok( iosb64.Information == 0xdeadbeef, "info changed to %Ix\n", (ULONG_PTR)iosb64.Information );
    ok( !memcmp( buffer, "data", iosb32.Information ),
        "got wrong data %s\n", debugstr_an(buffer, iosb32.Information) );

    memset( buffer, 0xcc, sizeof(buffer) );
    memset( &iosb32, 0x55, sizeof(iosb32) );
    iosb64.Pointer = PtrToUlong( &iosb32 );
    iosb64.Information = 0xdeadbeef;

    ret = WriteFile( client, "data", sizeof("data"), &size, NULL );
    ok( ret == TRUE, "got error %lu\n", GetLastError() );

    status = call_func64( read_func, ARRAY_SIZE(args), args );
    ok( status == STATUS_SUCCESS, "NtReadFile returned %lx\n", status );
    ok( iosb32.Status == STATUS_SUCCESS, "status changed to %lx\n", iosb32.Status );
    ok( iosb32.Information == sizeof("data"), "info changed to %Ix\n", iosb32.Information );
    ok( iosb64.Pointer == PtrToUlong(&iosb32), "pointer changed to %I64x\n", iosb64.Pointer );
    ok( iosb64.Information == 0xdeadbeef, "info changed to %Ix\n", (ULONG_PTR)iosb64.Information );
    ok( !memcmp( buffer, "data", iosb32.Information ),
        "got wrong data %s\n", debugstr_an(buffer, iosb32.Information) );

    /* syscalls which are always synchronous set iosb64 but not iosb32 */

    memset( &iosb32, 0x55, sizeof(iosb32) );
    iosb64.Pointer = PtrToUlong( &iosb32 );
    iosb64.Information = 0xdeadbeef;

    flush_args[0] = (LONG_PTR)server;
    status = call_func64( flush_func, ARRAY_SIZE(flush_args), flush_args );
    ok( status == STATUS_SUCCESS, "NtFlushBuffersFile returned %lx\n", status );
    ok( iosb32.Status == 0x55555555, "status changed to %lx\n", iosb32.Status );
    ok( iosb32.Information == 0x55555555, "info changed to %Ix\n", iosb32.Information );
    ok( iosb64.Pointer == STATUS_SUCCESS, "pointer changed to %I64x\n", iosb64.Pointer );
    ok( iosb64.Information == 0, "info changed to %Ix\n", (ULONG_PTR)iosb64.Information );

    CloseHandle( client );
    CloseHandle( server );

    /* synchronous calls set iosb64 but not iosb32 */

    server = CreateNamedPipeA( pipe_name, PIPE_ACCESS_DUPLEX,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                               4, 1024, 1024, 1000, NULL );
    ok( server != INVALID_HANDLE_VALUE, "CreateNamedPipe failed: %lu\n", GetLastError() );

    client = CreateFileA( pipe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                          FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, NULL );
    ok( client != INVALID_HANDLE_VALUE, "CreateFile failed: %lu\n", GetLastError() );

    ret = WriteFile( client, "data", sizeof("data"), &size, NULL );
    ok( ret == TRUE, "got error %lu\n", GetLastError() );

    memset( buffer, 0xcc, sizeof(buffer) );
    memset( &iosb32, 0x55, sizeof(iosb32) );
    iosb64.Pointer = PtrToUlong( &iosb32 );
    iosb64.Information = 0xdeadbeef;

    args[0] = (LONG_PTR)server;
    status = call_func64( read_func, ARRAY_SIZE(args), args );
    ok( status == STATUS_SUCCESS, "NtReadFile returned %lx\n", status );
    ok( iosb32.Status == 0x55555555, "status changed to %lx\n", iosb32.Status );
    ok( iosb32.Information == 0x55555555, "info changed to %Ix\n", iosb32.Information );
    ok( iosb64.Pointer == STATUS_SUCCESS, "pointer changed to %I64x\n", iosb64.Pointer );
    ok( iosb64.Information == sizeof("data"), "info changed to %Ix\n", (ULONG_PTR)iosb64.Information );
    ok( !memcmp( buffer, "data", iosb64.Information ),
        "got wrong data %s\n", debugstr_an(buffer, iosb64.Information) );

    thread = CreateThread( NULL, 0, iosb_delayed_write_thread, client, 0, NULL );

    memset( buffer, 0xcc, sizeof(buffer) );
    memset( &iosb32, 0x55, sizeof(iosb32) );
    iosb64.Pointer = PtrToUlong( &iosb32 );
    iosb64.Information = 0xdeadbeef;

    args[0] = (LONG_PTR)server;
    status = call_func64( read_func, ARRAY_SIZE(args), args );
    ok( status == STATUS_SUCCESS, "NtReadFile returned %lx\n", status );
    todo_wine
    {
    ok( iosb32.Status == 0x55555555, "status changed to %lx\n", iosb32.Status );
    ok( iosb32.Information == 0x55555555, "info changed to %Ix\n", iosb32.Information );
    ok( iosb64.Pointer == STATUS_SUCCESS, "pointer changed to %I64x\n", iosb64.Pointer );
    ok( iosb64.Information == sizeof("data"), "info changed to %Ix\n", (ULONG_PTR)iosb64.Information );
    ok( !memcmp( buffer, "data", iosb64.Information ),
        "got wrong data %s\n", debugstr_an(buffer, iosb64.Information) );
    }

    ret = WaitForSingleObject( thread, 1000 );
    ok(!ret, "got %d\n", ret );
    CloseHandle( thread );

    memset( &iosb32, 0x55, sizeof(iosb32) );
    iosb64.Pointer = PtrToUlong( &iosb32 );
    iosb64.Information = 0xdeadbeef;

    flush_args[0] = (LONG_PTR)server;
    status = call_func64( flush_func, ARRAY_SIZE(flush_args), flush_args );
    ok( status == STATUS_SUCCESS, "NtFlushBuffersFile returned %lx\n", status );
    ok( iosb32.Status == 0x55555555, "status changed to %lx\n", iosb32.Status );
    ok( iosb32.Information == 0x55555555, "info changed to %Ix\n", iosb32.Information );
    ok( iosb64.Pointer == STATUS_SUCCESS, "pointer changed to %I64x\n", iosb64.Pointer );
    ok( iosb64.Information == 0, "info changed to %Ix\n", (ULONG_PTR)iosb64.Information );

    CloseHandle( client );
    CloseHandle( server );
}

static NTSTATUS invoke_syscall( const char *name, ULONG args32[] )
{
    ULONG64 args64[] = { -1, PtrToUlong( args32 ) };
    ULONG64 func = get_proc_address64( modules.wow64, "Wow64SystemServiceEx" );
    BYTE *syscall = (BYTE *)GetProcAddress( GetModuleHandleA("ntdll.dll"), name );

    ok( syscall != NULL, "syscall %s not found\n", name );
    if (syscall[0] == 0xb8)
        args64[0] = *(DWORD *)(syscall + 1);
    else
        win_skip( "syscall thunk %s not recognized\n", name );

    return call_func64( func, ARRAY_SIZE(args64), args64 );
}

static void test_syscalls(void)
{
    ULONG64 func;
    ULONG args32[8];
    HANDLE event, event2;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name;
    NTSTATUS status;

    if (!is_wow64) return;
    if (!code_mem) return;
    if (!modules.ntdll) return;

    func = get_proc_address64( modules.wow64, "Wow64SystemServiceEx" );
    ok( func, "Wow64SystemServiceEx not found\n" );

    event = CreateEventA( NULL, FALSE, FALSE, NULL );

    status = NtSetEvent( event, NULL );
    ok( !status, "NtSetEvent failed %lx\n", status );
    args32[0] = HandleToLong( event );
    status = invoke_syscall( "NtClose", args32 );
    ok( !status, "syscall failed %lx\n", status );
    status = NtSetEvent( event, NULL );
    ok( status == STATUS_INVALID_HANDLE, "NtSetEvent failed %lx\n", status );
    status = invoke_syscall( "NtClose", args32 );
    ok( status == STATUS_INVALID_HANDLE, "syscall failed %lx\n", status );
    args32[0] = 0xdeadbeef;
    status = invoke_syscall( "NtClose", args32 );
    ok( status == STATUS_INVALID_HANDLE, "syscall failed %lx\n", status );

    RtlInitUnicodeString( &name, L"\\BaseNamedObjects\\wow64-test");
    InitializeObjectAttributes( &attr, &name, OBJ_OPENIF, 0, NULL );
    event = (HANDLE)0xdeadbeef;
    args32[0] = PtrToUlong(&event );
    args32[1] = EVENT_ALL_ACCESS;
    args32[2] = PtrToUlong( &attr );
    args32[3] = NotificationEvent;
    args32[4] = 0;
    status = invoke_syscall( "NtCreateEvent", args32 );
    ok( !status, "syscall failed %lx\n", status );
    status = NtSetEvent( event, NULL );
    ok( !status, "NtSetEvent failed %lx\n", status );

    event2 = (HANDLE)0xdeadbeef;
    args32[0] = PtrToUlong( &event2 );
    status = invoke_syscall( "NtOpenEvent", args32 );
    ok( !status, "syscall failed %lx\n", status );
    status = NtSetEvent( event2, NULL );
    ok( !status, "NtSetEvent failed %lx\n", status );
    args32[0] = HandleToLong( event2 );
    status = invoke_syscall( "NtClose", args32 );
    ok( !status, "syscall failed %lx\n", status );

    event2 = (HANDLE)0xdeadbeef;
    args32[0] = PtrToUlong( &event2 );
    status = invoke_syscall( "NtCreateEvent", args32 );
    ok( status == STATUS_OBJECT_NAME_EXISTS, "syscall failed %lx\n", status );
    status = NtSetEvent( event2, NULL );
    ok( !status, "NtSetEvent failed %lx\n", status );
    args32[0] = HandleToLong( event2 );
    status = invoke_syscall( "NtClose", args32 );
    ok( !status, "syscall failed %lx\n", status );

    status = NtClose( event );
    ok( !status, "NtClose failed %lx\n", status );

    if (pNtWow64ReadVirtualMemory64)
    {
        TEB64 *teb64 = (TEB64 *)NtCurrentTeb()->GdiBatchCount;
        PEB64 peb64, peb64_2;
        ULONG64 res, res2;
        HANDLE process = OpenProcess( PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId() );
        ULONG args32[] = { HandleToLong( process ), (ULONG)teb64->Peb, teb64->Peb >> 32,
                           PtrToUlong(&peb64_2), sizeof(peb64_2), 0, PtrToUlong(&res2) };

        ok( process != 0, "failed to open current process %lu\n", GetLastError() );
        status = pNtWow64ReadVirtualMemory64( process, teb64->Peb, &peb64, sizeof(peb64), &res );
        ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
        status = invoke_syscall( "NtWow64ReadVirtualMemory64", args32 );
        ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );
        ok( res2 == res, "wrong len %s / %s\n", wine_dbgstr_longlong(res), wine_dbgstr_longlong(res2) );
        ok( !memcmp( &peb64, &peb64_2, res ), "data is different\n" );
        NtClose( process );
    }
}

static void test_cpu_area(void)
{
    TEB64 *teb64 = (TEB64 *)NtCurrentTeb()->GdiBatchCount;
    ULONG64 ptr;
    NTSTATUS status;

    if (!is_wow64) return;
    if (!code_mem) return;
    if (!modules.ntdll) return;

    if ((ptr = get_proc_address64( modules.ntdll, "RtlWow64GetCurrentCpuArea" )))
    {
        USHORT machine = 0xdead;
        ULONG64 context, context_ex;
        ULONG64 args[] = { (ULONG_PTR)&machine, (ULONG_PTR)&context, (ULONG_PTR)&context_ex };

        status = call_func64( ptr, ARRAY_SIZE(args), args );
        ok( !status, "RtlWow64GetCpuAreaInfo failed %lx\n", status );
        ok( machine == IMAGE_FILE_MACHINE_I386, "wrong machine %x\n", machine );
        ok( context == teb64->TlsSlots[WOW64_TLS_CPURESERVED] + 4, "wrong context %s / %s\n",
            wine_dbgstr_longlong(context), wine_dbgstr_longlong(teb64->TlsSlots[WOW64_TLS_CPURESERVED]) );
        ok( !context_ex, "got context_ex %s\n", wine_dbgstr_longlong(context_ex) );
        args[0] = args[1] = args[2] = 0;
        status = call_func64( ptr, ARRAY_SIZE(args), args );
        ok( !status, "RtlWow64GetCpuAreaInfo failed %lx\n", status );
    }
    else win_skip( "RtlWow64GetCpuAreaInfo not supported\n" );

}

static void test_exception_dispatcher(void)
{
    ULONG64 ptr, hook_ptr, hook, expect, res;
    NTSTATUS status;
    BYTE code[8];

    if (!is_wow64) return;
    if (!code_mem) return;
    if (!modules.ntdll) return;

    ptr = get_proc_address64( modules.ntdll, "KiUserExceptionDispatcher" );
    ok( ptr, "KiUserExceptionDispatcher not found\n" );

    if (pNtWow64ReadVirtualMemory64)
    {
        HANDLE process = OpenProcess( PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId() );

        ok( process != 0, "failed to open current process %lu\n", GetLastError() );
        status = pNtWow64ReadVirtualMemory64( process, ptr, &code, sizeof(code), &res );
        ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );

        /* cld; mov xxx(%rip),%rax */
        ok( code[0] == 0xfc && code[1] == 0x48 && code[2] == 0x8b && code[3] == 0x05,
            "wrong opcodes %02x %02x %02x %02x\n", code[0], code[1], code[2], code[3] );
        hook_ptr = ptr + 8 + *(int *)(code + 4);
        status = pNtWow64ReadVirtualMemory64( process, hook_ptr, &hook, sizeof(hook), &res );
        ok( !status, "NtWow64ReadVirtualMemory64 failed %lx\n", status );

        expect = get_proc_address64( modules.wow64, "Wow64PrepareForException" );
        ok( hook == expect, "hook %I64x set to %I64x / %I64x\n", hook_ptr, hook, expect );
        NtClose( process );
    }
}

#endif  /* _WIN64 */

#ifdef __arm64ec__

struct doorbell_params
{
    ULONG *doorbell;
    ULONG64 suspend_rip;
    BOOL syscall;
    BOOL suspend;
};

static DWORD WINAPI doorbell_thread( void *arg )
{
    CHPE_V2_CPU_AREA_INFO *chpe = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    struct doorbell_params *params = arg;
    ULONG signaled_doorbell = -1;
    NTSTATUS status;
    HANDLE event;
    CONTEXT ctx;
    LONG i = 0;

    RtlCaptureContext( &ctx );

    if (InterlockedIncrement( &i ) == 1)
    {
        params->suspend_rip = ctx.Rip;

        if (params->syscall) chpe->InSyscallCallback = 1;
        else chpe->InSimulation = 1;
        params->doorbell = chpe->SuspendDoorbell;
        ok( params->doorbell != NULL, "doorbell is not available\n" );
        while (!(signaled_doorbell = *params->doorbell)) YieldProcessor();
        chpe->InSyscallCallback = chpe->InSimulation = 0;

        /* syscalls, including waits, continue working */
        event = CreateEventW( NULL, FALSE, TRUE, NULL );
        ok( event != NULL, "CreateEvent failed\n" );
        status = NtWaitForSingleObject( event, FALSE, NULL );
        ok( !status, "NtWaitForSingleObject failed\n" );
        status = NtClose( event );
        ok( !status, "NtClose failed\n" );

        NtContinue( &ctx, FALSE );
        ok( 0, "NtContinue failed\n" );
    }

    ok( !*params->doorbell, "doorbell = %lx\n", *params->doorbell );
    ok( signaled_doorbell == -1, "signaled_doorbell = %lx\n", signaled_doorbell );
    params->doorbell = NULL;
    return 0;
}

struct pipe_read_params
{
    CHPE_V2_CPU_AREA_INFO *chpe;
    HANDLE pipe;
    HANDLE event;
    int flush;
};

static DWORD WINAPI pipe_read_thread( void *arg )
{
    struct pipe_read_params *params = arg;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    char c;

    params->chpe = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    NtSetEvent( params->event, NULL );
    if (params->flush)
    {
        int i;
        for (i = 0; i < 100; i++) NtFlushInstructionCache( GetCurrentProcess, pipe_read_thread, 4 );
    }
    status = NtReadFile( params->pipe, NULL, NULL, NULL, &iosb, &c, sizeof(c), NULL, 0 );
    ok( !status, "NtReadFile failed: %lx\n", status );
    return 0;
}

struct nested_continue_params
{
    HANDLE event;
    ULONG64 suspend_rip;
    LONG pass;
};

static DWORD WINAPI nested_continue_thread( void *arg )
{
    CHPE_V2_CPU_AREA_INFO *chpe = NtCurrentTeb()->ChpeV2CpuAreaInfo;
    struct nested_continue_params *params = arg;
    CONTEXT ctx, nested_ctx;

    RtlCaptureContext( &ctx );
    if (InterlockedIncrement( &params->pass ) != 1) return 0;

    params->suspend_rip = ctx.Rip;
    chpe->InSyscallCallback = 1;
    NtSetEvent( params->event, NULL );
    while (!*chpe->SuspendDoorbell) YieldProcessor();

    /* with InSyscallCallback set, NtContinue does not suspend */
    RtlCaptureContext( &nested_ctx );
    if (InterlockedIncrement( &params->pass ) == 2) NtContinue( &nested_ctx, FALSE );
    chpe->InSyscallCallback = 0;

    /* with InSimulation set, NtContinue does not suspend */
    chpe->InSimulation = 1;
    RtlCaptureContext( &nested_ctx );
    if (InterlockedIncrement( &params->pass ) == 4) NtContinue( &nested_ctx, FALSE );
    chpe->InSimulation = 0;

    NtContinue( &ctx, FALSE );
    return 0;
}

static void test_suspend_doorbell(void)
{
    struct nested_continue_params nested_params;
    struct pipe_read_params read_params;
    struct doorbell_params params;
    HANDLE thread, pipe;
    CONTEXT ctx;
    DWORD ret, pass;
    char c = 0;

    for (pass = 0; pass < 4; pass++)
    {
        memset( &params, 0, sizeof(params) );
        params.suspend = (pass & 1) != 0;
        params.syscall = (pass & 2) != 0;

        thread = CreateThread( NULL, 0, doorbell_thread, &params, 0, NULL );
        ok( thread != NULL, "CreateThread failed\n" );

        while (!params.doorbell) YieldProcessor();
        ok( !*params.doorbell, "doorbell = %lx\n", *params.doorbell );

        if (params.suspend) SuspendThread( thread );

        memset( &ctx, 0xcc, sizeof(ctx) );
        ctx.ContextFlags = CONTEXT_FULL;
        GetThreadContext( thread, &ctx );
        ok( ctx.Rip == params.suspend_rip, "Rip = %llx, expected %llx\n", ctx.Rip, params.suspend_rip );

        if (params.suspend) ResumeThread( thread );

        WaitForSingleObject( thread, INFINITE );
        ok( !params.doorbell, "thread did not reset doorbell\n" );

        CloseHandle( thread );
    }

    for (read_params.flush = 0; read_params.flush < 2; read_params.flush++)
    {
        CreatePipe( &read_params.pipe, &pipe, NULL, 0 );
        read_params.event = CreateEventW( NULL, FALSE, FALSE, NULL );
        thread = CreateThread( NULL, 0, pipe_read_thread, &read_params, 0, NULL );

        ret = WaitForSingleObject( read_params.event, 10000 );
        ok( ret == 0, "wait failed %lx\n", ret );

        /* hammer the thread with suspend requests, making sure we never hit a syscall callback */
        for (pass = 0; pass < 100; pass++)
        {
            ret = SuspendThread( thread );
            ok( !ret, "SuspendThread failed: %lu\n", GetLastError() );
            ctx.ContextFlags = CONTEXT_FULL;
            ret = GetThreadContext( thread, &ctx );
            ok( ret, "GetThreadContext failed: %lu\n", GetLastError() );

            ok( !read_params.chpe->InSyscallCallback, "InSyscallCallback = %x\n",
                read_params.chpe->InSyscallCallback );

            ret = ResumeThread( thread );
            ok( ret == 1, "ResumeThread failed: %lu\n", GetLastError() );
        }

        WriteFile( pipe, &c, sizeof(c), NULL, NULL );
        ret = WaitForSingleObject( thread, 10000 );
        ok( ret == 0, "wait failed %lx\n", ret );

        CloseHandle( thread );
        CloseHandle( pipe );
        CloseHandle( read_params.pipe );
        CloseHandle( read_params.event );
    }

    nested_params.event = CreateEventW( NULL, FALSE, FALSE, NULL );
    nested_params.pass = 0;
    thread = CreateThread( NULL, 0, nested_continue_thread, &nested_params, 0, NULL );

    ret = WaitForSingleObject( nested_params.event, 10000 );
    ok( ret == 0, "wait failed %lx\n", ret );

    ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext( thread, &ctx );
    ok( ctx.Rip == nested_params.suspend_rip, "Rip = %llx, expected %llx\n", ctx.Rip, params.suspend_rip );

    ret = WaitForSingleObject( thread, 10000 );
    ok( ret == 0, "wait failed %lx\n", ret );
    ok( nested_params.pass == 6, "pass = %lu\n", nested_params.pass );

    CloseHandle( nested_params.event );
    CloseHandle( thread );
}

#endif /* __arm64ec__ */

static void test_arm64ec(void)
{
#ifdef __aarch64__
    PROCESS_INFORMATION pi;
    char cmdline[MAX_PATH];
    char **argv;

    trace( "restarting test as arm64ec\n" );

    winetest_get_mainargs( &argv );
    sprintf( cmdline, "%s %s", argv[0], argv[1] );
    if (create_process_machine( cmdline, 0, IMAGE_FILE_MACHINE_AMD64, &pi ))
    {
        DWORD exit_code, ret = WaitForSingleObject( pi.hProcess, 10000 );
        ok( ret == 0, "wait failed %lx\n", ret );
        GetExitCodeProcess( pi.hProcess, &exit_code );
        ok( exit_code == 0xbeef, "wrong exit code %lx\n", exit_code );
        CloseHandle( pi.hProcess );
        CloseHandle( pi.hThread );
    }
    else skip( "could not start arm64ec process: %lu\n", GetLastError() );
#endif
}

START_TEST(wow64)
{
    init();
    test_unixlib_dispatch_bounds();
    test_unixlib_dispatch_lifecycle();
    test_unixlib_zero_args_requires_wow64_model();
    test_query_architectures(SystemSupportedProcessorArchitectures);
    test_query_architectures(SystemSupportedProcessorArchitectures2);
    test_remote_memory_address_codec();
    test_remote_memory_codec_cleanup();
    test_wow64_server_call_protection();
    test_complete_new_process_request_validation();
    test_wow64_raw_server_call_protection();
    test_wow64_handle_pair_publication();
    test_wow64_stack_guard_granularity();
    test_continue_boolean_argument();
    test_peb_teb();
    test_selectors();
    test_image_mappings();
#ifdef _WIN64
    test_xtajit64();
    test_cross_process_work_list();
#else
    test_nt_wow64();
    test_modules();
    test_init_block();
    test_iosb();
    test_syscalls();
#endif
#ifdef __arm64ec__
    test_suspend_doorbell();
#endif
    test_memory_notifications();
#ifdef _WIN64
    test_translated_view_information();
#endif
#ifdef __arm64ec__
    test_x64_execution_gate();
#endif
    test_cpu_area();
    test_exception_dispatcher();
    test_arm64ec();
}
