/* Verify provider-owned and explicitly overridden graphics identities. */
#define COBJMACROS

#include <windows.h>
#include <dxgi1_2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_number(const char *name, unsigned int *value)
{
    char buffer[32], *end;
    DWORD length;
    unsigned long parsed;

    length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (!length || length >= sizeof(buffer))
    {
        fprintf(stderr, "Missing capability variable %s.\n", name);
        return 0;
    }

    parsed = strtoul(buffer, &end, 0);
    if (!*buffer || *end || parsed > UINT_MAX)
    {
        fprintf(stderr, "Invalid capability variable %s=%s.\n", name, buffer);
        return 0;
    }
    *value = parsed;
    return 1;
}

static int require_value(const char *name, const char *expected)
{
    char buffer[64];
    DWORD length;

    length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (!length || length >= sizeof(buffer) || strcmp(buffer, expected))
    {
        fprintf(stderr, "Expected %s=%s.\n", name, expected);
        return 0;
    }
    return 1;
}

static int require_absent(const char *name)
{
    char buffer[2];

    if (GetEnvironmentVariableA(name, buffer, sizeof(buffer)))
    {
        fprintf(stderr, "Unexpected capability variable %s.\n", name);
        return 0;
    }
    return 1;
}

int main(void)
{
    char actual_description[256], expected_description[256], mode[32];
    unsigned int vendor_id = 0, device_id = 0, subsystem_id = 0, revision_id = 0;
    IDXGIAdapter1 *adapter = NULL;
    IDXGIFactory1 *factory = NULL;
    DXGI_ADAPTER_DESC1 desc;
    DWORD length;
    HRESULT hr;
    BOOL explicit_identity;

    length = GetEnvironmentVariableA("SWITCHYARD_GPU_IDENTITY_MODE", mode,
            sizeof(mode));
    if (!length || length >= sizeof(mode))
    {
        fprintf(stderr, "Missing GPU identity mode.\n");
        return 1;
    }
    explicit_identity = !strcmp(mode, "explicit");
    if (!explicit_identity && strcmp(mode, "provider-default"))
    {
        fprintf(stderr, "Unknown GPU identity mode %s.\n", mode);
        return 1;
    }

    if (!require_value("SWITCHYARD_GPU_AMD_ADL", "0")
            || !require_value("SWITCHYARD_GPU_AMD_AGS_EXTENSIONS", "0")
            || !require_value("SWITCHYARD_GPU_AMD_UMD", "0"))
        return 1;

    if (explicit_identity)
    {
        if (!read_number("SWITCHYARD_GPU_REPORTED_VENDOR_ID", &vendor_id)
                || !read_number("SWITCHYARD_GPU_REPORTED_DEVICE_ID", &device_id)
                || !read_number("SWITCHYARD_GPU_REPORTED_SUBSYSTEM_ID",
                        &subsystem_id)
                || !read_number("SWITCHYARD_GPU_REPORTED_REVISION_ID",
                        &revision_id))
            return 1;

        length = GetEnvironmentVariableA("SWITCHYARD_GPU_REPORTED_DESCRIPTION",
                expected_description, sizeof(expected_description));
        if (!length || length >= sizeof(expected_description))
        {
            fprintf(stderr, "Missing reported GPU description.\n");
            return 1;
        }
    }
    else if (!require_absent("D3DM_VENDOR_ID")
            || !require_absent("D3DM_DEVICE_ID")
            || !require_absent("D3DM_DEVICE_SUBSYS")
            || !require_absent("D3DM_DEVICE_REVISION")
            || !require_absent("D3DM_DEVICE_DESCRIPTION")
            || !require_absent("SWITCHYARD_GPU_REPORTED_VENDOR_ID")
            || !require_absent("SWITCHYARD_GPU_REPORTED_DEVICE_ID")
            || !require_absent("SWITCHYARD_GPU_REPORTED_SUBSYSTEM_ID")
            || !require_absent("SWITCHYARD_GPU_REPORTED_REVISION_ID")
            || !require_absent("SWITCHYARD_GPU_REPORTED_DESCRIPTION"))
    {
        return 1;
    }

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr))
    {
        fprintf(stderr, "CreateDXGIFactory1 failed: %#lx.\n", hr);
        return 1;
    }
    hr = IDXGIFactory1_EnumAdapters1(factory, 0, &adapter);
    if (FAILED(hr))
    {
        fprintf(stderr, "EnumAdapters1 failed: %#lx.\n", hr);
        IDXGIFactory1_Release(factory);
        return 1;
    }
    hr = IDXGIAdapter1_GetDesc1(adapter, &desc);
    if (FAILED(hr))
    {
        fprintf(stderr, "GetDesc1 failed: %#lx.\n", hr);
        IDXGIAdapter1_Release(adapter);
        IDXGIFactory1_Release(factory);
        return 1;
    }

    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
            actual_description, sizeof(actual_description), NULL, NULL))
    {
        fprintf(stderr, "Could not convert the DXGI adapter description.\n");
        IDXGIAdapter1_Release(adapter);
        IDXGIFactory1_Release(factory);
        return 1;
    }

    printf("DXGI_ADAPTER=%s %04x:%04x subsys=%08x revision=%08x\n",
            actual_description, desc.VendorId, desc.DeviceId, desc.SubSysId,
            desc.Revision);

    IDXGIAdapter1_Release(adapter);
    IDXGIFactory1_Release(factory);

    if (!desc.VendorId || !desc.DeviceId || !actual_description[0])
    {
        fprintf(stderr, "DXGI returned an incomplete provider identity.\n");
        return 1;
    }

    if (explicit_identity && (desc.VendorId != vendor_id
            || desc.DeviceId != device_id || desc.SubSysId != subsystem_id
            || desc.Revision != revision_id
            || strcmp(actual_description, expected_description)))
    {
        fprintf(stderr, "DXGI identity does not match the explicit override.\n");
        return 1;
    }
    return 0;
}
