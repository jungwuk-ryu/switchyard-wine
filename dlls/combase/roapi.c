/*
 * Copyright 2014 Martin Storsjo
 * Copyright 2016 Michael Müller
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
#include "objbase.h"
#include "ctxtcall.h"
#include "comsvcs.h"
#include "initguid.h"
#include "roapi.h"
#include "roparameterizediid.h"
#include "roerrorapi.h"
#include "winstring.h"
#include "errhandlingapi.h"

#include "combase_private.h"

#include "wine/debug.h"
#include "wine/exception.h"
#include "wine/appx_package_graph.h"

WINE_DEFAULT_DEBUG_CHANNEL(combase);

enum appx_graph_offsets
{
    APPX_HEADER_PACKAGE_COUNT_OFFSET = 44,
    APPX_HEADER_PACKAGES_OFFSET      = 48,
    APPX_HEADER_LOADER_COUNT_OFFSET  = 52,
    APPX_HEADER_LOADERS_OFFSET       = 56,
    APPX_HEADER_CLASS_COUNT_OFFSET   = 104,
    APPX_HEADER_CLASSES_OFFSET       = 108,

    APPX_PACKAGE_ROOT_REF_OFFSET     = 104,

    APPX_LOADER_PACKAGE_INDEX_OFFSET = 0,
    APPX_LOADER_BASENAME_REF_OFFSET = 8,
    APPX_LOADER_PATH_REF_OFFSET      = 16,

    APPX_CLASS_PACKAGE_INDEX_OFFSET  = 0,
    APPX_CLASS_THREADING_OFFSET      = 4,
    APPX_CLASS_ID_REF_OFFSET         = 8,
    APPX_CLASS_PATH_REF_OFFSET       = 16,
};

enum appx_threading_model
{
    APPX_THREADING_BOTH,
    APPX_THREADING_STA,
    APPX_THREADING_MTA,
};

struct appx_class_graph
{
    const BYTE *data;
    UINT32 package_count;
    UINT32 packages_offset;
    UINT32 loader_count;
    UINT32 loaders_offset;
    UINT32 class_count;
    UINT32 classes_offset;
};

struct appx_class_graph_cache
{
    BOOL present;
    BOOL valid;
    struct appx_class_graph graph;
};

static INIT_ONCE appx_class_graph_once = INIT_ONCE_STATIC_INIT;
static struct appx_class_graph_cache appx_class_graph_cache;

struct activatable_class_data
{
    ULONG size;
    DWORD unk;
    DWORD module_len;
    DWORD module_offset;
    DWORD threading_model;
};

static BOOL actctx_section_contains_range(const ACTCTX_SECTION_KEYED_DATA *data, UINT_PTR offset, SIZE_T size)
{
    return data->lpSectionBase && offset <= data->ulSectionTotalLength
            && size <= data->ulSectionTotalLength - offset;
}

static BOOL actctx_section_contains_pointer(const ACTCTX_SECTION_KEYED_DATA *data, const void *ptr, SIZE_T size)
{
    UINT_PTR base = (UINT_PTR)data->lpSectionBase, value = (UINT_PTR)ptr;

    return ptr && value >= base && actctx_section_contains_range(data, value - base, size);
}

static BOOL actctx_section_contains_data(const ACTCTX_SECTION_KEYED_DATA *data, SIZE_T size)
{
    return data->ulLength >= size && actctx_section_contains_pointer(data, data->lpData, size);
}

static WCHAR appx_graph_char(const struct appx_class_graph *graph,
                             struct wine_appx_graph_string_ref ref, UINT32 index)
{
    return wine_appx_graph_read_u16(graph->data + ref.offset + index * sizeof(WCHAR));
}

static BOOL appx_path_component_is_reserved(const struct appx_class_graph *graph,
                                            struct wine_appx_graph_string_ref ref,
                                            UINT32 start, UINT32 length)
{
    WCHAR name[5];
    UINT32 i, base_length = 0;

    while (base_length < length && appx_graph_char(graph, ref, start + base_length) != '.')
        base_length++;
    if (!base_length || base_length > 4) return FALSE;
    for (i = 0; i < base_length; i++)
        name[i] = RtlUpcaseUnicodeChar(appx_graph_char(graph, ref, start + i));
    name[base_length] = 0;
    if ((base_length == 3 && (!wcscmp(name, L"CON") ||
                              !wcscmp(name, L"PRN") ||
                              !wcscmp(name, L"AUX") ||
                              !wcscmp(name, L"NUL"))) ||
        (base_length == 4 && (!wcsncmp(name, L"COM", 3) ||
                              !wcsncmp(name, L"LPT", 3)) &&
         name[3] >= '1' && name[3] <= '9'))
        return TRUE;
    return FALSE;
}

static BOOL appx_validate_path_components(const struct appx_class_graph *graph,
                                          struct wine_appx_graph_string_ref ref,
                                          UINT32 start, UINT32 length)
{
    UINT32 component = start, i;

    if (start == length) return TRUE;
    for (i = start; i <= length; i++)
    {
        WCHAR ch = i == length ? '\\' : appx_graph_char(graph, ref, i);

        if (ch == '/' || ch == ':' || ch == '"' || ch == '<' || ch == '>' ||
            ch == '|' || ch == '?' || ch == '*' || ch < 0x20)
            return FALSE;
        if (ch != '\\') continue;
        if (i == component ||
            (i - component == 1 && appx_graph_char(graph, ref, component) == '.') ||
            (i - component == 2 && appx_graph_char(graph, ref, component) == '.' &&
             appx_graph_char(graph, ref, component + 1) == '.') ||
            appx_graph_char(graph, ref, i - 1) == ' ' ||
            appx_graph_char(graph, ref, i - 1) == '.' ||
            i - component > 255 ||
            appx_path_component_is_reserved(graph, ref, component, i - component))
            return FALSE;
        component = i + 1;
    }
    return TRUE;
}

static BOOL appx_validate_package_root(const struct appx_class_graph *graph,
                                       struct wine_appx_graph_string_ref ref)
{
    UINT32 length;
    WCHAR drive;

    if (ref.chars < 4 || ref.chars > WINE_APPX_GRAPH_MAX_STRING_CHARS + 1)
        return FALSE;
    length = ref.chars - 1;
    drive = appx_graph_char(graph, ref, 0);
    if (!((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) ||
        appx_graph_char(graph, ref, 1) != ':' ||
        appx_graph_char(graph, ref, 2) != '\\')
        return FALSE;
    if (length > 3 && appx_graph_char(graph, ref, length - 1) == '\\')
        return FALSE;
    return appx_validate_path_components(graph, ref, 3, length);
}

static BOOL appx_validate_relative_path(const struct appx_class_graph *graph,
                                        struct wine_appx_graph_string_ref ref,
                                        BOOL basename_only)
{
    UINT32 i;

    if (ref.chars < 2 || ref.chars > WINE_APPX_GRAPH_MAX_STRING_CHARS + 1 ||
        appx_graph_char(graph, ref, 0) == '\\')
        return FALSE;
    if (!appx_validate_path_components(graph, ref, 0, ref.chars - 1))
        return FALSE;
    if (basename_only)
        for (i = 0; i + 1 < ref.chars; i++)
            if (appx_graph_char(graph, ref, i) == '\\') return FALSE;
    return TRUE;
}

static INT appx_compare_ref_ranges_ci(const struct appx_class_graph *graph,
                                      struct wine_appx_graph_string_ref left,
                                      UINT32 left_start, UINT32 left_length,
                                      struct wine_appx_graph_string_ref right,
                                      UINT32 right_start, UINT32 right_length)
{
    UINT32 length = min(left_length, right_length), i;

    for (i = 0; i < length; i++)
    {
        WCHAR left_ch = RtlUpcaseUnicodeChar(appx_graph_char(graph, left, left_start + i));
        WCHAR right_ch = RtlUpcaseUnicodeChar(appx_graph_char(graph, right, right_start + i));

        if (left_ch != right_ch) return left_ch < right_ch ? -1 : 1;
    }
    if (left_length == right_length) return 0;
    return left_length < right_length ? -1 : 1;
}

static INT appx_compare_refs_ci(const struct appx_class_graph *graph,
                                struct wine_appx_graph_string_ref left,
                                struct wine_appx_graph_string_ref right)
{
    return appx_compare_ref_ranges_ci(graph, left, 0, left.chars - 1,
                                      right, 0, right.chars - 1);
}

static BOOL appx_refs_equal(const struct appx_class_graph *graph,
                            struct wine_appx_graph_string_ref left,
                            UINT32 left_start,
                            struct wine_appx_graph_string_ref right,
                            UINT32 right_start, UINT32 length)
{
    UINT32 i;

    for (i = 0; i < length; i++)
        if (appx_graph_char(graph, left, left_start + i) !=
            appx_graph_char(graph, right, right_start + i))
            return FALSE;
    return TRUE;
}

static BOOL appx_basename_matches_path(const struct appx_class_graph *graph,
                                       struct wine_appx_graph_string_ref basename,
                                       struct wine_appx_graph_string_ref path)
{
    UINT32 start = path.chars - 1;

    while (start && appx_graph_char(graph, path, start - 1) != '\\') start--;
    return basename.chars - 1 == path.chars - 1 - start &&
           appx_refs_equal(graph, basename, 0, path, start,
                           basename.chars - 1);
}

static const BYTE *appx_find_loader_for_class(const struct appx_class_graph *graph,
                                              const BYTE *class_record)
{
    struct wine_appx_graph_string_ref class_path =
        wine_appx_graph_get_ref(class_record, APPX_CLASS_PATH_REF_OFFSET);
    struct wine_appx_graph_string_ref loader_path;
    UINT32 package_index = wine_appx_graph_read_u32(
        class_record + APPX_CLASS_PACKAGE_INDEX_OFFSET);
    UINT32 loader_index = wine_appx_graph_read_u32(
        class_record + WINE_APPX_GRAPH_CLASS_LOADER_INDEX_OFFSET);
    const BYTE *loader;

    if (loader_index >= graph->loader_count) return NULL;
    loader = graph->data + graph->loaders_offset +
             loader_index * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
    loader_path = wine_appx_graph_get_ref(
        loader, APPX_LOADER_PATH_REF_OFFSET);
    if (wine_appx_graph_read_u32(
            loader + APPX_LOADER_PACKAGE_INDEX_OFFSET) != package_index ||
        class_path.chars != loader_path.chars ||
        !appx_refs_equal(graph, class_path, 0, loader_path, 0,
                         class_path.chars) ||
        wine_appx_graph_read_u32(
            loader + WINE_APPX_GRAPH_LOADER_VOLUME_SERIAL_OFFSET) !=
            wine_appx_graph_read_u32(
                class_record + WINE_APPX_GRAPH_CLASS_VOLUME_SERIAL_OFFSET) ||
        wine_appx_graph_read_u32(
            loader + WINE_APPX_GRAPH_LOADER_FILE_INDEX_HIGH_OFFSET) !=
            wine_appx_graph_read_u32(
                class_record +
                WINE_APPX_GRAPH_CLASS_FILE_INDEX_HIGH_OFFSET) ||
        wine_appx_graph_read_u32(
            loader + WINE_APPX_GRAPH_LOADER_FILE_INDEX_LOW_OFFSET) !=
            wine_appx_graph_read_u32(
                class_record +
                WINE_APPX_GRAPH_CLASS_FILE_INDEX_LOW_OFFSET) ||
        wine_appx_graph_read_u64(
            loader + WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET) !=
            wine_appx_graph_read_u64(
                class_record + WINE_APPX_GRAPH_CLASS_CHANGE_TIME_OFFSET) ||
        wine_appx_graph_read_u64(
            loader + WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET) !=
            wine_appx_graph_read_u64(
                class_record + WINE_APPX_GRAPH_CLASS_FILE_SIZE_OFFSET))
        return NULL;
    return loader;
}

static BOOL appx_validate_class_graph_domain(const struct appx_class_graph *graph)
{
    struct wine_appx_graph_string_ref previous_basename = {0};
    struct wine_appx_graph_string_ref previous_path = {0};
    UINT32 previous_package = 0, previous_rank = 0, i;

    for (i = 0; i < graph->package_count; i++)
    {
        const BYTE *record = graph->data + graph->packages_offset +
                             i * WINE_APPX_GRAPH_BLOB_PACKAGE_RECORD_SIZE;

        if (!appx_validate_package_root(
                graph, wine_appx_graph_get_ref(record, APPX_PACKAGE_ROOT_REF_OFFSET)))
            return FALSE;
    }

    for (i = 0; i < graph->loader_count; i++)
    {
        const BYTE *record = graph->data + graph->loaders_offset +
                             i * WINE_APPX_GRAPH_BLOB_LOADER_RECORD_SIZE;
        struct wine_appx_graph_string_ref basename =
            wine_appx_graph_get_ref(record, APPX_LOADER_BASENAME_REF_OFFSET);
        struct wine_appx_graph_string_ref path =
            wine_appx_graph_get_ref(record, APPX_LOADER_PATH_REF_OFFSET);
        UINT32 package_index = wine_appx_graph_read_u32(
            record + APPX_LOADER_PACKAGE_INDEX_OFFSET);
        UINT32 search_rank = wine_appx_graph_read_u32(
            record + WINE_APPX_GRAPH_LOADER_SEARCH_RANK_OFFSET);
        UINT64 change_time = wine_appx_graph_read_u64(
            record + WINE_APPX_GRAPH_LOADER_CHANGE_TIME_OFFSET);
        UINT64 file_size = wine_appx_graph_read_u64(
            record + WINE_APPX_GRAPH_LOADER_FILE_SIZE_OFFSET);
        INT comparison;

        if (package_index >= graph->package_count ||
            (search_rank >= WINE_APPX_GRAPH_MAX_LOADER_SEARCH_PATHS &&
             search_rank != WINE_APPX_GRAPH_LOADER_EXPLICIT_ONLY) ||
            !change_time || (change_time >> 63) ||
            !file_size || (file_size >> 63) ||
            !appx_validate_relative_path(graph, basename, TRUE) ||
            !appx_validate_relative_path(graph, path, FALSE) ||
            !appx_basename_matches_path(graph, basename, path))
            return FALSE;
        if (i)
        {
            comparison = appx_compare_refs_ci(graph, previous_basename, basename);
            if (comparison > 0 ||
                (!comparison && previous_package > package_index) ||
                (!comparison && previous_package == package_index &&
                 previous_rank > search_rank) ||
                (!comparison && previous_package == package_index &&
                 previous_rank == search_rank &&
                 appx_compare_refs_ci(graph, previous_path, path) >= 0))
                return FALSE;
        }
        previous_basename = basename;
        previous_path = path;
        previous_package = package_index;
        previous_rank = search_rank;
    }

    for (i = 0; i < graph->class_count; i++)
    {
        const BYTE *record = graph->data + graph->classes_offset +
                             i * WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE;
        struct wine_appx_graph_string_ref path =
            wine_appx_graph_get_ref(record, APPX_CLASS_PATH_REF_OFFSET);

        if (!appx_validate_relative_path(graph, path, FALSE) ||
            !appx_find_loader_for_class(graph, record))
            return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI initialize_appx_class_graph(INIT_ONCE *once, void *param, void **context)
{
    const BYTE *data = NtCurrentTeb()->Peb->ProcessParameters->PackageDependencyData;
    struct appx_class_graph graph = {0};
    UINT32 size;

    if (!data)
    {
        appx_class_graph_cache.valid = TRUE;
        return TRUE;
    }

    appx_class_graph_cache.present = TRUE;
    __TRY
    {
        size = wine_appx_graph_read_u32(
            data + WINE_APPX_GRAPH_HEADER_TOTAL_SIZE_OFFSET);
        if (wine_appx_graph_validate_blob(data, size))
        {
            graph.data = data;
            graph.package_count = wine_appx_graph_read_u32(
                data + APPX_HEADER_PACKAGE_COUNT_OFFSET);
            graph.packages_offset = wine_appx_graph_read_u32(
                data + APPX_HEADER_PACKAGES_OFFSET);
            graph.loader_count = wine_appx_graph_read_u32(
                data + APPX_HEADER_LOADER_COUNT_OFFSET);
            graph.loaders_offset = wine_appx_graph_read_u32(
                data + APPX_HEADER_LOADERS_OFFSET);
            graph.class_count = wine_appx_graph_read_u32(
                data + APPX_HEADER_CLASS_COUNT_OFFSET);
            graph.classes_offset = wine_appx_graph_read_u32(
                data + APPX_HEADER_CLASSES_OFFSET);
            if (appx_validate_class_graph_domain(&graph))
            {
                appx_class_graph_cache.graph = graph;
                appx_class_graph_cache.valid = TRUE;
            }
        }
    }
    __EXCEPT_PAGE_FAULT
    {
        appx_class_graph_cache.valid = FALSE;
    }
    __ENDTRY
    return TRUE;
}

static INT appx_compare_class_id(const WCHAR *classid, UINT32 length,
                                 const struct appx_class_graph *graph,
                                 struct wine_appx_graph_string_ref ref)
{
    UINT32 ref_length = ref.chars - 1, count = min(length, ref_length), i;

    for (i = 0; i < count; i++)
    {
        WCHAR left = classid[i];
        WCHAR right = appx_graph_char(graph, ref, i);

        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return left < right ? -1 : 1;
    }
    if (length == ref_length) return 0;
    return length < ref_length ? -1 : 1;
}

static HRESULT get_appx_library_for_classid(const WCHAR *classid, UINT32 length,
                                            WCHAR **out, UINT32 *threading_model)
{
    const struct appx_class_graph *graph;
    struct wine_appx_graph_string_ref ref;
    const BYTE *record;
    UINT32 low = 0, high, middle, i;

    if (!InitOnceExecuteOnce(&appx_class_graph_once, initialize_appx_class_graph,
                             NULL, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    if (!appx_class_graph_cache.valid)
        return HRESULT_FROM_WIN32(APPMODEL_ERROR_PACKAGE_RUNTIME_CORRUPT);
    if (!appx_class_graph_cache.present) return S_FALSE;

    if (!length || length > WINE_APPX_GRAPH_MAX_STRING_CHARS)
        return E_INVALIDARG;

    graph = &appx_class_graph_cache.graph;
    high = graph->class_count;
    while (low < high)
    {
        middle = low + (high - low) / 2;
        record = graph->data + graph->classes_offset +
                 middle * WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE;
        ref = wine_appx_graph_get_ref(record, APPX_CLASS_ID_REF_OFFSET);
        if (appx_compare_class_id(classid, length, graph, ref) > 0)
            low = middle + 1;
        else
            high = middle;
    }
    if (low >= graph->class_count) return S_FALSE;
    record = graph->data + graph->classes_offset +
             low * WINE_APPX_GRAPH_BLOB_CLASS_RECORD_SIZE;
    ref = wine_appx_graph_get_ref(record, APPX_CLASS_ID_REF_OFFSET);
    if (appx_compare_class_id(classid, length, graph, ref)) return S_FALSE;

    ref = wine_appx_graph_get_ref(record, APPX_CLASS_PATH_REF_OFFSET);
    if (!(*out = malloc(ref.chars * sizeof(WCHAR)))) return E_OUTOFMEMORY;
    for (i = 0; i < ref.chars; i++) (*out)[i] = appx_graph_char(graph, ref, i);
    *threading_model = wine_appx_graph_read_u32(
        record + APPX_CLASS_THREADING_OFFSET);
    return S_OK;
}

static HRESULT activate_factory_in_current_apartment(
    const WCHAR *library, BOOL packaged, HSTRING classid, REFIID iid,
    void **class_factory)
{
    PFNGETACTIVATIONFACTORY get_activation_factory;
    IActivationFactory *factory;
    HMODULE module;
    HRESULT hr = S_OK;

    *class_factory = NULL;
    if (!(module = packaged ? LoadPackagedLibrary(library, 0) :
                              LoadLibraryW(library)))
        return HRESULT_FROM_WIN32(GetLastError());
    if (!(get_activation_factory =
          (void *)GetProcAddress(module, "DllGetActivationFactory")))
    {
        hr = E_FAIL;
        goto done;
    }
    if (SUCCEEDED(hr = get_activation_factory(classid, &factory)))
    {
        hr = IActivationFactory_QueryInterface(factory, iid, class_factory);
        IActivationFactory_Release(factory);
        if (SUCCEEDED(hr))
        {
            /*
             * The returned interface executes code from this module.  Keep
             * the loader reference for the process lifetime, as the existing
             * WinRT activation path has always done.
             */
            module = NULL;
        }
    }

done:
    if (module) FreeLibrary(module);
    return hr;
}

struct appx_activation_request
{
    const WCHAR *library;
    HSTRING classid;
    const IID *iid;
    IStream *stream;
    HRESULT hr;
};

struct appx_activation_host
{
    INIT_ONCE once;
    RO_INIT_TYPE init_type;
    CRITICAL_SECTION critical_section;
    HANDLE request_event;
    HANDLE completion_event;
    HANDLE ready_event;
    HANDLE thread;
    struct appx_activation_request *request;
    HRESULT hr;
};

static struct appx_activation_host appx_sta_host =
    { INIT_ONCE_STATIC_INIT, RO_INIT_SINGLETHREADED };
static struct appx_activation_host appx_mta_host =
    { INIT_ONCE_STATIC_INIT, RO_INIT_MULTITHREADED };

static DWORD WINAPI appx_activation_host_thread(void *parameter)
{
    struct appx_activation_host *host = parameter;
    struct appx_activation_request *request;
    IUnknown *object;
    DWORD wait;
    MSG msg;

    host->hr = RoInitialize(host->init_type);
    if (SUCCEEDED(host->hr) &&
        host->init_type == RO_INIT_SINGLETHREADED)
        PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(host->ready_event);
    if (FAILED(host->hr)) return 0;

    for (;;)
    {
        wait = MsgWaitForMultipleObjects(1, &host->request_event, FALSE,
                                         INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0)
        {
            request = host->request;
            object = NULL;
            request->stream = NULL;
            request->hr = activate_factory_in_current_apartment(
                request->library, TRUE, request->classid, request->iid,
                (void **)&object);
            if (SUCCEEDED(request->hr))
            {
                request->hr = CoMarshalInterThreadInterfaceInStream(
                    request->iid, object, &request->stream);
                IUnknown_Release(object);
            }
            SetEvent(host->completion_event);
        }
        else if (wait == WAIT_OBJECT_0 + 1)
        {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        else
            break;
    }
    RoUninitialize();
    return 0;
}

static BOOL CALLBACK initialize_appx_activation_host(
    INIT_ONCE *once, void *parameter, void **context)
{
    struct appx_activation_host *host = parameter;
    DWORD wait;

    InitializeCriticalSection(&host->critical_section);
    host->hr = E_OUTOFMEMORY;
    host->request_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    host->completion_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    host->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!host->request_event || !host->completion_event || !host->ready_event)
    {
        host->hr = HRESULT_FROM_WIN32(GetLastError());
        return TRUE;
    }
    if (!(host->thread = CreateThread(NULL, 0, appx_activation_host_thread,
                                      host, 0, NULL)))
    {
        host->hr = HRESULT_FROM_WIN32(GetLastError());
        return TRUE;
    }
    wait = WaitForSingleObject(host->ready_event, 10000);
    if (wait != WAIT_OBJECT_0)
        host->hr = wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) :
                                         HRESULT_FROM_WIN32(GetLastError());
    return TRUE;
}

static HRESULT activate_factory_in_host(
    UINT32 threading_model, const WCHAR *library, HSTRING classid, REFIID iid,
    void **class_factory)
{
    struct appx_activation_host *host =
        threading_model == APPX_THREADING_STA ? &appx_sta_host : &appx_mta_host;
    struct appx_activation_request request;
    DWORD index;
    HRESULT hr;

    *class_factory = NULL;
    if (!InitOnceExecuteOnce(&host->once, initialize_appx_activation_host,
                             host, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(host->hr)) return host->hr;

    EnterCriticalSection(&host->critical_section);
    request.library = library;
    request.classid = classid;
    request.iid = iid;
    request.stream = NULL;
    request.hr = E_UNEXPECTED;
    host->request = &request;
    ResetEvent(host->completion_event);
    if (!SetEvent(host->request_event))
        hr = HRESULT_FROM_WIN32(GetLastError());
    else
    {
        hr = CoWaitForMultipleHandles(COWAIT_DEFAULT, INFINITE, 1,
                                      &host->completion_event, &index);
        if (FAILED(hr))
        {
            WaitForSingleObject(host->completion_event, INFINITE);
            hr = request.hr;
        }
        else
            hr = request.hr;
        if (SUCCEEDED(hr))
        {
            hr = CoGetInterfaceAndReleaseStream(request.stream, iid,
                                                class_factory);
            request.stream = NULL;
        }
    }
    if (request.stream) IStream_Release(request.stream);
    host->request = NULL;
    LeaveCriticalSection(&host->critical_section);
    return hr;
}

static HRESULT activate_packaged_factory(
    UINT32 threading_model, const WCHAR *library, HSTRING classid, REFIID iid,
    void **class_factory)
{
    APTTYPEQUALIFIER qualifier;
    APTTYPE type;
    HRESULT hr;

    if (threading_model == APPX_THREADING_BOTH)
        return activate_factory_in_current_apartment(
            library, TRUE, classid, iid, class_factory);
    if (FAILED(hr = CoGetApartmentType(&type, &qualifier))) return hr;
    if ((threading_model == APPX_THREADING_STA &&
         (type == APTTYPE_STA || type == APTTYPE_MAINSTA)) ||
        (threading_model == APPX_THREADING_MTA && type == APTTYPE_MTA))
        return activate_factory_in_current_apartment(
            library, TRUE, classid, iid, class_factory);
    return activate_factory_in_host(threading_model, library, classid, iid,
                                    class_factory);
}

static HRESULT get_builtin_library_for_classid(const WCHAR *classid, WCHAR **out)
{
    static const struct
    {
        const WCHAR *classid;
        const WCHAR *library;
    }
    builtin_classes[] =
    {
        { L"Windows.UI.Core.CoreWindow", L"windows.ui.dll" },
        { L"Windows.UI.ViewManagement.AccessibilitySettings", L"windows.ui.dll" },
        { L"Windows.UI.ViewManagement.InputPane", L"windows.ui.dll" },
        { L"Windows.UI.ViewManagement.UISettings", L"windows.ui.dll" },
        { L"Windows.UI.ViewManagement.UIViewSettings", L"windows.ui.dll" },
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(builtin_classes); ++i)
    {
        if (!wcscmp(classid, builtin_classes[i].classid))
        {
            if (!(*out = wcsdup(builtin_classes[i].library)))
                return E_OUTOFMEMORY;
            return S_OK;
        }
    }

    return REGDB_E_CLASSNOTREG;
}

static HRESULT get_library_for_classid(const WCHAR *classid, UINT32 length,
                                       WCHAR **out, BOOL *packaged,
                                       UINT32 *threading_model)
{
    ACTCTX_SECTION_KEYED_DATA data = { sizeof(data) };
    HKEY hkey_root, hkey_class;
    DWORD type, size;
    HRESULT hr;
    WCHAR *buf = NULL;

    *out = NULL;
    *packaged = FALSE;
    *threading_model = APPX_THREADING_BOTH;

    /* search activation context first */
    if (FindActCtxSectionStringW(FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX, NULL,
            ACTIVATION_CONTEXT_SECTION_WINRT_ACTIVATABLE_CLASSES, classid, &data))
    {
        if (actctx_section_contains_data(&data, sizeof(struct activatable_class_data)))
        {
            struct activatable_class_data *activatable_class = (struct activatable_class_data *)data.lpData;
            SIZE_T module_size;

            if (activatable_class->module_len <= ~(ULONG)0 - sizeof(WCHAR)
                    && (module_size = activatable_class->module_len + sizeof(WCHAR))
                    && actctx_section_contains_range(&data, activatable_class->module_offset, module_size))
            {
                const WCHAR *ptr = (const WCHAR *)((BYTE *)data.lpSectionBase + activatable_class->module_offset);

                *out = wcsdup(ptr);
                if (data.hActCtx) ReleaseActCtx(data.hActCtx);
                return *out ? S_OK : E_OUTOFMEMORY;
            }
        }

        WARN("Ignoring invalid activation context data for class %s\n", debugstr_w(classid));
        if (data.hActCtx) ReleaseActCtx(data.hActCtx);
    }

    hr = get_appx_library_for_classid(classid, length, out, threading_model);
    if (hr != S_FALSE)
    {
        if (SUCCEEDED(hr)) *packaged = TRUE;
        return hr;
    }

    if (SUCCEEDED(hr = get_builtin_library_for_classid(classid, out)))
        return hr;

    /* load class registry key */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\WindowsRuntime\\ActivatableClassId",
                      0, KEY_READ, &hkey_root))
        return REGDB_E_READREGDB;
    if (RegOpenKeyExW(hkey_root, classid, 0, KEY_READ, &hkey_class))
    {
        WARN("Class %s not found in registry\n", debugstr_w(classid));
        RegCloseKey(hkey_root);
        return REGDB_E_CLASSNOTREG;
    }
    RegCloseKey(hkey_root);

    /* load (and expand) DllPath registry value */
    if (RegQueryValueExW(hkey_class, L"DllPath", NULL, &type, NULL, &size))
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ)
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (!(buf = malloc(size)))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (RegQueryValueExW(hkey_class, L"DllPath", NULL, NULL, (BYTE *)buf, &size))
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (type == REG_EXPAND_SZ)
    {
        WCHAR *expanded;
        DWORD len = ExpandEnvironmentStringsW(buf, NULL, 0);
        if (!(expanded = malloc(len * sizeof(WCHAR))))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        ExpandEnvironmentStringsW(buf, expanded, len);
        free(buf);
        buf = expanded;
    }

    *out = buf;
    return S_OK;

done:
    free(buf);
    RegCloseKey(hkey_class);
    return hr;
}


/***********************************************************************
 *      RoInitialize (combase.@)
 */
HRESULT WINAPI RoInitialize(RO_INIT_TYPE type)
{
    switch (type) {
    case RO_INIT_SINGLETHREADED:
        return CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    default:
        FIXME("type %d\n", type);
    case RO_INIT_MULTITHREADED:
        return CoInitializeEx(NULL, COINIT_MULTITHREADED);
    }
}

/***********************************************************************
 *      RoUninitialize (combase.@)
 */
void WINAPI RoUninitialize(void)
{
    CoUninitialize();
}

/***********************************************************************
 *      RoGetActivationFactory (combase.@)
 */
HRESULT WINAPI DECLSPEC_HOTPATCH RoGetActivationFactory(HSTRING classid, REFIID iid, void **class_factory)
{
    const WCHAR *classid_buffer;
    UINT32 classid_length, threading_model;
    BOOL embedded_null, packaged;
    WCHAR *library;
    HRESULT hr;

    TRACE("(%s, %s, %p)\n", debugstr_hstring(classid), debugstr_guid(iid), class_factory);

    if (!iid || !class_factory)
        return E_INVALIDARG;
    *class_factory = NULL;

    if (FAILED(hr = WindowsStringHasEmbeddedNull(classid, &embedded_null)))
        return hr;
    classid_buffer = WindowsGetStringRawBuffer(classid, &classid_length);
    if (embedded_null || !classid_length ||
        classid_length > WINE_APPX_GRAPH_MAX_STRING_CHARS)
        return E_INVALIDARG;

    if (FAILED(hr = ensure_mta()))
        return hr;

    hr = get_library_for_classid(classid_buffer, classid_length, &library,
                                 &packaged, &threading_model);
    if (FAILED(hr))
    {
        ERR("Failed to find library for %s\n", debugstr_hstring(classid));
        return hr;
    }

    TRACE("Found library %s for class %s\n", debugstr_w(library), debugstr_hstring(classid));
    if (packaged)
        hr = activate_packaged_factory(threading_model, library, classid, iid,
                                       class_factory);
    else
        hr = activate_factory_in_current_apartment(
            library, FALSE, classid, iid, class_factory);
    if (SUCCEEDED(hr))
        TRACE("Created interface %p\n", *class_factory);
    else
        ERR("Class %s not found in %s, hr %#lx.\n", wine_dbgstr_hstring(classid), debugstr_w(library), hr);

    free(library);
    return hr;
}

/***********************************************************************
 *      RoGetParameterizedTypeInstanceIID (combase.@)
 */
HRESULT WINAPI RoGetParameterizedTypeInstanceIID(UINT32 name_element_count, const WCHAR **name_elements,
                                                 const IRoMetaDataLocator *meta_data_locator, GUID *iid,
                                                 ROPARAMIIDHANDLE *hiid)
{
    FIXME("stub: %d %p %p %p %p\n", name_element_count, name_elements, meta_data_locator, iid, hiid);
    if (iid) *iid = GUID_NULL;
    if (hiid) *hiid = INVALID_HANDLE_VALUE;
    return E_NOTIMPL;
}

/***********************************************************************
 *      RoActivateInstance (combase.@)
 */
HRESULT WINAPI RoActivateInstance(HSTRING classid, IInspectable **instance)
{
    IActivationFactory *factory;
    HRESULT hr;

    TRACE("(%p, %p)\n", classid, instance);

    hr = RoGetActivationFactory(classid, &IID_IActivationFactory, (void **)&factory);
    if (SUCCEEDED(hr))
    {
        hr = IActivationFactory_ActivateInstance(factory, instance);
        IActivationFactory_Release(factory);
    }

    return hr;
}

struct agile_reference
{
    IAgileReference IAgileReference_iface;
    enum AgileReferenceOptions option;
    IStream *marshal_stream;
    CRITICAL_SECTION cs;
    IUnknown *obj;
    BOOLEAN is_agile;
    IUnknown *ctx;
    LONG ref;
};

static HRESULT marshal_object_in_agile_reference(struct agile_reference *ref, REFIID riid, IUnknown *obj)
{
    HRESULT hr;

    hr = CreateStreamOnHGlobal(0, TRUE, &ref->marshal_stream);
    if (FAILED(hr))
        return hr;

    hr = CoMarshalInterface(ref->marshal_stream, riid, obj, MSHCTX_INPROC, NULL, MSHLFLAGS_TABLESTRONG);
    if (FAILED(hr))
    {
        IStream_Release(ref->marshal_stream);
        ref->marshal_stream = NULL;
    }
    return hr;
}

static inline struct agile_reference *impl_from_IAgileReference(IAgileReference *iface)
{
    return CONTAINING_RECORD(iface, struct agile_reference, IAgileReference_iface);
}

static HRESULT WINAPI agile_ref_QueryInterface(IAgileReference *iface, REFIID riid, void **obj)
{
    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), obj);

    if (!riid || !obj) return E_INVALIDARG;

    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IAgileObject)
        || IsEqualGUID(riid, &IID_IAgileReference))
    {
        IUnknown_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    *obj = NULL;
    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI agile_ref_AddRef(IAgileReference *iface)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI agile_ref_Release(IAgileReference *iface)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    LONG ref = InterlockedDecrement(&impl->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);

        if (impl->obj)
            IUnknown_Release(impl->obj);

        if (impl->marshal_stream)
        {
            LARGE_INTEGER zero = {0};

            IStream_Seek(impl->marshal_stream, zero, STREAM_SEEK_SET, NULL);
            CoReleaseMarshalData(impl->marshal_stream);
            IStream_Release(impl->marshal_stream);
        }
        DeleteCriticalSection(&impl->cs);
        free(impl);
    }

    return ref;
}

struct marshal_context_params
{
    struct agile_reference *impl;
    REFIID iid;
};

static HRESULT WINAPI marshal_object_in_context(ComCallData *arg)
{
    struct marshal_context_params *params = (struct marshal_context_params *)arg;
    HRESULT hr;

    hr = marshal_object_in_agile_reference(params->impl, params->iid, params->impl->obj);
    IUnknown_Release(params->impl->obj);
    params->impl->obj = NULL;
    return hr;
}

static HRESULT WINAPI agile_ref_Resolve(IAgileReference *iface, REFIID riid, void **obj)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    LARGE_INTEGER zero = {0};
    void *cur_ctx;
    HRESULT hr;

    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), obj);

    if (impl->is_agile)
        return IUnknown_QueryInterface(impl->obj, riid, obj);

    if (FAILED(hr = CoGetContextToken((ULONG_PTR *)&cur_ctx)))
        return hr;

    EnterCriticalSection(&impl->cs);
    if (impl->option == AGILEREFERENCE_DELAYEDMARSHAL && impl->marshal_stream == NULL)
    {
        struct marshal_context_params params = { impl, riid };
        IContextCallback *ctx;

        if (FAILED(hr = IUnknown_QueryInterface(impl->ctx, &IID_IContextCallback, (void **)&ctx)))
        {
            LeaveCriticalSection(&impl->cs);
            return hr;
        }

        hr = IContextCallback_ContextCallback(ctx, marshal_object_in_context, (ComCallData *)&params,
                                              &IID_IContextCallback, 5, NULL);
        IContextCallback_Release(ctx);
        if (FAILED(hr))
        {
            LeaveCriticalSection(&impl->cs);
            return hr;
        }
    }

    if (SUCCEEDED(hr = IStream_Seek(impl->marshal_stream, zero, STREAM_SEEK_SET, NULL)))
        hr = CoUnmarshalInterface(impl->marshal_stream, riid, obj);

    LeaveCriticalSection(&impl->cs);
    return hr;
}

static const IAgileReferenceVtbl agile_ref_vtbl =
{
    agile_ref_QueryInterface,
    agile_ref_AddRef,
    agile_ref_Release,
    agile_ref_Resolve,
};

static BOOL object_has_interface(IUnknown *obj, REFIID iid)
{
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface(obj, iid, (void **)&unk);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
    return SUCCEEDED(hr);
}

/***********************************************************************
 *      RoGetAgileReference (combase.@)
 */
HRESULT WINAPI RoGetAgileReference(enum AgileReferenceOptions option, REFIID riid, IUnknown *obj,
                                   IAgileReference **agile_reference)
{
    struct apartment *apt;
    struct agile_reference *impl;
    HRESULT hr;

    TRACE("(%d, %s, %p, %p).\n", option, debugstr_guid(riid), obj, agile_reference);

    if (option != AGILEREFERENCE_DEFAULT && option != AGILEREFERENCE_DELAYEDMARSHAL)
        return E_INVALIDARG;

    if (!(apt = apartment_get_current_or_mta()))
    {
        ERR("Apartment not initialized\n");
        return CO_E_NOTINITIALIZED;
    }
    rpc_start_remoting(apt);
    apartment_release(apt);

    if (!object_has_interface(obj, riid))
        return E_NOINTERFACE;
    if (object_has_interface(obj, &IID_INoMarshal))
        return CO_E_NOT_SUPPORTED;

    impl = calloc(1, sizeof(*impl));
    if (!impl)
        return E_OUTOFMEMORY;

    impl->IAgileReference_iface.lpVtbl = &agile_ref_vtbl;
    impl->option = option;
    impl->is_agile = object_has_interface(obj, &IID_IAgileObject);
    impl->ref = 1;
    if (FAILED(hr = CoGetContextToken((ULONG_PTR *)&impl->ctx)))
    {
        free( impl );
        return hr;
    }

    if (option == AGILEREFERENCE_DELAYEDMARSHAL || impl->is_agile)
    {
        impl->obj = obj;
        IUnknown_AddRef(impl->obj);
    }
    else if (option == AGILEREFERENCE_DEFAULT)
    {
        if (FAILED(hr = marshal_object_in_agile_reference(impl, riid, obj)))
        {
            free(impl);
            return hr;
        }
    }

    InitializeCriticalSection(&impl->cs);

    *agile_reference = &impl->IAgileReference_iface;
    return S_OK;
}

/***********************************************************************
 *      RoFailFastWithErrorContextInternal2 (combase.@)
 */
void WINAPI RoFailFastWithErrorContextInternal2(HRESULT error, ULONG exception_count, /* PSTOWED_EXCEPTION_INFORMATION_V2 */void *information)
{
    FIXME("%#lx, %lu, %p stub.\n", error, exception_count, information);
    RaiseFailFastException(NULL, NULL, 0);
}

/***********************************************************************
 *      RoGetApartmentIdentifier (combase.@)
 */
HRESULT WINAPI RoGetApartmentIdentifier(UINT64 *identifier)
{
    FIXME("(%p): stub\n", identifier);

    if (!identifier)
        return E_INVALIDARG;

    *identifier = 0xdeadbeef;
    return S_OK;
}

/***********************************************************************
 *      RoRegisterForApartmentShutdown (combase.@)
 */
HRESULT WINAPI RoRegisterForApartmentShutdown(IApartmentShutdown *callback,
        UINT64 *identifier, APARTMENT_SHUTDOWN_REGISTRATION_COOKIE *cookie)
{
    HRESULT hr;

    FIXME("(%p, %p, %p): stub\n", callback, identifier, cookie);

    hr = RoGetApartmentIdentifier(identifier);
    if (FAILED(hr))
        return hr;

    if (cookie)
        *cookie = (void *)0xcafecafe;
    return S_OK;
}

/***********************************************************************
 *      RoGetServerActivatableClasses (combase.@)
 */
HRESULT WINAPI RoGetServerActivatableClasses(HSTRING name, HSTRING **classes, DWORD *count)
{
    FIXME("(%p, %p, %p): stub\n", name, classes, count);

    if (count)
        *count = 0;
    return S_OK;
}

/***********************************************************************
 *      RoRegisterActivationFactories (combase.@)
 */
HRESULT WINAPI RoRegisterActivationFactories(HSTRING *classes, PFNGETACTIVATIONFACTORY *callbacks,
                                             UINT32 count, RO_REGISTRATION_COOKIE *cookie)
{
    FIXME("(%p, %p, %d, %p): stub\n", classes, callbacks, count, cookie);

    return S_OK;
}

/***********************************************************************
 *      GetRestrictedErrorInfo (combase.@)
 */
HRESULT WINAPI GetRestrictedErrorInfo(IRestrictedErrorInfo **info)
{
    FIXME( "(%p)\n", info );
    return E_NOTIMPL;
}

/***********************************************************************
 *      SetRestrictedErrorInfo (combase.@)
 */
HRESULT WINAPI SetRestrictedErrorInfo(IRestrictedErrorInfo *info)
{
    FIXME( "(%p)\n", info );
    return E_NOTIMPL;
}

/***********************************************************************
 *      RoOriginateLanguageException (combase.@)
 */
BOOL WINAPI RoOriginateLanguageException(HRESULT error, HSTRING message, IUnknown *language_exception)
{
    FIXME("%#lx, %s, %p: stub\n", error, debugstr_hstring(message), language_exception);
    return FALSE;
}

/***********************************************************************
 *      RoOriginateError (combase.@)
 */
BOOL WINAPI RoOriginateError(HRESULT error, HSTRING message)
{
    FIXME("%#lx, %s: stub\n", error, debugstr_hstring(message));
    return FALSE;
}

/***********************************************************************
 *      RoOriginateErrorW (combase.@)
 */
BOOL WINAPI RoOriginateErrorW(HRESULT error, UINT max_len, const WCHAR *message)
{
    FIXME("%#lx, %u, %p: stub\n", error, max_len, message);
    return FALSE;
}

/***********************************************************************
 *      RoReportUnhandledError (combase.@)
 */
HRESULT WINAPI RoReportUnhandledError(IRestrictedErrorInfo *info)
{
    FIXME("(%p): stub\n", info);
    return S_OK;
}

/***********************************************************************
 *      RoSetErrorReportingFlags (combase.@)
 */
HRESULT WINAPI RoSetErrorReportingFlags(UINT32 flags)
{
    FIXME("(%08x): stub\n", flags);
    return S_OK;
}

/***********************************************************************
 *      RoGetErrorReportingFlags (combase.@)
 */
HRESULT WINAPI RoGetErrorReportingFlags(UINT32 *flags)
{
    FIXME("(%p): stub\n", flags);

    if (!flags)
        return E_POINTER;

    *flags = RO_ERROR_REPORTING_USESETERRORINFO;
    return S_OK;
}


/***********************************************************************
 *      CleanupTlsOleState (combase.@)
 */
void WINAPI CleanupTlsOleState(void *unknown)
{
    FIXME("(%p): stub\n", unknown);
}

/***********************************************************************
 *      DllGetActivationFactory (combase.@)
 */
HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    FIXME("(%s, %p): stub\n", debugstr_hstring(classid), factory);

    return REGDB_E_CLASSNOTREG;
}

/***********************************************************************
 *      RoFailFastWithErrorContext (combase.@)
 */
void WINAPI RoFailFastWithErrorContext(HRESULT hr)
{
    FIXME("(0x%08lx)\n", hr);
    RaiseFailFastException(NULL, NULL, 0);
}
