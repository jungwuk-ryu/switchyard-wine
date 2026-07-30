#define COBJMACROS

#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

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

typedef PVOID (WINAPI *rtl_pc_to_file_header_func)(PVOID, PVOID *);

static int verify_callback_module(const char *object, void *entry, unsigned int index,
                                  HMODULE expected,
                                  rtl_pc_to_file_header_func rtl_pc_to_file_header)
{
    HMODULE module = NULL;
    void *header = NULL;

    if (!GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (const WCHAR *)entry, &module ) || module != expected)
    {
        fprintf( stderr, "%s entry %u resolved to module %p, expected %p\n",
                 object, index, module, expected );
        return 0;
    }

    if (rtl_pc_to_file_header( entry, &header ) != expected || header != expected)
    {
        fprintf( stderr, "RtlPcToFileHeader(%s entry %u) returned %p, expected %p\n",
                 object, index, header, expected );
        return 0;
    }

    return 1;
}

static int verify_dxgi_factory_module(void)
{
    union
    {
        FARPROC proc;
        rtl_pc_to_file_header_func rtl_pc_to_file_header;
    } function;
    static const unsigned int entries[] = { 10, 15 };
    static const BYTE hotpatch[8] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    rtl_pc_to_file_header_func rtl_pc_to_file_header;
    IDXGIFactory2 *second_factory = NULL;
    IDXGIFactory2 *factory = NULL;
    BYTE saved[sizeof(hotpatch)];
    HMODULE expected, ntdll;
    DWORD old_protect, unused;
    void **vtable, *header;
    void *patched_entry;
    HRESULT hr;
    unsigned int i;
    int hotpatch_ok = 0;

    expected = GetModuleHandleW( L"dxgi.dll" );
    ntdll = GetModuleHandleW( L"ntdll.dll" );
    function.proc = ntdll ? GetProcAddress( ntdll, "RtlPcToFileHeader" ) : NULL;
    rtl_pc_to_file_header = function.rtl_pc_to_file_header;
    if (!expected || !rtl_pc_to_file_header)
    {
        fprintf( stderr, "Could not resolve DXGI or RtlPcToFileHeader\n" );
        return 0;
    }

    hr = CreateDXGIFactory1( &IID_IDXGIFactory2, (void **)&factory );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateDXGIFactory1(IDXGIFactory2) failed: %#lx\n", hr );
        return 0;
    }

    vtable = *(void ***)factory;
    for (i = 0; i < ARRAYSIZE(entries); ++i)
    {
        if (!verify_callback_module( "DXGI factory", vtable[entries[i]], entries[i],
                                     expected, rtl_pc_to_file_header ))
        {
            IDXGIFactory2_Release( factory );
            return 0;
        }
    }

    patched_entry = vtable[15];
    if (!VirtualProtect( patched_entry, sizeof(hotpatch), PAGE_EXECUTE_READWRITE,
                         &old_protect ))
    {
        fprintf( stderr, "Could not make factory entry 15 writable: %lu\n", GetLastError() );
        IDXGIFactory2_Release( factory );
        return 0;
    }
    memcpy( saved, patched_entry, sizeof(saved) );
    memcpy( patched_entry, hotpatch, sizeof(hotpatch) );
    FlushInstructionCache( GetCurrentProcess(), patched_entry, sizeof(hotpatch) );

    if (!verify_callback_module( "DXGI factory", patched_entry, 15, expected,
                                 rtl_pc_to_file_header ))
        goto restore_hotpatch;

    hr = CreateDXGIFactory1( &IID_IDXGIFactory2, (void **)&second_factory );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateDXGIFactory1 after hotpatch failed: %#lx\n", hr );
        goto restore_hotpatch;
    }
    if ((*(void ***)second_factory)[15] != patched_entry)
    {
        fprintf( stderr, "Factory entry 15 was rewrapped after hotpatch: %p -> %p\n",
                 patched_entry, (*(void ***)second_factory)[15] );
        goto restore_hotpatch;
    }
    hotpatch_ok = 1;

restore_hotpatch:
    memcpy( patched_entry, saved, sizeof(saved) );
    FlushInstructionCache( GetCurrentProcess(), patched_entry, sizeof(saved) );
    VirtualProtect( patched_entry, sizeof(saved), old_protect, &unused );
    if (second_factory) IDXGIFactory2_Release( second_factory );
    if (!hotpatch_ok)
    {
        IDXGIFactory2_Release( factory );
        return 0;
    }

    header = (void *)(uintptr_t)1;
    if (rtl_pc_to_file_header( (void *)(uintptr_t)1, &header ) || header)
    {
        fprintf( stderr, "RtlPcToFileHeader accepted an unmapped address\n" );
        IDXGIFactory2_Release( factory );
        return 0;
    }

    IDXGIFactory2_Release( factory );
    printf( "DXGI factory callback module attribution passed\n" );
    return 1;
}

static int verify_factory_adapter(void)
{
    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    DXGI_ADAPTER_DESC desc;
    D3D_FEATURE_LEVEL feature_level;
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIAdapter1 *adapter1 = NULL;
    IDXGIFactory1 *factory = NULL;
    HRESULT hr;
    unsigned int adapter_index;
    unsigned int i;

    for (i = 0; i < 8; ++i)
    {
        hr = CreateDXGIFactory1( &IID_IDXGIFactory1, (void **)&factory );
        if (FAILED(hr))
        {
            fprintf( stderr, "CreateDXGIFactory1 failed: %#lx\n", hr );
            return 0;
        }
        hr = IDXGIFactory1_EnumAdapters( factory, 0, &adapter );
        if (FAILED(hr))
        {
            fprintf( stderr, "EnumAdapters failed: %#lx\n", hr );
            IDXGIFactory1_Release( factory );
            return 0;
        }
        hr = IDXGIAdapter_QueryInterface( adapter, &IID_IDXGIAdapter1,
                                          (void **)&adapter1 );
        if (FAILED(hr))
        {
            fprintf( stderr, "QueryInterface(IDXGIAdapter1) failed: %#lx\n", hr );
            IDXGIAdapter_Release( adapter );
            IDXGIFactory1_Release( factory );
            return 0;
        }
        hr = IDXGIAdapter_GetDesc( adapter, &desc );
        if (FAILED(hr) || !desc.DeviceId)
        {
            fprintf( stderr, "IDXGIAdapter::GetDesc returned %#lx, device %#x\n",
                     hr, desc.DeviceId );
            IDXGIAdapter1_Release( adapter1 );
            IDXGIAdapter_Release( adapter );
            IDXGIFactory1_Release( factory );
            return 0;
        }
        hr = D3D11CreateDevice( adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                feature_levels, ARRAYSIZE(feature_levels),
                                D3D11_SDK_VERSION, &device, &feature_level, &context );
        if (FAILED(hr))
        {
            fprintf( stderr, "D3D11CreateDevice(adapter) failed: %#lx\n", hr );
            IDXGIAdapter1_Release( adapter1 );
            IDXGIAdapter_Release( adapter );
            IDXGIFactory1_Release( factory );
            return 0;
        }
        ID3D11DeviceContext_Release( context );
        ID3D11Device_Release( device );
        IDXGIAdapter1_Release( adapter1 );
        IDXGIAdapter_Release( adapter );
        for (adapter_index = 1; ; ++adapter_index)
        {
            adapter = NULL;
            hr = IDXGIFactory1_EnumAdapters( factory, adapter_index, &adapter );
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr))
            {
                fprintf( stderr, "EnumAdapters(%u) failed: %#lx\n",
                         adapter_index, hr );
                IDXGIFactory1_Release( factory );
                return 0;
            }
            hr = IDXGIAdapter_GetDesc( adapter, &desc );
            IDXGIAdapter_Release( adapter );
            if (FAILED(hr))
            {
                fprintf( stderr, "IDXGIAdapter::GetDesc(%u) failed: %#lx\n",
                         adapter_index, hr );
                IDXGIFactory1_Release( factory );
                return 0;
            }
        }
        IDXGIFactory1_Release( factory );
        context = NULL;
        device = NULL;
        hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                feature_levels, ARRAYSIZE(feature_levels),
                                D3D11_SDK_VERSION, &device, &feature_level, &context );
        if (FAILED(hr))
        {
            fprintf( stderr, "D3D11CreateDevice(default) failed: %#lx\n", hr );
            return 0;
        }
        ID3D11DeviceContext_Release( context );
        ID3D11Device_Release( device );
        context = NULL;
        device = NULL;
        adapter1 = NULL;
        adapter = NULL;
        factory = NULL;
    }

    printf( "DXGI adapter callback stress passed (device %#x)\n", desc.DeviceId );
    return 1;
}

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
                           ID3D11Texture2D *texture, UINT frame );

struct execute_thread_context
{
    ID3D11Device *device;
    ID3D11DeviceContext *immediate_context;
    ID3D11ShaderResourceView *view;
    HANDLE start_event;
    HANDLE ready_event;
    unsigned int iterations;
    int result;
};

static DWORD WINAPI execute_callback_thread( void *arg )
{
    struct execute_thread_context *context = arg;
    ID3D11DeviceContext *deferred_context = NULL;
    ID3D11CommandList *command_list = NULL;
    HRESULT hr;
    unsigned int i;
    BOOL ready = FALSE;

    context->result = 0;
    hr = ID3D11Device_CreateDeferredContext( context->device, 0, &deferred_context );
    if (FAILED(hr))
    {
        fprintf( stderr, "CreateDeferredContext in execute worker failed: %#lx\n", hr );
        context->result = 1;
        goto cleanup;
    }
    ID3D11DeviceContext_PSSetShaderResources( deferred_context, 0, 1, &context->view );
    ID3D11DeviceContext_Draw( deferred_context, 0, 0 );
    hr = ID3D11DeviceContext_FinishCommandList( deferred_context, TRUE, &command_list );
    ID3D11DeviceContext_Release( deferred_context );
    deferred_context = NULL;
    if (FAILED(hr) || !command_list)
    {
        fprintf( stderr, "FinishCommandList in execute worker failed: %#lx\n", hr );
        context->result = 1;
        goto cleanup;
    }

    ready = context->ready_event && SetEvent( context->ready_event );
    if (!ready || !context->start_event ||
        WaitForSingleObject( context->start_event, 60000 ) != WAIT_OBJECT_0)
    {
        fprintf( stderr, "Execute worker missed start event\n" );
        context->result = 1;
        goto cleanup;
    }

    for (i = 0; i < context->iterations; ++i)
    {
        ID3D11DeviceContext_ExecuteCommandList( context->immediate_context, command_list, FALSE );
        ID3D11DeviceContext_Flush( context->immediate_context );
    }

cleanup:
    if (!ready && context->ready_event) SetEvent( context->ready_event );
    if (deferred_context) ID3D11DeviceContext_Release( deferred_context );
    if (command_list) ID3D11CommandList_Release( command_list );
    return context->result;
}

struct child_upload_context
{
    HANDLE shared_handle;
    UINT frame;
    int result;
};

static DWORD WINAPI child_upload_thread( void *arg )
{
    struct child_upload_context *context = arg;

    context->result = !launch_child( "child", context->shared_handle, context->frame );
    return 0;
}

static void join_test_thread( HANDLE thread, const char *name )
{
    DWORD wait;

    if (!thread) return;
    wait = WaitForSingleObject( thread, 60000 );
    if (wait != WAIT_OBJECT_0)
        wait = WaitForSingleObject( thread, 5000 );
    if (wait == WAIT_OBJECT_0) return;

    fprintf( stderr, "%s worker did not stop (wait %#lx); terminating smoke process\n",
             name, wait );
    TerminateProcess( GetCurrentProcess(), 1 );
    ExitProcess( 1 );
}

static int run_d3d11_relay_stress( ID3D11Device *device, ID3D11DeviceContext *context,
                                   ID3D11Device *other_device, ID3D11DeviceContext *other_context )
{
    const D3D11_TEXTURE2D_DESC desc =
    {
        TEST_WIDTH, TEST_HEIGHT, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM,
        { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
        D3D11_RESOURCE_MISC_SHARED,
    };
    const unsigned int stress_rounds = 6;
    ID3D11Texture2D *owner_textures[2] = { NULL, NULL };
    ID3D11ShaderResourceView *owner_views[2] = { NULL, NULL };
    IDXGIResource *owner_resources[2] = { NULL, NULL };
    HANDLE shared_handles[2] = { NULL, NULL };
    ID3D11Multithread *multithread = NULL;
    struct execute_thread_context execute_context[2];
    struct child_upload_context upload_contexts[2];
    HANDLE start_event = NULL;
    HANDLE execute_threads[2] = { NULL, NULL };
    HANDLE execute_ready[2] = { NULL, NULL };
    HANDLE upload_threads[2] = { NULL, NULL };
    HRESULT hr;
    unsigned int i, round;
    int owned_zero_released = 0;
    int ret = 0;
    const unsigned int final_round = stress_rounds - 1;

    memset( execute_context, 0, sizeof(execute_context) );
    memset( upload_contexts, 0, sizeof(upload_contexts) );
    if (!device || !context || !other_device || !other_context)
    {
        fprintf( stderr, "run_d3d11_relay_stress missing required device/context\n" );
        return 0;
    }
    (void)other_device;

    for (i = 0; i < 2; ++i)
    {
        hr = ID3D11Device_CreateTexture2D( device, &desc, NULL, &owner_textures[i] );
        if (FAILED(hr))
        {
            fprintf( stderr, "CreateTexture2D(owner %u) failed: %#lx\n", i, hr );
            ret = 1;
            goto cleanup;
        }
        hr = ID3D11Device_CreateShaderResourceView( device, (ID3D11Resource *)owner_textures[i],
                                                   NULL, &owner_views[i] );
        if (FAILED(hr))
        {
            fprintf( stderr, "CreateShaderResourceView(owner %u) failed: %#lx\n", i, hr );
            ret = 1;
            goto cleanup;
        }
        hr = ID3D11Texture2D_QueryInterface( owner_textures[i], &IID_IDXGIResource,
                                             (void **)&owner_resources[i] );
        if (FAILED(hr))
        {
            fprintf( stderr, "QueryInterface(owner %u IDXGIResource) failed: %#lx\n", i, hr );
            ret = 1;
            goto cleanup;
        }
        hr = IDXGIResource_GetSharedHandle( owner_resources[i], &shared_handles[i] );
        if (FAILED(hr) || !shared_handles[i])
        {
            fprintf( stderr, "GetSharedHandle(owner %u) failed: %#lx, handle %p\n", i, hr,
                     shared_handles[i] );
            ret = 1;
            goto cleanup;
        }
        printf( "Relay stress owner %u shared handle %p\n", i, shared_handles[i] );
    }

    hr = ID3D11DeviceContext_QueryInterface( context, &IID_ID3D11Multithread,
                                             (void **)&multithread );
    if (FAILED(hr) || !multithread)
    {
        fprintf( stderr, "QueryInterface(ID3D11Multithread) failed: %#lx\n", hr );
        ret = 1;
        goto cleanup;
    }
    ID3D11Multithread_SetMultithreadProtected( multithread, TRUE );

    start_event = CreateEventA( NULL, TRUE, FALSE, NULL );
    if (!start_event)
    {
        fprintf( stderr, "CreateEvent for execute workers failed: %lu\n", GetLastError() );
        ret = 1;
        goto cleanup;
    }

    for (round = 0; round < stress_rounds && !ret; ++round)
    {
        UINT owner0_frame = 100 + round;
        UINT owner1_frame = 200 + round;

        upload_contexts[0].shared_handle = shared_handles[0];
        upload_contexts[0].frame = owner0_frame;
        upload_contexts[1].shared_handle = shared_handles[1];
        upload_contexts[1].frame = owner1_frame;
        upload_contexts[0].result = 0;
        upload_contexts[1].result = 0;
        upload_threads[0] = CreateThread( NULL, 0, child_upload_thread, &upload_contexts[0], 0, NULL );
        upload_threads[1] = CreateThread( NULL, 0, child_upload_thread, &upload_contexts[1], 0, NULL );
        if (!upload_threads[0] || !upload_threads[1])
        {
            fprintf( stderr, "CreateThread for upload workers failed: %lu\n", GetLastError() );
            ret = 1;
            goto cleanup;
        }

        join_test_thread( upload_threads[0], "child upload 0" );
        join_test_thread( upload_threads[1], "child upload 1" );
        if (upload_contexts[0].result || upload_contexts[1].result)
        {
            fprintf( stderr, "Child upload worker failed in round %u (%d,%d)\n", round,
                     upload_contexts[0].result, upload_contexts[1].result );
            ret = 1;
            goto cleanup;
        }
        CloseHandle( upload_threads[0] );
        CloseHandle( upload_threads[1] );
        upload_threads[0] = NULL;
        upload_threads[1] = NULL;
        ID3D11DeviceContext_Draw( other_context, 0, 0 );

        if (round == final_round && !owned_zero_released)
        {
            ID3D11DeviceContext_PSSetShaderResources( context, 0, 0, NULL );
            ID3D11DeviceContext_Flush( context );
            if (owner_views[0]) ID3D11ShaderResourceView_Release( owner_views[0] );
            owner_views[0] = NULL;
            if (owner_textures[0]) ID3D11Texture2D_Release( owner_textures[0] );
            owner_textures[0] = NULL;
            owned_zero_released = 1;
            printf( "Relay stress starting final release race for owner[0]\n" );

            for (i = 0; i < 2; ++i)
            {
                execute_ready[i] = CreateEventA( NULL, TRUE, FALSE, NULL );
                if (!execute_ready[i])
                {
                    fprintf( stderr, "CreateEvent for execute ready %u failed: %lu\n",
                             i, GetLastError() );
                    ret = 1;
                    goto cleanup;
                }
                execute_context[i].device = device;
                execute_context[i].immediate_context = context;
                execute_context[i].iterations = 64;
                execute_context[i].start_event = start_event;
                execute_context[i].ready_event = execute_ready[i];
                execute_context[i].view = i ? owner_views[1] : NULL;
                execute_context[i].result = 0;
            }
            for (i = 0; i < 2; ++i)
            {
                execute_threads[i] = CreateThread( NULL, 0, execute_callback_thread,
                                                  &execute_context[i], 0, NULL );
                if (!execute_threads[i])
                {
                    fprintf( stderr, "CreateThread for execute worker %u failed: %lu\n", i, GetLastError() );
                    ret = 1;
                    goto cleanup;
                }
            }
            if (WaitForMultipleObjects( 2, execute_ready, TRUE, 60000 ) != WAIT_OBJECT_0)
            {
                fprintf( stderr, "Execute workers did not reach the release-race barrier\n" );
                ret = 1;
                goto cleanup;
            }
            SetEvent( start_event );
            Sleep( 1 );
            if (owner_resources[0]) IDXGIResource_Release( owner_resources[0] );
            owner_resources[0] = NULL;
            join_test_thread( execute_threads[0], "execute 0" );
            join_test_thread( execute_threads[1], "execute 1" );
        }
        else
        {
            ID3D11DeviceContext_Draw( context, 0, 0 );
            if (!owned_zero_released && !verify_texture( device, context, owner_textures[0], owner0_frame ))
            {
                ret = 1;
                goto cleanup;
            }
        }
        if (!verify_texture( device, context, owner_textures[1], owner1_frame ))
        {
            ret = 1;
            goto cleanup;
        }
    }

    printf( "Relay stress upload and execute callback interleaving completed\n" );

cleanup:
    join_test_thread( upload_threads[0], "child upload 0 cleanup" );
    join_test_thread( upload_threads[1], "child upload 1 cleanup" );
    if (upload_threads[0]) CloseHandle( upload_threads[0] );
    if (upload_threads[1]) CloseHandle( upload_threads[1] );
    upload_threads[0] = NULL;
    upload_threads[1] = NULL;
    if (start_event) SetEvent( start_event );
    join_test_thread( execute_threads[0], "execute 0 cleanup" );
    join_test_thread( execute_threads[1], "execute 1 cleanup" );
    for (i = 0; i < 2; ++i)
    {
        if (execute_threads[i]) CloseHandle( execute_threads[i] );
        if (execute_ready[i]) CloseHandle( execute_ready[i] );
        if (execute_context[i].result) ret = 1;
        if (owner_views[i]) ID3D11ShaderResourceView_Release( owner_views[i] );
        if (owner_resources[i]) IDXGIResource_Release( owner_resources[i] );
        if (owner_textures[i]) ID3D11Texture2D_Release( owner_textures[i] );
    }
    if (start_event) CloseHandle( start_event );
    if (multithread) ID3D11Multithread_Release( multithread );
    if (ret)
        fprintf( stderr, "Relay stress failed; deterministic cleanup performed\n" );
    return !ret;
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
    if (argc >= 2 && !strcmp( argv[1], "adapter" ))
        return verify_factory_adapter() ? 0 : 1;

    if (!verify_dxgi_factory_module()) return 1;
    if (!verify_factory_adapter()) return 1;
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
    if (!run_d3d11_relay_stress( device, context, other_device, other_context )) return 1;

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
