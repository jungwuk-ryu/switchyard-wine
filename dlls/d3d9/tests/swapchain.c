/*
 * Direct3D 9 swapchain tests
 *
 * Copyright 2026 Switchyard Project
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

#include <d3d9.h>

#include "wine/test.h"

static HWND create_test_window(const char *class_name)
{
    WNDCLASSA wc = {0};

    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = class_name;
    RegisterClassA(&wc);

    return CreateWindowA(class_name, class_name, WS_OVERLAPPEDWINDOW,
            0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
}

static void test_context_after_swapchain_release(void)
{
    D3DPRESENT_PARAMETERS present_parameters = {0};
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3DQuery9 *query = NULL;
    IDirect3D9 *d3d9;
    HWND device_window;
    HWND swapchain_window;
    unsigned int i;
    ULONG refcount;
    BOOL signalled;
    BOOL query_issued = FALSE;
    HRESULT hr;

    device_window = create_test_window("d3d9_device_window");
    swapchain_window = create_test_window("d3d9_swapchain_window");
    ok(!!device_window && !!swapchain_window, "Failed to create test windows.\n");
    if (!device_window || !swapchain_window)
        goto done;

    d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    ok(!!d3d9, "Failed to create a D3D9 object.\n");
    if (!d3d9)
        goto done;

    present_parameters.BackBufferWidth = 64;
    present_parameters.BackBufferHeight = 64;
    present_parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    present_parameters.BackBufferCount = 1;
    present_parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present_parameters.hDeviceWindow = device_window;
    present_parameters.Windowed = TRUE;

    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, device_window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present_parameters, &device);
    if (FAILED(hr))
    {
        skip("Failed to create a D3D9 device, hr %#lx.\n", hr);
        IDirect3D9_Release(d3d9);
        goto done;
    }

    present_parameters.hDeviceWindow = swapchain_window;
    hr = IDirect3DDevice9_CreateAdditionalSwapChain(device, &present_parameters, &swapchain);
    ok(SUCCEEDED(hr), "Failed to create additional swapchain, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto release_device;

    hr = IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_EVENT, &query);
    if (FAILED(hr))
        skip("Event queries are not supported, hr %#lx.\n", hr);

    hr = IDirect3DSwapChain9_Present(swapchain, NULL, NULL, NULL, NULL, 0);
    ok(SUCCEEDED(hr), "Failed to present additional swapchain, hr %#lx.\n", hr);

    if (query)
    {
        hr = IDirect3DQuery9_Issue(query, D3DISSUE_END);
        ok(SUCCEEDED(hr), "Failed to issue event query, hr %#lx.\n", hr);
        query_issued = SUCCEEDED(hr);
    }

    refcount = IDirect3DSwapChain9_Release(swapchain);
    ok(!refcount, "Additional swapchain has %lu references left.\n", refcount);

    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear after releasing additional swapchain, hr %#lx.\n", hr);
    hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    ok(SUCCEEDED(hr), "Failed to present after releasing additional swapchain, hr %#lx.\n", hr);

    if (query_issued)
    {
        for (i = 0; i < 1000; ++i)
        {
            hr = IDirect3DQuery9_GetData(query, &signalled, sizeof(signalled), D3DGETDATA_FLUSH);
            if (hr != S_FALSE)
                break;
            Sleep(1);
        }
        ok(hr == S_OK, "Failed to complete event query, hr %#lx.\n", hr);
    }

    if (query)
    {
        refcount = IDirect3DQuery9_Release(query);
        ok(!refcount, "Event query has %lu references left.\n", refcount);
    }

release_device:
    refcount = IDirect3DDevice9_Release(device);
    ok(!refcount, "D3D9 device has %lu references left.\n", refcount);
    IDirect3D9_Release(d3d9);
done:
    if (swapchain_window)
        DestroyWindow(swapchain_window);
    if (device_window)
        DestroyWindow(device_window);
}

START_TEST(swapchain)
{
    test_context_after_swapchain_release();
}
