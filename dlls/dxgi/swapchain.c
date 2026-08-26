/*
 * Copyright 2008 Henri Verbeet for CodeWeavers
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
 *
 */

#include "dxgi_private.h"

#define VKD3D_NO_VULKAN_H
#define VKD3D_NO_WIN32_TYPES
#include "wine/vulkan.h"
#include <vkd3d.h>

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

const IID IID_IDCompositionTexture =
    {0x929bb1aa, 0x725f, 0x433b, {0xab, 0xd7, 0x27, 0x30, 0x75, 0xa8, 0x35, 0xf2}};

static DXGI_SWAP_EFFECT dxgi_swap_effect_from_wined3d(enum wined3d_swap_effect swap_effect)
{
    switch (swap_effect)
    {
        case WINED3D_SWAP_EFFECT_DISCARD:
            return DXGI_SWAP_EFFECT_DISCARD;
        case WINED3D_SWAP_EFFECT_SEQUENTIAL:
            return DXGI_SWAP_EFFECT_SEQUENTIAL;
        case WINED3D_SWAP_EFFECT_FLIP_DISCARD:
            return DXGI_SWAP_EFFECT_FLIP_DISCARD;
        case WINED3D_SWAP_EFFECT_FLIP_SEQUENTIAL:
            return DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        default:
            FIXME("Invalid swap effect %#x.\n", swap_effect);
            return DXGI_SWAP_EFFECT_DISCARD;
    }
}

static BOOL dxgi_validate_flip_swap_effect_format(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return TRUE;
        default:
            WARN("Invalid swapchain format %#x for flip presentation model.\n", format);
            return FALSE;
    }
}

BOOL dxgi_validate_swapchain_desc(const DXGI_SWAP_CHAIN_DESC1 *desc)
{
    unsigned int min_buffer_count;

    switch (desc->SwapEffect)
    {
        case DXGI_SWAP_EFFECT_DISCARD:
        case DXGI_SWAP_EFFECT_SEQUENTIAL:
            min_buffer_count = 1;
            break;

        case DXGI_SWAP_EFFECT_FLIP_DISCARD:
        case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
            min_buffer_count = 2;

            if (desc->Format && !dxgi_validate_flip_swap_effect_format(desc->Format))
                return FALSE;

            if (desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality)
            {
                WARN("Invalid sample desc %u, %u for swap effect %#x.\n",
                        desc->SampleDesc.Count, desc->SampleDesc.Quality, desc->SwapEffect);
                return FALSE;
            }
            break;

        default:
            WARN("Invalid swap effect %u used.\n", desc->SwapEffect);
            return FALSE;
    }

    if (desc->BufferCount < min_buffer_count || desc->BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS)
    {
        WARN("BufferCount is %u.\n", desc->BufferCount);
        return FALSE;
    }

    return TRUE;
}

HRESULT dxgi_get_output_from_window(IWineDXGIFactory *factory, HWND window, IDXGIOutput **dxgi_output)
{
    unsigned int adapter_idx, output_idx;
    DXGI_OUTPUT_DESC desc;
    IDXGIAdapter *adapter;
    IDXGIOutput *output;
    HMONITOR monitor;
    HRESULT hr;

    if (!(monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)))
    {
        WARN("Failed to get monitor from window.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    for (adapter_idx = 0; SUCCEEDED(hr = IWineDXGIFactory_EnumAdapters(factory, adapter_idx, &adapter));
            ++adapter_idx)
    {
        for (output_idx = 0; SUCCEEDED(hr = IDXGIAdapter_EnumOutputs(adapter, output_idx,
                &output)); ++output_idx)
        {
            if (FAILED(hr = IDXGIOutput_GetDesc(output, &desc)))
            {
                WARN("Adapter %u output %u: Failed to get output desc, hr %#lx.\n", adapter_idx,
                        output_idx, hr);
                IDXGIOutput_Release(output);
                continue;
            }

            if (desc.Monitor == monitor)
            {
                *dxgi_output = output;
                IDXGIAdapter_Release(adapter);
                return S_OK;
            }

            IDXGIOutput_Release(output);
        }
        IDXGIAdapter_Release(adapter);
    }

    if (hr != DXGI_ERROR_NOT_FOUND)
        WARN("Failed to enumerate outputs, hr %#lx.\n", hr);

    WARN("Output could not be found.\n");
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT dxgi_swapchain_resize_target(struct wined3d_swapchain_state *state,
        const DXGI_MODE_DESC *target_mode_desc)
{
    struct wined3d_display_mode mode;

    if (!target_mode_desc)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    TRACE("Mode: %s.\n", debug_dxgi_mode(target_mode_desc));

    if (target_mode_desc->Scaling)
        FIXME("Ignoring scaling %#x.\n", target_mode_desc->Scaling);

    wined3d_display_mode_from_dxgi(&mode, target_mode_desc);

    return wined3d_swapchain_state_resize_target(state, &mode);
}

static HWND d3d11_swapchain_get_hwnd(struct d3d11_swapchain *swapchain)
{
    struct wined3d_swapchain_desc wined3d_desc;

    wined3d_mutex_lock();
    wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &wined3d_desc);
    wined3d_mutex_unlock();

    return wined3d_desc.device_window;
}

static inline struct d3d11_swapchain *d3d11_swapchain_from_IDXGISwapChain4(IDXGISwapChain4 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_swapchain, IDXGISwapChain4_iface);
}

static inline struct d3d11_swapchain *d3d11_swapchain_from_IWineDXGISwapChain(IWineDXGISwapChain *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_swapchain, IWineDXGISwapChain_iface);
}

static inline struct d3d11_swapchain *d3d11_swapchain_from_IDCompositionTexture(IDCompositionTexture *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_swapchain, IDCompositionTexture_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_QueryInterface(IDXGISwapChain4 *iface, REFIID riid, void **object)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, riid %s, object %p\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_IUnknown)
            || IsEqualGUID(riid, &IID_IDXGIObject)
            || IsEqualGUID(riid, &IID_IDXGIDeviceSubObject)
            || IsEqualGUID(riid, &IID_IDXGISwapChain)
            || IsEqualGUID(riid, &IID_IDXGISwapChain1)
            || IsEqualGUID(riid, &IID_IDXGISwapChain2)
            || IsEqualGUID(riid, &IID_IDXGISwapChain3)
            || IsEqualGUID(riid, &IID_IDXGISwapChain4))
    {
        IUnknown_AddRef(iface);
        *object = iface;
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IWineDXGISwapChain))
    {
        IUnknown_AddRef(iface);
        *object = &swapchain->IWineDXGISwapChain_iface;
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IDCompositionTexture))
    {
        IUnknown_AddRef(iface);
        *object = &swapchain->IDCompositionTexture_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_texture_QueryInterface(IDCompositionTexture *iface,
        REFIID riid, void **object)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDCompositionTexture(iface);

    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (!object)
        return E_INVALIDARG;

    if (IsEqualGUID(riid, &IID_IDCompositionTexture))
    {
        IDXGISwapChain4_AddRef(&swapchain->IDXGISwapChain4_iface);
        *object = iface;
        return S_OK;
    }

    return IDXGISwapChain4_QueryInterface(&swapchain->IDXGISwapChain4_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE d3d11_swapchain_texture_AddRef(IDCompositionTexture *iface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDCompositionTexture(iface);

    return IDXGISwapChain4_AddRef(&swapchain->IDXGISwapChain4_iface);
}

static ULONG STDMETHODCALLTYPE d3d11_swapchain_texture_Release(IDCompositionTexture *iface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDCompositionTexture(iface);

    return IDXGISwapChain4_Release(&swapchain->IDXGISwapChain4_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_texture_SetSourceRect(IDCompositionTexture *iface,
        const D2D_RECT_U *source_rect)
{
    TRACE("iface %p, source_rect %p.\n", iface, source_rect);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_texture_SetColorSpace(IDCompositionTexture *iface,
        DXGI_COLOR_SPACE_TYPE color_space)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDCompositionTexture(iface);

    TRACE("iface %p, color_space %#x.\n", iface, color_space);

    return IDXGISwapChain4_SetColorSpace1(&swapchain->IDXGISwapChain4_iface, color_space);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_texture_SetAlphaMode(IDCompositionTexture *iface,
        DXGI_ALPHA_MODE alpha_mode)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDCompositionTexture(iface);

    TRACE("iface %p, alpha_mode %#x.\n", iface, alpha_mode);

    swapchain->alpha_mode = alpha_mode;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_texture_GetAvailableFence(IDCompositionTexture *iface,
        UINT64 *fence_value, REFIID iid, void **available_fence)
{
    FIXME("iface %p, fence_value %p, iid %s, available_fence %p stub!\n",
            iface, fence_value, debugstr_guid(iid), available_fence);

    if (fence_value)
        *fence_value = 0;
    if (available_fence)
        *available_fence = NULL;
    return E_NOTIMPL;
}

static const struct IDCompositionTextureVtbl d3d11_swapchain_texture_vtbl =
{
    d3d11_swapchain_texture_QueryInterface,
    d3d11_swapchain_texture_AddRef,
    d3d11_swapchain_texture_Release,
    d3d11_swapchain_texture_SetSourceRect,
    d3d11_swapchain_texture_SetColorSpace,
    d3d11_swapchain_texture_SetAlphaMode,
    d3d11_swapchain_texture_GetAvailableFence,
};

static ULONG STDMETHODCALLTYPE d3d11_swapchain_AddRef(IDXGISwapChain4 *iface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    ULONG refcount = InterlockedIncrement(&swapchain->refcount);

    TRACE("%p increasing refcount to %lu.\n", swapchain, refcount);

    if (refcount == 1)
        wined3d_swapchain_incref(swapchain->wined3d_swapchain);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_swapchain_Release(IDXGISwapChain4 *iface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    ULONG refcount = InterlockedDecrement(&swapchain->refcount);

    TRACE("%p decreasing refcount to %lu.\n", swapchain, refcount);

    if (!refcount)
    {
        IWineDXGIDevice *device = swapchain->device;
        if (swapchain->target)
        {
            WARN("Releasing fullscreen swapchain.\n");
            IDXGIOutput_Release(swapchain->target);
        }
        if (swapchain->front_buffer_surface)
            IDXGISurface_Release(swapchain->front_buffer_surface);
        IWineDXGIFactory_Release(swapchain->factory);
        wined3d_swapchain_decref(swapchain->wined3d_swapchain);
        IWineDXGIDevice_Release(device);
    }

    return refcount;
}

/* IDXGIObject methods */

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetPrivateData(IDXGISwapChain4 *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_set_private_data(&swapchain->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetPrivateDataInterface(IDXGISwapChain4 *iface,
        REFGUID guid, const IUnknown *object)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, object %p.\n", iface, debugstr_guid(guid), object);

    return dxgi_set_private_data_interface(&swapchain->private_store, guid, object);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetPrivateData(IDXGISwapChain4 *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_get_private_data(&swapchain->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetParent(IDXGISwapChain4 *iface, REFIID riid, void **parent)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, riid %s, parent %p.\n", iface, debugstr_guid(riid), parent);

    return IWineDXGIFactory_QueryInterface(swapchain->factory, riid, parent);
}

/* IDXGIDeviceSubObject methods */

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetDevice(IDXGISwapChain4 *iface, REFIID riid, void **device)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return IWineDXGIDevice_QueryInterface(swapchain->device, riid, device);
}

/* IDXGISwapChain1 methods */

static HRESULT d3d11_swapchain_present(struct d3d11_swapchain *swapchain,
        unsigned int sync_interval, unsigned int flags, const RECT *present_rect)
{
    HRESULT hr;

    if (sync_interval > 4)
    {
        WARN("Invalid sync interval %u.\n", sync_interval);
        return DXGI_ERROR_INVALID_CALL;
    }

    if (IsIconic(d3d11_swapchain_get_hwnd(swapchain)))
        return DXGI_STATUS_OCCLUDED;

    if (flags & ~(DXGI_PRESENT_TEST | DXGI_PRESENT_ALLOW_TEARING))
        FIXME("Unimplemented flags %#x.\n", flags);
    if (flags & DXGI_PRESENT_TEST)
    {
        WARN("Returning S_OK for DXGI_PRESENT_TEST.\n");
        return S_OK;
    }

    if (SUCCEEDED(hr = wined3d_swapchain_present(swapchain->wined3d_swapchain,
            present_rect, present_rect, NULL, sync_interval, 0)))
        InterlockedIncrement(&swapchain->present_count);
    return hr;
}

static BOOL d3d11_swapchain_desc_is_flip_model(const struct wined3d_swapchain_desc *desc)
{
    return desc->swap_effect == WINED3D_SWAP_EFFECT_FLIP_DISCARD
            || desc->swap_effect == WINED3D_SWAP_EFFECT_FLIP_SEQUENTIAL;
}

static struct wined3d_texture *d3d11_swapchain_get_presented_texture(struct d3d11_swapchain *swapchain,
        const struct wined3d_swapchain_desc *desc)
{
    if (d3d11_swapchain_desc_is_flip_model(desc) && desc->backbuffer_count > 1)
        return wined3d_swapchain_get_back_buffer(swapchain->wined3d_swapchain, desc->backbuffer_count - 1);

    return wined3d_swapchain_get_front_buffer(swapchain->wined3d_swapchain);
}

static void d3d11_swapchain_preserve_dirty_present(struct d3d11_swapchain *swapchain,
        const DXGI_PRESENT_PARAMETERS *present_parameters)
{
    struct wined3d_texture *presented_texture, *back_buffer;
    struct wined3d_swapchain_desc swapchain_desc;
    struct wined3d_sub_resource_desc desc;
    struct wined3d_device_context *context;
    LONG band_top, band_bottom, x, next_left;
    RECT bounds, dirty, rect;
    BOOL extended;
    UINT i;
    HRESULT hr;

    wined3d_mutex_lock();
    wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &swapchain_desc);
    if (!d3d11_swapchain_desc_is_flip_model(&swapchain_desc))
    {
        wined3d_mutex_unlock();
        return;
    }

    presented_texture = d3d11_swapchain_get_presented_texture(swapchain, &swapchain_desc);
    back_buffer = wined3d_swapchain_get_back_buffer(swapchain->wined3d_swapchain, 0);
    if (!presented_texture || !back_buffer || FAILED(wined3d_texture_get_sub_resource_desc(back_buffer, 0, &desc)))
    {
        wined3d_mutex_unlock();
        return;
    }

    SetRect(&bounds, 0, 0, desc.width, desc.height);
    context = wined3d_device_get_immediate_context(swapchain->wined3d_device);

    for (band_top = bounds.top; band_top < bounds.bottom; band_top = band_bottom)
    {
        band_bottom = bounds.bottom;
        for (i = 0; i < present_parameters->DirtyRectsCount; ++i)
        {
            if (!IntersectRect(&dirty, &present_parameters->pDirtyRects[i], &bounds))
                continue;
            if (dirty.top > band_top && dirty.top < band_bottom)
                band_bottom = dirty.top;
            if (dirty.bottom > band_top && dirty.bottom < band_bottom)
                band_bottom = dirty.bottom;
        }

        x = bounds.left;
        while (x < bounds.right)
        {
            extended = FALSE;
            next_left = bounds.right;
            for (i = 0; i < present_parameters->DirtyRectsCount; ++i)
            {
                if (!IntersectRect(&dirty, &present_parameters->pDirtyRects[i], &bounds)
                        || dirty.top >= band_bottom || dirty.bottom <= band_top || dirty.right <= x)
                    continue;

                if (dirty.left <= x)
                {
                    x = dirty.right;
                    extended = TRUE;
                }
                else if (dirty.left < next_left)
                {
                    next_left = dirty.left;
                }
            }

            if (extended)
                continue;

            SetRect(&rect, x, band_top, next_left, band_bottom);
            if (FAILED(hr = wined3d_device_context_blt(context, back_buffer, 0, &rect,
                    presented_texture, 0, &rect, 0, NULL, WINED3D_TEXF_POINT)))
                WARN("Failed to preserve dirty-present region %s, hr %#lx.\n",
                        wine_dbgstr_rect(&rect), hr);
            x = next_left;
        }
    }
    wined3d_mutex_unlock();
}

static HRESULT STDMETHODCALLTYPE DECLSPEC_HOTPATCH d3d11_swapchain_Present(IDXGISwapChain4 *iface, UINT sync_interval, UINT flags)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, sync_interval %u, flags %#x.\n", iface, sync_interval, flags);

    return d3d11_swapchain_present(swapchain, sync_interval, flags, NULL);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetBuffer(IDXGISwapChain4 *iface,
        UINT buffer_idx, REFIID riid, void **surface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    struct wined3d_texture *texture;
    IUnknown *parent;
    HRESULT hr;

    TRACE("iface %p, buffer_idx %u, riid %s, surface %p\n",
            iface, buffer_idx, debugstr_guid(riid), surface);

    wined3d_mutex_lock();

    if (!(texture = wined3d_swapchain_get_back_buffer(swapchain->wined3d_swapchain, buffer_idx)))
    {
        wined3d_mutex_unlock();
        return DXGI_ERROR_INVALID_CALL;
    }

    parent = wined3d_texture_get_parent(texture);
    hr = IUnknown_QueryInterface(parent, riid, surface);
    wined3d_mutex_unlock();

    return hr;
}

static HRESULT STDMETHODCALLTYPE DECLSPEC_HOTPATCH d3d11_swapchain_SetFullscreenState(IDXGISwapChain4 *iface,
        BOOL fullscreen, IDXGIOutput *target)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    struct wined3d_swapchain_desc swapchain_desc;
    struct wined3d_swapchain_state *state;
    struct dxgi_output *dxgi_output;
    LONG in_set_fullscreen_state;
    BOOL old_fs;
    HRESULT hr;

    TRACE("iface %p, fullscreen %#x, target %p.\n", iface, fullscreen, target);

    if (!fullscreen && target)
    {
        WARN("Invalid call.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (target)
    {
        IDXGIOutput_AddRef(target);
    }
    else if (FAILED(hr = IDXGISwapChain4_GetContainingOutput(iface, &target)))
    {
        WARN("Failed to get target output for swapchain, hr %#lx.\n", hr);
        return hr;
    }
    dxgi_output = unsafe_impl_from_IDXGIOutput(target);

    /* DXGI catches nested SetFullscreenState invocations, earlier versions of d3d
     * do not. Final Fantasy XIV depends on this behavior. It tries to call SFS on
     * WM_WINDOWPOSCHANGED messages. */
    in_set_fullscreen_state = InterlockedExchange(&swapchain->in_set_fullscreen_state, 1);
    if (in_set_fullscreen_state)
    {
        WARN("Nested invocation of SetFullscreenState.\n");
        IDXGIOutput_Release(target);
        IDXGISwapChain4_GetFullscreenState(iface, &old_fs, NULL);
        return old_fs == fullscreen ? S_OK : DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;
    }

    wined3d_mutex_lock();
    state = wined3d_swapchain_get_state(swapchain->wined3d_swapchain);
    wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &swapchain_desc);
    swapchain_desc.output = dxgi_output->wined3d_output;
    swapchain_desc.windowed = !fullscreen;
    hr = wined3d_swapchain_state_set_fullscreen(state, &swapchain_desc, NULL);
    if (FAILED(hr))
    {
        IDXGIOutput_Release(target);
        hr = DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        goto done;
    }

    if (!fullscreen)
    {
        IDXGIOutput_Release(target);
        target = NULL;
    }

    if (swapchain->target)
        IDXGIOutput_Release(swapchain->target);
    swapchain->target = target;

done:
    wined3d_mutex_unlock();
    InterlockedExchange(&swapchain->in_set_fullscreen_state, 0);
    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetFullscreenState(IDXGISwapChain4 *iface,
        BOOL *fullscreen, IDXGIOutput **target)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    struct wined3d_swapchain_desc swapchain_desc;
    HRESULT hr;

    TRACE("iface %p, fullscreen %p, target %p.\n", iface, fullscreen, target);

    if (fullscreen || target)
    {
        wined3d_mutex_lock();
        wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &swapchain_desc);
        wined3d_mutex_unlock();
    }

    if (fullscreen)
        *fullscreen = !swapchain_desc.windowed;

    if (target)
    {
        if (!swapchain_desc.windowed)
        {
            if (!swapchain->target && FAILED(hr = IDXGISwapChain4_GetContainingOutput(iface, &swapchain->target)))
                return hr;

            *target = swapchain->target;
            IDXGIOutput_AddRef(*target);
        }
        else
        {
            *target = NULL;
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetDesc(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_DESC *desc)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    struct wined3d_swapchain_desc wined3d_desc;

    TRACE("iface %p, desc %p.\n", iface, desc);

    if (!desc)
    {
        WARN("Invalid pointer.\n");
        return E_INVALIDARG;
    }

    wined3d_mutex_lock();
    wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &wined3d_desc);
    wined3d_mutex_unlock();

    FIXME("Ignoring ScanlineOrdering and Scaling.\n");

    desc->BufferDesc.Width = wined3d_desc.backbuffer_width;
    desc->BufferDesc.Height = wined3d_desc.backbuffer_height;
    desc->BufferDesc.RefreshRate.Numerator = wined3d_desc.refresh_rate;
    desc->BufferDesc.RefreshRate.Denominator = 1;
    desc->BufferDesc.Format = dxgi_format_from_wined3dformat(wined3d_desc.backbuffer_format);
    desc->BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    desc->BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    dxgi_sample_desc_from_wined3d(&desc->SampleDesc,
            wined3d_desc.multisample_type, wined3d_desc.multisample_quality);
    desc->BufferUsage = dxgi_usage_from_wined3d_bind_flags(wined3d_desc.backbuffer_bind_flags);
    desc->BufferCount = wined3d_desc.backbuffer_count;
    desc->OutputWindow = wined3d_desc.device_window;
    desc->Windowed = wined3d_desc.windowed;
    desc->SwapEffect = dxgi_swap_effect_from_wined3d(wined3d_desc.swap_effect);
    desc->Flags = dxgi_swapchain_flags_from_wined3d(wined3d_desc.flags);

    return S_OK;
}

static HRESULT d3d11_swapchain_create_d3d11_textures(struct d3d11_swapchain *swapchain,
        IWineDXGIDevice *device, struct wined3d_swapchain_desc *desc);

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_ResizeBuffers(IDXGISwapChain4 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    struct wined3d_swapchain_desc wined3d_desc;
    struct wined3d_texture *texture;
    IUnknown *parent;
    unsigned int i;
    HRESULT hr;

    TRACE("iface %p, buffer_count %u, width %u, height %u, format %s, flags %#x.\n",
            iface, buffer_count, width, height, debug_dxgi_format(format), flags);

    if (flags)
        FIXME("Ignoring flags %#x.\n", flags);

    wined3d_mutex_lock();
    wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &wined3d_desc);
    for (i = 0; i < wined3d_desc.backbuffer_count; ++i)
    {
        texture = wined3d_swapchain_get_back_buffer(swapchain->wined3d_swapchain, i);
        parent = wined3d_texture_get_parent(texture);
        IUnknown_AddRef(parent);
        if (IUnknown_Release(parent))
        {
            wined3d_mutex_unlock();
            return DXGI_ERROR_INVALID_CALL;
        }
    }
    if (format != DXGI_FORMAT_UNKNOWN)
        wined3d_desc.backbuffer_format = wined3dformat_from_dxgi_format(format);
    if (swapchain->front_buffer_surface)
    {
        IDXGISurface_Release(swapchain->front_buffer_surface);
        swapchain->front_buffer_surface = NULL;
    }
    hr = wined3d_swapchain_resize_buffers(swapchain->wined3d_swapchain, buffer_count, width, height,
            wined3d_desc.backbuffer_format, wined3d_desc.multisample_type, wined3d_desc.multisample_quality);
    /* wined3d_swapchain_resize_buffers() may recreate swapchain textures.
     * We do not need to remove the reference to the wined3d swapchain from the
     * old d3d11 textures: we just validated above that they have 0 references,
     * and therefore they are not actually holding a reference to the wined3d
     * swapchain, and will not do anything with it when they are destroyed. */
    d3d11_swapchain_create_d3d11_textures(swapchain, swapchain->device, &wined3d_desc);
    wined3d_mutex_unlock();

    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_ResizeTarget(IDXGISwapChain4 *iface,
        const DXGI_MODE_DESC *target_mode_desc)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    struct wined3d_swapchain_state *state;

    TRACE("iface %p, target_mode_desc %p.\n", iface, target_mode_desc);

    state = wined3d_swapchain_get_state(swapchain->wined3d_swapchain);

    return dxgi_swapchain_resize_target(state, target_mode_desc);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetContainingOutput(IDXGISwapChain4 *iface, IDXGIOutput **output)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    HWND window;

    TRACE("iface %p, output %p.\n", iface, output);

    if (swapchain->target)
    {
        IDXGIOutput_AddRef(*output = swapchain->target);
        return S_OK;
    }

    window = d3d11_swapchain_get_hwnd(swapchain);
    return dxgi_get_output_from_window(swapchain->factory, window, output);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetFrameStatistics(IDXGISwapChain4 *iface,
        DXGI_FRAME_STATISTICS *stats)
{
    FIXME("iface %p, stats %p stub!\n", iface, stats);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetLastPresentCount(IDXGISwapChain4 *iface,
        UINT *last_present_count)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, last_present_count %p.\n", iface, last_present_count);

    *last_present_count =  swapchain->present_count;

    return S_OK;
}

/* IDXGISwapChain1 methods */

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetDesc1(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_DESC1 *desc)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    struct wined3d_swapchain_desc wined3d_desc;

    TRACE("iface %p, desc %p.\n", iface, desc);

    if (!desc)
    {
        WARN("Invalid pointer.\n");
        return E_INVALIDARG;
    }

    wined3d_mutex_lock();
    wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &wined3d_desc);
    wined3d_mutex_unlock();

    FIXME("Ignoring Stereo and Scaling.\n");

    desc->Width = wined3d_desc.backbuffer_width;
    desc->Height = wined3d_desc.backbuffer_height;
    desc->Format = dxgi_format_from_wined3dformat(wined3d_desc.backbuffer_format);
    desc->Stereo = FALSE;
    dxgi_sample_desc_from_wined3d(&desc->SampleDesc,
            wined3d_desc.multisample_type, wined3d_desc.multisample_quality);
    desc->BufferUsage = dxgi_usage_from_wined3d_bind_flags(wined3d_desc.backbuffer_bind_flags);
    desc->BufferCount = wined3d_desc.backbuffer_count;
    desc->Scaling = DXGI_SCALING_STRETCH;
    desc->SwapEffect = dxgi_swap_effect_from_wined3d(wined3d_desc.swap_effect);
    desc->AlphaMode = swapchain->alpha_mode;
    desc->Flags = dxgi_swapchain_flags_from_wined3d(wined3d_desc.flags);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetFullscreenDesc(IDXGISwapChain4 *iface,
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    struct wined3d_swapchain_desc wined3d_desc;

    TRACE("iface %p, desc %p.\n", iface, desc);

    if (!desc)
    {
        WARN("Invalid pointer.\n");
        return E_INVALIDARG;
    }

    wined3d_mutex_lock();
    wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &wined3d_desc);
    wined3d_mutex_unlock();

    FIXME("Ignoring ScanlineOrdering and Scaling.\n");

    desc->RefreshRate.Numerator = wined3d_desc.refresh_rate;
    desc->RefreshRate.Denominator = 1;
    desc->ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    desc->Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    desc->Windowed = wined3d_desc.windowed;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetHwnd(IDXGISwapChain4 *iface, HWND *hwnd)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, hwnd %p.\n", iface, hwnd);

    if (!hwnd)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    *hwnd = d3d11_swapchain_get_hwnd(swapchain);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetCoreWindow(IDXGISwapChain4 *iface,
        REFIID iid, void **core_window)
{
    FIXME("iface %p, iid %s, core_window %p stub!\n", iface, debugstr_guid(iid), core_window);

    if (core_window)
        *core_window = NULL;

    return DXGI_ERROR_INVALID_CALL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_Present1(IDXGISwapChain4 *iface,
        UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS *present_parameters)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    RECT dirty_rect, rect;
    UINT i;

    TRACE("iface %p, sync_interval %u, flags %#x, present_parameters %p.\n",
            iface, sync_interval, flags, present_parameters);

    if (present_parameters && present_parameters->DirtyRectsCount)
    {
        if (!present_parameters->pDirtyRects)
            return DXGI_ERROR_INVALID_CALL;

        dirty_rect = present_parameters->pDirtyRects[0];
        for (i = 1; i < present_parameters->DirtyRectsCount; ++i)
        {
            rect = present_parameters->pDirtyRects[i];
            UnionRect(&dirty_rect, &dirty_rect, &rect);
        }

        if (present_parameters->pScrollRect || present_parameters->pScrollOffset)
            FIXME("Ignoring scroll rect %s and offset %p.\n",
                    wine_dbgstr_rect(present_parameters->pScrollRect), present_parameters->pScrollOffset);

        TRACE("Using dirty rect union %s.\n", wine_dbgstr_rect(&dirty_rect));
        d3d11_swapchain_preserve_dirty_present(swapchain, present_parameters);
        return d3d11_swapchain_present(swapchain, sync_interval, flags, &dirty_rect);
    }
    else if (present_parameters && (present_parameters->pScrollRect || present_parameters->pScrollOffset))
    {
        FIXME("Ignoring scroll rect %s and offset %p.\n",
                wine_dbgstr_rect(present_parameters->pScrollRect), present_parameters->pScrollOffset);
    }

    return d3d11_swapchain_present(swapchain, sync_interval, flags, NULL);
}

static BOOL STDMETHODCALLTYPE d3d11_swapchain_IsTemporaryMonoSupported(IDXGISwapChain4 *iface)
{
    FIXME("iface %p stub!\n", iface);

    return FALSE;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetRestrictToOutput(IDXGISwapChain4 *iface, IDXGIOutput **output)
{
    FIXME("iface %p, output %p stub!\n", iface, output);

    if (!output)
    {
        WARN("Invalid pointer.\n");
        return E_INVALIDARG;
    }

    *output = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetBackgroundColor(IDXGISwapChain4 *iface, const DXGI_RGBA *color)
{
    FIXME("iface %p, color %p stub!\n", iface, color);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetBackgroundColor(IDXGISwapChain4 *iface, DXGI_RGBA *color)
{
    FIXME("iface %p, color %p stub!\n", iface, color);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetRotation(IDXGISwapChain4 *iface, DXGI_MODE_ROTATION rotation)
{
    FIXME("iface %p, rotation %#x stub!\n", iface, rotation);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetRotation(IDXGISwapChain4 *iface, DXGI_MODE_ROTATION *rotation)
{
    FIXME("iface %p, rotation %p stub!\n", iface, rotation);

    return E_NOTIMPL;
}

/* IDXGISwapChain2 methods */

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetSourceSize(IDXGISwapChain4 *iface, UINT width, UINT height)
{
    FIXME("iface %p, width %u, height %u stub!\n", iface, width, height);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetSourceSize(IDXGISwapChain4 *iface, UINT *width, UINT *height)
{
    FIXME("iface %p, width %p, height %p stub!\n", iface, width, height);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetMaximumFrameLatency(IDXGISwapChain4 *iface, UINT max_latency)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    HRESULT hr;

    TRACE("iface %p, max_latency %u.\n", iface, max_latency);

    wined3d_mutex_lock();
    hr = wined3d_swapchain_set_max_frame_latency(swapchain->wined3d_swapchain, max_latency);
    wined3d_mutex_unlock();
    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetMaximumFrameLatency(IDXGISwapChain4 *iface, UINT *max_latency)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
    HRESULT hr;

    TRACE("iface %p, max_latency %p.\n", iface, max_latency);

    wined3d_mutex_lock();
    hr = wined3d_swapchain_get_max_frame_latency(swapchain->wined3d_swapchain, max_latency);
    wined3d_mutex_unlock();
    return hr;
}

static HANDLE STDMETHODCALLTYPE d3d11_swapchain_GetFrameLatencyWaitableObject(IDXGISwapChain4 *iface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p.\n", iface);

    return wined3d_swapchain_get_frame_latency_waitable_object(swapchain->wined3d_swapchain);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetMatrixTransform(IDXGISwapChain4 *iface,
        const DXGI_MATRIX_3X2_F *matrix)
{
    FIXME("iface %p, matrix %p stub!\n", iface, matrix);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_GetMatrixTransform(IDXGISwapChain4 *iface,
        DXGI_MATRIX_3X2_F *matrix)
{
    FIXME("iface %p, matrix %p stub!\n", iface, matrix);

    return E_NOTIMPL;
}

/* IDXGISwapChain3 methods */

static UINT STDMETHODCALLTYPE d3d11_swapchain_GetCurrentBackBufferIndex(IDXGISwapChain4 *iface)
{
    FIXME("iface %p stub!\n", iface);

    return 0;
}

static BOOL hdr10_chromaticity_is_valid(const UINT16 value[2])
{
    return value[0] <= 50000 && value[1] <= 50000
            && (UINT32)value[0] + value[1] <= 50000;
}

static BOOL hdr10_metadata_is_valid(const DXGI_HDR_METADATA_HDR10 *metadata)
{
    if (!hdr10_chromaticity_is_valid(metadata->RedPrimary)
            || !hdr10_chromaticity_is_valid(metadata->GreenPrimary)
            || !hdr10_chromaticity_is_valid(metadata->BluePrimary)
            || !hdr10_chromaticity_is_valid(metadata->WhitePoint))
        return FALSE;

    /* HDR10 permits zero for values that are not known.  Validate only
     * impossible PQ values and relationships for which both sides exist. */
    if (metadata->MaxMasteringLuminance > 10000
            || (metadata->MaxMasteringLuminance && metadata->MinMasteringLuminance
            > metadata->MaxMasteringLuminance * 10000u))
        return FALSE;
    if (metadata->MaxContentLightLevel > 10000
            || metadata->MaxFrameAverageLightLevel > 10000
            || (metadata->MaxContentLightLevel && metadata->MaxFrameAverageLightLevel
            > metadata->MaxContentLightLevel))
        return FALSE;

    return TRUE;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_CheckColorSpaceSupport(IDXGISwapChain4 *iface,
        DXGI_COLOR_SPACE_TYPE colour_space, UINT *colour_space_support)
{
    UINT support_flags = 0;

    TRACE("iface %p, colour_space %#x, colour_space_support %p.\n",
            iface, colour_space, colour_space_support);

    if (!colour_space_support)
        return E_INVALIDARG;

    if (colour_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
        support_flags |= DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;

    *colour_space_support = support_flags;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetColorSpace1(IDXGISwapChain4 *iface,
        DXGI_COLOR_SPACE_TYPE colour_space)
{
    TRACE("iface %p, colour_space %#x.\n", iface, colour_space);

    if (colour_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
    {
        WARN("Colour space %u not supported.\n", colour_space);
        return E_INVALIDARG;
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_ResizeBuffers1(IDXGISwapChain4 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
        const UINT *node_mask, IUnknown * const *present_queue)
{
    FIXME("iface %p, buffer_count %u, width %u, height %u, format %s, flags %#x, "
            "node_mask %p, present_queue %p stub!\n",
            iface, buffer_count, width, height, debug_dxgi_format(format), flags, node_mask, present_queue);

    return E_NOTIMPL;
}

/* IDXGISwapChain4 methods */

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_SetHDRMetaData(IDXGISwapChain4 *iface,
        DXGI_HDR_METADATA_TYPE type, UINT size, void *metadata)
{
    TRACE("iface %p, type %#x, size %#x, metadata %p.\n", iface, type, size, metadata);

    if (type == DXGI_HDR_METADATA_TYPE_NONE)
        return !size && !metadata ? S_OK : E_INVALIDARG;

    if (type != DXGI_HDR_METADATA_TYPE_HDR10)
        return E_INVALIDARG;
    if (size != sizeof(DXGI_HDR_METADATA_HDR10) || !metadata
            || !hdr10_metadata_is_valid(metadata))
        return E_INVALIDARG;

    /* The Wine D3D11/wined3d presentation provider has no explicit HDR
     * configuration hook.  Closed replacement providers own their swapchain
     * implementation and are not inferred here. */
    return DXGI_ERROR_INVALID_CALL;
}

static const struct IDXGISwapChain4Vtbl d3d11_swapchain_vtbl =
{
    /* IUnknown methods */
    d3d11_swapchain_QueryInterface,
    d3d11_swapchain_AddRef,
    d3d11_swapchain_Release,
    /* IDXGIObject methods */
    d3d11_swapchain_SetPrivateData,
    d3d11_swapchain_SetPrivateDataInterface,
    d3d11_swapchain_GetPrivateData,
    d3d11_swapchain_GetParent,
    /* IDXGIDeviceSubObject methods */
    d3d11_swapchain_GetDevice,
    /* IDXGISwapChain methods */
    d3d11_swapchain_Present,
    d3d11_swapchain_GetBuffer,
    d3d11_swapchain_SetFullscreenState,
    d3d11_swapchain_GetFullscreenState,
    d3d11_swapchain_GetDesc,
    d3d11_swapchain_ResizeBuffers,
    d3d11_swapchain_ResizeTarget,
    d3d11_swapchain_GetContainingOutput,
    d3d11_swapchain_GetFrameStatistics,
    d3d11_swapchain_GetLastPresentCount,
    /* IDXGISwapChain1 methods */
    d3d11_swapchain_GetDesc1,
    d3d11_swapchain_GetFullscreenDesc,
    d3d11_swapchain_GetHwnd,
    d3d11_swapchain_GetCoreWindow,
    d3d11_swapchain_Present1,
    d3d11_swapchain_IsTemporaryMonoSupported,
    d3d11_swapchain_GetRestrictToOutput,
    d3d11_swapchain_SetBackgroundColor,
    d3d11_swapchain_GetBackgroundColor,
    d3d11_swapchain_SetRotation,
    d3d11_swapchain_GetRotation,
    /* IDXGISwapChain2 methods */
    d3d11_swapchain_SetSourceSize,
    d3d11_swapchain_GetSourceSize,
    d3d11_swapchain_SetMaximumFrameLatency,
    d3d11_swapchain_GetMaximumFrameLatency,
    d3d11_swapchain_GetFrameLatencyWaitableObject,
    d3d11_swapchain_SetMatrixTransform,
    d3d11_swapchain_GetMatrixTransform,
    /* IDXGISwapChain3 methods */
    d3d11_swapchain_GetCurrentBackBufferIndex,
    d3d11_swapchain_CheckColorSpaceSupport,
    d3d11_swapchain_SetColorSpace1,
    d3d11_swapchain_ResizeBuffers1,
    /* IDXGISwapChain4 methods */
    d3d11_swapchain_SetHDRMetaData,
};

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_internal_QueryInterface(IWineDXGISwapChain *iface,
        REFIID riid, void **object)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IWineDXGISwapChain(iface);

    return IDXGISwapChain4_QueryInterface(&swapchain->IDXGISwapChain4_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE d3d11_swapchain_internal_AddRef(IWineDXGISwapChain *iface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IWineDXGISwapChain(iface);

    return IDXGISwapChain4_AddRef(&swapchain->IDXGISwapChain4_iface);
}

static ULONG STDMETHODCALLTYPE d3d11_swapchain_internal_Release(IWineDXGISwapChain *iface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IWineDXGISwapChain(iface);

    return IDXGISwapChain4_Release(&swapchain->IDXGISwapChain4_iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_swapchain_internal_get_front_buffer(IWineDXGISwapChain *iface,
        REFIID iid, void **surface)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IWineDXGISwapChain(iface);
    struct wined3d_swapchain_desc swapchain_desc;
    struct wined3d_texture *texture;
    IUnknown *parent;
    HRESULT hr;

    TRACE("iface %p, iid %s, surface %p.\n", iface, debugstr_guid(iid), surface);

    if (!surface)
        return E_INVALIDARG;

    *surface = NULL;
    wined3d_swapchain_wait_present(swapchain->wined3d_swapchain);

    wined3d_mutex_lock();
    wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &swapchain_desc);
    if (d3d11_swapchain_desc_is_flip_model(&swapchain_desc) && swapchain_desc.backbuffer_count > 1
            && (texture = d3d11_swapchain_get_presented_texture(swapchain, &swapchain_desc)))
    {
        parent = wined3d_texture_get_parent(texture);
        hr = IUnknown_QueryInterface(parent, iid, surface);
        wined3d_mutex_unlock();
        if (SUCCEEDED(hr))
            return hr;

        WARN("Failed to query presented back buffer surface, hr %#lx.\n", hr);
        *surface = NULL;
    }
    else
    {
        wined3d_mutex_unlock();
    }

    if (!swapchain->front_buffer_surface)
        return DXGI_ERROR_INVALID_CALL;

    return IDXGISurface_QueryInterface(swapchain->front_buffer_surface, iid, surface);
}

static const struct IWineDXGISwapChainVtbl d3d11_swapchain_internal_vtbl =
{
    /* IUnknown methods */
    d3d11_swapchain_internal_QueryInterface,
    d3d11_swapchain_internal_AddRef,
    d3d11_swapchain_internal_Release,
    /* IWineDXGISwapChain methods */
    d3d11_swapchain_internal_get_front_buffer,
};

static void STDMETHODCALLTYPE d3d11_swapchain_wined3d_object_released(void *parent)
{
    struct d3d11_swapchain *swapchain = parent;

    wined3d_private_store_cleanup(&swapchain->private_store);
    free(parent);
}

static const struct wined3d_parent_ops d3d11_swapchain_wined3d_parent_ops =
{
    d3d11_swapchain_wined3d_object_released,
};

static inline struct d3d11_swapchain *d3d11_swapchain_from_wined3d_swapchain_state_parent(struct wined3d_swapchain_state_parent *parent)
{
    return CONTAINING_RECORD(parent, struct d3d11_swapchain, state_parent);
}

static void CDECL d3d11_swapchain_windowed_state_changed(struct wined3d_swapchain_state_parent *parent,
        BOOL windowed)
{
    struct d3d11_swapchain *swapchain = d3d11_swapchain_from_wined3d_swapchain_state_parent(parent);

    TRACE("parent %p, windowed %d.\n", parent, windowed);

    if (windowed && swapchain->target)
    {
        IDXGIOutput_Release(swapchain->target);
        swapchain->target = NULL;
    }
}

static const struct wined3d_swapchain_state_parent_ops d3d11_swapchain_state_parent_ops =
{
    d3d11_swapchain_windowed_state_changed,
};

static BOOL d3d11_swapchain_format_supports_get_dc(enum wined3d_format_id format)
{
    return format == WINED3DFMT_B8G8R8A8_UNORM
            || format == WINED3DFMT_B8G8R8A8_TYPELESS
            || format == WINED3DFMT_B8G8R8A8_UNORM_SRGB;
}

static HRESULT d3d11_swapchain_create_d3d11_textures(struct d3d11_swapchain *swapchain,
        IWineDXGIDevice *device, struct wined3d_swapchain_desc *desc)
{
    IWineDXGIDeviceParent *dxgi_device_parent;
    unsigned int texture_flags = 0;
    unsigned int i;
    HRESULT hr;

    if (FAILED(hr = IWineDXGIDevice_QueryInterface(device,
            &IID_IWineDXGIDeviceParent, (void **)&dxgi_device_parent)))
    {
        ERR("Device should implement IWineDXGIDeviceParent.\n");
        return E_FAIL;
    }

    /* The software DComp compositor reads swapchain textures through a DC even
     * when the application did not request a GDI-compatible swapchain. Only
     * BGRA textures can carry D3D11_RESOURCE_MISC_GDI_COMPATIBLE, however. */
    if ((desc->flags & WINED3D_SWAPCHAIN_GDI_COMPATIBLE)
            || d3d11_swapchain_format_supports_get_dc(desc->backbuffer_format))
        texture_flags |= WINED3D_TEXTURE_CREATE_GET_DC;

    if (FAILED(hr = IWineDXGIDeviceParent_register_swapchain_texture(dxgi_device_parent,
            wined3d_swapchain_get_front_buffer(swapchain->wined3d_swapchain),
            texture_flags, &swapchain->front_buffer_surface)))
    {
        ERR("Failed to create parent swapchain front buffer texture, hr %#lx.\n", hr);
        IWineDXGIDeviceParent_Release(dxgi_device_parent);
        return hr;
    }

    for (i = 0; i < desc->backbuffer_count; ++i)
    {
        IDXGISurface *surface;

        if (FAILED(hr = IWineDXGIDeviceParent_register_swapchain_texture(dxgi_device_parent,
                wined3d_swapchain_get_back_buffer(swapchain->wined3d_swapchain, i), texture_flags, &surface)))
        {
            ERR("Failed to create parent swapchain texture, hr %#lx.\n", hr);
            break;
        }
        IDXGISurface_Release(surface);
    }

    IWineDXGIDeviceParent_Release(dxgi_device_parent);
    return hr;
}

HRESULT d3d11_swapchain_init(struct d3d11_swapchain *swapchain, struct dxgi_device *device,
        struct wined3d_swapchain_desc *desc, const DXGI_SWAP_CHAIN_DESC1 *dxgi_desc)
{
    struct wined3d_swapchain_state *state;
    BOOL fullscreen;
    HRESULT hr;

    if (desc->backbuffer_format == WINED3DFMT_UNKNOWN)
        return E_INVALIDARG;

    if (FAILED(hr = IWineDXGIAdapter_GetParent(device->adapter,
            &IID_IWineDXGIFactory, (void **)&swapchain->factory)))
    {
        WARN("Failed to get adapter parent, hr %#lx.\n", hr);
        return hr;
    }
    IWineDXGIDevice_AddRef(swapchain->device = &device->IWineDXGIDevice_iface);
    swapchain->wined3d_device = device->wined3d_device;

    swapchain->IDXGISwapChain4_iface.lpVtbl = &d3d11_swapchain_vtbl;
    swapchain->IWineDXGISwapChain_iface.lpVtbl = &d3d11_swapchain_internal_vtbl;
    swapchain->IDCompositionTexture_iface.lpVtbl = &d3d11_swapchain_texture_vtbl;
    swapchain->state_parent.ops = &d3d11_swapchain_state_parent_ops;
    swapchain->refcount = 1;
    swapchain->alpha_mode = dxgi_desc->AlphaMode == DXGI_ALPHA_MODE_UNSPECIFIED
            ? DXGI_ALPHA_MODE_IGNORE : dxgi_desc->AlphaMode;
    wined3d_mutex_lock();
    wined3d_private_store_init(&swapchain->private_store);

    if (!desc->windowed && (!desc->backbuffer_width || !desc->backbuffer_height))
        FIXME("Fullscreen swapchain with back buffer width/height equal to 0 not supported properly.\n");

    fullscreen = !desc->windowed;
    desc->windowed = TRUE;
    if (FAILED(hr = wined3d_swapchain_create(device->wined3d_device, desc, &swapchain->state_parent,
            swapchain, &d3d11_swapchain_wined3d_parent_ops, &swapchain->wined3d_swapchain)))
    {
        WARN("Failed to create wined3d swapchain, hr %#lx.\n", hr);
        if (hr == WINED3DERR_INVALIDCALL)
            hr = E_INVALIDARG;
        goto cleanup;
    }

    state = wined3d_swapchain_get_state(swapchain->wined3d_swapchain);
    wined3d_swapchain_state_get_size(state, &desc->backbuffer_width, &desc->backbuffer_height);

    if (FAILED(hr = d3d11_swapchain_create_d3d11_textures(swapchain, &device->IWineDXGIDevice_iface, desc)))
    {
        ERR("Failed to create d3d11 textures, hr %#lx.\n", hr);
        goto cleanup;
    }

    swapchain->target = NULL;
    if (fullscreen)
    {
        desc->windowed = FALSE;

        if (FAILED(hr = IDXGISwapChain4_GetContainingOutput(&swapchain->IDXGISwapChain4_iface,
                &swapchain->target)))
        {
            WARN("Failed to get target output for fullscreen swapchain, hr %#lx.\n", hr);
            wined3d_swapchain_decref(swapchain->wined3d_swapchain);
            goto cleanup;
        }

        if (FAILED(hr = wined3d_swapchain_state_set_fullscreen(state, desc, NULL)))
        {
            WARN("Failed to set fullscreen state, hr %#lx.\n", hr);
            IDXGIOutput_Release(swapchain->target);
            wined3d_swapchain_decref(swapchain->wined3d_swapchain);
            goto cleanup;
        }

    }
    wined3d_mutex_unlock();

    return S_OK;

cleanup:
    wined3d_private_store_cleanup(&swapchain->private_store);
    wined3d_mutex_unlock();
    IWineDXGIFactory_Release(swapchain->factory);
    IWineDXGIDevice_Release(swapchain->device);
    return hr;
}

struct dxgi_vk_funcs
{
    PFN_vkAcquireNextImageKHR p_vkAcquireNextImageKHR;
    PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
    PFN_vkAllocateMemory p_vkAllocateMemory;
    PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
    PFN_vkBindImageMemory p_vkBindImageMemory;
    PFN_vkCmdBlitImage p_vkCmdBlitImage;
    PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
    PFN_vkCreateCommandPool p_vkCreateCommandPool;
    PFN_vkCreateFence p_vkCreateFence;
    PFN_vkCreateImage p_vkCreateImage;
    PFN_vkCreateSemaphore p_vkCreateSemaphore;
    PFN_vkCreateSwapchainKHR p_vkCreateSwapchainKHR;
    PFN_vkCreateWin32SurfaceKHR p_vkCreateWin32SurfaceKHR;
    PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
    PFN_vkDestroyFence p_vkDestroyFence;
    PFN_vkDestroyImage p_vkDestroyImage;
    PFN_vkDestroySemaphore p_vkDestroySemaphore;
    PFN_vkDestroySurfaceKHR p_vkDestroySurfaceKHR;
    PFN_vkResetCommandBuffer p_vkResetCommandBuffer;
    PFN_vkDestroySwapchainKHR p_vkDestroySwapchainKHR;
    PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
    PFN_vkFreeMemory p_vkFreeMemory;
    PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
    PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
    PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR p_vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR p_vkGetPhysicalDeviceSurfacePresentModesKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR p_vkGetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR p_vkGetPhysicalDeviceWin32PresentationSupportKHR;
    PFN_vkGetSwapchainImagesKHR p_vkGetSwapchainImagesKHR;
    PFN_vkQueuePresentKHR p_vkQueuePresentKHR;
    PFN_vkQueueSubmit p_vkQueueSubmit;
    PFN_vkQueueWaitIdle p_vkQueueWaitIdle;
    PFN_vkResetFences p_vkResetFences;
    PFN_vkSetHdrMetadataEXT p_vkSetHdrMetadataEXT;
    PFN_vkWaitForFences p_vkWaitForFences;

    void *vulkan_module;
};

static HRESULT hresult_from_vk_result(VkResult vr)
{
    switch (vr)
    {
        case VK_SUCCESS:
            return S_OK;
        case VK_ERROR_OUT_OF_HOST_MEMORY:
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return E_OUTOFMEMORY;
        case VK_ERROR_DEVICE_LOST:
            return DXGI_ERROR_DEVICE_REMOVED;
        case VK_ERROR_SURFACE_LOST_KHR:
            return DXGI_ERROR_DEVICE_RESET;
        default:
            FIXME("Unhandled VkResult %d.\n", vr);
            return E_FAIL;
    }
}

#define INVALID_VK_IMAGE_INDEX (~(uint32_t)0)

struct d3d12_swapchain
{
    IDXGISwapChain4 IDXGISwapChain4_iface;
    LONG refcount;
    struct wined3d_private_store private_store;

    struct wined3d_swapchain_state *state;
    struct wined3d_swapchain_state_parent state_parent;

    VkSurfaceKHR vk_surface;
    VkFence vk_fence;
    VkInstance vk_instance;
    VkDevice vk_device;
    VkPhysicalDevice vk_physical_device;

    HANDLE worker_thread;
    HANDLE worker_stop_event;
    CRITICAL_SECTION worker_cs;
    CONDITION_VARIABLE worker_cv;
    bool worker_running;
    struct list worker_ops;
    LONG async_error;

    /* D3D12 side of the swapchain (frontend): these objects are
     * visible to the IDXGISwapChain client, so they must never be
     * recreated, except when ResizeBuffers*() is called. */
    unsigned int buffer_count;
    VkDeviceMemory vk_memory;
    VkImage vk_images[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    ID3D12Resource *buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    unsigned int current_buffer_index;
    VkFormat vk_format;

    /* Vulkan side of the swapchain (backend): these objects are also
     * destroyed and recreated when the Vulkan swapchain becomes out
     * of date or when the synchronization interval is changed; this
     * operation should be transparent to the IDXGISwapChain client
     * (except for timings: recreating the Vulkan swapchain creates a
     * noticeable delay, unfortunately). */
    VkSwapchainKHR vk_swapchain;
    VkCommandPool vk_cmd_pool;
    VkImage vk_swapchain_images[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    VkCommandBuffer vk_cmd_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    VkSemaphore vk_semaphores[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    unsigned int vk_swapchain_width;
    unsigned int vk_swapchain_height;
    VkPresentModeKHR present_mode;
    DXGI_SWAP_CHAIN_DESC1 backend_desc;

    ID3D12Fence *present_fence;

    uint32_t vk_image_index;

    struct dxgi_vk_funcs vk_funcs;

    ID3D12CommandQueue *command_queue;
    ID3D12Device *device;
    IWineDXGIFactory *factory;

    HWND window;
    IDXGIOutput *target;
    IDXGIOutput *restrict_to_output;
    DXGI_SWAP_CHAIN_DESC1 desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc;
    LONG in_set_fullscreen_state;

    HANDLE frame_latency_semaphore;

    uint64_t frame_number;
    uint32_t frame_latency;
    uint32_t pending_present_count;
    uint32_t pending_resize_count;
    bool frontend_resize_in_progress;
    bool color_reconfigure_in_progress;

    DXGI_COLOR_SPACE_TYPE color_space;
    DXGI_COLOR_SPACE_TYPE backend_color_space;
    DXGI_HDR_METADATA_HDR10 hdr10_metadata;
    bool has_hdr10_metadata;
};

enum d3d12_swapchain_op_type
{
    D3D12_SWAPCHAIN_OP_PRESENT,
    D3D12_SWAPCHAIN_OP_RESIZE_BUFFERS,
};

struct d3d12_swapchain_op
{
    struct list entry;
    enum d3d12_swapchain_op_type type;
    union
    {
        struct
        {
            unsigned int sync_interval;
            VkImage vk_image;
            uint64_t frame_number;
        } present;
        struct
        {
            /* The resize buffers op takes ownership of the memory and
             * images used to back the previous ID3D12Resource
             * objects, so that they're only released once the worker
             * thread is done with them. */
            VkDeviceMemory vk_memory;
            VkImage vk_images[DXGI_MAX_SWAP_CHAIN_BUFFERS];
            DXGI_SWAP_CHAIN_DESC1 desc;
        } resize_buffers;
    };
};

static void d3d12_swapchain_op_destroy(struct d3d12_swapchain *swapchain, struct d3d12_swapchain_op *op)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    unsigned int i;

    if (op->type == D3D12_SWAPCHAIN_OP_RESIZE_BUFFERS)
    {
        assert(swapchain->vk_device);

        for (i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; ++i)
        {
            if (op->resize_buffers.vk_images[i])
                vk_funcs->p_vkDestroyImage(swapchain->vk_device, op->resize_buffers.vk_images[i], NULL);
        }

        if (op->resize_buffers.vk_memory)
            vk_funcs->p_vkFreeMemory(swapchain->vk_device, op->resize_buffers.vk_memory, NULL);
    }

    free(op);
}

static HRESULT d3d12_swapchain_op_present_execute(struct d3d12_swapchain *swapchain, struct d3d12_swapchain_op *op);
static HRESULT d3d12_swapchain_op_resize_buffers_execute(struct d3d12_swapchain *swapchain, struct d3d12_swapchain_op *op);
static void d3d12_swapchain_apply_vk_hdr_metadata(struct d3d12_swapchain *swapchain);

static void d3d12_swapchain_record_async_error(struct d3d12_swapchain *swapchain, HRESULT hr)
{
    if (FAILED(hr))
        InterlockedCompareExchange(&swapchain->async_error, hr, S_OK);
}

static DWORD WINAPI d3d12_swapchain_worker_proc(void *data)
{
    struct d3d12_swapchain *swapchain = data;

    EnterCriticalSection(&swapchain->worker_cs);

    while (swapchain->worker_running)
    {
        if (!list_empty(&swapchain->worker_ops))
        {
            struct d3d12_swapchain_op *op = LIST_ENTRY(list_head(&swapchain->worker_ops), struct d3d12_swapchain_op, entry);
            bool present_op = op->type == D3D12_SWAPCHAIN_OP_PRESENT;
            bool resize_op = op->type == D3D12_SWAPCHAIN_OP_RESIZE_BUFFERS;
            HRESULT hr = E_UNEXPECTED;

            list_remove(&op->entry);

            LeaveCriticalSection(&swapchain->worker_cs);

            switch (op->type)
            {
                case D3D12_SWAPCHAIN_OP_PRESENT:
                    hr = d3d12_swapchain_op_present_execute(swapchain, op);
                    break;

                case D3D12_SWAPCHAIN_OP_RESIZE_BUFFERS:
                    hr = d3d12_swapchain_op_resize_buffers_execute(swapchain, op);
                    break;
            }

            d3d12_swapchain_record_async_error(swapchain, hr);
            if (present_op && !ReleaseSemaphore(swapchain->frame_latency_semaphore, 1, NULL))
            {
                hr = HRESULT_FROM_WIN32(GetLastError());
                ERR("Failed to release frame latency semaphore, hr %#lx.\n", hr);
                d3d12_swapchain_record_async_error(swapchain, hr);
            }

            d3d12_swapchain_op_destroy(swapchain, op);

            EnterCriticalSection(&swapchain->worker_cs);
            if (present_op)
            {
                assert(swapchain->pending_present_count);
                --swapchain->pending_present_count;
                WakeAllConditionVariable(&swapchain->worker_cv);
            }
            if (resize_op)
            {
                assert(swapchain->pending_resize_count);
                --swapchain->pending_resize_count;
                WakeAllConditionVariable(&swapchain->worker_cv);
            }
        }
        else if (!SleepConditionVariableCS(&swapchain->worker_cv, &swapchain->worker_cs, INFINITE))
        {
            HRESULT hr = HRESULT_FROM_WIN32(GetLastError());

            ERR("Cannot sleep on condition variable, hr %#lx.\n", hr);
            d3d12_swapchain_record_async_error(swapchain, hr);
            swapchain->worker_running = false;
            WakeAllConditionVariable(&swapchain->worker_cv);
            break;
        }
    }

    LeaveCriticalSection(&swapchain->worker_cs);

    return 0;
}

static DXGI_FORMAT dxgi_format_from_vk_format(VkFormat vk_format)
{
    switch (vk_format)
    {
        case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            WARN("Unhandled format %#x.\n", vk_format);
            return DXGI_FORMAT_UNKNOWN;
    }
}

static VkFormat get_swapchain_fallback_format(VkFormat vk_format)
{
    switch (vk_format)
    {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return VK_FORMAT_B8G8R8A8_UNORM;
        default:
            WARN("Unhandled format %#x.\n", vk_format);
            return VK_FORMAT_UNDEFINED;
    }
}

static HRESULT get_vk_surface_format(const DXGI_SWAP_CHAIN_DESC1 *swapchain_desc,
        DXGI_COLOR_SPACE_TYPE color_space, VkFormat *format, VkColorSpaceKHR *vk_color_space)
{
    *format = vkd3d_get_vk_format(swapchain_desc->Format);

    switch (color_space)
    {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
            *vk_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            return *format != VK_FORMAT_UNDEFINED ? S_OK : E_INVALIDARG;

        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
            if (swapchain_desc->Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
                return E_INVALIDARG;
            *vk_color_space = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
            return S_OK;

        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
            if (swapchain_desc->Format != DXGI_FORMAT_R10G10B10A2_UNORM)
                return E_INVALIDARG;
            *vk_color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT;
            return S_OK;

        default:
            return E_INVALIDARG;
    }
}

static HRESULT select_vk_format(const struct dxgi_vk_funcs *vk_funcs,
        VkPhysicalDevice vk_physical_device, VkSurfaceKHR vk_surface,
        const DXGI_SWAP_CHAIN_DESC1 *swapchain_desc, DXGI_COLOR_SPACE_TYPE color_space,
        VkFormat *vk_format, VkColorSpaceKHR *vk_color_space)
{
    VkSurfaceFormatKHR *formats;
    uint32_t format_count = 0, format_capacity;
    VkFormat format;
    unsigned int i;
    VkResult vr;

    *vk_format = VK_FORMAT_UNDEFINED;
    if (FAILED(get_vk_surface_format(swapchain_desc, color_space, &format, vk_color_space)))
        return E_INVALIDARG;

    vr = vk_funcs->p_vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface, &format_count, NULL);
    if (vr < 0 || !format_count || format_count > 4096)
    {
        WARN("Failed to get a bounded supported surface format list, count %u, vr %d.\n",
                format_count, vr);
        return DXGI_ERROR_INVALID_CALL;
    }

    format_capacity = format_count;
    if (!(formats = calloc(format_count, sizeof(*formats))))
        return E_OUTOFMEMORY;

    if ((vr = vk_funcs->p_vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device,
            vk_surface, &format_count, formats)) < 0)
    {
        WARN("Failed to enumerate supported surface formats, vr %d.\n", vr);
        free(formats);
        return hresult_from_vk_result(vr);
    }
    if (format_count > format_capacity)
    {
        WARN("Surface format count grew from %u to %u.\n", format_capacity, format_count);
        free(formats);
        return DXGI_ERROR_INVALID_CALL;
    }

    for (i = 0; i < format_count; ++i)
    {
        if (formats[i].format == format && formats[i].colorSpace == *vk_color_space)
            break;
    }
    if (i == format_count && color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
    {
        /* Try to create a swapchain with format conversion. */
        format = get_swapchain_fallback_format(format);
        WARN("Failed to find Vulkan swapchain format for %s.\n", debug_dxgi_format(swapchain_desc->Format));
        for (i = 0; i < format_count; ++i)
        {
            if (formats[i].format == format && formats[i].colorSpace == *vk_color_space)
            {
                format = formats[i].format;
                break;
            }
        }
    }
    free(formats);
    if (i == format_count)
    {
        FIXME("Failed to find Vulkan swapchain format for %s.\n", debug_dxgi_format(swapchain_desc->Format));
        return DXGI_ERROR_UNSUPPORTED;
    }

    TRACE("Using Vulkan swapchain format %#x.\n", format);

    *vk_format = format;
    return S_OK;
}

static HRESULT vk_select_memory_type(const struct dxgi_vk_funcs *vk_funcs,
        VkPhysicalDevice vk_physical_device, uint32_t memory_type_mask,
        VkMemoryPropertyFlags flags, uint32_t *memory_type_index)
{
    VkPhysicalDeviceMemoryProperties memory_properties;
    unsigned int i;

    vk_funcs->p_vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &memory_properties);
    for (i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        if (!(memory_type_mask & (1u << i)))
            continue;

        if ((memory_properties.memoryTypes[i].propertyFlags & flags) == flags)
        {
            *memory_type_index = i;
            return S_OK;
        }
    }

    FIXME("Failed to find memory type (allowed types %#x).\n", memory_type_mask);
    return E_FAIL;
}

static BOOL d3d12_swapchain_is_present_mode_supported(struct d3d12_swapchain *swapchain,
        VkPresentModeKHR present_mode)
{
    VkPhysicalDevice vk_physical_device = swapchain->vk_physical_device;
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    VkPresentModeKHR *modes;
    uint32_t count, i;
    BOOL supported;
    VkResult vr;

    if (present_mode == VK_PRESENT_MODE_FIFO_KHR)
        return TRUE;

    if ((vr = vk_funcs->p_vkGetPhysicalDeviceSurfacePresentModesKHR(vk_physical_device,
            swapchain->vk_surface, &count, NULL)) < 0)
    {
        WARN("Failed to get count of available present modes, vr %d.\n", vr);
        return FALSE;
    }

    supported = FALSE;

    if (!(modes = calloc(count, sizeof(*modes))))
        return FALSE;
    if ((vr = vk_funcs->p_vkGetPhysicalDeviceSurfacePresentModesKHR(vk_physical_device,
            swapchain->vk_surface, &count, modes)) >= 0)
    {
        for (i = 0; i < count; ++i)
        {
            if (modes[i] == present_mode)
            {
                supported = TRUE;
                break;
            }
        }
    }
    else
    {
        WARN("Failed to get available present modes, vr %d.\n", vr);
    }
    free(modes);

    return supported;
}

static HRESULT d3d12_swapchain_create_user_buffers(struct d3d12_swapchain *swapchain)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    VkDeviceSize image_offset[DXGI_MAX_SWAP_CHAIN_BUFFERS] = {0};
    VkDevice vk_device = swapchain->vk_device;
    VkMemoryAllocateInfo allocate_info;
    VkMemoryRequirements requirements;
    VkImageCreateInfo image_info;
    uint32_t memory_type_mask;
    VkDeviceSize memory_size;
    unsigned int i;
    VkResult vr;
    HRESULT hr;

    memset(&image_info, 0, sizeof(image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = swapchain->vk_format;
    image_info.extent.width = swapchain->desc.Width;
    image_info.extent.height = swapchain->desc.Height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
            | VK_IMAGE_USAGE_TRANSFER_DST_BIT
            | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (swapchain->desc.BufferUsage & DXGI_USAGE_SHADER_INPUT)
            image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.queueFamilyIndexCount = 0;
    image_info.pQueueFamilyIndices = NULL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    for (i = 0; i < swapchain->desc.BufferCount; ++i)
    {
        assert(swapchain->vk_images[i] == VK_NULL_HANDLE);
        if ((vr = vk_funcs->p_vkCreateImage(vk_device, &image_info, NULL, &swapchain->vk_images[i])) < 0)
        {
            WARN("Failed to create Vulkan image, vr %d.\n", vr);
            swapchain->vk_images[i] = VK_NULL_HANDLE;
            return hresult_from_vk_result(vr);
        }
    }

    memory_size = 0;
    memory_type_mask = ~0u;
    for (i = 0; i < swapchain->desc.BufferCount; ++i)
    {
        vk_funcs->p_vkGetImageMemoryRequirements(vk_device, swapchain->vk_images[i], &requirements);

        TRACE("Size %s, alignment %s, memory types %#x.\n",
                wine_dbgstr_longlong(requirements.size), wine_dbgstr_longlong(requirements.alignment),
                requirements.memoryTypeBits);

        if (!requirements.alignment || (requirements.alignment & (requirements.alignment - 1)))
        {
            ERR("Invalid image memory alignment %s.\n", wine_dbgstr_longlong(requirements.alignment));
            return E_FAIL;
        }
        if (memory_size > UINT64_MAX - (requirements.alignment - 1))
        {
            ERR("Image memory offset overflows VkDeviceSize.\n");
            return E_OUTOFMEMORY;
        }
        image_offset[i] = (memory_size + (requirements.alignment - 1)) & ~(requirements.alignment - 1);
        if (image_offset[i] > UINT64_MAX - requirements.size)
        {
            ERR("Image memory allocation size overflows VkDeviceSize.\n");
            return E_OUTOFMEMORY;
        }
        memory_size = image_offset[i] + requirements.size;

        memory_type_mask &= requirements.memoryTypeBits;
    }

    TRACE("Allocating %s bytes for user images.\n", wine_dbgstr_longlong(memory_size));

    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = NULL;
    allocate_info.allocationSize = memory_size;
    allocate_info.memoryTypeIndex = 0;

    if (FAILED(hr = vk_select_memory_type(vk_funcs, swapchain->vk_physical_device,
            memory_type_mask, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &allocate_info.memoryTypeIndex)))
        return hr;

    assert(swapchain->vk_memory == VK_NULL_HANDLE);
    if ((vr = vk_funcs->p_vkAllocateMemory(vk_device, &allocate_info, NULL, &swapchain->vk_memory)) < 0)
    {
        WARN("Failed to allocate device memory, vr %d.\n", vr);
        swapchain->vk_memory = VK_NULL_HANDLE;
        return hresult_from_vk_result(vr);
    }

    for (i = 0; i < swapchain->desc.BufferCount; ++i)
    {
        if ((vr = vk_funcs->p_vkBindImageMemory(vk_device, swapchain->vk_images[i],
                swapchain->vk_memory, image_offset[i])) < 0)
        {
            WARN("Failed to bind image memory, vr %d.\n", vr);
            return hresult_from_vk_result(vr);
        }
    }

    return S_OK;
}

static void vk_cmd_image_barrier(const struct dxgi_vk_funcs *vk_funcs, VkCommandBuffer cmd_buffer,
        VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
        VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
        VkImageLayout old_layout, VkImageLayout new_layout, VkImage image)
{
    VkImageMemoryBarrier barrier;

    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.pNext = NULL;
    barrier.srcAccessMask = src_access_mask;
    barrier.dstAccessMask = dst_access_mask;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    vk_funcs->p_vkCmdPipelineBarrier(cmd_buffer,
            src_stage_mask, dst_stage_mask, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static VkResult d3d12_swapchain_record_swapchain_blit(struct d3d12_swapchain *swapchain,
        VkCommandBuffer vk_cmd_buffer, VkImage vk_dst_image, VkImage vk_src_image)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    VkCommandBufferBeginInfo begin_info;
    VkImageBlit blit;
    VkFilter filter;
    VkResult vr;

    if (swapchain->backend_desc.Width != swapchain->vk_swapchain_width
            || swapchain->backend_desc.Height != swapchain->vk_swapchain_height)
        filter = VK_FILTER_LINEAR;
    else
        filter = VK_FILTER_NEAREST;

    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext = NULL;
    begin_info.flags = 0;
    begin_info.pInheritanceInfo = NULL;

    if ((vr = vk_funcs->p_vkBeginCommandBuffer(vk_cmd_buffer, &begin_info)) < 0)
    {
        WARN("Failed to begin command buffer, vr %d.\n", vr);
        return vr;
    }

    vk_cmd_image_barrier(vk_funcs, vk_cmd_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, vk_dst_image);

    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = 0;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].x = 0;
    blit.srcOffsets[0].y = 0;
    blit.srcOffsets[0].z = 0;
    blit.srcOffsets[1].x = swapchain->backend_desc.Width;
    blit.srcOffsets[1].y = swapchain->backend_desc.Height;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0].x = 0;
    blit.dstOffsets[0].y = 0;
    blit.dstOffsets[0].z = 0;
    if (swapchain->backend_desc.Scaling == DXGI_SCALING_NONE)
    {
        blit.srcOffsets[1].x = min(swapchain->vk_swapchain_width, blit.srcOffsets[1].x);
        blit.srcOffsets[1].y = min(swapchain->vk_swapchain_height, blit.srcOffsets[1].y);
        blit.dstOffsets[1].x = blit.srcOffsets[1].x;
        blit.dstOffsets[1].y = blit.srcOffsets[1].y;
    }
    else
    {
        /* FIXME: handle DXGI_SCALING_ASPECT_RATIO_STRETCH. */
        blit.dstOffsets[1].x = swapchain->vk_swapchain_width;
        blit.dstOffsets[1].y = swapchain->vk_swapchain_height;
    }
    blit.dstOffsets[1].z = 1;

    vk_funcs->p_vkCmdBlitImage(vk_cmd_buffer,
            vk_src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            vk_dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, filter);

    vk_cmd_image_barrier(vk_funcs, vk_cmd_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, vk_dst_image);

    if ((vr = vk_funcs->p_vkEndCommandBuffer(vk_cmd_buffer)) < 0)
        WARN("Failed to end command buffer, vr %d.\n", vr);

    return vr;
}

static HRESULT d3d12_swapchain_create_command_buffers(struct d3d12_swapchain *swapchain)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    VkDevice vk_device = swapchain->vk_device;
    VkCommandBufferAllocateInfo allocate_info;
    VkSemaphoreCreateInfo semaphore_info;
    VkCommandPoolCreateInfo pool_info;
    unsigned int i;
    VkResult vr;

    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.pNext = NULL;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vkd3d_get_vk_queue_family_index(swapchain->command_queue);

    assert(swapchain->vk_cmd_pool == VK_NULL_HANDLE);
    if ((vr = vk_funcs->p_vkCreateCommandPool(vk_device, &pool_info,
            NULL, &swapchain->vk_cmd_pool)) < 0)
    {
        WARN("Failed to create command pool, vr %d.\n", vr);
        swapchain->vk_cmd_pool = VK_NULL_HANDLE;
        return hresult_from_vk_result(vr);
    }

    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.pNext = NULL;
    allocate_info.commandPool = swapchain->vk_cmd_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = swapchain->buffer_count;

    if ((vr = vk_funcs->p_vkAllocateCommandBuffers(vk_device, &allocate_info,
            swapchain->vk_cmd_buffers)) < 0)
    {
        WARN("Failed to allocate command buffers, vr %d.\n", vr);
        goto fail;
    }

    for (i = 0; i < swapchain->buffer_count; ++i)
    {
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore_info.pNext = NULL;
        semaphore_info.flags = 0;

        assert(swapchain->vk_semaphores[i] == VK_NULL_HANDLE);
        if ((vr = vk_funcs->p_vkCreateSemaphore(vk_device, &semaphore_info,
                NULL, &swapchain->vk_semaphores[i])) < 0)
        {
            WARN("Failed to create semaphore, vr %d.\n", vr);
            swapchain->vk_semaphores[i] = VK_NULL_HANDLE;
            goto fail;
        }
    }

    return S_OK;

fail:
    for (i = 0; i < swapchain->buffer_count; ++i)
    {
        if (swapchain->vk_semaphores[i])
        {
            vk_funcs->p_vkDestroySemaphore(vk_device, swapchain->vk_semaphores[i], NULL);
            swapchain->vk_semaphores[i] = VK_NULL_HANDLE;
        }
        swapchain->vk_cmd_buffers[i] = VK_NULL_HANDLE;
    }
    if (swapchain->vk_cmd_pool)
    {
        vk_funcs->p_vkDestroyCommandPool(vk_device, swapchain->vk_cmd_pool, NULL);
        swapchain->vk_cmd_pool = VK_NULL_HANDLE;
    }
    return hresult_from_vk_result(vr);
}

static HRESULT d3d12_swapchain_create_image_resources(struct d3d12_swapchain *swapchain)
{
    struct vkd3d_image_resource_create_info resource_info;
    ID3D12Device *device = swapchain->device;
    unsigned int i;
    HRESULT hr;

    resource_info.type = VKD3D_STRUCTURE_TYPE_IMAGE_RESOURCE_CREATE_INFO;
    resource_info.next = NULL;
    resource_info.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_info.desc.Alignment = 0;
    resource_info.desc.Width = swapchain->desc.Width;
    resource_info.desc.Height = swapchain->desc.Height;
    resource_info.desc.DepthOrArraySize = 1;
    resource_info.desc.MipLevels = 1;
    resource_info.desc.Format = dxgi_format_from_vk_format(swapchain->vk_format);
    resource_info.desc.SampleDesc.Count = 1;
    resource_info.desc.SampleDesc.Quality = 0;
    resource_info.desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_info.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    resource_info.flags = VKD3D_RESOURCE_INITIAL_STATE_TRANSITION | VKD3D_RESOURCE_PRESENT_STATE_TRANSITION;
    resource_info.present_state = D3D12_RESOURCE_STATE_COPY_SOURCE;

    for (i = 0; i < swapchain->desc.BufferCount; ++i)
    {
        assert(swapchain->vk_images[i]);
        assert(!swapchain->buffers[i]);

        resource_info.vk_image = swapchain->vk_images[i];

        if (FAILED(hr = vkd3d_create_image_resource(device, &resource_info, &swapchain->buffers[i])))
        {
            WARN("Failed to create vkd3d resource for Vulkan image %u, hr %#lx.\n", i, hr);
            return hr;
        }

        vkd3d_resource_incref(swapchain->buffers[i]);
        ID3D12Resource_Release(swapchain->buffers[i]);
    }

    return S_OK;
}

static VkResult d3d12_swapchain_acquire_next_vulkan_image(struct d3d12_swapchain *swapchain)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    VkDevice vk_device = swapchain->vk_device;
    VkFence vk_fence = swapchain->vk_fence;
    VkResult vr;

    swapchain->vk_image_index = INVALID_VK_IMAGE_INDEX;

    do
    {
        vr = vk_funcs->p_vkAcquireNextImageKHR(vk_device, swapchain->vk_swapchain, 1000000,
                VK_NULL_HANDLE, vk_fence, &swapchain->vk_image_index);
        if ((vr == VK_TIMEOUT || vr == VK_NOT_READY) && swapchain->worker_stop_event
                && WaitForSingleObject(swapchain->worker_stop_event, 0) == WAIT_OBJECT_0)
            return VK_ERROR_DEVICE_LOST;
    } while (vr == VK_TIMEOUT || vr == VK_NOT_READY);

    if (vr < 0)
    {
        WARN("Failed to acquire next Vulkan image, vr %d.\n", vr);
        return vr;
    }

    do
    {
        vr = vk_funcs->p_vkWaitForFences(vk_device, 1, &vk_fence, VK_TRUE, 1000000);
        if (vr == VK_TIMEOUT && swapchain->worker_stop_event
                && WaitForSingleObject(swapchain->worker_stop_event, 0) == WAIT_OBJECT_0)
            return VK_ERROR_DEVICE_LOST;
    } while (vr == VK_TIMEOUT);
    if (vr != VK_SUCCESS)
    {
        ERR("Failed to wait for fence, vr %d.\n", vr);
        return vr;
    }
    if ((vr = vk_funcs->p_vkResetFences(vk_device, 1, &vk_fence)) < 0)
        ERR("Failed to reset fence, vr %d.\n", vr);

    return vr;
}

static HRESULT d3d12_swapchain_destroy_vulkan_resources(struct d3d12_swapchain *swapchain)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    HRESULT hr = S_OK;
    VkQueue vk_queue;
    unsigned int i;
    VkResult vr;

    if (swapchain->command_queue)
    {
        vk_queue = vkd3d_acquire_vk_queue(swapchain->command_queue);
        if ((vr = vk_funcs->p_vkQueueWaitIdle(vk_queue)) < 0)
        {
            WARN("Failed to wait for presentation queue, vr %d.\n", vr);
            hr = hresult_from_vk_result(vr);
        }
        vkd3d_release_vk_queue(swapchain->command_queue);
    }

    if (swapchain->vk_device)
    {
        for (i = 0; i < swapchain->buffer_count; ++i)
        {
            if (swapchain->vk_semaphores[i])
            {
                vk_funcs->p_vkDestroySemaphore(swapchain->vk_device, swapchain->vk_semaphores[i], NULL);
                swapchain->vk_semaphores[i] = VK_NULL_HANDLE;
            }
            swapchain->vk_cmd_buffers[i] = VK_NULL_HANDLE;
        }

        if (swapchain->vk_cmd_pool)
        {
            vk_funcs->p_vkDestroyCommandPool(swapchain->vk_device, swapchain->vk_cmd_pool, NULL);
            swapchain->vk_cmd_pool = VK_NULL_HANDLE;
        }
    }

    return hr;
}

static void d3d12_swapchain_destroy_d3d12_resources(struct d3d12_swapchain *swapchain)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    unsigned int i;

    for (i = 0; i < swapchain->desc.BufferCount; ++i)
    {
        if (swapchain->buffers[i])
        {
            vkd3d_resource_decref(swapchain->buffers[i]);
            swapchain->buffers[i] = NULL;
        }
        if (swapchain->vk_device && swapchain->vk_images[i])
        {
            vk_funcs->p_vkDestroyImage(swapchain->vk_device, swapchain->vk_images[i], NULL);
            swapchain->vk_images[i] = VK_NULL_HANDLE;
        }
    }
    if (swapchain->vk_device && swapchain->vk_memory)
    {
        vk_funcs->p_vkFreeMemory(swapchain->vk_device, swapchain->vk_memory, NULL);
        swapchain->vk_memory = VK_NULL_HANDLE;
    }
}

static HRESULT d3d12_swapchain_create_vulkan_swapchain(struct d3d12_swapchain *swapchain)
{
    VkPhysicalDevice vk_physical_device = swapchain->vk_physical_device;
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    VkSwapchainCreateInfoKHR vk_swapchain_desc;
    VkImage vk_swapchain_images[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    VkDevice vk_device = swapchain->vk_device;
    unsigned int width, height, image_count;
    VkSurfaceCapabilitiesKHR surface_caps;
    VkColorSpaceKHR vk_color_space;
    VkFormat vk_swapchain_format;
    VkSwapchainKHR vk_swapchain;
    VkImageUsageFlags usage;
    VkResult vr;
    HRESULT hr;

    if (FAILED(hr = select_vk_format(vk_funcs, vk_physical_device,
            swapchain->vk_surface, &swapchain->backend_desc, swapchain->backend_color_space,
            &vk_swapchain_format, &vk_color_space)))
        return hr;

    if ((vr = vk_funcs->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_physical_device,
            swapchain->vk_surface, &surface_caps)) < 0)
    {
        WARN("Failed to get surface capabilities, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    image_count = swapchain->backend_desc.BufferCount;
    image_count = max(image_count, surface_caps.minImageCount);
    if (surface_caps.maxImageCount)
        image_count = min(image_count, surface_caps.maxImageCount);

    if (image_count != swapchain->backend_desc.BufferCount)
    {
        WARN("Buffer count %u is not supported (%u-%u).\n", swapchain->backend_desc.BufferCount,
                surface_caps.minImageCount, surface_caps.maxImageCount);
    }

    width = swapchain->backend_desc.Width;
    height = swapchain->backend_desc.Height;
    width = max(width, surface_caps.minImageExtent.width);
    width = min(width, surface_caps.maxImageExtent.width);
    height = max(height, surface_caps.minImageExtent.height);
    height = min(height, surface_caps.maxImageExtent.height);

    if (width != swapchain->backend_desc.Width || height != swapchain->backend_desc.Height)
    {
        WARN("Swapchain dimensions %ux%u are not supported (%u-%u x %u-%u).\n",
                swapchain->backend_desc.Width, swapchain->backend_desc.Height,
                surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width,
                surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height);
    }

    TRACE("Vulkan swapchain extent %ux%u.\n", width, height);

    if (!(surface_caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
    {
        FIXME("Unsupported alpha mode.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    usage |= surface_caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    usage |= surface_caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        WARN("Transfer not supported for swapchain images.\n");
    if (swapchain->backend_desc.BufferUsage & DXGI_USAGE_SHADER_INPUT)
    {
        usage |= surface_caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!(usage & VK_IMAGE_USAGE_SAMPLED_BIT))
            WARN("Sampling not supported for swapchain images.\n");
    }

    vk_swapchain_desc.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    vk_swapchain_desc.pNext = NULL;
    vk_swapchain_desc.flags = 0;
    vk_swapchain_desc.surface = swapchain->vk_surface;
    vk_swapchain_desc.minImageCount = image_count;
    vk_swapchain_desc.imageFormat = vk_swapchain_format;
    vk_swapchain_desc.imageColorSpace = vk_color_space;
    vk_swapchain_desc.imageExtent.width = width;
    vk_swapchain_desc.imageExtent.height = height;
    vk_swapchain_desc.imageArrayLayers = 1;
    vk_swapchain_desc.imageUsage = usage;
    vk_swapchain_desc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_swapchain_desc.queueFamilyIndexCount = 0;
    vk_swapchain_desc.pQueueFamilyIndices = NULL;
    vk_swapchain_desc.preTransform = surface_caps.currentTransform;
    vk_swapchain_desc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    vk_swapchain_desc.presentMode = swapchain->present_mode;
    vk_swapchain_desc.clipped = VK_TRUE;
    vk_swapchain_desc.oldSwapchain = swapchain->vk_swapchain;
    if ((vr = vk_funcs->p_vkCreateSwapchainKHR(vk_device, &vk_swapchain_desc, NULL, &vk_swapchain)) < 0)
    {
        WARN("Failed to create Vulkan swapchain, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    if ((vr = vk_funcs->p_vkGetSwapchainImagesKHR(vk_device,
            vk_swapchain, &image_count, NULL)) != VK_SUCCESS)
    {
        WARN("Failed to get Vulkan swapchain images, vr %d.\n", vr);
        hr = hresult_from_vk_result(vr);
        goto fail;
    }
    if (image_count > ARRAY_SIZE(swapchain->vk_swapchain_images))
    {
        FIXME("Unsupported Vulkan swapchain image count %u.\n", image_count);
        hr = E_FAIL;
        goto fail;
    }
    if ((vr = vk_funcs->p_vkGetSwapchainImagesKHR(vk_device, vk_swapchain,
            &image_count, vk_swapchain_images)) != VK_SUCCESS)
    {
        WARN("Failed to get Vulkan swapchain images, vr %d.\n", vr);
        hr = hresult_from_vk_result(vr);
        goto fail;
    }

    if (swapchain->vk_swapchain)
        vk_funcs->p_vkDestroySwapchainKHR(vk_device, swapchain->vk_swapchain, NULL);
    swapchain->vk_swapchain = vk_swapchain;
    swapchain->buffer_count = image_count;
    memset(swapchain->vk_swapchain_images, 0, sizeof(swapchain->vk_swapchain_images));
    memcpy(swapchain->vk_swapchain_images, vk_swapchain_images,
            image_count * sizeof(*vk_swapchain_images));
    swapchain->vk_swapchain_width = width;
    swapchain->vk_swapchain_height = height;

    swapchain->vk_image_index = INVALID_VK_IMAGE_INDEX;

    return S_OK;

fail:
    vk_funcs->p_vkDestroySwapchainKHR(vk_device, vk_swapchain, NULL);
    return hr;
}

static HRESULT d3d12_swapchain_create_vulkan_resources(struct d3d12_swapchain *swapchain)
{
    HRESULT hr;

    if (FAILED(hr = d3d12_swapchain_create_vulkan_swapchain(swapchain)))
        return hr;

    return d3d12_swapchain_create_command_buffers(swapchain);
}

static HRESULT d3d12_swapchain_create_d3d12_resources(struct d3d12_swapchain *swapchain)
{
    HRESULT hr;

    if (!(swapchain->vk_format = vkd3d_get_vk_format(swapchain->desc.Format)))
    {
        WARN("Invalid format %#x.\n", swapchain->desc.Format);
        return DXGI_ERROR_INVALID_CALL;
    }

    if (FAILED(hr = d3d12_swapchain_create_user_buffers(swapchain)))
        return hr;

    return d3d12_swapchain_create_image_resources(swapchain);
}

static inline struct d3d12_swapchain *d3d12_swapchain_from_IDXGISwapChain4(IDXGISwapChain4 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_swapchain, IDXGISwapChain4_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_QueryInterface(IDXGISwapChain4 *iface, REFIID iid, void **object)
{
    TRACE("iface %p, iid %s, object %p.\n", iface, debugstr_guid(iid), object);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDXGIObject)
            || IsEqualGUID(iid, &IID_IDXGIDeviceSubObject)
            || IsEqualGUID(iid, &IID_IDXGISwapChain)
            || IsEqualGUID(iid, &IID_IDXGISwapChain1)
            || IsEqualGUID(iid, &IID_IDXGISwapChain2)
            || IsEqualGUID(iid, &IID_IDXGISwapChain3)
            || IsEqualGUID(iid, &IID_IDXGISwapChain4))
    {
        IUnknown_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_swapchain_AddRef(IDXGISwapChain4 *iface)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    ULONG refcount = InterlockedIncrement(&swapchain->refcount);

    TRACE("%p increasing refcount to %lu.\n", swapchain, refcount);

    return refcount;
}

static void d3d12_swapchain_destroy(struct d3d12_swapchain *swapchain)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    void *vulkan_module = vk_funcs->vulkan_module;
    struct d3d12_swapchain_op *op, *op2;
    DWORD ret;

    EnterCriticalSection(&swapchain->worker_cs);
    swapchain->worker_running = false;
    WakeAllConditionVariable(&swapchain->worker_cv);
    LeaveCriticalSection(&swapchain->worker_cs);

    if (swapchain->worker_stop_event && !SetEvent(swapchain->worker_stop_event))
        ERR("Failed to signal worker stop event, last error %ld.\n", GetLastError());

    if (swapchain->worker_thread)
    {
        if ((ret = WaitForSingleObject(swapchain->worker_thread, INFINITE)) != WAIT_OBJECT_0)
            ERR("Failed to wait for worker thread, return value %ld.\n", ret);

        if (!CloseHandle(swapchain->worker_thread))
            ERR("Failed to close worker thread, last error %ld.\n", GetLastError());
    }

    DeleteCriticalSection(&swapchain->worker_cs);

    d3d12_swapchain_destroy_vulkan_resources(swapchain);

    LIST_FOR_EACH_ENTRY_SAFE(op, op2, &swapchain->worker_ops, struct d3d12_swapchain_op, entry)
    {
        d3d12_swapchain_op_destroy(swapchain, op);
    }

    d3d12_swapchain_destroy_d3d12_resources(swapchain);

    if (swapchain->present_fence)
        ID3D12Fence_Release(swapchain->present_fence);

    if (swapchain->worker_stop_event)
        CloseHandle(swapchain->worker_stop_event);

    if (swapchain->frame_latency_semaphore)
        CloseHandle(swapchain->frame_latency_semaphore);

    if (swapchain->command_queue)
        ID3D12CommandQueue_Release(swapchain->command_queue);

    wined3d_private_store_cleanup(&swapchain->private_store);

    if (swapchain->vk_device)
    {
        if (swapchain->vk_fence)
            vk_funcs->p_vkDestroyFence(swapchain->vk_device, swapchain->vk_fence, NULL);
        if (swapchain->vk_swapchain)
            vk_funcs->p_vkDestroySwapchainKHR(swapchain->vk_device, swapchain->vk_swapchain, NULL);
    }

    if (swapchain->vk_instance && swapchain->vk_surface)
        vk_funcs->p_vkDestroySurfaceKHR(swapchain->vk_instance, swapchain->vk_surface, NULL);

    if (swapchain->target)
    {
        WARN("Destroying fullscreen swapchain.\n");
        IDXGIOutput_Release(swapchain->target);
    }

    if (swapchain->restrict_to_output)
        IDXGIOutput_Release(swapchain->restrict_to_output);

    if (swapchain->device)
        ID3D12Device_Release(swapchain->device);

    if (swapchain->factory)
        IWineDXGIFactory_Release(swapchain->factory);

    FreeLibrary(vulkan_module);

    wined3d_swapchain_state_destroy(swapchain->state);
}

static ULONG STDMETHODCALLTYPE d3d12_swapchain_Release(IDXGISwapChain4 *iface)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    ULONG refcount = InterlockedDecrement(&swapchain->refcount);

    TRACE("%p decreasing refcount to %lu.\n", swapchain, refcount);

    if (!refcount)
    {
        d3d12_swapchain_destroy(swapchain);
        free(swapchain);
    }

    return refcount;
}

/* IDXGIObject methods */

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetPrivateData(IDXGISwapChain4 *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_set_private_data(&swapchain->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetPrivateDataInterface(IDXGISwapChain4 *iface,
        REFGUID guid, const IUnknown *object)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, object %p.\n", iface, debugstr_guid(guid), object);

    return dxgi_set_private_data_interface(&swapchain->private_store, guid, object);
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetPrivateData(IDXGISwapChain4 *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_get_private_data(&swapchain->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetParent(IDXGISwapChain4 *iface, REFIID iid, void **parent)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, iid %s, parent %p.\n", iface, debugstr_guid(iid), parent);

    return IWineDXGIFactory_QueryInterface(swapchain->factory, iid, parent);
}

/* IDXGIDeviceSubObject methods */

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetDevice(IDXGISwapChain4 *iface, REFIID iid, void **device)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return ID3D12Device_QueryInterface(swapchain->device, iid, device);
}

/* IDXGISwapChain methods */

static HRESULT d3d12_swapchain_set_sync_interval(struct d3d12_swapchain *swapchain,
        unsigned int sync_interval)
{
    VkPresentModeKHR present_mode;
    HRESULT hr;

    switch (sync_interval)
    {
        case 0:
            present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            break;
        default:
            FIXME("Unsupported sync interval %u.\n", sync_interval);
        case 1:
            present_mode = VK_PRESENT_MODE_FIFO_KHR;
            break;
    }

    if (swapchain->present_mode == present_mode)
        return S_OK;

    if (!d3d12_swapchain_is_present_mode_supported(swapchain, present_mode))
    {
        FIXME("Vulkan present mode %#x is not supported.\n", present_mode);
        return S_OK;
    }

    if (FAILED(hr = d3d12_swapchain_destroy_vulkan_resources(swapchain)))
        return hr;
    swapchain->present_mode = present_mode;
    if (SUCCEEDED(hr = d3d12_swapchain_create_vulkan_resources(swapchain)))
        d3d12_swapchain_apply_vk_hdr_metadata(swapchain);
    return hr;
}

static VkResult d3d12_swapchain_queue_present(struct d3d12_swapchain *swapchain, VkImage vk_src_image,
        uint64_t frame_number)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    VkPresentInfoKHR present_info;
    VkCommandBuffer vk_cmd_buffer;
    VkSubmitInfo submit_info;
    VkImage vk_dst_image;
    uint32_t image_index;
    VkQueue vk_queue;
    VkResult vr;
    uint64_t completed_value;
    DWORD ret;

    if (swapchain->vk_image_index == INVALID_VK_IMAGE_INDEX)
    {
        if ((vr = d3d12_swapchain_acquire_next_vulkan_image(swapchain)) < 0)
            return vr;
    }

    assert(swapchain->vk_image_index < swapchain->buffer_count);
    image_index = swapchain->vk_image_index;

    vk_cmd_buffer = swapchain->vk_cmd_buffers[image_index];
    vk_dst_image = swapchain->vk_swapchain_images[image_index];

    if ((vr = vk_funcs->p_vkResetCommandBuffer(vk_cmd_buffer, 0)) < 0)
    {
        ERR("Failed to reset command buffer, vr %d.\n", vr);
        return vr;
    }

    if ((vr = d3d12_swapchain_record_swapchain_blit(swapchain,
            vk_cmd_buffer, vk_dst_image, vk_src_image)) < 0 )
        return vr;

    for (;;)
    {
        completed_value = ID3D12Fence_GetCompletedValue(swapchain->present_fence);
        if (completed_value >= frame_number + 1)
            break;
        if (completed_value == UINT64_MAX)
            return VK_ERROR_DEVICE_LOST;

        if ((ret = WaitForSingleObject(swapchain->worker_stop_event, 1)) == WAIT_OBJECT_0)
        {
            TRACE("Present fence wait cancelled.\n");
            return VK_ERROR_DEVICE_LOST;
        }
        if (ret != WAIT_TIMEOUT)
        {
            ERR("Failed to wait for present fence, return value %ld.\n", ret);
            return VK_ERROR_DEVICE_LOST;
        }
    }

    vk_queue = vkd3d_acquire_vk_queue(swapchain->command_queue);

    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = NULL;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = NULL;
    submit_info.pWaitDstStageMask = NULL;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &vk_cmd_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &swapchain->vk_semaphores[image_index];

    if ((vr = vk_funcs->p_vkQueueSubmit(vk_queue, 1, &submit_info, VK_NULL_HANDLE)) < 0)
    {
        ERR("Failed to blit swapchain buffer, vr %d.\n", vr);
        vkd3d_release_vk_queue(swapchain->command_queue);
        return vr;
    }

    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pNext = NULL;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &swapchain->vk_semaphores[image_index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain->vk_swapchain;
    present_info.pImageIndices = &image_index;
    present_info.pResults = NULL;

    vr = vk_funcs->p_vkQueuePresentKHR(vk_queue, &present_info);
    if (vr >= 0)
        swapchain->vk_image_index = INVALID_VK_IMAGE_INDEX;

    vkd3d_release_vk_queue(swapchain->command_queue);

    return vr;
}

static HRESULT d3d12_swapchain_op_present_execute(struct d3d12_swapchain *swapchain, struct d3d12_swapchain_op *op)
{
    VkResult vr;
    HRESULT hr;

    if (FAILED(hr = d3d12_swapchain_set_sync_interval(swapchain, op->present.sync_interval)))
        return hr;

    vr = d3d12_swapchain_queue_present(swapchain, op->present.vk_image, op->present.frame_number);
    if (vr == VK_ERROR_OUT_OF_DATE_KHR)
    {
        TRACE("Recreating Vulkan swapchain.\n");

        if (FAILED(hr = d3d12_swapchain_destroy_vulkan_resources(swapchain)))
            return hr;
        if (FAILED(hr = d3d12_swapchain_create_vulkan_resources(swapchain)))
            return hr;
        d3d12_swapchain_apply_vk_hdr_metadata(swapchain);

        if ((vr = d3d12_swapchain_queue_present(swapchain, op->present.vk_image, op->present.frame_number)) < 0)
            ERR("Failed to present after recreating swapchain, vr %d.\n", vr);
    }

    if (vr < 0)
    {
        ERR("Failed to queue present, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    return S_OK;
}

#define D3D12_SWAPCHAIN_MAX_PENDING_PRESENTS \
    (DXGI_FRAME_LATENCY_MAX * DXGI_MAX_SWAP_CHAIN_BUFFERS)

static HRESULT d3d12_swapchain_reserve_present_slot(struct d3d12_swapchain *swapchain, unsigned int flags)
{
    HRESULT hr;
    DWORD error;

    EnterCriticalSection(&swapchain->worker_cs);
    for (;;)
    {
        if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
            break;
        if (!swapchain->worker_running)
        {
            hr = DXGI_ERROR_DEVICE_REMOVED;
            break;
        }
        if (swapchain->color_reconfigure_in_progress)
        {
            hr = DXGI_ERROR_WAS_STILL_DRAWING;
            break;
        }
        if (!swapchain->color_reconfigure_in_progress
                && swapchain->pending_present_count < D3D12_SWAPCHAIN_MAX_PENDING_PRESENTS)
        {
            ++swapchain->pending_present_count;
            hr = S_OK;
            break;
        }
        if (flags & DXGI_PRESENT_DO_NOT_WAIT)
        {
            hr = DXGI_ERROR_WAS_STILL_DRAWING;
            break;
        }
        if (!SleepConditionVariableCS(&swapchain->worker_cv, &swapchain->worker_cs, 100)
                && (error = GetLastError()) != ERROR_TIMEOUT)
        {
            hr = HRESULT_FROM_WIN32(error);
            d3d12_swapchain_record_async_error(swapchain, hr);
            break;
        }
    }
    LeaveCriticalSection(&swapchain->worker_cs);

    return hr;
}

static void d3d12_swapchain_release_present_slot(struct d3d12_swapchain *swapchain)
{
    EnterCriticalSection(&swapchain->worker_cs);
    assert(swapchain->pending_present_count);
    --swapchain->pending_present_count;
    WakeAllConditionVariable(&swapchain->worker_cv);
    LeaveCriticalSection(&swapchain->worker_cs);
}

static void d3d12_swapchain_restore_frame_latency_token(struct d3d12_swapchain *swapchain,
        BOOL frame_latency_token)
{
    HRESULT hr;

    if (!frame_latency_token)
        return;

    if (!ReleaseSemaphore(swapchain->frame_latency_semaphore, 1, NULL))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        ERR("Failed to restore frame latency semaphore, hr %#lx.\n", hr);
        d3d12_swapchain_record_async_error(swapchain, hr);
    }
}

static HRESULT d3d12_swapchain_reserve_resize_slot(struct d3d12_swapchain *swapchain)
{
    HRESULT hr;
    DWORD error;

    EnterCriticalSection(&swapchain->worker_cs);
    for (;;)
    {
        if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
            break;
        if (!swapchain->worker_running)
        {
            hr = DXGI_ERROR_DEVICE_REMOVED;
            break;
        }
        if (swapchain->pending_resize_count || swapchain->color_reconfigure_in_progress)
        {
            hr = DXGI_ERROR_INVALID_CALL;
            break;
        }
        if (!swapchain->pending_resize_count && !swapchain->color_reconfigure_in_progress)
        {
            ++swapchain->pending_resize_count;
            swapchain->frontend_resize_in_progress = true;
            hr = S_OK;
            break;
        }
        if (!SleepConditionVariableCS(&swapchain->worker_cv, &swapchain->worker_cs, 100)
                && (error = GetLastError()) != ERROR_TIMEOUT)
        {
            hr = HRESULT_FROM_WIN32(error);
            d3d12_swapchain_record_async_error(swapchain, hr);
            break;
        }
    }
    LeaveCriticalSection(&swapchain->worker_cs);

    return hr;
}

static void d3d12_swapchain_release_resize_slot(struct d3d12_swapchain *swapchain)
{
    EnterCriticalSection(&swapchain->worker_cs);
    assert(swapchain->pending_resize_count);
    assert(swapchain->frontend_resize_in_progress);
    swapchain->frontend_resize_in_progress = false;
    --swapchain->pending_resize_count;
    WakeAllConditionVariable(&swapchain->worker_cv);
    LeaveCriticalSection(&swapchain->worker_cs);
}

static HRESULT d3d12_swapchain_begin_color_reconfigure(struct d3d12_swapchain *swapchain)
{
    HRESULT hr;
    DWORD error;

    EnterCriticalSection(&swapchain->worker_cs);
    for (;;)
    {
        if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
            break;
        if (!swapchain->worker_running)
        {
            hr = DXGI_ERROR_DEVICE_REMOVED;
            break;
        }
        if (swapchain->color_reconfigure_in_progress || swapchain->pending_resize_count)
        {
            hr = DXGI_ERROR_INVALID_CALL;
            break;
        }
        if (!swapchain->color_reconfigure_in_progress && !swapchain->pending_resize_count)
        {
            swapchain->color_reconfigure_in_progress = true;
            while (swapchain->pending_present_count && swapchain->worker_running
                    && SUCCEEDED(InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
            {
                if (!SleepConditionVariableCS(&swapchain->worker_cv, &swapchain->worker_cs, 100)
                        && (error = GetLastError()) != ERROR_TIMEOUT)
                {
                    hr = HRESULT_FROM_WIN32(error);
                    d3d12_swapchain_record_async_error(swapchain, hr);
                    goto fail;
                }
            }
            hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK);
            if (SUCCEEDED(hr) && !swapchain->worker_running) hr = DXGI_ERROR_DEVICE_REMOVED;
            if (SUCCEEDED(hr)) break;
fail:
            swapchain->color_reconfigure_in_progress = false;
            WakeAllConditionVariable(&swapchain->worker_cv);
            break;
        }
        if (!SleepConditionVariableCS(&swapchain->worker_cv, &swapchain->worker_cs, 100)
                && (error = GetLastError()) != ERROR_TIMEOUT)
        {
            hr = HRESULT_FROM_WIN32(error);
            d3d12_swapchain_record_async_error(swapchain, hr);
            break;
        }
    }
    LeaveCriticalSection(&swapchain->worker_cs);
    return hr;
}

static void d3d12_swapchain_end_color_reconfigure(struct d3d12_swapchain *swapchain)
{
    EnterCriticalSection(&swapchain->worker_cs);
    assert(swapchain->color_reconfigure_in_progress);
    swapchain->color_reconfigure_in_progress = false;
    WakeAllConditionVariable(&swapchain->worker_cv);
    LeaveCriticalSection(&swapchain->worker_cs);
}

static HRESULT d3d12_swapchain_present(struct d3d12_swapchain *swapchain,
        unsigned int sync_interval, unsigned int flags)
{
    static const unsigned int supported_flags = DXGI_PRESENT_TEST
            | DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_ALLOW_TEARING
            | DXGI_PRESENT_RESTRICT_TO_OUTPUT;
    struct d3d12_swapchain_op *op;
    BOOL frame_latency_token = FALSE;
    BOOL windowed;
    HRESULT hr;
    DWORD ret;

    if (sync_interval > 4)
    {
        WARN("Invalid sync interval %u.\n", sync_interval);
        return DXGI_ERROR_INVALID_CALL;
    }

    if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
        return hr;

    if (flags & ~supported_flags)
    {
        WARN("Invalid or unsupported present flags %#x.\n", flags & ~supported_flags);
        return DXGI_ERROR_INVALID_CALL;
    }

    if ((flags & DXGI_PRESENT_RESTRICT_TO_OUTPUT) && !swapchain->restrict_to_output)
        return DXGI_ERROR_INVALID_CALL;

    if (flags & DXGI_PRESENT_ALLOW_TEARING)
    {
        wined3d_mutex_lock();
        windowed = wined3d_swapchain_state_is_windowed(swapchain->state);
        wined3d_mutex_unlock();
        if (sync_interval || !windowed
                || !(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
            return DXGI_ERROR_INVALID_CALL;
    }

    if (flags & DXGI_PRESENT_TEST)
    {
        TRACE("Testing presentation support.\n");
        return S_OK;
    }

    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
    {
        do
        {
            ret = WaitForSingleObject(swapchain->frame_latency_semaphore,
                    flags & DXGI_PRESENT_DO_NOT_WAIT ? 0 : 100);
            if (ret == WAIT_TIMEOUT && (flags & DXGI_PRESENT_DO_NOT_WAIT))
                return DXGI_ERROR_WAS_STILL_DRAWING;
            if (ret != WAIT_TIMEOUT && ret != WAIT_OBJECT_0)
            {
                ERR("Failed to wait for frame latency semaphore, last error %ld.\n", GetLastError());
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (ret == WAIT_OBJECT_0)
                frame_latency_token = TRUE;
            if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
            {
                d3d12_swapchain_restore_frame_latency_token(swapchain, frame_latency_token);
                return hr;
            }
        } while (ret == WAIT_TIMEOUT);
    }

    if (FAILED(hr = d3d12_swapchain_reserve_present_slot(swapchain, flags)))
    {
        d3d12_swapchain_restore_frame_latency_token(swapchain, frame_latency_token);
        return hr;
    }

    if (!(op = calloc(1, sizeof(*op))))
    {
        WARN("Cannot allocate memory.\n");
        d3d12_swapchain_release_present_slot(swapchain);
        d3d12_swapchain_restore_frame_latency_token(swapchain, frame_latency_token);
        return E_OUTOFMEMORY;
    }

    op->type = D3D12_SWAPCHAIN_OP_PRESENT;
    op->present.sync_interval = sync_interval;

    EnterCriticalSection(&swapchain->worker_cs);
    if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
    {
        /* The worker failure is returned below. */
    }
    else if (!swapchain->worker_running)
    {
        hr = DXGI_ERROR_DEVICE_REMOVED;
    }
    else if (swapchain->frame_number == UINT64_MAX)
    {
        hr = DXGI_ERROR_DEVICE_REMOVED;
        d3d12_swapchain_record_async_error(swapchain, hr);
    }
    else
    {
        op->present.vk_image = swapchain->vk_images[swapchain->current_buffer_index];
        op->present.frame_number = swapchain->frame_number;

        if (FAILED(hr = vkd3d_queue_signal_on_cpu(swapchain->command_queue,
                swapchain->present_fence, swapchain->frame_number + 1)))
        {
            ERR("Failed to signal present fence, hr %#lx.\n", hr);
        }
        else
        {
            list_add_tail(&swapchain->worker_ops, &op->entry);
            ++swapchain->frame_number;
            swapchain->current_buffer_index = (swapchain->current_buffer_index + 1)
                    % swapchain->desc.BufferCount;
            WakeAllConditionVariable(&swapchain->worker_cv);
        }
    }

    if (FAILED(hr))
    {
        assert(swapchain->pending_present_count);
        --swapchain->pending_present_count;
        WakeAllConditionVariable(&swapchain->worker_cv);
    }
    LeaveCriticalSection(&swapchain->worker_cs);

    if (FAILED(hr))
    {
        free(op);
        d3d12_swapchain_restore_frame_latency_token(swapchain, frame_latency_token);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_Present(IDXGISwapChain4 *iface, UINT sync_interval, UINT flags)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, sync_interval %u, flags %#x.\n", iface, sync_interval, flags);

    return d3d12_swapchain_present(swapchain, sync_interval, flags);
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetBuffer(IDXGISwapChain4 *iface,
        UINT buffer_idx, REFIID iid, void **surface)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    DWORD error;
    HRESULT hr;

    TRACE("iface %p, buffer_idx %u, iid %s, surface %p.\n",
            iface, buffer_idx, debugstr_guid(iid), surface);

    EnterCriticalSection(&swapchain->worker_cs);
    while (swapchain->frontend_resize_in_progress)
    {
        if (!SleepConditionVariableCS(&swapchain->worker_cv, &swapchain->worker_cs, 100)
                && (error = GetLastError()) != ERROR_TIMEOUT)
        {
            LeaveCriticalSection(&swapchain->worker_cs);
            return HRESULT_FROM_WIN32(error);
        }
    }

    if (buffer_idx >= swapchain->desc.BufferCount)
    {
        WARN("Invalid buffer index %u.\n", buffer_idx);
        LeaveCriticalSection(&swapchain->worker_cs);
        return DXGI_ERROR_INVALID_CALL;
    }

    assert(swapchain->buffers[buffer_idx]);
    hr = ID3D12Resource_QueryInterface(swapchain->buffers[buffer_idx], iid, surface);
    LeaveCriticalSection(&swapchain->worker_cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE DECLSPEC_HOTPATCH d3d12_swapchain_SetFullscreenState(IDXGISwapChain4 *iface,
        BOOL fullscreen, IDXGIOutput *target)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
    struct wined3d_swapchain_desc wined3d_desc;
    HWND window = swapchain->window;
    LONG in_set_fullscreen_state;
    BOOL old_fs;
    HRESULT hr;

    TRACE("iface %p, fullscreen %#x, target %p.\n", iface, fullscreen, target);

    if (!fullscreen && target)
    {
        WARN("Invalid call.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (target)
    {
        IDXGIOutput_AddRef(target);
    }
    else if (FAILED(hr = IDXGISwapChain4_GetContainingOutput(iface, &target)))
    {
        WARN("Failed to get target output for swapchain, hr %#lx.\n", hr);
        return hr;
    }

    EnterCriticalSection(&swapchain->worker_cs);
    swapchain_desc = swapchain->desc;
    LeaveCriticalSection(&swapchain->worker_cs);

    wined3d_mutex_lock();
    fullscreen_desc = swapchain->fullscreen_desc;
    wined3d_mutex_unlock();

    if (FAILED(hr = wined3d_swapchain_desc_from_dxgi(&wined3d_desc, target, window, &swapchain_desc,
            &fullscreen_desc)))
    {
        IDXGIOutput_Release(target);
        return hr;
    }

    in_set_fullscreen_state = InterlockedExchange(&swapchain->in_set_fullscreen_state, 1);
    if (in_set_fullscreen_state)
    {
        WARN("Nested invocation of SetFullscreenState.\n");
        IDXGIOutput_Release(target);
        IDXGISwapChain4_GetFullscreenState(iface, &old_fs, NULL);
        return old_fs == fullscreen ? S_OK : DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;
    }

    wined3d_mutex_lock();
    wined3d_desc.windowed = !fullscreen;
    hr = wined3d_swapchain_state_set_fullscreen(swapchain->state, &wined3d_desc, NULL);
    if (FAILED(hr))
    {
        IDXGIOutput_Release(target);
        hr = DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        goto done;
    }

    swapchain->fullscreen_desc.Windowed = wined3d_desc.windowed;
    if (!fullscreen)
    {
        IDXGIOutput_Release(target);
        target = NULL;
    }

    if (swapchain->target)
        IDXGIOutput_Release(swapchain->target);
    swapchain->target = target;

done:
    wined3d_mutex_unlock();
    InterlockedExchange(&swapchain->in_set_fullscreen_state, 0);
    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetFullscreenState(IDXGISwapChain4 *iface,
        BOOL *fullscreen, IDXGIOutput **target)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    BOOL windowed;
    HRESULT hr;

    TRACE("iface %p, fullscreen %p, target %p.\n", iface, fullscreen, target);

    if (!fullscreen && !target)
        return S_OK;

    if (target)
        *target = NULL;

    wined3d_mutex_lock();
    windowed = wined3d_swapchain_state_is_windowed(swapchain->state);
    if (target && !windowed && swapchain->target)
    {
        *target = swapchain->target;
        IDXGIOutput_AddRef(*target);
    }
    wined3d_mutex_unlock();

    if (fullscreen)
        *fullscreen = !windowed;

    if (target && !windowed && !*target
            && FAILED(hr = IDXGISwapChain4_GetContainingOutput(iface, target)))
        return hr;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetDesc(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_DESC *desc)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
    BOOL windowed;

    TRACE("iface %p, desc %p.\n", iface, desc);

    if (!desc)
    {
        WARN("Invalid pointer.\n");
        return E_INVALIDARG;
    }

    wined3d_mutex_lock();
    windowed = wined3d_swapchain_state_is_windowed(swapchain->state);
    fullscreen_desc = swapchain->fullscreen_desc;
    wined3d_mutex_unlock();

    EnterCriticalSection(&swapchain->worker_cs);
    swapchain_desc = swapchain->desc;
    LeaveCriticalSection(&swapchain->worker_cs);

    desc->BufferDesc.Width = swapchain_desc.Width;
    desc->BufferDesc.Height = swapchain_desc.Height;
    desc->BufferDesc.RefreshRate = fullscreen_desc.RefreshRate;
    desc->BufferDesc.Format = swapchain_desc.Format;
    desc->BufferDesc.ScanlineOrdering = fullscreen_desc.ScanlineOrdering;
    desc->BufferDesc.Scaling = fullscreen_desc.Scaling;
    desc->SampleDesc = swapchain_desc.SampleDesc;
    desc->BufferUsage = swapchain_desc.BufferUsage;
    desc->BufferCount = swapchain_desc.BufferCount;
    desc->OutputWindow = swapchain->window;
    desc->Windowed = windowed;
    desc->SwapEffect = swapchain_desc.SwapEffect;
    desc->Flags = swapchain_desc.Flags;

    return S_OK;
}

static HRESULT d3d12_swapchain_op_resize_buffers_execute(struct d3d12_swapchain *swapchain, struct d3d12_swapchain_op *op)
{
    HRESULT hr;

    if (FAILED(hr = d3d12_swapchain_destroy_vulkan_resources(swapchain)))
        return hr;

    swapchain->backend_desc = op->resize_buffers.desc;
    if (SUCCEEDED(hr = d3d12_swapchain_create_vulkan_resources(swapchain)))
        d3d12_swapchain_apply_vk_hdr_metadata(swapchain);
    return hr;
}

static HRESULT d3d12_swapchain_resize_buffers(struct d3d12_swapchain *swapchain,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    static const UINT supported_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
            | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
            | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    DXGI_SWAP_CHAIN_DESC1 *desc, new_desc;
    struct d3d12_swapchain new_resources = {0};
    struct d3d12_swapchain_op *op;
    VkColorSpaceKHR vk_color_space;
    VkFormat vk_swapchain_format;
    unsigned int i;
    ULONG refcount;
    HRESULT hr;

    if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
        return hr;

    if (flags & ~supported_flags)
    {
        WARN("Invalid flags %#x.\n", flags & ~supported_flags);
        return E_INVALIDARG;
    }

    if ((swapchain->desc.Flags ^ flags) & (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
            | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
        return E_INVALIDARG;

    /* Serialize the complete front-end resize transaction, including validation
     * of the old buffers. Otherwise concurrent ResizeBuffers() calls can inspect
     * resources while another call is replacing them. */
    if (FAILED(hr = d3d12_swapchain_reserve_resize_slot(swapchain)))
        return hr;

    for (i = 0; i < swapchain->desc.BufferCount; ++i)
    {
        ID3D12Resource_AddRef(swapchain->buffers[i]);
        if ((refcount = ID3D12Resource_Release(swapchain->buffers[i])))
        {
            WARN("Buffer %p has %lu references left.\n", swapchain->buffers[i], refcount);
            hr = DXGI_ERROR_INVALID_CALL;
            goto fail;
        }
    }

    desc = &swapchain->desc;
    new_desc = swapchain->desc;
    new_desc.Flags = flags;

    if (buffer_count)
        new_desc.BufferCount = buffer_count;
    if (!width || !height)
    {
        RECT client_rect;

        if (!GetClientRect(swapchain->window, &client_rect))
        {
            WARN("Failed to get client rect, last error %#lx.\n", GetLastError());
            hr = DXGI_ERROR_INVALID_CALL;
            goto fail;
        }

        if (client_rect.right <= client_rect.left || client_rect.bottom <= client_rect.top)
        {
            WARN("Invalid client rect %s.\n", wine_dbgstr_rect(&client_rect));
            hr = DXGI_ERROR_INVALID_CALL;
            goto fail;
        }
        if (!width)
            width = client_rect.right - client_rect.left;
        if (!height)
            height = client_rect.bottom - client_rect.top;
    }
    new_desc.Width = width;
    new_desc.Height = height;

    if (format)
        new_desc.Format = format;

    if (!dxgi_validate_swapchain_desc(&new_desc))
    {
        hr = DXGI_ERROR_INVALID_CALL;
        goto fail;
    }
    if (FAILED(hr = select_vk_format(&swapchain->vk_funcs, swapchain->vk_physical_device,
            swapchain->vk_surface, &new_desc, swapchain->color_space,
            &vk_swapchain_format, &vk_color_space)))
    {
        WARN("Current colour space %#x is not supported with resized format %s.\n",
                swapchain->color_space, debug_dxgi_format(new_desc.Format));
        goto fail;
    }

    if (desc->Width == new_desc.Width && desc->Height == new_desc.Height
            && desc->Format == new_desc.Format && desc->BufferCount == new_desc.BufferCount)
    {
        EnterCriticalSection(&swapchain->worker_cs);
        if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK)))
        {
            /* Return the worker failure after releasing the reserved slot. */
        }
        else if (!swapchain->worker_running)
        {
            hr = DXGI_ERROR_DEVICE_REMOVED;
        }
        else
        {
            swapchain->current_buffer_index = 0;
            /* DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE is not applicable on D3D12. So there is no need to
             * recreate buffers when the flag gets toggled. */
            swapchain->desc.Flags = flags;
            hr = S_OK;
        }
        LeaveCriticalSection(&swapchain->worker_cs);
        d3d12_swapchain_release_resize_slot(swapchain);
        return hr;
    }

    if (!(op = calloc(1, sizeof(*op))))
    {
        WARN("Cannot allocate memory.\n");
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    new_resources.vk_funcs = swapchain->vk_funcs;
    new_resources.vk_physical_device = swapchain->vk_physical_device;
    new_resources.vk_device = swapchain->vk_device;
    new_resources.device = swapchain->device;
    new_resources.desc = new_desc;
    if (FAILED(hr = d3d12_swapchain_create_d3d12_resources(&new_resources)))
    {
        d3d12_swapchain_destroy_d3d12_resources(&new_resources);
        d3d12_swapchain_release_resize_slot(swapchain);
        free(op);
        return hr;
    }

    op->type = D3D12_SWAPCHAIN_OP_RESIZE_BUFFERS;
    op->resize_buffers.desc = new_desc;

    EnterCriticalSection(&swapchain->worker_cs);
    if (FAILED(hr = InterlockedCompareExchange(&swapchain->async_error, S_OK, S_OK))
            || !swapchain->worker_running)
    {
        assert(swapchain->pending_resize_count);
        assert(swapchain->frontend_resize_in_progress);
        swapchain->frontend_resize_in_progress = false;
        --swapchain->pending_resize_count;
        WakeAllConditionVariable(&swapchain->worker_cv);
        LeaveCriticalSection(&swapchain->worker_cs);
        d3d12_swapchain_destroy_d3d12_resources(&new_resources);
        free(op);
        return FAILED(hr) ? hr : DXGI_ERROR_DEVICE_REMOVED;
    }
    op->resize_buffers.vk_memory = swapchain->vk_memory;
    swapchain->vk_memory = VK_NULL_HANDLE;
    memcpy(&op->resize_buffers.vk_images, &swapchain->vk_images, sizeof(swapchain->vk_images));
    memset(&swapchain->vk_images, 0, sizeof(swapchain->vk_images));

    swapchain->current_buffer_index = 0;

    d3d12_swapchain_destroy_d3d12_resources(swapchain);

    swapchain->desc = new_desc;
    swapchain->vk_format = new_resources.vk_format;
    swapchain->vk_memory = new_resources.vk_memory;
    new_resources.vk_memory = VK_NULL_HANDLE;
    memcpy(swapchain->vk_images, new_resources.vk_images, sizeof(swapchain->vk_images));
    memset(new_resources.vk_images, 0, sizeof(new_resources.vk_images));
    memcpy(swapchain->buffers, new_resources.buffers, sizeof(swapchain->buffers));
    memset(new_resources.buffers, 0, sizeof(new_resources.buffers));

    /* Publish the resize only after the front-end resources have been replaced.
     * A later Present() will then select a new image and be queued after this op. */
    assert(swapchain->frontend_resize_in_progress);
    swapchain->frontend_resize_in_progress = false;
    list_add_tail(&swapchain->worker_ops, &op->entry);
    WakeAllConditionVariable(&swapchain->worker_cv);
    LeaveCriticalSection(&swapchain->worker_cs);

    return S_OK;

fail:
    d3d12_swapchain_release_resize_slot(swapchain);
    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_ResizeBuffers(IDXGISwapChain4 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, buffer_count %u, width %u, height %u, format %s, flags %#x.\n",
            iface, buffer_count, width, height, debug_dxgi_format(format), flags);

    return d3d12_swapchain_resize_buffers(swapchain, buffer_count, width, height, format, flags);
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_ResizeTarget(IDXGISwapChain4 *iface,
        const DXGI_MODE_DESC *target_mode_desc)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, target_mode_desc %p.\n", iface, target_mode_desc);

    return dxgi_swapchain_resize_target(swapchain->state, target_mode_desc);
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetContainingOutput(IDXGISwapChain4 *iface,
        IDXGIOutput **output)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    IUnknown *device_parent;
    IWineDXGIFactory *factory;
    IDXGIAdapter *adapter;
    HRESULT hr;

    TRACE("iface %p, output %p.\n", iface, output);

    if (!output)
        return DXGI_ERROR_INVALID_CALL;

    wined3d_mutex_lock();
    if (swapchain->target)
    {
        IDXGIOutput_AddRef(*output = swapchain->target);
        wined3d_mutex_unlock();
        return S_OK;
    }
    wined3d_mutex_unlock();

    device_parent = vkd3d_get_device_parent(swapchain->device);

    if (FAILED(hr = IUnknown_QueryInterface(device_parent, &IID_IDXGIAdapter, (void **)&adapter)))
    {
        WARN("Failed to get adapter, hr %#lx.\n", hr);
        return hr;
    }

    if (FAILED(hr = IDXGIAdapter_GetParent(adapter, &IID_IWineDXGIFactory, (void **)&factory)))
    {
        WARN("Failed to get factory, hr %#lx.\n", hr);
        IDXGIAdapter_Release(adapter);
        return hr;
    }

    hr = dxgi_get_output_from_window(factory, swapchain->window, output);
    IWineDXGIFactory_Release(factory);
    IDXGIAdapter_Release(adapter);
    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetFrameStatistics(IDXGISwapChain4 *iface,
        DXGI_FRAME_STATISTICS *stats)
{
    FIXME("iface %p, stats %p stub!\n", iface, stats);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetLastPresentCount(IDXGISwapChain4 *iface,
        UINT *last_present_count)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, last_present_count %p.\n", iface, last_present_count);

    if (!last_present_count)
        return DXGI_ERROR_INVALID_CALL;

    EnterCriticalSection(&swapchain->worker_cs);
    *last_present_count = swapchain->frame_number;
    LeaveCriticalSection(&swapchain->worker_cs);

    return S_OK;
}

/* IDXGISwapChain1 methods */

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetDesc1(IDXGISwapChain4 *iface, DXGI_SWAP_CHAIN_DESC1 *desc)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    if (!desc)
    {
        WARN("Invalid pointer.\n");
        return E_INVALIDARG;
    }

    EnterCriticalSection(&swapchain->worker_cs);
    *desc = swapchain->desc;
    LeaveCriticalSection(&swapchain->worker_cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetFullscreenDesc(IDXGISwapChain4 *iface,
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    BOOL windowed;

    TRACE("iface %p, desc %p.\n", iface, desc);

    if (!desc)
    {
        WARN("Invalid pointer.\n");
        return E_INVALIDARG;
    }

    wined3d_mutex_lock();
    windowed = wined3d_swapchain_state_is_windowed(swapchain->state);
    *desc = swapchain->fullscreen_desc;
    wined3d_mutex_unlock();

    desc->Windowed = windowed;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetHwnd(IDXGISwapChain4 *iface, HWND *hwnd)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, hwnd %p.\n", iface, hwnd);

    if (!hwnd)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    *hwnd = swapchain->window;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetCoreWindow(IDXGISwapChain4 *iface,
        REFIID iid, void **core_window)
{
    FIXME("iface %p, iid %s, core_window %p stub!\n", iface, debugstr_guid(iid), core_window);

    if (core_window)
        *core_window = NULL;

    return DXGI_ERROR_INVALID_CALL;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_Present1(IDXGISwapChain4 *iface,
        UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS *present_parameters)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, sync_interval %u, flags %#x, present_parameters %p.\n",
            iface, sync_interval, flags, present_parameters);

    if (present_parameters)
        FIXME("Ignored present parameters %p.\n", present_parameters);

    return d3d12_swapchain_present(swapchain, sync_interval, flags);
}

static BOOL STDMETHODCALLTYPE d3d12_swapchain_IsTemporaryMonoSupported(IDXGISwapChain4 *iface)
{
    FIXME("iface %p stub!\n", iface);

    return FALSE;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetRestrictToOutput(IDXGISwapChain4 *iface, IDXGIOutput **output)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);

    TRACE("iface %p, output %p.\n", iface, output);

    if (!output)
    {
        WARN("Invalid pointer.\n");
        return E_INVALIDARG;
    }

    if ((*output = swapchain->restrict_to_output))
        IDXGIOutput_AddRef(*output);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetBackgroundColor(IDXGISwapChain4 *iface, const DXGI_RGBA *color)
{
    FIXME("iface %p, color %p stub!\n", iface, color);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetBackgroundColor(IDXGISwapChain4 *iface, DXGI_RGBA *color)
{
    FIXME("iface %p, color %p stub!\n", iface, color);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetRotation(IDXGISwapChain4 *iface, DXGI_MODE_ROTATION rotation)
{
    FIXME("iface %p, rotation %#x stub!\n", iface, rotation);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetRotation(IDXGISwapChain4 *iface, DXGI_MODE_ROTATION *rotation)
{
    FIXME("iface %p, rotation %p stub!\n", iface, rotation);

    return E_NOTIMPL;
}

/* IDXGISwapChain2 methods */

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetSourceSize(IDXGISwapChain4 *iface, UINT width, UINT height)
{
    FIXME("iface %p, width %u, height %u stub!\n", iface, width, height);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetSourceSize(IDXGISwapChain4 *iface, UINT *width, UINT *height)
{
    FIXME("iface %p, width %p, height %p stub!\n", iface, width, height);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetMaximumFrameLatency(IDXGISwapChain4 *iface, UINT max_latency)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    DWORD error;
    HRESULT hr = S_OK;

    TRACE("iface %p, max_latency %u.\n", iface, max_latency);

    EnterCriticalSection(&swapchain->worker_cs);
    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
    {
        WARN("DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT not set for swap chain %p.\n", iface);
        hr = DXGI_ERROR_INVALID_CALL;
    }
    else if (!max_latency || max_latency > DXGI_FRAME_LATENCY_MAX)
    {
        WARN("Invalid maximum frame latency %u.\n", max_latency);
        hr = DXGI_ERROR_INVALID_CALL;
    }
    else if (max_latency > swapchain->frame_latency)
    {
        if (!ReleaseSemaphore(swapchain->frame_latency_semaphore,
                max_latency - swapchain->frame_latency, NULL))
        {
            error = GetLastError();
            ERR("Failed to release frame latency semaphore, last error %lu.\n", error);
            hr = HRESULT_FROM_WIN32(error);
        }
    }

    if (SUCCEEDED(hr))
        swapchain->frame_latency = max_latency;
    LeaveCriticalSection(&swapchain->worker_cs);
    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetMaximumFrameLatency(IDXGISwapChain4 *iface, UINT *max_latency)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    HRESULT hr = S_OK;

    TRACE("iface %p, max_latency %p.\n", iface, max_latency);

    EnterCriticalSection(&swapchain->worker_cs);
    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
    {
        WARN("DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT not set for swap chain %p.\n", iface);
        hr = DXGI_ERROR_INVALID_CALL;
    }
    else if (!max_latency)
    {
        hr = DXGI_ERROR_INVALID_CALL;
    }
    else
    {
        *max_latency = swapchain->frame_latency;
    }
    LeaveCriticalSection(&swapchain->worker_cs);
    return hr;
}

static HANDLE STDMETHODCALLTYPE d3d12_swapchain_GetFrameLatencyWaitableObject(IDXGISwapChain4 *iface)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    HANDLE dup;
    BOOL ret;

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&swapchain->worker_cs);
    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
    {
        LeaveCriticalSection(&swapchain->worker_cs);
        WARN("DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT not set for swap chain %p.\n", iface);
        return NULL;
    }
    LeaveCriticalSection(&swapchain->worker_cs);

    ret = DuplicateHandle(GetCurrentProcess(), swapchain->frame_latency_semaphore, GetCurrentProcess(),
            &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);

    if (!ret)
    {
        ERR("Cannot duplicate handle, last error %lu.\n", GetLastError());
        return NULL;
    }

    return dup;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetMatrixTransform(IDXGISwapChain4 *iface,
        const DXGI_MATRIX_3X2_F *matrix)
{
    FIXME("iface %p, matrix %p stub!\n", iface, matrix);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_GetMatrixTransform(IDXGISwapChain4 *iface,
        DXGI_MATRIX_3X2_F *matrix)
{
    FIXME("iface %p, matrix %p stub!\n", iface, matrix);

    return E_NOTIMPL;
}

/* IDXGISwapChain3 methods */

static UINT STDMETHODCALLTYPE d3d12_swapchain_GetCurrentBackBufferIndex(IDXGISwapChain4 *iface)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    unsigned int current_buffer_index;

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&swapchain->worker_cs);
    current_buffer_index = swapchain->current_buffer_index;
    assert(current_buffer_index < swapchain->desc.BufferCount);
    LeaveCriticalSection(&swapchain->worker_cs);

    TRACE("Current back buffer index %u.\n", current_buffer_index);
    return current_buffer_index;
}

static BOOL d3d12_swapchain_color_space_supported(struct d3d12_swapchain *swapchain,
        DXGI_COLOR_SPACE_TYPE color_space)
{
    DXGI_SWAP_CHAIN_DESC1 desc;
    VkColorSpaceKHR vk_color_space;
    VkFormat vk_format;

    EnterCriticalSection(&swapchain->worker_cs);
    desc = swapchain->desc;
    LeaveCriticalSection(&swapchain->worker_cs);

    return SUCCEEDED(select_vk_format(&swapchain->vk_funcs, swapchain->vk_physical_device,
            swapchain->vk_surface, &desc, color_space, &vk_format, &vk_color_space));
}

static void d3d12_swapchain_apply_vk_hdr_metadata(struct d3d12_swapchain *swapchain)
{
    const DXGI_HDR_METADATA_HDR10 *metadata = &swapchain->hdr10_metadata;
    VkHdrMetadataEXT vk_metadata;

    if (!swapchain->has_hdr10_metadata || !swapchain->vk_funcs.p_vkSetHdrMetadataEXT
            || swapchain->backend_color_space != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        return;

    memset(&vk_metadata, 0, sizeof(vk_metadata));
    vk_metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
    vk_metadata.displayPrimaryRed.x = metadata->RedPrimary[0] / 50000.0f;
    vk_metadata.displayPrimaryRed.y = metadata->RedPrimary[1] / 50000.0f;
    vk_metadata.displayPrimaryGreen.x = metadata->GreenPrimary[0] / 50000.0f;
    vk_metadata.displayPrimaryGreen.y = metadata->GreenPrimary[1] / 50000.0f;
    vk_metadata.displayPrimaryBlue.x = metadata->BluePrimary[0] / 50000.0f;
    vk_metadata.displayPrimaryBlue.y = metadata->BluePrimary[1] / 50000.0f;
    vk_metadata.whitePoint.x = metadata->WhitePoint[0] / 50000.0f;
    vk_metadata.whitePoint.y = metadata->WhitePoint[1] / 50000.0f;
    vk_metadata.maxLuminance = metadata->MaxMasteringLuminance;
    vk_metadata.minLuminance = metadata->MinMasteringLuminance / 10000.0f;
    vk_metadata.maxContentLightLevel = metadata->MaxContentLightLevel;
    vk_metadata.maxFrameAverageLightLevel = metadata->MaxFrameAverageLightLevel;
    swapchain->vk_funcs.p_vkSetHdrMetadataEXT(swapchain->vk_device, 1,
            &swapchain->vk_swapchain, &vk_metadata);
}

/* The caller must hold the colour-reconfiguration reservation.  That
 * reservation drains the worker and excludes resize, present, and other
 * colour state changes, so the backend and metadata snapshots stay coherent
 * for both the forward operation and rollback. */
static HRESULT d3d12_swapchain_recreate_for_color_space_locked(struct d3d12_swapchain *swapchain,
        DXGI_COLOR_SPACE_TYPE color_space, BOOL clear_hdr_metadata)
{
    DXGI_COLOR_SPACE_TYPE old_backend_color_space;
    DXGI_HDR_METADATA_HDR10 old_metadata;
    BOOL old_has_metadata;
    HRESULT restore_hr, hr;

    old_backend_color_space = swapchain->backend_color_space;
    old_metadata = swapchain->hdr10_metadata;
    old_has_metadata = swapchain->has_hdr10_metadata;
    if (FAILED(hr = d3d12_swapchain_destroy_vulkan_resources(swapchain)))
    {
        d3d12_swapchain_record_async_error(swapchain, hr);
        return hr;
    }

    swapchain->backend_color_space = color_space;
    if (clear_hdr_metadata)
    {
        memset(&swapchain->hdr10_metadata, 0, sizeof(swapchain->hdr10_metadata));
        swapchain->has_hdr10_metadata = false;
    }
    if (FAILED(hr = d3d12_swapchain_create_vulkan_resources(swapchain)))
    {
        WARN("Failed to create Vulkan resources for colour space %#x, hr %#lx.\n",
                color_space, hr);
        d3d12_swapchain_destroy_vulkan_resources(swapchain);
        swapchain->backend_color_space = old_backend_color_space;
        swapchain->hdr10_metadata = old_metadata;
        swapchain->has_hdr10_metadata = old_has_metadata;
        if (FAILED(restore_hr = d3d12_swapchain_create_vulkan_resources(swapchain)))
        {
            ERR("Failed to restore Vulkan resources for colour space %#x, hr %#lx.\n",
                    old_backend_color_space, restore_hr);
            d3d12_swapchain_record_async_error(swapchain, restore_hr);
            hr = restore_hr;
        }
        else
        {
            d3d12_swapchain_apply_vk_hdr_metadata(swapchain);
        }
        return hr;
    }

    EnterCriticalSection(&swapchain->worker_cs);
    swapchain->color_space = color_space;
    LeaveCriticalSection(&swapchain->worker_cs);
    d3d12_swapchain_apply_vk_hdr_metadata(swapchain);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_CheckColorSpaceSupport(IDXGISwapChain4 *iface,
        DXGI_COLOR_SPACE_TYPE colour_space, UINT *colour_space_support)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    UINT support_flags = 0;

    TRACE("iface %p, colour_space %#x, colour_space_support %p.\n",
            iface, colour_space, colour_space_support);

    if (!colour_space_support)
        return E_INVALIDARG;

    if (d3d12_swapchain_color_space_supported(swapchain, colour_space))
        support_flags |= DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;

    *colour_space_support = support_flags;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetColorSpace1(IDXGISwapChain4 *iface,
        DXGI_COLOR_SPACE_TYPE colour_space)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    DXGI_COLOR_SPACE_TYPE current_color_space;
    HRESULT hr;

    TRACE("iface %p, colour_space %#x.\n", iface, colour_space);

    if (FAILED(hr = d3d12_swapchain_begin_color_reconfigure(swapchain)))
        return hr;

    if (!d3d12_swapchain_color_space_supported(swapchain, colour_space))
    {
        WARN("Colour space %u not supported.\n", colour_space);
        d3d12_swapchain_end_color_reconfigure(swapchain);
        return E_INVALIDARG;
    }

    current_color_space = swapchain->color_space;
    if (current_color_space == colour_space)
        hr = S_OK;
    else
        hr = d3d12_swapchain_recreate_for_color_space_locked(swapchain, colour_space, FALSE);

    d3d12_swapchain_end_color_reconfigure(swapchain);
    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_ResizeBuffers1(IDXGISwapChain4 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
        const UINT *node_mask, IUnknown * const *present_queue)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    size_t i, count;

    TRACE("iface %p, buffer_count %u, width %u, height %u, format %s, flags %#x, "
            "node_mask %p, present_queue %p.\n",
            iface, buffer_count, width, height, debug_dxgi_format(format), flags, node_mask, present_queue);

    if (!node_mask || !present_queue)
        return DXGI_ERROR_INVALID_CALL;

    count = buffer_count ? buffer_count : swapchain->desc.BufferCount;
    for (i = 0; i < count; ++i)
    {
        if (node_mask[i] > 1 || !present_queue[i])
            return DXGI_ERROR_INVALID_CALL;
        if ((ID3D12CommandQueue*)present_queue[i] != swapchain->command_queue)
            FIXME("Ignoring present queue %p.\n", present_queue[i]);
    }

    return d3d12_swapchain_resize_buffers(swapchain, buffer_count, width, height, format, flags);
}

/* IDXGISwapChain4 methods */

static HRESULT STDMETHODCALLTYPE d3d12_swapchain_SetHDRMetaData(IDXGISwapChain4 *iface,
        DXGI_HDR_METADATA_TYPE type, UINT size, void *metadata)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_IDXGISwapChain4(iface);
    DXGI_HDR_METADATA_HDR10 new_metadata;
    DXGI_COLOR_SPACE_TYPE current_color_space;
    HRESULT hr;

    TRACE("iface %p, type %#x, size %#x, metadata %p.\n", iface, type, size, metadata);

    if (type == DXGI_HDR_METADATA_TYPE_NONE)
    {
        if (size || metadata) return E_INVALIDARG;

        if (FAILED(hr = d3d12_swapchain_begin_color_reconfigure(swapchain)))
            return hr;
        current_color_space = swapchain->color_space;
        if (!swapchain->has_hdr10_metadata)
            hr = S_OK;
        else if (current_color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            hr = d3d12_swapchain_recreate_for_color_space_locked(
                    swapchain, current_color_space, TRUE);
        else
        {
            memset(&swapchain->hdr10_metadata, 0, sizeof(swapchain->hdr10_metadata));
            swapchain->has_hdr10_metadata = false;
            hr = S_OK;
        }
        d3d12_swapchain_end_color_reconfigure(swapchain);
        return hr;
    }

    if (type != DXGI_HDR_METADATA_TYPE_HDR10)
        return E_INVALIDARG;
    if (size != sizeof(DXGI_HDR_METADATA_HDR10) || !metadata)
        return E_INVALIDARG;
    new_metadata = *(const DXGI_HDR_METADATA_HDR10 *)metadata;
    if (!hdr10_metadata_is_valid(&new_metadata)) return E_INVALIDARG;

    if (!swapchain->vk_funcs.p_vkSetHdrMetadataEXT)
    {
        WARN("HDR metadata is not supported by the active display/provider path.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (FAILED(hr = d3d12_swapchain_begin_color_reconfigure(swapchain)))
        return hr;
    current_color_space = swapchain->color_space;
    if (current_color_space != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
            || !d3d12_swapchain_color_space_supported(swapchain, current_color_space))
    {
        WARN("HDR metadata is not supported by the active display/provider path.\n");
        d3d12_swapchain_end_color_reconfigure(swapchain);
        return DXGI_ERROR_INVALID_CALL;
    }
    if (swapchain->has_hdr10_metadata
            && !memcmp(&swapchain->hdr10_metadata, &new_metadata, sizeof(new_metadata)))
    {
        d3d12_swapchain_end_color_reconfigure(swapchain);
        return S_OK;
    }

    swapchain->hdr10_metadata = new_metadata;
    swapchain->has_hdr10_metadata = true;
    d3d12_swapchain_apply_vk_hdr_metadata(swapchain);
    d3d12_swapchain_end_color_reconfigure(swapchain);
    return S_OK;
}

static const struct IDXGISwapChain4Vtbl d3d12_swapchain_vtbl =
{
    /* IUnknown methods */
    d3d12_swapchain_QueryInterface,
    d3d12_swapchain_AddRef,
    d3d12_swapchain_Release,
    /* IDXGIObject methods */
    d3d12_swapchain_SetPrivateData,
    d3d12_swapchain_SetPrivateDataInterface,
    d3d12_swapchain_GetPrivateData,
    d3d12_swapchain_GetParent,
    /* IDXGIDeviceSubObject methods */
    d3d12_swapchain_GetDevice,
    /* IDXGISwapChain methods */
    d3d12_swapchain_Present,
    d3d12_swapchain_GetBuffer,
    d3d12_swapchain_SetFullscreenState,
    d3d12_swapchain_GetFullscreenState,
    d3d12_swapchain_GetDesc,
    d3d12_swapchain_ResizeBuffers,
    d3d12_swapchain_ResizeTarget,
    d3d12_swapchain_GetContainingOutput,
    d3d12_swapchain_GetFrameStatistics,
    d3d12_swapchain_GetLastPresentCount,
    /* IDXGISwapChain1 methods */
    d3d12_swapchain_GetDesc1,
    d3d12_swapchain_GetFullscreenDesc,
    d3d12_swapchain_GetHwnd,
    d3d12_swapchain_GetCoreWindow,
    d3d12_swapchain_Present1,
    d3d12_swapchain_IsTemporaryMonoSupported,
    d3d12_swapchain_GetRestrictToOutput,
    d3d12_swapchain_SetBackgroundColor,
    d3d12_swapchain_GetBackgroundColor,
    d3d12_swapchain_SetRotation,
    d3d12_swapchain_GetRotation,
    /* IDXGISwapChain2 methods */
    d3d12_swapchain_SetSourceSize,
    d3d12_swapchain_GetSourceSize,
    d3d12_swapchain_SetMaximumFrameLatency,
    d3d12_swapchain_GetMaximumFrameLatency,
    d3d12_swapchain_GetFrameLatencyWaitableObject,
    d3d12_swapchain_SetMatrixTransform,
    d3d12_swapchain_GetMatrixTransform,
    /* IDXGISwapChain3 methods */
    d3d12_swapchain_GetCurrentBackBufferIndex,
    d3d12_swapchain_CheckColorSpaceSupport,
    d3d12_swapchain_SetColorSpace1,
    d3d12_swapchain_ResizeBuffers1,
    /* IDXGISwapChain4 methods */
    d3d12_swapchain_SetHDRMetaData,
};

static BOOL init_vk_funcs(struct dxgi_vk_funcs *dxgi, VkInstance vk_instance, VkDevice vk_device)
{
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;

    dxgi->vulkan_module = LoadLibraryA("vulkan-1.dll");
    if (!(vkGetInstanceProcAddr = (void *)GetProcAddress(dxgi->vulkan_module, "vkGetInstanceProcAddr")))
    {
        ERR_(winediag)("Failed to load Vulkan.\n");
        return FALSE;
    }

    vkGetDeviceProcAddr = (void *)vkGetInstanceProcAddr(vk_instance, "vkGetDeviceProcAddr");

#define LOAD_INSTANCE_PFN(name) \
    if (!(dxgi->p_##name = (void *)vkGetInstanceProcAddr(vk_instance, #name))) \
    { \
        ERR("Failed to get instance proc "#name".\n"); \
        FreeLibrary(dxgi->vulkan_module); \
        return FALSE; \
    }
    LOAD_INSTANCE_PFN(vkCreateWin32SurfaceKHR)
    LOAD_INSTANCE_PFN(vkDestroySurfaceKHR)
    LOAD_INSTANCE_PFN(vkGetPhysicalDeviceMemoryProperties)
    LOAD_INSTANCE_PFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
    LOAD_INSTANCE_PFN(vkGetPhysicalDeviceSurfaceFormatsKHR)
    LOAD_INSTANCE_PFN(vkGetPhysicalDeviceSurfacePresentModesKHR)
    LOAD_INSTANCE_PFN(vkGetPhysicalDeviceSurfaceSupportKHR)
    LOAD_INSTANCE_PFN(vkGetPhysicalDeviceWin32PresentationSupportKHR)
#undef LOAD_INSTANCE_PFN

#define LOAD_DEVICE_PFN(name) \
    if (!(dxgi->p_##name = (void *)vkGetDeviceProcAddr(vk_device, #name))) \
    { \
        ERR("Failed to get device proc "#name".\n"); \
        FreeLibrary(dxgi->vulkan_module); \
        return FALSE; \
    }
    LOAD_DEVICE_PFN(vkAcquireNextImageKHR)
    LOAD_DEVICE_PFN(vkAllocateCommandBuffers)
    LOAD_DEVICE_PFN(vkAllocateMemory)
    LOAD_DEVICE_PFN(vkBeginCommandBuffer)
    LOAD_DEVICE_PFN(vkBindImageMemory)
    LOAD_DEVICE_PFN(vkCmdBlitImage)
    LOAD_DEVICE_PFN(vkCmdPipelineBarrier)
    LOAD_DEVICE_PFN(vkCreateCommandPool)
    LOAD_DEVICE_PFN(vkCreateFence)
    LOAD_DEVICE_PFN(vkCreateImage)
    LOAD_DEVICE_PFN(vkCreateSemaphore)
    LOAD_DEVICE_PFN(vkCreateSwapchainKHR)
    LOAD_DEVICE_PFN(vkDestroyCommandPool)
    LOAD_DEVICE_PFN(vkDestroyFence)
    LOAD_DEVICE_PFN(vkDestroyImage)
    LOAD_DEVICE_PFN(vkDestroySemaphore)
    LOAD_DEVICE_PFN(vkDestroySwapchainKHR)
    LOAD_DEVICE_PFN(vkEndCommandBuffer)
    LOAD_DEVICE_PFN(vkFreeMemory)
    LOAD_DEVICE_PFN(vkResetCommandBuffer)
    LOAD_DEVICE_PFN(vkGetImageMemoryRequirements)
    LOAD_DEVICE_PFN(vkGetSwapchainImagesKHR)
    LOAD_DEVICE_PFN(vkQueuePresentKHR)
    LOAD_DEVICE_PFN(vkQueueSubmit)
    LOAD_DEVICE_PFN(vkQueueWaitIdle)
    LOAD_DEVICE_PFN(vkResetFences)
    LOAD_DEVICE_PFN(vkWaitForFences)
#undef LOAD_DEVICE_PFN

    /* HDR metadata is optional and must never be inferred from the provider. */
    dxgi->p_vkSetHdrMetadataEXT = (void *)vkGetDeviceProcAddr(vk_device, "vkSetHdrMetadataEXT");

    return TRUE;
}

static inline struct d3d12_swapchain *d3d12_swapchain_from_wined3d_swapchain_state_parent(struct wined3d_swapchain_state_parent *parent)
{
    return CONTAINING_RECORD(parent, struct d3d12_swapchain, state_parent);
}

static void CDECL d3d12_swapchain_windowed_state_changed(struct wined3d_swapchain_state_parent *parent,
        BOOL windowed)
{
    struct d3d12_swapchain *swapchain = d3d12_swapchain_from_wined3d_swapchain_state_parent(parent);

    TRACE("parent %p, windowed %d.\n", parent, windowed);

    if (windowed && swapchain->target)
    {
        IDXGIOutput_Release(swapchain->target);
        swapchain->target = NULL;
    }
}

static const struct wined3d_swapchain_state_parent_ops d3d12_swapchain_state_parent_ops =
{
    d3d12_swapchain_windowed_state_changed,
};

static HRESULT d3d12_swapchain_init(struct d3d12_swapchain *swapchain, IWineDXGIFactory *factory,
        ID3D12Device *device, ID3D12CommandQueue *queue, HWND window,
        const DXGI_SWAP_CHAIN_DESC1 *swapchain_desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
        IDXGIOutput *restrict_to_output)
{
    const struct dxgi_vk_funcs *vk_funcs = &swapchain->vk_funcs;
    struct wined3d_swapchain_desc wined3d_desc;
    VkWin32SurfaceCreateInfoKHR surface_desc;
    VkPhysicalDevice vk_physical_device;
    struct dxgi_factory *dxgi_factory;
    VkFenceCreateInfo fence_desc;
    uint32_t queue_family_index;
    VkSurfaceKHR vk_surface;
    VkInstance vk_instance;
    IDXGIAdapter *restrict_adapter;
    DXGI_ADAPTER_DESC restrict_desc;
    LUID device_luid;
    IDXGIOutput *output;
    VkBool32 supported;
    VkDevice vk_device;
    VkFence vk_fence;
    bool fullscreen;
    VkResult vr;
    HRESULT hr;

    if (window == GetDesktopWindow())
    {
        WARN("D3D12 swapchain cannot be created on desktop window.\n");
        return E_ACCESSDENIED;
    }

    if (restrict_to_output)
    {
        if (FAILED(hr = IDXGIOutput_GetParent(restrict_to_output,
                &IID_IDXGIAdapter, (void **)&restrict_adapter)))
        {
            WARN("Failed to get restricted output adapter, hr %#lx.\n", hr);
            return E_INVALIDARG;
        }
        hr = IDXGIAdapter_GetDesc(restrict_adapter, &restrict_desc);
        IDXGIAdapter_Release(restrict_adapter);
        if (FAILED(hr))
        {
            WARN("Failed to get restricted output adapter description, hr %#lx.\n", hr);
            return E_INVALIDARG;
        }

        device_luid = ID3D12Device_GetAdapterLuid(device);
        if (memcmp(&device_luid, &restrict_desc.AdapterLuid, sizeof(device_luid)))
        {
            WARN("Restricted output belongs to a different adapter.\n");
            return E_INVALIDARG;
        }
    }

    swapchain->IDXGISwapChain4_iface.lpVtbl = &d3d12_swapchain_vtbl;
    swapchain->state_parent.ops = &d3d12_swapchain_state_parent_ops;
    swapchain->refcount = 1;

    swapchain->window = window;
    swapchain->desc = *swapchain_desc;
    swapchain->backend_desc = *swapchain_desc;
    swapchain->color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    swapchain->backend_color_space = swapchain->color_space;
    swapchain->fullscreen_desc = *fullscreen_desc;

    swapchain->present_mode = VK_PRESENT_MODE_FIFO_KHR;

    switch (swapchain_desc->SwapEffect)
    {
        case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
        case DXGI_SWAP_EFFECT_FLIP_DISCARD:
            FIXME("Ignoring swap effect %#x.\n", swapchain_desc->SwapEffect);
            break;
        default:
            WARN("Invalid swap effect %#x.\n", swapchain_desc->SwapEffect);
            return DXGI_ERROR_INVALID_CALL;
    }

    if (FAILED(hr = dxgi_get_output_from_window(factory, window, &output)))
    {
        WARN("Failed to get output from window %p, hr %#lx.\n", window, hr);
        return hr;
    }

    hr = wined3d_swapchain_desc_from_dxgi(&wined3d_desc, output, window, swapchain_desc,
            fullscreen_desc);
    if (FAILED(hr))
    {
        IDXGIOutput_Release(output);
        return hr;
    }

    fullscreen = !wined3d_desc.windowed;
    wined3d_desc.windowed = TRUE;

    dxgi_factory = unsafe_impl_from_IDXGIFactory((IDXGIFactory *)factory);
    if (FAILED(hr = wined3d_swapchain_state_create(&wined3d_desc, window, dxgi_factory->wined3d,
            &swapchain->state_parent, &swapchain->state)))
    {
        IDXGIOutput_Release(output);
        return hr;
    }

    wined3d_swapchain_state_get_size(swapchain->state, &swapchain->desc.Width, &swapchain->desc.Height);

    if (fullscreen)
    {
        wined3d_desc.windowed = FALSE;
        hr = wined3d_swapchain_state_set_fullscreen(swapchain->state, &wined3d_desc, NULL);
        if (FAILED(hr))
        {
            wined3d_swapchain_state_destroy(swapchain->state);
            IDXGIOutput_Release(output);
            return hr;
        }

        swapchain->target = output;
    }
    else
    {
        IDXGIOutput_Release(output);
    }

    if (swapchain_desc->BufferUsage & ~(DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT))
        FIXME("Ignoring buffer usage %#x.\n", swapchain_desc->BufferUsage & ~(DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT));
    if (swapchain_desc->Scaling != DXGI_SCALING_STRETCH && swapchain_desc->Scaling != DXGI_SCALING_NONE)
        FIXME("Ignoring scaling %#x.\n", swapchain_desc->Scaling);
    if (swapchain_desc->AlphaMode && swapchain_desc->AlphaMode != DXGI_ALPHA_MODE_IGNORE)
        FIXME("Ignoring alpha mode %#x.\n", swapchain_desc->AlphaMode);
    if (swapchain_desc->Flags & ~(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
            | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
        FIXME("Ignoring swapchain flags %#x.\n", swapchain_desc->Flags
                & ~(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
                | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING));

    if (fullscreen_desc->RefreshRate.Numerator || fullscreen_desc->RefreshRate.Denominator)
        FIXME("Ignoring refresh rate.\n");
    if (fullscreen_desc->ScanlineOrdering)
        FIXME("Unhandled scanline ordering %#x.\n", fullscreen_desc->ScanlineOrdering);
    if (fullscreen_desc->Scaling)
        FIXME("Unhandled mode scaling %#x.\n", fullscreen_desc->Scaling);

    vk_instance = vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(device));
    vk_physical_device = vkd3d_get_vk_physical_device(device);
    vk_device = vkd3d_get_vk_device(device);

    swapchain->vk_instance = vk_instance;
    swapchain->vk_device = vk_device;
    swapchain->vk_physical_device = vk_physical_device;

    if (!init_vk_funcs(&swapchain->vk_funcs, vk_instance, vk_device))
    {
        if (swapchain->target)
        {
            IDXGIOutput_Release(swapchain->target);
            swapchain->target = NULL;
        }
        wined3d_swapchain_state_destroy(swapchain->state);
        return E_FAIL;
    }

    wined3d_private_store_init(&swapchain->private_store);

    InitializeCriticalSection(&swapchain->worker_cs);
    InitializeConditionVariable(&swapchain->worker_cv);
    swapchain->worker_running = true;
    list_init(&swapchain->worker_ops);
    if (restrict_to_output)
        IDXGIOutput_AddRef(swapchain->restrict_to_output = restrict_to_output);

    surface_desc.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_desc.pNext = NULL;
    surface_desc.flags = 0;
    surface_desc.hinstance = GetModuleHandleA("dxgi.dll");
    surface_desc.hwnd = window;
    if ((vr = vk_funcs->p_vkCreateWin32SurfaceKHR(vk_instance, &surface_desc, NULL, &vk_surface)) < 0)
    {
        WARN("Failed to create Vulkan surface, vr %d.\n", vr);
        d3d12_swapchain_destroy(swapchain);
        return hresult_from_vk_result(vr);
    }
    swapchain->vk_surface = vk_surface;

    queue_family_index = vkd3d_get_vk_queue_family_index(queue);
    if ((vr = vk_funcs->p_vkGetPhysicalDeviceSurfaceSupportKHR(vk_physical_device,
            queue_family_index, vk_surface, &supported)) < 0 || !supported)
    {
        FIXME("Queue family does not support presentation, vr %d.\n", vr);
        d3d12_swapchain_destroy(swapchain);
        return DXGI_ERROR_UNSUPPORTED;
    }

    ID3D12CommandQueue_AddRef(swapchain->command_queue = queue);
    ID3D12Device_AddRef(swapchain->device = device);

    if (FAILED(hr = d3d12_swapchain_create_d3d12_resources(swapchain)))
    {
        d3d12_swapchain_destroy(swapchain);
        return hr;
    }

    if (FAILED(hr = d3d12_swapchain_create_vulkan_resources(swapchain)))
    {
        d3d12_swapchain_destroy(swapchain);
        return hr;
    }

    fence_desc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_desc.pNext = NULL;
    fence_desc.flags = 0;
    if ((vr = vk_funcs->p_vkCreateFence(vk_device, &fence_desc, NULL, &vk_fence)) < 0)
    {
        WARN("Failed to create Vulkan fence, vr %d.\n", vr);
        d3d12_swapchain_destroy(swapchain);
        return hresult_from_vk_result(vr);
    }
    swapchain->vk_fence = vk_fence;

    swapchain->current_buffer_index = 0;

    if (swapchain_desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        swapchain->frame_latency = 1;
    else
        swapchain->frame_latency = 3;

    if (!(swapchain->frame_latency_semaphore = CreateSemaphoreW(NULL, swapchain->frame_latency, LONG_MAX, NULL)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        WARN("Failed to create frame latency semaphore, hr %#lx.\n", hr);
        d3d12_swapchain_destroy(swapchain);
        return hr;
    }

    if (FAILED(hr = ID3D12Device_CreateFence(device, 0, 0,
            &IID_ID3D12Fence, (void **)&swapchain->present_fence)))
    {
        WARN("Failed to create present fence, hr %#lx.\n", hr);
        d3d12_swapchain_destroy(swapchain);
        return hr;
    }

    if (!(swapchain->worker_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        WARN("Failed to create worker stop event, hr %#lx.\n", hr);
        d3d12_swapchain_destroy(swapchain);
        return hr;
    }

    IWineDXGIFactory_AddRef(swapchain->factory = factory);

    if (!(swapchain->worker_thread = CreateThread(NULL, 0, d3d12_swapchain_worker_proc, swapchain, 0, NULL)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        WARN("Failed to create worker thread, hr %#lx.\n", hr);
        d3d12_swapchain_destroy(swapchain);
        return hr;
    }

    return S_OK;
}

HRESULT d3d12_swapchain_create(IWineDXGIFactory *factory, ID3D12CommandQueue *queue, HWND window,
        const DXGI_SWAP_CHAIN_DESC1 *swapchain_desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
        IDXGIOutput *restrict_to_output, IDXGISwapChain1 **swapchain)
{
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC default_fullscreen_desc;
    struct D3D12_COMMAND_QUEUE_DESC queue_desc;
    struct d3d12_swapchain *object;
    ID3D12Device *device;
    HRESULT hr;

    if (swapchain_desc->Format == DXGI_FORMAT_UNKNOWN)
        return DXGI_ERROR_INVALID_CALL;

    queue_desc = ID3D12CommandQueue_GetDesc(queue);
    if (queue_desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return DXGI_ERROR_INVALID_CALL;

    if (!fullscreen_desc)
    {
        memset(&default_fullscreen_desc, 0, sizeof(default_fullscreen_desc));
        default_fullscreen_desc.Windowed = TRUE;
        fullscreen_desc = &default_fullscreen_desc;
    }

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = ID3D12CommandQueue_GetDevice(queue, &IID_ID3D12Device, (void **)&device)))
    {
        ERR("Failed to get d3d12 device, hr %#lx.\n", hr);
        free(object);
        return hr;
    }

    hr = d3d12_swapchain_init(object, factory, device, queue, window, swapchain_desc,
            fullscreen_desc, restrict_to_output);
    ID3D12Device_Release(device);
    if (FAILED(hr))
    {
        free(object);
        return hr;
    }

    TRACE("Created swapchain %p.\n", object);

    *swapchain = (IDXGISwapChain1 *)&object->IDXGISwapChain4_iface;

    return S_OK;
}
