/*
 * Host input method bridge for TSF-aware applications
 *
 * Copyright 2026 Switchyard Wine contributors
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
#include <stddef.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "objbase.h"
#include "oleauto.h"
#include "msctf.h"
#include "textstor.h"
#include "ntuser.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msctf);

/*
 * This CLSID identifies the broker only for ITfClientId allocation.  It does
 * not register or activate a text service and therefore cannot displace an
 * application's foreground TIP.
 */
static const CLSID CLSID_WineHostIME =
    {0x7cc87a76, 0x25d9, 0x4b22, {0x8a, 0x34, 0xda, 0x40, 0xa8, 0x95, 0xab, 0xa1}};

struct hostime_state;

struct hostime_composition_sink
{
    ITfCompositionSink ITfCompositionSink_iface;
    LONG ref;
    struct hostime_state *state; /* weak; cleared before state destruction */
    ULONG64 generation;
};

struct hostime_state
{
    DWORD thread_id;
    BOOL terminated;

    HWND root;
    ULONG64 focus_generation;
    ULONG64 last_transaction;
    ULONG64 last_serial;

    ITfThreadMgr *thread_mgr;
    ITfContext *context;
    ITfContextComposition *composition_services;
    ITextStoreACPServices *text_store_services;
    TfClientId client_id;

    ITfComposition *composition;
    struct hostime_composition_sink *sink;

    LONG original_start;
    LONG original_end;
    LONG original_selection_start;
    LONG original_selection_end;
    WCHAR *original_text;
    ULONG original_length;
};

struct hostime_route
{
    ITfThreadMgr *thread_mgr;
    ITfContext *context;
    ITfContextComposition *composition_services;
    ITextStoreACPServices *text_store_services;
    TfClientId client_id;
};

struct hostime_edit_session
{
    ITfEditSession ITfEditSession_iface;
    LONG ref;
    struct hostime_state *state;
    struct wine_host_ime_event *event;
    HRESULT result;
};

static INIT_ONCE hostime_fls_once = INIT_ONCE_STATIC_INIT;
static DWORD hostime_fls = FLS_OUT_OF_INDEXES;

static inline struct hostime_composition_sink *impl_from_ITfCompositionSink(
        ITfCompositionSink *iface)
{
    return CONTAINING_RECORD(iface, struct hostime_composition_sink,
            ITfCompositionSink_iface);
}

static inline struct hostime_edit_session *impl_from_ITfEditSession(ITfEditSession *iface)
{
    return CONTAINING_RECORD(iface, struct hostime_edit_session, ITfEditSession_iface);
}

static HWND hostime_root_window(HWND hwnd)
{
    HWND root;

    if (!hwnd) return NULL;
    return (root = GetAncestor(hwnd, GA_ROOT)) ? root : hwnd;
}

static void hostime_clear_snapshot(struct hostime_state *state)
{
    free(state->original_text);
    state->original_text = NULL;
    state->original_length = 0;
    state->original_start = state->original_end = 0;
    state->original_selection_start = state->original_selection_end = 0;
}

static void hostime_clear_composition(struct hostime_state *state)
{
    if (state->sink) state->sink->state = NULL;
    if (state->composition) ITfComposition_Release(state->composition);
    if (state->sink) ITfCompositionSink_Release(&state->sink->ITfCompositionSink_iface);
    state->composition = NULL;
    state->sink = NULL;
    state->terminated = FALSE;
    hostime_clear_snapshot(state);
}

static void hostime_release_route(struct hostime_state *state)
{
    hostime_clear_composition(state);
    if (state->text_store_services)
        ITextStoreACPServices_Release(state->text_store_services);
    if (state->composition_services)
        ITfContextComposition_Release(state->composition_services);
    if (state->context) ITfContext_Release(state->context);
    if (state->thread_mgr) ITfThreadMgr_Release(state->thread_mgr);
    state->text_store_services = NULL;
    state->composition_services = NULL;
    state->context = NULL;
    state->thread_mgr = NULL;
    state->client_id = 0;
    state->root = NULL;
}

static void WINAPI hostime_state_destroy(void *value)
{
    struct hostime_state *state = value;

    if (!state) return;
    hostime_release_route(state);
    free(state);
}

static BOOL CALLBACK hostime_init_fls(INIT_ONCE *once, void *param, void **context)
{
    hostime_fls = FlsAlloc(hostime_state_destroy);
    return TRUE;
}

static struct hostime_state *hostime_get_state(BOOL create)
{
    struct hostime_state *state;

    InitOnceExecuteOnce(&hostime_fls_once, hostime_init_fls, NULL, NULL);
    if (hostime_fls == FLS_OUT_OF_INDEXES) return NULL;
    if (!(state = FlsGetValue(hostime_fls)) && create)
    {
        if (!(state = calloc(1, sizeof(*state)))) return NULL;
        state->thread_id = GetCurrentThreadId();
        if (!FlsSetValue(hostime_fls, state))
        {
            free(state);
            return NULL;
        }
    }
    return state;
}

static BOOL hostime_variant_is_nonzero(const VARIANT *value)
{
    switch (V_VT(value))
    {
    case VT_I1:   return V_I1(value) != 0;
    case VT_UI1:  return V_UI1(value) != 0;
    case VT_I2:   return V_I2(value) != 0;
    case VT_UI2:  return V_UI2(value) != 0;
    case VT_I4:
    case VT_INT:  return V_I4(value) != 0;
    case VT_UI4:
    case VT_UINT: return V_UI4(value) != 0;
    case VT_BOOL: return V_BOOL(value) != VARIANT_FALSE;
    case VT_EMPTY:
    case VT_NULL: return FALSE;
    default:      return TRUE;
    }
}

static HRESULT hostime_check_compartment(ITfCompartmentMgr *manager, REFGUID guid)
{
    ITfCompartment *compartment;
    VARIANT value;
    HRESULT hr;

    if (FAILED(hr = ITfCompartmentMgr_GetCompartment(manager, guid, &compartment)))
        return hr;
    VariantInit(&value);
    hr = ITfCompartment_GetValue(compartment, &value);
    ITfCompartment_Release(compartment);
    if (FAILED(hr)) return hr;
    if (hostime_variant_is_nonzero(&value)) hr = S_FALSE;
    VariantClear(&value);
    return hr;
}

static void hostime_route_release(struct hostime_route *route)
{
    if (route->text_store_services)
        ITextStoreACPServices_Release(route->text_store_services);
    if (route->composition_services)
        ITfContextComposition_Release(route->composition_services);
    if (route->context) ITfContext_Release(route->context);
    if (route->thread_mgr) ITfThreadMgr_Release(route->thread_mgr);
    memset(route, 0, sizeof(*route));
}

static BOOL hostime_context_is_focused(struct hostime_state *state)
{
    ITfDocumentMgr *document = NULL;
    ITfContext *context = NULL;
    IUnknown *expected = NULL, *current = NULL;
    BOOL ret = FALSE;

    if (ITfThreadMgr_GetFocus(state->thread_mgr, &document) != S_OK ||
        !document ||
        ITfDocumentMgr_GetTop(document, &context) != S_OK || !context)
        goto done;
    if (FAILED(ITfContext_QueryInterface(state->context, &IID_IUnknown,
                                        (void **)&expected)) ||
        FAILED(ITfContext_QueryInterface(context, &IID_IUnknown,
                                        (void **)&current)))
        goto done;
    ret = expected == current;

done:
    if (current) IUnknown_Release(current);
    if (expected) IUnknown_Release(expected);
    if (context) ITfContext_Release(context);
    if (document) ITfDocumentMgr_Release(document);
    return ret;
}

static HRESULT hostime_open_route(HWND hwnd, struct hostime_route *route)
{
    ITfContextView *view = NULL;
    ITfCompartmentMgr *compartment_manager = NULL;
    ITfDocumentMgr *document_mgr = NULL;
    ITfInsertAtSelection *insert = NULL;
    ITfClientId *client_ids = NULL;
    GUITHREADINFO gui = {sizeof(gui)};
    TF_STATUS status;
    HWND context_hwnd = NULL, focus;
    DWORD window_thread;
    HRESULT hr;

    memset(route, 0, sizeof(*route));
    if (!hwnd || !IsWindow(hwnd)) return S_FALSE;
    window_thread = GetWindowThreadProcessId(hwnd, NULL);
    if (!window_thread || window_thread != GetCurrentThreadId()) return S_FALSE;

    /*
     * A context view is not implemented by older Wine msctf.  The focused top
     * context plus same-GUI-thread/root check is the compatible base gate.
     */
    if (!GetGUIThreadInfo(window_thread, &gui) || !(focus = gui.hwndFocus))
        focus = GetFocus();
    if (!focus || hostime_root_window(focus) != hostime_root_window(hwnd))
        return S_FALSE;

    if (FAILED(hr = TF_GetThreadMgr(&route->thread_mgr)) || !route->thread_mgr)
        return S_FALSE;
    if (ITfThreadMgr_GetFocus(route->thread_mgr, &document_mgr) != S_OK ||
        !document_mgr)
    {
        hr = S_FALSE;
        goto done;
    }
    if (FAILED(hr = ITfDocumentMgr_GetTop(document_mgr, &route->context)) ||
        !route->context)
    {
        hr = S_FALSE;
        goto done;
    }
    if (FAILED(hr = ITfContext_GetStatus(route->context, &status)) ||
        (status.dwDynamicFlags & (TF_SD_READONLY | TF_SD_LOADING)) ||
        (status.dwStaticFlags & (TF_SS_DISJOINTSEL | TF_SS_REGIONS)))
    {
        hr = S_FALSE;
        goto done;
    }

    if (FAILED(hr = ITfContext_QueryInterface(route->context,
            &IID_ITfContextComposition, (void **)&route->composition_services)) ||
        FAILED(hr = ITfContext_QueryInterface(route->context,
            &IID_ITfInsertAtSelection, (void **)&insert)) ||
        FAILED(hr = ITfContext_QueryInterface(route->context,
            &IID_ITextStoreACPServices, (void **)&route->text_store_services)) ||
        FAILED(hr = ITfContext_QueryInterface(route->context,
            &IID_ITfCompartmentMgr, (void **)&compartment_manager)))
    {
        hr = S_FALSE;
        goto done;
    }
    if (hostime_check_compartment(compartment_manager,
            &GUID_COMPARTMENT_KEYBOARD_DISABLED) != S_OK ||
        hostime_check_compartment(compartment_manager,
            &GUID_COMPARTMENT_EMPTYCONTEXT) != S_OK)
    {
        hr = S_FALSE;
        goto done;
    }

    /*
     * When a context view is available, it is stronger evidence than the GUI
     * focus fallback and must agree with the target root.
     */
    if (SUCCEEDED(ITfContext_GetActiveView(route->context, &view)) && view)
    {
        if (FAILED(ITfContextView_GetWnd(view, &context_hwnd)) ||
            !context_hwnd ||
            GetWindowThreadProcessId(context_hwnd, NULL) != window_thread ||
            hostime_root_window(context_hwnd) != hostime_root_window(hwnd))
        {
            hr = S_FALSE;
            goto done;
        }
    }

    if (FAILED(hr = ITfThreadMgr_QueryInterface(route->thread_mgr,
            &IID_ITfClientId, (void **)&client_ids)) ||
        FAILED(hr = ITfClientId_GetClientId(client_ids, &CLSID_WineHostIME,
            &route->client_id)) || !route->client_id)
    {
        hr = S_FALSE;
        goto done;
    }
    hr = S_OK;

done:
    if (view) ITfContextView_Release(view);
    if (client_ids) ITfClientId_Release(client_ids);
    if (compartment_manager) ITfCompartmentMgr_Release(compartment_manager);
    if (insert) ITfInsertAtSelection_Release(insert);
    if (document_mgr) ITfDocumentMgr_Release(document_mgr);
    if (hr != S_OK) hostime_route_release(route);
    return hr;
}

static HRESULT WINAPI hostime_sink_QueryInterface(ITfCompositionSink *iface,
        REFIID iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) ||
        IsEqualIID(iid, &IID_ITfCompositionSink))
        *out = iface;
    if (!*out) return E_NOINTERFACE;
    ITfCompositionSink_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI hostime_sink_AddRef(ITfCompositionSink *iface)
{
    struct hostime_composition_sink *sink = impl_from_ITfCompositionSink(iface);
    return InterlockedIncrement(&sink->ref);
}

static ULONG WINAPI hostime_sink_Release(ITfCompositionSink *iface)
{
    struct hostime_composition_sink *sink = impl_from_ITfCompositionSink(iface);
    ULONG ref = InterlockedDecrement(&sink->ref);
    if (!ref) free(sink);
    return ref;
}

static HRESULT WINAPI hostime_sink_OnCompositionTerminated(ITfCompositionSink *iface,
        TfEditCookie cookie, ITfComposition *composition)
{
    struct hostime_composition_sink *sink = impl_from_ITfCompositionSink(iface);

    TRACE("generation %s composition %p terminated.\n",
            wine_dbgstr_longlong(sink->generation), composition);
    if (sink->state && sink->state->sink == sink &&
        sink->state->focus_generation == sink->generation)
        sink->state->terminated = TRUE;
    return S_OK;
}

static const ITfCompositionSinkVtbl hostime_sink_vtbl =
{
    hostime_sink_QueryInterface,
    hostime_sink_AddRef,
    hostime_sink_Release,
    hostime_sink_OnCompositionTerminated,
};

static HRESULT hostime_sink_create(struct hostime_state *state,
        struct hostime_composition_sink **out)
{
    struct hostime_composition_sink *sink;

    *out = NULL;
    if (!(sink = calloc(1, sizeof(*sink)))) return E_OUTOFMEMORY;
    sink->ITfCompositionSink_iface.lpVtbl = &hostime_sink_vtbl;
    sink->ref = 1;
    sink->state = state;
    sink->generation = state->focus_generation;
    *out = sink;
    return S_OK;
}

static HRESULT hostime_get_extent(ITfRange *range, LONG *start, LONG *end)
{
    ITfRangeACP *range_acp;
    LONG length;
    HRESULT hr;

    if (FAILED(hr = ITfRange_QueryInterface(range, &IID_ITfRangeACP,
            (void **)&range_acp)))
        return hr;
    hr = ITfRangeACP_GetExtent(range_acp, start, &length);
    ITfRangeACP_Release(range_acp);
    if (FAILED(hr)) return hr;
    if (*start < 0 || length < 0 || *start > LONG_MAX - length)
        return E_INVALIDARG;
    *end = *start + length;
    return S_OK;
}

static HRESULT hostime_create_range(struct hostime_state *state, ULONG64 location,
        ULONG64 length, ITfRange **out)
{
    ITfRangeACP *range_acp;
    HRESULT hr;

    *out = NULL;
    if (location > LONG_MAX || length > LONG_MAX ||
        location + length < location || location + length > LONG_MAX)
        return E_INVALIDARG;
    hr = ITextStoreACPServices_CreateRange(state->text_store_services,
            location, location + length, &range_acp);
    if (SUCCEEDED(hr)) *out = (ITfRange *)range_acp;
    return hr;
}

static HRESULT hostime_get_selection(struct hostime_state *state, TfEditCookie cookie,
        ITfRange **out)
{
    TF_SELECTION selection;
    ULONG fetched = 0;
    HRESULT hr;

    memset(&selection, 0, sizeof(selection));
    *out = NULL;
    hr = ITfContext_GetSelection(state->context, cookie, TF_DEFAULT_SELECTION,
            1, &selection, &fetched);
    if (FAILED(hr)) return hr;
    if (fetched != 1 || !selection.range) return E_FAIL;
    *out = selection.range;
    return S_OK;
}

static HRESULT hostime_get_target_range(struct hostime_state *state, TfEditCookie cookie,
        const struct wine_host_ime_event *event, ITfRange **out)
{
    if (event->replacement_range.location != WINE_HOST_IME_RANGE_NOT_FOUND)
    {
        if (event->flags & WINE_HOST_IME_EVENT_REPLACEMENT_RELATIVE)
        {
            ITfRange *composition_range;
            ULONG64 composition_length;
            LONG start, end;
            HRESULT hr;

            if (!state->composition) return E_INVALIDARG;
            if (FAILED(hr = ITfComposition_GetRange(state->composition,
                    &composition_range)))
                return hr;
            hr = hostime_get_extent(composition_range, &start, &end);
            ITfRange_Release(composition_range);
            if (FAILED(hr)) return hr;
            composition_length = end - start;
            if (event->replacement_range.location > composition_length ||
                event->replacement_range.length > composition_length ||
                event->replacement_range.location +
                        event->replacement_range.length <
                        event->replacement_range.location ||
                event->replacement_range.location +
                        event->replacement_range.length > composition_length)
                return E_INVALIDARG;
            return hostime_create_range(state,
                    start + event->replacement_range.location,
                    event->replacement_range.length, out);
        }
        return hostime_create_range(state, event->replacement_range.location,
                event->replacement_range.length, out);
    }
    if (state->composition)
        return ITfComposition_GetRange(state->composition, out);
    return hostime_get_selection(state, cookie, out);
}

static HRESULT hostime_set_selection(struct hostime_state *state, TfEditCookie cookie,
        LONG inserted_start, ULONG inserted_length,
        const struct wine_host_ime_range *selected)
{
    TF_SELECTION selection;
    ULONG64 relative_start, relative_length;
    HRESULT hr;

    relative_start = selected->location;
    relative_length = selected->length;
    if (relative_start == WINE_HOST_IME_RANGE_NOT_FOUND)
    {
        relative_start = inserted_length;
        relative_length = 0;
    }
    if (relative_start > inserted_length || relative_length > inserted_length ||
        relative_start + relative_length < relative_start ||
        relative_start + relative_length > inserted_length)
        return E_INVALIDARG;

    memset(&selection, 0, sizeof(selection));
    if (FAILED(hr = hostime_create_range(state, inserted_start + relative_start,
            relative_length, &selection.range)))
        return hr;
    selection.style.ase = TF_AE_NONE;
    selection.style.fInterimChar = FALSE;
    hr = ITfContext_SetSelection(state->context, cookie, 1, &selection);
    ITfRange_Release(selection.range);
    return hr;
}

static HRESULT hostime_save_snapshot(struct hostime_state *state, TfEditCookie cookie,
        ITfRange *target)
{
    ITfRange *selection = NULL;
    ULONG fetched = 0, requested;
    LONG length;
    HRESULT hr;

    if (FAILED(hr = hostime_get_extent(target, &state->original_start,
            &state->original_end)))
        return hr;
    length = state->original_end - state->original_start;
    if (FAILED(hr = hostime_get_selection(state, cookie, &selection)))
        return hr;
    hr = hostime_get_extent(selection, &state->original_selection_start,
            &state->original_selection_end);
    ITfRange_Release(selection);
    if (FAILED(hr)) return hr;

    if (length)
    {
        if (!(state->original_text = malloc(length * sizeof(WCHAR))))
            return E_OUTOFMEMORY;
        requested = length;
        hr = ITfRange_GetText(target, cookie, 0, state->original_text,
                requested, &fetched);
        if (FAILED(hr) || fetched != requested)
        {
            hostime_clear_snapshot(state);
            return FAILED(hr) ? hr : E_FAIL;
        }
    }
    state->original_length = length;
    return S_OK;
}

static HRESULT hostime_start_composition(struct hostime_state *state,
        TfEditCookie cookie, ITfRange *range)
{
    struct hostime_composition_sink *sink;
    ITfComposition *composition = NULL;
    HRESULT hr;

    if (FAILED(hr = hostime_sink_create(state, &sink))) return hr;
    hr = ITfContextComposition_StartComposition(state->composition_services,
            cookie, range, &sink->ITfCompositionSink_iface, &composition);
    if (FAILED(hr) || !composition)
    {
        ITfCompositionSink_Release(&sink->ITfCompositionSink_iface);
        return FAILED(hr) ? hr : E_FAIL;
    }
    state->sink = sink;
    state->composition = composition;
    state->terminated = FALSE;
    return S_OK;
}

static HRESULT hostime_end_composition(struct hostime_state *state, TfEditCookie cookie)
{
    ITfComposition *composition;
    HRESULT hr;

    if (!(composition = state->composition)) return S_OK;
    hr = ITfComposition_EndComposition(composition, cookie);
    if (SUCCEEDED(hr)) hostime_clear_composition(state);
    return hr;
}

static HRESULT hostime_replace_range(struct hostime_state *state, TfEditCookie cookie,
        ITfRange *range, const struct wine_host_ime_event *event, LONG *inserted_start)
{
    LONG inserted_end;
    HRESULT hr;

    if (event->text_length > LONG_MAX) return E_INVALIDARG;
    if (FAILED(hr = ITfRange_SetText(range, cookie, 0, event->text,
            event->text_length)))
        return hr;
    if (FAILED(hr = hostime_get_extent(range, inserted_start, &inserted_end)))
        return hr;
    if (inserted_end - *inserted_start != event->text_length) return E_FAIL;
    return S_OK;
}

static HRESULT hostime_restore_snapshot(struct hostime_state *state,
        TfEditCookie cookie, ITfRange *range)
{
    ITfRange *selection = NULL;
    HRESULT hr;

    hr = ITfRange_SetText(range, cookie, 0, state->original_text,
            state->original_length);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = hostime_create_range(state, state->original_selection_start,
            state->original_selection_end - state->original_selection_start,
            &selection)))
        return hr;
    {
        TF_SELECTION selected;
        memset(&selected, 0, sizeof(selected));
        selected.range = selection;
        selected.style.ase = TF_AE_NONE;
        hr = ITfContext_SetSelection(state->context, cookie, 1, &selected);
    }
    ITfRange_Release(selection);
    return hr;
}

static HRESULT hostime_restore_cancelled_composition(struct hostime_state *state,
        TfEditCookie cookie)
{
    ITfRange *range = NULL;
    HRESULT hr;

    if (!state->composition) return S_OK;
    if (FAILED(hr = ITfComposition_GetRange(state->composition, &range)))
        return hr;
    hr = hostime_restore_snapshot(state, cookie, range);
    ITfRange_Release(range);
    if (FAILED(hr)) return hr;
    return hostime_end_composition(state, cookie);
}

static HRESULT hostime_do_operation(struct hostime_state *state, TfEditCookie cookie,
        const struct wine_host_ime_event *event)
{
    ITfRange *range = NULL;
    LONG inserted_start;
    BOOL starting;
    HRESULT hr;

    if (state->terminated) hostime_clear_composition(state);

    switch (event->operation)
    {
    case WINE_HOST_IME_SET_MARKED:
        starting = !state->composition;
        if (FAILED(hr = hostime_get_target_range(state, cookie, event, &range)))
            return hr;
        if (starting &&
            FAILED(hr = hostime_save_snapshot(state, cookie, range)))
            goto done;
        if (FAILED(hr = hostime_replace_range(state, cookie, range, event,
                &inserted_start)))
            goto rollback_start;
        if (starting &&
            FAILED(hr = hostime_start_composition(state, cookie, range)))
            goto rollback_start;
        hr = hostime_set_selection(state, cookie, inserted_start,
                event->text_length, &event->selected_range);
        if (FAILED(hr) && starting)
        {
            HRESULT restore_hr =
                    hostime_restore_cancelled_composition(state, cookie);
            if (FAILED(restore_hr)) hr = restore_hr;
        }
        break;

    case WINE_HOST_IME_COMMIT:
        if (FAILED(hr = hostime_get_target_range(state, cookie, event, &range)))
            return hr;
        if (FAILED(hr = hostime_replace_range(state, cookie, range, event,
                &inserted_start)))
            goto done;
        hr = hostime_set_selection(state, cookie, inserted_start,
                event->text_length, &event->selected_range);
        {
            HRESULT end_hr = hostime_end_composition(state, cookie);

            /*
             * Once replacement succeeded, completing the composition is the
             * authoritative state transition.  A rejected caret update should
             * not leave the application and broker disagreeing about whether
             * marked text is still active.
             */
            if (SUCCEEDED(end_hr)) hr = S_OK;
            else hr = end_hr;
        }
        break;

    case WINE_HOST_IME_UNMARK_EXISTING:
        hr = hostime_end_composition(state, cookie);
        break;

    case WINE_HOST_IME_CANCEL:
        hr = hostime_restore_cancelled_composition(state, cookie);
        break;

    case WINE_HOST_IME_CONSUMED_NO_TEXT:
        hr = S_OK;
        break;

    default:
        hr = E_INVALIDARG;
        break;
    }
    goto done;

rollback_start:
    if (starting)
    {
        HRESULT restore_hr = hostime_restore_snapshot(state, cookie, range);

        hostime_clear_snapshot(state);
        if (FAILED(restore_hr)) hr = restore_hr;
    }

done:
    if (range) ITfRange_Release(range);
    return hr;
}

static HRESULT WINAPI hostime_session_QueryInterface(ITfEditSession *iface,
        REFIID iid, void **out)
{
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITfEditSession))
        *out = iface;
    if (!*out) return E_NOINTERFACE;
    ITfEditSession_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI hostime_session_AddRef(ITfEditSession *iface)
{
    struct hostime_edit_session *session = impl_from_ITfEditSession(iface);
    return InterlockedIncrement(&session->ref);
}

static ULONG WINAPI hostime_session_Release(ITfEditSession *iface)
{
    struct hostime_edit_session *session = impl_from_ITfEditSession(iface);
    ULONG ref = InterlockedDecrement(&session->ref);
    if (!ref)
    {
        free(session->event);
        free(session);
    }
    return ref;
}

static HRESULT WINAPI hostime_session_DoEditSession(ITfEditSession *iface,
        TfEditCookie cookie)
{
    struct hostime_edit_session *session = impl_from_ITfEditSession(iface);
    return session->result = hostime_do_operation(session->state, cookie,
            session->event);
}

static const ITfEditSessionVtbl hostime_session_vtbl =
{
    hostime_session_QueryInterface,
    hostime_session_AddRef,
    hostime_session_Release,
    hostime_session_DoEditSession,
};

static HRESULT hostime_session_create(struct hostime_state *state,
        const struct wine_host_ime_event *event, ULONG size,
        struct hostime_edit_session **out)
{
    struct hostime_edit_session *session;

    *out = NULL;
    if (!(session = calloc(1, sizeof(*session)))) return E_OUTOFMEMORY;
    if (!(session->event = malloc(size)))
    {
        free(session);
        return E_OUTOFMEMORY;
    }
    memcpy(session->event, event, size);
    session->ITfEditSession_iface.lpVtbl = &hostime_session_vtbl;
    session->ref = 1;
    session->state = state;
    session->result = E_PENDING;
    *out = session;
    return S_OK;
}

static HRESULT hostime_validate_event(const struct wine_host_ime_event *event,
        ULONG size)
{
    HWND hwnd;
    SIZE_T required;

    if (!event || size < offsetof(struct wine_host_ime_event, text))
        return E_INVALIDARG;
    if (event->size != size || event->version != WINE_HOST_IME_EVENT_VERSION)
        return E_INVALIDARG;
    if ((SIZE_T)event->text_length >
        (size - offsetof(struct wine_host_ime_event, text)) / sizeof(WCHAR))
        return E_INVALIDARG;
    required = offsetof(struct wine_host_ime_event, text) +
            event->text_length * sizeof(WCHAR);
    if (required != size || event->operation > WINE_HOST_IME_CONSUMED_NO_TEXT)
        return E_INVALIDARG;
    if (event->flags & ~WINE_HOST_IME_EVENT_REPLACEMENT_RELATIVE)
        return E_INVALIDARG;
    if ((event->flags & WINE_HOST_IME_EVENT_REPLACEMENT_RELATIVE) &&
        event->replacement_range.location == WINE_HOST_IME_RANGE_NOT_FOUND)
        return E_INVALIDARG;
    if (event->thread_id && event->thread_id != GetCurrentThreadId())
        return E_INVALIDARG;
    hwnd = (HWND)(UINT_PTR)event->hwnd;
    if ((ULONG64)(UINT_PTR)hwnd != event->hwnd || !hwnd || !IsWindow(hwnd) ||
        GetWindowThreadProcessId(hwnd, NULL) != GetCurrentThreadId())
        return E_INVALIDARG;
    if (event->selected_range.location != WINE_HOST_IME_RANGE_NOT_FOUND &&
        (event->selected_range.location > LONG_MAX ||
         event->selected_range.length > LONG_MAX ||
         event->selected_range.location + event->selected_range.length <
                 event->selected_range.location ||
         event->selected_range.location + event->selected_range.length >
                 LONG_MAX))
        return E_INVALIDARG;
    if (event->replacement_range.location != WINE_HOST_IME_RANGE_NOT_FOUND &&
        (event->replacement_range.location > LONG_MAX ||
         event->replacement_range.length > LONG_MAX ||
         event->replacement_range.location + event->replacement_range.length <
                 event->replacement_range.location ||
         event->replacement_range.location + event->replacement_range.length >
                 LONG_MAX))
        return E_INVALIDARG;
    return S_OK;
}

static HRESULT hostime_prepare_state(struct hostime_state *state,
        const struct wine_host_ime_event *event)
{
    struct hostime_route route;
    HWND hwnd = (HWND)(UINT_PTR)event->hwnd;
    HWND root = hostime_root_window(hwnd);
    GUITHREADINFO gui = {sizeof(gui)};
    HWND focus;
    HRESULT hr;

    if (state->terminated) hostime_clear_composition(state);
    if (state->context)
    {
        if (state->focus_generation != event->focus_generation ||
            state->root != root)
        {
            /* Never reroute a live composition to a newly focused context. */
            if (state->composition) return S_FALSE;
            hostime_release_route(state);
        }
        else if (state->composition)
        {
            if (!GetGUIThreadInfo(state->thread_id, &gui) ||
                !(focus = gui.hwndFocus) ||
                hostime_root_window(focus) != state->root ||
                !hostime_context_is_focused(state))
                return S_FALSE;
            return S_OK;
        }
        else
        {
            /* Revalidate commit-only and consumed transactions. */
            hostime_release_route(state);
        }
    }

    if (FAILED(hr = hostime_open_route(hwnd, &route)) || hr != S_OK)
        return S_FALSE;
    state->thread_mgr = route.thread_mgr;
    state->context = route.context;
    state->composition_services = route.composition_services;
    state->text_store_services = route.text_store_services;
    state->client_id = route.client_id;
    state->root = root;
    state->focus_generation = event->focus_generation;
    memset(&route, 0, sizeof(route));
    return S_OK;
}

static HRESULT hostime_request_edit(struct hostime_state *state,
        const struct wine_host_ime_event *event, ULONG size)
{
    struct hostime_edit_session *session;
    HRESULT session_hr = E_FAIL, hr;

    if (FAILED(hr = hostime_session_create(state, event, size, &session)))
        return hr;
    hr = ITfContext_RequestEditSession(state->context, state->client_id,
            &session->ITfEditSession_iface, TF_ES_SYNC | TF_ES_READWRITE,
            &session_hr);
    if (SUCCEEDED(hr)) hr = session_hr;
    ITfEditSession_Release(&session->ITfEditSession_iface);

    /*
     * S_FALSE is reserved for a pre-mutation routing miss.  Once an edit
     * session has run, callers must not retry the packet through IMM even when
     * the target text store reports an error after a partial mutation.
     */
    if (hr == S_FALSE) hr = E_FAIL;
    return hr;
}

/***********************************************************************
 *           TF_WineHostIMEQuery (MSCTF.@)
 */
HRESULT WINAPI TF_WineHostIMEQuery(HWND hwnd, ULONG64 focus_generation,
        DWORD flags, DWORD *capabilities)
{
    struct hostime_route route;
    HRESULT hr;

    TRACE("hwnd %p generation %s flags %#lx.\n", hwnd,
            wine_dbgstr_longlong(focus_generation), flags);
    if (!capabilities) return E_INVALIDARG;
    *capabilities = 0;
    if (flags & WINE_HOST_IME_QUERY_DISABLED) return S_FALSE;
    if ((hr = hostime_open_route(hwnd, &route)) != S_OK) return S_FALSE;
    *capabilities = WINE_HOST_IME_CAP_COMPOSITION |
            WINE_HOST_IME_CAP_REPLACEMENT | WINE_HOST_IME_CAP_EXACT_UTF16;
    hostime_route_release(&route);
    return S_OK;
}

/***********************************************************************
 *           TF_WineHostIMEProcessKey (MSCTF.@)
 */
HRESULT WINAPI TF_WineHostIMEProcessKey(HWND hwnd, WPARAM wparam, LPARAM lparam,
        ULONG64 transaction_id, ULONG64 focus_generation, BOOL *eaten)
{
    struct hostime_route route;
    ITfKeystrokeMgr *keystrokes = NULL;
    CLSID foreground;
    BOOL key_up = !!(lparam & 0x80000000);
    HRESULT hr;

    TRACE("hwnd %p key %#Ix lparam %#Ix transaction %s generation %s.\n",
            hwnd, wparam, lparam, wine_dbgstr_longlong(transaction_id),
            wine_dbgstr_longlong(focus_generation));
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    if ((hr = hostime_open_route(hwnd, &route)) != S_OK) return S_FALSE;
    if (FAILED(hr = ITfThreadMgr_QueryInterface(route.thread_mgr,
            &IID_ITfKeystrokeMgr, (void **)&keystrokes)) ||
        ITfKeystrokeMgr_GetForeground(keystrokes, &foreground) != S_OK)
    {
        hr = S_FALSE;
        goto done;
    }
    if (key_up)
    {
        if (FAILED(hr = ITfKeystrokeMgr_TestKeyUp(keystrokes, wparam, lparam,
                eaten)) || !*eaten)
            goto done;
        hr = ITfKeystrokeMgr_KeyUp(keystrokes, wparam, lparam, eaten);
    }
    else
    {
        if (FAILED(hr = ITfKeystrokeMgr_TestKeyDown(keystrokes, wparam,
                lparam, eaten)) || !*eaten)
            goto done;
        hr = ITfKeystrokeMgr_KeyDown(keystrokes, wparam, lparam, eaten);
    }

done:
    if (keystrokes) ITfKeystrokeMgr_Release(keystrokes);
    hostime_route_release(&route);
    return hr;
}

/***********************************************************************
 *           TF_WineHostIMEApply (MSCTF.@)
 */
HRESULT WINAPI TF_WineHostIMEApply(const struct wine_host_ime_event *event,
        ULONG size)
{
    struct hostime_state *state;
    HRESULT hr;

    if (FAILED(hr = hostime_validate_event(event, size))) return hr;
    if (!(state = hostime_get_state(TRUE))) return E_OUTOFMEMORY;
    if (event->callback_serial && event->callback_serial <= state->last_serial)
    {
        TRACE("dropping stale callback %s (last %s).\n",
                wine_dbgstr_longlong(event->callback_serial),
                wine_dbgstr_longlong(state->last_serial));
        return S_FALSE;
    }
    if ((hr = hostime_prepare_state(state, event)) != S_OK) return S_FALSE;

    if (event->operation == WINE_HOST_IME_CONSUMED_NO_TEXT)
    {
        state->last_transaction = event->transaction_id;
        state->last_serial = event->callback_serial;
        return S_OK;
    }
    hr = hostime_request_edit(state, event, size);
    state->last_transaction = event->transaction_id;
    state->last_serial = event->callback_serial;
    return hr;
}

/***********************************************************************
 *           TF_WineHostIMEReset (MSCTF.@)
 */
HRESULT WINAPI TF_WineHostIMEReset(HWND hwnd, ULONG64 focus_generation,
        DWORD flags)
{
    struct hostime_state *state = hostime_get_state(FALSE);
    struct wine_host_ime_event event;
    const ULONG event_size = offsetof(struct wine_host_ime_event, text);
    HRESULT hr;

    TRACE("hwnd %p generation %s flags %#lx.\n", hwnd,
            wine_dbgstr_longlong(focus_generation), flags);
    if (flags & ~WINE_HOST_IME_RESET_CANCEL) return E_INVALIDARG;
    if (!state) return S_OK;
    if (state->thread_id != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if ((hwnd && state->root != hostime_root_window(hwnd)) ||
        (focus_generation &&
         state->focus_generation != focus_generation))
        return S_FALSE;
    if (state->terminated) hostime_clear_composition(state);

    if (state->composition)
    {
        if (!(flags & WINE_HOST_IME_RESET_CANCEL)) return S_FALSE;
        memset(&event, 0, sizeof(event));
        event.size = event_size;
        event.version = WINE_HOST_IME_EVENT_VERSION;
        event.hwnd = (UINT_PTR)state->root;
        event.thread_id = state->thread_id;
        event.operation = WINE_HOST_IME_CANCEL;
        event.focus_generation = state->focus_generation;
        event.selected_range.location = WINE_HOST_IME_RANGE_NOT_FOUND;
        event.replacement_range.location = WINE_HOST_IME_RANGE_NOT_FOUND;
        if (FAILED(hr = hostime_request_edit(state, &event, event_size)))
            return hr;
    }
    hostime_release_route(state);
    state->focus_generation = 0;
    state->last_transaction = 0;
    state->last_serial = 0;
    return S_OK;
}
