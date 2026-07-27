/*
 * AMD Display Library compatibility support
 *
 * Copyright 2026 Jungwuk
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
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(atiadl);

#define ADL_OK                         0
#define ADL_ERR                       -1
#define ADL_ERR_INVALID_PARAM         -3
#define ADL_ERR_INVALID_PARAM_SIZE    -4
#define ADL_ERR_INVALID_ADL_IDX       -5
#define ADL_ERR_NOT_SUPPORTED         -8

#define ADL_MAX_PATH 256

/*
 * D3DMetal intentionally presents an AMD-compatible DXGI adapter. Radeon
 * software version checks use ADL separately, so expose the oldest baseline
 * currently required by that compatibility path instead of claiming support
 * for capabilities from an arbitrarily newer Radeon driver.
 */
static const char compatibility_driver_version[] = "24.12.1";

typedef void *(CALLBACK *adl_malloc_callback)(int size);

struct adapter_info
{
    int size;
    int adapter_index;
    char udid[ADL_MAX_PATH];
    int bus_number;
    int device_number;
    int function_number;
    int vendor_id;
    char adapter_name[ADL_MAX_PATH];
    char display_name[ADL_MAX_PATH];
    int present;
    int exists;
    char driver_path[ADL_MAX_PATH];
    char driver_path_ext[ADL_MAX_PATH];
    char pnp_string[ADL_MAX_PATH];
    int os_display_index;
};

struct adapter_info_x2
{
    struct adapter_info info;
    int info_mask;
    int info_value;
};

struct adl_memory_info
{
    LONGLONG memory_size;
    char memory_type[ADL_MAX_PATH];
    LONGLONG memory_bandwidth;
};

struct adl_memory_info2
{
    LONGLONG memory_size;
    char memory_type[ADL_MAX_PATH];
    LONGLONG memory_bandwidth;
    LONGLONG hyper_memory_size;
    LONGLONG invisible_memory_size;
    LONGLONG visible_memory_size;
};

struct adl_versions_info
{
    char driver_version[ADL_MAX_PATH];
    char catalyst_version[ADL_MAX_PATH];
    char catalyst_web_link[ADL_MAX_PATH];
};

struct adl_versions_info_x2
{
    char driver_version[ADL_MAX_PATH];
    char catalyst_version[ADL_MAX_PATH];
    char crimson_version[ADL_MAX_PATH];
    char catalyst_web_link[ADL_MAX_PATH];
};

struct adl_context
{
    adl_malloc_callback malloc_callback;
};

static struct adl_context *legacy_context;

static BOOL is_switchyard_gptk_runtime(void)
{
    WCHAR *gptk_path = NULL, *wine_dll_path = NULL;
    DWORD gptk_path_size, wine_dll_path_size;
    BOOL ret = FALSE;

    gptk_path_size = GetEnvironmentVariableW(L"SWITCHYARD_GPTK_PATH", NULL, 0);
    wine_dll_path_size = GetEnvironmentVariableW(L"WINEDLLPATH", NULL, 0);
    if (!gptk_path_size || !wine_dll_path_size)
        return FALSE;

    if (!(gptk_path = HeapAlloc(GetProcessHeap(), 0, gptk_path_size * sizeof(*gptk_path)))
            || !(wine_dll_path = HeapAlloc(GetProcessHeap(), 0,
                    wine_dll_path_size * sizeof(*wine_dll_path))))
        goto done;

    if (GetEnvironmentVariableW(L"SWITCHYARD_GPTK_PATH", gptk_path, gptk_path_size)
                != gptk_path_size - 1
            || GetEnvironmentVariableW(L"WINEDLLPATH", wine_dll_path, wine_dll_path_size)
                != wine_dll_path_size - 1)
        goto done;

    /*
     * ADL is queried while some applications are still constructing their
     * first DXGI factory.  Calling back into DXGI here recursively initializes
     * D3DMetal and can leave the application's graphics device unusable.
     *
     * Switchyard selects D3DMetal by putting the selected GPTK Wine directory
     * first in WINEDLLPATH.  Use that already-established launch decision
     * instead of touching any graphics API from the driver information shim.
     */
    ret = wcsstr(wine_dll_path, gptk_path)
            && (wcsstr(wine_dll_path, L"/redist/lib/wine")
                || wcsstr(wine_dll_path, L"\\redist\\lib\\wine"));

done:
    HeapFree(GetProcessHeap(), 0, wine_dll_path);
    HeapFree(GetProcessHeap(), 0, gptk_path);
    return ret;
}

static BOOL needs_compatibility_driver_version(void)
{
    WCHAR process_path[MAX_PATH], setting[2];
    const WCHAR *process_name;
    DWORD length;

    /*
     * This synthetic Radeon version exists for GTA V Enhanced's minimum
     * driver check.  Advertising ADL to unrelated applications can make them
     * select Radeon driver-private entry points which D3DMetal does not
     * provide.  In particular, Overwatch then calls a missing atidxx64 entry
     * point instead of using the ordinary D3D11 path.
     *
     * Keep an explicit opt-in for probes and future narrowly-scoped launch
     * policy.  Any non-empty value other than "1" explicitly disables it.
     */
    length = GetEnvironmentVariableW(L"SWITCHYARD_ADL_COMPAT", setting, ARRAY_SIZE(setting));
    if (length)
        return length == 1 && setting[0] == L'1';

    length = GetModuleFileNameW(NULL, process_path, ARRAY_SIZE(process_path));
    if (!length || length >= ARRAY_SIZE(process_path))
        return FALSE;

    process_name = wcsrchr(process_path, L'\\');
    process_name = process_name ? process_name + 1 : process_path;

    return !lstrcmpiW(process_name, L"GTA5_Enhanced.exe")
            || !lstrcmpiW(process_name, L"PlayGTAV.exe");
}

static int create_context(adl_malloc_callback callback, struct adl_context **context)
{
    struct adl_context *object;

    if (!callback || !context)
        return ADL_ERR_INVALID_PARAM;

    *context = NULL;
    if (!is_switchyard_gptk_runtime())
    {
        TRACE("No Switchyard GPTK runtime was selected.\n");
        return ADL_ERR;
    }

    if (!needs_compatibility_driver_version())
    {
        TRACE("Synthetic Radeon driver information is disabled for this process.\n");
        return ADL_ERR;
    }

    if (!(object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object))))
        return ADL_ERR;

    object->malloc_callback = callback;
    *context = object;

    TRACE("Created version-information context %p without a physical AMD adapter.\n", object);
    return ADL_OK;
}

static int destroy_context(struct adl_context *context)
{
    if (!context)
        return ADL_ERR_INVALID_PARAM;

    HeapFree(GetProcessHeap(), 0, context);
    return ADL_OK;
}

static void fill_versions_info(struct adl_versions_info *info)
{
    memset(info, 0, sizeof(*info));
    strcpy(info->driver_version, compatibility_driver_version);
    strcpy(info->catalyst_version, compatibility_driver_version);
}

static void fill_versions_info_x2(struct adl_versions_info_x2 *info)
{
    memset(info, 0, sizeof(*info));
    strcpy(info->driver_version, compatibility_driver_version);
    strcpy(info->catalyst_version, compatibility_driver_version);
    strcpy(info->crimson_version, compatibility_driver_version);
}

int CDECL ADL2_Main_Control_Create(adl_malloc_callback callback, int enum_connected_adapters,
        struct adl_context **context)
{
    TRACE("callback %p, enum_connected_adapters %d, context %p.\n",
            callback, enum_connected_adapters, context);

    return create_context(callback, context);
}

int CDECL ADL2_Main_Control_Destroy(struct adl_context *context)
{
    TRACE("context %p.\n", context);

    return destroy_context(context);
}

int CDECL ADL2_Adapter_NumberOfAdapters_Get(struct adl_context *context, int *count)
{
    TRACE("context %p, count %p.\n", context, count);

    if (!context || !count)
        return ADL_ERR_INVALID_PARAM;

    *count = 0;
    return ADL_OK;
}

int CDECL ADL2_Adapter_AdapterInfoX2_Get(struct adl_context *context,
        struct adapter_info **infos)
{
    TRACE("context %p, infos %p.\n", context, infos);

    if (!context || !infos)
        return ADL_ERR_INVALID_PARAM;

    *infos = NULL;
    return ADL_OK;
}

int CDECL ADL2_Adapter_AdapterInfo_Get(struct adl_context *context,
        struct adapter_info *infos, int input_size)
{
    TRACE("context %p, infos %p, input_size %d.\n", context, infos, input_size);

    if (!context)
        return ADL_ERR_INVALID_PARAM;
    if (input_size)
        return ADL_ERR_INVALID_PARAM_SIZE;

    return ADL_OK;
}

int CDECL ADL2_Adapter_AdapterInfoX4_Get(struct adl_context *context, int adapter_index,
        int *count, struct adapter_info_x2 **infos)
{
    TRACE("context %p, adapter_index %d, count %p, infos %p.\n",
            context, adapter_index, count, infos);

    if (!context || !infos)
        return ADL_ERR_INVALID_PARAM;

    *infos = NULL;
    if (count)
        *count = 0;
    return adapter_index == -1 ? ADL_OK : ADL_ERR_INVALID_ADL_IDX;
}

int CDECL ADL2_Adapter_MemoryInfo_Get(struct adl_context *context, int adapter_index,
        struct adl_memory_info *info)
{
    TRACE("context %p, adapter_index %d, info %p.\n", context, adapter_index, info);

    if (!context || !info)
        return ADL_ERR_INVALID_PARAM;
    return ADL_ERR_INVALID_ADL_IDX;
}

int CDECL ADL2_Adapter_MemoryInfo2_Get(struct adl_context *context, int adapter_index,
        struct adl_memory_info2 *info)
{
    TRACE("context %p, adapter_index %d, info %p.\n", context, adapter_index, info);

    if (!context || !info)
        return ADL_ERR_INVALID_PARAM;
    return ADL_ERR_INVALID_ADL_IDX;
}

int CDECL ADL2_Graphics_Versions_Get(struct adl_context *context,
        struct adl_versions_info *info)
{
    TRACE("context %p, info %p.\n", context, info);

    if (!context || !info)
        return ADL_ERR_INVALID_PARAM;

    fill_versions_info(info);
    return ADL_OK;
}

int CDECL ADL2_Graphics_VersionsX2_Get(struct adl_context *context,
        struct adl_versions_info_x2 *info)
{
    TRACE("context %p, info %p.\n", context, info);

    if (!context || !info)
        return ADL_ERR_INVALID_PARAM;

    fill_versions_info_x2(info);
    return ADL_OK;
}

int CDECL ADL_Main_Control_Create(adl_malloc_callback callback, int enum_connected_adapters)
{
    TRACE("callback %p, enum_connected_adapters %d.\n", callback, enum_connected_adapters);

    if (legacy_context)
        return ADL_ERR;

    return create_context(callback, &legacy_context);
}

int CDECL ADL_Main_Control_Destroy(void)
{
    struct adl_context *context = legacy_context;

    TRACE("legacy context %p.\n", context);

    legacy_context = NULL;
    return destroy_context(context);
}

int CDECL ADL_Adapter_NumberOfAdapters_Get(int *count)
{
    TRACE("count %p.\n", count);

    if (!legacy_context || !count)
        return ADL_ERR_INVALID_PARAM;

    *count = 0;
    return ADL_OK;
}

int CDECL ADL_Adapter_AdapterInfo_Get(struct adapter_info *info, int input_size)
{
    TRACE("info %p, input_size %d.\n", info, input_size);

    if (!legacy_context)
        return ADL_ERR_INVALID_PARAM;
    if (input_size)
        return ADL_ERR_INVALID_PARAM_SIZE;

    return ADL_OK;
}

int CDECL ADL_Graphics_Versions_Get(struct adl_versions_info *info)
{
    TRACE("info %p.\n", info);

    if (!legacy_context || !info)
        return ADL_ERR_INVALID_PARAM;

    fill_versions_info(info);
    return ADL_OK;
}

int CDECL ADL_Graphics_VersionsX2_Get(struct adl_versions_info_x2 *info)
{
    TRACE("info %p.\n", info);

    if (!legacy_context || !info)
        return ADL_ERR_INVALID_PARAM;

    fill_versions_info_x2(info);
    return ADL_OK;
}

#define ADL_UNSUPPORTED(name) \
    int CDECL name(void *context, ...) \
    { \
        TRACE("context %p: unsupported.\n", context); \
        return ADL_ERR_NOT_SUPPORTED; \
    }

ADL_UNSUPPORTED(ADL2_Adapter_Graphic_Core_Info_Get)
ADL_UNSUPPORTED(ADL2_GcnAsicInfo_Get)
ADL_UNSUPPORTED(ADL2_Adapter_ASICFamilyType_Get)
ADL_UNSUPPORTED(ADL_Adapter_ObservedGameClockInfo_Get)
ADL_UNSUPPORTED(ADL2_Overdrive_Caps)
ADL_UNSUPPORTED(ADL2_Overdrive6_Capabilities_Get)
ADL_UNSUPPORTED(ADL2_Overdrive6_StateInfo_Get)
ADL_UNSUPPORTED(ADL2_OverdriveN_Capabilities_Get)
ADL_UNSUPPORTED(ADL2_OverdriveN_SystemClocks_Get)
ADL_UNSUPPORTED(ADL2_OverdriveN_MemoryClocks_Get)
ADL_UNSUPPORTED(ADL2_OverdriveN_PowerLimit_Get)
ADL_UNSUPPORTED(ADL2_OverdriveN_CapabilitiesX2_Get)
ADL_UNSUPPORTED(ADL2_OverdriveN_SystemClocksX2_Get)
ADL_UNSUPPORTED(ADL2_OverdriveN_MemoryClocksX2_Get)
ADL_UNSUPPORTED(ADL2_Overdrive8_Init_Setting_Get)
ADL_UNSUPPORTED(ADL2_Display_DisplayMapConfig_Get)
ADL_UNSUPPORTED(ADL2_Display_SLSMapIndex_Get)
ADL_UNSUPPORTED(ADL2_Display_SLSMapConfig_Get)
ADL_UNSUPPORTED(ADL2_Display_Modes_Get)
ADL_UNSUPPORTED(ADL2_Display_SourceContentAttribute_Get)
ADL_UNSUPPORTED(ADL2_Display_SourceContentAttribute_Set)
ADL_UNSUPPORTED(ADL2_Display_DDCInfo2_Get)
ADL_UNSUPPORTED(ADL2_Display_DisplayInfo_Get)
ADL_UNSUPPORTED(ADL2_Display_FreeSync_Cap)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    else if (reason == DLL_PROCESS_DETACH && !reserved && legacy_context)
        destroy_context(legacy_context);

    return TRUE;
}
