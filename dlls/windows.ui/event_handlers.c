/* WinRT Windows.UI event handler storage
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

#include "private.h"

struct event_handler_entry
{
    struct list entry;
    EventRegistrationToken token;
    IUnknown *handler;
};

static CRITICAL_SECTION handlers_cs;
static CRITICAL_SECTION_DEBUG handlers_cs_debug =
{
    0, 0, &handlers_cs,
    { &handlers_cs_debug.ProcessLocksList, &handlers_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": handlers_cs") }
};
static CRITICAL_SECTION handlers_cs = { &handlers_cs_debug, -1, 0, 0, 0, 0 };
static LONG next_token;

void event_handlers_init( struct event_handlers *handlers )
{
    list_init( &handlers->entries );
}

HRESULT event_handlers_add( struct event_handlers *handlers, IUnknown *handler, EventRegistrationToken *token )
{
    struct event_handler_entry *entry;

    if (!handler || !token) return E_INVALIDARG;
    if (!(entry = malloc( sizeof(*entry) ))) return E_OUTOFMEMORY;

    entry->handler = handler;
    IUnknown_AddRef( handler );
    entry->token.value = InterlockedIncrement( &next_token );

    EnterCriticalSection( &handlers_cs );
    list_add_tail( &handlers->entries, &entry->entry );
    LeaveCriticalSection( &handlers_cs );

    *token = entry->token;
    return S_OK;
}

HRESULT event_handlers_remove( struct event_handlers *handlers, EventRegistrationToken token )
{
    struct event_handler_entry *entry;
    BOOL found = FALSE;

    EnterCriticalSection( &handlers_cs );
    LIST_FOR_EACH_ENTRY( entry, &handlers->entries, struct event_handler_entry, entry )
    {
        if (entry->token.value != token.value) continue;
        list_remove( &entry->entry );
        found = TRUE;
        break;
    }
    LeaveCriticalSection( &handlers_cs );

    if (found)
    {
        IUnknown_Release( entry->handler );
        free( entry );
    }

    /* WinRT event removal is idempotent for stale tokens. */
    return S_OK;
}

void event_handlers_clear( struct event_handlers *handlers )
{
    struct event_handler_entry *entry, *next;
    struct list removed = LIST_INIT(removed);

    EnterCriticalSection( &handlers_cs );
    list_move_tail( &removed, &handlers->entries );
    LeaveCriticalSection( &handlers_cs );

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &removed, struct event_handler_entry, entry )
    {
        list_remove( &entry->entry );
        IUnknown_Release( entry->handler );
        free( entry );
    }
}
