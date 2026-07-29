/*
 *  ITfContext implementation
 *
 *  Copyright 2009 Aric Stewart, CodeWeavers
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

#include <stdarg.h>
#include <limits.h>

#define COBJMACROS

#include "wine/debug.h"
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winuser.h"
#include "shlwapi.h"
#include "winerror.h"
#include "objbase.h"
#include "olectl.h"

#include "msctf.h"
#include "msctf_internal.h"

WINE_DEFAULT_DEBUG_CHANNEL(msctf);

typedef struct tagEditRecord EditRecord;

typedef struct tagContext {
    ITfContext ITfContext_iface;
    ITfSource ITfSource_iface;
    /* const ITfContextCompositionVtbl *ContextCompositionVtbl; */
    ITfContextOwnerCompositionServices ITfContextOwnerCompositionServices_iface;
    /* const ITfContextOwnerServicesVtbl *ContextOwnerServicesVtbl; */
    ITfInsertAtSelection ITfInsertAtSelection_iface;
    /* const ITfMouseTrackerVtbl *MouseTrackerVtbl; */
    /* const ITfQueryEmbeddedVtbl *QueryEmbeddedVtbl; */
    ITfSourceSingle ITfSourceSingle_iface;
    ITextStoreACPSink ITextStoreACPSink_iface;
    ITextStoreACPServices ITextStoreACPServices_iface;
    LONG refCount;
    BOOL connected;

    /* Aggregation */
    ITfCompartmentMgr  *CompartmentMgr;

    TfClientId tidOwner;
    TfEditCookie defaultCookie;
    TS_STATUS documentStatus;
    ITfDocumentMgr *manager;

    ITextStoreACP   *pITextStoreACP;
    ITfContextOwnerCompositionSink *pITfContextOwnerCompositionSink;

    ITfEditSession* currentEditSession;
    TfClientId currentEditSessionTid;
    DWORD currentLockType;
    unsigned int editSessionDepth;
    unsigned int notifyingTextEditSinks;
    BOOL compositionOperation;
    EditRecord *activeEditRecord;

    /* kept as separate lists to reduce unnecessary iterations */
    struct list     pContextKeyEventSink;
    struct list     pEditTransactionSink;
    struct list     pStatusSink;
    struct list     pTextEditSink;
    struct list     pTextLayoutSink;
    struct list     editSessionRequests;
    struct list     properties;
    struct list     compositions;

} Context;

typedef struct tagEditCookie {
    DWORD lockType;
    Context *pOwningContext;
} EditCookie;

typedef struct tagEditSessionRequest
{
    struct list entry;
    ITfEditSession *session;
    TfClientId tid;
    DWORD flags;
    BOOL lock_requested;
    BOOL granted;
    BOOL in_request_call;
    BOOL completed;
} EditSessionRequest;

typedef struct tagTerminateCompositionSession
{
    ITfEditSession ITfEditSession_iface;
    LONG ref;
    Context *context;
    ITfCompositionView *target;
} TerminateCompositionSession;

typedef struct tagRangeEnumerator
{
    IEnumTfRanges IEnumTfRanges_iface;
    LONG ref;
    ULONG index, count;
    ITfRange **ranges;
} RangeEnumerator;

typedef struct tagComposition Composition;

typedef struct tagCompositionEnumerator
{
    IEnumITfCompositionView IEnumITfCompositionView_iface;
    LONG ref;
    ULONG index, count;
    ITfCompositionView **views;
} CompositionEnumerator;

typedef struct tagPropertyValueEnumerator
{
    IEnumTfPropertyValue IEnumTfPropertyValue_iface;
    LONG ref;
    ULONG index, count;
    TF_PROPERTYVAL *values;
} PropertyValueEnumerator;

typedef struct tagPropertyRun
{
    struct list entry;
    LONG start, end;
    VARIANT value;
} PropertyRun;

typedef struct tagContextProperty
{
    ITfProperty ITfProperty_iface;
    struct list entry;
    Context *context;
    GUID guid;
    struct list runs;
} ContextProperty;

typedef struct tagTrackingProperty
{
    ITfReadOnlyProperty ITfReadOnlyProperty_iface;
    LONG ref;
    Context *context;
    ULONG count;
    GUID guids[1];
} TrackingProperty;

typedef struct tagPropertyChange
{
    struct list entry;
    GUID guid;
    LONG start, end;
} PropertyChange;

struct tagEditRecord
{
    ITfEditRecord ITfEditRecord_iface;
    LONG ref;
    Context *context;
    BOOL selection_changed;
    BOOL text_changed;
    LONG text_start, text_end;
    struct list property_changes;
};

struct tagComposition
{
    ITfComposition ITfComposition_iface;
    ITfCompositionView ITfCompositionView_iface;
    LONG ref;
    struct list entry;
    Context *context;
    ITfRange *range;
    ITfCompositionSink *sink;
    CLSID owner;
    BOOL active;
};

static ULONG WINAPI Context_AddRef(ITfContext *iface);
static ULONG WINAPI Context_Release(ITfContext *iface);
static HRESULT context_set_property_value(Context *context, REFGUID guid, ITfRange *range,
        const VARIANT *value);
static HRESULT context_clear_property(Context *context, REFGUID guid, ITfRange *range);
static HRESULT WINAPI ContextProperty_SetValue(ITfProperty *iface, TfEditCookie cookie,
        ITfRange *range, const VARIANT *value);
static void composition_adjust_for_text_change(Composition *composition, const TS_TEXTCHANGE *change);
static HRESULT composition_finish(Composition *composition, TfEditCookie cookie, BOOL terminated);
static void context_kick_deferred_requests(Context *context);

static inline Context *impl_from_ITfContext(ITfContext *iface)
{
    return CONTAINING_RECORD(iface, Context, ITfContext_iface);
}

static inline Context *impl_from_ITfSource(ITfSource *iface)
{
    return CONTAINING_RECORD(iface, Context, ITfSource_iface);
}

static inline Context *impl_from_ITfContextOwnerCompositionServices(ITfContextOwnerCompositionServices *iface)
{
    return CONTAINING_RECORD(iface, Context, ITfContextOwnerCompositionServices_iface);
}

static inline Context *impl_from_ITfInsertAtSelection(ITfInsertAtSelection *iface)
{
    return CONTAINING_RECORD(iface, Context, ITfInsertAtSelection_iface);
}

static inline Context *impl_from_ITfSourceSingle(ITfSourceSingle* iface)
{
    return CONTAINING_RECORD(iface, Context, ITfSourceSingle_iface);
}

static inline Context *impl_from_ITextStoreACPSink(ITextStoreACPSink *iface)
{
    return CONTAINING_RECORD(iface, Context, ITextStoreACPSink_iface);
}

static inline Context *impl_from_ITextStoreACPServices(ITextStoreACPServices *iface)
{
    return CONTAINING_RECORD(iface, Context, ITextStoreACPServices_iface);
}

static inline RangeEnumerator *impl_from_IEnumTfRanges(IEnumTfRanges *iface)
{
    return CONTAINING_RECORD(iface, RangeEnumerator, IEnumTfRanges_iface);
}

static inline CompositionEnumerator *impl_from_IEnumITfCompositionView(IEnumITfCompositionView *iface)
{
    return CONTAINING_RECORD(iface, CompositionEnumerator, IEnumITfCompositionView_iface);
}

static inline PropertyValueEnumerator *impl_from_IEnumTfPropertyValue(IEnumTfPropertyValue *iface)
{
    return CONTAINING_RECORD(iface, PropertyValueEnumerator, IEnumTfPropertyValue_iface);
}

static inline ContextProperty *impl_from_ITfProperty(ITfProperty *iface)
{
    return CONTAINING_RECORD(iface, ContextProperty, ITfProperty_iface);
}

static inline TrackingProperty *impl_from_ITfReadOnlyProperty(ITfReadOnlyProperty *iface)
{
    return CONTAINING_RECORD(iface, TrackingProperty, ITfReadOnlyProperty_iface);
}

static inline EditRecord *impl_from_ITfEditRecord(ITfEditRecord *iface)
{
    return CONTAINING_RECORD(iface, EditRecord, ITfEditRecord_iface);
}

static inline TerminateCompositionSession *impl_from_terminate_ITfEditSession(
        ITfEditSession *iface)
{
    return CONTAINING_RECORD(iface, TerminateCompositionSession, ITfEditSession_iface);
}

static inline Composition *impl_from_ITfComposition(ITfComposition *iface)
{
    return CONTAINING_RECORD(iface, Composition, ITfComposition_iface);
}

static inline Composition *impl_from_ITfCompositionView(ITfCompositionView *iface)
{
    return CONTAINING_RECORD(iface, Composition, ITfCompositionView_iface);
}

static HRESULT WINAPI RangeEnumerator_QueryInterface(IEnumTfRanges *iface, REFIID iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IEnumTfRanges))
        *out = iface;
    if (!*out) return E_NOINTERFACE;
    IEnumTfRanges_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI RangeEnumerator_AddRef(IEnumTfRanges *iface)
{
    RangeEnumerator *enumerator = impl_from_IEnumTfRanges(iface);
    return InterlockedIncrement(&enumerator->ref);
}

static ULONG WINAPI RangeEnumerator_Release(IEnumTfRanges *iface)
{
    RangeEnumerator *enumerator = impl_from_IEnumTfRanges(iface);
    ULONG ref = InterlockedDecrement(&enumerator->ref);
    ULONG i;

    if (!ref)
    {
        for (i = 0; i < enumerator->count; ++i)
            ITfRange_Release(enumerator->ranges[i]);
        free(enumerator->ranges);
        free(enumerator);
    }
    return ref;
}

static HRESULT WINAPI RangeEnumerator_Clone(IEnumTfRanges *iface, IEnumTfRanges **out);

static HRESULT WINAPI RangeEnumerator_Next(IEnumTfRanges *iface, ULONG count, ITfRange **ranges,
        ULONG *fetched)
{
    RangeEnumerator *enumerator = impl_from_IEnumTfRanges(iface);
    ULONG copied = 0;

    if (!ranges || (!fetched && count != 1))
        return E_INVALIDARG;
    while (copied < count && enumerator->index < enumerator->count)
    {
        ranges[copied] = enumerator->ranges[enumerator->index++];
        ITfRange_AddRef(ranges[copied++]);
    }
    if (fetched) *fetched = copied;
    return copied == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI RangeEnumerator_Reset(IEnumTfRanges *iface)
{
    RangeEnumerator *enumerator = impl_from_IEnumTfRanges(iface);
    enumerator->index = 0;
    return S_OK;
}

static HRESULT WINAPI RangeEnumerator_Skip(IEnumTfRanges *iface, ULONG count)
{
    RangeEnumerator *enumerator = impl_from_IEnumTfRanges(iface);
    ULONG remaining = enumerator->count - enumerator->index;

    if (count > remaining)
    {
        enumerator->index = enumerator->count;
        return S_FALSE;
    }
    enumerator->index += count;
    return S_OK;
}

static const IEnumTfRangesVtbl RangeEnumeratorVtbl =
{
    RangeEnumerator_QueryInterface,
    RangeEnumerator_AddRef,
    RangeEnumerator_Release,
    RangeEnumerator_Clone,
    RangeEnumerator_Next,
    RangeEnumerator_Reset,
    RangeEnumerator_Skip
};

static HRESULT range_enumerator_create(ITfRange **ranges, ULONG count, ULONG index,
        IEnumTfRanges **out)
{
    RangeEnumerator *enumerator;
    ULONG i;

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!(enumerator = calloc(1, sizeof(*enumerator))))
        return E_OUTOFMEMORY;
    enumerator->IEnumTfRanges_iface.lpVtbl = &RangeEnumeratorVtbl;
    enumerator->ref = 1;
    enumerator->index = index;
    enumerator->count = count;
    if (count)
    {
        if (!(enumerator->ranges = calloc(count, sizeof(*enumerator->ranges))))
        {
            free(enumerator);
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < count; ++i)
        {
            enumerator->ranges[i] = ranges[i];
            ITfRange_AddRef(ranges[i]);
        }
    }
    *out = &enumerator->IEnumTfRanges_iface;
    return S_OK;
}

static HRESULT WINAPI RangeEnumerator_Clone(IEnumTfRanges *iface, IEnumTfRanges **out)
{
    RangeEnumerator *enumerator = impl_from_IEnumTfRanges(iface);
    return range_enumerator_create(enumerator->ranges, enumerator->count, enumerator->index, out);
}

static HRESULT WINAPI CompositionEnumerator_QueryInterface(IEnumITfCompositionView *iface,
        REFIID iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IEnumITfCompositionView))
        *out = iface;
    if (!*out) return E_NOINTERFACE;
    IEnumITfCompositionView_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI CompositionEnumerator_AddRef(IEnumITfCompositionView *iface)
{
    CompositionEnumerator *enumerator = impl_from_IEnumITfCompositionView(iface);
    return InterlockedIncrement(&enumerator->ref);
}

static ULONG WINAPI CompositionEnumerator_Release(IEnumITfCompositionView *iface)
{
    CompositionEnumerator *enumerator = impl_from_IEnumITfCompositionView(iface);
    ULONG ref = InterlockedDecrement(&enumerator->ref);
    ULONG i;

    if (!ref)
    {
        for (i = 0; i < enumerator->count; ++i)
            ITfCompositionView_Release(enumerator->views[i]);
        free(enumerator->views);
        free(enumerator);
    }
    return ref;
}

static HRESULT composition_enumerator_create(ITfCompositionView **views, ULONG count,
        ULONG index, IEnumITfCompositionView **out);

static HRESULT WINAPI CompositionEnumerator_Clone(IEnumITfCompositionView *iface,
        IEnumITfCompositionView **out)
{
    CompositionEnumerator *enumerator = impl_from_IEnumITfCompositionView(iface);
    return composition_enumerator_create(enumerator->views, enumerator->count,
            enumerator->index, out);
}

static HRESULT WINAPI CompositionEnumerator_Next(IEnumITfCompositionView *iface, ULONG count,
        ITfCompositionView **views, ULONG *fetched)
{
    CompositionEnumerator *enumerator = impl_from_IEnumITfCompositionView(iface);
    ULONG copied = 0;

    if (!views || (!fetched && count != 1))
        return E_INVALIDARG;
    while (copied < count && enumerator->index < enumerator->count)
    {
        views[copied] = enumerator->views[enumerator->index++];
        ITfCompositionView_AddRef(views[copied++]);
    }
    if (fetched) *fetched = copied;
    return copied == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI CompositionEnumerator_Reset(IEnumITfCompositionView *iface)
{
    CompositionEnumerator *enumerator = impl_from_IEnumITfCompositionView(iface);
    enumerator->index = 0;
    return S_OK;
}

static HRESULT WINAPI CompositionEnumerator_Skip(IEnumITfCompositionView *iface, ULONG count)
{
    CompositionEnumerator *enumerator = impl_from_IEnumITfCompositionView(iface);
    ULONG remaining = enumerator->count - enumerator->index;

    if (count > remaining)
    {
        enumerator->index = enumerator->count;
        return S_FALSE;
    }
    enumerator->index += count;
    return S_OK;
}

static const IEnumITfCompositionViewVtbl CompositionEnumeratorVtbl =
{
    CompositionEnumerator_QueryInterface,
    CompositionEnumerator_AddRef,
    CompositionEnumerator_Release,
    CompositionEnumerator_Clone,
    CompositionEnumerator_Next,
    CompositionEnumerator_Reset,
    CompositionEnumerator_Skip
};

static HRESULT composition_enumerator_create(ITfCompositionView **views, ULONG count,
        ULONG index, IEnumITfCompositionView **out)
{
    CompositionEnumerator *enumerator;
    ULONG i;

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!(enumerator = calloc(1, sizeof(*enumerator))))
        return E_OUTOFMEMORY;
    enumerator->IEnumITfCompositionView_iface.lpVtbl = &CompositionEnumeratorVtbl;
    enumerator->ref = 1;
    enumerator->index = index;
    enumerator->count = count;
    if (count)
    {
        if (!(enumerator->views = calloc(count, sizeof(*enumerator->views))))
        {
            free(enumerator);
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < count; ++i)
        {
            enumerator->views[i] = views[i];
            ITfCompositionView_AddRef(views[i]);
        }
    }
    *out = &enumerator->IEnumITfCompositionView_iface;
    return S_OK;
}

static HRESULT WINAPI PropertyValueEnumerator_QueryInterface(IEnumTfPropertyValue *iface,
        REFIID iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IEnumTfPropertyValue))
        *out = iface;
    if (!*out) return E_NOINTERFACE;
    IEnumTfPropertyValue_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI PropertyValueEnumerator_AddRef(IEnumTfPropertyValue *iface)
{
    PropertyValueEnumerator *enumerator = impl_from_IEnumTfPropertyValue(iface);
    return InterlockedIncrement(&enumerator->ref);
}

static ULONG WINAPI PropertyValueEnumerator_Release(IEnumTfPropertyValue *iface)
{
    PropertyValueEnumerator *enumerator = impl_from_IEnumTfPropertyValue(iface);
    ULONG ref = InterlockedDecrement(&enumerator->ref);
    ULONG i;

    if (!ref)
    {
        for (i = 0; i < enumerator->count; ++i)
            VariantClear(&enumerator->values[i].varValue);
        free(enumerator->values);
        free(enumerator);
    }
    return ref;
}

static HRESULT property_value_enumerator_create(const TF_PROPERTYVAL *values, ULONG count,
        ULONG index, IEnumTfPropertyValue **out);

static HRESULT WINAPI PropertyValueEnumerator_Clone(IEnumTfPropertyValue *iface,
        IEnumTfPropertyValue **out)
{
    PropertyValueEnumerator *enumerator = impl_from_IEnumTfPropertyValue(iface);
    return property_value_enumerator_create(enumerator->values, enumerator->count,
            enumerator->index, out);
}

static HRESULT WINAPI PropertyValueEnumerator_Next(IEnumTfPropertyValue *iface, ULONG count,
        TF_PROPERTYVAL *values, ULONG *fetched)
{
    PropertyValueEnumerator *enumerator = impl_from_IEnumTfPropertyValue(iface);
    ULONG copied = 0;
    HRESULT hr = S_OK;

    if (!values || (!fetched && count != 1))
        return E_INVALIDARG;
    while (copied < count && enumerator->index < enumerator->count)
    {
        values[copied].guidId = enumerator->values[enumerator->index].guidId;
        VariantInit(&values[copied].varValue);
        hr = VariantCopy(&values[copied].varValue,
                &enumerator->values[enumerator->index].varValue);
        if (FAILED(hr))
            break;
        ++copied;
        ++enumerator->index;
    }
    if (fetched) *fetched = copied;
    if (FAILED(hr)) return hr;
    return copied == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI PropertyValueEnumerator_Reset(IEnumTfPropertyValue *iface)
{
    PropertyValueEnumerator *enumerator = impl_from_IEnumTfPropertyValue(iface);
    enumerator->index = 0;
    return S_OK;
}

static HRESULT WINAPI PropertyValueEnumerator_Skip(IEnumTfPropertyValue *iface, ULONG count)
{
    PropertyValueEnumerator *enumerator = impl_from_IEnumTfPropertyValue(iface);
    ULONG remaining = enumerator->count - enumerator->index;

    if (count > remaining)
    {
        enumerator->index = enumerator->count;
        return S_FALSE;
    }
    enumerator->index += count;
    return S_OK;
}

static const IEnumTfPropertyValueVtbl PropertyValueEnumeratorVtbl =
{
    PropertyValueEnumerator_QueryInterface,
    PropertyValueEnumerator_AddRef,
    PropertyValueEnumerator_Release,
    PropertyValueEnumerator_Clone,
    PropertyValueEnumerator_Next,
    PropertyValueEnumerator_Reset,
    PropertyValueEnumerator_Skip
};

static HRESULT property_value_enumerator_create(const TF_PROPERTYVAL *values, ULONG count,
        ULONG index, IEnumTfPropertyValue **out)
{
    PropertyValueEnumerator *enumerator;
    ULONG i;
    HRESULT hr;

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!(enumerator = calloc(1, sizeof(*enumerator))))
        return E_OUTOFMEMORY;
    enumerator->IEnumTfPropertyValue_iface.lpVtbl = &PropertyValueEnumeratorVtbl;
    enumerator->ref = 1;
    enumerator->index = index;
    enumerator->count = count;
    if (count)
    {
        if (!(enumerator->values = calloc(count, sizeof(*enumerator->values))))
        {
            free(enumerator);
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < count; ++i)
        {
            enumerator->values[i].guidId = values[i].guidId;
            VariantInit(&enumerator->values[i].varValue);
            if (FAILED(hr = VariantCopy(&enumerator->values[i].varValue,
                    (VARIANT *)&values[i].varValue)))
            {
                while (i) VariantClear(&enumerator->values[--i].varValue);
                free(enumerator->values);
                free(enumerator);
                return hr;
            }
        }
    }
    *out = &enumerator->IEnumTfPropertyValue_iface;
    return S_OK;
}

static HRESULT WINAPI EditRecord_QueryInterface(ITfEditRecord *iface, REFIID iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITfEditRecord))
        *out = iface;
    if (!*out) return E_NOINTERFACE;
    ITfEditRecord_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI EditRecord_AddRef(ITfEditRecord *iface)
{
    EditRecord *record = impl_from_ITfEditRecord(iface);
    return InterlockedIncrement(&record->ref);
}

static ULONG WINAPI EditRecord_Release(ITfEditRecord *iface)
{
    EditRecord *record = impl_from_ITfEditRecord(iface);
    ULONG ref = InterlockedDecrement(&record->ref);

    if (!ref)
    {
        PropertyChange *change, *next;
        LIST_FOR_EACH_ENTRY_SAFE(change, next, &record->property_changes, PropertyChange, entry)
        {
            list_remove(&change->entry);
            free(change);
        }
        ITfContext_Release(&record->context->ITfContext_iface);
        free(record);
    }
    return ref;
}

static HRESULT WINAPI EditRecord_GetSelectionStatus(ITfEditRecord *iface, BOOL *changed)
{
    EditRecord *record = impl_from_ITfEditRecord(iface);
    if (!changed) return E_INVALIDARG;
    *changed = record->selection_changed;
    return S_OK;
}

static BOOL guid_is_requested(REFGUID guid, const GUID **guids, ULONG count)
{
    ULONG i;
    for (i = 0; i < count; ++i)
        if (guids[i] && IsEqualGUID(guid, guids[i]))
            return TRUE;
    return FALSE;
}

static HRESULT WINAPI EditRecord_GetTextAndPropertyUpdates(ITfEditRecord *iface, DWORD flags,
        const GUID **properties, ULONG count, IEnumTfRanges **out)
{
    EditRecord *record = impl_from_ITfEditRecord(iface);
    PropertyChange *change;
    ITfRange **ranges;
    ULONG range_count = 0, capacity = 1;
    HRESULT hr;

    if (!out || (flags & ~TF_GTP_INCL_TEXT) || (!(flags & TF_GTP_INCL_TEXT) &&
        (!properties || !count)) || (count && !properties))
        return E_INVALIDARG;
    *out = NULL;

    LIST_FOR_EACH_ENTRY(change, &record->property_changes, PropertyChange, entry)
        if (guid_is_requested(&change->guid, properties, count))
            ++capacity;
    if (!(ranges = calloc(capacity, sizeof(*ranges))))
        return E_OUTOFMEMORY;

    if ((flags & TF_GTP_INCL_TEXT) && record->text_changed)
    {
        hr = Range_Constructor(&record->context->ITfContext_iface, record->text_start,
                record->text_end, &ranges[range_count]);
        if (FAILED(hr)) goto failed;
        ++range_count;
    }
    LIST_FOR_EACH_ENTRY(change, &record->property_changes, PropertyChange, entry)
    {
        if (!guid_is_requested(&change->guid, properties, count))
            continue;
        hr = Range_Constructor(&record->context->ITfContext_iface, change->start,
                change->end, &ranges[range_count]);
        if (FAILED(hr)) goto failed;
        ++range_count;
    }

    hr = range_enumerator_create(ranges, range_count, 0, out);
failed:
    while (range_count) ITfRange_Release(ranges[--range_count]);
    free(ranges);
    return hr;
}

static const ITfEditRecordVtbl EditRecordVtbl =
{
    EditRecord_QueryInterface,
    EditRecord_AddRef,
    EditRecord_Release,
    EditRecord_GetSelectionStatus,
    EditRecord_GetTextAndPropertyUpdates
};

static EditRecord *edit_record_create(Context *context)
{
    EditRecord *record;

    if (!(record = calloc(1, sizeof(*record))))
        return NULL;
    record->ITfEditRecord_iface.lpVtbl = &EditRecordVtbl;
    record->ref = 1;
    record->context = context;
    ITfContext_AddRef(&context->ITfContext_iface);
    list_init(&record->property_changes);
    return record;
}

static void edit_record_add_text_change(EditRecord *record, const TS_TEXTCHANGE *change)
{
    LONG end = max(change->acpStart, change->acpNewEnd);

    if (!record->text_changed)
    {
        record->text_changed = TRUE;
        record->text_start = change->acpStart;
        record->text_end = end;
    }
    else
    {
        record->text_start = min(record->text_start, change->acpStart);
        record->text_end = max(record->text_end, end);
    }
}

static void edit_record_add_property_change(EditRecord *record, REFGUID guid, LONG start, LONG end)
{
    PropertyChange *change;

    LIST_FOR_EACH_ENTRY(change, &record->property_changes, PropertyChange, entry)
    {
        if (IsEqualGUID(&change->guid, guid))
        {
            change->start = min(change->start, start);
            change->end = max(change->end, end);
            return;
        }
    }
    if (!(change = malloc(sizeof(*change))))
        return;
    change->guid = *guid;
    change->start = start;
    change->end = end;
    list_add_tail(&record->property_changes, &change->entry);
}

static ContextProperty *context_find_property(Context *context, REFGUID guid)
{
    ContextProperty *property;

    LIST_FOR_EACH_ENTRY(property, &context->properties, ContextProperty, entry)
        if (IsEqualGUID(&property->guid, guid))
            return property;
    return NULL;
}

static HRESULT get_owned_range_extent(Context *context, ITfRange *range, LONG *start, LONG *end)
{
    ITfRangeACP *range_acp;
    ITfContext *range_context;
    LONG count;
    HRESULT hr;

    if (!range || !start || !end)
        return E_INVALIDARG;
    if (FAILED(hr = ITfRange_GetContext(range, &range_context)))
        return hr;
    if (range_context != &context->ITfContext_iface)
    {
        ITfContext_Release(range_context);
        return TF_E_NOTOWNEDRANGE;
    }
    ITfContext_Release(range_context);
    if (FAILED(hr = ITfRange_QueryInterface(range, &IID_ITfRangeACP, (void **)&range_acp)))
        return hr;
    hr = ITfRangeACP_GetExtent(range_acp, start, &count);
    ITfRangeACP_Release(range_acp);
    if (FAILED(hr))
        return hr;
    if (count < 0 || *start < 0 || *start > LONG_MAX - count)
        return E_INVALIDARG;
    *end = *start + count;
    return S_OK;
}

static void property_run_free(PropertyRun *run)
{
    VariantClear(&run->value);
    free(run);
}

static PropertyRun *property_run_create(LONG start, LONG end, const VARIANT *value)
{
    PropertyRun *run;

    if (!(run = calloc(1, sizeof(*run))))
        return NULL;
    run->start = start;
    run->end = end;
    VariantInit(&run->value);
    if (FAILED(VariantCopy(&run->value, (VARIANT *)value)))
    {
        free(run);
        return NULL;
    }
    return run;
}

static HRESULT property_clear_interval(ContextProperty *property, LONG start, LONG end)
{
    PropertyRun *run, *next, *left, *right;
    struct list replacements;

    list_init(&replacements);

    LIST_FOR_EACH_ENTRY(run, &property->runs, PropertyRun, entry)
    {
        if (run->end <= start || run->start >= end)
            continue;
        if (run->start < start)
        {
            if (!(left = property_run_create(run->start, start, &run->value)))
                goto failed;
            list_add_tail(&replacements, &left->entry);
        }
        if (run->end > end)
        {
            if (!(right = property_run_create(end, run->end, &run->value)))
                goto failed;
            list_add_tail(&replacements, &right->entry);
        }
    }
    LIST_FOR_EACH_ENTRY_SAFE(run, next, &property->runs, PropertyRun, entry)
    {
        if (run->end <= start || run->start >= end)
            continue;
        list_remove(&run->entry);
        property_run_free(run);
    }
    list_move_tail(&property->runs, &replacements);
    return S_OK;

failed:
    LIST_FOR_EACH_ENTRY_SAFE(run, next, &replacements, PropertyRun, entry)
    {
        list_remove(&run->entry);
        property_run_free(run);
    }
    return E_OUTOFMEMORY;
}

static HRESULT property_set_value(ContextProperty *property, ITfRange *range,
        const VARIANT *value)
{
    PropertyRun *run;
    LONG start, end;
    HRESULT hr;

    if (!value || (V_VT(value) != VT_EMPTY && V_VT(value) != VT_I4 &&
        V_VT(value) != VT_UNKNOWN && V_VT(value) != VT_BSTR))
        return E_INVALIDARG;
    if (FAILED(hr = get_owned_range_extent(property->context, range, &start, &end)))
        return hr;
    if (start == end)
        return E_INVALIDARG;
    if (V_VT(value) != VT_EMPTY)
    {
        if (!(run = property_run_create(start, end, value)))
            return E_OUTOFMEMORY;
    }
    else
        run = NULL;
    if (FAILED(hr = property_clear_interval(property, start, end)))
    {
        if (run) property_run_free(run);
        return hr;
    }
    if (run) list_add_tail(&property->runs, &run->entry);
    if (property->context->activeEditRecord)
        edit_record_add_property_change(property->context->activeEditRecord,
                &property->guid, start, end);
    return S_OK;
}

static HRESULT property_clear_value(ContextProperty *property, ITfRange *range)
{
    LONG start, end;
    HRESULT hr;

    if (FAILED(hr = get_owned_range_extent(property->context, range, &start, &end)))
        return hr;
    if (start == end)
        return S_OK;
    if (FAILED(hr = property_clear_interval(property, start, end)))
        return hr;
    if (property->context->activeEditRecord)
        edit_record_add_property_change(property->context->activeEditRecord,
                &property->guid, start, end);
    return S_OK;
}

static BOOL property_values_equal(const VARIANT *left, const VARIANT *right)
{
    IUnknown *left_identity, *right_identity;
    BOOL equal;

    if (V_VT(left) != V_VT(right))
        return FALSE;
    switch (V_VT(left))
    {
        case VT_EMPTY:
            return TRUE;
        case VT_I4:
            return V_I4(left) == V_I4(right);
        case VT_BSTR:
            if (!V_BSTR(left) || !V_BSTR(right))
                return V_BSTR(left) == V_BSTR(right);
            return !lstrcmpW(V_BSTR(left), V_BSTR(right));
        case VT_UNKNOWN:
            if (!V_UNKNOWN(left) || !V_UNKNOWN(right))
                return V_UNKNOWN(left) == V_UNKNOWN(right);
            if (FAILED(IUnknown_QueryInterface(V_UNKNOWN(left), &IID_IUnknown,
                    (void **)&left_identity)))
                return FALSE;
            if (FAILED(IUnknown_QueryInterface(V_UNKNOWN(right), &IID_IUnknown,
                    (void **)&right_identity)))
            {
                IUnknown_Release(left_identity);
                return FALSE;
            }
            equal = left_identity == right_identity;
            IUnknown_Release(right_identity);
            IUnknown_Release(left_identity);
            return equal;
        default:
            return FALSE;
    }
}

static HRESULT property_get_uniform_value(ContextProperty *property, LONG start, LONG end,
        VARIANT *value)
{
    PropertyRun *run, *covering;
    LONG position = start;
    HRESULT hr;

    VariantInit(value);
    if (start == end)
    {
        LIST_FOR_EACH_ENTRY(run, &property->runs, PropertyRun, entry)
            if (run->start <= start && run->end > start)
                return VariantCopy(value, &run->value);
        return S_FALSE;
    }

    while (position < end)
    {
        covering = NULL;
        LIST_FOR_EACH_ENTRY(run, &property->runs, PropertyRun, entry)
        {
            if (run->start <= position && run->end > position)
            {
                covering = run;
                break;
            }
        }
        if (!covering)
            goto not_uniform;
        if (V_VT(value) == VT_EMPTY)
        {
            if (FAILED(hr = VariantCopy(value, &covering->value)))
                return hr;
        }
        else if (!property_values_equal(value, &covering->value))
            goto not_uniform;
        position = min(end, covering->end);
    }
    return S_OK;

not_uniform:
    VariantClear(value);
    VariantInit(value);
    return S_FALSE;
}

static int compare_long(const void *left, const void *right)
{
    LONG a = *(const LONG *)left, b = *(const LONG *)right;
    return (a > b) - (a < b);
}

static BOOL property_is_tracked(const GUID *guid, const GUID *guids, ULONG count)
{
    ULONG i;
    for (i = 0; i < count; ++i)
        if (IsEqualGUID(guid, &guids[i]))
            return TRUE;
    return FALSE;
}

static HRESULT property_enum_ranges(Context *context, const GUID *guids, ULONG count,
        TfEditCookie cookie, ITfRange *target, IEnumTfRanges **out)
{
    ContextProperty *property;
    PropertyRun *run;
    ITfRange **ranges = NULL;
    LONG *boundaries = NULL, start = LONG_MAX, end = LONG_MIN;
    ULONG boundary_count = 0, capacity = 2, range_count = 0, i;
    HRESULT hr = S_OK;

    if (!out || !count)
        return E_INVALIDARG;
    *out = NULL;
    if (!Context_IsValidCookie(&context->ITfContext_iface, cookie, TS_LF_READ))
        return TF_E_NOLOCK;
    if (target)
    {
        if (FAILED(hr = get_owned_range_extent(context, target, &start, &end)))
            return hr;
    }
    else
    {
        LIST_FOR_EACH_ENTRY(property, &context->properties, ContextProperty, entry)
        {
            if (!property_is_tracked(&property->guid, guids, count))
                continue;
            LIST_FOR_EACH_ENTRY(run, &property->runs, PropertyRun, entry)
            {
                start = min(start, run->start);
                end = max(end, run->end);
            }
        }
        if (start == LONG_MAX)
            return range_enumerator_create(NULL, 0, 0, out);
    }

    LIST_FOR_EACH_ENTRY(property, &context->properties, ContextProperty, entry)
    {
        if (!property_is_tracked(&property->guid, guids, count))
            continue;
        LIST_FOR_EACH_ENTRY(run, &property->runs, PropertyRun, entry)
            if (run->end > start && run->start < end)
                capacity += 2;
    }
    if (!(boundaries = calloc(capacity, sizeof(*boundaries))))
        return E_OUTOFMEMORY;
    boundaries[boundary_count++] = start;
    boundaries[boundary_count++] = end;
    LIST_FOR_EACH_ENTRY(property, &context->properties, ContextProperty, entry)
    {
        if (!property_is_tracked(&property->guid, guids, count))
            continue;
        LIST_FOR_EACH_ENTRY(run, &property->runs, PropertyRun, entry)
        {
            if (run->end <= start || run->start >= end)
                continue;
            boundaries[boundary_count++] = max(start, run->start);
            boundaries[boundary_count++] = min(end, run->end);
        }
    }
    qsort(boundaries, boundary_count, sizeof(*boundaries), compare_long);
    for (i = 1; i < boundary_count; ++i)
        if (boundaries[i] != boundaries[range_count])
            boundaries[++range_count] = boundaries[i];
    boundary_count = range_count + 1;
    range_count = boundary_count > 1 ? boundary_count - 1 : 0;
    if (range_count && !(ranges = calloc(range_count, sizeof(*ranges))))
    {
        free(boundaries);
        return E_OUTOFMEMORY;
    }
    for (i = 0; i < range_count; ++i)
    {
        if (FAILED(hr = Range_Constructor(&context->ITfContext_iface, boundaries[i],
                boundaries[i + 1], &ranges[i])))
        {
            range_count = i;
            goto done;
        }
    }
    hr = range_enumerator_create(ranges, range_count, 0, out);
done:
    for (i = 0; i < range_count; ++i)
        ITfRange_Release(ranges[i]);
    free(ranges);
    free(boundaries);
    return hr;
}

static HRESULT WINAPI ContextProperty_QueryInterface(ITfProperty *iface, REFIID iid, void **out)
{
    ContextProperty *property = impl_from_ITfProperty(iface);

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITfReadOnlyProperty) ||
        IsEqualIID(iid, &IID_ITfProperty))
        *out = iface;
    if (!*out) return E_NOINTERFACE;
    Context_AddRef(&property->context->ITfContext_iface);
    return S_OK;
}

static ULONG WINAPI ContextProperty_AddRef(ITfProperty *iface)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    return Context_AddRef(&property->context->ITfContext_iface);
}

static ULONG WINAPI ContextProperty_Release(ITfProperty *iface)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    return Context_Release(&property->context->ITfContext_iface);
}

static HRESULT WINAPI ContextProperty_GetType(ITfProperty *iface, GUID *guid)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    if (!guid) return E_INVALIDARG;
    *guid = property->guid;
    return S_OK;
}

static HRESULT WINAPI ContextProperty_EnumRanges(ITfProperty *iface, TfEditCookie cookie,
        IEnumTfRanges **out, ITfRange *target)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    return property_enum_ranges(property->context, &property->guid, 1, cookie, target, out);
}

static HRESULT WINAPI ContextProperty_GetValue(ITfProperty *iface, TfEditCookie cookie,
        ITfRange *range, VARIANT *value)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    LONG start, end;
    HRESULT hr;

    if (!value) return E_INVALIDARG;
    VariantInit(value);
    if (!Context_IsValidCookie(&property->context->ITfContext_iface, cookie, TS_LF_READ))
        return TF_E_NOLOCK;
    if (FAILED(hr = get_owned_range_extent(property->context, range, &start, &end)))
        return hr;
    return property_get_uniform_value(property, start, end, value);
}

static HRESULT WINAPI ContextProperty_GetContext(ITfProperty *iface, ITfContext **context)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    if (!context) return E_INVALIDARG;
    *context = &property->context->ITfContext_iface;
    ITfContext_AddRef(*context);
    return S_OK;
}

static HRESULT WINAPI ContextProperty_FindRange(ITfProperty *iface, TfEditCookie cookie,
        ITfRange *range, ITfRange **out, TfAnchor anchor)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    PropertyRun *run;
    LONG start, end, position;
    HRESULT hr;

    if (!out || (anchor != TF_ANCHOR_START && anchor != TF_ANCHOR_END))
        return E_INVALIDARG;
    *out = NULL;
    if (!Context_IsValidCookie(&property->context->ITfContext_iface, cookie, TS_LF_READ))
        return TF_E_NOLOCK;
    if (FAILED(hr = get_owned_range_extent(property->context, range, &start, &end)))
        return hr;
    position = anchor == TF_ANCHOR_START ? start : end;
    LIST_FOR_EACH_ENTRY(run, &property->runs, PropertyRun, entry)
    {
        if ((anchor == TF_ANCHOR_START && run->start <= position && run->end > position) ||
            (anchor == TF_ANCHOR_END && run->start < position && run->end >= position))
            return Range_Constructor(&property->context->ITfContext_iface, run->start,
                    run->end, out);
    }
    return S_FALSE;
}

static HRESULT WINAPI ContextProperty_SetValueStore(ITfProperty *iface, TfEditCookie cookie,
        ITfRange *range, ITfPropertyStore *store)
{
    VARIANT value;
    HRESULT hr;

    if (!store) return E_INVALIDARG;
    VariantInit(&value);
    if (FAILED(hr = ITfPropertyStore_GetData(store, &value)))
        return hr;
    hr = ContextProperty_SetValue(iface, cookie, range, &value);
    VariantClear(&value);
    return hr;
}

static HRESULT WINAPI ContextProperty_SetValue(ITfProperty *iface, TfEditCookie cookie,
        ITfRange *range, const VARIANT *value)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    if (!Context_IsValidCookie(&property->context->ITfContext_iface, cookie, TS_LF_READWRITE))
        return TF_E_NOLOCK;
    return property_set_value(property, range, value);
}

static HRESULT WINAPI ContextProperty_Clear(ITfProperty *iface, TfEditCookie cookie,
        ITfRange *range)
{
    ContextProperty *property = impl_from_ITfProperty(iface);
    if (!Context_IsValidCookie(&property->context->ITfContext_iface, cookie, TS_LF_READWRITE))
        return TF_E_NOLOCK;
    return property_clear_value(property, range);
}

static const ITfPropertyVtbl ContextPropertyVtbl =
{
    ContextProperty_QueryInterface,
    ContextProperty_AddRef,
    ContextProperty_Release,
    ContextProperty_GetType,
    ContextProperty_EnumRanges,
    ContextProperty_GetValue,
    ContextProperty_GetContext,
    ContextProperty_FindRange,
    ContextProperty_SetValueStore,
    ContextProperty_SetValue,
    ContextProperty_Clear
};

static ContextProperty *context_create_property(Context *context, REFGUID guid)
{
    ContextProperty *property;

    if (!(property = calloc(1, sizeof(*property))))
        return NULL;
    property->ITfProperty_iface.lpVtbl = &ContextPropertyVtbl;
    property->context = context;
    property->guid = *guid;
    list_init(&property->runs);
    list_add_tail(&context->properties, &property->entry);
    return property;
}

static HRESULT WINAPI TrackingProperty_QueryInterface(ITfReadOnlyProperty *iface,
        REFIID iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITfReadOnlyProperty))
        *out = iface;
    if (!*out) return E_NOINTERFACE;
    ITfReadOnlyProperty_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI TrackingProperty_AddRef(ITfReadOnlyProperty *iface)
{
    TrackingProperty *property = impl_from_ITfReadOnlyProperty(iface);
    return InterlockedIncrement(&property->ref);
}

static ULONG WINAPI TrackingProperty_Release(ITfReadOnlyProperty *iface)
{
    TrackingProperty *property = impl_from_ITfReadOnlyProperty(iface);
    ULONG ref = InterlockedDecrement(&property->ref);
    if (!ref)
    {
        ITfContext_Release(&property->context->ITfContext_iface);
        free(property);
    }
    return ref;
}

static HRESULT WINAPI TrackingProperty_GetType(ITfReadOnlyProperty *iface, GUID *guid)
{
    if (!guid) return E_INVALIDARG;
    *guid = GUID_NULL;
    return S_OK;
}

static HRESULT WINAPI TrackingProperty_EnumRanges(ITfReadOnlyProperty *iface,
        TfEditCookie cookie, IEnumTfRanges **out, ITfRange *target)
{
    TrackingProperty *property = impl_from_ITfReadOnlyProperty(iface);
    return property_enum_ranges(property->context, property->guids, property->count,
            cookie, target, out);
}

static HRESULT WINAPI TrackingProperty_GetValue(ITfReadOnlyProperty *iface,
        TfEditCookie cookie, ITfRange *range, VARIANT *value)
{
    TrackingProperty *property = impl_from_ITfReadOnlyProperty(iface);
    TF_PROPERTYVAL *values;
    IEnumTfPropertyValue *enumerator;
    ContextProperty *context_property;
    LONG start, end;
    ULONG count = 0, i;
    HRESULT hr;

    if (!value) return E_INVALIDARG;
    VariantInit(value);
    if (!Context_IsValidCookie(&property->context->ITfContext_iface, cookie, TS_LF_READ))
        return TF_E_NOLOCK;
    if (FAILED(hr = get_owned_range_extent(property->context, range, &start, &end)))
        return hr;
    if (!(values = calloc(property->count, sizeof(*values))))
        return E_OUTOFMEMORY;
    for (i = 0; i < property->count; ++i)
    {
        VARIANT property_value;

        context_property = context_find_property(property->context, &property->guids[i]);
        if (!context_property)
            continue;
        VariantInit(&property_value);
        if (property_get_uniform_value(context_property, start, end, &property_value) == S_OK)
        {
            values[count].guidId = property->guids[i];
            values[count++].varValue = property_value;
        }
    }
    hr = property_value_enumerator_create(values, count, 0, &enumerator);
    while (count) VariantClear(&values[--count].varValue);
    free(values);
    if (FAILED(hr))
        return hr;
    V_VT(value) = VT_UNKNOWN;
    V_UNKNOWN(value) = (IUnknown *)enumerator;
    return S_OK;
}

static HRESULT WINAPI TrackingProperty_GetContext(ITfReadOnlyProperty *iface,
        ITfContext **context)
{
    TrackingProperty *property = impl_from_ITfReadOnlyProperty(iface);
    if (!context) return E_INVALIDARG;
    *context = &property->context->ITfContext_iface;
    ITfContext_AddRef(*context);
    return S_OK;
}

static const ITfReadOnlyPropertyVtbl TrackingPropertyVtbl =
{
    TrackingProperty_QueryInterface,
    TrackingProperty_AddRef,
    TrackingProperty_Release,
    TrackingProperty_GetType,
    TrackingProperty_EnumRanges,
    TrackingProperty_GetValue,
    TrackingProperty_GetContext
};

static HRESULT tracking_property_create(Context *context, const GUID **properties,
        ULONG count, ITfReadOnlyProperty **out)
{
    TrackingProperty *property;
    SIZE_T size;
    ULONG i;

    if (!out || !properties || !count)
        return E_INVALIDARG;
    *out = NULL;
#ifndef _WIN64
    if (count > (SIZE_MAX - offsetof(TrackingProperty, guids)) / sizeof(GUID))
        return E_OUTOFMEMORY;
#endif
    size = offsetof(TrackingProperty, guids) + count * sizeof(GUID);
    if (!(property = calloc(1, size)))
        return E_OUTOFMEMORY;
    property->ITfReadOnlyProperty_iface.lpVtbl = &TrackingPropertyVtbl;
    property->ref = 1;
    property->context = context;
    property->count = count;
    for (i = 0; i < count; ++i)
    {
        if (!properties[i])
        {
            free(property);
            return E_INVALIDARG;
        }
        property->guids[i] = *properties[i];
    }
    ITfContext_AddRef(&context->ITfContext_iface);
    *out = &property->ITfReadOnlyProperty_iface;
    return S_OK;
}

static HRESULT context_set_property_value(Context *context, REFGUID guid, ITfRange *range,
        const VARIANT *value)
{
    ContextProperty *property = context_find_property(context, guid);
    if (!property && !(property = context_create_property(context, guid)))
        return E_OUTOFMEMORY;
    return property_set_value(property, range, value);
}

static HRESULT context_clear_property(Context *context, REFGUID guid, ITfRange *range)
{
    ContextProperty *property = context_find_property(context, guid);
    if (!property)
        return S_OK;
    return property_clear_value(property, range);
}

static HRESULT WINAPI Composition_QueryInterface(ITfComposition *iface, REFIID iid, void **out)
{
    Composition *composition = impl_from_ITfComposition(iface);

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITfComposition))
        *out = &composition->ITfComposition_iface;
    else if (IsEqualIID(iid, &IID_ITfCompositionView))
        *out = &composition->ITfCompositionView_iface;
    if (!*out) return E_NOINTERFACE;
    ITfComposition_AddRef(&composition->ITfComposition_iface);
    return S_OK;
}

static ULONG WINAPI Composition_AddRef(ITfComposition *iface)
{
    Composition *composition = impl_from_ITfComposition(iface);
    return InterlockedIncrement(&composition->ref);
}

static ULONG WINAPI Composition_Release(ITfComposition *iface)
{
    Composition *composition = impl_from_ITfComposition(iface);
    ULONG ref = InterlockedDecrement(&composition->ref);

    if (!ref)
    {
        if (composition->sink) ITfCompositionSink_Release(composition->sink);
        if (composition->range) ITfRange_Release(composition->range);
        ITfContext_Release(&composition->context->ITfContext_iface);
        free(composition);
    }
    return ref;
}

static HRESULT WINAPI Composition_GetRange(ITfComposition *iface, ITfRange **range)
{
    Composition *composition = impl_from_ITfComposition(iface);
    if (!range) return E_INVALIDARG;
    *range = NULL;
    if (!composition->active) return E_UNEXPECTED;
    return ITfRange_Clone(composition->range, range);
}

static HRESULT composition_update_range(Composition *composition, TfEditCookie cookie,
        ITfRange *range, BOOL start_anchor)
{
    ITfRangeACP *current, *replacement;
    LONG current_start, current_count, new_start, new_end;
    LONGLONG current_end;
    VARIANT value;
    HRESULT hr;

    if (!composition->active)
        return E_UNEXPECTED;
    if (composition->context->compositionOperation)
        return E_UNEXPECTED;
    if (!Context_IsValidCookie(&composition->context->ITfContext_iface, cookie, TS_LF_READWRITE))
        return TF_E_NOLOCK;
    if (FAILED(hr = get_owned_range_extent(composition->context, range, &new_start, &new_end)))
        return hr;
    if (FAILED(hr = ITfRange_QueryInterface(composition->range, &IID_ITfRangeACP,
            (void **)&current)))
        return hr;
    hr = ITfRangeACP_GetExtent(current, &current_start, &current_count);
    if (FAILED(hr))
    {
        ITfRangeACP_Release(current);
        return hr;
    }
    current_end = (LONGLONG)current_start + current_count;
    if ((start_anchor && new_start > current_end) ||
        (!start_anchor && new_end < current_start))
    {
        ITfRangeACP_Release(current);
        return E_INVALIDARG;
    }
    if (FAILED(hr = ITfRange_QueryInterface(range, &IID_ITfRangeACP, (void **)&replacement)))
    {
        ITfRangeACP_Release(current);
        return hr;
    }
    ITfRangeACP_Release(replacement);

    context_clear_property(composition->context, &GUID_PROP_COMPOSING, composition->range);
    if (start_anchor)
    {
        current_count = current_end - new_start;
        current_start = new_start;
    }
    else
        current_count = new_end - current_start;
    hr = ITfRangeACP_SetExtent(current, current_start, current_count);
    ITfRangeACP_Release(current);
    if (FAILED(hr))
        return hr;

    VariantInit(&value);
    V_VT(&value) = VT_I4;
    V_I4(&value) = TRUE;
    if (current_count)
        hr = context_set_property_value(composition->context, &GUID_PROP_COMPOSING,
                composition->range, &value);
    if (SUCCEEDED(hr) && composition->context->pITfContextOwnerCompositionSink)
        hr = ITfContextOwnerCompositionSink_OnUpdateComposition(
                composition->context->pITfContextOwnerCompositionSink,
                &composition->ITfCompositionView_iface, composition->range);
    return hr;
}

static HRESULT WINAPI Composition_ShiftStart(ITfComposition *iface, TfEditCookie cookie,
        ITfRange *range)
{
    return composition_update_range(impl_from_ITfComposition(iface), cookie, range, TRUE);
}

static HRESULT WINAPI Composition_ShiftEnd(ITfComposition *iface, TfEditCookie cookie,
        ITfRange *range)
{
    return composition_update_range(impl_from_ITfComposition(iface), cookie, range, FALSE);
}

static HRESULT WINAPI Composition_EndComposition(ITfComposition *iface, TfEditCookie cookie)
{
    Composition *composition = impl_from_ITfComposition(iface);
    CLSID owner;

    if (!composition->active)
        return E_UNEXPECTED;
    if (!Context_IsValidCookie(&composition->context->ITfContext_iface, cookie, TS_LF_READWRITE))
        return TF_E_NOLOCK;
    owner = get_textservice_clsid(composition->context->currentEditSessionTid);
    if (!IsEqualCLSID(&owner, &composition->owner))
        return E_UNEXPECTED;
    return composition_finish(composition, cookie, FALSE);
}

static const ITfCompositionVtbl CompositionVtbl =
{
    Composition_QueryInterface,
    Composition_AddRef,
    Composition_Release,
    Composition_GetRange,
    Composition_ShiftStart,
    Composition_ShiftEnd,
    Composition_EndComposition
};

static HRESULT WINAPI CompositionView_QueryInterface(ITfCompositionView *iface,
        REFIID iid, void **out)
{
    Composition *composition = impl_from_ITfCompositionView(iface);
    return Composition_QueryInterface(&composition->ITfComposition_iface, iid, out);
}

static ULONG WINAPI CompositionView_AddRef(ITfCompositionView *iface)
{
    Composition *composition = impl_from_ITfCompositionView(iface);
    return Composition_AddRef(&composition->ITfComposition_iface);
}

static ULONG WINAPI CompositionView_Release(ITfCompositionView *iface)
{
    Composition *composition = impl_from_ITfCompositionView(iface);
    return Composition_Release(&composition->ITfComposition_iface);
}

static HRESULT WINAPI CompositionView_GetOwnerClsid(ITfCompositionView *iface, CLSID *owner)
{
    Composition *composition = impl_from_ITfCompositionView(iface);
    if (!owner) return E_INVALIDARG;
    *owner = composition->owner;
    return S_OK;
}

static HRESULT WINAPI CompositionView_GetRange(ITfCompositionView *iface, ITfRange **range)
{
    Composition *composition = impl_from_ITfCompositionView(iface);
    return Composition_GetRange(&composition->ITfComposition_iface, range);
}

static const ITfCompositionViewVtbl CompositionViewVtbl =
{
    CompositionView_QueryInterface,
    CompositionView_AddRef,
    CompositionView_Release,
    CompositionView_GetOwnerClsid,
    CompositionView_GetRange
};

static HRESULT composition_create(Context *context, ITfRange *range, ITfCompositionSink *sink,
        Composition **out)
{
    Composition *composition;
    HRESULT hr;

    if (!(composition = calloc(1, sizeof(*composition))))
        return E_OUTOFMEMORY;
    composition->ITfComposition_iface.lpVtbl = &CompositionVtbl;
    composition->ITfCompositionView_iface.lpVtbl = &CompositionViewVtbl;
    composition->ref = 1;
    composition->context = context;
    ITfContext_AddRef(&context->ITfContext_iface);
    if (FAILED(hr = ITfRange_Clone(range, &composition->range)))
    {
        ITfComposition_Release(&composition->ITfComposition_iface);
        return hr;
    }
    composition->sink = sink;
    if (sink) ITfCompositionSink_AddRef(sink);
    composition->owner = get_textservice_clsid(context->currentEditSessionTid);
    *out = composition;
    return S_OK;
}

static HRESULT composition_finish(Composition *composition, TfEditCookie cookie, BOOL terminated)
{
    Context *context = composition->context;
    HRESULT hr = S_OK;

    if (!composition->active)
        return E_UNEXPECTED;
    if (context->compositionOperation)
        return E_UNEXPECTED;

    context->compositionOperation = TRUE;
    context_clear_property(context, &GUID_PROP_COMPOSING, composition->range);
    list_remove(&composition->entry);
    if (context->pITfContextOwnerCompositionSink)
        hr = ITfContextOwnerCompositionSink_OnEndComposition(
                context->pITfContextOwnerCompositionSink,
                &composition->ITfCompositionView_iface);
    if (terminated && composition->sink)
        ITfCompositionSink_OnCompositionTerminated(composition->sink, cookie,
                &composition->ITfComposition_iface);
    composition->active = FALSE;
    context->compositionOperation = FALSE;

    /* Drop the reference held while the composition is in the context list. */
    ITfComposition_Release(&composition->ITfComposition_iface);
    return hr;
}

static void composition_adjust_for_text_change(Composition *composition,
        const TS_TEXTCHANGE *change)
{
    ITfRangeACP *range;
    LONG start, count, end, new_start, new_end;

    if (!composition->active ||
        FAILED(ITfRange_QueryInterface(composition->range, &IID_ITfRangeACP, (void **)&range)))
        return;
    if (FAILED(ITfRangeACP_GetExtent(range, &start, &count)))
    {
        ITfRangeACP_Release(range);
        return;
    }
    end = start + count;
    new_start = start;
    new_end = end;

    if (change->acpOldEnd == change->acpStart && start == end &&
        start == change->acpStart)
    {
        new_end = change->acpNewEnd;
    }
    else if (end <= change->acpStart)
    {
        /* Entirely before the edit. */
    }
    else if (start >= change->acpOldEnd)
    {
        LONG delta = change->acpNewEnd - change->acpOldEnd;
        new_start += delta;
        new_end += delta;
    }
    else
    {
        if (start >= change->acpStart) new_start = change->acpStart;
        if (end <= change->acpOldEnd) new_end = change->acpNewEnd;
        else new_end += change->acpNewEnd - change->acpOldEnd;
        if (new_end < new_start) new_end = new_start;
    }
    ITfRangeACP_SetExtent(range, new_start, new_end - new_start);
    ITfRangeACP_Release(range);
}

static HRESULT WINAPI TerminateCompositionSession_QueryInterface(ITfEditSession *iface,
        REFIID iid, void **out)
{
    if (!out)
        return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITfEditSession))
        *out = iface;
    if (!*out)
        return E_NOINTERFACE;
    ITfEditSession_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI TerminateCompositionSession_AddRef(ITfEditSession *iface)
{
    TerminateCompositionSession *session = impl_from_terminate_ITfEditSession(iface);
    return InterlockedIncrement(&session->ref);
}

static ULONG WINAPI TerminateCompositionSession_Release(ITfEditSession *iface)
{
    TerminateCompositionSession *session = impl_from_terminate_ITfEditSession(iface);
    ULONG ref = InterlockedDecrement(&session->ref);

    if (!ref)
    {
        if (session->target) ITfCompositionView_Release(session->target);
        ITfContext_Release(&session->context->ITfContext_iface);
        free(session);
    }
    return ref;
}

static HRESULT WINAPI TerminateCompositionSession_DoEditSession(ITfEditSession *iface,
        TfEditCookie cookie)
{
    TerminateCompositionSession *session = impl_from_terminate_ITfEditSession(iface);
    Composition *composition, *next;
    HRESULT result = S_OK, hr;

    if (session->target)
    {
        if (session->target->lpVtbl != &CompositionViewVtbl)
            return E_INVALIDARG;
        composition = impl_from_ITfCompositionView(session->target);
        if (composition->context != session->context || !composition->active)
            return E_INVALIDARG;
        return composition_finish(composition, cookie, TRUE);
    }

    LIST_FOR_EACH_ENTRY_SAFE(composition, next, &session->context->compositions,
            Composition, entry)
    {
        hr = composition_finish(composition, cookie, TRUE);
        if (FAILED(hr) && SUCCEEDED(result))
            result = hr;
    }
    return result;
}

static const ITfEditSessionVtbl TerminateCompositionSessionVtbl =
{
    TerminateCompositionSession_QueryInterface,
    TerminateCompositionSession_AddRef,
    TerminateCompositionSession_Release,
    TerminateCompositionSession_DoEditSession
};

static HRESULT terminate_composition_session_create(Context *context,
        ITfCompositionView *target, ITfEditSession **out)
{
    TerminateCompositionSession *session;

    if (!(session = calloc(1, sizeof(*session))))
        return E_OUTOFMEMORY;
    session->ITfEditSession_iface.lpVtbl = &TerminateCompositionSessionVtbl;
    session->ref = 1;
    session->context = context;
    ITfContext_AddRef(&context->ITfContext_iface);
    session->target = target;
    if (target) ITfCompositionView_AddRef(target);
    *out = &session->ITfEditSession_iface;
    return S_OK;
}

BOOL Context_IsValidCookie(ITfContext *context, TfEditCookie cookie, DWORD lock_type)
{
    EditCookie *data;

    if (!context || get_Cookie_magic(cookie) != COOKIE_MAGIC_EDITCOOKIE ||
        !(data = get_Cookie_data(cookie)) || data->pOwningContext != impl_from_ITfContext(context))
        return FALSE;
    if ((lock_type & TS_LF_READWRITE) == TS_LF_READWRITE)
        return (data->lockType & TS_LF_READWRITE) == TS_LF_READWRITE;
    if (lock_type & TS_LF_READ)
        return (data->lockType & TS_LF_READ) == TS_LF_READ;
    return TRUE;
}

HRESULT Context_GetTextStore(ITfContext *context, TfEditCookie cookie, DWORD lock_type,
        ITextStoreACP **store)
{
    Context *This;

    if (!context || !store)
        return E_INVALIDARG;
    *store = NULL;
    This = impl_from_ITfContext(context);
    if (!This->connected)
        return TF_E_DISCONNECTED;
    if (!Context_IsValidCookie(context, cookie, lock_type))
        return TF_E_NOLOCK;
    if (!This->pITextStoreACP)
        return E_NOTIMPL;
    *store = This->pITextStoreACP;
    ITextStoreACP_AddRef(*store);
    return S_OK;
}

static void property_adjust_for_text_change(ContextProperty *property,
        const TS_TEXTCHANGE *change)
{
    PropertyRun *run, *next, *left, *right;
    LONG delta = change->acpNewEnd - change->acpOldEnd;

    LIST_FOR_EACH_ENTRY_SAFE(run, next, &property->runs, PropertyRun, entry)
    {
        if (change->acpOldEnd == change->acpStart)
        {
            if (run->start >= change->acpStart)
            {
                run->start += delta;
                run->end += delta;
            }
            else if (run->end > change->acpStart)
                run->end += delta;
            continue;
        }
        if (run->end <= change->acpStart)
            continue;
        if (run->start >= change->acpOldEnd)
        {
            run->start += delta;
            run->end += delta;
            continue;
        }

        left = right = NULL;
        if (run->start < change->acpStart)
            left = property_run_create(run->start, change->acpStart, &run->value);
        if (run->end > change->acpOldEnd)
            right = property_run_create(change->acpNewEnd, run->end + delta, &run->value);
        list_remove(&run->entry);
        property_run_free(run);
        if (left) list_add_tail(&property->runs, &left->entry);
        if (right) list_add_tail(&property->runs, &right->entry);
    }
}

static void context_notify_text_edit_sinks(Context *context, EditRecord *record,
        TfEditCookie cookie)
{
    ITfTextEditSink **sinks = NULL, *sink;
    struct list *cursor;
    ULONG count = 0, i = 0;

    SINK_FOR_EACH(cursor, &context->pTextEditSink, ITfTextEditSink, sink)
        ++count;
    if (count && !(sinks = calloc(count, sizeof(*sinks))))
        return;
    SINK_FOR_EACH(cursor, &context->pTextEditSink, ITfTextEditSink, sink)
    {
        sinks[i++] = sink;
        ITfTextEditSink_AddRef(sink);
    }

    ++context->notifyingTextEditSinks;
    for (i = 0; i < count; ++i)
    {
        ITfTextEditSink_OnEndEdit(sinks[i], &context->ITfContext_iface, cookie,
                &record->ITfEditRecord_iface);
        ITfTextEditSink_Release(sinks[i]);
    }
    --context->notifyingTextEditSinks;
    free(sinks);
}

void Context_RecordTextChange(ITfContext *context, const TS_TEXTCHANGE *change)
{
    Context *This;
    ContextProperty *property;
    Composition *composition;
    VARIANT value;

    if (!context || !change)
        return;
    This = impl_from_ITfContext(context);
    LIST_FOR_EACH_ENTRY(property, &This->properties, ContextProperty, entry)
        property_adjust_for_text_change(property, change);
    LIST_FOR_EACH_ENTRY(composition, &This->compositions, Composition, entry)
        composition_adjust_for_text_change(composition, change);

    VariantInit(&value);
    V_VT(&value) = VT_I4;
    V_I4(&value) = TRUE;
    LIST_FOR_EACH_ENTRY(composition, &This->compositions, Composition, entry)
    {
        ITfRangeACP *range;
        LONG start, count;

        if (FAILED(ITfRange_QueryInterface(composition->range, &IID_ITfRangeACP,
                (void **)&range)))
            continue;
        if (SUCCEEDED(ITfRangeACP_GetExtent(range, &start, &count)) && count)
            context_set_property_value(This, &GUID_PROP_COMPOSING, composition->range, &value);
        ITfRangeACP_Release(range);
    }
    if (This->activeEditRecord)
        edit_record_add_text_change(This->activeEditRecord, change);
}

static void Context_Destructor(Context *This)
{
    EditSessionRequest *request, *request_next;
    ContextProperty *property, *property_next;
    PropertyRun *run, *run_next;
    EditCookie *cookie;
    TRACE("destroying %p\n", This);

    if (This->pITextStoreACP)
        ITextStoreACP_Release(This->pITextStoreACP);

    if (This->pITfContextOwnerCompositionSink)
        ITfContextOwnerCompositionSink_Release(This->pITfContextOwnerCompositionSink);

    if (This->defaultCookie)
    {
        cookie = remove_Cookie(This->defaultCookie);
        free(cookie);
        This->defaultCookie = 0;
    }

    free_sinks(&This->pContextKeyEventSink);
    free_sinks(&This->pEditTransactionSink);
    free_sinks(&This->pStatusSink);
    free_sinks(&This->pTextEditSink);
    free_sinks(&This->pTextLayoutSink);

    LIST_FOR_EACH_ENTRY_SAFE(request, request_next, &This->editSessionRequests,
            EditSessionRequest, entry)
    {
        list_remove(&request->entry);
        ITfEditSession_Release(request->session);
        free(request);
    }
    LIST_FOR_EACH_ENTRY_SAFE(property, property_next, &This->properties,
            ContextProperty, entry)
    {
        LIST_FOR_EACH_ENTRY_SAFE(run, run_next, &property->runs, PropertyRun, entry)
        {
            list_remove(&run->entry);
            property_run_free(run);
        }
        list_remove(&property->entry);
        free(property);
    }

    CompartmentMgr_Destructor(This->CompartmentMgr);
    free(This);
}

static HRESULT WINAPI Context_QueryInterface(ITfContext *iface, REFIID iid, LPVOID *ppvOut)
{
    Context *This = impl_from_ITfContext(iface);

    if (!ppvOut)
        return E_INVALIDARG;
    *ppvOut = NULL;

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITfContext))
    {
        *ppvOut = &This->ITfContext_iface;
    }
    else if (IsEqualIID(iid, &IID_ITfSource))
    {
        *ppvOut = &This->ITfSource_iface;
    }
    else if (IsEqualIID(iid, &IID_ITfContextComposition) ||
            IsEqualIID(iid, &IID_ITfContextOwnerCompositionServices))
    {
        *ppvOut = &This->ITfContextOwnerCompositionServices_iface;
    }
    else if (IsEqualIID(iid, &IID_ITfInsertAtSelection))
    {
        *ppvOut = &This->ITfInsertAtSelection_iface;
    }
    else if (IsEqualIID(iid, &IID_ITfCompartmentMgr))
    {
        *ppvOut = This->CompartmentMgr;
    }
    else if (IsEqualIID(iid, &IID_ITfSourceSingle))
    {
        *ppvOut = &This->ITfSourceSingle_iface;
    }

    if (*ppvOut)
    {
        ITfContext_AddRef(iface);
        return S_OK;
    }

    WARN("unsupported interface: %s\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI Context_AddRef(ITfContext *iface)
{
    Context *This = impl_from_ITfContext(iface);
    return InterlockedIncrement(&This->refCount);
}

static ULONG WINAPI Context_Release(ITfContext *iface)
{
    Context *This = impl_from_ITfContext(iface);
    ULONG ret;

    ret = InterlockedDecrement(&This->refCount);
    if (ret == 0)
        Context_Destructor(This);
    return ret;
}

/*****************************************************
 * ITfContext functions
 *****************************************************/
static DWORD edit_session_lock_flags(DWORD flags)
{
    DWORD lock_flags = 0;

    if (flags & TF_ES_SYNC)
        lock_flags |= TS_LF_SYNC;
    if ((flags & TF_ES_READWRITE) == TF_ES_READWRITE)
        lock_flags |= TS_LF_READWRITE;
    else
        lock_flags |= TS_LF_READ;
    return lock_flags;
}

/*
 * Requests made asynchronously from an active edit session must not ask the
 * text store for another lock until the outer callback has returned.  Some
 * stores grant RequestLock synchronously, which would otherwise overwrite the
 * outer session state.
 */
static void context_kick_deferred_requests(Context *context)
{
    EditSessionRequest *request;
    HRESULT session_hr, hr;

    while (!context->editSessionDepth && !list_empty(&context->editSessionRequests))
    {
        request = LIST_ENTRY(context->editSessionRequests.next, EditSessionRequest, entry);
        if (request->lock_requested)
            return;

        request->lock_requested = TRUE;
        request->in_request_call = TRUE;
        session_hr = E_FAIL;
        hr = ITextStoreACP_RequestLock(context->pITextStoreACP,
                edit_session_lock_flags(request->flags), &session_hr);
        request->in_request_call = FALSE;

        if (request->completed)
        {
            free(request);
            continue;
        }
        if (FAILED(hr) || (session_hr != TS_S_ASYNC && !request->granted))
        {
            list_remove(&request->entry);
            ITfEditSession_Release(request->session);
            free(request);
            continue;
        }
        return;
    }
}

static HRESULT WINAPI Context_RequestEditSession (ITfContext *iface,
        TfClientId tid, ITfEditSession *pes, DWORD dwFlags,
        HRESULT *phrSession)
{
    Context *This = impl_from_ITfContext(iface);
    EditSessionRequest *request;
    HRESULT hr;

    TRACE("(%p) %li %p %lx %p\n",This, tid, pes, dwFlags, phrSession);

    if (!pes || !phrSession)
        return E_INVALIDARG;
    *phrSession = E_FAIL;
    if (!This->connected)
        return TF_E_DISCONNECTED;
    if ((dwFlags & TF_ES_SYNC) && (dwFlags & TF_ES_ASYNC))
        return E_INVALIDARG;
    if (!(dwFlags & TF_ES_READ) || (dwFlags & ~(TF_ES_SYNC | TF_ES_ASYNC |
        TF_ES_READWRITE)))
    {
        return E_INVALIDARG;
    }

    if (!This->pITextStoreACP)
    {
        FIXME("No ITextStoreACP available\n");
        return E_FAIL;
    }

    if (This->notifyingTextEditSinks &&
        (dwFlags & TF_ES_READWRITE) == TF_ES_READWRITE && (dwFlags & TF_ES_SYNC))
    {
        *phrSession = TF_E_SYNCHRONOUS;
        return S_OK;
    }

    if (!This->documentStatus.dwDynamicFlags)
        ITextStoreACP_GetStatus(This->pITextStoreACP, &This->documentStatus);

    if (((dwFlags & TF_ES_READWRITE) == TF_ES_READWRITE) && (This->documentStatus.dwDynamicFlags & TS_SD_READONLY))
    {
        *phrSession = TS_E_READONLY;
        return S_OK;
    }

    if (This->editSessionDepth)
    {
        EditCookie *cookie;
        TfEditCookie ec;

        if (This->currentEditSessionTid != tid ||
            ((dwFlags & TF_ES_READWRITE) == TF_ES_READWRITE &&
             (This->currentLockType & TS_LF_READWRITE) != TS_LF_READWRITE))
            return TF_E_LOCKED;
        if (dwFlags & TF_ES_ASYNC)
        {
            if (!(request = calloc(1, sizeof(*request))))
                return E_OUTOFMEMORY;
            request->session = pes;
            ITfEditSession_AddRef(pes);
            request->tid = tid;
            request->flags = dwFlags;
            list_add_tail(&This->editSessionRequests, &request->entry);
            *phrSession = TS_S_ASYNC;
            return S_OK;
        }
        if (!(cookie = malloc(sizeof(*cookie))))
            return E_OUTOFMEMORY;
        cookie->lockType = This->currentLockType;
        cookie->pOwningContext = This;
        if (!(ec = generate_Cookie(COOKIE_MAGIC_EDITCOOKIE, cookie)))
        {
            free(cookie);
            return E_OUTOFMEMORY;
        }
        ++This->editSessionDepth;
        *phrSession = ITfEditSession_DoEditSession(pes, ec);
        --This->editSessionDepth;
        remove_Cookie(ec);
        free(cookie);
        return S_OK;
    }

    if (!(request = calloc(1, sizeof(*request))))
        return E_OUTOFMEMORY;
    request->session = pes;
    ITfEditSession_AddRef(pes);
    request->tid = tid;
    request->flags = dwFlags;
    request->lock_requested = TRUE;
    request->in_request_call = TRUE;
    if (dwFlags & TF_ES_SYNC)
        list_add_head(&This->editSessionRequests, &request->entry);
    else
        list_add_tail(&This->editSessionRequests, &request->entry);

    hr = ITextStoreACP_RequestLock(This->pITextStoreACP,
            edit_session_lock_flags(dwFlags), phrSession);
    request->in_request_call = FALSE;
    if (request->completed || FAILED(hr) ||
        (SUCCEEDED(hr) && *phrSession != TS_S_ASYNC && !request->granted))
    {
        if (!request->completed)
        {
            list_remove(&request->entry);
            ITfEditSession_Release(request->session);
        }
        free(request);
    }
    context_kick_deferred_requests(This);

    return hr;
}

static HRESULT WINAPI Context_InWriteSession (ITfContext *iface,
         TfClientId tid,
         BOOL *pfWriteSession)
{
    Context *This = impl_from_ITfContext(iface);

    if (!pfWriteSession)
        return E_INVALIDARG;
    *pfWriteSession = This->editSessionDepth && This->currentEditSessionTid == tid &&
        (This->currentLockType & TS_LF_READWRITE) == TS_LF_READWRITE;
    return S_OK;
}

static HRESULT WINAPI Context_GetSelection (ITfContext *iface,
        TfEditCookie ec, ULONG ulIndex, ULONG ulCount,
        TF_SELECTION *pSelection, ULONG *pcFetched)
{
    Context *This = impl_from_ITfContext(iface);
    ULONG count, i;
    ULONG totalFetched = 0;
    HRESULT hr = S_OK;

    if (!pSelection || !pcFetched || !ulCount)
        return E_INVALIDARG;

    *pcFetched = 0;

    if (!This->connected)
        return TF_E_DISCONNECTED;

    if (!Context_IsValidCookie(iface, ec, TS_LF_READ))
        return TF_E_NOLOCK;

    if (!This->pITextStoreACP)
    {
        FIXME("Context does not have a ITextStoreACP\n");
        return E_NOTIMPL;
    }

    if (ulIndex == TF_DEFAULT_SELECTION)
        count = 1;
    else
        count = ulCount;

    for (i = 0; i < count; i++)
    {
        DWORD fetched;
        TS_SELECTION_ACP acps;

        hr = ITextStoreACP_GetSelection(This->pITextStoreACP, ulIndex + i,
                1, &acps, &fetched);

        if (hr == TS_E_NOLOCK)
            return TF_E_NOLOCK;
        else if (SUCCEEDED(hr))
        {
            pSelection[totalFetched].style.ase = (TfActiveSelEnd)acps.style.ase;
            pSelection[totalFetched].style.fInterimChar = acps.style.fInterimChar;
            hr = Range_Constructor(iface, acps.acpStart, acps.acpEnd,
                    &pSelection[totalFetched].range);
            if (FAILED(hr))
                break;
            totalFetched ++;
        }
        else
            break;
    }

    *pcFetched = totalFetched;

    return hr;
}

static HRESULT WINAPI Context_SetSelection (ITfContext *iface,
        TfEditCookie ec, ULONG ulCount, const TF_SELECTION *pSelection)
{
    Context *This = impl_from_ITfContext(iface);
    TS_SELECTION_ACP *acp;
    ULONG i;
    HRESULT hr;

    TRACE("(%p) %li %li %p\n",This,ec,ulCount,pSelection);

    if (!pSelection || !ulCount)
        return E_INVALIDARG;
    if (!This->connected)
        return TF_E_DISCONNECTED;
    if (!This->pITextStoreACP)
    {
        FIXME("Context does not have a ITextStoreACP\n");
        return E_NOTIMPL;
    }

    if (!Context_IsValidCookie(iface, ec, TS_LF_READWRITE))
        return TF_E_NOLOCK;

    acp = calloc(ulCount, sizeof(*acp));
    if (!acp)
        return E_OUTOFMEMORY;

    for (i = 0; i < ulCount; i++)
    {
        ITfContext *selection_context;

        if (!pSelection[i].range ||
            FAILED(ITfRange_GetContext(pSelection[i].range, &selection_context)))
        {
            free(acp);
            return E_INVALIDARG;
        }
        if (selection_context != iface)
        {
            ITfContext_Release(selection_context);
            free(acp);
            return TF_E_NOTOWNEDRANGE;
        }
        ITfContext_Release(selection_context);
        if (FAILED(hr = TF_SELECTION_to_TS_SELECTION_ACP(&pSelection[i], &acp[i])))
        {
            TRACE("Selection Conversion Failed\n");
            free(acp);
            return hr;
        }
    }

    hr = ITextStoreACP_SetSelection(This->pITextStoreACP, ulCount, acp);

    free(acp);

    if (SUCCEEDED(hr) && This->activeEditRecord)
        This->activeEditRecord->selection_changed = TRUE;

    return hr;
}

static HRESULT WINAPI Context_GetStart (ITfContext *iface,
        TfEditCookie ec, ITfRange **ppStart)
{
    Context *This = impl_from_ITfContext(iface);

    TRACE("(%p) %li %p\n",This,ec,ppStart);

    if (!ppStart)
        return E_INVALIDARG;

    *ppStart = NULL;

    if (!This->connected)
        return TF_E_DISCONNECTED;

    if (!Context_IsValidCookie(iface, ec, TS_LF_READ))
        return TF_E_NOLOCK;

    return Range_Constructor(iface, 0, 0, ppStart);
}

static HRESULT WINAPI Context_GetEnd (ITfContext *iface,
        TfEditCookie ec, ITfRange **ppEnd)
{
    Context *This = impl_from_ITfContext(iface);
    LONG end;
    HRESULT hr;
    TRACE("(%p) %li %p\n",This,ec,ppEnd);

    if (!ppEnd)
        return E_INVALIDARG;

    *ppEnd = NULL;

    if (!This->connected)
        return TF_E_DISCONNECTED;

    if (!Context_IsValidCookie(iface, ec, TS_LF_READ))
        return TF_E_NOLOCK;

    if (!This->pITextStoreACP)
    {
        FIXME("Context does not have a ITextStoreACP\n");
        return E_NOTIMPL;
    }

    if (FAILED(hr = ITextStoreACP_GetEndACP(This->pITextStoreACP, &end)))
        return hr == TS_E_NOLOCK ? TF_E_NOLOCK : hr;

    return Range_Constructor(iface, end, end, ppEnd);
}

static HRESULT WINAPI Context_GetActiveView (ITfContext *iface,
  ITfContextView **ppView)
{
    Context *This = impl_from_ITfContext(iface);
    FIXME("STUB:(%p)\n",This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Context_EnumViews (ITfContext *iface,
        IEnumTfContextViews **ppEnum)
{
    Context *This = impl_from_ITfContext(iface);
    FIXME("STUB:(%p)\n",This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Context_GetStatus (ITfContext *iface,
        TF_STATUS *pdcs)
{
    Context *This = impl_from_ITfContext(iface);
    HRESULT hr;

    TRACE("(%p) %p\n",This,pdcs);

    if (!This->connected)
        return TF_E_DISCONNECTED;

    if (!pdcs)
        return E_INVALIDARG;

    if (!This->pITextStoreACP)
    {
        FIXME("Context does not have a ITextStoreACP\n");
        return E_NOTIMPL;
    }

    if (FAILED(hr = ITextStoreACP_GetStatus(This->pITextStoreACP, &This->documentStatus)))
        return hr;

    *pdcs = This->documentStatus;

    return S_OK;
}

static HRESULT WINAPI Context_GetProperty (ITfContext *iface,
        REFGUID guidProp, ITfProperty **ppProp)
{
    Context *This = impl_from_ITfContext(iface);
    ContextProperty *property;

    TRACE("%p, %s, %p.\n", This, debugstr_guid(guidProp), ppProp);

    if (!guidProp || !ppProp)
        return E_INVALIDARG;
    *ppProp = NULL;
    if (!This->connected)
        return TF_E_DISCONNECTED;
    property = context_find_property(This, guidProp);
    if (!property && !(property = context_create_property(This, guidProp)))
        return E_OUTOFMEMORY;
    *ppProp = &property->ITfProperty_iface;
    ITfProperty_AddRef(*ppProp);
    return S_OK;
}

static HRESULT WINAPI Context_GetAppProperty (ITfContext *iface,
        REFGUID guidProp, ITfReadOnlyProperty **ppProp)
{
    Context *This = impl_from_ITfContext(iface);
    TRACE("stub: %p, %s, %p.\n", This, debugstr_guid(guidProp), ppProp);
    if (!guidProp || !ppProp)
        return E_INVALIDARG;
    *ppProp = NULL;
    if (!This->connected)
        return TF_E_DISCONNECTED;
    return E_NOTIMPL;
}

static HRESULT WINAPI Context_TrackProperties (ITfContext *iface,
        const GUID **prgProp, ULONG cProp, const GUID **prgAppProp,
        ULONG cAppProp, ITfReadOnlyProperty **ppProperty)
{
    Context *This = impl_from_ITfContext(iface);
    const GUID **properties;
    ULONG count, i;
    HRESULT hr;

    TRACE("%p, %p, %lu, %p, %lu, %p.\n", This, prgProp, cProp,
            prgAppProp, cAppProp, ppProperty);

    if (!ppProperty || (!cProp && !cAppProp) || (cProp && !prgProp) ||
        (cAppProp && !prgAppProp))
        return E_INVALIDARG;
    *ppProperty = NULL;
    if (!This->connected)
        return TF_E_DISCONNECTED;
    if (cProp > ULONG_MAX - cAppProp)
        return E_OUTOFMEMORY;
    count = cProp + cAppProp;
    if (!(properties = calloc(count, sizeof(*properties))))
        return E_OUTOFMEMORY;
    for (i = 0; i < cProp; ++i) properties[i] = prgProp[i];
    for (i = 0; i < cAppProp; ++i) properties[cProp + i] = prgAppProp[i];
    hr = tracking_property_create(This, properties, count, ppProperty);
    free(properties);
    return hr;
}

static HRESULT WINAPI Context_EnumProperties (ITfContext *iface,
        IEnumTfProperties **ppEnum)
{
    Context *This = impl_from_ITfContext(iface);
    FIXME("STUB:(%p)\n",This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Context_GetDocumentMgr (ITfContext *iface,
        ITfDocumentMgr **ppDm)
{
    Context *This = impl_from_ITfContext(iface);
    TRACE("(%p) %p\n",This,ppDm);

    if (!ppDm)
        return E_INVALIDARG;

    *ppDm = This->manager;
    if (!This->manager)
        return S_FALSE;

    ITfDocumentMgr_AddRef(This->manager);

    return S_OK;
}

static HRESULT WINAPI Context_CreateRangeBackup (ITfContext *iface,
        TfEditCookie ec, ITfRange *pRange, ITfRangeBackup **ppBackup)
{
    Context *This = impl_from_ITfContext(iface);
    FIXME("STUB:(%p)\n",This);
    return E_NOTIMPL;
}

static const ITfContextVtbl ContextVtbl =
{
    Context_QueryInterface,
    Context_AddRef,
    Context_Release,
    Context_RequestEditSession,
    Context_InWriteSession,
    Context_GetSelection,
    Context_SetSelection,
    Context_GetStart,
    Context_GetEnd,
    Context_GetActiveView,
    Context_EnumViews,
    Context_GetStatus,
    Context_GetProperty,
    Context_GetAppProperty,
    Context_TrackProperties,
    Context_EnumProperties,
    Context_GetDocumentMgr,
    Context_CreateRangeBackup
};

/*****************************************************
 * ITfSource functions
 *****************************************************/
static HRESULT WINAPI ContextSource_QueryInterface(ITfSource *iface, REFIID iid, LPVOID *ppvOut)
{
    Context *This = impl_from_ITfSource(iface);
    return ITfContext_QueryInterface(&This->ITfContext_iface, iid, ppvOut);
}

static ULONG WINAPI ContextSource_AddRef(ITfSource *iface)
{
    Context *This = impl_from_ITfSource(iface);
    return ITfContext_AddRef(&This->ITfContext_iface);
}

static ULONG WINAPI ContextSource_Release(ITfSource *iface)
{
    Context *This = impl_from_ITfSource(iface);
    return ITfContext_Release(&This->ITfContext_iface);
}

static HRESULT WINAPI ContextSource_AdviseSink(ITfSource *iface,
        REFIID riid, IUnknown *punk, DWORD *pdwCookie)
{
    Context *This = impl_from_ITfSource(iface);

    TRACE("(%p) %s %p %p\n",This,debugstr_guid(riid),punk,pdwCookie);

    if (!riid || !punk || !pdwCookie)
        return E_INVALIDARG;

    if (IsEqualIID(riid, &IID_ITfTextEditSink))
        return advise_sink(&This->pTextEditSink, &IID_ITfTextEditSink, COOKIE_MAGIC_CONTEXTSINK, punk, pdwCookie);

    FIXME("(%p) Unhandled Sink: %s\n",This,debugstr_guid(riid));
    return E_NOTIMPL;
}

static HRESULT WINAPI ContextSource_UnadviseSink(ITfSource *iface, DWORD pdwCookie)
{
    Context *This = impl_from_ITfSource(iface);

    TRACE("(%p) %lx\n",This,pdwCookie);

    if (get_Cookie_magic(pdwCookie)!=COOKIE_MAGIC_CONTEXTSINK)
        return E_INVALIDARG;

    return unadvise_sink(pdwCookie);
}

static const ITfSourceVtbl ContextSourceVtbl =
{
    ContextSource_QueryInterface,
    ContextSource_AddRef,
    ContextSource_Release,
    ContextSource_AdviseSink,
    ContextSource_UnadviseSink
};

/*****************************************************
 * ITfContextOwnerCompositionServices functions
 *****************************************************/
static HRESULT WINAPI ContextOwnerCompositionServices_QueryInterface(ITfContextOwnerCompositionServices *iface,
        REFIID iid, LPVOID *ppvOut)
{
    Context *This = impl_from_ITfContextOwnerCompositionServices(iface);
    return ITfContext_QueryInterface(&This->ITfContext_iface, iid, ppvOut);
}

static ULONG WINAPI ContextOwnerCompositionServices_AddRef(ITfContextOwnerCompositionServices *iface)
{
    Context *This = impl_from_ITfContextOwnerCompositionServices(iface);
    return ITfContext_AddRef(&This->ITfContext_iface);
}

static ULONG WINAPI ContextOwnerCompositionServices_Release(ITfContextOwnerCompositionServices *iface)
{
    Context *This = impl_from_ITfContextOwnerCompositionServices(iface);
    return ITfContext_Release(&This->ITfContext_iface);
}

static HRESULT WINAPI ContextOwnerCompositionServices_StartComposition(ITfContextOwnerCompositionServices *iface,
        TfEditCookie ecWrite, ITfRange *pCompositionRange, ITfCompositionSink *pSink, ITfComposition **ppComposition)
{
    Context *This = impl_from_ITfContextOwnerCompositionServices(iface);
    Composition *composition;
    VARIANT value;
    LONG start, end;
    BOOL accept = TRUE;
    HRESULT hr;

    TRACE("%p, %#lx, %p, %p, %p.\n", This, ecWrite, pCompositionRange, pSink, ppComposition);

    if (!pCompositionRange || !ppComposition)
        return E_INVALIDARG;
    *ppComposition = NULL;
    if (!This->connected)
        return TF_E_DISCONNECTED;
    if (!Context_IsValidCookie(&This->ITfContext_iface, ecWrite, TS_LF_READWRITE))
        return TF_E_NOLOCK;
    if (This->compositionOperation)
        return E_UNEXPECTED;
    if (FAILED(hr = get_owned_range_extent(This, pCompositionRange, &start, &end)))
        return hr;
    if (FAILED(hr = composition_create(This, pCompositionRange, pSink, &composition)))
        return hr;

    This->compositionOperation = TRUE;
    VariantInit(&value);
    V_VT(&value) = VT_I4;
    V_I4(&value) = TRUE;
    if (start != end && FAILED(hr = context_set_property_value(This,
            &GUID_PROP_COMPOSING, composition->range, &value)))
        goto failed;
    if (This->pITfContextOwnerCompositionSink)
    {
        hr = ITfContextOwnerCompositionSink_OnStartComposition(
                This->pITfContextOwnerCompositionSink,
                &composition->ITfCompositionView_iface, &accept);
        if (FAILED(hr))
            goto failed;
    }
    if (!accept)
    {
        context_clear_property(This, &GUID_PROP_COMPOSING, composition->range);
        This->compositionOperation = FALSE;
        ITfComposition_Release(&composition->ITfComposition_iface);
        return S_OK;
    }

    composition->active = TRUE;
    list_add_tail(&This->compositions, &composition->entry);
    ITfComposition_AddRef(&composition->ITfComposition_iface); /* active-list reference */
    *ppComposition = &composition->ITfComposition_iface;
    This->compositionOperation = FALSE;
    return S_OK;

failed:
    context_clear_property(This, &GUID_PROP_COMPOSING, composition->range);
    This->compositionOperation = FALSE;
    ITfComposition_Release(&composition->ITfComposition_iface);
    return hr;
}

static HRESULT WINAPI ContextOwnerCompositionServices_EnumCompositions(ITfContextOwnerCompositionServices *iface,
        IEnumITfCompositionView **ppEnum)
{
    Context *This = impl_from_ITfContextOwnerCompositionServices(iface);
    Composition *composition;
    ITfCompositionView **views;
    ULONG count = 0, i = 0;
    HRESULT hr;

    if (!ppEnum) return E_INVALIDARG;
    *ppEnum = NULL;
    if (!This->connected) return TF_E_DISCONNECTED;
    LIST_FOR_EACH_ENTRY(composition, &This->compositions, Composition, entry)
        ++count;
    if (count && !(views = calloc(count, sizeof(*views))))
        return E_OUTOFMEMORY;
    if (!count) views = NULL;
    LIST_FOR_EACH_ENTRY(composition, &This->compositions, Composition, entry)
        views[i++] = &composition->ITfCompositionView_iface;
    hr = composition_enumerator_create(views, count, 0, ppEnum);
    free(views);
    return hr;
}

static HRESULT WINAPI ContextOwnerCompositionServices_FindComposition(ITfContextOwnerCompositionServices *iface,
        TfEditCookie ecRead, ITfRange *pTestRange, IEnumITfCompositionView **ppEnum)
{
    Context *This = impl_from_ITfContextOwnerCompositionServices(iface);
    Composition *composition;
    ITfCompositionView **views;
    LONG test_start, test_end;
    ULONG count = 0, i = 0;
    HRESULT hr;

    if (!ppEnum) return E_INVALIDARG;
    *ppEnum = NULL;
    if (!This->connected) return TF_E_DISCONNECTED;
    if (!Context_IsValidCookie(&This->ITfContext_iface, ecRead, TS_LF_READ))
        return TF_E_NOLOCK;
    if (pTestRange &&
        FAILED(hr = get_owned_range_extent(This, pTestRange, &test_start, &test_end)))
        return hr;
    LIST_FOR_EACH_ENTRY(composition, &This->compositions, Composition, entry)
    {
        LONG start, end;
        if (!pTestRange ||
            (SUCCEEDED(get_owned_range_extent(This, composition->range, &start, &end)) &&
             (test_start == test_end ? start <= test_start && end >= test_start :
              start < test_end && end > test_start)))
            ++count;
    }
    if (count && !(views = calloc(count, sizeof(*views))))
        return E_OUTOFMEMORY;
    if (!count) views = NULL;
    LIST_FOR_EACH_ENTRY(composition, &This->compositions, Composition, entry)
    {
        LONG start, end;
        if (!pTestRange ||
            (SUCCEEDED(get_owned_range_extent(This, composition->range, &start, &end)) &&
             (test_start == test_end ? start <= test_start && end >= test_start :
              start < test_end && end > test_start)))
            views[i++] = &composition->ITfCompositionView_iface;
    }
    hr = composition_enumerator_create(views, count, 0, ppEnum);
    free(views);
    return hr;
}

static HRESULT WINAPI ContextOwnerCompositionServices_TakeOwnership(ITfContextOwnerCompositionServices *iface,
        TfEditCookie ecWrite, ITfCompositionView *pComposition, ITfCompositionSink *pSink, ITfComposition **ppComposition)
{
    Context *This = impl_from_ITfContextOwnerCompositionServices(iface);

    TRACE("%p, %#lx, %p, %p, %p.\n", This, ecWrite, pComposition, pSink, ppComposition);
    return E_NOTIMPL;
}

static HRESULT WINAPI ContextOwnerCompositionServices_TerminateComposition(ITfContextOwnerCompositionServices *iface,
        ITfCompositionView *pComposition)
{
    Context *This = impl_from_ITfContextOwnerCompositionServices(iface);
    ITfEditSession *session;
    HRESULT session_hr, hr;

    if (!This->connected)
        return TF_E_DISCONNECTED;
    if (This->compositionOperation)
        return E_UNEXPECTED;
    if (This->editSessionDepth)
        return TF_E_NOLOCK;
    if (pComposition)
    {
        Composition *composition;

        if (pComposition->lpVtbl != &CompositionViewVtbl)
            return E_INVALIDARG;
        composition = impl_from_ITfCompositionView(pComposition);
        if (composition->context != This || !composition->active)
            return E_INVALIDARG;
    }
    else if (list_empty(&This->compositions))
        return S_OK;

    if (FAILED(hr = terminate_composition_session_create(This, pComposition, &session)))
        return hr;
    session_hr = E_FAIL;
    hr = ITfContext_RequestEditSession(&This->ITfContext_iface, This->tidOwner, session,
            TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
    ITfEditSession_Release(session);
    if (FAILED(hr))
        return hr;
    return session_hr;
}

static const ITfContextOwnerCompositionServicesVtbl ContextOwnerCompositionServicesVtbl =
{
    ContextOwnerCompositionServices_QueryInterface,
    ContextOwnerCompositionServices_AddRef,
    ContextOwnerCompositionServices_Release,
    ContextOwnerCompositionServices_StartComposition,
    ContextOwnerCompositionServices_EnumCompositions,
    ContextOwnerCompositionServices_FindComposition,
    ContextOwnerCompositionServices_TakeOwnership,
    ContextOwnerCompositionServices_TerminateComposition
};

/*****************************************************
 * ITfInsertAtSelection functions
 *****************************************************/
static HRESULT WINAPI InsertAtSelection_QueryInterface(ITfInsertAtSelection *iface, REFIID iid, LPVOID *ppvOut)
{
    Context *This = impl_from_ITfInsertAtSelection(iface);
    return ITfContext_QueryInterface(&This->ITfContext_iface, iid, ppvOut);
}

static ULONG WINAPI InsertAtSelection_AddRef(ITfInsertAtSelection *iface)
{
    Context *This = impl_from_ITfInsertAtSelection(iface);
    return ITfContext_AddRef(&This->ITfContext_iface);
}

static ULONG WINAPI InsertAtSelection_Release(ITfInsertAtSelection *iface)
{
    Context *This = impl_from_ITfInsertAtSelection(iface);
    return ITfContext_Release(&This->ITfContext_iface);
}

static HRESULT WINAPI InsertAtSelection_InsertTextAtSelection(
        ITfInsertAtSelection *iface, TfEditCookie ec, DWORD dwFlags,
        const WCHAR *pchText, LONG cch, ITfRange **ppRange)
{
    Context *This = impl_from_ITfInsertAtSelection(iface);
    LONG acpStart = 0, acpEnd = 0;
    TS_TEXTCHANGE change = {0};
    HRESULT hr;

    TRACE("(%p) %li %lx %s %p\n",This, ec, dwFlags, debugstr_wn(pchText,cch), ppRange);

    if (!This->connected)
        return TF_E_DISCONNECTED;

    if (!Context_IsValidCookie(&This->ITfContext_iface, ec, TS_LF_READWRITE))
        return TF_E_NOLOCK;

    if (!This->pITextStoreACP)
    {
        FIXME("Context does not have a ITextStoreACP\n");
        return E_NOTIMPL;
    }

    hr = ITextStoreACP_InsertTextAtSelection(This->pITextStoreACP, dwFlags, pchText, cch, &acpStart, &acpEnd, &change);
    if (SUCCEEDED(hr))
    {
        Context_RecordTextChange(&This->ITfContext_iface, &change);
        if (ppRange)
            Range_Constructor(&This->ITfContext_iface, change.acpStart, change.acpNewEnd, ppRange);
    }

    return hr;
}

static HRESULT WINAPI InsertAtSelection_InsertEmbeddedAtSelection(
        ITfInsertAtSelection *iface, TfEditCookie ec, DWORD dwFlags,
        IDataObject *pDataObject, ITfRange **ppRange)
{
    Context *This = impl_from_ITfInsertAtSelection(iface);
    FIXME("STUB:(%p)\n",This);
    return E_NOTIMPL;
}

static const ITfInsertAtSelectionVtbl InsertAtSelectionVtbl =
{
    InsertAtSelection_QueryInterface,
    InsertAtSelection_AddRef,
    InsertAtSelection_Release,
    InsertAtSelection_InsertTextAtSelection,
    InsertAtSelection_InsertEmbeddedAtSelection,
};

/*****************************************************
 * ITfSourceSingle functions
 *****************************************************/
static HRESULT WINAPI SourceSingle_QueryInterface(ITfSourceSingle *iface, REFIID iid, LPVOID *ppvOut)
{
    Context *This = impl_from_ITfSourceSingle(iface);
    return ITfContext_QueryInterface(&This->ITfContext_iface, iid, ppvOut);
}

static ULONG WINAPI SourceSingle_AddRef(ITfSourceSingle *iface)
{
    Context *This = impl_from_ITfSourceSingle(iface);
    return ITfContext_AddRef(&This->ITfContext_iface);
}

static ULONG WINAPI SourceSingle_Release(ITfSourceSingle *iface)
{
    Context *This = impl_from_ITfSourceSingle(iface);
    return ITfContext_Release(&This->ITfContext_iface);
}

static HRESULT WINAPI SourceSingle_AdviseSingleSink( ITfSourceSingle *iface,
    TfClientId tid, REFIID riid, IUnknown *punk)
{
    Context *This = impl_from_ITfSourceSingle(iface);
    FIXME("STUB:(%p) %li %s %p\n",This, tid, debugstr_guid(riid),punk);
    return E_NOTIMPL;
}

static HRESULT WINAPI SourceSingle_UnadviseSingleSink( ITfSourceSingle *iface,
    TfClientId tid, REFIID riid)
{
    Context *This = impl_from_ITfSourceSingle(iface);
    FIXME("STUB:(%p) %li %s\n",This, tid, debugstr_guid(riid));
    return E_NOTIMPL;
}

static const ITfSourceSingleVtbl ContextSourceSingleVtbl =
{
    SourceSingle_QueryInterface,
    SourceSingle_AddRef,
    SourceSingle_Release,
    SourceSingle_AdviseSingleSink,
    SourceSingle_UnadviseSingleSink,
};

/**************************************************************************
 *  ITextStoreACPSink
 **************************************************************************/

static HRESULT WINAPI TextStoreACPSink_QueryInterface(ITextStoreACPSink *iface, REFIID iid, LPVOID *ppvOut)
{
    Context *This = impl_from_ITextStoreACPSink(iface);

    *ppvOut = NULL;

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITextStoreACPSink))
    {
        *ppvOut = &This->ITextStoreACPSink_iface;
    }
    else if (IsEqualIID(iid, &IID_ITextStoreACPServices))
        *ppvOut = &This->ITextStoreACPServices_iface;

    if (*ppvOut)
    {
        ITextStoreACPSink_AddRef(iface);
        return S_OK;
    }

    WARN("unsupported interface: %s\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI TextStoreACPSink_AddRef(ITextStoreACPSink *iface)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    return ITfContext_AddRef(&This->ITfContext_iface);
}

static ULONG WINAPI TextStoreACPSink_Release(ITextStoreACPSink *iface)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    return ITfContext_Release(&This->ITfContext_iface);
}

/*****************************************************
 * ITextStoreACPSink functions
 *****************************************************/

static HRESULT WINAPI TextStoreACPSink_OnTextChange(ITextStoreACPSink *iface,
        DWORD dwFlags, const TS_TEXTCHANGE *pChange)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    EditRecord *record;

    TRACE("(%p) flags %#lx change %p\n", This, dwFlags, pChange);
    if (!pChange)
        return E_INVALIDARG;
    if (This->activeEditRecord)
    {
        Context_RecordTextChange(&This->ITfContext_iface, pChange);
        return S_OK;
    }
    if (!(record = edit_record_create(This)))
        return E_OUTOFMEMORY;
    This->activeEditRecord = record;
    Context_RecordTextChange(&This->ITfContext_iface, pChange);
    This->activeEditRecord = NULL;
    context_notify_text_edit_sinks(This, record, This->defaultCookie);
    ITfEditRecord_Release(&record->ITfEditRecord_iface);
    return S_OK;
}

static HRESULT WINAPI TextStoreACPSink_OnSelectionChange(ITextStoreACPSink *iface)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    EditRecord *record;

    TRACE("(%p)\n", This);
    if (This->activeEditRecord)
    {
        This->activeEditRecord->selection_changed = TRUE;
        return S_OK;
    }
    if (!(record = edit_record_create(This)))
        return E_OUTOFMEMORY;
    record->selection_changed = TRUE;
    context_notify_text_edit_sinks(This, record, This->defaultCookie);
    ITfEditRecord_Release(&record->ITfEditRecord_iface);
    return S_OK;
}

static HRESULT WINAPI TextStoreACPSink_OnLayoutChange(ITextStoreACPSink *iface,
    TsLayoutCode lcode, TsViewCookie vcView)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    TRACE("(%p) code %u view %lu\n", This, lcode, vcView);
    return S_OK;
}

static HRESULT WINAPI TextStoreACPSink_OnStatusChange(ITextStoreACPSink *iface,
        DWORD dwFlags)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    HRESULT hr, hrSession;

    TRACE("(%p) %lx\n",This, dwFlags);

    if (!This->pITextStoreACP)
    {
        FIXME("Context does not have a ITextStoreACP\n");
        return E_NOTIMPL;
    }

    hr = ITextStoreACP_RequestLock(This->pITextStoreACP, TS_LF_READ, &hrSession);

    if(SUCCEEDED(hr) && SUCCEEDED(hrSession))
        This->documentStatus.dwDynamicFlags = dwFlags;

    return S_OK;
}

static HRESULT WINAPI TextStoreACPSink_OnAttrsChange(ITextStoreACPSink *iface,
        LONG acpStart, LONG acpEnd, ULONG cAttrs, const TS_ATTRID *paAttrs)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    EditRecord *record;
    ULONG i;

    TRACE("%p, %ld, %ld, %lu, %p.\n", This, acpStart, acpEnd, cAttrs, paAttrs);
    if (cAttrs && !paAttrs)
        return E_INVALIDARG;
    record = This->activeEditRecord;
    if (!record && !(record = edit_record_create(This)))
        return E_OUTOFMEMORY;
    for (i = 0; i < cAttrs; ++i)
        edit_record_add_property_change(record, &paAttrs[i], acpStart, acpEnd);
    if (!This->activeEditRecord)
    {
        context_notify_text_edit_sinks(This, record, This->defaultCookie);
        ITfEditRecord_Release(&record->ITfEditRecord_iface);
    }
    return S_OK;
}

static HRESULT WINAPI TextStoreACPSink_OnLockGranted(ITextStoreACPSink *iface,
        DWORD dwLockFlags)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    EditSessionRequest *request;
    EditCookie *cookie, *sinkcookie;
    EditRecord *record = NULL;
    TfEditCookie ec = 0, sink_ec = 0;
    HRESULT hr = E_FAIL;

    TRACE("(%p) %lx\n",This, dwLockFlags);

    if (list_empty(&This->editSessionRequests))
    {
        FIXME("OnLockGranted called for something other than an EditSession\n");
        return S_OK;
    }
    request = LIST_ENTRY(This->editSessionRequests.next, EditSessionRequest, entry);
    list_remove(&request->entry);
    request->granted = TRUE;

    cookie = malloc(sizeof(EditCookie));
    if (!cookie)
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    cookie->lockType = dwLockFlags;
    cookie->pOwningContext = This;
    ec = generate_Cookie(COOKIE_MAGIC_EDITCOOKIE, cookie);
    if (!ec)
    {
        free(cookie);
        hr = E_OUTOFMEMORY;
        goto done;
    }

    This->currentEditSession = request->session;
    This->currentEditSessionTid = request->tid;
    This->currentLockType = dwLockFlags;
    This->editSessionDepth = 1;
    if ((dwLockFlags & TS_LF_READWRITE) == TS_LF_READWRITE)
    {
        if (!(record = edit_record_create(This)))
        {
            hr = E_OUTOFMEMORY;
            goto session_done;
        }
        This->activeEditRecord = record;
    }

    hr = ITfEditSession_DoEditSession(request->session, ec);

session_done:
    This->activeEditRecord = NULL;
    This->editSessionDepth = 0;
    This->currentLockType = 0;
    This->currentEditSessionTid = 0;
    This->currentEditSession = NULL;
    if ((dwLockFlags&TS_LF_READWRITE) == TS_LF_READWRITE)
    {
        if (record && (sinkcookie = malloc(sizeof(*sinkcookie))))
        {
            sinkcookie->lockType = TS_LF_READ;
            sinkcookie->pOwningContext = This;
            if ((sink_ec = generate_Cookie(COOKIE_MAGIC_EDITCOOKIE, sinkcookie)))
            {
                context_notify_text_edit_sinks(This, record, sink_ec);
                remove_Cookie(sink_ec);
            }
            free(sinkcookie);
        }
        if (record) ITfEditRecord_Release(&record->ITfEditRecord_iface);
    }

    /* Edit Cookie is only valid during the edit session */
    if (ec)
    {
        cookie = remove_Cookie(ec);
        free(cookie);
    }

done:
    ITfEditSession_Release(request->session);
    request->completed = TRUE;
    if (!request->in_request_call)
    {
        free(request);
        context_kick_deferred_requests(This);
    }
    return hr;
}

static HRESULT WINAPI TextStoreACPSink_OnStartEditTransaction(ITextStoreACPSink *iface)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    FIXME("STUB:(%p)\n",This);
    return E_NOTIMPL;
}

static HRESULT WINAPI TextStoreACPSink_OnEndEditTransaction(ITextStoreACPSink *iface)
{
    Context *This = impl_from_ITextStoreACPSink(iface);
    FIXME("STUB:(%p)\n",This);
    return E_NOTIMPL;
}

static const ITextStoreACPSinkVtbl TextStoreACPSinkVtbl =
{
    TextStoreACPSink_QueryInterface,
    TextStoreACPSink_AddRef,
    TextStoreACPSink_Release,
    TextStoreACPSink_OnTextChange,
    TextStoreACPSink_OnSelectionChange,
    TextStoreACPSink_OnLayoutChange,
    TextStoreACPSink_OnStatusChange,
    TextStoreACPSink_OnAttrsChange,
    TextStoreACPSink_OnLockGranted,
    TextStoreACPSink_OnStartEditTransaction,
    TextStoreACPSink_OnEndEditTransaction
};

static HRESULT WINAPI TextStoreACPServices_QueryInterface(ITextStoreACPServices *iface, REFIID riid, void **obj)
{
    Context *This = impl_from_ITextStoreACPServices(iface);
    return ITextStoreACPSink_QueryInterface(&This->ITextStoreACPSink_iface, riid, obj);
}

static ULONG WINAPI TextStoreACPServices_AddRef(ITextStoreACPServices *iface)
{
    Context *This = impl_from_ITextStoreACPServices(iface);
    return ITextStoreACPSink_AddRef(&This->ITextStoreACPSink_iface);
}

static ULONG WINAPI TextStoreACPServices_Release(ITextStoreACPServices *iface)
{
    Context *This = impl_from_ITextStoreACPServices(iface);
    return ITextStoreACPSink_Release(&This->ITextStoreACPSink_iface);
}

static HRESULT WINAPI TextStoreACPServices_Serialize(ITextStoreACPServices *iface, ITfProperty *prop, ITfRange *range,
    TF_PERSISTENT_PROPERTY_HEADER_ACP *header, IStream *stream)
{
    Context *This = impl_from_ITextStoreACPServices(iface);

    FIXME("stub: %p %p %p %p %p\n", This, prop, range, header, stream);

    return E_NOTIMPL;
}

static HRESULT WINAPI TextStoreACPServices_Unserialize(ITextStoreACPServices *iface, ITfProperty *prop,
    const TF_PERSISTENT_PROPERTY_HEADER_ACP *header, IStream *stream, ITfPersistentPropertyLoaderACP *loader)
{
    Context *This = impl_from_ITextStoreACPServices(iface);

    FIXME("stub: %p %p %p %p %p\n", This, prop, header, stream, loader);

    return E_NOTIMPL;
}

static HRESULT WINAPI TextStoreACPServices_ForceLoadProperty(ITextStoreACPServices *iface, ITfProperty *prop)
{
    Context *This = impl_from_ITextStoreACPServices(iface);

    FIXME("stub: %p %p\n", This, prop);

    return E_NOTIMPL;
}

static HRESULT WINAPI TextStoreACPServices_CreateRange(ITextStoreACPServices *iface,
    LONG start, LONG end, ITfRangeACP **range)
{
    Context *This = impl_from_ITextStoreACPServices(iface);

    TRACE("%p, %ld, %ld, %p.\n", This, start, end, range);

    return Range_Constructor(&This->ITfContext_iface, start, end, (ITfRange **)range);
}

static const ITextStoreACPServicesVtbl TextStoreACPServicesVtbl =
{
    TextStoreACPServices_QueryInterface,
    TextStoreACPServices_AddRef,
    TextStoreACPServices_Release,
    TextStoreACPServices_Serialize,
    TextStoreACPServices_Unserialize,
    TextStoreACPServices_ForceLoadProperty,
    TextStoreACPServices_CreateRange
};

HRESULT Context_Constructor(TfClientId tidOwner, IUnknown *punk, ITfDocumentMgr *mgr, ITfContext **ppOut, TfEditCookie *pecTextStore)
{
    Context *This;
    EditCookie *cookie;
    HRESULT hr;

    if (!ppOut || !pecTextStore)
        return E_INVALIDARG;
    *ppOut = NULL;
    *pecTextStore = 0;

    This = calloc(1, sizeof(Context));
    if (This == NULL)
        return E_OUTOFMEMORY;

    TRACE("(%p) %lx %p %p %p\n",This, tidOwner, punk, ppOut, pecTextStore);

    This->ITfContext_iface.lpVtbl= &ContextVtbl;
    This->ITfSource_iface.lpVtbl = &ContextSourceVtbl;
    This->ITfContextOwnerCompositionServices_iface.lpVtbl = &ContextOwnerCompositionServicesVtbl;
    This->ITfInsertAtSelection_iface.lpVtbl = &InsertAtSelectionVtbl;
    This->ITfSourceSingle_iface.lpVtbl = &ContextSourceSingleVtbl;
    This->ITextStoreACPSink_iface.lpVtbl = &TextStoreACPSinkVtbl;
    This->ITextStoreACPServices_iface.lpVtbl = &TextStoreACPServicesVtbl;
    This->refCount = 1;
    This->tidOwner = tidOwner;
    This->connected = FALSE;
    This->manager = mgr;

    list_init(&This->pContextKeyEventSink);
    list_init(&This->pEditTransactionSink);
    list_init(&This->pStatusSink);
    list_init(&This->pTextEditSink);
    list_init(&This->pTextLayoutSink);
    list_init(&This->editSessionRequests);
    list_init(&This->properties);
    list_init(&This->compositions);

    if (FAILED(hr = CompartmentMgr_Constructor((IUnknown *)&This->ITfContext_iface,
            &IID_IUnknown, (IUnknown **)&This->CompartmentMgr)))
        goto failed;

    if (punk)
    {
        IUnknown_QueryInterface(punk, &IID_ITextStoreACP,
                          (LPVOID*)&This->pITextStoreACP);

        IUnknown_QueryInterface(punk, &IID_ITfContextOwnerCompositionSink,
                                (LPVOID*)&This->pITfContextOwnerCompositionSink);

        if (!This->pITextStoreACP && !This->pITfContextOwnerCompositionSink)
            FIXME("Unhandled pUnk\n");
    }

    if (!(cookie = malloc(sizeof(*cookie))))
    {
        hr = E_OUTOFMEMORY;
        goto failed;
    }
    cookie->lockType = TS_LF_READ;
    cookie->pOwningContext = This;
    This->defaultCookie = generate_Cookie(COOKIE_MAGIC_EDITCOOKIE,cookie);
    if (!This->defaultCookie)
    {
        free(cookie);
        hr = E_OUTOFMEMORY;
        goto failed;
    }
    *pecTextStore = This->defaultCookie;

    *ppOut = &This->ITfContext_iface;
    TRACE("returning %p\n", *ppOut);

    return S_OK;

failed:
    if (This->pITfContextOwnerCompositionSink)
        ITfContextOwnerCompositionSink_Release(This->pITfContextOwnerCompositionSink);
    if (This->pITextStoreACP)
        ITextStoreACP_Release(This->pITextStoreACP);
    if (This->CompartmentMgr)
        CompartmentMgr_Destructor(This->CompartmentMgr);
    free(This);
    return hr;
}

HRESULT Context_Initialize(ITfContext *iface, ITfDocumentMgr *manager)
{
    Context *This = impl_from_ITfContext(iface);
    HRESULT hr;

    if (This->pITextStoreACP)
        if (FAILED(hr = ITextStoreACP_AdviseSink(This->pITextStoreACP,
                &IID_ITextStoreACPSink, (IUnknown *)&This->ITextStoreACPSink_iface,
                TS_AS_ALL_SINKS)))
            return hr;
    This->connected = TRUE;
    This->manager = manager;
    return S_OK;
}

HRESULT Context_Uninitialize(ITfContext *iface)
{
    Context *This = impl_from_ITfContext(iface);
    Composition *composition, *next;
    EditCookie cookie;
    TfEditCookie ec = 0;

    if (!This->connected)
        return S_OK;
    if (FAILED(ContextOwnerCompositionServices_TerminateComposition(
            &This->ITfContextOwnerCompositionServices_iface, NULL)) &&
        !list_empty(&This->compositions))
    {
        cookie.lockType = TS_LF_READWRITE;
        cookie.pOwningContext = This;
        if ((ec = generate_Cookie(COOKIE_MAGIC_EDITCOOKIE, &cookie)))
        {
            LIST_FOR_EACH_ENTRY_SAFE(composition, next, &This->compositions,
                    Composition, entry)
                composition_finish(composition, ec, TRUE);
            remove_Cookie(ec);
        }
    }
    if (This->pITextStoreACP)
        ITextStoreACP_UnadviseSink(This->pITextStoreACP, (IUnknown*)&This->ITextStoreACPSink_iface);
    This->connected = FALSE;
    This->manager = NULL;
    return S_OK;
}
