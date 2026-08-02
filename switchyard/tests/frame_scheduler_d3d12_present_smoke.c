#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <stdio.h>

#define TEST_WIDTH 640
#define TEST_HEIGHT 480
#define TEST_FRAMES 240
#define WAKE_TIMEOUT_MS 5000

struct frame_state
{
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *back_buffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    UINT64 in_flight_fence_value;
};

static int wait_for_fence_value( ID3D12Fence *fence, HANDLE event,
                                 UINT64 value )
{
    if (!fence || !event) return 0;
    if (ID3D12Fence_GetCompletedValue( fence ) >= value) return 1;
    if (FAILED( ID3D12Fence_SetEventOnCompletion( fence, value, event ))) return 0;
    if (WaitForSingleObject( event, WAKE_TIMEOUT_MS ) != WAIT_OBJECT_0)
        return 0;
    return ID3D12Fence_GetCompletedValue( fence ) >= value;
}

static int recreate_render_targets( ID3D12Device *device, IDXGISwapChain *swapchain,
                                   ID3D12DescriptorHeap *rtv_heap,
                                   struct frame_state *frames,
                                   unsigned int buffer_count )
{
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle;
    SIZE_T increment;
    unsigned int i;

    increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV );
    if (!increment)
    {
        fprintf( stderr, "Descriptor increment size was zero\n" );
        return 0;
    }

    rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart( rtv_heap );
    for (i = 0; i < buffer_count; ++i)
    {
        if (frames[i].back_buffer)
        {
            ID3D12Resource_Release( frames[i].back_buffer );
            frames[i].back_buffer = NULL;
            frames[i].rtv.ptr = 0;
        }
        if (FAILED( IDXGISwapChain_GetBuffer( swapchain, i, &IID_ID3D12Resource,
                                             (void **)&frames[i].back_buffer ) ))
        {
            fprintf( stderr, "IDXGISwapChain_GetBuffer(%u) failed\n", i );
            return 0;
        }
        frames[i].rtv = descriptor_handle;
        ID3D12Device_CreateRenderTargetView( device, frames[i].back_buffer, &rtv_desc,
                                            frames[i].rtv );
        descriptor_handle.ptr += increment;
    }

    return 1;
}

static int record_and_submit_frame( struct frame_state *frame,
                                   ID3D12CommandQueue *queue,
                                   UINT64 frame_value,
                                   ID3D12Fence *fence,
                                   HANDLE fence_event,
                                   IDXGISwapChain *swapchain,
                                   unsigned int frame_index )
{
    ID3D12Resource *back_buffer = frame->back_buffer;
    ID3D12GraphicsCommandList *list = frame->list;
    ID3D12CommandAllocator *allocator = frame->allocator;
    D3D12_RESOURCE_BARRIER barriers[2];
    const float clear_color[4] = { 0.03f, 0.20f, 0.35f, 1.0f };
    float color[4];
    ID3D12CommandList *command_lists[1];
    HRESULT hr;
    const size_t phase = frame_value & 3u;

    if (!back_buffer || !list || !allocator)
    {
        fprintf( stderr, "Frame %u missing command state\n", frame_index );
        return 0;
    }

    color[0] = clear_color[0] + ((float)phase * 0.05f);
    color[1] = clear_color[1] + ((float)(frame_index & 3u) * 0.08f);
    color[2] = clear_color[2];
    color[3] = 1.0f;

    if (frame->in_flight_fence_value)
    {
        if (!wait_for_fence_value( fence, fence_event, frame->in_flight_fence_value ))
        {
            fprintf( stderr, "Frame %u timed out waiting on in-flight fence\n", frame_index );
            return 0;
        }
    }

    hr = ID3D12CommandAllocator_Reset( allocator );
    if (FAILED(hr))
    {
        fprintf( stderr, "ID3D12CommandAllocator_Reset failed: %#lx\n", hr );
        return 0;
    }

    hr = ID3D12GraphicsCommandList_Reset( list, allocator, NULL );
    if (FAILED(hr))
    {
        fprintf( stderr, "ID3D12GraphicsCommandList_Reset failed: %#lx\n", hr );
        return 0;
    }

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[0].Transition.pResource = back_buffer;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[1].Transition.pResource = back_buffer;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    ID3D12GraphicsCommandList_ResourceBarrier( list, 1, &barriers[0] );
    ID3D12GraphicsCommandList_OMSetRenderTargets( list, 1, &frame->rtv, FALSE, NULL );
    ID3D12GraphicsCommandList_ClearRenderTargetView( list, frame->rtv, color, 0, NULL );
    ID3D12GraphicsCommandList_ResourceBarrier( list, 1, &barriers[1] );

    hr = ID3D12GraphicsCommandList_Close( list );
    if (FAILED(hr))
    {
        fprintf( stderr, "ID3D12GraphicsCommandList_Close failed: %#lx\n", hr );
        return 0;
    }

    command_lists[0] = (ID3D12CommandList *)list;
    ID3D12CommandQueue_ExecuteCommandLists( queue, 1, command_lists );

    /* Track submitted GPU work before Present.  A device-loss or presentation
     * error must not let cleanup release a back buffer still used by the
     * command queue. */
    hr = ID3D12CommandQueue_Signal( queue, fence, frame_value );
    if (FAILED(hr))
    {
        fprintf( stderr, "ID3D12CommandQueue_Signal failed: %#lx\n", hr );
        return 0;
    }
    frame->in_flight_fence_value = frame_value;

    hr = IDXGISwapChain_Present( swapchain, 1, 0 );
    if (FAILED(hr))
    {
        fprintf( stderr, "IDXGISwapChain_Present failed on frame %u: %#lx\n", frame_index, hr );
        return 0;
    }

    return 1;
}

static void close_swapchain_resources( struct frame_state *frames, unsigned int buffer_count )
{
    unsigned int i;

    for (i = 0; i < buffer_count; ++i)
    {
        if (frames[i].back_buffer)
        {
            ID3D12Resource_Release( frames[i].back_buffer );
            frames[i].back_buffer = NULL;
            frames[i].rtv.ptr = 0;
        }
    }
}

static int drain_frame_fences( struct frame_state *frames, unsigned int buffer_count,
                               ID3D12Fence *fence, HANDLE fence_event )
{
    unsigned int i;

    for (i = 0; i < buffer_count; ++i)
    {
        if (!frames[i].in_flight_fence_value) continue;
        if (!wait_for_fence_value( fence, fence_event,
                                   frames[i].in_flight_fence_value ))
            return 0;
        frames[i].in_flight_fence_value = 0;
    }
    return 1;
}

static int try_resize_and_continue( IDXGISwapChain *swapchain, struct frame_state *frames,
                                   unsigned int buffer_count, ID3D12Device *device,
                                   ID3D12DescriptorHeap *rtv_heap, HANDLE fence_event,
                                   ID3D12Fence *fence )
{
    if (!drain_frame_fences( frames, buffer_count, fence, fence_event )) return 0;

    close_swapchain_resources( frames, buffer_count );
    if (FAILED( IDXGISwapChain_ResizeBuffers( swapchain, 0, TEST_WIDTH + 64,
                                            TEST_HEIGHT + 48, DXGI_FORMAT_UNKNOWN,
                                            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT ) ))
    {
        fprintf( stderr, "IDXGISwapChain_ResizeBuffers failed\n" );
        return 0;
    }

    if (!recreate_render_targets( device, swapchain, rtv_heap, frames, buffer_count ))
        return 0;

    return 1;
}

int main(void)
{
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    DXGI_SWAP_CHAIN_DESC1 desc = {0};
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    IDXGIFactory2 *factory = NULL;
    IDXGISwapChain1 *swapchain1 = NULL;
    IDXGISwapChain2 *swapchain2 = NULL;
    IDXGISwapChain3 *swapchain3 = NULL;
    IDXGISwapChain *swapchain = NULL;
    HWND window = NULL;
    ID3D12Device *device = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12Fence *fence = NULL;
    ID3D12DescriptorHeap *rtv_heap = NULL;
    struct frame_state frames[2] = {0};
    HANDLE fence_event = NULL;
    HANDLE latency_handle = NULL;
    HRESULT hr;
    MSG message;
    static const WCHAR class_name[] = L"SwitchyardD3D12SwapchainSmoke";
    unsigned int i;
    UINT64 next_fence_value = 0;
    int result = 1;

    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    if (!RegisterClassW( &(WNDCLASSW){
            .lpfnWndProc = DefWindowProcW,
            .hInstance = GetModuleHandleW(NULL),
            .lpszClassName = class_name,
        } ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        fprintf( stderr, "RegisterClassW failed: %lu\n", GetLastError() );
        return 1;
    }

    window = CreateWindowW( class_name, L"Switchyard D3D12 present smoke",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           TEST_WIDTH, TEST_HEIGHT, NULL, NULL,
                           GetModuleHandleW(NULL), NULL );
    if (!window)
    {
        fprintf( stderr, "CreateWindowW failed: %lu\n", GetLastError() );
        goto done;
    }
    ShowWindow( window, SW_SHOW );
    UpdateWindow( window );

    hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                            (void **)&device );
    if (FAILED(hr))
    {
        fprintf( stderr, "D3D12CreateDevice failed: %#lx\n", hr );
        goto done;
    }

    hr = CreateDXGIFactory1( &IID_IDXGIFactory2, (void **)&factory );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateDXGIFactory1 failed: %#lx\n", hr );
        goto done;
    }

    hr = ID3D12Device_CreateCommandQueue( device, &queue_desc,
                                         &IID_ID3D12CommandQueue, (void **)&queue );
    if (FAILED(hr))
    {
        fprintf( stderr, "ID3D12Device_CreateCommandQueue failed: %#lx\n", hr );
        goto done;
    }

    desc.Width = TEST_WIDTH;
    desc.Height = TEST_HEIGHT;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    hr = IDXGIFactory2_CreateSwapChainForHwnd( factory, (IUnknown *)queue, window,
                                              &desc, NULL, NULL, &swapchain1 );
    if (FAILED(hr))
    {
        fprintf( stderr, "IDXGIFactory2_CreateSwapChainForHwnd failed: %#lx\n", hr );
        goto done;
    }
    swapchain = (IDXGISwapChain *)swapchain1;

    hr = IDXGISwapChain_QueryInterface( swapchain, &IID_IDXGISwapChain2, (void **)&swapchain2 );
    if (FAILED(hr))
    {
        fprintf( stderr, "IDXGISwapChain2 unavailable: %#lx\n", hr );
        goto done;
    }
    hr = IDXGISwapChain2_SetMaximumFrameLatency( swapchain2, 1 );
    if (FAILED(hr))
    {
        fprintf( stderr, "SetMaximumFrameLatency(1) failed: %#lx\n", hr );
        goto done;
    }
    latency_handle = IDXGISwapChain2_GetFrameLatencyWaitableObject( swapchain2 );
    if (!latency_handle)
    {
        fprintf( stderr, "GetFrameLatencyWaitableObject failed: %lu\n", GetLastError() );
        goto done;
    }

    hr = IDXGISwapChain_QueryInterface( swapchain, &IID_IDXGISwapChain3, (void **)&swapchain3 );
    if (FAILED(hr))
    {
        fprintf( stderr, "IDXGISwapChain3 unavailable: %#lx\n", hr );
        goto done;
    }

    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 2;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap( device, &rtv_heap_desc,
                                           &IID_ID3D12DescriptorHeap,
                                           (void **)&rtv_heap );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateDescriptorHeap(RTV) failed: %#lx\n", hr );
        goto done;
    }

    for (i = 0; i < 2; ++i)
    {
        hr = ID3D12Device_CreateCommandAllocator( device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 &IID_ID3D12CommandAllocator,
                                                 (void **)&frames[i].allocator );
        if (FAILED(hr))
        {
            fprintf( stderr, "CreateCommandAllocator(%u) failed: %#lx\n", i, hr );
            goto done;
        }

        hr = ID3D12Device_CreateCommandList( device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            frames[i].allocator, NULL,
                                            &IID_ID3D12GraphicsCommandList,
                                            (void **)&frames[i].list );
        if (FAILED(hr))
        {
            fprintf( stderr, "CreateCommandList(%u) failed: %#lx\n", i, hr );
            goto done;
        }
        hr = ID3D12GraphicsCommandList_Close( frames[i].list );
        if (FAILED(hr))
        {
            fprintf( stderr, "Initial CommandList::Close(%u) failed: %#lx\n", i, hr );
            goto done;
        }
    }

    if (!recreate_render_targets( device, swapchain, rtv_heap, frames, 2 ))
        goto done;

    hr = ID3D12Device_CreateFence( device, 0, D3D12_FENCE_FLAG_NONE,
                                   &IID_ID3D12Fence, (void **)&fence );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateFence failed: %#lx\n", hr );
        goto done;
    }

    fence_event = CreateEventW( NULL, FALSE, FALSE, NULL );
    if (!fence_event)
    {
        fprintf( stderr, "CreateEventW failed: %lu\n", GetLastError() );
        goto done;
    }

    for (i = 0; i < TEST_FRAMES; ++i)
    {
        DWORD wait_result;
        unsigned int frame_index;
        ++next_fence_value;

        if (i == 80) ShowWindow( window, SW_MINIMIZE );
        if (i == 90)
            ShowWindow( window, SW_RESTORE );
        if (i == 120)
        {
            SetWindowPos( window, NULL, 0, 0, TEST_WIDTH + 64, TEST_HEIGHT + 48,
                          SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER );
            if (!try_resize_and_continue( swapchain, frames, 2, device, rtv_heap,
                                          fence_event, fence ))
            {
                fprintf( stderr, "Resize path failed at frame %u\n", i );
                goto done;
            }
        }

        wait_result = WaitForSingleObject( latency_handle, WAKE_TIMEOUT_MS );
        if (wait_result != WAIT_OBJECT_0)
        {
            fprintf( stderr, "Frame latency wait failed at frame %u: %#lx\n", i, wait_result );
            goto done;
        }

        frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex( swapchain3 );
        if (frame_index > 1)
        {
            fprintf( stderr, "Current back buffer index invalid: %u\n", frame_index );
            goto done;
        }

        if (!record_and_submit_frame( &frames[frame_index], queue, next_fence_value,
                                     fence, fence_event, swapchain,
                                     frame_index ))
        {
            goto done;
        }

        while (PeekMessageW( &message, NULL, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &message );
            DispatchMessageW( &message );
        }
        if (i == 80 || i == 90)
        {
            Sleep( 100 );
        }
    }

    if (!drain_frame_fences( frames, 2, fence, fence_event ))
    {
        fprintf( stderr, "Final fence drain timed out\n" );
        goto done;
    }

    printf( "D3D12 swapchain present smoke passed for %u frames (1.0 frame latency).\n",
            TEST_FRAMES );
    result = 0;

done:
    if (fence && fence_event && !drain_frame_fences( frames, 2, fence, fence_event ))
        fprintf( stderr, "Cleanup fence drain timed out\n" );
    close_swapchain_resources( frames, 2 );

    if (latency_handle) CloseHandle( latency_handle );
    if (fence_event) CloseHandle( fence_event );
    if (swapchain3) IDXGISwapChain3_Release( swapchain3 );
    if (swapchain2) IDXGISwapChain2_Release( swapchain2 );
    if (swapchain) IDXGISwapChain_Release( swapchain );
    if (rtv_heap) ID3D12DescriptorHeap_Release( rtv_heap );

    for (i = 0; i < 2; ++i)
    {
        if (frames[i].list) ID3D12GraphicsCommandList_Release( frames[i].list );
        if (frames[i].allocator) ID3D12CommandAllocator_Release( frames[i].allocator );
    }
    if (fence) ID3D12Fence_Release( fence );
    if (queue) ID3D12CommandQueue_Release( queue );
    if (device) ID3D12Device_Release( device );
    if (factory) IDXGIFactory2_Release( factory );
    if (window) DestroyWindow( window );
    UnregisterClassW( class_name, GetModuleHandleW( NULL ) );
    return result;
}
