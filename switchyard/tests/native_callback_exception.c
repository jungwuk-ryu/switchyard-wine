#define COBJMACROS

#include <windows.h>
#include <d3d11.h>

#include <stdio.h>

typedef HRESULT (WINAPI *get_device_func)(ID3D11DeviceContext *, ID3D11Device **);

int main(void)
{
    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL feature_level;
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    ID3D11Device *output = NULL;
    union
    {
        void (WINAPI *native)(ID3D11DeviceContext *, ID3D11Device **);
        get_device_func test;
    } get_device;
    HRESULT hr;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                           feature_levels, ARRAYSIZE(feature_levels),
                           D3D11_SDK_VERSION, &device, &feature_level,
                           &context);
    if (FAILED(hr))
    {
        fprintf(stderr, "D3D11CreateDevice failed: %#lx\n", hr);
        return 1;
    }

    /*
     * Save the already-wrapped native vtable entry, then call it with an
     * invalid object.  D3DMetal raises a host access violation, which the
     * native callback bridge must contain and return from without recursively
     * faulting in the PE exception dispatcher.
     */
    get_device.native = context->lpVtbl->GetDevice;
    hr = get_device.test(NULL, &output);

    if (hr != E_FAIL || output)
    {
        fprintf(stderr, "Invalid native callback returned %#lx, device %p.\n",
                hr, output);
        if (output) ID3D11Device_Release(output);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        return 1;
    }

    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    puts("Native callback exception dispatch test passed");
    return 0;
}
