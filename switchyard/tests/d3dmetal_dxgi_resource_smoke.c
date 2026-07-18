#define COBJMACROS

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 1920
#define TEST_HEIGHT 1080
#define TEST_PITCH (TEST_WIDTH * 4 + 32)

struct test_context
{
    IDXGIResource *resource;
    HRESULT result;
    HANDLE handle;
};

static void fill_test_pixels( BYTE *pixels, UINT pitch, UINT frame )
{
    UINT x, y;

    for (y = 0; y < TEST_HEIGHT; ++y)
    {
        for (x = 0; x < TEST_WIDTH; ++x)
        {
            BYTE *pixel = pixels + y * pitch + x * 4;

            pixel[0] = 0x12 + x + frame * 7;
            pixel[1] = 0x34 + y + frame * 11;
            pixel[2] = 0x56 ^ x ^ y ^ (frame * 13);
            pixel[3] = 0xff;
        }
    }
}

static int create_device( ID3D11Device **device, ID3D11DeviceContext **context )
{
    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL feature_level;
    HRESULT hr;

    hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                            feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
                            device, &feature_level, context );
    if (FAILED(hr))
    {
        fprintf( stderr, "D3D11CreateDevice failed: %#lx\n", hr );
        return 0;
    }
    return 1;
}

static DWORD WINAPI get_shared_handle_thread( void *arg )
{
    struct test_context *context = arg;

    context->handle = NULL;
    context->result = IDXGIResource_GetSharedHandle( context->resource, &context->handle );
    printf( "GetSharedHandle returned %#lx, handle %p\n", context->result, context->handle );
    return 0;
}

static int run_child( HANDLE shared_handle, UINT frame )
{
    BYTE *pixels;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *texture = NULL;
    ID3D11Resource *resource = NULL;
    ID3D11Device *device = NULL;
    HRESULT hr;

    if (!create_device( &device, &context )) return 1;
    hr = ID3D11Device_OpenSharedResource( device, shared_handle, &IID_ID3D11Resource,
                                          (void **)&resource );
    printf( "OpenSharedResource returned %#lx, resource %p\n", hr, (void *)resource );
    if (FAILED(hr) || !resource) return 1;
    hr = ID3D11Resource_QueryInterface( resource, &IID_ID3D11Texture2D, (void **)&texture );
    if (FAILED(hr) || !texture)
    {
        fprintf( stderr, "QueryInterface(ID3D11Texture2D) failed: %#lx\n", hr );
        return 1;
    }

    if (!(pixels = malloc( (size_t)TEST_PITCH * TEST_HEIGHT )))
    {
        fprintf( stderr, "Allocating producer pixels failed\n" );
        return 1;
    }
    memset( pixels, 0xcc, (size_t)TEST_PITCH * TEST_HEIGHT );
    fill_test_pixels( pixels, TEST_PITCH, frame );
    ID3D11DeviceContext_UpdateSubresource( context, (ID3D11Resource *)texture, 0, NULL,
                                           pixels, TEST_PITCH, 0 );
    ID3D11DeviceContext_Flush( context );
    printf( "Child published frame %u\n", frame );
    free( pixels );

    ID3D11Texture2D_Release( texture );
    ID3D11Resource_Release( resource );
    ID3D11DeviceContext_Release( context );
    ID3D11Device_Release( device );
    return 0;
}

static int abandon_shared_mutant( UINT token )
{
    char name[64];
    HANDLE mutant;
    DWORD wait;

    snprintf( name, sizeof(name), "Global\\SwitchyardD3DShareLock-%08x", token );
    mutant = OpenMutexA( SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name );
    if (!mutant)
    {
        fprintf( stderr, "OpenMutex(%s) failed: %lu\n", name, GetLastError() );
        return 1;
    }
    wait = WaitForSingleObject( mutant, 10000 );
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    {
        fprintf( stderr, "Waiting for shared mutant failed: %#lx\n", wait );
        CloseHandle( mutant );
        return 1;
    }
    /* Process termination must leave the mutant abandoned for the next renderer. */
    printf( "Exiting while owning the shared mutant\n" );
    return 0;
}

static int launch_child( const char *mode, HANDLE shared_handle, UINT frame )
{
    PROCESS_INFORMATION process_info;
    STARTUPINFOA startup_info;
    char command[2 * MAX_PATH + 64];
    char path[MAX_PATH];
    DWORD exit_code, wait;

    if (!GetModuleFileNameA( NULL, path, ARRAYSIZE(path) ))
    {
        fprintf( stderr, "GetModuleFileName failed: %lu\n", GetLastError() );
        return 0;
    }
    snprintf( command, sizeof(command), "\"%s\" %s %#llx %u", path, mode,
              (unsigned long long)(uintptr_t)shared_handle, frame );
    memset( &startup_info, 0, sizeof(startup_info) );
    memset( &process_info, 0, sizeof(process_info) );
    startup_info.cb = sizeof(startup_info);
    if (!CreateProcessA( NULL, command, NULL, NULL, FALSE, 0, NULL, NULL,
                         &startup_info, &process_info ))
    {
        fprintf( stderr, "CreateProcess failed: %lu\n", GetLastError() );
        return 0;
    }
    wait = WaitForSingleObject( process_info.hProcess, 60000 );
    if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess( process_info.hProcess, &exit_code ))
    {
        fprintf( stderr, "Child wait failed: %#lx, error %lu\n", wait, GetLastError() );
        TerminateProcess( process_info.hProcess, 1 );
        exit_code = 1;
    }
    CloseHandle( process_info.hThread );
    CloseHandle( process_info.hProcess );
    if (exit_code)
    {
        fprintf( stderr, "Child exited with code %lu\n", exit_code );
        return 0;
    }
    return 1;
}

static int verify_texture( ID3D11Device *device, ID3D11DeviceContext *context,
                           ID3D11Texture2D *texture, UINT frame )
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11Texture2D *staging = NULL;
    BYTE *expected;
    UINT y;
    HRESULT hr;

    ID3D11Texture2D_GetDesc( texture, &desc );
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D( device, &desc, NULL, &staging );
    if (FAILED(hr))
    {
        fprintf( stderr, "Creating staging texture failed: %#lx\n", hr );
        return 0;
    }
    ID3D11DeviceContext_CopyResource( context, (ID3D11Resource *)staging,
                                      (ID3D11Resource *)texture );
    hr = ID3D11DeviceContext_Map( context, (ID3D11Resource *)staging, 0,
                                  D3D11_MAP_READ, 0, &mapped );
    if (FAILED(hr))
    {
        fprintf( stderr, "Mapping staging texture failed: %#lx\n", hr );
        ID3D11Texture2D_Release( staging );
        return 0;
    }

    if (!(expected = malloc( (size_t)TEST_WIDTH * TEST_HEIGHT * 4 )))
    {
        fprintf( stderr, "Allocating expected pixels failed\n" );
        ID3D11DeviceContext_Unmap( context, (ID3D11Resource *)staging, 0 );
        ID3D11Texture2D_Release( staging );
        return 0;
    }
    fill_test_pixels( expected, TEST_WIDTH * 4, frame );
    for (y = 0; y < TEST_HEIGHT; ++y)
    {
        if (memcmp( (const BYTE *)mapped.pData + y * mapped.RowPitch,
                    expected + y * TEST_WIDTH * 4, TEST_WIDTH * 4 ))
        {
            fprintf( stderr, "Shared texture row %u did not match the producer pixels\n", y );
            ID3D11DeviceContext_Unmap( context, (ID3D11Resource *)staging, 0 );
            ID3D11Texture2D_Release( staging );
            free( expected );
            return 0;
        }
    }
    ID3D11DeviceContext_Unmap( context, (ID3D11Resource *)staging, 0 );
    ID3D11Texture2D_Release( staging );
    free( expected );
    return 1;
}

static int verify_unsupported_shared_texture( ID3D11Device *device,
                                              const D3D11_TEXTURE2D_DESC *supported_desc )
{
    D3D11_TEXTURE2D_DESC desc = *supported_desc;
    ID3D11Texture2D *texture = NULL;
    IDXGIResource *resource = NULL;
    HANDLE handle = NULL;
    HRESULT hr;

    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = ID3D11Device_CreateTexture2D( device, &desc, NULL, &texture );
    if (FAILED(hr)) return 0;
    hr = ID3D11Texture2D_QueryInterface( texture, &IID_IDXGIResource, (void **)&resource );
    if (SUCCEEDED(hr)) hr = IDXGIResource_GetSharedHandle( resource, &handle );
    if (resource) IDXGIResource_Release( resource );
    ID3D11Texture2D_Release( texture );
    if (hr != E_NOTIMPL || handle)
    {
        fprintf( stderr, "Unsupported shared descriptor returned %#lx, handle %p\n", hr, handle );
        return 0;
    }
    return 1;
}

int main( int argc, char **argv )
{
    D3D11_TEXTURE2D_DESC desc =
    {
        TEST_WIDTH, TEST_HEIGHT, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
        { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
        D3D11_RESOURCE_MISC_SHARED,
    };
    struct test_context test = { 0 };
    ID3D11ShaderResourceView *view = NULL;
    ID3D11DeviceContext *deferred_context = NULL;
    ID3D11DeviceContext *other_context = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *texture = NULL;
    ID3D11Device *other_device = NULL;
    ID3D11Device *device = NULL;
    HANDLE second_handle = NULL;
    HANDLE thread;
    HRESULT hr;
    DWORD wait;

    setvbuf( stdout, NULL, _IONBF, 0 );
    setvbuf( stderr, NULL, _IONBF, 0 );
    if (argc >= 3 && !strcmp( argv[1], "child" ))
        return run_child( (HANDLE)(uintptr_t)strtoull( argv[2], NULL, 0 ),
                          argc >= 4 ? strtoul( argv[3], NULL, 0 ) : 0 );
    if (argc >= 3 && !strcmp( argv[1], "abandon" ))
        return abandon_shared_mutant( strtoul( argv[2], NULL, 0 ) );

    if (!create_device( &device, &context )) return 1;
    if (!create_device( &other_device, &other_context )) return 1;
    hr = ID3D11Device_CreateDeferredContext( device, 0, &deferred_context );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateDeferredContext failed: %#lx\n", hr );
        return 1;
    }
    if (!verify_unsupported_shared_texture( device, &desc )) return 1;
    hr = ID3D11Device_CreateTexture2D( device, &desc, NULL, &texture );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateTexture2D failed: %#lx\n", hr );
        return 1;
    }
    hr = ID3D11Device_CreateShaderResourceView( device, (ID3D11Resource *)texture,
                                                NULL, &view );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateShaderResourceView failed: %#lx\n", hr );
        return 1;
    }
    hr = ID3D11Texture2D_QueryInterface( texture, &IID_IDXGIResource,
                                         (void **)&test.resource );
    if (FAILED(hr))
    {
        fprintf( stderr, "QueryInterface(IDXGIResource) failed: %#lx\n", hr );
        return 1;
    }

    thread = CreateThread( NULL, 0, get_shared_handle_thread, &test, 0, NULL );
    if (!thread)
    {
        fprintf( stderr, "CreateThread failed: %lu\n", GetLastError() );
        return 1;
    }
    wait = WaitForSingleObject( thread, 30000 );
    CloseHandle( thread );
    if (wait != WAIT_OBJECT_0 || FAILED(test.result) || !test.handle)
    {
        fprintf( stderr, "GetSharedHandle worker failed: wait %#lx, hr %#lx, handle %p\n",
                 wait, test.result, test.handle );
        return 1;
    }
    hr = IDXGIResource_GetSharedHandle( test.resource, &second_handle );
    printf( "Second GetSharedHandle returned %#lx, handle %p\n", hr, second_handle );
    if (FAILED(hr) || second_handle != test.handle) return 1;
    ID3D11DeviceContext_PSSetShaderResources( context, 0, 1, &view );
    if (!launch_child( "abandon", test.handle, 0 )) return 1;
    if (!launch_child( "child", test.handle, 1 )) return 1;
    ID3D11DeviceContext_Draw( other_context, 0, 0 );
    ID3D11DeviceContext_Draw( deferred_context, 0, 0 );
    ID3D11DeviceContext_Draw( context, 0, 0 );
    if (!verify_texture( device, context, texture, 1 )) return 1;
    if (!launch_child( "child", test.handle, 2 )) return 1;
    ID3D11DeviceContext_Draw( context, 0, 0 );
    if (!verify_texture( device, context, texture, 2 )) return 1;
    printf( "Cross-process shared texture frames matched without rebinding\n" );

    ID3D11DeviceContext_PSSetShaderResources( context, 0, 0, NULL );
    IDXGIResource_Release( test.resource );
    ID3D11ShaderResourceView_Release( view );
    ID3D11Texture2D_Release( texture );
    ID3D11DeviceContext_Release( deferred_context );
    ID3D11DeviceContext_Release( other_context );
    ID3D11DeviceContext_Release( context );
    ID3D11Device_Release( other_device );
    ID3D11Device_Release( device );
    return 0;
}
