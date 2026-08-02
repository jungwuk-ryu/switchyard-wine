/*
 * Copyright 2021 Arkadiusz Hiler for CodeWeavers
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

#include <math.h>
#include <stdio.h>

#include "wine/test.h"

#define COBJMACROS

#ifdef STANDALONE
#include "initguid.h"
#endif

#include "mmdeviceapi.h"
#include "spatialaudioclient.h"
#include "mmsystem.h"

static IMMDeviceEnumerator *mme = NULL;
static IMMDevice *dev = NULL;
static ISpatialAudioClient *sac = NULL;
static UINT32 max_dyn_count;
static HANDLE event;
static WAVEFORMATEX format;
static BOOL spatial_stream_supported = TRUE;

#define ALL_STATIC_OBJECTS (AudioObjectType_FrontLeft | AudioObjectType_FrontRight | \
        AudioObjectType_FrontCenter | AudioObjectType_LowFrequency | AudioObjectType_SideLeft | \
        AudioObjectType_SideRight | AudioObjectType_BackLeft | AudioObjectType_BackRight | \
        AudioObjectType_TopFrontLeft | AudioObjectType_TopFrontRight | AudioObjectType_TopBackLeft | \
        AudioObjectType_TopBackRight | AudioObjectType_BottomFrontLeft | \
        AudioObjectType_BottomFrontRight | AudioObjectType_BottomBackLeft | \
        AudioObjectType_BottomBackRight | AudioObjectType_BackCenter)

static void test_formats(void)
{
    HRESULT hr;
    IAudioFormatEnumerator *afe;
    UINT32 format_count = 0;
    WAVEFORMATEX *fmt = NULL;

    hr = ISpatialAudioClient_GetSupportedAudioObjectFormatEnumerator(sac, &afe);
    ok(hr == S_OK, "Getting format enumerator failed: 0x%08lx\n", hr);

    hr = IAudioFormatEnumerator_GetCount(afe, &format_count);
    ok(hr == S_OK, "Getting format count failed: 0x%08lx\n", hr);
    ok(format_count == 1, "Got wrong format count, expected 1 got %u\n", format_count);

    hr = IAudioFormatEnumerator_GetFormat(afe, 0, &fmt);
    ok(hr == S_OK, "Getting format failed: 0x%08lx\n", hr);
    ok(fmt != NULL, "Expected to get non-NULL format\n");
    if (!fmt)
    {
        IAudioFormatEnumerator_Release(afe);
        return;
    }

    ok(fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT, "Wrong format, expected WAVE_FORMAT_IEEE_FLOAT got %hx\n", fmt->wFormatTag);
    ok(fmt->nChannels == 1, "Wrong number of channels, expected 1 got %hu\n", fmt->nChannels);
    ok(fmt->nSamplesPerSec == 48000, "Wrong sample ret, expected 48000 got %lu\n", fmt->nSamplesPerSec);
    ok(fmt->wBitsPerSample == 32, "Wrong bits per sample, expected 32 got %hu\n", fmt->wBitsPerSample);
    ok(fmt->nBlockAlign == 4, "Wrong block align, expected 4 got %hu\n", fmt->nBlockAlign);
    ok(fmt->nAvgBytesPerSec == 192000, "Wrong avg bytes per sec, expected 192000 got %lu\n", fmt->nAvgBytesPerSec);
    ok(fmt->cbSize == 0, "Wrong cbSize for simple format, expected 0, got %hu\n", fmt->cbSize);

    hr = ISpatialAudioClient_IsAudioObjectFormatSupported(sac, NULL);
    ok(hr == E_POINTER, "Got %#lx.\n", hr);

    memcpy(&format, fmt, sizeof(format));
    hr = ISpatialAudioClient_IsAudioObjectFormatSupported(sac, &format);
    ok(hr == S_OK, "Got %#lx.\n", hr);

    format.nBlockAlign *= 2;
    hr = ISpatialAudioClient_IsAudioObjectFormatSupported(sac, &format);
    todo_wine ok(hr == S_OK, "Got %#lx.\n", hr);

    memcpy(&format, fmt, sizeof(format));
    format.wBitsPerSample *= 2;
    hr = ISpatialAudioClient_IsAudioObjectFormatSupported(sac, &format);
    ok(hr == E_INVALIDARG, "Got %#lx.\n", hr);

    memcpy(&format, fmt, sizeof(format));
    format.nChannels = 2;
    hr = ISpatialAudioClient_IsAudioObjectFormatSupported(sac, &format);
    ok(hr == E_INVALIDARG, "Got %#lx.\n", hr);

    memcpy(&format, fmt, sizeof(format));

    IAudioFormatEnumerator_Release(afe);
}

static void test_static_object_properties(void)
{
    static const AudioObjectType types[] =
    {
        AudioObjectType_FrontLeft,
        AudioObjectType_FrontRight,
        AudioObjectType_FrontCenter,
        AudioObjectType_LowFrequency,
        AudioObjectType_SideLeft,
        AudioObjectType_SideRight,
        AudioObjectType_BackLeft,
        AudioObjectType_BackRight,
        AudioObjectType_TopFrontLeft,
        AudioObjectType_TopFrontRight,
        AudioObjectType_TopBackLeft,
        AudioObjectType_TopBackRight,
        AudioObjectType_BottomFrontLeft,
        AudioObjectType_BottomFrontRight,
        AudioObjectType_BottomBackLeft,
        AudioObjectType_BottomBackRight,
        AudioObjectType_BackCenter,
    };
    AudioObjectType mask;
    float x, y, z;
    unsigned int i;
    HRESULT hr;

    hr = ISpatialAudioClient_GetNativeStaticObjectTypeMask(sac, &mask);
    ok(hr == S_OK, "Failed to get native static object mask: %#lx.\n", hr);
    if (hr == S_OK)
    {
        ok(mask != AudioObjectType_None, "Expected at least one native static object.\n");
        ok(!(mask & AudioObjectType_Dynamic), "Got dynamic object in native mask %#x.\n", mask);
        ok(!(mask & ~ALL_STATIC_OBJECTS), "Got unknown static objects in native mask %#x.\n", mask);
        ok(mask == ALL_STATIC_OBJECTS, "Expected all static objects, got %#x.\n", mask);
    }

    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        x = y = z = NAN;
        hr = ISpatialAudioClient_GetStaticObjectPosition(sac, types[i], &x, &y, &z);
        ok(hr == S_OK, "Failed to get position for object %#x: %#lx.\n", types[i], hr);
        if (hr == S_OK)
            ok(isfinite(x) && isfinite(y) && isfinite(z),
                    "Got invalid position {%f, %f, %f} for object %#x.\n", x, y, z, types[i]);
    }

    hr = ISpatialAudioClient_GetStaticObjectPosition(sac, AudioObjectType_None, &x, &y, &z);
    ok(hr == E_INVALIDARG, "Got %#lx for a non-spatialized object.\n", hr);
    hr = ISpatialAudioClient_GetStaticObjectPosition(sac, AudioObjectType_Dynamic, &x, &y, &z);
    ok(hr == E_INVALIDARG, "Got %#lx for a dynamic object.\n", hr);
    hr = ISpatialAudioClient_GetStaticObjectPosition(sac,
            AudioObjectType_FrontLeft | AudioObjectType_FrontRight, &x, &y, &z);
    ok(hr == E_INVALIDARG, "Got %#lx for a combined object mask.\n", hr);
}

static void fill_activation_params(SpatialAudioObjectRenderStreamActivationParams *activation_params)
{
    activation_params->StaticObjectTypeMask = ALL_STATIC_OBJECTS;

    activation_params->MinDynamicObjectCount = 0;
    activation_params->MaxDynamicObjectCount = 0;
    activation_params->Category = AudioCategory_GameEffects;
    activation_params->EventHandle = event;
    activation_params->NotifyObject = NULL;

    activation_params->ObjectFormat = &format;
}

typedef struct NotifyObject
{
    ISpatialAudioObjectRenderStreamNotify ISpatialAudioObjectRenderStreamNotify_iface;
    LONG ref;
    LONG calls;
    BOOL expect_notification;
    BOOL reenter_stream;
    BOOL release_stream;
    HANDLE callback_event;
    LONGLONG deadline;
    UINT32 object_count;
} NotifyObject;

static WINAPI HRESULT notifyobj_QueryInterface(
        ISpatialAudioObjectRenderStreamNotify *This,
        REFIID riid,
        void **ppvObject)
{
    if (!ppvObject)
        return E_POINTER;
    *ppvObject = NULL;
    if (!IsEqualIID(riid, &IID_IUnknown) &&
            !IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStreamNotify))
        return E_NOINTERFACE;
    *ppvObject = This;
    ISpatialAudioObjectRenderStreamNotify_AddRef(This);
    return S_OK;
}

static WINAPI ULONG notifyobj_AddRef(
        ISpatialAudioObjectRenderStreamNotify *This)
{
    NotifyObject *obj = CONTAINING_RECORD(This, NotifyObject, ISpatialAudioObjectRenderStreamNotify_iface);
    ULONG ref = InterlockedIncrement(&obj->ref);
    return ref;
}

static WINAPI ULONG notifyobj_Release(
        ISpatialAudioObjectRenderStreamNotify *This)
{
    NotifyObject *obj = CONTAINING_RECORD(This, NotifyObject, ISpatialAudioObjectRenderStreamNotify_iface);
    ULONG ref = InterlockedDecrement(&obj->ref);
    return ref;
}

static WINAPI HRESULT notifyobj_OnAvailableDynamicObjectCountChange(
        ISpatialAudioObjectRenderStreamNotify *This,
        ISpatialAudioObjectRenderStreamBase *stream,
        LONGLONG deadline,
        UINT32 object_count)
{
    NotifyObject *obj = CONTAINING_RECORD(This, NotifyObject,
            ISpatialAudioObjectRenderStreamNotify_iface);

    if (!obj->expect_notification)
        ok(FALSE, "Expected to never be notified of dynamic object count change\n");
    obj->deadline = deadline;
    obj->object_count = object_count;
    InterlockedIncrement(&obj->calls);
    if (obj->release_stream)
        IUnknown_Release((IUnknown *)stream);
    else if (obj->reenter_stream)
    {
        IUnknown_AddRef((IUnknown *)stream);
        IUnknown_Release((IUnknown *)stream);
    }
    if (obj->callback_event)
        SetEvent(obj->callback_event);
    return S_OK;
}

static HRESULT activate_fault_stream(const char *fault_name, NotifyObject *notify,
        ISpatialAudioObjectRenderStream **stream)
{
    SpatialAudioObjectRenderStreamActivationParams params;
    PROPVARIANT prop;
    char *old_fault = NULL;
    DWORD old_fault_len;
    BOOL had_old_fault;
    HRESULT hr;

    *stream = NULL;
    SetLastError(ERROR_SUCCESS);
    old_fault_len = GetEnvironmentVariableA(fault_name, NULL, 0);
    had_old_fault = old_fault_len || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    if (old_fault_len)
    {
        old_fault = malloc(old_fault_len);
        if (!old_fault)
            return E_OUTOFMEMORY;
        GetEnvironmentVariableA(fault_name, old_fault, old_fault_len);
    }
    if (!SetEnvironmentVariableA(fault_name, "1"))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        free(old_fault);
        return hr;
    }

    fill_activation_params(&params);
    params.StaticObjectTypeMask = AudioObjectType_None;
    params.MinDynamicObjectCount = 1;
    params.MaxDynamicObjectCount = 1;
    params.NotifyObject = &notify->ISpatialAudioObjectRenderStreamNotify_iface;
    PropVariantInit(&prop);
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = (BYTE *)&params;

    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)stream);
    if (had_old_fault)
        SetEnvironmentVariableA(fault_name, old_fault ? old_fault : "");
    else
        SetEnvironmentVariableA(fault_name, NULL);
    free(old_fault);
    return hr;
}

static const ISpatialAudioObjectRenderStreamNotifyVtbl notifyobjvtbl =
{
    notifyobj_QueryInterface,
    notifyobj_AddRef,
    notifyobj_Release,
    notifyobj_OnAvailableDynamicObjectCountChange
};

static void test_stream_activation(void)
{
    HRESULT hr;
    WAVEFORMATEX wrong_format;
    ISpatialAudioObjectRenderStream *sas = NULL;
    ISpatialAudioObject *object;

    SpatialAudioObjectRenderStreamActivationParams activation_params;
    PROPVARIANT activation_params_prop;
    NotifyObject notify_object;

    PropVariantInit(&activation_params_prop);
    activation_params_prop.vt = VT_BLOB;
    activation_params_prop.blob.cbSize = sizeof(activation_params);
    activation_params_prop.blob.pBlobData = (BYTE*) &activation_params;

    fill_activation_params(&activation_params);
    activation_params_prop.vt = VT_EMPTY;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
    ok(hr == E_INVALIDARG, "Expected a non-blob activation value to fail: %#lx.\n", hr);
    ok(!sas, "Expected no stream for a malformed activation value.\n");

    activation_params_prop.vt = VT_BLOB;
    activation_params_prop.blob.cbSize = sizeof(activation_params) - 1;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
    ok(hr == E_INVALIDARG, "Expected a truncated activation blob to fail: %#lx.\n", hr);
    ok(!sas, "Expected no stream for a truncated activation blob.\n");

    activation_params_prop.blob.cbSize = sizeof(activation_params);
    activation_params_prop.blob.pBlobData = NULL;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
    ok(hr == E_INVALIDARG, "Expected a null activation blob to fail: %#lx.\n", hr);
    ok(!sas, "Expected no stream for a null activation blob.\n");
    activation_params_prop.blob.pBlobData = (BYTE *)&activation_params;

    /* correct params */
    fill_activation_params(&activation_params);
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop, &IID_ISpatialAudioObjectRenderStream, (void**)&sas);
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
    {
        win_skip("The audio backend does not support spatial render streams.\n");
        spatial_stream_supported = FALSE;
    }
    else
    {
        ok(hr == S_OK, "Failed to activate spatial audio stream: 0x%08lx\n", hr);
        spatial_stream_supported = hr == S_OK;
        if (hr == S_OK)
            ok(ISpatialAudioObjectRenderStream_Release(sas) == 0,
                    "Expected to release the last reference\n");
    }
    sas = NULL;

    /* AudioObjectType_None is independent of the static spatial channel mask. */
    if (spatial_stream_supported)
    {
        fill_activation_params(&activation_params);
        activation_params.StaticObjectTypeMask = AudioObjectType_None;
        hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
                &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
        ok(hr == S_OK, "Failed to activate a non-spatial-only stream: %#lx.\n", hr);
        if (hr == S_OK)
        {
            hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
                    AudioObjectType_None, &object);
            ok(hr == S_OK, "Failed to activate a non-spatialized object: %#lx.\n", hr);
            if (hr == S_OK)
                ISpatialAudioObject_Release(object);
            ISpatialAudioObjectRenderStream_Release(sas);
        }
        sas = NULL;
    }

    /* event handle */
    fill_activation_params(&activation_params);
    activation_params.EventHandle = NULL;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop, &IID_ISpatialAudioObjectRenderStream, (void**)&sas);
    ok(hr == E_INVALIDARG, "Expected lack of no EventHandle to be invalid: 0x%08lx\n", hr);
    ok(sas == NULL, "Expected spatial audio stream to be set to NULL upon failed activation\n");

    activation_params.EventHandle = INVALID_HANDLE_VALUE;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop, &IID_ISpatialAudioObjectRenderStream, (void**)&sas);
    ok(hr == E_INVALIDARG, "Expected INVALID_HANDLE_VALUE to be invalid: 0x%08lx\n", hr);
    ok(sas == NULL, "Expected spatial audio stream to be set to NULL upon failed activation\n");

    /* must use only queried sample rate */
    fill_activation_params(&activation_params);
    memcpy(&wrong_format, &format, sizeof(format));
    activation_params.ObjectFormat = &wrong_format;
    wrong_format.nSamplesPerSec = 44100;
    wrong_format.nAvgBytesPerSec = wrong_format.nSamplesPerSec * wrong_format.nBlockAlign;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop, &IID_ISpatialAudioObjectRenderStream, (void**)&sas);
    ok(hr == AUDCLNT_E_UNSUPPORTED_FORMAT, "Expected format to be unsupported: 0x%08lx\n", hr);
    ok(sas == NULL, "Expected spatial audio stream to be set to NULL upon failed activation\n");

    /* Dynamic is an activatable object type, not a static-bed mask bit. */
    fill_activation_params(&activation_params);
    activation_params.StaticObjectTypeMask |= AudioObjectType_Dynamic;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
    ok(hr == E_INVALIDARG, "Expected dynamic static-mask bit to be invalid: 0x%08lx\n", hr);
    ok(sas == NULL, "Expected spatial audio stream to be set to NULL upon failed activation\n");

    fill_activation_params(&activation_params);
    activation_params.MinDynamicObjectCount = max_dyn_count + 1;
    activation_params.MaxDynamicObjectCount = max_dyn_count + 1;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
    ok(hr == SPTLAUDCLNT_E_NO_MORE_OBJECTS,
            "Expected an unsatisfied dynamic minimum to fail: 0x%08lx\n", hr);

    fill_activation_params(&activation_params);
    activation_params.MinDynamicObjectCount = 1;
    activation_params.MaxDynamicObjectCount = 0;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
    ok(hr == E_INVALIDARG, "Expected reversed dynamic range to be invalid: 0x%08lx\n", hr);

    if (spatial_stream_supported)
    {
        fill_activation_params(&activation_params);
        activation_params.MaxDynamicObjectCount = max_dyn_count + 1;
        hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
                &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
        ok(hr == S_OK, "Expected dynamic maximum to be capped: 0x%08lx\n", hr);
        if (hr == S_OK)
            ISpatialAudioObjectRenderStream_Release(sas);
        sas = NULL;
    }

    /* ISpatialAudioObjectRenderStreamNotify */
    fill_activation_params(&activation_params);
    memset(&notify_object, 0, sizeof(notify_object));
    notify_object.ISpatialAudioObjectRenderStreamNotify_iface.lpVtbl = &notifyobjvtbl;
    activation_params.MaxDynamicObjectCount = max_dyn_count ? 1 : 0;
    activation_params.NotifyObject = &notify_object.ISpatialAudioObjectRenderStreamNotify_iface;
    if (spatial_stream_supported)
    {
        hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
                &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
        ok(hr == S_OK, "Failed to activate spatial audio stream: 0x%08lx\n", hr);
        if (hr == S_OK)
        {
            ok(notify_object.ref == 1, "Expected to get increased NotifyObject's ref count\n");
            ok(ISpatialAudioObjectRenderStream_Release(sas) == 0,
                    "Expected to release the last reference\n");
            ok(notify_object.ref == 0, "Expected to get lowered NotifyObject's ref count\n");
        }
    }
}

static void test_proactive_invalidation(void)
{
    static const char fault_name[] =
            "SWITCHYARD_SPATIAL_AUDIO_FAULT_INVALIDATE_ON_START";
    ISpatialAudioObjectRenderStream *stream = NULL;
    ISpatialAudioObject *object = NULL;
    NotifyObject notify;
    BYTE *buffer = NULL;
    UINT32 available = ~0u;
    UINT32 bytes = ~0u;
    BOOL active = TRUE;
    DWORD wait;
    HRESULT hr;

    if (!spatial_stream_supported || !max_dyn_count)
    {
        win_skip("Dynamic endpoint-loss notification is unavailable.\n");
        return;
    }

    memset(&notify, 0, sizeof(notify));
    notify.ISpatialAudioObjectRenderStreamNotify_iface.lpVtbl = &notifyobjvtbl;
    notify.expect_notification = TRUE;
    notify.reenter_stream = TRUE;
    notify.callback_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!notify.callback_event, "Failed to create the notification event: %lu.\n",
            GetLastError());
    if (!notify.callback_event)
        return;

    hr = activate_fault_stream(fault_name, &notify, &stream);
    ok(hr == S_OK, "Failed to activate the fault-injection stream: %#lx.\n", hr);
    if (hr != S_OK || !stream)
        goto done;

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(stream,
            AudioObjectType_Dynamic, &object);
    ok(hr == S_OK && object, "Failed to activate the endpoint-loss object: %#lx/%p.\n",
            hr, object);
    if (hr != S_OK || !object)
        goto done;

    hr = ISpatialAudioObjectRenderStream_Start(stream);
    ok(hr == S_OK || hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED ||
            hr == AUDCLNT_E_DEVICE_INVALIDATED ||
            hr == AUDCLNT_E_RESOURCES_INVALIDATED,
            "Unexpected fault-injection Start result: %#lx.\n", hr);
    wait = WaitForSingleObject(notify.callback_event, 5000);
    ok(wait == WAIT_OBJECT_0, "Timed out waiting for proactive notification: %#lx.\n",
            wait);
    if (wait == WAIT_OBJECT_0)
    {
        ok(notify.calls == 1, "Expected one notification, got %ld.\n", notify.calls);
        ok(!notify.deadline, "Expected a zero deadline, got %s.\n",
                wine_dbgstr_longlong(notify.deadline));
        ok(!notify.object_count, "Expected zero available objects, got %u.\n",
                notify.object_count);
        hr = ISpatialAudioObject_IsActive(object, &active);
        ok(hr == S_OK && !active,
                "Expected the endpoint-loss object to be inactive, got %#lx/%d.\n",
                hr, active);
        hr = ISpatialAudioObject_SetPosition(object, 1.0f, 0.0f, -1.0f);
        ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED,
                "Expected invalidated position state, got %#lx.\n", hr);
        hr = ISpatialAudioObject_GetBuffer(object, &buffer, &bytes);
        ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED,
                "Expected invalidated object buffer, got %#lx.\n", hr);
        hr = ISpatialAudioObjectRenderStream_GetAvailableDynamicObjectCount(stream,
                &available);
        ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED && !available,
                "Expected invalidated resources and zero objects, got %#lx/%u.\n",
                hr, available);
        wait = WaitForSingleObject(notify.callback_event, 100);
        ok(wait == WAIT_TIMEOUT, "Expected a one-shot notification, got %#lx.\n", wait);
    }

    ISpatialAudioObject_Release(object);
    object = NULL;
    ISpatialAudioObjectRenderStream_Release(stream);
    stream = NULL;
    for (wait = 0; wait < 100 &&
            InterlockedCompareExchange(&notify.ref, 0, 0); ++wait)
        Sleep(1);
    ok(!InterlockedCompareExchange(&notify.ref, 0, 0),
            "Notification reference was not released, got %ld.\n",
            InterlockedCompareExchange(&notify.ref, 0, 0));

done:
    if (object)
        ISpatialAudioObject_Release(object);
    if (stream)
        ISpatialAudioObjectRenderStream_Release(stream);
    CloseHandle(notify.callback_event);
}

static void test_proactive_reentrant_release(void)
{
    static const char fault_name[] =
            "SWITCHYARD_SPATIAL_AUDIO_FAULT_INVALIDATE_ON_START";
    ISpatialAudioObjectRenderStream *stream = NULL;
    NotifyObject notify;
    DWORD wait;
    HRESULT hr;

    if (!spatial_stream_supported || !max_dyn_count)
    {
        win_skip("Dynamic endpoint-loss notification is unavailable.\n");
        return;
    }

    memset(&notify, 0, sizeof(notify));
    notify.ISpatialAudioObjectRenderStreamNotify_iface.lpVtbl = &notifyobjvtbl;
    notify.expect_notification = TRUE;
    notify.release_stream = TRUE;
    notify.callback_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!notify.callback_event, "Failed to create the notification event: %lu.\n",
            GetLastError());
    if (!notify.callback_event)
        return;

    hr = activate_fault_stream(fault_name, &notify, &stream);
    ok(hr == S_OK, "Failed to activate the reentrant-release stream: %#lx.\n", hr);
    if (hr != S_OK || !stream)
        goto done;

    hr = ISpatialAudioObjectRenderStream_Start(stream);
    ok(hr == S_OK || hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED ||
            hr == AUDCLNT_E_DEVICE_INVALIDATED ||
            hr == AUDCLNT_E_RESOURCES_INVALIDATED,
            "Unexpected reentrant-release Start result: %#lx.\n", hr);
    wait = WaitForSingleObject(notify.callback_event, 5000);
    ok(wait == WAIT_OBJECT_0,
            "Timed out waiting for the reentrant-release notification: %#lx.\n",
            wait);
    if (wait == WAIT_OBJECT_0)
    {
        stream = NULL; /* The callback released the application's reference. */
        ok(notify.calls == 1, "Expected one notification, got %ld.\n", notify.calls);
    }

done:
    if (stream)
        ISpatialAudioObjectRenderStream_Release(stream);
    for (wait = 0; wait < 100 &&
            InterlockedCompareExchange(&notify.ref, 0, 0); ++wait)
        Sleep(1);
    ok(!InterlockedCompareExchange(&notify.ref, 0, 0),
            "Notification reference was not released, got %ld.\n",
            InterlockedCompareExchange(&notify.ref, 0, 0));
    CloseHandle(notify.callback_event);
}

static void test_prestart_invalidation(void)
{
    static const char fault_name[] =
            "SWITCHYARD_SPATIAL_AUDIO_FAULT_INVALIDATE_BEFORE_START";
    ISpatialAudioObjectRenderStream *stream = NULL;
    ISpatialAudioObject *object = NULL;
    NotifyObject notify;
    UINT32 available = ~0u;
    DWORD wait;
    HRESULT hr;

    if (!spatial_stream_supported || !max_dyn_count)
    {
        win_skip("Dynamic endpoint-loss notification is unavailable.\n");
        return;
    }

    memset(&notify, 0, sizeof(notify));
    notify.ISpatialAudioObjectRenderStreamNotify_iface.lpVtbl = &notifyobjvtbl;
    notify.expect_notification = TRUE;
    notify.callback_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!notify.callback_event, "Failed to create the notification event: %lu.\n",
            GetLastError());
    if (!notify.callback_event)
        return;

    hr = activate_fault_stream(fault_name, &notify, &stream);
    ok(hr == S_OK, "Failed to activate the pre-start fault stream: %#lx.\n", hr);
    if (hr != S_OK || !stream)
        goto done;

    hr = ISpatialAudioObjectRenderStream_GetAvailableDynamicObjectCount(stream,
            &available);
    ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED && !available,
            "Expected pre-start invalidation and zero objects, got %#lx/%u.\n",
            hr, available);
    ok(!notify.calls, "Notification escaped before Start, got %ld calls.\n",
            notify.calls);
    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(stream,
            AudioObjectType_Dynamic, &object);
    ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED && !object,
            "Expected object activation to observe endpoint loss, got %#lx/%p.\n",
            hr, object);

    hr = ISpatialAudioObjectRenderStream_Start(stream);
    ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED,
            "Expected Start to report endpoint loss, got %#lx.\n", hr);
    wait = WaitForSingleObject(notify.callback_event, 5000);
    ok(wait == WAIT_OBJECT_0,
            "Timed out waiting for the armed pre-start notification: %#lx.\n",
            wait);
    ok(notify.calls == 1, "Expected one notification, got %ld.\n", notify.calls);

done:
    if (object)
        ISpatialAudioObject_Release(object);
    if (stream)
        ISpatialAudioObjectRenderStream_Release(stream);
    for (wait = 0; wait < 100 &&
            InterlockedCompareExchange(&notify.ref, 0, 0); ++wait)
        Sleep(1);
    ok(!InterlockedCompareExchange(&notify.ref, 0, 0),
            "Notification reference was not released, got %ld.\n",
            InterlockedCompareExchange(&notify.ref, 0, 0));
    CloseHandle(notify.callback_event);
}

static void test_audio_object_activation(void)
{
    AudioObjectType type;
    HRESULT hr;
    BOOL is_active;
    ISpatialAudioObjectRenderStream *sas = NULL;
    ISpatialAudioObject *sao1, *sao2, *sao3;

    SpatialAudioObjectRenderStreamActivationParams activation_params;
    PROPVARIANT activation_params_prop;

    if (!spatial_stream_supported)
    {
        win_skip("The audio backend does not support spatial audio objects.\n");
        return;
    }

    PropVariantInit(&activation_params_prop);
    activation_params_prop.vt = VT_BLOB;
    activation_params_prop.blob.cbSize = sizeof(activation_params);
    activation_params_prop.blob.pBlobData = (BYTE*) &activation_params;

    fill_activation_params(&activation_params);
    activation_params.StaticObjectTypeMask &= ~AudioObjectType_FrontRight;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop, &IID_ISpatialAudioObjectRenderStream, (void**)&sas);
    ok(hr == S_OK, "Failed to activate spatial audio stream: 0x%08lx\n", hr);
    if (hr != S_OK)
        return;

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas, AudioObjectType_FrontLeft, &sao1);
    ok(hr == S_OK, "Failed to activate spatial audio object: 0x%08lx\n", hr);
    if (hr != S_OK)
    {
        ISpatialAudioObjectRenderStream_Release(sas);
        return;
    }
    hr = ISpatialAudioObject_IsActive(sao1, &is_active);
    ok(hr == S_OK, "Failed to check if spatial audio object is active: 0x%08lx\n", hr);
    if (hr == S_OK)
        ok(is_active, "Expected spatial audio object to be active\n");

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas, AudioObjectType_FrontLeft, &sao2);
    ok(hr == SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE, "Expected audio object to be already active: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas, AudioObjectType_FrontRight, &sao2);
    ok(hr == SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE, "Expected static object to be not available: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_FrontLeft | AudioObjectType_SideLeft, &sao2);
    ok(hr == SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE,
            "Expected a combined static object overlapping an active channel to fail: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_SideLeft | AudioObjectType_SideRight, &sao2);
    ok(hr == S_OK, "Failed to activate a combined static object: 0x%08lx\n", hr);
    if (hr == S_OK)
    {
        hr = ISpatialAudioObject_GetAudioObjectType(sao2, &type);
        ok(hr == S_OK, "Failed to query combined static object type: %#lx.\n", hr);
        ok(type == (AudioObjectType_SideLeft | AudioObjectType_SideRight),
                "Got unexpected combined object type %#x.\n", type);
        ISpatialAudioObject_Release(sao2);
    }

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas, AudioObjectType_Dynamic, &sao2);
    ok(hr == SPTLAUDCLNT_E_NO_MORE_OBJECTS, "Expected to not have no more dynamic objects: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_None, &sao2);
    ok(hr == S_OK, "Failed to activate a non-spatialized audio object: %#lx.\n", hr);
    if (hr == S_OK)
    {
        hr = ISpatialAudioObject_GetAudioObjectType(sao2, &type);
        ok(hr == S_OK, "Failed to query non-spatialized object type: %#lx.\n", hr);
        ok(type == AudioObjectType_None, "Got unexpected object type %#x.\n", type);
        hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
                AudioObjectType_None, &sao3);
        ok(hr == SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE,
                "Expected the non-spatialized slot to be bounded: %#lx.\n", hr);
        ISpatialAudioObject_Release(sao2);
    }

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_BottomBackRight, &sao2);
    ok(hr == S_OK, "Failed to activate a bottom spatial audio object: %#lx.\n", hr);
    if (hr == S_OK)
    {
        hr = ISpatialAudioObject_GetAudioObjectType(sao2, &type);
        ok(hr == S_OK, "Failed to query bottom spatial audio object type: %#lx.\n", hr);
        ok(type == AudioObjectType_BottomBackRight,
                "Got unexpected object type %#x.\n", type);
        ISpatialAudioObject_Release(sao2);
    }

    ISpatialAudioObject_Release(sao1);
    ISpatialAudioObjectRenderStream_Release(sas);
}

static BOOL is_buffer_zeroed(const BYTE *buffer, UINT32 buffer_length)
{
    UINT32 i;

    for (i = 0; i < buffer_length; i++)
    {
        if (buffer[i] != 0)
            return FALSE;
    }

    return TRUE;
}

static void test_audio_object_buffers(void)
{
    UINT32 dyn_object_count, frame_count, max_frame_count, buffer_length;
    SpatialAudioObjectRenderStreamActivationParams activation_params;
    ISpatialAudioObjectRenderStream *sas = NULL;
    PROPVARIANT activation_params_prop;
    ISpatialAudioObject *sao[4];
    BOOL is_active;
    BYTE *buffer;
    INT i, j, k;
    HRESULT hr;

    if (!spatial_stream_supported)
    {
        win_skip("The audio backend does not support spatial object buffers.\n");
        return;
    }

    PropVariantInit(&activation_params_prop);
    activation_params_prop.vt = VT_BLOB;
    activation_params_prop.blob.cbSize = sizeof(activation_params);
    activation_params_prop.blob.pBlobData = (BYTE*) &activation_params;

    fill_activation_params(&activation_params);
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop, &IID_ISpatialAudioObjectRenderStream, (void**)&sas);
    ok(hr == S_OK, "Failed to activate spatial audio stream: 0x%08lx\n", hr);
    if (hr != S_OK)
        return;

    hr = ISpatialAudioObjectRenderStream_Reset(sas);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = ISpatialAudioClient_GetMaxFrameCount(sac, &format, &max_frame_count);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    ok(max_frame_count != 0, "Expected a nonzero endpoint processing quantum.\n");

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas, AudioObjectType_FrontLeft, &sao[0]);
    ok(hr == S_OK, "Failed to activate spatial audio object: 0x%08lx\n", hr);

    hr = ISpatialAudioObject_SetVolume(sao[0], 0.5f);
    ok(hr == SPTLAUDCLNT_E_OUT_OF_ORDER, "Got %#lx outside an update pass.\n", hr);
    hr = ISpatialAudioObject_SetPosition(sao[0], 0.0f, 0.0f, 0.0f);
    ok(hr == SPTLAUDCLNT_E_OUT_OF_ORDER, "Got %#lx outside an update pass.\n", hr);

    hr = ISpatialAudioObjectRenderStream_Start(sas);
    ok(hr == S_OK, "Failed to activate spatial audio render stream: 0x%08lx\n", hr);
    hr = ISpatialAudioObjectRenderStream_Start(sas);
    ok(hr == SPTLAUDCLNT_E_STREAM_NOT_STOPPED,
            "Expected a second Start to report a running stream, got %#lx.\n", hr);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas, AudioObjectType_FrontRight, &sao[1]);
    ok(hr == S_OK, "Failed to activate spatial audio object: 0x%08lx\n", hr);

    hr = WaitForSingleObject(event, 200);
    ok(hr == WAIT_OBJECT_0, "Expected event to be flagged: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas, AudioObjectType_SideLeft, &sao[2]);
    ok(hr == S_OK, "Failed to activate spatial audio object: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &dyn_object_count, &frame_count);
    ok(hr == S_OK, "Failed to begin updating audio objects: 0x%08lx\n", hr);
    ok(dyn_object_count == 0, "Unexpected dynamic objects\n");
    ok(frame_count <= max_frame_count, "Got unexpected frame count %u.\n", frame_count);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas, AudioObjectType_SideRight, &sao[3]);
    ok(hr == S_OK, "Failed to activate spatial audio object: 0x%08lx\n", hr);

    for (i = 0; i < ARRAYSIZE(sao); i++)
    {
        hr = ISpatialAudioObject_GetBuffer(sao[i], &buffer, &buffer_length);
        ok(hr == S_OK, "Expected to be able to get buffers for audio object: 0x%08lx\n", hr);
        ok(buffer != NULL, "Expected to get a non-NULL buffer\n");
        ok(buffer_length == frame_count * format.wBitsPerSample / 8, "Expected buffer length to be sample_size * frame_count = %u but got %u\n",
           frame_count * format.wBitsPerSample / 8, buffer_length);
        ok(is_buffer_zeroed(buffer, buffer_length), "Expected audio object's buffer to be zeroed\n");
    }

    hr = ISpatialAudioObject_SetVolume(sao[0], 0.5f);
    ok(hr == S_OK, "Failed to set spatial audio object volume: %#lx.\n", hr);
    hr = ISpatialAudioObject_SetVolume(sao[0], -0.1f);
    ok(hr == E_INVALIDARG, "Got %#lx for an invalid volume.\n", hr);
    hr = ISpatialAudioObject_SetVolume(sao[0], 1.1f);
    ok(hr == E_INVALIDARG, "Got %#lx for an invalid volume.\n", hr);
    hr = ISpatialAudioObject_SetPosition(sao[0], 0.0f, 0.0f, 0.0f);
    ok(hr == SPTLAUDCLNT_E_PROPERTY_NOT_SUPPORTED,
            "Got %#lx when positioning a static object.\n", hr);

    hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
    ok(hr == S_OK, "Failed to end updating audio objects: 0x%08lx\n", hr);

    /* Emulate underrun and test frame count approximate limit. */

    /* Force 1ms Sleep() timer resolution. */
    timeBeginPeriod(1);
    for (j = 0; j < 20; ++j)
    {
        hr = WaitForSingleObject(event, 200);
        ok(hr == WAIT_OBJECT_0, "Expected event to be flagged: 0x%08lx, j %u.\n", hr, j);

        hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &dyn_object_count, &frame_count);
        ok(hr == S_OK, "Failed to begin updating audio objects: 0x%08lx\n", hr);
        ok(dyn_object_count == 0, "Unexpected dynamic objects\n");
        ok(frame_count <= max_frame_count, "Got unexpected frame_count %u.\n", frame_count);

        /* Audio starts crackling with delays 10ms and above. However, setting such delay (that is, the delay
         * which skips the whole quantum) breaks SA on some Testbot machines: _BeginUpdatingAudioObjects fails
         * with SPTLAUDCLNT_E_INTERNAL starting from some iteration or WaitForSingleObject timeouts. That seems
         * to work on the real hardware though. */
        Sleep(5);

        for (i = 0; i < ARRAYSIZE(sao); i++)
        {
            hr = ISpatialAudioObject_GetBuffer(sao[i], &buffer, &buffer_length);
            ok(hr == S_OK, "Expected to be able to get buffers for audio object: 0x%08lx, i %d\n", hr, i);
            ok(buffer != NULL, "Expected to get a non-NULL buffer\n");
            ok(buffer_length == frame_count * format.wBitsPerSample / 8,
                    "Expected buffer length to be sample_size * frame_count = %u but got %u\n",
                    frame_count * format.wBitsPerSample / 8, buffer_length);

            /* Enable to hear the test sound. */
            if (0)
            {
                if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
                {
                    for (k = 0; k < frame_count; ++k)
                    {
                        float time_sec = 10.0f / 1000.0f * (j + (float)k / frame_count);

                        /* 440Hz tone. */
                        ((float *)buffer)[k] = sinf(2.0f * M_PI * time_sec * 440.0f);
                    }
                }
            }
        }
        hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
        ok(hr == S_OK, "Failed to end updating audio objects: 0x%08lx\n", hr);
    }
    timeEndPeriod(1);

    hr = WaitForSingleObject(event, 200);
    ok(hr == WAIT_OBJECT_0, "Expected event to be flagged: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &dyn_object_count, &frame_count);
    ok(hr == S_OK, "Failed to begin updating audio objects: 0x%08lx\n", hr);
    ok(dyn_object_count == 0, "Unexpected dynamic objects\n");

    /* one more iteration but not with every object */
    for (i = 0; i < ARRAYSIZE(sao) - 1; i++)
    {
        hr = ISpatialAudioObject_GetBuffer(sao[i], &buffer, &buffer_length);
        ok(hr == S_OK, "Expected to be able to get buffers for audio object: 0x%08lx\n", hr);
        ok(buffer != NULL, "Expected to get a non-NULL buffer\n");
        ok(buffer_length == frame_count * format.wBitsPerSample / 8, "Expected buffer length to be sample_size * frame_count = %u but got %u\n",
                frame_count * format.wBitsPerSample / 8, buffer_length);
        ok(is_buffer_zeroed(buffer, buffer_length), "Expected audio object's buffer to be zeroed\n");
    }

    hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
    ok(hr == S_OK, "Failed to end updating audio objects: 0x%08lx\n", hr);

    /* ending the stream */
    hr = ISpatialAudioObject_SetEndOfStream(sao[0], 0);
    ok(hr == SPTLAUDCLNT_E_OUT_OF_ORDER, "Expected that ending the stream at this point won't be allowed: 0x%08lx\n", hr);

    hr = WaitForSingleObject(event, 200);
    ok(hr == WAIT_OBJECT_0, "Expected event to be flagged: 0x%08lx\n", hr);

    hr = ISpatialAudioObject_SetEndOfStream(sao[0], 0);
    ok(hr == SPTLAUDCLNT_E_OUT_OF_ORDER, "Expected that ending the stream at this point won't be allowed: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &dyn_object_count, &frame_count);
    ok(hr == S_OK, "Failed to begin updating audio objects: 0x%08lx\n", hr);
    ok(dyn_object_count == 0, "Unexpected dynamic objects\n");

    /* expect the object that was not updated last cycle to be invalidated */
    hr = ISpatialAudioObject_GetBuffer(sao[ARRAYSIZE(sao) - 1], &buffer, &buffer_length);
    ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED, "Expected audio object to be invalidated: 0x%08lx\n", hr);

    for (i = 0; i < ARRAYSIZE(sao) - 1; i++)
    {
        hr = ISpatialAudioObject_GetBuffer(sao[i], &buffer, &buffer_length);
        ok(hr == S_OK, "Expected to be able to get buffers for audio object: 0x%08lx\n", hr);

        hr = ISpatialAudioObject_SetEndOfStream(sao[i], 0);
        ok(hr == S_OK, "Failed to end the stream: 0x%08lx\n", hr);

        hr = ISpatialAudioObject_GetBuffer(sao[i], &buffer, &buffer_length);
        ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED, "Expected audio object to be invalidated: 0x%08lx\n", hr);

        hr = ISpatialAudioObject_IsActive(sao[i], &is_active);
        ok(hr == S_OK, "Failed to query object activity: %#lx.\n", hr);
        ok(!is_active, "Expected an ended object to be inactive.\n");
    }

    /* Applications may release an object immediately after submitting its
     * final buffer. The renderer must retain that buffer through this pass. */
    ISpatialAudioObject_Release(sao[0]);
    sao[0] = NULL;

    hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
    ok(hr == S_OK, "Failed to end updating audio objects: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_FrontLeft, &sao[0]);
    ok(hr == S_OK, "Failed to reactivate released spatial audio object: 0x%08lx\n", hr);

    hr = ISpatialAudioObjectRenderStream_Reset(sas);
    ok(hr == SPTLAUDCLNT_E_STREAM_NOT_STOPPED, "got %#lx.\n", hr);

    hr = ISpatialAudioObjectRenderStream_Stop(sas);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = ISpatialAudioObjectRenderStream_Reset(sas);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = ISpatialAudioObject_IsActive(sao[0], &is_active);
    ok(hr == S_OK, "Failed to query reset object activity: %#lx.\n", hr);
    ok(!is_active, "Expected reset to revoke the active spatial audio object.\n");

    hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &dyn_object_count, &frame_count);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = ISpatialAudioObject_GetBuffer(sao[0], &buffer, &buffer_length);
    ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED,
            "Expected the reset object to remain invalidated, got %#lx.\n", hr);

    hr = ISpatialAudioObjectRenderStream_Reset(sas);
    todo_wine ok(hr == S_OK, "got %#lx.\n", hr);

    hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
    ok(hr == S_OK, "got %#lx.\n", hr);

    hr = ISpatialAudioObjectRenderStream_Reset(sas);
    ok(hr == S_OK, "got %#lx.\n", hr);

    for (i = 0; i < ARRAYSIZE(sao); i++)
    {
        ISpatialAudioObject_Release(sao[i]);
    }

    ISpatialAudioObjectRenderStream_Release(sas);
}

struct object_release_thread_context
{
    ISpatialAudioObject *object;
    HANDLE ready_event;
    HANDLE release_event;
    ULONG release_ref;
};

static DWORD WINAPI object_release_thread(void *arg)
{
    struct object_release_thread_context *context = arg;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
        return 3;
    if (!SetEvent(context->ready_event))
    {
        CoUninitialize();
        return 1;
    }
    if (WaitForSingleObject(context->release_event, 5000) != WAIT_OBJECT_0)
    {
        CoUninitialize();
        return 2;
    }

    context->release_ref = ISpatialAudioObject_Release(context->object);
    CoUninitialize();
    return 0;
}

static void test_dynamic_audio_objects(void)
{
    SpatialAudioObjectRenderStreamActivationParams activation_params;
    ISpatialAudioObjectRenderStream *sas = NULL;
    ISpatialAudioObject **objects, *replacement = NULL, *excess = NULL;
    PROPVARIANT activation_params_prop;
    UINT32 available, frames, bytes, limit, i, j, iteration;
    BOOL active;
    BYTE *buffer;
    HRESULT hr;

    if (!spatial_stream_supported || !max_dyn_count)
    {
        win_skip("The endpoint does not expose dynamic spatial objects.\n");
        return;
    }

    limit = max_dyn_count;
    objects = calloc(limit, sizeof(*objects));
    if (!objects)
    {
        skip("Not enough memory for %u dynamic object references.\n", limit);
        return;
    }
    PropVariantInit(&activation_params_prop);
    activation_params_prop.vt = VT_BLOB;
    activation_params_prop.blob.cbSize = sizeof(activation_params);
    activation_params_prop.blob.pBlobData = (BYTE *)&activation_params;

    fill_activation_params(&activation_params);
    activation_params.StaticObjectTypeMask = AudioObjectType_None;
    activation_params.MinDynamicObjectCount = 1;
    activation_params.MaxDynamicObjectCount = limit;
    hr = ISpatialAudioClient_ActivateSpatialAudioStream(sac, &activation_params_prop,
            &IID_ISpatialAudioObjectRenderStream, (void **)&sas);
    ok(hr == S_OK, "Failed to activate a dynamic spatial stream: %#lx.\n", hr);
    if (hr != S_OK)
    {
        free(objects);
        return;
    }

    hr = ISpatialAudioObjectRenderStream_GetAvailableDynamicObjectCount(sas, &available);
    ok(hr == S_OK, "Failed to query dynamic capacity: %#lx.\n", hr);
    ok(available == limit, "Expected %u available objects, got %u.\n", limit, available);

    for (i = 0; i < limit; ++i)
    {
        AudioObjectType type = AudioObjectType_None;

        hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
                AudioObjectType_Dynamic, &objects[i]);
        ok(hr == S_OK, "Failed to activate dynamic object %u: %#lx.\n", i, hr);
        if (hr != S_OK)
            goto cleanup;
        hr = ISpatialAudioObject_GetAudioObjectType(objects[i], &type);
        ok(hr == S_OK && type == AudioObjectType_Dynamic,
                "Got type %#x and status %#lx for object %u.\n", type, hr, i);
    }

    hr = ISpatialAudioObjectRenderStream_GetAvailableDynamicObjectCount(sas, &available);
    ok(hr == S_OK && !available, "Expected exhausted capacity, got %u and %#lx.\n",
            available, hr);
    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_Dynamic, &excess);
    ok(hr == SPTLAUDCLNT_E_NO_MORE_OBJECTS,
            "Expected dynamic-object exhaustion, got %#lx.\n", hr);
    ok(!excess, "Expected a null object on exhaustion.\n");

    hr = ISpatialAudioObject_SetPosition(objects[0], 1.0f, 2.0f, 3.0f);
    ok(hr == SPTLAUDCLNT_E_OUT_OF_ORDER, "Got %#lx outside an update pass.\n", hr);
    hr = ISpatialAudioObject_SetVolume(objects[0], 0.25f);
    ok(hr == SPTLAUDCLNT_E_OUT_OF_ORDER, "Got %#lx outside an update pass.\n", hr);

    hr = ISpatialAudioObjectRenderStream_Start(sas);
    ok(hr == S_OK, "Failed to start dynamic stream: %#lx.\n", hr);
    if (hr != S_OK)
        goto cleanup;
    hr = WaitForSingleObject(event, 500);
    ok(hr == WAIT_OBJECT_0, "Timed out waiting for a dynamic update: %#lx.\n", hr);

    hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &available, &frames);
    ok(hr == S_OK, "Failed to begin dynamic update: %#lx.\n", hr);
    if (hr != S_OK)
        goto cleanup;
    ok(!available, "Expected no capacity while all slots are retained, got %u.\n", available);
    ok(frames != 0, "Expected a nonzero dynamic update.\n");

    for (i = 0; i < limit; ++i)
    {
        hr = ISpatialAudioObject_GetBuffer(objects[i], &buffer, &bytes);
        ok(hr == S_OK, "Failed to get dynamic buffer %u: %#lx.\n", i, hr);
        if (hr != S_OK)
            continue;
        ok(bytes == frames * format.nBlockAlign,
                "Expected %u bytes, got %u for dynamic object %u.\n",
                frames * format.nBlockAlign, bytes, i);
        for (j = 0; j < frames; ++j)
            ((float *)buffer)[j] = (float)(i + 1) / 16.0f;
    }

    hr = ISpatialAudioObject_SetPosition(objects[0], 1.25f, -0.5f, 3.0f);
    ok(hr == S_OK, "Failed to position a dynamic object: %#lx.\n", hr);
    hr = ISpatialAudioObject_SetPosition(objects[0], NAN, 0.0f, 0.0f);
    ok(hr == E_INVALIDARG, "Expected NaN position to be rejected, got %#lx.\n", hr);
    hr = ISpatialAudioObject_SetPosition(objects[0], 0.0f, INFINITY, 0.0f);
    ok(hr == E_INVALIDARG, "Expected infinite position to be rejected, got %#lx.\n", hr);
    hr = ISpatialAudioObject_SetVolume(objects[0], 0.25f);
    ok(hr == S_OK, "Failed to set dynamic volume: %#lx.\n", hr);

    if (limit > 1)
    {
        hr = ISpatialAudioObject_SetEndOfStream(objects[1], frames / 2);
        ok(hr == S_OK, "Failed to end a dynamic object: %#lx.\n", hr);
        hr = ISpatialAudioObject_IsActive(objects[1], &active);
        ok(hr == S_OK && !active, "Expected ended object to be inactive, got %d, %#lx.\n",
                active, hr);
    }

    hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
    ok(hr == S_OK, "Failed to submit dynamic update: %#lx.\n", hr);

    /* Keep the maximum-object graph busy long enough to exercise cursor and
     * metadata wraparound and to provide a useful callback percentile sample.
     * The ended slot stays retained but is not rendered. */
    for (iteration = 0; iteration < 64; ++iteration)
    {
        hr = WaitForSingleObject(event, 500);
        ok(hr == WAIT_OBJECT_0, "Timed out waiting for steady update %u: %#lx.\n",
                iteration, hr);
        if (hr != WAIT_OBJECT_0)
            goto cleanup;
        hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas,
                &available, &frames);
        ok(hr == S_OK, "Failed to begin steady update %u: %#lx.\n", iteration, hr);
        if (hr != S_OK)
            goto cleanup;
        for (i = 0; i < limit; ++i)
        {
            if (i == 1 && limit > 1)
                continue;
            hr = ISpatialAudioObject_GetBuffer(objects[i], &buffer, &bytes);
            ok(hr == S_OK, "Failed steady buffer %u/%u: %#lx.\n", iteration, i, hr);
            if (hr == S_OK)
                for (j = 0; j < frames; ++j)
                    ((float *)buffer)[j] = (float)(i + 1) / 32.0f;
        }
        hr = ISpatialAudioObject_SetPosition(objects[0],
                (iteration & 1) ? -1.0f : 1.0f,
                (float)(iteration % 5) * 0.1f, -2.0f);
        ok(hr == S_OK, "Failed steady position %u: %#lx.\n", iteration, hr);
        hr = ISpatialAudioObject_SetVolume(objects[0],
                0.25f + (float)(iteration % 4) * 0.125f);
        ok(hr == S_OK, "Failed steady volume %u: %#lx.\n", iteration, hr);
        hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
        ok(hr == S_OK, "Failed to submit steady update %u: %#lx.\n", iteration, hr);
    }

    hr = WaitForSingleObject(event, 500);
    ok(hr == WAIT_OBJECT_0, "Timed out waiting for a second dynamic update: %#lx.\n", hr);
    hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &available, &frames);
    ok(hr == S_OK, "Failed to begin second dynamic update: %#lx.\n", hr);
    if (hr != S_OK)
        goto cleanup;

    if (limit > 1)
    {
        hr = ISpatialAudioObject_GetBuffer(objects[1], &buffer, &bytes);
        ok(hr == SPTLAUDCLNT_E_RESOURCES_INVALIDATED,
                "Expected ended object to reject buffers, got %#lx.\n", hr);
        ISpatialAudioObject_Release(objects[1]);
        objects[1] = NULL;
        hr = ISpatialAudioObjectRenderStream_GetAvailableDynamicObjectCount(sas, &available);
        ok(hr == S_OK && !available,
                "A slot released during an update must remain retained until End, got %u, %#lx.\n",
                available, hr);
    }

    /* Omit object zero after its lifetime started: this is implicit EOS. */
    for (i = 1; i < limit; ++i)
    {
        if (!objects[i])
            continue;
        hr = ISpatialAudioObject_GetBuffer(objects[i], &buffer, &bytes);
        ok(hr == S_OK, "Failed to preserve dynamic object %u: %#lx.\n", i, hr);
    }
    hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
    ok(hr == S_OK, "Failed to submit implicit-EOS update: %#lx.\n", hr);
    hr = ISpatialAudioObject_IsActive(objects[0], &active);
    ok(hr == S_OK && !active, "Expected implicit EOS, got active %d and %#lx.\n", active, hr);

    ISpatialAudioObject_Release(objects[0]);
    objects[0] = NULL;
    hr = ISpatialAudioObjectRenderStream_GetAvailableDynamicObjectCount(sas, &available);
    ok(hr == S_OK && available == (limit > 1 ? 2 : 1),
            "Expected released dynamic slots to be reusable, got %u, %#lx.\n", available, hr);

    hr = ISpatialAudioObjectRenderStream_ActivateSpatialAudioObject(sas,
            AudioObjectType_Dynamic, &replacement);
    ok(hr == S_OK, "Failed to reuse a dynamic slot: %#lx.\n", hr);
    if (hr == S_OK)
    {
        hr = WaitForSingleObject(event, 500);
        ok(hr == WAIT_OBJECT_0, "Timed out waiting for reuse update: %#lx.\n", hr);
        hr = ISpatialAudioObjectRenderStream_BeginUpdatingAudioObjects(sas, &available, &frames);
        ok(hr == S_OK, "Failed to begin reuse update: %#lx.\n", hr);
        if (hr == S_OK)
        {
            struct object_release_thread_context *release_context;
            DWORD wait, exit_code = ~0u;
            HANDLE thread = NULL;

            hr = ISpatialAudioObject_GetBuffer(replacement, &buffer, &bytes);
            ok(hr == S_OK, "Failed to get reused dynamic buffer: %#lx.\n", hr);
            hr = ISpatialAudioObject_SetPosition(replacement, -2.0f, 1.0f, -4.0f);
            ok(hr == S_OK, "Failed to position reused dynamic object: %#lx.\n", hr);
            hr = ISpatialAudioObject_SetEndOfStream(replacement, frames);
            ok(hr == S_OK, "Failed to end reused dynamic object: %#lx.\n", hr);

            release_context = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*release_context));
            ok(!!release_context, "Failed to allocate release thread context.\n");
            if (release_context)
            {
                release_context->object = replacement;
                release_context->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
                release_context->release_event = CreateEventW(NULL, TRUE, FALSE, NULL);
            }
            ok(release_context && release_context->ready_event && release_context->release_event,
                    "Failed to create release barriers: %lu.\n", GetLastError());
            if (release_context && release_context->ready_event && release_context->release_event)
                thread = CreateThread(NULL, 0, object_release_thread,
                        release_context, 0, NULL);
            ok(!!thread, "Failed to create object-release thread: %lu.\n", GetLastError());

            if (thread)
            {
                /* Transfer the sole object reference to the worker. Both
                 * contenders leave this barrier together; either lock order
                 * must retain the final buffer and free the slot once. */
                replacement = NULL;
                wait = WaitForSingleObject(release_context->ready_event, 5000);
                ok(wait == WAIT_OBJECT_0,
                        "Object-release thread did not reach its barrier: %#lx.\n", wait);
                ok(SetEvent(release_context->release_event),
                        "Failed to open object-release barrier: %lu.\n", GetLastError());
                hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
                ok(hr == S_OK, "Failed concurrent EndUpdatingAudioObjects: %#lx.\n", hr);
                wait = WaitForSingleObject(thread, 5000);
                ok(wait == WAIT_OBJECT_0, "Object-release thread timed out: %#lx.\n", wait);
                ok(GetExitCodeThread(thread, &exit_code),
                        "Failed to query object-release thread: %lu.\n", GetLastError());
                ok(exit_code == 0, "Object-release thread failed: %lu.\n", exit_code);
                ok(release_context->release_ref == 0,
                        "Expected worker to release the last object reference, got %lu.\n",
                        release_context->release_ref);
                if (wait == WAIT_OBJECT_0 && exit_code != 0)
                    ISpatialAudioObject_Release(release_context->object);
                if (wait == WAIT_OBJECT_0)
                    CloseHandle(thread);
            }
            else
            {
                ISpatialAudioObject_Release(replacement);
                replacement = NULL;
                hr = ISpatialAudioObjectRenderStream_EndUpdatingAudioObjects(sas);
                ok(hr == S_OK, "Failed fallback release-during-update buffer: %#lx.\n", hr);
            }
            if (!thread || wait == WAIT_OBJECT_0)
            {
                if (release_context && release_context->release_event)
                    CloseHandle(release_context->release_event);
                if (release_context && release_context->ready_event)
                    CloseHandle(release_context->ready_event);
                if (release_context)
                    HeapFree(GetProcessHeap(), 0, release_context);
            }
            hr = ISpatialAudioObjectRenderStream_GetAvailableDynamicObjectCount(sas, &available);
            ok(hr == S_OK && available == (limit > 1 ? 2 : 1),
                    "Concurrent release leaked or duplicated a slot, got %u, %#lx.\n",
                    available, hr);
        }
    }

cleanup:
    if (replacement)
        ISpatialAudioObject_Release(replacement);
    for (i = 0; i < limit; ++i)
        if (objects[i])
            ISpatialAudioObject_Release(objects[i]);
    if (sas)
    {
        ISpatialAudioObjectRenderStream_Stop(sas);
        ISpatialAudioObjectRenderStream_Release(sas);
    }
    free(objects);
}

START_TEST(spatialaudio)
{
    HRESULT hr;

    event = CreateEventA(NULL, FALSE, FALSE, "spatial-audio-test-prog-event");
    ok(event != NULL, "Failed to create event, last error: 0x%08lx\n", GetLastError());

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (void**)&mme);
    if (FAILED(hr))
    {
        skip("mmdevapi not available: 0x%08lx\n", hr);
        goto cleanup;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(mme, eRender, eMultimedia, &dev);
    ok(hr == S_OK || hr == E_NOTFOUND, "GetDefaultAudioEndpoint failed: 0x%08lx\n", hr);
    if (hr != S_OK || !dev)
    {
        if (hr == E_NOTFOUND)
            skip("No sound card available\n");
        else
            skip("GetDefaultAudioEndpoint returns 0x%08lx\n", hr);
        goto cleanup;
    }

    hr = IMMDevice_Activate(dev, &IID_ISpatialAudioClient, CLSCTX_INPROC_SERVER, NULL, (void**)&sac);
    ok(hr == S_OK || hr == E_NOINTERFACE, "ISpatialAudioClient Activation failed: 0x%08lx\n", hr);
    if (hr != S_OK || !dev)
    {
        if (hr == E_NOINTERFACE)
            skip("ISpatialAudioClient interface not found\n");
        else
            skip("ISpatialAudioClient Activation returns 0x%08lx\n", hr);
        goto cleanup;
    }

    hr = ISpatialAudioClient_GetMaxDynamicObjectCount(sac, &max_dyn_count);
    ok(hr == S_OK, "Failed to get max dynamic object count: 0x%08lx\n", hr);

    /* that's the default, after manually enabling Windows Sonic it's possible to have max_dyn_count > 0 */
    /* ok(max_dyn_count == 0, "expected max dynamic object count to be 0 got %u\n", max_dyn_count); */

    test_formats();
    test_static_object_properties();
    test_stream_activation();
    test_audio_object_activation();
    test_audio_object_buffers();
    test_proactive_invalidation();
    test_proactive_reentrant_release();
    test_prestart_invalidation();
    test_dynamic_audio_objects();

    ISpatialAudioClient_Release(sac);

cleanup:
    if (dev)
        IMMDevice_Release(dev);
    if (mme)
        IMMDeviceEnumerator_Release(mme);
    CoUninitialize();
    CloseHandle(event);
}
