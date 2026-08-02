/*
 * Windows.Management.Deployment activation factory tests
 *
 * Copyright 2026 Jungwuk Ryu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "windows.h"
#include "roapi.h"
#include "winstring.h"
#include "windows.foundation.h"
#include "windows.management.deployment.h"

#include "wine/test.h"

typedef HRESULT (WINAPI *pDllGetActivationFactory)(HSTRING,
                                                    IActivationFactory **);

static void check_unknown_class(pDllGetActivationFactory get_factory,
                                const WCHAR *buffer, UINT32 length)
{
    IActivationFactory *factory = (IActivationFactory *)0xdeadbeef;
    HSTRING classid;
    HRESULT hr;

    hr = WindowsCreateString(buffer, length, &classid);
    ok(hr == S_OK, "WindowsCreateString failed, hr %#lx.\n", hr);
    if (FAILED(hr)) return;

    hr = get_factory(classid, &factory);
    ok(hr == CLASS_E_CLASSNOTAVAILABLE && !factory,
       "Unexpected activation result %#lx, factory %p.\n", hr, factory);
    WindowsDeleteString(classid);
}

static void test_activation_factory(void)
{
    static const WCHAR embedded_nul[] =
        L"Windows.Management.Deployment.PackageManager\0Suffix";
    static const WCHAR prefix[] = L"Windows.Management.Deployment.Package";
    static const WCHAR suffix[] =
        L"Windows.Management.Deployment.PackageManager.Suffix";
    pDllGetActivationFactory get_factory;
    IActivationFactory *factory;
    HMODULE module;
    HSTRING classid;
    HRESULT hr;

    module = LoadLibraryW(L"appxdeploymentclient.dll");
    ok(!!module, "Failed to load appxdeploymentclient.dll, error %lu.\n",
       GetLastError());
    if (!module) return;

    get_factory = (void *)GetProcAddress(module, "DllGetActivationFactory");
    ok(!!get_factory, "DllGetActivationFactory is not exported.\n");
    if (!get_factory)
    {
        FreeLibrary(module);
        return;
    }

    factory = (IActivationFactory *)0xdeadbeef;
    hr = get_factory(NULL, &factory);
    ok(hr == CLASS_E_CLASSNOTAVAILABLE && !factory,
       "NULL class returned %#lx, factory %p.\n", hr, factory);

    hr = WindowsCreateString(
        RuntimeClass_Windows_Management_Deployment_PackageManager,
        ARRAY_SIZE(RuntimeClass_Windows_Management_Deployment_PackageManager) - 1,
        &classid);
    ok(hr == S_OK, "WindowsCreateString failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        factory = NULL;
        hr = get_factory(classid, NULL);
        ok(hr == E_POINTER, "NULL output returned %#lx.\n", hr);
        hr = get_factory(classid, &factory);
        ok(hr == S_OK && !!factory,
           "PackageManager activation returned %#lx, factory %p.\n", hr,
           factory);
        if (factory) IActivationFactory_Release(factory);
        WindowsDeleteString(classid);
    }

    check_unknown_class(get_factory, embedded_nul,
                        ARRAY_SIZE(embedded_nul) - 1);
    check_unknown_class(get_factory, prefix, ARRAY_SIZE(prefix) - 1);
    check_unknown_class(get_factory, suffix, ARRAY_SIZE(suffix) - 1);
    check_unknown_class(get_factory, L"", 0);

    FreeLibrary(module);
}

START_TEST(main)
{
    test_activation_factory();
}
