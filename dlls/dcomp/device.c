/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
 * Copyright 2023 Zhiyi Zhang for CodeWeavers
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
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winternl.h"
#include "initguid.h"
#include "objidl.h"
#include "dcomp_private.h"
#include "dxgi.h"
#include "dwmapi.h"
#include "d3d11_4.h"
#include "d3dcompiler.h"
struct wined3d_resource;
struct wined3d_texture;
#include "wine/winedxgi.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static const GUID IID_IDCompositionDevice5 =
    {0x2c6bebfe, 0xa603, 0x472f, {0xaf, 0x34, 0xd2, 0x44, 0x33, 0x56, 0xe6, 0x1b}};

static CRITICAL_SECTION dcomp_cs;
static CRITICAL_SECTION_DEBUG dcomp_debug =
{
    0, 0, &dcomp_cs,
    { &dcomp_debug.ProcessLocksList, &dcomp_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dcomp_cs") }
};
static CRITICAL_SECTION dcomp_cs = { &dcomp_debug, -1, 0, 0, 0, 0 };

void dcomp_lock(void)
{
    EnterCriticalSection(&dcomp_cs);
}

void dcomp_unlock(void)
{
    LeaveCriticalSection(&dcomp_cs);
}

static HRESULT visual_ensure_interop_from_surface(struct composition_visual *visual,
        IDXGISurface *surface)
{
    ID2D1GdiInteropRenderTarget *interop;
    ID2D1DeviceContext *device_context;
    IDXGIDevice *dxgi_device;
    ID2D1Device *d2d_device;
    HRESULT hr;

    if (visual->device_context && visual->interop)
        return S_OK;

    hr = IDXGISurface_GetDevice(surface, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (FAILED(hr))
    {
        ERR("Failed to get the dxgi surface device, hr %#lx.\n", hr);
        return hr;
    }

    hr = D2D1CreateDevice(dxgi_device, NULL, &d2d_device);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr))
    {
        ERR("Failed to create a D2D device, hr %#lx.\n", hr);
        return hr;
    }

    hr = ID2D1Device_CreateDeviceContext(d2d_device, D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &device_context);
    ID2D1Device_Release(d2d_device);
    if (FAILED(hr))
    {
        ERR("Failed to create a D2D device context, hr %#lx.\n", hr);
        return hr;
    }

    hr = ID2D1DeviceContext_QueryInterface(device_context,
            &IID_ID2D1GdiInteropRenderTarget, (void **)&interop);
    if (FAILED(hr))
    {
        ERR("Failed to get a ID2D1GdiInteropRenderTarget, hr %#lx.\n", hr);
        ID2D1DeviceContext_Release(device_context);
        return hr;
    }

    if (visual->interop)
    {
        ID2D1GdiInteropRenderTarget_Release(visual->interop);
        visual->interop = NULL;
    }
    if (visual->device_context)
    {
        ID2D1DeviceContext_Release(visual->device_context);
        visual->device_context = NULL;
    }

    visual->device_context = device_context;
    visual->interop = interop;
    return S_OK;
}

static void dynamic_texture_destroy_cache(struct composition_dynamic_texture *texture)
{
    if (texture->cache_dc)
    {
        SelectObject(texture->cache_dc, texture->old_cache_bitmap);
        DeleteDC(texture->cache_dc);
        texture->cache_dc = NULL;
        texture->old_cache_bitmap = NULL;
    }
    if (texture->cache_bitmap)
    {
        DeleteObject(texture->cache_bitmap);
        texture->cache_bitmap = NULL;
    }
    texture->cache_width = 0;
    texture->cache_height = 0;
    texture->cache_valid = FALSE;
}

static HRESULT STDMETHODCALLTYPE dynamic_texture_QueryInterface(IDCompositionDynamicTexture *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out)
        return E_INVALIDARG;

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDCompositionDynamicTexture))
    {
        IDCompositionDynamicTexture_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dynamic_texture_AddRef(IDCompositionDynamicTexture *iface)
{
    struct composition_dynamic_texture *texture = impl_from_IDCompositionDynamicTexture(iface);
    ULONG ref = InterlockedIncrement(&texture->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE dynamic_texture_Release(IDCompositionDynamicTexture *iface)
{
    struct composition_dynamic_texture *texture = impl_from_IDCompositionDynamicTexture(iface);
    ULONG ref = InterlockedDecrement(&texture->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        dynamic_texture_destroy_cache(texture);
        if (texture->texture)
            IDCompositionTexture_Release(texture->texture);
        free(texture);
    }

    return ref;
}

static HRESULT STDMETHODCALLTYPE dynamic_texture_SetTexture(IDCompositionDynamicTexture *iface,
        IDCompositionTexture *dcomp_texture, const RECT *rects, SIZE_T rect_count)
{
    struct composition_dynamic_texture *texture = impl_from_IDCompositionDynamicTexture(iface);
    SIZE_T i;

    TRACE("iface %p, texture %p, rects %p, rect_count %Iu.\n", iface, dcomp_texture, rects, rect_count);

    dcomp_lock();
    if (texture->texture != dcomp_texture)
        texture->cache_valid = FALSE;

    if (texture->texture)
        IDCompositionTexture_Release(texture->texture);
    texture->texture = dcomp_texture;
    if (texture->texture)
        IDCompositionTexture_AddRef(texture->texture);

    texture->has_damage_rect = rects && rect_count;
    if (texture->has_damage_rect)
    {
        texture->damage_rect = rects[0];
        for (i = 1; i < rect_count; ++i)
            UnionRect(&texture->damage_rect, &texture->damage_rect, &rects[i]);
        TRACE("Damage rect %s.\n", wine_dbgstr_rect(&texture->damage_rect));
    }
    dcomp_unlock();

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dynamic_texture_SetTexture2(IDCompositionDynamicTexture *iface,
        IDCompositionTexture *texture)
{
    TRACE("iface %p, texture %p.\n", iface, texture);

    return IDCompositionDynamicTexture_SetTexture(iface, texture, NULL, 0);
}

static const struct IDCompositionDynamicTextureVtbl dynamic_texture_vtbl =
{
    dynamic_texture_QueryInterface,
    dynamic_texture_AddRef,
    dynamic_texture_Release,
    dynamic_texture_SetTexture,
    dynamic_texture_SetTexture2,
};

HRESULT create_dynamic_texture(IDCompositionDynamicTexture **out)
{
    struct composition_dynamic_texture *texture;

    TRACE("out %p.\n", out);

    if (!out)
        return E_INVALIDARG;

    if (!(texture = calloc(1, sizeof(*texture))))
        return E_OUTOFMEMORY;

    texture->IDCompositionDynamicTexture_iface.lpVtbl = &dynamic_texture_vtbl;
    texture->ref = 1;
    *out = &texture->IDCompositionDynamicTexture_iface;
    return S_OK;
}

HRESULT dcomp_dynamic_texture_get_dxgi_surface(IDCompositionDynamicTexture *iface,
        IDXGISurface **surface, DXGI_ALPHA_MODE *alpha_mode)
{
    struct composition_dynamic_texture *texture = impl_from_IDCompositionDynamicTexture(iface);
    IDCompositionTexture *dcomp_texture;
    IWineDXGISwapChain *wine_swapchain;
    IDXGISwapChain1 *swapchain;
    HRESULT hr;

    TRACE("iface %p, surface %p, alpha_mode %p.\n", iface, surface, alpha_mode);

    if (!surface)
        return E_INVALIDARG;

    *surface = NULL;
    if (alpha_mode)
        *alpha_mode = DXGI_ALPHA_MODE_IGNORE;

    dcomp_lock();
    if (!texture->texture)
    {
        dcomp_unlock();
        return DXGI_ERROR_INVALID_CALL;
    }
    dcomp_texture = texture->texture;
    IDCompositionTexture_AddRef(dcomp_texture);
    dcomp_unlock();

    if (alpha_mode && SUCCEEDED(IDCompositionTexture_QueryInterface(dcomp_texture,
            &IID_IDXGISwapChain1, (void **)&swapchain)))
    {
        DXGI_SWAP_CHAIN_DESC1 desc;

        if (SUCCEEDED(IDXGISwapChain1_GetDesc1(swapchain, &desc)))
            *alpha_mode = desc.AlphaMode;
        IDXGISwapChain1_Release(swapchain);
    }

    if (SUCCEEDED(hr = IDCompositionTexture_QueryInterface(dcomp_texture,
            &IID_IWineDXGISwapChain, (void **)&wine_swapchain)))
    {
        hr = IWineDXGISwapChain_get_front_buffer(wine_swapchain, &IID_IDXGISurface, (void **)surface);
        IWineDXGISwapChain_Release(wine_swapchain);
    }
    else
    {
        hr = IDCompositionTexture_QueryInterface(dcomp_texture, &IID_IDXGISurface, (void **)surface);
    }

    IDCompositionTexture_Release(dcomp_texture);
    return hr;
}

static HRESULT dynamic_texture_update_cache(struct composition_dynamic_texture *texture,
        HDC src_dc, UINT width, UINT height, HDC *cache_dc)
{
    RECT bounds = {0, 0, width, height}, update_rect = bounds;
    BITMAPINFO bitmap_info = {{0}};

    if (!texture->cache_dc || texture->cache_width != width || texture->cache_height != height)
    {
        dynamic_texture_destroy_cache(texture);

        bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
        bitmap_info.bmiHeader.biWidth = width;
        bitmap_info.bmiHeader.biHeight = -(LONG)height;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        if (!(texture->cache_dc = CreateCompatibleDC(src_dc)))
            return HRESULT_FROM_WIN32(GetLastError());

        if (!(texture->cache_bitmap = CreateDIBSection(src_dc, &bitmap_info, DIB_RGB_COLORS,
                NULL, NULL, 0)))
        {
            DWORD error = GetLastError();
            dynamic_texture_destroy_cache(texture);
            return HRESULT_FROM_WIN32(error);
        }

        texture->old_cache_bitmap = SelectObject(texture->cache_dc, texture->cache_bitmap);
        texture->cache_width = width;
        texture->cache_height = height;
    }

    if (texture->cache_valid && texture->has_damage_rect)
    {
        if (!IntersectRect(&update_rect, &texture->damage_rect, &bounds))
        {
            *cache_dc = texture->cache_dc;
            return S_OK;
        }
    }

    TRACE("Updating dynamic texture cache %p from rect %s.\n", texture->cache_bitmap,
            wine_dbgstr_rect(&update_rect));

    if (!BitBlt(texture->cache_dc, update_rect.left, update_rect.top,
            update_rect.right - update_rect.left, update_rect.bottom - update_rect.top,
            src_dc, update_rect.left, update_rect.top, SRCCOPY))
    {
        DWORD error = GetLastError();
        WARN("Failed to update dynamic texture cache, error %lu.\n", error);
        return HRESULT_FROM_WIN32(error);
    }

    texture->cache_valid = TRUE;
    *cache_dc = texture->cache_dc;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_QueryInterface(IDCompositionDevice *iface,
        REFIID iid, void **out)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, iid %s, out %p\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionDevice))
    {
        IUnknown_AddRef(&device->IDCompositionDevice_iface);
        *out = &device->IDCompositionDevice_iface;
        return S_OK;
    }
    else if ((device->version >= 2
              && (IsEqualGUID(iid, &IID_IDCompositionDevice2)
                  || IsEqualGUID(iid, &IID_IDCompositionDevice3)
                  || IsEqualGUID(iid, &IID_IDCompositionDevice5)
                  || IsEqualGUID(iid, &IID_IDCompositionDesktopDevice)))
              || IsEqualGUID(iid, &IID_IDCompositionDesktopDevicePartner)
              || IsEqualGUID(iid, &IID_IDCompositionDeviceUnknown))
    {
        IUnknown_AddRef(&device->IDCompositionDeviceUnknown_iface);
        *out = &device->IDCompositionDeviceUnknown_iface;

        if (IsEqualGUID(iid, &IID_IDCompositionDesktopDevicePartner)
            || IsEqualGUID(iid, &IID_IDCompositionDevice5)
            || IsEqualGUID(iid, &IID_IDCompositionDeviceUnknown))
            FIXME("Returning undocumented interface %s %p.\n", wine_dbgstr_guid(iid), *out);
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE device_AddRef(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedIncrement(&device->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE device_Release(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedDecrement(&device->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (device->thread)
        {
            WaitForSingleObject(device->thread, INFINITE);
            CloseHandle(device->thread);
        }
        free(device);
    }

    return ref;
}

/* TODO: Optimize this. There is plenty of room */
static HRESULT create_bgra_surface_from_rgba(IDXGISurface *rgba_surface, IDXGISurface **bgra_surface)
{
    static const D3D11_INPUT_ELEMENT_DESC layout_desc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    static const char vs_code[] =
        "void main(in float4 in_position : POSITION,\n"
        "          in float2 in_texcoord : TEXCOORD,\n"
        "          out float4 position : SV_Position,\n"
        "          out float2 texcoord : TEXCOORD)\n"
        "{\n"
        "     position = in_position;\n"
        "     texcoord = in_texcoord;\n"
        "}";

    static const char ps_code[] =
        "Texture2D src_texture : register(t0);\n"
        "SamplerState sam_linear : register(s0);\n"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET"
        "{\n"
        "    return src_texture.Sample(sam_linear, uv);\n"
        "}";

    struct vec2
    {
        float x, y;
    };

    static const struct
    {
        struct vec2 position;
        struct vec2 texcoord;
    } quad[] =
    {
        {{-1.0f, 1.0f}, {0.0f, 0.0f}},   /* Top-Left */
        {{1.0f, 1.0f}, {1.0f, 0.0f}},    /* Top-Right */
        {{-1.0f, -1.0f}, {0.0f, 1.0f}},  /* Bottom-Left */
        {{1.0f, -1.0f}, {1.0f, 1.0f}},   /* Bottom-Right */
    };

    ID3D11Texture2D *src_texture = NULL, *dst_texture = NULL;
    ID3DDeviceContextState *state = NULL, *old_state = NULL;
    ID3D11DeviceContext1 *d3d11_device_context1 = NULL;
    ID3D11DeviceContext *d3d11_device_context = NULL;
    static ID3D10Blob *vs_blob = NULL, *ps_blob = NULL;
    ID3D11InputLayout *input_layout = NULL;
    D3D11_SAMPLER_DESC sampler_desc = {0};
    D3D11_SUBRESOURCE_DATA resource_data;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Device1 *d3d11_device = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11SamplerState *sampler = NULL;
    D3D11_TEXTURE2D_DESC texture_desc;
    DXGI_SURFACE_DESC surface_desc;
    D3D11_BUFFER_DESC buffer_desc;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    unsigned int stride, offset;
    ID3D11Buffer *vb = NULL;
    D3D11_VIEWPORT vp;
    HRESULT hr;

    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    hr = IDXGISurface_QueryInterface(rgba_surface, &IID_ID3D11Texture2D, (void **)&src_texture);
    if (FAILED(hr))
    {
        ERR("Failed to get a ID3D11Texture2D, hr %#lx.\n", hr);
        goto done;
    }

    hr = IDXGISurface_GetDevice(rgba_surface, &IID_ID3D11Device1, (void **)&d3d11_device);
    if (FAILED(hr))
    {
        ERR("Failed to get device, hr %#lx.\n", hr);
        goto done;
    }

    hr = IDXGISurface_GetDesc(rgba_surface, &surface_desc);
    if (FAILED(hr))
    {
        ERR("Failed to get the dxgi surface description, hr %#lx.\n", hr);
        goto done;
    }

    texture_desc.Width = surface_desc.Width;
    texture_desc.Height = surface_desc.Height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = D3D11_RESOURCE_MISC_GUARDED | D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    hr = ID3D11Device1_CreateTexture2D(d3d11_device, &texture_desc, NULL, &dst_texture);
    if (FAILED(hr))
    {
        ERR("Failed to create a ID3D11Texture2D, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID3D11Device1_CreateShaderResourceView(d3d11_device, (ID3D11Resource *)src_texture, NULL, &srv);
    if (FAILED(hr))
    {
        ERR("Failed to create a SRV, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID3D11Device1_CreateRenderTargetView(d3d11_device, (ID3D11Resource *)dst_texture, NULL, &rtv);
    if (FAILED(hr))
    {
        ERR("Failed to create a RTV, hr %#lx.\n", hr);
        goto done;
    }

    ID3D11Device1_GetImmediateContext(d3d11_device, &d3d11_device_context);
    if (FAILED(hr = ID3D11DeviceContext_QueryInterface(d3d11_device_context, &IID_ID3D11DeviceContext1,
                                                       (void **)&d3d11_device_context1)))
    {
        ERR("Failed to query ID3D11DeviceContext1, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = ID3D11Device1_CreateDeviceContextState(d3d11_device, 0, feature_levels,
                                                           ARRAY_SIZE(feature_levels), D3D11_SDK_VERSION,
                                                           &IID_ID3D11Device1, NULL, &state)))
    {
        ERR("Failed to create device context state, hr %#lx.\n", hr);
        goto done;
    }
    ID3D11DeviceContext1_SwapDeviceContextState(d3d11_device_context1, state, &old_state);

    ID3D11DeviceContext1_OMSetRenderTargets(d3d11_device_context1, 1, &rtv, NULL);
    ID3D11DeviceContext1_PSSetShaderResources(d3d11_device_context1, 0, 1, &srv);

    /* Move this to dcomp device instance ? */
    if (vs_blob == NULL)
    {
        hr = D3DCompile(vs_code, sizeof(vs_code) - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vs_blob, NULL);
        if (FAILED(hr))
        {
            ERR("Failed to compile vertex shader, hr %#lx.\n", hr);
            goto done;
        }

        hr = D3DCompile(ps_code, sizeof(ps_code) - 1, "ps", NULL, NULL, "main", "ps_4_0", 0, 0, &ps_blob, NULL);
        if (FAILED(hr))
        {
            ERR("Failed to compile pixel shader, hr %#lx.\n", hr);
            goto done;
        }
    }

    hr = ID3D11Device1_CreateVertexShader(d3d11_device, ID3D10Blob_GetBufferPointer(vs_blob),
                                          ID3D10Blob_GetBufferSize(vs_blob), NULL, &vs);
    if (FAILED(hr))
    {
        ERR("Failed to create vertex shader, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID3D11Device1_CreatePixelShader(d3d11_device, ID3D10Blob_GetBufferPointer(ps_blob),
                                         ID3D10Blob_GetBufferSize(ps_blob), NULL, &ps);
    if (FAILED(hr))
    {
        ERR("Failed to create pixel shader, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID3D11Device1_CreateInputLayout(d3d11_device, layout_desc, ARRAY_SIZE(layout_desc),
                                         ID3D10Blob_GetBufferPointer(vs_blob),
                                         ID3D10Blob_GetBufferSize(vs_blob), &input_layout);
    if (FAILED(hr))
    {
        ERR("Failed to create input layout, hr %#lx.\n", hr);
        goto done;
    }

    buffer_desc.ByteWidth = sizeof(quad);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_desc.CPUAccessFlags = 0;
    buffer_desc.MiscFlags = 0;
    buffer_desc.StructureByteStride = 0;
    resource_data.pSysMem = quad;
    resource_data.SysMemPitch = 0;
    resource_data.SysMemSlicePitch = 0;
    hr = ID3D11Device1_CreateBuffer(d3d11_device, &buffer_desc, &resource_data, &vb);
    if (FAILED(hr))
    {
        ERR("Failed to create vertex buffer, hr %#lx.\n", hr);
        goto done;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = ID3D11Device1_CreateSamplerState(d3d11_device, &sampler_desc, &sampler);
    if (FAILED(hr))
    {
        ERR("Failed to create sampler state, hr %#lx.\n", hr);
        goto done;
    }
    ID3D11DeviceContext1_PSSetSamplers(d3d11_device_context1, 0, 1, &sampler);

    ID3D11DeviceContext1_IASetInputLayout(d3d11_device_context1, input_layout);
    ID3D11DeviceContext1_IASetPrimitiveTopology(d3d11_device_context1, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    stride = sizeof(quad[0]);
    offset = 0;
    ID3D11DeviceContext1_IASetVertexBuffers(d3d11_device_context1, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext1_VSSetShader(d3d11_device_context1, vs, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(d3d11_device_context1, ps, NULL, 0);

    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = surface_desc.Width;
    vp.Height = surface_desc.Height;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext1_RSSetViewports(d3d11_device_context1, 1, &vp);
    ID3D11DeviceContext1_Draw(d3d11_device_context1, 4, 0);
    ID3D11DeviceContext1_Flush(d3d11_device_context1);

    hr = ID3D11Texture2D_QueryInterface(dst_texture, &IID_IDXGISurface, (void **)bgra_surface);
    if (FAILED(hr))
    {
        ERR("Failed to get bgra surface, hr %#lx.\n", hr);
        goto done;
    }

done:
    if (old_state)
    {
        ID3D11DeviceContext1_SwapDeviceContextState(d3d11_device_context1, old_state, NULL);
        ID3DDeviceContextState_Release(old_state);
    }
    if (state)
        ID3DDeviceContextState_Release(state);
    if (vb)
        ID3D11Buffer_Release(vb);
    if (sampler)
        ID3D11SamplerState_Release(sampler);
    if (input_layout)
        ID3D11InputLayout_Release(input_layout);
    if (ps)
        ID3D11PixelShader_Release(ps);
    if (vs)
        ID3D11VertexShader_Release(vs);
    if (d3d11_device_context1)
        ID3D11DeviceContext1_Release(d3d11_device_context1);
    if (d3d11_device_context)
        ID3D11DeviceContext_Release(d3d11_device_context);
    if (rtv)
        ID3D11RenderTargetView_Release(rtv);
    if (srv)
        ID3D11ShaderResourceView_Release(srv);
    if (dst_texture)
        ID3D11Texture2D_Release(dst_texture);
    if (d3d11_device)
        ID3D11Device1_Release(d3d11_device);
    if (src_texture)
        ID3D11Texture2D_Release(src_texture);
    return hr;
}

static void dcomp_blit_dc_to_target(const struct composition_target *target,
                                    const struct composition_visual *visual,
                                    HDC src_dc, UINT src_width, UINT src_height,
                                    BLENDFUNCTION blend_func, BOOL force_opaque,
                                    float offset_x, float offset_y)
{
    UINT src_x = 0, src_y = 0, width = src_width, height = src_height;
    DWORD style = DCX_USESTYLE | DCX_CACHE;
    HDC dst_dc;
    int dst_x = offset_x, dst_y = offset_y;

    /* DComp content composes over child HWND hosts; child clipping would exclude Chromium CEF content. */
    if (visual->has_clip)
    {
        int left = max((int)visual->clip_rect.left, 0);
        int top = max((int)visual->clip_rect.top, 0);
        int right = min((int)visual->clip_rect.right, (int)src_width);
        int bottom = min((int)visual->clip_rect.bottom, (int)src_height);

        if (right <= left || bottom <= top)
            return;

        src_x = left;
        src_y = top;
        dst_x += left;
        dst_y += top;
        width = right - left;
        height = bottom - top;
    }

    dst_dc = GetDCEx(target->hwnd, 0, style);
    if (!dst_dc)
    {
        WARN("Failed to get destination DC, error %lu.\n", GetLastError());
        return;
    }

    if (!blend_func.AlphaFormat && blend_func.SourceConstantAlpha == 255)
    {
        if (!BitBlt(dst_dc, dst_x, dst_y, width, height, src_dc, src_x, src_y, SRCCOPY))
            WARN("BitBlt failed, error %lu.\n", GetLastError());
    }
    else if (!GdiAlphaBlend(dst_dc, dst_x, dst_y, width, height, src_dc, src_x, src_y,
            width, height, blend_func))
    {
        WARN("GdiAlphaBlend failed, error %lu.\n", GetLastError());
        if (force_opaque && !BitBlt(dst_dc, dst_x, dst_y, width, height, src_dc, src_x, src_y, SRCCOPY))
            WARN("Opaque BitBlt fallback failed, error %lu.\n", GetLastError());
    }

    ReleaseDC(target->hwnd, dst_dc);
}

static void do_composite_dxgi_surface(const struct composition_target *target,
                                      const struct composition_visual *visual,
                                      IDXGISurface *dxgi_surface,
                                      DXGI_ALPHA_MODE alpha_mode,
                                      BOOL force_opaque,
                                      IDCompositionDynamicTexture *dynamic_texture,
                                      float offset_x, float offset_y)
{
    ID3DDeviceContextState *state = NULL, *old_state = NULL;
    ID3D11DeviceContext1 *d3d11_device_context1 = NULL;
    ID3D11DeviceContext *d3d11_device_context = NULL;
    ID3D11Multithread *multithread = NULL;
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    ID2D1Bitmap1 *target_bitmap = NULL;
    ID3D11Device1 *d3d11_device = NULL;
    IDXGISurface1 *dxgi_surface1 = NULL;
    IDXGISurface *bgra_surface = NULL;
    DXGI_SURFACE_DESC surface_desc;
    ID2D1Bitmap *src_bitmap = NULL;
    ID2D1Image *old_target = NULL;
    HDC cache_dc = NULL;
    HDC src_dc;
    D2D1_SIZE_U size;
    BLENDFUNCTION blend_func = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HRESULT hr;

    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    if (!target->root)
    {
        ERR("Target has no root.\n");
        goto done;
    }

    hr = IDXGISurface_GetDevice(dxgi_surface, &IID_ID3D11Device1, (void **)&d3d11_device);
    if (FAILED(hr))
    {
        ERR("Failed to get device, hr %#lx.\n", hr);
        goto done;
    }

    ID3D11Device1_GetImmediateContext(d3d11_device, &d3d11_device_context);
    if (FAILED(hr = ID3D11DeviceContext_QueryInterface(d3d11_device_context, &IID_ID3D11DeviceContext1,
                                                       (void **)&d3d11_device_context1)))
    {
        ERR("Failed to query ID3D11DeviceContext1, hr %#lx.\n", hr);
        goto done;
    }

    /* Use ID3D11Multithread to avoid conflicts with rendering in an another thread with the same
     * immediate ID3D11DeviceContext */
    hr = ID3D11DeviceContext_QueryInterface(d3d11_device_context, &IID_ID3D11Multithread, (void **)&multithread);
    if (FAILED(hr))
    {
        ERR("Failed to query ID3D11Multithread, hr %#lx.\n", hr);
        goto done;
    }

    ID3D11Multithread_SetMultithreadProtected(multithread, TRUE);
    ID3D11Multithread_Enter(multithread);

    /* Is this really necessary? anyway, just to be safe */
    if (FAILED(hr = ID3D11Device1_CreateDeviceContextState(d3d11_device, 0, feature_levels,
                                                           ARRAY_SIZE(feature_levels), D3D11_SDK_VERSION,
                                                           &IID_ID3D11Device1, NULL, &state)))
    {
        ERR("Failed to create device context state, hr %#lx.\n", hr);
        goto done;
    }
    ID3D11DeviceContext1_SwapDeviceContextState(d3d11_device_context1, state, &old_state);

    if (FAILED(hr = IDXGISurface_GetDesc(dxgi_surface, &surface_desc)))
    {
        ERR("Failed to get the dxgi surface description, hr %#lx.\n", hr);
        goto done;
    }

    /* Convert DXGI_FORMAT_R8G8B8A8_UNORM to DXGI_FORMAT_B8G8R8A8_UNORM */
    if (surface_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = create_bgra_surface_from_rgba(dxgi_surface, &bgra_surface);
        if (FAILED(hr))
        {
            ERR("Failed to convert DXGI_FORMAT_R8G8B8A8_UNORM surface to DXGI_FORMAT_B8G8R8A8_UNORM, hr %#lx.\n", hr);
            goto done;
        }
        dxgi_surface = bgra_surface;
    }
    else if (surface_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        ERR("DXGI_FORMAT_R16G16B16A16_FLOAT surface is currently unsupported.\n");
        goto done;
    }

    if (force_opaque)
    {
        TRACE("Treating swapchain content as opaque for window target composition.\n");
        blend_func.AlphaFormat = 0;
    }
    else if (alpha_mode == DXGI_ALPHA_MODE_UNSPECIFIED || alpha_mode == DXGI_ALPHA_MODE_IGNORE)
        blend_func.AlphaFormat = 0;
    else if (alpha_mode != DXGI_ALPHA_MODE_PREMULTIPLIED)
        FIXME("Unsupported alpha mode %#x, treating as premultiplied.\n", alpha_mode);

    size.width = surface_desc.Width;
    size.height = surface_desc.Height;

    if (dynamic_texture && SUCCEEDED(hr = IDXGISurface_QueryInterface(dxgi_surface,
            &IID_IDXGISurface1, (void **)&dxgi_surface1)))
    {
        struct composition_dynamic_texture *texture = impl_from_IDCompositionDynamicTexture(dynamic_texture);

        if (SUCCEEDED(hr = IDXGISurface1_GetDC(dxgi_surface1, FALSE, &src_dc)))
        {
            hr = dynamic_texture_update_cache(texture, src_dc, size.width, size.height, &cache_dc);
            IDXGISurface1_ReleaseDC(dxgi_surface1, NULL);
            if (FAILED(hr))
            {
                WARN("Failed to update dynamic texture cache from surface DC, hr %#lx.\n", hr);
                goto done;
            }

            dcomp_blit_dc_to_target(target, visual, cache_dc, size.width, size.height,
                    blend_func, force_opaque, offset_x, offset_y);
            goto done;
        }

        TRACE("Failed to get direct dynamic texture DC, hr %#lx; falling back to D2D copy.\n", hr);
    }

    bitmap_desc.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bitmap_desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bitmap_desc.dpiX = 0;
    bitmap_desc.dpiY = 0;
    bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE;
    bitmap_desc.colorContext = NULL;

    assert(visual->device_context);
    if (FAILED(hr = ID2D1DeviceContext_CreateBitmap(visual->device_context, size, NULL, 0,
            &bitmap_desc, &target_bitmap)))
    {
        ERR("Failed to create a bitmap, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = ID2D1DeviceContext_CreateSharedBitmap(visual->device_context, &IID_IDXGISurface,
            dxgi_surface, NULL, &src_bitmap)))
    {
        ERR("Failed to create a bitmap from dxgi surface, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = ID2D1Bitmap1_CopyFromBitmap(target_bitmap, NULL, src_bitmap, NULL)))
    {
        ERR("Failed to copy bitmap, hr %#lx.\n", hr);
        goto done;
    }

    ID2D1DeviceContext_GetTarget(visual->device_context, &old_target);
    ID2D1DeviceContext_SetTarget(visual->device_context, (ID2D1Image *)target_bitmap);
    if (FAILED(hr = ID2D1GdiInteropRenderTarget_GetDC(visual->interop, D2D1_DC_INITIALIZE_MODE_COPY,
            &src_dc)))
    {
        ERR("GetDC failed, hr %#lx.\n", hr);
        ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
        if (old_target)
            ID2D1Image_Release(old_target);
        goto done;
    }

    if (dynamic_texture)
    {
        struct composition_dynamic_texture *texture = impl_from_IDCompositionDynamicTexture(dynamic_texture);

        if (FAILED(hr = dynamic_texture_update_cache(texture, src_dc, size.width, size.height, &cache_dc)))
        {
            WARN("Failed to update dynamic texture cache, hr %#lx.\n", hr);
            goto release_dc_target;
        }
        src_dc = cache_dc;
    }

    dcomp_blit_dc_to_target(target, visual, src_dc, size.width, size.height,
            blend_func, force_opaque, offset_x, offset_y);

release_dc_target:
    ID2D1GdiInteropRenderTarget_ReleaseDC(visual->interop, NULL);
    ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
    if (old_target)
        ID2D1Image_Release(old_target);

done:
    if (dxgi_surface1)
        IDXGISurface1_Release(dxgi_surface1);
    if (bgra_surface)
        IDXGISurface_Release(bgra_surface);
    if (src_bitmap)
        ID2D1Bitmap_Release(src_bitmap);
    if (target_bitmap)
        ID2D1Bitmap1_Release(target_bitmap);
    if (d3d11_device_context1 && old_state)
    {
        ID3D11DeviceContext1_SwapDeviceContextState(d3d11_device_context1, old_state, NULL);
        ID3DDeviceContextState_Release(old_state);
    }
    if (state)
        ID3DDeviceContextState_Release(state);
    if (multithread)
    {
        ID3D11Multithread_Leave(multithread);
        ID3D11Multithread_Release(multithread);
    }
    if (d3d11_device_context1)
        ID3D11DeviceContext1_Release(d3d11_device_context1);
    if (d3d11_device_context)
        ID3D11DeviceContext_Release(d3d11_device_context);
    if (d3d11_device)
        ID3D11Device1_Release(d3d11_device);
}

static struct composition_visual * shared_visual_target_get_root(HANDLE shared_visual_handle)
{
    struct composition_visual *visual = NULL;
    NTSTATUS status;

    SERVER_START_REQ(dcomp_get_shared_visual_info)
    {
        req->handle = wine_server_obj_handle(shared_visual_handle);
        if (!(status = wine_server_call(req)))
            visual = impl_from_IDCompositionVisual(wine_server_get_ptr(reply->target_root));
    }
    SERVER_END_REQ;

    if (status)
        ERR("dcomp_get_shared_visual_info failed, not in the same process? status %#lx.\n", status);

    return visual;
}

static void clear_composition_target(const struct composition_target *target)
{
    RECT rect;
    HDC dc;
    DWORD style = DCX_USESTYLE | DCX_CACHE;

    if (!GetClientRect(target->hwnd, &rect))
        return;

    dc = GetDCEx(target->hwnd, 0, style);
    if (!dc)
        return;

    FillRect(dc, &rect, GetStockObject(BLACK_BRUSH));
    ReleaseDC(target->hwnd, dc);
}

static HRESULT do_composite(const struct composition_target *target, struct composition_visual *visual,
        float offset_x, float offset_y)
{
    struct composition_visual *child_visual;
    DXGI_ALPHA_MODE alpha_mode = DXGI_ALPHA_MODE_IGNORE;
    IDCompositionDynamicTexture *dynamic_texture = NULL;
    IDXGISurface *dxgi_surface;
    HRESULT hr;

    offset_x += visual->offset_x;
    offset_y += visual->offset_y;

    if (visual->shared_visual_handle)
    {
        struct composition_visual *root = shared_visual_target_get_root(visual->shared_visual_handle);
        if (root)
            do_composite(target, root, offset_x, offset_y);
    }

    /* Render content */
    if (visual->content)
    {
        if (IsEqualGUID(&visual->content_iid, &IID_IDXGISwapChain1))
        {
            DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
            IWineDXGISwapChain *wine_swapchain;
            IDXGISwapChain1 *swapchain;

            hr = IUnknown_QueryInterface(visual->content, &IID_IDXGISwapChain1, (void **)&swapchain);
            if (FAILED(hr))
            {
                FIXME("Failed to query IDXGISwapChain1.\n");
                return hr;
            }

            hr = IDXGISwapChain1_GetDesc1(swapchain, &swapchain_desc);
            if (FAILED(hr))
            {
                ERR("Failed to get the swapchain description, hr %#lx.\n", hr);
                IDXGISwapChain1_Release(swapchain);
                return hr;
            }
            alpha_mode = swapchain_desc.AlphaMode;

            if (SUCCEEDED(IUnknown_QueryInterface(visual->content, &IID_IWineDXGISwapChain,
                    (void **)&wine_swapchain)))
            {
                hr = IWineDXGISwapChain_get_front_buffer(wine_swapchain, &IID_IDXGISurface,
                        (void **)&dxgi_surface);
                IWineDXGISwapChain_Release(wine_swapchain);
            }
            else
            {
                /* Fallback for non-Wine swapchains. */
                hr = IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_IDXGISurface,
                                               (void **)&dxgi_surface);
            }
            IDXGISwapChain1_Release(swapchain);
            if (FAILED(hr))
            {
                ERR("Failed to get the swapchain front buffer, hr %#lx.\n", hr);
                return hr;
            }
        }
        else if (IsEqualGUID(&visual->content_iid, &IID_IDCompositionSurface))
        {
            struct composition_surface *surface_impl;
            IDCompositionSurface *surface;

            hr = IUnknown_QueryInterface(visual->content, &IID_IDCompositionSurface, (void **)&surface);
            if (FAILED(hr))
            {
                FIXME("Failed to query IDCompositionSurface.\n");
                return hr;
            }

            surface_impl = impl_from_IDCompositionSurface(surface);
            if (IsEqualGUID(&surface_impl->physical_surface_iid, &IID_IDXGISurface))
            {
                alpha_mode = surface_impl->alpha_mode;
                dxgi_surface = (IDXGISurface *)surface_impl->physical_surface;
                IDXGISurface_AddRef(dxgi_surface);
                IDCompositionSurface_Release(surface);
            }
            else
            {
                FIXME("IDCompositionSurface physical_surface_iid %s is unsupported.\n",
                      wine_dbgstr_guid(&surface_impl->physical_surface_iid));
                IDCompositionSurface_Release(surface);
                return E_FAIL;
            }
        }
        else if (IsEqualGUID(&visual->content_iid, &IID_IDCompositionDynamicTexture))
        {
            hr = IUnknown_QueryInterface(visual->content, &IID_IDCompositionDynamicTexture,
                    (void **)&dynamic_texture);
            if (FAILED(hr))
            {
                FIXME("Failed to query IDCompositionDynamicTexture.\n");
                return hr;
            }

            hr = dcomp_dynamic_texture_get_dxgi_surface(dynamic_texture, &dxgi_surface, &alpha_mode);
            if (FAILED(hr))
            {
                IDCompositionDynamicTexture_Release(dynamic_texture);
                TRACE("Dynamic texture has no current DXGI surface, hr %#lx.\n", hr);
                return S_OK;
            }
        }
        else
        {
            FIXME("content_iid %s is unsupported.\n", wine_dbgstr_guid(&visual->content_iid));
            return E_FAIL;
        }

        if (dynamic_texture && FAILED(hr = visual_ensure_interop_from_surface(visual, dxgi_surface)))
        {
            IDCompositionDynamicTexture_Release(dynamic_texture);
            IDXGISurface_Release(dxgi_surface);
            return hr;
        }

        do_composite_dxgi_surface(target, visual, dxgi_surface, alpha_mode,
                IsEqualGUID(&visual->content_iid, &IID_IDXGISwapChain1),
                dynamic_texture, offset_x, offset_y);
        IDXGISurface_Release(dxgi_surface);
        if (dynamic_texture)
        {
            IDCompositionDynamicTexture_Release(dynamic_texture);
            dynamic_texture = NULL;
        }
    }

    LIST_FOR_EACH_ENTRY(child_visual, &visual->child_visuals, struct composition_visual, entry)
    {
        do_composite(target, child_visual, offset_x, offset_y);
    }

    return S_OK;
}


static DWORD WINAPI composite_thread_proc(void *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    unsigned int count, frequency, refresh_period;
    struct composition_target *target;
    struct composition_visual *visual;
    HDC hdc;

    /* TODO: Implement and use D3DKMTWaitForVerticalBlankEvent() */
    hdc = GetDC(0);
    frequency = GetDeviceCaps(hdc, VREFRESH);
    refresh_period = 1000 / frequency;
    ReleaseDC(0, hdc);

    while (TRUE)
    {
        dcomp_lock();

        count = 0;
        LIST_FOR_EACH_ENTRY(target, &device->targets, struct composition_target, entry)
        {
            if (!target->root)
            {
                FIXME("Target %p has no root.\n", &target->IDCompositionTarget_iface);
                continue;
            }

            /* Skip targets created from a shared visual handle. Composition for such targets will
             * be done for the visual tree where the visual created from the shared visual handle is at */
            if (target->shared_visual_handle)
                continue;

            visual = impl_from_IDCompositionVisual(target->root);
            clear_composition_target(target);
            if (SUCCEEDED(do_composite(target, visual, 0.0f, 0.0f)))
                count++;
        }

        if (!count)
        {
            TRACE("Composition thread exited.\n");
            device->thread_exited = TRUE;
            dcomp_unlock();
            break;
        }

        dcomp_unlock();

        if (device->exit_thread)
            break;

        Sleep(refresh_period);
    }

    return 0;
}

static HRESULT STDMETHODCALLTYPE device_Commit(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    HRESULT hr = S_OK;

    FIXME("iface %p semi-stub!\n", iface);

    dcomp_lock();

    if (!device->thread || device->thread_exited)
    {
        if (device->thread)
        {
            device->exit_thread = TRUE;
            WaitForSingleObject(device->thread, INFINITE);
            CloseHandle(device->thread);
        }
        device->thread_exited = FALSE;
        device->exit_thread = FALSE;
        device->thread = CreateThread(NULL, 0, composite_thread_proc, iface, 0, NULL);
    }

    dcomp_unlock();

    return hr;
}

static HRESULT STDMETHODCALLTYPE device_WaitForCommitCompletion(IDCompositionDevice *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_GetFrameStatistics(IDCompositionDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTargetForHwnd(IDCompositionDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p\n", iface, hwnd, topmost, target);

    return create_target(device, hwnd, topmost, target);
}

static HRESULT STDMETHODCALLTYPE device_CreateVisual(IDCompositionDevice *iface,
        IDCompositionVisual **visual)
{
    TRACE("iface %p, visual %p\n", iface, visual);

    return create_visual(1, &IID_IDCompositionVisual, (void **)visual);
}

static HRESULT STDMETHODCALLTYPE device_CreateSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateVirtualSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHandle(IDCompositionDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHwnd(IDCompositionDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTranslateTransform(IDCompositionDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateScaleTransform(IDCompositionDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRotateTransform(IDCompositionDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSkewTransform(IDCompositionDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateMatrixTransform(IDCompositionDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT elements,
        IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n", iface, transforms,
            elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTranslateTransform3D(IDCompositionDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, translate_transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateScaleTransform3D(IDCompositionDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRotateTransform3D(IDCompositionDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateMatrixTransform3D(IDCompositionDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", iface,
            transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateAnimation(IDCompositionDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CheckDeviceState(IDCompositionDevice *iface,
        BOOL *valid)
{
    FIXME("iface %p, valid %p stub!\n", iface, valid);
    return E_NOTIMPL;
}

static const struct IDCompositionDeviceVtbl device_vtbl =
{
    /* IUnknown methods */
    device_QueryInterface,
    device_AddRef,
    device_Release,
    /* IDCompositionDevice methods */
    device_Commit,
    device_WaitForCommitCompletion,
    device_GetFrameStatistics,
    device_CreateTargetForHwnd,
    device_CreateVisual,
    device_CreateSurface,
    device_CreateVirtualSurface,
    device_CreateSurfaceFromHandle,
    device_CreateSurfaceFromHwnd,
    device_CreateTranslateTransform,
    device_CreateScaleTransform,
    device_CreateRotateTransform,
    device_CreateSkewTransform,
    device_CreateMatrixTransform,
    device_CreateTransformGroup,
    device_CreateTranslateTransform3D,
    device_CreateScaleTransform3D,
    device_CreateRotateTransform3D,
    device_CreateMatrixTransform3D,
    device_CreateTransform3DGroup,
    device_CreateEffectGroup,
    device_CreateRectangleClip,
    device_CreateAnimation,
    device_CheckDeviceState,
};

static HRESULT STDMETHODCALLTYPE desktop_device_QueryInterface(IDCompositionDeviceUnknown *iface,
        REFIID iid, void **out)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE desktop_device_AddRef(IDCompositionDeviceUnknown *iface)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE desktop_device_Release(IDCompositionDeviceUnknown *iface)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_Commit(IDCompositionDeviceUnknown *iface)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_Commit(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_WaitForCommitCompletion(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_GetFrameStatistics(IDCompositionDeviceUnknown *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateVisual(IDCompositionDeviceUnknown *iface,
        IDCompositionVisual2 **visual)
{
    TRACE("iface %p, visual %p\n", iface, visual);

    return create_visual(2, &IID_IDCompositionVisual2, (void **)visual);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFactory(IDCompositionDeviceUnknown *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p, rendering_device %p, surface_factory %p.\n", iface, rendering_device, surface_factory);

    return create_surface_factory(device, rendering_device, surface_factory);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurface(IDCompositionDeviceUnknown *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateVirtualSurface(IDCompositionDeviceUnknown *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTranslateTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateScaleTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRotateTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSkewTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateMatrixTransform(IDCompositionDeviceUnknown *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTransformGroup(IDCompositionDeviceUnknown *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n", iface, transforms,
            elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTranslateTransform3D(IDCompositionDeviceUnknown *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, translate_transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateScaleTransform3D(IDCompositionDeviceUnknown *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRotateTransform3D(IDCompositionDeviceUnknown *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateMatrixTransform3D(IDCompositionDeviceUnknown *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTransform3DGroup(IDCompositionDeviceUnknown *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", iface,
            transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateEffectGroup(IDCompositionDeviceUnknown *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRectangleClip(IDCompositionDeviceUnknown *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateAnimation(IDCompositionDeviceUnknown *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTargetForHwnd(IDCompositionDeviceUnknown *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = impl_from_IDCompositionDeviceUnknown(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p\n", iface, hwnd, topmost, target);

    return IDCompositionDevice_CreateTargetForHwnd(&device->IDCompositionDevice_iface, hwnd,
            topmost, target);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFromHandle(IDCompositionDeviceUnknown *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFromHwnd(IDCompositionDeviceUnknown *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSharedResource(IDCompositionDeviceUnknown *iface,
        REFIID riid, void **obj)
{
    FIXME("iface %p, riid %s, obj %p stub!\n", iface, debugstr_guid(riid), obj);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_OpenSharedResourceHandle(IDCompositionDeviceUnknown *iface,
        IUnknown *unknown, HANDLE *handle)
{
    FIXME("iface %p, unknown %p, handle %p stub!\n", iface, unknown, handle);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateFromSharedVisualHandle(IDCompositionDeviceUnknown *iface,
     HANDLE visual_handle, REFIID iid, void **out)
{
    HRESULT hr;

    FIXME("iface %p visual_handle %p, iid %s, out %p stub!\n", iface, visual_handle, debugstr_guid(iid), out);

    /* visual_handle is from DCompositionCreateSharedVisualHandle(). iid is IID_IDCompositionVisual
    * or IDCompositionTarget */
    assert(IsEqualGUID(iid, &IID_IDCompositionVisual) || IsEqualGUID(iid, &IID_IDCompositionTarget));
    assert(out);

    if (IsEqualGUID(iid, &IID_IDCompositionVisual))
    {
        hr = create_visual_from_shared_visual_handle(visual_handle, out);
        FIXME("Created a IDCompositionVisual %p from a shared visual handle %p, hr %#lx.\n", *out,
              visual_handle, hr);
        return hr;
    }
    else if (IsEqualGUID(iid, &IID_IDCompositionTarget))
    {
        hr = create_target_from_shared_visual_handle(impl_from_IDCompositionDeviceUnknown(iface),
                                                     visual_handle, (void **)out);
        FIXME("Created a IDCompositionTarget %p from a shared visual handle %p, hr %#lx.\n", *out,
              visual_handle, hr);
        return hr;
    }
    else
    {
        FIXME("Unsupported GUID %s, returning E_NOTIMPL.\n", debugstr_guid(iid));
        return E_NOTIMPL;
    }
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown1(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown2(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown3(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown4(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown5(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown6(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown7(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown8(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown9(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown10(IDCompositionDeviceUnknown *iface,
        IDCompositionDynamicTexture **texture)
{
    TRACE("iface %p, texture %p.\n", iface, texture);

    return create_dynamic_texture(texture);
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown11(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown12(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown13(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown14(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown15(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown16(IDCompositionDeviceUnknown *iface, void **parameter1)
{
    FIXME("iface %p parameter1 %p stub!\n", iface, parameter1);

    if (!parameter1)
        return E_INVALIDARG;

    *parameter1 = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown17(IDCompositionDeviceUnknown *iface, void **parameter1)
{
    FIXME("iface %p parameter1 %p stub!\n", iface, parameter1);

    if (!parameter1)
        return E_INVALIDARG;

    *parameter1 = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown18(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown19(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown20(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown21(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown22(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown23(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown24(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown25(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown26(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown27(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown28(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown29(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown30(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown31(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown32(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown33(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown34(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown35(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown36(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown37(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown38(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown39(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown40(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown41(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown42(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown43(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown44(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown45(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown46(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown47(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown48(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown49(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown50(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown51(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown52(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown53(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown54(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown55(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown56(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown57(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown58(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown59(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown60(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown61(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown62(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown63(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown64(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown65(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown66(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown67(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown68(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown69(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown70(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown71(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown72(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown73(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown74(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown75(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown76(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown77(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown78(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown79(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown80(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown81(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown82(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown83(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown84(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown85(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown86(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown87(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown88(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown89(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown90(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown91(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown92(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown93(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown94(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown95(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown96(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown97(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown98(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_Unknown99(IDCompositionDeviceUnknown *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static const struct IDCompositionDeviceUnknownVtbl desktop_device_vtbl =
{
    /* IUnknown methods */
    desktop_device_QueryInterface,
    desktop_device_AddRef,
    desktop_device_Release,
    /* IDCompositionDevice2 methods */
    desktop_device_Commit,
    desktop_device_WaitForCommitCompletion,
    desktop_device_GetFrameStatistics,
    desktop_device_CreateVisual,
    desktop_device_CreateSurfaceFactory,
    desktop_device_CreateSurface,
    desktop_device_CreateVirtualSurface,
    desktop_device_CreateTranslateTransform,
    desktop_device_CreateScaleTransform,
    desktop_device_CreateRotateTransform,
    desktop_device_CreateSkewTransform,
    desktop_device_CreateMatrixTransform,
    desktop_device_CreateTransformGroup,
    desktop_device_CreateTranslateTransform3D,
    desktop_device_CreateScaleTransform3D,
    desktop_device_CreateRotateTransform3D,
    desktop_device_CreateMatrixTransform3D,
    desktop_device_CreateTransform3DGroup,
    desktop_device_CreateEffectGroup,
    desktop_device_CreateRectangleClip,
    desktop_device_CreateAnimation,
    /* IDCompositionDesktopDevice methods */
    desktop_device_CreateTargetForHwnd,
    desktop_device_CreateSurfaceFromHandle,
    desktop_device_CreateSurfaceFromHwnd,
    /* IDCompositionDesktopDevicePartner methods */
    desktop_device_CreateSharedResource,
    desktop_device_OpenSharedResourceHandle,
    desktop_device_CreateFromSharedVisualHandle,
    /* IDCompositionDeviceUnknown methods */
    desktop_device_Unknown1,
    desktop_device_Unknown2,
    desktop_device_Unknown3,
    desktop_device_Unknown4,
    desktop_device_Unknown5,
    desktop_device_Unknown6,
    desktop_device_Unknown7,
    desktop_device_Unknown8,
    desktop_device_Unknown9,
    desktop_device_Unknown10,
    desktop_device_Unknown11,
    desktop_device_Unknown12,
    desktop_device_Unknown13,
    desktop_device_Unknown14,
    desktop_device_Unknown15,
    desktop_device_Unknown16,
    desktop_device_Unknown17,
    desktop_device_Unknown18,
    desktop_device_Unknown19,
    desktop_device_Unknown20,
    desktop_device_Unknown21,
    desktop_device_Unknown22,
    desktop_device_Unknown23,
    desktop_device_Unknown24,
    desktop_device_Unknown25,
    desktop_device_Unknown26,
    desktop_device_Unknown27,
    desktop_device_Unknown28,
    desktop_device_Unknown29,
    desktop_device_Unknown30,
    desktop_device_Unknown31,
    desktop_device_Unknown32,
    desktop_device_Unknown33,
    desktop_device_Unknown34,
    desktop_device_Unknown35,
    desktop_device_Unknown36,
    desktop_device_Unknown37,
    desktop_device_Unknown38,
    desktop_device_Unknown39,
    desktop_device_Unknown40,
    desktop_device_Unknown41,
    desktop_device_Unknown42,
    desktop_device_Unknown43,
    desktop_device_Unknown44,
    desktop_device_Unknown45,
    desktop_device_Unknown46,
    desktop_device_Unknown47,
    desktop_device_Unknown48,
    desktop_device_Unknown49,
    desktop_device_Unknown50,
    desktop_device_Unknown51,
    desktop_device_Unknown52,
    desktop_device_Unknown53,
    desktop_device_Unknown54,
    desktop_device_Unknown55,
    desktop_device_Unknown56,
    desktop_device_Unknown57,
    desktop_device_Unknown58,
    desktop_device_Unknown59,
    desktop_device_Unknown60,
    desktop_device_Unknown61,
    desktop_device_Unknown62,
    desktop_device_Unknown63,
    desktop_device_Unknown64,
    desktop_device_Unknown65,
    desktop_device_Unknown66,
    desktop_device_Unknown67,
    desktop_device_Unknown68,
    desktop_device_Unknown69,
    desktop_device_Unknown70,
    desktop_device_Unknown71,
    desktop_device_Unknown72,
    desktop_device_Unknown73,
    desktop_device_Unknown74,
    desktop_device_Unknown75,
    desktop_device_Unknown76,
    desktop_device_Unknown77,
    desktop_device_Unknown78,
    desktop_device_Unknown79,
    desktop_device_Unknown80,
    desktop_device_Unknown81,
    desktop_device_Unknown82,
    desktop_device_Unknown83,
    desktop_device_Unknown84,
    desktop_device_Unknown85,
    desktop_device_Unknown86,
    desktop_device_Unknown87,
    desktop_device_Unknown88,
    desktop_device_Unknown89,
    desktop_device_Unknown90,
    desktop_device_Unknown91,
    desktop_device_Unknown92,
    desktop_device_Unknown93,
    desktop_device_Unknown94,
    desktop_device_Unknown95,
    desktop_device_Unknown96,
    desktop_device_Unknown97,
    desktop_device_Unknown98,
    desktop_device_Unknown99,
};

static HRESULT create_device(int version, REFIID iid, void **out)
{
    struct composition_device *device;
    HRESULT hr;

    if (!out)
        return E_INVALIDARG;

    device = calloc(1, sizeof(*device));
    if (!device)
        return E_OUTOFMEMORY;

    list_init(&device->targets);
    device->IDCompositionDevice_iface.lpVtbl = &device_vtbl;
    device->IDCompositionDeviceUnknown_iface.lpVtbl = &desktop_device_vtbl;
    device->version = version;
    device->ref = 1;
    hr = IDCompositionDevice_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
    IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
    return hr;
}

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p\n", dxgi_device, debugstr_guid(iid), device);

    if (!device)
        return E_INVALIDARG;

    *device = NULL;

    if (!IsEqualIID(iid, &IID_IDCompositionDevice))
        return E_NOINTERFACE;

    return create_device(1, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p\n", rendering_device, debugstr_guid(iid), device);

    if (!device)
        return E_INVALIDARG;

    *device = NULL;

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    return create_device(2, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    FIXME("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    if (!device)
        return E_INVALIDARG;

    *device = NULL;

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    return create_device(3, iid, device);
}

HRESULT WINAPI DCompositionCreateSharedVisualHandle(HANDLE *handle)
{
    NTSTATUS status;

    FIXME("handle %p stub!\n", handle);

    if (!handle)
        return STATUS_INVALID_PARAMETER;

    SERVER_START_REQ( dcomp_create_shared_visual )
    {
        if (!(status = wine_server_call( req )))
            *handle = wine_server_ptr_handle( reply->handle );
    }
    SERVER_END_REQ;

    return (HRESULT)status;
}
