/*
 * Switchyard AMD Display Library compatibility probe.
 *
 * This verifies both the 32-bit and 64-bit ADL entry points without requiring
 * an application-provided AMD AGS library.
 */

#include <windows.h>

#include <stdio.h>
#include <string.h>

#define ADL_MAX_PATH 256
#define ADL_ERR_INVALID_ADL_IDX -5

typedef void *(__stdcall *adl_malloc_callback)(int size);

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

struct adl_memory_info2
{
    LONGLONG memory_size;
    char memory_type[ADL_MAX_PATH];
    LONGLONG memory_bandwidth;
    LONGLONG hyper_memory_size;
    LONGLONG invisible_memory_size;
    LONGLONG visible_memory_size;
};

struct adl_versions_info_x2
{
    char driver_version[ADL_MAX_PATH];
    char catalyst_version[ADL_MAX_PATH];
    char crimson_version[ADL_MAX_PATH];
    char catalyst_web_link[ADL_MAX_PATH];
};

typedef int (__cdecl *adl_create_fn)(adl_malloc_callback callback, int enum_connected_adapters,
        void **context);
typedef int (__cdecl *adl_destroy_fn)(void *context);
typedef int (__cdecl *adl_adapter_count_fn)(void *context, int *count);
typedef int (__cdecl *adl_adapter_info_legacy_fn)(void *context, struct adapter_info *infos,
        int input_size);
typedef int (__cdecl *adl_adapter_info_fn)(void *context, struct adapter_info **infos);
typedef int (__cdecl *adl_adapter_info_x4_fn)(void *context, int adapter_index,
        int *count, struct adapter_info_x2 **infos);
typedef int (__cdecl *adl_memory_info2_fn)(void *context, int adapter_index,
        struct adl_memory_info2 *info);
typedef int (__cdecl *adl_versions_fn)(void *context, struct adl_versions_info_x2 *info);

static void *__stdcall allocate(int size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

int main(int argc, char **argv)
{
    struct adl_versions_info_x2 versions = {0};
    struct adl_memory_info2 memory_info = {0};
    struct adapter_info_x2 *adapter_x2 = (void *)0xdeadbeef;
    struct adapter_info *adapter = (void *)0xdeadbeef;
    adl_adapter_info_x4_fn get_adapter_x4;
    adl_memory_info2_fn get_memory_info2;
    adl_adapter_count_fn get_count;
    adl_adapter_info_legacy_fn get_adapter_legacy;
    adl_adapter_info_fn get_adapter;
    adl_versions_fn get_versions;
    adl_destroy_fn destroy;
    adl_create_fn create;
    void *context = NULL;
    HMODULE module;
    int count = 0;
    int result;
    int failed = 0;

    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: %s ADL_DLL [unavailable]\n", argv[0]);
        return 2;
    }

    if (!(module = LoadLibraryA(argv[1])))
    {
        fprintf(stderr, "LoadLibraryA(\"%s\") failed with error %lu.\n", argv[1], GetLastError());
        return 1;
    }

    create = (void *)GetProcAddress(module, "ADL2_Main_Control_Create");
    destroy = (void *)GetProcAddress(module, "ADL2_Main_Control_Destroy");
    get_count = (void *)GetProcAddress(module, "ADL2_Adapter_NumberOfAdapters_Get");
    get_adapter_legacy = (void *)GetProcAddress(module, "ADL2_Adapter_AdapterInfo_Get");
    get_adapter = (void *)GetProcAddress(module, "ADL2_Adapter_AdapterInfoX2_Get");
    get_adapter_x4 = (void *)GetProcAddress(module, "ADL2_Adapter_AdapterInfoX4_Get");
    get_memory_info2 = (void *)GetProcAddress(module, "ADL2_Adapter_MemoryInfo2_Get");
    get_versions = (void *)GetProcAddress(module, "ADL2_Graphics_VersionsX2_Get");
    if (!create || !destroy || !get_count || !get_adapter_legacy || !get_adapter || !get_adapter_x4
            || !get_memory_info2 || !get_versions)
    {
        fprintf(stderr, "The ADL library is missing required exports.\n");
        FreeLibrary(module);
        return 1;
    }

    result = create(allocate, 1, &context);
    printf("ADL2_Main_Control_Create result: %d.\n", result);
    if (result)
    {
        FreeLibrary(module);
        return argc == 3 && !strcmp(argv[2], "unavailable") ? 0 : 1;
    }
    if (argc == 3)
    {
        fprintf(stderr, "ADL unexpectedly initialized when it should be unavailable.\n");
        destroy(context);
        FreeLibrary(module);
        return 1;
    }

    result = get_count(context, &count);
    printf("ADL2 adapter count result: %d, count: %d.\n", result, count);
    if (result || count != 0)
        failed = 1;

    result = get_adapter_legacy(context, NULL, 0);
    printf("ADL2 legacy adapter info result: %d.\n", result);
    if (result)
        failed = 1;

    result = get_adapter(context, &adapter);
    printf("ADL2 adapter info result: %d.\n", result);
    if (result || adapter)
        failed = 1;

    count = -1;
    result = get_adapter_x4(context, -1, &count, &adapter_x2);
    printf("ADL2 adapter X4 wildcard result: %d, count: %d.\n", result, count);
    if (result || count || adapter_x2)
        failed = 1;

    adapter_x2 = (void *)0xdeadbeef;
    result = get_adapter_x4(context, 0, NULL, &adapter_x2);
    printf("ADL2 adapter X4 index 0 result: %d.\n", result);
    if (result != ADL_ERR_INVALID_ADL_IDX || adapter_x2)
        failed = 1;

    result = get_memory_info2(context, 0, &memory_info);
    printf("ADL2 memory info index 0 result: %d.\n", result);
    if (result != ADL_ERR_INVALID_ADL_IDX)
        failed = 1;

    result = get_versions(context, &versions);
    printf("ADL2 versions result: %d, driver=\"%s\", Radeon Software=\"%s\".\n",
            result, versions.driver_version, versions.crimson_version);
    if (result || strcmp(versions.driver_version, "24.12.1")
            || strcmp(versions.crimson_version, "24.12.1"))
        failed = 1;

    result = destroy(context);
    printf("ADL2_Main_Control_Destroy result: %d.\n", result);
    if (result)
        failed = 1;

    FreeLibrary(module);
    return failed;
}
