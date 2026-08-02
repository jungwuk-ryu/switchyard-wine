/*
 * Copyright (C) 2023 Mohamad Al-Jaf
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

#define COBJMACROS
#include "windef.h"
#include "initguid.h"
#include "d3d11.h"
#include "d3d12.h"
#include "dxgi1_6.h"
#include "dxcore.h"
#include "wine/wined3d.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxcore);

struct dxcore_adapter
{
    IDXCoreAdapter IDXCoreAdapter_iface;
    LONG refcount;
    LONG valid;

    struct dxcore_adapter *cache_next;
    IDXCoreAdapterFactory *factory;
    struct wined3d_adapter_identifier identifier;
    char driver_description[128];
};

struct dxcore_capability_cache
{
    struct dxcore_capability_cache *next;
    LUID luid;
    LARGE_INTEGER driver_version;
    GUID driver_uuid;
    GUID device_uuid;
    LONG d3d11_supported;
    LONG d3d12_supported;
};

static struct dxcore_capability_cache *capability_cache;
static SRWLOCK capability_cache_lock = SRWLOCK_INIT;
static struct dxcore_adapter *adapter_cache;
static SRWLOCK adapter_cache_lock = SRWLOCK_INIT;
static struct wined3d *cached_wined3d;
static ULONGLONG cached_wined3d_tick;
static SRWLOCK cached_wined3d_lock = SRWLOCK_INIT;
static SRWLOCK factory_refcount_lock = SRWLOCK_INIT;

#define DXCORE_WINED3D_CACHE_LIFETIME_MS 250

static struct wined3d *dxcore_wined3d_acquire(BOOL refresh)
{
    struct wined3d *wined3d, *old_wined3d = NULL;
    ULONGLONG now = GetTickCount64();

    AcquireSRWLockShared(&cached_wined3d_lock);
    if (cached_wined3d && (!refresh
            || now - cached_wined3d_tick < DXCORE_WINED3D_CACHE_LIFETIME_MS))
    {
        wined3d_incref(wined3d = cached_wined3d);
        ReleaseSRWLockShared(&cached_wined3d_lock);
        return wined3d;
    }
    ReleaseSRWLockShared(&cached_wined3d_lock);

    /* Serialize backend creation. Adapter validity and staleness checks may be
     * called concurrently by several clients, and constructing one wined3d
     * instance per caller creates a large, avoidable driver-enumeration spike. */
    AcquireSRWLockExclusive(&cached_wined3d_lock);
    now = GetTickCount64();
    if (cached_wined3d && (!refresh
            || now - cached_wined3d_tick < DXCORE_WINED3D_CACHE_LIFETIME_MS))
    {
        wined3d_incref(wined3d = cached_wined3d);
        ReleaseSRWLockExclusive(&cached_wined3d_lock);
        return wined3d;
    }

    if (!(wined3d = wined3d_create(0)))
    {
        if (cached_wined3d)
        {
            cached_wined3d_tick = now;
            wined3d_incref(wined3d = cached_wined3d);
        }
        ReleaseSRWLockExclusive(&cached_wined3d_lock);
        return wined3d;
    }

    old_wined3d = cached_wined3d;
    cached_wined3d = wined3d;
    cached_wined3d_tick = now;
    wined3d_incref(wined3d);
    ReleaseSRWLockExclusive(&cached_wined3d_lock);

    if (old_wined3d)
        wined3d_decref(old_wined3d);
    return wined3d;
}

static void dxcore_wined3d_cache_cleanup(void)
{
    struct wined3d *wined3d;

    AcquireSRWLockExclusive(&cached_wined3d_lock);
    wined3d = cached_wined3d;
    cached_wined3d = NULL;
    cached_wined3d_tick = 0;
    ReleaseSRWLockExclusive(&cached_wined3d_lock);

    if (wined3d)
        wined3d_decref(wined3d);
}

static inline struct dxcore_adapter *impl_from_IDXCoreAdapter(IDXCoreAdapter *iface)
{
    return CONTAINING_RECORD(iface, struct dxcore_adapter, IDXCoreAdapter_iface);
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_QueryInterface(IDXCoreAdapter *iface, REFIID riid, void **out)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);

    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;
    *out = NULL;

    if (IsEqualGUID(riid, &IID_IDXCoreAdapter)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        *out = &adapter->IDXCoreAdapter_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dxcore_adapter_AddRef(IDXCoreAdapter *iface)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);
    ULONG refcount = InterlockedIncrement(&adapter->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE dxcore_adapter_Release(IDXCoreAdapter *iface)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);
    struct dxcore_adapter **entry;
    IDXCoreAdapterFactory *factory;
    ULONG refcount;

    AcquireSRWLockExclusive(&adapter_cache_lock);
    refcount = InterlockedDecrement(&adapter->refcount);

    if (!refcount)
    {
        for (entry = &adapter_cache; *entry; entry = &(*entry)->cache_next)
        {
            if (*entry == adapter)
            {
                *entry = adapter->cache_next;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&adapter_cache_lock);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        factory = adapter->factory;
        free(adapter);
        IDXCoreAdapterFactory_Release(factory);
    }

    return refcount;
}

static BOOL dxcore_capability_cache_matches_adapter(const struct dxcore_capability_cache *entry,
        const struct dxcore_adapter *adapter)
{
    return !memcmp(&entry->luid, &adapter->identifier.adapter_luid, sizeof(LUID))
            && entry->driver_version.QuadPart == adapter->identifier.driver_version.QuadPart
            && IsEqualGUID(&entry->driver_uuid, &adapter->identifier.driver_uuid)
            && IsEqualGUID(&entry->device_uuid, &adapter->identifier.device_uuid);
}

static LONG dxcore_get_cached_capability(const struct dxcore_adapter *adapter, BOOL d3d12)
{
    struct dxcore_capability_cache *entry;
    LONG supported = 0;

    AcquireSRWLockShared(&capability_cache_lock);
    for (entry = capability_cache; entry; entry = entry->next)
    {
        if (dxcore_capability_cache_matches_adapter(entry, adapter))
        {
            supported = d3d12 ? entry->d3d12_supported : entry->d3d11_supported;
            break;
        }
    }
    ReleaseSRWLockShared(&capability_cache_lock);
    return supported;
}

static void dxcore_cache_capability(const struct dxcore_adapter *adapter, BOOL d3d12, LONG supported)
{
    struct dxcore_capability_cache *entry;

    AcquireSRWLockExclusive(&capability_cache_lock);
    for (entry = capability_cache; entry; entry = entry->next)
        if (dxcore_capability_cache_matches_adapter(entry, adapter))
            break;

    if (!entry && (entry = calloc(1, sizeof(*entry))))
    {
        entry->luid = adapter->identifier.adapter_luid;
        entry->driver_version = adapter->identifier.driver_version;
        entry->driver_uuid = adapter->identifier.driver_uuid;
        entry->device_uuid = adapter->identifier.device_uuid;
        entry->next = capability_cache;
        capability_cache = entry;
    }
    if (entry)
    {
        if (d3d12)
            entry->d3d12_supported = supported;
        else
            entry->d3d11_supported = supported;
    }
    ReleaseSRWLockExclusive(&capability_cache_lock);
}

static void dxcore_capability_cache_cleanup(void)
{
    struct dxcore_capability_cache *entry, *next;

    AcquireSRWLockExclusive(&capability_cache_lock);
    entry = capability_cache;
    capability_cache = NULL;
    ReleaseSRWLockExclusive(&capability_cache_lock);

    while (entry)
    {
        next = entry->next;
        free(entry);
        entry = next;
    }
}

static BOOL dxcore_adapter_identifier_equal(const struct dxcore_adapter *adapter,
        const struct wined3d_adapter_identifier *identifier, const char *description)
{
    const struct wined3d_adapter_identifier *cached = &adapter->identifier;

    return !memcmp(&cached->adapter_luid, &identifier->adapter_luid, sizeof(LUID))
            && cached->driver_version.QuadPart == identifier->driver_version.QuadPart
            && cached->vendor_id == identifier->vendor_id
            && cached->device_id == identifier->device_id
            && cached->subsystem_id == identifier->subsystem_id
            && cached->revision == identifier->revision
            && IsEqualGUID(&cached->device_identifier, &identifier->device_identifier)
            && IsEqualGUID(&cached->driver_uuid, &identifier->driver_uuid)
            && IsEqualGUID(&cached->device_uuid, &identifier->device_uuid)
            && cached->whql_level == identifier->whql_level
            && cached->video_memory == identifier->video_memory
            && cached->shared_system_memory == identifier->shared_system_memory
            && cached->is_software == identifier->is_software
            && !strcmp(adapter->driver_description, description);
}

static HRESULT dxcore_adapter_is_current(struct dxcore_adapter *adapter, BOOL *is_current)
{
    struct wined3d_adapter_identifier identifier;
    char driver_description[128];
    struct wined3d *wined3d;
    uint32_t count, i;
    HRESULT hr;

    *is_current = FALSE;

    if (!(wined3d = dxcore_wined3d_acquire(TRUE)))
        return E_FAIL;

    count = wined3d_get_adapter_count(wined3d);
    for (i = 0; i < count; ++i)
    {
        memset(&identifier, 0, sizeof(identifier));
        driver_description[0] = 0;
        identifier.description = driver_description;
        identifier.description_size = sizeof(driver_description);
        if (FAILED(hr = wined3d_adapter_get_identifier(wined3d_get_adapter(wined3d, i), 0, &identifier)))
        {
            wined3d_decref(wined3d);
            return hr;
        }
        driver_description[ARRAY_SIZE(driver_description) - 1] = 0;

        if (!memcmp(&adapter->identifier.adapter_luid, &identifier.adapter_luid, sizeof(LUID)))
        {
            *is_current = dxcore_adapter_identifier_equal(adapter, &identifier, driver_description);
            break;
        }
    }

    wined3d_decref(wined3d);
    return S_OK;
}

static BOOL STDMETHODCALLTYPE dxcore_adapter_IsValid(IDXCoreAdapter *iface)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);
    BOOL is_current;
    HRESULT hr;

    TRACE("iface %p.\n", iface);

    if (!InterlockedCompareExchange(&adapter->valid, 0, 0))
        return FALSE;

    if (FAILED(hr = dxcore_adapter_is_current(adapter, &is_current)))
    {
        WARN("Failed to enumerate adapters, hr %#lx.\n", hr);
        return TRUE;
    }

    if (!is_current)
        InterlockedExchange(&adapter->valid, FALSE);

    return !!InterlockedCompareExchange(&adapter->valid, 0, 0);
}

static BOOL dxcore_adapter_supports_d3d11(struct dxcore_adapter *adapter)
{
    HRESULT (WINAPI *create_factory)(UINT, REFIID, void **);
    PFN_D3D11_CREATE_DEVICE create_device;
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter *dxgi_adapter = NULL;
    HMODULE d3d11_module, dxgi_module;
    LONG cached, supported = 1;

    if ((cached = dxcore_get_cached_capability(adapter, FALSE)))
        return cached == 2;

    d3d11_module = LoadLibraryW(L"d3d11.dll");
    dxgi_module = LoadLibraryW(L"dxgi.dll");
    if (d3d11_module && dxgi_module &&
        (create_device = (void *)GetProcAddress(d3d11_module, "D3D11CreateDevice")) &&
        (create_factory = (void *)GetProcAddress(dxgi_module, "CreateDXGIFactory2")) &&
        SUCCEEDED(create_factory(0, &IID_IDXGIFactory4, (void **)&factory)) &&
        SUCCEEDED(IDXGIFactory4_EnumAdapterByLuid(factory, adapter->identifier.adapter_luid,
                &IID_IDXGIAdapter, (void **)&dxgi_adapter)) &&
        SUCCEEDED(create_device(dxgi_adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                D3D11_SDK_VERSION, NULL, NULL, NULL)))
    {
        supported = 2;
    }
    if (dxgi_adapter) IDXGIAdapter_Release(dxgi_adapter);
    if (factory) IDXGIFactory4_Release(factory);
    if (dxgi_module) FreeLibrary(dxgi_module);
    if (d3d11_module) FreeLibrary(d3d11_module);

    dxcore_cache_capability(adapter, FALSE, supported);
    return supported == 2;
}

static BOOL dxcore_adapter_supports_d3d12(struct dxcore_adapter *adapter)
{
    HRESULT (WINAPI *create_factory)(UINT, REFIID, void **);
    PFN_D3D12_CREATE_DEVICE create_device;
    IDXGIFactory4 *factory = NULL;
    IDXGIAdapter *dxgi_adapter = NULL;
    HMODULE d3d12_module, dxgi_module;
    LONG cached, supported = 1;

    if ((cached = dxcore_get_cached_capability(adapter, TRUE)))
        return cached == 2;

    d3d12_module = LoadLibraryW(L"d3d12.dll");
    dxgi_module = LoadLibraryW(L"dxgi.dll");
    if (d3d12_module && dxgi_module &&
        (create_device = (void *)GetProcAddress(d3d12_module, "D3D12CreateDevice")) &&
        (create_factory = (void *)GetProcAddress(dxgi_module, "CreateDXGIFactory2")) &&
        SUCCEEDED(create_factory(0, &IID_IDXGIFactory4, (void **)&factory)) &&
        SUCCEEDED(IDXGIFactory4_EnumAdapterByLuid(factory, adapter->identifier.adapter_luid,
                &IID_IDXGIAdapter, (void **)&dxgi_adapter)) &&
        SUCCEEDED(create_device((IUnknown *)dxgi_adapter, D3D_FEATURE_LEVEL_11_0,
                &IID_ID3D12Device, NULL)))
    {
        supported = 2;
    }
    if (dxgi_adapter) IDXGIAdapter_Release(dxgi_adapter);
    if (factory) IDXGIFactory4_Release(factory);
    if (dxgi_module) FreeLibrary(dxgi_module);
    if (d3d12_module) FreeLibrary(d3d12_module);

    dxcore_cache_capability(adapter, TRUE, supported);
    return supported == 2;
}

static BOOL dxcore_adapter_supports_attribute(struct dxcore_adapter *adapter, REFGUID attribute)
{
    if (IsEqualGUID(attribute, &DXCORE_ADAPTER_ATTRIBUTE_D3D11_GRAPHICS))
        return dxcore_adapter_supports_d3d11(adapter);
    if (IsEqualGUID(attribute, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS) ||
        IsEqualGUID(attribute, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE))
    {
        /* Both attributes describe device creation through D3D12CreateDevice;
         * Wine does not expose a separate core-compute driver path. */
        return dxcore_adapter_supports_d3d12(adapter);
    }
    return FALSE;
}

static BOOL STDMETHODCALLTYPE dxcore_adapter_IsAttributeSupported(IDXCoreAdapter *iface, REFGUID attribute)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);

    TRACE("iface %p, attribute %s.\n", iface, debugstr_guid(attribute));

    return dxcore_adapter_supports_attribute(adapter, attribute);
}

static BOOL dxcore_adapter_property_supported(DXCoreAdapterProperty property)
{
    switch (property)
    {
        case InstanceLuid:
        case DriverVersion:
        case DriverDescription:
        case HardwareID:
        case DedicatedAdapterMemory:
        case DedicatedSystemMemory:
        case SharedSystemMemory:
        case IsHardware:
            return TRUE;

        default:
            return FALSE;
    }
}

static BOOL STDMETHODCALLTYPE dxcore_adapter_IsPropertySupported(IDXCoreAdapter *iface, DXCoreAdapterProperty property)
{
    TRACE("iface %p, property %u.\n", iface, property);

    return dxcore_adapter_property_supported(property);
}

static HRESULT dxcore_adapter_get_property_size(struct dxcore_adapter *adapter,
        DXCoreAdapterProperty property, size_t *size)
{
    static const size_t property_sizes[] =
    {
        [InstanceLuid] = sizeof(LUID),
        [DriverVersion] = sizeof(LARGE_INTEGER),
        [HardwareID] = sizeof(DXCoreHardwareID),
        [DedicatedAdapterMemory] = sizeof(UINT64),
        [DedicatedSystemMemory] = sizeof(UINT64),
        [SharedSystemMemory] = sizeof(UINT64),
        [IsHardware] = sizeof(BYTE),
    };

    if (!dxcore_adapter_property_supported(property))
    {
        FIXME("Property %u not implemented.\n", property);
        return DXGI_ERROR_INVALID_CALL;
    }

    if (property == DriverDescription)
        *size = strlen(adapter->driver_description) + 1;
    else
        *size = property_sizes[property];

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_GetProperty(IDXCoreAdapter *iface, DXCoreAdapterProperty property,
        size_t buffer_size, void *buffer)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);
    DXCoreHardwareID hardware_id;
    LARGE_INTEGER driver_version;
    UINT64 uint64_value;
    LUID instance_luid;
    BYTE byte_value;
    size_t size;
    HRESULT hr;

    TRACE("iface %p, property %u, buffer_size %Iu, buffer %p\n", iface, property, buffer_size, buffer);

    if (!buffer)
        return E_POINTER;

    if (FAILED(hr = dxcore_adapter_get_property_size(adapter, property, &size)))
        return hr;

    if (property == DriverDescription && buffer && buffer_size)
        *(char *)buffer = 0;

    if (buffer_size < size)
        return E_INVALIDARG;

    switch (property)
    {
        case InstanceLuid:
            instance_luid = adapter->identifier.adapter_luid;
            memcpy(buffer, &instance_luid, sizeof(instance_luid));
            break;

        case DriverVersion:
            driver_version = adapter->identifier.driver_version;
            memcpy(buffer, &driver_version, sizeof(driver_version));
            break;

        case HardwareID:
            hardware_id.vendorID = adapter->identifier.vendor_id;
            hardware_id.deviceID = adapter->identifier.device_id;
            hardware_id.subSysID = adapter->identifier.subsystem_id;
            hardware_id.revision = adapter->identifier.revision;
            memcpy(buffer, &hardware_id, sizeof(hardware_id));
            break;

        case DedicatedAdapterMemory:
            uint64_value = adapter->identifier.video_memory;
            memcpy(buffer, &uint64_value, sizeof(uint64_value));
            break;

        case DedicatedSystemMemory:
            FIXME("Returning 0 for DedicatedSystemMemory.\n");
            uint64_value = 0;
            memcpy(buffer, &uint64_value, sizeof(uint64_value));
            break;

        case SharedSystemMemory:
            uint64_value = adapter->identifier.shared_system_memory;
            memcpy(buffer, &uint64_value, sizeof(uint64_value));
            break;

        case IsHardware:
            byte_value = !adapter->identifier.is_software;
            memcpy(buffer, &byte_value, sizeof(byte_value));
            break;

        case DriverDescription:
        {
            memcpy(buffer, adapter->identifier.description, size);
            break;
        }

        default:
            break;
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_GetPropertySize(IDXCoreAdapter *iface, DXCoreAdapterProperty property,
        size_t *buffer_size)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);

    TRACE("iface %p, property %u, buffer_size %p.\n", iface, property, buffer_size);

    if (!buffer_size)
        return E_POINTER;

    return dxcore_adapter_get_property_size(adapter, property, buffer_size);
}

static BOOL STDMETHODCALLTYPE dxcore_adapter_IsQueryStateSupported(IDXCoreAdapter *iface, DXCoreAdapterState property)
{
    TRACE("iface %p, property %u.\n", iface, property);

    return property == IsDriverUpdateInProgress || property == AdapterMemoryBudget;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_QueryState(IDXCoreAdapter *iface, DXCoreAdapterState state, size_t state_details_size,
        const void *state_details, size_t buffer_size, void *buffer)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);
    DXCoreAdapterMemoryBudgetNodeSegmentGroup details;
    DXCoreAdapterMemoryBudget memory_budget;
    struct wined3d_video_memory_info memory_info;
    struct wined3d_adapter_identifier identifier;
    char driver_description[128];
    struct wined3d *wined3d;
    uint32_t count, i;
    HRESULT hr;

    TRACE("iface %p, state %u, state_details_size %Iu, state_details %p, buffer_size %Iu, buffer %p.\n",
            iface, state, state_details_size, state_details, buffer_size, buffer);

    if (!buffer)
        return E_POINTER;

    if (!InterlockedCompareExchange(&adapter->valid, 0, 0))
        return DXGI_ERROR_DEVICE_REMOVED;
    if (!IDXCoreAdapter_IsValid(iface))
        return DXGI_ERROR_DEVICE_REMOVED;

    switch (state)
    {
        case IsDriverUpdateInProgress:
            if (state_details_size || buffer_size < sizeof(BYTE))
                return E_INVALIDARG;
            *(BYTE *)buffer = FALSE;
            return S_OK;

        case AdapterMemoryBudget:
            if (!state_details)
                return E_POINTER;
            if (state_details_size < sizeof(details) || buffer_size < sizeof(memory_budget))
                return E_INVALIDARG;

            memcpy(&details, state_details, sizeof(details));
            if (details.segmentGroup != Local && details.segmentGroup != NonLocal)
                return E_INVALIDARG;
            if (details.nodeIndex)
                return E_INVALIDARG;

            if (!(wined3d = dxcore_wined3d_acquire(FALSE)))
                return E_FAIL;

            hr = DXGI_ERROR_DEVICE_REMOVED;
            count = wined3d_get_adapter_count(wined3d);
            for (i = 0; i < count; ++i)
            {
                memset(&identifier, 0, sizeof(identifier));
                driver_description[0] = 0;
                identifier.description = driver_description;
                identifier.description_size = sizeof(driver_description);
                if (FAILED(hr = wined3d_adapter_get_identifier(
                        wined3d_get_adapter(wined3d, i), 0, &identifier)))
                    break;
                driver_description[ARRAY_SIZE(driver_description) - 1] = 0;
                if (memcmp(&adapter->identifier.adapter_luid,
                        &identifier.adapter_luid, sizeof(LUID)))
                    continue;
                if (!dxcore_adapter_identifier_equal(adapter, &identifier, driver_description))
                {
                    hr = DXGI_ERROR_DEVICE_REMOVED;
                    break;
                }

                hr = wined3d_adapter_get_video_memory_info(wined3d_get_adapter(wined3d, i),
                        details.nodeIndex, (enum wined3d_memory_segment_group)details.segmentGroup,
                        &memory_info);
                if (SUCCEEDED(hr))
                {
                    memory_budget.budget = memory_info.budget;
                    memory_budget.currentUsage = memory_info.current_usage;
                    memory_budget.availableForReservation = memory_info.available_reservation;
                    memory_budget.currentReservation = memory_info.current_reservation;
                    memcpy(buffer, &memory_budget, sizeof(memory_budget));
                }
                break;
            }

            wined3d_decref(wined3d);
            if (hr == DXGI_ERROR_DEVICE_REMOVED)
                InterlockedExchange(&adapter->valid, FALSE);
            return hr;

        default:
            return DXGI_ERROR_INVALID_CALL;
    }
}

static BOOL STDMETHODCALLTYPE dxcore_adapter_IsSetStateSupported(IDXCoreAdapter *iface, DXCoreAdapterState property)
{
    TRACE("iface %p, property %u.\n", iface, property);

    return FALSE;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_SetState(IDXCoreAdapter *iface, DXCoreAdapterState state, size_t state_details_size,
        const void *state_details, size_t buffer_size, const void *buffer)
{
    TRACE("iface %p, state %u, state_details_size %Iu, state_details %p, buffer_size %Iu, buffer %p.\n",
            iface, state, state_details_size, state_details, buffer_size, buffer);

    if (state != IsDriverUpdateInProgress && state != AdapterMemoryBudget)
        return DXGI_ERROR_INVALID_CALL;
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_GetFactory(IDXCoreAdapter *iface, REFIID riid, void **out)
{
    struct dxcore_adapter *adapter = impl_from_IDXCoreAdapter(iface);

    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;

    *out = NULL;
    return IDXCoreAdapterFactory_QueryInterface(adapter->factory, riid, out);
}

static const struct IDXCoreAdapterVtbl dxcore_adapter_vtbl =
{
    /* IUnknown methods */
    dxcore_adapter_QueryInterface,
    dxcore_adapter_AddRef,
    dxcore_adapter_Release,
    /* IDXCoreAdapter methods */
    dxcore_adapter_IsValid,
    dxcore_adapter_IsAttributeSupported,
    dxcore_adapter_IsPropertySupported,
    dxcore_adapter_GetProperty,
    dxcore_adapter_GetPropertySize,
    dxcore_adapter_IsQueryStateSupported,
    dxcore_adapter_QueryState,
    dxcore_adapter_IsSetStateSupported,
    dxcore_adapter_SetState,
    dxcore_adapter_GetFactory,
};

struct dxcore_adapter_list
{
    IDXCoreAdapterList IDXCoreAdapterList_iface;
    LONG refcount;
    LONG stale;
    SRWLOCK lock;

    IDXCoreAdapterFactory *factory;
    struct dxcore_adapter **adapters;
    uint32_t adapter_count;
    GUID *filter_attributes;
    uint32_t filter_attribute_count;
};

static inline struct dxcore_adapter_list *impl_from_IDXCoreAdapterList(IDXCoreAdapterList *iface)
{
    return CONTAINING_RECORD(iface, struct dxcore_adapter_list, IDXCoreAdapterList_iface);
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_list_QueryInterface(IDXCoreAdapterList *iface, REFIID riid, void **out)
{
    struct dxcore_adapter_list *list = impl_from_IDXCoreAdapterList(iface);

    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;
    *out = NULL;

    if (IsEqualGUID(riid, &IID_IDXCoreAdapterList)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        *out = &list->IDXCoreAdapterList_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dxcore_adapter_list_AddRef(IDXCoreAdapterList *iface)
{
    struct dxcore_adapter_list *list = impl_from_IDXCoreAdapterList(iface);
    ULONG refcount = InterlockedIncrement(&list->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static void dxcore_adapter_list_clear_adapters(struct dxcore_adapter_list *list)
{
    uint32_t i;

    for (i = 0; i < list->adapter_count; ++i)
    {
        if (list->adapters[i])
            IDXCoreAdapter_Release(&list->adapters[i]->IDXCoreAdapter_iface);
    }

    free(list->adapters);
    list->adapters = NULL;
    list->adapter_count = 0;
}

static ULONG STDMETHODCALLTYPE dxcore_adapter_list_Release(IDXCoreAdapterList *iface)
{
    struct dxcore_adapter_list *list = impl_from_IDXCoreAdapterList(iface);
    ULONG refcount = InterlockedDecrement(&list->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        IDXCoreAdapterFactory *factory = list->factory;

        dxcore_adapter_list_clear_adapters(list);
        free(list->filter_attributes);
        free(list);
        IDXCoreAdapterFactory_Release(factory);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_list_GetAdapter(IDXCoreAdapterList *iface, uint32_t index,
        REFIID riid, void **out)
{
    struct dxcore_adapter_list *list = impl_from_IDXCoreAdapterList(iface);
    IDXCoreAdapter *adapter;
    HRESULT hr;

    TRACE("iface %p, index %u, riid %s, out %p\n", iface, index, debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;

    *out = NULL;

    AcquireSRWLockShared(&list->lock);
    if (index >= list->adapter_count)
    {
        ReleaseSRWLockShared(&list->lock);
        return E_INVALIDARG;
    }

    adapter = &list->adapters[index]->IDXCoreAdapter_iface;
    TRACE("returning IDXCoreAdapter %p for index %u.\n", adapter, index);
    hr = IDXCoreAdapter_QueryInterface(adapter, riid, out);
    ReleaseSRWLockShared(&list->lock);
    return hr;
}

static uint32_t STDMETHODCALLTYPE dxcore_adapter_list_GetAdapterCount(IDXCoreAdapterList *iface)
{
    struct dxcore_adapter_list *list = impl_from_IDXCoreAdapterList(iface);
    uint32_t adapter_count;

    TRACE("iface %p\n", iface);

    AcquireSRWLockShared(&list->lock);
    adapter_count = list->adapter_count;
    ReleaseSRWLockShared(&list->lock);
    return adapter_count;
}

static HRESULT get_adapters(struct dxcore_adapter_list *list, uint32_t num_attributes,
        const GUID *filter_attributes, BOOL use_cache);

static BOOL dxcore_adapter_snapshot_equal(const struct dxcore_adapter *left,
        const struct dxcore_adapter *right)
{
    return dxcore_adapter_identifier_equal(left, &right->identifier, right->driver_description);
}

static BOOL STDMETHODCALLTYPE dxcore_adapter_list_IsStale(IDXCoreAdapterList *iface)
{
    struct dxcore_adapter_list *list = impl_from_IDXCoreAdapterList(iface);
    struct dxcore_adapter_list current = {0};
    uint32_t i, j;
    HRESULT hr;

    TRACE("iface %p.\n", iface);

    if (InterlockedCompareExchange(&list->stale, 0, 0))
        return TRUE;

    current.factory = list->factory;
    if (FAILED(hr = get_adapters(&current,
            list->filter_attribute_count, list->filter_attributes, FALSE)))
    {
        WARN("Failed to enumerate current adapters, hr %#lx.\n", hr);
        dxcore_adapter_list_clear_adapters(&current);
        return FALSE;
    }

    AcquireSRWLockShared(&list->lock);
    if (current.adapter_count != list->adapter_count)
    {
        InterlockedExchange(&list->stale, TRUE);
    }
    else
    {
        for (i = 0; i < list->adapter_count; ++i)
        {
            for (j = 0; j < current.adapter_count; ++j)
            {
                if (!memcmp(&list->adapters[i]->identifier.adapter_luid,
                        &current.adapters[j]->identifier.adapter_luid, sizeof(LUID)))
                    break;
            }

            if (j == current.adapter_count
                    || !dxcore_adapter_snapshot_equal(list->adapters[i], current.adapters[j]))
            {
                InterlockedExchange(&list->stale, TRUE);
                break;
            }
        }
    }
    ReleaseSRWLockShared(&list->lock);

    dxcore_adapter_list_clear_adapters(&current);
    return !!InterlockedCompareExchange(&list->stale, 0, 0);
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_list_GetFactory(IDXCoreAdapterList *iface, REFIID riid, void **out)
{
    struct dxcore_adapter_list *list = impl_from_IDXCoreAdapterList(iface);

    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;

    *out = NULL;
    return IDXCoreAdapterFactory_QueryInterface(list->factory, riid, out);
}

static int dxcore_adapter_compare(const struct dxcore_adapter *left, const struct dxcore_adapter *right,
        uint32_t num_preferences, const DXCoreAdapterPreference *preferences)
{
    uint32_t i;

    for (i = 0; i < num_preferences; ++i)
    {
        switch (preferences[i])
        {
            case Hardware:
                if (left->identifier.is_software != right->identifier.is_software)
                    return left->identifier.is_software ? 1 : -1;
                break;

            case MinimumPower:
                if (left->identifier.video_memory != right->identifier.video_memory)
                    return left->identifier.video_memory < right->identifier.video_memory ? -1 : 1;
                break;

            case HighPerformance:
                if (left->identifier.video_memory != right->identifier.video_memory)
                    return left->identifier.video_memory > right->identifier.video_memory ? -1 : 1;
                break;

            default:
                break;
        }
    }

    return 0;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_list_Sort(IDXCoreAdapterList *iface, uint32_t num_preferences,
        const DXCoreAdapterPreference *preferences)
{
    struct dxcore_adapter_list *list = impl_from_IDXCoreAdapterList(iface);
    struct dxcore_adapter *adapter;
    uint32_t i, j;

    TRACE("iface %p, num_preferences %u, preferences %p.\n", iface, num_preferences, preferences);

    if (!num_preferences || !preferences)
        return E_INVALIDARG;

    /* wined3d does not expose a power class for adapters. Dedicated memory is
     * used as a stable proxy for minimum-power and high-performance ordering. */
    AcquireSRWLockExclusive(&list->lock);
    for (i = 1; i < list->adapter_count; ++i)
    {
        adapter = list->adapters[i];
        for (j = i; j && dxcore_adapter_compare(adapter, list->adapters[j - 1],
                num_preferences, preferences) < 0; --j)
            list->adapters[j] = list->adapters[j - 1];
        list->adapters[j] = adapter;
    }
    ReleaseSRWLockExclusive(&list->lock);

    return S_OK;
}

static BOOL STDMETHODCALLTYPE dxcore_adapter_list_IsAdapterPreferenceSupported(IDXCoreAdapterList *iface,
        DXCoreAdapterPreference preference)
{
    TRACE("iface %p, preference %u.\n", iface, preference);

    return preference == Hardware || preference == MinimumPower || preference == HighPerformance;
}

static const struct IDXCoreAdapterListVtbl dxcore_adapter_list_vtbl =
{
    /* IUnknown methods */
    dxcore_adapter_list_QueryInterface,
    dxcore_adapter_list_AddRef,
    dxcore_adapter_list_Release,
    /* IDXCoreAdapterList methods */
    dxcore_adapter_list_GetAdapter,
    dxcore_adapter_list_GetAdapterCount,
    dxcore_adapter_list_IsStale,
    dxcore_adapter_list_GetFactory,
    dxcore_adapter_list_Sort,
    dxcore_adapter_list_IsAdapterPreferenceSupported,
};

struct dxcore_adapter_factory
{
    IDXCoreAdapterFactory IDXCoreAdapterFactory_iface;
    LONG refcount;
};

static inline struct dxcore_adapter_factory *impl_from_IDXCoreAdapterFactory(IDXCoreAdapterFactory *iface)
{
    return CONTAINING_RECORD(iface, struct dxcore_adapter_factory, IDXCoreAdapterFactory_iface);
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_factory_QueryInterface(IDXCoreAdapterFactory *iface, REFIID riid, void **out)
{
    struct dxcore_adapter_factory *factory = impl_from_IDXCoreAdapterFactory(iface);

    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;
    *out = NULL;

    if (IsEqualGUID(riid, &IID_IDXCoreAdapterFactory)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        *out = &factory->IDXCoreAdapterFactory_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dxcore_adapter_factory_AddRef(IDXCoreAdapterFactory *iface)
{
    struct dxcore_adapter_factory *factory = impl_from_IDXCoreAdapterFactory(iface);
    ULONG refcount;

    AcquireSRWLockExclusive(&factory_refcount_lock);
    refcount = InterlockedIncrement(&factory->refcount);
    ReleaseSRWLockExclusive(&factory_refcount_lock);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE dxcore_adapter_factory_Release(IDXCoreAdapterFactory *iface)
{
    struct dxcore_adapter_factory *factory = impl_from_IDXCoreAdapterFactory(iface);
    ULONG refcount;

    AcquireSRWLockExclusive(&factory_refcount_lock);
    refcount = InterlockedDecrement(&factory->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        dxcore_capability_cache_cleanup();
        dxcore_wined3d_cache_cleanup();
    }
    ReleaseSRWLockExclusive(&factory_refcount_lock);

    return refcount;
}

static struct dxcore_adapter *dxcore_adapter_cache_get(REFLUID luid)
{
    struct dxcore_adapter *adapter;

    AcquireSRWLockShared(&adapter_cache_lock);
    for (adapter = adapter_cache; adapter; adapter = adapter->cache_next)
    {
        if (!memcmp(luid, &adapter->identifier.adapter_luid, sizeof(*luid))
                && InterlockedCompareExchange(&adapter->valid, 0, 0))
        {
            InterlockedIncrement(&adapter->refcount);
            break;
        }
    }
    ReleaseSRWLockShared(&adapter_cache_lock);

    return adapter;
}

static HRESULT dxcore_adapter_create_uncached(const struct wined3d_adapter *wined3d_adapter,
        IDXCoreAdapterFactory *factory, struct dxcore_adapter **ret_adapter)
{
    struct dxcore_adapter *adapter;
    HRESULT hr;

    if (!(adapter = calloc(1, sizeof(*adapter))))
        return E_OUTOFMEMORY;

    adapter->IDXCoreAdapter_iface.lpVtbl = &dxcore_adapter_vtbl;
    adapter->refcount = 1;
    adapter->valid = TRUE;

    adapter->identifier.description_size = sizeof(adapter->driver_description);
    adapter->identifier.description = adapter->driver_description;
    if (FAILED(hr = wined3d_adapter_get_identifier(wined3d_adapter, 0, &adapter->identifier)))
    {
        free(adapter);
        return hr;
    }
    adapter->driver_description[ARRAY_SIZE(adapter->driver_description) - 1] = 0;

    adapter->factory = factory;
    IDXCoreAdapterFactory_AddRef(factory);

    *ret_adapter = adapter;
    return S_OK;
}

static HRESULT dxcore_adapter_get_or_create(const struct wined3d_adapter *wined3d_adapter,
        IDXCoreAdapterFactory *factory, struct dxcore_adapter **ret_adapter)
{
    struct dxcore_adapter *adapter, *cached_adapter;
    HRESULT hr;

    if (FAILED(hr = dxcore_adapter_create_uncached(wined3d_adapter, factory, &adapter)))
        return hr;

    AcquireSRWLockExclusive(&adapter_cache_lock);
    for (cached_adapter = adapter_cache; cached_adapter; cached_adapter = cached_adapter->cache_next)
    {
        if (!memcmp(&adapter->identifier.adapter_luid,
                &cached_adapter->identifier.adapter_luid, sizeof(LUID)))
        {
            if (InterlockedCompareExchange(&cached_adapter->valid, 0, 0)
                    && dxcore_adapter_snapshot_equal(cached_adapter, adapter))
            {
                InterlockedIncrement(&cached_adapter->refcount);
                ReleaseSRWLockExclusive(&adapter_cache_lock);
                IDXCoreAdapter_Release(&adapter->IDXCoreAdapter_iface);
                *ret_adapter = cached_adapter;
                return S_OK;
            }
            InterlockedExchange(&cached_adapter->valid, FALSE);
        }
    }

    adapter->cache_next = adapter_cache;
    adapter_cache = adapter;
    ReleaseSRWLockExclusive(&adapter_cache_lock);

    TRACE("Created adapter %p.\n", adapter);
    *ret_adapter = adapter;
    return S_OK;
}

static HRESULT get_adapters(struct dxcore_adapter_list *list, uint32_t num_attributes,
        const GUID *filter_attributes, BOOL use_cache)
{
    struct wined3d *wined3d = dxcore_wined3d_acquire(TRUE);
    uint32_t adapter_capacity, i, j;
    SIZE_T max_adapter_count;
    HRESULT hr = S_OK;

    if (!wined3d)
        return E_FAIL;

    if (!(adapter_capacity = wined3d_get_adapter_count(wined3d)))
        goto done;

    max_adapter_count = ~(SIZE_T)0 / sizeof(*list->adapters);
    if (adapter_capacity > max_adapter_count)
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    if (!(list->adapters = calloc(adapter_capacity, sizeof(*list->adapters))))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    for (i = 0; i < adapter_capacity; ++i)
    {
        struct dxcore_adapter *adapter;

        if (use_cache)
            hr = dxcore_adapter_get_or_create(wined3d_get_adapter(wined3d, i), list->factory, &adapter);
        else
            hr = dxcore_adapter_create_uncached(wined3d_get_adapter(wined3d, i), list->factory, &adapter);
        if (FAILED(hr))
            goto done;

        for (j = 0; j < num_attributes; ++j)
        {
            if (!dxcore_adapter_supports_attribute(adapter, &filter_attributes[j]))
                break;
        }

        if (j == num_attributes)
            list->adapters[list->adapter_count++] = adapter;
        else
            IDXCoreAdapter_Release(&adapter->IDXCoreAdapter_iface);
    }

done:
    wined3d_decref(wined3d);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_factory_CreateAdapterList(IDXCoreAdapterFactory *iface, uint32_t num_attributes,
        const GUID *filter_attributes, REFIID riid, void **out)
{
    struct dxcore_adapter_list *list;
    SIZE_T max_attribute_count;
    HRESULT hr;

    TRACE("iface %p, num_attributes %u, filter_attributes %p, riid %s, out %p.\n", iface, num_attributes, filter_attributes,
            debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;

    *out = NULL;

    if (!num_attributes || !filter_attributes)
        return E_INVALIDARG;

    max_attribute_count = ~(SIZE_T)0 / sizeof(*list->filter_attributes);
    if (num_attributes > max_attribute_count)
        return E_OUTOFMEMORY;

    if (!(list = calloc(1, sizeof(*list))))
        return E_OUTOFMEMORY;

    list->IDXCoreAdapterList_iface.lpVtbl = &dxcore_adapter_list_vtbl;
    list->refcount = 1;
    InitializeSRWLock(&list->lock);
    list->factory = iface;
    IDXCoreAdapterFactory_AddRef(iface);

    if (!(list->filter_attributes = calloc(num_attributes, sizeof(*list->filter_attributes))))
    {
        IDXCoreAdapterList_Release(&list->IDXCoreAdapterList_iface);
        return E_OUTOFMEMORY;
    }
    memcpy(list->filter_attributes, filter_attributes, num_attributes * sizeof(*filter_attributes));
    list->filter_attribute_count = num_attributes;

    if (FAILED(hr = get_adapters(list, num_attributes, filter_attributes, TRUE)))
    {
        IDXCoreAdapterList_Release(&list->IDXCoreAdapterList_iface);
        return hr;
    }

    hr = IDXCoreAdapterList_QueryInterface(&list->IDXCoreAdapterList_iface, riid, out);
    IDXCoreAdapterList_Release(&list->IDXCoreAdapterList_iface);
    TRACE("created IDXCoreAdapterList %p.\n", *out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_factory_GetAdapterByLuid(IDXCoreAdapterFactory *iface, REFLUID adapter_luid,
        REFIID riid, void **out)
{
    struct dxcore_adapter *adapter;
    struct wined3d *wined3d;
    uint32_t count;
    HRESULT hr;

    TRACE("iface %p, adapter_luid %p, riid %s, out %p.\n", iface, adapter_luid, debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;

    *out = NULL;

    if (!adapter_luid)
        return E_POINTER;

    if ((adapter = dxcore_adapter_cache_get(adapter_luid)))
    {
        if (IDXCoreAdapter_IsValid(&adapter->IDXCoreAdapter_iface))
        {
            hr = IDXCoreAdapter_QueryInterface(&adapter->IDXCoreAdapter_iface, riid, out);
            IDXCoreAdapter_Release(&adapter->IDXCoreAdapter_iface);
            return hr;
        }
        IDXCoreAdapter_Release(&adapter->IDXCoreAdapter_iface);
    }

    if (!(wined3d = dxcore_wined3d_acquire(TRUE)))
        return E_FAIL;

    count = wined3d_get_adapter_count(wined3d);
    for (uint32_t i = 0; i < count; ++i)
    {
        struct wined3d_adapter_identifier adapter_id = {0};
        struct wined3d_adapter *wined3d_adapter;

        wined3d_adapter = wined3d_get_adapter(wined3d, i);
        if (FAILED(hr = wined3d_adapter_get_identifier(wined3d_adapter, 0, &adapter_id)))
        {
            wined3d_decref(wined3d);
            return hr;
        }

        if (!memcmp(adapter_luid, &adapter_id.adapter_luid, sizeof(LUID)))
        {
            hr = dxcore_adapter_get_or_create(wined3d_adapter, iface, &adapter);
            wined3d_decref(wined3d);

            if (SUCCEEDED(hr))
            {
                hr = IDXCoreAdapter_QueryInterface(&adapter->IDXCoreAdapter_iface, riid, out);
                IDXCoreAdapter_Release(&adapter->IDXCoreAdapter_iface);
            }
            return hr;
        }
    }

    wined3d_decref(wined3d);
    return E_INVALIDARG;
}

static BOOL STDMETHODCALLTYPE dxcore_adapter_factory_IsNotificationTypeSupported(IDXCoreAdapterFactory *iface, DXCoreNotificationType type)
{
    FIXME("iface %p, type %u stub!\n", iface, type);
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_factory_RegisterEventNotification(IDXCoreAdapterFactory *iface, IUnknown *dxcore_object,
        DXCoreNotificationType type, PFN_DXCORE_NOTIFICATION_CALLBACK callback, void *callback_context, uint32_t *event_cookie)
{
    FIXME("iface %p, dxcore_object %p, type %u, callback %p, callback_context %p, event_cookie %p stub!\n", iface, dxcore_object, type, callback,
            callback_context, event_cookie);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxcore_adapter_factory_UnregisterEventNotification(IDXCoreAdapterFactory *iface, uint32_t event_cookie)
{
    FIXME("iface %p, event_cookie %u stub!\n", iface, event_cookie);
    return E_NOTIMPL;
}

static const struct IDXCoreAdapterFactoryVtbl dxcore_adapter_factory_vtbl =
{
    /* IUnknown methods */
    dxcore_adapter_factory_QueryInterface,
    dxcore_adapter_factory_AddRef,
    dxcore_adapter_factory_Release,
    /* IDXCoreAdapterFactory methods */
    dxcore_adapter_factory_CreateAdapterList,
    dxcore_adapter_factory_GetAdapterByLuid,
    dxcore_adapter_factory_IsNotificationTypeSupported,
    dxcore_adapter_factory_RegisterEventNotification,
    dxcore_adapter_factory_UnregisterEventNotification,
};

static struct dxcore_adapter_factory dxcore_adapter_factory = {
    .IDXCoreAdapterFactory_iface.lpVtbl = &dxcore_adapter_factory_vtbl,
    .refcount = 0
};

HRESULT STDMETHODCALLTYPE DXCoreCreateAdapterFactory(REFIID riid, void **out)
{
    HRESULT hr;

    TRACE("riid %s, out %p\n", debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;

    *out = NULL;
    hr = IDXCoreAdapterFactory_QueryInterface(&dxcore_adapter_factory.IDXCoreAdapterFactory_iface, riid, out);
    if (SUCCEEDED(hr))
        TRACE("returning factory %p\n", *out);

    return hr;
}
