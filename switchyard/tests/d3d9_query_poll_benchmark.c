/*
 * D3D9 command-stream query polling benchmark.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static unsigned int parse_count(const char *value, const char *name)
{
    char *end;
    unsigned long count;

    count = strtoul(value, &end, 10);
    if (!value[0] || *end || !count || count > 100000)
    {
        fprintf(stderr, "%s must be an integer from 1 through 100000.\n", name);
        exit(2);
    }
    return count;
}

static uint64_t filetime_to_100ns(const FILETIME *time)
{
    return ((uint64_t)time->dwHighDateTime << 32) | time->dwLowDateTime;
}

int main(int argc, char **argv)
{
    const unsigned int warmup_generations = 8;
    unsigned int generations = 128, clears = 128;
    uint64_t total_polls = 0, cpu_100ns;
    FILETIME create_time, exit_time, kernel_start, kernel_end, user_start, user_end;
    unsigned int min_polls = UINT_MAX, max_polls = 0;
    D3DPRESENT_PARAMETERS present_parameters = {0};
    IDirect3DQuery9 *query = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9 = NULL;
    LARGE_INTEGER frequency, start, end;
    WNDCLASSA window_class = {0};
    unsigned int generation, i, polls;
    ULONGLONG poll_start;
    BOOL signalled = FALSE;
    HWND window;
    HRESULT hr;

    if (argc > 3)
    {
        fprintf(stderr, "usage: %s [GENERATIONS [CLEARS_PER_GENERATION]]\n", argv[0]);
        return 2;
    }
    if (argc >= 2)
        generations = parse_count(argv[1], "GENERATIONS");
    if (argc == 3)
        clears = parse_count(argv[2], "CLEARS_PER_GENERATION");

    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.lpszClassName = "switchyard_d3d9_query_poll_benchmark";
    if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        fprintf(stderr, "RegisterClassA failed, error %lu.\n", GetLastError());
        return 1;
    }

    if (!(window = CreateWindowA(window_class.lpszClassName, window_class.lpszClassName,
            WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, window_class.hInstance, NULL)))
    {
        fprintf(stderr, "CreateWindowA failed, error %lu.\n", GetLastError());
        return 1;
    }

    if (!(d3d9 = Direct3DCreate9(D3D_SDK_VERSION)))
    {
        fprintf(stderr, "Direct3DCreate9 failed.\n");
        DestroyWindow(window);
        return 1;
    }

    present_parameters.BackBufferWidth = 640;
    present_parameters.BackBufferHeight = 480;
    present_parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    present_parameters.BackBufferCount = 1;
    present_parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present_parameters.hDeviceWindow = window;
    present_parameters.Windowed = TRUE;
    present_parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &present_parameters, &device);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present_parameters, &device);
    if (FAILED(hr))
    {
        fprintf(stderr, "CreateDevice failed, hr %#lx.\n", hr);
        IDirect3D9_Release(d3d9);
        DestroyWindow(window);
        return 1;
    }

    hr = IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_EVENT, &query);
    if (FAILED(hr))
    {
        printf("d3d9_query_poll_benchmark status=skip reason=event_query_unavailable hr=%#lx\n", hr);
        IDirect3DDevice9_Release(device);
        IDirect3D9_Release(d3d9);
        DestroyWindow(window);
        return 0;
    }

    QueryPerformanceFrequency(&frequency);

    for (generation = 0; generation < warmup_generations + generations; ++generation)
    {
        for (i = 0; i < clears; ++i)
        {
            hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                    0xff000000u | (generation << 8) | i, 1.0f, 0);
            if (FAILED(hr))
            {
                fprintf(stderr, "Clear failed, hr %#lx.\n", hr);
                goto failed;
            }
        }

        if (generation == warmup_generations)
        {
            QueryPerformanceCounter(&start);
            GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel_start, &user_start);
        }

        hr = IDirect3DQuery9_Issue(query, D3DISSUE_END);
        if (FAILED(hr))
        {
            fprintf(stderr, "Issue failed, hr %#lx.\n", hr);
            goto failed;
        }

        polls = 0;
        poll_start = GetTickCount64();
        do
        {
            signalled = FALSE;
            hr = IDirect3DQuery9_GetData(query, &signalled, sizeof(signalled), D3DGETDATA_FLUSH);
            ++polls;
            if (!(polls & 0xfff) && GetTickCount64() - poll_start > 10000)
            {
                hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                break;
            }
        } while (hr == S_FALSE);

        if (hr != S_OK)
        {
            fprintf(stderr, "GetData failed for generation %u after %u polls, hr %#lx.\n",
                    generation, polls, hr);
            goto failed;
        }
        if (!signalled)
        {
            fprintf(stderr, "Query generation %u completed without signalling.\n", generation);
            goto failed;
        }

        if (generation >= warmup_generations)
        {
            total_polls += polls;
            if (polls < min_polls)
                min_polls = polls;
            if (polls > max_polls)
                max_polls = polls;
        }
    }

    QueryPerformanceCounter(&end);
    GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel_end, &user_end);
    cpu_100ns = filetime_to_100ns(&kernel_end) - filetime_to_100ns(&kernel_start)
            + filetime_to_100ns(&user_end) - filetime_to_100ns(&user_start);

    printf("d3d9_query_poll_benchmark status=ok generations=%u clears=%u total_polls=%" PRIu64
            " min_polls=%u max_polls=%u wall_ms=%.3f cpu_ms=%.3f\n",
            generations, clears, total_polls, min_polls, max_polls,
            (double)(end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart,
            (double)cpu_100ns / 10000.0);

    IDirect3DQuery9_Release(query);
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d9);
    DestroyWindow(window);
    return 0;

failed:
    IDirect3DQuery9_Release(query);
    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d9);
    DestroyWindow(window);
    return 1;
}
