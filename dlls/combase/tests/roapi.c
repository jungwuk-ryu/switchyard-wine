/*
 * Copyright (c) 2018 Alistair Leslie-Hughes
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
#define COBJMACROS
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winioctl.h"
#include "winstring.h"
#include "winternl.h"

#include "initguid.h"
#include "roapi.h"
#include "roerrorapi.h"

#include "wine/appx_package_graph.h"
#include "wine/test.h"

#define EXPECT_REF(obj,ref) _expect_ref((IUnknown*)obj, ref, __LINE__)
static void _expect_ref(IUnknown* obj, ULONG ref, int line)
{
    ULONG rc;
    IUnknown_AddRef(obj);
    rc = IUnknown_Release(obj);
    ok_(__FILE__,line)(rc == ref, "expected refcount %ld, got %ld\n", ref, rc);
}

static void flush_events(void)
{
    int diff = 200;
    DWORD time;
    MSG msg;

    time = GetTickCount() + diff;
    while (diff > 0)
    {
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT) == WAIT_TIMEOUT)
            break;
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        diff = time - GetTickCount();
    }
}

static void load_resource(const WCHAR *filename)
{
    DWORD written = 0;
    HANDLE file;
    HRSRC res;
    void *ptr;

    file = CreateFileW(filename, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, 0);
    ok(file != INVALID_HANDLE_VALUE, "failed to create %s, error %lu\n", debugstr_w(filename), GetLastError());

    res = FindResourceW(NULL, filename, L"TESTDLL");
    ok(res != 0, "couldn't find resource\n");
    ptr = LockResource(LoadResource(GetModuleHandleW(NULL), res));
    WriteFile(file, ptr, SizeofResource(GetModuleHandleW(NULL), res), &written, NULL);
    ok(written == SizeofResource(GetModuleHandleW(NULL), res), "couldn't write resource\n");
    CloseHandle(file);
}

static void graph_write_u16(BYTE *data, UINT32 value)
{
    data[0] = value;
    data[1] = value >> 8;
}

static void graph_write_u32(BYTE *data, UINT32 value)
{
    graph_write_u16(data, value);
    graph_write_u16(data + 2, value >> 16);
}

static void graph_write_u64(BYTE *data, UINT64 value)
{
    graph_write_u32(data, value);
    graph_write_u32(data + 4, value >> 32);
}

static const WCHAR test_package_full_name[] =
    L"Wine.Package_1.0.0.0_neutral__pub";

static BOOL write_package_marker(const WCHAR *path)
{
    BYTE marker[40 + sizeof(test_package_full_name)];
    DWORD written;
    HANDLE file;
    UINT32 i;
    BOOL ret;

    memcpy(marker, "SWLM", 4);
    graph_write_u32(marker + 4, 1);
    for (i = 0; i < 32; i++) marker[8 + i] = i + 1;
    for (i = 0; i < ARRAY_SIZE(test_package_full_name); i++)
        graph_write_u16(marker + 40 + i * sizeof(WCHAR),
                        test_package_full_name[i]);

    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ret = WriteFile(file, marker, sizeof(marker), &written, NULL);
    if (ret && written != sizeof(marker))
    {
        SetLastError(ERROR_WRITE_FAULT);
        ret = FALSE;
    }
    if (ret) ret = FlushFileBuffers(file);
    CloseHandle(file);
    return ret;
}

static BOOL graph_append_string(BYTE *graph, UINT32 capacity, UINT32 *position,
                                UINT32 ref_offset, const WCHAR *string)
{
    UINT32 chars = lstrlenW(string) + 1, i;

    if (*position > capacity ||
        chars > WINE_APPX_GRAPH_MAX_STRING_CHARS + 1 ||
        chars > (capacity - *position) / sizeof(WCHAR))
        return FALSE;
    graph_write_u32(graph + ref_offset, *position);
    graph_write_u32(graph + ref_offset + 4, chars);
    for (i = 0; i < chars; i++)
        graph_write_u16(graph + *position + i * sizeof(WCHAR), string[i]);
    *position += chars * sizeof(WCHAR);
    return TRUE;
}

static BOOL get_file_identity(const WCHAR *path, DWORD *volume_serial,
                              DWORD *file_index_high, DWORD *file_index_low,
                              BYTE object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE],
                              UINT64 *change_time, UINT64 *file_size)
{
    BY_HANDLE_FILE_INFORMATION info;
    FILE_OBJECTID_BUFFER native_id;
    FILE_STANDARD_INFORMATION standard;
    FILE_BASIC_INFORMATION basic;
    IO_STATUS_BLOCK io;
    HANDLE file;
    BOOL ret;
    UINT32 i;

    file = CreateFileW(path, FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ret = GetFileInformationByHandle(file, &info);
    if (ret && object_id &&
        NtFsControlFile(file, 0, NULL, NULL, &io, FSCTL_GET_OBJECT_ID,
                        NULL, 0, &native_id, sizeof(native_id)))
        ret = FALSE;
    if (ret && change_time &&
        NtQueryInformationFile(file, &io, &basic, sizeof(basic),
                               FileBasicInformation))
        ret = FALSE;
    if (ret && file_size &&
        NtQueryInformationFile(file, &io, &standard, sizeof(standard),
                               FileStandardInformation))
        ret = FALSE;
    if (ret && object_id)
    {
        for (i = 0; i < WINE_APPX_GRAPH_OBJECT_ID_SIZE; i++)
            if (native_id.ObjectId[i]) break;
        if (i == WINE_APPX_GRAPH_OBJECT_ID_SIZE) ret = FALSE;
    }
    CloseHandle(file);
    if (!ret || (!info.nFileIndexHigh && !info.nFileIndexLow) ||
        (change_time && basic.ChangeTime.QuadPart <= 0) ||
        (file_size && standard.EndOfFile.QuadPart <= 0))
        return FALSE;
    *volume_serial = info.dwVolumeSerialNumber;
    *file_index_high = info.nFileIndexHigh;
    *file_index_low = info.nFileIndexLow;
    if (object_id) memcpy(object_id, native_id.ObjectId, WINE_APPX_GRAPH_OBJECT_ID_SIZE);
    if (change_time) *change_time = basic.ChangeTime.QuadPart;
    if (file_size) *file_size = standard.EndOfFile.QuadPart;
    return TRUE;
}

static BYTE *build_package_activation_graph(UINT32 threading_model,
                                            BOOL mismatched_identity,
                                            BOOL invalid_class_path,
                                            HANDLE lease, UINT32 *graph_size)
{
    static const UINT32 capacity = 64 * 1024;
    static const WCHAR package_dll[] = L"wine.combase.test.dll";
    static const WCHAR shadow_dll[] =
        L"zzshadow\\wine.combase.test.dll";
    static const WCHAR explicit_dll[] =
        L"shadow\\wine.combase.test.dll";
    static const WCHAR invalid_path[] = L"..\\wine.combase.test.dll";
    const UINT32 package_offset = WINE_APPX_GRAPH_BLOB_HEADER_SIZE;
    const UINT32 loader_offset = package_offset +
        WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;
    const UINT32 class_offset = loader_offset +
        3 * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
    const UINT32 strings_offset = class_offset +
        WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE;
    BY_HANDLE_FILE_INFORMATION lease_info;
    DWORD app_volume, app_high, app_low;
    DWORD dll_volume, dll_high, dll_low;
    BYTE app_object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
    BYTE dll_object_id[WINE_APPX_GRAPH_OBJECT_ID_SIZE];
    UINT64 dll_change_time, dll_file_size;
    DWORD class_low;
    WCHAR module[MAX_PATH], root[MAX_PATH];
    UINT32 position = strings_offset, i;
    BYTE *graph;

    if (!GetModuleFileNameW(NULL, module, ARRAY_SIZE(module)) ||
        !GetCurrentDirectoryW(ARRAY_SIZE(root), root) ||
        !get_file_identity(module, &app_volume, &app_high, &app_low,
                           app_object_id, NULL, NULL) ||
        !get_file_identity(package_dll, &dll_volume, &dll_high, &dll_low,
                           dll_object_id, &dll_change_time, &dll_file_size) ||
        !GetFileInformationByHandle(lease, &lease_info))
        return NULL;
    i = lstrlenW(root);
    if (i > 3 && root[i - 1] == '\\') root[i - 1] = 0;

    graph = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity);
    if (!graph) return NULL;
    memcpy(graph, "SWXGRAPH", 8);
    graph_write_u32(graph + 8, WINE_APPX_GRAPH_BLOB_VERSION);
    graph_write_u32(graph + 12, WINE_APPX_GRAPH_BLOB_HEADER_SIZE);
    graph_write_u64(graph + 24, 1);
    graph_write_u64(graph + 32, 1);
    graph_write_u32(graph + 40, 0);
    graph_write_u32(graph + 44, 1);
    graph_write_u32(graph + 48, package_offset);
    graph_write_u32(graph + 52, 3);
    graph_write_u32(graph + 56, loader_offset);
    graph_write_u32(graph + 60, strings_offset);
    graph_write_u32(graph + 68, 1);
    graph_write_u32(graph + WINE_APPX_GRAPH_HEADER_CLASS_COUNT_OFFSET, 1);
    graph_write_u32(graph + WINE_APPX_GRAPH_HEADER_CLASSES_OFFSET, class_offset);
    graph_write_u32(graph + WINE_APPX_GRAPH_HEADER_VOLUME_SERIAL_OFFSET, app_volume);
    graph_write_u32(graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_HIGH_OFFSET, app_high);
    graph_write_u32(graph + WINE_APPX_GRAPH_HEADER_FILE_INDEX_LOW_OFFSET, app_low);
    memcpy(graph + WINE_APPX_GRAPH_HEADER_OBJECT_ID_OFFSET, app_object_id,
           sizeof(app_object_id));

    graph_write_u64(graph + package_offset, 1);
    graph_write_u32(graph + package_offset + 8, 0);
    graph_write_u32(graph + package_offset + 12,
                    WINE_APPX_GRAPH_PACKAGE_ACTIVE |
                    WINE_APPX_GRAPH_PACKAGE_SIGNED);
    graph_write_u32(graph + package_offset + 16, 0);
    for (i = 0; i < 32; i++) graph[package_offset + 24 + i] = i + 1;

    for (i = 0; i < 3; i++)
    {
        BYTE *loader = graph + loader_offset +
                       i * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;

        graph_write_u32(loader, 0);
        graph_write_u32(
            loader + WINE_APPX_GRAPH_LOADER_SEARCH_RANK_OFFSET,
            i < 2 ? 1 : WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY);
        graph_write_u32(
            loader + WINE_APPX_GRAPH_LOADER_VOLUME_SERIAL_OFFSET,
            dll_volume);
        graph_write_u32(
            loader + WINE_APPX_GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET,
            dll_high);
        graph_write_u32(
            loader + WINE_APPX_GRAPH_LOADER_FILE_INDEX_LOW_OFFSET,
            dll_low);
        graph_write_u64(
            loader + WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET,
            dll_change_time);
        graph_write_u64(
            loader + WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET,
            dll_file_size);
        memcpy(loader + WINE_APPX_GRAPH_LOADER_OBJECT_ID_OFFSET,
               dll_object_id, sizeof(dll_object_id));
    }

    graph_write_u32(graph + class_offset, 0);
    graph_write_u32(graph + class_offset + 4, threading_model);
    graph_write_u32(graph + class_offset +
                    WINE_APPX_GRAPH_CLASS_VOLUME_SERIAL_OFFSET, dll_volume);
    graph_write_u32(graph + class_offset +
                    WINE_APPX_GRAPH_CLASS_FILE_INDEX_HIGH_OFFSET, dll_high);
    class_low = dll_low;
    if (mismatched_identity)
    {
        class_low ^= 1;
        if (!class_low && !dll_high) class_low = 2;
    }
    graph_write_u32(graph + class_offset +
                    WINE_APPX_GRAPH_CLASS_FILE_INDEX_LOW_OFFSET,
                    class_low);
    graph_write_u32(graph + class_offset +
                    WINE_APPX_GRAPH_CLASS_LOADER_INDEX_OFFSET, 0);
    graph_write_u64(graph + class_offset +
                    WINE_APPX_GRAPH_CLASS_CHANGE_TIME_OFFSET,
                    dll_change_time);
    graph_write_u64(graph + class_offset +
                    WINE_APPX_GRAPH_CLASS_FILE_SIZE_OFFSET,
                    dll_file_size);

    if (!graph_append_string(graph, capacity, &position, 72, L"App") ||
        !graph_append_string(graph, capacity, &position, 80,
                             L"Wine.Package_pub!App") ||
        !graph_append_string(graph, capacity, &position, 88, module) ||
        !graph_append_string(graph, capacity, &position, 96, L"") ||
        !graph_append_string(graph, capacity, &position, package_offset + 56,
                             L"Wine.Package") ||
        !graph_append_string(graph, capacity, &position, package_offset + 64,
                             L"CN=Wine") ||
        !graph_append_string(graph, capacity, &position, package_offset + 72, L"") ||
        !graph_append_string(graph, capacity, &position, package_offset + 80,
                             L"pub") ||
        !graph_append_string(graph, capacity, &position, package_offset + 88,
                             test_package_full_name) ||
        !graph_append_string(graph, capacity, &position, package_offset + 96,
                             L"Wine.Package_pub") ||
        !graph_append_string(graph, capacity, &position, package_offset + 104,
                             root) ||
        !graph_append_string(graph, capacity, &position, loader_offset + 8,
                             package_dll) ||
        !graph_append_string(graph, capacity, &position, loader_offset + 16,
                             package_dll) ||
        !graph_append_string(
            graph, capacity, &position,
            loader_offset + WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE + 8,
            package_dll) ||
        !graph_append_string(
            graph, capacity, &position,
            loader_offset + WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE + 16,
            shadow_dll) ||
        !graph_append_string(
            graph, capacity, &position,
            loader_offset + 2 * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE + 8,
            package_dll) ||
        !graph_append_string(
            graph, capacity, &position,
            loader_offset + 2 * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE + 16,
            explicit_dll) ||
        !graph_append_string(graph, capacity, &position, class_offset + 8,
                             L"Wine.Package.Test") ||
        !graph_append_string(graph, capacity, &position, class_offset + 16,
                             invalid_class_path ? invalid_path : package_dll))
        goto invalid;

    graph_write_u32(graph + 16, position);
    graph_write_u32(graph + 64, position - strings_offset);
    if (!wine_appx_graph_validate_blob(graph, position)) goto invalid;
    *graph_size = position;
    return graph;

invalid:
    HeapFree(GetProcessHeap(), 0, graph);
    return NULL;
}

static DWORD package_activation_child(const char *mode)
{
    static const WCHAR embedded_classid[] = L"Wine.Package.Test\0suffix";
    IActivationFactory *factory = NULL;
    RO_INIT_TYPE init_type = RO_INIT_MULTITHREADED;
    const WCHAR *expected_apartment = NULL;
    HRESULT expected = S_OK, hr;
    HSTRING classid;

    if (!strcmp(mode, "sta") || !strcmp(mode, "wrong-mta"))
        init_type = RO_INIT_SINGLETHREADED;
    if (!strcmp(mode, "sta") || !strcmp(mode, "wrong-sta"))
        expected_apartment = L"sta";
    else if (!strcmp(mode, "mta") || !strcmp(mode, "wrong-mta"))
        expected_apartment = L"mta";
    if (!strcmp(mode, "embedded-null"))
        expected = E_INVALIDARG;
    else if (!strcmp(mode, "bad-identity") || !strcmp(mode, "bad-path"))
        expected = HRESULT_FROM_WIN32(APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT);

    hr = RoInitialize(init_type);
    if (FAILED(hr)) return 0x10000 | (hr & 0xffff);
    if (!strcmp(mode, "embedded-null"))
        hr = WindowsCreateString(embedded_classid,
                                 ARRAY_SIZE(embedded_classid) - 1, &classid);
    else
        hr = WindowsCreateString(L"Wine.Package.Test",
                                 ARRAY_SIZE(L"Wine.Package.Test") - 1,
                                 &classid);
    if (FAILED(hr))
    {
        RoUninitialize();
        return 0x20000 | (hr & 0xffff);
    }
    if (expected_apartment &&
        !SetEnvironmentVariableW(L"WINE_COMBASE_TEST_APARTMENT",
                                 expected_apartment))
    {
        WindowsDeleteString(classid);
        RoUninitialize();
        return 0x21000 | (GetLastError() & 0xfff);
    }
    hr = RoGetActivationFactory(classid, &IID_IActivationFactory,
                                (void **)&factory);
    SetEnvironmentVariableW(L"WINE_COMBASE_TEST_APARTMENT", NULL);
    if (factory) IActivationFactory_Release(factory);
    WindowsDeleteString(classid);
    RoUninitialize();
    if (hr != expected) return 0x30000 | (hr & 0xffff);
    if (SUCCEEDED(expected) && !factory) return 0x40000;
    if (FAILED(expected) && factory) return 0x50000;
    return 0;
}

static DWORD run_package_activation_child(const char *mode, UINT32 threading_model,
                                          BOOL mismatched_identity,
                                          BOOL invalid_class_path)
{
    STARTUPINFOEXW startup = {{sizeof(startup)}};
    struct wine_appx_graph_attach attach = {0};
    PROCESS_INFORMATION process_info;
    unsigned long long lease_value;
    WCHAR temp_path[MAX_PATH], lease_path[MAX_PATH];
    WCHAR module[MAX_PATH], command[MAX_PATH + 80];
    BYTE *graph = NULL;
    UINT32 graph_size;
    SIZE_T attr_size = 0;
    HANDLE lease = INVALID_HANDLE_VALUE;
    DWORD exit_code = ~0u, wait;
    BOOL ret;

    if (!GetTempPathW(ARRAY_SIZE(temp_path), temp_path) ||
        !GetTempFileNameW(temp_path, L"cba", 0, lease_path))
        return HRESULT_FROM_WIN32(GetLastError());
    if (!write_package_marker(lease_path))
    {
        exit_code = HRESULT_FROM_WIN32(GetLastError());
        DeleteFileW(lease_path);
        return exit_code;
    }
    lease = CreateFileW(lease_path, GENERIC_READ | DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lease == INVALID_HANDLE_VALUE)
    {
        DeleteFileW(lease_path);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    graph = build_package_activation_graph(threading_model, mismatched_identity,
                                           invalid_class_path, lease, &graph_size);
    if (!graph)
    {
        exit_code = ERROR_INVALID_DATA;
        goto done;
    }

    lease_value = (ULONG_PTR)lease;
    attach.tag = WINE_APPX_GRAPH_ATTACH_TAG;
    attach.version = WINE_APPX_GRAPH_ATTACH_VERSION;
    attach.size = graph_size;
    attach.blob = (ULONG_PTR)graph;
    attach.leases = (ULONG_PTR)&lease_value;
    attach.lease_count = 1;

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    if (!(startup.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attr_size)))
    {
        exit_code = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                           &attr_size) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                   WINE_PROC_THREAD_ATTRIBUTE_PACKAGE_GRAPH,
                                   &attach, sizeof(attach), NULL, NULL))
    {
        exit_code = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    if (!GetModuleFileNameW(NULL, module, ARRAY_SIZE(module)) ||
        swprintf(command, ARRAY_SIZE(command), L"\"%s\" roapi package-child %hs",
                 module, mode) < 0)
    {
        exit_code = ERROR_FILENAME_EXCED_RANGE;
        goto done;
    }

    memset(&process_info, 0, sizeof(process_info));
    ret = CreateProcessW(module, command, NULL, NULL, FALSE,
                         EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                         &startup.StartupInfo, &process_info);
    if (!ret)
    {
        exit_code = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    wait = WaitForSingleObject(process_info.hProcess, 10000);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(process_info.hProcess, STATUS_TIMEOUT);
        WaitForSingleObject(process_info.hProcess, 5000);
        exit_code = STATUS_TIMEOUT;
    }
    else if (!GetExitCodeProcess(process_info.hProcess, &exit_code))
        exit_code = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

done:
    if (startup.lpAttributeList)
    {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    }
    HeapFree(GetProcessHeap(), 0, graph);
    if (lease != INVALID_HANDLE_VALUE) CloseHandle(lease);
    DeleteFileW(lease_path);
    return exit_code;
}

static void test_package_activation(void)
{
    DWORD result;

    result = run_package_activation_child("both", 0, FALSE, FALSE);
    ok(!result, "Both-threaded package activation child failed %#lx.\n", result);
    result = run_package_activation_child("sta", 1, FALSE, FALSE);
    ok(!result, "STA package activation child failed %#lx.\n", result);
    result = run_package_activation_child("mta", 2, FALSE, FALSE);
    ok(!result, "MTA package activation child failed %#lx.\n", result);
    result = run_package_activation_child("embedded-null", 0, FALSE, FALSE);
    ok(!result, "Embedded-NUL package activation child failed %#lx.\n", result);
    result = run_package_activation_child("wrong-sta", 1, FALSE, FALSE);
    ok(!result, "Wrong-apartment STA child failed %#lx.\n", result);
    result = run_package_activation_child("wrong-mta", 2, FALSE, FALSE);
    ok(!result, "Wrong-apartment MTA child failed %#lx.\n", result);
    result = run_package_activation_child("bad-identity", 0, TRUE, FALSE);
    ok(result == ERROR_INVALID_DATA,
       "Class/loader identity mismatch returned %#lx.\n", result);
    result = run_package_activation_child("bad-path", 0, FALSE, TRUE);
    ok(result == ERROR_INVALID_DATA,
       "Invalid class path returned %#lx.\n", result);
}

static void test_ActivationFactories(void)
{
    HRESULT hr;
    HSTRING str, str2;
    IActivationFactory *factory = NULL;
    IInspectable *inspect = NULL;
    ULONG ref;

    hr = WindowsCreateString(L"Windows.Data.Xml.Dom.XmlDocument",
                              ARRAY_SIZE(L"Windows.Data.Xml.Dom.XmlDocument") - 1, &str);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = WindowsCreateString(L"Does.Not.Exist", ARRAY_SIZE(L"Does.Not.Exist") - 1, &str2);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

    hr = RoGetActivationFactory(str2, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "Unexpected hr %#lx.\n", hr);

    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    todo_wine ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
    if(factory)
        IActivationFactory_Release(factory);

    hr = RoActivateInstance(str2, &inspect);
    ok(hr == REGDB_E_CLASSNOTREG, "Unexpected hr %#lx.\n", hr);

    hr = RoActivateInstance(str, &inspect);
    todo_wine ok(hr == S_OK, "UNexpected hr %#lx.\n", hr);
    if(inspect)
        IInspectable_Release(inspect);

    WindowsDeleteString(str2);
    WindowsDeleteString(str);

    hr = WindowsCreateString(L"Wine.Test.Missing", ARRAY_SIZE(L"Wine.Test.Missing") - 1, &str);
    ok(hr == S_OK, "WindowsCreateString returned %#lx.\n", hr);
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "RoGetActivationFactory returned %#lx.\n", hr);
    ok(factory == NULL, "got factory %p.\n", factory);
    WindowsDeleteString(str);
    hr = WindowsCreateString(L"Wine.Test.Class", ARRAY_SIZE(L"Wine.Test.Class") - 1, &str);
    ok(hr == S_OK, "WindowsCreateString returned %#lx.\n", hr);
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    todo_wine
    ok(hr == E_NOTIMPL || broken(hr == REGDB_E_CLASSNOTREG) /* <= w1064v1809 */,
            "RoGetActivationFactory returned %#lx.\n", hr);
    todo_wine
    ok(factory == NULL, "got factory %p.\n", factory);
    if (factory) IActivationFactory_Release(factory);
    WindowsDeleteString(str);
    hr = WindowsCreateString(L"Wine.Test.Trusted", ARRAY_SIZE(L"Wine.Test.Trusted") - 1, &str);
    ok(hr == S_OK, "WindowsCreateString returned %#lx.\n", hr);
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == S_OK || broken(hr == REGDB_E_CLASSNOTREG) /* <= w1064v1809 */,
            "RoGetActivationFactory returned %#lx.\n", hr);
    if (hr == REGDB_E_CLASSNOTREG)
        ok(!factory, "got factory %p.\n", factory);
    else
        ok(!!factory, "got factory %p.\n", factory);
    if (!factory) ref = 0;
    else ref = IActivationFactory_Release(factory);
    ok(ref == 0, "Release returned %lu\n", ref);
    WindowsDeleteString(str);

    RoUninitialize();
}

static APTTYPE check_thread_apttype;
static APTTYPEQUALIFIER check_thread_aptqualifier;
static HRESULT check_thread_hr;

static DWORD WINAPI check_apartment_thread(void *dummy)
{
    check_thread_apttype = 0xdeadbeef;
    check_thread_aptqualifier = 0xdeadbeef;
    check_thread_hr = CoGetApartmentType(&check_thread_apttype, &check_thread_aptqualifier);
    return 0;
}

#define check_thread_apartment(a) check_thread_apartment_(__LINE__, FALSE, a)
#define check_thread_apartment_broken(a) check_thread_apartment_(__LINE__, TRUE, a)
static void check_thread_apartment_(unsigned int line, BOOL broken_fail, HRESULT expected_hr_thread)
{
    HANDLE thread;

    check_thread_hr = 0xdeadbeef;
    thread = CreateThread(NULL, 0, check_apartment_thread, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    ok_(__FILE__, line)(check_thread_hr == expected_hr_thread
            || broken(broken_fail && expected_hr_thread == S_OK && check_thread_hr == CO_E_NOTINITIALIZED),
            "got %#lx, expected %#lx.\n", check_thread_hr, expected_hr_thread);
    if (SUCCEEDED(check_thread_hr))
    {
        ok_(__FILE__, line)(check_thread_apttype == APTTYPE_MTA, "got %d.\n", check_thread_apttype);
        ok_(__FILE__, line)(check_thread_aptqualifier == APTTYPEQUALIFIER_IMPLICIT_MTA, "got %d.\n", check_thread_aptqualifier);
    }
}

static HANDLE mta_init_thread_init_done_event, mta_init_thread_done_event;

static DWORD WINAPI mta_init_thread(void *dummy)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(hr == S_OK, "got %#lx.\n", hr);
    SetEvent(mta_init_thread_init_done_event);

    WaitForSingleObject(mta_init_thread_done_event, INFINITE);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI mta_init_implicit_thread(void *dummy)
{
    IActivationFactory *factory;
    HSTRING str;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = WindowsCreateString(L"Does.Not.Exist", ARRAY_SIZE(L"Does.Not.Exist") - 1, &str);
    ok(hr == S_OK, "got %#lx.\n", hr);
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
    WindowsDeleteString(str);

    SetEvent(mta_init_thread_init_done_event);
    WaitForSingleObject(mta_init_thread_done_event, INFINITE);

    /* No CoUninitialize(), testing cleanup on thread exit. */
    return 0;
}

enum oletlsflags
{
    OLETLS_UUIDINITIALIZED = 0x2,
    OLETLS_DISABLE_OLE1DDE = 0x40,
    OLETLS_APARTMENTTHREADED = 0x80,
    OLETLS_MULTITHREADED = 0x100,
};

struct oletlsdata
{
    void *threadbase;
    void *smallocator;
    DWORD id;
    DWORD flags;
};

static DWORD get_oletlsflags(void)
{
    struct oletlsdata *data = NtCurrentTeb()->ReservedForOle;
    return data ? data->flags : 0;
}

static void test_implicit_mta(void)
{
    static const struct
    {
        BOOL ro_init;
        BOOL mta;
    }
    tests[] =
    {
        { FALSE, FALSE },
        { FALSE, TRUE },
        { TRUE, FALSE },
        { TRUE, TRUE },
    };
    APTTYPEQUALIFIER aptqualifier;
    IActivationFactory *factory;
    APTTYPE apttype;
    unsigned int i;
    HANDLE thread;
    HSTRING str;
    HRESULT hr;

    hr = WindowsCreateString(L"Does.Not.Exist", ARRAY_SIZE(L"Does.Not.Exist") - 1, &str);
    ok(hr == S_OK, "got %#lx.\n", hr);
    /* RoGetActivationFactory doesn't implicitly initialize COM. */
    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == CO_E_NOTINITIALIZED, "got %#lx.\n", hr);

    check_thread_apartment(CO_E_NOTINITIALIZED);

    /* RoGetActivationFactory initializes implicit MTA. */
    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        winetest_push_context("test %u", i);
        if (tests[i].ro_init)
        {
            DWORD flags = tests[i].mta ? OLETLS_MULTITHREADED : OLETLS_APARTMENTTHREADED;

            hr = RoInitialize(tests[i].mta ? RO_INIT_MULTITHREADED : RO_INIT_SINGLETHREADED);

            flags |= OLETLS_DISABLE_OLE1DDE;
            ok((get_oletlsflags() & flags) == flags, "get_oletlsflags() = %lx\n", get_oletlsflags());
        }
        else
            hr = CoInitializeEx(NULL, tests[i].mta ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
        ok(hr == S_OK, "got %#lx.\n", hr);
        check_thread_apartment(tests[i].mta ? S_OK : CO_E_NOTINITIALIZED);
        hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
        ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
        check_thread_apartment_broken(S_OK); /* Broken on Win8. */
        hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
        ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
        check_thread_apartment_broken(S_OK); /* Broken on Win8. */
        if (tests[i].ro_init)
            RoUninitialize();
        else
            CoUninitialize();
        check_thread_apartment(CO_E_NOTINITIALIZED);
        winetest_pop_context();
    }

    mta_init_thread_init_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    mta_init_thread_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* RoGetActivationFactory references implicit MTA in a current thread
     * even if implicit MTA was already initialized: check with STA init
     * after RoGetActivationFactory(). */
    thread = CreateThread(NULL, 0, mta_init_thread, NULL, 0, NULL);
    ok(!!thread, "failed.\n");
    WaitForSingleObject(mta_init_thread_init_done_event, INFINITE);
    check_thread_apartment(S_OK);

    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
    check_thread_apartment(S_OK);

    hr = CoGetApartmentType(&apttype, &aptqualifier);
    ok(hr == S_OK, "got %#lx.\n", hr);
    ok(apttype == APTTYPE_MTA, "got %d.\n", apttype);
    ok(aptqualifier == APTTYPEQUALIFIER_IMPLICIT_MTA, "got %d.\n", aptqualifier);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = CoGetApartmentType(&apttype, &aptqualifier);
    ok(hr == S_OK, "got %#lx.\n", hr);
    ok(apttype == APTTYPE_MAINSTA, "got %d.\n", apttype);
    ok(aptqualifier == APTTYPEQUALIFIER_NONE, "got %d.\n", aptqualifier);

    SetEvent(mta_init_thread_done_event);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check_thread_apartment_broken(S_OK); /* Broken on Win8. */
    CoUninitialize();
    check_thread_apartment(CO_E_NOTINITIALIZED);

    /* RoGetActivationFactory references implicit MTA in a current thread
     * even if implicit MTA was already initialized: check with STA init
     * before RoGetActivationFactory(). */
    thread = CreateThread(NULL, 0, mta_init_thread, NULL, 0, NULL);
    ok(!!thread, "failed.\n");
    WaitForSingleObject(mta_init_thread_init_done_event, INFINITE);
    check_thread_apartment(S_OK);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = RoGetActivationFactory(str, &IID_IActivationFactory, (void **)&factory);
    ok(hr == REGDB_E_CLASSNOTREG, "got %#lx.\n", hr);
    check_thread_apartment(S_OK);

    SetEvent(mta_init_thread_done_event);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check_thread_apartment_broken(S_OK); /* Broken on Win8. */
    CoUninitialize();
    check_thread_apartment(CO_E_NOTINITIALIZED);

    /* Test implicit MTA apartment thread exit. */
    thread = CreateThread(NULL, 0, mta_init_implicit_thread, NULL, 0, NULL);
    ok(!!thread, "failed.\n");
    WaitForSingleObject(mta_init_thread_init_done_event, INFINITE);
    check_thread_apartment_broken(S_OK); /* Broken on Win8. */
    SetEvent(mta_init_thread_done_event);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check_thread_apartment(CO_E_NOTINITIALIZED);

    CloseHandle(mta_init_thread_init_done_event);
    CloseHandle(mta_init_thread_done_event);
    WindowsDeleteString(str);
}

struct unk_impl
{
    IUnknown IUnknown_iface;
    LONG ref;
};

static inline struct unk_impl *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct unk_impl, IUnknown_iface);
}

static HRESULT WINAPI unk_QueryInterface(IUnknown *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown))
    {
        *ppv = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static HRESULT WINAPI unk_no_marshal_QueryInterface(IUnknown *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_INoMarshal))
    {
        *ppv = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static HRESULT WINAPI unk_agile_QueryInterface(IUnknown *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IAgileObject))
    {
        *ppv = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI unk_AddRef(IUnknown *iface)
{
    struct unk_impl *impl = impl_from_IUnknown(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI unk_Release(IUnknown *iface)
{
    struct unk_impl *impl = impl_from_IUnknown(iface);
    return InterlockedDecrement(&impl->ref);
}

static const IUnknownVtbl unk_vtbl =
{
    unk_QueryInterface,
    unk_AddRef,
    unk_Release
};

static const IUnknownVtbl unk_no_marshal_vtbl =
{
    unk_no_marshal_QueryInterface,
    unk_AddRef,
    unk_Release
};

static const IUnknownVtbl unk_agile_vtbl =
{
    unk_agile_QueryInterface,
    unk_AddRef,
    unk_Release
};

/* IUnknown implementation that has affinity to the STA it was created in. */
struct unk_ctx_impl
{
    IUnknown IUnknown_iface;
    BOOL todo;
    UINT64 apt_id; /* Identifier of the apartment this object belongs to. */
    ULONG_PTR context; /* The COM context this object belongs to. */
    APTTYPE type; /* APTTYPE of the apartment this object belongs to. */
    DWORD thread_id;
    GUID com_thread_id;
    LONG ref;
};

static inline struct unk_ctx_impl *impl_unk_ctx_impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct unk_ctx_impl, IUnknown_iface);
}

/* If true, the method call is being made from the test thread. */
static BOOL caller_test_thread;

#define test_apartment_context(impl) test_apartment_context_(__LINE__, impl)
static void test_apartment_context_(int line, const struct unk_ctx_impl *impl)
{
    APTTYPEQUALIFIER qualifier;
    ULONG_PTR cur_ctx;
    UINT64 cur_id;
    APTTYPE type;
    HRESULT hr;

    hr = CoGetContextToken(&cur_ctx);
    ok_(__FILE__, line)(hr == S_OK, "CoGetContextToken failed, got hr %#lx\n", hr);
    /* As this object has apartment-affinity, its methods can only be called from the apartment and context it was
     * created in. */
    ok_(__FILE__, line)(cur_ctx == impl->context, "got cur_ctx %#Ix != %#Ix\n", cur_ctx, impl->context);

    hr = CoGetApartmentType(&type, &qualifier);
    ok_(__FILE__, line)(hr == S_OK, "CoGetApartmentType failed, got hr %#lx\n", hr);
    ok_(__FILE__, line)(type == impl->type, "got type %d\n", type);

    hr = RoGetApartmentIdentifier(&cur_id);
    /* win10 and below fail with ERROR_API_UNAVAILABLE */
    ok_(__FILE__, line)(hr == S_OK || broken(hr == HRESULT_FROM_WIN32(ERROR_API_UNAVAILABLE)),
                        "RoGetApartmentIdentifier failed, got hr %#lx\n", hr);
    if (SUCCEEDED(hr))
        ok_(__FILE__, line)(cur_id == impl->apt_id, "got cur_id %#I64x != %#I64x\n", cur_id, impl->apt_id);

    if (caller_test_thread)
    {
        DWORD cur_tid = GetCurrentThreadId();
        IUnknown *unk = (IUnknown *)cur_ctx;
        GUID ctx_com_tid, cur_com_tid;
        IComThreadingInfo *info;

        hr = IUnknown_QueryInterface(unk, &IID_IComThreadingInfo, (void **)&info);
        ok_(__FILE__, line)(hr == S_OK, "QueryInterface failed, got hr %#lx\n", hr);

        hr = IComThreadingInfo_GetCurrentLogicalThreadId(info, &ctx_com_tid);
        ok_(__FILE__, line)(hr == S_OK, "GetCurrentLogicalThreadId failed, got hr %#lx\n", hr);

        hr = CoGetCurrentLogicalThreadId(&cur_com_tid);
        ok_(__FILE__, line)(hr == S_OK, "CoGetCurrentLogicalThreadId failed, got hr %#lx\n", hr);
        ok_(__FILE__, line)(IsEqualGUID(&cur_com_tid, &ctx_com_tid), "Got cur_com_tid %s != %s.\n",
                            debugstr_guid(&cur_com_tid), debugstr_guid(&ctx_com_tid));
        IComThreadingInfo_Release(info);

        /* If this object belongs to an STA, we should now be in the same thread that the object was created in. */
        if (impl->type == APTTYPE_STA || impl->type == APTTYPE_MAINSTA)
        {
            ok(cur_tid == impl->thread_id, "Got cur_tid %lu != %lu\n", cur_tid, impl->thread_id);
            ok(!IsEqualGUID(&ctx_com_tid, &impl->com_thread_id), "Got cur_com_tid %s != %s\n",
               debugstr_guid(&ctx_com_tid), debugstr_guid(&impl->com_thread_id));
        }
        else
            /* If this object belongs to a MTA, then the method call will be made from the same thread as that of the
             * caller. */
            ok(cur_tid != impl->thread_id, "Got cur_tid %lu\n", cur_tid);
    }
}

static HRESULT WINAPI unk_ctx_impl_QueryInterface(IUnknown *iface, const GUID *iid, void **out)
{
    struct unk_ctx_impl *impl = impl_unk_ctx_impl_from_IUnknown(iface);

    if (winetest_debug > 1)
        trace("(%p, %s, %p)\n", iface, debugstr_guid(iid), out);

    test_apartment_context(impl);
    if (IsEqualGUID(iid, &IID_IUnknown))
    {
        *out = &impl->IUnknown_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    *out = NULL;
    if (winetest_debug > 1)
        trace("%s not implemeneted, returning E_NOINTERFACE\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI unk_ctx_impl_AddRef(IUnknown *iface)
{
    struct unk_ctx_impl *impl = impl_unk_ctx_impl_from_IUnknown(iface);

    test_apartment_context(impl);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI unk_ctx_impl_Release(IUnknown *iface)
{
    struct unk_ctx_impl *impl = impl_unk_ctx_impl_from_IUnknown(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);

    test_apartment_context(impl);
    if (!ref) free(impl);
    return ref;
}

static const IUnknownVtbl unk_ctx_impl_IUnknown_vtbl =
{
    unk_ctx_impl_QueryInterface,
    unk_ctx_impl_AddRef,
    unk_ctx_impl_Release,
};

static IUnknown *unk_ctx_impl_create(void)
{
    APTTYPEQUALIFIER qualifier;
    struct unk_ctx_impl *impl;
    UINT64 apt_id = 0;
    ULONG_PTR context;
    APTTYPE type;
    HRESULT hr;
    GUID id;

    hr = CoGetApartmentType(&type, &qualifier);
    ok(hr == S_OK, "got hr %#lx\n", hr);

    hr = CoGetContextToken(&context);
    ok(hr == S_OK, "got hr %#lx\n", hr);

    hr = RoGetApartmentIdentifier(&apt_id);
    /* win10 and below fail with ERROR_API_UNAVAILABLE */
    ok(hr == S_OK || broken(hr == HRESULT_FROM_WIN32(ERROR_API_UNAVAILABLE)), "got hr %#lx\n", hr);

    hr = CoGetCurrentLogicalThreadId(&id);
    ok(hr == S_OK, "got hr %#lx\n", hr);

    if (!(impl = calloc(1, sizeof(*impl)))) return NULL;
    impl->IUnknown_iface.lpVtbl = &unk_ctx_impl_IUnknown_vtbl;
    impl->context = context;
    impl->type = type;
    impl->apt_id = apt_id;
    impl->com_thread_id = id;
    impl->thread_id = GetCurrentThreadId();
    impl->ref = 1;
    return &impl->IUnknown_iface;
}

struct test_RoGetAgileReference_thread_param
{
    enum AgileReferenceOptions option;
    RO_INIT_TYPE from_type;
    RO_INIT_TYPE to_type;
    IAgileReference *agile_reference;
    IUnknown *unk_obj;
    BOOLEAN obj_is_agile;
};

static DWORD CALLBACK test_RoGetAgileReference_thread_proc(void *arg)
{
    struct test_RoGetAgileReference_thread_param *param = (struct test_RoGetAgileReference_thread_param *)arg;
    IUnknown *unknown;
    HRESULT hr;

    winetest_push_context("%d %d %d", param->option, param->from_type, param->to_type);

    RoInitialize(param->to_type);

    unknown = NULL;
    hr = IAgileReference_Resolve(param->agile_reference, &IID_IUnknown, (void **)&unknown);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(!!unknown, "Expected pointer not NULL.\n");
    if (param->obj_is_agile)
    {
        ok(unknown == param->unk_obj, "Expected the same object.\n");
        EXPECT_REF(param->unk_obj, 4);
    }
    else if (param->from_type == RO_INIT_MULTITHREADED && param->to_type == RO_INIT_MULTITHREADED)
    {
        ok(unknown == param->unk_obj, "Expected the same object.\n");
        todo_wine_if(param->option == AGILEREFERENCE_DEFAULT)
        EXPECT_REF(param->unk_obj, param->option == AGILEREFERENCE_DEFAULT ? 5 : 6);
    }
    else
    {
        ok(unknown != param->unk_obj, "Expected a different object pointer.\n");
        todo_wine_if(param->option == AGILEREFERENCE_DEFAULT)
        EXPECT_REF(param->unk_obj, param->option == AGILEREFERENCE_DEFAULT ? 4 : 5);
        EXPECT_REF(unknown, 1);
    }
    IUnknown_Release(unknown);

    RoUninitialize();
    winetest_pop_context();
    return 0;
}

struct test_agile_resolve_context_params
{
    RO_INIT_TYPE from_type;
    RO_INIT_TYPE to_type;
    IAgileReference *ref;
};

static DWORD CALLBACK test_agile_resolve_context(void *arg)
{
    struct test_agile_resolve_context_params *params = arg;
    IUnknown *unknown;
    HRESULT hr;

    caller_test_thread = TRUE;
    hr = RoInitialize(params->to_type);
    ok(hr == S_OK, "got hr %#lx.\n", hr);

    winetest_push_context("from_type=%d, to_type=%d", params->from_type, params->to_type);
    hr = IAgileReference_Resolve(params->ref, &IID_IUnknown, (void **)&unknown);
    ok(hr == S_OK, "got hr %#lx\n", hr);
    if (SUCCEEDED(hr))
        IUnknown_Release(unknown);
    winetest_pop_context();

    RoUninitialize();
    caller_test_thread = FALSE;
    return 0;
}

static void test_RoGetAgileReference(void)
{
    struct test_RoGetAgileReference_thread_param param;
    struct unk_impl unk_no_marshal_obj = {{&unk_no_marshal_vtbl}, 1};
    struct unk_impl unk_obj = {{&unk_vtbl}, 1};
    struct unk_impl unk_agile_obj = {{&unk_agile_vtbl}, 1};
    struct unk_ctx_impl *unk_ctx_impl;
    enum AgileReferenceOptions option;
    IAgileReference *agile_reference;
    RO_INIT_TYPE from_type, to_type;
    IUnknown *unknown, *unknown2;
    IAgileObject *agile_object;
    HANDLE thread;
    HRESULT hr;
    DWORD ret;

    for (option = AGILEREFERENCE_DEFAULT; option <= AGILEREFERENCE_DELAYEDMARSHAL; option++)
    {
        for (from_type = RO_INIT_SINGLETHREADED; from_type <= RO_INIT_MULTITHREADED; from_type++)
        {
            winetest_push_context("%d %d", option, from_type);

            hr = RoGetAgileReference(option, &IID_IUnknown, &unk_obj.IUnknown_iface, &agile_reference);
            ok(hr == CO_E_NOTINITIALIZED, "Got unexpected hr %#lx.\n", hr);

            RoInitialize(from_type);

            agile_reference = NULL;
            EXPECT_REF(&unk_obj, 1);

            /* Invalid option */
            hr = RoGetAgileReference(AGILEREFERENCE_DELAYEDMARSHAL + 1, &IID_IUnknown, &unk_obj.IUnknown_iface, &agile_reference);
            ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

            /* Non-existent interface for the object */
            hr = RoGetAgileReference(option, &IID_IActivationFactory, &unk_obj.IUnknown_iface, &agile_reference);
            ok(hr == E_NOINTERFACE, "Got unexpected hr %#lx.\n", hr);

            /* Objects that implements INoMarshal */
            hr = RoGetAgileReference(option, &IID_IUnknown, &unk_no_marshal_obj.IUnknown_iface, &agile_reference);
            ok(hr == CO_E_NOT_SUPPORTED, "Got unexpected hr %#lx.\n", hr);

            /* Create agile reference object */
            hr = RoGetAgileReference(option, &IID_IUnknown, &unk_obj.IUnknown_iface, &agile_reference);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!agile_reference, "Got unexpected agile_reference.\n");
            todo_wine_if(option == AGILEREFERENCE_DEFAULT)
            EXPECT_REF(&unk_obj, 2);

            /* Check the created agile reference object has IAgileObject */
            hr = IAgileReference_QueryInterface(agile_reference, &IID_IAgileObject, (void **)&agile_object);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            IAgileObject_Release(agile_object);

            /* Resolve once */
            unknown = NULL;
            hr = IAgileReference_Resolve(agile_reference, &IID_IUnknown, (void **)&unknown);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!unknown, "Expected pointer not NULL.\n");
            ok(unknown == &unk_obj.IUnknown_iface, "Expected the same object.\n");
            todo_wine
            EXPECT_REF(&unk_obj, 3);

            /* Resolve twice */
            unknown2 = NULL;
            hr = IAgileReference_Resolve(agile_reference, &IID_IUnknown, (void **)&unknown2);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!unknown2, "Expected pointer not NULL.\n");
            ok(unknown2 == &unk_obj.IUnknown_iface, "Expected the same object.\n");
            todo_wine
            EXPECT_REF(&unk_obj, 4);

            /* Resolve in another apartment */
            for (to_type = RO_INIT_SINGLETHREADED; to_type <= RO_INIT_MULTITHREADED; to_type++)
            {
                param.option = option;
                param.from_type = from_type;
                param.to_type = to_type;
                param.agile_reference = agile_reference;
                param.unk_obj = &unk_obj.IUnknown_iface;
                param.obj_is_agile = FALSE;
                thread = CreateThread(NULL, 0, test_RoGetAgileReference_thread_proc, &param, 0, NULL);
                flush_events();
                ret = WaitForSingleObject(thread, 100);
                ok(!ret, "WaitForSingleObject failed, error %ld.\n", GetLastError());
            }

            IUnknown_Release(unknown2);
            IUnknown_Release(unknown);
            IAgileReference_Release(agile_reference);
            EXPECT_REF(&unk_obj, 1);

            hr = RoGetAgileReference(option, &IID_IUnknown, &unk_agile_obj.IUnknown_iface, &agile_reference);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!agile_reference, "Got unexpected agile_reference.\n");
            EXPECT_REF(&unk_agile_obj, 2);

            unknown = NULL;
            hr = IAgileReference_Resolve(agile_reference, &IID_IUnknown, (void **)&unknown);
            ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
            ok(!!unknown, "Expected pointer not NULL.\n");
            ok(unknown == &unk_agile_obj.IUnknown_iface, "Expected the same object.\n");
            EXPECT_REF(&unk_agile_obj, 3);

            for (to_type = RO_INIT_SINGLETHREADED; to_type <= RO_INIT_MULTITHREADED; to_type++)
            {
                param.option = option;
                param.from_type = from_type;
                param.to_type = to_type;
                param.agile_reference = agile_reference;
                param.unk_obj = &unk_agile_obj.IUnknown_iface;
                param.obj_is_agile = TRUE;
                thread = CreateThread(NULL, 0, test_RoGetAgileReference_thread_proc, &param, 0, NULL);
                flush_events();
                ret = WaitForSingleObject(thread, 100);
                ok(!ret, "WaitForSingleObject failed, error %ld.\n", GetLastError());
            }

            IUnknown_Release(unknown);
            IAgileReference_Release(agile_reference);
            EXPECT_REF(&unk_obj, 1);

            RoUninitialize();
            winetest_pop_context();
        }
    }

    /* Tests specific to delayed marshaling */
    for (from_type = RO_INIT_SINGLETHREADED; from_type <= RO_INIT_MULTITHREADED; from_type++)
    {
        winetest_push_context("from_type=%d", from_type);
        hr = RoInitialize(from_type);
        ok(hr == S_OK, "got hr %#lx.\n", hr);

        unknown = unk_ctx_impl_create();
        ok(unknown != NULL, "got unknown %p.\n", unknown);

        unk_ctx_impl = impl_unk_ctx_impl_from_IUnknown(unknown);
        hr = RoGetAgileReference(AGILEREFERENCE_DELAYEDMARSHAL, &IID_IUnknown, unknown, &agile_reference);
        ok(hr == S_OK, "got hr %#lx\n", hr);
        EXPECT_REF(unknown, 2);

        for (to_type = RO_INIT_SINGLETHREADED; to_type <= RO_INIT_MULTITHREADED; to_type++)
        {
            struct test_agile_resolve_context_params params = {from_type, to_type, agile_reference};

            winetest_push_context("to_type=%d", to_type);
            unk_ctx_impl->todo = TRUE;
            thread = CreateThread(NULL, 0, test_agile_resolve_context, &params, 0, NULL);
            flush_events();
            ret = WaitForSingleObject(thread, INFINITE);
            ok(!ret, "got ret %lu\n", ret);
            CloseHandle(thread);
            unk_ctx_impl->todo = FALSE;
            winetest_pop_context();
        }

        IAgileReference_Release(agile_reference);
        EXPECT_REF(unknown, 1);
        IUnknown_Release(unknown);
        RoUninitialize();
        winetest_pop_context();
    }
}

static void test_RoGetErrorReportingFlags(void)
{
    UINT32 flags;
    HRESULT hr;

    hr = RoGetErrorReportingFlags(NULL);
    ok(hr == E_POINTER, "Got unexpected hr %#lx.\n", hr);

    hr = RoGetErrorReportingFlags(&flags);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(flags == RO_ERROR_REPORTING_USESETERRORINFO, "Got unexpected flag %#x.\n", flags);
}

START_TEST(roapi)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv);
    BOOL ret;

    if (argc >= 4 && !strcmp(argv[2], "package-child"))
        ExitProcess(package_activation_child(argv[3]));

    load_resource(L"wine.combase.test.dll");

    test_package_activation();
    test_implicit_mta();
    test_ActivationFactories();
    test_RoGetAgileReference();
    test_RoGetErrorReportingFlags();

    SetLastError(0xdeadbeef);
    ret = DeleteFileW(L"wine.combase.test.dll");
    todo_wine_if(!ret && GetLastError() == ERROR_ACCESS_DENIED)
    ok(ret, "Failed to delete file, error %lu\n", GetLastError());
}
