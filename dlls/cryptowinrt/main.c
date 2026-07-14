/*
 * Copyright 2022 Nikolay Sivov for CodeWeavers
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

#include "initguid.h"
#include "private.h"

#include <assert.h>

#include "wine/debug.h"
#include "objbase.h"

#include "bcrypt.h"
#include "wincrypt.h"

#define WIDL_using_Windows_Security_Cryptography
#include "windows.security.cryptography.h"
#include "robuffer.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypto);

struct cryptobuffer_factory
{
    IActivationFactory IActivationFactory_iface;
    ICryptographicBufferStatics ICryptographicBufferStatics_iface;
    LONG refcount;
};

static inline struct cryptobuffer_factory *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct cryptobuffer_factory, IActivationFactory_iface);
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_QueryInterface(
        IActivationFactory *iface, REFIID iid, void **out)
{
    struct cryptobuffer_factory *factory = impl_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) ||
        IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IUnknown_AddRef(iface);
        *out = &factory->IActivationFactory_iface;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_ICryptographicBufferStatics))
    {
        IUnknown_AddRef(iface);
        *out = &factory->ICryptographicBufferStatics_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE cryptobuffer_factory_AddRef(IActivationFactory *iface)
{
    struct cryptobuffer_factory *factory = impl_from_IActivationFactory(iface);
    ULONG refcount = InterlockedIncrement(&factory->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE cryptobuffer_factory_Release(IActivationFactory *iface)
{
    struct cryptobuffer_factory *factory = impl_from_IActivationFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_GetIids(
        IActivationFactory *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_GetRuntimeClassName(
        IActivationFactory *iface, HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_GetTrustLevel(
        IActivationFactory *iface, TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_ActivateInstance(
        IActivationFactory *iface, IInspectable **instance)
{
    FIXME("iface %p, instance %p stub!\n", iface, instance);
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl cryptobuffer_factory_vtbl =
{
    cryptobuffer_factory_QueryInterface,
    cryptobuffer_factory_AddRef,
    cryptobuffer_factory_Release,
    /* IInspectable methods */
    cryptobuffer_factory_GetIids,
    cryptobuffer_factory_GetRuntimeClassName,
    cryptobuffer_factory_GetTrustLevel,
    /* IActivationFactory methods */
    cryptobuffer_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE(cryptobuffer_statics, ICryptographicBufferStatics, struct cryptobuffer_factory, IActivationFactory_iface);

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_Compare(
        ICryptographicBufferStatics *iface, IBuffer *object1, IBuffer *object2, boolean *is_equal)
{
    FIXME("iface %p, object1 %p, object2 %p, is_equal %p stub!\n", iface, object1, object2, is_equal);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_GenerateRandom(
        ICryptographicBufferStatics *iface, UINT32 length, IBuffer **buffer)
{
    FIXME("iface %p, length %u, buffer %p stub!\n", iface, length, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_GenerateRandomNumber(
        ICryptographicBufferStatics *iface, UINT32 *value)
{
    TRACE("iface %p, value %p.\n", iface, value);

    BCryptGenRandom(NULL, (UCHAR *)value, sizeof(*value), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_CreateFromByteArray(
        ICryptographicBufferStatics *iface, UINT32 value_size, BYTE *value, IBuffer **buffer)
{
    FIXME("iface %p, value_size %u, value %p, buffer %p stub!\n", iface, value_size, value, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_CopyToByteArray(
        ICryptographicBufferStatics *iface, IBuffer *buffer, UINT32 *value_size, BYTE **value)
{
    FIXME("iface %p, buffer %p, value_size %p, value %p stub!\n", iface, buffer, value_size, value);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_DecodeFromHexString(
        ICryptographicBufferStatics *iface, HSTRING value, IBuffer **buffer)
{
    FIXME("iface %p, value %p, buffer %p stub!\n", iface, value, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_EncodeToHexString(
        ICryptographicBufferStatics *iface, IBuffer *buffer, HSTRING *value)
{
    FIXME("iface %p, buffer %p, value %p stub!\n", iface, buffer, value);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_DecodeFromBase64String(
        ICryptographicBufferStatics *iface, HSTRING value, IBuffer **buffer)
{
    FIXME("iface %p, value %p, buffer %p stub!\n", iface, value, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_EncodeToBase64String(
        ICryptographicBufferStatics *iface, IBuffer *buffer, HSTRING *value)
{
    IBufferByteAccess *buffer_access;
    HSTRING_BUFFER str_buffer;
    void *data = NULL;
    UINT32 length = 0;
    DWORD ret_length;
    WCHAR *str;
    HRESULT hr;

    TRACE("iface %p, buffer %p, value %p.\n", iface, buffer, value);

    if (buffer)
    {
        IBuffer_get_Length(buffer, &length);
        if (length)
        {
            if (SUCCEEDED(IBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void **)&buffer_access)))
            {
                IBufferByteAccess_Buffer(buffer_access, (byte **)&data);
                IBufferByteAccess_Release(buffer_access);
            }
        }
    }

    if (!length)
        return WindowsCreateString(NULL, 0, value);

    if (!data)
        return E_FAIL;

    if (!CryptBinaryToStringW(data, length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &ret_length))
        return E_FAIL;

    if (FAILED(hr = WindowsPreallocateStringBuffer(ret_length, &str, &str_buffer)))
        return hr;

    if (!CryptBinaryToStringW(data, length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, str, &ret_length))
    {
        WindowsDeleteStringBuffer(str_buffer);
        return E_FAIL;
    }

    return WindowsPromoteStringBuffer(str_buffer, value);
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_ConvertStringToBinary(
        ICryptographicBufferStatics *iface, HSTRING value, BinaryStringEncoding encoding,
        IBuffer **buffer)
{
    FIXME("iface %p, value %p, encoding %d, buffer %p stub!\n", iface, value, encoding, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_ConvertBinaryToString(
        ICryptographicBufferStatics *iface, BinaryStringEncoding encoding, IBuffer *buffer, HSTRING *value)
{
    FIXME("iface %p, encoding %d, buffer %p, value %p stub!\n", iface, encoding, buffer, value);

    return E_NOTIMPL;
}

static const struct ICryptographicBufferStaticsVtbl cryptobuffer_statics_vtbl =
{
    cryptobuffer_statics_QueryInterface,
    cryptobuffer_statics_AddRef,
    cryptobuffer_statics_Release,
    /* IInspectable methods */
    cryptobuffer_statics_GetIids,
    cryptobuffer_statics_GetRuntimeClassName,
    cryptobuffer_statics_GetTrustLevel,
    /* ICryptographicBufferStatics methods */
    cryptobuffer_statics_Compare,
    cryptobuffer_statics_GenerateRandom,
    cryptobuffer_statics_GenerateRandomNumber,
    cryptobuffer_statics_CreateFromByteArray,
    cryptobuffer_statics_CopyToByteArray,
    cryptobuffer_statics_DecodeFromHexString,
    cryptobuffer_statics_EncodeToHexString,
    cryptobuffer_statics_DecodeFromBase64String,
    cryptobuffer_statics_EncodeToBase64String,
    cryptobuffer_statics_ConvertStringToBinary,
    cryptobuffer_statics_ConvertBinaryToString,
};

static struct cryptobuffer_factory cryptobuffer_factory =
{
    .IActivationFactory_iface.lpVtbl = &cryptobuffer_factory_vtbl,
    .ICryptographicBufferStatics_iface.lpVtbl = &cryptobuffer_statics_vtbl,
    .refcount = 1,
};

IActivationFactory *cryptobuffer_activation_factory = &cryptobuffer_factory.IActivationFactory_iface;

struct appcapability_factory
{
    IActivationFactory IActivationFactory_iface;
    IAppCapabilityStatics IAppCapabilityStatics_iface;
    LONG refcount;
};

struct appcapability
{
    IAppCapability IAppCapability_iface;
    LONG refcount;
    HSTRING capability_name;
};

static inline struct appcapability_factory *impl_from_appcapability_factory_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct appcapability_factory, IActivationFactory_iface);
}

static inline struct appcapability *impl_from_IAppCapability(IAppCapability *iface)
{
    return CONTAINING_RECORD(iface, struct appcapability, IAppCapability_iface);
}

static HRESULT appcapability_get_class_name(HSTRING *class_name)
{
    return WindowsCreateString(RuntimeClass_Windows_Security_Authorization_AppCapabilityAccess_AppCapability,
            wcslen(RuntimeClass_Windows_Security_Authorization_AppCapabilityAccess_AppCapability), class_name);
}

static HRESULT WINAPI appcapability_factory_QueryInterface(IActivationFactory *iface, REFIID iid, void **out)
{
    struct appcapability_factory *factory = impl_from_appcapability_factory_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) ||
        IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IUnknown_AddRef(iface);
        *out = &factory->IActivationFactory_iface;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IAppCapabilityStatics))
    {
        IUnknown_AddRef(iface);
        *out = &factory->IAppCapabilityStatics_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI appcapability_factory_AddRef(IActivationFactory *iface)
{
    struct appcapability_factory *factory = impl_from_appcapability_factory_IActivationFactory(iface);
    ULONG refcount = InterlockedIncrement(&factory->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI appcapability_factory_Release(IActivationFactory *iface)
{
    struct appcapability_factory *factory = impl_from_appcapability_factory_IActivationFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static HRESULT WINAPI appcapability_factory_GetIids(IActivationFactory *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI appcapability_factory_GetRuntimeClassName(IActivationFactory *iface, HSTRING *class_name)
{
    TRACE("iface %p, class_name %p.\n", iface, class_name);
    return appcapability_get_class_name(class_name);
}

static HRESULT WINAPI appcapability_factory_GetTrustLevel(IActivationFactory *iface, TrustLevel *trust_level)
{
    TRACE("iface %p, trust_level %p.\n", iface, trust_level);
    *trust_level = BaseTrust;
    return S_OK;
}

static const struct IAppCapabilityVtbl appcapability_vtbl;

static HRESULT appcapability_create(HSTRING capability_name, IAppCapability **out)
{
    struct appcapability *impl;
    HRESULT hr;

    if (!(impl = calloc(1, sizeof(*impl))))
        return E_OUTOFMEMORY;

    impl->IAppCapability_iface.lpVtbl = &appcapability_vtbl;
    impl->refcount = 1;

    if (FAILED(hr = WindowsDuplicateString(capability_name, &impl->capability_name)))
    {
        free(impl);
        return hr;
    }

    *out = &impl->IAppCapability_iface;
    return S_OK;
}

static HRESULT WINAPI appcapability_factory_ActivateInstance(IActivationFactory *iface, IInspectable **instance)
{
    FIXME("iface %p, instance %p stub!\n", iface, instance);
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl appcapability_factory_vtbl =
{
    appcapability_factory_QueryInterface,
    appcapability_factory_AddRef,
    appcapability_factory_Release,
    /* IInspectable methods */
    appcapability_factory_GetIids,
    appcapability_factory_GetRuntimeClassName,
    appcapability_factory_GetTrustLevel,
    /* IActivationFactory methods */
    appcapability_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE(appcapability_statics, IAppCapabilityStatics, struct appcapability_factory, IActivationFactory_iface);

static HRESULT WINAPI appcapability_statics_RequestAccessForCapabilitiesAsync(IAppCapabilityStatics *iface,
        IIterable_HSTRING *capability_names, IAsyncOperation_IMapView_HSTRING_AppCapabilityAccessStatus **operation)
{
    FIXME("iface %p, capability_names %p, operation %p stub!\n", iface, capability_names, operation);
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI appcapability_statics_RequestAccessForCapabilitiesForUserAsync(IAppCapabilityStatics *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, IIterable_HSTRING *capability_names,
        IAsyncOperation_IMapView_HSTRING_AppCapabilityAccessStatus **operation)
{
    FIXME("iface %p, user %p, capability_names %p, operation %p stub!\n", iface, user, capability_names, operation);
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI appcapability_statics_Create(IAppCapabilityStatics *iface, HSTRING capability_name,
        IAppCapability **result)
{
    TRACE("iface %p, capability_name %s, result %p.\n", iface, debugstr_hstring(capability_name), result);
    return appcapability_create(capability_name, result);
}

static HRESULT WINAPI appcapability_statics_CreateWithProcessIdForUser(IAppCapabilityStatics *iface,
        __x_ABI_CWindows_CSystem_CIUser *user, HSTRING capability_name, UINT32 pid, IAppCapability **result)
{
    TRACE("iface %p, user %p, capability_name %s, pid %u, result %p.\n",
            iface, user, debugstr_hstring(capability_name), pid, result);
    return appcapability_create(capability_name, result);
}

static const struct IAppCapabilityStaticsVtbl appcapability_statics_vtbl =
{
    appcapability_statics_QueryInterface,
    appcapability_statics_AddRef,
    appcapability_statics_Release,
    /* IInspectable methods */
    appcapability_statics_GetIids,
    appcapability_statics_GetRuntimeClassName,
    appcapability_statics_GetTrustLevel,
    /* IAppCapabilityStatics methods */
    appcapability_statics_RequestAccessForCapabilitiesAsync,
    appcapability_statics_RequestAccessForCapabilitiesForUserAsync,
    appcapability_statics_Create,
    appcapability_statics_CreateWithProcessIdForUser,
};

static HRESULT WINAPI appcapability_QueryInterface(IAppCapability *iface, REFIID iid, void **out)
{
    struct appcapability *impl = impl_from_IAppCapability(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) ||
        IsEqualGUID(iid, &IID_IAppCapability))
    {
        IAppCapability_AddRef(iface);
        *out = &impl->IAppCapability_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI appcapability_AddRef(IAppCapability *iface)
{
    struct appcapability *impl = impl_from_IAppCapability(iface);
    ULONG refcount = InterlockedIncrement(&impl->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI appcapability_Release(IAppCapability *iface)
{
    struct appcapability *impl = impl_from_IAppCapability(iface);
    ULONG refcount = InterlockedDecrement(&impl->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        WindowsDeleteString(impl->capability_name);
        free(impl);
    }

    return refcount;
}

static HRESULT WINAPI appcapability_GetIids(IAppCapability *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI appcapability_GetRuntimeClassName(IAppCapability *iface, HSTRING *class_name)
{
    TRACE("iface %p, class_name %p.\n", iface, class_name);
    return appcapability_get_class_name(class_name);
}

static HRESULT WINAPI appcapability_GetTrustLevel(IAppCapability *iface, TrustLevel *trust_level)
{
    TRACE("iface %p, trust_level %p.\n", iface, trust_level);
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI appcapability_get_CapabilityName(IAppCapability *iface, HSTRING *value)
{
    struct appcapability *impl = impl_from_IAppCapability(iface);

    TRACE("iface %p, value %p.\n", iface, value);

    return WindowsDuplicateString(impl->capability_name, value);
}

static HRESULT WINAPI appcapability_get_User(IAppCapability *iface, __x_ABI_CWindows_CSystem_CIUser **value)
{
    TRACE("iface %p, value %p.\n", iface, value);

    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI appcapability_RequestAccessAsync(IAppCapability *iface,
        IAsyncOperation_AppCapabilityAccessStatus **operation)
{
    FIXME("iface %p, operation %p stub!\n", iface, operation);
    *operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI appcapability_CheckAccess(IAppCapability *iface, AppCapabilityAccessStatus *result)
{
    TRACE("iface %p, result %p.\n", iface, result);

    *result = AppCapabilityAccessStatus_DeniedBySystem;
    return S_OK;
}

static HRESULT WINAPI appcapability_add_AccessChanged(IAppCapability *iface,
        ITypedEventHandler_AppCapability_AppCapabilityAccessChangedEventArgs *handler, EventRegistrationToken *token)
{
    static LONG next_token;

    FIXME("iface %p, handler %p, token %p stub!\n", iface, handler, token);

    token->value = InterlockedIncrement(&next_token);
    return S_OK;
}

static HRESULT WINAPI appcapability_remove_AccessChanged(IAppCapability *iface, EventRegistrationToken token)
{
    FIXME("iface %p, token %#I64x stub!\n", iface, token.value);
    return S_OK;
}

static const struct IAppCapabilityVtbl appcapability_vtbl =
{
    appcapability_QueryInterface,
    appcapability_AddRef,
    appcapability_Release,
    /* IInspectable methods */
    appcapability_GetIids,
    appcapability_GetRuntimeClassName,
    appcapability_GetTrustLevel,
    /* IAppCapability methods */
    appcapability_get_CapabilityName,
    appcapability_get_User,
    appcapability_RequestAccessAsync,
    appcapability_CheckAccess,
    appcapability_add_AccessChanged,
    appcapability_remove_AccessChanged,
};

static struct appcapability_factory appcapability_factory =
{
    .IActivationFactory_iface.lpVtbl = &appcapability_factory_vtbl,
    .IAppCapabilityStatics_iface.lpVtbl = &appcapability_statics_vtbl,
    .refcount = 1,
};

IActivationFactory *appcapability_activation_factory = &appcapability_factory.IActivationFactory_iface;

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    FIXME("clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out);
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const WCHAR *name = WindowsGetStringRawBuffer(classid, NULL);

    TRACE("classid %s, factory %p.\n", debugstr_hstring(classid), factory);

    *factory = NULL;

    if (!wcscmp(name, RuntimeClass_Windows_Security_Cryptography_CryptographicBuffer))
        IActivationFactory_QueryInterface(cryptobuffer_activation_factory, &IID_IActivationFactory, (void **)factory);
    if (!wcscmp(name, RuntimeClass_Windows_Security_Credentials_KeyCredentialManager))
        IActivationFactory_QueryInterface(credentials_activation_factory, &IID_IActivationFactory, (void **)factory);
    if (!wcscmp(name, RuntimeClass_Windows_Security_Authorization_AppCapabilityAccess_AppCapability))
        IActivationFactory_QueryInterface(appcapability_activation_factory, &IID_IActivationFactory, (void **)factory);

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}
