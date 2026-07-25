/*
 * Switchyard AMD AGS adapter information probe.
 *
 * This loads an application-provided AMD AGS library and reports the display
 * adapter plus the driver strings returned by agsInitialize().
 */

#define COBJMACROS

#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d11.h>

#ifndef AGS_INVALID_ARGS
#define AGS_INVALID_ARGS 2
#endif
#ifndef AGS_DX_FAILURE
#define AGS_DX_FAILURE 9
#endif
#ifndef AGS_MISSING_D3D_DLL
#define AGS_MISSING_D3D_DLL 4
#endif

#include <stdio.h>
#include <string.h>

typedef void *(__stdcall *ags_alloc_callback)(size_t size);
typedef void (__stdcall *ags_free_callback)(void *ptr);

struct ags_configuration
{
    ags_alloc_callback alloc_callback;
    ags_free_callback free_callback;
};

struct ags_gpu_info
{
    const char *driver_version;
    const char *radeon_software_version;
    int device_count;
    void *devices;
};

typedef int (*ags_get_version_number_fn)(void);
typedef int (*ags_check_driver_version_fn)(const char *reported, unsigned int required);
typedef int (*ags_initialize_fn)(int version, const struct ags_configuration *configuration,
        void **context, struct ags_gpu_info *gpu_info);
typedef int (*ags_deinitialize_fn)(void *context);
typedef int (__stdcall *ags_dx11_create_device_fn)(void *context,
        const void *creation_params, const void *extension_params, void *returned_params);
typedef int (__stdcall *ags_context_only_fn)(void *context);
typedef int (__stdcall *ags_dx11_destroy_device_fn)(void *context, ID3D11Device *device,
        unsigned int *device_references, ID3D11DeviceContext *immediate_context,
        unsigned int *immediate_context_references);

struct ags_dx11_device_creation_params
{
    IDXGIAdapter *adapter;
    D3D_DRIVER_TYPE driver_type;
    HMODULE software;
    UINT flags;
    const D3D_FEATURE_LEVEL *feature_levels;
    UINT feature_level_count;
    UINT sdk_version;
    const DXGI_SWAP_CHAIN_DESC *swapchain_desc;
};

struct ags_dx11_returned_params
{
    ID3D11Device *device;
    ID3D11DeviceContext *immediate_context;
    IDXGISwapChain *swapchain;
    D3D_FEATURE_LEVEL feature_level;
    unsigned int extensions_supported;
    unsigned int crossfire_gpu_count;
    void *breadcrumb_buffer;
};

struct ags_dx12_device_creation_params
{
    IUnknown *adapter;
    IID iid;
    D3D_FEATURE_LEVEL feature_level;
};

struct ags_dx12_returned_params
{
    ID3D12Device *device;
    unsigned int extensions_supported;
};

typedef int (__stdcall *ags_dx12_create_device_fn)(void *context,
        const struct ags_dx12_device_creation_params *creation_params,
        const void *extension_params, struct ags_dx12_returned_params *returned_params);
typedef int (__stdcall *ags_dx12_destroy_device_fn)(void *context,
        ID3D12Device *device, unsigned int *device_references);
typedef int (__stdcall *ags_set_display_mode_fn)(void *context,
        int device_index, int display_index, const void *settings);

static void print_display_adapter(void)
{
    DISPLAY_DEVICEA device = {0};

    device.cb = sizeof(device);

    if (!EnumDisplayDevicesA(NULL, 0, &device, 0))
    {
        printf("EnumDisplayDevicesA failed with error %lu.\n", GetLastError());
        return;
    }

    printf("Display adapter: name=\"%s\" string=\"%s\" id=\"%s\" key=\"%s\" flags=%#lx.\n",
            device.DeviceName, device.DeviceString, device.DeviceID, device.DeviceKey, device.StateFlags);
}

int main(int argc, char **argv)
{
    ags_dx12_create_device_fn dx12_create_device;
    ags_dx12_destroy_device_fn dx12_destroy_device;
    ags_dx11_create_device_fn dx11_create_device;
    ags_dx11_destroy_device_fn dx11_destroy_device;
    ags_set_display_mode_fn set_display_mode;
    ags_context_only_fn begin_uav_overlap;
    ags_context_only_fn destroy_dx11_device_guard;
    ags_get_version_number_fn get_version_number;
    ags_check_driver_version_fn check_driver_version;
    ags_deinitialize_fn deinitialize;
    ags_initialize_fn initialize;
    struct ags_dx12_device_creation_params creation_params;
    struct ags_dx12_returned_params returned_params;
    struct ags_dx11_device_creation_params dx11_creation_params = {0};
    struct ags_dx11_returned_params dx11_returned_params = {0};
    struct ags_gpu_info gpu_info = {0};
    void *context = NULL;
    HMODULE module;
    unsigned int device_references;
    int init_only = 0;
    int version;
    int result;
    int failed = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc == 3 && !strcmp(argv[2], "init-only"))
        init_only = 1;

    if (argc < 2 || argc > 3 || (argc == 3 && !init_only))
    {
        fprintf(stderr, "usage: %s AMD_AGS_DLL [init-only]\n", argv[0]);
        return 2;
    }

    print_display_adapter();

    if (!(module = LoadLibraryA(argv[1])))
    {
        fprintf(stderr, "LoadLibraryA(\"%s\") failed with error %lu.\n", argv[1], GetLastError());
        return 1;
    }

    get_version_number = (void *)GetProcAddress(module, "agsGetVersionNumber");
    check_driver_version = (void *)GetProcAddress(module, "agsCheckDriverVersion");
    initialize = (void *)GetProcAddress(module, "agsInitialize");
    deinitialize = (void *)GetProcAddress(module, "agsDeInitialize");
    if (!get_version_number || !check_driver_version || !initialize || !deinitialize)
    {
        fprintf(stderr, "The AGS library is missing required exports.\n");
        FreeLibrary(module);
        return 1;
    }

    version = get_version_number();
    printf("AGS API version: %u.%u.%u (%#x).\n",
            (unsigned int)version >> 22,
            ((unsigned int)version >> 12) & 0x3ff,
            (unsigned int)version & 0xfff,
            (unsigned int)version);

    result = initialize(version, NULL, &context, &gpu_info);
    printf("agsInitialize result: %d.\n", result);
    if (!result)
    {
        printf("AMD internal driver version: \"%s\".\n",
                gpu_info.driver_version ? gpu_info.driver_version : "(null)");
        printf("Radeon Software version: \"%s\".\n",
                gpu_info.radeon_software_version ? gpu_info.radeon_software_version : "(null)");
        printf("Radeon Software 24.12.1 check result: %d.\n",
                check_driver_version(gpu_info.radeon_software_version,
                        (24u << 22) | (12u << 12) | 1u));
        printf("AGS device count: %d.\n", gpu_info.device_count);

        if (init_only)
        {
            printf("Skipping driver extension probes.\n");
        }
        else
        {
        dx12_create_device = (void *)GetProcAddress(module, "agsDriverExtensionsDX12_CreateDevice");
        dx12_destroy_device = (void *)GetProcAddress(module, "agsDriverExtensionsDX12_DestroyDevice");
        dx11_create_device = (void *)GetProcAddress(module, "agsDriverExtensionsDX11_CreateDevice");
        dx11_destroy_device = (void *)GetProcAddress(module, "agsDriverExtensionsDX11_DestroyDevice");
        begin_uav_overlap = (void *)GetProcAddress(module, "agsDriverExtensionsDX11_BeginUAVOverlap");
        set_display_mode = (void *)GetProcAddress(module, "agsSetDisplayMode");
        destroy_dx11_device_guard = (void *)dx11_destroy_device;
        if (!dx11_create_device || !dx11_destroy_device)
        {
            fprintf(stderr, "The AGS library is missing required DX11 device exports.\n");
            failed = 1;
        }
        else
        {
            result = dx11_create_device(context, NULL, NULL, NULL);
            printf("agsDriverExtensionsDX11_CreateDevice(null arguments) result: %d.\n", result);
            if (result != AGS_INVALID_ARGS)
                failed = 1;

            dx11_creation_params.driver_type = D3D_DRIVER_TYPE_UNKNOWN;
            dx11_creation_params.sdk_version = D3D11_SDK_VERSION;
            result = dx11_create_device(context, &dx11_creation_params, NULL, &dx11_returned_params);
            printf("agsDriverExtensionsDX11_CreateDevice(optional params null) result: %d.\n", result);
            if (result != 0 && result != AGS_MISSING_D3D_DLL && result != AGS_DX_FAILURE)
                failed = 1;
            else if (!result)
            {
                unsigned int device_refs = ~0u, context_refs = ~0u;

                if (!dx11_returned_params.device || !dx11_returned_params.immediate_context)
                {
                    fprintf(stderr, "Successful DX11 creation returned incomplete objects.\n");
                    failed = 1;
                }
                else
                {
                    result = dx11_destroy_device(context, dx11_returned_params.device, &device_refs,
                            dx11_returned_params.immediate_context, &context_refs);
                    printf("agsDriverExtensionsDX11_DestroyDevice result: %d, device references=%u, "
                            "context references=%u.\n", result, device_refs, context_refs);
                    if (result)
                        failed = 1;
                }
            }
        }

        if (!begin_uav_overlap || !destroy_dx11_device_guard)
        {
            fprintf(stderr, "The AGS library is missing required variable-signature DX11 exports.\n");
            failed = 1;
        }
        else
        {
            result = begin_uav_overlap(NULL);
            printf("agsDriverExtensionsDX11_BeginUAVOverlap(null context) result: %d.\n", result);
            if (result != AGS_INVALID_ARGS)
                failed = 1;

            result = destroy_dx11_device_guard(NULL);
            printf("agsDriverExtensionsDX11_DestroyDevice(null context) result: %d.\n", result);
            if (result != AGS_INVALID_ARGS)
                failed = 1;
        }

        if (!set_display_mode)
        {
            fprintf(stderr, "The AGS library is missing agsSetDisplayMode.\n");
            failed = 1;
        }
        else
        {
            result = set_display_mode(context, 0, 0, NULL);
            printf("agsSetDisplayMode(null settings) result: %d.\n", result);
            if (result != AGS_INVALID_ARGS)
                failed = 1;
        }

        if (!dx12_create_device || !dx12_destroy_device)
        {
            fprintf(stderr, "The AGS library is missing required DX12 exports.\n");
            failed = 1;
        }
        else
        {
            creation_params.adapter = NULL;
            creation_params.iid = IID_ID3D12Device;
            creation_params.feature_level = D3D_FEATURE_LEVEL_12_0;
            returned_params.device = NULL;
            returned_params.extensions_supported = ~0u;

            result = dx12_create_device(context, &creation_params, NULL, &returned_params);
            printf("agsDriverExtensionsDX12_CreateDevice result: %d, device=%p, extensions=%#x.\n",
                    result, returned_params.device, returned_params.extensions_supported);
            if (result || !returned_params.device || returned_params.extensions_supported)
                failed = 1;

            if (returned_params.device)
            {
                device_references = ~0u;
                result = dx12_destroy_device(context, returned_params.device, &device_references);
                printf("agsDriverExtensionsDX12_DestroyDevice result: %d, references=%u.\n",
                        result, device_references);
                if (result)
                    failed = 1;
            }
        }
        }

        if (deinitialize(context))
            failed = 1;
    }
    else
    {
        failed = 1;
    }

    FreeLibrary(module);
    return failed;
}
