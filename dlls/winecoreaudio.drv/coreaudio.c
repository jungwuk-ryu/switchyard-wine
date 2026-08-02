/*
 * Unixlib for winecoreaudio driver.
 *
 * Copyright 2011 Andrew Eikum for CodeWeavers
 * Copyright 2021 Huw Davies
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
#if 0
#pragma makedep unix
#endif

#include "config.h"

#define LoadResource __carbon_LoadResource
#define CompareString __carbon_CompareString
#define GetCurrentThread __carbon_GetCurrentThread
#define GetCurrentProcess __carbon_GetCurrentProcess

#include <stdarg.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <fenv.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <unistd.h>

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioFormat.h>
#include <AudioToolbox/AudioConverter.h>
#include <AudioUnit/AudioUnit.h>
#include <os/lock.h>

#undef LoadResource
#undef CompareString
#undef GetCurrentThread
#undef GetCurrentProcess
#undef _CDECL

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winternl.h"
#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"
#include "spatialaudioclient.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unixlib.h"

#if !defined(MAC_OS_VERSION_12_0) || MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_VERSION_12_0
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

WINE_DEFAULT_DEBUG_CHANNEL(coreaudio);

#define SPATIAL_AUDIO_MAX_DYNAMIC_OBJECTS 64
#define SPATIAL_AUDIO_PROFILE_SAMPLES 65536
#define SPATIAL_AUDIO_CACHE_LINE_SIZE 64

struct coreaudio_stream;

struct spatial_input_context
{
    struct coreaudio_stream *stream;
    UINT32 slot;
};

struct coreaudio_spatial_object_state
{
    float azimuth;
    float elevation;
    float distance;
    UINT32 active_frames;
};

struct coreaudio_stream
{
    os_unfair_lock lock;
    AudioComponentInstance unit;
    AudioComponentInstance spatial_unit;
    AudioConverterRef converter;
    AudioStreamBasicDescription dev_desc; /* audio unit format, not necessarily the same as fmt */
    AudioDeviceID dev_id;
    BOOL follows_default;
    BOOL unit_initialized;
    BOOL spatial_unit_initialized;
    BOOL unit_started;
    BOOL spatial_volumes_are_unity;
    BOOL spatial_resetting;
    BOOL spatial;
    UINT32 spatial_static_mask;
    UINT32 spatial_dynamic_objects;
    UINT32 spatial_endpoint_generation;
    BOOL spatial_native_output;

    EDataFlow flow;
    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    HANDLE timer_thread;
    semaphore_t event_semaphore;
    BOOL event_semaphore_created;
    semaphore_t callback_drain_semaphore;
    BOOL callback_drain_semaphore_created;
    UINT32 event_pending;

    BOOL playing, please_quit;
    REFERENCE_TIME period;
    UINT32 period_frames;
    UINT32 bufsize_frames, resamp_bufsize_frames;
    UINT32 lcl_offs_frames, held_frames, wri_offs_frames, tmp_buffer_frames;
    UINT32 cap_bufsize_frames, cap_offs_frames, cap_held_frames;
    UINT32 wrap_bufsize_frames;
    UINT64 written_frames;
    INT32 getbuf_last;
    WAVEFORMATEX *fmt;
    UINT32 *spatial_volume_bits;
    float *spatial_bed_buffer;
    float *spatial_dry_buffer;
    float *spatial_dry_delay_buffer;
    float *spatial_dynamic_buffer;
    struct spatial_input_context *spatial_input_contexts;
    AudioUnitParameterEvent *spatial_parameter_events;
    struct coreaudio_spatial_object_state *spatial_object_states;
    UINT64 *spatial_metadata_sequences;
    UINT32 spatial_metadata_capacity;
    UINT32 spatial_parameter_event_capacity;
    UINT32 spatial_dynamic_channel;
    UINT32 spatial_render_frames;
    UINT64 spatial_read_frames __attribute__((aligned(64)));
    char spatial_read_frames_padding[SPATIAL_AUDIO_CACHE_LINE_SIZE -
            sizeof(UINT64)];
    UINT64 spatial_write_frames __attribute__((aligned(64)));
    char spatial_write_frames_padding[SPATIAL_AUDIO_CACHE_LINE_SIZE -
            sizeof(UINT64)];
    UINT32 spatial_callbacks_inflight __attribute__((aligned(64)));
    UINT32 spatial_callback_active;
    char spatial_callback_padding[SPATIAL_AUDIO_CACHE_LINE_SIZE -
            2 * sizeof(UINT32)];
    UINT32 device_callbacks_inflight __attribute__((aligned(64)));
    char device_callback_padding[SPATIAL_AUDIO_CACHE_LINE_SIZE -
            sizeof(UINT32)];
    BOOL shutting_down;
    BOOL invalidated;
    UINT32 device_listener_mask;
    dispatch_queue_t device_listener_queue;
    AudioObjectPropertyListenerBlock device_listener;
    dispatch_block_t device_listener_detach;
    UINT64 underruns;
    UINT64 overruns;
    UINT64 reentrant_callbacks;
    BOOL profile_callbacks;
    UINT32 callback_timing_count;
    UINT64 *callback_timings;
    UINT32 spatial_bed_channels;
    UINT32 spatial_dry_capacity;
    UINT32 spatial_dry_channel;
    UINT32 spatial_dry_delay_frames;
    UINT32 spatial_dry_delay_pos;
    UINT32 spatial_dry_output[2];
    UINT32 spatial_dry_output_count;
    BYTE *local_buffer, *cap_buffer, *wrap_buffer, *resamp_buffer, *tmp_buffer;
};

static AudioDeviceID default_output_id = kAudioObjectUnknown;

static ULONG_PTR zero_bits = 0;

static const AudioObjectPropertyAddress spatial_device_properties[] =
{
    {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain},
    {kAudioDevicePropertyDeviceHasChanged, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain},
    {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain},
    {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain},
    {kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain},
    {kAudioDevicePropertyIOStoppedAbnormally, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain},
};

static NTSTATUS unix_get_mix_format(void *args);
static AudioDeviceID dev_id_from_device(const char *device);
static UINT ca_channel_layout_to_channel_mask(const AudioChannelLayout *layout);
static UINT32 count_channel_mask_bits(DWORD mask);
static void wait_for_spatial_callbacks(struct coreaudio_stream *stream);
static void wait_for_device_callbacks(struct coreaudio_stream *stream);

static NTSTATUS unix_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

static HRESULT osstatus_to_hresult(OSStatus sc)
{
    switch(sc){
    case kAudioFormatUnsupportedDataFormatError:
    case kAudioFormatUnknownFormatError:
    case kAudioDeviceUnsupportedFormatError:
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    case kAudioHardwareBadDeviceError:
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
    return E_FAIL;
}

static struct coreaudio_stream *handle_get_stream(stream_handle h)
{
    return (struct coreaudio_stream *)(UINT_PTR)h;
}

/* copied from kernelbase */
static int muldiv( int a, int b, int c )
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static AudioObjectPropertyScope get_scope(EDataFlow flow)
{
    return (flow == eRender) ? kAudioDevicePropertyScopeOutput : kAudioDevicePropertyScopeInput;
}

static BOOL device_has_channels(AudioDeviceID device, EDataFlow flow)
{
    AudioObjectPropertyAddress addr;
    AudioBufferList *buffers;
    BOOL ret = FALSE;
    OSStatus sc;
    UInt32 size;
    int i;

    addr.mSelector = kAudioDevicePropertyStreamConfiguration;
    addr.mScope = get_scope(flow);
    addr.mElement = 0;

    sc = AudioObjectGetPropertyDataSize(device, &addr, 0, NULL, &size);
    if(sc != noErr){
        WARN("Unable to get _StreamConfiguration property size for device %u: %x\n",
             (unsigned int)device, (int)sc);
        return FALSE;
    }

    buffers = malloc(size);
    if(!buffers) return FALSE;

    sc = AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, buffers);
    if(sc != noErr){
        WARN("Unable to get _StreamConfiguration property for device %u: %x\n",
             (unsigned int)device, (int)sc);
        free(buffers);
        return FALSE;
    }

    for(i = 0; i < buffers->mNumberBuffers; i++){
        if(buffers->mBuffers[i].mNumberChannels > 0){
            ret = TRUE;
            break;
        }
    }
    free(buffers);
    return ret;
}

static NTSTATUS unix_get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *params = args;
    unsigned int num_devices, i, needed, offset;
    AudioDeviceID *devices, default_id;
    AudioObjectPropertyAddress addr;
    struct endpoint *endpoint;
    UInt32 devsize, size;
    struct endpoint_info
    {
        CFStringRef name;
        CFStringRef uid;
        AudioDeviceID id;
    } *info;
    OSStatus sc;
    UniChar *ptr;

    params->num = 0;
    params->default_idx = 0;

    addr.mScope = kAudioObjectPropertyScopeGlobal;
    addr.mElement = kAudioObjectPropertyElementMain;
    if(params->flow == eRender) addr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
    else if(params->flow == eCapture) addr.mSelector = kAudioHardwarePropertyDefaultInputDevice;
    else{
        params->result = E_INVALIDARG;
        return STATUS_SUCCESS;
    }

    size = sizeof(default_id);
    sc = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &default_id);
    if(sc != noErr){
        WARN("Getting _DefaultInputDevice property failed: %x\n", (int)sc);
        default_id = -1;
    }
    else if(params->flow == eRender)
        default_output_id = default_id;

    addr.mSelector = kAudioHardwarePropertyDevices;
    sc = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &devsize);
    if(sc != noErr){
        WARN("Getting _Devices property size failed: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }

    num_devices = devsize / sizeof(AudioDeviceID);
    devices = malloc(devsize);
    info = malloc(num_devices * sizeof(*info));
    if(!devices || !info){
        free(info);
        free(devices);
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    sc = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &devsize, devices);
    if(sc != noErr){
        WARN("Getting _Devices property failed: %x\n", (int)sc);
        free(info);
        free(devices);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }

    addr.mScope = get_scope(params->flow);
    addr.mElement = 0;

    for(i = 0; i < num_devices; i++){
        if(!device_has_channels(devices[i], params->flow)) continue;

        addr.mSelector = kAudioObjectPropertyName;
        size = sizeof(CFStringRef);
        sc = AudioObjectGetPropertyData(devices[i], &addr, 0, NULL, &size, &info[params->num].name);
        if(sc != noErr){
            WARN("Unable to get _Name property for device %u: %x\n",
                 (unsigned int)devices[i], (int)sc);
            continue;
        }

        addr.mSelector = kAudioDevicePropertyDeviceUID;
        size = sizeof(CFStringRef);
        sc = AudioObjectGetPropertyData(devices[i], &addr, 0, NULL, &size, &info[params->num].uid);
        if(sc != noErr){
            WARN("Unable to get UID property for device %u: %x\n",
                 (unsigned int)devices[i], (int)sc);
            continue;
        }

        info[params->num++].id = devices[i];
    }
    free(devices);

    offset = needed = sizeof(*endpoint) * params->num;
    endpoint = params->endpoints;

    for(i = 0; i < params->num; i++){
        const SIZE_T name_len = CFStringGetLength(info[i].name) + 1;
        CFIndex device_len;

        CFStringGetBytes(info[i].uid, CFRangeMake(0, CFStringGetLength(info[i].uid)), kCFStringEncodingUTF8,
                         0, false, NULL, 0, &device_len);
        device_len++;   /* for null terminator */

        needed += name_len * sizeof(WCHAR) + ((device_len + 1) & ~1);

        if(needed <= params->size){
            endpoint->name = offset;
            ptr = (UniChar *)((char *)params->endpoints + offset);
            CFStringGetCharacters(info[i].name, CFRangeMake(0, name_len - 1), ptr);
            ptr[name_len - 1] = 0;
            offset += name_len * sizeof(WCHAR);

            endpoint->device = offset;
            CFStringGetBytes(info[i].uid, CFRangeMake(0, CFStringGetLength(info[i].uid)), kCFStringEncodingUTF8,
                             0, false, (UInt8 *)params->endpoints + offset, params->size - offset, NULL);
            ((char *)params->endpoints)[offset + device_len - 1] = '\0';
            offset += (device_len + 1) & ~1;

            endpoint++;
        }
        CFRelease(info[i].name);
        CFRelease(info[i].uid);
        if(info[i].id == default_id) params->default_idx = i;
    }
    free(info);

    if(needed > params->size){
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    else params->result = S_OK;

    return STATUS_SUCCESS;
}

static WAVEFORMATEX *clone_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEX *ret;
    size_t size;

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc(size);
    if(!ret)
        return NULL;

    memcpy(ret, fmt, size);

    ret->cbSize = size - sizeof(WAVEFORMATEX);

    return ret;
}

static void silence_buffer(struct coreaudio_stream *stream, BYTE *buffer, UINT32 frames)
{
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE*)stream->fmt;
    if((stream->fmt->wFormatTag == WAVE_FORMAT_PCM ||
        (stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))) &&
       stream->fmt->wBitsPerSample == 8)
        memset(buffer, 128, frames * stream->fmt->nBlockAlign);
    else
        memset(buffer, 0, frames * stream->fmt->nBlockAlign);
}

/* CA is pulling data from us */
static OSStatus ca_render_cb(void *user, AudioUnitRenderActionFlags *flags,
        const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
        AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    UINT32 to_copy_bytes, to_copy_frames, chunk_bytes, lcl_offs_bytes;

    os_unfair_lock_lock(&stream->lock);

    if(stream->playing){
        lcl_offs_bytes = stream->lcl_offs_frames * stream->fmt->nBlockAlign;
        to_copy_frames = min(nframes, stream->held_frames);
        to_copy_bytes = to_copy_frames * stream->fmt->nBlockAlign;

        chunk_bytes = (stream->bufsize_frames - stream->lcl_offs_frames) * stream->fmt->nBlockAlign;

        if(to_copy_bytes > chunk_bytes){
            memcpy(data->mBuffers[0].mData, stream->local_buffer + lcl_offs_bytes, chunk_bytes);
            memcpy(((BYTE *)data->mBuffers[0].mData) + chunk_bytes, stream->local_buffer, to_copy_bytes - chunk_bytes);
        }else
            memcpy(data->mBuffers[0].mData, stream->local_buffer + lcl_offs_bytes, to_copy_bytes);

        stream->lcl_offs_frames += to_copy_frames;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->held_frames -= to_copy_frames;
    }else
        to_copy_bytes = to_copy_frames = 0;

    if(nframes > to_copy_frames)
        silence_buffer(stream, ((BYTE *)data->mBuffers[0].mData) + to_copy_bytes, nframes - to_copy_frames);

    os_unfair_lock_unlock(&stream->lock);

    return noErr;
}

static float load_spatial_volume(struct coreaudio_stream *stream, UINT32 channel)
{
    UINT32 bits = __atomic_load_n(&stream->spatial_volume_bits[channel],
            __ATOMIC_RELAXED);
    float volume;

    memcpy(&volume, &bits, sizeof(volume));
    return volume;
}

static void spatial_position_to_parameters(
        const struct spatial_audio_object_state *source,
        struct coreaudio_spatial_object_state *target)
{
    double horizontal, radius;

    horizontal = hypot((double)source->x, (double)source->z);
    radius = hypot(horizontal, (double)source->y);
    if (!isfinite(horizontal) || !isfinite(radius))
    {
        target->azimuth = target->elevation = target->distance = 0.0f;
        target->active_frames = 0;
        return;
    }

    target->azimuth = atan2((double)source->x, -(double)source->z) *
            (180.0 / M_PI);
    target->elevation = atan2((double)source->y, horizontal) *
            (180.0 / M_PI);
    target->distance = min(radius, 10000.0);
    target->active_frames = source->active_frames;
}

static BOOL append_spatial_parameter_event(struct coreaudio_stream *stream,
        UINT32 *event_count, UINT32 slot, AudioUnitParameterID parameter,
        UINT32 offset, float value)
{
    AudioUnitParameterEvent *event;

    if (*event_count >= stream->spatial_parameter_event_capacity)
        return FALSE;
    event = &stream->spatial_parameter_events[(*event_count)++];
    event->scope = kAudioUnitScope_Input;
    event->element = slot + 1;
    event->parameter = parameter;
    event->eventType = kParameterEvent_Immediate;
    event->eventValues.immediate.bufferOffset = offset;
    event->eventValues.immediate.value = value;
    return TRUE;
}

static OSStatus schedule_spatial_positions(struct coreaudio_stream *stream,
        UINT64 read_frame, UINT32 frames)
{
    UINT32 event_count = 0, offset = 0;

    while (offset < frames)
    {
        UINT64 absolute = read_frame + offset;
        UINT64 sequence = absolute / stream->period_frames;
        UINT32 record = sequence % stream->spatial_metadata_capacity;
        UINT32 next = min(frames, offset + stream->period_frames -
                absolute % stream->period_frames);
        const struct coreaudio_spatial_object_state *states;
        UINT32 slot;

        if (__atomic_load_n(&stream->spatial_metadata_sequences[record],
                __ATOMIC_ACQUIRE) != sequence)
            return kAudio_ParamError;
        states = stream->spatial_object_states +
                (size_t)record * stream->spatial_dynamic_objects;

        for (slot = 0; slot < stream->spatial_dynamic_objects; ++slot)
        {
            const struct coreaudio_spatial_object_state *state = &states[slot];

            if (!append_spatial_parameter_event(stream, &event_count, slot,
                    kSpatialMixerParam_Azimuth, offset, state->azimuth) ||
                    !append_spatial_parameter_event(stream, &event_count, slot,
                    kSpatialMixerParam_Elevation, offset, state->elevation) ||
                    !append_spatial_parameter_event(stream, &event_count, slot,
                    kSpatialMixerParam_Distance, offset, state->distance))
                return kAudio_ParamError;
        }
        offset = next;
    }

    return event_count ? AudioUnitScheduleParameters(stream->spatial_unit,
            stream->spatial_parameter_events, event_count) : noErr;
}

static void signal_spatial_event(struct coreaudio_stream *stream)
{
    UINT32 expected = 0;

    if (stream->event_semaphore_created &&
            __atomic_compare_exchange_n(&stream->event_pending, &expected, 1,
                    FALSE, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        semaphore_signal(stream->event_semaphore);
}

static void record_spatial_callback_time(struct coreaudio_stream *stream,
        UINT64 started)
{
    UINT32 index;

    if (!started)
        return;
    index = __atomic_fetch_add(&stream->callback_timing_count, 1,
            __ATOMIC_RELAXED);
    if (index < SPATIAL_AUDIO_PROFILE_SAMPLES)
        stream->callback_timings[index] = mach_absolute_time() - started;
}

static void spatial_device_property_changed(struct coreaudio_stream *stream,
        UInt32 count, const AudioObjectPropertyAddress addresses[])
{
    UInt32 i;

    __atomic_add_fetch(&stream->device_callbacks_inflight, 1,
            __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&stream->shutting_down, __ATOMIC_ACQUIRE))
        goto done;

    for (i = 0; i < count; ++i)
    {
        __atomic_store_n(&stream->invalidated, TRUE, __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
        signal_spatial_event(stream);
done:
    __atomic_sub_fetch(&stream->device_callbacks_inflight, 1,
            __ATOMIC_RELEASE);
}

static HRESULT register_spatial_device_listeners(struct coreaudio_stream *stream)
{
    __block struct coreaudio_stream *target = stream;
    AudioObjectPropertyListenerBlock listener;
    dispatch_block_t detach;
    BOOL topology_listener = FALSE;
    UINT32 i;
    OSStatus sc;

    if (!(stream->device_listener_queue = dispatch_queue_create(
            "org.winehq.wine.coreaudio.spatial-device", DISPATCH_QUEUE_SERIAL)))
        return E_OUTOFMEMORY;
    listener = Block_copy(^(UInt32 count,
            const AudioObjectPropertyAddress addresses[])
    {
        struct coreaudio_stream *current = target;

        if (current)
            spatial_device_property_changed(current, count, addresses);
    });
    detach = Block_copy(^{ target = NULL; });
    if (!listener || !detach)
    {
        if (listener) Block_release(listener);
        if (detach) Block_release(detach);
        return E_OUTOFMEMORY;
    }
    stream->device_listener = listener;
    stream->device_listener_detach = detach;

    for (i = 0; i < ARRAY_SIZE(spatial_device_properties); ++i)
    {
        sc = AudioObjectAddPropertyListenerBlock(stream->dev_id,
                &spatial_device_properties[i], stream->device_listener_queue,
                stream->device_listener);
        if (sc == noErr)
        {
            stream->device_listener_mask |= 1u << i;
            if (i == 1 || i == 4)
                topology_listener = TRUE;
        }
        else if (i == 0 || i == 2 || i == 3)
        {
            WARN("Failed to register required spatial device listener %u: %x.\n",
                    i, (int)sc);
            return osstatus_to_hresult(sc);
        }
    }
    if (!topology_listener)
    {
        WARN("Failed to register a spatial topology-change listener.\n");
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
    return S_OK;
}

static void dispatch_barrier_noop(void *context)
{
    (void)context;
}

static void dispatch_invoke_block(void *context)
{
    ((dispatch_block_t)context)();
}

static void unregister_spatial_device_listeners(struct coreaudio_stream *stream)
{
    UINT32 i;
    OSStatus sc;

    if (stream->device_listener)
        for (i = 0; i < ARRAY_SIZE(spatial_device_properties); ++i)
            if (stream->device_listener_mask & (1u << i))
            {
                sc = AudioObjectRemovePropertyListenerBlock(stream->dev_id,
                        &spatial_device_properties[i],
                        stream->device_listener_queue,
                        stream->device_listener);
                if (sc != noErr)
                    WARN("Failed to remove spatial device listener %u: %x.\n",
                            i, (int)sc);
            }
    stream->device_listener_mask = 0;
    /* Detach the captured stream on the listener's serial queue.  Even if a
     * dead device refuses listener removal, any callback retained by
     * CoreAudio after this barrier observes NULL instead of freed storage. */
    if (stream->device_listener_queue && stream->device_listener_detach)
        dispatch_sync_f(stream->device_listener_queue,
                stream->device_listener_detach, dispatch_invoke_block);
    else if (stream->device_listener_queue)
        dispatch_sync_f(stream->device_listener_queue, NULL,
                dispatch_barrier_noop);
    wait_for_device_callbacks(stream);
    if (stream->device_listener)
    {
        Block_release(stream->device_listener);
        stream->device_listener = NULL;
    }
    if (stream->device_listener_detach)
    {
        Block_release(stream->device_listener_detach);
        stream->device_listener_detach = NULL;
    }
    if (stream->device_listener_queue)
    {
        dispatch_release(stream->device_listener_queue);
        stream->device_listener_queue = NULL;
    }
}

/* Split the private interleaved transport exactly once for each output slice.
 * The output callback is the sole ring consumer, so no callback-side lock is
 * needed. The producer publishes metadata before the matching write cursor. */
static OSStatus ca_prepare_spatial_inputs(struct coreaudio_stream *stream,
        UInt32 nframes, UINT64 *consumed_read_frame, UINT32 *consumed_frames)
{
    const struct coreaudio_spatial_object_state *states = NULL;
    UINT64 read_frame, write_frame;
    UINT64 sequence = 0;
    UINT32 bed_channel, channel, frame, in_quantum = 0, record;
    UINT32 source_frame, to_copy_frames;
    BOOL unity, playing;

    if (nframes > stream->spatial_dry_capacity)
        return kAudio_ParamError;
    stream->spatial_render_frames = nframes;

    read_frame = __atomic_load_n(&stream->spatial_read_frames,
            __ATOMIC_RELAXED);
    write_frame = __atomic_load_n(&stream->spatial_write_frames,
            __ATOMIC_ACQUIRE);
    playing = __atomic_load_n(&stream->playing, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&stream->spatial_resetting, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE);
    to_copy_frames = playing && write_frame >= read_frame ?
            min((UINT64)nframes, write_frame - read_frame) : 0;
    if (playing && to_copy_frames < nframes)
        __atomic_add_fetch(&stream->underruns, 1, __ATOMIC_RELAXED);
    unity = __atomic_load_n(&stream->spatial_volumes_are_unity,
            __ATOMIC_ACQUIRE);
    source_frame = read_frame % stream->bufsize_frames;
    if (stream->spatial_dynamic_objects && to_copy_frames)
    {
        sequence = read_frame / stream->period_frames;
        in_quantum = read_frame - sequence * stream->period_frames;
        record = sequence % stream->spatial_metadata_capacity;
        if (__atomic_load_n(&stream->spatial_metadata_sequences[record],
                __ATOMIC_ACQUIRE) == sequence)
            states = stream->spatial_object_states +
                    (size_t)record * stream->spatial_dynamic_objects;
    }

    for (frame = 0; frame < to_copy_frames; ++frame)
    {
        const float *source;
        UINT32 slot;

        source = (const float *)stream->local_buffer +
                (size_t)source_frame * stream->fmt->nChannels;

        bed_channel = 0;
        for (channel = 0; channel < stream->spatial_dynamic_channel; ++channel)
        {
            float sample = source[channel];

            if (!unity)
                sample *= load_spatial_volume(stream, channel);
            if (channel == stream->spatial_dry_channel)
                stream->spatial_dry_buffer[frame] = sample;
            else
                stream->spatial_bed_buffer[frame *
                        stream->spatial_bed_channels + bed_channel++] = sample;
        }

        for (slot = 0; slot < stream->spatial_dynamic_objects; ++slot)
        {
            float sample = source[stream->spatial_dynamic_channel + slot];

            if (!states || in_quantum >= states[slot].active_frames)
                sample = 0.0f;
            else if (!unity)
                sample *= load_spatial_volume(stream,
                        stream->spatial_dynamic_channel + slot);
            stream->spatial_dynamic_buffer[(size_t)slot *
                    stream->spatial_dry_capacity + frame] = sample;
        }

        if (++source_frame == stream->bufsize_frames)
            source_frame = 0;
        if (stream->spatial_dynamic_objects &&
                ++in_quantum == stream->period_frames)
        {
            in_quantum = 0;
            ++sequence;
            record = sequence % stream->spatial_metadata_capacity;
            states = NULL;
            if (frame + 1 < to_copy_frames &&
                    __atomic_load_n(
                            &stream->spatial_metadata_sequences[record],
                            __ATOMIC_ACQUIRE) == sequence)
                states = stream->spatial_object_states +
                        (size_t)record * stream->spatial_dynamic_objects;
        }
    }

    if (nframes > to_copy_frames)
    {
        memset(stream->spatial_bed_buffer + (size_t)to_copy_frames *
                stream->spatial_bed_channels, 0, (size_t)(nframes -
                to_copy_frames) * stream->spatial_bed_channels * sizeof(float));
        memset(stream->spatial_dry_buffer + to_copy_frames, 0,
                (nframes - to_copy_frames) * sizeof(float));
        for (channel = 0; channel < stream->spatial_dynamic_objects; ++channel)
            memset(stream->spatial_dynamic_buffer + (size_t)channel *
                    stream->spatial_dry_capacity + to_copy_frames, 0,
                    (nframes - to_copy_frames) * sizeof(float));
    }

    if (stream->spatial_dynamic_objects && to_copy_frames &&
            schedule_spatial_positions(stream, read_frame, to_copy_frames) != noErr)
    {
        __atomic_store_n(&stream->invalidated, TRUE, __ATOMIC_RELEASE);
        signal_spatial_event(stream);
        return kAudio_ParamError;
    }
    *consumed_read_frame = read_frame;
    *consumed_frames = to_copy_frames;
    return noErr;
}

static void delay_spatial_dry_input(struct coreaudio_stream *stream,
        UInt32 nframes)
{
    UINT32 frame, pos;

    if (!stream->spatial_dry_delay_frames)
        return;
    pos = stream->spatial_dry_delay_pos;
    for (frame = 0; frame < nframes; ++frame)
    {
        float sample = stream->spatial_dry_buffer[frame];

        stream->spatial_dry_buffer[frame] =
                stream->spatial_dry_delay_buffer[pos];
        stream->spatial_dry_delay_buffer[pos] = sample;
        if (++pos == stream->spatial_dry_delay_frames)
            pos = 0;
    }
    stream->spatial_dry_delay_pos = pos;
}

static void commit_spatial_inputs(struct coreaudio_stream *stream,
        UINT64 read_frame, UINT32 frames)
{
    UINT64 write_frame, queued_frames;

    if (!frames)
        return;
    read_frame += frames;
    __atomic_store_n(&stream->spatial_read_frames, read_frame,
            __ATOMIC_RELEASE);

    /* CoreAudio may request slices smaller than the Windows update quantum,
     * especially while its output unit is converting sample rates.  Wake the
     * producer only after an entire quantum fits; otherwise BeginUpdating()
     * would be woken merely to fail GetBuffer(period_frames). */
    write_frame = __atomic_load_n(&stream->spatial_write_frames,
            __ATOMIC_ACQUIRE);
    queued_frames = write_frame >= read_frame ? write_frame - read_frame : 0;
    if (stream->bufsize_frames >= stream->period_frames &&
            queued_frames <= stream->bufsize_frames - stream->period_frames)
        signal_spatial_event(stream);
}

static OSStatus ca_spatial_bed_render_cb(void *user, AudioUnitRenderActionFlags *flags,
        const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes, AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    UInt32 bytes;

    (void)flags;
    (void)ts;
    (void)bus;

    if (nframes != stream->spatial_render_frames ||
            nframes > stream->spatial_dry_capacity)
        return kAudio_ParamError;
    bytes = nframes * stream->spatial_bed_channels * sizeof(float);

    if (data->mNumberBuffers != 1 ||
            data->mBuffers[0].mNumberChannels != stream->spatial_bed_channels)
        return kAudio_ParamError;
    if (!data->mBuffers[0].mData)
    {
        data->mBuffers[0].mData = stream->spatial_bed_buffer;
        data->mBuffers[0].mDataByteSize = bytes;
    }
    else if (data->mBuffers[0].mDataByteSize < bytes)
        return kAudio_ParamError;
    else
        memcpy(data->mBuffers[0].mData, stream->spatial_bed_buffer, bytes);
    return noErr;
}

static OSStatus ca_spatial_dynamic_render_cb(void *user,
        AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
        UInt32 nframes, AudioBufferList *data)
{
    struct spatial_input_context *context = user;
    struct coreaudio_stream *stream = context->stream;
    float *source;
    UInt32 bytes;

    (void)flags;
    (void)ts;
    (void)bus;

    if (context->slot >= stream->spatial_dynamic_objects ||
            nframes != stream->spatial_render_frames ||
            nframes > stream->spatial_dry_capacity || data->mNumberBuffers != 1 ||
            data->mBuffers[0].mNumberChannels != 1)
        return kAudio_ParamError;

    bytes = nframes * sizeof(float);
    source = stream->spatial_dynamic_buffer +
            (size_t)context->slot * stream->spatial_dry_capacity;
    if (!data->mBuffers[0].mData)
    {
        data->mBuffers[0].mData = source;
        data->mBuffers[0].mDataByteSize = bytes;
    }
    else if (data->mBuffers[0].mDataByteSize < bytes)
        return kAudio_ParamError;
    else
        memcpy(data->mBuffers[0].mData, source, bytes);
    return noErr;
}

static void mix_spatial_dry_planar(struct coreaudio_stream *stream,
        UInt32 nframes, AudioBufferList *data)
{
    UINT32 channel, frame;

    for (channel = 0; channel < stream->spatial_dry_output_count; ++channel)
    {
        UINT32 index = stream->spatial_dry_output[channel];
        float *output = data->mBuffers[index].mData;

        for (frame = 0; frame < nframes; ++frame)
            output[frame] += stream->spatial_dry_buffer[frame];
    }
}

static OSStatus ca_spatial_render_cb(void *user, AudioUnitRenderActionFlags *flags,
        const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes, AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    UINT64 consumed_read_frame = 0, started = 0;
    UINT32 consumed_frames = 0;
    UINT32 expected = 0;
    unsigned int i;
    OSStatus result = noErr;

    (void)bus;

    __atomic_add_fetch(&stream->spatial_callbacks_inflight, 1,
            __ATOMIC_ACQUIRE);
    if (!__atomic_compare_exchange_n(&stream->spatial_callback_active,
            &expected, 1, FALSE, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        __atomic_add_fetch(&stream->reentrant_callbacks, 1, __ATOMIC_RELAXED);
        result = kAudioUnitErr_CannotDoInCurrentContext;
        goto done_inflight;
    }
    if (stream->profile_callbacks)
        started = mach_absolute_time();

    if (nframes > stream->spatial_dry_capacity ||
            __atomic_load_n(&stream->shutting_down, __ATOMIC_ACQUIRE))
    {
        result = kAudio_ParamError;
        goto done;
    }

    if (!__atomic_load_n(&stream->playing, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&stream->spatial_resetting, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
    {
        for (i = 0; i < data->mNumberBuffers; ++i)
            if (data->mBuffers[i].mData)
                memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
        goto done;
    }

    if ((result = ca_prepare_spatial_inputs(stream, nframes,
            &consumed_read_frame, &consumed_frames)) != noErr)
    {
        for (i = 0; i < data->mNumberBuffers; ++i)
            if (data->mBuffers[i].mData)
                memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
        goto done;
    }

    if ((result = AudioUnitRender(stream->spatial_unit, flags, ts, 0, nframes,
            data)) != noErr)
        goto done;

    if (data->mNumberBuffers < stream->dev_desc.mChannelsPerFrame)
    {
        result = kAudio_ParamError;
        goto done;
    }
    for (i = 0; i < stream->dev_desc.mChannelsPerFrame; ++i)
        if (!data->mBuffers[i].mData || data->mBuffers[i].mNumberChannels != 1 ||
                data->mBuffers[i].mDataByteSize < nframes * sizeof(float))
        {
            result = kAudio_ParamError;
            goto done;
        }

    delay_spatial_dry_input(stream, nframes);
    mix_spatial_dry_planar(stream, nframes, data);
    commit_spatial_inputs(stream, consumed_read_frame, consumed_frames);

done:
    __atomic_store_n(&stream->spatial_callback_active, 0, __ATOMIC_RELEASE);
done_inflight:
    record_spatial_callback_time(stream, started);
    if (!__atomic_sub_fetch(&stream->spatial_callbacks_inflight, 1,
            __ATOMIC_RELEASE) && stream->callback_drain_semaphore_created &&
            (__atomic_load_n(&stream->shutting_down, __ATOMIC_ACQUIRE) ||
             __atomic_load_n(&stream->spatial_resetting, __ATOMIC_ACQUIRE)))
        semaphore_signal(stream->callback_drain_semaphore);
    return result;
}

static OSStatus ca_native_spatial_render_cb(void *user, AudioUnitRenderActionFlags *flags,
        const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes, AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    UINT64 consumed_read_frame = 0, started = 0;
    UINT32 consumed_frames = 0;
    UINT32 expected = 0;
    float *output;
    UINT32 channel, frame;
    UInt32 bytes;
    OSStatus result = noErr;

    (void)flags;
    (void)ts;
    (void)bus;

    __atomic_add_fetch(&stream->spatial_callbacks_inflight, 1,
            __ATOMIC_ACQUIRE);
    if (!__atomic_compare_exchange_n(&stream->spatial_callback_active,
            &expected, 1, FALSE, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        __atomic_add_fetch(&stream->reentrant_callbacks, 1, __ATOMIC_RELAXED);
        result = kAudioUnitErr_CannotDoInCurrentContext;
        goto done_inflight;
    }
    if (stream->profile_callbacks)
        started = mach_absolute_time();
    if (nframes > stream->spatial_dry_capacity ||
            __atomic_load_n(&stream->shutting_down, __ATOMIC_ACQUIRE))
    {
        result = kAudio_ParamError;
        goto done;
    }
    bytes = nframes * stream->spatial_bed_channels * sizeof(float);

    if (data->mNumberBuffers != 1 ||
            data->mBuffers[0].mNumberChannels != stream->spatial_bed_channels)
    {
        result = kAudio_ParamError;
        goto done;
    }
    if (!data->mBuffers[0].mData)
    {
        data->mBuffers[0].mData = stream->spatial_bed_buffer;
        data->mBuffers[0].mDataByteSize = bytes;
    }
    else if (data->mBuffers[0].mDataByteSize < bytes)
    {
        result = kAudio_ParamError;
        goto done;
    }

    output = data->mBuffers[0].mData;
    if ((result = ca_prepare_spatial_inputs(stream, nframes,
            &consumed_read_frame, &consumed_frames)) != noErr)
        goto done;
    if (output != stream->spatial_bed_buffer)
        memcpy(output, stream->spatial_bed_buffer, bytes);

    delay_spatial_dry_input(stream, nframes);
    for (channel = 0; channel < stream->spatial_dry_output_count; ++channel)
    {
        UINT32 index = stream->spatial_dry_output[channel];

        for (frame = 0; frame < nframes; ++frame)
            output[frame * stream->spatial_bed_channels + index] +=
                    stream->spatial_dry_buffer[frame];
    }
    commit_spatial_inputs(stream, consumed_read_frame, consumed_frames);
done:
    __atomic_store_n(&stream->spatial_callback_active, 0, __ATOMIC_RELEASE);
done_inflight:
    record_spatial_callback_time(stream, started);
    if (!__atomic_sub_fetch(&stream->spatial_callbacks_inflight, 1,
            __ATOMIC_RELEASE) && stream->callback_drain_semaphore_created &&
            (__atomic_load_n(&stream->shutting_down, __ATOMIC_ACQUIRE) ||
             __atomic_load_n(&stream->spatial_resetting, __ATOMIC_ACQUIRE)))
        semaphore_signal(stream->callback_drain_semaphore);
    return result;
}

static void ca_wrap_buffer(BYTE *dst, UINT32 dst_offs, UINT32 dst_bytes,
                           BYTE *src, UINT32 src_bytes)
{
    UINT32 chunk_bytes = dst_bytes - dst_offs;

    if(chunk_bytes < src_bytes){
        memcpy(dst + dst_offs, src, chunk_bytes);
        memcpy(dst, src + chunk_bytes, src_bytes - chunk_bytes);
    }else
        memcpy(dst + dst_offs, src, src_bytes);
}

/* we need to trigger CA to pull data from the device and give it to us
 *
 * raw data from CA is stored in cap_buffer, possibly via wrap_buffer
 *
 * raw data is resampled from cap_buffer into resamp_buffer in period-size
 * chunks and copied to local_buffer
 */
static OSStatus ca_capture_cb(void *user, AudioUnitRenderActionFlags *flags,
                              const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                              AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    AudioBufferList list;
    OSStatus sc;
    UINT32 cap_wri_offs_frames;

    os_unfair_lock_lock(&stream->lock);

    cap_wri_offs_frames = (stream->cap_offs_frames + stream->cap_held_frames) % stream->cap_bufsize_frames;

    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = stream->fmt->nChannels;
    list.mBuffers[0].mDataByteSize = nframes * stream->fmt->nBlockAlign;

    if(!stream->playing || cap_wri_offs_frames + nframes > stream->cap_bufsize_frames){
        if(stream->wrap_bufsize_frames < nframes){
            free(stream->wrap_buffer);
            stream->wrap_buffer = malloc(list.mBuffers[0].mDataByteSize);
            stream->wrap_bufsize_frames = nframes;
        }

        list.mBuffers[0].mData = stream->wrap_buffer;
    }else
        list.mBuffers[0].mData = stream->cap_buffer + cap_wri_offs_frames * stream->fmt->nBlockAlign;

    sc = AudioUnitRender(stream->unit, flags, ts, bus, nframes, &list);
    if(sc != noErr){
        os_unfair_lock_unlock(&stream->lock);
        return sc;
    }

    if(stream->playing){
        if(list.mBuffers[0].mData == stream->wrap_buffer){
            ca_wrap_buffer(stream->cap_buffer,
                    cap_wri_offs_frames * stream->fmt->nBlockAlign,
                    stream->cap_bufsize_frames * stream->fmt->nBlockAlign,
                    stream->wrap_buffer, list.mBuffers[0].mDataByteSize);
        }

        stream->cap_held_frames += list.mBuffers[0].mDataByteSize / stream->fmt->nBlockAlign;
        if(stream->cap_held_frames > stream->cap_bufsize_frames){
            stream->cap_offs_frames += stream->cap_held_frames % stream->cap_bufsize_frames;
            stream->cap_offs_frames %= stream->cap_bufsize_frames;
            stream->cap_held_frames = stream->cap_bufsize_frames;
        }
    }

    os_unfair_lock_unlock(&stream->lock);
    return noErr;
}

static AudioComponentInstance get_audiounit(EDataFlow dataflow, AudioDeviceID adevid, BOOL follows_default)
{
    AudioComponentInstance unit;
    AudioComponent comp;
    AudioComponentDescription desc;
    BOOL use_default_output = dataflow == eRender && follows_default;
    OSStatus sc;

    memset(&desc, 0, sizeof(desc));
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = use_default_output
            ? kAudioUnitSubType_DefaultOutput : kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    if(!(comp = AudioComponentFindNext(NULL, &desc))){
        WARN("AudioComponentFindNext failed\n");
        return NULL;
    }

    sc = AudioComponentInstanceNew(comp, &unit);
    if(sc != noErr){
        WARN("AudioComponentInstanceNew failed: %x\n", (int)sc);
        return NULL;
    }

    if(dataflow == eCapture){
        UInt32 enableio;

        enableio = 1;
        sc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                kAudioUnitScope_Input, 1, &enableio, sizeof(enableio));
        if(sc != noErr){
            WARN("Couldn't enable I/O on input element: %x\n", (int)sc);
            AudioComponentInstanceDispose(unit);
            return NULL;
        }

        enableio = 0;
        sc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                kAudioUnitScope_Output, 0, &enableio, sizeof(enableio));
        if(sc != noErr){
            WARN("Couldn't disable I/O on output element: %x\n", (int)sc);
            AudioComponentInstanceDispose(unit);
            return NULL;
        }
    }

    if(!use_default_output){
        sc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                kAudioUnitScope_Global, 0, &adevid, sizeof(adevid));
        if(sc != noErr){
            WARN("Couldn't set audio unit device\n");
            AudioComponentInstanceDispose(unit);
            return NULL;
        }
    }
    else
        TRACE("Following the system default output device from %u\n", (unsigned int)adevid);

    return unit;
}

static AudioComponentInstance get_spatial_mixer(void)
{
    AudioComponentDescription desc = {0};
    AudioComponentInstance unit;
    AudioComponent component;
    OSStatus sc;

    desc.componentType = kAudioUnitType_Mixer;
    desc.componentSubType = kAudioUnitSubType_SpatialMixer;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    if (!(component = AudioComponentFindNext(NULL, &desc)))
    {
        WARN("Spatial mixer AudioComponent is unavailable.\n");
        return NULL;
    }

    if ((sc = AudioComponentInstanceNew(component, &unit)) != noErr)
    {
        WARN("Failed to create spatial mixer: %x.\n", (int)sc);
        return NULL;
    }

    return unit;
}

static UINT32 get_device_spatial_output_type(AudioDeviceID dev_id)
{
    AudioObjectPropertyAddress addr =
    {
        .mSelector = kAudioDevicePropertyStreams,
        .mScope = kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMain,
    };
    AudioStreamID *streams = NULL;
    UInt32 transport, size, count, i;
    BOOL speaker = FALSE;
    OSStatus sc;

    sc = AudioObjectGetPropertyDataSize(dev_id, &addr, 0, NULL, &size);
    if (sc == noErr && (streams = malloc(size)))
    {
        sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, streams);
        count = sc == noErr ? size / sizeof(*streams) : 0;

        addr.mSelector = kAudioStreamPropertyTerminalType;
        addr.mScope = kAudioObjectPropertyScopeGlobal;
        for (i = 0; i < count; ++i)
        {
            UInt32 terminal;

            size = sizeof(terminal);
            if (AudioObjectGetPropertyData(streams[i], &addr, 0, NULL, &size, &terminal) != noErr)
                continue;
            if (terminal == kAudioStreamTerminalTypeHeadphones)
            {
                free(streams);
                return kSpatialMixerOutputType_Headphones;
            }
            if (terminal == kAudioStreamTerminalTypeSpeaker)
                speaker = TRUE;
        }
    }
    free(streams);

    addr.mSelector = kAudioDevicePropertyTransportType;
    addr.mScope = kAudioObjectPropertyScopeGlobal;
    size = sizeof(transport);
    if (AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, &transport) != noErr)
        transport = kAudioDeviceTransportTypeUnknown;

    if (transport == kAudioDeviceTransportTypeBluetooth ||
            transport == kAudioDeviceTransportTypeBluetoothLE)
        return kSpatialMixerOutputType_Headphones;
    if (speaker)
        return transport == kAudioDeviceTransportTypeBuiltIn
                ? kSpatialMixerOutputType_BuiltInSpeakers
                : kSpatialMixerOutputType_ExternalSpeakers;
    if (transport == kAudioDeviceTransportTypeBuiltIn)
        return kSpatialMixerOutputType_BuiltInSpeakers;
    return kSpatialMixerOutputType_ExternalSpeakers;
}

static AudioDeviceID get_stream_device(struct coreaudio_stream *stream)
{
    AudioDeviceID dev_id;
    UInt32 size;
    OSStatus sc;

    if(!stream->follows_default)
        return stream->dev_id;

    size = sizeof(dev_id);
    sc = AudioUnitGetProperty(stream->unit, kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global, 0, &dev_id, &size);
    if(sc != noErr){
        WARN("Couldn't get default audio unit device: %x\n", (int)sc);
        return stream->dev_id;
    }

    if(stream->dev_id != dev_id)
        TRACE("Default output device changed from %u to %u\n",
                (unsigned int)stream->dev_id, (unsigned int)dev_id);
    stream->dev_id = dev_id;
    return dev_id;
}

static void dump_adesc(const char *aux, AudioStreamBasicDescription *desc)
{
    TRACE("%s: mSampleRate: %f\n", aux, desc->mSampleRate);
    TRACE("%s: mBytesPerPacket: %u\n", aux, (unsigned int)desc->mBytesPerPacket);
    TRACE("%s: mFramesPerPacket: %u\n", aux, (unsigned int)desc->mFramesPerPacket);
    TRACE("%s: mBytesPerFrame: %u\n", aux, (unsigned int)desc->mBytesPerFrame);
    TRACE("%s: mChannelsPerFrame: %u\n", aux, (unsigned int)desc->mChannelsPerFrame);
    TRACE("%s: mBitsPerChannel: %u\n", aux, (unsigned int)desc->mBitsPerChannel);
}

static HRESULT ca_get_audiodesc(AudioStreamBasicDescription *desc,
                                const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    desc->mFormatFlags = 0;

    if(fmt->wFormatTag == WAVE_FORMAT_PCM ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))){
        desc->mFormatID = kAudioFormatLinearPCM;
        if(fmt->wBitsPerSample > 8)
            desc->mFormatFlags = kAudioFormatFlagIsSignedInteger;
    }else if(fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))){
        desc->mFormatID = kAudioFormatLinearPCM;
        desc->mFormatFlags = kAudioFormatFlagIsFloat;
    }else if(fmt->wFormatTag == WAVE_FORMAT_MULAW ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_MULAW))){
        desc->mFormatID = kAudioFormatULaw;
    }else if(fmt->wFormatTag == WAVE_FORMAT_ALAW ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_ALAW))){
        desc->mFormatID = kAudioFormatALaw;
    }else
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    desc->mSampleRate = fmt->nSamplesPerSec;
    desc->mBytesPerPacket = fmt->nBlockAlign;
    desc->mFramesPerPacket = 1;
    desc->mBytesPerFrame = fmt->nBlockAlign;
    desc->mChannelsPerFrame = fmt->nChannels;
    desc->mBitsPerChannel = fmt->wBitsPerSample;
    desc->mReserved = 0;

    return S_OK;
}

static HRESULT ca_setup_audiounit(EDataFlow dataflow, AudioComponentInstance unit,
                                  const WAVEFORMATEX *fmt, AudioStreamBasicDescription *dev_desc,
                                  AudioConverterRef *converter)
{
    OSStatus sc;
    HRESULT hr;

    if(dataflow == eCapture){
        AudioStreamBasicDescription desc;
        UInt32 size;
        Float64 rate;
        fenv_t fenv;
        BOOL fenv_stored = TRUE;

        hr = ca_get_audiodesc(&desc, fmt);
        if(FAILED(hr))
            return hr;
        dump_adesc("requested", &desc);

        /* input-only units can't perform sample rate conversion, so we have to
         * set up our own AudioConverter to support arbitrary sample rates. */
        size = sizeof(*dev_desc);
        sc = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 1, dev_desc, &size);
        if(sc != noErr){
            WARN("Couldn't get unit format: %x\n", (int)sc);
            return osstatus_to_hresult(sc);
        }
        dump_adesc("hardware", dev_desc);

        rate = dev_desc->mSampleRate;
        *dev_desc = desc;
        dev_desc->mSampleRate = rate;

        dump_adesc("final", dev_desc);
        sc = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output, 1, dev_desc, sizeof(*dev_desc));
        if(sc != noErr){
            WARN("Couldn't set unit format: %x\n", (int)sc);
            return osstatus_to_hresult(sc);
        }

        /* AudioConverterNew requires divide-by-zero SSE exceptions to be masked */
        if(feholdexcept(&fenv)){
            WARN("Failed to store fenv state\n");
            fenv_stored = FALSE;
        }

        sc = AudioConverterNew(dev_desc, &desc, converter);

        if(fenv_stored && fesetenv(&fenv))
            WARN("Failed to restore fenv state\n");

        if(sc != noErr){
            WARN("Couldn't create audio converter: %x\n", (int)sc);
            return osstatus_to_hresult(sc);
        }
    }else{
        AudioChannelLayout layout;

        hr = ca_get_audiodesc(dev_desc, fmt);
        if(FAILED(hr))
            return hr;

        dump_adesc("final", dev_desc);
        sc = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0, dev_desc, sizeof(*dev_desc));
        if(sc != noErr){
            WARN("Couldn't set format: %x\n", (int)sc);
            return osstatus_to_hresult(sc);
        }

        /* Set channel layout: AudioChannelBitmap and dwChannelMask conveniently have identical positions */
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
        layout.mChannelBitmap    = (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? ((WAVEFORMATEXTENSIBLE *)fmt)->dwChannelMask : 0x3;
        layout.mNumberChannelDescriptions = 0;

        sc = AudioUnitSetProperty(unit, kAudioUnitProperty_AudioChannelLayout,
                                  kAudioUnitScope_Input, 0, &layout, sizeof(layout));
        if (sc != noErr)
            WARN("Couldn't set channel layout: %d\n", (int)sc);
    }

    return S_OK;
}

static HRESULT get_device_mix_format(const char *device, WAVEFORMATEXTENSIBLE *format)
{
    AudioObjectPropertyAddress addr =
    {
        .mSelector = kAudioDevicePropertyPreferredChannelLayout,
        .mScope = kAudioDevicePropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMain,
    };
    AudioChannelLayout *expanded = NULL, *layout = NULL, *original_layout = NULL;
    const AudioDeviceID dev_id = dev_id_from_device(device);
    AudioChannelLayoutTag tag;
    DWORD mask = 0;
    UInt32 channels = 0, expanded_size, size;
    OSStatus sc;

    memset(format, 0, sizeof(*format));

    sc = AudioObjectGetPropertyDataSize(dev_id, &addr, 0, NULL, &size);
    if (sc != noErr || !(layout = malloc(size)))
        goto fallback;
    original_layout = layout;
    sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, layout);
    if (sc != noErr)
        goto fallback;

    if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap)
    {
        mask = layout->mChannelBitmap;
        channels = count_channel_mask_bits(mask);
    }
    else
    {
        if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions)
        {
            tag = layout->mChannelLayoutTag;
            sc = AudioFormatGetPropertyInfo(kAudioFormatProperty_ChannelLayoutForTag,
                    sizeof(tag), &tag, &expanded_size);
            if (sc != noErr || !(expanded = malloc(expanded_size)))
                goto fallback;
            sc = AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag,
                    sizeof(tag), &tag, &expanded_size, expanded);
            if (sc != noErr ||
                    expanded->mChannelLayoutTag !=
                            kAudioChannelLayoutTag_UseChannelDescriptions)
                goto fallback;
            layout = expanded;
        }

        channels = layout->mNumberChannelDescriptions;
        mask = ca_channel_layout_to_channel_mask(layout);
    }

    if (!channels || count_channel_mask_bits(mask) != channels)
        goto fallback;

    format->Format.nChannels = channels;
    format->dwChannelMask = mask;
    free(original_layout);
    free(expanded);
    return S_OK;

fallback:
    free(original_layout);
    free(expanded);

    /* Stereo/mono endpoints occasionally omit a preferred layout. Their
     * fallback masks are unambiguous; fail closed for larger devices rather
     * than claiming a guessed speaker topology is physically lossless. */
    {
        struct get_mix_format_params params =
        {
            .device = device,
            .flow = eRender,
            .fmt = format,
        };

        memset(format, 0, sizeof(*format));
        unix_get_mix_format(&params);
        if (FAILED(params.result))
            return params.result;
        if (format->Format.nChannels > 2 ||
                count_channel_mask_bits(format->dwChannelMask) !=
                        format->Format.nChannels)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        return S_OK;
    }
}

static UINT32 count_channel_mask_bits(DWORD mask)
{
    UINT32 count = 0;

    while (mask)
    {
        mask &= mask - 1;
        ++count;
    }
    return count;
}

struct spatial_static_channel
{
    AudioObjectType type;
    DWORD speaker;
};

static const struct spatial_static_channel spatial_static_channels[] =
{
    {AudioObjectType_FrontLeft,        SPEAKER_FRONT_LEFT},
    {AudioObjectType_FrontRight,       SPEAKER_FRONT_RIGHT},
    {AudioObjectType_FrontCenter,      SPEAKER_FRONT_CENTER},
    {AudioObjectType_LowFrequency,     SPEAKER_LOW_FREQUENCY},
    {AudioObjectType_SideLeft,         SPEAKER_SIDE_LEFT},
    {AudioObjectType_SideRight,        SPEAKER_SIDE_RIGHT},
    {AudioObjectType_BackLeft,         SPEAKER_BACK_LEFT},
    {AudioObjectType_BackRight,        SPEAKER_BACK_RIGHT},
    {AudioObjectType_TopFrontLeft,     SPEAKER_TOP_FRONT_LEFT},
    {AudioObjectType_TopFrontRight,    SPEAKER_TOP_FRONT_RIGHT},
    {AudioObjectType_TopBackLeft,      SPEAKER_TOP_BACK_LEFT},
    {AudioObjectType_TopBackRight,     SPEAKER_TOP_BACK_RIGHT},
    {AudioObjectType_BottomFrontLeft,  SPATIAL_AUDIO_BOTTOM_FRONT_LEFT_SPEAKER},
    {AudioObjectType_BottomFrontRight, SPATIAL_AUDIO_BOTTOM_FRONT_RIGHT_SPEAKER},
    {AudioObjectType_BottomBackLeft,   SPATIAL_AUDIO_BOTTOM_BACK_LEFT_SPEAKER},
    {AudioObjectType_BottomBackRight,  SPATIAL_AUDIO_BOTTOM_BACK_RIGHT_SPEAKER},
    {AudioObjectType_BackCenter,       SPEAKER_BACK_CENTER},
};

static DWORD spatial_static_mask_to_transport_mask(UINT32 static_mask)
{
    DWORD mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
            SPATIAL_AUDIO_DRY_SPEAKER;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(spatial_static_channels); ++i)
        if (static_mask & spatial_static_channels[i].type)
            mask |= spatial_static_channels[i].speaker;
    return mask;
}

static HRESULT get_spatial_bed_format(const WAVEFORMATEX *transport,
        UINT32 static_mask, UINT32 dynamic_objects, WAVEFORMATEXTENSIBLE *bed,
        UINT32 *dry_channel, UINT32 *dynamic_channel)
{
    const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)transport;

    if (transport->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
            !IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ||
            transport->wBitsPerSample != 32 ||
            static_mask & ~SPATIAL_AUDIO_STATIC_OBJECT_MASK ||
            ext->dwChannelMask != spatial_static_mask_to_transport_mask(static_mask) ||
            dynamic_objects > UINT16_MAX ||
            count_channel_mask_bits(ext->dwChannelMask) + dynamic_objects !=
                    transport->nChannels)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    *bed = *ext;
    bed->dwChannelMask &= ~SPATIAL_AUDIO_DRY_SPEAKER;
    bed->Format.nChannels -= dynamic_objects + 1;
    bed->Format.nBlockAlign = bed->Format.nChannels * sizeof(float);
    bed->Format.nAvgBytesPerSec = bed->Format.nSamplesPerSec *
            bed->Format.nBlockAlign;

    if (!bed->Format.nChannels ||
            count_channel_mask_bits(bed->dwChannelMask) != bed->Format.nChannels ||
            (bed->dwChannelMask & (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)) !=
                    (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    *dry_channel = count_channel_mask_bits(ext->dwChannelMask &
            (SPATIAL_AUDIO_DRY_SPEAKER - 1));
    *dynamic_channel = count_channel_mask_bits(ext->dwChannelMask);
    return S_OK;
}

static HRESULT configure_spatial_dry_output(struct coreaudio_stream *stream, DWORD mask,
        UINT32 channels)
{
    if (count_channel_mask_bits(mask) != channels)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    stream->spatial_dry_output_count = 0;
    if ((mask & (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)) ==
            (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT))
    {
        stream->spatial_dry_output[stream->spatial_dry_output_count++] =
                count_channel_mask_bits(mask & (SPEAKER_FRONT_LEFT - 1));
        stream->spatial_dry_output[stream->spatial_dry_output_count++] =
                count_channel_mask_bits(mask & (SPEAKER_FRONT_RIGHT - 1));
    }
    else if (mask & SPEAKER_FRONT_CENTER)
    {
        stream->spatial_dry_output[stream->spatial_dry_output_count++] =
                count_channel_mask_bits(mask & (SPEAKER_FRONT_CENTER - 1));
    }
    else
    {
        WARN("Output channel layout has no front pair or center for dry spatial audio.\n");
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    return S_OK;
}

static HRESULT configure_spatial_max_frames(struct coreaudio_stream *stream)
{
    UINT32 metadata_capacity, parameter_events;
    UInt32 max_frames, size = sizeof(max_frames);
    UINT64 count;
    OSStatus sc;

    sc = AudioUnitGetProperty(stream->unit, kAudioUnitProperty_MaximumFramesPerSlice,
            kAudioUnitScope_Global, 0, &max_frames, &size);
    if (sc != noErr || !max_frames)
    {
        WARN("Failed to query output maximum frames per slice: %x.\n", (int)sc);
        return sc == noErr ? E_FAIL : osstatus_to_hresult(sc);
    }
    if (max_frames < stream->period_frames)
        max_frames = stream->period_frames;

    sc = AudioUnitSetProperty(stream->unit, kAudioUnitProperty_MaximumFramesPerSlice,
            kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));
    if (sc != noErr)
    {
        WARN("Failed to configure output maximum frames per slice: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }
    if (stream->spatial_unit)
    {
        sc = AudioUnitSetProperty(stream->spatial_unit,
                kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                0, &max_frames, sizeof(max_frames));
        if (sc != noErr)
        {
            WARN("Failed to configure spatial mixer maximum frames per slice: %x.\n", (int)sc);
            return osstatus_to_hresult(sc);
        }
    }

    if (max_frames > UINT32_MAX / sizeof(float) ||
            stream->spatial_bed_channels >
                    UINT32_MAX / sizeof(float) / max_frames)
    {
        WARN("Spatial render slice is too large (%u frames, %u channels).\n",
                max_frames, stream->spatial_bed_channels);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    if (!(stream->spatial_dry_buffer = malloc(max_frames *
            sizeof(*stream->spatial_dry_buffer))))
        return E_OUTOFMEMORY;
    if (!(stream->spatial_bed_buffer = malloc((size_t)max_frames *
            stream->spatial_bed_channels * sizeof(*stream->spatial_bed_buffer))))
    {
        free(stream->spatial_dry_buffer);
        stream->spatial_dry_buffer = NULL;
        return E_OUTOFMEMORY;
    }
    stream->spatial_dry_capacity = max_frames;

    if (!stream->spatial_dynamic_objects)
        return S_OK;

    count = (UINT64)max_frames * stream->spatial_dynamic_objects;
    if (count > SIZE_MAX / sizeof(*stream->spatial_dynamic_buffer) ||
            !(stream->spatial_dynamic_buffer = calloc(count,
                    sizeof(*stream->spatial_dynamic_buffer))))
        return E_OUTOFMEMORY;

    count = ((UINT64)stream->bufsize_frames + stream->period_frames - 1) /
            stream->period_frames + 2;
    if (count > UINT32_MAX)
        return E_OUTOFMEMORY;
    metadata_capacity = count;
    if (metadata_capacity < 3)
        metadata_capacity = 3;
    count = (UINT64)metadata_capacity * stream->spatial_dynamic_objects;
    if (count > SIZE_MAX / sizeof(*stream->spatial_object_states) ||
            !(stream->spatial_object_states = calloc(count,
                    sizeof(*stream->spatial_object_states))))
        return E_OUTOFMEMORY;
    if (!(stream->spatial_metadata_sequences = malloc(
            metadata_capacity * sizeof(*stream->spatial_metadata_sequences))))
        return E_OUTOFMEMORY;
    for (size = 0; size < metadata_capacity; ++size)
        stream->spatial_metadata_sequences[size] = UINT64_MAX;
    stream->spatial_metadata_capacity = metadata_capacity;

    count = (UINT64)(max_frames / stream->period_frames + 2) *
            stream->spatial_dynamic_objects * 3;
    if (count > UINT32_MAX ||
            count > SIZE_MAX / sizeof(*stream->spatial_parameter_events) ||
            !(parameter_events = count) ||
            !(stream->spatial_parameter_events = malloc((size_t)parameter_events *
                    sizeof(*stream->spatial_parameter_events))))
        return E_OUTOFMEMORY;
    stream->spatial_parameter_event_capacity = parameter_events;
    return S_OK;
}

static HRESULT configure_spatial_dry_delay(struct coreaudio_stream *stream)
{
    Float64 latency, frame_latency;
    UInt32 size = sizeof(latency), frames;
    OSStatus sc;

    if (!stream->spatial_unit)
        return S_OK;

    sc = AudioUnitGetProperty(stream->spatial_unit, kAudioUnitProperty_Latency,
            kAudioUnitScope_Global, 0, &latency, &size);
    if (sc != noErr)
    {
        WARN("Failed to query spatial mixer latency: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    frame_latency = latency * stream->fmt->nSamplesPerSec;
    if (!(latency >= 0.0) || !(frame_latency >= 0.0) ||
            frame_latency > UINT32_MAX)
    {
        WARN("Spatial mixer returned an invalid latency %f.\n", latency);
        return E_FAIL;
    }

    frames = (UINT32)frame_latency;
    if (frame_latency - frames >= 0.5 && frames != UINT32_MAX)
        ++frames;
    if (!frames)
        return S_OK;

    if (!(stream->spatial_dry_delay_buffer = calloc(frames,
            sizeof(*stream->spatial_dry_delay_buffer))))
        return E_OUTOFMEMORY;

    stream->spatial_dry_delay_frames = frames;
    TRACE("Delaying non-spatial objects by %u frames (%f seconds) to match the spatial mixer.\n",
            frames, latency);
    return S_OK;
}

static BOOL device_supports_spatial_format(const char *device, const WAVEFORMATEX *format)
{
    const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)format;
    WAVEFORMATEXTENSIBLE mix;

    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || !ext->dwChannelMask)
        return FALSE;
    if (FAILED(get_device_mix_format(device, &mix)))
        return FALSE;

    return mix.Format.nChannels >= format->nChannels &&
            !(ext->dwChannelMask & ~mix.dwChannelMask);
}

struct wave_channel_layout
{
    AudioChannelLayout layout;
    AudioChannelDescription extra_descriptions[17];
};

static AudioChannelLabel wave_speaker_to_channel_label(DWORD speaker)
{
    switch (speaker)
    {
    case SPEAKER_FRONT_LEFT: return kAudioChannelLabel_Left;
    case SPEAKER_FRONT_RIGHT: return kAudioChannelLabel_Right;
    case SPEAKER_FRONT_CENTER: return kAudioChannelLabel_Center;
    case SPEAKER_LOW_FREQUENCY: return kAudioChannelLabel_LFEScreen;
    case SPEAKER_BACK_LEFT: return kAudioChannelLabel_LeftBackSurround;
    case SPEAKER_BACK_RIGHT: return kAudioChannelLabel_RightBackSurround;
    case SPEAKER_FRONT_LEFT_OF_CENTER: return kAudioChannelLabel_LeftCenter;
    case SPEAKER_FRONT_RIGHT_OF_CENTER: return kAudioChannelLabel_RightCenter;
    case SPEAKER_BACK_CENTER: return kAudioChannelLabel_CenterSurround;
    case SPEAKER_SIDE_LEFT: return kAudioChannelLabel_LeftSideSurround;
    case SPEAKER_SIDE_RIGHT: return kAudioChannelLabel_RightSideSurround;
    case SPEAKER_TOP_CENTER: return kAudioChannelLabel_CenterTopMiddle;
    case SPEAKER_TOP_FRONT_LEFT: return kAudioChannelLabel_LeftTopFront;
    case SPEAKER_TOP_FRONT_CENTER: return kAudioChannelLabel_CenterTopFront;
    case SPEAKER_TOP_FRONT_RIGHT: return kAudioChannelLabel_RightTopFront;
    case SPEAKER_TOP_BACK_LEFT: return kAudioChannelLabel_LeftTopRear;
    case SPEAKER_TOP_BACK_CENTER: return kAudioChannelLabel_CenterTopRear;
    case SPEAKER_TOP_BACK_RIGHT: return kAudioChannelLabel_RightTopRear;
    default: return kAudioChannelLabel_Unknown;
    }
}

static HRESULT wave_mask_to_channel_layout(DWORD mask, UINT32 channels,
        struct wave_channel_layout *layout, UInt32 *size)
{
    AudioChannelLabel label;
    DWORD speaker;
    UINT32 index = 0;

    memset(layout, 0, sizeof(*layout));
    layout->layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;

    for (speaker = 1; speaker && speaker <= SPEAKER_TOP_BACK_RIGHT; speaker <<= 1)
    {
        if (!(mask & speaker))
            continue;
        if ((label = wave_speaker_to_channel_label(speaker)) == kAudioChannelLabel_Unknown)
            return E_INVALIDARG;

        layout->layout.mChannelDescriptions[index++].mChannelLabel = label;
    }

    if (index != channels || mask & ~(SPEAKER_TOP_BACK_RIGHT * 2 - 1))
        return E_INVALIDARG;

    layout->layout.mNumberChannelDescriptions = channels;
    *size = offsetof(AudioChannelLayout, mChannelDescriptions) +
            channels * sizeof(AudioChannelDescription);
    return S_OK;
}

static BOOL spatial_transport_speaker_to_coordinates(DWORD speaker, float *x,
        float *back_front, float *down_up)
{
    switch (speaker)
    {
    case SPATIAL_AUDIO_BOTTOM_FRONT_LEFT_SPEAKER:
        *x = -0.3535534f;
        *back_front = 0.6123724f;
        *down_up = -0.7071068f;
        return TRUE;
    case SPATIAL_AUDIO_BOTTOM_FRONT_RIGHT_SPEAKER:
        *x = 0.3535534f;
        *back_front = 0.6123724f;
        *down_up = -0.7071068f;
        return TRUE;
    case SPATIAL_AUDIO_BOTTOM_BACK_LEFT_SPEAKER:
        *x = -0.3535534f;
        *back_front = -0.6123724f;
        *down_up = -0.7071068f;
        return TRUE;
    case SPATIAL_AUDIO_BOTTOM_BACK_RIGHT_SPEAKER:
        *x = 0.3535534f;
        *back_front = -0.6123724f;
        *down_up = -0.7071068f;
        return TRUE;
    default:
        return FALSE;
    }
}

static HRESULT spatial_mask_to_channel_layout(DWORD mask, UINT32 channels,
        struct wave_channel_layout *layout, UInt32 *size)
{
    AudioChannelDescription *description;
    AudioChannelLabel label;
    DWORD speaker;
    UINT32 index = 0;

    memset(layout, 0, sizeof(*layout));
    layout->layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;

    for (speaker = 1; speaker && speaker <= SPEAKER_TOP_BACK_RIGHT; speaker <<= 1)
    {
        if (!(mask & speaker))
            continue;

        description = &layout->layout.mChannelDescriptions[index++];
        if (spatial_transport_speaker_to_coordinates(speaker,
                &description->mCoordinates[kAudioChannelCoordinates_LeftRight],
                &description->mCoordinates[kAudioChannelCoordinates_BackFront],
                &description->mCoordinates[kAudioChannelCoordinates_DownUp]))
        {
            description->mChannelLabel = kAudioChannelLabel_UseCoordinates;
            description->mChannelFlags = kAudioChannelFlags_RectangularCoordinates;
        }
        else
        {
            label = wave_speaker_to_channel_label(speaker);
            if (label == kAudioChannelLabel_Unknown)
                return E_INVALIDARG;
            description->mChannelLabel = label;
        }
    }

    if (index != channels || mask & ~(SPEAKER_TOP_BACK_RIGHT * 2 - 1) ||
            mask & SPATIAL_AUDIO_DRY_SPEAKER)
        return E_INVALIDARG;

    layout->layout.mNumberChannelDescriptions = channels;
    *size = offsetof(AudioChannelLayout, mChannelDescriptions) +
            channels * sizeof(AudioChannelDescription);
    return S_OK;
}

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 120300
static HRESULT disable_spatial_head_tracking(AudioComponentInstance unit)
{
    UInt32 value, size = sizeof(value);
    Boolean writable;
    OSStatus sc;

    sc = AudioUnitGetPropertyInfo(unit,
            kAudioUnitProperty_SpatialMixerEnableHeadTracking,
            kAudioUnitScope_Global, 0, &size, &writable);
    if (sc != noErr || size != sizeof(value))
    {
        WARN("Failed to inspect spatial mixer head tracking: %x.\n", (int)sc);
        return sc == noErr ? E_FAIL : osstatus_to_hresult(sc);
    }

    sc = AudioUnitGetProperty(unit,
            kAudioUnitProperty_SpatialMixerEnableHeadTracking,
            kAudioUnitScope_Global, 0, &value, &size);
    if (sc != noErr)
    {
        WARN("Failed to query spatial mixer head tracking: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }
    if (!value)
        return S_OK;
    if (!writable)
    {
        WARN("Spatial mixer head tracking is enabled but read-only.\n");
        return E_FAIL;
    }

    value = 0;
    sc = AudioUnitSetProperty(unit,
            kAudioUnitProperty_SpatialMixerEnableHeadTracking,
            kAudioUnitScope_Global, 0, &value, sizeof(value));
    if (sc != noErr)
    {
        WARN("Failed to disable spatial mixer head tracking: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }
    return S_OK;
}
#endif

static HRESULT ca_setup_spatial_audiounit(struct coreaudio_stream *stream, const char *device,
        const WAVEFORMATEXTENSIBLE *bed)
{
    AudioStreamBasicDescription input_desc, dynamic_desc, output_desc;
    struct wave_channel_layout input_layout, output_layout;
    AURenderCallbackStruct callback;
    WAVEFORMATEXTENSIBLE mix;
    UInt32 algorithm, bus, input_layout_size, output_layout_size;
    UInt32 output_type, rendering_flags, source_mode, value;
    OSStatus sc;
    HRESULT hr;

    if (FAILED(hr = get_device_mix_format(device, &mix)))
        return hr;
    if (!mix.Format.nChannels || !mix.dwChannelMask)
    {
        WARN("Cannot spatialize to an output device without a known channel layout.\n");
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    if (!(stream->spatial_unit = get_spatial_mixer()))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (FAILED(hr = ca_get_audiodesc(&input_desc, &bed->Format)))
        return hr;
    input_desc.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;

    output_desc.mSampleRate = input_desc.mSampleRate;
    output_desc.mFormatID = kAudioFormatLinearPCM;
    output_desc.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    output_desc.mBytesPerPacket = sizeof(float);
    output_desc.mFramesPerPacket = 1;
    output_desc.mBytesPerFrame = sizeof(float);
    output_desc.mChannelsPerFrame = mix.Format.nChannels;
    output_desc.mBitsPerChannel = sizeof(float) * 8;
    output_desc.mReserved = 0;

    if (FAILED(hr = spatial_mask_to_channel_layout(bed->dwChannelMask,
            bed->Format.nChannels, &input_layout, &input_layout_size)) ||
            FAILED(hr = wave_mask_to_channel_layout(mix.dwChannelMask,
            mix.Format.nChannels, &output_layout, &output_layout_size)))
    {
        WARN("Cannot translate a WAVE channel mask for the spatial mixer.\n");
        return hr;
    }

    if (stream->spatial_dynamic_objects == UINT32_MAX)
        return E_INVALIDARG;
    value = stream->spatial_dynamic_objects + 1;
    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_ElementCount,
            kAudioUnitScope_Input, 0, &value, sizeof(value));
    if (sc != noErr)
    {
        WARN("Failed to configure spatial mixer input count: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input, 0, &input_desc, sizeof(input_desc));
    if (sc != noErr)
    {
        WARN("Failed to set spatial mixer input format: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }
    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_AudioChannelLayout,
            kAudioUnitScope_Input, 0, &input_layout.layout, input_layout_size);
    if (sc != noErr)
    {
        WARN("Failed to set spatial mixer input layout: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Output, 0, &output_desc, sizeof(output_desc));
    if (sc != noErr)
    {
        WARN("Failed to set spatial mixer output format: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }
    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_AudioChannelLayout,
            kAudioUnitScope_Output, 0, &output_layout.layout, output_layout_size);
    if (sc != noErr)
    {
        WARN("Failed to set spatial mixer output layout: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    output_type = get_device_spatial_output_type(stream->dev_id);
    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_SpatialMixerOutputType,
            kAudioUnitScope_Global, 0, &output_type, sizeof(output_type));
    if (sc != noErr)
    {
        WARN("Failed to select spatial mixer output type: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    algorithm = kSpatializationAlgorithm_UseOutputType;
    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_SpatializationAlgorithm,
            kAudioUnitScope_Input, 0, &algorithm, sizeof(algorithm));
    if (sc != noErr)
    {
        algorithm = output_type == kSpatialMixerOutputType_Headphones
                ? kSpatializationAlgorithm_HRTFHQ : kSpatializationAlgorithm_VectorBasedPanning;
        sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_SpatializationAlgorithm,
                kAudioUnitScope_Input, 0, &algorithm, sizeof(algorithm));
    }
    if (sc != noErr)
    {
        WARN("Failed to select a spatialization algorithm: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    source_mode = kSpatialMixerSourceMode_AmbienceBed;
    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_SpatialMixerSourceMode,
            kAudioUnitScope_Input, 0, &source_mode, sizeof(source_mode));
    if (sc != noErr)
    {
        WARN("Failed to configure the spatial ambience bed: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    rendering_flags = 0;
    sc = AudioUnitSetProperty(stream->spatial_unit,
            kAudioUnitProperty_SpatialMixerRenderingFlags, kAudioUnitScope_Input,
            0, &rendering_flags, sizeof(rendering_flags));
    if (sc != noErr)
    {
        WARN("Failed to disable spatial distance processing: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    value = 0;
    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_UsesInternalReverb,
            kAudioUnitScope_Global, 0, &value, sizeof(value));
    if (sc != noErr)
    {
        WARN("Failed to disable spatial mixer reverb: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 120300
    if (output_type == kSpatialMixerOutputType_Headphones)
    {
        if (__builtin_available(macOS 12.3, *))
        {
            if (FAILED(hr = disable_spatial_head_tracking(stream->spatial_unit)))
                return hr;
        }
    }
#endif
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 130000
    if (output_type == kSpatialMixerOutputType_Headphones)
    {
        if (__builtin_available(macOS 13.0, *))
        {
            value = kSpatialMixerPersonalizedHRTFMode_Auto;
            sc = AudioUnitSetProperty(stream->spatial_unit,
                    kAudioUnitProperty_SpatialMixerPersonalizedHRTFMode,
                    kAudioUnitScope_Global, 0, &value, sizeof(value));
            if (sc != noErr)
                WARN("Failed to select automatic personalized HRTF: %x.\n", (int)sc);
        }
    }
#endif

    callback.inputProc = ca_spatial_bed_render_cb;
    callback.inputProcRefCon = stream;
    sc = AudioUnitSetProperty(stream->spatial_unit, kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (sc != noErr)
    {
        WARN("Failed to set spatial mixer input callback: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    if (stream->spatial_dynamic_objects)
    {
        UInt32 point_source_flags =
                kSpatialMixerRenderingFlags_InterAuralDelay |
                kSpatialMixerRenderingFlags_DistanceAttenuation;

        if (!(stream->spatial_input_contexts = calloc(
                stream->spatial_dynamic_objects,
                sizeof(*stream->spatial_input_contexts))))
            return E_OUTOFMEMORY;

        dynamic_desc = input_desc;
        dynamic_desc.mBytesPerPacket = sizeof(float);
        dynamic_desc.mBytesPerFrame = sizeof(float);
        dynamic_desc.mChannelsPerFrame = 1;
        for (bus = 1; bus <= stream->spatial_dynamic_objects; ++bus)
        {
            struct spatial_input_context *context =
                    &stream->spatial_input_contexts[bus - 1];

            sc = AudioUnitSetProperty(stream->spatial_unit,
                    kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                    bus, &dynamic_desc, sizeof(dynamic_desc));
            if (sc != noErr)
            {
                WARN("Failed to configure PointSource %u format: %x.\n",
                        bus - 1, (int)sc);
                return osstatus_to_hresult(sc);
            }

            source_mode = kSpatialMixerSourceMode_PointSource;
            sc = AudioUnitSetProperty(stream->spatial_unit,
                    kAudioUnitProperty_SpatialMixerSourceMode,
                    kAudioUnitScope_Input, bus, &source_mode,
                    sizeof(source_mode));
            if (sc == noErr)
                sc = AudioUnitSetProperty(stream->spatial_unit,
                        kAudioUnitProperty_SpatializationAlgorithm,
                        kAudioUnitScope_Input, bus, &algorithm,
                        sizeof(algorithm));
            if (sc == noErr)
                sc = AudioUnitSetProperty(stream->spatial_unit,
                        kAudioUnitProperty_SpatialMixerRenderingFlags,
                        kAudioUnitScope_Input, bus, &point_source_flags,
                        sizeof(point_source_flags));
            if (sc != noErr)
            {
                WARN("Failed to configure PointSource %u spatial mode: %x.\n",
                        bus - 1, (int)sc);
                return osstatus_to_hresult(sc);
            }

            context->stream = stream;
            context->slot = bus - 1;
            callback.inputProc = ca_spatial_dynamic_render_cb;
            callback.inputProcRefCon = context;
            sc = AudioUnitSetProperty(stream->spatial_unit,
                    kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                    bus, &callback, sizeof(callback));
            if (sc != noErr)
            {
                WARN("Failed to set PointSource %u callback: %x.\n",
                        bus - 1, (int)sc);
                return osstatus_to_hresult(sc);
            }

            sc = AudioUnitSetParameter(stream->spatial_unit,
                    kSpatialMixerParam_Azimuth, kAudioUnitScope_Input,
                    bus, 0.0f, 0);
            if (sc == noErr)
                sc = AudioUnitSetParameter(stream->spatial_unit,
                        kSpatialMixerParam_Elevation, kAudioUnitScope_Input,
                        bus, 0.0f, 0);
            if (sc == noErr)
                sc = AudioUnitSetParameter(stream->spatial_unit,
                        kSpatialMixerParam_Distance, kAudioUnitScope_Input,
                        bus, 0.0f, 0);
            if (sc != noErr)
            {
                WARN("Failed to initialize PointSource %u position: %x.\n",
                        bus - 1, (int)sc);
                return osstatus_to_hresult(sc);
            }
        }
    }

    sc = AudioUnitSetProperty(stream->unit, kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input, 0, &output_desc, sizeof(output_desc));
    if (sc != noErr)
    {
        WARN("Failed to set spatial output format: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }
    sc = AudioUnitSetProperty(stream->unit, kAudioUnitProperty_AudioChannelLayout,
            kAudioUnitScope_Input, 0, &output_layout.layout, output_layout_size);
    if (sc != noErr)
    {
        WARN("Failed to set spatial output layout: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    callback.inputProc = ca_spatial_render_cb;
    callback.inputProcRefCon = stream;
    sc = AudioUnitSetProperty(stream->unit, kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (sc != noErr)
    {
        WARN("Failed to set spatial output callback: %x.\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    if (FAILED(hr = configure_spatial_dry_output(stream, mix.dwChannelMask,
            mix.Format.nChannels)) ||
            FAILED(hr = configure_spatial_max_frames(stream)))
        return hr;

    stream->dev_desc = output_desc;
    TRACE("Using the spatial mixer for a %u-channel bed, %u PointSources, and %u output channels (type %u, algorithm %u).\n",
            bed->Format.nChannels, stream->spatial_dynamic_objects,
            mix.Format.nChannels, output_type, algorithm);
    return S_OK;
}

static AudioDeviceID dev_id_from_device(const char *device)
{
    AudioDeviceID id;
    CFStringRef uid;
    UInt32 size;
    OSStatus sc;
    const AudioObjectPropertyAddress addr =
    {
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
        .mSelector = kAudioHardwarePropertyTranslateUIDToDevice,
    };

    uid = CFStringCreateWithCStringNoCopy(NULL, device, kCFStringEncodingUTF8, kCFAllocatorNull);

    size = sizeof(id);
    sc = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(uid), &uid, &size, &id);
    CFRelease(uid);
    if(sc != noErr){
        WARN("Failed to get device ID for UID %s: %x\n", device, (int)sc);
        return kAudioObjectUnknown;
    }

    if (id == kAudioObjectUnknown)
        WARN("Failed to get device ID for UID %s\n", device);

    return id;
}

static UINT32 spatial_generation_hash(UINT32 hash, const void *data, size_t size)
{
    const BYTE *bytes = data;

    while (size--)
    {
        hash ^= *bytes++;
        hash *= 16777619u;
    }
    return hash;
}

static HRESULT get_device_timing(AudioDeviceID dev_id, UINT32 *period_frames,
        UINT32 *minimum_frames, UINT32 *sample_rate)
{
    AudioObjectPropertyAddress addr =
    {
        .mSelector = kAudioDevicePropertyDeviceIsAlive,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };
    AudioValueRange range;
    Float64 rate;
    UInt32 alive, frames, size;
    OSStatus sc;

    size = sizeof(alive);
    sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, &alive);
    if (sc != noErr || !alive)
        return AUDCLNT_E_DEVICE_INVALIDATED;

    addr.mSelector = kAudioDevicePropertyBufferFrameSize;
    size = sizeof(frames);
    if ((sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size,
            &frames)) != noErr || !frames)
        return sc == noErr ? E_FAIL : osstatus_to_hresult(sc);

    addr.mSelector = kAudioDevicePropertyBufferFrameSizeRange;
    size = sizeof(range);
    if ((sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size,
            &range)) != noErr || !isfinite(range.mMinimum) ||
            range.mMinimum < 1.0 || range.mMinimum > UINT32_MAX)
        return sc == noErr ? E_FAIL : osstatus_to_hresult(sc);

    addr.mSelector = kAudioDevicePropertyNominalSampleRate;
    size = sizeof(rate);
    if ((sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size,
            &rate)) != noErr || !isfinite(rate) || rate < 1.0 ||
            rate > UINT32_MAX)
        return sc == noErr ? E_FAIL : osstatus_to_hresult(sc);

    *period_frames = frames;
    *minimum_frames = (UINT32)ceil(range.mMinimum);
    *sample_rate = (UINT32)llround(rate);
    return S_OK;
}

static void release_spatial_probe(struct coreaudio_stream *stream)
{
    if (stream->unit_initialized)
        AudioUnitUninitialize(stream->unit);
    if (stream->spatial_unit_initialized)
        AudioUnitUninitialize(stream->spatial_unit);
    if (stream->spatial_unit)
        AudioComponentInstanceDispose(stream->spatial_unit);
    if (stream->unit)
        AudioComponentInstanceDispose(stream->unit);
    free(stream->spatial_bed_buffer);
    free(stream->spatial_dry_buffer);
    free(stream->spatial_dynamic_buffer);
    free(stream->spatial_input_contexts);
    free(stream->spatial_parameter_events);
    free(stream->spatial_object_states);
    free(stream->spatial_metadata_sequences);
}

static HRESULT validate_spatial_dynamic_capacity(const char *device,
        AudioDeviceID dev_id, UINT32 endpoint_period_frames,
        UINT32 endpoint_sample_rate, UINT32 dynamic_objects)
{
    struct coreaudio_stream probe = {0};
    WAVEFORMATEXTENSIBLE bed = {0};
    UINT64 period_time, object_period_frames;
    HRESULT hr;
    OSStatus sc;

    /* Windows exposes one 48 kHz object format even when the endpoint runs at
     * another nominal rate.  Probe the graph in that actual client format,
     * leaving the output AudioUnit to perform its normal rate conversion. */
    period_time = ((UINT64)endpoint_period_frames * 10000000 +
            endpoint_sample_rate / 2) / endpoint_sample_rate;
    object_period_frames = (period_time * 48000 + 5000000) / 10000000;
    if (!object_period_frames || object_period_frames > UINT32_MAX / 3)
        return E_INVALIDARG;

    bed.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    bed.Format.nChannels = 2;
    bed.Format.nSamplesPerSec = 48000;
    bed.Format.wBitsPerSample = 32;
    bed.Format.nBlockAlign = 2 * sizeof(float);
    bed.Format.nAvgBytesPerSec = bed.Format.nSamplesPerSec *
            bed.Format.nBlockAlign;
    bed.Format.cbSize = sizeof(bed) - sizeof(bed.Format);
    bed.Samples.wValidBitsPerSample = 32;
    bed.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    bed.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    probe.dev_id = dev_id;
    probe.spatial = TRUE;
    probe.spatial_dynamic_objects = dynamic_objects;
    probe.spatial_bed_channels = bed.Format.nChannels;
    probe.spatial_dry_channel = 2;
    probe.spatial_dynamic_channel = 3;
    probe.period_frames = object_period_frames;
    probe.bufsize_frames = probe.period_frames * 3;
    if (!(probe.unit = get_audiounit(eRender, dev_id, FALSE)))
        return AUDCLNT_E_DEVICE_INVALIDATED;

    hr = ca_setup_spatial_audiounit(&probe, device, &bed);
    if (SUCCEEDED(hr))
    {
        sc = AudioUnitInitialize(probe.spatial_unit);
        if (sc == noErr)
        {
            probe.spatial_unit_initialized = TRUE;
            sc = AudioUnitInitialize(probe.unit);
        }
        if (sc == noErr)
            probe.unit_initialized = TRUE;
        else
            hr = osstatus_to_hresult(sc);
    }
    release_spatial_probe(&probe);
    return hr;
}

static HRESULT query_spatial_audio_capabilities(const char *device,
        struct spatial_audio_capabilities *capabilities, BOOL validate_graph)
{
    WAVEFORMATEXTENSIBLE mix, verify_mix;
    AudioComponentInstance mixer;
    AudioDeviceID dev_id;
    UInt32 minimum_frames, verify_minimum, input_buses, size;
    UINT32 candidate, low, high, verify_period, verify_rate;
    UINT32 hash = 2166136261u;
    OSStatus sc;
    HRESULT hr;

    memset(capabilities, 0, sizeof(*capabilities));
    if (!device || (dev_id = dev_id_from_device(device)) == kAudioObjectUnknown)
        return AUDCLNT_E_DEVICE_INVALIDATED;
    if (FAILED(hr = get_device_timing(dev_id, &capabilities->period_frames,
            &minimum_frames, &capabilities->sample_rate)) ||
            FAILED(hr = get_device_mix_format(device, &mix)))
        return hr;

    hash = spatial_generation_hash(hash, &dev_id, sizeof(dev_id));
    hash = spatial_generation_hash(hash, &capabilities->period_frames,
            sizeof(capabilities->period_frames));
    hash = spatial_generation_hash(hash, &minimum_frames,
            sizeof(minimum_frames));
    hash = spatial_generation_hash(hash, &capabilities->sample_rate,
            sizeof(capabilities->sample_rate));
    hash = spatial_generation_hash(hash, &mix.Format.nChannels,
            sizeof(mix.Format.nChannels));
    hash = spatial_generation_hash(hash, &mix.dwChannelMask,
            sizeof(mix.dwChannelMask));
    capabilities->endpoint_generation = hash ? hash : 1;

    if (!(mixer = get_spatial_mixer()))
        return S_OK;
    size = sizeof(input_buses);
    sc = AudioUnitGetProperty(mixer, kAudioUnitProperty_ElementCount,
            kAudioUnitScope_Input, 0, &input_buses, &size);
    AudioComponentInstanceDispose(mixer);
    if (sc != noErr || input_buses <= 1)
        return S_OK;

    capabilities->max_dynamic_objects = min(input_buses - 1,
            SPATIAL_AUDIO_MAX_DYNAMIC_OBJECTS);

    if (!validate_graph || !capabilities->max_dynamic_objects)
        return S_OK;

    candidate = capabilities->max_dynamic_objects;
    hr = validate_spatial_dynamic_capacity(device, dev_id,
            capabilities->period_frames, capabilities->sample_rate, candidate);
    if (FAILED(hr) && hr != E_OUTOFMEMORY)
    {
        /* ElementCount is only an upper bound.  Some AudioUnits expose more
         * buses than a concrete endpoint can initialize, so find the highest
         * viable PointSource count without turning a mixer limitation into a
         * static-stream or timing failure. */
        low = 0;
        high = candidate - 1;
        while (low < high)
        {
            candidate = low + (high - low + 1) / 2;
            hr = validate_spatial_dynamic_capacity(device, dev_id,
                    capabilities->period_frames, capabilities->sample_rate,
                    candidate);
            if (SUCCEEDED(hr))
                low = candidate;
            else if (hr == E_OUTOFMEMORY)
                return hr;
            else
                high = candidate - 1;
        }
        capabilities->max_dynamic_objects = low;
    }
    else if (FAILED(hr))
        return hr;

    /* A graph error is a supported max=0 fallback only while the endpoint
     * queried above is still the same endpoint at the same timing. */
    if (FAILED(hr = get_device_timing(dev_id, &verify_period,
            &verify_minimum, &verify_rate)) ||
            FAILED(hr = get_device_mix_format(device, &verify_mix)))
        return hr;
    if (verify_period != capabilities->period_frames ||
            verify_minimum != minimum_frames ||
            verify_rate != capabilities->sample_rate ||
            verify_mix.Format.nChannels != mix.Format.nChannels ||
            verify_mix.dwChannelMask != mix.dwChannelMask)
        return AUDCLNT_E_DEVICE_INVALIDATED;
    return S_OK;
}

static NTSTATUS unix_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct spatial_audio_capabilities capabilities;
    struct coreaudio_stream *stream;
    WAVEFORMATEXTENSIBLE spatial_bed = {0};
    AURenderCallbackStruct input;
    kern_return_t kr;
    HRESULT hr;
    OSStatus sc;
    int computed_frames;
    SIZE_T size;

    params->result = S_OK;

    if (!(stream = calloc(1, sizeof(*stream)))) {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    stream->fmt = clone_format(params->fmt);
    if(!stream->fmt){
        params->result = E_OUTOFMEMORY;
        goto end;
    }

    stream->period = params->period;
    computed_frames = muldiv(params->period, stream->fmt->nSamplesPerSec,
            10000000);
    if (computed_frames <= 0)
    {
        params->result = E_INVALIDARG;
        goto end;
    }
    stream->period_frames = computed_frames;

    stream->dev_id = dev_id_from_device(params->device);
    stream->flow = params->flow;
    stream->flags = params->flags;
    stream->spatial = params->spatial;
    stream->spatial_static_mask = params->spatial_static_mask;
    stream->spatial_dynamic_objects = params->spatial_dynamic_objects;
    stream->spatial_endpoint_generation = params->spatial_endpoint_generation;
    stream->share = params->share;
    /* The DefaultOutput unit can migrate to a device with a different speaker
     * layout without giving us a safe point at which to rebuild the spatial
     * graph. Pin spatial streams to their activation endpoint rather than
     * rendering a stale speaker layout or HRTF mode on the new device. */
    stream->follows_default = !stream->spatial &&
            stream->flow == eRender && stream->dev_id == default_output_id;

    if (stream->spatial)
    {
        kr = semaphore_create(mach_task_self(),
                &stream->callback_drain_semaphore, SYNC_POLICY_FIFO, 0);
        if (kr != KERN_SUCCESS)
        {
            params->result = E_OUTOFMEMORY;
            goto end;
        }
        stream->callback_drain_semaphore_created = TRUE;
    }
    if (stream->spatial && (stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
    {
        kr = semaphore_create(mach_task_self(), &stream->event_semaphore,
                SYNC_POLICY_FIFO, 0);
        if (kr != KERN_SUCCESS)
        {
            params->result = E_OUTOFMEMORY;
            goto end;
        }
        stream->event_semaphore_created = TRUE;
    }

    if (stream->spatial)
    {
        UINT32 i;

        if (stream->flow != eRender || stream->share != AUDCLNT_SHAREMODE_SHARED)
        {
            params->result = E_INVALIDARG;
            goto end;
        }
        if (FAILED(params->result = query_spatial_audio_capabilities(
                params->device, &capabilities, FALSE)))
            goto end;
        if (stream->spatial_endpoint_generation &&
                stream->spatial_endpoint_generation !=
                        capabilities.endpoint_generation)
        {
            params->result = AUDCLNT_E_DEVICE_INVALIDATED;
            goto end;
        }
        if (stream->spatial_dynamic_objects >
                capabilities.max_dynamic_objects)
        {
            params->result = SPTLAUDCLNT_E_NO_MORE_OBJECTS;
            goto end;
        }
        if (FAILED(params->result = get_spatial_bed_format(stream->fmt,
                stream->spatial_static_mask, stream->spatial_dynamic_objects,
                &spatial_bed, &stream->spatial_dry_channel,
                &stream->spatial_dynamic_channel)))
            goto end;
        stream->spatial_bed_channels = spatial_bed.Format.nChannels;

        if (!(stream->spatial_volume_bits = malloc(stream->fmt->nChannels *
                sizeof(*stream->spatial_volume_bits))))
        {
            params->result = E_OUTOFMEMORY;
            goto end;
        }
        for (i = 0; i < stream->fmt->nChannels; ++i)
        {
            float volume = 1.0f;

            memcpy(&stream->spatial_volume_bits[i], &volume,
                    sizeof(stream->spatial_volume_bits[i]));
        }
        stream->spatial_volumes_are_unity = TRUE;
        if (getenv("SWITCHYARD_SPATIAL_AUDIO_PROFILE"))
        {
            if (!(stream->callback_timings = calloc(
                    SPATIAL_AUDIO_PROFILE_SAMPLES,
                    sizeof(*stream->callback_timings))))
            {
                params->result = E_OUTOFMEMORY;
                goto end;
            }
            stream->profile_callbacks = TRUE;
        }
        if (FAILED(params->result = register_spatial_device_listeners(stream)))
            goto end;
    }

    computed_frames = muldiv(params->duration, stream->fmt->nSamplesPerSec,
            10000000);
    if (computed_frames <= 0)
    {
        params->result = E_INVALIDARG;
        goto end;
    }
    stream->bufsize_frames = computed_frames;
    if(params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;

    if(!(stream->unit = get_audiounit(stream->flow, stream->dev_id, stream->follows_default))){
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }

    if (stream->spatial &&
            (stream->spatial_dynamic_objects ||
             get_device_spatial_output_type(stream->dev_id) ==
                    kSpatialMixerOutputType_Headphones ||
             (spatial_bed.dwChannelMask & SPATIAL_AUDIO_PRIVATE_BED_SPEAKERS) ||
             !device_supports_spatial_format(params->device, &spatial_bed.Format)))
    {
        params->result = ca_setup_spatial_audiounit(stream, params->device,
                &spatial_bed);
        if (FAILED(params->result))
            goto end;
    }
    else
    {
        params->result = ca_setup_audiounit(stream->flow, stream->unit,
                stream->spatial ? &spatial_bed.Format : stream->fmt,
                &stream->dev_desc, &stream->converter);
        if (FAILED(params->result))
            goto end;

        input.inputProcRefCon = stream;
        if (stream->flow == eCapture)
        {
            input.inputProc = ca_capture_cb;
            sc = AudioUnitSetProperty(stream->unit, kAudioOutputUnitProperty_SetInputCallback,
                    kAudioUnitScope_Output, 1, &input, sizeof(input));
        }
        else
        {
            input.inputProc = stream->spatial
                    ? ca_native_spatial_render_cb : ca_render_cb;
            sc = AudioUnitSetProperty(stream->unit, kAudioUnitProperty_SetRenderCallback,
                    kAudioUnitScope_Input, 0, &input, sizeof(input));
            if (stream->spatial)
            {
                stream->spatial_native_output = TRUE;
                TRACE("Using lossless native output for a %u-channel spatial bed.\n",
                        spatial_bed.Format.nChannels);
            }
        }
        if (sc != noErr)
        {
            WARN("Couldn't set callback: %x\n", (int)sc);
            params->result = osstatus_to_hresult(sc);
            goto end;
        }
        if (stream->spatial &&
                (FAILED(hr = configure_spatial_dry_output(stream,
                        spatial_bed.dwChannelMask, spatial_bed.Format.nChannels)) ||
                 FAILED(hr = configure_spatial_max_frames(stream))))
        {
            params->result = hr;
            goto end;
        }
    }

    if (stream->spatial_unit)
    {
        sc = AudioUnitInitialize(stream->spatial_unit);
        if (sc != noErr)
        {
            WARN("Couldn't initialize spatial mixer: %x\n", (int)sc);
            params->result = osstatus_to_hresult(sc);
            goto end;
        }
        stream->spatial_unit_initialized = TRUE;

        if (FAILED(params->result = configure_spatial_dry_delay(stream)))
            goto end;
    }

    sc = AudioUnitInitialize(stream->unit);
    if(sc != noErr){
        WARN("Couldn't initialize: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        goto end;
    }
    stream->unit_initialized = TRUE;

    /* we play audio continuously because AudioOutputUnitStart sometimes takes
     * a while to return */
    sc = AudioOutputUnitStart(stream->unit);
    if(sc != noErr){
        WARN("Unit failed to start: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        goto end;
    }
    stream->unit_started = TRUE;

    if (stream->bufsize_frames > SIZE_MAX / stream->fmt->nBlockAlign)
    {
        params->result = E_OUTOFMEMORY;
        goto end;
    }
    size = (SIZE_T)stream->bufsize_frames * stream->fmt->nBlockAlign;
    if(NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                               &size, MEM_COMMIT, PAGE_READWRITE)){
        params->result = E_OUTOFMEMORY;
        goto end;
    }
    silence_buffer(stream, stream->local_buffer, stream->bufsize_frames);

    if (stream->spatial)
    {
        size = (SIZE_T)stream->bufsize_frames * stream->fmt->nBlockAlign;
        if (NtAllocateVirtualMemory(GetCurrentProcess(),
                (void **)&stream->tmp_buffer, zero_bits, &size, MEM_COMMIT,
                PAGE_READWRITE))
        {
            params->result = E_OUTOFMEMORY;
            goto end;
        }
        stream->tmp_buffer_frames = stream->bufsize_frames;
    }

    if(stream->flow == eCapture){
        stream->cap_bufsize_frames = muldiv(params->duration, stream->dev_desc.mSampleRate, 10000000);
        if (stream->cap_bufsize_frames > SIZE_MAX / stream->fmt->nBlockAlign ||
                !(stream->cap_buffer = malloc((size_t)stream->cap_bufsize_frames *
                        stream->fmt->nBlockAlign)))
        {
            params->result = E_OUTOFMEMORY;
            goto end;
        }
    }
    if (stream->spatial)
    {
        struct spatial_audio_capabilities verified;

        /* Close the activation/listener registration window.  A device can
         * change after the caller's capability snapshot but before the graph
         * is constructed; re-snapshot after graph initialization, then drain
         * every property callback queued before this point. */
        params->result = query_spatial_audio_capabilities(params->device,
                &verified, FALSE);
        if (SUCCEEDED(params->result) && stream->device_listener_queue)
            dispatch_sync_f(stream->device_listener_queue, NULL,
                    dispatch_barrier_noop);
        if (SUCCEEDED(params->result) &&
                (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE) ||
                 (stream->spatial_endpoint_generation &&
                  verified.endpoint_generation !=
                        stream->spatial_endpoint_generation) ||
                 stream->spatial_dynamic_objects >
                        verified.max_dynamic_objects))
            params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        if (FAILED(params->result))
            goto end;
    }
    params->result = S_OK;

end:
    if(FAILED(params->result)){
        if(stream->converter) AudioConverterDispose(stream->converter);
        __atomic_store_n(&stream->shutting_down, TRUE, __ATOMIC_RELEASE);
        unregister_spatial_device_listeners(stream);
        if (stream->unit_started)
        {
            AudioOutputUnitStop(stream->unit);
            if (stream->spatial)
                wait_for_spatial_callbacks(stream);
        }
        if (stream->unit_initialized) AudioUnitUninitialize(stream->unit);
        if (stream->spatial_unit_initialized) AudioUnitUninitialize(stream->spatial_unit);
        if (stream->spatial_unit) AudioComponentInstanceDispose(stream->spatial_unit);
        if(stream->unit) AudioComponentInstanceDispose(stream->unit);
        free(stream->spatial_volume_bits);
        free(stream->spatial_bed_buffer);
        free(stream->spatial_dry_buffer);
        free(stream->spatial_dry_delay_buffer);
        free(stream->spatial_dynamic_buffer);
        free(stream->spatial_input_contexts);
        free(stream->spatial_parameter_events);
        free(stream->spatial_object_states);
        free(stream->spatial_metadata_sequences);
        free(stream->callback_timings);
        free(stream->cap_buffer);
        if (stream->local_buffer)
        {
            size = 0;
            NtFreeVirtualMemory(GetCurrentProcess(),
                    (void **)&stream->local_buffer, &size, MEM_RELEASE);
        }
        if (stream->tmp_buffer)
        {
            size = 0;
            NtFreeVirtualMemory(GetCurrentProcess(),
                    (void **)&stream->tmp_buffer, &size, MEM_RELEASE);
        }
        if (stream->event_semaphore_created)
            semaphore_destroy(mach_task_self(), stream->event_semaphore);
        if (stream->callback_drain_semaphore_created)
            semaphore_destroy(mach_task_self(),
                    stream->callback_drain_semaphore);
        free(stream->fmt);
        free(stream);
    } else {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }

    return STATUS_SUCCESS;
}

static int compare_uint64(const void *left, const void *right)
{
    UINT64 a = *(const UINT64 *)left;
    UINT64 b = *(const UINT64 *)right;

    return a < b ? -1 : a > b;
}

static UINT64 spatial_profile_nanoseconds(UINT64 ticks,
        const mach_timebase_info_data_t *timebase)
{
    long double value = (long double)ticks * timebase->numer / timebase->denom;

    return (UINT64)value;
}

static void report_spatial_profile(struct coreaudio_stream *stream)
{
    mach_timebase_info_data_t timebase;
    UINT32 count;

    if (!stream->profile_callbacks)
        return;
    count = min(__atomic_load_n(&stream->callback_timing_count,
            __ATOMIC_ACQUIRE), SPATIAL_AUDIO_PROFILE_SAMPLES);
    if (!count || mach_timebase_info(&timebase) != KERN_SUCCESS)
        return;

    qsort(stream->callback_timings, count, sizeof(*stream->callback_timings),
            compare_uint64);
    TRACE("Spatial callback profile: native %u, dynamic objects %u, samples %u, p50 %llu ns, p95 %llu ns, p99 %llu ns, underruns %llu, overruns %llu, reentrant rejects %llu, callback allocations 0.\n",
            stream->spatial_native_output, stream->spatial_dynamic_objects,
            count, (unsigned long long)spatial_profile_nanoseconds(
                    stream->callback_timings[(count - 1) * 50 / 100], &timebase),
            (unsigned long long)spatial_profile_nanoseconds(
                    stream->callback_timings[(count - 1) * 95 / 100], &timebase),
            (unsigned long long)spatial_profile_nanoseconds(
                    stream->callback_timings[(count - 1) * 99 / 100], &timebase),
            (unsigned long long)__atomic_load_n(&stream->underruns,
                    __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&stream->overruns,
                    __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&stream->reentrant_callbacks,
                    __ATOMIC_RELAXED));
}

static NTSTATUS unix_release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;

    __atomic_store_n(&stream->shutting_down, TRUE, __ATOMIC_RELEASE);
    unregister_spatial_device_listeners(stream);
    if(stream->timer_thread){
        __atomic_store_n(&stream->please_quit, TRUE, __ATOMIC_RELEASE);
        if (stream->event_semaphore_created)
            semaphore_signal(stream->event_semaphore);
        NtWaitForSingleObject(stream->timer_thread, FALSE, NULL);
        NtClose(stream->timer_thread);
    }

    if(stream->unit){
        if (stream->unit_started)
        {
            AudioOutputUnitStop(stream->unit);
            if (stream->spatial)
                wait_for_spatial_callbacks(stream);
        }
        if (stream->unit_initialized) AudioUnitUninitialize(stream->unit);
    }
    if (stream->spatial_unit)
    {
        if (stream->spatial_unit_initialized) AudioUnitUninitialize(stream->spatial_unit);
        AudioComponentInstanceDispose(stream->spatial_unit);
    }
    if (stream->unit)
        AudioComponentInstanceDispose(stream->unit);

    report_spatial_profile(stream);

    if(stream->converter) AudioConverterDispose(stream->converter);
    free(stream->spatial_volume_bits);
    free(stream->spatial_bed_buffer);
    free(stream->spatial_dry_buffer);
    free(stream->spatial_dry_delay_buffer);
    free(stream->spatial_dynamic_buffer);
    free(stream->spatial_input_contexts);
    free(stream->spatial_parameter_events);
    free(stream->spatial_object_states);
    free(stream->spatial_metadata_sequences);
    free(stream->callback_timings);
    if (stream->event_semaphore_created)
        semaphore_destroy(mach_task_self(), stream->event_semaphore);
    if (stream->callback_drain_semaphore_created)
        semaphore_destroy(mach_task_self(), stream->callback_drain_semaphore);
    free(stream->resamp_buffer);
    free(stream->wrap_buffer);
    free(stream->cap_buffer);
    if(stream->local_buffer){
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                            &size, MEM_RELEASE);
    }
    if(stream->tmp_buffer){
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                            &size, MEM_RELEASE);
    }
    free(stream->fmt);
    free(stream);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT ca_channel_layout_to_channel_mask(const AudioChannelLayout *layout)
{
    int i;
    UINT mask = 0;

    for (i = 0; i < layout->mNumberChannelDescriptions; ++i) {
        switch (layout->mChannelDescriptions[i].mChannelLabel) {
            default: FIXME("Unhandled channel 0x%x\n",
                           (unsigned int)layout->mChannelDescriptions[i].mChannelLabel); break;
            case kAudioChannelLabel_Unknown: break;
            case kAudioChannelLabel_Left: mask |= SPEAKER_FRONT_LEFT; break;
            case kAudioChannelLabel_Mono:
            case kAudioChannelLabel_Center: mask |= SPEAKER_FRONT_CENTER; break;
            case kAudioChannelLabel_Right: mask |= SPEAKER_FRONT_RIGHT; break;
            case kAudioChannelLabel_RearSurroundLeft:
            case kAudioChannelLabel_LeftBackSurround:
            case kAudioChannelLabel_LeftSurround: mask |= SPEAKER_BACK_LEFT; break;
            case kAudioChannelLabel_CenterSurroundDirect:
            case kAudioChannelLabel_CenterSurround: mask |= SPEAKER_BACK_CENTER; break;
            case kAudioChannelLabel_RearSurroundRight:
            case kAudioChannelLabel_RightBackSurround:
            case kAudioChannelLabel_RightSurround: mask |= SPEAKER_BACK_RIGHT; break;
            case kAudioChannelLabel_LFEScreen: mask |= SPEAKER_LOW_FREQUENCY; break;
            case kAudioChannelLabel_LeftSideSurround:
            case kAudioChannelLabel_LeftSurroundDirect: mask |= SPEAKER_SIDE_LEFT; break;
            case kAudioChannelLabel_RightSideSurround:
            case kAudioChannelLabel_RightSurroundDirect: mask |= SPEAKER_SIDE_RIGHT; break;
            case kAudioChannelLabel_TopCenterSurround: mask |= SPEAKER_TOP_CENTER; break;
            case kAudioChannelLabel_VerticalHeightLeft: mask |= SPEAKER_TOP_FRONT_LEFT; break;
            case kAudioChannelLabel_VerticalHeightCenter: mask |= SPEAKER_TOP_FRONT_CENTER; break;
            case kAudioChannelLabel_VerticalHeightRight: mask |= SPEAKER_TOP_FRONT_RIGHT; break;
            case kAudioChannelLabel_LeftTopRear:
            case kAudioChannelLabel_TopBackLeft: mask |= SPEAKER_TOP_BACK_LEFT; break;
            case kAudioChannelLabel_CenterTopRear:
            case kAudioChannelLabel_TopBackCenter: mask |= SPEAKER_TOP_BACK_CENTER; break;
            case kAudioChannelLabel_RightTopRear:
            case kAudioChannelLabel_TopBackRight: mask |= SPEAKER_TOP_BACK_RIGHT; break;
            case kAudioChannelLabel_LeftCenter: mask |= SPEAKER_FRONT_LEFT_OF_CENTER; break;
            case kAudioChannelLabel_RightCenter: mask |= SPEAKER_FRONT_RIGHT_OF_CENTER; break;
        }
    }

    return mask;
}

/* For most hardware on Windows, users must choose a configuration with an even
 * number of channels (stereo, quad, 5.1, 7.1). Users can then disable
 * channels, but those channels are still reported to applications from
 * GetMixFormat! Some applications behave badly if given an odd number of
 * channels (e.g. 2.1).  Here, we find the nearest configuration that Windows
 * would report for a given channel layout. */
static void convert_channel_layout(const AudioChannelLayout *ca_layout, WAVEFORMATEXTENSIBLE *fmt)
{
    UINT ca_mask = ca_channel_layout_to_channel_mask(ca_layout);

    TRACE("Got channel mask for CA: 0x%x\n", ca_mask);

    if (ca_layout->mNumberChannelDescriptions == 1)
    {
        fmt->Format.nChannels = 1;
        fmt->dwChannelMask = ca_mask;
        return;
    }

    /* compare against known configurations and find smallest configuration
     * which is a superset of the given speakers */

    if (ca_layout->mNumberChannelDescriptions <= 2 &&
            (ca_mask & ~KSAUDIO_SPEAKER_STEREO) == 0)
    {
        fmt->Format.nChannels = 2;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 4 &&
            (ca_mask & ~KSAUDIO_SPEAKER_QUAD) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_QUAD;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 4 &&
            (ca_mask & ~KSAUDIO_SPEAKER_SURROUND) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_SURROUND;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 6 &&
            (ca_mask & ~KSAUDIO_SPEAKER_5POINT1) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 6 &&
            (ca_mask & ~KSAUDIO_SPEAKER_5POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 8 &&
            (ca_mask & ~KSAUDIO_SPEAKER_7POINT1) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 8 &&
            (ca_mask & ~KSAUDIO_SPEAKER_7POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
        return;
    }

    /* oddball format, report truthfully */
    fmt->Format.nChannels = ca_layout->mNumberChannelDescriptions;
    fmt->dwChannelMask = ca_mask;
}

static DWORD get_channel_mask(unsigned int channels)
{
    switch(channels){
    case 0:
        return 0;
    case 1:
        return KSAUDIO_SPEAKER_MONO;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    case 3:
        return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;    /* not _SURROUND */
    case 5:
        return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1; /* not 5POINT1_SURROUND */
    case 7:
        return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND; /* Vista deprecates 7POINT1 */
    }
    FIXME("Unknown speaker configuration: %u\n", channels);
    return 0;
}

static NTSTATUS unix_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;
    AudioObjectPropertyAddress addr;
    AudioChannelLayout *layout;
    AudioBufferList *buffers;
    Float64 rate;
    UInt32 size;
    OSStatus sc;
    int i;
    const AudioDeviceID dev_id = dev_id_from_device(params->device);

    params->fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;

    addr.mScope = get_scope(params->flow);
    addr.mElement = 0;
    addr.mSelector = kAudioDevicePropertyPreferredChannelLayout;

    sc = AudioObjectGetPropertyDataSize(dev_id, &addr, 0, NULL, &size);
    if(sc == noErr){
        layout = malloc(size);
        sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, layout);
        if(sc == noErr){
            TRACE("Got channel layout: {tag: 0x%x, bitmap: 0x%x, num_descs: %u}\n",
                  (unsigned int)layout->mChannelLayoutTag, (unsigned int)layout->mChannelBitmap,
                  (unsigned int)layout->mNumberChannelDescriptions);

            if(layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions){
                convert_channel_layout(layout, params->fmt);
            }else{
                WARN("Haven't implemented support for this layout tag: 0x%x, guessing at layout\n",
                     (unsigned int)layout->mChannelLayoutTag);
                params->fmt->Format.nChannels = 0;
            }
        }else{
            TRACE("Unable to get _PreferredChannelLayout property: %x, guessing at layout\n", (int)sc);
            params->fmt->Format.nChannels = 0;
        }

        free(layout);
    }else{
        TRACE("Unable to get size for _PreferredChannelLayout property: %x, guessing at layout\n", (int)sc);
        params->fmt->Format.nChannels = 0;
    }

    if(params->fmt->Format.nChannels == 0){
        addr.mScope = get_scope(params->flow);
        addr.mElement = 0;
        addr.mSelector = kAudioDevicePropertyStreamConfiguration;

        sc = AudioObjectGetPropertyDataSize(dev_id, &addr, 0, NULL, &size);
        if(sc != noErr){
            WARN("Unable to get size for _StreamConfiguration property: %x\n", (int)sc);
            params->result = osstatus_to_hresult(sc);
            return STATUS_SUCCESS;
        }

        buffers = malloc(size);
        if(!buffers){
            params->result = E_OUTOFMEMORY;
            return STATUS_SUCCESS;
        }

        sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, buffers);
        if(sc != noErr){
            free(buffers);
            WARN("Unable to get _StreamConfiguration property: %x\n", (int)sc);
            params->result = osstatus_to_hresult(sc);
            return STATUS_SUCCESS;
        }

        for(i = 0; i < buffers->mNumberBuffers; ++i)
            params->fmt->Format.nChannels += buffers->mBuffers[i].mNumberChannels;

        free(buffers);

        params->fmt->dwChannelMask = get_channel_mask(params->fmt->Format.nChannels);
    }

    addr.mSelector = kAudioDevicePropertyNominalSampleRate;
    size = sizeof(Float64);
    sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, &rate);
    if(sc != noErr){
        WARN("Unable to get _NominalSampleRate property: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }
    params->fmt->Format.nSamplesPerSec = rate;

    params->fmt->Format.wBitsPerSample = 32;
    params->fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    params->fmt->Format.nBlockAlign = (params->fmt->Format.wBitsPerSample *
                                       params->fmt->Format.nChannels) / 8;
    params->fmt->Format.nAvgBytesPerSec = params->fmt->Format.nSamplesPerSec *
        params->fmt->Format.nBlockAlign;

    params->fmt->Samples.wValidBitsPerSample = params->fmt->Format.wBitsPerSample;
    params->fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;
    AudioStreamBasicDescription dev_desc;
    AudioConverterRef converter;
    AudioComponentInstance unit;
    const AudioDeviceID dev_id = dev_id_from_device(params->device);

    unit = get_audiounit(params->flow, dev_id, FALSE);

    converter = NULL;
    params->result = ca_setup_audiounit(params->flow, unit, params->fmt_in, &dev_desc, &converter);
    AudioComponentInstanceDispose(unit);
    if(converter) AudioConverterDispose(converter);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_device_period(void *args)
{
    struct get_device_period_params *params = args;
    AudioDeviceID dev_id;
    UINT32 frames, minimum_frames, rate;
    HRESULT hr;

    if (!params->device ||
            (dev_id = dev_id_from_device(params->device)) == kAudioObjectUnknown)
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (FAILED(hr = get_device_timing(dev_id, &frames, &minimum_frames,
            &rate)))
    {
        params->result = hr;
        return STATUS_SUCCESS;
    }

    if (params->def_period)
        *params->def_period = ((UINT64)frames * 10000000 + rate / 2) / rate;
    if (params->min_period)
        *params->min_period = ((UINT64)minimum_frames * 10000000 + rate / 2) /
                rate;
    params->result = S_OK;

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_spatial_audio_capabilities(void *args)
{
    struct get_spatial_audio_capabilities_params *params = args;

    params->result = query_spatial_audio_capabilities(params->device,
            &params->capabilities, TRUE);
    return STATUS_SUCCESS;
}

static UINT buf_ptr_diff(UINT left, UINT right, UINT bufsize)
{
    if(left <= right)
        return right - left;
    return bufsize - (left - right);
}

/* place data from cap_buffer into provided AudioBufferList */
static OSStatus feed_cb(AudioConverterRef converter, UInt32 *nframes, AudioBufferList *data,
                        AudioStreamPacketDescription **packets, void *user)
{
    struct coreaudio_stream *stream = user;

    *nframes = min(*nframes, stream->cap_held_frames);
    if(!*nframes){
        data->mBuffers[0].mData = NULL;
        data->mBuffers[0].mDataByteSize = 0;
        data->mBuffers[0].mNumberChannels = stream->fmt->nChannels;
        return noErr;
    }

    data->mBuffers[0].mDataByteSize = *nframes * stream->fmt->nBlockAlign;
    data->mBuffers[0].mNumberChannels = stream->fmt->nChannels;

    if(stream->cap_offs_frames + *nframes > stream->cap_bufsize_frames){
        UINT32 chunk_frames = stream->cap_bufsize_frames - stream->cap_offs_frames;

        if(stream->wrap_bufsize_frames < *nframes){
            free(stream->wrap_buffer);
            stream->wrap_buffer = malloc(data->mBuffers[0].mDataByteSize);
            stream->wrap_bufsize_frames = *nframes;
        }

        memcpy(stream->wrap_buffer, stream->cap_buffer + stream->cap_offs_frames * stream->fmt->nBlockAlign,
               chunk_frames * stream->fmt->nBlockAlign);
        memcpy(stream->wrap_buffer + chunk_frames * stream->fmt->nBlockAlign, stream->cap_buffer,
               (*nframes - chunk_frames) * stream->fmt->nBlockAlign);

        data->mBuffers[0].mData = stream->wrap_buffer;
    }else
        data->mBuffers[0].mData = stream->cap_buffer + stream->cap_offs_frames * stream->fmt->nBlockAlign;

    stream->cap_offs_frames += *nframes;
    stream->cap_offs_frames %= stream->cap_bufsize_frames;
    stream->cap_held_frames -= *nframes;

    if(packets)
        *packets = NULL;

    return noErr;
}

static void capture_resample(struct coreaudio_stream *stream)
{
    UINT32 resamp_period_frames = muldiv(stream->period_frames, stream->dev_desc.mSampleRate,
                                         stream->fmt->nSamplesPerSec);
    OSStatus sc;

    /* the resampling process often needs more source frames than we'd
     * guess from a straight conversion using the sample rate ratio. so
     * only convert if we have extra source data. */
    while(stream->cap_held_frames > resamp_period_frames * 2){
        AudioBufferList converted_list;
        UInt32 wanted_frames = stream->period_frames;

        converted_list.mNumberBuffers = 1;
        converted_list.mBuffers[0].mNumberChannels = stream->fmt->nChannels;
        converted_list.mBuffers[0].mDataByteSize = wanted_frames * stream->fmt->nBlockAlign;

        if(stream->resamp_bufsize_frames < wanted_frames){
            free(stream->resamp_buffer);
            stream->resamp_buffer = malloc(converted_list.mBuffers[0].mDataByteSize);
            stream->resamp_bufsize_frames = wanted_frames;
        }

        converted_list.mBuffers[0].mData = stream->resamp_buffer;

        sc = AudioConverterFillComplexBuffer(stream->converter, feed_cb,
                                             stream, &wanted_frames, &converted_list, NULL);
        if(sc != noErr){
            WARN("AudioConverterFillComplexBuffer failed: %x\n", (int)sc);
            break;
        }

        ca_wrap_buffer(stream->local_buffer,
                       stream->wri_offs_frames * stream->fmt->nBlockAlign,
                       stream->bufsize_frames * stream->fmt->nBlockAlign,
                       stream->resamp_buffer, wanted_frames * stream->fmt->nBlockAlign);

        stream->wri_offs_frames += wanted_frames;
        stream->wri_offs_frames %= stream->bufsize_frames;
        if(stream->held_frames + wanted_frames > stream->bufsize_frames){
            stream->lcl_offs_frames += buf_ptr_diff(stream->lcl_offs_frames, stream->wri_offs_frames,
                                                    stream->bufsize_frames);
            stream->held_frames = stream->bufsize_frames;
        }else
            stream->held_frames += wanted_frames;
    }
}

static NTSTATUS unix_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    os_unfair_lock_lock(&stream->lock);
    *params->frames = stream->bufsize_frames;
    os_unfair_lock_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static HRESULT ca_get_max_stream_latency(struct coreaudio_stream *stream, UInt32 *max)
{
    AudioObjectPropertyAddress addr;
    AudioDeviceID dev_id;
    AudioStreamID *ids;
    UInt32 size;
    OSStatus sc;
    int nstreams, i;

    addr.mScope = get_scope(stream->flow);
    addr.mElement = 0;
    addr.mSelector = kAudioDevicePropertyStreams;

    dev_id = get_stream_device(stream);
    sc = AudioObjectGetPropertyDataSize(dev_id, &addr, 0, NULL, &size);
    if(sc != noErr){
        WARN("Unable to get size for _Streams property: %x\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    ids = malloc(size);
    if(!ids)
        return E_OUTOFMEMORY;

    sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, ids);
    if(sc != noErr){
        WARN("Unable to get _Streams property: %x\n", (int)sc);
        free(ids);
        return osstatus_to_hresult(sc);
    }

    nstreams = size / sizeof(AudioStreamID);
    *max = 0;

    addr.mSelector = kAudioStreamPropertyLatency;
    for(i = 0; i < nstreams; ++i){
        UInt32 latency;

        size = sizeof(latency);
        sc = AudioObjectGetPropertyData(ids[i], &addr, 0, NULL, &size, &latency);
        if(sc != noErr){
            WARN("Unable to get _Latency property: %x\n", (int)sc);
            continue;
        }

        if(latency > *max)
            *max = latency;
    }

    free(ids);

    return S_OK;
}

static NTSTATUS unix_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    AudioDeviceID dev_id;
    UINT32 period_frames, minimum_frames, sample_rate;
    UInt32 latency, safety_offset, stream_latency, size;
    UINT64 device_frames, latency_time;
    AudioObjectPropertyAddress addr;
    HRESULT hr;
    OSStatus sc;

    os_unfair_lock_lock(&stream->lock);

    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
    {
        os_unfair_lock_unlock(&stream->lock);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    addr.mScope = get_scope(stream->flow);
    addr.mSelector = kAudioDevicePropertyLatency;
    addr.mElement = 0;

    dev_id = get_stream_device(stream);
    size = sizeof(latency);
    sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size, &latency);
    if(sc != noErr){
        WARN("Couldn't get _Latency property: %x\n", (int)sc);
        os_unfair_lock_unlock(&stream->lock);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }

    params->result = ca_get_max_stream_latency(stream, &stream_latency);
    if(FAILED(params->result)){
        os_unfair_lock_unlock(&stream->lock);
        return STATUS_SUCCESS;
    }

    addr.mSelector = kAudioDevicePropertySafetyOffset;
    size = sizeof(safety_offset);
    sc = AudioObjectGetPropertyData(dev_id, &addr, 0, NULL, &size,
            &safety_offset);
    if (sc != noErr)
    {
        WARN("Couldn't get _SafetyOffset property: %x\n", (int)sc);
        os_unfair_lock_unlock(&stream->lock);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }
    if (FAILED(hr = get_device_timing(dev_id, &period_frames,
            &minimum_frames, &sample_rate)))
    {
        os_unfair_lock_unlock(&stream->lock);
        params->result = hr;
        return STATUS_SUCCESS;
    }

    device_frames = (UINT64)latency + safety_offset + stream_latency;
    if (device_frames > (~(UINT64)0 - sample_rate + 1) / 10000000)
    {
        os_unfair_lock_unlock(&stream->lock);
        params->result = E_FAIL;
        return STATUS_SUCCESS;
    }
    latency_time = (device_frames * 10000000 + sample_rate - 1) /
            sample_rate;
    if (latency_time > INT64_MAX - stream->period)
    {
        os_unfair_lock_unlock(&stream->lock);
        params->result = E_FAIL;
        return STATUS_SUCCESS;
    }
    /* Include one producer quantum in the worst-case stream latency. */
    *params->latency = latency_time + stream->period;
    if (stream->spatial_dry_delay_frames)
    {
        UINT64 spatial_time = ((UINT64)stream->spatial_dry_delay_frames *
                10000000 + stream->fmt->nSamplesPerSec - 1) /
                stream->fmt->nSamplesPerSec;

        if (spatial_time > INT64_MAX - *params->latency)
        {
            os_unfair_lock_unlock(&stream->lock);
            params->result = E_FAIL;
            return STATUS_SUCCESS;
        }
        *params->latency += spatial_time;
    }

    os_unfair_lock_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT32 get_current_padding_nolock(struct coreaudio_stream *stream)
{
    if (stream->spatial)
    {
        UINT64 read_frame = __atomic_load_n(&stream->spatial_read_frames,
                __ATOMIC_ACQUIRE);
        UINT64 write_frame = __atomic_load_n(&stream->spatial_write_frames,
                __ATOMIC_ACQUIRE);

        if (write_frame < read_frame)
            return 0;
        return min(write_frame - read_frame, stream->bufsize_frames);
    }
    if(stream->flow == eCapture) capture_resample(stream);
    return stream->held_frames;
}

static NTSTATUS unix_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    os_unfair_lock_lock(&stream->lock);
    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else
    {
        *params->padding = get_current_padding_nolock(stream);
        params->result = S_OK;
    }
    os_unfair_lock_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static void unix_timer_loop(void *args)
{
    struct coreaudio_stream *stream = args;
    LARGE_INTEGER delay, next, last;
    int adjust;

    if (stream->spatial)
    {
        while (!__atomic_load_n(&stream->please_quit, __ATOMIC_ACQUIRE))
        {
            semaphore_wait(stream->event_semaphore);
            if (__atomic_load_n(&stream->please_quit, __ATOMIC_ACQUIRE))
                break;
            __atomic_store_n(&stream->event_pending, 0, __ATOMIC_RELEASE);
            if (__atomic_load_n(&stream->playing, __ATOMIC_ACQUIRE) &&
                    stream->event)
                NtSetEvent(stream->event, NULL);
        }
        return;
    }

    delay.QuadPart = -stream->period;
    NtQueryPerformanceCounter(&last, NULL);
    next.QuadPart = last.QuadPart + stream->period;

    while(!__atomic_load_n(&stream->please_quit, __ATOMIC_ACQUIRE)){
        NtSetEvent(stream->event, NULL);
        NtDelayExecution(FALSE, &delay);
        NtQueryPerformanceCounter(&last, NULL);

        adjust = next.QuadPart - last.QuadPart;
        if(adjust > stream->period / 2)
            adjust = stream->period / 2;
        else if(adjust < -stream->period / 2)
            adjust = -stream->period / 2;

        delay.QuadPart = -(stream->period + adjust);
        next.QuadPart += stream->period;
    }
}

static NTSTATUS unix_start(void *args)
{
    struct start_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    static const WCHAR name[] = {'a','u','d','i','o','_','c','l','i','e','n','t','_','t','i','m','e','r',0};
    NTSTATUS status;

    os_unfair_lock_lock(&stream->lock);

    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else if((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        params->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
    else if(__atomic_load_n(&stream->playing, __ATOMIC_ACQUIRE))
        params->result = AUDCLNT_E_NOT_STOPPED;
    else{
        __atomic_store_n(&stream->playing, TRUE, __ATOMIC_RELEASE);
        params->result = S_OK;
    }

    os_unfair_lock_unlock(&stream->lock);
    if (SUCCEEDED(params->result) &&
            (stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) &&
            !stream->timer_thread)
    {
        status = create_unix_thread(&stream->timer_thread, name,
                unix_timer_loop, stream);
        if (status)
        {
            __atomic_store_n(&stream->playing, FALSE, __ATOMIC_RELEASE);
            params->result = HRESULT_FROM_NT(status);
        }
    }
    if (SUCCEEDED(params->result) && stream->spatial)
        signal_spatial_event(stream);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_stop(void *args)
{
    struct stop_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    os_unfair_lock_lock(&stream->lock);

    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else if(!__atomic_load_n(&stream->playing, __ATOMIC_ACQUIRE))
        params->result = S_FALSE;
    else{
        __atomic_store_n(&stream->playing, FALSE, __ATOMIC_RELEASE);
        params->result = S_OK;
    }

    os_unfair_lock_unlock(&stream->lock);

    return STATUS_SUCCESS;
}

static void wait_for_spatial_callbacks(struct coreaudio_stream *stream)
{
    while (__atomic_load_n(&stream->spatial_callbacks_inflight,
            __ATOMIC_ACQUIRE))
        semaphore_wait(stream->callback_drain_semaphore);
}

static void wait_for_device_callbacks(struct coreaudio_stream *stream)
{
    LARGE_INTEGER delay;

    delay.QuadPart = -10000; /* One millisecond, off the real-time thread. */
    while (__atomic_load_n(&stream->device_callbacks_inflight,
            __ATOMIC_ACQUIRE))
        NtDelayExecution(FALSE, &delay);
}

static NTSTATUS unix_reset(void *args)
{
    struct reset_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    BOOL reset_spatial = FALSE;
    OSStatus sc;

    os_unfair_lock_lock(&stream->lock);

    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else if(__atomic_load_n(&stream->playing, __ATOMIC_ACQUIRE))
        params->result = AUDCLNT_E_NOT_STOPPED;
    else if(stream->getbuf_last)
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
    else{
        if (stream->spatial)
        {
            reset_spatial = TRUE;
            __atomic_store_n(&stream->spatial_resetting, TRUE,
                    __ATOMIC_RELEASE);
        }
        else
        {
            if(stream->flow == eRender)
                stream->written_frames = 0;
            else
                stream->written_frames += stream->held_frames;
            stream->held_frames = 0;
            stream->lcl_offs_frames = 0;
            stream->wri_offs_frames = 0;
            stream->cap_offs_frames = 0;
            stream->cap_held_frames = 0;
            if (stream->spatial_dry_delay_frames)
            {
                memset(stream->spatial_dry_delay_buffer, 0,
                        stream->spatial_dry_delay_frames *
                                sizeof(*stream->spatial_dry_delay_buffer));
                stream->spatial_dry_delay_pos = 0;
            }
        }
        params->result = S_OK;
    }

    os_unfair_lock_unlock(&stream->lock);

    if (reset_spatial)
    {
        UINT32 i;

        wait_for_spatial_callbacks(stream);
        os_unfair_lock_lock(&stream->lock);
        stream->written_frames = 0;
        stream->held_frames = 0;
        stream->lcl_offs_frames = 0;
        stream->wri_offs_frames = 0;
        stream->cap_offs_frames = 0;
        stream->cap_held_frames = 0;
        if (stream->spatial_dry_delay_frames)
        {
            memset(stream->spatial_dry_delay_buffer, 0,
                    stream->spatial_dry_delay_frames *
                            sizeof(*stream->spatial_dry_delay_buffer));
            stream->spatial_dry_delay_pos = 0;
        }
        __atomic_store_n(&stream->spatial_read_frames, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&stream->spatial_write_frames, 0, __ATOMIC_RELEASE);
        for (i = 0; i < stream->spatial_metadata_capacity; ++i)
            __atomic_store_n(&stream->spatial_metadata_sequences[i], UINT64_MAX,
                    __ATOMIC_RELEASE);
        os_unfair_lock_unlock(&stream->lock);

        sc = stream->spatial_unit ? AudioUnitReset(stream->spatial_unit,
                kAudioUnitScope_Global, 0) : noErr;
        __atomic_store_n(&stream->spatial_resetting, FALSE, __ATOMIC_RELEASE);
        if (sc != noErr)
        {
            WARN("Failed to reset spatial mixer state: %x.\n", (int)sc);
            params->result = osstatus_to_hresult(sc);
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;
    UINT32 pad;

    os_unfair_lock_lock(&stream->lock);

    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }

    pad = get_current_padding_nolock(stream);

    if(stream->getbuf_last){
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }
    if(!params->frames){
        params->result = S_OK;
        goto end;
    }
    if (pad > stream->bufsize_frames ||
            params->frames > stream->bufsize_frames - pad){
        if (stream->spatial)
            __atomic_add_fetch(&stream->overruns, 1, __ATOMIC_RELAXED);
        params->result = AUDCLNT_E_BUFFER_TOO_LARGE;
        goto end;
    }

    if(stream->wri_offs_frames + params->frames > stream->bufsize_frames){
        if(stream->tmp_buffer_frames < params->frames){
            if(stream->tmp_buffer){
                size = 0;
                NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                                    &size, MEM_RELEASE);
                stream->tmp_buffer = NULL;
            }
            size = params->frames * stream->fmt->nBlockAlign;
            if(NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                       &size, MEM_COMMIT, PAGE_READWRITE)){
                stream->tmp_buffer_frames = 0;
                params->result = E_OUTOFMEMORY;
                goto end;
            }
            stream->tmp_buffer_frames = params->frames;
        }
        *params->data = stream->tmp_buffer;
        stream->getbuf_last = -params->frames;
    }else{
        *params->data = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        stream->getbuf_last = params->frames;
    }

    silence_buffer(stream, *params->data, params->frames);
    params->result = S_OK;

end:
    os_unfair_lock_unlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    UINT64 spatial_write = 0, sequence;
    UINT32 record, slot;
    BYTE *buffer;

    os_unfair_lock_lock(&stream->lock);

    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
    {
        stream->getbuf_last = 0;
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    }
    else if(!params->written_frames){
        stream->getbuf_last = 0;
        params->result = S_OK;
    }else if(!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if(params->written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        params->result = AUDCLNT_E_INVALID_SIZE;
    else{
        if (stream->spatial)
        {
            spatial_write = __atomic_load_n(&stream->spatial_write_frames,
                    __ATOMIC_RELAXED);
            if (params->spatial_object_count !=
                    stream->spatial_dynamic_objects ||
                    (params->spatial_object_count && !params->spatial_objects) ||
                    spatial_write % stream->period_frames ||
                    params->written_frames != stream->period_frames)
            {
                params->result = E_INVALIDARG;
                goto done;
            }
            for (slot = 0; slot < params->spatial_object_count; ++slot)
                if (params->spatial_objects[slot].active_frames >
                            params->written_frames ||
                        !isfinite(params->spatial_objects[slot].x) ||
                        !isfinite(params->spatial_objects[slot].y) ||
                        !isfinite(params->spatial_objects[slot].z))
                {
                    params->result = E_INVALIDARG;
                    goto done;
                }
        }

        if(stream->getbuf_last >= 0)
            buffer = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        else
            buffer = stream->tmp_buffer;

        if(params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
            silence_buffer(stream, buffer, params->written_frames);

        if(stream->getbuf_last < 0)
            ca_wrap_buffer(stream->local_buffer,
                           stream->wri_offs_frames * stream->fmt->nBlockAlign,
                           stream->bufsize_frames * stream->fmt->nBlockAlign,
                           buffer, params->written_frames * stream->fmt->nBlockAlign);

        if (stream->spatial_dynamic_objects)
        {
            sequence = spatial_write / stream->period_frames;
            record = sequence % stream->spatial_metadata_capacity;
            for (slot = 0; slot < stream->spatial_dynamic_objects; ++slot)
                spatial_position_to_parameters(&params->spatial_objects[slot],
                        &stream->spatial_object_states[(size_t)record *
                                stream->spatial_dynamic_objects + slot]);
            __atomic_store_n(&stream->spatial_metadata_sequences[record],
                    sequence, __ATOMIC_RELEASE);
        }

        stream->wri_offs_frames += params->written_frames;
        stream->wri_offs_frames %= stream->bufsize_frames;
        if (!stream->spatial)
            stream->held_frames += params->written_frames;
        stream->written_frames += params->written_frames;
        stream->getbuf_last = 0;

        if (stream->spatial)
            __atomic_store_n(&stream->spatial_write_frames,
                    spatial_write + params->written_frames, __ATOMIC_RELEASE);

        params->result = S_OK;
    }

done:
    os_unfair_lock_unlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    UINT32 chunk_bytes, chunk_frames;
    LARGE_INTEGER stamp, freq;
    SIZE_T size;

    os_unfair_lock_lock(&stream->lock);

    if(stream->getbuf_last){
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }

    capture_resample(stream);

    *params->frames = 0;

    if(stream->held_frames < stream->period_frames){
        params->result = AUDCLNT_S_BUFFER_EMPTY;
        goto end;
    }

    *params->flags = 0;
    chunk_frames = stream->bufsize_frames - stream->lcl_offs_frames;
    if(chunk_frames < stream->period_frames){
        chunk_bytes = chunk_frames * stream->fmt->nBlockAlign;
        if(!stream->tmp_buffer){
            size = stream->period_frames * stream->fmt->nBlockAlign;
            NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                    &size, MEM_COMMIT, PAGE_READWRITE);
        }
        *params->data = stream->tmp_buffer;
        memcpy(*params->data, stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign,
               chunk_bytes);
        memcpy(*params->data + chunk_bytes, stream->local_buffer,
               stream->period_frames * stream->fmt->nBlockAlign - chunk_bytes);
    }else
        *params->data = stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign;

    stream->getbuf_last = *params->frames = stream->period_frames;

    if(params->devpos)
        *params->devpos = stream->written_frames;
    if(params->qpcpos){ /* fixme: qpc of recording time */
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }
    params->result = S_OK;

end:
    os_unfair_lock_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    os_unfair_lock_lock(&stream->lock);

    if(!params->done){
        stream->getbuf_last = 0;
        params->result = S_OK;
    }else if(!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if(stream->getbuf_last != params->done)
        params->result = AUDCLNT_E_INVALID_SIZE;
    else{
        stream->written_frames += params->done;
        stream->held_frames -= params->done;
        stream->lcl_offs_frames += params->done;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->getbuf_last = 0;
        params->result = S_OK;
    }

    os_unfair_lock_unlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    os_unfair_lock_lock(&stream->lock);

    capture_resample(stream);

    if(stream->held_frames >= stream->period_frames)
        *params->frames = stream->period_frames;
    else
        *params->frames = 0;

    os_unfair_lock_unlock(&stream->lock);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_position(void *args)
{
    struct get_position_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    LARGE_INTEGER stamp, freq;

    if (params->device) {
        FIXME("Device position reporting not implemented\n");
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    os_unfair_lock_lock(&stream->lock);

    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
    {
        os_unfair_lock_unlock(&stream->lock);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (stream->spatial)
        *params->pos = __atomic_load_n(&stream->spatial_read_frames,
                __ATOMIC_ACQUIRE);
    else
        *params->pos = stream->written_frames - stream->held_frames;

    if(stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->pos *= stream->fmt->nBlockAlign;

    if(params->qpctime){
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    os_unfair_lock_unlock(&stream->lock);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    if(stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_is_started(void *args)
{
    struct is_started_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else if(__atomic_load_n(&stream->playing, __ATOMIC_ACQUIRE))
        params->result = S_OK;
    else
        params->result = S_FALSE;

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_prop_value(void *args)
{
    struct get_prop_value_params *params = args;

    params->result = E_NOTIMPL;

    return STATUS_SUCCESS;
}

static NTSTATUS unix_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    AudioDeviceID dev_id;
    Float32 level = params->master_volume;
    OSStatus sc;
    UINT32 i;
    AudioObjectPropertyAddress prop_addr = {
        kAudioDevicePropertyVolumeScalar,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    os_unfair_lock_lock(&stream->lock);

    if (stream->spatial)
    {
        BOOL unity = params->master_volume == 1.0f;

        for (i = 0; i < stream->fmt->nChannels; ++i)
        {
            UINT32 bits;
            float volume = params->master_volume * params->session_volumes[i] *
                    params->volumes[i];

            memcpy(&bits, &volume, sizeof(bits));
            __atomic_store_n(&stream->spatial_volume_bits[i], bits,
                    __ATOMIC_RELAXED);
            if (volume != 1.0f)
                unity = FALSE;
        }
        __atomic_store_n(&stream->spatial_volumes_are_unity, unity,
                __ATOMIC_RELEASE);
        os_unfair_lock_unlock(&stream->lock);
        return STATUS_SUCCESS;
    }

    dev_id = get_stream_device(stream);

    sc = AudioObjectSetPropertyData(dev_id, &prop_addr, 0, NULL, sizeof(float), &level);
    if (sc == noErr)
        level = 1.0f;
    else
        WARN("Couldn't set master volume, applying it directly to the channels: %x\n", (int)sc);

    for (i = 1; i <= stream->fmt->nChannels; ++i) {
        const float vol = level * params->session_volumes[i - 1] * params->volumes[i - 1];

        prop_addr.mElement = i;

        sc = AudioObjectSetPropertyData(dev_id, &prop_addr, 0, NULL, sizeof(float), &vol);
        if (sc != noErr) {
            WARN("Couldn't set channel #%u volume: %x\n", i, (int)sc);
        }
    }

    os_unfair_lock_unlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    HRESULT hr = S_OK;

    os_unfair_lock_lock(&stream->lock);
    if (__atomic_load_n(&stream->invalidated, __ATOMIC_ACQUIRE))
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else if(!stream->unit)
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else if(!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        hr = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    else if(stream->event)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    else
        stream->event = params->event;
    os_unfair_lock_unlock(&stream->lock);

    params->result = hr;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
    unix_get_endpoint_ids,
    unix_create_stream,
    unix_release_stream,
    unix_start,
    unix_stop,
    unix_reset,
    unix_get_render_buffer,
    unix_release_render_buffer,
    unix_get_capture_buffer,
    unix_release_capture_buffer,
    unix_is_format_supported,
    unix_not_implemented,
    unix_get_mix_format,
    unix_get_device_period,
    unix_get_buffer_size,
    unix_get_latency,
    unix_get_current_padding,
    unix_get_next_packet_size,
    unix_get_frequency,
    unix_get_position,
    unix_set_volumes,
    unix_set_event_handle,
    unix_not_implemented,
    unix_not_implemented,
    unix_is_started,
    unix_get_prop_value,
    unix_not_implemented,
    unix_midi_init,
    unix_midi_release,
    unix_midi_out_message,
    unix_midi_in_message,
    unix_midi_notify_wait,
    unix_not_implemented,
    unix_get_spatial_audio_capabilities,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS unix_wow64_process_attach(void *args)
{
    SYSTEM_BASIC_INFORMATION info;

    NtQuerySystemInformation(SystemEmulationBasicInformation, &info, sizeof(info), NULL);
    zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    unix_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        DWORD flags;
        BOOL spatial;
        UINT32 spatial_static_mask;
        UINT32 spatial_dynamic_objects;
        UINT32 spatial_endpoint_generation;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .spatial = params32->spatial,
        .spatial_static_mask = params32->spatial_static_mask,
        .spatial_dynamic_objects = params32->spatial_dynamic_objects,
        .spatial_endpoint_generation =
                params32->spatial_endpoint_generation,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    unix_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
    };
    unix_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    unix_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_release_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 written_frames;
        UINT flags;
        PTR32 spatial_objects;
        UINT32 spatial_object_count;
        HRESULT result;
    } *params32 = args;
    struct release_render_buffer_params params =
    {
        .stream = params32->stream,
        .written_frames = params32->written_frames,
        .flags = params32->flags,
        .spatial_objects = ULongToPtr(params32->spatial_objects),
        .spatial_object_count = params32->spatial_object_count,
    };

    unix_release_render_buffer(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_spatial_audio_capabilities(void *args)
{
    struct
    {
        PTR32 device;
        HRESULT result;
        struct spatial_audio_capabilities capabilities;
    } *params32 = args;
    struct get_spatial_audio_capabilities_params params =
    {
        .device = ULongToPtr(params32->device),
    };

    unix_get_spatial_audio_capabilities(&params);
    params32->result = params.result;
    params32->capabilities = params.capabilities;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .data = &data,
        .frames = ULongToPtr(params32->frames),
        .flags = ULongToPtr(params32->flags),
        .devpos = ULongToPtr(params32->devpos),
        .qpcpos = ULongToPtr(params32->qpcpos)
    };
    unix_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
};

static NTSTATUS unix_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    unix_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt)
    };
    unix_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    unix_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    unix_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    unix_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    unix_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    unix_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    unix_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    unix_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return unix_set_volumes(&params);
}

static NTSTATUS unix_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };
    unix_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_prop_value(void *args)
{
    struct propvariant32
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        union
        {
            ULONG ulVal;
            PTR32 ptr;
            ULARGE_INTEGER uhVal;
        };
    } *value32;
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer; /* caller allocated buffer to hold value's strings */
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
        .buffer = ULongToPtr(params32->buffer),
        .buffer_size = ULongToPtr(params32->buffer_size)
    };
    unix_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = UlongToPtr(params32->value);
        value32->vt = value.vt;
        switch (value.vt)
        {
        case VT_UI4:
            value32->ulVal = value.ulVal;
            break;
        case VT_LPWSTR:
            value32->ptr = params32->buffer;
            break;
        default:
            FIXME("Unhandled vt %04x\n", value.vt);
        }
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    unix_wow64_process_attach,
    unix_not_implemented,
    unix_not_implemented,
    unix_not_implemented,
    unix_wow64_get_endpoint_ids,
    unix_wow64_create_stream,
    unix_wow64_release_stream,
    unix_start,
    unix_stop,
    unix_reset,
    unix_wow64_get_render_buffer,
    unix_wow64_release_render_buffer,
    unix_wow64_get_capture_buffer,
    unix_release_capture_buffer,
    unix_wow64_is_format_supported,
    unix_not_implemented,
    unix_wow64_get_mix_format,
    unix_wow64_get_device_period,
    unix_wow64_get_buffer_size,
    unix_wow64_get_latency,
    unix_wow64_get_current_padding,
    unix_wow64_get_next_packet_size,
    unix_wow64_get_frequency,
    unix_wow64_get_position,
    unix_wow64_set_volumes,
    unix_wow64_set_event_handle,
    unix_not_implemented,
    unix_not_implemented,
    unix_is_started,
    unix_wow64_get_prop_value,
    unix_not_implemented,
    unix_wow64_midi_init,
    unix_midi_release,
    unix_wow64_midi_out_message,
    unix_wow64_midi_in_message,
    unix_wow64_midi_notify_wait,
    unix_not_implemented,
    unix_wow64_get_spatial_audio_capabilities,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
