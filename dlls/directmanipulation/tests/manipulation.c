/*
 *
 * Copyright 2019 Alistair Leslie-Hughes
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

#include <stdarg.h>

#include "windows.h"
#include "directmanipulation.h"

#include "wine/test.h"

static void test_IDirectManipulationManager2(void)
{
    IDirectManipulationViewport2 *viewport;
    IDirectManipulationManager2 *manager2;
    IDirectManipulationUpdateManager *update;
    DIRECTMANIPULATION_STATUS status;
    RECT rect = {10, 20, 300, 400}, result;
    HWND window;
    HRESULT hres;

    hres = CoCreateInstance(&CLSID_DirectManipulationManager, NULL, CLSCTX_INPROC_SERVER|CLSCTX_INPROC_HANDLER,
            &IID_IDirectManipulationManager2, (void**)&manager2);
    if(FAILED(hres))
    {
        win_skip("Failed to create XMLView instance\n");
        return;
    }
    ok(hres == S_OK, "CoCreateInstance returned %lx, expected S_OK\n", hres);

    hres = IDirectManipulationManager2_GetUpdateManager(manager2, &IID_IDirectManipulationUpdateManager, (void**)&update);
    ok(hres == S_OK, "returned %lx, expected S_OK\n", hres);

    if(update)
        IDirectManipulationUpdateManager_Release(update);

    window = CreateWindowW(L"static", L"direct manipulation", WS_POPUP, 0, 0, 100, 100,
                           NULL, NULL, NULL, NULL);
    ok(!!window, "Failed to create window, error %lu.\n", GetLastError());
    if (!window) goto done;

    hres = IDirectManipulationManager2_Activate(manager2, window);
    ok(hres == S_OK, "Activate returned %lx, expected S_OK.\n", hres);
    if (FAILED(hres)) goto destroy_window;
    hres = IDirectManipulationManager2_CreateViewport(manager2, NULL, window,
            &IID_IDirectManipulationViewport2, (void **)&viewport);
    ok(hres == S_OK, "CreateViewport returned %lx, expected S_OK.\n", hres);
    if (FAILED(hres)) goto deactivate;

    hres = IDirectManipulationViewport2_SetViewportRect(viewport, &rect);
    ok(hres == S_OK, "SetViewportRect returned %lx, expected S_OK.\n", hres);
    SetRectEmpty(&result);
    hres = IDirectManipulationViewport2_GetViewportRect(viewport, &result);
    ok(hres == S_OK, "GetViewportRect returned %lx, expected S_OK.\n", hres);
    ok(EqualRect(&rect, &result), "Unexpected viewport rect %s.\n", wine_dbgstr_rect(&result));

    hres = IDirectManipulationViewport2_SetViewportOptions(viewport,
            DIRECTMANIPULATION_VIEWPORT_OPTIONS_AUTODISABLE | DIRECTMANIPULATION_VIEWPORT_OPTIONS_INPUT);
    ok(hres == S_OK, "SetViewportOptions returned %lx, expected S_OK.\n", hres);
    hres = IDirectManipulationViewport2_ActivateConfiguration(viewport,
            DIRECTMANIPULATION_CONFIGURATION_INTERACTION | DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X |
            DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y);
    ok(hres == S_OK, "ActivateConfiguration returned %lx, expected S_OK.\n", hres);
    hres = IDirectManipulationViewport2_Stop(viewport);
    ok(hres == S_OK, "Stop returned %lx, expected S_OK.\n", hres);
    hres = IDirectManipulationViewport2_GetStatus(viewport, &status);
    ok(hres == S_OK, "GetStatus returned %lx, expected S_OK.\n", hres);
    ok(status == DIRECTMANIPULATION_READY, "Unexpected status %u.\n", status);
    hres = IDirectManipulationViewport2_Abandon(viewport);
    ok(hres == S_OK, "Abandon returned %lx, expected S_OK.\n", hres);

    IDirectManipulationViewport2_Release(viewport);
deactivate:
    hres = IDirectManipulationManager2_Deactivate(manager2, window);
    ok(hres == S_OK, "Deactivate returned %lx, expected S_OK.\n", hres);
destroy_window:
    DestroyWindow(window);

done:
    IDirectManipulationManager2_Release(manager2);
}

START_TEST(manipulation)
{
    CoInitialize(NULL);

    test_IDirectManipulationManager2();

    CoUninitialize();
}
