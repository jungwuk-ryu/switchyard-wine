/*
 * Copyright 2009 Henri Verbeet for CodeWeavers
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "dxgi_private.h"
#include "ntgdi.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

#define DXGI_RESOURCE_MISC_SHARED            0x00000002u
#define DXGI_RESOURCE_MISC_SHARED_KEYEDMUTEX 0x00000100u
#define DXGI_RESOURCE_MISC_SHARED_NTHANDLE   0x00000800u
#define DXGI_LEGACY_SHARED_HANDLE_MASK       0xc0000000u
#define DXGI_D3DKMT_SHARED_HEAP_SIZE         0x10000u

struct dxgi_d3dkmt_resource_desc
{
    UINT size;
    UINT version;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT unknown_0;
    UINT unknown_1;
    UINT keyed_mutex;
    D3DKMT_HANDLE mutex_handle;
    D3DKMT_HANDLE sync_handle;
    UINT nt_shared;
    UINT unknown_2;
    UINT unknown_3;
    UINT unknown_4;
};

struct dxgi_shared_resource_entry
{
    HANDLE handle;
    WCHAR *name;
    BOOL nt_handle;
    struct dxgi_resource *resource;
    struct dxgi_shared_resource_entry *next;
};

static CRITICAL_SECTION dxgi_shared_resource_cs = { NULL, -1, 0, 0, 0, 0 };
static struct dxgi_shared_resource_entry *dxgi_shared_resources;
static LONG dxgi_shared_resource_next_id;

static struct dxgi_shared_resource_entry *dxgi_shared_resource_find(HANDLE handle, BOOL nt_handle)
{
    struct dxgi_shared_resource_entry *entry;

    for (entry = dxgi_shared_resources; entry; entry = entry->next)
    {
        if (entry->handle == handle && entry->nt_handle == nt_handle)
            return entry;
    }

    return NULL;
}

static struct dxgi_shared_resource_entry *dxgi_shared_resource_find_name(const WCHAR *name)
{
    struct dxgi_shared_resource_entry *entry;

    if (!name)
        return NULL;

    for (entry = dxgi_shared_resources; entry; entry = entry->next)
    {
        if (entry->nt_handle && entry->name && !wcsicmp(entry->name, name))
            return entry;
    }

    return NULL;
}

static WCHAR *dxgi_strdupW(const WCHAR *str)
{
    WCHAR *ret;
    size_t len;

    if (!str)
        return NULL;

    len = wcslen(str) + 1;
    if (!(ret = malloc(len * sizeof(*ret))))
        return NULL;
    memcpy(ret, str, len * sizeof(*ret));
    return ret;
}

static WCHAR *dxgi_shared_resource_object_name(HANDLE handle)
{
    OBJECT_NAME_INFORMATION *info;
    ULONG size = 0;
    WCHAR *name = NULL;
    NTSTATUS status;

    status = NtQueryObject(handle, ObjectNameInformation, NULL, 0, &size);
    if (status != STATUS_INFO_LENGTH_MISMATCH && status != STATUS_BUFFER_OVERFLOW)
        return NULL;

    if (!(info = malloc(size)))
        return NULL;

    status = NtQueryObject(handle, ObjectNameInformation, info, size, &size);
    if (!status && info->Name.Buffer && info->Name.Length)
    {
        size_t len = info->Name.Length / sizeof(WCHAR);

        if ((name = malloc((len + 1) * sizeof(*name))))
        {
            memcpy(name, info->Name.Buffer, info->Name.Length);
            name[len] = 0;
        }
    }

    free(info);
    return name;
}

static HRESULT dxgi_shared_resource_register_handle(struct dxgi_resource *resource,
        HANDLE handle, BOOL nt_handle, WCHAR *name)
{
    struct dxgi_shared_resource_entry *entry;

    if (!(entry = malloc(sizeof(*entry))))
        return E_OUTOFMEMORY;

    entry->handle = handle;
    entry->name = name;
    entry->nt_handle = nt_handle;
    entry->resource = resource;

    EnterCriticalSection(&dxgi_shared_resource_cs);
    entry->next = dxgi_shared_resources;
    dxgi_shared_resources = entry;
    LeaveCriticalSection(&dxgi_shared_resource_cs);

    return S_OK;
}

static HRESULT dxgi_shared_resource_get_handle(struct dxgi_resource *resource, HANDLE *handle)
{
    struct dxgi_shared_resource_entry *entry;
    ULONG id;

    if (resource->shared_handle)
    {
        *handle = resource->shared_handle;
        return S_OK;
    }

    if (!(entry = malloc(sizeof(*entry))))
        return E_OUTOFMEMORY;

    id = InterlockedIncrement(&dxgi_shared_resource_next_id) & ~DXGI_LEGACY_SHARED_HANDLE_MASK;
    if (!id)
        id = InterlockedIncrement(&dxgi_shared_resource_next_id) & ~DXGI_LEGACY_SHARED_HANDLE_MASK;

    EnterCriticalSection(&dxgi_shared_resource_cs);
    if (resource->shared_handle)
    {
        *handle = resource->shared_handle;
        LeaveCriticalSection(&dxgi_shared_resource_cs);
        free(entry);
        return S_OK;
    }

    entry->handle = (HANDLE)(ULONG_PTR)(DXGI_LEGACY_SHARED_HANDLE_MASK | id);
    entry->name = NULL;
    entry->nt_handle = FALSE;
    entry->resource = resource;
    entry->next = dxgi_shared_resources;
    dxgi_shared_resources = entry;
    resource->shared_handle = entry->handle;
    *handle = entry->handle;
    LeaveCriticalSection(&dxgi_shared_resource_cs);

    return S_OK;
}

static void dxgi_shared_resource_unregister(struct dxgi_resource *resource)
{
    struct dxgi_shared_resource_entry **entry;
    struct dxgi_shared_resource_entry *removed;

    EnterCriticalSection(&dxgi_shared_resource_cs);
    entry = &dxgi_shared_resources;
    while (*entry)
    {
        if ((*entry)->resource == resource)
        {
            removed = *entry;
            *entry = removed->next;
            free(removed->name);
            free(removed);
            continue;
        }
        entry = &(*entry)->next;
    }
    LeaveCriticalSection(&dxgi_shared_resource_cs);
    resource->shared_handle = NULL;
}

HRESULT dxgi_resource_open_shared(HANDLE handle, REFIID iid, void **resource)
{
    struct dxgi_shared_resource_entry *entry;
    IUnknown *outer_unknown;
    HRESULT hr;

    TRACE("handle %p, iid %s, resource %p.\n", handle, debugstr_guid(iid), resource);

    if (!resource)
        return E_POINTER;
    *resource = NULL;

    if (!((ULONG_PTR)handle & DXGI_LEGACY_SHARED_HANDLE_MASK))
        return E_INVALIDARG;

    EnterCriticalSection(&dxgi_shared_resource_cs);
    if (!(entry = dxgi_shared_resource_find(handle, FALSE)))
    {
        LeaveCriticalSection(&dxgi_shared_resource_cs);
        return E_INVALIDARG;
    }

    outer_unknown = entry->resource->outer_unknown;
    IUnknown_AddRef(outer_unknown);
    LeaveCriticalSection(&dxgi_shared_resource_cs);

    hr = IUnknown_QueryInterface(outer_unknown, iid, resource);
    IUnknown_Release(outer_unknown);
    return hr;
}

HRESULT dxgi_resource_open_shared_nt(HANDLE handle, REFIID iid, void **resource)
{
    struct dxgi_shared_resource_entry *entry;
    IUnknown *outer_unknown = NULL;
    WCHAR *name = NULL;
    HRESULT hr;

    TRACE("handle %p, iid %s, resource %p.\n", handle, debugstr_guid(iid), resource);

    if (!resource)
        return E_POINTER;
    *resource = NULL;

    if ((ULONG_PTR)handle & DXGI_LEGACY_SHARED_HANDLE_MASK)
        return E_INVALIDARG;

    EnterCriticalSection(&dxgi_shared_resource_cs);
    if ((entry = dxgi_shared_resource_find(handle, TRUE)))
    {
        outer_unknown = entry->resource->outer_unknown;
        IUnknown_AddRef(outer_unknown);
    }
    LeaveCriticalSection(&dxgi_shared_resource_cs);

    if (!outer_unknown)
    {
        name = dxgi_shared_resource_object_name(handle);

        EnterCriticalSection(&dxgi_shared_resource_cs);
        if ((entry = dxgi_shared_resource_find_name(name)))
        {
            outer_unknown = entry->resource->outer_unknown;
            IUnknown_AddRef(outer_unknown);
        }
        LeaveCriticalSection(&dxgi_shared_resource_cs);
    }

    if (!outer_unknown)
    {
        free(name);
        return E_INVALIDARG;
    }

    hr = IUnknown_QueryInterface(outer_unknown, iid, resource);
    IUnknown_Release(outer_unknown);
    free(name);
    return hr;
}

static HRESULT dxgi_shared_resource_name(struct dxgi_resource *resource, const WCHAR *name, WCHAR **object_name)
{
    static const WCHAR base_named_objectsW[] = L"\\Sessions\\%u\\BaseNamedObjects\\";
    static const WCHAR generated_nameW[] = L"SwitchyardDxgiSharedResource-%p-%08lx";
    WCHAR generated[128];
    WCHAR prefix[64];
    size_t prefix_len, name_len, len;
    const WCHAR *leaf;
    WCHAR *ret;

    if (name && name[0] == '\\')
    {
        if (!(ret = dxgi_strdupW(name)))
            return E_OUTOFMEMORY;
        *object_name = ret;
        return S_OK;
    }

    if (name)
    {
        leaf = name;
    }
    else
    {
        swprintf(generated, ARRAY_SIZE(generated), generated_nameW, resource,
                InterlockedIncrement(&dxgi_shared_resource_next_id));
        leaf = generated;
    }

    prefix_len = swprintf(prefix, ARRAY_SIZE(prefix), base_named_objectsW, RtlGetCurrentPeb()->SessionId);
    name_len = wcslen(leaf);
    len = prefix_len + name_len + 1;

    if (!(ret = malloc(len * sizeof(*ret))))
        return E_OUTOFMEMORY;

    wcscpy(ret, prefix);
    wcscat(ret, leaf);
    *object_name = ret;
    return S_OK;
}

static HRESULT dxgi_resource_create_shared_nt_handle(struct dxgi_resource *resource,
        const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *object_name, HANDLE *handle)
{
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open_adapter = {0};
    D3DKMT_CREATESTANDARDALLOCATION standard = {0};
    D3DKMT_DESTROYALLOCATION destroy_alloc = {0};
    D3DKMT_CREATEALLOCATION create_alloc = {0};
    D3DKMT_DESTROYDEVICE destroy_device = {0};
    struct dxgi_d3dkmt_resource_desc runtime = {0};
    D3DKMT_CREATEDEVICE create_device = {0};
    D3DDDI_ALLOCATIONINFO alloc_info = {0};
    D3DKMT_CLOSEADAPTER close_adapter = {0};
    struct wined3d_resource_desc desc;
    UNICODE_STRING name_str;
    OBJECT_ATTRIBUTES attr;
    void *sysmem = NULL;
    ULONG attr_flags;
    NTSTATUS status;

    *handle = NULL;

    wcscpy(open_adapter.DeviceName, L"\\\\.\\DISPLAY1");
    if ((status = D3DKMTOpenAdapterFromGdiDisplayName(&open_adapter)))
        return HRESULT_FROM_NT(status);

    create_device.hAdapter = open_adapter.hAdapter;
    if ((status = D3DKMTCreateDevice(&create_device)))
        goto done;

    if (!(sysmem = VirtualAlloc(NULL, DXGI_D3DKMT_SHARED_HEAP_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
    {
        status = STATUS_NO_MEMORY;
        goto done;
    }

    wined3d_resource_get_desc(resource->wined3d_resource, &desc);
    runtime.size = sizeof(runtime);
    runtime.version = 4;
    runtime.width = desc.width;
    runtime.height = desc.height;
    runtime.format = dxgi_format_from_wined3dformat(desc.format);
    runtime.keyed_mutex = !!(resource->misc_flags & DXGI_RESOURCE_MISC_SHARED_KEYEDMUTEX);
    runtime.nt_shared = 1;

    standard.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    standard.ExistingHeapData.Size = DXGI_D3DKMT_SHARED_HEAP_SIZE;

    alloc_info.pSystemMem = sysmem;
    create_alloc.hDevice = create_device.hDevice;
    create_alloc.Flags.ExistingSysMem = 1;
    create_alloc.Flags.StandardAllocation = 1;
    create_alloc.Flags.CreateResource = 1;
    create_alloc.Flags.NtSecuritySharing = 1;
    create_alloc.pStandardAllocation = &standard;
    create_alloc.NumAllocations = 1;
    create_alloc.pAllocationInfo = &alloc_info;
    create_alloc.pPrivateRuntimeData = &runtime;
    create_alloc.PrivateRuntimeDataSize = sizeof(runtime);

    if ((status = D3DKMTCreateAllocation(&create_alloc)))
        goto done;

    name_str.Buffer = (WCHAR *)object_name;
    name_str.Length = wcslen(object_name) * sizeof(WCHAR);
    name_str.MaximumLength = name_str.Length + sizeof(WCHAR);
    attr_flags = OBJ_CASE_INSENSITIVE;
    if (attributes && attributes->bInheritHandle)
        attr_flags |= OBJ_INHERIT;
    InitializeObjectAttributes(&attr, &name_str, attr_flags, NULL,
            attributes ? attributes->lpSecurityDescriptor : NULL);

    status = D3DKMTShareObjects(1, &create_alloc.hResource, &attr, access, handle);

done:
    if (create_alloc.hResource)
    {
        destroy_alloc.hDevice = create_device.hDevice;
        destroy_alloc.hResource = create_alloc.hResource;
        D3DKMTDestroyAllocation(&destroy_alloc);
    }
    if (sysmem)
        VirtualFree(sysmem, 0, MEM_RELEASE);
    if (create_device.hDevice)
    {
        destroy_device.hDevice = create_device.hDevice;
        D3DKMTDestroyDevice(&destroy_device);
    }
    if (open_adapter.hAdapter)
    {
        close_adapter.hAdapter = open_adapter.hAdapter;
        D3DKMTCloseAdapter(&close_adapter);
    }

    return status ? HRESULT_FROM_NT(status) : S_OK;
}

/* Inner IUnknown methods */

static inline struct dxgi_resource *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_resource, IUnknown_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_inner_QueryInterface(IUnknown *iface, REFIID riid, void **out)
{
    struct dxgi_resource *resource = impl_from_IUnknown(iface);
    bool is_subresource = !!resource->parent_resource;

    TRACE("iface %p, riid %s, out %p.\n", iface, debugstr_guid(riid), out);

    if ((IsEqualGUID(riid, &IID_IDXGISurface2)
            || IsEqualGUID(riid, &IID_IDXGISurface1)
            || IsEqualGUID(riid, &IID_IDXGISurface)) && resource->IDXGISurface2_iface.lpVtbl != NULL)
    {
        IDXGISurface2_AddRef(&resource->IDXGISurface2_iface);
        *out = &resource->IDXGISurface2_iface;
        return S_OK;
    }
    else if (!is_subresource && IsEqualGUID(riid, &IID_IDXGIKeyedMutex)
            && (resource->misc_flags & DXGI_RESOURCE_MISC_SHARED_KEYEDMUTEX))
    {
        IDXGIKeyedMutex_AddRef(&resource->IDXGIKeyedMutex_iface);
        *out = &resource->IDXGIKeyedMutex_iface;
        return S_OK;
    }
    else if ((!is_subresource && (IsEqualGUID(riid, &IID_IDXGIResource)
            || IsEqualGUID(riid, &IID_IDXGIResource1)))
            || IsEqualGUID(riid, &IID_IDXGIDeviceSubObject)
            || IsEqualGUID(riid, &IID_IDXGIObject)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        IDXGIResource1_AddRef(&resource->IDXGIResource1_iface);
        *out = &resource->IDXGIResource1_iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE\n", debugstr_guid(riid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dxgi_resource_inner_AddRef(IUnknown *iface)
{
    struct dxgi_resource *resource = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&resource->refcount);

    TRACE("%p increasing refcount to %lu.\n", resource, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE dxgi_resource_inner_Release(IUnknown *iface)
{
    struct dxgi_resource *resource = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&resource->refcount);

    TRACE("%p decreasing refcount to %lu.\n", resource, refcount);

    if (!refcount)
    {
        dxgi_shared_resource_unregister(resource);
        if (resource->parent_resource)
            IDXGIResource1_Release(resource->parent_resource);
        if (resource->misc_flags & DXGI_RESOURCE_MISC_SHARED_KEYEDMUTEX)
            DeleteCriticalSection(&resource->keyed_mutex_cs);
        wined3d_private_store_cleanup(&resource->private_store);
        free(resource);
    }

    return refcount;
}

static inline struct dxgi_resource *impl_from_IDXGISurface2(IDXGISurface2 *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_resource, IDXGISurface2_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE dxgi_surface_QueryInterface(IDXGISurface2 *iface, REFIID riid,
        void **object)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_QueryInterface(&resource->IDXGIResource1_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE dxgi_surface_AddRef(IDXGISurface2 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_AddRef(&resource->IDXGIResource1_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_surface_Release(IDXGISurface2 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_Release(&resource->IDXGIResource1_iface);
}

/* IDXGIObject methods */

static HRESULT STDMETHODCALLTYPE dxgi_surface_SetPrivateData(IDXGISurface2 *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_SetPrivateData(&resource->IDXGIResource1_iface, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_SetPrivateDataInterface(IDXGISurface2 *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_SetPrivateDataInterface(&resource->IDXGIResource1_iface, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_GetPrivateData(IDXGISurface2 *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_GetPrivateData(&resource->IDXGIResource1_iface, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_GetParent(IDXGISurface2 *iface, REFIID riid, void **parent)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_GetParent(&resource->IDXGIResource1_iface, riid, parent);
}

/* IDXGIDeviceSubObject methods */

static HRESULT STDMETHODCALLTYPE dxgi_surface_GetDevice(IDXGISurface2 *iface, REFIID riid, void **device)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    return IDXGIResource1_GetDevice(&resource->IDXGIResource1_iface, riid, device);
}

/* IDXGISurface methods */
static HRESULT STDMETHODCALLTYPE dxgi_surface_GetDesc(IDXGISurface2 *iface, DXGI_SURFACE_DESC *desc)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    bool is_subresource = !!resource->parent_resource, is_buffer = false;
    struct wined3d_sub_resource_desc subresource_desc;
    struct wined3d_resource_desc resource_desc;

    TRACE("iface %p, desc %p.\n", iface, desc);

    wined3d_mutex_lock();
    wined3d_resource_get_sub_resource_desc(resource->wined3d_resource, resource->subresource_idx, &subresource_desc);
    if (is_subresource)
    {
        wined3d_resource_get_desc(resource->wined3d_resource, &resource_desc);
        is_buffer = resource_desc.resource_type == WINED3D_RTYPE_BUFFER;
    }
    wined3d_mutex_unlock();
    desc->Width = subresource_desc.width;
    desc->Height = subresource_desc.height;
    desc->Format = is_buffer ? DXGI_FORMAT_UNKNOWN : dxgi_format_from_wined3dformat(subresource_desc.format);
    dxgi_sample_desc_from_wined3d(&desc->SampleDesc, subresource_desc.multisample_type, subresource_desc.multisample_quality);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_Map(IDXGISurface2 *iface, DXGI_MAPPED_RECT *mapped_rect, UINT flags)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    struct wined3d_map_desc wined3d_map_desc;
    DWORD wined3d_map_flags = 0;
    HRESULT hr;

    TRACE("iface %p, mapped_rect %p, flags %#x.\n", iface, mapped_rect, flags);

    if (flags & DXGI_MAP_READ)
        wined3d_map_flags |= WINED3D_MAP_READ;
    if (flags & DXGI_MAP_WRITE)
        wined3d_map_flags |= WINED3D_MAP_WRITE;
    if (flags & DXGI_MAP_DISCARD)
        wined3d_map_flags |= WINED3D_MAP_DISCARD;

    if (SUCCEEDED(hr = wined3d_resource_map(resource->wined3d_resource, resource->subresource_idx,
            &wined3d_map_desc, NULL, wined3d_map_flags)))
    {
        mapped_rect->Pitch = wined3d_map_desc.row_pitch;
        mapped_rect->pBits = wined3d_map_desc.data;
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_Unmap(IDXGISurface2 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);

    TRACE("iface %p.\n", iface);
    wined3d_resource_unmap(resource->wined3d_resource, resource->subresource_idx);
    return S_OK;
}

/* IDXGISurface1 methods */
static HRESULT STDMETHODCALLTYPE dxgi_surface_GetDC(IDXGISurface2 *iface, BOOL discard, HDC *hdc)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    HRESULT hr;

    TRACE("iface %p, discard %d, hdc %p.\n", iface, discard, hdc);

    if (!hdc)
        return E_INVALIDARG;
    if (resource->dc)
        return DXGI_ERROR_INVALID_CALL;

    wined3d_mutex_lock();
    hr = wined3d_texture_get_dc(wined3d_texture_from_resource(resource->wined3d_resource),
            resource->subresource_idx, hdc);
    wined3d_mutex_unlock();

    if (SUCCEEDED(hr))
        resource->dc = *hdc;
    else if (hr == WINED3DERR_INVALIDCALL)
        hr = DXGI_ERROR_INVALID_CALL;

    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_surface_ReleaseDC(IDXGISurface2 *iface, RECT *dirty_rect)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    struct wined3d_texture *texture;
    unsigned int layer;
    HRESULT hr;

    TRACE("iface %p, rect %s.\n", iface, wine_dbgstr_rect(dirty_rect));

    if (!resource->dc)
        return DXGI_ERROR_INVALID_CALL;

    wined3d_mutex_lock();
    texture = wined3d_texture_from_resource(resource->wined3d_resource);
    layer = resource->subresource_idx / wined3d_texture_get_level_count(texture);
    hr = wined3d_texture_release_dc(texture, resource->subresource_idx, resource->dc);
    if (SUCCEEDED(hr))
    {
        if (!dirty_rect)
        {
            wined3d_texture_add_dirty_region(texture, layer, NULL);
        }
        else if (!IsRectEmpty(dirty_rect))
        {
            struct wined3d_sub_resource_desc desc;
            struct wined3d_box box;

            wined3d_resource_get_sub_resource_desc(resource->wined3d_resource, resource->subresource_idx, &desc);
            box.left = max(dirty_rect->left, 0);
            box.top = max(dirty_rect->top, 0);
            box.right = min(dirty_rect->right, (LONG)desc.width);
            box.bottom = min(dirty_rect->bottom, (LONG)desc.height);
            box.front = 0;
            box.back = 1;

            if (box.left < box.right && box.top < box.bottom)
                wined3d_texture_add_dirty_region(texture, layer, &box);
        }
    }
    wined3d_mutex_unlock();

    if (SUCCEEDED(hr))
        resource->dc = NULL;
    else if (hr == WINED3DERR_INVALIDCALL)
        hr = DXGI_ERROR_INVALID_CALL;

    return hr;
}

/* IDXGISurface2 methods */
static HRESULT STDMETHODCALLTYPE dxgi_surface_GetResource(IDXGISurface2 *iface, REFIID iid,
        void **parent_resource, UINT *subresource_idx)
{
    struct dxgi_resource *resource = impl_from_IDXGISurface2(iface);
    HRESULT hr;

    TRACE("iface %p, iid %s, parent_resource %p, subresource_idx %p stub!\n", iface,
            wine_dbgstr_guid(iid), parent_resource, subresource_idx);

    if (!parent_resource)
        return E_POINTER;
    *parent_resource = NULL;

    if (resource->parent_resource)
        hr = IDXGIResource1_QueryInterface(resource->parent_resource, iid, parent_resource);
    else
        hr = IDXGIResource1_QueryInterface(&resource->IDXGIResource1_iface, iid, parent_resource);

    if (SUCCEEDED(hr))
        *subresource_idx = resource->subresource_idx;
    return hr;
}

static const struct IDXGISurface2Vtbl dxgi_surface_vtbl =
{
    /* IUnknown methods */
    dxgi_surface_QueryInterface,
    dxgi_surface_AddRef,
    dxgi_surface_Release,
    /* IDXGIObject methods */
    dxgi_surface_SetPrivateData,
    dxgi_surface_SetPrivateDataInterface,
    dxgi_surface_GetPrivateData,
    dxgi_surface_GetParent,
    /* IDXGIDeviceSubObject methods */
    dxgi_surface_GetDevice,
    /* IDXGISurface methods */
    dxgi_surface_GetDesc,
    dxgi_surface_Map,
    dxgi_surface_Unmap,
    /* IDXGISurface1 methods */
    dxgi_surface_GetDC,
    dxgi_surface_ReleaseDC,
    /* IDXGISurface2 methods */
    dxgi_surface_GetResource,
};

static inline struct dxgi_resource *impl_from_IDXGIResource1(IDXGIResource1 *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_resource, IDXGIResource1_iface);
}

/* IUnknown methods */

static HRESULT STDMETHODCALLTYPE dxgi_resource_QueryInterface(IDXGIResource1 *iface, REFIID riid,
        void **object)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    TRACE("Forwarding to outer IUnknown\n");
    return IUnknown_QueryInterface(resource->outer_unknown, riid, object);
}

static ULONG STDMETHODCALLTYPE dxgi_resource_AddRef(IDXGIResource1 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    TRACE("Forwarding to outer IUnknown\n");
    return IUnknown_AddRef(resource->outer_unknown);
}

static ULONG STDMETHODCALLTYPE dxgi_resource_Release(IDXGIResource1 *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    TRACE("Forwarding to outer IUnknown\n");
    return IUnknown_Release(resource->outer_unknown);
}

/* IDXGIObject methods */

static HRESULT STDMETHODCALLTYPE dxgi_resource_SetPrivateData(IDXGIResource1 *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_set_private_data(&resource->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_SetPrivateDataInterface(IDXGIResource1 *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, guid %s, object %p.\n", iface, debugstr_guid(guid), object);

    return dxgi_set_private_data_interface(&resource->private_store, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetPrivateData(IDXGIResource1 *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_get_private_data(&resource->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetParent(IDXGIResource1 *iface, REFIID riid, void **parent)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, riid %s, parent %p.\n", iface, debugstr_guid(riid), parent);

    return IDXGIDevice_QueryInterface(resource->device, riid, parent);
}

/* IDXGIDeviceSubObject methods */

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetDevice(IDXGIResource1 *iface, REFIID riid, void **device)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return IDXGIDevice_QueryInterface(resource->device, riid, device);
}

/* IDXGIResource methods */
static HRESULT STDMETHODCALLTYPE dxgi_resource_GetSharedHandle(IDXGIResource1 *iface, HANDLE *shared_handle)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);

    TRACE("iface %p, shared_handle %p.\n", iface, shared_handle);

    if (!shared_handle)
        return E_INVALIDARG;
    *shared_handle = NULL;

    if (resource->misc_flags & DXGI_RESOURCE_MISC_SHARED_NTHANDLE)
        return E_INVALIDARG;
    if (!(resource->misc_flags & (DXGI_RESOURCE_MISC_SHARED | DXGI_RESOURCE_MISC_SHARED_KEYEDMUTEX)))
        return S_OK;

    return dxgi_shared_resource_get_handle(resource, shared_handle);
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetUsage(IDXGIResource1 *iface, DXGI_USAGE *usage)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    struct wined3d_resource_desc resource_desc;

    TRACE("iface %p, usage %p.\n", iface, usage);

    wined3d_resource_get_desc(resource->wined3d_resource, &resource_desc);

    *usage = dxgi_usage_from_wined3d_bind_flags(resource_desc.bind_flags);

    if (resource_desc.resource_type != WINED3D_RTYPE_BUFFER)
    {
        struct wined3d_texture *texture = wined3d_texture_from_resource(resource->wined3d_resource);
        struct wined3d_swapchain_desc swapchain_desc;
        struct wined3d_swapchain *swapchain;

        if ((swapchain = wined3d_texture_get_swapchain(texture)))
        {
            *usage |= DXGI_USAGE_BACK_BUFFER;

            wined3d_swapchain_get_desc(swapchain, &swapchain_desc);
            if (swapchain_desc.swap_effect == WINED3D_SWAP_EFFECT_DISCARD)
                *usage |= DXGI_USAGE_DISCARD_ON_PRESENT;

            if (wined3d_swapchain_get_back_buffer(swapchain, 0) != texture)
                *usage |= DXGI_USAGE_READ_ONLY;
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_SetEvictionPriority(IDXGIResource1 *iface, UINT eviction_priority)
{
    FIXME("iface %p, eviction_priority %u stub!\n", iface, eviction_priority);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_GetEvictionPriority(IDXGIResource1 *iface, UINT *eviction_priority)
{
    FIXME("iface %p, eviction_priority %p stub!\n", iface, eviction_priority);

    return E_NOTIMPL;
}

/* IDXGIResource1 methods */
static HRESULT STDMETHODCALLTYPE dxgi_resource_CreateSubresourceSurface(IDXGIResource1 *iface,
        UINT index, IDXGISurface2 **surface)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface), *subresource;
    struct wined3d_resource_desc desc;
    unsigned int subresource_count;
    HRESULT hr;

    TRACE("iface %p, index %u, surface %p.\n", iface, index, surface);

    wined3d_mutex_lock();
    wined3d_resource_get_desc(resource->wined3d_resource, &desc);
    subresource_count = wined3d_resource_get_sub_resource_count(resource->wined3d_resource);
    wined3d_mutex_unlock();

    if ((desc.resource_type == WINED3D_RTYPE_TEXTURE_3D && desc.depth > 1)
            || index >= subresource_count)
        return E_INVALIDARG;

    if (!(subresource = calloc(1, sizeof(*subresource))))
    {
        ERR("Failed to allocate DXGI subresource surface object memory.\n");
        return E_OUTOFMEMORY;
    }

    if (FAILED(hr = dxgi_resource_init(subresource, resource->device, NULL, TRUE,
            resource->wined3d_resource, iface, index, resource->misc_flags)))
    {
        WARN("Failed to initialise resource, hr %#lx.\n", hr);
        free(subresource);
        return hr;
    }

    *surface = &subresource->IDXGISurface2_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_resource_CreateSharedHandle(IDXGIResource1 *iface,
        const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle)
{
    struct dxgi_resource *resource = impl_from_IDXGIResource1(iface);
    WCHAR *object_name = NULL;
    HRESULT hr;

    TRACE("iface %p, attributes %p, access %#lx, name %s, handle %p.\n", iface, attributes,
            access, wine_dbgstr_w(name), handle);

    if (!handle)
        return E_INVALIDARG;
    *handle = NULL;

    if (!(resource->misc_flags & DXGI_RESOURCE_MISC_SHARED_NTHANDLE))
        return E_INVALIDARG;

    if (FAILED(hr = dxgi_shared_resource_name(resource, name, &object_name)))
        return hr;

    if (SUCCEEDED(hr = dxgi_resource_create_shared_nt_handle(resource, attributes, access, object_name, handle)))
    {
        if (FAILED(hr = dxgi_shared_resource_register_handle(resource, *handle, TRUE, object_name)))
        {
            CloseHandle(*handle);
            *handle = NULL;
        }
        else
        {
            TRACE("Created NT shared handle %p for resource %p, name %s.\n",
                    *handle, resource, debugstr_w(object_name));
            return S_OK;
        }
    }

    free(object_name);
    return hr;
}

static const struct IDXGIResource1Vtbl dxgi_resource_vtbl =
{
    /* IUnknown methods */
    dxgi_resource_QueryInterface,
    dxgi_resource_AddRef,
    dxgi_resource_Release,
    /* IDXGIObject methods */
    dxgi_resource_SetPrivateData,
    dxgi_resource_SetPrivateDataInterface,
    dxgi_resource_GetPrivateData,
    dxgi_resource_GetParent,
    /* IDXGIDeviceSubObject methods */
    dxgi_resource_GetDevice,
    /* IDXGIResource methods */
    dxgi_resource_GetSharedHandle,
    dxgi_resource_GetUsage,
    dxgi_resource_SetEvictionPriority,
    dxgi_resource_GetEvictionPriority,
    /* IDXGIResource1 methods */
    dxgi_resource_CreateSubresourceSurface,
    dxgi_resource_CreateSharedHandle,
};

static const struct IUnknownVtbl dxgi_resource_inner_unknown_vtbl =
{
    /* IUnknown methods */
    dxgi_resource_inner_QueryInterface,
    dxgi_resource_inner_AddRef,
    dxgi_resource_inner_Release,
};

static inline struct dxgi_resource *impl_from_IDXGIKeyedMutex(IDXGIKeyedMutex *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_resource, IDXGIKeyedMutex_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_QueryInterface(IDXGIKeyedMutex *iface,
        REFIID riid, void **object)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    return IDXGIResource1_QueryInterface(&resource->IDXGIResource1_iface, riid, object);
}

static ULONG STDMETHODCALLTYPE dxgi_keyed_mutex_AddRef(IDXGIKeyedMutex *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    return IDXGIResource1_AddRef(&resource->IDXGIResource1_iface);
}

static ULONG STDMETHODCALLTYPE dxgi_keyed_mutex_Release(IDXGIKeyedMutex *iface)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    return IDXGIResource1_Release(&resource->IDXGIResource1_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_SetPrivateData(IDXGIKeyedMutex *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    return IDXGIResource1_SetPrivateData(&resource->IDXGIResource1_iface, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_SetPrivateDataInterface(IDXGIKeyedMutex *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    return IDXGIResource1_SetPrivateDataInterface(&resource->IDXGIResource1_iface, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_GetPrivateData(IDXGIKeyedMutex *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    return IDXGIResource1_GetPrivateData(&resource->IDXGIResource1_iface, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_GetParent(IDXGIKeyedMutex *iface,
        REFIID riid, void **parent)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    return IDXGIResource1_GetParent(&resource->IDXGIResource1_iface, riid, parent);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_GetDevice(IDXGIKeyedMutex *iface,
        REFIID riid, void **device)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    return IDXGIResource1_GetDevice(&resource->IDXGIResource1_iface, riid, device);
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_AcquireSync(IDXGIKeyedMutex *iface,
        UINT64 key, DWORD milliseconds)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);
    DWORD deadline = 0;

    TRACE("iface %p, key %s, milliseconds %#lx.\n", iface, wine_dbgstr_longlong(key), milliseconds);

    if (milliseconds != INFINITE)
        deadline = GetTickCount() + milliseconds;

    EnterCriticalSection(&resource->keyed_mutex_cs);
    for (;;)
    {
        if (!resource->keyed_mutex_acquired && resource->keyed_mutex_key == key)
        {
            resource->keyed_mutex_acquired = TRUE;
            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return S_OK;
        }

        if (!milliseconds)
        {
            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return WAIT_TIMEOUT;
        }

        if (milliseconds != INFINITE)
        {
            DWORD now = GetTickCount();

            if ((LONG)(deadline - now) <= 0)
            {
                LeaveCriticalSection(&resource->keyed_mutex_cs);
                return WAIT_TIMEOUT;
            }
            milliseconds = deadline - now;
        }

        if (!SleepConditionVariableCS(&resource->keyed_mutex_cv,
                &resource->keyed_mutex_cs, milliseconds))
        {
            DWORD error = GetLastError();

            if (error == ERROR_TIMEOUT)
            {
                LeaveCriticalSection(&resource->keyed_mutex_cs);
                return WAIT_TIMEOUT;
            }

            LeaveCriticalSection(&resource->keyed_mutex_cs);
            return HRESULT_FROM_WIN32(error);
        }
    }
}

static HRESULT STDMETHODCALLTYPE dxgi_keyed_mutex_ReleaseSync(IDXGIKeyedMutex *iface, UINT64 key)
{
    struct dxgi_resource *resource = impl_from_IDXGIKeyedMutex(iface);

    TRACE("iface %p, key %s.\n", iface, wine_dbgstr_longlong(key));

    EnterCriticalSection(&resource->keyed_mutex_cs);
    if (!resource->keyed_mutex_acquired)
    {
        LeaveCriticalSection(&resource->keyed_mutex_cs);
        return DXGI_ERROR_INVALID_CALL;
    }

    resource->keyed_mutex_key = key;
    resource->keyed_mutex_acquired = FALSE;
    WakeAllConditionVariable(&resource->keyed_mutex_cv);
    LeaveCriticalSection(&resource->keyed_mutex_cs);

    return S_OK;
}

static const struct IDXGIKeyedMutexVtbl dxgi_keyed_mutex_vtbl =
{
    /* IUnknown methods */
    dxgi_keyed_mutex_QueryInterface,
    dxgi_keyed_mutex_AddRef,
    dxgi_keyed_mutex_Release,
    /* IDXGIObject methods */
    dxgi_keyed_mutex_SetPrivateData,
    dxgi_keyed_mutex_SetPrivateDataInterface,
    dxgi_keyed_mutex_GetPrivateData,
    dxgi_keyed_mutex_GetParent,
    /* IDXGIDeviceSubObject methods */
    dxgi_keyed_mutex_GetDevice,
    /* IDXGIKeyedMutex methods */
    dxgi_keyed_mutex_AcquireSync,
    dxgi_keyed_mutex_ReleaseSync,
};

HRESULT dxgi_resource_init(struct dxgi_resource *resource, IDXGIDevice *device,
        IUnknown *outer, BOOL needs_surface, struct wined3d_resource *wined3d_resource,
        IDXGIResource1 *parent_resource, unsigned int subresource_index, unsigned int misc_flags)
{
    struct wined3d_resource_desc desc;
    bool is_subresource;

    is_subresource = !!parent_resource;
    wined3d_resource_get_desc(wined3d_resource, &desc);
    if (((desc.resource_type == WINED3D_RTYPE_TEXTURE_1D || desc.resource_type == WINED3D_RTYPE_TEXTURE_2D)
            && needs_surface) || is_subresource)
    {
        resource->IDXGISurface2_iface.lpVtbl = &dxgi_surface_vtbl;
    }
    else
        resource->IDXGISurface2_iface.lpVtbl = NULL;
    resource->IDXGIResource1_iface.lpVtbl = &dxgi_resource_vtbl;
    resource->IDXGIKeyedMutex_iface.lpVtbl = &dxgi_keyed_mutex_vtbl;
    resource->IUnknown_iface.lpVtbl = &dxgi_resource_inner_unknown_vtbl;
    resource->refcount = 1;
    wined3d_private_store_init(&resource->private_store);
    resource->outer_unknown = outer ? outer : &resource->IUnknown_iface;
    resource->device = device;
    resource->wined3d_resource = wined3d_resource;
    resource->misc_flags = misc_flags;
    resource->shared_handle = NULL;
    if (misc_flags & DXGI_RESOURCE_MISC_SHARED_KEYEDMUTEX)
    {
        InitializeCriticalSection(&resource->keyed_mutex_cs);
        InitializeConditionVariable(&resource->keyed_mutex_cv);
        resource->keyed_mutex_key = 0;
        resource->keyed_mutex_acquired = FALSE;
    }
    resource->dc = NULL;
    if (is_subresource)
    {
        resource->parent_resource = parent_resource;
        IDXGIResource1_AddRef(parent_resource);
        resource->subresource_idx = subresource_index;
    }
    else
    {
        resource->parent_resource = NULL;
        resource->subresource_idx = 0;
    }

    return S_OK;
}
