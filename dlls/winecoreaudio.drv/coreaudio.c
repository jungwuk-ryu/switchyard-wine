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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <fenv.h>
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

struct coreaudio_stream
{
    os_unfair_lock lock;
    os_unfair_lock spatial_unit_lock;
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

    EDataFlow flow;
    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    HANDLE timer_thread;

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
    float *spatial_volumes;
    float *spatial_bed_buffer;
    float *spatial_dry_buffer;
    float *spatial_dry_delay_buffer;
    UINT32 spatial_bed_channels;
    UINT32 spatial_dry_capacity;
    UINT32 spatial_dry_channel;
    UINT32 spatial_dry_delay_frames;
    UINT32 spatial_dry_delay_pos;
    UINT32 spatial_dry_output[2];
    UINT32 spatial_dry_output_count;
    BYTE *local_buffer, *cap_buffer, *wrap_buffer, *resamp_buffer, *tmp_buffer;
};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 50000;
static AudioDeviceID default_output_id = kAudioObjectUnknown;

static ULONG_PTR zero_bits = 0;

static NTSTATUS unix_get_mix_format(void *args);
static AudioDeviceID dev_id_from_device(const char *device);
static UINT ca_channel_layout_to_channel_mask(const AudioChannelLayout *layout);
static UINT32 count_channel_mask_bits(DWORD mask);

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

/* Spatial streams carry one private dry mono channel alongside their WAVE bed.
 * Pull and split both paths in a single callback so the spatial mixer and the
 * final dry injection consume exactly the same ring-buffer frames. */
static OSStatus ca_render_spatial_bed(struct coreaudio_stream *stream, UInt32 nframes,
        float *bed)
{
    UINT32 bed_channel, channel, frame, source_frame, to_copy_frames;

    if (nframes > stream->spatial_dry_capacity)
        return kAudio_ParamError;

    os_unfair_lock_lock(&stream->lock);

    to_copy_frames = stream->playing ? min(nframes, stream->held_frames) : 0;
    for (frame = 0; frame < to_copy_frames; ++frame)
    {
        const float *source;

        source_frame = stream->lcl_offs_frames + frame;
        if (source_frame >= stream->bufsize_frames)
            source_frame -= stream->bufsize_frames;
        source = (const float *)stream->local_buffer +
                source_frame * stream->fmt->nChannels;

        bed_channel = 0;
        for (channel = 0; channel < stream->fmt->nChannels; ++channel)
        {
            float sample = source[channel];

            if (!stream->spatial_volumes_are_unity)
                sample *= stream->spatial_volumes[channel];

            if (channel == stream->spatial_dry_channel)
                stream->spatial_dry_buffer[frame] = sample;
            else
                bed[frame * stream->spatial_bed_channels + bed_channel++] = sample;
        }
    }

    if (to_copy_frames)
    {
        stream->lcl_offs_frames += to_copy_frames;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->held_frames -= to_copy_frames;
    }

    if (nframes > to_copy_frames)
    {
        memset(bed + to_copy_frames * stream->spatial_bed_channels, 0,
                (nframes - to_copy_frames) * stream->spatial_bed_channels * sizeof(*bed));
        memset(stream->spatial_dry_buffer + to_copy_frames, 0,
                (nframes - to_copy_frames) * sizeof(*stream->spatial_dry_buffer));
    }

    /* The spatial mixer reports a fixed processing latency. Delay the private
     * non-spatial path by the same number of frames so dialogue remains sample
     * aligned with the spatial bed. Do this while holding the stream lock so
     * Reset cannot race the delay-line state. Native speaker output bypasses
     * the spatial mixer and therefore has no delay line. */
    if (stream->spatial_dry_delay_frames)
    {
        UINT32 pos = stream->spatial_dry_delay_pos;

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

    os_unfair_lock_unlock(&stream->lock);
    return noErr;
}

static OSStatus ca_spatial_bed_render_cb(void *user, AudioUnitRenderActionFlags *flags,
        const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes, AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    UInt32 bytes;

    (void)flags;
    (void)ts;
    (void)bus;

    if (nframes > stream->spatial_dry_capacity)
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

    return ca_render_spatial_bed(stream, nframes, data->mBuffers[0].mData);
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
    BOOL playing;
    unsigned int i;
    OSStatus sc;

    (void)bus;

    if (nframes > stream->spatial_dry_capacity)
        return kAudio_ParamError;

    os_unfair_lock_lock(&stream->lock);
    playing = stream->playing && !stream->spatial_resetting;
    os_unfair_lock_unlock(&stream->lock);

    if (!playing)
    {
        for (i = 0; i < data->mNumberBuffers; ++i)
            if (data->mBuffers[i].mData)
                memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
        return noErr;
    }

    os_unfair_lock_lock(&stream->spatial_unit_lock);
    os_unfair_lock_lock(&stream->lock);
    playing = stream->playing && !stream->spatial_resetting;
    os_unfair_lock_unlock(&stream->lock);
    if (!playing)
    {
        for (i = 0; i < data->mNumberBuffers; ++i)
            if (data->mBuffers[i].mData)
                memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
        sc = noErr;
        goto done;
    }

    if ((sc = AudioUnitRender(stream->spatial_unit, flags, ts, 0, nframes, data)) != noErr)
        goto done;

    if (data->mNumberBuffers < stream->dev_desc.mChannelsPerFrame)
    {
        sc = kAudio_ParamError;
        goto done;
    }
    for (i = 0; i < stream->dev_desc.mChannelsPerFrame; ++i)
        if (!data->mBuffers[i].mData || data->mBuffers[i].mNumberChannels != 1 ||
                data->mBuffers[i].mDataByteSize < nframes * sizeof(float))
        {
            sc = kAudio_ParamError;
            goto done;
        }

    mix_spatial_dry_planar(stream, nframes, data);
    sc = noErr;

done:
    os_unfair_lock_unlock(&stream->spatial_unit_lock);
    return sc;
}

static OSStatus ca_native_spatial_render_cb(void *user, AudioUnitRenderActionFlags *flags,
        const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes, AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    float *output;
    UINT32 channel, frame;
    UInt32 bytes;
    OSStatus sc;

    (void)flags;
    (void)ts;
    (void)bus;

    if (nframes > stream->spatial_dry_capacity)
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

    output = data->mBuffers[0].mData;
    if ((sc = ca_render_spatial_bed(stream, nframes, output)) != noErr)
        return sc;

    for (channel = 0; channel < stream->spatial_dry_output_count; ++channel)
    {
        UINT32 index = stream->spatial_dry_output[channel];

        for (frame = 0; frame < nframes; ++frame)
            output[frame * stream->spatial_bed_channels + index] +=
                    stream->spatial_dry_buffer[frame];
    }
    return noErr;
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
        UINT32 static_mask, WAVEFORMATEXTENSIBLE *bed, UINT32 *dry_channel)
{
    const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)transport;

    if (transport->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
            !IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ||
            transport->wBitsPerSample != 32 ||
            static_mask & ~SPATIAL_AUDIO_STATIC_OBJECT_MASK ||
            ext->dwChannelMask != spatial_static_mask_to_transport_mask(static_mask) ||
            count_channel_mask_bits(ext->dwChannelMask) != transport->nChannels)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    *bed = *ext;
    bed->dwChannelMask &= ~SPATIAL_AUDIO_DRY_SPEAKER;
    --bed->Format.nChannels;
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
    UInt32 max_frames, size = sizeof(max_frames);
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
    AudioStreamBasicDescription input_desc, output_desc;
    struct wave_channel_layout input_layout, output_layout;
    AURenderCallbackStruct callback;
    WAVEFORMATEXTENSIBLE mix;
    UInt32 algorithm, input_layout_size, output_layout_size;
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

    value = 1;
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
    TRACE("Using the spatial mixer for %u input and %u output channels (type %u, algorithm %u).\n",
            bed->Format.nChannels, mix.Format.nChannels, output_type, algorithm);
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

static NTSTATUS unix_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct coreaudio_stream *stream;
    WAVEFORMATEXTENSIBLE spatial_bed;
    AURenderCallbackStruct input;
    HRESULT hr;
    OSStatus sc;
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
    stream->period_frames = muldiv(params->period, stream->fmt->nSamplesPerSec, 10000000);

    if (stream->period_frames == 0)
    {
        params->result = E_INVALIDARG;
        goto end;
    }

    stream->dev_id = dev_id_from_device(params->device);
    stream->flow = params->flow;
    stream->flags = params->flags;
    stream->spatial = params->spatial;
    stream->spatial_static_mask = params->spatial_static_mask;
    stream->share = params->share;
    /* The DefaultOutput unit can migrate to a device with a different speaker
     * layout without giving us a safe point at which to rebuild the spatial
     * graph. Pin spatial streams to their activation endpoint rather than
     * rendering a stale speaker layout or HRTF mode on the new device. */
    stream->follows_default = !stream->spatial &&
            stream->flow == eRender && stream->dev_id == default_output_id;

    if (stream->spatial)
    {
        UINT32 i;

        if (stream->flow != eRender || stream->share != AUDCLNT_SHAREMODE_SHARED)
        {
            params->result = E_INVALIDARG;
            goto end;
        }
        if (FAILED(params->result = get_spatial_bed_format(stream->fmt,
                stream->spatial_static_mask,
                &spatial_bed, &stream->spatial_dry_channel)))
            goto end;
        stream->spatial_bed_channels = spatial_bed.Format.nChannels;

        if (!(stream->spatial_volumes = malloc(stream->fmt->nChannels *
                sizeof(*stream->spatial_volumes))))
        {
            params->result = E_OUTOFMEMORY;
            goto end;
        }
        for (i = 0; i < stream->fmt->nChannels; ++i)
            stream->spatial_volumes[i] = 1.0f;
        stream->spatial_volumes_are_unity = TRUE;
    }

    stream->bufsize_frames = muldiv(params->duration, stream->fmt->nSamplesPerSec, 10000000);
    if(params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;

    if(!(stream->unit = get_audiounit(stream->flow, stream->dev_id, stream->follows_default))){
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }

    if (stream->spatial &&
            (get_device_spatial_output_type(stream->dev_id) ==
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
                TRACE("Using lossless native output for a %u-channel spatial bed.\n",
                        spatial_bed.Format.nChannels);
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

    size = stream->bufsize_frames * stream->fmt->nBlockAlign;
    if(NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                               &size, MEM_COMMIT, PAGE_READWRITE)){
        params->result = E_OUTOFMEMORY;
        goto end;
    }
    silence_buffer(stream, stream->local_buffer, stream->bufsize_frames);

    if(stream->flow == eCapture){
        stream->cap_bufsize_frames = muldiv(params->duration, stream->dev_desc.mSampleRate, 10000000);
        stream->cap_buffer = malloc(stream->cap_bufsize_frames * stream->fmt->nBlockAlign);
    }
    params->result = S_OK;

end:
    if(FAILED(params->result)){
        if(stream->converter) AudioConverterDispose(stream->converter);
        if (stream->unit_started) AudioOutputUnitStop(stream->unit);
        if (stream->unit_initialized) AudioUnitUninitialize(stream->unit);
        if (stream->spatial_unit_initialized) AudioUnitUninitialize(stream->spatial_unit);
        if (stream->spatial_unit) AudioComponentInstanceDispose(stream->spatial_unit);
        if(stream->unit) AudioComponentInstanceDispose(stream->unit);
        free(stream->spatial_volumes);
        free(stream->spatial_bed_buffer);
        free(stream->spatial_dry_buffer);
        free(stream->spatial_dry_delay_buffer);
        free(stream->fmt);
        free(stream);
    } else {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;

    if(stream->timer_thread){
        stream->please_quit = TRUE;
        NtWaitForSingleObject(stream->timer_thread, FALSE, NULL);
        NtClose(stream->timer_thread);
    }

    if(stream->unit){
        if (stream->unit_started) AudioOutputUnitStop(stream->unit);
        if (stream->unit_initialized) AudioUnitUninitialize(stream->unit);
    }
    if (stream->spatial_unit)
    {
        if (stream->spatial_unit_initialized) AudioUnitUninitialize(stream->spatial_unit);
        AudioComponentInstanceDispose(stream->spatial_unit);
    }
    if (stream->unit)
        AudioComponentInstanceDispose(stream->unit);

    if(stream->converter) AudioConverterDispose(stream->converter);
    free(stream->spatial_volumes);
    free(stream->spatial_bed_buffer);
    free(stream->spatial_dry_buffer);
    free(stream->spatial_dry_delay_buffer);
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

    if (params->def_period)
        *params->def_period = def_period;
    if (params->min_period)
        *params->min_period = min_period;

    params->result = S_OK;

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
    UInt32 latency, stream_latency, size;
    AudioObjectPropertyAddress addr;
    OSStatus sc;

    os_unfair_lock_lock(&stream->lock);

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

    latency += stream_latency;
    /* pretend we process audio in Period chunks, so max latency includes
     * the period time */
    *params->latency = muldiv(latency, 10000000, stream->fmt->nSamplesPerSec) + stream->period;
    if (stream->spatial_dry_delay_frames)
        *params->latency += muldiv(stream->spatial_dry_delay_frames, 10000000,
                stream->fmt->nSamplesPerSec);

    os_unfair_lock_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT32 get_current_padding_nolock(struct coreaudio_stream *stream)
{
    if(stream->flow == eCapture) capture_resample(stream);
    return stream->held_frames;
}

static NTSTATUS unix_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    os_unfair_lock_lock(&stream->lock);
    *params->padding = get_current_padding_nolock(stream);
    os_unfair_lock_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static void unix_timer_loop(void *args)
{
    struct coreaudio_stream *stream = args;
    LARGE_INTEGER delay, next, last;
    int adjust;

    delay.QuadPart = -stream->period;
    NtQueryPerformanceCounter(&last, NULL);
    next.QuadPart = last.QuadPart + stream->period;

    while(!stream->please_quit){
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

    os_unfair_lock_lock(&stream->lock);

    if((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        params->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
    else if(stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else{
        stream->playing = TRUE;
        params->result = S_OK;
    }

    os_unfair_lock_unlock(&stream->lock);
    if (!stream->timer_thread) create_unix_thread( &stream->timer_thread, name, unix_timer_loop, stream );

    return STATUS_SUCCESS;
}

static NTSTATUS unix_stop(void *args)
{
    struct stop_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);

    os_unfair_lock_lock(&stream->lock);

    if(!stream->playing)
        params->result = S_FALSE;
    else{
        stream->playing = FALSE;
        params->result = S_OK;
    }

    os_unfair_lock_unlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_reset(void *args)
{
    struct reset_params *params = args;
    struct coreaudio_stream *stream = handle_get_stream(params->stream);
    BOOL reset_spatial = FALSE;
    OSStatus sc;

    os_unfair_lock_lock(&stream->lock);

    if(stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else if(stream->getbuf_last)
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
    else{
        if (stream->spatial_unit)
            reset_spatial = stream->spatial_resetting = TRUE;
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

    /* Do not hold the stream lock while resetting the audio unit: an in-flight
     * render can still be finishing its input callback and taking that lock.
     * New callbacks remain silent until the reset has completed. */
    if (reset_spatial)
    {
        os_unfair_lock_lock(&stream->spatial_unit_lock);
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
        os_unfair_lock_unlock(&stream->lock);

        sc = AudioUnitReset(stream->spatial_unit, kAudioUnitScope_Global, 0);
        os_unfair_lock_lock(&stream->lock);
        stream->spatial_resetting = FALSE;
        os_unfair_lock_unlock(&stream->lock);
        os_unfair_lock_unlock(&stream->spatial_unit_lock);
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

    pad = get_current_padding_nolock(stream);

    if(stream->getbuf_last){
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }
    if(!params->frames){
        params->result = S_OK;
        goto end;
    }
    if(pad + params->frames > stream->bufsize_frames){
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
    BYTE *buffer;

    os_unfair_lock_lock(&stream->lock);

    if(!params->written_frames){
        stream->getbuf_last = 0;
        params->result = S_OK;
    }else if(!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if(params->written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        params->result = AUDCLNT_E_INVALID_SIZE;
    else{
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

        stream->wri_offs_frames += params->written_frames;
        stream->wri_offs_frames %= stream->bufsize_frames;
        stream->held_frames += params->written_frames;
        stream->written_frames += params->written_frames;
        stream->getbuf_last = 0;

        params->result = S_OK;
    }

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

    if(stream->playing)
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
        stream->spatial_volumes_are_unity = params->master_volume == 1.0f;
        for (i = 0; i < stream->fmt->nChannels; ++i)
        {
            stream->spatial_volumes[i] = params->master_volume *
                    params->session_volumes[i] * params->volumes[i];
            if (stream->spatial_volumes[i] != 1.0f)
                stream->spatial_volumes_are_unity = FALSE;
        }
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
    if(!stream->unit)
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
    unix_release_render_buffer,
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
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
