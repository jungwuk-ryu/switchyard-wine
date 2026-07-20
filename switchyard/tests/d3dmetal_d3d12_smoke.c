#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS

#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

#include <stdio.h>
#include <string.h>

static int check_result( const char *operation, HRESULT hr )
{
    if (SUCCEEDED(hr)) return 1;
    fprintf( stderr, "%s failed: %#lx\n", operation, hr );
    return 0;
}

int main(void)
{
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {0};
    D3D12_DESCRIPTOR_HEAP_DESC returned_descriptor_heap_desc;
    D3D12_HEAP_PROPERTIES heap_properties = {0};
    D3D12_RESOURCE_DESC resource_desc = {0};
    D3D12_RESOURCE_DESC returned_resource_desc;
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info;
    D3D12_COMMAND_QUEUE_DESC returned_queue_desc;
    ID3D12GraphicsCommandList *command_list = NULL;
    ID3D12DescriptorHeap *descriptor_heap = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12Pageable *pageable = NULL;
    ID3D12Resource *resource = NULL;
    ID3D12Device *queue_device = NULL;
    ID3D12Device *device = NULL;
    ID3D12Fence *fence = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor_handle;
    ID3D12CommandList *command_lists[1];
    UINT64 timestamp_frequency = 0;
    UINT64 completed_value;
    void *mapped_data = NULL;
    HRESULT hr;
    int ret = 1;

    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_11_0,
                            &IID_ID3D12Device, (void **)&device );
    if (!check_result( "D3D12CreateDevice", hr )) goto done;

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
    hr = ID3D12GraphicsCommandList_Close( command_list );
    if (!check_result( "ID3D12GraphicsCommandList::Close", hr )) goto done;
    command_lists[0] = (ID3D12CommandList *)command_list;
    ID3D12CommandQueue_ExecuteCommandLists( queue, 1, command_lists );

    hr = ID3D12Device_CreateFence( device, 0, D3D12_FENCE_FLAG_NONE,
                                   &IID_ID3D12Fence, (void **)&fence );
    if (!check_result( "ID3D12Device::CreateFence", hr )) goto done;
    hr = ID3D12CommandQueue_Signal( queue, fence, 1 );
    if (!check_result( "ID3D12CommandQueue::Signal", hr )) goto done;
    completed_value = ID3D12Fence_GetCompletedValue( fence );
    printf( "D3D12 fence completed value: %llu\n", (unsigned long long)completed_value );

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
    if (resource)
    {
        if (mapped_data) ID3D12Resource_Unmap( resource, 0, NULL );
        ID3D12Resource_Release( resource );
    }
    if (descriptor_heap) ID3D12DescriptorHeap_Release( descriptor_heap );
    if (fence) ID3D12Fence_Release( fence );
    if (command_list) ID3D12GraphicsCommandList_Release( command_list );
    if (allocator) ID3D12CommandAllocator_Release( allocator );
    if (pageable) ID3D12Pageable_Release( pageable );
    if (queue_device) ID3D12Device_Release( queue_device );
    if (queue) ID3D12CommandQueue_Release( queue );
    if (device) ID3D12Device_Release( device );
    return ret;
}
