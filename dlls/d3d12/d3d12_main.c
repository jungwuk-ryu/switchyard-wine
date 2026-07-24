/*
 * Copyright 2018 Józef Kucia for CodeWeavers
 * Copyright 2020 Joshua Ashton for Valve Software
 * Copyright 2023 Hans-Kristian Arntzen for Valve Corporation
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

#define COBJMACROS
#define VKD3D_NO_VULKAN_H
#define VKD3D_NO_WIN32_TYPES

#include "windef.h"
#include "dxgi1_6.h"
#include "d3d12.h"
#include "wine/vulkan.h"

#include <vkd3d.h>

#include "initguid.h"
#include "dxcore.h"
#include "wine/wined3d.h"
#include "wine/winedxgi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d12);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

static const GUID clsid_d3d12_device_factory =
{0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};

static HMODULE vulkan_module;

struct d3d12_metal_backend
{
    HMODULE module;
    HRESULT (WINAPI *create_device)(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level,
            REFIID iid, void **device);
    HRESULT (WINAPI *create_root_signature_deserializer)(const void *data, SIZE_T data_size,
            REFIID iid, void **deserializer);
    HRESULT (WINAPI *create_versioned_root_signature_deserializer)(const void *data,
            SIZE_T data_size, REFIID iid, void **deserializer);
    HRESULT (WINAPI *enable_experimental_features)(UINT feature_count, const IID *iids,
            void *configurations, UINT *configuration_sizes);
    HRESULT (WINAPI *get_debug_interface)(REFIID iid, void **debug);
    PFN_D3D12_GET_INTERFACE get_interface;
    HRESULT (WINAPI *serialize_root_signature)(const D3D12_ROOT_SIGNATURE_DESC *desc,
            D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error_blob);
    HRESULT (WINAPI *serialize_versioned_root_signature)(
            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
            ID3DBlob **blob, ID3DBlob **error_blob);
};

static struct d3d12_metal_backend d3d12_metal;
static HMODULE d3d12_core_module;
static UINT *d3d12_core_sdk_version;

static BOOL WINAPI load_d3d12_core_once(INIT_ONCE *once, void *param, void **context)
{
    const UINT *application_sdk_version;
    HMODULE application;

    d3d12_core_module = LoadLibraryExW(L"d3d12core.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d3d12_core_module)
    {
        WARN("Failed to load the system D3D12 core module, error %lu.\n", GetLastError());
        return TRUE;
    }

    d3d12_core_sdk_version = (UINT *)GetProcAddress(d3d12_core_module, "D3D12SDKVersion");
    application = GetModuleHandleW(NULL);
    application_sdk_version = (const UINT *)GetProcAddress(application, "D3D12SDKVersion");
    if (d3d12_core_sdk_version && application_sdk_version)
    {
        *d3d12_core_sdk_version = *application_sdk_version;
        TRACE("Using application D3D12 SDK version %u.\n", *application_sdk_version);
    }

    return TRUE;
}

static void load_d3d12_core(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

    InitOnceExecuteOnce(&init_once, load_d3d12_core_once, NULL, NULL);
}

static void set_d3d12_core_sdk_version(UINT version)
{
    load_d3d12_core();
    if (d3d12_core_sdk_version)
        InterlockedExchange((LONG *)d3d12_core_sdk_version, version);
}

static BOOL WINAPI load_d3d12_metal_once(INIT_ONCE *once, void *param, void **context)
{
    HMODULE module;

    module = LoadLibraryExW(L"d3dmt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
    {
        TRACE("D3DMetal backend is not installed, using the Wine D3D12 backend.\n");
        return TRUE;
    }
    if (module == GetModuleHandleW(L"d3d12.dll"))
    {
        WARN("Ignoring a D3DMetal backend alias that resolved to the D3D12 proxy itself.\n");
        FreeLibrary(module);
        return TRUE;
    }

    d3d12_metal.create_device = (void *)GetProcAddress(module, "D3D12CreateDevice");
    d3d12_metal.create_root_signature_deserializer =
            (void *)GetProcAddress(module, "D3D12CreateRootSignatureDeserializer");
    d3d12_metal.create_versioned_root_signature_deserializer =
            (void *)GetProcAddress(module, "D3D12CreateVersionedRootSignatureDeserializer");
    d3d12_metal.enable_experimental_features =
            (void *)GetProcAddress(module, "D3D12EnableExperimentalFeatures");
    d3d12_metal.get_debug_interface =
            (void *)GetProcAddress(module, "D3D12GetDebugInterface");
    d3d12_metal.get_interface = (void *)GetProcAddress(module, "D3D12GetInterface");
    d3d12_metal.serialize_root_signature =
            (void *)GetProcAddress(module, "D3D12SerializeRootSignature");
    d3d12_metal.serialize_versioned_root_signature =
            (void *)GetProcAddress(module, "D3D12SerializeVersionedRootSignature");

    if (!d3d12_metal.create_device)
    {
        WARN("Ignoring D3DMetal backend without D3D12CreateDevice.\n");
        memset(&d3d12_metal, 0, sizeof(d3d12_metal));
        FreeLibrary(module);
        return TRUE;
    }

    d3d12_metal.module = module;
    TRACE("Loaded D3DMetal through the Agility-compatible D3D12 proxy.\n");
    return TRUE;
}

static BOOL switchyard_is_chromium_gpu_process(void)
{
    const WCHAR *command_line = GetCommandLineW();

    if (!command_line || !wcsstr(command_line, L"--type=gpu-process"))
        return FALSE;

    return wcsstr(command_line, L"--enable-chrome-runtime")
            || wcsstr(command_line, L"--user-agent-product")
            || wcsstr(command_line, L"--mojo-platform-channel-handle");
}

static const struct d3d12_metal_backend *get_d3d12_metal_backend(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

    if (switchyard_is_chromium_gpu_process())
    {
        TRACE("Keeping the Chromium GPU process on the Wine D3D12 fallback.\n");
        return NULL;
    }

    InitOnceExecuteOnce(&init_once, load_d3d12_metal_once, NULL, NULL);
    return d3d12_metal.module ? &d3d12_metal : NULL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}

/* FIXME: We should unload vulkan-1.dll. */
static BOOL WINAPI load_vulkan_dll_once(INIT_ONCE *once, void *param, void **context)
{
    vulkan_module = LoadLibraryA("vulkan-1.dll");
    return TRUE;
}

static PFN_vkGetInstanceProcAddr load_vulkan(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

    InitOnceExecuteOnce(&init_once, load_vulkan_dll_once, NULL, NULL);

    if (vulkan_module)
        return (void *)GetProcAddress(vulkan_module, "vkGetInstanceProcAddr");

    return NULL;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID iid, void **debug)
{
    const struct d3d12_metal_backend *backend;

    TRACE("iid %s, debug %p.\n", debugstr_guid(iid), debug);

    if ((backend = get_d3d12_metal_backend()) && backend->get_debug_interface)
        return backend->get_debug_interface(iid, debug);

    WARN("Returning DXGI_ERROR_SDK_COMPONENT_MISSING.\n");
    return DXGI_ERROR_SDK_COMPONENT_MISSING;
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT feature_count,
        const IID *iids, void *configurations, UINT *configurations_sizes)
{
    const struct d3d12_metal_backend *backend;

    if ((backend = get_d3d12_metal_backend()) && backend->enable_experimental_features)
        return backend->enable_experimental_features(feature_count, iids, configurations,
                configurations_sizes);

    FIXME("feature_count %u, iids %p, configurations %p, configurations_sizes %p stub!\n",
            feature_count, iids, configurations, configurations_sizes);
    return E_NOINTERFACE;
}

static HRESULT d3d12_signal_event(HANDLE event)
{
    return SetEvent(event) ? S_OK : E_FAIL;
}

static HRESULT d3d12_get_dxgi_adapter_for_dxcore_adapter(IDXCoreAdapter *adapter,
        IDXGIAdapter **dxgi_adapter)
{
    IDXGIFactory4 *factory;
    HRESULT hr;
    LUID luid;

    if (FAILED(hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(luid), &luid)))
    {
        WARN("Failed to get LUID for dxcore adapter, hr %#lx.\n", hr);
        return hr;
    }

    if (FAILED(hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory)))
    {
        WARN("Failed to create DXGI factory, hr %#lx.\n", hr);
        return hr;
    }

    if (FAILED(hr = IDXGIFactory4_EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter,
            (void **)dxgi_adapter)))
    {
        WARN("Failed to enumerate adapter by LUID, hr %#lx.\n", hr);
    }

    IDXGIFactory4_Release(factory);

    return hr;
}

static HRESULT d3d12_get_adapter(IWineDXGIAdapter **wine_adapter, IUnknown *adapter)
{
    IDXGIAdapter *dxgi_adapter = NULL;
    IDXGIFactory4 *factory = NULL;
    IDXCoreAdapter *dxcore_adapter;
    HRESULT hr;

    if (!adapter)
    {
        if (FAILED(hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory)))
        {
            WARN("Failed to create DXGI factory, hr %#lx.\n", hr);
            goto done;
        }

        if (FAILED(hr = IDXGIFactory4_EnumAdapters(factory, 0, &dxgi_adapter)))
        {
            WARN("Failed to enumerate primary adapter, hr %#lx.\n", hr);
            goto done;
        }

        adapter = (IUnknown *)dxgi_adapter;
    }
    else if (SUCCEEDED(IUnknown_QueryInterface(adapter, &IID_IDXCoreAdapter, (void **)&dxcore_adapter)))
    {
        if (FAILED(hr = d3d12_get_dxgi_adapter_for_dxcore_adapter(dxcore_adapter, &dxgi_adapter)))
        {
            WARN("Failed to create DXGI adapter for DXCore adapter, hr %#lx.\n", hr);
            goto done;
        }
        IDXCoreAdapter_Release(dxcore_adapter);

        adapter = (IUnknown *)dxgi_adapter;
    }

    if (FAILED(hr = IUnknown_QueryInterface(adapter, &IID_IWineDXGIAdapter, (void **)wine_adapter)))
        WARN("Invalid adapter %p, hr %#lx.\n", adapter, hr);

done:
    if (dxgi_adapter)
        IDXGIAdapter_Release(dxgi_adapter);
    if (factory)
        IDXGIFactory4_Release(factory);

    return hr;
}

static BOOL check_vk_instance_extension(VkInstance vk_instance,
        PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr, const char *name)
{
    PFN_vkEnumerateInstanceExtensionProperties pfn_vkEnumerateInstanceExtensionProperties;
    VkExtensionProperties *properties;
    BOOL ret = FALSE;
    unsigned int i;
    uint32_t count;

    pfn_vkEnumerateInstanceExtensionProperties
            = (void *)pfn_vkGetInstanceProcAddr(vk_instance, "vkEnumerateInstanceExtensionProperties");

    if (pfn_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) < 0)
        return FALSE;

    if (!(properties = calloc(count, sizeof(*properties))))
        return FALSE;

    if (pfn_vkEnumerateInstanceExtensionProperties(NULL, &count, properties) >= 0)
    {
        for (i = 0; i < count; ++i)
        {
            if (!strcmp(properties[i].extensionName, name))
            {
                ret = TRUE;
                break;
            }
        }
    }

    free(properties);
    return ret;
}

static VkPhysicalDevice d3d12_get_vk_physical_device(struct vkd3d_instance *instance,
        PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr, const struct wine_dxgi_adapter_info *adapter_info)
{
    PFN_vkGetPhysicalDeviceProperties2 pfn_vkGetPhysicalDeviceProperties2 = NULL;
    PFN_vkGetPhysicalDeviceProperties pfn_vkGetPhysicalDeviceProperties;
    PFN_vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices;
    VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceIDProperties id_properties;
    VkPhysicalDeviceProperties2 properties2;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDevice *vk_physical_devices;
    VkInstance vk_instance;
    unsigned int i;
    uint32_t count;
    VkResult vr;

    vk_instance = vkd3d_instance_get_vk_instance(instance);

    pfn_vkEnumeratePhysicalDevices = (void *)pfn_vkGetInstanceProcAddr(vk_instance, "vkEnumeratePhysicalDevices");

    pfn_vkGetPhysicalDeviceProperties = (void *)pfn_vkGetInstanceProcAddr(vk_instance, "vkGetPhysicalDeviceProperties");
    if (check_vk_instance_extension(vk_instance, pfn_vkGetInstanceProcAddr, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        pfn_vkGetPhysicalDeviceProperties2 = (void *)pfn_vkGetInstanceProcAddr(vk_instance, "vkGetPhysicalDeviceProperties2KHR");

    if ((vr = pfn_vkEnumeratePhysicalDevices(vk_instance, &count, NULL)) < 0)
    {
        WARN("Failed to get device count, vr %d.\n", vr);
        return VK_NULL_HANDLE;
    }
    if (!count)
    {
        WARN("No physical device available.\n");
        return VK_NULL_HANDLE;
    }

    if (!(vk_physical_devices = calloc(count, sizeof(*vk_physical_devices))))
        return VK_NULL_HANDLE;

    if ((vr = pfn_vkEnumeratePhysicalDevices(vk_instance, &count, vk_physical_devices)) < 0)
        goto done;

    if (!IsEqualGUID(&adapter_info->driver_uuid, &GUID_NULL) && pfn_vkGetPhysicalDeviceProperties2
            && check_vk_instance_extension(vk_instance, pfn_vkGetInstanceProcAddr,
                    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME))
    {
        TRACE("Matching adapters by UUIDs.\n");

        for (i = 0; i < count; ++i)
        {
            memset(&id_properties, 0, sizeof(id_properties));
            id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

            properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties2.pNext = &id_properties;

            pfn_vkGetPhysicalDeviceProperties2(vk_physical_devices[i], &properties2);

            if (!memcmp(id_properties.driverUUID, &adapter_info->driver_uuid, VK_UUID_SIZE)
                    && !memcmp(id_properties.deviceUUID, &adapter_info->device_uuid, VK_UUID_SIZE))
            {
                vk_physical_device = vk_physical_devices[i];
                break;
            }
        }
    }

    if (!vk_physical_device)
    {
        WARN("Matching adapters by PCI IDs.\n");

        for (i = 0; i < count; ++i)
        {
            pfn_vkGetPhysicalDeviceProperties(vk_physical_devices[i], &properties);

            if (properties.vendorID == adapter_info->vendor_id && properties.deviceID == adapter_info->device_id)
            {
                vk_physical_device = vk_physical_devices[i];
                break;
            }
        }
    }

    if (!vk_physical_device)
        FIXME("Could not find Vulkan physical device for DXGI adapter.\n");

done:
    free(vk_physical_devices);
    return vk_physical_device;
}

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level,
        REFIID iid, void **device)
{
    const struct d3d12_metal_backend *backend;
    IUnknown *probe_device = NULL;
    struct vkd3d_optional_instance_extensions_info optional_extensions_info;
    struct vkd3d_instance_create_info instance_create_info;
    PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr;
    struct vkd3d_device_create_info device_create_info;
    struct wine_dxgi_adapter_info adapter_info;
    struct vkd3d_instance *instance;
    IWineDXGIAdapter *wine_adapter;
    HRESULT hr;

    static const char * const instance_extensions[] =
    {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    static const char * const optional_instance_extensions[] =
    {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    static const char * const device_extensions[] =
    {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    static const struct vkd3d_application_info application_info =
    {
        .type = VKD3D_STRUCTURE_TYPE_APPLICATION_INFO,
        .api_version = VKD3D_API_VERSION_1_2,
    };

    TRACE("adapter %p, minimum_feature_level %#x, iid %s, device %p.\n",
            adapter, minimum_feature_level, debugstr_guid(iid), device);

    load_d3d12_core();

    if ((backend = get_d3d12_metal_backend()))
    {
        hr = backend->create_device(adapter, minimum_feature_level, iid,
                device ? device : (void **)&probe_device);
        if (probe_device)
            IUnknown_Release(probe_device);
        if (!device && SUCCEEDED(hr))
            hr = S_FALSE;
        TRACE("D3DMetal device creation returned %#lx.\n", hr);
        return hr;
    }

    if (!(pfn_vkGetInstanceProcAddr = load_vulkan()))
    {
        ERR_(winediag)("Failed to load Vulkan library.\n");
        return E_FAIL;
    }

    if (FAILED(hr = d3d12_get_adapter(&wine_adapter, adapter)))
        return hr;

    if (FAILED(hr = IWineDXGIAdapter_get_adapter_info(wine_adapter, &adapter_info)))
    {
        WARN("Failed to get adapter info, hr %#lx.\n", hr);
        goto done;
    }

    optional_extensions_info.type = VKD3D_STRUCTURE_TYPE_OPTIONAL_INSTANCE_EXTENSIONS_INFO;
    optional_extensions_info.next = &application_info;
    optional_extensions_info.extensions = optional_instance_extensions;
    optional_extensions_info.extension_count = ARRAY_SIZE(optional_instance_extensions);

    instance_create_info.type = VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.next = &optional_extensions_info;
    instance_create_info.pfn_signal_event = d3d12_signal_event;
    instance_create_info.pfn_create_thread = NULL;
    instance_create_info.pfn_join_thread = NULL;
    instance_create_info.wchar_size = sizeof(WCHAR);
    instance_create_info.pfn_vkGetInstanceProcAddr = pfn_vkGetInstanceProcAddr;
    instance_create_info.instance_extensions = instance_extensions;
    instance_create_info.instance_extension_count = ARRAY_SIZE(instance_extensions);

    if (FAILED(hr = vkd3d_create_instance(&instance_create_info, &instance)))
    {
        WARN("Failed to create vkd3d instance, hr %#lx.\n", hr);
        goto done;
    }

    device_create_info.type = VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.next = NULL;
    device_create_info.minimum_feature_level = minimum_feature_level;
    device_create_info.instance = instance;
    device_create_info.instance_create_info = NULL;
    device_create_info.vk_physical_device = d3d12_get_vk_physical_device(instance, pfn_vkGetInstanceProcAddr, &adapter_info);
    device_create_info.device_extensions = device_extensions;
    device_create_info.device_extension_count = ARRAY_SIZE(device_extensions);
    device_create_info.parent = (IUnknown *)wine_adapter;
    device_create_info.adapter_luid = adapter_info.luid;

    hr = vkd3d_create_device(&device_create_info, iid, device);

    vkd3d_instance_decref(instance);

done:
    IWineDXGIAdapter_Release(wine_adapter);
    return hr;
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T data_size,
        REFIID iid, void **deserializer)
{
    const struct d3d12_metal_backend *backend;

    TRACE("data %p, data_size %Iu, iid %s, deserializer %p.\n",
            data, data_size, debugstr_guid(iid), deserializer);

    if ((backend = get_d3d12_metal_backend()) && backend->create_root_signature_deserializer)
        return backend->create_root_signature_deserializer(data, data_size, iid, deserializer);

    return vkd3d_create_root_signature_deserializer(data, data_size, iid, deserializer);
}

HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T data_size,
        REFIID iid, void **deserializer)
{
    const struct d3d12_metal_backend *backend;

    TRACE("data %p, data_size %Iu, iid %s, deserializer %p.\n",
            data, data_size, debugstr_guid(iid), deserializer);

    if ((backend = get_d3d12_metal_backend())
            && backend->create_versioned_root_signature_deserializer)
        return backend->create_versioned_root_signature_deserializer(data, data_size,
                iid, deserializer);

    return vkd3d_create_versioned_root_signature_deserializer(data, data_size, iid, deserializer);
}

HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *root_signature_desc,
        D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error_blob)
{
    const struct d3d12_metal_backend *backend;

    TRACE("root_signature_desc %p, version %#x, blob %p, error_blob %p.\n",
            root_signature_desc, version, blob, error_blob);

    if ((backend = get_d3d12_metal_backend()) && backend->serialize_root_signature)
        return backend->serialize_root_signature(root_signature_desc, version, blob, error_blob);

    return vkd3d_serialize_root_signature(root_signature_desc, version, blob, error_blob);
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
        ID3DBlob **blob, ID3DBlob **error_blob)
{
    const struct d3d12_metal_backend *backend;

    TRACE("desc %p, blob %p, error_blob %p.\n", desc, blob, error_blob);

    if ((backend = get_d3d12_metal_backend()) && backend->serialize_versioned_root_signature)
        return backend->serialize_versioned_root_signature(desc, blob, error_blob);

    return vkd3d_serialize_versioned_root_signature(desc, blob, error_blob);
}

struct d3d12_device_factory
{
    ID3D12DeviceFactory ID3D12DeviceFactory_iface;
    LONG refcount;
    D3D12_DEVICE_FACTORY_FLAGS flags;
};

static inline struct d3d12_device_factory *impl_from_ID3D12DeviceFactory(
        ID3D12DeviceFactory *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_device_factory, ID3D12DeviceFactory_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_device_factory_QueryInterface(
        ID3D12DeviceFactory *iface, REFIID iid, void **object)
{
    TRACE("iface %p, iid %s, object %p.\n", iface, debugstr_guid(iid), object);

    if (!object)
        return E_POINTER;
    *object = NULL;

    if (IsEqualGUID(iid, &IID_ID3D12DeviceFactory) || IsEqualGUID(iid, &IID_IUnknown))
    {
        *object = iface;
        ID3D12DeviceFactory_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_device_factory_AddRef(ID3D12DeviceFactory *iface)
{
    struct d3d12_device_factory *factory = impl_from_ID3D12DeviceFactory(iface);
    return InterlockedIncrement(&factory->refcount);
}

static ULONG STDMETHODCALLTYPE d3d12_device_factory_Release(ID3D12DeviceFactory *iface)
{
    struct d3d12_device_factory *factory = impl_from_ID3D12DeviceFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    if (!refcount)
        free(factory);
    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_device_factory_InitializeFromGlobalState(
        ID3D12DeviceFactory *iface)
{
    TRACE("iface %p.\n", iface);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_device_factory_ApplyToGlobalState(
        ID3D12DeviceFactory *iface)
{
    TRACE("iface %p.\n", iface);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_device_factory_SetFlags(
        ID3D12DeviceFactory *iface, D3D12_DEVICE_FACTORY_FLAGS flags)
{
    struct d3d12_device_factory *factory = impl_from_ID3D12DeviceFactory(iface);

    TRACE("iface %p, flags %#x.\n", iface, flags);
    factory->flags = flags;
    return S_OK;
}

static D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE d3d12_device_factory_GetFlags(
        ID3D12DeviceFactory *iface)
{
    struct d3d12_device_factory *factory = impl_from_ID3D12DeviceFactory(iface);

    TRACE("iface %p.\n", iface);
    return factory->flags;
}

static HRESULT STDMETHODCALLTYPE d3d12_device_factory_GetConfigurationInterface(
        ID3D12DeviceFactory *iface, REFCLSID clsid, REFIID iid, void **object)
{
    TRACE("iface %p, clsid %s, iid %s, object %p.\n",
            iface, debugstr_guid(clsid), debugstr_guid(iid), object);
    return D3D12GetInterface(clsid, iid, object);
}

static HRESULT STDMETHODCALLTYPE d3d12_device_factory_EnableExperimentalFeatures(
        ID3D12DeviceFactory *iface, UINT feature_count, const IID *iids,
        void *configurations, UINT *configuration_sizes)
{
    TRACE("iface %p, feature_count %u, iids %p, configurations %p, configuration_sizes %p.\n",
            iface, feature_count, iids, configurations, configuration_sizes);
    return D3D12EnableExperimentalFeatures(feature_count, iids, configurations,
            configuration_sizes);
}

static HRESULT STDMETHODCALLTYPE d3d12_device_factory_CreateDevice(
        ID3D12DeviceFactory *iface, IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level,
        REFIID iid, void **device)
{
    TRACE("iface %p, adapter %p, minimum_feature_level %#x, iid %s, device %p.\n",
            iface, adapter, minimum_feature_level, debugstr_guid(iid), device);
    return D3D12CreateDevice(adapter, minimum_feature_level, iid, device);
}

static const ID3D12DeviceFactoryVtbl d3d12_device_factory_vtbl =
{
    d3d12_device_factory_QueryInterface,
    d3d12_device_factory_AddRef,
    d3d12_device_factory_Release,
    d3d12_device_factory_InitializeFromGlobalState,
    d3d12_device_factory_ApplyToGlobalState,
    d3d12_device_factory_SetFlags,
    d3d12_device_factory_GetFlags,
    d3d12_device_factory_GetConfigurationInterface,
    d3d12_device_factory_EnableExperimentalFeatures,
    d3d12_device_factory_CreateDevice,
};

static HRESULT create_d3d12_device_factory(REFIID iid, void **object)
{
    struct d3d12_device_factory *factory;
    HRESULT hr;

    if (!object)
        return E_POINTER;
    *object = NULL;

    if (!(factory = calloc(1, sizeof(*factory))))
        return E_OUTOFMEMORY;
    factory->ID3D12DeviceFactory_iface.lpVtbl = &d3d12_device_factory_vtbl;
    factory->refcount = 1;

    hr = ID3D12DeviceFactory_QueryInterface(&factory->ID3D12DeviceFactory_iface, iid, object);
    ID3D12DeviceFactory_Release(&factory->ID3D12DeviceFactory_iface);
    return hr;
}

struct d3d12_sdk_configuration
{
    ID3D12SDKConfiguration1 ID3D12SDKConfiguration1_iface;
    LONG refcount;
};

static inline struct d3d12_sdk_configuration *impl_from_ID3D12SDKConfiguration1(
        ID3D12SDKConfiguration1 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_sdk_configuration,
            ID3D12SDKConfiguration1_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_sdk_configuration_QueryInterface(
        ID3D12SDKConfiguration1 *iface, REFIID iid, void **object)
{
    TRACE("iface %p, iid %s, object %p.\n", iface, debugstr_guid(iid), object);

    if (!object)
        return E_POINTER;
    *object = NULL;

    if (IsEqualGUID(iid, &IID_ID3D12SDKConfiguration)
            || IsEqualGUID(iid, &IID_ID3D12SDKConfiguration1)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        *object = iface;
        ID3D12SDKConfiguration1_AddRef(iface);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_sdk_configuration_AddRef(
        ID3D12SDKConfiguration1 *iface)
{
    struct d3d12_sdk_configuration *configuration =
            impl_from_ID3D12SDKConfiguration1(iface);
    return InterlockedIncrement(&configuration->refcount);
}

static ULONG STDMETHODCALLTYPE d3d12_sdk_configuration_Release(
        ID3D12SDKConfiguration1 *iface)
{
    struct d3d12_sdk_configuration *configuration =
            impl_from_ID3D12SDKConfiguration1(iface);
    ULONG refcount = InterlockedDecrement(&configuration->refcount);

    if (!refcount)
        free(configuration);
    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_sdk_configuration_SetSDKVersion(
        ID3D12SDKConfiguration1 *iface, UINT version, const char *path)
{
    TRACE("iface %p, version %u, path %s.\n", iface, version, debugstr_a(path));
    set_d3d12_core_sdk_version(version);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_sdk_configuration_CreateDeviceFactory(
        ID3D12SDKConfiguration1 *iface, UINT version, const char *path,
        REFIID iid, void **factory)
{
    TRACE("iface %p, version %u, path %s, iid %s, factory %p.\n",
            iface, version, debugstr_a(path), debugstr_guid(iid), factory);

    set_d3d12_core_sdk_version(version);
    return create_d3d12_device_factory(iid, factory);
}

static void STDMETHODCALLTYPE d3d12_sdk_configuration_FreeUnusedSDKs(
        ID3D12SDKConfiguration1 *iface)
{
    TRACE("iface %p.\n", iface);
}

static const ID3D12SDKConfiguration1Vtbl d3d12_sdk_configuration_vtbl =
{
    d3d12_sdk_configuration_QueryInterface,
    d3d12_sdk_configuration_AddRef,
    d3d12_sdk_configuration_Release,
    d3d12_sdk_configuration_SetSDKVersion,
    d3d12_sdk_configuration_CreateDeviceFactory,
    d3d12_sdk_configuration_FreeUnusedSDKs,
};

static HRESULT create_d3d12_sdk_configuration(REFIID iid, void **object)
{
    struct d3d12_sdk_configuration *configuration;
    HRESULT hr;

    if (!object)
        return E_POINTER;
    *object = NULL;

    if (!(configuration = calloc(1, sizeof(*configuration))))
        return E_OUTOFMEMORY;
    configuration->ID3D12SDKConfiguration1_iface.lpVtbl = &d3d12_sdk_configuration_vtbl;
    configuration->refcount = 1;

    hr = ID3D12SDKConfiguration1_QueryInterface(
            &configuration->ID3D12SDKConfiguration1_iface, iid, object);
    ID3D12SDKConfiguration1_Release(&configuration->ID3D12SDKConfiguration1_iface);
    return hr;
}

HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID iid, void **object)
{
    const struct d3d12_metal_backend *backend;

    TRACE("clsid %s, iid %s, object %p.\n",
            debugstr_guid(clsid), debugstr_guid(iid), object);

    if (IsEqualGUID(clsid, &CLSID_D3D12SDKConfiguration))
        return create_d3d12_sdk_configuration(iid, object);
    if (IsEqualGUID(clsid, &clsid_d3d12_device_factory))
        return create_d3d12_device_factory(iid, object);

    if ((backend = get_d3d12_metal_backend()) && backend->get_interface)
        return backend->get_interface(clsid, iid, object);

    if (object)
        *object = NULL;
    return object ? E_NOINTERFACE : E_POINTER;
}
