/*
 *  ITfRange implementation
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

#define COBJMACROS

#include "wine/debug.h"
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winuser.h"
#include "shlwapi.h"
#include "winerror.h"
#include "objbase.h"

#include "msctf.h"
#include "msctf_internal.h"

WINE_DEFAULT_DEBUG_CHANNEL(msctf);

typedef struct tagRange {
    ITfRangeACP ITfRangeACP_iface;
    LONG refCount;

    ITfContext *context;

    TfGravity gravityStart, gravityEnd;
    LONG anchorStart, anchorEnd;

} Range;

static inline Range *impl_from_ITfRangeACP(ITfRangeACP *iface)
{
    return CONTAINING_RECORD(iface, Range, ITfRangeACP_iface);
}

static Range *unsafe_impl_from_ITfRange(ITfRange *iface)
{
    return CONTAINING_RECORD(iface, Range, ITfRangeACP_iface);
}

static HRESULT range_get_anchor(Range *range, ITfRange *other_range, TfAnchor position,
        LONG *anchor)
{
    ITfRangeACP *other_acp;
    ITfContext *other_context;
    LONG start, count;
    HRESULT hr;

    if (!other_range || !anchor ||
        (position != TF_ANCHOR_START && position != TF_ANCHOR_END))
        return E_INVALIDARG;
    if (FAILED(hr = ITfRange_GetContext(other_range, &other_context)))
        return hr;
    if (other_context != range->context)
    {
        ITfContext_Release(other_context);
        return TF_E_NOTOWNEDRANGE;
    }
    ITfContext_Release(other_context);
    if (FAILED(hr = ITfRange_QueryInterface(other_range, &IID_ITfRangeACP,
            (void **)&other_acp)))
        return hr;
    hr = ITfRangeACP_GetExtent(other_acp, &start, &count);
    ITfRangeACP_Release(other_acp);
    if (FAILED(hr))
        return hr;
    if (start < 0 || count < 0 || start > LONG_MAX - count)
        return E_INVALIDARG;
    *anchor = position == TF_ANCHOR_END ? start + count : start;
    return S_OK;
}

static void Range_Destructor(Range *This)
{
    TRACE("destroying %p\n", This);
    ITfContext_Release(This->context);
    free(This);
}

static HRESULT WINAPI Range_QueryInterface(ITfRangeACP *iface, REFIID iid, LPVOID *ppvOut)
{
    Range *range = impl_from_ITfRangeACP(iface);

    if (!ppvOut)
        return E_INVALIDARG;
    *ppvOut = NULL;

    if (IsEqualIID(iid, &IID_IUnknown) ||
            IsEqualIID(iid, &IID_ITfRange) ||
            IsEqualIID(iid, &IID_ITfRangeACP))
    {
        *ppvOut = &range->ITfRangeACP_iface;
    }

    if (*ppvOut)
    {
        ITfRangeACP_AddRef(iface);
        return S_OK;
    }

    WARN("unsupported interface: %s\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI Range_AddRef(ITfRangeACP *iface)
{
    Range *range = impl_from_ITfRangeACP(iface);
    return InterlockedIncrement(&range->refCount);
}

static ULONG WINAPI Range_Release(ITfRangeACP *iface)
{
    Range *range = impl_from_ITfRangeACP(iface);
    ULONG ret;

    ret = InterlockedDecrement(&range->refCount);
    if (ret == 0)
        Range_Destructor(range);
    return ret;
}

static HRESULT WINAPI Range_GetText(ITfRangeACP *iface, TfEditCookie ec,
        DWORD dwFlags, WCHAR *pchText, ULONG cchMax, ULONG *pcch)
{
    Range *range = impl_from_ITfRangeACP(iface);
    ITextStoreACP *store;
    TS_RUNINFO run;
    ULONG runs = 0;
    LONG next;
    HRESULT hr;

    TRACE("%p, %#lx, %#lx, %p, %lu, %p.\n", iface, ec, dwFlags, pchText, cchMax, pcch);

    if (!pcch || (!pchText && cchMax) ||
        (dwFlags & ~(TF_TF_MOVESTART | TF_TF_IGNOREEND)))
        return E_INVALIDARG;

    *pcch = 0;
    if (FAILED(hr = Context_GetTextStore(range->context, ec, TS_LF_READ, &store)))
        return hr;

    next = range->anchorStart;
    hr = ITextStoreACP_GetText(store, range->anchorStart,
            (dwFlags & TF_TF_IGNOREEND) ? -1 : range->anchorEnd, pchText, cchMax, pcch,
            &run, 1, &runs, &next);
    ITextStoreACP_Release(store);

    if (SUCCEEDED(hr) && (dwFlags & TF_TF_MOVESTART))
    {
        range->anchorStart = next;
        if (range->anchorStart > range->anchorEnd)
            range->anchorEnd = range->anchorStart;
    }
    return hr;
}

static HRESULT WINAPI Range_SetText(ITfRangeACP *iface, TfEditCookie ec,
         DWORD dwFlags, const WCHAR *pchText, LONG cch)
{
    Range *range = impl_from_ITfRangeACP(iface);
    ITextStoreACP *store;
    TS_TEXTCHANGE change;
    HRESULT hr;

    TRACE("%p, %#lx, %#lx, %s, %ld.\n", iface, ec, dwFlags, debugstr_wn(pchText, cch), cch);

    if (cch < 0 || (!pchText && cch) || (dwFlags & ~TF_ST_CORRECTION))
        return E_INVALIDARG;
    if (FAILED(hr = Context_GetTextStore(range->context, ec, TS_LF_READWRITE, &store)))
        return hr;

    hr = ITextStoreACP_SetText(store, (dwFlags & TF_ST_CORRECTION) ? TS_ST_CORRECTION : 0,
            range->anchorStart, range->anchorEnd, pchText, cch, &change);
    ITextStoreACP_Release(store);
    if (SUCCEEDED(hr))
    {
        range->anchorStart = change.acpStart;
        range->anchorEnd = change.acpNewEnd;
        Context_RecordTextChange(range->context, &change);
    }
    return hr;
}

static HRESULT WINAPI Range_GetFormattedText(ITfRangeACP *iface, TfEditCookie ec,
        IDataObject **ppDataObject)
{
    FIXME("STUB:(%p)\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI Range_GetEmbedded(ITfRangeACP *iface, TfEditCookie ec,
        REFGUID rguidService, REFIID riid, IUnknown **ppunk)
{
    FIXME("STUB:(%p)\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI Range_InsertEmbedded(ITfRangeACP *iface, TfEditCookie ec,
        DWORD dwFlags, IDataObject *pDataObject)
{
    FIXME("STUB:(%p)\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI Range_ShiftStart(ITfRangeACP *iface, TfEditCookie ec,
        LONG cchReq, LONG *pcch, const TF_HALTCOND *pHalt)
{
    Range *range = impl_from_ITfRangeACP(iface);
    ITextStoreACP *store;
    LONG old, end, target;
    LONGLONG requested;
    HRESULT hr;

    TRACE("%p, %#lx, %ld, %p, %p.\n", iface, ec, cchReq, pcch, pHalt);

    if (!pcch)
        return E_INVALIDARG;
    *pcch = 0;
    if (FAILED(hr = Context_GetTextStore(range->context, ec, TS_LF_READ, &store)))
        return hr;
    hr = ITextStoreACP_GetEndACP(store, &end);
    ITextStoreACP_Release(store);
    if (FAILED(hr))
        return hr;

    old = range->anchorStart;
    requested = (LONGLONG)old + cchReq;
    target = requested < 0 ? 0 : requested > end ? end : requested;
    if (pHalt && pHalt->pHaltRange)
    {
        LONG anchor;

        if (FAILED(hr = range_get_anchor(range, pHalt->pHaltRange,
                pHalt->aHaltPos, &anchor)))
            return hr;
        if (cchReq < 0 && target < anchor && old >= anchor) target = anchor;
        if (cchReq > 0 && target > anchor && old <= anchor) target = anchor;
    }

    range->anchorStart = target;
    if (range->anchorStart > range->anchorEnd)
        range->anchorEnd = range->anchorStart;
    *pcch = target - old;
    return S_OK;
}

static HRESULT WINAPI Range_ShiftEnd(ITfRangeACP *iface, TfEditCookie ec,
        LONG cchReq, LONG *pcch, const TF_HALTCOND *pHalt)
{
    Range *range = impl_from_ITfRangeACP(iface);
    ITextStoreACP *store;
    LONG old, end, target;
    LONGLONG requested;
    HRESULT hr;

    TRACE("%p, %#lx, %ld, %p, %p.\n", iface, ec, cchReq, pcch, pHalt);

    if (!pcch)
        return E_INVALIDARG;
    *pcch = 0;
    if (FAILED(hr = Context_GetTextStore(range->context, ec, TS_LF_READ, &store)))
        return hr;
    hr = ITextStoreACP_GetEndACP(store, &end);
    ITextStoreACP_Release(store);
    if (FAILED(hr))
        return hr;

    old = range->anchorEnd;
    requested = (LONGLONG)old + cchReq;
    target = requested < 0 ? 0 : requested > end ? end : requested;
    if (pHalt && pHalt->pHaltRange)
    {
        LONG anchor;

        if (FAILED(hr = range_get_anchor(range, pHalt->pHaltRange,
                pHalt->aHaltPos, &anchor)))
            return hr;
        if (cchReq < 0 && target < anchor && old >= anchor) target = anchor;
        if (cchReq > 0 && target > anchor && old <= anchor) target = anchor;
    }

    range->anchorEnd = target;
    if (range->anchorEnd < range->anchorStart)
        range->anchorStart = range->anchorEnd;
    *pcch = target - old;
    return S_OK;
}

static HRESULT WINAPI Range_ShiftStartToRange(ITfRangeACP *iface, TfEditCookie ec,
        ITfRange *pRange, TfAnchor aPos)
{
    Range *range = impl_from_ITfRangeACP(iface);
    LONG anchor;
    HRESULT hr;

    if (!pRange || (aPos != TF_ANCHOR_START && aPos != TF_ANCHOR_END))
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    if (FAILED(hr = range_get_anchor(range, pRange, aPos, &anchor)))
        return hr;

    range->anchorStart = anchor;
    if (range->anchorStart > range->anchorEnd)
        range->anchorEnd = range->anchorStart;
    return S_OK;
}

static HRESULT WINAPI Range_ShiftEndToRange(ITfRangeACP *iface, TfEditCookie ec,
        ITfRange *pRange, TfAnchor aPos)
{
    Range *range = impl_from_ITfRangeACP(iface);
    LONG anchor;
    HRESULT hr;

    if (!pRange || (aPos != TF_ANCHOR_START && aPos != TF_ANCHOR_END))
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    if (FAILED(hr = range_get_anchor(range, pRange, aPos, &anchor)))
        return hr;

    range->anchorEnd = anchor;
    if (range->anchorEnd < range->anchorStart)
        range->anchorStart = range->anchorEnd;
    return S_OK;
}

static HRESULT WINAPI Range_ShiftStartRegion(ITfRangeACP *iface, TfEditCookie ec,
        TfShiftDir dir, BOOL *pfNoRegion)
{
    Range *range = impl_from_ITfRangeACP(iface);

    if (!pfNoRegion)
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    *pfNoRegion = TRUE;
    return S_OK;
}

static HRESULT WINAPI Range_ShiftEndRegion(ITfRangeACP *iface, TfEditCookie ec,
        TfShiftDir dir, BOOL *pfNoRegion)
{
    Range *range = impl_from_ITfRangeACP(iface);

    if (!pfNoRegion)
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    *pfNoRegion = TRUE;
    return S_OK;
}

static HRESULT WINAPI Range_IsEmpty(ITfRangeACP *iface, TfEditCookie ec,
        BOOL *pfEmpty)
{
    Range *range = impl_from_ITfRangeACP(iface);

    if (!pfEmpty)
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    *pfEmpty = range->anchorStart == range->anchorEnd;
    return S_OK;
}

static HRESULT WINAPI Range_Collapse(ITfRangeACP *iface, TfEditCookie ec,
        TfAnchor aPos)
{
    Range *range = impl_from_ITfRangeACP(iface);

    TRACE("%p, %li, %i.\n", iface, ec, aPos);

    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;

    switch (aPos)
    {
        case TF_ANCHOR_START:
            range->anchorEnd = range->anchorStart;
            break;
        case TF_ANCHOR_END:
            range->anchorStart = range->anchorEnd;
            break;
        default:
            return E_INVALIDARG;
    }

    return S_OK;
}

static HRESULT WINAPI Range_IsEqualStart(ITfRangeACP *iface, TfEditCookie ec,
        ITfRange *pWith, TfAnchor aPos, BOOL *pfEqual)
{
    Range *range = impl_from_ITfRangeACP(iface);
    LONG anchor;
    HRESULT hr;

    if (!pfEqual || !pWith || (aPos != TF_ANCHOR_START && aPos != TF_ANCHOR_END))
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    hr = range_get_anchor(range, pWith, aPos, &anchor);
    if (SUCCEEDED(hr))
        *pfEqual = range->anchorStart == anchor;
    return hr;
}

static HRESULT WINAPI Range_IsEqualEnd(ITfRangeACP *iface, TfEditCookie ec,
        ITfRange *pWith, TfAnchor aPos, BOOL *pfEqual)
{
    Range *range = impl_from_ITfRangeACP(iface);
    LONG anchor;
    HRESULT hr;

    if (!pfEqual || !pWith || (aPos != TF_ANCHOR_START && aPos != TF_ANCHOR_END))
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    hr = range_get_anchor(range, pWith, aPos, &anchor);
    if (SUCCEEDED(hr))
        *pfEqual = range->anchorEnd == anchor;
    return hr;
}

static HRESULT WINAPI Range_CompareStart(ITfRangeACP *iface, TfEditCookie ec,
        ITfRange *pWith, TfAnchor aPos, LONG *plResult)
{
    Range *range = impl_from_ITfRangeACP(iface);
    LONG anchor;
    HRESULT hr;

    if (!plResult || !pWith || (aPos != TF_ANCHOR_START && aPos != TF_ANCHOR_END))
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    hr = range_get_anchor(range, pWith, aPos, &anchor);
    if (SUCCEEDED(hr))
        *plResult = (range->anchorStart > anchor) - (range->anchorStart < anchor);
    return hr;
}

static HRESULT WINAPI Range_CompareEnd(ITfRangeACP *iface, TfEditCookie ec,
        ITfRange *pWith, TfAnchor aPos, LONG *plResult)
{
    Range *range = impl_from_ITfRangeACP(iface);
    LONG anchor;
    HRESULT hr;

    if (!plResult || !pWith || (aPos != TF_ANCHOR_START && aPos != TF_ANCHOR_END))
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    hr = range_get_anchor(range, pWith, aPos, &anchor);
    if (SUCCEEDED(hr))
        *plResult = (range->anchorEnd > anchor) - (range->anchorEnd < anchor);
    return hr;
}

static HRESULT WINAPI Range_AdjustForInsert(ITfRangeACP *iface, TfEditCookie ec,
        ULONG cchInsert, BOOL *pfInsertOk)
{
    Range *range = impl_from_ITfRangeACP(iface);
    ITextStoreACP *store;
    LONG start, end;
    HRESULT hr;

    if (!pfInsertOk)
        return E_INVALIDARG;
    *pfInsertOk = FALSE;
    if (FAILED(hr = Context_GetTextStore(range->context, ec, TS_LF_READ, &store)))
        return hr;
    hr = ITextStoreACP_QueryInsert(store, range->anchorStart, range->anchorEnd,
            cchInsert, &start, &end);
    ITextStoreACP_Release(store);
    if (FAILED(hr))
        return hr;
    if (start < 0 || end < start)
        return E_FAIL;
    range->anchorStart = start;
    range->anchorEnd = end;
    *pfInsertOk = TRUE;
    return S_OK;
}

static HRESULT WINAPI Range_GetGravity(ITfRangeACP *iface,
        TfGravity *pgStart, TfGravity *pgEnd)
{
    Range *range = impl_from_ITfRangeACP(iface);

    if (!pgStart || !pgEnd)
        return E_INVALIDARG;
    *pgStart = range->gravityStart;
    *pgEnd = range->gravityEnd;
    return S_OK;
}

static HRESULT WINAPI Range_SetGravity(ITfRangeACP *iface, TfEditCookie ec,
         TfGravity gStart, TfGravity gEnd)
{
    Range *range = impl_from_ITfRangeACP(iface);

    if ((gStart != TF_GRAVITY_BACKWARD && gStart != TF_GRAVITY_FORWARD) ||
        (gEnd != TF_GRAVITY_BACKWARD && gEnd != TF_GRAVITY_FORWARD))
        return E_INVALIDARG;
    if (!Context_IsValidCookie(range->context, ec, TS_LF_READ))
        return TF_E_NOLOCK;
    range->gravityStart = gStart;
    range->gravityEnd = gEnd;
    return S_OK;
}

static HRESULT WINAPI Range_Clone(ITfRangeACP *iface, ITfRange **ppClone)
{
    Range *range = impl_from_ITfRangeACP(iface);
    Range *clone;
    HRESULT hr;

    if (!ppClone)
        return E_INVALIDARG;
    *ppClone = NULL;
    if (FAILED(hr = Range_Constructor(range->context, range->anchorStart, range->anchorEnd, ppClone)))
        return hr;
    clone = unsafe_impl_from_ITfRange(*ppClone);
    clone->gravityStart = range->gravityStart;
    clone->gravityEnd = range->gravityEnd;
    return S_OK;
}

static HRESULT WINAPI Range_GetContext(ITfRangeACP *iface, ITfContext **context)
{
    Range *range = impl_from_ITfRangeACP(iface);

    TRACE("%p, %p.\n", iface, context);

    if (!context)
        return E_INVALIDARG;

    *context = range->context;
    ITfContext_AddRef(*context);

    return S_OK;
}

static HRESULT WINAPI Range_GetExtent(ITfRangeACP *iface, LONG *anchor, LONG *count)
{
    Range *range = impl_from_ITfRangeACP(iface);

    TRACE("%p, %p, %p.\n", iface, anchor, count);

    if (!anchor || !count)
        return E_INVALIDARG;
    *anchor = range->anchorStart;
    *count = range->anchorEnd - range->anchorStart;
    return S_OK;
}

static HRESULT WINAPI Range_SetExtent(ITfRangeACP *iface, LONG anchor, LONG count)
{
    Range *range = impl_from_ITfRangeACP(iface);

    TRACE("%p, %ld, %ld.\n", iface, anchor, count);

    if (anchor < 0 || count < 0 || anchor > LONG_MAX - count)
        return E_INVALIDARG;
    range->anchorStart = anchor;
    range->anchorEnd = anchor + count;
    return S_OK;
}

static const ITfRangeACPVtbl rangevtbl =
{
    Range_QueryInterface,
    Range_AddRef,
    Range_Release,
    Range_GetText,
    Range_SetText,
    Range_GetFormattedText,
    Range_GetEmbedded,
    Range_InsertEmbedded,
    Range_ShiftStart,
    Range_ShiftEnd,
    Range_ShiftStartToRange,
    Range_ShiftEndToRange,
    Range_ShiftStartRegion,
    Range_ShiftEndRegion,
    Range_IsEmpty,
    Range_Collapse,
    Range_IsEqualStart,
    Range_IsEqualEnd,
    Range_CompareStart,
    Range_CompareEnd,
    Range_AdjustForInsert,
    Range_GetGravity,
    Range_SetGravity,
    Range_Clone,
    Range_GetContext,
    Range_GetExtent,
    Range_SetExtent,
};

HRESULT Range_Constructor(ITfContext *context, LONG anchorStart, LONG anchorEnd, ITfRange **ppOut)
{
    Range *This;

    if (!context || !ppOut || anchorStart < 0 || anchorEnd < anchorStart)
        return E_INVALIDARG;
    *ppOut = NULL;

    This = calloc(1, sizeof(Range));
    if (This == NULL)
        return E_OUTOFMEMORY;

    TRACE("(%p) %p\n", This, context);

    This->ITfRangeACP_iface.lpVtbl = &rangevtbl;
    This->refCount = 1;
    This->context = context;
    ITfContext_AddRef(This->context);
    This->anchorStart = anchorStart;
    This->anchorEnd = anchorEnd;
    This->gravityStart = TF_GRAVITY_FORWARD;
    This->gravityEnd = TF_GRAVITY_BACKWARD;

    *ppOut = (ITfRange *)&This->ITfRangeACP_iface;

    TRACE("returning %p\n", *ppOut);

    return S_OK;
}

/* Internal conversion functions */

HRESULT TF_SELECTION_to_TS_SELECTION_ACP(const TF_SELECTION *tf, TS_SELECTION_ACP *tsAcp)
{
    ITfRangeACP *range;
    LONG count;
    HRESULT hr;

    if (!tf || !tsAcp || !tf->range)
        return E_INVALIDARG;

    if (FAILED(hr = ITfRange_QueryInterface(tf->range, &IID_ITfRangeACP, (void **)&range)))
        return hr;
    hr = ITfRangeACP_GetExtent(range, &tsAcp->acpStart, &count);
    ITfRangeACP_Release(range);
    if (FAILED(hr))
        return hr;
    if (tsAcp->acpStart < 0 || count < 0 || tsAcp->acpStart > LONG_MAX - count)
        return E_INVALIDARG;
    tsAcp->acpEnd = tsAcp->acpStart + count;
    tsAcp->style.ase = (TsActiveSelEnd)tf->style.ase;
    tsAcp->style.fInterimChar = tf->style.fInterimChar;
    return S_OK;
}
