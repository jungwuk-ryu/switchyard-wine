/*
 * Copyright 2020 Andrew Eikum for CodeWeavers
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

#define COBJMACROS

#include <assert.h>
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "ole2.h"
#include "mmdeviceapi.h"
#include "mmsystem.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "spatialaudioclient.h"

#include "mmdevapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);

#define NATIVE_STATIC_OBJECT_MASK SPATIAL_AUDIO_STATIC_OBJECT_MASK

struct static_object_desc
{
    AudioObjectType type;
    DWORD speaker;
    float x, y, z;
};

/* Positions use the listener-relative, right-handed coordinate system documented
 * by ISpatialAudioClient. The exact distance is renderer-defined; keep every
 * directional speaker on a one-metre sphere and the non-directional LFE at the
 * listener origin. */
static const struct static_object_desc static_objects[] =
{
    {AudioObjectType_FrontLeft,        SPEAKER_FRONT_LEFT,       -0.5f,    0.0f, -0.8660254f},
    {AudioObjectType_FrontRight,       SPEAKER_FRONT_RIGHT,       0.5f,    0.0f, -0.8660254f},
    {AudioObjectType_FrontCenter,      SPEAKER_FRONT_CENTER,      0.0f,    0.0f, -1.0f},
    {AudioObjectType_LowFrequency,     SPEAKER_LOW_FREQUENCY,     0.0f,    0.0f,  0.0f},
    {AudioObjectType_SideLeft,         SPEAKER_SIDE_LEFT,        -1.0f,    0.0f,  0.0f},
    {AudioObjectType_SideRight,        SPEAKER_SIDE_RIGHT,        1.0f,    0.0f,  0.0f},
    {AudioObjectType_BackLeft,         SPEAKER_BACK_LEFT,        -0.5f,    0.0f,  0.8660254f},
    {AudioObjectType_BackRight,        SPEAKER_BACK_RIGHT,        0.5f,    0.0f,  0.8660254f},
    {AudioObjectType_TopFrontLeft,     SPEAKER_TOP_FRONT_LEFT,   -0.3535534f,  0.7071068f, -0.6123724f},
    {AudioObjectType_TopFrontRight,    SPEAKER_TOP_FRONT_RIGHT,   0.3535534f,  0.7071068f, -0.6123724f},
    {AudioObjectType_TopBackLeft,      SPEAKER_TOP_BACK_LEFT,    -0.3535534f,  0.7071068f,  0.6123724f},
    {AudioObjectType_TopBackRight,     SPEAKER_TOP_BACK_RIGHT,    0.3535534f,  0.7071068f,  0.6123724f},
    {AudioObjectType_BottomFrontLeft,  SPATIAL_AUDIO_BOTTOM_FRONT_LEFT_SPEAKER,
                                                               -0.3535534f, -0.7071068f, -0.6123724f},
    {AudioObjectType_BottomFrontRight, SPATIAL_AUDIO_BOTTOM_FRONT_RIGHT_SPEAKER,
                                                                0.3535534f, -0.7071068f, -0.6123724f},
    {AudioObjectType_BottomBackLeft,   SPATIAL_AUDIO_BOTTOM_BACK_LEFT_SPEAKER,
                                                               -0.3535534f, -0.7071068f,  0.6123724f},
    {AudioObjectType_BottomBackRight,  SPATIAL_AUDIO_BOTTOM_BACK_RIGHT_SPEAKER,
                                                                0.3535534f, -0.7071068f,  0.6123724f},
    {AudioObjectType_BackCenter,       SPEAKER_BACK_CENTER,       0.0f,    0.0f,  1.0f},
};

static UINT32 AudioObjectType_to_index(AudioObjectType type)
{
    UINT32 o = 0;
    while(type){
        type >>= 1;
        ++o;
    }
    return o - 2;
}

static const char *debugstr_fmtex(const WAVEFORMATEX *fmt)
{
    static char buf[2048];

    if (!fmt)
    {
        strcpy(buf, "(null)");
    }
    else if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
        snprintf(buf, sizeof(buf), "tag: 0x%x (%s), ch: %u (mask: 0x%lx), rate: %lu, depth: %u",
                fmt->wFormatTag, debugstr_guid(&fmtex->SubFormat),
                fmt->nChannels, fmtex->dwChannelMask, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    else
    {
        snprintf(buf, sizeof(buf), "tag: 0x%x, ch: %u, rate: %lu, depth: %u",
                fmt->wFormatTag, fmt->nChannels, fmt->nSamplesPerSec,
                fmt->wBitsPerSample);
    }
    return buf;
}

static BOOL formats_equal(const WAVEFORMATEX *fmt1, const WAVEFORMATEX *fmt2)
{
    return !memcmp(fmt1, fmt2, sizeof(*fmt1)) && !memcmp(fmt1 + 1, fmt2 + 1, fmt1->cbSize);
}

typedef struct SpatialAudioImpl SpatialAudioImpl;
typedef struct SpatialAudioStreamImpl SpatialAudioStreamImpl;
typedef struct SpatialAudioObjectImpl SpatialAudioObjectImpl;

struct SpatialAudioObjectImpl {
    ISpatialAudioObject ISpatialAudioObject_iface;
    LONG ref;

    SpatialAudioStreamImpl *sa_stream;
    AudioObjectType type;

    float *buf;
    float volume;
    UINT32 end_of_stream_frames;
    BOOL active;
    BOOL got_buffer;
    BOOL end_of_stream;
    BOOL released;

    struct list entry;
};

struct SpatialAudioStreamImpl {
    ISpatialAudioObjectRenderStream ISpatialAudioObjectRenderStream_iface;
    LONG ref;
    CRITICAL_SECTION lock;

    SpatialAudioImpl *sa_client;
    SpatialAudioObjectRenderStreamActivationParams params;

    IAudioClient *client;
    IAudioRenderClient *render;

    UINT32 period_frames, update_frames;
    WAVEFORMATEXTENSIBLE stream_fmtex;

    float *buf;

    UINT32 dry_channel;
    UINT32 static_object_map[17];

    struct list objects;
};

struct SpatialAudioImpl {
    ISpatialAudioClient ISpatialAudioClient_iface;
    IAudioFormatEnumerator IAudioFormatEnumerator_iface;
    IMMDevice *mmdev;
    LONG ref;
    WAVEFORMATEXTENSIBLE object_fmtex;
};

static inline SpatialAudioObjectImpl *impl_from_ISpatialAudioObject(ISpatialAudioObject *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioObjectImpl, ISpatialAudioObject_iface);
}

static inline SpatialAudioStreamImpl *impl_from_ISpatialAudioObjectRenderStream(ISpatialAudioObjectRenderStream *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioStreamImpl, ISpatialAudioObjectRenderStream_iface);
}

static inline SpatialAudioImpl *impl_from_ISpatialAudioClient(ISpatialAudioClient *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, ISpatialAudioClient_iface);
}

static inline SpatialAudioImpl *impl_from_IAudioFormatEnumerator(IAudioFormatEnumerator *iface)
{
    return CONTAINING_RECORD(iface, SpatialAudioImpl, IAudioFormatEnumerator_iface);
}

static HRESULT WINAPI SAO_QueryInterface(ISpatialAudioObject *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObject)) {
        *ppv = &This->ISpatialAudioObject_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAO_AddRef(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAO_Release(ISpatialAudioObject *iface)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);
    SpatialAudioStreamImpl *stream = This->sa_stream;
    ULONG ref = InterlockedDecrement(&This->ref);
    BOOL deferred = FALSE;

    TRACE("(%p) new ref %lu\n", This, ref);
    if (!ref)
    {
        EnterCriticalSection(&stream->lock);
        if (stream->update_frames != ~0u)
        {
            /* Keep the object's final buffer alive until EndUpdatingAudioObjects().
             * The object no longer owns its stream reference; a stream destroyed
             * before that call will discard the pending object itself. */
            This->released = TRUE;
            deferred = TRUE;
        }
        else
        {
            list_remove(&This->entry);
        }
        LeaveCriticalSection(&stream->lock);

        if (!deferred)
        {
            free(This->buf);
            free(This);
        }
        ISpatialAudioObjectRenderStream_Release(&stream->ISpatialAudioObjectRenderStream_iface);
    }
    return ref;
}

static HRESULT WINAPI SAO_GetBuffer(ISpatialAudioObject *iface,
        BYTE **buffer, UINT32 *bytes)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p, %p)\n", This, buffer, bytes);

    if (!buffer || !bytes)
        return E_POINTER;

    EnterCriticalSection(&This->sa_stream->lock);

    if (This->sa_stream->update_frames == ~0)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }
    if (!This->active)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }

    *buffer = (BYTE *)This->buf;
    *bytes = This->sa_stream->update_frames *
        This->sa_stream->sa_client->object_fmtex.Format.nBlockAlign;
    This->got_buffer = TRUE;

    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_SetEndOfStream(ISpatialAudioObject *iface, UINT32 frames)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%u)\n", This, frames);

    EnterCriticalSection(&This->sa_stream->lock);

    if (This->sa_stream->update_frames == ~0)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }
    if (!This->active)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }
    if (frames > This->sa_stream->update_frames)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return E_INVALIDARG;
    }

    This->got_buffer = TRUE;
    This->end_of_stream = TRUE;
    This->end_of_stream_frames = frames;
    This->active = FALSE;

    LeaveCriticalSection(&This->sa_stream->lock);
    return S_OK;
}

static HRESULT WINAPI SAO_IsActive(ISpatialAudioObject *iface, BOOL *active)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, active);

    if (!active)
        return E_POINTER;

    EnterCriticalSection(&This->sa_stream->lock);
    *active = This->active;
    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static HRESULT WINAPI SAO_GetAudioObjectType(ISpatialAudioObject *iface,
        AudioObjectType *type)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%p)\n", This, type);

    if (!type)
        return E_POINTER;

    *type = This->type;

    return S_OK;
}

static HRESULT WINAPI SAO_SetPosition(ISpatialAudioObject *iface, float x,
        float y, float z)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%f, %f, %f)\n", This, x, y, z);

    EnterCriticalSection(&This->sa_stream->lock);
    if (This->sa_stream->update_frames == ~0)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }
    if (!This->active)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }
    LeaveCriticalSection(&This->sa_stream->lock);

    /* Dynamic objects are not advertised until the backend can retain their
     * independent positions instead of collapsing them into the static bed. */
    return SPTLAUDCLNT_E_PROPERTY_NOT_SUPPORTED;
}

static HRESULT WINAPI SAO_SetVolume(ISpatialAudioObject *iface, float vol)
{
    SpatialAudioObjectImpl *This = impl_from_ISpatialAudioObject(iface);

    TRACE("(%p)->(%f)\n", This, vol);

    EnterCriticalSection(&This->sa_stream->lock);
    if (This->sa_stream->update_frames == ~0)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }
    if (!This->active)
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    }
    if (!(vol >= 0.0f && vol <= 1.0f))
    {
        LeaveCriticalSection(&This->sa_stream->lock);
        return E_INVALIDARG;
    }

    This->volume = vol;
    LeaveCriticalSection(&This->sa_stream->lock);

    return S_OK;
}

static ISpatialAudioObjectVtbl ISpatialAudioObject_vtbl = {
    SAO_QueryInterface,
    SAO_AddRef,
    SAO_Release,
    SAO_GetBuffer,
    SAO_SetEndOfStream,
    SAO_IsActive,
    SAO_GetAudioObjectType,
    SAO_SetPosition,
    SAO_SetVolume,
};

static HRESULT WINAPI SAORS_QueryInterface(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **ppv)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStreamBase) ||
            IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)) {
        *ppv = &This->ISpatialAudioObjectRenderStream_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAORS_AddRef(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAORS_Release(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object, *object2;
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) new ref %lu\n", This, ref);
    if (!ref)
    {
        IAudioClient_Stop(This->client);
        if (This->update_frames != ~0u && This->update_frames > 0)
            IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        IAudioRenderClient_Release(This->render);
        IAudioClient_Release(This->client);
        if (This->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_Release(This->params.NotifyObject);

        LIST_FOR_EACH_ENTRY_SAFE(object, object2, &This->objects, SpatialAudioObjectImpl, entry)
        {
            /* Live objects hold a stream reference, so only objects whose final
             * Release happened during an update can remain here. */
            assert(object->released);
            list_remove(&object->entry);
            free(object->buf);
            free(object);
        }

        free((void *)This->params.ObjectFormat);
        CloseHandle(This->params.EventHandle);
        DeleteCriticalSection(&This->lock);
        ISpatialAudioClient_Release(&This->sa_client->ISpatialAudioClient_iface);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAORS_GetAvailableDynamicObjectCount(
        ISpatialAudioObjectRenderStream *iface, UINT32 *count)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    FIXME("(%p)->(%p)\n", This, count);

    if (!count)
        return E_POINTER;

    *count = 0;
    return S_OK;
}

static HRESULT WINAPI SAORS_GetService(ISpatialAudioObjectRenderStream *iface,
        REFIID riid, void **service)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    FIXME("(%p)->(%s, %p)\n", This, debugstr_guid(riid), service);

    if (!service)
        return E_POINTER;

    *service = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI SAORS_Start(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Start(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Start failed: %08lx\n", hr);
        return hr;
    }

    return S_OK;
}

static HRESULT WINAPI SAORS_Stop(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Stop(This->client);
    if(FAILED(hr)){
        WARN("IAudioClient::Stop failed: %08lx\n", hr);
        return hr;
    }

    return S_OK;
}

static HRESULT WINAPI SAORS_Reset(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->()\n", This);

    hr = IAudioClient_Reset(This->client);
    if (hr == AUDCLNT_E_NOT_STOPPED)
        return SPTLAUDCLNT_E_STREAM_NOT_STOPPED;
    if (SUCCEEDED(hr))
    {
        EnterCriticalSection(&This->lock);
        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry)
        {
            object->active = FALSE;
            object->got_buffer = FALSE;
            object->end_of_stream = FALSE;
        }
        LeaveCriticalSection(&This->lock);
    }
    return hr;
}

static HRESULT WINAPI SAORS_BeginUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface,
        UINT32 *dyn_count, UINT32 *frames)
{
    static BOOL fixme_once = FALSE;
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object;
    HRESULT hr;

    TRACE("(%p)->(%p, %p)\n", This, dyn_count, frames);

    if (!dyn_count || !frames)
        return E_POINTER;

    EnterCriticalSection(&This->lock);

    if(This->update_frames != ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    This->update_frames = This->period_frames;

    if(This->update_frames > 0){
        hr = IAudioRenderClient_GetBuffer(This->render, This->update_frames, (BYTE **)&This->buf);
        if(FAILED(hr)){
            WARN("GetBuffer failed: %08lx\n", hr);
            This->update_frames = ~0;
            LeaveCriticalSection(&This->lock);
            return hr;
        }

        /* GetBuffer does not promise cleared storage. Every update is a fresh
         * mix, so initialize the complete interleaved transport bed before
         * accumulating individual object buffers into it. */
        memset(This->buf, 0, This->update_frames *
                This->stream_fmtex.Format.nBlockAlign);

        LIST_FOR_EACH_ENTRY(object, &This->objects, SpatialAudioObjectImpl, entry)
        {
            object->got_buffer = FALSE;
            object->end_of_stream = FALSE;
            if (object->active)
                memset(object->buf, 0, This->update_frames *
                        This->sa_client->object_fmtex.Format.nBlockAlign);
        }
    }else if (!fixme_once){
        fixme_once = TRUE;
        FIXME("Zero frame update.\n");
    }

    *dyn_count = 0;
    *frames = This->update_frames;

    LeaveCriticalSection(&This->lock);

    return S_OK;
}

static void mix_static_object(SpatialAudioStreamImpl *stream, SpatialAudioObjectImpl *object,
        UINT32 frames)
{
    unsigned int object_idx, frame;

    if (object->type == AudioObjectType_None)
    {
        float *out = stream->buf + stream->dry_channel;

        /* Keep non-spatialized objects on a private transport channel. The
         * backend mixes it into the physical front pair only after rendering
         * the spatial bed, so HRTF, distance, and room processing cannot color
         * dialogue or other listener-locked content. */
        assert(stream->stream_fmtex.dwChannelMask & SPATIAL_AUDIO_DRY_SPEAKER);
        for (frame = 0; frame < frames; ++frame)
        {
            *out += object->buf[frame] * object->volume;
            out += stream->stream_fmtex.Format.nChannels;
        }
        return;
    }

    for (object_idx = 0; object_idx < ARRAY_SIZE(static_objects); ++object_idx)
    {
        float *out;
        UINT32 channel;

        if (!(object->type & static_objects[object_idx].type))
            continue;

        channel = stream->static_object_map[AudioObjectType_to_index(static_objects[object_idx].type)];
        if (channel == ~0u)
        {
            WARN("Got unmapped static object type 0x%x.\n", static_objects[object_idx].type);
            continue;
        }

        out = stream->buf + channel;
        for (frame = 0; frame < frames; ++frame)
        {
            *out += object->buf[frame] * object->volume;
            out += stream->stream_fmtex.Format.nChannels;
        }
    }
}

static HRESULT WINAPI SAORS_EndUpdatingAudioObjects(ISpatialAudioObjectRenderStream *iface)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *object, *object2;
    struct list released_objects = LIST_INIT(released_objects);
    HRESULT hr = S_OK;

    TRACE("(%p)->()\n", This);

    EnterCriticalSection(&This->lock);

    if(This->update_frames == ~0){
        LeaveCriticalSection(&This->lock);
        return SPTLAUDCLNT_E_OUT_OF_ORDER;
    }

    if (This->update_frames > 0)
    {
        LIST_FOR_EACH_ENTRY_SAFE(object, object2, &This->objects, SpatialAudioObjectImpl, entry)
        {
            if (object->end_of_stream)
            {
                mix_static_object(This, object, object->end_of_stream_frames);
                object->end_of_stream = FALSE;
            }
            else if (object->active && object->got_buffer)
            {
                mix_static_object(This, object, This->update_frames);
            }
            else if (object->active)
            {
                TRACE("Implicitly ending object %p because it was not updated.\n", object);
                object->active = FALSE;
            }

            if (object->released)
            {
                list_remove(&object->entry);
                list_add_tail(&released_objects, &object->entry);
            }
        }

        hr = IAudioRenderClient_ReleaseBuffer(This->render, This->update_frames, 0);
        if (FAILED(hr))
            WARN("ReleaseBuffer failed: %08lx\n", hr);
    }
    else
    {
        LIST_FOR_EACH_ENTRY_SAFE(object, object2, &This->objects, SpatialAudioObjectImpl, entry)
        {
            if (object->released)
            {
                list_remove(&object->entry);
                list_add_tail(&released_objects, &object->entry);
            }
        }
    }

    This->update_frames = ~0;

    LeaveCriticalSection(&This->lock);

    LIST_FOR_EACH_ENTRY_SAFE(object, object2, &released_objects, SpatialAudioObjectImpl, entry)
    {
        list_remove(&object->entry);
        free(object->buf);
        free(object);
    }

    return hr;
}

static HRESULT WINAPI SAORS_ActivateSpatialAudioObject(ISpatialAudioObjectRenderStream *iface,
        AudioObjectType type, ISpatialAudioObject **object)
{
    SpatialAudioStreamImpl *This = impl_from_ISpatialAudioObjectRenderStream(iface);
    SpatialAudioObjectImpl *obj, *existing;

    TRACE("(%p)->(0x%x, %p)\n", This, type, object);

    if (!object)
        return E_POINTER;
    *object = NULL;

    if (type == AudioObjectType_Dynamic)
        return SPTLAUDCLNT_E_NO_MORE_OBJECTS;

    if (type != AudioObjectType_None && (type & ~NATIVE_STATIC_OBJECT_MASK))
        return E_INVALIDARG;

    if (type & ~This->params.StaticObjectTypeMask)
        return SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;

    obj = calloc(1, sizeof(*obj));
    if (!obj)
        return E_OUTOFMEMORY;

    obj->ISpatialAudioObject_iface.lpVtbl = &ISpatialAudioObject_vtbl;
    obj->ref = 1;
    obj->type = type;
    obj->volume = 1.0f;
    obj->active = TRUE;

    obj->sa_stream = This;
    obj->buf = calloc(This->period_frames, This->sa_client->object_fmtex.Format.nBlockAlign);
    if (!obj->buf)
    {
        free(obj);
        return E_OUTOFMEMORY;
    }

    EnterCriticalSection(&This->lock);

    LIST_FOR_EACH_ENTRY(existing, &This->objects, SpatialAudioObjectImpl, entry)
    {
        if (existing->type & type)
        {
            LeaveCriticalSection(&This->lock);
            free(obj->buf);
            free(obj);
            return SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE;
        }
    }

    SAORS_AddRef(&This->ISpatialAudioObjectRenderStream_iface);
    list_add_tail(&This->objects, &obj->entry);

    LeaveCriticalSection(&This->lock);

    *object = &obj->ISpatialAudioObject_iface;

    return S_OK;
}

static ISpatialAudioObjectRenderStreamVtbl ISpatialAudioObjectRenderStream_vtbl = {
    SAORS_QueryInterface,
    SAORS_AddRef,
    SAORS_Release,
    SAORS_GetAvailableDynamicObjectCount,
    SAORS_GetService,
    SAORS_Start,
    SAORS_Stop,
    SAORS_Reset,
    SAORS_BeginUpdatingAudioObjects,
    SAORS_EndUpdatingAudioObjects,
    SAORS_ActivateSpatialAudioObject,
};

static HRESULT WINAPI SAC_QueryInterface(ISpatialAudioClient *iface, REFIID riid, void **ppv)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_ISpatialAudioClient)) {
        *ppv = &This->ISpatialAudioClient_iface;
    }
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI SAC_AddRef(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI SAC_Release(ISpatialAudioClient *iface)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if (!ref) {
        IMMDevice_Release(This->mmdev);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI SAC_GetStaticObjectPosition(ISpatialAudioClient *iface,
        AudioObjectType type, float *x, float *y, float *z)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    unsigned int i;

    TRACE("(%p)->(0x%x, %p, %p, %p)\n", This, type, x, y, z);

    if (!x || !y || !z)
        return E_POINTER;

    for (i = 0; i < ARRAY_SIZE(static_objects); ++i)
    {
        if (static_objects[i].type != type)
            continue;

        *x = static_objects[i].x;
        *y = static_objects[i].y;
        *z = static_objects[i].z;
        return S_OK;
    }

    return E_INVALIDARG;
}

static HRESULT WINAPI SAC_GetNativeStaticObjectTypeMask(ISpatialAudioClient *iface,
        AudioObjectType *mask)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, mask);

    if (!mask)
        return E_POINTER;

    *mask = NATIVE_STATIC_OBJECT_MASK;
    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxDynamicObjectCount(ISpatialAudioClient *iface,
        UINT32 *value)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    FIXME("(%p)->(%p)\n", This, value);

    if (!value)
        return E_POINTER;

    *value = 0;
    return S_OK;
}

static HRESULT WINAPI SAC_GetSupportedAudioObjectFormatEnumerator(
        ISpatialAudioClient *iface, IAudioFormatEnumerator **enumerator)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    TRACE("(%p)->(%p)\n", This, enumerator);

    if (!enumerator)
        return E_POINTER;

    *enumerator = &This->IAudioFormatEnumerator_iface;
    SAC_AddRef(iface);

    return S_OK;
}

static HRESULT WINAPI SAC_GetMaxFrameCount(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);

    /* FIXME: should get device period from the device */
    static const REFERENCE_TIME period = 100000;

    TRACE("(%p)->(%p, %p)\n", This, format, count);

    if (!format || !count)
        return E_POINTER;

    *count = MulDiv(period, format->nSamplesPerSec, 10000000);

    return S_OK;
}

static HRESULT WINAPI SAC_IsAudioObjectFormatSupported(ISpatialAudioClient *iface,
        const WAVEFORMATEX *format)
{
    SpatialAudioImpl *sac = impl_from_ISpatialAudioClient(iface);

    TRACE("sac %p, format %s.\n", sac, debugstr_fmtex(format));

    if (!format)
        return E_POINTER;

    if (!formats_equal(&sac->object_fmtex.Format, format))
    {
        FIXME("Reporting format %s as unsupported.\n", debugstr_fmtex(format));
        return E_INVALIDARG;
    }

    return S_OK;
}

static HRESULT WINAPI SAC_IsSpatialAudioStreamAvailable(ISpatialAudioClient *iface,
        REFIID stream_uuid, const PROPVARIANT *info)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    FIXME("(%p)->(%s, %p)\n", This, debugstr_guid(stream_uuid), info);
    return E_NOTIMPL;
}

static WAVEFORMATEX *clone_fmtex(const WAVEFORMATEX *src)
{
    WAVEFORMATEX *r = malloc(sizeof(WAVEFORMATEX) + src->cbSize);

    if (!r)
        return NULL;

    memcpy(r, src, sizeof(WAVEFORMATEX) + src->cbSize);
    return r;
}

static unsigned int count_bits(DWORD value)
{
    unsigned int count = 0;

    while (value)
    {
        value &= value - 1;
        ++count;
    }
    return count;
}

static HRESULT static_mask_to_channels(AudioObjectType static_mask, WORD *count,
        DWORD *mask, UINT32 *dry_channel, UINT32 *map)
{
    unsigned int i;

    if (static_mask & ~NATIVE_STATIC_OBJECT_MASK)
        return E_INVALIDARG;

    *count = 0;
    *mask = 0;

    for (i = 0; i < ARRAY_SIZE(static_objects); ++i)
    {
        UINT32 object_idx = AudioObjectType_to_index(static_objects[i].type);

        map[object_idx] = ~0u;
        if (!(static_mask & static_objects[i].type))
            continue;

        if (!static_objects[i].speaker)
            return E_INVALIDARG;

        *mask |= static_objects[i].speaker;
    }

    /* Front left/right keep the bed renderable on stereo endpoints even when
     * the application did not request those static objects. The private dry
     * slot carries AudioObjectType_None. The backend removes it from the bed
     * layout and mixes it into the final front pair after spatial rendering. */
    *mask |= SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPATIAL_AUDIO_DRY_SPEAKER;
    *count = count_bits(*mask);
    *dry_channel = count_bits(*mask & (SPATIAL_AUDIO_DRY_SPEAKER - 1));
    for (i = 0; i < ARRAY_SIZE(static_objects); ++i)
    {
        UINT32 object_idx;

        if (!(static_mask & static_objects[i].type))
            continue;

        object_idx = AudioObjectType_to_index(static_objects[i].type);
        map[object_idx] = count_bits(*mask & (static_objects[i].speaker - 1));
        TRACE("Mapping object type 0x%x to channel %u.\n",
                static_objects[i].type, map[object_idx]);
    }

    return S_OK;
}

static HRESULT activate_stream(SpatialAudioStreamImpl *stream)
{
    WAVEFORMATEXTENSIBLE *object_fmtex = (WAVEFORMATEXTENSIBLE *)stream->params.ObjectFormat;
    HRESULT hr;
    REFERENCE_TIME period;

    if(!(object_fmtex->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (object_fmtex->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 IsEqualGUID(&object_fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)))){
        FIXME("Only float formats are supported for now\n");
        return E_INVALIDARG;
    }

    hr = IMMDevice_Activate(stream->sa_client->mmdev, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&stream->client);
    if(FAILED(hr)){
        WARN("Activate failed: %08lx\n", hr);
        return hr;
    }

    hr = IAudioClient_GetDevicePeriod(stream->client, &period, NULL);
    if(FAILED(hr)){
        WARN("GetDevicePeriod failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    stream->stream_fmtex.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    hr = static_mask_to_channels(stream->params.StaticObjectTypeMask,
            &stream->stream_fmtex.Format.nChannels, &stream->stream_fmtex.dwChannelMask,
            &stream->dry_channel, stream->static_object_map);
    if (FAILED(hr))
    {
        IAudioClient_Release(stream->client);
        stream->client = NULL;
        return hr;
    }
    stream->stream_fmtex.Format.nSamplesPerSec = stream->params.ObjectFormat->nSamplesPerSec;
    stream->stream_fmtex.Format.wBitsPerSample = stream->params.ObjectFormat->wBitsPerSample;
    stream->stream_fmtex.Format.nBlockAlign = (stream->stream_fmtex.Format.nChannels * stream->stream_fmtex.Format.wBitsPerSample) / 8;
    stream->stream_fmtex.Format.nAvgBytesPerSec = stream->stream_fmtex.Format.nSamplesPerSec * stream->stream_fmtex.Format.nBlockAlign;
    stream->stream_fmtex.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    stream->stream_fmtex.Samples.wValidBitsPerSample = stream->stream_fmtex.Format.wBitsPerSample;
    stream->stream_fmtex.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    hr = audio_client_initialize_spatial(stream->client,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            period, 0, &stream->stream_fmtex.Format, NULL,
            stream->params.StaticObjectTypeMask);
    if(FAILED(hr)){
        WARN("Initialize failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_SetEventHandle(stream->client, stream->params.EventHandle);
    if(FAILED(hr)){
        WARN("SetEventHandle failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    hr = IAudioClient_GetService(stream->client, &IID_IAudioRenderClient, (void**)&stream->render);
    if(FAILED(hr)){
        WARN("GetService(AudioRenderClient) failed: %08lx\n", hr);
        IAudioClient_Release(stream->client);
        return hr;
    }

    stream->period_frames = MulDiv(period, stream->stream_fmtex.Format.nSamplesPerSec, 10000000);

    return S_OK;
}

static HRESULT WINAPI SAC_ActivateSpatialAudioStream(ISpatialAudioClient *iface,
        const PROPVARIANT *prop, REFIID riid, void **stream)
{
    SpatialAudioImpl *This = impl_from_ISpatialAudioClient(iface);
    SpatialAudioObjectRenderStreamActivationParams *params;
    HRESULT hr;

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), stream);

    if (!stream)
        return E_POINTER;
    *stream = NULL;

    if(IsEqualIID(riid, &IID_ISpatialAudioObjectRenderStream)){
        SpatialAudioStreamImpl *obj;

        if (!prop || prop->vt != VT_BLOB ||
                prop->blob.cbSize != sizeof(SpatialAudioObjectRenderStreamActivationParams) ||
                !prop->blob.pBlobData)
        {
            WARN("Got invalid params\n");
            return E_INVALIDARG;
        }

        params = (SpatialAudioObjectRenderStreamActivationParams*) prop->blob.pBlobData;

        TRACE("Activation params: format {%s}, static mask %#x, dynamic %u..%u, "
                "category %u, event %p, notify %p.\n", debugstr_fmtex(params->ObjectFormat),
                params->StaticObjectTypeMask, params->MinDynamicObjectCount,
                params->MaxDynamicObjectCount, params->Category, params->EventHandle,
                params->NotifyObject);

        if ((params->StaticObjectTypeMask & ~NATIVE_STATIC_OBJECT_MASK) ||
                params->MinDynamicObjectCount || params->MaxDynamicObjectCount)
        {
            TRACE("Rejecting unavailable static or dynamic objects.\n");
            return E_INVALIDARG;
        }

        if(params->EventHandle == INVALID_HANDLE_VALUE ||
                params->EventHandle == 0){
            TRACE("Rejecting invalid event handle %p.\n", params->EventHandle);
            return E_INVALIDARG;
        }

        if(!(params->ObjectFormat && formats_equal(params->ObjectFormat, &This->object_fmtex.Format))) {
            TRACE("Rejecting unsupported object format; expected {%s}.\n",
                    debugstr_fmtex(&This->object_fmtex.Format));
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }

        obj = calloc(1, sizeof(SpatialAudioStreamImpl));
        if (!obj)
            return E_OUTOFMEMORY;

        obj->ISpatialAudioObjectRenderStream_iface.lpVtbl = &ISpatialAudioObjectRenderStream_vtbl;
        obj->ref = 1;
        memcpy(&obj->params, params, sizeof(obj->params));

        obj->update_frames = ~0;

        InitializeCriticalSection(&obj->lock);
        list_init(&obj->objects);

        obj->sa_client = This;
        SAC_AddRef(&This->ISpatialAudioClient_iface);

        obj->params.ObjectFormat = clone_fmtex(obj->params.ObjectFormat);
        if (!obj->params.ObjectFormat)
        {
            DeleteCriticalSection(&obj->lock);
            ISpatialAudioClient_Release(&obj->sa_client->ISpatialAudioClient_iface);
            free(obj);
            return E_OUTOFMEMORY;
        }

        if (!DuplicateHandle(GetCurrentProcess(), obj->params.EventHandle,
                GetCurrentProcess(), &obj->params.EventHandle, 0, FALSE,
                DUPLICATE_SAME_ACCESS))
        {
            DeleteCriticalSection(&obj->lock);
            free((void *)obj->params.ObjectFormat);
            ISpatialAudioClient_Release(&obj->sa_client->ISpatialAudioClient_iface);
            free(obj);
            return HRESULT_FROM_WIN32(GetLastError());
        }

        if(obj->params.NotifyObject)
            ISpatialAudioObjectRenderStreamNotify_AddRef(obj->params.NotifyObject);

        if(TRACE_ON(mmdevapi)){
            TRACE("ObjectFormat: {%s}\n", debugstr_fmtex(obj->params.ObjectFormat));
            TRACE("StaticObjectTypeMask: 0x%x\n", obj->params.StaticObjectTypeMask);
            TRACE("MinDynamicObjectCount: 0x%x\n", obj->params.MinDynamicObjectCount);
            TRACE("MaxDynamicObjectCount: 0x%x\n", obj->params.MaxDynamicObjectCount);
            TRACE("Category: 0x%x\n", obj->params.Category);
            TRACE("EventHandle: %p\n", obj->params.EventHandle);
            TRACE("NotifyObject: %p\n", obj->params.NotifyObject);
        }

        hr = activate_stream(obj);
        if(FAILED(hr)){
            if(obj->params.NotifyObject)
                ISpatialAudioObjectRenderStreamNotify_Release(obj->params.NotifyObject);
            DeleteCriticalSection(&obj->lock);
            free((void*)obj->params.ObjectFormat);
            CloseHandle(obj->params.EventHandle);
            ISpatialAudioClient_Release(&obj->sa_client->ISpatialAudioClient_iface);
            free(obj);
            return hr;
        }

        *stream = &obj->ISpatialAudioObjectRenderStream_iface;
    }else{
        FIXME("Unsupported audio stream IID: %s\n", debugstr_guid(riid));
        return E_NOTIMPL;
    }

    return S_OK;
}

static ISpatialAudioClientVtbl ISpatialAudioClient_vtbl = {
    SAC_QueryInterface,
    SAC_AddRef,
    SAC_Release,
    SAC_GetStaticObjectPosition,
    SAC_GetNativeStaticObjectTypeMask,
    SAC_GetMaxDynamicObjectCount,
    SAC_GetSupportedAudioObjectFormatEnumerator,
    SAC_GetMaxFrameCount,
    SAC_IsAudioObjectFormatSupported,
    SAC_IsSpatialAudioStreamAvailable,
    SAC_ActivateSpatialAudioStream,
};

static HRESULT WINAPI SAOFE_QueryInterface(IAudioFormatEnumerator *iface,
        REFIID riid, void **ppvObject)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_QueryInterface(&This->ISpatialAudioClient_iface, riid, ppvObject);
}

static ULONG WINAPI SAOFE_AddRef(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_AddRef(&This->ISpatialAudioClient_iface);
}

static ULONG WINAPI SAOFE_Release(IAudioFormatEnumerator *iface)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);
    return SAC_Release(&This->ISpatialAudioClient_iface);
}

static HRESULT WINAPI SAOFE_GetCount(IAudioFormatEnumerator *iface, UINT32 *count)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%p)\n", This, count);

    if (!count)
        return E_POINTER;

    *count = 1;
    return S_OK;
}

static HRESULT WINAPI SAOFE_GetFormat(IAudioFormatEnumerator *iface,
        UINT32 index, WAVEFORMATEX **format)
{
    SpatialAudioImpl *This = impl_from_IAudioFormatEnumerator(iface);

    TRACE("(%p)->(%u, %p)\n", This, index, format);

    if (!format)
        return E_POINTER;

    *format = NULL;
    if(index > 0)
        return E_INVALIDARG;

    *format = &This->object_fmtex.Format;

    return S_OK;
}

static IAudioFormatEnumeratorVtbl IAudioFormatEnumerator_vtbl = {
    SAOFE_QueryInterface,
    SAOFE_AddRef,
    SAOFE_Release,
    SAOFE_GetCount,
    SAOFE_GetFormat,
};

HRESULT SpatialAudioClient_Create(IMMDevice *mmdev, ISpatialAudioClient **out)
{
    SpatialAudioImpl *obj;

    if (!out)
        return E_POINTER;
    *out = NULL;

    if (!(obj = calloc(1, sizeof(*obj))))
        return E_OUTOFMEMORY;

    obj->ref = 1;
    obj->ISpatialAudioClient_iface.lpVtbl = &ISpatialAudioClient_vtbl;
    obj->IAudioFormatEnumerator_iface.lpVtbl = &IAudioFormatEnumerator_vtbl;

    obj->object_fmtex.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    obj->object_fmtex.Format.nChannels = 1;
    obj->object_fmtex.Format.nSamplesPerSec = 48000;
    obj->object_fmtex.Format.wBitsPerSample = sizeof(float) * 8;
    obj->object_fmtex.Format.nBlockAlign = (obj->object_fmtex.Format.nChannels * obj->object_fmtex.Format.wBitsPerSample) / 8;
    obj->object_fmtex.Format.nAvgBytesPerSec = obj->object_fmtex.Format.nSamplesPerSec * obj->object_fmtex.Format.nBlockAlign;
    obj->object_fmtex.Format.cbSize = 0;

    obj->mmdev = mmdev;
    IMMDevice_AddRef(mmdev);

    *out = &obj->ISpatialAudioClient_iface;

    return S_OK;
}
