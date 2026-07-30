#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS

#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

#include <float.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_D3D12_SDK_VERSION 611

__declspec(dllexport) const UINT D3D12SDKVersion = TEST_D3D12_SDK_VERSION;
__declspec(dllexport) const char *D3D12SDKPath = ".\\D3D12-REDIST\\";

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

static int verify_agility_contract(void)
{
    ID3D12SDKConfiguration1 *configuration = NULL;
    ID3D12DeviceFactory *factory = NULL;
    ID3D12Device *device = NULL;
    PFN_D3D12_GET_INTERFACE get_interface;
    D3D12_DEVICE_FACTORY_FLAGS flags;
    UINT *core_sdk_version;
    HMODULE d3d12, core;
    HRESULT hr;
    int ret = 0;

    if (!(d3d12 = GetModuleHandleW( L"d3d12.dll" )))
    {
        fprintf( stderr, "The D3D12 loader was not loaded.\n" );
        goto done;
    }
    if (GetModuleHandleW( L"d3d12core.dll" ))
    {
        fprintf( stderr, "The D3D12 loader activated its core module from DllMain.\n" );
        goto done;
    }
    if (!(get_interface = (void *)GetProcAddress( d3d12, "D3D12GetInterface" )))
    {
        fprintf( stderr, "The D3D12 loader does not export D3D12GetInterface.\n" );
        goto done;
    }

    hr = get_interface( &CLSID_D3D12SDKConfiguration,
                        &IID_ID3D12SDKConfiguration1, (void **)&configuration );
    if (!check_result( "D3D12GetInterface(ID3D12SDKConfiguration1)", hr ))
        goto done;
    hr = ID3D12SDKConfiguration1_SetSDKVersion(
        configuration, TEST_D3D12_SDK_VERSION, D3D12SDKPath );
    if (!check_result( "ID3D12SDKConfiguration1::SetSDKVersion", hr ))
        goto done;
    if (!(core = GetModuleHandleW( L"d3d12core.dll" )))
    {
        fprintf( stderr, "The D3D12 loader did not activate its system core module.\n" );
        goto done;
    }
    if (!(core_sdk_version = (UINT *)GetProcAddress( core, "D3D12SDKVersion" )))
    {
        fprintf( stderr, "D3D12Core does not export D3D12SDKVersion as data.\n" );
        goto done;
    }
    if (*core_sdk_version != TEST_D3D12_SDK_VERSION)
    {
        fprintf( stderr, "D3D12Core selected SDK version %u instead of %u.\n",
                 *core_sdk_version, TEST_D3D12_SDK_VERSION );
        goto done;
    }
    hr = ID3D12SDKConfiguration1_CreateDeviceFactory(
        configuration, TEST_D3D12_SDK_VERSION, D3D12SDKPath,
        &IID_ID3D12DeviceFactory, (void **)&factory );
    if (!check_result( "ID3D12SDKConfiguration1::CreateDeviceFactory", hr ))
        goto done;

    hr = ID3D12DeviceFactory_SetFlags(
        factory, D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON );
    if (!check_result( "ID3D12DeviceFactory::SetFlags", hr ))
        goto done;
    flags = ID3D12DeviceFactory_GetFlags( factory );
    if (flags != D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON)
    {
        fprintf( stderr, "ID3D12DeviceFactory returned flags %#x.\n", flags );
        goto done;
    }

    hr = ID3D12DeviceFactory_CreateDevice(
        factory, NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&device );
    if (!check_result( "ID3D12DeviceFactory::CreateDevice", hr ))
        goto done;
    if (!GetModuleHandleW( L"d3dmt.dll" ))
    {
        fprintf( stderr, "The D3D12 Agility proxy did not activate D3DMetal.\n" );
        goto done;
    }

    ret = 1;

done:
    if (device) ID3D12Device_Release( device );
    if (factory) ID3D12DeviceFactory_Release( factory );
    if (configuration) ID3D12SDKConfiguration1_Release( configuration );
    return ret;
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

static int run_chromium_gpu_probe(void)
{
    HRESULT hr;

    hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_11_0,
                            &IID_ID3D12Device, NULL );
    if (GetModuleHandleW( L"d3dmt.dll" ))
    {
        fprintf( stderr,
                 "The Chromium GPU capability probe activated D3DMetal.\n" );
        return 1;
    }

    printf( "Chromium GPU D3D12 probe stayed on the Wine fallback "
            "(result %#lx).\n", hr );
    return 0;
}

enum narrow_float_descriptor_copy
{
    NARROW_FLOAT_DESCRIPTOR_DIRECT,
    NARROW_FLOAT_DESCRIPTOR_COPY_SIMPLE,
    NARROW_FLOAT_DESCRIPTOR_COPY_RANGES,
    NARROW_FLOAT_DESCRIPTOR_COPY_SIMPLE_64,
    NARROW_FLOAT_DESCRIPTOR_COPY_RANGES_65,
};

struct narrow_float_clear_case
{
    const char *name;
    DXGI_FORMAT format;
    float values[4];
    DWORD expected;
    BOOL unordered_access;
    BOOL null_view_desc;
    enum narrow_float_descriptor_copy descriptor_copy;
};

static float narrow_float_value_from_bits( DWORD bits )
{
    union
    {
        DWORD bits;
        float value;
    } value;

    value.bits = bits;
    return value.value;
}

static int run_narrow_float_clear_case_with_objects(
    ID3D12Device *device, ID3D12CommandQueue *queue,
    const struct narrow_float_clear_case *test,
    ID3D12Resource *provided_texture,
    const D3D12_CPU_DESCRIPTOR_HANDLE *provided_handle )
{
    D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {0};
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {0};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {0};
    D3D12_HEAP_PROPERTIES default_heap = {0};
    D3D12_HEAP_PROPERTIES readback_heap = {0};
    D3D12_RESOURCE_DESC texture_desc = {0};
    D3D12_RESOURCE_DESC buffer_desc = {0};
    D3D12_RESOURCE_BARRIER barrier = {0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {0};
    D3D12_TEXTURE_COPY_LOCATION source_location = {0};
    D3D12_TEXTURE_COPY_LOCATION destination_location = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE source_handle = {0}, clear_handle = {0}, view_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = {0};
    ID3D12DescriptorHeap *descriptor_heap = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *command_list = NULL;
    ID3D12Resource *texture = NULL;
    ID3D12Resource *readback = NULL;
    ID3D12Fence *fence = NULL;
    ID3D12CommandList *command_lists[1];
    ID3D12DescriptorHeap *descriptor_heaps[1];
    D3D12_CPU_DESCRIPTOR_HANDLE destination_starts[1];
    D3D12_CPU_DESCRIPTOR_HANDLE source_starts[1];
    D3D12_RANGE read_range, written_range = {0};
    UINT descriptor_range_sizes[1] = {1};
    UINT descriptor_copy_count;
    UINT descriptor_increment;
    UINT i;
    UINT row_count;
    UINT64 row_size, total_size;
    D3D12_RESOURCE_STATES initial_state;
    D3D12_DESCRIPTOR_HEAP_TYPE descriptor_type;
    HANDLE fence_event = NULL;
    void *mapped = NULL;
    DWORD actual;
    HRESULT hr;
    int ret = 0;

    if (provided_handle &&
        (!provided_handle->ptr || test->unordered_access ||
         test->descriptor_copy != NARROW_FLOAT_DESCRIPTOR_DIRECT))
    {
        fprintf( stderr, "%s received an incompatible descriptor handle.\n",
                 test->name );
        goto done;
    }

    descriptor_type = test->unordered_access
        ? D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
        : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (test->descriptor_copy == NARROW_FLOAT_DESCRIPTOR_COPY_SIMPLE_64)
        descriptor_copy_count = 64;
    else if (test->descriptor_copy == NARROW_FLOAT_DESCRIPTOR_COPY_RANGES_65)
        descriptor_copy_count = 65;
    else
        descriptor_copy_count = 1;
    if (!provided_handle)
    {
        descriptor_heap_desc.Type = descriptor_type;
        descriptor_heap_desc.NumDescriptors =
            test->descriptor_copy == NARROW_FLOAT_DESCRIPTOR_DIRECT ?
            1 : descriptor_copy_count * 2;
        if (test->unordered_access)
            descriptor_heap_desc.Flags =
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = ID3D12Device_CreateDescriptorHeap(
            device, &descriptor_heap_desc, &IID_ID3D12DescriptorHeap,
            (void **)&descriptor_heap );
        if (!check_result( test->name, hr )) goto done;
        fprintf( stderr, "  descriptor heap created.\n" );
    }

    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = 1;
    texture_desc.Height = 1;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = test->format;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (test->unordered_access)
    {
        texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        initial_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    else
    {
        texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (provided_texture)
    {
        texture = provided_texture;
        ID3D12Resource_AddRef( texture );
        fprintf( stderr, "  provided texture retained.\n" );
    }
    else
    {
        hr = ID3D12Device_CreateCommittedResource(
            device, &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
            initial_state, NULL, &IID_ID3D12Resource, (void **)&texture );
        if (!check_result( test->name, hr )) goto done;
        fprintf( stderr, "  texture created.\n" );
    }

    if (provided_handle)
        source_handle = *provided_handle;
    else
        source_handle =
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
                descriptor_heap );
    clear_handle = source_handle;
    descriptor_increment =
        ID3D12Device_GetDescriptorHandleIncrementSize( device, descriptor_type );
    if (test->descriptor_copy != NARROW_FLOAT_DESCRIPTOR_DIRECT)
    {
        clear_handle.ptr += (UINT_PTR)(descriptor_copy_count * 2 - 1) *
                            descriptor_increment;
    }

    for (i = 0; i < descriptor_copy_count; ++i)
    {
        view_handle = source_handle;
        view_handle.ptr += (UINT_PTR)i * descriptor_increment;
        if (test->unordered_access)
        {
            uav_desc.Format = test->format;
            uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            ID3D12Device_CreateUnorderedAccessView(
                device, texture, NULL, test->null_view_desc ? NULL : &uav_desc,
                view_handle );
        }
        else
        {
            rtv_desc.Format = test->format;
            rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            ID3D12Device_CreateRenderTargetView(
                device, texture, test->null_view_desc ? NULL : &rtv_desc,
                view_handle );
        }
    }
    fprintf( stderr, "  view created.\n" );

    destination_starts[0] = source_handle;
    destination_starts[0].ptr +=
        (UINT_PTR)descriptor_copy_count * descriptor_increment;
    if (test->descriptor_copy == NARROW_FLOAT_DESCRIPTOR_COPY_SIMPLE ||
        test->descriptor_copy == NARROW_FLOAT_DESCRIPTOR_COPY_SIMPLE_64)
    {
        ID3D12Device_CopyDescriptorsSimple(
            device, descriptor_copy_count, destination_starts[0],
            source_handle, descriptor_type );
    }
    else if (test->descriptor_copy == NARROW_FLOAT_DESCRIPTOR_COPY_RANGES ||
             test->descriptor_copy == NARROW_FLOAT_DESCRIPTOR_COPY_RANGES_65)
    {
        source_starts[0] = source_handle;
        descriptor_range_sizes[0] = descriptor_copy_count;
        ID3D12Device_CopyDescriptors(
            device, 1, destination_starts, descriptor_range_sizes,
            1, source_starts, descriptor_range_sizes, descriptor_type );
    }
    fprintf( stderr, "  descriptor ready.\n" );

    hr = ID3D12Device_CreateCommandAllocator(
        device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator );
    if (!check_result( test->name, hr )) goto done;
    hr = ID3D12Device_CreateCommandList(
        device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&command_list );
    if (!check_result( test->name, hr )) goto done;
    fprintf( stderr, "  command list created.\n" );

    if (test->unordered_access)
    {
        gpu_handle =
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart( descriptor_heap );
        if (test->descriptor_copy != NARROW_FLOAT_DESCRIPTOR_DIRECT)
            gpu_handle.ptr += (UINT_PTR)(descriptor_copy_count * 2 - 1) *
                              descriptor_increment;
        descriptor_heaps[0] = descriptor_heap;
        ID3D12GraphicsCommandList_SetDescriptorHeaps(
            command_list, 1, descriptor_heaps );
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(
            command_list, gpu_handle, clear_handle, texture, test->values,
            0, NULL );
    }
    else
    {
        ID3D12GraphicsCommandList_ClearRenderTargetView(
            command_list, clear_handle, test->values, 0, NULL );
    }
    fprintf( stderr, "  clear recorded.\n" );

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = initial_state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier( command_list, 1, &barrier );

    ID3D12Device_GetCopyableFootprints(
        device, &texture_desc, 0, 1, 0, &footprint, &row_count,
        &row_size, &total_size );
    if (!total_size)
    {
        fprintf( stderr, "%s returned an empty copy footprint.\n", test->name );
        goto done;
    }

    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = ID3D12Device_CreateCommittedResource(
        device, &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
        (void **)&readback );
    if (!check_result( test->name, hr )) goto done;

    source_location.pResource = texture;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination_location.pResource = readback;
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination_location.PlacedFootprint = footprint;
    ID3D12GraphicsCommandList_CopyTextureRegion(
        command_list, &destination_location, 0, 0, 0,
        &source_location, NULL );

    hr = ID3D12Device_CreateFence(
        device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
        (void **)&fence );
    if (!check_result( test->name, hr )) goto done;
    if (!(fence_event = CreateEventW( NULL, FALSE, FALSE, NULL )))
    {
        fprintf( stderr, "%s CreateEvent failed: %lu\n",
                 test->name, GetLastError() );
        goto done;
    }

    hr = ID3D12GraphicsCommandList_Close( command_list );
    if (!check_result( test->name, hr )) goto done;
    fprintf( stderr, "  command list closed.\n" );
    command_lists[0] = (ID3D12CommandList *)command_list;
    ID3D12CommandQueue_ExecuteCommandLists( queue, 1, command_lists );
    fprintf( stderr, "  command list submitted.\n" );
    hr = ID3D12CommandQueue_Signal( queue, fence, 1 );
    if (!check_result( test->name, hr )) goto done;
    fprintf( stderr, "  fence signaled.\n" );
    hr = ID3D12Fence_SetEventOnCompletion( fence, 1, fence_event );
    if (!check_result( test->name, hr )) goto done;
    fprintf( stderr, "  fence event armed.\n" );
    if (WaitForSingleObject( fence_event, 30000 ) != WAIT_OBJECT_0)
    {
        fprintf( stderr, "%s timed out waiting for the clear.\n", test->name );
        goto done;
    }

    read_range.Begin = footprint.Offset;
    read_range.End = footprint.Offset + sizeof(actual);
    hr = ID3D12Resource_Map( readback, 0, &read_range, &mapped );
    if (!check_result( test->name, hr ) || !mapped) goto done;
    actual = *(const DWORD *)((const BYTE *)mapped + footprint.Offset);
    ID3D12Resource_Unmap( readback, 0, &written_range );
    mapped = NULL;
    if (actual != test->expected)
    {
        fprintf( stderr, "%s produced %#lx instead of %#lx.\n",
                 test->name, actual, test->expected );
        goto done;
    }

    printf( "%s produced %#lx.\n", test->name, actual );
    ret = 1;

done:
    if (mapped) ID3D12Resource_Unmap( readback, 0, &written_range );
    if (fence_event) CloseHandle( fence_event );
    if (fence) ID3D12Fence_Release( fence );
    if (readback) ID3D12Resource_Release( readback );
    if (texture) ID3D12Resource_Release( texture );
    if (command_list) ID3D12GraphicsCommandList_Release( command_list );
    if (allocator) ID3D12CommandAllocator_Release( allocator );
    if (descriptor_heap) ID3D12DescriptorHeap_Release( descriptor_heap );
    return ret;
}

static int run_narrow_float_clear_case(
    ID3D12Device *device, ID3D12CommandQueue *queue,
    const struct narrow_float_clear_case *test )
{
    return run_narrow_float_clear_case_with_objects(
        device, queue, test, NULL, NULL );
}

static int run_narrow_float_clear_case_with_handle(
    ID3D12Device *device, ID3D12CommandQueue *queue,
    const struct narrow_float_clear_case *test,
    const D3D12_CPU_DESCRIPTOR_HANDLE *provided_handle )
{
    return run_narrow_float_clear_case_with_objects(
        device, queue, test, NULL, provided_handle );
}

static int run_narrow_float_clear_case_with_resource(
    ID3D12Device *device, ID3D12CommandQueue *queue,
    const struct narrow_float_clear_case *test,
    ID3D12Resource *provided_texture )
{
    return run_narrow_float_clear_case_with_objects(
        device, queue, test, provided_texture, NULL );
}

static int verify_narrow_float_clear_conversion( ID3D12Device *device,
                                                ID3D12CommandQueue *queue )
{
    struct narrow_float_clear_case tests[] =
    {
        {
            "R16G16 RTV finite clear", DXGI_FORMAT_R16G16_FLOAT,
            { FLT_MAX, -FLT_MAX, 0.0f, 0.0f }, 0xfbff7bff,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R16G16 RTV default view clear", DXGI_FORMAT_R16G16_FLOAT,
            { FLT_MAX, -FLT_MAX, 0.0f, 0.0f }, 0xfbff7bff,
            FALSE, TRUE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R16G16 RTV simple descriptor copy", DXGI_FORMAT_R16G16_FLOAT,
            { FLT_MAX, -FLT_MAX, 0.0f, 0.0f }, 0xfbff7bff,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_COPY_SIMPLE
        },
        {
            "R16G16 RTV descriptor range copy", DXGI_FORMAT_R16G16_FLOAT,
            { 0.0f, 0.0f, 0.0f, 0.0f }, 0xfc007c00,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_COPY_RANGES
        },
        {
            "R16G16 RTV 64-descriptor simple copy", DXGI_FORMAT_R16G16_FLOAT,
            { FLT_MAX, -FLT_MAX, 0.0f, 0.0f }, 0xfbff7bff,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_COPY_SIMPLE_64
        },
        {
            "R16G16 RTV 65-descriptor range copy", DXGI_FORMAT_R16G16_FLOAT,
            { FLT_MAX, -FLT_MAX, 0.0f, 0.0f }, 0xfbff7bff,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_COPY_RANGES_65
        },
        {
            "R32 RTV finite clear", DXGI_FORMAT_R32_FLOAT,
            { FLT_MAX, 0.0f, 0.0f, 0.0f }, 0x7f7fffff,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R16G16 UAV finite clear", DXGI_FORMAT_R16G16_FLOAT,
            { FLT_MAX, -FLT_MAX, 0.0f, 0.0f }, 0xfbff7bff,
            TRUE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R16G16 UAV default view descriptor copy",
            DXGI_FORMAT_R16G16_FLOAT,
            { FLT_MAX, -FLT_MAX, 0.0f, 0.0f }, 0xfbff7bff,
            TRUE, TRUE, NARROW_FLOAT_DESCRIPTOR_COPY_SIMPLE
        },
        {
            "R11G11B10 RTV finite clear", DXGI_FORMAT_R11G11B10_FLOAT,
            { FLT_MAX, -FLT_MAX, FLT_MAX, 0.0f }, 0xf7c007bf,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R11G11B10 RTV infinity clear", DXGI_FORMAT_R11G11B10_FLOAT,
            { 0.0f, 0.0f, 0.0f, 0.0f }, 0xf80007c0,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R9G9B9E5 RTV finite clear", DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
            { FLT_MAX, -FLT_MAX, FLT_MAX, 0.0f }, 0xfffc01ff,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R9G9B9E5 RTV infinity clear", DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
            { 0.0f, 0.0f, 0.0f, 0.0f }, 0xf80001ff,
            FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R11G11B10 UAV finite clear", DXGI_FORMAT_R11G11B10_FLOAT,
            { FLT_MAX, -FLT_MAX, FLT_MAX, 0.0f }, 0xf7c007bf,
            TRUE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R11G11B10 UAV infinity clear", DXGI_FORMAT_R11G11B10_FLOAT,
            { 0.0f, 0.0f, 0.0f, 0.0f }, 0xf80007c0,
            TRUE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R9G9B9E5 UAV finite clear", DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
            { FLT_MAX, -FLT_MAX, FLT_MAX, 0.0f }, 0xfffc01ff,
            TRUE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
        {
            "R9G9B9E5 UAV infinity clear", DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
            { 0.0f, 0.0f, 0.0f, 0.0f }, 0xf80001ff,
            TRUE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
        },
    };
    unsigned int i;

    tests[3].values[0] = narrow_float_value_from_bits( 0x7f800000 );
    tests[3].values[1] = narrow_float_value_from_bits( 0xff800000 );
    tests[10].values[0] = narrow_float_value_from_bits( 0x7f800000 );
    tests[10].values[1] = narrow_float_value_from_bits( 0xff800000 );
    tests[10].values[2] = narrow_float_value_from_bits( 0x7f800000 );
    tests[12].values[0] = narrow_float_value_from_bits( 0x7f800000 );
    tests[12].values[1] = narrow_float_value_from_bits( 0xff800000 );
    tests[14].values[0] = narrow_float_value_from_bits( 0x7f800000 );
    tests[14].values[1] = narrow_float_value_from_bits( 0xff800000 );
    tests[14].values[2] = narrow_float_value_from_bits( 0x7f800000 );
    tests[16].values[0] = narrow_float_value_from_bits( 0x7f800000 );
    tests[16].values[1] = narrow_float_value_from_bits( 0xff800000 );
    for (i = 0; i < ARRAYSIZE(tests); ++i)
    {
        fprintf( stderr, "Running %s.\n", tests[i].name );
        if (!run_narrow_float_clear_case( device, queue, &tests[i] ))
            return 0;
    }

    return 1;
}

static ULONGLONG count_small_private_rw_regions( void )
{
    MEMORY_BASIC_INFORMATION mbi;
    ULONGLONG count = 0;
    SYSTEM_INFO system_info = {0};
    UINT_PTR address;
    UINT_PTR max_address;
    const SIZE_T small_region_size_limit = 65536;

    GetSystemInfo( &system_info );
    max_address = (UINT_PTR)system_info.lpMaximumApplicationAddress;
    for (address = 0; address <= max_address; )
    {
        SIZE_T info_size = VirtualQuery( (LPCVOID)address, &mbi, sizeof( mbi ));
        if (!info_size)
            break;

        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect & 0xff) == PAGE_READWRITE &&
            mbi.RegionSize <= small_region_size_limit)
            ++count;

        address += mbi.RegionSize;
        if (!mbi.RegionSize)
            break;
    }

    return count;
}

static void emit_vmq_marker( const char *label, ULONGLONG from_count,
                            ULONGLONG to_count )
{
    const char *pause_env = getenv( "SWITCHYARD_D3D12_DESCRIPTOR_STRESS_VMQ_PAUSE_MS" );
    int pause_ms = 0;
    if (pause_env) pause_ms = atoi( pause_env );
    fprintf( stderr, "D3D12 descriptor stress VMQ marker %s: %llu -> %llu.\n",
             label, from_count, to_count );
    if (pause_ms > 0)
    {
        if (pause_ms > 600000)
            pause_ms = 600000;
        fprintf( stderr,
                 "D3D12 descriptor stress VMQ pause requested for %d ms at marker %s.\n",
                 pause_ms, label );
        Sleep( (DWORD)pause_ms );
    }
}

static unsigned int legacy_d3d12_resource_format_hash( const void *resource )
{
    ULONG_PTR hash = (ULONG_PTR)resource >> 4;

    hash ^= hash >> 16;
    hash ^= hash >> 32;
    return hash & (4096 - 1);
}

static int run_descriptor_handle_reuse_stress( ID3D12Device *device,
                                              ID3D12CommandQueue *queue )
{
    const unsigned int resource_live_count = 4097;
    const unsigned int descriptor_warm_count = 1024;
    const unsigned int descriptor_fill_count = 16384;
    const unsigned int descriptor_post_count = 1024;
    const unsigned int descriptor_total_count =
        descriptor_fill_count + descriptor_post_count;
    const unsigned int release_churn_iterations = 256;
    D3D12_HEAP_PROPERTIES heap_properties = {0};
    D3D12_RESOURCE_DESC resource_desc = {0};
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {0};
    D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_heap_start = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    LARGE_INTEGER qpc_frequency;
    LARGE_INTEGER qpc_start;
    LARGE_INTEGER qpc_end;
    LARGE_INTEGER qpc_post_start;
    LARGE_INTEGER qpc_post_end;
    UINT descriptor_increment;
    struct narrow_float_clear_case baseline_case = {
        "R16G16 RTV finite clear (after churn)",
        DXGI_FORMAT_R16G16_FLOAT,
        { FLT_MAX, -FLT_MAX, 0.0f, 0.0f },
        0xfbff7bff, FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
    };
    struct narrow_float_clear_case reuse_case = {
        "R32 RTV finite clear (reused stress descriptor)",
        DXGI_FORMAT_R32_FLOAT,
        { FLT_MAX, 0.0f, 0.0f, 0.0f },
        0x7f7fffff, FALSE, FALSE, NARROW_FLOAT_DESCRIPTOR_DIRECT
    };
    struct narrow_float_clear_case live_resource_case = {
        "R16G16 RTV default view clear (legacy-table eviction victim)",
        DXGI_FORMAT_R16G16_FLOAT,
        { FLT_MAX, -FLT_MAX, 0.0f, 0.0f },
        0xfbff7bff, FALSE, TRUE, NARROW_FLOAT_DESCRIPTOR_DIRECT
    };
    struct narrow_float_clear_case retained_resource_case = {
        "R16G16 RTV default view clear (resource 1 after non-final release)",
        DXGI_FORMAT_R16G16_FLOAT,
        { FLT_MAX, -FLT_MAX, 0.0f, 0.0f },
        0xfbff7bff, FALSE, TRUE, NARROW_FLOAT_DESCRIPTOR_DIRECT
    };
    ID3D12Resource **resources = NULL;
    ID3D12Resource **legacy_slots = NULL;
    ID3D12Resource *legacy_evicted_resource = NULL;
    ID3D12Resource *resource = NULL;
    ID3D12Resource *temp_resource = NULL;
    ID3D12DescriptorHeap *descriptor_heap = NULL;
    HRESULT hr;
    ULONG refcount;
    unsigned int i, probe, start;
    double warm_ms;
    double post_ms;
    double ratio;
    ULONGLONG warm_ticks;
    ULONGLONG post_ticks;
    ULONGLONG warm_count = 0;
    ULONGLONG post_count = descriptor_post_count;
    ULONGLONG vmq_baseline_count = 0;
    ULONGLONG vmq_post_create_count = 0;
    ULONGLONG vmq_post_release_count = 0;
    int ret = 0;
    int performance_failed = 0;
    int vmq_failed = 0;

    printf( "D3D12 descriptor stress start (resources=%u descriptors=%u warm=%u fill=%u post=%u).\n",
            resource_live_count, descriptor_total_count, descriptor_warm_count,
            descriptor_fill_count, descriptor_post_count );
    if (!QueryPerformanceFrequency( &qpc_frequency ))
    {
        fprintf( stderr, "QueryPerformanceFrequency failed: %lu\n", GetLastError() );
        return 0;
    }
    vmq_baseline_count = count_small_private_rw_regions();
    emit_vmq_marker( "baseline", vmq_baseline_count, vmq_baseline_count );

    resources = calloc( resource_live_count, sizeof( *resources ));
    if (!resources)
    {
        fprintf( stderr, "Failed to allocate resource tracking array.\n" );
        goto done;
    }

    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = 1;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rtv_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;
    rtv_desc.Texture2D.PlaneSlice = 0;

    for (i = 0; i < resource_live_count; ++i)
    {
        hr = ID3D12Device_CreateCommittedResource(
            device, &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource,
            (void **)&resource );
        if (!check_result( "D3D12Device_CreateCommittedResource live resource", hr ))
            goto done;

        resources[i] = resource;
        resource = NULL;
    }
    vmq_post_create_count = count_small_private_rw_regions();
    emit_vmq_marker( "after-create", vmq_baseline_count, vmq_post_create_count );

    if (!(legacy_slots = calloc( 4096, sizeof(*legacy_slots) )))
    {
        fprintf( stderr,
                 "Failed to allocate the legacy resource tracking model.\n" );
        goto done;
    }
    for (i = 0; i < 4096; ++i)
    {
        start = legacy_d3d12_resource_format_hash( resources[i] );
        for (probe = 0; probe < 4096; ++probe)
        {
            unsigned int slot = (start + probe) & (4096 - 1);

            if (legacy_slots[slot]) continue;
            legacy_slots[slot] = resources[i];
            break;
        }
        if (probe == 4096)
        {
            fprintf( stderr,
                     "Legacy resource tracking model filled unexpectedly early.\n" );
            goto done;
        }
    }
    start = legacy_d3d12_resource_format_hash( resources[4096] );
    legacy_evicted_resource = legacy_slots[start];
    if (!legacy_evicted_resource)
    {
        fprintf( stderr,
                 "Legacy resource tracking model did not identify an eviction victim.\n" );
        goto done;
    }

    if (!run_narrow_float_clear_case_with_resource(
            device, queue, &live_resource_case, legacy_evicted_resource ))
    {
        fprintf( stderr,
                 "D3D12 descriptor stress lost the legacy-table eviction victim.\n" );
        goto done;
    }

    ID3D12Resource_AddRef( resources[1] );
    refcount = ID3D12Resource_Release( resources[1] );
    if (!refcount)
    {
        fprintf( stderr,
                 "D3D12 descriptor stress resource Release was unexpectedly final.\n" );
        resources[1] = NULL;
        goto done;
    }
    if (!run_narrow_float_clear_case_with_resource(
            device, queue, &retained_resource_case, resources[1] ))
    {
        fprintf( stderr,
                 "D3D12 descriptor stress lost a resource after non-final Release.\n" );
        goto done;
    }

    descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    descriptor_heap_desc.NumDescriptors = descriptor_total_count;
    descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateDescriptorHeap(
        device, &descriptor_heap_desc, &IID_ID3D12DescriptorHeap,
        (void **)&descriptor_heap );
    if (!check_result( "D3D12Device_CreateDescriptorHeap (single descriptor table)", hr ))
        goto done;
    descriptor_heap_start = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart( descriptor_heap );
    if (!descriptor_heap_start.ptr)
    {
        fprintf( stderr, "Null CPU descriptor handle for descriptor heap.\n" );
        goto done;
    }
    descriptor_increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV );

    QueryPerformanceCounter( &qpc_start );
    for (i = 0; i < descriptor_warm_count; ++i)
    {
        handle.ptr = descriptor_heap_start.ptr + (UINT_PTR)i * descriptor_increment;
        ID3D12Device_CreateRenderTargetView(
            device, resources[i % resource_live_count], &rtv_desc, handle );
    }
    QueryPerformanceCounter( &qpc_end );

    for (i = descriptor_warm_count; i < descriptor_fill_count; ++i)
        ID3D12Device_CreateRenderTargetView(
            device, resources[i % resource_live_count], &rtv_desc,
            (D3D12_CPU_DESCRIPTOR_HANDLE){
                .ptr = descriptor_heap_start.ptr + (UINT_PTR)i * descriptor_increment } );

    QueryPerformanceCounter( &qpc_post_start );
    for (i = descriptor_fill_count;
         i < descriptor_fill_count + descriptor_post_count; ++i)
    {
        handle.ptr = descriptor_heap_start.ptr + (UINT_PTR)i * descriptor_increment;
        ID3D12Device_CreateRenderTargetView(
            device, resources[i % resource_live_count], &rtv_desc, handle );
    }
    QueryPerformanceCounter( &qpc_post_end );

    warm_ticks = (ULONGLONG)(qpc_end.QuadPart - qpc_start.QuadPart);
    post_ticks = (ULONGLONG)(qpc_post_end.QuadPart - qpc_post_start.QuadPart);
    warm_count = descriptor_warm_count;
    if (!warm_ticks)
    {
        fprintf( stderr, "Descriptor warm timing measured zero ticks.\n" );
        goto done;
    }
    if (post_ticks * warm_count > warm_ticks * post_count * 5)
    {
        warm_ms = ((double)warm_ticks * 1000.0) / (double)qpc_frequency.QuadPart;
        post_ms = ((double)post_ticks * 1000.0) / (double)qpc_frequency.QuadPart;
        ratio = ((double)post_ticks / (double)post_count)
            / ((double)warm_ticks / (double)warm_count);
        fprintf( stderr, "Descriptor benchmark ratio %.2fx exceeded 5x threshold.\n", ratio );
        fprintf( stderr,
                 "Warm: %.3f ms for %llu descriptors, "
                 "fill: %u descriptors (untimed), Post: %.3f ms for %llu descriptors.\n",
                 warm_ms, warm_count, descriptor_fill_count - descriptor_warm_count,
                 post_ms, post_count );
        performance_failed = 1;
    }
    else
    {
        warm_ms = ((double)warm_ticks * 1000.0) / (double)qpc_frequency.QuadPart;
        post_ms = ((double)post_ticks * 1000.0) / (double)qpc_frequency.QuadPart;
        ratio = ((double)post_ticks / (double)post_count) / ((double)warm_ticks / (double)warm_count);
        printf( "D3D12 descriptor benchmark timings: warm=%.3f ms for %llu, "
                "fill=%u (untimed), post=%.3f ms for %llu, ratio=%.2fx.\n",
                warm_ms, warm_count, descriptor_fill_count - descriptor_warm_count,
                post_ms, post_count, ratio );
    }

    if (!run_narrow_float_clear_case( device, queue, &baseline_case ))
    {
        fprintf( stderr, "D3D12 descriptor stress failed narrow-float clear after writes.\n" );
        goto done;
    }

    handle.ptr = descriptor_heap_start.ptr;
    if (!run_narrow_float_clear_case_with_handle(
            device, queue, &reuse_case, &handle ))
    {
        fprintf( stderr, "D3D12 descriptor stress failed pointer/handle reuse check.\n" );
        goto done;
    }

    refcount = ID3D12DescriptorHeap_Release( descriptor_heap );
    descriptor_heap = NULL;
    if (refcount)
    {
        fprintf( stderr,
                 "D3D12 descriptor stress heap Release retained %lu references.\n",
                 refcount );
        goto done;
    }

    for (i = 0; i < release_churn_iterations; ++i)
    {
        hr = ID3D12Device_CreateCommittedResource(
            device, &heap_properties, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource,
            (void **)&temp_resource );
        if (!check_result( "D3D12Device_CreateCommittedResource release churn", hr ))
            goto done;
        ID3D12Resource_AddRef( temp_resource );
        ID3D12Resource_AddRef( temp_resource );
        ID3D12Resource_Release( temp_resource );
        ID3D12Resource_Release( temp_resource );
        ID3D12Resource_Release( temp_resource );
        temp_resource = NULL;
    }

    for (i = 0; i < resource_live_count; ++i)
    {
        if (resources[i])
        {
            ID3D12Resource_Release( resources[i] );
            resources[i] = NULL;
        }
    }
    vmq_post_release_count = count_small_private_rw_regions();
    emit_vmq_marker( "after-release", vmq_post_create_count, vmq_post_release_count );
    printf( "D3D12 descriptor stress VMQ counts: baseline=%llu create=%llu release=%llu.\n",
            vmq_baseline_count, vmq_post_create_count, vmq_post_release_count );

    if (vmq_post_create_count >= vmq_baseline_count + 1024)
    {
        ULONGLONG create_delta = vmq_post_create_count - vmq_baseline_count;
        ULONGLONG release_delta = vmq_post_release_count > vmq_baseline_count
            ? vmq_post_release_count - vmq_baseline_count : 0;
        if (release_delta * 2 > create_delta)
        {
            vmq_failed = 1;
            fprintf( stderr,
                     "D3D12 descriptor stress VMQ leak gate triggered: "
                     "create_delta=%llu release_delta=%llu.\n",
                    create_delta, release_delta );
        }
    }
    else
    {
        fprintf( stderr, "D3D12 descriptor stress VMQ gate skipped (delta=%llu < 1024, marker).\n",
                 vmq_post_create_count > vmq_baseline_count
                 ? vmq_post_create_count - vmq_baseline_count
                 : 0 );
    }

    if (!performance_failed && !vmq_failed)
        ret = 1;

done:
    for (i = 0; i < resource_live_count; ++i)
        if (resources && resources[i]) ID3D12Resource_Release( resources[i] );
    free( legacy_slots );
    free( resources );
    if (temp_resource) ID3D12Resource_Release( temp_resource );
    if (resource) ID3D12Resource_Release( resource );
    if (descriptor_heap) ID3D12DescriptorHeap_Release( descriptor_heap );
    return ret;
}

int main(int argc, char **argv)
{
    BOOL descriptor_stress_test = FALSE;
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
    ID3D12CommandQueue *second_queue = NULL;
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

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (argc > 1 && !strcmp( argv[1], "--switchyard-chromium-gpu-probe" ))
        return run_chromium_gpu_probe();
    if (argc > 1 && !strcmp( argv[1], "--switchyard-d3d12-descriptor-stress" ))
        descriptor_stress_test = TRUE;

    if (!verify_agility_contract()) goto done;

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
                            &IID_ID3D12Device, NULL );
    if (!check_result( "D3D12CreateDevice capability probe", hr )) goto done;
    if (hr != S_FALSE)
    {
        fprintf( stderr, "D3D12 capability probe returned %#lx instead of S_FALSE.\n", hr );
        goto done;
    }

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
    hr = ID3D12Device_CreateCommandQueue( device, &queue_desc,
                                          &IID_ID3D12CommandQueue,
                                          (void **)&second_queue );
    if (!check_result( "ID3D12Device::CreateCommandQueue (second)", hr ))
        goto done;
    if ((*(void ***)queue)[10] != (*(void ***)second_queue)[10] ||
        (*(void ***)queue)[14] != (*(void ***)second_queue)[14])
    {
        fprintf( stderr,
                 "D3D12 command queues did not share hookable "
                 "ExecuteCommandLists/Signal entry points.\n" );
        goto done;
    }
    if ((*(void ***)queue)[10] == (*(void ***)queue)[14])
    {
        fprintf( stderr,
                 "D3D12 command queue methods incorrectly shared one "
                 "hook entry point.\n" );
        goto done;
    }
    printf( "D3D12 command queue hook entries: %p, %p.\n",
            (*(void ***)queue)[10], (*(void ***)queue)[14] );
    if (descriptor_stress_test && !run_descriptor_handle_reuse_stress( device, queue ))
        goto done;
    if (!verify_narrow_float_clear_conversion( device, queue )) goto done;
#ifdef _WIN64
    if ((ULONG_PTR)(*(void ***)queue)[10] <= MAXDWORD ||
        (ULONG_PTR)(*(void ***)queue)[14] <= MAXDWORD)
    {
        fprintf( stderr,
                 "D3D12 command queue hook entry points were allocated "
                 "in the fragmented low address range.\n" );
        goto done;
    }
#endif
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
    if (second_queue) ID3D12CommandQueue_Release( second_queue );
    if (queue) ID3D12CommandQueue_Release( queue );
    if (device) ID3D12Device_Release( device );
    if (base_device) ID3D12Device_Release( base_device );
    return ret;
}
