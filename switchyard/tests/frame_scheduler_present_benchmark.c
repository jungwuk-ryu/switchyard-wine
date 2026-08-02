/*
 * DXGI present scheduling benchmark.
 *
 * This is deliberately a black-box Windows client.  It measures the public
 * frame-latency waitable-object contract and never calls a winemac-private
 * entry point, so the same executable can compare a baseline and candidate
 * runtime.
 */

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WARMUP_FRAMES 120
#define SAMPLE_FRAMES 1200

static LRESULT CALLBACK benchmark_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE)
    {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;

    return a > b ? 1 : a < b ? -1 : 0;
}

static uint64_t percentile(uint64_t *values, unsigned int count, unsigned int numerator)
{
    unsigned int index;

    qsort(values, count, sizeof(*values), compare_u64);
    index = (count * numerator + 99) / 100;
    if (!index) index = 1;
    return values[index - 1];
}

static uint64_t ticks_to_us(uint64_t ticks, uint64_t frequency)
{
    return (uint64_t)(((__uint128_t)ticks * 1000000u + frequency / 2) / frequency);
}

static uint64_t filetime_to_100ns(FILETIME value)
{
    ULARGE_INTEGER integer;

    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

static uint64_t process_cpu_100ns(void)
{
    FILETIME creation, exit, kernel, user;

    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return UINT64_MAX;
    return filetime_to_100ns(kernel) + filetime_to_100ns(user);
}

static void pump_messages(void)
{
    MSG message;

    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

int main(void)
{
    static const WCHAR class_name[] = L"SwitchyardFrameSchedulerBenchmark";
    uint64_t intervals[SAMPLE_FRAMES], completion[SAMPLE_FRAMES];
    uint64_t display_latency_proxy[SAMPLE_FRAMES], present_call[SAMPLE_FRAMES];
    D3D_FEATURE_LEVEL feature_level;
    ID3D11RenderTargetView *render_target = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain2 *swapchain2 = NULL;
    ID3D11Texture2D *back_buffer = NULL;
    IDXGISwapChain *swapchain = NULL;
    ID3D11Device *device = NULL;
    LARGE_INTEGER frequency, now;
    uint64_t previous_present_start = 0, previous_present_end = 0, previous_signal = 0;
    uint64_t active_cpu_start, active_cpu_end, idle_cpu_start, idle_cpu_end;
    uint64_t idle_wall_start, idle_wall_end;
    DXGI_SWAP_CHAIN_DESC desc;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    WNDCLASSW window_class;
    HANDLE latency_handle = NULL;
    HWND window = NULL;
    HRESULT hr;
    unsigned int i, sample = 0;
    int result = 1;

    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = benchmark_window_proc;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = class_name;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        fprintf(stderr, "RegisterClassW failed: %lu\n", GetLastError());
        return 1;
    }

    window = CreateWindowW(class_name, L"Switchyard present benchmark",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           640, 480, NULL, NULL, window_class.hInstance, NULL);
    if (!window)
    {
        fprintf(stderr, "CreateWindowW failed: %lu\n", GetLastError());
        goto done;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    memset(&desc, 0, sizeof(desc));
    desc.BufferDesc.Width = 640;
    desc.BufferDesc.Height = 480;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &desc, &swapchain, &device, &feature_level, &context);
    if (FAILED(hr))
    {
        fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed: %#lx\n", hr);
        goto done;
    }
    hr = IDXGISwapChain_QueryInterface(swapchain, &IID_IDXGISwapChain2, (void **)&swapchain2);
    if (FAILED(hr))
    {
        fprintf(stderr, "IDXGISwapChain2 is unavailable: %#lx\n", hr);
        goto done;
    }
    hr = IDXGISwapChain2_SetMaximumFrameLatency(swapchain2, 1);
    if (FAILED(hr))
    {
        fprintf(stderr, "SetMaximumFrameLatency failed: %#lx\n", hr);
        goto done;
    }
    if (!(latency_handle = IDXGISwapChain2_GetFrameLatencyWaitableObject(swapchain2)))
    {
        fprintf(stderr, "GetFrameLatencyWaitableObject failed: %lu\n", GetLastError());
        goto done;
    }
    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&back_buffer);
    if (FAILED(hr) || FAILED(hr = ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)back_buffer, NULL, &render_target)))
    {
        fprintf(stderr, "Creating the render target failed: %#lx\n", hr);
        goto done;
    }
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        fprintf(stderr, "QueryPerformanceFrequency failed.\n");
        goto done;
    }

    active_cpu_start = process_cpu_100ns();
    for (i = 0; i < WARMUP_FRAMES + SAMPLE_FRAMES; ++i)
    {
        const float color[] = {(i & 1) ? 0.1f : 0.7f, (i & 2) ? 0.6f : 0.2f,
                               (i & 4) ? 0.8f : 0.3f, 1.0f};
        uint64_t signal_time, present_start, present_end;
        DWORD wait_result;

        wait_result = WaitForSingleObject(latency_handle, 5000);
        QueryPerformanceCounter(&now);
        signal_time = now.QuadPart;
        if (wait_result != WAIT_OBJECT_0)
        {
            fprintf(stderr, "Frame latency wait failed at frame %u: %#lx (%lu)\n",
                    i, wait_result, GetLastError());
            goto done;
        }

        ID3D11DeviceContext_ClearRenderTargetView(context, render_target, color);
        QueryPerformanceCounter(&now);
        present_start = now.QuadPart;
        hr = IDXGISwapChain_Present(swapchain, 1, 0);
        QueryPerformanceCounter(&now);
        present_end = now.QuadPart;
        if (FAILED(hr))
        {
            fprintf(stderr, "Present failed at frame %u: %#lx\n", i, hr);
            goto done;
        }

        if (i >= WARMUP_FRAMES)
        {
            intervals[sample] = previous_signal ? signal_time - previous_signal : 0;
            completion[sample] = previous_present_end ? signal_time - previous_present_end : 0;
            display_latency_proxy[sample] = previous_present_start ?
                    signal_time - previous_present_start : 0;
            present_call[sample] = present_end - present_start;
            ++sample;
        }
        previous_signal = signal_time;
        previous_present_start = present_start;
        previous_present_end = present_end;
        pump_messages();
    }
    active_cpu_end = process_cpu_100ns();

    /* The first measured interval has no measured predecessor. */
    memmove(intervals, intervals + 1, (SAMPLE_FRAMES - 1) * sizeof(*intervals));
    memmove(completion, completion + 1, (SAMPLE_FRAMES - 1) * sizeof(*completion));
    memmove(display_latency_proxy, display_latency_proxy + 1,
            (SAMPLE_FRAMES - 1) * sizeof(*display_latency_proxy));
    --sample;

    /* Wine's Cocoa redraw link stops after two seconds with no dirty view.
     * Exclude that transition from the steady idle sample. */
    Sleep(2500);
    idle_cpu_start = process_cpu_100ns();
    QueryPerformanceCounter(&now);
    idle_wall_start = now.QuadPart;
    Sleep(3000);
    QueryPerformanceCounter(&now);
    idle_wall_end = now.QuadPart;
    idle_cpu_end = process_cpu_100ns();

    printf("scheduler_metrics backend=d3d11 frames=%u qpc=%" PRIu64
           " interval_p50_us=%" PRIu64 " interval_p95_us=%" PRIu64
           " interval_p99_us=%" PRIu64 " completion_proxy_p50_us=%" PRIu64
           " completion_proxy_p95_us=%" PRIu64 " completion_proxy_p99_us=%" PRIu64
           " display_latency_proxy_p50_us=%" PRIu64
           " display_latency_proxy_p95_us=%" PRIu64
           " display_latency_proxy_p99_us=%" PRIu64
           " present_call_p50_us=%" PRIu64 " present_call_p95_us=%" PRIu64
           " present_call_p99_us=%" PRIu64 " active_cpu_ms=%" PRIu64
           " idle_wall_ms=%" PRIu64 " idle_cpu_ms=%" PRIu64 "\n",
           sample, (uint64_t)frequency.QuadPart,
           ticks_to_us(percentile(intervals, sample, 50), frequency.QuadPart),
           ticks_to_us(percentile(intervals, sample, 95), frequency.QuadPart),
           ticks_to_us(percentile(intervals, sample, 99), frequency.QuadPart),
           ticks_to_us(percentile(completion, sample, 50), frequency.QuadPart),
           ticks_to_us(percentile(completion, sample, 95), frequency.QuadPart),
           ticks_to_us(percentile(completion, sample, 99), frequency.QuadPart),
           ticks_to_us(percentile(display_latency_proxy, sample, 50), frequency.QuadPart),
           ticks_to_us(percentile(display_latency_proxy, sample, 95), frequency.QuadPart),
           ticks_to_us(percentile(display_latency_proxy, sample, 99), frequency.QuadPart),
           ticks_to_us(percentile(present_call, sample, 50), frequency.QuadPart),
           ticks_to_us(percentile(present_call, sample, 95), frequency.QuadPart),
           ticks_to_us(percentile(present_call, sample, 99), frequency.QuadPart),
           active_cpu_start == UINT64_MAX || active_cpu_end == UINT64_MAX ? UINT64_MAX :
                   (active_cpu_end - active_cpu_start) / 10000,
           ticks_to_us(idle_wall_end - idle_wall_start, frequency.QuadPart) / 1000,
           idle_cpu_start == UINT64_MAX || idle_cpu_end == UINT64_MAX ? UINT64_MAX :
                   (idle_cpu_end - idle_cpu_start) / 10000);
    result = 0;

done:
    if (latency_handle) CloseHandle(latency_handle);
    if (render_target) ID3D11RenderTargetView_Release(render_target);
    if (back_buffer) ID3D11Texture2D_Release(back_buffer);
    if (swapchain2) IDXGISwapChain2_Release(swapchain2);
    if (swapchain) IDXGISwapChain_Release(swapchain);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    if (window) DestroyWindow(window);
    UnregisterClassW(class_name, window_class.hInstance);
    return result;
}
