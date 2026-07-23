#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS

#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

#include <float.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

static const GUID iid_id3d12_device14 =
{
    0x5f6e592d, 0xd895, 0x44c2, { 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23 }
};

static const GUID iid_id3d12_graphics_command_list10 =
{
    0x7013c015, 0xd161, 0x4b63, { 0xa0, 0x8c, 0x23, 0x85, 0x52, 0xdd, 0x8a, 0xcc }
};

static const DWORD test_vertex_shader_code[] =
{
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

static const DWORD test_pixel_shader_code[] =
{
    0x43425844, 0x8a4a8140, 0x5eba8e0b, 0x714e0791, 0xb4b8eed2, 0x00000001, 0x000000d8, 0x00000003,
    0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
    0x00000000, 0x00000001, 0x00000003, 0x00000000, 0x0000000f, 0x505f5653, 0x7469736f, 0x006e6f69,
    0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000000, 0x00000003,
    0x00000000, 0x0000000f, 0x545f5653, 0x65677261, 0xabab0074, 0x58454853, 0x0000003c, 0x00000050,
    0x0000000f, 0x0100086a, 0x03000065, 0x001020f2, 0x00000000, 0x08000036, 0x001020f2, 0x00000000,
    0x00004002, 0x00000000, 0x3f800000, 0x00000000, 0x3f800000, 0x0100003e,
};

static LONG root_signature_test_active;
static LONG root_signature_access_violations;
static LONG pso_thread_test_active;
static LONG pso_thread_access_violations;

static LONG WINAPI root_signature_exception_handler( EXCEPTION_POINTERS *exception )
{
    if (InterlockedCompareExchange( &root_signature_test_active, 0, 0 ) &&
        exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        InterlockedIncrement( &root_signature_access_violations );
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI pso_thread_exception_handler( EXCEPTION_POINTERS *exception )
{
    if (InterlockedCompareExchange( &pso_thread_test_active, 0, 0 ) &&
        exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        InterlockedIncrement( &pso_thread_access_violations );
    return EXCEPTION_CONTINUE_SEARCH;
}

static int check_result( const char *operation, HRESULT hr )
{
    if (SUCCEEDED(hr)) return 1;
    fprintf( stderr, "%s failed: %#lx\n", operation, hr );
    return 0;
}

struct root_signature_thread_context
{
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc;
    HANDLE start_event;
    ID3D12VersionedRootSignatureDeserializer *deserializer;
    ID3DBlob *blob;
    ID3DBlob *error;
    HRESULT serialize_hr;
    HRESULT deserialize_hr;
};

struct pso_worker_thread_context
{
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12PipelineState *pipeline_state;
    ID3D12Fence *fence;
    UINT64 signal_value;
    HANDLE start_event;
    HRESULT command_allocator_hr;
    HRESULT command_list_create_hr;
    DWORD command_list_set_pipeline_state_ok;
    HRESULT pipeline_cached_blob_hr;
    DWORD pipeline_cached_blob_methods_ok;
    HRESULT command_list_close_hr;
    HRESULT queue_signal_hr;
    BOOL timeout;
};

static unsigned __stdcall root_signature_thread_proc( void *opaque )
{
    struct root_signature_thread_context *context = opaque;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *returned_desc;
    DWORD ret = 1;
    void *data;
    SIZE_T size;

    if (WaitForSingleObject( context->start_event, 30000 ) != WAIT_OBJECT_0)
    {
        ret = 5;
        goto done;
    }

    context->serialize_hr = D3D12SerializeVersionedRootSignature(
        context->desc, &context->blob, &context->error );
    if (FAILED(context->serialize_hr)) goto done;

    data = ID3D10Blob_GetBufferPointer( (ID3D10Blob *)context->blob );
    size = ID3D10Blob_GetBufferSize( (ID3D10Blob *)context->blob );
    if (!data || !size)
    {
        ret = 2;
        goto done;
    }

    context->deserialize_hr = D3D12CreateVersionedRootSignatureDeserializer(
        data, size, &IID_ID3D12VersionedRootSignatureDeserializer,
        (void **)&context->deserializer );
    if (FAILED(context->deserialize_hr))
    {
        ret = 3;
        goto done;
    }

    returned_desc =
        ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(
            context->deserializer );
    if (!returned_desc || returned_desc->Version != context->desc->Version)
    {
        ret = 4;
        goto done;
    }

    ret = 0;

done:
    if (context->deserializer)
    {
        ID3D12VersionedRootSignatureDeserializer_Release( context->deserializer );
        context->deserializer = NULL;
    }
    if (context->error)
    {
        ID3D10Blob_Release( (ID3D10Blob *)context->error );
        context->error = NULL;
    }
    if (context->blob)
    {
        ID3D10Blob_Release( (ID3D10Blob *)context->blob );
        context->blob = NULL;
    }
    return ret;
}

static unsigned __stdcall pso_worker_thread_proc( void *opaque )
{
    struct pso_worker_thread_context *context = opaque;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3DBlob *cached_blob = NULL;
    ID3D12CommandList *command_list_for_execute;
    DWORD ret = 1;

    context->timeout = FALSE;

    if (WaitForSingleObject( context->start_event, 30000 ) != WAIT_OBJECT_0)
    {
        context->timeout = TRUE;
        return 2;
    }

    context->command_allocator_hr = ID3D12Device_CreateCommandAllocator(
        context->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
        (void **)&allocator );
    if (FAILED(context->command_allocator_hr))
    {
        ret = 3;
        goto done;
    }

    context->command_list_create_hr = ID3D12Device_CreateCommandList(
        context->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
        context->pipeline_state, &IID_ID3D12GraphicsCommandList, (void **)&list );
    if (FAILED(context->command_list_create_hr))
    {
        ret = 4;
        goto done;
    }

    context->command_list_set_pipeline_state_ok = 0;
    ID3D12GraphicsCommandList_SetPipelineState( list, context->pipeline_state );
    context->command_list_set_pipeline_state_ok = 1;

    context->pipeline_cached_blob_hr = ID3D12PipelineState_GetCachedBlob(
        context->pipeline_state, &cached_blob );
    if (FAILED(context->pipeline_cached_blob_hr) || !cached_blob)
    {
        ret = 8;
        goto done;
    }
    ID3D10Blob_GetBufferPointer( (ID3D10Blob *)cached_blob );
    ID3D10Blob_GetBufferSize( (ID3D10Blob *)cached_blob );
    context->pipeline_cached_blob_methods_ok = 1;

    context->command_list_close_hr = ID3D12GraphicsCommandList_Close( list );
    if (FAILED(context->command_list_close_hr))
    {
        ret = 5;
        goto done;
    }

    command_list_for_execute = (ID3D12CommandList *)list;
    ID3D12CommandQueue_ExecuteCommandLists( context->queue, 1, &command_list_for_execute );

    context->queue_signal_hr = ID3D12CommandQueue_Signal( context->queue,
        context->fence, context->signal_value );
    if (FAILED(context->queue_signal_hr))
        ret = 7;
    else
        ret = 0;

done:
    if (cached_blob)
        ID3D10Blob_Release( (ID3D10Blob *)cached_blob );
    if (list)
        ID3D12GraphicsCommandList_Release( list );
    if (allocator)
        ID3D12CommandAllocator_Release( allocator );
    return ret;
}

int main(void)
{
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {0};
    D3D12_DESCRIPTOR_HEAP_DESC returned_descriptor_heap_desc;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_state_desc = {0};
    D3D12_BLEND_DESC blend_desc = {0};
    D3D12_RASTERIZER_DESC rasterizer_desc = {0};
    D3D12_HEAP_PROPERTIES heap_properties = {0};
    D3D12_RESOURCE_DESC resource_desc = {0};
    D3D12_RESOURCE_DESC1 resource_desc1 = {0};
    D3D12_RESOURCE_DESC returned_resource_desc;
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info;
    D3D12_RESOURCE_ALLOCATION_INFO1 allocation_info1 = {0};
    D3D12_COMMAND_QUEUE_DESC returned_queue_desc;
    D3D12_DESCRIPTOR_RANGE1 root_descriptor_ranges[2] = {0};
    D3D12_ROOT_PARAMETER1 root_parameters[4] = {0};
    D3D12_STATIC_SAMPLER_DESC static_samplers[6] = {0};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_signature_desc = {0};
    D3D12_SHADER_BYTECODE vs = { test_vertex_shader_code, sizeof( test_vertex_shader_code ) };
    D3D12_SHADER_BYTECODE ps = { test_pixel_shader_code, sizeof( test_pixel_shader_code ) };
    struct root_signature_thread_context root_signature_contexts[1] = {0};
    struct pso_worker_thread_context pso_worker_context = {0};
    ID3D12PipelineState *pipeline_state = NULL;
    ID3D12RootSignature *pipeline_root_signature = NULL;
    ID3DBlob *serialized_root_signature = NULL;
    ID3DBlob *serialize_error = NULL;
    ID3D12GraphicsCommandList *command_list = NULL;
    ID3D12GraphicsCommandList *command_list10 = NULL;
    ID3D12DescriptorHeap *descriptor_heap = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12Pageable *pageable = NULL;
    ID3D12Resource *resource = NULL;
    ID3D12Device *queue_device = NULL;
    ID3D12Device *base_device = NULL;
    ID3D12Device *device = NULL;
    ID3D12Fence *fence = NULL;
    HANDLE root_signature_threads[1] = {0};
    HANDLE pso_worker_thread = NULL;
    HANDLE root_signature_start_event = NULL;
    HANDLE pso_worker_start_event = NULL;
    PVOID root_signature_exception_cookie = NULL;
    PVOID pso_thread_exception_cookie = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle;
    ID3D12CommandList *command_lists[1];
    DWORD root_signature_thread_result;
    DWORD pso_worker_thread_result;
    HANDLE pso_signal_event = NULL;
    UINT64 pso_signal_value = 0;
    UINT64 timestamp_frequency = 0;
    UINT64 completed_value;
    unsigned int i;
    void *mapped_data = NULL;
    HRESULT hr;
    int ret = 1;

    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    root_descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    root_descriptor_ranges[0].NumDescriptors = 64;
    root_descriptor_ranges[0].Flags =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
        D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    root_descriptor_ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    root_descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    root_descriptor_ranges[1].NumDescriptors = 32;
    root_descriptor_ranges[1].Flags =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    root_descriptor_ranges[1].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    for (i = 0; i < 2; ++i)
    {
        root_parameters[i].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        root_parameters[i].DescriptorTable.pDescriptorRanges =
            &root_descriptor_ranges[i];
        root_parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    for (i = 2; i < 4; ++i)
    {
        root_parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_parameters[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
        root_parameters[i].ShaderVisibility = i == 2
            ? D3D12_SHADER_VISIBILITY_PIXEL : D3D12_SHADER_VISIBILITY_VERTEX;
    }
    for (i = 0; i < ARRAYSIZE(static_samplers); ++i)
    {
        static_samplers[i].Filter = i < 2 ? D3D12_FILTER_MIN_MAG_MIP_POINT
            : i < 4 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT
            : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        static_samplers[i].AddressU = static_samplers[i].AddressV =
            static_samplers[i].AddressW = i % 2
            ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        static_samplers[i].MaxAnisotropy = 1;
        static_samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        static_samplers[i].BorderColor =
            D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        static_samplers[i].MaxLOD = FLT_MAX;
        static_samplers[i].ShaderRegister = i;
        static_samplers[i].RegisterSpace = 1000;
        static_samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    root_signature_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_signature_desc.Desc_1_1.NumParameters = ARRAYSIZE(root_parameters);
    root_signature_desc.Desc_1_1.pParameters = root_parameters;
    root_signature_desc.Desc_1_1.NumStaticSamplers = ARRAYSIZE(static_samplers);
    root_signature_desc.Desc_1_1.pStaticSamplers = static_samplers;
    root_signature_desc.Desc_1_1.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS;

    hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_11_0,
                            &IID_ID3D12Device, (void **)&base_device );
    if (!check_result( "D3D12CreateDevice", hr )) goto done;
    hr = ID3D12Device_QueryInterface( base_device, &iid_id3d12_device14,
                                      (void **)&device );
    if (!check_result( "ID3D12Device::QueryInterface(ID3D12Device14)", hr )) goto done;

    root_signature_exception_cookie =
        AddVectoredExceptionHandler( 1, root_signature_exception_handler );
    if (!root_signature_exception_cookie)
    {
        fprintf( stderr, "AddVectoredExceptionHandler failed: %lu\n", GetLastError() );
        goto done;
    }
    root_signature_start_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (!root_signature_start_event)
    {
        fprintf( stderr, "CreateEvent failed: %lu\n", GetLastError() );
        goto done;
    }
    for (i = 0; i < ARRAYSIZE(root_signature_threads); ++i)
    {
        root_signature_contexts[i].desc = &root_signature_desc;
        root_signature_contexts[i].start_event = root_signature_start_event;
        root_signature_threads[i] = (HANDLE)_beginthreadex(
            NULL, 0, root_signature_thread_proc, &root_signature_contexts[i], 0, NULL );
        if (!root_signature_threads[i])
        {
            fprintf( stderr, "_beginthreadex %u failed: %lu\n", i, GetLastError() );
            goto done;
        }
    }
    InterlockedExchange( &root_signature_access_violations, 0 );
    InterlockedExchange( &root_signature_test_active, 1 );
    if (!SetEvent( root_signature_start_event ))
    {
        fprintf( stderr, "SetEvent failed: %lu\n", GetLastError() );
        goto done;
    }
    if (WaitForMultipleObjects( ARRAYSIZE(root_signature_threads),
                                root_signature_threads, TRUE, 30000 ) != WAIT_OBJECT_0)
    {
        fprintf( stderr, "Waiting for the D3D12 root signature worker failed: %lu\n",
                 GetLastError() );
        goto done;
    }
    InterlockedExchange( &root_signature_test_active, 0 );
    for (i = 0; i < ARRAYSIZE(root_signature_threads); ++i)
    {
        if (!GetExitCodeThread( root_signature_threads[i],
                                &root_signature_thread_result ))
        {
            fprintf( stderr, "Reading D3D12 root signature worker %u failed: %lu\n",
                     i, GetLastError() );
            goto done;
        }
        CloseHandle( root_signature_threads[i] );
        root_signature_threads[i] = NULL;
        if (root_signature_thread_result)
        {
            fprintf( stderr,
                     "D3D12 root signature worker %u failed at step %lu "
                     "(serialize %#lx, deserialize %#lx).\n",
                     i, root_signature_thread_result,
                     root_signature_contexts[i].serialize_hr,
                     root_signature_contexts[i].deserialize_hr );
            goto done;
        }
    }
    if (InterlockedCompareExchange( &root_signature_access_violations, 0, 0 ))
    {
        fprintf( stderr,
                 "D3D12 root signature serialization raised %ld access violation(s).\n",
                 root_signature_access_violations );
        goto done;
    }

    hr = D3D12SerializeVersionedRootSignature( &root_signature_desc,
                                               &serialized_root_signature,
                                               &serialize_error );
    if (!check_result( "D3D12SerializeVersionedRootSignature", hr ))
        goto done;
    hr = ID3D12Device_CreateRootSignature( base_device, 0,
                                          ID3D10Blob_GetBufferPointer(
                                              (ID3D10Blob *)serialized_root_signature ),
                                          ID3D10Blob_GetBufferSize(
                                              (ID3D10Blob *)serialized_root_signature ),
                                          &IID_ID3D12RootSignature,
                                          (void **)&pipeline_root_signature );
    if (!check_result( "ID3D12Device::CreateRootSignature", hr ))
        goto done;

    blend_desc.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE;
    pipeline_state_desc.pRootSignature = pipeline_root_signature;
    pipeline_state_desc.VS = vs;
    pipeline_state_desc.PS = ps;
    pipeline_state_desc.BlendState = blend_desc;
    pipeline_state_desc.RasterizerState = rasterizer_desc;
    pipeline_state_desc.SampleMask = 0xffffffff;
    pipeline_state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline_state_desc.NumRenderTargets = 1;
    pipeline_state_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline_state_desc.SampleDesc.Count = 1;

    hr = ID3D12Device_CreateGraphicsPipelineState( base_device, &pipeline_state_desc,
                                                  &IID_ID3D12PipelineState,
                                                  (void **)&pipeline_state );
    if (!check_result( "ID3D12Device::CreateGraphicsPipelineState", hr ))
        goto done;

    hr = ID3D12Device_CreateCommandQueue( device, &queue_desc,
                                          &IID_ID3D12CommandQueue, (void **)&queue );
    if (!check_result( "ID3D12Device::CreateCommandQueue", hr )) goto done;
    returned_queue_desc = ID3D12CommandQueue_GetDesc( queue );
    if (returned_queue_desc.Type != queue_desc.Type ||
        returned_queue_desc.Priority != queue_desc.Priority)
    {
        fprintf( stderr, "D3D12 command queue returned an invalid descriptor.\n" );
        goto done;
    }
    hr = ID3D12CommandQueue_GetTimestampFrequency( queue, &timestamp_frequency );
    if (!check_result( "ID3D12CommandQueue::GetTimestampFrequency", hr ) ||
        !timestamp_frequency)
    {
        fprintf( stderr, "D3D12 command queue returned an invalid timestamp frequency.\n" );
        goto done;
    }
    hr = ID3D12CommandQueue_GetDevice( queue, &IID_ID3D12Device,
                                       (void **)&queue_device );
    if (!check_result( "ID3D12CommandQueue::GetDevice", hr )) goto done;
    ID3D12Device_Release( queue_device );
    queue_device = NULL;
    hr = ID3D12CommandQueue_QueryInterface( queue, &IID_ID3D12Pageable,
                                            (void **)&pageable );
    if (!check_result( "ID3D12CommandQueue::QueryInterface", hr )) goto done;
    ID3D12Pageable_Release( pageable );
    pageable = NULL;

    hr = ID3D12Device_CreateCommandAllocator( device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              &IID_ID3D12CommandAllocator,
                                              (void **)&allocator );
    if (!check_result( "ID3D12Device::CreateCommandAllocator", hr )) goto done;
    hr = ID3D12Device_CreateCommandList( device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator, NULL, &IID_ID3D12GraphicsCommandList,
                                         (void **)&command_list );
    if (!check_result( "ID3D12Device::CreateCommandList", hr )) goto done;
    hr = ID3D12GraphicsCommandList_QueryInterface(
        command_list, &iid_id3d12_graphics_command_list10, (void **)&command_list10 );
    if (!check_result( "ID3D12GraphicsCommandList::QueryInterface(ID3D12GraphicsCommandList10)",
                       hr )) goto done;
    ID3D12GraphicsCommandList1_SetViewInstanceMask(
        (ID3D12GraphicsCommandList1 *)command_list10, 0 );
    hr = ID3D12GraphicsCommandList_Close( command_list10 );
    if (!check_result( "ID3D12GraphicsCommandList10::Close", hr )) goto done;
    command_lists[0] = (ID3D12CommandList *)command_list10;
    ID3D12CommandQueue_ExecuteCommandLists( queue, 1, command_lists );

    hr = ID3D12Device_CreateFence( device, 0, D3D12_FENCE_FLAG_NONE,
                                   &IID_ID3D12Fence, (void **)&fence );
    if (!check_result( "ID3D12Device::CreateFence", hr )) goto done;
    hr = ID3D12CommandQueue_Signal( queue, fence, 1 );
    if (!check_result( "ID3D12CommandQueue::Signal", hr )) goto done;
    completed_value = ID3D12Fence_GetCompletedValue( fence );
    printf( "D3D12 fence completed value: %llu\n", (unsigned long long)completed_value );

    pso_signal_event = CreateEventW( NULL, FALSE, FALSE, NULL );
    if (!pso_signal_event)
    {
        fprintf( stderr, "CreateEvent failed: %lu\n", GetLastError() );
        goto done;
    }
    pso_worker_start_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (!pso_worker_start_event)
    {
        fprintf( stderr, "CreateEvent failed: %lu\n", GetLastError() );
        goto done;
    }
    pso_signal_value = 2;
    pso_worker_context.device = device;
    pso_worker_context.queue = queue;
    pso_worker_context.pipeline_state = pipeline_state;
    pso_worker_context.fence = fence;
    pso_worker_context.signal_value = pso_signal_value;
    pso_worker_context.start_event = pso_worker_start_event;
    pso_thread_exception_cookie = AddVectoredExceptionHandler( 1, pso_thread_exception_handler );
    if (!pso_thread_exception_cookie)
    {
        fprintf( stderr, "AddVectoredExceptionHandler failed: %lu\n", GetLastError() );
        goto done;
    }
    pso_worker_thread = (HANDLE)_beginthreadex(
        NULL, 0, pso_worker_thread_proc, &pso_worker_context, 0, NULL );
    if (!pso_worker_thread)
    {
        fprintf( stderr, "_beginthreadex for PSO worker failed: %lu\n", GetLastError() );
        goto done;
    }
    InterlockedExchange( &pso_thread_access_violations, 0 );
    InterlockedExchange( &pso_thread_test_active, 1 );
    if (!SetEvent( pso_worker_start_event ))
    {
        fprintf( stderr, "SetEvent failed: %lu\n", GetLastError() );
        goto done;
    }
    if (WaitForSingleObject( pso_worker_thread, 30000 ) != WAIT_OBJECT_0)
    {
        fprintf( stderr, "D3D12 PSO worker thread timed out.\n" );
        goto done;
    }
    if (!GetExitCodeThread( pso_worker_thread, &pso_worker_thread_result ))
    {
        fprintf( stderr, "Reading D3D12 PSO worker thread failed: %lu\n", GetLastError() );
        goto done;
    }
    CloseHandle( pso_worker_thread );
    pso_worker_thread = NULL;
    InterlockedExchange( &pso_thread_test_active, 0 );
    if (pso_worker_thread_result)
    {
        fprintf( stderr,
                 "D3D12 PSO worker thread failed at step %lu (allocator %#lx, command_list %#lx, set_pipeline_state_ok=%lu, cached_blob %#lx, cached_blob_methods_ok=%lu, close %#lx, signal %#lx).\n",
                 pso_worker_thread_result,
                 pso_worker_context.command_allocator_hr,
                 pso_worker_context.command_list_create_hr,
                 pso_worker_context.command_list_set_pipeline_state_ok,
                 pso_worker_context.pipeline_cached_blob_hr,
                 pso_worker_context.pipeline_cached_blob_methods_ok,
                 pso_worker_context.command_list_close_hr,
                 pso_worker_context.queue_signal_hr );
        goto done;
    }
    if (InterlockedCompareExchange( &pso_thread_access_violations, 0, 0 ))
    {
        fprintf( stderr,
                 "D3D12 PSO worker thread raised %ld access violation(s).\n",
                 pso_thread_access_violations );
        goto done;
    }

    hr = ID3D12Fence_SetEventOnCompletion( fence, pso_signal_value,
                                           pso_signal_event );
    if (!check_result( "ID3D12Fence::SetEventOnCompletion", hr ))
        goto done;
    if (WaitForSingleObject( pso_signal_event, 30000 ) != WAIT_OBJECT_0)
    {
        fprintf( stderr, "Timed out waiting for D3D12 PSO command completion.\n" );
        goto done;
    }
    completed_value = ID3D12Fence_GetCompletedValue( fence );
    if (completed_value < pso_signal_value)
    {
        fprintf( stderr,
                 "D3D12 fence completed value did not advance: got %llu expected at least %llu\n",
                 (unsigned long long)completed_value,
                 (unsigned long long)pso_signal_value );
        goto done;
    }

    descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptor_heap_desc.NumDescriptors = 1;
    descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = ID3D12Device_CreateDescriptorHeap( device, &descriptor_heap_desc,
                                            &IID_ID3D12DescriptorHeap,
                                            (void **)&descriptor_heap );
    if (!check_result( "ID3D12Device::CreateDescriptorHeap", hr )) goto done;
    returned_descriptor_heap_desc = ID3D12DescriptorHeap_GetDesc( descriptor_heap );
    if (returned_descriptor_heap_desc.Type != descriptor_heap_desc.Type ||
        returned_descriptor_heap_desc.NumDescriptors != descriptor_heap_desc.NumDescriptors ||
        returned_descriptor_heap_desc.Flags != descriptor_heap_desc.Flags)
    {
        fprintf( stderr, "D3D12 descriptor heap returned an invalid descriptor.\n" );
        goto done;
    }
    descriptor_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart( descriptor_heap );
    if (!descriptor_handle.ptr)
    {
        fprintf( stderr, "D3D12 descriptor heap returned a null CPU handle.\n" );
        goto done;
    }

    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = 4096;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc1.Dimension = resource_desc.Dimension;
    resource_desc1.Width = resource_desc.Width;
    resource_desc1.Height = resource_desc.Height;
    resource_desc1.DepthOrArraySize = resource_desc.DepthOrArraySize;
    resource_desc1.MipLevels = resource_desc.MipLevels;
    resource_desc1.SampleDesc = resource_desc.SampleDesc;
    resource_desc1.Layout = resource_desc.Layout;
    allocation_info = ID3D12Device8_GetResourceAllocationInfo2(
        (ID3D12Device8 *)device, 0, 1, &resource_desc1, &allocation_info1 );
    if (!allocation_info.SizeInBytes || !allocation_info.Alignment)
    {
        fprintf( stderr, "ID3D12Device14 returned invalid inherited resource allocation information.\n" );
        goto done;
    }
    allocation_info = ID3D12Device_GetResourceAllocationInfo( device, 0, 1,
                                                               &resource_desc );
    if (!allocation_info.SizeInBytes || !allocation_info.Alignment)
    {
        fprintf( stderr, "D3D12 returned invalid resource allocation information.\n" );
        goto done;
    }
    hr = ID3D12Device_CreateCommittedResource( device, &heap_properties,
                                               D3D12_HEAP_FLAG_NONE, &resource_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                               &IID_ID3D12Resource, (void **)&resource );
    if (!check_result( "ID3D12Device::CreateCommittedResource", hr )) goto done;
    returned_resource_desc = ID3D12Resource_GetDesc( resource );
    if (returned_resource_desc.Dimension != resource_desc.Dimension ||
        returned_resource_desc.Width != resource_desc.Width ||
        returned_resource_desc.Layout != resource_desc.Layout)
    {
        fprintf( stderr, "D3D12 resource returned an invalid descriptor.\n" );
        goto done;
    }
    hr = ID3D12Resource_Map( resource, 0, NULL, &mapped_data );
    if (!check_result( "ID3D12Resource::Map", hr ) || !mapped_data) goto done;
    memset( mapped_data, 0x5a, 4096 );
    ID3D12Resource_Unmap( resource, 0, NULL );
    mapped_data = NULL;

    printf( "D3DMetal D3D12 callback smoke test passed (frequency %llu).\n",
            (unsigned long long)timestamp_frequency );
    ret = 0;

done:
    InterlockedExchange( &root_signature_test_active, 0 );
    InterlockedExchange( &pso_thread_test_active, 0 );
    if (root_signature_exception_cookie)
        RemoveVectoredExceptionHandler( root_signature_exception_cookie );
    if (pso_thread_exception_cookie)
        RemoveVectoredExceptionHandler( pso_thread_exception_cookie );
    for (i = 0; i < ARRAYSIZE(root_signature_threads); ++i)
        if (root_signature_threads[i]) CloseHandle( root_signature_threads[i] );
    if (root_signature_start_event) CloseHandle( root_signature_start_event );
    if (pso_worker_thread) CloseHandle( pso_worker_thread );
    if (pso_worker_start_event) CloseHandle( pso_worker_start_event );
    if (pso_signal_event) CloseHandle( pso_signal_event );
    if (resource)
    {
        if (mapped_data) ID3D12Resource_Unmap( resource, 0, NULL );
        ID3D12Resource_Release( resource );
    }
    if (descriptor_heap) ID3D12DescriptorHeap_Release( descriptor_heap );
    if (fence) ID3D12Fence_Release( fence );
    if (pipeline_state) ID3D12PipelineState_Release( pipeline_state );
    if (pipeline_root_signature) ID3D12RootSignature_Release( pipeline_root_signature );
    if (serialized_root_signature)
        ID3D10Blob_Release( (ID3D10Blob *)serialized_root_signature );
    if (serialize_error)
        ID3D10Blob_Release( (ID3D10Blob *)serialize_error );
    if (command_list10) ID3D12GraphicsCommandList_Release( command_list10 );
    if (command_list) ID3D12GraphicsCommandList_Release( command_list );
    if (allocator) ID3D12CommandAllocator_Release( allocator );
    if (pageable) ID3D12Pageable_Release( pageable );
    if (queue_device) ID3D12Device_Release( queue_device );
    if (queue) ID3D12CommandQueue_Release( queue );
    if (device) ID3D12Device_Release( device );
    if (base_device) ID3D12Device_Release( base_device );
    return ret;
}
