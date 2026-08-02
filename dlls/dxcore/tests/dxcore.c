/*
 * Copyright (C) 2025 Mohamad Al-Jaf
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
#include "initguid.h"
#include "dxcore.h"

#include "wine/test.h"

static HRESULT (WINAPI *pDXCoreCreateAdapterFactory)(REFIID riid, void **out);

#define check_interface(iface, riid, supported) check_interface_(__LINE__, iface, riid, supported)
static void check_interface_(unsigned int line, void *iface, REFIID riid, BOOL supported)
{
    IUnknown *unknown = iface, *out;
    HRESULT hr, expected_hr;

    expected_hr = supported ? S_OK : E_NOINTERFACE;

    hr = IUnknown_QueryInterface(unknown, riid, (void **)&out);
    ok_(__FILE__, line)(hr == expected_hr, "got hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        IUnknown_Release(out);
}

static BOOL is_any_display_available(void)
{
    DISPLAY_DEVICEW display_device;
    display_device.cb = sizeof(display_device);
    return EnumDisplayDevicesW(NULL, 0, &display_device, 0);
}

static void test_DXCoreCreateAdapterFactory(void)
{
    IDXCoreAdapterFactory *factory2 = (void *)0xdeadbeef;
    IDXCoreAdapterFactory *factory = (void *)0xdeadbeef;
    IDXCoreAdapterList *list2 = (void *)0xdeadbeef;
    IDXCoreAdapterList *list = (void *)0xdeadbeef;
    IDXCoreAdapter *adapter2 = (void *)0xdeadbeef;
    IDXCoreAdapter *adapter = (void *)0xdeadbeef;
    uint32_t adapter_count = 0;
    LONG refcount;
    HRESULT hr;

    if (0) /* Crashes on w1064v1909 */
    {
        hr = pDXCoreCreateAdapterFactory(NULL, NULL);
        ok(hr == E_POINTER, "got hr %#lx.\n", hr);
    }
    hr = pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, NULL);
    ok(hr == E_POINTER || broken(hr == E_NOINTERFACE) /* w1064v1909 */, "got hr %#lx.\n", hr);
    hr = pDXCoreCreateAdapterFactory(&DXCORE_ADAPTER_ATTRIBUTE_D3D11_GRAPHICS, (void **)&factory);
    ok(hr == E_NOINTERFACE, "got hr %#lx.\n", hr);
    ok(factory == NULL || broken(factory == (void *)0xdeadbeef) /* w1064v1909 */, "got factory %p.\n", factory);

    hr = pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&factory);
    ok(hr == S_OK || broken(hr == E_NOINTERFACE) /* w1064v1909 */, "got hr %#lx.\n", hr);
    if (FAILED(hr))
        return;

    hr = pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&factory2);
    ok(hr == S_OK, "got hr %#lx.\n", hr);
    ok(factory == factory2, "got factory %p, factory2 %p.\n", factory, factory2);
    refcount = IDXCoreAdapterFactory_Release(factory2);
    ok(refcount == 1, "got refcount %ld.\n", refcount);

    check_interface(factory, &IID_IUnknown, TRUE);
    check_interface(factory, &IID_IDXCoreAdapterFactory, TRUE);
    check_interface(factory, &IID_IAgileObject, FALSE);
    check_interface(factory, &IID_IDXCoreAdapter, FALSE);
    check_interface(factory, &IID_IDXCoreAdapterList, FALSE);

    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 0, NULL, &IID_IDXCoreAdapterList, (void **)&list);
    ok(hr == E_INVALIDARG, "got hr %#lx.\n", hr);
    ok(list == NULL, "got list %p.\n", list);
    list = (void *)0xdeadbeef;
    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 0, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, &IID_IDXCoreAdapterList, (void **)&list);
    ok(hr == E_INVALIDARG, "got hr %#lx.\n", hr);
    ok(list == NULL, "got list %p.\n", list);
    list = (void *)0xdeadbeef;
    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, &IID_IDXCoreAdapterFactory, (void **)&list);
    ok(hr == E_NOINTERFACE, "got hr %#lx.\n", hr);
    ok(list == NULL, "got list %p.\n", list);
    list = (void *)0xdeadbeef;
    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, NULL, &IID_IDXCoreAdapterFactory, (void **)&list);
    ok(hr == E_INVALIDARG, "got hr %#lx.\n", hr);
    ok(list == NULL, "got list %p.\n", list);
    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, &IID_IDXCoreAdapterFactory, NULL);
    ok(hr == E_POINTER, "got hr %#lx.\n", hr);

    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, &IID_IDXCoreAdapterList, (void **)&list);
    ok(hr == S_OK, "got hr %#lx.\n", hr);
    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, &IID_IDXCoreAdapterList, (void **)&list2);
    ok(hr == S_OK, "got hr %#lx.\n", hr);
    ok(list != list2, "got same list %p, list2 %p.\n", list, list);
    refcount = IDXCoreAdapterList_Release(list2);
    ok(refcount == 0, "got refcount %ld.\n", refcount);

    check_interface(list, &IID_IUnknown, TRUE);
    check_interface(list, &IID_IDXCoreAdapterList, TRUE);
    check_interface(list, &IID_IAgileObject, FALSE);
    check_interface(list, &IID_IDXCoreAdapter, FALSE);
    check_interface(list, &IID_IDXCoreAdapterFactory, FALSE);

    adapter_count = IDXCoreAdapterList_GetAdapterCount(list);
    if (!adapter_count)
    {
        win_skip("No D3D12 graphics adapter is available.\n");
        IDXCoreAdapterList_Release(list);
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    hr = IDXCoreAdapterList_GetAdapter(list, 0xdeadbeef, &IID_IDXCoreAdapter, NULL);
    ok(hr == E_POINTER, "got hr %#lx.\n", hr);
    hr = IDXCoreAdapterList_GetAdapter(list, adapter_count, &IID_IDXCoreAdapter, (void **)&adapter);
    ok(hr == E_INVALIDARG, "got hr %#lx.\n", hr);
    ok(adapter == NULL, "got adapter %p.\n", adapter);
    hr = IDXCoreAdapterList_GetAdapter(list, 0, &IID_IDXCoreAdapterList, (void **)&adapter);
    ok(hr == E_NOINTERFACE, "got hr %#lx.\n", hr);
    ok(adapter == NULL, "got adapter %p.\n", adapter);

    hr = IDXCoreAdapterList_GetAdapter(list, 0, &IID_IDXCoreAdapter, (void **)&adapter);
    ok(hr == S_OK, "got hr %#lx.\n", hr);
    hr = IDXCoreAdapterList_GetAdapter(list, 0, &IID_IDXCoreAdapter, (void **)&adapter2);
    ok(hr == S_OK, "got hr %#lx.\n", hr);
    ok(adapter == adapter2, "got adapter %p, adapter2 %p.\n", adapter, adapter2);
    refcount = IDXCoreAdapter_Release(adapter2);
    todo_wine
    ok(refcount == 3, "got refcount %ld.\n", refcount);

    check_interface(adapter, &IID_IUnknown, TRUE);
    check_interface(adapter, &IID_IDXCoreAdapter, TRUE);
    check_interface(adapter, &IID_IAgileObject, FALSE);
    check_interface(adapter, &IID_IDXCoreAdapterList, FALSE);
    check_interface(adapter, &IID_IDXCoreAdapterFactory, FALSE);

    refcount = IDXCoreAdapter_Release(adapter);
    todo_wine
    ok(refcount == 2, "got refcount %ld.\n", refcount);
    refcount = IDXCoreAdapterList_Release(list);
    ok(refcount == 0, "got refcount %ld.\n", refcount);
    refcount = IDXCoreAdapterFactory_Release(factory);
    ok(refcount == 0, "got refcount %ld.\n", refcount);
}

static void test_GetFactory(void)
{
    IDXCoreAdapterFactory *factory2 = (void *)0xdeadbeef;
    IDXCoreAdapterFactory *factory;
    IDXCoreAdapterList *list;
    IDXCoreAdapter *adapter;
    void *factory_identity;
    uint32_t count;
    LONG refcount;
    HRESULT hr;

    if (FAILED(pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&factory)))
        return;

    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS,
            &IID_IDXCoreAdapterList, (void **)&list);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    hr = IDXCoreAdapterList_GetFactory(list, &IID_IDXCoreAdapterFactory, NULL);
    ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

    hr = IDXCoreAdapterList_GetFactory(list, &IID_IDXCoreAdapter, (void **)&factory2);
    ok(hr == E_NOINTERFACE, "Got hr %#lx.\n", hr);
    ok(factory2 == NULL, "Got factory %p.\n", factory2);

    hr = IDXCoreAdapterList_GetFactory(list, &IID_IDXCoreAdapterFactory, (void **)&factory2);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(factory2 == factory, "Got factory %p, expected %p.\n", factory2, factory);
    if (SUCCEEDED(hr))
    {
        refcount = IDXCoreAdapterFactory_Release(factory2);
        ok(refcount > 0, "Got refcount %ld.\n", refcount);
    }

    count = IDXCoreAdapterList_GetAdapterCount(list);
    if (!count)
    {
        win_skip("No D3D12 graphics adapter is available.\n");
        IDXCoreAdapterList_Release(list);
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    hr = IDXCoreAdapterList_GetAdapter(list, 0, &IID_IDXCoreAdapter, (void **)&adapter);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDXCoreAdapterList_Release(list);
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    ok(IDXCoreAdapter_IsValid(adapter), "Expected a valid adapter.\n");

    hr = IDXCoreAdapter_GetFactory(adapter, &IID_IDXCoreAdapterFactory, NULL);
    ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

    factory2 = (void *)0xdeadbeef;
    hr = IDXCoreAdapter_GetFactory(adapter, &IID_IDXCoreAdapter, (void **)&factory2);
    ok(hr == E_NOINTERFACE, "Got hr %#lx.\n", hr);
    ok(factory2 == NULL, "Got factory %p.\n", factory2);

    factory_identity = factory;
    refcount = IDXCoreAdapterFactory_Release(factory);
    ok(refcount > 0, "Got refcount %ld.\n", refcount);

    refcount = IDXCoreAdapterList_Release(list);
    ok(refcount == 0, "Got refcount %ld.\n", refcount);

    factory2 = (void *)0xdeadbeef;
    hr = pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&factory2);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(factory2 == factory_identity, "Got factory %p, expected %p.\n", factory2, factory_identity);
    if (SUCCEEDED(hr))
    {
        refcount = IDXCoreAdapterFactory_Release(factory2);
        ok(refcount > 0, "Got refcount %ld.\n", refcount);
    }

    factory2 = (void *)0xdeadbeef;
    hr = IDXCoreAdapter_GetFactory(adapter, &IID_IDXCoreAdapterFactory, (void **)&factory2);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(factory2 == factory_identity, "Got factory %p, expected %p.\n", factory2, factory_identity);
    if (SUCCEEDED(hr))
    {
        refcount = IDXCoreAdapterFactory_Release(factory2);
        ok(refcount > 0, "Got refcount %ld.\n", refcount);
    }

    IDXCoreAdapter_Release(adapter);
}

static void test_Sort(void)
{
    static const DXCoreAdapterPreference preferences[] =
    {
        Hardware,
        HighPerformance,
        MinimumPower,
    };
    DXCoreAdapterPreference invalid_preference = 0xdeadbeef;
    IDXCoreAdapterFactory *factory;
    IDXCoreAdapterList *list;
    IDXCoreAdapter *adapter;
    LUID *order, luid;
    BOOL saw_software;
    BYTE is_hardware;
    uint32_t count, i;
    HRESULT hr;

    if (FAILED(pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&factory)))
        return;

    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS,
            &IID_IDXCoreAdapterList, (void **)&list);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    ok(IDXCoreAdapterList_IsAdapterPreferenceSupported(list, Hardware),
            "Expected Hardware preference support.\n");
    ok(IDXCoreAdapterList_IsAdapterPreferenceSupported(list, MinimumPower),
            "Expected MinimumPower preference support.\n");
    ok(IDXCoreAdapterList_IsAdapterPreferenceSupported(list, HighPerformance),
            "Expected HighPerformance preference support.\n");
    ok(!IDXCoreAdapterList_IsAdapterPreferenceSupported(list, invalid_preference),
            "Unexpected invalid preference support.\n");

    hr = IDXCoreAdapterList_Sort(list, 0, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDXCoreAdapterList_Sort(list, 0, preferences);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDXCoreAdapterList_Sort(list, 1, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    count = IDXCoreAdapterList_GetAdapterCount(list);
    order = calloc(count, sizeof(*order));
    ok(!count || !!order, "Failed to allocate adapter order.\n");
    if (count && !order)
    {
        IDXCoreAdapterList_Release(list);
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    for (i = 0; i < count; ++i)
    {
        hr = IDXCoreAdapterList_GetAdapter(list, i, &IID_IDXCoreAdapter, (void **)&adapter);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(order[i]), &order[i]);
            ok(hr == S_OK, "Got hr %#lx.\n", hr);
            IDXCoreAdapter_Release(adapter);
        }
    }

    hr = IDXCoreAdapterList_Sort(list, 1, &invalid_preference);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    for (i = 0; i < count; ++i)
    {
        hr = IDXCoreAdapterList_GetAdapter(list, i, &IID_IDXCoreAdapter, (void **)&adapter);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(luid), &luid);
            ok(hr == S_OK, "Got hr %#lx.\n", hr);
            ok(!memcmp(&luid, &order[i], sizeof(luid)), "Adapter order changed at index %u.\n", i);
            IDXCoreAdapter_Release(adapter);
        }
    }

    hr = IDXCoreAdapterList_Sort(list, ARRAY_SIZE(preferences), preferences);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    for (i = 0; i < count; ++i)
    {
        hr = IDXCoreAdapterList_GetAdapter(list, i, &IID_IDXCoreAdapter, (void **)&adapter);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(order[i]), &order[i]);
            ok(hr == S_OK, "Got hr %#lx.\n", hr);
            IDXCoreAdapter_Release(adapter);
        }
    }

    hr = IDXCoreAdapterList_Sort(list, ARRAY_SIZE(preferences), preferences);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    saw_software = FALSE;
    for (i = 0; i < count; ++i)
    {
        hr = IDXCoreAdapterList_GetAdapter(list, i, &IID_IDXCoreAdapter, (void **)&adapter);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(luid), &luid);
            ok(hr == S_OK, "Got hr %#lx.\n", hr);
            ok(!memcmp(&luid, &order[i], sizeof(luid)), "Adapter order changed at index %u.\n", i);

            hr = IDXCoreAdapter_GetProperty(adapter, IsHardware, sizeof(is_hardware), &is_hardware);
            ok(hr == S_OK, "Got hr %#lx.\n", hr);
            if (is_hardware)
                ok(!saw_software, "Found hardware adapter after a software adapter at index %u.\n", i);
            else
                saw_software = TRUE;
            IDXCoreAdapter_Release(adapter);
        }
    }

    ok(!IDXCoreAdapterList_IsStale(list), "Expected a fresh adapter list.\n");

    free(order);
    IDXCoreAdapterList_Release(list);
    IDXCoreAdapterFactory_Release(factory);
}

static void test_QueryState(void)
{
    DXCoreAdapterMemoryBudgetNodeSegmentGroup details = {0, Local};
    DXCoreAdapterMemoryBudget budget;
    IDXCoreAdapterFactory *factory;
    IDXCoreAdapterList *list;
    IDXCoreAdapter *adapter;
    BYTE update_in_progress;
    HRESULT hr;

    if (FAILED(pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&factory)))
        return;

    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1,
            &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS,
            &IID_IDXCoreAdapterList, (void **)&list);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    if (!IDXCoreAdapterList_GetAdapterCount(list))
    {
        win_skip("No D3D12 graphics adapter is available.\n");
        IDXCoreAdapterList_Release(list);
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    hr = IDXCoreAdapterList_GetAdapter(list, 0, &IID_IDXCoreAdapter, (void **)&adapter);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDXCoreAdapterList_Release(list);
        IDXCoreAdapterFactory_Release(factory);
        return;
    }

    ok(IDXCoreAdapter_IsQueryStateSupported(adapter, IsDriverUpdateInProgress),
            "Expected driver update state support.\n");
    ok(IDXCoreAdapter_IsQueryStateSupported(adapter, AdapterMemoryBudget),
            "Expected memory budget state support.\n");
    ok(!IDXCoreAdapter_IsQueryStateSupported(adapter, 0xdeadbeef),
            "Unexpected invalid state support.\n");

    update_in_progress = 0xff;
    hr = IDXCoreAdapter_QueryState(adapter, IsDriverUpdateInProgress,
            0, NULL, sizeof(update_in_progress), &update_in_progress);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(update_in_progress == FALSE || update_in_progress == TRUE,
            "Got invalid update state %#x.\n", update_in_progress);

    memset(&budget, 0xcc, sizeof(budget));
    hr = IDXCoreAdapter_QueryState(adapter, AdapterMemoryBudget,
            sizeof(details), &details, sizeof(budget), &budget);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(budget.budget >= budget.availableForReservation,
                "Budget %#I64x is smaller than available reservation %#I64x.\n",
                budget.budget, budget.availableForReservation);
    }

    hr = IDXCoreAdapter_QueryState(adapter, AdapterMemoryBudget,
            sizeof(details), NULL, sizeof(budget), &budget);
    ok(hr == E_POINTER, "Got hr %#lx.\n", hr);
    hr = IDXCoreAdapter_QueryState(adapter, AdapterMemoryBudget,
            sizeof(details) - 1, &details, sizeof(budget), &budget);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDXCoreAdapter_QueryState(adapter, AdapterMemoryBudget,
            sizeof(details), &details, sizeof(budget) - 1, &budget);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDXCoreAdapter_QueryState(adapter, AdapterMemoryBudget,
            sizeof(details), &details, sizeof(budget), NULL);
    ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

    details.segmentGroup = 0xdeadbeef;
    hr = IDXCoreAdapter_QueryState(adapter, AdapterMemoryBudget,
            sizeof(details), &details, sizeof(budget), &budget);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDXCoreAdapter_QueryState(adapter, 0xdeadbeef,
            0, NULL, sizeof(budget), &budget);
    ok(hr == DXGI_ERROR_INVALID_CALL, "Got hr %#lx.\n", hr);

    IDXCoreAdapter_Release(adapter);
    IDXCoreAdapterList_Release(list);
    IDXCoreAdapterFactory_Release(factory);
}

static void test_GetProperty(void)
{
    static const DXCoreAdapterProperty supported_properties[] =
    {
        InstanceLuid,
        DriverVersion,
        DriverDescription,
        HardwareID,
        DedicatedAdapterMemory,
        DedicatedSystemMemory,
        SharedSystemMemory,
        IsHardware,
    };
    IDXCoreAdapterFactory *factory;
    IDXCoreAdapterList *list;
    DXCoreHardwareID hwid[2];
    IDXCoreAdapter *adapter;
    uint32_t count, dummy;
    uint64_t memory;
    LARGE_INTEGER version;
    BYTE is_hardware;
    LUID luid[2];
    size_t size;
    HRESULT hr;
    char *str;

    if (FAILED(pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&factory)))
        return;

    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS,
            &IID_IDXCoreAdapterList, (void **)&list);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    count = IDXCoreAdapterList_GetAdapterCount(list);

    for (uint32_t i = 0; i < count; ++i)
    {
        hr = IDXCoreAdapterList_GetAdapter(list, i, &IID_IDXCoreAdapter, (void **)&adapter);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);

        ok(IDXCoreAdapter_IsAttributeSupported(adapter, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS),
                "Expected D3D12 graphics support.\n");
        ok(!IDXCoreAdapter_IsAttributeSupported(adapter, &IID_IDXCoreAdapter),
                "Unexpected unknown attribute support.\n");

        for (uint32_t j = 0; j < ARRAY_SIZE(supported_properties); ++j)
            ok(IDXCoreAdapter_IsPropertySupported(adapter, supported_properties[j]),
                    "Expected property %u support.\n", supported_properties[j]);
        ok(!IDXCoreAdapter_IsPropertySupported(adapter, 0xdeadbeef),
                "Unexpected invalid property support.\n");

        hr = IDXCoreAdapter_GetProperty(adapter, 0xdeadbeef, 0, hwid);
        ok(hr == DXGI_ERROR_INVALID_CALL, "Got hr %#lx.\n", hr);

        /* InstanceLuid */
        hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, 0, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, 0, luid);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(luid[0]) - 1, luid);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, InstanceLuid, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, InstanceLuid, &size);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(size == sizeof(*luid), "Got size %Iu.\n", size);

        hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(luid[0]), luid);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(luid[0]) + 1, luid);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);

        /* HardwareID */
        hr = IDXCoreAdapter_GetProperty(adapter, HardwareID, 0, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, HardwareID, 0, hwid);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, HardwareID, sizeof(hwid[0]) - 1, hwid);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, HardwareID, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, HardwareID, &size);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(size == sizeof(*hwid), "Got size %Iu.\n", size);

        memset(hwid, 0, sizeof(hwid));
        hr = IDXCoreAdapter_GetProperty(adapter, HardwareID, sizeof(hwid[0]), hwid);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(!!hwid[0].vendorID, "Expected vendorID.\n");

        memset(hwid, 0, sizeof(hwid));
        hr = IDXCoreAdapter_GetProperty(adapter, HardwareID, sizeof(hwid[0]) + 1, hwid);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(!!hwid[0].vendorID, "Expected vendorID.\n");
        ok(!hwid[1].vendorID, "Expected no vendorID.\n");
        ok(!hwid[1].deviceID, "Expected no deviceID.\n");

        /* DedicatedAdapterMemory */
        hr = IDXCoreAdapter_GetProperty(adapter, DedicatedAdapterMemory, 0, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, DedicatedAdapterMemory, 0, &memory);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, DedicatedAdapterMemory, sizeof(memory) - 1, &memory);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, DedicatedAdapterMemory, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, DedicatedAdapterMemory, &size);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(size == sizeof(memory), "Got size %Iu.\n", size);

        hr = IDXCoreAdapter_GetProperty(adapter, DedicatedAdapterMemory, sizeof(memory), &memory);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);

        /* DedicatedSystemMemory */
        hr = IDXCoreAdapter_GetProperty(adapter, DedicatedSystemMemory, 0, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, DedicatedSystemMemory, 0, &memory);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, DedicatedSystemMemory, sizeof(memory) - 1, &memory);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, DedicatedSystemMemory, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, DedicatedSystemMemory, &size);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(size == sizeof(memory), "Got size %Iu.\n", size);

        hr = IDXCoreAdapter_GetProperty(adapter, DedicatedSystemMemory, sizeof(memory), &memory);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);

        /* SharedSystemMemory */
        hr = IDXCoreAdapter_GetProperty(adapter, SharedSystemMemory, 0, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, SharedSystemMemory, 0, &memory);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, SharedSystemMemory, sizeof(memory) - 1, &memory);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, SharedSystemMemory, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, SharedSystemMemory, &size);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(size == sizeof(memory), "Got size %Iu.\n", size);

        hr = IDXCoreAdapter_GetProperty(adapter, SharedSystemMemory, sizeof(memory), &memory);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);

        /* IsHardware */
        hr = IDXCoreAdapter_GetProperty(adapter, IsHardware, 0, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);
        hr = IDXCoreAdapter_GetProperty(adapter, IsHardware, 0, &is_hardware);
        ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, IsHardware, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetPropertySize(adapter, IsHardware, &size);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(size == sizeof(is_hardware), "Got property size.\n");

        is_hardware = 3;
        hr = IDXCoreAdapter_GetProperty(adapter, IsHardware, sizeof(is_hardware), &is_hardware);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(is_hardware == 0 || is_hardware == 1, "Got value %d.\n", is_hardware);

        dummy = 0;
        hr = IDXCoreAdapter_GetProperty(adapter, IsHardware, sizeof(dummy), &dummy);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(dummy == is_hardware, "Got value %#x.\n", dummy);

        /* DriverDescription */
        size = 0;
        hr = IDXCoreAdapter_GetPropertySize(adapter, DriverDescription, &size);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
        ok(!!size, "Unexpected property size.\n");

        str = malloc(size);

        hr = IDXCoreAdapter_GetProperty(adapter, DriverDescription, size, str);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetProperty(adapter, DriverDescription, size, NULL);
        ok(hr == E_POINTER, "Unexpected hr %#lx.\n", hr);

        *str = 0x1;
        hr = IDXCoreAdapter_GetProperty(adapter, DriverDescription, size - 1, str);
        ok(hr == E_INVALIDARG, "Unexpected hr %#lx.\n", hr);
        ok(!*str, "Unexpected buffer contents %s.\n", wine_dbgstr_a(str));

        *str = 0x1;
        hr = IDXCoreAdapter_GetProperty(adapter, DriverDescription, 0, str);
        ok(hr == E_INVALIDARG, "Unexpected hr %#lx.\n", hr);
        ok(*str == 0x1, "Unexpected buffer contents %s.\n", wine_dbgstr_a(str));

        free(str);

        /* DriverVersion */
        size = 0;
        hr = IDXCoreAdapter_GetPropertySize(adapter, DriverVersion, &size);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);
        ok(size == sizeof(LARGE_INTEGER), "Unexpected property size.\n");

        hr = IDXCoreAdapter_GetProperty(adapter, DriverVersion, size, &version);
        ok(hr == S_OK, "Unexpected hr %#lx.\n", hr);

        IDXCoreAdapter_Release(adapter);
    }

    IDXCoreAdapterList_Release(list);

    IDXCoreAdapterFactory_Release(factory);
}

static void test_GetAdapterByLuid(void)
{
    IDXCoreAdapter *adapter, *adapter2 = (void *)0xdeadbeef;
    IDXCoreAdapterFactory *factory;
    IDXCoreAdapterList *list;
    uint32_t count;
    HRESULT hr;
    LUID luid;

    if (FAILED(pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&factory)))
        return;

    hr = IDXCoreAdapterFactory_CreateAdapterList(factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS,
            &IID_IDXCoreAdapterList, (void **)&list);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    count = IDXCoreAdapterList_GetAdapterCount(list);

    for (uint32_t i = 0; i < count; ++i)
    {
        hr = IDXCoreAdapterList_GetAdapter(list, i, &IID_IDXCoreAdapter, (void **)&adapter);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapter_GetProperty(adapter, InstanceLuid, sizeof(luid), &luid);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);

        hr = IDXCoreAdapterFactory_GetAdapterByLuid(factory, &luid, &IID_IDXCoreAdapter, NULL);
        ok(hr == E_POINTER, "Got hr %#lx.\n", hr);

        adapter2 = (void *)0xdeadbeef;
        hr = IDXCoreAdapterFactory_GetAdapterByLuid(factory, &luid, &IID_IDXCoreAdapterList, (void **)&adapter2);
        ok(hr == E_NOINTERFACE, "Got hr %#lx.\n", hr);
        ok(adapter2 == NULL, "Got adapter %p.\n", adapter2);

        hr = IDXCoreAdapterFactory_GetAdapterByLuid(factory, &luid, &IID_IDXCoreAdapter, (void **)&adapter2);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        ok(adapter2 == adapter, "Got adapter %p, expected identity %p.\n", adapter2, adapter);
        if (adapter2)
            IDXCoreAdapter_Release(adapter2);

        IDXCoreAdapter_Release(adapter);
    }

    luid.LowPart = 0xdeadbeef;
    luid.HighPart = 0xdeadbeef;
    adapter2 = (void *)0xdeadbeef;
    hr = IDXCoreAdapterFactory_GetAdapterByLuid(factory, &luid, &IID_IDXCoreAdapter, (void **)&adapter2);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    ok(adapter2 == NULL, "Got adapter %p.\n", adapter2);

    IDXCoreAdapterList_Release(list);

    IDXCoreAdapterFactory_Release(factory);
}

START_TEST(dxcore)
{
    HMODULE dxcore_handle = LoadLibraryA("dxcore.dll");
    if (!dxcore_handle)
    {
        win_skip("Could not load dxcore.dll\n");
        return;
    }
    pDXCoreCreateAdapterFactory = (void *)GetProcAddress(dxcore_handle, "DXCoreCreateAdapterFactory");
    if (!pDXCoreCreateAdapterFactory)
    {
        win_skip("Failed to get DXCoreCreateAdapterFactory address, skipping dxcore tests\n");
        FreeLibrary(dxcore_handle);
        return;
    }

    if (!is_any_display_available())
    {
        skip("No display available.\n");
        return;
    }

    test_DXCoreCreateAdapterFactory();
    test_GetFactory();
    test_Sort();
    test_QueryState();
    test_GetProperty();
    test_GetAdapterByLuid();

    FreeLibrary(dxcore_handle);
}
