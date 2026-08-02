/*
 * Unit test suite for ntdll thread functions
 *
 * Copyright 2021 Paul Gofman for CodeWeavers
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
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winioctl.h"
#include "winternl.h"
#include "wine/appx_package_graph.h"
#include "wine/test.h"

C_ASSERT( offsetof(struct wine_appx_graph_attach, blob) == 16 );
C_ASSERT( offsetof(struct wine_appx_graph_attach, leases) == 24 );
C_ASSERT( offsetof(struct wine_appx_graph_attach, lease_count) == 32 );
C_ASSERT( sizeof(struct wine_appx_graph_attach) == 40 );
C_ASSERT( __alignof__(struct wine_appx_graph_attach) >= 8 );

static BOOL is_wow64;
static BOOL old_wow64;
static BOOL is_arm64_native_machine;
static HANDLE package_graph_test_lease[3] =
    {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
static unsigned long long package_graph_test_leases[3];
static const WCHAR * const package_graph_test_full_names[3] =
{
    L"Pkg_1.0.0.0_neutral__pub",
    L"Direct.Framework_1.0.0.0_neutral__pub",
    L"Transitive.Framework_1.0.0.0_neutral__pub",
};
static const BYTE package_graph_test_content_ids[3][32] =
{
    {0x11, 0x22, 0x33, 0x40},
    {0x11, 0x22, 0x33, 0x41},
    {0x11, 0x22, 0x33, 0x42},
};

static NTSTATUS (WINAPI *pNtAllocateReserveObject)( HANDLE *, const OBJECT_ATTRIBUTES *, MEMORY_RESERVE_OBJECT_TYPE );
static NTSTATUS (WINAPI *pNtCreateThreadEx)( HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *,
                                             HANDLE, PRTL_THREAD_START_ROUTINE, void *,
                                             ULONG, ULONG_PTR, SIZE_T, SIZE_T, PS_ATTRIBUTE_LIST * );
static NTSTATUS  (WINAPI *pNtSuspendProcess)(HANDLE process);
static NTSTATUS  (WINAPI *pNtResumeProcess)(HANDLE process);
static NTSTATUS  (WINAPI *pNtQueueApcThreadEx)(HANDLE handle, HANDLE reserve_handle, PNTAPCFUNC func,
                                               ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3);
static NTSTATUS  (WINAPI *pNtQueueApcThreadEx2)(HANDLE handle, HANDLE reserve_handle, ULONG flags, PNTAPCFUNC func,
                                                ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3);
static NTSTATUS  (WINAPI *pRtlWow64GetProcessMachines)(HANDLE, WORD*, WORD*);

#ifdef __x86_64__
static NTSTATUS (WINAPI *pNtAllocateVirtualMemoryEx)(HANDLE, PVOID *, SIZE_T *, ULONG, ULONG,
                                                     MEM_EXTENDED_PARAMETER *, ULONG);
#endif

static int * (CDECL *p_errno)(void);

static BOOL (WINAPI *pIsWow64Process)(HANDLE, PBOOL);

static void init_function_pointers(void)
{
    HMODULE hdll;
#define GET_FUNC(name) p##name = (void *)GetProcAddress( hdll, #name );
    hdll = GetModuleHandleA( "ntdll.dll" );
    GET_FUNC( NtAllocateReserveObject );
    GET_FUNC( NtCreateThreadEx );
    GET_FUNC( NtSuspendProcess );
    GET_FUNC( NtQueueApcThreadEx );
    GET_FUNC( NtQueueApcThreadEx2 );
    GET_FUNC( NtResumeProcess );
    GET_FUNC( RtlWow64GetProcessMachines );
    GET_FUNC( _errno );

#ifdef __x86_64__
    GET_FUNC( NtAllocateVirtualMemoryEx );
#endif

    hdll = GetModuleHandleA( "kernel32.dll" );
    GET_FUNC( IsWow64Process );
#undef GET_FUNC
}

static void CALLBACK test_NtCreateThreadEx_proc(void *param)
{
}

static void test_dbg_hidden_thread_creation(void)
{
    RTL_USER_PROCESS_PARAMETERS *params;
    PS_CREATE_INFO create_info;
    PS_ATTRIBUTE_LIST ps_attr;
    WCHAR path[MAX_PATH + 4];
    HANDLE process, thread;
    UNICODE_STRING imageW;
    BOOLEAN dbg_hidden;
    NTSTATUS status;

    if (!pNtCreateThreadEx)
    {
        win_skip( "NtCreateThreadEx is not available.\n" );
        return;
    }

    status = pNtCreateThreadEx( &thread, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), test_NtCreateThreadEx_proc,
                                NULL, THREAD_CREATE_FLAGS_CREATE_SUSPENDED, 0, 0, 0, NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );

    dbg_hidden = 0xcc;
    status = NtQueryInformationThread( thread, ThreadHideFromDebugger, &dbg_hidden, sizeof(dbg_hidden), NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    ok( !dbg_hidden, "Got unexpected dbg_hidden %#x.\n", dbg_hidden );

    status = NtResumeThread( thread, NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );

    status = pNtCreateThreadEx( &thread, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), test_NtCreateThreadEx_proc,
                                NULL, THREAD_CREATE_FLAGS_CREATE_SUSPENDED | THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER,
                                0, 0, 0, NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );

    dbg_hidden = 0xcc;
    status = NtQueryInformationThread( thread, ThreadHideFromDebugger, &dbg_hidden, sizeof(dbg_hidden), NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    ok( dbg_hidden == 1, "Got unexpected dbg_hidden %#x.\n", dbg_hidden );

    status = NtResumeThread( thread, NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );

    lstrcpyW( path, L"\\??\\" );
    GetModuleFileNameW( NULL, path + 4, MAX_PATH );

    RtlInitUnicodeString( &imageW, path );

    memset( &ps_attr, 0, sizeof(ps_attr) );
    ps_attr.Attributes[0].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
    ps_attr.Attributes[0].Size = lstrlenW(path) * sizeof(WCHAR);
    ps_attr.Attributes[0].ValuePtr = path;
    ps_attr.TotalLength = sizeof(ps_attr);

    status = RtlCreateProcessParametersEx( &params, &imageW, NULL, NULL,
                                           NULL, NULL, NULL, NULL,
                                           NULL, NULL, PROCESS_PARAMS_FLAG_NORMALIZED );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );

    /* NtCreateUserProcess() may return STATUS_INVALID_PARAMETER with some uninitialized data in create_info. */
    memset( &create_info, 0, sizeof(create_info) );
    create_info.Size = sizeof(create_info);

    status = NtCreateUserProcess( &process, &thread, PROCESS_ALL_ACCESS, THREAD_ALL_ACCESS,
                                  NULL, NULL, 0, THREAD_CREATE_FLAGS_CREATE_SUSPENDED
                                  | THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER, params,
                                  &create_info, &ps_attr );
    ok( status == STATUS_INVALID_PARAMETER, "Got unexpected status %#lx.\n", status );
    status = NtCreateUserProcess( &process, &thread, PROCESS_ALL_ACCESS, THREAD_ALL_ACCESS,
                                  NULL, NULL, 0, THREAD_CREATE_FLAGS_CREATE_SUSPENDED, params,
                                  &create_info, &ps_attr );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    status = NtTerminateProcess( process, 0 );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    CloseHandle( process );
    CloseHandle( thread );
}

struct unique_teb_thread_args
{
    TEB *teb;
    HANDLE running_event;
    HANDLE quit_event;
};

static void CALLBACK test_unique_teb_proc(void *param)
{
    struct unique_teb_thread_args *args = param;
    args->teb = NtCurrentTeb();
    SetEvent( args->running_event );
    WaitForSingleObject( args->quit_event, INFINITE );
}

static void test_unique_teb(void)
{
    HANDLE threads[2], running_events[2];
    struct unique_teb_thread_args args1, args2;
    NTSTATUS status;

    if (!pNtCreateThreadEx)
    {
        win_skip( "NtCreateThreadEx is not available.\n" );
        return;
    }

    args1.running_event = running_events[0] = CreateEventW( NULL, FALSE, FALSE, NULL );
    ok( args1.running_event != NULL, "CreateEventW failed %lu.\n", GetLastError() );

    args2.running_event = running_events[1] = CreateEventW( NULL, FALSE, FALSE, NULL );
    ok( args2.running_event != NULL, "CreateEventW failed %lu.\n", GetLastError() );

    args1.quit_event = args2.quit_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( args1.quit_event != NULL, "CreateEventW failed %lu.\n", GetLastError() );

    status = pNtCreateThreadEx( &threads[0], THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), test_unique_teb_proc,
                                &args1, 0, 0, 0, 0, NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );

    status = pNtCreateThreadEx( &threads[1], THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), test_unique_teb_proc,
                                &args2, 0, 0, 0, 0, NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );

    WaitForMultipleObjects( 2, running_events, TRUE, INFINITE );
    SetEvent( args1.quit_event );

    WaitForMultipleObjects( 2, threads, TRUE, INFINITE );
    CloseHandle( threads[0] );
    CloseHandle( threads[1] );
    CloseHandle( args1.running_event );
    CloseHandle( args2.running_event );
    CloseHandle( args1.quit_event );

    ok( NtCurrentTeb() != args1.teb, "Multiple threads have TEB %p.\n", args1.teb );
    ok( NtCurrentTeb() != args2.teb, "Multiple threads have TEB %p.\n", args2.teb );
    ok( args1.teb != args2.teb, "Multiple threads have TEB %p.\n", args1.teb );
}

static void test_errno(void)
{
    int val;

    if (!p_errno)
    {
        win_skip( "_errno not available\n" );
        return;
    }
    ok( NtCurrentTeb()->Peb->TlsBitmap->Buffer[0] & (1 << 16), "TLS entry 16 not allocated\n" );
    *p_errno() = 0xdead;
    val = PtrToLong( TlsGetValue( 16 ));
    ok( val == 0xdead, "wrong value %x\n", val );
    *p_errno() = 0xbeef;
    val = PtrToLong( TlsGetValue( 16 ));
    ok( val == 0xbeef, "wrong value %x\n", val );
}

static void test_NtCreateUserProcess(void)
{
    RTL_USER_PROCESS_PARAMETERS *params;
    PS_CREATE_INFO create_info;
    PS_ATTRIBUTE_LIST ps_attr;
    WCHAR path[MAX_PATH + 4];
    HANDLE process, thread;
    UNICODE_STRING imageW;
    NTSTATUS status;

    lstrcpyW( path, L"\\??\\" );
    GetModuleFileNameW( NULL, path + 4, MAX_PATH );

    RtlInitUnicodeString( &imageW, path );

    memset( &ps_attr, 0, sizeof(ps_attr) );
    ps_attr.Attributes[0].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
    ps_attr.Attributes[0].Size = lstrlenW(path) * sizeof(WCHAR);
    ps_attr.Attributes[0].ValuePtr = path;
    ps_attr.TotalLength = sizeof(ps_attr);

    status = RtlCreateProcessParametersEx( &params, &imageW, NULL, NULL,
                                           NULL, NULL, NULL, NULL,
                                           NULL, NULL, PROCESS_PARAMS_FLAG_NORMALIZED );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );

    memset( &create_info, 0, sizeof(create_info) );
    create_info.Size = sizeof(create_info);
    status = NtCreateUserProcess( &process, &thread, PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, SYNCHRONIZE,
                                  NULL, NULL, 0, THREAD_CREATE_FLAGS_CREATE_SUSPENDED, params,
                                  &create_info, &ps_attr );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    status = NtTerminateProcess( process, 0 );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    CloseHandle( process );
    CloseHandle( thread );
}

static void graph_write_u16( BYTE *data, unsigned int value )
{
    data[0] = value;
    data[1] = value >> 8;
}

static void graph_write_u32( BYTE *data, unsigned int value )
{
    graph_write_u16( data, value );
    graph_write_u16( data + 2, value >> 16 );
}

static void graph_write_u64( BYTE *data, UINT64 value )
{
    graph_write_u32( data, value );
    graph_write_u32( data + 4, value >> 32 );
}

static BOOL write_package_graph_test_marker( HANDLE marker,
                                             unsigned int package_index )
{
    BYTE header[40] = {'S','W','L','M',1};
    DWORD written;
    DWORD name_size =
        (lstrlenW( package_graph_test_full_names[package_index] ) + 1) *
        sizeof(WCHAR);

    memcpy( header + 8, package_graph_test_content_ids[package_index],
            sizeof(package_graph_test_content_ids[package_index]) );
    return WriteFile( marker, header, sizeof(header), &written, NULL ) &&
           written == sizeof(header) &&
           WriteFile( marker, package_graph_test_full_names[package_index],
                      name_size, &written, NULL ) &&
           written == name_size && FlushFileBuffers( marker );
}

static HANDLE create_package_graph_test_lease( const WCHAR *temp_path,
                                               WCHAR path[MAX_PATH],
                                               unsigned int package_index )
{
    HANDLE marker, lease;

    path[0] = 0;
    if (!GetTempFileNameW( temp_path, L"pgl", 0, path ))
        return INVALID_HANDLE_VALUE;
    marker = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (marker == INVALID_HANDLE_VALUE)
    {
        DeleteFileW( path );
        path[0] = 0;
        return INVALID_HANDLE_VALUE;
    }
    if (!write_package_graph_test_marker( marker, package_index ))
    {
        CloseHandle( marker );
        DeleteFileW( path );
        path[0] = 0;
        return INVALID_HANDLE_VALUE;
    }
    CloseHandle( marker );
    lease = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (lease == INVALID_HANDLE_VALUE)
    {
        DeleteFileW( path );
        path[0] = 0;
    }
    return lease;
}

static void graph_append_string( BYTE *graph, unsigned int *position,
                                 unsigned int ref_offset, const WCHAR *string )
{
    unsigned int i, chars = lstrlenW( string ) + 1;

    graph_write_u32( graph + ref_offset, *position );
    graph_write_u32( graph + ref_offset + 4, chars );
    for (i = 0; i < chars; i++)
        graph_write_u16( graph + *position + i * sizeof(WCHAR), string[i] );
    *position += chars * sizeof(WCHAR);
}

static BOOL get_test_graph_application_identity(
    DWORD *volume_serial, DWORD *file_index_high, DWORD *file_index_low,
    BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE] )
{
    BY_HANDLE_FILE_INFORMATION info;
    FILE_OBJECTID_BUFFER native_id;
    WCHAR module[MAX_PATH];
    IO_STATUS_BLOCK io;
    HANDLE file;
    BOOL ret;

    if (!GetModuleFileNameW( NULL, module, ARRAY_SIZE(module) )) return FALSE;
    file = CreateFileW(
        module, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ret = GetFileInformationByHandle( file, &info ) &&
          !NtFsControlFile(
              file, NULL, NULL, NULL, &io, FSCTL_GET_OBJECT_ID,
              NULL, 0, &native_id, sizeof(native_id) );
    CloseHandle( file );
    if (!ret || (!info.nFileIndexHigh && !info.nFileIndexLow)) return FALSE;
    *volume_serial = info.dwVolumeSerialNumber;
    *file_index_high = info.nFileIndexHigh;
    *file_index_low = info.nFileIndexLow;
    memcpy( object_id, native_id.ObjectId, WINE_APPX_GRAPH_OBJECT_ID_SIZE );
    return TRUE;
}

static BYTE *create_test_package_graph( unsigned int marker, unsigned int *size )
{
    static const BYTE loader_object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE] =
        {0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0,
         0x80, 0x70, 0x60, 0x50, 0, 0, 0, 0};
    const unsigned int package_offset = WINE_APPX_GRAPH_BLOB_HEADER_SIZE;
    const unsigned int loader_offset = package_offset +
        3 * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    const unsigned int class_offset = loader_offset +
        WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
    const unsigned int strings_offset = class_offset +
        2 * WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE;
    const unsigned int class2 = class_offset +
        WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE;
    const unsigned int package1 = package_offset +
        WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    const unsigned int package2 = package1 +
        WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    BYTE *graph = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, 4096 );
    BYTE application_object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
    DWORD application_volume, application_index_high, application_index_low;
    unsigned int position = strings_offset;

    if (!graph) return NULL;
    if (!get_test_graph_application_identity(
            &application_volume, &application_index_high,
            &application_index_low, application_object_id ))
    {
        HeapFree( GetProcessHeap(), 0, graph );
        return NULL;
    }
    memcpy( graph, "SWXGRAPH", 8 );
    graph_write_u32( graph + 8, WINE_APPX_GRAPH_BLOB_VERSION );
    graph_write_u32( graph + 12, WINE_APPX_GRAPH_BLOB_HEADER_SIZE );
    graph_write_u32( graph + 32, marker );
    graph_write_u32( graph + 40, WINE_APPX_GRAPH_CURRENT_ARCHITECTURE );
    graph_write_u32( graph + 44, 3 );
    graph_write_u32( graph + 48, package_offset );
    graph_write_u32( graph + 52, 1 );
    graph_write_u32( graph + 56, loader_offset );
    graph_write_u32( graph + 60, strings_offset );
    graph_write_u32( graph + 68, 1 ); /* full trust */
    graph_write_u32( graph + WINE_APPX_GRAPH_HEADER_CLASS_COUNT_OFFSET, 2 );
    graph_write_u32( graph + WINE_APPX_GRAPH_HEADER_CLASSES_OFFSET,
                     class_offset );
    graph_write_u32(
        graph + WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET,
        application_volume );
    graph_write_u32(
        graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET,
        application_index_high );
    graph_write_u32(
        graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET,
        application_index_low );
    memcpy( graph + WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET,
            application_object_id, sizeof(application_object_id) );

    graph_write_u32( graph + package_offset + 8, 0 ); /* neutral */
    graph_write_u32( graph + package_offset + 12,
                     WINE_APPX_GRAPH_PACKAGE_ACTIVE |
                     WINE_APPX_GRAPH_PACKAGE_SIGNED );
    graph_write_u32( graph + package_offset + 16, 0 );
    memcpy( graph + package_offset + 24,
            package_graph_test_content_ids[0],
            sizeof(package_graph_test_content_ids[0]) );
    graph_write_u32( graph + package1 + 8, 0 );
    graph_write_u32( graph + package1 + 12,
                     WINE_APPX_GRAPH_PACKAGE_ACTIVE |
                     WINE_APPX_GRAPH_PACKAGE_FRAMEWORK |
                     WINE_APPX_GRAPH_PACKAGE_SIGNED |
                     WINE_APPX_GRAPH_PACKAGE_DIRECT );
    graph_write_u32( graph + package1 + 16, 1 );
    memcpy( graph + package1 + 24, package_graph_test_content_ids[1],
            sizeof(package_graph_test_content_ids[1]) );
    graph_write_u32( graph + package2 + 8, 0 );
    graph_write_u32( graph + package2 + 12,
                     WINE_APPX_GRAPH_PACKAGE_ACTIVE |
                     WINE_APPX_GRAPH_PACKAGE_FRAMEWORK |
                     WINE_APPX_GRAPH_PACKAGE_SIGNED );
    graph_write_u32( graph + package2 + 16, 2 );
    memcpy( graph + package2 + 24, package_graph_test_content_ids[2],
            sizeof(package_graph_test_content_ids[2]) );

    graph_write_u32( graph + loader_offset, 0 );
    graph_write_u32(
        graph + loader_offset +
        WINE_APPX_GRAPH_LOADER_VOLUME_SERIAL_OFFSET, 0x10203040 );
    graph_write_u32(
        graph + loader_offset +
        WINE_APPX_GRAPH_LOADER_FILE_INDEX_LOW_OFFSET, 0x50607080 );
    graph_write_u64(
        graph + loader_offset +
        WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET, 0x01d0000000000000ULL );
    graph_write_u64(
        graph + loader_offset +
        WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET, 4096 );
    memcpy( graph + loader_offset + WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET,
            loader_object_id, sizeof(loader_object_id) );

    graph_write_u32( graph + class_offset, 0 );
    graph_write_u32( graph + class_offset + 4, 2 ); /* MTA */
    graph_write_u32(
        graph + class_offset + WINE_APPX_GRAPH_CLASS_VOLUME_SERIAL_OFFSET,
        0x10203040 );
    graph_write_u32(
        graph + class_offset + WINE_APPX_GRAPH_CLASS_FILE_INDEX_LOW_OFFSET,
        0x50607080 );
    graph_write_u32(
        graph + class_offset + WINE_APPX_GRAPH_CLASS_LOADER_INDEX_OFFSET, 0 );
    graph_write_u64(
        graph + class_offset + WINE_APPX_GRAPH_CLASS_CHANGE_TIME_OFFSET,
        0x01d0000000000000ULL );
    graph_write_u64(
        graph + class_offset + WINE_APPX_GRAPH_CLASS_FILE_SIZE_OFFSET, 4096 );
    graph_write_u32( graph + class2, 0 );
    graph_write_u32( graph + class2 + 4, 0 ); /* both */
    graph_write_u32(
        graph + class2 + WINE_APPX_GRAPH_CLASS_VOLUME_SERIAL_OFFSET,
        0x10203040 );
    graph_write_u32(
        graph + class2 + WINE_APPX_GRAPH_CLASS_FILE_INDEX_LOW_OFFSET,
        0x50607080 );
    graph_write_u32(
        graph + class2 + WINE_APPX_GRAPH_CLASS_LOADER_INDEX_OFFSET, 0 );
    graph_write_u64(
        graph + class2 + WINE_APPX_GRAPH_CLASS_CHANGE_TIME_OFFSET,
        0x01d0000000000000ULL );
    graph_write_u64(
        graph + class2 + WINE_APPX_GRAPH_CLASS_FILE_SIZE_OFFSET, 4096 );

    graph_append_string( graph, &position, 72, L"App" );
    graph_append_string( graph, &position, 80, L"Family!App" );
    graph_append_string( graph, &position, 88, L"app.exe" );
    graph_append_string( graph, &position, 96, L"" );
    graph_append_string( graph, &position, package_offset + 56, L"Pkg" );
    graph_append_string( graph, &position, package_offset + 64, L"CN=Test" );
    graph_append_string( graph, &position, package_offset + 72, L"" );
    graph_append_string( graph, &position, package_offset + 80, L"pub" );
    graph_append_string( graph, &position, package_offset + 88,
                         package_graph_test_full_names[0] );
    graph_append_string( graph, &position, package_offset + 96, L"Family" );
    graph_append_string( graph, &position, package_offset + 104,
                         L"C:\\Store\\Pkg" );
    graph_append_string( graph, &position, package1 + 56, L"Direct.Framework" );
    graph_append_string( graph, &position, package1 + 64, L"CN=Test" );
    graph_append_string( graph, &position, package1 + 72, L"" );
    graph_append_string( graph, &position, package1 + 80, L"pub" );
    graph_append_string( graph, &position, package1 + 88,
                         package_graph_test_full_names[1] );
    graph_append_string( graph, &position, package1 + 96,
                         L"Direct.Framework_pub" );
    graph_append_string( graph, &position, package1 + 104,
                         L"C:\\Store\\Direct.Framework" );
    graph_append_string( graph, &position, package2 + 56,
                         L"Transitive.Framework" );
    graph_append_string( graph, &position, package2 + 64, L"CN=Test" );
    graph_append_string( graph, &position, package2 + 72, L"" );
    graph_append_string( graph, &position, package2 + 80, L"pub" );
    graph_append_string( graph, &position, package2 + 88,
                         package_graph_test_full_names[2] );
    graph_append_string( graph, &position, package2 + 96,
                         L"Transitive.Framework_pub" );
    graph_append_string( graph, &position, package2 + 104,
                         L"C:\\Store\\Transitive.Framework" );
    graph_append_string( graph, &position, loader_offset + 8, L"a.dll" );
    graph_append_string( graph, &position, loader_offset + 16,
                         L"bin\\a.dll" );
    graph_append_string( graph, &position, class_offset + 8,
                         L"Wine.Class.A" );
    graph_append_string( graph, &position, class_offset + 16,
                         L"bin\\a.dll" );
    graph_append_string( graph, &position, class2 + 8,
                         L"Wine.Class.B" );
    graph_append_string( graph, &position, class2 + 16,
                         L"bin\\a.dll" );

    graph_write_u32( graph + 16, position );
    graph_write_u32( graph + 64, position - strings_offset );
    *size = position;
    if (!wine_appx_graph_validate_blob( graph, position ))
    {
        HeapFree( GetProcessHeap(), 0, graph );
        return NULL;
    }
    return graph;
}

static NTSTATUS create_package_graph_test_process(
    const WCHAR *mode, unsigned int expected_marker,
    const void *graph, unsigned int graph_size, HANDLE parent,
    HANDLE *process, HANDLE *thread )
{
    ULONG_PTR attr_buffer[offsetof( PS_ATTRIBUTE_LIST, Attributes[2] ) /
                           sizeof(ULONG_PTR)];
    PS_ATTRIBUTE_LIST *attr = (PS_ATTRIBUTE_LIST *)attr_buffer;
    struct wine_appx_graph_attach attach;
    RTL_USER_PROCESS_PARAMETERS *params;
    PS_CREATE_INFO create_info;
    UNICODE_STRING image, command;
    WCHAR module[MAX_PATH], nt_path[MAX_PATH + 4], command_line[MAX_PATH + 80];
    unsigned int pos = 0;
    NTSTATUS status;

    *process = *thread = NULL;
    GetModuleFileNameW( NULL, module, ARRAY_SIZE(module) );
    lstrcpyW( nt_path, L"\\??\\" );
    lstrcatW( nt_path, module );
    swprintf( command_line, ARRAY_SIZE(command_line),
              L"\"%s\" thread graph-child %u %s",
              module, expected_marker, mode );
    RtlInitUnicodeString( &image, nt_path );
    RtlInitUnicodeString( &command, command_line );
    status = RtlCreateProcessParametersEx(
        &params, &image, NULL, NULL, &command, NULL, NULL, NULL,
        NULL, NULL, PROCESS_PARAMS_FLAG_NORMALIZED );
    if (status) return status;

    if (graph)
    {
        if (!graph_size) params->PackageDependencyData = (void *)graph;
        else
        {
            attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
            attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
            attach.size = graph_size;
            attach.reserved = 0;
            attach.blob = (ULONG_PTR)graph;
            attach.leases = (ULONG_PTR)package_graph_test_leases;
            attach.lease_count = ARRAY_SIZE(package_graph_test_leases);
            attach.lease_reserved = 0;
            params->PackageDependencyData = &attach;
        }
    }

    memset( attr, 0, sizeof(attr_buffer) );
    attr->Attributes[pos].Attribute = PS_ATTRIBUTE_IMAGE_NAME;
    attr->Attributes[pos].Size = image.Length;
    attr->Attributes[pos].ValuePtr = image.Buffer;
    pos++;
    if (parent)
    {
        attr->Attributes[pos].Attribute = PS_ATTRIBUTE_PARENT_PROCESS;
        attr->Attributes[pos].Size = sizeof(parent);
        attr->Attributes[pos].ValuePtr = parent;
        pos++;
    }
    attr->TotalLength = offsetof( PS_ATTRIBUTE_LIST, Attributes[pos] );

    memset( &create_info, 0, sizeof(create_info) );
    create_info.Size = sizeof(create_info);
    status = NtCreateUserProcess(
        process, thread, PROCESS_ALL_ACCESS, THREAD_ALL_ACCESS,
        NULL, NULL, 0, THREAD_CREATE_FLAGS_CREATE_SUSPENDED, params,
        &create_info, attr );
    RtlDestroyProcessParameters( params );
    return status;
}

static DWORD resume_and_wait_package_graph_child( HANDLE process, HANDLE thread )
{
    DWORD exit_code = ~0u, wait;
    NTSTATUS status = NtResumeThread( thread, NULL );

    if (status) return status;
    wait = WaitForSingleObject( process, 10000 );
    if (wait != WAIT_OBJECT_0)
    {
        NtTerminateProcess( process, STATUS_TIMEOUT );
        WaitForSingleObject( process, 5000 );
        return STATUS_TIMEOUT;
    }
    GetExitCodeProcess( process, &exit_code );
    return exit_code;
}

static void free_package_graph_snapshot( void *graph )
{
    SIZE_T size = 0;
    NTSTATUS status;

    status = NtFreeVirtualMemory( NtCurrentProcess(), &graph, &size,
                                  MEM_RELEASE );
    ok( !status, "Failed to free package graph snapshot %#lx.\n", status );
}

static DWORD validate_package_graph_snapshot( HANDLE process,
                                              unsigned int expected,
                                              const void *different_from )
{
    void *graph = NULL;
    MEMORY_BASIC_INFORMATION memory;
    ULONG size = 0;
    NTSTATUS status;
    DWORD result = 0;

    status = __wine_get_process_package_graph( process, &graph, &size );
    if (status) return status;
    if (!graph || size < WINE_APPX_GRAPH_BLOB_HEADER_SIZE)
        result = 0xe001;
    else if (wine_appx_graph_read_u32(
                 (const BYTE *)graph +
                 WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET ) != size)
        result = 0xe002;
    else if (!wine_appx_graph_validate_blob( graph, size ))
        result = 0xe003;
    else if (wine_appx_graph_read_u32( (const BYTE *)graph + 32 ) !=
             expected)
        result = 0xe004;
    else if (graph == different_from)
        result = 0xe005;
    else if (!VirtualQuery( graph, &memory, sizeof(memory) ) ||
             memory.Protect != PAGE_READONLY)
        result = 0xe006;

    free_package_graph_snapshot( graph );
    return result;
}

static DWORD package_graph_child( int argc, char **argv )
{
    RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
    const BYTE *graph = params->PackageDependencyData;
    HANDLE duplicate;
    unsigned int expected, size;
    MEMORY_BASIC_INFORMATION memory;
    DWORD result;

    if (argc < 5) return 0x101;
    expected = strtoul( argv[3], NULL, 0 );
    if (!expected) return graph ? 0x102 : 0;
    if (!graph) return 0x103;
    size = wine_appx_graph_read_u32(
        graph + WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET );
    if (!wine_appx_graph_validate_blob( graph, size )) return 0x104;
    if (wine_appx_graph_read_u32( graph + 32 ) != expected) return 0x105;
    if (!VirtualQuery( graph, &memory, sizeof(memory) ) ||
        memory.Protect != PAGE_READONLY)
        return 0x106;
    if (wine_appx_graph_read_u32( graph + 44 ) != 3 ||
        wine_appx_graph_read_u32(
            graph + WINE_APPX_GRAPH_BLOB_HEADER_SIZE +
            WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE + 12 ) !=
            (WINE_APPX_GRAPH_PACKAGE_ACTIVE |
             WINE_APPX_GRAPH_PACKAGE_FRAMEWORK |
             WINE_APPX_GRAPH_PACKAGE_SIGNED |
             WINE_APPX_GRAPH_PACKAGE_DIRECT) ||
        wine_appx_graph_read_u32(
            graph + WINE_APPX_GRAPH_BLOB_HEADER_SIZE +
            2 * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE + 12 ) !=
            (WINE_APPX_GRAPH_PACKAGE_ACTIVE |
             WINE_APPX_GRAPH_PACKAGE_FRAMEWORK |
             WINE_APPX_GRAPH_PACKAGE_SIGNED))
        return 0x107;
    if (wine_appx_graph_read_u32(
            graph + WINE_APPX_GRAPH_HEADER_CLASS_COUNT_OFFSET ) != 2)
        return 0x108;
    if (wine_appx_graph_read_u32( graph + 52 ) != 1)
        return 0x10a;

    result = validate_package_graph_snapshot(
        NtCurrentProcess(), expected, graph );
    if (result) return 0x20000 | (result & 0xffff);
    if (!DuplicateHandle( GetCurrentProcess(), GetCurrentProcess(),
                          GetCurrentProcess(), &duplicate,
                          PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0 ))
        return 0x109;
    result = validate_package_graph_snapshot( duplicate, expected, graph );
    CloseHandle( duplicate );
    if (result) return 0x30000 | (result & 0xffff);

    if (!strcmp( argv[4], "chain" ))
    {
        HANDLE process, thread;
        NTSTATUS status = create_package_graph_test_process(
            L"inspect", expected, NULL, 0, NULL, &process, &thread );
        DWORD result;

        if (status) return status;
        result = resume_and_wait_package_graph_child( process, thread );
        CloseHandle( thread );
        CloseHandle( process );
        return result;
    }
    return 0;
}

struct package_graph_mutation_context
{
    struct wine_appx_graph_attach attach;
    BYTE *blob;
    unsigned int invalid_size;
    LONG stop;
};

static DWORD WINAPI mutate_package_graph_attach( void *arg )
{
    struct package_graph_mutation_context *context = arg;

    while (!InterlockedCompareExchange( &context->stop, 0, 0 ))
    {
        InterlockedExchange( (LONG volatile *)&context->attach.size,
                             WINE_APPX_GRAPH_BLOB_HEADER_SIZE );
        InterlockedExchange( (LONG volatile *)(context->blob +
                             WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET),
                             WINE_APPX_GRAPH_BLOB_HEADER_SIZE );
        InterlockedExchange( (LONG volatile *)&context->attach.size,
                             context->invalid_size );
        InterlockedExchange( (LONG volatile *)(context->blob +
                             WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET),
                             context->invalid_size );
    }
    return 0;
}

struct package_graph_query_context
{
    HANDLE process;
    unsigned int expected;
    unsigned int iterations;
    LONG failure;
};

static DWORD WINAPI query_package_graph_snapshots( void *arg )
{
    struct package_graph_query_context *context = arg;
    unsigned int i;

    for (i = 0; i < context->iterations; i++)
    {
        DWORD result = validate_package_graph_snapshot(
            context->process, context->expected, NULL );

        if (result)
        {
            InterlockedCompareExchange( &context->failure, result, 0 );
            break;
        }
    }
    return 0;
}

struct package_graph_peb_mutation_context
{
    HANDLE process;
    void *field;
    LONG stop;
    LONG failure;
    LONG writes;
};

static DWORD WINAPI mutate_remote_package_graph_pointer( void *arg )
{
    struct package_graph_peb_mutation_context *context = arg;
    ULONG_PTR replacement = 1;

    while (!InterlockedCompareExchange( &context->stop, 0, 0 ))
    {
        SIZE_T written = 0;
        void *value = (void *)replacement;
        NTSTATUS status;

        status = NtWriteVirtualMemory( context->process, context->field,
                                       &value, sizeof(value), &written );
        if (status || written != sizeof(value))
        {
            InterlockedCompareExchange(
                &context->failure, status ? status : STATUS_PARTIAL_COPY, 0 );
            break;
        }
        InterlockedIncrement( &context->writes );
        replacement = replacement ? 0 : 1;
    }
    return 0;
}

static void test_package_graph_process_state(void)
{
    BYTE *first_graph = NULL, *second_graph = NULL, *bad_offset_graph = NULL;
    BYTE *bad_class_graph = NULL;
    BYTE *mutation_graph = NULL;
    struct wine_appx_graph_attach invalid_attach;
    struct package_graph_mutation_context mutation;
    unsigned int first_size = 0, second_size = 0, loader_offset, class_offset;
    unsigned int string_offset;
    unsigned int second_string_offset;
    HANDLE mutation_thread = NULL, parent = NULL, parent_thread = NULL;
    HANDLE process, thread;
    void *snapshot;
    ULONG snapshot_size;
    NTSTATUS status;
    DWORD i, result, wait;
    WCHAR lease_paths[3][MAX_PATH] = {{0}}, temp_path[MAX_PATH];

    if (!winetest_platform_is_wine)
    {
        win_skip( "Package graph transport is Wine-specific.\n" );
        return;
    }

    if (!GetTempPathW( ARRAY_SIZE(temp_path), temp_path ))
    {
        ok( 0, "Failed to resolve the temporary path, error %lu.\n",
            GetLastError() );
        goto done;
    }
    for (i = 0; i < ARRAY_SIZE(package_graph_test_leases); i++)
    {
        package_graph_test_lease[i] = create_package_graph_test_lease(
            temp_path, lease_paths[i], i );
        if (package_graph_test_lease[i] == INVALID_HANDLE_VALUE)
        {
            ok( 0, "Failed to create package graph lease %lu, error %lu.\n",
                i, GetLastError() );
            goto done;
        }
        package_graph_test_leases[i] =
            (ULONG_PTR)package_graph_test_lease[i];
    }

    first_graph = create_test_package_graph( 0x13579bdf, &first_size );
    second_graph = create_test_package_graph( 0x2468ace0, &second_size );
    ok( !!first_graph && !!second_graph, "Failed to build test graphs.\n" );
    if (!first_graph || !second_graph) goto done;

    snapshot = (void *)(ULONG_PTR)1;
    snapshot_size = ~0u;
    status = __wine_get_process_package_graph(
        NtCurrentProcess(), &snapshot, &snapshot_size );
    ok( status == STATUS_NOT_FOUND,
        "Unpackaged current process returned %#lx.\n", status );
    ok( !snapshot && !snapshot_size,
        "Failure returned graph %p, size %lu.\n", snapshot, snapshot_size );

    status = __wine_get_process_package_graph(
        NtCurrentProcess(), NULL, &snapshot_size );
    ok( status == STATUS_INVALID_PARAMETER,
        "NULL graph output returned %#lx.\n", status );
    status = __wine_get_process_package_graph(
        NtCurrentProcess(), &snapshot, NULL );
    ok( status == STATUS_INVALID_PARAMETER,
        "NULL size output returned %#lx.\n", status );

    snapshot = (void *)(ULONG_PTR)1;
    snapshot_size = ~0u;
    status = __wine_get_process_package_graph(
        (HANDLE)(ULONG_PTR)0xdeadbeef, &snapshot, &snapshot_size );
    ok( status == STATUS_INVALID_HANDLE,
        "Invalid process handle returned %#lx.\n", status );
    ok( !snapshot && !snapshot_size,
        "Invalid handle returned graph %p, size %lu.\n",
        snapshot, snapshot_size );

    process = NULL;
    ok( DuplicateHandle( GetCurrentProcess(), GetCurrentProcess(),
                         GetCurrentProcess(), &process, 0, FALSE,
                         DUPLICATE_SAME_ACCESS ),
        "Failed to duplicate current process handle, error %lu.\n",
        GetLastError() );
    if (process)
    {
        CloseHandle( process );
        snapshot = (void *)(ULONG_PTR)1;
        snapshot_size = ~0u;
        status = __wine_get_process_package_graph(
            process, &snapshot, &snapshot_size );
        ok( status == STATUS_INVALID_HANDLE,
            "Closed process handle returned %#lx.\n", status );
        ok( !snapshot && !snapshot_size,
            "Closed handle returned graph %p, size %lu.\n",
            snapshot, snapshot_size );
    }

    status = create_package_graph_test_process(
        L"inspect", 0x13579bdf, first_graph, first_size, NULL,
        &process, &thread );
    ok( !status, "Explicit graph process creation failed %#lx.\n", status );
    if (!status)
    {
        HANDLE denied = NULL;

        result = validate_package_graph_snapshot(
            process, 0x13579bdf, first_graph );
        ok( !result, "Suspended process graph query failed %#lx.\n", result );

        ok( DuplicateHandle( GetCurrentProcess(), process,
                             GetCurrentProcess(), &denied, SYNCHRONIZE,
                             FALSE, 0 ),
            "Failed to create restricted process handle, error %lu.\n",
            GetLastError() );
        if (denied)
        {
            snapshot = (void *)(ULONG_PTR)1;
            snapshot_size = ~0u;
            status = __wine_get_process_package_graph(
                denied, &snapshot, &snapshot_size );
            ok( status == STATUS_ACCESS_DENIED,
                "Restricted process handle returned %#lx.\n", status );
            ok( !snapshot && !snapshot_size,
                "Access denial returned graph %p, size %lu.\n",
                snapshot, snapshot_size );
            CloseHandle( denied );
        }

        result = resume_and_wait_package_graph_child( process, thread );
        ok( !result, "Explicit graph child failed %#lx.\n", result );
        result = validate_package_graph_snapshot(
            process, 0x13579bdf, first_graph );
        ok( !result, "Exited process graph query failed %#lx.\n", result );
        CloseHandle( thread );
        CloseHandle( process );
    }

    /*
     * The creator may close every lease handle immediately after process
     * creation.  Wineserver must keep the generation protected while the
     * child is alive, then release it when the last thread exits even if a
     * process handle remains open for post-mortem graph queries.
     */
    {
        struct wine_appx_graph_attach attach;
        unsigned long long leases[3];
        WCHAR paths[3][MAX_PATH] = {{0}};
        HANDLE lifetime_leases[3] =
            {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
             INVALID_HANDLE_VALUE};
        BOOL ready = TRUE;

        for (i = 0; i < ARRAY_SIZE(lifetime_leases); i++)
        {
            lifetime_leases[i] = create_package_graph_test_lease(
                temp_path, paths[i], i );
            ok( lifetime_leases[i] != INVALID_HANDLE_VALUE,
                "Failed to create lifetime lease %lu, error %lu.\n",
                i, GetLastError() );
            if (lifetime_leases[i] == INVALID_HANDLE_VALUE)
                ready = FALSE;
            else
                leases[i] = (ULONG_PTR)lifetime_leases[i];
        }
        if (ready)
        {
            memset( &attach, 0, sizeof(attach) );
            attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
            attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
            attach.size = first_size;
            attach.blob = (ULONG_PTR)first_graph;
            attach.leases = (ULONG_PTR)leases;
            attach.lease_count = ARRAY_SIZE(leases);
            status = create_package_graph_test_process(
                L"inspect", 0x13579bdf, &attach, 0, NULL,
                &process, &thread );
            ok( !status, "Lease lifetime process creation failed %#lx.\n",
                status );
            for (i = 0; i < ARRAY_SIZE(lifetime_leases); i++)
            {
                CloseHandle( lifetime_leases[i] );
                lifetime_leases[i] = INVALID_HANDLE_VALUE;
            }
            if (!status)
            {
                for (i = 0; i < ARRAY_SIZE(paths); i++)
                {
                    HANDLE probe;

                    SetLastError( 0xdeadbeef );
                    probe = CreateFileW(
                        paths[i], GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
                    ok( probe == INVALID_HANDLE_VALUE &&
                        GetLastError() == ERROR_SHARING_VIOLATION,
                        "Live lease %lu write probe returned %p, error %lu.\n",
                        i, probe, GetLastError() );
                    if (probe != INVALID_HANDLE_VALUE) CloseHandle( probe );

                    SetLastError( 0xdeadbeef );
                    probe = CreateFileW(
                        paths[i], DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
                    ok( probe == INVALID_HANDLE_VALUE &&
                        GetLastError() == ERROR_SHARING_VIOLATION,
                        "Live lease %lu delete probe returned %p, error %lu.\n",
                        i, probe, GetLastError() );
                    if (probe != INVALID_HANDLE_VALUE) CloseHandle( probe );
                }

                result = resume_and_wait_package_graph_child(
                    process, thread );
                ok( !result, "Lease lifetime child failed %#lx.\n",
                    result );
                for (i = 0; i < ARRAY_SIZE(paths); i++)
                {
                    HANDLE probe;

                    probe = CreateFileW(
                        paths[i], GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
                    ok( probe != INVALID_HANDLE_VALUE,
                        "Exited process retained write exclusion %lu, error %lu.\n",
                        i, GetLastError() );
                    if (probe != INVALID_HANDLE_VALUE) CloseHandle( probe );

                    probe = CreateFileW(
                        paths[i], DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );

                    ok( probe != INVALID_HANDLE_VALUE,
                        "Exited process retained lease %lu, error %lu.\n",
                        i, GetLastError() );
                    if (probe != INVALID_HANDLE_VALUE) CloseHandle( probe );
                }
                CloseHandle( thread );
                CloseHandle( process );
            }
        }
        for (i = 0; i < ARRAY_SIZE(lifetime_leases); i++)
        {
            if (lifetime_leases[i] != INVALID_HANDLE_VALUE)
                CloseHandle( lifetime_leases[i] );
            if (paths[i][0])
                ok( DeleteFileW( paths[i] ),
                    "Failed to delete lifetime lease %lu, error %lu.\n",
                    i, GetLastError() );
        }
    }

    status = create_package_graph_test_process(
        L"chain", 0x13579bdf, first_graph, first_size, NULL,
        &process, &thread );
    ok( !status, "Graph inheritance process creation failed %#lx.\n", status );
    if (!status)
    {
        result = resume_and_wait_package_graph_child( process, thread );
        ok( !result, "Ordinary graph inheritance failed %#lx.\n", result );
        CloseHandle( thread );
        CloseHandle( process );
    }

    status = create_package_graph_test_process(
        L"inspect", 0x13579bdf, first_graph, first_size, NULL,
        &parent, &parent_thread );
    ok( !status, "Graph parent creation failed %#lx.\n", status );
    if (!status)
    {
        CloseHandle( parent_thread );
        parent_thread = NULL;
        status = create_package_graph_test_process(
            L"inspect", 0x13579bdf, NULL, 0, parent, &process, &thread );
        ok( status == STATUS_ACCESS_DENIED,
            "Foreign packaged parent inheritance returned %#lx.\n", status );
        if (!status)
        {
            NtTerminateProcess( process, 0 );
            WaitForSingleObject( process, 5000 );
            CloseHandle( thread );
            CloseHandle( process );
        }

        status = create_package_graph_test_process(
            L"inspect", 0x2468ace0, second_graph, second_size,
            parent, &process, &thread );
        ok( !status, "Explicit override process creation failed %#lx.\n", status );
        if (!status)
        {
            result = resume_and_wait_package_graph_child( process, thread );
            ok( !result, "Explicit graph override failed %#lx.\n", result );
            CloseHandle( thread );
            CloseHandle( process );
        }
        NtTerminateProcess( parent, 0 );
        WaitForSingleObject( parent, 5000 );

        status = create_package_graph_test_process(
            L"inspect", 0, NULL, 0, parent, &process, &thread );
        ok( status == STATUS_ACCESS_DENIED,
            "Exited packaged parent inheritance returned %#lx.\n", status );
        if (!status)
        {
            NtTerminateProcess( process, 0 );
            WaitForSingleObject( process, 5000 );
            CloseHandle( thread );
            CloseHandle( process );
        }

        status = create_package_graph_test_process(
            L"inspect", 0x2468ace0, second_graph, second_size,
            parent, &process, &thread );
        ok( !status, "Explicit graph over exited parent failed %#lx.\n",
            status );
        if (!status)
        {
            result = resume_and_wait_package_graph_child( process, thread );
            ok( !result, "Exited-parent graph override failed %#lx.\n",
                result );
            CloseHandle( thread );
            CloseHandle( process );
        }

        CloseHandle( parent );
        parent = NULL;
    }

    status = create_package_graph_test_process(
        L"inspect", 0, NULL, 0, NULL, &process, &thread );
    ok( !status, "Unpackaged process creation failed %#lx.\n", status );
    if (!status)
    {
        snapshot = (void *)(ULONG_PTR)1;
        snapshot_size = ~0u;
        status = __wine_get_process_package_graph(
            process, &snapshot, &snapshot_size );
        ok( status == STATUS_NOT_FOUND,
            "Suspended unpackaged process returned %#lx.\n", status );
        ok( !snapshot && !snapshot_size,
            "Unpackaged process returned graph %p, size %lu.\n",
            snapshot, snapshot_size );
        result = resume_and_wait_package_graph_child( process, thread );
        ok( !result, "Unpackaged child unexpectedly received a graph %#lx.\n",
            result );
        CloseHandle( thread );
        CloseHandle( process );
    }

    /*
     * Package graph snapshots belong to the server process object, not to
     * mutable client PEB state.  Replace the suspended child's PEB pointer
     * while multiple callers concurrently request graph snapshots.
     */
    {
        struct package_graph_query_context query;
        struct package_graph_peb_mutation_context peb_mutation;
        PROCESS_BASIC_INFORMATION basic;
        PEB remote_peb;
        HANDLE query_threads[2] = {NULL, NULL}, peb_mutation_thread = NULL;
        ULONG return_length = 0;
        SIZE_T read = 0;

        memset( &query, 0, sizeof(query) );
        memset( &peb_mutation, 0, sizeof(peb_mutation) );
        memset( &basic, 0, sizeof(basic) );
        memset( &remote_peb, 0, sizeof(remote_peb) );
        status = create_package_graph_test_process(
            L"inspect", 0x13579bdf, first_graph, first_size, NULL,
            &process, &thread );
        ok( !status, "PEB replacement test process creation failed %#lx.\n",
            status );
        if (!status)
        {
            status = NtQueryInformationProcess(
                process, ProcessBasicInformation, &basic, sizeof(basic),
                &return_length );
            ok( !status && return_length == sizeof(basic),
                "ProcessBasicInformation returned %#lx, length %lu.\n",
                status, return_length );
            if (!status)
            {
                status = NtReadVirtualMemory(
                    process, basic.PebBaseAddress, &remote_peb,
                    sizeof(remote_peb), &read );
                ok( !status && read == sizeof(remote_peb),
                    "Reading the remote PEB returned %#lx, size %Iu.\n",
                    status, read );
            }

            if (!status)
            {
                peb_mutation.process = process;
                peb_mutation.field = (void *)(
                    (ULONG_PTR)remote_peb.ProcessParameters +
                    offsetof( RTL_USER_PROCESS_PARAMETERS,
                              PackageDependencyData ));
                peb_mutation_thread = CreateThread(
                    NULL, 0, mutate_remote_package_graph_pointer,
                    &peb_mutation, 0, NULL );
                ok( !!peb_mutation_thread,
                    "Failed to create PEB mutation thread, error %lu.\n",
                    GetLastError() );
            }

            query.process = process;
            query.expected = 0x13579bdf;
            query.iterations = 64;
            for (i = 0; i < ARRAY_SIZE(query_threads); i++)
            {
                query_threads[i] = CreateThread(
                    NULL, 0, query_package_graph_snapshots, &query, 0, NULL );
                ok( !!query_threads[i],
                    "Failed to create graph query thread %lu, error %lu.\n",
                    i, GetLastError() );
            }
            for (i = 0; i < ARRAY_SIZE(query_threads); i++)
            {
                if (!query_threads[i]) continue;
                wait = WaitForSingleObject( query_threads[i], 15000 );
                ok( wait == WAIT_OBJECT_0,
                    "Graph query thread %lu wait returned %#lx.\n", i, wait );
                if (wait != WAIT_OBJECT_0)
                {
                    TerminateThread( query_threads[i], STATUS_TIMEOUT );
                    WaitForSingleObject( query_threads[i], 5000 );
                }
                CloseHandle( query_threads[i] );
            }
            ok( !query.failure,
                "Concurrent graph snapshot validation failed %#lx.\n",
                query.failure );

            if (peb_mutation_thread)
            {
                InterlockedExchange( &peb_mutation.stop, 1 );
                wait = WaitForSingleObject( peb_mutation_thread, 5000 );
                ok( wait == WAIT_OBJECT_0,
                    "PEB mutation thread wait returned %#lx.\n", wait );
                if (wait != WAIT_OBJECT_0)
                {
                    TerminateThread( peb_mutation_thread, STATUS_TIMEOUT );
                    WaitForSingleObject( peb_mutation_thread, 5000 );
                }
                CloseHandle( peb_mutation_thread );
                ok( peb_mutation.writes > 0,
                    "PEB mutation thread did not replace the graph pointer.\n" );
                ok( !peb_mutation.failure,
                    "PEB mutation failed %#lx after %lu writes.\n",
                    peb_mutation.failure, peb_mutation.writes );
            }

            NtTerminateProcess( process, 0 );
            WaitForSingleObject( process, 5000 );
            CloseHandle( thread );
            CloseHandle( process );
        }
    }

    status = create_package_graph_test_process(
        L"inspect", 0, first_graph, WINE_APPX_GRAPH_MAX_BLOB_SIZE + 1,
        NULL, &process, &thread );
    ok( status == STATUS_INVALID_PARAMETER,
        "Oversize graph returned %#lx.\n", status );
    if (!status)
    {
        NtTerminateProcess( process, 0 );
        WaitForSingleObject( process, 5000 );
        CloseHandle( thread );
        CloseHandle( process );
    }

    status = create_package_graph_test_process(
        L"inspect", 0, (const void *)(ULONG_PTR)1, 0,
        NULL, &process, &thread );
    ok( status == STATUS_ACCESS_VIOLATION,
        "Unreadable graph descriptor returned %#lx.\n", status );
    if (!status)
    {
        NtTerminateProcess( process, 0 );
        WaitForSingleObject( process, 5000 );
        CloseHandle( thread );
        CloseHandle( process );
    }

    status = create_package_graph_test_process(
        L"inspect", 0, (const void *)(ULONG_PTR)1,
        WINE_APPX_GRAPH_BLOB_HEADER_SIZE, NULL, &process, &thread );
    ok( status == STATUS_ACCESS_VIOLATION,
        "Unreadable graph blob returned %#lx.\n", status );
    if (!status)
    {
        NtTerminateProcess( process, 0 );
        WaitForSingleObject( process, 5000 );
        CloseHandle( thread );
        CloseHandle( process );
    }

    memset( &invalid_attach, 0, sizeof(invalid_attach) );
    invalid_attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
    invalid_attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
    invalid_attach.size = first_size;
    invalid_attach.blob = (ULONG_PTR)first_graph;
    invalid_attach.leases = (ULONG_PTR)package_graph_test_leases;
    invalid_attach.lease_count =
        ARRAY_SIZE(package_graph_test_leases) - 1;
    status = create_package_graph_test_process(
        L"inspect", 0, &invalid_attach, 0,
        NULL, &process, &thread );
    ok( status == STATUS_INVALID_PARAMETER,
        "Mismatched lease count returned %#lx.\n", status );
    if (!status)
    {
        NtTerminateProcess( process, 0 );
        WaitForSingleObject( process, 5000 );
        CloseHandle( thread );
        CloseHandle( process );
    }

    invalid_attach.leases = 1;
    invalid_attach.lease_count = ARRAY_SIZE(package_graph_test_leases);
    status = create_package_graph_test_process(
        L"inspect", 0, &invalid_attach, 0,
        NULL, &process, &thread );
    ok( status == STATUS_ACCESS_VIOLATION,
        "Unreadable lease array returned %#lx.\n", status );
    if (!status)
    {
        NtTerminateProcess( process, 0 );
        WaitForSingleObject( process, 5000 );
        CloseHandle( thread );
        CloseHandle( process );
    }

    {
        unsigned long long event_leases[3];
        HANDLE event = CreateEventW( NULL, TRUE, FALSE, NULL );

        ok( !!event, "Failed to create invalid lease event, error %lu.\n",
            GetLastError() );
        if (event)
        {
            for (i = 0; i < ARRAY_SIZE(event_leases); i++)
                event_leases[i] = (ULONG_PTR)event;
            invalid_attach.leases = (ULONG_PTR)event_leases;
            invalid_attach.lease_count = ARRAY_SIZE(event_leases);
            status = create_package_graph_test_process(
                L"inspect", 0, &invalid_attach, 0,
                NULL, &process, &thread );
            ok( status == STATUS_OBJECT_TYPE_MISMATCH,
                "Non-file leases returned %#lx.\n", status );
            if (!status)
            {
                NtTerminateProcess( process, 0 );
                WaitForSingleObject( process, 5000 );
                CloseHandle( thread );
                CloseHandle( process );
            }
            CloseHandle( event );
        }
    }

    {
        unsigned long long writable_shared_leases[
            ARRAY_SIZE(package_graph_test_leases)];
        HANDLE writable_shared_lease;

        memcpy( writable_shared_leases, package_graph_test_leases,
                sizeof(writable_shared_leases) );
        writable_shared_lease = CreateFileW(
            lease_paths[0], GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL );
        ok( writable_shared_lease != INVALID_HANDLE_VALUE,
            "Failed to open write-shared lease, error %lu.\n",
            GetLastError() );
        if (writable_shared_lease != INVALID_HANDLE_VALUE)
        {
            writable_shared_leases[0] = (ULONG_PTR)writable_shared_lease;
            memset( &invalid_attach, 0, sizeof(invalid_attach) );
            invalid_attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
            invalid_attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
            invalid_attach.size = first_size;
            invalid_attach.blob = (ULONG_PTR)first_graph;
            invalid_attach.leases = (ULONG_PTR)writable_shared_leases;
            invalid_attach.lease_count =
                ARRAY_SIZE(writable_shared_leases);
            status = create_package_graph_test_process(
                L"inspect", 0, &invalid_attach, 0,
                NULL, &process, &thread );
            ok( status == STATUS_INVALID_PARAMETER,
                "Write-shared lease returned %#lx.\n", status );
            if (!status)
            {
                NtTerminateProcess( process, 0 );
                WaitForSingleObject( process, 5000 );
                CloseHandle( thread );
                CloseHandle( process );
            }
            CloseHandle( writable_shared_lease );
        }
    }

    if (sizeof(void *) == 4)
    {
        memset( &invalid_attach, 0, sizeof(invalid_attach) );
        invalid_attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
        invalid_attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
        invalid_attach.size = WINE_APPX_GRAPH_BLOB_HEADER_SIZE;
        invalid_attach.blob = 1ULL << 32;
        invalid_attach.leases = (ULONG_PTR)package_graph_test_leases;
        invalid_attach.lease_count = ARRAY_SIZE(package_graph_test_leases);
        status = create_package_graph_test_process(
            L"inspect", 0, &invalid_attach, 0,
            NULL, &process, &thread );
        ok( status == STATUS_INVALID_PARAMETER,
            "Non-representable graph pointer returned %#lx.\n", status );
        if (!status)
        {
            NtTerminateProcess( process, 0 );
            WaitForSingleObject( process, 5000 );
            CloseHandle( thread );
            CloseHandle( process );
        }
    }

    mutation_graph = HeapAlloc( GetProcessHeap(), 0, first_size );
    ok( !!mutation_graph, "Failed to allocate graph mutation copy.\n" );
    if (mutation_graph)
    {
        memcpy( mutation_graph, first_graph, first_size );
        graph_write_u32( mutation_graph +
                         WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET,
                         WINE_APPX_GRAPH_BLOB_HEADER_SIZE );
        memset( &mutation, 0, sizeof(mutation) );
        mutation.attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
        mutation.attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
        mutation.attach.size = WINE_APPX_GRAPH_BLOB_HEADER_SIZE;
        mutation.attach.blob = (ULONG_PTR)mutation_graph;
        mutation.attach.leases = (ULONG_PTR)package_graph_test_leases;
        mutation.attach.lease_count =
            ARRAY_SIZE(package_graph_test_leases);
        mutation.blob = mutation_graph;
        mutation.invalid_size = first_size - 2;
        mutation_thread = CreateThread( NULL, 0, mutate_package_graph_attach,
                                        &mutation, 0, NULL );
        ok( !!mutation_thread, "Failed to create graph mutation thread.\n" );
        if (mutation_thread)
        {
            for (i = 0; i < 32; i++)
            {
                status = create_package_graph_test_process(
                    L"inspect", 0, &mutation.attach, 0,
                    NULL, &process, &thread );
                ok( status == STATUS_INVALID_PARAMETER,
                    "Racing malformed graph returned %#lx on iteration %lu.\n",
                    status, i );
                if (!status)
                {
                    NtTerminateProcess( process, 0 );
                    WaitForSingleObject( process, 5000 );
                    CloseHandle( thread );
                    CloseHandle( process );
                }
            }
            InterlockedExchange( &mutation.stop, 1 );
            wait = WaitForSingleObject( mutation_thread, 5000 );
            ok( wait == WAIT_OBJECT_0,
                "Graph mutation thread wait returned %#lx.\n", wait );
            if (wait != WAIT_OBJECT_0)
            {
                TerminateThread( mutation_thread, STATUS_TIMEOUT );
                WaitForSingleObject( mutation_thread, 5000 );
            }
            CloseHandle( mutation_thread );
            mutation_thread = NULL;
        }
    }

    status = create_package_graph_test_process(
        L"inspect", 0, first_graph, first_size - 2,
        NULL, &process, &thread );
    ok( status == STATUS_INVALID_PARAMETER,
        "Truncated graph returned %#lx.\n", status );
    if (!status)
    {
        NtTerminateProcess( process, 0 );
        WaitForSingleObject( process, 5000 );
        CloseHandle( thread );
        CloseHandle( process );
    }

    bad_offset_graph = HeapAlloc( GetProcessHeap(), 0, first_size );
    ok( !!bad_offset_graph, "Failed to allocate malformed graph.\n" );
    if (bad_offset_graph)
    {
        memcpy( bad_offset_graph, first_graph, first_size );
        graph_write_u32( bad_offset_graph + 72, first_size + 2 );
        status = create_package_graph_test_process(
            L"inspect", 0, bad_offset_graph, first_size,
            NULL, &process, &thread );
        ok( status == STATUS_INVALID_PARAMETER,
            "Out-of-range graph offset returned %#lx.\n", status );
        if (!status)
        {
            NtTerminateProcess( process, 0 );
            WaitForSingleObject( process, 5000 );
            CloseHandle( thread );
            CloseHandle( process );
        }
    }

    bad_class_graph = HeapAlloc( GetProcessHeap(), 0, first_size );
    ok( !!bad_class_graph, "Failed to allocate malformed class graph.\n" );
    if (bad_class_graph)
    {
        class_offset = wine_appx_graph_read_u32(
            first_graph + WINE_APPX_GRAPH_HEADER_CLASSES_OFFSET );
        loader_offset = wine_appx_graph_read_u32( first_graph + 56 );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph +
            WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET, 0 );
        ok( wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Rejected a zero application volume serial.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph +
            WINE_APPX_GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET, 0 );
        graph_write_u32(
            bad_class_graph +
            WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET, 0 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a zero application file identity.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        memset( bad_class_graph + WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET, 0,
                WINE_APPX_GRAPH_OBJECT_ID_SIZE );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a zero application object token.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph + WINE_APPX_GRAPH_HEADER_RESERVED_OFFSET, 1 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted nonzero application identity reserved data.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph + loader_offset +
            WINE_APPX_GRAPH_LOADER_VOLUME_SERIAL_OFFSET, 0 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a loader/class volume-serial mismatch.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph + loader_offset +
            WINE_APPX_GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET, 0 );
        graph_write_u32(
            bad_class_graph + loader_offset +
            WINE_APPX_GRAPH_LOADER_FILE_INDEX_LOW_OFFSET, 0 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a zero loader file identity.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        memset( bad_class_graph + loader_offset +
                    WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET,
                0, WINE_APPX_GRAPH_OBJECT_ID_SIZE );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a zero loader object token.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph + loader_offset +
            WINE_APPX_GRAPH_LOADER_RESERVED_OFFSET, 1 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted nonzero loader reserved data.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph + WINE_APPX_GRAPH_BLOB_HEADER_SIZE + 12,
            WINE_APPX_GRAPH_PACKAGE_ACTIVE |
            WINE_APPX_GRAPH_PACKAGE_SIGNED |
            WINE_APPX_GRAPH_PACKAGE_DIRECT );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a direct main package.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32( bad_class_graph + class_offset, 3 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted an out-of-range class package index.\n" );
        status = create_package_graph_test_process(
            L"inspect", 0, bad_class_graph, first_size,
            NULL, &process, &thread );
        ok( status == STATUS_INVALID_PARAMETER,
            "Out-of-range class package index returned %#lx.\n", status );
        if (!status)
        {
            NtTerminateProcess( process, 0 );
            WaitForSingleObject( process, 5000 );
            CloseHandle( thread );
            CloseHandle( process );
        }

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32( bad_class_graph + class_offset + 4, 3 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted an invalid class threading model.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph + class_offset +
            WINE_APPX_GRAPH_CLASS_VOLUME_SERIAL_OFFSET, 0 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a class/loader volume-serial mismatch.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph + class_offset +
            WINE_APPX_GRAPH_CLASS_FILE_INDEX_HIGH_OFFSET, 0 );
        graph_write_u32(
            bad_class_graph + class_offset +
            WINE_APPX_GRAPH_CLASS_FILE_INDEX_LOW_OFFSET, 0 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a zero class file identity.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32(
            bad_class_graph + class_offset +
            WINE_APPX_GRAPH_CLASS_LOADER_INDEX_OFFSET, 1 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted an out-of-range class loader index.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32( bad_class_graph +
                         WINE_APPX_GRAPH_HEADER_CLASS_COUNT_OFFSET,
                         WINE_APPX_GRAPH_MAX_CLASSES + 1 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted an excessive class count.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        graph_write_u32( bad_class_graph +
                         WINE_APPX_GRAPH_HEADER_CLASSES_OFFSET,
                         class_offset + 2 );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a misordered class array offset.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        string_offset = wine_appx_graph_read_u32(
            bad_class_graph + class_offset + 8 );
        graph_write_u16( bad_class_graph + string_offset +
                         (wine_appx_graph_read_u32(
                              bad_class_graph + class_offset + 12 ) - 1) *
                         sizeof(WCHAR), 'X' );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted a class identifier without a terminator.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        string_offset = wine_appx_graph_read_u32(
            bad_class_graph + class_offset + 8 );
        graph_write_u16(
            bad_class_graph + string_offset +
            (wine_appx_graph_read_u32(
                 bad_class_graph + class_offset + 12 ) - 2) * sizeof(WCHAR),
            'C' );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted descending class identifiers.\n" );

        memcpy( bad_class_graph, first_graph, first_size );
        string_offset = wine_appx_graph_read_u32(
            bad_class_graph + class_offset + 8 );
        second_string_offset = wine_appx_graph_read_u32(
            bad_class_graph + class_offset +
            WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE + 8 );
        graph_write_u16( bad_class_graph + string_offset, 'w' );
        memcpy( bad_class_graph + string_offset + sizeof(WCHAR),
                bad_class_graph + second_string_offset + sizeof(WCHAR),
                (wine_appx_graph_read_u32(
                     bad_class_graph + class_offset + 12 ) - 2) *
                sizeof(WCHAR) );
        ok( !wine_appx_graph_validate_blob( bad_class_graph, first_size ),
            "Accepted ASCII case-fold duplicate class identifiers.\n" );
    }

done:
    if (mutation_thread)
    {
        InterlockedExchange( &mutation.stop, 1 );
        wait = WaitForSingleObject( mutation_thread, 5000 );
        if (wait != WAIT_OBJECT_0)
        {
            TerminateThread( mutation_thread, STATUS_TIMEOUT );
            WaitForSingleObject( mutation_thread, 5000 );
        }
        CloseHandle( mutation_thread );
    }
    if (bad_offset_graph)
        HeapFree( GetProcessHeap(), 0, bad_offset_graph );
    if (bad_class_graph)
        HeapFree( GetProcessHeap(), 0, bad_class_graph );
    if (mutation_graph)
        HeapFree( GetProcessHeap(), 0, mutation_graph );
    if (parent_thread) CloseHandle( parent_thread );
    if (parent)
    {
        NtTerminateProcess( parent, 0 );
        WaitForSingleObject( parent, 5000 );
        CloseHandle( parent );
    }
    if (second_graph) HeapFree( GetProcessHeap(), 0, second_graph );
    if (first_graph) HeapFree( GetProcessHeap(), 0, first_graph );
    for (i = 0; i < ARRAY_SIZE(package_graph_test_lease); i++)
    {
        if (package_graph_test_lease[i] != INVALID_HANDLE_VALUE)
        {
            CloseHandle( package_graph_test_lease[i] );
            package_graph_test_lease[i] = INVALID_HANDLE_VALUE;
        }
        if (lease_paths[i][0])
            ok( DeleteFileW( lease_paths[i] ),
                "Failed to delete package graph lease %lu, error %lu.\n",
                i, GetLastError() );
    }
}

static void CALLBACK test_thread_bypass_process_freeze_proc(void *param)
{
    pNtSuspendProcess(NtCurrentProcess());
    /* The current process will be suspended forever here if THREAD_CREATE_FLAGS_BYPASS_PROCESS_FREEZE is nonfunctional. */
    pNtResumeProcess(NtCurrentProcess());
}

static void test_thread_bypass_process_freeze(void)
{
    HANDLE thread;
    NTSTATUS status;

    if (is_wow64 && is_arm64_native_machine)
    {
        skip( "Skipping process suspend test broken under ARM64 WOW64.\n" );
        return;
    }

    if (!pNtCreateThreadEx || !pNtSuspendProcess || !pNtResumeProcess)
    {
        win_skip( "NtCreateThreadEx/NtSuspendProcess/NtResumeProcess are not available.\n" );
        return;
    }

    status = pNtCreateThreadEx( &thread, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), test_thread_bypass_process_freeze_proc,
                                NULL, THREAD_CREATE_FLAGS_BYPASS_PROCESS_FREEZE, 0, 0, 0, NULL );
    ok( status == STATUS_SUCCESS ||
        broken(status == STATUS_INVALID_PARAMETER_7) /* <= Win10-1809 */,
        "Got unexpected status %#lx.\n", status );

    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
}

static void CALLBACK apc_func( ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3 )
{
}

static void test_NtQueueApcThreadEx(void)
{
    NTSTATUS status, expected;
    HANDLE reserve;

    if (!pNtQueueApcThreadEx)
    {
        win_skip( "NtQueueApcThreadEx is not available.\n" );
        return;
    }

    status = pNtQueueApcThreadEx( GetCurrentThread(), (HANDLE)QUEUE_USER_APC_CALLBACK_DATA_CONTEXT, apc_func, 0x1234, 0x5678, 0xdeadbeef );
    ok( status == STATUS_INVALID_HANDLE, "got %#lx, expected %#lx.\n", status, STATUS_INVALID_HANDLE );

    status = pNtQueueApcThreadEx( GetCurrentThread(), (HANDLE)QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC, apc_func, 0x1234, 0x5678, 0xdeadbeef );
    todo_wine_if(old_wow64)
    ok( status == STATUS_SUCCESS || status == STATUS_INVALID_HANDLE /* wow64 and win64 on Win version before Win10 1809 */,
        "got %#lx.\n", status );

    status = pNtQueueApcThreadEx( GetCurrentThread(), GetCurrentThread(), apc_func, 0x1234, 0x5678, 0xdeadbeef );
    ok( status == STATUS_OBJECT_TYPE_MISMATCH, "got %#lx.\n", status );

    status = pNtAllocateReserveObject( &reserve, NULL, MemoryReserveObjectTypeUserApc );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    status = pNtQueueApcThreadEx( GetCurrentThread(), reserve, apc_func, 0x1234, 0x5678, 0xdeadbeef );
    ok( !status, "got %#lx.\n", status );
    status = pNtQueueApcThreadEx( GetCurrentThread(), reserve, apc_func, 0x1234, 0x5678, 0xdeadbeef );
    ok( status == STATUS_INVALID_PARAMETER_2, "got %#lx.\n", status );
    SleepEx( 0, TRUE );
    status = pNtQueueApcThreadEx( GetCurrentThread(), reserve, apc_func, 0x1234, 0x5678, 0xdeadbeef );
    ok( !status, "got %#lx.\n", status );

    NtClose( reserve );

    status = pNtAllocateReserveObject( &reserve, NULL, MemoryReserveObjectTypeIoCompletion );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );
    status = pNtQueueApcThreadEx( GetCurrentThread(), reserve, apc_func, 0x1234, 0x5678, 0xdeadbeef );
    ok( status == STATUS_OBJECT_TYPE_MISMATCH, "got %#lx.\n", status );
    NtClose( reserve );

    SleepEx( 0, TRUE );

    if (!pNtQueueApcThreadEx2)
    {
        win_skip( "NtQueueApcThreadEx2 is not available.\n" );
        return;
    }
    expected = is_wow64 ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    status = pNtQueueApcThreadEx2( GetCurrentThread(), NULL, QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC, apc_func, 0x1234, 0x5678, 0xdeadbeef );
    ok( status == expected, "got %#lx, expected %#lx.\n", status, expected );

    status = pNtQueueApcThreadEx2( GetCurrentThread(), (HANDLE)QUEUE_USER_APC_CALLBACK_DATA_CONTEXT, 0, apc_func, 0x1234, 0x5678, 0xdeadbeef );
    ok( status == STATUS_INVALID_HANDLE, "got %#lx.\n", status );

    SleepEx( 0, TRUE );
}

static void extract_resource(const char *name, const char *type, const char *path)
{
    DWORD written;
    HANDLE file;
    HRSRC res;
    void *ptr;

    file = CreateFileA(path, GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, 0);
    ok(file != INVALID_HANDLE_VALUE, "file creation failed, at %s, error %ld\n", path, GetLastError());

    res = FindResourceA(NULL, name, type);
    ok( res != 0, "couldn't find resource\n" );
    ptr = LockResource( LoadResource( GetModuleHandleA(NULL), res ));
    WriteFile( file, ptr, SizeofResource( GetModuleHandleA(NULL), res ), &written, NULL );
    ok( written == SizeofResource( GetModuleHandleA(NULL), res ), "couldn't write resource\n" );
    CloseHandle( file );
}

struct skip_thread_attach_args
{
    BOOL teb_flag;
    PVOID teb_tls_pointer;
    PVOID teb_fls_slots;
};

static void CALLBACK test_skip_thread_attach_proc(void *param)
{
    struct skip_thread_attach_args *args = param;
    args->teb_flag = NtCurrentTeb()->SkipThreadAttach;
    args->teb_tls_pointer = NtCurrentTeb()->ThreadLocalStoragePointer;
    args->teb_fls_slots = NtCurrentTeb()->FlsSlots;
}

static void test_skip_thread_attach(void)
{
    BOOL *seen_thread_attach, *seen_thread_detach;
    struct skip_thread_attach_args args;
    HANDLE thread;
    NTSTATUS status;
    char path_dll_local[MAX_PATH + 11];
    char path_tmp[MAX_PATH];
    HMODULE module;

    if (!pNtCreateThreadEx)
    {
        win_skip( "NtCreateThreadEx is not available.\n" );
        return;
    }

    GetTempPathA(sizeof(path_tmp), path_tmp);

    sprintf(path_dll_local, "%s%s", path_tmp, "testdll.dll");
    extract_resource("testdll.dll", "TESTDLL", path_dll_local);

    module = LoadLibraryA(path_dll_local);
    if (!module) {
        trace("Could not load testdll.\n");
        goto delete;
    }

    seen_thread_attach = (BOOL *)GetProcAddress(module, "seen_thread_attach");
    seen_thread_detach = (BOOL *)GetProcAddress(module, "seen_thread_detach");

    ok( !*seen_thread_attach, "Unexpected\n" );
    ok( !*seen_thread_detach, "Unexpected\n" );

    status = pNtCreateThreadEx( &thread, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), test_skip_thread_attach_proc,
                                &args, THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH, 0, 0, 0, NULL );
    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );

    WaitForSingleObject( thread, INFINITE );

    CloseHandle( thread );

    ok( !*seen_thread_attach, "Unexpected\n" );
    ok( !*seen_thread_detach, "Unexpected\n" );
    ok( args.teb_flag, "Unexpected\n" );
    ok( !args.teb_tls_pointer, "Unexpected\n" );
    ok( !args.teb_fls_slots, "Unexpected\n" );

    FreeLibrary(module);
delete:
    DeleteFileA(path_dll_local);
}

struct test_arm64_skip_load_init_args
{
    USHORT teb_same_teb_flags;
};

static void test_arm64_skip_loader_init(void)
{
    static ULONG native_code[] =
    {
        0xd282fdc2, /* mov x2, #0x17ee */
        0x78626a41, /* ldrh w1, [x18, x2]   (NtCurrentTeb()->SameTebFlags) */
        0x79000001, /* strh w1, [x0]        (args->teb_same_teb_flags)     */
        0xd65f03c0, /* ret */
    };

    struct test_arm64_skip_load_init_args args;
    HANDLE thread;
    NTSTATUS status;
    void *code_mem = NULL;
#ifdef __x86_64__
    MEM_EXTENDED_PARAMETER param = { 0 };
    SIZE_T code_size = 0x10000;

    param.Type = MemExtendedParameterAttributeFlags;
    param.ULong64 = MEM_EXTENDED_PARAMETER_EC_CODE;
    if (!pNtAllocateVirtualMemoryEx ||
        pNtAllocateVirtualMemoryEx( GetCurrentProcess(), &code_mem, &code_size, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_EXECUTE_READWRITE, &param, 1 ))
    {
        trace("NtAllocateVirtualMemoryEx failed\n");
        return;

    }
#else
    code_mem = VirtualAlloc( NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE );
    if (!code_mem)
    {
        trace("VirtualAlloc failed\n");
        return;
    }
#endif
    if (!pNtCreateThreadEx)
    {
        win_skip( "NtCreateThreadEx is not available.\n" );
        return;
    }

    memcpy( code_mem, native_code, sizeof(native_code) );

    status = pNtCreateThreadEx( &thread, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), (PRTL_THREAD_START_ROUTINE)code_mem,
                                &args, THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH | THREAD_CREATE_FLAGS_SKIP_LOADER_INIT, 0, 0, 0, NULL );

    ok( status == STATUS_SUCCESS, "Got unexpected status %#lx.\n", status );

    WaitForSingleObject( thread, INFINITE );

    ok( (args.teb_same_teb_flags & 0x4008) == 0x4008, "wrong value %x\n", args.teb_same_teb_flags );

    CloseHandle( thread );
}

START_TEST(thread)
{
    char **argv;
    int argc = winetest_get_mainargs( &argv );

    if (argc >= 3 && !strcmp( argv[2], "graph-child" ))
        ExitProcess( package_graph_child( argc, argv ) );

    init_function_pointers();

    if (!pIsWow64Process || !pIsWow64Process( GetCurrentProcess(), &is_wow64 )) is_wow64 = FALSE;
    if (is_wow64)
    {
        TEB64 *teb64 = ULongToPtr( NtCurrentTeb()->GdiBatchCount );

        if (teb64)
        {
            PEB64 *peb64 = ULongToPtr(teb64->Peb);
            old_wow64 = !peb64->LdrData;
        }
    }

    if (pRtlWow64GetProcessMachines)
    {
        USHORT current, native;
        is_arm64_native_machine = !pRtlWow64GetProcessMachines( GetCurrentProcess(), &current, &native ) &&
                                  native == IMAGE_FILE_MACHINE_ARM64;
        if (is_arm64_native_machine) test_arm64_skip_loader_init();
    }

    test_dbg_hidden_thread_creation();
    test_unique_teb();
    test_errno();
    test_NtCreateUserProcess();
    test_package_graph_process_state();
    test_thread_bypass_process_freeze();
    test_NtQueueApcThreadEx();
    test_skip_thread_attach();
}
