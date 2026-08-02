/*
 * Copyright 2017 Józef Kucia for CodeWeavers
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define COBJMACROS
#include "initguid.h"
#include "d3d12.h"
#include "d3d12sdklayers.h"
#include "dxgi1_6.h"
#include "dxcore.h"
#include "wine/test.h"

static HRESULT (WINAPI *pDXCoreCreateAdapterFactory)(REFIID riid, void **out);

static BOOL compare_uint(unsigned int x, unsigned int y, unsigned int max_diff)
{
    unsigned int diff = x > y ? x - y : y - x;

    return diff <= max_diff;
}

static BOOL compare_color(DWORD c1, DWORD c2, unsigned int max_diff)
{
    return compare_uint(c1 & 0xff, c2 & 0xff, max_diff)
            && compare_uint((c1 >> 8) & 0xff, (c2 >> 8) & 0xff, max_diff)
            && compare_uint((c1 >> 16) & 0xff, (c2 >> 16) & 0xff, max_diff)
            && compare_uint((c1 >> 24) & 0xff, (c2 >> 24) & 0xff, max_diff);
}

static BOOL equal_luid(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static unsigned int format_size(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return 4;
        default:
            trace("Unhandled format %#x.\n", format);
            return 1;
    }
}

static size_t align(size_t addr, size_t alignment)
{
    return (addr + (alignment - 1)) & ~(alignment - 1);
}

static void set_viewport(D3D12_VIEWPORT *vp, float x, float y,
        float width, float height, float min_depth, float max_depth)
{
    vp->TopLeftX = x;
    vp->TopLeftY = y;
    vp->Width = width;
    vp->Height = height;
    vp->MinDepth = min_depth;
    vp->MaxDepth = max_depth;
}

static BOOL use_warp_adapter;
static unsigned int use_adapter_idx;

static IDXGIAdapter *create_adapter(void)
{
    IDXGIFactory4 *factory;
    IDXGIAdapter *adapter;
    HRESULT hr;

    if (!use_warp_adapter && !use_adapter_idx)
        return NULL;

    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    adapter = NULL;
    if (use_warp_adapter)
    {
        hr = IDXGIFactory4_EnumWarpAdapter(factory, &IID_IDXGIAdapter, (void **)&adapter);
    }
    else
    {
        hr = IDXGIFactory4_EnumAdapters(factory, use_adapter_idx, &adapter);
    }
    IDXGIFactory4_Release(factory);
    if (FAILED(hr))
        trace("Failed to get adapter, hr %#lx.\n", hr);
    return adapter;
}

static ID3D12Device *create_device(void)
{
    IDXGIAdapter *adapter;
    ID3D12Device *device;
    HRESULT hr;

    adapter = create_adapter();
    hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (adapter)
        IDXGIAdapter_Release(adapter);
    if (FAILED(hr))
        return NULL;

    return device;
}

static void print_adapter_info(void)
{
    DXGI_ADAPTER_DESC adapter_desc;
    IDXGIFactory4 *factory;
    IDXGIAdapter *adapter;
    ID3D12Device *device;
    HRESULT hr;
    LUID luid;

    if (!(device = create_device()))
        return;
    luid = ID3D12Device_GetAdapterLuid(device);
    ID3D12Device_Release(device);

    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    hr = IDXGIFactory4_EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter, (void **)&adapter);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDXGIFactory4_Release(factory);

    if (FAILED(hr))
        return;

    hr = IDXGIAdapter_GetDesc(adapter, &adapter_desc);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDXGIAdapter_Release(adapter);

    trace("Adapter: %s, %04x:%04x.\n", wine_dbgstr_w(adapter_desc.Description),
            adapter_desc.VendorId, adapter_desc.DeviceId);
}

static ULONG get_refcount(void *iface)
{
    IUnknown *unknown = iface;
    IUnknown_AddRef(unknown);
    return IUnknown_Release(unknown);
}

#define check_interface(a, b, c) check_interface_(__LINE__, a, b, c)
static void check_interface_(unsigned int line, void *iface_ptr, REFIID iid, BOOL supported)
{
    IUnknown *iface = iface_ptr;
    HRESULT hr, expected_hr;
    IUnknown *unk;

    expected_hr = supported ? S_OK : E_NOINTERFACE;

    hr = IUnknown_QueryInterface(iface, iid, (void **)&unk);
    ok_(__FILE__, line)(hr == expected_hr, "Got unexpected hr %#lx, expected %#lx.\n", hr, expected_hr);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
}

static HRESULT create_root_signature(ID3D12Device *device, const D3D12_ROOT_SIGNATURE_DESC *desc,
        ID3D12RootSignature **root_signature)
{
    ID3DBlob *blob;
    HRESULT hr;

    if (FAILED(hr = D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, NULL)))
        return hr;

    hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)root_signature);
    ID3D10Blob_Release(blob);
    return hr;
}

static ID3D12RootSignature *create_default_root_signature(ID3D12Device *device)
{
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    ID3D12RootSignature *root_signature = NULL;
    D3D12_ROOT_PARAMETER root_parameters[1];
    HRESULT hr;

    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = 4;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_signature_desc.NumParameters = ARRAY_SIZE(root_parameters);
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 0;
    root_signature_desc.pStaticSamplers = NULL;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    return root_signature;
}

#define create_pipeline_state(a, b, c, d) create_pipeline_state_(__LINE__, a, b, c, d)
static ID3D12PipelineState *create_pipeline_state_(unsigned int line, ID3D12Device *device,
        ID3D12RootSignature *root_signature, DXGI_FORMAT rt_format, const D3D12_SHADER_BYTECODE *ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_state_desc;
    ID3D12PipelineState *pipeline_state;
    HRESULT hr;

    static const DWORD vs_code[] =
    {
#if 0
        void main(uint id : SV_VertexID, out float4 position : SV_Position)
        {
            float2 coords = float2((id << 1) & 2, id & 2);
            position = float4(coords * float2(2, -2) + float2(-1, 1), 0, 1);
        }
#endif
        0x43425844, 0xf900d25e, 0x68bfefa7, 0xa63ac0a7, 0xa476af7a, 0x00000001, 0x0000018c, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000006, 0x00000001, 0x00000000, 0x00000101, 0x565f5653, 0x65747265, 0x00444978,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x505f5653, 0x7469736f, 0x006e6f69, 0x58454853, 0x000000f0, 0x00010050,
        0x0000003c, 0x0100086a, 0x04000060, 0x00101012, 0x00000000, 0x00000006, 0x04000067, 0x001020f2,
        0x00000000, 0x00000001, 0x02000068, 0x00000001, 0x0b00008c, 0x00100012, 0x00000000, 0x00004001,
        0x00000001, 0x00004001, 0x00000001, 0x0010100a, 0x00000000, 0x00004001, 0x00000000, 0x07000001,
        0x00100042, 0x00000000, 0x0010100a, 0x00000000, 0x00004001, 0x00000002, 0x05000056, 0x00100032,
        0x00000000, 0x00100086, 0x00000000, 0x0f000032, 0x00102032, 0x00000000, 0x00100046, 0x00000000,
        0x00004002, 0x40000000, 0xc0000000, 0x00000000, 0x00000000, 0x00004002, 0xbf800000, 0x3f800000,
        0x00000000, 0x00000000, 0x08000036, 0x001020c2, 0x00000000, 0x00004002, 0x00000000, 0x00000000,
        0x00000000, 0x3f800000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE vs = {vs_code, sizeof(vs_code)};
    static const DWORD ps_code[] =
    {
#if 0
        void main(const in float4 position : SV_Position, out float4 target : SV_Target0)
        {
            target = float4(0.0f, 1.0f, 0.0f, 1.0f);
        }
#endif
        0x43425844, 0x8a4a8140, 0x5eba8e0b, 0x714e0791, 0xb4b8eed2, 0x00000001, 0x000000d8, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x505f5653, 0x7469736f, 0x006e6f69,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
        0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000003c, 0x00000050,
        0x0000000f, 0x0100086a, 0x03000065, 0x001020f2, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
        0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE default_ps = {ps_code, sizeof(ps_code)};

    if (!ps)
        ps = &default_ps;

    memset(&pipeline_state_desc, 0, sizeof(pipeline_state_desc));
    pipeline_state_desc.pRootSignature = root_signature;
    pipeline_state_desc.VS = vs;
    pipeline_state_desc.PS = *ps;
    pipeline_state_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline_state_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline_state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pipeline_state_desc.SampleMask = ~(UINT)0;
    pipeline_state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline_state_desc.NumRenderTargets = 1;
    pipeline_state_desc.RTVFormats[0] = rt_format;
    pipeline_state_desc.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(device, &pipeline_state_desc,
            &IID_ID3D12PipelineState, (void **)&pipeline_state);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create graphics pipeline state, hr %#lx.\n", hr);

    return pipeline_state;
}

#define create_command_queue(a, b) create_command_queue_(__LINE__, a, b)
static ID3D12CommandQueue *create_command_queue_(unsigned int line,
        ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type)
{
    D3D12_COMMAND_QUEUE_DESC command_queue_desc;
    ID3D12CommandQueue *queue;
    HRESULT hr;

    command_queue_desc.Type = type;
    command_queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    command_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    command_queue_desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &command_queue_desc,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create command queue, hr %#lx.\n", hr);

    return queue;
}

struct test_context_desc
{
    BOOL no_render_target;
    BOOL no_pipeline;
    const D3D12_SHADER_BYTECODE *ps;
};

#define MAX_FRAME_COUNT 4

struct test_context
{
    ID3D12Device *device;

    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *allocator[MAX_FRAME_COUNT];
    ID3D12GraphicsCommandList *list[MAX_FRAME_COUNT];

    ID3D12DescriptorHeap *rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[MAX_FRAME_COUNT];
    ID3D12Resource *render_target[MAX_FRAME_COUNT];

    ID3D12RootSignature *root_signature;
    ID3D12PipelineState *pipeline_state;

    D3D12_VIEWPORT viewport;
    RECT scissor_rect;
};

#define reset_command_list(a, b) reset_command_list_(__LINE__, a, b)
static void reset_command_list_(unsigned int line, struct test_context *context, unsigned int index)
{
    HRESULT hr;

    assert(index < MAX_FRAME_COUNT);

    hr = ID3D12CommandAllocator_Reset(context->allocator[index]);
    ok_(__FILE__, line)(hr == S_OK, "Failed to reset command allocator, hr %#lx.\n", hr);
    hr = ID3D12GraphicsCommandList_Reset(context->list[index], context->allocator[index], NULL);
    ok_(__FILE__, line)(hr == S_OK, "Failed to reset command list, hr %#lx.\n", hr);
}

static void destroy_render_targets(struct test_context *context)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(context->render_target); ++i)
    {
        if (context->render_target[i])
        {
            ID3D12Resource_Release(context->render_target[i]);
            context->render_target[i] = NULL;
        }
    }
}

#define create_render_target(context) create_render_target_(__LINE__, context)
static void create_render_target_(unsigned int line, struct test_context *context)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    D3D12_CLEAR_VALUE clear_value;
    HRESULT hr;

    destroy_render_targets(context);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear_value.Format = resource_desc.Format;
    clear_value.Color[0] = 1.0f;
    clear_value.Color[1] = 1.0f;
    clear_value.Color[2] = 1.0f;
    clear_value.Color[3] = 1.0f;
    hr = ID3D12Device_CreateCommittedResource(context->device,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            &IID_ID3D12Resource, (void **)&context->render_target[0]);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create texture, hr %#lx.\n", hr);

    set_viewport(&context->viewport, 0.0f, 0.0f, resource_desc.Width, resource_desc.Height, 0.0f, 1.0f);
    SetRect(&context->scissor_rect, 0, 0, resource_desc.Width, resource_desc.Height);

    ID3D12Device_CreateRenderTargetView(context->device, context->render_target[0], NULL, context->rtv[0]);
}

#define init_test_context(a, b) init_test_context_(__LINE__, a, b)
static BOOL init_test_context_(unsigned int line, struct test_context *context,
        const struct test_context_desc *desc)
{
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
    unsigned int rtv_size;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    memset(context, 0, sizeof(*context));

    if (!(context->device = create_device()))
    {
        skip_(__FILE__, line)("Failed to create device.\n");
        return FALSE;
    }
    device = context->device;

    context->queue = create_command_queue_(line, device, D3D12_COMMAND_LIST_TYPE_DIRECT);

    for (i = 0; i < ARRAY_SIZE(context->allocator); ++i)
    {
        hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&context->allocator[i]);
        ok_(__FILE__, line)(hr == S_OK, "Failed to create command allocator %u, hr %#lx.\n", i, hr);
    }

    for (i = 0; i < ARRAY_SIZE(context->list); ++i)
    {
        hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                context->allocator[i], NULL, &IID_ID3D12GraphicsCommandList, (void **)&context->list[i]);
        ok_(__FILE__, line)(hr == S_OK, "Failed to create command list %u, hr %#lx.\n", i, hr);
    }

    if (desc && desc->no_render_target)
        return TRUE;

    rtv_heap_desc.NumDescriptors = MAX_FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_heap_desc.NodeMask = 0;
    hr = ID3D12Device_CreateDescriptorHeap(device, &rtv_heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&context->rtv_heap);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create descriptor heap, hr %#lx.\n", hr);

    rtv_size = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (i = 0; i < ARRAY_SIZE(context->rtv); ++i)
    {
        context->rtv[i] = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(context->rtv_heap);
        context->rtv[i].ptr += i * rtv_size;
    }

    context->root_signature = create_default_root_signature(device);

    if (desc && desc->no_pipeline)
        return TRUE;

    context->pipeline_state = create_pipeline_state_(line, device,
            context->root_signature, DXGI_FORMAT_B8G8R8A8_UNORM, desc ? desc->ps : NULL);

    return TRUE;
}

#define destroy_test_context(context) destroy_test_context_(__LINE__, context)
static void destroy_test_context_(unsigned int line, struct test_context *context)
{
    unsigned int i;
    ULONG refcount;

    if (context->pipeline_state)
        ID3D12PipelineState_Release(context->pipeline_state);
    if (context->root_signature)
        ID3D12RootSignature_Release(context->root_signature);

    if (context->rtv_heap)
        ID3D12DescriptorHeap_Release(context->rtv_heap);
    destroy_render_targets(context);

    for (i = 0; i < ARRAY_SIZE(context->allocator); ++i)
        ID3D12CommandAllocator_Release(context->allocator[i]);
    ID3D12CommandQueue_Release(context->queue);
    for (i = 0; i < ARRAY_SIZE(context->list); ++i)
        ID3D12GraphicsCommandList_Release(context->list[i]);

    refcount = ID3D12Device_Release(context->device);
    ok_(__FILE__, line)(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
}

static void exec_command_list(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list)
{
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)list};
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
}

static HRESULT wait_for_fence(ID3D12Fence *fence, UINT64 value)
{
    HANDLE event;
    HRESULT hr;
    DWORD ret;

    if (ID3D12Fence_GetCompletedValue(fence) >= value)
        return S_OK;

    if (!(event = CreateEventA(NULL, FALSE, FALSE, NULL)))
        return E_FAIL;

    if (FAILED(hr = ID3D12Fence_SetEventOnCompletion(fence, value, event)))
    {
        CloseHandle(event);
        return hr;
    }

    ret = WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    return ret == WAIT_OBJECT_0 ? S_OK : E_FAIL;
}

#define wait_queue_idle(a, b) wait_queue_idle_(__LINE__, a, b)
static void wait_queue_idle_(unsigned int line, ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12Fence *fence;
    HRESULT hr;

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create fence, hr %#lx.\n", hr);

    hr = ID3D12CommandQueue_Signal(queue, fence, 1);
    ok_(__FILE__, line)(hr == S_OK, "Failed to signal fence, hr %#lx.\n", hr);
    hr = wait_for_fence(fence, 1);
    ok_(__FILE__, line)(hr == S_OK, "Failed to wait for fence, hr %#lx.\n", hr);

    ID3D12Fence_Release(fence);
}

static void transition_sub_resource_state(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
        unsigned int sub_resource_idx, D3D12_RESOURCE_STATES state_before, D3D12_RESOURCE_STATES state_after)
{
    D3D12_RESOURCE_BARRIER barrier;

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = sub_resource_idx;
    barrier.Transition.StateBefore = state_before;
    barrier.Transition.StateAfter = state_after;

    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

#define create_buffer(a, b, c, d, e) create_buffer_(__LINE__, a, b, c, d, e)
static ID3D12Resource *create_buffer_(unsigned int line, ID3D12Device *device,
        D3D12_HEAP_TYPE heap_type, size_t size, D3D12_RESOURCE_FLAGS resource_flags,
        D3D12_RESOURCE_STATES initial_resource_state)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *buffer = NULL;
    HRESULT hr;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = heap_type;

    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = size;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = resource_flags;

    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties,
            D3D12_HEAP_FLAG_NONE, &resource_desc, initial_resource_state,
            NULL, &IID_ID3D12Resource, (void **)&buffer);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create buffer, hr %#lx.\n", hr);
    return buffer;
}

#define create_readback_buffer(a, b) create_readback_buffer_(__LINE__, a, b)
static ID3D12Resource *create_readback_buffer_(unsigned int line, ID3D12Device *device,
        size_t size)
{
    return create_buffer_(line, device, D3D12_HEAP_TYPE_READBACK, size,
            D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
}

static HWND create_window(DWORD style)
{
    return CreateWindowA("static", "d3d12_test", style, 0, 0, 256, 256, NULL, NULL, NULL, NULL);
}

static IDXGISwapChain3 *create_swapchain(struct test_context *context, ID3D12CommandQueue *queue,
        HWND window, unsigned int buffer_count, DXGI_FORMAT format, unsigned int width, unsigned int height)
{
    IDXGISwapChain1 *swapchain1;
    DXGI_SWAP_CHAIN_DESC1 desc;
    IDXGISwapChain3 *swapchain;
    IDXGIFactory4 *factory;
    unsigned int i;
    HRESULT hr;

    assert(buffer_count <= MAX_FRAME_COUNT);

    if (context)
        destroy_render_targets(context);

    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = buffer_count;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = 0;
    hr = IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, window, &desc, NULL, NULL, &swapchain1);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    IDXGIFactory4_Release(factory);

    hr = IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain3, (void **)&swapchain);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDXGISwapChain1_Release(swapchain1);

    if (context)
    {
        set_viewport(&context->viewport, 0.0f, 0.0f, width, height, 0.0f, 1.0f);
        SetRect(&context->scissor_rect, 0, 0, width, height);

        for (i = 0; i < buffer_count; ++i)
        {
            hr = IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource, (void **)&context->render_target[i]);
            ok(hr == S_OK, "Failed to get swapchain buffer %u, hr %#lx.\n", i, hr);
            ID3D12Device_CreateRenderTargetView(context->device, context->render_target[i], NULL, context->rtv[i]);
        }
    }

    return swapchain;
}

struct resource_readback
{
    unsigned int width;
    unsigned int height;
    ID3D12Resource *resource;
    unsigned int row_pitch;
    void *data;
};

static void get_texture_readback_with_command_list(ID3D12Resource *texture, unsigned int sub_resource,
        struct resource_readback *rb, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list)
{
    D3D12_TEXTURE_COPY_LOCATION dst_location, src_location;
    D3D12_RESOURCE_DESC resource_desc;
    D3D12_RANGE read_range;
    unsigned int miplevel;
    ID3D12Device *device;
    DXGI_FORMAT format;
    HRESULT hr;

    hr = ID3D12Resource_GetDevice(texture, &IID_ID3D12Device, (void **)&device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    resource_desc = ID3D12Resource_GetDesc(texture);
    ok(resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER,
            "Resource %p is not texture.\n", texture);
    ok(resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D,
            "Readback not implemented for 3D textures.\n");

    miplevel = sub_resource % resource_desc.MipLevels;
    rb->width = max(1, resource_desc.Width >> miplevel);
    rb->height = max(1, resource_desc.Height >> miplevel);
    rb->row_pitch = align(rb->width * format_size(resource_desc.Format), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    rb->data = NULL;

    format = resource_desc.Format;

    rb->resource = create_readback_buffer(device, rb->row_pitch * rb->height);

    dst_location.pResource = rb->resource;
    dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_location.PlacedFootprint.Offset = 0;
    dst_location.PlacedFootprint.Footprint.Format = format;
    dst_location.PlacedFootprint.Footprint.Width = rb->width;
    dst_location.PlacedFootprint.Footprint.Height = rb->height;
    dst_location.PlacedFootprint.Footprint.Depth = 1;
    dst_location.PlacedFootprint.Footprint.RowPitch = rb->row_pitch;

    src_location.pResource = texture;
    src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_location.SubresourceIndex = sub_resource;

    ID3D12GraphicsCommandList_CopyTextureRegion(command_list, &dst_location, 0, 0, 0, &src_location, NULL);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    exec_command_list(queue, command_list);
    wait_queue_idle(device, queue);
    ID3D12Device_Release(device);

    read_range.Begin = 0;
    read_range.End = resource_desc.Width;
    hr = ID3D12Resource_Map(rb->resource, 0, &read_range, &rb->data);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
}

static void *get_readback_data(struct resource_readback *rb, unsigned int x, unsigned int y,
        size_t element_size)
{
    return &((BYTE *)rb->data)[rb->row_pitch * y + x * element_size];
}

static unsigned int get_readback_uint(struct resource_readback *rb, unsigned int x, unsigned int y)
{
    return *(unsigned int *)get_readback_data(rb, x, y, sizeof(unsigned int));
}

static void release_resource_readback(struct resource_readback *rb)
{
    D3D12_RANGE range = {0, 0};
    ID3D12Resource_Unmap(rb->resource, 0, &range);
    ID3D12Resource_Release(rb->resource);
}

#define check_readback_data_uint(a, b, c, d) check_readback_data_uint_(__LINE__, a, b, c, d)
static void check_readback_data_uint_(unsigned int line, struct resource_readback *rb,
        const RECT *rect, unsigned int expected, unsigned int max_diff)
{
    RECT r = {0, 0, rb->width, rb->height};
    unsigned int x = 0, y;
    BOOL all_match = TRUE;
    unsigned int got = 0;

    if (rect)
        r = *rect;

    for (y = r.top; y < r.bottom; ++y)
    {
        for (x = r.left; x < r.right; ++x)
        {
            got = get_readback_uint(rb, x, y);
            if (!compare_color(got, expected, max_diff))
            {
                all_match = FALSE;
                break;
            }
        }
        if (!all_match)
            break;
    }
    ok_(__FILE__, line)(all_match, "Got 0x%08x, expected 0x%08x at (%u, %u).\n", got, expected, x, y);
}

#define check_sub_resource_uint(a, b, c, d, e, f) check_sub_resource_uint_(__LINE__, a, b, c, d, e, f)
static void check_sub_resource_uint_(unsigned int line, ID3D12Resource *texture,
        unsigned int sub_resource_idx, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command_list,
        unsigned int expected, unsigned int max_diff)
{
    struct resource_readback rb;

    get_texture_readback_with_command_list(texture, 0, &rb, queue, command_list);
    check_readback_data_uint_(line, &rb, NULL, expected, max_diff);
    release_resource_readback(&rb);
}

static void test_ordinals(void)
{
    PFN_D3D12_CREATE_DEVICE pfn_D3D12CreateDevice, pfn_101;
    HMODULE d3d12;

    d3d12 = GetModuleHandleA("d3d12.dll");
    ok(!!d3d12, "Failed to get module handle.\n");

    pfn_D3D12CreateDevice = (void *)GetProcAddress(d3d12, "D3D12CreateDevice");
    ok(!!pfn_D3D12CreateDevice, "Failed to get D3D12CreateDevice() proc address.\n");

    pfn_101 = (void *)GetProcAddress(d3d12, (const char *)101);
    ok(pfn_101 == pfn_D3D12CreateDevice, "Got %p, expected %p.\n", pfn_101, pfn_D3D12CreateDevice);
}

static void test_interfaces(void)
{
    D3D12_COMMAND_QUEUE_DESC desc;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    check_interface(device, &IID_ID3D12Object, TRUE);
    check_interface(device, &IID_ID3D12DeviceChild, FALSE);
    check_interface(device, &IID_ID3D12Pageable, FALSE);
    check_interface(device, &IID_ID3D12Device, TRUE);

    check_interface(device, &IID_IDXGIObject, FALSE);
    check_interface(device, &IID_IDXGIDeviceSubObject, FALSE);
    check_interface(device, &IID_IDXGIDevice, FALSE);
    check_interface(device, &IID_IDXGIDevice1, FALSE);
    check_interface(device, &IID_IDXGIDevice2, FALSE);
    check_interface(device, &IID_IDXGIDevice3, FALSE);
    check_interface(device, &IID_IDXGIDevice4, FALSE);

    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(device, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    check_interface(queue, &IID_ID3D12Object, TRUE);
    check_interface(queue, &IID_ID3D12DeviceChild, TRUE);
    check_interface(queue, &IID_ID3D12Pageable, TRUE);
    check_interface(queue, &IID_ID3D12CommandQueue, TRUE);

    check_interface(queue, &IID_IDXGIObject, FALSE);
    check_interface(queue, &IID_IDXGIDeviceSubObject, FALSE);
    check_interface(queue, &IID_IDXGIDevice, FALSE);
    check_interface(queue, &IID_IDXGIDevice1, FALSE);
    check_interface(queue, &IID_IDXGIDevice2, FALSE);
    check_interface(queue, &IID_IDXGIDevice3, FALSE);
    check_interface(queue, &IID_IDXGIDevice4, FALSE);

    refcount = ID3D12CommandQueue_Release(queue);
    ok(!refcount, "Command queue has %lu references left.\n", refcount);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_create_device(void)
{
    IDXCoreAdapterFactory *dxcore_factory;
    IDXCoreAdapterList *dxcore_list;
    IDXCoreAdapter *dxcore_adapter;
    DXGI_ADAPTER_DESC adapter_desc;
    IDXGISwapChain3 *swapchain;
    ID3D12CommandQueue *queue;
    LUID adapter_luid, luid;
    IDXGIFactory4 *factory;
    IDXGIAdapter *adapter;
    HMODULE dxcore_handle;
    ID3D12Device *device;
    IDXGIOutput *output;
    ULONG refcount;
    uint32_t count;
    HWND window;
    HRESULT hr;
    RECT rect;
    BOOL ret;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDXGIFactory4_EnumAdapters(factory, 0, &adapter);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDXGIFactory4_Release(factory);

    hr = IDXGIAdapter_GetDesc(adapter, &adapter_desc);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    adapter_luid = adapter_desc.AdapterLuid;

    refcount = get_refcount(adapter);
    ok(refcount == 1, "Got unexpected refcount %lu.\n", refcount);
    hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDXGIAdapter_Release(adapter);
    adapter = NULL;

    luid = ID3D12Device_GetAdapterLuid(device);
    ok(equal_luid(luid, adapter_luid), "Got unexpected LUID %08lx:%08lx, expected %08lx:%08lx.\n",
            luid.HighPart, luid.LowPart, adapter_luid.HighPart, adapter_luid.LowPart);

    queue = create_command_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    window = create_window(WS_VISIBLE);
    ret = GetClientRect(window, &rect);
    ok(ret, "Failed to get client rect.\n");
    swapchain = create_swapchain(NULL, queue, window, 2, DXGI_FORMAT_B8G8R8A8_UNORM, rect.right, rect.bottom);

    hr = IDXGISwapChain3_GetContainingOutput(swapchain, &output);
    if (hr != DXGI_ERROR_UNSUPPORTED)
    {
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        hr = IDXGIOutput_GetParent(output, &IID_IDXGIAdapter, (void **)&adapter);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDXGIOutput_Release(output);

        memset(&adapter_desc, 0, sizeof(adapter_desc));
        hr = IDXGIAdapter_GetDesc(adapter, &adapter_desc);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        IDXGIAdapter_Release(adapter);

        ok(equal_luid(adapter_desc.AdapterLuid, adapter_luid),
                "Got unexpected LUID %08lx:%08lx, expected %08lx:%08lx.\n",
                adapter_desc.AdapterLuid.HighPart, adapter_desc.AdapterLuid.LowPart,
                adapter_luid.HighPart, adapter_luid.LowPart);
    }
    else
    {
        skip("GetContainingOutput() is not supported.\n");
    }

    refcount = IDXGISwapChain3_Release(swapchain);
    ok(!refcount, "Swapchain has %lu references left.\n", refcount);
    DestroyWindow(window);
    ID3D12CommandQueue_Release(queue);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    /* Creating a device using DXCore adapter instance. */
    dxcore_handle = LoadLibraryA("dxcore.dll");
    if (!dxcore_handle)
    {
        win_skip("Could not load dxcore.dll\n");
        return;
    }

    pDXCoreCreateAdapterFactory = (void *)GetProcAddress(dxcore_handle, "DXCoreCreateAdapterFactory");

    hr = pDXCoreCreateAdapterFactory(&IID_IDXCoreAdapterFactory, (void **)&dxcore_factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    hr = IDXCoreAdapterFactory_CreateAdapterList(dxcore_factory, 1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS,
            &IID_IDXCoreAdapterList, (void **)&dxcore_list);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    count = IDXCoreAdapterList_GetAdapterCount(dxcore_list);
    if (!count)
    {
        skip("DXCore was unable to enumerate adapters.\n");
        IDXCoreAdapterList_Release(dxcore_list);
        IDXCoreAdapterFactory_Release(dxcore_factory);
        return;
    }

    hr = IDXCoreAdapterList_GetAdapter(dxcore_list, 0, &IID_IDXCoreAdapter, (void **)&dxcore_adapter);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    IDXCoreAdapterList_Release(dxcore_list);

    hr = D3D12CreateDevice((IUnknown *)dxcore_adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "Device has %lu references left.\n", refcount);

    IDXCoreAdapter_Release(dxcore_adapter);

    IDXCoreAdapterFactory_Release(dxcore_factory);
}

static void test_draw(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    struct test_context context;
    ID3D12CommandQueue *queue;

    if (!init_test_context(&context, NULL))
        return;
    command_list = context.list[0];
    queue = context.queue;

    create_render_target(&context);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv[0], white, 0, NULL);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv[0], FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_sub_resource_state(command_list, context.render_target[0], 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    check_sub_resource_uint(context.render_target[0], 0, queue, command_list, 0xff00ff00, 0);

    destroy_test_context(&context);
}

static void test_swapchain_draw(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    struct test_context_desc desc;
    struct test_context context;
    ID3D12Resource *backbuffer;
    IDXGISwapChain3 *swapchain;
    ID3D12CommandQueue *queue;
    unsigned int i, index;
    ID3D12Device *device;
    ULONG refcount;
    HWND window;
    HRESULT hr;
    RECT rect;
    BOOL ret;

    static const DWORD ps_code[] =
    {
#if 0
        float4 color;

        float4 main() : SV_Target
        {
            return color;
        }
#endif
        0x43425844, 0x69e703c1, 0xf0db50aa, 0x9af7ae76, 0x623b93f7, 0x00000001, 0x000000bc, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000044, 0x00000050, 0x00000011,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x06000036, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};

    static const struct
    {
        DXGI_FORMAT format;
        float input[4];
        unsigned int color;
    }
    tests[] =
    {
        {DXGI_FORMAT_B8G8R8A8_UNORM,    {1.0f, 0.0f, 0.0f, 1.0f}, 0xffff0000},
        {DXGI_FORMAT_B8G8R8A8_UNORM,    {0.0f, 1.0f, 0.0f, 1.0f}, 0xff00ff00},
        {DXGI_FORMAT_R8G8B8A8_UNORM,    {1.0f, 0.0f, 0.0f, 1.0f}, 0xff0000ff},
        {DXGI_FORMAT_R8G8B8A8_UNORM,    {0.0f, 1.0f, 0.0f, 1.0f}, 0xff00ff00},
        {DXGI_FORMAT_R10G10B10A2_UNORM, {1.0f, 0.0f, 0.0f, 1.0f}, 0xc00003ff},
        {DXGI_FORMAT_R10G10B10A2_UNORM, {0.0f, 1.0f, 0.0f, 1.0f}, 0xc00ffc00},
    };

    memset(&desc, 0, sizeof(desc));
    desc.no_pipeline = TRUE;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list[0];
    queue = context.queue;

    window = create_window(WS_VISIBLE);
    ret = GetClientRect(window, &rect);
    ok(ret, "Failed to get client rect.\n");

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        context.pipeline_state = create_pipeline_state(device,
                context.root_signature, tests[i].format, &ps);

        swapchain = create_swapchain(&context, queue, window, 2, tests[i].format, rect.right, rect.bottom);
        index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
        backbuffer = context.render_target[index];
        rtv = context.rtv[index];

        transition_sub_resource_state(command_list, backbuffer, 0,
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv, white, 0, NULL);

        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &rect);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 4, tests[i].input, 0);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

        transition_sub_resource_state(command_list, backbuffer, 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_uint(backbuffer, 0, queue, command_list, tests[i].color, 0);

        reset_command_list(&context, 0);
        transition_sub_resource_state(command_list, backbuffer, 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        hr = ID3D12GraphicsCommandList_Close(command_list);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        exec_command_list(queue, command_list);

        hr = IDXGISwapChain3_Present(swapchain, 0, 0);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        wait_queue_idle(device, queue);

        destroy_render_targets(&context);
        refcount = IDXGISwapChain3_Release(swapchain);
        ok(!refcount, "Swapchain has %lu references left.\n", refcount);
        ID3D12PipelineState_Release(context.pipeline_state);
        context.pipeline_state = NULL;

        reset_command_list(&context, 0);
    }

    DestroyWindow(window);
    destroy_test_context(&context);
}

static void test_swapchain_refcount(void)
{
    const unsigned int buffer_count = 4;
    struct test_context_desc desc;
    struct test_context context;
    IDXGISwapChain3 *swapchain;
    unsigned int i;
    ULONG refcount;
    HWND window;
    HRESULT hr;
    RECT rect;
    BOOL ret;

    memset(&desc, 0, sizeof(desc));
    desc.no_pipeline = TRUE;
    if (!init_test_context(&context, &desc))
        return;

    window = create_window(WS_VISIBLE);
    ret = GetClientRect(window, &rect);
    ok(ret, "Failed to get client rect.\n");
    swapchain = create_swapchain(&context, context.queue, window,
            buffer_count, DXGI_FORMAT_B8G8R8A8_UNORM, rect.right, rect.bottom);

    for (i = 0; i < buffer_count; ++i)
    {
        refcount = get_refcount(swapchain);
        todo_wine ok(refcount == 2, "Got refcount %lu.\n", refcount);
        ID3D12Resource_Release(context.render_target[i]);
        context.render_target[i] = NULL;
    }
    refcount = get_refcount(swapchain);
    ok(refcount == 1, "Got refcount %lu.\n", refcount);

    refcount = IDXGISwapChain3_AddRef(swapchain);
    ok(refcount == 2, "Got refcount %lu.\n", refcount);
    hr = IDXGISwapChain3_GetBuffer(swapchain, 0, &IID_ID3D12Resource, (void **)&context.render_target[0]);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    refcount = get_refcount(swapchain);
    todo_wine ok(refcount == 3, "Got refcount %lu.\n", refcount);

    refcount = ID3D12Resource_AddRef(context.render_target[0]);
    ok(refcount == 2, "Got refcount %lu.\n", refcount);
    refcount = get_refcount(swapchain);
    todo_wine ok(refcount == 3, "Got refcount %lu.\n", refcount);

    hr = IDXGISwapChain3_GetBuffer(swapchain, 1, &IID_ID3D12Resource, (void **)&context.render_target[1]);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    refcount = get_refcount(swapchain);
    todo_wine ok(refcount == 3, "Got refcount %lu.\n", refcount);

    ID3D12Resource_Release(context.render_target[0]);
    ID3D12Resource_Release(context.render_target[0]);
    context.render_target[0] = NULL;
    refcount = get_refcount(swapchain);
    todo_wine ok(refcount == 3, "Got refcount %lu.\n", refcount);

    refcount = IDXGISwapChain3_Release(swapchain);
    todo_wine ok(refcount == 2, "Got refcount %lu.\n", refcount);
    ID3D12Resource_Release(context.render_target[1]);
    context.render_target[1] = NULL;
    refcount = get_refcount(swapchain);
    ok(refcount == 1, "Got refcount %lu.\n", refcount);

    refcount = IDXGISwapChain3_Release(swapchain);
    ok(!refcount, "Swapchain has %lu references left.\n", refcount);
    DestroyWindow(window);
    destroy_test_context(&context);
}

static void test_swapchain_size_mismatch(void)
{
    static const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
    UINT64 frame_fence_value[MAX_FRAME_COUNT] = {0};
    ID3D12GraphicsCommandList *command_list;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    struct test_context_desc desc;
    struct test_context context;
    ID3D12Resource *backbuffer;
    IDXGISwapChain3 *swapchain;
    ID3D12CommandQueue *queue;
    unsigned int index, i;
    ID3D12Device *device;
    ID3D12Fence *fence;
    UINT64 fence_value;
    ULONG refcount;
    HWND window;
    HRESULT hr;
    RECT rect;
    BOOL ret;

    memset(&desc, 0, sizeof(desc));
    desc.no_pipeline = TRUE;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    command_list = context.list[0];
    queue = context.queue;

    window = CreateWindowA("static", "d3d12_test", WS_VISIBLE, 0, 0, 200, 200, NULL, NULL, NULL, NULL);
    swapchain = create_swapchain(&context, queue, window, 2, DXGI_FORMAT_B8G8R8A8_UNORM, 400, 400);
    index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
    backbuffer = context.render_target[index];
    rtv = context.rtv[index];

    transition_sub_resource_state(command_list, backbuffer, 0,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv, green, 0, NULL);
    transition_sub_resource_state(command_list, backbuffer, 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(backbuffer, 0, queue, command_list, 0xff00ff00, 0);

    reset_command_list(&context, 0);
    transition_sub_resource_state(command_list, backbuffer, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    exec_command_list(queue, command_list);

    hr = IDXGISwapChain3_Present(swapchain, 1, 0);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    wait_queue_idle(device, queue);
    reset_command_list(&context, 0);

    destroy_render_targets(&context);
    refcount = IDXGISwapChain3_Release(swapchain);
    ok(!refcount, "Swapchain has %lu references left.\n", refcount);
    DestroyWindow(window);

    window = create_window(WS_VISIBLE);
    ret = GetClientRect(window, &rect);
    ok(ret, "Failed to get client rect.\n");
    swapchain = create_swapchain(&context, queue, window, 4, DXGI_FORMAT_B8G8R8A8_UNORM, rect.right, rect.bottom);

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    for (i = 0; i < ARRAY_SIZE(context.list); ++i)
    {
        hr = ID3D12GraphicsCommandList_Close(context.list[i]);
        ok(hr == S_OK, "Failed to close command list %u, hr %#lx.\n", i, hr);
    }

    fence_value = 1;
    for (i = 0; i < 20; ++i)
    {
        index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);

        hr = wait_for_fence(fence, frame_fence_value[index]);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        reset_command_list(&context, index);
        backbuffer = context.render_target[index];
        command_list = context.list[index];
        rtv = context.rtv[index];

        transition_sub_resource_state(command_list, backbuffer, 0,
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv, green, 0, NULL);
        transition_sub_resource_state(command_list, backbuffer, 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        hr = ID3D12GraphicsCommandList_Close(command_list);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        exec_command_list(queue, command_list);

        hr = IDXGISwapChain3_Present(swapchain, 1, 0);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        if (i == 6)
        {
            wait_queue_idle(device, queue);
            MoveWindow(window, 0, 0, 100, 100, TRUE);
        }

        frame_fence_value[index] = fence_value;
        hr = ID3D12CommandQueue_Signal(queue, fence, fence_value);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        ++fence_value;
    }

    wait_queue_idle(device, queue);

    ID3D12Fence_Release(fence);
    destroy_render_targets(&context);
    refcount = IDXGISwapChain3_Release(swapchain);
    ok(!refcount, "Swapchain has %lu references left.\n", refcount);
    DestroyWindow(window);
    destroy_test_context(&context);
}

static void test_swapchain_backbuffer_index(void)
{
    static const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
    UINT64 frame_fence_value[MAX_FRAME_COUNT] = {0};
    ID3D12GraphicsCommandList *command_list;
    unsigned int expected_index, index, i;
    const unsigned int buffer_count = 2;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    struct test_context_desc desc;
    struct test_context context;
    ID3D12Resource *backbuffer;
    unsigned int sync_interval;
    IDXGISwapChain3 *swapchain;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    ID3D12Fence *fence;
    UINT64 fence_value;
    ULONG refcount;
    HWND window;
    HRESULT hr;
    RECT rect;
    BOOL ret;

    memset(&desc, 0, sizeof(desc));
    desc.no_pipeline = TRUE;
    if (!init_test_context(&context, &desc))
        return;
    device = context.device;
    queue = context.queue;

    window = create_window(WS_VISIBLE);
    ret = GetClientRect(window, &rect);
    ok(ret, "Failed to get client rect.\n");
    swapchain = create_swapchain(&context, queue, window,
            buffer_count, DXGI_FORMAT_B8G8R8A8_UNORM, rect.right, rect.bottom);

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    for (i = 0; i < ARRAY_SIZE(context.list); ++i)
    {
        hr = ID3D12GraphicsCommandList_Close(context.list[i]);
        ok(hr == S_OK, "Failed to close command list %u, hr %#lx.\n", i, hr);
    }

    index = 1;
    fence_value = 1;
    for (i = 0; i < 20; ++i)
    {
        expected_index = (index + 1) % buffer_count;
        index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
        ok(index == expected_index, "Test %u: Got index %u, expected %u.\n", i, index, expected_index);

        hr = wait_for_fence(fence, frame_fence_value[index]);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        reset_command_list(&context, index);
        backbuffer = context.render_target[index];
        command_list = context.list[index];
        rtv = context.rtv[index];

        transition_sub_resource_state(command_list, backbuffer, 0,
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv, green, 0, NULL);
        transition_sub_resource_state(command_list, backbuffer, 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        hr = ID3D12GraphicsCommandList_Close(command_list);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        exec_command_list(queue, command_list);

        if (i <= 4 || (8 <= i && i <= 14))
            sync_interval = 1;
        else
            sync_interval = 0;

        hr = IDXGISwapChain3_Present(swapchain, sync_interval, 0);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

        frame_fence_value[index] = fence_value;
        hr = ID3D12CommandQueue_Signal(queue, fence, fence_value);
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
        ++fence_value;
    }

    wait_queue_idle(device, queue);

    ID3D12Fence_Release(fence);
    destroy_render_targets(&context);
    refcount = IDXGISwapChain3_Release(swapchain);
    ok(!refcount, "Swapchain has %lu references left.\n", refcount);
    DestroyWindow(window);
    destroy_test_context(&context);
}

static void test_desktop_window(void)
{
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
    struct test_context_desc desc;
    struct test_context context;
    IDXGISwapChain1 *swapchain;
    IDXGIFactory4 *factory;
    IUnknown *queue;
    HWND window;
    HRESULT hr;
    RECT rect;
    BOOL ret;

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = TRUE;
    if (!init_test_context(&context, NULL))
        return;
    queue = (IUnknown *)context.queue;

    window = GetDesktopWindow();
    ret = GetClientRect(window, &rect);
    ok(ret, "Failed to get client rect.\n");

    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);

    swapchain_desc.Width = 640;
    swapchain_desc.Height = 480;
    swapchain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchain_desc.Stereo = FALSE;
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.SampleDesc.Quality = 0;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = 2;
    swapchain_desc.Scaling = DXGI_SCALING_STRETCH;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapchain_desc.Flags = 0;
    hr = IDXGIFactory4_CreateSwapChainForHwnd(factory, queue, window, &swapchain_desc, NULL, NULL, &swapchain);
    ok(hr == E_ACCESSDENIED, "Got unexpected hr %#lx.\n", hr);

    swapchain_desc.Width = rect.right;
    swapchain_desc.Height = rect.bottom;
    hr = IDXGIFactory4_CreateSwapChainForHwnd(factory, queue, window, &swapchain_desc, NULL, NULL, &swapchain);
    ok(hr == E_ACCESSDENIED || broken(hr == E_OUTOFMEMORY /* win10 1709 */),
       "Got unexpected hr %#lx.\n", hr);

    IDXGIFactory4_Release(factory);
    destroy_test_context(&context);
}

static void test_invalid_command_queue_types(void)
{
    static const enum D3D12_COMMAND_LIST_TYPE queue_types[] =
    {
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        D3D12_COMMAND_LIST_TYPE_COPY,
    };

    DXGI_SWAP_CHAIN_DESC swapchain_desc;
    ID3D12CommandQueue *queue;
    IDXGISwapChain *swapchain;
    IDXGIFactory *factory;
    ID3D12Device *device;
    HRESULT hr, expected;
    IUnknown *queue_unk;
    RECT client_rect;
    ULONG refcount;
    unsigned int i;
    HWND window;
    BOOL ret;

    if (!(device = create_device()))
    {
        skip("Failed to create Direct3D 12 device.\n");
        return;
    }

    window = create_window(WS_VISIBLE);
    ret = GetClientRect(window, &client_rect);
    ok(ret, "Failed to get client rect.\n");

    for (i = 0; i < ARRAY_SIZE(queue_types); ++i)
    {
        queue = create_command_queue(device, queue_types[i]);
        hr = ID3D12CommandQueue_QueryInterface(queue, &IID_IUnknown, (void **)&queue_unk);
        ok(hr == S_OK, "Got unexpected hr %lx.\n", hr);

        hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
        ok(hr == S_OK, "Got unexpected hr %lx.\n", hr);

        swapchain_desc.BufferDesc.Width = 640;
        swapchain_desc.BufferDesc.Height = 480;
        swapchain_desc.BufferDesc.RefreshRate.Numerator = 60;
        swapchain_desc.BufferDesc.RefreshRate.Denominator = 1;
        swapchain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapchain_desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        swapchain_desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        swapchain_desc.SampleDesc.Count = 1;
        swapchain_desc.SampleDesc.Quality = 0;
        swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapchain_desc.BufferCount = 2;
        swapchain_desc.OutputWindow = window;
        swapchain_desc.Windowed = TRUE;
        swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapchain_desc.Flags = 0;

        hr = IDXGIFactory_CreateSwapChain(factory, queue_unk, &swapchain_desc, &swapchain);
        expected = queue_types[i] == D3D12_COMMAND_LIST_TYPE_DIRECT ? S_OK : DXGI_ERROR_INVALID_CALL;
        ok(hr == expected, "Got unexpected hr %#lx.\n", hr);

        if (hr == S_OK)
        {
            refcount = IDXGISwapChain_Release(swapchain);
            ok(!refcount, "Swapchain has %lu references left.\n", refcount);
        }

        wait_queue_idle(device, queue);

        IDXGIFactory_Release(factory);

        IUnknown_Release(queue_unk);
        refcount = ID3D12CommandQueue_Release(queue);
        ok(!refcount, "Command queue has %lu references left.\n", refcount);
    }

    DestroyWindow(window);

    refcount = ID3D12Device_Release(device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_options14_to_options18_invalid_sizes(void)
{
    static const struct
    {
        D3D12_FEATURE feature;
        UINT size;
    }
    tests[] =
    {
        {D3D12_FEATURE_D3D12_OPTIONS14, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS14)},
        {D3D12_FEATURE_D3D12_OPTIONS15, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS15)},
        {D3D12_FEATURE_D3D12_OPTIONS16, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16)},
        {D3D12_FEATURE_D3D12_OPTIONS17, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS17)},
        {D3D12_FEATURE_D3D12_OPTIONS18, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS18)},
    };
    union
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14;
        D3D12_FEATURE_DATA_D3D12_OPTIONS15 options15;
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16;
        D3D12_FEATURE_DATA_D3D12_OPTIONS17 options17;
        D3D12_FEATURE_DATA_D3D12_OPTIONS18 options18;
        BYTE bytes[32];
    } data, expected;
    ID3D12Device *device;
    unsigned int i, j;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create Direct3D 12 device.\n");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        static const int size_offsets[] = {-1, 1};

        for (j = 0; j < ARRAY_SIZE(size_offsets); ++j)
        {
            memset(&data, 0x5a, sizeof(data));
            expected = data;
            hr = ID3D12Device_CheckFeatureSupport(device, tests[i].feature, &data,
                    tests[i].size + size_offsets[j]);
            ok(hr == E_INVALIDARG, "Test %u, size offset %d: Got unexpected hr %#lx.\n",
                    i, size_offsets[j], hr);
            ok(!memcmp(&data, &expected, sizeof(data)),
                    "Test %u, size offset %d: Feature data was modified.\n", i, size_offsets[j]);
        }
    }

    ID3D12Device_Release(device);
}

static void test_create_command_queue1(void)
{
    static const GUID creator_id =
    {
        0x751f9bf8, 0xaec2, 0x438d, {0xa3, 0x74, 0xe1, 0x7f, 0x10, 0x84, 0x84, 0x50}
    };
    D3D12_COMMAND_QUEUE_DESC queue_desc, actual_desc;
    ID3D12CommandQueue *queue;
    ID3D12Device9 *device9;
    ID3D12Device *device;
    ULONG refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create Direct3D 12 device.\n");
        return;
    }

    hr = ID3D12Device_QueryInterface(device, &IID_ID3D12Device9, (void **)&device9);
    if (hr == E_NOINTERFACE)
    {
        win_skip("ID3D12Device9 is not supported.\n");
        ID3D12Device_Release(device);
        return;
    }
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        ID3D12Device_Release(device);
        return;
    }

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    queue = NULL;
    hr = ID3D12Device9_CreateCommandQueue1(device9, &queue_desc, &creator_id,
            &IID_ID3D12CommandQueue, (void **)&queue);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(!!queue, "Failed to create a command queue.\n");
    if (queue)
    {
        actual_desc = ID3D12CommandQueue_GetDesc(queue);
        ok(actual_desc.Type == queue_desc.Type, "Got unexpected queue type %#x.\n", actual_desc.Type);
        ok(actual_desc.Priority == queue_desc.Priority, "Got unexpected priority %d.\n", actual_desc.Priority);
        ok(actual_desc.Flags == queue_desc.Flags, "Got unexpected flags %#x.\n", actual_desc.Flags);
        ok(actual_desc.NodeMask == queue_desc.NodeMask, "Got unexpected node mask %#x.\n", actual_desc.NodeMask);
        refcount = ID3D12CommandQueue_Release(queue);
        ok(!refcount, "Command queue has %lu references left.\n", refcount);
    }

    ID3D12Device9_Release(device9);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "Device has %lu references left.\n", refcount);
}

static void test_feature_level_reporting(void)
{
    static const D3D_FEATURE_LEVEL requested_feature_levels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels;
    ID3D12Device *device;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create Direct3D 12 device.\n");
        return;
    }

    feature_levels.NumFeatureLevels = ARRAY_SIZE(requested_feature_levels);
    feature_levels.pFeatureLevelsRequested = requested_feature_levels;
    feature_levels.MaxSupportedFeatureLevel = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels, sizeof(feature_levels));
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(feature_levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_11_0,
            "Got unexpected maximum feature level %#x.\n", feature_levels.MaxSupportedFeatureLevel);

    ID3D12Device_Release(device);
}

static void init_shader_cache_desc(D3D12_SHADER_CACHE_SESSION_DESC *desc,
        unsigned int identifier, D3D12_SHADER_CACHE_MODE mode, UINT64 version)
{
    static const GUID base_identifier =
            {0x8b1a56f2, 0xe984, 0x49d7, {0x82, 0xe7, 0x59, 0xd7, 0xe9, 0xda, 0x31, 0x41}};

    memset(desc, 0, sizeof(*desc));
    desc->Identifier = base_identifier;
    desc->Identifier.Data1 += identifier;
    desc->Mode = mode;
    desc->Version = version;
    if (mode == D3D12_SHADER_CACHE_MODE_DISK)
        desc->Flags = D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR;
}

static BOOL get_shader_cache_device(ID3D12Device **device, ID3D12Device9 **device9)
{
    HRESULT hr;

    *device9 = NULL;
    if (!(*device = create_device()))
    {
        skip("Failed to create device.\n");
        return FALSE;
    }

    hr = ID3D12Device_QueryInterface(*device, &IID_ID3D12Device9, (void **)device9);
    if (hr == E_NOINTERFACE)
    {
        win_skip("ID3D12Device9 is not supported.\n");
        ID3D12Device_Release(*device);
        *device = NULL;
        return FALSE;
    }
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        ID3D12Device_Release(*device);
        *device = NULL;
        return FALSE;
    }

    return TRUE;
}

static HRESULT create_shader_cache_session_(unsigned int line, ID3D12Device9 *device,
        const D3D12_SHADER_CACHE_SESSION_DESC *desc, ID3D12ShaderCacheSession **session)
{
    HRESULT hr;

    *session = NULL;
    hr = ID3D12Device9_CreateShaderCacheSession(device, desc,
            &IID_ID3D12ShaderCacheSession, (void **)session);
    ok_(__FILE__, line)(hr == S_OK, "Failed to create shader cache session, hr %#lx.\n", hr);
    return hr;
}
#define create_shader_cache_session(a, b, c) create_shader_cache_session_(__LINE__, a, b, c)

static void check_shader_cache_defaults(ID3D12ShaderCacheSession *session,
        const D3D12_SHADER_CACHE_SESSION_DESC *desc)
{
    D3D12_SHADER_CACHE_SESSION_DESC actual;

    actual = ID3D12ShaderCacheSession_GetDesc(session);
    ok(IsEqualGUID(&actual.Identifier, &desc->Identifier), "Got unexpected identifier.\n");
    ok(actual.Mode == desc->Mode, "Got unexpected mode %#x.\n", actual.Mode);
    ok(actual.Flags == desc->Flags, "Got unexpected flags %#x.\n", actual.Flags);
    ok(actual.MaximumInMemoryCacheSizeBytes == 1024,
            "Got unexpected memory size %u.\n", actual.MaximumInMemoryCacheSizeBytes);
    ok(actual.MaximumInMemoryCacheEntries == 128,
            "Got unexpected memory entry count %u.\n", actual.MaximumInMemoryCacheEntries);
    ok(actual.MaximumValueFileSizeBytes == 128 * 1024 * 1024,
            "Got unexpected maximum value file size %u.\n", actual.MaximumValueFileSizeBytes);
    ok(actual.Version == desc->Version, "Got unexpected version %#I64x.\n", actual.Version);
}

static void check_shader_cache_value_(unsigned int line, ID3D12ShaderCacheSession *session,
        const char *key, UINT key_size, UINT expected)
{
    UINT value, value_size;
    HRESULT hr;

    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session, key, key_size, NULL, &value_size);
    ok_(__FILE__, line)(hr == S_OK, "Failed to query shader cache value size, hr %#lx.\n", hr);
    ok_(__FILE__, line)(value_size == sizeof(value), "Got value size %u.\n", value_size);

    value = 0;
    value_size = sizeof(value);
    hr = ID3D12ShaderCacheSession_FindValue(session, key, key_size, &value, &value_size);
    ok_(__FILE__, line)(hr == S_OK, "Failed to find shader cache value, hr %#lx.\n", hr);
    ok_(__FILE__, line)(value_size == sizeof(value), "Got value size %u.\n", value_size);
    ok_(__FILE__, line)(value == expected, "Got value %#x, expected %#x.\n", value, expected);
}
#define check_shader_cache_value(a, b, c, d) check_shader_cache_value_(__LINE__, a, b, c, d)

static void check_shader_cache_find_small_buffer(ID3D12ShaderCacheSession *session,
        const char *key, UINT key_size)
{
    UINT value, value_size;
    HRESULT hr;

    value = 0xdeadbeef;
    value_size = sizeof(value) - 1;
    hr = ID3D12ShaderCacheSession_FindValue(session, key, key_size, &value, &value_size);
    todo_wine_if(hr != DXGI_ERROR_MORE_DATA)
    ok(hr == DXGI_ERROR_MORE_DATA, "Got unexpected hr %#lx.\n", hr);
    ok(value_size == sizeof(value), "Got value size %u.\n", value_size);
}

struct shader_cache_working_dir
{
    char path[MAX_PATH];
    char old_path[MAX_PATH];
    BOOL active;
};

static void remove_directory_tree(const char *path)
{
    WIN32_FIND_DATAA data;
    char search[MAX_PATH];
    char child[MAX_PATH];
    HANDLE find;

    if (strlen(path) + 3 >= ARRAY_SIZE(search))
        return;
    strcpy(search, path);
    strcat(search, "\\*");

    find = FindFirstFileA(search, &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
                continue;
            if (strlen(path) + strlen(data.cFileName) + 2 >= ARRAY_SIZE(child))
                continue;
            strcpy(child, path);
            strcat(child, "\\");
            strcat(child, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    RemoveDirectoryA(child);
                else
                    DeleteFileA(child);
            }
            else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                remove_directory_tree(child);
                RemoveDirectoryA(child);
            }
            else
            {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child);
            }
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }

    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryA(path);
}

static BOOL shader_cache_working_dir_init(struct shader_cache_working_dir *dir)
{
    char temp_path[MAX_PATH];
    DWORD len;

    dir->active = FALSE;
    len = GetCurrentDirectoryA(ARRAY_SIZE(dir->old_path), dir->old_path);
    ok(len && len < ARRAY_SIZE(dir->old_path), "Failed to get current directory, error %lu.\n", GetLastError());
    if (!len || len >= ARRAY_SIZE(dir->old_path))
        return FALSE;

    len = GetTempPathA(ARRAY_SIZE(temp_path), temp_path);
    ok(len && len < ARRAY_SIZE(temp_path), "Failed to get temporary directory, error %lu.\n", GetLastError());
    if (!len || len >= ARRAY_SIZE(temp_path))
        return FALSE;

    if (!GetTempFileNameA(temp_path, "d3d", 0, dir->path))
    {
        ok(FALSE, "Failed to get temporary filename, error %lu.\n", GetLastError());
        return FALSE;
    }
    DeleteFileA(dir->path);
    if (!CreateDirectoryA(dir->path, NULL))
    {
        ok(FALSE, "Failed to create temporary directory, error %lu.\n", GetLastError());
        return FALSE;
    }
    if (!SetCurrentDirectoryA(dir->path))
    {
        ok(FALSE, "Failed to change current directory, error %lu.\n", GetLastError());
        remove_directory_tree(dir->path);
        return FALSE;
    }

    dir->active = TRUE;
    return TRUE;
}

static void shader_cache_working_dir_cleanup(struct shader_cache_working_dir *dir);

static void check_shader_cache_reparse_root(ID3D12Device9 *device, unsigned int identifier)
{
    struct shader_cache_working_dir dir;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    char outside[MAX_PATH], temp_path[MAX_PATH], sentinel[MAX_PATH];
    HANDLE file;
    HRESULT hr;

    if (!shader_cache_working_dir_init(&dir))
        return;
    if (!GetTempPathA(ARRAY_SIZE(temp_path), temp_path)
            || !GetTempFileNameA(temp_path, "d3o", 0, outside))
    {
        shader_cache_working_dir_cleanup(&dir);
        return;
    }
    DeleteFileA(outside);
    if (!CreateDirectoryA(outside, NULL))
    {
        shader_cache_working_dir_cleanup(&dir);
        return;
    }
    sprintf(sentinel, "%s\\sentinel", outside);
    file = CreateFileA(sentinel, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(file != INVALID_HANDLE_VALUE, "Failed to create sentinel, error %lu.\n", GetLastError());
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);

    if (!CreateSymbolicLinkA("vkd3d-cache", outside, SYMBOLIC_LINK_FLAG_DIRECTORY))
    {
        win_skip("Cannot create directory symlink, error %lu.\n", GetLastError());
        shader_cache_working_dir_cleanup(&dir);
        DeleteFileA(sentinel);
        RemoveDirectoryA(outside);
        return;
    }

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_DISK, 1);
    session = (ID3D12ShaderCacheSession *)0xdeadbeef;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    ok(FAILED(hr), "Unsafe reparse cache root opened, hr %#lx.\n", hr);
    ok(!session, "Got unexpected session %p.\n", session);
    ok(GetFileAttributesA(sentinel) != INVALID_FILE_ATTRIBUTES,
            "Cache root escape modified the sentinel, error %lu.\n", GetLastError());

    shader_cache_working_dir_cleanup(&dir);
    DeleteFileA(sentinel);
    RemoveDirectoryA(outside);
}

static void shader_cache_working_dir_cleanup(struct shader_cache_working_dir *dir)
{
    if (!dir->active)
        return;
    ok(SetCurrentDirectoryA(dir->old_path), "Failed to restore current directory, error %lu.\n", GetLastError());
    remove_directory_tree(dir->path);
    dir->active = FALSE;
}

struct shader_cache_thread_data
{
    ID3D12Device9 *device;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    HANDLE start_event;
    HRESULT create_hr;
    HRESULT store_hr;
    char key[32];
    UINT value;
};

static DWORD WINAPI shader_cache_store_thread(void *arg)
{
    struct shader_cache_thread_data *data = arg;
    ID3D12ShaderCacheSession *session;

    WaitForSingleObject(data->start_event, INFINITE);
    data->create_hr = ID3D12Device9_CreateShaderCacheSession(data->device, &data->desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    if (SUCCEEDED(data->create_hr))
    {
        data->store_hr = ID3D12ShaderCacheSession_StoreValue(session, data->key,
                strlen(data->key) + 1, &data->value, sizeof(data->value));
        ID3D12ShaderCacheSession_Release(session);
    }
    return 0;
}

static void check_shader_cache_thread_race(ID3D12Device9 *device, D3D12_SHADER_CACHE_MODE mode,
        unsigned int identifier, BOOL duplicate_keys)
{
    static const unsigned int thread_count = 8;
    struct shader_cache_thread_data data[8];
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    unsigned int i, success_count;
    HANDLE threads[8], start_event;
    BOOL all_threads_created;
    DWORD wait;
    UINT value, value_size;
    HRESULT hr;

    ok(thread_count == ARRAY_SIZE(data), "Unexpected thread count.\n");

    init_shader_cache_desc(&desc, identifier, mode, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    start_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    ok(!!start_event, "Failed to create event, error %lu.\n", GetLastError());
    if (!start_event)
    {
        ID3D12ShaderCacheSession_Release(session);
        return;
    }

    all_threads_created = TRUE;
    for (i = 0; i < thread_count; ++i)
    {
        memset(&data[i], 0, sizeof(data[i]));
        data[i].device = device;
        data[i].start_event = start_event;
        data[i].desc = desc;
        if (duplicate_keys)
            strcpy(data[i].key, "thread-duplicate");
        else
            sprintf(data[i].key, "thread-%u", i);
        data[i].value = 0x1000 + i;
        data[i].create_hr = E_FAIL;
        data[i].store_hr = E_FAIL;
        threads[i] = CreateThread(NULL, 0, shader_cache_store_thread, &data[i], 0, NULL);
        ok(!!threads[i], "Failed to create thread %u, error %lu.\n", i, GetLastError());
        if (!threads[i])
            all_threads_created = FALSE;
    }

    if (!all_threads_created)
    {
        SetEvent(start_event);
        for (i = 0; i < thread_count; ++i)
        {
            if (threads[i])
            {
                wait = WaitForSingleObject(threads[i], 10000);
                ok(wait == WAIT_OBJECT_0, "%u: Got unexpected wait result %#lx.\n", i, wait);
                CloseHandle(threads[i]);
            }
        }
        CloseHandle(start_event);
        ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
        ID3D12ShaderCacheSession_Release(session);
        return;
    }

    SetEvent(start_event);
    wait = WaitForMultipleObjects(thread_count, threads, TRUE, 10000);
    ok(wait == WAIT_OBJECT_0, "Got unexpected wait result %#lx.\n", wait);
    if (wait != WAIT_OBJECT_0)
    {
        /* The thread arguments are stack-backed. Do not force termination and
         * allow a worker to outlive them after a transiently slow run. */
        WaitForMultipleObjects(thread_count, threads, TRUE, INFINITE);
        for (i = 0; i < thread_count; ++i)
            CloseHandle(threads[i]);
        CloseHandle(start_event);
        ID3D12ShaderCacheSession_Release(session);
        return;
    }

    for (i = 0; i < thread_count; ++i)
    {
        if (threads[i])
            CloseHandle(threads[i]);
    }
    CloseHandle(start_event);

    success_count = 0;
    for (i = 0; i < thread_count; ++i)
    {
        ok(data[i].create_hr == S_OK, "%u: Got unexpected create hr %#lx.\n", i, data[i].create_hr);
        if (duplicate_keys)
        {
            ok(data[i].store_hr == S_OK || data[i].store_hr == DXGI_ERROR_ALREADY_EXISTS,
                    "%u: Got unexpected store hr %#lx.\n", i, data[i].store_hr);
            if (data[i].store_hr == S_OK)
                ++success_count;
        }
        else
        {
            ok(data[i].store_hr == S_OK, "%u: Got unexpected store hr %#lx.\n", i, data[i].store_hr);
        }
    }
    if (duplicate_keys)
        ok(success_count == 1, "Got %u successful duplicate stores.\n", success_count);

    if (duplicate_keys)
    {
        value = 0;
        value_size = sizeof(value);
        hr = ID3D12ShaderCacheSession_FindValue(session, data[0].key, strlen(data[0].key) + 1, &value, &value_size);
        ok(hr == S_OK, "Failed to find duplicate race value, hr %#lx.\n", hr);
        ok(value >= 0x1000 && value < 0x1000 + thread_count, "Got unexpected value %#x.\n", value);
    }
    else
    {
        for (i = 0; i < thread_count; ++i)
            check_shader_cache_value(session, data[i].key, strlen(data[i].key) + 1, data[i].value);
    }

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_common_contract(ID3D12Device9 *device,
        D3D12_SHADER_CACHE_MODE mode, unsigned int identifier)
{
    static const char key[] = "shader";
    static const char missing_key[] = "missing";
    static const UINT expected = 0x12345678;
    static const UINT replacement = 0x87654321;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    HRESULT hr;
    UINT value_size;

    init_shader_cache_desc(&desc, identifier, mode, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session, missing_key, sizeof(missing_key), NULL, &value_size);
    ok(hr == DXGI_ERROR_NOT_FOUND, "Got unexpected hr %#lx.\n", hr);

    hr = ID3D12ShaderCacheSession_StoreValue(session, key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store shader cache value, hr %#lx.\n", hr);
    hr = ID3D12ShaderCacheSession_StoreValue(session, key, sizeof(key), &replacement, sizeof(replacement));
    ok(hr == DXGI_ERROR_ALREADY_EXISTS, "Got unexpected hr %#lx.\n", hr);

    check_shader_cache_value(session, key, sizeof(key), expected);
    check_shader_cache_find_small_buffer(session, key, sizeof(key));

    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_process_identity(ID3D12Device9 *device, unsigned int identifier)
{
    static const char key[] = "shared-identifier";
    static const UINT expected = 0x9abcdef0;
    D3D12_SHADER_CACHE_SESSION_DESC desc, desc2, actual;
    ID3D12ShaderCacheSession *session, *session2;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_MEMORY, 7);
    desc.MaximumInMemoryCacheSizeBytes = 4096;
    desc.MaximumInMemoryCacheEntries = 16;
    desc.MaximumValueFileSizeBytes = 64;
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    desc2 = desc;
    desc2.Mode = D3D12_SHADER_CACHE_MODE_DISK;
    desc2.Flags = D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED
            | D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR;
    desc2.MaximumInMemoryCacheSizeBytes = 8192;
    desc2.MaximumInMemoryCacheEntries = 32;
    desc2.MaximumValueFileSizeBytes = 128;
    hr = create_shader_cache_session(device, &desc2, &session2);
    if (FAILED(hr))
    {
        ID3D12ShaderCacheSession_Release(session);
        return;
    }

    actual = ID3D12ShaderCacheSession_GetDesc(session2);
    ok(actual.Mode == desc.Mode, "Got unexpected shared mode %#x.\n", actual.Mode);
    ok(actual.Flags == desc.Flags, "Got unexpected shared flags %#x.\n", actual.Flags);
    ok(actual.MaximumInMemoryCacheSizeBytes == desc.MaximumInMemoryCacheSizeBytes,
            "Got unexpected shared memory size %u.\n", actual.MaximumInMemoryCacheSizeBytes);
    ok(actual.MaximumInMemoryCacheEntries == desc.MaximumInMemoryCacheEntries,
            "Got unexpected shared entry count %u.\n", actual.MaximumInMemoryCacheEntries);
    ok(actual.MaximumValueFileSizeBytes == desc.MaximumValueFileSizeBytes,
            "Got unexpected shared file size %u.\n", actual.MaximumValueFileSizeBytes);

    hr = ID3D12ShaderCacheSession_StoreValue(session,
            key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store shared value, hr %#lx.\n", hr);
    check_shader_cache_value(session2, key, sizeof(key), expected);

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session2);
    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_invalid_arguments(ID3D12Device9 *device, unsigned int identifier)
{
    static const GUID guid_null;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    UINT value = 0x12345678, value_size;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_MEMORY, 1);

    session = (ID3D12ShaderCacheSession *)0xdeadbeef;
    hr = ID3D12Device9_CreateShaderCacheSession(device, NULL,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    desc.Identifier = guid_null;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_MEMORY, 1);
    desc.Mode = 0x7fffffff;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_MEMORY, 1);
    desc.Flags = 0x80000000;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_MEMORY, 1);
    desc.MaximumValueFileSizeBytes = 1024 * 1024 * 1024u + 1;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_MEMORY, 1);
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, NULL);
    ok(hr == S_FALSE, "Got unexpected hr %#lx.\n", hr);

    init_shader_cache_desc(&desc, identifier + 1, D3D12_SHADER_CACHE_MODE_MEMORY, 1);
    desc.MaximumValueFileSizeBytes = 1024 * 1024 * 1024u;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    ok(hr == S_OK, "Maximum valid file size failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        ID3D12ShaderCacheSession_Release(session);

    init_shader_cache_desc(&desc, identifier + 2, D3D12_SHADER_CACHE_MODE_MEMORY, 1);
    session = (ID3D12ShaderCacheSession *)0xdeadbeef;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12Device, (void **)&session);
    ok(hr == E_NOINTERFACE, "Got unexpected interface hr %#lx.\n", hr);
    ok(!session, "Got unexpected session %p.\n", session);

    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    value_size = sizeof(value);
    hr = ID3D12ShaderCacheSession_FindValue(session, NULL, 1, &value, &value_size);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12ShaderCacheSession_FindValue(session, &value, 0, &value, &value_size);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12ShaderCacheSession_FindValue(session, &value, sizeof(value), &value, NULL);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12ShaderCacheSession_StoreValue(session, NULL, 1, &value, sizeof(value));
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12ShaderCacheSession_StoreValue(session, &value, 0, &value, sizeof(value));
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12ShaderCacheSession_StoreValue(session, &value, sizeof(value), NULL, sizeof(value));
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12ShaderCacheSession_StoreValue(session, &value, sizeof(value), &value, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);

    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_control(ID3D12Device9 *device, unsigned int identifier)
{
    static const char key[] = "control";
    static const UINT expected = 0x12345678;
    D3D12_FEATURE_DATA_SHADER_CACHE support;
    D3D12_SHADER_CACHE_SESSION_DESC desc, other_desc;
    ID3D12ShaderCacheSession *other_session = NULL, *session, *session2;
    ID3D12Device9 *other_device9 = NULL;
    ID3D12Device *other_device = NULL;
    UINT value_size;
    HRESULT hr;

    if (strcmp(winetest_platform, "wine"))
    {
        skip("ShaderCacheControl requires Windows developer mode.\n");
        return;
    }

    support.SupportFlags = 0xdeadbeef;
    hr = ID3D12Device9_CheckFeatureSupport(device,
            D3D12_FEATURE_SHADER_CACHE, &support, sizeof(support));
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok((support.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_SHADER_CONTROL_CLEAR),
            "Shader cache clear support was not reported.\n");
    ok((support.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_SHADER_SESSION_DELETE),
            "Shader session deletion support was not reported.\n");
    ok(!(support.SupportFlags & (D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_DISK_CACHE
            | D3D12_SHADER_CACHE_SUPPORT_DRIVER_MANAGED_CACHE)),
            "Unexpected provider cache support %#x.\n", support.SupportFlags);

    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED, 0);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12Device9_ShaderCacheControl(device, 0, D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED,
            D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE | D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED
                    | D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED,
            D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE);
    ok(hr == S_OK, "Mixed cache kinds failed, hr %#lx.\n", hr);
    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED,
            D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR);
    ok(hr == E_NOTIMPL, "Got unexpected unsupported-kind hr %#lx.\n", hr);

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_MEMORY, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    hr = ID3D12ShaderCacheSession_StoreValue(session,
            key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store value, hr %#lx.\n", hr);

    if (get_shader_cache_device(&other_device, &other_device9))
    {
        init_shader_cache_desc(&other_desc, identifier + 10,
                D3D12_SHADER_CACHE_MODE_MEMORY, 1);
        hr = create_shader_cache_session(other_device9, &other_desc, &other_session);
        ok(hr == S_OK, "Failed to create second-device session, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = ID3D12ShaderCacheSession_StoreValue(other_session,
                    key, sizeof(key), &expected, sizeof(expected));
            ok(hr == S_OK, "Failed to store second-device value, hr %#lx.\n", hr);
        }
    }

    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED,
            D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE);
    ok(hr == S_OK, "Failed to disable shader caches, hr %#lx.\n", hr);
    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session, key, sizeof(key), NULL, &value_size);
    ok(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "Got unexpected hr %#lx.\n", hr);
    hr = ID3D12ShaderCacheSession_StoreValue(session,
            "disabled", sizeof("disabled"), &expected, sizeof(expected));
    ok(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "Got unexpected hr %#lx.\n", hr);
    if (other_session)
    {
        value_size = 0;
        hr = ID3D12ShaderCacheSession_FindValue(other_session,
                key, sizeof(key), NULL, &value_size);
        ok(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE,
                "Second-device cache was not disabled, hr %#lx.\n", hr);
    }
    session2 = (ID3D12ShaderCacheSession *)0xdeadbeef;
    desc.Identifier.Data1++;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session2);
    ok(hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "Got unexpected hr %#lx.\n", hr);
    ok(!session2, "Got unexpected session %p.\n", session2);

    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED,
            D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE);
    ok(hr == S_OK, "Failed to enable shader caches, hr %#lx.\n", hr);
    check_shader_cache_value(session, key, sizeof(key), expected);
    if (other_session)
        check_shader_cache_value(other_session, key, sizeof(key), expected);

    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED,
            D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR);
    ok(hr == S_OK, "Failed to clear shader caches, hr %#lx.\n", hr);
    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session, key, sizeof(key), NULL, &value_size);
    ok(hr == DXGI_ERROR_NOT_FOUND, "Got unexpected hr %#lx.\n", hr);
    if (other_session)
    {
        value_size = 0;
        hr = ID3D12ShaderCacheSession_FindValue(other_session,
                key, sizeof(key), NULL, &value_size);
        ok(hr == DXGI_ERROR_NOT_FOUND,
                "Second-device active cache was not cleared, hr %#lx.\n", hr);
    }

    ID3D12ShaderCacheSession_Release(session);
    if (other_session)
        ID3D12ShaderCacheSession_Release(other_session);
    if (other_device9)
        ID3D12Device9_Release(other_device9);
    if (other_device)
        ID3D12Device_Release(other_device);
}

static void check_shader_cache_eviction(ID3D12Device9 *device,
        D3D12_SHADER_CACHE_MODE mode, unsigned int identifier)
{
    static const char keys[][4] = {"one", "two", "tri"};
    static const BYTE values[3][16] = {{1}, {2}, {3}};
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    BYTE value[16];
    UINT value_size;
    HRESULT hr;
    unsigned int i, first_expected;

    init_shader_cache_desc(&desc, identifier, mode, 1);
    if (mode == D3D12_SHADER_CACHE_MODE_MEMORY)
    {
        desc.MaximumInMemoryCacheSizeBytes = 40;
        desc.MaximumInMemoryCacheEntries = 2;
    }
    else
    {
        desc.MaximumValueFileSizeBytes = sizeof(values[0]);
    }
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    for (i = 0; i < ARRAY_SIZE(keys); ++i)
    {
        hr = ID3D12ShaderCacheSession_StoreValue(session,
                keys[i], sizeof(keys[i]), values[i], sizeof(values[i]));
        ok(hr == S_OK, "%u: Failed to store value, hr %#lx.\n", i, hr);
    }

    first_expected = mode == D3D12_SHADER_CACHE_MODE_MEMORY ? 1 : 0;
    if (first_expected)
    {
        value_size = sizeof(value);
        hr = ID3D12ShaderCacheSession_FindValue(session,
                keys[0], sizeof(keys[0]), value, &value_size);
        ok(hr == DXGI_ERROR_NOT_FOUND, "Got unexpected hr %#lx.\n", hr);
    }
    for (i = first_expected; i < ARRAY_SIZE(keys); ++i)
    {
        memset(value, 0, sizeof(value));
        value_size = sizeof(value);
        hr = ID3D12ShaderCacheSession_FindValue(session,
                keys[i], sizeof(keys[i]), value, &value_size);
        ok(hr == S_OK, "%u: Failed to find value, hr %#lx.\n", i, hr);
        ok(!memcmp(value, values[i], sizeof(value)), "%u: Got unexpected value.\n", i);
    }

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_fuzz_inputs(ID3D12Device9 *device,
        D3D12_SHADER_CACHE_MODE mode, unsigned int identifier, unsigned int count)
{
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    BYTE key[68], value[68], result[68];
    UINT key_size, value_size, result_size;
    unsigned int i, j;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, mode, 1);
    desc.MaximumInMemoryCacheSizeBytes = 2 * 1024 * 1024;
    desc.MaximumInMemoryCacheEntries = count;
    desc.MaximumValueFileSizeBytes = 2 * 1024 * 1024;
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    for (i = 0; i < count; ++i)
    {
        key_size = 5 + i % 64;
        value_size = 1 + (i * 29) % 68;
        memset(key, i, key_size);
        memcpy(key, &i, sizeof(i));
        for (j = 0; j < value_size; ++j)
            value[j] = (BYTE)(i * 17 + j);
        hr = ID3D12ShaderCacheSession_StoreValue(session, key, key_size, value, value_size);
        ok(hr == S_OK, "%u: Failed to store fuzz value, hr %#lx.\n", i, hr);
    }

    for (i = 0; i < count; ++i)
    {
        key_size = 5 + i % 64;
        value_size = 1 + (i * 29) % 68;
        memset(key, i, key_size);
        memcpy(key, &i, sizeof(i));
        for (j = 0; j < value_size; ++j)
            value[j] = (BYTE)(i * 17 + j);
        memset(result, 0, sizeof(result));
        result_size = sizeof(result);
        hr = ID3D12ShaderCacheSession_FindValue(session, key, key_size, result, &result_size);
        ok(hr == S_OK, "%u: Failed to find fuzz value, hr %#lx.\n", i, hr);
        ok(result_size == value_size, "%u: Got size %u, expected %u.\n", i, result_size, value_size);
        ok(!memcmp(result, value, value_size), "%u: Got unexpected fuzz value.\n", i);
    }

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_live_version_conflict(ID3D12Device9 *device,
        D3D12_SHADER_CACHE_MODE mode, unsigned int identifier)
{
    D3D12_SHADER_CACHE_SESSION_DESC desc, desc2;
    ID3D12ShaderCacheSession *session, *session2;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, mode, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    init_shader_cache_desc(&desc2, identifier, mode, 2);
    session2 = (ID3D12ShaderCacheSession *)0xdeadbeef;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc2,
            &IID_ID3D12ShaderCacheSession, (void **)&session2);
    ok(hr == DXGI_ERROR_ALREADY_EXISTS, "Got unexpected hr %#lx.\n", hr);
    ok(!session2, "Got unexpected session %p.\n", session2);

    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_version_clear(ID3D12Device9 *device,
        D3D12_SHADER_CACHE_MODE mode, unsigned int identifier)
{
    static const char key[] = "versioned";
    static const UINT expected = 0x12345678;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    UINT value_size;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, mode, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    hr = ID3D12ShaderCacheSession_StoreValue(session, key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store shader cache value, hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_Release(session);

    init_shader_cache_desc(&desc, identifier, mode, 2);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session, key, sizeof(key), NULL, &value_size);
    ok(hr == DXGI_ERROR_NOT_FOUND, "Got unexpected hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);

    init_shader_cache_desc(&desc, identifier, mode, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session, key, sizeof(key), NULL, &value_size);
    ok(hr == DXGI_ERROR_NOT_FOUND, "Got unexpected hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_delete_lifetime(ID3D12Device9 *device,
        D3D12_SHADER_CACHE_MODE mode, unsigned int identifier)
{
    static const char key[] = "delete";
    static const UINT expected = 0x12345678;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session, *session2;
    UINT value_size;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, mode, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    hr = ID3D12ShaderCacheSession_StoreValue(session, key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store shader cache value, hr %#lx.\n", hr);

    hr = create_shader_cache_session(device, &desc, &session2);
    if (FAILED(hr))
    {
        ID3D12ShaderCacheSession_Release(session);
        return;
    }
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);

    check_shader_cache_value(session2, key, sizeof(key), expected);
    ID3D12ShaderCacheSession_Release(session2);

    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session, key, sizeof(key), NULL, &value_size);
    ok(hr == DXGI_ERROR_NOT_FOUND, "Got unexpected hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_quota_too_small(ID3D12Device9 *device,
        D3D12_SHADER_CACHE_MODE mode, unsigned int identifier)
{
    static const char key[] = "oversized";
    static const UINT expected = 0x12345678;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, mode, 1);
    if (mode == D3D12_SHADER_CACHE_MODE_MEMORY)
    {
        desc.MaximumInMemoryCacheSizeBytes = 1;
        desc.MaximumInMemoryCacheEntries = 8;
    }
    else
    {
        desc.MaximumValueFileSizeBytes = 1;
    }

    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    hr = ID3D12ShaderCacheSession_StoreValue(session, key, sizeof(key), &expected, sizeof(expected));
    todo_wine_if(hr != DXGI_ERROR_CACHE_FULL)
    ok(hr == DXGI_ERROR_CACHE_FULL, "Got unexpected hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_value_file_limit(ID3D12Device9 *device,
        D3D12_SHADER_CACHE_MODE mode, unsigned int identifier)
{
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    char key[32];
    UINT value, value_size;
    unsigned int i;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, mode, 1);
    desc.MaximumInMemoryCacheSizeBytes = 64 * 1024;
    desc.MaximumInMemoryCacheEntries = 256;
    desc.MaximumValueFileSizeBytes = mode == D3D12_SHADER_CACHE_MODE_DISK ? sizeof(value) : 1;
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;

    /* MaximumValueFileSizeBytes is per disk value, not an aggregate cache
     * quota, and is ignored for memory-mode storage. */
    for (i = 0; i < 128; ++i)
    {
        sprintf(key, "value-limit-%u", i);
        value = 0x5000 + i;
        hr = ID3D12ShaderCacheSession_StoreValue(session,
                key, strlen(key) + 1, &value, sizeof(value));
        ok(hr == S_OK, "%u: Failed to store value, hr %#lx.\n", i, hr);
    }
    for (i = 0; i < 128; ++i)
    {
        sprintf(key, "value-limit-%u", i);
        value = 0;
        value_size = sizeof(value);
        hr = ID3D12ShaderCacheSession_FindValue(session,
                key, strlen(key) + 1, &value, &value_size);
        ok(hr == S_OK, "%u: Failed to find value, hr %#lx.\n", i, hr);
        ok(value == 0x5000 + i, "%u: Got unexpected value %#x.\n", i, value);
    }

    if (mode == D3D12_SHADER_CACHE_MODE_DISK)
    {
        UINT64 oversized = 0;

        hr = ID3D12ShaderCacheSession_StoreValue(session,
                "too-large", sizeof("too-large"), &oversized, sizeof(oversized));
        ok(hr == DXGI_ERROR_CACHE_FULL, "Got unexpected oversized store hr %#lx.\n", hr);
    }

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static void test_shader_cache_memory_session(ID3D12Device9 *device)
{
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    HRESULT hr;

    init_shader_cache_desc(&desc, 1, D3D12_SHADER_CACHE_MODE_MEMORY, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    check_shader_cache_defaults(session, &desc);
    ID3D12ShaderCacheSession_Release(session);

    check_shader_cache_common_contract(device, D3D12_SHADER_CACHE_MODE_MEMORY, 2);
    check_shader_cache_process_identity(device, 14);
    check_shader_cache_live_version_conflict(device, D3D12_SHADER_CACHE_MODE_MEMORY, 3);
    check_shader_cache_version_clear(device, D3D12_SHADER_CACHE_MODE_MEMORY, 4);
    check_shader_cache_delete_lifetime(device, D3D12_SHADER_CACHE_MODE_MEMORY, 5);
    check_shader_cache_quota_too_small(device, D3D12_SHADER_CACHE_MODE_MEMORY, 6);
    check_shader_cache_value_file_limit(device, D3D12_SHADER_CACHE_MODE_MEMORY, 15);
    check_shader_cache_thread_race(device, D3D12_SHADER_CACHE_MODE_MEMORY, 7, FALSE);
    check_shader_cache_thread_race(device, D3D12_SHADER_CACHE_MODE_MEMORY, 8, TRUE);
    check_shader_cache_invalid_arguments(device, 9);
    check_shader_cache_eviction(device, D3D12_SHADER_CACHE_MODE_MEMORY, 10);
    check_shader_cache_fuzz_inputs(device, D3D12_SHADER_CACHE_MODE_MEMORY, 11, 256);
    check_shader_cache_control(device, 13);
    if (winetest_interactive)
        check_shader_cache_fuzz_inputs(device, D3D12_SHADER_CACHE_MODE_MEMORY, 12, 10000);
}

static void check_shader_cache_disk_persistence(ID3D12Device9 *device, unsigned int identifier)
{
    static const char key[] = "persistent";
    static const UINT expected = 0x12345678;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_DISK, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    hr = ID3D12ShaderCacheSession_StoreValue(session, key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store shader cache value, hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_Release(session);

    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    check_shader_cache_value(session, key, sizeof(key), expected);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static void check_shader_cache_driver_versioned(ID3D12Device9 *device, unsigned int identifier)
{
    static const char key[] = "driver-versioned";
    static const UINT expected = 0x76543210;
    D3D12_SHADER_CACHE_SESSION_DESC desc, actual;
    ID3D12ShaderCacheSession *session;
    HRESULT hr;

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_DISK, 1);
    desc.Flags |= D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED;
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    actual = ID3D12ShaderCacheSession_GetDesc(session);
    ok(actual.Flags == desc.Flags, "Got unexpected driver-versioned flags %#x.\n", actual.Flags);
    hr = ID3D12ShaderCacheSession_StoreValue(session,
            key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store driver-versioned value, hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_Release(session);

    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    check_shader_cache_value(session, key, sizeof(key), expected);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static BOOL is_shader_cache_hex_name(const char *name)
{
    unsigned int i;

    if (strlen(name) != 64)
        return FALSE;
    for (i = 0; i < 64; ++i)
    {
        if (!((name[i] >= '0' && name[i] <= '9')
                || (name[i] >= 'a' && name[i] <= 'f')))
            return FALSE;
    }
    return TRUE;
}

static void check_shader_cache_root_namespace_quota(ID3D12Device9 *device,
        unsigned int identifier)
{
    static const char active_key[] = "active-root-namespace";
    static const UINT active_value = 0x2468ace0;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *active, *session;
    WIN32_FIND_DATAA data;
    char key[32];
    HANDLE find;
    HRESULT hr;
    unsigned int i, namespace_count = 0;
    UINT value;

    if (strcmp(winetest_platform, "wine"))
        return;

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_DISK, 1);
    hr = create_shader_cache_session(device, &desc, &active);
    if (FAILED(hr))
        return;
    hr = ID3D12ShaderCacheSession_StoreValue(active, active_key,
            sizeof(active_key), &active_value, sizeof(active_value));
    ok(hr == S_OK, "Failed to store active namespace value, hr %#lx.\n", hr);

    for (i = 0; i < 136; ++i)
    {
        init_shader_cache_desc(&desc, identifier + 1 + i, D3D12_SHADER_CACHE_MODE_DISK, 1);
        hr = create_shader_cache_session(device, &desc, &session);
        ok(hr == S_OK, "%u: Failed to create quota namespace, hr %#lx.\n", i, hr);
        if (FAILED(hr))
            continue;
        sprintf(key, "root-quota-%u", i);
        value = i;
        hr = ID3D12ShaderCacheSession_StoreValue(session,
                key, strlen(key) + 1, &value, sizeof(value));
        ok(hr == S_OK, "%u: Failed to store quota value, hr %#lx.\n", i, hr);
        ID3D12ShaderCacheSession_Release(session);
    }

    find = FindFirstFileA("vkd3d-cache\\*", &data);
    ok(find != INVALID_HANDLE_VALUE, "Failed to enumerate cache root, error %lu.\n", GetLastError());
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    && is_shader_cache_hex_name(data.cFileName))
                ++namespace_count;
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    ok(namespace_count <= 128, "Root retained %u namespaces.\n", namespace_count);
    check_shader_cache_value(active, active_key, sizeof(active_key), active_value);

    ID3D12ShaderCacheSession_SetDeleteOnDestroy(active);
    ID3D12ShaderCacheSession_Release(active);
}

static void check_shader_cache_disk_control_scope(ID3D12Device9 *device, unsigned int identifier)
{
    static const char key[] = "clear-scope";
    static const UINT expected = 0x12345678;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    UINT value_size;
    HRESULT hr;

    if (strcmp(winetest_platform, "wine"))
        return;

    init_shader_cache_desc(&desc, identifier, D3D12_SHADER_CACHE_MODE_DISK, 1);
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    hr = ID3D12ShaderCacheSession_StoreValue(session,
            key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store working-directory value, hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_Release(session);

    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED,
            D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR);
    ok(hr == S_OK, "Failed to clear shader caches, hr %#lx.\n", hr);

    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    check_shader_cache_value(session, key, sizeof(key), expected);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);

    init_shader_cache_desc(&desc, identifier + 1, D3D12_SHADER_CACHE_MODE_DISK, 1);
    desc.Flags = D3D12_SHADER_CACHE_FLAG_NONE;
    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    hr = ID3D12ShaderCacheSession_StoreValue(session,
            key, sizeof(key), &expected, sizeof(expected));
    ok(hr == S_OK, "Failed to store default disk value, hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_Release(session);

    hr = ID3D12Device9_ShaderCacheControl(device,
            D3D12_SHADER_CACHE_KIND_FLAG_APPLICATION_MANAGED,
            D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR);
    ok(hr == S_OK, "Failed to clear default shader caches, hr %#lx.\n", hr);

    hr = create_shader_cache_session(device, &desc, &session);
    if (FAILED(hr))
        return;
    value_size = 0;
    hr = ID3D12ShaderCacheSession_FindValue(session, key, sizeof(key), NULL, &value_size);
    ok(hr == DXGI_ERROR_NOT_FOUND, "Got unexpected hr %#lx.\n", hr);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);
}

static void test_shader_cache_disk_session(ID3D12Device9 *device)
{
    struct shader_cache_working_dir working_dir;
    D3D12_SHADER_CACHE_SESSION_DESC desc;
    ID3D12ShaderCacheSession *session;
    HRESULT hr;

    if (!shader_cache_working_dir_init(&working_dir))
        return;

    init_shader_cache_desc(&desc, 100, D3D12_SHADER_CACHE_MODE_DISK, 1);
    session = NULL;
    hr = ID3D12Device9_CreateShaderCacheSession(device, &desc,
            &IID_ID3D12ShaderCacheSession, (void **)&session);
    todo_wine_if(hr == DXGI_ERROR_UNSUPPORTED)
    ok(hr == S_OK, "Failed to create disk shader cache session, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        shader_cache_working_dir_cleanup(&working_dir);
        return;
    }
    check_shader_cache_defaults(session, &desc);
    ID3D12ShaderCacheSession_SetDeleteOnDestroy(session);
    ID3D12ShaderCacheSession_Release(session);

    check_shader_cache_common_contract(device, D3D12_SHADER_CACHE_MODE_DISK, 101);
    check_shader_cache_disk_persistence(device, 102);
    check_shader_cache_driver_versioned(device, 115);
    check_shader_cache_root_namespace_quota(device, 120);
    check_shader_cache_live_version_conflict(device, D3D12_SHADER_CACHE_MODE_DISK, 103);
    check_shader_cache_version_clear(device, D3D12_SHADER_CACHE_MODE_DISK, 104);
    check_shader_cache_delete_lifetime(device, D3D12_SHADER_CACHE_MODE_DISK, 105);
    check_shader_cache_quota_too_small(device, D3D12_SHADER_CACHE_MODE_DISK, 106);
    check_shader_cache_value_file_limit(device, D3D12_SHADER_CACHE_MODE_DISK, 113);
    check_shader_cache_thread_race(device, D3D12_SHADER_CACHE_MODE_DISK, 107, FALSE);
    check_shader_cache_thread_race(device, D3D12_SHADER_CACHE_MODE_DISK, 108, TRUE);
    check_shader_cache_eviction(device, D3D12_SHADER_CACHE_MODE_DISK, 109);
    check_shader_cache_fuzz_inputs(device, D3D12_SHADER_CACHE_MODE_DISK, 110, 256);
    check_shader_cache_disk_control_scope(device, 112);
    if (winetest_interactive)
        check_shader_cache_fuzz_inputs(device, D3D12_SHADER_CACHE_MODE_DISK, 111, 10000);

    shader_cache_working_dir_cleanup(&working_dir);
    check_shader_cache_reparse_root(device, 114);
}

static void test_shader_cache_session(void)
{
    ID3D12Device9 *device9;
    ID3D12Device *device;
    ULONG refcount;

    if (!get_shader_cache_device(&device, &device9))
        return;

    test_shader_cache_memory_session(device9);
    test_shader_cache_disk_session(device9);

    ID3D12Device9_Release(device9);
    refcount = ID3D12Device_Release(device);
    ok(!refcount, "Device has %u references left.\n", (unsigned int)refcount);
}

static void test_copy_rtv_dsv_descriptors(void)
{
    static const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
    static const UINT range_sizes[] = {0, 1};
    D3D12_CPU_DESCRIPTOR_HANDLE dst_ranges[ARRAY_SIZE(range_sizes)];
    D3D12_CPU_DESCRIPTOR_HANDLE src_ranges[ARRAY_SIZE(range_sizes)];
    D3D12_CPU_DESCRIPTOR_HANDLE dsv[2];
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    D3D12_CLEAR_VALUE clear_value;
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    ID3D12DescriptorHeap *dsv_heap;
    ID3D12Resource *depth;
    struct test_context context;
    unsigned int descriptor_size;
    HRESULT hr;

    if (!init_test_context(&context, NULL))
        return;

    create_render_target(&context);
    dst_ranges[0] = dst_ranges[1] = context.rtv[1];
    src_ranges[0] = src_ranges[1] = context.rtv[0];
    ID3D12Device_CopyDescriptors(context.device, ARRAY_SIZE(dst_ranges), dst_ranges, range_sizes,
            ARRAY_SIZE(src_ranges), src_ranges, range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12GraphicsCommandList_ClearRenderTargetView(context.list[0], context.rtv[1], green, 0, NULL);
    transition_sub_resource_state(context.list[0], context.render_target[0], 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target[0], 0,
            context.queue, context.list[0], 0xff00ff00, 0);

    memset(&rtv_desc, 0, sizeof(rtv_desc));
    rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateRenderTargetView(context.device, NULL, &rtv_desc, context.rtv[0]);
    ID3D12Device_CopyDescriptorsSimple(context.device, 1, context.rtv[1], context.rtv[0],
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    reset_command_list(&context, 0);
    ID3D12GraphicsCommandList_ClearRenderTargetView(context.list[0], context.rtv[1], green, 0, NULL);
    hr = ID3D12GraphicsCommandList_Close(context.list[0]);
    ok(hr == S_OK, "Failed to close after a null RTV clear, hr %#lx.\n", hr);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_desc.NumDescriptors = ARRAY_SIZE(dsv);
    hr = ID3D12Device_CreateDescriptorHeap(context.device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
    ok(hr == S_OK, "Failed to create DSV heap, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        destroy_test_context(&context);
        return;
    }

    dsv[0] = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
    descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
            context.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    dsv[1] = dsv[0];
    dsv[1].ptr += descriptor_size;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    memset(&resource_desc, 0, sizeof(resource_desc));
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_D32_FLOAT;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    memset(&clear_value, 0, sizeof(clear_value));
    clear_value.Format = resource_desc.Format;
    clear_value.DepthStencil.Depth = 1.0f;

    hr = ID3D12Device_CreateCommittedResource(context.device, &heap_properties,
            D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear_value, &IID_ID3D12Resource, (void **)&depth);
    ok(hr == S_OK, "Failed to create depth resource, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ID3D12Device_CreateDepthStencilView(context.device, depth, NULL, dsv[0]);
        dst_ranges[0] = dst_ranges[1] = dsv[1];
        src_ranges[0] = src_ranges[1] = dsv[0];
        ID3D12Device_CopyDescriptors(context.device, ARRAY_SIZE(dst_ranges), dst_ranges, range_sizes,
                ARRAY_SIZE(src_ranges), src_ranges, range_sizes, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        reset_command_list(&context, 0);
        ID3D12GraphicsCommandList_ClearDepthStencilView(context.list[0], dsv[1],
                D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, NULL);
        transition_sub_resource_state(context.list[0], depth, 0,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_uint(depth, 0, context.queue, context.list[0], 0x3e800000, 0);

        memset(&dsv_desc, 0, sizeof(dsv_desc));
        dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        ID3D12Device_CreateDepthStencilView(context.device, NULL, &dsv_desc, dsv[0]);
        ID3D12Device_CopyDescriptorsSimple(context.device, 1, dsv[1], dsv[0],
                D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        reset_command_list(&context, 0);
        ID3D12GraphicsCommandList_ClearDepthStencilView(context.list[0], dsv[1],
                D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, NULL);
        hr = ID3D12GraphicsCommandList_Close(context.list[0]);
        ok(hr == S_OK, "Failed to close after a null DSV clear, hr %#lx.\n", hr);

        ID3D12Resource_Release(depth);
    }

    ID3D12DescriptorHeap_Release(dsv_heap);
    destroy_test_context(&context);
}

static void test_command_list_clear_state(void)
{
    static const float red[] = {1.0f, 0.0f, 0.0f, 1.0f};
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const DWORD ps_code[] =
    {
#if 0
        float4 color;

        float4 main() : SV_Target
        {
            return color;
        }
#endif
        0x43425844, 0x69e703c1, 0xf0db50aa, 0x9af7ae76, 0x623b93f7, 0x00000001, 0x000000bc, 0x00000003,
        0x0000002c, 0x0000003c, 0x00000070, 0x4e475349, 0x00000008, 0x00000000, 0x00000008, 0x4e47534f,
        0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003, 0x00000000,
        0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x00000044, 0x00000050, 0x00000011,
        0x0100086a, 0x04000059, 0x00208e46, 0x00000000, 0x00000001, 0x03000065, 0x001020f2, 0x00000000,
        0x06000036, 0x001020f2, 0x00000000, 0x00208e46, 0x00000000, 0x00000000, 0x0100003e,
    };
    static const D3D12_SHADER_BYTECODE ps = {ps_code, sizeof(ps_code)};
    ID3D12PipelineState *green_pipeline, *red_pipeline;
    ID3D12GraphicsCommandList *bundle = NULL;
    ID3D12CommandAllocator *bundle_allocator = NULL;
    ID3D12GraphicsCommandList *command_list;
    struct test_context_desc desc;
    struct test_context context;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.no_pipeline = TRUE;
    if (!init_test_context(&context, &desc))
        return;

    create_render_target(&context);
    command_list = context.list[0];

    red_pipeline = create_pipeline_state(context.device, context.root_signature,
            DXGI_FORMAT_B8G8R8A8_UNORM, &ps);
    green_pipeline = create_pipeline_state(context.device, context.root_signature,
            DXGI_FORMAT_B8G8R8A8_UNORM, NULL);

    ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv[0], white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv[0], FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, red_pipeline);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, ARRAY_SIZE(red), red, 0);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    ID3D12GraphicsCommandList_ClearState(command_list, green_pipeline);

    ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv[0], FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    transition_sub_resource_state(command_list, context.render_target[0], 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target[0], 0, context.queue, command_list, 0xff00ff00, 0);

    hr = ID3D12Device_CreateCommandAllocator(context.device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&bundle_allocator);
    ok(hr == S_OK, "Failed to create bundle allocator, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = ID3D12Device_CreateCommandList(context.device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
                bundle_allocator, red_pipeline, &IID_ID3D12GraphicsCommandList, (void **)&bundle);
        ok(hr == S_OK, "Failed to create bundle, hr %#lx.\n", hr);
    }
    if (bundle)
    {
        ID3D12GraphicsCommandList_ClearState(bundle, green_pipeline);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(bundle, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(bundle, 0, ARRAY_SIZE(red), red, 0);
        ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
        hr = ID3D12GraphicsCommandList_Close(bundle);
        ok(hr == E_FAIL, "Got unexpected hr %#lx after ClearState() on a bundle.\n", hr);

        hr = ID3D12GraphicsCommandList_Reset(bundle, bundle_allocator, red_pipeline);
        ok(hr == S_OK, "Failed to reset bundle, hr %#lx.\n", hr);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(bundle, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(bundle, 0, ARRAY_SIZE(red), red, 0);
        ID3D12GraphicsCommandList_DrawInstanced(bundle, 3, 1, 0, 0);
        hr = ID3D12GraphicsCommandList_Close(bundle);
        ok(hr == S_OK, "Failed to close bundle, hr %#lx.\n", hr);

        reset_command_list(&context, 0);
        transition_sub_resource_state(command_list, context.render_target[0], 0,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, context.rtv[0], white, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &context.rtv[0], FALSE, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_ExecuteBundle(command_list, bundle);
        transition_sub_resource_state(command_list, context.render_target[0], 0,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check_sub_resource_uint(context.render_target[0], 0, context.queue, command_list, 0xffff0000, 0);
    }

    if (bundle)
        ID3D12GraphicsCommandList_Release(bundle);
    if (bundle_allocator)
        ID3D12CommandAllocator_Release(bundle_allocator);
    ID3D12PipelineState_Release(green_pipeline);
    ID3D12PipelineState_Release(red_pipeline);
    destroy_test_context(&context);
}

static void test_resource_aliasing_barriers(void)
{
    static const UINT expected_values[] =
    {
        0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00, 0x13579bdf,
    };
    ID3D12Resource *before[ARRAY_SIZE(expected_values)] = {0};
    ID3D12Resource *after[ARRAY_SIZE(expected_values)] = {0};
    ID3D12Resource *upload = NULL, *readback = NULL, *source;
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info;
    D3D12_RESOURCE_BARRIER barrier;
    D3D12_RESOURCE_DESC resource_desc;
    struct test_context_desc context_desc;
    ID3D12GraphicsCommandList *command_list;
    struct test_context context;
    D3D12_HEAP_DESC heap_desc;
    D3D12_RANGE range;
    ID3D12Heap *heap = NULL;
    UINT64 allocation_stride;
    UINT *mapped_data;
    unsigned int i;
    HRESULT hr;

    memset(&context_desc, 0, sizeof(context_desc));
    context_desc.no_render_target = TRUE;
    if (!init_test_context(&context, &context_desc))
        return;
    command_list = context.list[0];

    memset(&resource_desc, 0, sizeof(resource_desc));
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = sizeof(UINT);
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    allocation_info = ID3D12Device_GetResourceAllocationInfo(context.device, 0, 1, &resource_desc);
    ok(allocation_info.SizeInBytes && allocation_info.SizeInBytes != ~(UINT64)0,
            "Got invalid allocation size %#I64x.\n", allocation_info.SizeInBytes);
    ok(allocation_info.Alignment, "Got invalid allocation alignment %#I64x.\n", allocation_info.Alignment);
    if (!allocation_info.SizeInBytes || allocation_info.SizeInBytes == ~(UINT64)0
            || !allocation_info.Alignment)
        goto cleanup;
    ok(allocation_info.SizeInBytes <= ~(UINT64)0 - (allocation_info.Alignment - 1),
            "Allocation size %#I64x and alignment %#I64x overflow.\n",
            allocation_info.SizeInBytes, allocation_info.Alignment);
    if (allocation_info.SizeInBytes > ~(UINT64)0 - (allocation_info.Alignment - 1))
        goto cleanup;
    allocation_stride = (allocation_info.SizeInBytes + allocation_info.Alignment - 1)
            & ~(allocation_info.Alignment - 1);
    ok(allocation_stride <= ~(UINT64)0 / ARRAY_SIZE(expected_values),
            "Allocation stride %#I64x overflows the heap size.\n", allocation_stride);
    if (allocation_stride > ~(UINT64)0 / ARRAY_SIZE(expected_values))
        goto cleanup;

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.SizeInBytes = allocation_stride * ARRAY_SIZE(expected_values);
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    hr = ID3D12Device_CreateHeap(context.device, &heap_desc, &IID_ID3D12Heap, (void **)&heap);
    ok(hr == S_OK, "Failed to create heap, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto cleanup;

    for (i = 0; i < ARRAY_SIZE(expected_values); ++i)
    {
        hr = ID3D12Device_CreatePlacedResource(context.device, heap, i * allocation_stride,
                &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
                &IID_ID3D12Resource, (void **)&before[i]);
        ok(hr == S_OK, "Failed to create resource before %u, hr %#lx.\n", i, hr);
        if (FAILED(hr))
            goto cleanup;
        hr = ID3D12Device_CreatePlacedResource(context.device, heap, i * allocation_stride,
                &resource_desc, D3D12_RESOURCE_STATE_COMMON, NULL,
                &IID_ID3D12Resource, (void **)&after[i]);
        ok(hr == S_OK, "Failed to create resource after %u, hr %#lx.\n", i, hr);
        if (FAILED(hr))
            goto cleanup;
    }

    upload = create_buffer(context.device, D3D12_HEAP_TYPE_UPLOAD, sizeof(expected_values),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = create_readback_buffer(context.device, sizeof(expected_values));

    range.Begin = range.End = 0;
    hr = ID3D12Resource_Map(upload, 0, &range, (void **)&mapped_data);
    ok(hr == S_OK, "Failed to map upload buffer, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto cleanup;
    memcpy(mapped_data, expected_values, sizeof(expected_values));
    range.End = sizeof(expected_values);
    ID3D12Resource_Unmap(upload, 0, &range);

    for (i = 0; i < ARRAY_SIZE(expected_values); ++i)
    {
        ID3D12GraphicsCommandList_CopyBufferRegion(command_list,
                before[i], 0, upload, i * sizeof(UINT), sizeof(UINT));

        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        source = after[i];
        switch (i)
        {
            case 0:
                barrier.Aliasing.pResourceBefore = before[i];
                barrier.Aliasing.pResourceAfter = after[i];
                break;

            case 1:
                barrier.Aliasing.pResourceBefore = NULL;
                barrier.Aliasing.pResourceAfter = after[i];
                break;

            case 2:
                barrier.Aliasing.pResourceBefore = before[i];
                barrier.Aliasing.pResourceAfter = NULL;
                break;

            case 3:
                barrier.Aliasing.pResourceBefore = NULL;
                barrier.Aliasing.pResourceAfter = NULL;
                break;

            default:
                barrier.Aliasing.pResourceBefore = before[i];
                barrier.Aliasing.pResourceAfter = before[i];
                source = before[i];
                break;
        }
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
        /* Contents are undefined after activating an alias. Reinitialize the
         * selected resource before observing it. */
        ID3D12GraphicsCommandList_CopyBufferRegion(command_list,
                source, 0, upload, i * sizeof(UINT), sizeof(UINT));
        transition_sub_resource_state(command_list, source, 0,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(command_list,
                readback, i * sizeof(UINT), source, 0, sizeof(UINT));
    }

    hr = ID3D12GraphicsCommandList_Close(command_list);
    ok(hr == S_OK, "Failed to close command list, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto cleanup;
    exec_command_list(context.queue, command_list);
    wait_queue_idle(context.device, context.queue);

    range.Begin = 0;
    range.End = sizeof(expected_values);
    hr = ID3D12Resource_Map(readback, 0, &range, (void **)&mapped_data);
    ok(hr == S_OK, "Failed to map readback buffer, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_values); ++i)
            ok(mapped_data[i] == expected_values[i], "Test %u: Got %#x, expected %#x.\n",
                    i, mapped_data[i], expected_values[i]);
        range.Begin = range.End = 0;
        ID3D12Resource_Unmap(readback, 0, &range);
    }

cleanup:
    if (readback)
        ID3D12Resource_Release(readback);
    if (upload)
        ID3D12Resource_Release(upload);
    for (i = 0; i < ARRAY_SIZE(expected_values); ++i)
    {
        if (after[i])
            ID3D12Resource_Release(after[i]);
        if (before[i])
            ID3D12Resource_Release(before[i]);
    }
    if (heap)
        ID3D12Heap_Release(heap);
    destroy_test_context(&context);
}

static void test_texture_aliasing_barrier(void)
{
    static const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
    static const float red[] = {1.0f, 0.0f, 0.0f, 1.0f};
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info;
    ID3D12Resource *before = NULL, *after = NULL;
    D3D12_RESOURCE_BARRIER barrier;
    D3D12_RESOURCE_DESC resource_desc;
    struct test_context_desc context_desc;
    struct test_context context;
    D3D12_HEAP_DESC heap_desc;
    ID3D12Heap *heap = NULL;
    HRESULT hr;

    memset(&context_desc, 0, sizeof(context_desc));
    context_desc.no_pipeline = TRUE;
    if (!init_test_context(&context, &context_desc))
        return;

    memset(&resource_desc, 0, sizeof(resource_desc));
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = 32;
    resource_desc.Height = 32;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    allocation_info = ID3D12Device_GetResourceAllocationInfo(context.device, 0, 1, &resource_desc);
    ok(allocation_info.SizeInBytes && allocation_info.SizeInBytes != ~(UINT64)0,
            "Got invalid texture allocation size %#I64x.\n", allocation_info.SizeInBytes);
    if (!allocation_info.SizeInBytes || allocation_info.SizeInBytes == ~(UINT64)0)
        goto done;

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.SizeInBytes = allocation_info.SizeInBytes;
    heap_desc.Alignment = allocation_info.Alignment;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    hr = ID3D12Device_CreateHeap(context.device, &heap_desc, &IID_ID3D12Heap, (void **)&heap);
    ok(hr == S_OK, "Failed to create texture heap, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    hr = ID3D12Device_CreatePlacedResource(context.device, heap, 0, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
            &IID_ID3D12Resource, (void **)&before);
    ok(hr == S_OK, "Failed to create the first aliased texture, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;
    hr = ID3D12Device_CreatePlacedResource(context.device, heap, 0, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
            &IID_ID3D12Resource, (void **)&after);
    ok(hr == S_OK, "Failed to create the second aliased texture, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    ID3D12Device_CreateRenderTargetView(context.device, before, NULL, context.rtv[0]);
    ID3D12Device_CreateRenderTargetView(context.device, after, NULL, context.rtv[1]);

    ID3D12GraphicsCommandList_ClearRenderTargetView(context.list[0], context.rtv[0], red, 0, NULL);
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Aliasing.pResourceBefore = before;
    barrier.Aliasing.pResourceAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(context.list[0], 1, &barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(context.list[0], context.rtv[1], green, 0, NULL);
    transition_sub_resource_state(context.list[0], after, 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(after, 0, context.queue, context.list[0], 0xff00ff00, 0);

    reset_command_list(&context, 0);
    barrier.Aliasing.pResourceBefore = after;
    barrier.Aliasing.pResourceAfter = before;
    ID3D12GraphicsCommandList_ResourceBarrier(context.list[0], 1, &barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(context.list[0], context.rtv[0], red, 0, NULL);
    transition_sub_resource_state(context.list[0], before, 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(before, 0, context.queue, context.list[0], 0xffff0000, 0);

done:
    if (after)
        ID3D12Resource_Release(after);
    if (before)
        ID3D12Resource_Release(before);
    if (heap)
        ID3D12Heap_Release(heap);
    destroy_test_context(&context);
}

static void test_execute_indirect(void)
{
    static const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    enum { argument_offset = sizeof(UINT), argument_stride = 32, command_count = 2 };
    BYTE argument_data[argument_offset + argument_stride * command_count];
    D3D12_COMMAND_SIGNATURE_DESC signature_desc;
    D3D12_INDIRECT_ARGUMENT_DESC argument_desc;
    D3D12_DRAW_ARGUMENTS draw_arguments;
    ID3D12CommandSignature *signature = NULL;
    ID3D12Resource *argument_buffer = NULL;
    struct test_context context;
    D3D12_RANGE range;
    void *mapped_data;
    HRESULT hr;

    if (!init_test_context(&context, NULL))
        return;

    create_render_target(&context);

    memset(&argument_desc, 0, sizeof(argument_desc));
    argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    memset(&signature_desc, 0, sizeof(signature_desc));
    signature_desc.ByteStride = argument_stride;
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &argument_desc;
    hr = ID3D12Device_CreateCommandSignature(context.device, &signature_desc, NULL,
            &IID_ID3D12CommandSignature, (void **)&signature);
    ok(hr == S_OK, "Failed to create command signature, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        destroy_test_context(&context);
        return;
    }

    memset(argument_data, 0, sizeof(argument_data));
    memcpy(argument_data, &(const UINT){0xdeadbeef}, sizeof(UINT));
    memset(&draw_arguments, 0, sizeof(draw_arguments));
    memcpy(argument_data + argument_offset, &draw_arguments, sizeof(draw_arguments));
    draw_arguments.VertexCountPerInstance = 3;
    draw_arguments.InstanceCount = 1;
    memcpy(argument_data + argument_offset + argument_stride, &draw_arguments, sizeof(draw_arguments));

    argument_buffer = create_buffer(context.device, D3D12_HEAP_TYPE_UPLOAD,
            sizeof(argument_data), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!argument_buffer)
        goto done;

    range.Begin = 0;
    range.End = 0;
    hr = ID3D12Resource_Map(argument_buffer, 0, &range, &mapped_data);
    ok(hr == S_OK, "Failed to map indirect argument buffer, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;
    memcpy(mapped_data, argument_data, sizeof(argument_data));
    range.End = sizeof(argument_data);
    ID3D12Resource_Unmap(argument_buffer, 0, &range);

    ID3D12GraphicsCommandList_ClearRenderTargetView(context.list[0], context.rtv[0], white, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(context.list[0], 1, &context.rtv[0], FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(context.list[0], context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(context.list[0], context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(context.list[0], D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_RSSetViewports(context.list[0], 1, &context.viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(context.list[0], 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_ExecuteIndirect(context.list[0], signature, command_count,
            argument_buffer, argument_offset, NULL, 0);
    transition_sub_resource_state(context.list[0], context.render_target[0], 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    check_sub_resource_uint(context.render_target[0], 0,
            context.queue, context.list[0], 0xff00ff00, 0);

done:
    if (argument_buffer)
        ID3D12Resource_Release(argument_buffer);
    ID3D12CommandSignature_Release(signature);
    destroy_test_context(&context);
}

static void test_write_buffer_immediate(void)
{
    static const UINT expected[] = {0x12345678, 0x90abcdef};
    static const UINT bundle_expected[] = {0x0badc0de, 0xfeedface};
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameters[ARRAY_SIZE(expected)];
    D3D12_WRITEBUFFERIMMEDIATE_MODE modes[ARRAY_SIZE(expected)];
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3;
    ID3D12GraphicsCommandList2 *command_list2, *bundle2 = NULL;
    ID3D12GraphicsCommandList *bundle = NULL;
    ID3D12CommandAllocator *bundle_allocator = NULL;
    ID3D12Resource *buffer, *readback;
    struct test_context_desc desc;
    struct test_context context;
    D3D12_RANGE range;
    UINT *data;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.no_render_target = TRUE;
    if (!init_test_context(&context, &desc))
        return;

    memset(&options3, 0, sizeof(options3));
    hr = ID3D12Device_CheckFeatureSupport(context.device,
            D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3));
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr) || !(options3.WriteBufferImmediateSupportFlags
            & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT))
    {
        win_skip("Direct command lists do not support WriteBufferImmediate().\n");
        destroy_test_context(&context);
        return;
    }

    hr = ID3D12GraphicsCommandList_QueryInterface(context.list[0],
            &IID_ID3D12GraphicsCommandList2, (void **)&command_list2);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        destroy_test_context(&context);
        return;
    }

    buffer = create_buffer(context.device, D3D12_HEAP_TYPE_DEFAULT, sizeof(expected),
            D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    readback = create_readback_buffer(context.device, sizeof(expected));
    if (!buffer || !readback)
        goto done;

    parameters[0].Dest = ID3D12Resource_GetGPUVirtualAddress(buffer);
    parameters[0].Value = expected[0];
    parameters[1].Dest = parameters[0].Dest + sizeof(UINT);
    parameters[1].Value = expected[1];
    modes[0] = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
    modes[1] = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    ID3D12GraphicsCommandList2_WriteBufferImmediate(command_list2,
            ARRAY_SIZE(parameters), parameters, modes);
    transition_sub_resource_state(context.list[0], buffer, 0,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(context.list[0], readback, 0,
            buffer, 0, sizeof(expected));

    hr = ID3D12GraphicsCommandList_Close(context.list[0]);
    ok(hr == S_OK, "Failed to close command list, hr %#lx.\n", hr);
    exec_command_list(context.queue, context.list[0]);
    wait_queue_idle(context.device, context.queue);

    range.Begin = 0;
    range.End = sizeof(expected);
    hr = ID3D12Resource_Map(readback, 0, &range, (void **)&data);
    ok(hr == S_OK, "Failed to map readback buffer, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(data[0] == expected[0], "Got %#x, expected %#x.\n", data[0], expected[0]);
        ok(data[1] == expected[1], "Got %#x, expected %#x.\n", data[1], expected[1]);
        range.Begin = range.End = 0;
        ID3D12Resource_Unmap(readback, 0, &range);
    }

    if (!(options3.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_BUNDLE))
    {
        win_skip("Bundles do not support WriteBufferImmediate().\n");
        goto done;
    }

    hr = ID3D12Device_CreateCommandAllocator(context.device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&bundle_allocator);
    ok(hr == S_OK, "Failed to create bundle allocator, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;
    hr = ID3D12Device_CreateCommandList(context.device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            bundle_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&bundle);
    ok(hr == S_OK, "Failed to create bundle, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;
    hr = ID3D12GraphicsCommandList_QueryInterface(bundle,
            &IID_ID3D12GraphicsCommandList2, (void **)&bundle2);
    ok(hr == S_OK, "Failed to get ID3D12GraphicsCommandList2 for bundle, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    parameters[0].Value = bundle_expected[0];
    parameters[1].Value = bundle_expected[1];
    ID3D12GraphicsCommandList2_WriteBufferImmediate(bundle2,
            ARRAY_SIZE(parameters), parameters, NULL);
    hr = ID3D12GraphicsCommandList_Close(bundle);
    ok(hr == S_OK, "Failed to close WriteBufferImmediate() bundle, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    reset_command_list(&context, 0);
    transition_sub_resource_state(context.list[0], buffer, 0,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_ExecuteBundle(context.list[0], bundle);
    transition_sub_resource_state(context.list[0], buffer, 0,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(context.list[0], readback, 0,
            buffer, 0, sizeof(bundle_expected));
    hr = ID3D12GraphicsCommandList_Close(context.list[0]);
    ok(hr == S_OK, "Failed to close command list after bundle, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;
    exec_command_list(context.queue, context.list[0]);
    wait_queue_idle(context.device, context.queue);

    range.Begin = 0;
    range.End = sizeof(bundle_expected);
    hr = ID3D12Resource_Map(readback, 0, &range, (void **)&data);
    ok(hr == S_OK, "Failed to map bundled write result, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(data[0] == bundle_expected[0], "Got %#x, expected %#x.\n", data[0], bundle_expected[0]);
        ok(data[1] == bundle_expected[1], "Got %#x, expected %#x.\n", data[1], bundle_expected[1]);
        range.Begin = range.End = 0;
        ID3D12Resource_Unmap(readback, 0, &range);
    }

done:
    if (bundle2)
        ID3D12GraphicsCommandList2_Release(bundle2);
    if (bundle)
        ID3D12GraphicsCommandList_Release(bundle);
    if (bundle_allocator)
        ID3D12CommandAllocator_Release(bundle_allocator);
    if (readback)
        ID3D12Resource_Release(readback);
    if (buffer)
        ID3D12Resource_Release(buffer);
    ID3D12GraphicsCommandList2_Release(command_list2);
    destroy_test_context(&context);
}

static void test_copy_texture_region_between_buffers(void)
{
    enum {buffer_size = 512, row_pitch = 256};
    static const BYTE source_values[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    D3D12_TEXTURE_COPY_LOCATION dst_location, src_location;
    ID3D12Resource *src_buffer = NULL, *dst_buffer = NULL;
    struct test_context_desc context_desc;
    struct test_context context;
    D3D12_RANGE range;
    D3D12_BOX box;
    BYTE *data;
    HRESULT hr;
    unsigned int i;

    memset(&context_desc, 0, sizeof(context_desc));
    context_desc.no_pipeline = TRUE;
    if (!init_test_context(&context, &context_desc))
        return;

    src_buffer = create_buffer(context.device, D3D12_HEAP_TYPE_UPLOAD, buffer_size,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    dst_buffer = create_readback_buffer(context.device, buffer_size);
    if (!src_buffer || !dst_buffer)
        goto done;

    range.Begin = range.End = 0;
    hr = ID3D12Resource_Map(src_buffer, 0, &range, (void **)&data);
    ok(hr == S_OK, "Failed to map source buffer, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;
    memset(data, 0, buffer_size);
    memcpy(data + 2, source_values, 4);
    memcpy(data + row_pitch + 2, source_values + 4, 4);
    range.Begin = 0;
    range.End = buffer_size;
    ID3D12Resource_Unmap(src_buffer, 0, &range);

    memset(&src_location, 0, sizeof(src_location));
    src_location.pResource = src_buffer;
    src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    src_location.PlacedFootprint.Footprint.Width = 8;
    src_location.PlacedFootprint.Footprint.Height = 2;
    src_location.PlacedFootprint.Footprint.Depth = 1;
    src_location.PlacedFootprint.Footprint.RowPitch = row_pitch;
    dst_location = src_location;
    dst_location.pResource = dst_buffer;

    box.left = 2;
    box.top = 0;
    box.front = 0;
    box.right = 6;
    box.bottom = 2;
    box.back = 1;
    ID3D12GraphicsCommandList_CopyTextureRegion(context.list[0],
            &dst_location, 1, 0, 0, &src_location, &box);

    hr = ID3D12GraphicsCommandList_Close(context.list[0]);
    ok(hr == S_OK, "Failed to close command list, hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;
    exec_command_list(context.queue, context.list[0]);
    wait_queue_idle(context.device, context.queue);

    range.Begin = 0;
    range.End = buffer_size;
    hr = ID3D12Resource_Map(dst_buffer, 0, &range, (void **)&data);
    ok(hr == S_OK, "Failed to map destination buffer, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < 4; ++i)
        {
            ok(data[1 + i] == source_values[i], "Row 0 byte %u is %#x, expected %#x.\n",
                    i, data[1 + i], source_values[i]);
            ok(data[row_pitch + 1 + i] == source_values[4 + i],
                    "Row 1 byte %u is %#x, expected %#x.\n",
                    i, data[row_pitch + 1 + i], source_values[4 + i]);
        }
        range.Begin = range.End = 0;
        ID3D12Resource_Unmap(dst_buffer, 0, &range);
    }

done:
    if (dst_buffer)
        ID3D12Resource_Release(dst_buffer);
    if (src_buffer)
        ID3D12Resource_Release(src_buffer);
    destroy_test_context(&context);
}

static void test_resolve_subresource_region(void)
{
    static const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
    static const float red[] = {1.0f, 0.0f, 0.0f, 1.0f};
    const RECT green_rect = {16, 8, 24, 16};
    const RECT top_rect = {0, 0, 32, 8};
    const RECT bottom_rect = {0, 16, 32, 32};
    const RECT left_rect = {0, 8, 16, 16};
    const RECT right_rect = {24, 8, 32, 16};
    D3D12_RECT src_rect = {4, 4, 12, 12};
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality_levels;
    ID3D12GraphicsCommandList1 *command_list1;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    D3D12_CLEAR_VALUE clear_value;
    struct resource_readback rb;
    struct test_context_desc desc;
    struct test_context context;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.no_pipeline = TRUE;
    if (!init_test_context(&context, &desc))
        return;

    hr = ID3D12GraphicsCommandList_QueryInterface(context.list[0],
            &IID_ID3D12GraphicsCommandList1, (void **)&command_list1);
    if (hr == E_NOINTERFACE)
    {
        win_skip("ID3D12GraphicsCommandList1 is not supported.\n");
        destroy_test_context(&context);
        return;
    }
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        destroy_test_context(&context);
        return;
    }

    memset(&quality_levels, 0, sizeof(quality_levels));
    quality_levels.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    quality_levels.SampleCount = 4;
    hr = ID3D12Device_CheckFeatureSupport(context.device,
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
            &quality_levels, sizeof(quality_levels));
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr) || !quality_levels.NumQualityLevels)
    {
        quality_levels.SampleCount = 2;
        quality_levels.NumQualityLevels = 0;
        hr = ID3D12Device_CheckFeatureSupport(context.device,
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &quality_levels, sizeof(quality_levels));
        ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    }
    if (FAILED(hr) || !quality_levels.NumQualityLevels)
    {
        win_skip("Multisampling is not supported for B8G8R8A8_UNORM.\n");
        ID3D12GraphicsCommandList1_Release(command_list1);
        destroy_test_context(&context);
        return;
    }

    create_render_target(&context);

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc = ID3D12Resource_GetDesc(context.render_target[0]);
    resource_desc.SampleDesc.Count = quality_levels.SampleCount;
    clear_value.Format = resource_desc.Format;
    memcpy(clear_value.Color, green, sizeof(clear_value.Color));
    hr = ID3D12Device_CreateCommittedResource(context.device, &heap_properties,
            D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clear_value, &IID_ID3D12Resource, (void **)&context.render_target[1]);
    ok(hr == S_OK, "Failed to create multisampled texture, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        ID3D12GraphicsCommandList1_Release(command_list1);
        destroy_test_context(&context);
        return;
    }
    ID3D12Device_CreateRenderTargetView(context.device,
            context.render_target[1], NULL, context.rtv[1]);

    ID3D12GraphicsCommandList_ClearRenderTargetView(context.list[0],
            context.rtv[0], red, 0, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(context.list[0],
            context.rtv[1], green, 0, NULL);
    transition_sub_resource_state(context.list[0], context.render_target[0], 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    transition_sub_resource_state(context.list[0], context.render_target[1], 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(command_list1,
            context.render_target[0], 0, green_rect.left, green_rect.top,
            context.render_target[1], 0, &src_rect,
            DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE);
    transition_sub_resource_state(context.list[0], context.render_target[0], 0,
            D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_texture_readback_with_command_list(context.render_target[0], 0,
            &rb, context.queue, context.list[0]);
    check_readback_data_uint(&rb, &green_rect, 0xff00ff00, 0);
    check_readback_data_uint(&rb, &top_rect, 0xffff0000, 0);
    check_readback_data_uint(&rb, &bottom_rect, 0xffff0000, 0);
    check_readback_data_uint(&rb, &left_rect, 0xffff0000, 0);
    check_readback_data_uint(&rb, &right_rect, 0xffff0000, 0);
    release_resource_readback(&rb);

    ID3D12GraphicsCommandList1_Release(command_list1);
    destroy_test_context(&context);
}

START_TEST(d3d12)
{
    BOOL enable_debug_layer = FALSE;
    unsigned int argc, i;
    ID3D12Debug *debug;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    for (i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--validate"))
            enable_debug_layer = TRUE;
        else if (!strcmp(argv[i], "--warp"))
            use_warp_adapter = TRUE;
        else if (!strcmp(argv[i], "--adapter") && i + 1 < argc)
            use_adapter_idx = atoi(argv[++i]);
    }

    if (enable_debug_layer && SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug)))
    {
        ID3D12Debug_EnableDebugLayer(debug);
        ID3D12Debug_Release(debug);
    }

    print_adapter_info();

    test_ordinals();
    test_interfaces();
    test_create_device();
    test_draw();
    test_swapchain_draw();
    test_swapchain_refcount();
    test_swapchain_size_mismatch();
    test_swapchain_backbuffer_index();
    test_desktop_window();
    test_invalid_command_queue_types();
    test_options14_to_options18_invalid_sizes();
    test_create_command_queue1();
    test_feature_level_reporting();
    test_shader_cache_session();
    test_copy_rtv_dsv_descriptors();
    test_command_list_clear_state();
    test_resource_aliasing_barriers();
    test_texture_aliasing_barrier();
    test_execute_indirect();
    test_write_buffer_immediate();
    test_copy_texture_region_between_buffers();
    test_resolve_subresource_region();
}
