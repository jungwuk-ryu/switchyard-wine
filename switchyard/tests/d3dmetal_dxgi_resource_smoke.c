#define COBJMACROS

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <stdio.h>

struct test_context
{
    IDXGIResource *resource;
    HRESULT result;
};

static int shared_handle_result_is_valid( const char *call, HRESULT hr )
{
    if (SUCCEEDED(hr) || hr == E_NOTIMPL) return 1;

    fprintf( stderr, "%s returned unexpected failure %#lx\n", call, hr );
    return 0;
}

static DWORD WINAPI get_shared_handle_thread( void *arg )
{
    struct test_context *context = arg;
    HANDLE handle = NULL;

    context->result = IDXGIResource_GetSharedHandle( context->resource, &handle );
    printf( "GetSharedHandle returned %#lx, handle %p\n", context->result, handle );
    return 0;
}

int main(void)
{
    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D11_TEXTURE2D_DESC desc =
    {
        16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
        D3D11_RESOURCE_MISC_SHARED,
    };
    struct test_context context = { 0 };
    D3D_FEATURE_LEVEL feature_level;
    ID3D11Texture2D *texture = NULL;
    ID3D11Device *device = NULL;
    ID3D11Device *queried_device = NULL;
    ID3D11DeviceContext *immediate_context = NULL;
    IDXGIResource1 *resource1 = NULL;
    DXGI_USAGE usage = 0;
    UINT eviction_priority = 0;
    HANDLE shared_handle = NULL;
    HANDLE thread;
    HRESULT hr;
    DWORD wait;

    setvbuf( stdout, NULL, _IONBF, 0 );
    setvbuf( stderr, NULL, _IONBF, 0 );

    hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                            feature_levels, sizeof(feature_levels) / sizeof(feature_levels[0]),
                            D3D11_SDK_VERSION,
                            &device, &feature_level, &immediate_context );
    if (FAILED(hr))
    {
        fprintf( stderr, "D3D11CreateDevice failed: %#lx\n", hr );
        return 1;
    }

    hr = ID3D11Device_CreateTexture2D( device, &desc, NULL, &texture );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateTexture2D failed: %#lx\n", hr );
        return 1;
    }

    hr = ID3D11Texture2D_QueryInterface( texture, &IID_IDXGIResource,
                                         (void **)&context.resource );
    if (FAILED(hr))
    {
        fprintf( stderr, "QueryInterface(IDXGIResource) failed: %#lx\n", hr );
        return 1;
    }

    thread = CreateThread( NULL, 0, get_shared_handle_thread, &context, 0, NULL );
    if (!thread)
    {
        fprintf( stderr, "CreateThread failed: %lu\n", GetLastError() );
        return 1;
    }
    wait = WaitForSingleObject( thread, 30000 );
    CloseHandle( thread );
    if (wait != WAIT_OBJECT_0)
    {
        fprintf( stderr, "GetSharedHandle worker timed out: %#lx\n", wait );
        return 1;
    }
    if (!shared_handle_result_is_valid( "Worker GetSharedHandle", context.result ))
        return 1;

    hr = IDXGIResource_GetSharedHandle( context.resource, &shared_handle );
    printf( "Second GetSharedHandle returned %#lx, handle %p\n", hr, shared_handle );
    if (!shared_handle_result_is_valid( "Second GetSharedHandle", hr )) return 1;

    hr = IDXGIResource_GetDevice( context.resource, &IID_ID3D11Device,
                                  (void **)&queried_device );
    printf( "GetDevice returned %#lx, device %p\n", hr, (void *)queried_device );
    if (FAILED(hr) || !queried_device) return 1;
    ID3D11Device_Release( queried_device );

    hr = IDXGIResource_GetUsage( context.resource, &usage );
    printf( "GetUsage returned %#lx, usage %#x\n", hr, usage );

    hr = IDXGIResource_SetEvictionPriority( context.resource, 0 );
    printf( "SetEvictionPriority returned %#lx\n", hr );
    if (FAILED(hr)) return 1;
    hr = IDXGIResource_GetEvictionPriority( context.resource, &eviction_priority );
    printf( "GetEvictionPriority returned %#lx, priority %#x\n", hr, eviction_priority );
    if (FAILED(hr)) return 1;

    hr = ID3D11Texture2D_QueryInterface( texture, &IID_IDXGIResource1,
                                         (void **)&resource1 );
    printf( "QueryInterface(IDXGIResource1) returned %#lx\n", hr );
    if (SUCCEEDED(hr))
    {
        int valid;

        shared_handle = NULL;
        hr = IDXGIResource1_CreateSharedHandle(
            resource1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            NULL, &shared_handle );
        printf( "CreateSharedHandle returned %#lx, handle %p\n", hr, shared_handle );
        valid = shared_handle_result_is_valid( "CreateSharedHandle", hr );
        if (shared_handle) CloseHandle( shared_handle );
        IDXGIResource1_Release( resource1 );
        if (!valid) return 1;
    }

    IDXGIResource_Release( context.resource );
    ID3D11Texture2D_Release( texture );
    ID3D11DeviceContext_Release( immediate_context );
    ID3D11Device_Release( device );
    return 0;
}
